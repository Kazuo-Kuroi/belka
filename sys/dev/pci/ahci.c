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
 * A port has ap_nslots command slots, each with its own command table
 * (room for AHCI_NPRD PRDs): as many as both the HBA and the disk can
 * queue, if both do native command queueing, else one. Reads and
 * writes are then READ and WRITE FPDMA QUEUED, the slot being the tag,
 * issued by PxSACT and PxCI, many at once; without NCQ, READ and WRITE
 * DMA EXT one at a time. A flush (FLUSH CACHE EXT) is never queued: it
 * waits for the port to be idle and runs alone. At attach, IDENTIFY
 * DEVICE is polled in slot 0.
 *
 * A requester sleeps on its slot's semaphore. The interrupt (MSI, or
 * MSI-X, one for the HBA, in its interrupt thread) clears each port's
 * causes and wakes the requester of every slot no longer in PxSACT or
 * PxCI. An error (a task file error, or the HBA's), or a command not
 * done in blkdev_timeout ticks (a callout), has its requester recover
 * the port (ap_recover): stopped, so that nothing more is read or
 * written, every command in flight failed, its link reset (COMRESET),
 * and started again. A requester returns only once the port is
 * stopped if its command went wrong, so its buffer is no longer the
 * HBA's.
 *
 * Locks: ap_qlock (an mrlock) is held shared while a queued command is
 * issued, and exclusive by a command that is not queued (from issue to
 * completion) and by a recovery. ap_intrlock (a spin lock) covers the
 * slots' states, the free and active masks, and the port's idleness
 * (ap_idlesv), which the handler, the timeout callouts and a recovery
 * change under it.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
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
#define AHCI_NPRD       32              /* in each command table */
#define AHCI_MAXSLOTS   32
#define AHCI_CT_SIZE    (AHCI_CT_PRDT + AHCI_NPRD * 16)  /* 0x280, aligned */
#define AHCI_CL_OFF     0x000           /* in the port's first page */
#define AHCI_FB_OFF     0x400
#define AHCI_ID_OFF     0x800           /* IDENTIFY's data, at attach */
#define AHCI_TIMEOUT    5000            /* ms, at attach */

/* A slot's state, under ap_intrlock. */
#define SL_IDLE         0
#define SL_PENDING      1               /* with the HBA */
#define SL_DONE         2
#define SL_TIMEDOUT     3               /* its callout came first */
#define SL_ERROR        4               /* the port reported an error */
#define SL_FAILED       5               /* a recovery took it away */

struct ahci_softc;
struct ahci_port;

struct ahci_slot {
        struct ahci_port *sl_ap;
        unsigned int    sl_tag;
        int             sl_state;
        struct sema     sl_done;
        uint8_t         *sl_ct;         /* its command table */
        paddr_t         sl_ctpa;
};

struct ahci_port {
        struct blkdev   ap_bd;
        struct ahci_softc *ap_sc;
        unsigned int    ap_num;
        uint8_t         *ap_page;       /* command list, FISes, IDENTIFY */
        paddr_t         ap_pagepa;
        unsigned int    ap_nslots;
        int             ap_ncq;
        struct mrlock   ap_qlock;
        unsigned int    ap_resets;      /* recoveries; under ap_qlock */
        struct sema     ap_free;        /* a unit per free slot */
        struct spinlock ap_intrlock;
        uint32_t        ap_freemask;
        uint32_t        ap_active;      /* slots issued, not yet done */
        struct sv       ap_idlesv;      /* ap_active is 0 */
        struct ahci_slot ap_slot[AHCI_MAXSLOTS];
};

struct ahci_softc {
        struct pci_dev  *ah_pd;
        volatile uint8_t *ah_regs;
        size_t          ah_maplen;      /* bytes of registers mapped */
        uint32_t        ah_cap;
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

/* Spin for about ms milliseconds. */
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

/* Reset the link (COMRESET), and wait for it to come back. */
static void
ap_comreset(struct ahci_port *ap)
{
        ap_write(ap, AHCI_PxSCTL, AHCI_SCTL_DET_INIT);
        ah_spin(2);
        ap_write(ap, AHCI_PxSCTL, 0);
        (void)ap_wait(ap, AHCI_PxSSTS, 0xf, 3, 1000);
        ap_write(ap, AHCI_PxSERR, 0xffffffff);
}

/*
 * Build the command in slot sl: cmd at lba for nsec sectors, with len
 * bytes at buf (none if NULL), toward the device if write; queued (NCQ,
 * the slot its tag) if ncq.
 */
static void
ap_build(struct ahci_port *ap, struct ahci_slot *sl, uint8_t cmd,
    uint64_t lba, uint16_t nsec, const void *buf, size_t len, int write,
    int ncq)
{
        volatile struct ahci_cmdhdr *ch = (volatile struct ahci_cmdhdr *)
            (volatile void *)(ap->ap_page + AHCI_CL_OFF) + sl->sl_tag;
        uint8_t *ct = sl->sl_ct;
        struct ahci_prd *prd = (struct ahci_prd *)(void *)(ct + AHCI_CT_PRDT);
        uint8_t *fis = ct + AHCI_CT_CFIS;
        uintptr_t va = (uintptr_t)buf;
        int n = 0;

        memset(ct, 0, AHCI_CT_SIZE);
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
        if (ncq) {
                /* The count in the features; the tag in the count. */
                fis[3] = (uint8_t)nsec;
                fis[11] = (uint8_t)(nsec >> 8);
                fis[12] = (uint8_t)(sl->sl_tag << 3);
        } else {
                fis[12] = (uint8_t)nsec;
                fis[13] = (uint8_t)(nsec >> 8);
        }
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
        ch->ch_ctba = (uint32_t)sl->sl_ctpa;
        ch->ch_ctbau = (uint32_t)(sl->sl_ctpa >> 32);
        membar_producer();
}

/* Run slot 0's command, polling (at attach); 0 or an errno value. */
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
        if ((is & AHCI_PxI_ERRORS) != 0 ||
            (ap_read(ap, AHCI_PxTFD) & AHCI_TFD_ERR) != 0)
                return EIO;
        return 0;
}

/*
 * Recover the port, unless another recovery has come since resets (the
 * caller's count when its command went out): stopped, every command in
 * flight failed, the link reset, started again. ap_qlock is held
 * exclusive if held, else taken.
 */
static void
ap_recover(struct ahci_port *ap, unsigned int resets, int held, int why)
{
        int s;

        if (!held)
                mrupdate(&ap->ap_qlock);
        if (ap->ap_resets == resets) {
                printf("%s: %s; resetting the port\n", ap->ap_bd.bd_name,
                    why == SL_TIMEDOUT ? "a command timed out" :
                    "a command failed");
                (void)ap_stop(ap);
                s = spin_lock(&ap->ap_intrlock);
                for (unsigned int i = 0; i < ap->ap_nslots; i++) {
                        struct ahci_slot *sl = &ap->ap_slot[i];

                        if (sl->sl_state == SL_PENDING) {
                                sl->sl_state = SL_FAILED;
                                vsema(&sl->sl_done);
                        }
                }
                ap->ap_active = 0;
                sv_broadcast(&ap->ap_idlesv);
                spin_unlock(&ap->ap_intrlock, s);
                ap_comreset(ap);
                if (ap_start(ap) != 0)
                        printf("%s: the port stays busy\n",
                            ap->ap_bd.bd_name);
                ap->ap_resets++;
        }
        if (!held)
                mrunlock(&ap->ap_qlock);
}

/* A slot's callout: blkdev_timeout ticks and no completion. */
static void
ahci_timeout(void *arg)
{
        struct ahci_slot *sl = arg;
        struct ahci_port *ap = sl->sl_ap;
        int s = spin_lock(&ap->ap_intrlock);

        if (sl->sl_state == SL_PENDING) {
                sl->sl_state = SL_TIMEDOUT;
                vsema(&sl->sl_done);
        }
        spin_unlock(&ap->ap_intrlock, s);
}

/* The HBA's interrupt: each port's causes, its finished slots woken. */
static int
ahci_intr(void *arg)
{
        struct ahci_softc *sc = arg;
        uint32_t is = ah_read(sc, AHCI_IS);

        if (is == 0)
                return 0;
        for (unsigned int p = 0; p < AHCI_NPORTS; p++) {
                struct ahci_port *ap = sc->ah_ports[p];
                uint32_t pis, busy, done;
                int s;

                if ((is & (1u << p)) == 0)
                        continue;
                pis = ah_read(sc, AHCI_PORT(p) + AHCI_PxIS);
                ah_write(sc, AHCI_PORT(p) + AHCI_PxIS, pis);
                if (ap == NULL)
                        continue;
                s = spin_lock(&ap->ap_intrlock);
                if ((pis & AHCI_PxI_ERRORS) != 0) {
                        /* Every command in flight: the port must recover. */
                        for (unsigned int i = 0; i < ap->ap_nslots; i++) {
                                struct ahci_slot *sl = &ap->ap_slot[i];

                                if (sl->sl_state == SL_PENDING) {
                                        sl->sl_state = SL_ERROR;
                                        vsema(&sl->sl_done);
                                }
                        }
                        spin_unlock(&ap->ap_intrlock, s);
                        continue;
                }
                busy = ap_read(ap, AHCI_PxSACT) | ap_read(ap, AHCI_PxCI);
                done = ap->ap_active & ~busy;
                for (unsigned int i = 0; done != 0; i++, done >>= 1) {
                        struct ahci_slot *sl = &ap->ap_slot[i];

                        if ((done & 1) == 0)
                                continue;
                        ap->ap_active &= ~(1u << i);
                        if (sl->sl_state != SL_PENDING || blkdev_lose_one())
                                continue;
                        sl->sl_state = SL_DONE;
                        vsema(&sl->sl_done);
                }
                if (ap->ap_active == 0)
                        sv_broadcast(&ap->ap_idlesv);
                spin_unlock(&ap->ap_intrlock, s);
        }
        ah_write(sc, AHCI_IS, is);
        return 1;
}

/*
 * One command on ap, waited for: queued if ncq (many may be out), else
 * alone on an idle port. 0 or an errno value.
 */
static int
ap_cmd(struct ahci_port *ap, uint8_t cmd, uint64_t lba, uint16_t nsec,
    const void *buf, size_t len, int write, int ncq)
{
        struct ahci_slot *sl;
        unsigned int tag, resets;
        uint32_t bit;
        toid_t toid;
        int s, state;

        if (ncq) {
                mraccess(&ap->ap_qlock);
        } else {
                mrupdate(&ap->ap_qlock);
                s = spin_lock(&ap->ap_intrlock);
                while (ap->ap_active != 0) {
                        sv_wait(&ap->ap_idlesv, &ap->ap_intrlock, s);
                        s = spin_lock(&ap->ap_intrlock);
                }
                spin_unlock(&ap->ap_intrlock, s);
        }
        psema(&ap->ap_free);
        resets = ap->ap_resets;
        s = spin_lock(&ap->ap_intrlock);
        for (tag = 0; (ap->ap_freemask & (1u << tag)) == 0; tag++)
                continue;
        bit = 1u << tag;
        ap->ap_freemask &= ~bit;
        sl = &ap->ap_slot[tag];
        spin_unlock(&ap->ap_intrlock, s);

        ap_build(ap, sl, cmd, lba, nsec, buf, len, write, ncq);
        toid = timeout(ahci_timeout, sl, blkdev_timeout);
        /*
         * Issued under ap_intrlock, which the handler takes: it must not
         * see the slot active before PxSACT and PxCI say it is busy.
         */
        s = spin_lock(&ap->ap_intrlock);
        sl->sl_state = SL_PENDING;
        ap->ap_active |= bit;
        if (ncq)
                ap_write(ap, AHCI_PxSACT, bit);
        ap_write(ap, AHCI_PxCI, bit);
        spin_unlock(&ap->ap_intrlock, s);
        if (ncq)
                mrunlock(&ap->ap_qlock);

        psema(&sl->sl_done);
        (void)untimeout(toid);
        s = spin_lock(&ap->ap_intrlock);
        state = sl->sl_state;
        spin_unlock(&ap->ap_intrlock, s);
        /* Gone wrong: the port stopped before the buffer is given back. */
        if (state == SL_TIMEDOUT || state == SL_ERROR)
                ap_recover(ap, resets, !ncq, state);
        if (!ncq)
                mrunlock(&ap->ap_qlock);
        s = spin_lock(&ap->ap_intrlock);
        sl->sl_state = SL_IDLE;
        ap->ap_active &= ~bit;
        ap->ap_freemask |= bit;
        if (ap->ap_active == 0)
                sv_broadcast(&ap->ap_idlesv);
        spin_unlock(&ap->ap_intrlock, s);
        vsema(&ap->ap_free);
        return state == SL_DONE ? 0 : state == SL_TIMEDOUT ? ETIMEDOUT : EIO;
}

static int
ahci_rw(struct blkdev *bd, uint64_t lba, const void *buf, size_t nsec,
    int write)
{
        struct ahci_port *ap = bd->bd_softc;
        uint8_t cmd = ap->ap_ncq ?
            (write ? ATA_WRITE_FPDMA_QUEUED : ATA_READ_FPDMA_QUEUED) :
            (write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT);

        return ap_cmd(ap, cmd, lba, (uint16_t)nsec, buf,
            nsec * bd->bd_secsize, write, ap->ap_ncq);
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
        return ap_cmd(bd->bd_softc, ATA_FLUSH_CACHE_EXT, 0, 0, NULL, 0, 0, 0);
}

/* The port's slots and their command tables, from slot 0. */
static int
ap_slots(struct ahci_port *ap, unsigned int nslots)
{
        uint8_t *va = NULL;
        size_t off = PAGESIZE;

        for (unsigned int i = 0; i < nslots; i++) {
                struct ahci_slot *sl = &ap->ap_slot[i];

                if (off + AHCI_CT_SIZE > PAGESIZE) {
                        struct page *pg = page_alloc(0, PA_ZERO);

                        if (pg == NULL)
                                return -1;
                        va = page_to_kva(pg);
                        off = 0;
                }
                sl->sl_ap = ap;
                sl->sl_tag = i;
                sl->sl_ct = va + off;
                sl->sl_ctpa = kva_to_phys(va + off);
                sema_init(&sl->sl_done, 0, "ahci");
                off += AHCI_CT_SIZE;
        }
        ap->ap_nslots = nslots;
        ap->ap_freemask = nslots == 32 ? 0xffffffffu : (1u << nslots) - 1;
        return 0;
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
        unsigned int secsize = AHCI_SECSIZE, nslots = 1;
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
                ap_comreset(ap);
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
        if (ap_slots(ap, 1) != 0 || ap_start(ap) != 0) {
                printf("ahci: port %u stays busy\n", p);
                goto stop;
        }

        /* IDENTIFY DEVICE, polled in slot 0. */
        id = (uint16_t *)(void *)(ap->ap_page + AHCI_ID_OFF);
        ap_build(ap, &ap->ap_slot[0], ATA_IDENTIFY, 0, 0, id, 512, 0, 0);
        ap->ap_slot[0].sl_ct[AHCI_CT_CFIS + 7] = 0;     /* no LBA */
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

        /* Queued, if both the HBA and the disk can: as deep as both. */
        if ((sc->ah_cap & AHCI_CAP_SNCQ) != 0 &&
            (id[ATA_ID_SATACAP] & ATA_SATACAP_NCQ) != 0) {
                nslots = (id[ATA_ID_QDEPTH] & 0x1f) + 1u;
                if (nslots > AHCI_CAP_NCS(sc->ah_cap))
                        nslots = AHCI_CAP_NCS(sc->ah_cap);
                ap->ap_ncq = nslots > 1;
        }
        if (ap->ap_ncq && ap_slots(ap, nslots) != 0) {
                ap->ap_ncq = 0;
                nslots = 1;
        }
        mrinit(&ap->ap_qlock, "ahciq");
        sema_init(&ap->ap_free, (int)ap->ap_nslots, "ahcislot");
        spinlock_init(&ap->ap_intrlock, "ahciintr");
        sv_init(&ap->ap_idlesv, "ahciidle");
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
        if (ap->ap_ncq)
                printf("%s: %s, on port %u, NCQ %u deep\n",
                    ap->ap_bd.bd_name, model, p, ap->ap_nslots);
        else
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
        sc->ah_cap = ah_read(sc, AHCI_CAP);
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
        printf("ahci %u:%02u.%u: AHCI %u.%u, %u ports, %u slots%s\n",
            pd->pd_bus, pd->pd_dev, pd->pd_fn, ah_read(sc, AHCI_VS) >> 16,
            (ah_read(sc, AHCI_VS) >> 8) & 0xff, AHCI_CAP_NP(sc->ah_cap),
            AHCI_CAP_NCS(sc->ah_cap),
            (sc->ah_cap & AHCI_CAP_SNCQ) != 0 ? ", NCQ" : "");
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
