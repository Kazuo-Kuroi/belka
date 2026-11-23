/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

/*
 * STREAMS for modules and drivers: messages, queues, and the DDI
 * routines that move messages between them (see doc/design/decisions.md,
 * "The STREAMS framework"). The names are System V's; the layouts are
 * Belka's own, past the fields the DDI names.
 */

#ifndef _SYS_STREAM_H_
#define _SYS_STREAM_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/spinlock.h>
#include <sys/types.h>

struct cred;
struct proc;
struct stdata;
struct syncq;
struct vnode;

/* --- Messages. --- */

typedef struct free_rtn {
        void            (*free_func)(void *);
        void            *free_arg;
} frtn_t;

typedef struct datab {
        unsigned char   *db_base;       /* the buffer */
        unsigned char   *db_lim;        /* just past it */
        volatile uint32_t db_ref;       /* message blocks sharing it */
        unsigned char   db_type;        /* M_DATA, ... */
        unsigned char   db_flags;       /* DBF_* (Belka's) */
        size_t          db_alloc;       /* bytes allocated with the dblk */
        frtn_t          *db_frtnp;      /* esballoc's free routine */
} dblk_t;

#define DBF_EXT         0x01    /* buffer is the caller's (esballoc) */

typedef struct msgb {
        struct msgb     *b_next;        /* next message on a queue */
        struct msgb     *b_prev;
        struct msgb     *b_cont;        /* next block of this message */
        unsigned char   *b_rptr;        /* first unread byte */
        unsigned char   *b_wptr;        /* first unwritten byte */
        struct datab    *b_datap;
        unsigned char   b_band;         /* priority band */
        unsigned short  b_flag;         /* MSG* */
        struct queue    *b_queue;       /* deferred put's target (Belka's) */
} mblk_t;

/* b_flag. */
#define MSGMARK         0x01    /* last byte of the message is marked */
#define MSGNOLOOP       0x02
#define MSGDELIM        0x04    /* message is delimited */

/* Message types; QPCTL and above are high priority. */
#define M_DATA          0x00
#define M_PROTO         0x01
#define M_BREAK         0x08
#define M_PASSFP        0x09
#define M_EVENT         0x0a
#define M_SIG           0x0b
#define M_DELAY         0x0c
#define M_CTL           0x0d
#define M_IOCTL         0x0e
#define M_SETOPTS       0x10
#define M_RSE           0x11
#define QPCTL           0x80
#define M_IOCACK        0x81
#define M_IOCNAK        0x82
#define M_PCPROTO       0x83
#define M_PCSIG         0x84
#define M_READ          0x85
#define M_FLUSH         0x86
#define M_STOP          0x87
#define M_START         0x88
#define M_HANGUP        0x89
#define M_ERROR         0x8a
#define M_COPYIN        0x8b
#define M_COPYOUT       0x8c
#define M_IOCDATA       0x8d
#define M_PCRSE         0x8e
#define M_STOPI         0x8f
#define M_STARTI        0x90
#define M_PCEVENT       0x91
#define M_UNHANGUP      0x92

/* allocb priorities, kept for the DDI; Belka does not use them. */
#define BPRI_LO         1
#define BPRI_MED        2
#define BPRI_HI         3

#define INFPSZ          (-1)    /* no maximum packet size */

/* M_FLUSH's first byte, and I_FLUSH's argument (<sys/stropts.h>). */
#ifndef FLUSHR
#define FLUSHR          0x01
#define FLUSHW          0x02
#define FLUSHRW         0x03
#define FLUSHBAND       0x04
#endif

/* flushq's flag. */
#define FLUSHALL        1       /* every message */
#define FLUSHDATA       0       /* data, protocol and delay messages only */

/* M_ERROR's bytes, when an error is to be cleared. */
#define NOERROR         ((unsigned char)-1)

/* M_IOCTL, M_IOCACK, M_IOCNAK. */
struct iocblk {
        int             ioc_cmd;
        struct cred     *ioc_cr;
        unsigned int    ioc_id;         /* matches the reply to the request */
        size_t          ioc_count;      /* bytes in b_cont, or TRANSPARENT */
        int             ioc_error;
        int             ioc_rval;
        int             ioc_flag;
};

#define TRANSPARENT     ((size_t)-1)

/* I_LINK, I_PLINK, I_UNLINK, I_PUNLINK: the M_IOCTL's data. */
struct linkblk {
        struct queue    *l_qtop;        /* driver's upper write queue, or NULL */
        struct queue    *l_qbot;        /* its write queue atop the lower */
        int             l_index;        /* the link's multiplexor id */
};

