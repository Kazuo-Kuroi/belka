/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * Virtio network device driver: a DLPI version 2, style 1,
 * connectionless provider (see doc/design/decisions.md, "virtio-net and
 * DLPI"). Each device is /dev/vionetN; each open is a stream of its
 * own, bound to an Ethernet type with DL_BIND_REQ.
 *
 * Queue 0 receives and queue 1 transmits; every buffer starts with the
 * 12-byte virtio-net header, all zeros on the way out (no offloads).
 * Receive completions are handed by the queue's interrupt thread to the
 * device's kernel thread, which copies each frame into a message for
 * every stream that wants it and reposts the buffer. A transmitted frame
 * is copied into one buffer, which its completion frees.
 *
 * Locks: vnet_lock (a mutex) covers the list of streams and their state;
 * the kernel thread holds it while it puts frames to the streams (a put
 * never waits). vnet_rxlock (a spin lock) covers the completed receive
 * buffers, and is taken inside the receive queue's vq_lock.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/dlpi.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mkdev.h>
#include <sys/spinlock.h>
#include <sys/stream.h>
#include <sys/strsubr.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>

#include "dev/virtio/virtioreg.h"
#include "dev/virtio/virtiovar.h"

#define VIRTIO_NET_F_MAC        5
#define VIRTIO_NET_F_STATUS     16

#define VN_HDRLEN       12              /* struct virtio_net_hdr, version 1 */
#define VN_ETHERLEN     14
#define VN_ADDRLEN      6
#define VN_MTU          1500
#define VN_MINFRAME     60              /* without the frame check sequence */
#define VN_RXBUFSZ      2048
#define VN_NRX          64
#define VN_QSIZE        128
#define VN_MAXUNITS     8
#define VN_DLADDRLEN    (VN_ADDRLEN + 2)

struct vnet_rxbuf {
        struct vnet_rxbuf *rb_next;
        struct vionet_softc *rb_sc;
        uint32_t        rb_len;         /* what the device wrote */
        uint8_t         *rb_data;       /* VN_RXBUFSZ bytes */
};

struct vnet_txbuf {
        size_t          tb_size;        /* of this allocation */
        uint8_t         tb_data[];
};

/* One open stream. */
struct vnet_stream {
        struct vnet_stream *vs_next;
        struct vionet_softc *vs_sc;
        queue_t         *vs_rq;
        int             vs_state;       /* DL_UNBOUND, DL_IDLE */
        uint16_t        vs_sap;
        int             vs_promisc_phys;
        int             vs_promisc_sap;
        int             vs_promisc_multi;
};

struct vionet_softc {
        struct virtio_softc *vnet_virtio;
        struct virtqueue *vnet_rxq;
        struct virtqueue *vnet_txq;
        unsigned int    vnet_unit;
        uint8_t         vnet_addr[VN_ADDRLEN];
        struct mutex    vnet_lock;
        struct vnet_stream *vnet_streams;
        struct spinlock vnet_rxlock;
        struct sv       vnet_rxsv;
        struct vnet_rxbuf *vnet_rxhead;     /* completed, for the thread */
        struct vnet_rxbuf *vnet_rxtail;
        volatile uint64_t vnet_ipackets;
        volatile uint64_t vnet_opackets;
        volatile uint64_t vnet_drops;
};

static struct vionet_softc *vionet_units[VN_MAXUNITS];
static unsigned int vionet_nunits;

static const uint8_t vnet_broadcast[VN_ADDRLEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static int vnet_open(queue_t *, dev_t *, int, int, struct cred *);
static int vnet_close(queue_t *, int, struct cred *);
static int vnet_rput(queue_t *, mblk_t *);
static int vnet_wput(queue_t *, mblk_t *);

static struct module_info vnet_minfo = {
        0x564e, "vionet", 0, INFPSZ, 65536, 4096
};
static struct qinit vnet_rinit = {
        vnet_rput, NULL, vnet_open, vnet_close, NULL, &vnet_minfo, NULL
};
static struct qinit vnet_winit = {
        vnet_wput, NULL, NULL, NULL, NULL, &vnet_minfo, NULL
};
static struct streamtab vionet_info = { &vnet_rinit, &vnet_winit, NULL, NULL };

/* --- The device. --- */

/* Receive completion: from the interrupt thread, with vq_lock held. */
static void
vnet_rxdone(void *cookie, uint32_t len)
{
        struct vnet_rxbuf *rb = cookie;
        struct vionet_softc *sc = rb->rb_sc;
        int s = spin_lock(&sc->vnet_rxlock);

        rb->rb_len = len;
        rb->rb_next = NULL;
        if (sc->vnet_rxtail != NULL)
                sc->vnet_rxtail->rb_next = rb;
        else
                sc->vnet_rxhead = rb;
        sc->vnet_rxtail = rb;
        sv_signal(&sc->vnet_rxsv);
        spin_unlock(&sc->vnet_rxlock, s);
}

/* Transmit completion: the frame's buffer is done with. */
static void
vnet_txdone(void *cookie, uint32_t len __unused)
{
        struct vnet_txbuf *tb = cookie;

        kmem_free(tb, tb->tb_size);
}

static void
vnet_post(struct vionet_softc *sc, struct vnet_rxbuf *rb)
{
        struct vq_seg seg = { rb->rb_data, VN_RXBUFSZ };

        if (vq_submit(sc->vnet_rxq, &seg, 0, 1, rb) != 0)
                panic("vionet%u: cannot post a receive buffer", sc->vnet_unit);
}

/* sap and station address, in DLPI's form (address, then type). */
static void
vnet_dladdr(uint8_t *to, const uint8_t *station, uint16_t sap)
{
        memcpy(to, station, VN_ADDRLEN);
        memcpy(to + VN_ADDRLEN, &sap, sizeof(sap));
}

/* A DL_UNITDATA_IND for one stream: the frame's addresses, then data. */
static mblk_t *
vnet_unitdata_ind(const uint8_t *frame, uint16_t type, const uint8_t *data,
    size_t len)
{
        dl_unitdata_ind_t ind;
        mblk_t *mp, *dp;

        if ((mp = allocb(sizeof(ind) + 2 * VN_DLADDRLEN, BPRI_MED)) == NULL)
                return NULL;
        if ((dp = allocb(len, BPRI_MED)) == NULL) {
                freeb(mp);
                return NULL;
        }
        memset(&ind, 0, sizeof(ind));
        ind.dl_primitive = DL_UNITDATA_IND;
        ind.dl_dest_addr_length = VN_DLADDRLEN;
        ind.dl_dest_addr_offset = sizeof(ind);
        ind.dl_src_addr_length = VN_DLADDRLEN;
        ind.dl_src_addr_offset = sizeof(ind) + VN_DLADDRLEN;
        ind.dl_group_address = (frame[0] & 1) != 0;
        mp->b_datap->db_type = M_PROTO;
        memcpy(mp->b_wptr, &ind, sizeof(ind));
        vnet_dladdr(mp->b_wptr + sizeof(ind), frame, type);
        vnet_dladdr(mp->b_wptr + sizeof(ind) + VN_DLADDRLEN,
            frame + VN_ADDRLEN, type);
        mp->b_wptr += sizeof(ind) + 2 * VN_DLADDRLEN;
        memcpy(dp->b_wptr, data, len);
        dp->b_wptr += len;
        mp->b_cont = dp;
        return mp;
}

/* A received frame (after the virtio header): to whichever streams want it. */
static void
vnet_input(struct vionet_softc *sc, const uint8_t *frame, size_t len)
{
        uint16_t type;
        int group, forus;

        if (len < VN_ETHERLEN)
                return;
        type = (uint16_t)(frame[12] << 8 | frame[13]);
        if (type < 0x0600)
                return;                 /* 802.3 length: not yet */
        group = (frame[0] & 1) != 0;
        forus = group || memcmp(frame, sc->vnet_addr, VN_ADDRLEN) == 0;
        atomic_inc_64_nv(&sc->vnet_ipackets);

        mutex_lock(&sc->vnet_lock);
        for (struct vnet_stream *vs = sc->vnet_streams; vs != NULL;
            vs = vs->vs_next) {
                mblk_t *mp;

                if (!forus && !vs->vs_promisc_phys)
                        continue;
                if (!vs->vs_promisc_sap &&
                    (vs->vs_state != DL_IDLE || vs->vs_sap != type))
                        continue;
                mp = vnet_unitdata_ind(frame, type, frame + VN_ETHERLEN,
                    len - VN_ETHERLEN);
                if (mp == NULL) {
                        atomic_inc_64_nv(&sc->vnet_drops);
                        continue;
                }
                (void)put(vs->vs_rq, mp);
        }
        mutex_unlock(&sc->vnet_lock);
}

/* The device's kernel thread: frames received, to the streams. */
static void
vnet_thread(void *arg)
{
        struct vionet_softc *sc = arg;

        for (;;) {
                struct vnet_rxbuf *rb, *next;
                int s = spin_lock(&sc->vnet_rxlock);

                while (sc->vnet_rxhead == NULL) {
                        sv_wait(&sc->vnet_rxsv, &sc->vnet_rxlock, s);
                        s = spin_lock(&sc->vnet_rxlock);
                }
                rb = sc->vnet_rxhead;
                sc->vnet_rxhead = sc->vnet_rxtail = NULL;
                spin_unlock(&sc->vnet_rxlock, s);

                for (; rb != NULL; rb = next) {
                        next = rb->rb_next;
                        if (rb->rb_len > VN_HDRLEN)
                                vnet_input(sc, rb->rb_data + VN_HDRLEN,
                                    rb->rb_len - VN_HDRLEN);
                        vnet_post(sc, rb);
                }
        }
}

int
vionet_attach(struct virtio_softc *vsc)
{
        struct vionet_softc *sc;
        char name[16];
        uint64_t features;

        if (vionet_nunits >= VN_MAXUNITS) {
                printf("vionet: too many devices\n");
                return -1;
        }
        features = virtio_negotiate(vsc,
            ((uint64_t)1 << VIRTIO_NET_F_MAC) |
            ((uint64_t)1 << VIRTIO_NET_F_STATUS));
        if (features == 0 || !virtio_has_feature(vsc, VIRTIO_NET_F_MAC)) {
                printf("vionet: feature negotiation failed\n");
                return -1;
        }
        sc = kmem_zalloc(sizeof(*sc), KM_SLEEP);
        sc->vnet_virtio = vsc;
        sc->vnet_unit = vionet_nunits;
        mutex_init(&sc->vnet_lock, "vionet");
        spinlock_init(&sc->vnet_rxlock, "vionetrx");
        sv_init(&sc->vnet_rxsv, "vionetrx");
        for (int i = 0; i < VN_ADDRLEN; i++)
                sc->vnet_addr[i] = (uint8_t)virtio_cfg_read(vsc, i, 1);
        snprintf(name, sizeof(name), "vionet%u", sc->vnet_unit);
        sc->vnet_rxq = virtio_alloc_vq(vsc, 0, VN_QSIZE, vnet_rxdone, name);
        sc->vnet_txq = virtio_alloc_vq(vsc, 1, VN_QSIZE, vnet_txdone, name);
        if (sc->vnet_rxq == NULL || sc->vnet_txq == NULL) {
                printf("%s: cannot set up its queues\n", name);
                kmem_free(sc, sizeof(*sc));
                return -1;
        }
        vsc->vs_child = sc;
        virtio_ready(vsc);
        for (int i = 0; i < VN_NRX; i++) {
                struct vnet_rxbuf *rb = kmem_zalloc(sizeof(*rb), KM_SLEEP);

                rb->rb_sc = sc;
                rb->rb_data = kmem_alloc(VN_RXBUFSZ, KM_SLEEP);
                vnet_post(sc, rb);
        }
        if (strdev_register(name, &vionet_info, SQLVL_QPAIR,
            DEV_VIONET_MAJOR, sc->vnet_unit, 0600, sc) == NULL) {
                printf("%s: no room for its device\n", name);
                return -1;
        }
        vionet_units[vionet_nunits++] = sc;
        (void)kthread_create(vnet_thread, sc, PRI_KERNEL + 5, name);
        printf("%s: address %02x:%02x:%02x:%02x:%02x:%02x\n", name,
            sc->vnet_addr[0], sc->vnet_addr[1], sc->vnet_addr[2], sc->vnet_addr[3],
            sc->vnet_addr[4], sc->vnet_addr[5]);
        return 0;
}

/* --- The streams. --- */

static int
vnet_open(queue_t *q, dev_t *devp, int flag __unused, int sflag,
    struct cred *cr __unused)
{
        unsigned int unit = STR_UNIT(*devp);
        struct vionet_softc *sc;
        struct vnet_stream *vs;

        if (sflag != CLONEOPEN || unit >= vionet_nunits)
                return ENXIO;
        sc = vionet_units[unit];
        vs = kmem_zalloc(sizeof(*vs), KM_SLEEP);
        vs->vs_sc = sc;
        vs->vs_rq = q;
        vs->vs_state = DL_UNBOUND;
        q->q_ptr = WR(q)->q_ptr = vs;
        mutex_lock(&sc->vnet_lock);
        vs->vs_next = sc->vnet_streams;
        sc->vnet_streams = vs;
        mutex_unlock(&sc->vnet_lock);
        return 0;
}

static int
vnet_close(queue_t *q, int flag __unused, struct cred *cr __unused)
{
        struct vnet_stream *vs = q->q_ptr, **link;
        struct vionet_softc *sc = vs->vs_sc;

        mutex_lock(&sc->vnet_lock);
        for (link = &sc->vnet_streams; *link != NULL; link = &(*link)->vs_next)
                if (*link == vs) {
                        *link = vs->vs_next;
                        break;
                }
        mutex_unlock(&sc->vnet_lock);
        q->q_ptr = WR(q)->q_ptr = NULL;
        kmem_free(vs, sizeof(*vs));
        return 0;
}

/* Up to the head, unless it is full: then the frame is dropped. */
static int
vnet_rput(queue_t *q, mblk_t *mp)
{
        struct vnet_stream *vs = q->q_ptr;

        if (DB_TYPE(mp) >= QPCTL || canputnext(q)) {
                (void)putnext(q, mp);
        } else {
                atomic_inc_64_nv(&vs->vs_sc->vnet_drops);
                freemsg(mp);
        }
        return 0;
}

/* A primitive's reply, of type and size, its first word prim. */
static mblk_t *
vnet_reply(unsigned char type, size_t size, t_uscalar_t prim)
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
vnet_error_ack(queue_t *q, mblk_t *mp, t_uscalar_t prim, t_uscalar_t err,
    int uerr)
{
        dl_error_ack_t ack = { DL_ERROR_ACK, prim, err, (t_uscalar_t)uerr };
        mblk_t *rp = vnet_reply(M_PCPROTO, sizeof(ack), DL_ERROR_ACK);

        freemsg(mp);
        if (rp == NULL)
                return;
        memcpy(rp->b_rptr, &ack, sizeof(ack));
        qreply(q, rp);
}

static void
vnet_ok_ack(queue_t *q, mblk_t *mp, t_uscalar_t prim)
{
        dl_ok_ack_t ack = { DL_OK_ACK, prim };
        mblk_t *rp = vnet_reply(M_PCPROTO, sizeof(ack), DL_OK_ACK);

        freemsg(mp);
        if (rp == NULL)
                return;
        memcpy(rp->b_rptr, &ack, sizeof(ack));
        qreply(q, rp);
}

static void
vnet_uderror(queue_t *q, mblk_t *mp, t_uscalar_t err)
{
        dl_uderror_ind_t ind;
        mblk_t *rp = vnet_reply(M_PROTO, sizeof(ind), DL_UDERROR_IND);

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
vnet_info(queue_t *q, mblk_t *mp)
{
        struct vnet_stream *vs = q->q_ptr;
        dl_info_ack_t ack;
        size_t size = sizeof(ack) + VN_DLADDRLEN + VN_ADDRLEN;
        mblk_t *rp = vnet_reply(M_PCPROTO, size, DL_INFO_ACK);

        freemsg(mp);
        if (rp == NULL)
                return;
        memset(&ack, 0, sizeof(ack));
        ack.dl_primitive = DL_INFO_ACK;
        ack.dl_max_sdu = VN_MTU;
        ack.dl_min_sdu = 1;
        ack.dl_addr_length = VN_DLADDRLEN;
        ack.dl_mac_type = DL_ETHER;
        ack.dl_current_state = (t_uscalar_t)vs->vs_state;
        ack.dl_sap_length = -2;
        ack.dl_service_mode = DL_CLDLS;
        ack.dl_provider_style = DL_STYLE1;
        ack.dl_addr_offset = sizeof(ack);
        ack.dl_version = DL_VERSION_2;
        ack.dl_brdcst_addr_length = VN_ADDRLEN;
        ack.dl_brdcst_addr_offset = sizeof(ack) + VN_DLADDRLEN;
        memcpy(rp->b_rptr, &ack, sizeof(ack));
        vnet_dladdr(rp->b_rptr + sizeof(ack), vs->vs_sc->vnet_addr, vs->vs_sap);
        memcpy(rp->b_rptr + sizeof(ack) + VN_DLADDRLEN, vnet_broadcast,
            VN_ADDRLEN);
        qreply(q, rp);
}

static void
vnet_bind(queue_t *q, mblk_t *mp)
{
        struct vnet_stream *vs = q->q_ptr;
        struct vionet_softc *sc = vs->vs_sc;
        dl_bind_req_t req;
        dl_bind_ack_t ack;
        mblk_t *rp;

        memcpy(&req, mp->b_rptr, sizeof(req));
        if (vs->vs_state != DL_UNBOUND) {
                vnet_error_ack(q, mp, DL_BIND_REQ, DL_OUTSTATE, 0);
                return;
        }
        if (req.dl_service_mode != DL_CLDLS) {
                vnet_error_ack(q, mp, DL_BIND_REQ, DL_UNSUPPORTED, 0);
                return;
        }
        if (req.dl_sap < 0x0600 || req.dl_sap > 0xffff) {
                vnet_error_ack(q, mp, DL_BIND_REQ, DL_BADSAP, 0);
                return;
        }
        rp = vnet_reply(M_PCPROTO, sizeof(ack) + VN_DLADDRLEN, DL_BIND_ACK);
        if (rp == NULL) {
                vnet_error_ack(q, mp, DL_BIND_REQ, DL_SYSERR, ENOSR);
                return;
        }
        mutex_lock(&sc->vnet_lock);
        vs->vs_sap = (uint16_t)req.dl_sap;
        vs->vs_state = DL_IDLE;
        mutex_unlock(&sc->vnet_lock);
        memset(&ack, 0, sizeof(ack));
        ack.dl_primitive = DL_BIND_ACK;
        ack.dl_sap = req.dl_sap;
        ack.dl_addr_length = VN_DLADDRLEN;
        ack.dl_addr_offset = sizeof(ack);
        memcpy(rp->b_rptr, &ack, sizeof(ack));
        vnet_dladdr(rp->b_rptr + sizeof(ack), sc->vnet_addr, vs->vs_sap);
        freemsg(mp);
        qreply(q, rp);
}

static void
vnet_unbind(queue_t *q, mblk_t *mp)
{
        struct vnet_stream *vs = q->q_ptr;

        if (vs->vs_state != DL_IDLE) {
                vnet_error_ack(q, mp, DL_UNBIND_REQ, DL_OUTSTATE, 0);
                return;
        }
        mutex_lock(&vs->vs_sc->vnet_lock);
        vs->vs_state = DL_UNBOUND;
        vs->vs_sap = 0;
        mutex_unlock(&vs->vs_sc->vnet_lock);
        vnet_ok_ack(q, mp, DL_UNBIND_REQ);
}

static void
vnet_promisc(queue_t *q, mblk_t *mp, t_uscalar_t prim)
{
        struct vnet_stream *vs = q->q_ptr;
        dl_promiscon_req_t req;
        int on = prim == DL_PROMISCON_REQ, *flag;

        memcpy(&req, mp->b_rptr, sizeof(req));
        switch (req.dl_level) {
        case DL_PROMISC_PHYS:
                flag = &vs->vs_promisc_phys;
                break;
        case DL_PROMISC_SAP:
                flag = &vs->vs_promisc_sap;
                break;
        case DL_PROMISC_MULTI:
                flag = &vs->vs_promisc_multi;  /* every group frame comes */
                break;
        default:
                vnet_error_ack(q, mp, prim, DL_NOTSUPPORTED, 0);
                return;
        }
        if (!on && !*flag) {
                vnet_error_ack(q, mp, prim, DL_NOTENAB, 0);
                return;
        }
        mutex_lock(&vs->vs_sc->vnet_lock);
        *flag = on;
        mutex_unlock(&vs->vs_sc->vnet_lock);
        vnet_ok_ack(q, mp, prim);
}

static void
vnet_phys_addr(queue_t *q, mblk_t *mp)
{
        struct vnet_stream *vs = q->q_ptr;
        dl_phys_addr_req_t req;
        dl_phys_addr_ack_t ack = { DL_PHYS_ADDR_ACK, VN_ADDRLEN, 0 };
        mblk_t *rp;

        memcpy(&req, mp->b_rptr, sizeof(req));
        if (req.dl_addr_type != DL_FACT_PHYS_ADDR &&
            req.dl_addr_type != DL_CURR_PHYS_ADDR) {
                vnet_error_ack(q, mp, DL_PHYS_ADDR_REQ, DL_UNSUPPORTED, 0);
                return;
        }
        rp = vnet_reply(M_PCPROTO, sizeof(ack) + VN_ADDRLEN, DL_PHYS_ADDR_ACK);
        if (rp == NULL) {
                vnet_error_ack(q, mp, DL_PHYS_ADDR_REQ, DL_SYSERR, ENOSR);
                return;
        }
        ack.dl_addr_offset = sizeof(ack);
        memcpy(rp->b_rptr, &ack, sizeof(ack));
        memcpy(rp->b_rptr + sizeof(ack), vs->vs_sc->vnet_addr, VN_ADDRLEN);
        freemsg(mp);
        qreply(q, rp);
}

/* DL_UNITDATA_REQ: one frame out, to the address given. */
static void
vnet_unitdata(queue_t *q, mblk_t *mp)
{
        struct vnet_stream *vs = q->q_ptr;
        struct vionet_softc *sc = vs->vs_sc;
        dl_unitdata_req_t req;
        const uint8_t *dest;
        struct vnet_txbuf *tb;
        struct vq_seg seg;
        size_t len = msgdsize(mp->b_cont), frame, size;
        uint16_t sap;
        uint8_t *p;

        if (vs->vs_state != DL_IDLE) {
                vnet_error_ack(q, mp, DL_UNITDATA_REQ, DL_OUTSTATE, 0);
                return;
        }
        memcpy(&req, mp->b_rptr, sizeof(req));
        if ((req.dl_dest_addr_length != VN_DLADDRLEN &&
            req.dl_dest_addr_length != VN_ADDRLEN) ||
            req.dl_dest_addr_offset < sizeof(req) ||
            req.dl_dest_addr_offset + req.dl_dest_addr_length > MBLKL(mp)) {
                vnet_uderror(q, mp, DL_BADADDR);
                return;
        }
        if (len == 0 || len > VN_MTU) {
                vnet_uderror(q, mp, DL_BADDATA);
                return;
        }
        dest = mp->b_rptr + req.dl_dest_addr_offset;
        sap = vs->vs_sap;
        if (req.dl_dest_addr_length == VN_DLADDRLEN)
                memcpy(&sap, dest + VN_ADDRLEN, sizeof(sap));
        frame = VN_ETHERLEN + len;
        if (frame < VN_MINFRAME)
                frame = VN_MINFRAME;
        size = sizeof(*tb) + VN_HDRLEN + frame;
        if ((tb = kmem_zalloc(size, KM_NOSLEEP)) == NULL) {
                atomic_inc_64_nv(&sc->vnet_drops);
                freemsg(mp);
                return;
        }
        tb->tb_size = size;
        p = tb->tb_data + VN_HDRLEN;    /* the virtio header stays zero */
        memcpy(p, dest, VN_ADDRLEN);
        memcpy(p + VN_ADDRLEN, sc->vnet_addr, VN_ADDRLEN);
        p[12] = (uint8_t)(sap >> 8);
        p[13] = (uint8_t)sap;
        p += VN_ETHERLEN;
        for (mblk_t *bp = mp->b_cont; bp != NULL; bp = bp->b_cont) {
                if (DB_TYPE(bp) != M_DATA)
                        continue;
                memcpy(p, bp->b_rptr, MBLKL(bp));
                p += MBLKL(bp);
        }
        freemsg(mp);
        seg.vs_buf = tb->tb_data;
        seg.vs_len = VN_HDRLEN + frame;
        if (vq_trysubmit(sc->vnet_txq, &seg, 1, 0, tb) != 0) {
                atomic_inc_64_nv(&sc->vnet_drops);
                kmem_free(tb, size);
                return;
        }
        atomic_inc_64_nv(&sc->vnet_opackets);
}

static void
vnet_proto(queue_t *q, mblk_t *mp)
{
        t_uscalar_t prim;
        size_t need;

        if (MBLKL(mp) < sizeof(prim)) {
                vnet_error_ack(q, mp, 0, DL_BADPRIM, 0);
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
                vnet_error_ack(q, mp, prim, DL_OUTSTATE, 0);      /* style 1 */
                return;
        case DL_ENABMULTI_REQ:
        case DL_DISABMULTI_REQ:
        case DL_SET_PHYS_ADDR_REQ:
        case DL_GET_STATISTICS_REQ:
        case DL_UDQOS_REQ:
        case DL_SUBS_BIND_REQ:
        case DL_SUBS_UNBIND_REQ:
                vnet_error_ack(q, mp, prim, DL_NOTSUPPORTED, 0);
                return;
        default:
                vnet_error_ack(q, mp, prim, DL_BADPRIM, 0);
                return;
        }
        if (MBLKL(mp) < need) {
                vnet_error_ack(q, mp, prim, DL_BADPRIM, 0);
                return;
        }
        switch (prim) {
        case DL_INFO_REQ:
                vnet_info(q, mp);
                break;
        case DL_BIND_REQ:
                vnet_bind(q, mp);
                break;
        case DL_UNBIND_REQ:
                vnet_unbind(q, mp);
                break;
        case DL_UNITDATA_REQ:
                vnet_unitdata(q, mp);
                break;
        case DL_PHYS_ADDR_REQ:
                vnet_phys_addr(q, mp);
                break;
        default:
                vnet_promisc(q, mp, prim);
                break;
        }
}

static int
vnet_wput(queue_t *q, mblk_t *mp)
{
        switch (DB_TYPE(mp)) {
        case M_PROTO:
        case M_PCPROTO:
                vnet_proto(q, mp);
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
