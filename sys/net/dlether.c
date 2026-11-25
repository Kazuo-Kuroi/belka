/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * The Ethernet DLPI provider (see <sys/dlether.h>): DLPI version 2,
 * style 1, connectionless, for every Ethernet driver (virtio-net's,
 * Intel's). It was vionet's own, from 2026-11-22 (doc/design/decisions.md,
 * "virtio-net and DLPI"), until a second driver needed it.
 *
 * Each open of /dev/<name> is a stream of its own, bound to an Ethernet
 * type with DL_BIND_REQ, or taking every type with DL_PROMISCON_REQ.
 * dlether_input() copies a frame into a DL_UNITDATA_IND for every
 * stream that wants it; DL_UNITDATA_REQ builds a frame and gives it to
 * the driver's de_output.
 *
 * Locks: de_lock (a mutex) covers the list of streams and their state;
 * dlether_input holds it while it puts frames to the streams (a put
 * never waits), and the driver's de_promisc is called with it held.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/dlether.h>
#include <sys/dlpi.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/mkdev.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>

#define DE_DLADDRLEN    (ETHER_ADDRLEN + 2)     /* address, then type */

/* One open stream. */
struct dlether_stream {
        struct dlether_stream *ds_next;
        struct dlether  *ds_de;
        queue_t         *ds_rq;
        int             ds_state;       /* DL_UNBOUND, DL_IDLE */
        uint16_t        ds_sap;
        int             ds_promisc_phys;
        int             ds_promisc_sap;
        int             ds_promisc_multi;
};

static const uint8_t de_broadcast[ETHER_ADDRLEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static int de_open(queue_t *, dev_t *, int, int, struct cred *);
static int de_close(queue_t *, int, struct cred *);
static int de_rput(queue_t *, mblk_t *);
static int de_wput(queue_t *, mblk_t *);

static struct module_info de_minfo = {
        0x4445, "dlether", 0, INFPSZ, 65536, 4096
};
static struct qinit de_rinit = {
        de_rput, NULL, de_open, de_close, NULL, &de_minfo, NULL
};
static struct qinit de_winit = {
        de_wput, NULL, NULL, NULL, NULL, &de_minfo, NULL
};
static struct streamtab dlether_info = { &de_rinit, &de_winit, NULL, NULL };

int
dlether_register(struct dlether *de, unsigned int major, unsigned int unit)
{
        mutex_init(&de->de_lock, "dlether");
        if (strdev_register(de->de_name, &dlether_info, SQLVL_QPAIR, major,
            unit, 0600, de) == NULL) {
                printf("%s: no room for its device\n", de->de_name);
                return -1;
        }
        return 0;
}

/* sap and station address, in DLPI's form (address, then type). */
static void
de_dladdr(uint8_t *to, const uint8_t *station, uint16_t sap)
{
        memcpy(to, station, ETHER_ADDRLEN);
        memcpy(to + ETHER_ADDRLEN, &sap, sizeof(sap));
}

/* A DL_UNITDATA_IND for one stream: the frame's addresses, then data. */
static mblk_t *
de_unitdata_ind(const uint8_t *frame, uint16_t type, const uint8_t *data,
    size_t len)
{
        dl_unitdata_ind_t ind;
        mblk_t *mp, *dp;

        if ((mp = allocb(sizeof(ind) + 2 * DE_DLADDRLEN, BPRI_MED)) == NULL)
                return NULL;
        if ((dp = allocb(len, BPRI_MED)) == NULL) {
                freeb(mp);
                return NULL;
        }
        memset(&ind, 0, sizeof(ind));
        ind.dl_primitive = DL_UNITDATA_IND;
        ind.dl_dest_addr_length = DE_DLADDRLEN;
        ind.dl_dest_addr_offset = sizeof(ind);
        ind.dl_src_addr_length = DE_DLADDRLEN;
        ind.dl_src_addr_offset = sizeof(ind) + DE_DLADDRLEN;
        ind.dl_group_address = (frame[0] & 1) != 0;
        mp->b_datap->db_type = M_PROTO;
        memcpy(mp->b_wptr, &ind, sizeof(ind));
        de_dladdr(mp->b_wptr + sizeof(ind), frame, type);
        de_dladdr(mp->b_wptr + sizeof(ind) + DE_DLADDRLEN,
            frame + ETHER_ADDRLEN, type);
        mp->b_wptr += sizeof(ind) + 2 * DE_DLADDRLEN;
        memcpy(dp->b_wptr, data, len);
        dp->b_wptr += len;
        mp->b_cont = dp;
        return mp;
}

void
dlether_input(struct dlether *de, const uint8_t *frame, size_t len)
{
        uint16_t type;
        int group, forus;

        if (len < ETHER_HDRLEN)
                return;
        type = (uint16_t)(frame[12] << 8 | frame[13]);
        if (type < 0x0600)
                return;                 /* 802.3 length: not yet */
        group = (frame[0] & 1) != 0;
        forus = group || memcmp(frame, de->de_addr, ETHER_ADDRLEN) == 0;
        atomic_inc_64_nv(&de->de_ipackets);

        mutex_lock(&de->de_lock);
        for (struct dlether_stream *ds = de->de_streams; ds != NULL;
            ds = ds->ds_next) {
                mblk_t *mp;

                if (!forus && !ds->ds_promisc_phys)
                        continue;
                if (!ds->ds_promisc_sap &&
                    (ds->ds_state != DL_IDLE || ds->ds_sap != type))
                        continue;
                mp = de_unitdata_ind(frame, type, frame + ETHER_HDRLEN,
                    len - ETHER_HDRLEN);
                if (mp == NULL) {
                        atomic_inc_64_nv(&de->de_drops);
                        continue;
                }
                (void)put(ds->ds_rq, mp);
        }
        mutex_unlock(&de->de_lock);
}

/* --- The streams. --- */

static int
de_open(queue_t *q, dev_t *devp, int flag __unused, int sflag,
    struct cred *cr __unused)
{
        struct dlether *de = strdev_priv(*devp);
        struct dlether_stream *ds;

        if (sflag != CLONEOPEN || de == NULL)
                return ENXIO;
        ds = kmem_zalloc(sizeof(*ds), KM_SLEEP);
        ds->ds_de = de;
        ds->ds_rq = q;
        ds->ds_state = DL_UNBOUND;
        q->q_ptr = WR(q)->q_ptr = ds;
        mutex_lock(&de->de_lock);
        ds->ds_next = de->de_streams;
        de->de_streams = ds;
        mutex_unlock(&de->de_lock);
        return 0;
}

static int
de_close(queue_t *q, int flag __unused, struct cred *cr __unused)
{
        struct dlether_stream *ds = q->q_ptr, **link;
        struct dlether *de = ds->ds_de;
        int last = 0;

        mutex_lock(&de->de_lock);
        for (link = &de->de_streams; *link != NULL; link = &(*link)->ds_next)
                if (*link == ds) {
                        *link = ds->ds_next;
                        break;
                }
        if (ds->ds_promisc_phys)
                last = --de->de_nphys == 0;
        if (last && de->de_promisc != NULL)
                de->de_promisc(de, 0);
        mutex_unlock(&de->de_lock);
        q->q_ptr = WR(q)->q_ptr = NULL;
        kmem_free(ds, sizeof(*ds));
        return 0;
}

/* Up to the head, unless it is full: then the frame is dropped. */
static int
de_rput(queue_t *q, mblk_t *mp)
{
        struct dlether_stream *ds = q->q_ptr;

        if (DB_TYPE(mp) >= QPCTL || canputnext(q)) {
                (void)putnext(q, mp);
        } else {
                atomic_inc_64_nv(&ds->ds_de->de_drops);
                freemsg(mp);
        }
        return 0;
}

/* A primitive's reply, of type and size, its first word prim. */
static mblk_t *
de_reply(unsigned char type, size_t size, t_uscalar_t prim)
{
        mblk_t *mp = allocb(size, BPRI_HI);

        if (mp == NULL)
                return NULL;
        mp->b_datap->db_type = type;
        memset(mp->b_wptr, 0, size);
        memcpy(mp->b_wptr, &prim, sizeof(prim));
        mp->b_wptr += size;
        return mp;
}

static void
de_error_ack(queue_t *q, mblk_t *mp, t_uscalar_t prim, t_uscalar_t err,
    int uerr)
{
        dl_error_ack_t ack = { DL_ERROR_ACK, prim, err, (t_uscalar_t)uerr };
        mblk_t *rp = de_reply(M_PCPROTO, sizeof(ack), DL_ERROR_ACK);

        freemsg(mp);
        if (rp == NULL)
                return;
        memcpy(rp->b_rptr, &ack, sizeof(ack));
        qreply(q, rp);
}

static void
de_ok_ack(queue_t *q, mblk_t *mp, t_uscalar_t prim)
{
        dl_ok_ack_t ack = { DL_OK_ACK, prim };
        mblk_t *rp = de_reply(M_PCPROTO, sizeof(ack), DL_OK_ACK);

        freemsg(mp);
        if (rp == NULL)
                return;
        memcpy(rp->b_rptr, &ack, sizeof(ack));
        qreply(q, rp);
}

static void
de_uderror(queue_t *q, mblk_t *mp, t_uscalar_t err)
{
        dl_uderror_ind_t ind;
        mblk_t *rp = de_reply(M_PROTO, sizeof(ind), DL_UDERROR_IND);

        freemsg(mp);
        if (rp == NULL)
                return;
        memset(&ind, 0, sizeof(ind));
        ind.dl_primitive = DL_UDERROR_IND;
        ind.dl_errno = err;
        memcpy(rp->b_rptr, &ind, sizeof(ind));
        qreply(q, rp);
}

static void
de_info(queue_t *q, mblk_t *mp)
{
        struct dlether_stream *ds = q->q_ptr;
        dl_info_ack_t ack;
        size_t size = sizeof(ack) + DE_DLADDRLEN + ETHER_ADDRLEN;
        mblk_t *rp = de_reply(M_PCPROTO, size, DL_INFO_ACK);

        freemsg(mp);
        if (rp == NULL)
                return;
        memset(&ack, 0, sizeof(ack));
        ack.dl_primitive = DL_INFO_ACK;
        ack.dl_max_sdu = ETHER_MTU;
        ack.dl_min_sdu = 1;
        ack.dl_addr_length = DE_DLADDRLEN;
        ack.dl_mac_type = DL_ETHER;
        ack.dl_current_state = (t_uscalar_t)ds->ds_state;
        ack.dl_sap_length = -2;
        ack.dl_service_mode = DL_CLDLS;
        ack.dl_provider_style = DL_STYLE1;
        ack.dl_addr_offset = sizeof(ack);
        ack.dl_version = DL_VERSION_2;
        ack.dl_brdcst_addr_length = ETHER_ADDRLEN;
        ack.dl_brdcst_addr_offset = sizeof(ack) + DE_DLADDRLEN;
        memcpy(rp->b_rptr, &ack, sizeof(ack));
        de_dladdr(rp->b_rptr + sizeof(ack), ds->ds_de->de_addr, ds->ds_sap);
        memcpy(rp->b_rptr + sizeof(ack) + DE_DLADDRLEN, de_broadcast,
            ETHER_ADDRLEN);
        qreply(q, rp);
}

static void
de_bind(queue_t *q, mblk_t *mp)
{
        struct dlether_stream *ds = q->q_ptr;
        struct dlether *de = ds->ds_de;
        dl_bind_req_t req;
        dl_bind_ack_t ack;
        mblk_t *rp;

        memcpy(&req, mp->b_rptr, sizeof(req));
        if (ds->ds_state != DL_UNBOUND) {
                de_error_ack(q, mp, DL_BIND_REQ, DL_OUTSTATE, 0);
                return;
        }
        if (req.dl_service_mode != DL_CLDLS) {
                de_error_ack(q, mp, DL_BIND_REQ, DL_UNSUPPORTED, 0);
                return;
        }
        if (req.dl_sap < 0x0600 || req.dl_sap > 0xffff) {
                de_error_ack(q, mp, DL_BIND_REQ, DL_BADSAP, 0);
                return;
        }
        rp = de_reply(M_PCPROTO, sizeof(ack) + DE_DLADDRLEN, DL_BIND_ACK);
        if (rp == NULL) {
                de_error_ack(q, mp, DL_BIND_REQ, DL_SYSERR, ENOSR);
                return;
        }
        mutex_lock(&de->de_lock);
        ds->ds_sap = (uint16_t)req.dl_sap;
        ds->ds_state = DL_IDLE;
        mutex_unlock(&de->de_lock);
        memset(&ack, 0, sizeof(ack));
        ack.dl_primitive = DL_BIND_ACK;
        ack.dl_sap = req.dl_sap;
        ack.dl_addr_length = DE_DLADDRLEN;
        ack.dl_addr_offset = sizeof(ack);
        memcpy(rp->b_rptr, &ack, sizeof(ack));
        de_dladdr(rp->b_rptr + sizeof(ack), de->de_addr, ds->ds_sap);
        freemsg(mp);
        qreply(q, rp);
}

static void
de_unbind(queue_t *q, mblk_t *mp)
{
        struct dlether_stream *ds = q->q_ptr;

        if (ds->ds_state != DL_IDLE) {
                de_error_ack(q, mp, DL_UNBIND_REQ, DL_OUTSTATE, 0);
                return;
        }
        mutex_lock(&ds->ds_de->de_lock);
        ds->ds_state = DL_UNBOUND;
        ds->ds_sap = 0;
        mutex_unlock(&ds->ds_de->de_lock);
        de_ok_ack(q, mp, DL_UNBIND_REQ);
}

static void
de_promisc(queue_t *q, mblk_t *mp, t_uscalar_t prim)
{
        struct dlether_stream *ds = q->q_ptr;
        struct dlether *de = ds->ds_de;
        dl_promiscon_req_t req;
        int on = prim == DL_PROMISCON_REQ, change = 0, *flag;

        memcpy(&req, mp->b_rptr, sizeof(req));
        switch (req.dl_level) {
        case DL_PROMISC_PHYS:
                flag = &ds->ds_promisc_phys;
                break;
        case DL_PROMISC_SAP:
                flag = &ds->ds_promisc_sap;
                break;
        case DL_PROMISC_MULTI:
                flag = &ds->ds_promisc_multi;  /* every group frame comes */
                break;
        default:
                de_error_ack(q, mp, prim, DL_NOTSUPPORTED, 0);
                return;
        }
        if (!on && !*flag) {
                de_error_ack(q, mp, prim, DL_NOTENAB, 0);
                return;
        }
        mutex_lock(&de->de_lock);
        if (flag == &ds->ds_promisc_phys && *flag != on) {
                de->de_nphys += on ? 1 : -1;
                change = de->de_nphys == (on ? 1 : 0);
        }
        *flag = on;
        if (change && de->de_promisc != NULL)
                de->de_promisc(de, on);
        mutex_unlock(&de->de_lock);
        de_ok_ack(q, mp, prim);
}

static void
de_phys_addr(queue_t *q, mblk_t *mp)
{
        struct dlether_stream *ds = q->q_ptr;
        dl_phys_addr_req_t req;
        dl_phys_addr_ack_t ack = { DL_PHYS_ADDR_ACK, ETHER_ADDRLEN, 0 };
        mblk_t *rp;

        memcpy(&req, mp->b_rptr, sizeof(req));
        if (req.dl_addr_type != DL_FACT_PHYS_ADDR &&
            req.dl_addr_type != DL_CURR_PHYS_ADDR) {
                de_error_ack(q, mp, DL_PHYS_ADDR_REQ, DL_UNSUPPORTED, 0);
                return;
        }
        rp = de_reply(M_PCPROTO, sizeof(ack) + ETHER_ADDRLEN,
            DL_PHYS_ADDR_ACK);
        if (rp == NULL) {
                de_error_ack(q, mp, DL_PHYS_ADDR_REQ, DL_SYSERR, ENOSR);
                return;
        }
        ack.dl_addr_offset = sizeof(ack);
        memcpy(rp->b_rptr, &ack, sizeof(ack));
        memcpy(rp->b_rptr + sizeof(ack), ds->ds_de->de_addr, ETHER_ADDRLEN);
        freemsg(mp);
        qreply(q, rp);
}

/* DL_UNITDATA_REQ: one frame out, to the address given. */
static void
de_unitdata(queue_t *q, mblk_t *mp)
{
        struct dlether_stream *ds = q->q_ptr;
        struct dlether *de = ds->ds_de;
        dl_unitdata_req_t req;
        const uint8_t *dest;
        size_t len = msgdsize(mp->b_cont), frame;
        uint8_t *buf, *p;
        uint16_t sap;

        if (ds->ds_state != DL_IDLE) {
                de_error_ack(q, mp, DL_UNITDATA_REQ, DL_OUTSTATE, 0);
                return;
        }
        memcpy(&req, mp->b_rptr, sizeof(req));
        if ((req.dl_dest_addr_length != DE_DLADDRLEN &&
            req.dl_dest_addr_length != ETHER_ADDRLEN) ||
            req.dl_dest_addr_offset < sizeof(req) ||
            req.dl_dest_addr_offset + req.dl_dest_addr_length > MBLKL(mp)) {
                de_uderror(q, mp, DL_BADADDR);
                return;
        }
        if (len == 0 || len > ETHER_MTU) {
                de_uderror(q, mp, DL_BADDATA);
                return;
        }
        dest = mp->b_rptr + req.dl_dest_addr_offset;
        sap = ds->ds_sap;
        if (req.dl_dest_addr_length == DE_DLADDRLEN)
                memcpy(&sap, dest + ETHER_ADDRLEN, sizeof(sap));
        frame = ETHER_HDRLEN + len;
        if (frame < ETHER_MINFRAME)
                frame = ETHER_MINFRAME;
        if ((buf = kmem_zalloc(frame, KM_NOSLEEP)) == NULL) {
                atomic_inc_64_nv(&de->de_drops);
                freemsg(mp);
                return;
        }
        memcpy(buf, dest, ETHER_ADDRLEN);
        memcpy(buf + ETHER_ADDRLEN, de->de_addr, ETHER_ADDRLEN);
        buf[12] = (uint8_t)(sap >> 8);
        buf[13] = (uint8_t)sap;
        p = buf + ETHER_HDRLEN;
        for (mblk_t *bp = mp->b_cont; bp != NULL; bp = bp->b_cont) {
                if (DB_TYPE(bp) != M_DATA)
                        continue;
                memcpy(p, bp->b_rptr, MBLKL(bp));
                p += MBLKL(bp);
        }
        freemsg(mp);
        if (de->de_output(de, buf, frame) == 0)
                atomic_inc_64_nv(&de->de_opackets);
        else
                atomic_inc_64_nv(&de->de_drops);
        kmem_free(buf, frame);
}

static void
de_proto(queue_t *q, mblk_t *mp)
{
        t_uscalar_t prim;
        size_t need;

        if (MBLKL(mp) < sizeof(prim)) {
                de_error_ack(q, mp, 0, DL_BADPRIM, 0);
                return;
        }
        memcpy(&prim, mp->b_rptr, sizeof(prim));
        switch (prim) {
        case DL_INFO_REQ:
                need = sizeof(dl_info_req_t);
                break;
        case DL_BIND_REQ:
                need = sizeof(dl_bind_req_t);
                break;
        case DL_UNBIND_REQ:
                need = sizeof(dl_unbind_req_t);
                break;
        case DL_UNITDATA_REQ:
                need = sizeof(dl_unitdata_req_t);
                break;
        case DL_PHYS_ADDR_REQ:
                need = sizeof(dl_phys_addr_req_t);
                break;
        case DL_PROMISCON_REQ:
        case DL_PROMISCOFF_REQ:
                need = sizeof(dl_promiscon_req_t);
                break;
        case DL_ATTACH_REQ:
        case DL_DETACH_REQ:
                de_error_ack(q, mp, prim, DL_OUTSTATE, 0);        /* style 1 */
                return;
        case DL_ENABMULTI_REQ:
        case DL_DISABMULTI_REQ:
        case DL_SET_PHYS_ADDR_REQ:
        case DL_GET_STATISTICS_REQ:
        case DL_UDQOS_REQ:
        case DL_SUBS_BIND_REQ:
        case DL_SUBS_UNBIND_REQ:
                de_error_ack(q, mp, prim, DL_NOTSUPPORTED, 0);
                return;
        default:
                de_error_ack(q, mp, prim, DL_BADPRIM, 0);
                return;
        }
        if (MBLKL(mp) < need) {
                de_error_ack(q, mp, prim, DL_BADPRIM, 0);
                return;
        }
        switch (prim) {
        case DL_INFO_REQ:
                de_info(q, mp);
                break;
        case DL_BIND_REQ:
                de_bind(q, mp);
                break;
        case DL_UNBIND_REQ:
                de_unbind(q, mp);
                break;
        case DL_UNITDATA_REQ:
                de_unitdata(q, mp);
                break;
        case DL_PHYS_ADDR_REQ:
                de_phys_addr(q, mp);
                break;
        default:
                de_promisc(q, mp, prim);
                break;
        }
}

static int
de_wput(queue_t *q, mblk_t *mp)
{
        switch (DB_TYPE(mp)) {
        case M_PROTO:
        case M_PCPROTO:
                de_proto(q, mp);
                return 0;
        case M_FLUSH:
                flushq_reply(q, mp);
                return 0;
        case M_IOCTL:
                miocnak(q, mp, 0, EINVAL);
                return 0;
        default:
                freemsg(mp);            /* no raw mode */
                return 0;
        }
}
