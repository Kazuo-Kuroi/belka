/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * Virtio network device driver (see doc/design/decisions.md, "virtio-net
 * and DLPI"). Each device is /dev/vionetN, whose streams are the shared
 * Ethernet DLPI provider's (net/dlether.c).
 *
 * Queue 0 receives and queue 1 transmits; every buffer starts with the
 * 12-byte virtio-net header, all zeros on the way out (no offloads).
 * Receive completions are handed by the queue's interrupt thread to the
 * device's kernel thread, which gives each frame to dlether_input and
 * reposts the buffer. A transmitted frame is copied into one buffer,
 * which its completion frees.
 *
 * Locks: vnet_rxlock (a spin lock) covers the completed receive
 * buffers, and is taken inside the receive queue's vq_lock.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/dlether.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/mkdev.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include "dev/virtio/virtioreg.h"
#include "dev/virtio/virtiovar.h"

#define VIRTIO_NET_F_MAC        5
#define VIRTIO_NET_F_STATUS     16

#define VN_HDRLEN       12              /* struct virtio_net_hdr, version 1 */
#define VN_RXBUFSZ      2048
#define VN_NRX          64
#define VN_QSIZE        128
#define VN_MAXUNITS     8

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

struct vionet_softc {
        struct dlether  vnet_dl;
        struct virtio_softc *vnet_virtio;
        struct virtqueue *vnet_rxq;
        struct virtqueue *vnet_txq;
        unsigned int    vnet_unit;
        struct spinlock vnet_rxlock;
        struct sv       vnet_rxsv;
        struct vnet_rxbuf *vnet_rxhead;     /* completed, for the thread */
        struct vnet_rxbuf *vnet_rxtail;
};

static unsigned int vionet_nunits;

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
                                dlether_input(&sc->vnet_dl,
                                    rb->rb_data + VN_HDRLEN,
                                    rb->rb_len - VN_HDRLEN);
                        vnet_post(sc, rb);
                }
        }
}

/* A frame out: copied after a zeroed virtio header. */
static int
vnet_output(struct dlether *de, const uint8_t *frame, size_t len)
{
        struct vionet_softc *sc = de->de_softc;
        size_t size = sizeof(struct vnet_txbuf) + VN_HDRLEN + len;
        struct vnet_txbuf *tb;
        struct vq_seg seg;

        if ((tb = kmem_zalloc(size, KM_NOSLEEP)) == NULL)
                return -1;
        tb->tb_size = size;
        memcpy(tb->tb_data + VN_HDRLEN, frame, len);
        seg.vs_buf = tb->tb_data;
        seg.vs_len = VN_HDRLEN + len;
        if (vq_trysubmit(sc->vnet_txq, &seg, 1, 0, tb) != 0) {
                kmem_free(tb, size);
                return -1;
        }
        return 0;
}

int
vionet_attach(struct virtio_softc *vsc)
{
        struct vionet_softc *sc;
        uint64_t features;
        const char *name;

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
        spinlock_init(&sc->vnet_rxlock, "vionetrx");
        sv_init(&sc->vnet_rxsv, "vionetrx");
        snprintf(sc->vnet_dl.de_name, sizeof(sc->vnet_dl.de_name),
            "vionet%u", sc->vnet_unit);
        name = sc->vnet_dl.de_name;
        for (int i = 0; i < ETHER_ADDRLEN; i++)
                sc->vnet_dl.de_addr[i] = (uint8_t)virtio_cfg_read(vsc, i, 1);
        sc->vnet_dl.de_output = vnet_output;
        sc->vnet_dl.de_softc = sc;
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
        if (dlether_register(&sc->vnet_dl, DEV_VIONET_MAJOR,
            sc->vnet_unit) != 0)
                return -1;
        vionet_nunits++;
        (void)kthread_create(vnet_thread, sc, PRI_KERNEL + 5, name);
        printf("%s: address %02x:%02x:%02x:%02x:%02x:%02x\n", name,
            sc->vnet_dl.de_addr[0], sc->vnet_dl.de_addr[1],
            sc->vnet_dl.de_addr[2], sc->vnet_dl.de_addr[3],
            sc->vnet_dl.de_addr[4], sc->vnet_dl.de_addr[5]);
        return 0;
}
