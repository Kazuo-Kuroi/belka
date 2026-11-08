/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * amd64 four-level page table entries.
 *
 * Level 4 is the PML4, level 3 the page directory pointer table, level 2
 * the page directory, and level 1 the page table. An entry at level 3 or
 * 2 with PTE_PS set maps a 1 GB or 2 MB page directly.
 */

#ifndef _MACHINE_PTE_H_
#define _MACHINE_PTE_H_

#include <stdint.h>

typedef uint64_t        pt_entry_t;

#define PTE_P           UINT64_C(0x0000000000000001)    /* present */
#define PTE_W           UINT64_C(0x0000000000000002)    /* writable */
#define PTE_U           UINT64_C(0x0000000000000004)    /* user */
#define PTE_PWT         UINT64_C(0x0000000000000008)    /* write-through */
#define PTE_PCD         UINT64_C(0x0000000000000010)    /* cache disable */
#define PTE_A           UINT64_C(0x0000000000000020)    /* accessed */
#define PTE_D           UINT64_C(0x0000000000000040)    /* dirty */
#define PTE_PS          UINT64_C(0x0000000000000080)    /* large page */
#define PTE_PAT4K       UINT64_C(0x0000000000000080)    /* PAT, level 1 */
#define PTE_G           UINT64_C(0x0000000000000100)    /* global */
#define PTE_PATLG       UINT64_C(0x0000000000001000)    /* PAT, large page */
#define PTE_NX          UINT64_C(0x8000000000000000)    /* no execute */

#define PTE_FRAME       UINT64_C(0x000ffffffffff000)    /* 4 KB frame */
#define PTE_LGFRAME     UINT64_C(0x000fffffffffe000)    /* large frame */

#define PT_LEVELS       4
#define PT_ENTRIES      512

/* Bit position of the index for each level. */
#define PT_SHIFT(level) (12 + 9 * ((level) - 1))
#define PT_INDEX(va, level) \
        ((unsigned int)(((va) >> PT_SHIFT(level)) & (PT_ENTRIES - 1)))
#define PT_SIZE(level)  (UINT64_C(1) << PT_SHIFT(level))

#define NBPD_1G         PT_SIZE(3)
#define NBPD_2M         PT_SIZE(2)

/*
 * The page attribute table, programmed identically on every processor.
 * It matches the layout the Limine protocol documents, so mappings the
 * boot loader made keep their meaning. Index 0 is write-back; index 3
 * (PCD and PWT) is uncached; index 5 (PAT and PWT) is write-combining.
 */
#define PAT_UC          0x00
#define PAT_WC          0x01
#define PAT_WT          0x04
#define PAT_WP          0x05
#define PAT_WB          0x06
#define PAT_UCMINUS     0x07
#define PAT_VALUE                                                       \
        ((uint64_t)PAT_WB | ((uint64_t)PAT_WT << 8) |                   \
        ((uint64_t)PAT_UCMINUS << 16) | ((uint64_t)PAT_UC << 24) |      \
        ((uint64_t)PAT_WP << 32) | ((uint64_t)PAT_WC << 40) |           \
        ((uint64_t)PAT_UCMINUS << 48) | ((uint64_t)PAT_UC << 56))

#endif /* _MACHINE_PTE_H_ */
