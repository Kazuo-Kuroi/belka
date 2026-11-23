/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * STREAMS queues, flow control, syncqs, and the service threads.
 *
 * A queue's messages, counts and flags are under its q_lock, a spin
 * lock that may be taken from interrupts: putq, getq, qenable and the
 * rest may be called from anywhere. Messages are kept high priority
 * first, then by band, highest first, then in arrival order.
 *
 * Every queue pair belongs to a syncq, and a module's procedures run
 * only while their syncq is claimed. put() leaves the message on the
 * syncq and claims it if it is free, then runs what is waiting there
 * before letting go: so a put to a busy syncq costs the caller nothing,
 * and calls that loop back to the caller's own syncq are run after it
 * returns, not inside it. Enabled queues and timeout callbacks are left
 * on the syncq the same way, but are not run in the caller's context
 * (which may hold locks a service procedure would take): if the syncq
 * is free they go to the service threads, one per processor.
 *
 * Only plumbing (sq_enter) waits to claim a syncq; procedures never do,
 * and no thread sleeps while it holds one except during a module's open
 * or close. Lock order: sd_qlock, q_lock, sq_lock, str_runlock;
 * str_cblock before sq_lock.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/smp.h>
#include <sys/spinlock.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/intr.h>

/* A qtimeout or qbufcall: armed, then posted to its syncq, then run. */
struct strcb {
        struct strcb    *cb_next;       /* on the syncq */
        struct strcb    *cb_allnext;    /* every live callback */
        queue_t         *cb_q;
        void            (*cb_fn)(void *);
        void            *cb_arg;
        int64_t         cb_id;
        toid_t          cb_toid;
        int             cb_posted;
};

static struct spinlock str_runlock = SPINLOCK_INITIALIZER("strrun");
static struct syncq *str_runhead, *str_runtail;
static struct sv str_runsv;

static struct spinlock str_cblock = SPINLOCK_INITIALIZER("strcb");
static struct strcb *str_callbacks;
static int64_t str_nextcb = 1;

static void sq_drain(struct syncq *);

/* --- Syncqs. --- */

struct syncq *
sq_alloc(void)
{
        struct syncq *sq = kmem_zalloc(sizeof(*sq), KM_SLEEP);

        spinlock_init(&sq->sq_lock, "syncq");
        sv_init(&sq->sq_sv, "syncq");
        sq->sq_refs = 1;
        return sq;
}

void
sq_hold(struct syncq *sq)
{
        atomic_inc_32_nv(&sq->sq_refs);
}

void
sq_rele(struct syncq *sq)
{
        if (atomic_dec_32_nv(&sq->sq_refs) == 0)
                kmem_free(sq, sizeof(*sq));
}

/* sq_lock held: hand sq to the service threads, if it is not theirs. */
static void
sq_schedule(struct syncq *sq)
{
        int s;

        if ((sq->sq_flags & (SQ_BUSY | SQ_QUEUED)) != 0)
                return;
        sq->sq_flags |= SQ_QUEUED;
        sq_hold(sq);                    /* the run list's */
        s = spin_lock(&str_runlock);
        sq->sq_runnext = NULL;
        if (str_runtail != NULL)
                str_runtail->sq_runnext = sq;
        else
                str_runhead = sq;
        str_runtail = sq;
        sv_signal(&str_runsv);
        spin_unlock(&str_runlock, s);
}

/* Claim sq, waiting for it if need be. For plumbing and the head. */
void
sq_enter(struct syncq *sq)
{
        int s = spin_lock(&sq->sq_lock);

        while ((sq->sq_flags & SQ_BUSY) != 0) {
                if (sq->sq_owner == curthread)
                        panic("sq_enter: syncq %p already claimed", (void *)sq);
                sq->sq_flags |= SQ_WANTED;
                sv_wait(&sq->sq_sv, &sq->sq_lock, s);
                s = spin_lock(&sq->sq_lock);
        }
        sq->sq_flags |= SQ_BUSY;
        sq->sq_owner = curthread;
        spin_unlock(&sq->sq_lock, s);
}

/* Run what waits on sq, then let it go. */
void
sq_exit(struct syncq *sq)
{
        sq_drain(sq);
}

/* Claim several syncqs, in address order, each once. */
void
sq_claim_set(struct syncq **set, int n)
{
        for (int i = 1; i < n; i++)
                for (int j = i; j > 0 && (uintptr_t)set[j - 1] >
                    (uintptr_t)set[j]; j--) {
                        struct syncq *t = set[j];

                        set[j] = set[j - 1];
                        set[j - 1] = t;
                }
        for (int i = 0; i < n; i++)
                if (set[i] != NULL && (i == 0 || set[i] != set[i - 1]))
                        sq_enter(set[i]);
}

