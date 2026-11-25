/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 */

/*
 * The Ethernet DLPI provider every Ethernet driver shares (net/dlether.c):
 * DLPI version 2, style 1, connectionless, one stream per open, each
 * bound to an Ethernet type. A driver fills in a struct dlether with its
 * name, station address and transmit routine, registers it, and hands
 * each frame it receives to dlether_input().
 */

#ifndef _SYS_DLETHER_H_
#define _SYS_DLETHER_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/synch.h>

#define ETHER_ADDRLEN   6
#define ETHER_HDRLEN    14
#define ETHER_MTU       1500
#define ETHER_MINFRAME  60              /* without the frame check sequence */
#define ETHER_MAXFRAME  (ETHER_HDRLEN + ETHER_MTU)

struct dlether_stream;

struct dlether {
        /* Set by the driver before dlether_register(). */
        char            de_name[16];    /* /dev/<name> and the IP name */
        uint8_t         de_addr[ETHER_ADDRLEN];
        /*
         * Send one frame, len bytes (header and data, padded to
         * ETHER_MINFRAME), from any context but an interrupt; it may not
         * wait. 0 if queued, else the frame is dropped.
         */
        int             (*de_output)(struct dlether *, const uint8_t *,
                            size_t);
        /*
         * Optional: take every frame (on) or only the station's and
         * group ones (off), as the first stream turns DL_PROMISC_PHYS on
         * or the last turns it off; from a stream's put or
         * close procedure, with de_lock held, so it may not wait.
         */
        void            (*de_promisc)(struct dlether *, int);
        void            *de_softc;      /* the driver's */

        /* The provider's. */
        struct mutex    de_lock;        /* the streams and their state */
        struct dlether_stream *de_streams;
        int             de_nphys;       /* streams with DL_PROMISC_PHYS */
        volatile uint64_t de_ipackets;
        volatile uint64_t de_opackets;
        volatile uint64_t de_drops;
};

/* /dev/<de_name> as unit of major; 0, or -1 if there is no room. */
int dlether_register(struct dlether *, unsigned int major, unsigned int unit);

/*
 * A received frame (no frame check sequence), to the streams that want
 * it, from a thread that may take a mutex; it is copied.
 */
void dlether_input(struct dlether *, const uint8_t *, size_t);

#endif /* _SYS_DLETHER_H_ */
