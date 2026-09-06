#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "completion.h"

#include "builtin_registry.h"

#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#define require(condition) (condition)

enum {
    COMPLETION_ENTRY_CAP = 8192,
    COMPLETION_PATH_CAP = 32768,
    COMPLETION_PATH_COMPONENT_CAP = 1024,
    COMPLETION_CANDIDATE_CAP = 192,
    COMPLETION_CONTEXT_NAME_CAP = 128,
};

typedef enum {
    COMPLETION_PATH_ANY = 0,
    COMPLETION_PATH_DIRECTORY,
    COMPLETION_PATH_EXECUTABLE,
} completion_path_mode;

typedef struct {
    size_t begin;
    size_t prefix_length;
    size_t argument_index;
    bool command;
    bool at_end;
    bool variable;
    bool braced_variable;
    bool closed_variable;
    char prefix[GSH_COMPLETION_TEXT_CAP];
    char command_name[COMPLETION_CONTEXT_NAME_CAP];
} completion_word;

typedef struct {
    size_t common_length;
    size_t first_length;
    size_t entries;
    bool matched;
    bool distinct;
    bool limited;
    bool candidate_limited;
    uint16_t candidate_offsets[COMPLETION_CANDIDATE_CAP];
    uint16_t candidate_lengths[COMPLETION_CANDIDATE_CAP];
    uint8_t candidate_directories[COMPLETION_CANDIDATE_CAP];
    size_t candidate_count;
    size_t candidate_text_used;
    char common[GSH_COMPLETION_TEXT_CAP];
    char first[GSH_COMPLETION_TEXT_CAP];
    char candidate_text[GSH_COMPLETION_TEXT_CAP];
} completion_matches;

typedef struct {
    size_t begin;
    size_t arguments;
    bool expect_command;
    bool in_word;
    bool escaped;
    char command_name[COMPLETION_CONTEXT_NAME_CAP];
} completion_context;

static bool word_boundary(unsigned char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n';
}

static bool command_boundary(unsigned char byte)
{
    return byte == ';' || byte == '|' || byte == '&' || byte == '(' ||
           byte == ')';
}

static bool variable_name_byte(unsigned char byte, bool first)
{
    if ((byte >= 'a' && byte <= 'z') ||
        (byte >= 'A' && byte <= 'Z') || byte == '_') return true;
    return !first && byte >= '0' && byte <= '9';
}

static bool assignment_word(const char *line, size_t begin, size_t end)
{
    size_t index;

    if (!require(line != NULL)) return false;
    if (!require(begin < end)) return false;
    for (index = begin; index < end; index++) {
        unsigned char byte = (unsigned char)line[index];

        if (byte == '=') return index > begin;
        if (!variable_name_byte(byte, index == begin)) return false;
    }
    return false;
}

static void finish_scanned_word(const char *line, size_t begin, size_t end,
                                bool *expect_command)
{
    if (!require(line != NULL && expect_command != NULL)) return;
    if (!require(begin <= end)) return;
    if (*expect_command && begin < end && !assignment_word(line, begin, end)) {
        *expect_command = false;
    }
}

/* ── One Bounded Scan Finds the Completion Context ─────────────
 * Completion originally had no lexical boundary and Tab became input text.
 * The editor now scans only the bytes before its cursor, retaining whether the
 * current word is a command and accepting backslash escapes without expansion.
 * Quotes and substitutions are left unchanged because completing them would
 * require the shell expansion engine and could silently alter their meaning.
 * Fixed line and component limits keep this classification deterministic.
 * ─────────────────────────────────────────────────────────────── */
static bool locate_completion_word(const char *line, size_t length,
                                   size_t cursor, completion_word *word)
{
    size_t begin = cursor;
    size_t index;
    bool command = true;
    bool in_word = false;
    bool escaped = false;
    bool unsafe = false;

    if (!require(line != NULL && word != NULL)) return false;
    if (!require(cursor <= length && length < GSH_COMPLETION_TEXT_CAP))
        return false;
    (void)memset(word, 0, sizeof(*word));
    for (index = 0U; index < cursor; index++) {
        unsigned char byte = (unsigned char)line[index];

        if (escaped) { escaped = false; continue; }
        if (byte == '\\' && in_word) { escaped = true; continue; }
        if (word_boundary(byte) || command_boundary(byte) ||
            byte == '<' || byte == '>') {
            if (in_word) finish_scanned_word(line, begin, index, &command);
            in_word = false;
            unsafe = false;
            if (command_boundary(byte)) command = true;
            continue;
        }
        if (!in_word) { begin = index; in_word = true; }
        if (byte == '\'' || byte == '"' || byte == '$' || byte == '`')
            unsafe = true;
    }
    if (!in_word) begin = cursor;
    if (escaped || unsafe) return false;
    word->begin = begin;
    word->command = command;
    word->at_end = cursor == length || word_boundary((unsigned char)line[cursor]) ||
                   command_boundary((unsigned char)line[cursor]) ||
                   line[cursor] == '<' || line[cursor] == '>';
    return true;
}

