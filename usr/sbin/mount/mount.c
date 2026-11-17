/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-16
 * SPDX-License-Identifier: MIT
 */

/*
 * mount and umount, System V's, over mount(2) and umount(2) (see
 * <sys/mount.h>), /etc/vfstab and the kernel's mount table, /etc/mnttab.
 *
 *   mount                              list what is mounted
 *   mount [-F type] [-r] [-o opts] special mount_point
 *   mount [-F type] [-r] [-o opts] special | mount_point
 *                                      the rest from /etc/vfstab
 *   mount -a [-F type]                 each /etc/vfstab entry marked to be
 *                                      mounted at boot, in order, but for
 *                                      those already mounted
 *   umount special | mount_point
 *   umount -a                          all but /, /dev and /proc, last
 *                                      mounted first
 *
 * The options are ro and rw (-r is ro). /etc/vfstab has a line for each
 * file system, its fields separated by blanks, "-" for none:
 *
 *   special  fsckdev  mount_point  type  fsckpass  at_boot  options
 *
 * where at_boot is "yes" or "no"; fsckdev and fsckpass are for fsck,
 * which Belka does not have yet. A type of ext2fs needs a disk's block
 * device (/dev/vd1) as special; tmpfs takes any name ("swap").
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VFSTAB          "/etc/vfstab"
#define MNTTAB          "/etc/mnttab"
#define MAXMOUNTS       64

static const char *prog = "mount";

struct vfsent {
        char    *special;
        char    *mountp;
        char    *fstype;
        char    *atboot;
        char    *opts;
};

struct mntent {
        char    special[256];
        char    mountp[1024];
        char    fstype[32];
        char    opts[64];
        long    when;
};

static void
usage(int um)
{
        if (um)
                fprintf(stderr, "usage: umount special | mount_point\n"
                    "       umount -a\n");
        else
                fprintf(stderr, "usage: mount [-F type] [-r] [-o opts] "
                    "special mount_point\n"
                    "       mount [-F type] [-r] [-o opts] special | "
                    "mount_point\n"
                    "       mount -a [-F type]\n");
        exit(2);
}

/* "-" in a table means none. */
static char *
field(char *s)
{
        return strcmp(s, "-") == 0 ? NULL : s;
}

/* The next /etc/vfstab entry, or 0 at the end. */
static int
vfstab_next(FILE *f, char *line, size_t len, struct vfsent *v)
{
        char *f7[7];

        while (fgets(line, (int)len, f) != NULL) {
                int n = 0;

                for (char *s = strtok(line, " \t\n"); s != NULL && n < 7;
                    s = strtok(NULL, " \t\n"))
                        f7[n++] = s;
                if (n == 0 || f7[0][0] == '#')
                        continue;
                if (n < 7) {
                        fprintf(stderr, "%s: %s: an entry needs seven "
                            "fields\n", prog, VFSTAB);
                        continue;
                }
                v->special = field(f7[0]);
                v->mountp = field(f7[2]);
                v->fstype = field(f7[3]);
                v->atboot = field(f7[5]);
                v->opts = field(f7[6]);
                return 1;
        }
        return 0;
}

/* What is mounted, from the kernel's table; the count. */
static int
mnttab_read(struct mntent *m, int max)
{
        char line[1400];
        FILE *f;
        int n = 0;

        if ((f = fopen(MNTTAB, "r")) == NULL) {
                fprintf(stderr, "%s: %s: %s\n", prog, MNTTAB,
                    strerror(errno));
                exit(1);
        }
        while (n < max && fgets(line, sizeof(line), f) != NULL) {
                if (sscanf(line, "%255s %1023s %31s %63s %ld", m[n].special,
                    m[n].mountp, m[n].fstype, m[n].opts, &m[n].when) == 5)
                        n++;
        }
        fclose(f);
        return n;
}

/* Whether something is mounted on mountp. */
static int
mounted_on(const char *mountp)
{
        struct mntent m[MAXMOUNTS];
        int n = mnttab_read(m, MAXMOUNTS);

        for (int i = 0; i < n; i++)
                if (strcmp(m[i].mountp, mountp) == 0)
                        return 1;
        return 0;
}

/* mount -o: ro or rw, separated by commas; sets *rdonly. */
static int
parse_opts(char *opts, int *rdonly)
{
        if (opts == NULL)
                return 0;
        for (char *o = strtok(opts, ","); o != NULL; o = strtok(NULL, ",")) {
                if (strcmp(o, "ro") == 0) {
                        *rdonly = 1;
                } else if (strcmp(o, "rw") == 0) {
                        *rdonly = 0;
                } else {
                        fprintf(stderr, "%s: %s: unknown option\n", prog, o);
                        return -1;
                }
        }
        return 0;
}

static int
do_mount(const char *special, const char *mountp, const char *fstype,
    int rdonly)
{
        struct stat st;

        if (fstype == NULL) {
                /* A disk is ext2; anything else must be named. */
                if (stat(special, &st) == 0 && S_ISBLK(st.st_mode)) {
                        fstype = "ext2fs";
                } else {
                        fprintf(stderr, "%s: %s: give its type with -F\n",
                            prog, special);
                        return 1;
                }
        }
        if (mount(special, mountp, MS_DATA | (rdonly ? MS_RDONLY : 0),
            fstype, NULL, 0) != 0) {
                fprintf(stderr, "%s: %s on %s: %s\n", prog, special, mountp,
                    strerror(errno));
                return 1;
        }
        return 0;
}

