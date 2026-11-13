/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_STORAGE_H
#define MERGEDOS_STORAGE_H

/*
 * Disk layout inspection and *additive-only* partitioning.
 *
 * Safety contract (enforced in disk.c, checked by tests/test_storage.c):
 *   - Nothing here can resize, format, move or delete a partition.
 *   - On MBR disks the only writes are: one first sector inside space proven
 *     to be unallocated, and sector 0 with exactly one empty entry filled in.
 *   - On GPT disks: one data sector, then one entry in each copy of the
 *     entry array and the two header CRC fields, backup copy first.
 *   - Anything not fully understood (damaged or overlapping tables, a GPT
 *     whose two copies disagree, a file system without a table, data where
 *     free space should be) is refused, not guessed at.
 *
 * Drivers plug in through struct block_dev (AHCI and ATA PIO today; NVMe later).
 */
#include <mergedos/compat.h>

#define SECTOR_SIZE          512
#define MBR_TYPE_MERGEDOS    0x7F       /* MBR type reserved for experimental systems */
#define DISK_MAX_PARTS       16
#define DISK_ALIGN           2048       /* partitions start on 1 MiB boundaries */
#define DISK_MIN_INSTALL     (16 * 2048)        /* 16 MiB */

struct block_dev {
	const char *name;
	u64 sectors;
	void *ctx;
	int (*read_sectors)(struct block_dev *dev, u64 lba, u32 count, void *buf);
	int (*write_sectors)(struct block_dev *dev, u64 lba, u32 count, const void *buf);
};

enum disk_scheme {
	DISK_BLANK,             /* first MiB and tail are all zero            */
	DISK_MBR,
	DISK_GPT,
	DISK_UNKNOWN            /* holds data, but no partition table we read */
};

enum fs_kind {
	FS_NONE = 0, FS_NTFS, FS_BITLOCKER, FS_FAT, FS_EXFAT, FS_EXT, FS_SWAP, FS_LUKS, FS_XFS,
	FS_BTRFS, FS_MERGEDOS
};

struct disk_part {
	u64 start, sectors;
	u8 mbr_type;            /* MBR type; GPT: 0x7F for the MergedOS type GUID, else 0 */
	enum fs_kind fs;
};

/* GPT bookkeeping kept from the scan so that a write can prove nothing moved. */
struct gpt_info {
	u64 first_usable, last_usable;
	u64 entries_lba, backup_lba, backup_entries_lba;
	u32 nentries;
	int free_slot;          /* first all-zero entry, or -1 */
	bool backup_ok;         /* backup header + array valid and equal to the primary */
	u8 header[SECTOR_SIZE], backup[SECTOR_SIZE];
};

struct disk_layout {
	enum disk_scheme scheme;
	u64 sectors;
	bool consistent;        /* table passed every sanity check */
	u32 nparts;
	struct disk_part part[DISK_MAX_PARTS];
	u8 mbr[SECTOR_SIZE];    /* sector 0 exactly as scanned */
	struct gpt_info gpt;
};

struct disk_plan {
	bool blank;             /* true: write a fresh MBR on an empty disk */
	bool gpt;               /* true: add a GPT entry (gpt_slot) */
	int mbr_slot, gpt_slot;
	u64 start, sectors;
};

int disk_scan(struct block_dev *dev, struct disk_layout *l);

/*
 * Find room for MergedOS. 0, or:
 *   -MERR_NOTSUP  no partition table we understand
 *   -MERR_INVAL   partition table is inconsistent, or the GPT backup copy is not valid
 *   -MERR_RANGE   no free table entry or no gap of DISK_MIN_INSTALL
 *   -MERR_ACCES   the gap is not empty (a file system signature is there)
 */
int disk_plan(struct block_dev *dev, const struct disk_layout *l, struct disk_plan *p);

/* Carry out a plan: write `first_sector` at p->start, then add the table entry. */
int disk_apply(struct block_dev *dev, const struct disk_layout *l, const struct disk_plan *p,
	       const u8 first_sector[SECTOR_SIZE]);

/* Read the first sector of the MergedOS partition, if the disk has one. */
int disk_find_mergedos(struct block_dev *dev, const struct disk_layout *l,
		       u8 first_sector[SECTOR_SIZE]);

void disk_describe(const struct disk_layout *l, char *buf, size_t cap);
const char *fs_name(enum fs_kind fs);
u32 mos_crc32(u32 crc, const void *data, size_t n);

#endif /* MERGEDOS_STORAGE_H */
