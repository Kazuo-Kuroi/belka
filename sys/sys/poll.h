/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-12
 * SPDX-License-Identifier: MIT
 */

/*
 * poll() and the kernel's poll heads.
 *
 * The event bits are System V's, as the ABI requires. A pollable object
 * keeps a struct pollhead, and VOP_POLL reports which of the asked-for
 * events are true now, handing back the head; poll() registers on the
 * heads of every descriptor and sleeps until pollwakeup() on one of
 * them, a signal, or the timeout.
 */

#ifndef _SYS_POLL_H_
#define _SYS_POLL_H_

#include <stdint.h>

struct pollfd {
        int             fd;
        short           events;
        short           revents;
};

#define POLLIN          0x0001  /* data to read */
#define POLLPRI         0x0002  /* urgent data to read */
#define POLLOUT         0x0004  /* room to write */
#define POLLRDNORM      0x0040  /* ordinary data to read */
#define POLLWRNORM      POLLOUT
#define POLLRDBAND      0x0080  /* priority band data to read */
#define POLLWRBAND      0x0100  /* room to write priority band data */
#define POLLNORM        POLLRDNORM

/* Reported whether asked for or not. */
#define POLLERR         0x0008  /* an error on the object */
#define POLLHUP         0x0010  /* hung up */
#define POLLNVAL        0x0020  /* fd is not open */

#define POLL_READ       (POLLIN | POLLRDNORM | POLLRDBAND | POLLPRI)
#define POLL_WRITE      (POLLOUT | POLLWRBAND)

#ifdef _KERNEL

#include <sys/spinlock.h>

struct polldat;

struct pollhead {
        struct spinlock ph_lock;
        struct polldat  *ph_list;       /* pollers registered, ph_lock */
};

void pollhead_init(struct pollhead *);
void pollwakeup(struct pollhead *);

#endif /* _KERNEL */

#endif /* _SYS_POLL_H_ */
