/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Wayland compositor (src/wayland): wire codec round trips, truncation and
 * corruption fuzz, and a test client talking to the real state machine over a
 * socketpair with SCM_RIGHTS fd passing and memfd shm pools.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include "tests/test.h"
#include "src/wayland/wl.h"

/* ---- mock window manager ---- */

#define MW 8
static struct {
	int used, w, h, destroyed;
	char title[48];
	uint32_t px[200 * 200];
} mwin[MW];
static int mwin_created;

static int m_create(void *ctx, int w, int h)
{
	(void)ctx;
	for (int i = 1; i < MW; i++)
		if (!mwin[i].used) {
			memset(&mwin[i], 0, sizeof(mwin[i]));
			mwin[i].used = 1;
			mwin[i].w = w;
			mwin[i].h = h;
			mwin_created++;
			return i;
		}
	return 0;
}

static void m_resize(void *ctx, int win, int w, int h)
{
	(void)ctx;
	mwin[win].w = w;
	mwin[win].h = h;
}

static void m_title(void *ctx, int win, const char *t)
{
	(void)ctx;
	snprintf(mwin[win].title, sizeof(mwin[win].title), "%s", t);
}

static void m_pixels(void *ctx, int win, int x, int y, int w, int h, const uint32_t *src,
		     size_t stride)
{
	(void)ctx;
	if (x < 0 || y < 0 || x + w > mwin[win].w || y + h > mwin[win].h || x + w > 200 || y + h > 200)
		abort();                        /* the compositor must clip */
	for (int r = 0; r < h; r++)
		memcpy(&mwin[win].px[(y + r) * 200 + x], src + (size_t)r * stride, (size_t)w * 4);
}

static void m_destroy(void *ctx, int win)
{
	(void)ctx;
	mwin[win].used = 0;
	mwin[win].destroyed = 1;
}

static uint8_t *m_map(void *ctx, int fd, size_t size)
{
	void *p = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);

	(void)ctx;
	return p == MAP_FAILED ? NULL : p;
}

static void m_unmap(void *ctx, uint8_t *p, size_t size)
{
	(void)ctx;
	munmap(p, size);
}

static void m_close(void *ctx, int fd)
{
	(void)ctx;
	close(fd);
}

static int m_keymap(void *ctx, uint32_t *size)
{
	const int fd = memfd_create("keymap", 0);
	const size_t n = strlen(wl_keymap_text) + 1;

	(void)ctx;
	if (write(fd, wl_keymap_text, n) != (ssize_t)n)
		abort();
	*size = (uint32_t)n;
	return fd;
}

static uint32_t m_now(void *ctx)
{
	(void)ctx;
	return 1234;
}

static const struct wl_host mock = {
	.map = m_map, .unmap = m_unmap, .close_fd = m_close, .win_create = m_create,
	.win_resize = m_resize, .win_title = m_title, .win_pixels = m_pixels,
	.win_destroy = m_destroy, .keymap_fd = m_keymap, .now_ms = m_now,
	.out_w = 1024, .out_h = 768,
};

/* ---- test client ---- */

struct rec {
	uint32_t obj, op, size;
	uint32_t w[16];
	int fd;
};

static struct {
	int fd, sfd;
	struct wl_server srv;
	struct wl_client *cl;
	struct wl_conn c;               /* the client's own conn: writer for requests, reader for events */
	struct rec ev[512];
	int nev;
	uint8_t sent[1 << 16];          /* everything the client sent, for the fuzzers */
	size_t nsent;
} T;

