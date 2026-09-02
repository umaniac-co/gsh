#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_stateful.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define require(condition) (condition)

enum {
    GSH_READ_RECORD_CAP = 4096,
    GSH_READ_VARIABLE_CAP = GSH_VARIABLE_JOURNAL_CAP,
    GSH_READ_SCAN_CAP = GSH_READ_RECORD_CAP * 2,
};

typedef struct {
    char text[GSH_READ_RECORD_CAP];
    unsigned char escaped[GSH_READ_RECORD_CAP];
    size_t length;
    bool eof;
    bool overflow;
} read_record;

typedef struct {
    size_t offsets[GSH_READ_VARIABLE_CAP];
    size_t lengths[GSH_READ_VARIABLE_CAP];
    char text[GSH_READ_RECORD_CAP + GSH_READ_VARIABLE_CAP];
    size_t used;
} read_values;

typedef struct {
    const char *name;
    size_t name_length;
    const char *value;
    size_t value_length;
    bool unset;
} variable_update;

/* ── Blocking Input Commits as One Bounded Change ─────────────────
 * A read builtin cannot safely mutate variables as bytes arrive: EOF,
 * cancellation, or an invalid final name would expose a partial record.
 * The isolated evaluator first captures one capped logical record, then builds
 * every value in a second capped arena.  A scratch variable store and journal
 * are validated before either live representation changes.  The reactor never
 * blocks, and a failed read leaves its owning shell state byte-for-byte intact.
 * ────────────────────────────────────────────── */
