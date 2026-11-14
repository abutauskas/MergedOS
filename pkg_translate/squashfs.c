/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * squashfs 4.0, read only, from the on-disk format as documented with the
 * Linux kernel and squashfs-tools: superblock, metadata blocks, inode and
 * directory tables, data blocks and fragments.
 */
#include "pkg_translate/pkg_internal.h"
#include "pkg_translate/squashfs.h"

#define SQ_MAGIC     0x73717368u
#define SQ_NO_FRAG   0xFFFFFFFFu
#define SQ_RAW_BLOCK (1u << 24)

enum { C_GZIP = 1, C_LZMA, C_LZO, C_XZ, C_LZ4, C_ZSTD };

const char *sqfs_comp_name(u16 comp)
{
	static const char *const names[] = { "?", "gzip", "lzma", "lzo", "xz", "lz4", "zstd" };

	return comp < ARRAY_SIZE(names) ? names[comp] : "?";
}

/* LZ4 block format (no frame); prefix semantics like the other decoders. */
static int lz4_block(const u8 *in, size_t n, u8 *out, size_t cap, size_t *got)
{
	size_t i = 0, o = 0;

	for (;;) {
		size_t ll, ml, off, k;
		unsigned tok;
		u8 b;

		if (i >= n)
			return -1;
		tok = in[i++];
		ll = tok >> 4;
		if (ll == 15)
			do {
				if (i >= n)
					return -1;
				ll += b = in[i++];
			} while (b == 255);
		if (ll > n - i)
			return -1;
		k = ll < cap - o ? ll : cap - o;
		memcpy(out + o, in + i, k);
		o += k;
		i += ll;
		*got = o;
		if (k < ll)
			return 1;
		if (i == n)
			return 0;                       /* the last sequence has no match */
		if (n - i < 2 || !(off = rd16le(in + i)) || off > o)
			return -1;
		i += 2;
		ml = (tok & 15) + 4u;
		if ((tok & 15) == 15)
			do {
				if (i >= n)
					return -1;
				ml += b = in[i++];
			} while (b == 255);
		for (; ml; ml--, o++) {
			if (o == cap) {
				*got = o;
				return 1;
			}
			out[o] = out[o - off];
		}
		*got = o;
	}
}

static size_t codec_work(u16 comp)
{
	return comp == C_XZ ? XZ_WORK_SIZE : comp == C_ZSTD ? ZSTD_WORK_SIZE : 0;
}

/* 0 complete, 1 out filled first (prefix valid), <0 corrupt or unsupported. */
static int decomp(struct sqfs *fs, const u8 *in, size_t n, u8 *out, size_t cap, size_t *got)
{
	size_t used;

	*got = 0;
	switch (fs->comp) {
	case C_GZIP: return zlib_decode(in, n, out, cap, &used, got);
	case C_XZ:   return xz_decode(in, n, out, cap, &used, got, fs->cwork, fs->cwork_len);
	case C_ZSTD: return zstd_decode(in, n, out, cap, &used, got, fs->cwork, fs->cwork_len);
	case C_LZ4:  return lz4_block(in, n, out, cap, got);
	default:     return -MERR_NOTSUP;
	}
}

size_t sqfs_work_size(const u8 *img, size_t len)
{
	return SQFS_META_SLOTS * SQFS_META_MAX + 8 + (len >= 96 ? codec_work(rd16le(img + 20)) : 0);
}

int sqfs_open(struct sqfs *fs, const u8 *img, size_t len, u8 *work, size_t worklen)
{
	u32 blog;
	u8 *cw;

	memset(fs, 0, sizeof(*fs));
	if (len < 96 || rd32le(img) != SQ_MAGIC)
		return -MERR_NOEXEC;
	if (rd16le(img + 28) != 4)
		return -MERR_NOTSUP;
	fs->img = img;
	fs->inode_count = rd32le(img + 4);
	fs->mtime = rd32le(img + 8);
	fs->block_size = rd32le(img + 12);
	fs->frag_count = rd32le(img + 16);
	fs->comp = rd16le(img + 20);
	blog = rd16le(img + 22);
	fs->flags = rd16le(img + 24);
	fs->root = rd64le(img + 32);
	fs->bytes_used = rd64le(img + 40);
	fs->inode_tab = rd64le(img + 64);
	fs->dir_tab = rd64le(img + 72);
	fs->frag_tab = rd64le(img + 80);
	if (blog < 12 || blog > 20 || fs->block_size != 1u << blog || fs->bytes_used < 96 ||
	    fs->bytes_used > len)
		return -MERR_NOEXEC;
	fs->len = (size_t)fs->bytes_used;
	if (fs->comp != C_GZIP && fs->comp != C_XZ && fs->comp != C_ZSTD && fs->comp != C_LZ4)
		return -MERR_NOTSUP;
	if (!work || worklen < sqfs_work_size(img, len))
		return -MERR_NOMEM;
	for (int k = 0; k < SQFS_META_SLOTS; k++)
		fs->meta[k].buf = work + k * SQFS_META_MAX;
	cw = work + SQFS_META_SLOTS * SQFS_META_MAX;
	cw += (8 - ((u64)cw & 7)) & 7;
	fs->cwork = cw;
	fs->cwork_len = codec_work(fs->comp);
	fs->tmp = cw + fs->cwork_len;
	fs->tmp_len = worklen - (size_t)(fs->tmp - work);
	return 0;
}

