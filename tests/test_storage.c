/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Storage safety tests. The central claim under test: installing never
 * changes a byte that belongs to an existing partition, to the boot code,
 * or to another partition's table entry.
 */
#include "tests/test.h"
#include "installer/installer.h"
#include "storage/storage.h"

#define MIB 2048u                       /* sectors per MiB */

struct ram_disk {
	u8 *data;
	int writes;
	int fail_write;                 /* fail the Nth write (1-based); 0 = never */
	struct block_dev dev;
};

static int ram_read(struct block_dev *dev, u64 lba, u32 count, void *buf)
{
	struct ram_disk *r = dev->ctx;

	if (lba + count > dev->sectors)
		return -MERR_IO;
	memcpy(buf, r->data + lba * SECTOR_SIZE, (size_t)count * SECTOR_SIZE);
	return 0;
}

static int ram_write(struct block_dev *dev, u64 lba, u32 count, const void *buf)
{
	struct ram_disk *r = dev->ctx;

	if (lba + count > dev->sectors || ++r->writes == r->fail_write)
		return -MERR_IO;
	memcpy(r->data + lba * SECTOR_SIZE, buf, (size_t)count * SECTOR_SIZE);
	return 0;
}

static void disk_new(struct ram_disk *r, u32 mib)
{
	memset(r, 0, sizeof(*r));
	r->dev = (struct block_dev){ "ram", (u64)mib * MIB, r, ram_read, ram_write };
	r->data = calloc(mib, 1024 * 1024);
}

static void fill_noise(struct ram_disk *r, u64 lba, u64 sectors, u32 seed)
{
	for (u64 i = lba * SECTOR_SIZE; i < (lba + sectors) * SECTOR_SIZE; i++) {
		seed = seed * 1664525u + 1013904223u;
		r->data[i] = (u8)(seed >> 24);
	}
}

static void mbr_entry(struct ram_disk *r, int slot, u8 type, u32 start, u32 count)
{
	u8 *e = r->data + 446 + slot * 16;

	e[0] = slot == 0 ? 0x80 : 0;
	e[4] = type;
	memcpy(e + 8, &start, 4);
	memcpy(e + 12, &count, 4);
	r->data[510] = 0x55;
	r->data[511] = 0xAA;
}

/* A realistic dual-boot disk: boot code, Windows NTFS, Linux ext4, free tail. */
static void make_dual_boot(struct ram_disk *r)
{
	disk_new(r, 64);
	fill_noise(r, 0, 1, 7);                         /* boot code + disk signature */
	memset(r->data + 446, 0, 66);
	mbr_entry(r, 0, 0x07, 1 * MIB, 20 * MIB);
	mbr_entry(r, 1, 0x83, 21 * MIB, 10 * MIB);
	fill_noise(r, 1 * MIB, 20 * MIB, 11);
	memcpy(r->data + (u64)1 * MIB * SECTOR_SIZE + 3, "NTFS    ", 8);
	fill_noise(r, 21 * MIB, 10 * MIB, 13);
	r->data[((u64)21 * MIB + 2) * SECTOR_SIZE + 56] = 0x53;
	r->data[((u64)21 * MIB + 2) * SECTOR_SIZE + 57] = 0xEF;
}

static struct install_config sample_config(void)
{
	struct install_config c;

	memset(&c, 0, sizeof(c));
	c.lang = LANG_ES;
	strcpy(c.name, "Zo\xC3\xAB N\xC3\xBA\xC3\xB1" "ez");
	c.password_enabled = true;
	c.iterations = 1000;
	c.desktop = DE_RETRO;
	for (int i = 0; i < PW_SALT_LEN; i++)
		c.salt[i] = (u8)(i * 3);
	pbkdf2_hmac_sha256("abc12345", 8, c.salt, PW_SALT_LEN, c.iterations, c.hash);
	return c;
}

static bool same_config(const struct install_config *a, const struct install_config *b)
{
	return a->lang == b->lang && !strcmp(a->name, b->name) &&
	       a->password_enabled == b->password_enabled && a->iterations == b->iterations &&
	       a->desktop == b->desktop &&
	       !memcmp(a->salt, b->salt, PW_SALT_LEN) && !memcmp(a->hash, b->hash, SHA256_LEN);
}

