/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Register layout of the National Semiconductor 16550 UART and its
 * compatibles. Offsets are in register units from the base address.
 */

#ifndef _DEV_IC_NS16550REG_H_
#define _DEV_IC_NS16550REG_H_

#define COM_DATA        0       /* receive/transmit data (DLAB = 0) */
#define COM_IER         1       /* interrupt enable (DLAB = 0) */
#define COM_DLL         0       /* divisor latch low (DLAB = 1) */
#define COM_DLM         1       /* divisor latch high (DLAB = 1) */
#define COM_FCR         2       /* FIFO control (write) */
#define COM_LCR         3       /* line control */
#define COM_MCR         4       /* modem control */
#define COM_LSR         5       /* line status */
#define COM_SCR         7       /* scratch */

#define FCR_ENABLE      0x01
#define FCR_RXRESET     0x02
#define FCR_TXRESET     0x04
#define FCR_TRIGGER_14  0xc0

#define LCR_8BITS       0x03
#define LCR_DLAB        0x80

#define MCR_DTR         0x01
#define MCR_RTS         0x02
#define MCR_OUT2        0x08

#define LSR_RXRDY       0x01
#define LSR_TXRDY       0x20    /* transmit holding register empty */
#define LSR_TSRE        0x40    /* transmitter completely empty */

#define COM_FREQ        1843200 /* standard input clock, Hz */

#endif /* _DEV_IC_NS16550REG_H_ */