static void srv_pump(void)
{
	uint8_t buf[8192];
	char cbuf[CMSG_SPACE(sizeof(int) * WL_MAX_FDS)];
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf,
			     .msg_controllen = sizeof(cbuf) };
	ssize_t n;

	while ((n = recvmsg(T.sfd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC)) > 0) {
		int fds[WL_MAX_FDS], nfd = 0;

		for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm))
			if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
				nfd = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
				memcpy(fds, CMSG_DATA(cm), (size_t)nfd * sizeof(int));
			}
		CHECK(wl_feed(&T.cl->conn, buf, (size_t)n, fds, nfd));
		(void)wl_client_dispatch(T.cl);
		mh.msg_controllen = sizeof(cbuf);
	}
	/* flush the server's output to the client, fds with the first chunk */
	while (T.cl->conn.out_len) {
		struct wl_conn *c = &T.cl->conn;
		size_t chunk = c->out_len < 4096 ? c->out_len : 4096;
		struct iovec oi = { c->out, chunk };
		char ob[CMSG_SPACE(sizeof(int) * WL_MAX_FDS)] = { 0 };
		struct msghdr om = { .msg_iov = &oi, .msg_iovlen = 1 };

		if (c->out_nfd) {
			struct cmsghdr *cm;

			om.msg_control = ob;
			om.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)c->out_nfd);
			cm = CMSG_FIRSTHDR(&om);
			cm->cmsg_level = SOL_SOCKET;
			cm->cmsg_type = SCM_RIGHTS;
			cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)c->out_nfd);
			memcpy(CMSG_DATA(cm), c->out_fd, sizeof(int) * (size_t)c->out_nfd);
		}
		if (sendmsg(T.sfd, &om, 0) != (ssize_t)chunk)
			abort();
		for (int i = 0; i < c->out_nfd; i++)
			close(c->out_fd[i]);
		wl_sent(c, chunk);
	}
}

static void c_flush(void)
{
	struct wl_conn *c = &T.c;
	struct iovec iov = { c->out, c->out_len };
	char cb[CMSG_SPACE(sizeof(int) * WL_MAX_FDS)] = { 0 };
	struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };

	if (c->out_nfd) {
		struct cmsghdr *cm;

		mh.msg_control = cb;
		mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)c->out_nfd);
		cm = CMSG_FIRSTHDR(&mh);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)c->out_nfd);
		memcpy(CMSG_DATA(cm), c->out_fd, sizeof(int) * (size_t)c->out_nfd);
	}
	if (T.nsent + c->out_len <= sizeof(T.sent)) {
		memcpy(T.sent + T.nsent, c->out, c->out_len);
		T.nsent += c->out_len;
	}
	if (sendmsg(T.fd, &mh, 0) != (ssize_t)c->out_len)
		abort();
	wl_sent(c, c->out_len);
	srv_pump();
	/* collect events */
	for (;;) {
		uint8_t buf[8192];
		char cbuf[CMSG_SPACE(sizeof(int) * WL_MAX_FDS)];
		struct iovec ri = { buf, sizeof(buf) };
		struct msghdr rm = { .msg_iov = &ri, .msg_iovlen = 1, .msg_control = cbuf,
				     .msg_controllen = sizeof(cbuf) };
		ssize_t n = recvmsg(T.fd, &rm, MSG_DONTWAIT);
		int fds[WL_MAX_FDS], nfd = 0;
		struct wl_msg m;

		if (n <= 0)
			break;
		for (struct cmsghdr *cm = CMSG_FIRSTHDR(&rm); cm; cm = CMSG_NXTHDR(&rm, cm))
			if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
				nfd = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
				memcpy(fds, CMSG_DATA(cm), (size_t)nfd * sizeof(int));
			}
		CHECK(wl_feed(c, buf, (size_t)n, fds, nfd));
		while (wl_next(c, &m) == 1) {
			struct rec *r = &T.ev[T.nev++ % 512];

			memset(r, 0, sizeof(*r));
			r->obj = m.obj;
			r->op = m.op;
			r->size = m.size;
			r->fd = -1;
			memcpy(r->w, m.p, (size_t)(m.end - m.p) < sizeof(r->w) ? (size_t)(m.end - m.p) : sizeof(r->w));
			if (c->in_nfd && (m.op == 0) && r->obj != 1)
				r->fd = wl_fd(&m);
			wl_pop(c, &m);
		}
	}
}

