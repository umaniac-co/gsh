#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_fc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h> /* CANON-INCLUDE: linux */
#include <unistd.h>

enum {
    GSH_FC_EDITOR_ARGUMENT_CAP = 3,
    GSH_FC_IO_ATTEMPT_CAP = 2 * 1024 * 1024,
    GSH_FC_EXCESS_RETRY_CAP = 1024,
};

typedef struct {
    bool list;
    bool suppress_numbers;
    bool reverse;
    bool substitute;
    const char *editor;
    const char *replacement;
    size_t operand;
} fc_invocation;

static int fc_error(const gsh_builtin_io *io, const char *message)
{
    if (io == NULL || message == NULL) {
        return -1;
    }
    return gsh_builtin_error(io, "fc", message);
}

static int parse_fc_options(size_t argc, char *const argv[],
                            fc_invocation *invocation,
                            const gsh_builtin_io *io)
{
    if (argv == NULL || invocation == NULL || io == NULL) {
        return -1;
    }
    size_t index = 1U;

    (void)memset(invocation, 0, sizeof(*invocation));
    while (index < argc && argv[index][0] == '-' &&
           argv[index][1] != '\0') {
        const char *option = argv[index] + 1U;

        if (strcmp(argv[index], "--") == 0) {
            index++;
            break;
        }
        if (option[0] >= '0' && option[0] <= '9') {
            break;
        }
        while (*option != '\0') {
            if (*option == 'l') invocation->list = true;
            else if (*option == 'n') invocation->suppress_numbers = true;
            else if (*option == 'r') invocation->reverse = true;
            else if (*option == 's') invocation->substitute = true;
            else if (*option == 'e' && option[1] == '\0' &&
                     index + 1U < argc) {
                invocation->editor = argv[++index];
            } else return fc_error(io, "invalid option");
            option++;
        }
        index++;
    }
    invocation->operand = index;
    if ((invocation->substitute &&
         (invocation->list || invocation->suppress_numbers ||
          invocation->reverse || invocation->editor != NULL)) ||
        (invocation->suppress_numbers && !invocation->list) ||
        (invocation->list && invocation->editor != NULL)) {
        return fc_error(io, "incompatible options");
    }
    return 0;
}

static bool parse_event_number(const char *text, int64_t *number)
{
    if (number == NULL || text == NULL) {
        return false;
    }
    char *end;
    intmax_t parsed;

    errno = 0;
    parsed = strtoimax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < INT64_MIN || parsed > INT64_MAX) {
        return false;
    }
    *number = (int64_t)parsed;
    return true;
}

static int resolve_fc_event(const gsh_history_store *history,
                            const char *operand, uint64_t eligible_last,
                            uint64_t *event)
{
    if (event == NULL || history == NULL) {
        return -1;
    }
    int64_t number;

    if (parse_event_number(operand, &number)) {
        if (number < 0) {
            uint64_t distance = (uint64_t)(-(number + 1)) + 1U;

            if (distance > eligible_last) {
                return -1;
            }
            *event = eligible_last + 1U - distance;
        } else {
            *event = (uint64_t)number;
        }
        return gsh_history_event(history, *event, &(size_t){0}) != NULL
                   ? 0 : -1;
    }
    return gsh_history_find_prefix(history, operand, strlen(operand),
                                   eligible_last, event);
}

static int fc_default_range(const gsh_history_store *history,
                            const fc_invocation *invocation,
                            uint64_t eligible_last, uint64_t *first,
                            uint64_t *last)
{
    if (invocation == NULL) return -1;
    if (first == NULL || history == NULL || last == NULL) {
        return -1;
    }
    uint64_t oldest = gsh_history_oldest_event(history);

    if (oldest == 0 || eligible_last < oldest) {
        return -1;
    }
    *last = eligible_last;
    if (invocation->list) {
        *first = eligible_last - oldest >= 15U
                     ? eligible_last - 15U : oldest;
    } else {
        *first = eligible_last;
    }
    return 0;
}

