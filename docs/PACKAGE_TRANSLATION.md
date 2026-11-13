# Package-to-native runtime translation layer

`pkg_translate/` lets MergedOS consume software in the packaging formats it
is already distributed in: Linux distribution packages and macOS `.app`
bundles.

## Model

A foreign package is never converted to another format. It is read in
place and described by a native manifest:

```c
struct pkg_manifest {
    enum pkg_format format;           /* deb, rpm, apk, pacman, app ...     */
    char name[64], version[48];
    char arch[16], foreign_arch[16];  /* x86_64  <-  amd64                  */
    enum abi_personality personality; /* which syscall dialect its code uses */
    char libc[12];                    /* glibc, musl, libSystem             */
    struct pkg_dep deps[16];          /* name, constraint, capability, base */
    enum pkg_codec meta_codec, payload_codec;
    u64 payload_off, payload_size;
    u32 flags;                        /* scripts, payload readable, relocatable */
    char store_prefix[160];           /* /pkg/<family>/<name>/<version>     */
    ...
};
```

The manifest answers the three questions a launcher has: which personality
does this code need, what else must be present, and where do its files live.

## Pipeline

```
bytes --pkg_probe()--> format
      --container reader--> metadata member  --codec--> text / header
      --metadata reader--> manifest fields, raw dependencies
      --depmap--> capabilities
      --pkg_payload_exec() / pkg_store_path()--> program bytes, native paths
```

| Format | Container | Metadata | Payload |
|---|---|---|---|
| `.deb` | `ar`: `debian-binary`, `control.tar.*`, `data.tar.*` | `control` (RFC 822 style) | tar |
| `.rpm` | lead, signature header, header, payload | binary tag index (never compressed) | cpio "newc" |
| `.apk` | three concatenated gzip streams | `.PKGINFO` (`key = value`) | tar |
| pacman | one compressed tar | `.PKGINFO` | same tar |
| `.app` | a directory | `Contents/Info.plist` (XML) | used in place |

Identification is by content first (magic numbers, the first `ar` member,
the first tar entry name) and by file name only to break ties, for example
between an xz-compressed pacman package and any other xz file.

Also identified, with no translator backend yet: AppImage (ELF with `AI`
marker), Snap (squashfs), Flatpak bundles (by name), Nix archives, macOS
`.dmg` (UDIF trailer) and `.pkg` (xar).

## Codecs

| Codec | Status |
|---|---|
| stored | built in |
| gzip | built in: `inflate.c`, about 300 lines, 2.5 KiB of stack, CRC-32 verified |
| xz, zstd, bzip2 | detected and reported; not built in |

This matters in practice. Current defaults are xz (Debian), zstd (Ubuntu,
Fedora, Arch) and gzip (Alpine). So today the layer fully reads Alpine
packages, RPM *metadata* for any RPM, and `.deb`/RPM/pacman packages built
with gzip. Adding xz and zstd decoders (both have permissively licensed
reference implementations) is roadmap item M6 and is the single change that
turns "format support" into "reads what the distributions ship".

## Dependency translation

Distributions name the same thing differently. `depmap.c` maps names to
capabilities:

| As written | Family | Capability |
|---|---|---|
| `libc6 (>= 2.34)` | deb | `runtime:linux-glibc` |
| `glibc >= 2.34` | rpm, pacman | `runtime:linux-glibc` |
| `libc.so.6(GLIBC_2.34)(64bit)` | rpm (automatic) | `runtime:linux-glibc` |
| `so:libc.musl-x86_64.so.1` | apk | `runtime:linux-musl` |
| `/bin/sh`, `bash`, `busybox` | any | `runtime:posix-sh` |
| `so:libz.so.1`, `libz.so.1()(64bit)` | apk, rpm | `soname:libz.so.1` |
| `zlib1g` | deb | `pkg:deb/zlib1g` |
| `/usr/lib/libSystem.B.dylib` | Mach-O | `runtime:darwin-libsystem` |
| `/System/Library/Frameworks/AppKit.framework/...` | Mach-O | `framework:AppKit` |
| `@rpath/libfoo.dylib` | Mach-O | `bundle:libfoo.dylib` |

`runtime:*` capabilities are meant to be satisfied by MergedOS base
runtimes, one per C library, shared by every package that needs them.
`soname:*` and `pkg:*` are resolved by the package database at install
time. RPM's `rpmlib(...)` pseudo-dependencies are dropped. Debian
alternatives (`a | b`) currently keep only the first.

The mapping table is small and hand-written; a real system needs it
generated from distribution metadata.

## Path view

Distributions also disagree about where files go. Each package is stored
under its own prefix and sees the paths it expects:

