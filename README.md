# MergedOS

A free, open-source, low-memory hybrid kernel that runs programs built for
**Linux, FreeBSD, macOS and Windows** by giving each process the system-call
*personality* its binary format asks for. There is no CPU emulation and no
virtual machine: foreign code runs directly on the processor and only the
kernel boundary is translated.

**Status: working proof of concept (v0.1.0), not a production OS.** The
kernel boots on x86-64 in about 428 KiB of RAM, loads ELF, PE32+ and Mach-O
images, and runs one statically linked test program per ABI. It does not yet
run real-world applications. See [What works today](#what-works-today) for
the exact line between implemented and planned.

```
$ make test-boot QEMU_MEM=64
MergedOS 0.1.0 (x86_64) - hybrid kernel proof of concept
[hw] cpu: AuthenticAMD, QEMU Virtual CPU version 2.5+ ..., 1 logical CPU
[hw] ram: 63 MiB available (65023 KiB)
[hw] gpu0: 1234:1111 QEMU/Bochs standard VGA at 00:02.0 - basic framebuffer, 16 MiB aperture
[gfx] 1024x768, 32 bpp framebuffer via Bochs VBE; PS/2 mouse found
[mem] kernel footprint: 352 KiB resident (image + boot tables 348 KiB, frame bitmap 4 KiB)

== ABI personalities: four binary formats, one kernel ==
Hello from a Linux ELF64 binary: write=1, exit_group=231, errors as -errno!
[abi] bin/hello.linux    ELF64     -> linux   5 syscalls, 18 pages, exit 0  OK
Hello from a FreeBSD ELF64 binary: write=4, exit=1, errors via carry flag!
[abi] bin/hello.freebsd  ELF64     -> freebsd 4 syscalls, 17 pages, exit 0  OK
Hello from a macOS Mach-O binary: write=0x2000004, exit=0x2000001, carry-flag errors!
[abi] bin/hello.macho    Mach-O 64 -> xnu     4 syscalls, 17 pages, exit 0  OK
Hello from a Windows PE32+ binary: NtWriteFile=0x08, 9 arguments, NTSTATUS result!
[abi] bin/hello.exe      PE32+     -> nt      4 syscalls, 19 pages, exit 0  OK

== MergedOS desktop selector ==
   [1] Taskbar shell     Windows-style      --      needs >= 192 MiB RAM (have 63)
   [2] Dock layout       Mac-like layout     --      needs >= 1024 MiB RAM (have 63)
 * [3] Tiling WM         Linux-style     score  59  fit 98% (ram 96%, gpu 100%, cpu 100%)
   [4] Retro 640x480x16  TempleOS-style  score  40  fit 100% (ram 100%, gpu 100%, cpu 100%)

msh:/> mpkg run /pkg/hello-1.0-1.x86_64.rpm
[mpkg] hello 1.0-1 (rpm (Red Hat / SUSE family)) -> linux personality, running ./usr/bin/hello
Hello from a Linux ELF64 binary: write=1, exit_group=231, errors as -errno!
...
MERGEDOS-SELFTEST: PASS (0 failures)
```

## Contents

- [What works today](#what-works-today)
- [Quick start](#quick-start)
- [Repository layout](#repository-layout)
- [ABI translation logic](#abi-translation-logic)
- [Memory optimisation](#memory-optimisation)
- [Hardware-aware desktop selector](#hardware-aware-desktop-selector)
- [Unified command line](#unified-command-line)
- [Package-to-native translation](#package-to-native-translation)
- [Installation](#installation)
- [Licensing](#licensing)
- [Roadmap](#roadmap)

## What works today

| Area | Implemented and tested | Not yet (see [roadmap](docs/ROADMAP.md)) |
|---|---|---|
| Boot | Multiboot 1 (GRUB, QEMU `-kernel`), 32-bit to long mode, higher-half kernel | UEFI boot, other architectures |
| Memory | Bitmap frame allocator, 4-level paging, per-task address spaces, W^X mappings, leak check | Kernel heap, demand paging, swap, RAM above 4 GiB |
| Execution | Ring-3 tasks via `SYSCALL`/`SYSRET`, fault isolation, one user task at a time, preemptible, with a CPU budget that ends runaway programs | Several user processes at once, SMP, signals |
| Interrupts and threads | PIC remapped, 100 Hz PIT tick, kernel threads with round-robin preemption and sleep | APIC/HPET timers, priorities, mutexes, interrupt-driven keyboard, mouse and disks (still polled) |
| Binary formats | ELF64 (Linux and FreeBSD brands), PE32+, Mach-O 64 and fat; static ELF and Mach-O images; PE32+ console programs that import DLLs, bound by the kernel's NT loader to a built-in Win32 runtime (about 200 kernel32/ntdll/msvcrt/ucrtbase functions; runs MinGW-w64 programs and Wine's `hostname.exe` / `whoami.exe`) | Dynamic linking: `ld.so`, `dyld`; real DLL files; GUI programs, threads, SEH, 32-bit PE |
| System calls | `write`, `exit`, `getpid` (+ `exit_group`, `NtWriteFile`, `NtTerminateProcess`) per personality, with each ABI's own numbers, argument passing and error convention; NT also `NtReadFile`, `NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `NtProtectVirtualMemory`, `NtQueryPerformanceCounter`, `NtQuerySystemTime` | Everything else: files, memory, processes, sockets |
| Graphics and input | 1024x768x32 linear framebuffer (GRUB video mode, or Bochs VBE under QEMU), anti-aliased text (strokes built from the 5x7 dot font, 3x3 samples), rounded boxes, gradient logo, framebuffer text console, PS/2 mouse pointer; `gfx=off` keeps VGA text | USB input |
| Setup wizard | Widget toolkit (label, button, text box, checkbox, list; mouse hit-testing, Tab focus), logo, wizard steps 1 to 6: language (English, Dutch, Indonesian, Spanish, German), name, password (PBKDF2-HMAC-SHA256, on by default, can be switched off), storage, Wi-Fi (states that there is no wireless driver; Skip), desktop (four layouts, or "recommended", which follows the hardware). Saving restarts the machine | Wi-Fi itself; changing the setup afterwards |
| Boot and login | Splash with logo and progress bar; password prompt (graphical, or on the text console with `gfx=off`) checked against the stored hash, doubling delay after each wrong attempt; then the saved desktop layout | Disk encryption (the password gates this OS only, not the disk); several users; lock screen |
| Text | UTF-8 decoding; ASCII plus Latin-1 letters drawn as base letter + diacritic | Other scripts (Cyrillic, Greek, CJK, Arabic), combining sequences, right-to-left; typing accents on the keyboard |
| Storage | SATA disks through AHCI, IDE disks as fallback (both polled). Reads MBR and GPT, names NTFS, BitLocker, FAT, exFAT, ext, swap, LUKS, XFS, Btrfs. Adds one MergedOS partition in unallocated space (MBR or GPT) or on a blank disk and saves the setup there; next boot skips the wizard | NVMe and USB disks; 4K-sector disks; a file system; installing the OS itself to disk (it still boots from the ramdisk/ISO) |
| Hardware probe | CPUID, ACPI MADT CPU count, firmware memory map, PCI scan with GPU classification | Drivers; USB, storage, network, real graphics |
| Desktop | Selection policy; graphical shell in four layouts (taskbar, dock, tiling, retro) with a small window manager: up to six overlapping windows (Terminal running msh, System, Files, Mergedfence, Viewer), z-order, close/minimise/maximise per layout style, tiling in the tiling layout, drag with a live outline, running-window strip (taskbar buttons, dock dots, top-bar titles), clipped repaint without a back buffer. Files runs programs and opens every other file in the Viewer | Keyboard window management, resizing, scrolling lists, real applications, a compositor, wallpapers, settings. A terminal tile narrower than 80 columns shows only what fits. With `gfx=off` the text mock-ups remain |
| Shell | CMD, PowerShell and POSIX names for the same builtins; both path notations; in-kernel | User-space shell, pipes, scripting, VFS-backed commands |
| Mergedfence | Native `.merged` package format (manifest + payload + SHA-256 trailer, verified before use); domain blocking engine for hosts-file and `||domain^` / `@@||domain^` lists with parent-domain matching; `mergedfence status\|check\|on\|off` (alias `fence`) and a desktop launcher; sample list of fictional `.example`/`.invalid` domains | A network stack and resolver to apply verdicts to (nothing is blocked on the wire yet); signed lists; third-party cookie blocking (only possible inside a browser) |
| File types | Every file opens: `file <path>` names it (content first, file name second) with a MIME type; `open <path>` shows it in a Viewer window with Text / Hex / Image / Info tabs, or prints an info card and preview in text mode. Shown: UTF-8 text (plain, scripts, JSON, XML/HTML, Markdown, CSV, SVG: as source, not formatted), hex dump of anything, BMP (24/32-bit) and PNG (non-interlaced, all colour types, CRC- and Adler-checked) decoded and scaled to the window, zip / tar / tar.gz listings. Identified with header facts only: JPEG, GIF, WebP, ICO (dimensions), WAV, FLAC, MP3 (rate, channels, length), Ogg, MP4/MOV, MKV/WebM, AVI, PDF (page count, heuristic), DOCX/XLSX/PPTX/EPUB/ODF/JAR/Android APK (by zip members), xz, zstd, bzip2, 7z, RAR, TTF/OTF/WOFF, SQLite, ISO 9660, MBR/GPT images; programs and packages via the existing probes. Samples in `/home/samples` | Decoders for JPEG, GIF, WebP; audio playback (no sound driver); video; rendering PDF or office documents; extracting archives; interlaced PNG; files outside the ramdisk (no VFS); scrolling by wheel or keyboard (Viewer pages with Up/Down) |
| Packages | `.deb`, `.rpm`, `.apk`, pacman: read, translate metadata, extract and run a static program. `.app`: Info.plist, dependencies, launch. gzip, xz and zstd codecs. squashfs 4.0 reader (gzip/xz/zstd/lz4); AppImage and Snap: metadata, listing, program extraction; Flatpak bundles: metadata only | bzip2, xz BCJ filters, zstd dictionaries, installation, dependency resolution, Flatpak payload (OSTree), DMG backend |

Two claims in the original project brief need qualifying, and the code is
written to match the qualified version:

- **"Zero emulation, zero overhead."** No instruction is emulated, so foreign
  code runs at native CPU speed. System calls are not free: each one costs a
  table lookup and an argument-marshalling thunk on top of a native call.
- **"Runs Windows, macOS, Linux and FreeBSD binaries."** The kernel side of
  that is what exists here. Real applications also need user-space runtimes
  that this repository does not contain: a C library and dynamic linker per
  personality, a Win32 API implementation (the job Wine and ReactOS do), and
  Apple's frameworks (the job Darling does). Those are years of work and are
  on the roadmap as such.

## Quick start

Requirements: an x86-64 Linux host with `gcc`, `binutils`, `make`,
`python3`, and `qemu-system-x86_64`. For the ISO: `grub-mkrescue`,
`xorriso`, `mtools`.

```bash
sudo apt-get install build-essential python3 qemu-system-x86 grub-pc-bin grub-common xorriso mtools
```

```bash
make            # build/mergedos.elf (GRUB) and build/mergedos.elf32 (QEMU -kernel)
```

```bash
make test       # host unit tests, QEMU boot self-test, disk install tests, setup/restart/login end-to-end test
```

```bash
make run        # boot with a VGA window; serial console on stdio
```

```bash
make run-serial APPEND="de=retro"     # headless, force a desktop profile
```

```bash
make iso        # build/mergedos.iso, bootable with GRUB
```

```bash
make tools && build/host/mpkg inspect some-package.rpm
```

Kernel command line: `selftest` (run the scripted self-test and power off),
`de=<taskbar|dock|tiling|retro>`, `de_timeout=<seconds>`, `gfx=off`, `setup=off` (skip the setup wizard; it also
stops appearing once a setup has been saved to a disk), `splash=<ms>` (how long the
boot splash stays up; default 500).

At the `msh:/>` prompt try `help`, `dir`, `ls /pkg`, `Get-Process`,
`hello.exe`, `./bin/hello.linux`, `open /Applications/Hello.app`,
`mpkg inspect /pkg/hello_1.0-1_amd64.deb`, `de dock`.

## Repository layout

```
MergedOS/
├── Makefile                    build, run, test, iso
├── LICENSE                     BSD-2-Clause
├── kernel/                     the kernel proper
│   ├── kmain.c                 entry point and boot sequence
│   ├── arch/x86_64/
│   │   ├── boot.S              Multiboot header, long-mode entry, boot page tables
│   │   ├── entry.S             SYSCALL entry, exception stubs, ring-3 enter/exit
│   │   ├── cpu.c               GDT, TSS, IDT, SYSCALL MSRs
│   │   ├── traps.c             exception handling
│   │   └── linker.ld           higher-half image layout
│   ├── pmm.c  vmm.c            physical frames, address spaces
│   ├── task.c  syscall.c       exec for all formats; native services
│   ├── sched.c                 kernel threads, timer tick, preemption
│   ├── deskrun.c               desktop thread: mouse, windows, drag outline, status
│   ├── console.c  printk.c     text console (VGA text or framebuffer) + serial
│   ├── gfx.c  font5x7.c        framebuffer mode set, drawing, pointer sprite
│   ├── mouse.c                 PS/2 mouse
│   ├── setup.c                 splash, wizard and login event loops, salt source, disk glue
│   ├── ahci.c  ata.c           SATA (AHCI) and IDE disk drivers, polled
│   ├── initrd.c                ustar ramdisk, read in place
│   ├── monitor.c               in-kernel shell front end
│   └── include/mergedos/       kernel headers
├── abi_dispatch/               ABI translation layer
│   ├── binfmt.c                ELF / PE / Mach-O detection and load planning
│   ├── dispatch.c              system-call dispatcher
│   ├── personality_linux.c     one file per personality:
│   ├── personality_freebsd.c   register convention, syscall table,
│   ├── personality_xnu.c       result encoding
│   ├── personality_nt.c
│   └── errno_map.c             native errors -> errno / NTSTATUS
├── ui/                         widget toolkit, UTF-8 text, anti-aliasing, logo (portable)
├── installer/                  setup wizard, login, strings, SHA-256 / PBKDF2, saved-setup sector (portable)
├── storage/                    partition tables, safe allocation (portable)
├── desktop/                    graphical shell: window manager, layouts, icons (portable)
├── filetype/                   file identification, text/hex/image/archive viewers (portable)
├── hardware_probe/             CPUID, ACPI MADT, PCI, GPU classification
├── de_selector/                desktop policy, boot menu, session start
├── pkg_translate/              package-to-native translation layer
│   ├── pkgfmt.c                format identification
│   ├── containers.c  tar.c     ar, cpio, ustar readers
│   ├── inflate.c               gzip decoder
│   ├── xz.c  zstd.c            xz (LZMA2) and Zstandard decoders
│   ├── squashfs.c              squashfs 4.0 reader
│   ├── bundles.c               AppImage, Snap, Flatpak bundles
│   ├── metadata.c              Debian control, .PKGINFO, RPM headers
│   ├── appbundle.c             macOS .app bundles
│   ├── depmap.c                dependency -> capability mapping
│   └── translate.c             manifest, payload access, store path view
├── src/
│   ├── msh/                    unified shell core (aliases, paths, tokenizer)
│   └── guest/                  test programs: hand-written ELF, PE32+, Mach-O
├── tests/                      host unit tests
├── tools/                      mkimage.py, mpkg.c, boot-test.sh, disk-test.sh, e2e-test.py, grub.cfg
└── docs/                       design documents, logo.svg
```

Roughly 9,300 lines. Code under `abi_dispatch/`, `pkg_translate/`,
`src/msh/`, `de_selector/de_policy.c` and `hardware_probe/hw_classify.c` is
portable C with no kernel dependencies; the same sources are linked into the
kernel, the unit tests and the `mpkg` host tool.

## ABI translation logic

Full detail: [docs/ABI_TRANSLATION.md](docs/ABI_TRANSLATION.md).

On x86-64, Linux, FreeBSD, macOS and Windows all enter the kernel with the
same instruction, `SYSCALL`. They differ in what the registers mean. So the
kernel has one entry point and decides per process how to read it.

**1. At exec, pick a personality from the file itself** (`binfmt.c`):

| Evidence in the image | Personality |
|---|---|
| ELF, `EI_OSABI` = 9, or a `FreeBSD` note, or interpreter `ld-elf.so` | FreeBSD |
| ELF, `EI_OSABI` = 3, or a `GNU` ABI note, or `ld-linux` / `ld-musl`; unbranded ELF | Linux |
| `MZ` + `PE\0\0` | NT |
| Mach-O magic, or the x86-64 slice of a fat binary | XNU |

ELF program headers, PE sections and Mach-O load commands are reduced to one
neutral segment list, so there is a single mapping routine in the kernel.

**2. At each system call, translate at the boundary** (`dispatch.c`):

```
SYSCALL -> syscall_entry (entry.S) saves one ABI-neutral register frame
        -> abi_dispatch(): decode -> lookup -> thunk -> encode
        -> SYSRET
```

| | Linux | FreeBSD | XNU | NT |
|---|---|---|---|---|
| Call number | `rax` | `rax` | `rax` = class<<24 \| n | `eax` = service index |
| Arguments | rdi rsi rdx r10 r8 r9 | same | same | r10 rdx r8 r9, then stack at `rsp+0x28` |
| `write` | 1 | 4 | 0x2000004 | `NtWriteFile` 0x08, nine arguments |
| Error return | `rax` = -errno | carry flag, `rax` = errno | carry flag, `rax` = errno | `eax` = NTSTATUS |
| "Not implemented" | -38 | 78 | 78 | 0xC000001C |

Each personality is a dense array indexed by call number, so lookup is one
bounds check and one load. Thunks marshal arguments into a small set of
native services (`msys_*`) that return native error codes; the personality
encodes those in its own dialect on the way out.

**How this is verified.** The four test programs in `src/guest/` are complete
executables written out by hand, headers included. `file(1)` identifies them
as a Linux ELF, a FreeBSD ELF, a `Mach-O 64-bit x86_64 executable` and a
`PE32+ executable (console) x86-64, for MS Windows`. Each checks its own
ABI's contract (argument vector, error convention, zero-filled `.bss`,
unknown-call behaviour) and exits non-zero at the first mismatch. The Linux
one also runs unmodified on a real Linux kernel, which pins the kernel's
behaviour to the real thing rather than to its own tests.

**Limits.** NT service numbers are not a stable ABI (Microsoft renumbers
them between builds), so the NT contract is "the numbers MergedOS's own
future `ntdll` uses"; the Win32 API itself must be reimplemented in user
space. macOS binaries need Mach traps, Mach IPC, `dyld` and `libSystem`
before anything beyond a static test program runs.

## Memory optimisation

**Measured**, booted to the shell prompt with all four personalities, the
package layer and the ramdisk linked in (QEMU, `make test-boot`):

| Machine RAM | Kernel resident | Of which frame bitmap |
|---|---|---|
| 24 MiB | 428 KiB | 4 KiB |
| 64 MiB | 428 KiB | 4 KiB |
| 256 MiB | 432 KiB | 8 KiB |
| 4 GiB | 552 KiB | 128 KiB |

Image breakdown: 38 KiB text, 41 KiB read-only data (20 KiB of it the
ramdisk), 149 KiB `.bss` (96 KiB package decode buffer, two 16 KiB stacks,
IDT), 36 KiB boot page tables and stack. A test program costs 17 to 19
pages while it runs, and the boot self-test checks that every frame comes
back when it exits.

That is the kernel with no drivers, no file system, no network stack and no
desktop. It shows the baseline is small; it does not show that a complete
system fits in 64 MiB. The techniques used, and the ones planned to keep the
full system inside that budget:

- **One implementation, many ABIs.** A personality is a table of thunks
  (16 bytes per entry) over shared native services, not a second kernel.
- **Metadata proportional to the machine.** The frame bitmap is sized from
  the firmware memory map: 1/32768 of RAM.
- **Zero-copy, zero-allocation parsers.** The ramdisk is read in place.
  Executable and package parsers work on caller-supplied buffers and never
  allocate; the gzip decoder needs about 2.5 KiB of stack, xz and zstd take
  a 28 / 9 KiB work area from the caller.
- **No kernel floating-point state.** Built with `-mgeneral-regs-only`, so
  kernel entry never saves or restores SSE/x87 registers.
- **Size-optimised, W^X.** `-Os`; user pages get exactly the permissions the
  image's own headers request.
- **Planned:** demand paging and page-cache sharing of runtime libraries
  across personalities; drivers and personalities as loadable modules, so an
  unused ABI costs nothing; a slab allocator with per-type caches; per-desktop
  memory budgets enforced by the selector (2 MiB to 128 MiB).

Target budget for the complete system on a 64 MiB machine, for comparison
with the measured numbers above: see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#memory-budget).

## Hardware-aware desktop selector

`hardware_probe/` produces a `struct hw_profile`; `de_selector/de_policy.c`
is a pure function from that profile to a recommendation.

| Profile | Style | Minimum | Comfortable | Compositor |
|---|---|---|---|---|
| `retro` | 640x480, 16 colours | 16 MiB, no GPU | 32 MiB | none, planar VGA |
| `tiling` | keyboard-driven tiles | 32 MiB, framebuffer | 64 MiB | none, direct framebuffer |
| `taskbar` | taskbar and start menu | 192 MiB, framebuffer | 768 MiB, 2 CPUs | stacking, software or GPU |
| `dock` | menu bar and dock | 1 GiB, accelerated GPU, 2 CPUs | 4 GiB, discrete GPU, 4 CPUs | GPU compositing |

For each profile that meets its minimum: `fit = 50% RAM headroom + 30% GPU
tier + 20% CPU count`, and `score = fit x weight`, where weight is the
richness of the desktop. A rich desktop wins only when the machine runs it
comfortably. Results on real boots: 24 MiB selects `retro`, 64 MiB selects
`tiling`, 4 GiB with 4 CPUs and virtio-gpu selects `dock`. The unit tests pin
14 machine configurations.

The boot menu shows the table with reasons, waits 5 seconds for a key (1 to
4, or Enter) on keyboard or serial, and accepts `de=` on the command line.
The desktops themselves are not implemented: selecting one draws a
text-mode mock-up of its layout and runs the shell inside it.

## Unified command line

`src/msh/` maps three vocabularies onto one set of builtins and one path
namespace:

| POSIX | CMD | PowerShell | |
|---|---|---|---|
| `ls` | `dir` | `Get-ChildItem`, `gci` | list a directory |
| `cat` | `type` | `Get-Content` | print a file |
| `ps` | `tasklist` | `Get-Process` | list tasks |
| `clear` | `cls` | `Clear-Host` | clear the screen |
| `which` | `where` | `Get-Command` | resolve a name |
| `open`, `xdg-open` | `start` | `Invoke-Item` | launch a bundle or package |
| `rm`, `cp`, `mv` ... | `del`, `copy`, `move` ... | `Remove-Item` ... | recognised; need the VFS |

`C:\bin\hello.exe`, `c:/bin/hello.exe` and `/bin/hello.exe` name the same
file (`C:` is the system volume; `D:\x` is `/vol/d/x`; `\\srv\share\x` is
`/net/srv/share/x`). Windows names match case-insensitively, POSIX names do
not, as on their home systems. Any program of any supported format starts
from the same prompt.

This is command *names and paths*. It is not bash or PowerShell: there is no
scripting language, no pipeline and no object model yet.

## Package-to-native translation

Full detail: [docs/PACKAGE_TRANSLATION.md](docs/PACKAGE_TRANSLATION.md).

Foreign packages are read in their own format and described by one native
manifest; nothing is converted or repacked.

```
msh:/> mpkg inspect /pkg/hello_1.0-1_amd64.deb
  format       deb (Debian family)
  package      hello 1.0-1
  arch         amd64 -> x86_64
  personality  linux (glibc)
  payload      gzip, 493 bytes
  scripts      maintainer scripts present (run jailed at install)
  store        /pkg/deb/hello/1.0-1
  requires     libc6 >= 2.34 -> runtime:linux-glibc [base]
  requires     libgcc-s1 -> runtime:linux-gcc [base]
  requires     zlib1g -> pkg:deb/zlib1g
```

| Format | Used by | Identify | Metadata | Extract and run program |
|---|---|---|---|---|
| `.deb` | Debian, Ubuntu, Mint | yes | yes | yes |
| `.rpm` | Fedora, RHEL, openSUSE | yes | yes | yes |
| `.apk` | Alpine | yes | yes | yes |
| `.pkg.tar.*` | Arch, Manjaro | yes | yes | yes |
| `.app` bundle | macOS | yes | yes (Info.plist, linked libraries) | launch in place |
| AppImage, Snap | cross-distro | yes | yes (.desktop, snap.yaml) | yes (from squashfs) |
| Flatpak bundle | cross-distro | yes | yes (ref, key file) | no |
| Nix NAR | Nix | yes | no | no |
| `.dmg`, `.pkg` (xar) | macOS | yes | no | no |

"Yes" for the four Linux formats holds when the relevant members are stored
or compressed with **gzip, xz or zstd** (the defaults of current Debian,
Ubuntu, Fedora and Arch packages); bzip2 is detected and reported
(`recognised, but it needs a codec ... not built in`) rather than
mis-parsing. `mpkg ls` lists a squashfs image, AppImage or snap. Checked against real tools: `dpkg-deb` and `rpm` accept the
test fixtures; the layer reads a real `rpmbuild` package and a real Ubuntu
package repacked with gzip, and the extracted programs are byte-identical
to the originals.

Two ideas carry the design. Dependencies are mapped from distro package
names to **capabilities** (`libc6`, `glibc` and
`libc.so.6(GLIBC_2.34)(64bit)` are all `runtime:linux-glibc`), so packages
from different distributions can share one runtime. And each package gets a
**path view**: `/usr/lib/x86_64-linux-gnu/libz.so.1` (Debian) and
`/usr/lib64/libz.so.1` (Fedora) both resolve to `lib/libz.so.1` inside the
package's store prefix.

"Every Linux distribution" means those four package families plus the
cross-distro formats; it does not mean every package runs. A package whose
programs are dynamically linked needs the Linux user-space runtime, which
is not built yet.

## Installation

MergedOS has no installer and no disk file system; it runs from a ramdisk.
"Installing" it means booting the kernel image.

**In an emulator** (recommended):

```bash
make run
```

**From a GRUB ISO**, in a VM or written to a USB stick:

```bash
make iso
```

```bash
qemu-system-x86_64 -m 64 -cdrom build/mergedos.iso
```

```bash
sudo dd if=build/mergedos.iso of=/dev/sdX bs=4M status=progress   # replace sdX; this erases the stick
```

**From an existing GRUB installation**, copy `build/mergedos.elf` to `/boot`
and add:

```
menuentry "MergedOS" {
    multiboot /boot/mergedos.elf
}
```

Real hardware needs legacy BIOS boot (or CSM), an x86-64 CPU, a VGA text
console or a serial port on COM1, and a PS/2-compatible keyboard. It has
been tested on QEMU only (TCG, 24 MiB to 4 GiB, 1 to 4 CPUs, GRUB and
direct kernel boot). There is no UEFI boot path and no USB keyboard driver.

## Licensing

Full analysis: [docs/LICENSING.md](docs/LICENSING.md).

All code in this repository is original and licensed under the
**BSD 2-Clause License** ([LICENSE](LICENSE)). It contains no code from
Linux, FreeBSD, XNU, Windows, ReactOS, Wine or Darling. Binary formats and
system-call conventions are implemented from public specifications.

The licence of a future full system depends on what is merged in:

| Source | Licence | Consequence |
|---|---|---|
| FreeBSD (network stack, jails) | BSD-2-Clause | Compatible; attribution kept |
| Linux drivers | GPL-2.0-only | A kernel image that links them is distributed under GPL-2.0. Plan: a clean-room driver-API shim, with GPL drivers as separately licensed modules |
| XNU | APSL-2.0 | Not GPL-compatible. No XNU code may be imported; interfaces only |
| Windows | proprietary | Clean-room only: contributors who have seen leaked Microsoft source cannot work on the NT personality |

"macOS-style", "Windows-style" and "TempleOS-style" describe layout
paradigms. MergedOS ships no third-party artwork, fonts or trademarks, and
running macOS software on non-Apple hardware may be restricted by that
software's own licence.

## Roadmap

See [docs/ROADMAP.md](docs/ROADMAP.md). In short: M1 (this release) boot,
exec and personalities; M2 scheduler, interrupts, SMP, signals; M3 VFS and
a user-space shell; M4 Linux and FreeBSD dynamic linking against real libcs;
M5 drivers and network; M6 package store and installer; M7 NT and Darwin
user-space runtimes; M8 graphics and the four desktops.
