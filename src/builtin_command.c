#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "builtin_command.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

enum {
    GSH_COMMAND_ARGUMENT_CAP = 128,
};

_Static_assert(GSH_COMMAND_PATH_CAP > 4,
               "command paths need room for a directory and name");

typedef enum {
    GSH_COMMAND_NOT_FOUND = 0,
    GSH_COMMAND_RESERVED,
    GSH_COMMAND_ALIAS,
    GSH_COMMAND_SPECIAL,
    GSH_COMMAND_FUNCTION,
    GSH_COMMAND_REGULAR,
    GSH_COMMAND_EXTERNAL,
} gsh_command_kind;

typedef struct {
    gsh_command_kind kind;
    const char *alias_value;
    char path[GSH_COMMAND_PATH_CAP];
} gsh_command_result;

static bool name_in_table(const char *name, size_t length,
                          const char *const table[], size_t count)
{
    size_t index;

    if (name == NULL || table == NULL) {
        return false;
    }
    for (index = 0; index < count; index++) {
        if (strlen(table[index]) == length &&
            memcmp(table[index], name, length) == 0) {
            return true;
        }
    }
    return false;
}

bool gsh_command_special_builtin_name(const char *name, size_t length)
{
    static const char *const names[] = {
        ".",      ":",      "break",  "continue", "eval", "exec",
        "exit",   "export", "readonly", "return", "set",  "shift",
        "times",  "trap",   "unset",
    };

    return name_in_table(name, length, names,
                         sizeof(names) / sizeof(names[0]));
}

static bool reserved_word_name(const char *name, size_t length)
{
    static const char *const names[] = {
        "!",    "{",    "}",    "case", "do",   "done", "elif",
        "else", "esac", "fi",   "for",  "if",   "in",   "then",
        "until", "while",
    };

    return name_in_table(name, length, names,
                         sizeof(names) / sizeof(names[0]));
}

static bool implemented_special_name(const char *name, size_t length)
{
    static const char *const names[] = {
        ".",      ":",      "break", "continue", "eval", "exec",
        "exit",   "export", "readonly", "return", "set", "shift",
        "times",  "trap",   "unset",
    };

    return name_in_table(name, length, names,
                         sizeof(names) / sizeof(names[0]));
}

static bool regular_builtin_name(const char *name, size_t length)
{
    static const char *const names[] = {
        "alias", "bg",      "cd",    "command", "false", "fg",
        "hash",  "help",    "pwd",   "rt",      "true",  "type",
        "ulimit", "umask",  "unalias", "wait",
    };

    return name_in_table(name, length, names,
                         sizeof(names) / sizeof(names[0]));
}

bool gsh_command_intrinsic_name(const char *name, size_t length)
{
    return implemented_special_name(name, length) ||
           regular_builtin_name(name, length);
}

/* ── One Bounded Search Defines Command Identity ────────────────────────
 * Command inspection once grew naturally beside each builtin dispatcher.
 * That would make aliases, functions, and executable paths disagree by mode.
 * A single resolver now follows the shell search order and caps every PATH
 * byte and candidate before touching the filesystem.  Inspection therefore
 * stays allocation-free and cannot monopolize the interactive reactor.
 * ─────────────────────────────────────────────────────────────── */
static gsh_command_result resolve_name(
    const char *name, const char *path, const gsh_alias_store *aliases,
    const gsh_function_store *functions, gsh_command_cache *cache,
    uint64_t path_generation, bool *cache_changed)
{
    gsh_command_result result = {0};
    size_t length = name == NULL
                        ? 0
                        : strnlen(name, GSH_COMMAND_PATH_CAP);

    if (length == GSH_COMMAND_PATH_CAP) {
        return result;
    }
    if (reserved_word_name(name, length)) {
        result.kind = GSH_COMMAND_RESERVED;
    } else if (aliases != NULL &&
               (result.alias_value = gsh_aliases_lookup(
                    aliases, name, length, NULL)) != NULL) {
        result.kind = GSH_COMMAND_ALIAS;
    } else if (implemented_special_name(name, length)) {
        result.kind = GSH_COMMAND_SPECIAL;
    } else if (functions != NULL &&
               gsh_functions_lookup(functions, name, length) != NULL) {
        result.kind = GSH_COMMAND_FUNCTION;
    } else if (regular_builtin_name(name, length)) {
        result.kind = GSH_COMMAND_REGULAR;
    } else {
        bool changed = false;
        int found = gsh_command_cache_resolve(
            cache, path_generation, name, path, false, &changed,
            result.path);

        if (changed && cache_changed != NULL) {
            *cache_changed = true;
        }
        if (found == 1) {
            result.kind = GSH_COMMAND_EXTERNAL;
        }
    }
    return result;
}

