/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-07
 * SPDX-License-Identifier: MIT
 */

/*
 * Console output switch. Output before cninit() is discarded.
 *
 * There is no locking yet: only the bootstrap processor runs. A console
 * spinlock is added together with the synchronisation primitives.
 */

#include <stddef.h>

#include <sys/cons.h>

static struct consdev *cn_tab;

void
cninit(struct consdev *cp)
{
        cn_tab = cp;
}

/* Write one character, expanding a newline to carriage return and newline. */
void
cnputc(int c)
{
        if (cn_tab == NULL)
                return;
        if (c == '\n')
                cn_tab->cn_putc(cn_tab, '\r');
        cn_tab->cn_putc(cn_tab, c);
}
