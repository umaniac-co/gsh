#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "shell_invocation.h"

#include <errno.h>
#include <string.h>

typedef struct {
    bool command;
    bool standard_input;
} invocation_flags;

static int apply_named_option(gsh_shell_options *options, bool enabled,
                              const char *name)
{
    if (options == NULL || name == NULL || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    return gsh_options_update_name(options, name, enabled);
}

static int apply_short_option(gsh_shell_options *options,
                              invocation_flags *flags, char option,
                              bool enabled)
{
    if (options == NULL || flags == NULL) {
        errno = EINVAL;
        return -1;
    }
    if ((option == 'c' || option == 's') && !enabled) {
        errno = EINVAL;
        return -1;
    }
    if (option == 'c') {
        flags->command = true;
        return 0;
    }
    if (option == 's') {
        flags->standard_input = true;
        return 0;
    }
    return gsh_options_update_letter(options, option, enabled);
}

static int parse_option_word(int argc, char *const argv[], size_t *index,
                             gsh_shell_options *options,
                             invocation_flags *flags)
{
    const char *word;
    bool enabled;
    size_t offset;

    if (argv == NULL || flags == NULL || index == NULL || options == NULL ||
        *index >= (size_t)argc) {
        errno = EINVAL;
        return -1;
    }
    word = argv[*index];
    enabled = word[0] == '-';
    for (offset = 1U; word[offset] != '\0' &&
                      offset < GSH_OPTION_FLAG_CAP; offset++) {
        if (word[offset] == 'o') {
            const char *name = word[offset + 1U] == '\0'
                                   ? NULL : word + offset + 1U;

            if (name == NULL && ++(*index) < (size_t)argc) {
                name = argv[*index];
            }
            return apply_named_option(options, enabled, name);
        }
        if (apply_short_option(options, flags, word[offset], enabled) == -1) {
            return -1;
        }
    }
    if (word[offset] != '\0') {
        errno = E2BIG;
        return -1;
    }
    return 0;
}

static int parse_options(int argc, char *const argv[], size_t *index,
                         gsh_shell_options *options,
                         invocation_flags *flags)
{
    size_t parsed;

    if (argv == NULL || flags == NULL || index == NULL || options == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (parsed = 0U; *index < (size_t)argc &&
                       parsed < GSH_INVOCATION_ARGUMENT_CAP; parsed++) {
        const char *word = argv[*index];

        if (strcmp(word, "--") == 0) {
            (*index)++;
            return 0;
        }
        if (strcmp(word, "-") == 0) {
            (*index)++;
            flags->standard_input = true;
            return 0;
        }
        if ((word[0] != '-' && word[0] != '+') || word[1] == '\0') {
            return 0;
        }
        if (parse_option_word(argc, argv, index, options, flags) == -1) {
            return -1;
        }
        (*index)++;
        if (flags->command) {
            return 0;
        }
    }
    if (*index < (size_t)argc) {
        errno = E2BIG;
        return -1;
    }
    return 0;
}

static void select_mode(int argc, bool stdin_is_tty, size_t index,
                        const invocation_flags *flags,
                        gsh_invocation *invocation)
{
    if (flags == NULL || invocation == NULL) return;
    invocation->source_index = index;
    invocation->parameter_zero_index = (size_t)argc;
    invocation->positional_index = (size_t)argc;
    if (flags->command) {
        invocation->mode = GSH_INVOCATION_COMMAND;
        invocation->parameter_zero_index = index + 1U < (size_t)argc
                                               ? index + 1U : (size_t)argc;
        invocation->positional_index = index + 2U < (size_t)argc
                                           ? index + 2U : (size_t)argc;
    } else if (!flags->standard_input && index < (size_t)argc) {
        invocation->mode = GSH_INVOCATION_FILE;
        invocation->parameter_zero_index = index;
        invocation->positional_index = index + 1U;
    } else if (!flags->standard_input && index == (size_t)argc &&
               stdin_is_tty) {
        invocation->mode = GSH_INVOCATION_INTERACTIVE;
    } else {
        invocation->mode = GSH_INVOCATION_STDIN;
        invocation->positional_index = index;
    }
}

/* ── Invocation Is Parsed Once At The Process Boundary ────────
 * argv is an OS-bounded input, but the shell still caps option scanning before
 * touching evaluator state. Parsing produces one immutable startup decision:
 * source kind, positional slice, and initial option snapshot. The command hot
 * path never revisits argv and invalid combinations cannot partially commit.
 * ────────────────────────────────────────────────────────── */
int gsh_invocation_parse(int argc, char *const argv[], bool stdin_is_tty,
                         gsh_invocation *invocation)
{
    invocation_flags flags = {0};
    size_t index = 1U;

    if (argc <= 0 || argc > GSH_INVOCATION_ARGUMENT_CAP || argv == NULL ||
        argv[0] == NULL || invocation == NULL) {
        errno = argc > GSH_INVOCATION_ARGUMENT_CAP ? E2BIG : EINVAL;
        return -1;
    }
    (void)memset(invocation, 0, sizeof(*invocation));
    gsh_options_initialize(&invocation->options, false);
    if (parse_options(argc, argv, &index, &invocation->options, &flags) == -1 ||
        (flags.command && index >= (size_t)argc)) {
        if (flags.command && index >= (size_t)argc) errno = EINVAL;
        return -1;
    }
    select_mode(argc, stdin_is_tty, index, &flags, invocation);
    return 0;
}
