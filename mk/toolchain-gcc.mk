#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# GCC and GNU binutils, using the host compiler. CC may be overridden
# on the command line, for example with a cross compiler.
#

ifeq ($(origin CC),default)
CC              := gcc
endif
LD              := ld
CC_INCLUDE      := $(shell $(CC) -print-file-name=include)
# Without this, GCC may turn the loops in libkern/string.c into calls to
# the very functions they implement.
TC_CFLAGS       := -fno-tree-loop-distribute-patterns

# Programs built against mlibc (see the Makefile): the compiler's own
# run-time support library, linked last, and its static start and end
# objects (__dso_handle, the .ctors and .dtors ends).
LIBC_CC         := $(CC)
LIBC_LDFLAGS_TC := -no-pie
LIBC_RTLIB      := $(shell $(CC) -print-libgcc-file-name)
LIBC_CRTBEGIN   := $(shell $(CC) -print-file-name=crtbeginT.o)
LIBC_CRTEND     := $(shell $(CC) -print-file-name=crtend.o)