static bool escaped_at(const char *line, size_t offset)
{
    size_t slashes = 0U;

    if (!require(line != NULL)) return true;
    while (offset > slashes && line[offset - slashes - 1U] == '\\')
        slashes++;
    return slashes % 2U != 0U;
}

static bool locate_variable_word(const char *line, size_t length,
                                 size_t cursor, completion_word *word)
{
    size_t name_begin = cursor;
    size_t begin;
    bool braced = false;

    if (!require(line != NULL && word != NULL)) return false;
    if (!require(cursor <= length && length < GSH_COMPLETION_TEXT_CAP))
        return false;
    while (name_begin > 0U && variable_name_byte(
               (unsigned char)line[name_begin - 1U], false)) name_begin--;
    if (name_begin > 0U && line[name_begin - 1U] == '$') {
        begin = name_begin - 1U;
    } else if (name_begin > 1U && line[name_begin - 2U] == '$' &&
               line[name_begin - 1U] == '{') {
        begin = name_begin - 2U;
        braced = true;
    } else return false;
    if (escaped_at(line, begin) || cursor - begin >= sizeof(word->prefix))
        return false;
    (void)memset(word, 0, sizeof(*word));
    word->begin = begin;
    word->prefix_length = cursor - begin;
    word->variable = true;
    word->braced_variable = braced;
    word->closed_variable = braced && cursor < length && line[cursor] == '}';
    word->at_end = cursor == length ||
                   (braced && (unsigned char)line[cursor] == '}');
    (void)memcpy(word->prefix, line + begin, word->prefix_length);
    word->prefix[word->prefix_length] = '\0';
    return true;
}

static bool decode_prefix(const char *line, size_t cursor,
                          completion_word *word)
{
    size_t input;
    size_t output = 0U;

    if (!require(line != NULL && word != NULL)) return false;
    if (!require(word->begin <= cursor)) return false;
    for (input = word->begin; input < cursor; input++) {
        unsigned char byte = (unsigned char)line[input];

        if (byte == '\\') {
            if (++input >= cursor) return false;
            byte = (unsigned char)line[input];
        }
        if (output + 1U >= sizeof(word->prefix)) return false;
        word->prefix[output++] = (char)byte;
    }
    word->prefix[output] = '\0';
    word->prefix_length = output;
    return true;
}

static bool copy_context_word(const char *line, size_t begin, size_t end,
                              char *output, size_t capacity)
{
    size_t input;
    size_t used = 0U;

    if (!require(line != NULL && output != NULL)) return false;
    if (!require(begin <= end && capacity > 0U)) return false;
    for (input = begin; input < end; input++) {
        unsigned char byte = (unsigned char)line[input];

        if (byte == '\\' && input + 1U < end)
            byte = (unsigned char)line[++input];
        else if (byte == '\'' || byte == '"' || byte == '$' || byte == '`')
            return false;
        if (used + 1U >= capacity) return false;
        output[used++] = (char)byte;
    }
    output[used] = '\0';
    return used != 0U;
}

static void finish_context_word(const char *line, size_t end,
                                completion_context *context)
{
    if (!require(line != NULL && context != NULL)) return;
    if (!require(context->in_word && context->begin < end)) return;
    if (context->expect_command &&
        assignment_word(line, context->begin, end)) {
        context->in_word = false;
        return;
    }
    if (context->expect_command) {
        if (copy_context_word(line, context->begin, end,
                              context->command_name,
                              sizeof(context->command_name))) {
            context->expect_command = false;
            context->arguments = 0U;
        }
    } else context->arguments++;
    context->in_word = false;
}

static void scan_completion_context(const char *line, completion_word *word)
{
    completion_context context;
    size_t index;

    if (!require(line != NULL && word != NULL)) return;
    (void)memset(&context, 0, sizeof(context));
    context.expect_command = true;
    for (index = 0U; index < word->begin; index++) {
        unsigned char byte = (unsigned char)line[index];

        if (context.escaped) { context.escaped = false; continue; }
        if (byte == '\\' && context.in_word) {
            context.escaped = true;
            continue;
        }
        if (word_boundary(byte) || command_boundary(byte) ||
            byte == '<' || byte == '>') {
            if (context.in_word) finish_context_word(line, index, &context);
            if (command_boundary(byte)) {
                context.expect_command = true;
                context.arguments = 0U;
                context.command_name[0] = '\0';
            }
            continue;
        }
        if (!context.in_word) { context.begin = index; context.in_word = true; }
    }
    if (context.in_word)
        finish_context_word(line, word->begin, &context);
    word->command = context.expect_command;
    word->argument_index = context.expect_command ? 0U : context.arguments + 1U;
    (void)memcpy(word->command_name, context.command_name,
                 sizeof(word->command_name));
}

