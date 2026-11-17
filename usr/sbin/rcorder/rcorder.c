/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-16
 * SPDX-License-Identifier: MIT
 */

/*
 * rcorder [-k keyword] [-s keyword] file ...: print the files in the
 * order their dependencies need, for /etc/rc and the rc.d scripts.
 *
 * A file's dependencies are comment lines at its start, each a word
 * after "# " and a colon, then names separated by blanks:
 *
 *   # PROVIDE: name ...     what the file sets up
 *   # REQUIRE: name ...     what must be set up before it
 *   # BEFORE: name ...      what it must come before
 *   # KEYWORD: word ...     for -k and -s
 *
 * Reading stops at the first line that is neither a comment nor blank.
 * A file comes after every file that provides a name it requires, and
 * before every file that provides a name in its BEFORE line; otherwise
 * the files keep the order they were given in. A name required but
 * provided by no file is reported and ignored. A circle of dependencies
 * is reported and broken where it was found, and rcorder then exits
 * with 1, after printing every file.
 *
 * With -k, only files that have one of the -k keywords are printed;
 * with -s, files that have one of the -s keywords are not. Both may be
 * given more than once. Files left out still order the others.
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAXKEYS         16

struct names {
        char    **v;
        int     n;
};

struct rcfile {
        const char      *path;
        struct names    provide, require, before, keyword;
        int             state;          /* 0 new, 1 visiting, 2 done */
};

static struct rcfile *files;
static int nfiles;
static const char *keep[MAXKEYS], *skip[MAXKEYS];
static int nkeep, nskip, status;

static void *
xmalloc(size_t n)
{
        void *p = malloc(n);

        if (p == NULL) {
                fprintf(stderr, "rcorder: out of memory\n");
                exit(2);
        }
        return p;
}

static void
names_add(struct names *ns, const char *s, size_t len)
{
        char *c = xmalloc(len + 1);
        char **v = realloc(ns->v, sizeof(*v) * (size_t)(ns->n + 1));

        if (v == NULL) {
                fprintf(stderr, "rcorder: out of memory\n");
                exit(2);
        }
        memcpy(c, s, len);
        c[len] = '\0';
        ns->v = v;
        ns->v[ns->n++] = c;
}

static int
names_has(const struct names *ns, const char *s)
{
        for (int i = 0; i < ns->n; i++)
                if (strcmp(ns->v[i], s) == 0)
                        return 1;
        return 0;
}

/* Add the blank-separated names in s to ns. */
static void
names_split(struct names *ns, const char *s)
{
        while (*s != '\0') {
                size_t len;

                while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
                        s++;
                len = strcspn(s, " \t\r\n");
                if (len > 0)
                        names_add(ns, s, len);
                s += len;
        }
}

/* Read a file's dependency lines. */
static void
read_file(struct rcfile *f)
{
        static const struct {
                const char      *word;
                size_t          off;
        } kinds[] = {
                { "PROVIDE:", offsetof(struct rcfile, provide) },
                { "REQUIRE:", offsetof(struct rcfile, require) },
                { "REQUIRES:", offsetof(struct rcfile, require) },
                { "BEFORE:", offsetof(struct rcfile, before) },
                { "KEYWORD:", offsetof(struct rcfile, keyword) },
                { "KEYWORDS:", offsetof(struct rcfile, keyword) },
        };
        char line[1024];
        FILE *fp;

        if ((fp = fopen(f->path, "r")) == NULL) {
                fprintf(stderr, "rcorder: %s: %s\n", f->path, strerror(errno));
                status = 1;
                return;
        }
        while (fgets(line, sizeof(line), fp) != NULL) {
                char *s = line;

                while (*s == ' ' || *s == '\t')
                        s++;
                if (*s == '\n' || *s == '\0')
                        continue;
                if (*s != '#')
                        break;
                s++;
                while (*s == ' ' || *s == '\t')
                        s++;
                for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
                        size_t len = strlen(kinds[k].word);

                        if (strncmp(s, kinds[k].word, len) == 0) {
                                names_split((struct names *)(void *)
                                    ((char *)f + kinds[k].off), s + len);
                                break;
                        }
                }
        }
        fclose(fp);
}

static void visit(struct rcfile *);

/* Visit every file that provides name (for f, which needs it first). */
static void
visit_providers(struct rcfile *f, const char *name)
{
        int found = 0;

        for (int i = 0; i < nfiles; i++) {
                if (!names_has(&files[i].provide, name))
                        continue;
                found = 1;
                if (&files[i] == f)
                        continue;
                if (files[i].state == 1) {
                        fprintf(stderr, "rcorder: circular dependency on "
                            "%s, between %s and %s\n", name, f->path,
                            files[i].path);
                        status = 1;
                        continue;
                }
                visit(&files[i]);
        }
        if (!found)
                fprintf(stderr, "rcorder: %s: requirement %s has no "
                    "provider\n", f->path, name);
}

static int
printed(const struct rcfile *f)
{
        int has = 0;

        for (int i = 0; i < nskip; i++)
                if (names_has(&f->keyword, skip[i]))
                        return 0;
        if (nkeep == 0)
                return 1;
        for (int i = 0; i < nkeep; i++)
                has |= names_has(&f->keyword, keep[i]);
        return has;
}

/* Print f after everything that must come before it. */
static void
visit(struct rcfile *f)
{
        if (f->state != 0)
                return;
        f->state = 1;
        for (int i = 0; i < f->require.n; i++)
                visit_providers(f, f->require.v[i]);
        /* A file whose BEFORE names what f provides comes first. */
        for (int i = 0; i < nfiles; i++) {
                struct rcfile *g = &files[i];

                if (g == f || g->state == 2)
                        continue;
                for (int j = 0; j < f->provide.n; j++) {
                        if (!names_has(&g->before, f->provide.v[j]))
                                continue;
                        if (g->state == 1) {
                                fprintf(stderr, "rcorder: circular "
                                    "dependency on %s, between %s and %s\n",
                                    f->provide.v[j], f->path, g->path);
                                status = 1;
                        } else {
                                visit(g);
                        }
                        break;
                }
        }
        f->state = 2;
        if (printed(f))
                printf("%s\n", f->path);
}

int
main(int argc, char **argv)
{
        int c;

        while ((c = getopt(argc, argv, "k:s:")) != -1) {
                switch (c) {
                case 'k':
                        if (nkeep < MAXKEYS)
                                keep[nkeep++] = optarg;
                        break;
                case 's':
                        if (nskip < MAXKEYS)
                                skip[nskip++] = optarg;
                        break;
                default:
                        fprintf(stderr, "usage: rcorder [-k keyword] "
                            "[-s keyword] file ...\n");
                        return 2;
                }
        }
        argc -= optind;
        argv += optind;
        files = xmalloc(sizeof(*files) * (size_t)(argc > 0 ? argc : 1));
        memset(files, 0, sizeof(*files) * (size_t)(argc > 0 ? argc : 1));
        for (int i = 0; i < argc; i++) {
                files[nfiles].path = argv[i];
                read_file(&files[nfiles++]);
        }
        for (int i = 0; i < nfiles; i++)
                visit(&files[i]);
        return status;
}
