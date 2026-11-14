/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-13
 * SPDX-License-Identifier: MIT
 */

/*
 * The terminal line discipline, with System V's termios modes.
 *
 * Input: tty_input() takes each received character through c_iflag
 * (ISTRIP, IGNCR, ICRNL, INLCR, IXON and IXANY), then the signal
 * characters (ISIG: VINTR, VQUIT, VSUSP send SIGINT, SIGQUIT, SIGTSTP to
 * the foreground process group and flush input unless NOFLSH), then:
 * - In canonical mode (ICANON) into the line being edited, with
 *   VERASE, VKILL, and with IEXTEN VWERASE and VLNEXT. A newline, VEOL,
 *   or VEOL2 ends the line, which joins the input queue whole; VEOF ends
 *   it without being read itself, and on an empty line reads as end of
 *   file. A read returns at most one line.
 * - Otherwise straight into the input queue, and reads follow VMIN and
 *   VTIME (tenths of a second): VMIN characters, or what came before
 *   VTIME passed since the read (VMIN 0) or since the last character.
 * Echo (ECHO, and ECHOE, ECHOK, ECHOKE, ECHONL, ECHOCTL) goes out
 * through output processing, as written characters do.
 *
 * Output: with OPOST, ONLCR, OCRNL, ONOCR, ONLRET, and OLCUC; output
 * stopped by VSTOP or TCXONC waits for VSTART or TCXONC again. There
 * are no fill or delay characters.
 *
 * Job control (POSIX): a session leader with no controlling terminal
 * that opens one belonging to no session, without O_NOCTTY, makes it
 * its own (tty_open), or takes it with TIOCSCTTY; TIOCSPGRP sets the
 * foreground group, one of the session's. A process of a background
 * group of that session that reads it gets SIGTTIN, and one that writes
 * with TOSTOP or changes the modes SIGTTOU, sent to its group, and the
 * call is made again once it continues; an orphaned group gets EIO
 * instead, as does a read with SIGTTIN held or ignored. VSUSP does not
 * stop an orphaned group. When the leader exits (tty_session_end) or
 * the line hangs up (tty_hangup), the foreground group gets SIGHUP and
 * SIGCONT and the session loses the terminal; after a hang-up reads
 * see end of file and writes fail with EIO. TIOCSTI puts a character on
 * the input as if received.
 */

#include <stddef.h>
#include <stdint.h>

#include <sys/cred.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/signal.h>
#include <sys/systm.h>
#include <sys/tty.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#define CTRL(c)         ((c) & 037)
#define DEL             0177
#define TTY_CHUNK       128

static int tty_cc(const struct tty *, int, int);
static void tty_out(struct tty *, const char *, size_t);
static void tty_echo(struct tty *, int);
static void tty_erase1(struct tty *);
static int tty_enqueue(struct tty *, uint16_t);
static int tty_store(struct tty *, int);
static void tty_flush_input(struct tty *);
static void tty_end_line(struct tty *, int);
static size_t tty_readable(const struct tty *);
static int tty_sleep(struct tty *, struct sv *);
static void tty_signal(struct tty *, int);
static int tty_setmodes(struct tty *, const struct termios *, int);
static int tty_bgcheck(struct tty *, int);
static int tty_outroom(struct tty *);

/*
 * Every terminal, for the controlling terminal of a session. The first
 * tty_init(), the console's at start-up, sets the lock up.
 */
static struct mutex tty_list_lock;
static int tty_list_ready;
static struct tty *tty_list;

/* Modes a new terminal starts with; it joins the list of terminals. */
void
tty_init(struct tty *tp)
{
        struct termios *t = &tp->t_termios;

        memset(tp, 0, sizeof(*tp));
        mutex_init(&tp->t_lock, "tty");
        sv_init(&tp->t_insv, "ttyin");
        sv_init(&tp->t_outsv, "ttyout");
        sv_init(&tp->t_roomsv, "ttyroom");
        pollhead_init(&tp->t_ph);
        if (!tty_list_ready) {
                mutex_init(&tty_list_lock, "ttylist");
                tty_list_ready = 1;
        }
        mutex_lock(&tty_list_lock);
        tp->t_next = tty_list;
        tty_list = tp;
        mutex_unlock(&tty_list_lock);
        memset(t, 0, sizeof(*t));
        t->c_iflag = BRKINT | ICRNL | IXON | IMAXBEL;
        t->c_oflag = OPOST | ONLCR;
        t->c_cflag = B38400 | CS8 | CREAD | HUPCL;
        t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN |
            ECHOCTL | ECHOKE;
        t->c_cc[VINTR] = CTRL('c');
        t->c_cc[VQUIT] = CTRL('\\');
        t->c_cc[VERASE] = DEL;
        t->c_cc[VKILL] = CTRL('u');
        t->c_cc[VEOF] = CTRL('d');
        t->c_cc[VSTART] = CTRL('q');
        t->c_cc[VSTOP] = CTRL('s');
        t->c_cc[VSUSP] = CTRL('z');
        t->c_cc[VDSUSP] = CTRL('y');
        t->c_cc[VREPRINT] = CTRL('r');
        t->c_cc[VDISCARD] = CTRL('o');
        t->c_cc[VWERASE] = CTRL('w');
        t->c_cc[VLNEXT] = CTRL('v');
        tp->t_winsize.ws_row = 24;
        tp->t_winsize.ws_col = 80;
}

