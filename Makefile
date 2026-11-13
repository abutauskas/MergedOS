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

TEST_C := tests/test_main.c tests/test_abi.c tests/test_pe.c tests/test_de_msh.c tests/test_pkg.c tests/test_codec.c tests/test_ui.c tests/test_storage.c tests/test_desktop.c tests/test_fence.c tests/test_filetype.c tests/test_ramfs.c tests/test_darwin.c

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

NTRT_SRC  := src/ntrt/start.S src/ntrt/kernel32.c src/ntrt/crt.c
NTRT_OBJS := $(patsubst src/ntrt/%,$(BUILD)/ntrt/%.o,$(basename $(NTRT_SRC)))
NTRT_CFLAGS := -std=gnu11 -Os $(WARNINGS) -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pic -fno-pie -fcf-protection=none -mcmodel=small \
	-fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns -I.

$(BUILD)/ntrt/%.o: src/ntrt/%.c src/ntrt/rt.h src/ntrt/ntrt_abi.h
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

# Dynamically linked test programs: run natively under their own, unmodified
# dynamic linkers (glibc ld-linux, musl ld-musl). Built when the toolchain is
# present; otherwise empty files, and the kernel reports the checks as skipped.
# They live only in the sysroot module (build/sysroot.tar), not the ramdisk.
GNU_CC ?= gcc
$(BUILD)/guest/libdyntest.so: src/guest/libdyntest.c
	@mkdir -p $(dir $@)
	@if command -v $(GNU_CC) >/dev/null 2>&1; then \
		$(GNU_CC) -O2 -fPIC -shared -s -o $@ $<; \
	else : > $@; fi
$(BUILD)/guest/dyntest.gnu: src/guest/dyntest.c $(BUILD)/guest/libdyntest.so
	@mkdir -p $(dir $@)
	@if command -v $(GNU_CC) >/dev/null 2>&1; then \
		$(GNU_CC) -O2 -s -DLIBC_NAME='"glibc"' -DDLLIB='"/lib/libdyntest.so"' \
			-o $@ $< -ldl; \
	else echo "note: $(GNU_CC) not found, dyntest.gnu left out"; : > $@; fi
$(BUILD)/guest/libdyntest-musl.so: src/guest/libdyntest.c
	@mkdir -p $(dir $@)
	@if command -v $(MUSL_CC) >/dev/null 2>&1; then \
		$(MUSL_CC) -O2 -fPIC -shared -s -o $@ $<; \
	else : > $@; fi
$(BUILD)/guest/dyntest.musl: src/guest/dyntest.c $(BUILD)/guest/libdyntest-musl.so
	@mkdir -p $(dir $@)
	@if command -v $(MUSL_CC) >/dev/null 2>&1; then \
		$(MUSL_CC) -O2 -s -DLIBC_NAME='"musl"' -DDLLIB='"/lib/libdyntest-musl.so"' \
			-o $@ $<; \
	else echo "note: $(MUSL_CC) not found, dyntest.musl left out"; : > $@; fi
DYN_BINS := $(BUILD)/guest/dyntest.gnu $(BUILD)/guest/libdyntest.so \
	    $(BUILD)/guest/dyntest.musl $(BUILD)/guest/libdyntest-musl.so
GUEST_BINS += $(DYN_BINS)

# Darwin user space: MergedOS's own dyld and libSystem.B.dylib plus small
# command-line programs, built as x86-64 Mach-O with the host clang and
# ld64.lld (apt: clang lld). Without them empty files are produced and the
# kernel reports the checks as skipped. Installed only in the sysroot module,
# laid out as a Darwin root: usr/lib/{dyld,libSystem.B.dylib,...}.
DARWIN_CC ?= clang
DARWIN_LLD ?= $(shell for c in ld64.lld ld64.lld-18; do command -v $$c && break; done 2>/dev/null)
DARWIN_DIR := $(BUILD)/guest/darwin
DARWIN_ROOT := $(DARWIN_DIR)/root
DARWIN_LINKROOT := $(DARWIN_DIR)/linkroot
DARWIN_CFLAGS := -target x86_64-apple-macos10.15 -ffreestanding -fPIC -Os -fno-stack-protector \
	-Wall -Wextra -Isrc/darwin
