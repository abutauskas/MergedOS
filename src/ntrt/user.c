/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The few user32 / shell32 entry points console programs such as Wine's
 * cmd.exe import: character classification and case mapping, wsprintf, and
 * shell calls that report "not supported". No windowing.
 */
#include "src/ntrt/nt.h"

static bool is_alpha(wchar c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c != 0xD7 && c != 0xF7) ||
	       c == 0xAA || c == 0xB5 || c == 0xBA;
}

static API BOOL w_IsCharAlphaW(wchar c) { return is_alpha(c); }
EXPORT(IsCharAlphaW);

static API BOOL w_IsCharAlphaNumericW(wchar c) { return is_alpha(c) || (c >= '0' && c <= '9'); }
EXPORT(IsCharAlphaNumericW);

static API u32 w_CharUpperBuffW(wchar *s, u32 n)
{
	for (u32 i = 0; i < n; i++)
		s[i] = nt_upcase(s[i]);
	return n;
}
EXPORT(CharUpperBuffW);

static API u32 w_CharLowerBuffW(wchar *s, u32 n)
{
	for (u32 i = 0; i < n; i++)
		s[i] = nt_downcase(s[i]);
	return n;
}
EXPORT(CharLowerBuffW);

/* Single-byte code page: the next character is the next byte. */
static API char *w_CharNextExA(u16 cp, char *s, u32 flags)
{
	(void)cp;
	(void)flags;
	return *s ? s + 1 : s;
}
EXPORT(CharNextExA);

static API char *w_CharNextA(char *s) { return *s ? s + 1 : s; }
EXPORT(CharNextA);

static API int w_wsprintfW(wchar *buf, const wchar *fmt, ...)
{
	ms_va_list ap;
	struct rt_sink s = { .wbuf = buf, .cap = 1024 };

	ms_va_start(ap, fmt);
	rt_format(&s, fmt, true, (const u64 *)ap);
	ms_va_end(ap);
	buf[s.len < 1024 ? s.len : 1023] = 0;
	return (int)s.len;
}
EXPORT(wsprintfW);

static API int w_wsprintfA(char *buf, const char *fmt, ...)
{
	ms_va_list ap;
	struct rt_sink s = { .buf = buf, .cap = 1024 };

	ms_va_start(ap, fmt);
	rt_format(&s, fmt, false, (const u64 *)ap);
	ms_va_end(ap);
	buf[s.len < 1024 ? s.len : 1023] = 0;
	return (int)s.len;
}
EXPORT(wsprintfA);

/* shell32: nothing to find, nothing to copy. */
static API void *w_FindExecutableW(const wchar *f, const wchar *dir, wchar *result)
{
	(void)f;
	(void)dir;
	if (result)
		result[0] = 0;
	return (void *)2;                       /* SE_ERR_FNF */
}
EXPORT(FindExecutableW);

static API int w_SHFileOperationW(void *op)
{
	(void)op;
	return 0x7C;                            /* ERROR_INVALID_LEVEL: unsupported */
}
EXPORT(SHFileOperationW);

static API u64 w_SHGetFileInfoW(const wchar *path, u32 attrs, void *info, u32 size, u32 flags)
{
	(void)path; (void)attrs; (void)info; (void)size; (void)flags;
	return 0;
}
EXPORT(SHGetFileInfoW);
