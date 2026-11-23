/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Split virtqueues. See <dev/virtio/virtiovar.h>.
 *
 * The descriptor table and the available ring share one page and the
 * used ring has another, both from the page allocator so that their
 * physical addresses are what the device is given. Free descriptors are
 * kept on a list threaded through their next fields.
 *
 * Ordering: the device must see a chain's descriptors before the
 * available index that publishes it, and the index before the notify
 * write; on the completion side the used index is read before the
 * entries it covers. amd64 orders ordinary stores and loads, and an
 * uncached store is not passed by earlier stores, so compiler barriers
 * suffice except before the notify, where a full barrier is used.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/kmem.h>
#include <sys/spinlock.h>
#include <sys/synch.h>
#include <sys/systm.h>

#include <machine/atomic.h>
#include <machine/param.h>
#include <machine/vmparam.h>

#include <vm/hat.h>
#include <vm/page.h>

#include "dev/virtio/virtiovar.h"

int
vq_init(struct virtqueue *vq, int size)
{
        struct page *ring, *used;

        if (size <= 0 || size > VIRTIO_MAXQSIZE)
                return -1;
        if ((ring = page_alloc(0, PA_ZERO)) == NULL ||
            (used = page_alloc(0, PA_ZERO)) == NULL)
                panic("vq_init: out of memory");

        spinlock_init(&vq->vq_lock, "virtqueue");
        sv_init(&vq->vq_space, "vq_space");
        vq->vq_size = (uint16_t)size;
        vq->vq_desc = page_to_kva(ring);
        vq->vq_avail = (volatile struct vring_avail *)(volatile void *)
            ((volatile char *)vq->vq_desc + 16 * size);
        vq->vq_used = page_to_kva(used);
        vq->vq_cookie = kmem_zalloc((size_t)size * sizeof(void *), KM_SLEEP);
        for (int i = 0; i < size; i++)
                vq->vq_desc[i].next = (uint16_t)(i + 1);
        vq->vq_free_head = 0;
        vq->vq_nfree = (uint16_t)size;
        vq->vq_last_used = 0;
        return 0;
}

/* Descriptors a request needs: one per page each segment touches. */
int
vq_descriptors_needed(const struct vq_seg *segs, int nsegs)
{
        int n = 0;

        for (int i = 0; i < nsegs; i++) {
                uintptr_t start = (uintptr_t)segs[i].vs_buf;
                uintptr_t end = start + segs[i].vs_len;

                if (segs[i].vs_len == 0)
                        continue;
                n += (int)((((end - 1) & ~(uintptr_t)PAGEOFFSET) -
                    (start & ~(uintptr_t)PAGEOFFSET)) / PAGESIZE) + 1;
        }
        return n;
}

/*
 * Queue a request of nout device-readable segments followed by nin
 * device-writable ones, waiting for free descriptors if wait (else
 * returning 1 if there are too few now), and notify the device. cookie
 * is passed to the completion callback. Returns 0, or -1 if the
 * request can never fit.
 */
static int
vq_submit1(struct virtqueue *vq, const struct vq_seg *segs, int nout,
    int nin, void *cookie, int wait)
{
        int need = vq_descriptors_needed(segs, nout + nin);
        uint16_t head, idx, prev = 0;
        int s;

        if (need == 0 || need > vq->vq_size)
                return -1;
        s = spin_lock(&vq->vq_lock);
        while (vq->vq_nfree < need) {
                if (!wait) {
                        spin_unlock(&vq->vq_lock, s);
                        return 1;
                }
                sv_wait(&vq->vq_space, &vq->vq_lock, s);
                s = spin_lock(&vq->vq_lock);
        }

        head = idx = vq->vq_free_head;
        for (int i = 0; i < nout + nin; i++) {
                uintptr_t va = (uintptr_t)segs[i].vs_buf;
                size_t left = segs[i].vs_len;
                uint16_t flags = i >= nout ? VRING_DESC_F_WRITE : 0;

                while (left > 0) {
                        size_t off = va & PAGEOFFSET;
                        size_t chunk = PAGESIZE - off < left ?
                            PAGESIZE - off : left;
                        volatile struct vring_desc *d = &vq->vq_desc[idx];
                        paddr_t pa;

                        if (hat_kextract(va, &pa) != 0)
                                panic("vq_submit: buffer %p is not mapped",
                                    (void *)va);
                        d->addr = pa;
                        d->len = (uint32_t)chunk;
                        d->flags = flags | VRING_DESC_F_NEXT;
                        prev = idx;
                        idx = d->next;
                        va += chunk;
                        left -= chunk;
                }
        }
        vq->vq_desc[prev].flags &= (uint16_t)~VRING_DESC_F_NEXT;
        vq->vq_free_head = idx;
        vq->vq_nfree = (uint16_t)(vq->vq_nfree - need);
        vq->vq_cookie[head] = cookie;

        vq->vq_avail->ring[vq->vq_avail->idx % vq->vq_size] = head;
        membar_producer();
        vq->vq_avail->idx = (uint16_t)(vq->vq_avail->idx + 1);
        membar_sync();
        *vq->vq_notify = vq->vq_index;
        spin_unlock(&vq->vq_lock, s);
        return 0;
}

/* Thread context only: may wait for descriptors. */
int
vq_submit(struct virtqueue *vq, const struct vq_seg *segs, int nout,
    int nin, void *cookie)
{
        return vq_submit1(vq, segs, nout, nin, cookie, 1);
}

/* Never waits (for STREAMS procedures): 1 if the ring is full now. */
int
vq_trysubmit(struct virtqueue *vq, const struct vq_seg *segs, int nout,
    int nin, void *cookie)
{
        return vq_submit1(vq, segs, nout, nin, cookie, 0);
}

/*
 * Collect completed requests: return each chain to the free list and
 * call the completion callback. The queue's interrupt handler; returns
 * nonzero if anything had completed.
 */
int
vq_intr(void *arg)
{
        struct virtqueue *vq = arg;
        int claimed = 0;
        int s = spin_lock(&vq->vq_lock);

        while (vq->vq_last_used != vq->vq_used->idx) {
                volatile struct vring_used_elem *e;
                uint16_t id, last, n = 1;
                uint32_t len;
                void *cookie;

                membar_consumer();
                e = &vq->vq_used->ring[vq->vq_last_used % vq->vq_size];
                id = (uint16_t)e->id;
                len = e->len;
                cookie = vq->vq_cookie[id];
                vq->vq_cookie[id] = NULL;

                for (last = id; (vq->vq_desc[last].flags &
                    VRING_DESC_F_NEXT) != 0; last = vq->vq_desc[last].next)
                        n++;
                vq->vq_desc[last].next = vq->vq_free_head;
                vq->vq_free_head = id;
                vq->vq_nfree = (uint16_t)(vq->vq_nfree + n);
                vq->vq_last_used++;
                vq->vq_completed++;
                claimed = 1;
                if (vq->vq_done != NULL)
                        vq->vq_done(cookie, len);
        }
        if (claimed)
                (void)sv_broadcast(&vq->vq_space);
        spin_unlock(&vq->vq_lock, s);
        return claimed;
}
