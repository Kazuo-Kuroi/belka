/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-25
 * SPDX-License-Identifier: MIT
 */

/*
 * AHCI Serial ATA controllers (dev/pci/ahcireg.h), written from the
 * AHCI and ATA specifications. Each port with an ATA disk is a block
 * device, sdN; ATAPI devices (the CD-ROM QEMU puts on q35's controller)
 * and empty ports are passed over.
 *
 * Each port has one page: its command list (1 KB), its received FIS
 * area (256 bytes), and one command table with room for AHCI_NPRD
 * PRDs. A port runs one command at a time, in slot 0, under its
 * ap_lock: the request is built, issued (PxCI), and the requester
 * sleeps until the interrupt says it is done. At attach, IDENTIFY
 * DEVICE is polled, as attach cannot sleep. Reads and writes are READ
 * and WRITE DMA EXT (48-bit LBAs); a task file error is reported as
 * EIO after the port is stopped and started again.
 *
 * Interrupts come by MSI (or MSI-X), one for the HBA: the handler (in
 * the vector's interrupt thread) clears each port's causes and wakes
 * the port's requester if its command is done or failed.
 *
 * Locks: ap_lock (a mutex), one per port, covers its command slot and
 * registers while a command runs; ap_intrlock (a spin lock) the causes
 * the handler saw, for the requester.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>
#include <sys/time.h>

#include <machine/atomic.h>
#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/hat.h>
#include <vm/page.h>

#include "dev/pci/ahcireg.h"
#include "dev/pci/pcivar.h"

#define AHCI_SECSIZE    512
#define AHCI_MAXIO      (64 * 1024)
#define AHCI_NPRD       32              /* in the command table */
#define AHCI_CL_OFF     0x000           /* in the port's page */
#define AHCI_FB_OFF     0x400
#define AHCI_CT_OFF     0x500
#define AHCI_TIMEOUT    5000            /* ms, at attach */

struct ahci_softc;

struct ahci_port {
        struct blkdev   ap_bd;
        struct ahci_softc *ap_sc;
        unsigned int    ap_num;
        uint8_t         *ap_page;
        paddr_t         ap_pagepa;
        struct mutex    ap_lock;
        struct sema     ap_done;
        struct spinlock ap_intrlock;
        uint32_t        ap_is;          /* causes for the requester */
        int             ap_waiting;
};

struct ahci_softc {
        struct pci_dev  *ah_pd;
        volatile uint8_t *ah_regs;
        size_t          ah_maplen;      /* bytes of registers mapped */
        uint32_t        ah_pi;
        struct ahci_port *ah_ports[AHCI_NPORTS];
};

static int ahci_read(struct blkdev *, uint64_t, void *, size_t);
static int ahci_write(struct blkdev *, uint64_t, const void *, size_t);
static int ahci_flush(struct blkdev *);

static const struct blkdev_ops ahci_ops = {
        ahci_read, ahci_write, ahci_flush
};

static uint32_t
ah_read(struct ahci_softc *sc, uint32_t reg)
{
        return *(volatile uint32_t *)(volatile void *)(sc->ah_regs + reg);
}

static void
ah_write(struct ahci_softc *sc, uint32_t reg, uint32_t v)
{
        *(volatile uint32_t *)(volatile void *)(sc->ah_regs + reg) = v;
}

static uint32_t
ap_read(struct ahci_port *ap, uint32_t reg)
{
        return ah_read(ap->ap_sc, AHCI_PORT(ap->ap_num) + reg);
}

static void
ap_write(struct ahci_port *ap, uint32_t reg, uint32_t v)
{
        ah_write(ap->ap_sc, AHCI_PORT(ap->ap_num) + reg, v);
}

/* Spin until (reg & mask) == want, up to ms; 0, or -1. */
static int
ap_wait(struct ahci_port *ap, uint32_t reg, uint32_t mask, uint32_t want,
    uint64_t ms)
{
        uint64_t end = gethrtime() + ms * 1000000;

        while ((ap_read(ap, reg) & mask) != want)
                if (gethrtime() > end)
                        return -1;
        return 0;
}

/* Spin for about ms milliseconds, at attach. */
static void
ah_spin(uint64_t ms)
{
        uint64_t end = gethrtime() + ms * 1000000;

        while (gethrtime() < end)
                continue;
}

/*
 * Whether the device has sent its signature and is not busy, waiting
 * up to half a second (FIS receipt on).
 */
