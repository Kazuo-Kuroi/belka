/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Directory name lookup cache. See <sys/dnlc.h>.
 *
 * A fixed pool of entries on hash chains keyed by directory and name,
 * with one LRU list; a new entry takes the least recently used one.
 * Entries are soft references; a hit takes a real one with vn_get()
 * under dnlc_lock, and an entry whose target has gone is dropped.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/dnlc.h>
#include <sys/spinlock.h>
#include <sys/systm.h>
#include <sys/vfs.h>
#include <sys/vnode.h>

#define DNLC_HASHSIZE   256

struct ncache {
        struct ncache   *nc_hnext;      /* hash chain */
        struct ncache   *nc_hprev;
        struct ncache   *nc_lnext;      /* LRU, most recent at the head */
        struct ncache   *nc_lprev;
        struct vnode    *nc_dvp;        /* NULL if unused */
        struct vnode    *nc_vp;
        uint64_t        nc_dnum;        /* their v_numbers */
        uint64_t        nc_vnum;
        struct vfs      *nc_vfsp;       /* the directory's file system */
        uint8_t         nc_namlen;
        char            nc_name[NC_NAMLEN];
};

static struct ncache nc_pool[DNLC_SIZE];
static struct ncache *nc_hash[DNLC_HASHSIZE];
static struct ncache *nc_lru_head, *nc_lru_tail;
static struct spinlock dnlc_lock = SPINLOCK_INITIALIZER("dnlc");
static struct dnlc_stats dnlc_st;

static unsigned int
nc_hashfn(const struct vnode *dvp, const char *name, size_t len)
{
        uintptr_t h = (uintptr_t)dvp >> 4;

        for (size_t i = 0; i < len; i++)
                h = h * 31 + (unsigned char)name[i];
        return (unsigned int)(h % DNLC_HASHSIZE);
}

static void
nc_lru_unlink(struct ncache *nc)
{
        if (nc->nc_lprev != NULL)
                nc->nc_lprev->nc_lnext = nc->nc_lnext;
        else
                nc_lru_head = nc->nc_lnext;
        if (nc->nc_lnext != NULL)
                nc->nc_lnext->nc_lprev = nc->nc_lprev;
        else
                nc_lru_tail = nc->nc_lprev;
}

static void
nc_lru_push(struct ncache *nc, int head)
{
        if (head) {
                nc->nc_lprev = NULL;
                nc->nc_lnext = nc_lru_head;
                if (nc_lru_head != NULL)
                        nc_lru_head->nc_lprev = nc;
                else
                        nc_lru_tail = nc;
                nc_lru_head = nc;
        } else {
                nc->nc_lnext = NULL;
                nc->nc_lprev = nc_lru_tail;
                if (nc_lru_tail != NULL)
                        nc_lru_tail->nc_lnext = nc;
                else
                        nc_lru_head = nc;
                nc_lru_tail = nc;
        }
}

static void
nc_hash_unlink(struct ncache *nc)
{
        unsigned int h = nc_hashfn(nc->nc_dvp, nc->nc_name, nc->nc_namlen);

        if (nc->nc_hprev != NULL)
                nc->nc_hprev->nc_hnext = nc->nc_hnext;
        else
                nc_hash[h] = nc->nc_hnext;
        if (nc->nc_hnext != NULL)
                nc->nc_hnext->nc_hprev = nc->nc_hprev;
        nc->nc_hnext = nc->nc_hprev = NULL;
}

/* Retire an entry: off its hash chain, to the LRU tail for reuse. */
static void
nc_retire(struct ncache *nc)
{
        nc_hash_unlink(nc);
        nc->nc_dvp = nc->nc_vp = NULL;
        nc->nc_vfsp = NULL;
        nc_lru_unlink(nc);
        nc_lru_push(nc, 0);
        dnlc_st.ds_entries--;
        dnlc_st.ds_purges++;
}

static struct ncache *
nc_find(const struct vnode *dvp, const char *name, size_t len)
{
        for (struct ncache *nc = nc_hash[nc_hashfn(dvp, name, len)];
            nc != NULL; nc = nc->nc_hnext) {
                if (nc->nc_dvp == dvp && nc->nc_dnum == dvp->v_number &&
                    nc->nc_namlen == len &&
                    memcmp(nc->nc_name, name, len) == 0)
                        return nc;
        }
        return NULL;
}

