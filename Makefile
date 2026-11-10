#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# Top-level build for the Belka kernel.
#
# Variables:
#   TOOLCHAIN   gcc (default) or clang
#   INLINE_ASM  yes (default) or no; "no" builds the out-of-line
#               fallback paths that a compiler without GNU inline
#               assembly would use
#   WERROR      yes to treat warnings as errors
#   LINKER      illumos (default) or gnu: the link-editor for the
#               kernel and user programs (mk/linker-*.mk)
#   SMP         number of CPUs given to QEMU (default 4)
#
# Targets: all, iso, run, test, test-fault, test-apfault, test-stack,
# test-uaf, test-reboot, test-all, limine, clean, distclean.
#

ARCH            ?= amd64
TOOLCHAIN       ?= gcc
LINKER          ?= illumos
INLINE_ASM      ?= yes
WERROR          ?= no
SMP             ?= 4
MEM             ?= 512M

# Outputs that depend on the link-editor are kept apart for each.
ifeq ($(LINKER),illumos)
LDSUFFIX        :=
else
LDSUFFIX        := -$(LINKER)ld
endif

BUILD           := build/$(ARCH)-$(TOOLCHAIN)
ifneq ($(INLINE_ASM),yes)
BUILD           := $(BUILD)-noasm
endif
BUILD           := $(BUILD)$(LDSUFFIX)

.DEFAULT_GOAL   := all

include mk/toolchain-$(TOOLCHAIN).mk
include sys/arch/$(ARCH)/conf/files.mk
include mk/linker-$(LINKER).mk

LIMINE_VERSION  := 11.2.1
LIMINE_DIR      := build/limine-$(LIMINE_VERSION)
OVMF_CODE       ?= /usr/share/OVMF/OVMF_CODE_4M.fd

# A test disk with a known pattern in every sector, attached as a
# modern-only virtio block device. Tests write only to its last 384
# sectors, so the pattern elsewhere survives from run to run.
HOSTCC          ?= cc
DISK            := build/test-disk.img
MKDISK          := build/tools/mkdisk

# Two ext2 images, rebuilt before every QEMU run because the tests write
# to them: vd1 with 1 KB blocks and vd2 with 4 KB blocks.
EXT2_1K         := build/test-ext2-1k.img
EXT2_4K         := build/test-ext2-4k.img
E2FSCK          ?= /sbin/e2fsck
DEBUGFS         ?= /sbin/debugfs

KERNEL          := $(BUILD)/belka
ISO             := $(BUILD)/belka.iso
TEST_ISO        := $(BUILD)/belka-test.iso
FAULT_ISO       := $(BUILD)/belka-fault.iso
APFAULT_ISO     := $(BUILD)/belka-apfault.iso
STACK_ISO       := $(BUILD)/belka-stack.iso
UAF_ISO         := $(BUILD)/belka-uaf.iso
REBOOT_ISO      := $(BUILD)/belka-reboot.iso

INCLUDES        := -nostdinc -isystem $(CC_INCLUDE) \
                   -Isys -Isys/arch/$(ARCH)/include -Isys/external

WARNINGS        := -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes \
                   -Wpointer-arith -Wshadow -Wcast-qual
ifeq ($(WERROR),yes)
WARNINGS        += -Werror
endif

DEFINES         := -D_KERNEL
ifneq ($(INLINE_ASM),yes)
DEFINES         += -DBELKA_NO_INLINE_ASM
endif

CFLAGS          := -std=c99 -pedantic $(WARNINGS) -O2 -g \
                   -ffreestanding -fno-builtin -fno-common \
                   -fno-strict-aliasing -fno-stack-protector \
                   -fno-pic -fno-pie -fno-omit-frame-pointer \
                   -fno-asynchronous-unwind-tables -fno-unwind-tables \
                   $(MD_CFLAGS) $(TC_CFLAGS) $(DEFINES) $(INCLUDES)
ASFLAGS         := -g -fno-pic -fno-pie $(MD_ASFLAGS) $(TC_CFLAGS) $(DEFINES) $(INCLUDES)