/* Take a terminal off the list, before its driver frees it. */
void
tty_destroy(struct tty *tp)
{
        struct tty **link;

        mutex_lock(&tty_list_lock);
        for (link = &tty_list; *link != NULL; link = &(*link)->t_next) {
                if (*link == tp) {
                        *link = tp->t_next;
                        break;
                }
        }
        mutex_unlock(&tty_list_lock);
}

/* Whether session sid has a controlling terminal. tty_list_lock held. */
static int
tty_session_has(pid_t sid)
{
        for (struct tty *tp = tty_list; tp != NULL; tp = tp->t_next)
                if (tp->t_sid == sid)
                        return 1;
        return 0;
}

/*
 * The device was opened with file flags flags: a session leader with
 * no controlling terminal, opening one that is nobody's without
 * O_NOCTTY, makes it its own, with its group in the foreground (SVR4).
 */
void
tty_open(struct tty *tp, int flags)
{
        struct proc *p = curproc;

        /* Init, process 1, never has a controlling terminal. */
        if ((flags & FNOCTTY) != 0 || curlwp == NULL ||
            p->p_pid != p->p_sid || p == initproc)
                return;
        mutex_lock(&tty_list_lock);
        mutex_lock(&tp->t_lock);
        if (tp->t_sid == 0 && !tp->t_hungup && !tty_session_has(p->p_sid)) {
                tp->t_sid = p->p_sid;
                tp->t_pgrp = p->p_pgid;
        }
        mutex_unlock(&tp->t_lock);
        mutex_unlock(&tty_list_lock);
}

/* SIGHUP, then SIGCONT, to a group that lost its terminal. */
static void
tty_hup_signal(pid_t pgrp)
{
        if (pgrp > 0) {
                pgsignal(pgrp, SIGHUP);
                pgsignal(pgrp, SIGCONT);
        }
}

/*
 * The line went (a pseudo-terminal's master was closed): reads see end
 * of file and writes fail with EIO from now on; the foreground group
 * and the session leader's group get SIGHUP and SIGCONT, and the
 * session loses its controlling terminal.
 */
void
tty_hangup(struct tty *tp)
{
        pid_t pgrp, sid;

        mutex_lock(&tp->t_lock);
        tp->t_hungup = 1;
        tty_flush_input(tp);
        tp->t_stopped = 0;
        pgrp = tp->t_pgrp;
        sid = tp->t_sid;
        tp->t_pgrp = tp->t_sid = 0;
        sv_broadcast(&tp->t_insv);
        sv_broadcast(&tp->t_outsv);
        sv_broadcast(&tp->t_roomsv);
        mutex_unlock(&tp->t_lock);
        pollwakeup(&tp->t_ph);
        tty_hup_signal(pgrp);
        if (sid > 0 && sid != pgrp)
                tty_hup_signal(sid);
}

/*
 * The leader of session sid exits: its controlling terminal is let go,
 * and the foreground group gets SIGHUP and SIGCONT (POSIX).
 */
void
tty_session_end(pid_t sid)
{
        for (;;) {
                pid_t pgrp = -1;

                mutex_lock(&tty_list_lock);
                for (struct tty *tp = tty_list; tp != NULL; tp = tp->t_next) {
                        mutex_lock(&tp->t_lock);
                        if (tp->t_sid == sid) {
                                pgrp = tp->t_pgrp;
                                tp->t_sid = tp->t_pgrp = 0;
                        }
                        mutex_unlock(&tp->t_lock);
                        if (pgrp != -1)
                                break;
                }
                mutex_unlock(&tty_list_lock);
                if (pgrp == -1)
                        return;
                tty_hup_signal(pgrp);
        }
}

