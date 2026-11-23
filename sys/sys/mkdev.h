/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * Device numbers, with System V's names: the major number in the high
 * 32 bits of a dev_t, the minor in the low 32. The majors are Belka's.
 */

#ifndef _SYS_MKDEV_H_
#define _SYS_MKDEV_H_

#include <stdint.h>

#include <sys/types.h>

#define makedev(maj, min)       (((dev_t)(maj) << 32) | (uint32_t)(min))
#define major(dev)              ((uint32_t)((dev) >> 32))
#define minor(dev)              ((uint32_t)(dev))

#define DEV_CONS_MAJOR          1       /* the console */
#define DEV_MEM_MAJOR           2       /* null (0), zero (1) */
#define DEV_TTY_MAJOR           3       /* /dev/tty */
#define DEV_PTM_MAJOR           4       /* pseudo-terminal masters */
#define DEV_PTS_MAJOR           5       /* and slaves; minor: the index */
#define DEV_BLK_MAJOR           6       /* disks; minor: the blkdev index */
#define DEV_STRLOOP_MAJOR       7       /* STREAMS loopback clone */
#define DEV_STRMUX_MAJOR        8       /* STREAMS multiplexor clone (tests) */
#define DEV_VIONET_MAJOR        9       /* virtio-net DLPI; unit in the minor */

#endif /* _SYS_MKDEV_H_ */
