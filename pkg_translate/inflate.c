/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * gzip (RFC 1952) / DEFLATE (RFC 1951) decoder.
 *
 * Small rather than fast: canonical Huffman codes are decoded one bit at a
 * time, which needs no lookup tables and about 2.5 KiB of stack. It is used
 * for package *metadata* (a few KiB), where footprint matters more than
 * throughput. The whole output must fit the caller's buffer, which doubles
 * as the 32 KiB history window.
 */
#include "pkg_translate/pkg_internal.h"

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

enum { INF_OK = 0, INF_FULL = 1, INF_ERR = -1 };

struct inf {
	const u8 *in;
	size_t inlen, inpos;
	u8 *out;
	size_t outcap, outpos;
	u32 bitbuf;
	int bitcnt;
};

struct huffman {
	u16 count[MAXBITS + 1];
	u16 symbol[FIXLCODES];
};

/* Fetch `need` (0..16) bits, least significant first. Returns -1 on underrun. */
static int bits(struct inf *s, int need, u32 *val)
{
	u32 v = s->bitbuf;

	while (s->bitcnt < need) {
		if (s->inpos == s->inlen)
			return -1;
		v |= (u32)s->in[s->inpos++] << s->bitcnt;
		s->bitcnt += 8;
	}
	s->bitbuf = v >> need;
	s->bitcnt -= need;
	*val = v & ((1u << need) - 1);
	return 0;
}

static int decode(struct inf *s, const struct huffman *h)
{
	int code = 0, first = 0, index = 0;

	for (int len = 1; len <= MAXBITS; len++) {
		u32 b;
		int count;

		if (bits(s, 1, &b))
			return -1;
		code |= (int)b;
		count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/* Build decoding tables from code lengths. Returns 0 if the code is complete,
 * >0 if incomplete, <0 if over-subscribed. */
static int construct(struct huffman *h, const u16 *length, int n)
{
	u16 offs[MAXBITS + 1];
	int left = 1;

	memset(h->count, 0, sizeof(h->count));
	for (int sym = 0; sym < n; sym++)
		h->count[length[sym]]++;
	if (h->count[0] == n)
		return 0;
	for (int len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return left;
	}
	offs[1] = 0;
	for (int len = 1; len < MAXBITS; len++)
		offs[len + 1] = (u16)(offs[len] + h->count[len]);
	for (int sym = 0; sym < n; sym++)
		if (length[sym])
			h->symbol[offs[length[sym]]++] = (u16)sym;
	return left;
}

static int codes(struct inf *s, const struct huffman *lencode, const struct huffman *distcode)
{
	static const u16 lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
				       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
	static const u8 lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
				     3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
	static const u16 dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
				       257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
				       8193, 12289, 16385, 24577 };
	static const u8 dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
				     7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

	for (;;) {
		int sym = decode(s, lencode);
		u32 extra;
		size_t len, dist;

		if (sym < 0)
			return INF_ERR;
		if (sym < 256) {
			if (s->outpos == s->outcap)
				return INF_FULL;
			s->out[s->outpos++] = (u8)sym;
			continue;
		}
		if (sym == 256)
			return INF_OK;
		sym -= 257;
		if (sym >= 29 || bits(s, lext[sym], &extra))
			return INF_ERR;
		len = lbase[sym] + extra;

		sym = decode(s, distcode);
		if (sym < 0 || sym >= 30 || bits(s, dext[sym], &extra))
			return INF_ERR;
		dist = dbase[sym] + extra;
		if (dist > s->outpos)
			return INF_ERR;
		while (len--) {
			if (s->outpos == s->outcap)
				return INF_FULL;
			s->out[s->outpos] = s->out[s->outpos - dist];
			s->outpos++;
		}
	}
}

static int block_stored(struct inf *s)
{
	size_t len;

	s->bitbuf = 0;                          /* discard bits up to the byte boundary */
	s->bitcnt = 0;
	if (s->inlen - s->inpos < 4)
		return INF_ERR;
	len = rd16le(s->in + s->inpos);
	if (len != (u16)~rd16le(s->in + s->inpos + 2))
		return INF_ERR;
	s->inpos += 4;
	if (s->inlen - s->inpos < len)
		return INF_ERR;
	while (len--) {
		if (s->outpos == s->outcap)
			return INF_FULL;
		s->out[s->outpos++] = s->in[s->inpos++];
	}
	return INF_OK;
}

static int block_fixed(struct inf *s)
{
	struct huffman lencode, distcode;
	u16 lengths[FIXLCODES];
	int sym = 0;

	for (; sym < 144; sym++)
		lengths[sym] = 8;
	for (; sym < 256; sym++)
		lengths[sym] = 9;
	for (; sym < 280; sym++)
		lengths[sym] = 7;
	for (; sym < FIXLCODES; sym++)
		lengths[sym] = 8;
	construct(&lencode, lengths, FIXLCODES);
	for (sym = 0; sym < MAXDCODES; sym++)
		lengths[sym] = 5;
	construct(&distcode, lengths, MAXDCODES);
	return codes(s, &lencode, &distcode);
}

static int block_dynamic(struct inf *s)
{
	static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	struct huffman lencode, distcode;
	u16 lengths[MAXLCODES + MAXDCODES];
	u32 v;
	int nlen, ndist, ncode, index, err;

	if (bits(s, 5, &v))
		return INF_ERR;
	nlen = (int)v + 257;
	if (bits(s, 5, &v))
		return INF_ERR;
	ndist = (int)v + 1;
	if (bits(s, 4, &v))
		return INF_ERR;
	ncode = (int)v + 4;
	if (nlen > MAXLCODES || ndist > MAXDCODES)
		return INF_ERR;

	for (index = 0; index < ncode; index++) {
		if (bits(s, 3, &v))
			return INF_ERR;
		lengths[order[index]] = (u16)v;
	}
	for (; index < 19; index++)
		lengths[order[index]] = 0;
	if (construct(&lencode, lengths, 19) != 0)
		return INF_ERR;

	for (index = 0; index < nlen + ndist;) {
		int sym = decode(s, &lencode), repeat;
		u16 len = 0;

		if (sym < 0)
			return INF_ERR;
		if (sym < 16) {
			lengths[index++] = (u16)sym;
			continue;
		}
		if (sym == 16) {
			if (index == 0 || bits(s, 2, &v))
				return INF_ERR;
			len = lengths[index - 1];
			repeat = 3 + (int)v;
		} else if (sym == 17) {
			if (bits(s, 3, &v))
				return INF_ERR;
			repeat = 3 + (int)v;
		} else {
			if (bits(s, 7, &v))
				return INF_ERR;
			repeat = 11 + (int)v;
		}
		if (index + repeat > nlen + ndist)
			return INF_ERR;
		while (repeat--)
			lengths[index++] = len;
	}
	if (lengths[256] == 0)                  /* no end-of-block code */
		return INF_ERR;

	/* An incomplete code is only legal when it consists of a single symbol. */
	err = construct(&lencode, lengths, nlen);
	if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1))
		return INF_ERR;
	err = construct(&distcode, lengths + nlen, ndist);
	if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1))
		return INF_ERR;
	return codes(s, &lencode, &distcode);
}

