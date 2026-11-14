#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Boot the kernel headless in QEMU with the `selftest` command line and check
# that it reports success. Usage: tools/boot-test.sh <kernel.elf32> [MiB ...]
#
# The kernel signals the result twice: a marker line on the serial console
# and the isa-debug-exit device (QEMU exit status 33 = pass, 35 = fail).
set -u

KERNEL=${1:?usage: boot-test.sh <kernel.elf32> [memory sizes in MiB]}
shift
QEMU=${QEMU:-qemu-system-x86_64}
TIMEOUT=${TIMEOUT:-90}
[ $# -gt 0 ] || set -- "${QEMU_MEM:-64}"

# Optional sysroot module (glibc/musl runtimes) for the dynamic-linking checks.
SYSROOT=${SYSROOT:-}
mods=""
[ -n "$SYSROOT" ] && [ -s "$SYSROOT" ] && mods="$SYSROOT"
# The ntdll runtime and Wine's DLLs (when fetched): a second module.
[ -n "${BOOT_MODULE:-}" ] && [ -f "$BOOT_MODULE" ] && mods="${mods:+$mods,}$BOOT_MODULE"
mod=""
[ -n "$mods" ] && mod="-initrd $mods"

LOG=$(mktemp)
# Host-side HTTP server the guest reaches through QEMU's user networking
# (10.0.2.2 is the host). No external internet is needed.
WEB=$(mktemp -d)
echo "MERGEDOS-NET-OK" > "$WEB/index.html"
PORT=""
HTTPPID=""
if command -v python3 >/dev/null 2>&1; then
	PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
	(cd "$WEB" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
	HTTPPID=$!
	sleep 1
fi
trap 'rm -f "$LOG"; rm -rf "$WEB"; [ -n "$HTTPPID" ] && kill "$HTTPPID" 2>/dev/null' EXIT
status=0

for mem in "$@"; do
	timeout "$TIMEOUT" "$QEMU" -m "$mem" -no-reboot -display none -serial stdio \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-kernel "$KERNEL" $mod -append "selftest${PORT:+ nettest=$PORT}" >"$LOG" 2>&1
	rc=$?
	if [ "$rc" -eq 33 ] && grep -q '^MERGEDOS-SELFTEST: PASS' "$LOG"; then
		footprint=$(sed -n 's/.*kernel footprint: \([0-9]* KiB\).*/\1/p' "$LOG" | tail -n 1)
		desktop=$(sed -n 's/^ *selected: //p' "$LOG" | tail -n 1)
		echo "boot-test: ${mem} MiB  PASS  (kernel footprint ${footprint}; desktop: ${desktop})"
		grep '^\[net\]' "$LOG" | sed 's/^/boot-test:   /' 
	else
		echo "boot-test: ${mem} MiB  FAIL  (qemu exit ${rc}); last lines of the log:"
		tail -n 25 "$LOG"
		status=1
	fi
done
exit $status
