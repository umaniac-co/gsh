#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "native_viewer.h"
#include "resource_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h> /* CANON-INCLUDE: macos */
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

enum {
    VIEW_READ_CAP = 16384,
    VIEW_LINE_CAP = 8192,
    VIEW_RENDER_CAP = 131072,
    VIEW_SEARCH_CAP = 128,
    VIEW_LINE_INDEX_CAP = 1048576,
    VIEW_KEY_CAP = 16,
};

typedef enum {
    VIEW_PLAIN,
    VIEW_PYTHON,
    VIEW_SHELL,
    VIEW_C,
    VIEW_JAVASCRIPT,
    VIEW_RUST,
    VIEW_GO,
    VIEW_JSON,
    VIEW_DATA,
    VIEW_MARKDOWN,
} view_language;

typedef struct {
    int file;
    int index;
    off_t size;
    size_t lines;
    size_t top;
    size_t selected;
    size_t horizontal;
    size_t preferred_column;
    bool binary;
    bool index_truncated;
    view_language language;
    struct stat status;
    char search[VIEW_SEARCH_CAP];
    size_t search_length;
    struct termios saved_modes;
    bool modes_saved;
    int resource_descriptor;
} viewer_state;

typedef struct {
    size_t rows;
    size_t columns;
    size_t separator_column;
    size_t content_column;
    size_t content_columns;
    bool split;
} viewer_layout;

static size_t viewer_split_column(size_t columns)
{
    size_t repl_columns;
    if (columns < GSH_VIEWER_SPLIT_MIN_COLUMNS) return 0U;
    repl_columns = columns * 45U / 100U;
    if (repl_columns < GSH_VIEWER_REPL_MIN_COLUMNS)
        repl_columns = GSH_VIEWER_REPL_MIN_COLUMNS;
    if (columns - repl_columns < GSH_VIEWER_REPL_MIN_COLUMNS + 2U)
        return 0U;
    return repl_columns + 1U;
}

static bool send_view_layout(const viewer_state *state,
                             size_t separator_column)
{
    gsh_resource_view_record record;
    ssize_t written;
    if (state == NULL || state->resource_descriptor < 0) return false;
    record.version = GSH_RESOURCE_PROTOCOL_VERSION;
    record.size = (uint32_t)sizeof(record);
    record.event = separator_column == 0U
                       ? GSH_RESOURCE_PROTOCOL_VIEW_FULL
                       : GSH_RESOURCE_PROTOCOL_VIEW_SPLIT;
    record.separator_column = (uint32_t)separator_column;
    written = write(state->resource_descriptor, &record, sizeof(record));
    return written == (ssize_t)sizeof(record);
}

