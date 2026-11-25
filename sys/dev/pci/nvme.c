/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-25
 * SPDX-License-Identifier: MIT
 */

/*
 * NVM Express controllers (dev/pci/nvmereg.h), written from the base
 * specification. Each namespace with blocks is a block device, nvdN.
 *
 * At attach the controller is reset and enabled with an admin queue
 * pair, through which (polled) it is identified, asked for one I/O
 * queue pair, and given it. Then the namespaces are identified and
 * registered.
 *
 * The I/O queue pair is NVME_QD entries deep. A request takes a
 * command identifier (at most NVME_QD - 1 are out, so the submission
 * queue never overflows), fills the next submission entry, rings the
 * doorbell and sleeps on its semaphore; the completion queue's MSI-X
 * handler (in the vector's interrupt thread) records each completion's
 * status and wakes its request. Data is described by PRPs: the first
 * page by PRP1, a second by PRP2, or more by a list in PRP2, each
 * command identifier having room for one. Pages are 4 KB (CC.MPS 0).
 *
 * A request not done in blkdev_timeout ticks is timed out (a callout),
 * and its requester resets the controller: disabled, every request in
 * flight failed, then enabled again with fresh queues (nvme_reset). A
 * controller that does not come back is dead, and fails everything. At
 * shutdown (uadmin) the controller is told, and waited for.
 *
 * Locks: nv_resetlock (an mrlock) is held shared while a request is
 * submitted, and exclusive by a reset, so that nothing is submitted to
 * queues being made again. nv_sqlock (a spin lock) covers the
 * submission queue's tail and the free command identifiers;
 * nv_cqlock (a spin lock) the completion queue's head and phase and
 * every request's state, which the handler, the timeout callout and a
 * reset change under it. Neither spin lock is held with the other.
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

#include "dev/pci/nvmereg.h"
#include "dev/pci/pcivar.h"

#define NVME_AQD        16              /* admin queue entries */
#define NVME_QD         64              /* I/O queue entries */
#define NVME_MAXIO      (64 * 1024)
#define NVME_PRPMAX     32              /* list entries per command */
#define NVME_MAXNS      4               /* namespaces taken */
#define NVME_REGSIZE    0x2000

/* A request's state, under nv_cqlock. */
#define NR_IDLE         0
#define NR_PENDING      1               /* with the controller */
#define NR_DONE         2               /* its completion came */
#define NR_TIMEDOUT     3               /* its callout came first */
#define NR_FAILED       4               /* a reset took it away */

struct nvme_softc;

struct nvme_req {
        struct nvme_softc *nr_sc;
        struct sema     nr_done;
        int             nr_state;
        uint16_t        nr_status;
        uint64_t        *nr_prpl;       /* NVME_PRPMAX entries */
        paddr_t         nr_prplpa;
};

struct nvme_ns {
        struct blkdev   ns_bd;
        struct nvme_softc *ns_sc;
        uint32_t        ns_id;
};

struct nvme_softc {
        struct pci_dev  *nv_pd;
        volatile uint8_t *nv_regs;
        uint64_t        nv_cap;
        uint32_t        nv_dstrd;
        uint32_t        nv_iv;          /* the I/O completions' vector */
        int             nv_vwc;         /* a volatile write cache */
        uint32_t        nv_rtd3e;       /* shutdown latency, us (0: ?) */
        size_t          nv_maxio;
        uint8_t         *nv_id;         /* a page for identify data */
        paddr_t         nv_idpa;

        /* The admin queue pair, polled (attach, reset). */
        volatile struct nvme_sqe *nv_asq;
        volatile struct nvme_cqe *nv_acq;
        paddr_t         nv_asqpa, nv_acqpa;
        unsigned int    nv_asqt, nv_acqh, nv_aphase;

