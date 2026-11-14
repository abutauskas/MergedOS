/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * xz decoder: the .xz container (format 1.0.4) with the LZMA2 filter, written
 * from the public xz file-format and LZMA specifications.
 *
 * Single call, flat output: the output buffer is the LZMA dictionary, so the
 * only state is the probability model, kept in caller-supplied `work`.
 * Checks: CRC32, CRC64 and SHA-256 are verified, others are skipped. Filter
 * chains other than a lone LZMA2 (BCJ, delta) are reported as -MERR_NOTSUP.
 */
#include "installer/sha256.h"
#include "pkg_translate/pkg_internal.h"

/* Probability model layout (16-bit probabilities, 11-bit precision). */
enum {
	P_MATCH = 0,                    /* [12 states][16 pos states] */
	P_REP = P_MATCH + 192,
	P_REPG0 = P_REP + 12,
	P_REPG1 = P_REPG0 + 12,
	P_REPG2 = P_REPG1 + 12,
	P_REP0L = P_REPG2 + 12,         /* [12][16] */
	P_SLOT = P_REP0L + 192,         /* [4 length states][64] */
	P_SPEC = P_SLOT + 256,          /* 115 reverse-tree probabilities, slots 4..13 */
	P_ALIGN = P_SPEC + 115,
	P_LEN = P_ALIGN + 16,           /* choice, choice2, low[16][8], mid[16][8], high[256] */
	P_REPLEN = P_LEN + 514,
	P_LIT = P_REPLEN + 514,         /* 0x300 << (lc + lp) */
	P_TOTAL = P_LIT + (0x300 << 4)
};

struct xz_work {
	u16 p[P_TOTAL];
};
_Static_assert(sizeof(struct xz_work) <= XZ_WORK_SIZE, "XZ_WORK_SIZE too small");

struct lz {
	const u8 *in;
	size_t ip, iend;
	u32 range, code;
	u8 *out;
	size_t op, olimit, dstart;      /* dstart: position of the last dictionary reset */
	u32 state, rep[4];
	unsigned lc, lp, pb;
	u16 *p;
	bool err;
};

static inline u16 rd16be(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }

static void rc_norm(struct lz *z)
{
	if (z->range < (1u << 24)) {
		z->range <<= 8;
		z->code <<= 8;
		if (z->ip < z->iend)
			z->code |= z->in[z->ip++];
		else
			z->err = true;
	}
}

static unsigned rc_bit(struct lz *z, u16 *p)
{
	u32 bound;

	rc_norm(z);
	bound = (z->range >> 11) * *p;
	if (z->code < bound) {
		z->range = bound;
		*p += (2048 - *p) >> 5;
		return 0;
	}
	z->range -= bound;
	z->code -= bound;
	*p -= *p >> 5;
	return 1;
}

static unsigned rc_tree(struct lz *z, u16 *p, int bits)
{
	unsigned m = 1;

	for (int i = 0; i < bits; i++)
		m = m << 1 | rc_bit(z, p + m);
	return m - (1u << bits);
}

static unsigned rc_rtree(struct lz *z, u16 *p, int bits)
{
	unsigned m = 1, s = 0;

	for (int i = 0; i < bits; i++) {
		unsigned b = rc_bit(z, p + m);

		m = m << 1 | b;
		s |= b << i;
	}
	return s;
}

static u32 rc_direct(struct lz *z, int bits)
{
	u32 s = 0;

	while (bits--) {
		rc_norm(z);
		z->range >>= 1;
		s <<= 1;
		if (z->code >= z->range) {
			z->code -= z->range;
			s |= 1;
		}
	}
	return s;
}

static unsigned lz_len(struct lz *z, u16 *p, unsigned ps)
{
	if (!rc_bit(z, p))
		return rc_tree(z, p + 2 + ps * 8, 3);
	if (!rc_bit(z, p + 1))
		return 8 + rc_tree(z, p + 130 + ps * 8, 3);
	return 16 + rc_tree(z, p + 258, 8);
}

