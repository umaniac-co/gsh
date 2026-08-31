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
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum {
    GSH_CONFIG_FILE_CAP = 65536,
    GSH_CONFIG_VERSION = 1,
};

static const uint64_t GSH_HISTORY_REMINDER_MIN_NS =
    4ULL * 60ULL * 60ULL * 1000000000ULL;
static const uint64_t GSH_HISTORY_REMINDER_MAX_NS =
    6ULL * 60ULL * 60ULL * 1000000000ULL;
static const char initial_config[] =
    "config.version = 1\n"
    "\n"
    "shell.async_repl.enabled = true\n"
    "shell.history.enabled = true\n"
    "shell.history.max_entries = 1024\n"
    "shell.history.deduplicate = false\n"
    "shell.history.store_failed = true\n"
    "shell.history.ignore_space = true\n"
    "shell.history.unlock_ttl = infinite\n"
    "shell.history.reminder_min = 4h\n"
    "shell.history.reminder_max = 6h\n";

typedef struct {
    char *text;
    size_t length;
    size_t line;
    bool version_seen;
    bool seen[9];
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
    memset(config, 0, sizeof(*config));
    config->async_repl_enabled = true;
    config->history_enabled = true;
    config->history_max_entries = GSH_HISTORY_CAP;
    config->history_store_failed = true;
    config->history_ignore_space = true;
    config->history_unlock_infinite = true;
    config->history_reminder_min_ns = GSH_HISTORY_REMINDER_MIN_NS;
    config->history_reminder_max_ns = GSH_HISTORY_REMINDER_MAX_NS;
}

static int write_all(int descriptor, const char *text, size_t length)
{
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
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    return text;
}

static void trim_right(char *text)
{
    size_t length = strlen(text);

    while (length > 0 &&
           (text[length - 1U] == ' ' || text[length - 1U] == '\t' ||
            text[length - 1U] == '\r')) {
        text[--length] = '\0';
    }
}

static int parse_boolean(const char *text, bool *value)
{
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

static int parse_duration(const char *text, uint64_t *value)
{
    uint64_t number = 0;
    uint64_t multiplier;
    size_t index = 0;

    while (text[index] >= '0' && text[index] <= '9') {
        if (number > (UINT64_MAX - (uint64_t)(text[index] - '0')) / 10U) {
            errno = ERANGE;
            return -1;
        }
        number = number * 10U + (uint64_t)(text[index++] - '0');
    }
    if (number == 0) {
        errno = EINVAL;
        return -1;
    }
    if (strcmp(text + index, "s") == 0) {
        multiplier = 1000000000ULL;
    } else if (strcmp(text + index, "m") == 0) {
        multiplier = 60ULL * 1000000000ULL;
    } else if (strcmp(text + index, "h") == 0) {
        multiplier = 60ULL * 60ULL * 1000000000ULL;
    } else {
        errno = EINVAL;
        return -1;
    }
    if (number > UINT64_MAX / multiplier) {
        errno = ERANGE;
        return -1;
    }
    *value = number * multiplier;
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
    };
    size_t index;

    for (index = 0; index < sizeof(keys) / sizeof(keys[0]); index++) {
        if (strcmp(key, keys[index]) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int apply_history_field(gsh_shell_config *config, int field,
                               const char *value)
{
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
        config->history_unlock_infinite = strcmp(value, "infinite") == 0;
        return config->history_unlock_infinite ? 0 : -1;
    case 6:
        return parse_duration(value, &config->history_reminder_min_ns);
    case 7:
        return parse_duration(value, &config->history_reminder_max_ns);
    case 8:
        return parse_boolean(value, &config->async_repl_enabled);
    default:
        errno = EINVAL;
        return -1;
    }
}

static int parse_assignment(config_parser *parser,
                            gsh_shell_config *config, char *line)
{
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
    return apply_history_field(config, field, value);
}

static int parse_text(gsh_shell_config *candidate, char *text, size_t length,
                      size_t *failed_line)
{
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
    if (!parser.version_seen ||
        candidate->history_reminder_min_ns >
            candidate->history_reminder_max_ns) {
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
    memcpy(candidate.path, config->path, strlen(config->path) + 1U);
    if (parse_text(&candidate, text, (size_t)length, &failed_line) == -1) {
        set_diagnostic(config, "%s:%zu: invalid configuration", config->path,
                       failed_line);
        return -1;
    }
    *config = candidate;
    return 0;
}