static void remember_candidate(completion_matches *matches,
                               const char *candidate, size_t length,
                               bool directory)
{
    size_t index;
    size_t offset;

    if (!require(matches != NULL && candidate != NULL)) return;
    for (index = 0U; index < matches->candidate_count; index++) {
        offset = matches->candidate_offsets[index];
        if (matches->candidate_lengths[index] == length &&
            memcmp(matches->candidate_text + offset, candidate, length) == 0) {
            if (directory) matches->candidate_directories[index] = 1U;
            return;
        }
    }
    if (matches->candidate_count >= COMPLETION_CANDIDATE_CAP ||
        matches->candidate_text_used + length + 1U >
            sizeof(matches->candidate_text)) {
        matches->candidate_limited = true;
        return;
    }
    index = matches->candidate_count++;
    offset = matches->candidate_text_used;
    matches->candidate_offsets[index] = (uint16_t)offset;
    matches->candidate_lengths[index] = (uint16_t)length;
    matches->candidate_directories[index] = directory ? 1U : 0U;
    (void)memcpy(matches->candidate_text + offset, candidate, length);
    matches->candidate_text[offset + length] = '\0';
    matches->candidate_text_used += length + 1U;
}

static void offer_candidate(completion_matches *matches,
                            const completion_word *word,
                            const char *candidate, size_t length,
                            bool directory)
{
    size_t shared;

    if (!require(matches != NULL && word != NULL)) return;
    if (!require(candidate != NULL && length < sizeof(matches->first))) return;
    if (length < word->prefix_length ||
        memcmp(candidate, word->prefix, word->prefix_length) != 0) return;
    remember_candidate(matches, candidate, length, directory);
    if (!matches->matched) {
        (void)memcpy(matches->first, candidate, length + 1U);
        (void)memcpy(matches->common, candidate, length + 1U);
        matches->first_length = length;
        matches->common_length = length;
        matches->matched = true;
        return;
    }
    if (length != matches->first_length ||
        memcmp(candidate, matches->first, length) != 0) matches->distinct = true;
    shared = 0U;
    while (shared < length && shared < matches->common_length &&
           candidate[shared] == matches->common[shared]) shared++;
    matches->common_length = shared;
    matches->common[shared] = '\0';
}

static bool executable_candidate(const char *directory, const char *name)
{
    char path[GSH_COMPLETION_TEXT_CAP];
    struct stat information;
    int length;

    if (!require(directory != NULL && name != NULL)) return false;
    if (!require(directory[0] != '\0' && name[0] != '\0')) return false;
    length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof(path)) return false;
    return stat(path, &information) == 0 && !S_ISDIR(information.st_mode) &&
           access(path, X_OK) == 0;
}

static void scan_command_directory(const char *directory,
                                   const completion_word *word,
                                   completion_matches *matches)
{
    DIR *stream;
    size_t attempts;

    if (!require(directory != NULL && word != NULL)) return;
    if (!require(matches != NULL)) return;
    stream = opendir(directory);
    if (stream == NULL) return;
    for (attempts = 0U; attempts < COMPLETION_ENTRY_CAP; attempts++) {
        struct dirent *entry = readdir(stream);
        size_t length;

        if (entry == NULL) break;
        if (matches->entries == COMPLETION_ENTRY_CAP) {
            matches->limited = true;
            break;
        }
        matches->entries++;
        if (entry->d_name[0] == '.' && word->prefix[0] != '.') continue;
        length = strnlen(entry->d_name, GSH_COMPLETION_TEXT_CAP);
        if (length == GSH_COMPLETION_TEXT_CAP) continue;
        if (executable_candidate(directory, entry->d_name))
            offer_candidate(matches, word, entry->d_name, length, false);
    }
    if (attempts == COMPLETION_ENTRY_CAP) matches->limited = true;
    (void)closedir(stream);
}

static void scan_path_commands(const char *path,
                               const completion_word *word,
                               completion_matches *matches)
{
    size_t offset = 0U;
    size_t component;

    if (!require(path != NULL && word != NULL)) return;
    if (!require(matches != NULL)) return;
    for (component = 0U; component < COMPLETION_PATH_COMPONENT_CAP;
         component++) {
        char directory[GSH_COMPLETION_TEXT_CAP];
        size_t begin = offset;
        size_t length;

        while (offset < COMPLETION_PATH_CAP && path[offset] != '\0' &&
               path[offset] != ':') offset++;
        if (offset == COMPLETION_PATH_CAP) { matches->limited = true; break; }
        length = offset - begin;
        if (length == 0U) (void)memcpy(directory, ".", 2U);
        else if (length >= sizeof(directory)) matches->limited = true;
        else { (void)memcpy(directory, path + begin, length); directory[length] = '\0'; }
        if (!matches->limited) scan_command_directory(directory, word, matches);
        if (path[offset] == '\0') break;
        offset++;
    }
    if (component == COMPLETION_PATH_COMPONENT_CAP) matches->limited = true;
}

