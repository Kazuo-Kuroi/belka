/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 *
 * Dropbear's configuration for Belka, written by hand in place of the
 * one its configure script makes (contrib/dropbear/src/config.h.in),
 * from what mlibc and the compat library have.
 *
 * Left out on purpose: setresgid (mlibc's has no Belka system call
 * under it), so the server keeps root until the session's fork
 * (localoptions.h); sys/prctl.h, which on Belka is IRIX's prctl, not
 * Linux's; stropts.h, as ptys come from openpty; fexecve, so no
 * re-execution; getrandom, so the seed is read from /dev/urandom;
 * zlib, PAM, lastlog and the old utmp and wtmp. Sessions are recorded
 * in utmpx and wtmpx only.
 */

#ifndef BELKA_DROPBEAR_CONFIG_H
#define BELKA_DROPBEAR_CONFIG_H

#define PACKAGE_NAME            "dropbear"
#define PACKAGE_TARNAME         "dropbear"
#define PACKAGE_VERSION         "2026.94"
#define PACKAGE_STRING          "dropbear 2026.94"
#define PACKAGE_BUGREPORT       ""
#define PACKAGE_URL             ""

#define BUNDLED_LIBTOM          1

#define DISABLE_ZLIB            1
#define DISABLE_PAM             1
#define DISABLE_LASTLOG         1
#define DISABLE_UTMP            1
#define DISABLE_WTMP            1
#define DISABLE_PUTUTLINE       1

#define CONF_UTMPX_FILE         "/var/adm/utmpx"
#define CONF_WTMPX_FILE         "/var/adm/wtmpx"

/* Headers. */
#define STDC_HEADERS            1
#define HAVE_ENDIAN_H           1
#define HAVE_INTTYPES_H         1
#define HAVE_LIBGEN_H           1
#define HAVE_NETDB_H            1
#define HAVE_NETINET_IN_H       1
#define HAVE_NETINET_IN_SYSTM_H 1
#define HAVE_NETINET_TCP_H      1
#define HAVE_PATHS_H            1
#define HAVE_PTY_H              1
#define HAVE_SHADOW_H           1
#define HAVE_STDINT_H           1
#define HAVE_STDIO_H            1
#define HAVE_STDLIB_H           1
#define HAVE_STRINGS_H          1
#define HAVE_STRING_H           1
#define HAVE_SYS_SELECT_H       1
#define HAVE_SYS_SOCKET_H       1
#define HAVE_SYS_STAT_H         1
#define HAVE_SYS_TYPES_H        1
#define HAVE_SYS_UIO_H          1
#define HAVE_SYS_WAIT_H         1
#define HAVE_UNISTD_H           1
#define HAVE_UTMPX_H            1

/* Types. */
#define HAVE_UINT8_T            1
#define HAVE_UINT16_T           1
#define HAVE_UINT32_T           1
#define HAVE_U_INT8_T           1
#define HAVE_U_INT16_T          1
#define HAVE_U_INT32_T          1
#define HAVE_STRUCT_ADDRINFO    1
#define HAVE_STRUCT_IN6_ADDR    1
#define HAVE_STRUCT_SOCKADDR_IN6 1
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_STRUCT_SOCKADDR_STORAGE_SS_FAMILY 1
#define HAVE_STRUCT_UTMPX_UT_ADDR_V6 1
#define HAVE_STRUCT_UTMPX_UT_HOST 1
#define HAVE_STRUCT_UTMPX_UT_ID 1
#define HAVE_STRUCT_UTMPX_UT_TV 1
#define HAVE_STRUCT_UTMPX_UT_TYPE 1
#define HAVE_STATIC_ASSERT      1
#define HAVE_DECL_HTOLE64       1
#define HAVE_CONST_GAI_STRERROR_PROTO 1

/* Functions. */
#define HAVE_BASENAME           1
#define HAVE_CLEARENV           1
#define HAVE_CLOCK_GETTIME      1
#define HAVE_CRYPT              1
#define HAVE_DAEMON             1
#define HAVE_EXPLICIT_BZERO     1
#define HAVE_FORK               1
#define HAVE_FREEADDRINFO       1
#define HAVE_GAI_STRERROR       1
#define HAVE_GETADDRINFO        1
#define HAVE_GETGROUPLIST       1
#define HAVE_GETNAMEINFO        1
#define HAVE_GETPASS            1
#define HAVE_GETSPNAM           1
#define HAVE_GETUSERSHELL       1
#define HAVE_ENDUTXENT          1
#define HAVE_GETUTXENT          1
#define HAVE_GETUTXID           1
#define HAVE_GETUTXLINE         1
#define HAVE_PUTUTXLINE         1
#define HAVE_SETUTXENT          1
#define HAVE_UTMPXNAME          1
#define HAVE_OPENPTY            1
#define HAVE_PUTENV             1
#define HAVE_STRLCAT            1
#define HAVE_STRLCPY            1
#define HAVE_WRITEV             1

#define SELECT_TYPE_ARG1        int
#define SELECT_TYPE_ARG234      (fd_set *)
#define SELECT_TYPE_ARG5        (struct timeval *)

/* crypt(3) is the compat library's, which has no header of its own. */
char *crypt(const char *, const char *);

#endif
