/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * The amd64 task state segment.
 *
 * The hardware layout puts 64-bit fields at 4-byte offsets, which a C
 * structure cannot express without packing. The TSS is therefore kept
 * as an array of 32-bit words and its fields are written through
 * tss_set64(). Word indices:
 *
 *   1-2    rsp0, the stack loaded on entry from user mode
 *   3-6    rsp1, rsp2 (unused)
 *   9-22   ist1 to ist7, stacks selected by IDT gates
 *   25     I/O map base in the high 16 bits
 */

#ifndef _MACHINE_TSS_H_
#define _MACHINE_TSS_H_

#include <stdint.h>

#define TSS_WORDS       26
#define TSS_SIZE        (TSS_WORDS * 4)         /* 104 bytes */

#define TSS_RSP0        1
#define TSS_IST(n)      (9 + 2 * ((n) - 1))     /* n from 1 to 7 */
#define TSS_IOBASE      25

static inline void
tss_set64(uint32_t *tss, int word, uint64_t value)
{
        tss[word] = (uint32_t)value;
        tss[word + 1] = (uint32_t)(value >> 32);
}

#endif /* _MACHINE_TSS_H_ */
