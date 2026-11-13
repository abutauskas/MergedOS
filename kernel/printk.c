/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Kernel formatted output. Supports the printf subset used across the tree:
 *   flags '-' '0', field width, length modifiers l / ll / z,
 *   conversions d i u x X p s c %.
 */
#include <mergedos/kernel.h>
#include <mergedos/console.h>
#include <mergedos/io.h>

struct outbuf {
	char *buf;
	size_t cap, pos;
};

static void put(struct outbuf *o, char c)
{
	if (o->pos + 1 < o->cap)
		o->buf[o->pos] = c;
	o->pos++;
}

static void pad(struct outbuf *o, int n, char c)
{
	while (n-- > 0)
		put(o, c);
}

static void put_num(struct outbuf *o, unsigned long long v, unsigned base, bool neg,
		    int width, bool zero, bool left, bool upper)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[24];
	int n = 0, len;

	do {
		tmp[n++] = digits[v % base];
		v /= base;
	} while (v);
	len = n + (neg ? 1 : 0);

	if (!left && !zero)
		pad(o, width - len, ' ');
	if (neg)
		put(o, '-');
	if (!left && zero)
		pad(o, width - len, '0');
	while (n)
		put(o, tmp[--n]);
	if (left)
		pad(o, width - len, ' ');
}

int kvsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
	struct outbuf o = { buf, cap, 0 };

	for (; *fmt; fmt++) {
		bool left = false, zero = false;
		int width = 0, lng = 0;

		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-')
				left = true;
			else if (*fmt == '0')
				zero = true;
			else
				break;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l' || *fmt == 'z') {
			lng += (*fmt == 'z') ? 2 : 1;
			fmt++;
		}

		switch (*fmt) {
		case 'd':
		case 'i': {
			long long v = lng >= 2 ? va_arg(ap, long long)
				    : lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
			unsigned long long mag = v < 0 ? 0ULL - (unsigned long long)v
						       : (unsigned long long)v;
			put_num(&o, mag, 10, v < 0, width, zero, left, false);
			break;
		}
		case 'u':
		case 'x':
		case 'X': {
			unsigned long long v = lng >= 2 ? va_arg(ap, unsigned long long)
					     : lng == 1 ? va_arg(ap, unsigned long)
							: va_arg(ap, unsigned int);
			put_num(&o, v, *fmt == 'u' ? 10 : 16, false, width, zero, left,
				*fmt == 'X');
			break;
		}
		case 'p':
			put(&o, '0');
			put(&o, 'x');
			put_num(&o, (unsigned long long)(uintptr_t)va_arg(ap, void *), 16,
				false, 16, true, false, false);
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			int len;

			if (!s)
				s = "(null)";
			len = (int)strlen(s);
			if (!left)
				pad(&o, width - len, ' ');
			while (*s)
				put(&o, *s++);
			if (left)
				pad(&o, width - len, ' ');
			break;
		}
		case 'c':
			put(&o, (char)va_arg(ap, int));
			break;
		case '%':
			put(&o, '%');
			break;
		case '\0':
			fmt--;                  /* trailing '%': stop cleanly */
			break;
		default:
			put(&o, '%');
			put(&o, *fmt);
			break;
		}
	}

	if (cap)
		buf[o.pos < cap ? o.pos : cap - 1] = '\0';
	return (int)o.pos;
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = kvsnprintf(buf, cap, fmt, ap);
	va_end(ap);
	return n;
}

void kprintf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = kvsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	console_write(buf, MOS_MIN((size_t)n, sizeof(buf) - 1));
}

void panic(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	__asm__ volatile("cli");
	va_start(ap, fmt);
	n = kvsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	console_set_attr(VGA_ATTR(VGA_WHITE, VGA_RED));
	kprintf("\n*** MergedOS kernel panic ***\n");
	console_write(buf, MOS_MIN((size_t)n, sizeof(buf) - 1));
	kprintf("\nSystem halted.\n");
	outb(0xF4, 0x7F);                       /* QEMU isa-debug-exit: fail the test run */
	for (;;)
		cpu_halt();
}
