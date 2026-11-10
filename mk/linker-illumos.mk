#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-09
# SPDX-License-Identifier: MIT
#
# The illumos link-editor (mk/illumos-ld.mk); see mk/linker-gnu.mk for
# what is defined.
#
# Static programs are linked with -dn and tools/ld/program.map, and
# Belka's own crtbegin.o and crtend.o (usr/lib/crt) mark their
# initialization arrays; the kernel is linked with kernel.map. Dynamic
# programs name their interpreter with -I and their run path with -R.
#

LD              := $(ILLUMOS_LD)
LD_DEPS         := $(LD)

LINK_KERNEL     = $(LD) -dn -e start -M $(KERNEL_MAP) -o $@
LINK_PROGRAM    = $(LD) -dn -e _start -M tools/ld/program.map -o $@
LINK_DYNAMIC    = $(LD) -dy -e _start -I $(1) -R $(2) -o $@
PROGRAM_BEGIN   = $(CRT_DIR)/crtbegin.o
PROGRAM_END     = $(CRT_DIR)/crtend.o
