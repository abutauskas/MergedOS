# ABI translation

How one kernel serves four system-call dialects, what is implemented, and
what each personality still needs before it can run real software.

## Principle

MergedOS does not emulate, interpret or rewrite foreign code. An x86-64
program for any of the four systems is ordinary x86-64 machine code; the
only place it depends on its home operating system is the kernel boundary.
That boundary is translated. Everything else runs natively.

This is the technique of FreeBSD's Linux ABI layer and of the first Windows
Subsystem for Linux, applied to four personalities at once. It handles
same-architecture binaries only: an arm64 macOS program on an x86-64 machine
would need instruction translation, which is out of scope.

## Stage 1: identification at exec

`binfmt_probe()` inspects headers only. All offsets are bounds-checked; the
unit tests feed it every truncation and many bit flips of each test image
under AddressSanitizer.

**ELF** needs care because Linux and FreeBSD share the container. The order
of evidence follows what multi-ABI kernels do in practice:

1. `EI_OSABI`: 9 is FreeBSD, 3 is Linux/GNU.
2. `PT_NOTE`: a `FreeBSD` ABI tag, or a `GNU` ABI tag with OS = Linux.
3. `PT_INTERP`: `ld-linux*` / `ld-musl*` is Linux, `ld-elf.so*` is FreeBSD.
4. Fallback brand: Linux. GNU toolchains leave `EI_OSABI` at 0, so an
   unbranded image is far more likely to be a Linux program.

**PE**: `MZ`, `e_lfanew`, `PE\0\0`, machine type, optional-header magic,
subsystem (console or GUI), DLL flag, and whether the import directory is
non-empty.

**Mach-O**: thin 64-bit images and fat (universal) containers; for fat
files the x86-64 slice is selected. `0xCAFEBABE` is also the Java class
magic, so the architecture count is sanity-checked.

`binfmt_load_plan()` then produces the same structure for all three:

```c
struct exec_plan {
    struct binfmt_info info;      /* format, arch, personality, flags */
    u64 entry;
    u32 nsegs;
    struct exec_seg { u64 vaddr, memsz, fileoff, filesz; u32 prot; } seg[16];
};
```

Format-specific handling worth noting: PE headers are mapped read-only at
the image base as their own segment; Mach-O `__PAGEZERO` (protection 0) is a
reservation and is not mapped; a fat slice's offsets are rebased to the
whole file.

Images that need a dynamic linker run under it (ELF `PT_INTERP`: see
DYNAMIC_LINKING.md; Mach-O `LC_LOAD_DYLINKER`: see DARWIN_USERSPACE.md); a
Mach-O `LC_MAIN` without a dylinker and PE DLLs are recognised and rejected
with "not supported yet" rather than run incorrectly. PE32+ programs that *import*
DLLs are run: see "NT: imports and the built-in Win32 runtime" below.

## Stage 2: initial process state

| | Stack and registers at entry |
|---|---|
| Linux | `rsp` -> `argc, argv[], NULL, envp[], NULL, auxv` |
| FreeBSD | same vector; its address in `rdi`; `rsp` aligned as after a `CALL` |
| XNU (static) | `rsp` -> `argc, argv[], NULL, envp[], NULL, apple[]` |
| NT | entry point is *called*: `rsp` holds a return address and 32 bytes of home space; `rcx` would carry the PEB |

For NT the return address points at a small thunk mapped into the process
that performs `NtTerminateProcess(-1, eax)`, mirroring what
`ntdll!RtlUserThreadStart` does, so a PE entry point can simply `return`.
A PE that imports DLLs starts in the built-in runtime instead (`rip` =
its start routine, `rdi` = its parameters page, `gs` base = the TEB); the
runtime sets up TEB/PEB and static TLS, runs TLS callbacks, then calls the
image's entry point with the PEB in `rcx`.

### NT: imports and the built-in Win32 runtime

No DLL files are loaded. `kernel/ntload.c` does the work of the Windows
image loader for one static image:

1. **Place.** The image goes to its preferred base unless it clashes with
   the runtime area or sets `IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE`; such
   images are moved to `0x200000000`, as ASLR would move them, so the
   relocation path runs for every MinGW program. A fixed image that does
   not fit is refused.
2. **Relocate.** `binfmt_pe_relocs()` walks `.reloc` (ABSOLUTE and DIR64;
   anything else is "not supported").
3. **Map the runtime.** `src/ntrt/` is user-mode C with the Microsoft x64
   calling convention (`__attribute__((ms_abi))`), linked at `0x7F000000`
   and embedded in the kernel as a flat binary (`kernel/ntrt_blob.S`). It
   begins with a header (`src/ntrt/ntrt_abi.h`) giving its export table,
   start routine, TEB and missing-import handler.
4. **Bind.** `binfmt_pe_imports()` walks the import directory; every
   symbol imported by name from `kernel32`, `kernelbase`, `ntdll`,
   `msvcrt`, `ucrtbase`, `api-ms-win-*`, `advapi32` or `secur32` is looked
   up in the runtime's export table. Anything else gets a stub that prints
   `missing import DLL!Name` and exits with `STATUS_ENTRYPOINT_NOT_FOUND`
   (0xC0000139) *when called*, so a program that never calls it still runs.
5. **Parameters.** A page at `0x7FFD0000` (image base, entry, stack,
   ids, computer name, command line) and a `KUSER_SHARED_DATA` page at
   `0x7FFE0000` with the version fields, filled from `abi_identity(ABI_NT)`
   (Windows 11, 10.0.22631). `GetVersionEx` and `RtlGetVersion` read them.

The runtime provides about 200 exports: process start-up, console I/O,
heap, virtual memory, TLS, critical sections (no-ops: one thread), version,
code pages (UTF-8 only), module lookups (`GetModuleHandle` / `LoadLibrary`
return a pseudo handle for the modules above; `GetProcAddress` searches the
built-in table), and the msvcrt / ucrtbase start-up glue, stdio on the three
console streams, strings, and an own printf engine (narrow and wide,
Microsoft length modifiers, `_snprintf` / `__stdio_common_vs*printf`
semantics). Its ntdll stubs issue the NT system calls below.

What runs: `win_hello.exe` (MinGW-w64, normal msvcrt CRT; boot self-test)
and Wine 9.0's own x64 builds of `hostname.exe` and `whoami.exe`, run
unmodified (`make WINE_EXE_DIR=...`; checked by the self-test when
present). What does not: GUI programs, threads, SEH / C++ exceptions
(`__C_specific_handler` declines everything, a fault kills the process),
files other than the console, the registry, real DLLs, 32-bit PE. The
shell passes no arguments, so `argc` is 1. `NtFreeVirtualMemory` keeps
pages mapped until exit, and `NtProtectVirtualMemory` only adds rights.

## Stage 3: system-call dispatch

`syscall_entry` (assembly) saves one register frame. `abi_dispatch()` then
calls three per-personality operations:

- `decode`: registers (and for NT, the user stack pointer) to a neutral
  `struct abi_call { nr, klass, arg[6], usp }`.
- `lookup`: bounds-checked index into a dense table.
- `encode`: native result to the personality's return convention.

Thunks call native services that return `>= 0` or a negative native error
(`-MERR_*`). Native error numbers match no foreign ABI on purpose; every
personality has to translate, so none can leak another's numbering.

| Native | Linux | FreeBSD / XNU | NT |
|---|---|---|---|
| `MERR_BADF` | `-9` | 9 + carry | `STATUS_INVALID_HANDLE` |
| `MERR_FAULT` | `-14` | 14 + carry | `STATUS_ACCESS_VIOLATION` |
| `MERR_AGAIN` | `-11` | 35 + carry | `STATUS_RETRY` |
| `MERR_NOSYS` | `-38` | 78 + carry | `STATUS_INVALID_SYSTEM_SERVICE` |

