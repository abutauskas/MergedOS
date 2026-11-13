/* SPDX-License-Identifier: BSD-2-Clause */
/* Outer containers used by package formats: ar (.deb), cpio newc (.rpm payload). */
#include "pkg_translate/pkg_internal.h"

#define AR_MAGIC     "!<arch>\n"
#define AR_HDR_SIZE  60
#define CPIO_HDR     110

static u64 parse_dec(const u8 *p, size_t n)
{
	u64 v = 0;

	for (; n && *p >= '0' && *p <= '9'; p++, n--)
		v = v * 10 + (u64)(*p - '0');
	return v;
}

int ar_next(const u8 *buf, size_t len, size_t *off, struct ar_member *m)
{
	const u8 *h;
	size_t n = 0;

	if (*off == 0) {
		if (len < 8 || memcmp(buf, AR_MAGIC, 8))
			return -MERR_INVAL;
		*off = 8;
	}
	if (*off >= len)
		return 0;
	if (!mos_in_range(len, *off, AR_HDR_SIZE))
		return -MERR_INVAL;
	h = buf + *off;
	if (h[58] != '`' || h[59] != '\n')
		return -MERR_INVAL;

	/* Name: 16 bytes, space padded; GNU ar appends '/'. */
	while (n < 16 && h[n] != ' ' && h[n] != '/')
		n++;
	mos_fieldcpy(m->name, sizeof(m->name), (const char *)h, n);
	m->size = parse_dec(h + 48, 10);
	m->data = h + AR_HDR_SIZE;
	if (!mos_in_range(len, *off + AR_HDR_SIZE, m->size))
		return -MERR_INVAL;
	*off += AR_HDR_SIZE + (size_t)m->size + (m->size & 1);  /* members are 2-aligned */
	return 1;
}

static bool parse_hex8(const u8 *p, u32 *out)
{
	u32 v = 0;

	for (int i = 0; i < 8; i++) {
		u8 c = p[i];

		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (u32)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (u32)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (u32)(c - 'A' + 10);
		else
			return false;
	}
	*out = v;
	return true;
}

int cpio_next(const u8 *buf, size_t len, size_t *off, struct cpio_entry *e)
{
	const u8 *h;
	u32 mode, filesize, namesize;
	u64 name_off, data_off;

	if (!mos_in_range(len, *off, CPIO_HDR))
		return 0;
	h = buf + *off;
	if (memcmp(h, "070701", 6) && memcmp(h, "070702", 6))
		return -MERR_INVAL;
	/* 13 eight-digit hex fields: ino, mode, uid, gid, nlink, mtime, filesize, ... namesize, check */
	if (!parse_hex8(h + 6 + 1 * 8, &mode) || !parse_hex8(h + 6 + 6 * 8, &filesize) ||
	    !parse_hex8(h + 6 + 11 * 8, &namesize) || namesize == 0)
		return -MERR_INVAL;

	name_off = (u64)*off + CPIO_HDR;
	data_off = ALIGN_UP(name_off + namesize, 4);
	if (!mos_in_range(len, name_off, namesize) || buf[name_off + namesize - 1] != '\0')
		return -MERR_INVAL;
	if (!mos_in_range(len, data_off, filesize))
		return 0;                       /* truncated (partially decoded payload) */

	e->name = (const char *)buf + name_off;
	e->mode = mode;
	e->size = filesize;
	e->data = buf + data_off;
	*off = (size_t)ALIGN_UP(data_off + filesize, 4);
	return strcmp(e->name, "TRAILER!!!") ? 1 : 0;
}

enum pkg_codec pkg_codec_sniff(const u8 *p, size_t len)
{
	if (len >= 3 && p[0] == 0x1F && p[1] == 0x8B && p[2] == 8)
		return PKG_CODEC_GZIP;
	if (len >= 6 && !memcmp(p, "\3757zXZ\0", 6))
		return PKG_CODEC_XZ;
	if (len >= 4 && rd32le(p) == 0xFD2FB528u)
		return PKG_CODEC_ZSTD;
	if (len >= 3 && !memcmp(p, "BZh", 3))
		return PKG_CODEC_BZIP2;
	if (tar_probe(p, len) || (len >= 6 && !memcmp(p, "07070", 5)))
		return PKG_CODEC_NONE;
	return PKG_CODEC_UNKNOWN;
}

int pkg_decode(enum pkg_codec codec, const u8 *in, size_t inlen, u8 *buf, size_t cap,
	       size_t *consumed, size_t *produced)
{
	size_t need = codec == PKG_CODEC_XZ ? XZ_WORK_SIZE : ZSTD_WORK_SIZE, wk;

	*produced = 0;
	if (codec == PKG_CODEC_GZIP)
		return gz_decode(in, inlen, buf, cap, consumed, produced);
	if (codec != PKG_CODEC_XZ && codec != PKG_CODEC_ZSTD)
		return -MERR_NOTSUP;
	if (!buf || cap < need + 8)
		return -MERR_NOMEM;
	wk = (size_t)(ALIGN_DOWN((u64)buf + cap - need, 8) - (u64)buf);
	return (codec == PKG_CODEC_XZ ? xz_decode : zstd_decode)(in, inlen, buf, wk, consumed,
								 produced, buf + wk, cap - wk);
}

int pkg_unpack(enum pkg_codec codec, const u8 *in, size_t inlen, struct pkg_scratch *s,
	       const u8 **out, size_t *outlen)
{
	size_t consumed = 0;
	int rc;

	if (codec == PKG_CODEC_NONE) {
		*out = in;
		*outlen = inlen;
		return 0;
	}
	if (codec != PKG_CODEC_GZIP && codec != PKG_CODEC_XZ && codec != PKG_CODEC_ZSTD)
		return -MERR_NOTSUP;            /* bzip2: codec not built in */
	if (!s || !s->buf || !s->cap)
		return -MERR_NOMEM;
	rc = pkg_decode(codec, in, inlen, s->buf, s->cap, &consumed, outlen);
	if (rc < 0)
		return rc == -MERR_NOTSUP || rc == -MERR_NOMEM ? rc : -MERR_NOEXEC;
	*out = s->buf;
	return rc;
}
