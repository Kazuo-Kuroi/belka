/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#ifndef BELKA_MANDOC_CONFIG_H
#define BELKA_MANDOC_CONFIG_H

#include <sys/types.h>
#include <wchar.h>
wint_t mandoc_putwchar(wchar_t);
#define putwchar mandoc_putwchar
#define MAN_CONF_FILE "/etc/man.conf"
#define MANPATH_BASE "/usr/share/man"
#define MANPATH_DEFAULT "/usr/share/man:/usr/local/share/man"
#define OSENUM MANDOC_OS_OTHER
#define OSNAME "Belka"
#define UTF8_LOCALE "C.UTF-8"
#define VERSION "1.14.6"
#define BINM_APROPOS "apropos"
#define BINM_CATMAN "catman"
#define BINM_MAKEWHATIS "makewhatis"
#define BINM_MAN "man"
#define BINM_SOELIM "soelim"
#define BINM_WHATIS "whatis"
#define BINM_PAGER "/usr/bin/less"
#define NEED_XPG4_2 0
#define HAVE_DIRENT_NAMLEN 0
#define HAVE_ENDIAN 1
#define HAVE_ERR 1
#define HAVE_FTS 1
#define HAVE_FTS_COMPARE_CONST 0
#define HAVE_GETLINE 1
#define HAVE_GETSUBOPT 1
#define HAVE_ISBLANK 1
#define HAVE_LESS_T 0
#define HAVE_MKDTEMP 1
#define HAVE_MKSTEMPS 1
#define HAVE_NTOHL 1
#define HAVE_PLEDGE 0
#define HAVE_PROGNAME 1
#define HAVE_REALLOCARRAY 0
#define HAVE_RECALLOCARRAY 0
#define HAVE_REWB_BSD 0
#define HAVE_REWB_SYSV 1
#define HAVE_SANDBOX_INIT 0
#define HAVE_STRCASESTR 1
#define HAVE_STRINGLIST 0
#define HAVE_STRLCAT 1
#define HAVE_STRLCPY 1
#define HAVE_STRNDUP 1
#define HAVE_STRPTIME 1
#define HAVE_STRSEP 1
#define HAVE_STRTONUM 1
#define HAVE_SYS_ENDIAN 1
#define HAVE_VASPRINTF 1
#define HAVE_WCHAR 1
#define HAVE_OHASH 0
#define HAVE_CMSG 1
#define HAVE_RECVMSG 1
void *reallocarray(void *, size_t, size_t);
void *recallocarray(void *, size_t, size_t, size_t);

#endif