| Path the package refers to | Store path |
|---|---|
| `/usr/lib/x86_64-linux-gnu/libz.so.1` (Debian multiarch) | `<prefix>/lib/libz.so.1` |
| `/usr/lib64/libz.so.1` (Fedora) | `<prefix>/lib/libz.so.1` |
| `/usr/lib/libz.so.1` (Arch, Alpine) | `<prefix>/lib/libz.so.1` |
| `/usr/bin/x`, `/bin/x`, `/usr/sbin/x` | `<prefix>/bin/x` |
| `/usr/share/...`, `/etc/...` | `<prefix>/share/...`, `<prefix>/etc/...` |
| anything else | `<prefix>/root/<path>` |

`pkg_store_path()` implements the mapping. Enforcing it at run time (so
that a process from a Debian package that opens
`/usr/lib/x86_64-linux-gnu/libz.so.1` gets the store file) requires the VFS
and per-jail namespaces, which are not built yet.

## macOS `.app` bundles

A bundle needs no unpacking, so translation is:

1. Read `Contents/Info.plist`: `CFBundleExecutable`, `CFBundleIdentifier`,
   `CFBundleName`, `CFBundleShortVersionString`, `LSMinimumSystemVersion`.
   The executable name may not contain `/`.
2. Check that `Contents/MacOS/<executable>` is a Mach-O and read its
   `LC_LOAD_DYLIB` commands for dependencies.
3. Launch it with the XNU personality.

`open /Applications/Hello.app` does exactly this in the PoC. Limits: XML
property lists only (binary plists are detected and refused); the bundled
test program is static, because `dyld` and the frameworks do not exist
here. An `.app` that links AppKit is correctly reported as needing
`framework:AppKit` and will not run.

## Using it

In the kernel shell:

```
mpkg inspect /pkg/hello-1.0-r0.apk
mpkg run /pkg/hello_1.0-1_amd64.deb
open /Applications/Hello.app
```

On the host, against real files:

```bash
make tools
```

```bash
build/host/mpkg inspect path/to/package.rpm
```

```bash
build/host/mpkg extract path/to/package.deb ./program
```

## Mergedfence and .merged

`.merged` is the native package format. All integers are little-endian:

| Offset | Field |
|---|---|
| 0 | magic `MRGD` |
| 4 | u32 format version (1) |
| 8 | u32 manifest length (1..4096) |
| 12 | u32 payload length (up to 16 MiB) |
| 16 | manifest: UTF-8 `key: value` lines; `name`, `version`, `type` (`app` or `blocklist`) required, `summary` optional, unknown keys ignored |
| ... | payload bytes |
| end - 32 | SHA-256 over every byte before it |

`merged_parse()` (`pkg_translate/merged.c`) rejects truncated or padded
files, oversized sections, a hash mismatch, missing or duplicate keys,
unknown types, and names or versions outside `[A-Za-z0-9._+-]` (they become
store paths). `pkg_probe()` reports it as `PKG_MERGED`, so `mpkg inspect`
works on it on the host and in the shell; it gets the native personality and
the store prefix `/pkg/merged/<name>/<version>`. The SHA-256 trailer detects
corruption only: it is not a signature.

Mergedfence (`mergedfence/fence.c`) is a domain blocking engine. It reads
hosts-file lines (`0.0.0.0 name`, `127.0.0.1 name`) and adblock-style
`||name^` rules with `@@||name^` exceptions; `#` and `!` start comments, and
every other line (cosmetic rules, rules with options or paths) is counted as
ignored. A rule covers its name and every name below it, on label
boundaries only (`example.com` covers `ads.example.com`, not
`badexample.com`); matching ignores case and exceptions always win. Rules
live in a fixed table of 512 entries of up to 63 characters, with no
allocation.

The ramdisk ships `/etc/mergedfence/default.merged` (type `blocklist`), a
sample list built by `tools/mkimage.py` from fictional names under the
reserved `.example` and `.invalid` TLDs. The shell command
`mergedfence status | check <name> | on | off` (alias `fence`, also a
desktop launcher) verifies and loads it on first use.

Limits, stated plainly: MergedOS has no network stack yet, so nothing is
blocked on the wire. `mergedfence check` only answers what would happen;
the engine is written to sit in the DNS resolver path once there is one.
Blocking third-party cookies is not something a DNS-level blocker can do:
that can only happen inside a browser.

## Safety properties

Packages are untrusted input. The parsers never allocate, bounds-check
every offset against the buffer, verify tar header checksums and gzip
CRC-32, and return an error rather than partial data when a buffer is too
small. The unit tests run every truncation and a bit flip at every byte of
each fixture through the full pipeline under AddressSanitizer and UBSan.

Not yet addressed: signature verification (packages are not authenticated),
and maintainer scripts (detected and flagged; they must run jailed).
