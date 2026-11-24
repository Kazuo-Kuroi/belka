/*
 * Copyright (c) 2026 Setsuna Software L.C.
 * Created: 2026-11-17
 * SPDX-License-Identifier: MIT
 */

/* System V text account administration. New accounts start password-locked. */

#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../../lib/compat/include/accounts.h"

struct row {
        char *original;
        char *field[9];
        int active;
        int changed;
};

struct table {
        struct row **row;
        size_t count;
        int fields;
};

static const char *program;
static struct table tables[ACCT_FILES] = {
        { NULL, 0, 7 }, { NULL, 0, 9 }, { NULL, 0, 4 }
};

static void
fail(const char *message)
{
        fprintf(stderr, "%s: %s\n", program, message);
        exit(1);
}

static void *
allocate(size_t size)
{
        void *memory = calloc(1, size);

        if (memory == NULL)
                fail("out of memory");
        return memory;
}

static char *
copy(const char *text)
{
        char *result = strdup(text);

        if (result == NULL)
                fail("out of memory");
        return result;
}

static void
setfield(struct row *row, int field, const char *text)
{
        char *value = copy(text);

        free(row->field[field]);
        row->field[field] = value;
        row->changed = 1;
}

static void
append(struct table *table, struct row *row)
{
        struct row **rows;

        if (table->count >= SIZE_MAX / sizeof(*rows) - 1)
                fail("account file is too large");
        rows = realloc(table->row, (table->count + 1) * sizeof(*rows));
        if (rows == NULL)
                fail("out of memory");
        table->row = rows;
        table->row[table->count++] = row;
}

static struct row *
newrow(struct table *table)
{
        struct row *row = allocate(sizeof(*row));

        row->active = row->changed = 1;
        for (int i = 0; i < table->fields; i++)
                row->field[i] = copy("");
        append(table, row);
        return row;
}

static void
load(int index)
{
        struct table *table = &tables[index];
        FILE *file = fopen(acct_files[index], "r");
        char *line = NULL;
        size_t size = 0;
        ssize_t length;

        if (file == NULL)
                fail("cannot read the account files");
        while ((length = getline(&line, &size, file)) >= 0) {
                struct row *row = allocate(sizeof(*row));
                char *storage, *cursor;

                if ((size_t)length != strlen(line))
                        fail("NUL byte in an account file");
                row->original = copy(line);
                append(table, row);
                if (line[0] == '#' || line[0] == '\n' || line[0] == '\0')
                        continue;
                storage = cursor = copy(line);
                storage[strcspn(storage, "\r\n")] = '\0';
                for (int i = 0; i < table->fields; i++) {
                        char *value = strsep(&cursor, ":");

                        if (value == NULL)
                                fail("malformed account record");
                        row->field[i] = copy(value);
                }
                if (cursor != NULL || row->field[0][0] == '\0')
                        fail("malformed account record");
                row->active = 1;
                free(storage);
        }
        free(line);
        if (ferror(file) || fclose(file) != 0)
                fail("cannot read the account files");
}

static struct row *
find(struct table *table, const char *name)
{
        struct row *found = NULL;

        for (size_t i = 0; i < table->count; i++) {
                struct row *row = table->row[i];

                if (row->active && strcmp(row->field[0], name) == 0) {
                        if (found != NULL)
                                fail("duplicate account records");
                        found = row;
                }
        }
        return found;
}

static uint32_t
number(const char *text)
{
        char *end;
        uintmax_t value;

        errno = 0;
        value = strtoumax(text, &end, 10);
        if (!isdigit((unsigned char)text[0]) || *end != '\0' ||
            errno != 0 || value > INT_MAX)
                fail("invalid numeric identifier");
        return (uint32_t)value;
}

static void
setnumber(struct row *row, int field, uint32_t value)
{
        char text[32];

        snprintf(text, sizeof(text), "%" PRIu32, value);
        setfield(row, field, text);
}

static struct row *
byid(struct table *table, uint32_t id, struct row *except)
{
        for (size_t i = 0; i < table->count; i++) {
                struct row *row = table->row[i];

                if (row->active && row != except && number(row->field[2]) == id)
                        return row;
        }
        return NULL;
}

