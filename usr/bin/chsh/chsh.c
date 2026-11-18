/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

/* Change the shell in System V's text passwd file under acct_lock(). */

#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <shadow.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../lib/compat/include/accounts.h"

char *crypt(const char *, const char *);

static void
die(const char *message)
{
        fprintf(stderr, "chsh: %s\n", message);
        exit(1);
}

static int
listed(const char *shell)
{
        char *entry;
        int found = 0;

        setusershell();
        while ((entry = getusershell()) != NULL)
                if (strcmp(entry, shell) == 0)
                        found = 1;
        endusershell();
        return found;
}

/* Copy every other field and account verbatim, including a final partial line. */
static int
rewrite(const char *user, uid_t uid, const char *shell, uid_t caller)
{
        FILE *in = NULL, *out = NULL;
        struct stat st;
        char *line = NULL;
        char temporary[] = "/etc/ptmp.XXXXXX";
        size_t size = 0, length = strlen(user);
        ssize_t count;
        int fd = -1, created = 0, found = 0, result = -1;

        in = fopen("/etc/passwd", "r");
        if (in == NULL || fstat(fileno(in), &st) != 0)
                goto done;
        fd = mkstemp(temporary);
        if (fd < 0)
                goto done;
        created = 1;
        out = fdopen(fd, "w");
        if (out == NULL)
                goto done;
        fd = -1;
        while ((count = getline(&line, &size, in)) >= 0) {
                if (strncmp(line, user, length) == 0 && line[length] == ':') {
                        char *field[7], *cursor = line, *end;
                        unsigned long current_uid;

                        if (found++)
                                goto done;
                        for (int i = 0; i < 7; i++) {
                                field[i] = strsep(&cursor, ":");
                                if (field[i] == NULL)
                                        goto done;
                        }
                        if (cursor != NULL)
                                goto done;
                        errno = 0;
                        current_uid = strtoul(field[2], &end, 10);
                        if (errno != 0 || field[2][0] == '\0' || *end != '\0' ||
                            current_uid != uid)
                                goto done;
                        field[6][strcspn(field[6], "\r\n")] = '\0';
                        if (caller != 0 && !listed(field[6]))
                                goto done;
                        if (fprintf(out, "%s:%s:%s:%s:%s:%s:%s\n",
                            field[0], field[1], field[2], field[3], field[4],
                            field[5], shell) < 0)
                                goto done;
                } else if (fwrite(line, 1, (size_t)count, out) != (size_t)count) {
                        goto done;
                }
        }
        if (ferror(in) || found != 1 || fflush(out) != 0 ||
            fchown(fileno(out), st.st_uid, st.st_gid) != 0 ||
            fchmod(fileno(out), st.st_mode & 0777) != 0 ||
            fsync(fileno(out)) != 0)
                goto done;
        if (fclose(out) != 0) {
                out = NULL;
                goto done;
        }
        out = NULL;
        if (rename(temporary, "/etc/passwd") != 0)
                goto done;
        result = 0;
done:
        free(line);
        if (in != NULL)
                fclose(in);
        if (out != NULL)
                fclose(out);
        if (fd >= 0)
                close(fd);
        if (created && result != 0)
                unlink(temporary);
        return result;
}

int
main(int argc, char **argv)
{
        struct passwd *pw;
        struct stat st;
        const char *shell = NULL;
        char *user, *input = NULL;
        size_t capacity = 0;
        uid_t caller = getuid(), target;
        int option;

        while ((option = getopt(argc, argv, "s:")) != -1) {
                if (option != 's')
                        die("usage: chsh [-s shell] [user]");
                shell = optarg;
        }
        if (argc - optind > 1)
                die("usage: chsh [-s shell] [user]");
        pw = optind < argc ? getpwnam(argv[optind]) : getpwuid(caller);
        if (pw == NULL)
                die("unknown user");
        target = pw->pw_uid;
        user = strdup(pw->pw_name);
        if (user == NULL)
                die("out of memory");
        if (caller != 0 && caller != target)
                die("only the superuser may change another user's shell");
        if (caller != 0 && !listed(pw->pw_shell))
                die("the current shell is restricted");
        if (shell == NULL) {
                FILE *tty = fopen("/dev/tty", "r+");

                if (tty == NULL)
                        die("cannot open the terminal");
                fprintf(tty, "New shell [%s]: ", pw->pw_shell);
                fflush(tty);
                if (getline(&input, &capacity, tty) < 0)
                        die("cannot read the new shell");
                fclose(tty);
                input[strcspn(input, "\r\n")] = '\0';
                if (input[0] == '\0')
                        return 0;
                shell = input;
        }
        if (shell[0] != '/' || strpbrk(shell, ":\r\n") != NULL ||
            stat(shell, &st) != 0 || !S_ISREG(st.st_mode) ||
            access(shell, X_OK) != 0)
                die("the new shell must be an absolute executable path");
        if (caller != 0 && !listed(shell))
                die("the new shell is not listed in /etc/shells");

        signal(SIGHUP, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        if (acct_lock() != 0)
                die("the password files are busy; try again");
        if (caller != 0) {
                struct spwd *sp = getspnam(user);
                char *password, *hash;
                int valid;

                if (sp == NULL) {
                        acct_unlock();
                        die("cannot read the account password");
                }
                password = getpass("Password: ");
                hash = password != NULL ? crypt(password, sp->sp_pwdp) : NULL;
                valid = password != NULL &&
                    ((sp->sp_pwdp[0] == '\0' && password[0] == '\0') ||
                    (hash != NULL && strcmp(hash, sp->sp_pwdp) == 0));
                if (password != NULL)
                        explicit_bzero(password, strlen(password));
                if (!valid) {
                        acct_unlock();
                        die("authentication failed");
                }
        }
        if (rewrite(user, target, shell, caller) != 0) {
                acct_unlock();
                die("cannot update /etc/passwd");
        }
        acct_unlock();
        free(user);
        free(input);
        puts("The shell is changed.");
        return 0;
}
