/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-10
 * SPDX-License-Identifier: MIT
 */

/*
 * libdt.so.1: a shared library of the tests' own, for dyntest. It has
 * exported functions and data (which a non-PIC program copies into its
 * own data), thread-local storage, a constructor, and calls into
 * libc.so.1.
 */

#include <stdio.h>

int dt_counter = 7;
__thread int dt_tls = 3;
static int constructed;

__attribute__((constructor)) static void
dt_construct(void)
{
        constructed = 1;
}

int
dt_constructed(void)
{
        return constructed;
}

int
dt_add(int a, int b)
{
        return a + b;
}

/* The library's view of dt_counter: the program's copy, if it made one. */
int
dt_get_counter(void)
{
        return dt_counter;
}

int
dt_get_tls(void)
{
        return dt_tls;
}

void
dt_set_tls(int v)
{
        dt_tls = v;
}

int
dt_format(char *buf, size_t size, int n)
{
        return snprintf(buf, size, "libdt %d", n);
}
