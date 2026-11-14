/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/* Dereference a null pointer: the kernel must end us with SIGSEGV. */

int
main(void)
{
        *(volatile int *)0 = 1;
        return 0;
}
