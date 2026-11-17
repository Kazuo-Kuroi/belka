/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * passwd, System V's:
 *
 *   passwd [user]                      change the password
 *   passwd -s [user]                   show its state
 *   passwd [-n min] [-x max] [-w warn] [-f] [-l] [-d] user
 *                                      change its ageing (superuser)
 *
 * A user changes their own password, giving the old one first, and not
 * within min days of the last change; the superuser changes anyone's
 * without either. The new one is asked for twice, hashed with crypt(3)'s
 * "$sha1$" scheme under a new salt, and written to /etc/shadow with the
 * day of the change. /etc/passwd says "x" for the password and is not
 * changed.
 *
 * The superuser's options: -n, -x and -w set the days a password must
 * be kept before it may change, may be kept before it must, and are
 * warned of before that (-1: no limit); -f makes it due at once (the
 * last change, 0, is "now"); -l locks the account (*LK*, which no
 * password matches) and -d empties the password. -s prints the state as
 * System V's passwd does: the name, PS (a password), LK (locked) or NP
 * (none), the last change as mm/dd/yy, and min, max and warn, when set.
 *
 * /etc/shadow is replaced under the password files' lock (lckpwdf, a
 * write lock on /etc/.pwd.lock, which every program changing them
 * takes): a copy is written to /etc/stmp, mode 0400, and renamed over
 * it. Set-user-ID root.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SHADOW_FILE     "/etc/shadow"
#define STMP            "/etc/stmp"     /* the new copy */
#define LOCKED          "*LK*"
#define ITERATIONS      24680           /* the rounds of HMAC-SHA1 */
#define SALTLEN         8
#define UNSET           (-2)            /* an ageing field left alone */

/* The compat library's (usr/lib/compat). */
char *crypt(const char *, const char *);
uint32_t arc4random_uniform(uint32_t);

/* What to change in the user's line; NULL or UNSET: as it is. */
struct change {
        const char      *hash;
        long            lastchg, min, max, warn;
};

static const char itoa64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void
die(const char *msg)
{
        fprintf(stderr, "passwd: %s\n", msg);
        exit(1);
}

static void
usage(void)
{
        fprintf(stderr, "usage: passwd [user]\n"
            "       passwd -s [user]\n"
            "       passwd [-n min] [-x max] [-w warn] [-f] [-l] [-d] "
            "user\n");
        exit(2);
}

static long
today(void)
{
        return (long)(time(NULL) / 86400);
}

/* A setting for crypt(): "$sha1$rounds$salt$", rounds a little varied. */
static void
new_setting(char *buf, size_t len)
{
        char salt[SALTLEN + 1];
        unsigned int rounds = ITERATIONS - arc4random_uniform(ITERATIONS / 4);

        for (int i = 0; i < SALTLEN; i++)
                salt[i] = itoa64[arc4random_uniform(64)];
        salt[SALTLEN] = '\0';
        snprintf(buf, len, "$sha1$%u$%s$", rounds, salt);
}

/* A number of days for -n, -x or -w: 0 or more, or -1 for none. */
static long
days(const char *s)
{
        char *end;
        long v;

        errno = 0;
        v = strtol(s, &end, 10);
        if (*s == '\0' || *end != '\0' || errno != 0 || v < -1)
                die("days must be a number, or -1 for none");
        return v;
}

/* Write an ageing field: empty for -1 (none). */
static void
put_field(FILE *out, long v)
{
        if (v >= 0)
                fprintf(out, "%ld", v);
}

/* An ageing field of a line: -1 if it is empty. */
static long
get_field(const char *s)
{
        return s[0] != '\0' ? atol(s) : -1;
}

/*
 * Write user's line again with the changes: its nine fields are name,
 * password, last change, min, max, warn, inactive, expire, flag.
 */