static void rq(uint32_t obj, uint16_t op, const char *fmt, ...)
{
	struct wl_out o = wl_begin(&T.c, obj, op);
	va_list ap;

	va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt == 'u')
			wl_put_u32(&o, va_arg(ap, uint32_t));
		else if (*fmt == 'i')
			wl_put_u32(&o, (uint32_t)va_arg(ap, int));
		else if (*fmt == 's')
			wl_put_str(&o, va_arg(ap, const char *));
		else if (*fmt == 'f')
			wl_put_fd(&o, va_arg(ap, int));
	}
	va_end(ap);
	wl_end(&o);
	c_flush();
}

static const struct rec *find(uint32_t obj, uint32_t op)
{
	for (int i = T.nev - 1; i >= 0 && i >= T.nev - 512; i--)
		if (T.ev[i % 512].obj == obj && T.ev[i % 512].op == op)
			return &T.ev[i % 512];
	return NULL;
}

static int count(uint32_t obj, uint32_t op)
{
	int n = 0;

	for (int i = T.nev > 512 ? T.nev - 512 : 0; i < T.nev; i++)
		n += T.ev[i % 512].obj == obj && T.ev[i % 512].op == op;
	return n;
}

static void session_start(void)
{
	int sv[2];

	memset(&T, 0, sizeof(T));
	memset(mwin, 0, sizeof(mwin));
	mwin_created = 0;
	CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
	T.fd = sv[0];
	T.sfd = sv[1];
	wl_server_init(&T.srv, &mock);
	T.cl = wl_client_add(&T.srv);
}

static void session_end(void)
{
	wl_client_drop(T.cl);
	close(T.fd);
	close(T.sfd);
}

/* ---- tests ---- */

static void test_codec_roundtrip(void)
{
	static struct wl_conn a, b;
	struct wl_out o = wl_begin(&a, 7, 3);
	struct wl_msg m;
	const uint8_t arr[5] = { 1, 2, 3, 4, 5 };
	uint32_t n;
	int fd = 42;

	wl_put_u32(&o, 0xdeadbeef);
	wl_put_str(&o, "hello");
	wl_put_arr(&o, arr, 5);
	wl_put_fd(&o, fd);
	wl_end(&o);
	CHECK_EQ(a.out_len, 8 + 4 + (4 + 8) + (4 + 8));
	CHECK(wl_feed(&b, a.out, a.out_len, a.out_fd, a.out_nfd));
	CHECK_EQ(wl_next(&b, &m), 1);
	CHECK_EQ(m.obj, 7);
	CHECK_EQ(m.op, 3);
	CHECK_EQ(m.size, a.out_len);
	CHECK_EQ(wl_u32(&m), 0xdeadbeef);
	CHECK_STR(wl_str(&m), "hello");
	CHECK(memcmp(wl_arr(&m, &n), arr, 5) == 0 && n == 5);
	CHECK_EQ(wl_fd(&m), 42);
	CHECK(!m.bad && m.p == m.end);
	(void)wl_u32(&m);                       /* past the end */
	CHECK(m.bad);
	wl_pop(&b, &m);
	CHECK_EQ(b.in_len, 0);

	/* framing errors */
	memset(&b, 0, sizeof(b));
	memcpy(b.in, "\1\0\0\0\0\0\4\0", 8);    /* size 4 < header */
	b.in_len = 8;
	CHECK_EQ(wl_next(&b, &m), -1);
	memcpy(b.in, "\1\0\0\0\1\0\x0a\0", 8);  /* size 10: not a multiple of 4 */
	CHECK_EQ(wl_next(&b, &m), -1);
	memcpy(b.in, "\1\0\0\0\1\0\x10\0", 8);  /* wants 16 bytes, has 8 */
	CHECK_EQ(wl_next(&b, &m), 0);
	/* a string without its NUL, an array longer than the message */
	memset(&b, 0, sizeof(b));
	{
		struct wl_out w = wl_begin(&b, 1, 0);

		wl_put_arr(&w, "abcd", 4);
		wl_end(&w);
		CHECK(wl_feed(&b, b.out, b.out_len, NULL, 0));
	}
	CHECK_EQ(wl_next(&b, &m), 1);
	CHECK(wl_str(&m) == NULL && m.bad);
	/* writer overflow rolls back */
	memset(&a, 0, sizeof(a));
	o = wl_begin(&a, 1, 1);
	{
		static uint8_t big[WL_OUT_BUF];

		wl_put_arr(&o, big, sizeof(big));
		wl_end(&o);
	}
	CHECK_EQ(a.out_len, 0);
	CHECK(a.err);
}

