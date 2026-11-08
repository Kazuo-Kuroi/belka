/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * I/O APIC driver: routes global system interrupts (GSIs) to vectors.
 *
 * Every pin starts masked. ioapic_route() programs a pin for a vector,
 * trigger mode, and polarity, delivered in physical mode to one local
 * APIC, and leaves it masked; ioapic_unmask() enables it. A spin lock
 * per I/O APIC keeps the select/window register pairs together.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/spinlock.h>
#include <sys/systm.h>

#include <machine/acpi_machdep.h>
#include <machine/ioapic.h>

#include <vm/hat.h>
#include <vm/kva.h>

struct ioapic {
        volatile uint32_t *io_regs;
        struct spinlock io_lock;
        uint32_t        io_id;
        uint32_t        io_gsi_base;
        unsigned int    io_npins;
};

static struct ioapic ioapics[MADT_MAXIOAPIC];
static unsigned int nioapics;

static uint32_t ioapic_read(struct ioapic *, unsigned int);
static void ioapic_write(struct ioapic *, unsigned int, uint32_t);
static struct ioapic *ioapic_for_gsi(uint32_t, unsigned int *);
static void ioapic_set_mask(uint32_t, int);

/* Callers hold io_lock. */
static uint32_t
ioapic_read(struct ioapic *io, unsigned int reg)
{
        io->io_regs[IOAPIC_REGSEL / 4] = reg;
        return io->io_regs[IOAPIC_WINDOW / 4];
}

static void
ioapic_write(struct ioapic *io, unsigned int reg, uint32_t value)
{
        io->io_regs[IOAPIC_REGSEL / 4] = reg;
        io->io_regs[IOAPIC_WINDOW / 4] = value;
}

static struct ioapic *
ioapic_for_gsi(uint32_t gsi, unsigned int *pinp)
{
        for (unsigned int i = 0; i < nioapics; i++) {
                struct ioapic *io = &ioapics[i];

                if (gsi >= io->io_gsi_base &&
                    gsi < io->io_gsi_base + io->io_npins) {
                        *pinp = gsi - io->io_gsi_base;
                        return io;
                }
        }
        return NULL;
}

void
ioapic_init(void)
{
        for (unsigned int i = 0; i < madt.mt_nioapic; i++) {
                struct ioapic *io = &ioapics[nioapics];
                int s;

                io->io_regs = kva_map_phys(madt.mt_ioapic[i].mi_addr, 0x20,
                    HAT_WRITE | HAT_NOCACHE);
                spinlock_init(&io->io_lock, "ioapic");
                io->io_id = madt.mt_ioapic[i].mi_id;
                io->io_gsi_base = madt.mt_ioapic[i].mi_gsi_base;

                s = spin_lock(&io->io_lock);
                io->io_npins = ((ioapic_read(io, IOAPIC_REG_VER) >> 16) &
                    0xff) + 1;
                for (unsigned int pin = 0; pin < io->io_npins; pin++) {
                        ioapic_write(io, IOAPIC_REDTBL(pin) + 1, 0);
                        ioapic_write(io, IOAPIC_REDTBL(pin),
                            IOAPIC_RTE_MASKED);
                }
                spin_unlock(&io->io_lock, s);

                printf("ioapic%u: id %u at %#jx, GSIs %u-%u\n", nioapics,
                    io->io_id, (uintmax_t)madt.mt_ioapic[i].mi_addr,
                    io->io_gsi_base, io->io_gsi_base + io->io_npins - 1);
                nioapics++;
        }
}

/*
 * Route gsi to vector on the local APIC dest, masked. Returns 0, or -1
 * if no I/O APIC serves gsi or dest does not fit physical xAPIC
 * addressing.
 */
int
ioapic_route(uint32_t gsi, int vector, int level, int active_low,
    uint32_t dest)
{
        unsigned int pin;
        struct ioapic *io = ioapic_for_gsi(gsi, &pin);
        uint32_t low = (uint32_t)vector | IOAPIC_RTE_MASKED;
        int s;

        if (io == NULL || dest > 0xff)
                return -1;
        if (level)
                low |= IOAPIC_RTE_LEVEL;
        if (active_low)
                low |= IOAPIC_RTE_ACTLOW;

        s = spin_lock(&io->io_lock);
        ioapic_write(io, IOAPIC_REDTBL(pin), IOAPIC_RTE_MASKED);
        ioapic_write(io, IOAPIC_REDTBL(pin) + 1, dest << IOAPIC_RTE_DEST_SHIFT);
        ioapic_write(io, IOAPIC_REDTBL(pin), low);
        spin_unlock(&io->io_lock, s);
        return 0;
}

static void
ioapic_set_mask(uint32_t gsi, int masked)
{
        unsigned int pin;
        struct ioapic *io = ioapic_for_gsi(gsi, &pin);
        uint32_t low;
        int s;

        if (io == NULL)
                return;
        s = spin_lock(&io->io_lock);
        low = ioapic_read(io, IOAPIC_REDTBL(pin));
        if (masked)
                low |= IOAPIC_RTE_MASKED;
        else
                low &= ~(uint32_t)IOAPIC_RTE_MASKED;
        ioapic_write(io, IOAPIC_REDTBL(pin), low);
        spin_unlock(&io->io_lock, s);
}

void
ioapic_mask(uint32_t gsi)
{
        ioapic_set_mask(gsi, 1);
}

void
ioapic_unmask(uint32_t gsi)
{
        ioapic_set_mask(gsi, 0);
}

unsigned int
ioapic_npins(void)
{
        unsigned int n = 0;

        for (unsigned int i = 0; i < nioapics; i++)
                n += ioapics[i].io_npins;
        return n;
}
