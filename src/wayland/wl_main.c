/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * wl_comp: the MergedOS Wayland compositor process (docs/WAYLAND.md).
 *
 * A static Linux x86-64 ELF on raw system calls (no libc): listens on an
 * AF_UNIX socket, runs the portable compositor in wl_comp.c for every client,
 * and turns toplevels into windows of the MergedOS window manager through
 * /dev/mwm (kernel/include/mergedos/mwm.h). It also runs on a plain Linux host
 * (give it a regular file or pipe as the mwm path) for testing.
 *
 *   wl_comp [-m mwm_path] [-q] [socket_path]
 *
 * socket_path defaults to $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY (wayland-0), or
 * /tmp/wayland-0. -q: no log lines. Exits on SIGTERM/SIGKILL only.
 */
#include <stddef.h>
#include <stdint.h>
#include "kernel/include/mergedos/mwm.h"
#include "src/wayland/wl.h"

/* ---- system calls ---- */

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	register long r9 __asm__("r9") = f;
	long ret;

	__asm__ volatile("syscall"
			 : "=a"(ret)
			 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return ret;
}
#define sys(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

enum { SYS_read = 0, SYS_write = 1, SYS_open = 2, SYS_close = 3, SYS_poll = 7, SYS_mmap = 9,
       SYS_munmap = 11, SYS_nanosleep = 35, SYS_socket = 41, SYS_bind = 49, SYS_listen = 50,
       SYS_sendmsg = 46, SYS_recvmsg = 47, SYS_exit = 60, SYS_ftruncate = 77, SYS_unlink = 87,
       SYS_clock_gettime = 228, SYS_accept4 = 288, SYS_memfd_create = 319 };

#define O_RDWR 2
#define O_NONBLOCK 0x800
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000
#define MSG_CMSG_CLOEXEC 0x40000000
#define SOCK_STREAM 1
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC 0x80000
#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define EAGAIN 11
#define SOL_SOCKET 1
#define SCM_RIGHTS 1

struct iovec { void *base; size_t len; };
struct msghdr {
	void *name;
	uint32_t namelen;
	struct iovec *iov;
	size_t iovlen;
	void *control;
	size_t controllen;
	int flags;
};
struct cmsghdr { size_t len; int level; int type; };
struct pollfd { int fd; short events, revents; };
struct sockaddr_un { uint16_t family; char path[108]; };
struct timespec { long sec, nsec; };

#define CMSG_ALIGN(n) (((n) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_LEN(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_SPACE(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(n))
#define CMSG_DATA(c) ((int *)((char *)(c) + CMSG_ALIGN(sizeof(struct cmsghdr))))

/* ---- the few libc functions the compiler and wl_*.c need ---- */

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

void *memcpy(void *d, const void *s, size_t n)
{
	void *r = d;

	__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return r;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dd = d;
	const unsigned char *ss = s;

	if (dd < ss || dd >= ss + n) {
		memcpy(d, s, n);
	} else {
		while (n--)
			dd[n] = ss[n];
	}
	return d;
}

void *memset(void *d, int c, size_t n)
{
	void *r = d;

	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return r;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;

	for (; n; n--, p++)
		if (*p == (unsigned char)c)
			return (void *)p;
	return NULL;
}

size_t strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (unsigned char)*a - (unsigned char)*b;
}

/* ---- state ---- */

static struct wl_server srv;
static int mwm_fd = -1, listen_fd = -1;
static int cfd[WL_MAX_CLIENTS];
static bool quiet, mwm_live = true;   /* mwm_live: the device delivers events (not EOF) */
static unsigned char win_used[256];     /* mwm handles in use */
static uint8_t wbuf[sizeof(struct mwm_cmd) + (256 << 10)];

static bool prefix(const char *s, const char *p)
{
	while (*p && *s == *p)
		s++, p++;
	return !*p;
}

static void logs(const char *s)
{
	if (!quiet)
		sys(SYS_write, 2, s, strlen(s));
}

/* ---- mwm host callbacks ---- */

static int mwm_send(uint32_t op, uint32_t win, int a, int b, int c, int d, const void *pay,
		    uint32_t len)
{
	struct mwm_cmd *m = (struct mwm_cmd *)wbuf;

	*m = (struct mwm_cmd){ op, win, a, b, c, d, len };
	if (len)
		memcpy(wbuf + sizeof(*m), pay, len);
	return sys(SYS_write, mwm_fd, wbuf, sizeof(*m) + len) < 0 ? -1 : 0;
}

