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

LOG=$(mktemp)
trap 'rm -f "$LOG"' EXIT
status=0

for mem in "$@"; do
	# Wine's DLLs (make wine), when present, go in as a Multiboot module.
	modarg=
	[ -n "${WINE_TAR:-}" ] && [ -f "$WINE_TAR" ] && modarg="-initrd $WINE_TAR"
	timeout "$TIMEOUT" "$QEMU" -m "$mem" -no-reboot -display none -serial stdio \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 \
		-kernel "$KERNEL" $modarg -append "selftest" >"$LOG" 2>&1
	rc=$?
	if [ "$rc" -eq 33 ] && grep -q '^MERGEDOS-SELFTEST: PASS' "$LOG"; then
		footprint=$(sed -n 's/.*kernel footprint: \([0-9]* KiB\).*/\1/p' "$LOG" | tail -n 1)
		desktop=$(sed -n 's/^ *selected: //p' "$LOG" | tail -n 1)
		echo "boot-test: ${mem} MiB  PASS  (kernel footprint ${footprint}; desktop: ${desktop})"
	else
		echo "boot-test: ${mem} MiB  FAIL  (qemu exit ${rc}); last lines of the log:"
		tail -n 25 "$LOG"
		status=1
	fi
done
exit $status
