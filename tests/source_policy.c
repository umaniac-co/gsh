#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh policy checks require the POSIX.1-2024 feature-test baseline"
#endif

#include "canon_call_graph.h"
#include "canon_preprocessor.h"
#include "canon_symbols.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define require(condition) (condition)

enum {
    POLICY_DIRECTORY_CAP = 32,
    POLICY_FILE_CAP = 4 * 1024 * 1024,
    POLICY_IDENTIFIER_CAP = 128,
    POLICY_MAP_LINE_CAP = 4096,
    POLICY_MAP_READ_CAP = 64 * 1024 * 1024,
    POLICY_BINARY_CHUNK_CAP = POLICY_MAP_READ_CAP / 8192,
};

static const char *const FORBIDDEN_SUFFIXES[] = {
    ".cc", ".cpp", ".cxx", ".go", ".js", ".py", ".rs", ".sh", ".ts",
};

static const char *const FORBIDDEN_CALLS[] = {
    "abort", "assert", "calloc", "fdopen", "free", "glob", "globfree", "longjmp",
    "malloc", "mmap", "munmap", "qsort", "realloc", "setjmp",
    "tmpfile",
};

static const char *const MANIFEST_TARGETS[] = {
    "alias-test", "background-test", "benchmark-report-test",
    "command-cache-test", "config-test", "conformance", "fault", "function-test",
    "file-builtins-test", "fuzz", "gsh", "job-probe",
    "llm-hardware-test", "llm-json-test", "llm-journal-test", "llm-worker",
    "llm-worker-test",
    "positional-test",
    "pty", "resource-actions-test", "source-policy",
    "setup", "source-workspace-test", "trap-test", "variable-test",
};

typedef struct {
    size_t goto_statements;
    size_t forbidden_calls;
    size_t function_pointers;
    size_t approved_function_pointers;
    size_t disabled_blocks;
    size_t constant_branches;
    size_t unreachable_functions;
    size_t direct_recursive_calls;
    size_t recursive_functions;
    size_t oversized_functions;
    size_t assertion_deficit;
    size_t unbounded_loops;
    size_t unused_declarations;
    size_t nested_dereferences;
    size_t mutable_globals;
    size_t preprocessor_violations;
    size_t unchecked_returns;
    size_t unvalidated_parameters;
} canon_counts;

typedef struct {
    char paths[POLICY_DIRECTORY_CAP][PATH_MAX];
    size_t count;
} directory_stack;

typedef enum {
    LEX_NORMAL,
    LEX_LINE_COMMENT,
    LEX_BLOCK_COMMENT,
    LEX_STRING,
    LEX_CHARACTER,
} lexical_state;

typedef struct {
    lexical_state state;
    char token[POLICY_IDENTIFIER_CAP];
    bool pending_forbidden_call;
    bool escaped;
    size_t line_begin;
    size_t pointer_stage;
    size_t constant_stage;
    size_t loop_stage;
    size_t dereference_stage;
    size_t global_brace_depth;
    bool global_declaration;
    bool global_const;
    bool global_volatile;
    bool global_sig_atomic;
    bool global_parenthesis;
    bool global_assignment;
    size_t index;
} c_token_scanner;

typedef enum {
    LINK_MAP_UNKNOWN,
    LINK_MAP_DARWIN_LIVE,
    LINK_MAP_DARWIN_DISCARDED,
    LINK_MAP_LINUX_LIVE,
    LINK_MAP_LINUX_DISCARDED,
} link_map_state;

typedef struct {
    bool live[GSH_CANON_FUNCTION_CAP];
    bool discarded[GSH_CANON_FUNCTION_CAP];
} link_map_evidence;

static bool has_suffix(const char *path, const char *suffix)
{
    if (!require(path != NULL)) return false;
    if (!require(suffix != NULL)) return false;
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);

    return suffix_length <= path_length &&
           strcmp(path + path_length - suffix_length, suffix) == 0;
}