static void scan_shell_commands(const completion_word *word,
                                const gsh_alias_store *aliases,
                                const gsh_function_store *functions,
                                completion_matches *matches)
{
    size_t index;
    const char *function_text;

    if (!require(word != NULL && aliases != NULL)) return;
    if (!require(functions != NULL && matches != NULL)) return;
    for (index = 0U; index < gsh_builtin_descriptor_count(); index++) {
        const gsh_builtin_descriptor *entry = gsh_builtin_descriptor_at(index);

        if (entry != NULL && entry->implemented)
            offer_candidate(matches, word, entry->name, entry->name_length,
                            false);
    }
    for (index = 0U; index < gsh_aliases_count(aliases); index++) {
        const char *assignment = gsh_aliases_assignment(aliases, index);

        if (assignment != NULL)
            offer_candidate(matches, word, assignment,
                            aliases->entries[index].name_length, false);
    }
    function_text = gsh_functions_text(functions);
    for (index = 0U; index < gsh_functions_count(functions); index++) {
        const gsh_function_entry *entry = &functions->entries[index];

        if (function_text != NULL)
            offer_candidate(matches, word,
                            function_text + entry->source_offset,
                            entry->name_length, false);
    }
}

static void scan_alias_names(const completion_word *word,
                             const gsh_alias_store *aliases,
                             completion_matches *matches)
{
    size_t index;

    if (!require(word != NULL && aliases != NULL && matches != NULL)) return;
    for (index = 0U; index < gsh_aliases_count(aliases); index++) {
        const char *assignment = gsh_aliases_assignment(aliases, index);

        if (assignment != NULL)
            offer_candidate(matches, word, assignment,
                            aliases->entries[index].name_length, false);
    }
}

static void scan_variable_names(const completion_word *word,
                                const gsh_variable_store *variables,
                                bool syntax, completion_matches *matches)
{
    size_t index;

    if (!require(word != NULL && variables != NULL && matches != NULL)) return;
    for (index = 0U; index < gsh_variables_count(variables); index++) {
        const char *assignment;
        size_t name_length;
        char candidate[GSH_COMPLETION_TEXT_CAP];
        size_t lead = syntax ? (word->braced_variable ? 2U : 1U) : 0U;

        assignment = gsh_variables_assignment(variables, index, NULL);
        if (assignment == NULL || !gsh_variables_is_set(variables, index))
            continue;
        name_length = variables->entries[index].name_length;
        if (lead + name_length >= sizeof(candidate)) continue;
        if (lead == 1U) candidate[0] = '$';
        else if (lead == 2U) {
            candidate[0] = '$';
            candidate[1] = '{';
        }
        (void)memcpy(candidate + lead, assignment, name_length);
        candidate[lead + name_length] = '\0';
        offer_candidate(matches, word, candidate, lead + name_length, false);
    }
}

static void scan_fixed_candidates(const completion_word *word,
                                  const char *const *candidates,
                                  size_t count, completion_matches *matches)
{
    size_t index;

    if (!require(word != NULL && candidates != NULL && matches != NULL))
        return;
    for (index = 0U; index < count; index++)
        offer_candidate(matches, word, candidates[index],
                        strlen(candidates[index]), false);
}

static void scan_git_subcommands(const completion_word *word,
                                 completion_matches *matches)
{
    static const char *const candidates[] = {
        "add", "bisect", "branch", "checkout", "cherry-pick", "clone",
        "commit", "diff", "fetch", "grep", "init", "log", "merge",
        "mv", "pull", "push", "rebase", "reset", "restore", "revert",
        "rm", "show", "stash", "status", "switch", "tag", "worktree",
    };

    scan_fixed_candidates(word, candidates,
                          sizeof(candidates) / sizeof(candidates[0]), matches);
}

