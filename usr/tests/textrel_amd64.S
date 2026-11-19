/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-18
 * SPDX-License-Identifier: MIT
 *
 * Narrow runtime relocations in executable text. The following ret also
 * detects an erroneous pointer-width store over a four-byte relocation.
 */
        .text
        .weak   dt_missing_textrel_symbol
        .globl  dt_textrel_signed
        .type   dt_textrel_signed, @function
dt_textrel_signed:
        movq    $dt_missing_textrel_symbol-1, %rax
        ret
        .globl  dt_textrel_unsigned
        .type   dt_textrel_unsigned, @function
dt_textrel_unsigned:
        movl    $dt_missing_textrel_symbol+0xffffffff, %eax
        ret
        .globl  dt_textrel_write
        .type   dt_textrel_write, @function
dt_textrel_write:
        movb    $88, (%rdi)
        ret
        .section .note.GNU-stack, "", @progbits
