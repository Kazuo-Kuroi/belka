/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-22
 * SPDX-License-Identifier: MIT
 */

/*
 * STREAMS internals: the stream head, syncqs, and the tables of
 * modules and drivers. Not for modules and drivers themselves, which
 * use <sys/stream.h>.
 */

#ifndef _SYS_STRSUBR_H_
#define _SYS_STRSUBR_H_

#include <stdint.h>

#include <sys/mkdev.h>
#include <sys/poll.h>
#include <sys/specvn.h>
#include <sys/spinlock.h>
#include <sys/stream.h>
#include <sys/synch.h>
#include <sys/vnode.h>

struct kthread;
struct strcb;

/*
 * A syncq: what serialises a module's procedures (see str_queue.c).
 * Work that finds it claimed waits on it: deferred puts (chained by
 * b_next, with their target in b_queue), enabled queues, and callbacks.
 */
struct syncq {
        struct spinlock sq_lock;
        unsigned int    sq_flags;
        struct kthread  *sq_owner;
        mblk_t          *sq_puthead;
        mblk_t          *sq_puttail;
        queue_t         *sq_svhead;
        queue_t         *sq_svtail;
        struct strcb    *sq_cbhead;
        struct strcb    *sq_cbtail;
        struct syncq    *sq_runnext;    /* the service threads' list */
        struct sv       sq_sv;          /* claimants that wait */
        unsigned int    sq_refs;        /* queue pairs using it */
};

#define SQ_BUSY         0x01    /* claimed */
#define SQ_QUEUED       0x02    /* on the service threads' list */
#define SQ_WANTED       0x04    /* a thread waits to claim it */

/* A module or a driver, as the tables give them. */
struct fmodsw {
        const char      *f_name;
        struct streamtab *f_str;
        int             f_sqlvl;        /* SQLVL_* */
        struct syncq    *f_syncq;       /* SQLVL_MODULE's one */
};

struct strdev {
        char            d_name[16];     /* under /dev */
        struct streamtab *d_str;
        int             d_sqlvl;
        unsigned int    d_major;
        mode_t          d_mode;
        struct syncq    *d_syncq;
        volatile uint32_t d_nextminor;
        unsigned int    d_unit;         /* in the top 8 bits of each minor */
        void            *d_private;     /* the driver's, for its open */
};

#define STRDEV_MAX      32
#define STR_UNIT(dev)   (minor(dev) >> 24)

#define NSTRPUSH        9       /* modules on one stream */

/* The stream head. */
struct stdata {
        struct mutex    sd_lock;        /* the head's state below */
        struct strsig   *sd_signals;    /* I_SETSIG registrations */
        queue_t         *sd_rq;         /* the head's read queue */
        queue_t         *sd_wq;
        struct spinlock sd_qlock;       /* q_next, q_back walks */
        struct mutex    sd_plumb;       /* push, pop, close */
        struct mutex    sd_rdlock;      /* one reader at a time */
        struct mutex    sd_iocwait;     /* one ioctl at a time */
        struct strdev   *sd_dev;
        struct vnode    sd_vnode;
        struct specattr sd_attr;
        struct pollhead sd_poll;
        struct sv       sd_readsv;
        struct sv       sd_writesv;
        unsigned int    sd_flag;        /* STR* */
        unsigned int    sd_wgen;        /* back-enables of the head */
        int             sd_rerror;
        int             sd_werror;
        int             sd_rdopt;
        int             sd_wropt;
        int             sd_pushcnt;
        int             sd_closetime;   /* milliseconds */
        int             sd_closeflag;   /* the file flags of the last close */
        unsigned short  sd_wroff;
        ssize_t         sd_minpsz;      /* of the queue below the head */
        ssize_t         sd_maxpsz;

        /* The ioctl in progress, under sd_iocspin. */
        struct spinlock sd_iocspin;
        struct sv       sd_iocsv;
        unsigned int    sd_iocid;
        mblk_t          *sd_iocblk;     /* its reply */
        int             sd_ioctimeout;
        int             sd_iocpending;
};

/* sd_flag. */
#define STRHUP          0x0001  /* hung up */
#define STRDERR         0x0002  /* read error */
#define STWRERR         0x0004  /* write error */
#define STRCLOSE        0x0008
#define STRMREAD        0x0010  /* send M_READ before waiting */
#define STRDELIM        0x0020  /* mark messages delimited */
#define STRTOSTOP       0x0040
#define STPLEX          0x0080  /* linked under a multiplexor */

/* Stream heads (kern/strhead.c). */
void    strhead_init(void);
int     str_clone(struct strdev *, struct vnode **, int, struct cred *);
int     str_kopen(const char *, int, struct cred *, struct vnode **);
int     str_kioctl(struct stdata *, int, const void *, size_t, struct cred *);
int     str_kputmsg(struct stdata *, mblk_t *, int);
int     str_kgetmsg(struct stdata *, int, int, int, mblk_t **);
int     str_kwaitwrite(struct stdata *, int);
int     str_ksend(struct stdata *, mblk_t *);
void    str_kunget(struct stdata *, mblk_t *);
int     strsig_ioctl(struct proc *, int, unsigned long, uint64_t);
void    strsig_close(struct stdata *, struct proc *);
int     sys_getmsg(const uint64_t *, uint64_t *);
int     sys_putmsg(const uint64_t *, uint64_t *);
int     sys_getpmsg(const uint64_t *, uint64_t *);
int     sys_putpmsg(const uint64_t *, uint64_t *);
int     strioctl(struct vnode *, unsigned long, uint64_t, int, struct cred *,
            uint64_t *);

/* Syncqs and the service threads (kern/str_queue.c). */
struct syncq *sq_alloc(void);
void    sq_hold(struct syncq *);
void    sq_rele(struct syncq *);
void    sq_enter(struct syncq *);
void    sq_exit(struct syncq *);
void    sq_purge(struct syncq *, queue_t *);
void    sq_claim_set(struct syncq **, int);
void    sq_exit_set(struct syncq **, int);
queue_t *allocq(void);
void    freeq(queue_t *);
void    setq(queue_t *, struct qinit *, struct qinit *, struct syncq *,
            struct stdata *);
void    str_init(void);
void    str_backenable(queue_t *);

/* The tables (kern/str_conf.c). */
struct fmodsw *fmod_find(const char *);
struct strdev *strdev_find(const char *);
extern struct strdev strdevsw[];
extern int nstrdev;
struct strdev *strdev_register(const char *, struct streamtab *, int,
            unsigned int, unsigned int, mode_t, void *);

#endif /* _SYS_STRSUBR_H_ */
