/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Processes and threads: clone/fork/vfork, execve, exit and wait, signals,
 * futexes and NT-style thread creation. See docs/KERNEL_PROCESSES.md.
 *
 * Everything here runs inside a system call (interrupts off) or from a ring-3
 * trap, so on one CPU no locking is needed beyond not blocking halfway
 * through an update.
 */
#include <mergedos/kernel.h>
#include <mergedos/cpu.h>
#include <mergedos/io.h>
#include <mergedos/mm.h>
#include <mergedos/sched.h>
#include <mergedos/task.h>
#include <mergedos/vfs.h>

#define SIGKILL 9
#define SIGSEGV 11
#define SIGCHLD 17
#define SIGSTOP 19
#define SIG_DFL 0
#define SIG_IGN 1
#define SA_NODEFER   0x40000000ULL
#define SA_RESETHAND 0x80000000ULL
#define SIGBIT(s)    (1ULL << ((s) - 1))
#define UNBLOCKABLE  (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))
/* SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG, SIGWINCH */
#define DEFAULT_IGNORE (SIGBIT(17) | SIGBIT(18) | SIGBIT(19) | SIGBIT(20) | SIGBIT(21) | \
			SIGBIT(22) | SIGBIT(23) | SIGBIT(28))
#define AUX_CHILD  0                    /* thread_wake(task, aux): a child changed state */
#define AUX_THREAD 1                    /*                         a thread ended        */

static bool sig_ignored(const struct task *t, int sig)
{
	u64 h = t->sig[sig].handler;

	if (sig == SIGKILL)
		return false;
	return h == SIG_IGN || (h == SIG_DFL && (DEFAULT_IGNORE & SIGBIT(sig)));
}

void task_signal(struct task *t, int sig)
{
	if (sig < 1 || sig > TASK_NSIG || t->zombie || sig_ignored(t, sig))
		return;
	t->sigpending |= SIGBIT(sig);
	thread_wake_proc(t);                    /* blocking calls notice and return EINTR */
}

bool task_signal_pending(void)
{
	struct thread *th = thread_current();

	return th->proc && (th->proc->sigpending & ~th->sigmask);
}

/* ---- exit and wait ------------------------------------------------------------- */

static void process_finish(struct task *t)
{
	struct task *parent = t->ppid ? task_find(t->ppid) : NULL;

	fd_close_all(t);
	if (t->mm_borrowed) {                   /* a vfork child gives the space back */
		t->vfork_done = true;
		thread_wake(&t->vfork_done, 0, SCHED_MAX_THREADS);
	}
	for (struct task *c = task_first(); c; c = c->next) {
		if (c->ppid == t->pid && c != t) {
			c->ppid = 0;            /* orphans: the kernel reaps them */
			c->autoreap = true;
		}
	}
	t->zombie = true;
	task_mark_cleanup();
	if (parent) {
		task_signal(parent, SIGCHLD);
		thread_wake(parent, AUX_CHILD, SCHED_MAX_THREADS);
	} else if (t->kernel_wait) {
		thread_wake(t, AUX_CHILD, SCHED_MAX_THREADS);
	} else {
		t->autoreap = true;
	}
}

void task_exit_thread(int status)
{
	struct thread *th = thread_current();
	struct task *t = th->proc;

	if (th->clear_tid) {                    /* CLONE_CHILD_CLEARTID: wake pthread_join */
		u32 zero = 0;

		if (!uaccess_copy_out(t, th->clear_tid, &zero, sizeof(zero)))
			thread_wake((const void *)th->clear_tid, t->pml4, 1);
	}
	thread_wake(t, AUX_THREAD, SCHED_MAX_THREADS);
	if (--t->nthreads <= 0) {
		if (!t->exited) {
			t->exited = true;
			t->exit_code = status;
		}
		process_finish(t);
	}
	thread_exit();
}