/* Decode DEFLATE blocks up to and including the final one. */
static int inflate_blocks(struct inf *s)
{
	u32 last, type;
	int rc;

	do {
		if (bits(s, 1, &last) || bits(s, 2, &type))
			return INF_ERR;
		rc = type == 0 ? block_stored(s)
		   : type == 1 ? block_fixed(s)
		   : type == 2 ? block_dynamic(s) : INF_ERR;
		if (rc != INF_OK)
			return rc;
	} while (!last);
	return INF_OK;
}

u32 pkg_crc32(u32 crc, const u8 *p, size_t n)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

int gz_decode(const u8 *in, size_t inlen, u8 *out, size_t outcap, size_t *consumed,
	      size_t *produced)
{
	struct inf s = { .in = in, .inlen = inlen, .out = out, .outcap = outcap };
	u8 flags;
	int rc;

	*produced = 0;
	if (inlen < 18 || in[0] != 0x1F || in[1] != 0x8B || in[2] != 8)
		return -MERR_INVAL;
	flags = in[3];
	s.inpos = 10;
	if (flags & 0x04) {                     /* FEXTRA */
		if (s.inpos + 2 > inlen)
			return -MERR_INVAL;
		s.inpos += 2 + (size_t)rd16le(in + s.inpos);
	}
	for (int f = 0x08; f <= 0x10; f <<= 1)  /* FNAME, FCOMMENT: NUL-terminated */
		if (flags & f) {
			while (s.inpos < inlen && in[s.inpos])
				s.inpos++;
			s.inpos++;
		}
	if (flags & 0x02)                       /* FHCRC */
		s.inpos += 2;
	if (s.inpos >= inlen)
		return -MERR_INVAL;

	rc = inflate_blocks(&s);
	if (rc == INF_FULL) {
		*produced = s.outpos;
		return 1;
	}
	if (rc != INF_OK)
		return -MERR_INVAL;

	/* Trailer: CRC-32 and length of the uncompressed data, both little-endian. */
	if (inlen - s.inpos < 8 || rd32le(in + s.inpos) != pkg_crc32(0, out, s.outpos) ||
	    rd32le(in + s.inpos + 4) != (u32)s.outpos)
		return -MERR_INVAL;
	*produced = s.outpos;
	*consumed = s.inpos + 8;
	return 0;
}

int inflate_raw(const u8 *in, size_t inlen, u8 *out, size_t outcap, size_t *consumed,
		size_t *produced)
{
	struct inf s = { .in = in, .inlen = inlen, .out = out, .outcap = outcap };
	int rc = inflate_blocks(&s);

	*produced = s.outpos;
	if (rc == INF_FULL)
		return 1;
	if (rc != INF_OK)
		return -MERR_INVAL;
	*consumed = s.inpos;
	return 0;
}

int zlib_decode(const u8 *in, size_t inlen, u8 *out, size_t outcap, size_t *consumed,
		size_t *produced)
{
	size_t used = 0;
	u32 a = 1, b = 0;
	int rc;

	*produced = 0;
	/* CMF/FLG: deflate, window <= 32 KiB, no preset dictionary, check bits. */
	if (inlen < 6 || (in[0] & 0x0F) != 8 || (in[0] >> 4) > 7 || (in[1] & 0x20) ||
	    ((u32)in[0] << 8 | in[1]) % 31)
		return -MERR_INVAL;
	rc = inflate_raw(in + 2, inlen - 2, out, outcap, &used, produced);
	if (rc)
		return rc;
	for (size_t i = 0; i < *produced; i++) {        /* Adler-32 of the output */
		a = (a + out[i]) % 65521;
		b = (b + a) % 65521;
	}
	if (inlen - 2 - used < 4 || rd32be(in + 2 + used) != (b << 16 | a))
		return -MERR_INVAL;
	*consumed = 2 + used + 4;
	return 0;
}