static void scan_command_options(const completion_word *word,
                                 completion_matches *matches)
{
    static const char *const cd_options[] = {"-L", "-P"};
    static const char *const ls_options[] = {
        "-1", "-A", "-C", "-F", "-G", "-H", "-L", "-R", "-S",
        "-a", "-c", "-d", "-f", "-g", "-i", "-k", "-l", "-m",
        "-n", "-o", "-p", "-q", "-r", "-s", "-t", "-u", "-x",
    };
    static const char *const command_options[] = {"-V", "-p", "-v"};
    static const char *const name_options[] = {"-f", "-p", "-v"};
    const char *const *options = NULL;
    size_t count = 0U;

    if (!require(word != NULL && matches != NULL)) return;
    if (strcmp(word->command_name, "cd") == 0) {
        options = cd_options; count = sizeof(cd_options) / sizeof(cd_options[0]);
    } else if (strcmp(word->command_name, "ls") == 0 ||
               strcmp(word->command_name, "ll") == 0) {
        options = ls_options; count = sizeof(ls_options) / sizeof(ls_options[0]);
    } else if (strcmp(word->command_name, "command") == 0) {
        options = command_options;
        count = sizeof(command_options) / sizeof(command_options[0]);
    } else if (strcmp(word->command_name, "export") == 0 ||
               strcmp(word->command_name, "readonly") == 0 ||
               strcmp(word->command_name, "unset") == 0) {
        options = name_options; count = sizeof(name_options) / sizeof(name_options[0]);
    }
    if (options != NULL) scan_fixed_candidates(word, options, count, matches);
}

static bool completion_scan_path(const completion_word *word,
                                 const char *home, char *directory,
                                 size_t *display_length)
{
    const char *slash;
    size_t length;

    if (!require(word != NULL && directory != NULL)) return false;
    if (!require(display_length != NULL)) return false;
    slash = strrchr(word->prefix, '/');
    *display_length = slash == NULL ? 0U : (size_t)(slash - word->prefix) + 1U;
    if (slash == NULL) { (void)memcpy(directory, ".", 2U); return true; }
    length = *display_length - 1U;
    if (length == 0U) { (void)memcpy(directory, "/", 2U); return true; }
    if (word->prefix[0] == '~' && word->prefix[1] == '/' && home != NULL) {
        int written = *display_length == 2U
                          ? snprintf(directory, GSH_COMPLETION_TEXT_CAP,
                                     "%s", home)
                          : snprintf(directory, GSH_COMPLETION_TEXT_CAP,
                                     "%s/%.*s", home,
                                     (int)(*display_length - 3U),
                                     word->prefix + 2U);

        return written >= 0 && written < GSH_COMPLETION_TEXT_CAP;
    }
    if (length >= GSH_COMPLETION_TEXT_CAP) return false;
    (void)memcpy(directory, word->prefix, length);
    directory[length] = '\0';
    return true;
}

static bool pathname_candidate(const char *directory, const char *name,
                               completion_path_mode mode, bool *is_directory)
{
    char path[GSH_COMPLETION_TEXT_CAP];
    struct stat information;
    int length;

    if (!require(directory != NULL && name != NULL)) return false;
    if (!require(is_directory != NULL)) return false;
    length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (length < 0 || (size_t)length >= sizeof(path) ||
        stat(path, &information) == -1) return false;
    *is_directory = S_ISDIR(information.st_mode);
    if (mode == COMPLETION_PATH_DIRECTORY) return *is_directory;
    if (mode == COMPLETION_PATH_EXECUTABLE)
        return *is_directory || access(path, X_OK) == 0;
    return true;
}

static void scan_pathnames(const completion_word *word, const char *home,
                           completion_path_mode mode,
                           completion_matches *matches)
{
    char directory[GSH_COMPLETION_TEXT_CAP];
    size_t display_length;
    DIR *stream;
    size_t attempts;

    if (!require(word != NULL && matches != NULL)) return;
    if (!require(word->prefix_length < GSH_COMPLETION_TEXT_CAP)) return;
    if (strcmp(word->prefix, "~") == 0 && home != NULL && home[0] != '\0') {
        offer_candidate(matches, word, "~", 1U, true);
        return;
    }
    if (!completion_scan_path(word, home, directory, &display_length)) return;
    stream = opendir(directory);
    if (stream == NULL) return;
    for (attempts = 0U; attempts < COMPLETION_ENTRY_CAP; attempts++) {
        struct dirent *entry = readdir(stream);
        char candidate[GSH_COMPLETION_TEXT_CAP];
        size_t name_length;
        bool directory_candidate;

        if (entry == NULL) break;
        if ((entry->d_name[0] == '.' &&
             word->prefix[display_length] != '.') ||
            strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) continue;
        if (!pathname_candidate(directory, entry->d_name, mode,
                                &directory_candidate)) continue;
        name_length = strnlen(entry->d_name, sizeof(candidate));
        if (display_length + name_length >= sizeof(candidate)) continue;
        (void)memcpy(candidate, word->prefix, display_length);
        (void)memcpy(candidate + display_length, entry->d_name,
                     name_length + 1U);
        offer_candidate(matches, word, candidate,
                        display_length + name_length,
                        directory_candidate);
    }
    if (attempts == COMPLETION_ENTRY_CAP) matches->limited = true;
    (void)closedir(stream);
}

static bool safe_word_byte(unsigned char byte, bool first)
{
    if (!require(byte != 0U)) return false;
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '_' || byte == '-' ||
           byte == '.' || byte == '/' || (byte == '~' && first) ||
           byte >= 0x80U;
}

