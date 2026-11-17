/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

/* mandoc interleaves byte output and wide characters on stdout. */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

wint_t mandoc_putwchar(wchar_t);

wint_t
mandoc_putwchar(wchar_t character)
{
        char bytes[MB_LEN_MAX];
        mbstate_t state;
        size_t length;

        memset(&state, 0, sizeof(state));
        length = wcrtomb(bytes, character, &state);
        if (length == (size_t)-1 || fwrite(bytes, 1, length, stdout) != length)
                return WEOF;
        return (wint_t)character;
}
