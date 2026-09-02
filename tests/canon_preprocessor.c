#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh policy checks require the POSIX.1-2024 feature-test baseline"
#endif

#include "canon_preprocessor.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define require(condition) (condition)

static bool identifier_start(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') || byte == '_';
}

static bool identifier_continue(unsigned char byte)
{
    return identifier_start(byte) || (byte >= '0' && byte <= '9');
}

static bool span_is(const unsigned char *source, size_t begin, size_t end,
                    const char *text)
{
    size_t length;

    if (!require(source != NULL && text != NULL)) return false;
    if (!require(begin <= end)) return false;
    length = end - begin;
    return strlen(text) == length && memcmp(source + begin, text, length) == 0;
}

static bool header_guard_name(const unsigned char *source, size_t begin,
                              size_t end)
{
    static const char prefix[] = "GSH_";
    size_t length;

    if (!require(source != NULL)) return false;
    if (!require(begin <= end)) return false;
    length = end - begin;
    return length > sizeof(prefix) + 1U &&
           memcmp(source + begin, prefix, sizeof(prefix) - 1U) == 0 &&
           source[end - 2U] == '_' && source[end - 1U] == 'H';
}

static bool condition_name_allowed(const unsigned char *source,
                                   size_t begin, size_t end)
{
    static const char *const names[] = {
        "GSH_FUZZ_STANDALONE", "RLIMIT_AS", "RLIMIT_CPU",
        "RLIMIT_NPROC", "SIGABRT", "SIGALRM", "SIGBUS", "SIGCHLD",
        "SIGCONT", "SIGFPE", "SIGHUP", "SIGILL", "SIGINT", "SIGKILL",
        "SIGPIPE", "SIGPOLL", "SIGPROF", "SIGQUIT", "SIGSEGV",
        "SIGSTOP", "SIGSYS", "SIGTERM", "SIGTRAP", "SIGTSTP",
        "SIGTTIN", "SIGTTOU", "SIGURG", "SIGUSR1", "SIGUSR2",
        "SIGVTALRM", "SIGWINCH", "SIGXCPU", "SIGXFSZ", "TIOCSCTTY",
        "TIOCSIG", "VDSUSP", "WCONTINUED", "WIFCONTINUED",
        "_POSIX_C_SOURCE", "_XOPEN_SOURCE", "__APPLE__", "__GNUC__",
        "__clang__", "__linux__", "defined",
    };
    size_t index;

    if (!require(source != NULL && begin < end)) return false;
    if (!require(end - begin < 128U)) return false;
    if (header_guard_name(source, begin, end)) return true;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); index++) {
        if (span_is(source, begin, end, names[index])) return true;
    }
    return false;
}

static size_t skip_horizontal(const unsigned char *source, size_t cursor,
                              size_t end)
{
    if (!require(source != NULL)) return end;
    if (!require(cursor <= end)) return end;
    while (cursor < end &&
           (source[cursor] == ' ' || source[cursor] == '\t' ||
            source[cursor] == '\r' || source[cursor] == '\f' ||
            source[cursor] == '\v')) {
        cursor++;
    }
    return cursor;
}

static size_t logical_line_end(const unsigned char *source, size_t length,
                               size_t begin)
{
    size_t cursor = begin;

    if (!require(source != NULL && begin <= length)) return length;
    while (cursor < length) {
        size_t line_end = cursor;
        size_t tail;

        while (line_end < length && source[line_end] != '\n') line_end++;
        tail = line_end;
        while (tail > cursor &&
               (source[tail - 1U] == ' ' || source[tail - 1U] == '\t' ||
                source[tail - 1U] == '\r')) {
            tail--;
        }
        if (tail == cursor || source[tail - 1U] != '\\' ||
            line_end == length) return line_end;
        cursor = line_end + 1U;
    }
    return length;
}

static size_t identifier_end(const unsigned char *source, size_t cursor,
                             size_t end)
{
    if (!require(source != NULL)) return cursor;
    if (!require(cursor <= end)) return cursor;
    while (cursor < end && identifier_continue(source[cursor])) cursor++;
    return cursor;
}

static bool condition_is_closed(const unsigned char *source, size_t cursor,
                                size_t end)
{
    bool have_identifier = false;

    if (!require(source != NULL)) return false;
    if (!require(cursor <= end)) return false;
    while (cursor < end) {
        if (identifier_start(source[cursor])) {
            size_t finish = identifier_end(source, cursor, end);

            have_identifier = true;
            if (cursor > 0U && source[cursor - 1U] >= '0' &&
                source[cursor - 1U] <= '9') {
                cursor = finish;
                continue;
            }
            if (!condition_name_allowed(source, cursor, finish)) return false;
            cursor = finish;
        } else {
            cursor++;
        }
    }
    return have_identifier;
}

