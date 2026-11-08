/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * I/O APIC.
 *
 * Each I/O APIC has two registers in memory: a select register at
 * offset 0 and a data window at offset 0x10. Register 1 is the version,
 * whose bits 16-23 give the highest redirection entry; entry n is the
 * register pair 0x10 + 2n (low) and 0x11 + 2n (high).
 */

#ifndef _MACHINE_IOAPIC_H_
#define _MACHINE_IOAPIC_H_

#include <stdint.h>

#define IOAPIC_REGSEL           0x00
#define IOAPIC_WINDOW           0x10
#define IOAPIC_REG_ID           0x00
#define IOAPIC_REG_VER          0x01
#define IOAPIC_REDTBL(n)        (0x10 + 2 * (n))

#define IOAPIC_RTE_MASKED       0x00010000
#define IOAPIC_RTE_LEVEL        0x00008000
#define IOAPIC_RTE_ACTLOW       0x00002000
#define IOAPIC_RTE_DEST_SHIFT   24      /* in the high word */

void ioapic_init(void);
int ioapic_route(uint32_t, int, int, int, uint32_t);
void ioapic_mask(uint32_t);
void ioapic_unmask(uint32_t);
unsigned int ioapic_npins(void);

#endif /* _MACHINE_IOAPIC_H_ */