static bool buffer_contains(const unsigned char *data, size_t length,
                            const char *needle)
{
    if (needle == NULL) {
        return false;
    }
    size_t needle_length = strlen(needle);
    size_t index;

    if (needle_length == 0 || needle_length > length) {
        return false;
    }
    for (index = 0; index + needle_length <= length; index++) {
        if (memcmp(data + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool identifier_start(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') || byte == '_';
}

static bool identifier_continue(unsigned char byte)
{
    return identifier_start(byte) || (byte >= '0' && byte <= '9');
}

static bool token_is_forbidden_call(const char *token)
{
    size_t index;

    if (!require(token != NULL)) return false;
    for (index = 0;
         index < sizeof(FORBIDDEN_CALLS) / sizeof(FORBIDDEN_CALLS[0]);
         index++) {
        if (strcmp(token, FORBIDDEN_CALLS[index]) == 0) {
            return true;
        }
    }
    return false;
}

static bool line_has_disabled_block(const unsigned char *data, size_t begin,
                                    size_t end)
{
    if (data == NULL) {
        return false;
    }
    size_t index = begin;

    while (index < end && (data[index] == ' ' || data[index] == '\t')) {
        index++;
    }
    if (index >= end || data[index] != '#') {
        return false;
    }
    index++;
    while (index < end && (data[index] == ' ' || data[index] == '\t')) {
        index++;
    }
    if (index + 2U > end || data[index] != 'i' || data[index + 1U] != 'f') {
        return false;
    }
    index += 2U;
    while (index < end && (data[index] == ' ' || data[index] == '\t')) {
        index++;
    }
    if (index >= end || data[index] != '0') {
        return false;
    }
    index++;
    return index == end || data[index] == ' ' || data[index] == '\t' ||
           data[index] == '/';
}

static bool approved_exception_identifier(const unsigned char *identifier,
                                          size_t length)
{
    static const char *const approved[] = {
        "POSIX-SIGNAL-DISPOSITION", "C-PROCESS-ABI",
        "PLATFORM-CAPABILITY",
    };
    size_t index;

    if (!require(identifier != NULL)) return false;
    if (!require(length > 0U)) return false;
    for (index = 0U; index < sizeof(approved) / sizeof(approved[0]);
         index++) {
        if (strlen(approved[index]) == length &&
            memcmp(identifier, approved[index], length) == 0) {
            return true;
        }
    }
    return false;
}

/* ── Exception Names Are Closed, Source-Visible Contracts ───────
 * Free-form exception prose would let a patch silently invent new policy.
 * The comment lexer recognizes only CANON-EXCEPTION markers and the three IDs
 * approved in CODE.md, while quoted examples remain ordinary test data.
 * Unknown or empty identifiers fail before the repository counts are checked.
 * This keeps local design notes tied to the centrally reviewed exception set.
 * ─────────────────────────────────────────────────────────────── */
static int validate_exception_markers(const unsigned char *data,
                                      size_t length, const char *path,
                                      bool report)
{
    static const unsigned char marker[] = "CANON-EXCEPTION:";
    lexical_state state = LEX_NORMAL;
    bool escaped = false;
    size_t index = 0U;

    if (!require(data != NULL)) return -1;
    if (!require(path != NULL)) return -1;
    while (index < length) {
        unsigned char byte = data[index];

        if (state == LEX_LINE_COMMENT || state == LEX_BLOCK_COMMENT) {
            bool marker_here =
                sizeof(marker) - 1U <= length - index &&
                memcmp(data + index, marker, sizeof(marker) - 1U) == 0;

            if (marker_here) {
                size_t begin = index + sizeof(marker) - 1U;
                size_t end;

                while (begin < length &&
                       (data[begin] == ' ' || data[begin] == '\t')) {
                    begin++;
                }
                end = begin;
                while (end < length &&
                       ((data[end] >= 'A' && data[end] <= 'Z') ||
                        data[end] == '-')) {
                    end++;
                }
                if (begin == end ||
                    !approved_exception_identifier(data + begin,
                                                   end - begin)) {
                    if (report) {
                        (void)fprintf(stderr,
                                "source policy: unknown canon exception in %s\n",
                                path);
                    }
                    return 1;
                }
                index = end;
                continue;
            }
            if (state == LEX_LINE_COMMENT && byte == '\n') {
                state = LEX_NORMAL;
            } else if (state == LEX_BLOCK_COMMENT && byte == '*' &&
                       index + 1U < length && data[index + 1U] == '/') {
                state = LEX_NORMAL;
                index += 2U;
                continue;
            }
            index++;
            continue;
        }
        if (state == LEX_STRING || state == LEX_CHARACTER) {
            unsigned char terminator = state == LEX_STRING ? '"' : '\'';

            if (!escaped && byte == terminator) state = LEX_NORMAL;
            escaped = !escaped && byte == '\\';
            if (byte != '\\') escaped = false;
            index++;
            continue;
        }
        if (byte == '/' && index + 1U < length &&
            data[index + 1U] == '/') {
            state = LEX_LINE_COMMENT;
            index += 2U;
        } else if (byte == '/' && index + 1U < length &&
                   data[index + 1U] == '*') {
            state = LEX_BLOCK_COMMENT;
            index += 2U;
        } else if (byte == '"' || byte == '\'') {
            state = byte == '"' ? LEX_STRING : LEX_CHARACTER;
            escaped = false;
            index++;
        } else {
            index++;
        }
    }
    return 0;
}

static int exception_fixture_test(void)
{
    static const unsigned char positive[] =
        "/* CANON-EXCEPTION: C-PROCESS-ABI */\n"
        "const char *example = \"CANON-EXCEPTION: NOT-AN-ID\";\n";
    static const unsigned char negative[] =
        "/* CANON-EXCEPTION: UNREVIEWED-ESCAPE */\n";

    if (validate_exception_markers(positive, sizeof(positive) - 1U,
                                   "fixture/positive.c", false) != 0) {
        (void)fputs("source policy: approved exception fixture rejected\n",
              stderr);
        return 1;
    }
    if (validate_exception_markers(negative, sizeof(negative) - 1U,
                                   "fixture/negative.c", false) != 1) {
        (void)fputs("source policy: unknown exception fixture not detected\n",
              stderr);
        return 1;
    }
    return 0;
}

/* ── A Bounded Lexer Makes the Permanent Gate Source-Aware ─────
 * Raw text searches once treated examples and comments as executable code,
 * making policy counts unstable whenever documentation improved.
 * The policy tool now walks each byte once with explicit lexical states and
 * fixed storage, recognizing only identifiers and punctuation in C code.
 * Embedded fixtures exercise both ignored prose and rejected constructs.
 * ─────────────────────────────────────────────────────────────── */
static bool scan_non_code_byte(const unsigned char *data, size_t length,
                               c_token_scanner *scanner,
                               canon_counts *counts)
{
    if (counts == NULL || data == NULL || scanner == NULL) {
        return false;
    }
    unsigned char byte;

    byte = data[scanner->index];
    if (byte == '\n') {
        if (scanner->state == LEX_NORMAL &&
            line_has_disabled_block(data, scanner->line_begin,
                                    scanner->index)) {
            counts->disabled_blocks++;
        }
        scanner->line_begin = scanner->index + 1U;
        if (scanner->state == LEX_LINE_COMMENT) {
            scanner->state = LEX_NORMAL;
        }
    }
    if (scanner->state == LEX_LINE_COMMENT) {
        scanner->index++;
        return true;
    }
    if (scanner->state == LEX_BLOCK_COMMENT) {
        if (byte == '*' && scanner->index + 1U < length &&
            data[scanner->index + 1U] == '/') {
            scanner->state = LEX_NORMAL;
            scanner->index += 2U;
        } else {
            scanner->index++;
        }
        return true;
    }
    if (scanner->state == LEX_STRING ||
        scanner->state == LEX_CHARACTER) {
        unsigned char terminator =
            scanner->state == LEX_STRING ? '"' : '\'';

        if (!scanner->escaped && byte == terminator) {
            scanner->state = LEX_NORMAL;
        }
        scanner->escaped = !scanner->escaped && byte == '\\';
        if (byte != '\\') scanner->escaped = false;
        scanner->index++;
        return true;
    }
    if (byte == '/' && scanner->index + 1U < length &&
        data[scanner->index + 1U] == '/') {
        scanner->state = LEX_LINE_COMMENT;
        scanner->index += 2U;
        return true;
    }
    if (byte == '/' && scanner->index + 1U < length &&
        data[scanner->index + 1U] == '*') {
        scanner->state = LEX_BLOCK_COMMENT;
        scanner->index += 2U;
        return true;
    }
    if (byte == '"' || byte == '\'') {
        scanner->state = byte == '"' ? LEX_STRING : LEX_CHARACTER;
        scanner->escaped = false;
        scanner->pending_forbidden_call = false;
        scanner->pointer_stage = 0U;
        scanner->index++;
        return true;
    }
    return false;
}

static int scan_identifier_token(const unsigned char *data, size_t length,
                                 const char *path,
                                 c_token_scanner *scanner,
                                 canon_counts *counts)
{
    if (scanner == NULL) {
        return -1;
    }
    size_t begin = scanner->index;
    size_t token_length;

    if (!require(data != NULL && path != NULL && scanner != NULL)) return 1;
    if (!require(counts != NULL && begin < length &&
                 identifier_start(data[begin]))) return 1;
    while (scanner->index < length &&
           identifier_continue(data[scanner->index])) {
        scanner->index++;
    }
    token_length = scanner->index - begin;
    if (token_length >= sizeof(scanner->token)) {
        (void)fprintf(stderr, "source policy: identifier too long: %s\n", path);
        return 1;
    }
    (void)memcpy(scanner->token, data + begin, token_length);
    scanner->token[token_length] = '\0';
    if (scanner->global_brace_depth == 0U) {
        if (!scanner->global_declaration &&
            strcmp(scanner->token, "static") == 0) {
            scanner->global_declaration = true;
        } else if (scanner->global_declaration &&
                   strcmp(scanner->token, "const") == 0) {
            scanner->global_const = true;
        } else if (scanner->global_declaration &&
                   strcmp(scanner->token, "volatile") == 0) {
            scanner->global_volatile = true;
        } else if (scanner->global_declaration &&
                   strcmp(scanner->token, "sig_atomic_t") == 0) {
            scanner->global_sig_atomic = true;
        }
    }
    if (strcmp(scanner->token, "goto") == 0) {
        counts->goto_statements++;
    }
    if (strcmp(scanner->token, "if") == 0 ||
        strcmp(scanner->token, "while") == 0) {
        scanner->constant_stage = 1U;
    } else if (scanner->constant_stage != 0U) {
        scanner->constant_stage = 0U;
    }
    scanner->pending_forbidden_call =
        token_is_forbidden_call(scanner->token);
    if (strcmp(scanner->token, "for") == 0) {
        scanner->loop_stage = 1U;
    } else if (strcmp(scanner->token, "while") == 0) {
        scanner->loop_stage = 10U;
    } else if (scanner->loop_stage == 11U &&
               strcmp(scanner->token, "true") == 0) {
        scanner->loop_stage = 12U;
    } else if (scanner->loop_stage != 0U) {
        scanner->loop_stage = 0U;
    }
    if (scanner->pointer_stage == 2U) {
        scanner->pointer_stage = 3U;
    } else if (scanner->pointer_stage != 0U) {
        scanner->pointer_stage = 0U;
    }
    if (scanner->dereference_stage == 3U) {
        scanner->dereference_stage = 4U;
    } else {
        scanner->dereference_stage = 1U;
    }
    return 0;
}

static void scan_loop_punctuation(unsigned char byte,
                                  c_token_scanner *scanner,
                                  canon_counts *counts)
{
    if (!require(scanner != NULL)) return;
    if (!require(counts != NULL)) return;
    if (byte == '(' && scanner->loop_stage == 1U) {
        scanner->loop_stage = 2U;
    } else if (byte == ';' && scanner->loop_stage == 2U) {
        scanner->loop_stage = 3U;
    } else if (byte == ';' && scanner->loop_stage == 3U) {
        scanner->loop_stage = 4U;
    } else if (byte == ')' && scanner->loop_stage == 4U) {
        counts->unbounded_loops++;
        scanner->loop_stage = 0U;
    } else if (byte == '(' && scanner->loop_stage == 10U) {
        scanner->loop_stage = 11U;
    } else if (byte == '1' && scanner->loop_stage == 11U) {
        scanner->loop_stage = 12U;
    } else if (byte == ')' && scanner->loop_stage == 12U) {
        counts->unbounded_loops++;
        scanner->loop_stage = 0U;
    } else if (scanner->loop_stage != 0U) {
        scanner->loop_stage = 0U;
    }
}

static void scan_global_scope_punctuation(unsigned char byte,
                                          const char *path,
                                          c_token_scanner *scanner,
                                          canon_counts *counts)
{
    if (!require(path != NULL && scanner != NULL)) return;
    if (!require(counts != NULL)) return;
    if (scanner->global_declaration && byte == '(' &&
        scanner->global_brace_depth == 0U) {
        scanner->global_parenthesis = true;
    } else if (scanner->global_declaration && byte == '=' &&
               scanner->global_brace_depth == 0U) {
        scanner->global_assignment = true;
    }
    if (byte == '{') {
        if (scanner->global_declaration && scanner->global_parenthesis &&
            !scanner->global_assignment &&
            scanner->global_brace_depth == 0U) {
            scanner->global_declaration = false;
            scanner->global_const = false;
            scanner->global_volatile = false;
            scanner->global_sig_atomic = false;
            scanner->global_parenthesis = false;
            scanner->global_assignment = false;
        }
        scanner->global_brace_depth++;
    } else if (byte == '}' && scanner->global_brace_depth > 0U) {
        scanner->global_brace_depth--;
    } else if (byte == ';' && scanner->global_declaration &&
               scanner->global_brace_depth == 0U) {
        if (!scanner->global_const &&
            !(scanner->global_volatile && scanner->global_sig_atomic) &&
            !(scanner->global_parenthesis && !scanner->global_assignment)) {
            counts->mutable_globals++;
            if (strncmp(path, "fixture/", 8U) != 0) {
                (void)fprintf(stderr,
                        "source policy: mutable file-scope object: %s at "
                        "byte %zu\n",
                        path, scanner->index);
            }
        }
        scanner->global_declaration = false;
        scanner->global_const = false;
        scanner->global_volatile = false;
        scanner->global_sig_atomic = false;
        scanner->global_parenthesis = false;
        scanner->global_assignment = false;
    }
}

static void scan_code_punctuation(unsigned char byte, const char *path,
                                  c_token_scanner *scanner,
                                  canon_counts *counts)
{
    if (!require(path != NULL && scanner != NULL)) return;
    if (!require(counts != NULL)) return;
    scan_global_scope_punctuation(byte, path, scanner, counts);
    if (scanner->pending_forbidden_call && byte == '(') {
        counts->forbidden_calls++;
    }
    scanner->pending_forbidden_call = false;
    if (byte == '-' && scanner->dereference_stage == 1U) {
        scanner->dereference_stage = 2U;
    } else if (byte == '>' && scanner->dereference_stage == 2U) {
        scanner->dereference_stage = 3U;
    } else if (byte == '-' && scanner->dereference_stage == 4U) {
        scanner->dereference_stage = 5U;
    } else if (byte == '>' && scanner->dereference_stage == 5U) {
        counts->nested_dereferences++;
        if (strncmp(path, "fixture/", 8U) != 0) {
            (void)fprintf(stderr,
                    "source policy: nested dereference: %s at byte %zu\n",
                    path, scanner->index);
        }
        scanner->dereference_stage = 3U;
    } else {
        scanner->dereference_stage = 0U;
    }
    if (byte == '(' && scanner->constant_stage == 1U) {
        scanner->constant_stage = 2U;
    } else if (byte == '0' && scanner->constant_stage == 2U) {
        scanner->constant_stage = 3U;
    } else if (byte == ')' && scanner->constant_stage == 3U) {
        counts->constant_branches++;
        scanner->constant_stage = 0U;
    } else {
        scanner->constant_stage = 0U;
    }
    if (byte == '(') {
        if (scanner->pointer_stage == 0U) {
            scanner->pointer_stage = 1U;
        } else if (scanner->pointer_stage == 4U) {
            if (has_suffix(path, "/src/gsh.c") &&
                strcmp(scanner->token, "handler") == 0) {
                counts->approved_function_pointers++;
            } else {
                counts->function_pointers++;
            }
            scanner->pointer_stage = 0U;
        } else {
            scanner->pointer_stage = 0U;
        }
    } else if (byte == '*' && scanner->pointer_stage == 1U) {
        scanner->pointer_stage = 2U;
    } else if (byte == ')' && scanner->pointer_stage == 3U) {
        scanner->pointer_stage = 4U;
    } else {
        scanner->pointer_stage = 0U;
    }
    scan_loop_punctuation(byte, scanner, counts);
}

static int scan_c_tokens(const unsigned char *data, size_t length,
                         const char *path, canon_counts *counts)
{
    c_token_scanner scanner = {0};

    if (data == NULL || path == NULL || counts == NULL ||
        length > POLICY_FILE_CAP) {
        errno = EINVAL;
        return -1;
    }
    while (scanner.index < length) {
        unsigned char byte = data[scanner.index];

        if (scan_non_code_byte(data, length, &scanner, counts)) continue;
        if (identifier_start(byte)) {
            if (scan_identifier_token(data, length, path, &scanner,
                                      counts) != 0) return 1;
            continue;
        }
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' ||
            byte == '\f' || byte == '\v') {
            scanner.index++;
            continue;
        }
        scan_code_punctuation(byte, path, &scanner, counts);
        scanner.index++;
    }
    if (scanner.state == LEX_NORMAL && scanner.line_begin < length &&
        line_has_disabled_block(data, scanner.line_begin, length)) {
        counts->disabled_blocks++;
    }
    return 0;
}

static bool canon_counts_equal(const canon_counts *left,
                               const canon_counts *right)
{
    if (!require(left != NULL)) return false;
    if (!require(right != NULL)) return false;
    return left->goto_statements == right->goto_statements &&
           left->forbidden_calls == right->forbidden_calls &&
           left->function_pointers == right->function_pointers &&
           left->approved_function_pointers ==
               right->approved_function_pointers &&
           left->disabled_blocks == right->disabled_blocks &&
           left->constant_branches == right->constant_branches &&
           left->unreachable_functions == right->unreachable_functions &&
           left->direct_recursive_calls == right->direct_recursive_calls &&
           left->recursive_functions == right->recursive_functions &&
           left->oversized_functions == right->oversized_functions &&
           left->assertion_deficit == right->assertion_deficit &&
           left->unbounded_loops == right->unbounded_loops &&
           left->unused_declarations == right->unused_declarations &&
           left->nested_dereferences == right->nested_dereferences &&
           left->mutable_globals == right->mutable_globals &&
           left->preprocessor_violations == right->preprocessor_violations &&
           left->unchecked_returns == right->unchecked_returns &&
           left->unvalidated_parameters == right->unvalidated_parameters;
}

static int lexer_fixture_test(void)
{
    static const unsigned char positive[] =
        "/* goto malloc(1); #if 0 */\n"
        "const char *s = \"goto qsort(0,0,0,0)\";\n"
        "static const int live_constant = 1;\n"
        "static volatile sig_atomic_t signal_flag;\n";
    static const unsigned char negative[] =
        "#if 0\n#endif\n"
        "void f(void (*callback)(int)) { if (0) goto done; "
        "done: malloc(1); abort(); for (;;) {} }\n"
        "int nested(struct node *value) { return value->next->count; }\n"
        "static int mutable_global;\n";
    canon_counts actual = {0};
    canon_counts expected = {
        1U, 2U, 1U, 0U, 1U, 1U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 1U, 1U,
        0U, 0U, 0U};

    if (scan_c_tokens(positive, sizeof(positive) - 1U,
                      "fixture/positive.c", &actual) != 0 ||
        !canon_counts_equal(&actual, &(canon_counts){0})) {
        (void)fputs("source policy: positive lexer fixture rejected\n", stderr);
        return 1;
    }
    (void)memset(&actual, 0, sizeof(actual));
    if (scan_c_tokens(negative, sizeof(negative) - 1U,
                      "fixture/negative.c", &actual) != 0 ||
        !canon_counts_equal(&actual, &expected)) {
        (void)fputs("source policy: negative lexer fixture not detected\n", stderr);
        return 1;
    }
    return 0;
}

static int preprocessor_fixture_test(void)
{
    static const unsigned char positive[] =
        "#ifndef GSH_FIXTURE_H\n#define GSH_FIXTURE_H\n#endif\n"
        "#define SIMPLE(value) ((value) + 1)\n"
        "#if defined(__APPLE__)\n#endif\n";
    static const unsigned char negative[] =
        "#define BAD_VARIADIC(...) 0\n"
        "#define BAD_PASTE(a, b) a ## b\n"
        "#define BAD_RECURSIVE BAD_RECURSIVE\n"
        "#define BAD_FRAGMENT(value) , value\n"
        "#ifdef UNKNOWN_CONFIGURATION\n#endif\n";
    size_t violations = 0U;

    if (gsh_canon_preprocessor_analyze(
            positive, sizeof(positive) - 1U, "fixture/preprocessor-ok.c",
            &violations, false) == -1 || violations != 0U) {
        (void)fputs("source policy: valid preprocessor fixture rejected\n", stderr);
        return 1;
    }
    if (gsh_canon_preprocessor_analyze(
            negative, sizeof(negative) - 1U,
            "fixture/preprocessor-bad.c", &violations, false) == -1 ||
        violations != 5U) {
        (void)fputs("source policy: preprocessor violations not detected\n",
              stderr);
        return 1;
    }
    return 0;
}

static int read_manifest_text(const char *path, char *data, size_t capacity,
                              size_t *length)
{
    size_t attempts;
    int descriptor;

    if (path == NULL || data == NULL || capacity < 2U || length == NULL) {
        errno = EINVAL;
        return -1;
    }
    *length = 0U;
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor == -1) return -1;
    for (attempts = 0; *length < capacity - 1U && attempts < capacity;
         attempts++) {
        ssize_t count = read(descriptor, data + *length,
                             capacity - 1U - *length);

        if (count > 0) {
            *length += (size_t)count;
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            (void)close(descriptor);
            return -1;
        }
    }
    if (close(descriptor) == -1 || *length == capacity - 1U) {
        errno = EOVERFLOW;
        return -1;
    }
    data[*length] = '\0';
    return 0;
}

/* ── The Call-Graph Proof Tests Its Own Blind Spots ─────────────
 * A reachability checker that merely runs can still accept every function.
 * These in-memory translation units give it one live helper and one orphan.
 * The positive graph must close from main without recursion; the negative
 * graph must identify exactly the uncalled function and no invented cycle.
 * Fixed fixture text exercises the same lexer and graph used on the repository.
 * ─────────────────────────────────────────────────────────────── */
static int parameter_call_graph_fixture_test(void)
{
    static const unsigned char unvalidated[] =
        "static int value(const char *text) { return text[0]; } "
        "int main(void) { return value((const char *)1); }\n";
    static const unsigned char indirect[] =
        "static int first(const char *text) { return *text; } "
        "static int field(const struct item *item) { return item->value; } "
        "int main(void) { return first((const char *)1) + "
        "field((const struct item *)1); }\n";
    static const unsigned char guarded[] =
        "static int value(const char *text) { "
        "if (text == 0) return -1; return text[0]; } "
        "int main(void) { return value((const char *)1); }\n";
    static const unsigned char forwarded[] =
        "static int sink(const char *text) { (void)text; return 0; } "
        "static int value(const char *text) { return sink(text); } "
        "int main(void) { return value((const char *)0); }\n";
    static const unsigned char abi_guarded[] =
        "int main(int argc, char **argv) { "
        "if (argc < 2) return 1; return argv[1] == 0; }\n";
    static const unsigned char abi_unguarded[] =
        "int main(int argc, char **argv) { return argv[0] == 0; }\n";
    static gsh_canon_call_graph graph;
    size_t unreachable;
    size_t direct;
    size_t recursive;
    size_t oversized;
    size_t assertion_deficit;
    size_t unchecked_returns;
    size_t parameters;

    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(&graph, unvalidated,
                                 sizeof(unvalidated) - 1U,
                                 "fixture/unvalidated-parameter.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns, &parameters, false) == -1 ||
        unreachable != 0U || parameters != 1U) return 1;
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(&graph, indirect, sizeof(indirect) - 1U,
                                 "fixture/indirect-parameter.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns, &parameters, false) == -1 ||
        unreachable != 0U || parameters != 2U) return 1;
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(&graph, guarded, sizeof(guarded) - 1U,
                                 "fixture/guarded-parameter.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns, &parameters, false) == -1 ||
        unreachable != 0U || parameters != 0U) return 1;
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(&graph, forwarded, sizeof(forwarded) - 1U,
                                 "fixture/forwarded-parameter.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns, &parameters, false) == -1 ||
        unreachable != 0U || parameters != 0U) return 1;
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(&graph, abi_guarded,
                                 sizeof(abi_guarded) - 1U,
                                 "fixture/abi-guarded.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns, &parameters, false) == -1 ||
        unreachable != 0U || parameters != 0U) return 1;
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(&graph, abi_unguarded,
                                 sizeof(abi_unguarded) - 1U,
                                 "fixture/abi-unguarded.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns, &parameters, false) == -1 ||
        unreachable != 0U || parameters != 1U) return 1;
    return 0;
}

static int call_graph_fixture_test(void)
{
    static const unsigned char positive[] =
        "static void live(void) {} int main(void) { live(); }\n";
    static const unsigned char negative[] =
        "static void orphan(void) {} int main(void) {}\n";
    static const unsigned char guard[] =
        "static int guarded(const void *p, unsigned long n) { "
        "if (p == 0 || n > 4) return -1; return 0; } "
        "int main(void) { return guarded((void *)1, 1); }\n";
    static const unsigned char asserted_guard[] =
        "static int guarded(const void *p) { "
        "if (!require(p != 0)) return -1; return 0; } "
        "int main(void) { return guarded((void *)1); }\n";
    static const unsigned char effectful_guard[] =
        "static int probe(void) { return 1; } "
        "static int guarded(void) { if (probe()) return -1; return 0; } "
        "int main(void) { return guarded(); }\n";
    static const unsigned char checked_return[] =
        "static int value(void) { return 1; } "
        "int main(void) { (void)value(); return 0; }\n";
    static const unsigned char unchecked_return[] =
        "static int value(void) { return 1; } "
        "int main(void) { value(); return 0; }\n";
    static gsh_canon_call_graph graph;
    size_t unreachable;
    size_t direct;
    size_t recursive;
    size_t oversized;
    size_t assertion_deficit;
    size_t unchecked_returns;
    size_t unvalidated_parameters;

    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(&graph, positive,
                                 sizeof(positive) - 1U,
                                 "fixture/positive.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns,
            &unvalidated_parameters, false) == -1 ||
        unreachable != 0U || direct != 0U || recursive != 0U ||
        oversized != 0U || assertion_deficit != 4U ||
        unchecked_returns != 0U || unvalidated_parameters != 0U) {
        (void)fputs("source policy: positive call-graph fixture rejected\n",
              stderr);
        return 1;
    }
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(&graph, negative,
                                 sizeof(negative) - 1U,
                                 "fixture/negative.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns,
            &unvalidated_parameters, false) == -1 ||
        unreachable != 1U || direct != 0U || recursive != 0U ||
        oversized != 0U || assertion_deficit != 4U ||
        unchecked_returns != 0U || unvalidated_parameters != 0U) {
        (void)fputs("source policy: negative call-graph fixture not detected\n",
              stderr);
        return 1;
    }
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(&graph, guard, sizeof(guard) - 1U,
                                 "fixture/guard.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns,
            &unvalidated_parameters, false) == -1 ||
        unreachable != 0U || assertion_deficit != 2U ||
        unvalidated_parameters != 0U) {
        (void)fputs("source policy: recoverable guard fixture rejected\n", stderr);
        return 1;
    }
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(
            &graph, asserted_guard, sizeof(asserted_guard) - 1U,
            "fixture/asserted-guard.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns,
            &unvalidated_parameters, false) == -1 ||
        unreachable != 0U || assertion_deficit != 3U) {
        (void)fputs("source policy: asserted guard counted twice\n", stderr);
        return 1;
    }
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(
            &graph, effectful_guard, sizeof(effectful_guard) - 1U,
            "fixture/effectful-guard.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns,
            &unvalidated_parameters, false) == -1 ||
        unreachable != 0U || assertion_deficit != 6U) {
        (void)fputs("source policy: effectful guard accepted as assertion\n",
              stderr);
        return 1;
    }
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(
            &graph, checked_return, sizeof(checked_return) - 1U,
            "fixture/checked-return.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns,
            &unvalidated_parameters, false) == -1 ||
        unreachable != 0U || unchecked_returns != 0U) {
        (void)fputs("source policy: explicit ignored return rejected\n", stderr);
        return 1;
    }
    gsh_canon_call_graph_initialize(&graph);
    if (gsh_canon_call_graph_add(
            &graph, unchecked_return, sizeof(unchecked_return) - 1U,
            "fixture/unchecked-return.c") == -1 ||
        gsh_canon_call_graph_analyze(
            &graph, &unreachable, &direct, &recursive, &oversized,
            &assertion_deficit, &unchecked_returns,
            &unvalidated_parameters, false) == -1 ||
        unreachable != 0U || unchecked_returns != 1U) {
        (void)fputs("source policy: ignored return fixture not detected\n", stderr);
        return 1;
    }
    return parameter_call_graph_fixture_test();
}