static bool encode_word(const completion_word *word,
                        const char *text, size_t length,
                        gsh_completion_result *result)
{
    size_t input;
    size_t output = 0U;

    if (!require(word != NULL && text != NULL && result != NULL)) return false;
    if (!require(length < GSH_COMPLETION_TEXT_CAP)) return false;
    for (input = 0U; input < length; input++) {
        unsigned char byte = (unsigned char)text[input];

        if (!word->variable && !safe_word_byte(byte, input == 0U)) {
            if (output + 2U >= sizeof(result->text)) return false;
            result->text[output++] = '\\';
        } else if (output + 1U >= sizeof(result->text)) return false;
        result->text[output++] = (char)byte;
    }
    result->text[output] = '\0';
    result->text_length = (uint32_t)output;
    return true;
}

static const char *candidate_at(const completion_matches *matches,
                                size_t index)
{
    if (!require(matches != NULL)) return NULL;
    if (!require(index < matches->candidate_count)) return NULL;
    return matches->candidate_text + matches->candidate_offsets[index];
}

static int compare_candidates(const completion_matches *matches,
                              size_t left, size_t right)
{
    const char *left_text;
    const char *right_text;

    if (!require(matches != NULL)) return 0;
    left_text = candidate_at(matches, left);
    right_text = candidate_at(matches, right);
    if (left_text == NULL || right_text == NULL) return 0;
    return strcmp(left_text, right_text);
}

static void sort_candidate_order(const completion_matches *matches,
                                 uint16_t order[COMPLETION_CANDIDATE_CAP])
{
    size_t index;

    if (!require(matches != NULL && order != NULL)) return;
    for (index = 0U; index < matches->candidate_count; index++) {
        size_t position = index;
        uint16_t candidate = (uint16_t)index;

        while (position > 0U && compare_candidates(
                   matches, candidate, order[position - 1U]) < 0) {
            order[position] = order[position - 1U];
            position--;
        }
        order[position] = candidate;
    }
}

static bool append_result_text(gsh_completion_result *result,
                               const char *text, size_t length)
{
    if (!require(result != NULL && text != NULL)) return false;
    if (result->menu_length + length >= sizeof(result->menu)) return false;
    (void)memcpy(result->menu + result->menu_length, text, length);
    result->menu_length += (uint32_t)length;
    result->menu[result->menu_length] = '\0';
    return true;
}

static bool encode_menu_candidate(const completion_word *word,
                                  const char *text, size_t length,
                                  bool directory, char *output,
                                  size_t capacity, size_t *used)
{
    size_t input;
    size_t output_length = 0U;

    if (!require(word != NULL && text != NULL && output != NULL)) return false;
    if (!require(capacity > 0U && used != NULL)) return false;
    for (input = 0U; input < length; input++) {
        unsigned char byte = (unsigned char)text[input];

        if (byte < 0x20U || byte == 0x7fU) byte = '?';
        if (!word->variable && !safe_word_byte(byte, input == 0U)) {
            if (output_length + 2U >= capacity) return false;
            output[output_length++] = '\\';
        } else if (output_length + 1U >= capacity) return false;
        output[output_length++] = (char)byte;
    }
    if (directory && (output_length == 0U ||
                      output[output_length - 1U] != '/')) {
        if (output_length + 1U >= capacity) return false;
        output[output_length++] = '/';
    }
    output[output_length] = '\0';
    *used = output_length;
    return true;
}

static size_t completion_menu_cell_width(
    const completion_word *word, const completion_matches *matches)
{
    size_t index;
    size_t maximum = 0U;

    if (!require(word != NULL && matches != NULL)) return 1U;
    for (index = 0U; index < matches->candidate_count; index++) {
        char encoded[GSH_COMPLETION_TEXT_CAP];
        size_t length = 0U;
        const char *candidate = candidate_at(matches, index);

        if (candidate != NULL && encode_menu_candidate(
                word, candidate, matches->candidate_lengths[index],
                matches->candidate_directories[index] != 0U,
                encoded, sizeof(encoded), &length) && length > maximum)
            maximum = length;
    }
    return maximum + 2U;
}

static bool append_result_padding(gsh_completion_result *result,
                                  size_t length)
{
    static const char spaces[] =
        "                                                                ";

    if (!require(result != NULL)) return false;
    while (length != 0U) {
        size_t chunk = length < sizeof(spaces) - 1U
                           ? length : sizeof(spaces) - 1U;

        if (!append_result_text(result, spaces, chunk)) return false;
        length -= chunk;
    }
    return true;
}