void
sq_exit_set(struct syncq **set, int n)
{
        for (int i = n - 1; i >= 0; i--)
                if (set[i] != NULL && (i == 0 || set[i] != set[i - 1]))
                        sq_exit(set[i]);
}

static void
run_put(mblk_t *mp)
{
        queue_t *q = mp->b_queue;

        mp->b_queue = NULL;
        if ((q->q_flag & QOFF) != 0 || q->q_qinfo->qi_putp == NULL)
                freemsg(mp);
        else
                (void)q->q_qinfo->qi_putp(q, mp);
}

static void
run_service(queue_t *q)
{
        int s = spin_lock(&q->q_lock), off;

        q->q_flag &= ~QENAB;
        off = (q->q_flag & QOFF) != 0;
        spin_unlock(&q->q_lock, s);
        if (!off && q->q_qinfo->qi_srvp != NULL)
                (void)q->q_qinfo->qi_srvp(q);
}

static void
cb_unlist(struct strcb *cb)
{
        int s = spin_lock(&str_cblock);
        struct strcb **link = &str_callbacks;

        while (*link != NULL && *link != cb)
                link = &(*link)->cb_allnext;
        if (*link == cb)
                *link = cb->cb_allnext;
        spin_unlock(&str_cblock, s);
}

static void
run_callback(struct strcb *cb)
{
        cb_unlist(cb);
        if ((cb->cb_q->q_flag & QOFF) == 0)
                cb->cb_fn(cb->cb_arg);
        kmem_free(cb, sizeof(*cb));
}

/* The caller holds sq. Run its work until there is none, and let go. */
static void
sq_drain(struct syncq *sq)
{
        for (;;) {
                int s = spin_lock(&sq->sq_lock);
                mblk_t *mp;
                struct strcb *cb;
                queue_t *q;

                if ((mp = sq->sq_puthead) != NULL) {
                        if ((sq->sq_puthead = mp->b_next) == NULL)
                                sq->sq_puttail = NULL;
                        mp->b_next = NULL;
                        spin_unlock(&sq->sq_lock, s);
                        run_put(mp);
                } else if ((cb = sq->sq_cbhead) != NULL) {
                        if ((sq->sq_cbhead = cb->cb_next) == NULL)
                                sq->sq_cbtail = NULL;
                        spin_unlock(&sq->sq_lock, s);
                        run_callback(cb);
                } else if ((q = sq->sq_svhead) != NULL) {
                        if ((sq->sq_svhead = q->q_svnext) == NULL)
                                sq->sq_svtail = NULL;
                        q->q_svnext = NULL;
                        spin_unlock(&sq->sq_lock, s);
                        run_service(q);
                } else {
                        sq->sq_flags &= ~SQ_BUSY;
                        sq->sq_owner = NULL;
                        if ((sq->sq_flags & SQ_WANTED) != 0) {
                                sq->sq_flags &= ~SQ_WANTED;
                                sv_broadcast(&sq->sq_sv);
                        }
                        spin_unlock(&sq->sq_lock, s);
                        return;
                }
        }
}

/*
 * The caller holds sq: drop the work waiting on it for q's pair, which
 * is leaving the stream. Callbacks still armed are cancelled.
 */
void
sq_purge(struct syncq *sq, queue_t *q)
{
        queue_t *rq = RD(q), *wq = WR(q);
        mblk_t *mp, **mlink, *dead = NULL;
        struct strcb *cb, **clink, *cdead = NULL;
        queue_t **qlink;
        int s;

        /* Callbacks still armed: cancel them; untimeout waits for one firing. */
        for (;;) {
                s = spin_lock(&str_cblock);
                for (cb = str_callbacks; cb != NULL; cb = cb->cb_allnext)
                        if ((cb->cb_q == rq || cb->cb_q == wq) &&
                            !cb->cb_posted)
                                break;
                spin_unlock(&str_cblock, s);
                if (cb == NULL)
                        break;
                if (untimeout(cb->cb_toid)) {
                        cb_unlist(cb);
                        kmem_free(cb, sizeof(*cb));
                }
                /* Otherwise it fired, and is now posted: dropped below. */
        }

        s = spin_lock(&sq->sq_lock);
        mlink = &sq->sq_puthead;
        sq->sq_puttail = NULL;
        while ((mp = *mlink) != NULL) {
                if (mp->b_queue == rq || mp->b_queue == wq) {
                        *mlink = mp->b_next;
                        mp->b_next = dead;
                        dead = mp;
                } else {
                        sq->sq_puttail = mp;
                        mlink = &mp->b_next;
                }
        }
        clink = &sq->sq_cbhead;
        sq->sq_cbtail = NULL;
        while ((cb = *clink) != NULL) {
                if (cb->cb_q == rq || cb->cb_q == wq) {
                        *clink = cb->cb_next;
                        cb->cb_next = cdead;
                        cdead = cb;
                } else {
                        sq->sq_cbtail = cb;
                        clink = &cb->cb_next;
                }
        }
        qlink = &sq->sq_svhead;
        sq->sq_svtail = NULL;
        while (*qlink != NULL) {
                if (*qlink == rq || *qlink == wq) {
                        queue_t *gone = *qlink;

                        *qlink = gone->q_svnext;
                        gone->q_svnext = NULL;
                } else {
                        sq->sq_svtail = *qlink;
                        qlink = &(*qlink)->q_svnext;
                }
        }
        spin_unlock(&sq->sq_lock, s);
        rq->q_flag &= ~QENAB;
        wq->q_flag &= ~QENAB;

        while ((mp = dead) != NULL) {
                dead = mp->b_next;
                mp->b_next = NULL;
                mp->b_queue = NULL;
                freemsg(mp);
        }
        while ((cb = cdead) != NULL) {
                cdead = cb->cb_next;
                cb_unlist(cb);
                kmem_free(cb, sizeof(*cb));
        }
}

