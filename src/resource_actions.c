#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "resource_actions.h"

#include <string.h>
#include <wchar.h>

enum { RESOURCE_WORD_CAP = 256 };

typedef enum {
    ADAPTER_NONE,
    ADAPTER_LS_WORDS,
    ADAPTER_LS_WHOLE,
    ADAPTER_LINE,
    ADAPTER_WHOLE,
    ADAPTER_GIT,
    ADAPTER_TREE,
} adapter_kind;

static bool boundary_byte(unsigned char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static size_t visual_width(const char *text, size_t length)
{
    mbstate_t state;
    size_t offset = 0U;
    size_t width = 0U;
    if (text == NULL) return 0U;
    (void)memset(&state, 0, sizeof(state));
    while (offset < length) {
        wchar_t character;
        size_t bytes = mbrtowc(&character, text + offset,
                               length - offset, &state);
        int columns;
        if (bytes == (size_t)-1 || bytes == (size_t)-2 || bytes == 0U) {
            width++;
            offset++;
            (void)memset(&state, 0, sizeof(state));
            continue;
        }
        columns = wcwidth(character);
        width += columns < 0 ? 1U : (size_t)columns;
        offset += bytes;
    }
    return width;
}

static size_t command_word(const char *command, size_t wanted,
                           char output[RESOURCE_WORD_CAP])
{
    size_t cursor = 0U;
    size_t word = 0U;
    if (command == NULL || output == NULL) return 0U;
    while (command[cursor] != '\0' && word <= wanted) {
        size_t used = 0U;
        char quote = '\0';
        while (boundary_byte((unsigned char)command[cursor])) cursor++;
        while (command[cursor] != '\0' &&
               (quote != '\0' || !boundary_byte((unsigned char)command[cursor]))) {
            char byte = command[cursor++];
            if (quote == '\0' && (byte == '\'' || byte == '"')) { quote = byte; continue; }
            if (quote != '\0' && byte == quote) { quote = '\0'; continue; }
            if (byte == '\\' && command[cursor] != '\0' && quote != '\'') byte = command[cursor++];
            if (word == wanted && used + 1U < RESOURCE_WORD_CAP) output[used++] = byte;
        }
        if (word == wanted) { output[used] = '\0'; return used; }
        word++;
    }
    output[0] = '\0';
    return 0U;
}

static adapter_kind ls_adapter(const char *command)
{
    char word[RESOURCE_WORD_CAP];
    size_t argument;
    bool long_format = false;
    bool one_per_line = false;
    if (command == NULL) return ADAPTER_NONE;
    for (argument = 1U; argument < 32U; argument++) {
        size_t length = command_word(command, argument, word);
        size_t option;
        if (length == 0U || strcmp(word, "--") == 0 || word[0] != '-') break;
        if (word[1] == '-') continue;
        for (option = 1U; option < length; option++) {
            if (word[option] == 'C' || word[option] == 'm' ||
                word[option] == 'x') {
                long_format = false;
                one_per_line = false;
            } else if (word[option] == '1' && !long_format) {
                one_per_line = true;
            } else if (word[option] == 'g' || word[option] == 'l' ||
                       word[option] == 'n' || word[option] == 'o') {
                long_format = true;
                one_per_line = false;
            }
        }
    }
    if (long_format) return ADAPTER_NONE;
    return one_per_line ? ADAPTER_LS_WHOLE : ADAPTER_LS_WORDS;
}

static adapter_kind command_adapter(const char *command)
{
    char word[RESOURCE_WORD_CAP];
    char second[RESOURCE_WORD_CAP];
    const char *base;
    if (command_word(command, 0U, word) == 0U) return ADAPTER_NONE;
    base = strrchr(word, '/');
    base = base == NULL ? word : base + 1U;
    if (strcmp(base, "ls") == 0 && strchr(word, '/') != NULL)
        return ls_adapter(command);
    if (strcmp(base, "find") == 0 || strcmp(base, "fd") == 0) return ADAPTER_WHOLE;
    if (strcmp(base, "tree") == 0) return ADAPTER_TREE;
    if (strcmp(base, "grep") == 0 || strcmp(base, "rg") == 0) {
        return strstr(command, "--files") != NULL ? ADAPTER_WHOLE : ADAPTER_LINE;
    }
    if (strcmp(base, "git") == 0 &&
        command_word(command, 1U, second) != 0U &&
        strcmp(second, "status") == 0) return ADAPTER_GIT;
    return ADAPTER_NONE;
}

static bool digits_only(const char *text, size_t begin, size_t end,
                        size_t *value)
{
    size_t parsed = 0U;
    size_t index;
    if (text == NULL || value == NULL || begin == end) return false;
    for (index = begin; index < end; index++) {
        if (text[index] < '0' || text[index] > '9' ||
            parsed > (SIZE_MAX - (size_t)(text[index] - '0')) / 10U) return false;
        parsed = parsed * 10U + (size_t)(text[index] - '0');
    }
    *value = parsed;
    return parsed != 0U;
}

static void parse_location(const char *text, size_t *end,
                           size_t *line, size_t *column)
{
    size_t split;
    size_t value;
    size_t last = 0U;
    size_t previous = 0U;
    size_t found = 0U;
    if (text == NULL || end == NULL || line == NULL || column == NULL) return;
    split = *end;
    while (split > 0U && found < 2U) {
        size_t digit_end = split;
        while (split > 0U && text[split - 1U] >= '0' && text[split - 1U] <= '9') split--;
        if (split == digit_end || split == 0U || text[split - 1U] != ':' ||
            !digits_only(text, split, digit_end, &value)) break;
        if (found == 0U) last = value; else previous = value;
        found++;
        split--;
    }
    if (found != 0U && split != 0U) {
        *end = split;
        *line = found == 2U ? previous : last;
        *column = found == 2U ? last : 1U;
    }
}

static bool url_like(const char *text, size_t begin, size_t end)
{
    size_t index;
    if (text == NULL || begin >= end) return true;
    for (index = begin; index + 2U < end; index++) {
        if (text[index] == ':' && text[index + 1U] == '/' &&
            text[index + 2U] == '/') return true;
    }
    return false;
}

static void trim_candidate(const char *text, size_t length,
                           size_t *begin, size_t *end)
{
    static const char opening[] = "'\"`([{<";
    static const char closing[] = "'\"`)]}>,;.:";
    if (text == NULL || begin == NULL || end == NULL) return;
    while (*begin < *end && *begin < length &&
           strchr(opening, text[*begin]) != NULL) (*begin)++;
    while (*end > *begin && *end <= length &&
           strchr(closing, text[*end - 1U]) != NULL) (*end)--;
}

static bool path_shape(const char *text, size_t begin, size_t end)
{
    size_t index;
    if (text == NULL || begin >= end || url_like(text, begin, end)) return false;
    if (text[begin] == '/' ||
        (end - begin >= 2U && text[begin] == '.' && text[begin + 1U] == '/') ||
        (end - begin >= 3U && text[begin] == '.' && text[begin + 1U] == '.' && text[begin + 2U] == '/') ||
        (end - begin >= 2U && text[begin] == '~' && text[begin + 1U] == '/')) return true;
    for (index = begin; index < end; index++) if (text[index] == '/') return true;
    return false;
}

static int add_candidate(gsh_resource_candidate *candidates,
                         size_t capacity, size_t *count, const char *text,
                         size_t begin, size_t end,
                         gsh_resource_provenance provenance,
                         gsh_resource_type type, bool bare,
                         bool navigable_root)
{
    gsh_resource_candidate *candidate;
    size_t path_end;
    size_t separator;
    size_t length;
    if (candidates == NULL || count == NULL || text == NULL || *count >= capacity) return -1;
    trim_candidate(text, end, &begin, &end);
    for (separator = begin; separator < end; separator++) {
        if (text[separator] == '=' &&
            path_shape(text, separator + 1U, end)) begin = separator + 1U;
    }
    path_end = end;
    candidates[*count].line = 0U;
    candidates[*count].column = 0U;
    parse_location(text, &path_end, &candidates[*count].line,
                   &candidates[*count].column);
    if (path_end < end) end = path_end;
    trim_candidate(text, end, &begin, &end);
    if (begin >= end || (!bare && !path_shape(text, begin, end))) return 0;
    length = end - begin;
    if (length >= sizeof(candidates[*count].path)) return 0;
    candidate = &candidates[(*count)++];
    candidate->begin = begin;
    candidate->end = end;
    candidate->column_begin = visual_width(text, begin);
    candidate->column_end = candidate->column_begin +
                            visual_width(text + begin, length);
    if (candidate->column_end == candidate->column_begin)
        candidate->column_end++;
    (void)memcpy(candidate->path, text + begin, length);
    candidate->path[length] = '\0';
    candidate->provenance = provenance;
    candidate->type = type;
    candidate->navigable_root = navigable_root;
    return 1;
}

static size_t detect_words(const char *text, size_t length,
                           gsh_resource_candidate *candidates,
                           size_t capacity, gsh_resource_provenance provenance,
                           bool bare, bool navigable_root)
{
    if (text == NULL || candidates == NULL) return 0U;
    size_t cursor = 0U;
    size_t count = 0U;
    while (cursor < length && count < capacity) {
        size_t begin;
        char quote = '\0';
        while (cursor < length && boundary_byte((unsigned char)text[cursor])) cursor++;
        begin = cursor;
        if (cursor < length &&
            (text[cursor] == '\'' || text[cursor] == '"' ||
             text[cursor] == '`')) quote = text[cursor++];
        while (cursor < length &&
               (quote != '\0' ||
                !boundary_byte((unsigned char)text[cursor]))) {
            if (quote != '\0' && text[cursor] == quote) {
                cursor++;
                break;
            }
            cursor++;
        }
        if (begin < cursor) (void)add_candidate(candidates, capacity, &count,
            text, begin, cursor, provenance, GSH_RESOURCE_UNKNOWN, bare,
            navigable_root);
    }
    return count;
}

static size_t detect_whole(const char *text, size_t length,
                           gsh_resource_candidate *candidates,
                           size_t capacity, gsh_resource_provenance provenance,
                           bool bare)
{
    if (text == NULL || candidates == NULL) return 0U;
    size_t begin = 0U;
    size_t end = length;
    size_t count = 0U;
    while (begin < end && boundary_byte((unsigned char)text[begin])) begin++;
    while (end > begin && boundary_byte((unsigned char)text[end - 1U])) end--;
    (void)add_candidate(candidates, capacity, &count, text, begin, end,
                        provenance, GSH_RESOURCE_UNKNOWN, bare, false);
    return count;
}

static size_t detect_line_format(const char *text, size_t length,
                                 gsh_resource_candidate *candidates,
                                 size_t capacity)
{
    if (text == NULL || candidates == NULL) return 0U;
    size_t colon;
    size_t count = 0U;
    for (colon = 1U; colon < length; colon++) {
        size_t end = colon + 1U;
        size_t value;
        if (text[colon] == ':' && end < length) {
            while (end < length && text[end] >= '0' && text[end] <= '9') end++;
            if (digits_only(text, colon + 1U, end, &value) && end < length &&
                text[end] == ':') {
                size_t location_end = end;
                size_t column_end = end + 1U;
                size_t column;
                while (column_end < length && text[column_end] >= '0' &&
                       text[column_end] <= '9') column_end++;
                if (digits_only(text, end + 1U, column_end, &column) &&
                    column_end < length && text[column_end] == ':') {
                    location_end = column_end;
                }
                (void)add_candidate(candidates, capacity, &count, text, 0U,
                                    location_end, GSH_RESOURCE_ADAPTER,
                                    GSH_RESOURCE_UNKNOWN, true, false);
                break;
            }
        }
    }
    return count;
}

static size_t detect_git(const char *text, size_t length,
                         gsh_resource_candidate *candidates, size_t capacity)
{
    static const char *const labels[] = {
        "modified:", "new file:", "deleted:", "renamed:", "copied:"};
    static const char status[] = " MADRCU?!";
    size_t begin = 0U;
    size_t arrow;
    size_t count = 0U;
    size_t label;
    if (text == NULL || candidates == NULL) return 0U;
    while (begin < length && (text[begin] == ' ' || text[begin] == '\t'))
        begin++;
    if (length >= 3U && strchr(status, text[0]) != NULL &&
        strchr(status, text[1]) != NULL && text[2] == ' ') {
        begin = 3U;
    } else {
        bool matched = false;
        for (label = 0U; label < sizeof(labels) / sizeof(labels[0]); label++) {
            size_t label_length = strlen(labels[label]);
            if (label_length <= length - begin &&
                memcmp(text + begin, labels[label], label_length) == 0) {
                begin += label_length;
                while (begin < length &&
                       (text[begin] == ' ' || text[begin] == '\t')) begin++;
                matched = true;
                break;
            }
        }
        if (!matched) return 0U;
    }
    for (arrow = begin; arrow + 4U <= length; arrow++) {
        if (memcmp(text + arrow, " -> ", 4U) == 0) begin = arrow + 4U;
    }
    (void)add_candidate(candidates, capacity, &count, text, begin, length,
                        GSH_RESOURCE_ADAPTER, GSH_RESOURCE_UNKNOWN, true,
                        false);
    return count;
}

static size_t detect_tree(const char *text, size_t length,
                          gsh_resource_candidate *candidates, size_t capacity)
{
    if (text == NULL || candidates == NULL) return 0U;
    size_t begin = 0U;
    size_t count = 0U;
    while (begin < length) {
        if (text[begin] == ' ' || text[begin] == '|' ||
            text[begin] == '`' || text[begin] == '-' ||
            text[begin] == '+') {
            begin++;
        } else if (begin + 3U <= length &&
                   (unsigned char)text[begin] == 0xe2U &&
                   (unsigned char)text[begin + 1U] == 0x94U) {
            begin += 3U;
        } else break;
    }
    (void)add_candidate(candidates, capacity, &count, text, begin, length,
                        GSH_RESOURCE_ADAPTER, GSH_RESOURCE_UNKNOWN, true,
                        false);
    return count;
}

size_t gsh_resource_detect(const char *command, const char *launch_directory,
                           const char *text, size_t length,
                           gsh_path_detection_mode mode,
                           gsh_resource_candidate *candidates,
                           size_t capacity)
{
    adapter_kind adapter;
    size_t count = 0U;
    (void)launch_directory;
    if (command == NULL || text == NULL || candidates == NULL || capacity == 0U) return 0U;
    (void)memset(candidates, 0, capacity * sizeof(candidates[0]));
    adapter = command_adapter(command);
    if (mode == GSH_PATH_DETECTION_OFF) return 0U;
    if (adapter == ADAPTER_LS_WORDS)
        count = detect_words(text, length, candidates, capacity,
                             GSH_RESOURCE_ADAPTER, true, false);
    else if (adapter == ADAPTER_LS_WHOLE)
        count = detect_whole(text, length, candidates, capacity,
                             GSH_RESOURCE_ADAPTER, true);
    else if (adapter == ADAPTER_WHOLE) count = detect_whole(text, length, candidates, capacity, GSH_RESOURCE_ADAPTER, true);
    else if (adapter == ADAPTER_LINE) count = detect_line_format(text, length, candidates, capacity);
    else if (adapter == ADAPTER_GIT) count = detect_git(text, length, candidates, capacity);
    else if (adapter == ADAPTER_TREE) count = detect_tree(text, length, candidates, capacity);
    if (count == 0U && mode == GSH_PATH_DETECTION_SAFE) {
        count = detect_words(text, length, candidates, capacity,
                             GSH_RESOURCE_DETECTED, false, false);
    }
    return count;
}
