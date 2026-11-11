#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-09
# SPDX-License-Identifier: MIT
#
# The GNU link-editor (binutils ld), from the host.
#
# LINK_KERNEL links the kernel's objects (in order) to $@; LINK_PROGRAM
# links a static user program, given its objects; $(call LINK_DYNAMIC,
# interpreter[,run path]) links a dynamic one; $(call LINK_SHARED,soname)
# links a shared object, and LINK_PIE (as LINK_DYNAMIC) a position-
# independent program, between PIE_BEGIN and PIE_END; PROGRAM_BEGIN and
# PROGRAM_END are linked around a C program's objects and libraries.
#

LD              := ld
LD_DEPS         :=

LINK_KERNEL     = $(LD) -nostdlib -static -z max-page-size=0x1000 \
                  -z noexecstack -T $(LDSCRIPT) -o $@
LINK_PROGRAM    = $(LD) -static --build-id=none -z max-page-size=4096 \
                  -z noexecstack -e _start -o $@
# Shared objects' own dependencies are found in the sysroot.
LINK_DYNLIB     = -rpath-link $(SYSLIB)
LINK_DYNAMIC    = $(LD) --build-id=none $(LINK_DYNLIB) \
                  -z max-page-size=4096 -z noexecstack -e _start \
                  --dynamic-linker $(1) \
                  $(if $(2),-rpath $(2)) -o $@
LINK_SHARED     = $(LD) -shared --build-id=none $(LINK_DYNLIB) \
                  -z max-page-size=4096 -z noexecstack -z defs \
                  -soname $(1) -o $@
LINK_PIE        = $(LD) -pie --build-id=none $(LINK_DYNLIB) \
                  -z max-page-size=4096 -z noexecstack -z text -e _start \
                  --dynamic-linker $(1) \
                  $(if $(2),-rpath $(2)) -o $@
PIE_BEGIN       := $(LIBC_PIE_CRTBEGIN)
PIE_END         := $(LIBC_PIE_CRTEND)
PROGRAM_BEGIN   := $(LIBC_CRTBEGIN)
PROGRAM_END     := $(LIBC_CRTEND)
