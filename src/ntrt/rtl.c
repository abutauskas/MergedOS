/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * MergedOS ntdll, part 2: Rtl* run-time library - strings and code pages,
 * paths, the environment block, heaps, bitmaps, time, error codes, SIDs and
 * the CRT functions ntdll exports. Written from the documented NT API.
 */
#include "src/ntrt/nt.h"

#define STATUS_VARIABLE_NOT_FOUND 0xC0000100u
#define STATUS_NAME_TOO_LONG      0xC0000106u
#define STATUS_SXS_KEY_NOT_FOUND  0xC0150008u

/* ---- UTF-8 / UTF-16 ------------------------------------------------------------- */

size_t nt_wtou(char *out, size_t cap, const wchar *in, size_t n)
{
	size_t o = 0;

	for (size_t i = 0; i < n; i++) {
		u32 c = in[i];
		u8 tmp[4];
		size_t k;

		if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && in[i + 1] >= 0xDC00 && in[i + 1] < 0xE000)
			c = 0x10000 + ((c - 0xD800) << 10) + (in[++i] - 0xDC00);
		if (c < 0x80) {
			tmp[0] = (u8)c;
			k = 1;
		} else if (c < 0x800) {
			tmp[0] = (u8)(0xC0 | c >> 6);
			tmp[1] = (u8)(0x80 | (c & 63));
			k = 2;
		} else if (c < 0x10000) {
			tmp[0] = (u8)(0xE0 | c >> 12);
			tmp[1] = (u8)(0x80 | ((c >> 6) & 63));
			tmp[2] = (u8)(0x80 | (c & 63));
			k = 3;
		} else {
			tmp[0] = (u8)(0xF0 | c >> 18);
			tmp[1] = (u8)(0x80 | ((c >> 12) & 63));
			tmp[2] = (u8)(0x80 | ((c >> 6) & 63));
			tmp[3] = (u8)(0x80 | (c & 63));
			k = 4;
		}
		for (size_t j = 0; j < k; j++, o++)
			if (out && o < cap)
				out[o] = (char)tmp[j];
	}
	return o;
}

size_t nt_utow(wchar *out, size_t cap, const char *in8, size_t n)
{
	const u8 *in = (const u8 *)in8;
	size_t o = 0;

	for (size_t i = 0; i < n; ) {
		u32 c = in[i++];
		int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;

		if (more) {
			c &= 0x3F >> more;
			for (; more && i < n && (in[i] & 0xC0) == 0x80; more--)
				c = c << 6 | (in[i++] & 63);
			if (more)
				c = 0xFFFD;
		} else if (c >= 0x80) {
			c = 0xFFFD;
		}
		if (c >= 0x10000) {
			if (out && o + 1 < cap) {
				out[o] = (wchar)(0xD800 + ((c - 0x10000) >> 10));
				out[o + 1] = (wchar)(0xDC00 + ((c - 0x10000) & 0x3FF));
			}
			o += 2;
		} else {
			if (out && o < cap)
				out[o] = (wchar)c;
			o++;
		}
	}
	return o;
}

/* ---- case and comparison -------------------------------------------------------- */

wchar nt_upcase(wchar c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
		return (wchar)(c - 32);
	if (c == 0xFF)
		return 0x178;
	return c;
}

wchar nt_downcase(wchar c)
{
	if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
		return (wchar)(c + 32);
	return c;
}

int nt_wcsnicmp(const wchar *a, const wchar *b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		wchar x = nt_upcase(a[i]), y = nt_upcase(b[i]);

		if (x != y)
			return (int)x - (int)y;
		if (!x)
			break;
	}
	return 0;
}

int nt_wcsicmp(const wchar *a, const wchar *b)
{
	return nt_wcsnicmp(a, b, (size_t)-1);
}

int nt_stricmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int x = rt_lower((u8)*a), y = rt_lower((u8)*b);

		if (x != y || !x)
			return x - y;
	}
}

void nt_init_ustr(UNICODE_STRING *u, const wchar *s)
{
	size_t n = s ? rt_wcslen(s) * 2 : 0;

	if (n > 0xFFFC)
		n = 0xFFFC;
	u->len = (u16)n;
	u->max = s ? (u16)(n + 2) : 0;
	u->buf = (wchar *)s;
}

/* ---- code pages ----------------------------------------------------------------- */

/* CPTABLEINFO, as RtlInitCodePageTable fills it. */
struct cptable {
	u16 cp, maxchar, defchar, unidefchar, transdef, transunidef, dbcs;
	u8 lead[12];
	u16 *mb;                        /* byte -> UTF-16                */
	void *wc;                       /* UTF-16 -> byte(s)             */
	u16 *ranges, *offsets;
};

static struct cptable ansi_cp, oem_cp;
static bool cp_ready;

static API void w_RtlInitCodePageTable(u16 *p, struct cptable *t)
{
	u16 hdr = p[0];

	memset(t, 0, sizeof(*t));
	if (p[1] == 65001 || !hdr) {            /* UTF-8 pseudo table */
		t->cp = 65001;
		t->maxchar = 4;
		t->defchar = '?';
		t->unidefchar = 0xFFFD;
		return;
	}
	/* Header (in 16-bit words): size, code page, max char size, default
	 * chars, lead bytes. Then the multi-byte table: its own size word, 256
	 * entries, an optional 256-entry glyph table, DBCS ranges; the wide
	 * table follows the multi-byte table. */
	t->cp = p[1];
	t->maxchar = p[2];
	t->defchar = p[3];
	t->unidefchar = p[4];
	t->transdef = p[5];
	t->transunidef = p[6];
	memcpy(t->lead, p + 7, 12);
	p += hdr;
	t->wc = p + p[0] + 1;
	t->mb = ++p;
	p += 256;
	if (*p++)
		p += 256;
	t->ranges = p;
	t->dbcs = *p ? 1 : 0;
	t->offsets = *p ? p + 1 : NULL;
}
EXPORT(RtlInitCodePageTable);

static void cp_init(void)
{
	void *a, *o;

	if (cp_ready)
		return;
	cp_ready = true;
	a = *(void **)(nt_peb + 0xA0);
	o = *(void **)(nt_peb + 0xA8);
	if (a)
		w_RtlInitCodePageTable(a, &ansi_cp);
	if (o)
		w_RtlInitCodePageTable(o, &oem_cp);
}

/* Single-byte code page conversion; without a table, Latin-1. DBCS pages are not used. */
static size_t cp_to_wide(const struct cptable *t, wchar *out, size_t cap, const char *in, size_t n)
{
	size_t i;

	if (t->cp == 65001)
		return nt_utow(out, cap, in, n);
	for (i = 0; i < n && (!out || i < cap); i++)
		if (out)
			out[i] = t->mb ? t->mb[(u8)in[i]] : (wchar)(u8)in[i];
	return out ? i : n;
}

static size_t cp_from_wide(const struct cptable *t, char *out, size_t cap, const wchar *in,
			   size_t n)
{
	size_t i;

	if (t->cp == 65001)
		return nt_wtou(out, cap, in, n);
	for (i = 0; i < n && (!out || i < cap); i++)
		if (out)
			out[i] = t->wc && !t->dbcs ? (char)((u8 *)t->wc)[in[i]] :
				 in[i] < 0x100 ? (char)in[i] : '?';
	return out ? i : n;
}

static struct cptable *ansi(void)
{
	cp_init();
	return &ansi_cp;
}

static struct cptable *oem(void)
{
	cp_init();
	return &oem_cp;
}

static API NTSTATUS w_RtlMultiByteToUnicodeN(wchar *dst, u32 dlen, u32 *ret, const char *src,
					     u32 slen)
{
	size_t n = cp_to_wide(ansi(), dst, dlen / 2, src, slen);

	if (ret)
		*ret = (u32)n * 2;
	return 0;
}
EXPORT(RtlMultiByteToUnicodeN);

static API NTSTATUS w_RtlOemToUnicodeN(wchar *dst, u32 dlen, u32 *ret, const char *src, u32 slen)
{
	size_t n = cp_to_wide(oem(), dst, dlen / 2, src, slen);

	if (ret)
		*ret = (u32)n * 2;
	return 0;
}
EXPORT(RtlOemToUnicodeN);

static API NTSTATUS w_RtlMultiByteToUnicodeSize(u32 *size, const char *src, u32 slen)
{
	*size = (u32)cp_to_wide(ansi(), NULL, 0, src, slen) * 2;
	return 0;
}
EXPORT(RtlMultiByteToUnicodeSize);

static API NTSTATUS w_RtlUnicodeToMultiByteN(char *dst, u32 dlen, u32 *ret, const wchar *src,
					     u32 slen)
{
	size_t n = cp_from_wide(ansi(), dst, dlen, src, slen / 2);

	if (ret)
		*ret = (u32)n;
	return 0;
}
EXPORT(RtlUnicodeToMultiByteN);

static API NTSTATUS w_RtlUnicodeToOemN(char *dst, u32 dlen, u32 *ret, const wchar *src, u32 slen)
{
	size_t n = cp_from_wide(oem(), dst, dlen, src, slen / 2);

	if (ret)
		*ret = (u32)n;
	return 0;
}
EXPORT(RtlUnicodeToOemN);

static API NTSTATUS w_RtlUnicodeToMultiByteSize(u32 *size, const wchar *src, u32 slen)
{
	*size = (u32)cp_from_wide(ansi(), NULL, 0, src, slen / 2);
	return 0;
}
EXPORT(RtlUnicodeToMultiByteSize);

static API NTSTATUS w_RtlUTF8ToUnicodeN(wchar *dst, u32 dlen, u32 *ret, const char *src, u32 slen)
{
	size_t n = nt_utow(dst, dst ? dlen / 2 : 0, src, slen);

	if (ret)
		*ret = (u32)(dst && n > dlen / 2 ? dlen / 2 : n) * 2;
	return dst && n > dlen / 2 ? STATUS_BUFFER_TOO_SMALL : 0;
}
EXPORT(RtlUTF8ToUnicodeN);

static API NTSTATUS w_RtlUnicodeToUTF8N(char *dst, u32 dlen, u32 *ret, const wchar *src, u32 slen)
{
	size_t n = nt_wtou(dst, dst ? dlen : 0, src, slen / 2);

	if (ret)
		*ret = (u32)(dst && n > dlen ? dlen : n);
	return dst && n > dlen ? STATUS_BUFFER_TOO_SMALL : 0;
}
EXPORT(RtlUnicodeToUTF8N);

static API NTSTATUS w_RtlUpcaseUnicodeToMultiByteN(char *dst, u32 dlen, u32 *ret, const wchar *src,
						   u32 slen)
{
	wchar tmp[256];
	u32 n = slen / 2 < 256 ? slen / 2 : 256;

	for (u32 i = 0; i < n; i++)
		tmp[i] = nt_upcase(src[i]);
	return w_RtlUnicodeToMultiByteN(dst, dlen, ret, tmp, n * 2);
}
EXPORT(RtlUpcaseUnicodeToMultiByteN);

/* ---- counted strings ------------------------------------------------------------ */

static API void w_RtlInitUnicodeString(UNICODE_STRING *u, const wchar *s)
{
	nt_init_ustr(u, s);
}
EXPORT(RtlInitUnicodeString);

static API NTSTATUS w_RtlInitUnicodeStringEx(UNICODE_STRING *u, const wchar *s)
{
	if (s && rt_wcslen(s) > 0x7FFE)
		return STATUS_NAME_TOO_LONG;
	nt_init_ustr(u, s);
	return 0;
}
EXPORT(RtlInitUnicodeStringEx);

