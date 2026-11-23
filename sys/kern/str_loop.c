/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * strloop, the loopback STREAMS driver (/dev/strloop): each open is a
 * stream of its own, and what is sent down it comes back up, data and
 * protocol messages alike, with flow control: what the head has not
 * yet read stops the writer. Its ioctls (LOOP_*) let the tests make it
 * send the head what a real driver would: hang-ups, errors and options.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/errno.h>
#include <sys/stream.h>
#include <sys/systm.h>

#define LOOP_ECHO       (('L' << 8) | 1)        /* ack with the data sent */
#define LOOP_HANGUP     (('L' << 8) | 2)        /* send M_HANGUP up */
#define LOOP_ERROR      (('L' << 8) | 3)        /* M_ERROR with the data */
#define LOOP_SETOPTS    (('L' << 8) | 4)        /* M_SETOPTS: struct stroptions */
#define LOOP_SIG        (('L' << 8) | 5)        /* M_PCSIG with the data */
#define LOOP_SILENT     (('L' << 8) | 6)        /* no answer: I_STR times out */
#define LOOP_MARK       (('L' << 8) | 7)        /* mark the next message */

static int loop_open(queue_t *, dev_t *, int, int, struct cred *);
static int loop_close(queue_t *, int, struct cred *);
static int loop_wput(queue_t *, mblk_t *);
static int loop_wsrv(queue_t *);
static int loop_rsrv(queue_t *);

static struct module_info loop_minfo = {
        0x4c50, "strloop", 0, INFPSZ, 4096, 1024
};
static struct qinit loop_rinit = {
        NULL, loop_rsrv, loop_open, loop_close, NULL, &loop_minfo, NULL
};
static struct qinit loop_winit = {
        loop_wput, loop_wsrv, NULL, NULL, NULL, &loop_minfo, NULL
};
struct streamtab strloopinfo = { &loop_rinit, &loop_winit, NULL, NULL };

static int
loop_open(queue_t *q, dev_t *devp __unused, int flag __unused, int sflag,
    struct cred *cr __unused)
{
        if (sflag != CLONEOPEN)
                return ENXIO;
        q->q_ptr = WR(q)->q_ptr = NULL;
        return 0;
}

static int
loop_close(queue_t *q __unused, int flag __unused, struct cred *cr __unused)
{
        return 0;
}

/* Up to the head: a reply, or what the ioctl asks to be sent. */
static mblk_t *
loop_ctl(int type, mblk_t *data)
{
        mblk_t *mp = data != NULL ? data : allocb(0, BPRI_HI);

        if (mp != NULL)
                mp->b_datap->db_type = (unsigned char)type;
        return mp;
}

static void
loop_ioctl(queue_t *q, mblk_t *mp)
{
        struct iocblk *ioc = (struct iocblk *)mp->b_rptr;
        mblk_t *data = mp->b_cont, *up = NULL;

        if (ioc->ioc_count == TRANSPARENT) {
                miocnak(q, mp, 0, EINVAL);
                return;
        }
        switch (ioc->ioc_cmd) {
        case LOOP_ECHO:
                miocack(q, mp, ioc->ioc_count, (int)ioc->ioc_count);
                return;
        case LOOP_SILENT:
                freemsg(mp);
                return;
        case LOOP_MARK:
                q->q_ptr = (void *)1;
                miocack(q, mp, 0, 0);
                return;
        case LOOP_HANGUP:
                up = loop_ctl(M_HANGUP, NULL);
                break;
        case LOOP_ERROR:
        case LOOP_SETOPTS:
        case LOOP_SIG:
                if (data == NULL || (data = msgpullup(data, -1)) == NULL) {
                        miocnak(q, mp, 0, EINVAL);
                        return;
                }
                up = loop_ctl(ioc->ioc_cmd == LOOP_ERROR ? M_ERROR :
                    ioc->ioc_cmd == LOOP_SETOPTS ? M_SETOPTS : M_PCSIG, data);
                break;
        default:
                miocnak(q, mp, 0, EINVAL);
                return;
        }
        if (up == NULL) {
                miocnak(q, mp, 0, ENOSR);
                return;
        }
        /* Answer first: an M_ERROR would fail the ioctl waiting for it. */
        if (mp->b_cont != NULL) {
                freemsg(mp->b_cont);
                mp->b_cont = NULL;
        }
        miocack(q, mp, 0, 0);
        (void)putnext(RD(q), up);
}

static int
loop_wput(queue_t *q, mblk_t *mp)
{
        switch (DB_TYPE(mp)) {
        case M_DATA:
        case M_PROTO:
        case M_PCPROTO:
                if (q->q_ptr != NULL) {
                        mp->b_flag |= MSGMARK;
                        q->q_ptr = NULL;
                }
                if (DB_TYPE(mp) >= QPCTL)
                        (void)putnext(RD(q), mp);
                else if (q->q_first == NULL &&
                    bcanputnext(RD(q), mp->b_band))
                        (void)putnext(RD(q), mp);
                else
                        (void)putq(q, mp);
                return 0;
        case M_FLUSH:
                /* What was written is what would be read: flush both. */
                if ((*mp->b_rptr & FLUSHW) != 0) {
                        flushq(q, FLUSHDATA);
                        *mp->b_rptr |= FLUSHR;
                }
                flushq_reply(q, mp);
                return 0;
        case M_IOCTL:
                loop_ioctl(q, mp);
                return 0;
        default:
                freemsg(mp);
                return 0;
        }
}

/*
 * Send up what waits, while the head has room. The first message is
 * looked at before it is taken, so the queue never seems to empty (and
 * let a writer by) while the head is full.
 */
static int
loop_wsrv(queue_t *q)
{
        mblk_t *mp;

        while ((mp = q->q_first) != NULL) {
                if (!bcanputnext(RD(q), mp->b_band))
                        break;
                (void)putnext(RD(q), getq(q));
        }
        return 0;
}

/* The head has drained (a back-enable up the read side): write more. */
static int
loop_rsrv(queue_t *q)
{
        qenable(WR(q));
        return 0;
}
