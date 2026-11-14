/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Built-in Win32 runtime, part 2: the subset of msvcrt.dll / ucrtbase.dll
 * that MinGW-built console programs import - start-up glue, stdio on the
 * three console streams, the heap, strings and an own printf engine.
 *
 * Written from the public CRT documentation. stdio is unbuffered and only
 * knows stdin/stdout/stderr; there is no locale beyond "C" + UTF-8.
 */
#include "src/ntrt/rt.h"

/* ---- memory and string primitives (also used by the compiler) ------------ */

void *memcpy(void *d, const void *s, size_t n)
{
	u8 *dp = d;
	const u8 *sp = s;

	while (n--)
		*dp++ = *sp++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	u8 *dp = d;
	const u8 *sp = s;

	if (dp < sp || dp >= sp + n)
		return memcpy(d, s, n);
	while (n--)
		dp[n] = sp[n];
	return d;
}

void *memset(void *d, int c, size_t n)
{
	u8 *dp = d;

	while (n--)
		*dp++ = (u8)c;
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const u8 *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

size_t rt_strlen(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

size_t rt_wcslen(const wchar *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

int rt_lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static int rt_upper(int c)
{
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

/* ---- printf engine ---------------------------------------------------------- */

void rt_sink_flush(struct rt_sink *s)
{
	if (s->tlen)
		rt_write(s->fd, s->tmp, s->tlen);
	s->tlen = 0;
}

static void unit(struct rt_sink *s, u32 u)
{
	if (s->wbuf) {
		if (s->len < s->cap)
			s->wbuf[s->len] = (wchar)u;
	} else if (s->buf) {
		if (s->len < s->cap)
			s->buf[s->len] = (char)u;
	} else {
		s->tmp[s->tlen++] = (char)u;
		if (s->tlen == sizeof(s->tmp))
			rt_sink_flush(s);
	}
	s->len++;
}

/* A Unicode code point: UTF-16 into wide sinks, UTF-8 into narrow ones. */
static void put_cp(struct rt_sink *s, u32 c)
{
	if (s->wbuf) {
		if (c >= 0x10000) {
			unit(s, 0xD800 + ((c - 0x10000) >> 10));
			c = 0xDC00 + ((c - 0x10000) & 0x3FF);
		}
		unit(s, c);
	} else {
		char tmp[4];
		wchar w[2] = { (wchar)c, 0 };
		size_t n;

		if (c < 0x80) {
			unit(s, c);
			return;
		}
		n = rt_utf8_from_wide(tmp, sizeof(tmp), w, 1);
		for (size_t i = 0; i < n && i < 4; i++)
			unit(s, (u8)tmp[i]);
	}
}

static void pad(struct rt_sink *s, int n, char c)
{
	while (n-- > 0)
		unit(s, (u8)c);
}

#define F_LEFT  0x01
#define F_PLUS  0x02
#define F_SPACE 0x04
#define F_ALT   0x08
#define F_ZERO  0x10

/* prefix (sign, 0x), `zeros` leading zeros, then the digits, padded to width. */
static void field(struct rt_sink *s, const char *pre, const char *dig, int ndig, int zeros,
		  int flags, int width)
{
	int npre = (int)rt_strlen(pre), total = npre + zeros + ndig;

	if (!(flags & (F_LEFT | F_ZERO)))
		pad(s, width - total, ' ');
	while (*pre)
		unit(s, (u8)*pre++);
	if ((flags & (F_LEFT | F_ZERO)) == F_ZERO)
		pad(s, width - total, '0');
	pad(s, zeros, '0');
	for (int i = 0; i < ndig; i++)
		unit(s, (u8)dig[i]);
	if (flags & F_LEFT)
		pad(s, width - total, ' ');
}

/* Digits of a non-negative double in fixed notation with `prec` decimals. */
static int fmt_fixed(char *out, int cap, double v, int prec)
{
	double r = 0.5;
	int n = 0, ie = 0;
	char ib[400];
	int ni = 0;

	for (int i = 0; i < prec; i++)
		r /= 10;
	v += r;
	while (v >= 1e18 && ie < 330) {
		v /= 10;
		ie++;
	}
	{
		u64 ip = (u64)v;

		v -= (double)ip;
		do {
			ib[ni++] = (char)('0' + ip % 10);
			ip /= 10;
		} while (ip);
	}
	while (ni && n < cap)
		out[n++] = ib[--ni];
	for (; ie && n < cap; ie--)
		out[n++] = '0';
	if (prec && n < cap)
		out[n++] = '.';
	for (int i = 0; i < prec && n < cap; i++) {
		int d;

		v *= 10;
		d = (int)v;
		if (d > 9)
			d = 9;
		out[n++] = (char)('0' + d);
		v -= d;
	}
	return n;
}

/* d.ddde+XX; returns length, exponent through *exp10. */
static int fmt_exp(char *out, int cap, double v, int prec, char e, int *exp10)
{
	int x = 0, n;
	double r = 0.5;

	if (v != 0) {
		while (v >= 10) {
			v /= 10;
			x++;
		}
		while (v < 1) {
			v *= 10;
			x--;
		}
	}
	for (int i = 0; i < prec; i++)
		r /= 10;
	if (v + r >= 10) {
		v /= 10;
		x++;
	}
	n = fmt_fixed(out, cap - 5, v, prec);
	*exp10 = x;
	out[n++] = e;
	out[n++] = x < 0 ? '-' : '+';
	if (x < 0)
		x = -x;
	if (x >= 100)
		out[n++] = (char)('0' + x / 100);
	out[n++] = (char)('0' + x / 10 % 10);
	out[n++] = (char)('0' + x % 10);
	return n;
}

static void fmt_float(struct rt_sink *s, double v, char conv, int prec, int flags, int width)
{
	char buf[420];
	const char *sign = "";
	int n, x;
	bool upper = conv >= 'A' && conv <= 'Z';
	u64 bits;

	memcpy(&bits, &v, 8);
	if (bits >> 63) {
		sign = "-";
		v = -v;
	} else if (flags & F_PLUS) {
		sign = "+";
	} else if (flags & F_SPACE) {
		sign = " ";
	}
	if (((bits >> 52) & 0x7FF) == 0x7FF) {
		field(s, sign, (bits << 12) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf"), 3,
		      0, flags & ~F_ZERO, width);
		return;
	}
	if (prec < 0)
		prec = 6;
	conv = (char)rt_lower(conv);
	if (conv == 'f') {
		n = fmt_fixed(buf, sizeof(buf), v, prec);
	} else if (conv == 'e') {
		n = fmt_exp(buf, sizeof(buf), v, prec, upper ? 'E' : 'e', &x);
	} else {                        /* %g: the shorter of %e and %f */
		int p = prec ? prec : 1;

		fmt_exp(buf, sizeof(buf), v, p - 1, 'e', &x);
		if (x >= -4 && x < p)
			n = fmt_fixed(buf, sizeof(buf), v, p - 1 - x);
		else
			n = fmt_exp(buf, sizeof(buf), v, p - 1, upper ? 'E' : 'e', &x);
		if (!(flags & F_ALT)) {
			/* Strip trailing zeros of the fraction (before any exponent). */
			int end = 0, dot = -1, k;

			while (end < n && buf[end] != 'e' && buf[end] != 'E') {
				if (buf[end] == '.')
					dot = end;
				end++;
			}
			if (dot >= 0) {
				k = end;
				while (k > dot + 1 && buf[k - 1] == '0')
					k--;
				if (k == dot + 1)
					k = dot;
				memmove(buf + k, buf + end, (size_t)(n - end));
				n -= end - k;
			}
		}
	}
	field(s, sign, buf, n, 0, flags, width);
}

/* Read one format unit from a narrow or wide format string. */
static u32 fch(const void *fmt, size_t i, bool wide)
{
	return wide ? ((const wchar *)fmt)[i] : ((const u8 *)fmt)[i];
}

void rt_format(struct rt_sink *s, const void *fmt, bool wide, const u64 *args)
{
	size_t i = 0;
	u32 c;

	while ((c = fch(fmt, i++, wide)) != 0) {
		int flags = 0, width = 0, prec = -1, size = 0;  /* size: -2 hh, -1 h, 1 l, 2 ll */
		bool wide_arg;

		if (c != '%') {
			if (wide)
				put_cp(s, c);
			else
				unit(s, c);
			continue;
		}
		for (;; i++) {
			c = fch(fmt, i, wide);
			if (c == '-')      flags |= F_LEFT;
			else if (c == '+') flags |= F_PLUS;
			else if (c == ' ') flags |= F_SPACE;
			else if (c == '#') flags |= F_ALT;
			else if (c == '0') flags |= F_ZERO;
			else break;
		}
		if (fch(fmt, i, wide) == '*') {
			width = (int)*args++;
			if (width < 0) {
				flags |= F_LEFT;
				width = -width;
			}
			i++;
		} else {
			while ((c = fch(fmt, i, wide)) >= '0' && c <= '9') {
				width = width * 10 + (int)(c - '0');
				i++;
			}
		}
		if (fch(fmt, i, wide) == '.') {
			i++;
			prec = 0;
			if (fch(fmt, i, wide) == '*') {
				prec = (int)*args++;
				i++;
			} else {
				while ((c = fch(fmt, i, wide)) >= '0' && c <= '9') {
					prec = prec * 10 + (int)(c - '0');
					i++;
				}
			}
		}
		/* Length: Windows longs are 32-bit; I64, ll, z, j, t, I are 64-bit. */
		wide_arg = wide;
		for (;; i++) {
			c = fch(fmt, i, wide);
			if (c == 'h') {
				size = size < 0 ? -2 : -1;
				wide_arg = false;
			} else if (c == 'l' || c == 'w') {
				size = size > 0 ? 2 : 1;
				wide_arg = true;
			} else if (c == 'L' || c == 'z' || c == 'j' || c == 't') {
				size = 2;
			} else if (c == 'I') {
				if (fch(fmt, i + 1, wide) == '6' && fch(fmt, i + 2, wide) == '4') {
					i += 2;
					size = 2;
				} else if (fch(fmt, i + 1, wide) == '3' && fch(fmt, i + 2, wide) == '2') {
					i += 2;
					size = 0;
				} else {
					size = 2;
				}
			} else {
				break;
			}
		}
		c = fch(fmt, i++, wide);
		if (c == 0)
			break;
		switch (c) {
		case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
			u64 v = *args++;
			bool neg = false;
			char dig[24];
			int nd = 0, base = c == 'o' ? 8 : (c == 'x' || c == 'X' || c == 'p') ? 16 : 10;
			const char *hex = c == 'x' ? "0123456789abcdef" : "0123456789ABCDEF";
			char pre[4] = "";
			int zeros;

			if (c == 'p') {
				size = 2;
				if (prec < 0)
					prec = 16;
			}
			if (c == 'd' || c == 'i') {
				s64 sv = size == 2 ? (s64)v : size == -1 ? (short)v : size == -2 ? (signed char)v
					: (int)v;

				neg = sv < 0;
				v = neg ? 0 - (u64)sv : (u64)sv;
				pre[0] = neg ? '-' : (flags & F_PLUS) ? '+' : (flags & F_SPACE) ? ' ' : 0;
			} else {
				v = size == 2 ? v : size == -1 ? (u16)v : size == -2 ? (u8)v : (u32)v;
				if ((flags & F_ALT) && v && base == 16 && c != 'p') {
					pre[0] = '0';
					pre[1] = (char)c;
				}
			}
			while (v) {
				dig[nd++] = hex[v % (u64)base];
				v /= (u64)base;
			}
			if (prec < 0 && nd == 0)
				dig[nd++] = '0';
			if ((flags & F_ALT) && base == 8 && (nd == 0 || dig[nd - 1] != '0'))
				dig[nd++] = '0';
			for (int a = 0, b = nd - 1; a < b; a++, b--) {
				char t = dig[a];

				dig[a] = dig[b];
				dig[b] = t;
			}
			zeros = prec > nd ? prec - nd : 0;
			field(s, pre, dig, nd, zeros, prec >= 0 ? flags & ~F_ZERO : flags, width);
			break;
		}
		case 'c': case 'C': {
			u32 ch = (u32)*args++;
			bool w = c == 'C' ? !wide : wide_arg;

			ch = w ? (u16)ch : (u8)ch;
			if (!(flags & F_LEFT))
				pad(s, width - 1, ' ');
			if (w)
				put_cp(s, ch);
			else
				unit(s, ch);
			if (flags & F_LEFT)
				pad(s, width - 1, ' ');
			break;
		}
		case 's': case 'S': {
			const void *str = (const void *)*args++;
			bool w = c == 'S' ? !wide : (size ? wide_arg : wide);
			size_t n = 0;

			if (!str) {
				str = w ? (const void *)u"(null)" : "(null)";
			}
			while ((prec < 0 || n < (size_t)prec) &&
			       (w ? ((const wchar *)str)[n] : ((const char *)str)[n]))
				n++;
			if (!(flags & F_LEFT))
				pad(s, width - (int)n, ' ');
			for (size_t k = 0; k < n; k++) {
				if (!w) {
					unit(s, ((const u8 *)str)[k]);
				} else {
					u32 u = ((const wchar *)str)[k];

					if (u >= 0xD800 && u < 0xDC00 && k + 1 < n) {
						u = 0x10000 + ((u - 0xD800) << 10) +
						    (((const wchar *)str)[++k] - 0xDC00);
					}
					put_cp(s, u);
				}
			}
			if (flags & F_LEFT)
				pad(s, width - (int)n, ' ');
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
			double d;

			memcpy(&d, args++, 8);
			fmt_float(s, d, (char)(c == 'a' ? 'e' : c == 'A' ? 'E' : c), prec, flags, width);
			break;
		}
		case 'n':
			args++;         /* %n is disabled, as in the Microsoft CRT */
			break;
		default:
			unit(s, c);     /* "%%" and unknown conversions print themselves */
			break;
		}
	}
}

/* ---- stdio on the console streams ----------------------------------------- */

#define FILE_SIZE 48            /* sizeof(FILE) in msvcrt */
static u8 iob[3 * FILE_SIZE] __attribute__((aligned(16)));

static int stream_fd(const void *f)
{
	const u8 *p = f;

	if (p < iob || p >= iob + sizeof(iob) || (p - iob) % FILE_SIZE)
		return -1;
	return (int)((p - iob) / FILE_SIZE);
}

static int vfmt_fd(int fd, const void *fmt, bool wide, const u64 *args)
{
	struct rt_sink s = { .fd = fd };

	if (fd <= 0)
		return -1;
	rt_format(&s, fmt, wide, args);
	rt_sink_flush(&s);
	return (int)s.len;
}

/* Into a buffer of `cap` units. Returns the untruncated length. */
static size_t vfmt_buf(void *buf, size_t cap, bool wide_out, const void *fmt, bool wide,
		       const u64 *args)
{
	struct rt_sink s = { .cap = cap ? cap - 1 : 0 };

	if (wide_out)
		s.wbuf = buf;
	else
		s.buf = buf;
	if (!buf)
		s.cap = 0, s.buf = (char *)"", s.wbuf = NULL;
	rt_format(&s, fmt, wide, args);
	if (buf && cap) {
		size_t end = s.len < cap - 1 ? s.len : cap - 1;

		if (wide_out)
			((wchar *)buf)[end] = 0;
		else
			((char *)buf)[end] = 0;
	}
	return s.len;
}

static API void *w___iob_func(void) { return iob; }
EXPORT(__iob_func);
EXPORT_AS("_iob", iob);
static API void *w___acrt_iob_func(u32 i) { return i < 3 ? iob + i * FILE_SIZE : NULL; }
EXPORT(__acrt_iob_func);

static API int w_vfprintf(void *f, const char *fmt, const u64 *ap) { return vfmt_fd(stream_fd(f), fmt, false, ap); }
EXPORT(vfprintf);
static API int w_vprintf(const char *fmt, const u64 *ap) { return vfmt_fd(1, fmt, false, ap); }
EXPORT(vprintf);
static API int w_vfwprintf(void *f, const wchar *fmt, const u64 *ap) { return vfmt_fd(stream_fd(f), fmt, true, ap); }
EXPORT(vfwprintf);

static API int w_printf(const char *fmt, ...)
{
	ms_va_list ap;
	int r;

	ms_va_start(ap, fmt);
	r = vfmt_fd(1, fmt, false, (const u64 *)ap);
	ms_va_end(ap);
	return r;
}
EXPORT(printf);

static API int w_wprintf(const wchar *fmt, ...)
{
	ms_va_list ap;
	int r;

	ms_va_start(ap, fmt);
	r = vfmt_fd(1, fmt, true, (const u64 *)ap);
	ms_va_end(ap);
	return r;
}
EXPORT(wprintf);

static API int w_fprintf(void *f, const char *fmt, ...)
{
	ms_va_list ap;
	int r;

	ms_va_start(ap, fmt);
	r = vfmt_fd(stream_fd(f), fmt, false, (const u64 *)ap);
	ms_va_end(ap);
	return r;
}
EXPORT(fprintf);

static API int w_vsprintf(char *buf, const char *fmt, const u64 *ap)
{
	return (int)vfmt_buf(buf, (size_t)1 << 30, false, fmt, false, ap);
}
EXPORT(vsprintf);

static API int w_sprintf(char *buf, const char *fmt, ...)
{
	ms_va_list ap;
	int r;

	ms_va_start(ap, fmt);
	r = w_vsprintf(buf, fmt, (const u64 *)ap);
	ms_va_end(ap);
	return r;
}
EXPORT(sprintf);

/* C99 vsnprintf: always terminated, returns the full length. */
static API int w_vsnprintf(char *buf, size_t n, const char *fmt, const u64 *ap)
{
	return (int)vfmt_buf(buf, n, false, fmt, false, ap);
}
EXPORT(vsnprintf);

static API int w_snprintf(char *buf, size_t n, const char *fmt, ...)
{
	ms_va_list ap;
	int r;

	ms_va_start(ap, fmt);
	r = w_vsnprintf(buf, n, fmt, (const u64 *)ap);
	ms_va_end(ap);
	return r;
}
EXPORT(snprintf);

/* Microsoft _vsnprintf: -1 and no terminator when the output does not fit. */
static API int w__vsnprintf(char *buf, size_t n, const char *fmt, const u64 *ap)
{
	struct rt_sink s = { .buf = buf, .cap = n };

	rt_format(&s, fmt, false, ap);
	if (s.len >= n)
		return s.len == n ? (int)n : -1;
	buf[s.len] = 0;
	return (int)s.len;
}
EXPORT(_vsnprintf);

static API int w__snprintf(char *buf, size_t n, const char *fmt, ...)
{
	ms_va_list ap;
	int r;

	ms_va_start(ap, fmt);
	r = w__vsnprintf(buf, n, fmt, (const u64 *)ap);
	ms_va_end(ap);
	return r;
}
EXPORT(_snprintf);

static API int w__vscprintf(const char *fmt, const u64 *ap) { return (int)vfmt_buf(NULL, 0, false, fmt, false, ap); }
EXPORT(_vscprintf);

static API int w__vsnwprintf(wchar *buf, size_t n, const wchar *fmt, const u64 *ap)
{
	struct rt_sink s = { .wbuf = buf, .cap = n };

	rt_format(&s, fmt, true, ap);
	if (s.len >= n)
		return s.len == n ? (int)n : -1;
	buf[s.len] = 0;
	return (int)s.len;
}
EXPORT(_vsnwprintf);

static API int w__snwprintf(wchar *buf, size_t n, const wchar *fmt, ...)
{
	ms_va_list ap;
	int r;

	ms_va_start(ap, fmt);
	r = w__vsnwprintf(buf, n, fmt, (const u64 *)ap);
	ms_va_end(ap);
	return r;
}
EXPORT(_snwprintf);

/* Legacy msvcrt swprintf takes no buffer size. */
static API int w_swprintf(wchar *buf, const wchar *fmt, ...)
{
	ms_va_list ap;
	int r;

	ms_va_start(ap, fmt);
	r = (int)vfmt_buf(buf, (size_t)1 << 30, true, fmt, true, (const u64 *)ap);
	ms_va_end(ap);
	return r;
}
EXPORT(swprintf);

/*
 * ucrtbase's printf back ends. Option bits: 1 = legacy "no terminator on
 * overflow", 2 = C99 snprintf return value.
 */
static int common_vsprintf(u64 opt, void *buf, size_t n, const void *fmt, bool wide,
			   const u64 *ap)
{
	size_t len = vfmt_buf(buf, n, wide, fmt, wide, ap);

	if (!buf)
		return (int)len;
	if (len < n)
		return (int)len;
	if (opt & 2)
		return (int)len;
	if ((opt & 1) && n) {
		if (wide)
			((wchar *)buf)[n - 1] = (wchar)0;       /* overwritten unit */
	}
	return -1;
}

static API int w___stdio_common_vsprintf(u64 opt, char *buf, size_t n, const char *fmt,
					 void *loc, const u64 *ap)
{
	(void)loc;
	return common_vsprintf(opt, buf, n, fmt, false, ap);
}
EXPORT(__stdio_common_vsprintf);

static API int w___stdio_common_vswprintf(u64 opt, wchar *buf, size_t n, const wchar *fmt,
					  void *loc, const u64 *ap)
{
	(void)loc;
	return common_vsprintf(opt, buf, n, fmt, true, ap);
}
EXPORT(__stdio_common_vswprintf);

static API int w___stdio_common_vfprintf(u64 opt, void *f, const char *fmt, void *loc,
					 const u64 *ap)
{
	(void)opt;
	(void)loc;
	return vfmt_fd(stream_fd(f), fmt, false, ap);
}
EXPORT(__stdio_common_vfprintf);

static API int w___stdio_common_vfwprintf(u64 opt, void *f, const wchar *fmt, void *loc,
					  const u64 *ap)
{
	(void)opt;
	(void)loc;
	return vfmt_fd(stream_fd(f), fmt, true, ap);
}
EXPORT(__stdio_common_vfwprintf);

static API size_t w_fwrite(const void *p, size_t size, size_t n, void *f)
{
	int fd = stream_fd(f);
	long r;

	if (fd <= 0 || !size || !n)
		return 0;
	r = rt_write(fd, p, size * n);
	return r < 0 ? 0 : (size_t)r / size;
}
EXPORT(fwrite);

static API int w_fputs(const char *str, void *f)
{
	return w_fwrite(str, 1, rt_strlen(str), f) == rt_strlen(str) ? 0 : -1;
}
EXPORT(fputs);

static API int w_fputc(int c, void *f)
{
	int fd = stream_fd(f);

	if (fd <= 0)
		return -1;
	rt_putc(fd, (char)c);
	return (u8)c;
}
EXPORT(fputc);
EXPORT_AS("putc", w_fputc);

static API int w_putchar(int c) { return w_fputc(c, iob + FILE_SIZE); }
EXPORT(putchar);

static API int w_puts(const char *str)
{
	if (w_fputs(str, iob + FILE_SIZE) < 0)
		return -1;
	return w_putchar('\n') < 0 ? -1 : 0;
}
EXPORT(puts);

static API int w_fflush(void *f) { (void)f; rt_flush(); return 0; }
EXPORT(fflush);
static API int w_setvbuf(void *f, char *b, int mode, size_t n)
{
	(void)f; (void)b; (void)mode; (void)n;
	return 0;
}
EXPORT(setvbuf);

/* ---- process start-up and exit --------------------------------------------- */

static int argc;
static char **argv;
static wchar **wargv;
static char *environ_a[1];
static wchar *environ_w[1];
static char **initenv = environ_a;
static wchar **winitenv = environ_w;
static int commode, fmode, errno_value, app_type;

/* Split the command line on blanks, honouring double quotes. */
static void build_argv(void)
{
	const char *cl = rt_cmdline();
	size_t n = rt_strlen(cl);
	char *copy = rt_alloc(n + 1);
	int cnt = 0;

	if (argv || !copy)
		return;
	argv = rt_alloc(sizeof(char *) * (n / 2 + 2));
	wargv = rt_alloc(sizeof(wchar *) * (n / 2 + 2));
	for (size_t i = 0, o = 0; i < n;) {
		bool quote = false;

		while (cl[i] == ' ' || cl[i] == '\t')
			i++;
		if (!cl[i])
			break;
		argv[cnt] = copy + o;
		while (cl[i] && (quote || (cl[i] != ' ' && cl[i] != '\t'))) {
			if (cl[i] == '"')
				quote = !quote;
			else
				copy[o++] = cl[i];
			i++;
		}
		copy[o++] = 0;
		cnt++;
	}
	for (int i = 0; i < cnt; i++) {
		size_t len = rt_strlen(argv[i]);

		wargv[i] = rt_alloc((len + 1) * 2);
		for (size_t k = 0; k < len; k++)
			wargv[i][k] = (u8)argv[i][k];
	}
	argc = cnt;
}

static API int w___getmainargs(int *pargc, char ***pargv, char ***penv, int glob, void *si)
{
	(void)glob;
	(void)si;
	build_argv();
	*pargc = argc;
	*pargv = argv;
	*penv = environ_a;
	return 0;
}
EXPORT(__getmainargs);

static API int w___wgetmainargs(int *pargc, wchar ***pargv, wchar ***penv, int glob, void *si)
{
	(void)glob;
	(void)si;
	build_argv();
	*pargc = argc;
	*pargv = wargv;
	*penv = environ_w;
	return 0;
}
EXPORT(__wgetmainargs);

static API int w__configure_narrow_argv(int mode) { (void)mode; build_argv(); return 0; }
EXPORT(_configure_narrow_argv);
EXPORT_AS("_configure_wide_argv", w__configure_narrow_argv);
static API int w__initialize_narrow_environment(void) { return 0; }
EXPORT(_initialize_narrow_environment);
EXPORT_AS("_initialize_wide_environment", w__initialize_narrow_environment);
static API char **w__get_initial_narrow_environment(void) { return environ_a; }
EXPORT(_get_initial_narrow_environment);
static API wchar **w__get_initial_wide_environment(void) { return environ_w; }
EXPORT(_get_initial_wide_environment);

static API int *w___p___argc(void) { return &argc; }
EXPORT(__p___argc);
static API char ***w___p___argv(void) { return &argv; }
EXPORT(__p___argv);
static API wchar ***w___p___wargv(void) { return &wargv; }
EXPORT(__p___wargv);
static API char ***w___p__environ(void) { return &initenv; }
EXPORT(__p__environ);
static API wchar ***w___p__wenviron(void) { return &winitenv; }
EXPORT(__p__wenviron);
static API int *w___p__commode(void) { return &commode; }
EXPORT(__p__commode);
static API int *w___p__fmode(void) { return &fmode; }
EXPORT(__p__fmode);
EXPORT_AS("__initenv", &initenv);
EXPORT_AS("__winitenv", &winitenv);
EXPORT_AS("_commode", &commode);
EXPORT_AS("_fmode", &fmode);

static API int *w__errno(void) { return &errno_value; }
EXPORT(_errno);
static API void w___set_app_type(int t) { app_type = t; }
EXPORT(__set_app_type);
EXPORT_AS("_set_app_type", w___set_app_type);
static API void w___setusermatherr(void *f) { (void)f; }
EXPORT(__setusermatherr);
static API int w__set_new_mode(int m) { (void)m; return 0; }
EXPORT(_set_new_mode);
static API int w__configthreadlocale(int m) { (void)m; return 1; }
EXPORT(_configthreadlocale);
static API void w__lock(int n) { (void)n; }
EXPORT(_lock);
EXPORT_AS("_unlock", w__lock);
static API int w____lc_codepage_func(void) { return 65001; }
EXPORT(___lc_codepage_func);
static API int w____mb_cur_max_func(void) { return 4; }
EXPORT(___mb_cur_max_func);


/* Signals are never raised; handlers are accepted and forgotten. */
static API void *w_signal(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
EXPORT(signal);

typedef API void (*vfn)(void);
typedef API int (*ifn)(void);

static API void w__initterm(vfn *a, vfn *b)
{
	for (; a < b; a++)
		if (*a)
			(*a)();
}
EXPORT(_initterm);

static API int w__initterm_e(ifn *a, ifn *b)
{
	for (; a < b; a++)
		if (*a) {
			int r = (*a)();

			if (r)
				return r;
		}
	return 0;
}
EXPORT(_initterm_e);

#define ATEXIT_MAX 64
static vfn atexit_fns[ATEXIT_MAX];
static int atexit_n;

void rt_run_atexit(void)
{
	while (atexit_n > 0) {
		vfn f = atexit_fns[--atexit_n];

		f();
	}
}

static API vfn w__onexit(vfn f)
{
	if (atexit_n == ATEXIT_MAX)
		return NULL;
	atexit_fns[atexit_n++] = f;
	return f;
}
EXPORT(_onexit);

static API int w_atexit(vfn f) { return w__onexit(f) ? 0 : -1; }
EXPORT(atexit);
EXPORT_AS("_crt_atexit", w_atexit);

static API void w_exit(int code)
{
	rt_run_atexit();
	rt_exit((u32)code);
}
EXPORT(exit);

static API void w__exit(int code) { rt_exit((u32)code); }
EXPORT(_exit);
EXPORT_AS("_Exit", w__exit);

static API void w__cexit(void) { rt_run_atexit(); }
EXPORT(_cexit);
static API void w__c_exit(void) { }
EXPORT(_c_exit);

static API void w_abort(void)
{
	static const char msg[] = "abnormal program termination\n";

	rt_write(2, msg, sizeof(msg) - 1);
	rt_exit(3);
}
EXPORT(abort);

static API void w__amsg_exit(int n)
{
	static const char msg[] = "runtime error (_amsg_exit)\n";

	(void)n;
	rt_write(2, msg, sizeof(msg) - 1);
	rt_exit(255);
}
EXPORT(_amsg_exit);

/* ---- heap -------------------------------------------------------------------- */

static API void *w_malloc(size_t n) { return rt_alloc(n ? n : 1); }
EXPORT(malloc);
static API void w_free(void *p) { rt_free(p); }
EXPORT(free);
static API void *w_realloc(void *p, size_t n) { return rt_realloc(p, n); }
EXPORT(realloc);

static API void *w_calloc(size_t a, size_t b)
{
	if (b && a > ~(size_t)0 / b)
		return NULL;
	return rt_alloc(a && b ? a * b : 1);   /* blocks come zeroed */
}
EXPORT(calloc);

/* ---- strings ----------------------------------------------------------------- */

static API void *w_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
EXPORT(memcpy);
static API void *w_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
EXPORT(memmove);
static API void *w_memset(void *d, int c, size_t n) { return memset(d, c, n); }
EXPORT(memset);
static API int w_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
EXPORT(memcmp);

static API void *w_memchr(const void *p, int c, size_t n)
{
	for (const u8 *q = p; n--; q++)
		if (*q == (u8)c)
			return (void *)q;
	return NULL;
}
EXPORT(memchr);

static API size_t w_strlen(const char *s) { return rt_strlen(s); }
EXPORT(strlen);

static API size_t w_strnlen(const char *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n])
		n++;
	return n;
}
EXPORT(strnlen);

static API int w_strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++)
		if (*a != *b || !*a)
			return (u8)*a - (u8)*b;
	return 0;
}
EXPORT(strncmp);