static int h_win_create(void *ctx, int w, int h)
{
	(void)ctx;
	for (int i = 1; i < 256; i++)
		if (!win_used[i]) {
			if (mwm_send(MWM_CREATE, (uint32_t)i, w, h, 0, 0, NULL, 0))
				return 0;
			win_used[i] = 1;
			return i;
		}
	return 0;
}

static void h_win_resize(void *ctx, int win, int w, int h)
{
	(void)ctx;
	(void)mwm_send(MWM_RESIZE, (uint32_t)win, w, h, 0, 0, NULL, 0);
}

static void h_win_title(void *ctx, int win, const char *t)
{
	(void)ctx;
	(void)mwm_send(MWM_TITLE, (uint32_t)win, 0, 0, 0, 0, t, (uint32_t)strlen(t));
}

static void h_win_destroy(void *ctx, int win)
{
	(void)ctx;
	(void)mwm_send(MWM_DESTROY, (uint32_t)win, 0, 0, 0, 0, NULL, 0);
	/* the handle stays reserved: the desktop thread frees the window asynchronously */
}

static void h_win_pixels(void *ctx, int win, int x, int y, int w, int h, const uint32_t *src,
			 size_t stride)
{
	const int per = (int)((sizeof(wbuf) - sizeof(struct mwm_cmd)) / ((size_t)w * 4));

	(void)ctx;
	for (int row = 0; row < h; row += per) {
		const int n = h - row < per ? h - row : per;
		struct mwm_cmd *m = (struct mwm_cmd *)wbuf;
		uint32_t *dst = (uint32_t *)(void *)(wbuf + sizeof(*m));

		*m = (struct mwm_cmd){ MWM_PIXELS, (uint32_t)win, x, y + row, w, n, (uint32_t)(w * 4 * n) };
		for (int r = 0; r < n; r++)
			memcpy(dst + (size_t)r * (size_t)w, src + (size_t)(row + r) * stride, (size_t)w * 4);
		if (sys(SYS_write, mwm_fd, wbuf, sizeof(*m) + (size_t)m->len) < 0)
			return;
	}
}

static uint8_t *h_map(void *ctx, int fd, size_t size)
{
	const long r = sys6(SYS_mmap, 0, (long)size, 1 /* PROT_READ */, 1 /* MAP_SHARED */, fd, 0);

	(void)ctx;
	return r < 0 && r > -4096 ? NULL : (uint8_t *)r;
}

static void h_unmap(void *ctx, uint8_t *p, size_t size)
{
	(void)ctx;
	sys(SYS_munmap, p, size, 0);
}

static void h_close(void *ctx, int fd)
{
	(void)ctx;
	sys(SYS_close, fd, 0, 0);
}

static int h_keymap(void *ctx, uint32_t *size)
{
	const size_t n = strlen(wl_keymap_text) + 1;
	const long fd = sys(SYS_memfd_create, "xkb", 1 /* MFD_CLOEXEC */, 0);

	(void)ctx;
	if (fd < 0)
		return -1;
	if (sys(SYS_write, fd, wl_keymap_text, n) != (long)n) {
		sys(SYS_close, fd, 0, 0);
		return -1;
	}
	*size = (uint32_t)n;
	return (int)fd;
}

static uint32_t h_now(void *ctx)
{
	struct timespec ts;

	(void)ctx;
	sys(SYS_clock_gettime, 1 /* CLOCK_MONOTONIC */, &ts, 0);
	return (uint32_t)(ts.sec * 1000 + ts.nsec / 1000000);
}

/* ---- socket i/o ---- */

static bool flush_client(struct wl_client *c)
{
	while (c->conn.out_len) {
		struct wl_conn *k = &c->conn;
		struct iovec iov = { k->out, k->out_len < 8192 ? k->out_len : 8192 };
		struct msghdr mh = { .iov = &iov, .iovlen = 1 };
		union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * WL_MAX_FDS)]; } cm;
		long n;

		memset(&cm, 0, sizeof(cm));
		if (k->out_nfd) {
			cm.h.len = CMSG_LEN(sizeof(int) * (size_t)k->out_nfd);
			cm.h.level = SOL_SOCKET;
			cm.h.type = SCM_RIGHTS;
			memcpy(CMSG_DATA(&cm.h), k->out_fd, sizeof(int) * (size_t)k->out_nfd);
			mh.control = &cm;
			mh.controllen = CMSG_SPACE(sizeof(int) * (size_t)k->out_nfd);
		}
		n = sys(SYS_sendmsg, cfd[c - srv.cl], &mh, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (n == -EAGAIN)
			return true;                    /* try again when POLLOUT says so */
		if (n <= 0)
			return false;
		for (int i = 0; i < k->out_nfd; i++)
			sys(SYS_close, k->out_fd[i], 0, 0);
		wl_sent(k, (size_t)n);
	}
	return true;
}

