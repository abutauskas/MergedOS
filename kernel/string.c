/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Minimal freestanding string routines. GCC emits calls to memcpy/memset/
 * memmove/memcmp even with -ffreestanding, so these four are mandatory.
 * (Built with -fno-tree-loop-distribute-patterns so the loops below are not
 * "optimised" back into calls to themselves.)
 */
#include <mergedos/kernel.h>

void *memcpy(void *dst, const void *src, size_t n)
{
	u8 *d = dst;
	const u8 *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
	u8 *d = dst;
	const u8 *s = src;

	if (d < s) {
		while (n--)
			*d++ = *s++;
	} else {
		while (n--)
			d[n] = s[n];
	}
	return dst;
}

void *memset(void *dst, int c, size_t n)
{
	u8 *d = dst;

	while (n--)
		*d++ = (u8)c;
	return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const u8 *x = a, *y = b;

	for (; n--; x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const u8 *p = s;

	for (; n--; p++)
		if (*p == (u8)c)
			return (void *)p;
	return NULL;
}

size_t strlen(const char *s)
{
	const char *p = s;

	while (*p)
		p++;
	return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	for (; n; n--, a++, b++) {
		if (*a != *b)
			return (u8)*a - (u8)*b;
		if (!*a)
			break;
	}
	return 0;
}

char *strchr(const char *s, int c)
{
	for (;; s++) {
		if (*s == (char)c)
			return (char *)s;
		if (!*s)
			return NULL;
	}
}