static API int w_strcmp(const char *a, const char *b) { return w_strncmp(a, b, ~(size_t)0); }
EXPORT(strcmp);

static API int w__strnicmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++)
		if (rt_lower((u8)*a) != rt_lower((u8)*b) || !*a)
			return rt_lower((u8)*a) - rt_lower((u8)*b);
	return 0;
}
EXPORT(_strnicmp);

static API int w__stricmp(const char *a, const char *b) { return w__strnicmp(a, b, ~(size_t)0); }
EXPORT(_stricmp);

static API char *w_strcpy(char *d, const char *s) { return memcpy(d, s, rt_strlen(s) + 1); }
EXPORT(strcpy);

static API char *w_strncpy(char *d, const char *s, size_t n)
{
	size_t i = 0;

	for (; i < n && s[i]; i++)
		d[i] = s[i];
	for (; i < n; i++)
		d[i] = 0;
	return d;
}
EXPORT(strncpy);

static API char *w_strcat(char *d, const char *s)
{
	w_strcpy(d + rt_strlen(d), s);
	return d;
}
EXPORT(strcat);

static API char *w_strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}
EXPORT(strchr);

static API char *w_strrchr(const char *s, int c)
{
	const char *r = NULL;

	for (;; s++) {
		if (*s == (char)c)
			r = s;
		if (!*s)
			return (char *)r;
	}
}
EXPORT(strrchr);

