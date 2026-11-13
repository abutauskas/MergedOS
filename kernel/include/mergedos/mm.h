/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_MM_H
#define MERGEDOS_MM_H

#include <mergedos/types.h>
#include <mergedos/boot.h>

/*
 * Virtual address space layout (x86_64, 4-level paging)
 *
 *   0x0000000000000000 - 0x00007FFFFFFFFFFF   user space (per task)
 *   0xFFFF800000000000 - 0xFFFF8000FFFFFFFF   direct map of physical 0..4 GiB
 *   0xFFFFFFFF80000000 - 0xFFFFFFFFBFFFFFFF   kernel image (phys 0..1 GiB)
 */
#define KERNEL_VMA   0xFFFFFFFF80000000ULL
#define DMAP_BASE    0xFFFF800000000000ULL
#define DMAP_LIMIT   0x0000000100000000ULL      /* physical bytes covered by the direct map */
#define USER_TOP     0x0000800000000000ULL      /* first non-canonical user address */
#define USER_MIN     0x0000000000010000ULL      /* keep the NULL page region unmapped */

static inline void *phys_to_virt(u64 phys) { return (void *)(phys + DMAP_BASE); }

/* Linker-provided section boundaries (virtual addresses). */
extern u8 __kernel_start[], __kernel_end[];
extern u8 __text_start[], __text_end[];
extern u8 __rodata_start[], __rodata_end[];
extern u8 __data_start[], __data_end[];
extern u8 __bss_start[], __bss_end[];

/* ---- Physical frame allocator (pmm.c) ---------------------------------- */

struct pmm_stats {
	u64 managed_top;        /* highest managed physical address          */
	u64 usable_frames;      /* frames of available RAM above 1 MiB       */
	u64 free_frames;
	u64 kernel_frames;      /* kernel image + boot page tables           */
	u64 bitmap_frames;      /* allocator metadata                        */
	u64 peak_used_frames;   /* high-water mark of usable - free          */
};

void pmm_init(const struct boot_info *bi);
u64  pmm_alloc(void);               /* zeroed 4 KiB frame, 0 when out of memory */
void pmm_free(u64 phys);
void pmm_get_stats(struct pmm_stats *out);

/* ---- Virtual memory (vmm.c) --------------------------------------------- */

#define VMM_R 0x1
#define VMM_W 0x2
#define VMM_X 0x4

void vmm_init(void);
u64  vmm_kernel_pml4(void);
void vmm_switch(u64 pml4_phys);

u64  vmm_as_create(void);                       /* new user address space, 0 on OOM */
void vmm_as_destroy(u64 pml4_phys);

/* Map (or widen the permissions of) one user page; returns its frame or 0. */
u64  vmm_map_user(u64 pml4_phys, u64 va, u32 prot);
u64  vmm_user_to_phys(u64 pml4_phys, u64 va);   /* 0 if not mapped for user */
bool vmm_user_ok(u64 pml4_phys, u64 va, u64 len, bool write);

#endif /* MERGEDOS_MM_H */