static size_t macro_replacement_begin(const unsigned char *source,
                                      size_t cursor, size_t end)
{
    size_t depth = 0U;

    if (!require(source != NULL && cursor <= end)) return end;
    if (cursor >= end || source[cursor] != '(') {
        return skip_horizontal(source, cursor, end);
    }
    while (cursor < end) {
        if (source[cursor] == '(') {
            depth++;
        } else if (source[cursor] == ')') {
            if (depth == 0U) return end;
            depth--;
            if (depth == 0U) {
                return skip_horizontal(source, cursor + 1U, end);
            }
        }
        cursor++;
    }
    return end;
}

static bool macro_replacement_valid(const unsigned char *source,
                                    size_t begin, size_t end,
                                    size_t name_begin, size_t name_end)
{
    size_t cursor = begin;
    size_t first = end;
    size_t last = end;
    int parentheses = 0;
    int brackets = 0;
    int braces = 0;

    if (!require(source != NULL && begin <= end)) return false;
    if (!require(name_begin < name_end && name_end <= begin)) return false;
    while (cursor < end) {
        unsigned char byte = source[cursor];

        if (byte == '\\' && cursor + 1U < end &&
            source[cursor + 1U] == '\n') {
            cursor += 2U;
            continue;
        }
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' ||
            byte == '\f' || byte == '\v') {
            cursor++;
            continue;
        }
        if (first == end) first = cursor;
        last = cursor;
        if (byte == '#' ||
            (byte == '.' && cursor + 2U < end &&
             source[cursor + 1U] == '.' && source[cursor + 2U] == '.')) {
            return false;
        }
        if (identifier_start(byte)) {
            size_t finish = identifier_end(source, cursor, end);

            if (finish - cursor == name_end - name_begin &&
                memcmp(source + cursor, source + name_begin,
                       finish - cursor) == 0) return false;
            cursor = finish;
            continue;
        }
        if (byte == '(') parentheses++;
        if (byte == ')') parentheses--;
        if (byte == '[') brackets++;
        if (byte == ']') brackets--;
        if (byte == '{') braces++;
        if (byte == '}') braces--;
        if (parentheses < 0 || brackets < 0 || braces < 0) return false;
        cursor++;
    }
    if (first == end) return true;
    return source[first] != ',' && source[last] != ',' &&
           parentheses == 0 && brackets == 0 && braces == 0;
}

static bool define_is_valid(const unsigned char *source, size_t cursor,
                            size_t end)
{
    size_t name_begin;
    size_t name_end;
    size_t replacement;

    if (!require(source != NULL && cursor <= end)) return false;
    cursor = skip_horizontal(source, cursor, end);
    if (cursor == end || !identifier_start(source[cursor])) return false;
    name_begin = cursor;
    name_end = identifier_end(source, cursor, end);
    replacement = macro_replacement_begin(source, name_end, end);
    for (cursor = name_end; cursor < replacement; cursor++) {
        if (source[cursor] == '#' ||
            (source[cursor] == '.' && cursor + 2U < replacement &&
             source[cursor + 1U] == '.' &&
             source[cursor + 2U] == '.')) return false;
    }
    if (replacement == end) return header_guard_name(source, name_begin,
                                                     name_end) ||
                                   span_is(source, name_begin, name_end,
                                           "_DARWIN_C_SOURCE") ||
                                   span_is(source, name_begin, name_end,
                                           "_GNU_SOURCE");
    return macro_replacement_valid(source, replacement, end, name_begin,
                                   name_end);
}

static bool directive_is_valid(const unsigned char *source, size_t begin,
                               size_t end)
{
    if (source == NULL) {
        return false;
    }
    size_t cursor = skip_horizontal(source, begin, end);
    size_t directive_begin;
    size_t directive_end;

    if (!require(source != NULL && begin <= end)) return false;
    if (cursor == end || source[cursor] != '#') return true;
    cursor = skip_horizontal(source, cursor + 1U, end);
    directive_begin = cursor;
    directive_end = identifier_end(source, cursor, end);
    cursor = skip_horizontal(source, directive_end, end);
    if (span_is(source, directive_begin, directive_end, "define")) {
        return define_is_valid(source, cursor, end);
    }
    if (span_is(source, directive_begin, directive_end, "if") ||
        span_is(source, directive_begin, directive_end, "ifdef") ||
        span_is(source, directive_begin, directive_end, "ifndef") ||
        span_is(source, directive_begin, directive_end, "elif")) {
        return condition_is_closed(source, cursor, end);
    }
    return true;
}

int gsh_canon_preprocessor_analyze(const unsigned char *source,
                                   size_t length, const char *path,
                                   size_t *violations, bool report)
{
    size_t cursor = 0U;

    if (source == NULL || path == NULL || violations == NULL) {
        errno = EINVAL;
        return -1;
    }
    *violations = 0U;
    while (cursor < length) {
        size_t begin = cursor;
        size_t end = logical_line_end(source, length, begin);

        if (!directive_is_valid(source, begin, end)) {
            (*violations)++;
            if (report) {
                (void)fprintf(stderr,
                        "source policy: preprocessor violation: %s at byte "
                        "%zu\n",
                        path, begin);
            }
        }
        cursor = end < length ? end + 1U : length;
    }
    return 0;
}
