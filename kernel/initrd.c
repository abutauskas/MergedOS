/* SPDX-License-Identifier: BSD-2-Clause */
#include <mergedos/kernel.h>
#include <mergedos/initrd.h>
#include <mergedos/boot.h>
#include <mergedos/mm.h>

extern const u8 initrd_start[], initrd_end[];

/*
 * An optional second archive passed by the boot loader as a Multiboot
 * module (Wine's DLLs, see `make wine`). It is searched after the ramdisk
 * and read in place like it; the kernel never runs anything from it.
 */
static const u8 *mod_data;
static size_t mod_size;
#define MOD_CURSOR (1ULL << 62)        /* initrd_next(): cursor is in the module */

static size_t initrd_size(void)
{
	return (size_t)(initrd_end - initrd_start);
}

static const char *strip(const char *path)
{
	while (path[0] == '.' && path[1] == '/')
		path += 2;
	while (*path == '/')
		path++;
	return path;
}

void initrd_init(void)
{
	struct tar_entry e;
	size_t off = 0, files = 0;

	if (!tar_probe(initrd_start, initrd_size()))
		panic("initrd is not a ustar archive");
	while (tar_next(initrd_start, initrd_size(), &off, &e) > 0)
		if (e.type == '0')
			files++;
	kprintf("[initrd] %u files, %u KiB (ustar, mapped in place)\n", (unsigned)files,
		(unsigned)(initrd_size() / 1024));
	{
		const struct boot_info *bi = boot_info_get();

		if (bi->mod_end && tar_probe(phys_to_virt(bi->mod_start), bi->mod_end - bi->mod_start)) {
			mod_data = phys_to_virt(bi->mod_start);
			mod_size = bi->mod_end - bi->mod_start;
			kprintf("[initrd] boot module: %u KiB (ustar, searched after the ramdisk)\n",
				(unsigned)(mod_size / 1024));
		} else if (bi->mod_end) {
			kprintf("[initrd] boot module ignored: not a ustar archive\n");
		}
	}
}

int initrd_find(const char *path, const u8 **data, size_t *size)
{
	struct tar_entry e;
	int rc = tar_find(initrd_start, initrd_size(), strip(path), &e);

	if (rc && mod_data)
		rc = tar_find(mod_data, mod_size, strip(path), &e);
	if (rc)
		return rc;
	*data = e.data;
	*size = (size_t)e.size;
	return 0;
}

int initrd_next(size_t *cursor, struct tar_entry *e)
{
	size_t off;
	int rc;

	if (!(*cursor & MOD_CURSOR)) {
		rc = tar_next(initrd_start, initrd_size(), cursor, e);
		if (rc > 0 || !mod_data)
			return rc;
		*cursor = MOD_CURSOR;
	}
	off = *cursor & ~MOD_CURSOR;
	rc = tar_next(mod_data, mod_size, &off, e);
	*cursor = off | MOD_CURSOR;
	return rc;
}

bool initrd_is_dir(const char *path)
{
	struct tar_entry e;
	size_t off = 0, n;

	path = strip(path);
	n = strlen(path);
	while (n && path[n - 1] == '/')
		n--;
	if (n == 0)
		return true;                    /* the root */
	while (initrd_next(&off, &e) > 0)
		if (!strncmp(e.name, path, n) && (e.name[n] == '/' || e.name[n] == '\0') &&
		    (e.type == '5' || e.name[n] == '/'))
			return true;
	return false;
}
