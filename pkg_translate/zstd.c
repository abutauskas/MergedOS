/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Zstandard decoder, written from RFC 8878: frames and skippable frames,
 * raw/RLE/compressed blocks, Huffman-coded literals, FSE-coded sequences,
 * repeat offsets and the XXH64 content checksum. Dictionaries are not
 * supported (-MERR_NOTSUP).
 *
 * Single call, flat output: matches reference earlier output directly, so no
 * window buffer is needed. Literals are decoded lazily while sequences run,
 * so no literal buffer is needed either and a short `out` still yields the
 * longest valid prefix. Tables live in caller-supplied `work`.
 */
#include "pkg_translate/pkg_internal.h"

#define BLOCK_MAX (128u * 1024)
#define HUF_LOG_MAX 11

struct fse {
	u8 sym, nb;
	u16 base;
};

struct huf {
	u8 sym, nb;
};

struct zstd_work {
	struct huf huf[1 << HUF_LOG_MAX];
	struct fse ll[1 << 9], of[1 << 8], ml[1 << 9];
	u8 huf_log, ll_log, of_log, ml_log;
	bool huf_ok, ll_ok, of_ok, ml_ok;
};
_Static_assert(sizeof(struct zstd_work) <= ZSTD_WORK_SIZE, "ZSTD_WORK_SIZE too small");

/* Bit stream; `pos` counts bits. Backward streams read downwards from the marker. */
struct bits {
	const u8 *p;
	size_t n;
	long long pos;
};

static unsigned hibit(u32 x) { return 31 - (unsigned)__builtin_clz(x); }

/* Bits [lo, lo + k) of the stream as a little-endian number; outside reads as 0. */
static u64 bits_at(const struct bits *b, long long lo, unsigned k)
{
	unsigned sh = 0;
	u64 acc = 0;
	size_t byte;

	if (!k)
		return 0;
	if (lo < 0) {
		if ((unsigned long long)-lo >= k)
			return 0;
		sh = (unsigned)-lo;
		k -= sh;
		lo = 0;
	}
	byte = (size_t)(lo >> 3);
	for (unsigned j = 0; j < 8 && j * 8 < (lo & 7) + k && byte + j < b->n; j++)
		acc |= (u64)b->p[byte + j] << (8 * j);
	return ((acc >> (lo & 7)) & ((1ull << k) - 1)) << sh;
}

static u64 rd_back(struct bits *b, unsigned k)
{
	b->pos -= k;
	return bits_at(b, b->pos, k);
}

static int bits_back(struct bits *b, const u8 *p, size_t n)
{
	if (!n || !p[n - 1])
		return -1;
	b->p = p;
	b->n = n;
	b->pos = 8 * (long long)(n - 1) + hibit(p[n - 1]);
	return 0;
}

/* ---- FSE ------------------------------------------------------------------ */

static int fse_norm(const u8 *p, size_t n, short *norm, unsigned maxsym, unsigned maxlog,
		    unsigned *log, size_t *used)
{
	struct bits b = { p, n, 4 };
	int remaining, thr, nb;
	unsigned s = 0, al;

	if (!n || (al = (p[0] & 15) + 5) > maxlog)
		return -1;
	thr = 1 << al;
	remaining = thr + 1;
	nb = (int)al + 1;
	while (remaining > 1) {
		int max = 2 * thr - 1 - remaining, v;
		u32 raw = (u32)bits_at(&b, b.pos, (unsigned)nb);

		if (s > maxsym)
			return -1;
		if ((int)(raw & (u32)(thr - 1)) < max) {
			v = (int)(raw & (u32)(thr - 1));
			b.pos += nb - 1;
		} else {
			v = (int)(raw & (u32)(2 * thr - 1));
			if (v >= thr)
				v -= max;
			b.pos += nb;
		}
		v--;
		remaining -= v < 0 ? -v : v;
		if (remaining < 1)
			return -1;
		norm[s++] = (short)v;
		if (!v) {
			unsigned r;

			do {
				r = (unsigned)bits_at(&b, b.pos, 2);
				b.pos += 2;
				for (unsigned k = 0; k < r; k++) {
					if (s > maxsym)
						return -1;
					norm[s++] = 0;
				}
			} while (r == 3 && b.pos <= 8 * (long long)n);
		}
		while (remaining < thr) {
			nb--;
			thr >>= 1;
		}
		if (b.pos > 8 * (long long)n)
			return -1;
	}
	if (remaining != 1)
		return -1;
	while (s <= maxsym)
		norm[s++] = 0;
	*log = al;
	*used = (size_t)((b.pos + 7) >> 3);
	return 0;
}