/* ── Declarations Must Earn Their Place ─────────────────────────
 * Compiler warnings do not diagnose every unused field, public type, enum,
 * or macro, especially when a declaration lives in a shared header.  These
 * fixtures prove that the bounded repository inventory ignores prose, accepts
 * declarations with a real token use, and rejects each declaration class when
 * only its defining occurrence exists.  This closes the obvious dead-symbol
 * gap without treating naming conventions as evidence of reachability.
 * ─────────────────────────────────────────────────────────────── */
static int symbol_fixture_test(void)
{
    static const unsigned char positive[] =
        "#define LIVE_MACRO 1\n"
        "typedef struct { int live_field; } live_type;\n"
        "enum { LIVE_ENUM = 2 };\n"
        "int use(live_type *value) { "
        "return value->live_field + LIVE_MACRO + LIVE_ENUM; }\n";
    static const unsigned char negative[] =
        "#define DEAD_MACRO 1\n"
        "typedef struct { int dead_field; } dead_type;\n"
        "enum { DEAD_ENUM = 2 };\n";
    static gsh_canon_symbol_inventory inventory;
    size_t unused = 0U;

    gsh_canon_symbols_initialize(&inventory);
    if (gsh_canon_symbols_add(&inventory, positive,
                              sizeof(positive) - 1U,
                              "fixture/symbol-positive.c") == -1 ||
        gsh_canon_symbols_analyze(&inventory, &unused, false) == -1 ||
        unused != 0U) {
        (void)fputs("source policy: live declaration fixture rejected\n", stderr);
        return 1;
    }
    gsh_canon_symbols_initialize(&inventory);
    if (gsh_canon_symbols_add(&inventory, negative,
                              sizeof(negative) - 1U,
                              "fixture/symbol-negative.c") == -1 ||
        gsh_canon_symbols_analyze(&inventory, &unused, false) == -1 ||
        unused != 4U) {
        (void)fputs("source policy: unused declaration fixture not detected\n",
              stderr);
        return 1;
    }
    return 0;
}

