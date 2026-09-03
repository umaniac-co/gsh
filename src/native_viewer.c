#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "native_viewer.h"
#include "resource_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h> /* CANON-INCLUDE: macos */
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum {
    VIEW_READ_CAP = 16384,
    VIEW_LINE_CAP = 8192,
    VIEW_RENDER_CAP = 262144,
    VIEW_SEARCH_CAP = 128,
    VIEW_LINE_INDEX_CAP = 1048576,
    VIEW_KEY_CAP = 16,
    VIEW_ITERM_RAW_CHUNK = 2046,
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
    gsh_terminal_images_mode images_mode;
    uint32_t image_protocol;
    uint32_t frame_generation;
    uint32_t frame_id;
    uint32_t frame_format;
    uint32_t frame_pixel_width;
    uint32_t frame_pixel_height;
    uint32_t frame_row;
    uint32_t frame_column;
    uint32_t frame_rows;
    uint32_t frame_columns;
    bool frame_pending;
    bool frame_sent;
    bool frame_temporary;
    char frame_path[PATH_MAX];
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
    bool selection_gutter;
    size_t selection_row;
} viewer_layout;

static bool viewer_png_protocol_available(const viewer_state *state);

static bool send_frame_datagram(int descriptor, const void *message,
                                size_t length)
{
    if (descriptor < 0 || message == NULL || length == 0U ||
        length > sizeof(gsh_preview_frame_record) +
            GSH_PREVIEW_FRAME_CHUNK_CAP) return false;
    for (size_t attempt = 0U; attempt < 64U; attempt++) {
        ssize_t written = write(descriptor, message, length);

        if (written == (ssize_t)length) return true;
        if (written == -1 && errno == EINTR) continue;
        if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd output = {descriptor, POLLOUT, 0};
            int ready = poll(&output, 1U, 5);

            if (ready > 0 && (output.revents & POLLOUT) != 0) continue;
        }
        return false;
    }
    return false;
}

static void initialize_frame_record(const viewer_state *state,
                                    gsh_preview_frame_record *frame,
                                    uint32_t event)
{
    if (state == NULL || frame == NULL) return;
    (void)memset(frame, 0, sizeof(*frame));
    frame->version = GSH_RESOURCE_PROTOCOL_VERSION;
    frame->size = (uint32_t)sizeof(*frame);
    frame->event = event;
    frame->generation = state->frame_generation;
    frame->frame_id = state->frame_id;
    frame->format = state->frame_format;
    frame->pixel_width = state->frame_pixel_width;
    frame->pixel_height = state->frame_pixel_height;
    frame->cell_row = state->frame_row;
    frame->cell_column = state->frame_column;
    frame->cell_rows = state->frame_rows;
    frame->cell_columns = state->frame_columns;
}

static void delete_sent_frame(viewer_state *state)
{
    gsh_preview_frame_record frame;

    if (state == NULL || !state->frame_sent ||
        state->resource_descriptor < 0) return;
    initialize_frame_record(state, &frame,
                            GSH_RESOURCE_PROTOCOL_FRAME_DELETE);
    (void)send_frame_datagram(state->resource_descriptor, &frame,
                              sizeof(frame));
    state->frame_sent = false;
}

static bool send_frame_chunks(viewer_state *state, int descriptor,
                              uint32_t total)
{
    union {
        gsh_preview_frame_record frame;
        unsigned char bytes[sizeof(gsh_preview_frame_record) +
                            GSH_PREVIEW_FRAME_CHUNK_CAP];
    } message;
    gsh_preview_frame_record *frame = &message.frame;
    uint32_t offset = 0U;

    if (state == NULL || descriptor < 0 || total == 0U) return false;
    for (size_t turn = 0U; offset < total &&
         turn < GSH_PREVIEW_FRAME_TOTAL_CAP; turn++) {
        size_t request = total - offset;
        ssize_t count;

        if (state->image_protocol == GSH_RESOURCE_IMAGE_ITERM &&
            request > VIEW_ITERM_RAW_CHUNK) request = VIEW_ITERM_RAW_CHUNK;
        else if (request > GSH_PREVIEW_FRAME_CHUNK_CAP)
            request = GSH_PREVIEW_FRAME_CHUNK_CAP;
        do { count = pread(descriptor, message.bytes + sizeof(*frame), request,
                           (off_t)offset); } while (count == -1 &&
                                                   errno == EINTR);
        if (count <= 0) return false;
        initialize_frame_record(state, frame,
                                GSH_RESOURCE_PROTOCOL_FRAME_CHUNK);
        frame->offset = offset;
        frame->chunk_length = (uint32_t)count;
        frame->size = (uint32_t)(sizeof(*frame) + (size_t)count);
        if (!send_frame_datagram(state->resource_descriptor, message.bytes,
                                 frame->size)) return false;
        offset += (uint32_t)count;
    }
    return offset == total;
}

/* ── Workers Send Bytes, Never Terminal Programs ─────────────────
 * Markdown image bytes are sent outside the reactor.
 * Earlier designs let that worker print OSC directly, allowing a malformed
 * document or interrupted transfer to leak terminal state into scrollback.
 * The private datagram stream now carries typed, ordered raw chunks only.
 * The compositor is the sole component that may wrap them in image escapes.
 * ─────────────────────────────────────────────────────────────── */
static void send_pending_frame(viewer_state *state)
{
    gsh_preview_frame_record frame;
    struct stat status;
    int descriptor;
    bool complete;

    if (state == NULL || !state->frame_pending ||
        state->resource_descriptor < 0) return;
    descriptor = open(state->frame_path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0 || fstat(descriptor, &status) == -1 ||
        !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        status.st_size > GSH_PREVIEW_FRAME_TOTAL_CAP) {
        if (descriptor >= 0) (void)close(descriptor);
        if (state->frame_temporary) (void)unlink(state->frame_path);
        state->frame_temporary = false;
        return;
    }
    initialize_frame_record(state, &frame,
                            GSH_RESOURCE_PROTOCOL_FRAME_BEGIN);
    frame.total_length = (uint32_t)status.st_size;
    if (!send_frame_datagram(state->resource_descriptor, &frame,
                             sizeof(frame))) {
        (void)close(descriptor);
        if (state->frame_temporary) (void)unlink(state->frame_path);
        state->frame_temporary = false;
        return;
    }
    complete = send_frame_chunks(state, descriptor, frame.total_length);
    (void)close(descriptor);
    if (state->frame_temporary) (void)unlink(state->frame_path);
    state->frame_temporary = false;
    initialize_frame_record(state, &frame,
        complete ? GSH_RESOURCE_PROTOCOL_FRAME_END
                 : GSH_RESOURCE_PROTOCOL_FRAME_DELETE);
    if (send_frame_datagram(state->resource_descriptor, &frame,
                            sizeof(frame)) && complete)
        state->frame_sent = true;
}

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

