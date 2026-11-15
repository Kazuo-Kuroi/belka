/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-14
 * SPDX-License-Identifier: MIT
 */

/*
 * passwd [user]: change a password. A user changes their own, giving
 * the old one first; the superuser changes anyone's without it. The new
 * one is asked for twice, hashed with crypt(3)'s "$sha1$" scheme under a
 * new salt, and written to /etc/passwd, which keeps the hashes for now
 * (there is no shadow file): a copy is written to /etc/ptmp, which is
 * also the lock, and renamed over it. Set-user-ID root.
 */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PASSWD          "/etc/passwd"
#define PTMP            "/etc/ptmp"     /* the new copy, and the lock */
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

/* Replace user's hash in /etc/passwd with hash; 0, or -1 with a reason. */
static int
rewrite(const char *user, const char *hash, const char **why)
{
        size_t ulen = strlen(user), cap = 0;
        char *line = NULL;
        struct stat st;
        FILE *in, *out;
        int fd, found = 0;
        ssize_t n;

        if ((fd = open(PTMP, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0) {
                *why = errno == EEXIST ? PTMP " exists: the password file "
                    "is busy; try again" : "cannot create " PTMP;
                return -1;
        }
        if ((in = fopen(PASSWD, "r")) == NULL ||
            (out = fdopen(fd, "w")) == NULL) {
                *why = "cannot read " PASSWD;
                close(fd);
                unlink(PTMP);
                return -1;
        }
        while ((n = getline(&line, &cap, in)) > 0) {
                char *colon;

                if (strncmp(line, user, ulen) == 0 && line[ulen] == ':' &&
                    (colon = strchr(line + ulen + 1, ':')) != NULL) {
                        fprintf(out, "%s:%s%s", user, hash, colon);
                        found = 1;
                } else {
                        fputs(line, out);
                }
        }
        free(line);
        fclose(in);
        if (!found) {
                *why = "user not in " PASSWD;
                fclose(out);
                unlink(PTMP);
                return -1;
        }
        if (fflush(out) != 0 || fsync(fileno(out)) != 0 ||
            fstat(fileno(out), &st) != 0) {
                *why = "cannot write " PTMP;
                fclose(out);
                unlink(PTMP);
                return -1;
        }
        fclose(out);
        if (rename(PTMP, PASSWD) != 0) {
                *why = "cannot replace " PASSWD;
                unlink(PTMP);
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
        printf("Changing the password of %s.\n", pw->pw_name);

        /* The old one, unless the superuser or there is none. */
        if (uid != 0 && pw->pw_passwd[0] != '\0') {
                if ((p = getpass("Old password:")) == NULL)
                        die("cannot read the password");
                hash = crypt(p, pw->pw_passwd);
                explicit_bzero(p, strlen(p));
                if (hash == NULL || strcmp(hash, pw->pw_passwd) != 0) {
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
        if (rewrite(pw->pw_name, hash, &why) != 0)
                die(why);
        printf("The password is changed.\n");
        return 0;
}
