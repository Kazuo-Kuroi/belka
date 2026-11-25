/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * Intel's PCIe gigabit Ethernet controllers (dev/pci/e1000reg.h): the
 * 82574 for now, which QEMU emulates as "e1000e". Each device is
 * /dev/e1000gN, whose streams are the shared Ethernet DLPI provider's
 * (net/dlether.c). Written from Intel's datasheet.
 *
 * One receive and one transmit ring of legacy descriptors, E1000_NDESC
 * each, every descriptor with a 2048-byte buffer of its own, two to a
 * page (so that no buffer crosses a page and each is physically one
 * piece). A received frame is copied out of its buffer, which goes
 * back on the ring; a frame to send is copied into the next free
 * slot's buffer. The device strips the CRC and pads short frames.
 *
 * Interrupts are MSI-X, all causes on vector 0. The handler (in the
 * vector's interrupt thread) takes the causes from ICR and wakes the
 * device's kernel thread, which drains the receive ring into
 * dlether_input and notes link changes. Transmitted slots are reclaimed
 * as the next frames go out. Group frames all come (MPE); every unicast
 * one too while a stream is promiscuous (UPE).
 *
 * Locks: e1k_intrlock (a spin lock) covers the causes waiting for the
 * thread; e1k_txlock (a spin lock) the transmit ring and RCTL, and may
 * be taken with dlether's de_lock held. Neither spin lock is held while
 * the other is taken, nor across dlether_input.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/dlether.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mkdev.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <machine/atomic.h>
#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/page.h>

#include "dev/pci/e1000reg.h"
#include "dev/pci/pcivar.h"

#define E1000_NDESC     128             /* per ring: 2048 bytes of them */
#define E1000_BUFSZ     2048
#define E1000_MAXUNITS  8
#define E1000_REGSIZE   0x20000         /* BAR 0 */

struct e1000g_softc {
        struct dlether  e1k_dl;
        struct pci_dev  *e1k_pd;
        volatile uint8_t *e1k_regs;
        unsigned int    e1k_unit;
        int             e1k_link;       /* up, as last seen */

        volatile struct e1000_rxdesc *e1k_rxd;
        uint8_t         *e1k_rxbuf[E1000_NDESC];
        unsigned int    e1k_rxnext;     /* the next to complete */

        struct spinlock e1k_txlock;
        volatile struct e1000_txdesc *e1k_txd;
        uint8_t         *e1k_txbuf[E1000_NDESC];
        unsigned int    e1k_txnext;     /* the next to fill */
        unsigned int    e1k_txfree;     /* slots free */
        unsigned int    e1k_txclean;    /* the oldest not reclaimed */

        struct spinlock e1k_intrlock;
        struct sv       e1k_sv;
        uint32_t        e1k_causes;     /* from ICR, for the thread */
};

static unsigned int e1000g_nunits;

static uint32_t
e1k_read(struct e1000g_softc *sc, uint32_t reg)
{
        return *(volatile uint32_t *)(volatile void *)(sc->e1k_regs + reg);
}

static void
e1k_write(struct e1000g_softc *sc, uint32_t reg, uint32_t v)
{
        *(volatile uint32_t *)(volatile void *)(sc->e1k_regs + reg) = v;
}

/* Wait about us microseconds, at boot (attach cannot sleep). */
static void
e1k_spin(uint64_t us)
{
        uint64_t end = gethrtime() + us * 1000;

        while (gethrtime() < end)
                continue;
}

#define E1000_PERPAGE   ((int)(PAGESIZE / E1000_BUFSZ))

/* E1000_NDESC buffers of E1000_BUFSZ, two to a page. */
static void
e1k_buffers(uint8_t **bufs)
{
        for (int i = 0; i < E1000_NDESC; i += E1000_PERPAGE) {
                struct page *pg = page_alloc(0, PA_ZERO);
                uint8_t *va;

                if (pg == NULL)
                        panic("e1000g: out of memory");
                va = page_to_kva(pg);
                for (int j = 0; j < E1000_PERPAGE; j++)
                        bufs[i + j] = va + j * E1000_BUFSZ;
        }
}

/* One page of descriptors, its physical address in *pa. */
static void *
e1k_ring(paddr_t *pa)
{
        struct page *pg = page_alloc(0, PA_ZERO);
        void *va;

        if (pg == NULL)
                panic("e1000g: out of memory");
        va = page_to_kva(pg);
        *pa = kva_to_phys(va);
        return va;
}

/* Reclaim the transmit slots the device is done with; e1k_txlock held. */
static void
e1k_txreclaim(struct e1000g_softc *sc)
{
        while (sc->e1k_txfree < E1000_NDESC) {
                volatile struct e1000_txdesc *td =
                    &sc->e1k_txd[sc->e1k_txclean];

                if ((td->td_status & E1000_TXS_DD) == 0)
                        break;
                td->td_status = 0;
                sc->e1k_txclean = (sc->e1k_txclean + 1) % E1000_NDESC;
                sc->e1k_txfree++;
        }
}

/* A frame out, into the next slot; -1 if the ring is full. */
static int
e1k_output(struct dlether *de, const uint8_t *frame, size_t len)
{
        struct e1000g_softc *sc = de->de_softc;
        volatile struct e1000_txdesc *td;
        unsigned int slot;
        int s;

        if (len > E1000_BUFSZ)
                return -1;
        s = spin_lock(&sc->e1k_txlock);
        if (sc->e1k_txfree == 0)
                e1k_txreclaim(sc);
        if (sc->e1k_txfree == 0) {
                spin_unlock(&sc->e1k_txlock, s);
                return -1;
        }
        slot = sc->e1k_txnext;
        memcpy(sc->e1k_txbuf[slot], frame, len);
        td = &sc->e1k_txd[slot];
        td->td_addr = kva_to_phys(sc->e1k_txbuf[slot]);
        td->td_len = (uint16_t)len;
        td->td_cso = 0;
        td->td_css = 0;
        td->td_special = 0;
        td->td_status = 0;
        td->td_cmd = E1000_TXC_EOP | E1000_TXC_IFCS | E1000_TXC_RS;
        sc->e1k_txnext = (slot + 1) % E1000_NDESC;
        sc->e1k_txfree--;
        /* The descriptor before the tail that hands it over. */
        membar_producer();
        e1k_write(sc, E1000_TDT, sc->e1k_txnext);
        if (sc->e1k_txfree < E1000_NDESC / 4)
                e1k_txreclaim(sc);
        spin_unlock(&sc->e1k_txlock, s);
        return 0;
}

/* Every unicast frame, or only ours (dlether, with de_lock held). */
static void
e1k_promisc(struct dlether *de, int on)
{
        struct e1000g_softc *sc = de->de_softc;
        int s = spin_lock(&sc->e1k_txlock);
        uint32_t rctl = e1k_read(sc, E1000_RCTL);

        if (on)
                rctl |= E1000_RCTL_UPE;
        else
                rctl &= ~E1000_RCTL_UPE;
        e1k_write(sc, E1000_RCTL, rctl);
        spin_unlock(&sc->e1k_txlock, s);
}

/* Received frames, to the streams; each buffer back on the ring. */
static void
e1k_rxdrain(struct e1000g_softc *sc)
{
        unsigned int last = E1000_NDESC;

        for (;;) {
                unsigned int i = sc->e1k_rxnext;
                volatile struct e1000_rxdesc *rd = &sc->e1k_rxd[i];
                uint8_t status = rd->rd_status;

                if ((status & E1000_RXD_DD) == 0)
                        break;
                /* The status before the rest the device wrote. */
                membar_consumer();
                if ((status & E1000_RXD_EOP) != 0 &&
                    (rd->rd_errors & E1000_RXE_MASK) == 0 &&
                    rd->rd_len <= E1000_BUFSZ)
                        dlether_input(&sc->e1k_dl, sc->e1k_rxbuf[i],
                            rd->rd_len);
                else
                        atomic_inc_64_nv(&sc->e1k_dl.de_drops);
                rd->rd_status = 0;
                rd->rd_errors = 0;
                last = i;
                sc->e1k_rxnext = (i + 1) % E1000_NDESC;
        }
        if (last != E1000_NDESC) {
                membar_producer();
                e1k_write(sc, E1000_RDT, last);
        }
}

static void
e1k_linkcheck(struct e1000g_softc *sc)
{
        uint32_t st = e1k_read(sc, E1000_STATUS);
        int up = (st & E1000_STATUS_LU) != 0;
        static const unsigned int mbps[] = { 10, 100, 1000, 1000 };

        if (up == sc->e1k_link)
                return;
        sc->e1k_link = up;
        if (up)
                printf("%s: link up, %u Mb/s, %s duplex\n",
                    sc->e1k_dl.de_name, mbps[E1000_STATUS_SPEED(st)],
                    (st & E1000_STATUS_FD) != 0 ? "full" : "half");
        else
                printf("%s: link down\n", sc->e1k_dl.de_name);
}

/* MSI-X vector 0: the causes, to the thread. */
static int
e1k_intr(void *arg)
{
        struct e1000g_softc *sc = arg;
        uint32_t icr = e1k_read(sc, E1000_ICR);
        int s;

        if (icr == 0)
                return 0;
        e1k_write(sc, E1000_ICR, icr);
        s = spin_lock(&sc->e1k_intrlock);
        sc->e1k_causes |= icr;
        sv_signal(&sc->e1k_sv);
        spin_unlock(&sc->e1k_intrlock, s);
        return 1;
}

/* The device's kernel thread. */
static void
e1k_thread(void *arg)
{
        struct e1000g_softc *sc = arg;

        for (;;) {
                uint32_t causes;
                int s = spin_lock(&sc->e1k_intrlock);

                while (sc->e1k_causes == 0) {
                        sv_wait(&sc->e1k_sv, &sc->e1k_intrlock, s);
                        s = spin_lock(&sc->e1k_intrlock);
                }
                causes = sc->e1k_causes;
                sc->e1k_causes = 0;
                spin_unlock(&sc->e1k_intrlock, s);

                if ((causes & E1000_INT_LSC) != 0)
                        e1k_linkcheck(sc);
                e1k_rxdrain(sc);
                if ((causes & (E1000_INT_TXDW | E1000_INT_TXQ0)) != 0) {
                        s = spin_lock(&sc->e1k_txlock);
                        e1k_txreclaim(sc);
                        spin_unlock(&sc->e1k_txlock, s);
                }
        }
}

/* Reset, and wait for the device to load its NVM (the address). */
static int
e1k_reset(struct e1000g_softc *sc)
{
        e1k_write(sc, E1000_IMC, 0xffffffff);
        e1k_write(sc, E1000_RCTL, 0);
        e1k_write(sc, E1000_TCTL, 0);
        (void)e1k_read(sc, E1000_STATUS);
        e1k_spin(10000);
        e1k_write(sc, E1000_CTRL, e1k_read(sc, E1000_CTRL) | E1000_CTRL_RST);
        e1k_spin(20000);
        for (int i = 0; i < 100; i++) {
                if ((e1k_read(sc, E1000_CTRL) & E1000_CTRL_RST) == 0) {
                        e1k_write(sc, E1000_IMC, 0xffffffff);
                        (void)e1k_read(sc, E1000_ICR);
                        return 0;
                }
                e1k_spin(1000);
        }
        return -1;
}

static void
e1k_rxinit(struct e1000g_softc *sc)
{
        paddr_t pa;

        sc->e1k_rxd = e1k_ring(&pa);
        e1k_buffers(sc->e1k_rxbuf);
        for (int i = 0; i < E1000_NDESC; i++)
                sc->e1k_rxd[i].rd_addr = kva_to_phys(sc->e1k_rxbuf[i]);
        e1k_write(sc, E1000_RDBAL, (uint32_t)pa);
        e1k_write(sc, E1000_RDBAH, (uint32_t)(pa >> 32));
        e1k_write(sc, E1000_RDLEN,
            E1000_NDESC * sizeof(struct e1000_rxdesc));
        e1k_write(sc, E1000_RDH, 0);
        /* Every descriptor but one: head == tail would mean none. */
        e1k_write(sc, E1000_RDT, E1000_NDESC - 1);
        e1k_write(sc, E1000_RDTR, 0);
        e1k_write(sc, E1000_RADV, 0);
        e1k_write(sc, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM |
            E1000_RCTL_MPE | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);
}

static void
e1k_txinit(struct e1000g_softc *sc)
{
        paddr_t pa;

        spinlock_init(&sc->e1k_txlock, "e1000gtx");
        sc->e1k_txd = e1k_ring(&pa);
        e1k_buffers(sc->e1k_txbuf);
        sc->e1k_txfree = E1000_NDESC;
        e1k_write(sc, E1000_TDBAL, (uint32_t)pa);
        e1k_write(sc, E1000_TDBAH, (uint32_t)(pa >> 32));
        e1k_write(sc, E1000_TDLEN,
            E1000_NDESC * sizeof(struct e1000_txdesc));
        e1k_write(sc, E1000_TDH, 0);
        e1k_write(sc, E1000_TDT, 0);
        e1k_write(sc, E1000_TIDV, 0);
        e1k_write(sc, E1000_TXDCTL,
            e1k_read(sc, E1000_TXDCTL) | E1000_TXDCTL_COUNT);
        e1k_write(sc, E1000_TIPG, E1000_TIPG_DEFAULT);
        e1k_write(sc, E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
            E1000_TCTL_CT(0x0f) | E1000_TCTL_COLD(0x3f));
}

static int
e1000g_match(const struct pci_dev *pd)
{
        return pd->pd_vendor == E1000_VENDOR_INTEL &&
            (pd->pd_device == E1000_DEV_82574L ||
            pd->pd_device == E1000_DEV_82574LA);
}

static int
e1000g_attach(struct pci_dev *pd)
{
        struct e1000g_softc *sc;
        uint32_t ral, rah;
        const char *name;

        if (e1000g_nunits >= E1000_MAXUNITS) {
                printf("e1000g: too many devices\n");
                return -1;
        }
        if (pd->pd_bar[0].pb_addr == 0 || pd->pd_bar[0].pb_io ||
            pci_msix_count(pd) == 0) {
                printf("e1000g %u:%02u.%u: no registers or no MSI-X\n",
                    pd->pd_bus, pd->pd_dev, pd->pd_fn);
                return -1;
        }
        sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
        sc->e1k_pd = pd;
        sc->e1k_unit = e1000g_nunits;
        snprintf(sc->e1k_dl.de_name, sizeof(sc->e1k_dl.de_name), "e1000g%u",
            sc->e1k_unit);
        name = sc->e1k_dl.de_name;
        sc->e1k_regs = pci_map_bar(pd, 0, 0, E1000_REGSIZE);
        if (sc->e1k_regs == NULL) {
                printf("%s: cannot map its registers\n", name);
                kmem_free(sc, sizeof(*sc));
                return -1;
        }
        pci_enable(pd, 0);
        if (e1k_reset(sc) != 0) {
                printf("%s: does not come out of reset\n", name);
                kmem_free(sc, sizeof(*sc));
                return -1;
        }

        /* The station address, as the device loaded it from its NVM. */
        ral = e1k_read(sc, E1000_RAL0);
        rah = e1k_read(sc, E1000_RAH0);
        if ((rah & E1000_RAH_AV) == 0) {
                printf("%s: no station address\n", name);
                kmem_free(sc, sizeof(*sc));
                return -1;
        }
        for (int i = 0; i < 4; i++)
                sc->e1k_dl.de_addr[i] = (uint8_t)(ral >> (8 * i));
        sc->e1k_dl.de_addr[4] = (uint8_t)rah;
        sc->e1k_dl.de_addr[5] = (uint8_t)(rah >> 8);
        sc->e1k_dl.de_output = e1k_output;
        sc->e1k_dl.de_promisc = e1k_promisc;
        sc->e1k_dl.de_softc = sc;
        for (int i = 0; i < E1000_NMTA; i++)
                e1k_write(sc, E1000_MTA + 4 * i, 0);

        spinlock_init(&sc->e1k_intrlock, "e1000g");
        sv_init(&sc->e1k_sv, "e1000g");
        if (pci_msix_establish(pd, 0, 0, e1k_intr, sc, name) == NULL) {
                printf("%s: cannot set up its interrupt\n", name);
                kmem_free(sc, sizeof(*sc));
                return -1;
        }
        e1k_write(sc, E1000_IVAR, E1000_IVAR_RXQ0(0) | E1000_IVAR_TXQ0(0) |
            E1000_IVAR_OTHER(0) | E1000_IVAR_EVERYWB);
        e1k_write(sc, E1000_CTRL_EXT,
            e1k_read(sc, E1000_CTRL_EXT) | E1000_CTRL_EXT_PBA);

        e1k_txinit(sc);
        e1k_rxinit(sc);
        e1k_write(sc, E1000_CTRL, (e1k_read(sc, E1000_CTRL) |
            E1000_CTRL_SLU | E1000_CTRL_ASDE));

        if (dlether_register(&sc->e1k_dl, DEV_E1000G_MAJOR,
            sc->e1k_unit) != 0)
                return -1;
        pd->pd_softc = sc;
        e1000g_nunits++;
        (void)kthread_create(e1k_thread, sc, PRI_KERNEL + 5, name);
        e1k_write(sc, E1000_IMS, E1000_INT_TXDW | E1000_INT_LSC |
            E1000_INT_RXDMT0 | E1000_INT_RXO | E1000_INT_RXT0 |
            E1000_INT_RXQ0 | E1000_INT_TXQ0 | E1000_INT_OTHER);
        printf("%s: Intel 82574, address %02x:%02x:%02x:%02x:%02x:%02x\n",
            name, sc->e1k_dl.de_addr[0], sc->e1k_dl.de_addr[1],
            sc->e1k_dl.de_addr[2], sc->e1k_dl.de_addr[3],
            sc->e1k_dl.de_addr[4], sc->e1k_dl.de_addr[5]);
        e1k_linkcheck(sc);
        return 0;
}

const struct pci_driver e1000g_pci_driver = {
        "e1000g", e1000g_match, e1000g_attach
};