void task_exit_group(struct task *t, int code, int sig)
{
	struct thread *self = thread_current();

	if (!t->exited || t->termsig < sig) {
		t->exited = true;
		t->exit_code = code;
		t->termsig = sig;
	}
	for (int i = 0; i < SCHED_MAX_THREADS; i++) {
		struct thread *th = thread_slot(i);

		if (th != self && th->proc == t && thread_alive(i)) {
			thread_kill(th);
			t->nthreads--;
		}
	}
	task_exit_thread(code);
}

long msys_wait(struct task *t, int pid, u64 ustatus, int options)
{
	for (;;) {
		bool any = false;

		for (struct task *c = task_first(); c; c = c->next) {
			int status, cpid;

			if (c->ppid != t->pid || c == t)
				continue;
			if (pid > 0 ? c->pid != pid : pid < -1 ? c->pgid != -pid
						      : pid == 0 && c->pgid != t->pgid)
				continue;
			any = true;
			if (!c->zombie)
				continue;
			status = c->termsig ? (c->termsig & 0x7F) : (c->exit_code & 0xFF) << 8;
			if (ustatus && uaccess_copy_out(t, ustatus, &status, sizeof(status)))
				return -MERR_FAULT;
			cpid = c->pid;
			task_free(c);
			return cpid;
		}
		if (!any)
			return -MERR_CHILD;
		if (options & MSYS_WNOHANG)
			return 0;
		if (task_signal_pending())
			return -MERR_INTR;
		thread_block(t, AUX_CHILD, 0);
	}
}

long msys_thread_exit(struct task *t, int status)
{
	(void)t;
	task_exit_thread(status);
}

long msys_getppid(struct task *t)
{
	return t->ppid;
}

long msys_gettid(struct task *t)
{
	(void)t;
	return thread_current()->tid;
}

long msys_setpgid(struct task *t, int pid, int pgid)
{
	struct task *p = pid ? task_find(pid) : t;

	if (!p || (p != t && p->ppid != t->pid))
		return -MERR_SRCH;
	if (pgid < 0)
		return -MERR_INVAL;
	p->pgid = pgid ? pgid : p->pid;
	return 0;
}

long msys_getpgid(struct task *t, int pid)
{
	struct task *p = pid ? task_find(pid) : t;

	return p ? p->pgid : -MERR_SRCH;
}

long msys_set_tid_address(struct task *t, u64 uaddr)
{
	struct thread *th = thread_current();

	(void)t;
	th->clear_tid = uaddr;
	return th->tid;
}

/* ---- clone, fork, vfork ----------------------------------------------------------- */

/* The registers of the system call in progress, as a frame a new thread starts from. */
static void frame_of_caller(struct trap_frame *tf)
{
	const struct syscall_frame *f = thread_current()->uframe;

	memset(tf, 0, sizeof(*tf));
	memcpy(tf, f, 15 * sizeof(u64));
	tf->rip = f->rip;
	tf->rflags = f->rflags;
	tf->rsp = f->rsp;
	tf->rax = 0;                            /* the child's return value */
}

static long clone_thread(struct task *t, u64 flags, u64 newsp, u64 ptid, u64 ctid, u64 tls)
{
	struct thread *self = thread_current(), *th;
	struct trap_frame tf;
	u8 fx[512] __attribute__((aligned(16)));
	int tid;

	if (!(flags & MSYS_CLONE_VM))
		return -MERR_INVAL;
	frame_of_caller(&tf);
	if (newsp)
		tf.rsp = newsp;
	fpu_save(fx);
	th = thread_create_user(t, &tf, fx);
	if (!th)
		return -MERR_AGAIN;
	tid = task_new_pid();
	th->tid = tid;
	th->fs_base = (flags & MSYS_CLONE_SETTLS) ? tls : self->fs_base;
	th->gs_base = self->gs_base;
	th->sigmask = self->sigmask;
	th->clear_tid = (flags & MSYS_CLONE_CHILD_CLEARTID) ? ctid : 0;
	t->nthreads++;
	if (flags & MSYS_CLONE_PARENT_SETTID)
		uaccess_copy_out(t, ptid, &tid, sizeof(tid));
	if (flags & MSYS_CLONE_CHILD_SETTID)
		uaccess_copy_out(t, ctid, &tid, sizeof(tid));
	return tid;
}

