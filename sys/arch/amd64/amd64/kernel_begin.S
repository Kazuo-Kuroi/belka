/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-09
 * SPDX-License-Identifier: MIT
 *
 * Linked before every other kernel object, so that its empty sections
 * mark where the kernel's text, read-only data, and data begin
 * (<machine/md_var.h>). The text ends where the read-only data begins,
 * and that ends where the data begins: each is a page-aligned segment.
 * tools/check-kernel.sh checks the result.
 */

        .section .text, "ax", @progbits
        .globl  kernel_start
kernel_start:

        .section .rodata, "a", @progbits
        .globl  etext
etext:

        .section .data, "aw", @progbits
        .globl  erodata
erodata:

        .section .note.GNU-stack, "", @progbits