static void record_link_map_name(
    const gsh_canon_call_graph *graph, link_map_evidence *evidence,
    const char *name, size_t length, link_map_state state)
{
    char normalized[GSH_CANON_NAME_CAP];
    size_t function;
    size_t used = 0U;

    if (!require(graph != NULL && evidence != NULL)) return;
    if (!require(name != NULL &&
                 graph->function_count <= GSH_CANON_FUNCTION_CAP)) return;
    if ((state == LINK_MAP_DARWIN_LIVE ||
         state == LINK_MAP_DARWIN_DISCARDED) && length > 0U &&
        name[0] == '_') {
        name++;
        length--;
    }
    while (used < length && used + 1U < sizeof(normalized) &&
           identifier_continue((unsigned char)name[used])) {
        normalized[used] = name[used];
        used++;
    }
    if (used == 0U ||
        (used + 1U == sizeof(normalized) && used < length &&
         identifier_continue((unsigned char)name[used]))) return;
    if (used < length && name[used] != ' ' && name[used] != '\t' &&
        name[used] != '\r' && name[used] != '\n' &&
        strncmp(name + used, ".cold.", 6U) != 0 &&
        strncmp(name + used, ".constprop.", 11U) != 0 &&
        strncmp(name + used, ".isra.", 6U) != 0 &&
        strncmp(name + used, ".part.", 6U) != 0) return;
    normalized[used] = '\0';
    for (function = 0U; function < graph->function_count; function++) {
        if (strcmp(normalized, graph->functions[function].name) == 0) {
            if (state == LINK_MAP_DARWIN_LIVE ||
                state == LINK_MAP_LINUX_LIVE) {
                evidence->live[function] = true;
            } else {
                evidence->discarded[function] = true;
            }
        }
    }
}