DARWIN_LDFLAGS := -arch x86_64 -platform_version macos 10.15 10.15 -no_uuid
DARWIN_HAVE := $(and $(DARWIN_LLD),$(shell command -v $(DARWIN_CC) 2>/dev/null))
# $(call DARWIN_BUILD,output,recipe-when-toolchain-present)
DARWIN_BUILD = @mkdir -p $(dir $@); if [ -n "$(DARWIN_HAVE)" ]; then $(2); \
	else echo "note: clang/ld64.lld not found, $(notdir $@) left out"; : > $@; fi

$(DARWIN_DIR)/%.o: src/darwin/%.c src/darwin/mini.h
	$(call DARWIN_BUILD,$@,$(DARWIN_CC) $(DARWIN_CFLAGS) -c $< -o $@)

$(DARWIN_ROOT)/usr/lib/dyld: $(DARWIN_DIR)/dyld.o
	$(call DARWIN_BUILD,$@,$(DARWIN_LLD) $(DARWIN_LDFLAGS) -e _dyld_start \
		-pagezero_size 0x7ff000000000 -o $@ $<)
$(DARWIN_ROOT)/usr/lib/libSystem.B.dylib: $(DARWIN_DIR)/libsystem.o
	$(call DARWIN_BUILD,$@,$(DARWIN_LLD) $(DARWIN_LDFLAGS) -dylib \
		-install_name /usr/lib/libSystem.B.dylib -o $@ $<)
$(DARWIN_ROOT)/usr/lib/libgreet.dylib: $(DARWIN_DIR)/libgreet.o $(DARWIN_ROOT)/usr/lib/libSystem.B.dylib
	$(call DARWIN_BUILD,$@,$(DARWIN_LLD) $(DARWIN_LDFLAGS) -dylib -install_name /usr/lib/libgreet.dylib \
		-o $@ $^)
$(DARWIN_ROOT)/usr/lib/libwrap.dylib: $(DARWIN_DIR)/libwrap.o $(DARWIN_ROOT)/usr/lib/libSystem.B.dylib \
		$(DARWIN_ROOT)/usr/lib/libgreet.dylib
	$(call DARWIN_BUILD,$@,$(DARWIN_LLD) $(DARWIN_LDFLAGS) -dylib -install_name /usr/lib/libwrap.dylib \
		-syslibroot $(DARWIN_ROOT) -reexport_library $(DARWIN_ROOT)/usr/lib/libgreet.dylib \
		-o $@ $< $(DARWIN_ROOT)/usr/lib/libSystem.B.dylib)

# Link-time stand-ins (what an SDK's .tbd files are): same install names, but
# libwrap defines everything itself, so programs bind to libwrap by ordinal and
# the real, re-exporting libwrap.dylib has to resolve through libgreet at run time.
$(DARWIN_LINKROOT)/usr/lib/libwrap.dylib: $(DARWIN_DIR)/libwrap_stub.o
	$(call DARWIN_BUILD,$@,$(DARWIN_LLD) $(DARWIN_LDFLAGS) -dylib -install_name /usr/lib/libwrap.dylib \
		-o $@ $<)
$(DARWIN_LINKROOT)/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation: \
		$(DARWIN_DIR)/libfoundation_stub.o
	$(call DARWIN_BUILD,$@,$(DARWIN_LLD) $(DARWIN_LDFLAGS) -dylib -install_name \
		/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation -o $@ $<)

DARWIN_LINK = $(DARWIN_LLD) $(DARWIN_LDFLAGS) -syslibroot $(DARWIN_LINKROOT)
DARWIN_SYSTEM := $(DARWIN_ROOT)/usr/lib/libSystem.B.dylib
$(DARWIN_ROOT)/bin/darwin-hello: $(DARWIN_DIR)/hello.o $(DARWIN_SYSTEM)
	$(call DARWIN_BUILD,$@,$(DARWIN_LINK) -o $@ $^)