/* The controlling terminal of session sid, held, for /dev/tty; or NULL. */
struct vnode *
tty_ctty_vnode(pid_t sid)
{
        struct vnode *vp = NULL;

        if (sid == 0)
                return NULL;
        mutex_lock(&tty_list_lock);
        for (struct tty *tp = tty_list; tp != NULL; tp = tp->t_next) {
                if (tp->t_sid == sid && tp->t_vnode != NULL &&
                    vn_get(tp->t_vnode, tp->t_vnode->v_number)) {
                        vp = tp->t_vnode;
                        break;
                }
        }
        mutex_unlock(&tty_list_lock);
        return vp;
}

/*
 * Wait until the input queue has room for a character from the device
 * (a pseudo-terminal's master writes this way): 0, EAGAIN if nonblock,
 * EIO after a hang-up, or EINTR or ERESTART for a signal.
 */
int
tty_wait_room(struct tty *tp, int nonblock)
{
        int error = 0;

        mutex_lock(&tp->t_lock);
        while (tp->t_incount + tp->t_linelen + 1 >= TTY_INQ &&
            !tp->t_hungup) {
                if (nonblock) {
                        error = EAGAIN;
                        break;
                }
                if ((error = tty_sleep(tp, &tp->t_roomsv)) != 0)
                        break;
        }
        if (error == 0 && tp->t_hungup)
                error = EIO;
        mutex_unlock(&tp->t_lock);
        return error;
}

/*
 * Job control: a process of a background group of the terminal's own
 * session that reads it (sig SIGTTIN), or writes it with TOSTOP or
 * changes its modes (SIGTTOU), stops its group with sig and has the
 * call made again when it continues. Returns 0 to go on, or the error:
 * ERESTART (or EINTR, for a handler without SA_RESTART); EIO when the
 * group is orphaned and could never be continued, or for a read with
 * SIGTTIN held or ignored. A write or a mode change with SIGTTOU held
 * or ignored goes on. Called without t_lock.
 */
static int
tty_bgcheck(struct tty *tp, int sig)
{
        struct proc *p = curproc;
        pid_t sid, pgrp;
        int r;

        if (curlwp == NULL)
                return 0;
        mutex_lock(&tp->t_lock);
        sid = tp->t_sid;
        pgrp = tp->t_pgrp;
        mutex_unlock(&tp->t_lock);
        if (sid == 0 || p->p_sid != sid || p->p_pgid == pgrp)
                return 0;
        if (sig_held_or_ignored(sig))
                return sig == SIGTTIN ? EIO : 0;
        if (pgrp_orphaned(p->p_pgid, NULL))
                return EIO;
        pgsignal(p->p_pgid, sig);
        r = sig_sleep_check();
        return r == 2 ? ERESTART : sig_restart(EINTR);
}

/* Whether the device has room for a chunk of output. t_lock is held. */
static int
tty_outroom(struct tty *tp)
{
        return tp->t_outspace == NULL || tp->t_outspace(tp) >= 2 * TTY_CHUNK;
}

/* Whether c is the control character in slot i (not disabled). */
static int
tty_cc(const struct tty *tp, int i, int c)
{
        cc_t cc = tp->t_termios.c_cc[i];

        return cc != _POSIX_VDISABLE && c == cc;
}

/* Write n characters through output processing. t_lock is held. */
static void
tty_out(struct tty *tp, const char *buf, size_t n)
{
        const struct termios *t = &tp->t_termios;
        char out[2 * TTY_CHUNK];
        size_t o = 0;

        if ((t->c_oflag & OPOST) == 0) {
                tp->t_output(tp, buf, n);
                return;
        }
        for (size_t i = 0; i < n; i++) {
                char c = buf[i];

                if (o + 2 > sizeof(out)) {
                        tp->t_output(tp, out, o);
                        o = 0;
                }
                if ((t->c_oflag & OLCUC) != 0 && c >= 'a' && c <= 'z')
                        c = (char)(c - 'a' + 'A');
                if (c == '\n' && (t->c_oflag & ONLCR) != 0) {
                        out[o++] = '\r';
                } else if (c == '\r') {
                        if ((t->c_oflag & ONOCR) != 0)
                                continue;       /* no column count yet */
                        if ((t->c_oflag & OCRNL) != 0)
                                c = '\n';
                }
                out[o++] = c;
        }
        if (o != 0)
                tp->t_output(tp, out, o);
}

