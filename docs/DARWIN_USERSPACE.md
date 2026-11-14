# Darwin user space: dyld, libSystem, Mach traps (M7, workstream 3)

Short spec written before the code; the code follows it. Goal: dynamically
linked x86-64 Mach-O command-line programs run natively (no emulation) on the
XNU personality. Apple's `dyld` and `libSystem` are neither available nor
redistributable here, so MergedOS ships its own, written from scratch.

## Kernel: exec

- `MH_EXECUTE` with `LC_LOAD_DYLINKER` (and `LC_MAIN` or `LC_UNIXTHREAD`) is
  planned like a dynamic ELF: the executable's segments are mapped at their
  link-time addresses (`__PAGEZERO` is a reservation only), the dynamic
  linker named by `LC_LOAD_DYLINKER` is read from the VFS, its segments are
  mapped at *its* link-time address (no bias), and execution starts at its
  entry point (`LC_MAIN` entryoff or `LC_UNIXTHREAD` rip). `MH_DYLINKER` and
  `MH_EXECUTE` both qualify as the linker image; it may not name a linker.
- A Mach-O with `LC_MAIN` but no dylinker is refused (`ENOEXEC`-class
  `NOTSUP`): its entry point is a C function, not a process start.
- Initial stack for a dynamic program, Darwin style, 16-byte aligned:
  `rsp -> [mach_header *][argc][argv..][0][envp..][0][apple..][0]`, strings
  above. `apple[0]` is `executable_path=<absolute path>`.
- File-backed mappings reuse the Linux VMA code (`msys_mmap` with an fd).

## Kernel: system calls (class 2, BSD; class 1, Mach; class 3, machdep)

- BSD, XNU numbers, carry flag + positive errno: `exit`, `read`, `write`,
  `open`/`openat` (Darwin `O_*` flags translated), `close`, `pread`, `lseek`,
  `mmap`/`munmap`/`mprotect` (Darwin `MAP_*`), `stat64`/`lstat64`/`fstat64`
  (144-byte Darwin `struct stat`), `access`, `getattrlist` (`ENOTSUP`),
  `sysctl` and `sysctlbyname` (a small `kern.*` / `hw.*` table), `issetugid`,
  `getpid`/`getuid`-family, `getentropy`, `csops` and `proc_info` (stubs),
  `shared_region_check_np` (`ENOMEM`: dyld maps libraries itself),
  `thread_selfid`, `bsdthread_register`, `ulock_wait`/`ulock_wake`, `unlink`,
  `mkdir`, `rmdir`, `rename`, `dup`/`dup2`, `fcntl`, `ioctl` (`ENOTTY`),
  `gettimeofday`, `sigaction`/`sigprocmask`/`sigaltstack` (accepted).
- Mach traps: `mach_reply_port`, `task_self_trap`, `thread_self_trap`,
  `host_self_trap` (fixed port names), `mach_msg_trap` (a canned
  `KERN_NOT_SUPPORTED` MIG reply when a reply is wanted, otherwise
  `MACH_SEND_INVALID_DEST`), `_kernelrpc_mach_vm_allocate/deallocate/protect/
  map_trap` (anonymous memory only), `mach_port_*` traps (accepted),
  `mach_absolute_time`, `mach_timebase_info` (1/1, nanoseconds).
- Class 3: `thread_fast_set_cthread_self` sets the `gs` base (TSD).

## User space: `dyld` and `libSystem.B.dylib`

- Both are freestanding C built with the host `clang -target
  x86_64-apple-macos10.15` and `ld64.lld`, installed as `/usr/lib/dyld` and
  `/usr/lib/libSystem.B.dylib` in the sysroot module. `dyld` is a static
  executable linked at `0x7FF000000000`.