static int
ap_signature(struct ahci_port *ap)
{
        uint64_t end = gethrtime() + 500000000ull;

        do {
                if (ap_read(ap, AHCI_PxSIG) != 0xffffffff &&
                    (ap_read(ap, AHCI_PxTFD) & AHCI_TFD_BSY) == 0)
                        return 1;
        } while (gethrtime() < end);
        return 0;
}

/* Stop the port's command engine and FIS receipt; 0, or -1. */
static int
ap_stop(struct ahci_port *ap)
{
        uint32_t cmd = ap_read(ap, AHCI_PxCMD);

        ap_write(ap, AHCI_PxCMD, cmd & ~AHCI_PxCMD_ST);
        if (ap_wait(ap, AHCI_PxCMD, AHCI_PxCMD_CR, 0, 500) != 0)
                return -1;
        cmd = ap_read(ap, AHCI_PxCMD);
        ap_write(ap, AHCI_PxCMD, cmd & ~AHCI_PxCMD_FRE);
        return ap_wait(ap, AHCI_PxCMD, AHCI_PxCMD_FR, 0, 500);
}

/* Start it again, once the device is not busy; 0, or -1. */
static int
ap_start(struct ahci_port *ap)
{
        ap_write(ap, AHCI_PxSERR, 0xffffffff);
        ap_write(ap, AHCI_PxIS, 0xffffffff);
        ap_write(ap, AHCI_PxCMD, ap_read(ap, AHCI_PxCMD) | AHCI_PxCMD_FRE);
        if (ap_wait(ap, AHCI_PxTFD, AHCI_TFD_BSY | AHCI_TFD_DRQ, 0,
            AHCI_TIMEOUT) != 0)
                return -1;
        ap_write(ap, AHCI_PxCMD, ap_read(ap, AHCI_PxCMD) | AHCI_PxCMD_ST);
        return 0;
}

/*
 * Build the command in slot 0: cmd at lba for nsec sectors, with len
 * bytes at buf (none if NULL), toward the device if write.
 */
static void
ap_build(struct ahci_port *ap, uint8_t cmd, uint64_t lba, uint16_t nsec,
    const void *buf, size_t len, int write)
{
        volatile struct ahci_cmdhdr *ch = (volatile struct ahci_cmdhdr *)
            (volatile void *)(ap->ap_page + AHCI_CL_OFF);
        uint8_t *ct = ap->ap_page + AHCI_CT_OFF;
        struct ahci_prd *prd = (struct ahci_prd *)(void *)(ct + AHCI_CT_PRDT);
        uint8_t *fis = ct + AHCI_CT_CFIS;
        uintptr_t va = (uintptr_t)buf;
        paddr_t ctpa = ap->ap_pagepa + AHCI_CT_OFF;
        int n = 0;

        memset(ct, 0, AHCI_CT_PRDT + AHCI_NPRD * sizeof(*prd));
        fis[0] = FIS_TYPE_REG_H2D;
        fis[1] = FIS_H2D_C;
        fis[2] = cmd;
        fis[4] = (uint8_t)lba;
        fis[5] = (uint8_t)(lba >> 8);
        fis[6] = (uint8_t)(lba >> 16);
        fis[7] = ATA_DEV_LBA;
        fis[8] = (uint8_t)(lba >> 24);
        fis[9] = (uint8_t)(lba >> 32);
        fis[10] = (uint8_t)(lba >> 40);
        fis[12] = (uint8_t)nsec;
        fis[13] = (uint8_t)(nsec >> 8);
        while (buf != NULL && len > 0) {
                size_t chunk = PAGESIZE - (va & PAGEOFFSET);
                paddr_t pa;

                if (chunk > len)
                        chunk = len;
                if (hat_kextract(va, &pa) != 0)
                        panic("ahci: buffer %p is not mapped", (void *)va);
                prd[n].prd_dba = (uint32_t)pa;
                prd[n].prd_dbau = (uint32_t)(pa >> 32);
                prd[n].prd_dbc = (uint32_t)(chunk - 1);
                n++;
                va += chunk;
                len -= chunk;
        }
        ch->ch_flags = AHCI_CH_CFL(FIS_H2D_LEN / 4) |
            (write ? AHCI_CH_W : 0);
        ch->ch_prdtl = (uint16_t)n;
        ch->ch_prdbc = 0;
        ch->ch_ctba = (uint32_t)ctpa;
        ch->ch_ctbau = (uint32_t)(ctpa >> 32);
        membar_producer();
}

