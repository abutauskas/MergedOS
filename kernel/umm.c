/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * A process's virtual memory: a sorted array of VMAs over the page tables.
 * The VMA is the truth about rights; page-table entries are a cache filled
 * on first touch (zero page, private copy of a file image, or the frame of a
 * shared ramfs file), unshared by copy-on-write after fork, and demoted to
 * "kept but inaccessible" by PROT_NONE. Reservations of up to 128 TiB cost
 * nothing until touched. docs/WINE_STACK.md.
 */
#include <mergedos/kernel.h>
#include <mergedos/cpu.h>
#include <mergedos/io.h>
#include <mergedos/sched.h>
#include <mergedos/mm.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>

#define MMAP_TOP   0x00007EFF00000000ULL        /* unhinted mappings grow down from here */
#define MMAP_NORESERVE 0x4000u
#define MREMAP_MAYMOVE 1u
#define MREMAP_FIXED   2u

struct vma *umm_find(struct task *t, u64 va)
{
	u32 lo = 0, hi = t->nvma;

	while (lo < hi) {                       /* first VMA with end > va */
		u32 mid = (lo + hi) / 2;

		if (t->vma[mid].end > va)
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo < t->nvma && t->vma[lo].start <= va ? &t->vma[lo] : NULL;
}

/* Index of the first VMA that ends after `addr`. */
static u32 vma_first_after(const struct task *t, u64 addr)
{
	u32 lo = 0, hi = t->nvma;

	while (lo < hi) {
		u32 mid = (lo + hi) / 2;

		if (t->vma[mid].end > addr)
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

static bool range_free(const struct task *t, u64 a, u64 b)
{
	u32 i = vma_first_after(t, a);

	return i >= t->nvma || t->vma[i].start >= b;
}

static int vma_insert(struct task *t, u32 idx, const struct vma *v)
{
	if (t->nvma == t->vma_cap) {
		u32 cap = t->vma_cap ? t->vma_cap * 2 : 32;
		struct vma *n = kzalloc(cap * sizeof(*n));

		if (!n)
			return -MERR_NOMEM;
		if (t->nvma)
			memcpy(n, t->vma, t->nvma * sizeof(*n));
		kfree(t->vma);
		t->vma = n;
		t->vma_cap = cap;
	}
	memmove(&t->vma[idx + 1], &t->vma[idx], (t->nvma - idx) * sizeof(*v));
	t->vma[idx] = *v;
	t->nvma++;
	return 0;
}

static void vma_remove(struct task *t, u32 idx)
{
	memmove(&t->vma[idx], &t->vma[idx + 1], (t->nvma - idx - 1) * sizeof(struct vma));
	t->nvma--;
}

/* The part of `v` from address `to` onwards: file bytes stay aligned with addresses. */
static void vma_advance(struct vma *v, u64 to)
{
	u64 d = to - v->start;

	if (v->src) {
		v->src += d;
		v->srclen = v->srclen > d ? v->srclen - d : 0;
		if (!v->srclen)
			v->src = NULL;
	}
	v->foff += d;
	v->start = to;
}

/* Remove [from, to) from the address space: VMAs trimmed or split, pages freed. */
static int carve(struct task *t, u64 from, u64 to)
{
	u32 freed;
	u32 i = vma_first_after(t, from);

	/* A split needs a free slot: make room first so it cannot fail halfway. */
	if (i < t->nvma && t->vma[i].start < from && t->vma[i].end > to && t->nvma == t->vma_cap) {
		struct vma tmp = t->vma[i];
		int rc = vma_insert(t, i, &tmp);

		if (rc)
			return rc;
		vma_remove(t, i);
	}
	while (i < t->nvma && t->vma[i].start < to) {
		struct vma *v = &t->vma[i];
		u64 lo = MOS_MAX(v->start, from), hi = MOS_MIN(v->end, to);

		t->vm_reserved -= hi - lo;
		if (lo == v->start && hi == v->end) {
			if (v->file)
				file_put(v->file);
			vma_remove(t, i);
			continue;
		}
		if (lo == v->start) {
			vma_advance(v, hi);
		} else if (hi == v->end) {
			v->end = lo;
		} else {
			struct vma tail = *v;

			vma_advance(&tail, hi);
			if (tail.file)
				file_get(tail.file);
			v->end = lo;
			vma_insert(t, i + 1, &tail);    /* room was made above */
			i++;
		}
		i++;
	}
	freed = vmm_unmap_range(t->pml4, from, to);
	t->pages -= MOS_MIN(t->pages, freed);
	return 0;
}

void umm_release(struct task *t)
{
	for (u32 i = 0; i < t->nvma; i++)
		if (t->vma[i].file)
			file_put(t->vma[i].file);
	kfree(t->vma);
	t->vma = NULL;
	t->nvma = t->vma_cap = 0;
	t->vm_reserved = 0;
}

int umm_fork(struct task *c, const struct task *p)
{
	if (!p->nvma)
		return 0;
	c->vma = kzalloc(p->vma_cap * sizeof(struct vma));
	if (!c->vma)
		return -MERR_NOMEM;
	memcpy(c->vma, p->vma, p->nvma * sizeof(struct vma));
	c->nvma = p->nvma;
	c->vma_cap = p->vma_cap;
	for (u32 i = 0; i < c->nvma; i++)
		if (c->vma[i].file)
			file_get(c->vma[i].file);
	return 0;
}

/* Add VMAs for the parts of [start, end) not covered yet (exec-time mappings). */
int umm_cover(struct task *t, u64 start, u64 end, u32 prot)
{
	u64 pos = ALIGN_DOWN(start, PAGE_SIZE);

	end = ALIGN_UP(end, PAGE_SIZE);
	while (pos < end) {
		u32 i = vma_first_after(t, pos);
		u64 stop = (i < t->nvma && t->vma[i].start < end) ? t->vma[i].start : end;
		int rc;

		if (stop > pos) {
			struct vma v = { .start = pos, .end = stop, .prot = prot };

			rc = vma_insert(t, i, &v);
			if (rc)
				return rc;
			t->vm_reserved += stop - pos;
			pos = stop;
		} else {
			pos = t->vma[i].end;
		}
	}
	return 0;
}

/* Highest free range of `len` bytes between TASK_MMAP_BASE and MMAP_TOP, or 0. */
static u64 find_gap(const struct task *t, u64 len)
{
	u64 hi = MMAP_TOP;

	for (u32 i = t->nvma; i-- > 0;) {
		const struct vma *v = &t->vma[i];

		if (v->start >= hi)
			continue;
		if (v->end <= hi && hi - v->end >= len && hi - len >= TASK_MMAP_BASE)
			return hi - len;
		hi = v->start;
	}
	return hi - TASK_MMAP_BASE >= len ? hi - len : 0;
}

/* ---- demand paging, copy-on-write ------------------------------------------------- */

int task_fault_in(struct task *t, u64 va, bool write)
{
	struct vma *v = umm_find(t, va);
	u64 page = ALIGN_DOWN(va, PAGE_SIZE);
	int st;

	if (!v || !v->prot || (write && !(v->prot & VMM_W)))
		return -MERR_FAULT;
	st = vmm_page_state(t->pml4, page);
	if (st) {                                       /* a frame is there: fix its rights */
		if (st & 2) {
			vmm_remap_user(t->pml4, page, v->prot);
			st = vmm_page_state(t->pml4, page);
		}
		if (write && (st & 4))
			return vmm_cow_fault(t->pml4, page);
		if (write && !vmm_user_ok(t->pml4, page, 1, true))
			vmm_remap_user(t->pml4, page, v->prot);
		return vmm_user_ok(t->pml4, page, 1, write) ? 0 : -MERR_FAULT;
	}
	if (t->pages >= TASK_PAGE_LIMIT)
		return -MERR_NOMEM;
	if ((v->flags & VMA_SHARED) && v->file) {
		/* The frame of the shared file itself: both mappings see one page. */
		struct rnode *n = v->file->node;
		u64 off = v->foff + (page - v->start), frame;

		if (off >= ALIGN_UP(n->size, PAGE_SIZE) || off + PAGE_SIZE > n->cap ||
		    ((u64)n->data & (PAGE_SIZE - 1)))
			return -MERR_FAULT;             /* SIGBUS: beyond the end of the file */
		frame = (u64)(n->data + off) - DMAP_BASE;
		pmm_ref(frame);
		if (!vmm_map_frame(t->pml4, page, frame, v->prot, true)) {
			pmm_free(frame);
			return -MERR_NOMEM;
		}
		t->pages++;
		return 0;
	}
	if (v->src && page < v->start + v->srclen) {    /* private copy of a file image */
		u64 off = page - v->start, n = MOS_MIN((u64)PAGE_SIZE, v->srclen - off);

		if (!vmm_map_user(t->pml4, page, VMM_R | VMM_W))
			return -MERR_NOMEM;
		t->pages++;
		memcpy(phys_to_virt(vmm_user_to_phys(t->pml4, page)), v->src + off, n);
		vmm_remap_user(t->pml4, page, v->prot);
		return 0;
	}
	if (!vmm_map_user(t->pml4, page, v->prot))
		return -MERR_NOMEM;
	t->pages++;
	return 0;
}

/* ---- brk ---------------------------------------------------------------------------- */

long msys_brk(struct task *t, u64 addr)
{
	u64 cur_end = ALIGN_UP(t->brk, PAGE_SIZE), new_end;
	struct vma *h = cur_end > t->brk_start ? umm_find(t, cur_end - 1) : NULL;

	/* Linux semantics: on any failure the old break is returned unchanged. */
	if (addr < t->brk_start)
		return (long)t->brk;
	new_end = ALIGN_UP(addr, PAGE_SIZE);
	if (new_end > cur_end) {
		u64 from = h && (h->flags & VMA_HEAP) ? cur_end : t->brk_start;

		if (new_end >= USER_STACK_TOP - USER_STACK_MAX || !range_free(t, from, new_end))
			return (long)t->brk;
		if (h && (h->flags & VMA_HEAP)) {
			h->end = new_end;
			t->vm_reserved += new_end - cur_end;
		} else {
			struct vma v = { .start = t->brk_start, .end = new_end, .prot = VMM_R | VMM_W,
					 .flags = VMA_HEAP };

			if (vma_insert(t, vma_first_after(t, t->brk_start), &v))
				return (long)t->brk;
			t->vm_reserved += new_end - t->brk_start;
		}
	} else if (new_end < cur_end) {
		carve(t, MOS_MAX(new_end, t->brk_start), cur_end);
	}
	t->brk = addr;
	return (long)t->brk;
}

/* ---- mmap family -------------------------------------------------------------------- */

long msys_mmap_anon(struct task *t, u64 len)
{
	return msys_mmap(t, 0, len, VMM_R | VMM_W, MSYS_MAP_ANON, -1, 0);
}

/*
 * mmap for every personality (prot in VMM_* bits, flags as on Linux). File
 * mappings: an immutable ramdisk/sysroot image is demand-paged as a private
 * copy; a MAP_PRIVATE ramfs file is copied at once; a MAP_SHARED ramfs file
 * maps the file's own frames. Without MAP_FIXED a free hint is honoured,
 * otherwise the highest free range below MMAP_TOP is used.
 */
long msys_mmap(struct task *t, u64 addr, u64 len, u32 prot, u32 flags, int fd, u64 offset)
{
	struct vma v = { .prot = prot };
	struct file *f = NULL;
	bool fixed = flags & (MSYS_MAP_FIXED | MSYS_MAP_FIXED_NOREPLACE), eager = false;
	u64 srcn = 0;
	int rc;

	if (!len || len > TASK_VM_LIMIT)
		return -MERR_INVAL;
	len = ALIGN_UP(len, PAGE_SIZE);
	if (!(flags & MSYS_MAP_ANON)) {
		const u8 *src;
		bool imm = false;
		long avail;

		if (offset & (PAGE_SIZE - 1))
			return -MERR_INVAL;
		f = fd_file(t, fd);
		if (!f)
			return -MERR_BADF;
		avail = vfs_mmap_source(t, fd, offset, &src, &imm);
		if (avail < 0)
			return avail;
		srcn = MOS_MIN((u64)avail, len);
		if (imm) {
			v.src = src;
			v.srclen = srcn;
		} else if (flags & MSYS_MAP_SHARED) {
			if (ramfs_pin(f->node, f->node->size))
				return -MERR_NOMEM;
			v.flags |= VMA_SHARED;
			v.file = file_get(f);
			v.foff = offset;
		} else {
			eager = true;
		}
	} else if (flags & MSYS_MAP_SHARED) {
		v.flags |= VMA_SHARED;          /* anonymous shared: private until forked */
	}
	if (len > TASK_VM_LIMIT - t->vm_reserved)
		goto nomem;

	if (fixed) {
		addr = ALIGN_DOWN(addr, PAGE_SIZE);
		if (addr < USER_MIN || addr + len > USER_TOP || addr + len < addr) {
			rc = -MERR_INVAL;
			goto fail;
		}
		if ((flags & MSYS_MAP_FIXED_NOREPLACE) && !range_free(t, addr, addr + len)) {
			rc = -MERR_EXIST;
			goto fail;
		}
		rc = carve(t, addr, addr + len);
		if (rc)
			goto fail;
	} else {
		addr = ALIGN_DOWN(addr, PAGE_SIZE);
		if (!addr || addr < USER_MIN || addr + len > USER_TOP || addr + len < addr ||
		    !range_free(t, addr, addr + len))
			addr = find_gap(t, len);
		if (!addr)
			goto nomem;
	}
	v.start = addr;
	v.end = addr + len;
	rc = vma_insert(t, vma_first_after(t, addr), &v);
	if (rc)
		goto fail;
	t->vm_reserved += len;

	if (eager) {                            /* private copy of a mutable ramfs file */
		const u8 *ram;
		bool imm;
		long got = vfs_mmap_source(t, fd, offset, &ram, &imm);

		for (u64 p = 0; got > 0 && p < srcn; p += PAGE_SIZE) {
			u64 n = MOS_MIN((u64)PAGE_SIZE, srcn - p);

			if (!vmm_map_user(t->pml4, addr + p, VMM_R | VMM_W)) {
				msys_munmap(t, addr, len);
				return -MERR_NOMEM;
			}
			t->pages++;
			memcpy(phys_to_virt(vmm_user_to_phys(t->pml4, addr + p)), ram + p, n);
			vmm_remap_user(t->pml4, addr + p, prot);
		}
	}
	return (long)addr;
nomem:
	rc = -MERR_NOMEM;
fail:
	if (v.file)
		file_put(v.file);
	return rc;
}

long msys_munmap(struct task *t, u64 addr, u64 len)
{
	u64 end = addr + ALIGN_UP(len, PAGE_SIZE);

	if ((addr & (PAGE_SIZE - 1)) || !len || end < addr || end > USER_TOP)
		return -MERR_INVAL;
	return carve(t, addr, end);
}

/* mprotect: exact rights for [addr, addr + len); every byte must be mapped. */
long umm_protect(struct task *t, u64 addr, u64 len, u32 prot)
{
	u64 end = ALIGN_UP(addr + len, PAGE_SIZE), pos;
	u32 i;

	if ((addr & (PAGE_SIZE - 1)) || end < addr || end > USER_TOP)
		return -MERR_INVAL;
	if (!len)
		return 0;
	for (pos = addr, i = vma_first_after(t, addr); pos < end; i++) {   /* all mapped? */
		if (i >= t->nvma || t->vma[i].start > pos)
			return -MERR_NOMEM;
		pos = t->vma[i].end;
	}
	/* Split at both edges so the range is made of whole VMAs. */
	for (int edge = 0; edge < 2; edge++) {
		u64 at = edge ? end : addr;
		struct vma *v = umm_find(t, at);

		if (v && v->start < at) {
			struct vma tail = *v;
			u32 idx = (u32)(v - t->vma);

			vma_advance(&tail, at);
			if (tail.file)
				file_get(tail.file);
			if (vma_insert(t, idx + 1, &tail))
				return -MERR_NOMEM;
			t->vma[idx].end = at;
		}
	}
	for (i = vma_first_after(t, addr); i < t->nvma && t->vma[i].start < end; i++)
		t->vma[i].prot = prot;
	vmm_protect_range(t->pml4, addr, end, prot);
	return 0;
}

long msys_protect(struct task *t, u64 addr, u64 len, u32 prot)
{
	if (prot & MSYS_PROT_NONE)
		return umm_protect(t, addr, len, 0);
	return umm_protect(t, addr, len, VMM_R | (prot & MSYS_PROT_W ? VMM_W : 0) |
					(prot & MSYS_PROT_X ? VMM_X : 0));
}

long umm_madvise(struct task *t, u64 addr, u64 len, int advice)
{
	u64 end = ALIGN_UP(addr + len, PAGE_SIZE);

	if ((addr & (PAGE_SIZE - 1)) || end < addr || end > USER_TOP)
		return -MERR_INVAL;
	if (advice != 4 && advice != 8)         /* MADV_DONTNEED, MADV_FREE: drop private pages */
		return 0;
	for (u32 i = vma_first_after(t, addr); i < t->nvma && t->vma[i].start < end; i++) {
		struct vma *v = &t->vma[i];
		u32 n;

		if ((v->flags & VMA_SHARED) || v->src)
			continue;
		n = vmm_unmap_range(t->pml4, MOS_MAX(addr, v->start), MOS_MIN(end, v->end));
		t->pages -= MOS_MIN(t->pages, n);
	}
	return 0;
}

long umm_mremap(struct task *t, u64 old, u64 oldlen, u64 newlen, u32 flags, u64 newaddr)
{
	struct vma *v;
	struct vma nv;
	u64 dst;
	int rc;

	oldlen = ALIGN_UP(oldlen, PAGE_SIZE);
	newlen = ALIGN_UP(newlen, PAGE_SIZE);
	if ((old & (PAGE_SIZE - 1)) || !newlen || !oldlen)
		return -MERR_INVAL;
	v = umm_find(t, old);
	if (!v || v->end < old + oldlen)
		return -MERR_FAULT;
	if (!(flags & MREMAP_FIXED)) {
		if (newlen == oldlen)
			return (long)old;
		if (newlen < oldlen)
			return carve(t, old + newlen, old + oldlen) ? -MERR_NOMEM : (long)old;
		if (old + oldlen == v->end && range_free(t, v->end, old + newlen) &&
		    newlen - oldlen <= TASK_VM_LIMIT - t->vm_reserved) {
			v->end = old + newlen;
			t->vm_reserved += newlen - oldlen;
			return (long)old;
		}
	}
	if (!(flags & MREMAP_MAYMOVE))
		return -MERR_NOMEM;
	nv = *v;
	if (nv.start < old)
		vma_advance(&nv, old);
	nv.end = old + oldlen;
	if (flags & MREMAP_FIXED) {
		dst = ALIGN_DOWN(newaddr, PAGE_SIZE);
		if (dst < USER_MIN || dst + newlen > USER_TOP ||
		    (dst < old + oldlen && dst + newlen > old))
			return -MERR_INVAL;
		rc = carve(t, dst, dst + newlen);
		if (rc)
			return rc;
		v = umm_find(t, old);
		if (!v)
			return -MERR_FAULT;
	} else {
		dst = find_gap(t, newlen);
		if (!dst)
			return -MERR_NOMEM;
	}
	nv.start = dst;
	nv.end = dst + newlen;
	if (nv.file)
		file_get(nv.file);
	rc = vma_insert(t, vma_first_after(t, dst), &nv);
	if (rc) {
		if (nv.file)
			file_put(nv.file);
		return rc;
	}
	t->vm_reserved += newlen;
	/* Move the frames: same pages, new addresses. */
	for (u64 off = 0; off < MOS_MIN(oldlen, newlen); off += PAGE_SIZE) {
		bool shared;
		u64 frame = vmm_take_frame(t->pml4, old + off, &shared);

		if (frame && !vmm_map_frame(t->pml4, dst + off, frame, nv.prot, shared))
			pmm_free(frame);
	}
	carve(t, old, old + oldlen);
	return (long)dst;
}