static int read_byte(unsigned char *byte)
{
    if (byte == NULL) {
        return -1;
    }
    size_t attempts;

    for (attempts = 0; attempts < 16U; attempts++) {
        ssize_t count = read(STDIN_FILENO, byte, 1);

        if (count == 1) {
            return 1;
        }
        if (count == 0) {
            return 0;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
    errno = EINTR;
    return -1;
}

static int drain_read_record(bool raw)
{
    size_t scanned;

    for (scanned = 0; scanned < GSH_READ_SCAN_CAP; scanned++) {
        unsigned char byte;
        int status = read_byte(&byte);

        if (status <= 0 || byte == '\n') {
            return status < 0 ? -1 : 0;
        }
        if (!raw && byte == '\\') {
            status = read_byte(&byte);
            if (status <= 0) return status < 0 ? -1 : 0;
            if (byte == '\n') continue;
        }
    }
    errno = E2BIG;
    return -1;
}

static int append_read_byte(read_record *record, unsigned char byte,
                            bool escaped)
{
    if (record == NULL) return -1;
    if (record->length + 1U >= GSH_READ_RECORD_CAP) {
        record->overflow = true;
        return -1;
    }
    record->text[record->length] = (char)byte;
    record->escaped[record->length] = escaped ? 1U : 0U;
    record->length++;
    return 0;
}

static void prompt_read_continuation(
    const gsh_variable_store *variables, const gsh_builtin_io *io)
{
    if (variables == NULL) {
        return;
    }
    bool found;
    const char *prompt;

    if (!isatty(STDIN_FILENO) || !gsh_builtin_io_valid(io)) return;
    prompt = gsh_variables_lookup(variables, "PS2", 3U, &found);
    if (!found || prompt == NULL) prompt = "> ";
    (void)gsh_builtin_output(io, STDERR_FILENO, prompt, strlen(prompt));
}

static int capture_read_record(bool raw,
                               const gsh_variable_store *variables,
                               const gsh_builtin_io *io,
                               read_record *record)
{
    if (record == NULL) {
        return -1;
    }
    size_t scanned;

    (void)memset(record, 0, sizeof(*record));
    if (io == NULL || variables == NULL) {
        return -1;
    }
    for (scanned = 0; scanned < GSH_READ_SCAN_CAP; scanned++) {
        unsigned char byte;
        int status = read_byte(&byte);

        if (status == 0) {
            record->eof = true;
            break;
        }
        if (status < 0) {
            return -1;
        }
        if (byte == '\n') {
            break;
        }
        if (!raw && byte == '\\') {
            status = read_byte(&byte);
            if (status == 0) {
                record->eof = true;
                break;
            }
            if (status < 0) {
                return -1;
            }
            if (byte == '\n') {
                prompt_read_continuation(variables, io);
                continue;
            }
            if (append_read_byte(record, byte, true) == -1) {
                return drain_read_record(raw);
            }
        } else if (append_read_byte(record, byte, false) == -1) {
            return drain_read_record(raw);
        }
    }
    if (scanned == GSH_READ_SCAN_CAP) {
        errno = E2BIG;
        return -1;
    }
    record->text[record->length] = '\0';
    return record->overflow ? -1 : 0;
}

static bool ifs_contains(const char *ifs, unsigned char byte)
{
    if (ifs == NULL) {
        return false;
    }
    size_t index;

    for (index = 0; ifs[index] != '\0'; index++) {
        if ((unsigned char)ifs[index] == byte) {
            return true;
        }
    }
    return false;
}

static bool ifs_whitespace(const char *ifs, unsigned char byte)
{
    if (ifs == NULL) {
        return false;
    }
    return (byte == ' ' || byte == '\t' || byte == '\n') &&
           ifs_contains(ifs, byte);
}

static size_t skip_ifs_whitespace(const read_record *record,
                                  const char *ifs, size_t offset)
{
    if (ifs == NULL || record == NULL) {
        return 0U;
    }
    while (offset < record->length && record->escaped[offset] == 0U &&
           ifs_whitespace(ifs, (unsigned char)record->text[offset])) {
        offset++;
    }
    return offset;
}

static int store_read_value(read_values *values, size_t index,
                            const char *text, size_t length)
{
    if (index >= GSH_READ_VARIABLE_CAP ||
        length + 1U > sizeof(values->text) - values->used) {
        errno = E2BIG;
        return -1;
    }
    values->offsets[index] = values->used;
    values->lengths[index] = length;
    (void)memcpy(values->text + values->used, text, length);
    values->text[values->used + length] = '\0';
    values->used += length + 1U;
    return 0;
}

static size_t read_field_end(const read_record *record, const char *ifs,
                             size_t offset)
{
    if (ifs == NULL || record == NULL) {
        return 0U;
    }
    while (offset < record->length &&
           (record->escaped[offset] != 0U ||
            !ifs_contains(ifs, (unsigned char)record->text[offset]))) {
        offset++;
    }
    return offset;
}

static size_t consume_ifs_separator(const read_record *record,
                                    const char *ifs, size_t offset)
{
    if (ifs == NULL || record == NULL) {
        return 0U;
    }
    bool white = offset < record->length &&
                 ifs_whitespace(ifs, (unsigned char)record->text[offset]);

    if (offset < record->length && record->escaped[offset] == 0U) {
        offset++;
    }
    if (white) {
        offset = skip_ifs_whitespace(record, ifs, offset);
        if (offset < record->length && record->escaped[offset] == 0U &&
            ifs_contains(ifs, (unsigned char)record->text[offset]) &&
            !ifs_whitespace(ifs, (unsigned char)record->text[offset])) {
            offset++;
            offset = skip_ifs_whitespace(record, ifs, offset);
        }
    } else {
        offset = skip_ifs_whitespace(record, ifs, offset);
    }
    return offset;
}

static size_t trim_last_value(const read_record *record, const char *ifs,
                              size_t begin)
{
    if (ifs == NULL || record == NULL) {
        return 0U;
    }
    size_t end = record->length;

    while (end > begin && record->escaped[end - 1U] == 0U &&
           ifs_whitespace(ifs, (unsigned char)record->text[end - 1U])) {
        end--;
    }
    return end;
}

static int split_read_values(const read_record *record, const char *ifs,
                             size_t variable_count, read_values *values)
{
    if (ifs == NULL || record == NULL || values == NULL) {
        return -1;
    }
    size_t offset = skip_ifs_whitespace(record, ifs, 0);
    size_t variable;

    (void)memset(values, 0, sizeof(*values));
    for (variable = 0; variable < variable_count; variable++) {
        size_t begin = offset;
        size_t end;

        if (variable + 1U == variable_count) {
            end = trim_last_value(record, ifs, begin);
            offset = record->length;
        } else if (ifs[0] == '\0') {
            end = variable == 0 ? record->length : offset;
            offset = record->length;
        } else {
            end = read_field_end(record, ifs, offset);
            offset = end < record->length
                         ? consume_ifs_separator(record, ifs, end)
                         : end;
        }
        if (store_read_value(values, variable, record->text + begin,
                             end - begin) == -1) {
            return -1;
        }
    }
    return 0;
}

static int apply_one_update(gsh_variable_store *store,
                            gsh_variable_journal *journal,
                            const variable_update *update)
{
    if (update == NULL) return -1;
    if (store == NULL) {
        return -1;
    }
    int changed;

    if (update->unset) {
        changed = gsh_variables_unset(store, update->name,
                                      update->name_length);
        if (changed != -1 && journal != NULL) {
            changed = gsh_variable_journal_record_unset(
                journal, update->name, update->name_length);
        }
    } else {
        changed = gsh_variables_set(store, update->name,
                                    update->name_length, update->value,
                                    update->value_length, 0, 0);
        if (changed != -1 && journal != NULL) {
            changed = gsh_variable_journal_record(
                journal, update->name, update->name_length,
                update->value, update->value_length, 0, 0);
        }
    }
    return changed;
}

static int apply_updates_atomically(gsh_variable_store *variables,
                                    gsh_variable_store *scratch,
                                    gsh_variable_journal *journal,
                                    const variable_update *updates,
                                    size_t count)
{
    if (updates == NULL) return -1;
    gsh_variable_journal journal_scratch;
    gsh_variable_journal *candidate = NULL;
    size_t index;

    if (variables == NULL || scratch == NULL ||
        count > GSH_VARIABLE_JOURNAL_CAP) {
        errno = EINVAL;
        return -1;
    }
    (void)memcpy(scratch, variables, sizeof(*scratch));
    if (journal != NULL) {
        (void)memcpy(&journal_scratch, journal, sizeof(journal_scratch));
        candidate = &journal_scratch;
    }
    for (index = 0; index < count; index++) {
        if (!gsh_variable_name_is_valid(updates[index].name,
                                        updates[index].name_length) ||
            apply_one_update(scratch, candidate, &updates[index]) == -1) {
            return -1;
        }
    }
    (void)memcpy(variables, scratch, sizeof(*variables));
    if (journal != NULL) {
        (void)memcpy(journal, candidate, sizeof(*journal));
    }
    return 0;
}

static const char *read_ifs(const gsh_variable_store *variables)
{
    if (variables == NULL) {
        return NULL;
    }
    bool found;
    const char *ifs = gsh_variables_lookup(variables, "IFS", 3, &found);

    return found && ifs != NULL ? ifs : " \t\n";
}

int gsh_builtin_read(size_t argc, char *const argv[],
                     const gsh_variable_store *lookup_variables,
                     gsh_variable_store *variables,
                     gsh_variable_store *scratch,
                     gsh_variable_journal *journal,
                     const gsh_builtin_io *io)
{
    if (argv == NULL || io == NULL) {
        return -1;
    }
    read_record record;
    read_values values;
    variable_update updates[GSH_READ_VARIABLE_CAP];
    size_t first = 1U;
    size_t count;
    size_t index;
    bool raw = false;

    while (first < argc && argv[first][0] == '-' &&
           argv[first][1] != '\0') {
        if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        }
        if (strcmp(argv[first], "-r") != 0) {
            return gsh_builtin_error(io, "read", "invalid option");
        }
        raw = true;
        first++;
    }
    count = argc - first;
    if (count == 0 || count > GSH_READ_VARIABLE_CAP) {
        return gsh_builtin_error(io, "read", "invalid variable count");
    }
    for (index = 0; index < count; index++) {
        if (!gsh_variable_name_is_valid(argv[first + index],
                                        strlen(argv[first + index]))) {
            return gsh_builtin_error(io, "read", "invalid variable name");
        }
    }
    if (capture_read_record(raw, lookup_variables, io, &record) == -1 ||
        record.overflow ||
        split_read_values(&record, read_ifs(lookup_variables), count,
                          &values) == -1) {
        return gsh_builtin_error(
            io, "read", errno == E2BIG || errno == EOVERFLOW ||
                                record.overflow
                            ? "input record exceeds capacity"
                            : "input error");
    }
    for (index = 0; index < count; index++) {
        updates[index].name = argv[first + index];
        updates[index].name_length = strlen(updates[index].name);
        updates[index].value = values.text + values.offsets[index];
        updates[index].value_length = values.lengths[index];
        updates[index].unset = false;
    }
    if (apply_updates_atomically(variables, scratch, journal, updates,
                                 count) == -1) {
        return gsh_builtin_error(io, "read", "variable transaction failed");
    }
    return record.eof ? 1 : 0;
}

static int parse_optind(const gsh_variable_store *variables)
{
    if (variables == NULL) {
        return -1;
    }
    bool found;
    const char *text = gsh_variables_lookup(variables, "OPTIND", 6, &found);
    char *end;
    long value;

    if (!found || text == NULL) {
        return 1;
    }
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 1 ||
        value > GSH_POSITIONAL_CAP + 1) {
        return 1;
    }
    return (int)value;
}