/* --- The service threads. --- */

static void
str_service(void *arg __unused)
{
        for (;;) {
                struct syncq *sq;
                int s = spin_lock(&str_runlock);

                while (str_runhead == NULL) {
                        sv_wait(&str_runsv, &str_runlock, s);
                        s = spin_lock(&str_runlock);
                }
                sq = str_runhead;
                if ((str_runhead = sq->sq_runnext) == NULL)
                        str_runtail = NULL;
                spin_unlock(&str_runlock, s);

                s = spin_lock(&sq->sq_lock);
                sq->sq_flags &= ~SQ_QUEUED;
                if ((sq->sq_flags & SQ_BUSY) != 0) {
                        spin_unlock(&sq->sq_lock, s);  /* its holder runs it */
                } else {
                        sq->sq_flags |= SQ_BUSY;
                        sq->sq_owner = curthread;
                        spin_unlock(&sq->sq_lock, s);
                        sq_drain(sq);
                }
                sq_rele(sq);
        }
}

void
str_init(void)
{
        char name[KT_NAMELEN];

        str_msginit();
        strhead_init();
        sv_init(&str_runsv, "strrun");
        for (unsigned int i = 0; i < ncpus; i++) {
                snprintf(name, sizeof(name), "strsvc%u", i);
                (void)kthread_create_bound(str_service, NULL, PRI_KERNEL + 10,
                    name, (int)i);
        }
}

/* --- Queues. --- */

queue_t *
allocq(void)
{
        queue_t *q = kmem_zalloc(2 * sizeof(*q), KM_SLEEP);

        spinlock_init(&q[0].q_lock, "rq");
        spinlock_init(&q[1].q_lock, "wq");
        q[0].q_flag = QREADR | QWANTR;
        q[1].q_flag = QWANTR;
        return q;
}

static void
setq1(queue_t *q, struct qinit *qi)
{
        struct module_info *mi = qi->qi_minfo;

        q->q_qinfo = qi;
        q->q_minpsz = mi->mi_minpsz;
        q->q_maxpsz = mi->mi_maxpsz;
        q->q_hiwat = mi->mi_hiwat;
        q->q_lowat = mi->mi_lowat;
}

/* Fill in a new pair, which takes a reference on sq. */
void
setq(queue_t *rq, struct qinit *rinit, struct qinit *winit, struct syncq *sq,
    struct stdata *sd)
{
        queue_t *wq = WR(rq);

        setq1(rq, rinit);
        setq1(wq, winit);
        rq->q_syncq = wq->q_syncq = sq;
        rq->q_stream = wq->q_stream = sd;
        sq_hold(sq);
}

static void
freebands(queue_t *q)
{
        qband_t *qb;

        while ((qb = q->q_bandp) != NULL) {
                q->q_bandp = qb->qb_next;
                kmem_free(qb, sizeof(*qb));
        }
        q->q_nband = 0;
}

/* Free a pair that has left its stream; its syncq is the caller's. */
void
freeq(queue_t *rq)
{
        queue_t *wq = WR(rq);

        flushq(rq, FLUSHALL);
        flushq(wq, FLUSHALL);
        freebands(rq);
        freebands(wq);
        if (rq->q_syncq != NULL)
                sq_rele(rq->q_syncq);
        kmem_free(rq, 2 * sizeof(*rq));
}