static void consume_link_map_line(
    const gsh_canon_call_graph *graph, link_map_evidence *evidence,
    const char *line, link_map_state *state)
{
    static const char text_prefix[] = ".text.";
    const char *token;
    const char *cursor;

    if (!require(graph != NULL && evidence != NULL && line != NULL)) return;
    if (!require(state != NULL)) return;
    if (strcmp(line, "# Symbols:") == 0) {
        *state = LINK_MAP_DARWIN_LIVE;
        return;
    }
    if (strcmp(line, "# Dead Stripped Symbols:") == 0) {
        *state = LINK_MAP_DARWIN_DISCARDED;
        return;
    }
    if (strcmp(line, "Discarded input sections") == 0) {
        *state = LINK_MAP_LINUX_DISCARDED;
        return;
    }
    if (strcmp(line, "Memory Configuration") == 0) {
        *state = LINK_MAP_UNKNOWN;
        return;
    }
    if (strcmp(line, "Linker script and memory map") == 0) {
        *state = LINK_MAP_LINUX_LIVE;
        return;
    }
    if (*state == LINK_MAP_DARWIN_LIVE ||
        *state == LINK_MAP_DARWIN_DISCARDED) {
        token = strrchr(line, ' ');
        token = token == NULL ? line : token + 1U;
        if (token[0] == '_') {
            record_link_map_name(graph, evidence, token, strlen(token),
                                 *state);
        }
        return;
    }
    if (*state != LINK_MAP_LINUX_LIVE &&
        *state != LINK_MAP_LINUX_DISCARDED) return;
    cursor = strstr(line, text_prefix);
    if (cursor == NULL) return;
    cursor += sizeof(text_prefix) - 1U;
    if (strncmp(cursor, "startup.", 8U) == 0) cursor += 8U;
    if (strncmp(cursor, "unlikely.", 9U) == 0) cursor += 9U;
    if (strncmp(cursor, "hot.", 4U) == 0) cursor += 4U;
    record_link_map_name(graph, evidence, cursor, strlen(cursor), *state);
}

static int scan_linker_map(const char *path,
                           const gsh_canon_call_graph *graph,
                           link_map_evidence *evidence)
{
    unsigned char chunk[8192];
    char line[POLICY_MAP_LINE_CAP];
    link_map_state state = LINK_MAP_UNKNOWN;
    size_t line_length = 0U;
    size_t total = 0U;
    int descriptor;

    if (!require(path != NULL && graph != NULL)) return -1;
    if (!require(evidence != NULL)) return -1;
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor == -1) return -1;
    while (total < POLICY_MAP_READ_CAP) {
        ssize_t count = read(descriptor, chunk, sizeof(chunk));
        size_t index;

        if (count == -1 && errno == EINTR) continue;
        if (count <= 0) {
            int close_status;

            if (count == -1) {
                (void)close(descriptor);
                return -1;
            }
            if (line_length != 0U) {
                line[line_length] = '\0';
                consume_link_map_line(graph, evidence, line, &state);
            }
            close_status = close(descriptor);
            if (close_status == -1 || state == LINK_MAP_UNKNOWN) {
                if (close_status == 0) errno = EPROTO;
                return -1;
            }
            return 0;
        }
        total += (size_t)count;
        for (index = 0U; index < (size_t)count; index++) {
            if (chunk[index] == '\n') {
                line[line_length] = '\0';
                consume_link_map_line(graph, evidence, line, &state);
                line_length = 0U;
            } else if (line_length + 1U < sizeof(line)) {
                line[line_length++] = (char)chunk[index];
            } else {
                (void)close(descriptor);
                errno = EOVERFLOW;
                return -1;
            }
        }
    }
    (void)close(descriptor);
    errno = EFBIG;
    return -1;
}

