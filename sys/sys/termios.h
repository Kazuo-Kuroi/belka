/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-13
 * SPDX-License-Identifier: MIT
 */

/*
 * Terminal modes and the terminal ioctls, with the System V values:
 * struct termios (four mode words and NCCS control characters, for
 * TCGETS and TCSETS), the older struct termio (16-bit mode words, a
 * line discipline byte and NCC characters, for TCGETA and TCSETA), and
 * struct winsize. The mode words are 32 bits, as UnixWare's were.
 */

#ifndef _SYS_TERMIOS_H_
#define _SYS_TERMIOS_H_

#include <stdint.h>

typedef uint32_t        tcflag_t;
typedef uint8_t         cc_t;
typedef uint32_t        speed_t;

#define NCCS            19
#define NCC             8

/*
 * Control characters: indexes into c_cc. VMIN and VTIME share slots
 * with VEOF and VEOL, and apply only outside canonical mode.
 */
#define VINTR           0
#define VQUIT           1
#define VERASE          2
#define VKILL           3
#define VEOF            4
#define VEOL            5
#define VEOL2           6
#define VMIN            4
#define VTIME           5
#define VSWTCH          7
#define VSTART          8
#define VSTOP           9
#define VSUSP           10
#define VDSUSP          11
#define VREPRINT        12
#define VDISCARD        13
#define VWERASE         14
#define VLNEXT          15

#define _POSIX_VDISABLE 0       /* a c_cc slot that matches nothing */

/* c_iflag */
#define IGNBRK          0000001
#define BRKINT          0000002
#define IGNPAR          0000004
#define PARMRK          0000010
#define INPCK           0000020
#define ISTRIP          0000040
#define INLCR           0000100
#define IGNCR           0000200
#define ICRNL           0000400
#define IUCLC           0001000
#define IXON            0002000
#define IXANY           0004000
#define IXOFF           0010000
#define IMAXBEL         0020000

/* c_oflag */
#define OPOST           0000001
#define OLCUC           0000002
#define ONLCR           0000004
#define OCRNL           0000010
#define ONOCR           0000020
#define ONLRET          0000040
#define OFILL           0000100
#define OFDEL           0000200
#define NLDLY           0000400
#define CRDLY           0003000
#define TABDLY          0014000
#define XTABS           0014000
#define BSDLY           0020000
#define VTDLY           0040000
#define FFDLY           0100000

/* c_cflag */
#define CBAUD           0000017
#define CSIZE           0000060
#define CS5             0000000
#define CS6             0000020
#define CS7             0000040
#define CS8             0000060
#define CSTOPB          0000100
#define CREAD           0000200
#define PARENB          0000400
#define PARODD          0001000
#define HUPCL           0002000
#define CLOCAL          0004000
#define CIBAUD          03600000
#define IBSHIFT         16

/* c_lflag */
#define ISIG            0000001
#define ICANON          0000002
#define XCASE           0000004
#define ECHO            0000010
#define ECHOE           0000020
#define ECHOK           0000040
#define ECHONL          0000100
#define NOFLSH          0000200
#define TOSTOP          0000400
#define ECHOCTL         0001000
#define ECHOPRT         0002000
#define ECHOKE          0004000
#define FLUSHO          0020000
#define PENDIN          0040000
#define IEXTEN          0100000

/* Speeds, in CBAUD. */
#define B0              0
#define B50             1
#define B75             2
#define B110            3
#define B134            4
#define B150            5
#define B200            6
#define B300            7
#define B600            8
#define B1200           9
#define B1800           10
#define B2400           11
#define B4800           12
#define B9600           13
#define B19200          14
#define B38400          15

struct termios {
        tcflag_t        c_iflag;
        tcflag_t        c_oflag;
        tcflag_t        c_cflag;
        tcflag_t        c_lflag;
        cc_t            c_cc[NCCS];
};

struct termio {
        uint16_t        c_iflag;
        uint16_t        c_oflag;
        uint16_t        c_cflag;
        uint16_t        c_lflag;
        char            c_line;
        uint8_t         c_cc[NCC];
};

struct winsize {
        uint16_t        ws_row;
        uint16_t        ws_col;
        uint16_t        ws_xpixel;
        uint16_t        ws_ypixel;
};

/* ioctls: ('T' << 8) | n, and the job control ones ('t' << 8) | n. */
#define TIOC            ('T' << 8)
#define TCGETA          (TIOC | 1)
#define TCSETA          (TIOC | 2)
#define TCSETAW         (TIOC | 3)
#define TCSETAF         (TIOC | 4)
#define TCSBRK          (TIOC | 5)
#define TCXONC          (TIOC | 6)
#define TCFLSH          (TIOC | 7)
#define TCGETS          (TIOC | 13)
#define TCSETS          (TIOC | 14)
#define TCSETSW         (TIOC | 15)
#define TCSETSF         (TIOC | 16)
#define TIOCSWINSZ      (TIOC | 103)
#define TIOCGWINSZ      (TIOC | 104)

#define tIOC            ('t' << 8)
#define TIOCGPGRP       (tIOC | 20)
#define TIOCSPGRP       (tIOC | 21)
#define TIOCGSID        (tIOC | 22)
#define TIOCSTI         (tIOC | 23)
#define TIOCSCTTY       (tIOC | 132)    /* Solaris's number; not in UnixWare */

/* File ioctls, for any descriptor. */
#define FIOCLEX         0x20006601      /* close on exec */
#define FIONCLEX        0x20006602
#define FIONBIO         0x8004667e
#define FIONREAD        0x4004667f

/* TCFLSH arguments, and TCXONC's. */
#define TCIFLUSH        0
#define TCOFLUSH        1
#define TCIOFLUSH       2
#define TCOOFF          0
#define TCOON           1
#define TCIOFF          2
#define TCION           3

#endif /* _SYS_TERMIOS_H_ */