static bool render_completion_menu(const completion_word *word,
                                   const completion_matches *matches,
                                   size_t terminal_columns,
                                   uint32_t highlighted_index,
                                   gsh_completion_result *result)
{
    uint16_t order[COMPLETION_CANDIDATE_CAP];
    size_t row;
    size_t rows;
    size_t columns;
    size_t cell_width;
    size_t width = terminal_columns == 0U ? 80U : terminal_columns;

    if (!require(word != NULL && matches != NULL && result != NULL))
        return false;
    if (width < 20U) width = 20U;
    if (width > 256U) width = 256U;
    sort_candidate_order(matches, order);
    cell_width = completion_menu_cell_width(word, matches);
    columns = cell_width > width ? 1U : width / cell_width;
    if (columns == 0U) columns = 1U;
    if (columns > matches->candidate_count)
        columns = matches->candidate_count;
    rows = (matches->candidate_count + columns - 1U) / columns;
    for (row = 0U; row < rows; row++) {
        size_t column;

        for (column = 0U; column < columns; column++) {
            char encoded[GSH_COMPLETION_TEXT_CAP];
            size_t position = column * rows + row;
            size_t candidate_index;
            size_t encoded_length;
            const char *candidate;

            if (position >= matches->candidate_count) continue;
            candidate_index = order[position];
            candidate = candidate_at(matches, candidate_index);
            if (candidate == NULL || !encode_menu_candidate(
                    word, candidate,
                    matches->candidate_lengths[candidate_index],
                    matches->candidate_directories[candidate_index] != 0U,
                    encoded, sizeof(encoded), &encoded_length))
                return false;
            if (position == highlighted_index &&
                !append_result_text(result, "\033[7m", 4U)) return false;
            if (!append_result_text(result, encoded, encoded_length))
                return false;
            if (position == highlighted_index &&
                !append_result_text(result, "\033[0m", 4U)) return false;
            if (column + 1U < columns &&
                (column + 1U) * rows + row < matches->candidate_count &&
                !append_result_padding(result, cell_width - encoded_length))
                return false;
        }
        if (row + 1U < rows &&
            !append_result_text(result, "\r\n", 2U)) return false;
    }
    if (matches->candidate_limited &&
        !append_result_text(result, "\r\n...", 5U)) return false;
    return result->menu_length != 0U;
}

static bool complete_candidate(const completion_word *word,
                               const char *candidate, size_t length,
                               bool directory, bool final_candidate,
                               uint32_t status,
                               gsh_completion_result *result)
{
    char completed[GSH_COMPLETION_TEXT_CAP];
    bool append_space = false;

    if (!require(word != NULL && candidate != NULL && result != NULL))
        return false;
    if (!require(length < sizeof(completed))) return false;
    (void)memcpy(completed, candidate, length + 1U);
    if (word->at_end && final_candidate) {
        if (word->variable && word->braced_variable &&
            !word->closed_variable) {
            if (length + 1U >= sizeof(completed)) return false;
            completed[length++] = '}';
            completed[length] = '\0';
        } else if (directory &&
                   (length == 0U || completed[length - 1U] != '/')) {
            if (length + 1U >= sizeof(completed)) return false;
            completed[length++] = '/';
            completed[length] = '\0';
        } else if (!word->variable) append_space = true;
    }
    if (!encode_word(word, completed, length, result)) return false;
    if (append_space) {
        if (result->text_length + 1U >= sizeof(result->text)) return false;
        result->text[result->text_length++] = ' ';
        result->text[result->text_length] = '\0';
    }
    result->status = status;
    return true;
}

static void finalize_completion(const completion_word *word,
                                completion_matches *matches,
                                size_t terminal_columns,
                                uint32_t selection_index,
                                gsh_completion_result *result)
{
    uint16_t order[COMPLETION_CANDIDATE_CAP];
    size_t selected;

    if (!require(word != NULL && matches != NULL && result != NULL)) return;
    if (matches->limited) { result->status = GSH_COMPLETION_LIMIT; return; }
    if (!matches->matched || matches->candidate_count == 0U) return;
    result->candidate_count = (uint32_t)matches->candidate_count;
    if (selection_index != GSH_COMPLETION_SELECT_MENU) {
        sort_candidate_order(matches, order);
        selected = selection_index % matches->candidate_count;
        result->selected_index = (uint32_t)selected;
        selected = order[selected];
        if (!complete_candidate(
                word, candidate_at(matches, selected),
                matches->candidate_lengths[selected],
                matches->candidate_directories[selected] != 0U,
                true, GSH_COMPLETION_CYCLE, result))
            result->status = GSH_COMPLETION_LIMIT;
        else if (!render_completion_menu(
                     word, matches, terminal_columns,
                     result->selected_index, result))
            result->status = GSH_COMPLETION_LIMIT;
        return;
    }
    if (matches->distinct &&
        (word->prefix_length == 0U ||
         matches->common_length == word->prefix_length)) {
        if (render_completion_menu(word, matches, terminal_columns,
                                   GSH_COMPLETION_SELECT_MENU, result))
            result->status = GSH_COMPLETION_MENU;
        else result->status = GSH_COMPLETION_LIMIT;
        return;
    }
    if (!complete_candidate(
            word, matches->distinct ? matches->common : matches->first,
            matches->distinct ? matches->common_length : matches->first_length,
            !matches->distinct && matches->candidate_directories[0] != 0U,
            !matches->distinct, GSH_COMPLETION_EDIT, result))
        result->status = GSH_COMPLETION_LIMIT;
}

