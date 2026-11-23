/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * Self-tests for the STREAMS framework, below the stream head: message
 * routines, queue order and flow control, back-enabling, syncqs (a put
 * to a claimed syncq waits for its holder; procedures never nest), the
 * service threads, and qtimeout. The head is tested from user mode by
 * libctest, through /dev/strloop.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/param.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/systm.h>

int selftest_str(void);

static int failures;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("selftest: %s:%d: %s\n",                 \
                            __FILE__, __LINE__, #expr);                 \
                        failures++;                                     \
                }                                                       \
        } while (0)

static mblk_t *
msg(const char *s, unsigned char type, unsigned char band)
{
        size_t n = strlen(s);
        mblk_t *mp = allocb(n, BPRI_MED);

        if (mp == NULL)
                return NULL;
        memcpy(mp->b_wptr, s, n);
        mp->b_wptr += n;
        mp->b_datap->db_type = type;
        mp->b_band = band;
        return mp;
}

static int freed;

static void
ext_free(void *arg)
{
        *(int *)arg += 1;
}

static void
test_messages(void)
{
        static unsigned char ext[32];
        frtn_t fr = { ext_free, &freed };
        mblk_t *a, *b, *c, *d;

        a = msg("hello", M_PROTO, 0);
        b = msg(", ", M_DATA, 0);
        c = msg("world", M_DATA, 0);
        CHECK(a != NULL && b != NULL && c != NULL);
        if (a == NULL || b == NULL || c == NULL)
                return;
        linkb(a, b);
        linkb(a, c);
        CHECK(msgdsize(a) == 7);
        CHECK(msgsize(a) == 12);

        d = dupmsg(a);
        CHECK(d != NULL && d->b_datap == a->b_datap && a->b_datap->db_ref == 2);
        freemsg(d);
        CHECK(a->b_datap->db_ref == 1);

        d = copymsg(a);
        CHECK(d != NULL && d->b_datap != a->b_datap && msgsize(d) == 12 &&
            DB_TYPE(d) == M_PROTO && memcmp(d->b_cont->b_rptr, ", ", 2) == 0);
        freemsg(d);

        /* The data part: join it; the first block is kept. */
        c = unlinkb(a);
        CHECK(c == b && b->b_cont != NULL && msgdsize(b) == 7);
        freeb(a);
        a = b;
        CHECK(pullupmsg(a, -1) == 1 && MBLKL(a) == 7 && a->b_cont == NULL &&
            memcmp(a->b_rptr, ", world", 7) == 0);
        CHECK(pullupmsg(a, 8) == 0);
        CHECK(adjmsg(a, 2) == 1 && MBLKL(a) == 5);
        CHECK(adjmsg(a, -2) == 1 && MBLKL(a) == 3 &&
            memcmp(a->b_rptr, "wor", 3) == 0);
        CHECK(adjmsg(a, 4) == 0 && adjmsg(a, -4) == 0);
        b = msg("!", M_DATA, 0);
        linkb(a, b);
        CHECK(rmvb(a, b) == a && a->b_cont == NULL);
        CHECK(rmvb(a, b) == (mblk_t *)-1);
        freeb(b);
        freemsg(a);

        freed = 0;
        a = esballoc(ext, sizeof(ext), BPRI_MED, &fr);
        CHECK(a != NULL && a->b_rptr == ext && MBLKSIZE(a) == sizeof(ext));
        if (a != NULL) {
                b = dupb(a);
                freeb(a);
                CHECK(freed == 0);
                freeb(b);
                CHECK(freed == 1);
        }
}

/* --- Queues. --- */

static int srv_runs;

static int
count_srv(queue_t *q __unused)
{
        srv_runs++;
        return 0;
}

static struct module_info t_minfo = { 1, "stest", 0, INFPSZ, 100, 20 };
static struct qinit t_qinit = { NULL, count_srv, NULL, NULL, NULL, &t_minfo,
    NULL };

/* The message types, in queue order, as letters; for the checks. */
static void
qorder(queue_t *q, char *buf, size_t len)
{
        size_t i = 0;

        for (mblk_t *mp = q->q_first; mp != NULL && i + 1 < len;
            mp = mp->b_next)
                buf[i++] = (char)*mp->b_rptr;
        buf[i] = 0;
}

