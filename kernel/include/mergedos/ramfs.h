/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_RAMFS_H
#define MERGEDOS_RAMFS_H

/*
 * ramfs: a writable file system that lives in memory. Portable code (kernel
 * and host unit tests); every path is relative to a mount root, uses '/'
 * separators and may contain "." and "..", which never climb above the root.
 * Functions return 0 / a count on success and -MERR_* on failure.
 */
#include <mergedos/compat.h>

#define RAMFS_NAME_MAX 60
#define RAMFS_FILE_MAX (64ULL << 20)

struct rnode {
	char name[RAMFS_NAME_MAX];
	bool dir;
	bool unlinked;                  /* gone from its directory, still open */
	bool symlink;                   /* data holds the link target */
	u32 mode;                       /* permission bits */
	u64 atime, mtime, ctime;        /* nanoseconds, set by the caller */
	u32 opens;
	u64 ino;
	struct rnode *parent, *child, *next;
	u8 *data;
	u64 size, cap;
};

struct rnode *ramfs_new_root(void);
void ramfs_destroy(struct rnode *root);         /* the whole tree; host tests */
int  ramfs_lookup(struct rnode *root, const char *path, struct rnode **out);
/* Create a file or directory. Existing file: returned unless `excl`. */
int  ramfs_create(struct rnode *root, const char *path, bool dir, bool excl, struct rnode **out);
int  ramfs_unlink(struct rnode *root, const char *path, bool dir);
int  ramfs_rename(struct rnode *root, const char *from, const char *to);
long ramfs_read(const struct rnode *n, u64 pos, void *buf, u64 len);
long ramfs_write(struct rnode *n, u64 pos, const void *buf, u64 len);
int  ramfs_truncate(struct rnode *n, u64 size);
/* Keep the data in page-aligned frames of at least `size` bytes (for shared mappings). */
int  ramfs_pin(struct rnode *n, u64 size);
/* The index-th entry of a directory, or NULL past the end. */
struct rnode *ramfs_entry(const struct rnode *dir, u64 index);
void ramfs_open(struct rnode *n);
void ramfs_close(struct rnode *n);              /* frees an unlinked node on last close */

#endif /* MERGEDOS_RAMFS_H */