/* M_COPYIN and M_COPYOUT. */
struct copyreq {
        int             cq_cmd;
        struct cred     *cq_cr;
        unsigned int    cq_id;
        void            *cq_addr;       /* user address */
        size_t          cq_size;
        int             cq_flag;
        mblk_t          *cq_private;
};

/* M_IOCDATA. */
struct copyresp {
        int             cp_cmd;
        struct cred     *cp_cr;
        unsigned int    cp_id;
        void            *cp_rval;       /* 0: done; else the errno */
        size_t          cp_pad1;
        int             cp_pad2;
        mblk_t          *cp_private;
};

/* M_SETOPTS. */
struct stroptions {
        unsigned int    so_flags;       /* SO_* */
        short           so_readopt;
        unsigned short  so_wroff;
        ssize_t         so_minpsz;
        ssize_t         so_maxpsz;
        size_t          so_hiwat;
        size_t          so_lowat;
        unsigned char   so_band;
        unsigned short  so_erropt;
};

#define SO_ALL          0x003f
#define SO_READOPT      0x0001
#define SO_WROFF        0x0002
#define SO_MINPSZ       0x0004
#define SO_MAXPSZ       0x0008
#define SO_HIWAT        0x0010
#define SO_LOWAT        0x0020
#define SO_MREADON      0x0040
#define SO_MREADOFF     0x0080
#define SO_NDELON       0x0100
#define SO_NDELOFF      0x0200
#define SO_ISTTY        0x0400
#define SO_ISNTTY       0x0800
#define SO_TOSTOP       0x1000
#define SO_TONSTOP      0x2000
#define SO_BAND         0x4000
#define SO_DELIM        0x8000
#define SO_NODELIM      0x010000
#define SO_STRHOLD      0x020000
#define SO_ERROPT       0x040000

/* --- Queues. --- */

typedef struct queue queue_t;

struct module_info {
        unsigned short  mi_idnum;
        const char      *mi_idname;
        ssize_t         mi_minpsz;
        ssize_t         mi_maxpsz;
        size_t          mi_hiwat;
        size_t          mi_lowat;
};

struct qinit {
        int     (*qi_putp)(queue_t *, mblk_t *);
        int     (*qi_srvp)(queue_t *);
        int     (*qi_qopen)(queue_t *, dev_t *, int, int, struct cred *);
        int     (*qi_qclose)(queue_t *, int, struct cred *);
        int     (*qi_qadmin)(void);
        struct module_info *qi_minfo;
        void    *qi_mstat;
};

struct streamtab {
        struct qinit    *st_rdinit;
        struct qinit    *st_wrinit;
        struct qinit    *st_muxrinit;
        struct qinit    *st_muxwinit;
};

/* The flow control of one band above zero. */
typedef struct qband {
        struct qband    *qb_next;
        size_t          qb_count;
        mblk_t          *qb_first;
        mblk_t          *qb_last;
        size_t          qb_hiwat;
        size_t          qb_lowat;
        unsigned int    qb_flag;
} qband_t;

#define QB_FULL         0x01
#define QB_WANTW        0x02
#define QB_BACK         0x04

struct queue {
        struct qinit    *q_qinfo;
        mblk_t          *q_first;
        mblk_t          *q_last;
        queue_t         *q_next;        /* the next queue on the stream */
        queue_t         *q_link;
        void            *q_ptr;         /* the module's own */
        size_t          q_count;        /* bytes in band 0 and high priority */
        unsigned int    q_flag;
        ssize_t         q_minpsz;
        ssize_t         q_maxpsz;
        size_t          q_hiwat;
        size_t          q_lowat;
        qband_t         *q_bandp;
        unsigned char   q_nband;

        /* Belka's own. */
        struct spinlock q_lock;         /* messages, counts and flags */
        queue_t         *q_back;        /* the queue whose q_next this is */
        struct syncq    *q_syncq;
        struct stdata   *q_stream;
        queue_t         *q_svnext;      /* syncq's list of enabled queues */
};

/* q_flag. */
#define QENAB           0x0001  /* on its syncq's service list */
#define QWANTR          0x0002  /* a reader found it empty */
#define QWANTW          0x0004  /* a writer found it full */
#define QFULL           0x0008
#define QREADR          0x0010  /* the read queue of its pair */
#define QUSE            0x0020
#define QNOENB          0x0040  /* putq does not enable it */
#define QBACK           0x0100  /* back-enabled */
#define QOFF            0x0200  /* procedures off: closing (Belka's) */
#define QISHEAD         0x0400  /* the stream head's pair (Belka's) */

