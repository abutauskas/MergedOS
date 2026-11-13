# SPDX-License-Identifier: BSD-2-Clause
#
# MergedOS build system
#
#   make              build the kernel            (build/mergedos.elf, .elf32)
#   make run          boot it in QEMU             (VGA window + serial on stdio)
#   make run-serial   boot it headless            (serial console only)
#   make test         host unit tests + QEMU boot self-test
#   make iso          GRUB-bootable ISO image     (needs grub-mkrescue, xorriso)
#   make tools        host tools                  (build/host/mpkg)
#   make clean
#
# Useful variables:
#   CROSS=x86_64-elf-   cross toolchain prefix (not needed on x86-64 Linux)
#   QEMU_MEM=64         guest RAM in MiB for run / test-boot
#   APPEND="de=tiling"  kernel command line for run targets

.DEFAULT_GOAL := all

BUILD    ?= build
CROSS    ?=
CC       := $(CROSS)gcc
LD       := $(CROSS)ld
OBJCOPY  := $(CROSS)objcopy
HOSTCC   ?= cc
PYTHON   ?= python3
QEMU     ?= qemu-system-x86_64
QEMU_MEM ?= 64
APPEND   ?=
VERSION  := 0.1.0

KERNEL_ELF   := $(BUILD)/mergedos.elf
KERNEL_ELF32 := $(BUILD)/mergedos.elf32
INITRD       := $(BUILD)/initrd.tar
NTRT_BIN     := $(BUILD)/ntrt/ntrt.bin
ISO          := $(BUILD)/mergedos.iso

# ---- sources -----------------------------------------------------------------

# Portable code: linked into the kernel *and* into host tests/tools.
SHARED_C := \
	abi_dispatch/binfmt.c \
	abi_dispatch/dispatch.c \
	abi_dispatch/errno_map.c \
	abi_dispatch/identity.c \
	abi_dispatch/personality_linux.c \
	abi_dispatch/personality_freebsd.c \
	abi_dispatch/personality_xnu.c \
	abi_dispatch/personality_nt.c \
	kernel/ramfs.c \
	hardware_probe/hw_classify.c \
	de_selector/de_policy.c \
	pkg_translate/tar.c \
	pkg_translate/inflate.c \
	pkg_translate/xz.c \
	pkg_translate/zstd.c \
	pkg_translate/squashfs.c \
	pkg_translate/bundles.c \
	pkg_translate/containers.c \
	pkg_translate/pkgfmt.c \
	pkg_translate/metadata.c \
	pkg_translate/appbundle.c \
	pkg_translate/depmap.c \
	pkg_translate/translate.c \
	pkg_translate/merged.c \
	installer/sha256.c \
	mergedfence/fence.c \
	src/msh/cmdmap.c \
	src/msh/pathmap.c \
	filetype/identify.c \
	filetype/view.c \
	filetype/image.c

# Portable UI toolkit, disk layout code and setup wizard: kernel and unit tests.
UI_C := \
	desktop/desktop.c \
	ui/widgets.c \
	ui/aa.c \
	kernel/font5x7.c \
	ui/text.c \
	storage/disk.c \
	installer/config.c \
	installer/login.c \
	installer/wizard.c \
	installer/i18n.c \
	installer/selfcheck.c

# Kernel-only code.
KERNEL_C := \
	kernel/setup.c \
	kernel/deskrun.c \
	kernel/ata.c \
	kernel/ahci.c \
	kernel/kmain.c \
	kernel/console.c \
	kernel/printk.c \
	kernel/string.c \
	kernel/bootinfo.c \
	kernel/pmm.c \
	kernel/vmm.c \
	kernel/time.c \
	kernel/gfx.c \
	kernel/mouse.c \
	kernel/task.c \
	kernel/proc.c \
	kernel/vfs.c \
	kernel/kmalloc.c \
	kernel/ntload.c \
	kernel/sched.c \
	kernel/syscall.c \
	kernel/initrd.c \
	kernel/monitor.c \
	kernel/arch/x86_64/cpu.c \
	kernel/arch/x86_64/traps.c \
	hardware_probe/hwprobe.c \
	hardware_probe/cpuid.c \
	hardware_probe/pci.c \
	hardware_probe/acpi.c \
	de_selector/de_menu.c \
	de_selector/de_session.c