static void
test_queue_order(void)
{
        struct syncq *sq = sq_alloc();
        queue_t *rq = allocq(), *q = WR(rq);
        char order[16];
        mblk_t *mp;

        setq(rq, &t_qinit, &t_qinit, sq, NULL);
        noenable(q);
        CHECK(putq(q, msg("a", M_DATA, 0)) == 1);
        CHECK(putq(q, msg("b", M_DATA, 2)) == 1);
        CHECK(putq(q, msg("c", M_DATA, 1)) == 1);
        CHECK(putq(q, msg("d", M_PCPROTO, 0)) == 1);
        CHECK(putq(q, msg("e", M_DATA, 2)) == 1);
        CHECK(putq(q, msg("f", M_PCPROTO, 0)) == 1);
        qorder(q, order, sizeof(order));
        CHECK(strcmp(order, "dfbeca") == 0);
        CHECK(q->q_nband == 2 && qsize(q) == 6);

        mp = getq(q);
        CHECK(mp != NULL && *mp->b_rptr == 'd');
        CHECK(putbq(q, mp) == 1);
        mp = getq(q);
        freemsg(mp);
        mp = getq(q);                   /* f */
        freemsg(mp);
        mp = getq(q);                   /* b, then back ahead of e */
        CHECK(mp != NULL && *mp->b_rptr == 'b');
        CHECK(putbq(q, mp) == 1);
        qorder(q, order, sizeof(order));
        CHECK(strcmp(order, "beca") == 0);

        /* insq keeps the order or refuses. */
        mp = msg("g", M_DATA, 1);
        CHECK(insq(q, q->q_first, mp) == 0);
        CHECK(insq(q, q->q_last, mp) == 1);
        qorder(q, order, sizeof(order));
        CHECK(strcmp(order, "becga") == 0);
        rmvq(q, mp);
        freemsg(mp);

        flushband(q, 2, FLUSHALL);
        qorder(q, order, sizeof(order));
        CHECK(strcmp(order, "ca") == 0);
        CHECK(putq(q, msg("h", M_IOCTL, 0)) == 1);
        flushq(q, FLUSHDATA);
        qorder(q, order, sizeof(order));
        CHECK(strcmp(order, "h") == 0);
        flushq(q, FLUSHALL);
        CHECK(q->q_first == NULL && q->q_count == 0 && getq(q) == NULL);

        sq_enter(sq);
        sq_purge(sq, rq);
        sq_exit(sq);
        freeq(rq);
        sq_rele(sq);
}

/* Wait for the service threads, at most a second. */
static void
await(volatile int *p, int want)
{
        for (int i = 0; i < HZ && *p < want; i++)
                delay(1);
}

static void
test_flow(void)
{
        struct syncq *sq = sq_alloc();
        queue_t *up = allocq(), *down = allocq();
        queue_t *uw = WR(up), *dw = WR(down);
        mblk_t *mp;

        setq(up, &t_qinit, &t_qinit, sq, NULL);
        setq(down, &t_qinit, &t_qinit, sq, NULL);
        uw->q_next = dw;
        dw->q_back = uw;

        srv_runs = 0;
        CHECK(canputnext(uw));
        /* The first put enables the queue; the service threads run it. */
        CHECK(putq(dw, msg("0123456789", M_DATA, 0)) == 1);
        await(&srv_runs, 1);
        CHECK(srv_runs == 1);
        for (int i = 0; i < 9; i++)
                (void)putq(dw, msg("0123456789", M_DATA, 0));
        CHECK((dw->q_flag & QFULL) != 0 && dw->q_count == 100);
        CHECK(!canputnext(uw) && (dw->q_flag & QWANTW) != 0);
        /* Band flow control is separate. */
        CHECK(bcanputnext(uw, 1));

        /* Draining to the low-water mark back-enables the writer. */
        srv_runs = 0;
        for (int i = 0; i < 7; i++)
                freemsg(getq(dw));
        CHECK(srv_runs == 0 && (dw->q_flag & QFULL) == 0);
        mp = getq(dw);                  /* 20 left: at the low-water mark */
        freemsg(mp);
        await(&srv_runs, 1);
        CHECK(srv_runs == 1 && (uw->q_flag & QBACK) != 0);
        CHECK(canputnext(uw));

        flushq(dw, FLUSHALL);
        sq_enter(sq);
        sq_purge(sq, up);
        sq_purge(sq, down);
        sq_exit(sq);
        freeq(up);
        freeq(down);
        sq_rele(sq);
}