static int fse_build(struct fse *t, const short *norm, unsigned nsym, unsigned log)
{
	unsigned size = 1u << log, high = size - 1, pos = 0, step = (size >> 1) + (size >> 3) + 3;
	u16 next[64];

	for (unsigned s = 0; s < nsym; s++) {
		if (norm[s] == -1) {
			t[high--].sym = (u8)s;
			next[s] = 1;
		} else {
			next[s] = (u16)norm[s];
		}
	}
	for (unsigned s = 0; s < nsym; s++)
		for (int k = 0; k < norm[s]; k++) {
			t[pos].sym = (u8)s;
			do
				pos = (pos + step) & (size - 1);
			while (pos > high);
		}
	if (pos)
		return -1;
	for (unsigned u = 0; u < size; u++) {
		unsigned x = next[t[u].sym]++;

		t[u].nb = (u8)(log - hibit(x));
		t[u].base = (u16)((x << t[u].nb) - size);
	}
	return 0;
}

/* ---- Huffman literals ------------------------------------------------------- */

static int huf_table(struct zstd_work *w, const u8 *p, size_t n, size_t *used)
{
	u8 wt[256];
	unsigned nw = 0, maxb, cnt[HUF_LOG_MAX + 2] = { 0 }, start[HUF_LOG_MAX + 2];
	u32 total = 0, rest, acc = 0;

	if (!n)
		return -1;
	if (p[0] >= 128) {                              /* 4-bit direct weights */
		nw = p[0] - 127u;
		if (n < 1 + (nw + 1) / 2)
			return -1;
		for (unsigned k = 0; k < nw; k++)
			wt[k] = (k & 1) ? p[1 + k / 2] & 15 : p[1 + k / 2] >> 4;
		*used = 1 + (nw + 1) / 2;
	} else {                                        /* FSE-compressed weights */
		struct fse t[64];
		short norm[16];
		unsigned log, s1, s2;
		size_t h = p[0], u;
		struct bits b;

		if (!h || n < 1 + h || fse_norm(p + 1, h, norm, 15, 6, &log, &u) || u >= h ||
		    fse_build(t, norm, 16, log) || bits_back(&b, p + 1 + u, h - u))
			return -1;
		s1 = (unsigned)rd_back(&b, log);
		s2 = (unsigned)rd_back(&b, log);
		for (;;) {
			if (nw > 253)
				return -1;
			wt[nw++] = t[s1].sym;
			s1 = t[s1].base + (unsigned)rd_back(&b, t[s1].nb);
			if (b.pos < 0) {
				wt[nw++] = t[s2].sym;
				break;
			}
			wt[nw++] = t[s2].sym;
			s2 = t[s2].base + (unsigned)rd_back(&b, t[s2].nb);
			if (b.pos < 0) {
				wt[nw++] = t[s1].sym;
				break;
			}
		}
		*used = 1 + h;
	}
	for (unsigned k = 0; k < nw; k++) {
		if (wt[k] > HUF_LOG_MAX)
			return -1;
		if (wt[k])
			total += 1u << (wt[k] - 1);
	}
	if (!total || nw > 255 || (maxb = hibit(total) + 1) > HUF_LOG_MAX)
		return -1;
	rest = (1u << maxb) - total;
	if (rest & (rest - 1))
		return -1;
	wt[nw++] = (u8)(hibit(rest) + 1);               /* the last weight is implied */
	for (unsigned k = 0; k < nw; k++)
		cnt[wt[k]]++;
	for (unsigned v = 1; v <= maxb; v++) {
		start[v] = acc;
		acc += cnt[v] << (v - 1);
	}
	for (unsigned k = 0; k < nw; k++) {
		unsigned v = wt[k];

		if (!v)
			continue;
		for (u32 j = 0; j < 1u << (v - 1); j++)
			w->huf[start[v] + j] = (struct huf){ (u8)k, (u8)(maxb + 1 - v) };
		start[v] += 1u << (v - 1);
	}
	w->huf_log = (u8)maxb;
	w->huf_ok = true;
	return 0;
}

