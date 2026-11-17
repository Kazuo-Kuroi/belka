/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#include "sh.h"

/* Host type variables for the amd64 Belka target. */
void
getmachine(void)
{
        tsetenv(STRHOSTTYPE, str2short("amd64-belka"));
        tsetenv(STRMACHTYPE, str2short("amd64"));
        tsetenv(STROSTYPE, str2short("belka"));
        tsetenv(STRVENDOR, str2short("unknown"));
}
