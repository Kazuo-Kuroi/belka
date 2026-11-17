/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * open() flags and fcntl() commands, with the System V values. The
 * kernel's own file flags (<sys/file.h>) are the open flags plus one in
 * the access-mode bits, as in System V.
 */

#ifndef _SYS_FCNTL_H_
#define _SYS_FCNTL_H_

#define O_RDONLY        0x0000
#define O_WRONLY        0x0001
#define O_RDWR          0x0002
#define O_ACCMODE       0x0003
#define O_NDELAY        0x0004
#define O_APPEND        0x0008
#define O_SYNC          0x0010
#define O_NONBLOCK      0x0080
#define O_CREAT         0x0100
#define O_TRUNC         0x0200
#define O_EXCL          0x0400
#define O_NOCTTY        0x0800

#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUP2FD        9
#define F_GETOWN        23
#define F_SETOWN        24

#define FD_CLOEXEC      1

/*
 * The *at calls: a directory descriptor meaning the current directory,
 * and their flags. Belka's own; UnixWare 2 has no *at calls.
 */
#define AT_FDCWD                (-100)
#define AT_REMOVEDIR            0x1     /* unlinkat: remove a directory */
#define AT_SYMLINK_FOLLOW       0x2     /* linkat: follow a last link */
#define AT_EACCESS              0x4     /* faccessat: effective ids */
#define AT_SYMLINK_NOFOLLOW     0x1000  /* do not follow a last link */

#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

#endif /* _SYS_FCNTL_H_ */
