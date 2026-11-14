/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * wlclient.linux - a minimal Wayland client and the in-guest test of the
 * compositor (docs/WAYLAND.md). Static, raw Linux system calls, wire code from
 * src/wayland/wl_wire.c. It starts /bin/wl_comp.linux itself, connects over
 * AF_UNIX, creates an shm pool in a memfd, shows an xdg_toplevel through
 * /dev/mwm, then waits for a pointer button and a key from the desktop.
 */
#include <stddef.h>
#include <stdint.h>
#include "src/wayland/wl.h"

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
	register long r10 __asm__("r10") = d;
	register long r8 __asm__("r8") = e;
	register long r9 __asm__("r9") = f;
	long ret;

	__asm__ volatile("syscall" : "=a"(ret)
			 : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
			 : "rcx", "r11", "memory");
	return ret;
}
#define sys(n, a, b, c) sys6(n, (long)(a), (long)(b), (long)(c), 0, 0, 0)

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
size_t strlen(const char *s);
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

	if (dd < ss || dd >= ss + n)
		return memcpy(d, s, n);
	while (n--)
		dd[n] = ss[n];
	return d;
}
void *memset(void *d, int c, size_t n)
{
	void *r = d;

	__asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return r;
}
size_t strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

static void say(const char *s)
{
	sys(1, 1, s, strlen(s));
}

static void num(long v)
{
	char b[24];
	int i = 23;

	b[i] = 0;
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	say(b + i);
}

struct iovec { void *base; size_t len; };
struct msghdr { void *name; uint32_t namelen; struct iovec *iov; size_t iovlen; void *control;
		size_t controllen; int flags; };
struct cmsghdr { size_t len; int level; int type; };
struct sockaddr_un { uint16_t family; char path[108]; };
struct timespec { long sec, nsec; };
#define CALIGN(n) (((n) + 7) & ~(size_t)7)

static int fd;
static struct wl_conn c;
static uint32_t next_id = 3;

static void pause_ms(int ms)
{
	struct timespec ts = { 0, ms * 1000000L };

	sys(35, &ts, 0, 0);
}

static void flush(void)
{
	struct iovec iov = { c.out, c.out_len };
	union { struct cmsghdr h; char b[64]; } cm;
	struct msghdr mh = { .iov = &iov, .iovlen = 1 };

	memset(&cm, 0, sizeof(cm));
	if (c.out_nfd) {
		cm.h.len = CALIGN(sizeof(cm.h)) + sizeof(int) * (size_t)c.out_nfd;
		cm.h.level = 1;
		cm.h.type = 1;
		memcpy((char *)&cm.h + CALIGN(sizeof(cm.h)), c.out_fd, sizeof(int) * (size_t)c.out_nfd);
		mh.control = &cm;
		mh.controllen = CALIGN(cm.h.len);
	}
	if (sys(46, fd, &mh, 0) != (long)c.out_len)
		say("wlclient: send failed\n");
	c.out_len = 0;
	c.out_nfd = 0;
}

static void req(uint32_t obj, uint16_t op, const char *fmt, ...)
{
	struct wl_out o = wl_begin(&c, obj, op);
	__builtin_va_list ap;

	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt == 'u')
			wl_put_u32(&o, __builtin_va_arg(ap, uint32_t));
		else if (*fmt == 'i')
			wl_put_u32(&o, (uint32_t)__builtin_va_arg(ap, int));
		else if (*fmt == 's')
			wl_put_str(&o, __builtin_va_arg(ap, const char *));
		else if (*fmt == 'f')
			wl_put_fd(&o, __builtin_va_arg(ap, int));
	}
	__builtin_va_end(ap);
	wl_end(&o);
	flush();
}

/* Registry state and event flags filled in by pump(). */
static uint32_t g_name[16];
static char g_iface[16][40];
static int g_n;
static uint32_t pointer_id, keyboard_id, sync_id, toplevel_id, xsurf_id;
static int sync_done, configured, buttons, keys, closed;
static uint32_t cfg_serial;

static void pump(void)
{
	uint8_t buf[4096];
	union { struct cmsghdr h; char b[128]; } cm;
	struct iovec iov = { buf, sizeof(buf) };
	struct msghdr mh = { .iov = &iov, .iovlen = 1, .control = &cm, .controllen = sizeof(cm) };
	struct wl_msg m;
	int fds[8], nfd = 0;
	long n;

	memset(&cm, 0, sizeof(cm));
	n = sys6(47, fd, (long)&mh, 0x40 /* MSG_DONTWAIT */, 0, 0, 0);

	if (n <= 0)
		return;
	if (cm.h.len > CALIGN(sizeof(cm.h)) && cm.h.level == 1 && cm.h.type == 1) {
		nfd = (int)((cm.h.len - CALIGN(sizeof(cm.h))) / sizeof(int));
		if (nfd > 8)
			nfd = 8;
		memcpy(fds, (char *)&cm.h + CALIGN(sizeof(cm.h)), sizeof(int) * (size_t)nfd);
	}
	(void)wl_feed(&c, buf, (size_t)n, fds, nfd);
	while (wl_next(&c, &m) == 1) {
		if (m.obj == 2 && m.op == 0 && g_n < 16) {
			const uint32_t name = wl_u32(&m);
			const char *s = wl_str(&m);

			g_name[g_n] = name;
			for (int i = 0; s && i < 39 && s[i]; i++)
				g_iface[g_n][i] = s[i];
			g_n++;
		} else if (m.obj == sync_id && m.op == 0) {
			sync_done = 1;
		} else if (m.obj == xsurf_id && m.op == 0) {
			cfg_serial = wl_u32(&m);
			configured = 1;
		} else if (m.obj == toplevel_id && m.op == 1) {
			closed = 1;
		} else if (m.obj == pointer_id && m.op == 3) {
			buttons++;
			say("wlclient: pointer button\n");
		} else if (m.obj == keyboard_id && m.op == 3) {
			(void)wl_u32(&m);
			(void)wl_u32(&m);
			{
				const uint32_t key = wl_u32(&m);

				if (wl_u32(&m) == 1) {
					keys++;
					say("wlclient: key ");
					num(key);
					say("\n");
				}
			}
		} else if (m.obj == 1 && m.op == 0) {
			say("wlclient: protocol error from the compositor\n");
			closed = 1;
		}
		wl_pop(&c, &m);
	}
	while (c.in_nfd)                        /* keymap fd etc.: not needed */
		sys(3, c.in_fd[--c.in_nfd], 0, 0);
}

