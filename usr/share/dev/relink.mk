# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-17
# SPDX-License-Identifier: MIT
# Relink the native link-editor from installed target objects.
# Full source bootstrapping additionally requires a native C compiler.
LD = /usr/bin/ld
OUT = ld.stage2
LIB = /usr/lib64
SGS = $(LIB)/illumos-ld
ARCHIVES = $(SGS)/libld.a $(SGS)/liblddbg.a $(SGS)/libelf.a $(SGS)/libconv.a
all:
	$(LD) -64 -dy -z defs -I /usr/lib64/ld.so.1 -o $(OUT) \
	    $(LIB)/crt1.o $(LIB)/crti.o $(LIB)/crtbegin.o \
	    $(SGS)/ld.o $(SGS)/msg.o $(ARCHIVES) $(ARCHIVES) \
	    -L$(LIB) -lc $(LIB)/libcompiler_rt.a $(LIB)/crtend.o $(LIB)/crtn.o
