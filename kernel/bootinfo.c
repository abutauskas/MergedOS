/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Capture Multiboot information into kernel-owned storage. The loader's
 * structures live in RAM the physical allocator is about to manage, so
 * nothing may keep a pointer into them after this runs.
 */
#include <mergedos/kernel.h>
#include <mergedos/boot.h>
#include <mergedos/mm.h>

static struct boot_info info;

static void copy_phys_string(char *dst, size_t cap, u64 phys)
{
	const char *src = phys_to_virt(phys);
	size_t i = 0;

	if (phys == 0 || phys >= DMAP_LIMIT)
		return;
	for (; i + 1 < cap && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

void boot_info_init(u32 magic, u64 mbi_phys)
{
	const struct mb1_info *mbi = phys_to_virt(mbi_phys);

	if (magic != MULTIBOOT1_BOOTLOADER_MAGIC)
		panic("not booted by a Multiboot 1 loader (magic 0x%x)", magic);

	if (mbi->flags & MB1_INFO_CMDLINE)
		copy_phys_string(info.cmdline, sizeof(info.cmdline), mbi->cmdline);
	if (mbi->flags & MB1_INFO_LOADER_NAME)
		copy_phys_string(info.loader, sizeof(info.loader), mbi->boot_loader_name);

	/* The first boot module, if any: a ustar archive of extra files (Wine's DLLs). */
	if ((mbi->flags & MB1_INFO_MODS) && mbi->mods_count) {
		const u32 *mod = phys_to_virt(mbi->mods_addr);

		if (mod[1] > mod[0]) {
			info.mod_start = mod[0];
			info.mod_end = mod[1];
		}
	}

	if (!(mbi->flags & MB1_INFO_MMAP))
		panic("boot loader did not provide a memory map");

	for (u64 off = 0; off < mbi->mmap_length && info.nmmap < BOOT_MMAP_MAX;) {
		const struct mb1_mmap_entry *e = phys_to_virt((u64)mbi->mmap_addr + off);

		info.mmap[info.nmmap].base = e->addr;
		info.mmap[info.nmmap].len = e->len;
		info.mmap[info.nmmap].type = e->type;
		info.nmmap++;
		off += e->size + sizeof(e->size);
	}

	if (mbi->flags & MB1_INFO_FRAMEBUFFER) {
		info.has_fb = true;
		info.fb_addr = mbi->framebuffer_addr;
		info.fb_pitch = mbi->framebuffer_pitch;
		info.fb_width = mbi->framebuffer_width;
		info.fb_height = mbi->framebuffer_height;
		info.fb_bpp = mbi->framebuffer_bpp;
		info.fb_type = mbi->framebuffer_type;
	}
}

const struct boot_info *boot_info_get(void)
{
	return &info;
}

bool boot_cmdline_get(const char *key, char *val, size_t cap)
{
	size_t klen = strlen(key);
	const char *p = info.cmdline;

	while (*p) {
		const char *word;
		size_t wlen;

		while (*p == ' ')
			p++;
		word = p;
		while (*p && *p != ' ')
			p++;
		wlen = (size_t)(p - word);

		if (wlen >= klen && !strncmp(word, key, klen) &&
		    (wlen == klen || word[klen] == '=')) {
			if (val && cap) {
				size_t vlen = wlen > klen ? wlen - klen - 1 : 0;

				if (vlen > cap - 1)
					vlen = cap - 1;
				memcpy(val, word + klen + 1, vlen);
				val[vlen] = '\0';
			}
			return true;
		}
	}
	return false;
}