/* Decode one LZMA chunk up to z->olimit. `partial`: the output ends early. */
static int lz_run(struct lz *z, bool partial)
{
	u16 *p = z->p;

	while (z->op < z->olimit && !z->err) {
		size_t pos = z->op - z->dstart, n;
		unsigned ps = pos & ((1u << z->pb) - 1), st = z->state;

		if (!rc_bit(z, p + P_MATCH + (st << 4) + ps)) {
			unsigned prev = pos ? z->out[z->op - 1] : 0, sym = 1;
			u16 *lp = p + P_LIT + 0x300 * (((pos & ((1u << z->lp) - 1)) << z->lc) +
						       (prev >> (8 - z->lc)));

			if (st >= 7) {
				unsigned mb;

				if (z->rep[0] >= pos)
					return -MERR_INVAL;
				mb = z->out[z->op - z->rep[0] - 1];
				while (sym < 0x100) {
					unsigned mbit = (mb >> 7) & 1, b;

					mb <<= 1;
					b = rc_bit(z, lp + ((1 + mbit) << 8) + sym);
					sym = sym << 1 | b;
					if (b != mbit)
						break;
				}
			}
			while (sym < 0x100)
				sym = sym << 1 | rc_bit(z, lp + sym);
			z->out[z->op++] = (u8)sym;
			z->state = st < 4 ? 0 : st < 10 ? st - 3 : st - 6;
			continue;
		}
		if (!rc_bit(z, p + P_REP + st)) {               /* new match */
			unsigned len = lz_len(z, p + P_LEN, ps), slot;
			u32 d;

			z->rep[3] = z->rep[2];
			z->rep[2] = z->rep[1];
			z->rep[1] = z->rep[0];
			z->state = st < 7 ? 7 : 10;
			slot = rc_tree(z, p + P_SLOT + ((len < 3 ? len : 3) << 6), 6);
			d = slot;
			if (slot >= 4) {
				int nb = (int)(slot >> 1) - 1;

				d = (2 | (slot & 1)) << nb;
				if (slot < 14)
					d += rc_rtree(z, p + P_SPEC + d - slot, nb);
				else
					d += (rc_direct(z, nb - 4) << 4) + rc_rtree(z, p + P_ALIGN, 4);
			}
			z->rep[0] = d;
			n = len + 2;
		} else if (!rc_bit(z, p + P_REPG0 + st)) {
			if (!rc_bit(z, p + P_REP0L + (st << 4) + ps)) {
				z->state = st < 7 ? 9 : 11;     /* short rep: one byte */
				n = 1;
				goto copy;
			}
			n = lz_len(z, p + P_REPLEN, ps) + 2;
			z->state = st < 7 ? 8 : 11;
		} else {
			u32 d;

			if (!rc_bit(z, p + P_REPG1 + st)) {
				d = z->rep[1];
			} else {
				if (!rc_bit(z, p + P_REPG2 + st)) {
					d = z->rep[2];
				} else {
					d = z->rep[3];
					z->rep[3] = z->rep[2];
				}
				z->rep[2] = z->rep[1];
			}
			z->rep[1] = z->rep[0];
			z->rep[0] = d;
			n = lz_len(z, p + P_REPLEN, ps) + 2;
			z->state = st < 7 ? 8 : 11;
		}
copy:
		if (z->err || (u64)z->rep[0] + 1 > pos)
			return -MERR_INVAL;
		if (n > z->olimit - z->op) {
			if (!partial)
				return -MERR_INVAL;
			n = z->olimit - z->op;
		}
		for (u8 *d = z->out + z->op, *s = d - z->rep[0] - 1; n; n--, z->op++)
			*d++ = *s++;
	}
	if (z->err)
		return -MERR_INVAL;
	if (partial)
		return 1;
	rc_norm(z);
	return !z->err && z->ip == z->iend && z->code == 0 ? 0 : -MERR_INVAL;
}