static void
put_line(FILE *out, char *line, const struct change *c)
{
        char *f[9];
        int n = 0;

        line[strcspn(line, "\n")] = '\0';
        for (char *s = line; n < 9 && s != NULL; n++) {
                f[n] = s;
                if ((s = strchr(s, ':')) != NULL)
                        *s++ = '\0';
        }
        while (n < 9)
                f[n++] = (char *)"";
        fprintf(out, "%s:%s:", f[0], c->hash != NULL ? c->hash : f[1]);
        put_field(out, c->lastchg != UNSET ? c->lastchg : get_field(f[2]));
        fputc(':', out);
        put_field(out, c->min != UNSET ? c->min : get_field(f[3]));
        fputc(':', out);
        put_field(out, c->max != UNSET ? c->max : get_field(f[4]));
        fputc(':', out);
        put_field(out, c->warn != UNSET ? c->warn : get_field(f[5]));
        fprintf(out, ":%s:%s:%s\n", f[6], f[7], f[8]);
}

/*
 * Make the changes to user's line of /etc/shadow; 0, or -1 with a
 * reason. The caller holds the password files' lock.
 */
static int
rewrite(const char *user, const struct change *c, const char **why)
{
        size_t ulen = strlen(user), cap = 0;
        char *line = NULL;
        FILE *in, *out;
        int fd, found = 0;
        ssize_t n;

        (void)unlink(STMP);             /* a copy a crash left */
        if ((fd = open(STMP, O_WRONLY | O_CREAT | O_EXCL, 0400)) < 0) {
                *why = "cannot create " STMP;
                return -1;
        }
        if ((in = fopen(SHADOW_FILE, "r")) == NULL ||
            (out = fdopen(fd, "w")) == NULL) {
                *why = "cannot read " SHADOW_FILE;
                if (in != NULL)
                        fclose(in);
                close(fd);
                unlink(STMP);
                return -1;
        }
        while ((n = getline(&line, &cap, in)) > 0) {
                if (strncmp(line, user, ulen) == 0 && line[ulen] == ':') {
                        put_line(out, line, c);
                        found = 1;
                } else {
                        fputs(line, out);
                }
        }
        free(line);
        fclose(in);
        if (!found) {
                *why = "user not in " SHADOW_FILE;
                fclose(out);
                unlink(STMP);
                return -1;
        }
        if (fflush(out) != 0 || fsync(fileno(out)) != 0 ||
            fchmod(fileno(out), 0400) != 0) {
                *why = "cannot write " STMP;
                fclose(out);
                unlink(STMP);
                return -1;
        }
        fclose(out);
        if (rename(STMP, SHADOW_FILE) != 0) {
                *why = "cannot replace " SHADOW_FILE;
                unlink(STMP);
                return -1;
        }
        return 0;
}

