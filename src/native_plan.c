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

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

#define require(condition) (condition)

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
    if (node == NULL) return false;
    if (child == NULL || storage == NULL) {
        return false;
    }
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
    if (pipeline == NULL) {
        return;
    }
    pipeline->command_count = 0;
    pipeline->heredoc_count = 0;
    pipeline->negated = false;
    pipeline->text_used = 0;
    pipeline->heredoc_text_used = 0;
}

static bool reserve_byte(gsh_native_pipeline *pipeline, unsigned char byte)
{
    if (pipeline == NULL) {
        return false;
    }
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
    if (pipeline == NULL) {
        return;
    }
    while (begin < pipeline->text_used) {
        pipeline->provenance[begin++] = provenance;
    }
}

static bool reserve_empty_marker(gsh_native_pipeline *pipeline)
{
    if (pipeline == NULL) {
        return false;
    }
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
    if (pipeline == NULL) {
        return false;
    }
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

static bool dollar_simple_escape(unsigned char escape,
                                 unsigned char *byte)
{
    if (!require(byte != NULL)) return false;
    if (!require(escape != 0U)) return false;
    if (escape == '"' || escape == '\'' || escape == '\\') *byte = escape;
    else if (escape == 'a') *byte = '\a';
    else if (escape == 'b') *byte = '\b';
    else if (escape == 'e') *byte = 0x1bU;
    else if (escape == 'f') *byte = '\f';
    else if (escape == 'n') *byte = '\n';
    else if (escape == 'r') *byte = '\r';
    else if (escape == 't') *byte = '\t';
    else if (escape == 'v') *byte = '\v';
    else return false;
    return true;
}

static gsh_native_plan_status dollar_escaped_byte(
    const char *input, size_t end, size_t *offset, unsigned char *byte)
{
    unsigned char escape;
    unsigned int value;

    if (!require(input != NULL && offset != NULL && byte != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(*offset <= end)) return GSH_NATIVE_PLAN_UNSUPPORTED;
    if (*offset == end) return GSH_NATIVE_PLAN_UNSUPPORTED;
    escape = (unsigned char)input[(*offset)++];
    if (dollar_simple_escape(escape, byte)) return GSH_NATIVE_PLAN_OK;
    if (escape == 'c') {
        unsigned char control;

        if (*offset == end) return GSH_NATIVE_PLAN_UNSUPPORTED;
        control = (unsigned char)input[(*offset)++];
        if (control >= '@' && control <= '_') *byte = control - '@';
        else if (control == '?') *byte = 0x7fU;
        else return GSH_NATIVE_PLAN_UNSUPPORTED;
        return GSH_NATIVE_PLAN_OK;
    }
    if (escape == 'x') {
        size_t digits = 0U;

        value = 0U;
        while (*offset < end && digits < 2U) {
            int digit = hexadecimal_digit((unsigned char)input[*offset]);

            if (digit < 0) break;
            value = value * 16U + (unsigned int)digit;
            (*offset)++;
            digits++;
        }
        if (digits == 0U) return GSH_NATIVE_PLAN_UNSUPPORTED;
    } else if (escape >= '0' && escape <= '7') {
        size_t digits = 1U;

        value = (unsigned int)(escape - '0');
        while (*offset < end && digits < 3U && input[*offset] >= '0' &&
               input[*offset] <= '7') {
            value = value * 8U + (unsigned int)(input[(*offset)++] - '0');
            digits++;
        }
        if (value > 255U) return GSH_NATIVE_PLAN_UNSUPPORTED;
    } else return GSH_NATIVE_PLAN_UNSUPPORTED;
    *byte = (unsigned char)value;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status expand_dollar_single_quote(
    const char *input, size_t end, size_t *offset,
    gsh_native_pipeline *pipeline)
{
    if (input == NULL || offset == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    bool closed = false;

    while (*offset < end) {
        unsigned char byte = (unsigned char)input[(*offset)++];

        if (byte == '\'') {
            closed = true;
            break;
        }
        if (byte == '\\') {
            gsh_native_plan_status status = dollar_escaped_byte(
                input, end, offset, &byte);

            if (status != GSH_NATIVE_PLAN_OK) return status;
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
    if (entry == NULL || name == NULL) return false;
    size_t offset;

    for (offset = 0; offset < name_length; offset++) {
        if (entry[offset] == '\0' || entry[offset] != name[offset]) {
            return false;
        }
    }
    return entry[name_length] == '=';
}

static void expansion_diagnostic(const gsh_native_variable_state *state,
                                 const char *text, size_t length)
{
    if (text == NULL) {
        return;
    }
    if (state != NULL && !state->preflight &&
        state->diagnostic_io != NULL) {
        (void)gsh_builtin_output(state->diagnostic_io, STDERR_FILENO,
                                 text, length);
    }
}

static void expansion_diagnostic_newline(
    const gsh_native_variable_state *state)
{
    if (state == NULL) {
        return;
    }
    expansion_diagnostic(state, state != NULL && state->carriage_return
                                    ? "\r\n"
                                    : "\n",
                         state != NULL && state->carriage_return ? 2U : 1U);
}

static const char *native_variable_lookup(
    gsh_native_variable_state *state, const char *name,
    size_t name_length, bool *found)
{
    if (state == NULL) return NULL;
    if (found == NULL || name == NULL) {
        return NULL;
    }
    const char *value;
    gsh_variable_journal_value_state journal_state;

    if (state->mode == GSH_NATIVE_VARIABLE_OVERLAY && state->isolated) {
        value = gsh_variable_journal_lookup_scoped(
            state->scope_changes, state->current_scope, name, name_length,
            &journal_state);
        if (journal_state != GSH_VARIABLE_JOURNAL_VALUE_ABSENT) {
            *found = journal_state == GSH_VARIABLE_JOURNAL_VALUE_SET;
            return value;
        }
        return gsh_variables_lookup(state->scope_base, name, name_length,
                                    found);
    }
    if (state->mode == GSH_NATIVE_VARIABLE_OVERLAY && state->mutated) {
        value = gsh_variable_journal_lookup(
            state->journal, name, name_length, &journal_state);
        if (journal_state != GSH_VARIABLE_JOURNAL_VALUE_ABSENT) {
            *found = journal_state == GSH_VARIABLE_JOURNAL_VALUE_SET;
            return value;
        }
    }
    return gsh_variables_lookup(state->variables, name, name_length, found);
}

static gsh_native_plan_status native_variable_command_begin(
    gsh_native_variable_state *state, size_t command_index,
    size_t command_count)
{
    if (state == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (state->mode == GSH_NATIVE_VARIABLE_OVERLAY) {
        state->isolated = command_count > 1U;
        if (state->isolated && command_index == 0U) {
            gsh_variable_journal_initialize(state->scope_changes, 0);
            state->scope_base = state->variables;
            state->command_count = command_count;
        }
        state->current_scope = command_index + 1U;
        return GSH_NATIVE_PLAN_OK;
    }
    if (state->scope_changes == NULL) {
        return GSH_NATIVE_PLAN_OK;
    }
    if (state->scope_base == NULL || state->command_count != command_count ||
        command_index >= command_count) {
        return GSH_NATIVE_PLAN_ERROR;
    }
    (void)memcpy(state->variables, state->scope_base, sizeof(*state->variables));
    state->current_scope = command_index + 1U;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status overlay_variable_assign(
    gsh_native_variable_state *state, const char *name,
    size_t name_length, const char *value, size_t value_length)
{
    if (state == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (name == NULL || value == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    int result;

    if (state->isolated) {
        result = gsh_variable_journal_record_scoped(
            state->scope_changes, state->current_scope, name, name_length,
            value, value_length, state->attributes, state->attributes);
        if (result == 0) {
            result = gsh_variables_can_apply_journal_scope(
                state->scope_base, state->scope_changes,
                state->current_scope);
        }
    } else {
        if (!state->mutated) {
            gsh_variable_journal_initialize(state->journal,
                                            state->journal_generation);
        }
        result = gsh_variable_journal_record(
            state->journal, name, name_length, value, value_length,
            state->attributes, state->attributes);
        if (result == 0) {
            result = gsh_variables_can_apply_journal(state->variables,
                                                     state->journal);
        }
        state->mutated = result == 0 || state->mutated;
    }
    if (result == 0) {
        return GSH_NATIVE_PLAN_OK;
    }
    expansion_diagnostic(
        state,
        state->isolated
            ? "gsh: pipeline parameter assignment failed"
            : "gsh: parameter assignment failed",
        state->isolated ? 41U : 32U);
    expansion_diagnostic_newline(state);
    return errno == ENOSPC || errno == E2BIG ? GSH_NATIVE_PLAN_LIMIT
                                             : GSH_NATIVE_PLAN_ERROR;
}

static gsh_native_plan_status live_variable_assign(
    gsh_native_variable_state *state, const char *name,
    size_t name_length, const char *value, size_t value_length)
{
    if (state == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (name == NULL || value == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    int result;

    if (state->assignment_fault_enabled &&
        state->assignment_fault_calls != NULL &&
        ++(*state->assignment_fault_calls) ==
            state->assignment_fault_trigger) {
        errno = ENOSPC;
        result = -1;
    } else {
        result = gsh_variables_set(
            state->variables, name, name_length, value, value_length,
            state->attributes, state->attributes);
    }
    if (result == 0 && state->scope_changes != NULL) {
        result = gsh_variable_journal_record_scoped(
            state->scope_changes, state->current_scope, name, name_length,
            value, value_length, state->attributes, state->attributes);
    } else if (result == 0 && state->journal != NULL) {
        result = gsh_variable_journal_record(
            state->journal, name, name_length, value, value_length,
            state->attributes, state->attributes);
    }
    if (result == 0) {
        return GSH_NATIVE_PLAN_OK;
    }
    expansion_diagnostic(state, "gsh: parameter assignment failed", 32U);
    expansion_diagnostic_newline(state);
    if (state->fatal_error != NULL && state->scope_changes == NULL) {
        *state->fatal_error = true;
    }
    return errno == ENOSPC || errno == E2BIG ? GSH_NATIVE_PLAN_LIMIT
                                             : GSH_NATIVE_PLAN_ERROR;
}

static gsh_native_plan_status native_variable_assign(
    gsh_native_variable_state *state, const char *name,
    size_t name_length, const char *value, size_t value_length)
{
    if (name == NULL || value == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (state == NULL || state->variables == NULL) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    return state->mode == GSH_NATIVE_VARIABLE_OVERLAY
               ? overlay_variable_assign(state, name, name_length, value,
                                         value_length)
               : live_variable_assign(state, name, name_length, value,
                                      value_length);
}

static gsh_native_plan_status native_parameter_error(
    gsh_native_variable_state *state, const char *name,
    size_t name_length, const char *message, size_t message_length,
    bool default_message)
{
    if (message == NULL || name == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (state == NULL) {
        return GSH_NATIVE_PLAN_ERROR;
    }
    if (state->preflight) {
        return GSH_NATIVE_PLAN_OK;
    }
    expansion_diagnostic(state, "gsh: ", 5U);
    expansion_diagnostic(state, name, name_length);
    expansion_diagnostic(state, ": ", 2U);
    expansion_diagnostic(state,
                         default_message ? "parameter null or not set"
                                         : message,
                         default_message ? 25U : message_length);
    expansion_diagnostic_newline(state);
    if (state->fatal_error != NULL && state->scope_changes == NULL) {
        *state->fatal_error = true;
    }
    return GSH_NATIVE_PLAN_ERROR;
}

static gsh_native_plan_status native_arithmetic_error(
    gsh_native_variable_state *state, const char *message,
    size_t message_length)
{
    if (message == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (state == NULL) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (state->preflight) {
        return GSH_NATIVE_PLAN_OK;
    }
    expansion_diagnostic(state, "gsh: arithmetic expansion: ", 27U);
    expansion_diagnostic(state, message, message_length);
    expansion_diagnostic_newline(state);
    if (state->fatal_error != NULL && state->scope_changes == NULL) {
        *state->fatal_error = true;
    }
    return GSH_NATIVE_PLAN_ERROR;
}

static const char *lookup_variable(
    const gsh_native_expansion_context *context, const char *name,
    size_t name_length, bool *found)
{
    if (context == NULL) return NULL;
    if (found == NULL || name == NULL) {
        return NULL;
    }
    size_t index;

    *found = false;
    if (context->variable_state != NULL) {
        return native_variable_lookup(context->variable_state, name,
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
    if (value == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    while (*value != '\0') {
        if (!reserve_byte(pipeline, (unsigned char)*value++)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

void gsh_native_substitutions_initialize(
    gsh_native_substitution_state *state, bool execute)
{
    if (state != NULL) {
        (void)memset(state, 0, sizeof(*state));
        state->execute = execute;
    }
}

void gsh_native_substitutions_rewind(gsh_native_substitution_state *state)
{
    if (state != NULL) {
        state->cursor = 0;
        state->pending = false;
        state->request_length = 0;
    }
}

const char *gsh_native_substitution_request(
    const gsh_native_substitution_state *state, size_t *length)
{
    if (state == NULL || length == NULL || !state->pending) {
        return NULL;
    }
    *length = state->request_length;
    return state->request;
}

char *gsh_native_substitution_output(gsh_native_substitution_state *state,
                                     size_t *capacity)
{
    if (state == NULL || capacity == NULL || !state->pending ||
        state->text_used > sizeof(state->text)) {
        return NULL;
    }
    *capacity = sizeof(state->text) - state->text_used;
    return state->text + state->text_used;
}

gsh_native_plan_status gsh_native_substitution_complete(
    gsh_native_substitution_state *state, size_t length, int exit_status)
{
    gsh_native_substitution_result *result;

    if (state == NULL || !state->pending ||
        state->count == GSH_NATIVE_SUBSTITUTION_CAP ||
        length > sizeof(state->text) - state->text_used ||
        state->text_used > UINT32_MAX || length > UINT32_MAX) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    result = &state->results[state->count++];
    result->offset = (uint32_t)state->text_used;
    result->length = (uint32_t)length;
    result->exit_status = exit_status;
    state->text_used += length;
    state->pending = false;
    state->request_length = 0;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status native_command_substitution(
    const gsh_native_expansion_context *context, const char *commands,
    size_t command_length, char *output, size_t output_capacity,
    size_t *output_length, int *exit_status)
{
    if (commands == NULL || context == NULL || exit_status == NULL || output == NULL || output_length == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    gsh_native_substitution_state *state =
        context == NULL ? NULL : context->substitutions;

    *output_length = 0;
    *exit_status = 0;
    if (state == NULL) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!state->execute) {
        if (output_capacity == 0U) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        output[0] = '0';
        *output_length = 1U;
        return GSH_NATIVE_PLAN_OK;
    }
    if (state->cursor < state->count) {
        const gsh_native_substitution_result *result =
            &state->results[state->cursor++];

        if (result->offset > state->text_used ||
            result->length > state->text_used - result->offset ||
            result->length > output_capacity) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        (void)memcpy(output, state->text + result->offset, result->length);
        *output_length = result->length;
        *exit_status = result->exit_status;
        return GSH_NATIVE_PLAN_OK;
    }
    if (state->pending || state->count == GSH_NATIVE_SUBSTITUTION_CAP ||
        command_length >= sizeof(state->request)) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    (void)memcpy(state->request, commands, command_length);
    state->request[command_length] = '\0';
    state->request_length = command_length;
    state->pending = true;
    return GSH_NATIVE_PLAN_DEFERRED;
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

static gsh_native_plan_status skip_parameter_backquote(
    const char *input, size_t end, size_t *offset)
{
    unsigned char byte = 0U;

    if (!require(input != NULL && offset != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(*offset < end && input[*offset] == 0x60)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    (*offset)++;
    while (*offset < end) {
        byte = (unsigned char)input[(*offset)++];
        if (byte == '\\' && *offset < end) (*offset)++;
        else if (byte == 0x60U) return GSH_NATIVE_PLAN_OK;
    }
    return GSH_NATIVE_PLAN_UNSUPPORTED;
}

static gsh_native_plan_status skip_parameter_command(
    const char *input, size_t end, size_t *offset)
{
    size_t contents_end;
    size_t after;
    bool arithmetic;
    gsh_native_plan_status status;

    if (!require(input != NULL && offset != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(*offset + 1U < end && input[*offset] == '$' &&
                 input[*offset + 1U] == '(')) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    arithmetic = *offset + 2U < end && input[*offset + 2U] == '(';
    status = find_substitution_end(input, end, *offset + 1U, arithmetic,
                                   &contents_end, &after);
    if (status == GSH_NATIVE_PLAN_OK) *offset = after;
    return status;
}

static gsh_native_plan_status advance_parameter_brace(
    const char *input, size_t end, size_t *offset, size_t *nesting,
    size_t *closing, bool *handled, bool *closed)
{
    unsigned char byte;

    if (!require(input != NULL && offset != NULL && nesting != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(closing != NULL && handled != NULL && closed != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    byte = (unsigned char)input[*offset];
    *handled = false;
    *closed = false;
    if (byte == '$' && *offset + 1U < end &&
        input[*offset + 1U] == '{') {
        if (*nesting == 128U) return GSH_NATIVE_PLAN_LIMIT;
        (*nesting)++;
        *offset += 2U;
        *handled = true;
    } else if (byte == '}') {
        (*nesting)--;
        if (*nesting == 0U) {
            *closing = *offset;
            *closed = true;
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

typedef enum {
    PARAMETER_SCAN_NONE,
    PARAMETER_SCAN_SINGLE,
    PARAMETER_SCAN_DOUBLE,
    PARAMETER_SCAN_DOLLAR_SINGLE,
} parameter_scan_quote;

typedef enum {
    PARAMETER_QUOTE_ERROR,
    PARAMETER_QUOTE_UNHANDLED,
    PARAMETER_QUOTE_HANDLED,
} parameter_quote_step;

static parameter_quote_step advance_parameter_quote(
    unsigned char byte, unsigned char next, bool has_next, size_t *offset,
    parameter_scan_quote *quote)
{
    if (!require(offset != NULL && quote != NULL)) {
        return PARAMETER_QUOTE_ERROR;
    }
    if (*quote == PARAMETER_SCAN_SINGLE) {
        (*offset)++;
        if (byte == '\'') *quote = PARAMETER_SCAN_NONE;
        return PARAMETER_QUOTE_HANDLED;
    }
    if (*quote == PARAMETER_SCAN_DOLLAR_SINGLE) {
        *offset += byte == '\\' && has_next ? 2U : 1U;
        if (byte == '\'') *quote = PARAMETER_SCAN_NONE;
        return PARAMETER_QUOTE_HANDLED;
    }
    if (byte == '\\' && has_next) {
        *offset += 2U;
        return PARAMETER_QUOTE_HANDLED;
    }
    if (*quote == PARAMETER_SCAN_DOUBLE && byte == '"') {
        *quote = PARAMETER_SCAN_NONE;
    } else if (*quote == PARAMETER_SCAN_NONE && byte == '\'') {
        *quote = PARAMETER_SCAN_SINGLE;
    } else if (*quote == PARAMETER_SCAN_NONE && byte == '"') {
        *quote = PARAMETER_SCAN_DOUBLE;
    } else if (*quote == PARAMETER_SCAN_NONE && byte == '$' && has_next &&
               next == '\'') {
        *quote = PARAMETER_SCAN_DOLLAR_SINGLE;
        (*offset)++;
    } else {
        return PARAMETER_QUOTE_UNHANDLED;
    }
    (*offset)++;
    return PARAMETER_QUOTE_HANDLED;
}

static gsh_native_plan_status find_parameter_end(
    const char *input, size_t end, size_t opening_brace, size_t *closing)
{
    if (closing == NULL || input == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    parameter_scan_quote quote = PARAMETER_SCAN_NONE;
    size_t nesting = 1;
    size_t offset = opening_brace + 1U;

    while (offset < end) {
        unsigned char byte = (unsigned char)input[offset];

        if (byte == '\0') {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        {
            bool has_next = offset + 1U < end;
            parameter_quote_step step = advance_parameter_quote(
                byte, has_next ? (unsigned char)input[offset + 1U] : 0U,
                has_next, &offset, &quote);

            if (step == PARAMETER_QUOTE_ERROR) return GSH_NATIVE_PLAN_LIMIT;
            if (step == PARAMETER_QUOTE_HANDLED) continue;
        }
        if (byte == 0x60U) {
            gsh_native_plan_status status = skip_parameter_backquote(
                input, end, &offset);

            if (status != GSH_NATIVE_PLAN_OK) return status;
            continue;
        }
        if (byte == '$' && offset + 1U < end &&
            input[offset + 1U] == '(') {
            gsh_native_plan_status status = skip_parameter_command(
                input, end, &offset);

            if (status != GSH_NATIVE_PLAN_OK) return status;
            continue;
        }
        {
            bool handled;
            bool closed;
            gsh_native_plan_status status = advance_parameter_brace(
                input, end, &offset, &nesting, closing, &handled, &closed);

            if (status != GSH_NATIVE_PLAN_OK) return status;
            if (closed) return GSH_NATIVE_PLAN_OK;
            if (handled) continue;
        }
        offset++;
    }
    return GSH_NATIVE_PLAN_UNSUPPORTED;
}

static void mark_expansion(gsh_native_pipeline *pipeline, size_t begin,
                           bool quoted)
{
    if (pipeline == NULL) {
        return;
    }
    unsigned char provenance = GSH_WORD_EXPANDED |
                               (quoted ? GSH_WORD_QUOTED : 0);

    while (begin < pipeline->text_used) {
        pipeline->provenance[begin++] |= provenance;
    }
}

static size_t decimal_text(unsigned long value, char output[32])
{
    if (output == NULL) {
        return 0U;
    }
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

static gsh_native_plan_status parameter_special_reference(
    unsigned char special, size_t begin,
    const gsh_native_expansion_context *context, size_t *after,
    const char **value, bool *found, gsh_parameter_kind *kind,
    char numeric[32], bool *handled)
{
    if (numeric == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (!require(context != NULL && after != NULL && value != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(found != NULL && kind != NULL && handled != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    *handled = true;
    *after = begin + 1U;
    *found = true;
    if (special == '?') {
        (void)decimal_text((unsigned long)(context->last_status & 255),
                           numeric);
        *value = numeric;
    } else if (special == '$') {
        if (context->shell_pid < 0) return GSH_NATIVE_PLAN_UNSUPPORTED;
        (void)decimal_text((unsigned long)context->shell_pid, numeric);
        *value = numeric;
    } else if (special == '!') {
        if (context->last_background_pid > 0) {
            (void)decimal_text((unsigned long)context->last_background_pid,
                               numeric);
            *value = numeric;
        } else {
            *value = "";
            *found = false;
        }
    } else if (special == '@' || special == '*') {
        *value = "";
        *found = context->positional_count != 0U;
        *kind = special == '@' ? GSH_PARAMETER_AT : GSH_PARAMETER_STAR;
    } else if (special == '#') {
        (void)decimal_text((unsigned long)context->positional_count, numeric);
        *value = numeric;
    } else if (special == '-') {
        *value = context->option_flags != NULL ? context->option_flags : "";
    } else {
        *handled = false;
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status parameter_positional_reference(
    const char *input, size_t end, size_t begin,
    const gsh_native_expansion_context *context, bool braced,
    size_t *after, const char **value, bool *found)
{
    size_t cursor = begin;
    size_t number = 0U;

    if (!require(input != NULL && context != NULL && begin < end)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(after != NULL && value != NULL && found != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
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
    if (number == 0U) {
        *value = context->parameter_zero != NULL ? context->parameter_zero
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

static gsh_native_plan_status parameter_reference(
    const char *input, size_t end, size_t begin,
    const gsh_native_expansion_context *context, bool braced,
    size_t *after, const char **value, bool *found,
    gsh_parameter_kind *kind, char numeric[32])
{
    if (after == NULL || found == NULL || input == NULL || kind == NULL || numeric == NULL || value == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    unsigned char special;
    size_t name_end;
    bool handled;
    gsh_native_plan_status status;

    if (context == NULL || begin == end) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    special = (unsigned char)input[begin];
    *kind = GSH_PARAMETER_VALUE;
    status = parameter_special_reference(
        special, begin, context, after, value, found, kind, numeric,
        &handled);
    if (status != GSH_NATIVE_PLAN_OK || handled) return status;
    if (special >= '0' && special <= '9') {
        return parameter_positional_reference(
            input, end, begin, context, braced, after, value, found);
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
    if (context == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (name == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (!context->nounset) {
        return GSH_NATIVE_PLAN_OK;
    }
    if (context->variable_state == NULL) {
        return GSH_NATIVE_PLAN_ERROR;
    }
    return native_parameter_error(context->variable_state, name,
                                  name_length, "", 0, true);
}

static gsh_native_plan_status positional_separator(
    const gsh_native_expansion_context *context, const char **separator,
    size_t *separator_length)
{
    if (context == NULL || separator == NULL || separator_length == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    (void)memset(&state, 0, sizeof(state));
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
    if (context == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    if (length == NULL || value == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    mbstate_t state;
    const char *cursor = value;
    size_t bytes = 0;

    (void)memset(&state, 0, sizeof(state));
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

/* ── Expansion Frames Replace the Nested C Call Chain ────────────
 * Parameter operators and arithmetic expressions can contain another word.
 * The original implementation followed that grammar with mutually recursive
 * C calls, so the language depth limit did not also bound native stack use.
 * A fixed frame array now records the suspended parent and its continuation.
 * Each child returns through the driver, preserving behavior with fixed memory.
 * ─────────────────────────────────────────────────────────────── */

typedef enum {
    EXPANSION_QUOTE_NONE,
    EXPANSION_QUOTE_SINGLE,
    EXPANSION_QUOTE_DOUBLE,
} expansion_quote;

typedef enum {
    EXPANSION_CONTINUATION_NONE,
    EXPANSION_CONTINUATION_PATTERN,
    EXPANSION_CONTINUATION_OPERATOR,
    EXPANSION_CONTINUATION_ARITHMETIC,
} expansion_continuation;

typedef struct {
    size_t value_begin;
    size_t cursor;
    size_t parameter_end;
    size_t closing;
    size_t word_begin;
    const char *parameter_value;
    bool quoted;
    bool colon;
    unsigned char operator;
    gsh_parameter_pattern_operation pattern_operation;
} expansion_parameter_pending;

typedef struct {
    size_t expression_begin;
    size_t after;
    bool deferred;
} expansion_arithmetic_pending;

typedef struct {
    gsh_word_ref word;
    gsh_expansion_mode mode;
    size_t logical_depth;
    size_t offset;
    size_t expanded_begin;
    expansion_quote quote;
    expansion_continuation continuation;
    expansion_parameter_pending parameter;
    expansion_arithmetic_pending arithmetic;
    char numeric[32];
    bool complete;
} expansion_word_frame;

enum {
    EXPANSION_FRAME_CAP = 33,
    EXPANSION_MACHINE_STEP_CAP = 4 * 1024 * 1024,
};

typedef struct {
    const char *input;
    const gsh_native_expansion_context *context;
    gsh_native_pipeline *pipeline;
    expansion_word_frame frames[EXPANSION_FRAME_CAP];
    size_t depth;
    gsh_word_ref requested_word;
    gsh_expansion_mode requested_mode;
    size_t requested_depth;
    bool push_requested;
    bool child_ready;
    size_t child_begin;
    size_t child_length;
} expansion_machine;

static gsh_native_pipeline *expansion_pipeline(
    const expansion_machine *machine)
{
    if (!require(machine != NULL)) return NULL;
    if (!require(machine->pipeline != NULL)) return NULL;
    return machine->pipeline;
}

static bool request_expansion_word(expansion_machine *machine,
                                   gsh_word_ref word,
                                   gsh_expansion_mode mode,
                                   size_t logical_depth)
{
    if (!require(machine != NULL && machine->pipeline != NULL)) return false;
    if (!require(word.begin <= word.end && !machine->push_requested)) {
        return false;
    }
    if (logical_depth > 32U || machine->depth == EXPANSION_FRAME_CAP) {
        return false;
    }
    machine->requested_word = word;
    machine->requested_mode = mode;
    machine->requested_depth = logical_depth;
    machine->push_requested = true;
    return true;
}

static bool pattern_has_syntax(const gsh_native_pipeline *pipeline,
                               size_t begin, size_t length)
{
    if (pipeline == NULL) {
        return false;
    }
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
    if (pipeline == NULL || star == NULL) {
        return false;
    }
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
    if (pattern == NULL || selected == NULL || subject == NULL) {
        return false;
    }
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
    if (pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    if (input == NULL) {
        return false;
    }
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
    if (text == NULL) return false;
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
    if (pattern == NULL) return 0U;
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
    if (pattern == NULL || width == NULL) {
        return false;
    }
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
    if (pattern == NULL || prefix_width == NULL || star == NULL || suffix_width == NULL) {
        return false;
    }
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
    if (pattern == NULL || selected == NULL || subject == NULL) {
        return false;
    }
    size_t minimum;
    size_t maximum;
    size_t candidate;
    size_t step;
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
    for (step = 0; step <= GSH_NATIVE_TEXT_CAP; step++) {
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
    if (selected == NULL || subject == NULL) {
        return false;
    }
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
    if (pattern == NULL) {
        return 0U;
    }
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
    if (pattern == NULL) {
        return;
    }
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
    if (pattern == NULL) {
        return 0U;
    }
    while (offset < length &&
           (unsigned char)pattern[offset] != GSH_PATTERN_STAR_MARKER) {
        offset++;
    }
    return offset;
}

static size_t previous_pattern_star(const char *pattern, size_t offset)
{
    if (pattern == NULL) return 0U;
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
    if (pattern == NULL) return 0U;
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
    if (pattern == NULL || subject == NULL) {
        return false;
    }
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
    if (selected == NULL) {
        return false;
    }
    size_t width;
    size_t candidate;
    size_t step;

    if (!fixed_pattern_width(pattern + pattern_begin,
                             pattern_end - pattern_begin, &width) ||
        width > subject_length || low > high || high > subject_length - width) {
        return false;
    }
    candidate = forward ? low : high;
    for (step = 0; step <= GSH_NATIVE_TEXT_CAP; step++) {
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
    return false;
}

static bool fixed_stars_shortest_match(
    char *subject, size_t subject_length, char *pattern,
    gsh_parameter_pattern_operation operation, size_t *selected)
{
    if (pattern == NULL || selected == NULL || subject == NULL) {
        return false;
    }
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
        size_t step;

        for (step = 0; step <= GSH_NATIVE_TEXT_CAP; step++) {
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
        if (step > GSH_NATIVE_TEXT_CAP) return false;
    }
    *selected = cursor;
    return true;
}

static bool segment_match_exists(char *pattern, size_t capacity,
                                 const char *subject, bool prefix,
                                 bool *known)
{
    if (known == NULL || pattern == NULL || subject == NULL) {
        return false;
    }
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
        (void)memmove(pattern + 1U, pattern, length + 1U);
        pattern[0] = '*';
        exists = fnmatch(pattern, subject, 0) == 0;
        (void)memmove(pattern, pattern + 1U, length + 1U);
    }
    *known = true;
    return exists;
}

typedef struct {
    char *subject;
    size_t subject_length;
    char *pattern;
    bool prefix;
    bool first_match;
    bool shortest_removal;
    bool fixed;
    bool fixed_star;
    bool fixed_stars;
    bool existence_known;
    bool possible;
    bool found;
    size_t selected;
} parameter_pattern_search;

static gsh_native_plan_status prepare_parameter_pattern_search(
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline, char *pattern, size_t pattern_length,
    char *subject, size_t subject_length,
    gsh_parameter_pattern_operation operation,
    parameter_pattern_search *search)
{
    if (context == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    size_t fixed_width = 0;
    size_t star_offset = 0;
    size_t prefix_width = 0;
    size_t suffix_width = 0;
    size_t star_count;
    gsh_native_plan_status status;

    if (!require(pipeline != NULL && pattern != NULL && subject != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(search != NULL && pattern_length < GSH_NATIVE_TEXT_CAP)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    (void)memset(search, 0, sizeof(*search));
    search->subject = subject;
    search->subject_length = subject_length;
    search->pattern = pattern;
    search->prefix = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                     operation == GSH_REMOVE_LARGEST_PREFIX;
    search->first_match = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                          operation == GSH_REMOVE_LARGEST_SUFFIX;
    search->shortest_removal = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                               operation == GSH_REMOVE_SMALLEST_SUFFIX;
    status = encode_fnmatch_pattern(
        pipeline, (size_t)(pattern - pipeline->text), pattern_length);
    if (status != GSH_NATIVE_PLAN_OK) return status;
    search->fixed = ascii_text(subject, subject_length) &&
                    fixed_pattern_width(pattern, strlen(pattern),
                                        &fixed_width);
    search->fixed_star = !search->fixed && ascii_text(subject, subject_length) &&
                         fixed_single_star_pattern(
                             pattern, &star_offset, &prefix_width,
                             &suffix_width);
    if (search->fixed) {
        search->found = fixed_pattern_match(
            subject, subject_length, pattern, fixed_width, operation,
            &search->selected);
        return GSH_NATIVE_PLAN_OK;
    }
    if (search->fixed_star) {
        search->found = fixed_single_star_match(
            subject, subject_length, pattern, star_offset, prefix_width,
            suffix_width, operation, &search->selected);
        return GSH_NATIVE_PLAN_OK;
    }
    star_count = ascii_text(subject, subject_length)
                     ? mark_fixed_pattern_stars(pattern)
                     : 0;
    if (star_count > 1U) {
        size_t maximum_segment = fixed_pattern_max_segment(pattern);
        bool bounded = context == NULL || !context->defer_complex_patterns ||
                       maximum_segment == 0 ||
                       subject_length <= GSH_PATTERN_REACTOR_WORK_CAP /
                                             maximum_segment;

        if (bounded && search->shortest_removal) {
            search->fixed_stars = true;
            search->found = fixed_stars_shortest_match(
                subject, subject_length, pattern, operation,
                &search->selected);
        }
    }
    if (star_count > 0) restore_pattern_stars(pattern);
    return GSH_NATIVE_PLAN_OK;
}

static bool parameter_pattern_scan_required(
    const gsh_native_expansion_context *context,
    parameter_pattern_search *search, size_t pattern_capacity)
{
    if (!require(search != NULL && search->pattern != NULL)) return false;
    if (!require(search->subject != NULL && pattern_capacity > 0)) return false;
    if (search->fixed || search->fixed_star || search->fixed_stars) {
        return false;
    }
    if (context != NULL && context->defer_complex_patterns) {
        if (context->deferred_work != NULL) *context->deferred_work = true;
        return false;
    }
    search->possible = segment_match_exists(
        search->pattern, pattern_capacity, search->subject,
        search->prefix, &search->existence_known);
    return !search->existence_known || search->possible;
}

static bool parameter_pattern_matches_at(parameter_pattern_search *search,
                                         size_t cut)
{
    bool matches;

    if (!require(search != NULL && search->pattern != NULL)) return false;
    if (!require(search->subject != NULL && cut <= search->subject_length)) {
        return false;
    }
    if (search->prefix) {
        char saved = search->subject[cut];

        search->subject[cut] = '\0';
        matches = fnmatch(search->pattern, search->subject, 0) == 0;
        search->subject[cut] = saved;
        return matches;
    }
    return fnmatch(search->pattern, search->subject + cut, 0) == 0;
}

static gsh_native_plan_status scan_parameter_pattern(
    parameter_pattern_search *search)
{
    mbstate_t character_state;
    size_t cut = 0;
    size_t steps;

    if (!require(search != NULL && search->subject != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(search->pattern != NULL)) return GSH_NATIVE_PLAN_UNSUPPORTED;
    if (!search->first_match &&
        ascii_text(search->subject, search->subject_length)) {
        cut = search->subject_length;
        for (steps = 0; steps <= search->subject_length; steps++) {
            if (parameter_pattern_matches_at(search, cut)) {
                search->selected = cut;
                search->found = true;
                break;
            }
            if (cut == 0) break;
            cut--;
        }
        return GSH_NATIVE_PLAN_OK;
    }
    (void)memset(&character_state, 0, sizeof(character_state));
    for (steps = 0; steps <= search->subject_length; steps++) {
        if (parameter_pattern_matches_at(search, cut)) {
            search->selected = cut;
            search->found = true;
            if (search->first_match) break;
        }
        if (cut == search->subject_length) break;
        {
            size_t amount = mbrlen(search->subject + cut,
                                   search->subject_length - cut,
                                   &character_state);

            if (amount == (size_t)-1 || amount == (size_t)-2 || amount == 0) {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            cut += amount;
        }
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status select_general_parameter_pattern(
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline, char *pattern, size_t pattern_length,
    char *subject, size_t subject_length,
    gsh_parameter_pattern_operation operation, bool *found,
    size_t *selected)
{
    if (context == NULL || subject == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    parameter_pattern_search search;
    gsh_native_plan_status status;

    if (!require(pipeline != NULL && pattern != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(found != NULL && selected != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    status = prepare_parameter_pattern_search(
        context, pipeline, pattern, pattern_length, subject, subject_length,
        operation, &search);
    if (status != GSH_NATIVE_PLAN_OK) return status;
    if (parameter_pattern_scan_required(
            context, &search,
            GSH_NATIVE_TEXT_CAP - (size_t)(pattern - pipeline->text))) {
        status = scan_parameter_pattern(&search);
    }
    *found = search.found;
    *selected = search.selected;
    return status;
}

static gsh_native_plan_status remove_parameter_pattern(
    const char *input, gsh_word_ref word,
    const gsh_native_expansion_context *context, gsh_native_pipeline *pipeline,
    char *pattern, size_t pattern_length, size_t value_begin,
    const char *parameter_value, gsh_parameter_pattern_operation operation)
{
    if (context == NULL || parameter_value == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    char *subject_scratch =
        pipeline->heredoc_text + pipeline->heredoc_text_used;
    size_t subject_capacity = GSH_NATIVE_HEREDOC_TEXT_CAP -
                              pipeline->heredoc_text_used;
    const char *subject = parameter_value;
    size_t subject_length = strnlen(parameter_value, subject_capacity);
    size_t result_begin = 0;
    size_t result_length = subject_length;
    gsh_native_plan_status status;

    if (subject_length == subject_capacity) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (!pattern_word_preserves_variables(input, word)) {
        (void)memcpy(subject_scratch, parameter_value, subject_length + 1U);
        subject = subject_scratch;
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
            size_t selected_general = 0;
            bool found = false;
            bool prefix = operation == GSH_REMOVE_SMALLEST_PREFIX ||
                          operation == GSH_REMOVE_LARGEST_PREFIX;
            char *match_subject = subject_scratch;

            if (subject != subject_scratch) {
                (void)memcpy(subject_scratch, subject, subject_length + 1U);
            }
            status = select_general_parameter_pattern(
                context, pipeline, pattern, pattern_length, match_subject,
                subject_length, operation, &found, &selected_general);
            if (status != GSH_NATIVE_PLAN_OK) return status;
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

static gsh_native_plan_status reserve_parameter_reference(
    const gsh_native_expansion_context *context, const char *name,
    size_t name_length, const char *value, bool found,
    gsh_parameter_kind kind, bool quoted, gsh_expansion_mode mode,
    size_t value_begin, gsh_native_pipeline *pipeline)
{
    gsh_native_plan_status status;

    if (!require(context != NULL && name != NULL && value != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(pipeline != NULL && value_begin <= pipeline->text_used)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (kind == GSH_PARAMETER_VALUE && !found) {
        status = report_unset_parameter(context, name, name_length);
        if (status != GSH_NATIVE_PLAN_OK) return status;
        if (context->preflight) value = "0";
    }
    status = kind == GSH_PARAMETER_VALUE
                 ? reserve_parameter_value(pipeline, value)
                 : reserve_positional_parameters(context, kind, quoted,
                                                  mode, pipeline);
    if (status == GSH_NATIVE_PLAN_OK && kind == GSH_PARAMETER_VALUE) {
        mark_expansion(pipeline, value_begin, quoted);
    }
    return status;
}

static gsh_native_plan_status reserve_parameter_length(
    const gsh_native_expansion_context *context, const char *name,
    size_t name_length, const char *value, bool found,
    gsh_parameter_kind kind, bool quoted, size_t value_begin,
    gsh_native_pipeline *pipeline)
{
    unsigned long length;
    gsh_native_plan_status status;

    if (!require(context != NULL && name != NULL && value != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(pipeline != NULL && kind == GSH_PARAMETER_VALUE)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!found) {
        status = report_unset_parameter(context, name, name_length);
        if (status != GSH_NATIVE_PLAN_OK) return status;
    }
    status = parameter_character_length(value, &length);
    if (status == GSH_NATIVE_PLAN_OK && !reserve_decimal(pipeline, length)) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (status == GSH_NATIVE_PLAN_OK) {
        mark_expansion(pipeline, value_begin, quoted);
    }
    return status;
}

static gsh_parameter_pattern_operation parameter_pattern_operation(
    unsigned char operator, bool largest)
{
    if (!require(operator == '#' || operator == '%')) {
        return GSH_REMOVE_SMALLEST_PREFIX;
    }
    if (operator == '#') {
        return largest ? GSH_REMOVE_LARGEST_PREFIX
                       : GSH_REMOVE_SMALLEST_PREFIX;
    }
    return largest ? GSH_REMOVE_LARGEST_SUFFIX
                   : GSH_REMOVE_SMALLEST_SUFFIX;
}

static bool parameter_operator_details(
    const char *input, size_t parameter_end, size_t closing,
    bool found, const char *value, bool *colon, size_t *word_begin,
    unsigned char *operator, bool *use_word)
{
    size_t operator_offset;

    if (!require(input != NULL && value != NULL && colon != NULL)) {
        return false;
    }
    if (!require(word_begin != NULL && operator != NULL && use_word != NULL)) {
        return false;
    }
    *colon = input[parameter_end] == ':';
    operator_offset = parameter_end + (*colon ? 1U : 0U);
    if (operator_offset >= closing) return false;
    *operator = (unsigned char)input[operator_offset++];
    if (*operator != '-' && *operator != '+' && *operator != '=' &&
        *operator != '?') return false;
    *word_begin = operator_offset;
    if (*operator == '-' || *operator == '=') {
        *use_word = !found || (*colon && *value == '\0');
    } else if (*operator == '+') {
        *use_word = found && (!*colon || *value != '\0');
    } else {
        *use_word = !found || (*colon && *value == '\0');
    }
    return true;
}

static gsh_native_plan_status commit_parameter_operator(
    const char *input, size_t cursor, size_t parameter_end, bool colon,
    unsigned char operator, size_t word_begin, size_t closing,
    const gsh_native_expansion_context *context, char *expanded_word,
    size_t expanded_length, size_t value_begin, gsh_native_pipeline *pipeline)
{
    gsh_native_plan_status status;

    if (!require(input != NULL && context != NULL && pipeline != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(expanded_word != NULL && word_begin <= closing)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    status = GSH_NATIVE_PLAN_OK;
    if (operator == '=') {
        if (!name_start((unsigned char)input[cursor]) ||
            parameter_end != word_begin - (colon ? 2U : 1U) ||
            context->variable_state == NULL) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        status = native_variable_assign(
            context->variable_state, input + cursor, parameter_end - cursor,
            expanded_word, expanded_length);
    } else if (operator == '?') {
        if (context->variable_state == NULL) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        status = native_parameter_error(
            context->variable_state, input + cursor, parameter_end - cursor,
            expanded_word, expanded_length, word_begin == closing);
    }
    if (status != GSH_NATIVE_PLAN_OK) return status;
    pipeline->text_used--;
    if (operator == '?') pipeline->text_used = value_begin;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status defer_parameter_pattern(
    expansion_machine *machine, expansion_word_frame *frame,
    size_t value_begin, size_t cursor, size_t parameter_end,
    size_t closing, size_t word_begin, const char *parameter_value,
    bool quoted, gsh_parameter_pattern_operation operation, size_t depth)
{
    if (!require(machine != NULL && frame != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(parameter_value != NULL && word_begin <= closing)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    frame->parameter = (expansion_parameter_pending){
        .value_begin = value_begin,
        .cursor = cursor,
        .parameter_end = parameter_end,
        .closing = closing,
        .word_begin = word_begin,
        .parameter_value = parameter_value,
        .quoted = quoted,
        .pattern_operation = operation,
    };
    frame->continuation = EXPANSION_CONTINUATION_PATTERN;
    if (request_expansion_word(
            machine, (gsh_word_ref){word_begin, closing},
            GSH_EXPAND_SCALAR, depth + 1U)) return GSH_NATIVE_PLAN_DEFERRED;
    frame->continuation = EXPANSION_CONTINUATION_NONE;
    return GSH_NATIVE_PLAN_LIMIT;
}

static gsh_native_plan_status defer_parameter_operator(
    expansion_machine *machine, expansion_word_frame *frame,
    size_t value_begin, size_t cursor, size_t parameter_end,
    size_t closing, size_t word_begin, const char *parameter_value,
    bool quoted, bool colon, unsigned char operator,
    gsh_expansion_mode mode, size_t depth)
{
    if (!require(machine != NULL && frame != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(parameter_value != NULL && word_begin <= closing)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    frame->parameter = (expansion_parameter_pending){
        .value_begin = value_begin,
        .cursor = cursor,
        .parameter_end = parameter_end,
        .closing = closing,
        .word_begin = word_begin,
        .parameter_value = parameter_value,
        .quoted = quoted,
        .colon = colon,
        .operator = operator,
    };
    frame->continuation = EXPANSION_CONTINUATION_OPERATOR;
    if (request_expansion_word(
            machine, (gsh_word_ref){word_begin, closing},
            operator == '?' ? GSH_EXPAND_SCALAR : mode,
            depth + 1U)) return GSH_NATIVE_PLAN_DEFERRED;
    frame->continuation = EXPANSION_CONTINUATION_NONE;
    return GSH_NATIVE_PLAN_LIMIT;
}

static gsh_native_plan_status expand_unbraced_parameter(
    const char *input, size_t end, size_t *offset, bool quoted,
    gsh_expansion_mode mode,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline, expansion_word_frame *frame)
{
    if (pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    const char *value;
    bool found;
    gsh_parameter_kind kind;
    size_t value_begin = pipeline->text_used;
    size_t parameter_end;
    gsh_native_plan_status status;

    if (!require(input != NULL && offset != NULL && context != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(pipeline != NULL && frame != NULL && *offset < end)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    status = parameter_reference(
        input, end, *offset, context, false, &parameter_end, &value, &found,
        &kind, frame->numeric);
    if (status != GSH_NATIVE_PLAN_OK) return status;
    status = reserve_parameter_reference(
        context, input + *offset, parameter_end - *offset, value, found,
        kind, quoted, mode, value_begin, pipeline);
    if (status == GSH_NATIVE_PLAN_OK) *offset = parameter_end;
    return status;
}

static gsh_native_plan_status finish_simple_braced_parameter(
    const char *input, size_t *offset, size_t cursor, size_t parameter_end,
    size_t closing, bool length_form, const char *value, bool found,
    gsh_parameter_kind kind, bool quoted, gsh_expansion_mode mode,
    const gsh_native_expansion_context *context, size_t value_begin,
    gsh_native_pipeline *pipeline, bool *handled)
{
    gsh_native_plan_status status;

    if (!require(input != NULL && offset != NULL && value != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(context != NULL && pipeline != NULL && handled != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    *handled = length_form || parameter_end == closing;
    if (!*handled) return GSH_NATIVE_PLAN_OK;
    if (length_form) {
        if (parameter_end != closing || kind != GSH_PARAMETER_VALUE) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        status = reserve_parameter_length(
            context, input + cursor, parameter_end - cursor, value, found,
            kind, quoted, value_begin, pipeline);
    } else {
        status = reserve_parameter_reference(
            context, input + cursor, parameter_end - cursor, value, found,
            kind, quoted, mode, value_begin, pipeline);
    }
    if (status == GSH_NATIVE_PLAN_OK) *offset = closing + 1U;
    return status;
}

static gsh_native_plan_status expand_parameter_pattern(
    const char *input, size_t cursor, size_t parameter_end, size_t closing,
    const char *parameter_value, bool parameter_found, bool quoted,
    size_t depth, const gsh_native_expansion_context *context,
    expansion_machine *machine, expansion_word_frame *frame,
    size_t value_begin)
{
    unsigned char operator;
    size_t word_begin;
    bool largest;
    gsh_native_plan_status status;

    if (input == NULL || parameter_value == NULL || context == NULL ||
        machine == NULL || frame == NULL) return GSH_NATIVE_PLAN_ERROR;
    operator = (unsigned char)input[parameter_end];
    word_begin = parameter_end + 1U;
    largest = word_begin < closing &&
              (unsigned char)input[word_begin] == operator;
    if (!parameter_found) {
        status = report_unset_parameter(
            context, input + cursor, parameter_end - cursor);
        if (status != GSH_NATIVE_PLAN_OK) return status;
    }
    if (depth == 32U) return GSH_NATIVE_PLAN_LIMIT;
    if (largest) word_begin++;
    return defer_parameter_pattern(
        machine, frame, value_begin, cursor, parameter_end, closing,
        word_begin, parameter_value, quoted,
        parameter_pattern_operation(operator, largest), depth);
}

static gsh_native_plan_status expand_parameter(
    const char *input, size_t end, size_t *offset, bool quoted,
    gsh_expansion_mode mode, size_t depth,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline, expansion_machine *machine,
    expansion_word_frame *frame)
{
    if (input == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (context == NULL || frame == NULL || machine == NULL ||
        offset == NULL || pipeline == NULL || *offset == end) {
        return context == NULL || frame == NULL || machine == NULL ||
                       offset == NULL || pipeline == NULL
                   ? GSH_NATIVE_PLAN_LIMIT
                   : GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    const char *parameter_value;
    bool parameter_found;
    gsh_parameter_kind parameter_kind;
    size_t value_begin = pipeline->text_used;
    size_t parameter_end;
    gsh_native_plan_status status;

    if (input[*offset] != '{') return expand_unbraced_parameter(
        input, end, offset, quoted, mode, context, pipeline, frame);
    {
        size_t closing;
        size_t cursor = *offset + 1U;
        bool length_form = false;
        bool handled = false;

        status = find_parameter_end(input, end, *offset, &closing);
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
                                     frame->numeric);
        if (status != GSH_NATIVE_PLAN_OK) {
            return status;
        }
        status = finish_simple_braced_parameter(
            input, offset, cursor, parameter_end, closing, length_form,
            parameter_value, parameter_found, parameter_kind, quoted, mode,
            context, value_begin, pipeline, &handled);
        if (handled || status != GSH_NATIVE_PLAN_OK) return status;
        if (parameter_kind != GSH_PARAMETER_VALUE) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        if (input[parameter_end] == '#' || input[parameter_end] == '%') {
            return expand_parameter_pattern(
                input, cursor, parameter_end, closing, parameter_value,
                parameter_found, quoted, depth, context, machine, frame,
                value_begin);
        }
        {
            bool colon;
            size_t operator_offset;
            unsigned char operator;
            bool use_word;

            if (!parameter_operator_details(
                    input, parameter_end, closing, parameter_found,
                    parameter_value, &colon, &operator_offset, &operator,
                    &use_word)) {
                return GSH_NATIVE_PLAN_UNSUPPORTED;
            }
            if (use_word) {
                if (depth == 32U) {
                    return GSH_NATIVE_PLAN_LIMIT;
                }
                return defer_parameter_operator(
                    machine, frame, value_begin, cursor, parameter_end,
                    closing, operator_offset, parameter_value, quoted,
                    colon, operator, mode, depth);
            } else {
                status = reserve_parameter_value(pipeline, parameter_value);
                if (status != GSH_NATIVE_PLAN_OK) {
                    return status;
                }
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
    if (stack == NULL) return false;
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
    if (input == NULL) return false;
    if (offset == NULL || stack == NULL) {
        return false;
    }
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

static gsh_native_plan_status scan_substitution_quote(
    const char *input, size_t end, size_t *offset,
    substitution_context_stack *stack, bool *handled)
{
    substitution_context_kind kind;
    unsigned char byte;

    if (!require(input != NULL && offset != NULL && stack != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(*offset < end && stack->length > 0 && handled != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    kind = stack->items[stack->length - 1U];
    byte = (unsigned char)input[*offset];
    *handled = kind == SUBSTITUTION_SINGLE ||
               kind == SUBSTITUTION_DOLLAR_SINGLE ||
               kind == SUBSTITUTION_BACKQUOTE ||
               kind == SUBSTITUTION_DOUBLE;
    if (!*handled) return GSH_NATIVE_PLAN_OK;
    if (kind == SUBSTITUTION_SINGLE) {
        (*offset)++;
        if (byte == '\'') stack->length--;
        return GSH_NATIVE_PLAN_OK;
    }
    if (kind == SUBSTITUTION_DOLLAR_SINGLE ||
        kind == SUBSTITUTION_BACKQUOTE) {
        if (byte == '\\' && *offset + 1U < end) *offset += 2U;
        else {
            (*offset)++;
            if ((kind == SUBSTITUTION_DOLLAR_SINGLE && byte == '\'') ||
                (kind == SUBSTITUTION_BACKQUOTE && byte == 0x60U)) {
                stack->length--;
            }
        }
        return GSH_NATIVE_PLAN_OK;
    }
    if (byte == '"') {
        (*offset)++;
        stack->length--;
    } else if (byte == '\\' && *offset + 1U < end) {
        *offset += 2U;
    } else if (byte == 0x60U) {
        if (!substitution_push(stack, SUBSTITUTION_BACKQUOTE)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        (*offset)++;
    } else if (byte == '$' &&
               substitution_nested(input, end, offset, stack)) {
        return GSH_NATIVE_PLAN_OK;
    } else {
        (*offset)++;
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status scan_substitution_unquoted(
    const char *input, size_t end, size_t *offset,
    substitution_context_stack *stack, size_t *contents_end,
    size_t *after, bool *complete);

static gsh_native_plan_status scan_substitution_closer(
    const char *input, size_t end, size_t *offset,
    substitution_context_stack *stack, size_t *contents_end,
    size_t *after, bool *complete)
{
    substitution_context_kind kind;
    unsigned char byte;

    if (!require(input != NULL && offset != NULL && stack != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(*offset < end && contents_end != NULL && after != NULL &&
                 complete != NULL)) return GSH_NATIVE_PLAN_UNSUPPORTED;
    if (!require(stack->length > 0)) return GSH_NATIVE_PLAN_UNSUPPORTED;
    kind = stack->items[stack->length - 1U];
    byte = (unsigned char)input[*offset];
    if (kind == SUBSTITUTION_PARAMETER && byte == '}') {
        (*offset)++;
        stack->length--;
    } else if ((kind == SUBSTITUTION_COMMAND ||
                kind == SUBSTITUTION_PARENTHESIS) && byte == '(') {
        if (!substitution_push(stack, SUBSTITUTION_PARENTHESIS)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        (*offset)++;
    } else if (kind == SUBSTITUTION_PARENTHESIS && byte == ')') {
        (*offset)++;
        stack->length--;
    } else if (kind == SUBSTITUTION_COMMAND && byte == ')') {
        stack->length--;
        if (stack->length == 0) {
            *contents_end = *offset;
            *after = *offset + 1U;
            *complete = true;
        } else (*offset)++;
    } else if (kind == SUBSTITUTION_ARITHMETIC &&
               *offset + 1U < end && byte == ')' &&
               input[*offset + 1U] == ')') {
        size_t closing = *offset;

        *offset += 2U;
        stack->length--;
        if (stack->length == 0) {
            *contents_end = closing;
            *after = *offset;
            *complete = true;
        }
    } else if (kind == SUBSTITUTION_ARITHMETIC && byte == '(') {
        if (!substitution_push(stack, SUBSTITUTION_PARENTHESIS)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        (*offset)++;
    } else (*offset)++;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status scan_substitution_unquoted(
    const char *input, size_t end, size_t *offset,
    substitution_context_stack *stack, size_t *contents_end,
    size_t *after, bool *complete)
{
    if (after == NULL || contents_end == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    substitution_context_kind kind;
    unsigned char byte;

    if (!require(input != NULL && offset != NULL && stack != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(*offset < end && stack->length > 0 &&
                 complete != NULL)) return GSH_NATIVE_PLAN_UNSUPPORTED;
    kind = stack->items[stack->length - 1U];
    byte = (unsigned char)input[*offset];
    *complete = false;
    if (byte == '\\' && *offset + 1U < end) *offset += 2U;
    else if (byte == '\'' && kind != SUBSTITUTION_ARITHMETIC) {
        if (!substitution_push(stack, SUBSTITUTION_SINGLE)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        (*offset)++;
    } else if (byte == '"' && kind != SUBSTITUTION_ARITHMETIC) {
        if (!substitution_push(stack, SUBSTITUTION_DOUBLE)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        (*offset)++;
    } else if (byte == 0x60U) {
        if (!substitution_push(stack, SUBSTITUTION_BACKQUOTE)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        (*offset)++;
    } else if (byte == '$' &&
               substitution_nested(input, end, offset, stack)) {
        return GSH_NATIVE_PLAN_OK;
    } else {
        return scan_substitution_closer(
            input, end, offset, stack, contents_end, after, complete);
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status find_substitution_end(
    const char *input, size_t end, size_t opening_parenthesis,
    bool arithmetic, size_t *contents_end, size_t *after)
{
    if (input == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (after == NULL || contents_end == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    substitution_context_stack stack = {{0}, 0};
    size_t offset = opening_parenthesis + (arithmetic ? 2U : 1U);
    size_t steps;

    if (!substitution_push(&stack, arithmetic ? SUBSTITUTION_ARITHMETIC
                                              : SUBSTITUTION_COMMAND)) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    for (steps = 0; steps <= end - opening_parenthesis; steps++) {
        bool handled;
        bool complete;
        gsh_native_plan_status status;

        if (offset >= end || input[offset] == '\0') break;
        status = scan_substitution_quote(input, end, &offset, &stack,
                                         &handled);
        if (status != GSH_NATIVE_PLAN_OK) return status;
        if (handled) continue;
        status = scan_substitution_unquoted(
            input, end, &offset, &stack, contents_end, after, &complete);
        if (status != GSH_NATIVE_PLAN_OK || complete) return status;
    }
    return GSH_NATIVE_PLAN_UNSUPPORTED;
}

static gsh_native_plan_status expand_command_substitution(
    const char *input, size_t end, size_t *offset,
    const gsh_native_expansion_context *context, char *output,
    size_t output_capacity, size_t *output_used)
{
    if (offset == NULL || input == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (output == NULL || output_used == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    size_t commands_end;
    size_t after;
    size_t produced = 0;
    int exit_status = 0;
    gsh_native_plan_status status;

    if (context == NULL || context->substitutions == NULL ||
        *offset >= end || input[*offset] != '(' ||
        (*offset + 1U < end && input[*offset + 1U] == '(')) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    status = find_substitution_end(input, end, *offset, false,
                                   &commands_end, &after);
    if (status != GSH_NATIVE_PLAN_OK) {
        return status;
    }
    status = native_command_substitution(
        context, input + *offset + 1U,
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
    if (input == NULL || offset == NULL || output == NULL || output_used == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    char commands[GSH_NATIVE_TEXT_CAP];
    size_t command_used = 0;
    size_t cursor = *offset;
    size_t produced = 0;
    int exit_status = 0;
    gsh_native_plan_status status;

    if (context == NULL || context->substitutions == NULL) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    while (cursor < end) {
        unsigned char byte = (unsigned char)input[cursor++];

        if (byte == 0x60U) {
            status = native_command_substitution(
                context, commands, command_used,
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

typedef struct {
    size_t name_begin;
    size_t name_length;
    arithmetic_assignment_operator operator;
    long left;
} arithmetic_assignment_frame;

typedef enum {
    ARITHMETIC_FRAME_COMMA,
    ARITHMETIC_FRAME_ASSIGNMENT,
    ARITHMETIC_FRAME_CONDITIONAL,
    ARITHMETIC_FRAME_LOGICAL_OR,
    ARITHMETIC_FRAME_LOGICAL_AND,
    ARITHMETIC_FRAME_BITOR,
    ARITHMETIC_FRAME_BITXOR,
    ARITHMETIC_FRAME_BITAND,
    ARITHMETIC_FRAME_EQUALITY,
    ARITHMETIC_FRAME_RELATIONAL,
    ARITHMETIC_FRAME_SHIFT,
    ARITHMETIC_FRAME_ADD,
    ARITHMETIC_FRAME_MULTIPLY,
    ARITHMETIC_FRAME_UNARY,
    ARITHMETIC_FRAME_PRIMARY,
} arithmetic_frame_kind;

typedef enum {
    ARITHMETIC_OPERATOR_NONE,
    ARITHMETIC_OPERATOR_MULTIPLY,
    ARITHMETIC_OPERATOR_DIVIDE,
    ARITHMETIC_OPERATOR_REMAINDER,
    ARITHMETIC_OPERATOR_ADD,
    ARITHMETIC_OPERATOR_SUBTRACT,
    ARITHMETIC_OPERATOR_SHIFT_LEFT,
    ARITHMETIC_OPERATOR_SHIFT_RIGHT,
    ARITHMETIC_OPERATOR_LESS,
    ARITHMETIC_OPERATOR_LESS_EQUAL,
    ARITHMETIC_OPERATOR_GREATER,
    ARITHMETIC_OPERATOR_GREATER_EQUAL,
    ARITHMETIC_OPERATOR_EQUAL,
    ARITHMETIC_OPERATOR_NOT_EQUAL,
    ARITHMETIC_OPERATOR_BITAND,
    ARITHMETIC_OPERATOR_BITXOR,
    ARITHMETIC_OPERATOR_BITOR,
    ARITHMETIC_OPERATOR_LOGICAL_AND,
    ARITHMETIC_OPERATOR_LOGICAL_OR,
} arithmetic_operator;

typedef struct {
    arithmetic_frame_kind kind;
    unsigned int phase;
    bool evaluate;
    bool condition;
    long value;
    long saved;
    arithmetic_operator operator;
    arithmetic_assignment_frame assignment;
} arithmetic_parse_frame;

enum {
    ARITHMETIC_FRAME_CAP = 512,
    ARITHMETIC_MACHINE_STEP_CAP = GSH_NATIVE_TEXT_CAP * 64,
};

typedef struct {
    arithmetic_parser *parser;
    arithmetic_parse_frame frames[ARITHMETIC_FRAME_CAP];
    size_t depth;
    long value;
    bool returned;
} arithmetic_machine;

static const gsh_native_expansion_context *arithmetic_context(
    const arithmetic_parser *parser)
{
    if (!require(parser != NULL)) return NULL;
    if (!require(parser->context != NULL)) return NULL;
    return parser->context;
}

static arithmetic_parser *arithmetic_machine_parser(
    const arithmetic_machine *machine)
{
    if (!require(machine != NULL)) return NULL;
    if (!require(machine->parser != NULL)) return NULL;
    return machine->parser;
}

static bool arithmetic_failure(arithmetic_parser *parser,
                               const char *diagnostic)
{
    if (parser == NULL) return false;
    if (parser->diagnostic == NULL) {
        parser->diagnostic = diagnostic;
    }
    return false;
}

static void arithmetic_skip_offset(const arithmetic_parser *parser,
                                   size_t *offset)
{
    if (offset == NULL || parser == NULL) {
        return;
    }
    while (*offset < parser->end &&
           (parser->input[*offset] == ' ' ||
            parser->input[*offset] == '\t' ||
            parser->input[*offset] == '\n')) {
        (*offset)++;
    }
}

static void arithmetic_skip_space(arithmetic_parser *parser)
{
    if (parser == NULL) {
        return;
    }
    arithmetic_skip_offset(parser, &parser->offset);
}

static bool arithmetic_match(arithmetic_parser *parser, const char *text)
{
    if (parser == NULL || text == NULL) {
        return false;
    }
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
    if (parser == NULL) {
        return false;
    }
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
    if (text == NULL) return false;
    if (value == NULL) {
        return false;
    }
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
    if (parser == NULL) return false;
    if (name == NULL || value == NULL) {
        return false;
    }
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
    if (result == NULL) {
        return false;
    }
    if ((right > 0 && left > LONG_MAX - right) ||
        (right < 0 && left < LONG_MIN - right)) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool arithmetic_subtract(long left, long right, long *result)
{
    if (result == NULL) {
        return false;
    }
    if ((right > 0 && left < LONG_MIN + right) ||
        (right < 0 && left > LONG_MAX + right)) {
        return false;
    }
    *result = left - right;
    return true;
}

static bool arithmetic_multiply(long left, long right, long *result)
{
    if (result == NULL) {
        return false;
    }
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

static bool arithmetic_assignment_operator_at(
    const arithmetic_parser *parser, size_t cursor,
    arithmetic_assignment_operator *operator, size_t *length)
{
    unsigned char byte;

    if (!require(parser != NULL && operator != NULL && length != NULL)) {
        return false;
    }
    if (!require(cursor < parser->end)) return false;
    byte = (unsigned char)parser->input[cursor];
    if ((byte == '<' || byte == '>') && cursor + 2U < parser->end &&
        parser->input[cursor + 1U] == (char)byte &&
        parser->input[cursor + 2U] == '=') {
        *operator = byte == '<' ? ARITHMETIC_ASSIGN_SHIFT_LEFT
                                : ARITHMETIC_ASSIGN_SHIFT_RIGHT;
        *length = 3U;
        return true;
    }
    if (cursor + 1U < parser->end && parser->input[cursor + 1U] == '=') {
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
        *length = 2U;
        return true;
    }
    if (byte != '=' ||
        (cursor + 1U < parser->end && parser->input[cursor + 1U] == '=')) {
        return false;
    }
    *operator = ARITHMETIC_ASSIGN_SET;
    *length = 1U;
    return true;
}

static bool arithmetic_assignment_target(
    arithmetic_parser *parser, size_t *name_begin, size_t *name_length,
    arithmetic_assignment_operator *operator)
{
    if (name_begin == NULL || name_length == NULL || parser == NULL) {
        return false;
    }
    size_t cursor = parser->offset;
    size_t parentheses = 0;
    size_t operator_length = 0;

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
    if (!arithmetic_assignment_operator_at(parser, cursor, operator,
                                           &operator_length)) return false;
    parser->offset = cursor + operator_length;
    return true;
}

static bool arithmetic_apply_assignment(
    arithmetic_assignment_operator operator, long left, long right,
    long *result)
{
    if (result == NULL) {
        return false;
    }
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

static gsh_native_plan_status reserve_arithmetic_value(
    char *output, size_t capacity, size_t *used, long value);

static bool arithmetic_commit_assignment(
    arithmetic_parser *parser, const arithmetic_assignment_frame *frame,
    long right, long *value)
{
    if (frame == NULL) return false;
    if (parser == NULL) {
        return false;
    }
    char text[3U * sizeof(long) + 2U];
    size_t text_length = 0U;
    gsh_native_plan_status status;

    if (!arithmetic_apply_assignment(frame->operator, frame->left, right,
                                     value)) {
        return arithmetic_failure(
            parser,
            (frame->operator == ARITHMETIC_ASSIGN_DIVIDE ||
             frame->operator == ARITHMETIC_ASSIGN_REMAINDER) && right == 0
                ? "division by zero"
                : (frame->operator == ARITHMETIC_ASSIGN_SHIFT_LEFT ||
                   frame->operator == ARITHMETIC_ASSIGN_SHIFT_RIGHT)
                      ? "shift count or value is out of range"
                      : "integer overflow");
    }
    if (parser->context == NULL ||
        arithmetic_context(parser)->variable_state == NULL) {
        return false;
    }
    status = reserve_arithmetic_value(text, sizeof(text), &text_length,
                                      *value);
    if (status == GSH_NATIVE_PLAN_OK) {
        status = native_variable_assign(
            arithmetic_context(parser)->variable_state,
            parser->input + frame->name_begin, frame->name_length,
            text, text_length);
    }
    if (status != GSH_NATIVE_PLAN_OK) {
        parser->error = status;
        return false;
    }
    return true;
}

static bool arithmetic_parse_atom(arithmetic_parser *parser, bool evaluate,
                                  long *value)
{
    if (parser == NULL) {
        return false;
    }
    size_t begin;
    bool explicit_parameter = false;

    arithmetic_skip_space(parser);
    if (parser->offset == parser->end) return false;
    if (parser->input[parser->offset] >= '0' &&
        parser->input[parser->offset] <= '9') {
        unsigned int base = 10U;

        begin = parser->offset;
        if (parser->input[parser->offset] == '0') {
            base = 8U;
            parser->offset++;
            if (parser->offset < parser->end &&
                (parser->input[parser->offset] == 'x' ||
                 parser->input[parser->offset] == 'X')) {
                base = 16U;
                parser->offset++;
            }
        }
        while (parser->offset < parser->end) {
            int digit = arithmetic_digit(
                (unsigned char)parser->input[parser->offset]);

            if (digit < 0 || (unsigned int)digit >= base) break;
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
                !name_start((unsigned char)parser->input[begin])) return false;
            parser->offset++;
            while (parser->offset < parser->end &&
                   name_byte((unsigned char)parser->input[parser->offset])) {
                parser->offset++;
            }
            if (parser->offset == parser->end ||
                parser->input[parser->offset++] != '}') return false;
            return arithmetic_environment_value(
                parser, parser->input + begin,
                parser->offset - begin - 1U, true, value);
        }
        begin = parser->offset;
    }
    if (begin >= parser->end ||
        !name_start((unsigned char)parser->input[begin])) return false;
    parser->offset = begin + 1U;
    while (parser->offset < parser->end &&
           name_byte((unsigned char)parser->input[parser->offset])) {
        parser->offset++;
    }
    return arithmetic_environment_value(
        parser, parser->input + begin, parser->offset - begin,
        evaluate || explicit_parameter, value);
}

static bool arithmetic_machine_push(arithmetic_machine *machine,
                                    arithmetic_frame_kind kind,
                                    bool evaluate)
{
    arithmetic_parse_frame *frame;

    if (!require(machine != NULL && machine->parser != NULL)) return false;
    if (!require(kind >= ARITHMETIC_FRAME_COMMA &&
                 kind <= ARITHMETIC_FRAME_PRIMARY) ||
        machine->depth >= ARITHMETIC_FRAME_CAP || machine->returned) {
        arithmetic_machine_parser(machine)->error = GSH_NATIVE_PLAN_LIMIT;
        return false;
    }
    frame = &machine->frames[machine->depth++];
    (void)memset(frame, 0, sizeof(*frame));
    frame->kind = kind;
    frame->evaluate = evaluate;
    return true;
}

static void arithmetic_machine_complete(arithmetic_machine *machine,
                                        long value)
{
    if (machine == NULL) {
        return;
    }
    machine->depth--;
    machine->value = value;
    machine->returned = true;
}

static bool arithmetic_machine_take(arithmetic_machine *machine,
                                    long *value)
{
    if (!require(machine != NULL && value != NULL &&
                 machine->parser != NULL)) return false;
    if (!machine->returned) return false;
    *value = machine->value;
    machine->returned = false;
    return true;
}

static arithmetic_operator arithmetic_single_binary(
    arithmetic_parser *parser, char first, char excluded,
    arithmetic_operator operator)
{
    if (parser == NULL) {
        return ARITHMETIC_OPERATOR_NONE;
    }
    arithmetic_skip_space(parser);
    if (parser->offset == parser->end ||
        parser->input[parser->offset] != first ||
        (parser->offset + 1U < parser->end &&
         (parser->input[parser->offset + 1U] == excluded ||
          parser->input[parser->offset + 1U] == '='))) {
        return ARITHMETIC_OPERATOR_NONE;
    }
    parser->offset++;
    return operator;
}

static arithmetic_operator arithmetic_next_multiply(
    arithmetic_parser *parser)
{
    if (parser == NULL) {
        return ARITHMETIC_OPERATOR_NONE;
    }
    unsigned char byte;

    arithmetic_skip_space(parser);
    if (parser->offset == parser->end) return ARITHMETIC_OPERATOR_NONE;
    byte = (unsigned char)parser->input[parser->offset];
    if ((byte != '*' && byte != '/' && byte != '%') ||
        (parser->offset + 1U < parser->end &&
         parser->input[parser->offset + 1U] == '=')) {
        return ARITHMETIC_OPERATOR_NONE;
    }
    parser->offset++;
    if (byte == '*') return ARITHMETIC_OPERATOR_MULTIPLY;
    return byte == '/' ? ARITHMETIC_OPERATOR_DIVIDE
                       : ARITHMETIC_OPERATOR_REMAINDER;
}

static arithmetic_operator arithmetic_next_add(arithmetic_parser *parser)
{
    if (parser == NULL) {
        return ARITHMETIC_OPERATOR_NONE;
    }
    unsigned char byte;

    arithmetic_skip_space(parser);
    if (parser->offset == parser->end) return ARITHMETIC_OPERATOR_NONE;
    byte = (unsigned char)parser->input[parser->offset];
    if ((byte != '+' && byte != '-') ||
        (parser->offset + 1U < parser->end &&
         parser->input[parser->offset + 1U] == '=')) {
        return ARITHMETIC_OPERATOR_NONE;
    }
    parser->offset++;
    return byte == '+' ? ARITHMETIC_OPERATOR_ADD
                       : ARITHMETIC_OPERATOR_SUBTRACT;
}

static arithmetic_operator arithmetic_next_relational(
    arithmetic_parser *parser)
{
    if (arithmetic_match(parser, "<=")) {
        return ARITHMETIC_OPERATOR_LESS_EQUAL;
    }
    if (arithmetic_match(parser, ">=")) {
        return ARITHMETIC_OPERATOR_GREATER_EQUAL;
    }
    arithmetic_skip_space(parser);
    if (parser->offset < parser->end &&
        parser->input[parser->offset] == '<' &&
        (parser->offset + 1U == parser->end ||
         parser->input[parser->offset + 1U] != '<')) {
        parser->offset++;
        return ARITHMETIC_OPERATOR_LESS;
    }
    if (parser->offset < parser->end &&
        parser->input[parser->offset] == '>' &&
        (parser->offset + 1U == parser->end ||
         parser->input[parser->offset + 1U] != '>')) {
        parser->offset++;
        return ARITHMETIC_OPERATOR_GREATER;
    }
    return ARITHMETIC_OPERATOR_NONE;
}

static arithmetic_operator arithmetic_next_binary(
    arithmetic_parser *parser, arithmetic_frame_kind kind)
{
    if (!require(parser != NULL && parser->input != NULL)) {
        return ARITHMETIC_OPERATOR_NONE;
    }
    if (kind == ARITHMETIC_FRAME_MULTIPLY) {
        return arithmetic_next_multiply(parser);
    }
    if (kind == ARITHMETIC_FRAME_ADD) return arithmetic_next_add(parser);
    if (kind == ARITHMETIC_FRAME_SHIFT) {
        if (arithmetic_match_binary(parser, "<<")) {
            return ARITHMETIC_OPERATOR_SHIFT_LEFT;
        }
        return arithmetic_match_binary(parser, ">>")
                   ? ARITHMETIC_OPERATOR_SHIFT_RIGHT
                   : ARITHMETIC_OPERATOR_NONE;
    }
    if (kind == ARITHMETIC_FRAME_RELATIONAL) {
        return arithmetic_next_relational(parser);
    }
    if (kind == ARITHMETIC_FRAME_EQUALITY) {
        if (arithmetic_match(parser, "==")) return ARITHMETIC_OPERATOR_EQUAL;
        return arithmetic_match(parser, "!=")
                   ? ARITHMETIC_OPERATOR_NOT_EQUAL
                   : ARITHMETIC_OPERATOR_NONE;
    }
    if (kind == ARITHMETIC_FRAME_BITAND) {
        return arithmetic_single_binary(parser, '&', '&',
                                        ARITHMETIC_OPERATOR_BITAND);
    }
    if (kind == ARITHMETIC_FRAME_BITXOR) {
        return arithmetic_match_binary(parser, "^")
                   ? ARITHMETIC_OPERATOR_BITXOR
                   : ARITHMETIC_OPERATOR_NONE;
    }
    if (kind == ARITHMETIC_FRAME_BITOR) {
        return arithmetic_single_binary(parser, '|', '|',
                                        ARITHMETIC_OPERATOR_BITOR);
    }
    if (kind == ARITHMETIC_FRAME_LOGICAL_AND) {
        return arithmetic_match(parser, "&&")
                   ? ARITHMETIC_OPERATOR_LOGICAL_AND
                   : ARITHMETIC_OPERATOR_NONE;
    }
    return arithmetic_match(parser, "||")
               ? ARITHMETIC_OPERATOR_LOGICAL_OR
               : ARITHMETIC_OPERATOR_NONE;
}

static bool arithmetic_apply_binary(arithmetic_parser *parser,
                                    arithmetic_operator operator,
                                    long left, long right, long *value)
{
    if (!require(parser != NULL && value != NULL)) return false;
    unsigned long bits = (unsigned long)(sizeof(long) * CHAR_BIT);

    if (operator == ARITHMETIC_OPERATOR_MULTIPLY &&
        !arithmetic_multiply(left, right, value)) {
        return arithmetic_failure(parser, "integer overflow");
    }
    if (operator == ARITHMETIC_OPERATOR_DIVIDE ||
        operator == ARITHMETIC_OPERATOR_REMAINDER) {
        if (right == 0) return arithmetic_failure(parser, "division by zero");
        if (left == LONG_MIN && right == -1) {
            return arithmetic_failure(parser, "integer overflow");
        }
        *value = operator == ARITHMETIC_OPERATOR_DIVIDE ? left / right
                                                        : left % right;
    } else if ((operator == ARITHMETIC_OPERATOR_ADD &&
                !arithmetic_add(left, right, value)) ||
               (operator == ARITHMETIC_OPERATOR_SUBTRACT &&
                !arithmetic_subtract(left, right, value))) {
        return arithmetic_failure(parser, "integer overflow");
    } else if (operator == ARITHMETIC_OPERATOR_SHIFT_LEFT ||
               operator == ARITHMETIC_OPERATOR_SHIFT_RIGHT) {
        bool left_shift = operator == ARITHMETIC_OPERATOR_SHIFT_LEFT;

        if (right < 0 || (unsigned long)right >= bits ||
            (left_shift &&
             (left < 0 || left > (LONG_MAX >> (unsigned int)right)))) {
            return arithmetic_failure(
                parser, "shift count or value is out of range");
        }
        *value = left_shift ? left << (unsigned int)right
                            : left >> (unsigned int)right;
    } else if (operator >= ARITHMETIC_OPERATOR_LESS &&
               operator <= ARITHMETIC_OPERATOR_NOT_EQUAL) {
        *value = operator == ARITHMETIC_OPERATOR_LESS          ? left < right
                 : operator == ARITHMETIC_OPERATOR_LESS_EQUAL  ? left <= right
                 : operator == ARITHMETIC_OPERATOR_GREATER     ? left > right
                 : operator == ARITHMETIC_OPERATOR_GREATER_EQUAL
                     ? left >= right
                 : operator == ARITHMETIC_OPERATOR_EQUAL ? left == right
                                                         : left != right;
    } else if (operator == ARITHMETIC_OPERATOR_BITAND) {
        *value = left & right;
    } else if (operator == ARITHMETIC_OPERATOR_BITXOR) {
        *value = left ^ right;
    } else if (operator == ARITHMETIC_OPERATOR_BITOR) {
        *value = left | right;
    } else if (operator == ARITHMETIC_OPERATOR_LOGICAL_AND) {
        *value = left != 0 && right != 0;
    } else if (operator == ARITHMETIC_OPERATOR_LOGICAL_OR) {
        *value = left != 0 || right != 0;
    }
    return true;
}

static bool arithmetic_step_binary(arithmetic_machine *machine,
                                   arithmetic_parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->parser != NULL)) return false;
    arithmetic_frame_kind child =
        (arithmetic_frame_kind)((unsigned int)frame->kind + 1U);
    long right;

    if (frame->phase == 0U) {
        frame->phase = 1U;
        return arithmetic_machine_push(machine, child, frame->evaluate);
    }
    if (!arithmetic_machine_take(machine, &right)) return false;
    if (frame->phase == 2U && frame->evaluate &&
        !arithmetic_apply_binary(machine->parser, frame->operator,
                                 frame->value, right, &frame->value)) {
        return false;
    } else if (frame->phase == 1U) {
        frame->value = right;
    }
    frame->operator = arithmetic_next_binary(machine->parser, frame->kind);
    if (frame->operator == ARITHMETIC_OPERATOR_NONE) {
        arithmetic_machine_complete(machine,
                                    frame->evaluate ? frame->value : 0);
        return true;
    }
    frame->phase = 2U;
    return arithmetic_machine_push(
        machine, child,
        frame->evaluate &&
            (frame->operator != ARITHMETIC_OPERATOR_LOGICAL_AND ||
             frame->value != 0) &&
            (frame->operator != ARITHMETIC_OPERATOR_LOGICAL_OR ||
             frame->value == 0));
}

static bool arithmetic_step_primary(arithmetic_machine *machine,
                                    arithmetic_parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->parser != NULL)) return false;
    arithmetic_parser *parser = machine->parser;
    long value;

    if (frame->phase == 0U && arithmetic_match(parser, "(")) {
        if (parser->nesting == 32U) {
            parser->error = GSH_NATIVE_PLAN_LIMIT;
            return false;
        }
        parser->nesting++;
        frame->phase = 1U;
        return arithmetic_machine_push(machine, ARITHMETIC_FRAME_COMMA,
                                       frame->evaluate);
    }
    if (frame->phase == 1U) {
        if (!arithmetic_machine_take(machine, &value) ||
            !arithmetic_match(parser, ")")) return false;
        parser->nesting--;
        arithmetic_machine_complete(machine, value);
        return true;
    }
    if (!arithmetic_parse_atom(parser, frame->evaluate, &value)) return false;
    arithmetic_machine_complete(machine, frame->evaluate ? value : 0);
    return true;
}

static bool arithmetic_step_unary(arithmetic_machine *machine,
                                  arithmetic_parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->parser != NULL)) return false;
    arithmetic_parser *parser = machine->parser;
    long value;

    if (frame->phase == 0U) {
        const char operators[] = {'+', '-', '!', '~'};
        size_t index;

        for (index = 0U; index < sizeof(operators); index++) {
            char text[2] = {operators[index], '\0'};

            if (arithmetic_match(parser, text)) {
                frame->operator = (arithmetic_operator)operators[index];
                frame->phase = 1U;
                return arithmetic_machine_push(
                    machine, ARITHMETIC_FRAME_UNARY, frame->evaluate);
            }
        }
        frame->phase = 2U;
        return arithmetic_machine_push(machine, ARITHMETIC_FRAME_PRIMARY,
                                       frame->evaluate);
    }
    if (!arithmetic_machine_take(machine, &value)) return false;
    if (frame->evaluate && frame->phase == 1U) {
        char operator = (char)frame->operator;

        if (operator == '-' && value == LONG_MIN) {
            return arithmetic_failure(parser, "integer overflow");
        }
        if (operator == '-') value = -value;
        if (operator == '!') value = value == 0 ? 1 : 0;
        if (operator == '~') value = ~value;
    }
    arithmetic_machine_complete(machine, frame->evaluate ? value : 0);
    return true;
}

static bool arithmetic_step_conditional(arithmetic_machine *machine,
                                        arithmetic_parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->parser != NULL)) return false;
    arithmetic_parser *parser = machine->parser;
    long value;

    if (frame->phase == 0U) {
        frame->phase = 1U;
        return arithmetic_machine_push(machine, ARITHMETIC_FRAME_LOGICAL_OR,
                                       frame->evaluate);
    }
    if (!arithmetic_machine_take(machine, &value)) return false;
    if (frame->phase == 1U) {
        if (!arithmetic_match(parser, "?")) {
            arithmetic_machine_complete(machine,
                                        frame->evaluate ? value : 0);
            return true;
        }
        if (parser->nesting == 32U) {
            parser->error = GSH_NATIVE_PLAN_LIMIT;
            return false;
        }
        parser->nesting++;
        frame->condition = frame->evaluate && value != 0;
        frame->phase = 2U;
        return arithmetic_machine_push(machine, ARITHMETIC_FRAME_COMMA,
                                       frame->condition);
    }
    if (frame->phase == 2U) {
        frame->saved = value;
        if (!arithmetic_match(parser, ":")) return false;
        frame->phase = 3U;
        return arithmetic_machine_push(
            machine, ARITHMETIC_FRAME_CONDITIONAL,
            frame->evaluate && !frame->condition);
    }
    parser->nesting--;
    arithmetic_machine_complete(
        machine, frame->evaluate ? (frame->condition ? frame->saved : value)
                                 : 0);
    return true;
}

static bool arithmetic_step_assignment(arithmetic_machine *machine,
                                       arithmetic_parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->parser != NULL)) return false;
    arithmetic_parser *parser = machine->parser;
    long value;

    if (frame->phase == 0U) {
        size_t saved = parser->offset;

        if (!arithmetic_assignment_target(
                parser, &frame->assignment.name_begin,
                &frame->assignment.name_length,
                &frame->assignment.operator)) {
            parser->offset = saved;
            frame->phase = 2U;
            return arithmetic_machine_push(
                machine, ARITHMETIC_FRAME_CONDITIONAL, frame->evaluate);
        }
        if (parser->nesting == 32U) {
            parser->error = GSH_NATIVE_PLAN_LIMIT;
            return false;
        }
        if (frame->evaluate &&
            frame->assignment.operator != ARITHMETIC_ASSIGN_SET &&
            !arithmetic_environment_value(
                parser, parser->input + frame->assignment.name_begin,
                frame->assignment.name_length, true,
                &frame->assignment.left)) return false;
        parser->nesting++;
        frame->phase = 1U;
        return arithmetic_machine_push(machine, ARITHMETIC_FRAME_ASSIGNMENT,
                                       frame->evaluate);
    }
    if (!arithmetic_machine_take(machine, &value)) return false;
    if (frame->phase == 1U) {
        parser->nesting--;
        if (frame->evaluate && !arithmetic_commit_assignment(
                                   parser, &frame->assignment, value,
                                   &value)) return false;
    }
    arithmetic_machine_complete(machine, frame->evaluate ? value : 0);
    return true;
}

static bool arithmetic_step_comma(arithmetic_machine *machine,
                                  arithmetic_parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->parser != NULL)) return false;
    long value;

    if (frame->phase == 0U) {
        frame->phase = 1U;
        return arithmetic_machine_push(machine, ARITHMETIC_FRAME_ASSIGNMENT,
                                       frame->evaluate);
    }
    if (!arithmetic_machine_take(machine, &value)) return false;
    frame->value = value;
    if (arithmetic_match(machine->parser, ",")) {
        return arithmetic_machine_push(machine, ARITHMETIC_FRAME_ASSIGNMENT,
                                       frame->evaluate);
    }
    arithmetic_machine_complete(machine, frame->evaluate ? frame->value : 0);
    return true;
}

static bool arithmetic_machine_step(arithmetic_machine *machine)
{
    if (!require(machine != NULL && machine->depth > 0U &&
                 machine->depth <= ARITHMETIC_FRAME_CAP)) return false;
    arithmetic_parse_frame *frame = &machine->frames[machine->depth - 1U];

    if (frame->kind == ARITHMETIC_FRAME_COMMA) {
        return arithmetic_step_comma(machine, frame);
    }
    if (frame->kind == ARITHMETIC_FRAME_ASSIGNMENT) {
        return arithmetic_step_assignment(machine, frame);
    }
    if (frame->kind == ARITHMETIC_FRAME_CONDITIONAL) {
        return arithmetic_step_conditional(machine, frame);
    }
    if (frame->kind >= ARITHMETIC_FRAME_LOGICAL_OR &&
        frame->kind <= ARITHMETIC_FRAME_MULTIPLY) {
        return arithmetic_step_binary(machine, frame);
    }
    if (frame->kind == ARITHMETIC_FRAME_UNARY) {
        return arithmetic_step_unary(machine, frame);
    }
    return arithmetic_step_primary(machine, frame);
}

/* ── Arithmetic Precedence Is a Bounded Pushdown Machine ────────
 * Recursive precedence functions made a short expression consume an implicit
 * C stack and connected word expansion back into the arithmetic call cycle.
 * Frames now carry precedence, short-circuit state, and assignment ownership
 * explicitly.  The grammar keeps its 32-level semantic nesting limit while a
 * separate fixed transition ceiling makes malformed input deterministic.
 * ─────────────────────────────────────────────────────────────── */
static bool arithmetic_parse(arithmetic_parser *parser, bool evaluate,
                             long *value)
{
    static arithmetic_machine machine;
    size_t steps;

    if (!require(parser != NULL && parser->input != NULL && value != NULL)) {
        return false;
    }
    (void)memset(&machine, 0, sizeof(machine));
    machine.parser = parser;
    if (!arithmetic_machine_push(&machine, ARITHMETIC_FRAME_COMMA,
                                 evaluate)) {
        machine.parser = NULL;
        return false;
    }
    for (steps = 0U;
         steps < ARITHMETIC_MACHINE_STEP_CAP && machine.depth > 0U;
         steps++) {
        if (!arithmetic_machine_step(&machine)) {
            machine.parser = NULL;
            return false;
        }
    }
    if (machine.depth != 0U || !machine.returned) {
        parser->error = GSH_NATIVE_PLAN_LIMIT;
        machine.parser = NULL;
        return false;
    }
    *value = machine.value;
    machine.parser = NULL;
    return true;
}

static gsh_native_plan_status reserve_arithmetic_value(
    char *output, size_t capacity, size_t *used, long value)
{
    if (output == NULL || used == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    if (parser == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    const char *message = parser->diagnostic != NULL
                              ? parser->diagnostic
                              : "invalid expression";

    if (parser->context == NULL) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    return native_arithmetic_error(arithmetic_context(parser)->variable_state,
                                   message, strlen(message));
}

static bool arithmetic_expression_needs_expansion(
    const char *input, size_t begin, size_t end,
    const gsh_native_expansion_context *context, bool *deferred)
{
    bool needs_expansion = false;
    size_t cursor;

    if (!require(input != NULL && deferred != NULL)) return false;
    if (!require(begin <= end)) return false;
    for (cursor = begin; cursor < end; cursor++) {
        unsigned char byte = (unsigned char)input[cursor];

        if (byte == '$' || byte == 0x60U || byte == '\\' || byte == '\'' ||
            byte == '"') needs_expansion = true;
        if (!*deferred && (byte == '$' || byte == 0x60U) &&
            context != NULL && context->preflight &&
            context->deferred_work != NULL) {
            *context->deferred_work = true;
            *deferred = true;
        }
    }
    return needs_expansion;
}

static gsh_native_plan_status recover_arithmetic_failure(
    arithmetic_parser *parser, bool deferred, long *value)
{
    gsh_native_plan_status status;

    if (!require(parser != NULL && value != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(parser->error >= GSH_NATIVE_PLAN_OK)) {
        return GSH_NATIVE_PLAN_ERROR;
    }
    if (deferred && parser->error == GSH_NATIVE_PLAN_OK) {
        *value = 0;
        return GSH_NATIVE_PLAN_OK;
    }
    status = parser->error == GSH_NATIVE_PLAN_OK
                 ? arithmetic_diagnostic(parser)
                 : parser->error;
    if (status == GSH_NATIVE_PLAN_OK && parser->context != NULL &&
        arithmetic_context(parser)->preflight) {
        *value = 0;
        return GSH_NATIVE_PLAN_OK;
    }
    return status == GSH_NATIVE_PLAN_OK ? GSH_NATIVE_PLAN_ERROR : status;
}

static gsh_native_plan_status evaluate_arithmetic_expression(
    arithmetic_parser *parser, bool deferred, long *value)
{
    if (!require(parser != NULL && value != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(parser->offset <= parser->end)) {
        return GSH_NATIVE_PLAN_ERROR;
    }
    if (!arithmetic_parse(parser, true, value)) {
        return recover_arithmetic_failure(parser, deferred, value);
    }
    arithmetic_skip_space(parser);
    if (parser->offset == parser->end) return GSH_NATIVE_PLAN_OK;
    if (deferred) {
        *value = 0;
        return GSH_NATIVE_PLAN_OK;
    }
    return recover_arithmetic_failure(parser, false, value);
}

static gsh_native_plan_status expand_arithmetic(
    const char *input, size_t end, size_t *offset,
    const gsh_native_expansion_context *context, size_t depth,
    gsh_native_pipeline *pipeline, char *output, size_t output_capacity,
    size_t *output_used, expansion_machine *machine,
    expansion_word_frame *frame)
{
    if (offset == NULL || input == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (context == NULL || frame == NULL || machine == NULL || output == NULL || output_used == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    arithmetic_parser parser;
    size_t expression_end;
    size_t after;
    size_t expression_begin = pipeline->text_used;
    bool deferred = false;
    bool needs_expansion;
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
    needs_expansion = arithmetic_expression_needs_expansion(
        input, *offset + 2U, expression_end, context, &deferred);
    if (needs_expansion) {
        if (machine == NULL || frame == NULL) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        frame->arithmetic = (expansion_arithmetic_pending){
            .expression_begin = expression_begin,
            .after = after,
            .deferred = deferred,
        };
        frame->continuation = EXPANSION_CONTINUATION_ARITHMETIC;
        if (!request_expansion_word(
                machine, (gsh_word_ref){*offset + 2U, expression_end},
                GSH_EXPAND_ARITHMETIC, depth + 1U)) {
            frame->continuation = EXPANSION_CONTINUATION_NONE;
            return GSH_NATIVE_PLAN_LIMIT;
        }
        return GSH_NATIVE_PLAN_DEFERRED;
    }
    parser.input = input;
    parser.offset = *offset + 2U;
    parser.end = expression_end;
    parser.context = context;
    parser.nesting = 0;
    parser.error = GSH_NATIVE_PLAN_OK;
    parser.diagnostic = NULL;
    status = evaluate_arithmetic_expression(&parser, deferred, &value);
    if (status != GSH_NATIVE_PLAN_OK) {
        pipeline->text_used = expression_begin;
        return status;
    }
    pipeline->text_used = expression_begin;
    status = reserve_arithmetic_value(output, output_capacity, output_used,
                                      value);
    if (status == GSH_NATIVE_PLAN_OK) {
        *offset = after;
    }
    return status;
}

static gsh_native_plan_status expand_dollar_byte(
    expansion_machine *machine, expansion_word_frame *frame,
    bool quoted)
{
    const char *input;
    gsh_native_pipeline *pipeline;
    size_t value_begin;
    gsh_native_plan_status status;

    if (!require(machine != NULL && frame != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(machine->input != NULL && machine->pipeline != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    input = machine->input;
    pipeline = machine->pipeline;
    value_begin = pipeline->text_used;
    if (quoted && frame->offset < frame->word.end &&
        input[frame->offset] == '\'') {
        if (!reserve_byte(pipeline, '$')) return GSH_NATIVE_PLAN_LIMIT;
        mark_reserved(pipeline, value_begin, GSH_WORD_QUOTED);
        return GSH_NATIVE_PLAN_OK;
    }
    if (!quoted && frame->mode != GSH_EXPAND_ARITHMETIC &&
        frame->offset < frame->word.end && input[frame->offset] == '\'') {
        frame->offset++;
        if (frame->mode == GSH_EXPAND_FIELDS &&
            !reserve_empty_marker(pipeline)) return GSH_NATIVE_PLAN_LIMIT;
        value_begin = pipeline->text_used;
        status = expand_dollar_single_quote(
            input, frame->word.end, &frame->offset, pipeline);
        if (status == GSH_NATIVE_PLAN_OK) {
            mark_reserved(pipeline, value_begin, GSH_WORD_QUOTED);
        }
        return status;
    }
    if (frame->offset < frame->word.end && input[frame->offset] == '(') {
        status = frame->offset + 1U < frame->word.end &&
                         input[frame->offset + 1U] == '('
                     ? expand_arithmetic(
                           input, frame->word.end, &frame->offset,
                           machine->context, frame->logical_depth, pipeline,
                           pipeline->text, GSH_NATIVE_TEXT_CAP,
                           &pipeline->text_used, machine, frame)
                     : expand_command_substitution(
                           input, frame->word.end, &frame->offset,
                           machine->context, pipeline->text,
                           GSH_NATIVE_TEXT_CAP, &pipeline->text_used);
        if (status == GSH_NATIVE_PLAN_OK) {
            mark_reserved(pipeline, value_begin,
                          GSH_WORD_EXPANDED |
                              (quoted ? GSH_WORD_QUOTED : 0));
        }
        return status;
    }
    if (frame->offset == frame->word.end || input[frame->offset] == ' ' ||
        input[frame->offset] == '\t' || input[frame->offset] == '\n') {
        if (!reserve_byte(pipeline, '$')) return GSH_NATIVE_PLAN_LIMIT;
        if (quoted) mark_reserved(pipeline, value_begin, GSH_WORD_QUOTED);
        return GSH_NATIVE_PLAN_OK;
    }
    return expand_parameter(
        input, frame->word.end, &frame->offset,
        quoted || frame->mode == GSH_EXPAND_ARITHMETIC,
        frame->mode == GSH_EXPAND_ARITHMETIC ? GSH_EXPAND_SCALAR
                                             : frame->mode,
        frame->logical_depth, machine->context, pipeline, machine, frame);
}

static gsh_native_plan_status expand_backquote_byte(
    expansion_machine *machine, expansion_word_frame *frame,
    bool quoted)
{
    size_t value_begin;
    gsh_native_plan_status status;

    if (!require(machine != NULL && frame != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(machine->pipeline != NULL && machine->input != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    value_begin = expansion_pipeline(machine)->text_used;
    status = expand_backquote_substitution(
        machine->input, frame->word.end, &frame->offset,
        quoted || frame->mode == GSH_EXPAND_ARITHMETIC, machine->context,
        expansion_pipeline(machine)->text, GSH_NATIVE_TEXT_CAP,
        &expansion_pipeline(machine)->text_used);
    if (status == GSH_NATIVE_PLAN_OK) {
        mark_reserved(machine->pipeline, value_begin,
                      GSH_WORD_EXPANDED |
                          (quoted ? GSH_WORD_QUOTED : 0));
    }
    return status;
}

static gsh_native_plan_status expand_tilde_byte(
    expansion_machine *machine, expansion_word_frame *frame)
{
    bool found;
    const char *home;
    size_t value_begin;
    gsh_native_plan_status status;

    if (!require(machine != NULL && frame != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(machine->context != NULL && machine->pipeline != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (frame->offset < frame->word.end &&
        machine->input[frame->offset] != '/') {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    home = lookup_variable(machine->context, "HOME", 4, &found);
    if (home == NULL || !found) return GSH_NATIVE_PLAN_UNSUPPORTED;
    value_begin = expansion_pipeline(machine)->text_used;
    status = reserve_parameter_value(machine->pipeline, home);
    if (status != GSH_NATIVE_PLAN_OK) return status;
    mark_reserved(machine->pipeline, value_begin, GSH_WORD_QUOTED);
    if (frame->mode == GSH_EXPAND_FIELDS &&
        expansion_pipeline(machine)->text_used == value_begin &&
        !reserve_empty_marker(machine->pipeline)) return GSH_NATIVE_PLAN_LIMIT;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status expand_single_quoted_byte(
    gsh_native_pipeline *pipeline, expansion_word_frame *frame,
    unsigned char byte)
{
    if (!require(pipeline != NULL && frame != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(frame->quote == EXPANSION_QUOTE_SINGLE)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (byte == '\'') {
        frame->quote = EXPANSION_QUOTE_NONE;
        return GSH_NATIVE_PLAN_OK;
    }
    if (!reserve_byte(pipeline, byte)) return GSH_NATIVE_PLAN_LIMIT;
    pipeline->provenance[pipeline->text_used - 1U] = GSH_WORD_QUOTED;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status expand_double_quoted_byte(
    expansion_machine *machine, expansion_word_frame *frame,
    unsigned char byte)
{
    gsh_native_pipeline *pipeline;

    if (!require(machine != NULL && frame != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(machine->pipeline != NULL && machine->input != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    pipeline = machine->pipeline;
    if (byte == '"') {
        frame->quote = EXPANSION_QUOTE_NONE;
        return GSH_NATIVE_PLAN_OK;
    }
    if (byte == '$') return expand_dollar_byte(machine, frame, true);
    if (byte == 0x60U) return expand_backquote_byte(machine, frame, true);
    if (byte == '\\' && frame->offset < frame->word.end) {
        unsigned char next = (unsigned char)machine->input[frame->offset];

        if (next == '$' || next == 0x60U || next == '"' || next == '\\' ||
            next == '\n') {
            frame->offset++;
            if (next == '\n') return GSH_NATIVE_PLAN_OK;
            byte = next;
        }
    }
    if (!reserve_byte(pipeline, byte)) return GSH_NATIVE_PLAN_LIMIT;
    pipeline->provenance[pipeline->text_used - 1U] = GSH_WORD_QUOTED;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status expand_unquoted_byte(
    expansion_machine *machine, expansion_word_frame *frame,
    unsigned char byte)
{
    const char *input;
    gsh_native_pipeline *pipeline;

    if (!require(machine != NULL && frame != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(machine->input != NULL && machine->pipeline != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    input = machine->input;
    pipeline = machine->pipeline;
    if (byte == '\'' && frame->mode != GSH_EXPAND_ARITHMETIC) {
        frame->quote = EXPANSION_QUOTE_SINGLE;
        if (frame->mode == GSH_EXPAND_FIELDS &&
            !reserve_empty_marker(pipeline)) return GSH_NATIVE_PLAN_LIMIT;
        return GSH_NATIVE_PLAN_OK;
    }
    if (byte == '"' && frame->mode != GSH_EXPAND_ARITHMETIC) {
        frame->quote = EXPANSION_QUOTE_DOUBLE;
        if (frame->mode == GSH_EXPAND_FIELDS &&
            !reserve_empty_marker(pipeline)) return GSH_NATIVE_PLAN_LIMIT;
        return GSH_NATIVE_PLAN_OK;
    }
    if (byte == '\\') {
        if (frame->offset == frame->word.end) {
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        if (frame->mode != GSH_EXPAND_ARITHMETIC ||
            input[frame->offset] == '$' || input[frame->offset] == 0x60 ||
            input[frame->offset] == '\\' || input[frame->offset] == '\n') {
            byte = (unsigned char)input[frame->offset++];
        }
        if (byte == '\n') return GSH_NATIVE_PLAN_OK;
        if (!reserve_byte(pipeline, byte)) return GSH_NATIVE_PLAN_LIMIT;
        pipeline->provenance[pipeline->text_used - 1U] = GSH_WORD_QUOTED;
        return GSH_NATIVE_PLAN_OK;
    }
    if (byte == '$') return expand_dollar_byte(machine, frame, false);
    if (byte == '~' && frame->mode != GSH_EXPAND_ARITHMETIC &&
        frame->offset == frame->word.begin + 1U) {
        return expand_tilde_byte(machine, frame);
    }
    if (byte == 0x60U) return expand_backquote_byte(machine, frame, false);
    return reserve_byte(pipeline, byte) ? GSH_NATIVE_PLAN_OK
                                        : GSH_NATIVE_PLAN_LIMIT;
}

static gsh_native_plan_status expand_word_frame(
    expansion_machine *machine, expansion_word_frame *frame)
{
    if (!require(machine != NULL && frame != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(machine->input != NULL && machine->pipeline != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    while (frame->offset < frame->word.end) {
        unsigned char byte =
            (unsigned char)machine->input[frame->offset++];
        gsh_native_plan_status status;

        if (frame->quote == EXPANSION_QUOTE_SINGLE) {
            status = expand_single_quoted_byte(
                machine->pipeline, frame, byte);
        } else if (frame->quote == EXPANSION_QUOTE_DOUBLE) {
            status = expand_double_quoted_byte(machine, frame, byte);
        } else status = expand_unquoted_byte(machine, frame, byte);
        if (status != GSH_NATIVE_PLAN_OK) return status;
    }
    if (frame->quote != EXPANSION_QUOTE_NONE) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    frame->complete = true;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status resume_parameter_expansion(
    expansion_machine *machine, expansion_word_frame *frame)
{
    if (frame == NULL || machine == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    expansion_parameter_pending *pending = &frame->parameter;
    gsh_native_pipeline *pipeline = machine->pipeline;
    char *expanded = pipeline->text + machine->child_begin;
    gsh_native_plan_status status;

    if (!require(machine->child_ready && pending->word_begin <=
                                           pending->closing)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(machine->child_begin <= pipeline->text_used)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (frame->continuation == EXPANSION_CONTINUATION_PATTERN) {
        gsh_word_ref word = {pending->word_begin, pending->closing};

        status = remove_parameter_pattern(
            machine->input, word, machine->context, pipeline, expanded,
            machine->child_length, pending->value_begin,
            pending->parameter_value, pending->pattern_operation);
    } else {
        status = commit_parameter_operator(
            machine->input, pending->cursor, pending->parameter_end,
            pending->colon, pending->operator, pending->word_begin,
            pending->closing, machine->context, expanded,
            machine->child_length, pending->value_begin, pipeline);
    }
    if (status != GSH_NATIVE_PLAN_OK) return status;
    mark_expansion(pipeline, pending->value_begin, pending->quoted);
    frame->offset = pending->closing + 1U;
    frame->continuation = EXPANSION_CONTINUATION_NONE;
    machine->child_ready = false;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status resume_arithmetic_expansion(
    expansion_machine *machine, expansion_word_frame *frame)
{
    if (frame == NULL || machine == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    expansion_arithmetic_pending *pending = &frame->arithmetic;
    gsh_native_pipeline *pipeline = machine->pipeline;
    arithmetic_parser parser;
    long value = 0;
    gsh_native_plan_status status;

    if (!require(machine->child_ready && machine->child_begin ==
                                           pending->expression_begin)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(machine->child_length < GSH_NATIVE_TEXT_CAP)) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    parser.input = pipeline->text + machine->child_begin;
    parser.offset = 0;
    parser.end = machine->child_length;
    parser.context = machine->context;
    parser.nesting = 0;
    parser.error = GSH_NATIVE_PLAN_OK;
    parser.diagnostic = NULL;
    status = evaluate_arithmetic_expression(
        &parser, pending->deferred, &value);
    pipeline->text_used = pending->expression_begin;
    if (status != GSH_NATIVE_PLAN_OK) return status;
    status = reserve_arithmetic_value(
        pipeline->text, GSH_NATIVE_TEXT_CAP, &pipeline->text_used, value);
    if (status != GSH_NATIVE_PLAN_OK) return status;
    mark_reserved(pipeline, pending->expression_begin,
                  GSH_WORD_EXPANDED |
                      (frame->quote == EXPANSION_QUOTE_DOUBLE
                           ? GSH_WORD_QUOTED
                           : 0));
    frame->offset = pending->after;
    frame->continuation = EXPANSION_CONTINUATION_NONE;
    machine->child_ready = false;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status resume_expansion_frame(
    expansion_machine *machine, expansion_word_frame *frame)
{
    if (!require(machine != NULL && frame != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(frame->continuation != EXPANSION_CONTINUATION_NONE)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (frame->continuation == EXPANSION_CONTINUATION_ARITHMETIC) {
        return resume_arithmetic_expansion(machine, frame);
    }
    return resume_parameter_expansion(machine, frame);
}

static gsh_native_plan_status push_expansion_frame(
    expansion_machine *machine)
{
    expansion_word_frame *frame;

    if (!require(machine != NULL && machine->push_requested)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(machine->pipeline != NULL &&
                 machine->requested_word.begin <=
                     machine->requested_word.end)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (machine->depth == EXPANSION_FRAME_CAP ||
        expansion_pipeline(machine)->text_used == GSH_NATIVE_TEXT_CAP) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    frame = &machine->frames[machine->depth++];
    (void)memset(frame, 0, sizeof(*frame));
    frame->word = machine->requested_word;
    frame->mode = machine->requested_mode;
    frame->logical_depth = machine->requested_depth;
    frame->offset = frame->word.begin;
    frame->expanded_begin = expansion_pipeline(machine)->text_used;
    frame->quote = EXPANSION_QUOTE_NONE;
    frame->continuation = EXPANSION_CONTINUATION_NONE;
    machine->push_requested = false;
    machine->child_ready = false;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status complete_expansion_frame(
    expansion_machine *machine)
{
    expansion_word_frame *frame;

    if (!require(machine != NULL && machine->depth > 0)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(machine->pipeline != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    frame = &machine->frames[machine->depth - 1U];
    machine->child_begin = frame->expanded_begin;
    machine->child_length = expansion_pipeline(machine)->text_used -
                            frame->expanded_begin;
    if (!reserve_byte(machine->pipeline, '\0')) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    machine->depth--;
    machine->child_ready = true;
    return GSH_NATIVE_PLAN_OK;
}

static void cleanup_expansion_failure(expansion_machine *machine)
{
    size_t index;

    if (!require(machine != NULL && machine->pipeline != NULL)) return;
    if (!require(machine->depth <= EXPANSION_FRAME_CAP)) return;
    for (index = machine->depth; index > 0; index--) {
        expansion_word_frame *frame = &machine->frames[index - 1U];

        if (frame->continuation == EXPANSION_CONTINUATION_ARITHMETIC) {
            expansion_pipeline(machine)->text_used =
                frame->arithmetic.expression_begin;
            break;
        }
    }
}

static gsh_native_plan_status
expand_static_word(const char *input, gsh_word_ref word,
                   const gsh_native_expansion_context *context,
                   gsh_expansion_mode mode, size_t depth,
                   gsh_native_pipeline *pipeline, char **expanded,
                   size_t *expanded_length)
{
    if (context == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    expansion_machine machine;
    gsh_native_plan_status status;
    size_t steps;

    if (!require(input != NULL && pipeline != NULL && expanded != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(word.begin <= word.end && depth <= 32U)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    (void)memset(&machine, 0, sizeof(machine));
    machine.input = input;
    machine.context = context;
    machine.pipeline = pipeline;
    if (!request_expansion_word(&machine, word, mode, depth)) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    for (steps = 0; steps < EXPANSION_MACHINE_STEP_CAP; steps++) {
        expansion_word_frame *frame;

        if (machine.push_requested) {
            status = push_expansion_frame(&machine);
            if (status != GSH_NATIVE_PLAN_OK) break;
            continue;
        }
        if (machine.depth == 0) {
            *expanded = pipeline->text + machine.child_begin;
            if (expanded_length != NULL) {
                *expanded_length = machine.child_length;
            }
            return GSH_NATIVE_PLAN_OK;
        }
        frame = &machine.frames[machine.depth - 1U];
        status = frame->continuation != EXPANSION_CONTINUATION_NONE
                     ? resume_expansion_frame(&machine, frame)
                     : expand_word_frame(&machine, frame);
        if (status == GSH_NATIVE_PLAN_DEFERRED && machine.push_requested) {
            continue;
        }
        if (status != GSH_NATIVE_PLAN_OK) break;
        if (frame->complete) {
            status = complete_expansion_frame(&machine);
            if (status != GSH_NATIVE_PLAN_OK) break;
        }
    }
    if (steps == EXPANSION_MACHINE_STEP_CAP) status = GSH_NATIVE_PLAN_LIMIT;
    cleanup_expansion_failure(&machine);
    return status;
}

static gsh_native_plan_status expand_heredoc_arithmetic(
    const char *input, size_t end, size_t *offset,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline)
{
    if (context == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    arithmetic_parser parser;
    size_t expression_end;
    size_t after;
    size_t expression_begin = pipeline->text_used;
    char *expression;
    size_t expression_length;
    bool deferred = false;
    bool needs_expansion;
    long value = 0;
    gsh_native_plan_status status;

    if (!require(input != NULL && offset != NULL && pipeline != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(*offset + 1U < end && input[*offset] == '(' &&
                 input[*offset + 1U] == '(')) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    status = find_substitution_end(input, end, *offset, true,
                                   &expression_end, &after);
    if (status != GSH_NATIVE_PLAN_OK) return status;
    needs_expansion = arithmetic_expression_needs_expansion(
        input, *offset + 2U, expression_end, context, &deferred);
    if (needs_expansion) {
        status = expand_static_word(
            input, (gsh_word_ref){*offset + 2U, expression_end}, context,
            GSH_EXPAND_ARITHMETIC, 1U, pipeline, &expression,
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
    status = evaluate_arithmetic_expression(&parser, deferred, &value);
    pipeline->text_used = expression_begin;
    if (status != GSH_NATIVE_PLAN_OK) return status;
    status = reserve_arithmetic_value(
        pipeline->heredoc_text, GSH_NATIVE_HEREDOC_TEXT_CAP,
        &pipeline->heredoc_text_used, value);
    if (status == GSH_NATIVE_PLAN_OK) *offset = after;
    return status;
}

gsh_native_plan_status gsh_native_expand_scalar(
    const char *input, gsh_word_ref word,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *scratch, char **expanded)
{
    if (context == NULL || expanded == NULL || input == NULL || scratch == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    if (text == NULL) return false;
    if (descriptor == NULL) {
        return false;
    }
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
    if (pipeline == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (pipeline->heredoc_text_used == GSH_NATIVE_HEREDOC_TEXT_CAP) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    pipeline->heredoc_text[pipeline->heredoc_text_used++] = (char)byte;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status
reserve_heredoc_value(gsh_native_pipeline *pipeline, const char *value)
{
    if (pipeline == NULL || value == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    if (context == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    if (input == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (offset == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    if (pipeline == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (context == NULL || heredoc_index == NULL || input == NULL || source == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
                             ? expand_heredoc_arithmetic(
                                   input, source->body.end, &offset, context,
                                   pipeline)
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
    if (context == NULL || input == NULL || pipeline == NULL || source == NULL || target == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    gsh_native_plan_status status;

    (void)memset(target, 0, sizeof(*target));
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
    if (pipeline == NULL || input == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (assignment == NULL || context == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    if (input == NULL) return 0U;
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
    if (ifs == NULL) {
        return false;
    }
    return strchr(ifs, (int)byte) != NULL;
}

typedef struct {
    char *text;
    size_t provenance_offset;
    size_t length;
    bool has_pattern;
} gsh_expanded_field;

enum {
    GSH_PATHNAME_CANDIDATE_CAP = GSH_NATIVE_ARGUMENT_CAP,
    GSH_PATHNAME_DIRECTORY_ENTRY_CAP = 65536,
};

typedef struct {
    char current[GSH_PATHNAME_CANDIDATE_CAP][PATH_MAX];
    char next[GSH_PATHNAME_CANDIDATE_CAP][PATH_MAX];
    bool busy;
} gsh_pathname_workspace;

static gsh_pathname_workspace *acquire_pathname_workspace(void)
{
    static gsh_pathname_workspace workspace;

    if (workspace.busy) {
        return NULL;
    }
    workspace.busy = true;
    return &workspace;
}

static void release_pathname_workspace(gsh_pathname_workspace *workspace)
{
    if (workspace == NULL) {
        return;
    }
    (void)memset(workspace->current, 0, sizeof(workspace->current));
    (void)memset(workspace->next, 0, sizeof(workspace->next));
    workspace->busy = false;
}

static bool split_delimiter(const gsh_native_pipeline *pipeline,
                            size_t offset, const char *ifs)
{
    if (ifs == NULL || pipeline == NULL) {
        return false;
    }
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
    if (field_count == NULL || write == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (fields == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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

static bool consume_ifs_delimiters(
    const gsh_native_pipeline *pipeline, size_t end, const char *ifs,
    size_t *read, unsigned char first)
{
    bool nonwhite_seen = !ifs_white_space(first);

    if (!require(pipeline != NULL && ifs != NULL && read != NULL)) {
        return nonwhite_seen;
    }
    if (!require(*read <= end && end <= pipeline->text_used)) {
        return nonwhite_seen;
    }
    if (ifs_white_space(first)) {
        while (*read < end && split_delimiter(pipeline, *read, ifs) &&
               ifs_white_space((unsigned char)pipeline->text[*read])) {
            (*read)++;
        }
        if (*read < end && split_delimiter(pipeline, *read, ifs) &&
            !ifs_white_space((unsigned char)pipeline->text[*read])) {
            nonwhite_seen = true;
            (*read)++;
        }
    }
    while (*read < end && split_delimiter(pipeline, *read, ifs) &&
           ifs_white_space((unsigned char)pipeline->text[*read])) {
        (*read)++;
    }
    return nonwhite_seen;
}

static gsh_native_plan_status expansion_field_separator(
    const gsh_native_expansion_context *context, const char **separator)
{
    bool found = false;

    if (!require(separator != NULL)) return GSH_NATIVE_PLAN_LIMIT;
    *separator = " \t\n";
    if (context == NULL) return GSH_NATIVE_PLAN_OK;
    *separator = lookup_variable(context, "IFS", 3, &found);
    if (*separator == NULL) return GSH_NATIVE_PLAN_UNSUPPORTED;
    if (!found) *separator = " \t\n";
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status split_expanded_word(
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline, size_t begin, size_t length,
    gsh_expanded_field fields[GSH_NATIVE_ARGUMENT_CAP],
    size_t *field_count)
{
    if (field_count == NULL || fields == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    const char *ifs;
    gsh_native_plan_status separator_status;
    size_t read = begin;
    size_t end = begin + length;
    size_t write = begin;
    size_t field_begin = begin;
    bool candidate_has_bytes = false;
    bool candidate_has_quote = false;
    bool has_pattern = false;

    *field_count = 0;
    if ((separator_status = expansion_field_separator(context, &ifs)) !=
        GSH_NATIVE_PLAN_OK) return separator_status;
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
            bool nonwhite_seen;

            read++;
            nonwhite_seen = consume_ifs_delimiters(
                pipeline, end, ifs, &read, byte);
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

static bool pathname_component_has_magic(const char *pattern,
                                         size_t length)
{
    if (pattern == NULL) return false;
    size_t index;

    for (index = 0; index < length; index++) {
        if (pattern[index] == '\\' && index + 1U < length) {
            index++;
        } else if (pattern[index] == '*' || pattern[index] == '?' ||
                   pattern[index] == '[') {
            return true;
        }
    }
    return false;
}

static bool pathname_literal_component(const char *pattern, size_t length,
                                       char output[PATH_MAX])
{
    if (pattern == NULL) return false;
    if (output == NULL) {
        return false;
    }
    size_t read;
    size_t write = 0;

    for (read = 0; read < length; read++) {
        if (pattern[read] == '\\' && read + 1U < length) {
            read++;
        }
        if (write == PATH_MAX - 1U) {
            return false;
        }
        output[write++] = pattern[read];
    }
    output[write] = '\0';
    return true;
}

static bool pathname_join(char output[PATH_MAX], const char *directory,
                          const char *name)
{
    if (directory == NULL || name == NULL || output == NULL) {
        return false;
    }
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    bool separator = directory_length != 0U &&
                     directory[directory_length - 1U] != '/';
    size_t total = directory_length + (separator ? 1U : 0U) + name_length;

    if (total >= PATH_MAX) {
        return false;
    }
    (void)memcpy(output, directory, directory_length);
    if (separator) {
        output[directory_length++] = '/';
    }
    (void)memcpy(output + directory_length, name, name_length + 1U);
    return true;
}

static bool pathname_is_directory(const char *path)
{
    if (path == NULL) {
        return false;
    }
    struct stat status;

    return stat(path[0] == '\0' ? "." : path, &status) == 0 &&
           S_ISDIR(status.st_mode);
}

static bool pathname_exists(const char *path)
{
    if (path == NULL) {
        return false;
    }
    struct stat status;

    return lstat(path, &status) == 0;
}

static gsh_native_plan_status append_literal_component(
    gsh_pathname_workspace *workspace, size_t current_count,
    const char *component, size_t component_length, bool final,
    size_t *next_count)
{
    if (next_count == NULL || workspace == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    char literal[PATH_MAX];
    size_t index;

    if (!pathname_literal_component(component, component_length, literal)) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    *next_count = 0;
    for (index = 0; index < current_count; index++) {
        char *destination = workspace->next[*next_count];

        if (!pathname_join(destination, workspace->current[index], literal)) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        if ((!final && !pathname_is_directory(destination)) ||
            (final && !pathname_exists(destination))) {
            continue;
        }
        (*next_count)++;
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status append_pattern_component(
    gsh_pathname_workspace *workspace, size_t current_count,
    const char *component, size_t component_length, bool final,
    size_t *next_count)
{
    if (next_count == NULL || workspace == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    char matcher[PATH_MAX];
    size_t candidate;

    if (component == NULL || component_length >= sizeof(matcher)) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    (void)memcpy(matcher, component, component_length);
    matcher[component_length] = '\0';
    *next_count = 0;
    for (candidate = 0; candidate < current_count; candidate++) {
        const char *base = workspace->current[candidate];
        DIR *directory = opendir(base[0] == '\0' ? "." : base);
        size_t entries;

        if (directory == NULL) {
            continue;
        }
        errno = 0;
        for (entries = 0; entries < GSH_PATHNAME_DIRECTORY_ENTRY_CAP;
             entries++) {
            struct dirent *entry = readdir(directory);
            char *destination;

            if (entry == NULL) {
                break;
            }
            /* ── libc Owns Locale-Aware Component Semantics ──
             * Keep traversal and capacity under gsh control, but delegate the
             * POSIX bracket, escaping, and multibyte rules to fnmatch(3).
             * FNM_PERIOD preserves the rule that wildcards do not implicitly
             * expose a leading dot in each traversed pathname component.
             */
            if (fnmatch(matcher, entry->d_name, FNM_PERIOD) != 0) {
                continue;
            }
            if (*next_count == GSH_PATHNAME_CANDIDATE_CAP) {
                (void)closedir(directory);
                return GSH_NATIVE_PLAN_LIMIT;
            }
            destination = workspace->next[*next_count];
            if (!pathname_join(destination, base, entry->d_name)) {
                (void)closedir(directory);
                return GSH_NATIVE_PLAN_LIMIT;
            }
            if (!final && !pathname_is_directory(destination)) {
                continue;
            }
            (*next_count)++;
        }
        if (entries == GSH_PATHNAME_DIRECTORY_ENTRY_CAP) {
            (void)closedir(directory);
            return GSH_NATIVE_PLAN_LIMIT;
        }
        if (errno != 0) {
            int saved_errno = errno;

            (void)closedir(directory);
            errno = saved_errno;
            return GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        (void)closedir(directory);
    }
    return GSH_NATIVE_PLAN_OK;
}

static void rotate_pathname_candidates(gsh_pathname_workspace *workspace,
                                       size_t count)
{
    if (workspace == NULL) {
        return;
    }
    size_t index;

    for (index = 0; index < count; index++) {
        (void)memcpy(workspace->current[index], workspace->next[index], PATH_MAX);
    }
}

static void sort_pathname_candidates(gsh_pathname_workspace *workspace,
                                     size_t count)
{
    if (workspace == NULL) {
        return;
    }
    size_t unsorted;

    for (unsorted = 1; unsorted < count; unsorted++) {
        char value[PATH_MAX];
        size_t insertion = unsorted;

        (void)memcpy(value, workspace->current[unsorted], sizeof(value));
        while (insertion > 0U &&
               strcoll(workspace->current[insertion - 1U], value) > 0) {
            (void)memcpy(workspace->current[insertion],
                   workspace->current[insertion - 1U], PATH_MAX);
            insertion--;
        }
        (void)memcpy(workspace->current[insertion], value, sizeof(value));
    }
}

static gsh_native_plan_status enumerate_pathname_pattern(
    gsh_pathname_workspace *workspace, const char *pattern,
    size_t pattern_length, size_t *match_count)
{
    if (match_count == NULL || pattern == NULL || workspace == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    size_t offset = pattern[0] == '/' ? 1U : 0U;
    size_t current_count = 1U;
    bool trailing_slash = pattern_length > 1U &&
                          pattern[pattern_length - 1U] == '/';

    workspace->current[0][0] = pattern[0] == '/' ? '/' : '\0';
    workspace->current[0][pattern[0] == '/' ? 1U : 0U] = '\0';
    while (offset < pattern_length) {
        size_t component_begin;
        size_t component_length;
        size_t next_count;
        bool final;
        gsh_native_plan_status status;

        while (offset < pattern_length && pattern[offset] == '/') {
            offset++;
        }
        if (offset == pattern_length) {
            break;
        }
        component_begin = offset;
        while (offset < pattern_length && pattern[offset] != '/') {
            offset++;
        }
        component_length = offset - component_begin;
        final = offset == pattern_length ||
                (trailing_slash && offset + 1U == pattern_length);
        status = pathname_component_has_magic(
                     pattern + component_begin, component_length)
                     ? append_pattern_component(
                           workspace, current_count,
                           pattern + component_begin, component_length,
                           final, &next_count)
                     : append_literal_component(
                           workspace, current_count,
                           pattern + component_begin, component_length,
                           final, &next_count);
        if (status != GSH_NATIVE_PLAN_OK || next_count == 0U) {
            *match_count = 0;
            return status;
        }
        rotate_pathname_candidates(workspace, next_count);
        current_count = next_count;
    }
    if (trailing_slash) {
        size_t index;

        for (index = 0; index < current_count; index++) {
            size_t length = strlen(workspace->current[index]);

            if (!pathname_is_directory(workspace->current[index]) ||
                length == PATH_MAX - 1U) {
                *match_count = 0;
                return length == PATH_MAX - 1U
                           ? GSH_NATIVE_PLAN_LIMIT
                           : GSH_NATIVE_PLAN_OK;
            }
            workspace->current[index][length] = '/';
            workspace->current[index][length + 1U] = '\0';
        }
    }
    sort_pathname_candidates(workspace, current_count);
    *match_count = current_count;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status expand_pathname_field(
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline, const gsh_expanded_field *field,
    char *expanded[GSH_NATIVE_ARGUMENT_CAP], size_t *expanded_count)
{
    if (field == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (expanded == NULL || expanded_count == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    char pattern[GSH_NATIVE_TEXT_CAP];
    gsh_pathname_workspace *workspace;
    gsh_native_plan_status status;
    size_t index;
    size_t match_count;
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
    workspace = acquire_pathname_workspace();
    if (workspace == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    status = enumerate_pathname_pattern(
        workspace, pattern, pattern_used, &match_count);
    if (status != GSH_NATIVE_PLAN_OK) {
        release_pathname_workspace(workspace);
        return status;
    }
    if (match_count == 0U) {
        release_pathname_workspace(workspace);
        expanded[(*expanded_count)++] = field->text;
        return GSH_NATIVE_PLAN_OK;
    }
    for (index = 0; index < match_count; index++) {
        gsh_native_plan_status reserve_status;

        expanded[index] = pipeline->text + pipeline->text_used;
        reserve_status = reserve_parameter_value(
            pipeline, workspace->current[index]);
        if (reserve_status == GSH_NATIVE_PLAN_OK &&
            !reserve_byte(pipeline, '\0')) {
            reserve_status = GSH_NATIVE_PLAN_LIMIT;
        }
        if (reserve_status != GSH_NATIVE_PLAN_OK) {
            release_pathname_workspace(workspace);
            return reserve_status;
        }
    }
    *expanded_count = match_count;
    release_pathname_workspace(workspace);
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status append_expanded_word(
    const char *input, gsh_word_ref word,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline,
    char *expanded[GSH_NATIVE_ARGUMENT_CAP], size_t *expanded_count)
{
    if (expanded_count == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (context == NULL || expanded == NULL || input == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    if (context == NULL || expanded == NULL || expanded_count == NULL || input == NULL || scratch == NULL || words == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    if (command == NULL) {
        return false;
    }
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
    if (node == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (command == NULL || context == NULL || input == NULL || pipeline == NULL || storage == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    (void)memset(command, 0, sizeof(*command));
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
    if (storage == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (context == NULL || input == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
    (void)memset(command, 0, sizeof(*command));
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
    if (storage == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (input == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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

        if (context != NULL && context->variable_state != NULL) {
            status = native_variable_command_begin(
                context->variable_state, pipeline->command_count,
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
            (void)memset(&pipeline->commands[pipeline->command_count], 0,
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
    if (storage == NULL) return GSH_NATIVE_PLAN_ERROR;
    if (context == NULL || input == NULL || pipeline == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
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