/* Expect planning to be refused with `rc` and the disk left byte-identical. */
static void expect_refused(struct ram_disk *r, int rc)
{
	struct disk_layout l;
	struct disk_plan p;
	u8 sector[SECTOR_SIZE] = { 0 };
	size_t size = (size_t)r->dev.sectors * SECTOR_SIZE;
	u8 *before = malloc(size);

	memcpy(before, r->data, size);
	CHECK_EQ(disk_scan(&r->dev, &l), 0);
	CHECK_EQ(disk_plan(&r->dev, &l, &p), rc);
	CHECK(disk_apply(&r->dev, &l, &p, sector) < 0);        /* even if a caller ignores the refusal */
	CHECK_EQ(r->writes, 0);
	CHECK(!memcmp(before, r->data, size));
	free(before);
	free(r->data);
}

/* ---- GPT ---------------------------------------------------------------------- */

#define GPT_ARRAY_SECTORS 32            /* 128 entries of 128 bytes */

static void w32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void w64(u8 *p, u64 v) { memcpy(p, &v, 8); }

static u32 header_crc(const u8 *h)
{
	u8 tmp[SECTOR_SIZE];

	memcpy(tmp, h, SECTOR_SIZE);
	memset(tmp + 16, 0, 4);
	return mos_crc32(0, tmp, 92);
}

static void gpt_header(struct ram_disk *r, u64 self, u64 other, u64 table, u32 entries_crc)
{
	u8 *h = r->data + self * SECTOR_SIZE;

	memset(h, 0, SECTOR_SIZE);
	memcpy(h, "EFI PART", 8);
	w32(h + 8, 0x00010000);
	w32(h + 12, 92);
	w64(h + 24, self);
	w64(h + 32, other);
	w64(h + 40, 34);
	w64(h + 48, r->dev.sectors - 34);
	memset(h + 56, 0x5A, 16);                       /* disk GUID */
	w64(h + 72, table);
	w32(h + 80, 128);
	w32(h + 84, 128);
	w32(h + 88, entries_crc);
	w32(h + 16, header_crc(h));
}

/* Mirror the primary entry array to the backup and (re)write both headers. */
static void gpt_finish(struct ram_disk *r)
{
	u64 last = r->dev.sectors - 1;
	u8 *array = r->data + 2 * SECTOR_SIZE;
	u32 crc = mos_crc32(0, array, 128 * 128);

	memcpy(r->data + (last - GPT_ARRAY_SECTORS) * SECTOR_SIZE, array,
	       GPT_ARRAY_SECTORS * SECTOR_SIZE);
	gpt_header(r, 1, last, 2, crc);
	gpt_header(r, last, 1, last - GPT_ARRAY_SECTORS, crc);
}

static void gpt_entry(struct ram_disk *r, int slot, u8 type_byte, u64 start, u64 count)
{
	u8 *e = r->data + 2 * SECTOR_SIZE + slot * 128;

	memset(e, type_byte, 16);                       /* type GUID   */
	memset(e + 16, slot + 1, 16);                   /* unique GUID */
	w64(e + 32, start);
	w64(e + 40, start + count - 1);
	memcpy(e + 56, "p\0a\0r\0t\0", 8);
}

/* UEFI dual boot: EFI system partition, Windows NTFS, Linux ext4, free tail. */
static void make_gpt_dual_boot(struct ram_disk *r)
{
	disk_new(r, 64);
	mbr_entry(r, 0, 0xEE, 1, 64 * MIB - 1);         /* protective MBR */
	r->data[446] = 0;
	gpt_entry(r, 0, 0xC1, 1 * MIB, 2 * MIB);
	gpt_entry(r, 1, 0xEB, 3 * MIB, 20 * MIB);
	gpt_entry(r, 2, 0x0F, 23 * MIB, 10 * MIB);
	fill_noise(r, 1 * MIB, 32 * MIB, 17);
	memcpy(r->data + (u64)1 * MIB * SECTOR_SIZE + 0x52, "FAT32   ", 8);
	memcpy(r->data + (u64)3 * MIB * SECTOR_SIZE + 3, "NTFS    ", 8);
	memset(r->data + (u64)23 * MIB * SECTOR_SIZE, 0, 8 * SECTOR_SIZE);
	r->data[((u64)23 * MIB + 2) * SECTOR_SIZE + 56] = 0x53;
	r->data[((u64)23 * MIB + 2) * SECTOR_SIZE + 57] = 0xEF;
	gpt_finish(r);
}

