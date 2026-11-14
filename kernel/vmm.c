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
#define PTE_COW   (1ULL << 9)           /* software: a write needs a private copy */
#define PTE_SHR   (1ULL << 10)          /* software: shared mapping, never COW    */
#define PTE_PNONE (1ULL << 11)          /* software: frame kept behind PROT_NONE  */
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

/* A leaf that owns a frame: present, or kept behind PROT_NONE. */
static inline bool pte_has_frame(u64 e)
{
	return e & (PTE_P | PTE_PNONE);
}

static inline void tlb_flush_if_current(u64 pml4_phys)
{
	if ((read_cr3() & PTE_ADDR) == pml4_phys)
		write_cr3(pml4_phys);
}

/* Hardware bits for a mapped leaf with rights `prot`; `e` supplies frame and software bits. */
static u64 pte_with_rights(u64 e, u32 prot)
{
	u64 frame = e & PTE_ADDR, soft = e & (PTE_COW | PTE_SHR);

	if (!(prot & (VMM_R | VMM_W | VMM_X)))
		return frame | soft | PTE_PNONE;                /* not present, frame kept */
	e = frame | soft | PTE_P | PTE_U;
	if ((prot & VMM_W) && !(soft & PTE_COW)) {
		if (!(soft & PTE_SHR) && pmm_refs(frame) > 1)
			e |= PTE_COW;                           /* first write copies */
		else
			e |= PTE_W;
	}
	if (cpu_nx_enabled() && !(prot & VMM_X))
		e |= PTE_NX;
	return e;
}

u64 vmm_map_user(u64 pml4_phys, u64 va, u32 prot)
{
	u64 *pte;

	if (va < USER_MIN || va >= USER_TOP)
		return 0;
	pte = walk(pml4_phys, ALIGN_DOWN(va, PAGE_SIZE), true);
	if (!pte)
		return 0;

	if (!pte_has_frame(*pte)) {
		u64 frame = pmm_alloc();

		if (!frame)
			return 0;
		*pte = frame | PTE_P | PTE_U | (cpu_nx_enabled() ? PTE_NX : 0);
	} else if (!(*pte & PTE_P)) {
		*pte = pte_with_rights(*pte, VMM_R);
	}
	/* Two segments may share a page: the page gets the union of their rights. */
	if ((prot & VMM_W) && !(*pte & PTE_COW))
		*pte |= PTE_W;
	if (prot & VMM_X)
		*pte &= ~PTE_NX;
	return *pte & PTE_ADDR;
}

/* Map an existing frame (the caller's reference moves into the page table). */
bool vmm_map_frame(u64 pml4_phys, u64 va, u64 frame, u32 prot, bool shared)
{
	u64 *pte;

	if (va < USER_MIN || va >= USER_TOP)
		return false;
	pte = walk(pml4_phys, ALIGN_DOWN(va, PAGE_SIZE), true);
	if (!pte || pte_has_frame(*pte))
		return false;
	*pte = pte_with_rights(frame | (shared ? PTE_SHR : 0), prot);
	return true;
}

bool vmm_remap_user(u64 pml4_phys, u64 va, u32 prot)
{
	u64 *pte = walk(pml4_phys, ALIGN_DOWN(va, PAGE_SIZE), false);

	if (!pte || !pte_has_frame(*pte))
		return false;
	*pte = pte_with_rights(*pte, prot);
	if ((read_cr3() & PTE_ADDR) == pml4_phys)
		__asm__ volatile("invlpg (%0)" : : "r"(ALIGN_DOWN(va, PAGE_SIZE)) : "memory");
	return true;
}

/* Detach a page's frame without freeing it (mremap): the reference moves to the caller. */
u64 vmm_take_frame(u64 pml4_phys, u64 va, bool *shared)
{
	u64 *pte = walk(pml4_phys, ALIGN_DOWN(va, PAGE_SIZE), false), frame;

	if (!pte || !pte_has_frame(*pte))
		return 0;
	frame = *pte & PTE_ADDR;
	*shared = *pte & PTE_SHR;
	*pte = 0;
	if ((read_cr3() & PTE_ADDR) == pml4_phys)
		__asm__ volatile("invlpg (%0)" : : "r"(ALIGN_DOWN(va, PAGE_SIZE)) : "memory");
	return frame;
}

/* 0: nothing; 1: mapped; 3: kept behind PROT_NONE; +4: copy-on-write. */
int vmm_page_state(u64 pml4_phys, u64 va)
{
	u64 *pte = walk(pml4_phys, ALIGN_DOWN(va, PAGE_SIZE), false);

	if (!pte || !pte_has_frame(*pte))
		return 0;
	return 1 | ((*pte & PTE_P) ? 0 : 2) | ((*pte & PTE_COW) ? 4 : 0);
}

