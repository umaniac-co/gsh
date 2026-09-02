#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_trap.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static bool unsigned_decimal(const char *text)
{
    if (text == NULL) {
        return false;
    }
    const char *cursor = text;

    if (cursor == NULL || *cursor == '\0') {
        return false;
    }
    while (*cursor >= '0' && *cursor <= '9') {
        cursor++;
    }
    return *cursor == '\0';
}

static int output_text(const gsh_builtin_io *io, const char *text,
                       size_t length)
{
    if (io == NULL || text == NULL) {
        return -1;
    }
    return gsh_builtin_output(io, STDOUT_FILENO, text, length);
}

static int output_quoted_action(const gsh_builtin_io *io,
                                const char *action, size_t length)
{
    if (action == NULL) {
        return -1;
    }
    size_t offset = 0;

    if (output_text(io, "'", 1U) != 0) {
        return -1;
    }
    while (offset < length) {
        const char *quote = memchr(action + offset, '\'', length - offset);
        size_t segment = quote == NULL ? length - offset
                                       : (size_t)(quote - action) - offset;

        if ((segment != 0U &&
             output_text(io, action + offset, segment) != 0) ||
            (quote != NULL && output_text(io, "'\\''", 4U) != 0)) {
            return -1;
        }
        offset += segment + (quote != NULL ? 1U : 0U);
    }
    return output_text(io, "'", 1U) == 0 ? 0 : -1;
}

static int output_condition(const gsh_builtin_io *io,
                            const gsh_trap_store *traps,
                            size_t condition)
{
    if (traps == NULL) {
        return -1;
    }
    gsh_trap_state state = gsh_traps_query_state(traps, condition);
    const char *name = gsh_traps_condition_name(condition);
    const char *action = NULL;
    size_t action_length = 0;

    if (name == NULL || output_text(io, "trap -- ", 8U) != 0) {
        return -1;
    }
    if (state == GSH_TRAP_DEFAULT) {
        if (output_text(io, "-", 1U) != 0) {
            return -1;
        }
    } else if (state == GSH_TRAP_IGNORE ||
               state == GSH_TRAP_INHERITED_IGNORE) {
        if (output_text(io, "''", 2U) != 0) {
            return -1;
        }
    } else {
        action = gsh_traps_query_action(traps, condition,
                                        &action_length);
        if (action == NULL ||
            output_quoted_action(io, action, action_length) == -1) {
            return -1;
        }
    }
    return output_text(io, " ", 1U) == 0 &&
                   output_text(io, name, strlen(name)) == 0 &&
                   output_text(io, "\n", 1U) == 0
               ? 0
               : -1;
}

static int invalid_condition(const gsh_builtin_io *io, const char *text)
{
    if (io == NULL || text == NULL) {
        return -1;
    }
    (void)gsh_builtin_output(io, STDERR_FILENO,
                     "gsh: trap: invalid condition: ", 30U);
    (void)gsh_builtin_output(io, STDERR_FILENO, text, strlen(text));
    (void)gsh_builtin_output(io, STDERR_FILENO, "\n", 1U);
    return 1;
}

static int invalid_option(const gsh_builtin_io *io, const char *text)
{
    if (io == NULL || text == NULL) {
        return -1;
    }
    (void)gsh_builtin_output(io, STDERR_FILENO,
                     "gsh: trap: invalid option: ", 27U);
    (void)gsh_builtin_output(io, STDERR_FILENO, text, strlen(text));
    (void)gsh_builtin_output(io, STDERR_FILENO, "\n", 1U);
    return 2;
}

