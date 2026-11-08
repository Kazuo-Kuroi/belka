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
#   SMP         number of CPUs given to QEMU (default 4)
#
# Targets: all, iso, run, test, limine, clean, distclean.
#

ARCH            ?= amd64
TOOLCHAIN       ?= gcc
INLINE_ASM      ?= yes
WERROR          ?= no
SMP             ?= 4
MEM             ?= 512M

BUILD           := build/$(ARCH)-$(TOOLCHAIN)
ifneq ($(INLINE_ASM),yes)
BUILD           := $(BUILD)-noasm
endif

include mk/toolchain-$(TOOLCHAIN).mk
include sys/arch/$(ARCH)/conf/files.mk

LIMINE_VERSION  := 11.2.1
LIMINE_DIR      := build/limine-$(LIMINE_VERSION)
OVMF_CODE       ?= /usr/share/OVMF/OVMF_CODE_4M.fd

KERNEL          := $(BUILD)/belka
ISO             := $(BUILD)/belka.iso
TEST_ISO        := $(BUILD)/belka-test.iso

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
LDFLAGS         := -nostdlib -static -z max-page-size=0x1000 \
                   -z noexecstack -T $(LDSCRIPT)

SRCS            := $(MI_SRCS) $(MD_SRCS)
OBJS            := $(patsubst %,$(BUILD)/%.o,$(basename $(SRCS)))

.PHONY: all iso run test limine clean distclean

all: $(KERNEL)

$(KERNEL): $(OBJS) $(LDSCRIPT)
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS:.o=.d)

limine: $(LIMINE_DIR)/.stamp

$(LIMINE_DIR)/.stamp: tools/fetch-limine.sh
	sh tools/fetch-limine.sh $(LIMINE_VERSION) $(LIMINE_DIR)
	@touch $@

iso: $(ISO)

# mkiso.sh <kernel> <limine dir> <limine.conf> <output>
$(ISO): $(KERNEL) $(LIMINE_DIR)/.stamp $(LIMINE_CONF) tools/mkiso.sh
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) $(LIMINE_CONF) $@

$(TEST_ISO): $(KERNEL) $(LIMINE_DIR)/.stamp $(LIMINE_TEST_CONF) tools/mkiso.sh
	sh tools/mkiso.sh $(KERNEL) $(LIMINE_DIR) $(LIMINE_TEST_CONF) $@

QEMU            ?= qemu-system-x86_64
QEMU_FLAGS      := -machine q35,accel=kvm:tcg -cpu max -smp $(SMP) -m $(MEM) \
                   -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
                   -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
                   -display none -no-reboot

run: $(ISO)
	$(QEMU) $(QEMU_FLAGS) -cdrom $(ISO) -serial mon:stdio

# A test boot passes when the kernel reports success through the QEMU
# debug-exit device, which makes QEMU exit with status 33.
test: $(TEST_ISO)
	sh tools/qemu-test.sh $(BUILD)/test-serial.log \
	    $(QEMU) $(QEMU_FLAGS) -cdrom $(TEST_ISO)

clean:
	rm -rf $(BUILD)

distclean:
	rm -rf build
