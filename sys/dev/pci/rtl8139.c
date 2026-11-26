/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-26
 * SPDX-License-Identifier: MIT
 */

/*
 * Realtek RTL8139 in legacy mode. The device writes receive records to
 * a byte ring and takes transmit buffers through four address registers.
 * Register definitions follow the RTL8139C(L) datasheet, revision 1.6.
 */

#include <stdint.h>

#include <sys/dlether.h>
#include <sys/intr.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mkdev.h>
#include <sys/systm.h>

#include <machine/atomic.h>

#include <vm/kva.h>
#include <vm/page.h>

#include "dev/pci/pcivar.h"

#define RTL_IDR         0x00
#define RTL_MAR         0x08
#define RTL_TSD         0x10
#define RTL_TSAD        0x20
#define RTL_RBSTART     0x30
#define RTL_CR          0x37
#define RTL_CAPR        0x38
#define RTL_CBR         0x3a
#define RTL_IMR         0x3c
#define RTL_ISR         0x3e
#define RTL_TCR         0x40
#define RTL_RCR         0x44

#define RTL_CR_RESET    0x10
#define RTL_CR_RX       0x08
#define RTL_CR_TX       0x04
#define RTL_TSD_OWN     (1U << 13)
#define RTL_TSD_THRESH  (8U << 16)       /* start at 256 bytes in FIFO */
#define RTL_TCR_CONFIG  ((3U << 24) | (6U << 8)) /* normal IFG, 1 KiB DMA */
#define RTL_TCR_CLRABT  0x01

#define RTL_RCR_FIFO    (7U << 13)       /* no early receive threshold */
#define RTL_RCR_32K     (2U << 11)
#define RTL_RCR_DMA     (6U << 8)
#define RTL_RCR_AB      0x08
#define RTL_RCR_AM      0x04
#define RTL_RCR_APM     0x02
#define RTL_RCR_AAP     0x01
#define RTL_RCR_CONFIG  (RTL_RCR_FIFO | RTL_RCR_32K | RTL_RCR_DMA | \
                         RTL_RCR_AB | RTL_RCR_AM | RTL_RCR_APM | RTL_RCR_AAP)
#define RTL_RX_OK       0x0001
#define RTL_RX_ERRORS   0x003e
#define RTL_RX_BUSY     0xfff0
#define RTL_RX_HEADER   4
#define RTL_RX_FCS      4
#define RTL_CAPR_GAP    16

#define RTL_INT_RXOK    0x0001
#define RTL_INT_RXERR   0x0002
#define RTL_INT_TXERR   0x0008
#define RTL_INT_RXOVW   0x0010
#define RTL_INT_FIFO    0x0040
#define RTL_INT_RXLOSS  (RTL_INT_RXERR | RTL_INT_RXOVW | RTL_INT_FIFO)
#define RTL_INTERRUPTS  (RTL_INT_RXOK | RTL_INT_RXLOSS | RTL_INT_TXERR)

#define RTL_REGSIZE     0x100
#define RTL_RX_ORDER    3
#define RTL_RX_SIZE     32768U
#define RTL_TX_ORDER    1
#define RTL_TX_SLOTS    4
#define RTL_TX_SIZE     2048
#define RTL_MAXUNITS    16

/* Natural-width MMIO accesses on the little-endian PCI host. */
#define RTL_REG(sc, reg, type)   (*(volatile type *)((sc)->r_regs + (reg)))
#define RTL_REG8(sc, reg)        RTL_REG(sc, reg, uint8_t)
#define RTL_REG16(sc, reg)       RTL_REG(sc, reg, uint16_t)
#define RTL_REG32(sc, reg)       RTL_REG(sc, reg, uint32_t)

struct rtl8139_softc {
        struct dlether  r_dl;
        volatile uint8_t *r_regs;
        uint8_t         *r_rx;
        uint8_t         *r_tx;
        unsigned int    r_rxnext;       /* receive thread only */
        struct spinlock r_txlock;
        unsigned int    r_txnext;
        struct spinlock r_intrlock;
        struct sv       r_sv;
        unsigned int    r_pending;
};

static unsigned int rtl8139_units;

static int
rtl_output(struct dlether *de, const uint8_t *frame, size_t len)
{
        struct rtl8139_softc *sc = de->de_softc;
        unsigned int slot;
        int s;

        if (len < ETHER_MINFRAME || len > ETHER_MAXFRAME)
                return -1;
        s = spin_lock(&sc->r_txlock);
        slot = sc->r_txnext;
        if ((RTL_REG32(sc, RTL_TSD + 4 * slot) & RTL_TSD_OWN) == 0) {
                spin_unlock(&sc->r_txlock, s);
                return -1;
        }
        memcpy(sc->r_tx + slot * RTL_TX_SIZE, frame, len);
        membar_producer();
        RTL_REG32(sc, RTL_TSD + 4 * slot) = RTL_TSD_THRESH | (uint32_t)len;
        sc->r_txnext = (slot + 1) % RTL_TX_SLOTS;
        spin_unlock(&sc->r_txlock, s);
        return 0;
}