KERNEL_S := \
	kernel/arch/x86_64/boot.S \
	kernel/arch/x86_64/entry.S \
	kernel/initrd_blob.S \
	kernel/ntrt_blob.S

TEST_C := tests/test_main.c tests/test_abi.c tests/test_pe.c tests/test_de_msh.c tests/test_pkg.c tests/test_codec.c tests/test_ui.c tests/test_storage.c tests/test_desktop.c tests/test_fence.c tests/test_filetype.c tests/test_ramfs.c

KOBJS := $(patsubst %,$(BUILD)/obj/%.o,$(basename $(KERNEL_S) $(KERNEL_C) $(SHARED_C) $(UI_C)))
KDEPS := $(KOBJS:.o=.d)

# ---- flags -------------------------------------------------------------------

INCLUDES := -Ikernel/include -I.
WARNINGS := -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wshadow

# -Os + no SSE/x87 in the kernel: small image, and no FPU state to save on
# kernel entry. -mno-red-zone because exceptions arrive on the current stack.
KCFLAGS := -std=gnu11 -Os -g $(WARNINGS) \
	-ffreestanding -fno-stack-protector -fno-pic -fno-pie -fcf-protection=none \
	-mno-red-zone -mcmodel=kernel -mgeneral-regs-only \
	-fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
	-DMERGEDOS_VERSION=\"$(VERSION)\" $(INCLUDES)
KASFLAGS := $(INCLUDES) -DINITRD_FILE=\"$(INITRD)\" -DNTRT_FILE=\"$(NTRT_BIN)\"
KLDFLAGS := -n -nostdlib -z max-page-size=0x1000 -z noexecstack \
	-T kernel/arch/x86_64/linker.ld \
	$(shell $(LD) --help 2>/dev/null | grep -q no-warn-rwx-segments && echo --no-warn-rwx-segments)

HOST_CFLAGS := -std=gnu11 -O1 -g $(WARNINGS) -DMERGEDOS_HOSTED \
	-DMERGEDOS_VERSION=\"$(VERSION)\" $(INCLUDES)
HOST_SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined

# ---- kernel ------------------------------------------------------------------

.PHONY: all
all: $(KERNEL_ELF32)

