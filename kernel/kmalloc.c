/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Small kernel allocator: power-of-two size classes carved out of single
 * frames, larger requests as runs of contiguous frames.
 *
 * Every page starts with a 32-byte header, so kfree() finds the size class
 * from the pointer alone. A page whose last object is freed goes straight
 * back to the frame allocator: nothing is cached, which keeps the boot-time
 * leak check (free frames before == after) meaningful.
 */
#include <mergedos/kernel.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>

#define KM_MAGIC   0x4B4D414Cu          /* "KMAL" */
#define KM_HDR     32
#define KM_CLASSES 6                    /* 32 .. 1024 bytes */
#define KM_LARGE   0xFFFF

struct km_page {
	u32 magic;
	u16 cls;
	u16 used;
	void *free;                     /* free objects in this page (small classes) */
	struct km_page *next;           /* next page with free objects, same class   */
	u64 npages;                     /* large allocations                         */
};

static struct km_page *partial[KM_CLASSES];

static void list_remove(struct km_page **head, struct km_page *pg)
{
	for (; *head; head = &(*head)->next) {
		if (*head == pg) {
			*head = pg->next;
			return;
		}
	}
}

void *kzalloc(size_t size)
{
	u64 flags = irq_save();
	struct km_page *pg;
	unsigned cls = 0;
	void *obj = NULL;

	if (size > (32u << (KM_CLASSES - 1))) {
		u64 n = (size + KM_HDR + PAGE_SIZE - 1) / PAGE_SIZE, phys;

		irq_restore(flags);
		phys = n == 1 ? pmm_alloc() : pmm_alloc_contig(n);
		if (!phys)
			return NULL;
		pg = phys_to_virt(phys);
		pg->magic = KM_MAGIC;
		pg->cls = KM_LARGE;
		pg->npages = n;
		return (u8 *)pg + KM_HDR;
	}
	while ((32u << cls) < size)
		cls++;
	pg = partial[cls];
	if (!pg) {
		u64 phys = pmm_alloc();
		u32 osz = 32u << cls;

		if (!phys) {
			irq_restore(flags);
			return NULL;
		}
		pg = phys_to_virt(phys);
		pg->magic = KM_MAGIC;
		pg->cls = (u16)cls;
		/* Chain every object of the fresh page onto its free list. */
		for (u64 off = osz; off + osz <= PAGE_SIZE; off += osz) {
			void **o = (void **)((u8 *)pg + off);

			*o = pg->free;
			pg->free = o;
		}
		pg->next = partial[cls];
		partial[cls] = pg;
	}
	obj = pg->free;
	pg->free = *(void **)obj;
	pg->used++;
	if (!pg->free)
		list_remove(&partial[cls], pg);
	irq_restore(flags);
	memset(obj, 0, 32u << cls);
	return obj;
}

void kfree(void *p)
{
	struct km_page *pg;
	u64 flags;

	if (!p)
		return;
	pg = (struct km_page *)ALIGN_DOWN((u64)p, PAGE_SIZE);
	if (pg->magic != KM_MAGIC)
		panic("kfree: %p is not a kmalloc pointer", p);
	if (pg->cls == KM_LARGE) {
		u64 n = pg->npages, phys = (u64)pg - DMAP_BASE;

		pg->magic = 0;
		if (n == 1)
			pmm_free(phys);
		else
			pmm_free_contig(phys, n);
		return;
	}
	flags = irq_save();
	if (!pg->free)
		pg->next = partial[pg->cls], partial[pg->cls] = pg;
	*(void **)p = pg->free;
	pg->free = p;
	if (--pg->used == 0) {
		list_remove(&partial[pg->cls], pg);
		pg->magic = 0;
		pmm_free((u64)pg - DMAP_BASE);
	}
	irq_restore(flags);
}