static const char *const need[] = { "wl_compositor", "xdg_wm_base", "wl_shm", "wl_subcompositor",
				    "zwp_pointer_constraints_v1", "zwp_relative_pointer_manager_v1",
				    "wl_seat", "wl_output", "wp_viewporter", "zxdg_output_manager_v1" };

static void bindg(uint32_t name, const char *iface, uint32_t ver, uint32_t id)
{
	rq(2, 0, "usui", name, iface, ver, id);
}

static void test_client_session(void)
{
	const uint32_t W = 64, H = 32;
	int mfd;
	uint32_t *map;
	const struct rec *r;
	uint32_t serial;

	session_start();
	rq(1, 1, "u", 2u);                      /* get_registry */
	CHECK_EQ(count(2, 0), 11);
	for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
		int ok = 0;

		for (int e = 0; e < T.nev; e++)
			if (T.ev[e].obj == 2 && T.ev[e].op == 0 &&
			    !strcmp((const char *)&T.ev[e].w[2], need[i]))
				ok = 1;
		CHECK(ok);
	}
	bindg(1, "wl_compositor", 5, 3);
	bindg(2, "wl_subcompositor", 1, 4);
	bindg(3, "wl_shm", 1, 5);
	CHECK_EQ(count(5, 0), 2);
	bindg(4, "xdg_wm_base", 5, 6);
	bindg(5, "wl_seat", 7, 7);
	CHECK_EQ(find(7, 0)->w[0], 3);
	bindg(6, "wl_output", 4, 8);
	CHECK(find(8, 0) && find(8, 1) && find(8, 2));
	CHECK_EQ(find(8, 1)->w[1], 1024);
	bindg(7, "zxdg_output_manager_v1", 2, 9);
	rq(9, 1, "uu", 10u, 8u);                // get_xdg_output
	CHECK_EQ(find(10, 1)->w[1], 768);
	bindg(9, "wp_viewporter", 1, 11);
	bindg(10, "zwp_pointer_constraints_v1", 1, 12);
	bindg(11, "zwp_relative_pointer_manager_v1", 1, 13);
	bindg(8, "wl_data_device_manager", 3, 14);

	rq(1, 0, "u", 15u);                     /* sync */
	CHECK(find(15, 0) != NULL);
	CHECK(find(1, 1) != NULL);              /* delete_id */

	/* toplevel */
	rq(3, 0, "u", 20u);                     /* create_surface */
	rq(6, 2, "uu", 21u, 20u);               /* get_xdg_surface */
	rq(21, 1, "u", 22u);                    /* get_toplevel */
	rq(22, 2, "s", "hello");                /* set_title */
	rq(20, 6, "");                          /* commit: no buffer yet */
	r = find(22, 0);
	CHECK(r != NULL);
	r = find(21, 0);
	CHECK(r != NULL);
	serial = r ? r->w[0] : 0;
	rq(21, 4, "u", serial);                 /* ack_configure */
	CHECK_EQ(mwin_created, 0);

	/* shm buffer with a pattern */
	mfd = memfd_create("pool", 0);
	CHECK_EQ(ftruncate(mfd, (off_t)(W * H * 4 + 4096)), 0);
	map = mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
	for (uint32_t i = 0; i < W * H; i++)
		map[i] = 0xff000000u | i;
	rq(5, 0, "uif", 23u, (int)(W * H * 4 + 4096), mfd);     /* create_pool */
	rq(23, 0, "uiiiiu", 24u, 0, (int)W, (int)H, (int)(W * 4), 1u);
	rq(20, 3, "u", 25u);                    /* frame */
	rq(20, 1, "uii", 24u, 0, 0);            /* attach */
	rq(20, 2, "iiii", 0, 0, (int)W, (int)H);
	rq(20, 6, "");
	CHECK_EQ(mwin_created, 1);
	CHECK_EQ(mwin[1].w, (int)W);
	CHECK_STR(mwin[1].title, "hello");
	CHECK(mwin[1].px[0] == 0xff000000u && mwin[1].px[5 * 200 + 7] == (0xff000000u | (5 * W + 7)));
	CHECK_EQ(find(25, 0)->w[0], 1234);      /* frame callback */

	/* input */
	rq(7, 1, "u", 26u);                     /* get_keyboard */
	r = find(26, 0);
	CHECK(r && r->w[0] == 1 && r->fd >= 0 && r->w[1] == strlen(wl_keymap_text) + 1);
	if (r && r->fd >= 0) {
		char *km = mmap(NULL, r->w[1], PROT_READ, MAP_PRIVATE, r->fd, 0);

		CHECK(km != MAP_FAILED && !strcmp(km, wl_keymap_text));
		munmap(km, r->w[1]);
	}
	rq(7, 0, "u", 27u);                     /* get_pointer */
	rq(13, 1, "uu", 28u, 27u);              /* relative pointer */
	rq(12, 1, "uuuuu", 29u, 20u, 27u, 0u, 1u);      /* lock_pointer */
	CHECK(find(29, 0) != NULL);
	wl_in_focus(&T.srv, 1, true);
	srv_pump();
	c_flush();
	CHECK(find(26, 1) != NULL);             /* keyboard enter */
	wl_in_pointer_motion(&T.srv, 1, 10, 5);
	wl_in_pointer_motion(&T.srv, 1, 12, 6);
	rq(1, 0, "u", 30u);                     /* flush */
	CHECK(find(27, 0) != NULL && find(27, 2) != NULL);
	CHECK_EQ(find(27, 2)->w[1], 12 * 256);
	CHECK_EQ(find(28, 0)->w[2], 2 * 256);   /* relative dx */
	wl_in_pointer_button(&T.srv, 1, 12, 6, WL_BTN_LEFT, true);
	{
		unsigned mods;
		unsigned k = wl_ascii_to_key('A', &mods);

		CHECK_EQ(k, 30);
		CHECK_EQ(mods, 1);
		wl_in_key(&T.srv, k, true, mods);
		wl_in_key(&T.srv, k, false, mods);
		wl_in_key(&T.srv, 0, false, 0);
	}
	wl_in_close(&T.srv, 1);
	rq(1, 0, "u", 31u);
	CHECK(find(27, 3) != NULL);
	CHECK_EQ(count(26, 3), 3);
	CHECK(count(26, 4) >= 2);
	CHECK(find(22, 1) != NULL);             /* xdg_toplevel.close */

	/* subsurface at (8, 8) and a popup anchored at (4, 4) */
	for (uint32_t i = 0; i < 16 * 16; i++)
		map[i] = 0xff00ff00u;
	rq(3, 0, "u", 40u);
	rq(4, 1, "uuu", 41u, 40u, 20u);         /* get_subsurface */
	rq(41, 1, "ii", 8, 8);
	rq(23, 0, "uiiiiu", 42u, 0, 16, 16, 64, 0u);
	rq(40, 1, "uii", 42u, 0, 0);
	rq(40, 6, "");
	rq(20, 6, "");                          /* parent commit applies the position */
	CHECK_EQ(mwin[1].px[8 * 200 + 8], 0xff00ff00u);
	CHECK(mwin[1].px[7 * 200 + 7] != 0xff00ff00u);
	rq(6, 1, "u", 50u);                     /* positioner */
	rq(50, 1, "ii", 20, 10);
	rq(50, 2, "iiii", 4, 4, 1, 1);
	rq(3, 0, "u", 51u);
	rq(6, 2, "uu", 52u, 51u);
	rq(52, 2, "uuu", 53u, 21u, 50u);        /* get_popup */
	CHECK(find(53, 0) != NULL);
	rq(23, 0, "uiiiiu", 54u, 0, 20, 10, 80, 0u);
	rq(51, 1, "uii", 54u, 0, 0);
	rq(51, 6, "");
	CHECK_EQ(mwin[1].px[4 * 200 + 4], 0xff00ff00u);
	CHECK_EQ(mwin[1].px[13 * 200 + 23], 0xff00ff00u);

	/* replacing the buffer releases the old one */
	rq(23, 0, "uiiiiu", 60u, 0, (int)W, (int)H, (int)(W * 4), 0u);
	rq(20, 1, "uii", 60u, 0, 0);
	rq(20, 6, "");
	CHECK(find(24, 0) != NULL);             /* wl_buffer.release */

	/* viewport scaling: 16x16 source shown as 32x32 */
	rq(11, 1, "uu", 70u, 40u);
	rq(70, 2, "ii", 32, 32);
	rq(40, 6, "");
	CHECK_EQ(mwin[1].px[8 * 200 + 8 + 31], 0xff00ff00u);

	/* tear down: toplevel destroy closes the window */
	rq(22, 0, "");
	CHECK(mwin[1].destroyed);
	munmap(map, W * H * 4);
	close(mfd);
	session_end();
}