/* mount with no operands: the table, as System V prints it. */
static int
list(void)
{
        struct mntent m[MAXMOUNTS];
        int n = mnttab_read(m, MAXMOUNTS);

        for (int i = 0; i < n; i++) {
                time_t t = (time_t)m[i].when;
                char when[32];

                strftime(when, sizeof(when), "%a %b %e %H:%M:%S %Y",
                    localtime(&t));
                printf("%s on %s %s on %s\n", m[i].mountp, m[i].special,
                    strcmp(m[i].opts, "ro") == 0 ? "read-only" :
                    "read/write", when);
        }
        return 0;
}

/* mount -a: /etc/vfstab's boot entries (of type, if given). */
static int
mount_all(const char *type)
{
        char line[1400];
        struct vfsent v;
        FILE *f;
        int status = 0;

        if ((f = fopen(VFSTAB, "r")) == NULL) {
                fprintf(stderr, "%s: %s: %s\n", prog, VFSTAB,
                    strerror(errno));
                return 1;
        }
        while (vfstab_next(f, line, sizeof(line), &v)) {
                int rdonly = 0;

                if (v.atboot == NULL || strcmp(v.atboot, "yes") != 0 ||
                    v.mountp == NULL || v.special == NULL ||
                    (type != NULL &&
                    (v.fstype == NULL || strcmp(v.fstype, type) != 0)))
                        continue;
                if (mounted_on(v.mountp))
                        continue;
                if (parse_opts(v.opts, &rdonly) != 0) {
                        status = 1;
                        continue;
                }
                status |= do_mount(v.special, v.mountp, v.fstype, rdonly);
        }
        fclose(f);
        return status;
}

/* The /etc/vfstab entry whose special or mount point is name. */
static int
vfstab_find(const char *name, char *line, size_t len, struct vfsent *v)
{
        FILE *f;
        int found = 0;

        if ((f = fopen(VFSTAB, "r")) == NULL)
                return 0;
        while (!found && vfstab_next(f, line, len, v))
                found = (v->special != NULL &&
                    strcmp(v->special, name) == 0) ||
                    (v->mountp != NULL && strcmp(v->mountp, name) == 0);
        fclose(f);
        return found;
}

static int
mount_main(int argc, char **argv)
{
        char *type = NULL, *opts = NULL, line[1400];
        int c, aflag = 0, rdonly = 0;
        struct vfsent v;

        while ((c = getopt(argc, argv, "aF:o:r")) != -1) {
                switch (c) {
                case 'a':
                        aflag = 1;
                        break;
                case 'F':
                        type = optarg;
                        break;
                case 'o':
                        opts = optarg;
                        break;
                case 'r':
                        rdonly = 1;
                        break;
                default:
                        usage(0);
                }
        }
        argc -= optind;
        argv += optind;
        if (getuid() != 0 && (aflag || argc > 0)) {
                fprintf(stderr, "%s: only the superuser may mount\n", prog);
                return 1;
        }
        if (aflag) {
                if (argc != 0 || opts != NULL || rdonly)
                        usage(0);
                return mount_all(type);
        }
        if (argc == 0) {
                if (type != NULL || opts != NULL || rdonly)
                        usage(0);
                return list();
        }
        if (parse_opts(opts, &rdonly) != 0)
                return 2;
        if (argc == 2)
                return do_mount(argv[0], argv[1], type, rdonly);
        if (argc != 1)
                usage(0);
        if (!vfstab_find(argv[0], line, sizeof(line), &v) ||
            v.special == NULL || v.mountp == NULL) {
                fprintf(stderr, "%s: %s: not in %s\n", prog, argv[0],
                    VFSTAB);
                return 1;
        }
        if (opts == NULL && parse_opts(v.opts, &rdonly) != 0)
                return 2;
        return do_mount(v.special, v.mountp, type != NULL ? type :
            v.fstype, rdonly);
}

static int
umount_one(const char *name)
{
        if (umount(name) != 0) {
                fprintf(stderr, "%s: %s: %s\n", prog, name, strerror(errno));
                return 1;
        }
        return 0;
}

static int
umount_main(int argc, char **argv)
{
        struct mntent m[MAXMOUNTS];
        int c, aflag = 0, status = 0, n;

        while ((c = getopt(argc, argv, "a")) != -1) {
                if (c == 'a')
                        aflag = 1;
                else
                        usage(1);
        }
        argc -= optind;
        argv += optind;
        if (aflag ? argc != 0 : argc != 1)
                usage(1);
        if (getuid() != 0) {
                fprintf(stderr, "%s: only the superuser may unmount\n", prog);
                return 1;
        }
        if (!aflag)
                return umount_one(argv[0]);
        /*
         * The kernel's table has the root, then the rest newest first:
         * unmounting in its order takes a mount inside another first.
         */
        n = mnttab_read(m, MAXMOUNTS);
        for (int i = 0; i < n; i++) {
                if (strcmp(m[i].mountp, "/") == 0 ||
                    strcmp(m[i].mountp, "/dev") == 0 ||
                    strcmp(m[i].mountp, "/proc") == 0)
                        continue;
                status |= umount_one(m[i].mountp);
        }
        return status;
}

int
main(int argc, char **argv)
{
        const char *base = strrchr(argv[0], '/');

        base = base != NULL ? base + 1 : argv[0];
        if (strcmp(base, "umount") == 0) {
                prog = "umount";
                return umount_main(argc, argv);
        }
        return mount_main(argc, argv);
}