static bool bounded_contains(const char *bytes, size_t length,
                             const char *needle, size_t needle_length)
{
    size_t offset;

    if (bytes == NULL || needle == NULL || needle_length == 0U ||
        needle_length > length) return false;
    for (offset = 0U; offset <= length - needle_length; offset++) {
        if (memcmp(bytes + offset, needle, needle_length) == 0) return true;
    }
    return false;
}

static view_language shebang_language(const char *bytes, size_t length,
                                      view_language fallback)
{
    size_t checked;

    if (bytes == NULL || length < 2U || bytes[0] != '#' || bytes[1] != '!') return fallback;
    checked = length < 256U ? length : 256U;
    if (bounded_contains(bytes, checked, "python", 6U)) return VIEW_PYTHON;
    if (bounded_contains(bytes, checked, "sh", 2U)) return VIEW_SHELL;
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
    static const char selected[] = "\033[1;38;5;81m>\033[0m ";
    char cursor[64];
    int length;
    if (layout == NULL || output == NULL || used == NULL) return -1;
    if (layout->split) {
        length = snprintf(cursor, sizeof(cursor), "\033[%zu;%zuH", row + 1U,
                          layout->separator_column);
        if (length < 0 || (size_t)length >= sizeof(cursor) ||
            render_append(output, used, cursor, (size_t)length) == -1 ||
            render_append(output, used, separator,
                          sizeof(separator) - 1U) == -1) return -1;
    }
    if (!layout->selection_gutter) return 0;
    return row == layout->selection_row
               ? render_append(output, used, selected, sizeof(selected) - 1U)
               : render_append(output, used, "  ", 2U);
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

static int append_placeholder_cell(bool unicode, const char *wide,
                                   char ascii,
                                   char output[VIEW_RENDER_CAP],
                                   size_t *used)
{
    char narrow[1] = {ascii};

    if (output == NULL || used == NULL || wide == NULL) return -1;
    return unicode ? render_append(output, used, wide, strlen(wide))
                   : render_append(output, used, narrow, sizeof(narrow));
}

/* ── Missing Graphics Preserve Document Geometry ─────────────────
 * Inline graphics protocols vary across terminals and often disappear across
 * SSH or a multiplexer.  Collapsing an unavailable image would move every
 * following Markdown block and make page navigation inconsistent.  The
 * viewer therefore reserves the same bounded cell rectangle and draws a
 * deterministic frame.  UTF-8 terminals receive box glyphs; every other
 * terminal receives the same geometry in portable ASCII.
 * ─────────────────────────────────────────────────────────────── */
static int render_placeholder_row(const viewer_layout *layout, size_t screen_row,
                                  size_t row, size_t height, size_t width,
                                  size_t label_begin, size_t label_length,
                                  const char *label, bool unicode,
                                  char output[VIEW_RENDER_CAP], size_t *used)
{
    size_t diagonal_left = 1U;
    size_t diagonal_right = width - 2U;

    if (layout == NULL || label == NULL || output == NULL || used == NULL)
        return -1;
    if (height > 3U && row > 0U && row + 1U < height) {
        diagonal_left = 1U + (width - 3U) * (row - 1U) / (height - 3U);
        diagonal_right = width - 2U -
            (width - 3U) * (row - 1U) / (height - 3U);
    }
    if (begin_viewer_row(layout, screen_row, output, used) == -1) return -1;
    for (size_t column = 0U; column < width; column++) {
        bool label_cell = row == height / 2U && column >= label_begin &&
                          column < label_begin + label_length;
        if (label_cell) {
            if (render_append(output, used, label + column - label_begin,
                              1U) == -1) return -1;
        } else if (row == 0U || row + 1U == height) {
            if (column == 0U && append_placeholder_cell(unicode,
                    row == 0U ? "┌" : "└", '+', output, used) == -1)
                return -1;
            if (column + 1U == width && append_placeholder_cell(unicode,
                    row == 0U ? "┐" : "┘", '+', output, used) == -1)
                return -1;
            if (column > 0U && column + 1U < width &&
                append_placeholder_cell(unicode, "─", '-', output,
                                        used) == -1) return -1;
        } else if (column == 0U || column + 1U == width) {
            if (append_placeholder_cell(unicode, "│", '|', output,
                                        used) == -1) return -1;
        } else if (column == diagonal_left && column == diagonal_right) {
            if (append_placeholder_cell(unicode, "╳", 'X', output,
                                        used) == -1) return -1;
        } else if (column == diagonal_left) {
            if (append_placeholder_cell(unicode, "╲", '\\', output,
                                        used) == -1) return -1;
        } else if (column == diagonal_right) {
            if (append_placeholder_cell(unicode, "╱", '/', output,
                                        used) == -1) return -1;
        } else if (render_append(output, used, " ", 1U) == -1) return -1;
    }
    return end_viewer_row(layout, false, output, used);
}

static int render_placeholder(const viewer_layout *layout, size_t first_row,
                              size_t height, const char *label,
                              char output[VIEW_RENDER_CAP], size_t *used)
{
    size_t width;
    size_t label_length;
    size_t label_begin;
    bool unicode;

    if (layout == NULL || label == NULL || output == NULL || used == NULL)
        return -1;
    width = layout->content_columns;
    if (width < 8U || height < 4U) {
        if (begin_viewer_row(layout, first_row, output, used) == -1 ||
            render_append(output, used, "[image unavailable]", 19U) == -1 ||
            end_viewer_row(layout, false, output, used) == -1) return -1;
        return 0;
    }
    unicode = MB_CUR_MAX > 1;
    label_length = strlen(label);
    if (label_length > width - 4U) label_length = width - 4U;
    label_begin = (width - label_length) / 2U;
    for (size_t row = 0U; row < height; row++) {
        if (render_placeholder_row(layout, first_row + row, row, height,
                width, label_begin, label_length, label, unicode, output,
                used) == -1) return -1;
    }
    return 0;
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

static view_language markdown_code_language(const char *text, size_t length)
{
    if (text == NULL) return VIEW_PLAIN;
    if ((length == 6U && memcmp(text, "python", 6U) == 0) ||
        (length == 2U && memcmp(text, "py", 2U) == 0)) return VIEW_PYTHON;
    if (length == 2U && memcmp(text, "sh", 2U) == 0) return VIEW_SHELL;
    if (length == 4U && memcmp(text, "bash", 4U) == 0) return VIEW_SHELL;
    if (length == 1U && text[0] == 'c') return VIEW_C;
    if (length == 3U && memcmp(text, "cpp", 3U) == 0) return VIEW_C;
    if (length == 2U && memcmp(text, "js", 2U) == 0) return VIEW_JAVASCRIPT;
    if (length == 10U && memcmp(text, "javascript", 10U) == 0)
        return VIEW_JAVASCRIPT;
    if (length == 2U && memcmp(text, "ts", 2U) == 0) return VIEW_JAVASCRIPT;
    if (length == 4U && memcmp(text, "rust", 4U) == 0) return VIEW_RUST;
    if (length == 2U && memcmp(text, "go", 2U) == 0) return VIEW_GO;
    if (length == 4U && memcmp(text, "json", 4U) == 0) return VIEW_JSON;
    if ((length == 4U && memcmp(text, "toml", 4U) == 0) ||
        (length == 4U && memcmp(text, "yaml", 4U) == 0)) return VIEW_DATA;
    return VIEW_PLAIN;
}

static bool markdown_fence_marker(const char *text, size_t length,
                                  view_language *language)
{
    size_t offset = 0U;
    char marker;

    if (text == NULL || language == NULL) return false;
    while (offset < length && offset < 3U && text[offset] == ' ') offset++;
    if (offset + 3U > length ||
        !((text[offset] == '`' && text[offset + 1U] == '`' &&
           text[offset + 2U] == '`') ||
          (text[offset] == '~' && text[offset + 1U] == '~' &&
           text[offset + 2U] == '~'))) return false;
    marker = text[offset];
    while (offset < length && text[offset] == marker) offset++;
    while (offset < length && text[offset] == ' ') offset++;
    *language = markdown_code_language(text + offset, length - offset);
    return true;
}

static bool markdown_fence_before(const viewer_state *state, size_t line,
                                  view_language *language)
{
    char text[VIEW_LINE_CAP];
    bool fenced = false;

    if (state == NULL || language == NULL) return false;
    *language = VIEW_PLAIN;
    for (size_t index = 0U; index < line && index < VIEW_LINE_INDEX_CAP;
         index++) {
        size_t length = 0U;
        view_language candidate = VIEW_PLAIN;

        if (read_line(state, index, text, &length) == -1) break;
        if (!markdown_fence_marker(text, length, &candidate)) continue;
        fenced = !fenced;
        if (fenced) *language = candidate;
        else *language = VIEW_PLAIN;
    }
    return fenced;
}

static size_t markdown_find(const char *text, size_t length, size_t begin,
                            char wanted)
{
    if (text == NULL || begin > length) return length;
    for (size_t index = begin; index < length; index++) {
        if (text[index] == wanted) return index;
    }
    return length;
}

static size_t markdown_math_group(const char *text, size_t length,
                                  size_t begin, size_t *content_begin,
                                  size_t *content_length)
{
    if (text == NULL || content_begin == NULL || content_length == NULL ||
        begin >= length || text[begin] != '{') return 0U;
    for (size_t offset = begin + 1U; offset < length; offset++) {
        if (text[offset] == '}') {
            *content_begin = begin + 1U;
            *content_length = offset - begin - 1U;
            return offset + 1U;
        }
        if (text[offset] == '{') return 0U;
    }
    return 0U;
}

static const char *markdown_script_character(unsigned char byte, bool upper)
{
    static const char *const superscript[] = {
        "⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹",
    };
    static const char *const subscript[] = {
        "₀", "₁", "₂", "₃", "₄", "₅", "₆", "₇", "₈", "₉",
    };

    if (byte >= '0' && byte <= '9')
        return upper ? superscript[byte - '0'] : subscript[byte - '0'];
    if (upper && byte == '+') return "⁺";
    if (upper && byte == '-') return "⁻";
    if (upper && byte == '=') return "⁼";
    if (!upper && byte == '+') return "₊";
    if (!upper && byte == '-') return "₋";
    if (!upper && byte == '=') return "₌";
    return NULL;
}

static size_t render_markdown_script(const char *text, size_t length,
                                     bool upper,
                                     char output[VIEW_RENDER_CAP],
                                     size_t *used, size_t *columns)
{
    size_t begin = 1U;
    size_t script_length = 1U;
    size_t end;

    if (text == NULL || output == NULL || used == NULL || columns == NULL ||
        length < 2U) return 0U;
    if (text[1] == '{') {
        end = markdown_math_group(text, length, 1U, &begin, &script_length);
        if (end == 0U) return 0U;
    } else end = 2U;
    for (size_t index = 0U; index < script_length; index++) {
        const char *replacement = markdown_script_character(
            (unsigned char)text[begin + index], upper);

        if (replacement == NULL ||
            render_append(output, used, replacement,
                          strlen(replacement)) == -1) return 0U;
        (*columns)++;
    }
    return end;
}

static size_t render_markdown_fraction(const char *text, size_t length,
                                       char output[VIEW_RENDER_CAP],
                                       size_t *used, size_t *columns)
{
    size_t numerator_begin;
    size_t numerator_length;
    size_t denominator_begin;
    size_t denominator_length;
    size_t next;
    size_t end;

    if (text == NULL || output == NULL || used == NULL || columns == NULL ||
        length < 8U || memcmp(text, "\\frac{", 6U) != 0) return 0U;
    next = markdown_math_group(text, length, 5U, &numerator_begin,
                               &numerator_length);
    if (next == 0U || next >= length || text[next] != '{') return 0U;
    end = markdown_math_group(text, length, next, &denominator_begin,
                              &denominator_length);
    if (end == 0U || render_append(output, used, "(", 1U) == -1 ||
        render_safe_text(output, used, text + numerator_begin,
                         numerator_length) == -1 ||
        render_append(output, used, ")/(", 3U) == -1 ||
        render_safe_text(output, used, text + denominator_begin,
                         denominator_length) == -1 ||
        render_append(output, used, ")", 1U) == -1) return 0U;
    *columns += numerator_length + denominator_length + 5U;
    return end;
}

static size_t render_markdown_root(const char *text, size_t length,
                                   char output[VIEW_RENDER_CAP], size_t *used,
                                   size_t *columns)
{
    size_t content_begin;
    size_t content_length;
    size_t end;

    if (text == NULL || output == NULL || used == NULL || columns == NULL ||
        length < 7U || memcmp(text, "\\sqrt{", 6U) != 0) return 0U;
    end = markdown_math_group(text, length, 5U, &content_begin,
                              &content_length);
    if (end == 0U || render_append(output, used, "√(", strlen("√(")) == -1 ||
        render_safe_text(output, used, text + content_begin,
                         content_length) == -1 ||
        render_append(output, used, ")", 1U) == -1) return 0U;
    *columns += content_length + 3U;
    return end;
}

static size_t render_markdown_math_construct(
    const char *text, size_t length, char output[VIEW_RENDER_CAP],
    size_t *used, size_t *columns)
{
    size_t consumed;

    if (text == NULL || output == NULL || used == NULL || columns == NULL ||
        length == 0U) return 0U;
    consumed = render_markdown_fraction(text, length, output, used, columns);
    if (consumed != 0U) return consumed;
    consumed = render_markdown_root(text, length, output, used, columns);
    if (consumed != 0U) return consumed;
    if (text[0] == '^' || text[0] == '_')
        return render_markdown_script(text, length, text[0] == '^', output,
                                      used, columns);
    if (length >= 5U && memcmp(text, "\\left", 5U) == 0) return 5U;
    if (length >= 6U && memcmp(text, "\\right", 6U) == 0) return 6U;
    if (length >= 14U && memcmp(text, "\\begin{matrix}", 14U) == 0) {
        if (render_append(output, used, "[ ", 2U) == -1) return 0U;
        *columns += 2U;
        return 14U;
    }
    if (length >= 12U && memcmp(text, "\\end{matrix}", 12U) == 0) {
        if (render_append(output, used, " ]", 2U) == -1) return 0U;
        *columns += 2U;
        return 12U;
    }
    if (text[0] == '&') {
        if (render_append(output, used, " ", 1U) == -1) return 0U;
        (*columns)++;
        return 1U;
    }
    if (length >= 2U && text[0] == '\\' && text[1] == '\\') {
        if (render_append(output, used, "; ", 2U) == -1) return 0U;
        *columns += 2U;
        return 2U;
    }
    return 0U;
}

static int render_markdown_math(const char *text, size_t length,
                                size_t width,
                                char output[VIEW_RENDER_CAP], size_t *used,
                                size_t *rendered_columns)
{
    static const struct {
        const char *source;
        const char *rendered;
    } symbols[] = {
        {"\\alpha", "α"}, {"\\beta", "β"}, {"\\gamma", "γ"},
        {"\\delta", "δ"}, {"\\theta", "θ"}, {"\\lambda", "λ"},
        {"\\mu", "μ"}, {"\\pi", "π"}, {"\\sigma", "σ"},
        {"\\phi", "φ"}, {"\\omega", "ω"},
        {"\\times", "×"}, {"\\le", "≤"}, {"\\ge", "≥"},
        {"\\neq", "≠"}, {"\\infty", "∞"}, {"\\sum", "Σ"},
        {"\\int", "∫"}, {"\\prod", "Π"}, {"\\pm", "±"},
        {"\\to", "→"}, {"\\in", "∈"}, {"\\partial", "∂"},
    };
    size_t offset = 0U;
    size_t columns = 0U;

    if (text == NULL || output == NULL || used == NULL ||
        rendered_columns == NULL) return -1;
    while (offset < length && columns < width) {
        bool replaced = false;
        size_t constructed = render_markdown_math_construct(
            text + offset, length - offset, output, used, &columns);

        if (constructed != 0U) { offset += constructed; continue; }
        for (size_t index = 0U;
             index < sizeof(symbols) / sizeof(symbols[0]); index++) {
            size_t source_length = strlen(symbols[index].source);
            if (source_length <= length - offset &&
                memcmp(text + offset, symbols[index].source,
                       source_length) == 0) {
                if (render_append(output, used, symbols[index].rendered,
                                  strlen(symbols[index].rendered)) == -1)
                    return -1;
                offset += source_length;
                columns++;
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            if (render_safe_text(output, used, text + offset, 1U) == -1)
                return -1;
            offset++;
            columns++;
        }
    }
    *rendered_columns = columns;
    return 0;
}

static int render_markdown_plain(const char *text, size_t length,
                                 size_t *offset, size_t *columns,
                                 char output[VIEW_RENDER_CAP], size_t *used)
{
    size_t sequence;

    if (text == NULL || offset == NULL || columns == NULL || output == NULL ||
        used == NULL || *offset >= length) return -1;
    sequence = safe_utf8_sequence((const unsigned char *)text + *offset,
                                  length - *offset);
    if (sequence == 0U) sequence = 1U;
    if (render_safe_text(output, used, text + *offset, sequence) == -1)
        return -1;
    *offset += sequence;
    (*columns)++;
    return 0;
}

static int render_markdown_inline(const char *text, size_t length,
                                  size_t width,
                                  char output[VIEW_RENDER_CAP], size_t *used)
{
    size_t offset = 0U;
    size_t columns = 0U;
    bool bold = false;
    bool italic = false;

    if (text == NULL || output == NULL || used == NULL) return -1;
    while (offset < length && columns < width) {
        if (offset + 1U < length && text[offset] == '*' &&
            text[offset + 1U] == '*') {
            bold = !bold;
            if (render_append(output, used, bold ? "\033[1m" : "\033[22m",
                              bold ? 4U : 5U) == -1) return -1;
            offset += 2U;
            continue;
        }
        if (text[offset] == '*' || text[offset] == '_') {
            italic = !italic;
            if (render_append(output, used, italic ? "\033[3m" : "\033[23m",
                              italic ? 4U : 5U) == -1) return -1;
            offset++;
            continue;
        }
        if (text[offset] == '`') {
            size_t end = markdown_find(text, length, offset + 1U, '`');
            if (end < length) {
                size_t shown = end - offset - 1U;
                if (shown > width - columns) shown = width - columns;
                if (render_append(output, used, "\033[38;5;114m", 11U) == -1 ||
                    render_safe_text(output, used, text + offset + 1U,
                                     shown) == -1 ||
                    render_append(output, used, "\033[39m", 5U) == -1)
                    return -1;
                columns += shown;
                offset = end + 1U;
                continue;
            }
        }
        if (text[offset] == '$') {
            size_t end = markdown_find(text, length, offset + 1U, '$');
            if (end < length) {
                size_t math_columns = 0U;

                if (render_append(output, used, "\033[38;5;177m", 11U) == -1 ||
                    render_markdown_math(text + offset + 1U,
                        end - offset - 1U, width - columns, output, used,
                        &math_columns) == -1 ||
                    render_append(output, used, "\033[39m", 5U) == -1)
                    return -1;
                columns += math_columns;
                offset = end + 1U;
                continue;
            }
        }
        if (text[offset] == '[') {
            size_t close = markdown_find(text, length, offset + 1U, ']');
            if (close + 1U < length && text[close + 1U] == '(') {
                size_t end = markdown_find(text, length, close + 2U, ')');
                if (end < length) {
                    size_t shown = close - offset - 1U;
                    if (shown > width - columns) shown = width - columns;
                    if (render_append(output, used, "\033[4;38;5;81m", 12U) == -1 ||
                        render_safe_text(output, used, text + offset + 1U,
                                         shown) == -1 ||
                        render_append(output, used, "\033[0m", 4U) == -1)
                        return -1;
                    columns += shown;
                    offset = end + 1U;
                    continue;
                }
            }
        }
        if (render_markdown_plain(text, length, &offset, &columns,
                                  output, used) == -1) return -1;
    }
    if ((bold || italic) && render_append(output, used, "\033[0m", 4U) == -1)
        return -1;
    return 0;
}

static bool markdown_image_label(const char *text, size_t length,
                                 char label[256], char target[PATH_MAX])
{
    size_t begin = 0U;
    size_t alt_end;
    size_t path_end;
    size_t alt_length;
    size_t target_begin;
    size_t target_length;
    int result;

    if (text == NULL || label == NULL || target == NULL) return false;
    while (begin < length && (text[begin] == ' ' || text[begin] == '\t'))
        begin++;
    if (begin + 4U >= length || text[begin] != '!' ||
        text[begin + 1U] != '[') return false;
    alt_end = markdown_find(text, length, begin + 2U, ']');
    if (alt_end + 1U >= length || text[alt_end + 1U] != '(') return false;
    path_end = markdown_find(text, length, alt_end + 2U, ')');
    if (path_end >= length) return false;
    target_begin = alt_end + 2U;
    while (target_begin < path_end && text[target_begin] == ' ')
        target_begin++;
    if (target_begin < path_end && text[target_begin] == '<') {
        target_begin++;
        target_length = path_end - target_begin;
        while (target_length > 0U &&
               text[target_begin + target_length - 1U] != '>')
            target_length--;
        if (target_length == 0U) return false;
    } else {
        target_length = path_end - target_begin;
        for (size_t index = 0U; index < target_length; index++) {
            if (text[target_begin + index] == ' ' ||
                text[target_begin + index] == '\t') {
                target_length = index;
                break;
            }
        }
    }
    while (target_length > 0U &&
           (text[target_begin + target_length - 1U] == ' ' ||
            text[target_begin + target_length - 1U] == '>')) target_length--;
    if (target_length == 0U || target_length >= PATH_MAX) return false;
    (void)memcpy(target, text + target_begin, target_length);
    target[target_length] = '\0';
    alt_length = alt_end - begin - 2U;
    if (alt_length == 0U) {
        begin = alt_end + 2U;
        alt_length = path_end - begin;
    } else begin += 2U;
    if (alt_length > 180U) alt_length = 180U;
    result = snprintf(label, 256U, "%.*s - image preview unavailable",
                      (int)alt_length, text + begin);
    return result > 0 && result < 256;
}

static bool image_target_is_remote(const char *target)
{
    size_t length;

    if (target == NULL) return true;
    length = strlen(target);
    for (size_t index = 0U; index < length && index < 32U; index++) {
        unsigned char byte = (unsigned char)target[index];
        if (byte == '/' || byte == '\\') return false;
        if (byte == ':') return index > 0U;
        if (!((byte >= 'a' && byte <= 'z') ||
              (byte >= 'A' && byte <= 'Z') ||
              (index > 0U && byte >= '0' && byte <= '9') ||
              (index > 0U && (byte == '+' || byte == '-' || byte == '.'))))
            return false;
    }
    return false;
}

static bool image_path_resolve(const char *document, const char *target,
                               char resolved[PATH_MAX])
{
    char document_path[PATH_MAX];
    char candidate[PATH_MAX];
    char *slash;
    size_t directory_length;
    struct stat status;
    int length;

    if (document == NULL || target == NULL || resolved == NULL ||
        image_target_is_remote(target) ||
        realpath(document, document_path) == NULL) return false;
    slash = strrchr(document_path, '/');
    if (slash == NULL) return false;
    if (slash == document_path) slash[1] = '\0';
    else *slash = '\0';
    length = target[0] == '/'
        ? snprintf(candidate, sizeof(candidate), "%s", target)
        : snprintf(candidate, sizeof(candidate), "%s/%s", document_path,
                   target);
    if (length < 0 || (size_t)length >= sizeof(candidate) ||
        realpath(candidate, resolved) == NULL) return false;
    directory_length = strlen(document_path);
    if (!(directory_length == 1U && document_path[0] == '/') &&
        (strncmp(document_path, resolved, directory_length) != 0 ||
         resolved[directory_length] != '/')) return false;
    return stat(resolved, &status) == 0 && S_ISREG(status.st_mode);
}

static uint32_t image_u32_be(const unsigned char *bytes)
{
    if (bytes == NULL) return 0U;
    return ((uint32_t)bytes[0] << 24U) | ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) | (uint32_t)bytes[3];
}

static bool jpeg_dimensions(const unsigned char *bytes, size_t length,
                            size_t *width, size_t *height)
{
    size_t offset = 2U;

    if (bytes == NULL || width == NULL || height == NULL || length < 4U ||
        bytes[0] != 0xffU || bytes[1] != 0xd8U) return false;
    for (size_t segment = 0U; segment < 256U && offset + 4U <= length;
         segment++) {
        size_t segment_length;
        unsigned char marker;

        while (offset < length && bytes[offset] == 0xffU) offset++;
        if (offset >= length) break;
        marker = bytes[offset++];
        if (marker == 0xd8U || marker == 0xd9U) continue;
        if (offset + 2U > length) break;
        segment_length = ((size_t)bytes[offset] << 8U) | bytes[offset + 1U];
        if (segment_length < 2U || segment_length > length - offset) break;
        if ((marker >= 0xc0U && marker <= 0xc3U) && segment_length >= 7U) {
            *height = ((size_t)bytes[offset + 3U] << 8U) | bytes[offset + 4U];
            *width = ((size_t)bytes[offset + 5U] << 8U) | bytes[offset + 6U];
            return *width > 0U && *height > 0U;
        }
        offset += segment_length;
    }
    return false;
}

static bool image_information(const char *path, size_t *width, size_t *height,
                              uint32_t *format)
{
    unsigned char bytes[4096];
    int descriptor;
    ssize_t count;

    if (path == NULL || width == NULL || height == NULL || format == NULL)
        return false;
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return false;
    do { count = read(descriptor, bytes, sizeof(bytes)); }
    while (count == -1 && errno == EINTR);
    (void)close(descriptor);
    if (count >= 24 && memcmp(bytes, "\x89PNG\r\n\x1a\n", 8U) == 0) {
        *width = image_u32_be(bytes + 16U);
        *height = image_u32_be(bytes + 20U);
        *format = GSH_RESOURCE_PROTOCOL_FRAME_PNG;
        return *width > 0U && *height > 0U;
    }
    if (count >= 10 && (memcmp(bytes, "GIF87a", 6U) == 0 ||
                        memcmp(bytes, "GIF89a", 6U) == 0)) {
        *width = (size_t)bytes[6] | ((size_t)bytes[7] << 8U);
        *height = (size_t)bytes[8] | ((size_t)bytes[9] << 8U);
        *format = GSH_RESOURCE_PROTOCOL_FRAME_GIF;
        return *width > 0U && *height > 0U;
    }
    if (count > 0 && jpeg_dimensions(bytes, (size_t)count, width, height)) {
        *format = GSH_RESOURCE_PROTOCOL_FRAME_JPEG;
        return true;
    }
    return false;
}

static size_t markdown_image_height(const char *document, const char *target,
                                    size_t columns)
{
    char resolved[PATH_MAX];
    size_t width;
    size_t height;
    size_t rows = 8U;
    uint32_t format;

    if (document == NULL || target == NULL || columns == 0U) return rows;
    if (image_path_resolve(document, target, resolved) &&
        image_information(resolved, &width, &height, &format) && width > 0U &&
        height <= SIZE_MAX / columns) {
        rows = height * columns / width / 2U;
    }
    if (rows < 4U) rows = 4U;
    if (rows > 16U) rows = 16U;
    return rows;
}

static void prepare_markdown_frame(viewer_state *state, const char *document,
                                   const char *target,
                                   const viewer_layout *layout, size_t row,
                                   size_t rows)
{
    char resolved[PATH_MAX];
    size_t width;
    size_t height;
    uint32_t format;

    if (state == NULL || document == NULL || target == NULL || layout == NULL ||
        state->frame_pending || state->images_mode == GSH_TERMINAL_IMAGES_OFF ||
        !viewer_png_protocol_available(state) ||
        !image_path_resolve(document, target, resolved) ||
        !image_information(resolved, &width, &height, &format) ||
        width > UINT32_MAX || height > UINT32_MAX ||
        strlen(resolved) >= sizeof(state->frame_path)) return;
    (void)memcpy(state->frame_path, resolved, strlen(resolved) + 1U);
    state->frame_format = format;
    state->frame_pixel_width = (uint32_t)width;
    state->frame_pixel_height = (uint32_t)height;
    state->frame_row = (uint32_t)(row + 1U);
    state->frame_column = (uint32_t)layout->content_column;
    state->frame_rows = (uint32_t)rows;
    state->frame_columns = (uint32_t)layout->content_columns;
    state->frame_pending = true;
}

static int render_markdown_rule(const viewer_layout *layout, size_t row,
                                char output[VIEW_RENDER_CAP], size_t *used)
{
    bool unicode;

    if (layout == NULL || output == NULL || used == NULL) return -1;
    unicode = MB_CUR_MAX > 1;
    if (begin_viewer_row(layout, row, output, used) == -1 ||
        render_append(output, used, "\033[38;5;240m", 11U) == -1) return -1;
    for (size_t column = 0U; column < layout->content_columns; column++) {
        if (append_placeholder_cell(unicode, "─", '-', output, used) == -1)
            return -1;
    }
    return render_append(output, used, "\033[0m", 4U) == -1 ||
           end_viewer_row(layout, false, output, used) == -1 ? -1 : 0;
}

static bool markdown_display_formula(const char *text, size_t length,
                                     size_t *begin, size_t *formula_length)
{
    size_t first = 0U;
    size_t last = length;

    if (text == NULL || begin == NULL || formula_length == NULL) return false;
    while (first < last && (text[first] == ' ' || text[first] == '\t'))
        first++;
    while (last > first && (text[last - 1U] == ' ' ||
                            text[last - 1U] == '\t')) last--;
    if (last - first >= 4U && text[first] == '$' &&
        text[first + 1U] == '$' && text[last - 2U] == '$' &&
        text[last - 1U] == '$') {
        *begin = first + 2U;
        *formula_length = last - first - 4U;
        return true;
    }
    if (last - first >= 4U && text[first] == '\\' &&
        text[first + 1U] == '[' && text[last - 2U] == '\\' &&
        text[last - 1U] == ']') {
        *begin = first + 2U;
        *formula_length = last - first - 4U;
        return true;
    }
    return false;
}

static bool markdown_table_line(const char *text, size_t length)
{
    size_t pipes = 0U;

    if (text == NULL) return false;
    for (size_t index = 0U; index < length; index++) {
        if (text[index] == '|' && (index == 0U || text[index - 1U] != '\\'))
            pipes++;
    }
    return pipes >= 2U;
}

static bool markdown_table_separator(const char *text, size_t length)
{
    size_t dashes = 0U;

    if (!markdown_table_line(text, length)) return false;
    for (size_t index = 0U; index < length; index++) {
        if (text[index] == '-') dashes++;
        else if (text[index] != '|' && text[index] != ':' &&
                 text[index] != ' ' && text[index] != '\t') return false;
    }
    return dashes >= 3U;
}

static int render_markdown_table(const viewer_layout *layout, size_t row,
                                 const char *text, size_t length,
                                 char output[VIEW_RENDER_CAP], size_t *used)
{
    size_t begin = 0U;
    size_t columns = 0U;

    if (layout == NULL || text == NULL || output == NULL || used == NULL)
        return -1;
    if (markdown_table_separator(text, length))
        return render_markdown_rule(layout, row, output, used);
    if (begin_viewer_row(layout, row, output, used) == -1) return -1;
    for (size_t offset = 0U; offset <= length &&
         columns < layout->content_columns; offset++) {
        if (offset < length && text[offset] != '|') continue;
        if (offset > begin) {
            size_t shown = offset - begin;
            if (shown > layout->content_columns - columns)
                shown = layout->content_columns - columns;
            if (render_markdown_inline(text + begin, offset - begin, shown,
                                       output, used) == -1) return -1;
            columns += shown;
        }
        if (offset < length && columns < layout->content_columns) {
            static const char separator[] = "\033[38;5;240m│\033[0m";
            if (render_append(output, used, separator,
                              sizeof(separator) - 1U) == -1) return -1;
            columns++;
        }
        begin = offset + 1U;
    }
    return end_viewer_row(layout, false, output, used);
}

static int render_markdown_formula(const viewer_layout *layout, size_t row,
                                   const char *text, size_t length,
                                   char output[VIEW_RENDER_CAP], size_t *used)
{
    size_t begin;
    size_t formula_length;
    size_t rendered_columns = 0U;

    if (layout == NULL || text == NULL || output == NULL || used == NULL ||
        !markdown_display_formula(text, length, &begin, &formula_length))
        return -1;
    if (begin_viewer_row(layout, row, output, used) == -1 ||
        render_append(output, used, "\033[38;5;177m  ", 13U) == -1 ||
        render_markdown_math(text + begin, formula_length,
            layout->content_columns > 2U ? layout->content_columns - 2U : 1U,
            output, used, &rendered_columns) == -1 ||
        render_append(output, used, "\033[0m", 4U) == -1 ||
        end_viewer_row(layout, false, output, used) == -1) return -1;
    return 0;
}

static int render_markdown_line(const viewer_layout *layout, size_t row,
                                const char *text, size_t length,
                                char output[VIEW_RENDER_CAP], size_t *used)
{
    size_t offset = 0U;
    const char *style = NULL;
    const char *prefix = "";
    size_t prefix_length = 0U;

    if (layout == NULL || text == NULL || output == NULL || used == NULL)
        return -1;
    while (offset < length && text[offset] == '#') offset++;
    if (offset > 0U && offset <= 6U && offset < length && text[offset] == ' ') {
        style = offset == 1U ? "\033[1;38;5;81m" : "\033[1;38;5;75m";
        offset++;
    } else {
        offset = 0U;
        if (length >= 2U && text[0] == '>' && text[1] == ' ') {
            prefix = "\033[38;5;75m│\033[0m ";
            prefix_length = 2U;
            offset = 2U;
        } else if (length >= 2U &&
                   (text[0] == '-' || text[0] == '*' || text[0] == '+') &&
                   text[1] == ' ') {
            prefix = "\033[38;5;81m•\033[0m ";
            prefix_length = 2U;
            offset = 2U;
        }
    }
    if (begin_viewer_row(layout, row, output, used) == -1 ||
        (prefix_length > 0U &&
         render_append(output, used, prefix, strlen(prefix)) == -1) ||
        (style != NULL &&
         render_append(output, used, style, strlen(style)) == -1) ||
        render_markdown_inline(text + offset, length - offset,
            layout->content_columns > prefix_length
                ? layout->content_columns - prefix_length : 1U,
            output, used) == -1 ||
        (style != NULL && render_append(output, used, "\033[0m", 4U) == -1) ||
        end_viewer_row(layout, false, output, used) == -1) return -1;
    return 0;
}

static int render_markdown_rich_line(const viewer_layout *layout, size_t row,
                                     const char *text, size_t length,
                                     char output[VIEW_RENDER_CAP],
                                     size_t *used)
{
    size_t formula_begin;
    size_t formula_length;

    if (layout == NULL || text == NULL || output == NULL || used == NULL)
        return -1;
    if (markdown_display_formula(text, length, &formula_begin,
                                 &formula_length))
        return render_markdown_formula(layout, row, text, length, output,
                                       used);
    if (markdown_table_line(text, length))
        return render_markdown_table(layout, row, text, length, output, used);
    if (strcmp(text, "---") == 0 || strcmp(text, "***") == 0 ||
        strcmp(text, "___") == 0)
        return render_markdown_rule(layout, row, output, used);
    return render_markdown_line(layout, row, text, length, output, used);
}

/* ── Markdown Preview Keeps Source Positions Stable ──────────────
 * Editing must reopen the source line even though Markdown markers are not
 * displayed.  The renderer therefore walks source lines in order and never
 * creates a second document model.  Fenced blocks carry one bounded lexical
 * state, while an image consumes visual rows but still owns one source line.
 * This preserves search and editor anchors across reloads and resize.
 * ─────────────────────────────────────────────────────────────── */
static int render_markdown_page(viewer_state *state, const char *path,
                                const viewer_layout *layout,
                                char output[VIEW_RENDER_CAP], size_t *used)
{
    char text[VIEW_LINE_CAP];
    viewer_layout content_layout;
    view_language code_language;
    bool fenced;
    size_t source;
    size_t row = 0U;
    size_t content_rows;

    if (state == NULL || path == NULL || layout == NULL || output == NULL ||
        used == NULL)
        return -1;
    content_layout = *layout;
    if (content_layout.content_columns > 2U) {
        content_layout.content_column += 2U;
        content_layout.content_columns -= 2U;
        content_layout.selection_gutter = true;
    }
    content_layout.selection_row = SIZE_MAX;
    content_rows = layout->rows > 3U ? layout->rows - 3U : 1U;
    source = state->top;
    fenced = markdown_fence_before(state, source, &code_language);
    while (row < content_rows && source < state->lines) {
        size_t length = 0U;
        view_language marker_language = VIEW_PLAIN;
        char image_label[256];
        char image_target[PATH_MAX];

        if (read_line(state, source, text, &length) == -1) return -1;
        content_layout.selection_row = source == state->selected
                                           ? row : SIZE_MAX;
        if (markdown_fence_marker(text, length, &marker_language)) {
            const char *edge = fenced ? "└─" : "┌─";
            if (begin_viewer_row(&content_layout, row, output, used) == -1 ||
                render_append(output, used, "\033[38;5;240m", 11U) == -1 ||
                render_append(output, used, edge, strlen(edge)) == -1 ||
                (!fenced && marker_language != VIEW_PLAIN &&
                 render_append(output, used, " code", 5U) == -1) ||
                render_append(output, used, "\033[0m", 4U) == -1 ||
                end_viewer_row(&content_layout, false, output, used) == -1)
                return -1;
            fenced = !fenced;
            code_language = fenced ? marker_language : VIEW_PLAIN;
            row++;
        } else if (fenced) {
            if (begin_viewer_row(&content_layout, row, output, used) == -1 ||
                render_append(output, used, "\033[38;5;240m│\033[0m ",
                              strlen("\033[38;5;240m│\033[0m ")) == -1 ||
                highlight_line(code_language, text, length, 0U,
                    content_layout.content_columns > 2U
                        ? content_layout.content_columns - 2U : 1U,
                    output, used) == -1 ||
                end_viewer_row(&content_layout, false, output, used) == -1)
                return -1;
            row++;
        } else if (markdown_image_label(text, length, image_label,
                                        image_target)) {
            size_t height = markdown_image_height(
                path, image_target, content_layout.content_columns);
            if (height > content_rows - row) height = content_rows - row;
            if (render_placeholder(&content_layout, row, height, image_label,
                                   output, used) == -1) return -1;
            prepare_markdown_frame(state, path, image_target,
                                   &content_layout, row, height);
            row += height < 4U ? 1U : height;
        } else {
            if (render_markdown_rich_line(&content_layout, row, text, length,
                                          output, used) == -1) return -1;
            row++;
        }
        source++;
    }
    content_layout.selection_row = SIZE_MAX;
    while (row < content_rows) {
        if (begin_viewer_row(&content_layout, row, output, used) == -1 ||
            render_append(output, used, "~", 1U) == -1 ||
            end_viewer_row(&content_layout, false, output, used) == -1)
            return -1;
        row++;
    }
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

static bool viewer_terminal_feature_present(const char *features,
                                            const char *wanted)
{
    size_t wanted_length;
    size_t length;

    if (features == NULL || wanted == NULL) return false;
    wanted_length = strlen(wanted);
    length = strnlen(features, 256U);
    if (wanted_length == 0U || length == 256U) return false;
    for (size_t offset = 0U; offset + wanted_length <= length; offset++) {
        bool before = offset == 0U || features[offset - 1U] == ',' ||
                      features[offset - 1U] == ';' ||
                      features[offset - 1U] == ':' ||
                      features[offset - 1U] == ' ';
        size_t after_offset = offset + wanted_length;
        bool after = after_offset == length || features[after_offset] == ',' ||
                     features[after_offset] == ';' ||
                     features[after_offset] == ':' ||
                     features[after_offset] == ' ';

        if (before && after &&
            memcmp(features + offset, wanted, wanted_length) == 0)
            return true;
    }
    return false;
}

static bool viewer_png_protocol_available(const viewer_state *state)
{
    const char *features;
    const char *program;
    bool local;

    if (state == NULL || state->images_mode == GSH_TERMINAL_IMAGES_OFF)
        return false;
    if (state->image_protocol == GSH_RESOURCE_IMAGE_KITTY ||
        state->image_protocol == GSH_RESOURCE_IMAGE_ITERM) return true;
    features = getenv("TERM_FEATURES");
    if (viewer_terminal_feature_present(features, "K") ||
        viewer_terminal_feature_present(features, "F"))
        return true;
    local = getenv("SSH_CONNECTION") == NULL && getenv("TMUX") == NULL &&
            getenv("STY") == NULL;
    program = getenv("TERM_PROGRAM");
    return local && program != NULL &&
           ((strcmp(program, "iTerm.app") == 0 &&
             getenv("ITERM_SESSION_ID") != NULL) ||
            (strcmp(program, "kitty") == 0 &&
             getenv("KITTY_WINDOW_ID") != NULL) ||
            (strcmp(program, "ghostty") == 0 &&
             getenv("TERM_PROGRAM_VERSION") != NULL));
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

static void prepare_render_frame(viewer_state *state)
{
    if (state == NULL) return;
    delete_sent_frame(state);
    if (state->frame_temporary) (void)unlink(state->frame_path);
    state->frame_temporary = false;
    state->frame_pending = false;
    state->frame_path[0] = '\0';
    state->frame_generation++;
    if (state->frame_generation == 0U) state->frame_generation = 1U;
    state->frame_id = state->frame_generation;
}

static int finish_render_frame(viewer_state *state, const char *output,
                               size_t length)
{
    if (state == NULL || output == NULL) return -1;
    if (viewer_write(output, length) == -1) return -1;
    send_pending_frame(state);
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
    prepare_render_frame(state);
    prepare_viewer_layout(state, &layout);
    content_rows = layout.rows > 3U ? layout.rows - 3U : 1U;
    if (!layout.split &&
        render_append(output, &used, "\033[H", 3U) == -1) return -1;
    if (state->language == VIEW_MARKDOWN) {
        if (render_markdown_page(state, path, &layout, output, &used) == -1)
            return -1;
    } else if (state->binary) {
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
            render_status_row(&layout, content_rows + 2U,
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
    return finish_render_frame(state, output, used);
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
    delete_sent_frame(state);
    if (state->frame_temporary) (void)unlink(state->frame_path);
    state->frame_temporary = false;
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

static int handle_text_key(viewer_state *state, const char *path,
                           unsigned char byte, size_t rows)
{
    if (state == NULL || path == NULL) return -1;
    if (byte == 'j') move_vertical(state, 1);
    else if (byte == 'k') move_vertical(state, -1);
    else if (byte == 0x0eU) move_vertical(state, 3);
    else if (byte == 0x10U) move_vertical(state, -3);
    else if (byte == 'n')
        (void)search_from(state, state->selected + 1U, false);
    else if (byte == 'N')
        (void)search_from(state, state->selected == 0U
            ? state->lines - 1U : state->selected - 1U, true);
    else if (byte == '/') return read_search(state, path) == -1 ? -1 : 1;
    else if (byte == 'r') return reload_viewer(state, path);
    else if (byte == 'e')
        return edit_from_viewer(state, path) == -1 ? -1 : 1;
    else if (byte == 0x06U || byte == ' ')
        move_vertical(state, (long)(rows > 3U ? rows - 3U : 1U));
    else if (byte == 0x02U)
        move_vertical(state, -(long)(rows > 3U ? rows - 3U : 1U));
    return 0;
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
        } else {
            int action = handle_text_key(state, path, byte, prior_rows);
            if (action < 0) return 1;
            if (action > 0) continue;
        }
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
    gsh_shell_config config;
    const char *home;

    if (path == NULL || io == NULL || line == 0U || column == 0U) return 1;
    (void)memset(&state, 0, sizeof(state));
    state.file = -1;
    state.index = -1;
    gsh_config_defaults(&config);
    home = getenv("HOME");
    if (home != NULL) (void)gsh_config_load(&config, home, false);
    state.images_mode = config.terminal_images;
    resources = io->resources;
    state.resource_descriptor = resources == NULL ? -1 : resources->descriptor;
    state.image_protocol = resources == NULL
                               ? GSH_RESOURCE_IMAGE_NONE
                               : resources->image_protocol;
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
