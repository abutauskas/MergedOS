/* SPDX-License-Identifier: BSD-2-Clause */
#include <mergedos/kernel.h>
#include <mergedos/initrd.h>
#include <mergedos/boot.h>
#include <mergedos/mm.h>

extern const u8 initrd_start[], initrd_end[];

/* The sysroot: a second ustar archive passed as a boot module, merged under /. */
static const u8 *sysroot;
static size_t sysroot_len;

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

	const struct boot_info *bi = boot_info_get();

	if (bi->mod_end && tar_probe(phys_to_virt(bi->mod_start), bi->mod_end - bi->mod_start)) {
		sysroot = phys_to_virt(bi->mod_start);
		sysroot_len = bi->mod_end - bi->mod_start;
		for (off = 0, files = 0; tar_next(sysroot, sysroot_len, &off, &e) > 0;)
			files += e.type == '0';
		kprintf("[initrd] sysroot module: %u files, %u KiB, merged under /\n",
			(unsigned)files, (unsigned)(sysroot_len / 1024));
	} else if (bi->mod_end) {
		kprintf("[initrd] boot module is not a ustar archive: ignored\n");
	}
}

int initrd_find(const char *path, const u8 **data, size_t *size)
{
	struct tar_entry e;
	int rc = tar_find(initrd_start, initrd_size(), strip(path), &e);

	if (rc && sysroot)
		rc = tar_find(sysroot, sysroot_len, strip(path), &e);
	if (rc)
		return rc;
	*data = e.data;
	*size = (size_t)e.size;
	return 0;
}

/* Cursors below initrd_size() walk the ramdisk, the ones above it the sysroot. */
int initrd_next(size_t *cursor, struct tar_entry *e)
{
	size_t off;
	int rc;

	if (*cursor < initrd_size()) {
		rc = tar_next(initrd_start, initrd_size(), cursor, e);
		if (rc > 0 || !sysroot)
			return rc;
		*cursor = initrd_size();
	}
	if (!sysroot)
		return 0;
	off = *cursor - initrd_size();
	rc = tar_next(sysroot, sysroot_len, &off, e);
	*cursor = initrd_size() + off;
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
