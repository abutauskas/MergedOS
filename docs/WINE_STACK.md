# Wine's complete stack on the Linux personality

Short spec written before the code; the code follows it. Owner decision: GUI
and console Windows programs run through Wine's own ELF side (`wine64`
loader, `ntdll.so`, `win32u.so`, `wineserver64`) plus its PE DLLs, natively
on the CPU, on top of the Linux personality. No emulator, no code copied from
other projects. MergedOS's own ntdll (`src/ntrt`) stays for the console tests.

## Delivery

- `make wine-linux` runs `apt-get download wine64 libwine libunwind8
  liblzma5`, unpacks each with `dpkg-deb -x` into its own empty directory
  under `build/wine/lx/` and packs a fixed subset (ELF loader, server,
  `ntdll.so`, `win32u.so`, the PE DLLs and NLS files a console program
  needs, `wine.inf`) into `build/wine/lx.tar`, a ustar boot module. Nothing is
  committed, nothing runs on the host. glibc comes from the sysroot.
- The module is searched like the sysroot (read-only, merged under `/`); the
  Wine prefix is `/home/user/.wine` on ramfs. Tests skip without the module.

## Kernel interfaces (tracked by logging the number of every ENOSYS)

- Files: long paths, cwd/`chdir`/`fchdir`/`getcwd`, `*at` calls with real
  dirfds, symlinks (ramfs + tar), `/dev/null`, `/dev/urandom`, `/proc/self/*`
  (maps, exe, fd), `pwrite`, `fsync`, `flock`, `utimensat`, fd table of 1024.
- Memory: refcounted frames, copy-on-write `fork`, real `MAP_SHARED` and
  `memfd_create` (shared pages, `ftruncate`), `mprotect` that splits VMAs,
  `PROT_NONE` enforced, `mremap`, `madvise`, `MAP_NORESERVE`, VMAs in a
  growable array, 128 TiB of lazily backed address space.
- Sockets: generic socket file in `kernel/socket.c` with a per-family ops
  table; `kernel/unix.c` is AF_UNIX stream/dgram, `socketpair`, bind/listen/
  accept/connect on ramfs paths, `sendmsg`/`recvmsg` with `SCM_RIGHTS`.
  AF_INET registers its own ops table later (not here).
- Readiness: `poll`/`ppoll`/`select`/`pselect6`, `epoll`, `eventfd2`.
- Signals: `SA_SIGINFO` with the full x86-64 `siginfo`/`ucontext`/`fpstate`,
  `sigaltstack`, `SA_RESTART`, CPU faults as SIGSEGV/SIGBUS/SIGFPE/SIGILL/
  SIGTRAP, `tgkill`, `rt_sigtimedwait`.
- Threads: futex `REQUEUE`/`CMP_REQUEUE`/`WAIT_BITSET`, robust lists,
  `prctl`, `sched_yield`, `setrlimit`.

## Goal and status

`wine64 cmd.exe /c echo hi` with `wineserver64` as a separate process, run
from the boot self-test. Status is kept at the end of this file.
