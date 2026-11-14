#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Pack Wine's Linux stack (unpacked .deb trees) into a ustar boot module.

usage: wine_pack.py OUT STRIP DIR...   (DIRs are `dpkg-deb -x` results)
Nothing is compiled or executed; files are copied, PE files stripped of debug
sections with STRIP (a mingw binutils strip, or "cp").
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkimage import ustar  # noqa: E402

PE = """apisetschema ntdll kernel32 kernelbase win32u msvcrt ucrtbase user32 gdi32 advapi32 sechost
rpcrt4 shlwapi shell32 ole32 combase imm32 version bcrypt ws2_32 wineboot conhost services
cmd plugplay winedevice explorer rundll32 regedit start winemenubuilder
setupapi cfgmgr32 wineusb winebus mountmgr msi wtsapi32 userenv nsi netapi32 iphlpapi
secur32 crypt32 wintrust dbghelp apphelp oleaut32 uxtheme wldap32 mpr oleacc
authz mswsock dnsapi winspool""".split()
UNIX = "ntdll win32u ws2_32 bcrypt mountmgr nsiproxy netapi32 secur32 crypt32 dnsapi".split()
LIBS = ["libunwind.so.8", "liblzma.so.5", "libgcc_s.so.1"]


def find(roots, rel):
    for r in roots:
        p = os.path.join(r, rel)
        if os.path.exists(p):
            return os.path.realpath(p)
    return None


def main():
    out, strip, roots = sys.argv[1], sys.argv[2], sys.argv[3:]
    tree = {}

    def add(dst, src, mode=0o755, do_strip=False):
        if do_strip and strip != "cp":
            tmp = tempfile.NamedTemporaryFile(delete=False)
            tmp.close()
            subprocess.check_call([strip, "--strip-debug", "-o", tmp.name, src])
            data = open(tmp.name, "rb").read()
            os.unlink(tmp.name)
        else:
            data = open(src, "rb").read()
        tree[dst] = (data, mode)

    for rel in ("usr/lib/wine/wine64", "usr/lib/wine/wineserver64"):
        p = find(roots, rel)
        if not p:
            sys.exit(f"wine_pack: {rel} not found")
        add(rel, p)
    tree["usr/lib/wine/wineserver"] = tree["usr/lib/wine/wineserver64"]   # where the loader looks
    wd = "usr/lib/x86_64-linux-gnu/wine/"
    for n in UNIX:
        p = find(roots, f"{wd}x86_64-unix/{n}.so")
        if not p:
            sys.exit(f"wine_pack: {n}.so not found")
        add(f"{wd}x86_64-unix/{n}.so", p)
    for n in PE:
        for ext in ("dll", "exe", "sys", "drv"):
            p = find(roots, f"{wd}x86_64-windows/{n}.{ext}")
            if p:
                add(f"{wd}x86_64-windows/{n}.{ext}", p, 0o644, True)
                break
    nls = find(roots, "usr/share/wine/nls")
    for f in sorted(os.listdir(nls)):
        add(f"usr/share/wine/nls/{f}", os.path.join(nls, f), 0o644)
    add("usr/share/wine/wine.inf", find(roots, "usr/share/wine/wine.inf"), 0o644)
    for n in LIBS:
        for d in ("lib/x86_64-linux-gnu", "usr/lib/x86_64-linux-gnu"):
            p = find(roots, f"{d}/{n}")
            if p:
                add(f"lib/x86_64-linux-gnu/{n}", p)
                break
    members, dirs = [], set()
    for path in sorted(tree):
        parts = path.split("/")
        for i in range(1, len(parts)):
            d = "/".join(parts[:i]) + "/"
            if d not in dirs:
                dirs.add(d)
                members.append((d, b"", 0o755))
        members.append((path, *tree[path]))
    open(out, "wb").write(ustar(members))
    print(f"wine_pack: {out}: {len(tree)} files, {os.path.getsize(out) // 1024} KiB")


main()
