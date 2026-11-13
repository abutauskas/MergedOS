/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Native kernel services ("msys") - the single implementation every ABI
 * personality is translated onto - plus checked user-memory access and the
 * process's virtual memory: reserved ranges backed by zeroed pages on first
 * touch. Files live in vfs.c, processes and signals in proc.c.
 */
#include <mergedos/kernel.h>
#include <mergedos/cpu.h>
#include <mergedos/io.h>
#include <mergedos/sched.h>
#include <mergedos/mm.h>
#include <mergedos/task.h>

/*
 * C side of SYSCALL: called from entry.S with the saved register frame.
 * Returns 0 to leave through SYSRET with that frame, or the address of a
 * full trap frame to IRET through (signal delivery, sigreturn, execve).
 */
u64 syscall_handler(struct syscall_frame *f)
{
	struct thread *th = thread_current();
	struct task *t = th->proc;
	struct trap_frame *tf;

	if (!t)
		panic("SYSCALL from ring 3 with no current task");
	th->uframe = f;
	th->iret_pending = false;
	abi_dispatch(&t->abi, f);
	if (t->exited)
		task_exit_group(t, t->exit_code, t->termsig);  /* exit_group(), or killed */
	if (!th->iret_pending && !(t->sigpending & ~th->sigmask))
		return 0;
	tf = KSTACK_IRET(th);
	if (!th->iret_pending) {
		memset(tf, 0, sizeof(*tf));
		memcpy(tf, f, 15 * sizeof(u64));        /* same general-register layout */
		tf->rip = f->rip;
		tf->rflags = f->rflags;
		tf->rsp = f->rsp;
		tf->cs = USER_CS;
		tf->ss = USER_DS;
	}
	task_signal_deliver(tf);
	/* The top of the kernel stack is free for it: f lies there and is done with. */
	f = (struct syscall_frame *)(KSTACK_TOP(th) - sizeof(*tf));
	memcpy(f, tf, sizeof(*tf));
	return (u64)f;
}

/*
 * User pointers are validated against the task's own page tables before
 * they are touched, so a bad pointer becomes an error return in the
 * caller's dialect (EFAULT / STATUS_ACCESS_VIOLATION), never a kernel fault.
 * Reserved pages that were never touched are backed first. The task's
 * address space is the active one while it is in a system call.
 */
static bool user_range(struct task *t, u64 uaddr, size_t len, bool write)
{
	if (vmm_user_ok(t->pml4, uaddr, len, write))
		return true;
	if (uaddr + len < uaddr || uaddr + len > USER_TOP)
		return false;
	for (u64 page = ALIGN_DOWN(uaddr, PAGE_SIZE); page < uaddr + len; page += PAGE_SIZE)
		if (!vmm_user_to_phys(t->pml4, page) && task_fault_in(t, page, write))
			return false;
	return vmm_user_ok(t->pml4, uaddr, len, write);
}

int uaccess_copy_in(struct task *t, void *dst, u64 uaddr, size_t len)
{
	if (!user_range(t, uaddr, len, false))
		return -MERR_FAULT;
	memcpy(dst, (const void *)uaddr, len);
	return 0;
}

int uaccess_copy_out(struct task *t, u64 uaddr, const void *src, size_t len)
{
	if (!user_range(t, uaddr, len, true))
		return -MERR_FAULT;
	memcpy((void *)uaddr, src, len);
	return 0;
}

long msys_getpid(struct task *t)
{
	return t->pid;
}

/* exit_group(): the whole process; syscall_handler() carries it out. */
void msys_exit(struct task *t, int status)
{
	if (!t->exited) {
		t->exited = true;
		t->exit_code = status;
	}
}

/* ---- virtual memory ------------------------------------------------------------- */

static struct vma *vma_find(struct task *t, u64 va)
{
	for (u32 i = 0; i < t->nvma; i++)
		if (va >= t->vma[i].start && va < t->vma[i].end)
			return &t->vma[i];
	return NULL;
}

/* Back one page of a reserved range (or of the heap) with a zeroed frame. */
int task_fault_in(struct task *t, u64 va, bool write)
{
	struct vma *v = vma_find(t, va);
	u32 prot;

	if (v)
		prot = v->prot;
	else if (va >= t->brk_start && va < ALIGN_UP(t->brk, PAGE_SIZE))
		prot = VMM_R | VMM_W;
	else
		return -MERR_FAULT;
	if (write && !(prot & VMM_W))
		return -MERR_FAULT;
	if (vmm_user_to_phys(t->pml4, va))
		return -MERR_FAULT;             /* present: a protection fault */
	if (!vmm_map_user(t->pml4, va, prot))
		return -MERR_NOMEM;
	t->pages++;
	return 0;
}

