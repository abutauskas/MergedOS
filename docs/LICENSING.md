# Licensing

This is an engineering summary, not legal advice.

## This repository

Every file is original work under the **BSD 2-Clause License** and carries
an SPDX identifier. No source was copied from Linux, FreeBSD, XNU/Darwin,
Windows, ReactOS, Wine or Darling. File formats (ELF, PE/COFF, Mach-O, ar,
tar, cpio, gzip, RPM, Debian control) and calling conventions are
implemented from their public specifications; system-call numbers and error
values are interface facts.

BSD-2-Clause was chosen because it can be combined with every other licence
the project expects to meet, including the GPL.

## What "merging" other systems means for licensing

| Component | Licence | Can its code be imported? | Effect |
|---|---|---|---|
| FreeBSD kernel code | BSD-2-Clause | Yes | Keep copyright notices. No effect on the rest |
| Linux kernel code, including drivers | GPL-2.0-only | Yes, but | Any kernel image that links it is a derived work and must be distributed under GPL-2.0 with full source. BSD-licensed files stay BSD individually; the combination is GPL |
| XNU | APSL-2.0 | No | APSL is not GPL-compatible, so XNU code could never share an image with Linux-derived code. Treat as interface reference only |
| Apple frameworks, `libSystem` binaries | proprietary | No | Cannot be redistributed. Free reimplementations only |
| Windows | proprietary | No | Clean-room only |
| Wine | LGPL-2.1+ | As user-space DLLs | Fine as separately linked libraries |
| Darling | GPL-3.0 | User space only | GPL-3.0 is not compatible with GPL-2.0-only kernel code; keep it out of the kernel |
| musl | MIT | Yes | |
| glibc | LGPL-2.1+ | As a shared library | |
| xz (liblzma), zstd | 0BSD / BSD-3-Clause | Yes | Intended codec sources |

## Plan for Linux driver support

To keep the core kernel BSD-licensed while still using Linux drivers:

1. Write a driver-API shim (the subset of Linux's in-kernel API that drivers
   call) from scratch under BSD-2-Clause, as FreeBSD did with LinuxKPI.
2. Build Linux drivers against it as loadable modules, each distributed
   under its own licence (GPL-2.0, or MIT for dual-licensed drivers such as
   many DRM drivers), with source.
3. Ship them as separate files, not linked into the kernel image.

Whether a module built against a compatibility shim is a derived work of
Linux is a contested legal question. A distribution that wants no ambiguity
should ship the GPL-driver configuration entirely under GPL-2.0, which
BSD-2-Clause permits.

## Clean-room rule for the NT personality

Contributors who have read leaked or licensed Microsoft source code must
not contribute to the NT personality, the PE loader or any Win32
reimplementation. Use public documentation, published headers from
permissively licensed projects, and black-box testing. This is the same
rule ReactOS and Wine apply.

## Names, look and feel

"Windows-style", "macOS-style" and "TempleOS-style" describe layout
paradigms (a taskbar, a dock, a 16-colour screen). MergedOS must not ship
other systems' artwork, icons, fonts, sounds or logos, and must not use
their trademarks in a way that suggests endorsement.

## Running other vendors' software

Being technically able to run a program does not grant a licence to it.
Much macOS software, and macOS itself, is licensed for use on Apple
hardware only. Users are responsible for the licences of what they run.

## Contributions

Contributions are accepted under BSD-2-Clause. Each new file needs an SPDX
identifier. Do not submit code derived from a source whose licence is not
compatible with BSD-2-Clause without discussing where it will live.
