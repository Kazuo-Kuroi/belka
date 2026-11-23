/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#ifndef _SYS_SOCKET_H_
#define _SYS_SOCKET_H_

#define AF_UNSPEC       0
#define AF_UNIX         1
#define AF_INET         2
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_NONBLOCK   04000
#define SOCK_CLOEXEC    02000000
#define MSG_OOB         0x0001
#define MSG_PEEK        0x0002
#define MSG_TRUNC       0x0020
#define MSG_DONTWAIT    0x0040
#define MSG_NOSIGNAL    0x4000
#define SHUT_RD         0
#define SHUT_WR         1
#define SHUT_RDWR       2

/* Options, at level SOL_SOCKET (numbered as Linux numbers them). */
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_LINGER       13
#define SO_ACCEPTCONN   30

#include <stdint.h>

struct sockaddr_un {
        uint16_t        sun_family;
        char            sun_path[108];
};

struct linger {
        int             l_onoff;
        int             l_linger;       /* seconds */
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
int sockaddr_out(const void *, uint32_t, uint64_t, uint64_t);
int sockopt_out(const void *, uint32_t, uint64_t, uint64_t);
void sock_init(void);
int sockowner_set(struct sockowner *, pid_t, struct cred *);
void sockowner_signal(const struct sockowner *);

/* Sockets over TPI providers (kern/sock_tpi.c). */
struct file;
int tpisock_is(const struct vnode *);
int tpisock_socket(int, int, int, int, struct file **);
int tpisock_bind(struct vnode *, uint64_t, uint64_t);
int tpisock_connect(struct file *, uint64_t, uint64_t);
int tpisock_sendto(struct file *, const uint64_t *, uint64_t *);
int tpisock_recvfrom(struct file *, const uint64_t *, uint64_t *);
int tpisock_name(struct vnode *, int, uint64_t, uint64_t);
int tpisock_listen(struct vnode *, int);
int tpisock_accept(struct file *, const uint64_t *, uint64_t *);
int tpisock_shutdown(struct vnode *, int);
int tpisock_setsockopt(struct vnode *, int, int, uint64_t, uint64_t);
int tpisock_getsockopt(struct vnode *, int, int, uint64_t, uint64_t);
#endif

#endif /* _SYS_SOCKET_H_ */