        /* The I/O queue pair. */
        struct mrlock   nv_resetlock;
        unsigned int    nv_resets;      /* made so far; under the lock */
        int             nv_dead;
        struct spinlock nv_sqlock;
        volatile struct nvme_sqe *nv_sq;
        paddr_t         nv_sqpa;
        unsigned int    nv_sqt;
        uint16_t        nv_free[NVME_QD];
        unsigned int    nv_nfree;
        struct sema     nv_slots;
        struct spinlock nv_cqlock;
        volatile struct nvme_cqe *nv_cq;
        paddr_t         nv_cqpa;
        unsigned int    nv_cqh, nv_phase;
        struct nvme_req nv_req[NVME_QD];

        struct nvme_ns  nv_ns[NVME_MAXNS];
};

static int nvme_read(struct blkdev *, uint64_t, void *, size_t);
static int nvme_write(struct blkdev *, uint64_t, const void *, size_t);
static int nvme_flush(struct blkdev *);

static const struct blkdev_ops nvme_ops = {
        nvme_read, nvme_write, nvme_flush
};

static uint32_t
nv_read(struct nvme_softc *sc, uint32_t reg)
{
        return *(volatile uint32_t *)(volatile void *)(sc->nv_regs + reg);
}

static void
nv_write(struct nvme_softc *sc, uint32_t reg, uint32_t v)
{
        *(volatile uint32_t *)(volatile void *)(sc->nv_regs + reg) = v;
}

static void
nv_write64(struct nvme_softc *sc, uint32_t reg, uint64_t v)
{
        nv_write(sc, reg, (uint32_t)v);
        nv_write(sc, reg + 4, (uint32_t)(v >> 32));
}

/* A zeroed page, its physical address in *pa. */
static void *
nv_page(paddr_t *pa)
{
        struct page *pg = page_alloc(0, PA_ZERO);
        void *va;

        if (pg == NULL)
                panic("nvme: out of memory");
        va = page_to_kva(pg);
        *pa = kva_to_phys(va);
        return va;
}

/* Wait until CSTS.RDY is rdy, as long as CAP.TO allows; 0, or -1. */
static int
nv_waitrdy(struct nvme_softc *sc, uint32_t rdy)
{
        uint64_t ms = 500 * ((uint64_t)NVME_CAP_TO(sc->nv_cap) + 1);
        uint64_t end = gethrtime() + ms * 1000000;

        while ((nv_read(sc, NVME_CSTS) & NVME_CSTS_RDY) != rdy) {
                if ((nv_read(sc, NVME_CSTS) & NVME_CSTS_CFS) != 0 ||
                    gethrtime() > end)
                        return -1;
        }
        return 0;
}

/* One admin command, polled; its status (0: success), or -1. */
static int
nv_admin(struct nvme_softc *sc, struct nvme_sqe *sqe, uint32_t *dw0)
{
        uint64_t end = gethrtime() + 5000000000ull;
        volatile struct nvme_cqe *cqe;
        uint16_t status;

        sqe->sqe_cid = (uint16_t)sc->nv_asqt;
        memcpy((void *)(uintptr_t)&sc->nv_asq[sc->nv_asqt], sqe, sizeof(*sqe));
        sc->nv_asqt = (sc->nv_asqt + 1) % NVME_AQD;
        membar_producer();
        nv_write(sc, NVME_SQTDBL(0, sc->nv_dstrd), sc->nv_asqt);
        cqe = &sc->nv_acq[sc->nv_acqh];
        while (NVME_CQE_PHASE(cqe->cqe_status) != sc->nv_aphase)
                if (gethrtime() > end)
                        return -1;
        membar_consumer();
        status = cqe->cqe_status;
        if (dw0 != NULL)
                *dw0 = cqe->cqe_dw0;
        if (++sc->nv_acqh == NVME_AQD) {
                sc->nv_acqh = 0;
                sc->nv_aphase ^= 1;
        }
        nv_write(sc, NVME_CQHDBL(0, sc->nv_dstrd), sc->nv_acqh);
        return NVME_CQE_STATUS(status);
}