/* Echo an input character. t_lock is held. */
static void
tty_echo(struct tty *tp, int c)
{
        tcflag_t lflag = tp->t_termios.c_lflag;
        char buf[2];

        if ((lflag & ECHO) == 0) {
                if (c == '\n' && (lflag & (ICANON | ECHONL)) ==
                    (ICANON | ECHONL))
                        tty_out(tp, "\n", 1);
                return;
        }
        if ((lflag & ECHOCTL) != 0 && ((c < ' ' && c != '\t' && c != '\n') ||
            c == DEL)) {
                buf[0] = '^';
                buf[1] = c == DEL ? '?' : (char)(c + '@');
                tty_out(tp, buf, 2);
                return;
        }
        buf[0] = (char)c;
        tty_out(tp, buf, 1);
}

/* Take the last character off the line being edited, and its echo. */
static void
tty_erase1(struct tty *tp)
{
        tcflag_t lflag = tp->t_termios.c_lflag;
        int c;

        if (tp->t_linelen == 0)
                return;
        c = (unsigned char)tp->t_line[--tp->t_linelen];
        if ((lflag & ECHO) == 0)
                return;
        if ((lflag & ECHOE) == 0) {
                tty_echo(tp, tp->t_termios.c_cc[VERASE]);
                return;
        }
        tty_out(tp, "\b \b", 3);
        /* A control character echoed as ^X took two columns. */
        if ((lflag & ECHOCTL) != 0 && ((c < ' ' && c != '\t') || c == DEL))
                tty_out(tp, "\b \b", 3);
}

/* Append an entry to the input queue; 0 if it is full. */
static int
tty_enqueue(struct tty *tp, uint16_t e)
{
        if (tp->t_incount == TTY_INQ)
                return 0;
        tp->t_inq[(tp->t_inhead + tp->t_incount) % TTY_INQ] = e;
        tp->t_incount++;
        if ((e & (TQ_DELIM | TQ_EOF)) != 0)
                tp->t_lines++;
        return 1;
}

static void
tty_flush_input(struct tty *tp)
{
        tp->t_inhead = tp->t_incount = tp->t_lines = 0;
        tp->t_linelen = 0;
        tp->t_lnext = 0;
}

/*
 * Move the edited line to the input queue, ending it with delim (a
 * character) or, if delim is -1 (VEOF), with an end-of-file mark, which
 * ends the line but is not read. If the queue has no room for the whole
 * line, the line is dropped.
 */
static void
tty_end_line(struct tty *tp, int delim)
{
        size_t need = tp->t_linelen + 1;

        if (TTY_INQ - tp->t_incount < need) {
                tp->t_linelen = 0;
                return;
        }
        for (size_t i = 0; i < tp->t_linelen; i++)
                (void)tty_enqueue(tp, (unsigned char)tp->t_line[i]);
        tp->t_linelen = 0;
        (void)tty_enqueue(tp, delim >= 0 ? (uint16_t)(delim | TQ_DELIM) :
            TQ_EOF);
}

/*
 * Keep an ordinary input character: on the line being edited, or in
 * the input queue, and echo it. Returns whether readers have more.
 * t_lock is held.
 */
static int
tty_store(struct tty *tp, int c)
{
        const struct termios *t = &tp->t_termios;

        if ((t->c_lflag & ICANON) != 0) {
                if (tp->t_linelen + 1 >= TTY_LINE) {
                        if ((t->c_iflag & IMAXBEL) != 0)
                                tp->t_output(tp, "\a", 1);
                        return 0;
                }
                tp->t_line[tp->t_linelen++] = (char)c;
                tty_echo(tp, c);
                return 0;
        }
        if (!tty_enqueue(tp, (uint16_t)c)) {
                if ((t->c_iflag & IMAXBEL) != 0)
                        tp->t_output(tp, "\a", 1);
                return 0;
        }
        tty_echo(tp, c);
        return 1;
}

/* Signal the foreground process group. Called without t_lock. */
static void
tty_signal(struct tty *tp, int sig)
{
        pid_t pgrp;

        mutex_lock(&tp->t_lock);
        pgrp = tp->t_pgrp;
        mutex_unlock(&tp->t_lock);
        if (pgrp > 0) {
                /* A group nothing could continue is not stopped. */
                if (sig == SIGTSTP && pgrp_orphaned(pgrp, NULL))
                        return;
                pgsignal(pgrp, sig);
        }
}

