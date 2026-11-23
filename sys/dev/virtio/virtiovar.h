/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Virtio devices: the transport (PCI, for now) and split virtqueues,
 * shared by the device drivers.
 *
 * A driver sees its device as a struct virtio_softc: it negotiates
 * features, allocates virtqueues, reads its configuration space, and
 * marks itself ready. Requests are chains of buffer segments; each
 * segment is either read by the device ("out") or written by it ("in"),
 * outs first.
 *
 * Buffers are given by kernel virtual address and translated page by
 * page, so they may lie anywhere in the kernel address space; one
 * segment may therefore become several descriptors.
 *
 * Locking: vq_lock protects a virtqueue's rings, free list, and cookies.
 * The completion callback runs with vq_lock held, from the queue's
 * interrupt thread, and may only wake threads.
 */

#ifndef _DEV_VIRTIO_VIRTIOVAR_H_
#define _DEV_VIRTIO_VIRTIOVAR_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/spinlock.h>
#include <sys/synch.h>

#include "dev/virtio/virtioreg.h"

#define VIRTIO_MAXQUEUES        8
#define VIRTIO_MAXQSIZE         128     /* rings fit in one page each */

struct pci_dev;
struct virtio_softc;

struct vq_seg {
        const void      *vs_buf;
        size_t          vs_len;
};

struct virtqueue {
        struct virtio_softc *vq_sc;
        struct spinlock vq_lock;
        struct sv       vq_space;       /* descriptors were freed */
        uint16_t        vq_index;
        uint16_t        vq_size;
        uint16_t        vq_free_head;
        uint16_t        vq_nfree;
        uint16_t        vq_last_used;
        volatile struct vring_desc *vq_desc;
        volatile struct vring_avail *vq_avail;
        volatile struct vring_used *vq_used;
        volatile uint16_t *vq_notify;
        void            **vq_cookie;    /* by head descriptor */
        void            (*vq_done)(void *, uint32_t);
        void            *vq_intr;
        volatile uint64_t vq_completed;
};

struct virtio_softc {
        struct pci_dev  *vs_pci;
        volatile uint8_t *vs_common;
        volatile uint8_t *vs_notify_base;
        volatile uint8_t *vs_devcfg;
        uint32_t        vs_notify_mult;
        uint64_t        vs_features;    /* negotiated */
        int             vs_type;
        int             vs_nvqs;
        struct virtqueue vs_vq[VIRTIO_MAXQUEUES];
        void            *vs_child;
};

/* Transport operations, for drivers. */
uint64_t virtio_negotiate(struct virtio_softc *, uint64_t);
int virtio_has_feature(const struct virtio_softc *, int);
struct virtqueue *virtio_alloc_vq(struct virtio_softc *, int, int,
    void (*)(void *, uint32_t), const char *);
void virtio_ready(struct virtio_softc *);
void virtio_fail(struct virtio_softc *);
uint32_t virtio_cfg_read(struct virtio_softc *, int, int);
uint64_t virtio_cfg_read64(struct virtio_softc *, int);

/* Virtqueues (virtio.c). */
int vq_init(struct virtqueue *, int);
int vq_submit(struct virtqueue *, const struct vq_seg *, int, int, void *);
int vq_trysubmit(struct virtqueue *, const struct vq_seg *, int, int, void *);
int vq_intr(void *);
int vq_descriptors_needed(const struct vq_seg *, int);

/* Device drivers, by type. */
int vioblk_attach(struct virtio_softc *);
int vionet_attach(struct virtio_softc *);

#endif /* _DEV_VIRTIO_VIRTIOVAR_H_ */