- `dyld` entry gets the Darwin stack, parses the main image, loads
  `LC_LOAD_DYLIB`/`LC_LOAD_WEAK_DYLIB`/`LC_REEXPORT_DYLIB` recursively
  (open, `pread` header, reserve the span, map segments, rebase), applies
  fixups (`LC_DYLD_CHAINED_FIXUPS` and classic `LC_DYLD_INFO` rebase/bind/
  weak/lazy opcodes; lazy binds are resolved eagerly), resolves symbols
  through each image's export trie with two-level namespace (ordinal ->
  image), re-exports and weak imports (`0` when absent), runs
  `__mod_init_func` / `__init_offsets` initialisers, hands `argc/argv/envp/
  apple` to libSystem, calls `main` and then `exit`.
- Every missing library and every unresolved symbol is collected and
  reported once before exiting 1 (real-dyld wording: `Library not loaded:`,
  `Symbol not found:`, plus `Referenced from`/`Expected in`).
- `libSystem.B.dylib` exports the libc subset small CLI programs import
  (see `src/darwin/libsystem.c`): stdio (`printf` family, `fopen`/`fgets`/
  `fread`/`fwrite`/`fclose`, `puts`, `putchar`, `fflush`, `__stdoutp` ...),
  `malloc` family, `string.h`, `ctype`, `strtol`, `getenv`, `exit`/`_exit`/
  `abort`/`atexit`, `__error` (errno), `___stack_chk_guard`/`___stack_chk_fail`,
  `dyld_stub_binder`, `_NSGetArgv`/`_NSGetEnviron` and friends.

## Tests

- Host tests: Mach-O plan with dylinker, Darwin stack layout inputs, BSD/Mach
  thunks (flags, stat layout, sysctl, Mach traps), import reporting.
- Boot self-test (needs the sysroot module): hello, argv/env, file I/O on
  `/tmp`, a program importing a missing framework (exact report).

## .app bundles

- The package translator lists every `LC_LOAD_DYLIB` and, per library not
  provided by MergedOS, the imported symbols (`binfmt_macho_imports`), so a
  bundle that needs Foundation/AppKit says exactly what is missing instead of
  failing vaguely.

## Status

Passing in the boot self-test (needs the sysroot module and the host
clang + ld64.lld): `darwin-hello` (classic fixups), `darwin-argv` (chained
fixups, argv/env/heap/`snprintf`), `darwin-fileio` (`fopen`/`fgets`/`stat`/
`unlink` on `/tmp`), `darwin-dyn` (a re-exporting dylib, a dylib initialiser
and data, a weak import resolved to 0, flat-namespace lookup), and
`darwin-needs-foundation`, which dyld refuses with the exact library and
symbols (`Library not loaded: .../Foundation (image not found)`, `Missing
symbols: _NSApplicationMain _NSLog`). Host tests cover the call tables, the
Mach-O plan, import walking (classic and chained) with truncation fuzzing
and the `.app` report. Without clang/ld64.lld the Darwin images are empty
files and the checks are reported as skipped.

Known limits:

- Only programs built against this libSystem are tested. Real macOS binaries
  need more of libSystem (and usually libc++ / libobjc, which do not exist
  here): every symbol or library that is absent is reported by dyld, not
  guessed at. No Apple frameworks, no Objective-C/Swift runtime.
- Kernel: no `fork`/threads/signals for Darwin programs, no Mach IPC (every
  `mach_msg` request/reply is answered `KERN_NOT_SUPPORTED`), `ulock_wait`
  returns at once, `getattrlist`/`proc_info`/`csops` are stubs,
  `mprotect` inherits the Linux limits (see DYNAMIC_LINKING.md),
  `hw.memsize`/`hw.physmem` are fixed at 1 GiB, `sysctl` is a small table.
- dyld: thin x86-64 images only (no fat dylibs), no `dlopen`/`dlsym`, no
  `DYLD_*` variables, no weak-definition coalescing, no threaded
  (arm64e) binds, no Objective-C/Swift registration, 24 images, no
  interposing, `__DATA_CONST` stays writable. Chained pointer formats 2 and 6
  and import formats 1 to 3 are handled.
- libSystem: no `scanf`, locale, time zones, threads, signals or sockets;
  `strtod`/`atof` are absent and `printf` renders `%f` with simple rounding
  only (no `%e`/`%g`); `malloc` never returns memory to the kernel.
- Only the first 256 bytes of a program's output are captured by the
  self-test.