void
dnlc_init(void)
{
        for (int i = 0; i < DNLC_SIZE; i++)
                nc_lru_push(&nc_pool[i], 0);
}

/* Return the cached vnode for name in dvp, held, or NULL. */
struct vnode *
dnlc_lookup(struct vnode *dvp, const char *name)
{
        size_t len = strlen(name);
        struct ncache *nc;
        struct vnode *vp = NULL;
        int s;

        if (len > NC_NAMLEN)
                return NULL;
        s = spin_lock(&dnlc_lock);
        if ((nc = nc_find(dvp, name, len)) != NULL) {
                if (vn_get(nc->nc_vp, nc->nc_vnum)) {
                        vp = nc->nc_vp;
                        nc_lru_unlink(nc);
                        nc_lru_push(nc, 1);
                        dnlc_st.ds_hits++;
                } else {
                        /* The vnode has gone or is going: forget it. */
                        nc_retire(nc);
                        dnlc_st.ds_stale++;
                        dnlc_st.ds_misses++;
                }
        } else {
                dnlc_st.ds_misses++;
        }
        spin_unlock(&dnlc_lock, s);
        return vp;
}

void
dnlc_enter(struct vnode *dvp, const char *name, struct vnode *vp)
{
        size_t len = strlen(name);
        struct ncache *nc;
        unsigned int h;
        int s;

        if (len > NC_NAMLEN || len == 0)
                return;
        s = spin_lock(&dnlc_lock);
        if ((nc = nc_find(dvp, name, len)) != NULL) {
                /* Refresh it: the old target may have been replaced. */
                nc->nc_vp = vp;
                nc->nc_vnum = vp->v_number;
                spin_unlock(&dnlc_lock, s);
                return;
        }
        nc = nc_lru_tail;
        if (nc->nc_dvp != NULL)
                nc_retire(nc);
        nc_lru_unlink(nc);
        nc->nc_dvp = dvp;
        nc->nc_vp = vp;
        nc->nc_dnum = dvp->v_number;
        nc->nc_vnum = vp->v_number;
        nc->nc_vfsp = dvp->v_vfsp;
        nc->nc_namlen = (uint8_t)len;
        memcpy(nc->nc_name, name, len);
        h = nc_hashfn(dvp, name, len);
        nc->nc_hprev = NULL;
        nc->nc_hnext = nc_hash[h];
        if (nc_hash[h] != NULL)
                nc_hash[h]->nc_hprev = nc;
        nc_hash[h] = nc;
        nc_lru_push(nc, 1);
        dnlc_st.ds_enters++;
        dnlc_st.ds_entries++;
        spin_unlock(&dnlc_lock, s);
}

void
dnlc_remove(struct vnode *dvp, const char *name)
{
        size_t len = strlen(name);
        struct ncache *nc;
        int s;

        if (len > NC_NAMLEN)
                return;
        s = spin_lock(&dnlc_lock);
        if ((nc = nc_find(dvp, name, len)) != NULL)
                nc_retire(nc);
        spin_unlock(&dnlc_lock, s);
}

/* Drop every entry naming vp (as directory or target) or in vfsp. */
static void
dnlc_purge(struct vnode *vp, struct vfs *vfsp)
{
        int s = spin_lock(&dnlc_lock);

        for (int i = 0; i < DNLC_SIZE; i++) {
                struct ncache *nc = &nc_pool[i];

                if (nc->nc_dvp == NULL)
                        continue;
                if ((vp != NULL && (nc->nc_dvp == vp || nc->nc_vp == vp)) ||
                    (vfsp != NULL && nc->nc_vfsp == vfsp))
                        nc_retire(nc);
        }
        spin_unlock(&dnlc_lock, s);
}

void
dnlc_purge_vp(struct vnode *vp)
{
        dnlc_purge(vp, NULL);
}

void
dnlc_purge_vfsp(struct vfs *vfsp)
{
        dnlc_purge(NULL, vfsp);
}

void
dnlc_get_stats(struct dnlc_stats *st)
{
        int s = spin_lock(&dnlc_lock);

        *st = dnlc_st;
        spin_unlock(&dnlc_lock, s);
}
