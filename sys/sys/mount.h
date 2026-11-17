/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-16
 * SPDX-License-Identifier: MIT
 */

/*
 * mount(2) and umount(2), System V's:
 *
 *   mount(spec, dir, mflag, fstype, dataptr, datalen)
 *   umount(file)
 *
 * mflag must have MS_DATA, so that fstype names the type ("ext2fs",
 * "tmpfs"); System V's older form, with a type number, is not taken.
 * spec is a disk's block device (/dev/vd1) for ext2fs and ignored for
 * tmpfs; dir must be an absolute path. dataptr and datalen are ignored:
 * no file system takes options yet. umount takes the mount point or,
 * for a disk, its block device. Only the superuser may do either.
 */

#ifndef _SYS_MOUNT_H_
#define _SYS_MOUNT_H_

#define MS_RDONLY       0x01    /* read-only */
#define MS_FSS          0x02    /* old: fstype is a number (refused) */
#define MS_DATA         0x04    /* fstype is a name; data follows */
#define MS_NOSUID       0x10    /* (refused: not yet) */
#define MS_REMOUNT      0x20    /* (refused: not yet) */

#endif /* _SYS_MOUNT_H_ */
