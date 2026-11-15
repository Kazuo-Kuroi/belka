/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * The one curses function libterminfo defines: setupterm.c includes
 * <curses.h> for use_env()'s declaration, as POSIX puts it there. Seen
 * only while libterminfo builds, until there is a curses.
 */

#ifndef _COMPAT_TERMINFO_CURSES_H_
#define _COMPAT_TERMINFO_CURSES_H_

#include <stdbool.h>

void use_env(bool);

#endif /* _COMPAT_TERMINFO_CURSES_H_ */
