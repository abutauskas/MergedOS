/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_BOOT_H
#define MERGEDOS_BOOT_H

#include <mergedos/types.h>

#define MULTIBOOT1_BOOTLOADER_MAGIC 0x2BADB002u

/* Multiboot 1 information structure (fields we consume). */
struct mb1_info {
	u32 flags;
	u32 mem_lower, mem_upper;
	u32 boot_device;
	u32 cmdline;
	u32 mods_count, mods_addr;
	u32 syms[4];
	u32 mmap_length, mmap_addr;
	u32 drives_length, drives_addr;
	u32 config_table;
	u32 boot_loader_name;
	u32 apm_table;
	u32 vbe_control_info, vbe_mode_info;
	u16 vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
	u64 framebuffer_addr;
	u32 framebuffer_pitch, framebuffer_width, framebuffer_height;
	u8  framebuffer_bpp, framebuffer_type;
} MOS_PACKED;

#define MB1_INFO_CMDLINE     (1u << 2)
#define MB1_INFO_MODS        (1u << 3)
#define MB1_INFO_MMAP        (1u << 6)
#define MB1_INFO_LOADER_NAME (1u << 9)
#define MB1_INFO_FRAMEBUFFER (1u << 12)

struct mb1_mmap_entry {
	u32 size;               /* size of the rest of the entry */
	u64 addr, len;
	u32 type;               /* 1 = available RAM */
} MOS_PACKED;

/*
 * Boot information, copied out of the loader's structures into kernel-owned
 * storage before the physical allocator is allowed to hand out memory.
 */
#define BOOT_MMAP_MAX 64
#define BOOT_MEM_AVAILABLE 1

struct boot_mmap_entry {
	u64 base, len;
	u32 type;
};

#define BOOT_MODS_MAX 4

struct boot_info {
	char cmdline[256];
	char loader[48];
	u32  nmmap;
	struct boot_mmap_entry mmap[BOOT_MMAP_MAX];
	bool has_fb;
	u64  fb_addr;
	u32  fb_width, fb_height, fb_bpp, fb_pitch;
	u8   fb_type;           /* 0 indexed, 1 RGB, 2 EGA text */
	u64  mod_start, mod_end;        /* envelope of all boot modules (physical), 0 if none */
	u32  nmods;
	struct { u64 start, end; } mods[BOOT_MODS_MAX];     /* each module */
};

void boot_info_init(u32 magic, u64 mbi_phys);
const struct boot_info *boot_info_get(void);

/*
 * Kernel command line: space-separated `flag` and `key=value` words.
 * Returns true when `key` is present; copies the value (may be "") if val != NULL.
 */
bool boot_cmdline_get(const char *key, char *val, size_t cap);

#endif /* MERGEDOS_BOOT_H */
