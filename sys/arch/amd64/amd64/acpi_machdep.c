/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * amd64 use of the ACPI MADT and FADT.
 *
 * MADT ("APIC") layout from the ACPI specification: the local APIC
 * address at offset 36, flags at 40 (bit 0: 8259s present), then
 * variable-length entries, each starting with a type byte and a length
 * byte:
 *
 *   0  processor local APIC: APIC id at 3, flags at 4 (bit 0 enabled,
 *      bit 1 online capable)
 *   1  I/O APIC: id at 2, address at 4, GSI base at 8
 *   2  interrupt source override: bus at 2, IRQ at 3, GSI at 4,
 *      flags at 8
 *   5  local APIC address override: address at 4
 *   9  processor local x2APIC: x2APIC id at 4, flags at 8
 *
 * FADT ("FACP"): flags at 112 (bit 10: reset register supported), the
 * reset register as a generic address structure at 116 (address space
 * at 116, address at 120), and the reset value at 128.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/systm.h>

#include <machine/acpi_machdep.h>
#include <machine/cpufunc.h>

#include <vm/hat.h>
#include <vm/kva.h>

#include "dev/acpi/acpivar.h"

#define FADT_FLAGS              112
#define FADT_RESET_REG          116
#define FADT_RESET_VALUE        128
#define FADT_RESET_REG_SUP      0x00000400

#define GAS_SPACE_MEMORY        0
#define GAS_SPACE_IO            1

struct madt_info madt;

static int fadt_reset_ok;
static uint8_t fadt_reset_space;
static uint64_t fadt_reset_addr;
static uint8_t fadt_reset_value;

static void madt_parse(const uint8_t *, size_t);
static void madt_add_lapic(uint32_t, uint32_t);
static void fadt_parse(const uint8_t *, size_t);

static void
madt_add_lapic(uint32_t id, uint32_t flags)
{
        /* Count processors that are enabled now. */
        if ((flags & 1) == 0)
                return;
        if (madt.mt_nlapic < MAXCPUS)
                madt.mt_lapic_id[madt.mt_nlapic] = id;
        madt.mt_nlapic++;
}

static void
madt_parse(const uint8_t *t, size_t len)
{
        madt.mt_lapic_addr = acpi_le32(t + 36);
        madt.mt_pcat_compat = (acpi_le32(t + 40) & 1) != 0;

        for (size_t off = 44; off + 2 <= len; ) {
                const uint8_t *e = t + off;
                uint8_t type = e[0], elen = e[1];

                if (elen < 2 || off + elen > len)
                        break;
                switch (type) {
                case 0:
                        if (elen >= 8)
                                madt_add_lapic(e[3], acpi_le32(e + 4));
                        break;
                case 1:
                        if (elen >= 12 && madt.mt_nioapic < MADT_MAXIOAPIC) {
                                struct madt_ioapic *io =
                                    &madt.mt_ioapic[madt.mt_nioapic++];

                                io->mi_id = e[2];
                                io->mi_addr = acpi_le32(e + 4);
                                io->mi_gsi_base = acpi_le32(e + 8);
                        }
                        break;
                case 2:
                        if (elen >= 10 && e[2] == 0 &&
                            madt.mt_noverride < MADT_MAXOVERRIDE) {
                                struct madt_override *o =
                                    &madt.mt_override[madt.mt_noverride++];

                                o->mo_irq = e[3];
                                o->mo_gsi = acpi_le32(e + 4);
                                o->mo_flags = acpi_le16(e + 8);
                        }
                        break;
                case 5:
                        if (elen >= 12)
                                madt.mt_lapic_addr = acpi_le64(e + 4);
                        break;
                case 9:
                        if (elen >= 16)
                                madt_add_lapic(acpi_le32(e + 4),
                                    acpi_le32(e + 8));
                        break;
                default:
                        break;
                }
                off += elen;
        }
        madt.mt_valid = 1;
}

static void
fadt_parse(const uint8_t *t, size_t len)
{
        if (len <= FADT_RESET_VALUE)
                return;
        if ((acpi_le32(t + FADT_FLAGS) & FADT_RESET_REG_SUP) == 0)
                return;
        fadt_reset_space = t[FADT_RESET_REG];
        fadt_reset_addr = acpi_le64(t + FADT_RESET_REG + 4);
        fadt_reset_value = t[FADT_RESET_VALUE];
        if (fadt_reset_addr != 0 && (fadt_reset_space == GAS_SPACE_IO ||
            fadt_reset_space == GAS_SPACE_MEMORY))
                fadt_reset_ok = 1;
}

void
acpi_md_init(void)
{
        const uint8_t *t;
        size_t len;

        if ((t = acpi_find_table("APIC", 0, &len)) != NULL)
                madt_parse(t, len);
        if ((t = acpi_find_table("FACP", 0, &len)) != NULL)
                fadt_parse(t, len);

        if (madt.mt_valid) {
                printf("acpi: MADT: %u processors, %u I/O APICs, "
                    "%u overrides%s\n", madt.mt_nlapic, madt.mt_nioapic,
                    madt.mt_noverride,
                    madt.mt_pcat_compat ? ", 8259s present" : "");
                for (unsigned int i = 0; i < madt.mt_noverride; i++) {
                        const struct madt_override *o = &madt.mt_override[i];

                        printf("acpi:   IRQ %u -> GSI %u, %s, active %s\n",
                            o->mo_irq, o->mo_gsi,
                            (o->mo_flags & MPS_TRIGGER_MASK) ==
                            MPS_TRIGGER_LEVEL ? "level" : "edge",
                            (o->mo_flags & MPS_POLARITY_MASK) ==
                            MPS_POLARITY_LOW ? "low" : "high");
                }
        } else {
                printf("acpi: no MADT\n");
        }
        if (fadt_reset_ok)
                printf("acpi: reset register %s %#jx, value %#x\n",
                    fadt_reset_space == GAS_SPACE_IO ? "port" : "memory",
                    (uintmax_t)fadt_reset_addr, fadt_reset_value);
}

/*
 * ISA interrupts are edge-triggered and active high, on the GSI of the
 * same number, unless the MADT overrides them. A "conforms" field in an
 * override keeps the ISA default for that property.
 */
uint32_t
isa_irq_to_gsi(int irq, int *levelp, int *lowp)
{
        *levelp = 0;
        *lowp = 0;
        for (unsigned int i = 0; i < madt.mt_noverride; i++) {
                const struct madt_override *o = &madt.mt_override[i];

                if (o->mo_irq != irq)
                        continue;
                *levelp = (o->mo_flags & MPS_TRIGGER_MASK) ==
                    MPS_TRIGGER_LEVEL;
                *lowp = (o->mo_flags & MPS_POLARITY_MASK) ==
                    MPS_POLARITY_LOW;
                return o->mo_gsi;
        }
        return (uint32_t)irq;
}

void
acpi_reset(void)
{
        if (!fadt_reset_ok)
                return;
        if (fadt_reset_space == GAS_SPACE_IO) {
                outb((uint16_t)fadt_reset_addr, fadt_reset_value);
        } else {
                volatile uint8_t *reg = kva_map_phys(fadt_reset_addr, 1,
                    HAT_WRITE | HAT_NOCACHE);

                *reg = fadt_reset_value;
        }
        /* Give the reset a moment to take effect. */
        for (volatile int i = 0; i < 1000000; i++)
                continue;
}
