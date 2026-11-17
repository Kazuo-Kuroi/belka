/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <termios.h>

speed_t getspeed(const char *);
unsigned int baudrate(speed_t);

static const unsigned int rates[] = {
        0, 50, 75, 110, 134, 150, 200, 300,
        600, 1200, 1800, 2400, 4800, 9600, 19200, 38400
};

speed_t
getspeed(const char *text)
{
        char *end;
        unsigned long rate;

        errno = 0;
        rate = strtoul(text, &end, 10);
        if (errno == 0 && text[0] != '\0' && *end == '\0') {
                for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
                        if (rate == rates[i])
                                return (speed_t)i;
        }
        errx(1, "unsupported speed: %s", text);
}

unsigned int
baudrate(speed_t speed)
{
        if (speed >= sizeof(rates) / sizeof(rates[0]))
                errx(1, "invalid terminal speed: %u", speed);
        return rates[speed];
}