static API char *w_strstr(const char *h, const char *n)
{
	size_t len = rt_strlen(n);

	for (; *h; h++)
		if (!memcmp(h, n, len))
			return (char *)h;
	return len ? NULL : (char *)h;
}
EXPORT(strstr);

static API size_t w_strcspn(const char *s, const char *rej)
{
	size_t n = 0;

	while (s[n] && !w_strchr(rej, s[n]))
		n++;
	return n;
}
EXPORT(strcspn);

static API size_t w_strspn(const char *s, const char *acc)
{
	size_t n = 0;

	while (s[n] && w_strchr(acc, s[n]))
		n++;
	return n;
}
EXPORT(strspn);

static API char *w__strdup(const char *s)
{
	size_t n = rt_strlen(s) + 1;
	char *d = rt_alloc(n);

	return d ? memcpy(d, s, n) : NULL;
}
EXPORT(_strdup);
EXPORT_AS("strdup", w__strdup);

static API char *w_strerror(int e) { (void)e; return (char *)"unknown error"; }
EXPORT(strerror);
static API char *w_getenv(const char *n) { (void)n; return NULL; }
EXPORT(getenv);
static API wchar *w__wgetenv(const wchar *n) { (void)n; return NULL; }
EXPORT(_wgetenv);

static API int w_tolower(int c) { return rt_lower(c); }
EXPORT(tolower);
static API int w_toupper(int c) { return rt_upper(c); }
EXPORT(toupper);
static API int w_isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
EXPORT(isspace);
static API int w_isdigit(int c) { return c >= '0' && c <= '9'; }
EXPORT(isdigit);
static API int w_isalpha(int c) { return rt_lower(c) >= 'a' && rt_lower(c) <= 'z'; }
EXPORT(isalpha);

