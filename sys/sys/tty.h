/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-13
 * SPDX-License-Identifier: MIT
 */

/*
 * Terminals: the line discipline between a character device and the
 * programs that read and write it (see kern/tty.c).
 *
 * A driver fills in t_output and calls tty_init(); it passes received
 * characters to tty_input(), from its interrupt thread, and routes
 * read, write, ioctl, and poll on its vnode to tty_read(), tty_write(),
 * tty_ioctl(), and tty_poll().
 *
 * t_lock, a mutex, protects everything below it.
 */

#ifndef _SYS_TTY_H_
#define _SYS_TTY_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/poll.h>
#include <sys/synch.h>
#include <sys/termios.h>
#include <sys/types.h>

#define TTY_INQ         1024    /* characters waiting to be read */
#define TTY_LINE        256     /* the line being edited (MAX_CANON) */

/* Input queue entries: a character and these flags. */
#define TQ_DELIM        0x100   /* ends a line (canonical mode) */
#define TQ_EOF          0x200   /* end of file: ends a line, is not read */

struct tty {
        struct mutex    t_lock;
        struct sv       t_insv;         /* input came, or modes changed */
        struct sv       t_outsv;        /* output was restarted */
        struct pollhead t_ph;
        struct termios  t_termios;
        struct winsize  t_winsize;
        uint16_t        t_inq[TTY_INQ]; /* ring of entries */
        size_t          t_inhead;
        size_t          t_incount;
        size_t          t_lines;        /* TQ_DELIM entries in t_inq */
        char            t_line[TTY_LINE];
        size_t          t_linelen;
        int             t_stopped;      /* output stopped (VSTOP) */
        int             t_lnext;        /* the next character is literal */
        pid_t           t_pgrp;         /* foreground process group */
        pid_t           t_sid;          /* its session */

        /* The device: write n characters as they are. */
        void            (*t_output)(struct tty *, const char *, size_t);
        void            *t_softc;
};

struct uio;
struct cred;

void    tty_init(struct tty *);
void    tty_input(struct tty *, int);
int     tty_read(struct tty *, struct uio *, int);
int     tty_write(struct tty *, struct uio *, int);
int     tty_ioctl(struct tty *, unsigned long, uint64_t, int, struct cred *);
int     tty_poll(struct tty *, short, short *, struct pollhead **);

#endif /* _SYS_TTY_H_ */