long msys_clone(struct task *t, u64 flags, u64 newsp, u64 ptid, u64 ctid, u64 tls)
{
	struct thread *self = thread_current(), *th;
	bool share = (flags & (MSYS_CLONE_VM | MSYS_CLONE_VFORK)) ==
		     (MSYS_CLONE_VM | MSYS_CLONE_VFORK);
	struct trap_frame tf;
	u8 fx[512] __attribute__((aligned(16)));
	struct task *c;
	int pid;

	if (flags & MSYS_CLONE_THREAD)
		return clone_thread(t, flags, newsp, ptid, ctid, tls);
	if ((flags & MSYS_CLONE_VM) && !share)
		return -MERR_NOTSUP;            /* shared memory without a thread group */
	c = task_new(t->name);
	if (!c)
		return -MERR_AGAIN;
	pid = c->pid;
	c->ppid = t->pid;
	c->pgid = t->pgid;
	c->abi = t->abi;
	c->abi.task = c;
	memset(&c->abi.stats, 0, sizeof(c->abi.stats));
	c->format = t->format;
	c->pages = t->pages;
	c->vm_reserved = t->vm_reserved;
	c->brk_start = t->brk_start;
	c->brk = t->brk;
	c->gs_base = t->gs_base;
	c->umask = t->umask;
	memcpy(c->cwd, t->cwd, sizeof(c->cwd));
	if (umm_fork(c, t)) {
		task_free(c);
		return -MERR_NOMEM;
	}
	memcpy(c->sig, t->sig, sizeof(c->sig));
	if (share) {
		c->pml4 = t->pml4;
		c->mm_borrowed = true;
	} else {
		c->pml4 = vmm_as_clone(t->pml4);
		if (!c->pml4) {
			task_free(c);
			return -MERR_NOMEM;
		}
	}
	fd_fork(c, t);

	frame_of_caller(&tf);
	if (newsp)
		tf.rsp = newsp;
	if (t->abi.personality == ABI_FREEBSD)
		tf.rdx = 1;                     /* FreeBSD: rdx tells the child it is the child */
	fpu_save(fx);
	th = thread_create_user(c, &tf, fx);
	if (!th) {
		task_free(c);
		return -MERR_AGAIN;
	}
	th->tid = pid;
	th->fs_base = (flags & MSYS_CLONE_SETTLS) ? tls : self->fs_base;
	th->gs_base = self->gs_base;
	th->sigmask = self->sigmask;
	th->clear_tid = (flags & MSYS_CLONE_CHILD_CLEARTID) ? ctid : 0;
	c->nthreads = 1;
	if (flags & MSYS_CLONE_PARENT_SETTID)
		uaccess_copy_out(t, ptid, &pid, sizeof(pid));
	if (flags & MSYS_CLONE_CHILD_SETTID)
		task_copy_to_as(c->pml4, ctid, &pid, sizeof(pid));
	if (t->abi.personality == ABI_FREEBSD)
		self->uframe->rdx = 0;
	if (flags & MSYS_CLONE_VFORK) {
		/* The child runs in our memory: wait until it execs or exits. */
		while (!c->vfork_done)
			thread_block(&c->vfork_done, 0, 0);
	}
	return pid;
}

/* ---- execve ------------------------------------------------------------------------- */

static long copy_str(struct task *t, u64 uaddr, char *dst, u32 cap)
{
	for (u32 i = 0; i < cap; i++) {
		if (uaccess_copy_in(t, &dst[i], uaddr + i, 1))
			return -MERR_FAULT;
		if (!dst[i])
			return i + 1;
	}
	return -MERR_RANGE;
}