/* A character from the device. */
void
tty_input(struct tty *tp, int c)
{
        struct termios *t = &tp->t_termios;
        int sig = 0, wake = 0;

        c &= 0xff;
        mutex_lock(&tp->t_lock);
        if ((t->c_iflag & ISTRIP) != 0)
                c &= 0x7f;
        if (tp->t_lnext) {
                /* Over the ^V echoed for VLNEXT. */
                tp->t_lnext = 0;
                if ((t->c_lflag & (ECHO | ECHOCTL)) == (ECHO | ECHOCTL))
                        tty_out(tp, "\b\b", 2);
                wake = tty_store(tp, c);
                goto done;
        }
        if (c == '\r') {
                if ((t->c_iflag & IGNCR) != 0)
                        goto out;
                if ((t->c_iflag & ICRNL) != 0)
                        c = '\n';
        } else if (c == '\n' && (t->c_iflag & INLCR) != 0) {
                c = '\r';
        }
        if ((t->c_iflag & IXON) != 0) {
                if (tty_cc(tp, VSTOP, c)) {
                        tp->t_stopped = 1;
                        goto out;
                }
                if (tty_cc(tp, VSTART, c) || (tp->t_stopped &&
                    (t->c_iflag & IXANY) != 0)) {
                        tp->t_stopped = 0;
                        sv_broadcast(&tp->t_outsv);
                        if (tty_cc(tp, VSTART, c))
                                goto out;
                }
        }
        if ((t->c_lflag & ISIG) != 0) {
                if (tty_cc(tp, VINTR, c))
                        sig = SIGINT;
                else if (tty_cc(tp, VQUIT, c))
                        sig = SIGQUIT;
                else if (tty_cc(tp, VSUSP, c))
                        sig = SIGTSTP;
                if (sig != 0) {
                        if ((t->c_lflag & NOFLSH) == 0)
                                tty_flush_input(tp);
                        tty_echo(tp, c);
                        goto out;
                }
        }
        if ((t->c_lflag & ICANON) != 0) {
                if ((t->c_lflag & IEXTEN) != 0 && tty_cc(tp, VLNEXT, c)) {
                        tp->t_lnext = 1;
                        if ((t->c_lflag & ECHO) != 0)
                                tty_echo(tp, c);
                        goto out;
                }
                if (tty_cc(tp, VERASE, c)) {
                        tty_erase1(tp);
                        goto out;
                }
                if ((t->c_lflag & IEXTEN) != 0 && tty_cc(tp, VWERASE, c)) {
                        while (tp->t_linelen > 0 &&
                            tp->t_line[tp->t_linelen - 1] == ' ')
                                tty_erase1(tp);
                        while (tp->t_linelen > 0 &&
                            tp->t_line[tp->t_linelen - 1] != ' ')
                                tty_erase1(tp);
                        goto out;
                }
                if (tty_cc(tp, VKILL, c)) {
                        if ((t->c_lflag & ECHOKE) != 0 &&
                            (t->c_lflag & ECHO) != 0) {
                                while (tp->t_linelen > 0)
                                        tty_erase1(tp);
                        } else {
                                tp->t_linelen = 0;
                                tty_echo(tp, c);
                                if ((t->c_lflag & ECHOK) != 0)
                                        tty_out(tp, "\n", 1);
                        }
                        goto out;
                }
                if (tty_cc(tp, VEOF, c)) {
                        tty_end_line(tp, -1);
                        wake = 1;
                        goto done;
                }
                if (c == '\n' || tty_cc(tp, VEOL, c) ||
                    tty_cc(tp, VEOL2, c)) {
                        tty_echo(tp, c);
                        tty_end_line(tp, c);
                        wake = 1;
                        goto done;
                }
        }
        wake = tty_store(tp, c);
done:
        if (wake)
                sv_broadcast(&tp->t_insv);
out:
        mutex_unlock(&tp->t_lock);
        if (wake)
                pollwakeup(&tp->t_ph);
        if (sig != 0)
                tty_signal(tp, sig);
}

/* Characters a read could take now. t_lock is held. */
static size_t
tty_readable(const struct tty *tp)
{
        if ((tp->t_termios.c_lflag & ICANON) != 0)
                return tp->t_lines;
        return tp->t_incount;
}

/*
 * Sleep on sv, which t_lock is held for and is again on return: 0, or
 * EINTR or ERESTART for a signal. A timed wait's end returns 0 with
 * l_timedout set.
 */
