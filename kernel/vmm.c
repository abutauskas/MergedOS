/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Virtual memory: per-task user address spaces on 4-level page tables.
 *
 * Every address space shares the kernel half (PML4 slots 256..511) and owns
 * the user half. Foreign binaries are mapped W^X according to the
 * protections recorded in their own segment/section headers.
 */
#include <mergedos/kernel.h>
#include <mergedos/cpu.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>

#define PTE_P     0x001ULL
#define PTE_W     0x002ULL
#define PTE_U     0x004ULL
#define PTE_PS    0x080ULL
#define PTE_NX    (1ULL << 63)
#define PTE_ADDR  0x000FFFFFFFFFF000ULL

static u64 kernel_pml4;

static inline unsigned pt_index(u64 va, int level)
{
	return (unsigned)((va >> (PAGE_SHIFT + 9 * level)) & 0x1FF);
}

void vmm_init(void)
{
	u64 *pml4;

	kernel_pml4 = read_cr3() & PTE_ADDR;
	pml4 = phys_to_virt(kernel_pml4);

	/* The boot-time identity map has served its purpose: NULL now faults. */
	pml4[0] = 0;
	write_cr3(kernel_pml4);
}

u64 vmm_kernel_pml4(void)
{
	return kernel_pml4;
}

void vmm_switch(u64 pml4_phys)
{
	write_cr3(pml4_phys);
}

u64 vmm_as_create(void)
{
	u64 phys = pmm_alloc();

	if (phys) {
		u64 *dst = phys_to_virt(phys);
		const u64 *src = phys_to_virt(kernel_pml4);

		for (int i = 256; i < 512; i++)
			dst[i] = src[i];
	}
	return phys;
}

/* Returns a pointer to the PTE for `va`, optionally building the path to it. */
static u64 *walk(u64 pml4_phys, u64 va, bool create)
{
	u64 *table = phys_to_virt(pml4_phys);

	for (int level = 3; level > 0; level--) {
		u64 *e = &table[pt_index(va, level)];

		if (!(*e & PTE_P)) {
			u64 frame;

			if (!create || !(frame = pmm_alloc()))
				return NULL;
			/* Permissions are enforced at the leaf; keep tables permissive. */
			*e = frame | PTE_P | PTE_W | PTE_U;
		} else if (*e & PTE_PS) {
			return NULL;
		}
		table = phys_to_virt(*e & PTE_ADDR);
	}
	return &table[pt_index(va, 0)];
}

u64 vmm_map_user(u64 pml4_phys, u64 va, u32 prot)
{
	u64 *pte;

	if (va < USER_MIN || va >= USER_TOP)
		return 0;
	pte = walk(pml4_phys, ALIGN_DOWN(va, PAGE_SIZE), true);
	if (!pte)
		return 0;

	if (!(*pte & PTE_P)) {
		u64 frame = pmm_alloc();

		if (!frame)
			return 0;
		*pte = frame | PTE_P | PTE_U | (cpu_nx_enabled() ? PTE_NX : 0);
	}
	/* Two segments may share a page: the page gets the union of their rights. */
	if (prot & VMM_W)
		*pte |= PTE_W;
	if (prot & VMM_X)
		*pte &= ~PTE_NX;
	return *pte & PTE_ADDR;
}

u64 vmm_user_to_phys(u64 pml4_phys, u64 va)
{
	u64 *pte;

	if (va >= USER_TOP)
		return 0;
	pte = walk(pml4_phys, va, false);
	if (!pte || (*pte & (PTE_P | PTE_U)) != (PTE_P | PTE_U))
		return 0;
	return (*pte & PTE_ADDR) | (va & (PAGE_SIZE - 1));
}

bool vmm_user_ok(u64 pml4_phys, u64 va, u64 len, bool write)
{
	u64 end = va + len;

	if (end < va || end > USER_TOP)
		return false;
	for (u64 page = ALIGN_DOWN(va, PAGE_SIZE); page < end; page += PAGE_SIZE) {
		u64 *pte = walk(pml4_phys, page, false);

		if (!pte || (*pte & (PTE_P | PTE_U)) != (PTE_P | PTE_U))
			return false;
		if (write && !(*pte & PTE_W))
			return false;
	}
	return true;
}

/* Free a table at `level` and everything below it. Level 0 entries are data frames. */
static void free_table(u64 table_phys, int level)
{
	u64 *table = phys_to_virt(table_phys);

	for (int i = 0; i < 512; i++) {
		if (!(table[i] & PTE_P))
			continue;
		if (level > 0)
			free_table(table[i] & PTE_ADDR, level - 1);
		else
			pmm_free(table[i] & PTE_ADDR);
	}
	pmm_free(table_phys);
}

void vmm_as_destroy(u64 pml4_phys)
{
	u64 *pml4 = phys_to_virt(pml4_phys);

	for (int i = 0; i < 256; i++)           /* user half only */
		if (pml4[i] & PTE_P)
			free_table(pml4[i] & PTE_ADDR, 2);
	pmm_free(pml4_phys);
}