$(BUILD)/obj/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(KCFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(KASFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/kernel/initrd_blob.o: $(INITRD)
$(BUILD)/obj/kernel/ntrt_blob.o: $(NTRT_BIN)

# ---- built-in Win32 runtime ---------------------------------------------------
#
# User-mode code (Microsoft x64 calling convention) linked at a fixed address
# and embedded in the kernel as a flat binary; see src/ntrt/ntrt_abi.h.

NTRT_SRC  := src/ntrt/start.S src/ntrt/kernel32.c src/ntrt/crt.c src/ntrt/ldr.c src/ntrt/ntdll.c src/ntrt/rtl.c src/ntrt/ntsync.c src/ntrt/ntreg.c src/ntrt/ntexc.c src/ntrt/user.c
NTRT_OBJS := $(patsubst src/ntrt/%,$(BUILD)/ntrt/%.o,$(basename $(NTRT_SRC)))
NTRT_CFLAGS := -std=gnu11 -Os $(WARNINGS) -ffreestanding -fno-builtin -fshort-wchar \
	-fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -mcmodel=small \
	-fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns -I.

$(BUILD)/ntrt/%.o: src/ntrt/%.c src/ntrt/rt.h src/ntrt/nt.h src/ntrt/ntrt_abi.h
	@mkdir -p $(dir $@)
	$(CC) $(NTRT_CFLAGS) -c $< -o $@

$(BUILD)/ntrt/%.o: src/ntrt/%.S
	@mkdir -p $(dir $@)
	$(CC) -c $< -o $@

$(NTRT_BIN): $(NTRT_OBJS) src/ntrt/ntrt.ld
	$(LD) -nostdlib -static -z noexecstack -T src/ntrt/ntrt.ld -o $(BUILD)/ntrt/ntrt.elf $(NTRT_OBJS)
	$(OBJCOPY) -O binary $(BUILD)/ntrt/ntrt.elf $@

$(KERNEL_ELF): $(KOBJS) kernel/arch/x86_64/linker.ld
	$(LD) $(KLDFLAGS) -o $@ $(KOBJS)

# QEMU's -kernel Multiboot loader only accepts 32-bit ELF containers. The code
# inside is unchanged; GRUB can boot either file.
$(KERNEL_ELF32): $(KERNEL_ELF)
	$(OBJCOPY) -O elf32-i386 $< $@
	@size=$$(stat -c %s $@ 2>/dev/null || stat -f %z $@); \
	 echo "built $@ ($$((size / 1024)) KiB on disk)"

# ---- guest programs and boot ramdisk -----------------------------------------
#
# Each guest is one assembly file that spells out its own executable headers
# (ELF, PE32+, Mach-O), so "assemble + objcopy -O binary" yields the final file.

GUEST_BINS := $(addprefix $(BUILD)/guest/,hello.linux hello.freebsd hello.macho hello.exe spin.linux)

define GUEST_RULE
$(BUILD)/guest/$(1): src/guest/$(2)
	@mkdir -p $$(dir $$@)
	$(CC) -c $$< -o $$@.o
	$(OBJCOPY) -O binary -j .text $$@.o $$@
	@chmod +x $$@
endef
# Processes, threads, signals, ramfs: freestanding C on raw Linux system calls,
# linked by the host toolchain into an ordinary static ELF executable.
$(BUILD)/guest/proctest.linux: src/guest/proctest.c
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 -Os -static -nostdlib -ffreestanding -fno-pic -no-pie \
		-fno-stack-protector -fcf-protection=none -fno-asynchronous-unwind-tables \
		-fno-builtin -s -Wl,--build-id=none -Wl,-z,noexecstack -Wl,-z,noseparate-code -o $@ $<
GUEST_BINS += $(BUILD)/guest/proctest.linux

$(eval $(call GUEST_RULE,hello.linux,hello_linux.S))
$(eval $(call GUEST_RULE,hello.freebsd,hello_freebsd.S))
$(eval $(call GUEST_RULE,hello.macho,hello_macho.S))
$(eval $(call GUEST_RULE,hello.exe,hello_pe.S))
$(eval $(call GUEST_RULE,spin.linux,spin_linux.S))

# A real C program against the unmodified musl libc, when musl-gcc is installed.
# Without it an empty file is produced and the kernel reports the check as skipped.
MUSL_CC ?= musl-gcc
$(BUILD)/guest/demo.musl: src/guest/musl_demo.c
	@mkdir -p $(dir $@)
	@if command -v $(MUSL_CC) >/dev/null 2>&1; then \
		$(MUSL_CC) -static -no-pie -O2 -s -o $@ $<; \
	else echo "note: $(MUSL_CC) not found, bin/demo.musl left out"; : > $@; fi
GUEST_BINS += $(BUILD)/guest/demo.musl

# A real Windows x64 console program against the MinGW-w64 C runtime, when the
# cross compiler is installed. Without it an empty file is produced and the
# kernel reports the check as skipped.
MINGW_CC ?= x86_64-w64-mingw32-gcc
$(BUILD)/guest/win_hello.exe: src/guest/win_hello.c
	@mkdir -p $(dir $@)
	@if command -v $(MINGW_CC) >/dev/null 2>&1; then \
		$(MINGW_CC) -O2 -s -Wall -o $@ $<; \
	else echo "note: $(MINGW_CC) not found, bin/win_hello.exe left out"; : > $@; fi
GUEST_BINS += $(BUILD)/guest/win_hello.exe

# Optional real third-party Windows programs, e.g. Wine's x86_64-windows
# builtins: `make WINE_EXE_DIR=.../wine/x86_64-windows` copies them, unmodified,
# into the ramdisk as bin/wine/*.exe. Not part of the self-test.
WINE_EXE_DIR ?=
WINE_EXES := $(if $(WINE_EXE_DIR),$(addprefix $(BUILD)/guest/wine/,hostname.exe whoami.exe))
$(BUILD)/guest/wine/%.exe: $(WINE_EXE_DIR)/%.exe $(BUILD)/guest/wine.stamp
	@mkdir -p $(dir $@)
	cp $< $@
# Remembers WINE_EXE_DIR, so changing or dropping it rebuilds the ramdisk.
$(BUILD)/guest/wine.stamp: FORCE
	@mkdir -p $(dir $@)
	@echo "$(WINE_EXE_DIR)" | cmp -s - $@ || { echo "$(WINE_EXE_DIR)" > $@; rm -rf $(BUILD)/guest/wine; }
GUEST_BINS += $(BUILD)/guest/wine.stamp $(WINE_EXES)

# ---- Wine's PE DLLs as a separate boot module ---------------------------------
#
# `make wine` fetches Debian/Ubuntu's libwine package with apt-get download,
# unpacks it with dpkg-deb into its own empty directory and copies a chosen set
# of x86_64-windows PE files (debug sections stripped) and NLS tables into
# build/wine/wine.tar, laid out as C:\windows\system32. Nothing from it is
# compiled in, committed, or run on the host: the archive is handed to QEMU as
# a Multiboot module (-initrd) and only ever executed inside MergedOS, on the
# NT personality with MergedOS's own ntdll. Every test that needs it skips
# when the archive is missing.
WINE_DIR   := $(BUILD)/wine
WINE_TAR   := $(WINE_DIR)/wine.tar
WINE_PKG   ?= libwine
WINE_STRIP ?= x86_64-w64-mingw32-strip
WINE_PE    := kernel32.dll kernelbase.dll ucrtbase.dll msvcrt.dll advapi32.dll sechost.dll \
	cmd.exe reg.exe hostname.exe whoami.exe
WINE_NLS   := locale.nls l_intl.nls c_437.nls c_1252.nls c_20127.nls normnfc.nls normnfd.nls normidna.nls sortdefault.nls

.PHONY: wine wine-optional
wine: $(WINE_TAR)

# Never fails: tests run without Wine when it cannot be fetched.
wine-optional:
	@[ -f $(WINE_TAR) ] || $(MAKE) -s wine >/dev/null 2>&1 || \
		echo "note: Wine DLLs not available (make wine failed); NT Wine checks will be skipped"

$(WINE_TAR): Makefile
	@rm -rf $(WINE_DIR)/pkg $(WINE_DIR)/stage
	@mkdir -p $(WINE_DIR)/deb $(WINE_DIR)/pkg $(WINE_DIR)/stage/windows/system32
	@ls $(WINE_DIR)/deb/*.deb >/dev/null 2>&1 || (cd $(WINE_DIR)/deb && apt-get download $(WINE_PKG))
	dpkg-deb -x $$(ls $(WINE_DIR)/deb/*.deb | head -n 1) $(WINE_DIR)/pkg
	@pe=$$(find $(WINE_DIR)/pkg -type d -name x86_64-windows | head -n 1); \
	 nls=$$(find $(WINE_DIR)/pkg -type d -name nls | head -n 1); \
	 strip=$$(command -v $(WINE_STRIP) || echo cp); \
	 for f in $(WINE_PE); do \
		if [ "$$strip" = cp ]; then cp "$$pe/$$f" $(WINE_DIR)/stage/windows/system32/; \
		else $$strip --strip-debug -o $(WINE_DIR)/stage/windows/system32/$$f "$$pe/$$f"; fi || exit 1; \
	 done; \
	 for f in $(WINE_NLS); do cp "$$nls/$$f" $(WINE_DIR)/stage/windows/system32/ || exit 1; done
	tar --format=ustar --owner=0 --group=0 -cf $@.tmp -C $(WINE_DIR)/stage windows
	@mv $@.tmp $@
	@echo "built $@ ($$(($$(stat -c %s $@) / 1024)) KiB)"

.PHONY: FORCE
FORCE:

.PHONY: guests
guests: $(GUEST_BINS)

IMAGE_DEPS := $(GUEST_BINS) tools/mkimage.py src/guest/Hello.app/Contents/Info.plist

$(INITRD): $(IMAGE_DEPS)
	$(PYTHON) tools/mkimage.py initrd --guest-dir $(BUILD)/guest --src-dir src/guest --out $@

$(BUILD)/fixtures/.stamp: $(IMAGE_DEPS)
	@rm -rf $(BUILD)/fixtures
	$(PYTHON) tools/mkimage.py fixtures --guest-dir $(BUILD)/guest --src-dir src/guest \
		--out $(BUILD)/fixtures
	@touch $@

# ---- host tools and tests ----------------------------------------------------

.PHONY: tools
tools: $(BUILD)/host/mpkg

$(BUILD)/host/mpkg: tools/mpkg.c $(SHARED_C)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) -o $@ tools/mpkg.c $(SHARED_C)

$(BUILD)/host/run_tests: $(TEST_C) $(SHARED_C) $(UI_C) $(wildcard tests/*.h)
	@mkdir -p $(dir $@)
	$(HOSTCC) $(HOST_CFLAGS) $(HOST_SAN) -DFIXTURE_DIR=\"$(BUILD)/fixtures\" \
		-o $@ $(TEST_C) $(SHARED_C) $(UI_C)

.PHONY: test test-host test-boot test-disk test-e2e
test: test-host test-boot test-disk test-e2e

test-host: $(BUILD)/host/run_tests $(BUILD)/fixtures/.stamp
	$(BUILD)/host/run_tests

test-boot: $(KERNEL_ELF32) wine-optional
	QEMU=$(QEMU) QEMU_MEM=$(QEMU_MEM) WINE_TAR=$(WINE_TAR) tools/boot-test.sh $(KERNEL_ELF32)

# Install onto an emulated dual-boot IDE disk; prove existing partitions are untouched.
test-disk: $(KERNEL_ELF32)
	QEMU=$(QEMU) PYTHON=$(PYTHON) tools/disk-test.sh $(KERNEL_ELF32)

# Wizard -> save -> reset -> login (wrong, then right password) -> saved desktop.
test-e2e: $(KERNEL_ELF32)
	QEMU=$(QEMU) $(PYTHON) tools/e2e-test.py $(KERNEL_ELF32)

# ---- running -----------------------------------------------------------------

QEMU_FLAGS := -m $(QEMU_MEM) -no-reboot \
	-device isa-debug-exit,iobase=0xf4,iosize=0x04

.PHONY: run run-serial
WINE_INITRD = $(if $(wildcard $(WINE_TAR)),-initrd $(WINE_TAR))
run: $(KERNEL_ELF32)
	$(QEMU) $(QEMU_FLAGS) -kernel $< $(WINE_INITRD) -serial stdio -append "$(APPEND)"

run-serial: $(KERNEL_ELF32)
	$(QEMU) $(QEMU_FLAGS) -kernel $< $(WINE_INITRD) -display none -serial stdio -append "$(APPEND)"

# ---- bootable ISO --------------------------------------------------------------

.PHONY: iso
iso: $(ISO)

$(ISO): $(KERNEL_ELF) tools/grub.cfg
	@command -v grub-mkrescue >/dev/null || { echo "grub-mkrescue not found"; exit 1; }
	@rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp $(KERNEL_ELF) $(BUILD)/iso/boot/mergedos.elf
	cp tools/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ $(BUILD)/iso 2>/dev/null
	@echo "built $@"

.PHONY: clean
clean:
	rm -rf $(BUILD)

-include $(KDEPS)
