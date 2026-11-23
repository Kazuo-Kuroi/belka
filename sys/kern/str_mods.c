/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * Two small modules, for testing plumbing: strpass passes everything
 * on unchanged; strup upper-cases the data going up, answers its own
 * ioctl (STRUP_COUNT: how many messages it has changed) and passes the
 * rest on. strup's instances share one syncq (SQLVL_MODULE), and its
 * read side is service-driven, so that both kinds of procedure and both
 * syncq levels are exercised.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/stream.h>
#include <sys/systm.h>

#define STRUP_COUNT     (('U' << 8) | 1)

static int pass_open(queue_t *, dev_t *, int, int, struct cred *);
static int pass_close(queue_t *, int, struct cred *);
static int pass_put(queue_t *, mblk_t *);

static struct module_info pass_minfo = {
        0x5053, "strpass", 0, INFPSZ, 4096, 1024
};
static struct qinit pass_rinit = {
        pass_put, NULL, pass_open, pass_close, NULL, &pass_minfo, NULL
};
static struct qinit pass_winit = {
        pass_put, NULL, NULL, NULL, NULL, &pass_minfo, NULL
};
struct streamtab strpassinfo = { &pass_rinit, &pass_winit, NULL, NULL };

static int
pass_open(queue_t *q __unused, dev_t *devp __unused, int flag __unused,
    int sflag, struct cred *cr __unused)
{
        return sflag == MODOPEN ? 0 : ENXIO;
}

static int
pass_close(queue_t *q __unused, int flag __unused, struct cred *cr __unused)
{
        return 0;
}

static int
pass_put(queue_t *q, mblk_t *mp)
{
        (void)putnext(q, mp);
        return 0;
}

/* --- strup. --- */

struct strup {
        unsigned int    su_count;       /* messages upper-cased */
};

static int up_open(queue_t *, dev_t *, int, int, struct cred *);
static int up_close(queue_t *, int, struct cred *);
static int up_rput(queue_t *, mblk_t *);
static int up_rsrv(queue_t *);
static int up_wput(queue_t *, mblk_t *);

static struct module_info up_minfo = {
        0x5550, "strup", 0, INFPSZ, 4096, 1024
};
static struct qinit up_rinit = {
        up_rput, up_rsrv, up_open, up_close, NULL, &up_minfo, NULL
};
static struct qinit up_winit = {
        up_wput, NULL, NULL, NULL, NULL, &up_minfo, NULL
};
struct streamtab strupinfo = { &up_rinit, &up_winit, NULL, NULL };

static int
up_open(queue_t *q, dev_t *devp __unused, int flag __unused, int sflag,
    struct cred *cr __unused)
{
        struct strup *su;

        if (sflag != MODOPEN)
                return ENXIO;
        if (q->q_ptr != NULL)
                return 0;
        su = kmem_zalloc(sizeof(*su), KM_SLEEP);
        q->q_ptr = WR(q)->q_ptr = su;
        return 0;
}

static int
up_close(queue_t *q, int flag __unused, struct cred *cr __unused)
{
        kmem_free(q->q_ptr, sizeof(struct strup));
        q->q_ptr = WR(q)->q_ptr = NULL;
        return 0;
}

static int
up_rput(queue_t *q, mblk_t *mp)
{
        if (DB_TYPE(mp) >= QPCTL) {
                if (DB_TYPE(mp) == M_FLUSH && (*mp->b_rptr & FLUSHR) != 0)
                        flushq(q, FLUSHDATA);
                (void)putnext(q, mp);
        } else {
                (void)putq(q, mp);
        }
        return 0;
}

static void
up_retry(void *arg)
{
        qenable(arg);
}

static int
up_rsrv(queue_t *q)
{
        struct strup *su = q->q_ptr;
        mblk_t *mp;

        while ((mp = getq(q)) != NULL) {
                if (!bcanputnext(q, mp->b_band)) {
                        (void)putbq(q, mp);
                        break;
                }
                if (DB_TYPE(mp) == M_DATA) {
                        if (mp->b_datap->db_ref > 1) {
                                mblk_t *copy = copymsg(mp);

                                if (copy == NULL) {
                                        (void)putbq(q, mp);
                                        (void)qbufcall(q, msgsize(mp),
                                            BPRI_MED, up_retry, q);
                                        break;
                                }
                                freemsg(mp);
                                mp = copy;
                        }
                        for (mblk_t *bp = mp; bp != NULL; bp = bp->b_cont)
                                for (unsigned char *p = bp->b_rptr;
                                    p < bp->b_wptr; p++)
                                        if (*p >= 'a' && *p <= 'z')
                                                *p -= 'a' - 'A';
                        su->su_count++;
                }
                (void)putnext(q, mp);
        }
        return 0;
}

static int
up_wput(queue_t *q, mblk_t *mp)
{
        struct strup *su = q->q_ptr;
        struct iocblk *ioc;

        switch (DB_TYPE(mp)) {
        case M_IOCTL:
                ioc = (struct iocblk *)mp->b_rptr;
                if (ioc->ioc_cmd != STRUP_COUNT)
                        break;
                if (ioc->ioc_count != sizeof(int) || mp->b_cont == NULL ||
                    MBLKL(mp->b_cont) < sizeof(int)) {
                        miocnak(q, mp, 0, EINVAL);
                        return 0;
                }
                memcpy(mp->b_cont->b_rptr, &su->su_count, sizeof(int));
                miocack(q, mp, sizeof(int), 0);
                return 0;
        case M_FLUSH:
                if ((*mp->b_rptr & FLUSHW) != 0)
                        flushq(q, FLUSHDATA);
                break;
        default:
                break;
        }
        (void)putnext(q, mp);
        return 0;
}