static API void w_RtlInitAnsiString(ANSI_STRING *a, const char *s)
{
	size_t n = s ? rt_strlen(s) : 0;

	a->len = (u16)n;
	a->max = s ? (u16)(n + 1) : 0;
	a->buf = (char *)s;
}
EXPORT(RtlInitAnsiString);
EXPORT_AS("RtlInitString", w_RtlInitAnsiString);

static API NTSTATUS w_RtlInitAnsiStringEx(ANSI_STRING *a, const char *s)
{
	w_RtlInitAnsiString(a, s);
	return 0;
}
EXPORT(RtlInitAnsiStringEx);

static API BOOL w_RtlCreateUnicodeString(UNICODE_STRING *u, const wchar *s)
{
	size_t n = (rt_wcslen(s) + 1) * 2;

	u->buf = nt_alloc(n);
	if (!u->buf)
		return 0;
	memcpy(u->buf, s, n);
	u->len = (u16)(n - 2);
	u->max = (u16)n;
	return 1;
}
EXPORT(RtlCreateUnicodeString);

static API BOOL w_RtlCreateUnicodeStringFromAsciiz(UNICODE_STRING *u, const char *s)
{
	size_t n = rt_strlen(s);

	u->buf = nt_alloc((n + 1) * 2);
	if (!u->buf)
		return 0;
	n = cp_to_wide(ansi(), u->buf, n, s, n);
	u->buf[n] = 0;
	u->len = (u16)(n * 2);
	u->max = (u16)(n * 2 + 2);
	return 1;
}
EXPORT(RtlCreateUnicodeStringFromAsciiz);

static API void w_RtlFreeUnicodeString(UNICODE_STRING *u)
{
	nt_free(u->buf);
	u->buf = NULL;
	u->len = u->max = 0;
}
EXPORT(RtlFreeUnicodeString);
EXPORT_AS("RtlFreeAnsiString", w_RtlFreeUnicodeString);
EXPORT_AS("RtlFreeOemString", w_RtlFreeUnicodeString);

static API void w_RtlCopyUnicodeString(UNICODE_STRING *d, const UNICODE_STRING *s)
{
	u16 n;

	if (!s) {
		d->len = 0;
		return;
	}
	n = s->len < d->max ? s->len : d->max;
	memcpy(d->buf, s->buf, n);
	d->len = n;
	if (n + 2 <= d->max)
		d->buf[n / 2] = 0;
}
EXPORT(RtlCopyUnicodeString);

static API NTSTATUS w_RtlDuplicateUnicodeString(u32 flags, const UNICODE_STRING *s,
						UNICODE_STRING *d)
{
	u32 n = s ? s->len : 0;

	if (!n && !(flags & 2)) {               /* ALLOCATE_NULL_STRING */
		d->len = d->max = 0;
		d->buf = NULL;
		return 0;
	}
	d->buf = nt_alloc(n + 2);
	if (!d->buf)
		return STATUS_NO_MEMORY;
	if (n)
		memcpy(d->buf, s->buf, n);
	d->len = (u16)n;
	d->max = (u16)(n + 2);
	return 0;
}
EXPORT(RtlDuplicateUnicodeString);

static API NTSTATUS w_RtlAppendUnicodeToString(UNICODE_STRING *d, const wchar *s)
{
	size_t n = s ? rt_wcslen(s) * 2 : 0;

	if (d->len + n > d->max)
		return STATUS_BUFFER_TOO_SMALL;
	memcpy((u8 *)d->buf + d->len, s, n);
	d->len = (u16)(d->len + n);
	if (d->len + 2 <= d->max)
		d->buf[d->len / 2] = 0;
	return 0;
}
EXPORT(RtlAppendUnicodeToString);

static API NTSTATUS w_RtlAppendUnicodeStringToString(UNICODE_STRING *d, const UNICODE_STRING *s)
{
	if (d->len + s->len > d->max)
		return STATUS_BUFFER_TOO_SMALL;
	memcpy((u8 *)d->buf + d->len, s->buf, s->len);
	d->len = (u16)(d->len + s->len);
	if (d->len + 2 <= d->max)
		d->buf[d->len / 2] = 0;
	return 0;
}
EXPORT(RtlAppendUnicodeStringToString);

static API int w_RtlCompareUnicodeStrings(const wchar *a, size_t an, const wchar *b, size_t bn,
					  u8 ci)
{
	size_t n = an < bn ? an : bn;

	for (size_t i = 0; i < n; i++) {
		wchar x = ci ? nt_upcase(a[i]) : a[i], y = ci ? nt_upcase(b[i]) : b[i];

		if (x != y)
			return (int)x - (int)y;
	}
	return (int)an - (int)bn;
}
EXPORT(RtlCompareUnicodeStrings);

static API int w_RtlCompareUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b, u8 ci)
{
	return w_RtlCompareUnicodeStrings(a->buf, a->len / 2, b->buf, b->len / 2, ci);
}
EXPORT(RtlCompareUnicodeString);

static API u8 w_RtlEqualUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b, u8 ci)
{
	return a->len == b->len && !w_RtlCompareUnicodeString(a, b, ci);
}
EXPORT(RtlEqualUnicodeString);

static API u8 w_RtlPrefixUnicodeString(const UNICODE_STRING *p, const UNICODE_STRING *s, u8 ci)
{
	return p->len <= s->len && !w_RtlCompareUnicodeStrings(p->buf, p->len / 2, s->buf,
								 p->len / 2, ci);
}
EXPORT(RtlPrefixUnicodeString);

static API wchar w_RtlUpcaseUnicodeChar(wchar c)
{
	return nt_upcase(c);
}
EXPORT(RtlUpcaseUnicodeChar);
EXPORT_AS("towupper", w_RtlUpcaseUnicodeChar);

static API wchar w_RtlDowncaseUnicodeChar(wchar c)
{
	return nt_downcase(c);
}
EXPORT(RtlDowncaseUnicodeChar);
EXPORT_AS("towlower", w_RtlDowncaseUnicodeChar);

static API char w_RtlUpperChar(char c)
{
	return c >= 'a' && c <= 'z' ? (char)(c - 32) : c;
}
EXPORT(RtlUpperChar);

static NTSTATUS case_copy(UNICODE_STRING *d, const UNICODE_STRING *s, u8 alloc, bool up)
{
	if (alloc) {
		d->buf = nt_alloc(s->len + 2);
		if (!d->buf)
			return STATUS_NO_MEMORY;
		d->max = (u16)(s->len + 2);
	} else if (s->len > d->max) {
		return STATUS_BUFFER_OVERFLOW;
	}
	for (u32 i = 0; i < s->len / 2u; i++)
		d->buf[i] = up ? nt_upcase(s->buf[i]) : nt_downcase(s->buf[i]);
	d->len = s->len;
	return 0;
}

static API NTSTATUS w_RtlUpcaseUnicodeString(UNICODE_STRING *d, const UNICODE_STRING *s, u8 alloc)
{
	return case_copy(d, s, alloc, true);
}
EXPORT(RtlUpcaseUnicodeString);

static API NTSTATUS w_RtlDowncaseUnicodeString(UNICODE_STRING *d, const UNICODE_STRING *s,
					       u8 alloc)
{
	return case_copy(d, s, alloc, false);
}
EXPORT(RtlDowncaseUnicodeString);

static NTSTATUS to_wide(UNICODE_STRING *d, const ANSI_STRING *s, u8 alloc, struct cptable *t)
{
	size_t n = cp_to_wide(t, NULL, 0, s->buf, s->len);

	if (n * 2 + 2 > 0xFFFF)
		return STATUS_INVALID_PARAMETER;
	if (alloc) {
		d->buf = nt_alloc(n * 2 + 2);
		if (!d->buf)
			return STATUS_NO_MEMORY;
		d->max = (u16)(n * 2 + 2);
	} else if (n * 2 > d->max) {
		return STATUS_BUFFER_OVERFLOW;
	}
	cp_to_wide(t, d->buf, n, s->buf, s->len);
	d->len = (u16)(n * 2);
	if (d->len + 2 <= d->max)
		d->buf[n] = 0;
	return 0;
}

static NTSTATUS to_narrow(ANSI_STRING *d, const UNICODE_STRING *s, u8 alloc, struct cptable *t)
{
	size_t n = cp_from_wide(t, NULL, 0, s->buf, s->len / 2);
	NTSTATUS st = 0;

	if (alloc) {
		d->buf = nt_alloc(n + 1);
		if (!d->buf)
			return STATUS_NO_MEMORY;
		d->max = (u16)(n + 1);
	} else if (n >= d->max) {
		if (!d->max)
			return STATUS_BUFFER_OVERFLOW;
		n = d->max - 1u;
		st = STATUS_BUFFER_OVERFLOW;
	}
	cp_from_wide(t, d->buf, n, s->buf, s->len / 2);
	d->buf[n] = 0;
	d->len = (u16)n;
	return st;
}

static API NTSTATUS w_RtlAnsiStringToUnicodeString(UNICODE_STRING *d, const ANSI_STRING *s, u8 a)
{
	return to_wide(d, s, a, ansi());
}
EXPORT(RtlAnsiStringToUnicodeString);

static API NTSTATUS w_RtlOemStringToUnicodeString(UNICODE_STRING *d, const ANSI_STRING *s, u8 a)
{
	return to_wide(d, s, a, oem());
}
EXPORT(RtlOemStringToUnicodeString);

static API NTSTATUS w_RtlUnicodeStringToAnsiString(ANSI_STRING *d, const UNICODE_STRING *s, u8 a)
{
	return to_narrow(d, s, a, ansi());
}
EXPORT(RtlUnicodeStringToAnsiString);

static API NTSTATUS w_RtlUnicodeStringToOemString(ANSI_STRING *d, const UNICODE_STRING *s, u8 a)
{
	return to_narrow(d, s, a, oem());
}
EXPORT(RtlUnicodeStringToOemString);

static API u32 w_RtlUnicodeStringToAnsiSize(const UNICODE_STRING *s)
{
	return (u32)cp_from_wide(ansi(), NULL, 0, s->buf, s->len / 2) + 1;
}
EXPORT(RtlUnicodeStringToAnsiSize);
EXPORT_AS("RtlUnicodeStringToOemSize", w_RtlUnicodeStringToAnsiSize);

static API u32 w_RtlAnsiStringToUnicodeSize(const ANSI_STRING *s)
{
	return (u32)cp_to_wide(ansi(), NULL, 0, s->buf, s->len) * 2 + 2;
}
EXPORT(RtlAnsiStringToUnicodeSize);
EXPORT_AS("RtlOemStringToUnicodeSize", w_RtlAnsiStringToUnicodeSize);

static API u8 w_RtlIsTextUnicode(const void *buf, int len, int *flags)
{
	(void)buf;
	(void)len;
	if (flags)
		*flags = 0;
	return 0;
}
EXPORT(RtlIsTextUnicode);

static API NTSTATUS w_RtlNormalizeString(u32 form, const wchar *src, int slen, wchar *dst,
					 int *dlen)
{
	(void)form;
	if (slen < 0)
		slen = (int)rt_wcslen(src) + 1;
	if (!*dlen) {
		*dlen = slen;
		return 0;
	}
	if (*dlen < slen) {
		*dlen = slen;
		return STATUS_BUFFER_TOO_SMALL;
	}
	memcpy(dst, src, (size_t)slen * 2);
	*dlen = slen;
	return 0;
}
EXPORT(RtlNormalizeString);