/* Literal source, consumed byte by byte while the sequences execute. */
struct lits {
	unsigned type, nstreams, cur;
	const u8 *raw;
	size_t total, done, seg_end[4];
	struct bits b[4];
	const struct zstd_work *w;
};

static int lit_next(struct lits *l, u8 *out)
{
	const struct huf *h;
	struct bits *b;

	if (l->type == 0)
		*out = l->raw[l->done];
	else if (l->type == 1)
		*out = l->raw[0];
	else {
		while (l->done >= l->seg_end[l->cur]) {
			if (l->b[l->cur].pos != 0)
				return -1;
			l->cur++;
		}
		b = &l->b[l->cur];
		h = &l->w->huf[bits_at(b, b->pos - l->w->huf_log, l->w->huf_log)];
		*out = h->sym;
		b->pos -= h->nb;
		if (b->pos < 0)
			return -1;
	}
	l->done++;
	return 0;
}

/* Everything consumed exactly? */
static bool lit_end(const struct lits *l)
{
	if (l->done != l->total)
		return false;
	if (l->type >= 2)
		for (unsigned k = l->cur; k < l->nstreams; k++)
			if (l->b[k].pos != 0)
				return false;
	return true;
}

static int lit_section(struct zstd_work *w, const u8 *p, size_t n, struct lits *l, size_t *used)
{
	unsigned sf;
	size_t hl, cs;
	u64 h = 0;

	if (!n)
		return -1;
	memset(l, 0, sizeof(*l));
	l->w = w;
	l->type = p[0] & 3;
	sf = (p[0] >> 2) & 3;
	if (l->type < 2) {
		hl = (sf & 1) ? sf == 1 ? 2 : 3 : 1;
		if (n < hl)
			return -1;
		l->total = hl == 1 ? p[0] >> 3u
			 : hl == 2 ? (p[0] >> 4u) | (size_t)p[1] << 4
				   : (p[0] >> 4u) | (size_t)p[1] << 4 | (size_t)p[2] << 12;
		cs = l->type == 0 ? l->total : 1;
		if (n - hl < cs)
			return -1;
		l->raw = p + hl;
		*used = hl + cs;
	} else {
		unsigned nbits = sf < 2 ? 10 : sf == 2 ? 14 : 18;
		const u8 *q;
		size_t qn, seg, t;

		hl = sf < 2 ? 3 : sf == 2 ? 4 : 5;
		if (n < hl)
			return -1;
		for (size_t k = 0; k < hl; k++)
			h |= (u64)p[k] << (8 * k);
		l->total = (size_t)((h >> 4) & ((1u << nbits) - 1));
		cs = (size_t)((h >> (4 + nbits)) & ((1u << nbits) - 1));
		if (n - hl < cs)
			return -1;
		*used = hl + cs;
		q = p + hl;
		qn = cs;
		if (l->type == 2) {
			if (huf_table(w, q, qn, &t) || t > qn)
				return -1;
			q += t;
			qn -= t;
		} else if (!w->huf_ok) {
			return -1;
		}
		l->nstreams = sf ? 4 : 1;
		if (l->nstreams == 1) {
			l->seg_end[0] = l->total;
			if (bits_back(&l->b[0], q, qn))
				return -1;
		} else {
			size_t sz[4];

			if (qn < 6)
				return -1;
			sz[0] = rd16le(q);
			sz[1] = rd16le(q + 2);
			sz[2] = rd16le(q + 4);
			if (sz[0] + sz[1] + sz[2] > qn - 6)
				return -1;
			sz[3] = qn - 6 - sz[0] - sz[1] - sz[2];
			seg = (l->total + 3) / 4;
			if (3 * seg > l->total)
				return -1;
			q += 6;
			for (unsigned k = 0; k < 4; k++) {
				if (bits_back(&l->b[k], q, sz[k]))
					return -1;
				q += sz[k];
				l->seg_end[k] = k < 3 ? seg * (k + 1) : l->total;
			}
		}
	}
	return l->total > BLOCK_MAX ? -1 : 0;
}

