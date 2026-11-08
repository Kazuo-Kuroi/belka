/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/* Dereference a null pointer: the kernel must end us with SIGSEGV. */

#include "lib/libt.h"

int
main(int argc __attribute__((__unused__)), char **argv __attribute__((__unused__)),
    char **envp __attribute__((__unused__)))
{
        *(volatile int *)0 = 1;
        return 0;
}
