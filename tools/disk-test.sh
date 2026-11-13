#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# End-to-end install test on emulated disks. Two scenarios:
#   mbr-ide    BIOS-style dual boot (NTFS + ext4) on a legacy IDE disk
#   gpt-ahci   UEFI-style dual boot (ESP + NTFS + ext4, GPT) on a SATA disk
#              behind an AHCI controller
# For each one:
#   1. Build a 64 MiB image whose partitions are filled with random data.
#   2. Boot the kernel with `selftest selftest_install`: it must add a
#      MergedOS partition in the unallocated space and save a setup there.
#   3. Compare the image with the original byte for byte. Only the changes
#      the safety contract allows may differ; every existing partition, the
#      boot sector and every existing table entry must be identical.
#   4. Boot again: the saved setup must be found.
# Usage: tools/disk-test.sh <kernel.elf32>
set -u

KERNEL=${1:?usage: disk-test.sh <kernel.elf32>}
QEMU=${QEMU:-qemu-system-x86_64}
PYTHON=${PYTHON:-python3}
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

cat >"$DIR/img.py" <<'EOF'
import random, struct, sys, zlib
MIB, SEC = 1024 * 1024, 512
LAST = 64 * MIB // SEC - 1
BARRAY = LAST - 32                       # backup entry array

def mbr_entry(img, slot, boot, ptype, start, count):
    img[446 + slot * 16:462 + slot * 16] = struct.pack(
        "<B3sB3sII", boot, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff", start, count)

def gpt_header(img, self, other, table, crc):
    h = bytearray(92)
    struct.pack_into("<8sIIIIQQQQ16sQIII", h, 0, b"EFI PART", 0x10000, 92, 0, 0, self, other,
                     34, LAST - 33, b"\x5a" * 16, table, 128, 128, crc)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)))
    img[self * SEC:self * SEC + 92] = h

def make(kind, path):
    rnd = random.Random(1)
    img = bytearray(64 * MIB)
    if kind == "mbr":
        img[0:440] = rnd.randbytes(440)                         # boot code
        mbr_entry(img, 0, 0x80, 0x07, 1 * 2048, 20 * 2048)     # Windows
        mbr_entry(img, 1, 0x00, 0x83, 21 * 2048, 10 * 2048)    # Linux
        img[1 * MIB:31 * MIB] = rnd.randbytes(30 * MIB)
        img[1 * MIB + 3:1 * MIB + 11] = b"NTFS    "
        img[21 * MIB:21 * MIB + 4096] = bytes(4096)
        img[21 * MIB + 1080:21 * MIB + 1082] = b"\x53\xef"
    else:
        mbr_entry(img, 0, 0, 0xEE, 1, LAST)                     # protective MBR
        parts = [(1, 2), (3, 20), (23, 10)]                     # ESP, Windows, Linux (MiB)
        for i, (start, size) in enumerate(parts):
            struct.pack_into("<16s16sQQQ", img, 2 * SEC + i * 128, rnd.randbytes(16),
                             rnd.randbytes(16), start * 2048, (start + size) * 2048 - 1, 0)
        img[1 * MIB:33 * MIB] = rnd.randbytes(32 * MIB)
        img[1 * MIB + 0x52:1 * MIB + 0x5a] = b"FAT32   "
        img[3 * MIB + 3:3 * MIB + 11] = b"NTFS    "
        img[23 * MIB:23 * MIB + 4096] = bytes(4096)
        img[23 * MIB + 1080:23 * MIB + 1082] = b"\x53\xef"
        array = bytes(img[2 * SEC:34 * SEC])
        img[BARRAY * SEC:LAST * SEC] = array
        gpt_header(img, 1, LAST, 2, zlib.crc32(array))
        gpt_header(img, LAST, 1, BARRAY, zlib.crc32(array))
    img[510:512] = b"\x55\xaa"
    open(path, "wb").write(img)