/* Disable the controller, then enable it with empty admin queues. */
static int
nv_enable(struct nvme_softc *sc)
{
        nv_write(sc, NVME_CC, 0);
        if (nv_waitrdy(sc, 0) != 0) {
                printf("nvme: does not stop\n");
                return -1;
        }
        memset((void *)(uintptr_t)sc->nv_asq, 0, PAGESIZE);
        memset((void *)(uintptr_t)sc->nv_acq, 0, PAGESIZE);
        sc->nv_asqt = sc->nv_acqh = 0;
        sc->nv_aphase = 1;
        nv_write(sc, NVME_AQA, (NVME_AQD - 1) << 16 | (NVME_AQD - 1));
        nv_write64(sc, NVME_ASQ, sc->nv_asqpa);
        nv_write64(sc, NVME_ACQ, sc->nv_acqpa);
        nv_write(sc, NVME_INTMS, 0xffffffff);
        nv_write(sc, NVME_CC, NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS(0) |
            NVME_CC_IOSQES(6) | NVME_CC_IOCQES(4));
        if (nv_waitrdy(sc, NVME_CSTS_RDY) != 0) {
                printf("nvme: does not become ready\n");
                return -1;
        }
        return 0;
}

/* The I/O queue pair, empty, its completions on vector nv_iv; 0, or -1. */
static int
nv_mkqueues(struct nvme_softc *sc)
{
        struct nvme_sqe sqe;

        memset((void *)(uintptr_t)sc->nv_sq, 0, PAGESIZE);
        memset((void *)(uintptr_t)sc->nv_cq, 0, PAGESIZE);
        sc->nv_sqt = sc->nv_cqh = 0;
        sc->nv_phase = 1;
        memset(&sqe, 0, sizeof(sqe));
        sqe.sqe_opc = NVME_ADM_SET_FEATURES;
        sqe.sqe_cdw10 = NVME_FEAT_NQUEUES;
        sqe.sqe_cdw11 = 0;                      /* one of each, less one */
        if (nv_admin(sc, &sqe, NULL) != 0)
                return -1;
        memset(&sqe, 0, sizeof(sqe));
        sqe.sqe_opc = NVME_ADM_CREATE_CQ;
        sqe.sqe_prp1 = sc->nv_cqpa;
        sqe.sqe_cdw10 = (uint32_t)(NVME_QD - 1) << 16 | 1;
        sqe.sqe_cdw11 = sc->nv_iv << 16 | NVME_CQ_IEN | NVME_QUEUE_PC;
        if (nv_admin(sc, &sqe, NULL) != 0)
                return -1;
        memset(&sqe, 0, sizeof(sqe));
        sqe.sqe_opc = NVME_ADM_CREATE_SQ;
        sqe.sqe_prp1 = sc->nv_sqpa;
        sqe.sqe_cdw10 = (uint32_t)(NVME_QD - 1) << 16 | 1;
        sqe.sqe_cdw11 = 1u << 16 | NVME_QUEUE_PC;       /* completions: 1 */
        if (nv_admin(sc, &sqe, NULL) != 0)
                return -1;
        nv_write(sc, NVME_INTMC, 0xffffffff);
        return 0;
}

/* The I/O completion queue's interrupt. */
static int
nvme_intr(void *arg)
{
        struct nvme_softc *sc = arg;
        int s = spin_lock(&sc->nv_cqlock), n = 0;

        for (;;) {
                volatile struct nvme_cqe *cqe = &sc->nv_cq[sc->nv_cqh];
                uint16_t status = cqe->cqe_status;
                struct nvme_req *r;
                uint16_t cid;

                if (NVME_CQE_PHASE(status) != sc->nv_phase)
                        break;
                membar_consumer();
                cid = cqe->cqe_cid;
                if (++sc->nv_cqh == NVME_QD) {
                        sc->nv_cqh = 0;
                        sc->nv_phase ^= 1;
                }
                n++;
                if (cid >= NVME_QD)
                        continue;
                r = &sc->nv_req[cid];
                if (r->nr_state != NR_PENDING || blkdev_lose_one())
                        continue;
                r->nr_status = NVME_CQE_STATUS(status);
                r->nr_state = NR_DONE;
                vsema(&r->nr_done);
        }
        if (n > 0)
                nv_write(sc, NVME_CQHDBL(1, sc->nv_dstrd), sc->nv_cqh);
        spin_unlock(&sc->nv_cqlock, s);
        return n > 0;
}

