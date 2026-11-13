/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * AHCI (SATA) driver: polled, one command slot, bounce-buffered.
 *
 * Small on purpose. Each disk costs two page frames: one holds the command
 * list, the received-FIS area and a single command table; the other is a
 * 4 KiB bounce buffer, so callers may pass any kernel pointer. No NCQ, no
 * interrupts, no hot-plug, no ATAPI. It fills in struct block_dev exactly
 * like the IDE driver does.
 *
 * Limit: the controller's registers are reached through the boot direct
 * map, which is cacheable; fine under emulation, to be replaced by a proper
 * uncached MMIO mapping before this is trusted on real hardware.
 */
#include <mergedos/kernel.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>
#include <mergedos/setup.h>
#include "hardware_probe/hwprobe.h"

/* HBA registers */
#define HBA_GHC        0x04
#define HBA_PI         0x0C
#define GHC_AE         (1u << 31)
/* Port registers, relative to 0x100 + port * 0x80 */
#define PX_CLB         0x00
#define PX_FB          0x08
#define PX_IS          0x10
#define PX_IE          0x14
#define PX_CMD         0x18
#define PX_TFD         0x20
#define PX_SIG         0x24
#define PX_SSTS        0x28
#define PX_SERR        0x30
#define PX_CI          0x38
#define CMD_ST         0x0001
#define CMD_FRE        0x0010
#define CMD_FR         0x4000
#define CMD_CR         0x8000
#define IS_TFES        (1u << 30)
#define SIG_SATA_DISK  0x00000101

#define ATA_IDENTIFY   0xEC
#define ATA_READ_EXT   0x25
#define ATA_WRITE_EXT  0x35
#define ATA_FLUSH_EXT  0xEA

#define FIS_OFF        0x400            /* layout of the per-port frame */
#define TABLE_OFF      0x500
#define BOUNCE_SECTORS 8
#define SPIN           5000000

struct ahci_port {
	volatile u32 *reg;
	u64 mem_phys, bounce_phys;
	char name[8];
	struct block_dev dev;
};

static struct ahci_port ports[AHCI_MAX_DISKS];
static int nports;

static bool wait_clear(volatile u32 *reg, u32 mask)
{
	for (int i = 0; i < SPIN; i++) {
		if (!(*reg & mask))
			return true;
		cpu_relax();
	}
	return false;
}

/* Run one command in slot 0. `bytes` of data move through the bounce buffer. */
static int ahci_exec(struct ahci_port *p, u8 command, u64 lba, u16 count, bool write, u32 bytes)
{
	u32 *hdr = phys_to_virt(p->mem_phys);
	u8 *table = phys_to_virt(p->mem_phys + TABLE_OFF);
	u32 *prdt = (u32 *)(table + 0x80);

	if (!wait_clear(&p->reg[PX_TFD / 4], 0x88))             /* BSY | DRQ */
		return -MERR_IO;

	hdr[0] = 5u | (write ? 1u << 6 : 0) | (bytes ? 1u << 16 : 0);  /* FIS length, W, PRDTL */
	hdr[1] = 0;
	hdr[2] = (u32)(p->mem_phys + TABLE_OFF);
	hdr[3] = 0;
	memset(table, 0, 0x90);
	table[0] = 0x27;                                        /* host-to-device register FIS */
	table[1] = 0x80;                                        /* this FIS carries a command */
	table[2] = command;
	table[4] = (u8)lba;
	table[5] = (u8)(lba >> 8);
	table[6] = (u8)(lba >> 16);
	table[7] = 0x40;                                        /* LBA mode */
	table[8] = (u8)(lba >> 24);
	table[9] = (u8)(lba >> 32);
	table[10] = (u8)(lba >> 40);
	table[12] = (u8)count;
	table[13] = (u8)(count >> 8);
	if (bytes) {
		prdt[0] = (u32)p->bounce_phys;
		prdt[1] = 0;
		prdt[3] = bytes - 1;
	}

	p->reg[PX_IS / 4] = 0xFFFFFFFFu;
	p->reg[PX_CI / 4] = 1;
	for (int i = 0; i < SPIN; i++) {
		if (p->reg[PX_IS / 4] & IS_TFES)
			return -MERR_IO;
		if (!(p->reg[PX_CI / 4] & 1))
			return (p->reg[PX_TFD / 4] & 0x01) ? -MERR_IO : 0;
		cpu_relax();
	}
	return -MERR_IO;
}