/* Decode the metadata block at `pos` into a cache slot; returns the slot or -1. */
static int meta_block(struct sqfs *fs, u64 pos)
{
	unsigned k, old = 0;
	size_t n;
	u16 h;

	for (k = 0; k < SQFS_META_SLOTS; k++) {
		if (fs->meta[k].used && fs->meta[k].pos == pos) {
			fs->meta[k].used = ++fs->clock;
			return (int)k;
		}
		if (fs->meta[k].used < fs->meta[old].used)
			old = k;
	}
	k = old;                                        /* evict the least recently used */
	fs->meta[k].used = 0;
	if (!mos_in_range(fs->len, pos, 2))
		return -1;
	h = rd16le(fs->img + pos);
	n = h & 0x7FFF;
	if (!n || !mos_in_range(fs->len, pos + 2, n))
		return -1;
	if (h & 0x8000) {
		if (n > SQFS_META_MAX)
			return -1;
		memcpy(fs->meta[k].buf, fs->img + pos + 2, n);
		fs->meta[k].len = n;
	} else if (decomp(fs, fs->img + pos + 2, n, fs->meta[k].buf, SQFS_META_MAX,
			  &fs->meta[k].len)) {
		return -1;
	}
	fs->meta[k].pos = pos;
	fs->meta[k].next = pos + 2 + n;
	fs->meta[k].used = ++fs->clock;
	return (int)k;
}

/* Read `n` bytes of metadata at (table + *blk, *off), advancing the cursor. */
static int meta_read(struct sqfs *fs, u64 table, u64 *blk, u32 *off, void *dst, size_t n)
{
	u8 *d = dst;

	while (n) {
		int k = meta_block(fs, table + *blk);
		size_t len, c;

		if (k < 0)
			return -MERR_NOEXEC;
		len = fs->meta[k].len;
		if (*off >= len) {
			if (*off > len)
				return -MERR_NOEXEC;
			*blk = fs->meta[k].next - table;
			*off = 0;
			continue;
		}
		c = len - *off < n ? len - *off : n;
		memcpy(d, fs->meta[k].buf + *off, c);
		d += c;
		*off += (u32)c;
		n -= c;
	}
	return 0;
}

int sqfs_inode(struct sqfs *fs, u64 ref, struct sqfs_inode *ino)
{
	static const u8 fixed[15] = { 0, 16, 16, 8, 8, 8, 4, 4, 24, 40, 8, 12, 12, 8, 8 };
	u64 blk = ref >> 16;
	u32 off = ref & 0xFFFF;
	u8 h[16], b[40];
	u16 type;

	memset(ino, 0, sizeof(*ino));
	if (meta_read(fs, fs->inode_tab, &blk, &off, h, 16))
		return -MERR_NOEXEC;
	type = rd16le(h);
	if (!type || type >= ARRAY_SIZE(fixed) || meta_read(fs, fs->inode_tab, &blk, &off, b,
							     fixed[type]))
		return -MERR_NOEXEC;
	ino->kind = (enum sqfs_kind)(type > 7 ? type - 7 : type);
	ino->mode = rd16le(h + 2);
	ino->mtime = rd32le(h + 8);
	ino->ino = rd32le(h + 12);
	ino->frag = SQ_NO_FRAG;
	switch (type) {
	case 1:
		ino->start = rd32le(b);
		ino->size = rd16le(b + 8);
		ino->dir_off = rd16le(b + 10);
		break;
	case 8:
		ino->size = rd32le(b + 4);
		ino->start = rd32le(b + 8);
		ino->dir_off = rd16le(b + 18);
		break;
	case 2:
		ino->start = rd32le(b);
		ino->frag = rd32le(b + 4);
		ino->frag_off = rd32le(b + 8);
		ino->size = rd32le(b + 12);
		break;
	case 9:
		ino->start = rd64le(b);
		ino->size = rd64le(b + 8);
		ino->frag = rd32le(b + 28);
		ino->frag_off = rd32le(b + 32);
		break;
	case 3:
	case 10:
		ino->size = rd32le(b + 4);
		break;
	}
	ino->tail_blk = blk;
	ino->tail_off = off;
	return 0;
}

