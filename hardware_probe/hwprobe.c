/* SPDX-License-Identifier: BSD-2-Clause */
/* Hardware probe driver: gather CPU, memory, PCI and firmware facts once at boot. */
#include <mergedos/kernel.h>
#include <mergedos/boot.h>
#include "hardware_probe/hwprobe.h"

void hwprobe_run(struct hw_profile *hw)
{
	const struct boot_info *bi = boot_info_get();
	u32 madt_cpus;

	memset(hw, 0, sizeof(*hw));

	hwprobe_cpu(hw);
	madt_cpus = hwprobe_acpi_cpu_count();
	if (madt_cpus)
		hw->cpus = madt_cpus;

	for (u32 i = 0; i < bi->nmmap; i++)
		if (bi->mmap[i].type == BOOT_MEM_AVAILABLE)
			hw->ram_kib += bi->mmap[i].len / 1024;
	hw->ram_mib = (u32)((hw->ram_kib + 512) / 1024);

	hwprobe_pci(hw);

	hw->has_fb = bi->has_fb;
	hw->fb_width = bi->fb_width;
	hw->fb_height = bi->fb_height;
	hw->fb_bpp = bi->fb_bpp;
}

void hwprobe_print(const struct hw_profile *hw)
{
	char feat[160];

	hw_feature_string(hw->features, feat, sizeof(feat));
	kprintf("[hw] cpu: %s, %s (family %u model %u stepping %u), %u logical CPU%s\n",
		hw->cpu_vendor, hw->cpu_brand[0] ? hw->cpu_brand : "unknown model",
		hw->cpu_family, hw->cpu_model, hw->cpu_stepping, hw->cpus,
		hw->cpus == 1 ? "" : "s");
	kprintf("[hw] cpu features: %s\n", feat);
	if (hw->features & HWF_HYPERVISOR)
		kprintf("[hw] running under a hypervisor: %s\n", hw->hypervisor);
	kprintf("[hw] ram: %u MiB available (%llu KiB)\n", hw->ram_mib,
		(unsigned long long)hw->ram_kib);
	kprintf("[hw] pci: %u functions, %u storage, %u network, %u usb, %u display\n",
		hw->pci_devices, hw->n_storage, hw->n_net, hw->n_usb, hw->ngpu);
	for (u32 i = 0; i < hw->ngpu; i++) {
		const struct hw_gpu *g = &hw->gpu[i];

		kprintf("[hw] gpu%u: %04x:%04x %s at %02x:%02x.%u - %s, %u MiB aperture\n", i,
			g->vendor, g->device, g->name, g->bus, g->dev, g->fn,
			hw_gpu_tier_name(g->tier), g->aperture_mib);
	}
	if (!hw->ngpu)
		kprintf("[hw] gpu: none found (headless)\n");
	if (hw->has_fb)
		kprintf("[hw] boot framebuffer: %ux%u, %u bpp\n", hw->fb_width, hw->fb_height,
			hw->fb_bpp);
}
