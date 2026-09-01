#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "native_plan.h"

#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum {
    GSH_WORD_EXPANDED = 1U << 0,
    GSH_WORD_QUOTED = 1U << 1,
    GSH_WORD_EMPTY_MARKER = 1U << 2,
    GSH_WORD_FIELD_BOUNDARY = 1U << 3,
};

typedef enum {
    GSH_EXPAND_SCALAR,
    GSH_EXPAND_FIELDS,
    GSH_EXPAND_ARITHMETIC,
} gsh_expansion_mode;

static bool node_has_only_child(const gsh_parse_storage *storage,
                                const gsh_ast_node *node,
                                gsh_ast_kind child_kind, size_t *child)
{
    const gsh_ast_node *candidate;

    if (node->first_child == GSH_AST_NONE) {
        return false;
    }
    candidate = &storage->nodes[node->first_child];
    if (candidate->kind != child_kind ||
        candidate->next_sibling != GSH_AST_NONE) {
        return false;
    }
    *child = node->first_child;
    return true;
}

static void reset_pipeline(gsh_native_pipeline *pipeline)
{
    pipeline->command_count = 0;
    pipeline->heredoc_count = 0;
    pipeline->negated = false;
    pipeline->text_used = 0;
    pipeline->heredoc_text_used = 0;
}

static bool reserve_byte(gsh_native_pipeline *pipeline, unsigned char byte)
{
    size_t offset = pipeline->text_used;

    if (offset >= GSH_NATIVE_TEXT_CAP) {
        return false;
    }
    pipeline->text[offset] = (char)byte;
    pipeline->provenance[offset] = 0;
    pipeline->text_used = offset + 1U;
    return true;
}

static void mark_reserved(gsh_native_pipeline *pipeline, size_t begin,
                          unsigned char provenance)
{
    while (begin < pipeline->text_used) {
        pipeline->provenance[begin++] = provenance;
    }
}

static bool reserve_empty_marker(gsh_native_pipeline *pipeline)
{
    size_t offset = pipeline->text_used;

    if (!reserve_byte(pipeline, 0)) {
        return false;
    }
    pipeline->provenance[offset] =
        GSH_WORD_QUOTED | GSH_WORD_EMPTY_MARKER;
    return true;
}

static bool reserve_positional_marker(gsh_native_pipeline *pipeline,
                                      bool boundary, bool quoted)
{
    size_t offset = pipeline->text_used;

    if (!reserve_byte(pipeline, 0)) {
        return false;
    }
    pipeline->provenance[offset] =
        GSH_WORD_EXPANDED |
        (boundary ? GSH_WORD_FIELD_BOUNDARY : GSH_WORD_EMPTY_MARKER) |
        (quoted ? GSH_WORD_QUOTED : 0);
    return true;
}

static bool reserve_decimal(gsh_native_pipeline *pipeline,
                            unsigned long value)
{
    unsigned char digits[32];
    size_t count = 0;

    do {
        digits[count++] = (unsigned char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) {
        if (!reserve_byte(pipeline, digits[--count])) {
            return false;
        }
    }
    return true;
}

static int hexadecimal_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') {
        return (int)(byte - '0');
    }
    if (byte >= 'a' && byte <= 'f') {
        return 10 + (int)(byte - 'a');
    }
    if (byte >= 'A' && byte <= 'F') {
        return 10 + (int)(byte - 'A');
    }
    return -1;
}

