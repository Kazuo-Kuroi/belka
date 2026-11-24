/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-13
 * SPDX-License-Identifier: MIT
 */

/*
 * The credentials test, built against mlibc: user and group ids with
 * saved ids (and setresuid and setresgid), supplementary groups, the file permission rules (search,
 * read and write, directory writes, sticky directories, new files'
 * owners, set-group-ID directories, chmod and chown), access() with the
 * real ids, set-ID programs and AT_SECURE, and who may send a signal or
 * raise a hard limit.
 *
 * It starts as the superuser, sets files up in dir/credtest.d, and does
 * the checks as user 100 (groups 100 and 20) in child processes.
 *
 * usage: credtest dir, where dir holds bin/credtest and is writable.
 *        credtest ids ruid euid rgid egid secure: exit 0 if the program
 *        runs with those ids and AT_SECURE value (the set-ID check).
 * Exits with the number of failed checks.
 */

#define _DEFAULT_SOURCE         /* setgroups */
#define _POSIX_C_SOURCE 202405L
#define _XOPEN_SOURCE 800       /* setresuid and the rest, in mlibc */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define USER    100             /* the unprivileged user and its group */
#define GROUP2  20              /* its supplementary group */
#define SUID    200             /* the set-ID program's owner and group */
#define SGID    30
#define DIRGID  55              /* the set-group-ID directory's group */

static int failures;
static const char *dir;