static void drop(struct wl_client *c)
{
	const int i = (int)(c - srv.cl);

	logs(c->conn.err ? "wl_comp: client dropped after a protocol error\n" : "wl_comp: client gone\n");
	sys(SYS_close, cfd[i], 0, 0);
	cfd[i] = -1;
	wl_client_drop(c);
}

static bool read_client(struct wl_client *c)
{
	uint8_t buf[4096];
	union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * WL_MAX_FDS)]; } cm;
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr mh = { .iov = &iov, .iovlen = 1, .control = &cm, .controllen = sizeof(cm) };
	long n;
	int fds[WL_MAX_FDS], nfd = 0;

	memset(&cm, 0, sizeof(cm));
	n = sys(SYS_recvmsg, cfd[c - srv.cl], &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
	if (n == -EAGAIN)
		return true;
	if (n <= 0)
		return false;
	for (struct cmsghdr *h = cm.h.len > CMSG_ALIGN(sizeof(*h)) ? &cm.h : NULL; h;) {
		if (h->level == SOL_SOCKET && h->type == SCM_RIGHTS) {
			nfd = (int)((h->len - CMSG_LEN(0)) / sizeof(int));
			if (nfd > WL_MAX_FDS)
				nfd = WL_MAX_FDS;
			memcpy(fds, CMSG_DATA(h), sizeof(int) * (size_t)nfd);
		}
		break;                          /* one control message is all a Wayland client sends */
	}
	if (!wl_feed(&c->conn, buf, (size_t)n, fds, nfd)) {
		for (int i = 0; i < nfd; i++)
			sys(SYS_close, fds[i], 0, 0);
		return false;
	}
	return wl_client_dispatch(c);
}

/* ---- input from the window manager ---- */

static void handle_events(void)
{
	struct mwm_event ev[16];
	long n = sys(SYS_read, mwm_fd, ev, sizeof(ev));

	if (n == 0)
		mwm_live = false;               /* a plain file (host test): nothing to wait for */

	for (long i = 0; i < n / (long)sizeof(ev[0]); i++) {
		const struct mwm_event *e = &ev[i];
		static const unsigned btn[3] = { WL_BTN_LEFT, WL_BTN_RIGHT, WL_BTN_MIDDLE };

		switch (e->type) {
		case MWM_EV_MOTION:
			wl_in_pointer_motion(&srv, (int)e->win, e->x, e->y);
			break;
		case MWM_EV_BUTTON:
			wl_in_pointer_button(&srv, (int)e->win, e->x, e->y, btn[e->code % 3], e->state != 0);
			break;
		case MWM_EV_LEAVE:
			wl_in_pointer_leave(&srv);
			break;
		case MWM_EV_FOCUS:
			wl_in_focus(&srv, (int)e->win, e->state != 0);
			break;
		case MWM_EV_CLOSE:
			wl_in_close(&srv, (int)e->win);
			break;
		case MWM_EV_KEY: {
			unsigned mods, k = wl_ascii_to_key((int)e->code, &mods);

			if (k) {
				wl_in_mods(&srv, mods);
				wl_in_key(&srv, k, true);
				wl_in_key(&srv, k, false);
				wl_in_mods(&srv, 0);
			}
			break;
		}
		}
	}
}

/* ---- main ---- */

