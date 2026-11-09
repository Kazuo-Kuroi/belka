#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-09
# SPDX-License-Identifier: MIT
#
# The GNU link-editor (binutils ld), from the host.
#
# LINK_KERNEL links the kernel's objects (in order) to $@; LINK_PROGRAM
# links a static user program, given its objects; PROGRAM_BEGIN and
# PROGRAM_END are linked around a C program's objects and libraries.
#

LD              := ld
LD_DEPS         :=

LINK_KERNEL     = $(LD) -nostdlib -static -z max-page-size=0x1000 \
                  -z noexecstack -T $(LDSCRIPT) -o $@
LINK_PROGRAM    = $(LD) -static --build-id=none -z max-page-size=4096 \
                  -z noexecstack -e _start -o $@
PROGRAM_BEGIN   := $(LIBC_CRTBEGIN)
PROGRAM_END     := $(LIBC_CRTEND)
