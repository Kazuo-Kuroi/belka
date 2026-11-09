#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# Clang, targeting a bare ELF platform.
#

CC              := clang
CC_INCLUDE      := $(shell $(CC) -print-file-name=include)
TC_CFLAGS       := --target=x86_64-unknown-none-elf

# Programs built against mlibc (see the Makefile): compiler-rt's
# builtins, which clang installs for its Linux target, linked last, and
# its start and end objects (__dso_handle).
LIBC_CC         := $(CC) --target=x86_64-unknown-elf
LIBC_RTLIB      := $(shell $(CC) --target=x86_64-linux-gnu --rtlib=compiler-rt -print-libgcc-file-name)
LIBC_CRTBEGIN   := $(dir $(LIBC_RTLIB))clang_rt.crtbegin-x86_64.o
LIBC_CRTEND     := $(dir $(LIBC_RTLIB))clang_rt.crtend-x86_64.o