/* Append a NULL-terminated user array of strings to the argument block. */
static long copy_vec(struct task *t, u64 uvec, struct exec_args *a, u32 *count)
{
	for (u32 i = 0; uvec; i++) {
		u64 p;
		long n;

		if (uaccess_copy_in(t, &p, uvec + i * 8, 8))
			return -MERR_FAULT;
		if (!p)
			return 0;
		if (a->argc + a->envc >= EXEC_ARGS_MAX)
			return -MERR_RANGE;
		n = copy_str(t, p, a->buf + a->len, EXEC_ARGS_BUF - a->len);
		if (n < 0)
			return n;
		a->len += (u32)n;
		(*count)++;
	}
	return 0;
}

long msys_execve(struct task *t, u64 upath, u64 uargv, u64 uenvp)
{
	struct thread *self = thread_current();
	struct exec_args *a = kzalloc(sizeof(*a));
	struct task *save = kzalloc(sizeof(*save));
	char path[128];
	const char *base;
	struct user_start start;
	struct trap_frame *tf;
	const u8 *img;
	size_t len;
	long rc;

	rc = a && save ? copy_str(t, upath, path, sizeof(path)) : -MERR_NOMEM;
	if (rc > 0)
		rc = copy_vec(t, uargv, a, &a->argc);
	if (rc >= 0 && !a->argc) {              /* argv[0] is expected to exist */
		a->len = (u32)mos_strlcpy(a->buf, path, 64) + 1;
		a->argc = 1;
	}
	if (rc >= 0)
		rc = copy_vec(t, uenvp, a, &a->envc);
	if (rc >= 0)
		rc = vfs_image(path, &img, &len);
	if (rc < 0)
		goto out;
	mos_strlcpy(a->path, path, sizeof(a->path));

	memcpy(save, t, sizeof(*t));
	rc = task_load_image(t, img, len, a, &start);
	if (rc) {
		memcpy(t, save, sizeof(*t));    /* the old program carries on */
		goto out;
	}
	/* Point of no return: switch to the new space and drop the old one. */
	vmm_switch(t->pml4);
	if (save->mm_borrowed) {
		t->vfork_done = true;
		thread_wake(&t->vfork_done, 0, SCHED_MAX_THREADS);
	} else {
		vmm_as_destroy(save->pml4);
	}
	for (int i = 0; i < SCHED_MAX_THREADS; i++) {
		struct thread *th = thread_slot(i);

		if (th != self && th->proc == t && thread_alive(i)) {
			thread_kill(th);
			t->nthreads--;
		}
	}
	fd_close_on_exec(t);
	for (int s = 1; s <= TASK_NSIG; s++)
		if (t->sig[s].handler != SIG_IGN)
			memset(&t->sig[s], 0, sizeof(t->sig[s]));
	base = path;
	for (const char *p = path; *p; p++)
		if (*p == '/' && p[1])
			base = p + 1;
	mos_strlcpy(t->name, base, sizeof(t->name));
	self->name = t->name;
	self->clear_tid = 0;
	self->fs_base = 0;
	self->gs_base = t->gs_base;
	wrmsr(0xC0000100, 0);
	wrmsr(0xC0000101, t->gs_base);
	fpu_init_state(KSTACK_FX(self));
	fpu_restore(KSTACK_FX(self));
	tf = KSTACK_IRET(self);
	trap_frame_from_start(tf, &start);
	self->iret_pending = true;
	rc = 0;
out:
	kfree(a);
	kfree(save);
	return rc;
}

/* ---- signals -------------------------------------------------------------------------- */

long msys_kill(struct task *t, int pid, int sig)
{
	int hits = 0;

	if (sig < 0 || sig > TASK_NSIG)
		return -MERR_INVAL;
	if (pid > 0) {
		struct task *p = task_find(pid);

		for (int i = 0; !p && i < SCHED_MAX_THREADS; i++) {     /* a thread id */
			struct thread *th = thread_slot(i);

			if (thread_alive(i) && th->proc && th->tid == pid)
				p = th->proc;
		}
		if (!p || p->zombie)
			return -MERR_SRCH;
		task_signal(p, sig);
		return 0;
	}
	for (struct task *p = task_first(); p; p = p->next) {
		if (p->zombie || (pid == 0 && p->pgid != t->pgid) || (pid < -1 && p->pgid != -pid) ||
		    (pid == -1 && p == t))
			continue;
		task_signal(p, sig);
		hits++;
	}
	return hits ? 0 : -MERR_SRCH;
}