/* ---- sequences ------------------------------------------------------------- */

static const short ll_def[36] = { 4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2,
				  2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1 };
static const short ml_def[53] = { 1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,
				  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
				  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1 };
static const short of_def[32] = { 1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1,
				  1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1 };
static const u32 ll_base[36] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
				 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024,
				 2048, 4096, 8192, 16384, 32768, 65536 };
static const u8 ll_bits[36] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
				1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const u32 ml_base[53] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
				 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
				 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027,
				 2051, 4099, 8195, 16387, 32771, 65539 };
static const u8 ml_bits[53] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
				0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
				2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

static int seq_table(struct fse *t, u8 *log, bool *ok, unsigned mode, const u8 *p, size_t n,
		     size_t *i, const short *def, unsigned deflog, unsigned maxsym, unsigned maxlog)
{
	short norm[64];
	unsigned lg;
	size_t used;

	switch (mode) {
	case 0:
		if (fse_build(t, def, maxsym + 1, deflog))
			return -1;
		*log = (u8)deflog;
		break;
	case 1:
		if (*i >= n || p[*i] > maxsym)
			return -1;
		t[0] = (struct fse){ p[(*i)++], 0, 0 };
		*log = 0;
		break;
	case 2:
		if (fse_norm(p + *i, n - *i, norm, maxsym, maxlog, &lg, &used) ||
		    fse_build(t, norm, maxsym + 1, lg))
			return -1;
		*i += used;
		*log = (u8)lg;
		break;
	default:
		if (!*ok)
			return -1;
		return 0;
	}
	*ok = true;
	return 0;
}

struct zctx {
	struct zstd_work *w;
	u8 *out;
	size_t op, cap, fstart;
	u32 rep[3];
};

/* Copy `n` literals; 1 if the output filled first. */
static int put_lits(struct zctx *c, struct lits *l, size_t n)
{
	if (n > l->total - l->done)
		return -1;
	for (; n; n--) {
		if (c->op == c->cap)
			return 1;
		if (lit_next(l, &c->out[c->op]))
			return -1;
		c->op++;
	}
	return 0;
}