static API long long w__strtoi64(const char *s, char **end, int base)
{
	bool neg = false;
	unsigned long long v = 0;

	while (w_isspace((u8)*s))
		s++;
	if (*s == '-' || *s == '+')
		neg = *s++ == '-';
	if ((base == 0 || base == 16) && s[0] == '0' && rt_lower(s[1]) == 'x') {
		s += 2;
		base = 16;
	} else if (base == 0) {
		base = s[0] == '0' ? 8 : 10;
	}
	for (;; s++) {
		int d = w_isdigit((u8)*s) ? *s - '0' : w_isalpha((u8)*s) ? rt_lower(*s) - 'a' + 10 : 99;

		if (d >= base)
			break;
		v = v * (unsigned)base + (unsigned)d;
	}
	if (end)
		*end = (char *)s;
	return neg ? -(long long)v : (long long)v;
}
EXPORT(_strtoi64);

static API int w_strtol(const char *s, char **end, int base) { return (int)w__strtoi64(s, end, base); }
EXPORT(strtol);
EXPORT_AS("strtoul", w_strtol);
static API int w_atoi(const char *s) { return (int)w__strtoi64(s, NULL, 10); }
EXPORT(atoi);
EXPORT_AS("atol", w_atoi);

static API size_t w_wcslen(const wchar *s) { return rt_wcslen(s); }
EXPORT(wcslen);