static uint32_t
nextid(struct table *table)
{
        for (uint32_t id = 1000; id < 60000; id++)
                if (byid(table, id, NULL) == NULL)
                        return id;
        fail("no unused identifier in the automatic range");
        return 0;
}

static void
valid_name(const char *name)
{
        if (!(isalpha((unsigned char)name[0]) || name[0] == '_') ||
            strlen(name) >= LOGIN_NAME_MAX)
                fail("invalid account name");
        for (const char *p = name; *p; p++)
                if (!(isalnum((unsigned char)*p) || *p == '_' ||
                    *p == '-' || *p == '.'))
                        fail("invalid account name");
}

static void
valid_field(const char *text)
{
        if (strpbrk(text, ":\r\n") != NULL)
                fail("account fields cannot contain colons or newlines");
}

static struct row *
resolve_group(const char *name)
{
        struct row *group = isdigit((unsigned char)name[0]) ?
            byid(&tables[2], number(name), NULL) : find(&tables[2], name);

        if (group == NULL)
                fail("unknown group");
        return group;
}

/* Update a name or membership without dropping any other member. */
static void
members(struct row *group, const char *oldname, const char *newname, int add)
{
        char *storage = copy(group->field[3]), *cursor = storage, *member;
        size_t size = strlen(storage) + (newname ? strlen(newname) : 0) + 2;
        char *result = allocate(size);
        int present = 0, replaced = 0;

        while ((member = strsep(&cursor, ",")) != NULL) {
                if (strcmp(member, oldname) == 0) {
                        replaced = 1;
                        continue;
                }
                if (member == NULL || *member == '\0')
                        continue;
                if (newname != NULL && strcmp(member, newname) == 0)
                        present = 1;
                if (result[0] != '\0')
                        strcat(result, ",");
                strcat(result, member);
        }
        if (newname != NULL && (add || replaced) && !present) {
                if (result[0] != '\0')
                        strcat(result, ",");
                strcat(result, newname);
        }
        if (strcmp(result, group->field[3]) != 0)
                setfield(group, 3, result);
        free(result);
        free(storage);
}

static void
supplementary(const char *oldname, const char *newname, const char *selection,
    uint32_t primary)
{
        struct table *groups = &tables[2];
        unsigned char *selected = allocate(groups->count + 1);

        if (selection != NULL && selection[0] != '\0') {
                char *storage = copy(selection), *cursor = storage, *name;

                while ((name = strsep(&cursor, ",")) != NULL) {
                        struct row *group = resolve_group(name);

                        for (size_t i = 0; i < groups->count; i++)
                                if (groups->row[i] == group)
                                        selected[i] = 1;
                }
                free(storage);
        }
        if (selection != NULL) {
                long limit = sysconf(_SC_NGROUPS_MAX);
                size_t count = 1;

                for (size_t i = 0; i < groups->count; i++)
                        if (selected[i] &&
                            number(groups->row[i]->field[2]) != primary)
                                count++;
                if (limit > 0 && count > (size_t)limit)
                        fail("too many supplementary groups");
        }
        for (size_t i = 0; i < groups->count; i++) {
                struct row *group = groups->row[i];

                if (!group->active)
                        continue;
                if (selection != NULL || newname == NULL)
                        members(group, oldname, NULL, 0);
                else
                        members(group, oldname, newname, 0);
                if (selection != NULL && selected[i])
                        members(group, "", newname, 1);
        }
        free(selected);
}

