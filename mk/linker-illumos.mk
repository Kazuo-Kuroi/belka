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
# programs name their interpreter with -I and their run path with -R;
# shared objects are linked with -G and named with -h, and position-
# independent programs with -z type=pie (a Belka addition).
#

LD              := $(ILLUMOS_LD)
LD_DEPS         := $(LD)

LINK_KERNEL     = $(LD) -dn -e start -M $(KERNEL_MAP) -o $@
LINK_PROGRAM    = $(LD) -dn -e _start -M tools/ld/program.map -o $@
# Shared objects' own dependencies (libc.so.1's ld.so.1) are found in
# the sysroot, as the default directory (-Y P) of the system linked for.
LINK_DYNLIB     = -dy -Y P,$(SYSLIB)
LINK_DYNAMIC    = $(LD) $(LINK_DYNLIB) -e _start -I $(1) $(if $(2),-R $(2)) \
                  -o $@
LINK_SHARED     = $(LD) -G $(LINK_DYNLIB) -z defs -h $(1) -o $@
LINK_PIE        = $(LD) -z type=pie $(LINK_DYNLIB) -e _start -I $(1) \
                  $(if $(2),-R $(2)) -o $@
PIE_BEGIN       = $(PROGRAM_BEGIN)
PIE_END         = $(PROGRAM_END)
PROGRAM_BEGIN   = $(CRT_DIR)/crtbegin.o
PROGRAM_END     = $(CRT_DIR)/crtend.o
