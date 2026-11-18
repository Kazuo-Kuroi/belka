/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

/*
 * Recoverable text passwd/shadow/group updates under the lckpwdf lock.
 * Backups and staged copies stay on the same file system in a root-only
 * directory. A durable ready marker precedes publication. Recovery rolls
 * back unless a committed marker records completion of all renames.
 */

#include <errno.h>
#include <fcntl.h>
#include <shadow.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <accounts.h>

#define JOURNAL "/etc/.accttxn"
#define READY JOURNAL "/ready"
#define COMMITTED JOURNAL "/committed"

const char *const acct_files[ACCT_FILES] = {
        "/etc/passwd", "/etc/shadow", "/etc/group"
};

static void
journal_path(char *path, size_t size, int index, const char *suffix)
{
        snprintf(path, size, JOURNAL "/%d.%s", index, suffix);
}

static int
sync_directory(const char *path)
{
        int fd = open(path, O_RDONLY | O_DIRECTORY);
        int result, saved;

        if (fd < 0)
                return -1;
        result = fsync(fd);
        saved = errno;
        close(fd);
        errno = saved;
        return result;
}

static int
exists(const char *path)
{
        struct stat st;

        if (lstat(path, &st) == 0)
                return 1;
        return errno == ENOENT ? 0 : -1;
}

static int
discard_journal(void)
{
        char path[80];

        for (int i = 0; i < ACCT_FILES; i++) {
                journal_path(path, sizeof(path), i, "new");
                if (unlink(path) != 0 && errno != ENOENT)
                        return -1;
                journal_path(path, sizeof(path), i, "old");
                if (unlink(path) != 0 && errno != ENOENT)
                        return -1;
        }
        if (unlink(READY) != 0 && errno != ENOENT)
                return -1;
        if (unlink(COMMITTED) != 0 && errno != ENOENT)
                return -1;
        if (rmdir(JOURNAL) != 0)
                return -1;
        return sync_directory("/etc");
}

static int
recover(void)
{
        struct stat st;
        char path[80];
        int ready, committed;

        if (lstat(JOURNAL, &st) != 0)
                return errno == ENOENT ? 0 : -1;
        if (!S_ISDIR(st.st_mode) || st.st_uid != 0 ||
            (st.st_mode & 0777) != 0700) {
                errno = EACCES;
                return -1;
        }
        ready = exists(READY);
        committed = exists(COMMITTED);
        if (ready < 0 || committed < 0)
                return -1;
        if (ready && !committed) {
                for (int i = 0; i < ACCT_FILES; i++) {
                        journal_path(path, sizeof(path), i, "old");
                        if (rename(path, acct_files[i]) != 0 &&
                            errno != ENOENT)
                                return -1;
                }
                if (sync_directory("/etc") != 0)
                        return -1;
        }
        return discard_journal();
}

int
acct_lock(void)
{
        int saved;

        if (lckpwdf() != 0)
                return -1;
        if (recover() == 0)
                return 0;
        saved = errno;
        ulckpwdf();
        errno = saved;
        return -1;
}

void
acct_unlock(void)
{
        ulckpwdf();
}

/* Create and persist one complete replacement before publishing anything. */
static int
stage_file(int index, const char *text)
{
        struct stat st;
        char path[80];
        size_t size = strlen(text), written = 0;
        int fd = -1, result = -1, saved;

        if (stat(acct_files[index], &st) != 0)
                return -1;
        journal_path(path, sizeof(path), index, "old");
        if (link(acct_files[index], path) != 0)
                return -1;
        journal_path(path, sizeof(path), index, "new");
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
                return -1;
        while (written < size) {
                ssize_t count = write(fd, text + written, size - written);

                if (count < 0 && errno == EINTR)
                        continue;
                if (count <= 0)
                        goto done;
                written += (size_t)count;
        }
        if (fchown(fd, st.st_uid, st.st_gid) != 0 ||
            fchmod(fd, st.st_mode & 07777) != 0 || fsync(fd) != 0)
                goto done;
        result = 0;
done:
        saved = errno;
        if (close(fd) != 0)
                return -1;
        errno = saved;
        return result;
}

int
acct_commit(const char *const text[ACCT_FILES])
{
        char path[80];
        int fd, saved;

        if (mkdir(JOURNAL, 0700) != 0)
                return -1;
        for (int i = 0; i < ACCT_FILES; i++)
                if (stage_file(i, text[i]) != 0)
                        goto failed;
        fd = open(READY, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
                goto failed;
        if (fsync(fd) != 0) {
                saved = errno;
                close(fd);
                errno = saved;
                goto failed;
        }
        if (close(fd) != 0 || sync_directory(JOURNAL) != 0 ||
            sync_directory("/etc") != 0)
                goto failed;
        for (int i = 0; i < ACCT_FILES; i++) {
                journal_path(path, sizeof(path), i, "new");
                if (rename(path, acct_files[i]) != 0)
                        goto failed;
        }
        if (sync_directory("/etc") != 0 ||
            rename(READY, COMMITTED) != 0 ||
            sync_directory(JOURNAL) != 0)
                goto failed;
        (void)discard_journal();
        return 0;
failed:
        saved = errno;
        if (recover() != 0)
                return -1;
        errno = saved;
        return -1;
}
