#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "shell_config.h"

#include "history_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h> /* CANON-INCLUDE: macos */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    GSH_CONFIG_FILE_CAP = 65536,
};

static const char initial_config[] =
    "config.version = 1\n"
    "\n"
    "shell.async_repl.enabled = true\n"
    "shell.completion.enabled = true\n"
    "terminal.actions = auto\n"
    "terminal.actions.path_detection = safe\n"
    "terminal.images = auto\n"
    "shell.preview.editor = auto\n"
    "shell.history.enabled = true\n"
    "shell.history.max_entries = 1024\n"
    "shell.history.deduplicate = false\n"
    "shell.history.store_failed = true\n"
    "shell.history.ignore_space = true\n";

typedef struct {
    char *text;
    size_t length;
    size_t line;
    bool version_seen;
    bool seen[14];
} config_parser;

/* ── Configuration Is Parsed Before It Can Mutate State ───────
 * Sourcing shell text would let configuration execute before validation.
 * The gshrc reader instead scans one bounded buffer into a candidate value.
 * Duplicate, unknown, malformed, and unsafe inputs reject that whole candidate.
 * Defaults remain usable when loading fails, so configuration cannot brick gsh.
 * Static limits and a single pass keep normal startup work small and measurable.
 * ─────────────────────────────────────────────── */

static void set_diagnostic(gsh_shell_config *config, const char *format, ...)
{
    if (config == NULL || format == NULL) {
        return;
    }
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(config->diagnostic, sizeof(config->diagnostic), format,
                    arguments);
    va_end(arguments);
}

void gsh_config_defaults(gsh_shell_config *config)
{
    if (config == NULL) {
        return;
    }
    (void)memset(config, 0, sizeof(*config));
    config->async_repl_enabled = true;
    config->completion_enabled = true;
    config->history_enabled = true;
    config->history_max_entries = GSH_HISTORY_CAP;
    config->history_store_failed = true;
    config->history_ignore_space = true;
    config->terminal_actions = GSH_TERMINAL_ACTIONS_AUTO;
    config->terminal_images = GSH_TERMINAL_IMAGES_AUTO;
    config->path_detection = GSH_PATH_DETECTION_SAFE;
    config->preview_editor_auto = true;
}

