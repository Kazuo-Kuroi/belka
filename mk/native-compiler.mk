# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-18
# SPDX-License-Identifier: MIT
# Explicit compiler-bootstrap targets; ordinary kernel builds do not fetch GCC.
NATIVE_JOBS ?= 8
COMPILER_SRC_STAMP := build/toolchain-src/prepared.stamp
CROSS_GCC_STAMP := build/toolchain-cross-gcc/built.stamp
CROSS_RUNTIME_STAMP := build/toolchain-cross-gcc/runtime.stamp
NATIVE_AS_STAMP := build/toolchain-native-binutils/built.stamp
NATIVE_GCC_STAMP := build/toolchain-native-gcc/built.stamp

$(COMPILER_SRC_STAMP): tools/toolchain/prepare.sh tools/toolchain/sources \
        $(wildcard contrib/gcc/patches/*.patch) contrib/gcc/belka.h \
        contrib/binutils/patches/0001-belka-target.patch
	sh tools/toolchain/prepare.sh

$(CROSS_GCC_STAMP): $(COMPILER_SRC_STAMP) $(MLIBC) $(ILLUMOS_LD) \
        $(PROGRAM_BEGIN) $(PROGRAM_END) usr/lib/crt/crtbegin.S usr/lib/crt/crtend.S \
        tools/toolchain/build-cross.sh
	+JOBS=$(NATIVE_JOBS) sh tools/toolchain/build-cross.sh
	touch $@

$(CROSS_RUNTIME_STAMP): $(CROSS_GCC_STAMP) tools/toolchain/build-runtime.sh
	+JOBS=$(NATIVE_JOBS) sh tools/toolchain/build-runtime.sh
	touch $@

$(NATIVE_AS_STAMP): $(COMPILER_SRC_STAMP) $(SYS_DYNDEPS) \
        tools/toolchain/build-native-as.sh tools/ld/belka-cc
	+$(BELKA_CC_ENV) JOBS=$(NATIVE_JOBS) sh tools/toolchain/build-native-as.sh
	touch $@

$(NATIVE_GCC_STAMP): $(CROSS_RUNTIME_STAMP) tools/toolchain/build-native-gcc.sh
	+JOBS=$(NATIVE_JOBS) sh tools/toolchain/build-native-gcc.sh
	touch $@

cross-compiler: $(CROSS_RUNTIME_STAMP)

# Packaging is explicit. Ordinary image builds only consume existing stamps;
# libc rebuilds must not silently launch a compiler bootstrap through ROOT_DEPS.
native-assembler: $(NATIVE_AS_STAMP)
	sh tools/toolchain/package-as.sh

native-compiler: $(NATIVE_GCC_STAMP) $(NATIVE_AS_STAMP)
	sh tools/toolchain/package-compiler.sh

test-native-compiler: native-compiler
	+$(MAKE) QEMU_TIMEOUT=600 test-boot

.PHONY: test-native-compiler cross-compiler native-assembler native-compiler
