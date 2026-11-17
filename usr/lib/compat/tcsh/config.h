/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#ifndef BELKA_TCSH_CONFIG_H
#define BELKA_TCSH_CONFIG_H

/* The target's capabilities, independent of the build host. */
#define POSIX
#define POSIXJOBS
#define BSDJOBS
#define BSDTIMES
#define BSDLIMIT
#define TERMIO
#define SYSVREL 2
#define SHORT_STRINGS
#define GETGROUPS_T gid_t
#define GETPGRP_VOID 1
#define SETPGRP_VOID 1
#define ECHO_STYLE BOTH_ECHO
#define _PATH_TCSHELL "/bin/tcsh"

#define STDC_HEADERS 1
#define HAVE_DECL_CRYPT 1
#define HAVE_DECL_ENVIRON 1
#define HAVE_DECL_GETHOSTNAME 1
#define HAVE_DECL_GETPGRP 1
#define HAVE_DIRENT_H 1
#define HAVE_DUP2 1
#define HAVE_GETCWD 1
#define HAVE_GETHOSTNAME 1
#define HAVE_GETPWENT 1
#define HAVE_INTTYPES_H 1
#define HAVE_LONG_LONG 1
#define HAVE_MEMMOVE 1
#define HAVE_MEMSET 1
#define HAVE_MKSTEMP 1
#define HAVE_NICE 1
#define HAVE_PATHS_H 1
#define HAVE_SETPGID 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRCOLL 1
#define HAVE_STRERROR 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_STRSTR 1
#define HAVE_STRUCT_DIRENT_D_INO 1
#define HAVE_SYSCONF 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WAIT4 1
#define HAVE_WCHAR_H 1
#define HAVE_WCTYPE_H 1
#define SIZEOF_WCHAR_T 4

#include "config_f.h"

/* Belka currently has local sessions only. */
#undef REMOTEHOST

#endif