static int list_traps(int argc, char *const argv[], int first,
                      const gsh_trap_store *traps,
                      const gsh_builtin_io *io, bool all)
{
    if (argv == NULL) return -1;
    if (io == NULL) {
        return -1;
    }
    size_t condition;

    if (first < argc) {
        for (; first < argc; first++) {
            if (!gsh_traps_parse_condition(argv[first], &condition)) {
                return invalid_condition(io, argv[first]);
            }
            if (output_condition(io, traps, condition) == -1) {
                return 1;
            }
        }
        return 0;
    }
    for (condition = 0; condition < gsh_traps_condition_count();
         condition++) {
        if ((all ||
             gsh_traps_query_state(traps, condition) != GSH_TRAP_DEFAULT) &&
            output_condition(io, traps, condition) == -1) {
            return 1;
        }
    }
    return 0;
}

static bool selected_conditions_fit(const gsh_trap_store *traps,
                                    const bool selected[],
                                    size_t action_length)
{
    if (traps == NULL || selected == NULL) return false;
    size_t projected = 0;
    size_t condition;

    for (condition = 0; condition < gsh_traps_condition_count();
         condition++) {
        if (gsh_traps_state(traps, condition) == GSH_TRAP_ACTION) {
            projected += traps->entries[condition].length;
        }
    }

    for (condition = 0; condition < gsh_traps_condition_count();
         condition++) {
        if (!selected[condition]) {
            continue;
        }
        if (gsh_traps_state(traps, condition) == GSH_TRAP_ACTION) {
            projected -= traps->entries[condition].length;
        }
        if (action_length > GSH_TRAP_TEXT_CAP - projected) {
            return false;
        }
        projected += action_length;
    }
    return true;
}

static int configure_traps(int argc, char *const argv[], int first,
                           const char *action, size_t action_length,
                           gsh_trap_store *traps,
                           const gsh_builtin_io *io)
{
    if (argv == NULL) return -1;
    if (io == NULL) {
        return -1;
    }
    bool selected[GSH_TRAP_CONDITION_CAP] = {false};
    size_t condition;
    int operand;

    for (operand = first; operand < argc; operand++) {
        if (!gsh_traps_parse_condition(argv[operand], &condition)) {
            return invalid_condition(io, argv[operand]);
        }
        selected[condition] = true;
    }
    if (!selected_conditions_fit(traps, selected,
                                 action == NULL ? 0U : action_length)) {
        (void)gsh_builtin_error(io, "trap", "action storage exhausted");
        return 125;
    }
    for (condition = 0; condition < gsh_traps_condition_count();
         condition++) {
        if (selected[condition] &&
            gsh_traps_configure(traps, condition, action,
                                action == NULL ? 0U : action_length) == -1) {
            (void)gsh_builtin_error(io, "trap", strerror(errno));
            return 125;
        }
    }
    return 0;
}

int gsh_builtin_trap(int argc, char *const argv[], gsh_trap_store *traps,
                     const gsh_builtin_io *io)
{
    int first = 1;
    bool print_all = false;
    bool options_terminated = false;
    const char *action;

    if (argc < 1 || argc > GSH_TRAP_OPERAND_CAP || argv == NULL ||
        traps == NULL || !gsh_builtin_io_valid(io)) {
        errno = EINVAL;
        return 1;
    }
    if (first < argc && strcmp(argv[first], "-p") == 0) {
        print_all = true;
        first++;
        if (first < argc && strcmp(argv[first], "--") == 0) {
            first++;
        }
        return list_traps(argc, argv, first, traps, io, print_all);
    }
    if (first < argc && strcmp(argv[first], "--") == 0) {
        options_terminated = true;
        first++;
    }
    if (first == argc) {
        return list_traps(argc, argv, first, traps, io, false);
    }
    if (!options_terminated && argv[first][0] == '-' &&
        strcmp(argv[first], "-") != 0) {
        return invalid_option(io, argv[first]);
    }
    if (unsigned_decimal(argv[first])) {
        return configure_traps(argc, argv, first, NULL, 0U, traps, io);
    }
    action = argv[first++];
    return configure_traps(argc, argv, first,
                           strcmp(action, "-") == 0 ? NULL : action,
                           strcmp(action, "-") == 0 ? 0U : strlen(action),
                           traps, io);
}
