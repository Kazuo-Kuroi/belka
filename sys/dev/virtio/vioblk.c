/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Virtio block device driver.
 *
 * One request queue. Each request is a 16-byte header the device
 * reads, the data, and a status byte the device writes; the caller
 * sleeps on a semaphore in the request until the queue's interrupt
 * thread sees it complete. Sectors are always 512 bytes in the virtio
 * block protocol, whatever the device's preferred block size.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/blkdev.h>
#include <sys/errno.h>
#include <sys/kmem.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include "dev/virtio/virtioreg.h"
#include "dev/virtio/virtiovar.h"

#define VIOBLK_SECSIZE  512
#define VIOBLK_MAXIO    (64 * 1024)
#define VIOBLK_QSIZE    128

/* The header must be exactly what the device reads. */
struct vioblk_req {
        uint32_t        vr_type;
        uint32_t        vr_reserved;
        uint64_t        vr_sector;
        uint8_t         vr_status;      /* written by the device */
        struct sema     vr_done;
};

CTASSERT(offsetof(struct vioblk_req, vr_status) == 16);

struct vioblk_softc {
        struct virtio_softc *vb_virtio;
        struct virtqueue *vb_vq;
        struct kmem_zone *vb_reqzone;
        struct blkdev   vb_bd;
};

static void vioblk_done(void *, uint32_t);
static int vioblk_io(struct vioblk_softc *, uint32_t, uint64_t, void *,
    size_t);
static int vioblk_read(struct blkdev *, uint64_t, void *, size_t);
static int vioblk_write(struct blkdev *, uint64_t, const void *, size_t);
static int vioblk_flush(struct blkdev *);

static const struct blkdev_ops vioblk_ops = {
        vioblk_read, vioblk_write, vioblk_flush
};

/* Completion, from the queue's interrupt thread with vq_lock held. */
static void
vioblk_done(void *cookie, uint32_t len __unused)
{
        struct vioblk_req *vr = cookie;

        vsema(&vr->vr_done);
}

/* Issue one request and wait for it. Returns 0 or an errno value. */
static int
vioblk_io(struct vioblk_softc *vb, uint32_t type, uint64_t sector,
    void *buf, size_t len)
{
        struct vioblk_req *vr = kmem_zone_alloc(vb->vb_reqzone, KM_SLEEP);
        struct vq_seg segs[3];
        int nseg = 0, nout;
        uint8_t status;

        vr->vr_type = type;
        vr->vr_reserved = 0;
        vr->vr_sector = sector;
        vr->vr_status = 0xff;
        sema_init(&vr->vr_done, 0, "vioblk");

        segs[nseg].vs_buf = vr;
        segs[nseg++].vs_len = 16;
        if (type == VIRTIO_BLK_T_OUT) {
                segs[nseg].vs_buf = buf;
                segs[nseg++].vs_len = len;
        }
        nout = nseg;
        if (type == VIRTIO_BLK_T_IN) {
                segs[nseg].vs_buf = buf;
                segs[nseg++].vs_len = len;
        }
        segs[nseg].vs_buf = &vr->vr_status;
        segs[nseg++].vs_len = 1;

        if (vq_submit(vb->vb_vq, segs, nout, nseg - nout, vr) != 0) {
                kmem_zone_free(vb->vb_reqzone, vr);
                return EINVAL;
        }
        psema(&vr->vr_done);
        status = vr->vr_status;
        kmem_zone_free(vb->vb_reqzone, vr);

        switch (status) {
        case VIRTIO_BLK_S_OK:
                return 0;
        case VIRTIO_BLK_S_UNSUPP:
                return EOPNOTSUPP;
        default:
                return EIO;
        }
}

static int
vioblk_read(struct blkdev *bd, uint64_t sector, void *buf, size_t nsec)
{
        return vioblk_io(bd->bd_softc, VIRTIO_BLK_T_IN, sector, buf,
            nsec * VIOBLK_SECSIZE);
}

static int
vioblk_write(struct blkdev *bd, uint64_t sector, const void *buf,
    size_t nsec)
{
        /* The device only reads the buffer. */
        return vioblk_io(bd->bd_softc, VIRTIO_BLK_T_OUT, sector,
            (void *)(uintptr_t)buf, nsec * VIOBLK_SECSIZE);
}

static int
vioblk_flush(struct blkdev *bd)
{
        struct vioblk_softc *vb = bd->bd_softc;

        if (!virtio_has_feature(vb->vb_virtio, VIRTIO_BLK_F_FLUSH))
                return 0;
        return vioblk_io(vb, VIRTIO_BLK_T_FLUSH, 0, NULL, 0);
}

int
vioblk_attach(struct virtio_softc *sc)
{
        struct vioblk_softc *vb;
        uint64_t features;

        features = virtio_negotiate(sc,
            ((uint64_t)1 << VIRTIO_BLK_F_RO) |
            ((uint64_t)1 << VIRTIO_BLK_F_BLK_SIZE) |
            ((uint64_t)1 << VIRTIO_BLK_F_FLUSH));
        if (features == 0) {
                printf("vioblk: feature negotiation failed\n");
                return -1;
        }

        vb = kmem_zalloc(sizeof(*vb), KM_SLEEP);
        vb->vb_virtio = sc;
        vb->vb_reqzone = kmem_zone_init(sizeof(struct vioblk_req), "vioblk");
        vb->vb_vq = virtio_alloc_vq(sc, 0, VIOBLK_QSIZE, vioblk_done,
            "vioblk");
        if (vb->vb_vq == NULL) {
                kmem_free(vb, sizeof(*vb));
                return -1;
        }
        sc->vs_child = vb;

        vb->vb_bd.bd_secsize = VIOBLK_SECSIZE;
        vb->vb_bd.bd_nsectors = virtio_cfg_read64(sc,
            VIRTIO_BLK_CFG_CAPACITY);
        vb->vb_bd.bd_maxio = VIOBLK_MAXIO;
        vb->vb_bd.bd_readonly = virtio_has_feature(sc, VIRTIO_BLK_F_RO);
        vb->vb_bd.bd_ops = &vioblk_ops;
        vb->vb_bd.bd_softc = vb;
        virtio_ready(sc);
        return blkdev_register(&vb->vb_bd, "vd") == 0 ? 0 : -1;
}