/* q_lock held. Band pri's qband, made if need be (NULL: no memory). */
static qband_t *
qband_get(queue_t *q, unsigned char pri, int make)
{
        qband_t *qb = NULL, **link = &q->q_bandp;
        unsigned int n = 1;

        for (; *link != NULL; link = &(*link)->qb_next, n++)
                if (n == pri)
                        return *link;
        if (!make)
                return NULL;
        for (; n <= pri; n++) {
                if ((qb = kmem_zalloc(sizeof(*qb), KM_NOSLEEP)) == NULL)
                        return NULL;
                qb->qb_hiwat = q->q_hiwat;
                qb->qb_lowat = q->q_lowat;
                *link = qb;
                link = &qb->qb_next;
                q->q_nband = (unsigned char)n;
        }
        return qb;
}

static int
ishipri(const mblk_t *mp)
{
        return mp->b_datap->db_type >= QPCTL;
}

/* q_lock held: account for mp coming (+1) or going (-1). */
static int
qcount(queue_t *q, mblk_t *mp, int dir)
{
        size_t size = msgsize(mp);
        qband_t *qb;

        if (ishipri(mp) || mp->b_band == 0) {
                if (dir > 0) {
                        q->q_count += size;
                        if (q->q_count >= q->q_hiwat)
                                q->q_flag |= QFULL;
                        return 0;
                }
                q->q_count -= size;
                if (q->q_count < q->q_hiwat)
                        q->q_flag &= ~QFULL;
                if ((q->q_flag & QWANTW) != 0 &&
                    (q->q_count <= q->q_lowat || q->q_count == 0)) {
                        q->q_flag &= ~QWANTW;
                        return 1;               /* back-enable */
                }
                return 0;
        }
        qb = qband_get(q, mp->b_band, 0);
        if (qb == NULL)
                return 0;
        if (dir > 0) {
                qb->qb_count += size;
                if (qb->qb_count >= qb->qb_hiwat)
                        qb->qb_flag |= QB_FULL;
                return 0;
        }
        qb->qb_count -= size;
        if (qb->qb_count < qb->qb_hiwat)
                qb->qb_flag &= ~QB_FULL;
        if ((qb->qb_flag & QB_WANTW) != 0 &&
            (qb->qb_count <= qb->qb_lowat || qb->qb_count == 0)) {
                qb->qb_flag &= ~QB_WANTW;
                return 1;
        }
        return 0;
}

/* q_lock held: fix the band's first and last after a change around mp. */
static void
qband_fix(queue_t *q, unsigned char band)
{
        qband_t *qb;
        mblk_t *mp;

        if (band == 0 || (qb = qband_get(q, band, 0)) == NULL)
                return;
        qb->qb_first = qb->qb_last = NULL;
        for (mp = q->q_first; mp != NULL; mp = mp->b_next) {
                if (ishipri(mp) || mp->b_band != band)
                        continue;
                if (qb->qb_first == NULL)
                        qb->qb_first = mp;
                qb->qb_last = mp;
        }
}

/* q_lock held: link mp in before before (NULL: at the tail). */
static void
qlink(queue_t *q, mblk_t *mp, mblk_t *before)
{
        mp->b_next = before;
        if (before == NULL) {
                mp->b_prev = q->q_last;
                if (q->q_last != NULL)
                        q->q_last->b_next = mp;
                else
                        q->q_first = mp;
                q->q_last = mp;
        } else {
                mp->b_prev = before->b_prev;
                if (before->b_prev != NULL)
                        before->b_prev->b_next = mp;
                else
                        q->q_first = mp;
                before->b_prev = mp;
        }
}

static void
qunlink(queue_t *q, mblk_t *mp)
{
        if (mp->b_prev != NULL)
                mp->b_prev->b_next = mp->b_next;
        else
                q->q_first = mp->b_next;
        if (mp->b_next != NULL)
                mp->b_next->b_prev = mp->b_prev;
        else
                q->q_last = mp->b_prev;
        mp->b_next = mp->b_prev = NULL;
}

/* Where mp goes: before the first message it outranks (NULL: the tail). */
static mblk_t *
qplace(queue_t *q, const mblk_t *mp, int back)
{
        mblk_t *at;

        for (at = q->q_first; at != NULL; at = at->b_next) {
                if (ishipri(mp)) {
                        if (!ishipri(at) || back)
                                break;
                } else if (!ishipri(at) && (at->b_band < mp->b_band ||
                    (back && at->b_band == mp->b_band))) {
                        break;
                }
        }
        return at;
}