static int
tty_sleep(struct tty *tp, struct sv *sv)
{
        int r = 0;

        if (sv_wait_mutex_sig(sv, &tp->t_lock) != 0 &&
            !curlwp->l_timedout && (r = sig_sleep_check()) != 0)
                r = r == 1 ? EINTR : ERESTART;
        mutex_lock(&tp->t_lock);
        return r;
}

int
tty_read(struct tty *tp, struct uio *uio, int ioflag)
{
        struct termios *t = &tp->t_termios;
        size_t last = 0;
        int64_t id = 0;
        int timing = 0, error = 0;

        if ((error = tty_bgcheck(tp, SIGTTIN)) != 0)
                return error;
        mutex_lock(&tp->t_lock);
        /* Wait for something to read, as the mode says. */
        for (;;) {
                size_t avail = tty_readable(tp);

                if (tp->t_hungup)
                        goto out;               /* end of file */
                size_t want = t->c_cc[VMIN];
                long vtime = (long)t->c_cc[VTIME] * HZ / 10;

                if ((t->c_lflag & ICANON) != 0) {
                        if (avail != 0)
                                break;
                } else {
                        if (want > (size_t)uio->uio_resid)
                                want = (size_t)uio->uio_resid;
                        if (want == 0 && (vtime == 0 || avail != 0))
                                break;          /* VMIN 0: what there is */
                        if (want != 0 && avail >= want)
                                break;
                        if (timing && curlwp->l_timedout)
                                break;          /* VTIME ran out */
                        /*
                         * VTIME runs from the read with VMIN 0, else
                         * afresh from each character that comes.
                         */
                        if (vtime != 0 && ((want == 0 && !timing) ||
                            (want != 0 && avail > last))) {
                                if (timing)
                                        lwp_timeout_stop(id);
                                id = lwp_timeout_start(vtime);
                                timing = 1;
                        }
                        last = avail;
                }
                if ((ioflag & (IO_NDELAY | IO_NONBLOCK)) != 0) {
                        if (avail != 0)
                                break;
                        error = (ioflag & IO_NONBLOCK) != 0 ? EAGAIN : 0;
                        goto out;
                }
                if ((error = tty_sleep(tp, &tp->t_insv)) != 0)
                        goto out;
        }

        /* Take characters: one line at most in canonical mode. */
        while (tp->t_incount != 0 && uio->uio_resid > 0) {
                char buf[TTY_CHUNK];
                size_t n = 0;
                int done = 0;

                while (tp->t_incount != 0 && n < sizeof(buf) &&
                    (ssize_t)n < uio->uio_resid) {
                        uint16_t e = tp->t_inq[tp->t_inhead];

                        /* The end-of-file mark ends the line unread. */
                        if ((e & TQ_EOF) != 0) {
                                tp->t_inhead = (tp->t_inhead + 1) % TTY_INQ;
                                tp->t_incount--;
                                tp->t_lines--;
                                done = 1;
                                break;
                        }
                        tp->t_inhead = (tp->t_inhead + 1) % TTY_INQ;
                        tp->t_incount--;
                        buf[n++] = (char)(e & 0xff);
                        if ((e & (TQ_DELIM | TQ_EOF)) != 0) {
                                tp->t_lines--;
                                if ((t->c_lflag & ICANON) != 0) {
                                        done = 1;
                                        break;
                                }
                        }
                }
                if ((error = uiomove(buf, n, UIO_READ, uio)) != 0)
                        break;
                if (done)
                        break;
        }
        sv_broadcast(&tp->t_roomsv);
out:
        mutex_unlock(&tp->t_lock);
        if (timing)
                lwp_timeout_stop(id);
        return error;
}

int
tty_write(struct tty *tp, struct uio *uio, int ioflag)
{
        char buf[TTY_CHUNK];
        int error = 0;

        if ((tp->t_termios.c_lflag & TOSTOP) != 0 &&
            (error = tty_bgcheck(tp, SIGTTOU)) != 0)
                return error;
        while (uio->uio_resid > 0) {
                size_t n = uio->uio_resid < TTY_CHUNK ?
                    (size_t)uio->uio_resid : TTY_CHUNK;

                /*
                 * Wait for output to be started, and for the device to
                 * have room, before taking any.
                 */
                mutex_lock(&tp->t_lock);
                while (!tp->t_hungup && (tp->t_stopped || !tty_outroom(tp))) {
                        if ((ioflag & IO_NONBLOCK) != 0) {
                                error = EAGAIN;
                                break;
                        }
                        if ((error = tty_sleep(tp, &tp->t_outsv)) != 0)
                                break;
                }
                if (error == 0 && tp->t_hungup)
                        error = EIO;
                if (error == 0 &&
                    (error = uiomove(buf, n, UIO_WRITE, uio)) == 0)
                        tty_out(tp, buf, n);
                mutex_unlock(&tp->t_lock);
                if (error != 0)
                        break;
        }
        return error;
}

