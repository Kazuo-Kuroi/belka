/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * strmux, a multiplexing STREAMS driver for the tests (/dev/strmux):
 * streams are linked under it, and what an upper stream writes goes
 * down one of them (the one it chose with MUX_SELECT, or the newest),
 * and what comes up a lower stream goes to the upper stream that last
 * wrote to it (or linked it). All of its pairs, upper and lower, share
 * one syncq (SQLVL_MODULE), which is what guards its tables.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/stream.h>
#include <sys/stropts.h>
#include <sys/systm.h>

#define MUX_SELECT      (('M' << 8) | 1)        /* int: the muxid to use */
#define MUX_COUNT       (('M' << 8) | 2)        /* int: links it has */

struct mux_lower {
        struct mux_lower *ml_next;
        queue_t         *ml_wq;         /* l_qbot: the driver's, atop it */
        queue_t         *ml_upper;      /* read queue to send up to */
        int             ml_id;
};

struct mux_upper {
        int             mu_select;      /* 0: the newest link */
};

static struct mux_lower *mux_lowers;

static int mux_open(queue_t *, dev_t *, int, int, struct cred *);
static int mux_close(queue_t *, int, struct cred *);
static int mux_uwput(queue_t *, mblk_t *);
static int mux_lrput(queue_t *, mblk_t *);
static int mux_lwput(queue_t *, mblk_t *);

static struct module_info mux_minfo = {
        0x4d58, "strmux", 0, INFPSZ, 4096, 1024
};
static struct qinit mux_urinit = {
        NULL, NULL, mux_open, mux_close, NULL, &mux_minfo, NULL
};
static struct qinit mux_uwinit = {
        mux_uwput, NULL, NULL, NULL, NULL, &mux_minfo, NULL
};
static struct qinit mux_lrinit = {
        mux_lrput, NULL, NULL, NULL, NULL, &mux_minfo, NULL
};
static struct qinit mux_lwinit = {
        mux_lwput, NULL, NULL, NULL, NULL, &mux_minfo, NULL
};
struct streamtab strmuxinfo = {
        &mux_urinit, &mux_uwinit, &mux_lrinit, &mux_lwinit
};

static int
mux_open(queue_t *q, dev_t *devp __unused, int flag __unused, int sflag,
    struct cred *cr __unused)
{
        struct mux_upper *mu;

        if (sflag != CLONEOPEN)
                return ENXIO;
        mu = kmem_zalloc(sizeof(*mu), KM_SLEEP);
        q->q_ptr = WR(q)->q_ptr = mu;
        return 0;
}

static int
mux_close(queue_t *q, int flag __unused, struct cred *cr __unused)
{
        for (struct mux_lower *ml = mux_lowers; ml != NULL; ml = ml->ml_next)
                if (ml->ml_upper == q)
                        ml->ml_upper = NULL;
        kmem_free(q->q_ptr, sizeof(struct mux_upper));
        q->q_ptr = WR(q)->q_ptr = NULL;
        return 0;
}

static struct mux_lower *
mux_find(int id)
{
        struct mux_lower *ml = mux_lowers;

        if (id == 0)
                return ml;
        while (ml != NULL && ml->ml_id != id)
                ml = ml->ml_next;
        return ml;
}

