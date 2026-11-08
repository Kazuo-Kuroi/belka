/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Interrupt vector assignments.
 *
 *   0x00 - 0x1f        processor exceptions
 *   0x20 - 0x2f        the 8259 PICs, remapped here and masked
 *   0x30 - 0xef        device interrupts (later)
 *   0xf0 - 0xff        local APIC: timer, IPIs, error, spurious
 *
 * Vectors with higher numbers have higher priority in the local APIC.
 */

#ifndef _MACHINE_INTRDEFS_H_
#define _MACHINE_INTRDEFS_H_

#define ICU_OFFSET              0x20    /* 8259 vectors start here */

#define DEVICE_VECTOR_FIRST     0x30
#define DEVICE_VECTOR_LAST      0xef

#define LAPIC_TIMER_VECTOR      0xf0
#define IPI_ACTION_VECTOR       0xf1
#define IPI_RESCHED_VECTOR      0xf2
#define LAPIC_ERROR_VECTOR      0xfe
#define LAPIC_SPURIOUS_VECTOR   0xff

/* Stubs for vectors 32 to 255 in vector.S, 16 bytes apart. */
extern const char intr_stubs[], intr_stubs_end[];
#define INTR_STUB_SIZE          16

#endif /* _MACHINE_INTRDEFS_H_ */
