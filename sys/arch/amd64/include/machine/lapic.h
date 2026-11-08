/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Local APIC.
 *
 * The driver uses x2APIC mode, with registers reached through MSRs,
 * whenever the bootstrap processor supports it, and otherwise xAPIC
 * mode, with registers in an uncached KVA mapping. Every processor uses
 * the same mode. Register offsets below are the xAPIC ones; in x2APIC
 * mode register r is MSR 0x800 + r / 16.
 */

#ifndef _MACHINE_LAPIC_H_
#define _MACHINE_LAPIC_H_

#include <stdint.h>

#define LAPIC_ID                0x020
#define LAPIC_VERSION           0x030
#define LAPIC_TPR               0x080
#define LAPIC_EOI               0x0b0
#define LAPIC_SVR               0x0f0
#define LAPIC_ESR               0x280
#define LAPIC_ICR_LO            0x300
#define LAPIC_ICR_HI            0x310
#define LAPIC_LVT_TIMER         0x320
#define LAPIC_LVT_THERMAL       0x330
#define LAPIC_LVT_PERF          0x340
#define LAPIC_LVT_LINT0         0x350
#define LAPIC_LVT_LINT1         0x360
#define LAPIC_LVT_ERROR         0x370
#define LAPIC_TIMER_ICR         0x380
#define LAPIC_TIMER_CCR         0x390
#define LAPIC_TIMER_DCR         0x3e0

#define LAPIC_SVR_ENABLE        0x00000100

#define LAPIC_LVT_MASKED        0x00010000
#define LAPIC_LVT_PERIODIC      0x00020000
#define LAPIC_LVT_NMI           0x00000400      /* delivery mode NMI */

#define LAPIC_DCR_DIV16         0x3

#define LAPIC_ICR_FIXED         0x00000000
#define LAPIC_ICR_NMI           0x00000400
#define LAPIC_ICR_PENDING       0x00001000      /* xAPIC delivery status */
#define LAPIC_ICR_ASSERT        0x00004000
#define LAPIC_ICR_ALLEXCL       0x000c0000      /* all but self */

#define MSR_APIC_BASE           0x0000001b
#define APIC_BASE_ENABLE        0x00000800
#define APIC_BASE_X2APIC        0x00000400
#define APIC_BASE_ADDR          UINT64_C(0x000ffffffffff000)
#define MSR_X2APIC_BASE         0x00000800

#define CPUID_1_ECX_X2APIC      0x00200000
#define CPUID_80000007_EDX_INVTSC 0x00000100

void lapic_init(void);
void lapic_cpu_init(void);
uint32_t lapic_id(void);
void lapic_eoi(void);
void lapic_send_ipi(uint32_t, int);
void lapic_send_nmi_others(void);
int lapic_ready(void);
int lapic_is_x2apic(void);

void clock_calibrate(void);
void lapic_timer_start(void);

/* Time-stamp counter frequency in Hz, set by clock_calibrate(). */
extern uint64_t tsc_freq;

#endif /* _MACHINE_LAPIC_H_ */
