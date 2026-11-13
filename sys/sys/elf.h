/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * The ELF-64 structures exec needs, from the published ELF and AMD64
 * psABI specifications.
 */

#ifndef _SYS_ELF_H_
#define _SYS_ELF_H_

#include <stdint.h>

#define EI_NIDENT       16
#define EI_CLASS        4
#define EI_DATA         5
#define EI_VERSION      6
#define ELFCLASS64      2
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ET_EXEC         2
#define ET_DYN          3
#define EM_X86_64       62

#define PT_NULL         0
#define PT_LOAD         1
#define PT_INTERP       3
#define PT_PHDR         6

#define PF_X            0x1
#define PF_W            0x2
#define PF_R            0x4

/* Auxiliary vector types, as in System V. */
#define AT_NULL         0
#define AT_IGNORE       1
#define AT_EXECFD       2
#define AT_PHDR         3
#define AT_PHENT        4
#define AT_PHNUM        5
#define AT_PAGESZ       6
#define AT_BASE         7
#define AT_FLAGS        8
#define AT_ENTRY        9
/*
 * Not System V's: the C library's (Linux's) numbers. AT_SECURE: the
 * program runs with ids it was not started with (set-ID).
 */
#define AT_SECURE       23
#define AT_EXECFN       31

typedef struct {
        unsigned char   e_ident[EI_NIDENT];
        uint16_t        e_type;
        uint16_t        e_machine;
        uint32_t        e_version;
        uint64_t        e_entry;
        uint64_t        e_phoff;
        uint64_t        e_shoff;
        uint32_t        e_flags;
        uint16_t        e_ehsize;
        uint16_t        e_phentsize;
        uint16_t        e_phnum;
        uint16_t        e_shentsize;
        uint16_t        e_shnum;
        uint16_t        e_shstrndx;
} Elf64_Ehdr;

typedef struct {
        uint32_t        p_type;
        uint32_t        p_flags;
        uint64_t        p_offset;
        uint64_t        p_vaddr;
        uint64_t        p_paddr;
        uint64_t        p_filesz;
        uint64_t        p_memsz;
        uint64_t        p_align;
} Elf64_Phdr;

typedef struct {
        uint64_t        a_type;
        uint64_t        a_val;
} Elf64_auxv_t;

#endif /* _SYS_ELF_H_ */
