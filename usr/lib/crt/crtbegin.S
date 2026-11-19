/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-09
 * SPDX-License-Identifier: MIT
 *
 * Belka's crtbegin, for programs the illumos link-editor links (the GNU
 * one defines these symbols itself, and uses the compiler's crtbegin):
 * linked after crti.o and before the program's own objects, it marks
 * where the initialization and finalization arrays begin, which the C
 * library runs, and defines __dso_handle, which C++ destructors name.
 *
 * Its array sections take priority 0, so that the link-editor, which
 * orders them by priority, puts them before every other.
 */

        .section .preinit_array, "aw", @preinit_array
        .p2align 3
        .globl  __preinit_array_start
        .hidden __preinit_array_start
__preinit_array_start:

        .section .init_array.0, "aw", @init_array
        .p2align 3
        .globl  __init_array_start
        .hidden __init_array_start
__init_array_start:

        .section .fini_array.0, "aw", @fini_array
        .p2align 3
        .globl  __fini_array_start
        .hidden __fini_array_start
__fini_array_start:

#ifndef BELKA_CRT_ARRAYS_ONLY
        .section .data
        .p2align 3
        .globl  __dso_handle
        .hidden __dso_handle
__dso_handle:
        .quad   0
#endif

        .section .note.GNU-stack, "", @progbits
