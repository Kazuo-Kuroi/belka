/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * STREAMS messages: allocation, duplication, copying, and the routines
 * that reshape a message's chain of blocks.
 *
 * A message block (mblk_t) comes from a zone. Its data block (dblk_t)
 * and, for allocb, the buffer are one kmem allocation, freed when the
 * last block that shares them is. esballoc's buffer is the caller's,
 * handed back through its free routine. Everything here may be called
 * from any context, interrupts included, and takes no lock but kmem's.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/stream.h>
#include <sys/systm.h>

#include <machine/atomic.h>

#define DBLK_HDR        ((sizeof(dblk_t) + KMEM_ALIGN - 1) & \
                            ~(size_t)(KMEM_ALIGN - 1))

static struct kmem_zone *mblk_zone;

void
str_msginit(void)
{
        mblk_zone = kmem_zone_init(sizeof(mblk_t), "mblk");
}

static mblk_t *
mblk_new(dblk_t *db, int km)
{
        mblk_t *mp = kmem_zone_alloc(mblk_zone, km);

        if (mp == NULL)
                return NULL;
        mp->b_next = mp->b_prev = mp->b_cont = NULL;
        mp->b_rptr = mp->b_wptr = db->db_base;
        mp->b_datap = db;
        mp->b_band = 0;
        mp->b_flag = 0;
        mp->b_queue = NULL;
        return mp;
}

static mblk_t *
allocb_km(size_t size, int km)
{
        size_t len = DBLK_HDR + size;
        dblk_t *db;
        mblk_t *mp;

        if (len < size)
                return NULL;
        if ((db = kmem_alloc(len, km)) == NULL)
                return NULL;
        db->db_base = (unsigned char *)db + DBLK_HDR;
        db->db_lim = db->db_base + size;
        db->db_ref = 1;
        db->db_type = M_DATA;
        db->db_flags = 0;
        db->db_alloc = len;
        db->db_frtnp = NULL;
        if ((mp = mblk_new(db, km)) == NULL)
                kmem_free(db, len);
        return mp;
}

/* A message of one block with room for size bytes; NULL if none now. */
mblk_t *
allocb(size_t size, unsigned int pri __unused)
{
        return allocb_km(size, KM_NOSLEEP);
}

/* allocb for the stream head, which may wait for memory. */
mblk_t *
allocb_wait(size_t size)
{
        return allocb_km(size, KM_SLEEP);
}

/* Whether allocb(size) would succeed now; Belka cannot tell, so yes. */
int
testb(size_t size __unused, unsigned int pri __unused)
{
        return 1;
}

/* A block over the caller's buffer, which frtn gives back. */
mblk_t *
esballoc(unsigned char *base, size_t size, unsigned int pri __unused,
    frtn_t *frtn)
{
        dblk_t *db;
        mblk_t *mp;

        if (base == NULL || frtn == NULL)
                return NULL;
        if ((db = kmem_alloc(sizeof(*db), KM_NOSLEEP)) == NULL)
                return NULL;
        db->db_base = base;
        db->db_lim = base + size;
        db->db_ref = 1;
        db->db_type = M_DATA;
        db->db_flags = DBF_EXT;
        db->db_alloc = sizeof(*db);
        db->db_frtnp = frtn;
        if ((mp = mblk_new(db, KM_NOSLEEP)) == NULL)
                kmem_free(db, sizeof(*db));
        return mp;
}

void
freeb(mblk_t *mp)
{
        dblk_t *db = mp->b_datap;

        kmem_zone_free(mblk_zone, mp);
        if (atomic_dec_32_nv(&db->db_ref) != 0)
                return;
        if ((db->db_flags & DBF_EXT) != 0)
                db->db_frtnp->free_func(db->db_frtnp->free_arg);
        kmem_free(db, db->db_alloc);
}