static int zstd_block(struct zctx *c, const u8 *p, size_t n)
{
	struct zstd_work *w = c->w;
	struct lits l;
	struct bits b;
	size_t i, nseq;
	unsigned lls, ofs, mls;
	int rc;

	if (lit_section(w, p, n, &l, &i) || i >= n)
		return -1;
	nseq = p[i++];
	if (nseq >= 128) {
		if (nseq == 255) {
			if (n - i < 2)
				return -1;
			nseq = rd16le(p + i) + 0x7F00u;
			i += 2;
		} else {
			if (i >= n)
				return -1;
			nseq = ((nseq - 128) << 8) + p[i++];
		}
	}
	if (nseq) {
		unsigned m;

		if (i >= n || ((m = p[i++]) & 3) ||
		    seq_table(w->ll, &w->ll_log, &w->ll_ok, m >> 6, p, n, &i, ll_def, 6, 35, 9) ||
		    seq_table(w->of, &w->of_log, &w->of_ok, (m >> 4) & 3, p, n, &i, of_def, 5, 31, 8) ||
		    seq_table(w->ml, &w->ml_log, &w->ml_ok, (m >> 2) & 3, p, n, &i, ml_def, 6, 52, 9) ||
		    bits_back(&b, p + i, n - i))
			return -1;
		lls = (unsigned)rd_back(&b, w->ll_log);
		ofs = (unsigned)rd_back(&b, w->of_log);
		mls = (unsigned)rd_back(&b, w->ml_log);
		for (size_t s = 0; s < nseq; s++) {
			unsigned ofc = w->of[ofs].sym, mlc = w->ml[mls].sym, llc = w->ll[lls].sym;
			u64 ofv = (1ull << ofc) + rd_back(&b, ofc), off;
			size_t mlv = ml_base[mlc] + (size_t)rd_back(&b, ml_bits[mlc]);
			size_t llv = ll_base[llc] + (size_t)rd_back(&b, ll_bits[llc]);

			if (s + 1 < nseq) {
				lls = w->ll[lls].base + (unsigned)rd_back(&b, w->ll[lls].nb);
				mls = w->ml[mls].base + (unsigned)rd_back(&b, w->ml[mls].nb);
				ofs = w->of[ofs].base + (unsigned)rd_back(&b, w->of[ofs].nb);
			}
			if (b.pos < 0)
				return -1;
			if (ofv > 3) {
				off = ofv - 3;
				c->rep[2] = c->rep[1];
				c->rep[1] = c->rep[0];
			} else {
				unsigned idx = (unsigned)ofv - 1 + (llv == 0);

				off = idx == 3 ? c->rep[0] - 1ull : c->rep[idx];
				if (!off)
					return -1;
				if (idx >= 2)
					c->rep[2] = c->rep[1];
				if (idx >= 1)
					c->rep[1] = c->rep[0];
			}
			c->rep[0] = (u32)off;
			if ((rc = put_lits(c, &l, llv)))
				return rc;
			if (off > c->op - c->fstart)
				return -1;
			for (const u8 *src = c->out + c->op - off; mlv; mlv--) {
				if (c->op == c->cap)
					return 1;
				c->out[c->op++] = *src++;
			}
		}
		if (b.pos != 0)
			return -1;
	}
	if ((rc = put_lits(c, &l, l.total - l.done)))
		return rc;
	return lit_end(&l) ? 0 : -1;
}

/* ---- XXH64 ------------------------------------------------------------------- */

#define XP1 0x9E3779B185EBCA87ull
#define XP2 0xC2B2AE3D27D4EB4Full
#define XP3 0x165667B19E3779F9ull
#define XP4 0x85EBCA77C2B2AE63ull
#define XP5 0x27D4EB2F165667C5ull

static u64 rotl(u64 x, int r) { return x << r | x >> (64 - r); }
static u64 xround(u64 acc, u64 v) { return rotl(acc + v * XP2, 31) * XP1; }

u64 pkg_xxh64(const u8 *p, size_t n)
{
	const u8 *end = p + n;
	u64 h;

	if (n >= 32) {
		u64 v[4] = { XP1 + XP2, XP2, 0, 0 - XP1 };

		for (; end - p >= 32; p += 32)
			for (int k = 0; k < 4; k++)
				v[k] = xround(v[k], rd64le(p + 8 * k));
		h = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18);
		for (int k = 0; k < 4; k++)
			h = (h ^ xround(0, v[k])) * XP1 + XP4;
	} else {
		h = XP5;
	}
	h += n;
	for (; end - p >= 8; p += 8)
		h = rotl(h ^ xround(0, rd64le(p)), 27) * XP1 + XP4;
	if (end - p >= 4) {
		h = rotl(h ^ (u64)rd32le(p) * XP1, 23) * XP2 + XP3;
		p += 4;
	}
	for (; p < end; p++)
		h = rotl(h ^ *p * XP5, 11) * XP1;
	h ^= h >> 33;
	h *= XP2;
	h ^= h >> 29;
	h *= XP3;
	return h ^ (h >> 32);
}