static void unmap_range(struct task *t, u64 from, u64 to)
{
	for (u64 page = ALIGN_UP(from, PAGE_SIZE); page < to; page += PAGE_SIZE)
		if (vmm_unmap_user(t->pml4, page))
			t->pages--;
}

long msys_brk(struct task *t, u64 addr)
{
	/* Linux semantics: on any failure the old break is returned unchanged. */
	if (addr > t->brk && addr - t->brk_start <= TASK_VM_LIMIT) {
		t->brk = addr;                  /* backed on first touch */
	} else if (addr >= t->brk_start && addr <= t->brk) {
		unmap_range(t, addr, ALIGN_UP(t->brk, PAGE_SIZE));
		t->brk = addr;
	}
	return (long)t->brk;
}

long msys_mmap_anon(struct task *t, u64 len)
{
	u64 addr = t->mmap_next;

	len = ALIGN_UP(len, PAGE_SIZE);
	if (!len || len > TASK_VM_LIMIT - t->vm_reserved || t->nvma >= TASK_VMA_MAX)
		return -MERR_NOMEM;
	t->vma[t->nvma++] = (struct vma){ addr, addr + len, VMM_R | VMM_W };
	t->vm_reserved += len;
	t->mmap_next = addr + len + PAGE_SIZE;  /* leave an unmapped guard page between mappings */
	return (long)addr;
}

long msys_munmap(struct task *t, u64 addr, u64 len)
{
	u64 end = addr + ALIGN_UP(len, PAGE_SIZE);

	if ((addr & (PAGE_SIZE - 1)) || !len || end < addr || end > USER_TOP)
		return -MERR_INVAL;
	for (u32 i = 0; i < t->nvma; i++) {
		struct vma *v = &t->vma[i];
		u64 lo = MOS_MAX(v->start, addr), hi = MOS_MIN(v->end, end);

		if (lo >= hi)
			continue;
		unmap_range(t, lo, hi);
		t->vm_reserved -= hi - lo;
		if (lo == v->start && hi == v->end) {
			t->vma[i--] = t->vma[--t->nvma];
		} else if (lo == v->start) {
			v->start = hi;
		} else if (hi == v->end) {
			v->end = lo;
		} else if (t->nvma < TASK_VMA_MAX) {
			t->vma[t->nvma++] = (struct vma){ hi, v->end, v->prot };
			v->end = lo;
		} else {
			t->vm_reserved += hi - lo;      /* no slot to split: keep the hole reserved */
		}
	}
	return 0;
}

long msys_set_fs_base(struct task *t, u64 base)
{
	(void)t;
	if (base >= USER_TOP)
		return -MERR_PERM;
	thread_current()->fs_base = base;
	wrmsr(0xC0000100, base);                /* IA32_FS_BASE */
	return 0;
}

long msys_get_fs_base(struct task *t, u64 uaddr)
{
	u64 base = thread_current()->fs_base;

	return uaccess_copy_out(t, uaddr, &base, sizeof(base));
}

long msys_protect(struct task *t, u64 addr, u64 len, u32 prot)
{
	u64 end = addr + len, page;

	if (!len || end < addr || end > USER_TOP)
		return -MERR_INVAL;
	for (page = ALIGN_DOWN(addr, PAGE_SIZE); page < end; page += PAGE_SIZE)
		if (!vmm_user_to_phys(t->pml4, page) && task_fault_in(t, page, false))
			return -MERR_FAULT;     /* all or nothing */
	for (page = ALIGN_DOWN(addr, PAGE_SIZE); page < end; page += PAGE_SIZE) {
		vmm_map_user(t->pml4, page, VMM_R | (prot & MSYS_PROT_W ? VMM_W : 0) |
					    (prot & MSYS_PROT_X ? VMM_X : 0));
		__asm__ volatile("invlpg (%0)" : : "r"(page) : "memory");
	}
	return 0;
}

u64 msys_clock_ns(void)
{
	return time_ticks() * (1000000000ULL / SCHED_HZ);
}

long msys_nanosleep(struct task *t, u64 ns)
{
	u64 ticks = (ns + 1000000000ULL / SCHED_HZ - 1) / (1000000000ULL / SCHED_HZ);

	(void)t;
	if (!ticks) {
		thread_yield();
		return 0;
	}
	if (task_signal_pending())
		return -MERR_INTR;
	thread_block(NULL, 0, ticks);
	return task_signal_pending() ? -MERR_INTR : 0;
}