/* Resolve a write fault on a copy-on-write page: 0 done, -MERR_FAULT not COW, -MERR_NOMEM. */
int vmm_cow_fault(u64 pml4_phys, u64 va)
{
	u64 *pte = walk(pml4_phys, ALIGN_DOWN(va, PAGE_SIZE), false), frame, copy;

	if (!pte || !(*pte & PTE_P) || !(*pte & PTE_COW))
		return -MERR_FAULT;
	frame = *pte & PTE_ADDR;
	if (pmm_refs(frame) > 1) {
		copy = pmm_alloc();
		if (!copy)
			return -MERR_NOMEM;
		memcpy(phys_to_virt(copy), phys_to_virt(frame), PAGE_SIZE);
		*pte = (*pte & ~(PTE_ADDR | PTE_COW)) | copy | PTE_W;
		pmm_free(frame);
	} else {
		*pte = (*pte & ~PTE_COW) | PTE_W;
	}
	__asm__ volatile("invlpg (%0)" : : "r"(ALIGN_DOWN(va, PAGE_SIZE)) : "memory");
	return 0;
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
		if (!(table[i] & (PTE_P | PTE_PNONE)))
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

/*
 * Visit every leaf that owns a frame in [from, to), skipping empty subtrees
 * (so a terabyte-sized reservation costs only what was ever touched).
 */
typedef void (*leaf_fn)(u64 *pte, void *arg);

static void range_walk(u64 table_phys, int level, u64 base, u64 from, u64 to, leaf_fn fn, void *arg)
{
	u64 *table = phys_to_virt(table_phys);
	u64 span = 1ULL << (PAGE_SHIFT + 9 * level);

	for (unsigned i = from > base ? (unsigned)((from - base) / span) : 0; i < 512; i++) {
		u64 lo = base + i * span;

		if (lo >= to)
			break;
		if (level == 0) {
			if (pte_has_frame(table[i]))
				fn(&table[i], arg);
		} else if (table[i] & PTE_P) {
			range_walk(table[i] & PTE_ADDR, level - 1, lo, from, to, fn, arg);
		}
	}
}

static void leaf_free(u64 *pte, void *arg)
{
	pmm_free(*pte & PTE_ADDR);
	*pte = 0;
	++*(u32 *)arg;
}

static void leaf_rights(u64 *pte, void *arg)
{
	*pte = pte_with_rights(*pte, *(u32 *)arg);
}

static void for_range(u64 pml4_phys, u64 from, u64 to, leaf_fn fn, void *arg)
{
	const u64 *pml4 = phys_to_virt(pml4_phys);

	for (unsigned i = (unsigned)(from >> 39); i < 256 && ((u64)i << 39) < to; i++)
		if (pml4[i] & PTE_P)
			range_walk(pml4[i] & PTE_ADDR, 2, (u64)i << 39, from, to, fn, arg);
	tlb_flush_if_current(pml4_phys);
}

/* Remove every page in [from, to) and free the frames; returns how many were mapped. */
u32 vmm_unmap_range(u64 pml4_phys, u64 from, u64 to)
{
	u32 n = 0;

	for_range(pml4_phys, ALIGN_DOWN(from, PAGE_SIZE), ALIGN_UP(to, PAGE_SIZE), leaf_free, &n);
	return n;
}

/* Set the rights of every mapped page in [from, to). */
void vmm_protect_range(u64 pml4_phys, u64 from, u64 to, u32 prot)
{
	for_range(pml4_phys, ALIGN_DOWN(from, PAGE_SIZE), ALIGN_UP(to, PAGE_SIZE), leaf_rights, &prot);
}

/* Remove one user page and free its frame. Returns false if nothing was mapped. */
bool vmm_unmap_user(u64 pml4_phys, u64 va)
{
	u64 *pte = walk(pml4_phys, ALIGN_DOWN(va, PAGE_SIZE), false);

	if (!pte || !pte_has_frame(*pte))
		return false;
	pmm_free(*pte & PTE_ADDR);
	*pte = 0;
	if ((read_cr3() & PTE_ADDR) == pml4_phys)
		__asm__ volatile("invlpg (%0)" : : "r"(ALIGN_DOWN(va, PAGE_SIZE)) : "memory");
	return true;
}

/*
 * Copy a table and everything below it. Leaf frames are shared, not copied:
 * writable private pages become copy-on-write in both spaces; pages of a
 * shared mapping stay writable in both.
 */
static u64 clone_table(u64 src_phys, int level)
{
	u64 *src = phys_to_virt(src_phys);
	u64 dst_phys = pmm_alloc(), *dst;

	if (!dst_phys)
		return 0;
	dst = phys_to_virt(dst_phys);
	for (int i = 0; i < 512; i++) {
		u64 e = src[i], copy;

		if (!(e & (PTE_P | PTE_PNONE)))
			continue;
		if (level > 0) {
			copy = clone_table(e & PTE_ADDR, level - 1);
			if (!copy) {
				free_table(dst_phys, level);
				return 0;
			}
			dst[i] = copy | (e & ~PTE_ADDR);
		} else {
			if (!(e & PTE_SHR) && (e & (PTE_W | PTE_COW))) {
				e = (e & ~PTE_W) | PTE_COW;
				src[i] = e;
			}
			pmm_ref(e & PTE_ADDR);
			dst[i] = e;
		}
	}
	return dst_phys;
}

/* fork(): a new address space sharing every user page copy-on-write. */
u64 vmm_as_clone(u64 src_pml4)
{
	u64 dst = vmm_as_create();
	const u64 *s = phys_to_virt(src_pml4);
	u64 *d;

	if (!dst)
		return 0;
	d = phys_to_virt(dst);
	for (int i = 0; i < 256; i++) {
		u64 copy;

		if (!(s[i] & PTE_P))
			continue;
		copy = clone_table(s[i] & PTE_ADDR, 2);
		if (!copy) {
			vmm_as_destroy(dst);
			return 0;
		}
		d[i] = copy | (s[i] & ~PTE_ADDR);
	}
	tlb_flush_if_current(src_pml4);
	return dst;
}
