#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-09
# SPDX-License-Identifier: MIT
#
# The illumos link-editor, from the contrib/illumos-ld submodule, built
# for the host with HOSTCC; see mk/linker-gnu.mk for what is defined.
#
# Programs are linked statically (-dn) with tools/ld/program.map, and
# Belka's own crtbegin.o and crtend.o (usr/lib/crt) mark their
# initialization arrays; the kernel is linked with kernel.map.
#

ILLUMOS_LD_SRC  := contrib/illumos-ld
ILLUMOS_LD_DIR  := build/tools/illumos-ld
LD              := $(ILLUMOS_LD_DIR)/ld
LD_DEPS         := $(LD)

LINK_KERNEL     = $(LD) -dn -e start -M $(KERNEL_MAP) -o $@
LINK_PROGRAM    = $(LD) -dn -e _start -M tools/ld/program.map -o $@
PROGRAM_BEGIN   = $(CRT_DIR)/crtbegin.o
PROGRAM_END     = $(CRT_DIR)/crtend.o

# The sub-make decides what to rebuild; ld's time changes only when it does.
$(LD): FORCE
	@test -f $(ILLUMOS_LD_SRC)/port/Makefile || { \
	    echo "$(ILLUMOS_LD_SRC): run git submodule update --init" >&2; \
	    exit 1; }
	@$(MAKE) --no-print-directory -C $(ILLUMOS_LD_SRC)/port \
	    O=$(abspath $(ILLUMOS_LD_DIR)) HOSTCC="$(HOSTCC)"