static int ahci_rw(struct block_dev *dev, u64 lba, u32 count, void *buf, bool write)
{
	struct ahci_port *p = dev->ctx;
	u8 *bounce = phys_to_virt(p->bounce_phys);

	if (lba >= dev->sectors || count > dev->sectors - lba)
		return -MERR_IO;
	while (count) {
		u32 n = MOS_MIN(count, (u32)BOUNCE_SECTORS);

		if (write)
			memcpy(bounce, buf, (size_t)n * SECTOR_SIZE);
		if (ahci_exec(p, write ? ATA_WRITE_EXT : ATA_READ_EXT, lba, (u16)n, write,
			      n * SECTOR_SIZE))
			return -MERR_IO;
		if (!write)
			memcpy(buf, bounce, (size_t)n * SECTOR_SIZE);
		buf = (u8 *)buf + (size_t)n * SECTOR_SIZE;
		lba += n;
		count -= n;
	}
	/* A write is only reported done once the drive has flushed its cache. */
	return write ? ahci_exec(p, ATA_FLUSH_EXT, 0, 0, false, 0) : 0;
}

static int ahci_read(struct block_dev *dev, u64 lba, u32 count, void *buf)
{
	return ahci_rw(dev, lba, count, buf, false);
}

static int ahci_write(struct block_dev *dev, u64 lba, u32 count, const void *buf)
{
	return ahci_rw(dev, lba, count, (void *)(uintptr_t)buf, true);
}

static void port_init(volatile u32 *hba, int n)
{
	struct ahci_port *p = &ports[nports];
	volatile u32 *reg = hba + (0x100 + n * 0x80) / 4;
	const u16 *id;
	u32 ssts = reg[PX_SSTS / 4];

	/* Device present with an established link, and it is a plain SATA disk. */
	if (nports == AHCI_MAX_DISKS || (ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1 ||
	    reg[PX_SIG / 4] != SIG_SATA_DISK)
		return;

	/* Stop the port before moving its command list and FIS area. */
	reg[PX_CMD / 4] &= ~(u32)(CMD_ST | CMD_FRE);
	if (!wait_clear(&reg[PX_CMD / 4], CMD_CR | CMD_FR))
		return;
	p->mem_phys = pmm_alloc();
	p->bounce_phys = pmm_alloc();
	if (!p->mem_phys || !p->bounce_phys)
		return;
	p->reg = reg;
	reg[PX_CLB / 4] = (u32)p->mem_phys;
	reg[PX_CLB / 4 + 1] = 0;
	reg[PX_FB / 4] = (u32)(p->mem_phys + FIS_OFF);
	reg[PX_FB / 4 + 1] = 0;
	reg[PX_SERR / 4] = 0xFFFFFFFFu;
	reg[PX_IE / 4] = 0;                                     /* polled */
	reg[PX_CMD / 4] |= CMD_FRE;
	reg[PX_CMD / 4] |= CMD_ST;

	if (ahci_exec(p, ATA_IDENTIFY, 0, 0, false, SECTOR_SIZE))
		return;
	id = phys_to_virt(p->bounce_phys);
	p->dev.sectors = (id[83] & (1u << 10))
			 ? ((u64)id[103] << 48) | ((u64)id[102] << 32) | ((u64)id[101] << 16) | id[100]
			 : ((u32)id[61] << 16) | id[60];
	if (!p->dev.sectors)
		return;
	ksnprintf(p->name, sizeof(p->name), "sata%d", nports);
	p->dev.name = p->name;
	p->dev.ctx = p;
	p->dev.read_sectors = ahci_read;
	p->dev.write_sectors = ahci_write;
	nports++;
}

int ahci_init(void)
{
	u8 bus, dev, fn;
	volatile u32 *hba;
	u64 abar;
	u32 pi;

	/* Mass storage (0x01), SATA (0x06), AHCI 1.0 programming interface (0x01). */
	if (!pci_find_class(0x01, 0x06, 0x01, &bus, &dev, &fn))
		return 0;
	abar = pci_read(bus, dev, fn, 0x24) & ~0xFu;            /* BAR5 */
	if (!abar || abar >= DMAP_LIMIT - 0x1100)
		return 0;
	pci_write(bus, dev, fn, 0x04, (pci_read(bus, dev, fn, 0x04) & 0xFFFF) | 0x6);  /* MMIO + bus master */

	hba = phys_to_virt(abar);
	hba[HBA_GHC / 4] |= GHC_AE;
	pi = hba[HBA_PI / 4];
	for (int n = 0; n < 32; n++)
		if (pi & (1u << n))
			port_init(hba, n);
	return nports;
}

struct block_dev *ahci_disk(int index)
{
	return index >= 0 && index < nports ? &ports[index].dev : NULL;
}
