/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-09
 * SPDX-License-Identifier: MIT
 *
 * Belka's crtend, for programs the illumos link-editor links: linked
 * after the program's objects and libraries and before crtn.o, it marks
 * where the initialization and finalization arrays end. See crtbegin.S.
 */

        .section .preinit_array, "aw", @preinit_array
        .p2align 3
        .globl  __preinit_array_end
        .hidden __preinit_array_end
__preinit_array_end:

        .section .init_array, "aw", @init_array
        .p2align 3
        .globl  __init_array_end
        .hidden __init_array_end
__init_array_end:

        .section .fini_array, "aw", @fini_array
        .p2align 3
        .globl  __fini_array_end
        .hidden __fini_array_end
__fini_array_end:

        .section .note.GNU-stack, "", @progbits