static size_t count_discarded_link_functions(
    const gsh_canon_call_graph *graph, const link_map_evidence *evidence,
    bool report)
{
    size_t function;
    size_t discarded = 0U;

    if (!require(graph != NULL && evidence != NULL)) {
        return GSH_CANON_FUNCTION_CAP + 1U;
    }
    if (!require(graph->function_count <= GSH_CANON_FUNCTION_CAP)) {
        return GSH_CANON_FUNCTION_CAP + 1U;
    }
    for (function = 0U; function < graph->function_count; function++) {
        if (evidence->discarded[function] && !evidence->live[function]) {
            if (report) {
                (void)fprintf(stderr,
                        "source policy: function discarded by every target: "
                        "%s (%s)\n",
                        graph->functions[function].name,
                        graph->files[graph->functions[function].file]);
            }
            discarded++;
        }
    }
    return discarded;
}

static int link_map_fixture_test(void)
{
    static gsh_canon_call_graph graph;
    link_map_evidence evidence = {{false}, {false}};
    link_map_state state = LINK_MAP_UNKNOWN;
    char darwin_live[] = "0x1 0x1 [ 1] _live";
    char darwin_dead[] = "<<dead>> 0x1 [ 1] _orphan";
    char darwin_local_data[] =
        "<<dead>> 0x1 [ 1] _data_owner.metadata";
    char linux_live[] =
        " .text.startup.linux_live.part.0 0x1 0x1 /tmp/a.o";

    (void)memset(&graph, 0, sizeof(graph));
    graph.function_count = 4U;
    (void)memcpy(graph.functions[0].name, "live", sizeof("live"));
    (void)memcpy(graph.functions[1].name, "orphan", sizeof("orphan"));
    (void)memcpy(graph.functions[2].name, "linux_live", sizeof("linux_live"));
    (void)memcpy(graph.functions[3].name, "data_owner", sizeof("data_owner"));
    consume_link_map_line(&graph, &evidence, "# Symbols:", &state);
    consume_link_map_line(&graph, &evidence, darwin_live, &state);
    consume_link_map_line(&graph, &evidence, "# Dead Stripped Symbols:",
                          &state);
    consume_link_map_line(&graph, &evidence, darwin_dead, &state);
    consume_link_map_line(&graph, &evidence, darwin_local_data, &state);
    consume_link_map_line(&graph, &evidence,
                          "Linker script and memory map", &state);
    consume_link_map_line(&graph, &evidence, linux_live, &state);
    if (!evidence.live[0] || !evidence.discarded[1] ||
        !evidence.live[2] || evidence.discarded[3] ||
        count_discarded_link_functions(&graph, &evidence, false) != 1U) {
        (void)fputs("source policy: linker-map fixtures failed\n", stderr);
        return 1;
    }
    return 0;
}

static int validate_linker_maps(const gsh_canon_call_graph *graph,
                                size_t count, char *const paths[])
{
    static link_map_evidence evidence;
    size_t index;

    if (!require(graph != NULL && paths != NULL)) return 1;
    if (!require(count > 0U)) return 1;
    (void)memset(&evidence, 0, sizeof(evidence));
    for (index = 0U; index < count; index++) {
        if (scan_linker_map(paths[index], graph, &evidence) == -1) {
            (void)fprintf(stderr, "source policy: cannot inspect linker map: %s\n",
                    paths[index]);
            return 1;
        }
    }
    return count_discarded_link_functions(graph, &evidence, true) == 0U
               ? 0
               : 1;
}

static size_t manifest_target_index(const char *name, size_t length)
{
    size_t index;

    if (!require(name != NULL)) return SIZE_MAX;
    if (!require(length > 0U && length < 256U)) return SIZE_MAX;
    for (index = 0U;
         index < sizeof(MANIFEST_TARGETS) / sizeof(MANIFEST_TARGETS[0]);
         index++) {
        if (strlen(MANIFEST_TARGETS[index]) == length &&
            memcmp(MANIFEST_TARGETS[index], name, length) == 0) {
            return index;
        }
    }
    return SIZE_MAX;
}

static bool manifest_role_is_root(const char *role, bool *root)
{
    if (!require(role != NULL)) return false;
    if (!require(root != NULL)) return false;
    if (strcmp(role, "dependency") == 0) {
        *root = false;
        return true;
    }
    if (strcmp(role, "main") == 0 ||
        strcmp(role, "LLVMFuzzerTestOneInput,main") == 0) {
        *root = true;
        return true;
    }
    return false;
}

static bool record_manifest_targets(
    const char *targets, bool root,
    bool mentioned[sizeof(MANIFEST_TARGETS) / sizeof(MANIFEST_TARGETS[0])],
    bool rooted[sizeof(MANIFEST_TARGETS) / sizeof(MANIFEST_TARGETS[0])])
{
    if (!require(targets != NULL && mentioned != NULL)) return false;
    if (!require(rooted != NULL && targets[0] != '\0')) return false;
    const char *cursor = targets;
    size_t components;

    for (components = 0U;
         components < sizeof(MANIFEST_TARGETS) /
                          sizeof(MANIFEST_TARGETS[0]);
         components++) {
        const char *separator = strchr(cursor, ',');
        size_t length = separator == NULL
                            ? strlen(cursor)
                            : (size_t)(separator - cursor);
        size_t index = manifest_target_index(cursor, length);

        if (index == SIZE_MAX) return false;
        mentioned[index] = true;
        rooted[index] = rooted[index] || root;
        if (separator == NULL) return true;
        cursor = separator + 1U;
        if (*cursor == '\0') return false;
    }
    return false;
}

static int manifest_target_fixture_test(void)
{
    bool mentioned[sizeof(MANIFEST_TARGETS) /
                   sizeof(MANIFEST_TARGETS[0])] = {false};
    bool rooted[sizeof(MANIFEST_TARGETS) /
                sizeof(MANIFEST_TARGETS[0])] = {false};
    size_t gsh_index = manifest_target_index("gsh", 3U);
    size_t fault_index = manifest_target_index("fault", 5U);
    bool root;

    if (!require(gsh_index != SIZE_MAX)) return 1;
    if (!require(fault_index != SIZE_MAX)) return 1;
    if (!manifest_role_is_root("main", &root) || !root ||
        !record_manifest_targets("gsh,fault", root, mentioned, rooted) ||
        !mentioned[gsh_index] || !rooted[fault_index]) {
        (void)fputs("source policy: valid target manifest fixture rejected\n",
              stderr);
        return 1;
    }
    if (record_manifest_targets("orphan-target", false, mentioned,
                                rooted) ||
        manifest_role_is_root("future", &root)) {
        (void)fputs("source policy: invalid target manifest fixture accepted\n",
              stderr);
        return 1;
    }
    return 0;
}

static int manifest_targets_complete(
    const bool mentioned[sizeof(MANIFEST_TARGETS) /
                         sizeof(MANIFEST_TARGETS[0])],
    const bool rooted[sizeof(MANIFEST_TARGETS) /
                      sizeof(MANIFEST_TARGETS[0])])
{
    size_t target;

    if (!require(mentioned != NULL && rooted != NULL)) return 0;
    for (target = 0U;
         target < sizeof(MANIFEST_TARGETS) / sizeof(MANIFEST_TARGETS[0]);
         target++) {
        if (!mentioned[target] || !rooted[target]) {
            (void)fprintf(stderr,
                    "source policy: target lacks manifest %s: %s\n",
                    !mentioned[target] ? "membership" : "root",
                    MANIFEST_TARGETS[target]);
            return 0;
        }
    }
    return 1;
}