/* Independent check that one copy (header at `lba`) is complete and valid. */
static bool gpt_copy_valid(const struct ram_disk *r, u64 lba)
{
	const u8 *h = r->data + lba * SECTOR_SIZE;
	u64 table = rd64le(h + 72);

	return !memcmp(h, "EFI PART", 8) && header_crc(h) == rd32le(h + 16) &&
	       mos_crc32(0, r->data + table * SECTOR_SIZE, 128 * 128) == rd32le(h + 88);
}

static void copy_range(u8 *dst, const u8 *src, u64 off, u64 n)
{
	memcpy(dst + off, src + off, n);
}

static void test_gpt(void)
{
	struct install_config cfg = sample_config(), back;
	struct ram_disk r;
	struct disk_layout l;
	struct disk_plan p;
	u8 sector[SECTOR_SIZE], *before, *expect;
	const u64 last = 64 * MIB - 1, barray = last - GPT_ARRAY_SECTORS;
	size_t size;

	config_pack(&cfg, sector);
	make_gpt_dual_boot(&r);
	size = (size_t)r.dev.sectors * SECTOR_SIZE;
	before = malloc(size);
	expect = malloc(size);
	memcpy(before, r.data, size);

	CHECK_EQ(disk_scan(&r.dev, &l), 0);
	CHECK(l.scheme == DISK_GPT && l.consistent && l.gpt.backup_ok && l.nparts == 3);
	CHECK(l.part[0].fs == FS_FAT && l.part[1].fs == FS_NTFS && l.part[2].fs == FS_EXT);
	CHECK_EQ(l.gpt.free_slot, 3);
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	CHECK(p.gpt && p.gpt_slot == 3 && p.start == 33 * MIB && p.sectors == 30 * MIB);
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), 0);
	CHECK_EQ(r.writes, 5);

	/*
	 * Byte-for-byte: start from the original image and admit only the
	 * changes the contract allows. Everything else must be identical:
	 * the protective MBR, the three existing entries in both arrays,
	 * every other header field, and all partition contents.
	 */
	memcpy(expect, before, size);
	copy_range(expect, r.data, (u64)33 * MIB * SECTOR_SIZE, SECTOR_SIZE);   /* config sector    */
	copy_range(expect, r.data, 2 * SECTOR_SIZE + 3 * 128, 128);            /* primary entry 3  */
	copy_range(expect, r.data, barray * SECTOR_SIZE + 3 * 128, 128);       /* backup entry 3   */
	copy_range(expect, r.data, 1 * SECTOR_SIZE + 16, 4);                   /* header CRCs ...  */
	copy_range(expect, r.data, 1 * SECTOR_SIZE + 88, 4);
	copy_range(expect, r.data, last * SECTOR_SIZE + 16, 4);
	copy_range(expect, r.data, last * SECTOR_SIZE + 88, 4);
	CHECK(!memcmp(expect, r.data, size));
	CHECK(!memcmp(before + (u64)1 * MIB * SECTOR_SIZE, r.data + (u64)1 * MIB * SECTOR_SIZE,
		      (size_t)32 * MIB * SECTOR_SIZE));                         /* ESP + NTFS + ext */
	CHECK(gpt_copy_valid(&r, 1) && gpt_copy_valid(&r, last));

	CHECK_EQ(disk_scan(&r.dev, &l), 0);
	CHECK(l.consistent && l.gpt.backup_ok && l.nparts == 4 && l.gpt.free_slot == 4);
	CHECK(l.part[3].start == 33 * MIB && l.part[3].fs == FS_MERGEDOS);
	CHECK(!disk_find_mergedos(&r.dev, &l, sector) && !config_unpack(sector, &back) &&
	      same_config(&cfg, &back));
	config_pack(&cfg, sector);

	/* ---- apply-time guards: each refused before any write ------------------- */
	memcpy(r.data, before, size);
	r.writes = 0;
	CHECK_EQ(disk_scan(&r.dev, &l), 0);
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	r.data[last * SECTOR_SIZE + 60] ^= 1;           /* backup header changed after the scan */
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_AGAIN);
	r.data[last * SECTOR_SIZE + 60] ^= 1;
	r.data[barray * SECTOR_SIZE + 3 * 128 + 5] = 9; /* arrays disagree about the slot */
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_INVAL);
	r.data[barray * SECTOR_SIZE + 3 * 128 + 5] = 0;
	p.gpt_slot = 1;                                 /* a plan aimed at Windows' entry */
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_INVAL);
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	p.start = 4 * MIB;                              /* a plan aimed at Windows' data */
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_INVAL);
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	p.sectors += 2 * MIB;                           /* a plan running into the backup table */
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_INVAL);
	CHECK_EQ(r.writes, 0);
	CHECK(!memcmp(before, r.data, size));

	/*
	 * Power loss at any of the five writes: existing partitions are intact
	 * and at least one complete, valid copy of the table remains.
	 */
	for (int k = 1; k <= 5; k++) {
		memcpy(r.data, before, size);
		r.writes = 0;
		r.fail_write = k;
		CHECK_EQ(disk_scan(&r.dev, &l), 0);
		CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
		CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_IO);
		CHECK(!memcmp(before + (u64)1 * MIB * SECTOR_SIZE,
			      r.data + (u64)1 * MIB * SECTOR_SIZE, (size_t)32 * MIB * SECTOR_SIZE));
		CHECK(!memcmp(before, r.data, SECTOR_SIZE));            /* protective MBR */
		CHECK(gpt_copy_valid(&r, 1) || gpt_copy_valid(&r, last));
		if (k <= 3)                     /* primary not touched yet: still the old table */
			CHECK(!memcmp(before + SECTOR_SIZE, r.data + SECTOR_SIZE, 33 * SECTOR_SIZE));
	}
	r.fail_write = 0;
	free(r.data);

	/* ---- tables that must be refused, with zero writes ---------------------- */
	make_gpt_dual_boot(&r);                         /* backup header destroyed */
	memset(r.data + last * SECTOR_SIZE, 0, SECTOR_SIZE);
	expect_refused(&r, -MERR_INVAL);

	make_gpt_dual_boot(&r);                         /* backup array differs from primary */
	r.data[barray * SECTOR_SIZE + 40] ^= 1;
	expect_refused(&r, -MERR_INVAL);

	make_gpt_dual_boot(&r);                         /* primary header CRC wrong */
	r.data[SECTOR_SIZE + 60] ^= 1;
	expect_refused(&r, -MERR_INVAL);

	make_gpt_dual_boot(&r);                         /* primary array CRC wrong */
	r.data[2 * SECTOR_SIZE + 20] ^= 1;
	expect_refused(&r, -MERR_INVAL);

	make_gpt_dual_boot(&r);                         /* overlapping entries */
	gpt_entry(&r, 3, 0x33, 10 * MIB, 4 * MIB);
	gpt_finish(&r);
	expect_refused(&r, -MERR_INVAL);

	make_gpt_dual_boot(&r);                         /* every entry slot in use */
	for (int i = 3; i < 128; i++)
		memset(r.data + 2 * SECTOR_SIZE + i * 128 + 50, 1, 1);  /* non-zero: not free */
	gpt_finish(&r);
	expect_refused(&r, -MERR_RANGE);

	make_gpt_dual_boot(&r);                         /* no gap of 16 MiB */
	gpt_entry(&r, 3, 0x44, 33 * MIB, 20 * MIB);
	gpt_finish(&r);
	expect_refused(&r, -MERR_RANGE);

	make_gpt_dual_boot(&r);                         /* a lost file system in the free space */
	memcpy(r.data + (u64)33 * MIB * SECTOR_SIZE, "XFSB", 4);
	expect_refused(&r, -MERR_ACCES);

	free(before);
	free(expect);
}

