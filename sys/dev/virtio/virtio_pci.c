/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Virtio over PCI, the "modern" (virtio 1.x) interface only.
 *
 * The device's vendor-specific capabilities locate its register blocks
 * in its BARs: the common configuration, the notification area, and the
 * device-specific configuration. Each queue interrupts through its own
 * MSI-X entry, equal to its index; configuration change interrupts are
 * not used. Transitional devices (ids 0x1000-0x103f) work through the
 * same modern interface; legacy-only devices are refused.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/systm.h>

#include <machine/vmparam.h>

#include "dev/pci/pcivar.h"
#include "dev/virtio/virtioreg.h"
#include "dev/virtio/virtiovar.h"

#define VIRTIO_PCI_VENDOR       0x1af4
#define VIRTIO_PCI_DEV_FIRST    0x1000
#define VIRTIO_PCI_DEV_MODERN   0x1040
#define VIRTIO_PCI_DEV_LAST     0x107f

static int virtio_pci_match(const struct pci_dev *);
static int virtio_pci_attach(struct pci_dev *);
static void *virtio_pci_map_cap(struct pci_dev *, int, uint32_t *,
    uint32_t *);

const struct pci_driver virtio_pci_driver = {
        "virtio", virtio_pci_match, virtio_pci_attach
};

static uint8_t
common_read8(struct virtio_softc *sc, int off)
{
        return sc->vs_common[off];
}

static void
common_write8(struct virtio_softc *sc, int off, uint8_t v)
{
        sc->vs_common[off] = v;
}

static uint16_t
common_read16(struct virtio_softc *sc, int off)
{
        return *(volatile uint16_t *)(volatile void *)(sc->vs_common + off);
}

static void
common_write16(struct virtio_softc *sc, int off, uint16_t v)
{
        *(volatile uint16_t *)(volatile void *)(sc->vs_common + off) = v;
}

static uint32_t
common_read32(struct virtio_softc *sc, int off)
{
        return *(volatile uint32_t *)(volatile void *)(sc->vs_common + off);
}

static void
common_write32(struct virtio_softc *sc, int off, uint32_t v)
{
        *(volatile uint32_t *)(volatile void *)(sc->vs_common + off) = v;
}

static void
common_write64(struct virtio_softc *sc, int off, uint64_t v)
{
        common_write32(sc, off, (uint32_t)v);
        common_write32(sc, off + 4, (uint32_t)(v >> 32));
}

static int
virtio_pci_match(const struct pci_dev *pd)
{
        return pd->pd_vendor == VIRTIO_PCI_VENDOR &&
            pd->pd_device >= VIRTIO_PCI_DEV_FIRST &&
            pd->pd_device <= VIRTIO_PCI_DEV_LAST;
}

/*
 * Map the region of the first vendor capability of type cfg_type.
 * Stores the capability's config offset and the region length.
 */
static void *
virtio_pci_map_cap(struct pci_dev *pd, int cfg_type, uint32_t *capp,
    uint32_t *lenp)
{
        for (int i = 0; ; i++) {
                int cap = pci_find_cap(pd, PCI_CAP_VENDOR, i);
                int bar;
                uint32_t off, len;

                if (cap == 0)
                        return NULL;
                if ((int)pci_conf_read(pd, cap + VIRTIO_PCI_CAP_TYPE, 1) !=
                    cfg_type)
                        continue;
                bar = (int)pci_conf_read(pd, cap + VIRTIO_PCI_CAP_BAR, 1);
                off = pci_conf_read(pd, cap + VIRTIO_PCI_CAP_OFFSET, 4);
                len = pci_conf_read(pd, cap + VIRTIO_PCI_CAP_LENGTH, 4);
                if (bar >= PCI_NBARS || len == 0)
                        continue;
                *capp = (uint32_t)cap;
                *lenp = len;
                return pci_map_bar(pd, bar, off, len);
        }
}

