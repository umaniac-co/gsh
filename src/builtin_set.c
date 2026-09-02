#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_set.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static int output(const gsh_builtin_io *io, const char *text, size_t length)
{
    if (io == NULL || text == NULL) {
        return -1;
    }
    return gsh_builtin_output(io, STDOUT_FILENO, text, length);
}

static int fail(const gsh_builtin_io *io, const char *message)
{
    if (io == NULL || message == NULL) {
        return -1;
    }
    return gsh_builtin_error(io, "set", message);
}

static void sort_assignments(const char **assignments, size_t count)
{
    if (assignments == NULL) {
        return;
    }
    size_t unsorted;

    for (unsorted = 1; unsorted < count; unsorted++) {
        const char *value = assignments[unsorted];
        size_t insertion = unsorted;

        while (insertion > 0 &&
               strcoll(assignments[insertion - 1U], value) > 0) {
            assignments[insertion] = assignments[insertion - 1U];
            insertion--;
        }
        assignments[insertion] = value;
    }
}

static int quote_assignment(const gsh_builtin_io *io,
                            const char *assignment)
{
    if (assignment == NULL) {
        return -1;
    }
    const char *separator = strchr(assignment, '=');
    const char *cursor;

    if (separator == NULL ||
        output(io, assignment, (size_t)(separator - assignment)) != 0 ||
        output(io, "='", 2) != 0) {
        return 1;
    }
    cursor = separator + 1U;
    for (size_t segment = 0; segment < GSH_VARIABLE_TEXT_CAP; segment++) {
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
    errno = E2BIG;
    return 1;
}

static int list_variables(const gsh_variable_store *variables,
                          const gsh_builtin_io *io)
{
    if (variables == NULL) {
        return -1;
    }
    const char *assignments[GSH_VARIABLE_CAP];
    size_t count = 0;
    size_t index;

    for (index = 0; index < gsh_variables_count(variables); index++) {
        if (gsh_variables_is_set(variables, index)) {
            assignments[count++] =
                gsh_variables_assignment(variables, index, NULL);
        }
    }
    sort_assignments(assignments, count);
    for (index = 0; index < count; index++) {
        if (quote_assignment(io, assignments[index]) != 0) {
            return 1;
        }
    }
    return 0;
}

static int list_options(const gsh_shell_options *options, bool reusable,
                        const gsh_builtin_io *io)
{
    if (io == NULL || options == NULL) {
        return -1;
    }
    size_t index;

    for (index = 0; index < gsh_options_count(); index++) {
        const char *name = gsh_options_name(index);
        bool enabled = gsh_options_index_enabled(options, index);

        if (reusable) {
            const char *prefix = enabled ? "set -o " : "set +o ";

            if (output(io, prefix, 7) != 0 ||
                output(io, name, strlen(name)) != 0 ||
                output(io, "\n", 1) != 0) {
                return 1;
            }
        } else if (output(io, name, strlen(name)) != 0 ||
                   output(io, enabled ? "\ton\n" : "\toff\n",
                          enabled ? 4U : 5U) != 0) {
            return 1;
        }
    }
    return 0;
}

static int update_letters(gsh_shell_options *options, const char *letters,
                          bool enabled, const gsh_builtin_io *io)
{
    if (io == NULL || letters == NULL) {
        return -1;
    }
    while (*letters != '\0') {
        if (*letters == 'o' ||
            gsh_options_update_letter(options, *letters, enabled) == -1) {
            return fail(io, "invalid option");
        }
        letters++;
    }
    return 0;
}

bool gsh_builtin_set_mutates_positionals(size_t argc,
                                         char *const argv[])
{
    if (argv == NULL) {
        return false;
    }
    size_t index = 1;

    while (index < argc) {
        const char *argument = argv[index];

        if (strcmp(argument, "--") == 0 ||
            ((argument[0] != '-' && argument[0] != '+') ||
             argument[1] == '\0')) {
            return true;
        }
        if (strcmp(argument + 1U, "o") == 0 &&
            index + 1U < argc &&
            strcmp(argv[index + 1U], "--") != 0) {
            index++;
        }
        index++;
    }
    return false;
}

int gsh_builtin_set(size_t argc, char *const argv[],
                    gsh_variable_store *variables,
                    gsh_positional_store *positionals,
                    gsh_shell_options *options,
                    const gsh_builtin_io *io)
{
    if (argv == NULL || io == NULL || options == NULL || variables == NULL) {
        return -1;
    }
    gsh_shell_options next = *options;
    size_t index = 1;
    size_t positional_begin = argc;
    bool replace_positionals = false;
    int report = 0;

    if (argc == 1U) {
        return list_variables(variables, io);
    }
    while (index < argc) {
        const char *argument = argv[index];
        bool enabled;

        if (strcmp(argument, "--") == 0) {
            replace_positionals = true;
            positional_begin = ++index;
            break;
        }
        if ((argument[0] != '-' && argument[0] != '+') ||
            argument[1] == '\0') {
            replace_positionals = true;
            positional_begin = index;
            break;
        }
        enabled = argument[0] == '-';
        if (strcmp(argument + 1U, "o") == 0) {
            if (index + 1U == argc ||
                strcmp(argv[index + 1U], "--") == 0) {
                report = enabled ? 1 : 2;
                index++;
                continue;
            }
            if (gsh_options_update_name(&next, argv[index + 1U],
                                        enabled) == -1) {
                return fail(io, "invalid option name");
            }
            index += 2U;
            continue;
        }
        if (update_letters(&next, argument + 1U, enabled, io) != 0) {
            return 1;
        }
        index++;
    }
    if (replace_positionals &&
        gsh_positionals_assign(positionals, argc - positional_begin,
                               argv + positional_begin) == -1) {
        return fail(io, "positional parameter limit exceeded");
    }
    *options = next;
    return report == 0 ? 0 : list_options(options, report == 2, io);
}