long msys_sigaction(struct task *t, int sig, u64 uact, u64 uold)
{
	struct sigact act;

	if (sig < 1 || sig > TASK_NSIG || ((sig == SIGKILL || sig == SIGSTOP) && uact))
		return -MERR_INVAL;
	if (uold && uaccess_copy_out(t, uold, &t->sig[sig], sizeof(act)))
		return -MERR_FAULT;
	if (!uact)
		return 0;
	if (uaccess_copy_in(t, &act, uact, sizeof(act)))
		return -MERR_FAULT;
	t->sig[sig] = act;
	if (sig_ignored(t, sig))
		t->sigpending &= ~SIGBIT(sig);
	return 0;
}

long msys_sigprocmask(struct task *t, int how, u64 uset, u64 uold)
{
	struct thread *th = thread_current();
	u64 set;

	if (uold && uaccess_copy_out(t, uold, &th->sigmask, sizeof(th->sigmask)))
		return -MERR_FAULT;
	if (!uset)
		return 0;
	if (uaccess_copy_in(t, &set, uset, sizeof(set)))
		return -MERR_FAULT;
	switch (how) {
	case 0: th->sigmask |= set; break;      /* SIG_BLOCK   */
	case 1: th->sigmask &= ~set; break;     /* SIG_UNBLOCK */
	case 2: th->sigmask = set; break;       /* SIG_SETMASK */
	default: return -MERR_INVAL;
	}
	th->sigmask &= ~UNBLOCKABLE;
	return 0;
}

/*
 * Linux x86-64 rt_sigframe, as glibc and musl expect it:
 *   rsp ->  return address (sa_restorer)
 *           struct ucontext   (304 bytes: flags, link, stack, mcontext, sigmask)
 *           struct siginfo    (128 bytes)
 *   higher: FXSAVE image, 64-byte aligned
 */
#define UC_MCONTEXT 40
#define UC_SIGMASK  296
#define UC_SIZE     304
#define SI_SIZE     128
enum { R8, R9, R10, R11, R12, R13, R14, R15, RDI, RSI, RBP, RBX, RDX, RAX, RCX, RSP, RIP,
       EFL, CSGSFS, ERR, TRAPNO, OLDMASK, CR2, NGREG };

static bool setup_frame(struct task *t, struct thread *th, struct trap_frame *tf, int sig,
			const struct sigact *act)
{
	u8 fx[512] __attribute__((aligned(16)));
	u8 uc[UC_SIZE] = { 0 };
	u8 si[SI_SIZE] = { 0 };
	u64 g[NGREG] = { 0 };
	u64 fxp = ALIGN_DOWN(tf->rsp - 128 - sizeof(fx), 64);
	u64 sp = ALIGN_DOWN(fxp - UC_SIZE - SI_SIZE - 8, 16) - 8;   /* as after a CALL */
	u64 ucp = sp + 8, sip = sp + 8 + UC_SIZE;
	s32 si_head[4] = { sig, 0, 0, 0 };      /* signo, errno, code SI_USER, pad */

	g[R8] = tf->r8;   g[R9] = tf->r9;   g[R10] = tf->r10; g[R11] = tf->r11;
	g[R12] = tf->r12; g[R13] = tf->r13; g[R14] = tf->r14; g[R15] = tf->r15;
	g[RDI] = tf->rdi; g[RSI] = tf->rsi; g[RBP] = tf->rbp; g[RBX] = tf->rbx;
	g[RDX] = tf->rdx; g[RAX] = tf->rax; g[RCX] = tf->rcx; g[RSP] = tf->rsp;
	g[RIP] = tf->rip; g[EFL] = tf->rflags; g[CSGSFS] = USER_CS; g[OLDMASK] = th->sigmask;
	memcpy(uc + UC_MCONTEXT, g, sizeof(g));
	memcpy(uc + UC_MCONTEXT + NGREG * 8, &fxp, 8);         /* fpregs pointer */
	memcpy(uc + UC_SIGMASK, &th->sigmask, 8);
	memcpy(si, si_head, sizeof(si_head));
	memcpy(si + 16, &t->pid, 4);                            /* si_pid */
	fpu_save(fx);
	if (uaccess_copy_out(t, fxp, fx, sizeof(fx)) || uaccess_copy_out(t, ucp, uc, sizeof(uc)) ||
	    uaccess_copy_out(t, sip, si, sizeof(si)) ||
	    uaccess_copy_out(t, sp, &act->restorer, 8))
		return false;
	tf->rip = act->handler;
	tf->rsp = sp;
	tf->rdi = (u64)sig;
	tf->rsi = sip;
	tf->rdx = ucp;
	tf->rax = 0;
	tf->rflags &= ~0x400ULL;                /* DF clear on handler entry */
	th->sigmask |= act->mask | ((act->flags & SA_NODEFER) ? 0 : SIGBIT(sig));
	th->sigmask &= ~UNBLOCKABLE;
	return true;
}