/* --- Syncqs. --- */

static char trace[32];
static int ntrace, depth, maxdepth;
static queue_t *partner;

static int
trace_put(queue_t *q, mblk_t *mp)
{
        if (++depth > maxdepth)
                maxdepth = depth;
        if (ntrace < (int)sizeof(trace) - 1)
                trace[ntrace++] = (char)*mp->b_rptr;
        /* Each of 'a' and 'b' sends one more message to the pair. */
        if (*mp->b_rptr == 'a')
                (void)put(OTHERQ(q), msg("c", M_DATA, 0));
        if (*mp->b_rptr == 'b' && partner != NULL)
                (void)put(partner, msg("d", M_DATA, 0));
        freemsg(mp);
        depth--;
        return 0;
}

static struct qinit trace_qinit = { trace_put, NULL, NULL, NULL, NULL,
    &t_minfo, NULL };

static volatile int cb_runs;
static struct syncq *cb_sq;

static void
cb_fn(void *arg)
{
        /* Inside the syncq: it is claimed, by this thread. */
        if (cb_sq->sq_owner == curthread && (cb_sq->sq_flags & SQ_BUSY) != 0)
                cb_runs += (int)(intptr_t)arg;
        else
                cb_runs = -100;
}

static void
test_syncq(void)
{
        struct syncq *sq = sq_alloc(), *sq2 = sq_alloc();
        queue_t *p = allocq(), *p2 = allocq();
        int64_t id;

        setq(p, &trace_qinit, &trace_qinit, sq, NULL);
        setq(p2, &trace_qinit, &trace_qinit, sq2, NULL);

        /* A put into one's own syncq runs after, not inside. */
        ntrace = depth = maxdepth = 0;
        partner = NULL;
        (void)put(p, msg("a", M_DATA, 0));
        trace[ntrace] = 0;
        CHECK(strcmp(trace, "ac") == 0 && maxdepth == 1);

        /* A put to a claimed syncq waits for its holder to let go. */
        ntrace = 0;
        sq_enter(sq);
        (void)put(p, msg("x", M_DATA, 0));
        (void)put(WR(p), msg("y", M_DATA, 0));
        CHECK(ntrace == 0);
        sq_exit(sq);
        trace[ntrace] = 0;
        CHECK(strcmp(trace, "xy") == 0);

        /* Into another, free, syncq it runs at once, nested. */
        ntrace = depth = maxdepth = 0;
        partner = p2;
        (void)put(p, msg("b", M_DATA, 0));
        trace[ntrace] = 0;
        CHECK(strcmp(trace, "bd") == 0 && maxdepth == 2);
        partner = NULL;

        /* qtimeout's function runs in the queue's syncq. */
        cb_sq = sq;
        cb_runs = 0;
        (void)qtimeout(p, cb_fn, (void *)1, 1);
        await(&cb_runs, 1);
        CHECK(cb_runs == 1);
        cb_runs = 0;
        id = qtimeout(p, cb_fn, (void *)10, HZ);
        sq_enter(sq);
        quntimeout(p, id);
        sq_exit(sq);
        delay(HZ + 2);
        CHECK(cb_runs == 0);
        /* One that fires while the syncq is held waits, then is dropped. */
        sq_enter(sq);
        (void)qtimeout(p, cb_fn, (void *)100, 1);
        delay(3);
        CHECK(cb_runs == 0 && sq->sq_cbhead != NULL);
        sq_purge(sq, p);
        CHECK(sq->sq_cbhead == NULL);
        sq_exit(sq);
        CHECK(cb_runs == 0);

        sq_enter(sq2);
        sq_purge(sq2, p2);
        sq_exit(sq2);
        freeq(p);
        freeq(p2);
        sq_rele(sq);
        sq_rele(sq2);
}

int
selftest_str(void)
{
        failures = 0;
        test_messages();
        test_queue_order();
        test_flow();
        test_syncq();
        return failures;
}