static int
virtio_pci_attach(struct pci_dev *pd)
{
        struct virtio_softc *sc;
        uint32_t cap, len;
        int type;

        type = pd->pd_device >= VIRTIO_PCI_DEV_MODERN ?
            pd->pd_device - VIRTIO_PCI_DEV_MODERN : pd->pd_subdevice;

        sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
        sc->vs_pci = pd;
        sc->vs_type = type;
        sc->vs_common = virtio_pci_map_cap(pd, VIRTIO_PCI_CAP_COMMON_CFG,
            &cap, &len);
        sc->vs_notify_base = virtio_pci_map_cap(pd,
            VIRTIO_PCI_CAP_NOTIFY_CFG, &cap, &len);
        if (sc->vs_notify_base != NULL)
                sc->vs_notify_mult = pci_conf_read(pd,
                    (int)cap + VIRTIO_PCI_NOTIFY_MULT, 4);
        sc->vs_devcfg = virtio_pci_map_cap(pd, VIRTIO_PCI_CAP_DEVICE_CFG,
            &cap, &len);
        if (sc->vs_common == NULL || sc->vs_notify_base == NULL) {
                printf("virtio %u:%02u.%u: no modern interface; legacy "
                    "virtio is not supported\n", pd->pd_bus, pd->pd_dev,
                    pd->pd_fn);
                kmem_free(sc, sizeof(*sc));
                return -1;
        }
        if (pci_msix_count(pd) == 0) {
                printf("virtio %u:%02u.%u: no MSI-X\n", pd->pd_bus,
                    pd->pd_dev, pd->pd_fn);
                kmem_free(sc, sizeof(*sc));
                return -1;
        }
        pd->pd_softc = sc;
        pci_enable(pd, 0);

        /* Reset, then announce ourselves. */
        common_write8(sc, VIRTIO_COMMON_STATUS, 0);
        while (common_read8(sc, VIRTIO_COMMON_STATUS) != 0)
                continue;
        common_write8(sc, VIRTIO_COMMON_STATUS, VIRTIO_STATUS_ACK);
        common_write8(sc, VIRTIO_COMMON_STATUS,
            VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
        common_write16(sc, VIRTIO_COMMON_MSIX, VIRTIO_MSI_NO_VECTOR);

        switch (type) {
        case VIRTIO_ID_BLOCK:
                if (vioblk_attach(sc) == 0)
                        return 0;
                break;
        default:
                printf("virtio %u:%02u.%u: device type %d has no driver\n",
                    pd->pd_bus, pd->pd_dev, pd->pd_fn, type);
                break;
        }
        virtio_fail(sc);
        return -1;
}

/*
 * Accept the features in wanted that the device offers, always
 * requiring VIRTIO_F_VERSION_1. Returns the negotiated set, or 0 if the
 * device refused it.
 */
uint64_t
virtio_negotiate(struct virtio_softc *sc, uint64_t wanted)
{
        uint64_t offered, chosen;

        common_write32(sc, VIRTIO_COMMON_DFSELECT, 0);
        offered = common_read32(sc, VIRTIO_COMMON_DF);
        common_write32(sc, VIRTIO_COMMON_DFSELECT, 1);
        offered |= (uint64_t)common_read32(sc, VIRTIO_COMMON_DF) << 32;

        chosen = offered & (wanted | ((uint64_t)1 << VIRTIO_F_VERSION_1));
        if ((chosen & ((uint64_t)1 << VIRTIO_F_VERSION_1)) == 0)
                return 0;
        common_write32(sc, VIRTIO_COMMON_GFSELECT, 0);
        common_write32(sc, VIRTIO_COMMON_GF, (uint32_t)chosen);
        common_write32(sc, VIRTIO_COMMON_GFSELECT, 1);
        common_write32(sc, VIRTIO_COMMON_GF, (uint32_t)(chosen >> 32));
        common_write8(sc, VIRTIO_COMMON_STATUS,
            common_read8(sc, VIRTIO_COMMON_STATUS) |
            VIRTIO_STATUS_FEATURES_OK);
        if ((common_read8(sc, VIRTIO_COMMON_STATUS) &
            VIRTIO_STATUS_FEATURES_OK) == 0)
                return 0;
        sc->vs_features = chosen;
        return chosen;
}

int
virtio_has_feature(const struct virtio_softc *sc, int bit)
{
        return (sc->vs_features & ((uint64_t)1 << bit)) != 0;
}

/*
 * Set up queue index with at most maxsize entries, interrupting through
 * MSI-X entry index into done(cookie, len). Returns the queue or NULL.
 */
struct virtqueue *
virtio_alloc_vq(struct virtio_softc *sc, int index, int maxsize,
    void (*done)(void *, uint32_t), const char *name)
{
        struct virtqueue *vq;
        int size;
        uint16_t noff;

        if (index >= VIRTIO_MAXQUEUES ||
            index >= common_read16(sc, VIRTIO_COMMON_NUMQ))
                return NULL;
        common_write16(sc, VIRTIO_COMMON_Q_SELECT, (uint16_t)index);
        size = common_read16(sc, VIRTIO_COMMON_Q_SIZE);
        if (size == 0)
                return NULL;
        while (size > maxsize || size > VIRTIO_MAXQSIZE)
                size /= 2;

        vq = &sc->vs_vq[index];
        vq->vq_sc = sc;
        vq->vq_index = (uint16_t)index;
        vq->vq_done = done;
        if (vq_init(vq, size) != 0)
                return NULL;

        common_write16(sc, VIRTIO_COMMON_Q_SIZE, (uint16_t)size);
        common_write64(sc, VIRTIO_COMMON_Q_DESC,
            kva_to_phys((const void *)(uintptr_t)vq->vq_desc));
        common_write64(sc, VIRTIO_COMMON_Q_AVAIL,
            kva_to_phys((const void *)(uintptr_t)vq->vq_avail));
        common_write64(sc, VIRTIO_COMMON_Q_USED,
            kva_to_phys((const void *)(uintptr_t)vq->vq_used));

        vq->vq_intr = pci_msix_establish(sc->vs_pci, index, 0, vq_intr, vq,
            name);
        common_write16(sc, VIRTIO_COMMON_Q_MSIX, (uint16_t)index);
        if (vq->vq_intr == NULL ||
            common_read16(sc, VIRTIO_COMMON_Q_MSIX) != index) {
                printf("virtio: queue %d: cannot set up its interrupt\n",
                    index);
                return NULL;
        }

        noff = common_read16(sc, VIRTIO_COMMON_Q_NOFF);
        vq->vq_notify = (volatile uint16_t *)(volatile void *)
            (sc->vs_notify_base + (uint32_t)noff * sc->vs_notify_mult);
        common_write16(sc, VIRTIO_COMMON_Q_ENABLE, 1);
        if (index >= sc->vs_nvqs)
                sc->vs_nvqs = index + 1;
        return vq;
}

void
virtio_ready(struct virtio_softc *sc)
{
        common_write8(sc, VIRTIO_COMMON_STATUS,
            common_read8(sc, VIRTIO_COMMON_STATUS) |
            VIRTIO_STATUS_DRIVER_OK);
}

void
virtio_fail(struct virtio_softc *sc)
{
        common_write8(sc, VIRTIO_COMMON_STATUS,
            common_read8(sc, VIRTIO_COMMON_STATUS) | VIRTIO_STATUS_FAILED);
}

/* Read width bytes (1, 2, or 4) of device configuration at off. */
uint32_t
virtio_cfg_read(struct virtio_softc *sc, int off, int width)
{
        volatile uint8_t *p = sc->vs_devcfg + off;

        if (sc->vs_devcfg == NULL)
                return 0;
        if (width == 1)
                return *p;
        if (width == 2)
                return *(volatile uint16_t *)(volatile void *)p;
        return *(volatile uint32_t *)(volatile void *)p;
}

/* A 64-bit field, read until the configuration generation is stable. */
uint64_t
virtio_cfg_read64(struct virtio_softc *sc, int off)
{
        uint8_t gen;
        uint64_t v;

        do {
                gen = common_read8(sc, VIRTIO_COMMON_CFGGEN);
                v = virtio_cfg_read(sc, off, 4) |
                    ((uint64_t)virtio_cfg_read(sc, off + 4, 4) << 32);
        } while (gen != common_read8(sc, VIRTIO_COMMON_CFGGEN));
        return v;
}
