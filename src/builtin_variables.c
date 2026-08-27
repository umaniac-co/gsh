#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_variables.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

static int output(const gsh_builtin_io *io, int descriptor,
                  const char *text, size_t length)
{
    return io->output(io->opaque, descriptor, text, length);
}

static int fail(const gsh_builtin_io *io, const char *name,
                const char *message)
{
    return gsh_builtin_error(io, name, message);
}

static int journal_failure(const gsh_builtin_io *io, const char *name)
{
    (void)fail(io, name, "variable transaction capacity exceeded");
    return 125;
}

static int list_variables(const char *name, unsigned int attribute,
                          const gsh_variable_store *variables,
                          const gsh_builtin_io *io)
{
    size_t index;

    for (index = 0; index < gsh_variables_count(variables); index++) {
        unsigned int attributes;
        const char *assignment =
            gsh_variables_assignment(variables, index, &attributes);
        const char *separator;
        const char *cursor;

        if ((attributes & attribute) == 0) {
            continue;
        }
        separator = strchr(assignment, '=');
        if (separator == NULL ||
            output(io, STDOUT_FILENO, name, strlen(name)) != 0 ||
            output(io, STDOUT_FILENO, " ", 1) != 0 ||
            output(io, STDOUT_FILENO, assignment,
                   (size_t)(separator - assignment)) != 0) {
            return 1;
        }
        if (!gsh_variables_is_set(variables, index)) {
            if (output(io, STDOUT_FILENO, "\n", 1) != 0) {
                return 1;
            }
            continue;
        }
        if (output(io, STDOUT_FILENO, "='", 2) != 0) {
            return 1;
        }
        cursor = separator + 1U;
        for (;;) {
            const char *quote = strchr(cursor, '\'');
            size_t length = quote != NULL ? (size_t)(quote - cursor)
                                          : strlen(cursor);

            if ((length != 0 &&
                 output(io, STDOUT_FILENO, cursor, length) != 0) ||
                (quote != NULL &&
                 output(io, STDOUT_FILENO, "'\\''", 4) != 0)) {
                return 1;
            }
            if (quote == NULL) {
                break;
            }
            cursor = quote + 1U;
        }
        if (output(io, STDOUT_FILENO, "'\n", 2) != 0) {
            return 1;
        }
    }
    return 0;
}

static int declare_variables(size_t argc, char *const argv[],
                             gsh_variable_store *variables,
                             gsh_variable_journal *journal,
                             unsigned int assignment_attributes,
                             const gsh_builtin_io *io)
{
    const char *name = argv[0];
    unsigned int attribute = strcmp(name, "export") == 0
                                 ? GSH_VARIABLE_EXPORTED
                                 : GSH_VARIABLE_READONLY;
    size_t index = 1;
    int status = 0;

    if (index < argc && strcmp(argv[index], "--") == 0) {
        index++;
    } else if (index < argc && strcmp(argv[index], "-p") == 0) {
        if (index + 1U != argc) {
            return fail(io, name, "-p does not accept operands");
        }
        return list_variables(name, attribute, variables, io);
    } else if (index < argc && argv[index][0] == '-' &&
               argv[index][1] != '\0') {
        return fail(io, name, "invalid option");
    }
    for (; index < argc; index++) {
        const char *operand = argv[index];
        const char *separator = strchr(operand, '=');
        size_t name_length = separator != NULL
                                 ? (size_t)(separator - operand)
                                 : strlen(operand);
        bool is_set;
        unsigned int attributes;
        int changed;

        if (!gsh_variable_name_is_valid(operand, name_length)) {
            status = fail(io, name, "invalid variable name");
            continue;
        }
        if (separator != NULL) {
            const char *value = separator + 1U;
            unsigned int assigned = attribute | assignment_attributes;

            changed = gsh_variables_set(
                variables, operand, name_length, value, strlen(value),
                assigned, assigned);
            if (changed == 0 && journal != NULL &&
                gsh_variable_journal_record(
                    journal, operand, name_length, value, strlen(value),
                    assigned, assigned) == -1) {
                return journal_failure(io, name);
            }
        } else {
            if (gsh_variables_get_state(variables, operand, name_length,
                                        &is_set, &attributes) &&
                (attributes & attribute) != 0) {
                continue;
            }
            changed = gsh_variables_set_attributes(
                variables, operand, name_length, attribute, attribute);
            if (changed == 0 && journal != NULL &&
                gsh_variable_journal_record_attributes(
                    journal, operand, name_length, attribute, attribute) ==
                    -1) {
                return journal_failure(io, name);
            }
        }
        if (changed == -1) {
            status = fail(io, name, errno == EROFS
                                        ? "variable is readonly"
                                        : "variable update failed");
        }
    }
    return status;
}

static int unset_variables(size_t argc, char *const argv[],
                           gsh_variable_store *variables,
                           gsh_variable_journal *journal,
                           const gsh_builtin_io *io)
{
    size_t index = 1;
    bool functions = false;
    bool variables_selected = false;
    int status = 0;

    while (index < argc && argv[index][0] == '-' &&
           argv[index][1] != '\0') {
        const char *option = argv[index] + 1U;

        if (strcmp(argv[index], "--") == 0) {
            index++;
            break;
        }
        while (*option != '\0') {
            if (*option == 'f') {
                functions = true;
            } else if (*option == 'v') {
                variables_selected = true;
            } else {
                return fail(io, "unset", "invalid option");
            }
            option++;
        }
        index++;
    }
    for (; index < argc; index++) {
        size_t length = strlen(argv[index]);
        bool is_set;
        unsigned int attributes;
        int changed;

        if (!gsh_variable_name_is_valid(argv[index], length)) {
            status = fail(io, "unset", "invalid variable name");
            continue;
        }
        if (functions && !variables_selected) {
            continue;
        }
        if (!gsh_variables_get_state(variables, argv[index], length,
                                     &is_set, &attributes)) {
            continue;
        }
        changed = gsh_variables_unset(variables, argv[index], length);
        if (changed == 0 && journal != NULL &&
            gsh_variable_journal_record_unset(
                journal, argv[index], length) == -1) {
            return journal_failure(io, "unset");
        }
        if (changed == -1) {
            status = fail(io, "unset", errno == EROFS
                                       ? "variable is readonly"
                                       : "variable update failed");
        }
    }
    return status;
}

int gsh_builtin_variables(size_t argc, char *const argv[],
                          gsh_variable_store *variables,
                          gsh_variable_journal *journal,
                          unsigned int assignment_attributes,
                          const gsh_builtin_io *io)
{
    if (argc == 0 || argv == NULL || variables == NULL || io == NULL ||
        io->output == NULL) {
        errno = EINVAL;
        return 125;
    }
    return strcmp(argv[0], "unset") == 0
               ? unset_variables(argc, argv, variables, journal, io)
               : declare_variables(argc, argv, variables, journal,
                                   assignment_attributes, io);
}