static int
putq_common(queue_t *q, mblk_t *mp, int back)
{
        int s, enable = 0;

        if (ishipri(mp))
                mp->b_band = 0;
        s = spin_lock(&q->q_lock);
        if (mp->b_band != 0 && qband_get(q, mp->b_band, 1) == NULL) {
                spin_unlock(&q->q_lock, s);
                return 0;
        }
        qlink(q, mp, qplace(q, mp, back));
        (void)qcount(q, mp, 1);
        qband_fix(q, mp->b_band);
        if (!back && (ishipri(mp) || ((q->q_flag & QNOENB) == 0 &&
            ((q->q_flag & QWANTR) != 0 || mp->b_band != 0))))
                enable = 1;
        if (!back)
                q->q_flag &= ~QWANTR;
        spin_unlock(&q->q_lock, s);
        if (enable)
                qenable(q);
        return 1;
}

int
putq(queue_t *q, mblk_t *mp)
{
        return putq_common(q, mp, 0);
}

int
putbq(queue_t *q, mblk_t *mp)
{
        return putq_common(q, mp, 1);
}

mblk_t *
getq(queue_t *q)
{
        int s = spin_lock(&q->q_lock), back;
        mblk_t *mp = q->q_first;

        if (mp == NULL) {
                q->q_flag |= QWANTR;
                spin_unlock(&q->q_lock, s);
                return NULL;
        }
        qunlink(q, mp);
        back = qcount(q, mp, -1);
        qband_fix(q, mp->b_band);
        q->q_flag &= ~QWANTR;
        spin_unlock(&q->q_lock, s);
        if (back)
                str_backenable(q);
        return mp;
}

/* Take mp, which is on q, off it. */
void
rmvq(queue_t *q, mblk_t *mp)
{
        int s = spin_lock(&q->q_lock), back;

        qunlink(q, mp);
        back = qcount(q, mp, -1);
        qband_fix(q, mp->b_band);
        spin_unlock(&q->q_lock, s);
        if (back)
                str_backenable(q);
}

/* Put mp before emp (NULL: at the end); 0 if that breaks the order. */
int
insq(queue_t *q, mblk_t *emp, mblk_t *mp)
{
        int s, ok = 1;

        if (ishipri(mp))
                mp->b_band = 0;
        s = spin_lock(&q->q_lock);
        if (emp != NULL) {
                mblk_t *prev = emp->b_prev;

                if (ishipri(emp) && !ishipri(mp))
                        ok = 0;
                else if (!ishipri(mp) && !ishipri(emp) &&
                    emp->b_band > mp->b_band)
                        ok = 0;
                if (prev != NULL && !ishipri(prev) && (ishipri(mp) ||
                    prev->b_band < mp->b_band))
                        ok = 0;
        } else if (q->q_last != NULL && ((ishipri(mp) &&
            !ishipri(q->q_last)) || (!ishipri(mp) && !ishipri(q->q_last) &&
            q->q_last->b_band < mp->b_band))) {
                ok = 0;
        }
        if (ok && mp->b_band != 0 && qband_get(q, mp->b_band, 1) == NULL)
                ok = 0;
        if (ok) {
                qlink(q, mp, emp);
                (void)qcount(q, mp, 1);
                qband_fix(q, mp->b_band);
        }
        spin_unlock(&q->q_lock, s);
        return ok;
}

static void
flush_common(queue_t *q, int flag, int band)
{
        mblk_t *mp, *next, *dead = NULL;
        int s = spin_lock(&q->q_lock), back = 0;

        for (mp = q->q_first; mp != NULL; mp = next) {
                next = mp->b_next;
                if (band >= 0 && (ishipri(mp) || mp->b_band != band))
                        continue;
                if (flag != FLUSHALL && !datamsg(mp->b_datap->db_type))
                        continue;
                qunlink(q, mp);
                back |= qcount(q, mp, -1);
                mp->b_next = dead;
                dead = mp;
        }
        for (qband_t *qb = q->q_bandp; qb != NULL; qb = qb->qb_next)
                qb->qb_first = qb->qb_last = NULL;
        for (unsigned int b = 1; b <= q->q_nband; b++)
                qband_fix(q, (unsigned char)b);
        if (q->q_first == NULL && (q->q_flag & QWANTW) != 0) {
                q->q_flag &= ~QWANTW;
                back = 1;
        }
        spin_unlock(&q->q_lock, s);
        while ((mp = dead) != NULL) {
                dead = mp->b_next;
                mp->b_next = NULL;
                freemsg(mp);
        }
        if (back)
                str_backenable(q);
}

void
flushq(queue_t *q, int flag)
{
        flush_common(q, flag, -1);
}

void
flushband(queue_t *q, unsigned char pri, int flag)
{
        flush_common(q, flag, pri);
}