static int write_output(const gsh_builtin_io *io, int descriptor,
                        const char *text)
{
    return io->output(io->opaque, descriptor, text, strlen(text));
}

static int write_alias_definition(const gsh_builtin_io *io,
                                  const char *name, const char *value)
{
    const char *cursor = value;
    size_t remaining = strnlen(value, GSH_ALIAS_VALUE_CAP + 1U);
    size_t segments = 0;

    if (remaining > GSH_ALIAS_VALUE_CAP ||
        write_output(io, STDOUT_FILENO, "alias ") != 0 ||
        write_output(io, STDOUT_FILENO, name) != 0 ||
        write_output(io, STDOUT_FILENO, "='") != 0) {
        return 1;
    }
    while (remaining > 0 && segments < GSH_ALIAS_VALUE_CAP) {
        const char *quote = memchr(cursor, '\'', remaining);
        size_t length = quote == NULL ? remaining
                                      : (size_t)(quote - cursor);

        if ((length != 0 && io->output(io->opaque, STDOUT_FILENO, cursor,
                                       length) != 0) ||
            (quote != NULL &&
             write_output(io, STDOUT_FILENO, "'\\''") != 0)) {
            return 1;
        }
        if (quote == NULL) {
            remaining = 0;
            break;
        }
        cursor = quote + 1U;
        remaining -= length + 1U;
        segments++;
    }
    if (remaining != 0) {
        return 1;
    }
    return write_output(io, STDOUT_FILENO, "'\n") == 0 ? 0 : 1;
}

static int write_description(const gsh_builtin_io *io, const char *name,
                             const gsh_command_result *result,
                             bool verbose)
{
    static const char *const descriptions[] = {
        "not found",       "a shell reserved word", "an alias for ",
        "a special builtin", "a shell function",      "a regular builtin",
        "",
    };

    if (!verbose) {
        if (result->kind == GSH_COMMAND_ALIAS) {
            return write_alias_definition(io, name, result->alias_value);
        }
        return write_output(io, STDOUT_FILENO,
                            result->kind == GSH_COMMAND_EXTERNAL
                                ? result->path
                                : name) != 0 ||
                       write_output(io, STDOUT_FILENO, "\n") != 0;
    }
    if (write_output(io, STDOUT_FILENO, name) != 0 ||
        write_output(io, STDOUT_FILENO, " is ") != 0 ||
        write_output(io, STDOUT_FILENO,
                     descriptions[result->kind]) != 0) {
        return 1;
    }
    if (result->kind == GSH_COMMAND_ALIAS &&
        write_output(io, STDOUT_FILENO, result->alias_value) != 0) {
        return 1;
    }
    if (result->kind == GSH_COMMAND_EXTERNAL &&
        write_output(io, STDOUT_FILENO, result->path) != 0) {
        return 1;
    }
    return write_output(io, STDOUT_FILENO, "\n") == 0 ? 0 : 1;
}