/* One LZMA2 stream: 0 at its end marker, 1 when `out` filled first, <0 on error. */
static int lzma2_decode(struct xz_work *w, const u8 *in, size_t inlen, size_t *ip, u8 *out,
			size_t outcap, size_t *op)
{
	struct lz z = { .in = in, .out = out, .op = *op, .p = w->p };
	bool have_dict = false, have_props = false;
	size_t i = *ip;

	for (;;) {
		size_t un, cn;
		unsigned c, reset;
		int rc;

		if (i >= inlen)
			return -MERR_INVAL;
		c = in[i++];
		if (c == 0)
			break;
		if (c <= 2) {                                   /* stored chunk */
			if (inlen - i < 2)
				return -MERR_INVAL;
			un = rd16be(in + i) + 1u;
			i += 2;
			if (c == 1) {
				z.dstart = z.op;
				have_dict = true;
			} else if (!have_dict) {
				return -MERR_INVAL;
			}
			if (inlen - i < un)
				return -MERR_INVAL;
			if (un > outcap - z.op) {
				memcpy(out + z.op, in + i, outcap - z.op);
				*op = outcap;
				return 1;
			}
			memcpy(out + z.op, in + i, un);
			i += un;
			z.op += un;
			continue;
		}
		if (c < 0x80 || inlen - i < 4)
			return -MERR_INVAL;
		un = ((size_t)(c & 0x1F) << 16) + rd16be(in + i) + 1;
		cn = rd16be(in + i + 2) + 1u;
		i += 4;
		reset = (c >> 5) & 3;
		if (reset == 3) {
			z.dstart = z.op;
			have_dict = true;
		} else if (!have_dict) {
			return -MERR_INVAL;
		}
		if (reset >= 2) {
			unsigned pr;

			if (i >= inlen || (pr = in[i++]) >= 225)
				return -MERR_INVAL;
			z.lc = pr % 9;
			z.lp = (pr / 9) % 5;
			z.pb = pr / 45;
			if (z.lc + z.lp > 4)
				return -MERR_INVAL;
			have_props = true;
		} else if (!have_props) {
			return -MERR_INVAL;
		}
		if (reset >= 1) {
			z.state = 0;
			z.rep[0] = z.rep[1] = z.rep[2] = z.rep[3] = 0;
			for (size_t k = 0; k < P_LIT + (0x300u << (z.lc + z.lp)); k++)
				w->p[k] = 1024;
		}
		if (cn < 5 || inlen - i < cn || in[i] != 0)
			return -MERR_INVAL;
		z.code = rd32be(in + i + 1);
		z.range = 0xFFFFFFFFu;
		z.ip = i + 5;
		z.iend = i + cn;
		z.olimit = un > outcap - z.op ? outcap : z.op + un;
		rc = lz_run(&z, z.olimit != z.op + un);
		if (rc) {
			*op = z.op;
			return rc;
		}
		i += cn;
	}
	*ip = i;
	*op = z.op;
	return 0;
}

static int vli(const u8 *p, size_t end, size_t *i, u64 *v)
{
	*v = 0;
	for (int k = 0; k < 9 && *i < end; k++) {
		u8 b = p[(*i)++];

		*v |= (u64)(b & 0x7F) << (7 * k);
		if (!(b & 0x80))
			return k && !b ? -1 : 0;        /* no redundant trailing zero byte */
	}
	return -1;
}

static u64 crc64(const u8 *p, size_t n)
{
	u64 c = ~0ull;

	while (n--) {
		c ^= *p++;
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xC96C5795D7870F42ull & (0 - (c & 1)));
	}
	return ~c;
}

static bool check_ok(unsigned type, const u8 *data, size_t n, const u8 *ck)
{
	if (type == 1)
		return rd32le(ck) == pkg_crc32(0, data, n);
	if (type == 4)
		return rd64le(ck) == crc64(data, n);
	if (type == 10) {
		struct sha256 c;
		u8 h[SHA256_LEN];

		sha256_init(&c);
		sha256_update(&c, data, n);
		sha256_final(&c, h);
		return !memcmp(h, ck, SHA256_LEN);
	}
	return true;                                    /* none, or a check we cannot compute */
}