static char *
serialize(struct table *table)
{
        size_t size = 1;
        char *text, *out;

        for (size_t i = 0; i < table->count; i++) {
                struct row *row = table->row[i];

                if (!row->active && row->changed)
                        continue;
                if (!row->changed)
                        size += strlen(row->original) + 1;
                else {
                        size_t length = 0;

                        for (int j = 0; j < table->fields; j++)
                                length += strlen(row->field[j]) + 1;
                        if (table == &tables[0] && length >= NSS_BUFLEN_PASSWD)
                                fail("passwd record exceeds the libc record limit");
                        size += length;
                }
        }
        out = text = allocate(size);
        for (size_t i = 0; i < table->count; i++) {
                struct row *row = table->row[i];

                if (!row->active && row->changed)
                        continue;
                if (out != text && out[-1] != '\n')
                        *out++ = '\n';
                if (!row->changed) {
                        size_t length = strlen(row->original);

                        memcpy(out, row->original, length);
                        out += length;
                } else {
                        for (int j = 0; j < table->fields; j++) {
                                size_t length = strlen(row->field[j]);

                                memcpy(out, row->field[j], length);
                                out += length;
                                *out++ = j + 1 == table->fields ? '\n' : ':';
                        }
                }
        }
        *out = '\0';
        return text;
}

static void
usage(void)
{
        fprintf(stderr,
            "usage: useradd [-m] [-u uid] [-g group] [-G groups] "
            "[-d home] [-s shell] [-c comment] name\n"
            "       usermod [-u uid] [-g group] [-G groups] "
            "[-d home] [-s shell] [-c comment] [-l login] name\n"
            "       userdel name\n"
            "       groupadd [-g gid] name\n"
            "       groupmod [-g gid] [-n name] group\n"
            "       groupdel group\n");
        exit(2);
}