static API int w_wcsncmp(const wchar *a, const wchar *b, size_t n)
{
	for (; n; n--, a++, b++)
		if (*a != *b || !*a)
			return *a - *b;
	return 0;
}
EXPORT(wcsncmp);

static API int w_wcscmp(const wchar *a, const wchar *b) { return w_wcsncmp(a, b, ~(size_t)0); }
EXPORT(wcscmp);

static API int w__wcsnicmp(const wchar *a, const wchar *b, size_t n)
{
	for (; n; n--, a++, b++)
		if (rt_lower(*a) != rt_lower(*b) || !*a)
			return rt_lower(*a) - rt_lower(*b);
	return 0;
}
EXPORT(_wcsnicmp);

static API int w__wcsicmp(const wchar *a, const wchar *b) { return w__wcsnicmp(a, b, ~(size_t)0); }
EXPORT(_wcsicmp);

static API wchar *w_wcscpy(wchar *d, const wchar *s)
{
	return memcpy(d, s, (rt_wcslen(s) + 1) * 2);
}
EXPORT(wcscpy);

static API wchar *w_wcscat(wchar *d, const wchar *s)
{
	w_wcscpy(d + rt_wcslen(d), s);
	return d;
}
EXPORT(wcscat);

static API wchar *w_wcschr(const wchar *s, wchar c)
{
	for (;; s++) {
		if (*s == c)
			return (wchar *)s;
		if (!*s)
			return NULL;
	}
}
EXPORT(wcschr);