void
freemsg(mblk_t *mp)
{
        while (mp != NULL) {
                mblk_t *next = mp->b_cont;

                freeb(mp);
                mp = next;
        }
}

/* Another block over the same data. */
mblk_t *
dupb(mblk_t *mp)
{
        mblk_t *nmp = kmem_zone_alloc(mblk_zone, KM_NOSLEEP);

        if (nmp == NULL)
                return NULL;
        atomic_inc_32_nv(&mp->b_datap->db_ref);
        *nmp = *mp;
        nmp->b_next = nmp->b_prev = nmp->b_cont = NULL;
        nmp->b_queue = NULL;
        return nmp;
}

mblk_t *
dupmsg(mblk_t *mp)
{
        mblk_t *head = NULL, **tail = &head;

        for (; mp != NULL; mp = mp->b_cont) {
                if ((*tail = dupb(mp)) == NULL) {
                        freemsg(head);
                        return NULL;
                }
                tail = &(*tail)->b_cont;
        }
        return head;
}

/* A new block with a copy of mp's data, at the same offset. */
mblk_t *
copyb(mblk_t *mp)
{
        size_t size = MBLKSIZE(mp), off = MBLKHEAD(mp);
        mblk_t *nmp;

        if ((nmp = allocb(size, BPRI_MED)) == NULL)
                return NULL;
        nmp->b_datap->db_type = mp->b_datap->db_type;
        nmp->b_band = mp->b_band;
        nmp->b_flag = mp->b_flag;
        nmp->b_rptr = nmp->b_datap->db_base + off;
        nmp->b_wptr = nmp->b_rptr + MBLKL(mp);
        if (MBLKL(mp) != 0)
                memcpy(nmp->b_rptr, mp->b_rptr, MBLKL(mp));
        return nmp;
}

mblk_t *
copymsg(mblk_t *mp)
{
        mblk_t *head = NULL, **tail = &head;

        for (; mp != NULL; mp = mp->b_cont) {
                if ((*tail = copyb(mp)) == NULL) {
                        freemsg(head);
                        return NULL;
                }
                tail = &(*tail)->b_cont;
        }
        return head;
}

/* Bytes in the message's M_DATA blocks. */
size_t
msgdsize(const mblk_t *mp)
{
        size_t n = 0;

        for (; mp != NULL; mp = mp->b_cont)
                if (mp->b_datap->db_type == M_DATA)
                        n += MBLKL(mp);
        return n;
}

/* Bytes in every block of the message (Belka's). */
size_t
msgsize(const mblk_t *mp)
{
        size_t n = 0;

        for (; mp != NULL; mp = mp->b_cont)
                n += MBLKL(mp);
        return n;
}

void
linkb(mblk_t *mp, mblk_t *bp)
{
        while (mp->b_cont != NULL)
                mp = mp->b_cont;
        mp->b_cont = bp;
}

/* Take the first block off the message: the rest, which may be NULL. */
mblk_t *
unlinkb(mblk_t *mp)
{
        mblk_t *rest = mp->b_cont;

        mp->b_cont = NULL;
        return rest;
}

/* Take bp out of the message mp: the message left, or (mblk_t *)-1. */
mblk_t *
rmvb(mblk_t *mp, mblk_t *bp)
{
        mblk_t **link = &mp;

        while (*link != NULL && *link != bp)
                link = &(*link)->b_cont;
        if (*link == NULL)
                return (mblk_t *)-1;
        *link = bp->b_cont;
        bp->b_cont = NULL;
        return mp;
}

/*
 * Join the first len bytes (all, for -1) of the leading blocks of mp's
 * type into a new first block, aligned as allocb aligns. The message
 * keeps its first mblk_t, so callers' pointers to it stay good.
 */
