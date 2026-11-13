/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Just enough ACPI to count processors: RSDP -> RSDT/XSDT -> MADT ("APIC").
 * Tables are read in place through the direct map; anything outside it, or
 * with a bad checksum, is ignored and the caller falls back to CPUID.
 */
#include <mergedos/kernel.h>
#include <mergedos/mm.h>
#include "hardware_probe/hwprobe.h"

#define SDT_HEADER_SIZE 36

static const u8 *map(u64 phys, u64 len)
{
	if (phys == 0 || phys >= DMAP_LIMIT || len > DMAP_LIMIT - phys)
		return NULL;
	return phys_to_virt(phys);
}

static bool checksum_ok(const u8 *p, u32 len)
{
	u8 sum = 0;

	while (len--)
		sum = (u8)(sum + *p++);
	return sum == 0;
}

static const u8 *find_rsdp_in(u64 start, u64 len)
{
	const u8 *p = map(start, len);

	for (u64 off = 0; p && off + 20 <= len; off += 16)
		if (!memcmp(p + off, "RSD PTR ", 8) && checksum_ok(p + off, 20))
			return p + off;
	return NULL;
}

/* Map a system description table and validate it. */
static const u8 *map_sdt(u64 phys)
{
	const u8 *h = map(phys, SDT_HEADER_SIZE);
	u32 len;

	if (!h)
		return NULL;
	len = rd32le(h + 4);
	if (len < SDT_HEADER_SIZE || !map(phys, len) || !checksum_ok(h, len))
		return NULL;
	return h;
}

static u32 count_madt_cpus(const u8 *madt)
{
	u32 len = rd32le(madt + 4), count = 0;

	for (u32 off = 44; off + 2 <= len;) {
		u8 type = madt[off], elen = madt[off + 1];

		if (elen < 2 || off + elen > len)
			break;
		if (type == 0 && elen >= 8 && (rd32le(madt + off + 4) & 1))
			count++;                /* Processor Local APIC, enabled   */
		else if (type == 9 && elen >= 16 && (rd32le(madt + off + 8) & 1))
			count++;                /* Processor Local x2APIC, enabled */
		off += elen;
	}
	return count;
}

u32 hwprobe_acpi_cpu_count(void)
{
	const u8 *ebda_ptr = map(0x40E, 2);
	const u8 *rsdp = NULL, *root;
	bool xsdt;
	u32 entry_size, n;

	if (ebda_ptr)
		rsdp = find_rsdp_in((u64)rd16le(ebda_ptr) << 4, 1024);
	if (!rsdp)
		rsdp = find_rsdp_in(0xE0000, 0x20000);
	if (!rsdp)
		return 0;

	xsdt = rsdp[15] >= 2 && rd64le(rsdp + 24) != 0;
	root = map_sdt(xsdt ? rd64le(rsdp + 24) : rd32le(rsdp + 16));
	if (!root)
		return 0;
	entry_size = xsdt ? 8 : 4;
	n = (rd32le(root + 4) - SDT_HEADER_SIZE) / entry_size;

	for (u32 i = 0; i < n; i++) {
		const u8 *e = root + SDT_HEADER_SIZE + i * entry_size;
		const u8 *sdt = map_sdt(xsdt ? rd64le(e) : rd32le(e));

		if (sdt && !memcmp(sdt, "APIC", 4) && rd32le(sdt + 4) >= 44)
			return count_madt_cpus(sdt);
	}
	return 0;
}