int
main(int argc, char **argv)
{
        const char *id = NULL, *group = NULL, *groups = NULL;
        const char *home = NULL, *shell = NULL, *comment = NULL, *rename_to = NULL;
        const char *name, *operation, *text[ACCT_FILES];
        char *default_home = NULL;
        struct row *record, *shadow;
        uint32_t uid = 0, gid = 0;
        int user, add, modify, remove, create_home = 0, made_home = 0, option;

        program = strrchr(argv[0], '/');
        program = program != NULL ? program + 1 : argv[0];
        if (getuid() != 0)
                fail("only the superuser may administer accounts");
        signal(SIGHUP, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        if (strcmp(program, "acctrecover") == 0) {
                if (argc != 1)
                        usage();
                if (acct_lock() != 0)
                        fail("cannot recover the account files");
                acct_unlock();
                return 0;
        }
        user = strncmp(program, "user", 4) == 0;
        if (!user && strncmp(program, "group", 5) != 0)
                usage();
        operation = program + (user ? 4 : 5);
        add = strcmp(operation, "add") == 0;
        modify = strcmp(operation, "mod") == 0;
        remove = strcmp(operation, "del") == 0;
        if (!(add || modify || remove))
                usage();
        while ((option = getopt(argc, argv, user ? "u:g:G:d:s:c:ml:" : "g:n:")) != -1) {
                if (remove)
                        usage();
                switch (option) {
                case 'u': id = optarg; break;
                case 'g':
                        if (user)
                                group = optarg;
                        else
                                id = optarg;
                        break;
                case 'G': groups = optarg; break;
                case 'd': home = optarg; break;
                case 's': shell = optarg; break;
                case 'c': comment = optarg; break;
                case 'm':
                        if (!add)
                                usage();
                        create_home = 1;
                        break;
                case 'l':
                case 'n':
                        if (!modify)
                                usage();
                        rename_to = optarg;
                        break;
                default: usage();
                }
        }
        if (argc - optind != 1)
                usage();
        name = argv[optind];
        valid_name(name);
        if (rename_to != NULL)
                valid_name(rename_to);
        if (home != NULL) {
                valid_field(home);
                if (home[0] != '/')
                        fail("home directory must be an absolute path");
        }
        if (comment != NULL)
                valid_field(comment);
        if (shell != NULL) {
                struct stat st;

                valid_field(shell);
                if (shell[0] != '/' || stat(shell, &st) != 0 ||
                    !S_ISREG(st.st_mode) || access(shell, X_OK) != 0)
                        fail("shell must be an absolute executable path");
        }
        if (acct_lock() != 0)
                fail("cannot lock or recover the account files");
        for (int i = 0; i < ACCT_FILES; i++)
                load(i);
        record = find(&tables[user ? 0 : 2], name);
        if (add ? record != NULL : record == NULL)
                fail(add ? "account already exists" : "unknown account");
        if (rename_to != NULL && strcmp(rename_to, name) != 0 &&
            find(&tables[user ? 0 : 2], rename_to) != NULL)
                fail("new account name already exists");
        if (user && strcmp(name, "root") == 0 &&
            (remove || id != NULL || rename_to != NULL))
                fail("cannot remove or change root's identity");
        if (!remove) {
                uid = id != NULL ? number(id) :
                    add ? nextid(&tables[user ? 0 : 2]) : number(record->field[2]);
                if (byid(&tables[user ? 0 : 2], uid, record) != NULL)
                        fail("numeric identifier is already in use");
        }
        if (user) {
                shadow = find(&tables[1], name);
                if (add) {
                        char today[32];

                        if (shadow != NULL)
                                fail("shadow entry already exists");
                        record = newrow(&tables[0]);
                        shadow = newrow(&tables[1]);
                        setfield(record, 0, name);
                        setfield(record, 1, "x");
                        setfield(shadow, 0, name);
                        setfield(shadow, 1, "*LK*");
                        snprintf(today, sizeof(today), "%ld",
                            (long)(time(NULL) / 86400));
                        setfield(shadow, 2, today);
                        if (home == NULL) {
                                default_home = allocate(sizeof("/usr/people/") +
                                    strlen(name));
                                sprintf(default_home, "/usr/people/%s", name);
                                home = default_home;
                        }
                        if (shell == NULL)
                                shell = "/bin/sh";
                } else if (shadow == NULL) {
                        fail("account has no shadow entry");
                }
                if (remove) {
                        record->active = shadow->active = 0;
                        record->changed = shadow->changed = 1;
                        supplementary(name, NULL, NULL, 0);
                } else {
                        gid = group != NULL ? number(resolve_group(group)->field[2]) :
                            add ? number(resolve_group("users")->field[2]) :
                            number(record->field[3]);
                        setnumber(record, 2, uid);
                        setnumber(record, 3, gid);
                        if (home != NULL)
                                setfield(record, 5, home);
                        if (shell != NULL)
                                setfield(record, 6, shell);
                        if (comment != NULL)
                                setfield(record, 4, comment);
                        if (rename_to != NULL) {
                                if (find(&tables[1], rename_to) != NULL &&
                                    strcmp(rename_to, name) != 0)
                                        fail("new shadow name already exists");
                                setfield(record, 0, rename_to);
                                setfield(shadow, 0, rename_to);
                        }
                        if (groups != NULL || rename_to != NULL)
                                supplementary(name, rename_to != NULL ?
                                    rename_to : name, groups, gid);
                }
        } else {
                if (add) {
                        record = newrow(&tables[2]);
                        setfield(record, 0, name);
                        setfield(record, 1, "*");
                } else {
                        uint32_t old = number(record->field[2]);

                        for (size_t i = 0; i < tables[0].count; i++) {
                                struct row *pw = tables[0].row[i];

                                if (pw->active && number(pw->field[3]) == old) {
                                        if (remove)
                                                fail("group is a primary group");
                                        setnumber(pw, 3, uid);
                                }
                        }
                }
                if (remove) {
                        record->active = 0;
                        record->changed = 1;
                } else {
                        setnumber(record, 2, uid);
                        if (rename_to != NULL)
                                setfield(record, 0, rename_to);
                }
        }
        for (int i = 0; i < ACCT_FILES; i++)
                text[i] = serialize(&tables[i]);
        if (create_home) {
                int fd;
                struct stat st;

                if (mkdir(home, 0750) == 0) {
                        made_home = 1;
                        fd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                        if (fd < 0 || fchown(fd, uid, gid) != 0) {
                                if (fd >= 0)
                                        close(fd);
                                rmdir(home);
                                fail("cannot set home directory ownership");
                        }
                        close(fd);
                } else if (errno != EEXIST || lstat(home, &st) != 0 ||
                    !S_ISDIR(st.st_mode)) {
                        fail("cannot create or use the home directory");
                }
        }
        if (acct_commit(text) != 0) {
                if (made_home && getpwnam(name) == NULL)
                        rmdir(home);
                fail("account update failed");
        }
        acct_unlock();
        free(default_home);
        return 0;
}
