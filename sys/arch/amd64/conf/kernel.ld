/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 *
 * Link map for the amd64 kernel. The Limine protocol requires the kernel
 * to be linked in the top 2 GB of the address space. Text, read-only
 * data, and writable data get separate page-aligned segments so that
 * each can be mapped with the narrowest permissions: kernel_start to
 * etext is executable, etext to erodata is read-only, and erodata to
 * kernel_end is writable. Those symbols come from kernel_begin.S and
 * kernel_end.S, linked first and last. This is the GNU ld map; the
 * illumos link-editor uses kernel.map.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(start)

PHDRS
{
        text    PT_LOAD FLAGS(5);       /* R-X */
        rodata  PT_LOAD FLAGS(4);       /* R-- */
        data    PT_LOAD FLAGS(6);       /* RW- */
}

SECTIONS
{
        . = 0xffffffff80000000;

        .text : {
                *(.text .text.*)
        } :text

        . = ALIGN(4096);
        .rodata : {
                *(.rodata .rodata.*)
        } :rodata

        . = ALIGN(4096);
        .data : {
                *(.data .data.*)
        } :data

        .bss : {
                *(.bss .bss.*)
                *(COMMON)
        } :data

        /DISCARD/ : {
                *(.eh_frame .eh_frame_hdr)
                *(.note .note.*)
                *(.comment)
        }
}
