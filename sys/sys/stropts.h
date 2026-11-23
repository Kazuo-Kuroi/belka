/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

/*
 * The user's interface to a stream: the stream head's ioctl commands
 * and their structures, getmsg and putmsg's buffers and flags. The
 * numbers are System V's.
 */

#ifndef _SYS_STROPTS_H_
#define _SYS_STROPTS_H_

#include <stdint.h>

#define STR             ('S' << 8)
#define I_NREAD         (STR | 001)
#define I_PUSH          (STR | 002)
#define I_POP           (STR | 003)
#define I_LOOK          (STR | 004)
#define I_FLUSH         (STR | 005)
#define I_SRDOPT        (STR | 006)
#define I_GRDOPT        (STR | 007)
#define I_STR           (STR | 010)
#define I_SETSIG        (STR | 011)
#define I_GETSIG        (STR | 012)
#define I_FIND          (STR | 013)
#define I_LINK          (STR | 014)
#define I_UNLINK        (STR | 015)
#define I_RECVFD        (STR | 016)
#define I_PEEK          (STR | 017)
#define I_FDINSERT      (STR | 020)
#define I_SENDFD        (STR | 021)
#define I_SWROPT        (STR | 023)
#define I_GWROPT        (STR | 024)
#define I_LIST          (STR | 025)
#define I_PLINK         (STR | 026)
#define I_PUNLINK       (STR | 027)
#define I_FLUSHBAND     (STR | 034)
#define I_CKBAND        (STR | 035)
#define I_GETBAND       (STR | 036)
#define I_ATMARK        (STR | 037)
#define I_SETCLTIME     (STR | 040)
#define I_GETCLTIME     (STR | 041)
#define I_CANPUT        (STR | 042)

#define FMNAMESZ        8       /* a module name's longest */

/* I_SETSIG's events. */
#define S_INPUT         0x0001
#define S_HIPRI         0x0002
#define S_OUTPUT        0x0004
#define S_MSG           0x0008
#define S_ERROR         0x0010
#define S_HANGUP        0x0020
#define S_RDNORM        0x0040
#define S_WRNORM        S_OUTPUT
#define S_RDBAND        0x0080
#define S_WRBAND        0x0100
#define S_BANDURG       0x0200

/* I_FLUSH. */
#ifndef FLUSHR
#define FLUSHR          0x01
#define FLUSHW          0x02
#define FLUSHRW         0x03
#define FLUSHBAND       0x04
#endif

/* I_SRDOPT and I_GRDOPT. */
#define RNORM           0x0000  /* byte stream */
#define RMSGD           0x0001  /* message discard */
#define RMSGN           0x0002  /* message non-discard */
#define RMODEMASK       0x0003
#define RPROTDAT        0x0004  /* control part read as data */
#define RPROTDIS        0x0008  /* control part discarded */
#define RPROTNORM       0x0010  /* control part fails the read */
#define RPROTMASK       0x001c

/* I_SWROPT and I_GWROPT. */
#define SNDZERO         0x0001  /* send zero-length writes */
#define SNDPIPE         0x0002  /* SIGPIPE on a write after an error */

/* I_ATMARK. */
#define ANYMARK         0x01
#define LASTMARK        0x02

/* getmsg and putmsg's flags. */
#define RS_HIPRI        0x01
#define MSG_HIPRI       0x01
#define MSG_ANY         0x02
#define MSG_BAND        0x04

/* getmsg's results beside 0: what was left at the head. */
#define MORECTL         1
#define MOREDATA        2

#define INFTIM          (-1)    /* I_STR waits as long as it takes */

struct strbuf {
        int             maxlen;
        int             len;
        char            *buf;
};

struct strioctl {
        int             ic_cmd;
        int             ic_timout;      /* seconds; INFTIM: none */
        int             ic_len;
        char            *ic_dp;
};

struct strpeek {
        struct strbuf   ctlbuf;
        struct strbuf   databuf;
        uint32_t        flags;
};

struct bandinfo {
        unsigned char   bi_pri;
        int             bi_flag;
};

struct str_mlist {
        char            l_name[FMNAMESZ + 1];
};

struct str_list {
        int             sl_nmods;
        struct str_mlist *sl_modlist;
};

#endif /* _SYS_STROPTS_H_ */
