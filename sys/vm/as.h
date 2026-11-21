/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * User address spaces, after System V: a sorted list of segments over a
 * HAT.
 *
 * Locking: a_lock is an mrlock, the IRIX address space lock. Faults
 * take it for access, so the LWPs of a process fault in parallel;
 * mapping, unmapping, and protection changes take it for update. The
 * segment drivers serialise the pages of a segment among faults.
 */

#ifndef _VM_AS_H_
#define _VM_AS_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/synch.h>

#include <machine/types.h>

#include <vm/seg.h>

struct hat;

struct as {
        volatile uint32_t a_ref;        /* process and temporary owners */
        struct mrlock   a_lock;
        struct mutex    a_brklock;      /* heap extent, before a_lock */
        vaddr_t         a_brkbase;
        size_t          a_brksize;
        struct seg      *a_segs;        /* address order */
        struct hat      *a_hat;
        unsigned int    a_nsegs;
};

void as_init(void);
struct as *as_alloc(void);
void as_hold(struct as *);
void as_free(struct as *);
struct as *as_dup(struct as *);
int as_map(struct as *, vaddr_t, size_t, int (*)(struct seg *, void *),
    void *);
int as_unmap(struct as *, vaddr_t, size_t);
size_t as_size(struct as *);
int as_setprot(struct as *, vaddr_t, size_t, unsigned int);
int as_fault(struct as *, vaddr_t, enum seg_rw);
int as_gap(struct as *, size_t, vaddr_t, vaddr_t *);
int as_findgap(struct as *, size_t, vaddr_t, vaddr_t *);
int as_brk(struct as *, vaddr_t, size_t, size_t);
int as_map_anywhere(struct as *, size_t, vaddr_t,
    int (*)(struct seg *, void *), void *, vaddr_t *);
int as_map_topdown(struct as *, size_t, vaddr_t,
    int (*)(struct seg *, void *), void *, vaddr_t *);
struct seg *as_findseg(struct as *, vaddr_t);

#endif /* _VM_AS_H_ */