void task_signal_deliver(struct trap_frame *tf)
{
	struct thread *th = thread_current();
	struct task *t = th->proc;

	while (t) {
		u64 pend = t->sigpending & ~th->sigmask;
		struct sigact *act;
		int sig;

		if (!pend)
			return;
		sig = __builtin_ctzll(pend) + 1;
		t->sigpending &= ~SIGBIT(sig);
		act = &t->sig[sig];
		if (sig == SIGKILL || act->handler == SIG_DFL) {
			if (DEFAULT_IGNORE & SIGBIT(sig))
				continue;
			task_exit_group(t, 128 + sig, sig);
		}
		if (act->handler == SIG_IGN)
			continue;
		if (!setup_frame(t, th, tf, sig, act))
			task_exit_group(t, 128 + SIGSEGV, SIGSEGV);
		if (act->flags & SA_RESETHAND)
			act->handler = SIG_DFL;
		return;                         /* one handler at a time */
	}
}

long msys_sigreturn(struct task *t)
{
	struct thread *th = thread_current();
	struct trap_frame *tf = KSTACK_IRET(th);
	u64 ucp = th->uframe->rsp, g[NGREG], fxp, mask;
	u8 fx[512] __attribute__((aligned(16)));

	if (uaccess_copy_in(t, g, ucp + UC_MCONTEXT, sizeof(g)) ||
	    uaccess_copy_in(t, &fxp, ucp + UC_MCONTEXT + NGREG * 8, 8) ||
	    uaccess_copy_in(t, &mask, ucp + UC_SIGMASK, 8) || g[RIP] >= USER_TOP ||
	    g[RSP] >= USER_TOP)
		task_exit_group(t, 128 + SIGSEGV, SIGSEGV);
	if (fxp && !uaccess_copy_in(t, fx, fxp, sizeof(fx))) {
		u32 mxcsr;

		memcpy(&mxcsr, fx + 24, 4);
		mxcsr &= 0xFFFF;                /* reserved MXCSR bits would fault FXRSTOR */
		memcpy(fx + 24, &mxcsr, 4);
		fpu_restore(fx);
	}
	memset(tf, 0, sizeof(*tf));
	tf->r8 = g[R8];   tf->r9 = g[R9];   tf->r10 = g[R10]; tf->r11 = g[R11];
	tf->r12 = g[R12]; tf->r13 = g[R13]; tf->r14 = g[R14]; tf->r15 = g[R15];
	tf->rdi = g[RDI]; tf->rsi = g[RSI]; tf->rbp = g[RBP]; tf->rbx = g[RBX];
	tf->rdx = g[RDX]; tf->rax = g[RAX]; tf->rcx = g[RCX]; tf->rsp = g[RSP];
	tf->rip = g[RIP];
	tf->rflags = (g[EFL] & 0xCD5) | 0x202;
	tf->cs = USER_CS;
	tf->ss = USER_DS;
	th->sigmask = mask & ~UNBLOCKABLE;
	th->iret_pending = true;
	return 0;
}

