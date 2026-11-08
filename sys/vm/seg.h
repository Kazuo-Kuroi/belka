/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Segments: contiguous page-aligned ranges of an address space, each
 * managed by a segment driver, after System V.
 *
 * The address space lock, held for update, covers every change to the
 * segment list and to a segment's range; faults hold it for access.
 */

#ifndef _VM_SEG_H_
#define _VM_SEG_H_

#include <stddef.h>

#include <machine/types.h>

struct as;
struct seg;

/* Access that caused a fault. */
enum seg_rw { S_READ, S_WRITE, S_EXEC };

/* Fault results. */
#define FC_OK           0
#define FC_NOMAP        1       /* nothing is mapped there */
#define FC_PROT         2       /* the access is not allowed */
#define FC_OBJERR       3       /* the backing object failed */
#define FC_NOMEM        4       /* no memory for a page */

struct seg_ops {
        /* Set up newseg, in a new address space, as fork's copy of seg. */
        int     (*so_dup)(struct seg *, struct seg *);
        /* Unmap part of seg, within it; may shrink, split, or free it. */
        int     (*so_unmap)(struct seg *, vaddr_t, size_t);
        /* Tear seg down entirely. */
        void    (*so_free)(struct seg *);
        int     (*so_fault)(struct seg *, vaddr_t, enum seg_rw);
        /* Change the protection of all of seg. */
        int     (*so_setprot)(struct seg *, unsigned int);
        /* Split seg at addr; the new segment covers addr to the end. */
        int     (*so_split)(struct seg *, vaddr_t);
};

struct seg {
        vaddr_t         s_base;
        size_t          s_size;
        struct as       *s_as;
        struct seg      *s_next;        /* address order */
        const struct seg_ops *s_ops;
        void            *s_data;
};

struct seg *seg_alloc(struct as *, vaddr_t, size_t);
void seg_free(struct seg *);

#endif /* _VM_SEG_H_ */
