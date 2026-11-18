/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-18
 * SPDX-License-Identifier: MIT
 */

int devvalue(void);

/* A separate archive member forces native archive symbol resolution. */
int
devvalue(void)
{
        return 42;
}