/* A request's callout: blkdev_timeout ticks and no completion. */
static void
nvme_timeout(void *arg)
{
        struct nvme_req *r = arg;
        struct nvme_softc *sc = r->nr_sc;
        int s = spin_lock(&sc->nv_cqlock);

        if (r->nr_state == NR_PENDING) {
                r->nr_state = NR_TIMEDOUT;
                vsema(&r->nr_done);
        }
        spin_unlock(&sc->nv_cqlock, s);
}

/*
 * Reset the controller, unless another reset has come since resets (a
 * request's own count when it was sent): every request in flight
 * fails, and the queues are made again. A controller that does not
 * come back is dead.
 */
static void
nvme_reset(struct nvme_softc *sc, unsigned int resets)
{
        int s;

        mrupdate(&sc->nv_resetlock);
        if (sc->nv_resets != resets || sc->nv_dead) {
                mrunlock(&sc->nv_resetlock);
                return;
        }
        printf("nvme %u:%02u.%u: a command timed out; resetting\n",
            sc->nv_pd->pd_bus, sc->nv_pd->pd_dev, sc->nv_pd->pd_fn);
        nv_write(sc, NVME_CC, 0);
        (void)nv_waitrdy(sc, 0);
        /* Nothing more comes from it now: fail what it had. */
        s = spin_lock(&sc->nv_cqlock);
        for (int i = 0; i < NVME_QD; i++) {
                struct nvme_req *r = &sc->nv_req[i];

                if (r->nr_state == NR_PENDING) {
                        r->nr_state = NR_FAILED;
                        vsema(&r->nr_done);
                }
        }
        spin_unlock(&sc->nv_cqlock, s);
        if (nv_enable(sc) != 0 || nv_mkqueues(sc) != 0) {
                printf("nvme %u:%02u.%u: does not come back; given up\n",
                    sc->nv_pd->pd_bus, sc->nv_pd->pd_dev, sc->nv_pd->pd_fn);
                sc->nv_dead = 1;
        }
        sc->nv_resets++;
        mrunlock(&sc->nv_resetlock);
}

/* PRP1 and PRP2 for len bytes at buf, with r's list if need be. */
static void
nvme_prp(struct nvme_req *r, const void *buf, size_t len, uint64_t *prp1,
    uint64_t *prp2)
{
        uintptr_t va = (uintptr_t)buf;
        size_t first = PAGESIZE - (va & PAGEOFFSET);
        paddr_t pa;
        int n = 0;

        if (hat_kextract(va, &pa) != 0)
                panic("nvme: buffer %p is not mapped", buf);
        *prp1 = pa;
        *prp2 = 0;
        if (len <= first)
                return;
        len -= first;
        va += first;
        if (len <= PAGESIZE) {
                if (hat_kextract(va, &pa) != 0)
                        panic("nvme: buffer %p is not mapped", (void *)va);
                *prp2 = pa;
                return;
        }
        while (len > 0) {
                size_t chunk = len < PAGESIZE ? len : PAGESIZE;

                if (hat_kextract(va, &pa) != 0)
                        panic("nvme: buffer %p is not mapped", (void *)va);
                r->nr_prpl[n++] = pa;
                va += chunk;
                len -= chunk;
        }
        *prp2 = r->nr_prplpa;
}