static API NTSTATUS w_RtlIsNormalizedString(u32 form, const wchar *s, int len, u8 *res)
{
	(void)form;
	(void)s;
	(void)len;
	*res = 1;
	return 0;
}
EXPORT(RtlIsNormalizedString);

/* ---- integers --------------------------------------------------------------------- */

static int digit(u32 c)
{
	if (c >= '0' && c <= '9')
		return (int)(c - '0');
	c |= 0x20;
	return c >= 'a' && c <= 'z' ? (int)(c - 'a' + 10) : 99;
}

static NTSTATUS parse_uint(const wchar *w, const char *a, size_t n, u32 base, u32 *out)
{
	size_t i = 0;
	bool neg = false;
	u32 v = 0;

#define CH(k) (w ? (u32)w[k] : (u32)(u8)a[k])
	while (i < n && (CH(i) == ' ' || CH(i) == '\t'))
		i++;
	if (i < n && (CH(i) == '+' || CH(i) == '-'))
		neg = CH(i++) == '-';
	if (!base) {
		base = 10;
		if (i + 1 < n && CH(i) == '0') {
			u32 c = CH(i + 1) | 0x20;

			if (c == 'x' || c == 'o' || c == 'b') {
				base = c == 'x' ? 16 : c == 'o' ? 8 : 2;
				i += 2;
			}
		}
	} else if (base != 2 && base != 8 && base != 10 && base != 16) {
		return STATUS_INVALID_PARAMETER;
	}
	for (; i < n && digit(CH(i)) < (int)base; i++)
		v = v * base + (u32)digit(CH(i));
#undef CH
	*out = neg ? (u32)-(s64)v : v;
	return 0;
}

static API NTSTATUS w_RtlCharToInteger(const char *s, u32 base, u32 *v)
{
	return parse_uint(NULL, s, rt_strlen(s), base, v);
}
EXPORT(RtlCharToInteger);

static API NTSTATUS w_RtlUnicodeStringToInteger(const UNICODE_STRING *s, u32 base, u32 *v)
{
	return parse_uint(s->buf, NULL, s->len / 2, base, v);
}
EXPORT(RtlUnicodeStringToInteger);

static size_t fmt_int(char *out, u64 v, u32 base)
{
	char tmp[66];
	size_t n = 0, k = 0;

	do
		tmp[n++] = "0123456789ABCDEF"[v % base];
	while ((v /= base));
	while (n)
		out[k++] = tmp[--n];
	out[k] = 0;
	return k;
}

static API NTSTATUS w_RtlIntegerToChar(u32 v, u32 base, int len, char *out)
{
	char tmp[40];
	size_t n = fmt_int(tmp, v, base ? base : 10);

	if (len > 0 && (size_t)len < n)
		return STATUS_BUFFER_OVERFLOW;
	memcpy(out, tmp, n + ((size_t)len > n ? 1 : 0));
	return 0;
}
EXPORT(RtlIntegerToChar);

static API NTSTATUS w_RtlInt64ToUnicodeString(u64 v, u32 base, UNICODE_STRING *s)
{
	char tmp[68];
	size_t n = fmt_int(tmp, v, base ? base : 10);

	if (n * 2 + 2 > s->max)
		return STATUS_BUFFER_OVERFLOW;
	for (size_t i = 0; i <= n; i++)
		s->buf[i] = (wchar)tmp[i];
	s->len = (u16)(n * 2);
	return 0;
}
EXPORT(RtlInt64ToUnicodeString);

static API NTSTATUS w_RtlIntegerToUnicodeString(u32 v, u32 base, UNICODE_STRING *s)
{
	return w_RtlInt64ToUnicodeString(v, base, s);
}
EXPORT(RtlIntegerToUnicodeString);

static API NTSTATUS w_RtlHashUnicodeString(const UNICODE_STRING *s, u8 ci, u32 alg, u32 *h)
{
	u32 v = 0;

	(void)alg;
	for (u32 i = 0; i < s->len / 2u; i++)
		v = v * 65599 + (ci ? nt_upcase(s->buf[i]) : s->buf[i]);
	*h = v;
	return 0;
}
EXPORT(RtlHashUnicodeString);

static API NTSTATUS w_RtlGUIDFromString(const UNICODE_STRING *s, u8 *g)
{
	static const u8 pos[16] = { 4, 3, 2, 1, 6, 5, 9, 8, 11, 12, 14, 15, 16, 17, 18, 19 };
	const wchar *p = s->buf;
	u8 raw[16];
	int k = 0;

	if (s->len < 76 || p[0] != '{')
		return STATUS_INVALID_PARAMETER;
	for (int i = 1; i < 37 && k < 16; i++) {
		if (p[i] == '-')
			continue;
		if (digit(p[i]) > 15 || digit(p[i + 1]) > 15)
			return STATUS_INVALID_PARAMETER;
		raw[k++] = (u8)(digit(p[i]) << 4 | digit(p[i + 1]));
		i++;
	}
	(void)pos;
	/* Data1..3 are little-endian numbers, Data4 is bytes. */
	g[0] = raw[3]; g[1] = raw[2]; g[2] = raw[1]; g[3] = raw[0];
	g[4] = raw[5]; g[5] = raw[4]; g[6] = raw[7]; g[7] = raw[6];
	memcpy(g + 8, raw + 8, 8);
	return 0;
}
EXPORT(RtlGUIDFromString);

/* ---- paths ------------------------------------------------------------------------ */

static int path_type(const wchar *p)
{
	if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/')) {
		if ((p[2] == '.' || p[2] == '?') && (p[3] == '\\' || p[3] == '/'))
			return 6;               /* local device \\.\x */
		if ((p[2] == '.' || p[2] == '?') && !p[3])
			return 7;
		return 1;                       /* UNC */
	}
	if (p[0] == '\\' || p[0] == '/')
		return 4;                       /* rooted */
	if (p[0] && p[1] == ':')
		return p[2] == '\\' || p[2] == '/' ? 2 : 3;
	return 5;                               /* relative */
}

static API u32 w_RtlDetermineDosPathNameType_U(const wchar *p)
{
	return (u32)path_type(p);
}
EXPORT(RtlDetermineDosPathNameType_U);

/* DOS device names (CON, NUL, AUX, PRN, COMn, LPTn) in the last component. */
static API u32 w_RtlIsDosDeviceName_U(const wchar *p)
{
	static const char *const devs[] = { "CON", "NUL", "AUX", "PRN", "CONIN$", "CONOUT$" };
	size_t n = rt_wcslen(p), start = n, end;

	while (start && p[start - 1] != '\\' && p[start - 1] != '/' && p[start - 1] != ':')
		start--;
	end = start;
	while (end < n && p[end] != '.' && p[end] != ':')
		end++;
	while (end > start && p[end - 1] == ' ')
		end--;
	for (size_t i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
		size_t k = rt_strlen(devs[i]);

		if (end - start != k)
			continue;
		for (size_t j = 0; j < k; j++)
			if (nt_upcase(p[start + j]) != (wchar)devs[i][j])
				goto next;
		return (u32)(start * 2) << 16 | (u32)(k * 2);
next:;
	}
	if (end - start == 4 && p[start + 3] >= '1' && p[start + 3] <= '9' &&
	    ((nt_upcase(p[start]) == 'C' && nt_upcase(p[start + 1]) == 'O' &&
	      nt_upcase(p[start + 2]) == 'M') ||
	     (nt_upcase(p[start]) == 'L' && nt_upcase(p[start + 1]) == 'P' &&
	      nt_upcase(p[start + 2]) == 'T')))
		return (u32)(start * 2) << 16 | 8;
	return 0;
}
EXPORT(RtlIsDosDeviceName_U);

/* Collapse separators, "." and ".." in the part after the root (prefix length `root`). */
static size_t normalise(wchar *p, size_t root)
{
	size_t r = root, w = root;
	size_t n = rt_wcslen(p);

	while (r < n) {
		size_t s = r, len;

		while (r < n && p[r] != '\\' && p[r] != '/')
			r++;
		len = r - s;
		if (r < n)
			r++;
		if (!len || (len == 1 && p[s] == '.'))
			continue;
		if (len == 2 && p[s] == '.' && p[s + 1] == '.') {
			if (w > root) {
				w--;
				while (w > root && p[w - 1] != '\\')
					w--;
			}
			continue;
		}
		memmove(p + w, p + s, len * 2);
		w += len;
		p[w++] = '\\';
	}
	if (w > root && (n == 0 || (p[n - 1] != '\\' && p[n - 1] != '/')))
		w--;                            /* no trailing separator unless given */
	/* Trailing dots and spaces of the last component are dropped. */
	while (w > root && (p[w - 1] == '.' || p[w - 1] == ' ') && p[w - 1] != '\\')
		w--;
	p[w] = 0;
	return w;
}

/* Full DOS path of `name` into a heap buffer. */
static wchar *full_path(const wchar *name)
{
	const wchar *cur = nt_curdir();
	size_t n = rt_wcslen(name), cn = rt_wcslen(cur), root;
	wchar *out = nt_alloc((n + cn + 8) * 2);
	int t = path_type(name);

	if (!out)
		return NULL;
	switch (t) {
	case 2:
		memcpy(out, name, (n + 1) * 2);
		out[0] = nt_upcase(out[0]);
		root = 3;
		break;
	case 3:                                 /* "C:x": relative to the current directory */
		memcpy(out, cur, cn * 2);
		memcpy(out + cn, name + 2, (n - 1) * 2);
		root = 3;
		break;
	case 4:
		out[0] = cur[0];
		out[1] = ':';
		memcpy(out + 2, name, (n + 1) * 2);
		root = 3;
		break;
	case 1:
	case 6:
	case 7:
		memcpy(out, name, (n + 1) * 2);
		root = t == 1 ? 2 : 4;
		if (root > n)
			root = n;
		break;
	default:
		memcpy(out, cur, cn * 2);
		memcpy(out + cn, name, (n + 1) * 2);
		root = 3;
		break;
	}
	for (size_t i = 0; out[i]; i++)
		if (out[i] == '/')
			out[i] = '\\';
	normalise(out, root);
	if (t != 1 && out[0] && out[1] == ':' && !out[2]) {
		out[2] = '\\';
		out[3] = 0;
	}
	return out;
}

static API u32 w_RtlGetFullPathName_U(const wchar *name, u32 size, wchar *buf, wchar **file)
{
	wchar *full;
	u32 dev = w_RtlIsDosDeviceName_U(name), n;

	if (file)
		*file = NULL;
	if (!name || !*name)
		return 0;
	if (dev) {                              /* \\.\CON */
		static const wchar pre[] = L"\\\\.\\";

		n = 4 + (dev & 0xFFFF) / 2;
		if (size < (n + 1) * 2)
			return (n + 1) * 2;
		memcpy(buf, pre, 8);
		memcpy(buf + 4, name + (dev >> 17), dev & 0xFFFF);
		buf[n] = 0;
		return n * 2;
	}
	full = full_path(name);
	if (!full)
		return 0;
	n = (u32)rt_wcslen(full);
	if (size < (n + 1) * 2) {
		nt_free(full);
		return (n + 1) * 2;
	}
	memcpy(buf, full, (n + 1) * 2);
	nt_free(full);
	if (file && n && buf[n - 1] != '\\') {
		u32 k = n;

		while (k && buf[k - 1] != '\\')
			k--;
		*file = buf + k;
	}
	return n * 2;
}
EXPORT(RtlGetFullPathName_U);