gsh_command_invocation gsh_command_parse(size_t argc,
                                         char *const argv[])
{
    gsh_command_invocation invocation = {GSH_COMMAND_FORM_EXECUTE, 1U,
                                         false, false};
    size_t index = 1U;
    bool inspect = false;

    if (argc == 0 || argc > GSH_COMMAND_ARGUMENT_CAP || argv == NULL ||
        strcmp(argv[0], "command") != 0) {
        invocation.form = GSH_COMMAND_FORM_ERROR;
        return invocation;
    }
    while (index < argc && index < GSH_COMMAND_ARGUMENT_CAP &&
           argv[index][0] == '-' &&
           argv[index][1] != '\0') {
        const char *option = argv[index] + 1U;
        size_t scanned = 0;

        if (strcmp(argv[index], "--") == 0) {
            index++;
            break;
        }
        while (*option != '\0' && scanned < GSH_COMMAND_PATH_CAP) {
            if (*option == 'p') {
                invocation.use_default_path = true;
            } else if ((*option == 'v' || *option == 'V') && !inspect) {
                inspect = true;
                invocation.verbose = *option == 'V';
            } else {
                invocation.form = GSH_COMMAND_FORM_ERROR;
                return invocation;
            }
            option++;
            scanned++;
        }
        if (*option != '\0') {
            invocation.form = GSH_COMMAND_FORM_ERROR;
            return invocation;
        }
        index++;
    }
    invocation.first_operand = index;
    invocation.form = inspect ? GSH_COMMAND_FORM_INSPECT
                              : (index == argc ? GSH_COMMAND_FORM_EMPTY
                                               : GSH_COMMAND_FORM_EXECUTE);
    return invocation;
}

bool gsh_command_is_inspection_builtin(size_t argc,
                                       char *const argv[])
{
    gsh_command_invocation invocation;

    if (argc == 0 || argv == NULL) {
        return false;
    }
    if (strcmp(argv[0], "type") == 0) {
        return true;
    }
    if (strcmp(argv[0], "command") != 0) {
        return false;
    }
    invocation = gsh_command_parse(argc, argv);
    return invocation.form != GSH_COMMAND_FORM_EXECUTE;
}

static int inspect_operands(size_t argc, char *const argv[], size_t first,
                            bool verbose, const char *path,
                            const gsh_alias_store *aliases,
                            const gsh_function_store *functions,
                            gsh_command_cache *cache,
                            uint64_t path_generation,
                            bool *cache_changed,
                            const gsh_builtin_io *io)
{
    size_t index;
    int status = 0;

    if (path == NULL || io == NULL || io->output == NULL) {
        errno = EINVAL;
        return 125;
    }
    if (argc > GSH_COMMAND_ARGUMENT_CAP || first > argc) {
        errno = E2BIG;
        return 125;
    }
    for (index = first; index < argc &&
                        index < GSH_COMMAND_ARGUMENT_CAP; index++) {
        gsh_command_result result = resolve_name(
            argv[index], path, aliases, functions, cache,
            path_generation, cache_changed);

        if (result.kind == GSH_COMMAND_NOT_FOUND) {
            status = 1;
        } else if (write_description(io, argv[index], &result, verbose) !=
                   0) {
            return 1;
        }
    }
    return status;
}

int gsh_builtin_command_inspect(
    size_t argc, char *const argv[], const char *path,
    const char *default_path, const gsh_alias_store *aliases,
    const gsh_function_store *functions, gsh_command_cache *cache,
    uint64_t path_generation, bool cacheable, bool *cache_changed,
    const gsh_builtin_io *io)
{
    gsh_command_invocation invocation = gsh_command_parse(argc, argv);

    if (invocation.form == GSH_COMMAND_FORM_ERROR) {
        return gsh_builtin_error(io, "command", "invalid option");
    }
    if (invocation.form == GSH_COMMAND_FORM_EMPTY) {
        return 0;
    }
    if (invocation.form != GSH_COMMAND_FORM_INSPECT) {
        errno = EINVAL;
        return 125;
    }
    return inspect_operands(
        argc, argv, invocation.first_operand, invocation.verbose,
        invocation.use_default_path ? default_path : path,
        aliases, functions,
        cacheable && !invocation.use_default_path ? cache : NULL,
        path_generation, cache_changed, io);
}

int gsh_builtin_type(size_t argc, char *const argv[], const char *path,
                     const gsh_alias_store *aliases,
                     const gsh_function_store *functions,
                     gsh_command_cache *cache, uint64_t path_generation,
                     bool *cache_changed,
                     const gsh_builtin_io *io)
{
    size_t first = 1U;

    if (argc == 0 || argv == NULL || strcmp(argv[0], "type") != 0) {
        errno = EINVAL;
        return 125;
    }
    if (argc > GSH_COMMAND_ARGUMENT_CAP) {
        errno = E2BIG;
        return 125;
    }
    if (argc > 1U && strcmp(argv[1], "--") == 0) {
        first++;
    }
    if (first == argc) {
        return gsh_builtin_error(io, "type", "missing name");
    }
    if (argv[first][0] == '-' && argv[first][1] != '\0') {
        return gsh_builtin_error(io, "type", "invalid option");
    }
    return inspect_operands(argc, argv, first, true, path, aliases,
                            functions, cache, path_generation,
                            cache_changed, io);
}