/* Reinitialize only the receive side when a record cannot be parsed. */
static void
rtl_rxrestart(struct rtl8139_softc *sc)
{
        RTL_REG8(sc, RTL_CR) = RTL_CR_TX;
        sc->r_rxnext = 0;
        RTL_REG8(sc, RTL_CR) = RTL_CR_RX | RTL_CR_TX;
        RTL_REG32(sc, RTL_RCR) = RTL_RCR_CONFIG;
        RTL_REG16(sc, RTL_CAPR) = (uint16_t)-RTL_CAPR_GAP;
}

static void
rtl_rxdrain(struct rtl8139_softc *sc)
{
        uint8_t frame[ETHER_MAXFRAME];

        for (;;) {
                unsigned int pos = sc->r_rxnext;
                unsigned int end = RTL_REG16(sc, RTL_CBR);
                unsigned int available = (end - pos) & (RTL_RX_SIZE - 1);
                unsigned int status, len, advance, first;
                int good;

                if (available < RTL_RX_HEADER)
                        return;
                membar_consumer();
                /* Record headers are aligned and never straddle the ring. */
                status = sc->r_rx[pos] | (unsigned int)sc->r_rx[pos + 1] << 8;
                len = sc->r_rx[pos + 2] | (unsigned int)sc->r_rx[pos + 3] << 8;
                if (len == RTL_RX_BUSY)
                        return;
                advance = (RTL_RX_HEADER + len + 3) & ~3U;
                if (len < RTL_RX_FCS || advance >= RTL_RX_SIZE) {
                        atomic_inc_64_nv(&sc->r_dl.de_drops);
                        rtl_rxrestart(sc);
                        return;
                }
                if (advance > available)
                        return;         /* device has not finished the record */
                len -= RTL_RX_FCS;
                good = (status & (RTL_RX_OK | RTL_RX_ERRORS)) == RTL_RX_OK &&
                    len >= ETHER_HDRLEN && len <= sizeof(frame);
                if (good) {
                        pos = (pos + RTL_RX_HEADER) & (RTL_RX_SIZE - 1);
                        first = RTL_RX_SIZE - pos;
                        if (first > len)
                                first = len;
                        memcpy(frame, sc->r_rx + pos, first);
                        if (first != len)
                                memcpy(frame + first, sc->r_rx, len - first);
                }
                sc->r_rxnext = (sc->r_rxnext + advance) & (RTL_RX_SIZE - 1);
                /* Copy before returning the bytes to the device. */
                membar_sync();
                RTL_REG16(sc, RTL_CAPR) = (uint16_t)(sc->r_rxnext - RTL_CAPR_GAP);
                if (good)
                        dlether_input(&sc->r_dl, frame, len);
                else
                        atomic_inc_64_nv(&sc->r_dl.de_drops);
        }
}

static int
rtl_intr(void *arg)
{
        struct rtl8139_softc *sc = arg;
        unsigned int status = RTL_REG16(sc, RTL_ISR);
        unsigned int causes = status & RTL_REG16(sc, RTL_IMR);
        int s;

        if (status == UINT16_MAX || causes == 0)
                return 0;
        RTL_REG16(sc, RTL_ISR) = (uint16_t)causes;
        (void)RTL_REG16(sc, RTL_ISR);    /* deassert shared INTx before return */
        s = spin_lock(&sc->r_intrlock);
        sc->r_pending |= causes;
        sv_signal(&sc->r_sv);
        spin_unlock(&sc->r_intrlock, s);
        return 1;
}

static void
rtl_worker(void *arg)
{
        struct rtl8139_softc *sc = arg;

        for (;;) {
                unsigned int causes;
                int s = spin_lock(&sc->r_intrlock);

                while (sc->r_pending == 0) {
                        sv_wait(&sc->r_sv, &sc->r_intrlock, s);
                        s = spin_lock(&sc->r_intrlock);
                }
                causes = sc->r_pending;
                sc->r_pending = 0;
                spin_unlock(&sc->r_intrlock, s);
                if ((causes & RTL_INT_RXLOSS) != 0)
                        atomic_inc_64_nv(&sc->r_dl.de_drops);
                if ((causes & RTL_INT_TXERR) != 0) {
                        atomic_inc_64_nv(&sc->r_dl.de_drops);
                        s = spin_lock(&sc->r_txlock);
                        RTL_REG32(sc, RTL_TCR) = RTL_TCR_CONFIG | RTL_TCR_CLRABT;
                        spin_unlock(&sc->r_txlock, s);
                }
                rtl_rxdrain(sc);
        }
}