static API NTSTATUS w_RtlGetFullPathName_UEx(const wchar *name, u32 size, wchar *buf, wchar **file,
					     u32 *len)
{
	u32 n = w_RtlGetFullPathName_U(name, size, buf, file);

	if (len)
		*len = n;
	if (!n)
		return STATUS_OBJECT_NAME_INVALID;
	return n >= size ? STATUS_BUFFER_TOO_SMALL : 0;
}
EXPORT(RtlGetFullPathName_UEx);

static API NTSTATUS w_RtlDosPathNameToNtPathName_U_WithStatus(const wchar *dos,
							      UNICODE_STRING *nt, wchar **file,
							      void *rel)
{
	static const wchar pre[] = L"\\??\\";
	wchar *full;
	size_t n;
	u32 dev;

	if (rel)
		memset(rel, 0, 24);
	if (!dos || !*dos)
		return STATUS_OBJECT_NAME_INVALID;
	if (dos[0] == '\\' && dos[1] == '\\' && dos[2] == '?' && dos[3] == '\\') {
		n = rt_wcslen(dos);
		nt->buf = nt_alloc((n + 1) * 2);
		if (!nt->buf)
			return STATUS_NO_MEMORY;
		memcpy(nt->buf, dos, (n + 1) * 2);
		nt->buf[1] = '?';
		nt->buf[2] = '?';
		nt->len = (u16)(n * 2);
		nt->max = (u16)(n * 2 + 2);
		if (file)
			*file = NULL;
		return 0;
	}
	dev = w_RtlIsDosDeviceName_U(dos);
	if (dev) {
		n = (dev & 0xFFFF) / 2;
		full = nt_alloc((n + 1) * 2);
		if (!full)
			return STATUS_NO_MEMORY;
		memcpy(full, dos + (dev >> 17), n * 2);
		full[n] = 0;
	} else {
		full = full_path(dos);
		if (!full)
			return STATUS_NO_MEMORY;
	}
	n = rt_wcslen(full);
	if (full[0] == '\\' && full[1] == '\\') {       /* UNC or \\.\: \??\UNC\x or \??\x */
		bool unc = path_type(full) == 1;
		size_t skip = 2, extra = unc ? 4 : 0;

		if (!unc && n >= 4)
			skip = 4;
		nt->buf = nt_alloc((4 + extra + n - skip + 1) * 2);
		if (!nt->buf) {
			nt_free(full);
			return STATUS_NO_MEMORY;
		}
		memcpy(nt->buf, pre, 8);
		if (unc)
			memcpy(nt->buf + 4, L"UNC\\", 8);
		memcpy(nt->buf + 4 + extra, full + skip, (n - skip + 1) * 2);
		n = 4 + extra + n - skip;
	} else {
		nt->buf = nt_alloc((n + 5) * 2);
		if (!nt->buf) {
			nt_free(full);
			return STATUS_NO_MEMORY;
		}
		memcpy(nt->buf, pre, 8);
		memcpy(nt->buf + 4, full, (n + 1) * 2);
		n += 4;
	}
	nt_free(full);
	nt->len = (u16)(n * 2);
	nt->max = (u16)(n * 2 + 2);
	if (file) {
		size_t k = n;

		while (k && nt->buf[k - 1] != '\\')
			k--;
		*file = k < n ? nt->buf + k : NULL;
	}
	return 0;
}
EXPORT(RtlDosPathNameToNtPathName_U_WithStatus);

static API u8 w_RtlDosPathNameToNtPathName_U(const wchar *dos, UNICODE_STRING *nt, wchar **file,
					     void *rel)
{
	return !w_RtlDosPathNameToNtPathName_U_WithStatus(dos, nt, file, rel);
}
EXPORT(RtlDosPathNameToNtPathName_U);

static API NTSTATUS w_RtlDosPathNameToRelativeNtPathName_U_WithStatus(const wchar *dos,
								      UNICODE_STRING *nt,
								      wchar **file, void *rel)
{
	return w_RtlDosPathNameToNtPathName_U_WithStatus(dos, nt, file, rel);
}
EXPORT(RtlDosPathNameToRelativeNtPathName_U_WithStatus);

static API void w_RtlReleaseRelativeName(void *rel)
{
	(void)rel;
}
EXPORT(RtlReleaseRelativeName);

/* NT (or DOS) path -> absolute VFS path in UTF-8. Every drive is the VFS root. */
NTSTATUS nt_unix_path(const UNICODE_STRING *name, u64 root, char *out, size_t cap)
{
	size_t n = name->len / 2, i = 0, o = 0;
	const wchar *p = name->buf;
	wchar *tmp;
	char *u8p;

	if (root) {
		struct nt_obj *r = nt_handle_get(root);

		if (!r || !r->path)
			return STATUS_INVALID_HANDLE;
		o = nt_strlcpy(out, r->path, cap);
		if (o + 2 >= cap)
			return STATUS_NAME_TOO_LONG;
		if (o > 1)
			out[o++] = '/';
	} else {
		if (n >= 4 && p[0] == '\\' && (p[1] == '?' || p[1] == '\\') && p[2] == '?' &&
		    p[3] == '\\')
			i = 4;                  /* \??\ or \\?\ */
		else if (n >= 12 && !nt_wcsnicmp(p, L"\\DosDevices\\", 12))
			i = 12;
		if (n - i >= 4 && !nt_wcsnicmp(p + i, L"UNC\\", 4))
			return STATUS_OBJECT_PATH_NOT_FOUND;
		if (n - i >= 2 && p[i + 1] == ':')
			i += 2;                 /* drive letter */
		else if (!i && n && p[0] != '\\' && p[0] != '/') {
			/* relative DOS name: against the current directory */
			const wchar *cur = nt_curdir();

			o = nt_wtou(out, cap, cur + 2, rt_wcslen(cur) - 2);
		}
		if (n - i && !o && p[i] != '\\' && p[i] != '/')
			out[o++] = '/';
	}
	tmp = nt_alloc((n - i + 1) * 2);
	u8p = nt_alloc((n - i) * 3 + 2);
	if (!tmp || !u8p) {
		nt_free(tmp);
		nt_free(u8p);
		return STATUS_NO_MEMORY;
	}
	memcpy(tmp, p + i, (n - i) * 2);
	tmp[n - i] = 0;
	{
		size_t k = nt_wtou(u8p, (n - i) * 3 + 1, tmp, n - i);

		u8p[k] = 0;
		for (size_t j = 0; j < k; j++)
			if (u8p[j] == '\\')
				u8p[j] = '/';
		for (size_t j = 0; j < k && o + 1 < cap; j++)
			out[o++] = u8p[j];
	}
	nt_free(tmp);
	nt_free(u8p);
	if (o + 1 >= cap)
		return STATUS_NAME_TOO_LONG;
	out[o] = 0;
	/* Canonicalise: no "//", ".", ".." or trailing slash. */
	{
		char *w = out, *r = out;

		if (*r != '/')
			return STATUS_OBJECT_PATH_NOT_FOUND;
		while (*r) {
			char *s;
			size_t len;

			while (*r == '/')
				r++;
			s = r;
			while (*r && *r != '/')
				r++;
			len = (size_t)(r - s);
			if (!len || (len == 1 && s[0] == '.'))
				continue;
			if (len == 2 && s[0] == '.' && s[1] == '.') {
				while (w > out && *--w != '/')
					;
				continue;
			}
			*w++ = '/';
			memmove(w, s, len);
			w += len;
		}
		if (w == out)
			*w++ = '/';
		*w = 0;
	}
	return 0;
}

static API u32 w_RtlGetCurrentDirectory_U(u32 size, wchar *buf)
{
	const wchar *c = nt_curdir();
	u32 n = (u32)rt_wcslen(c);

	if (n > 3 && c[n - 1] == '\\')
		n--;
	if (size < (n + 1) * 2)
		return (n + 1) * 2;
	memcpy(buf, c, n * 2);
	buf[n] = 0;
	return n * 2;
}
EXPORT(RtlGetCurrentDirectory_U);

static API NTSTATUS w_RtlSetCurrentDirectory_U(const UNICODE_STRING *dir)
{
	wchar *tmp = nt_alloc(dir->len + 2), *full;
	char path[512];
	u64 st[2];
	UNICODE_STRING u;

	if (!tmp)
		return STATUS_NO_MEMORY;
	memcpy(tmp, dir->buf, dir->len);
	full = full_path(tmp);
	nt_free(tmp);
	if (!full)
		return STATUS_NO_MEMORY;
	nt_init_ustr(&u, full);
	if (nt_unix_path(&u, 0, path, sizeof(path)) || mos_stat(path, st) < 0) {
		nt_free(full);
		return STATUS_OBJECT_PATH_NOT_FOUND;
	}
	if (st[1] != MOS_ST_DIR) {
		nt_free(full);
		return STATUS_NOT_A_DIRECTORY;
	}
	nt_set_curdir(full);
	nt_free(full);
	return 0;
}
EXPORT(RtlSetCurrentDirectory_U);

static API u8 w_RtlDoesFileExists_U(const wchar *name)
{
	wchar *full = full_path(name);
	char path[512];
	UNICODE_STRING u;
	u64 st[2];
	bool ok;

	if (!full)
		return 0;
	nt_init_ustr(&u, full);
	ok = !nt_unix_path(&u, 0, path, sizeof(path)) && mos_stat(path, st) >= 0;
	nt_free(full);
	return ok;
}
EXPORT(RtlDoesFileExists_U);

static API u32 w_RtlDosSearchPath_U(const wchar *paths, const wchar *name, const wchar *ext,
				    u32 size, wchar *buf, wchar **file)
{
	size_t nn = rt_wcslen(name), en = ext ? rt_wcslen(ext) : 0;
	bool has_ext = false;

	for (size_t i = 0; i < nn; i++)
		if (name[i] == '.')
			has_ext = true;
		else if (name[i] == '\\' || name[i] == '/')
			has_ext = false;
	if (path_type(name) != 5) {
		if (!w_RtlDoesFileExists_U(name))
			return 0;
		return w_RtlGetFullPathName_U(name, size, buf, file);
	}
	while (paths && *paths) {
		size_t pn = 0;
		wchar *cand;

		while (paths[pn] && paths[pn] != ';')
			pn++;
		cand = nt_alloc((pn + nn + en + 3) * 2);
		if (cand) {
			size_t k = pn;

			memcpy(cand, paths, pn * 2);
			if (k && cand[k - 1] != '\\')
				cand[k++] = '\\';
			memcpy(cand + k, name, nn * 2);
			k += nn;
			if (!has_ext && en) {
				memcpy(cand + k, ext, en * 2);
				k += en;
			}
			cand[k] = 0;
			if (w_RtlDoesFileExists_U(cand)) {
				u32 r = w_RtlGetFullPathName_U(cand, size, buf, file);

				nt_free(cand);
				return r;
			}
			nt_free(cand);
		}
		paths += pn;
		if (*paths)
			paths++;
	}
	return 0;
}
EXPORT(RtlDosSearchPath_U);

static API NTSTATUS w_RtlGetSearchPath(wchar **path)
{
	static const wchar def[] = L"C:\\windows\\system32;C:\\windows;.";
	wchar *p = nt_alloc(sizeof(def));

	if (!p)
		return STATUS_NO_MEMORY;
	memcpy(p, def, sizeof(def));
	*path = p;
	return 0;
}
EXPORT(RtlGetSearchPath);

static API NTSTATUS w_RtlGetExePath(const wchar *name, wchar **path)
{
	(void)name;
	return w_RtlGetSearchPath(path);
}
EXPORT(RtlGetExePath);