int
qsize(queue_t *q)
{
        int s = spin_lock(&q->q_lock), n = 0;

        for (mblk_t *mp = q->q_first; mp != NULL; mp = mp->b_next)
                n++;
        spin_unlock(&q->q_lock, s);
        return n;
}

/* Schedule q's service procedure, unless it is already. */
void
qenable(queue_t *q)
{
        struct syncq *sq = q->q_syncq;
        int s;

        if (q->q_qinfo->qi_srvp == NULL)
                return;
        s = spin_lock(&q->q_lock);
        if ((q->q_flag & (QENAB | QOFF)) != 0) {
                spin_unlock(&q->q_lock, s);
                return;
        }
        q->q_flag |= QENAB;
        spin_unlock(&q->q_lock, s);

        s = spin_lock(&sq->sq_lock);
        q->q_svnext = NULL;
        if (sq->sq_svtail != NULL)
                sq->sq_svtail->q_svnext = q;
        else
                sq->sq_svhead = q;
        sq->sq_svtail = q;
        sq_schedule(sq);
        spin_unlock(&sq->sq_lock, s);
}

void
noenable(queue_t *q)
{
        int s = spin_lock(&q->q_lock);

        q->q_flag |= QNOENB;
        spin_unlock(&q->q_lock, s);
}

void
enableok(queue_t *q)
{
        int s = spin_lock(&q->q_lock);

        q->q_flag &= ~QNOENB;
        spin_unlock(&q->q_lock, s);
}

static int
stream_lock(queue_t *q)
{
        return q->q_stream != NULL ? spin_lock(&q->q_stream->sd_qlock) : 0;
}

static void
stream_unlock(queue_t *q, int s)
{
        if (q->q_stream != NULL)
                spin_unlock(&q->q_stream->sd_qlock, s);
}

/*
 * Whether band pri may be put to q: the answer of the first queue from
 * q on that has a service procedure (or the last). If not, it is marked
 * so that it back-enables when it drains.
 */
int
bcanput(queue_t *q, unsigned char pri)
{
        queue_t *start = q;
        int s = stream_lock(start), ok = 1, qs;

        while (q->q_qinfo->qi_srvp == NULL && q->q_next != NULL)
                q = q->q_next;
        qs = spin_lock(&q->q_lock);
        if (pri == 0) {
                if ((q->q_flag & QFULL) != 0) {
                        q->q_flag |= QWANTW;
                        ok = 0;
                }
        } else {
                qband_t *qb = qband_get(q, pri, 0);

                if (qb != NULL && (qb->qb_flag & QB_FULL) != 0) {
                        qb->qb_flag |= QB_WANTW;
                        ok = 0;
                }
        }
        spin_unlock(&q->q_lock, qs);
        stream_unlock(start, s);
        return ok;
}

int
canput(queue_t *q)
{
        return bcanput(q, 0);
}

int
canputnext(queue_t *q)
{
        return bcanput(q->q_next, 0);
}

int
bcanputnext(queue_t *q, unsigned char pri)
{
        return bcanput(q->q_next, pri);
}

/*
 * q has drained: enable the nearest queue behind it with a service
 * procedure, which may now send more.
 */
void
str_backenable(queue_t *q)
{
        int s = stream_lock(q);
        queue_t *b = q->q_back;

        while (b != NULL && b->q_qinfo->qi_srvp == NULL)
                b = b->q_back;
        if (b != NULL) {
                int qs = spin_lock(&b->q_lock);

                b->q_flag |= QBACK;
                spin_unlock(&b->q_lock, qs);
                qenable(b);
        }
        stream_unlock(q, s);
}

/* --- Passing messages. --- */

int
put(queue_t *q, mblk_t *mp)
{
        struct syncq *sq = q->q_syncq;
        int s;

        mp->b_queue = q;
        mp->b_next = NULL;
        s = spin_lock(&sq->sq_lock);
        if (sq->sq_puttail != NULL)
                sq->sq_puttail->b_next = mp;
        else
                sq->sq_puthead = mp;
        sq->sq_puttail = mp;
        if ((sq->sq_flags & SQ_BUSY) != 0) {
                spin_unlock(&sq->sq_lock, s);
                return 0;
        }
        sq->sq_flags |= SQ_BUSY;
        sq->sq_owner = curthread;
        spin_unlock(&sq->sq_lock, s);
        sq_drain(sq);
        return 0;
}

int
putnext(queue_t *q, mblk_t *mp)
{
        if (q->q_next == NULL) {
                freemsg(mp);
                return 0;
        }
        return put(q->q_next, mp);
}

void
qreply(queue_t *q, mblk_t *mp)
{
        (void)putnext(OTHERQ(q), mp);
}