/* Make the changes, under the lock and without interruptions. */
static void
commit(const char *user, const struct change *c)
{
        const char *why;

        signal(SIGHUP, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        if (lckpwdf() != 0)
                die("the password files are busy; try again");
        if (rewrite(user, c, &why) != 0) {
                ulckpwdf();
                die(why);
        }
        ulckpwdf();
}

/* passwd -s: the state of sp, as System V prints it. */
static void
status(const struct spwd *sp)
{
        const char *state = sp->sp_pwdp[0] == '\0' ? "NP" :
            strcmp(sp->sp_pwdp, LOCKED) == 0 ? "LK" : "PS";

        printf("%s %s", sp->sp_namp, state);
        if (sp->sp_lstchg > 0) {
                time_t t = (time_t)sp->sp_lstchg * 86400;
                struct tm tm;
                char when[16];

                gmtime_r(&t, &tm);
                strftime(when, sizeof(when), "%m/%d/%y", &tm);
                printf(" %s", when);
        } else if (sp->sp_lstchg == 0) {
                printf(" 00/00/00");
        }
        if (sp->sp_max >= 0 || sp->sp_min >= 0) {
                printf(" %ld %ld", sp->sp_min, sp->sp_max);
                if (sp->sp_warn >= 0)
                        printf(" %ld", sp->sp_warn);
        }
        printf("\n");
}

/* Ask for the new password twice; its hash ("" for an empty one). */
static const char *
ask_new(uid_t uid)
{
        static char hash[128];
        char setting[64], newpw[256], *p, *h;

        if ((p = getpass("New password:")) == NULL)
                die("cannot read the password");
        if (p[0] == '\0' && uid != 0)
                die("a password must not be empty");
        snprintf(newpw, sizeof(newpw), "%s", p);
        explicit_bzero(p, strlen(p));
        if ((p = getpass("Retype new password:")) == NULL)
                die("cannot read the password");
        if (strcmp(p, newpw) != 0) {
                explicit_bzero(p, strlen(p));
                explicit_bzero(newpw, sizeof(newpw));
                die("they do not match; the password is unchanged");
        }
        explicit_bzero(p, strlen(p));
        if (newpw[0] == '\0') {
                hash[0] = '\0';
        } else {
                new_setting(setting, sizeof(setting));
                if ((h = crypt(newpw, setting)) == NULL)
                        die("cannot hash the password");
                snprintf(hash, sizeof(hash), "%s", h);
        }
        explicit_bzero(newpw, sizeof(newpw));
        return hash;
}

int
main(int argc, char **argv)
{
        struct change c = { NULL, UNSET, UNSET, UNSET, UNSET };
        int ch, sflag = 0, admin = 0;
        struct passwd *pw;
        struct spwd *sp;
        uid_t uid = getuid();
        char *p, *hash;

        while ((ch = getopt(argc, argv, "dfln:sw:x:")) != -1) {
                switch (ch) {
                case 'd':
                        c.hash = "";
                        admin = 1;
                        break;
                case 'f':
                        c.lastchg = 0;
                        admin = 1;
                        break;
                case 'l':
                        c.hash = LOCKED;
                        admin = 1;
                        break;
                case 'n':
                        c.min = days(optarg);
                        admin = 1;
                        break;
                case 's':
                        sflag = 1;
                        break;
                case 'w':
                        c.warn = days(optarg);
                        admin = 1;
                        break;
                case 'x':
                        c.max = days(optarg);
                        admin = 1;
                        break;
                default:
                        usage();
                }
        }
        argc -= optind;
        argv += optind;
        if (argc > 1 || (sflag && admin) || (admin && argc != 1))
                usage();
        pw = argc == 1 ? getpwnam(argv[0]) : getpwuid(uid);
        if (pw == NULL)
                die(argc == 1 ? "unknown user" : "who are you?");
        if (uid != 0 && (admin || pw->pw_uid != uid))
                die("only the superuser may do that");
        if ((sp = getspnam(pw->pw_name)) == NULL)
                die("no entry in " SHADOW_FILE);

        if (sflag) {
                status(sp);
                return 0;
        }
        if (admin) {
                commit(pw->pw_name, &c);
                return 0;
        }

        /* A change of password. */
        printf("Changing the password of %s.\n", pw->pw_name);
        if (uid != 0) {
                if (strcmp(sp->sp_pwdp, LOCKED) == 0)
                        die("the account is locked");
                if (sp->sp_min > 0 && sp->sp_lstchg > 0 &&
                    today() < sp->sp_lstchg + sp->sp_min)
                        die("the password was changed too recently");
        }
        /* The old one, unless the superuser or there is none. */
        if (uid != 0 && sp->sp_pwdp[0] != '\0') {
                if ((p = getpass("Old password:")) == NULL)
                        die("cannot read the password");
                hash = crypt(p, sp->sp_pwdp);
                explicit_bzero(p, strlen(p));
                if (hash == NULL || strcmp(hash, sp->sp_pwdp) != 0) {
                        sleep(2);
                        die("that is not the password");
                }
        }
        c.hash = ask_new(uid);
        c.lastchg = today();
        commit(pw->pw_name, &c);
        printf("The password is changed.\n");
        return 0;
}
