#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# GCC, using the host compiler. CC (and CXX, for mlibc) may be
# overridden on the command line, for example with a cross compiler;
# GCC_VERSION, say 14, picks gcc-14 and g++-14 instead of gcc and g++.
#

ifeq ($(origin CC),default)
CC              := gcc$(if $(GCC_VERSION),-$(GCC_VERSION))
endif
ifeq ($(origin CXX),default)
CXX             := g++$(if $(GCC_VERSION),-$(GCC_VERSION))
endif
CC_INCLUDE      := $(shell $(CC) -print-file-name=include)
# Without this, GCC may turn the loops in libkern/string.c into calls to
# the very functions they implement.
TC_CFLAGS       := -fno-tree-loop-distribute-patterns

# Programs built against mlibc (see the Makefile): the compiler's own
# run-time support library, linked last, and its static start and end
# objects (__dso_handle, the .ctors and .dtors ends).
LIBC_CC         := $(CC)
LIBC_RTLIB      := $(shell $(CC) -print-libgcc-file-name)
LIBC_CRTBEGIN   := $(shell $(CC) -print-file-name=crtbeginT.o)
LIBC_CRTEND     := $(shell $(CC) -print-file-name=crtend.o)
LIBC_PIE_CRTBEGIN := $(shell $(CC) -print-file-name=crtbeginS.o)
LIBC_PIE_CRTEND := $(shell $(CC) -print-file-name=crtendS.o)
# Position-independent programs reach other objects' data through the
# GOT, not by copy relocations, which a position-independent executable
# from the illumos link-editor does not have.
LIBC_PIE_CFLAGS := -fPIE -mno-direct-extern-access
