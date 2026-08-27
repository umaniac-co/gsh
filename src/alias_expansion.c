#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "alias_expansion.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    size_t begin;
    size_t end;
    uint64_t ancestors[2];
} alias_frame;

typedef struct {
    const gsh_alias_store *aliases;
    const char *input;
    alias_frame *frames;
    size_t frame_count;
    size_t matched_index;
    const char *matched_value;
} alias_probe;

static bool reserved_word(const char *word, size_t length)
{
    static const char *const reserved[] = {
        "if", "then", "else", "elif", "fi", "do", "done", "case",
        "esac", "while", "until", "for", "in", "{", "}", "!",
    };
    size_t index;

    for (index = 0; index < sizeof(reserved) / sizeof(reserved[0]); index++) {
        if (strlen(reserved[index]) == length &&
            memcmp(word, reserved[index], length) == 0) {
            return true;
        }
    }
    return false;
}

static void token_ancestors(const alias_probe *probe, size_t begin,
                            size_t end, uint64_t ancestors[2])
{
    size_t index;

    ancestors[0] = 0;
    ancestors[1] = 0;
    for (index = 0; index < probe->frame_count; index++) {
        const alias_frame *frame = &probe->frames[index];

        if (frame->begin < end && begin < frame->end) {
            ancestors[0] |= frame->ancestors[0];
            ancestors[1] |= frame->ancestors[1];
        }
    }
}

static bool find_alias(alias_probe *probe, const char *word, size_t length)
{
    uint64_t ancestors[2];
    size_t alias_index;
    size_t begin = (size_t)(word - probe->input);
    const char *value;

    if (!gsh_alias_name_is_valid(word, length) ||
        reserved_word(word, length)) {
        return false;
    }
    value = gsh_aliases_lookup(probe->aliases, word, length, &alias_index);
    if (value == NULL || alias_index >= GSH_ALIAS_CAP) {
        return false;
    }
    token_ancestors(probe, begin, begin + length, ancestors);
    if ((ancestors[alias_index / 64U] &
         (UINT64_C(1) << (alias_index % 64U))) != 0) {
        return false;
    }
    probe->matched_index = alias_index;
    probe->matched_value = value;
    return true;
}

static bool command_probe(void *opaque, const char *word, size_t length)
{
    return find_alias(opaque, word, length);
}

static bool last_command_probe(void *opaque, const char *word,
                               size_t length)
{
    bool *is_command = opaque;

    *is_command = length == 7U && memcmp(word, "command", 7) == 0;
    return false;
}

static bool replacement_ends_in_command(
    const char *value, size_t length, gsh_parse_storage *storage)
{
    gsh_word_ref candidate;
    bool is_command = false;
    gsh_parse_result result = gsh_parse_command_probe(
        value, length, storage, last_command_probe, &is_command,
        &candidate);

    (void)candidate;
    return result.status == GSH_PARSE_OK && is_command;
}

static bool ends_in_unquoted_blank(const char *value, size_t length)
{
    enum { QUOTE_NONE, QUOTE_SINGLE, QUOTE_DOUBLE } quote = QUOTE_NONE;
    bool final_quoted = false;
    size_t offset;

    if (length == 0 ||
        (value[length - 1U] != ' ' && value[length - 1U] != '\t')) {
        return false;
    }
    for (offset = 0; offset < length; offset++) {
        unsigned char byte = (unsigned char)value[offset];

        final_quoted = quote != QUOTE_NONE;
        if (quote == QUOTE_SINGLE) {
            if (byte == '\'') {
                quote = QUOTE_NONE;
            }
            continue;
        }
        if (quote == QUOTE_DOUBLE) {
            if (byte == '"') {
                quote = QUOTE_NONE;
            } else if (byte == '\\' && offset + 1U < length &&
                       (value[offset + 1U] == '$' ||
                        value[offset + 1U] == '`' ||
                        value[offset + 1U] == '"' ||
                        value[offset + 1U] == '\\' ||
                        value[offset + 1U] == '\n')) {
                offset++;
            }
            continue;
        }
        if (byte == '\'') {
            quote = QUOTE_SINGLE;
        } else if (byte == '"') {
            quote = QUOTE_DOUBLE;
        } else if (byte == '\\' && offset + 1U < length) {
            offset++;
            final_quoted = offset + 1U == length;
        }
    }
    return !final_quoted;
}

static bool rewrite_alias(char *input, size_t *length, size_t capacity,
                          size_t begin, size_t end, const char *value,
                          size_t alias_index, alias_frame *frames,
                          size_t *frame_count)
{
    size_t value_length = strlen(value);
    size_t replacement_length = value_length + 1U;
    size_t old_length = end - begin;
    uint64_t ancestors[2] = {0, 0};
    size_t read;
    size_t write = 0;
    ptrdiff_t delta;

    if (*frame_count == GSH_ALIAS_EXPANSION_LIMIT || begin > end ||
        end > *length ||
        replacement_length >= capacity - (*length - old_length)) {
        return false;
    }
    delta = (ptrdiff_t)replacement_length - (ptrdiff_t)old_length;
    for (read = 0; read < *frame_count; read++) {
        alias_frame frame = frames[read];

        if (frame.begin < end && begin < frame.end) {
            ancestors[0] |= frame.ancestors[0];
            ancestors[1] |= frame.ancestors[1];
        }
        if (frame.end <= begin) {
            frames[write++] = frame;
        } else if (frame.begin >= end) {
            frame.begin = (size_t)((ptrdiff_t)frame.begin + delta);
            frame.end = (size_t)((ptrdiff_t)frame.end + delta);
            frames[write++] = frame;
        } else if (frame.begin <= begin && frame.end >= end) {
            frame.end = (size_t)((ptrdiff_t)frame.end + delta);
            frames[write++] = frame;
        }
    }
    memmove(input + begin + replacement_length, input + end,
            *length - end);
    memcpy(input + begin, value, value_length);
    input[begin + value_length] = ' ';
    *length = (size_t)((ptrdiff_t)*length + delta);
    input[*length] = '\0';
    ancestors[alias_index / 64U] |= UINT64_C(1) << (alias_index % 64U);
    frames[write].begin = begin;
    frames[write].end = begin + value_length;
    frames[write].ancestors[0] = ancestors[0];
    frames[write].ancestors[1] = ancestors[1];
    *frame_count = write + 1U;
    return true;
}