static gsh_native_plan_status expand_dollar_single_quote(
    const char *input, size_t end, size_t *offset,
    gsh_native_pipeline *pipeline)
{
    bool closed = false;

    while (*offset < end) {
        unsigned char byte = (unsigned char)input[(*offset)++];

        if (byte == '\'') {
            closed = true;
            break;
        }
        if (byte == '\\') {
            unsigned int value;
            unsigned char escape;

            if (*offset == end) {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            escape = (unsigned char)input[(*offset)++];
            if (escape == '"' || escape == '\'' || escape == '\\') {
                byte = escape;
            } else if (escape == 'a') {
                byte = '\a';
            } else if (escape == 'b') {
                byte = '\b';
            } else if (escape == 'e') {
                byte = 0x1bU;
            } else if (escape == 'f') {
                byte = '\f';
            } else if (escape == 'n') {
                byte = '\n';
            } else if (escape == 'r') {
                byte = '\r';
            } else if (escape == 't') {
                byte = '\t';
            } else if (escape == 'v') {
                byte = '\v';
            } else if (escape == 'c') {
                unsigned char control;

                if (*offset == end) {
                    return GSH_NATIVE_PLAN_UNSUPPORTED;
                }
                control = (unsigned char)input[(*offset)++];
                if (control >= '@' && control <= '_') {
                    byte = (unsigned char)(control - '@');
                } else if (control == '?') {
                    byte = 0x7fU;
                } else {
                    return GSH_NATIVE_PLAN_UNSUPPORTED;
                }
            } else if (escape == 'x') {
                size_t digits = 0;
                int digit;

                value = 0;
                while (*offset < end && digits < 2U &&
                       (digit = hexadecimal_digit(
                            (unsigned char)input[*offset])) >= 0) {
                    value = value * 16U + (unsigned int)digit;
                    (*offset)++;
                    digits++;
                }
                if (digits == 0) {
                    return GSH_NATIVE_PLAN_UNSUPPORTED;
                }
                byte = (unsigned char)value;
            } else if (escape >= '0' && escape <= '7') {
                size_t digits = 1;

                value = (unsigned int)(escape - '0');
                while (*offset < end && digits < 3U &&
                       input[*offset] >= '0' && input[*offset] <= '7') {
                    value = value * 8U +
                            (unsigned int)(input[*offset] - '0');
                    (*offset)++;
                    digits++;
                }
                if (value > 255U) {
                    return GSH_NATIVE_PLAN_UNSUPPORTED;
                }
                byte = (unsigned char)value;
            } else {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
        }
        if (byte == '\0') {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        if (!reserve_byte(pipeline, byte)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
    }
    return closed ? GSH_NATIVE_PLAN_OK : GSH_NATIVE_PLAN_UNSUPPORTED;
}

static bool name_start(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') || byte == '_';
}

static bool name_byte(unsigned char byte)
{
    return name_start(byte) || (byte >= '0' && byte <= '9');
}

static bool environment_name_matches(const char *entry, const char *name,
                                     size_t name_length)
{
    size_t offset;

    for (offset = 0; offset < name_length; offset++) {
        if (entry[offset] == '\0' || entry[offset] != name[offset]) {
            return false;
        }
    }
    return entry[name_length] == '=';
}

static const char *lookup_variable(
    const gsh_native_expansion_context *context, const char *name,
    size_t name_length, bool *found)
{
    size_t index;

    *found = false;
    if (context->variable_lookup != NULL) {
        return context->variable_lookup(context->variable_opaque, name,
                                        name_length, found);
    }
    if (context->environment == NULL) {
        return "";
    }
    for (index = 0; index < GSH_NATIVE_ENVIRONMENT_CAP; index++) {
        const char *entry = context->environment[index];

        if (entry == NULL) {
            return "";
        }
        if (environment_name_matches(entry, name, name_length)) {
            *found = true;
            return entry + name_length + 1U;
        }
    }
    return NULL;
}

static gsh_native_plan_status reserve_parameter_value(
    gsh_native_pipeline *pipeline, const char *value)
{
    while (*value != '\0') {
        if (!reserve_byte(pipeline, (unsigned char)*value++)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status find_substitution_end(
    const char *input, size_t end, size_t opening_parenthesis,
    bool arithmetic, size_t *contents_end, size_t *after);
static gsh_native_plan_status
expand_static_word(const char *input, gsh_word_ref word,
                   const gsh_native_expansion_context *context,
                   gsh_expansion_mode mode, size_t depth,
                   gsh_native_pipeline *pipeline, char **expanded,
                   size_t *expanded_length);

static gsh_native_plan_status find_parameter_end(
    const char *input, size_t end, size_t opening_brace, size_t *closing)
{
    enum {
        PARAMETER_SCAN_NONE,
        PARAMETER_SCAN_SINGLE,
        PARAMETER_SCAN_DOUBLE,
        PARAMETER_SCAN_DOLLAR_SINGLE,
    } quote = PARAMETER_SCAN_NONE;
    size_t nesting = 1;
    size_t offset = opening_brace + 1U;

    while (offset < end) {
        unsigned char byte = (unsigned char)input[offset];

        if (byte == '\0') {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        if (quote == PARAMETER_SCAN_SINGLE) {
            offset++;
            if (byte == '\'') {
                quote = PARAMETER_SCAN_NONE;
            }
            continue;
        }
        if (quote == PARAMETER_SCAN_DOLLAR_SINGLE) {
            if (byte == '\\' && offset + 1U < end) {
                offset += 2U;
            } else {
                offset++;
                if (byte == '\'') {
                    quote = PARAMETER_SCAN_NONE;
                }
            }
            continue;
        }
        if (byte == '\\' && offset + 1U < end) {
            offset += 2U;
            continue;
        }
        if (quote == PARAMETER_SCAN_DOUBLE && byte == '"') {
            quote = PARAMETER_SCAN_NONE;
            offset++;
            continue;
        }
        if (quote == PARAMETER_SCAN_NONE && byte == '\'') {
            quote = PARAMETER_SCAN_SINGLE;
            offset++;
            continue;
        }
        if (quote == PARAMETER_SCAN_NONE && byte == '"') {
            quote = PARAMETER_SCAN_DOUBLE;
            offset++;
            continue;
        }
        if (quote == PARAMETER_SCAN_NONE && byte == '$' &&
            offset + 1U < end &&
            input[offset + 1U] == '\'') {
            quote = PARAMETER_SCAN_DOLLAR_SINGLE;
            offset += 2U;
            continue;
        }
        if (byte == 0x60U) {
            offset++;
            while (offset < end) {
                byte = (unsigned char)input[offset++];
                if (byte == '\\' && offset < end) {
                    offset++;
                } else if (byte == 0x60U) {
                    break;
                }
            }
            if (byte != 0x60U) {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            continue;
        }
        if (byte == '$' && offset + 1U < end &&
            input[offset + 1U] == '(') {
            size_t contents_end;
            size_t after;
            bool arithmetic = offset + 2U < end &&
                              input[offset + 2U] == '(';
            gsh_native_plan_status status = find_substitution_end(
                input, end, offset + 1U, arithmetic, &contents_end, &after);

            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
            offset = after;
            continue;
        }
        if (byte == '$' && offset + 1U < end &&
            input[offset + 1U] == '{') {
            if (nesting == 128U) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            nesting++;
            offset += 2U;
            continue;
        }
        if (byte == '}') {
            nesting--;
            if (nesting == 0) {
                *closing = offset;
                return GSH_NATIVE_PLAN_OK;
            }
        }
        offset++;
    }
    return GSH_NATIVE_PLAN_UNSUPPORTED;
}

static void mark_expansion(gsh_native_pipeline *pipeline, size_t begin,
                           bool quoted)
{
    unsigned char provenance = GSH_WORD_EXPANDED |
                               (quoted ? GSH_WORD_QUOTED : 0);

    while (begin < pipeline->text_used) {
        pipeline->provenance[begin++] |= provenance;
    }
}

static size_t decimal_text(unsigned long value, char output[32])
{
    char reversed[32];
    size_t count = 0;
    size_t index;

    do {
        reversed[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0);
    for (index = 0; index < count; index++) {
        output[index] = reversed[count - index - 1U];
    }
    output[count] = '\0';
    return count;
}

typedef enum {
    GSH_PARAMETER_VALUE,
    GSH_PARAMETER_AT,
    GSH_PARAMETER_STAR,
} gsh_parameter_kind;

static gsh_native_plan_status parameter_reference(
    const char *input, size_t end, size_t begin,
    const gsh_native_expansion_context *context, bool braced,
    size_t *after, const char **value, bool *found,
    gsh_parameter_kind *kind, char numeric[32])
{
    unsigned char special;
    size_t name_end;

    if (context == NULL || begin == end) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    special = (unsigned char)input[begin];
    *kind = GSH_PARAMETER_VALUE;
    if (special == '?') {
        (void)decimal_text((unsigned long)(context->last_status & 255),
                           numeric);
        *after = begin + 1U;
        *value = numeric;
        *found = true;
        return GSH_NATIVE_PLAN_OK;
    }
    if (special == '$' && context->shell_pid >= 0) {
        (void)decimal_text((unsigned long)context->shell_pid, numeric);
        *after = begin + 1U;
        *value = numeric;
        *found = true;
        return GSH_NATIVE_PLAN_OK;
    }
    if (special == '!') {
        *after = begin + 1U;
        if (context->last_background_pid > 0) {
            (void)decimal_text(
                (unsigned long)context->last_background_pid, numeric);
            *value = numeric;
            *found = true;
        } else {
            *value = "";
            *found = false;
        }
        return GSH_NATIVE_PLAN_OK;
    }
    if (special == '@' || special == '*') {
        *after = begin + 1U;
        *value = "";
        *found = context->positional_count != 0;
        *kind = special == '@' ? GSH_PARAMETER_AT : GSH_PARAMETER_STAR;
        return GSH_NATIVE_PLAN_OK;
    }
    if (special == '#') {
        (void)decimal_text((unsigned long)context->positional_count,
                           numeric);
        *after = begin + 1U;
        *value = numeric;
        *found = true;
        return GSH_NATIVE_PLAN_OK;
    }
    if (special == '-') {
        *after = begin + 1U;
        *value = context->option_flags != NULL ? context->option_flags : "";
        *found = true;
        return GSH_NATIVE_PLAN_OK;
    }
    if (special >= '0' && special <= '9') {
        size_t cursor = begin;
        size_t number = 0;

        do {
            size_t digit = (size_t)(input[cursor] - '0');

            if (number > (SIZE_MAX - digit) / 10U) {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            number = number * 10U + digit;
            cursor++;
        } while (braced && cursor < end && input[cursor] >= '0' &&
                 input[cursor] <= '9');
        *after = cursor;
        if (number == 0) {
            *value = context->parameter_zero != NULL
                         ? context->parameter_zero
                         : "";
            *found = true;
        } else if (number <= context->positional_count &&
                   context->positional_parameters != NULL) {
            *value = context->positional_parameters[number - 1U];
            *found = true;
        } else {
            *value = "";
            *found = false;
        }
        return GSH_NATIVE_PLAN_OK;
    }
    if (!name_start(special)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    name_end = begin + 1U;
    while (name_end < end &&
           name_byte((unsigned char)input[name_end])) {
        name_end++;
    }
    *value = lookup_variable(context, input + begin, name_end - begin,
                             found);
    if (*value == NULL) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    *after = name_end;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status report_unset_parameter(
    const gsh_native_expansion_context *context, const char *name,
    size_t name_length)
{
    if (!context->nounset) {
        return GSH_NATIVE_PLAN_OK;
    }
    if (context->parameter_error == NULL) {
        return GSH_NATIVE_PLAN_ERROR;
    }
    return context->parameter_error(context->variable_opaque, name,
                                    name_length, "", 0, true);
}

static gsh_native_plan_status positional_separator(
    const gsh_native_expansion_context *context, const char **separator,
    size_t *separator_length)
{
    bool found = false;
    const char *ifs = lookup_variable(context, "IFS", 3, &found);
    mbstate_t state;
    size_t length;

    if (ifs == NULL) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!found) {
        ifs = " ";
    }
    *separator = ifs;
    if (*ifs == '\0') {
        *separator_length = 0;
        return GSH_NATIVE_PLAN_OK;
    }
    memset(&state, 0, sizeof(state));
    length = mbrlen(ifs, MB_CUR_MAX, &state);
    if (length == (size_t)-1 || length == (size_t)-2 || length == 0) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    *separator_length = length;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status reserve_positional_parameters(
    const gsh_native_expansion_context *context, gsh_parameter_kind kind,
    bool quoted, gsh_expansion_mode mode,
    gsh_native_pipeline *pipeline)
{
    bool join = mode == GSH_EXPAND_SCALAR ||
                (kind == GSH_PARAMETER_STAR && quoted);
    size_t value_begin = pipeline->text_used;
    size_t index;

    if (join) {
        const char *separator = "";
        size_t separator_length = 0;
        gsh_native_plan_status status = positional_separator(
            context, &separator, &separator_length);

        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        for (index = 0; index < context->positional_count; index++) {
            size_t byte;

            if (index != 0) {
                for (byte = 0; byte < separator_length; byte++) {
                    if (!reserve_byte(
                            pipeline, (unsigned char)separator[byte])) {
                        return GSH_NATIVE_PLAN_LIMIT;
                    }
                }
            }
            status = reserve_parameter_value(
                pipeline, context->positional_parameters[index]);
            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
        }
        mark_expansion(pipeline, value_begin, quoted);
        return GSH_NATIVE_PLAN_OK;
    }
    if (context->positional_count == 0) {
        if (kind == GSH_PARAMETER_AT && quoted && value_begin > 0 &&
            (pipeline->provenance[value_begin - 1U] &
             GSH_WORD_EMPTY_MARKER) != 0) {
            pipeline->text_used--;
        }
        return GSH_NATIVE_PLAN_OK;
    }
    for (index = 0; index < context->positional_count; index++) {
        size_t begin = pipeline->text_used;
        const char *value = context->positional_parameters[index];
        gsh_native_plan_status status =
            reserve_parameter_value(pipeline, value);

        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        mark_expansion(pipeline, begin, quoted);
        if (*value == '\0' &&
            !reserve_positional_marker(pipeline, false, quoted)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        if (index + 1U < context->positional_count &&
            !reserve_positional_marker(pipeline, true, quoted)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status parameter_character_length(
    const char *value, unsigned long *length)
{
    mbstate_t state;
    const char *cursor = value;
    size_t bytes = 0;

    memset(&state, 0, sizeof(state));
    *length = 0;
    while (*cursor != '\0') {
        wchar_t character;
        size_t amount;

        if (bytes == GSH_NATIVE_TEXT_CAP) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        amount = mbrtowc(&character, cursor, GSH_NATIVE_TEXT_CAP - bytes,
                         &state);
        if (amount == (size_t)-1 || amount == (size_t)-2) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        if (amount == 0) {
            break;
        }
        cursor += amount;
        bytes += amount;
        (*length)++;
    }
    return GSH_NATIVE_PLAN_OK;
}

typedef enum {
    GSH_REMOVE_SMALLEST_PREFIX,
    GSH_REMOVE_LARGEST_PREFIX,
    GSH_REMOVE_SMALLEST_SUFFIX,
    GSH_REMOVE_LARGEST_SUFFIX,
} gsh_parameter_pattern_operation;

static bool pattern_has_syntax(const gsh_native_pipeline *pipeline,
                               size_t begin, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        unsigned char byte = (unsigned char)pipeline->text[begin + index];

        if ((pipeline->provenance[begin + index] & GSH_WORD_QUOTED) == 0 &&
            (byte == '*' || byte == '?' || byte == '[' || byte == '\\')) {
            return true;
        }
    }
    return false;
}

static bool single_star_pattern(const gsh_native_pipeline *pipeline,
                                size_t begin, size_t length, size_t *star)
{
    size_t index;
    size_t count = 0;

    for (index = 0; index < length; index++) {
        unsigned char byte = (unsigned char)pipeline->text[begin + index];

        if (byte >= 0x80U) {
            return false;
        }
        if ((pipeline->provenance[begin + index] & GSH_WORD_QUOTED) == 0) {
            if (byte == '*') {
                *star = index;
                count++;
            } else if (byte == '?' || byte == '[' || byte == '\\') {
                return false;
            }
        }
    }
    return count == 1U;
}

static bool single_star_match(const char *subject, size_t subject_length,
                              char *pattern, size_t star,
                              gsh_parameter_pattern_operation operation,
                              size_t *selected)
{
    size_t prefix_length = star;
    size_t suffix_length = strlen(pattern + star + 1U);
    const char *limit;
    const char *match;
    const char *last = NULL;
    bool prefix = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                  operation == GSH_REMOVE_LARGEST_PREFIX;
    bool smallest = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                    operation == GSH_REMOVE_SMALLEST_SUFFIX;

    if (subject_length < prefix_length + suffix_length) {
        return false;
    }
    if (prefix) {
        if (memcmp(subject, pattern, prefix_length) != 0) {
            return false;
        }
        limit = subject + subject_length - suffix_length;
        if (suffix_length == 0) {
            *selected = smallest ? prefix_length : subject_length;
            return true;
        }
        match = subject + prefix_length;
        while ((match = strstr(match, pattern + star + 1U)) != NULL &&
               match <= limit) {
            last = match;
            if (smallest) {
                break;
            }
            match++;
        }
        if (last == NULL) {
            return false;
        }
        *selected = (size_t)(last - subject) + suffix_length;
        return true;
    }
    if (memcmp(subject + subject_length - suffix_length,
               pattern + star + 1U, suffix_length) != 0) {
        return false;
    }
    limit = subject + subject_length - suffix_length;
    pattern[star] = '\0';
    if (prefix_length == 0) {
        last = smallest ? limit : subject;
    } else {
        const char *prefix_limit = limit - prefix_length;

        match = subject;
        while ((match = strstr(match, pattern)) != NULL &&
               match <= prefix_limit) {
            last = match;
            if (!smallest) {
                break;
            }
            match++;
        }
    }
    pattern[star] = '*';
    if (last == NULL) {
        return false;
    }
    *selected = (size_t)(last - subject);
    return true;
}

static gsh_native_plan_status encode_fnmatch_pattern(
    gsh_native_pipeline *pipeline, size_t begin, size_t length)
{
    static const char quoted_syntax[] = "*?[]\\-!^";
    size_t escapes = 0;
    size_t read;
    size_t write;

    for (read = 0; read < length; read++) {
        unsigned char byte = (unsigned char)pipeline->text[begin + read];

        if ((pipeline->provenance[begin + read] & GSH_WORD_QUOTED) != 0 &&
            strchr(quoted_syntax, (int)byte) != NULL) {
            escapes++;
        }
    }
    if (length + escapes >= GSH_NATIVE_TEXT_CAP - begin) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    read = begin + length;
    write = read + escapes;
    pipeline->text[write] = '\0';
    pipeline->text_used = write + 1U;
    while (read > begin) {
        unsigned char byte = (unsigned char)pipeline->text[--read];
        bool quoted =
            (pipeline->provenance[read] & GSH_WORD_QUOTED) != 0 &&
            strchr(quoted_syntax, (int)byte) != NULL;

        pipeline->text[--write] = (char)byte;
        if (quoted) {
            pipeline->text[--write] = '\\';
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

static bool pattern_word_preserves_variables(const char *input,
                                             gsh_word_ref word)
{
    size_t offset;

    for (offset = word.begin; offset < word.end; offset++) {
        unsigned char byte = (unsigned char)input[offset];

        if (byte == '$' || byte == 0x60U) {
            return false;
        }
    }
    return true;
}

static bool ascii_text(const char *text, size_t length)
{
    size_t offset;

    for (offset = 0; offset < length; offset++) {
        if ((unsigned char)text[offset] >= 0x80U) {
            return false;
        }
    }
    return true;
}

static size_t bracket_pattern_length(const char *pattern, size_t length)
{
    size_t offset = 1U;
    bool item = false;

    if (offset < length &&
        (pattern[offset] == '!' || pattern[offset] == '^')) {
        offset++;
    }
    if (offset < length && pattern[offset] == ']') {
        offset++;
        item = true;
    }
    while (offset < length) {
        if (pattern[offset] == ']' && item) {
            return offset + 1U;
        }
        if (pattern[offset] == '\\' && offset + 1U < length) {
            offset += 2U;
        } else if (pattern[offset] == '[' && offset + 1U < length &&
                   strchr(".:=", pattern[offset + 1U]) != NULL) {
            char marker = pattern[offset + 1U];
            size_t closing = offset + 2U;

            while (closing + 1U < length &&
                   !(pattern[closing] == marker &&
                     pattern[closing + 1U] == ']')) {
                closing++;
            }
            if (closing + 1U == length) {
                return 0;
            }
            offset = closing + 2U;
        } else {
            offset++;
        }
        item = true;
    }
    return 0;
}

static bool fixed_pattern_width(const char *pattern, size_t length,
                                size_t *width)
{
    size_t offset = 0;

    *width = 0;
    while (offset < length) {
        unsigned char byte = (unsigned char)pattern[offset];
        size_t token_length;

        if (byte >= 0x80U || byte == '*') {
            return false;
        }
        if (byte == '\\') {
            if (offset + 1U == length) {
                return false;
            }
            token_length = 2U;
        } else if (byte == '[') {
            token_length = bracket_pattern_length(pattern + offset,
                                                   length - offset);
            if (token_length == 0) {
                return false;
            }
        } else {
            token_length = 1U;
        }
        offset += token_length;
        (*width)++;
    }
    return true;
}

static bool fixed_single_star_pattern(const char *pattern, size_t *star,
                                      size_t *prefix_width,
                                      size_t *suffix_width)
{
    size_t length = strlen(pattern);
    size_t offset = 0;
    size_t count = 0;

    while (offset < length) {
        unsigned char byte = (unsigned char)pattern[offset];

        if (byte >= 0x80U) {
            return false;
        }
        if (byte == '\\') {
            if (offset + 1U == length) {
                return false;
            }
            offset += 2U;
        } else if (byte == '[') {
            size_t token_length = bracket_pattern_length(
                pattern + offset, length - offset);

            if (token_length == 0) {
                return false;
            }
            offset += token_length;
        } else {
            if (byte == '*') {
                *star = offset;
                count++;
            }
            offset++;
        }
    }
    return count == 1U &&
           fixed_pattern_width(pattern, *star, prefix_width) &&
           fixed_pattern_width(pattern + *star + 1U,
                               length - *star - 1U, suffix_width);
}

static bool fixed_single_star_match(
    char *subject, size_t subject_length, char *pattern, size_t star,
    size_t prefix_width, size_t suffix_width,
    gsh_parameter_pattern_operation operation, size_t *selected)
{
    size_t minimum;
    size_t maximum;
    size_t candidate;
    bool prefix = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                  operation == GSH_REMOVE_LARGEST_PREFIX;
    bool forward = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                   operation == GSH_REMOVE_LARGEST_SUFFIX;
    char saved;

    if (subject_length < prefix_width + suffix_width) {
        return false;
    }
    pattern[star] = '\0';
    if (prefix) {
        saved = subject[prefix_width];
        subject[prefix_width] = '\0';
    }
    if ((prefix && fnmatch(pattern, subject, 0) != 0) ||
        (!prefix && fnmatch(pattern + star + 1U,
                            subject + subject_length - suffix_width,
                            0) != 0)) {
        if (prefix) {
            subject[prefix_width] = saved;
        }
        pattern[star] = '*';
        return false;
    }
    if (prefix) {
        subject[prefix_width] = saved;
    }
    minimum = prefix ? prefix_width + suffix_width : 0;
    maximum = prefix ? subject_length
                     : subject_length - prefix_width - suffix_width;
    candidate = forward ? minimum : maximum;
    for (;;) {
        char *segment = prefix ? subject + candidate - suffix_width
                               : subject + candidate;
        size_t end = prefix ? candidate : candidate + prefix_width;

        saved = subject[end];
        subject[end] = '\0';
        if (fnmatch(prefix ? pattern + star + 1U : pattern,
                    segment, 0) == 0) {
            subject[end] = saved;
            pattern[star] = '*';
            *selected = candidate;
            return true;
        }
        subject[end] = saved;
        if (candidate == (forward ? maximum : minimum)) {
            break;
        }
        candidate = forward ? candidate + 1U : candidate - 1U;
    }
    pattern[star] = '*';
    return false;
}

static bool fixed_pattern_match(
    char *subject, size_t subject_length, char *pattern,
    size_t pattern_width, gsh_parameter_pattern_operation operation,
    size_t *selected)
{
    bool prefix = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                  operation == GSH_REMOVE_LARGEST_PREFIX;
    size_t begin;
    char saved;

    if (subject_length < pattern_width) {
        return false;
    }
    begin = prefix ? 0 : subject_length - pattern_width;
    saved = subject[begin + pattern_width];
    subject[begin + pattern_width] = '\0';
    if (fnmatch(pattern, subject + begin, 0) != 0) {
        subject[begin + pattern_width] = saved;
        return false;
    }
    subject[begin + pattern_width] = saved;
    *selected = prefix ? pattern_width : begin;
    return true;
}

enum {
    GSH_PATTERN_STAR_MARKER = 0x80,
    GSH_PATTERN_REACTOR_WORK_CAP = 32768,
};

static size_t mark_fixed_pattern_stars(char *pattern)
{
    size_t length = strlen(pattern);
    size_t offset = 0;
    size_t stars = 0;

    while (offset < length) {
        unsigned char byte = (unsigned char)pattern[offset];

        if (byte >= 0x80U) {
            stars = 0;
            break;
        }
        if (byte == '\\') {
            if (offset + 1U == length) {
                stars = 0;
                break;
            }
            offset += 2U;
        } else if (byte == '[') {
            size_t token_length = bracket_pattern_length(
                pattern + offset, length - offset);

            if (token_length == 0) {
                stars = 0;
                break;
            }
            offset += token_length;
        } else {
            if (byte == '*') {
                pattern[offset] = (char)GSH_PATTERN_STAR_MARKER;
                stars++;
            }
            offset++;
        }
    }
    if (stars == 0) {
        for (offset = 0; offset < length; offset++) {
            if ((unsigned char)pattern[offset] ==
                GSH_PATTERN_STAR_MARKER) {
                pattern[offset] = '*';
            }
        }
    }
    return stars;
}

static void restore_pattern_stars(char *pattern)
{
    size_t offset;

    for (offset = 0; pattern[offset] != '\0'; offset++) {
        if ((unsigned char)pattern[offset] == GSH_PATTERN_STAR_MARKER) {
            pattern[offset] = '*';
        }
    }
}

static size_t next_pattern_star(const char *pattern, size_t length,
                                size_t offset)
{
    while (offset < length &&
           (unsigned char)pattern[offset] != GSH_PATTERN_STAR_MARKER) {
        offset++;
    }
    return offset;
}

static size_t previous_pattern_star(const char *pattern, size_t offset)
{
    while (offset > 0) {
        offset--;
        if ((unsigned char)pattern[offset] == GSH_PATTERN_STAR_MARKER) {
            return offset;
        }
    }
    return SIZE_MAX;
}

static size_t fixed_pattern_max_segment(const char *pattern)
{
    size_t maximum = 0;
    size_t begin = 0;
    size_t offset;

    for (offset = 0;; offset++) {
        if (pattern[offset] == '\0' ||
            (unsigned char)pattern[offset] == GSH_PATTERN_STAR_MARKER) {
            if (offset - begin > maximum) {
                maximum = offset - begin;
            }
            if (pattern[offset] == '\0') {
                return maximum;
            }
            begin = offset + 1U;
        }
    }
}

static bool fixed_segment_match(char *subject, char *pattern,
                                size_t pattern_begin, size_t pattern_end,
                                size_t subject_begin, size_t width)
{
    char pattern_saved = pattern[pattern_end];
    char subject_saved = subject[subject_begin + width];
    bool matches;

    pattern[pattern_end] = '\0';
    subject[subject_begin + width] = '\0';
    matches = fnmatch(pattern + pattern_begin,
                      subject + subject_begin, 0) == 0;
    subject[subject_begin + width] = subject_saved;
    pattern[pattern_end] = pattern_saved;
    return matches;
}

static bool find_fixed_segment(
    char *subject, size_t subject_length, char *pattern,
    size_t pattern_begin, size_t pattern_end, size_t low, size_t high,
    bool forward, size_t *selected)
{
    size_t width;
    size_t candidate;

    if (!fixed_pattern_width(pattern + pattern_begin,
                             pattern_end - pattern_begin, &width) ||
        width > subject_length || low > high || high > subject_length - width) {
        return false;
    }
    candidate = forward ? low : high;
    for (;;) {
        if (fixed_segment_match(subject, pattern, pattern_begin,
                                pattern_end, candidate, width)) {
            *selected = candidate;
            return true;
        }
        if (candidate == (forward ? high : low)) {
            return false;
        }
        candidate = forward ? candidate + 1U : candidate - 1U;
    }
}

static bool fixed_stars_shortest_match(
    char *subject, size_t subject_length, char *pattern,
    gsh_parameter_pattern_operation operation, size_t *selected)
{
    size_t pattern_length = strlen(pattern);
    bool prefix = operation == GSH_REMOVE_SMALLEST_PREFIX;
    size_t cursor = prefix ? 0 : subject_length;
    bool separated = false;

    if (prefix) {
        size_t begin = 0;

        while (begin <= pattern_length) {
            size_t end = next_pattern_star(pattern, pattern_length, begin);

            if (end > begin) {
                size_t width;
                size_t position;

                if (!fixed_pattern_width(pattern + begin, end - begin,
                                         &width) ||
                    width > subject_length - cursor) {
                    return false;
                }
                if (!separated) {
                    position = 0;
                    if (!fixed_segment_match(subject, pattern, begin, end,
                                             position, width)) {
                        return false;
                    }
                } else if (!find_fixed_segment(
                               subject, subject_length, pattern, begin, end,
                               cursor, subject_length - width, true,
                               &position)) {
                    return false;
                }
                cursor = position + width;
            }
            if (end == pattern_length) {
                break;
            }
            separated = true;
            begin = end + 1U;
        }
    } else {
        size_t end = pattern_length;

        while (true) {
            size_t marker = previous_pattern_star(pattern, end);
            size_t begin = marker == SIZE_MAX ? 0 : marker + 1U;

            if (end > begin) {
                size_t width;
                size_t position;

                if (!fixed_pattern_width(pattern + begin, end - begin,
                                         &width) || width > cursor) {
                    return false;
                }
                if (!separated) {
                    position = subject_length - width;
                    if (!fixed_segment_match(subject, pattern, begin, end,
                                             position, width)) {
                        return false;
                    }
                } else if (!find_fixed_segment(
                               subject, subject_length, pattern, begin, end,
                               0, cursor - width, false, &position)) {
                    return false;
                }
                cursor = position;
            }
            if (marker == SIZE_MAX) {
                break;
            }
            separated = true;
            end = marker;
        }
    }
    *selected = cursor;
    return true;
}

static bool segment_match_exists(char *pattern, size_t capacity,
                                 const char *subject, bool prefix,
                                 bool *known)
{
    size_t length = strlen(pattern);
    size_t backslashes = 0;
    bool exists;

    *known = false;
    if (length + 2U > capacity) {
        return false;
    }
    while (backslashes < length &&
           pattern[length - backslashes - 1U] == '\\') {
        backslashes++;
    }
    if (prefix && backslashes % 2U != 0) {
        return false;
    }
    if (prefix) {
        pattern[length] = '*';
        pattern[length + 1U] = '\0';
        exists = fnmatch(pattern, subject, 0) == 0;
        pattern[length] = '\0';
    } else {
        memmove(pattern + 1U, pattern, length + 1U);
        pattern[0] = '*';
        exists = fnmatch(pattern, subject, 0) == 0;
        memmove(pattern, pattern + 1U, length + 1U);
    }
    *known = true;
    return exists;
}

static gsh_native_plan_status remove_parameter_pattern(
    const char *input, gsh_word_ref word,
    const gsh_native_expansion_context *context, size_t depth,
    gsh_native_pipeline *pipeline, size_t value_begin,
    const char *parameter_value, gsh_parameter_pattern_operation operation)
{
    char *subject_scratch =
        pipeline->heredoc_text + pipeline->heredoc_text_used;
    size_t subject_capacity = GSH_NATIVE_HEREDOC_TEXT_CAP -
                              pipeline->heredoc_text_used;
    const char *subject = parameter_value;
    char *pattern;
    size_t pattern_length;
    size_t subject_length = strnlen(parameter_value, subject_capacity);
    size_t result_begin = 0;
    size_t result_length = subject_length;
    gsh_native_plan_status status;

    if (subject_length == subject_capacity) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (!pattern_word_preserves_variables(input, word)) {
        memcpy(subject_scratch, parameter_value, subject_length + 1U);
        subject = subject_scratch;
    }
    status = expand_static_word(input, word, context, GSH_EXPAND_SCALAR,
                                depth + 1U, pipeline, &pattern,
                                &pattern_length);
    if (status != GSH_NATIVE_PLAN_OK) {
        return status;
    }
    if (!pattern_has_syntax(
            pipeline, (size_t)(pattern - pipeline->text), pattern_length)) {
        bool prefix = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                      operation == GSH_REMOVE_LARGEST_PREFIX;
        bool matches = subject_length >= pattern_length &&
                       memcmp(prefix ? subject
                                     : subject + subject_length -
                                                   pattern_length,
                              pattern, pattern_length) == 0;

        if (matches && prefix) {
            result_begin = pattern_length;
            result_length -= pattern_length;
        } else if (matches) {
            result_length -= pattern_length;
        }
    } else {
        size_t star = 0;
        size_t selected;
        bool single = single_star_pattern(
            pipeline, (size_t)(pattern - pipeline->text), pattern_length,
            &star);

        if (single) {
            if (single_star_match(subject, subject_length, pattern, star,
                                  operation, &selected)) {
                bool prefix = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                              operation == GSH_REMOVE_LARGEST_PREFIX;

                if (prefix) {
                    result_begin = selected;
                    result_length -= selected;
                } else {
                    result_length = selected;
                }
            }
        } else {
            size_t cut = 0;
            size_t selected_general = 0;
            bool found = false;
            bool prefix = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                          operation == GSH_REMOVE_LARGEST_PREFIX;
            bool first_match = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                               operation == GSH_REMOVE_LARGEST_SUFFIX;
            mbstate_t character_state;
            char *match_subject = subject_scratch;
            bool existence_known = false;
            bool possible = false;
            bool fixed;
            bool fixed_star;
            bool fixed_stars = false;
            size_t fixed_width;
            size_t fixed_star_offset;
            size_t fixed_prefix_width;
            size_t fixed_suffix_width;

            status = encode_fnmatch_pattern(
                pipeline, (size_t)(pattern - pipeline->text),
                pattern_length);
            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
            if (subject != subject_scratch) {
                memcpy(subject_scratch, subject, subject_length + 1U);
            }
            fixed = ascii_text(match_subject, subject_length) &&
                    fixed_pattern_width(pattern, strlen(pattern),
                                        &fixed_width);
            fixed_star = !fixed &&
                         ascii_text(match_subject, subject_length) &&
                         fixed_single_star_pattern(
                             pattern, &fixed_star_offset,
                             &fixed_prefix_width, &fixed_suffix_width);
            if (fixed) {
                if (fixed_pattern_match(
                        match_subject, subject_length, pattern,
                        fixed_width, operation, &selected_general)) {
                    found = true;
                }
            } else if (fixed_star) {
                if (fixed_single_star_match(
                        match_subject, subject_length, pattern,
                        fixed_star_offset, fixed_prefix_width,
                        fixed_suffix_width, operation,
                        &selected_general)) {
                    found = true;
                }
            } else {
                size_t star_count =
                    ascii_text(match_subject, subject_length)
                        ? mark_fixed_pattern_stars(pattern)
                        : 0;
                size_t maximum_segment = star_count > 0
                                             ? fixed_pattern_max_segment(
                                                   pattern)
                                             : pattern_length;
                bool bounded = context == NULL ||
                               !context->defer_complex_patterns ||
                               maximum_segment == 0 ||
                               subject_length <=
                                   GSH_PATTERN_REACTOR_WORK_CAP /
                                       maximum_segment;

                if (star_count > 1U && bounded &&
                    (operation == GSH_REMOVE_SMALLEST_PREFIX ||
                     operation == GSH_REMOVE_SMALLEST_SUFFIX)) {
                    fixed_stars = true;
                    if (fixed_stars_shortest_match(
                            match_subject, subject_length, pattern,
                            operation, &selected_general)) {
                        found = true;
                    }
                }
                if (star_count > 0) {
                    restore_pattern_stars(pattern);
                }
                if (!fixed_stars) {
                    if (context != NULL &&
                        context->defer_complex_patterns) {
                        if (context->deferred_work != NULL) {
                            *context->deferred_work = true;
                        }
                        result_begin = 0;
                        result_length = subject_length;
                    } else {
                        possible = segment_match_exists(
                            pattern,
                            GSH_NATIVE_TEXT_CAP -
                                (size_t)(pattern - pipeline->text),
                            match_subject, prefix, &existence_known);
                    }
                }
            }
            if (!fixed && !fixed_star && !fixed_stars &&
                (context == NULL || !context->defer_complex_patterns) &&
                (!existence_known || possible)) {
                if (!first_match &&
                    ascii_text(match_subject, subject_length)) {
                    cut = subject_length;
                    for (;;) {
                        bool matches;

                        if (prefix) {
                            char saved = match_subject[cut];

                            match_subject[cut] = '\0';
                            matches = fnmatch(pattern, match_subject, 0) == 0;
                            match_subject[cut] = saved;
                        } else {
                            matches = fnmatch(pattern,
                                              match_subject + cut, 0) == 0;
                        }
                        if (matches) {
                            selected_general = cut;
                            found = true;
                            break;
                        }
                        if (cut == 0) {
                            break;
                        }
                        cut--;
                    }
                } else {
                    memset(&character_state, 0, sizeof(character_state));
                    for (;;) {
                        bool matches;

                        if (prefix) {
                            char saved = match_subject[cut];

                            match_subject[cut] = '\0';
                            matches = fnmatch(pattern, match_subject, 0) == 0;
                            match_subject[cut] = saved;
                        } else {
                            matches = fnmatch(pattern,
                                              match_subject + cut, 0) == 0;
                        }
                        if (matches) {
                            selected_general = cut;
                            found = true;
                            if (first_match) {
                                break;
                            }
                        }
                        if (cut == subject_length) {
                            break;
                        }
                        {
                            size_t amount = mbrlen(
                                match_subject + cut,
                                subject_length - cut, &character_state);

                            if (amount == (size_t)-1 ||
                                amount == (size_t)-2 || amount == 0) {
                                return GSH_NATIVE_PLAN_UNSUPPORTED;
                            }
                            cut += amount;
                        }
                    }
                }
            }
            if (found && prefix) {
                result_begin = selected_general;
                result_length -= selected_general;
            } else if (found) {
                result_length = selected_general;
            }
        }
    }
    pipeline->text_used = value_begin;
    while (result_length > 0) {
        if (!reserve_byte(pipeline,
                          (unsigned char)subject[result_begin++])) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        result_length--;
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status expand_parameter(
    const char *input, size_t end, size_t *offset, bool quoted,
    gsh_expansion_mode mode, size_t depth,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline)
{
    char numeric[32];
    const char *parameter_value;
    bool parameter_found;
    gsh_parameter_kind parameter_kind;
    size_t value_begin = pipeline->text_used;
    size_t parameter_end;
    gsh_native_plan_status status;

    if (context == NULL || *offset == end) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (input[*offset] != '{') {
        status = parameter_reference(input, end, *offset, context, false,
                                     &parameter_end, &parameter_value,
                                     &parameter_found, &parameter_kind,
                                     numeric);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        if (parameter_kind == GSH_PARAMETER_VALUE && !parameter_found) {
            status = report_unset_parameter(
                context, input + *offset, parameter_end - *offset);
            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
            if (context->preflight) {
                parameter_value = "0";
            }
        }
        status = parameter_kind == GSH_PARAMETER_VALUE
                     ? reserve_parameter_value(pipeline, parameter_value)
                     : reserve_positional_parameters(
                           context, parameter_kind, quoted, mode, pipeline);
        if (status == GSH_NATIVE_PLAN_OK) {
            if (parameter_kind == GSH_PARAMETER_VALUE) {
                mark_expansion(pipeline, value_begin, quoted);
            }
            *offset = parameter_end;
        }
        return status;
    }
    {
        size_t opening = *offset;
        size_t closing;
        size_t cursor = opening + 1U;
        bool length_form = false;

        status = find_parameter_end(input, end, opening, &closing);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        if (cursor + 1U < closing && input[cursor] == '#') {
            length_form = true;
            cursor++;
        }
        status = parameter_reference(input, closing, cursor, context, true,
                                     &parameter_end, &parameter_value,
                                     &parameter_found, &parameter_kind,
                                     numeric);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        if (length_form) {
            unsigned long length;

            if (parameter_end != closing ||
                parameter_kind != GSH_PARAMETER_VALUE) {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            if (!parameter_found) {
                status = report_unset_parameter(
                    context, input + cursor, parameter_end - cursor);
                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
            }
            status = parameter_character_length(parameter_value, &length);
            if (status == GSH_NATIVE_PLAN_OK &&
                !reserve_decimal(pipeline, length)) {
                status = GSH_NATIVE_PLAN_LIMIT;
            }
            if (status == GSH_NATIVE_PLAN_OK) {
                mark_expansion(pipeline, value_begin, quoted);
                *offset = closing + 1U;
            }
            return status;
        }
        if (parameter_end == closing) {
            if (parameter_kind == GSH_PARAMETER_VALUE && !parameter_found) {
                status = report_unset_parameter(
                    context, input + cursor, parameter_end - cursor);
                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
                if (context->preflight) {
                    parameter_value = "0";
                }
            }
            status = parameter_kind == GSH_PARAMETER_VALUE
                         ? reserve_parameter_value(pipeline,
                                                   parameter_value)
                         : reserve_positional_parameters(
                               context, parameter_kind, quoted, mode,
                               pipeline);
            if (status == GSH_NATIVE_PLAN_OK) {
                if (parameter_kind == GSH_PARAMETER_VALUE) {
                    mark_expansion(pipeline, value_begin, quoted);
                }
                *offset = closing + 1U;
            }
            return status;
        }
        if (parameter_kind != GSH_PARAMETER_VALUE) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        if (input[parameter_end] == '#' || input[parameter_end] == '%') {
            unsigned char operator =
                (unsigned char)input[parameter_end];
            size_t word_begin = parameter_end + 1U;
            bool largest = word_begin < closing &&
                           (unsigned char)input[word_begin] == operator;
            gsh_parameter_pattern_operation operation;

            if (!parameter_found) {
                status = report_unset_parameter(
                    context, input + cursor, parameter_end - cursor);
                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
            }
            if (depth == 32U) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            if (largest) {
                word_begin++;
            }
            if (operator == '#') {
                operation = largest ? GSH_REMOVE_LARGEST_PREFIX
                                    : GSH_REMOVE_SMALLEST_PREFIX;
            } else {
                operation = largest ? GSH_REMOVE_LARGEST_SUFFIX
                                    : GSH_REMOVE_SMALLEST_SUFFIX;
            }
            status = remove_parameter_pattern(
                input, (gsh_word_ref){word_begin, closing}, context,
                depth, pipeline, value_begin, parameter_value, operation);
            if (status == GSH_NATIVE_PLAN_OK) {
                mark_expansion(pipeline, value_begin, quoted);
                *offset = closing + 1U;
            }
            return status;
        }
        {
            bool colon = input[parameter_end] == ':';
            size_t operator_offset = parameter_end + (colon ? 1U : 0U);
            unsigned char operator;
            bool use_word;

            if (operator_offset >= closing) {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            operator = (unsigned char)input[operator_offset++];
            if (operator != '-' && operator != '+' && operator != '=' &&
                operator != '?') {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            if (operator == '-' || operator == '=') {
                use_word = !parameter_found ||
                           (colon && *parameter_value == '\0');
            } else if (operator == '+') {
                use_word = parameter_found &&
                           (!colon || *parameter_value != '\0');
            } else {
                use_word = !parameter_found ||
                           (colon && *parameter_value == '\0');
            }
            if (use_word) {
                gsh_word_ref word = {operator_offset, closing};
                char *expanded_word;
                size_t expanded_length;

                if (depth == 32U) {
                    return GSH_NATIVE_PLAN_LIMIT;
                }
                status = expand_static_word(
                    input, word, context,
                    operator == '?' ? GSH_EXPAND_SCALAR : mode,
                    depth + 1U, pipeline,
                    &expanded_word, &expanded_length);
                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
                if (operator == '=') {
                    if (!name_start((unsigned char)input[cursor]) ||
                        parameter_end != operator_offset -
                                             (colon ? 2U : 1U) ||
                        context->variable_assign == NULL) {
                        return GSH_NATIVE_PLAN_UNSUPPORTED;
                    }
                    status = context->variable_assign(
                        context->variable_opaque, input + cursor,
                        parameter_end - cursor, expanded_word,
                        expanded_length);
                    if (status != GSH_NATIVE_PLAN_OK) {
                        return status;
                    }
                } else if (operator == '?') {
                    if (context->parameter_error == NULL) {
                        return GSH_NATIVE_PLAN_UNSUPPORTED;
                    }
                    status = context->parameter_error(
                        context->variable_opaque, input + cursor,
                        parameter_end - cursor, expanded_word,
                        expanded_length, operator_offset == closing);
                    if (status != GSH_NATIVE_PLAN_OK) {
                        return status;
                    }
                }
                pipeline->text_used--;
            } else {
                status = reserve_parameter_value(pipeline, parameter_value);
                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
            }
            if (operator == '?' && use_word) {
                pipeline->text_used = value_begin;
            }
            mark_expansion(pipeline, value_begin, quoted);
            *offset = closing + 1U;
            return GSH_NATIVE_PLAN_OK;
        }
    }
}

typedef enum {
    SUBSTITUTION_SINGLE,
    SUBSTITUTION_DOUBLE,
    SUBSTITUTION_DOLLAR_SINGLE,
    SUBSTITUTION_BACKQUOTE,
    SUBSTITUTION_PARAMETER,
    SUBSTITUTION_COMMAND,
    SUBSTITUTION_ARITHMETIC,
    SUBSTITUTION_PARENTHESIS,
} substitution_context_kind;

typedef struct {
    substitution_context_kind items[128];
    size_t length;
} substitution_context_stack;

static bool substitution_push(substitution_context_stack *stack,
                              substitution_context_kind kind)
{
    if (stack->length == sizeof(stack->items) / sizeof(stack->items[0])) {
        return false;
    }
    stack->items[stack->length++] = kind;
    return true;
}

static bool substitution_nested(const char *input, size_t end,
                                size_t *offset,
                                substitution_context_stack *stack)
{
    size_t remaining = end - *offset;

    if (remaining >= 3U && input[*offset] == '$' &&
        input[*offset + 1U] == '(' &&
        input[*offset + 2U] == '(') {
        if (!substitution_push(stack, SUBSTITUTION_ARITHMETIC)) {
            return false;
        }
        *offset += 3U;
        return true;
    }
    if (remaining >= 2U && input[*offset] == '$' &&
        input[*offset + 1U] == '(') {
        if (!substitution_push(stack, SUBSTITUTION_COMMAND)) {
            return false;
        }
        *offset += 2U;
        return true;
    }
    if (remaining >= 2U && input[*offset] == '$' &&
        input[*offset + 1U] == '{') {
        if (!substitution_push(stack, SUBSTITUTION_PARAMETER)) {
            return false;
        }
        *offset += 2U;
        return true;
    }
    if (remaining >= 2U && input[*offset] == '$' &&
        input[*offset + 1U] == '\'') {
        if (!substitution_push(stack, SUBSTITUTION_DOLLAR_SINGLE)) {
            return false;
        }
        *offset += 2U;
        return true;
    }
    return false;
}

static gsh_native_plan_status find_substitution_end(
    const char *input, size_t end, size_t opening_parenthesis,
    bool arithmetic, size_t *contents_end, size_t *after)
{
    substitution_context_stack stack = {{0}, 0};
    size_t offset = opening_parenthesis + (arithmetic ? 2U : 1U);

    if (!substitution_push(&stack, arithmetic ? SUBSTITUTION_ARITHMETIC
                                              : SUBSTITUTION_COMMAND)) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    while (offset < end) {
        substitution_context_kind kind = stack.items[stack.length - 1U];
        unsigned char byte = (unsigned char)input[offset];

        if (byte == '\0') {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        if (kind == SUBSTITUTION_SINGLE) {
            offset++;
            if (byte == '\'') {
                stack.length--;
            }
            continue;
        }
        if (kind == SUBSTITUTION_DOLLAR_SINGLE) {
            if (byte == '\\' && offset + 1U < end) {
                offset += 2U;
            } else {
                offset++;
                if (byte == '\'') {
                    stack.length--;
                }
            }
            continue;
        }
        if (kind == SUBSTITUTION_BACKQUOTE) {
            if (byte == '\\' && offset + 1U < end) {
                offset += 2U;
            } else {
                offset++;
                if (byte == 0x60U) {
                    stack.length--;
                }
            }
            continue;
        }
        if (kind == SUBSTITUTION_DOUBLE) {
            if (byte == '"') {
                offset++;
                stack.length--;
            } else if (byte == '\\' && offset + 1U < end) {
                offset += 2U;
            } else if (byte == 0x60U) {
                if (!substitution_push(&stack, SUBSTITUTION_BACKQUOTE)) {
                    return GSH_NATIVE_PLAN_LIMIT;
                }
                offset++;
            } else if (byte == '$' &&
                       substitution_nested(input, end, &offset, &stack)) {
                continue;
            } else {
                offset++;
            }
            continue;
        }

        if (byte == '\\' && offset + 1U < end) {
            offset += 2U;
        } else if (byte == '\'' && kind != SUBSTITUTION_ARITHMETIC) {
            if (!substitution_push(&stack, SUBSTITUTION_SINGLE)) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            offset++;
        } else if (byte == '"' && kind != SUBSTITUTION_ARITHMETIC) {
            if (!substitution_push(&stack, SUBSTITUTION_DOUBLE)) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            offset++;
        } else if (byte == 0x60U) {
            if (!substitution_push(&stack, SUBSTITUTION_BACKQUOTE)) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            offset++;
        } else if (byte == '$' &&
                   substitution_nested(input, end, &offset, &stack)) {
            continue;
        } else if (kind == SUBSTITUTION_PARAMETER && byte == '}') {
            offset++;
            stack.length--;
        } else if ((kind == SUBSTITUTION_COMMAND ||
                    kind == SUBSTITUTION_PARENTHESIS) &&
                   byte == '(') {
            if (!substitution_push(&stack, SUBSTITUTION_PARENTHESIS)) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            offset++;
        } else if (kind == SUBSTITUTION_PARENTHESIS && byte == ')') {
            offset++;
            stack.length--;
        } else if (kind == SUBSTITUTION_COMMAND && byte == ')') {
            stack.length--;
            if (stack.length == 0) {
                *contents_end = offset;
                *after = offset + 1U;
                return GSH_NATIVE_PLAN_OK;
            }
            offset++;
        } else if (kind == SUBSTITUTION_ARITHMETIC &&
                   offset + 1U < end && byte == ')' &&
                   input[offset + 1U] == ')') {
            size_t closing = offset;

            offset += 2U;
            stack.length--;
            if (stack.length == 0) {
                *contents_end = closing;
                *after = offset;
                return GSH_NATIVE_PLAN_OK;
            }
        } else if (kind == SUBSTITUTION_ARITHMETIC && byte == '(') {
            if (!substitution_push(&stack, SUBSTITUTION_PARENTHESIS)) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            offset++;
        } else {
            offset++;
        }
    }
    return GSH_NATIVE_PLAN_UNSUPPORTED;
}

static gsh_native_plan_status expand_command_substitution(
    const char *input, size_t end, size_t *offset,
    const gsh_native_expansion_context *context, char *output,
    size_t output_capacity, size_t *output_used)
{
    size_t commands_end;
    size_t after;
    size_t produced = 0;
    int exit_status = 0;
    gsh_native_plan_status status;

    if (context == NULL || context->command_substitute == NULL ||
        *offset >= end || input[*offset] != '(' ||
        (*offset + 1U < end && input[*offset + 1U] == '(')) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    status = find_substitution_end(input, end, *offset, false,
                                   &commands_end, &after);
    if (status != GSH_NATIVE_PLAN_OK) {
        return status;
    }
    status = context->command_substitute(
        context->command_substitute_opaque, input + *offset + 1U,
        commands_end - *offset - 1U, output + *output_used,
        output_capacity - *output_used, &produced, &exit_status);
    if (status != GSH_NATIVE_PLAN_OK) {
        return status;
    }
    if (produced > output_capacity - *output_used) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (context->command_substitution_performed != NULL &&
        context->command_substitution_status != NULL) {
        *context->command_substitution_performed = true;
        *context->command_substitution_status = exit_status;
    }
    *output_used += produced;
    *offset = after;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status expand_backquote_substitution(
    const char *input, size_t end, size_t *offset, bool double_quoted,
    const gsh_native_expansion_context *context, char *output,
    size_t output_capacity, size_t *output_used)
{
    char commands[GSH_NATIVE_TEXT_CAP];
    size_t command_used = 0;
    size_t cursor = *offset;
    size_t produced = 0;
    int exit_status = 0;
    gsh_native_plan_status status;

    if (context == NULL || context->command_substitute == NULL) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    while (cursor < end) {
        unsigned char byte = (unsigned char)input[cursor++];

        if (byte == 0x60U) {
            status = context->command_substitute(
                context->command_substitute_opaque, commands, command_used,
                output + *output_used, output_capacity - *output_used,
                &produced, &exit_status);
            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
            if (produced > output_capacity - *output_used) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            if (context->command_substitution_performed != NULL &&
                context->command_substitution_status != NULL) {
                *context->command_substitution_performed = true;
                *context->command_substitution_status = exit_status;
            }
            *output_used += produced;
            *offset = cursor;
            return GSH_NATIVE_PLAN_OK;
        }
        if (byte == '\\' && cursor < end) {
            unsigned char next = (unsigned char)input[cursor];
            bool remove_escape = next == '$' || next == 0x60U ||
                                 next == '\\' ||
                                 (double_quoted && next == '\n');

            if (remove_escape) {
                cursor++;
                if (next == '\n') {
                    continue;
                }
                byte = next;
            }
        }
        if (command_used == sizeof(commands)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        commands[command_used++] = (char)byte;
    }
    return GSH_NATIVE_PLAN_UNSUPPORTED;
}

typedef struct {
    const char *input;
    size_t offset;
    size_t end;
    const gsh_native_expansion_context *context;
    size_t nesting;
    gsh_native_plan_status error;
    const char *diagnostic;
} arithmetic_parser;

typedef enum {
    ARITHMETIC_ASSIGN_NONE,
    ARITHMETIC_ASSIGN_SET,
    ARITHMETIC_ASSIGN_MULTIPLY,
    ARITHMETIC_ASSIGN_DIVIDE,
    ARITHMETIC_ASSIGN_REMAINDER,
    ARITHMETIC_ASSIGN_ADD,
    ARITHMETIC_ASSIGN_SUBTRACT,
    ARITHMETIC_ASSIGN_SHIFT_LEFT,
    ARITHMETIC_ASSIGN_SHIFT_RIGHT,
    ARITHMETIC_ASSIGN_AND,
    ARITHMETIC_ASSIGN_XOR,
    ARITHMETIC_ASSIGN_OR,
} arithmetic_assignment_operator;

static bool arithmetic_failure(arithmetic_parser *parser,
                               const char *diagnostic)
{
    if (parser->diagnostic == NULL) {
        parser->diagnostic = diagnostic;
    }
    return false;
}

static void arithmetic_skip_offset(const arithmetic_parser *parser,
                                   size_t *offset)
{
    while (*offset < parser->end &&
           (parser->input[*offset] == ' ' ||
            parser->input[*offset] == '\t' ||
            parser->input[*offset] == '\n')) {
        (*offset)++;
    }
}

static void arithmetic_skip_space(arithmetic_parser *parser)
{
    arithmetic_skip_offset(parser, &parser->offset);
}

static bool arithmetic_match(arithmetic_parser *parser, const char *text)
{
    size_t length = strlen(text);

    arithmetic_skip_space(parser);
    if (length > parser->end - parser->offset ||
        memcmp(parser->input + parser->offset, text, length) != 0) {
        return false;
    }
    parser->offset += length;
    return true;
}

static bool arithmetic_match_binary(arithmetic_parser *parser,
                                    const char *text)
{
    size_t saved = parser->offset;

    if (!arithmetic_match(parser, text)) {
        return false;
    }
    if (parser->offset < parser->end &&
        parser->input[parser->offset] == '=') {
        parser->offset = saved;
        return false;
    }
    return true;
}

static int arithmetic_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') {
        return (int)(byte - '0');
    }
    if (byte >= 'a' && byte <= 'f') {
        return 10 + (int)(byte - 'a');
    }
    if (byte >= 'A' && byte <= 'F') {
        return 10 + (int)(byte - 'A');
    }
    return -1;
}

static bool arithmetic_constant(const char *text, size_t length,
                                long *value)
{
    size_t offset = 0;
    bool negative = false;
    unsigned int base = 10;
    unsigned long magnitude = 0;
    unsigned long maximum;
    size_t digits = 0;

    if (offset < length && (text[offset] == '+' || text[offset] == '-')) {
        negative = text[offset++] == '-';
    }
    if (offset >= length) {
        return false;
    }
    if (text[offset] == '0') {
        if (offset + 1U < length &&
            (text[offset + 1U] == 'x' || text[offset + 1U] == 'X')) {
            base = 16;
            offset += 2U;
        } else {
            base = 8;
        }
    }
    maximum = negative ? (unsigned long)LONG_MAX + 1UL
                       : (unsigned long)LONG_MAX;
    while (offset < length) {
        int digit = arithmetic_digit((unsigned char)text[offset]);

        if (digit < 0 || (unsigned int)digit >= base ||
            magnitude > (maximum - (unsigned long)digit) / base) {
            return false;
        }
        magnitude = magnitude * base + (unsigned long)digit;
        offset++;
        digits++;
    }
    if (digits == 0) {
        return false;
    }
    if (negative) {
        *value = magnitude == (unsigned long)LONG_MAX + 1UL
                     ? LONG_MIN
                     : -(long)magnitude;
    } else {
        *value = (long)magnitude;
    }
    return true;
}

static bool arithmetic_environment_value(
    arithmetic_parser *parser, const char *name, size_t name_length,
    bool evaluate, long *value)
{
    bool found;
    const char *text;
    size_t length;

    if (!evaluate) {
        *value = 0;
        return true;
    }
    if (parser->context == NULL) {
        return false;
    }
    text = lookup_variable(parser->context, name, name_length, &found);
    if (text == NULL) {
        return false;
    }
    if (!found) {
        gsh_native_plan_status status = report_unset_parameter(
            parser->context, name, name_length);

        if (status != GSH_NATIVE_PLAN_OK) {
            parser->error = status;
            return false;
        }
        *value = 0;
        return true;
    }
    if (*text == '\0') {
        *value = 0;
        return true;
    }
    length = strnlen(text, GSH_NATIVE_TEXT_CAP);
    if (length == GSH_NATIVE_TEXT_CAP ||
        !arithmetic_constant(text, length, value)) {
        return arithmetic_failure(
            parser, "variable value is not a valid integer");
    }
    return true;
}

static bool arithmetic_add(long left, long right, long *result)
{
    if ((right > 0 && left > LONG_MAX - right) ||
        (right < 0 && left < LONG_MIN - right)) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool arithmetic_subtract(long left, long right, long *result)
{
    if ((right > 0 && left < LONG_MIN + right) ||
        (right < 0 && left > LONG_MAX + right)) {
        return false;
    }
    *result = left - right;
    return true;
}

static bool arithmetic_multiply(long left, long right, long *result)
{
    if (left == 0 || right == 0) {
        *result = 0;
        return true;
    }
    if ((left == LONG_MIN && right == -1) ||
        (right == LONG_MIN && left == -1)) {
        return false;
    }
    if (left > 0) {
        if ((right > 0 && left > LONG_MAX / right) ||
            (right < 0 && right < LONG_MIN / left)) {
            return false;
        }
    } else if ((right > 0 && left < LONG_MIN / right) ||
               (right < 0 && left < LONG_MAX / right)) {
        return false;
    }
    *result = left * right;
    return true;
}

static bool arithmetic_assignment_target(
    arithmetic_parser *parser, size_t *name_begin, size_t *name_length,
    arithmetic_assignment_operator *operator)
{
    size_t cursor = parser->offset;
    size_t parentheses = 0;
    size_t operator_length = 0;
    unsigned char byte;

    arithmetic_skip_offset(parser, &cursor);
    while (cursor < parser->end && parser->input[cursor] == '(') {
        if (parser->nesting + parentheses == 32U) {
            return false;
        }
        parentheses++;
        cursor++;
        arithmetic_skip_offset(parser, &cursor);
    }
    if (cursor == parser->end ||
        !name_start((unsigned char)parser->input[cursor])) {
        return false;
    }
    *name_begin = cursor++;
    while (cursor < parser->end &&
           name_byte((unsigned char)parser->input[cursor])) {
        cursor++;
    }
    *name_length = cursor - *name_begin;
    while (parentheses > 0) {
        arithmetic_skip_offset(parser, &cursor);
        if (cursor == parser->end || parser->input[cursor++] != ')') {
            return false;
        }
        parentheses--;
    }
    arithmetic_skip_offset(parser, &cursor);
    if (cursor == parser->end) {
        return false;
    }
    byte = (unsigned char)parser->input[cursor];
    if ((byte == '<' || byte == '>') && cursor + 2U < parser->end &&
        parser->input[cursor + 1U] == (char)byte &&
        parser->input[cursor + 2U] == '=') {
        *operator = byte == '<' ? ARITHMETIC_ASSIGN_SHIFT_LEFT
                                : ARITHMETIC_ASSIGN_SHIFT_RIGHT;
        operator_length = 3U;
    } else if (cursor + 1U < parser->end &&
               parser->input[cursor + 1U] == '=') {
        switch (byte) {
        case '*': *operator = ARITHMETIC_ASSIGN_MULTIPLY; break;
        case '/': *operator = ARITHMETIC_ASSIGN_DIVIDE; break;
        case '%': *operator = ARITHMETIC_ASSIGN_REMAINDER; break;
        case '+': *operator = ARITHMETIC_ASSIGN_ADD; break;
        case '-': *operator = ARITHMETIC_ASSIGN_SUBTRACT; break;
        case '&': *operator = ARITHMETIC_ASSIGN_AND; break;
        case '^': *operator = ARITHMETIC_ASSIGN_XOR; break;
        case '|': *operator = ARITHMETIC_ASSIGN_OR; break;
        default: return false;
        }
        operator_length = 2U;
    } else if (byte == '=' &&
               (cursor + 1U == parser->end ||
                parser->input[cursor + 1U] != '=')) {
        *operator = ARITHMETIC_ASSIGN_SET;
        operator_length = 1U;
    } else {
        return false;
    }
    parser->offset = cursor + operator_length;
    return true;
}

static bool arithmetic_apply_assignment(
    arithmetic_assignment_operator operator, long left, long right,
    long *result)
{
    unsigned long bits = (unsigned long)(sizeof(long) * CHAR_BIT);

    switch (operator) {
    case ARITHMETIC_ASSIGN_SET:
        *result = right;
        return true;
    case ARITHMETIC_ASSIGN_MULTIPLY:
        return arithmetic_multiply(left, right, result);
    case ARITHMETIC_ASSIGN_DIVIDE:
    case ARITHMETIC_ASSIGN_REMAINDER:
        if (right == 0 || (left == LONG_MIN && right == -1)) {
            return false;
        }
        *result = operator == ARITHMETIC_ASSIGN_DIVIDE
                      ? left / right
                      : left % right;
        return true;
    case ARITHMETIC_ASSIGN_ADD:
        return arithmetic_add(left, right, result);
    case ARITHMETIC_ASSIGN_SUBTRACT:
        return arithmetic_subtract(left, right, result);
    case ARITHMETIC_ASSIGN_SHIFT_LEFT:
    case ARITHMETIC_ASSIGN_SHIFT_RIGHT:
        if (right < 0 || (unsigned long)right >= bits ||
            (operator == ARITHMETIC_ASSIGN_SHIFT_LEFT &&
             (left < 0 || left > (LONG_MAX >> (unsigned int)right)))) {
            return false;
        }
        *result = operator == ARITHMETIC_ASSIGN_SHIFT_LEFT
                      ? left << (unsigned int)right
                      : left >> (unsigned int)right;
        return true;
    case ARITHMETIC_ASSIGN_AND: *result = left & right; return true;
    case ARITHMETIC_ASSIGN_XOR: *result = left ^ right; return true;
    case ARITHMETIC_ASSIGN_OR: *result = left | right; return true;
    case ARITHMETIC_ASSIGN_NONE: return false;
    }
    return false;
}

static bool arithmetic_parse_comma(arithmetic_parser *parser,
                                   bool evaluate, long *value);
static gsh_native_plan_status reserve_arithmetic_value(
    char *output, size_t capacity, size_t *used, long value);

static bool arithmetic_parse_primary(arithmetic_parser *parser,
                                     bool evaluate, long *value)
{
    size_t begin;
    bool explicit_parameter = false;

    arithmetic_skip_space(parser);
    if (parser->offset == parser->end) {
        return false;
    }
    if (arithmetic_match(parser, "(")) {
        bool parsed;

        if (parser->nesting == 32U) {
            parser->error = GSH_NATIVE_PLAN_LIMIT;
            return false;
        }
        parser->nesting++;
        parsed = arithmetic_parse_comma(parser, evaluate, value) &&
                 arithmetic_match(parser, ")");
        parser->nesting--;
        if (!parsed) {
            return false;
        }
        return true;
    }
    if (parser->input[parser->offset] >= '0' &&
        parser->input[parser->offset] <= '9') {
        unsigned int base = 10;

        begin = parser->offset;
        if (parser->input[parser->offset] == '0') {
            base = 8;
            parser->offset++;
            if (parser->offset < parser->end &&
                (parser->input[parser->offset] == 'x' ||
                 parser->input[parser->offset] == 'X')) {
                base = 16;
                parser->offset++;
            }
        }
        while (parser->offset < parser->end) {
            int digit = arithmetic_digit(
                (unsigned char)parser->input[parser->offset]);

            if (digit < 0 || (unsigned int)digit >= base) {
                break;
            }
            parser->offset++;
        }
        if (!arithmetic_constant(parser->input + begin,
                                 parser->offset - begin, value)) {
            return arithmetic_failure(
                parser, "integer constant is invalid or out of range");
        }
        return true;
    }
    begin = parser->offset;
    if (parser->input[parser->offset] == '$') {
        explicit_parameter = true;
        parser->offset++;
        if (parser->offset < parser->end &&
            parser->input[parser->offset] == '{') {
            parser->offset++;
            begin = parser->offset;
            if (begin == parser->end ||
                !name_start((unsigned char)parser->input[begin])) {
                return false;
            }
            parser->offset++;
            while (parser->offset < parser->end &&
                   name_byte((unsigned char)parser->input[parser->offset])) {
                parser->offset++;
            }
            if (parser->offset == parser->end ||
                parser->input[parser->offset++] != '}') {
                return false;
            }
            return arithmetic_environment_value(
                parser, parser->input + begin,
                parser->offset - begin - 1U,
                evaluate || explicit_parameter, value);
        }
        begin = parser->offset;
    }
    if (begin < parser->end &&
        name_start((unsigned char)parser->input[begin])) {
        parser->offset = begin + 1U;
        while (parser->offset < parser->end &&
               name_byte((unsigned char)parser->input[parser->offset])) {
            parser->offset++;
        }
        return arithmetic_environment_value(
            parser, parser->input + begin, parser->offset - begin,
            evaluate || explicit_parameter, value);
    }
    return false;
}

static bool arithmetic_parse_unary(arithmetic_parser *parser,
                                   bool evaluate, long *value)
{
    char operators[128];
    size_t count = 0;

    for (;;) {
        char operator;

        if (arithmetic_match(parser, "+")) {
            operator = '+';
        } else if (arithmetic_match(parser, "-")) {
            operator = '-';
        } else if (arithmetic_match(parser, "!")) {
            operator = '!';
        } else if (arithmetic_match(parser, "~")) {
            operator = '~';
        } else {
            break;
        }
        if (count == sizeof(operators)) {
            parser->error = GSH_NATIVE_PLAN_LIMIT;
            return false;
        }
        operators[count++] = operator;
    }
    if (!arithmetic_parse_primary(parser, evaluate, value)) {
        return false;
    }
    while (count > 0) {
        char operator = operators[--count];

        if (!evaluate || operator == '+') {
            continue;
        }
        if (operator == '-') {
            if (*value == LONG_MIN) {
                return arithmetic_failure(parser, "integer overflow");
            }
            *value = -*value;
        } else if (operator == '!') {
            *value = *value == 0 ? 1 : 0;
        } else {
            *value = ~*value;
        }
    }
    return true;
}

static bool arithmetic_parse_multiply(arithmetic_parser *parser,
                                      bool evaluate, long *value)
{
    long right;

    if (!arithmetic_parse_unary(parser, evaluate, value)) {
        return false;
    }
    for (;;) {
        char operator;

        arithmetic_skip_space(parser);
        if (parser->offset == parser->end ||
            (parser->input[parser->offset] != '*' &&
             parser->input[parser->offset] != '/' &&
             parser->input[parser->offset] != '%') ||
            (parser->offset + 1U < parser->end &&
             parser->input[parser->offset + 1U] == '=')) {
            return true;
        }
        operator = parser->input[parser->offset++];
        if (!arithmetic_parse_unary(parser, evaluate, &right)) {
            return false;
        }
        if (!evaluate) {
            continue;
        }
        if (operator == '*') {
            if (!arithmetic_multiply(*value, right, value)) {
                return arithmetic_failure(parser, "integer overflow");
            }
        } else if (right == 0) {
            return arithmetic_failure(parser, "division by zero");
        } else if (*value == LONG_MIN && right == -1) {
            return arithmetic_failure(parser, "integer overflow");
        } else if (operator == '/') {
            *value /= right;
        } else {
            *value %= right;
        }
    }
}

static bool arithmetic_parse_add(arithmetic_parser *parser, bool evaluate,
                                 long *value)
{
    long right;

    if (!arithmetic_parse_multiply(parser, evaluate, value)) {
        return false;
    }
    for (;;) {
        char operator;

        arithmetic_skip_space(parser);
        if (parser->offset == parser->end ||
            (parser->input[parser->offset] != '+' &&
             parser->input[parser->offset] != '-') ||
            (parser->offset + 1U < parser->end &&
             parser->input[parser->offset + 1U] == '=')) {
            return true;
        }
        operator = parser->input[parser->offset++];
        if (!arithmetic_parse_multiply(parser, evaluate, &right)) {
            return false;
        }
        if (evaluate &&
            !(operator == '+' ? arithmetic_add(*value, right, value)
                              : arithmetic_subtract(*value, right, value))) {
            return arithmetic_failure(parser, "integer overflow");
        }
    }
}

static bool arithmetic_parse_shift(arithmetic_parser *parser,
                                   bool evaluate, long *value)
{
    long right;

    if (!arithmetic_parse_add(parser, evaluate, value)) {
        return false;
    }
    for (;;) {
        bool left;

        if (arithmetic_match_binary(parser, "<<")) {
            left = true;
        } else if (arithmetic_match_binary(parser, ">>")) {
            left = false;
        } else {
            return true;
        }
        if (!arithmetic_parse_add(parser, evaluate, &right)) {
            return false;
        }
        if (evaluate) {
            unsigned long bits = (unsigned long)(sizeof(long) * CHAR_BIT);

            if (right < 0 || (unsigned long)right >= bits ||
                (left && (*value < 0 ||
                          *value > (LONG_MAX >> (unsigned int)right)))) {
                return arithmetic_failure(
                    parser, "shift count or value is out of range");
            }
            *value = left ? *value << (unsigned int)right
                          : *value >> (unsigned int)right;
        }
    }
}

static bool arithmetic_parse_relational(arithmetic_parser *parser,
                                        bool evaluate, long *value)
{
    long right;

    if (!arithmetic_parse_shift(parser, evaluate, value)) {
        return false;
    }
    for (;;) {
        enum { REL_NONE, REL_LT, REL_LE, REL_GT, REL_GE } operator = REL_NONE;

        if (arithmetic_match(parser, "<=")) {
            operator = REL_LE;
        } else if (arithmetic_match(parser, ">=")) {
            operator = REL_GE;
        } else {
            arithmetic_skip_space(parser);
            if (parser->offset < parser->end &&
                parser->input[parser->offset] == '<' &&
                (parser->offset + 1U == parser->end ||
                 parser->input[parser->offset + 1U] != '<')) {
                parser->offset++;
                operator = REL_LT;
            } else if (parser->offset < parser->end &&
                       parser->input[parser->offset] == '>' &&
                       (parser->offset + 1U == parser->end ||
                        parser->input[parser->offset + 1U] != '>')) {
                parser->offset++;
                operator = REL_GT;
            }
        }
        if (operator == REL_NONE) {
            return true;
        }
        if (!arithmetic_parse_shift(parser, evaluate, &right)) {
            return false;
        }
        if (evaluate) {
            *value = operator == REL_LT   ? *value < right
                     : operator == REL_LE ? *value <= right
                     : operator == REL_GT ? *value > right
                                          : *value >= right;
        }
    }
}

static bool arithmetic_parse_equality(arithmetic_parser *parser,
                                      bool evaluate, long *value)
{
    long right;

    if (!arithmetic_parse_relational(parser, evaluate, value)) {
        return false;
    }
    for (;;) {
        bool equal;

        if (arithmetic_match(parser, "==")) {
            equal = true;
        } else if (arithmetic_match(parser, "!=")) {
            equal = false;
        } else {
            return true;
        }
        if (!arithmetic_parse_relational(parser, evaluate, &right)) {
            return false;
        }
        if (evaluate) {
            *value = equal ? *value == right : *value != right;
        }
    }
}

static bool arithmetic_parse_bitand(arithmetic_parser *parser,
                                    bool evaluate, long *value)
{
    long right;

    if (!arithmetic_parse_equality(parser, evaluate, value)) {
        return false;
    }
    for (;;) {
        arithmetic_skip_space(parser);
        if (parser->offset == parser->end ||
            parser->input[parser->offset] != '&' ||
            (parser->offset + 1U < parser->end &&
             (parser->input[parser->offset + 1U] == '&' ||
              parser->input[parser->offset + 1U] == '='))) {
            return true;
        }
        parser->offset++;
        if (!arithmetic_parse_equality(parser, evaluate, &right)) {
            return false;
        }
        if (evaluate) {
            *value &= right;
        }
    }
}

static bool arithmetic_parse_bitxor(arithmetic_parser *parser,
                                    bool evaluate, long *value)
{
    long right;

    if (!arithmetic_parse_bitand(parser, evaluate, value)) {
        return false;
    }
    while (arithmetic_match_binary(parser, "^")) {
        if (!arithmetic_parse_bitand(parser, evaluate, &right)) {
            return false;
        }
        if (evaluate) {
            *value ^= right;
        }
    }
    return true;
}

static bool arithmetic_parse_bitor(arithmetic_parser *parser,
                                   bool evaluate, long *value)
{
    long right;

    if (!arithmetic_parse_bitxor(parser, evaluate, value)) {
        return false;
    }
    for (;;) {
        arithmetic_skip_space(parser);
        if (parser->offset == parser->end ||
            parser->input[parser->offset] != '|' ||
            (parser->offset + 1U < parser->end &&
             (parser->input[parser->offset + 1U] == '|' ||
              parser->input[parser->offset + 1U] == '='))) {
            return true;
        }
        parser->offset++;
        if (!arithmetic_parse_bitxor(parser, evaluate, &right)) {
            return false;
        }
        if (evaluate) {
            *value |= right;
        }
    }
}

static bool arithmetic_parse_logical_and(arithmetic_parser *parser,
                                         bool evaluate, long *value)
{
    long right;

    if (!arithmetic_parse_bitor(parser, evaluate, value)) {
        return false;
    }
    while (arithmetic_match(parser, "&&")) {
        bool evaluate_right = evaluate && *value != 0;

        if (!arithmetic_parse_bitor(parser, evaluate_right, &right)) {
            return false;
        }
        if (evaluate) {
            *value = *value != 0 && right != 0;
        }
    }
    return true;
}

static bool arithmetic_parse_logical_or(arithmetic_parser *parser,
                                        bool evaluate, long *value)
{
    long right;

    if (!arithmetic_parse_logical_and(parser, evaluate, value)) {
        return false;
    }
    while (arithmetic_match(parser, "||")) {
        bool evaluate_right = evaluate && *value == 0;

        if (!arithmetic_parse_logical_and(parser, evaluate_right, &right)) {
            return false;
        }
        if (evaluate) {
            *value = *value != 0 || right != 0;
        }
    }
    return true;
}

static bool arithmetic_parse_conditional(arithmetic_parser *parser,
                                         bool evaluate, long *value)
{
    long when_true;
    long when_false;
    bool condition;

    if (!arithmetic_parse_logical_or(parser, evaluate, value)) {
        return false;
    }
    if (!arithmetic_match(parser, "?")) {
        return true;
    }
    if (parser->nesting == 32U) {
        parser->error = GSH_NATIVE_PLAN_LIMIT;
        return false;
    }
    parser->nesting++;
    condition = evaluate && *value != 0;
    if (!arithmetic_parse_comma(parser, condition, &when_true) ||
        !arithmetic_match(parser, ":") ||
        !arithmetic_parse_conditional(parser, evaluate && !condition,
                                      &when_false)) {
        parser->nesting--;
        return false;
    }
    parser->nesting--;
    if (evaluate) {
        *value = condition ? when_true : when_false;
    }
    return true;
}

static bool arithmetic_parse_assignment(arithmetic_parser *parser,
                                        bool evaluate, long *value)
{
    size_t saved = parser->offset;
    size_t name_begin;
    size_t name_length;
    arithmetic_assignment_operator operator = ARITHMETIC_ASSIGN_NONE;
    long left = 0;
    long right;
    char text[3U * sizeof(long) + 2U];
    size_t text_length = 0;
    gsh_native_plan_status status;

    if (!arithmetic_assignment_target(parser, &name_begin, &name_length,
                                      &operator)) {
        parser->offset = saved;
        return arithmetic_parse_conditional(parser, evaluate, value);
    }
    if (evaluate && operator != ARITHMETIC_ASSIGN_SET &&
        !arithmetic_environment_value(parser, parser->input + name_begin,
                                      name_length, true, &left)) {
        return false;
    }
    if (parser->nesting == 32U) {
        parser->error = GSH_NATIVE_PLAN_LIMIT;
        return false;
    }
    parser->nesting++;
    if (!arithmetic_parse_assignment(parser, evaluate, &right)) {
        parser->nesting--;
        return false;
    }
    parser->nesting--;
    if (!evaluate) {
        *value = 0;
        return true;
    }
    if (!arithmetic_apply_assignment(operator, left, right, value)) {
        return arithmetic_failure(
            parser,
            (operator == ARITHMETIC_ASSIGN_DIVIDE ||
             operator == ARITHMETIC_ASSIGN_REMAINDER) && right == 0
                ? "division by zero"
                : (operator == ARITHMETIC_ASSIGN_SHIFT_LEFT ||
                   operator == ARITHMETIC_ASSIGN_SHIFT_RIGHT)
                      ? "shift count or value is out of range"
                      : "integer overflow");
    }
    if (parser->context == NULL ||
        parser->context->variable_assign == NULL) {
        return false;
    }
    status = reserve_arithmetic_value(text, sizeof(text), &text_length,
                                      *value);
    if (status == GSH_NATIVE_PLAN_OK) {
        status = parser->context->variable_assign(
            parser->context->variable_opaque, parser->input + name_begin,
            name_length, text, text_length);
    }
    if (status != GSH_NATIVE_PLAN_OK) {
        parser->error = status;
        return false;
    }
    return true;
}

static bool arithmetic_parse_comma(arithmetic_parser *parser,
                                   bool evaluate, long *value)
{
    if (!arithmetic_parse_assignment(parser, evaluate, value)) {
        return false;
    }
    while (arithmetic_match(parser, ",")) {
        if (!arithmetic_parse_assignment(parser, evaluate, value)) {
            return false;
        }
    }
    return true;
}

static gsh_native_plan_status reserve_arithmetic_value(
    char *output, size_t capacity, size_t *used, long value)
{
    unsigned char digits[3U * sizeof(value) + 1U];
    unsigned long magnitude;
    size_t count = 0;

    if (value < 0) {
        if (*used == capacity) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        output[(*used)++] = '-';
        magnitude = (unsigned long)(-(value + 1L)) + 1UL;
    } else {
        magnitude = (unsigned long)value;
    }
    do {
        digits[count++] = (unsigned char)('0' + magnitude % 10UL);
        magnitude /= 10UL;
    } while (magnitude != 0);
    if (count > capacity - *used) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    while (count > 0) {
        output[(*used)++] = (char)digits[--count];
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status arithmetic_diagnostic(
    const arithmetic_parser *parser)
{
    const char *message = parser->diagnostic != NULL
                              ? parser->diagnostic
                              : "invalid expression";

    if (parser->context == NULL) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (parser->context->expansion_error == NULL) {
        return GSH_NATIVE_PLAN_ERROR;
    }
    return parser->context->expansion_error(
        parser->context->variable_opaque, message, strlen(message));
}

static gsh_native_plan_status expand_arithmetic(
    const char *input, size_t end, size_t *offset,
    const gsh_native_expansion_context *context, size_t depth,
    gsh_native_pipeline *pipeline, char *output, size_t output_capacity,
    size_t *output_used)
{
    arithmetic_parser parser;
    size_t expression_end;
    size_t after;
    size_t expression_begin = pipeline->text_used;
    size_t expression_length;
    size_t cursor;
    char *expression;
    bool deferred = false;
    bool needs_expansion = false;
    long value = 0;
    gsh_native_plan_status status;

    if (*offset + 1U >= end || input[*offset] != '(' ||
        input[*offset + 1U] != '(') {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    status = find_substitution_end(input, end, *offset, true,
                                   &expression_end, &after);
    if (status != GSH_NATIVE_PLAN_OK) {
        return status;
    }
    if (depth == 32U) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    for (cursor = *offset + 2U; cursor < expression_end; cursor++) {
        unsigned char byte = (unsigned char)input[cursor];

        if (byte == '$' || byte == 0x60U || byte == '\\' || byte == '\'' ||
            byte == '"') {
            needs_expansion = true;
        }
        if (!deferred && (byte == '$' || byte == 0x60U) &&
            context != NULL && context->preflight &&
            context->deferred_work != NULL) {
            *context->deferred_work = true;
            deferred = true;
        }
    }
    if (needs_expansion) {
        status = expand_static_word(
            input, (gsh_word_ref){*offset + 2U, expression_end}, context,
            GSH_EXPAND_ARITHMETIC, depth + 1U, pipeline, &expression,
            &expression_length);
        if (status != GSH_NATIVE_PLAN_OK) {
            pipeline->text_used = expression_begin;
            return status;
        }
        parser.input = expression;
        parser.offset = 0;
        parser.end = expression_length;
    } else {
        parser.input = input;
        parser.offset = *offset + 2U;
        parser.end = expression_end;
    }
    parser.context = context;
    parser.nesting = 0;
    parser.error = GSH_NATIVE_PLAN_OK;
    parser.diagnostic = NULL;
    if (!arithmetic_parse_comma(&parser, true, &value)) {
        if (deferred && parser.error == GSH_NATIVE_PLAN_OK) {
            value = 0;
            goto reserve_result;
        }
        status = parser.error == GSH_NATIVE_PLAN_OK
                     ? arithmetic_diagnostic(&parser)
                     : parser.error;
        if (status == GSH_NATIVE_PLAN_OK && context != NULL &&
            context->preflight) {
            value = 0;
            goto reserve_result;
        }
        pipeline->text_used = expression_begin;
        return status == GSH_NATIVE_PLAN_OK ? GSH_NATIVE_PLAN_ERROR
                                            : status;
    }
    arithmetic_skip_space(&parser);
    if (parser.offset != parser.end) {
        if (deferred) {
            value = 0;
            goto reserve_result;
        }
        status = arithmetic_diagnostic(&parser);
        if (status == GSH_NATIVE_PLAN_OK && context != NULL &&
            context->preflight) {
            value = 0;
            goto reserve_result;
        }
        pipeline->text_used = expression_begin;
        return status == GSH_NATIVE_PLAN_OK ? GSH_NATIVE_PLAN_ERROR
                                            : status;
    }
reserve_result:
    pipeline->text_used = expression_begin;
    status = reserve_arithmetic_value(output, output_capacity, output_used,
                                      value);
    if (status == GSH_NATIVE_PLAN_OK) {
        *offset = after;
    }
    return status;
}

static gsh_native_plan_status
expand_static_word(const char *input, gsh_word_ref word,
                   const gsh_native_expansion_context *context,
                   gsh_expansion_mode mode, size_t depth,
                   gsh_native_pipeline *pipeline, char **expanded,
                   size_t *expanded_length)
{
    enum {
        QUOTE_NONE,
        QUOTE_SINGLE,
        QUOTE_DOUBLE,
    } quote = QUOTE_NONE;
    size_t offset = word.begin;
    size_t expanded_begin;

    if (pipeline->text_used == GSH_NATIVE_TEXT_CAP) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    *expanded = pipeline->text + pipeline->text_used;
    expanded_begin = pipeline->text_used;
    while (offset < word.end) {
        unsigned char byte = (unsigned char)input[offset++];

        if (quote == QUOTE_SINGLE) {
            if (byte == '\'') {
                quote = QUOTE_NONE;
            } else if (!reserve_byte(pipeline, byte)) {
                return GSH_NATIVE_PLAN_LIMIT;
            } else {
                pipeline->provenance[pipeline->text_used - 1U] =
                    GSH_WORD_QUOTED;
            }
            continue;
        }
        if (quote == QUOTE_DOUBLE) {
            if (byte == '"') {
                quote = QUOTE_NONE;
                continue;
            }
            if (byte == '$') {
                gsh_native_plan_status status;
                size_t value_begin = pipeline->text_used;
                bool parameter_expansion = false;

                if (offset < word.end && input[offset] == '\'') {
                    if (!reserve_byte(pipeline, '$')) {
                        return GSH_NATIVE_PLAN_LIMIT;
                    }
                    pipeline->provenance[pipeline->text_used - 1U] =
                        GSH_WORD_QUOTED;
                    continue;
                }
                if (offset < word.end && input[offset] == '(') {
                    status = offset + 1U < word.end &&
                                     input[offset + 1U] == '('
                                 ? expand_arithmetic(
                                       input, word.end, &offset, context,
                                       depth, pipeline, pipeline->text,
                                       GSH_NATIVE_TEXT_CAP,
                                       &pipeline->text_used)
                                 : expand_command_substitution(
                                       input, word.end, &offset, context,
                                       pipeline->text, GSH_NATIVE_TEXT_CAP,
                                       &pipeline->text_used);
                } else if (offset == word.end || input[offset] == ' ' ||
                           input[offset] == '\t' || input[offset] == '\n') {
                    if (!reserve_byte(pipeline, '$')) {
                        return GSH_NATIVE_PLAN_LIMIT;
                    }
                    mark_reserved(pipeline, value_begin, GSH_WORD_QUOTED);
                    continue;
                } else {
                    parameter_expansion = true;
                    status = expand_parameter(
                        input, word.end, &offset, true, mode, depth,
                        context, pipeline);
                }

                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
                if (!parameter_expansion) {
                    mark_reserved(pipeline, value_begin,
                                  GSH_WORD_EXPANDED | GSH_WORD_QUOTED);
                }
                continue;
            }
            if (byte == 0x60U) {
                size_t value_begin = pipeline->text_used;
                gsh_native_plan_status status =
                    expand_backquote_substitution(
                        input, word.end, &offset, true, context,
                        pipeline->text, GSH_NATIVE_TEXT_CAP,
                        &pipeline->text_used);

                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
                mark_reserved(pipeline, value_begin,
                              GSH_WORD_EXPANDED | GSH_WORD_QUOTED);
                continue;
            }
            if (byte == '\\' && offset < word.end) {
                unsigned char next = (unsigned char)input[offset];

                if (next == '$' || next == 0x60U || next == '"' ||
                    next == '\\' || next == '\n') {
                    offset++;
                    if (next == '\n') {
                        continue;
                    }
                    byte = next;
                }
            }
            if (!reserve_byte(pipeline, byte)) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            pipeline->provenance[pipeline->text_used - 1U] =
                GSH_WORD_QUOTED;
            continue;
        }

        if (byte == '\'' && mode != GSH_EXPAND_ARITHMETIC) {
            quote = QUOTE_SINGLE;
            if (mode == GSH_EXPAND_FIELDS &&
                !reserve_empty_marker(pipeline)) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
        } else if (byte == '"' && mode != GSH_EXPAND_ARITHMETIC) {
            quote = QUOTE_DOUBLE;
            if (mode == GSH_EXPAND_FIELDS &&
                !reserve_empty_marker(pipeline)) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
        } else if (byte == '\\') {
            if (offset == word.end) {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            if (mode == GSH_EXPAND_ARITHMETIC &&
                input[offset] != '$' && input[offset] != 0x60 &&
                input[offset] != '\\' && input[offset] != '\n') {
                byte = '\\';
            } else {
                byte = (unsigned char)input[offset++];
            }
            if (byte != '\n' && !reserve_byte(pipeline, byte)) {
                return GSH_NATIVE_PLAN_LIMIT;
            } else if (byte != '\n') {
                pipeline->provenance[pipeline->text_used - 1U] =
                    GSH_WORD_QUOTED;
            }
        } else if (byte == '$') {
            gsh_native_plan_status status;
            size_t value_begin;

            if (mode != GSH_EXPAND_ARITHMETIC && offset < word.end &&
                input[offset] == '\'') {
                offset++;
                if (mode == GSH_EXPAND_FIELDS &&
                    !reserve_empty_marker(pipeline)) {
                    return GSH_NATIVE_PLAN_LIMIT;
                }
                value_begin = pipeline->text_used;
                status = expand_dollar_single_quote(input, word.end, &offset,
                                                    pipeline);
                if (status == GSH_NATIVE_PLAN_OK) {
                    mark_reserved(pipeline, value_begin, GSH_WORD_QUOTED);
                }
            } else if (offset < word.end && input[offset] == '(') {
                value_begin = pipeline->text_used;
                if (offset + 1U < word.end &&
                    input[offset + 1U] == '(') {
                    status = expand_arithmetic(
                        input, word.end, &offset, context, depth, pipeline,
                        pipeline->text, GSH_NATIVE_TEXT_CAP,
                        &pipeline->text_used);
                } else {
                    status = expand_command_substitution(
                        input, word.end, &offset, context, pipeline->text,
                        GSH_NATIVE_TEXT_CAP, &pipeline->text_used);
                }
                if (status == GSH_NATIVE_PLAN_OK) {
                    mark_reserved(pipeline, value_begin,
                                  GSH_WORD_EXPANDED);
                }
            } else if (offset == word.end || input[offset] == ' ' ||
                       input[offset] == '\t' || input[offset] == '\n') {
                status = reserve_byte(pipeline, '$')
                             ? GSH_NATIVE_PLAN_OK
                             : GSH_NATIVE_PLAN_LIMIT;
            } else {
                status = expand_parameter(
                    input, word.end, &offset,
                    mode == GSH_EXPAND_ARITHMETIC,
                    mode == GSH_EXPAND_ARITHMETIC ? GSH_EXPAND_SCALAR
                                                  : mode,
                    depth, context, pipeline);
            }

            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
        } else if (byte == '~' && mode != GSH_EXPAND_ARITHMETIC &&
                   offset == word.begin + 1U) {
            bool home_found;
            const char *home;

            if (offset < word.end && input[offset] != '/') {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            if (context == NULL) {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            home = lookup_variable(context, "HOME", 4, &home_found);
            if (home == NULL || !home_found) {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            {
                size_t value_begin = pipeline->text_used;
                gsh_native_plan_status status =
                    reserve_parameter_value(pipeline, home);

                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
                mark_reserved(pipeline, value_begin, GSH_WORD_QUOTED);
                if (mode == GSH_EXPAND_FIELDS &&
                    pipeline->text_used == value_begin &&
                    !reserve_empty_marker(pipeline)) {
                    return GSH_NATIVE_PLAN_LIMIT;
                }
            }
        } else if (byte == 0x60U) {
            size_t value_begin = pipeline->text_used;
            gsh_native_plan_status status = expand_backquote_substitution(
                input, word.end, &offset,
                mode == GSH_EXPAND_ARITHMETIC, context, pipeline->text,
                GSH_NATIVE_TEXT_CAP, &pipeline->text_used);

            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
            mark_reserved(pipeline, value_begin, GSH_WORD_EXPANDED);
        } else if (!reserve_byte(pipeline, byte)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
    }
    if (quote != QUOTE_NONE) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (expanded_length != NULL) {
        *expanded_length = pipeline->text_used - expanded_begin;
    }
    if (!reserve_byte(pipeline, '\0')) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    return GSH_NATIVE_PLAN_OK;
}

gsh_native_plan_status gsh_native_expand_scalar(
    const char *input, gsh_word_ref word,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *scratch, char **expanded)
{
    size_t expanded_length;

    scratch->text_used = 0;
    return expand_static_word(input, word, context, GSH_EXPAND_SCALAR, 0,
                              scratch, expanded, &expanded_length);
}

static int default_redirect_descriptor(gsh_token_kind operator_kind)
{
    if (operator_kind == GSH_TOKEN_LESS ||
        operator_kind == GSH_TOKEN_LESSAND ||
        operator_kind == GSH_TOKEN_LESSGREAT ||
        operator_kind == GSH_TOKEN_DLESS ||
        operator_kind == GSH_TOKEN_DLESSDASH) {
        return 0;
    }
    return 1;
}

static bool parse_descriptor(const char *text, int *descriptor)
{
    int value = 0;

    if (*text == '\0') {
        return false;
    }
    while (*text != '\0') {
        unsigned int digit;

        if (*text < '0' || *text > '9') {
            return false;
        }
        digit = (unsigned int)(*text++ - '0');
        if (value > (INT_MAX - (int)digit) / 10) {
            return false;
        }
        value = value * 10 + (int)digit;
    }
    *descriptor = value;
    return true;
}

static gsh_native_plan_status
reserve_heredoc_byte(gsh_native_pipeline *pipeline, unsigned char byte)
{
    if (pipeline->heredoc_text_used == GSH_NATIVE_HEREDOC_TEXT_CAP) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    pipeline->heredoc_text[pipeline->heredoc_text_used++] = (char)byte;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status
reserve_heredoc_value(gsh_native_pipeline *pipeline, const char *value)
{
    while (*value != '\0') {
        gsh_native_plan_status status = reserve_heredoc_byte(
            pipeline, (unsigned char)*value++);

        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status reserve_heredoc_positionals(
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline)
{
    const char *separator;
    size_t separator_length;
    size_t index;
    gsh_native_plan_status status = positional_separator(
        context, &separator, &separator_length);

    if (status != GSH_NATIVE_PLAN_OK) {
        return status;
    }
    for (index = 0; index < context->positional_count; index++) {
        size_t byte;

        if (index != 0) {
            for (byte = 0; byte < separator_length; byte++) {
                status = reserve_heredoc_byte(
                    pipeline, (unsigned char)separator[byte]);
                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
            }
        }
        status = reserve_heredoc_value(
            pipeline, context->positional_parameters[index]);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status expand_heredoc_parameter(
    const char *input, size_t end, size_t *offset,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline)
{
    char numeric[32];
    const char *value;
    bool found;
    gsh_parameter_kind kind;
    size_t name_begin = *offset;
    size_t after;
    gsh_native_plan_status status;

    if (context == NULL || *offset == end) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (input[*offset] == '{') {
        size_t closing;

        name_begin++;
        status = find_parameter_end(input, end, *offset, &closing);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        status = parameter_reference(input, closing, *offset + 1U,
                                     context, true, &after, &value, &found,
                                     &kind, numeric);
        if (status != GSH_NATIVE_PLAN_OK || after != closing) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        *offset = closing + 1U;
    } else {
        status = parameter_reference(input, end, *offset, context, false,
                                     &after, &value, &found, &kind,
                                     numeric);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        *offset = after;
    }
    if (kind == GSH_PARAMETER_VALUE && !found) {
        status = report_unset_parameter(
            context, input + name_begin, after - name_begin);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
    }
    return kind == GSH_PARAMETER_VALUE
               ? reserve_heredoc_value(pipeline, value)
               : reserve_heredoc_positionals(context, pipeline);
}

static gsh_native_plan_status plan_heredoc(
    const char *input, const gsh_redirect *source,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline, size_t *heredoc_index)
{
    gsh_native_heredoc *heredoc;
    size_t offset = source->body.begin;
    bool at_line_start = true;
    bool quoted = (source->flags & GSH_REDIRECT_HEREDOC_QUOTED) != 0;
    bool strip_tabs =
        (source->flags & GSH_REDIRECT_HEREDOC_STRIP_TABS) != 0;

    if (pipeline->heredoc_count == GSH_NATIVE_HEREDOC_CAP) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    *heredoc_index = pipeline->heredoc_count++;
    heredoc = &pipeline->heredocs[*heredoc_index];
    heredoc->body = pipeline->heredoc_text + pipeline->heredoc_text_used;
    while (offset < source->body.end) {
        unsigned char byte = (unsigned char)input[offset++];
        gsh_native_plan_status status;

        if (strip_tabs && at_line_start && byte == '\t') {
            continue;
        }
        if (!quoted && byte == '\\' && offset < source->body.end) {
            unsigned char next = (unsigned char)input[offset];

            if (next == '$' || next == 0x60U || next == '\\' ||
                next == '\n') {
                offset++;
                if (next == '\n') {
                    continue;
                }
                byte = next;
            }
        } else if (!quoted && byte == '$') {
            if (offset < source->body.end && input[offset] == '(') {
                status = offset + 1U < source->body.end &&
                                 input[offset + 1U] == '('
                             ? expand_arithmetic(
                                   input, source->body.end, &offset, context,
                                   0, pipeline,
                                   pipeline->heredoc_text,
                                   GSH_NATIVE_HEREDOC_TEXT_CAP,
                                   &pipeline->heredoc_text_used)
                             : expand_command_substitution(
                                   input, source->body.end, &offset, context,
                                   pipeline->heredoc_text,
                                   GSH_NATIVE_HEREDOC_TEXT_CAP,
                                   &pipeline->heredoc_text_used);
            } else {
                status = expand_heredoc_parameter(
                    input, source->body.end, &offset, context, pipeline);
            }
            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
            at_line_start = false;
            continue;
        } else if (!quoted && byte == 0x60U) {
            status = expand_backquote_substitution(
                input, source->body.end, &offset, false, context,
                pipeline->heredoc_text, GSH_NATIVE_HEREDOC_TEXT_CAP,
                &pipeline->heredoc_text_used);
            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
            at_line_start = false;
            continue;
        }
        status = reserve_heredoc_byte(pipeline, byte);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        at_line_start = byte == '\n';
    }
    heredoc->length = (size_t)(pipeline->heredoc_text +
                               pipeline->heredoc_text_used - heredoc->body);
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status
plan_redirect(const char *input, const gsh_redirect *source,
              const gsh_native_expansion_context *context,
              gsh_native_pipeline *pipeline, gsh_native_redirect *target)
{
    gsh_native_plan_status status;

    memset(target, 0, sizeof(*target));
    target->descriptor = source->descriptor >= 0
                             ? source->descriptor
                             : default_redirect_descriptor(
                                   source->operator_kind);
    target->operator_kind = source->operator_kind;
    target->duplicate_descriptor = -1;
    target->heredoc_index = 0;
    if (source->operator_kind == GSH_TOKEN_DLESS ||
        source->operator_kind == GSH_TOKEN_DLESSDASH) {
        return plan_heredoc(input, source, context, pipeline,
                            &target->heredoc_index);
    }
    status = expand_static_word(input, source->target, context,
                                GSH_EXPAND_SCALAR, 0, pipeline,
                                &target->target, NULL);
    if (status != GSH_NATIVE_PLAN_OK) {
        return status;
    }
    if (source->operator_kind == GSH_TOKEN_LESSAND ||
        source->operator_kind == GSH_TOKEN_GREATAND) {
        if (strcmp(target->target, "-") == 0) {
            target->close_descriptor = true;
        } else if (!parse_descriptor(target->target,
                                     &target->duplicate_descriptor)) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status
plan_assignment(const char *input, gsh_word_ref word, size_t name_length,
                const gsh_native_expansion_context *context,
                gsh_native_pipeline *pipeline, char **assignment)
{
    gsh_word_ref value = {word.begin + name_length + 1U, word.end};
    size_t offset;
    if (pipeline->text_used == GSH_NATIVE_TEXT_CAP) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    *assignment = pipeline->text + pipeline->text_used;
    for (offset = word.begin; offset < word.begin + name_length; offset++) {
        if (!reserve_byte(pipeline, (unsigned char)input[offset])) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
    }
    if (!reserve_byte(pipeline, '=')) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    {
        char *expanded_value;

        return expand_static_word(input, value, context,
                                  GSH_EXPAND_SCALAR, 0, pipeline,
                                  &expanded_value, NULL);
    }
}

static size_t assignment_name_length(const char *input, gsh_word_ref word)
{
    size_t offset = word.begin;

    if (offset == word.end || !name_start((unsigned char)input[offset])) {
        return 0;
    }
    offset++;
    while (offset < word.end &&
           name_byte((unsigned char)input[offset])) {
        offset++;
    }
    return offset < word.end && input[offset] == '=' ? offset - word.begin
                                                      : 0;
}

static bool ifs_white_space(unsigned char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\n';
}

static bool is_ifs_byte(const char *ifs, unsigned char byte)
{
    return strchr(ifs, (int)byte) != NULL;
}

typedef struct {
    char *text;
    size_t provenance_offset;
    size_t length;
    bool has_pattern;
} gsh_expanded_field;

static bool split_delimiter(const gsh_native_pipeline *pipeline,
                            size_t offset, const char *ifs)
{
    unsigned char provenance = pipeline->provenance[offset];

    return (provenance & GSH_WORD_EXPANDED) != 0 &&
           (provenance & GSH_WORD_QUOTED) == 0 &&
           is_ifs_byte(ifs, (unsigned char)pipeline->text[offset]);
}

static gsh_native_plan_status emit_expanded_field(
    gsh_native_pipeline *pipeline,
    gsh_expanded_field fields[GSH_NATIVE_ARGUMENT_CAP],
    size_t *field_count, size_t field_begin, size_t *write,
    bool has_pattern)
{
    if (*field_count == GSH_NATIVE_ARGUMENT_CAP ||
        *write == GSH_NATIVE_TEXT_CAP) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    fields[*field_count].text = pipeline->text + field_begin;
    fields[*field_count].provenance_offset = field_begin;
    fields[*field_count].length = *write - field_begin;
    fields[*field_count].has_pattern = has_pattern;
    (*field_count)++;
    pipeline->text[*write] = '\0';
    pipeline->provenance[(*write)++] = 0;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status split_expanded_word(
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline, size_t begin, size_t length,
    gsh_expanded_field fields[GSH_NATIVE_ARGUMENT_CAP],
    size_t *field_count)
{
    bool ifs_found = false;
    const char *ifs = " \t\n";
    size_t read = begin;
    size_t end = begin + length;
    size_t write = begin;
    size_t field_begin = begin;
    bool candidate_has_bytes = false;
    bool candidate_has_quote = false;
    bool has_pattern = false;

    *field_count = 0;
    if (context != NULL) {
        ifs = lookup_variable(context, "IFS", 3, &ifs_found);
        if (ifs == NULL) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        if (!ifs_found) {
            ifs = " \t\n";
        }
    }
    while (read < end) {
        unsigned char byte = (unsigned char)pipeline->text[read];
        unsigned char provenance = pipeline->provenance[read];

        if ((provenance & GSH_WORD_FIELD_BOUNDARY) != 0) {
            if (candidate_has_bytes || candidate_has_quote) {
                gsh_native_plan_status status = emit_expanded_field(
                    pipeline, fields, field_count, field_begin, &write,
                    has_pattern);

                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
            }
            read++;
            field_begin = write;
            candidate_has_bytes = false;
            candidate_has_quote = false;
            has_pattern = false;
            continue;
        }
        if ((provenance & GSH_WORD_EMPTY_MARKER) != 0) {
            candidate_has_quote = candidate_has_quote ||
                                  (provenance & GSH_WORD_QUOTED) != 0;
            read++;
            continue;
        }
        if (*ifs != '\0' && split_delimiter(pipeline, read, ifs)) {
            bool nonwhite_seen = !ifs_white_space(byte);

            read++;
            if (ifs_white_space(byte)) {
                while (read < end && split_delimiter(pipeline, read, ifs) &&
                       ifs_white_space(
                           (unsigned char)pipeline->text[read])) {
                    read++;
                }
                if (read < end && split_delimiter(pipeline, read, ifs) &&
                    !ifs_white_space(
                        (unsigned char)pipeline->text[read])) {
                    nonwhite_seen = true;
                    read++;
                }
            }
            while (read < end && split_delimiter(pipeline, read, ifs) &&
                   ifs_white_space((unsigned char)pipeline->text[read])) {
                read++;
            }
            if (candidate_has_bytes || nonwhite_seen) {
                gsh_native_plan_status status = emit_expanded_field(
                    pipeline, fields, field_count, field_begin, &write,
                    has_pattern);

                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
            }
            field_begin = write;
            candidate_has_bytes = false;
            candidate_has_quote = false;
            has_pattern = false;
            continue;
        }
        pipeline->text[write] = (char)byte;
        pipeline->provenance[write] = provenance;
        if ((provenance & GSH_WORD_QUOTED) == 0 &&
            (byte == '*' || byte == '?' || byte == '[')) {
            has_pattern = true;
        }
        write++;
        read++;
        candidate_has_bytes = true;
    }
    if (candidate_has_bytes || candidate_has_quote) {
        gsh_native_plan_status status = emit_expanded_field(
            pipeline, fields, field_count, field_begin, &write,
            has_pattern);

        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
    }
    pipeline->text_used = write;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status expand_pathname_field(
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline, const gsh_expanded_field *field,
    char *expanded[GSH_NATIVE_ARGUMENT_CAP], size_t *expanded_count)
{
    char pattern[GSH_NATIVE_TEXT_CAP];
    glob_t matches;
    int result;
    size_t index;
    size_t pattern_used = 0;

    *expanded_count = 0;
    if (!field->has_pattern) {
        expanded[(*expanded_count)++] = field->text;
        return GSH_NATIVE_PLAN_OK;
    }
    if (context == NULL ||
        context->pathname_mode == GSH_NATIVE_PATHNAME_REJECT) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (context->pathname_mode == GSH_NATIVE_PATHNAME_PREFLIGHT) {
        expanded[(*expanded_count)++] = field->text;
        return GSH_NATIVE_PLAN_OK;
    }
    for (index = 0; index < field->length; index++) {
        unsigned char byte = (unsigned char)field->text[index];
        unsigned char provenance = pipeline->provenance[
            field->provenance_offset + index];

        if ((provenance & GSH_WORD_QUOTED) != 0 &&
            strchr("*?[]\\-!^", (int)byte) != NULL) {
            if (pattern_used == sizeof(pattern) - 1U) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            pattern[pattern_used++] = '\\';
        }
        if (pattern_used == sizeof(pattern) - 1U) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        pattern[pattern_used++] = (char)byte;
    }
    pattern[pattern_used] = '\0';
    memset(&matches, 0, sizeof(matches));
    result = glob(pattern, 0, NULL, &matches);
    if (result == GLOB_NOSPACE) {
        globfree(&matches);
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (result != 0 && result != GLOB_NOMATCH) {
        globfree(&matches);
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (result == GLOB_NOMATCH || matches.gl_pathc == 0) {
        globfree(&matches);
        expanded[(*expanded_count)++] = field->text;
        return GSH_NATIVE_PLAN_OK;
    }
    if (matches.gl_pathc > GSH_NATIVE_ARGUMENT_CAP) {
        globfree(&matches);
        return GSH_NATIVE_PLAN_LIMIT;
    }
    for (index = 0; index < matches.gl_pathc; index++) {
        gsh_native_plan_status status;

        expanded[index] = pipeline->text + pipeline->text_used;
        status = reserve_parameter_value(pipeline, matches.gl_pathv[index]);
        if (status == GSH_NATIVE_PLAN_OK &&
            !reserve_byte(pipeline, '\0')) {
            status = GSH_NATIVE_PLAN_LIMIT;
        }
        if (status != GSH_NATIVE_PLAN_OK) {
            globfree(&matches);
            return status;
        }
    }
    *expanded_count = matches.gl_pathc;
    globfree(&matches);
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status append_expanded_word(
    const char *input, gsh_word_ref word,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline,
    char *expanded[GSH_NATIVE_ARGUMENT_CAP], size_t *expanded_count)
{
    gsh_expanded_field fields[GSH_NATIVE_ARGUMENT_CAP];
    char *word_text;
    size_t expanded_begin = pipeline->text_used;
    size_t expanded_length;
    size_t field_count;
    size_t field;
    gsh_native_plan_status status = expand_static_word(
        input, word, context, GSH_EXPAND_FIELDS, 0, pipeline, &word_text,
        &expanded_length);

    if (status != GSH_NATIVE_PLAN_OK) {
        return status;
    }
    (void)word_text;
    status = split_expanded_word(context, pipeline, expanded_begin,
                                 expanded_length, fields, &field_count);
    if (status != GSH_NATIVE_PLAN_OK) {
        return status;
    }
    for (field = 0; field < field_count; field++) {
        char *paths[GSH_NATIVE_ARGUMENT_CAP];
        size_t path_count;
        size_t path;

        status = expand_pathname_field(context, pipeline, &fields[field],
                                       paths, &path_count);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        if (path_count > GSH_NATIVE_ARGUMENT_CAP - *expanded_count) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        for (path = 0; path < path_count; path++) {
            expanded[(*expanded_count)++] = paths[path];
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

gsh_native_plan_status gsh_native_expand_words(
    const char *input, const gsh_word_ref *words, size_t word_count,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *scratch,
    char *expanded[GSH_NATIVE_ARGUMENT_CAP], size_t *expanded_count)
{
    size_t index;

    scratch->text_used = 0;
    *expanded_count = 0;
    if (word_count > GSH_NATIVE_ARGUMENT_CAP) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    for (index = 0; index < word_count; index++) {
        gsh_native_plan_status status = append_expanded_word(
            input, words[index], context, scratch, expanded,
            expanded_count);

        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

/* ── Command Preserves Declaration Expansion ──────────────────────
 * Assignment-shaped operands normally undergo field and pathname expansion.
 * Export and readonly instead require assignment context, even when reached
 * through one or more command wrappers.  The planner recognizes only the
 * already-expanded prefix and accepts only `command`'s execution option, so
 * later operands receive the right expansion without speculative execution.
 * ─────────────────────────────────────────────────────────────── */
static bool planned_declaration_utility(
    const gsh_native_command *command)
{
    size_t index = 0;
    size_t wrappers = 0;

    while (index < command->argc &&
           index < GSH_NATIVE_ARGUMENT_CAP &&
           wrappers < GSH_NATIVE_ARGUMENT_CAP) {
        if (strcmp(command->argv[index], "export") == 0 ||
            strcmp(command->argv[index], "readonly") == 0) {
            return true;
        }
        if (strcmp(command->argv[index], "command") != 0) {
            return false;
        }
        index++;
        while (index < command->argc &&
               index < GSH_NATIVE_ARGUMENT_CAP &&
               command->argv[index][0] == '-' &&
               command->argv[index][1] != '\0') {
            const char *option = command->argv[index] + 1U;
            size_t scanned = 0;

            if (strcmp(command->argv[index], "--") == 0) {
                index++;
                break;
            }
            while (*option != '\0' &&
                   scanned < GSH_NATIVE_TEXT_CAP) {
                if (*option != 'p') {
                    return false;
                }
                option++;
                scanned++;
            }
            if (*option != '\0') {
                return false;
            }
            index++;
        }
        wrappers++;
    }
    return false;
}

static gsh_native_plan_status
plan_command(const char *input, const gsh_parse_storage *storage,
             const gsh_ast_node *node,
             const gsh_native_expansion_context *context,
             gsh_native_pipeline *pipeline,
             gsh_native_command *command)
{
    gsh_native_expansion_context command_context;
    const gsh_native_expansion_context *active_context = context;
    size_t index;

    if (node->kind != GSH_AST_SIMPLE ||
        (node->word_count == 0 && node->redirect_count == 0)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (node->word_count >
            GSH_NATIVE_ARGUMENT_CAP + GSH_NATIVE_ASSIGNMENT_CAP ||
        node->redirect_count > GSH_NATIVE_REDIRECT_CAP) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    memset(command, 0, sizeof(*command));
    if (context != NULL) {
        command_context = *context;
        command_context.command_substitution_performed =
            &command->command_substitution_performed;
        command_context.command_substitution_status =
            &command->command_substitution_status;
        active_context = &command_context;
    }
    for (index = 0; index < node->word_count; index++) {
        gsh_word_ref word = storage->words[node->first_word + index];
        bool declaration = planned_declaration_utility(command);
        size_t assignment_length =
            command->argc == 0 || declaration
                ? assignment_name_length(input, word)
                : 0;
        gsh_native_plan_status status;

        if (assignment_length != 0 && command->argc == 0) {
            if (command->assignment_count == GSH_NATIVE_ASSIGNMENT_CAP) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            status = plan_assignment(
                input, word, assignment_length, active_context, pipeline,
                &command->assignments[command->assignment_count]);
            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
            command->assignment_count++;
            continue;
        }
        if (assignment_length != 0) {
            if (command->argc == GSH_NATIVE_ARGUMENT_CAP) {
                return GSH_NATIVE_PLAN_LIMIT;
            }
            status = plan_assignment(
                input, word, assignment_length, active_context, pipeline,
                &command->argv[command->argc]);
            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
            command->argc++;
            continue;
        }
        status = append_expanded_word(input, word, active_context, pipeline,
                                      command->argv, &command->argc);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
    }
    if (command->argc == 0 && command->assignment_count == 0) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    command->argv[command->argc] = NULL;
    for (index = 0; index < node->redirect_count; index++) {
        gsh_native_plan_status status = plan_redirect(
            input, &storage->redirects[node->first_redirect + index],
            active_context, pipeline, &command->redirects[index]);

        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        command->redirect_count++;
    }
    return GSH_NATIVE_PLAN_OK;
}

gsh_native_plan_status gsh_native_plan_redirects_with_context(
    const char *input, const gsh_parse_storage *storage, size_t node_index,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline)
{
    const gsh_ast_node *node;
    gsh_native_command *command;
    size_t index;

    reset_pipeline(pipeline);
    if (node_index == GSH_AST_NONE || node_index >= storage->node_count) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    node = &storage->nodes[node_index];
    if (node->redirect_count > GSH_NATIVE_REDIRECT_CAP) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    command = &pipeline->commands[0];
    memset(command, 0, sizeof(*command));
    pipeline->command_count = 1;
    for (index = 0; index < node->redirect_count; index++) {
        gsh_native_plan_status status = plan_redirect(
            input, &storage->redirects[node->first_redirect + index],
            context, pipeline, &command->redirects[index]);

        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        command->redirect_count++;
    }
    return GSH_NATIVE_PLAN_OK;
}

gsh_native_plan_status
gsh_native_plan_pipeline_node_with_context(
    const char *input, const gsh_parse_storage *storage,
    size_t pipeline_node, const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline)
{
    const gsh_ast_node *node;
    size_t command;
    size_t command_count = 0;

    reset_pipeline(pipeline);
    if (pipeline_node == GSH_AST_NONE ||
        pipeline_node >= storage->node_count) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    node = &storage->nodes[pipeline_node];
    if (node->kind != GSH_AST_PIPELINE) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    pipeline->negated = (node->flags & GSH_AST_FLAG_NEGATED) != 0;
    command = node->first_child;
    while (command != GSH_AST_NONE) {
        if (command_count == GSH_NATIVE_PIPELINE_CAP) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        command_count++;
        command = storage->nodes[command].next_sibling;
    }
    if (command_count == 0) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    command = node->first_child;
    while (command != GSH_AST_NONE) {
        gsh_native_plan_status status;
        size_t text_begin;
        size_t heredoc_begin;
        size_t heredoc_text_begin;

        if (context != NULL && context->command_begin != NULL) {
            status = context->command_begin(
                context->command_opaque, pipeline->command_count,
                command_count);
            if (status != GSH_NATIVE_PLAN_OK) {
                return status;
            }
        }
        text_begin = pipeline->text_used;
        heredoc_begin = pipeline->heredoc_count;
        heredoc_text_begin = pipeline->heredoc_text_used;
        status = plan_command(input, storage, &storage->nodes[command],
                              context, pipeline,
                              &pipeline->commands[pipeline->command_count]);
        if (status == GSH_NATIVE_PLAN_ERROR && command_count > 1U) {
            pipeline->text_used = text_begin;
            pipeline->heredoc_count = heredoc_begin;
            pipeline->heredoc_text_used = heredoc_text_begin;
            memset(&pipeline->commands[pipeline->command_count], 0,
                   sizeof(pipeline->commands[pipeline->command_count]));
            pipeline->commands[pipeline->command_count].expansion_error =
                true;
        } else if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        pipeline->command_count++;
        command = storage->nodes[command].next_sibling;
    }
    return pipeline->command_count == 0 ? GSH_NATIVE_PLAN_UNSUPPORTED
                                        : GSH_NATIVE_PLAN_OK;
}

gsh_native_plan_status
gsh_native_plan_pipeline_with_context(
    const char *input, const gsh_parse_storage *storage, size_t root,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline)
{
    const gsh_ast_node *node;
    size_t list;
    size_t and_or;
    size_t pipeline_node;

    reset_pipeline(pipeline);
    if (root == GSH_AST_NONE || root >= storage->node_count) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    node = &storage->nodes[root];
    if (node->kind != GSH_AST_PROGRAM ||
        !node_has_only_child(storage, node, GSH_AST_LIST, &list)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    node = &storage->nodes[list];
    if (!node_has_only_child(storage, node, GSH_AST_AND_OR, &and_or)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    node = &storage->nodes[and_or];
    if ((node->flags & GSH_AST_FLAG_ASYNC) != 0 ||
        !node_has_only_child(storage, node, GSH_AST_PIPELINE,
                             &pipeline_node)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    return gsh_native_plan_pipeline_node_with_context(
        input, storage, pipeline_node, context, pipeline);
}

gsh_native_plan_status
gsh_native_plan_pipeline_node(const char *input,
                              const gsh_parse_storage *storage,
                              size_t pipeline_node,
                              gsh_native_pipeline *pipeline)
{
    return gsh_native_plan_pipeline_node_with_context(
        input, storage, pipeline_node, NULL, pipeline);
}

gsh_native_plan_status
gsh_native_plan_pipeline(const char *input,
                         const gsh_parse_storage *storage, size_t root,
                         gsh_native_pipeline *pipeline)
{
    return gsh_native_plan_pipeline_with_context(input, storage, root, NULL,
                                                 pipeline);
}

const char *gsh_native_plan_status_name(gsh_native_plan_status status)
{
    static const char *const names[] = {
        "ok", "unsupported", "limit", "expansion error"};

    if ((size_t)status >= sizeof(names) / sizeof(names[0])) {
        return "invalid";
    }
    return names[status];
}
