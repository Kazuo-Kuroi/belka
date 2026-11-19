/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-19
 * SPDX-License-Identifier: MIT
 */
#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *dir;
static int failures;
#define CHECK(expr) do { \
        if (!(expr)) { \
                fprintf(stderr, "exectest: %d: %s\n", __LINE__, #expr); \
                failures++; \
        } \
} while (0)

/* POSIX PATH search and text-file fallback preserve arguments and environment. */
static void
test_exec_scripts(void)
{
        char directory[512], script[512];
        FILE *stream;
        pid_t child;
        int status;
        char *arguments[] = { "ranlib", "with spaces", "", "kept", NULL };

        snprintf(directory, sizeof(directory), "%s/exec-fixture", dir);
        snprintf(script, sizeof(script), "%s/exec-fixture/script", dir);
        CHECK(mkdir(directory, 0755) == 0);
        stream = fopen(script, "w");
        CHECK(stream != NULL);
        if (!stream)
                return;
        fputs("if [ \"$#\" -eq 3 ] && [ \"$1\" = 'with spaces' ] && "
            "[ -z \"$2\" ] && [ \"$EXEC_CHECK\" = \"$3\" ]; then\n"
            "/bin/true || exit 99\nexit 37\nfi\n"
            "exit 99\n", stream);
        CHECK(fclose(stream) == 0);
        CHECK(chmod(script, 0755) == 0);
        for (int mode = 0; mode < 6; mode++) {
                child = fork();
                if (child == 0) {
                        setenv("EXEC_CHECK", "kept", 1);
                        setenv("PATH", directory, 1);
                        if (mode >= 2 && mode <= 4) {
                                if (chdir(directory) != 0)
                                        _exit(98);
                                setenv("PATH", mode == 2 ? ":/missing" :
                                    mode == 3 ? "/missing:" : "", 1);
                        }
                        if (mode == 5)
                                execlp("script", "ranlib", "with spaces", "",
                                    "kept", (char *)NULL);
                        else
                                execvp(mode == 0 ? script : "script", arguments);
                        _exit(97);
                }
                CHECK(child > 0 && waitpid(child, &status, 0) == child &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 37);
        }
        errno = 0;
        char *environment[] = { NULL };
        CHECK(execve(script, arguments, environment) == -1 && errno == ENOEXEC);
        errno = 0;
        CHECK(execvp("", arguments) == -1 && errno == ENOENT);
        CHECK(unlink(script) == 0);
        CHECK(rmdir(directory) == 0);
}

int
main(int argc, char **argv)
{
        if (argc != 2)
                return 2;
        dir = argv[1];
        test_exec_scripts();
        printf("exectest: %d failures\n", failures);
        return failures != 0;
}
