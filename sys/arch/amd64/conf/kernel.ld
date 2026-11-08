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
 * kernel_end is writable.
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
        kernel_start = .;

        .text : {
                *(.text .text.*)
        } :text

        . = ALIGN(4096);
        etext = .;
        .rodata : {
                *(.rodata .rodata.*)
        } :rodata

        . = ALIGN(4096);
        erodata = .;
        .data : {
                *(.data .data.*)
        } :data

        .bss : {
                bss_start = .;
                *(.bss .bss.*)
                *(COMMON)
                bss_end = .;
        } :data

        . = ALIGN(4096);
        kernel_end = .;

        /DISCARD/ : {
                *(.eh_frame .eh_frame_hdr)
                *(.note .note.*)
                *(.comment)
        }
}