static API wchar *w_wcsrchr(const wchar *s, wchar c)
{
	const wchar *r = NULL;

	for (;; s++) {
		if (*s == c)
			r = s;
		if (!*s)
			return (wchar *)r;
	}
}
EXPORT(wcsrchr);

static API wchar *w__wcslwr(wchar *s)
{
	for (wchar *p = s; *p; p++)
		*p = (wchar)rt_lower(*p);
	return s;
}
EXPORT(_wcslwr);

static API wchar *w__wcsupr(wchar *s)
{
	for (wchar *p = s; *p; p++)
		*p = (wchar)rt_upper(*p);
	return s;
}
EXPORT(_wcsupr);

static API wchar *w__wcsdup(const wchar *s)
{
	size_t n = (rt_wcslen(s) + 1) * 2;
	wchar *d = rt_alloc(n);

	return d ? memcpy(d, s, n) : NULL;
}
EXPORT(_wcsdup);

/* struct lconv for the "C" locale: ten strings, then CHAR_MAX counters. */
static API void *w_localeconv(void)
{
	static struct {
		const char *str[10];
		char num[8];
	} lc;

	for (int i = 0; i < 10; i++)
		lc.str[i] = "";
	lc.str[0] = ".";
	memset(lc.num, 127, sizeof(lc.num));
	return &lc;
}
EXPORT(localeconv);