static bool command_is(const completion_word *word, const char *name)
{
    if (!require(word != NULL && name != NULL)) return false;
    return strcmp(word->command_name, name) == 0;
}

static void scan_all_commands(const completion_word *word, const char *path,
                              const gsh_alias_store *aliases,
                              const gsh_function_store *functions,
                              completion_matches *matches)
{
    if (!require(word != NULL && path != NULL && aliases != NULL)) return;
    if (!require(functions != NULL && matches != NULL)) return;
    scan_shell_commands(word, aliases, functions, matches);
    scan_path_commands(path, word, matches);
}

static void scan_context_candidates(
    const completion_word *word, const char *path, const char *home,
    const gsh_alias_store *aliases, const gsh_function_store *functions,
    const gsh_variable_store *variables, completion_matches *matches)
{
    bool command_argument;

    if (!require(word != NULL && path != NULL && aliases != NULL)) return;
    if (!require(functions != NULL && variables != NULL && matches != NULL))
        return;
    if (word->variable) {
        scan_variable_names(word, variables, true, matches);
        return;
    }
    command_argument = (command_is(word, "command") ||
                        command_is(word, "exec") ||
                        command_is(word, "help") ||
                        command_is(word, "type") ||
                        command_is(word, "hash")) &&
                       word->argument_index == 1U;
    if ((word->command || command_argument) &&
        strchr(word->prefix, '/') == NULL) {
        scan_all_commands(word, path, aliases, functions, matches);
        return;
    }
    if (word->prefix[0] == '-') {
        scan_command_options(word, matches);
        if (matches->matched) return;
    }
    if (command_is(word, "git") && word->argument_index == 1U) {
        scan_git_subcommands(word, matches);
    } else if (command_is(word, "unalias")) {
        scan_alias_names(word, aliases, matches);
    } else if (command_is(word, "export") || command_is(word, "readonly") ||
               command_is(word, "unset")) {
        scan_variable_names(word, variables, false, matches);
    } else if (command_is(word, "cd")) {
        scan_pathnames(word, home, COMPLETION_PATH_DIRECTORY, matches);
    } else {
        completion_path_mode mode = word->command
                                        ? COMPLETION_PATH_EXECUTABLE
                                        : COMPLETION_PATH_ANY;

        scan_pathnames(word, home, mode, matches);
    }
}

int gsh_completion_generate(
    const char *line, size_t line_length, size_t cursor,
    const char *path, const char *home,
    const gsh_alias_store *aliases,
    const gsh_function_store *functions,
    const gsh_variable_store *variables,
    size_t terminal_columns, uint32_t selection_index,
    uint64_t request_id, gsh_completion_result *result)
{
    completion_word word;
    completion_matches matches;

    if (!require(line != NULL && path != NULL && result != NULL)) return -1;
    if (!require(aliases != NULL && functions != NULL && variables != NULL))
        return -1;
    (void)memset(result, 0, sizeof(*result));
    result->version = GSH_COMPLETION_VERSION;
    result->request_id = request_id;
    if (!locate_variable_word(line, line_length, cursor, &word)) {
        if (!locate_completion_word(line, line_length, cursor, &word) ||
            !decode_prefix(line, cursor, &word)) return 0;
        scan_completion_context(line, &word);
    }
    (void)memset(&matches, 0, sizeof(matches));
    scan_context_candidates(&word, path, home, aliases, functions, variables,
                            &matches);
    result->begin = (uint32_t)word.begin;
    result->end = (uint32_t)cursor;
    finalize_completion(&word, &matches, terminal_columns, selection_index,
                        result);
    return 0;
}

bool gsh_completion_result_valid(const gsh_completion_result *result)
{
    if (!require(result != NULL)) return false;
    if (!require(result->version == GSH_COMPLETION_VERSION)) return false;
    return result->status <= GSH_COMPLETION_LIMIT &&
           result->begin <= result->end &&
           result->end < GSH_COMPLETION_TEXT_CAP &&
           result->text_length < GSH_COMPLETION_TEXT_CAP &&
           result->menu_length < GSH_COMPLETION_TEXT_CAP &&
           result->candidate_count <= COMPLETION_CANDIDATE_CAP &&
           result->text[result->text_length] == '\0' &&
           result->menu[result->menu_length] == '\0';
}
