/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * What amd64 takes from the ACPI tables: the interrupt controllers and
 * ISA interrupt overrides from the MADT, and the reset register from
 * the FADT.
 */

#ifndef _MACHINE_ACPI_MACHDEP_H_
#define _MACHINE_ACPI_MACHDEP_H_

#include <stdint.h>

#include <machine/param.h>
#include <machine/types.h>

#define MADT_MAXIOAPIC  8
#define MADT_MAXOVERRIDE 16
#define ISA_NIRQ        16

struct madt_ioapic {
        uint32_t        mi_id;
        paddr_t         mi_addr;
        uint32_t        mi_gsi_base;
};

/* MPS INTI flags, as the MADT gives them. */
#define MPS_POLARITY_MASK       0x0003
#define MPS_POLARITY_HIGH       0x0001
#define MPS_POLARITY_LOW        0x0003
#define MPS_TRIGGER_MASK        0x000c
#define MPS_TRIGGER_EDGE        0x0004
#define MPS_TRIGGER_LEVEL       0x000c

struct madt_override {
        uint8_t         mo_irq;
        uint32_t        mo_gsi;
        uint16_t        mo_flags;
};

struct madt_info {
        int             mt_valid;
        int             mt_pcat_compat; /* legacy 8259s present */
        paddr_t         mt_lapic_addr;
        unsigned int    mt_nlapic;      /* enabled processors */
        uint32_t        mt_lapic_id[MAXCPUS];
        unsigned int    mt_nioapic;
        struct madt_ioapic mt_ioapic[MADT_MAXIOAPIC];
        unsigned int    mt_noverride;
        struct madt_override mt_override[MADT_MAXOVERRIDE];
};

extern struct madt_info madt;

/* Parse the MADT and FADT; called after acpi_init(). */
void acpi_md_init(void);

/*
 * Map ISA IRQ irq to its global system interrupt, applying any MADT
 * override, and report whether it is level-triggered and active low.
 */
uint32_t isa_irq_to_gsi(int, int *, int *);

/* Reset the machine through the FADT reset register; returns if absent. */
void acpi_reset(void);

#endif /* _MACHINE_ACPI_MACHDEP_H_ */
