/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Physical frame allocator: one bit per 4 KiB frame.
 *
 * Footprint is the point here. The bitmap costs 1/32768 of managed RAM
 * (2 KiB for a 64 MiB machine, 128 KiB for 4 GiB) and is sized from the
 * firmware memory map at boot, so a small machine pays for a small map.
 *
 * The boot page tables map physical 0..4 GiB at DMAP_BASE; RAM above that
 * is added to the direct map here, with 2 MiB pages, before any of it can
 * be handed out (up to DMAP_RAM_MAX, one PML4 slot).
 */
#include <mergedos/kernel.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>

#define LOW_RESERVED 0x100000ULL        /* never hand out the first 1 MiB */

static u8 *bitmap;                      /* bit set = frame in use / not RAM */
static u64 nframes;
static u64 next_hint;
static struct pmm_stats stats;
u64 dmap_top = DMAP_LIMIT;

static inline bool frame_used(u64 f)   { return bitmap[f >> 3] & (1u << (f & 7)); }
static inline void frame_set(u64 f)    { bitmap[f >> 3] |= (u8)(1u << (f & 7)); }
static inline void frame_clear(u64 f)  { bitmap[f >> 3] &= (u8)~(1u << (f & 7)); }

static void reserve_range(u64 start, u64 end, u64 *counter)
{
	for (u64 f = start >> PAGE_SHIFT; f < (ALIGN_UP(end, PAGE_SIZE) >> PAGE_SHIFT); f++) {
		if (f < nframes && !frame_used(f)) {
			frame_set(f);
			stats.free_frames--;
			if (counter)
				(*counter)++;
		}
	}
}

/*
 * Map RAM above 4 GiB into the direct map: one page directory (512 x 2 MiB)
 * per GiB, hung off the direct map's PDPT, which every address space shares.
 * The directories come from low memory, which is already mapped.
 */
static void dmap_extend(u64 top)
{
	u64 *pml4 = phys_to_virt(read_cr3() & ~0xFFFULL);
	u64 *pdpt = phys_to_virt(pml4[DMAP_BASE >> 39 & 0x1FF] & 0x000FFFFFFFFFF000ULL);

	for (u64 gib = DMAP_LIMIT >> 30; gib < (top + (1ULL << 30) - 1) >> 30; gib++) {
		u64 pd_phys = pmm_alloc(), *pd;

		if (!pd_phys)
			panic("no memory for the direct map above 4 GiB");
		stats.kernel_frames++;
		pd = phys_to_virt(pd_phys);
		for (u64 i = 0; i < 512; i++)
			pd[i] = (gib << 30 | i << 21) | 0x83;  /* present, writable, 2 MiB */
		pdpt[gib] = pd_phys | 0x3;
	}
	dmap_top = MOS_MAX(top, DMAP_LIMIT);
	if (top > DMAP_LIMIT) {
		/* Prove the new mapping: the very last frame of RAM is reachable. */
		volatile u64 *last = phys_to_virt(top - 8);
		u64 old = *last;

		*last = 0x4D45524745444F53ULL;
		if (*last != 0x4D45524745444F53ULL)
			panic("direct map above 4 GiB does not work");
		*last = old;
		kprintf("[mem] direct map extended to %llu MiB of RAM\n",
			(unsigned long long)(top >> 20));
	}
}

void pmm_init(const struct boot_info *bi)
{
	u64 kernel_start = (u64)__kernel_start - KERNEL_VMA;
	u64 kernel_end = (u64)__kernel_end - KERNEL_VMA;
	u64 top = 0, bitmap_phys, bitmap_bytes;

	for (u32 i = 0; i < bi->nmmap; i++) {
		u64 end = bi->mmap[i].base + bi->mmap[i].len;

		if (bi->mmap[i].type == BOOT_MEM_AVAILABLE && end > top)
			top = end;
	}
	if (top > DMAP_RAM_MAX)
		top = DMAP_RAM_MAX;
	top = ALIGN_DOWN(top, PAGE_SIZE);

	nframes = top >> PAGE_SHIFT;
	bitmap_bytes = (nframes + 7) / 8;
	/* Boot loaders put modules right after the kernel: keep the bitmap clear of them. */
	bitmap_phys = ALIGN_UP(MOS_MAX(kernel_end, bi->mod_end), PAGE_SIZE);
	if (bitmap_phys + bitmap_bytes > top)
		panic("not enough RAM for the frame bitmap (top=%llx)", (unsigned long long)top);

	bitmap = phys_to_virt(bitmap_phys);
	memset(bitmap, 0xFF, bitmap_bytes);

	/* Free every whole frame of available RAM above the low-memory hole. */
	for (u32 i = 0; i < bi->nmmap; i++) {
		u64 start, end;

		if (bi->mmap[i].type != BOOT_MEM_AVAILABLE)
			continue;
		start = ALIGN_UP(MOS_MAX(bi->mmap[i].base, LOW_RESERVED), PAGE_SIZE);
		end = ALIGN_DOWN(MOS_MIN(bi->mmap[i].base + bi->mmap[i].len, top), PAGE_SIZE);
		for (u64 f = start >> PAGE_SHIFT; start < end && f < (end >> PAGE_SHIFT); f++) {
			if (frame_used(f)) {
				frame_clear(f);
				stats.usable_frames++;
			}
		}
	}
	stats.free_frames = stats.usable_frames;
	stats.managed_top = top;

	reserve_range(kernel_start, kernel_end, &stats.kernel_frames);
	reserve_range(bitmap_phys, bitmap_phys + bitmap_bytes, &stats.bitmap_frames);
	if (bi->mod_end)
		reserve_range(bi->mod_start, bi->mod_end, &stats.module_frames);

	next_hint = (bitmap_phys + bitmap_bytes) >> PAGE_SHIFT;
	dmap_extend(top);
	stats.peak_used_frames = stats.usable_frames - stats.free_frames;
}