static int fc_resolve_range(size_t argc, char *const argv[],
                            const gsh_history_store *history,
                            const fc_invocation *invocation,
                            uint64_t eligible_last, uint64_t *first,
                            uint64_t *last, const gsh_builtin_io *io)
{
    if (argv == NULL) return -1;
    if (invocation == NULL || io == NULL) {
        return -1;
    }
    size_t operands = argc - invocation->operand;

    if (operands > 2U || (invocation->substitute && operands > 1U)) {
        return fc_error(io, "too many operands");
    }
    if (fc_default_range(history, invocation, eligible_last,
                         first, last) == -1) {
        return fc_error(io, "history is empty");
    }
    if (operands >= 1U &&
        resolve_fc_event(history, argv[invocation->operand], eligible_last,
                         first) == -1) {
        return fc_error(io, "history event not found");
    }
    if (operands == 2U &&
        resolve_fc_event(history, argv[invocation->operand + 1U],
                         eligible_last, last) == -1) {
        return fc_error(io, "history event not found");
    } else if (operands == 1U || invocation->substitute) {
        *last = *first;
    }
    return 0;
}

static int write_fc_listing(const gsh_history_store *history,
                            uint64_t first, uint64_t last,
                            const fc_invocation *invocation,
                            const gsh_builtin_io *io)
{
    if (invocation == NULL) return -1;
    if (history == NULL) {
        return -1;
    }
    int64_t step = first <= last ? 1 : -1;
    uint64_t event = first;
    size_t visited;

    if (invocation->reverse) {
        uint64_t swap = first;

        first = last;
        last = swap;
        event = first;
        step = first <= last ? 1 : -1;
    }
    for (visited = 0; visited < GSH_HISTORY_CAP; visited++) {
        size_t length;
        const char *entry = gsh_history_event(history, event, &length);
        char prefix[32];
        int prefix_length = invocation->suppress_numbers
                                ? 0
                                : snprintf(prefix, sizeof(prefix),
                                           "%llu\t",
                                           (unsigned long long)event);

        if (entry == NULL || prefix_length < 0 ||
            (size_t)prefix_length >= sizeof(prefix) ||
            (prefix_length != 0 &&
             gsh_builtin_output(io, STDOUT_FILENO, prefix,
                        (size_t)prefix_length) != 0) ||
            gsh_builtin_output(io, STDOUT_FILENO, entry, length) != 0 ||
            gsh_builtin_output(io, STDOUT_FILENO, "\n", 1) != 0) {
            return 1;
        }
        if (event == last) {
            return 0;
        }
        event = step > 0 ? event + 1U : event - 1U;
    }
    return fc_error(io, "history range exceeds capacity");
}

static int append_fc_commands(const gsh_history_store *history,
                              uint64_t first, uint64_t last, bool reverse,
                              char *command, size_t capacity,
                              size_t *command_length)
{
    if (command == NULL || command_length == NULL || history == NULL) {
        return -1;
    }
    int64_t step = first <= last ? 1 : -1;
    uint64_t event = first;
    size_t used = 0;
    size_t visited;

    if (reverse) {
        uint64_t swap = first;

        first = last;
        last = swap;
        event = first;
        step = first <= last ? 1 : -1;
    }
    for (visited = 0; visited < GSH_HISTORY_CAP; visited++) {
        size_t length;
        const char *entry = gsh_history_event(history, event, &length);

        if (entry == NULL || length + (used == 0 ? 0U : 1U) >=
                                 capacity - used) {
            errno = E2BIG;
            return -1;
        }
        if (used != 0) command[used++] = '\n';
        (void)memcpy(command + used, entry, length);
        used += length;
        if (event == last) {
            command[used] = '\0';
            *command_length = used;
            return 0;
        }
        event = step > 0 ? event + 1U : event - 1U;
    }
    errno = E2BIG;
    return -1;
}