/* ---- futexes and NT threads ------------------------------------------------------------- */

static u64 ns_to_ticks(u64 ns)
{
	u64 per = 1000000000ULL / SCHED_HZ;

	return ns / per + 1;
}

long msys_futex(struct task *t, u64 uaddr, int op, u32 val, u64 utimeout)
{
	int cmd = op & 0x7F;                    /* without FUTEX_PRIVATE / CLOCK_REALTIME */
	u64 ticks = 0;

	if (uaddr & 3)
		return -MERR_INVAL;
	if (cmd == 1 || cmd == 10)              /* FUTEX_WAKE, FUTEX_WAKE_BITSET */
		return thread_wake((const void *)uaddr, t->pml4, (int)MOS_MIN(val, 0x7FFFFFFFu));
	if (cmd == 0 || cmd == 9) {             /* FUTEX_WAIT, FUTEX_WAIT_BITSET */
		u32 cur;

		if (uaccess_copy_in(t, &cur, uaddr, sizeof(cur)))
			return -MERR_FAULT;
		if (cur != val)
			return -MERR_AGAIN;
		if (utimeout) {
			u64 ts[2], ns, now = msys_clock_ns();

			if (uaccess_copy_in(t, ts, utimeout, sizeof(ts)))
				return -MERR_FAULT;
			ns = ts[0] * 1000000000ULL + ts[1];
			if (cmd == 9)           /* absolute deadline */
				ns = ns > now ? ns - now : 0;
			ticks = ns_to_ticks(ns);
		}
		if (task_signal_pending())
			return -MERR_INTR;
		if (thread_block((const void *)uaddr, t->pml4, ticks))
			return task_signal_pending() ? -MERR_INTR : 0;
		return -MERR_TIMEDOUT;
	}
	return -MERR_NOSYS;
}

long msys_thread_spawn(struct task *t, u64 entry, u64 arg, u64 stack_size, u64 exit_thunk)
{
	struct thread *self = thread_current(), *th;
	struct trap_frame tf = { 0 };
	u64 size = stack_size ? ALIGN_UP(stack_size, PAGE_SIZE) : (1ULL << 20);
	long base = msys_mmap_anon(t, size);
	u64 sp;

	if (base < 0)
		return base;
	sp = (u64)base + size - 0x30 - 8;       /* home space; rsp % 16 == 8 as after a CALL */
	if (uaccess_copy_out(t, sp, &exit_thunk, 8))
		return -MERR_FAULT;
	tf.rip = entry;
	tf.rsp = sp;
	tf.rcx = arg;
	tf.rflags = 0x202;
	th = thread_create_user(t, &tf, NULL);
	if (!th)
		return -MERR_AGAIN;
	th->tid = task_new_pid();
	th->gs_base = self->gs_base;            /* one TEB for all threads, for now */
	th->sigmask = self->sigmask;
	t->nthreads++;
	return th->tid;
}

long msys_thread_join(struct task *t, int tid, u64 timeout_ns)
{
	u64 deadline = timeout_ns == ~0ULL ? 0 : time_ticks() + ns_to_ticks(timeout_ns);

	for (;;) {
		bool alive = false;

		for (int i = 0; i < SCHED_MAX_THREADS; i++) {
			struct thread *th = thread_slot(i);

			if (thread_alive(i) && th->proc == t && th->tid == tid)
				alive = true;
		}
		if (!alive)
			return 0;
		if (deadline && time_ticks() >= deadline)
			return -MERR_TIMEDOUT;
		thread_block(t, AUX_THREAD, deadline ? deadline - time_ticks() : 0);
	}
}