static void test_protocol_errors(void)
{
	session_start();
	rq(1, 1, "u", 2u);
	rq(99, 0, "");                          /* no such object */
	CHECK(T.cl->conn.err);
	CHECK(find(1, 0) != NULL);              /* wl_display.error */
	session_end();

	session_start();
	rq(1, 1, "u", 2u);
	rq(2, 0, "usui", 3u, "wl_compositor", 1u, 5);  /* wrong interface for the name */
	CHECK(T.cl->conn.err);
	session_end();

	session_start();
	rq(1, 1, "u", 2u);
	rq(2, 0, "usui", 1u, "wl_compositor", 5u, 3);
	rq(3, 0, "u", 3u);                      /* id already in use */
	CHECK(T.cl->conn.err);
	session_end();

	session_start();                        /* shm pool with a hostile buffer */
	{
		int mfd = memfd_create("p", 0);

		CHECK_EQ(ftruncate(mfd, 4096), 0);
		rq(1, 1, "u", 2u);
		rq(2, 0, "usui", 3u, "wl_shm", 1u, 5);
		rq(5, 0, "uif", 6u, 4096, mfd);
		rq(6, 0, "uiiiiu", 7u, 0, 1000, 1000, 4000, 1u);
		CHECK(T.cl->conn.err);
		close(mfd);
	}
	session_end();
}