/*
 * Take new modes. A canonical line still being edited when ICANON goes
 * off is passed on as it stands. t_lock is held.
 */
static int
tty_setmodes(struct tty *tp, const struct termios *nt, int flush)
{
        tcflag_t was = tp->t_termios.c_lflag;

        if (flush)
                tty_flush_input(tp);
        if ((was & ICANON) != 0 && (nt->c_lflag & ICANON) == 0) {
                for (size_t i = 0; i < tp->t_linelen; i++)
                        (void)tty_enqueue(tp, (unsigned char)tp->t_line[i]);
                tp->t_linelen = 0;
        }
        tp->t_termios = *nt;
        if ((nt->c_iflag & IXON) == 0 && tp->t_stopped) {
                tp->t_stopped = 0;
                sv_broadcast(&tp->t_outsv);
        }
        sv_broadcast(&tp->t_insv);
        return 0;
}

/*
 * The terminal ioctls (see <sys/termios.h>); arg is the user address
 * or value the call was given.
 */
int
tty_ioctl(struct tty *tp, unsigned long cmd, uint64_t arg, int flag,
    struct cred *cr)
{
        void *uaddr = (void *)(uintptr_t)arg;
        struct proc *p = curproc;
        struct termios nt;
        struct termio to;
        struct winsize ws;
        int32_t v;
        int error = 0, sigwinch = 0;
        char c;

