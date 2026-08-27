#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "builtin_alias.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static int output(const gsh_builtin_io *io, const char *text, size_t length)
{
    return io->output(io->opaque, STDOUT_FILENO, text, length);
}

static int write_definition(const gsh_builtin_io *io,
                            const char *assignment)
{
    const char *separator = strchr(assignment, '=');
    const char *cursor;

    if (separator == NULL ||
        output(io, assignment, (size_t)(separator - assignment) + 1U) != 0 ||
        output(io, "'", 1) != 0) {
        return 1;
    }
    cursor = separator + 1U;
    for (;;) {
        const char *quote = strchr(cursor, '\'');
        size_t length = quote == NULL ? strlen(cursor)
                                      : (size_t)(quote - cursor);

        if ((length != 0 && output(io, cursor, length) != 0) ||
            (quote != NULL && output(io, "'\\''", 4) != 0)) {
            return 1;
        }
        if (quote == NULL) {
            return output(io, "'\n", 2);
        }
        cursor = quote + 1U;
    }
}

static int list_all(const gsh_alias_store *aliases,
                    const gsh_builtin_io *io)
{
    size_t order[GSH_ALIAS_CAP];
    size_t count = gsh_aliases_count(aliases);
    size_t index;

    for (index = 0; index < count; index++) {
        size_t position = index;

        order[index] = index;
        while (position > 0 &&
               strcmp(gsh_aliases_assignment(aliases, order[position - 1U]),
                      gsh_aliases_assignment(aliases, order[position])) > 0) {
            size_t temporary = order[position - 1U];

            order[position - 1U] = order[position];
            order[position] = temporary;
            position--;
        }
    }
    for (index = 0; index < count; index++) {
        if (write_definition(io,
                             gsh_aliases_assignment(aliases, order[index])) !=
            0) {
            return 1;
        }
    }
    return 0;
}

int gsh_builtin_alias(size_t argc, char *const argv[],
                      gsh_alias_store *aliases,
                      gsh_alias_journal *journal,
                      const gsh_builtin_io *io)
{
    size_t index = 1;
    int status = 0;

    if (argc == 0 || argv == NULL || io == NULL || io->output == NULL) {
        errno = EINVAL;
        return 125;
    }
    if (index < argc && strcmp(argv[index], "--") == 0) {
        index++;
    } else if (index < argc && argv[index][0] == '-' &&
               argv[index][1] != '\0') {
        return gsh_builtin_error(io, "alias", "invalid option");
    }
    if (index == argc) {
        return list_all(aliases, io);
    }
    for (; index < argc; index++) {
        const char *operand = argv[index];
        const char *separator = strchr(operand, '=');
        size_t name_length = separator == NULL
                                 ? strlen(operand)
                                 : (size_t)(separator - operand);

        if (!gsh_alias_name_is_valid(operand, name_length)) {
            status = gsh_builtin_error(io, "alias", "invalid alias name");
            continue;
        }
        if (separator == NULL) {
            size_t alias_index;
            const char *value = gsh_aliases_lookup(
                aliases, operand, name_length, &alias_index);

            if (value == NULL) {
                status = gsh_builtin_error(io, "alias",
                                           "alias is not defined");
            } else if (write_definition(
                           io, gsh_aliases_assignment(aliases,
                                                       alias_index)) != 0) {
                status = 1;
            }
            continue;
        }
        if (aliases == NULL) {
            return gsh_builtin_error(io, "alias", "alias storage unavailable");
        }
        {
            const char *value = separator + 1U;
            size_t value_length = strlen(value);

            if (gsh_aliases_set(aliases, operand, name_length, value,
                                value_length) == -1) {
                status = gsh_builtin_error(io, "alias",
                                           "alias capacity exceeded");
            } else if (journal != NULL &&
                       gsh_alias_journal_record_set(
                           journal, operand, name_length, value,
                           value_length) == -1) {
                (void)gsh_builtin_error(
                    io, "alias", "alias transaction capacity exceeded");
                return 125;
            }
        }
    }
    return status;
}
