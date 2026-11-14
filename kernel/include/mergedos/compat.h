/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_COMPAT_H
#define MERGEDOS_COMPAT_H

/*
 * Portability shim for "shared" code: sources that are linked both into the
 * freestanding kernel and into host-side unit tests / tools (ABI dispatcher,
 * binary-format probes, DE policy, package translation, shell core).
 *
 * Build with -DMERGEDOS_HOSTED to target a normal libc.
 */
#include <mergedos/types.h>
#include <mergedos/errno.h>

#ifdef MERGEDOS_HOSTED
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define mos_snprintf snprintf
#define mos_zalloc(n) calloc(1, (n))
#define mos_free free
#else
#include <mergedos/kernel.h>
#define mos_snprintf ksnprintf
#define mos_zalloc kzalloc
#define mos_free kfree
#endif

static inline int mos_tolower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static inline int mos_strcasecmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int d = mos_tolower((u8)*a) - mos_tolower((u8)*b);
		if (d || !*a)
			return d;
	}
}

/* Always NUL-terminates (when cap > 0); returns strlen(src). */
static inline size_t mos_strlcpy(char *dst, const char *src, size_t cap)
{
	size_t n = strlen(src);

	if (cap) {
		size_t c = n < cap - 1 ? n : cap - 1;
		memcpy(dst, src, c);
		dst[c] = '\0';
	}
	return n;
}

/* Copy a length-delimited (not NUL-terminated) field into a C string. */
static inline void mos_fieldcpy(char *dst, size_t cap, const char *src, size_t n)
{
	if (!cap)
		return;
	if (n > cap - 1)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static inline size_t mos_strlcat(char *dst, const char *src, size_t cap)
{
	size_t d = strlen(dst);

	if (d >= cap)
		return d + strlen(src);
	return d + mos_strlcpy(dst + d, src, cap - d);
}

/* Find a NUL-terminated needle inside a length-delimited haystack. */
static inline const char *mos_memstr(const char *hay, size_t haylen, const char *needle)
{
	size_t n = strlen(needle);

	if (!n)
		return hay;
	for (; haylen >= n; hay++, haylen--)
		if (*hay == *needle && !memcmp(hay, needle, n))
			return hay;
	return NULL;
}

static inline bool mos_has_prefix(const char *s, const char *prefix)
{
	return !strncmp(s, prefix, strlen(prefix));
}

/* Case-insensitive suffix test ("Foo.APP" matches ".app"). */
static inline bool mos_has_suffix(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);

	return n >= m && !mos_strcasecmp(s + n - m, suffix);
}

/* Unaligned little/big-endian readers for on-disk structures. */
static inline u16 rd16le(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u32 rd32le(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static inline u64 rd64le(const u8 *p)
{
	return (u64)rd32le(p) | ((u64)rd32le(p + 4) << 32);
}
static inline u32 rd32be(const u8 *p)
{
	return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

/* True when [off, off + n) lies inside a buffer of `len` bytes (overflow-safe). */
static inline bool mos_in_range(u64 len, u64 off, u64 n)
{
	return off <= len && n <= len - off;
}

#endif /* MERGEDOS_COMPAT_H */