static API void w_RtlReleasePath(wchar *p)
{
	nt_free(p);
}
EXPORT(RtlReleasePath);

static API NTSTATUS w_RtlSetSearchPathMode(u32 flags)
{
	(void)flags;
	return 0;
}
EXPORT(RtlSetSearchPathMode);

static API u8 w_RtlIsNameLegalDOS8Dot3(const UNICODE_STRING *s, void *oem, u8 *spaces)
{
	u32 n = s->len / 2, dot = n;

	(void)oem;
	if (spaces)
		*spaces = 0;
	for (u32 i = 0; i < n; i++)
		if (s->buf[i] == '.') {
			if (dot != n)
				return 0;
			dot = i;
		}
	return n && dot <= 8 && (n - dot <= 4);
}
EXPORT(RtlIsNameLegalDOS8Dot3);

static API NTSTATUS w_RtlWow64EnableFsRedirectionEx(u64 disable, u64 *old)
{
	(void)disable;
	if (old)
		*old = 0;
	return 0;
}
EXPORT(RtlWow64EnableFsRedirectionEx);

static API NTSTATUS w_RtlWow64EnableFsRedirection(u8 enable)
{
	(void)enable;
	return 0;
}
EXPORT(RtlWow64EnableFsRedirection);

static API NTSTATUS w_RtlWow64GetProcessMachines(u64 p, u16 *cur, u16 *native)
{
	(void)p;
	if (cur)
		*cur = 0;                       /* IMAGE_FILE_MACHINE_UNKNOWN: not WoW64 */
	if (native)
		*native = 0x8664;
	return 0;
}
EXPORT(RtlWow64GetProcessMachines);

/* ---- environment ------------------------------------------------------------------ */

static wchar **cur_env(void)
{
	return (wchar **)(nt_params() + 0x80);
}

static size_t env_len(const wchar *e)
{
	size_t n = 0;

	while (e[n] || e[n + 1])
		n++;
	return n + 2;
}

/* Value of `name` (n characters) in block `e`, or NULL. */
static const wchar *env_find(const wchar *e, const wchar *name, size_t n)
{
	while (e && *e) {
		size_t l = rt_wcslen(e);

		if (l > n && e[n] == '=' && !nt_wcsnicmp(e, name, n) && e[0] != '=')
			return e + n + 1;
		if (l > n && e[n] == '=' && e[0] == '=' && !nt_wcsnicmp(e, name, n))
			return e + n + 1;
		e += l + 1;
	}
	return NULL;
}

static API NTSTATUS w_RtlCreateEnvironment(u8 inherit, wchar **env)
{
	const wchar *src = *cur_env();
	size_t n = inherit && src ? env_len(src) : 2;
	wchar *e = nt_alloc(n * 2);

	if (!e)
		return STATUS_NO_MEMORY;
	if (inherit && src)
		memcpy(e, src, n * 2);
	*env = e;
	return 0;
}
EXPORT(RtlCreateEnvironment);

static API NTSTATUS w_RtlDestroyEnvironment(wchar *env)
{
	nt_free(env);
	return 0;
}
EXPORT(RtlDestroyEnvironment);

static API NTSTATUS w_RtlQueryEnvironmentVariable(const wchar *env, const wchar *name, size_t nlen,
						  wchar *val, size_t vlen, size_t *ret)
{
	const wchar *v = env_find(env ? env : *cur_env(), name, nlen);
	size_t n;

	if (!v)
		return STATUS_VARIABLE_NOT_FOUND;
	n = rt_wcslen(v);
	*ret = n;
	if (n >= vlen) {
		*ret = n + 1;
		return STATUS_BUFFER_TOO_SMALL;
	}
	memcpy(val, v, (n + 1) * 2);
	return 0;
}
EXPORT(RtlQueryEnvironmentVariable);

static API NTSTATUS w_RtlQueryEnvironmentVariable_U(const wchar *env, const UNICODE_STRING *name,
						    UNICODE_STRING *val)
{
	const wchar *v = env_find(env ? env : *cur_env(), name->buf, name->len / 2);
	size_t n;

	if (!v)
		return STATUS_VARIABLE_NOT_FOUND;
	n = rt_wcslen(v) * 2;
	val->len = (u16)n;
	if (n + 2 > val->max)
		return STATUS_BUFFER_TOO_SMALL;
	memcpy(val->buf, v, n + 2);
	return 0;
}
EXPORT(RtlQueryEnvironmentVariable_U);

static API NTSTATUS w_RtlSetEnvironmentVariable(wchar **penv, const UNICODE_STRING *name,
						const UNICODE_STRING *val)
{
	wchar **where = penv ? penv : cur_env();
	const wchar *e = *where;
	size_t total = e ? env_len(e) : 2, nn = name->len / 2;
	wchar *ne = nt_alloc((total + nn + (val ? val->len / 2 : 0) + 4) * 2), *w = ne;
	bool done = false;

	if (!ne)
		return STATUS_NO_MEMORY;
	while (e && *e) {
		size_t l = rt_wcslen(e);
		bool same = l > nn && e[nn] == '=' && !nt_wcsnicmp(e, name->buf, nn);
		int cmp = nt_wcsnicmp(e, name->buf, nn);

		if (!done && !same && cmp > 0 && e[0] != '=' && val) {
			memcpy(w, name->buf, nn * 2);
			w += nn;
			*w++ = '=';
			memcpy(w, val->buf, val->len);
			w += val->len / 2;
			*w++ = 0;
			done = true;
		}
		if (same) {
			if (val && !done) {
				memcpy(w, name->buf, nn * 2);
				w += nn;
				*w++ = '=';
				memcpy(w, val->buf, val->len);
				w += val->len / 2;
				*w++ = 0;
			}
			done = true;
		} else {
			memcpy(w, e, (l + 1) * 2);
			w += l + 1;
		}
		e += l + 1;
	}
	if (!done && val) {
		memcpy(w, name->buf, nn * 2);
		w += nn;
		*w++ = '=';
		memcpy(w, val->buf, val->len);
		w += val->len / 2;
		*w++ = 0;
	}
	*w++ = 0;
	if (w - ne == 1)
		*w++ = 0;
	/* The old block may still be referenced (GetEnvironmentStrings): keep it. */
	*where = ne;
	if (!penv)
		*(u64 *)(nt_params() + 0x3F0) = (u64)(w - ne) * 2;
	return 0;
}
EXPORT(RtlSetEnvironmentVariable);

static API NTSTATUS w_RtlSetCurrentEnvironment(wchar *env, wchar **old)
{
	if (old)
		*old = *cur_env();
	*cur_env() = env;
	*(u64 *)(nt_params() + 0x3F0) = env_len(env) * 2;
	return 0;
}
EXPORT(RtlSetCurrentEnvironment);

static API NTSTATUS w_RtlExpandEnvironmentStrings(const wchar *env, const wchar *src, size_t slen,
						  wchar *dst, size_t dlen, size_t *ret)
{
	size_t o = 0;

	for (size_t i = 0; i < slen && src[i]; ) {
		const wchar *v = NULL;
		size_t end = i + 1;

		if (src[i] == '%') {
			while (end < slen && src[end] && src[end] != '%')
				end++;
			if (end < slen && src[end] == '%')
				v = env_find(env ? env : *cur_env(), src + i + 1, end - i - 1);
		}
		if (v) {
			for (; *v; v++, o++)
				if (o < dlen)
					dst[o] = *v;
			i = end + 1;
		} else {
			if (o < dlen)
				dst[o] = src[i];
			o++;
			i++;
		}
	}
	if (o < dlen)
		dst[o] = 0;
	o++;
	if (ret)
		*ret = o;
	return o > dlen ? STATUS_BUFFER_TOO_SMALL : 0;
}
EXPORT(RtlExpandEnvironmentStrings);

static API NTSTATUS w_RtlExpandEnvironmentStrings_U(const wchar *env, const UNICODE_STRING *src,
						    UNICODE_STRING *dst, u32 *ret)
{
	size_t n;
	NTSTATUS s = w_RtlExpandEnvironmentStrings(env, src->buf, src->len / 2, dst->buf,
						   dst->max / 2, &n);

	if (ret)
		*ret = (u32)n * 2;
	if (!s)
		dst->len = (u16)((n - 1) * 2);
	return s;
}
EXPORT(RtlExpandEnvironmentStrings_U);

/* ---- heaps: one allocator behind every handle ------------------------------------- */

static u8 heap_dummy[64];

static API void *w_RtlCreateHeap(u32 flags, void *base, size_t res, size_t commit, void *lock,
				 void *params)
{
	(void)flags;
	(void)base;
	(void)res;
	(void)commit;
	(void)lock;
	(void)params;
	return nt_alloc(64);
}
EXPORT(RtlCreateHeap);

static API void *w_RtlDestroyHeap(void *heap)
{
	(void)heap;
	return NULL;
}
EXPORT(RtlDestroyHeap);

static API void *w_RtlAllocateHeap(void *heap, u32 flags, size_t n)
{
	(void)heap;
	(void)flags;
	return nt_alloc(n);             /* always zeroed */
}
EXPORT(RtlAllocateHeap);

static API u8 w_RtlFreeHeap(void *heap, u32 flags, void *p)
{
	(void)heap;
	(void)flags;
	nt_free(p);
	return 1;
}
EXPORT(RtlFreeHeap);

static API void *w_RtlReAllocateHeap(void *heap, u32 flags, void *p, size_t n)
{
	size_t old = rt_size(p);
	void *q;

	(void)heap;
	if (!p)
		return NULL;
	if (n <= old)
		return p;
	if (flags & 0x10)               /* HEAP_REALLOC_IN_PLACE_ONLY */
		return NULL;
	q = nt_realloc_(p, n);
	if (q && (flags & 0x8))
		memset((u8 *)q + old, 0, rt_size(q) - old);
	return q;
}
EXPORT(RtlReAllocateHeap);

static API size_t w_RtlSizeHeap(void *heap, u32 flags, void *p)
{
	(void)heap;
	(void)flags;
	return p ? rt_size(p) : (size_t)-1;
}
EXPORT(RtlSizeHeap);

static API u8 w_RtlTrue(void)
{
	return 1;
}
EXPORT_AS("RtlLockHeap", w_RtlTrue);
EXPORT_AS("RtlUnlockHeap", w_RtlTrue);
EXPORT_AS("RtlValidateHeap", w_RtlTrue);

static API size_t w_RtlCompactHeap(void *heap, u32 flags)
{
	(void)heap;
	(void)flags;
	return 0x10000;
}
EXPORT(RtlCompactHeap);

static API u32 w_RtlGetProcessHeaps(u32 n, void **heaps)
{
	if (n)
		heaps[0] = *(void **)(nt_peb + 0x30);
	return 1;
}
EXPORT(RtlGetProcessHeaps);

static API NTSTATUS w_RtlWalkHeap(void *heap, void *entry)
{
	(void)heap;
	(void)entry;
	return STATUS_NO_MORE_ENTRIES;
}
EXPORT(RtlWalkHeap);

static API NTSTATUS w_RtlSetHeapInformation(void *heap, u32 cls, void *info, size_t len)
{
	(void)heap;
	(void)cls;
	(void)info;
	(void)len;
	return 0;
}
EXPORT(RtlSetHeapInformation);