static int validate_target_manifest(const char *repository_root,
                                    const char *manifest_path,
                                    const gsh_canon_call_graph *graph)
{
    if (graph == NULL || repository_root == NULL) {
        return -1;
    }
    enum { MANIFEST_CAP = 16384 };
    char data[MANIFEST_CAP];
    bool matched[GSH_CANON_FILE_CAP] = {false};
    bool mentioned[sizeof(MANIFEST_TARGETS) /
                   sizeof(MANIFEST_TARGETS[0])] = {false};
    bool rooted[sizeof(MANIFEST_TARGETS) /
                sizeof(MANIFEST_TARGETS[0])] = {false};
    size_t root_length = strlen(repository_root);
    size_t length;
    size_t begin = 0U;
    size_t entries = 0U;

    if (read_manifest_text(manifest_path, data, sizeof(data), &length) ==
        -1) return -1;
    while (begin < length) {
        char source[GSH_CANON_PATH_CAP];
        char target[256];
        char role[256];
        size_t end = begin;
        size_t file;
        bool found = false;
        bool root = false;

        while (end < length && data[end] != '\n') end++;
        data[end] = '\0';
        if (data[begin] != '\0' && data[begin] != '#') {
            if (sscanf(data + begin, "%4095s %255s %255s", source, target,
                       role) != 3 ||
                (strncmp(source, "src/", 4U) != 0 &&
                 strncmp(source, "tests/", 6U) != 0) ||
                !manifest_role_is_root(role, &root) ||
                !record_manifest_targets(target, root, mentioned, rooted)) {
                errno = EINVAL;
                return -1;
            }
            for (file = 0; file < graph->file_count; file++) {
                const char *absolute = graph->files[file];
                const char *relative = absolute;

                if (strncmp(absolute, repository_root, root_length) == 0 &&
                    absolute[root_length] == '/') {
                    relative = absolute + root_length + 1U;
                }
                if (strcmp(source, relative) == 0) {
                    if (matched[file]) {
                        (void)fprintf(stderr,
                                "source policy: duplicate manifest source: %s\n",
                                source);
                        return 1;
                    }
                    matched[file] = true;
                    found = true;
                    entries++;
                    break;
                }
            }
            if (!found) {
                (void)fprintf(stderr,
                        "source policy: manifest source is not executable C: %s\n",
                        source);
                return 1;
            }
        }
        begin = end + 1U;
    }
    if (entries != graph->file_count) {
        size_t file;

        for (file = 0; file < graph->file_count; file++) {
            if (!matched[file]) {
                (void)fprintf(stderr, "source policy: unowned C source: %s\n",
                        graph->files[file]);
            }
        }
        return 1;
    }
    return manifest_targets_complete(mentioned, rooted) ? 0 : 1;
}

static int scan_source_file(const char *path, canon_counts *counts,
                            gsh_canon_call_graph *graph,
                            gsh_canon_symbol_inventory *symbols,
                            bool executable)
{
    if (counts == NULL) return -1;
    if (path == NULL) {
        return -1;
    }
    static unsigned char data[POLICY_FILE_CAP];
    struct stat information;
    size_t length = 0U;
    size_t preprocessor_violations = 0U;
    size_t attempts;
    int descriptor;

    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor == -1 || fstat(descriptor, &information) == -1) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return -1;
    }
    if (information.st_size < 0 ||
        (uintmax_t)information.st_size > (uintmax_t)sizeof(data)) {
        (void)close(descriptor);
        errno = EFBIG;
        return -1;
    }
    for (attempts = 0;
         length < (size_t)information.st_size && attempts <= sizeof(data);
         attempts++) {
        ssize_t count = read(descriptor, data + length,
                             (size_t)information.st_size - length);

        if (count > 0) {
            length += (size_t)count;
        } else if (count != -1 || errno != EINTR) {
            (void)close(descriptor);
            return -1;
        }
    }
    if (close(descriptor) == -1 || length != (size_t)information.st_size) {
        return -1;
    }
    if (gsh_canon_preprocessor_analyze(
            data, length, path, &preprocessor_violations, true) == -1 ||
        SIZE_MAX - counts->preprocessor_violations <
            preprocessor_violations ||
        validate_exception_markers(data, length, path, true) != 0 ||
        scan_c_tokens(data, length, path, counts) != 0 ||
        gsh_canon_symbols_add(symbols, data, length, path) == -1 ||
        (executable &&
         gsh_canon_call_graph_add(graph, data, length, path) == -1)) {
        return -1;
    }
    counts->preprocessor_violations += preprocessor_violations;
    return 0;
}

static int check_c_baseline(const char *path)
{
    if (path == NULL) {
        return -1;
    }
    unsigned char data[2048];
    ssize_t length;
    int fd = open(path, O_RDONLY);

    if (fd == -1) {
        return -1;
    }
    length = read(fd, data, sizeof(data));
    (void)close(fd);
    if (length < 0) {
        return -1;
    }
    if (!buffer_contains(data, (size_t)length, "_POSIX_C_SOURCE") ||
        !buffer_contains(data, (size_t)length, "202405L")) {
        (void)fprintf(stderr, "source policy: missing POSIX.1-2024 baseline: %s\n",
                path);
        return 1;
    }
    return 0;
}

static int push_directory(directory_stack *stack, const char *path)
{
    size_t length;

    if (stack == NULL || path == NULL || stack->count >= POLICY_DIRECTORY_CAP) {
        errno = ENOSPC;
        return -1;
    }
    length = strnlen(path, PATH_MAX);
    if (length == PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)memcpy(stack->paths[stack->count], path, length + 1U);
    stack->count++;
    return 0;
}

static int inspect_source_entry(const char *child,
                                const struct stat *information,
                                directory_stack *stack,
                                canon_counts *counts,
                                gsh_canon_call_graph *graph,
                                gsh_canon_symbol_inventory *symbols)
{
    if (information == NULL) return -1;
    if (child == NULL || stack == NULL) {
        return -1;
    }
    size_t suffix;
    bool c_source;
    bool c_header;
    int failed = 0;

    if (S_ISDIR(information->st_mode)) {
        return push_directory(stack, child);
    }
    if (!S_ISREG(information->st_mode)) {
        (void)fprintf(stderr, "source policy: non-regular source entry: %s\n",
                child);
        return 1;
    }
    for (suffix = 0;
         suffix < sizeof(FORBIDDEN_SUFFIXES) /
                      sizeof(FORBIDDEN_SUFFIXES[0]);
         suffix++) {
        if (has_suffix(child, FORBIDDEN_SUFFIXES[suffix])) {
            (void)fprintf(stderr, "source policy: forbidden first-party language: %s\n",
                    child);
            failed = 1;
        }
    }
    c_source = has_suffix(child, ".c");
    c_header = has_suffix(child, ".h");
    if ((c_source || c_header) &&
        ((c_source && check_c_baseline(child) != 0) ||
         scan_source_file(child, counts, graph, symbols, c_source) != 0)) {
        (void)fprintf(stderr, "source policy: cannot inspect C source: %s\n", child);
        failed = 1;
    }
    return failed;
}

/* ── Directory Discovery Has a Visible Ceiling ──────────────────
 * The original policy scanner called itself for every nested directory, so
 * the verifier violated the same recursion rule it was meant to enforce.
 * A fixed LIFO now owns discovery and rejects a tree deeper than its declared
 * capacity.  Corpus data remains excluded because it is input, not code.
 * Every executable source file is still visited exactly once.
 * ─────────────────────────────────────────────────────────────── */
static int scan_directory(const char *path, canon_counts *counts,
                          gsh_canon_call_graph *graph,
                          gsh_canon_symbol_inventory *symbols)
{
    directory_stack stack = {{{0}}, 0U};
    size_t iterations;
    int failed = 0;

    if (!require(path != NULL)) return -1;
    if (!require(counts != NULL)) return -1;
    if (!require(graph != NULL)) return -1;
    if (!require(symbols != NULL)) return -1;
    if (push_directory(&stack, path) == -1) {
        return errno == ENOENT ? 0 : -1;
    }
    for (iterations = 0;
         stack.count > 0U && iterations < POLICY_DIRECTORY_CAP;
         iterations++) {
        char current[PATH_MAX];
        struct dirent *entry;
        DIR *directory;

        stack.count--;
        (void)memcpy(current, stack.paths[stack.count], sizeof(current));
        directory = opendir(current);
        if (directory == NULL) {
            if (errno != ENOENT) {
                failed = 1;
            }
            continue;
        }
        while ((entry = readdir(directory)) != NULL) {
            char child[PATH_MAX];
            struct stat information;
            int length;

            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0 ||
                strcmp(entry->d_name, "corpus") == 0) {
                continue;
            }
            length = snprintf(child, sizeof(child), "%s/%s", current,
                              entry->d_name);
            if (length < 0 || (size_t)length >= sizeof(child) ||
                lstat(child, &information) == -1 ||
                inspect_source_entry(child, &information, &stack, counts,
                                     graph, symbols) != 0) {
                failed = 1;
            }
        }
        if (closedir(directory) == -1) {
            failed = 1;
        }
    }
    if (stack.count != 0U) {
        (void)fprintf(stderr, "source policy: directory traversal exceeded %d\n",
                POLICY_DIRECTORY_CAP);
        failed = 1;
    }
    return failed;
}

