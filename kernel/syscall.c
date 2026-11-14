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
		if (!vmm_user_ok(t->pml4, page, 1, write) && task_fault_in(t, page, write))
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

/* ---- virtual memory: see umm.c ------------------------------------------------- */

long msys_set_fs_base(struct task *t, u64 base)
{
	(void)t;
	if (base >= USER_TOP)
		return -MERR_PERM;
	thread_current()->fs_base = base;
	wrmsr(0xC0000100, base);                /* IA32_FS_BASE */
	return 0;
}

long msys_set_gs_base(struct task *t, u64 base)
{
	(void)t;
	if (base >= USER_TOP)
		return -MERR_PERM;
	thread_current()->gs_base = base;
	wrmsr(0xC0000101, base);                /* IA32_GS_BASE */
	return 0;
}

long msys_get_fs_base(struct task *t, u64 uaddr)
{
	u64 base = thread_current()->fs_base;

	return uaccess_copy_out(t, uaddr, &base, sizeof(base));
}

/* getrandom: a non-cryptographic PRNG (rdtsc-seeded xorshift), enough for a
 * C library's stack canary and ld.so's pointer guard. */
long msys_getrandom(struct task *t, u64 ubuf, u64 len)
{
	u8 buf[256];
	u64 done = 0;
	static u64 s = 0x9E3779B97F4A7C15ULL;

	if (len > (1u << 20))
		len = 1u << 20;
	while (done < len) {
		u64 n = MOS_MIN(len - done, sizeof(buf)), i;
		u32 lo, hi;

		__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
		s ^= ((u64)hi << 32) | lo | 1;
		for (i = 0; i < n; i++) {
			s ^= s << 13; s ^= s >> 7; s ^= s << 17;
			buf[i] = (u8)(s >> 24);
		}
		if (uaccess_copy_out(t, ubuf + done, buf, (size_t)n))
			return done ? (long)done : -MERR_FAULT;
		done += n;
	}
	return (long)len;
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
