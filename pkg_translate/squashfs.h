/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_SQUASHFS_H
#define MERGEDOS_SQUASHFS_H

/*
 * Read-only squashfs 4.0 reader over an in-memory image (squashfs.c).
 * Compressors: gzip (zlib), xz, zstd, lz4. All decoding goes into
 * caller-supplied buffers; nothing is allocated.
 */
#include <mergedos/compat.h>

#define SQFS_META_MAX 8192u
#define SQFS_META_SLOTS 3

enum sqfs_kind { SQFS_DIR = 1, SQFS_FILE, SQFS_SYMLINK, SQFS_BLKDEV, SQFS_CHRDEV, SQFS_FIFO,
		 SQFS_SOCKET };

struct sqfs {
	const u8 *img;
	size_t len;                     /* image bytes, starting at the superblock */
	u32 block_size, frag_count, inode_count, mtime;
	u16 comp, flags;
	u64 root, bytes_used, inode_tab, dir_tab, frag_tab;
	struct {                        /* cache: decoded metadata blocks (directory, */
		u8 *buf;                /* inode and fragment tables in turn)        */
		u64 pos, next;
		size_t len;
		u32 used;               /* LRU stamp; 0 = empty */
	} meta[SQFS_META_SLOTS];
	u32 clock;
	void *cwork;                    /* codec work area */
	size_t cwork_len;
	u8 *tmp;                        /* spare buffer for fragment tails (may be NULL) */
	size_t tmp_len;
	u32 budget;                     /* entries left for sqfs_walk */
};

struct sqfs_inode {
	enum sqfs_kind kind;
	u16 mode;
	u32 mtime, ino;
	u64 size;                       /* file bytes, listing bytes, link length */
	u64 start;                      /* file: first data block; dir: listing block */
	u32 frag, frag_off;
	u16 dir_off;
	u64 tail_blk;                   /* metadata cursor after the fixed part */
	u32 tail_off;
};

struct sqfs_dir {
	u64 blk;
	u32 off, left, count, start;
};

/* Work area: at least SQFS_META_SLOTS * SQFS_META_MAX + the codec's work size (sqfs_work_size). */
size_t sqfs_work_size(const u8 *img, size_t len);
int sqfs_open(struct sqfs *fs, const u8 *img, size_t len, u8 *work, size_t worklen);
int sqfs_inode(struct sqfs *fs, u64 ref, struct sqfs_inode *ino);
int sqfs_lookup(struct sqfs *fs, const char *path, struct sqfs_inode *ino);
int sqfs_dir_open(struct sqfs *fs, const struct sqfs_inode *dir, struct sqfs_dir *d);
/* 1 entry, 0 end, <0 error. `name` gets the NUL-terminated entry name. */
int sqfs_dir_next(struct sqfs *fs, struct sqfs_dir *d, char *name, size_t cap, u64 *ref);
/* File contents from the start: 0 whole file, 1 truncated to `cap`, <0 error. */
int sqfs_read(struct sqfs *fs, const struct sqfs_inode *ino, u8 *out, size_t cap, size_t *got);
int sqfs_readlink(struct sqfs *fs, const struct sqfs_inode *ino, char *out, size_t cap);
/*
 * Depth-first walk; `cb` gets "dir/name" paths and returns non-zero to stop
 * (that value is returned). At most 16 levels and 4096 entries.
 */
int sqfs_walk(struct sqfs *fs, int (*cb)(const char *path, const struct sqfs_inode *ino,
					 void *arg), void *arg);
const char *sqfs_comp_name(u16 comp);
/* Offset of a squashfs image in `img`: 0, after an ELF runtime (AppImage), or -1. */
long sqfs_find(const u8 *img, size_t len);

#endif /* MERGEDOS_SQUASHFS_H */