static API NTSTATUS w_RtlQueryHeapInformation(void *heap, u32 cls, void *info, size_t len,
					      size_t *ret)
{
	(void)heap;
	if (cls != 0 || len < 4) {
		if (ret)
			*ret = 4;
		return cls ? STATUS_INVALID_INFO_CLASS : STATUS_BUFFER_TOO_SMALL;
	}
	*(u32 *)info = 2;               /* low-fragmentation heap */
	if (ret)
		*ret = 4;
	return 0;
}
EXPORT(RtlQueryHeapInformation);

static API u8 w_RtlGetUserInfoHeap(void *heap, u32 flags, void *p, void **val, u32 *uflags)
{
	(void)heap;
	(void)flags;
	(void)p;
	if (val)
		*val = NULL;
	if (uflags)
		*uflags = 0;
	return 1;
}
EXPORT(RtlGetUserInfoHeap);

static API u8 w_RtlSetUserValueHeap(void *heap, u32 flags, void *p, void *val)
{
	(void)heap;
	(void)flags;
	(void)p;
	(void)val;
	return 1;
}
EXPORT(RtlSetUserValueHeap);

void *rtl_process_heap(void)
{
	return heap_dummy;
}

/* ---- bitmaps (RTL_BITMAP: size in bits, buffer of ULONGs) ------------------------- */

struct bitmap {
	u32 size;
	u32 *buf;
};

static bool bit(const struct bitmap *b, u32 i)
{
	return b->buf[i / 32] >> (i % 32) & 1;
}

static API void w_RtlInitializeBitMap(struct bitmap *b, u32 *buf, u32 size)
{
	b->size = size;
	b->buf = buf;
}
EXPORT(RtlInitializeBitMap);

static API void w_RtlSetBits(struct bitmap *b, u32 start, u32 n)
{
	for (u32 i = start; i < start + n && i < b->size; i++)
		b->buf[i / 32] |= 1u << (i % 32);
}
EXPORT(RtlSetBits);

static API void w_RtlClearBits(struct bitmap *b, u32 start, u32 n)
{
	for (u32 i = start; i < start + n && i < b->size; i++)
		b->buf[i / 32] &= ~(1u << (i % 32));
}
EXPORT(RtlClearBits);

static API u8 w_RtlAreBitsSet(const struct bitmap *b, u32 start, u32 n)
{
	if (start + n > b->size || start + n < start)
		return 0;
	for (u32 i = start; i < start + n; i++)
		if (!bit(b, i))
			return 0;
	return 1;
}
EXPORT(RtlAreBitsSet);

static API u8 w_RtlAreBitsClear(const struct bitmap *b, u32 start, u32 n)
{
	if (start + n > b->size || start + n < start)
		return 0;
	for (u32 i = start; i < start + n; i++)
		if (bit(b, i))
			return 0;
	return 1;
}
EXPORT(RtlAreBitsClear);

static API u32 w_RtlFindClearBits(const struct bitmap *b, u32 n, u32 hint)
{
	if (!n || n > b->size)
		return ~0u;
	if (hint >= b->size)
		hint = 0;
	for (u32 k = 0; k < b->size; k++) {
		u32 i = (hint + k) % b->size;

		if (i + n <= b->size && w_RtlAreBitsClear(b, i, n))
			return i;
	}
	return ~0u;
}
EXPORT(RtlFindClearBits);

static API u32 w_RtlFindClearBitsAndSet(struct bitmap *b, u32 n, u32 hint)
{
	u32 i = w_RtlFindClearBits(b, n, hint);

	if (i != ~0u)
		w_RtlSetBits(b, i, n);
	return i;
}
EXPORT(RtlFindClearBitsAndSet);

static API u32 w_RtlFindSetBits(const struct bitmap *b, u32 n, u32 hint)
{
	if (!n || n > b->size)
		return ~0u;
	for (u32 k = 0; k < b->size; k++) {
		u32 i = (hint + k) % b->size;

		if (i + n <= b->size && w_RtlAreBitsSet(b, i, n))
			return i;
	}
	return ~0u;
}
EXPORT(RtlFindSetBits);

static API u32 w_RtlNumberOfSetBits(const struct bitmap *b)
{
	u32 c = 0;

	for (u32 i = 0; i < b->size; i++)
		c += bit(b, i);
	return c;
}
EXPORT(RtlNumberOfSetBits);

/* ---- time ------------------------------------------------------------------------- */

/* TIME_FIELDS: Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday (CSHORT). */
static API void w_RtlTimeToTimeFields(const s64 *t, s16 *f)
{
	s64 ms = *t / 10000, days = ms / 86400000, rem = ms % 86400000;
	s64 y, doy, mp, d;

	f[3] = (s16)(rem / 3600000);
	f[4] = (s16)(rem / 60000 % 60);
	f[5] = (s16)(rem / 1000 % 60);
	f[6] = (s16)(rem % 1000);
	f[7] = (s16)((days + 1) % 7);           /* 1601-01-01 was a Monday */
	/* Civil date from days since 1601-01-01 (proleptic Gregorian). */
	d = days + 584388 - 60;                 /* days since 0000-03-01 */
	y = (d * 400 + 399) / 146097;
	doy = d - (365 * y + y / 4 - y / 100 + y / 400);
	if (doy < 0) {
		y--;
		doy = d - (365 * y + y / 4 - y / 100 + y / 400);
	}
	mp = (5 * doy + 2) / 153;
	f[2] = (s16)(doy - (153 * mp + 2) / 5 + 1);
	f[1] = (s16)(mp < 10 ? mp + 3 : mp - 9);
	f[0] = (s16)(y + (f[1] <= 2));
}
EXPORT(RtlTimeToTimeFields);

static API u8 w_RtlTimeFieldsToTime(const s16 *f, s64 *t)
{
	s64 y = f[0], m = f[1], d = f[2], days;

	if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1601)
		return 0;
	if (m <= 2) {
		y--;
		m += 12;
	}
	days = 365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d - 1;
	days -= 584388 - 60;
	*t = ((days * 86400 + f[3] * 3600 + f[4] * 60 + f[5]) * 1000 + f[6]) * 10000;
	return 1;
}
EXPORT(RtlTimeFieldsToTime);

static API u8 w_RtlTimeToSecondsSince1970(const s64 *t, u32 *s)
{
	s64 v = *t / 10000000 - 11644473600ll;

	if (v < 0 || v > 0xFFFFFFFFll)
		return 0;
	*s = (u32)v;
	return 1;
}
EXPORT(RtlTimeToSecondsSince1970);

static API void w_RtlSecondsSince1970ToTime(u32 s, s64 *t)
{
	*t = ((s64)s + 11644473600ll) * 10000000;
}
EXPORT(RtlSecondsSince1970ToTime);

static API NTSTATUS w_RtlSystemTimeToLocalTime(const s64 *sys, s64 *local)
{
	*local = *sys;                          /* the machine runs on UTC */
	return 0;
}
EXPORT(RtlSystemTimeToLocalTime);
EXPORT_AS("RtlLocalTimeToSystemTime", w_RtlSystemTimeToLocalTime);

static API NTSTATUS w_RtlQueryTimeZoneInformation(u8 *tzi)
{
	static const wchar utc[] = L"Coordinated Universal Time";

	memset(tzi, 0, 172);
	memcpy(tzi + 4, utc, sizeof(utc));
	memcpy(tzi + 88, utc, sizeof(utc));
	return 0;
}
EXPORT(RtlQueryTimeZoneInformation);

static API NTSTATUS w_RtlQueryDynamicTimeZoneInformation(u8 *tzi)
{
	static const wchar key[] = L"UTC";

	memset(tzi, 0, 432);
	w_RtlQueryTimeZoneInformation(tzi);
	memcpy(tzi + 172, key, sizeof(key));
	return 0;
}
EXPORT(RtlQueryDynamicTimeZoneInformation);

static API NTSTATUS w_RtlSetTimeZoneInformation(const void *tzi)
{
	(void)tzi;
	return 0;
}
EXPORT(RtlSetTimeZoneInformation);

/* ---- errors ----------------------------------------------------------------------- */

static API u32 w_RtlNtStatusToDosError(NTSTATUS s)
{
	static const struct {
		u32 st, err;
	} map[] = {
		{ 0, 0 }, { STATUS_TIMEOUT, 1460 }, { STATUS_PENDING, 997 },
		{ STATUS_BUFFER_OVERFLOW, 234 }, { STATUS_NO_MORE_FILES, 18 },
		{ STATUS_NO_MORE_ENTRIES, 259 }, { STATUS_UNSUCCESSFUL, 31 },
		{ STATUS_NOT_IMPLEMENTED, 1 }, { STATUS_INVALID_INFO_CLASS, 87 },
		{ STATUS_INFO_LENGTH_MISMATCH, 24 }, { STATUS_ACCESS_VIOLATION, 998 },
		{ STATUS_INVALID_HANDLE, 6 }, { STATUS_INVALID_PARAMETER, 87 },
		{ STATUS_NO_SUCH_FILE, 2 }, { STATUS_INVALID_DEVICE_REQUEST, 1 },
		{ STATUS_END_OF_FILE, 38 }, { STATUS_NO_MEMORY, 8 }, { STATUS_ACCESS_DENIED, 5 },
		{ STATUS_BUFFER_TOO_SMALL, 122 }, { STATUS_OBJECT_TYPE_MISMATCH, 6 },
		{ STATUS_OBJECT_NAME_INVALID, 123 }, { STATUS_OBJECT_NAME_NOT_FOUND, 2 },
		{ STATUS_OBJECT_NAME_COLLISION, 183 }, { STATUS_OBJECT_PATH_NOT_FOUND, 3 },
		{ STATUS_MUTANT_NOT_OWNED, 288 }, { STATUS_SEMAPHORE_LIMIT_EXCEEDED, 298 },
		{ STATUS_INVALID_IMAGE_FORMAT, 193 }, { STATUS_NO_TOKEN, 1008 },
		{ STATUS_FILE_IS_A_DIRECTORY, 5 }, { STATUS_NOT_SUPPORTED, 50 },
		{ STATUS_DIRECTORY_NOT_EMPTY, 145 }, { STATUS_NOT_A_DIRECTORY, 267 },
		{ STATUS_DLL_NOT_FOUND, 126 }, { STATUS_ENTRYPOINT_NOT_FOUND, 127 },
		{ STATUS_DLL_INIT_FAILED, 1114 }, { STATUS_KEY_DELETED, 1018 },
		{ STATUS_NOT_FOUND, 1168 }, { STATUS_VARIABLE_NOT_FOUND, 203 },
		{ STATUS_RESOURCE_DATA_NOT_FOUND, 1812 }, { STATUS_RESOURCE_TYPE_NOT_FOUND, 1813 },
		{ STATUS_RESOURCE_NAME_NOT_FOUND, 1814 }, { STATUS_RESOURCE_LANG_NOT_FOUND, 1815 },
		{ 0xC000000Bu, 87 }, { 0xC0000043u, 32 }, { 0xC0000054u, 33 },
		{ 0xC0000101u, 145 }, { 0xC000014Bu, 109 }, { 0xC0000120u, 995 },
		{ 0xC0000034u, 2 }, { STATUS_SXS_KEY_NOT_FOUND, 14007 }, { 0xC0000353u, 87 },
		{ STATUS_NAME_TOO_LONG, 206 }, { 0xC00000E3u, 1 }, { 0x80000014u, 1460 },
		{ 0xC0000008u, 6 }, { 0xC0000024u, 6 }, { 0xC000000Fu, 2 },
	};

	if ((s & 0xFFFF0000u) == 0x80070000u || (s & 0xFFFF0000u) == 0xC0070000u)
		return s & 0xFFFF;
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (map[i].st == s)
			return map[i].err;
	return 317;                             /* ERROR_MR_MID_NOT_FOUND */
}
EXPORT(RtlNtStatusToDosError);

