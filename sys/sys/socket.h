/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_SOCKET_H_
#define _SYS_SOCKET_H_

#define AF_UNIX         1
#define SOCK_STREAM     1
#define SOCK_NONBLOCK   04000
#define SOCK_CLOEXEC    02000000
#define MSG_OOB         0x0001
#define MSG_PEEK        0x0002
#define MSG_DONTWAIT    0x0040
#define MSG_NOSIGNAL    0x4000

#include <stdint.h>

struct sockaddr_un {
        uint16_t        sun_family;
        char            sun_path[108];
};

#ifdef _KERNEL
#include <sys/types.h>

struct cred;
struct vnode;

struct sockowner {
        pid_t           so_id;
        uint64_t        so_identity;
        struct cred     *so_cred;
};

int sock_fcntl(struct vnode *, int, int, uint64_t *);
void sock_init(void);
int sockowner_set(struct sockowner *, pid_t, struct cred *);
void sockowner_signal(const struct sockowner *);
#endif

#endif /* _SYS_SOCKET_H_ */
