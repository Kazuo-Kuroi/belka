/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Memory mapping: protections and flags for mmap(), mprotect(), and the
 * segment drivers, with the System V values. MAP_ANON is not in
 * UnixWare 2, which mapped /dev/zero instead; it takes the value later
 * System V releases gave it.
 */

#ifndef _SYS_MMAN_H_
#define _SYS_MMAN_H_

#define PROT_NONE       0x0
#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4
#define PROT_ALL        (PROT_READ | PROT_WRITE | PROT_EXEC)

#define MAP_SHARED      0x001
#define MAP_PRIVATE     0x002
#define MAP_TYPE        0x00f
#define MAP_FIXED       0x010
#define MAP_ANON        0x100

#define MAP_FAILED      ((void *)-1)

#endif /* _SYS_MMAN_H_ */