static void
mux_ioctl(queue_t *q, mblk_t *mp)
{
        struct iocblk *ioc = (struct iocblk *)mp->b_rptr;
        struct mux_upper *mu = q->q_ptr;
        struct mux_lower *ml, **link;
        struct linkblk *lb;
        int n;

        if (ioc->ioc_count == TRANSPARENT) {
                miocnak(q, mp, 0, EINVAL);
                return;
        }
        switch (ioc->ioc_cmd) {
        case I_LINK:
        case I_PLINK:
                lb = (struct linkblk *)mp->b_cont->b_rptr;
                ml = kmem_zalloc(sizeof(*ml), KM_NOSLEEP);
                if (ml == NULL) {
                        miocnak(q, mp, 0, ENOMEM);
                        return;
                }
                ml->ml_wq = lb->l_qbot;
                ml->ml_id = lb->l_index;
                ml->ml_upper = RD(q);
                ml->ml_next = mux_lowers;
                mux_lowers = ml;
                miocack(q, mp, 0, 0);
                return;
        case I_UNLINK:
        case I_PUNLINK:
                lb = (struct linkblk *)mp->b_cont->b_rptr;
                for (link = &mux_lowers; (ml = *link) != NULL;
                    link = &ml->ml_next)
                        if (ml->ml_id == lb->l_index)
                                break;
                if (ml == NULL) {
                        miocnak(q, mp, 0, EINVAL);
                        return;
                }
                *link = ml->ml_next;
                kmem_free(ml, sizeof(*ml));
                miocack(q, mp, 0, 0);
                return;
        case MUX_SELECT:
                if (ioc->ioc_count != sizeof(int) || mp->b_cont == NULL) {
                        miocnak(q, mp, 0, EINVAL);
                        return;
                }
                memcpy(&n, mp->b_cont->b_rptr, sizeof(n));
                if (n != 0 && mux_find(n) == NULL) {
                        miocnak(q, mp, 0, ENXIO);
                        return;
                }
                mu->mu_select = n;
                miocack(q, mp, 0, 0);
                return;
        case MUX_COUNT:
                if (ioc->ioc_count != sizeof(int) || mp->b_cont == NULL) {
                        miocnak(q, mp, 0, EINVAL);
                        return;
                }
                n = 0;
                for (ml = mux_lowers; ml != NULL; ml = ml->ml_next)
                        n++;
                memcpy(mp->b_cont->b_rptr, &n, sizeof(n));
                miocack(q, mp, sizeof(n), 0);
                return;
        default:
                miocnak(q, mp, 0, EINVAL);
                return;
        }
}

static int
mux_uwput(queue_t *q, mblk_t *mp)
{
        struct mux_upper *mu = q->q_ptr;
        struct mux_lower *ml;

        switch (DB_TYPE(mp)) {
        case M_DATA:
        case M_PROTO:
        case M_PCPROTO:
                if ((ml = mux_find(mu->mu_select)) == NULL ||
                    !bcanputnext(ml->ml_wq, mp->b_band)) {
                        freemsg(mp);
                        return 0;
                }
                ml->ml_upper = RD(q);
                (void)putnext(ml->ml_wq, mp);
                return 0;
        case M_FLUSH:
                if ((*mp->b_rptr & FLUSHR) != 0) {
                        *mp->b_rptr &= ~FLUSHW;
                        qreply(q, mp);
                } else {
                        freemsg(mp);
                }
                return 0;
        case M_IOCTL:
                mux_ioctl(q, mp);
                return 0;
        default:
                freemsg(mp);
                return 0;
        }
}

/* Up from a lower stream. */
static int
mux_lrput(queue_t *q, mblk_t *mp)
{
        struct mux_lower *ml;

        switch (DB_TYPE(mp)) {
        case M_DATA:
        case M_PROTO:
        case M_PCPROTO:
                for (ml = mux_lowers; ml != NULL; ml = ml->ml_next)
                        if (ml->ml_wq == WR(q))
                                break;
                if (ml == NULL || ml->ml_upper == NULL ||
                    !bcanputnext(ml->ml_upper, mp->b_band))
                        freemsg(mp);
                else
                        (void)putnext(ml->ml_upper, mp);
                return 0;
        case M_FLUSH:
                /* A flush turned round below: send its write half back. */
                if ((*mp->b_rptr & FLUSHW) != 0) {
                        *mp->b_rptr &= ~FLUSHR;
                        qreply(q, mp);
                } else {
                        freemsg(mp);
                }
                return 0;
        default:
                freemsg(mp);
                return 0;
        }
}

static int
mux_lwput(queue_t *q, mblk_t *mp)
{
        (void)putnext(q, mp);
        return 0;
}
