/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * amd64 PCI configuration space access.
 *
 * With an ACPI MCFG table, configuration space is memory-mapped (ECAM):
 * function (bus, dev, fn) has 4 KB at base + (bus << 20 | dev << 15 |
 * fn << 12). Each bus's megabyte is mapped uncached the first time it is
 * used. Without MCFG, the legacy mechanism through ports 0xcf8 and
 * 0xcfc is used, which reaches only the first 256 bytes of each
 * function.
 *
 * MCFG layout (PCI Firmware Specification): 8 reserved bytes after the
 * header, then 16-byte entries: base address at 0, segment at 8, first
 * bus at 10, last bus at 11. Only segment 0 is used.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/spinlock.h>
#include <sys/systm.h>

#include <machine/cpufunc.h>

#include <vm/hat.h>
#include <vm/kva.h>

#include "dev/acpi/acpivar.h"
#include "dev/pci/pcivar.h"

#define PCI_CF8         0x0cf8
#define PCI_CFC         0x0cfc

#define ECAM_BUS_SIZE   (1u << 20)

static paddr_t ecam_base;
static int ecam_first_bus, ecam_last_bus;
static volatile uint8_t *ecam_bus_va[256];
static struct spinlock pci_conf_lock = SPINLOCK_INITIALIZER("pciconf");

static volatile uint8_t *ecam_addr(int, int, int, int);

/* Returns 1 if ECAM is in use, 0 for the legacy mechanism. */
int
pci_md_init(void)
{
        const uint8_t *t;
        size_t len;

        if ((t = acpi_find_table("MCFG", 0, &len)) != NULL) {
                for (size_t off = 44; off + 16 <= len; off += 16) {
                        if (acpi_le16(t + off + 8) != 0)
                                continue;
                        ecam_base = acpi_le64(t + off);
                        ecam_first_bus = t[off + 10];
                        ecam_last_bus = t[off + 11];
                        break;
                }
        }
        if (ecam_base != 0) {
                printf("pci: ECAM at %#jx, buses %d-%d\n",
                    (uintmax_t)ecam_base, ecam_first_bus, ecam_last_bus);
                return 1;
        }
        printf("pci: no MCFG, using configuration mechanism 1\n");
        return 0;
}

/* The ECAM address of a register, mapping its bus on first use. */
static volatile uint8_t *
ecam_addr(int bus, int dev, int fn, int reg)
{
        if (bus < ecam_first_bus || bus > ecam_last_bus)
                return NULL;
        if (ecam_bus_va[bus] == NULL) {
                void *va = kva_map_phys(ecam_base +
                    (paddr_t)(bus - ecam_first_bus) * ECAM_BUS_SIZE,
                    ECAM_BUS_SIZE, HAT_WRITE | HAT_NOCACHE);
                int s = spin_lock(&pci_conf_lock);

                if (ecam_bus_va[bus] == NULL) {
                        ecam_bus_va[bus] = va;
                        va = NULL;
                }
                spin_unlock(&pci_conf_lock, s);
                if (va != NULL)
                        kva_unmap_phys(va, ECAM_BUS_SIZE);
        }
        return ecam_bus_va[bus] + ((uint32_t)dev << 15) +
            ((uint32_t)fn << 12) + (uint32_t)reg;
}

uint32_t
pci_conf_read_bdf(int bus, int dev, int fn, int reg, int width)
{
        uint32_t v;
        int s;

        if (ecam_base != 0) {
                volatile uint8_t *p = ecam_addr(bus, dev, fn, reg);

                if (p == NULL)
                        return 0xffffffff;
                switch (width) {
                case 1:
                        return *p;
                case 2:
                        return *(volatile uint16_t *)(volatile void *)p;
                default:
                        return *(volatile uint32_t *)(volatile void *)p;
                }
        }
        if (reg >= 256)
                return 0xffffffff;
        s = spin_lock(&pci_conf_lock);
        outl(PCI_CF8, 0x80000000u | ((uint32_t)bus << 16) |
            ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
            ((uint32_t)reg & 0xfc));
        v = inl(PCI_CFC);
        spin_unlock(&pci_conf_lock, s);
        v >>= (reg & 3) * 8;
        if (width == 1)
                v &= 0xff;
        else if (width == 2)
                v &= 0xffff;
        return v;
}

void
pci_conf_write_bdf(int bus, int dev, int fn, int reg, int width,
    uint32_t value)
{
        int s;

        if (ecam_base != 0) {
                volatile uint8_t *p = ecam_addr(bus, dev, fn, reg);

                if (p == NULL)
                        return;
                switch (width) {
                case 1:
                        *p = (uint8_t)value;
                        break;
                case 2:
                        *(volatile uint16_t *)(volatile void *)p =
                            (uint16_t)value;
                        break;
                default:
                        *(volatile uint32_t *)(volatile void *)p = value;
                        break;
                }
                return;
        }
        if (reg >= 256)
                return;
        s = spin_lock(&pci_conf_lock);
        outl(PCI_CF8, 0x80000000u | ((uint32_t)bus << 16) |
            ((uint32_t)dev << 11) | ((uint32_t)fn << 8) |
            ((uint32_t)reg & 0xfc));
        if (width == 4) {
                outl(PCI_CFC, value);
        } else {
                uint32_t shift = (uint32_t)(reg & 3) * 8;
                uint32_t mask = (width == 1 ? 0xffu : 0xffffu) << shift;
                uint32_t v = inl(PCI_CFC);

                outl(PCI_CFC, (v & ~mask) | ((value << shift) & mask));
        }
        spin_unlock(&pci_conf_lock, s);
}

uint32_t
pci_conf_read(const struct pci_dev *pd, int reg, int width)
{
        return pci_conf_read_bdf(pd->pd_bus, pd->pd_dev, pd->pd_fn, reg,
            width);
}

void
pci_conf_write(const struct pci_dev *pd, int reg, int width, uint32_t value)
{
        pci_conf_write_bdf(pd->pd_bus, pd->pd_dev, pd->pd_fn, reg, width,
            value);
}