static int xz_stream(struct xz_work *w, const u8 *in, size_t inlen, size_t *ip, u8 *out,
		     size_t outcap, size_t *op)
{
	static const u8 check_len[16] = { 0, 4, 4, 4, 8, 8, 8, 16, 16, 16, 32, 32, 32, 64, 64, 64 };
	size_t s = *ip, i = s + 12, ix;
	u64 nblk = 0, sum_unpadded = 0, sum_unc = 0, n, a, b, su = 0, sc = 0;
	unsigned check;

	if (inlen - s < 12 || memcmp(in + s, "\3757zXZ\0", 6) || in[s + 6] || in[s + 7] > 15 ||
	    pkg_crc32(0, in + s + 6, 2) != rd32le(in + s + 8))
		return -MERR_INVAL;
	check = in[s + 7];
	for (;;) {
		size_t hs, h, hend, dstart, ostart = *op, clen;
		u64 csize = ~0ull, usize = ~0ull, id, psz;
		u8 f;
		int rc;

		if (i >= inlen)
			return -MERR_INVAL;
		if (!in[i])
			break;                                  /* index indicator */
		hs = (in[i] + 1u) * 4;
		if (inlen - i < hs || pkg_crc32(0, in + i, hs - 4) != rd32le(in + i + hs - 4))
			return -MERR_INVAL;
		f = in[i + 1];
		h = i + 2;
		hend = i + hs - 4;
		if ((f & 0x3C) || ((f & 0x40) && (vli(in, hend, &h, &csize) || !csize)) ||
		    ((f & 0x80) && vli(in, hend, &h, &usize)))
			return -MERR_INVAL;
		if (f & 3)
			return -MERR_NOTSUP;                    /* filter chain (BCJ, delta) */
		if (vli(in, hend, &h, &id) || vli(in, hend, &h, &psz) || psz > hend - h)
			return -MERR_INVAL;
		if (id != 0x21 || psz != 1)
			return -MERR_NOTSUP;
		if (in[h] > 40)
			return -MERR_INVAL;
		for (h++; h < hend; h++)
			if (in[h])
				return -MERR_INVAL;
		i += hs;
		dstart = i;
		rc = lzma2_decode(w, in, inlen, &i, out, outcap, op);
		if (rc)
			return rc;
		clen = i - dstart;
		if ((csize != ~0ull && csize != clen) || (usize != ~0ull && usize != *op - ostart))
			return -MERR_INVAL;
		for (; (i - s) & 3; i++)
			if (i >= inlen || in[i])
				return -MERR_INVAL;
		if (inlen - i < check_len[check] ||
		    !check_ok(check, out + ostart, *op - ostart, in + i))
			return -MERR_INVAL;
		i += check_len[check];
		nblk++;
		sum_unpadded += hs + clen + check_len[check];
		sum_unc += *op - ostart;
	}
	ix = i++;
	if (vli(in, inlen, &i, &n) || n != nblk)
		return -MERR_INVAL;
	for (u64 k = 0; k < n; k++) {
		if (vli(in, inlen, &i, &a) || vli(in, inlen, &i, &b))
			return -MERR_INVAL;
		su += a;
		sc += b;
	}
	if (su != sum_unpadded || sc != sum_unc)
		return -MERR_INVAL;
	for (; (i - ix) & 3; i++)
		if (i >= inlen || in[i])
			return -MERR_INVAL;
	if (inlen - i < 16 || rd32le(in + i) != pkg_crc32(0, in + ix, i - ix))
		return -MERR_INVAL;
	i += 4;
	if (pkg_crc32(0, in + i + 4, 6) != rd32le(in + i) ||
	    (u64)rd32le(in + i + 4) != (i - ix) / 4 - 1 || memcmp(in + i + 8, in + s + 6, 2) ||
	    memcmp(in + i + 10, "YZ", 2))
		return -MERR_INVAL;
	*ip = i + 12;
	return 0;
}

int xz_decode(const u8 *in, size_t inlen, u8 *out, size_t outcap, size_t *consumed,
	      size_t *produced, void *work, size_t worklen)
{
	size_t i = 0, op = 0;

	*produced = 0;
	if (!work || worklen < sizeof(struct xz_work))
		return -MERR_NOMEM;
	do {
		int rc = xz_stream(work, in, inlen, &i, out, outcap, &op);

		*produced = op;
		if (rc)
			return rc;
		while (inlen - i >= 4 && !rd32le(in + i))       /* stream padding */
			i += 4;
	} while (i < inlen);
	*consumed = i;
	return 0;
}