static int
rtl_match(const struct pci_dev *pd)
{
        return pd->pd_vendor == 0x10ec && pd->pd_device == 0x8139;
}

static int
rtl_attach(struct pci_dev *pd)
{
        struct rtl8139_softc *sc;
        struct page *rx = NULL, *tx = NULL;
        void *regs, *ih = NULL;
        unsigned int unit = rtl8139_units;
        int retry;

        if (unit >= RTL_MAXUNITS || (regs = pci_map_bar(pd, 1, 0,
            RTL_REGSIZE)) == NULL)
                return -1;
        sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
        sc->r_regs = regs;
        snprintf(sc->r_dl.de_name, sizeof(sc->r_dl.de_name), "rtl8139%u", unit);
        pci_enable(pd, 0);
        RTL_REG16(sc, RTL_IMR) = 0;
        RTL_REG8(sc, RTL_CR) = RTL_CR_RESET;
        for (retry = 0; retry < 10; retry++) {
                if ((RTL_REG8(sc, RTL_CR) & RTL_CR_RESET) == 0)
                        break;
                delay(1);
        }
        if (retry == 10) {
                printf("%s: reset timed out\n", sc->r_dl.de_name);
                goto fail;
        }

        rx = page_alloc(RTL_RX_ORDER, PA_ZERO | PA_DMA32);
        tx = page_alloc(RTL_TX_ORDER, PA_ZERO | PA_DMA32);
        if (rx == NULL || tx == NULL) {
                printf("%s: no 32-bit DMA buffers\n", sc->r_dl.de_name);
                goto fail;
        }
        sc->r_rx = page_to_kva(rx);
        sc->r_tx = page_to_kva(tx);
        for (int i = 0; i < ETHER_ADDRLEN; i++)
                sc->r_dl.de_addr[i] = RTL_REG8(sc, RTL_IDR + i);
        sc->r_dl.de_output = rtl_output;
        sc->r_dl.de_softc = sc;
        spinlock_init(&sc->r_txlock, "rtl8139tx");
        spinlock_init(&sc->r_intrlock, "rtl8139intr");
        sv_init(&sc->r_sv, "rtl8139");

        RTL_REG16(sc, RTL_ISR) = UINT16_MAX;
        ih = pci_intx_establish(pd, 0, rtl_intr, sc, sc->r_dl.de_name);
        if (ih == NULL) {
                printf("%s: cannot set up INTx\n", sc->r_dl.de_name);
                goto fail;
        }
        if (dlether_register(&sc->r_dl, DEV_RTL8139_MAJOR, unit) != 0)
                goto fail;

        /*
         * Reset selects legacy mode. Even writing zero to C+ Command
         * makes QEMU interpret the legacy transmit registers as C+.
         */
        RTL_REG32(sc, RTL_RBSTART) = (uint32_t)page_to_pa(rx);
        for (unsigned int i = 0; i < RTL_TX_SLOTS; i++)
                RTL_REG32(sc, RTL_TSAD + 4 * i) =
                    (uint32_t)(page_to_pa(tx) + i * RTL_TX_SIZE);
        RTL_REG32(sc, RTL_MAR) = UINT32_MAX;
        RTL_REG32(sc, RTL_MAR + 4) = UINT32_MAX;
        /*
         * QEMU resets both ring pointers on RCR writes. Keep acceptance
         * fixed and let DLPI filter by station and per-stream promiscuity.
         */
        rtl_rxrestart(sc);
        RTL_REG32(sc, RTL_TCR) = RTL_TCR_CONFIG;
        pd->pd_softc = sc;
        rtl8139_units++;
        (void)kthread_create(rtl_worker, sc, PRI_KERNEL + 5, sc->r_dl.de_name);
        RTL_REG16(sc, RTL_IMR) = RTL_INTERRUPTS;
        printf("%s: Realtek RTL8139, INTx, address %02x:%02x:%02x:%02x:%02x:%02x\n",
            sc->r_dl.de_name, sc->r_dl.de_addr[0], sc->r_dl.de_addr[1],
            sc->r_dl.de_addr[2], sc->r_dl.de_addr[3], sc->r_dl.de_addr[4],
            sc->r_dl.de_addr[5]);
        return 0;

fail:
        if (ih != NULL)
                intr_disestablish(ih);
        if (tx != NULL)
                page_free(tx, RTL_TX_ORDER);
        if (rx != NULL)
                page_free(rx, RTL_RX_ORDER);
        kva_unmap_phys(regs, RTL_REGSIZE);
        kmem_free(sc, sizeof(*sc));
        return -1;
}

const struct pci_driver rtl8139_pci_driver = {
        "rtl8139", rtl_match, rtl_attach
};