static int fc_substitute(const char *replacement, char *command,
                         size_t capacity, size_t *length)
{
    if (length == NULL) return -1;
    if (command == NULL || replacement == NULL) {
        return -1;
    }
    const char *separator = strchr(replacement, '=');
    char original[GSH_HISTORY_ENTRY_CAP];
    char old[GSH_HISTORY_ENTRY_CAP];
    const char *match;
    size_t old_length;
    size_t new_length;
    size_t prefix;
    size_t suffix;

    if (separator == NULL || *length >= sizeof(original)) {
        return separator == NULL ? 0 : -1;
    }
    old_length = (size_t)(separator - replacement);
    if (old_length >= sizeof(old)) {
        errno = E2BIG;
        return -1;
    }
    (void)memcpy(original, command, *length + 1U);
    (void)memcpy(old, replacement, old_length);
    old[old_length] = '\0';
    new_length = strlen(separator + 1U);
    match = old_length == 0 ? original : strstr(original, old);
    if (match == NULL) {
        errno = ENOENT;
        return -1;
    }
    prefix = (size_t)(match - original);
    suffix = *length - prefix - old_length;
    if (prefix + new_length + suffix >= capacity) {
        errno = E2BIG;
        return -1;
    }
    (void)memcpy(command, original, prefix);
    (void)memcpy(command + prefix, separator + 1U, new_length);
    (void)memcpy(command + prefix + new_length, match + old_length, suffix);
    *length = prefix + new_length + suffix;
    command[*length] = '\0';
    return 1;
}

static const char *fc_editor(const fc_invocation *invocation,
                             const gsh_variable_store *variables)
{
    if (invocation == NULL) return NULL;
    if (variables == NULL) {
        return NULL;
    }
    bool found;
    const char *value;

    if (invocation->editor != NULL) return invocation->editor;
    value = gsh_variables_lookup(variables, "FCEDIT", 6, &found);
    if (found && value != NULL && value[0] != '\0') return value;
    value = gsh_variables_lookup(variables, "EDITOR", 6, &found);
    return found && value != NULL && value[0] != '\0' ? value : "ed";
}

static int write_editor_input(int descriptor, const char *command,
                              size_t length)
{
    if (command == NULL) {
        return -1;
    }
    size_t written = 0;
    size_t attempt;

    for (attempt = 0; attempt < GSH_FC_IO_ATTEMPT_CAP && written < length;
         attempt++) {
        ssize_t count = write(descriptor, command + written,
                              length - written);

        if (count > 0) {
            written += (size_t)count;
        } else if (count == -1 && errno != EINTR) {
            return -1;
        }
    }
    if (written == length) return 0;
    errno = EAGAIN;
    return -1;
}