`NtWriteFile` shows why thunks exist. POSIX `write` takes three register
arguments and returns the count. `NtWriteFile` takes nine: four in
registers, five read from the caller's stack at `rsp + 0x28`; the first is a
handle that must be translated to a descriptor; and the byte count comes
back through a user-space `IO_STATUS_BLOCK`, not the return value.

**Cost.** Two indirect calls, one bounds check, one thunk. No hashing, no
string comparison, no allocation.

## What is implemented

| Personality | Calls | Verified by |
|---|---|---|
| Linux | `write` 1, `getpid` 39, `exit` 60, `exit_group` 231 | `hello.linux`: bss zeroing, writable data, `-EBADF`, `-ENOSYS` |
| FreeBSD | `exit` 1, `write` 4, `getpid` 20 | `hello.freebsd`: `rdi` vector, carry-flag errors |
| XNU | BSD class: `exit` 1, `write` 4, `getpid` 20 | `hello.macho`: class-tagged numbers, carry-flag errors |
| NT | `NtReadFile` 0x06, `NtWriteFile` 0x08, `NtAllocateVirtualMemory` 0x18, `NtFreeVirtualMemory` 0x1E, `NtTerminateProcess` 0x2C, `NtQueryPerformanceCounter` 0x31, `NtProtectVirtualMemory` 0x50, `NtQuerySystemTime` 0x5A | `hello.exe`: stack arguments, `IO_STATUS_BLOCK`, NTSTATUS, return-from-entry; `win_hello.exe` (MinGW-w64 CRT) through the built-in runtime |

The NT row is 8 table entries. Linux alone has roughly 360 system calls.

Known divergence: real FreeBSD and XNU also raise `SIGSYS` for an unknown
system-call number. MergedOS has no signal delivery yet and only returns
`ENOSYS`.

## What each personality needs next

**Linux.** The most tractable: the system-call interface is the stable ABI
and is documented. Needs the VFS, `mmap`/`brk`, `clone`/`fork`/`execve`,
signals, futexes, `epoll`, sockets, `/proc` and `/sys` subsets, then an
unmodified glibc or musl and `ld.so` run as-is.

**FreeBSD.** Similar in kind; largely shares native services with Linux
under different numbers and structure layouts, plus `kqueue`, `sysctl` and
`rtld`.

**XNU / macOS.** BSD system calls are the easy part. macOS programs are
never statically linked in practice: they need `dyld`, `libSystem`, Mach
traps and Mach IPC, and then Apple's frameworks. Open-source `dyld` and
`libSystem` exist under the APSL; the frameworks do not. Darling's
reimplementations are the existing free approach. Realistic near-term
target: command-line Darwin tools.

**NT / Windows.** The system-call layer is not the application ABI. Service
numbers change between Windows builds, and applications call `ntdll.dll`
and Win32 DLLs, never the kernel directly. So:

- MergedOS defines its own NT service table; the built-in runtime's ntdll
  stubs use it. (The numbers are pinned to their long-standing Windows
  10/11 x64 values, but nothing may rely on that.)
- Done: imports, relocations, TEB/PEB with the `gs` base, static TLS, a
  small kernel32/msvcrt/ucrtbase subset (see Stage 2).
- Next: loading real DLL files (and their own imports and exports),
  threads, SEH unwinding, files and handles. Win32 beyond that is a
  user-space DLL set, which is the entire scope of Wine; the realistic plan
  is to host Wine's DLLs (LGPL) on the NT personality rather than grow the
  built-in runtime.

## Adding a system call

1. Add or reuse a native service in `kernel/syscall.c`
   (`long msys_foo(struct task *, ...)`, returning `-MERR_*` on error).
2. Declare it in `abi_dispatch/abi.h` and add a mock in `tests/test_main.c`.
3. Add a thunk and a table entry in each personality that has the call.
4. Add a case to `tests/test_abi.c` per personality, including an error path.
