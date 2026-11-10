#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-10
# SPDX-License-Identifier: MIT
#
# The illumos link-editor, from the contrib/illumos-ld submodule, built
# for the host with HOSTCC. It is Belka's link-editor: whatever LINKER
# says, it links the shared C library (mlibc's ld.so and libc.so).
#

ILLUMOS_LD_SRC  := contrib/illumos-ld
ILLUMOS_LD_DIR  := build/tools/illumos-ld
ILLUMOS_LD      := $(ILLUMOS_LD_DIR)/ld

# The sub-make decides what to rebuild; ld's time changes only when it does.
$(ILLUMOS_LD): FORCE
	@test -f $(ILLUMOS_LD_SRC)/port/Makefile || { \
	    echo "$(ILLUMOS_LD_SRC): run git submodule update --init" >&2; \
	    exit 1; }
	@$(MAKE) --no-print-directory -C $(ILLUMOS_LD_SRC)/port \
	    O=$(abspath $(ILLUMOS_LD_DIR)) HOSTCC="$(HOSTCC)"
