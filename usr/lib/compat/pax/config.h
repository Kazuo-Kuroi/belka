/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#include <inttypes.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <vis.h>

#define lchmod compat_pax_lchmod
int compat_pax_lchmod(const char *, mode_t);
char *vis(char *, int, int, int);

#define _PATH_DEFTAPE "/dev/rmt0"
int raise_default_signal(int);
int uid_from_user(const char *, uid_t *);
int gid_from_group(const char *, gid_t *);