/* ---- fuzzing ---- */

static uint32_t rng_state = 12345;

static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static void fuzz_run(const uint8_t *data, size_t n, size_t chunk)
{
	struct wl_server srv;
	struct wl_client *c;
	int mfd = memfd_create("fz", 0);
	int fds[4];

	if (ftruncate(mfd, 1 << 16))
		abort();
	for (int i = 0; i < 4; i++)
		fds[i] = dup(mfd);
	wl_server_init(&srv, &mock);
	c = wl_client_add(&srv);
	if (!wl_feed(&c->conn, NULL, 0, fds, 4))
		abort();
	for (size_t off = 0; off < n; off += chunk) {
		const size_t k = n - off < chunk ? n - off : chunk;

		if (!wl_feed(&c->conn, data + off, k, NULL, 0))
			break;
		if (!wl_client_dispatch(c))
			break;
		c->conn.out_len = 0;
		c->conn.out_nfd = 0;
	}
	/* output fds the server queued (keymaps) are the platform's to close */
	for (int i = 0; i < c->conn.out_nfd; i++)
		close(c->conn.out_fd[i]);
	wl_client_drop(c);
	close(mfd);
}

static void test_fuzz(void)
{
	static uint8_t good[sizeof(T.sent)], bad[sizeof(T.sent)];
	size_t n;

	/* Record a whole session (without its fds the shm paths fail, so give the fuzzer fds). */
	session_start();
	rq(1, 1, "u", 2u);
	bindg(1, "wl_compositor", 5, 3);
	bindg(3, "wl_shm", 1, 5);
	bindg(4, "xdg_wm_base", 5, 6);
	bindg(5, "wl_seat", 7, 7);
	rq(3, 0, "u", 20u);
	rq(6, 2, "uu", 21u, 20u);
	rq(21, 1, "u", 22u);
	rq(22, 2, "s", "fuzz");
	rq(20, 6, "");
	{
		int mfd = memfd_create("p", 0);

		CHECK_EQ(ftruncate(mfd, 8192), 0);
		rq(5, 0, "uif", 23u, 8192, mfd);
		close(mfd);
	}
	rq(23, 0, "uiiiiu", 24u, 0, 16, 16, 64, 1u);
	rq(20, 1, "uii", 24u, 0, 0);
	rq(20, 6, "");
	rq(7, 1, "u", 26u);
	n = T.nsent;
	memcpy(good, T.sent, n);
	session_end();
	CHECK(n > 200);

	fuzz_run(good, n, n);                   /* the pristine stream is accepted whole */
	for (size_t cut = 0; cut <= n; cut++) { /* every truncation point */
		fuzz_run(good, cut, cut ? cut : 1);
		fuzz_run(good, cut, 7);         /* and dribbled in odd chunks */
	}
	for (int it = 0; it < 4000; it++) {     /* random corruption */
		memcpy(bad, good, n);
		for (int k = 0, flips = 1 + (int)(rng() % 4); k < flips; k++)
			bad[rng() % n] ^= (uint8_t)(1u << (rng() % 8)) | (uint8_t)(rng() % 3 ? 0 : rng());
		fuzz_run(bad, n, it % 3 ? n : 13);
	}
	for (int it = 0; it < 1000; it++) {     /* pure noise and headers with huge sizes */
		for (size_t i = 0; i < 256; i++)
			bad[i] = (uint8_t)rng();
		fuzz_run(bad, 256, 256);
	}
	CHECK(1);
}