#define CHECK(expr)                                                     \
        do {                                                            \
                if (!(expr)) {                                          \
                        printf("credtest: %d: %s\n", __LINE__, #expr);  \
                        failures++;                                     \
                }                                                       \
        } while (0)

static char *
path(const char *rel)
{
        static char buf[4][256];
        static int next;
        char *p = buf[next++ % 4];

        snprintf(p, 256, "%s/credtest.d%s", dir, rel);
        return p;
}

/* Make file rel with owner, group, and mode (and some contents). */
static void
make_file(const char *rel, uid_t uid, gid_t gid, mode_t mode)
{
        int fd = open(path(rel), O_WRONLY | O_CREAT | O_EXCL, 0600);

        CHECK(fd >= 0);
        CHECK(write(fd, "data\n", 5) == 5);
        CHECK(close(fd) == 0);
        CHECK(chown(path(rel), uid, gid) == 0);
        CHECK(chmod(path(rel), mode) == 0);
}

static void
make_dir(const char *rel, uid_t uid, gid_t gid, mode_t mode)
{
        CHECK(mkdir(path(rel), 0700) == 0);
        CHECK(chown(path(rel), uid, gid) == 0);
        CHECK(chmod(path(rel), mode) == 0);
}

static mode_t
mode_of(const char *p)
{
        struct stat st;

        return stat(p, &st) == 0 ? st.st_mode & 07777 : (mode_t)-1;
}

static gid_t
gid_of(const char *p)
{
        struct stat st;

        return stat(p, &st) == 0 ? st.st_gid : (gid_t)-1;
}

/* Copy this program to rel, for a set-ID copy. */
static void
copy_self(const char *rel)
{
        char from[256], buf[4096];
        int in, out;
        ssize_t n;

        snprintf(from, sizeof(from), "%s/bin/credtest", dir);
        in = open(from, O_RDONLY);
        out = open(path(rel), O_WRONLY | O_CREAT | O_EXCL, 0755);
        CHECK(in >= 0 && out >= 0);
        while ((n = read(in, buf, sizeof(buf))) > 0)
                CHECK(write(out, buf, (size_t)n) == n);
        CHECK(n == 0);
        close(in);
        close(out);
}

/* Run fn in a child process; returns its failure count. */
static int
in_child(void (*fn)(void))
{
        int status;
        pid_t pid;

        fflush(stdout);
        if ((pid = fork()) == 0) {
                failures = 0;
                fn();
                fflush(stdout);
                _exit(failures);
        }
        if (pid < 0 || waitpid(pid, &status, 0) != pid ||
            !WIFEXITED(status))
                return 1;
        return WEXITSTATUS(status);
}

/* Become user 100, in groups 100 and 20. */
static void
become_user(void)
{
        gid_t groups[] = { GROUP2 };

        CHECK(setgroups(1, groups) == 0);
        CHECK(setgid(USER) == 0);
        CHECK(setuid(USER) == 0);
}

/* Ids and groups as the superuser, then as the user. */
static void
user_ids(void)
{
        gid_t g[4];

        become_user();
        CHECK(getuid() == USER && geteuid() == USER);
        CHECK(getgid() == USER && getegid() == USER);
        CHECK(getgroups(0, NULL) == 1);
        CHECK(getgroups(4, g) == 1 && g[0] == GROUP2);
        /* Saved id 100 as well: there is no way back. */
        CHECK(setuid(0) == -1 && errno == EPERM);
        CHECK(seteuid(0) == -1 && errno == EPERM);
        CHECK(setgid(0) == -1 && errno == EPERM);
        CHECK(setegid(GROUP2) == -1 && errno == EPERM);
        CHECK(setuid(USER) == 0 && seteuid(USER) == 0);
        CHECK(setgroups(0, NULL) == -1 && errno == EPERM);
}

/* The effective id away from 0 and back, with access() on real ids. */
static void
saved_ids(void)
{
        CHECK(seteuid(USER) == 0);
        CHECK(getuid() == 0 && geteuid() == USER);
        /* The file is root's, mode 0600. */
        CHECK(open(path("/rootonly"), O_RDONLY) == -1 && errno == EACCES);
        CHECK(access(path("/rootonly"), R_OK) == 0);
        CHECK(faccessat(AT_FDCWD, path("/rootonly"), R_OK, AT_EACCESS) ==
            -1 && errno == EACCES);
        CHECK(seteuid(0) == 0 && geteuid() == 0);
        CHECK(open(path("/rootonly"), O_RDONLY) >= 0);
        /* The superuser's setuid sets all three. */
        CHECK(setuid(USER) == 0);
        CHECK(getuid() == USER && geteuid() == USER);
        CHECK(seteuid(0) == -1 && errno == EPERM);
        CHECK(access(path("/rootonly"), R_OK) == -1 && errno == EACCES);
}

/*
 * setresuid and setresgid (POSIX 2024), and getresuid and getresgid:
 * the superuser sets any of the three, -1 leaving one as it is; then,
 * as the user, a saved group kept for a later setegid (as an SSH
 * server keeps utmp's), the saved user id 0 taken back as effective,
 * and nothing outside the three allowed.
 */
static void
res_ids(void)
{
        uid_t r, e, s;
        gid_t rg, eg, sg;

        CHECK(getresuid(&r, &e, &s) == 0 && r == 0 && e == 0 && s == 0);
        CHECK(setresuid(0, (uid_t)0x80000000, 0) == -1 && errno == EINVAL);
        CHECK(getresuid(NULL, &e, &s) == -1 && errno == EFAULT);
        CHECK(setresgid(USER, USER, GROUP2) == 0);
        CHECK(getresgid(&rg, &eg, &sg) == 0 && rg == USER && eg == USER &&
            sg == GROUP2);
        CHECK(setresuid(USER, USER, (uid_t)-1) == 0);
        CHECK(getresuid(&r, &e, &s) == 0 && r == USER && e == USER &&
            s == 0);
        /* No longer the superuser. */
        CHECK(setresgid((gid_t)-1, 0, (gid_t)-1) == -1 && errno == EPERM);
        CHECK(setegid(GROUP2) == 0 && getegid() == GROUP2);
        CHECK(setegid(USER) == 0 && getegid() == USER);
        CHECK(setresgid(GROUP2, (gid_t)-1, USER) == 0);
        CHECK(getresgid(&rg, &eg, &sg) == 0 && rg == GROUP2 && eg == USER &&
            sg == USER);
        CHECK(setresuid((uid_t)-1, 0, (uid_t)-1) == 0 && geteuid() == 0);
        CHECK(setresuid(USER, USER, USER) == 0);
        CHECK(setresuid((uid_t)-1, 0, (uid_t)-1) == -1 && errno == EPERM);
        CHECK(getresuid(&r, &e, &s) == 0 && r == USER && e == USER &&
            s == USER);
}

/* Reading and writing by owner, group, and others. */
static void
file_modes(void)
{
        int fd;

        become_user();
        CHECK((fd = open(path("/mine"), O_RDWR)) >= 0);
        close(fd);
        CHECK((fd = open(path("/grp"), O_RDONLY)) >= 0);
        close(fd);
        CHECK(open(path("/grp"), O_WRONLY) == -1 && errno == EACCES);
        CHECK((fd = open(path("/other"), O_RDONLY)) >= 0);
        close(fd);
        CHECK(open(path("/other"), O_RDWR) == -1 && errno == EACCES);
        CHECK(open(path("/rootonly"), O_RDONLY) == -1 && errno == EACCES);
        CHECK(access(path("/grp"), R_OK) == 0);
        CHECK(access(path("/grp"), W_OK) == -1 && errno == EACCES);
        CHECK(truncate(path("/grp"), 0) == -1 && errno == EACCES);
        CHECK(truncate(path("/mine"), 2) == 0);
        /* A file no one may execute; the user may not run its own 0644. */
        CHECK(access(path("/mine"), X_OK) == -1 && errno == EACCES);

        /* A directory it may not search hides what is in it. */
        CHECK(open(path("/closed/inside"), O_RDONLY) == -1 &&
            errno == EACCES);
        CHECK(access(path("/closed/inside"), F_OK) == -1 && errno == EACCES);
        CHECK(chdir(path("/closed")) == -1 && errno == EACCES);

        /* The top directory is root's 0755: no new names, no removals. */
        CHECK(open(path("/new"), O_WRONLY | O_CREAT, 0644) == -1 &&
            errno == EACCES);
        CHECK(mkdir(path("/newdir"), 0755) == -1 && errno == EACCES);
        CHECK(unlink(path("/mine")) == -1 && errno == EACCES);
        CHECK(rename(path("/mine"), path("/mine2")) == -1 &&
            errno == EACCES);
        CHECK(symlink("x", path("/link")) == -1 && errno == EACCES);
        CHECK(link(path("/mine"), path("/link")) == -1 && errno == EACCES);
        CHECK(mkfifo(path("/fifo"), 0644) == -1 && errno == EACCES);
}

/* Sticky and set-group-ID directories; a new file's owner. */
static void
directories(void)
{
        struct stat st;
        int fd;

        become_user();
        /* A new file in a writable directory: mode 0444 still opens. */
        CHECK((fd = open(path("/tmp/mine"), O_RDWR | O_CREAT, 0444)) >= 0);
        CHECK(write(fd, "x", 1) == 1);
        close(fd);
        CHECK(stat(path("/tmp/mine"), &st) == 0 && st.st_uid == USER &&
            st.st_gid == USER);
        /* tmp is sticky: root's file stays, the user's own goes. */
        CHECK(unlink(path("/tmp/roots")) == -1 && errno == EPERM);
        CHECK(rename(path("/tmp/roots"), path("/tmp/x")) == -1 &&
            errno == EPERM);
        CHECK(rename(path("/tmp/mine"), path("/tmp/mine2")) == 0);
        CHECK(rename(path("/tmp/mine2"), path("/tmp/roots")) == -1 &&
            errno == EPERM);
        CHECK(unlink(path("/tmp/mine2")) == 0);

        /* sgid is set-group-ID, group 55: new names take its group. */
        CHECK((fd = open(path("/sgid/f"), O_WRONLY | O_CREAT, 02644)) >= 0);
        close(fd);
        CHECK(gid_of(path("/sgid/f")) == DIRGID);
        /* Not in group 55: asking for set-group-ID on it is ignored. */
        CHECK((mode_of(path("/sgid/f")) & S_ISGID) == 0);
        CHECK(mkdir(path("/sgid/d"), 0755) == 0);
        CHECK(gid_of(path("/sgid/d")) == DIRGID);
        CHECK((mode_of(path("/sgid/d")) & S_ISGID) != 0);
        CHECK(rmdir(path("/sgid/d")) == 0);
        CHECK(unlink(path("/sgid/f")) == 0);
}

/* chmod and chown by the owner and by others. */
static void
owners(void)
{
        int fd;

        become_user();
        CHECK(chmod(path("/grp"), 0666) == -1 && errno == EPERM);
        CHECK(chown(path("/grp"), USER, USER) == -1 && errno == EPERM);
        CHECK((fd = open(path("/tmp/own"), O_WRONLY | O_CREAT, 0644)) >=
            0);
        close(fd);
        CHECK(chmod(path("/tmp/own"), 0600) == 0);
        CHECK(mode_of(path("/tmp/own")) == 0600);
        /* The sticky bit is for directories, unless root sets it. */
        CHECK(chmod(path("/tmp/own"), 01644) == 0);
        CHECK(mode_of(path("/tmp/own")) == 0644);
        /* Its own groups only, and never another owner. */
        CHECK(chown(path("/tmp/own"), (uid_t)-1, GROUP2) == 0);
        CHECK(gid_of(path("/tmp/own")) == GROUP2);
        CHECK(chown(path("/tmp/own"), (uid_t)-1, 99) == -1 &&
            errno == EPERM);
        CHECK(chown(path("/tmp/own"), 0, (gid_t)-1) == -1 &&
            errno == EPERM);
        /* Set-group-ID on a group it is in stays; chown takes it. */
        CHECK(chmod(path("/tmp/own"), 02755) == 0);
        CHECK(mode_of(path("/tmp/own")) == 02755);
        CHECK(chown(path("/tmp/own"), USER, USER) == 0);
        CHECK(mode_of(path("/tmp/own")) == 0755);
        CHECK(unlink(path("/tmp/own")) == 0);

        /* Its write takes the set-ID bits of root's world-writable file. */
        CHECK((fd = open(path("/setid-w"), O_WRONLY)) >= 0);
        CHECK(write(fd, "x", 1) == 1);
        close(fd);
        CHECK(mode_of(path("/setid-w")) == 0666);
}

/* Signals and limits. */
static void
privileges(void)
{
        struct rlimit rl;

        become_user();
        CHECK(kill(getppid(), 0) == -1 && errno == EPERM);
        CHECK(kill(getpid(), 0) == 0);
        CHECK(kill(-1, 0) == 0);        /* itself, at least */
        CHECK(getrlimit(RLIMIT_CORE, &rl) == 0);
        rl.rlim_max = 1024;
        rl.rlim_cur = 1024;
        CHECK(setrlimit(RLIMIT_CORE, &rl) == 0);
        rl.rlim_max = 2048;
        CHECK(setrlimit(RLIMIT_CORE, &rl) == -1 && errno == EPERM);
}

/* Run the set-ID copy, which checks its own ids. */
static void
setid_exec(void)
{
        char *argv[8];
        char a[5][16];
        int status;
        pid_t pid;

        become_user();
        snprintf(a[0], 16, "%d", USER);
        snprintf(a[1], 16, "%d", SUID);
        snprintf(a[2], 16, "%d", USER);
        snprintf(a[3], 16, "%d", SGID);
        snprintf(a[4], 16, "1");
        argv[0] = "credtest";
        argv[1] = "ids";
        for (int i = 0; i < 5; i++)
                argv[2 + i] = a[i];
        argv[7] = NULL;
        fflush(stdout);
        if ((pid = fork()) == 0) {
                execv(path("/setid"), argv);
                _exit(99);
        }
        CHECK(pid > 0 && waitpid(pid, &status, 0) == pid &&
            WIFEXITED(status) && WEXITSTATUS(status) == 0);

        /* The user may not write the program. */
        CHECK(open(path("/setid"), O_WRONLY) == -1 && errno == EACCES);
}

/* credtest ids ...: whether this process runs with the given ids. */
static int
check_ids(char **argv)
{
        unsigned long secure = getauxval(AT_SECURE);
        int ok = getuid() == (uid_t)atoi(argv[0]) &&
            geteuid() == (uid_t)atoi(argv[1]) &&
            getgid() == (gid_t)atoi(argv[2]) &&
            getegid() == (gid_t)atoi(argv[3]) &&
            secure == (unsigned long)atoi(argv[4]);

        if (!ok)
                printf("credtest: ids %d %d %d %d, AT_SECURE %lu\n",
                    (int)getuid(), (int)geteuid(), (int)getgid(),
                    (int)getegid(), secure);
        /* The saved id is the effective one: it may switch back. */
        if (ok && (seteuid(getuid()) != 0 ||
            seteuid((uid_t)atoi(argv[1])) != 0))
                ok = 0;
        return ok ? 0 : 1;
}

static void
setup(void)
{
        char p[256];

        snprintf(p, sizeof(p), "%s/credtest.d", dir);
        CHECK(mkdir(p, 0755) == 0);
        make_file("/mine", USER, USER, 0644);
        make_file("/grp", 0, GROUP2, 0640);
        make_file("/other", 0, 0, 0604);
        make_file("/rootonly", 0, 0, 0600);
        make_file("/setid-w", 0, 0, 06666);
        CHECK(mode_of(path("/setid-w")) == 06666);
        make_dir("/closed", 0, 0, 0700);
        make_file("/closed/inside", 0, 0, 0644);
        make_dir("/tmp", 0, 0, 01777);
        make_file("/tmp/roots", 0, 0, 0644);
        make_dir("/sgid", 0, DIRGID, 02777);
        CHECK((mode_of(path("/sgid")) & S_ISGID) != 0);
        copy_self("/setid");
        CHECK(chown(path("/setid"), SUID, SGID) == 0);
        CHECK(chmod(path("/setid"), 06755) == 0);
}

static void
cleanup(void)
{
        static const char *const files[] = {
                "/mine", "/grp", "/other", "/rootonly", "/setid-w",
                "/closed/inside", "/tmp/roots", "/setid"
        };
        static const char *const dirs[] = { "/closed", "/tmp", "/sgid" };
        char p[256];

        for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
                CHECK(unlink(path(files[i])) == 0);
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
                CHECK(rmdir(path(dirs[i])) == 0);
        snprintf(p, sizeof(p), "%s/credtest.d", dir);
        CHECK(rmdir(p) == 0);
}

int
main(int argc, char **argv)
{
        gid_t g[4] = { 7, 8 };

        if (argc == 7 && strcmp(argv[1], "ids") == 0)
                return check_ids(argv + 2);
        if (argc != 2) {
                printf("usage: credtest dir\n");
                return 1;
        }
        dir = argv[1];

        CHECK(getuid() == 0 && geteuid() == 0);
        CHECK(getgid() == 0 && getegid() == 0);
        CHECK(getgroups(0, NULL) == 0);
        CHECK(setgroups(2, g) == 0);
        g[0] = g[1] = 0;
        CHECK(getgroups(0, NULL) == 2);
        CHECK(getgroups(1, g) == -1 && errno == EINVAL);
        CHECK(getgroups(4, g) == 2 && g[0] == 7 && g[1] == 8);
        CHECK(setgroups(17, g) == -1 && errno == EINVAL);
        CHECK(setgroups(0, NULL) == 0 && getgroups(0, NULL) == 0);
        CHECK(setuid((uid_t)-1) == -1 && errno == EINVAL);

        setup();
        failures += in_child(user_ids);
        failures += in_child(saved_ids);
        failures += in_child(res_ids);
        failures += in_child(file_modes);
        failures += in_child(directories);
        failures += in_child(owners);
        failures += in_child(privileges);
        failures += in_child(setid_exec);
        cleanup();

        printf("credtest: %d failures\n", failures);
        return failures;
}