u64 pmm_alloc(void)
{
	u64 flags = irq_save();

	for (u64 n = 0; n < nframes; n++) {
		u64 f = next_hint + n;

		if (f >= nframes)
			f -= nframes;
		if (!frame_used(f)) {
			u64 used;

			frame_set(f);
			next_hint = f + 1;
			stats.free_frames--;
			used = stats.usable_frames - stats.free_frames;
			if (used > stats.peak_used_frames)
				stats.peak_used_frames = used;
			irq_restore(flags);
			memset(phys_to_virt(f << PAGE_SHIFT), 0, PAGE_SIZE);
			return f << PAGE_SHIFT;
		}
	}
	irq_restore(flags);
	return 0;
}

/*
 * Frames mapped more than once (copy-on-write after fork, shared mappings)
 * carry extra references in a small hash table; a frame with no entry has
 * exactly one owner, so the common case costs no memory at all.
 */
struct refnode {
	struct refnode *next;
	u64 frame;
	u32 extra;                      /* references beyond the first */
};
static struct refnode *reftab[256];

static struct refnode **ref_slot(u64 f)
{
	struct refnode **pp = &reftab[(f ^ (f >> 8)) & 255];

	while (*pp && (*pp)->frame != f)
		pp = &(*pp)->next;
	return pp;
}

void pmm_ref(u64 phys)
{
	u64 f = phys >> PAGE_SHIFT, flags = irq_save();
	struct refnode **pp = ref_slot(f);

	if (*pp) {
		(*pp)->extra++;
	} else {
		struct refnode *n = kzalloc(sizeof(*n));

		if (!n)
			panic("pmm_ref: out of memory for the reference table");
		n->frame = f;
		n->extra = 1;
		*pp = n;
	}
	irq_restore(flags);
}

u32 pmm_ref_entries(void)
{
	u32 n = 0;

	for (unsigned i = 0; i < 256; i++)
		for (struct refnode *r = reftab[i]; r; r = r->next)
			n++;
	return n;
}

u32 pmm_refs(u64 phys)
{
	u64 flags = irq_save();
	struct refnode *n = *ref_slot(phys >> PAGE_SHIFT);
	u32 r = n ? n->extra + 1 : 1;

	irq_restore(flags);
	return r;
}

/* Drop one reference; the frame goes back to the allocator with the last. */
void pmm_free(u64 phys)
{
	u64 f = phys >> PAGE_SHIFT, flags = irq_save();
	struct refnode **pp = ref_slot(f), *n = *pp;

	if (n) {
		if (--n->extra == 0) {
			*pp = n->next;
			kfree(n);
		}
		irq_restore(flags);
		return;
	}
	if (f >= nframes || !frame_used(f))
		panic("pmm_free: bad or double free of frame %llx", (unsigned long long)phys);
	frame_clear(f);
	stats.free_frames++;
	if (f < next_hint)
		next_hint = f;
	irq_restore(flags);
}

/* n physically contiguous zeroed frames (first-fit), 0 when none. */
u64 pmm_alloc_contig(u64 n)
{
	u64 flags = irq_save(), run = 0, used;

	for (u64 f = 0; f < nframes && n; f++) {
		run = frame_used(f) ? 0 : run + 1;
		if (run == n) {
			u64 first = f + 1 - n;

			for (u64 i = first; i <= f; i++)
				frame_set(i);
			stats.free_frames -= n;
			used = stats.usable_frames - stats.free_frames;
			if (used > stats.peak_used_frames)
				stats.peak_used_frames = used;
			irq_restore(flags);
			memset(phys_to_virt(first << PAGE_SHIFT), 0, n << PAGE_SHIFT);
			return first << PAGE_SHIFT;
		}
	}
	irq_restore(flags);
	return 0;
}

void pmm_free_contig(u64 phys, u64 n)
{
	for (u64 i = 0; i < n; i++)
		pmm_free(phys + (i << PAGE_SHIFT));
}

void pmm_get_stats(struct pmm_stats *out)
{
	*out = stats;
}
