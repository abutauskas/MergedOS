/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef MERGEDOS_MM_H
#define MERGEDOS_MM_H

#include <mergedos/types.h>
#include <mergedos/boot.h>

/*
 * Virtual address space layout (x86_64, 4-level paging)
 *
 *   0x0000000000000000 - 0x00007FFFFFFFFFFF   user space (per task)
 *   0xFFFF800000000000 - 0xFFFF807FFFFFFFFF   direct map of physical 0..4 GiB, plus
 *                                             RAM above 4 GiB (up to 512 GiB)
 *   0xFFFFFFFF80000000 - 0xFFFFFFFFBFFFFFFF   kernel image (phys 0..1 GiB)
 */
#define KERNEL_VMA   0xFFFFFFFF80000000ULL
#define DMAP_BASE    0xFFFF800000000000ULL
#define DMAP_LIMIT   0x0000000100000000ULL      /* boot direct map: RAM and MMIO below 4 GiB */
#define DMAP_RAM_MAX 0x0000008000000000ULL      /* RAM above 4 GiB is mapped up to 512 GiB */
extern u64 dmap_top;                            /* end of the direct map after pmm_init() */
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
	u64 module_frames;      /* boot module (sysroot archive), read only  */
	u64 peak_used_frames;   /* high-water mark of usable - free          */
};

void pmm_init(const struct boot_info *bi);
u64  pmm_alloc(void);               /* zeroed 4 KiB frame, 0 when out of memory */
void pmm_free(u64 phys);            /* drops a reference; frees with the last */
void pmm_ref(u64 phys);             /* one more owner of a frame (COW, shared) */
u32  pmm_refs(u64 phys);
u64  pmm_alloc_contig(u64 n);       /* n contiguous zeroed frames, 0 on OOM  */
void pmm_free_contig(u64 phys, u64 n);
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
/* Set the exact rights (VMM_*) on a page that is already mapped. Returns false if not. */
bool vmm_remap_user(u64 pml4_phys, u64 va, u32 prot);
u64  vmm_user_to_phys(u64 pml4_phys, u64 va);   /* 0 if not mapped for user */
bool vmm_user_ok(u64 pml4_phys, u64 va, u64 len, bool write);
bool vmm_unmap_user(u64 pml4_phys, u64 va);     /* frees the frame */
u64  vmm_as_clone(u64 pml4_phys);               /* copy-on-write copy of the user half, 0 on OOM */
bool vmm_map_frame(u64 pml4_phys, u64 va, u64 frame, u32 prot, bool shared);
int  vmm_page_state(u64 pml4_phys, u64 va);     /* 0 none, 1 mapped, 3 PROT_NONE, +4 COW */
int  vmm_cow_fault(u64 pml4_phys, u64 va);
u64  vmm_take_frame(u64 pml4_phys, u64 va, bool *shared);
u32  vmm_unmap_range(u64 pml4_phys, u64 from, u64 to);
void vmm_protect_range(u64 pml4_phys, u64 from, u64 to, u32 prot);

#endif /* MERGEDOS_MM_H */