static void getopts_arguments(size_t argc, char *const argv[],
                              const gsh_positional_store *positionals,
                              char *storage[GSH_POSITIONAL_CAP],
                              char *const **arguments, size_t *count)
{
    if (arguments == NULL || argv == NULL || count == NULL || positionals == NULL || storage == NULL) {
        return;
    }
    if (argc > 3U) {
        *arguments = argv + 3U;
        *count = argc - 3U;
    } else {
        gsh_positionals_view(positionals, storage);
        *arguments = storage;
        *count = gsh_positionals_count(positionals);
    }
}

static int getopts_diagnostic(const gsh_builtin_io *io, char option,
                              bool missing)
{
    if (io == NULL) {
        return -1;
    }
    char message[64];
    int length = snprintf(message, sizeof(message),
                          missing ? "option requires an argument -- %c"
                                  : "illegal option -- %c", option);

    if (length < 0 || (size_t)length >= sizeof(message)) {
        return 1;
    }
    return gsh_builtin_error(io, "getopts", message);
}

static int commit_getopts_result(gsh_variable_store *variables,
                                 gsh_variable_store *scratch,
                                 gsh_variable_journal *journal,
                                 const char *name, char result,
                                 bool optarg_set, const char *optarg,
                                 size_t optarg_length, unsigned int optind)
{
    if (name == NULL || optarg == NULL || scratch == NULL ||
        variables == NULL) {
        return -1;
    }
    variable_update updates[3];
    char result_text[2] = {result, '\0'};
    char index_text[16];
    int length = snprintf(index_text, sizeof(index_text), "%u", optind);

    if (length < 0 || (size_t)length >= sizeof(index_text)) {
        errno = EOVERFLOW;
        return -1;
    }
    updates[0] = (variable_update){name, strlen(name), result_text, 1, false};
    updates[1] = (variable_update){"OPTARG", 6, optarg, optarg_length,
                                   !optarg_set};
    updates[2] = (variable_update){"OPTIND", 6, index_text,
                                   (size_t)length, false};
    return apply_updates_atomically(variables, scratch, journal, updates, 3);
}