void test_storage(void)
{
	struct install_config cfg = sample_config(), back;
	struct ram_disk r;
	struct disk_layout l;
	struct disk_plan p;
	u8 sector[SECTOR_SIZE], *before;
	char text[160];
	size_t size;

	/* ---- config sector --------------------------------------------------- */
	config_pack(&cfg, sector);
	CHECK_EQ(config_unpack(sector, &back), 0);
	CHECK(same_config(&cfg, &back));
	sector[100] ^= 0x04;                            /* any flipped bit fails the CRC */
	CHECK_EQ(config_unpack(sector, &back), -MERR_INVAL);
	memset(sector, 0, sizeof(sector));
	CHECK_EQ(config_unpack(sector, &back), -MERR_INVAL);
	/* Desktop choice: every profile and "recommend" survive; junk is rejected. */
	for (int d = 0; d <= DE_COUNT; d++) {
		cfg.desktop = d < DE_COUNT ? (u8)d : DESKTOP_AUTO;
		config_pack(&cfg, sector);
		CHECK(!config_unpack(sector, &back) && back.desktop == cfg.desktop);
	}
	sector[21] = 0x40;
	sector[508] = sector[509] = sector[510] = sector[511] = 0;
	{
		u32 crc = mos_crc32(0, sector, 508);

		memcpy(sector + 508, &crc, 4);  /* valid CRC, invalid desktop id */
	}
	CHECK_EQ(config_unpack(sector, &back), -MERR_INVAL);
	cfg.desktop = DE_RETRO;
	config_pack(&cfg, sector);

	/* ---- blank disk: becomes one MergedOS partition ----------------------- */
	disk_new(&r, 64);
	CHECK_EQ(disk_scan(&r.dev, &l), 0);
	CHECK_EQ(l.scheme, DISK_BLANK);
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	CHECK(p.blank && p.start == MIB && p.sectors == 63 * MIB);
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), 0);
	CHECK_EQ(r.writes, 2);
	CHECK_EQ(disk_scan(&r.dev, &l), 0);
	CHECK(l.scheme == DISK_MBR && l.consistent && l.nparts == 1);
	CHECK(l.part[0].mbr_type == MBR_TYPE_MERGEDOS && l.part[0].fs == FS_MERGEDOS);
	CHECK_EQ(disk_find_mergedos(&r.dev, &l, sector), 0);
	CHECK(!config_unpack(sector, &back) && same_config(&cfg, &back));
	free(r.data);

	/* ---- dual boot: Windows and Linux must come through untouched --------- */
	make_dual_boot(&r);
	size = (size_t)r.dev.sectors * SECTOR_SIZE;
	before = malloc(size);
	memcpy(before, r.data, size);
	CHECK_EQ(disk_scan(&r.dev, &l), 0);
	CHECK(l.scheme == DISK_MBR && l.consistent && l.nparts == 2);
	CHECK(l.part[0].fs == FS_NTFS && l.part[1].fs == FS_EXT);
	disk_describe(&l, text, sizeof(text));
	CHECK(strstr(text, "Windows NTFS 20 MiB") && strstr(text, "Linux ext 10 MiB"));
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	CHECK(!p.blank && p.mbr_slot == 2 && p.start == 31 * MIB && p.sectors == 33 * MIB);
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), 0);
	CHECK_EQ(r.writes, 2);                          /* config sector + MBR, nothing else */
	/* Sector 0: only the 16 bytes of table entry 2 may differ. */
	CHECK(!memcmp(before, r.data, 446 + 2 * 16));
	CHECK(!memcmp(before + 446 + 3 * 16, r.data + 446 + 3 * 16, 512 - (446 + 3 * 16)));
	/* Everything from sector 1 to the start of the new partition: identical.
	 * That range holds both existing partitions in full. */
	CHECK(!memcmp(before + SECTOR_SIZE, r.data + SECTOR_SIZE, (size_t)(31 * MIB - 1) * SECTOR_SIZE));
	/* After the config sector: identical to the end of the disk. */
	CHECK(!memcmp(before + (size_t)(31 * MIB + 1) * SECTOR_SIZE,
		      r.data + (size_t)(31 * MIB + 1) * SECTOR_SIZE,
		      size - (size_t)(31 * MIB + 1) * SECTOR_SIZE));
	CHECK_EQ(disk_scan(&r.dev, &l), 0);
	CHECK(l.consistent && l.nparts == 3 && l.part[0].fs == FS_NTFS && l.part[1].fs == FS_EXT);
	CHECK(!disk_find_mergedos(&r.dev, &l, sector) && !config_unpack(sector, &back) &&
	      same_config(&cfg, &back));
	config_pack(&cfg, sector);

	/* ---- apply-time guards ----------------------------------------------- */
	memcpy(r.data, before, size);                   /* back to the pristine dual-boot disk */
	r.writes = 0;
	CHECK_EQ(disk_scan(&r.dev, &l), 0);
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	r.data[100] ^= 1;                               /* disk changed after the scan */
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_AGAIN);
	r.data[100] ^= 1;
	p.start = 2 * MIB;                              /* a plan pointing into Windows */
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_INVAL);
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	p.sectors += 4 * MIB;                           /* a plan running off the disk */
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_INVAL);
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	p.mbr_slot = 0;                                 /* a plan overwriting Windows' entry */
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_INVAL);
	CHECK_EQ(r.writes, 0);
	CHECK(!memcmp(before, r.data, size));
	/* The MBR write fails: the table, and so every partition, is as before. */
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	r.fail_write = 2;
	CHECK_EQ(disk_apply(&r.dev, &l, &p, sector), -MERR_IO);
	CHECK(!memcmp(before, r.data, SECTOR_SIZE));
	CHECK(!memcmp(before + SECTOR_SIZE, r.data + SECTOR_SIZE, (size_t)(31 * MIB - 1) * SECTOR_SIZE));
	free(before);
	free(r.data);

	/* ---- layouts that must be refused, with zero writes -------------------- */
	disk_new(&r, 64);                               /* one partition fills the disk */
	mbr_entry(&r, 0, 0x83, MIB, 63 * MIB);
	expect_refused(&r, -MERR_RANGE);

	make_dual_boot(&r);                             /* free space, but no free table slot */
	mbr_entry(&r, 2, 0x82, 31 * MIB, MIB);
	mbr_entry(&r, 3, 0x0B, 32 * MIB, MIB);
	expect_refused(&r, -MERR_RANGE);

	disk_new(&r, 64);                               /* largest gap is under 16 MiB */
	mbr_entry(&r, 0, 0x83, MIB, 50 * MIB);
	expect_refused(&r, -MERR_RANGE);

	make_dual_boot(&r);                             /* overlapping entries */
	mbr_entry(&r, 2, 0x83, 10 * MIB, 5 * MIB);
	expect_refused(&r, -MERR_INVAL);

	make_dual_boot(&r);                             /* entry past the end of the disk */
	mbr_entry(&r, 2, 0x83, 60 * MIB, 10 * MIB);
	expect_refused(&r, -MERR_INVAL);

	make_dual_boot(&r);                             /* "free" space holds a lost NTFS volume */
	memcpy(r.data + (u64)31 * MIB * SECTOR_SIZE + 3, "NTFS    ", 8);
	expect_refused(&r, -MERR_ACCES);

	disk_new(&r, 64);                               /* file system with no partition table */
	r.data[2 * SECTOR_SIZE + 56] = 0x53;
	r.data[2 * SECTOR_SIZE + 57] = 0xEF;
	expect_refused(&r, -MERR_NOTSUP);

	disk_new(&r, 64);                               /* FAT boot sector as sector 0 */
	memcpy(r.data + 0x52, "FAT32   ", 8);
	r.data[510] = 0x55;
	r.data[511] = 0xAA;
	expect_refused(&r, -MERR_NOTSUP);

	disk_new(&r, 64);                               /* not blank: data near the end */
	r.data[(size_t)64 * 1024 * 1024 - 100] = 1;
	expect_refused(&r, -MERR_NOTSUP);

	/* ---- a gap between partitions is found and aligned to 1 MiB ------------ */
	disk_new(&r, 64);
	mbr_entry(&r, 0, 0x07, MIB, 4 * MIB + 5);       /* ends off a MiB boundary */
	mbr_entry(&r, 1, 0x05, 40 * MIB, 24 * MIB);     /* extended container: occupied */
	CHECK_EQ(disk_scan(&r.dev, &l), 0);
	CHECK_EQ(disk_plan(&r.dev, &l, &p), 0);
	CHECK(p.start == 6 * MIB && p.sectors == 34 * MIB && p.mbr_slot == 2);
	free(r.data);

	test_gpt();
}
