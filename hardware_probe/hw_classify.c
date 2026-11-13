/* SPDX-License-Identifier: BSD-2-Clause */
/* Pure classification helpers (no I/O): usable on the host for unit tests. */
#include "hardware_probe/hwprobe.h"

void hw_gpu_classify(u16 vendor, u16 device, enum hw_gpu_tier *tier, const char **name)
{
	switch (vendor) {
	case 0x10DE:
		*tier = HW_GPU_DISCRETE;
		*name = "NVIDIA";
		break;
	case 0x1002:
		/* Radeon APUs share the vendor ID; without a device table we assume discrete. */
		*tier = HW_GPU_DISCRETE;
		*name = "AMD Radeon";
		break;
	case 0x8086:
		/* Arc discrete cards: DG2 (0x56xx) and Battlemage (0xE2xx) device ranges. */
		if ((device >> 8) == 0x56 || (device >> 8) == 0xE2) {
			*tier = HW_GPU_DISCRETE;
			*name = "Intel Arc";
		} else {
			*tier = HW_GPU_INTEGRATED;
			*name = "Intel integrated graphics";
		}
		break;
	case 0x1AF4:
		*tier = HW_GPU_PARAVIRT;
		*name = "virtio-gpu";
		break;
	case 0x15AD:
		*tier = HW_GPU_PARAVIRT;
		*name = "VMware SVGA";
		break;
	case 0x1B36:
		*tier = HW_GPU_PARAVIRT;
		*name = "QXL";
		break;
	case 0x80EE:
		*tier = HW_GPU_PARAVIRT;
		*name = "VirtualBox graphics";
		break;
	case 0x1234:
		*tier = HW_GPU_BASIC;
		*name = "QEMU/Bochs standard VGA";
		break;
	case 0x1013:
		*tier = HW_GPU_BASIC;
		*name = "Cirrus Logic VGA";
		break;
	default:
		*tier = HW_GPU_BASIC;
		*name = "generic VGA-compatible adapter";
		break;
	}
}

const char *hw_gpu_tier_name(enum hw_gpu_tier tier)
{
	switch (tier) {
	case HW_GPU_BASIC:      return "basic framebuffer";
	case HW_GPU_PARAVIRT:   return "paravirtual";
	case HW_GPU_INTEGRATED: return "integrated";
	case HW_GPU_DISCRETE:   return "discrete";
	default:                return "none";
	}
}

void hw_feature_string(u32 features, char *buf, size_t cap)
{
	static const char *const names[] = {
		"sse2", "sse3", "ssse3", "sse4.1", "sse4.2", "popcnt", "aes", "avx", "avx2",
		"rdrand", "x2apic", "nx", "1g-pages", "hypervisor",
	};

	if (cap)
		buf[0] = '\0';
	for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
		if (!(features & (1u << i)))
			continue;
		if (buf[0])
			mos_strlcat(buf, " ", cap);
		mos_strlcat(buf, names[i], cap);
	}
}
