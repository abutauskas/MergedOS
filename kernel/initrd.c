/* SPDX-License-Identifier: BSD-2-Clause */
#include <mergedos/kernel.h>
#include <mergedos/initrd.h>
#include <mergedos/boot.h>
#include <mergedos/mm.h>

extern const u8 initrd_start[], initrd_end[];

/* Boot modules that are ustar archives (sysroot, NT runtime + DLLs), merged under /. */
static struct {
	const u8 *data;
	size_t len;
} mods[BOOT_MODS_MAX];
static unsigned nmods;

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

	for (unsigned i = 0; i < bi->nmods; i++) {
		const u8 *d = phys_to_virt(bi->mods[i].start);
		size_t n = bi->mods[i].end - bi->mods[i].start;

		if (!tar_probe(d, n)) {
			kprintf("[initrd] boot module %u is not a ustar archive: ignored\n", i);
			continue;
		}
		mods[nmods].data = d;
		mods[nmods].len = n;
		for (off = 0, files = 0; tar_next(d, n, &off, &e) > 0;)
			files += e.type == '0';
		kprintf("[initrd] boot module %u: %u files, %u KiB, merged under /\n", i,
			(unsigned)files, (unsigned)(n / 1024));
		nmods++;
	}
}

int initrd_find(const char *path, const u8 **data, size_t *size)
{
	struct tar_entry e;
	int rc = tar_find(initrd_start, initrd_size(), strip(path), &e);

	for (unsigned i = 0; rc && i < nmods; i++)
		rc = tar_find(mods[i].data, mods[i].len, strip(path), &e);
	if (rc)
		return rc;
	*data = e.data;
	*size = (size_t)e.size;
	return 0;
}

/* Cursors below initrd_size() walk the ramdisk; module m's cursor is (m+1) * MOD_STRIDE + offset. */
#define MOD_STRIDE (1ull << 40)
int initrd_next(size_t *cursor, struct tar_entry *e)
{
	int rc;

	if (*cursor < initrd_size()) {
		rc = tar_next(initrd_start, initrd_size(), cursor, e);
		if (rc > 0)
			return rc;
		*cursor = MOD_STRIDE;
	}
	for (;;) {
		size_t m = *cursor / MOD_STRIDE - 1, off = *cursor % MOD_STRIDE;

		if (m >= nmods)
			return 0;
		rc = tar_next(mods[m].data, mods[m].len, &off, e);
		*cursor = (m + 1) * MOD_STRIDE + off;
		if (rc > 0)
			return rc;
		*cursor = (m + 2) * MOD_STRIDE;
	}
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
