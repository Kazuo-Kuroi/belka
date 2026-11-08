/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Polled console driver for 16550-compatible UARTs.
 */

#include <stdint.h>

#include <sys/cons.h>

#include <machine/cpufunc.h>

#include "dev/ic/ns16550reg.h"
#include "dev/ic/ns16550var.h"

/* Give up on a stuck transmitter after this many polls. */
#define TX_SPIN_LIMIT   100000

static void ns16550_putc(struct consdev *, int);
static void ns16550_flush(struct consdev *);

static void
ns16550_putc(struct consdev *cp, int c)
{
        struct ns16550_softc *sc = cp->cn_private;
        int spin;

        if (sc->sc_present == 0)
                return;
        for (spin = 0; spin < TX_SPIN_LIMIT; spin++) {
                if ((inb(sc->sc_iobase + COM_LSR) & LSR_TXRDY) != 0)
                        break;
                cpu_spinwait();
        }
        outb(sc->sc_iobase + COM_DATA, (uint8_t)c);
}

/* Wait for the FIFO and shift register to drain. */
static void
ns16550_flush(struct consdev *cp)
{
        struct ns16550_softc *sc = cp->cn_private;

        if (sc->sc_present == 0)
                return;
        for (int spin = 0; spin < TX_SPIN_LIMIT; spin++) {
                if ((inb(sc->sc_iobase + COM_LSR) & LSR_TSRE) != 0)
                        break;
                cpu_spinwait();
        }
}

/*
 * Program the UART at iobase for speed bps, 8 data bits, no parity, one
 * stop bit, with FIFOs enabled and interrupts off, and make it the
 * console. Returns 0 on success, or -1 if no UART answers at iobase.
 */
int
ns16550_cninit(struct ns16550_softc *sc, uint16_t iobase, unsigned int bps)
{
        unsigned int divisor = COM_FREQ / (16 * bps);

        sc->sc_iobase = iobase;
        sc->sc_present = 0;
        sc->sc_cons.cn_putc = ns16550_putc;
        sc->sc_cons.cn_flush = ns16550_flush;
        sc->sc_cons.cn_private = sc;
        sc->sc_cons.cn_name = "com";

        /* Probe through the scratch register. */
        outb(iobase + COM_SCR, 0x5a);
        if (inb(iobase + COM_SCR) != 0x5a)
                return -1;

        outb(iobase + COM_IER, 0);
        outb(iobase + COM_LCR, LCR_DLAB);
        outb(iobase + COM_DLL, (uint8_t)(divisor & 0xff));
        outb(iobase + COM_DLM, (uint8_t)(divisor >> 8));
        outb(iobase + COM_LCR, LCR_8BITS);
        outb(iobase + COM_FCR,
            FCR_ENABLE | FCR_RXRESET | FCR_TXRESET | FCR_TRIGGER_14);
        outb(iobase + COM_MCR, MCR_DTR | MCR_RTS | MCR_OUT2);

        sc->sc_present = 1;
        cninit(&sc->sc_cons);
        return 0;
}