static const char *getopts_find_option(const char *optstring, char option)
{
    if (optstring == NULL) {
        return NULL;
    }
    const char *cursor = optstring[0] == ':' ? optstring + 1U : optstring;

    while (*cursor != '\0') {
        if (*cursor == option) {
            return cursor;
        }
        cursor += cursor[1] == ':' ? 2U : 1U;
    }
    return NULL;
}

static int finish_getopts(
    gsh_variable_store *variables, gsh_variable_store *scratch,
    gsh_variable_journal *journal, const char *name, char result,
    bool optarg_set, const char *optarg, size_t optarg_length,
    gsh_shell_options *next, gsh_shell_options *options,
    const gsh_builtin_io *io, int success_status)
{
    if (io == NULL) {
        return -1;
    }
    if (!require(variables != NULL && scratch != NULL && name != NULL)) {
        return gsh_builtin_error(io, "getopts", "invalid state");
    }
    if (!require(next != NULL && options != NULL && optarg != NULL)) {
        return gsh_builtin_error(io, "getopts", "invalid state");
    }
    if (commit_getopts_result(
            variables, scratch, journal, name, result, optarg_set, optarg,
            optarg_length, next->getopts_index) == -1) {
        return gsh_builtin_error(io, "getopts", "state update failed");
    }
    next->getopts_optind_generation = gsh_variables_value_generation(
        variables, "OPTIND", 6U);
    *options = *next;
    return success_status;
}

static int handle_invalid_getopts(
    const char *candidate, char option, bool silent,
    gsh_variable_store *variables, gsh_variable_store *scratch,
    gsh_variable_journal *journal, const char *name,
    gsh_shell_options *next, gsh_shell_options *options,
    const gsh_builtin_io *io)
{
    if (io == NULL) {
        return -1;
    }
    bool cluster_done;
    int result;

    if (!require(candidate != NULL && name != NULL && next != NULL)) return 1;
    if (!require(options != NULL && variables != NULL && scratch != NULL)) {
        return 1;
    }
    cluster_done = candidate[next->getopts_offset + 1U] == '\0';
    next->getopts_offset++;
    if (cluster_done) {
        next->getopts_index++;
        next->getopts_offset = 1U;
    }
    result = finish_getopts(variables, scratch, journal, name, '?', silent,
                            &option, 1U, next, options, io, 0);
    if (result != 0) return result;
    if (!silent) (void)getopts_diagnostic(io, option, false);
    return 0;
}