int sqfs_dir_open(struct sqfs *fs, const struct sqfs_inode *dir, struct sqfs_dir *d)
{
	(void)fs;
	if (dir->kind != SQFS_DIR)
		return -MERR_INVAL;
	d->blk = dir->start;
	d->off = dir->dir_off;
	d->left = dir->size > 3 ? (u32)(dir->size - 3) : 0;
	d->count = 0;
	return 0;
}

int sqfs_dir_next(struct sqfs *fs, struct sqfs_dir *d, char *name, size_t cap, u64 *ref)
{
	u8 e[12];
	size_t n;

	if (!d->count) {
		if (d->left < 12)
			return 0;
		if (meta_read(fs, fs->dir_tab, &d->blk, &d->off, e, 12))
			return -MERR_NOEXEC;
		d->left -= 12;
		d->count = rd32le(e) + 1;
		d->start = rd32le(e + 4);
		if (d->count > 256)
			return -MERR_NOEXEC;
	}
	if (d->left < 8 || meta_read(fs, fs->dir_tab, &d->blk, &d->off, e, 8))
		return -MERR_NOEXEC;
	n = rd16le(e + 6) + 1u;
	if (n >= cap)
		return -MERR_RANGE;
	if (d->left - 8 < n || meta_read(fs, fs->dir_tab, &d->blk, &d->off, name, n))
		return -MERR_NOEXEC;
	d->left -= 8 + (u32)n;
	name[n] = '\0';
	if (memchr(name, '/', n) || memchr(name, 0, n) || !strcmp(name, ".") || !strcmp(name, ".."))
		return -MERR_NOEXEC;
	*ref = (u64)d->start << 16 | rd16le(e);
	d->count--;
	return 1;
}

int sqfs_lookup(struct sqfs *fs, const char *path, struct sqfs_inode *ino)
{
	char name[256];
	int rc = sqfs_inode(fs, fs->root, ino);

	while (!rc) {
		struct sqfs_dir d;
		size_t n;
		u64 ref;

		while (*path == '/')
			path++;
		if (!*path)
			return 0;
		for (n = 0; path[n] && path[n] != '/'; n++)
			;
		if ((rc = sqfs_dir_open(fs, ino, &d)))
			return rc;
		while ((rc = sqfs_dir_next(fs, &d, name, sizeof(name), &ref)) == 1)
			if (strlen(name) == n && !memcmp(name, path, n))
				break;
		if (rc != 1)
			return rc ? rc : -MERR_NOENT;
		path += n;
		rc = sqfs_inode(fs, ref, ino);
	}
	return rc;
}

/* One block (or its first `want` bytes) of file data into dst. */
static int read_block(struct sqfs *fs, u64 pos, u32 ent, u8 *dst, size_t want)
{
	size_t dsz = ent & ~SQ_RAW_BLOCK, got;
	int rc;

	if (!dsz) {                                     /* sparse */
		memset(dst, 0, want);
		return 0;
	}
	if (!mos_in_range(fs->len, pos, dsz))
		return -MERR_NOEXEC;
	if (ent & SQ_RAW_BLOCK) {
		if (dsz < want)
			return -MERR_NOEXEC;
		memcpy(dst, fs->img + pos, want);
		return 0;
	}
	rc = decomp(fs, fs->img + pos, dsz, dst, want, &got);
	return rc >= 0 && got == want ? 0 : rc == -MERR_NOTSUP ? rc : -MERR_NOEXEC;
}

int sqfs_read(struct sqfs *fs, const struct sqfs_inode *ino, u8 *out, size_t cap, size_t *got)
{
	u64 bs = fs->block_size, size = ino->size, nblk, pos = ino->start, blk = ino->tail_blk;
	u32 off = ino->tail_off;
	size_t done = 0, tail;
	int rc;

	*got = 0;
	if (ino->kind != SQFS_FILE)
		return -MERR_INVAL;
	nblk = ino->frag == SQ_NO_FRAG ? (size + bs - 1) / bs : size / bs;
	for (u64 k = 0; k < nblk; k++) {
		u64 left = size - k * bs;
		size_t want = (size_t)(left < bs ? left : bs);
		u8 e[4];

		if (done == cap)
			return 1;
		if (want > cap - done)
			want = cap - done;
		if (meta_read(fs, fs->inode_tab, &blk, &off, e, 4))
			return -MERR_NOEXEC;
		if ((rc = read_block(fs, pos, rd32le(e), out + done, want)))
			return rc;
		pos += rd32le(e) & ~SQ_RAW_BLOCK;
		done += want;
		*got = done;
	}
	tail = ino->frag == SQ_NO_FRAG ? 0 : (size_t)(size - nblk * bs);
	if (tail) {
		u64 fpos, p = fs->frag_tab + (u64)(ino->frag / 512) * 8;
		u32 foff = (ino->frag % 512) * 16;
		u8 e[16], *buf;
		size_t k = tail < cap - done ? tail : cap - done;

		if (!k)
			return 1;
		if (ino->frag >= fs->frag_count || tail >= bs || !mos_in_range(fs->len, p, 8))
			return -MERR_NOEXEC;
		fpos = rd64le(fs->img + p);
		if (meta_read(fs, 0, &fpos, &foff, e, 16))
			return -MERR_NOEXEC;
		if ((u64)ino->frag_off + k <= cap - done)
			buf = out + done;
		else if ((u64)ino->frag_off + k <= fs->tmp_len)
			buf = fs->tmp;
		else
			return -MERR_NOMEM;
		if ((rc = read_block(fs, rd64le(e), rd32le(e + 8), buf, ino->frag_off + k)))
			return rc;
		memmove(out + done, buf + ino->frag_off, k);
		done += k;
		*got = done;
	}
	return done < size ? 1 : 0;
}

