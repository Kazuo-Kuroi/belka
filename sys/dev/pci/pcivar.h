/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * PCI bus support.
 *
 * pci_init() walks the configuration space from bus 0, following
 * PCI-to-PCI bridges, and records every function with its BARs as the
 * firmware assigned them. Drivers are then matched against the devices.
 *
 * The driver interface is deliberately small for now; the plan is a
 * NetBSD-style autoconfiguration (match, attach, bus_space, bus_dma),
 * and the names here stay close to it to ease that move.
 *
 * Configuration space access is machine-dependent: pci_conf_read() and
 * pci_conf_write() below.
 */

#ifndef _DEV_PCI_PCIVAR_H_
#define _DEV_PCI_PCIVAR_H_

#include <stddef.h>
#include <stdint.h>

#include <machine/types.h>

#include "dev/pci/pcireg.h"

#define PCI_MAXDEVICES  128

struct pci_bar {
        paddr_t         pb_addr;        /* 0 if not implemented */
        uint64_t        pb_size;
        int             pb_io;          /* I/O space rather than memory */
        int             pb_64;
        int             pb_prefetch;
};

struct pci_dev {
        uint8_t         pd_bus;
        uint8_t         pd_dev;
        uint8_t         pd_fn;
        uint8_t         pd_hdrtype;
        uint16_t        pd_vendor;
        uint16_t        pd_device;
        uint16_t        pd_subvendor;
        uint16_t        pd_subdevice;
        uint8_t         pd_class;
        uint8_t         pd_subclass;
        uint8_t         pd_progif;
        uint8_t         pd_revision;
        uint8_t         pd_intpin;
        struct pci_bar  pd_bar[PCI_NBARS];
        volatile uint32_t *pd_msix_table; /* mapped on first use */
        const char      *pd_driver;     /* attached driver, or NULL */
        void            *pd_softc;
};

struct pci_driver {
        const char      *pdr_name;
        int             (*pdr_match)(const struct pci_dev *);
        int             (*pdr_attach)(struct pci_dev *);
};

/* Machine-dependent configuration space access; width is 1, 2, or 4. */
int pci_md_init(void);
uint32_t pci_conf_read(const struct pci_dev *, int, int);
void pci_conf_write(const struct pci_dev *, int, int, uint32_t);
uint32_t pci_conf_read_bdf(int, int, int, int, int);
void pci_conf_write_bdf(int, int, int, int, int, uint32_t);

void pci_init(void);
unsigned int pci_ndevices(void);
struct pci_dev *pci_device(unsigned int);
struct pci_dev *pci_find(uint16_t, uint16_t, int);

/* Find capability id; returns its config offset, or 0. */
int pci_find_cap(const struct pci_dev *, int, int);

/* Map a memory BAR region uncached; returns the virtual address. */
void *pci_map_bar(const struct pci_dev *, int, uint64_t, size_t);

void pci_enable(const struct pci_dev *, int);

/*
 * Attach handler fn(arg) to MSI-X table entry entry of dev. Returns an
 * interrupt cookie for intr_disestablish(), or NULL.
 */
void *pci_msix_establish(struct pci_dev *, int, int, int (*)(void *),
    void *, const char *);
int pci_msix_count(const struct pci_dev *);

/*
 * Attach handler fn(arg) to the device's one MSI vector, for a device
 * without MSI-X. Returns an interrupt cookie, or NULL (no MSI).
 */
void *pci_msi_establish(struct pci_dev *, int, int (*)(void *), void *,
    const char *);

/*
 * Attach handler fn(arg) to the device's INTx pin, routed by the ACPI
 * _PRT (the pin swizzled across bridges to a root bus). The line may be
 * shared: fn must say whether its device raised it. Returns a cookie,
 * or NULL if the device has no pin or ACPI does not route it.
 */
void *pci_intx_establish(struct pci_dev *, int, int (*)(void *), void *,
    const char *);

#endif /* _DEV_PCI_PCIVAR_H_ */