/* The keymap must parse in the real libxkbcommon, if this host has it. */
static void test_keymap(void)
{
	void *h = dlopen("libxkbcommon.so.0", RTLD_NOW);
	void *(*ctx_new)(int);
	void *(*km_new)(void *, const char *, int, int);
	unsigned (*km_max)(void *);

	CHECK(strlen(wl_keymap_text) > 1000);
	if (!h) {
		printf("  (libxkbcommon not installed: keymap parse check skipped)\n");
		return;
	}
	ctx_new = (void *(*)(int))dlsym(h, "xkb_context_new");
	km_new = (void *(*)(void *, const char *, int, int))dlsym(h, "xkb_keymap_new_from_string");
	km_max = (unsigned (*)(void *))dlsym(h, "xkb_keymap_max_keycode");
	if (ctx_new && km_new && km_max) {
		void *ctx = ctx_new(0x1 /* XKB_CONTEXT_NO_DEFAULT_INCLUDES */);
		void *km = km_new(ctx, wl_keymap_text, 1, 0);

		CHECK(km != NULL);
		if (km) {
			void (*unref)(void *) = (void (*)(void *))dlsym(h, "xkb_keymap_unref");

			CHECK_EQ(km_max(km), 119);
			unref(km);
		}
		{
			void (*cunref)(void *) = (void (*)(void *))dlsym(h, "xkb_context_unref");

			cunref(ctx);
		}
	}
	dlclose(h);
}

void test_wayland(void)
{
	test_codec_roundtrip();
	test_client_session();
	test_protocol_errors();
	test_keymap();
	test_fuzz();
}
