/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-24
 * SPDX-License-Identifier: MIT
 *
 * Dropbear's options for Belka, over its defaults
 * (contrib/dropbear/src/default_options.h). The programs are installed
 * under OpenSSH's names: dropbear as /usr/sbin/sshd, dbclient as
 * /usr/bin/ssh, dropbearkey as /usr/bin/ssh-keygen, scp as itself.
 */

#ifndef BELKA_DROPBEAR_LOCALOPTIONS_H
#define BELKA_DROPBEAR_LOCALOPTIONS_H

/* The client, as scp and the server's own -J run it. */
#define DROPBEAR_PATH_SSH_PROGRAM "/usr/bin/ssh"

/* The host keys, in Dropbear's format. */
#define RSA_PRIV_FILENAME       "/etc/dropbear/dropbear_rsa_host_key"
#define ECDSA_PRIV_FILENAME     "/etc/dropbear/dropbear_ecdsa_host_key"
#define ED25519_PRIV_FILENAME   "/etc/dropbear/dropbear_ed25519_host_key"

#define DROPBEAR_PIDFILE        "/var/run/sshd.pid"

/* A session's PATH: /etc/profile's (doc/design/hierarchy.md). */
#define DEFAULT_PATH            "/usr/bin:/opt/bin"
#define DEFAULT_ROOT_PATH       "/sbin:/usr/sbin:/usr/bin:/opt/sbin:/opt/bin"

/*
 * The server stays root until a session's fork, then becomes the user
 * there (svr_switch_user): dropping privileges at authentication needs
 * setresgid, which Belka has not. Forwarding Unix-domain sockets needs
 * the drop, so it is off; TCP forwarding and agent forwarding are not.
 */
#define DROPBEAR_SVR_DROP_PRIVS 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0

/* Not built: X11, re-execution (no fexecve), and an SFTP server. */
#define DROPBEAR_X11FWD         0
#define DROPBEAR_REEXEC         0
#define DROPBEAR_SFTPSERVER     0

#endif