$(DARWIN_ROOT)/bin/darwin-fileio: $(DARWIN_DIR)/fileio.o $(DARWIN_SYSTEM)
	$(call DARWIN_BUILD,$@,$(DARWIN_LINK) -o $@ $^)
# Chained fixups (LC_DYLD_CHAINED_FIXUPS) instead of classic rebase/bind opcodes.
$(DARWIN_ROOT)/bin/darwin-argv: $(DARWIN_DIR)/argv.o $(DARWIN_SYSTEM)
	$(call DARWIN_BUILD,$@,$(DARWIN_LLD) -arch x86_64 -platform_version macos 12.0 12.0 -no_uuid \
		-fixup_chains -o $@ $^)
$(DARWIN_ROOT)/bin/darwin-dyn: $(DARWIN_DIR)/dyn.o $(DARWIN_LINKROOT)/usr/lib/libwrap.dylib \
		$(DARWIN_SYSTEM)
	$(call DARWIN_BUILD,$@,$(DARWIN_LINK) -U _optional_symbol -U _optional_data -o $@ $^)
# Needs Foundation, which MergedOS does not provide: exercises the missing-library report.
$(DARWIN_ROOT)/bin/darwin-needs-foundation: $(DARWIN_DIR)/needs_foundation.o \
		$(DARWIN_LINKROOT)/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation \
		$(DARWIN_SYSTEM)
	$(call DARWIN_BUILD,$@,$(DARWIN_LINK) -o $@ $^)

DARWIN_BINS := $(addprefix $(DARWIN_ROOT)/,usr/lib/dyld usr/lib/libSystem.B.dylib \
	usr/lib/libgreet.dylib usr/lib/libwrap.dylib bin/darwin-hello bin/darwin-fileio \
	bin/darwin-argv bin/darwin-dyn bin/darwin-needs-foundation)

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

.PHONY: FORCE
FORCE:

.PHONY: guests
guests: $(GUEST_BINS)

IMAGE_DEPS := $(GUEST_BINS) tools/mkimage.py src/guest/Hello.app/Contents/Info.plist

$(INITRD): $(IMAGE_DEPS)
	$(PYTHON) tools/mkimage.py initrd --guest-dir $(BUILD)/guest --src-dir src/guest --out $@

# The sysroot module: the host's glibc and musl runtimes plus the dynamic test
# programs and a distribution BusyBox, as a ustar archive loaded as a boot
# module (qemu -initrd). Merged under / by the kernel, read-only. Not in the
# kernel image, so it does not count towards the kernel footprint.
SYSROOT := $(BUILD)/sysroot.tar
$(SYSROOT): $(DYN_BINS) $(DARWIN_BINS) tools/mkimage.py
	$(PYTHON) tools/mkimage.py sysroot --guest-dir $(BUILD)/guest --src-dir src/guest --out $@

.PHONY: sysroot
sysroot: $(SYSROOT)

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
		-DDARWIN_ROOT=\"$(DARWIN_ROOT)\" \
		-o $@ $(TEST_C) $(SHARED_C) $(UI_C)

.PHONY: test test-host test-boot test-disk test-e2e
test: test-host test-boot test-disk test-e2e

test-host: $(BUILD)/host/run_tests $(BUILD)/fixtures/.stamp $(DARWIN_BINS)
	$(BUILD)/host/run_tests

test-boot: $(KERNEL_ELF32) $(SYSROOT)
	QEMU=$(QEMU) QEMU_MEM=$(QEMU_MEM) SYSROOT=$(SYSROOT) tools/boot-test.sh $(KERNEL_ELF32)

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
run: $(KERNEL_ELF32)
	$(QEMU) $(QEMU_FLAGS) -kernel $< -serial stdio -append "$(APPEND)"

run-serial: $(KERNEL_ELF32)
	$(QEMU) $(QEMU_FLAGS) -kernel $< -display none -serial stdio -append "$(APPEND)"

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
