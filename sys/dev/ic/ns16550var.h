/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Polled 16550 UART, used as the early console.
 *
 * Register access is through x86 port I/O for now. When bus_space is
 * introduced the I/O base becomes a bus space tag and handle, and the
 * driver can then serve memory-mapped UARTs as well.
 */

#ifndef _DEV_IC_NS16550VAR_H_
#define _DEV_IC_NS16550VAR_H_

#include <stdint.h>

#include <sys/cons.h>

struct ns16550_softc {
        struct consdev  sc_cons;
        uint16_t        sc_iobase;
        int             sc_present;
        void            (*sc_input)(int);       /* received characters */
        void            (*sc_disconnect)(void); /* carrier lost */
};

int ns16550_cninit(struct ns16550_softc *, uint16_t, unsigned int);
int ns16550_intr_attach(struct ns16550_softc *, int, void (*)(int),
    void (*)(void));

#endif /* _DEV_IC_NS16550VAR_H_ */
