/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * File access flags.
 */

#ifndef _SYS_FILE_H_
#define _SYS_FILE_H_

/* Open flags, as the kernel sees them (System V values). */
#define FREAD           0x0001
#define FWRITE          0x0002
#define FAPPEND         0x0008
#define FCREAT          0x0100
#define FTRUNC          0x0200
#define FEXCL           0x0400

#endif /* _SYS_FILE_H_ */