static int write_all(int descriptor, const char *text, size_t length)
{
    if (text == NULL) {
        return -1;
    }
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(descriptor, text + offset, length - offset);

        if (written > 0) {
            offset += (size_t)written;
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static int create_initial(const char *path)
{
    if (path == NULL) {
        return -1;
    }
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                    O_NOFOLLOW,
                          S_IRUSR | S_IWUSR);
    int result = 0;

    if (descriptor == -1) {
        return errno == EEXIST ? 0 : -1;
    }
    if (write_all(descriptor, initial_config,
                  sizeof(initial_config) - 1U) == -1 ||
        fsync(descriptor) == -1) {
        result = -1;
    }
    if (close(descriptor) == -1) {
        result = -1;
    }
    return result;
}

static int secure_regular_file(int descriptor)
{
    struct stat status;

    if (fstat(descriptor, &status) == -1 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 0022) != 0 ||
        status.st_nlink != 1) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static ssize_t read_config(int descriptor, char *text, size_t capacity)
{
    if (text == NULL) {
        return -1;
    }
    size_t used = 0;

    while (used < capacity) {
        ssize_t count = read(descriptor, text + used, capacity - used);

        if (count > 0) {
            used += (size_t)count;
        } else if (count == 0) {
            return (ssize_t)used;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    errno = EFBIG;
    return -1;
}

static char *trim_left(char *text)
{
    if (text == NULL) {
        return NULL;
    }
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    return text;
}

static void trim_right(char *text)
{
    if (text == NULL) {
        return;
    }
    size_t length = strlen(text);

    while (length > 0 &&
           (text[length - 1U] == ' ' || text[length - 1U] == '\t' ||
            text[length - 1U] == '\r')) {
        text[--length] = '\0';
    }
}

static int parse_boolean(const char *text, bool *value)
{
    if (value == NULL) {
        return -1;
    }
    if (strcmp(text, "true") == 0) {
        *value = true;
        return 0;
    }
    if (strcmp(text, "false") == 0) {
        *value = false;
        return 0;
    }
    errno = EINVAL;
    return -1;
}

static int parse_count(const char *text, size_t *value)
{
    if (text == NULL) return -1;
    if (value == NULL) {
        return -1;
    }
    size_t parsed = 0;
    size_t index;

    if (text[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    for (index = 0; text[index] != '\0'; index++) {
        if (text[index] < '0' || text[index] > '9' ||
            parsed > (GSH_HISTORY_CAP - (size_t)(text[index] - '0')) / 10U) {
            errno = ERANGE;
            return -1;
        }
        parsed = parsed * 10U + (size_t)(text[index] - '0');
    }
    if (parsed == 0 || parsed > GSH_HISTORY_CAP) {
        errno = ERANGE;
        return -1;
    }
    *value = parsed;
    return 0;
}

/* ── Removed History Keys Remain Safe Upgrade Tombstones ──────
 * Plain-text history replaced the encrypted vault, so its unlock and reminder
 * settings no longer affect runtime behavior. Existing user files nevertheless
 * contain those keys and must remain usable after an in-place gsh upgrade.
 * The parser recognizes their former value grammar but discards the result.
 * All other unknown keys stay errors, preserving strict typo detection.
 * ─────────────────────────────────────────────────────────────── */
static int parse_removed_duration(const char *text)
{
    uint64_t number = 0U;
    uint64_t multiplier;
    size_t index = 0U;

    if (text == NULL) return -1;
    while (index < GSH_CONFIG_FILE_CAP && text[index] >= '0' &&
           text[index] <= '9') {
        if (number > (UINT64_MAX - (uint64_t)(text[index] - '0')) / 10U) {
            errno = ERANGE;
            return -1;
        }
        number = number * 10U + (uint64_t)(text[index++] - '0');
    }
    if (number == 0U) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(text + index, "s") == 0) multiplier = 1000000000ULL;
    else if (strcmp(text + index, "m") == 0)
        multiplier = 60ULL * 1000000000ULL;
    else if (strcmp(text + index, "h") == 0)
        multiplier = 60ULL * 60ULL * 1000000000ULL;
    else {
        errno = EINVAL;
        return -1;
    }
    if (number > UINT64_MAX / multiplier) {
        errno = ERANGE;
        return -1;
    }
    return 0;
}

static int field_index(const char *key)
{
    static const char *const keys[] = {
        "shell.history.enabled",       "shell.history.max_entries",
        "shell.history.deduplicate",   "shell.history.store_failed",
        "shell.history.ignore_space",  "shell.history.unlock_ttl",
        "shell.history.reminder_min",  "shell.history.reminder_max",
        "shell.async_repl.enabled",
        "terminal.actions",
        "terminal.actions.path_detection",
        "terminal.images",
        "shell.preview.editor",
        "shell.completion.enabled",
    };
    size_t index;

    for (index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
        if (strcmp(key, keys[index]) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int parse_actions_mode(const char *value,
                              gsh_terminal_actions_mode *mode)
{
    if (value == NULL || mode == NULL) return -1;
    if (strcmp(value, "auto") == 0) *mode = GSH_TERMINAL_ACTIONS_AUTO;
    else if (strcmp(value, "on") == 0) *mode = GSH_TERMINAL_ACTIONS_ON;
    else if (strcmp(value, "off") == 0) *mode = GSH_TERMINAL_ACTIONS_OFF;
    else { errno = EINVAL; return -1; }
    return 0;
}

static int parse_detection_mode(const char *value,
                                gsh_path_detection_mode *mode)
{
    if (value == NULL || mode == NULL) return -1;
    if (strcmp(value, "off") == 0) *mode = GSH_PATH_DETECTION_OFF;
    else if (strcmp(value, "known") == 0) *mode = GSH_PATH_DETECTION_KNOWN;
    else if (strcmp(value, "safe") == 0) *mode = GSH_PATH_DETECTION_SAFE;
    else { errno = EINVAL; return -1; }
    return 0;
}

static int parse_images_mode(const char *value,
                             gsh_terminal_images_mode *mode)
{
    if (value == NULL || mode == NULL) return -1;
    if (strcmp(value, "auto") == 0) *mode = GSH_TERMINAL_IMAGES_AUTO;
    else if (strcmp(value, "on") == 0) *mode = GSH_TERMINAL_IMAGES_ON;
    else if (strcmp(value, "off") == 0) *mode = GSH_TERMINAL_IMAGES_OFF;
    else { errno = EINVAL; return -1; }
    return 0;
}

static int parse_editor_string(const char **cursor, char *storage,
                               size_t capacity, size_t *used)
{
    const char *text;

    if (cursor == NULL || *cursor == NULL || storage == NULL || used == NULL ||
        **cursor != '"') return -1;
    text = ++*cursor;
    (void)text;
    while (**cursor != '\0' && **cursor != '"') {
        char byte = **cursor;
        if (byte == '\\') {
            (*cursor)++;
            byte = **cursor;
            if (byte != '\\' && byte != '"') return -1;
        }
        if (*used + 1U >= capacity) { errno = E2BIG; return -1; }
        storage[(*used)++] = byte;
        (*cursor)++;
    }
    if (**cursor != '"' || *used + 1U > capacity) return -1;
    storage[(*used)++] = '\0';
    (*cursor)++;
    return 0;
}

static void skip_editor_space(const char **cursor)
{
    if (cursor == NULL || *cursor == NULL) return;
    while (**cursor == ' ' || **cursor == '\t') (*cursor)++;
}

static int parse_editor_argv(const char *value, gsh_shell_config *config)
{
    const char *cursor = value;
    size_t used = 0U;
    size_t placeholders = 0U;

    if (value == NULL || config == NULL) return -1;
    if (strcmp(value, "auto") == 0) {
        config->preview_editor_auto = true;
        config->preview_editor_argc = 0U;
        return 0;
    }
    config->preview_editor_auto = false;
    config->preview_editor_argc = 0U;
    skip_editor_space(&cursor);
    if (*cursor++ != '[') return -1;
    for (size_t item = 0U; item <= GSH_CONFIG_EDITOR_ARG_CAP; item++) {
        size_t offset;
        skip_editor_space(&cursor);
        if (*cursor == ']') { cursor++; break; }
        if (config->preview_editor_argc >= GSH_CONFIG_EDITOR_ARG_CAP) {
            errno = E2BIG;
            return -1;
        }
        offset = used;
        if (parse_editor_string(&cursor, config->preview_editor_storage,
                                sizeof(config->preview_editor_storage),
                                &used) == -1) return -1;
        config->preview_editor_offsets[config->preview_editor_argc++] = offset;
        if (strcmp(config->preview_editor_storage + offset, "{file}") == 0) placeholders++;
        skip_editor_space(&cursor);
        if (*cursor == ',') { cursor++; continue; }
        if (*cursor == ']') { cursor++; break; }
        return -1;
    }
    skip_editor_space(&cursor);
    if (*cursor != '\0' || config->preview_editor_argc == 0U ||
        placeholders != 1U || used > GSH_CONFIG_EDITOR_STORAGE_CAP) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int apply_config_field(gsh_shell_config *config, int field,
                              const char *value)
{
    if (config == NULL || value == NULL) {
        return -1;
    }
    switch (field) {
    case 0:
        return parse_boolean(value, &config->history_enabled);
    case 1:
        return parse_count(value, &config->history_max_entries);
    case 2:
        return parse_boolean(value, &config->history_deduplicate);
    case 3:
        return parse_boolean(value, &config->history_store_failed);
    case 4:
        return parse_boolean(value, &config->history_ignore_space);
    case 5:
        if (strcmp(value, "infinite") == 0) return 0;
        errno = EINVAL;
        return -1;
    case 6:
    case 7:
        return parse_removed_duration(value);
    case 8:
        return parse_boolean(value, &config->async_repl_enabled);
    case 9:
        return parse_actions_mode(value, &config->terminal_actions);
    case 10:
        return parse_detection_mode(value, &config->path_detection);
    case 11:
        return parse_images_mode(value, &config->terminal_images);
    case 12:
        return parse_editor_argv(value, config);
    case 13:
        return parse_boolean(value, &config->completion_enabled);
    default:
        errno = EINVAL;
        return -1;
    }
}

const char *gsh_config_editor_argument(const gsh_shell_config *config,
                                       size_t index)
{
    size_t offset;

    if (config == NULL || index >= config->preview_editor_argc) return NULL;
    offset = config->preview_editor_offsets[index];
    return offset < sizeof(config->preview_editor_storage)
               ? config->preview_editor_storage + offset : NULL;
}

static int parse_assignment(config_parser *parser,
                            gsh_shell_config *config, char *line)
{
    if (config == NULL || line == NULL || parser == NULL) {
        return -1;
    }
    char *separator = strchr(line, '=');
    char *key;
    char *value;
    int field;

    if (separator == NULL) {
        errno = EINVAL;
        return -1;
    }
    *separator = '\0';
    key = trim_left(line);
    trim_right(key);
    value = trim_left(separator + 1);
    trim_right(value);
    if (strcmp(key, "config.version") == 0) {
        if (parser->version_seen || strcmp(value, "1") != 0) {
            errno = EINVAL;
            return -1;
        }
        parser->version_seen = true;
        return 0;
    }
    field = field_index(key);
    if (field < 0 || parser->seen[field]) {
        errno = EINVAL;
        return -1;
    }
    parser->seen[field] = true;
    return apply_config_field(config, field, value);
}

static int parse_text(gsh_shell_config *candidate, char *text, size_t length,
                      size_t *failed_line)
{
    if (failed_line == NULL || text == NULL) {
        return -1;
    }
    config_parser parser = {.text = text, .length = length, .line = 1};
    size_t begin = 0;

    while (begin <= length) {
        size_t end = begin;
        char *line;

        while (end < length && text[end] != '\n') {
            end++;
        }
        text[end] = '\0';
        line = trim_left(text + begin);
        if (*line != '\0' && *line != '#' &&
            parse_assignment(&parser, candidate, line) == -1) {
            *failed_line = parser.line;
            return -1;
        }
        if (end == length) {
            break;
        }
        begin = end + 1U;
        parser.line++;
    }
    if (!parser.version_seen) {
        *failed_line = parser.line;
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int gsh_config_load(gsh_shell_config *config, const char *home,
                    bool create_missing)
{
    char text[GSH_CONFIG_FILE_CAP + 1U];
    gsh_shell_config candidate;
    ssize_t length;
    size_t failed_line = 0;
    int descriptor;

    if (config == NULL || home == NULL || home[0] != '/' ||
        snprintf(config->path, sizeof(config->path), "%s/.gshrc", home) >=
            (int)sizeof(config->path)) {
        errno = EINVAL;
        return -1;
    }
    if (create_missing && access(config->path, F_OK) == -1 &&
        errno == ENOENT && create_initial(config->path) == -1) {
        set_diagnostic(config, "cannot create %s: %s", config->path,
                       strerror(errno));
        return -1;
    }
    descriptor = open(config->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor == -1 || secure_regular_file(descriptor) == -1) {
        set_diagnostic(config, "cannot securely read %s: %s", config->path,
                       strerror(errno));
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return -1;
    }
    length = read_config(descriptor, text, GSH_CONFIG_FILE_CAP);
    (void)close(descriptor);
    if (length < 0) {
        set_diagnostic(config, "cannot read %s: %s", config->path,
                       strerror(errno));
        return -1;
    }
    text[(size_t)length] = '\0';
    gsh_config_defaults(&candidate);
    (void)memcpy(candidate.path, config->path, strlen(config->path) + 1U);
    if (parse_text(&candidate, text, (size_t)length, &failed_line) == -1) {
        set_diagnostic(config, "%s:%zu: invalid configuration", config->path,
                       failed_line);
        return -1;
    }
    *config = candidate;
    return 0;
}
