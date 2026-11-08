/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-08
 * SPDX-License-Identifier: MIT
 */

/*
 * Pathname limits and lookup, with the UnixWare limits.
 */

#ifndef _SYS_PATHNAME_H_
#define _SYS_PATHNAME_H_

#define MAXPATHLEN      1024    /* including the terminating NUL */
#define MAXNAMELEN      256     /* a component, including the NUL */
#define MAXSYMLINKS     20

/* lookuppn() follow argument. */
#define NO_FOLLOW       0
#define FOLLOW          1

struct vnode;

/*
 * Resolve path from startdir (or the root, for an absolute path or a
 * NULL startdir). With dvpp, also return the parent directory of the
 * last component and its name in lastname (MAXNAMELEN bytes); then a
 * missing last component is not an error, and *vpp is set to NULL. All
 * returned vnodes are held. See vfs_lookup.c for the rules.
 */
int lookuppn(const char *, int, struct vnode *, struct vnode **, char *,
    struct vnode **);
int lookupname(const char *, int, struct vnode **);

#endif /* _SYS_PATHNAME_H_ */
