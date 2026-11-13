# Processes, threads, files: kernel core spec (M2/M3 slice)

Short spec written before the code; the code follows it.

## Scheduling and threads

- Every user thread is a scheduler thread with its own 16 KiB kernel stack
  (allocated, not static). On a switch the scheduler loads the next
  thread's address space (CR3), FS/GS base, `TSS.rsp0` and the SYSCALL
  stack, and saves/restores the user FPU/SSE state (`fxsave`/`fxrstor`).
- One CPU, round-robin, 10 ms slice, preemption on the timer tick in ring 3
  and ring 0. System calls run with interrupts off (a big kernel lock on one
  CPU) and may block: `thread_block(chan)` / `thread_wake(chan)`.
- A dead thread's stack, and a dead process's address space, are freed by
  the next thread that runs (never by the dying thread itself).
- `task_exec()` keeps its synchronous contract (load, run, report) but now
  spawns a process and waits for it; the waiting thread feeds keyboard
  input to the foreground process group and turns Ctrl-C into SIGINT.

## Processes (Linux numbers first, FreeBSD where trivial)

- `struct task` is a process: pid, ppid, pgid, address space, VMAs, brk,
  fd table, signal actions, pending signals, live-thread count, zombie
  state. Threads share it (`CLONE_VM|CLONE_THREAD`).
- `fork`/`clone` without `CLONE_VM`: eager copy of every mapped user page
  (demand paging keeps that to touched pages; no COW). `vfork` /
  `CLONE_VM|CLONE_VFORK`: child borrows the parent's address space, parent
  sleeps until the child calls `execve` or exits.
- `execve(path, argv, envp)`: image from the ramdisk or ramfs; new address
  space, argv/envp on the stack, personality chosen by the binary, CLOEXEC
  descriptors closed, caught signals reset, other threads killed.
- `exit`/`exit_group`: descriptors closed, children reparented to the
  kernel (auto-reaped), parent gets SIGCHLD and is woken; the process stays
  a zombie until `wait4`/`waitpid` reaps it (`WNOHANG` supported, status in
  the Linux/BSD `wait` encoding). `getpid`, `getppid`, `gettid`.

## Descriptors, pipes, terminal

- Per-process table of 32 slots pointing at refcounted open files (console,
  ramdisk file, ramfs file/dir, pipe end); `fork` shares them, `dup`,
  `dup2`, `dup3`, `fcntl(F_DUPFD/F_GETFD/F_SETFD)`, `O_CLOEXEC`.
- `pipe`/`pipe2`: 4 KiB ring, blocking reads/writes, EOF when all writers
  close, `SIGPIPE` + `EPIPE` when all readers close.
- `read(0)` from the console blocks until a line of keyboard/serial input
  arrives for the foreground process group; Ctrl-D is end of file.

## Signals

- `kill`, `tgkill`, `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`,
  64 signals, per-process actions and pending set, per-thread mask.
- Delivered on the way back to ring 3 (after a system call or interrupt).
  Handlers get a Linux-layout `rt_sigframe` (siginfo, ucontext with the
  general registers and FPU state) and return through `SA_RESTORER`.
- Default actions: terminate, or ignore for SIGCHLD/SIGURG/SIGWINCH/
  SIGCONT/SIGSTOP-family (no job control yet). SIGKILL cannot be caught.
  Blocking calls return `EINTR` when a signal arrives.

## Threads

- `clone(CLONE_VM|CLONE_THREAD|CLONE_SETTLS|CLONE_PARENT_SETTID|
  CLONE_CHILD_SETTID|CLONE_CHILD_CLEARTID)`, `set_tid_address`,
  `arch_prctl(SET_FS/GET_FS)`, `futex(WAIT/WAKE)` keyed by address space +
  address, CLEARTID wakes joiners on thread exit.
- NT: `NtCreateThreadEx`, `NtTerminateThread`, `NtWaitForSingleObject` and
  `NtClose` on thread handles, threads started through an exit thunk.

## Files

- ramfs (writable, in memory) mounted at `/tmp` and `/home/user`: `open`
  with `O_CREAT|O_TRUNC|O_APPEND|O_EXCL`, `read`, `write`, `lseek`,
  `unlink`, `mkdir`, `rmdir`, `rename`, `getdents64`, `stat`. Everything
  else is the read-only ramdisk. The ramfs core is portable and unit-tested
  on the host.

## Memory

- Per-process limit of 1 GiB of virtual memory (was 16 MiB of mapped
  pages). `brk` and anonymous `mmap` only reserve a range; pages are
  allocated zeroed on first touch (page fault or kernel copy). `munmap`
  frees pages.
- The frame allocator manages RAM above 4 GiB; the direct map is extended
  at boot with 2 MiB pages up to the top of RAM (512 GiB cap).