static int read_editor_output(int descriptor, char *command,
                              size_t capacity, size_t *length)
{
    if (command == NULL || length == NULL) {
        return -1;
    }
    size_t used = 0;
    size_t attempt;
    char excess;

    for (attempt = 0; attempt < GSH_FC_IO_ATTEMPT_CAP && used < capacity;
         attempt++) {
        ssize_t count = read(descriptor, command + used, capacity - used);

        if (count > 0) {
            if (memchr(command + used, '\0', (size_t)count) != NULL) {
                errno = EINVAL;
                return -1;
            }
            used += (size_t)count;
        } else if (count == 0) {
            command[used] = '\0';
            *length = used;
            return 0;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    for (attempt = 0; attempt < GSH_FC_EXCESS_RETRY_CAP; attempt++) {
        ssize_t count = read(descriptor, &excess, 1U);

        if (count == 0) {
            command[used] = '\0';
            *length = used;
            return 0;
        }
        if (count > 0) {
            errno = E2BIG;
            return -1;
        }
        if (errno != EINTR) return -1;
    }
    errno = EINTR;
    return -1;
}

/* ── The Editor Is an Operand, Never a Shell Delegate ─────────────────
 * POSIX fc deliberately invokes an editor, but passing its command through
 * another shell would reopen the compatibility bridge and add injection risk.
 * gsh creates one private file, execs the selected editor name with that file
 * as its only operand, and reads the result through a fixed source capacity.
 * Every descriptor and pathname is cleaned on success, failure, or signal.
 * The edited text is later parsed by gsh's own bounded evaluator.
 * ────────────────────────────────────────────── */
static int run_fc_editor(const char *editor, char *command,
                         size_t capacity, size_t *length)
{
    if (length == NULL) return -1;
    if (editor == NULL) {
        return -1;
    }
    char path[] = "/tmp/gsh-fc.XXXXXX";
    char *arguments[GSH_FC_EDITOR_ARGUMENT_CAP];
    int descriptor = mkstemp(path);
    pid_t pid;
    int wait_status;
    int status = -1;

    if (descriptor == -1 ||
        write_editor_input(descriptor, command, *length) == -1 ||
        close(descriptor) == -1) {
        if (descriptor >= 0) (void)close(descriptor);
        (void)unlink(path);
        return -1;
    }
    arguments[0] = (char *)editor;
    arguments[1] = path;
    arguments[2] = NULL;
    pid = fork();
    if (pid == 0) {
        execvp(editor, arguments);
        _exit(126);
    }
    if (pid > 0) {
        do {
            status = waitpid(pid, &wait_status, 0) == pid ? 0 : -1;
        } while (status == -1 && errno == EINTR);
        if (status == 0 && (!WIFEXITED(wait_status) ||
                            WEXITSTATUS(wait_status) != 0)) status = -1;
    }
    descriptor = status == 0 ? open(path, O_RDONLY | O_CLOEXEC) : -1;
    if (descriptor >= 0) {
        status = read_editor_output(descriptor, command, capacity - 1U,
                                    length);
        if (close(descriptor) == -1) status = -1;
    } else status = -1;
    (void)unlink(path);
    return status;
}

int gsh_builtin_fc_prepare(size_t argc, char *const argv[],
                           const gsh_history_store *history,
                           const gsh_variable_store *variables,
                           bool exclude_newest, char *command,
                           size_t command_capacity,
                           gsh_fc_result *result,
                           const gsh_builtin_io *io)
{
    if (argv == NULL || history == NULL || io == NULL) {
        return -1;
    }
    fc_invocation invocation;
    uint64_t newest = gsh_history_newest_event(history);
    uint64_t eligible_last;
    uint64_t first = 0;
    uint64_t last = 0;
    int status;

    if (history == NULL || variables == NULL || command == NULL ||
        command_capacity < GSH_HISTORY_ENTRY_CAP || result == NULL) {
        return fc_error(io, "history unavailable");
    }
    (void)memset(result, 0, sizeof(*result));
    status = parse_fc_options(argc, argv, &invocation, io);
    if (status != 0) return status;
    if (invocation.substitute && invocation.operand < argc &&
        strchr(argv[invocation.operand], '=') != NULL) {
        invocation.replacement = argv[invocation.operand++];
    }
    if (newest == 0 || (exclude_newest && newest == 1U))
        return fc_error(io, "history is empty");
    eligible_last = exclude_newest ? newest - 1U : newest;
    status = fc_resolve_range(argc, argv, history, &invocation,
                              eligible_last, &first, &last, io);
    if (status != 0) return status;
    if (invocation.list)
        return write_fc_listing(history, first, last, &invocation, io);
    if (append_fc_commands(history, first, last, invocation.reverse,
                           command, command_capacity,
                           &result->command_length) == -1)
        return fc_error(io, "selected commands exceed capacity");
    if (invocation.substitute && invocation.replacement != NULL) {
        if (fc_substitute(invocation.replacement, command,
                          command_capacity, &result->command_length) == -1)
            return fc_error(io, "substitution failed");
    } else if (!invocation.substitute &&
               strcmp(fc_editor(&invocation, variables), "-") != 0 &&
               run_fc_editor(fc_editor(&invocation, variables), command,
                             command_capacity,
                             &result->command_length) == -1) {
        return fc_error(io, "editor failed");
    }
    result->execute = true;
    return 0;
}