static int roundtrip(void)
{
	sync_id = next_id++;
	sync_done = 0;
	req(1, 0, "u", sync_id);
	for (int i = 0; i < 500 && !sync_done; i++) {
		pump();
		if (!sync_done)
			pause_ms(10);
	}
	return sync_done;
}

static uint32_t bind_global(const char *iface, uint32_t ver)
{
	for (int i = 0; i < g_n; i++) {
		const char *a = g_iface[i];
		int j = 0;

		while (iface[j] && a[j] == iface[j])
			j++;
		if (!iface[j] && !a[j]) {
			const uint32_t id = next_id++;

			req(2, 0, "usui", g_name[i], iface, ver, id);
			return id;
		}
	}
	return 0;
}

#define W 160
#define H 100

static int run(int ac, char **av)
{
	struct sockaddr_un sa = { .family = 1 };
	const char *path = "/tmp/wayland-0";
	char *const argv[] = { "wl_comp", "-m", ac > 2 ? av[2] : "/dev/mwm", NULL }, *const envp[] = { NULL };
	long pid = sys(57, 0, 0, 0);
	uint32_t comp, shm, wm, seat, surf, pool, buf;
	long mfd;
	uint32_t *px;
	int connected = 0;

	if (pid == 0) {
		sys(59, ac > 1 ? av[1] : "/bin/wl_comp.linux", argv, envp);
		sys(60, 127, 0, 0);
	}
	memcpy(sa.path, path, strlen(path) + 1);
	for (int i = 0; i < 300 && !connected; i++) {
		fd = (int)sys(41, 1, 1, 0);
		if (sys(42, fd, &sa, 2 + strlen(path) + 1) == 0) {
			connected = 1;
		} else {
			sys(3, fd, 0, 0);
			pause_ms(20);
		}
	}
	if (!connected) {
		say("wlclient: FAIL connect\n");
		goto out;
	}
	say("wlclient: connected\n");
	req(1, 1, "u", 2u);
	if (!roundtrip() || g_n < 11) {
		say("wlclient: FAIL registry\n");
		goto out;
	}
	comp = bind_global("wl_compositor", 5);
	shm = bind_global("wl_shm", 1);
	wm = bind_global("xdg_wm_base", 5);
	seat = bind_global("wl_seat", 7);
	(void)bind_global("zwp_pointer_constraints_v1", 1);
	(void)bind_global("wl_subcompositor", 1);
	if (!comp || !shm || !wm || !seat) {
		say("wlclient: FAIL globals\n");
		goto out;
	}
	pointer_id = next_id++;
	req(seat, 0, "u", pointer_id);
	keyboard_id = next_id++;
	req(seat, 1, "u", keyboard_id);
	surf = next_id++;
	req(comp, 0, "u", surf);
	xsurf_id = next_id++;
	req(wm, 2, "uu", xsurf_id, surf);
	toplevel_id = next_id++;
	req(xsurf_id, 1, "u", toplevel_id);
	req(toplevel_id, 2, "s", "wlclient");
	req(surf, 6, "");
	if (!roundtrip() || !configured) {
		say("wlclient: FAIL configure sync=");
		num(sync_done);
		say(" cfg=");
		num(configured);
		say(" closed=");
		num(closed);
		say("\n");
		goto out;
	}
	req(xsurf_id, 4, "u", cfg_serial);
	say("wlclient: configured\n");
	mfd = sys(319, "wlpool", 0, 0);
	sys(77, mfd, W * H * 4, 0);
	px = (uint32_t *)sys6(9, 0, W * H * 4, 3, 1, mfd, 0);
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			px[y * W + x] = 0xff000000u | (uint32_t)(x * 255 / W) << 16 | (uint32_t)(y * 255 / H) << 8 | 0x80;
	pool = next_id++;
	req(shm, 0, "uif", pool, W * H * 4, (int)mfd);
	buf = next_id++;
	req(pool, 0, "uiiiiu", buf, 0, W, H, W * 4, 1u);
	req(surf, 1, "uii", buf, 0, 0);
	req(surf, 6, "");
	if (!roundtrip()) {
		say("wlclient: FAIL commit\n");
		goto out;
	}
	say("wlclient: window shown\n");
	for (int i = 0; i < 6000 && !(buttons && keys) && !closed; i++) {
		pump();
		pause_ms(10);
	}
	req(toplevel_id, 0, "");
	say(buttons && keys ? "wlclient: OK\n" : "wlclient: FAIL input\n");
out:
	sys(62, pid, 9, 0);
	sys(61, pid, 0, 0);
	return 0;
}

__attribute__((used)) static void c_main(long *sp)
{
	sys(60, run((int)sp[0], (char **)(sp + 1)), 0, 0);
	for (;;)
		;
}

__asm__(".text\n.globl _start\n_start:\n\tmov %rsp, %rdi\n\tand $-16, %rsp\n\tcall c_main\n");
