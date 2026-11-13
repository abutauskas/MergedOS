/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_INITRD_H
#define MERGEDOS_INITRD_H

#include <mergedos/types.h>
#include "pkg_translate/tar.h"

/*
 * Boot ramdisk: a ustar archive embedded in the kernel image and read in
 * place (zero copies, zero allocations). It stands in for a root file
 * system until the VFS milestone.
 */
void initrd_init(void);

/* Look up a regular file. Leading "/" and "./" are ignored. */
int initrd_find(const char *path, const u8 **data, size_t *size);

/* Iterate all entries: start with *cursor = 0, stop when it returns 0. */
int initrd_next(size_t *cursor, struct tar_entry *e);

/* True if `path` names a directory (i.e. some entry lives beneath it). */
bool initrd_is_dir(const char *path);

#endif /* MERGEDOS_INITRD_H */