/* One I/O command on namespace ns, waited for; 0 or an errno value. */
static int
nvme_io(struct nvme_ns *ns, uint8_t opc, uint64_t lba, const void *buf,
    size_t nblk)
{
        struct nvme_softc *sc = ns->ns_sc;
        struct nvme_sqe sqe;
        struct nvme_req *r;
        unsigned int resets;
        uint16_t cid, status;
        int s, state, error;
        toid_t toid;

        memset(&sqe, 0, sizeof(sqe));
        sqe.sqe_opc = opc;
        sqe.sqe_nsid = ns->ns_id;
        psema(&sc->nv_slots);
        s = spin_lock(&sc->nv_sqlock);
        cid = sc->nv_free[--sc->nv_nfree];
        spin_unlock(&sc->nv_sqlock, s);
        r = &sc->nv_req[cid];
        if (buf != NULL) {
                nvme_prp(r, buf, nblk * ns->ns_bd.bd_secsize, &sqe.sqe_prp1,
                    &sqe.sqe_prp2);
                sqe.sqe_cdw10 = (uint32_t)lba;
                sqe.sqe_cdw11 = (uint32_t)(lba >> 32);
                sqe.sqe_cdw12 = (uint32_t)(nblk - 1);
        }
        sqe.sqe_cid = cid;

        mraccess(&sc->nv_resetlock);
        resets = sc->nv_resets;
        if (sc->nv_dead) {
                mrunlock(&sc->nv_resetlock);
                error = EIO;
                goto out;
        }
        s = spin_lock(&sc->nv_cqlock);
        r->nr_state = NR_PENDING;
        r->nr_status = 0xffff;
        spin_unlock(&sc->nv_cqlock, s);
        toid = timeout(nvme_timeout, r, blkdev_timeout);
        s = spin_lock(&sc->nv_sqlock);
        memcpy((void *)(uintptr_t)&sc->nv_sq[sc->nv_sqt], &sqe, sizeof(sqe));
        sc->nv_sqt = (sc->nv_sqt + 1) % NVME_QD;
        membar_producer();
        nv_write(sc, NVME_SQTDBL(1, sc->nv_dstrd), sc->nv_sqt);
        spin_unlock(&sc->nv_sqlock, s);
        mrunlock(&sc->nv_resetlock);

        psema(&r->nr_done);
        (void)untimeout(toid);
        s = spin_lock(&sc->nv_cqlock);
        state = r->nr_state;
        status = r->nr_status;
        r->nr_state = NR_IDLE;
        spin_unlock(&sc->nv_cqlock, s);
        if (state == NR_TIMEDOUT)
                nvme_reset(sc, resets);
        error = state == NR_DONE && status == 0 ? 0 :
            state == NR_TIMEDOUT ? ETIMEDOUT : EIO;
out:
        s = spin_lock(&sc->nv_sqlock);
        sc->nv_free[sc->nv_nfree++] = cid;
        spin_unlock(&sc->nv_sqlock, s);
        vsema(&sc->nv_slots);
        return error;
}

static int
nvme_read(struct blkdev *bd, uint64_t lba, void *buf, size_t nblk)
{
        return nvme_io(bd->bd_softc, NVME_CMD_READ, lba, buf, nblk);
}

static int
nvme_write(struct blkdev *bd, uint64_t lba, const void *buf, size_t nblk)
{
        return nvme_io(bd->bd_softc, NVME_CMD_WRITE, lba, buf, nblk);
}

static int
nvme_flush(struct blkdev *bd)
{
        struct nvme_ns *ns = bd->bd_softc;

        if (!ns->ns_sc->nv_vwc)
                return 0;
        return nvme_io(ns, NVME_CMD_FLUSH, 0, NULL, 0);
}

/*
 * At shutdown (uadmin, after the file systems are written back): a
 * normal shutdown, waited for as long as the controller says it takes
 * (RTD3E), at least a second and at most ten.
 */
static void
nvme_shutdown(void *arg)
{
        struct nvme_softc *sc = arg;
        uint64_t us = sc->nv_rtd3e, end;

        if (sc->nv_dead)
                return;
        if (us < 1000000)
                us = 1000000;
        if (us > 10000000)
                us = 10000000;
        mrupdate(&sc->nv_resetlock);
        nv_write(sc, NVME_CC, (nv_read(sc, NVME_CC) & ~NVME_CC_SHN_MASK) |
            NVME_CC_SHN_NORMAL);
        end = gethrtime() + us * 1000;
        while ((nv_read(sc, NVME_CSTS) & NVME_CSTS_SHST_MASK) !=
            NVME_CSTS_SHST_DONE) {
                if (gethrtime() > end) {
                        printf("nvme %u:%02u.%u: shutdown not complete\n",
                            sc->nv_pd->pd_bus, sc->nv_pd->pd_dev,
                            sc->nv_pd->pd_fn);
                        break;
                }
        }
        if ((nv_read(sc, NVME_CSTS) & NVME_CSTS_SHST_MASK) ==
            NVME_CSTS_SHST_DONE)
                printf("nvme %u:%02u.%u: shut down\n", sc->nv_pd->pd_bus,
                    sc->nv_pd->pd_dev, sc->nv_pd->pd_fn);
        sc->nv_dead = 1;                /* no more commands */
        mrunlock(&sc->nv_resetlock);
}

