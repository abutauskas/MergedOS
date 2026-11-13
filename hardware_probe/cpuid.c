/* SPDX-License-Identifier: BSD-2-Clause */
/* CPU identification through CPUID. */
#include <mergedos/kernel.h>
#include <mergedos/io.h>
#include "hardware_probe/hwprobe.h"

static void put_regs(char *dst, u32 r0, u32 r1, u32 r2)
{
	memcpy(dst, &r0, 4);
	memcpy(dst + 4, &r1, 4);
	memcpy(dst + 8, &r2, 4);
	dst[12] = '\0';
}

void hwprobe_cpu(struct hw_profile *hw)
{
	static const struct { u32 bit; u32 flag; } ecx1[] = {
		{ 0, HWF_SSE3 }, { 9, HWF_SSSE3 }, { 19, HWF_SSE41 }, { 20, HWF_SSE42 },
		{ 21, HWF_X2APIC }, { 23, HWF_POPCNT }, { 25, HWF_AES }, { 28, HWF_AVX },
		{ 30, HWF_RDRAND }, { 31, HWF_HYPERVISOR },
	};
	u32 a, b, c, d, max_leaf, max_ext;

	cpuid(0, 0, &max_leaf, &b, &c, &d);
	put_regs(hw->cpu_vendor, b, d, c);

	cpuid(1, 0, &a, &b, &c, &d);
	hw->cpu_stepping = a & 0xF;
	hw->cpu_model = (a >> 4) & 0xF;
	hw->cpu_family = (a >> 8) & 0xF;
	if (hw->cpu_family == 6 || hw->cpu_family == 15)
		hw->cpu_model |= ((a >> 16) & 0xF) << 4;
	if (hw->cpu_family == 15)
		hw->cpu_family += (a >> 20) & 0xFF;
	for (unsigned i = 0; i < ARRAY_SIZE(ecx1); i++)
		if (c & (1u << ecx1[i].bit))
			hw->features |= ecx1[i].flag;
	if (d & (1u << 26))
		hw->features |= HWF_SSE2;
	/* Fallback count; replaced by the ACPI MADT count when one is available. */
	hw->cpus = (d & (1u << 28)) ? MOS_MAX((b >> 16) & 0xFF, 1u) : 1;

	if (max_leaf >= 7) {
		cpuid(7, 0, &a, &b, &c, &d);
		if (b & (1u << 5))
			hw->features |= HWF_AVX2;
	}

	cpuid(0x80000000, 0, &max_ext, &b, &c, &d);
	if (max_ext >= 0x80000001) {
		cpuid(0x80000001, 0, &a, &b, &c, &d);
		if (d & (1u << 20))
			hw->features |= HWF_NX;
		if (d & (1u << 26))
			hw->features |= HWF_PAGE1G;
	}
	if (max_ext >= 0x80000004) {
		char brand[49];
		const char *p = brand;

		for (u32 i = 0; i < 3; i++) {
			cpuid(0x80000002 + i, 0, &a, &b, &c, &d);
			memcpy(brand + i * 16, &a, 4);
			memcpy(brand + i * 16 + 4, &b, 4);
			memcpy(brand + i * 16 + 8, &c, 4);
			memcpy(brand + i * 16 + 12, &d, 4);
		}
		brand[48] = '\0';
		while (*p == ' ')
			p++;
		mos_strlcpy(hw->cpu_brand, p, sizeof(hw->cpu_brand));
	}

	if (hw->features & HWF_HYPERVISOR) {
		cpuid(0x40000000, 0, &a, &b, &c, &d);
		put_regs(hw->hypervisor, b, c, d);
	}
}
