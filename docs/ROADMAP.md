# Roadmap

Each milestone ends in something that boots and passes its own self-test.
Effort grows sharply after M3; M7 and M8 are multi-year efforts for a team.

## M1 - Boot, exec, personalities (this release, v0.1.0)

Done: long-mode boot, frame allocator, per-task address spaces, ring-3
execution, ELF/PE/Mach-O loading, four syscall personalities with three
calls each, hardware probe, desktop selection policy, unified shell core,
package translation for deb/rpm/apk/pacman/.app with gzip, host tests, boot
self-test, GRUB ISO.

Known limits carried forward:

- No interrupts: devices are polled.
- Static executables only.
- BIOS boot only; QEMU is the only tested platform.
- Desktop choice is not persisted.

## M2 - A real kernel core

Done (see [KERNEL_PROCESSES.md](KERNEL_PROCESSES.md)): preemptive
round-robin scheduling of several user processes and threads, per-process
descriptor tables, `fork`/`vfork`/`clone`/`execve`/`wait4`, threads with
futexes and TLS, NT threads, signals with handlers and default actions,
pipes, keyboard stdin and Ctrl-C, a small page-backed kernel allocator,
demand-backed anonymous memory (1 GiB per process), direct map and frame
allocator for all RAM. Also from M3: a writable ramfs at `/tmp` and
`/home/user`.

Still open:

- APIC timer, interrupt-driven devices
- Per-CPU data via `%gs`/`swapgs`, SMP bring-up
- Copy-on-write fork (fork copies the touched pages today)
- Job control (SIGSTOP/SIGCONT), sessions, `SIGSYS` for unknown BSD/XNU calls

## M3 - Files and a user-space shell

- VFS with ramfs and a read-only disk file system
- File system calls for the Linux and FreeBSD personalities
- `msh` as a user-space process; pipes and redirection
- Jails: per-task root, namespaces, service filters
- Persisted settings, including the desktop choice

## M4 - Dynamic linking for the UNIX personalities

- ELF `PT_INTERP`, `mmap`, TLS (`arch_prctl`), futexes
- Run unmodified musl, then glibc, with their own dynamic linkers
- Target: BusyBox and a statically then dynamically linked `bash`
- FreeBSD `rtld` and libc on the FreeBSD personality

## M5 - Hardware

- Native drivers: AHCI/NVMe, virtio, USB HID, framebuffer
- Linux driver-API shim and a first GPL driver module
- Own TCP/IP stack (docs/NETWORKING.md, replaces the FreeBSD port): done for virtio-net/e1000, IPv4, AF_INET on the Linux personality; open: IPv6, other personalities
- UEFI boot

## M6 - Package store

- xz and zstd codecs (done: single-call decoders, CRC32/CRC64/SHA-256 and
  XXH64 checks; no xz BCJ filters, no zstd dictionaries)
- Content-addressed store under `/pkg`, installation, removal
- Dependency resolution over capabilities; base runtimes for glibc and musl
- Path views enforced through jails
- Signature verification
- AppImage, Snap and Flatpak backends (squashfs). Done: squashfs 4.0 reader
  (gzip/xz/zstd/lz4), AppImage and Snap metadata and program extraction,
  Flatpak bundle metadata. Open: Flatpak payload (OSTree objects), mounting

## M7 - NT and Darwin user space

- NT object manager, sections, PEB/TEB; user-space PE loader with imports
  and relocations; a native `ntdll.dll`
- Host Wine's Win32 DLLs on the NT personality; target console programs
  first
- Mach traps and IPC; `dyld` and `libSystem`; target Darwin command-line
  tools. Started: own `dyld` + `libSystem.B.dylib`, BSD/Mach call tables,
  CLI programs run (docs/DARWIN_USERSPACE.md); Mach IPC, Apple frameworks
  and real macOS binaries are still open
- Binary property lists, code-signature parsing

## M8 - Graphics and desktops

- Display server, GPU acceleration through Linux DRM drivers
- The four desktops as real sessions, with the memory budgets the selector
  assumes (2, 8, 48 and 128 MiB)
- Win32 GDI/USER and a free AppKit-compatible layer for GUI programs

## Not planned

- Instruction-set translation (running arm64 binaries on x86-64 or the
  reverse)
- Running other systems' kernel-mode drivers except through the Linux shim
- Binary compatibility with Windows or macOS kernel extensions
