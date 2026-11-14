/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * mwmtest.linux - /dev/mwm end to end (kernel/include/mergedos/mwm.h): opens the
 * window device, creates a 96x64 window with a gradient and a title, waits
 * for a pointer click and a key from the desktop, then closes the window.
 * Freestanding on raw Linux system calls. Skips (exit 0) without a desktop.
 */
#include <stdint.h>
#include "kernel/include/mergedos/mwm.h"

static long sys6(long n, long a, long b, long c)
{
	long ret;

	__asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c)
			 : "rcx", "r11", "memory");
	return ret;
}

static void say(const char *s)
{
	long n = 0;

	while (s[n])
		n++;
	sys6(1, 1, (long)s, n);
}

static void num(int v)
{
	char b[12];
	int i = 11;

	b[i] = 0;
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v);
	say(b + i);
}

void *memcpy(void *d, const void *s, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n)
{
	__asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return d;
}

static int mfd;
static uint8_t buf[sizeof(struct mwm_cmd) + 96 * 64 * 4];

static long cmd(uint32_t op, uint32_t win, int a, int b, int c, int d, const void *pay, uint32_t len)
{
	*(struct mwm_cmd *)buf = (struct mwm_cmd){ op, win, a, b, c, d, len };
	if (len)
		memcpy(buf + sizeof(struct mwm_cmd), pay, len);
	return sys6(1, mfd, (long)buf, (long)(sizeof(struct mwm_cmd) + len));
}

static void run(void)
{
	static uint32_t px[96 * 64];
	struct mwm_event ev;
	int fd = (int)sys6(2, (long)MWM_PATH, 2 /* O_RDWR */, 0);
	int got_button = 0, got_key = 0;

	if (fd < 0) {
		say("mwmtest: no desktop, skipped\n");
		return;
	}
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 96; x++)
			px[y * 96 + x] = (uint32_t)(x * 2) << 16 | (uint32_t)(y * 4) << 8 | 0x40;
	mfd = fd;
	if (cmd(MWM_CREATE, 1, 96, 64, 0, 0, 0, 0) < 0 ||
	    cmd(MWM_TITLE, 1, 0, 0, 0, 0, "mwmtest", 7) < 0 ||
	    cmd(MWM_PIXELS, 1, 0, 0, 96, 64, px, sizeof(px)) < 0) {
		say("mwmtest: FAIL create\n");
		return;
	}
	say("mwmtest: window created\n");
	while (!got_button || !got_key) {
		if (sys6(0, fd, (long)&ev, sizeof(ev)) != (long)sizeof(ev))
			break;
		if (ev.type == MWM_EV_BUTTON && ev.state == 1 && !got_button) {
			got_button = 1;
			say("mwmtest: button ");
			num(ev.x);
			say(" ");
			num(ev.y);
			say("\n");
		}
		if (ev.type == MWM_EV_KEY && !got_key) {
			got_key = 1;
			say("mwmtest: key ");
			num((int)ev.code);
			say("\n");
		}
	}
	cmd(MWM_DESTROY, 1, 0, 0, 0, 0, 0, 0);
	sys6(3, fd, 0, 0);
	say(got_button && got_key ? "mwmtest: OK\n" : "mwmtest: FAIL events\n");
}

__attribute__((used)) static void c_main(void)
{
	run();
	for (;;)
		sys6(60, 0, 0, 0);
}

__asm__(".text\n.globl _start\n_start:\n\tand $-16, %rsp\n\tcall c_main\n");
