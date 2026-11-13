/* SPDX-License-Identifier: BSD-2-Clause */
#include "storage/storage.h"

#define MBR_TABLE     446
#define MBR_ENTRY     16
#define MBR_MAX_LBA   0xFFFFFFFFULL
#define GPT_ENTRY     128

u32 mos_crc32(u32 crc, const void *data, size_t n)
{
	const u8 *p = data;

	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

static int rd(struct block_dev *dev, u64 lba, void *buf)
{
	if (lba >= dev->sectors)
		return -MERR_RANGE;
	return dev->read_sectors(dev, lba, 1, buf);
}

static bool all_zero(const u8 *p, size_t n)
{
	while (n--)
		if (*p++)
			return false;
	return true;
}

/* Identify what starts at `lba` from on-disk signatures. Read-only. */
static enum fs_kind detect_fs(struct block_dev *dev, u64 lba)
{
	u8 s[SECTOR_SIZE];

	if (rd(dev, lba, s))
		return FS_NONE;
	if (!memcmp(s, "MRGDCFG1", 8))
		return FS_MERGEDOS;
	if (!memcmp(s + 3, "NTFS    ", 8))
		return FS_NTFS;
	if (!memcmp(s + 3, "-FVE-FS-", 8))
		return FS_BITLOCKER;
	if (!memcmp(s + 3, "EXFAT   ", 8))
		return FS_EXFAT;
	if (!memcmp(s + 0x36, "FAT", 3) || !memcmp(s + 0x52, "FAT32", 5))
		return FS_FAT;
	if (!memcmp(s, "XFSB", 4))
		return FS_XFS;
	if (!memcmp(s, "LUKS\xBA\xBE", 6))
		return FS_LUKS;
	if (!rd(dev, lba + 2, s) && s[56] == 0x53 && s[57] == 0xEF)
		return FS_EXT;                  /* superblock magic at byte 1080 */
	if (!rd(dev, lba + 7, s) && !memcmp(s + 502, "SWAPSPACE2", 10))
		return FS_SWAP;
	if (!rd(dev, lba + 128, s) && !memcmp(s + 0x40, "_BHRfS_M", 8))
		return FS_BTRFS;
	return FS_NONE;
}

const char *fs_name(enum fs_kind fs)
{
	static const char *const names[] = {
		"unknown data", "Windows NTFS", "Windows BitLocker", "FAT", "exFAT", "Linux ext",
		"Linux swap", "Linux LUKS", "Linux XFS", "Linux Btrfs", "MergedOS",
	};

	return names[fs];
}

/* Record a partition; flags the table inconsistent on any impossible geometry. */
static void add_part(struct block_dev *dev, struct disk_layout *l, u64 start, u64 sectors, u8 type)
{
	struct disk_part *p;

	if (!start || !sectors || start >= l->sectors || sectors > l->sectors - start ||
	    l->nparts == DISK_MAX_PARTS) {
		l->consistent = false;
		return;
	}
	for (u32 i = 0; i < l->nparts; i++)
		if (start < l->part[i].start + l->part[i].sectors &&
		    l->part[i].start < start + sectors)
			l->consistent = false;          /* overlap */
	p = &l->part[l->nparts++];
	p->start = start;
	p->sectors = sectors;
	p->mbr_type = type;
	p->fs = detect_fs(dev, start);
}

/* Partition type GUID for MergedOS (on-disk byte order). */
static const u8 gpt_mergedos_type[16] = {
	0x44, 0x47, 0x52, 0x4D, 0x53, 0x4F, 0x49, 0x4C, 0x8E, 0x72, 0x9A, 0x1F, 0x6B, 0x2C, 0x3D, 0x4E,
};

static void put32(u8 *p, u32 v)
{
	for (int i = 0; i < 4; i++)
		p[i] = (u8)(v >> (8 * i));
}

static void put64(u8 *p, u64 v)
{
	for (int i = 0; i < 8; i++)
		p[i] = (u8)(v >> (8 * i));
}

static u32 gpt_header_crc(const u8 *h)
{
	u8 tmp[SECTOR_SIZE];

	memcpy(tmp, h, SECTOR_SIZE);
	memset(tmp + 16, 0, 4);                 /* the CRC field counts as zero */
	return mos_crc32(0, tmp, rd32le(h + 12));
}

/* A header copy is usable if it is self-consistent and says where it is. */
static bool gpt_header_ok(const u8 *h, u64 self, u64 other, u64 sectors)
{
	u32 hsize = rd32le(h + 12), n = rd32le(h + 80);

	return !memcmp(h, "EFI PART", 8) && hsize >= 92 && hsize <= SECTOR_SIZE &&
	       gpt_header_crc(h) == rd32le(h + 16) && rd64le(h + 24) == self &&
	       rd64le(h + 32) == other && rd32le(h + 84) == GPT_ENTRY && n >= 1 && n <= 1024 &&
	       rd64le(h + 40) <= rd64le(h + 48) && rd64le(h + 48) < sectors;
}

/* CRC of an entry array. With `l` set, also records partitions and the first free slot. */
static bool gpt_array_crc(struct block_dev *dev, u64 table, u32 n, u32 *crc_out,
			  struct disk_layout *l)
{
	u8 s[SECTOR_SIZE];
	u32 crc = 0;

	for (u32 i = 0; i < n; i++) {
		const u8 *e = s + (i % 4) * GPT_ENTRY;

		if (i % 4 == 0 && rd(dev, table + i / 4, s))
			return false;
		crc = mos_crc32(crc, e, GPT_ENTRY);
		if (!l)
			continue;
		if (all_zero(e, GPT_ENTRY)) {
			if (l->gpt.free_slot < 0)
				l->gpt.free_slot = (int)i;
		} else if (!all_zero(e, 16)) {  /* type GUID set: entry in use */
			add_part(dev, l, rd64le(e + 32), rd64le(e + 40) - rd64le(e + 32) + 1,
				 memcmp(e, gpt_mergedos_type, 16) ? 0 : MBR_TYPE_MERGEDOS);
		}
	}
	*crc_out = crc;
	return true;
}

static void scan_gpt(struct block_dev *dev, struct disk_layout *l)
{
	struct gpt_info *g = &l->gpt;
	u8 *h = g->header, *b = g->backup;
	u32 crc = 0, bcrc = 0, array_sectors;

	l->scheme = DISK_GPT;
	g->free_slot = -1;
	if (rd(dev, 1, h) || !gpt_header_ok(h, 1, rd64le(h + 32), l->sectors)) {
		l->consistent = false;
		return;
	}
	g->backup_lba = rd64le(h + 32);
	g->first_usable = rd64le(h + 40);
	g->last_usable = rd64le(h + 48);
	g->entries_lba = rd64le(h + 72);
	g->nentries = rd32le(h + 80);
	array_sectors = (g->nentries + 3) / 4;
	/* The primary array must sit between the header and the first usable sector. */
	if (g->entries_lba < 2 || g->entries_lba + array_sectors > g->first_usable ||
	    !gpt_array_crc(dev, g->entries_lba, g->nentries, &crc, l) || crc != rd32le(h + 88)) {
		l->consistent = false;
		return;
	}
	/*
	 * The backup copy: valid on its own, pointing back at the primary,
	 * describing the same disk, with a byte-identical entry array placed
	 * after the last usable sector.
	 */
	g->backup_ok = g->backup_lba < l->sectors && g->backup_lba > g->last_usable &&
		       !rd(dev, g->backup_lba, b) &&
		       gpt_header_ok(b, g->backup_lba, 1, l->sectors) &&
		       !memcmp(h + 40, b + 40, 32) &&   /* usable range + disk GUID */
		       rd32le(b + 80) == g->nentries && rd32le(b + 88) == crc &&
		       rd64le(b + 72) > g->last_usable &&
		       rd64le(b + 72) + array_sectors <= g->backup_lba &&
		       gpt_array_crc(dev, rd64le(b + 72), g->nentries, &bcrc, NULL) && bcrc == crc;
	g->backup_entries_lba = rd64le(b + 72);
}

int disk_scan(struct block_dev *dev, struct disk_layout *l)
{
	u8 s[SECTOR_SIZE];

	memset(l, 0, sizeof(*l));
	l->sectors = dev->sectors;
	l->consistent = true;
	if (rd(dev, 0, l->mbr))
		return -MERR_IO;

	if (l->mbr[510] != 0x55 || l->mbr[511] != 0xAA) {
		/* No table. Blank only if the first MiB and the GPT backup area are zero. */
		u64 head = MOS_MIN(dev->sectors, (u64)DISK_ALIGN);

		l->scheme = DISK_BLANK;
		for (u64 lba = 0; lba < dev->sectors; lba++) {
			if (lba == head) {
				if (dev->sectors < head + 34)
					break;
				lba = dev->sectors - 34;
			}
			if (rd(dev, lba, s))
				return -MERR_IO;
			if (!all_zero(s, sizeof(s))) {
				l->scheme = DISK_UNKNOWN;
				break;
			}
		}
		return 0;
	}
	/* 0x55AA also ends a FAT/NTFS boot sector written straight to the disk. */
	if (detect_fs(dev, 0) != FS_NONE) {
		l->scheme = DISK_UNKNOWN;
		return 0;
	}
	l->scheme = DISK_MBR;
	for (int i = 0; i < 4; i++)
		if (l->mbr[MBR_TABLE + i * MBR_ENTRY + 4] == 0xEE) {
			scan_gpt(dev, l);
			return 0;
		}
	for (int i = 0; i < 4; i++) {
		const u8 *e = l->mbr + MBR_TABLE + i * MBR_ENTRY;

		if (e[4] || rd32le(e + 12))
			add_part(dev, l, rd32le(e + 8), rd32le(e + 12), e[4]);
	}
	return 0;
}

static bool overlaps_any(const struct disk_layout *l, u64 start, u64 sectors)
{
	for (u32 i = 0; i < l->nparts; i++)
		if (start < l->part[i].start + l->part[i].sectors &&
		    l->part[i].start < start + sectors)
			return true;
	return false;
}

int disk_plan(struct block_dev *dev, const struct disk_layout *l, struct disk_plan *p)
{
	u64 limit = MOS_MIN(l->sectors, MBR_MAX_LBA), cursor = DISK_ALIGN;

	memset(p, 0, sizeof(*p));
	p->mbr_slot = -1;
	p->gpt_slot = -1;
	if (l->scheme == DISK_UNKNOWN)
		return -MERR_NOTSUP;
	if (!l->consistent)
		return -MERR_INVAL;

	if (l->scheme == DISK_GPT) {
		if (!l->gpt.backup_ok)
			return -MERR_INVAL;             /* never write with only one good copy */
		if (l->gpt.free_slot < 0)
			return -MERR_RANGE;
		p->gpt = true;
		p->gpt_slot = l->gpt.free_slot;
		cursor = ALIGN_UP(MOS_MAX(l->gpt.first_usable, (u64)DISK_ALIGN), DISK_ALIGN);
		limit = l->gpt.last_usable + 1;
	} else if (l->scheme == DISK_BLANK) {
		p->blank = true;
		p->mbr_slot = 0;
	} else {
		for (int i = 3; i >= 0; i--)
			if (all_zero(l->mbr + MBR_TABLE + i * MBR_ENTRY, MBR_ENTRY))
				p->mbr_slot = i;
		if (p->mbr_slot < 0)
			return -MERR_RANGE;
	}

	/* Largest aligned gap: sweep upward, jumping over whichever partition is in the way. */
	while (cursor < limit) {
		u64 end = limit, next = limit;

		for (u32 i = 0; i < l->nparts; i++) {
			u64 ps = l->part[i].start, pe = ps + l->part[i].sectors;

			if (ps <= cursor && cursor < pe) {
				end = cursor;           /* inside a partition */
				next = MOS_MIN(next, ALIGN_UP(pe, DISK_ALIGN));
			} else if (ps > cursor && ps < end) {
				end = ps;
				next = ALIGN_UP(pe, DISK_ALIGN);
			}
		}
		end = ALIGN_DOWN(end, DISK_ALIGN);
		if (end > cursor && end - cursor > p->sectors) {
			p->start = cursor;
			p->sectors = end - cursor;
		}
		if (next <= cursor)
			break;
		cursor = next;
	}
	if (p->sectors < DISK_MIN_INSTALL)
		return -MERR_RANGE;
	/* "Unallocated" must also mean "nothing recognisable lives there". */
	if (detect_fs(dev, p->start) != FS_NONE)
		return -MERR_ACCES;
	return 0;
}

/* Write one sector and read it back. */
static int put_sector(struct block_dev *dev, u64 lba, const u8 *buf)
{
	u8 back[SECTOR_SIZE];

	if (dev->write_sectors(dev, lba, 1, buf) || rd(dev, lba, back) ||
	    memcmp(back, buf, SECTOR_SIZE))
		return -MERR_IO;
	return 0;
}

/*
 * Add one GPT entry. Only an all-zero slot is ever filled; existing entries
 * are carried through byte for byte because the sector that holds the slot
 * is re-read here and only the slot's 128 bytes are changed.
 *
 * Order: data sector, backup array, backup header, primary array, primary
 * header. At every point at least one copy of the table is complete and
 * valid: the old primary until the last two writes, the new backup after.
 */
static int gpt_apply(struct block_dev *dev, const struct disk_layout *l,
		     const struct disk_plan *p, const u8 first_sector[SECTOR_SIZE])
{
	static const char name[] = "MergedOS";
	const struct gpt_info *g = &l->gpt;
	u8 cur[SECTOR_SIZE], pri[SECTOR_SIZE], bak[SECTOR_SIZE], hdr[SECTOR_SIZE];
	u32 crc = 0, slot = (u32)p->gpt_slot;
	u64 sec = slot / 4;
	u8 *e = pri + (slot % 4) * GPT_ENTRY;

	if (!g->backup_ok || p->gpt_slot < 0 || slot >= g->nentries || p->start < g->first_usable ||
	    p->start + p->sectors - 1 > g->last_usable)
		return -MERR_INVAL;
	/* Both headers exactly as scanned, both arrays agreeing on an empty slot. */
	if (rd(dev, 1, cur) || memcmp(cur, g->header, SECTOR_SIZE) || rd(dev, g->backup_lba, cur) ||
	    memcmp(cur, g->backup, SECTOR_SIZE))
		return -MERR_AGAIN;
	if (rd(dev, g->entries_lba + sec, pri) || rd(dev, g->backup_entries_lba + sec, bak))
		return -MERR_IO;
	if (memcmp(pri, bak, SECTOR_SIZE) || !all_zero(e, GPT_ENTRY))
		return -MERR_INVAL;

	memcpy(e, gpt_mergedos_type, 16);
	for (int i = 0; i < 4; i++)             /* unique GUID: derived, version-4 shaped */
		put32(e + 16 + 4 * i, mos_crc32((u32)i, g->header, SECTOR_SIZE) ^
					      (u32)(p->start * 2654435761u));
	e[16 + 7] = (u8)((e[16 + 7] & 0x0F) | 0x40);
	e[16 + 8] = (u8)((e[16 + 8] & 0x3F) | 0x80);
	put64(e + 32, p->start);
	put64(e + 40, p->start + p->sectors - 1);
	for (size_t i = 0; i < sizeof(name) - 1; i++)
		e[56 + 2 * i] = (u8)name[i];    /* UTF-16LE */

	/* New array CRC: every sector as it is on disk, except the one changed above. */
	for (u32 i = 0; i < g->nentries; i++) {
		if (i % 4 == 0 && i / 4 != sec && rd(dev, g->entries_lba + i / 4, cur))
			return -MERR_IO;
		crc = mos_crc32(crc, (i / 4 == sec ? pri : cur) + (i % 4) * GPT_ENTRY, GPT_ENTRY);
	}

	if (put_sector(dev, p->start, first_sector) ||
	    put_sector(dev, g->backup_entries_lba + sec, pri))
		return -MERR_IO;
	memcpy(hdr, g->backup, SECTOR_SIZE);
	put32(hdr + 88, crc);
	put32(hdr + 16, gpt_header_crc(hdr));
	if (put_sector(dev, g->backup_lba, hdr) || put_sector(dev, g->entries_lba + sec, pri))
		return -MERR_IO;
	memcpy(hdr, g->header, SECTOR_SIZE);
	put32(hdr + 88, crc);
	put32(hdr + 16, gpt_header_crc(hdr));
	return put_sector(dev, 1, hdr);
}

int disk_apply(struct block_dev *dev, const struct disk_layout *l, const struct disk_plan *p,
	       const u8 first_sector[SECTOR_SIZE])
{
	u8 cur[SECTOR_SIZE], mbr[SECTOR_SIZE], back[SECTOR_SIZE];
	u8 *e;

	if (p->gpt) {
		if (l->scheme != DISK_GPT || !l->consistent || p->start < DISK_ALIGN ||
		    p->start % DISK_ALIGN || p->sectors < DISK_MIN_INSTALL ||
		    p->sectors > dev->sectors || p->start > dev->sectors - p->sectors ||
		    overlaps_any(l, p->start, p->sectors))
			return -MERR_INVAL;
		if (detect_fs(dev, p->start) != FS_NONE)
			return -MERR_ACCES;
		return gpt_apply(dev, l, p, first_sector);
	}

	/* Re-derive every safety property here; never trust the plan alone. */
	if (l->scheme != (p->blank ? DISK_BLANK : DISK_MBR) || !l->consistent ||
	    p->mbr_slot < 0 || p->mbr_slot > 3 || p->start < DISK_ALIGN ||
	    p->start % DISK_ALIGN || p->sectors < DISK_MIN_INSTALL ||
	    p->start + p->sectors > MOS_MIN(dev->sectors, MBR_MAX_LBA) ||
	    overlaps_any(l, p->start, p->sectors))
		return -MERR_INVAL;
	if (rd(dev, 0, cur))
		return -MERR_IO;
	if (memcmp(cur, l->mbr, SECTOR_SIZE))
		return -MERR_AGAIN;                     /* disk changed since the scan */
	e = mbr + MBR_TABLE + p->mbr_slot * MBR_ENTRY;
	memcpy(mbr, cur, SECTOR_SIZE);
	if (!all_zero(e, MBR_ENTRY) || (p->blank && !all_zero(cur, SECTOR_SIZE)))
		return -MERR_INVAL;

	if (detect_fs(dev, p->start) != FS_NONE)
		return -MERR_ACCES;                     /* the "free" space is not empty */

	/* 1. Data first: until the table points here, this sector is still free space. */
	if (dev->write_sectors(dev, p->start, 1, first_sector) || rd(dev, p->start, back) ||
	    memcmp(back, first_sector, SECTOR_SIZE))
		return -MERR_IO;

	/* 2. One table entry. Boot code, disk signature and other entries stay byte-identical. */
	e[0] = 0x00;
	e[1] = e[5] = 0xFE;                             /* CHS fields: "use LBA" */
	e[2] = e[3] = e[6] = e[7] = 0xFF;
	e[4] = MBR_TYPE_MERGEDOS;
	for (int i = 0; i < 4; i++) {
		e[8 + i] = (u8)(p->start >> (8 * i));
		e[12 + i] = (u8)(p->sectors >> (8 * i));
	}
	mbr[510] = 0x55;
	mbr[511] = 0xAA;
	if (dev->write_sectors(dev, 0, 1, mbr) || rd(dev, 0, back) || memcmp(back, mbr, SECTOR_SIZE))
		return -MERR_IO;
	return 0;
}

int disk_find_mergedos(struct block_dev *dev, const struct disk_layout *l,
		       u8 first_sector[SECTOR_SIZE])
{
	for (u32 i = 0; i < l->nparts; i++)
		if (l->part[i].mbr_type == MBR_TYPE_MERGEDOS && l->part[i].fs == FS_MERGEDOS)
			return rd(dev, l->part[i].start, first_sector) ? -MERR_IO : 0;
	return -MERR_NOENT;
}

void disk_describe(const struct disk_layout *l, char *buf, size_t cap)
{
	static const char *const schemes[] = { "blank", "MBR", "GPT", "no partition table" };
	u64 used = 0;

	mos_snprintf(buf, cap, "%llu MiB, %s%s", (unsigned long long)(l->sectors / 2048),
		     schemes[l->scheme], l->consistent ? "" : " (damaged)");
	for (u32 i = 0; i < l->nparts; i++) {
		char item[48];

		mos_snprintf(item, sizeof(item), "%s %s %llu MiB", i ? "," : ":",
			     fs_name(l->part[i].fs), (unsigned long long)(l->part[i].sectors / 2048));
		mos_strlcat(buf, item, cap);
		used += l->part[i].sectors;
	}
	if (l->nparts) {
		char item[48];

		mos_snprintf(item, sizeof(item), ", %llu MiB unallocated",
			     (unsigned long long)((l->sectors - MOS_MIN(used, l->sectors)) / 2048));
		mos_strlcat(buf, item, cap);
	}
}