static API u32 w_RtlNtStatusToDosErrorNoTeb(NTSTATUS s)
{
	return w_RtlNtStatusToDosError(s);
}
EXPORT(RtlNtStatusToDosErrorNoTeb);

static API void w_RtlSetLastWin32Error(u32 e)
{
	*(u32 *)(nt_teb() + 0x68) = e;
}
EXPORT(RtlSetLastWin32Error);
EXPORT_AS("RtlRestoreLastWin32Error", w_RtlSetLastWin32Error);

static API u32 w_RtlGetLastWin32Error(void)
{
	return *(u32 *)(nt_teb() + 0x68);
}
EXPORT(RtlGetLastWin32Error);

static API NTSTATUS w_RtlGetLastNtStatus(void)
{
	return *(u32 *)(nt_teb() + 0x1250);
}
EXPORT(RtlGetLastNtStatus);

static API void w_RtlSetLastWin32ErrorAndNtStatusFromNtStatus(NTSTATUS s)
{
	*(u32 *)(nt_teb() + 0x1250) = s;
	w_RtlSetLastWin32Error(w_RtlNtStatusToDosError(s));
}
EXPORT(RtlSetLastWin32ErrorAndNtStatusFromNtStatus);

/* ---- process-wide odds and ends ---------------------------------------------------- */

static API void *w_RtlGetCurrentPeb(void)
{
	return nt_peb;
}
EXPORT(RtlGetCurrentPeb);

static API void *w_NtCurrentTeb(void)
{
	return nt_teb();
}
EXPORT(NtCurrentTeb);

static volatile u32 peb_lock;

static API void w_RtlAcquirePebLock(void)
{
	nt_lock(&peb_lock);
}
EXPORT(RtlAcquirePebLock);

static API void w_RtlReleasePebLock(void)
{
	nt_unlock(&peb_lock);
}
EXPORT(RtlReleasePebLock);

static API u32 w_RtlGetNtGlobalFlags(void)
{
	return 0;
}
EXPORT(RtlGetNtGlobalFlags);

static API u32 w_RtlUniform(u32 *seed)
{
	*seed = (*seed * 0x7FFFFFED + 0x7FFFFFC3) % 0x7FFFFFFF;
	return *seed;
}
EXPORT(RtlUniform);
EXPORT_AS("RtlRandom", w_RtlUniform);
EXPORT_AS("RtlRandomEx", w_RtlUniform);

static API u32 w_RtlComputeCrc32(u32 crc, const u8 *p, u32 n)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = crc >> 1 ^ (0xEDB88320u & -(crc & 1));
	}
	return ~crc;
}
EXPORT(RtlComputeCrc32);

static API size_t w_RtlCompareMemory(const u8 *a, const u8 *b, size_t n)
{
	size_t i = 0;

	while (i < n && a[i] == b[i])
		i++;
	return i;
}
EXPORT(RtlCompareMemory);

static API void w_RtlFillMemory(void *d, size_t n, int c)
{
	memset(d, c, n);
}
EXPORT(RtlFillMemory);

static API void w_RtlZeroMemory(void *d, size_t n)
{
	memset(d, 0, n);
}
EXPORT(RtlZeroMemory);

static API void w_RtlMoveMemory(void *d, const void *s, size_t n)
{
	memmove(d, s, n);
}
EXPORT(RtlMoveMemory);
EXPORT_AS("RtlCopyMemory", w_RtlMoveMemory);

static API u8 w_RtlIsProcessorFeaturePresent(u32 f)
{
	/* MMX, SSE, SSE2, SSE3, cmpxchg16b, NX, RDTSC, ... : the x86-64 baseline. */
	return f < 64 && ((0x0000000000E1B5D2ull >> f) & 1);
}
EXPORT(RtlIsProcessorFeaturePresent);

static API u8 w_RtlGetProductInfo(u32 a, u32 b, u32 c, u32 d, u32 *type)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	*type = 0x30;                           /* PRODUCT_PROFESSIONAL */
	return 1;
}
EXPORT(RtlGetProductInfo);

static API void w_RtlGetNtVersionNumbers(u32 *major, u32 *minor, u32 *build)
{
	if (major)
		*major = *(u32 *)(nt_peb + 0x118);
	if (minor)
		*minor = *(u32 *)(nt_peb + 0x11C);
	if (build)
		*build = 0xF0000000u | *(u16 *)(nt_peb + 0x120);
}
EXPORT(RtlGetNtVersionNumbers);

static u32 thread_error_mode;

static API u32 w_RtlGetThreadErrorMode(void)
{
	return thread_error_mode;
}
EXPORT(RtlGetThreadErrorMode);

static API NTSTATUS w_RtlSetThreadErrorMode(u32 mode, u32 *old)
{
	if (old)
		*old = thread_error_mode;
	thread_error_mode = mode;
	return 0;
}
EXPORT(RtlSetThreadErrorMode);

static API u64 w_RtlGetEnabledExtendedFeatures(u64 mask)
{
	return mask & 3;                        /* x87 + SSE */
}
EXPORT(RtlGetEnabledExtendedFeatures);
EXPORT_AS("RtlGetExtendedFeaturesMask", w_RtlGetEnabledExtendedFeatures);

static API NTSTATUS w_ApiSetQueryApiSetPresenceEx(const UNICODE_STRING *name, u8 *in_schema,
						  u8 *present)
{
	(void)name;
	*in_schema = 1;
	*present = 1;
	return 0;
}
EXPORT(ApiSetQueryApiSetPresenceEx);

static API NTSTATUS w_ApiSetQueryApiSetPresence(const UNICODE_STRING *name, u8 *present)
{
	(void)name;
	*present = 1;
	return 0;
}
EXPORT(ApiSetQueryApiSetPresence);

static API void *w_RtlNormalizeProcessParams(void *p)
{
	return p;
}
EXPORT(RtlNormalizeProcessParams);
EXPORT_AS("RtlDeNormalizeProcessParams", w_RtlNormalizeProcessParams);

/* Activation contexts: none exist, so lookups find nothing. */
static API NTSTATUS w_RtlGetActiveActivationContext(void **ctx)
{
	*ctx = NULL;
	return 0;
}
EXPORT(RtlGetActiveActivationContext);

static API NTSTATUS w_RtlNotFound5(void *a, void *b, void *c, void *d, void *e)
{
	(void)a;
	(void)b;
	(void)c;
	(void)d;
	(void)e;
	return STATUS_SXS_KEY_NOT_FOUND;
}
EXPORT_AS("RtlFindActivationContextSectionString", w_RtlNotFound5);
EXPORT_AS("RtlFindActivationContextSectionGuid", w_RtlNotFound5);

static API NTSTATUS w_RtlQueryInformationActivationContext(u32 flags, void *ctx, void *sub,
							   u32 cls, void *buf, size_t len,
							   size_t *ret)
{
	(void)flags;
	(void)ctx;
	(void)sub;
	(void)cls;
	if (len >= 8)
		memset(buf, 0, len < 64 ? len : 64);
	if (ret)
		*ret = 0;
	return cls == 1 ? 0 : STATUS_SXS_KEY_NOT_FOUND;
}
EXPORT(RtlQueryInformationActivationContext);

static API NTSTATUS w_RtlActivateActivationContext(u32 flags, void *ctx, u64 *cookie)
{
	(void)flags;
	(void)ctx;
	if (cookie)
		*cookie = 1;
	return 0;
}
EXPORT(RtlActivateActivationContext);

static API void w_RtlVoid1(void *a)
{
	(void)a;
}
EXPORT_AS("RtlAddRefActivationContext", w_RtlVoid1);
EXPORT_AS("RtlReleaseActivationContext", w_RtlVoid1);
EXPORT_AS("RtlFreeActivationContextStack", w_RtlVoid1);
EXPORT_AS("DbgBreakPoint", w_RtlVoid1);

static API NTSTATUS w_RtlDeactivateActivationContext(u32 flags, u64 cookie)
{
	(void)flags;
	(void)cookie;
	return 0;
}
EXPORT(RtlDeactivateActivationContext);

static API NTSTATUS w_RtlCreateActivationContext(void **ctx, const void *data)
{
	(void)data;
	*ctx = NULL;
	return STATUS_SXS_KEY_NOT_FOUND;
}
EXPORT(RtlCreateActivationContext);

static API NTSTATUS w_RtlQueryActivationContextApplicationSettings(u32 f, void *h, const wchar *ns,
								    const wchar *s, wchar *b,
								    size_t l, size_t *r)
{
	(void)f;
	(void)h;
	(void)ns;
	(void)s;
	(void)b;
	(void)l;
	(void)r;
	return STATUS_SXS_KEY_NOT_FOUND;
}
EXPORT(RtlQueryActivationContextApplicationSettings);

/* Preferred UI languages: en-US only. */
static API NTSTATUS w_RtlGetUserPreferredUILanguages(u32 flags, u32 unk, u32 *count, wchar *buf,
						     u32 *size)
{
	static const wchar en[] = L"en-US\0";

	(void)flags;
	(void)unk;
	*count = 1;
	if (buf && *size >= 7)
		memcpy(buf, en, sizeof(en));
	*size = 7;
	return 0;
}
EXPORT(RtlGetUserPreferredUILanguages);
EXPORT_AS("RtlGetSystemPreferredUILanguages", w_RtlGetUserPreferredUILanguages);
EXPORT_AS("RtlGetThreadPreferredUILanguages", w_RtlGetUserPreferredUILanguages);
EXPORT_AS("RtlGetProcessPreferredUILanguages", w_RtlGetUserPreferredUILanguages);

static API NTSTATUS w_RtlSetPreferredUILanguages(u32 flags, const wchar *buf, u32 *count)
{
	(void)flags;
	(void)buf;
	if (count)
		*count = 1;
	return 0;
}
EXPORT_AS("RtlSetProcessPreferredUILanguages", w_RtlSetPreferredUILanguages);
EXPORT_AS("RtlSetThreadPreferredUILanguages", w_RtlSetPreferredUILanguages);

/* ---- SIDs ------------------------------------------------------------------------- */

static API u32 w_RtlLengthRequiredSid(u32 n)
{
	return 8 + 4 * n;
}
EXPORT(RtlLengthRequiredSid);

static API u32 w_RtlLengthSid(const u8 *sid)
{
	return 8 + 4u * sid[1];
}
EXPORT(RtlLengthSid);

static API u8 w_RtlValidSid(const u8 *sid)
{
	return sid && sid[0] == 1 && sid[1] <= 15;
}
EXPORT(RtlValidSid);

static API NTSTATUS w_RtlInitializeSid(u8 *sid, const u8 *auth, u8 n)
{
	sid[0] = 1;
	sid[1] = n;
	memcpy(sid + 2, auth, 6);
	return 0;
}
EXPORT(RtlInitializeSid);

static API NTSTATUS w_RtlAllocateAndInitializeSid(const u8 *auth, u8 n, u32 a0, u32 a1, u32 a2,
						  u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, u8 **out)
{
	u32 sub[8] = { a0, a1, a2, a3, a4, a5, a6, a7 };
	u8 *sid;

	if (n > 8)
		return STATUS_INVALID_PARAMETER;
	sid = nt_alloc(8 + 4u * n);
	if (!sid)
		return STATUS_NO_MEMORY;
	w_RtlInitializeSid(sid, auth, n);
	memcpy(sid + 8, sub, 4u * n);
	*out = sid;
	return 0;
}
EXPORT(RtlAllocateAndInitializeSid);

