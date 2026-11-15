/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * libterminfo's termcap_hash.c, which termcap.c includes after
 * termcap_map.c: for a two-letter termcap name, its index in that file's
 * tables, or a value past their end. NetBSD makes this with nbperf, a
 * perfect hash generator; a plain search does as well here.
 */

static uint32_t
_t_search(const TENTRY *tbl, size_t n, const unsigned char *key, size_t len)
{
        for (size_t i = 0; i < n; i++)
                if (strlen(tbl[i].id) == len &&
                    memcmp(tbl[i].id, key, len) == 0)
                        return (uint32_t)i;
        return UINT32_MAX;
}

static uint32_t
_t_flaghash(const unsigned char *key, size_t len)
{
        return _t_search(_ti_cap_flagids, __arraycount(_ti_cap_flagids), key,
            len);
}

static uint32_t
_t_numhash(const unsigned char *key, size_t len)
{
        return _t_search(_ti_cap_numids, __arraycount(_ti_cap_numids), key,
            len);
}

static uint32_t
_t_strhash(const unsigned char *key, size_t len)
{
        return _t_search(_ti_cap_strids, __arraycount(_ti_cap_strids), key,
            len);
}
