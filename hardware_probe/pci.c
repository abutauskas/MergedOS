/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * PCI enumeration through configuration mechanism #1 (ports 0xCF8/0xCFC).
 * Counts device classes and records display controllers for the desktop
 * selector. ECAM (memory-mapped config space) arrives with the ACPI MCFG
 * work; mechanism #1 is enough to find every device on bus segment 0.
 */
#include <mergedos/kernel.h>
#include <mergedos/io.h>
#include "hardware_probe/hwprobe.h"

#define PCI_CLASS_STORAGE 0x01
#define PCI_CLASS_NETWORK 0x02
#define PCI_CLASS_DISPLAY 0x03
#define PCI_CLASS_SERIAL  0x0C
#define PCI_SUBCLASS_USB  0x03

static u32 cfg_read(u8 bus, u8 dev, u8 fn, u8 off)
{
	outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) |
			    (off & 0xFC));
	return inl(0xCFC);
}

static void cfg_write(u8 bus, u8 dev, u8 fn, u8 off, u32 v)
{
	outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) |
			    (off & 0xFC));
	outl(0xCFC, v);
}

/* Size the memory BARs of a (type 0) function; return the largest, in MiB. */
static u32 largest_mem_bar_mib(u8 bus, u8 dev, u8 fn, u64 *base)
{
	u32 cmd = cfg_read(bus, dev, fn, 0x04) & 0xFFFF;
	u64 best = 0;

	/* Decoding off while BARs hold the all-ones probe value. Status bits are
	 * write-one-to-clear, so writing zeros to the upper half is harmless. */
	cfg_write(bus, dev, fn, 0x04, cmd & ~0x3u);
	for (u8 i = 0; i < 6; i++) {
		u8 off = (u8)(0x10 + i * 4);
		u32 orig = cfg_read(bus, dev, fn, off), lo, hi = 0xFFFFFFFFu, orig_hi = 0;
		bool is64 = (orig & 0x6) == 0x4;
		u64 size;

		if (orig & 0x1)                 /* I/O BAR */
			continue;
		cfg_write(bus, dev, fn, off, 0xFFFFFFFFu);
		lo = cfg_read(bus, dev, fn, off) & ~0xFu;
		cfg_write(bus, dev, fn, off, orig);
		if (is64) {
			orig_hi = cfg_read(bus, dev, fn, (u8)(off + 4));
			cfg_write(bus, dev, fn, (u8)(off + 4), 0xFFFFFFFFu);
			hi = cfg_read(bus, dev, fn, (u8)(off + 4));
			cfg_write(bus, dev, fn, (u8)(off + 4), orig_hi);
			i++;
		}
		if (!lo && (!is64 || !hi))      /* unimplemented BAR */
			continue;
		size = ~(((u64)hi << 32) | lo) + 1;
		if (size > best) {
			best = size;
			*base = ((u64)orig_hi << 32) | (orig & ~0xFu);
		}
	}
	cfg_write(bus, dev, fn, 0x04, cmd);
	return (u32)(best >> 20);
}

static void visit(struct hw_profile *hw, u8 bus, u8 dev, u8 fn, u32 id)
{
	u32 class_reg = cfg_read(bus, dev, fn, 0x08);
	u8 class = (u8)(class_reg >> 24), subclass = (u8)(class_reg >> 16);
	u8 header = (u8)(cfg_read(bus, dev, fn, 0x0C) >> 16) & 0x7F;

	hw->pci_devices++;
	if (class == PCI_CLASS_STORAGE)
		hw->n_storage++;
	else if (class == PCI_CLASS_NETWORK)
		hw->n_net++;
	else if (class == PCI_CLASS_SERIAL && subclass == PCI_SUBCLASS_USB)
		hw->n_usb++;
	else if (class == PCI_CLASS_DISPLAY && hw->ngpu < HW_MAX_GPUS) {
		struct hw_gpu *g = &hw->gpu[hw->ngpu++];

		g->vendor = (u16)id;
		g->device = (u16)(id >> 16);
		g->bus = bus;
		g->dev = dev;
		g->fn = fn;
		hw_gpu_classify(g->vendor, g->device, &g->tier, &g->name);
		g->aperture_mib = header == 0 ? largest_mem_bar_mib(bus, dev, fn, &g->mem_bar_phys) : 0;
		if (g->tier > hw->gpu_tier ||
		    (g->tier == hw->gpu_tier && g->aperture_mib > hw->gpu_aperture_mib)) {
			hw->gpu_tier = g->tier;
			hw->gpu_aperture_mib = g->aperture_mib;
		}
	}
}

void hwprobe_pci(struct hw_profile *hw)
{
	for (unsigned bus = 0; bus < 256; bus++) {
		for (u8 dev = 0; dev < 32; dev++) {
			u32 id = cfg_read((u8)bus, dev, 0, 0x00);
			u8 nfn;

			if ((id & 0xFFFF) == 0xFFFF)
				continue;
			nfn = (cfg_read((u8)bus, dev, 0, 0x0C) & 0x00800000u) ? 8 : 1;
			for (u8 fn = 0; fn < nfn; fn++) {
				if (fn)
					id = cfg_read((u8)bus, dev, fn, 0x00);
				if ((id & 0xFFFF) != 0xFFFF)
					visit(hw, (u8)bus, dev, fn, id);
			}
		}
	}
}

/* ---- for device drivers ---------------------------------------------------- */

u32 pci_read(u8 bus, u8 dev, u8 fn, u8 off)
{
	return cfg_read(bus, dev, fn, off);
}

void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 v)
{
	cfg_write(bus, dev, fn, off, v);
}

bool pci_find_class(u8 class, u8 subclass, u8 progif, u8 *bus, u8 *dev, u8 *fn)
{
	for (unsigned b = 0; b < 256; b++)
		for (u8 d = 0; d < 32; d++)
			for (u8 f = 0; f < 8; f++) {
				if ((cfg_read((u8)b, d, f, 0x00) & 0xFFFF) == 0xFFFF) {
					if (f == 0)
						break;          /* no device in this slot */
					continue;
				}
				if (cfg_read((u8)b, d, f, 0x08) >> 8 ==
				    (((u32)class << 16) | ((u32)subclass << 8) | progif)) {
					*bus = (u8)b;
					*dev = d;
					*fn = f;
					return true;
				}
			}
	return false;
}
