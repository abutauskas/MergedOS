/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Physical frame allocator: one bit per 4 KiB frame.
 *
 * Footprint is the point here. The bitmap costs 1/32768 of managed RAM
 * (2 KiB for a 64 MiB machine, 128 KiB for 4 GiB) and is sized from the
 * firmware memory map at boot, so a small machine pays for a small map.
 *
 * PoC limit: only frames below DMAP_LIMIT (4 GiB) are managed, because that
 * is what the boot-time direct map covers.
 */
#include <mergedos/kernel.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>

#define LOW_RESERVED 0x100000ULL        /* never hand out the first 1 MiB */

static u8 *bitmap;                      /* bit set = frame in use / not RAM */
static u64 nframes;
static u64 next_hint;
static struct pmm_stats stats;

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
	if (top > DMAP_LIMIT)
		top = DMAP_LIMIT;
	top = ALIGN_DOWN(top, PAGE_SIZE);

	nframes = top >> PAGE_SHIFT;
	bitmap_bytes = (nframes + 7) / 8;
	bitmap_phys = ALIGN_UP(kernel_end, PAGE_SIZE);
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

	next_hint = (bitmap_phys + bitmap_bytes) >> PAGE_SHIFT;
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

void pmm_free(u64 phys)
{
	u64 f = phys >> PAGE_SHIFT, flags = irq_save();

	if (f >= nframes || !frame_used(f))
		panic("pmm_free: bad or double free of frame %llx", (unsigned long long)phys);
	frame_clear(f);
	stats.free_frames++;
	if (f < next_hint)
		next_hint = f;
	irq_restore(flags);
}

void pmm_get_stats(struct pmm_stats *out)
{
	*out = stats;
}