static mblk_t *
ctlmsg(int type, int withparam, int param)
{
        mblk_t *mp;

        if (datamsg(type) && type != M_DELAY)
                return NULL;
        if ((mp = allocb(withparam ? 1 : 0, BPRI_HI)) == NULL)
                return NULL;
        mp->b_datap->db_type = (unsigned char)type;
        if (withparam)
                *mp->b_wptr++ = (unsigned char)param;
        return mp;
}

int
putctl(queue_t *q, int type)
{
        mblk_t *mp = ctlmsg(type, 0, 0);

        if (mp == NULL)
                return 0;
        (void)put(q, mp);
        return 1;
}

int
putctl1(queue_t *q, int type, int param)
{
        mblk_t *mp = ctlmsg(type, 1, param);

        if (mp == NULL)
                return 0;
        (void)put(q, mp);
        return 1;
}

int
putnextctl(queue_t *q, int type)
{
        return q->q_next != NULL ? putctl(q->q_next, type) : 0;
}

int
putnextctl1(queue_t *q, int type, int param)
{
        return q->q_next != NULL ? putctl1(q->q_next, type, param) : 0;
}

/* Reply to an M_IOCTL: done, with count bytes in b_cont, and rval. */
void
miocack(queue_t *q, mblk_t *mp, size_t count, int rval)
{
        struct iocblk *ioc = (struct iocblk *)mp->b_rptr;

        mp->b_datap->db_type = M_IOCACK;
        ioc->ioc_count = count;
        ioc->ioc_error = 0;
        ioc->ioc_rval = rval;
        qreply(q, mp);
}

void
miocnak(queue_t *q, mblk_t *mp, size_t count, int error)
{
        struct iocblk *ioc = (struct iocblk *)mp->b_rptr;

        mp->b_datap->db_type = M_IOCNAK;
        ioc->ioc_count = count;
        ioc->ioc_error = error;
        ioc->ioc_rval = -1;
        qreply(q, mp);
}

/*
 * A driver's answer to M_FLUSH on its write queue q: flush what it
 * says, and send the read half back up (or free the message).
 */
void
flushq_reply(queue_t *q, mblk_t *mp)
{
        unsigned char how = *mp->b_rptr;
        int band = (how & FLUSHBAND) != 0 && MBLKL(mp) > 1 ?
            mp->b_rptr[1] : -1;

        if ((how & FLUSHW) != 0) {
                if (band >= 0)
                        flushband(q, (unsigned char)band, FLUSHDATA);
                else
                        flushq(q, FLUSHDATA);
                *mp->b_rptr &= ~FLUSHW;
        }
        if ((*mp->b_rptr & FLUSHR) != 0) {
                if (band >= 0)
                        flushband(RD(q), (unsigned char)band, FLUSHDATA);
                else
                        flushq(RD(q), FLUSHDATA);
                qreply(q, mp);
        } else {
                freemsg(mp);
        }
}

int
strqget(queue_t *q, qfields_t what, unsigned char pri, intptr_t *valp)
{
        int s = spin_lock(&q->q_lock), error = 0;
        qband_t *qb = pri != 0 ? qband_get(q, pri, 0) : NULL;

        if (pri != 0 && qb == NULL && what != QFLAG) {
                *valp = 0;
                spin_unlock(&q->q_lock, s);
                return 0;
        }
        switch (what) {
        case QHIWAT:
                *valp = (intptr_t)(qb != NULL ? qb->qb_hiwat : q->q_hiwat);
                break;
        case QLOWAT:
                *valp = (intptr_t)(qb != NULL ? qb->qb_lowat : q->q_lowat);
                break;
        case QMAXPSZ:
                *valp = (intptr_t)q->q_maxpsz;
                break;
        case QMINPSZ:
                *valp = (intptr_t)q->q_minpsz;
                break;
        case QCOUNT:
                *valp = (intptr_t)(qb != NULL ? qb->qb_count : q->q_count);
                break;
        case QFIRST:
                *valp = (intptr_t)(qb != NULL ? qb->qb_first : q->q_first);
                break;
        case QLAST:
                *valp = (intptr_t)(qb != NULL ? qb->qb_last : q->q_last);
                break;
        case QFLAG:
                *valp = (intptr_t)(qb != NULL ? qb->qb_flag : q->q_flag);
                break;
        default:
                error = EINVAL;
                break;
        }
        spin_unlock(&q->q_lock, s);
        return error;
}

