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
#   GCC_VERSION with TOOLCHAIN=gcc, build with gcc-N and g++-N (say 14)
#
# Settings for one host (GCC_VERSION, say) may go in config.mk, which is
# read first if it exists and is not kept in git.
#
# Targets: all, iso, run, test, test-fault, test-apfault, test-stack,
# test-uaf, test-reboot, test-all, limine, clean, distclean.
#

-include config.mk

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
include mk/illumos-ld.mk
include mk/linker-$(LINKER).mk

LIMINE_VERSION  := 11.2.1
LIMINE_DIR      := build/limine-$(LIMINE_VERSION)
OVMF_CODE       ?= /usr/share/OVMF/OVMF_CODE_4M.fd

# A test disk with a known pattern in every sector, attached as a
# modern-only virtio block device. Tests write only to its last 384
# sectors, so the pattern elsewhere survives from run to run.
HOSTCC          ?= cc
DISK            := build/test-disk.img
DISK_NVME       := build/test-disk-nvme.img
DISK_SATA       := build/test-disk-sata.img
DISK_GPT        := build/test-disk-gpt.img
MKGPT           := build/tools/mkgpt
MKDISK          := build/tools/mkdisk

# Two ext2 images, rebuilt before every QEMU run because the tests write
# to them: vd1 with 1 KB blocks and vd2 with 4 KB blocks.
EXT2_1K         := build/test-ext2-1k.img
EXT2_4K         := build/test-ext2-4k.img
EXT2_4KN        := build/test-ext2-4kn.img
E2FSCK          ?= /sbin/e2fsck
MKE2FS          ?= /sbin/mke2fs
DEBUGFS         ?= /sbin/debugfs

KERNEL          := $(BUILD)/belka
ISO             := $(BUILD)/belka.iso
TEST_ISO        := $(BUILD)/belka-test.iso
FAULT_ISO       := $(BUILD)/belka-fault.iso
APFAULT_ISO     := $(BUILD)/belka-apfault.iso
STACK_ISO       := $(BUILD)/belka-stack.iso
UAF_ISO         := $(BUILD)/belka-uaf.iso
REBOOT_ISO      := $(BUILD)/belka-reboot.iso
BOOT_ISO        := $(BUILD)/belka-boot.iso

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
# uACPI and Belka's glue to it: C11 as uACPI is written, with its
# options (sizes given to frees, printf-style logging) and include path,
# and without the warnings Belka's own code is held to.
UACPI_DEFINES   := -DUACPI_SIZED_FREES -DUACPI_FORMATTED_LOGGING
UACPI_CFLAGS    := $(filter-out -std=c99 -pedantic -Wmissing-prototypes \
                   -Wstrict-prototypes -Wshadow -Wcast-qual,$(CFLAGS)) \
                   -std=gnu11 -Isys/external/uacpi/include $(UACPI_DEFINES)
$(BUILD)/sys/external/uacpi/%.o: CFLAGS := $(UACPI_CFLAGS) \
                   -Wno-unused-parameter -Wno-sign-compare
$(BUILD)/sys/dev/acpi/acpi_uacpi.o $(BUILD)/sys/dev/acpi/acpi_namespace.o: \
                   CFLAGS := $(UACPI_CFLAGS)
ASFLAGS         := -g -fno-pic -fno-pie $(MD_ASFLAGS) $(TC_CFLAGS) $(DEFINES) $(INCLUDES)

SRCS            := $(MI_SRCS) $(MD_SRCS)
OBJS            := $(patsubst %,$(BUILD)/%.o,$(basename $(SRCS)))

# Linked first and last: they mark the kernel's segments (kernel_begin.S).
KERNEL_BEGIN    := $(BUILD)/$(basename $(KERNEL_BEGIN_SRC)).o
KERNEL_END      := $(BUILD)/$(basename $(KERNEL_END_SRC)).o
KERNEL_OBJS     := $(KERNEL_BEGIN) $(OBJS) $(KERNEL_END)

.PHONY: FORCE mlibc mlibc-shared ext2-images user-progs all iso run test test-fault test-apfault test-stack test-uaf \
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
ISO_DEPS        := $(KERNEL) $(LIMINE_DIR)/.stamp tools/mkiso.sh $(DISK) \
                   $(DISK_NVME) $(DISK_SATA) $(DISK_GPT)