int
pullupmsg(mblk_t *mp, ssize_t len)
{
        unsigned char type = mp->b_datap->db_type;
        size_t want = 0, have;
        mblk_t *bp, *nmp;
        dblk_t *db;

        for (bp = mp; bp != NULL && bp->b_datap->db_type == type;
            bp = bp->b_cont)
                want += MBLKL(bp);
        if (len >= 0) {
                if ((size_t)len > want)
                        return 0;
                want = (size_t)len;
        }
        if (MBLKL(mp) >= want && ((uintptr_t)mp->b_rptr & 7) == 0 &&
            mp->b_datap->db_ref == 1)
                return 1;
        if ((nmp = allocb(want, BPRI_MED)) == NULL)
                return 0;
        for (bp = mp, have = 0; have < want; bp = bp->b_cont) {
                size_t n = MBLKL(bp);

                if (n > want - have)
                        n = want - have;
                memcpy(nmp->b_wptr, bp->b_rptr, n);
                nmp->b_wptr += n;
                bp->b_rptr += n;
                have += n;
        }
        /*
         * Swap the new data block into the first mblk_t, and the first
         * block's old data (what is left of it) into nmp, which follows.
         */
        db = mp->b_datap;
        nmp->b_datap->db_type = type;
        {
                unsigned char *r = mp->b_rptr, *w = mp->b_wptr;

                mp->b_datap = nmp->b_datap;
                mp->b_rptr = nmp->b_rptr;
                mp->b_wptr = nmp->b_wptr;
                nmp->b_datap = db;
                nmp->b_rptr = r;
                nmp->b_wptr = w;
        }
        nmp->b_cont = mp->b_cont;
        mp->b_cont = nmp;
        /* Drop emptied blocks behind the new first one. */
        while (mp->b_cont != NULL && MBLKL(mp->b_cont) == 0 &&
            mp->b_cont->b_datap->db_type == type) {
                bp = mp->b_cont;
                mp->b_cont = bp->b_cont;
                freeb(bp);
        }
        return 1;
}

/* A new message whose first len bytes are in one block; mp is kept. */
mblk_t *
msgpullup(mblk_t *mp, ssize_t len)
{
        mblk_t *nmp = copymsg(mp);

        if (nmp == NULL)
                return NULL;
        if (!pullupmsg(nmp, len)) {
                freemsg(nmp);
                return NULL;
        }
        return nmp;
}

/*
 * Trim len bytes from the head of the message (len > 0) or its tail
 * (len < 0), within the leading or trailing blocks of one type.
 */
int
adjmsg(mblk_t *mp, ssize_t len)
{
        unsigned char type;
        size_t n, total = 0;
        mblk_t *bp;

        if (mp == NULL)
                return 0;
        if (len >= 0) {
                type = mp->b_datap->db_type;
                for (bp = mp; bp != NULL && bp->b_datap->db_type == type;
                    bp = bp->b_cont)
                        total += MBLKL(bp);
                if ((size_t)len > total)
                        return 0;
                for (n = (size_t)len, bp = mp; n > 0; bp = bp->b_cont) {
                        size_t take = MBLKL(bp) < n ? MBLKL(bp) : n;

                        bp->b_rptr += take;
                        n -= take;
                }
                return 1;
        }
        /* From the tail: the trailing run of blocks of the last's type. */
        {
                mblk_t *last = mp, *run = mp;

                for (bp = mp; bp != NULL; bp = bp->b_cont) {
                        if (bp->b_datap->db_type != last->b_datap->db_type)
                                run = bp;
                        last = bp;
                }
                for (bp = run; bp != NULL; bp = bp->b_cont)
                        total += MBLKL(bp);
                n = (size_t)-len;
                if (n > total)
                        return 0;
                /* Keep total - n bytes from the start of the run. */
                total -= n;
                for (bp = run; bp != NULL; bp = bp->b_cont) {
                        if (MBLKL(bp) <= total) {
                                total -= MBLKL(bp);
                        } else {
                                bp->b_wptr = bp->b_rptr + total;
                                total = 0;
                        }
                }
        }
        return 1;
}
