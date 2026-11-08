#
# Copyright (c) 2026 Setsuna Software L.C.
# Created: 2026-11-07
# SPDX-License-Identifier: MIT
#
# Source lists and machine-dependent flags for the amd64 kernel.
#

MD_CFLAGS       := -mcmodel=kernel -mno-red-zone \
                   -mno-mmx -mno-sse -mno-sse2 -mno-avx
MD_ASFLAGS      :=

LDSCRIPT        := sys/arch/amd64/conf/kernel.ld
LIMINE_CONF     := sys/arch/amd64/conf/limine.conf
LIMINE_TEST_CONF := sys/arch/amd64/conf/limine-test.conf

MI_SRCS         := sys/kern/init_main.c \
                   sys/kern/kern_cons.c \
                   sys/kern/subr_prf.c \
                   sys/kern/subr_selftest.c \
                   sys/libkern/string.c \
                   sys/dev/ic/ns16550.c

MD_SRCS         := sys/arch/amd64/amd64/locore.S \
                   sys/arch/amd64/amd64/atomic.S \
                   sys/arch/amd64/amd64/cpufunc.S \
                   sys/arch/amd64/amd64/limine_boot.c \
                   sys/arch/amd64/amd64/machdep.c
