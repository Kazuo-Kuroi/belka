/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-09
 * SPDX-License-Identifier: MIT
 *
 * Linked after every other kernel object: its empty .bss marks the end
 * of the kernel image (<machine/md_var.h>). See kernel_begin.S.
 */

        .section .bss, "aw", @nobits
        .globl  kernel_end
kernel_end:

        .section .note.GNU-stack, "", @progbits