static int viewer_write(const char *text, size_t length)
{
    size_t offset = 0U;
    if (text == NULL) return -1;
    while (offset < length) {
        ssize_t count = write(STDOUT_FILENO, text + offset, length - offset);
        if (count > 0) offset += (size_t)count;
        else if (count == -1 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static int index_write(int descriptor, size_t index, off_t value)
{
    const char *bytes = (const char *)&value;
    size_t used = 0U;
    off_t position = (off_t)(index * sizeof(value));
    if (descriptor < 0) return -1;
    while (used < sizeof(value)) {
        ssize_t count = pwrite(descriptor, bytes + used,
                               sizeof(value) - used,
                               position + (off_t)used);
        if (count > 0) used += (size_t)count;
        else if (count == -1 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static int index_read(int descriptor, size_t index, off_t *value)
{
    char *bytes = (char *)value;
    size_t used = 0U;
    off_t position = (off_t)(index * sizeof(*value));
    if (descriptor < 0 || value == NULL) return -1;
    while (used < sizeof(*value)) {
        ssize_t count = pread(descriptor, bytes + used,
                              sizeof(*value) - used,
                              position + (off_t)used);
        if (count > 0) used += (size_t)count;
        else if (count == -1 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static int viewer_tempfile(void)
{
    char pattern[] = "/tmp/gsh-view-XXXXXX";
    int descriptor = mkstemp(pattern);
    if (descriptor >= 0) {
        (void)unlink(pattern);
        (void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    }
    return descriptor;
}

static view_language suffix_language(const char *path)
{
    const char *name;
    const char *suffix;
    if (path == NULL) return VIEW_PLAIN;
    name = strrchr(path, '/');
    name = name == NULL ? path : name + 1U;
    suffix = strrchr(name, '.');
    if (strcmp(name, "Makefile") == 0 || strcmp(name, "Dockerfile") == 0 ||
        strcmp(name, ".profile") == 0 || strcmp(name, ".bashrc") == 0 ||
        strcmp(name, ".zshrc") == 0) return VIEW_SHELL;
    if (suffix == NULL) return VIEW_PLAIN;
    if (strcmp(suffix, ".py") == 0 || strcmp(suffix, ".pyw") == 0) return VIEW_PYTHON;
    if (strcmp(suffix, ".sh") == 0 || strcmp(suffix, ".bash") == 0 ||
        strcmp(suffix, ".zsh") == 0) return VIEW_SHELL;
    if (strcmp(suffix, ".c") == 0 || strcmp(suffix, ".h") == 0 ||
        strcmp(suffix, ".cc") == 0 || strcmp(suffix, ".cpp") == 0 ||
        strcmp(suffix, ".hpp") == 0) return VIEW_C;
    if (strcmp(suffix, ".js") == 0 || strcmp(suffix, ".jsx") == 0 ||
        strcmp(suffix, ".ts") == 0 || strcmp(suffix, ".tsx") == 0) return VIEW_JAVASCRIPT;
    if (strcmp(suffix, ".rs") == 0) return VIEW_RUST;
    if (strcmp(suffix, ".go") == 0) return VIEW_GO;
    if (strcmp(suffix, ".json") == 0) return VIEW_JSON;
    if (strcmp(suffix, ".toml") == 0 || strcmp(suffix, ".yaml") == 0 ||
        strcmp(suffix, ".yml") == 0) return VIEW_DATA;
    if (strcmp(suffix, ".md") == 0 || strcmp(suffix, ".markdown") == 0) return VIEW_MARKDOWN;
    return VIEW_PLAIN;
}

static view_language shebang_language(const char *bytes, size_t length,
                                      view_language fallback)
{
    if (bytes == NULL || length < 2U || bytes[0] != '#' || bytes[1] != '!') return fallback;
    if (memmem(bytes, length < 256U ? length : 256U, "python", 6U) != NULL) return VIEW_PYTHON;
    if (memmem(bytes, length < 256U ? length : 256U, "sh", 2U) != NULL) return VIEW_SHELL;
    return fallback;
}

static int build_line_index(viewer_state *state, const char *path)
{
    char bytes[VIEW_READ_CAP];
    off_t offset = 0;
    size_t turns;

    if (state == NULL || path == NULL) return -1;
    state->file = open(path, O_RDONLY | O_CLOEXEC);
    state->index = viewer_tempfile();
    if (state->file < 0 || state->index < 0 ||
        fstat(state->file, &state->status) == -1 ||
        index_write(state->index, 0U, 0) == -1) return -1;
    state->lines = 1U;
    state->binary = false;
    state->index_truncated = false;
    for (turns = 0U; turns < VIEW_LINE_INDEX_CAP; turns++) {
        ssize_t count = pread(state->file, bytes, sizeof(bytes), offset);
        size_t index;
        if (count == 0) break;
        if (count == -1 && errno == EINTR) { turns--; continue; }
        if (count < 0) return -1;
        if (offset == 0) state->language = shebang_language(bytes, (size_t)count, state->language);
        for (index = 0U; index < (size_t)count; index++) {
            if (bytes[index] == '\0') state->binary = true;
            if (bytes[index] == '\n') {
                if (state->lines >= VIEW_LINE_INDEX_CAP) {
                    state->index_truncated = true;
                    break;
                }
                if (index_write(state->index, state->lines,
                                offset + (off_t)index + 1) == -1) return -1;
                state->lines++;
            }
        }
        offset += count;
        if (state->index_truncated) break;
    }
    state->size = lseek(state->file, 0, SEEK_END);
    if (state->size >= 0 && state->binary) {
        state->lines = (size_t)((state->size + 15) / 16);
        if (state->lines == 0U) state->lines = 1U;
    }
    return state->size < 0 ? -1 : 0;
}

static char viewer_mode_type(mode_t mode)
{
    if (S_ISDIR(mode)) return 'd';
    if (S_ISLNK(mode)) return 'l';
    if (S_ISCHR(mode)) return 'c';
    if (S_ISBLK(mode)) return 'b';
    if (S_ISFIFO(mode)) return 'p';
    if (S_ISSOCK(mode)) return 's';
    return '-';
}

static void viewer_mode_text(mode_t mode, char text[11])
{
    static const mode_t masks[9] = {
        S_IRUSR, S_IWUSR, S_IXUSR, S_IRGRP, S_IWGRP,
        S_IXGRP, S_IROTH, S_IWOTH, S_IXOTH};
    static const char letters[3] = {'r', 'w', 'x'};
    size_t index;
    if (text == NULL) return;
    text[0] = viewer_mode_type(mode);
    for (index = 0U; index < 9U; index++)
        text[index + 1U] = (mode & masks[index]) != 0
                               ? letters[index % 3U] : '-';
    if ((mode & S_ISUID) != 0) text[3] = (mode & S_IXUSR) != 0 ? 's' : 'S';
    if ((mode & S_ISGID) != 0) text[6] = (mode & S_IXGRP) != 0 ? 's' : 'S';
    if ((mode & S_ISVTX) != 0) text[9] = (mode & S_IXOTH) != 0 ? 't' : 'T';
    text[10] = '\0';
}

static void viewer_size_text(off_t size, char text[16])
{
    static const char units[] = "BKMGTPE";
    double value = size < 0 ? 0.0 : (double)size;
    size_t unit = 0U;
    if (text == NULL) return;
    while (value >= 1024.0 && unit + 1U < sizeof(units) - 1U) {
        value /= 1024.0;
        unit++;
    }
    if (unit == 0U) (void)snprintf(text, 16U, "%.0fB", value);
    else if (value < 10.0)
        (void)snprintf(text, 16U, "%.1f%c", value, units[unit]);
    else (void)snprintf(text, 16U, "%.0f%c", value, units[unit]);
}

static int viewer_metadata(const viewer_state *state, char *output,
                           size_t capacity)
{
    char mode[11];
    char size[16];
    char modified[32];
    char owner[32];
    char group[32];
    struct passwd *owner_entry;
    struct group *group_entry;
    struct tm local;
    const char *owner_value;
    const char *group_value;
    if (state == NULL || output == NULL || capacity == 0U) return -1;
    viewer_mode_text(state->status.st_mode, mode);
    viewer_size_text(state->status.st_size, size);
    owner_entry = getpwuid(state->status.st_uid);
    group_entry = getgrgid(state->status.st_gid);
    (void)snprintf(owner, sizeof(owner), "%ju",
                   (uintmax_t)state->status.st_uid);
    (void)snprintf(group, sizeof(group), "%ju",
                   (uintmax_t)state->status.st_gid);
    owner_value = owner_entry == NULL ? owner : owner_entry->pw_name;
    group_value = group_entry == NULL ? group : group_entry->gr_name;
    if (localtime_r(&state->status.st_mtime, &local) == NULL ||
        strftime(modified, sizeof(modified), "%Y-%m-%d %H:%M", &local) == 0U)
        (void)snprintf(modified, sizeof(modified), "%jd",
                       (intmax_t)state->status.st_mtime);
    return snprintf(output, capacity, "  %s %s:%s %s  %s",
                    mode, owner_value, group_value, size, modified);
}

static int read_line(const viewer_state *state, size_t line,
                     char output[VIEW_LINE_CAP], size_t *length)
{
    off_t begin;
    off_t end;
    size_t wanted;
    ssize_t count;

    if (state == NULL || output == NULL || length == NULL || line >= state->lines) return -1;
    if (index_read(state->index, line, &begin) == -1) return -1;
    if (line + 1U < state->lines) {
        if (index_read(state->index, line + 1U, &end) == -1) return -1;
    } else end = state->size;
    if (end < begin) return -1;
    wanted = (size_t)(end - begin);
    if (wanted >= VIEW_LINE_CAP) wanted = VIEW_LINE_CAP - 1U;
    count = pread(state->file, output, wanted, begin);
    if (count < 0) return -1;
    *length = (size_t)count;
    while (*length > 0U && (output[*length - 1U] == '\n' || output[*length - 1U] == '\r')) (*length)--;
    output[*length] = '\0';
    return 0;
}

static bool identifier_byte(unsigned char byte)
{
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_';
}

static bool keyword_list_match(const char *word, size_t length,
                               const char *const *keywords, size_t count)
{
    size_t index;
    if (word == NULL || keywords == NULL) return false;
    for (index = 0U; index < count; index++) {
        if (strlen(keywords[index]) == length &&
            memcmp(word, keywords[index], length) == 0) return true;
    }
    return false;
}

static bool keyword_match(view_language language, const char *word,
                          size_t length)
{
    static const char *const python[] = {
        "False", "None", "True", "and", "as", "assert", "async", "await",
        "break", "class", "continue", "def", "del", "elif", "else",
        "except", "finally", "for", "from", "global", "if", "import",
        "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise",
        "return", "try", "while", "with", "yield"};
    static const char *const shell[] = {
        "break", "case", "continue", "do", "done", "elif", "else", "esac",
        "export", "fi", "for", "function", "if", "in", "local", "readonly",
        "return", "select", "then", "until", "while"};
    static const char *const c_family[] = {
        "auto", "bool", "break", "case", "char", "class", "const",
        "continue", "default", "do", "double", "else", "enum", "extern",
        "false", "float", "for", "if", "inline", "int", "long", "namespace",
        "return", "short", "signed", "sizeof", "static", "struct", "switch",
        "template", "true", "typedef", "union", "unsigned", "void", "volatile",
        "while"};
    static const char *const modern[] = {
        "async", "await", "break", "case", "const", "continue", "default",
        "defer", "else", "enum", "false", "fn", "for", "func", "function",
        "go", "if", "impl", "import", "in", "interface", "let", "match",
        "mod", "null", "package", "return", "struct", "switch", "trait",
        "true", "type", "use", "var", "while"};
    const char *const *keywords = NULL;
    size_t count = 0U;
    if (language == VIEW_PYTHON) {
        keywords = python;
        count = sizeof(python) / sizeof(python[0]);
    } else if (language == VIEW_SHELL) {
        keywords = shell;
        count = sizeof(shell) / sizeof(shell[0]);
    } else if (language == VIEW_C) {
        keywords = c_family;
        count = sizeof(c_family) / sizeof(c_family[0]);
    } else if (language == VIEW_JAVASCRIPT || language == VIEW_RUST ||
               language == VIEW_GO || language == VIEW_JSON ||
               language == VIEW_DATA) {
        keywords = modern;
        count = sizeof(modern) / sizeof(modern[0]);
    }
    return keywords != NULL &&
           keyword_list_match(word, length, keywords, count);
}

static bool comment_start(view_language language, const char *text,
                          size_t length, size_t offset)
{
    if (text == NULL || offset >= length) return false;
    if ((language == VIEW_PYTHON || language == VIEW_SHELL ||
         language == VIEW_DATA) && text[offset] == '#') return true;
    return (language == VIEW_C || language == VIEW_JAVASCRIPT ||
            language == VIEW_RUST || language == VIEW_GO) &&
           text[offset] == '/' && offset + 1U < length && text[offset + 1U] == '/';
}

static int render_append(char output[VIEW_RENDER_CAP], size_t *used,
                         const char *text, size_t length)
{
    if (output == NULL || used == NULL || text == NULL ||
        length > VIEW_RENDER_CAP - *used) return -1;
    (void)memcpy(output + *used, text, length);
    *used += length;
    return 0;
}

static size_t safe_utf8_sequence(const unsigned char *text, size_t length)
{
    if (text == NULL || length == 0U) return 0U;
    if (text[0] >= 0x20U && text[0] < 0x7fU) return 1U;
    if (text[0] >= 0xc2U && text[0] <= 0xdfU && length >= 2U &&
        text[1] >= 0x80U && text[1] <= 0xbfU) return 2U;
    if (length >= 3U && text[2] >= 0x80U && text[2] <= 0xbfU &&
        ((text[0] == 0xe0U && text[1] >= 0xa0U && text[1] <= 0xbfU) ||
         (text[0] >= 0xe1U && text[0] <= 0xecU &&
          text[1] >= 0x80U && text[1] <= 0xbfU) ||
         (text[0] == 0xedU && text[1] >= 0x80U && text[1] <= 0x9fU) ||
         (text[0] >= 0xeeU && text[0] <= 0xefU &&
          text[1] >= 0x80U && text[1] <= 0xbfU))) return 3U;
    if (length >= 4U && text[2] >= 0x80U && text[2] <= 0xbfU &&
        text[3] >= 0x80U && text[3] <= 0xbfU &&
        ((text[0] == 0xf0U && text[1] >= 0x90U && text[1] <= 0xbfU) ||
         (text[0] >= 0xf1U && text[0] <= 0xf3U &&
          text[1] >= 0x80U && text[1] <= 0xbfU) ||
         (text[0] == 0xf4U && text[1] >= 0x80U &&
          text[1] <= 0x8fU))) return 4U;
    return 0U;
}

static int render_safe_text(char output[VIEW_RENDER_CAP], size_t *used,
                            const char *text, size_t length)
{
    size_t offset = 0U;
    if (output == NULL || used == NULL || text == NULL) return -1;
    while (offset < length) {
        size_t sequence;
        if ((unsigned char)text[offset] == '\t') {
            if (render_append(output, used, "    ", 4U) == -1) return -1;
            offset++;
            continue;
        }
        sequence = safe_utf8_sequence(
            (const unsigned char *)text + offset, length - offset);
        if (sequence == 0U) {
            if (render_append(output, used, "?", 1U) == -1) return -1;
            offset++;
        } else {
            if (render_append(output, used, text + offset, sequence) == -1)
                return -1;
            offset += sequence;
        }
    }
    return 0;
}

static int highlight_line(view_language language, const char *text,
                          size_t length, size_t horizontal, size_t width,
                          char output[VIEW_RENDER_CAP], size_t *used)
{
    size_t offset = horizontal < length ? horizontal : length;
    size_t columns = 0U;

    if (text == NULL || output == NULL || used == NULL) return -1;
    while (offset < length && columns < width) {
        size_t end;
        unsigned char byte = (unsigned char)text[offset];
        if (comment_start(language, text, length, offset)) {
            end = length;
            if (end - offset > width - columns) end = offset + width - columns;
            if (render_append(output, used, "\033[38;5;244m", 11U) == -1 ||
                render_safe_text(output, used, text + offset,
                                 end - offset) == -1 ||
                render_append(output, used, "\033[0m", 4U) == -1) return -1;
            return 0;
        }
        if (byte == '"' || byte == '\'') {
            char quote = (char)byte;
            end = offset + 1U;
            while (end < length && text[end] != quote && end - offset < width - columns) {
                if (text[end] == '\\' && end + 1U < length) end++;
                end++;
            }
            if (end < length && text[end] == quote) end++;
            if (render_append(output, used, "\033[38;5;114m", 11U) == -1 ||
                render_safe_text(output, used, text + offset,
                                 end - offset) == -1 ||
                render_append(output, used, "\033[0m", 4U) == -1) return -1;
            columns += end - offset;
            offset = end;
            continue;
        }
        if (identifier_byte(byte) && !(byte >= '0' && byte <= '9')) {
            end = offset + 1U;
            while (end < length && identifier_byte((unsigned char)text[end])) end++;
            if (end - offset > width - columns) end = offset + width - columns;
            if (keyword_match(language, text + offset, end - offset) &&
                render_append(output, used, "\033[38;5;75m", 10U) == -1) return -1;
            if (render_safe_text(output, used, text + offset,
                                 end - offset) == -1) return -1;
            if (keyword_match(language, text + offset, end - offset) &&
                render_append(output, used, "\033[0m", 4U) == -1) return -1;
            columns += end - offset;
            offset = end;
            continue;
        }
        if (byte >= 0x80U) {
            size_t sequence = safe_utf8_sequence(
                (const unsigned char *)text + offset, length - offset);
            if (sequence > 1U) {
                if (render_safe_text(output, used, text + offset,
                                     sequence) == -1) return -1;
                offset += sequence;
                columns++;
                continue;
            }
        }
        if ((byte >= '0' && byte <= '9') &&
            render_append(output, used, "\033[38;5;215m", 11U) == -1) return -1;
        if (render_safe_text(output, used, text + offset, 1U) == -1) return -1;
        if (byte >= '0' && byte <= '9' && render_append(output, used, "\033[0m", 4U) == -1) return -1;
        offset++;
        columns++;
    }
    return 0;
}

static void viewer_dimensions(size_t *rows, size_t *columns)
{
    struct winsize size = {0};
    if (rows == NULL || columns == NULL) return;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) {
        *rows = size.ws_row == 0U ? 24U : size.ws_row;
        *columns = size.ws_col == 0U ? 80U : size.ws_col;
    } else { *rows = 24U; *columns = 80U; }
    if (*rows > 256U) *rows = 256U;
    if (*columns > 512U) *columns = 512U;
}

static void prepare_viewer_layout(viewer_state *state, viewer_layout *layout)
{
    size_t separator;
    if (state == NULL || layout == NULL) return;
    (void)memset(layout, 0, sizeof(*layout));
    viewer_dimensions(&layout->rows, &layout->columns);
    separator = viewer_split_column(layout->columns);
    if (separator != 0U && !send_view_layout(state, separator)) separator = 0U;
    if (separator == 0U) (void)send_view_layout(state, 0U);
    layout->split = separator != 0U;
    layout->separator_column = separator;
    layout->content_column = layout->split ? separator + 2U : 1U;
    layout->content_columns = layout->split
                                  ? layout->columns - separator - 1U
                                  : layout->columns;
}

static int begin_viewer_row(const viewer_layout *layout, size_t row,
                            char output[VIEW_RENDER_CAP], size_t *used)
{
    static const char separator[] = "\033[38;5;240m│\033[0m ";
    char cursor[64];
    int length;
    if (layout == NULL || output == NULL || used == NULL) return -1;
    if (!layout->split) return 0;
    length = snprintf(cursor, sizeof(cursor), "\033[%zu;%zuH", row + 1U,
                      layout->separator_column);
    if (length < 0 || (size_t)length >= sizeof(cursor) ||
        render_append(output, used, cursor, (size_t)length) == -1)
        return -1;
    return render_append(output, used, separator, sizeof(separator) - 1U);
}

static int end_viewer_row(const viewer_layout *layout, bool final,
                          char output[VIEW_RENDER_CAP], size_t *used)
{
    if (layout == NULL || output == NULL || used == NULL) return -1;
    if (render_append(output, used, "\033[K", 3U) == -1) return -1;
    return !layout->split && !final
               ? render_append(output, used, "\r\n", 2U) : 0;
}

static size_t visible_text_prefix(const char *text, size_t length,
                                  size_t columns)
{
    size_t offset = 0U;
    size_t width = 0U;
    if (text == NULL) return 0U;
    while (offset < length && width < columns) {
        size_t sequence = safe_utf8_sequence(
            (const unsigned char *)text + offset, length - offset);
        offset += sequence == 0U ? 1U : sequence;
        width++;
    }
    return offset;
}

static int render_status_row(const viewer_layout *layout, size_t row,
                             const char *text, bool inverse, bool final,
                             char output[VIEW_RENDER_CAP], size_t *used)
{
    size_t length;
    if (layout == NULL || text == NULL || output == NULL || used == NULL)
        return -1;
    length = visible_text_prefix(text, strlen(text), layout->content_columns);
    if (begin_viewer_row(layout, row, output, used) == -1 ||
        (inverse && render_append(output, used, "\033[7m", 4U) == -1) ||
        render_safe_text(output, used, text, length) == -1 ||
        (inverse && render_append(output, used, "\033[0m", 4U) == -1) ||
        end_viewer_row(layout, final, output, used) == -1) return -1;
    return 0;
}

static int render_text_page(const viewer_state *state, const char *path,
                            const viewer_layout *layout,
                            char output[VIEW_RENDER_CAP], size_t *used)
{
    size_t row;
    size_t content_rows;
    char line[VIEW_LINE_CAP];

    if (state == NULL || path == NULL || layout == NULL || output == NULL ||
        used == NULL) return -1;
    content_rows = layout->rows > 3U ? layout->rows - 3U : 1U;
    for (row = 0U; row < content_rows; row++) {
        size_t line_number = state->top + row;
        size_t length = 0U;
        char prefix[32];
        int prefix_length;
        if (line_number >= state->lines) {
            if (begin_viewer_row(layout, row, output, used) == -1 ||
                render_append(output, used, "~", 1U) == -1 ||
                end_viewer_row(layout, false, output, used) == -1) return -1;
            continue;
        }
        if (read_line(state, line_number, line, &length) == -1) return -1;
        prefix_length = snprintf(prefix, sizeof(prefix), "%c%6zu ",
                                 line_number == state->selected ? '>' : ' ',
                                 line_number + 1U);
        if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix) ||
            begin_viewer_row(layout, row, output, used) == -1 ||
            render_append(output, used, prefix, (size_t)prefix_length) == -1 ||
            highlight_line(state->language, line, length, state->horizontal,
                           layout->content_columns > (size_t)prefix_length
                               ? layout->content_columns - (size_t)prefix_length
                               : 1U,
                           output, used) == -1 ||
            end_viewer_row(layout, false, output, used) == -1) return -1;
    }
    return 0;
}

static int render_binary_page(const viewer_state *state,
                              const viewer_layout *layout,
                              char output[VIEW_RENDER_CAP], size_t *used)
{
    if (state == NULL || layout == NULL || output == NULL || used == NULL)
        return -1;
    size_t row;
    size_t content_rows = layout->rows > 3U ? layout->rows - 3U : 1U;
    for (row = 0U; row < content_rows; row++) {
        unsigned char bytes[16];
        off_t offset = (off_t)((state->top + row) * 16U);
        ssize_t count = offset < state->size ? pread(state->file, bytes, sizeof(bytes), offset) : 0;
        char line[128];
        size_t used_line = 0U;
        size_t index;
        int length = snprintf(line, sizeof(line), "%08jx  ", (uintmax_t)offset);
        if (length < 0) return -1;
        used_line = (size_t)length;
        for (index = 0U; index < 16U && used_line + 4U < sizeof(line); index++) {
            length = index < (size_t)(count < 0 ? 0 : count)
                         ? snprintf(line + used_line, sizeof(line) - used_line, "%02x ", bytes[index])
                         : snprintf(line + used_line, sizeof(line) - used_line, "   ");
            if (length < 0) return -1;
            used_line += (size_t)length;
        }
        if (begin_viewer_row(layout, row, output, used) == -1 ||
            render_append(output, used, line, used_line) == -1 ||
            end_viewer_row(layout, false, output, used) == -1) return -1;
    }
    return 0;
}

static int render_viewer(viewer_state *state, const char *path)
{
    char output[VIEW_RENDER_CAP];
    char metadata[512];
    char status[2048];
    viewer_layout layout;
    size_t used = 0U;
    size_t content_rows;
    const char *display_path;
    int status_length;

    if (state == NULL || path == NULL) return -1;
    prepare_viewer_layout(state, &layout);
    content_rows = layout.rows > 3U ? layout.rows - 3U : 1U;
    if (!layout.split &&
        render_append(output, &used, "\033[H", 3U) == -1) return -1;
    if (state->binary) {
        if (render_binary_page(state, &layout, output, &used) == -1) return -1;
    } else if (render_text_page(state, path, &layout, output, &used) == -1)
        return -1;
    status_length = viewer_metadata(state, metadata, sizeof(metadata));
    if (status_length < 0 || (size_t)status_length >= sizeof(metadata))
        return -1;
    display_path = strrchr(path, '/');
    display_path = display_path == NULL || display_path[1] == '\0'
                       ? path : display_path + 1U;
    status_length = snprintf(status, sizeof(status),
        " %s%s  %s%zu/%zu%s col %zu",
        layout.split || layout.content_columns < 120U ? display_path : path,
        metadata,
        state->binary ? "hex " : "", state->selected + 1U, state->lines,
        state->index_truncated ? "+" : "",
        state->horizontal + state->preferred_column);
    if (status_length < 0 || (size_t)status_length >= sizeof(status) ||
        (!layout.split &&
         render_status_row(&layout, content_rows, status, true, false,
                           output, &used) == -1)) return -1;
    if (layout.split) {
        status_length = snprintf(status, sizeof(status),
            " %s  %s%zu/%zu%s col %zu", display_path,
            state->binary ? "hex " : "", state->selected + 1U,
            state->lines, state->index_truncated ? "+" : "",
            state->horizontal + state->preferred_column);
        if (status_length < 0 || (size_t)status_length >= sizeof(status) ||
            render_status_row(&layout, content_rows, status, true, false,
                              output, &used) == -1 ||
            render_status_row(&layout, content_rows + 1U, metadata + 2U,
                              false, false, output, &used) == -1 ||
            render_status_row(
                &layout, content_rows + 2U,
                "Esc: panel · q: close · ↑↓/Pg: scroll · /: find · n/N · r · e",
                false, true, output, &used) == -1) return -1;
    } else if (layout.content_columns >= 70U) {
        if (render_status_row(
                &layout, content_rows + 1U,
                "Esc: switch panel · q: close · ↑↓/PgUp/PgDn: scroll",
                false, false, output, &used) == -1 ||
            render_status_row(
                &layout, content_rows + 2U,
                "←→: horizontal · /: search · n/N: results · r: reload · e: edit",
                false, true, output, &used) == -1) return -1;
    } else if (render_status_row(
                   &layout, content_rows + 1U,
                   "Esc: panel · q: close · ↑↓/Pg: scroll", false, false,
                   output, &used) == -1 ||
               render_status_row(
                   &layout, content_rows + 2U,
                   "←→: horizontal · /: search · n/N · r · e: edit", false,
                   true, output, &used) == -1) return -1;
    return viewer_write(output, used);
}

static int viewer_raw(viewer_state *state)
{
    struct termios modes;
    if (state == NULL || tcgetattr(STDIN_FILENO, &state->saved_modes) == -1) return -1;
    state->modes_saved = true;
    modes = state->saved_modes;
    modes.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
    modes.c_iflag &= (tcflag_t)~(ICRNL | IXON);
    modes.c_cc[VMIN] = 1;
    modes.c_cc[VTIME] = 0;
    return tcsetattr(STDIN_FILENO, TCSANOW, &modes);
}

static void viewer_restore(viewer_state *state)
{
    static const char restore[] = "\033[0m\033[?25h\033[?1049l";
    if (state == NULL) return;
    (void)send_view_layout(state, 0U);
    if (state->modes_saved) (void)tcsetattr(STDIN_FILENO, TCSANOW, &state->saved_modes);
    (void)viewer_write(restore, sizeof(restore) - 1U);
}

static void keep_selected_visible(viewer_state *state)
{
    size_t rows;
    size_t columns;
    size_t page;
    if (state == NULL) return;
    viewer_dimensions(&rows, &columns);
    (void)columns;
    page = rows > 3U ? rows - 3U : 1U;
    if (state->selected < state->top) state->top = state->selected;
    if (state->selected >= state->top + page) state->top = state->selected - page + 1U;
}

static bool search_from(viewer_state *state, size_t start, bool reverse)
{
    size_t step;
    if (state == NULL || state->search_length == 0U || state->binary)
        return false;
    for (step = 0U; step < state->lines; step++) {
        size_t line_number = reverse
                                 ? (start + state->lines - step) % state->lines
                                 : (start + step) % state->lines;
        char line[VIEW_LINE_CAP];
        size_t length;
        if (read_line(state, line_number, line, &length) == 0 &&
            strstr(line, state->search) != NULL) {
            state->selected = line_number;
            keep_selected_visible(state);
            return true;
        }
    }
    return false;
}

static int read_search(viewer_state *state, const char *path)
{
    if (state == NULL || path == NULL) return -1;
    state->search_length = 0U;
    state->search[0] = '\0';
    for (size_t turn = 0U; turn < VIEW_SEARCH_CAP * 4U; turn++) {
        unsigned char byte;
        char prompt[VIEW_SEARCH_CAP + 16U];
        int length = snprintf(prompt, sizeof(prompt), "\r\033[K/%s", state->search);
        if (length < 0 || viewer_write(prompt, (size_t)length) == -1) return -1;
        if (read(STDIN_FILENO, &byte, 1U) != 1) return -1;
        if (byte == '\r' || byte == '\n') break;
        if (byte == 0x1bU) { state->search_length = 0U; break; }
        if ((byte == 0x7fU || byte == 0x08U) && state->search_length > 0U) state->search_length--;
        else if (byte >= 0x20U && state->search_length + 1U < sizeof(state->search)) state->search[state->search_length++] = (char)byte;
        state->search[state->search_length] = '\0';
    }
    if (state->search_length != 0U) (void)search_from(state, state->selected + 1U, false);
    return render_viewer(state, path);
}

static int editor_exit_status(pid_t pid)
{
    int status;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited == -1 && errno == EINTR);
    if (waited != pid) return 125;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int spawn_editor_argv(char *const arguments[])
{
    pid_t pid;
    if (arguments == NULL || arguments[0] == NULL) return 127;
    pid = fork();
    if (pid == 0) { execvp(arguments[0], arguments); _exit(errno == ENOENT ? 127 : 126); }
    return pid < 0 ? 125 : editor_exit_status(pid);
}

static int configured_editor(const char *path, size_t line)
{
    gsh_shell_config config;
    const char *home = getenv("HOME");
    char line_option[64];
    char *arguments[GSH_CONFIG_EDITOR_ARG_CAP + 2U];
    size_t index;

    if (path == NULL) return 125;
    gsh_config_defaults(&config);
    if (home != NULL) (void)gsh_config_load(&config, home, false);
    if (config.preview_editor_auto) {
        static const char *const choices[] = {"nvim", "vim", "nano"};
        for (index = 0U; index < sizeof(choices) / sizeof(choices[0]); index++) {
            if (strcmp(choices[index], "nano") == 0) {
                (void)snprintf(line_option, sizeof(line_option), "+%zu,1", line);
                arguments[0] = (char *)choices[index];
                arguments[1] = (char *)"-l";
                arguments[2] = line_option;
                arguments[3] = (char *)"--";
                arguments[4] = (char *)path;
                arguments[5] = NULL;
            } else {
                (void)snprintf(line_option, sizeof(line_option), "+%zu", line);
                arguments[0] = (char *)choices[index];
                arguments[1] = line_option;
                arguments[2] = (char *)"-c";
                arguments[3] = (char *)"set number norelativenumber";
                arguments[4] = (char *)"--";
                arguments[5] = (char *)path;
                arguments[6] = NULL;
            }
            { int status = spawn_editor_argv(arguments); if (status != 127) return status; }
        }
        return 127;
    }
    for (index = 0U; index < config.preview_editor_argc; index++) {
        const char *value = gsh_config_editor_argument(&config, index);
        arguments[index] = strcmp(value, "{file}") == 0 ? (char *)path : (char *)value;
    }
    arguments[config.preview_editor_argc] = NULL;
    return spawn_editor_argv(arguments);
}

static int reload_viewer(viewer_state *state, const char *path)
{
    size_t selected;
    if (state == NULL || path == NULL) return -1;
    selected = state->selected;
    if (state->file >= 0) (void)close(state->file);
    if (state->index >= 0) (void)close(state->index);
    state->file = -1;
    state->index = -1;
    state->language = suffix_language(path);
    if (build_line_index(state, path) == -1) return -1;
    state->selected = selected < state->lines ? selected : state->lines - 1U;
    keep_selected_visible(state);
    return 0;
}

static int edit_from_viewer(viewer_state *state, const char *path)
{
    static const char enter[] = "\033[3J\033[?1049h\033[?25l";
    int status;
    if (state == NULL || path == NULL) return -1;
    viewer_restore(state);
    state->modes_saved = false;
    status = configured_editor(path, state->selected + 1U);
    if (viewer_raw(state) == -1 ||
        viewer_write(enter, sizeof(enter) - 1U) == -1 ||
        reload_viewer(state, path) == -1) return -1;
    (void)status;
    return render_viewer(state, path);
}

static void move_vertical(viewer_state *state, long amount)
{
    if (state == NULL) return;
    if (amount < 0 && (size_t)(-amount) > state->selected) state->selected = 0U;
    else if (amount < 0) state->selected -= (size_t)(-amount);
    else if ((size_t)amount >= state->lines - state->selected) state->selected = state->lines - 1U;
    else state->selected += (size_t)amount;
    keep_selected_visible(state);
}

static int handle_escape_key(viewer_state *state)
{
    unsigned char sequence[VIEW_KEY_CAP];
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    int ready;
    ssize_t count;
    if (state == NULL) return -1;
    ready = poll(&input, 1U, 30);
    if (ready == 0) return 0;
    if (ready < 0 ||
        (input.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return -1;
    count = read(STDIN_FILENO, sequence, sizeof(sequence));
    if (count < 1 || sequence[0] != '[') return 0;
    if (count >= 2 && sequence[1] == 'A') move_vertical(state, -1);
    else if (count >= 2 && sequence[1] == 'B') move_vertical(state, 1);
    else if (count >= 2 && sequence[1] == 'C') state->horizontal++;
    else if (count >= 2 && sequence[1] == 'D' && state->horizontal > 0U) state->horizontal--;
    else if (count >= 3 && sequence[1] == '5' && sequence[2] == '~') move_vertical(state, -20);
    else if (count >= 3 && sequence[1] == '6' && sequence[2] == '~') move_vertical(state, 20);
    return 1;
}

static int viewer_read_key(viewer_state *state, const char *path,
                           unsigned char *byte, size_t *prior_rows,
                           size_t *prior_columns)
{
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    size_t rows;
    size_t columns;
    int ready;
    ssize_t count;
    if (state == NULL || path == NULL || byte == NULL ||
        prior_rows == NULL || prior_columns == NULL) return -1;
    ready = poll(&input, 1U, 100);
    if (ready == 0) {
        viewer_dimensions(&rows, &columns);
        if ((rows != *prior_rows || columns != *prior_columns) &&
            render_viewer(state, path) == -1) return -1;
        *prior_rows = rows;
        *prior_columns = columns;
        return 0;
    }
    if (ready == -1 && errno == EINTR)
        return render_viewer(state, path) == -1 ? -1 : 0;
    if (ready < 0 ||
        (input.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return -1;
    count = read(STDIN_FILENO, byte, 1U);
    if (count == -1 && errno == EINTR)
        return render_viewer(state, path) == -1 ? -1 : 0;
    if (count != 1) return -1;
    viewer_dimensions(prior_rows, prior_columns);
    return 1;
}

static int viewer_loop(viewer_state *state, const char *path)
{
    size_t prior_rows;
    size_t prior_columns;
    if (state == NULL || path == NULL) return 1;
    viewer_dimensions(&prior_rows, &prior_columns);
    for (size_t turn = 0U; turn < VIEW_LINE_INDEX_CAP; turn++) {
        unsigned char byte;
        int key = viewer_read_key(state, path, &byte, &prior_rows,
                                  &prior_columns);
        if (key < 0) return 1;
        if (key == 0) continue;
        if (byte == 'q') return 0;
        if (byte == 0x1bU) {
            int escape = handle_escape_key(state);
            if (escape == 0) continue;
        } else if (byte == 'j') move_vertical(state, 1);
        else if (byte == 'k') move_vertical(state, -1);
        else if (byte == 'n') (void)search_from(state, state->selected + 1U, false);
        else if (byte == 'N') (void)search_from(state, state->selected == 0U ? state->lines - 1U : state->selected - 1U, true);
        else if (byte == '/') { if (read_search(state, path) == -1) return 1; continue; }
        else if (byte == 'r') { if (reload_viewer(state, path) == -1) return 1; }
        else if (byte == 'e') { if (edit_from_viewer(state, path) == -1) return 1; continue; }
        else if (byte == 0x06U || byte == ' ') move_vertical(state, (long)(prior_rows > 3U ? prior_rows - 3U : 1U));
        else if (byte == 0x02U) move_vertical(state, -(long)(prior_rows > 3U ? prior_rows - 3U : 1U));
        if (render_viewer(state, path) == -1) return 1;
    }
    return 1;
}

int gsh_native_viewer(const char *path, size_t line, size_t column,
                      const gsh_builtin_io *io)
{
    static const char enter[] = "\033[?1049h\033[?25l";
    viewer_state state;
    int result;
    int saved_error;
    const gsh_builtin_resource_sink *resources;

    if (path == NULL || io == NULL || line == 0U || column == 0U) return 1;
    (void)memset(&state, 0, sizeof(state));
    state.file = -1;
    state.index = -1;
    resources = io->resources;
    state.resource_descriptor = resources == NULL ? -1 : resources->descriptor;
    state.language = suffix_language(path);
    state.horizontal = column > 9U ? column - 9U : 0U;
    state.preferred_column = column - state.horizontal;
    if (build_line_index(&state, path) == -1) {
        saved_error = errno;
        if (state.file >= 0) (void)close(state.file);
        if (state.index >= 0) (void)close(state.index);
        errno = saved_error;
        return 1;
    }
    state.selected = line - 1U < state.lines ? line - 1U : state.lines - 1U;
    keep_selected_visible(&state);
    if (viewer_raw(&state) == -1 ||
        viewer_write(enter, sizeof(enter) - 1U) == -1 ||
        render_viewer(&state, path) == -1) result = 1;
    else result = viewer_loop(&state, path);
    saved_error = errno;
    viewer_restore(&state);
    if (state.file >= 0) (void)close(state.file);
    if (state.index >= 0) (void)close(state.index);
    errno = saved_error;
    return result;
}