/* The task file after a command: EIO if it failed, else 0. */
static int
ap_result(struct ahci_port *ap, uint32_t is)
{
        if ((is & AHCI_PxI_ERRORS) == 0 &&
            (ap_read(ap, AHCI_PxTFD) & AHCI_TFD_ERR) == 0)
                return 0;
        printf("%s: command failed (status %#x, task file %#x)\n",
            ap->ap_bd.bd_name[0] != '\0' ? ap->ap_bd.bd_name : "ahci", is,
            ap_read(ap, AHCI_PxTFD));
        (void)ap_stop(ap);
        (void)ap_start(ap);
        return EIO;
}

/* Run the built command, polling (at attach); 0 or an errno value. */
static int
ap_poll(struct ahci_port *ap)
{
        uint64_t end = gethrtime() + AHCI_TIMEOUT * 1000000ull;
        uint32_t is;

        ap_write(ap, AHCI_PxIS, 0xffffffff);
        ap_write(ap, AHCI_PxCI, 1);
        for (;;) {
                is = ap_read(ap, AHCI_PxIS);
                if ((is & AHCI_PxI_ERRORS) != 0 ||
                    (ap_read(ap, AHCI_PxCI) & 1) == 0)
                        break;
                if (gethrtime() > end)
                        return ETIMEDOUT;
        }
        ap_write(ap, AHCI_PxIS, is);
        return ap_result(ap, is);
}

/* Run it, sleeping until the interrupt; ap_lock held. */
static int
ap_run(struct ahci_port *ap)
{
        uint32_t is;
        int s;

        s = spin_lock(&ap->ap_intrlock);
        ap->ap_is = 0;
        ap->ap_waiting = 1;
        spin_unlock(&ap->ap_intrlock, s);
        ap_write(ap, AHCI_PxCI, 1);
        psema(&ap->ap_done);
        s = spin_lock(&ap->ap_intrlock);
        is = ap->ap_is;
        spin_unlock(&ap->ap_intrlock, s);
        return ap_result(ap, is);
}

/* The HBA's interrupt: each port's causes, its requester woken. */
static int
ahci_intr(void *arg)
{
        struct ahci_softc *sc = arg;
        uint32_t is = ah_read(sc, AHCI_IS);

        if (is == 0)
                return 0;
        for (unsigned int p = 0; p < AHCI_NPORTS; p++) {
                struct ahci_port *ap = sc->ah_ports[p];
                uint32_t pis;
                int s;

                if ((is & (1u << p)) == 0)
                        continue;
                pis = ah_read(sc, AHCI_PORT(p) + AHCI_PxIS);
                ah_write(sc, AHCI_PORT(p) + AHCI_PxIS, pis);
                if (ap == NULL)
                        continue;
                s = spin_lock(&ap->ap_intrlock);
                ap->ap_is |= pis;
                if (ap->ap_waiting && ((pis & AHCI_PxI_ERRORS) != 0 ||
                    (ap_read(ap, AHCI_PxCI) & 1) == 0)) {
                        ap->ap_waiting = 0;
                        vsema(&ap->ap_done);
                }
                spin_unlock(&ap->ap_intrlock, s);
        }
        ah_write(sc, AHCI_IS, is);
        return 1;
}

static int
ahci_rw(struct blkdev *bd, uint64_t lba, const void *buf, size_t nsec,
    int write)
{
        struct ahci_port *ap = bd->bd_softc;
        int error;

        mutex_lock(&ap->ap_lock);
        ap_build(ap, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba,
            (uint16_t)nsec, buf, nsec * bd->bd_secsize, write);
        error = ap_run(ap);
        mutex_unlock(&ap->ap_lock);
        return error;
}

static int
ahci_read(struct blkdev *bd, uint64_t lba, void *buf, size_t nsec)
{
        return ahci_rw(bd, lba, buf, nsec, 0);
}

static int
ahci_write(struct blkdev *bd, uint64_t lba, const void *buf, size_t nsec)
{
        return ahci_rw(bd, lba, buf, nsec, 1);
}

static int
ahci_flush(struct blkdev *bd)
{
        struct ahci_port *ap = bd->bd_softc;
        int error;

        mutex_lock(&ap->ap_lock);
        ap_build(ap, ATA_FLUSH_CACHE_EXT, 0, 0, NULL, 0, 0);
        error = ap_run(ap);
        mutex_unlock(&ap->ap_lock);
        return error;
}

