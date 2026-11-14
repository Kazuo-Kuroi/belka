/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/* The smallest user program: a line on the console and a clean exit. */

#include <stdio.h>
#include <unistd.h>

int
main(void)
{
        printf("hello from user space, pid %d\n", (int)getpid());
        return 0;
}