        (void)flag;
        /* Changing the modes from the background stops the group. */
        switch (cmd) {
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
        case TCSETA:
        case TCSETAW:
        case TCSETAF:
        case TCSBRK:
        case TCXONC:
        case TCFLSH:
        case TIOCSPGRP:
                if ((error = tty_bgcheck(tp, SIGTTOU)) != 0)
                        return error;
                break;
        }
        switch (cmd) {
        case TCGETS:
                mutex_lock(&tp->t_lock);
                nt = tp->t_termios;
                mutex_unlock(&tp->t_lock);
                return copyout(&nt, uaddr, sizeof(nt));
        case TCSETS:
        case TCSETSW:
        case TCSETSF:
                if ((error = copyin(uaddr, &nt, sizeof(nt))) != 0)
                        return error;
                mutex_lock(&tp->t_lock);
                error = tty_setmodes(tp, &nt, cmd == TCSETSF);
                mutex_unlock(&tp->t_lock);
                return error;
        case TCGETA:
                mutex_lock(&tp->t_lock);
                nt = tp->t_termios;
                mutex_unlock(&tp->t_lock);
                memset(&to, 0, sizeof(to));
                to.c_iflag = (uint16_t)nt.c_iflag;
                to.c_oflag = (uint16_t)nt.c_oflag;
                to.c_cflag = (uint16_t)nt.c_cflag;
                to.c_lflag = (uint16_t)nt.c_lflag;
                memcpy(to.c_cc, nt.c_cc, NCC);
                return copyout(&to, uaddr, sizeof(to));
        case TCSETA:
        case TCSETAW:
        case TCSETAF:
                /* The low 16 bits of each mode word, and NCC characters. */
                if ((error = copyin(uaddr, &to, sizeof(to))) != 0)
                        return error;
                mutex_lock(&tp->t_lock);
                nt = tp->t_termios;
                nt.c_iflag = (nt.c_iflag & ~0xffffu) | to.c_iflag;
                nt.c_oflag = (nt.c_oflag & ~0xffffu) | to.c_oflag;
                nt.c_cflag = (nt.c_cflag & ~0xffffu) | to.c_cflag;
                nt.c_lflag = (nt.c_lflag & ~0xffffu) | to.c_lflag;
                memcpy(nt.c_cc, to.c_cc, NCC);
                error = tty_setmodes(tp, &nt, cmd == TCSETAF);
                mutex_unlock(&tp->t_lock);
                return error;
        case TCSBRK:
                /* Output is synchronous: it has drained; no break. */
                return 0;
        case TCXONC:
                mutex_lock(&tp->t_lock);
                switch (arg) {
                case TCOOFF:
                        tp->t_stopped = 1;
                        break;
                case TCOON:
                        tp->t_stopped = 0;
                        sv_broadcast(&tp->t_outsv);
                        break;
                case TCIOFF:
                case TCION:
                        c = (char)tp->t_termios.c_cc[arg == TCIOFF ?
                            VSTOP : VSTART];
                        tp->t_output(tp, &c, 1);
                        break;
                default:
                        error = EINVAL;
                }
                mutex_unlock(&tp->t_lock);
                return error;
        case TCFLSH:
                if (arg > TCIOFLUSH)
                        return EINVAL;
                mutex_lock(&tp->t_lock);
                if (arg != TCOFLUSH)
                        tty_flush_input(tp);
                mutex_unlock(&tp->t_lock);
                return 0;
        case TIOCGWINSZ:
                mutex_lock(&tp->t_lock);
                ws = tp->t_winsize;
                mutex_unlock(&tp->t_lock);
                return copyout(&ws, uaddr, sizeof(ws));
        case TIOCSWINSZ:
                if ((error = copyin(uaddr, &ws, sizeof(ws))) != 0)
                        return error;
                mutex_lock(&tp->t_lock);
                sigwinch = memcmp(&ws, &tp->t_winsize, sizeof(ws)) != 0;
                tp->t_winsize = ws;
                mutex_unlock(&tp->t_lock);
                if (sigwinch)
                        tty_signal(tp, SIGWINCH);
                return 0;
        case TIOCGPGRP:
        case TIOCGSID:
                mutex_lock(&tp->t_lock);
                v = cmd == TIOCGPGRP ? tp->t_pgrp : tp->t_sid;
                mutex_unlock(&tp->t_lock);
                if (cmd == TIOCGSID && v == 0)
                        return ENOTTY;
                return copyout(&v, uaddr, sizeof(v));
        case TIOCSPGRP:
                if ((error = copyin(uaddr, &v, sizeof(v))) != 0)
                        return error;
                if (v <= 0 || !pgrp_in_session(v, p->p_sid))
                        return EPERM;
                mutex_lock(&tp->t_lock);
                if (tp->t_sid != 0 && tp->t_sid != p->p_sid)
                        error = ENOTTY;
                else {
                        tp->t_pgrp = v;
                        tp->t_sid = p->p_sid;
                }
                mutex_unlock(&tp->t_lock);
                return error;
        case TIOCSCTTY:
                /*
                 * A session leader makes this its controlling terminal,
                 * with its group in the foreground; one that belongs to
                 * another session only the superuser may take (arg 1).
                 */
                if (p->p_pid != p->p_sid)
                        return EPERM;
                mutex_lock(&tp->t_lock);
                if (tp->t_sid != 0 && tp->t_sid != p->p_sid &&
                    !(arg == 1 && CRED_ISROOT(cr)))
                        error = EPERM;
                else {
                        tp->t_sid = p->p_sid;
                        tp->t_pgrp = p->p_pgid;
                }
                mutex_unlock(&tp->t_lock);
                return error;
        case TIOCSTI:
                /* Only the superuser, or a process of its session. */
                mutex_lock(&tp->t_lock);
                v = tp->t_sid;
                mutex_unlock(&tp->t_lock);
                if (!CRED_ISROOT(cr) && v != p->p_sid)
                        return EPERM;
                if ((error = copyin(uaddr, &c, 1)) != 0)
                        return error;
                tty_input(tp, (unsigned char)c);
                return 0;
        case FIONREAD:
                mutex_lock(&tp->t_lock);
                v = 0;
                for (size_t i = 0; i < tp->t_incount; i++)
                        if ((tp->t_inq[(tp->t_inhead + i) % TTY_INQ] &
                            TQ_EOF) == 0)
                                v++;
                mutex_unlock(&tp->t_lock);
                return copyout(&v, uaddr, sizeof(v));
        default:
                return ENOTTY;
        }
}

int
tty_poll(struct tty *tp, short events, short *reventsp,
    struct pollhead **phpp)
{
        short rev = POLLOUT;

        (void)events;
        mutex_lock(&tp->t_lock);
        if (tty_readable(tp) != 0)
                rev |= POLLIN | POLLRDNORM;
        mutex_unlock(&tp->t_lock);
        *reventsp = rev;
        *phpp = &tp->t_ph;
        return 0;
}
