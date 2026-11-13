/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_TAR_H
#define MERGEDOS_TAR_H

/*
 * Read-only, zero-copy reader for POSIX ustar archives held in memory.
 * Used for the boot initrd and for the tar layers inside .deb, .apk and
 * pacman packages.
 */
#include <mergedos/compat.h>

struct tar_entry {
	char name[256];         /* normalised: no leading "./" or "/"       */
	char type;              /* '0' file, '5' directory, '2' symlink ... */
	u64 size;
	const u8 *data;         /* points into the archive buffer           */
};

/*
 * Iterate an archive. Start with *off = 0 and call until it returns 0.
 *   1  an entry was produced
 *   0  end of archive (terminator blocks, or a truncated final entry)
 *  <0  -MERR_INVAL: not a tar archive / corrupt header
 */
int tar_next(const u8 *buf, size_t len, size_t *off, struct tar_entry *e);

/* Find a regular file by its normalised path. Returns 0 or -MERR_NOENT. */
int tar_find(const u8 *buf, size_t len, const char *path, struct tar_entry *e);

/* True if `buf` starts with a plausible ustar header. */
bool tar_probe(const u8 *buf, size_t len);

#endif /* MERGEDOS_TAR_H */
