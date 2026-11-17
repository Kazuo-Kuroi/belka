/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * passwd [user]: change a password. A user changes their own, giving
 * the old one first; the superuser changes anyone's without it. The new
 * one is asked for twice, hashed with crypt(3)'s "$sha1$" scheme under a
 * new salt, and written to /etc/shadow with the day of the change, its
 * third field. /etc/passwd says "x" for the password and is not changed.
 *
 * The file is replaced under the password files' lock (lckpwdf, a write
 * lock on /etc/.pwd.lock, which every program changing them takes): a
 * copy is written to /etc/stmp, mode 0400, and renamed over it.
 * Set-user-ID root.
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
#define ITERATIONS      24680           /* the rounds of HMAC-SHA1 */
#define SALTLEN         8

/* The compat library's (usr/lib/compat). */
char *crypt(const char *, const char *);
uint32_t arc4random_uniform(uint32_t);

static const char itoa64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void
die(const char *msg)
{
        fprintf(stderr, "passwd: %s\n", msg);
        exit(1);
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

/*
 * Replace user's hash in /etc/shadow with hash, and its last change
 * with today (days since 1970); 0, or -1 with a reason. The caller
 * holds the password files' lock.
 */
static int
rewrite(const char *user, const char *hash, const char **why)
{
        size_t ulen = strlen(user), cap = 0;
        long today = (long)(time(NULL) / 86400);
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
                char *rest;

                /* name:password:lastchg:min:max:warn:inactive:expire:flag */
                if (strncmp(line, user, ulen) == 0 && line[ulen] == ':' &&
                    (rest = strchr(line + ulen + 1, ':')) != NULL) {
                        char *after = strchr(rest + 1, ':');

                        fprintf(out, "%s:%s:%ld%s", user, hash, today,
                            after != NULL ? after : "::::::\n");
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

int
main(int argc, char **argv)
{
        char setting[64], newpw[256], *p, *hash;
        const char *why;
        struct passwd *pw;
        struct spwd *sp;
        uid_t uid = getuid();

        if (argc > 2) {
                fprintf(stderr, "usage: passwd [user]\n");
                return 2;
        }
        pw = argc == 2 ? getpwnam(argv[1]) : getpwuid(uid);
        if (pw == NULL)
                die(argc == 2 ? "unknown user" : "who are you?");
        if (uid != 0 && pw->pw_uid != uid)
                die("only the superuser may change another's password");
        if ((sp = getspnam(pw->pw_name)) == NULL)
                die("no entry in " SHADOW_FILE);
        printf("Changing the password of %s.\n", pw->pw_name);

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
                hash = (char *)"";
        } else {
                new_setting(setting, sizeof(setting));
                if ((hash = crypt(newpw, setting)) == NULL)
                        die("cannot hash the password");
        }
        explicit_bzero(newpw, sizeof(newpw));

        /* No interruptions while the file is replaced. */
        signal(SIGHUP, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        if (lckpwdf() != 0)
                die("the password files are busy; try again");
        if (rewrite(pw->pw_name, hash, &why) != 0) {
                ulckpwdf();
                die(why);
        }
        ulckpwdf();
        printf("The password is changed.\n");
        return 0;
}
