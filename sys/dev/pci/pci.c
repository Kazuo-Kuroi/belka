/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * PCI enumeration, BAR and capability discovery, MSI-X, and driver
 * matching. See <dev/pci/pcivar.h>.
 *
 * BARs are sized by the usual method: with decoding turned off, write
 * all ones, read back the writable bits, and restore the original
 * value. The addresses the firmware assigned are kept.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/intr.h>
#include <sys/systm.h>

#include <vm/hat.h>
#include <vm/kva.h>

#include "dev/pci/pcireg.h"
#include "dev/pci/pcivar.h"

#define PCI_MAXBUSDEPTH 32
#define PCI_MAXCAPS     48

extern const struct pci_driver virtio_pci_driver, e1000g_pci_driver,
    nvme_pci_driver, ahci_pci_driver;

static const struct pci_driver *const pci_drivers[] = {
        &virtio_pci_driver,
        &e1000g_pci_driver,
        &nvme_pci_driver,
        &ahci_pci_driver
};
#define PCI_NDRIVERS    (sizeof(pci_drivers) / sizeof(pci_drivers[0]))

static struct pci_dev pci_devs[PCI_MAXDEVICES];
static unsigned int pci_ndevs;

static void pci_scan_bus(int, int);
static void pci_probe_fn(int, int, int);
static void pci_size_bars(struct pci_dev *);

