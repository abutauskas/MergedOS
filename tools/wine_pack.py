#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Pack Wine's Linux stack (unpacked .deb trees) into a ustar boot module.

usage: wine_pack.py OUT STRIP DIR...   (DIRs are `dpkg-deb -x` results)
Nothing is compiled or executed; files are copied, PE files stripped of debug
sections with STRIP (a mingw binutils strip, or "cp").
"""
import os
import re
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
    if os.environ.get("WINE_ALL"):             # every PE file: what wine.inf's registration steps load
        d = os.path.dirname(find(roots, f"{wd}x86_64-windows/ntdll.dll"))
        for f in sorted(os.listdir(d)):
            if f.endswith((".dll", ".exe", ".sys", ".drv", ".ocx", ".cpl", ".acm", ".ax", ".tlb", ".mui")):
                add(f"{wd}x86_64-windows/{f}", os.path.join(d, f), 0o644, True)
    # Pull in every DLL the packed ones import (objdump reads import tables; nothing is run).
    srcdir = os.path.dirname(find(roots, f"{wd}x86_64-windows/ntdll.dll"))
    todo = [n for n in tree if "x86_64-windows/" in n]
    while todo:
        cur = todo.pop()
        dump = subprocess.run(["x86_64-w64-mingw32-objdump", "-x", os.path.join(srcdir, os.path.basename(cur))],
                             capture_output=True, text=True).stdout
        deps = [l.split(":", 1)[1].strip().lower() for l in dump.splitlines() if "DLL Name:" in l]
        deps += [m + ".dll" for m in re.findall(r"__wine_delay_load_(\w+)", dump)]   # delay-load imports
        for dep in deps:
            if True:
                dst = f"{wd}x86_64-windows/{dep}"
                path = os.path.join(srcdir, dep)
                if dst not in tree and os.path.exists(path) and not dep.startswith("api-ms"):
                    add(dst, path, 0o644, True)
                    todo.append(dst)
    z = find(roots, "usr/x86_64-w64-mingw32/lib/zlib1.dll")   # user32 imports it (libz-mingw-w64)
    if z:
        add(f"{wd}x86_64-windows/zlib1.dll", z, 0o644)
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