static int binary_excludes_fault_injection(const char *path)
{
    if (path == NULL) {
        return -1;
    }
    enum {
        BINARY_CHUNK_SIZE = 8192,
        BINARY_OVERLAP_SIZE = 128,
    };
    static const char *const forbidden[] = {
        "GSH_FAULT", "job-fork",
        "terminal-handoff", "pipeline-pipe", "pipeline-fork",
        "descriptor-dup", "redirect-open", "evaluator-gate",
        "evaluator-fork", "subshell-fork", "time-source-failure",
        "heredoc-pipe",
        "heredoc-fork", "heredoc-write", "substitution-pipe",
        "substitution-fork", "substitution-read",
        "source-workspace-exhaustion", "state-commit-pipe",
        "state-commit-write", "state-commit-read",
        "state-commit-malformed", "state-control-commit-malformed",
        "positional-allocation",
        "positional-commit-allocation",
        "positional-commit-malformed",
        "alias-allocation", "alias-transaction-allocation",
        "alias-commit-malformed",
    };
    unsigned char data[BINARY_CHUNK_SIZE + BINARY_OVERLAP_SIZE];
    size_t overlap_length = 0;
    int fd = open(path, O_RDONLY);

    if (fd == -1) {
        return -1;
    }
    size_t chunk_index;

    for (chunk_index = 0; chunk_index < POLICY_BINARY_CHUNK_CAP;
         chunk_index++) {
        ssize_t count = read(fd, data + overlap_length, BINARY_CHUNK_SIZE);
        size_t total;
        size_t index;

        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            (void)close(fd);
            return count == 0 ? 0 : -1;
        }
        total = overlap_length + (size_t)count;
        for (index = 0; index < sizeof(forbidden) / sizeof(forbidden[0]);
             index++) {
            if (buffer_contains(data, total, forbidden[index])) {
                (void)fprintf(stderr,
                        "source policy: production binary contains fault "
                        "hook %s\n",
                        forbidden[index]);
                (void)close(fd);
                return 1;
            }
        }
        overlap_length = total < BINARY_OVERLAP_SIZE
                             ? total
                             : BINARY_OVERLAP_SIZE;
        (void)memmove(data, data + total - overlap_length, overlap_length);
    }
    (void)close(fd);
    errno = EFBIG;
    return -1;
}

int main(int argc, char **argv)
{
    static gsh_canon_call_graph graph;
    static gsh_canon_symbol_inventory symbols;
    char path[PATH_MAX];
    static const char *const directories[] = {"src", "tests", "tools",
                                               "bench"};
    canon_counts actual = {0};
    const canon_counts required = {.approved_function_pointers = 1U};
    size_t map_begin = 0U;
    size_t option = 4U;
    size_t index;
    bool verbose = false;
    int failed = 0;

    if (!require(argv != NULL)) return 2;
    if (!require(argc > 0 && argv[0] != NULL)) return 2;
    if (argc < 4) {
        (void)fprintf(stderr,
                "usage: source-policy repository-root gsh-binary manifest "
                "[--verbose] [--maps map...]\n");
        return 2;
    }
    if (option < (size_t)argc && strcmp(argv[option], "--verbose") == 0) {
        verbose = true;
        option++;
    }
    if (option < (size_t)argc && strcmp(argv[option], "--maps") == 0) {
        map_begin = ++option;
    }
    if (option != (size_t)argc && map_begin == 0U) {
        (void)fputs("source policy: invalid option sequence\n", stderr);
        return 2;
    }
    if (map_begin != 0U && map_begin == (size_t)argc) {
        (void)fputs("source policy: --maps requires at least one map\n", stderr);
        return 2;
    }
    if (lexer_fixture_test() != 0 || preprocessor_fixture_test() != 0 ||
        exception_fixture_test() != 0 ||
        call_graph_fixture_test() != 0 || symbol_fixture_test() != 0 ||
        link_map_fixture_test() != 0 ||
        manifest_target_fixture_test() != 0) {
        (void)fputs("source policy: internal fixture failure\n", stderr);
        return 2;
    }
    gsh_canon_call_graph_initialize(&graph);
    gsh_canon_symbols_initialize(&symbols);
    for (index = 0; index < sizeof(directories) / sizeof(directories[0]);
         index++) {
        if (snprintf(path, sizeof(path), "%s/%s", argv[1],
                     directories[index]) >= (int)sizeof(path) ||
            scan_directory(path, &actual, &graph, &symbols) != 0) {
            failed = 1;
        }
    }
    if (gsh_canon_call_graph_analyze(
            &graph, &actual.unreachable_functions,
            &actual.direct_recursive_calls,
            &actual.recursive_functions, &actual.oversized_functions,
            &actual.assertion_deficit, &actual.unchecked_returns,
            &actual.unvalidated_parameters, verbose) == -1) {
        (void)fputs("source policy: call graph analysis failed\n", stderr);
        failed = 1;
    }
    if (gsh_canon_symbols_analyze(
            &symbols, &actual.unused_declarations, verbose) == -1) {
        (void)fputs("source policy: symbol inventory analysis failed\n", stderr);
        failed = 1;
    }
    if (validate_target_manifest(argv[1], argv[3], &graph) != 0) {
        failed = 1;
    }
    if (map_begin != 0U &&
        validate_linker_maps(&graph, (size_t)argc - map_begin,
                             argv + map_begin) != 0) {
        failed = 1;
    }
    if (!canon_counts_equal(&actual, &required)) {
        (void)fprintf(stderr,
                "source policy: Code Canon violation count is nonzero "
                "(goto=%zu forbidden=%zu callbacks=%zu approved=%zu "
                "#if0=%zu constant-branches=%zu unreachable=%zu "
                "direct-recursion=%zu recursive=%zu oversized=%zu "
                "assertion-deficit=%zu unbounded-loops=%zu unused=%zu "
                "nested-dereferences=%zu mutable-globals=%zu "
                "preprocessor=%zu unchecked-returns=%zu "
                "unvalidated-parameters=%zu)\n",
                actual.goto_statements, actual.forbidden_calls,
                actual.function_pointers,
                actual.approved_function_pointers,
                actual.disabled_blocks, actual.constant_branches,
                actual.unreachable_functions,
                actual.direct_recursive_calls,
                actual.recursive_functions, actual.oversized_functions,
                actual.assertion_deficit, actual.unbounded_loops,
                actual.unused_declarations, actual.nested_dereferences,
                actual.mutable_globals, actual.preprocessor_violations,
                actual.unchecked_returns, actual.unvalidated_parameters);
        failed = 1;
    }
    if (binary_excludes_fault_injection(argv[2]) != 0) {
        failed = 1;
    }
    if (failed) {
        return 1;
    }
    (void)printf("source policy: permanent Code Canon gate passed "
           "(goto=%zu forbidden=%zu callbacks=%zu approved=%zu #if0=%zu "
           "constant-branches=%zu unreachable=%zu direct-recursion=%zu "
           "recursive=%zu oversized=%zu assertion-deficit=%zu "
           "unbounded-loops=%zu unused=%zu nested-dereferences=%zu "
           "mutable-globals=%zu preprocessor=%zu unchecked-returns=%zu "
           "unvalidated-parameters=%zu); "
           "C/header sources and production fault isolation passed\n",
           actual.goto_statements, actual.forbidden_calls,
           actual.function_pointers, actual.approved_function_pointers,
           actual.disabled_blocks, actual.constant_branches,
           actual.unreachable_functions,
           actual.direct_recursive_calls, actual.recursive_functions,
           actual.oversized_functions, actual.assertion_deficit,
           actual.unbounded_loops, actual.unused_declarations,
           actual.nested_dereferences, actual.mutable_globals,
           actual.preprocessor_violations, actual.unchecked_returns,
           actual.unvalidated_parameters);
    return 0;
}
