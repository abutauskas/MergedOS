# MergedOS architecture

This document describes the design as a whole. Sections are marked
**[implemented]** when the code in this repository does what is described
and **[planned]** when it is design intent only.

## Shape of the system

```
          Linux ELF     FreeBSD ELF     Mach-O (macOS)     PE32+ (Windows)
              |              |                |                  |
   user  -----+--------------+----------------+------------------+-----  ring 3
              |   SYSCALL (same instruction, four register dialects)
   kernel     v
        +------------------------------------------------------------+
        | abi_dispatch: decode -> table lookup -> thunk -> encode    |
        |   linux | freebsd | xnu | nt personality tables            |
        +------------------------------------------------------------+
        | native services (msys_*): one implementation               |
        +----------------+---------------+---------------------------+
        | tasks, VM      | VFS [planned] | network stack [planned]   |
        +----------------+---------------+---------------------------+
        | driver layer [planned]: native drivers + Linux driver shim |
        +------------------------------------------------------------+
        | arch/x86_64: boot, paging, traps, SYSCALL entry            |
        +------------------------------------------------------------+
```

"Hybrid" means: mechanisms that must be fast and trusted (memory, tasks,
system-call translation) are in the kernel; policy and large compatibility
surfaces (Win32, Apple frameworks, C libraries, the desktops, the package
store) belong in user space.

## Boot sequence [implemented]

1. `boot.S`: Multiboot 1 entry in 32-bit protected mode. Verifies long
   mode, builds boot page tables (2 MiB pages: identity map, direct map of
   physical 0 to 4 GiB at `0xFFFF800000000000`, kernel at
   `0xFFFFFFFF80000000`), enables paging, jumps to the higher half.
2. `kmain()`: console, copy boot information out of loader memory, GDT/TSS/
   IDT and SYSCALL MSRs, frame allocator, drop the identity map, ramdisk.
3. Hardware probe, graphics mode, boot splash (logo and progress bar), disk
   probe, ABI personality self-check.
4. If a disk holds a saved setup: login (when password login is on), then
   the saved desktop layout. Otherwise the setup wizard; once it has saved
   the setup the machine resets and comes back up through this path.
5. The shell, inside the chosen layout.

## Memory management [implemented, minimal]

- **Physical**: one bit per 4 KiB frame, sized from the firmware memory map.
  Frames are zeroed on allocation. Limit: only RAM below 4 GiB is managed,
  because that is what the boot direct map covers.
- **Virtual**: each task owns PML4 slots 0 to 255 and shares 256 to 511 with
  the kernel. User pages are mapped with the permissions the executable's
  own headers ask for (NX unless executable, read-only unless writable).
- **User access**: system calls validate user pointers against the task's
  page tables before touching them; a bad pointer becomes `EFAULT` or
  `STATUS_ACCESS_VIOLATION`, never a kernel fault.

Planned: kernel heap (slab), demand paging, copy-on-write `fork`, a page
cache shared between personalities, swap.

## Interrupts, timer and threads [implemented]

- The legacy PIC is remapped to vectors 32 to 47. Only IRQ 0 is unmasked.
- PIT channel 0 ticks at 100 Hz. Channel 2 stays free for polled delays,
  which also gives the self-test an independent clock to check the tick.
- `kernel/sched.c`: up to six kernel threads, round-robin, one tick per
  turn. A switch pushes the callee-saved registers and swaps stack pointers.
  Preemption happens at the end of the timer interrupt; the interrupted
  context resumes through the normal IRET when its thread runs again.
- Sleeping threads are woken by tick count; an idle thread halts the CPU.
- Locking: one CPU, so code that must not interleave (console output, frame
  allocator) runs with interrupts off for its critical section.

## Tasks [implemented, minimal]

One user task at a time, started by whichever thread calls `task_exec()`.
User code runs with interrupts on, so it is preempted like anything else.
`user_enter()` saves the kernel context and `IRET`s to ring 3; an exit
system call, a fault, or the CPU budget calls `user_exit_resume()`, which
unwinds to the caller. A fault in ring 3 ends the task; a fault in ring 0
panics. A task that uses more than 10 s of user-mode CPU is ended with exit
status 124: a stand-in for signals, which do not exist yet.

Limits: one CPU. No APIC, so no SMP. System calls run with interrupts off.
Keyboard, mouse and disks are still polled. No priorities, no mutexes, no
user-level threads, and still only one user address space alive at a time.

## Unified kernel services [planned]

The original brief asks for Linux driver support, the FreeBSD network stack
and jails, and Windows subsystem APIs in one kernel. The intended approach
for each:

- **Linux drivers**: not by merging Linux. A driver shim that implements the
  subset of the Linux in-kernel driver API that real drivers use (the
  approach of FreeBSD's LinuxKPI), so selected drivers build against it as
  separately licensed GPL modules. See `docs/LICENSING.md`.
- **FreeBSD network stack**: port the stack (BSD-licensed) onto native
  primitives: mbufs over the slab allocator, native locks, native timers.
  Sockets are then one native service that the Linux, FreeBSD and XNU
  personalities map onto directly and Winsock maps onto in user space.
- **Jails**: every task carries a jail identity that scopes its file-system
  root, process table view, network addresses and permitted native
  services. Package maintainer scripts and untrusted packages run jailed.
  This is also the mechanism that gives a package its path view.
- **Windows APIs**: the kernel provides the NT system-call personality and
  the object model behind it (handles, sections, events). Win32 itself
  (kernel32, user32, gdi32 and so on) is user-space DLLs.

None of this exists in v0.1.0.

## Hardware probe [implemented]

`hardware_probe/` runs once at boot and fills `struct hw_profile`:

| Source | Facts |
|---|---|
| CPUID | vendor, brand, family/model, SSE2 to AVX2, AES, NX, 1 GiB pages, hypervisor |
| ACPI MADT | number of enabled logical processors (falls back to CPUID) |
| Firmware memory map | available RAM |
| PCI config space | function count, storage/network/USB counts, display controllers |
| Display controller | vendor/device, tier, largest memory BAR as a video-memory proxy |

GPU tiers, in increasing order: none, basic framebuffer, paravirtual,
integrated, discrete. Known limit: AMD integrated and discrete GPUs share a
vendor ID and are both classified as discrete; a device table would fix it.

## Graphics and pointer [implemented, module 1]

- Mode: the Multiboot header asks the loader for 1024x768x32; GRUB sets it
  through VBE. Under `qemu -kernel` (no loader video) the kernel programs the
  Bochs "dispi" interface itself. If neither works, VGA text mode stays.
- No back buffer and no allocation: primitives write to the framebuffer,
  which is reached through the boot direct map. Only the pixels under the
  pointer are saved.
- The text console keeps an 80x25 character grid and renders it either to
  VGA text memory or as scaled 5x7 glyphs; scrolling redraws only cells that
  changed.
- PS/2 mouse: polled from the i8042 aux port, standard 3-byte packets.
- Text is anti-aliased without floating point: the 5x7 dot patterns are
  treated as skeletons, touching dots are joined by round-ended strokes, and
  each pixel takes 3x3 samples. Coverage for the console size is cached
  (23 KiB); other sizes are computed per draw. Letter shapes are still
  those of a 5x7 font (no true descenders); only the edges are smooth.
- Rounded boxes and the logo hexagon use 4x4 samples on their edges.

Limits: no USB mice, no wheel, no acceleration, 24/32 bpp modes only, and a
framebuffer above 4 GiB is refused.

## UI toolkit and setup wizard [implemented, module 2]

`ui/` is a portable widget set: a screen is a fixed array of at most 16
widgets (label, button, text box, checkbox, list). Input arrives as events
(mouse down, mouse up, key); `ui_dispatch()` returns the id of the widget
the user acted on. Tab order is the order widgets were added. Drawing goes
through two functions the host supplies, so the same code runs on the
framebuffer and in unit tests. Only widgets whose state changed are redrawn.

`installer/` is the wizard state machine: language, name, password,
storage, Wi-Fi, desktop, summary. The Wi-Fi step is honest about there
being no wireless driver and offers only "Skip". The desktop step lists the
four layouts plus "recommended"; a note under the list shows the policy's
verdict for the highlighted one on this machine. "Recommended" is stored as
such and re-evaluated at every boot, so it follows hardware changes.

Password handling:

- On by default; one checkbox turns password login off.
- Minimum 8 characters, entered twice.
- Stored form: 16-byte salt plus PBKDF2-HMAC-SHA256, 100,000 iterations.
  The typed text is wiped from the text boxes as soon as it is hashed.
- Salt source: hash of the cycle counter at every input event, plus RDRAND
  where the CPU has it. Adequate for salts; not a general random generator.

Login (`installer/login.c`, loops in `kernel/setup.c`): when the saved
setup has password login on, the desktop does not start until
`login_check()` accepts the password: PBKDF2 with the stored salt and
iteration count, compared without early exit. The typed text is wiped after
every attempt and never written to the log. Each wrong attempt doubles the
wait (1, 2, 4, 8, 16 s). With `gfx=off` the prompt moves to the text
console and input is not echoed.

What the password does and does not protect: it gates this installed
system. It does not encrypt the disk, so booting other media bypasses it;
that needs disk encryption, which does not exist here. The `selftest`
command-line word skips login, runs the tests and powers the machine off
without ever starting a shell.

Text is UTF-8 throughout. `ui/text.c` decodes it (rejecting overlong forms,
surrogates and out-of-range values) and maps each code point to a base
glyph plus a diacritic: acute, grave, circumflex, tilde, diaeresis, ring,
cedilla. Capitals carry the mark above the cell; small letters carry it in
the cell's top two rows. Coverage is ASCII and Latin-1 letters, which is
Western European text; anything else draws as a box. Text boxes count and
delete whole characters, not bytes. The PS/2 keyboard map is US ASCII, so
accented letters can be shown and stored but not yet typed on the console.

## Storage and saved setup [implemented, module 3]

Layers: `struct block_dev` (`read_sectors` / `write_sectors`) is the only
interface drivers implement. `kernel/ahci.c` provides it for SATA disks
(one command slot, polled, a 4 KiB bounce buffer per disk) and
`kernel/ata.c` for legacy IDE ports by PIO. SATA disks are listed first, IDE
disks after them; an NVMe driver will fill the same structure.

`storage/disk.c` reads the layout: MBR primary entries, or GPT (header and
entry-array CRCs checked), and names what each partition holds from its
on-disk signature. A disk counts as blank only if its first MiB and its
last 34 sectors are all zero.

Writing is additive only. The rules, each enforced in code and tested:

- No function resizes, formats, moves or deletes a partition.
- The target is the largest gap between partitions, aligned to 1 MiB, of
  at least 16 MiB, on a disk with a free table entry, or a blank disk.
- Refused outright: damaged or overlapping tables, a GPT whose backup copy
  is missing, invalid or different from the primary, a file system with no
  partition table, and a gap whose first sectors carry a file-system
  signature.
- `disk_apply()` re-checks all of that itself, re-reads sector 0 and
  refuses if it changed since the scan.
- MBR: exactly two sectors are written: the first sector of the new
  partition, then sector 0 with one empty 16-byte entry filled in.
- GPT: five sectors: the data sector, then for the backup copy and after it
  the primary copy, the entry-array sector holding the chosen empty slot
  and the header with its two CRC-32 fields recomputed. Only an all-zero
  128-byte slot is filled; existing entries are carried through unchanged.
  Writing the backup first means a complete, valid table exists at every
  step: the old primary until the last two writes, the new backup after.
- Data goes first in both cases, and every write is read back and compared.

The saved setup is one sector (`installer/config.c`): magic, language,
name, salt, password hash, iteration count, CRC-32. On boot the kernel looks
for a type `0x7F` partition starting with that magic; if the CRC holds, the
wizard is skipped.

Verification: host tests compare MBR and GPT dual-boot images before and
after, byte for byte, and simulate a failure at each write.
`tools/disk-test.sh` does the comparison through the real drivers in QEMU
(MBR on IDE, GPT on AHCI), checks the new GPT with an independent CRC
implementation, and boots again to confirm the setup is found.

Limits: 512-byte sectors are assumed (4K-native disks are not handled).
NVMe and USB disks have no driver. Only the first AHCI controller is used.
AHCI registers are reached through a cacheable mapping, which is fine
under emulation and must be fixed before real hardware. The MergedOS
partition holds one config sector; there is no file system and the OS is
not installed to disk.

## Desktop selector [policy implemented, desktops planned]

`de_recommend()` is a pure function of the hardware profile:

```
eligible = ram >= min_ram and gpu_tier >= min_gpu and cpus >= min_cpus
ram_fit  = clamp((ram - min_ram) / (comfort_ram - min_ram))
gpu_fit  = 1.0 at ideal tier, 0.4 at minimum tier, linear between
cpu_fit  = 1.0 if cpus >= ideal_cpus else 0.5
fit      = 0.5 ram_fit + 0.3 gpu_fit + 0.2 cpu_fit
score    = weight * fit            (weights: retro 40, tiling 60, taskbar 80, dock 100)
```

Ties go to the lighter profile. If nothing is eligible, `retro` is the
fallback. All arithmetic is integer. The choice is not persisted yet (there
is no writable storage); it will live in `/etc/mergedos/desktop.conf`.

## Graphical desktop shell [implemented, module 5]

`desktop/desktop.c` (portable, no allocation, no kernel headers) is a small
window manager. Per layout it computes the work area, bar, dock, launcher
rectangles and a running-window strip, and it keeps up to six windows,
each with an id, kind, state (normal, minimised, maximised), frame and the
frame to restore after maximise. A z-order array lists them back to front;
the focused window is the topmost visible one. The API opens, closes,
focuses, minimises, maximises/restores and moves windows (moves are
clamped to the work area) and hit-tests a point to a launcher, a strip
button, or the topmost window and the part of it under the point (title,
close, minimise, maximise, content row, push button).

Window kinds:

- Terminal: exactly one, and it cannot be closed (its close button
  minimises it), so the shell always has a home. Its content is the text
  console, which the host repaints.
- System: hardware summary and memory figures.
- Files: the programs and text files on the ramdisk; clicking a line types
  `run <path>` or `cat <path>` into the shell and raises the terminal.
- Mergedfence: enabled state and counters, with an On/Off button that
  types `mergedfence on` or `off`.

The portable code draws all window content itself from lines of text the
host supplies through a callback; icons, controls and buttons are built
from rounded boxes and discs. Controls follow the layout: three discs on
the left (dock), minimise/maximise/close on the right (taskbar), a single
close box (retro), none in tiling, where windows are tiled instead (one
fills the work area, two split it side by side, three or more form a
master on the left and a stack on the right). Open windows show as
buttons in the taskbar, as dots under their launcher in the dock, and as
titles in the top bar for tiling and retro.

Every change records the screen areas it spoiled. `desktop_paint(clip)`
repaints one area back to front - wallpaper, bar, dock, then each visible
window that intersects it - and never draws outside it: fills are
clipped, rounded shapes and glyphs that straddle the edge are drawn pixel
by pixel through `ui_draw_blend`. There is no back buffer.

`kernel/deskrun.c` hosts it. A kernel thread polls the mouse every 20 ms.
Launchers for Terminal, Files, System and Mergedfence open (or focus) their
window; Files also types `ls /`; Packages, Help and Power type `ls /pkg`,
`help` and `poweroff` (`console_inject`). Pressing a title bar raises the
window and, while the button is held, a 2-pixel outline follows the
pointer (each step repaints only the four strips of the previous outline);
on release the window moves and its old and new frames are repainted.
Whenever any window overlaps the terminal, the console is muted (output
still reaches the text grid and serial) and the cells inside a damaged
area are repainted on demand (`console_fb_repaint`); once the terminal is
uncovered it is unmuted with one full repaint. The console also clips to
the terminal's content area (`console_fb_frame`), drawing only whole
cells. The status text is refreshed once a second, and the System and
Mergedfence windows when their numbers change. The kernel command-line
word `desk_demo` opens System and Files over the terminal at start.

Limits: six windows; no keyboard window management (keys always go to the
shell); no resizing other than maximise; the Files list does not scroll
and shows only what fits; a terminal tile narrower than the 80x25 console
(tiling with two or more windows on a 1024-pixel screen) shows only the
columns that fit; maximising the terminal does not enlarge the console. In
tiling there are no controls: clicking the launcher of the focused window
closes it. Repaints flicker slightly because they draw straight to the
framebuffer, and console output that lands under a drag outline can leave
a mark until the next repaint of that spot. No compositor, wallpapers or
settings.

## Unified shell [core implemented]

`src/msh/` is the portable core: alias table, tokenizer, path translation.
`kernel/monitor.c` is a temporary in-kernel front end. After M3 the same
core becomes a user-space `msh` process, and real `bash`, `zsh` and
PowerShell builds can run beside it as ordinary programs under their own
personalities.

## Memory budget

Measured for v0.1.0 (64 MiB QEMU machine): **428 KiB** kernel resident (plus 8 KiB per SATA disk).

Design targets for the complete system on a 64 MiB machine. These are
budgets to design against, not measurements:

| Component | Target |
|---|---|
| Kernel text and read-only data, all personalities | 2 MiB |
| Kernel dynamic data (slabs, page tables, task structures) | 6 MiB |
| Drivers for the hardware actually present | 4 MiB |
| Network stack, idle | 3 MiB |
| File-system and page cache floor | 8 MiB |
| Init, shell, one personality runtime (libc) | 8 MiB |
| Desktop: tiling profile | 8 MiB |
| **Baseline total** | **39 MiB**, leaving 25 MiB for applications |

The levers that make this plausible are listed in the README under
"Memory optimisation". The largest risk to the budget is the user-space
runtimes: a glibc process image or a Win32 DLL set is far larger than
anything in the kernel, which is why runtime pages must be shared through
the page cache rather than duplicated per personality.