static API void *w_RtlFreeSid(void *sid)
{
	nt_free(sid);
	return NULL;
}
EXPORT(RtlFreeSid);

static API NTSTATUS w_RtlCopySid(u32 len, u8 *dst, const u8 *src)
{
	u32 n = w_RtlLengthSid(src);

	if (len < n)
		return STATUS_BUFFER_TOO_SMALL;
	memcpy(dst, src, n);
	return 0;
}
EXPORT(RtlCopySid);

static API u8 w_RtlEqualSid(const u8 *a, const u8 *b)
{
	return a && b && a[1] == b[1] && !memcmp(a, b, w_RtlLengthSid(a));
}
EXPORT(RtlEqualSid);

static API u8 w_RtlEqualPrefixSid(const u8 *a, const u8 *b)
{
	return a && b && a[1] == b[1] && (!a[1] || !memcmp(a, b, 8 + 4u * (a[1] - 1u)));
}
EXPORT(RtlEqualPrefixSid);

static API u32 *w_RtlSubAuthoritySid(u8 *sid, u32 i)
{
	return (u32 *)(sid + 8) + i;
}
EXPORT(RtlSubAuthoritySid);

static API u8 *w_RtlSubAuthorityCountSid(u8 *sid)
{
	return sid + 1;
}
EXPORT(RtlSubAuthorityCountSid);

static API u8 *w_RtlIdentifierAuthoritySid(u8 *sid)
{
	return sid + 2;
}
EXPORT(RtlIdentifierAuthoritySid);

static API NTSTATUS w_RtlConvertSidToUnicodeString(UNICODE_STRING *s, const u8 *sid, u8 alloc)
{
	char tmp[160];
	size_t n = 0;
	u64 auth = 0;

	for (int i = 2; i < 8; i++)
		auth = auth << 8 | sid[i];
	tmp[n++] = 'S';
	tmp[n++] = '-';
	n += fmt_int(tmp + n, sid[0], 10);
	tmp[n++] = '-';
	n += fmt_int(tmp + n, auth, 10);
	for (u32 i = 0; i < sid[1]; i++) {
		tmp[n++] = '-';
		n += fmt_int(tmp + n, ((const u32 *)(sid + 8))[i], 10);
	}
	if (alloc) {
		s->buf = nt_alloc((n + 1) * 2);
		if (!s->buf)
			return STATUS_NO_MEMORY;
		s->max = (u16)((n + 1) * 2);
	} else if ((n + 1) * 2 > s->max) {
		return STATUS_BUFFER_OVERFLOW;
	}
	for (size_t i = 0; i <= n; i++)
		s->buf[i] = (wchar)tmp[i];
	s->len = (u16)(n * 2);
	return 0;
}
EXPORT(RtlConvertSidToUnicodeString);

/* Security descriptors: kept, never enforced. */
static API NTSTATUS w_RtlCreateSecurityDescriptor(u8 *sd, u32 rev)
{
	memset(sd, 0, 40);
	sd[0] = (u8)rev;
	return 0;
}
EXPORT(RtlCreateSecurityDescriptor);

static API NTSTATUS w_RtlSetDaclSecurityDescriptor(u8 *sd, u8 present, void *dacl, u8 def)
{
	(void)def;
	if (present)
		*(u16 *)(sd + 2) |= 4;
	*(void **)(sd + 32) = dacl;
	return 0;
}
EXPORT(RtlSetDaclSecurityDescriptor);

static API NTSTATUS w_RtlGetDaclSecurityDescriptor(u8 *sd, u8 *present, void **dacl, u8 *def)
{
	*present = 0;
	*dacl = NULL;
	if (def)
		*def = 0;
	(void)sd;
	return 0;
}
EXPORT(RtlGetDaclSecurityDescriptor);

static API u32 w_RtlLengthSecurityDescriptor(const u8 *sd)
{
	(void)sd;
	return 20;
}
EXPORT(RtlLengthSecurityDescriptor);

static API u8 w_RtlValidSecurityDescriptor(const u8 *sd)
{
	return sd && sd[0] == 1;
}
EXPORT(RtlValidSecurityDescriptor);

static API NTSTATUS w_RtlSetOwnerSecurityDescriptor(u8 *sd, void *sid, u8 def)
{
	(void)def;
	*(void **)(sd + 8) = sid;
	return 0;
}
EXPORT(RtlSetOwnerSecurityDescriptor);

static API NTSTATUS w_RtlSetGroupSecurityDescriptor(u8 *sd, void *sid, u8 def)
{
	(void)def;
	*(void **)(sd + 16) = sid;
	return 0;
}
EXPORT(RtlSetGroupSecurityDescriptor);

static API NTSTATUS w_RtlCreateAcl(u8 *acl, u32 len, u32 rev)
{
	if (len < 8)
		return STATUS_BUFFER_TOO_SMALL;
	memset(acl, 0, 8);
	acl[0] = (u8)rev;
	*(u16 *)(acl + 2) = (u16)len;
	return 0;
}
EXPORT(RtlCreateAcl);

static API NTSTATUS w_RtlAddAccessAllowedAce(u8 *acl, u32 rev, u32 mask, u8 *sid)
{
	(void)acl;
	(void)rev;
	(void)mask;
	(void)sid;
	return 0;
}
EXPORT(RtlAddAccessAllowedAce);

static API u8 w_RtlValidAcl(const u8 *acl)
{
	return acl != NULL;
}
EXPORT(RtlValidAcl);

static API NTSTATUS w_RtlMakeSelfRelativeSD(const u8 *abs, u8 *rel, u32 *len)
{
	(void)abs;
	if (*len < 20) {
		*len = 20;
		return STATUS_BUFFER_TOO_SMALL;
	}
	memset(rel, 0, 20);
	rel[0] = 1;
	*(u16 *)(rel + 2) = 0x8000;
	*len = 20;
	return 0;
}
EXPORT(RtlMakeSelfRelativeSD);

static API NTSTATUS w_RtlGetControlSecurityDescriptor(const u8 *sd, u16 *ctl, u32 *rev)
{
	*ctl = *(const u16 *)(sd + 2);
	*rev = sd[0];
	return 0;
}
EXPORT(RtlGetControlSecurityDescriptor);

/* ---- CRT functions ntdll exports that the CRT file lacks -------------------------- */

static API int w_isalnum(int c)
{
	return (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z');
}
EXPORT(isalnum);

static API int w_islower(int c)
{
	return c >= 'a' && c <= 'z';
}
EXPORT(islower);

static API int w_isupper(int c)
{
	return c >= 'A' && c <= 'Z';
}
EXPORT(isupper);

static API int w_isxdigit(int c)
{
	return digit((u32)c) < 16;
}
EXPORT(isxdigit);

static API int w_iswspace(wchar c)
{
	return c == ' ' || (c >= 9 && c <= 13);
}
EXPORT(iswspace);

static API int w_iswdigit(wchar c)
{
	return c >= '0' && c <= '9';
}
EXPORT(iswdigit);

static API int w_iswalpha(wchar c)
{
	return ((c | 32) >= 'a' && (c | 32) <= 'z') || (c >= 0xC0 && c != 0xD7 && c != 0xF7);
}
EXPORT(iswalpha);

static API char *w_strpbrk(const char *s, const char *set)
{
	for (; *s; s++)
		for (const char *k = set; *k; k++)
			if (*s == *k)
				return (char *)s;
	return NULL;
}
EXPORT(strpbrk);

static API size_t w_wcsspn(const wchar *s, const wchar *set)
{
	size_t n = 0;

	for (; s[n]; n++) {
		const wchar *k = set;

		while (*k && *k != s[n])
			k++;
		if (!*k)
			break;
	}
	return n;
}
EXPORT(wcsspn);

static API size_t w_wcscspn(const wchar *s, const wchar *set)
{
	size_t n = 0;

	for (; s[n]; n++)
		for (const wchar *k = set; *k; k++)
			if (*k == s[n])
				return n;
	return n;
}
EXPORT(wcscspn);

static API wchar *w_wcspbrk(const wchar *s, const wchar *set)
{
	size_t n = w_wcscspn(s, set);

	return s[n] ? (wchar *)s + n : NULL;
}
EXPORT(wcspbrk);

static API size_t w_wcsnlen(const wchar *s, size_t max)
{
	size_t n = 0;

	while (n < max && s[n])
		n++;
	return n;
}
EXPORT(wcsnlen);

static API wchar *w_wcsstr(const wchar *s, const wchar *sub)
{
	size_t n = rt_wcslen(sub);

	if (!n)
		return (wchar *)s;
	for (; *s; s++)
		if (!memcmp(s, sub, n * 2))
			return (wchar *)s;
	return NULL;
}
EXPORT(wcsstr);

static u64 wcs_to_u(const wchar *s, wchar **end, int base, bool *neg)
{
	const wchar *p = s;
	u64 v = 0;
	bool any = false;

	while (w_iswspace(*p))
		p++;
	*neg = false;
	if (*p == '+' || *p == '-')
		*neg = *p++ == '-';
	if ((!base || base == 16) && p[0] == '0' && (p[1] | 32) == 'x') {
		p += 2;
		base = 16;
	} else if (!base) {
		base = p[0] == '0' ? 8 : 10;
	}
	for (; digit(*p) < base; p++) {
		v = v * (u64)base + (u64)digit(*p);
		any = true;
	}
	if (end)
		*end = (wchar *)(any ? p : s);
	return v;
}

static API long w_wcstol(const wchar *s, wchar **end, int base)
{
	bool neg;
	u64 v = wcs_to_u(s, end, base, &neg);

	if (v > 0x7FFFFFFF)
		v = neg ? 0x80000000ull : 0x7FFFFFFF;
	return neg ? -(long)(s32)v : (long)(s32)v;
}
EXPORT(wcstol);

static API u32 w_wcstoul(const wchar *s, wchar **end, int base)
{
	bool neg;
	u64 v = wcs_to_u(s, end, base, &neg);

	if (v > 0xFFFFFFFF)
		v = 0xFFFFFFFF;
	return neg ? (u32)-(s64)v : (u32)v;
}
EXPORT(wcstoul);

static API s64 w__wcstoi64(const wchar *s, wchar **end, int base)
{
	bool neg;
	u64 v = wcs_to_u(s, end, base, &neg);

	return neg ? -(s64)v : (s64)v;
}
EXPORT(_wcstoi64);

static API u64 w__wcstoui64(const wchar *s, wchar **end, int base)
{
	bool neg;
	u64 v = wcs_to_u(s, end, base, &neg);

	return neg ? (u64)-(s64)v : v;
}
EXPORT(_wcstoui64);

static API wchar *w_wcsncpy(wchar *d, const wchar *s, size_t n)
{
	size_t i = 0;

	for (; i < n && s[i]; i++)
		d[i] = s[i];
	for (; i < n; i++)
		d[i] = 0;
	return d;
}
EXPORT(wcsncpy);

static API wchar *w_wcsncat(wchar *d, const wchar *s, size_t n)
{
	size_t k = rt_wcslen(d), i = 0;

	for (; i < n && s[i]; i++)
		d[k + i] = s[i];
	d[k + i] = 0;
	return d;
}
EXPORT(wcsncat);

static API int w__wtoi(const wchar *s)
{
	return (int)w_wcstol(s, NULL, 10);
}
EXPORT(_wtoi);
EXPORT_AS("_wtol", w__wtoi);