int
strqset(queue_t *q, qfields_t what, unsigned char pri, intptr_t val)
{
        int s = spin_lock(&q->q_lock), error = 0;
        qband_t *qb = NULL;

        if (pri != 0 && (qb = qband_get(q, pri, 1)) == NULL) {
                spin_unlock(&q->q_lock, s);
                return EAGAIN;
        }
        switch (what) {
        case QHIWAT:
                if (qb != NULL)
                        qb->qb_hiwat = (size_t)val;
                else
                        q->q_hiwat = (size_t)val;
                break;
        case QLOWAT:
                if (qb != NULL)
                        qb->qb_lowat = (size_t)val;
                else
                        q->q_lowat = (size_t)val;
                break;
        case QMAXPSZ:
                if (qb != NULL)
                        error = EINVAL;
                else
                        q->q_maxpsz = (ssize_t)val;
                break;
        case QMINPSZ:
                if (qb != NULL)
                        error = EINVAL;
                else
                        q->q_minpsz = (ssize_t)val;
                break;
        default:
                error = EPERM;
                break;
        }
        spin_unlock(&q->q_lock, s);
        return error;
}

/* Let the pair's procedures run, or not (puts to it are then dropped). */
void
qprocson(queue_t *q)
{
        int s;

        s = spin_lock(&RD(q)->q_lock);
        RD(q)->q_flag &= ~QOFF;
        spin_unlock(&RD(q)->q_lock, s);
        s = spin_lock(&WR(q)->q_lock);
        WR(q)->q_flag &= ~QOFF;
        spin_unlock(&WR(q)->q_lock, s);
}

void
qprocsoff(queue_t *q)
{
        int s;

        s = spin_lock(&RD(q)->q_lock);
        RD(q)->q_flag |= QOFF;
        spin_unlock(&RD(q)->q_lock, s);
        s = spin_lock(&WR(q)->q_lock);
        WR(q)->q_flag |= QOFF;
        spin_unlock(&WR(q)->q_lock, s);
}

/* --- Callbacks: they run in their queue's syncq, in thread context. --- */

static void
cb_fire(void *arg)
{
        struct strcb *cb = arg;
        struct syncq *sq = cb->cb_q->q_syncq;
        int s = spin_lock(&str_cblock), ss;

        cb->cb_posted = 1;
        ss = spin_lock(&sq->sq_lock);
        spin_unlock(&str_cblock, 0);
        cb->cb_next = NULL;
        if (sq->sq_cbtail != NULL)
                sq->sq_cbtail->cb_next = cb;
        else
                sq->sq_cbhead = cb;
        sq->sq_cbtail = cb;
        sq_schedule(sq);
        spin_unlock(&sq->sq_lock, ss);
        splx(s);
}

int64_t
qtimeout(queue_t *q, void (*fn)(void *), void *arg, long ticks)
{
        struct strcb *cb = kmem_zalloc(sizeof(*cb), KM_NOSLEEP);
        int s;

        if (cb == NULL)
                return 0;
        cb->cb_q = q;
        cb->cb_fn = fn;
        cb->cb_arg = arg;
        s = spin_lock(&str_cblock);
        cb->cb_id = str_nextcb++;
        cb->cb_allnext = str_callbacks;
        str_callbacks = cb;
        /* Armed under the lock, so cb_fire cannot see cb_toid unset. */
        cb->cb_toid = timeout(cb_fire, cb, ticks);
        spin_unlock(&str_cblock, s);
        return cb->cb_id;
}

/* From q's own procedures, inside its syncq. */
void
quntimeout(queue_t *q, int64_t id)
{
        struct strcb *cb, **link;
        struct syncq *sq = q->q_syncq;
        int s = spin_lock(&str_cblock);

        for (cb = str_callbacks; cb != NULL; cb = cb->cb_allnext)
                if (cb->cb_id == id)
                        break;
        spin_unlock(&str_cblock, s);
        if (cb == NULL)
                return;                 /* it has run */
        if (untimeout(cb->cb_toid)) {
                cb_unlist(cb);
                kmem_free(cb, sizeof(*cb));
                return;
        }
        /* It fired and is waiting on the syncq, which the caller holds. */
        s = spin_lock(&sq->sq_lock);
        sq->sq_cbtail = NULL;
        for (link = &sq->sq_cbhead; *link != NULL; ) {
                if (*link == cb) {
                        *link = cb->cb_next;
                        continue;
                }
                sq->sq_cbtail = *link;
                link = &(*link)->cb_next;
        }
        spin_unlock(&sq->sq_lock, s);
        cb_unlist(cb);
        kmem_free(cb, sizeof(*cb));
}

/* Memory is never waited for (KM_SLEEP cannot fail): retry next tick. */
int64_t
qbufcall(queue_t *q, size_t size __unused, unsigned int pri __unused,
    void (*fn)(void *), void *arg)
{
        return qtimeout(q, fn, arg, 1);
}

void
qunbufcall(queue_t *q, int64_t id)
{
        quntimeout(q, id);
}