/* Bring up port p; a disk on it is identified and registered. */
static void
ahci_port_attach(struct ahci_softc *sc, unsigned int p)
{
        struct ahci_port *ap = kmem_zalloc(sizeof(*ap), KM_SLEEP);
        struct page *pg = NULL;
        uint16_t *id;
        uint32_t sig;
        uint64_t nsec;
        unsigned int secsize = AHCI_SECSIZE;
        char model[41];
        int i;

        ap->ap_sc = sc;
        ap->ap_num = p;
        if (ap_stop(ap) != 0) {
                printf("ahci: port %u does not stop\n", p);
                goto fail;
        }
        if (AHCI_SSTS_DET(ap_read(ap, AHCI_PxSSTS)) != 3)
                goto fail;                      /* nothing there */

        /*
         * The device's signature comes in its first register FIS, which
         * needs somewhere to land: the port's memory, and FIS receipt on.
         */
        if ((pg = page_alloc(0, PA_ZERO)) == NULL)
                goto fail;
        ap->ap_page = page_to_kva(pg);
        ap->ap_pagepa = kva_to_phys(ap->ap_page);
        ap_write(ap, AHCI_PxCLB, (uint32_t)(ap->ap_pagepa + AHCI_CL_OFF));
        ap_write(ap, AHCI_PxCLBU, (uint32_t)((ap->ap_pagepa + AHCI_CL_OFF) >>
            32));
        ap_write(ap, AHCI_PxFB, (uint32_t)(ap->ap_pagepa + AHCI_FB_OFF));
        ap_write(ap, AHCI_PxFBU, (uint32_t)((ap->ap_pagepa + AHCI_FB_OFF) >>
            32));
        ap_write(ap, AHCI_PxSERR, 0xffffffff);
        ap_write(ap, AHCI_PxCMD, ap_read(ap, AHCI_PxCMD) | AHCI_PxCMD_FRE);
        if (!ap_signature(ap)) {
                /* None yet: reset the link (COMRESET) and wait again. */
                ap_write(ap, AHCI_PxSCTL, AHCI_SCTL_DET_INIT);
                ah_spin(2);
                ap_write(ap, AHCI_PxSCTL, 0);
                (void)ap_wait(ap, AHCI_PxSSTS, 0xf, 3, 1000);
                ap_write(ap, AHCI_PxSERR, 0xffffffff);
                if (!ap_signature(ap)) {
                        printf("ahci: port %u: no signature from its "
                            "device\n", p);
                        goto stop;
                }
        }
        sig = ap_read(ap, AHCI_PxSIG);
        if (sig != AHCI_SIG_ATA) {
                if (sig == AHCI_SIG_ATAPI)
                        printf("ahci: port %u: ATAPI, passed over\n", p);
                else
                        printf("ahci: port %u: signature %#x, passed over\n",
                            p, sig);
                goto stop;
        }
        if (ap_start(ap) != 0) {
                printf("ahci: port %u stays busy\n", p);
                goto stop;
        }

        /* IDENTIFY DEVICE, into the second half of the page. */
        id = (uint16_t *)(void *)(ap->ap_page + PAGESIZE / 2);
        ap_build(ap, ATA_IDENTIFY, 0, 0, id, 512, 0);
        ap->ap_page[AHCI_CT_OFF + 7] = 0;       /* no LBA for IDENTIFY */
        if (ap_poll(ap) != 0) {
                printf("ahci: port %u: cannot identify its disk\n", p);
                goto stop;
        }
        if ((id[ATA_ID_CMDSET2] & ATA_CMDSET2_LBA48) == 0) {
                printf("ahci: port %u: no 48-bit LBAs\n", p);
                goto stop;
        }
        nsec = 0;
        for (i = 3; i >= 0; i--)
                nsec = nsec << 16 | id[ATA_ID_LBA48 + i];
        if (ATA_SECSIZE_VALID(id[ATA_ID_SECSIZE]) &&
            (id[ATA_ID_SECSIZE] & ATA_SECSIZE_LARGE) != 0)
                secsize = 2 * (id[ATA_ID_LSECSIZE] |
                    (uint32_t)id[ATA_ID_LSECSIZE + 1] << 16);
        for (i = 0; i < 20; i++) {
                model[2 * i] = (char)(id[ATA_ID_MODEL + i] >> 8);
                model[2 * i + 1] = (char)id[ATA_ID_MODEL + i];
        }
        for (i = 40; i > 0 && (model[i - 1] == ' ' || model[i - 1] == 0); i--)
                continue;
        model[i] = '\0';

        mutex_init(&ap->ap_lock, "ahciport");
        sema_init(&ap->ap_done, 0, "ahci");
        spinlock_init(&ap->ap_intrlock, "ahciintr");
        ap->ap_bd.bd_secsize = secsize;
        ap->ap_bd.bd_nsectors = nsec;
        ap->ap_bd.bd_maxio = AHCI_MAXIO;
        ap->ap_bd.bd_ops = &ahci_ops;
        ap->ap_bd.bd_softc = ap;
        ap_write(ap, AHCI_PxIE, AHCI_PxI_DHRS | AHCI_PxI_PSS | AHCI_PxI_DSS |
            AHCI_PxI_SDBS | AHCI_PxI_ERRORS);
        sc->ah_ports[p] = ap;
        if (blkdev_register(&ap->ap_bd, "sd") != 0) {
                sc->ah_ports[p] = NULL;
                return;
        }
        printf("%s: %s, on port %u\n", ap->ap_bd.bd_name, model, p);
        return;
stop:
        (void)ap_stop(ap);
        page_free(pg, 0);
fail:
        kmem_free(ap, sizeof(*ap));
}