static bool force_following_aliases(
    char *input, size_t *length, size_t capacity, size_t marker,
    alias_probe *probe, size_t *expansions)
{
    while (marker < *length && *expansions < GSH_ALIAS_EXPANSION_LIMIT) {
        gsh_lexer lexer;
        gsh_token token;
        const char *value;
        size_t value_length;
        size_t begin;
        size_t end;

        gsh_lexer_init(&lexer, input + marker, *length - marker);
        if (gsh_lexer_next(&lexer, &token) != GSH_LEX_OK ||
            token.kind != GSH_TOKEN_WORD) {
            return true;
        }
        begin = marker + token.begin;
        end = marker + token.end;
        probe->input = input;
        if (!find_alias(probe, input + begin, end - begin)) {
            return true;
        }
        value = probe->matched_value;
        value_length = strlen(value);
        if (!rewrite_alias(input, length, capacity, begin, end, value,
                           probe->matched_index, probe->frames,
                           &probe->frame_count)) {
            return false;
        }
        (*expansions)++;
        if (!ends_in_unquoted_blank(value, value_length)) {
            return true;
        }
        marker = begin + value_length + 1U;
    }
    return marker >= *length;
}

static gsh_parse_result limit_result(size_t offset)
{
    gsh_parse_result result = {
        .status = GSH_PARSE_LIMIT,
        .root = GSH_AST_NONE,
        .error_offset = offset,
        .unexpected = GSH_TOKEN_WORD,
    };

    return result;
}

gsh_parse_result gsh_alias_parse(
    const char *input, size_t length, const gsh_alias_store *aliases,
    char *expanded, size_t expanded_capacity, gsh_parse_storage *storage,
    const char **parsed_input, size_t *parsed_length)
{
    alias_frame frames[GSH_ALIAS_EXPANSION_LIMIT];
    alias_probe probe = {
        .aliases = aliases,
        .input = input,
        .frames = frames,
        .frame_count = 0,
        .matched_index = GSH_ALIAS_CAP,
        .matched_value = NULL,
    };
    size_t expansions = 0;
    gsh_word_ref candidate;
    gsh_parse_result result;

    *parsed_input = input;
    *parsed_length = length;
    if (gsh_aliases_count(aliases) == 0) {
        return gsh_parse(input, length, storage);
    }
    result = gsh_parse_command_probe(
        input, length, storage, command_probe, &probe, &candidate);
    if (result.status != GSH_PARSE_REWRITE) {
        return result;
    }
    if (expanded == NULL || length >= expanded_capacity) {
        return limit_result(length);
    }
    memcpy(expanded, input, length);
    expanded[length] = '\0';
    probe.input = expanded;
    if (!rewrite_alias(expanded, &length, expanded_capacity,
                       candidate.begin, candidate.end,
                       probe.matched_value, probe.matched_index, frames,
                       &probe.frame_count)) {
        return limit_result(candidate.begin);
    }
    expansions++;
    {
        size_t value_length = strlen(probe.matched_value);

        if (ends_in_unquoted_blank(probe.matched_value, value_length) &&
            !replacement_ends_in_command(
                probe.matched_value, value_length, storage) &&
            !force_following_aliases(
                expanded, &length, expanded_capacity,
                candidate.begin + value_length + 1U, &probe,
                &expansions)) {
            return limit_result(candidate.begin);
        }
    }
    for (;;) {
        result = gsh_parse_command_probe(
            expanded, length, storage, command_probe, &probe, &candidate);

        if (result.status != GSH_PARSE_REWRITE) {
            *parsed_input = expanded;
            *parsed_length = length;
            return result;
        }
        if (expansions == GSH_ALIAS_EXPANSION_LIMIT ||
            !rewrite_alias(expanded, &length, expanded_capacity,
                           candidate.begin, candidate.end,
                           probe.matched_value, probe.matched_index, frames,
                           &probe.frame_count)) {
            return limit_result(candidate.begin);
        }
        {
            size_t value_length = strlen(probe.matched_value);

            expansions++;
            probe.input = expanded;
            if (ends_in_unquoted_blank(probe.matched_value,
                                       value_length) &&
                !replacement_ends_in_command(
                    probe.matched_value, value_length, storage) &&
                !force_following_aliases(
                    expanded, &length, expanded_capacity,
                    candidate.begin + value_length + 1U, &probe,
                    &expansions)) {
                return limit_result(candidate.begin);
            }
        }
    }
}
