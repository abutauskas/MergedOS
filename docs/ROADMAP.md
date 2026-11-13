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

- One task at a time; a task that loops forever hangs the machine.
- No interrupts: devices are polled.
- RAM above 4 GiB is not managed.
- Static executables only.
- BIOS boot only; QEMU is the only tested platform.
- Desktop choice is not persisted.

## M2 - A real kernel core

- IDT for device interrupts, APIC and timer, preemptive round-robin scheduler
- Per-CPU data via `%gs`/`swapgs`, SMP bring-up
- Kernel heap (slab), direct map for all RAM
- `fork`/`clone`, `wait`, threads
- Signals (and with them `SIGSYS` for unknown BSD/XNU calls)
- Demand paging and copy-on-write

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
- Port of the FreeBSD network stack; sockets for all personalities
- UEFI boot

## M6 - Package store

- xz and zstd codecs
- Content-addressed store under `/pkg`, installation, removal
- Dependency resolution over capabilities; base runtimes for glibc and musl
- Path views enforced through jails
- Signature verification
- AppImage, Snap and Flatpak backends (squashfs)

## M7 - NT and Darwin user space

- NT object manager, sections, PEB/TEB; user-space PE loader with imports
  and relocations; a native `ntdll.dll`
- Host Wine's Win32 DLLs on the NT personality; target console programs
  first
- Mach traps and IPC; `dyld` and `libSystem`; target Darwin command-line
  tools
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