static int
ahci_match(const struct pci_dev *pd)
{
        return pd->pd_class == AHCI_PCI_CLASS &&
            pd->pd_subclass == AHCI_PCI_SUBCLASS &&
            pd->pd_progif == AHCI_PCI_PROGIF;
}

static int
ahci_attach(struct pci_dev *pd)
{
        struct ahci_softc *sc;
        const struct pci_bar *bar = &pd->pd_bar[AHCI_BAR];
        uint64_t end;
        uint32_t cap;
        void *ih;

        if (bar->pb_addr == 0 || bar->pb_io) {
                printf("ahci %u:%02u.%u: no registers\n", pd->pd_bus,
                    pd->pd_dev, pd->pd_fn);
                return -1;
        }
        sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
        sc->ah_pd = pd;
        /* Up to 32 ports' registers; some HBAs (ICH9) map fewer. */
        sc->ah_maplen = bar->pb_size < AHCI_PORT(AHCI_NPORTS) ?
            (size_t)bar->pb_size : AHCI_PORT(AHCI_NPORTS);
        sc->ah_regs = pci_map_bar(pd, AHCI_BAR, 0, sc->ah_maplen);
        if (sc->ah_regs == NULL) {
                printf("ahci: cannot map its registers\n");
                goto fail;
        }
        pci_enable(pd, 0);

        /* Reset the HBA, then enable AHCI with interrupts off. */
        ah_write(sc, AHCI_GHC, AHCI_GHC_AE);
        ah_write(sc, AHCI_GHC, AHCI_GHC_AE | AHCI_GHC_HR);
        end = gethrtime() + 1000000000ull;
        while ((ah_read(sc, AHCI_GHC) & AHCI_GHC_HR) != 0)
                if (gethrtime() > end) {
                        printf("ahci: does not come out of reset\n");
                        goto fail;
                }
        ah_write(sc, AHCI_GHC, AHCI_GHC_AE);
        cap = ah_read(sc, AHCI_CAP);
        sc->ah_pi = ah_read(sc, AHCI_PI);

        if (pci_msix_count(pd) > 0)
                ih = pci_msix_establish(pd, 0, 0, ahci_intr, sc, "ahci");
        else
                ih = pci_msi_establish(pd, 0, ahci_intr, sc, "ahci");
        if (ih == NULL) {
                printf("ahci %u:%02u.%u: no MSI\n", pd->pd_bus, pd->pd_dev,
                    pd->pd_fn);
                goto fail;
        }
        printf("ahci %u:%02u.%u: AHCI %u.%u, %u ports, %u slots\n",
            pd->pd_bus, pd->pd_dev, pd->pd_fn, ah_read(sc, AHCI_VS) >> 16,
            (ah_read(sc, AHCI_VS) >> 8) & 0xff, AHCI_CAP_NP(cap),
            AHCI_CAP_NCS(cap));
        for (unsigned int p = 0; p < AHCI_NPORTS; p++)
                if ((sc->ah_pi & (1u << p)) != 0 &&
                    AHCI_PORT(p + 1) <= sc->ah_maplen)
                        ahci_port_attach(sc, p);
        ah_write(sc, AHCI_IS, 0xffffffff);
        ah_write(sc, AHCI_GHC, AHCI_GHC_AE | AHCI_GHC_IE);
        pd->pd_softc = sc;
        return 0;
fail:
        kmem_free(sc, sizeof(*sc));
        return -1;
}

const struct pci_driver ahci_pci_driver = {
        "ahci", ahci_match, ahci_attach
};
