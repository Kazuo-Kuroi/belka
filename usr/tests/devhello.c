/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-18
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>

int devvalue(void);

/* Guest ld links this object against an archive built inside Belka. */
int
main(void)
{
        if (devvalue() != 42)
                return 1;
        return puts("native linker") < 0;
}