static int hash_report(const gsh_command_cache *cache,
                       const gsh_builtin_io *io)
{
    size_t index;

    for (index = 0; index < gsh_command_cache_count(cache); index++) {
        const char *name = gsh_command_cache_name(cache, index);
        const char *path = gsh_command_cache_path(cache, index);

        if (write_output(io, STDOUT_FILENO, name) != 0 ||
            write_output(io, STDOUT_FILENO, "=") != 0 ||
            write_output(io, STDOUT_FILENO, path) != 0 ||
            write_output(io, STDOUT_FILENO, "\n") != 0) {
            return 1;
        }
    }
    return 0;
}

/* -- Explicit Hashing Shares the Execution Resolver ------------------
 * Builtins and functions never enter the table. Every external operand is
 * resolved by the same bounded search used by execution, while -r and PATH
 * epochs make invalidation observable immediately in the current shell.
 * -------------------------------------------------------------------- */
int gsh_builtin_hash(size_t argc, char *const argv[], const char *path,
                     const gsh_function_store *functions,
                     gsh_command_cache *cache, uint64_t path_generation,
                     bool *cache_changed, const gsh_builtin_io *io)
{
    size_t first = 1U;
    size_t index;
    bool reset = false;
    int status = 0;

    if (cache_changed != NULL) {
        *cache_changed = false;
    }
    if (argc == 0 || argc > GSH_COMMAND_ARGUMENT_CAP || argv == NULL ||
        path == NULL || cache == NULL || io == NULL || io->output == NULL ||
        strcmp(argv[0], "hash") != 0) {
        errno = EINVAL;
        return 125;
    }
    if (gsh_command_cache_sync(cache, path_generation) &&
        cache_changed != NULL) {
        *cache_changed = true;
    }
    if (first < argc && strcmp(argv[first], "--") == 0) {
        first++;
    } else if (first < argc && strcmp(argv[first], "-r") == 0) {
        reset = true;
        first++;
        if (first < argc && strcmp(argv[first], "--") == 0) {
            first++;
        }
    } else if (first < argc && argv[first][0] == '-' &&
               argv[first][1] != '\0') {
        return gsh_builtin_error(io, "hash", "invalid option");
    }
    if (reset) {
        if (first != argc) {
            return gsh_builtin_error(io, "hash", "-r does not accept operands");
        }
        gsh_command_cache_clear(cache, path_generation);
        if (cache_changed != NULL) {
            *cache_changed = true;
        }
        return 0;
    }
    if (first == argc) {
        return hash_report(cache, io);
    }
    for (index = first; index < argc; index++) {
        size_t length = strnlen(argv[index], GSH_COMMAND_PATH_CAP);
        bool changed = false;
        char resolved[GSH_COMMAND_PATH_CAP];
        int found;

        if (length == 0 || length == GSH_COMMAND_PATH_CAP ||
            strchr(argv[index], '/') != NULL ||
            memchr(argv[index], '\n', length) != NULL) {
            status = gsh_builtin_error(io, "hash", "invalid utility name");
            continue;
        }
        if (gsh_command_intrinsic_name(argv[index], length) ||
            (functions != NULL &&
             gsh_functions_lookup(functions, argv[index], length) != NULL)) {
            continue;
        }
        found = gsh_command_cache_resolve(
            cache, path_generation, argv[index], path, true, &changed,
            resolved);
        if (changed && cache_changed != NULL) {
            *cache_changed = true;
        }
        if (found != 1) {
            const char *message =
                found == 0 ? "utility not found"
                           : (errno == ENOSPC
                                  ? "command cache capacity exceeded"
                                  : "utility lookup failed");

            status = gsh_builtin_error(
                io, "hash", message);
        }
    }
    return status;
}
