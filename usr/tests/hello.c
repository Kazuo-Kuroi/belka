/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/* The smallest user program: a line on the console and a clean exit. */

#include "lib/libt.h"

int
main(int argc __attribute__((__unused__)), char **argv __attribute__((__unused__)),
    char **envp __attribute__((__unused__)))
{
        printf("hello from user space, pid %d\n", getpid());
        return 0;
}