/* Identify (cns, nsid) into the identify page; it, or NULL. */
static uint8_t *
nv_identify(struct nvme_softc *sc, uint32_t cns, uint32_t nsid)
{
        struct nvme_sqe sqe;

        memset(&sqe, 0, sizeof(sqe));
        sqe.sqe_opc = NVME_ADM_IDENTIFY;
        sqe.sqe_nsid = nsid;
        sqe.sqe_prp1 = sc->nv_idpa;
        sqe.sqe_cdw10 = cns;
        if (nv_admin(sc, &sqe, NULL) != 0)
                return NULL;
        return sc->nv_id;
}

/* Trailing blanks off an identify string of len bytes, into out. */
static void
nv_string(char *out, const uint8_t *in, size_t len)
{
        while (len > 0 && (in[len - 1] == ' ' || in[len - 1] == '\0'))
                len--;
        memcpy(out, in, len);
        out[len] = '\0';
}

static int
nvme_match(const struct pci_dev *pd)
{
        return pd->pd_class == NVME_PCI_CLASS &&
            pd->pd_subclass == NVME_PCI_SUBCLASS &&
            pd->pd_progif == NVME_PCI_PROGIF;
}

static int
nvme_attach(struct pci_dev *pd)
{
        struct nvme_softc *sc;
        uint32_t nn;
        uint8_t *idc, *lists = NULL;
        paddr_t lpa = 0;
        char model[41], serial[21];
        int nns = 0;

        if (pd->pd_bar[0].pb_addr == 0 || pd->pd_bar[0].pb_io ||
            pci_msix_count(pd) == 0) {
                printf("nvme %u:%02u.%u: no registers or no MSI-X\n",
                    pd->pd_bus, pd->pd_dev, pd->pd_fn);
                return -1;
        }
        sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
        sc->nv_pd = pd;
        sc->nv_regs = pci_map_bar(pd, 0, 0, NVME_REGSIZE);
        if (sc->nv_regs == NULL)
                goto fail;
        pci_enable(pd, 0);
        sc->nv_cap = nv_read(sc, NVME_CAP) |
            (uint64_t)nv_read(sc, NVME_CAP + 4) << 32;
        sc->nv_dstrd = NVME_CAP_DSTRD(sc->nv_cap);
        if (NVME_CAP_MPSMIN(sc->nv_cap) != 0 ||
            NVME_CAP_MQES(sc->nv_cap) < NVME_QD ||
            NVME_DOORBELL + 4 * (4u << sc->nv_dstrd) > NVME_REGSIZE) {
                printf("nvme: unsupported controller (CAP %#jx)\n",
                    (uintmax_t)sc->nv_cap);
                goto fail;
        }

        /* The queues' and requests' memory, kept across resets. */
        sc->nv_asq = nv_page(&sc->nv_asqpa);
        sc->nv_acq = nv_page(&sc->nv_acqpa);
        sc->nv_sq = nv_page(&sc->nv_sqpa);
        sc->nv_cq = nv_page(&sc->nv_cqpa);
        sc->nv_id = nv_page(&sc->nv_idpa);
        mrinit(&sc->nv_resetlock, "nvmereset");
        spinlock_init(&sc->nv_sqlock, "nvmesq");
        spinlock_init(&sc->nv_cqlock, "nvmecq");
        sema_init(&sc->nv_slots, NVME_QD - 1, "nvme");
        for (int i = 0; i < NVME_QD; i++) {
                struct nvme_req *r = &sc->nv_req[i];
                size_t off = (size_t)i * NVME_PRPMAX * sizeof(uint64_t) %
                    PAGESIZE;

                if (off == 0)
                        lists = nv_page(&lpa);
                r->nr_sc = sc;
                r->nr_prpl = (uint64_t *)(void *)(lists + off);
                r->nr_prplpa = lpa + off;
                sema_init(&r->nr_done, 0, "nvmeio");
                if (i < NVME_QD - 1)
                        sc->nv_free[sc->nv_nfree++] = (uint16_t)i;
        }

        if (nv_enable(sc) != 0)
                goto fail;
        if ((idc = nv_identify(sc, NVME_CNS_CONTROLLER, 0)) == NULL) {
                printf("nvme: cannot identify the controller\n");
                goto fail;
        }
        nv_string(model, idc + NVME_IDC_MN, 40);
        nv_string(serial, idc + NVME_IDC_SN, 20);
        memcpy(&nn, idc + NVME_IDC_NN, sizeof(nn));
        memcpy(&sc->nv_rtd3e, idc + NVME_IDC_RTD3E, sizeof(sc->nv_rtd3e));
        sc->nv_vwc = (idc[NVME_IDC_VWC] & 1) != 0;
        sc->nv_maxio = NVME_MAXIO;
        if (idc[NVME_IDC_MDTS] != 0 && idc[NVME_IDC_MDTS] < 16 &&
            ((size_t)PAGESIZE << idc[NVME_IDC_MDTS]) < sc->nv_maxio)
                sc->nv_maxio = (size_t)PAGESIZE << idc[NVME_IDC_MDTS];

        /*
         * Completions on a vector of their own, if there are two, set up
         * first: a queue may only be given a vector that is enabled.
         */
        sc->nv_iv = pci_msix_count(pd) > 1 ? 1 : 0;
        if (pci_msix_establish(pd, (int)sc->nv_iv, 0, nvme_intr, sc,
            "nvme") == NULL || nv_mkqueues(sc) != 0) {
                printf("nvme: cannot make its I/O queues\n");
                goto fail;
        }
        printf("nvme %u:%02u.%u: %s, serial %s, %u namespace%s%s\n",
            pd->pd_bus, pd->pd_dev, pd->pd_fn, model, serial, nn,
            nn == 1 ? "" : "s", sc->nv_vwc ? ", write cache" : "");

        for (uint32_t id = 1; id <= nn && nns < NVME_MAXNS; id++) {
                struct nvme_ns *ns = &sc->nv_ns[nns];
                uint8_t *idn = nv_identify(sc, NVME_CNS_NAMESPACE, id);
                uint64_t nsze;
                uint32_t lbaf;

                if (idn == NULL)
                        continue;
                memcpy(&nsze, idn + NVME_IDN_NSZE, sizeof(nsze));
                memcpy(&lbaf, idn + NVME_IDN_LBAF +
                    4 * (idn[NVME_IDN_FLBAS] & 0xf), sizeof(lbaf));
                if (nsze == 0 || NVME_LBAF_LBADS(lbaf) < 9 ||
                    NVME_LBAF_LBADS(lbaf) > 12)
                        continue;
                ns->ns_sc = sc;
                ns->ns_id = id;
                ns->ns_bd.bd_secsize = 1u << NVME_LBAF_LBADS(lbaf);
                ns->ns_bd.bd_nsectors = nsze;
                ns->ns_bd.bd_maxio = sc->nv_maxio;
                ns->ns_bd.bd_ops = &nvme_ops;
                ns->ns_bd.bd_softc = ns;
                if (blkdev_register(&ns->ns_bd, "nvd") == 0)
                        nns++;
        }
        (void)shutdown_hook_establish(nvme_shutdown, sc);
        pd->pd_softc = sc;
        return 0;
fail:
        kmem_free(sc, sizeof(*sc));
        return -1;
}

const struct pci_driver nvme_pci_driver = {
        "nvme", nvme_match, nvme_attach
};