/* ---- frames ------------------------------------------------------------------- */

static int zstd_frame(struct zctx *c, const u8 *in, size_t inlen, size_t *ip)
{
	size_t i = *ip;
	unsigned fhd, fcsf, ss, didf, fcs_len;
	u64 did = 0, fcs = 0;
	bool last;

	if (inlen - i < 5 || ((fhd = in[i + 4]) & 8))
		return -1;
	i += 5;
	fcsf = fhd >> 6;
	ss = (fhd >> 5) & 1;
	didf = fhd & 3;
	fcs_len = fcsf ? 1u << fcsf : ss;
	if (inlen - i < !ss + (didf ? 1u << (didf - 1) : 0) + fcs_len)
		return -1;
	i += !ss;                                       /* window descriptor: flat output */
	for (unsigned k = 0; didf && k < 1u << (didf - 1); k++)
		did |= (u64)in[i++] << (8 * k);
	if (did)
		return -MERR_NOTSUP;
	for (unsigned k = 0; k < fcs_len; k++)
		fcs |= (u64)in[i++] << (8 * k);
	if (fcsf == 1)
		fcs += 256;
	c->rep[0] = 1;
	c->rep[1] = 4;
	c->rep[2] = 8;
	c->w->huf_ok = c->w->ll_ok = c->w->of_ok = c->w->ml_ok = false;
	c->fstart = c->op;
	do {
		u32 bh;
		size_t bs, k;
		int rc = 0;

		if (inlen - i < 3)
			return -1;
		bh = in[i] | (u32)in[i + 1] << 8 | (u32)in[i + 2] << 16;
		i += 3;
		last = bh & 1;
		bs = bh >> 3;
		if (bs > BLOCK_MAX || ((bh >> 1) & 3) == 3 || inlen - i < (((bh >> 1) & 3) == 1 ? 1 : bs))
			return -1;
		switch ((bh >> 1) & 3) {
		case 0:
			k = bs < c->cap - c->op ? bs : c->cap - c->op;
			memcpy(c->out + c->op, in + i, k);
			c->op += k;
			rc = k < bs;
			i += bs;
			break;
		case 1:
			k = bs < c->cap - c->op ? bs : c->cap - c->op;
			memset(c->out + c->op, in[i], k);
			c->op += k;
			rc = k < bs;
			i++;
			break;
		default:
			rc = zstd_block(c, in + i, bs);
			i += bs;
		}
		if (rc)
			return rc;
	} while (!last);
	if ((fcsf || ss) && c->op - c->fstart != fcs)
		return -1;
	if (fhd & 4) {
		if (inlen - i < 4 ||
		    rd32le(in + i) != (u32)pkg_xxh64(c->out + c->fstart, c->op - c->fstart))
			return -1;
		i += 4;
	}
	*ip = i;
	return 0;
}

int zstd_decode(const u8 *in, size_t inlen, u8 *out, size_t outcap, size_t *consumed,
		size_t *produced, void *work, size_t worklen)
{
	struct zctx c = { .w = work, .out = out, .cap = outcap };
	size_t i = 0;
	int frames = 0;

	*produced = 0;
	if (!work || worklen < sizeof(struct zstd_work))
		return -MERR_NOMEM;
	while (inlen - i >= 4) {
		u32 magic = rd32le(in + i);
		int rc;

		if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {     /* skippable frame */
			if (inlen - i < 8 || rd32le(in + i + 4) > inlen - i - 8)
				return -MERR_INVAL;
			i += 8 + rd32le(in + i + 4);
			continue;
		}
		if (magic != 0xFD2FB528u)
			return -MERR_INVAL;
		rc = zstd_frame(&c, in, inlen, &i);
		*produced = c.op;
		if (rc)
			return rc == 1 ? 1 : rc == -MERR_NOTSUP ? rc : -MERR_INVAL;
		frames++;
	}
	if (!frames || i != inlen)
		return -MERR_INVAL;
	*consumed = i;
	return 0;
}