SRCS            := $(MI_SRCS) $(MD_SRCS)
OBJS            := $(patsubst %,$(BUILD)/%.o,$(basename $(SRCS)))

# Linked first and last: they mark the kernel's segments (kernel_begin.S).
KERNEL_BEGIN    := $(BUILD)/$(basename $(KERNEL_BEGIN_SRC)).o
KERNEL_END      := $(BUILD)/$(basename $(KERNEL_END_SRC)).o
KERNEL_OBJS     := $(KERNEL_BEGIN) $(OBJS) $(KERNEL_END)

.PHONY: FORCE mlibc ext2-images user-progs all iso run test test-fault test-apfault test-stack test-uaf \
	test-reboot test-all limine clean distclean

all: $(KERNEL)

$(KERNEL): $(KERNEL_OBJS) $(LDSCRIPT) $(KERNEL_MAP) $(LD_DEPS) \
                tools/check-kernel.sh
	$(LINK_KERNEL) $(KERNEL_OBJS)
	sh tools/check-kernel.sh $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

-include $(KERNEL_OBJS:.o=.d)

limine: $(LIMINE_DIR)/.stamp

$(LIMINE_DIR)/.stamp: tools/fetch-limine.sh
	sh tools/fetch-limine.sh $(LIMINE_VERSION) $(LIMINE_DIR)
	@touch $@

iso: $(ISO)

# mkiso.sh kernel limine-dir timeout cmdline output
ISO_DEPS        := $(KERNEL) $(LIMINE_DIR)/.stamp tools/mkiso.sh $(DISK)