/* strqget and strqset. */
typedef enum qfields {
        QHIWAT, QLOWAT, QMAXPSZ, QMINPSZ, QCOUNT, QFIRST, QLAST, QFLAG,
        QBAD
} qfields_t;

/* Open's sflag. */
#define DRVOPEN         0
#define MODOPEN         1
#define CLONEOPEN       2

#define OPENFAIL        (-1)

/* Syncq levels: how a module's instances are serialised. */
#define SQLVL_QPAIR     0       /* one syncq per queue pair */
#define SQLVL_MODULE    1       /* one for every instance of the module */

#define RD(q)           (((q)->q_flag & QREADR) != 0 ? (q) : (q) - 1)
#define WR(q)           (((q)->q_flag & QREADR) != 0 ? (q) + 1 : (q))
#define OTHERQ(q)       (((q)->q_flag & QREADR) != 0 ? (q) + 1 : (q) - 1)
#define SAMESTR(q)      ((q)->q_next != NULL && \
                            (((q)->q_flag ^ (q)->q_next->q_flag) & QREADR) == 0)

#define datamsg(t)      ((t) == M_DATA || (t) == M_PROTO || \
                            (t) == M_PCPROTO || (t) == M_DELAY)
#define MBLKL(mp)       ((size_t)((mp)->b_wptr - (mp)->b_rptr))
#define MBLKSIZE(mp)    ((size_t)((mp)->b_datap->db_lim - \
                            (mp)->b_datap->db_base))
#define MBLKHEAD(mp)    ((size_t)((mp)->b_rptr - (mp)->b_datap->db_base))
#define MBLKTAIL(mp)    ((size_t)((mp)->b_datap->db_lim - (mp)->b_wptr))
#define DB_TYPE(mp)     ((mp)->b_datap->db_type)

/* Messages (kern/str_msg.c). */
mblk_t  *allocb(size_t, unsigned int);
mblk_t  *allocb_wait(size_t);
mblk_t  *esballoc(unsigned char *, size_t, unsigned int, frtn_t *);
void    freeb(mblk_t *);
void    freemsg(mblk_t *);
mblk_t  *dupb(mblk_t *);
mblk_t  *dupmsg(mblk_t *);
mblk_t  *copyb(mblk_t *);
mblk_t  *copymsg(mblk_t *);
size_t  msgdsize(const mblk_t *);
size_t  msgsize(const mblk_t *);
int     pullupmsg(mblk_t *, ssize_t);
mblk_t  *msgpullup(mblk_t *, ssize_t);
int     adjmsg(mblk_t *, ssize_t);
void    linkb(mblk_t *, mblk_t *);
mblk_t  *unlinkb(mblk_t *);
mblk_t  *rmvb(mblk_t *, mblk_t *);
int     testb(size_t, unsigned int);
void    str_msginit(void);

/* Queues (kern/str_queue.c). */
int     put(queue_t *, mblk_t *);
int     putnext(queue_t *, mblk_t *);
void    qreply(queue_t *, mblk_t *);
int     putq(queue_t *, mblk_t *);
int     putbq(queue_t *, mblk_t *);
mblk_t  *getq(queue_t *);
int     insq(queue_t *, mblk_t *, mblk_t *);
void    rmvq(queue_t *, mblk_t *);
void    flushq(queue_t *, int);
void    flushband(queue_t *, unsigned char, int);
void    qenable(queue_t *);
void    noenable(queue_t *);
void    enableok(queue_t *);
int     canput(queue_t *);
int     canputnext(queue_t *);
int     bcanput(queue_t *, unsigned char);
int     bcanputnext(queue_t *, unsigned char);
int     qsize(queue_t *);
int     putctl(queue_t *, int);
int     putctl1(queue_t *, int, int);
int     putnextctl(queue_t *, int);
int     putnextctl1(queue_t *, int, int);
int     strqget(queue_t *, qfields_t, unsigned char, intptr_t *);
int     strqset(queue_t *, qfields_t, unsigned char, intptr_t);
void    qprocson(queue_t *);
void    qprocsoff(queue_t *);
int64_t qtimeout(queue_t *, void (*)(void *), void *, long);
void    quntimeout(queue_t *, int64_t);
int64_t qbufcall(queue_t *, size_t, unsigned int, void (*)(void *), void *);
void    qunbufcall(queue_t *, int64_t);
void    miocack(queue_t *, mblk_t *, size_t, int);
void    miocnak(queue_t *, mblk_t *, size_t, int);
void    flushq_reply(queue_t *, mblk_t *);

#endif /* _SYS_STREAM_H_ */