$(ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 3 "" $@

$(TEST_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "selftest debugexit" $@

$(FAULT_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "selftest debugexit testfault" $@

$(APFAULT_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "selftest debugexit testapfault" $@

$(STACK_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "selftest debugexit teststack" $@

$(UAF_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "selftest debugexit testuaf" $@

$(REBOOT_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "selftest debugexit testreboot" $@

$(BOOT_ISO): $(ISO_DEPS)
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) 0 "debugexit" $@


$(MKDISK): tools/mkdisk.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -o $@ tools/mkdisk.c

# The test disk, and copies of it for each kind of disk the self-tests
# drives (NVMe, and SATA on q35's AHCI), as each gets written.
$(DISK) $(DISK_NVME) $(DISK_SATA): $(MKDISK)
	$(MKDISK) $@

$(MKGPT): tools/mkgpt.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -o $@ tools/mkgpt.c

# The test disk again, partitioned (GPT): entries 1, 2 and 4, in the
# pattern's sectors, for the partition self-tests. (A copy with its
# primary header gone would not do for the backup: OVMF mends it from
# the backup before the kernel boots, so the self-test breaks it.)
GPT_PARTS       := 2048,4095,efi,ESP 4096,12287,linux,belka-root \
                   -n 4 12288,16000,swap
$(DISK_GPT): $(MKDISK) $(MKGPT)
	$(MKDISK) $@
	$(MKGPT) $@ $(GPT_PARTS)

# The guest's processor: everything the host offers, with an invariant
# TSC, which Belka's nanosecond clock needs (QEMU leaves it out of
# "max"); QEMU_CPU=qemu64 tests the clock-tick fallback.
QEMU            ?= qemu-system-x86_64
QEMU_CPU        ?= max,+invtsc
QEMU_BASE       := -machine q35,accel=kvm:tcg -cpu $(QEMU_CPU) -smp $(SMP) -m $(MEM) \
                   -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
                   -device isa-debug-exit,iobase=0xf4,iosize=0x04
# The self-test boots: the raw test disk and the two ext2 images, the
# test disk again on an NVMe controller and on q35's AHCI (with the
# CD-ROM the ISO is on), a virtio-net device on QEMU's user-mode network (no host privilege), at a
# station address usr/tests/nettest.c expects, and two more on a hub of
# their own, for usr/tests/fwdtest.c (the IP interface, and the hosts).
# Intel's 82574 (QEMU's e1000e, by MSI-X) and 82540EM (e1000, by INTx)
# are on that hub too, for usr/tests/e1000test.c.
TEST_MAC        := 52:54:00:12:34:56
TEST_MAC1       := 52:54:00:12:34:57
TEST_MAC2       := 52:54:00:12:34:58
TEST_MAC3       := 52:54:00:12:34:59
TEST_MAC4       := 52:54:00:12:34:5a
QEMU_FLAGS      := $(QEMU_BASE) \
                   -drive file=$(DISK),if=none,id=vd0,format=raw \
                   -device virtio-blk-pci,drive=vd0,disable-legacy=on \
                   -drive file=$(EXT2_1K),if=none,id=vd1,format=raw \
                   -device virtio-blk-pci,drive=vd1,disable-legacy=on \
                   -drive file=$(EXT2_4K),if=none,id=vd2,format=raw \
                   -device virtio-blk-pci,drive=vd2,disable-legacy=on \
                   -drive file=$(DISK_GPT),if=none,id=vd3,format=raw \
                   -device virtio-blk-pci,drive=vd3,disable-legacy=on \
                   -drive file=$(DISK_NVME),if=none,id=nv0,format=raw \
                   -device nvme,drive=nv0,serial=belka0 \
                   -drive file=$(EXT2_4KN),if=none,id=nv1,format=raw \
                   -device nvme,drive=nv1,serial=belka4k,logical_block_size=4096,physical_block_size=4096 \
                   -drive file=$(DISK_SATA),if=none,id=sd0,format=raw \
                   -device ide-hd,drive=sd0,bus=ide.0 \
                   -nic none -netdev user,id=n0 \
                   -device virtio-net-pci,netdev=n0,mac=$(TEST_MAC),disable-legacy=on \
                   -netdev hubport,id=h1,hubid=1 \
                   -device virtio-net-pci,netdev=h1,mac=$(TEST_MAC1),disable-legacy=on \
                   -netdev hubport,id=h2,hubid=1 \
                   -device virtio-net-pci,netdev=h2,mac=$(TEST_MAC2),disable-legacy=on \
                   -netdev hubport,id=h3,hubid=1 \
                   -device e1000e,netdev=h3,mac=$(TEST_MAC3) \
                   -netdev hubport,id=h4,hubid=1 \
                   -device e1000,netdev=h4,mac=$(TEST_MAC4) \
                   -display none -no-reboot
# A normal boot: the root image (tools/mkroot.sh) as vd0, and the
# virtio-net device on QEMU's user-mode network, which /etc/rc.conf
# configures, with $(2) after the user-mode network's id for options of
# its own. For "run": SSH_PORT, if set, is a port on the host's loopback that
# QEMU forwards to Belka's ssh (port 22), as "ssh -p 2222 user@localhost"
# reaches it with SSH_PORT=2222.
SSH_PORT        ?=
comma           := ,
RUN_NETOPTS     := $(if $(SSH_PORT),$(comma)hostfwd=tcp:127.0.0.1:$(SSH_PORT)-:22)
QEMU_ROOT_FLAGS = $(QEMU_BASE) \
                   -drive file=$(1),if=none,id=vd0,format=raw \
                   -device virtio-blk-pci,drive=vd0,disable-legacy=on \
                   -nic none -netdev user,id=n0$(2) \
                   -device virtio-net-pci,netdev=n0,disable-legacy=on \
                   -display none -no-reboot

# The C library: mlibc, from the contrib/mlibc submodule (the belka
# branch of github.com/Kazuo-Kuroi/mlibc), built with muon and samurai
# (C implementations of meson and ninja), with the same compiler as the
# kernel, and installed into a sysroot. The first configure fetches
# mlibc's own subprojects (frigg and the freestanding headers) with git.
#
# It is built twice: as libc.a, for static programs, and as the runtime
# linker ld.so.1 and libc.so.1, which the illumos link-editor links
# whatever LINKER says (through tools/ld/illumos-cc, since mlibc links
# through the compiler driver with GNU ld's options). Both go in
# /usr/lib64, with SVR4's versioned names; dynamic programs name
# /usr/lib64/ld.so.1 as their interpreter. A build is configured afresh
# when its options or its cross file change.
MUON            ?= muon
SAMU            ?= samu
MLIBC_SRC       := contrib/mlibc
MLIBC_OPTS      := -Dprefix=/usr -Dlibdir=lib64 -Dbuildtype=debugoptimized \
                   -Duse_freestnd_hdrs=enabled -Dlibgcc_dependency=false \
                   -Dsysdeps=belka -Ddefault_library_paths=/usr/lib64
MLIBC_BUILD     := build/mlibc-muon-$(TOOLCHAIN)
MLIBC_A         := $(MLIBC_BUILD)/libc.a
MLIBC_CROSS     := $(MLIBC_BUILD).cross
MLIBC_SO_BUILD  := build/mlibc-shared-$(TOOLCHAIN)
MLIBC_SO_CROSS  := $(MLIBC_SO_BUILD).cross
MLIBC_SO        := $(MLIBC_SO_BUILD)/libc.so.1
MLIBC_LDSO      := $(MLIBC_SO_BUILD)/ld.so.1
SYSROOT         := build/sysroot-$(TOOLCHAIN)
SYSLIB          := $(SYSROOT)/usr/lib64
MLIBC           := $(SYSLIB)/libc.a
MLIBC_SHARED    := $(SYSLIB)/libc.so.1
DYN_INTERP      := /usr/lib64/ld.so.1

# Configure the mlibc build in $(1) with the options $(2), unless it was
# configured so already and its cross file ($(3)) is no newer.
define MLIBC_SETUP
	@test -f $(MLIBC_SRC)/meson.build || { echo "$(MLIBC_SRC):" \
	    "run git submodule update --init" >&2; exit 1; }
	@if [ ! -f $(1)/build.ninja ] || \
	    [ "$$(cat $(1).opts 2>/dev/null)" != "$(strip $(2))" ] || \
	    [ $(3) -nt $(1)/build.ninja ]; then \
		rm -rf $(1) && \
		$(MUON) -C $(MLIBC_SRC) setup $(2) $(abspath $(1)) && \
		echo "$(strip $(2))" > $(1).opts; \
	fi
endef

# samu decides what to rebuild; libc.a's time changes only when it does.
$(MLIBC_A): $(MLIBC_CROSS) FORCE
	$(call MLIBC_SETUP,$(MLIBC_BUILD),-pcross:$(abspath $(MLIBC_CROSS)) \
	    $(MLIBC_OPTS) -Ddefault_library=static,$(MLIBC_CROSS))
	$(SAMU) -C $(MLIBC_BUILD)

# The cross files, with the toolchain's compilers (CC and CXX) and the
# paths filled in. Each is replaced only when that changes it, which
# configures its build afresh.
MLIBC_SUBST     = @sed -e 's|@CC@|$(CC)|' -e 's|@CXX@|$(CXX)|' \
	    -e 's|@ILLUMOS_CC@|$(abspath tools/ld/illumos-cc)|' \
	    -e 's|@LD@|$(abspath $(ILLUMOS_LD))|' \
	    -e 's|@RTLIB@|$(LIBC_RTLIB)|' $< > $@.new && \
	    { cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@; }

$(MLIBC_CROSS): tools/mlibc/$(TOOLCHAIN).cross FORCE
	@mkdir -p build
	$(MLIBC_SUBST)

$(MLIBC_SO_CROSS): tools/mlibc/$(TOOLCHAIN)-shared.cross FORCE
	@mkdir -p build
	$(MLIBC_SUBST)

$(MLIBC_SO): $(MLIBC_SO_CROSS) $(ILLUMOS_LD) FORCE
	$(call MLIBC_SETUP,$(MLIBC_SO_BUILD),-pcross:$(abspath \
	    $(MLIBC_SO_CROSS)) $(MLIBC_OPTS) -Ddefault_library=shared \
	    -Dsoversion=1,$(MLIBC_SO_CROSS))
	$(SAMU) -C $(MLIBC_SO_BUILD)

# samu links ld.so.1 before libc.so.1. (An empty recipe, or make would
# try its built-in rules.)
$(MLIBC_LDSO): $(MLIBC_SO) ;

# muon installs every file again, so the sysroot is made afresh only
# when libc.a was rebuilt; otherwise every program would be relinked.
# The shared objects are installed over it.
$(MLIBC): $(MLIBC_A)
	rm -rf $(SYSROOT)
	$(MUON) -C $(MLIBC_BUILD) install -d $(abspath $(SYSROOT))
	@touch $@

$(MLIBC_SHARED): $(MLIBC_SO) $(MLIBC)
	$(MUON) -C $(MLIBC_SO_BUILD) install -d $(abspath $(SYSROOT))
	@touch $@

mlibc: $(MLIBC) $(MLIBC_SHARED)
mlibc-shared: $(MLIBC_SHARED)

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

# The user-mode test programs, built against mlibc and put in /bin on
# both ext2 images; usr/tests/rawsys.h gives them the kernel's own
# interfaces where they test those. They are linked dynamically, and
# libctest also statically (libctest-static); dyntest-pie is dyntest
# as a position-independent program. dyntest also uses
# libdt.so.1, a library of the tests' own, found through its run path
# $ORIGIN/../lib. They are linked with the link-editor itself rather
# than through the compiler driver, so that both toolchains link the
# same way.
LIBC_DIR        := build/user-libc-$(TOOLCHAIN)$(LDSUFFIX)
LIBC_DBGDIR     := $(LIBC_DIR)-debug
LIBC_PROGS      := hello segv argtest utest sigtest lwptest libctest \
                   threadtest dyntest credtest ttytest ptytest sproctest \
                   strtest nettest iptest udptest tcptest fwdtest \
                   e1000test
LIBC_SPROGS     := libctest-static sproctest-static
LIBC_PIEPROGS   := dyntest-pie
LIBC_DBGBINS    := $(addprefix $(LIBC_DBGDIR)/,$(LIBC_PROGS))
LIBC_DBINS      := $(addprefix $(LIBC_DIR)/,$(LIBC_PROGS) $(LIBC_PIEPROGS))
LIBC_SBINS      := $(addprefix $(LIBC_DIR)/,$(LIBC_SPROGS))
LIBC_BINS       := $(LIBC_DBINS) $(LIBC_SBINS)
LIBC_CFLAGS     := -std=c99 -O2 -g -Wall -Wextra -Werror -nostdinc \
                   -isystem $(SYSROOT)/usr/include -isystem $(CC_INCLUDE) \
                   -fno-stack-protector
LIBC_CRT0       := $(SYSLIB)/crt1.o $(SYSLIB)/crti.o
LIBC_CRTN       := $(SYSLIB)/crtn.o
# The test images' shared objects: mlibc's in /usr/lib64 (which the
# process self-test links to the image's), the tests' own in /lib.
LIBC_SODIR      := build/lib64-$(TOOLCHAIN)
LIBC_SOS        := $(LIBC_SODIR)/ld.so.1 $(LIBC_SODIR)/libc.so.1
TESTLIB_DIR     := build/testlib-$(TOOLCHAIN)$(LDSUFFIX)
TESTLIB         := $(TESTLIB_DIR)/libdt.so.1

# Belka's own crtbegin and crtend, for the illumos link-editor.
CRT_DIR         := build/crt-$(TOOLCHAIN)

$(CRT_DIR)/%.o: usr/lib/crt/%.S
	@mkdir -p $(CRT_DIR)
	$(LIBC_CC) -c -o $@ $<

LIBC_PROG_DEPS  := $(MLIBC) $(MLIBC_SHARED) $(ABICHECK)/ok \
                   usr/tests/rawsys.h \
                   $(PROGRAM_BEGIN) $(PROGRAM_END) $(LD_DEPS)

$(LIBC_DBGDIR)/%-static: usr/tests/%.c $(LIBC_PROG_DEPS)
	@mkdir -p $(LIBC_DBGDIR)
	$(LIBC_CC) $(LIBC_CFLAGS) -fno-pic -fno-pie -c -o $@.o $<
	$(LINK_PROGRAM) $(LIBC_CRT0) $(PROGRAM_BEGIN) $@.o $(MLIBC) \
	    $(LIBC_RTLIB) $(PROGRAM_END) $(LIBC_CRTN)

$(TESTLIB): usr/tests/libdt.c $(LIBC_PROG_DEPS)
	@mkdir -p $(TESTLIB_DIR) $(LIBC_DBGDIR)
	$(LIBC_CC) $(LIBC_CFLAGS) -fPIC -c -o $(LIBC_DBGDIR)/libdt.o $<
	$(call LINK_SHARED,libdt.so.1) $(LIBC_DBGDIR)/libdt.o \
	    $(SYSLIB)/libc.so

TEXTREL_OBJ := $(LIBC_DBGDIR)/textrel_amd64.o
$(TEXTREL_OBJ): usr/tests/textrel_amd64.S
	@mkdir -p $(LIBC_DBGDIR)
	$(LIBC_CC) -c -o $@ $<

$(LIBC_DBGDIR)/dyntest: usr/tests/dyntest.c $(TEXTREL_OBJ)

$(LIBC_DBGBINS): $(LIBC_DBGDIR)/%: usr/tests/%.c $(LIBC_PROG_DEPS) \
                $(TESTLIB)
	@mkdir -p $(LIBC_DBGDIR)
	$(LIBC_CC) $(LIBC_CFLAGS) -fno-pic -fno-pie \
	    $(if $(filter dyntest,$*),-DBELKA_TEXTREL_TEST) -c -o $@.o $<
	$(call LINK_DYNAMIC,$(DYN_INTERP),$(if $(filter dyntest,$*),\
	    '$$ORIGIN/../lib')) $(LIBC_CRT0) $(PROGRAM_BEGIN) $@.o \
	    $(if $(filter dyntest,$*),$(TEXTREL_OBJ) $(TESTLIB)) $(SYSLIB)/libc.so \
	    $(LIBC_RTLIB) $(PROGRAM_END) $(LIBC_CRTN)

$(LIBC_DBGDIR)/%-pie: usr/tests/%.c $(LIBC_PROG_DEPS) $(TESTLIB) $(PIE_BEGIN) \
                $(PIE_END)
	@mkdir -p $(LIBC_DBGDIR)
	$(LIBC_CC) $(LIBC_CFLAGS) $(LIBC_PIE_CFLAGS) -c -o $@.o $<
	$(call LINK_PIE,$(DYN_INTERP),'$$ORIGIN/../lib') $(LIBC_CRT0) $(PIE_BEGIN) \
	    $@.o $(TESTLIB) $(SYSLIB)/libc.so $(LIBC_RTLIB) $(PIE_END) \
	    $(LIBC_CRTN)

$(LIBC_SBINS): $(LIBC_DIR)/%: $(LIBC_DBGDIR)/%
	@mkdir -p $(LIBC_DIR)
	$(LINK_PROGRAM) -s $(LIBC_CRT0) $(PROGRAM_BEGIN) $<.o $(MLIBC) \
	    $(LIBC_RTLIB) $(PROGRAM_END) $(LIBC_CRTN)

# GNU strip rewrites the illumos link-editor's dynamic objects wrongly
# (moving segments), and removes initialized TLS from static LOAD ranges.
# Static tests are linked again with -s; dynamic programs are copied, and
# the shared objects are linked again with the link-editor's -s:
# samu's command for each is run with it. The images get the shared
# objects with the programs.
$(LIBC_DBINS): $(LIBC_DIR)/%: $(LIBC_DBGDIR)/% $(LIBC_SOS)
	@mkdir -p $(LIBC_DIR)
	cp $< $@

$(LIBC_SODIR)/%.so.1: $(MLIBC_SO_BUILD)/%.so.1
	@mkdir -p $(LIBC_SODIR)
	cd $(MLIBC_SO_BUILD) && \
	    eval "$$($(SAMU) -t commands $*.so.1 | tail -1)" \
	    -Wl,-s -o $(abspath $@)

user-progs: $(LIBC_BINS)

include mk/userland.mk
include mk/native-compiler.mk
include mk/native-build.mk

EXT2_DIRS       = $(LIBC_DIR) usr/lib64=$(LIBC_SODIR) \
                  lib=$(TESTLIB_DIR)

# Every recipe that boots the kernel rebuilds the images first, since a
# prerequisite would run only once for all the boots of one make run.
MKEXT2          = @mkdir -p build && \
                  sh tools/mkext2.sh $(EXT2_1K) 1024 $(EXT2_DIRS) && \
                  sh tools/mkext2.sh $(EXT2_4K) 4096 $(EXT2_DIRS) && \
                  cp $(EXT2_4K) $(EXT2_4KN)

ext2-images: $(LIBC_BINS)
	$(MKEXT2)

# A normal boot to a login prompt on the serial console, with the root
# image; "root" has no password. "run-selftest" boots the self-tests.
run: $(ISO) $(ROOT_IMG)
	@# A boot that did not end in halt leaves the image dirty, which the
	@# kernel mounts read-only; repair it here, as Belka has no fsck yet.
	$(E2FSCK) -p $(ROOT_IMG) || [ $$? -le 1 ]
	$(QEMU) $(call QEMU_ROOT_FLAGS,$(ROOT_IMG),$(RUN_NETOPTS)) -cdrom $(ISO) \
	    -serial mon:stdio

run-selftest: $(TEST_ISO) $(DISK) $(LIBC_BINS)
	$(MKEXT2)
	$(QEMU) $(QEMU_FLAGS) -cdrom $(TEST_ISO) -serial mon:stdio

# The kernel reports its result through the QEMU debug-exit device:
# QEMU exits with 33 after a clean halt and 35 after a panic. "test"
# expects a clean run; "test-fault" makes the kernel touch an unmapped
# address and expects the resulting panic; "test-apfault" does the same
# on a secondary processor; "test-stack" and "test-uaf" check that stack
# overflow and use after free are caught. "test" may take 180 seconds
# (QEMU_TIMEOUT): on one processor it takes about a minute and a half,
# half a minute of it fwdtest waiting out a reassembly.
test: $(TEST_ISO) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_TIMEOUT="$(if $(QEMU_TIMEOUT),$(QEMU_TIMEOUT),180)" \
	    sh tools/qemu-test.sh pass $(BUILD)/test-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(TEST_ISO)
	@# The kernel unmounted both file systems; they must be clean and
	@# hold what the self-tests left for us.
	$(E2FSCK) -fn $(EXT2_1K)
	$(E2FSCK) -fn $(EXT2_4K)
	$(E2FSCK) -fn $(EXT2_4KN)
	sh tools/check-ext2.sh $(DEBUGFS) $(EXT2_1K) $(EXT2_4K)

test-fault: $(FAULT_ISO) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_EXPECT="panic: page fault" sh tools/qemu-test.sh panic \
	    $(BUILD)/test-fault-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(FAULT_ISO)

test-apfault: $(APFAULT_ISO) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_EXPECT="panic: page fault" sh tools/qemu-test.sh panic \
	    $(BUILD)/test-apfault-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(APFAULT_ISO)

# A kernel stack overflow must hit the guard page and double fault.
test-stack: $(STACK_ISO) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_EXPECT="double fault" sh tools/qemu-test.sh panic \
	    $(BUILD)/test-stack-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(STACK_ISO)

# A write to freed kmem must be caught when the object is reused.
test-uaf: $(UAF_ISO) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_EXPECT="modified after free" sh tools/qemu-test.sh panic \
	    $(BUILD)/test-uaf-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(UAF_ISO)

# A reboot through the ACPI reset register makes QEMU, run with
# -no-reboot, exit with status 0.
test-reboot: $(REBOOT_ISO) $(LIBC_BINS)
	$(MKEXT2)
	QEMU_EXPECT="rebooting" sh tools/qemu-test.sh reset \
	    $(BUILD)/test-reboot-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(REBOOT_ISO)

# The system as booted: init, /etc/rc, getty, login and the shell, with
# usr/tests/boottest run from /etc/rc.local on the test root image. It
# halts through uadmin, which must leave the image clean; the console
# must have shown init's getty.
test-boot: $(BOOT_ISO) $(ROOT_DEPS) $(SYS_DIR)/boottest $(SYS_DIR)/exectest usr/tests/utiltest.sh \
                usr/tests/compiler.sh usr/tests/buildtools.sh \
                contrib/ksenon-utils/tests/run.sh
	$(MKROOT_TEST)
	$(MKDATA_TEST)
	QEMU_TIMEOUT="$(if $(QEMU_TIMEOUT),$(QEMU_TIMEOUT),$(if $(wildcard build/native-compiler-sdk/compiler.stamp),600,60))" \
	QEMU_EXPECT="boottest: 0 failures$$(printf '\nlogin: ')" \
	    sh tools/qemu-test.sh pass $(BUILD)/test-boot-serial.log \
	    $(QEMU) $(call QEMU_ROOT_FLAGS,$(ROOT_TEST_IMG)) -cdrom $(BOOT_ISO) \
	    -drive file=$(DATA_TEST_IMG),if=none,id=vd1,format=raw \
	    -device virtio-blk-pci,drive=vd1,disable-legacy=on
	$(E2FSCK) -fn $(ROOT_TEST_IMG)
	$(E2FSCK) -fn $(DATA_TEST_IMG)

# A session at the console, typed through a socket by tools/condrive
# (usr/tests/console.script), on a fresh copy of the normal root image:
# root's login on the console, line editing, ^C, job control, logging
# out and in, and halt, which must leave the image clean.
CONDRIVE        := build/tools/condrive
ROOT_CON_IMG    := build/root-console-$(TOOLCHAIN)$(LDSUFFIX).img

$(CONDRIVE): tools/condrive.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -Wall -o $@ tools/condrive.c

test-console: $(BOOT_ISO) $(ROOT_DEPS) $(CONDRIVE) usr/tests/console.script \
                tools/console-test.sh
	sh tools/mkroot.sh $(ROOT_CON_IMG) $(SYS_DIR) $(LIBC_SODIR)
	sh tools/console-test.sh $(CONDRIVE) usr/tests/console.script \
	    $(BUILD)/test-console-serial.log \
	    $(QEMU) $(call QEMU_ROOT_FLAGS,$(ROOT_CON_IMG)) -cdrom $(BOOT_ISO)
	$(E2FSCK) -fn $(ROOT_CON_IMG)

test-all: test test-fault test-apfault test-stack test-uaf test-reboot test-boot test-console

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf build
