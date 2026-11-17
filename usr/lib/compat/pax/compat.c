/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <string.h>
#include "config.h"

int
uid_from_user(const char *name, uid_t *uid)
{
        struct passwd *pw = getpwnam(name);

        if (pw == NULL)
                return -1;
        *uid = pw->pw_uid;
        return 0;
}

int
gid_from_group(const char *name, gid_t *gid)
{
        struct group *gr = getgrnam(name);

        if (gr == NULL)
                return -1;
        *gid = gr->gr_gid;
        return 0;
}

int
raise_default_signal(int signal_number)
{
        struct sigaction action;
        sigset_t mask;

        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        sigemptyset(&mask);
        sigaddset(&mask, signal_number);
        if (sigaction(signal_number, &action, NULL) != 0 ||
            sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0)
                return -1;
        return raise(signal_number);
}

/* Symlink modes have no representation on Belka. Never follow the link. */
int
compat_pax_lchmod(const char *path, mode_t mode)
{
        struct stat st;

        if (fstatat(AT_FDCWD, path, &st, AT_SYMLINK_NOFOLLOW) != 0)
                return -1;
        if (S_ISLNK(st.st_mode))
                return 0;
        return fchmodat(AT_FDCWD, path, mode, AT_SYMLINK_NOFOLLOW);
}