static int run(int argc, char **argv, char **envp)
{
	const char *mwm_path = MWM_PATH, *sock = NULL, *rt = NULL, *disp = "wayland-0";
	static char pathbuf[128];
	struct sockaddr_un sa = { .family = 1 };
	size_t pl;

	for (char **e = envp; e && *e; e++) {
		if (prefix(*e, "XDG_RUNTIME_DIR="))
			rt = *e + 16;
		else if (prefix(*e, "WAYLAND_DISPLAY="))
			disp = *e + 16;
	}
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-m") && i + 1 < argc)
			mwm_path = argv[++i];
		else if (!strcmp(argv[i], "-q"))
			quiet = true;
		else
			sock = argv[i];
	}
	if (!sock) {
		const char *dir = rt ? rt : "/tmp";
		size_t a = strlen(dir), b = strlen(disp);

		if (a + b + 2 > sizeof(pathbuf))
			return 2;
		memcpy(pathbuf, dir, a);
		pathbuf[a] = '/';
		memcpy(pathbuf + a + 1, disp, b + 1);
		sock = pathbuf;
	}
	pl = strlen(sock);
	if (pl >= sizeof(sa.path)) {
		logs("wl_comp: socket path too long\n");
		return 2;
	}
	mwm_fd = (int)sys(SYS_open, mwm_path, O_RDWR | O_NONBLOCK, 0);
	if (mwm_fd < 0) {
		logs("wl_comp: cannot open the window device\n");
		return 1;
	}
	memcpy(sa.path, sock, pl + 1);
	sys(SYS_unlink, sock, 0, 0);
	listen_fd = (int)sys(SYS_socket, 1 /* AF_UNIX */, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd < 0 || sys(SYS_bind, listen_fd, &sa, 2 + pl + 1) < 0 ||
	    sys(SYS_listen, listen_fd, 8, 0) < 0) {
		logs("wl_comp: cannot listen on the socket\n");
		return 1;
	}
	{
		const struct wl_host host = {
			.map = h_map, .unmap = h_unmap, .close_fd = h_close, .win_create = h_win_create,
			.win_resize = h_win_resize, .win_title = h_win_title, .win_pixels = h_win_pixels,
			.win_destroy = h_win_destroy, .keymap_fd = h_keymap, .now_ms = h_now,
			.out_w = 1024, .out_h = 768,
		};

		wl_server_init(&srv, &host);
	}
	for (int i = 0; i < WL_MAX_CLIENTS; i++)
		cfd[i] = -1;
	logs("wl_comp: ready\n");
	for (;;) {
		struct pollfd pf[2 + WL_MAX_CLIENTS];
		int idx[2 + WL_MAX_CLIENTS], np = 1;

		pf[0] = (struct pollfd){ listen_fd, POLLIN, 0 };
		for (int i = 0; i < WL_MAX_CLIENTS; i++)
			if (srv.cl[i].used) {
				pf[np] = (struct pollfd){ cfd[i], (short)(POLLIN | (srv.cl[i].conn.out_len ? POLLOUT : 0)), 0 };
				idx[np++] = i;
			}
		if (mwm_live)
			pf[np] = (struct pollfd){ mwm_fd, POLLIN, 0 };
		sys(SYS_poll, pf, np + (mwm_live ? 1 : 0), mwm_live ? -1 : 10);
		if (pf[0].revents & POLLIN) {
			const long fd = sys6(SYS_accept4, listen_fd, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC, 0, 0);
			struct wl_client *c = fd >= 0 ? wl_client_add(&srv) : NULL;

			if (c)
				cfd[c - srv.cl] = (int)fd;
			else if (fd >= 0)
				sys(SYS_close, fd, 0, 0);
		}
		for (int k = 1; k < np; k++) {
			struct wl_client *c = &srv.cl[idx[k]];
			bool ok = true;

			if (pf[k].revents & POLLIN)
				ok = read_client(c);
			else if (pf[k].revents & (POLLHUP | POLLERR))
				ok = false;
			if (c->used && !flush_client(c))
				ok = false;
			if (!ok || (c->used && c->conn.err)) {
				(void)flush_client(c);          /* the protocol error message, if any */
				drop(c);
			}
		}
		handle_events();
		for (int i = 0; i < WL_MAX_CLIENTS; i++)
			if (srv.cl[i].used && !flush_client(&srv.cl[i]))
				drop(&srv.cl[i]);
	}
}

__attribute__((used)) static void c_main(long *sp)
{
	const int argc = (int)sp[0];
	char **argv = (char **)(sp + 1);
	char **envp = argv + argc + 1;

	sys(SYS_exit, run(argc, argv, envp), 0, 0);
	for (;;)
		;
}

__asm__(".text\n.globl _start\n_start:\n\tmov %rsp, %rdi\n\tand $-16, %rsp\n\tcall c_main\n");
