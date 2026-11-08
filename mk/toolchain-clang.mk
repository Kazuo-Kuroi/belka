#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# Clang and the LLVM linker, targeting a bare ELF platform.
#

CC              := clang
LD              := ld.lld
CC_INCLUDE      := $(shell $(CC) -print-file-name=include)
TC_CFLAGS       := --target=x86_64-unknown-none-elf