$(ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 3 "" $@

$(TEST_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "debugexit" $@

$(FAULT_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "debugexit testfault" $@

$(APFAULT_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "debugexit testapfault" $@

$(STACK_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "debugexit teststack" $@

$(UAF_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "debugexit testuaf" $@

$(REBOOT_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "debugexit testreboot" $@


$(MKDISK): tools/mkdisk.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -o $@ tools/mkdisk.c

$(DISK): $(MKDISK)
	$(MKDISK) $@

QEMU            ?= qemu-system-x86_64
QEMU_FLAGS      := -machine q35,accel=kvm:tcg -cpu max -smp $(SMP) -m $(MEM) \
                   -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
                   -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
                   -drive file=$(DISK),if=none,id=vd0,format=raw \
                   -device virtio-blk-pci,drive=vd0,disable-legacy=on \
                   -drive file=$(EXT2_1K),if=none,id=vd1,format=raw \
                   -device virtio-blk-pci,drive=vd1,disable-legacy=on \
                   -drive file=$(EXT2_4K),if=none,id=vd2,format=raw \
                   -device virtio-blk-pci,drive=vd2,disable-legacy=on \
                   -display none -no-reboot

# User-mode test programs, built with the host compiler against the
# kernel's own ABI headers and a tiny C library (usr/tests/lib), and put
# in /bin on both ext2 images.
USER_CC         ?= $(HOSTCC)
USER_DIR        := build/user$(LDSUFFIX)
USER_OBJ        := build/user-obj
USER_PROGS      := hello segv argtest utest sigtest lwptest
USER_BINS       := $(addprefix $(USER_DIR)/,$(USER_PROGS))
USER_CFLAGS     := -std=c99 -O2 -g -Wall -Wextra -Werror -ffreestanding \
                   -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
                   -Isys -Isys/arch/$(ARCH)/include
USER_LIB        := $(USER_OBJ)/lib/crt0.o $(USER_OBJ)/lib/libt.o

$(USER_OBJ)/%.o: usr/tests/%.c usr/tests/lib/libt.h
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c -o $@ $<

$(USER_OBJ)/%.o: usr/tests/%.S
	@mkdir -p $(dir $@)
	$(USER_CC) $(USER_CFLAGS) -c -o $@ $<

$(USER_BINS): $(USER_DIR)/%: $(USER_OBJ)/%.o $(USER_LIB) $(LD_DEPS)
	@mkdir -p $(USER_DIR)
	$(LINK_PROGRAM) $(USER_LIB) $<

# The C library: mlibc, from the contrib/mlibc submodule (the belka
# branch of github.com/Kazuo-Kuroi/mlibc), built with muon and samurai
# (C implementations of meson and ninja) and installed into a sysroot,
# with the same compiler as the kernel. The first configure fetches
# mlibc's own subprojects (frigg and the freestanding headers) with git.
# Programs that use it are linked statically; the test images get
# copies stripped of debug information.
MUON            ?= muon
SAMU            ?= samu
MLIBC_SRC       := contrib/mlibc
MLIBC_BUILD     := build/mlibc-muon-$(TOOLCHAIN)
MLIBC_A         := $(MLIBC_BUILD)/libc.a
SYSROOT         := build/sysroot-$(TOOLCHAIN)
MLIBC           := $(SYSROOT)/usr/lib/libc.a
LIBC_DIR        := build/user-libc-$(TOOLCHAIN)$(LDSUFFIX)
LIBC_PROGS      := libctest threadtest
LIBC_BINS       := $(addprefix $(LIBC_DIR)/,$(LIBC_PROGS))
LIBC_DBGDIR     := $(LIBC_DIR)-debug
LIBC_DBGBINS    := $(addprefix $(LIBC_DBGDIR)/,$(LIBC_PROGS))
LIBC_CFLAGS     := -std=c99 -O2 -g -Wall -Wextra -Werror -nostdinc \
                   -isystem $(SYSROOT)/usr/include -isystem $(CC_INCLUDE) \
                   -fno-pic -fno-pie -fno-stack-protector
LIBC_CRT0       := $(SYSROOT)/usr/lib/crt1.o $(SYSROOT)/usr/lib/crti.o
LIBC_CRTN       := $(SYSROOT)/usr/lib/crtn.o

# Belka's own crtbegin and crtend, for the illumos link-editor.
CRT_DIR         := build/crt-$(TOOLCHAIN)

$(CRT_DIR)/%.o: usr/lib/crt/%.S
	@mkdir -p $(CRT_DIR)
	$(LIBC_CC) -c -o $@ $<

# samu decides what to rebuild; libc.a's time changes only when it does.
$(MLIBC_A): FORCE
	@test -f $(MLIBC_SRC)/meson.build || { echo "$(MLIBC_SRC):" \
	    "run git submodule update --init" >&2; exit 1; }
	@test -f $(MLIBC_BUILD)/build.ninja || $(MUON) -C $(MLIBC_SRC) setup \
	    -pcross:$(abspath tools/mlibc/$(TOOLCHAIN).cross) \
	    -Dprefix=/usr -Ddefault_library=static -Dbuildtype=debugoptimized \
	    -Duse_freestnd_hdrs=enabled -Dlibgcc_dependency=false \
	    -Dsysdeps=belka $(abspath $(MLIBC_BUILD))
	$(SAMU) -C $(MLIBC_BUILD)

# muon installs every file again, so the sysroot is made afresh only
# when libc.a was rebuilt; otherwise every program would be relinked.
$(MLIBC): $(MLIBC_A)
	rm -rf $(SYSROOT)
	$(MUON) -C $(MLIBC_BUILD) install -d $(abspath $(SYSROOT))
	@touch $@

mlibc: $(MLIBC)

# The kernel's ABI as mlibc's headers describe it must match the
# kernel's own headers (tools/abicheck).
ABICHECK        := build/abicheck-$(TOOLCHAIN)

$(ABICHECK)/ok: tools/abicheck/kernel.c tools/abicheck/libc.c \
                tools/abicheck/abi.def $(MLIBC) $(wildcard sys/sys/*.h)
	@mkdir -p $(ABICHECK)
	$(HOSTCC) -std=c99 -ffreestanding -Isys -Isys/arch/$(ARCH)/include \
	    -o $(ABICHECK)/kernel tools/abicheck/kernel.c
	$(ABICHECK)/kernel > $(ABICHECK)/kernel-abi.h
	$(LIBC_CC) $(LIBC_CFLAGS) -I$(ABICHECK) -c -o $(ABICHECK)/libc.o \
	    tools/abicheck/libc.c
	@touch $@

# Linked with the link-editor itself rather than through the compiler
# driver, so that both toolchains link the same way.
$(LIBC_DBGBINS): $(LIBC_DBGDIR)/%: usr/tests/%.c $(MLIBC) $(ABICHECK)/ok \
                $(PROGRAM_BEGIN) $(PROGRAM_END) $(LD_DEPS)
	@mkdir -p $(LIBC_DBGDIR)
	$(LIBC_CC) $(LIBC_CFLAGS) -c -o $@.o $<
	$(LINK_PROGRAM) $(LIBC_CRT0) $(PROGRAM_BEGIN) $@.o $(MLIBC) \
	    $(LIBC_RTLIB) $(PROGRAM_END) $(LIBC_CRTN)

$(LIBC_BINS): $(LIBC_DIR)/%: $(LIBC_DBGDIR)/%
	@mkdir -p $(LIBC_DIR)
	strip --strip-debug -o $@ $<

user-progs: $(USER_BINS) $(LIBC_BINS)

# Every recipe that boots the kernel rebuilds the images first, since a
# prerequisite would run only once for all the boots of one make run.
MKEXT2          = @mkdir -p build && \
                  sh tools/mkext2.sh $(EXT2_1K) 1024 $(USER_DIR) $(LIBC_DIR) && \
                  sh tools/mkext2.sh $(EXT2_4K) 4096 $(USER_DIR) $(LIBC_DIR)

ext2-images: $(USER_BINS) $(LIBC_BINS)
	$(MKEXT2)

run: $(ISO) $(DISK) $(USER_BINS) $(LIBC_BINS)
	$(MKEXT2)
	$(QEMU) $(QEMU_FLAGS) -cdrom $(ISO) -serial mon:stdio

# The kernel reports its result through the QEMU debug-exit device:
# QEMU exits with 33 after a clean halt and 35 after a panic. "test"
# expects a clean run; "test-fault" makes the kernel touch an unmapped
# address and expects the resulting panic; "test-apfault" does the same
# on a secondary processor; "test-stack" and "test-uaf" check that stack
# overflow and use after free are caught.
test: $(TEST_ISO) $(USER_BINS) $(LIBC_BINS)
	$(MKEXT2)
	sh tools/qemu-test.sh pass $(BUILD)/test-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(TEST_ISO)
	@# The kernel unmounted both file systems; they must be clean and
	@# hold what the self-tests left for us.
	$(E2FSCK) -fn $(EXT2_1K)
	$(E2FSCK) -fn $(EXT2_4K)
	sh tools/check-ext2.sh $(DEBUGFS) $(EXT2_1K) $(EXT2_4K)

test-fault: $(FAULT_ISO) $(USER_BINS) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_EXPECT="panic: page fault" sh tools/qemu-test.sh panic \
	    $(BUILD)/test-fault-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(FAULT_ISO)

test-apfault: $(APFAULT_ISO) $(USER_BINS) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_EXPECT="panic: page fault" sh tools/qemu-test.sh panic \
	    $(BUILD)/test-apfault-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(APFAULT_ISO)

# A kernel stack overflow must hit the guard page and double fault.
test-stack: $(STACK_ISO) $(USER_BINS) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_EXPECT="double fault" sh tools/qemu-test.sh panic \
	    $(BUILD)/test-stack-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(STACK_ISO)

# A write to freed kmem must be caught when the object is reused.
test-uaf: $(UAF_ISO) $(USER_BINS) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_EXPECT="modified after free" sh tools/qemu-test.sh panic \
	    $(BUILD)/test-uaf-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(UAF_ISO)

# A reboot through the ACPI reset register makes QEMU, run with
# -no-reboot, exit with status 0.
test-reboot: $(REBOOT_ISO) $(USER_BINS) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_EXPECT="rebooting" sh tools/qemu-test.sh reset \
	    $(BUILD)/test-reboot-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(REBOOT_ISO)

test-all: test test-fault test-apfault test-stack test-uaf test-reboot

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf build
