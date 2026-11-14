# Dynamic linking on the Linux personality (M4, workstream 4b)

Short spec written before the code; the code follows it. The kernel side
only: programs run with their own, unmodified dynamic linkers (glibc
`ld-linux-x86-64.so.2`, musl `ld-musl-x86_64.so.1`).

## exec

- `binfmt_load_plan()` accepts ELF `ET_EXEC` and `ET_DYN` (PIE, static PIE,
  shared objects used as interpreters). For `ET_DYN` the segment addresses
  are relative; the kernel adds a load bias. `PT_INTERP` is reported as an
  offset/length into the image (NUL-terminated, < 128 bytes, else
  `ENOEXEC`).
- PIE executables load at `0x555555554000`, the interpreter at
  `0x7F0000000000` (fixed bases; no ASLR yet). The heap (`brk`) starts after
  the executable, never after the interpreter.
- The interpreter is read from the VFS like any file, mapped eagerly like
  the executable, and started at its own entry point. It must itself be
  `ET_DYN` without `PT_INTERP`.
- Linux auxv: `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_BASE`,
  `AT_FLAGS`, `AT_ENTRY`, `AT_UID/EUID/GID/EGID` (0), `AT_SECURE` (0),
  `AT_RANDOM`, `AT_HWCAP` (CPUID.1 EDX), `AT_CLKTCK` (100), `AT_EXECFN`
  (the path given to exec, on the stack). No vDSO (`AT_SYSINFO_EHDR` is
  left out; the C libraries fall back to real system calls).
- The process remembers the executable's path for `readlink("/proc/self/exe")`.

## Memory

- A VMA may be file-backed: it points at the file's bytes (ramdisk or
  sysroot module, both immutable in memory) and is filled page by page on
  first touch (private copy; pages past end of file are zero). ramfs files
  are copied eagerly at `mmap` time instead, since their buffer can move.
- `mmap`: `MAP_PRIVATE` / `MAP_SHARED` (both private copies; writes are
  never written back), `MAP_ANONYMOUS`, `MAP_FIXED` (replaces whatever was
  there, including exec-time image pages), `MAP_FIXED_NOREPLACE`
  (`EEXIST` on overlap). Without `MAP_FIXED` the hint is ignored and the
  next free address above `TASK_MMAP_BASE` is used.
- `munmap` frees every present page in the range and carves VMAs.
- `mprotect` splits VMAs and sets exact rights on present pages (W and NX
  bits). Limit: `PROT_NONE` on a page that is already present leaves it
  readable (no "present but inaccessible" PTE state yet); untouched
  `PROT_NONE` ranges fault as they should.
- Up to 64 VMAs per process.

## System calls added or completed (Linux numbers)

`pread64`, `mprotect` (real), `mmap` (file-backed, fixed), `munmap` (real),
`readlink`/`readlinkat` (`/proc/self/exe` only), `getrandom`, `prlimit64`
and `getrlimit`, `sched_getaffinity` (one CPU), `set_robust_list` (accepted),
`rseq` (`ENOSYS`), `ioctl(TCGETS)` on the terminal, `statx`,
`newfstatat(fd, "", AT_EMPTY_PATH)`, `madvise` (accepted), `getuid`-family
already return 0.

## Library source

- A second ustar archive, the *sysroot*, is passed by the boot loader as a
  Multiboot module (`qemu -initrd build/sysroot.tar`). It is mapped in
  place and read-only; ramdisk lookups fall through to it, so it appears
  merged into `/`. It is not part of the kernel image and is reported
  separately from the kernel footprint.
- Contents: the host's glibc runtime (`/lib64/ld-linux-x86-64.so.2`,
  `/lib/x86_64-linux-gnu/libc.so.6`, `libresolv.so.2` for BusyBox), musl
  (`/lib/ld-musl-x86_64.so.1`), a small test library, the dynamically
  linked test programs, and the distribution's dynamically linked BusyBox
  when present. Regular files only (no symlinks in the archive).

## Boot self-test

With the sysroot present: glibc and musl hello (PIE and non-PIE), argv/env
printing, `dlopen` of a test library, `busybox echo` and `busybox ls`.
Without it (GRUB disk boots) the checks are reported as skipped.

## Status

Passing in the boot self-test: glibc `dyntest` (PIE, `dlopen`), musl
`dyntest` (`dlopen`), distribution BusyBox (glibc) `echo` and `ls`.
`st_dev`/`st_ino` matter: musl's `dlopen` treats equal dev/ino as "already
loaded", so `stat`/`fstat`/`statx` report a per-file inode (address of the
backing image or node; not stable across boots).

Known limits / failures:

- `PROT_NONE` on an already present page stays readable (see Memory).
- Program output captured by the self-test is only the first 192 bytes.
- `MAP_SHARED` writes are never written back; no `mremap`, no vDSO, no
  `AT_SYSINFO_EHDR`, no ASLR, no `fork` of dynamically linked programs
  beyond what the static personality already supports.
- Splitting or trimming a file-backed VMA keeps its file bytes aligned with
  addresses (`vma_skip` in `kernel/syscall.c`).
- `mprotect` does not split VMAs; it sets rights on present pages only, and
  faults ranges in first, so later re-mapping uses the original VMA rights.
- The sysroot is only loaded when the boot loader passes it as a module;
  GRUB disk installs skip these checks.