static int handle_missing_getopts_value(
    char option, bool silent, gsh_variable_store *variables,
    gsh_variable_store *scratch, gsh_variable_journal *journal,
    const char *name, gsh_shell_options *next,
    gsh_shell_options *options, const gsh_builtin_io *io)
{
    int result;

    if (name == NULL || next == NULL || options == NULL) return 1;
    next->getopts_index++;
    next->getopts_offset = 1U;
    result = finish_getopts(
        variables, scratch, journal, name, silent ? ':' : '?', silent,
        &option, silent ? 1U : 0U, next, options, io, 0);
    if (result != 0) return result;
    if (!silent) (void)getopts_diagnostic(io, option, true);
    return 0;
}

int gsh_builtin_getopts(size_t argc, char *const argv[],
                        const gsh_variable_store *lookup_variables,
                        gsh_variable_store *variables,
                        gsh_variable_store *scratch,
                        gsh_variable_journal *journal,
                        const gsh_positional_store *positionals,
                        gsh_shell_options *options,
                        const gsh_builtin_io *io)
{
    if (argv == NULL) return 125;
    if (io == NULL || lookup_variables == NULL || positionals == NULL) {
        return -1;
    }
    char *positional_view[GSH_POSITIONAL_CAP];
    char *const *arguments;
    gsh_shell_options next;
    size_t argument_count;
    unsigned int index;
    uint64_t optind_generation;
    const char *candidate;
    const char *definition;
    char option;
    bool silent;

    if (argc < 3U || !gsh_variable_name_is_valid(argv[2], strlen(argv[2])) ||
        variables == NULL || scratch == NULL || options == NULL) {
        return gsh_builtin_error(io, "getopts", "invalid operands");
    }
    next = *options;
    getopts_arguments(argc, argv, positionals, positional_view,
                      &arguments, &argument_count);
    index = (unsigned int)parse_optind(lookup_variables);
    optind_generation = gsh_variables_value_generation(
        lookup_variables, "OPTIND", 6U);
    if (optind_generation != next.getopts_optind_generation ||
        index != next.getopts_index) {
        next.getopts_index = (uint16_t)index;
        next.getopts_offset = 1U;
    }
    if (index == 0 || index > argument_count) {
        return finish_getopts(variables, scratch, journal, argv[2], '?',
                              false, "", 0U, &next, options, io, 1);
    }
    candidate = arguments[index - 1U];
    if (strcmp(candidate, "--") == 0) {
        next.getopts_index = (uint16_t)(index + 1U);
        next.getopts_offset = 1U;
        return finish_getopts(variables, scratch, journal, argv[2], '?',
                              false, "", 0U, &next, options, io, 1);
    }
    if (candidate[0] != '-' || candidate[1] == '\0' ||
        next.getopts_offset >= strlen(candidate)) {
        return finish_getopts(variables, scratch, journal, argv[2], '?',
                              false, "", 0U, &next, options, io, 1);
    }
    option = candidate[next.getopts_offset];
    definition = getopts_find_option(argv[1], option);
    silent = argv[1][0] == ':';
    if (definition == NULL || option == ':') {
        return handle_invalid_getopts(
            candidate, option, silent, variables, scratch, journal,
            argv[2], &next, options, io);
    }
    if (definition[1] == ':') {
        const char *value = candidate + next.getopts_offset + 1U;

        if (*value != '\0') {
            next.getopts_index++;
        } else if (index < argument_count) {
            value = arguments[index];
            next.getopts_index += 2U;
        } else {
            return handle_missing_getopts_value(
                option, silent, variables, scratch, journal, argv[2],
                &next, options, io);
        }
        next.getopts_offset = 1U;
        return finish_getopts(variables, scratch, journal, argv[2], option,
                              true, value, strlen(value), &next, options,
                              io, 0);
    }
    next.getopts_offset++;
    if (candidate[next.getopts_offset] == '\0') {
        next.getopts_index++;
        next.getopts_offset = 1U;
    }
    return finish_getopts(variables, scratch, journal, argv[2], option,
                          false, "", 0U, &next, options, io, 0);
}
