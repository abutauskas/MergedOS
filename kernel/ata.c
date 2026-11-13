/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ATA PIO driver (legacy IDE ports), polled, one sector per command.
 * Slow but tiny, and enough for partition tables and the config sector.
 * It only fills in struct block_dev: AHCI/NVMe drivers will do the same.
 */
#include <mergedos/kernel.h>
#include <mergedos/io.h>
#include <mergedos/setup.h>

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x21                 /* ERR | DF */
#define ATA_SPIN   2000000

struct ata_disk {
	u16 io, ctl;
	u8 slave;
	bool lba48;
	char name[8];
	struct block_dev dev;
};

static struct ata_disk disks[ATA_MAX_DISKS];
static int ndisks;

static void delay400(u16 ctl)
{
	for (int i = 0; i < 4; i++)
		(void)inb(ctl);
}

/* Wait for BSY to clear; if want_drq, also for DRQ. -1 on error or timeout. */
static int ata_wait(const struct ata_disk *d, bool want_drq)
{
	for (int i = 0; i < ATA_SPIN; i++) {
		u8 st = inb(d->io + 7);

		if (st & ATA_SR_BSY)
			continue;
		if (st & ATA_SR_ERR)
			return -1;
		if (!want_drq || (st & ATA_SR_DRQ))
			return 0;
	}
	return -1;
}

static int ata_rw(struct ata_disk *d, u64 lba, void *buf, bool write)
{
	bool ext = d->lba48 && lba >= (1u << 28);

	if (lba >= d->dev.sectors || ata_wait(d, false))
		return -MERR_IO;
	if (ext) {
		outb(d->io + 6, (u8)(0x40 | (d->slave << 4)));
		outb(d->io + 2, 0);
		outb(d->io + 3, (u8)(lba >> 24));
		outb(d->io + 4, (u8)(lba >> 32));
		outb(d->io + 5, (u8)(lba >> 40));
	} else {
		outb(d->io + 6, (u8)(0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F)));
	}
	outb(d->io + 2, 1);
	outb(d->io + 3, (u8)lba);
	outb(d->io + 4, (u8)(lba >> 8));
	outb(d->io + 5, (u8)(lba >> 16));
	outb(d->io + 7, write ? (ext ? 0x34 : 0x30) : (ext ? 0x24 : 0x20));
	delay400(d->ctl);
	if (ata_wait(d, true))
		return -MERR_IO;
	if (write) {
		__asm__ volatile("rep outsw" : "+S"(buf), "+c"((u64){ 256 }) : "d"(d->io) : "memory");
		outb(d->io + 7, ext ? 0xEA : 0xE7);     /* flush the drive's write cache */
		delay400(d->ctl);
	} else {
		__asm__ volatile("rep insw" : "+D"(buf), "+c"((u64){ 256 }) : "d"(d->io) : "memory");
	}
	return ata_wait(d, false) ? -MERR_IO : 0;
}

static int ata_read(struct block_dev *dev, u64 lba, u32 count, void *buf)
{
	for (u32 i = 0; i < count; i++)
		if (ata_rw(dev->ctx, lba + i, (u8 *)buf + (size_t)i * SECTOR_SIZE, false))
			return -MERR_IO;
	return 0;
}

static int ata_write(struct block_dev *dev, u64 lba, u32 count, const void *buf)
{
	for (u32 i = 0; i < count; i++)
		if (ata_rw(dev->ctx, lba + i, (u8 *)(uintptr_t)buf + (size_t)i * SECTOR_SIZE, true))
			return -MERR_IO;
	return 0;
}

static void ata_identify(u16 io, u16 ctl, u8 slave)
{
	struct ata_disk *d = &disks[ndisks];
	u16 id[256];

	if (ndisks == ATA_MAX_DISKS || inb(io + 7) == 0xFF)     /* floating bus: no controller */
		return;
	d->io = io;
	d->ctl = ctl;
	d->slave = slave;
	outb(ctl, 0x02);                                        /* no interrupts: we poll */
	outb(io + 6, (u8)(0xA0 | (slave << 4)));
	delay400(ctl);
	outb(io + 2, 0);
	outb(io + 3, 0);
	outb(io + 4, 0);
	outb(io + 5, 0);
	outb(io + 7, 0xEC);                                     /* IDENTIFY DEVICE */
	delay400(ctl);
	if (inb(io + 7) == 0 || ata_wait(d, false) || inb(io + 4) || inb(io + 5) ||
	    ata_wait(d, true))
		return;                                         /* absent, or ATAPI (CD-ROM) */
	__asm__ volatile("rep insw" : "+D"((void *){ id }), "+c"((u64){ 256 }) : "d"(io) : "memory");

	d->lba48 = id[83] & (1u << 10);
	d->dev.sectors = d->lba48 ? ((u64)id[103] << 48) | ((u64)id[102] << 32) |
				    ((u64)id[101] << 16) | id[100]
				  : ((u32)id[61] << 16) | id[60];
	if (!d->dev.sectors)
		return;
	ksnprintf(d->name, sizeof(d->name), "ata%d", ndisks);
	d->dev.name = d->name;
	d->dev.ctx = d;
	d->dev.read_sectors = ata_read;
	d->dev.write_sectors = ata_write;
	ndisks++;
}

int ata_init(void)
{
	static const u16 ports[2][2] = { { 0x1F0, 0x3F6 }, { 0x170, 0x376 } };

	for (int bus = 0; bus < 2; bus++)
		for (u8 slave = 0; slave < 2; slave++)
			ata_identify(ports[bus][0], ports[bus][1], slave);
	return ndisks;
}

struct block_dev *ata_disk(int index)
{
	return index >= 0 && index < ndisks ? &disks[index].dev : NULL;
}
