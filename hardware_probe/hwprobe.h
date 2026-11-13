/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_HWPROBE_H
#define MERGEDOS_HWPROBE_H

/*
 * Boot-time hardware inventory. The result (struct hw_profile) is the only
 * input of the desktop selector's policy, which keeps that policy a pure
 * function that can be unit-tested with synthetic machines.
 */
#include <mergedos/compat.h>

/* Ordered from least to most capable: comparisons are meaningful. */
enum hw_gpu_tier {
	HW_GPU_NONE = 0,        /* no display controller (headless / serial)     */
	HW_GPU_BASIC,           /* dumb framebuffer: VGA, Bochs/std-vga, Cirrus  */
	HW_GPU_PARAVIRT,        /* virtio-gpu, VMware SVGA, QXL, VirtualBox      */
	HW_GPU_INTEGRATED,      /* Intel iGPU                                    */
	HW_GPU_DISCRETE,        /* NVIDIA, AMD Radeon, Intel Arc                 */
};

#define HWF_SSE2        (1u << 0)
#define HWF_SSE3        (1u << 1)
#define HWF_SSSE3       (1u << 2)
#define HWF_SSE41       (1u << 3)
#define HWF_SSE42       (1u << 4)
#define HWF_POPCNT      (1u << 5)
#define HWF_AES         (1u << 6)
#define HWF_AVX         (1u << 7)
#define HWF_AVX2        (1u << 8)
#define HWF_RDRAND      (1u << 9)
#define HWF_X2APIC      (1u << 10)
#define HWF_NX          (1u << 11)
#define HWF_PAGE1G      (1u << 12)
#define HWF_HYPERVISOR  (1u << 13)

#define HW_MAX_GPUS 4

struct hw_gpu {
	u16 vendor, device;
	u8  bus, dev, fn;
	enum hw_gpu_tier tier;
	u32 aperture_mib;       /* largest memory BAR: a proxy for video memory */
	u64 mem_bar_phys;       /* where that BAR is mapped (the linear framebuffer) */
	const char *name;
};

struct hw_profile {
	char cpu_vendor[13];
	char cpu_brand[49];
	u32  cpu_family, cpu_model, cpu_stepping;
	u32  cpus;              /* enabled logical processors */
	u32  features;          /* HWF_* */
	char hypervisor[13];

	u64  ram_kib;           /* firmware-reported available RAM */
	u32  ram_mib;

	u32  ngpu;
	struct hw_gpu gpu[HW_MAX_GPUS];
	enum hw_gpu_tier gpu_tier;      /* best tier present */
	u32  gpu_aperture_mib;

	u32  pci_devices, n_storage, n_net, n_usb;
	bool has_fb;
	u32  fb_width, fb_height, fb_bpp;
};

/* ---- portable (hw_classify.c) ------------------------------------------- */
void hw_gpu_classify(u16 vendor, u16 device, enum hw_gpu_tier *tier, const char **name);
const char *hw_gpu_tier_name(enum hw_gpu_tier tier);
void hw_feature_string(u32 features, char *buf, size_t cap);

/* ---- kernel only ----------------------------------------------------------- */
void hwprobe_run(struct hw_profile *hw);
void hwprobe_print(const struct hw_profile *hw);
void hwprobe_cpu(struct hw_profile *hw);        /* cpuid.c */
void hwprobe_pci(struct hw_profile *hw);        /* pci.c   */
u32  hwprobe_acpi_cpu_count(void);              /* acpi.c: 0 if no MADT */
u32  pci_read(u8 bus, u8 dev, u8 fn, u8 off);   /* pci.c: config space, for drivers */
void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 v);
bool pci_find_class(u8 class, u8 subclass, u8 progif, u8 *bus, u8 *dev, u8 *fn);

#endif /* MERGEDOS_HWPROBE_H */
