/* SPDX-License-Identifier: BSD-2-Clause */
#include "pkg_translate/tar.h"

#define TAR_BLOCK 512

static u64 parse_octal(const u8 *p, size_t n)
{
	u64 v = 0;

	while (n && (*p == ' ' || *p == '0'))
		p++, n--;
	for (; n && *p >= '0' && *p <= '7'; p++, n--)
		v = (v << 3) | (u64)(*p - '0');
	return v;
}

static bool block_is_zero(const u8 *p)
{
	for (int i = 0; i < TAR_BLOCK; i++)
		if (p[i])
			return false;
	return true;
}

/* Header checksum: byte sum of the block with the checksum field as spaces. */
static bool checksum_ok(const u8 *h)
{
	u64 sum = 0;

	for (int i = 0; i < TAR_BLOCK; i++)
		sum += (i >= 148 && i < 156) ? ' ' : h[i];
	return sum == parse_octal(h + 148, 8);
}

bool tar_probe(const u8 *buf, size_t len)
{
	return len >= TAR_BLOCK && !memcmp(buf + 257, "ustar", 5) && checksum_ok(buf);
}

int tar_next(const u8 *buf, size_t len, size_t *off, struct tar_entry *e)
{
	for (;;) {
		const u8 *h;
		const char *name;
		size_t n = 0;
		u64 size, span;

		if (!mos_in_range(len, *off, TAR_BLOCK))
			return 0;
		h = buf + *off;
		if (block_is_zero(h))
			return 0;
		if (!checksum_ok(h))
			return -MERR_INVAL;

		size = parse_octal(h + 124, 12);
		span = ALIGN_UP(size, TAR_BLOCK);
		if (!mos_in_range(len, *off + TAR_BLOCK, size))
			return 0;               /* truncated (e.g. partially decoded stream) */

		/* ustar splits long paths into prefix[155] + '/' + name[100]. */
		e->name[0] = '\0';
		if (!memcmp(h + 257, "ustar", 5) && h[345]) {
			for (; n < 155 && h[345 + n]; n++)
				e->name[n] = (char)h[345 + n];
			e->name[n++] = '/';
		}
		for (size_t i = 0; i < 100 && h[i]; i++)
			e->name[n++] = (char)h[i];
		e->name[n] = '\0';

		name = e->name;
		while (name[0] == '.' && name[1] == '/')
			name += 2;
		while (name[0] == '/')
			name++;
		if (name != e->name)
			memmove(e->name, name, strlen(name) + 1);

		e->type = h[156] ? (char)h[156] : '0';
		e->size = size;
		e->data = h + TAR_BLOCK;
		*off += TAR_BLOCK + (size_t)span;

		/* Skip pax / GNU extension records; their payload is metadata. */
		if (e->type == 'x' || e->type == 'g' || e->type == 'L' || e->type == 'K')
			continue;
		return 1;
	}
}

int tar_find(const u8 *buf, size_t len, const char *path, struct tar_entry *e)
{
	size_t off = 0;

	while (*path == '/')
		path++;
	while (tar_next(buf, len, &off, e) > 0)
		if ((e->type == '0' || e->type == '7') && !strcmp(e->name, path))
			return 0;
	return -MERR_NOENT;
}