static void
pci_size_bars(struct pci_dev *pd)
{
        int nbars = (pd->pd_hdrtype & PCI_HDR_TYPE_MASK) == PCI_HDR_DEVICE ?
            PCI_NBARS : 2;
        uint32_t cmd = pci_conf_read(pd, PCI_COMMAND_STATUS_REG, 2);

        pci_conf_write(pd, PCI_COMMAND_STATUS_REG, 2,
            cmd & ~(uint32_t)(PCI_COMMAND_IO | PCI_COMMAND_MEM));
        for (int i = 0; i < nbars; i++) {
                int reg = PCI_BAR0 + 4 * i;
                uint32_t orig = pci_conf_read(pd, reg, 4);
                struct pci_bar *pb = &pd->pd_bar[i];
                uint32_t mask;

                pci_conf_write(pd, reg, 4, 0xffffffff);
                mask = pci_conf_read(pd, reg, 4);
                pci_conf_write(pd, reg, 4, orig);
                if (mask == 0)
                        continue;

                if ((orig & PCI_BAR_IO) != 0) {
                        mask &= PCI_BAR_IO_MASK;
                        if ((mask & 0xffff0000) == 0)
                                mask |= 0xffff0000;
                        pb->pb_io = 1;
                        pb->pb_addr = orig & PCI_BAR_IO_MASK;
                        pb->pb_size = (uint32_t)(~mask + 1);
                } else if ((orig & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_64 &&
                    i + 1 < nbars) {
                        uint32_t orighi = pci_conf_read(pd, reg + 4, 4);
                        uint32_t maskhi;
                        uint64_t m;

                        pci_conf_write(pd, reg + 4, 4, 0xffffffff);
                        maskhi = pci_conf_read(pd, reg + 4, 4);
                        pci_conf_write(pd, reg + 4, 4, orighi);
                        m = ((uint64_t)maskhi << 32) |
                            (mask & PCI_BAR_MEM_MASK);
                        pb->pb_64 = 1;
                        pb->pb_prefetch = (orig & PCI_BAR_MEM_PREFETCH) != 0;
                        pb->pb_addr = ((uint64_t)orighi << 32) |
                            (orig & PCI_BAR_MEM_MASK);
                        pb->pb_size = ~m + 1;
                        i++;            /* the upper half is not a BAR */
                } else {
                        mask &= PCI_BAR_MEM_MASK;
                        pb->pb_prefetch = (orig & PCI_BAR_MEM_PREFETCH) != 0;
                        pb->pb_addr = orig & PCI_BAR_MEM_MASK;
                        pb->pb_size = (uint32_t)(~mask + 1);
                }
        }
        pci_conf_write(pd, PCI_COMMAND_STATUS_REG, 2, cmd);
}

static void
pci_probe_fn(int bus, int dev, int fn)
{
        uint32_t id = pci_conf_read_bdf(bus, dev, fn, PCI_ID_REG, 4);
        uint32_t class, sub;
        struct pci_dev *pd;

        if ((id & 0xffff) == PCI_VENDOR_INVALID)
                return;
        if (pci_ndevs == PCI_MAXDEVICES) {
                printf("pci: more than %d functions; rest ignored\n",
                    PCI_MAXDEVICES);
                return;
        }
        pd = &pci_devs[pci_ndevs++];
        pd->pd_bus = (uint8_t)bus;
        pd->pd_dev = (uint8_t)dev;
        pd->pd_fn = (uint8_t)fn;
        pd->pd_vendor = (uint16_t)(id & 0xffff);
        pd->pd_device = (uint16_t)(id >> 16);
        class = pci_conf_read(pd, PCI_CLASS_REG, 4);
        pd->pd_revision = (uint8_t)class;
        pd->pd_progif = (uint8_t)(class >> 8);
        pd->pd_subclass = (uint8_t)(class >> 16);
        pd->pd_class = (uint8_t)(class >> 24);
        pd->pd_hdrtype = (uint8_t)(pci_conf_read(pd, PCI_BHLC_REG, 4) >> 16);
        if ((pd->pd_hdrtype & PCI_HDR_TYPE_MASK) == PCI_HDR_DEVICE) {
                sub = pci_conf_read(pd, PCI_SUBSYS_ID_REG, 4);
                pd->pd_subvendor = (uint16_t)(sub & 0xffff);
                pd->pd_subdevice = (uint16_t)(sub >> 16);
        }
        pd->pd_intpin = (uint8_t)(pci_conf_read(pd, PCI_INTERRUPT_REG, 4) >> 8);
        pci_size_bars(pd);
}

static void
pci_scan_bus(int bus, int depth)
{
        for (int dev = 0; dev < 32; dev++) {
                uint32_t id = pci_conf_read_bdf(bus, dev, 0, PCI_ID_REG, 4);
                int nfn;

                if ((id & 0xffff) == PCI_VENDOR_INVALID)
                        continue;
                nfn = (pci_conf_read_bdf(bus, dev, 0, PCI_BHLC_REG, 4) >> 16) &
                    PCI_HDR_MULTIFN ? 8 : 1;
                for (int fn = 0; fn < nfn; fn++) {
                        uint32_t hdr;

                        if ((pci_conf_read_bdf(bus, dev, fn, PCI_ID_REG, 4) &
                            0xffff) == PCI_VENDOR_INVALID)
                                continue;
                        pci_probe_fn(bus, dev, fn);
                        hdr = (pci_conf_read_bdf(bus, dev, fn, PCI_BHLC_REG,
                            4) >> 16) & PCI_HDR_TYPE_MASK;
                        if (hdr == PCI_HDR_BRIDGE && depth < PCI_MAXBUSDEPTH) {
                                int sec = (int)(pci_conf_read_bdf(bus, dev, fn,
                                    PCI_BRIDGE_BUS_REG, 4) >> 8) & 0xff;

                                if (sec > bus)
                                        pci_scan_bus(sec, depth + 1);
                        }
                }
        }
}

void
pci_init(void)
{
        (void)pci_md_init();
        pci_scan_bus(0, 0);

        for (unsigned int i = 0; i < pci_ndevs; i++) {
                struct pci_dev *pd = &pci_devs[i];

                printf("pci %u:%02u.%u: %04x:%04x class %02x.%02x.%02x\n",
                    pd->pd_bus, pd->pd_dev, pd->pd_fn, pd->pd_vendor,
                    pd->pd_device, pd->pd_class, pd->pd_subclass,
                    pd->pd_progif);
        }
        for (unsigned int i = 0; i < pci_ndevs; i++) {
                struct pci_dev *pd = &pci_devs[i];

                for (size_t d = 0; d < PCI_NDRIVERS; d++) {
                        if (!pci_drivers[d]->pdr_match(pd))
                                continue;
                        if (pci_drivers[d]->pdr_attach(pd) == 0) {
                                pd->pd_driver = pci_drivers[d]->pdr_name;
                                break;
                        }
                }
        }
}

unsigned int
pci_ndevices(void)
{
        return pci_ndevs;
}

struct pci_dev *
pci_device(unsigned int i)
{
        return i < pci_ndevs ? &pci_devs[i] : NULL;
}

/* The index'th function with this vendor and device, or NULL. */
struct pci_dev *
pci_find(uint16_t vendor, uint16_t device, int index)
{
        for (unsigned int i = 0; i < pci_ndevs; i++) {
                if (pci_devs[i].pd_vendor == vendor &&
                    pci_devs[i].pd_device == device && index-- == 0)
                        return &pci_devs[i];
        }
        return NULL;
}

int
pci_find_cap(const struct pci_dev *pd, int id, int index)
{
        int ptr, n = 0;

        if ((pci_conf_read(pd, PCI_COMMAND_STATUS_REG, 4) &
            PCI_STATUS_CAPLIST) == 0)
                return 0;
        ptr = (int)pci_conf_read(pd, PCI_CAPLIST_PTR_REG, 1) & 0xfc;
        while (ptr != 0 && n++ < PCI_MAXCAPS) {
                if ((int)pci_conf_read(pd, ptr, 1) == id && index-- == 0)
                        return ptr;
                ptr = (int)pci_conf_read(pd, ptr + 1, 1) & 0xfc;
        }
        return 0;
}

void *
pci_map_bar(const struct pci_dev *pd, int bar, uint64_t off, size_t len)
{
        const struct pci_bar *pb = &pd->pd_bar[bar];

        if (bar < 0 || bar >= PCI_NBARS || pb->pb_addr == 0 || pb->pb_io ||
            off + len > pb->pb_size)
                return NULL;
        return kva_map_phys(pb->pb_addr + off, len, HAT_WRITE | HAT_NOCACHE);
}

/* Turn on memory decoding and bus mastering, and I/O decoding if io. */
void
pci_enable(const struct pci_dev *pd, int io)
{
        uint32_t cmd = pci_conf_read(pd, PCI_COMMAND_STATUS_REG, 2);

        cmd |= PCI_COMMAND_MEM | PCI_COMMAND_MASTER;
        if (io)
                cmd |= PCI_COMMAND_IO;
        pci_conf_write(pd, PCI_COMMAND_STATUS_REG, 2, cmd);
}

int
pci_msix_count(const struct pci_dev *pd)
{
        int cap = pci_find_cap(pd, PCI_CAP_MSIX, 0);

        if (cap == 0)
                return 0;
        return (int)(pci_conf_read(pd, cap + PCI_MSIX_CTL, 2) &
            PCI_MSIX_CTL_TBLSIZE) + 1;
}

void *
pci_msix_establish(struct pci_dev *pd, int entry, int flags,
    int (*fn)(void *), void *arg, const char *name)
{
        int cap = pci_find_cap(pd, PCI_CAP_MSIX, 0);
        volatile uint32_t *e;
        uint32_t ctl, table, data;
        uint64_t addr;
        void *cookie;
        int n;

        if (cap == 0 || entry < 0 || entry >= (n = pci_msix_count(pd)))
                return NULL;
        if (pd->pd_msix_table == NULL) {
                table = pci_conf_read(pd, cap + PCI_MSIX_TABLE, 4);
                pd->pd_msix_table = pci_map_bar(pd,
                    (int)(table & PCI_MSIX_BIR_MASK),
                    table & ~(uint32_t)PCI_MSIX_BIR_MASK,
                    (size_t)n * PCI_MSIX_ENTRY_SIZE);
                if (pd->pd_msix_table == NULL)
                        return NULL;
        }
        if ((cookie = intr_establish_msi(flags, fn, arg, name, &addr,
            &data)) == NULL)
                return NULL;

        e = pd->pd_msix_table + entry * (PCI_MSIX_ENTRY_SIZE / 4);
        e[PCI_MSIX_ENTRY_VCTL / 4] |= PCI_MSIX_VCTL_MASK;
        e[PCI_MSIX_ENTRY_ADDR_LO / 4] = (uint32_t)addr;
        e[PCI_MSIX_ENTRY_ADDR_HI / 4] = (uint32_t)(addr >> 32);
        e[PCI_MSIX_ENTRY_DATA / 4] = data;
        e[PCI_MSIX_ENTRY_VCTL / 4] &= ~(uint32_t)PCI_MSIX_VCTL_MASK;

        ctl = pci_conf_read(pd, cap + PCI_MSIX_CTL, 2);
        ctl = (ctl | PCI_MSIX_CTL_ENABLE) & ~(uint32_t)PCI_MSIX_CTL_FUNCMASK;
        pci_conf_write(pd, cap + PCI_MSIX_CTL, 2, ctl);
        pci_conf_write(pd, PCI_COMMAND_STATUS_REG, 2,
            pci_conf_read(pd, PCI_COMMAND_STATUS_REG, 2) |
            PCI_COMMAND_INTX_DISABLE);
        return cookie;
}

void *
pci_msi_establish(struct pci_dev *pd, int flags, int (*fn)(void *),
    void *arg, const char *name)
{
        int cap = pci_find_cap(pd, PCI_CAP_MSI, 0);
        uint32_t ctl, data;
        uint64_t addr;
        void *cookie;

        if (cap == 0)
                return NULL;
        if ((cookie = intr_establish_msi(flags, fn, arg, name, &addr,
            &data)) == NULL)
                return NULL;
        ctl = pci_conf_read(pd, cap + PCI_MSI_CTL, 2);
        pci_conf_write(pd, cap + PCI_MSI_CTL, 2, ctl & ~(uint32_t)
            (PCI_MSI_CTL_ENABLE | PCI_MSI_CTL_MME_MASK));
        pci_conf_write(pd, cap + PCI_MSI_ADDR_LO, 4, (uint32_t)addr);
        if ((ctl & PCI_MSI_CTL_64BIT) != 0) {
                pci_conf_write(pd, cap + PCI_MSI_ADDR_HI, 4,
                    (uint32_t)(addr >> 32));
                pci_conf_write(pd, cap + PCI_MSI_DATA_64, 2, data);
        } else {
                pci_conf_write(pd, cap + PCI_MSI_DATA_32, 2, data);
        }
        pci_conf_write(pd, cap + PCI_MSI_CTL, 2,
            (ctl & ~(uint32_t)PCI_MSI_CTL_MME_MASK) | PCI_MSI_CTL_ENABLE);
        pci_conf_write(pd, PCI_COMMAND_STATUS_REG, 2,
            pci_conf_read(pd, PCI_COMMAND_STATUS_REG, 2) |
            PCI_COMMAND_INTX_DISABLE);
        return cookie;
}