int sqfs_readlink(struct sqfs *fs, const struct sqfs_inode *ino, char *out, size_t cap)
{
	u64 blk = ino->tail_blk;
	u32 off = ino->tail_off;

	if (ino->kind != SQFS_SYMLINK)
		return -MERR_INVAL;
	if (ino->size >= cap)
		return -MERR_RANGE;
	if (meta_read(fs, fs->inode_tab, &blk, &off, out, (size_t)ino->size))
		return -MERR_NOEXEC;
	out[ino->size] = '\0';
	return (int)ino->size;
}

struct walk {
	struct sqfs *fs;
	int (*cb)(const char *path, const struct sqfs_inode *ino, void *arg);
	void *arg;
	char path[256];
};

static int walk_dir(struct walk *w, const struct sqfs_inode *dir, size_t plen, int depth)
{
	struct sqfs_dir d;
	struct sqfs_inode ino;
	int rc;

	if (depth > 16)
		return -MERR_RANGE;
	if ((rc = sqfs_dir_open(w->fs, dir, &d)))
		return rc;
	for (;;) {
		size_t base = plen ? plen + 1 : 0;
		u64 ref;

		if (base >= sizeof(w->path) - 1)
			return -MERR_RANGE;
		rc = sqfs_dir_next(w->fs, &d, w->path + base, sizeof(w->path) - base, &ref);
		if (rc <= 0) {
			w->path[plen] = '\0';
			return rc;
		}
		if (plen)
			w->path[plen] = '/';
		if (!w->fs->budget)
			return -MERR_RANGE;
		w->fs->budget--;
		if ((rc = sqfs_inode(w->fs, ref, &ino)) || (rc = w->cb(w->path, &ino, w->arg)))
			return rc;
		if (ino.kind == SQFS_DIR && (rc = walk_dir(w, &ino, strlen(w->path), depth + 1)))
			return rc;
		w->path[plen] = '\0';
	}
}

int sqfs_walk(struct sqfs *fs, int (*cb)(const char *path, const struct sqfs_inode *ino,
					 void *arg), void *arg)
{
	struct walk w = { .fs = fs, .cb = cb, .arg = arg };
	struct sqfs_inode root;
	int rc = sqfs_inode(fs, fs->root, &root);

	fs->budget = 4096;
	return rc ? rc : walk_dir(&w, &root, 0, 0);
}

static bool sq_super(const u8 *p)
{
	return rd32le(p) == SQ_MAGIC && rd16le(p + 28) == 4 && rd16le(p + 22) <= 20 &&
	       rd32le(p + 12) == 1u << rd16le(p + 22);
}

long sqfs_find(const u8 *img, size_t len)
{
	u64 end, phoff;

	if (len >= 96 && sq_super(img))
		return 0;
	if (len < 64 || memcmp(img, "\177ELF", 4) || img[4] != 2)
		return -1;
	/* AppImage type 2: the image starts where the ELF runtime ends. */
	end = rd64le(img + 40) + (u64)rd16le(img + 58) * rd16le(img + 60);
	phoff = rd64le(img + 32);
	for (u64 k = 0; k < rd16le(img + 56); k++) {
		u64 ph = phoff + k * rd16le(img + 54);

		if (mos_in_range(len, ph, 56) && rd64le(img + ph + 8) + rd64le(img + ph + 32) > end)
			end = rd64le(img + ph + 8) + rd64le(img + ph + 32);
	}
	if (mos_in_range(len, end, 96) && sq_super(img + end))
		return (long)end;
	for (size_t o = 64; o + 96 <= len; o++)
		if (sq_super(img + o))
			return (long)o;
	return -1;
}