def check(kind, orig, new):
    a, b = open(orig, "rb").read(), bytearray(open(new, "rb").read())
    if kind == "mbr":
        start = 31 * MIB
        allowed = [(446 + 32, 16), (start, SEC)]                # table entry 2, config sector
        boot, _, ptype, _, lba, count = struct.unpack("<B3sB3sII", b[446 + 32:446 + 48])
        assert (ptype, lba, count) == (0x7F, 31 * 2048, 33 * 2048), (ptype, lba, count)
        kept = "20 MiB NTFS + 10 MiB ext4"
    else:
        start = 33 * MIB
        allowed = [(start, SEC), (2 * SEC + 3 * 128, 128), (BARRAY * SEC + 3 * 128, 128),
                   (SEC + 16, 4), (SEC + 88, 4), (LAST * SEC + 16, 4), (LAST * SEC + 88, 4)]
        for hdr, table in ((1, 2), (LAST, BARRAY)):             # both copies valid and equal
            h = bytearray(b[hdr * SEC:hdr * SEC + 92])
            crc, ecrc = struct.unpack_from("<I", h, 16)[0], struct.unpack_from("<I", h, 88)[0]
            struct.pack_into("<I", h, 16, 0)
            assert zlib.crc32(bytes(h)) == crc, "header CRC wrong at LBA %d" % hdr
            assert zlib.crc32(bytes(b[table * SEC:(table + 32) * SEC])) == ecrc, "array CRC"
        assert b[2 * SEC:34 * SEC] == b[BARRAY * SEC:LAST * SEC], "arrays differ"
        first, lastlba = struct.unpack_from("<QQ", b, 2 * SEC + 3 * 128 + 32)
        assert (first, lastlba) == (33 * 2048, 63 * 2048 - 1), (first, lastlba)
        kept = "2 MiB ESP + 20 MiB NTFS + 10 MiB ext4"
    assert b[start:start + 8] == b"MRGDCFG1"
    changed = sum(x != y for x, y in zip(a, b))
    for off, n in allowed:                                      # mask what may change ...
        b[off:off + n] = a[off:off + n]
    assert a == bytes(b), "bytes outside the allowed ranges changed"   # ... nothing else did
    print("disk-test: %s: existing partitions byte-identical (%s); %d bytes changed in total"
          % (sys.argv[2], kept, changed))

if sys.argv[1] == "make":
    make(sys.argv[2][:3], sys.argv[3]); make(sys.argv[2][:3], sys.argv[4])
else:
    check(sys.argv[2][:3], sys.argv[3], sys.argv[4])
EOF

boot() {        # $1 scenario, $2 kernel command line, $3 log file
	case $1 in
	mbr-ide)  set -- "$@" -drive "file=$IMG,format=raw,if=ide" ;;
	gpt-ahci) set -- "$@" -device ahci,id=ahci -drive "id=d0,file=$IMG,format=raw,if=none" \
			-device ide-hd,drive=d0,bus=ahci.0 ;;
	esac
	cmdline=$2 log=$3
	shift 3
	timeout 120 "$QEMU" -m 64 -no-reboot -display none -serial stdio \
		-device isa-debug-exit,iobase=0xf4,iosize=0x04 "$@" \
		-kernel "$KERNEL" -append "$cmdline" >"$log" 2>&1
}

status=0
for scenario in mbr-ide gpt-ahci; do
	IMG=$DIR/$scenario.img
	$PYTHON "$DIR/img.py" make "$scenario" "$IMG" "$DIR/$scenario.orig" || exit 1

	boot "$scenario" "selftest selftest_install" "$DIR/boot1.log"
	rc=$?
	if [ "$rc" -ne 33 ] || ! grep -q 'install self-test: ok' "$DIR/boot1.log"; then
		echo "disk-test: $scenario: FAIL (install boot, qemu exit $rc)"
		grep -aE '^\[disk\]|^\[setup\]|FAIL|panic' "$DIR/boot1.log" | tail -n 12
		status=1
		continue
	fi
	grep -a '^\[disk\] [a-z]*[0-9]: [0-9]' "$DIR/boot1.log" | head -n 1 | sed "s/^/disk-test: $scenario: /"
	if ! $PYTHON "$DIR/img.py" check "$scenario" "$DIR/$scenario.orig" "$IMG"; then
		echo "disk-test: $scenario: FAIL (image comparison)"
		status=1
		continue
	fi
	boot "$scenario" "selftest" "$DIR/boot2.log"
	rc=$?
	if [ "$rc" -ne 33 ] || ! grep -q 'saved setup found' "$DIR/boot2.log"; then
		echo "disk-test: $scenario: FAIL (second boot did not find the saved setup, exit $rc)"
		grep -aE '^\[disk\]|^\[setup\]' "$DIR/boot2.log" | tail -n 8
		status=1
		continue
	fi
	echo "disk-test: $scenario: PASS (saved setup found on second boot)"
done
exit $status
