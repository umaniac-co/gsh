#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh call graph requires the POSIX.1-2024 baseline"
#endif

#include "canon_call_graph.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define require(condition) (condition)

typedef enum {
    GRAPH_CODE,
    GRAPH_LINE_COMMENT,
    GRAPH_BLOCK_COMMENT,
    GRAPH_STRING,
    GRAPH_CHARACTER,
    GRAPH_PREPROCESSOR,
} graph_lexical_state;

typedef enum {
    GRAPH_GUARD_NONE,
    GRAPH_GUARD_IF_OPEN,
    GRAPH_GUARD_CONDITION,
    GRAPH_GUARD_BODY,
    GRAPH_GUARD_BRACED_BODY,
    GRAPH_GUARD_RETURN,
} graph_guard_state;

typedef struct {
    graph_lexical_state state;
    char identifier[GSH_CANON_NAME_CAP];
    char top_identifier[GSH_CANON_NAME_CAP];
    char function_name[GSH_CANON_NAME_CAP];
    char call_name[GSH_CANON_NAME_CAP];
    char declaration_return_type[GSH_CANON_NAME_CAP];
    char parameter_candidate[GSH_CANON_NAME_CAP];
    char parameters[GSH_CANON_PARAMETER_CAP][GSH_CANON_NAME_CAP];
    size_t current_function;
    size_t line;
    size_t call_line;
    size_t brace_depth;
    size_t header_parentheses;
    size_t guard_parentheses;
    size_t guard_condition_terms;
    size_t statement_parentheses;
    size_t unevaluated_close_depth;
    size_t control_close_depth;
    size_t ternary_depth;
    size_t bare_call_index;
    size_t bare_call_depth;
    size_t parameter_count;
    size_t pending_parameter;
    graph_guard_state guard_state;
    unsigned char guard_previous;
    bool guard_condition_has_assertion;
    bool guard_condition_has_token;
    bool guard_condition_side_effect_free;
    bool declaration_internal;
    bool declaration_void;
    bool declaration_pointer;
    bool header_active;
    bool header_closed;
    bool call_pending;
    bool call_bare_pending;
    bool bare_call_closed;
    bool parameter_pointer[GSH_CANON_PARAMETER_CAP];
    bool parameter_validated[GSH_CANON_PARAMETER_CAP];
    bool parameter_reported[GSH_CANON_PARAMETER_CAP];
    bool parameter_candidate_pointer;
    bool parameter_array;
    bool previous_token_is_punctuation;
    bool statement_start;
    bool control_pending;
    bool escaped;
    bool line_prefix;
} graph_parser;

static void graph_reset_guard(graph_parser *parser)
{
    if (!require(parser != NULL)) return;
    if (!require(parser->guard_state <= GRAPH_GUARD_RETURN)) return;
    parser->guard_state = GRAPH_GUARD_NONE;
    parser->guard_parentheses = 0U;
    parser->guard_condition_terms = 0U;
    parser->guard_previous = 0U;
    parser->guard_condition_has_assertion = false;
    parser->guard_condition_has_token = false;
    parser->guard_condition_side_effect_free = false;
}

static bool graph_pure_condition_operator(const graph_parser *parser,
                                          unsigned char byte,
                                          unsigned char next)
{
    if (!require(parser != NULL)) return false;
    if (byte == ',' || byte == ';') return false;
    if (byte == '+' && (next == '+' || parser->guard_previous == '+')) {
        return false;
    }
    if (byte == '-' && (next == '-' || parser->guard_previous == '-')) {
        return false;
    }
    if (byte != '=') return true;
    return next == '=' || parser->guard_previous == '=' ||
           parser->guard_previous == '!' || parser->guard_previous == '<' ||
           parser->guard_previous == '>';
}

static bool graph_pure_condition_call(const char *name)
{
    static const char *const observers[] = {
        "S_ISBLK", "S_ISCHR", "S_ISDIR", "S_ISFIFO", "S_ISLNK",
        "S_ISREG", "S_ISSOCK", "WEXITSTATUS", "WIFCONTINUED",
        "WIFEXITED", "WIFSIGNALED", "WIFSTOPPED", "WSTOPSIG",
        "WTERMSIG", "_Alignof", "isalnum", "isalpha",
        "isdigit", "isspace", "memchr", "memcmp", "require", "sizeof",
        "strchr", "strcmp", "strlen", "strncmp", "strnlen", "strrchr",
        "strstr", "arithmetic_context", "arithmetic_machine_parser",
        "arithmetic_match", "capture_contains", "cell_index_valid",
        "cell_terminal", "copy_destination", "copy_source", "current_token",
        "current_word_is", "graph_call_resolves", "gsh_alias_name_is_valid",
        "gsh_aliases_count", "gsh_background_has_capacity",
        "gsh_builtin_io_valid", "gsh_command_cache_count",
        "gsh_functions_lookup", "gsh_functions_snapshot_header_valid",
        "gsh_options_enabled", "gsh_positionals_count",
        "gsh_source_workspaces_depth", "gsh_traps_condition_count",
        "gsh_variable_name_is_valid", "gsh_variables_is_set",
        "evaluator_functions", "evaluator_pipeline",
        "evaluator_pipeline_scope", "evaluator_source_request_command",
        "evaluator_source_workspaces", "evaluator_storage",
        "expansion_pipeline", "is_redirect_operator", "is_unset",
        "main_transaction_state", "name_start",
        "noninteractive_context_pipeline", "output", "parser_storage",
        "process_child_count", "reactor_word_is", "source_frame_workspace",
        "starts_with", "state_alias_commit", "state_async_repl",
        "state_history", "state_native_pipeline", "state_parse_storage",
        "state_source_workspaces", "state_variable_commit",
        "state_variables", "token_at", "trap_execution_workspace",
    };
    size_t index;

    if (!require(name != NULL)) return false;
    if (!require(name[0] != '\0')) return false;
    for (index = 0U; index < sizeof(observers) / sizeof(observers[0]);
         index++) {
        if (strcmp(name, observers[index]) == 0) return true;
    }
    return false;
}

static bool graph_identifier_start(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') || byte == '_';
}

static bool graph_identifier_continue(unsigned char byte)
{
    return graph_identifier_start(byte) || (byte >= '0' && byte <= '9');
}

static bool graph_call_keyword(const char *name)
{
    static const char *const keywords[] = {
        "_Alignof", "_Generic", "_Static_assert", "case", "do", "for",
        "if", "return", "sizeof", "switch", "while",
    };
    size_t index;

    if (!require(name != NULL)) return false;
    if (!require(name[0] != '\0')) return false;
    for (index = 0; index < sizeof(keywords) / sizeof(keywords[0]); index++) {
        if (strcmp(name, keywords[index]) == 0) {
            return true;
        }
    }
    return false;
}

static bool graph_behavior_keyword(const char *name)
{
    static const char *const keywords[] = {
        "case", "default", "do", "else", "for", "if", "switch", "while",
    };
    size_t index;

    if (!require(name != NULL)) return false;
    if (!require(name[0] != '\0')) return false;
    for (index = 0U; index < sizeof(keywords) / sizeof(keywords[0]); index++) {
        if (strcmp(name, keywords[index]) == 0) return true;
    }
    return false;
}

static bool graph_guard_control_keyword(const char *name)
{
    static const char *const keywords[] = {
        "break", "case", "continue", "default", "do", "else", "for",
        "goto", "if", "switch", "while",
    };
    size_t index;

    if (!require(name != NULL)) return false;
    if (!require(name[0] != '\0')) return false;
    for (index = 0U; index < sizeof(keywords) / sizeof(keywords[0]); index++) {
        if (strcmp(name, keywords[index]) == 0) return true;
    }
    return false;
}

static int graph_increment(unsigned short *value)
{
    if (!require(value != NULL)) return -1;
    if (*value == USHRT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    (*value)++;
    return 0;
}

static int graph_increment_by(unsigned short *value, size_t amount)
{
    if (value == NULL || amount > (size_t)USHRT_MAX - *value) {
        errno = EOVERFLOW;
        return -1;
    }
    *value = (unsigned short)(*value + amount);
    return 0;
}

static int graph_copy_name(char destination[GSH_CANON_NAME_CAP],
                           const unsigned char *source, size_t length)
{
    if (destination == NULL || source == NULL || length == 0U ||
        length >= GSH_CANON_NAME_CAP) {
        errno = length >= GSH_CANON_NAME_CAP ? ENAMETOOLONG : EINVAL;
        return -1;
    }
    (void)memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}

static int graph_file_index(gsh_canon_call_graph *graph, const char *path,
                            size_t *file_index)
{
    if (file_index == NULL || graph == NULL) {
        return -1;
    }
    size_t path_length;
    size_t index;

    for (index = 0; index < graph->file_count; index++) {
        if (strcmp(graph->files[index], path) == 0) {
            *file_index = index;
            return 0;
        }
    }
    path_length = strnlen(path, GSH_CANON_PATH_CAP);
    if (path_length == GSH_CANON_PATH_CAP ||
        graph->file_count >= GSH_CANON_FILE_CAP) {
        errno = path_length == GSH_CANON_PATH_CAP ? ENAMETOOLONG : ENOSPC;
        return -1;
    }
    (void)memcpy(graph->files[graph->file_count], path, path_length + 1U);
    *file_index = graph->file_count;
    graph->file_count++;
    return 0;
}

static bool graph_root_name(const char *name)
{
    if (!require(name != NULL)) return false;
    if (!require(name[0] != '\0')) return false;
    return strcmp(name, "main") == 0 ||
           strcmp(name, "LLVMFuzzerTestOneInput") == 0 ||
           strcmp(name, "require_at") == 0 ||
           strcmp(name, "signal_handler") == 0 ||
           strcmp(name, "trap_signal_handler") == 0;
}

static int graph_add_function(gsh_canon_call_graph *graph,
                              graph_parser *parser, size_t file_index,
                              size_t body_offset)
{
    if (graph == NULL) return -1;
    if (parser == NULL) {
        return -1;
    }
    gsh_canon_function *function;

    if (graph->function_count >= GSH_CANON_FUNCTION_CAP ||
        file_index > UINT16_MAX) {
        errno = ENOSPC;
        return -1;
    }
    function = &graph->functions[graph->function_count];
    (void)memcpy(function->name, parser->function_name,
           sizeof(function->name));
    function->file = (unsigned short)file_index;
    function->body_offset = body_offset;
    function->internal = parser->declaration_internal;
    function->root = graph_root_name(function->name);
    function->returns_void = parser->declaration_void &&
                             !parser->declaration_pointer;
    function->returns_pointer = parser->declaration_pointer;
    (void)memcpy(function->return_type, parser->declaration_return_type,
                 sizeof(function->return_type));
    function->unvalidated_parameters = 0U;
    parser->current_function = graph->function_count;
    parser->statement_start = true;
    graph->function_count++;
    return 0;
}

static int graph_add_call(gsh_canon_call_graph *graph,
                          const graph_parser *parser)
{
    if (parser == NULL || graph == NULL) return -1;
    gsh_canon_call *call;

    if (parser->current_function == SIZE_MAX ||
        graph_call_keyword(parser->call_name)) {
        return 0;
    }
    if (graph->call_count >= GSH_CANON_CALL_CAP ||
        parser->current_function > UINT16_MAX) {
        errno = ENOSPC;
        return -1;
    }
    call = &graph->calls[graph->call_count];
    (void)memcpy(call->name, parser->call_name, sizeof(call->name));
    call->caller = (unsigned short)parser->current_function;
    call->line = parser->call_line;
    call->bare_statement = false;
    graph->call_count++;
    return 0;
}

static void graph_reset_declaration(graph_parser *parser)
{
    if (!require(parser != NULL)) return;
    parser->top_identifier[0] = '\0';
    parser->function_name[0] = '\0';
    parser->declaration_return_type[0] = '\0';
    parser->declaration_internal = false;
    parser->declaration_void = false;
    parser->declaration_pointer = false;
    parser->header_active = false;
    parser->header_closed = false;
    parser->header_parentheses = 0U;
    parser->parameter_candidate[0] = '\0';
    parser->parameter_count = 0U;
    parser->pending_parameter = SIZE_MAX;
    parser->parameter_candidate_pointer = false;
    parser->parameter_array = false;
    graph_reset_guard(parser);
}

static int graph_finish_parameter(graph_parser *parser)
{
    size_t parameter;

    if (!require(parser != NULL)) return -1;
    if (parser->parameter_candidate[0] == '\0' ||
        (strcmp(parser->parameter_candidate, "void") == 0 &&
         !parser->parameter_candidate_pointer)) {
        parser->parameter_candidate[0] = '\0';
        parser->parameter_candidate_pointer = false;
        parser->parameter_array = false;
        return 0;
    }
    if (parser->parameter_count >= GSH_CANON_PARAMETER_CAP) {
        errno = ENOSPC;
        return -1;
    }
    parameter = parser->parameter_count++;
    (void)memcpy(parser->parameters[parameter],
                 parser->parameter_candidate,
                 sizeof(parser->parameters[parameter]));
    parser->parameter_pointer[parameter] =
        parser->parameter_candidate_pointer;
    parser->parameter_validated[parameter] = false;
    parser->parameter_reported[parameter] = false;
    parser->parameter_candidate[0] = '\0';
    parser->parameter_candidate_pointer = false;
    parser->parameter_array = false;
    return 0;
}

static int graph_record_unvalidated_parameter(
    gsh_canon_function *function, graph_parser *parser, size_t parameter)
{
    if (!require(function != NULL && parser != NULL)) return -1;
    if (!require(parameter < parser->parameter_count)) return -1;
    if (parser->parameter_reported[parameter]) return 0;
    if (function->unvalidated_parameters == USHRT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    if (function->unvalidated_parameters >= GSH_CANON_PARAMETER_CAP) {
        errno = ENOSPC;
        return -1;
    }
    (void)memcpy(
        function->unvalidated_parameter_names
            [function->unvalidated_parameters],
        parser->parameters[parameter],
        strlen(parser->parameters[parameter]) + 1U);
    function->unvalidated_parameters++;
    parser->parameter_reported[parameter] = true;
    return 0;
}

static int graph_resolve_parameter_use(gsh_canon_function *function,
                                       graph_parser *parser,
                                       bool dereference)
{
    size_t parameter;

    if (!require(function != NULL && parser != NULL)) return -1;
    parameter = parser->pending_parameter;
    parser->pending_parameter = SIZE_MAX;
    if (parameter == SIZE_MAX || parser->parameter_validated[parameter]) {
        return 0;
    }
    if (dereference) {
        return graph_record_unvalidated_parameter(function, parser,
                                                  parameter);
    }
    if (parser->guard_state == GRAPH_GUARD_CONDITION) {
        parser->parameter_validated[parameter] = true;
    }
    return 0;
}

static int graph_begin_parameter_use(gsh_canon_function *function,
                                     graph_parser *parser,
                                     const char *identifier)
{
    size_t parameter;

    if (!require(function != NULL && parser != NULL)) return -1;
    if (!require(identifier != NULL)) return -1;
    if (graph_resolve_parameter_use(function, parser, false) == -1) {
        return -1;
    }
    if (parser->guard_state == GRAPH_GUARD_CONDITION &&
        strcmp(function->name, "main") == 0 &&
        strcmp(identifier, "argc") == 0) {
        for (parameter = 0U; parameter < parser->parameter_count;
             parameter++) {
            if (strcmp(parser->parameters[parameter], "argv") == 0 ||
                strcmp(parser->parameters[parameter], "envp") == 0) {
                parser->parameter_validated[parameter] = true;
            }
        }
    }
    for (parameter = 0U; parameter < parser->parameter_count; parameter++) {
        if (parser->parameter_pointer[parameter] &&
            !parser->parameter_validated[parameter] &&
            strcmp(identifier, parser->parameters[parameter]) == 0) {
            parser->pending_parameter = parameter;
            if (parser->previous_token_is_punctuation &&
                parser->guard_previous == '*' &&
                parser->unevaluated_close_depth == 0U) {
                return graph_resolve_parameter_use(function, parser, true);
            }
            break;
        }
    }
    return 0;
}

static void graph_accept_guard_identifier(graph_parser *parser,
                                          const char *identifier)
{
    if (!require(parser != NULL)) return;
    if (!require(identifier != NULL)) return;
    if (strcmp(identifier, "if") == 0 &&
        parser->guard_state != GRAPH_GUARD_CONDITION) {
        graph_reset_guard(parser);
        parser->guard_state = GRAPH_GUARD_IF_OPEN;
    } else if ((parser->guard_state == GRAPH_GUARD_BODY ||
                parser->guard_state == GRAPH_GUARD_BRACED_BODY) &&
               strcmp(identifier, "return") == 0) {
        parser->guard_state = GRAPH_GUARD_RETURN;
    } else if (parser->guard_state == GRAPH_GUARD_BRACED_BODY &&
               !graph_guard_control_keyword(identifier)) {
        return;
    } else if (parser->guard_state == GRAPH_GUARD_CONDITION) {
        parser->guard_condition_has_token = true;
    } else if (parser->guard_state != GRAPH_GUARD_RETURN) {
        graph_reset_guard(parser);
    }
}

static int graph_accept_body_identifier(gsh_canon_call_graph *graph,
                                        graph_parser *parser,
                                        const unsigned char *source,
                                        size_t begin, size_t length)
{
    gsh_canon_function *function;
    bool statement_start;

    if (!require(graph != NULL && parser != NULL)) return -1;
    if (!require(source != NULL)) return -1;
    if (!require(parser->current_function < graph->function_count)) return -1;
    function = &graph->functions[parser->current_function];
    statement_start = parser->statement_start;
    if (parser->bare_call_closed) {
        parser->bare_call_index = SIZE_MAX;
        parser->bare_call_closed = false;
    }
    if (graph_begin_parameter_use(function, parser, parser->identifier) ==
        -1) {
        return -1;
    }
    graph_accept_guard_identifier(parser, parser->identifier);
    if (strcmp(parser->identifier, "_Static_assert") == 0 &&
        graph_increment(&function->assertion_count) == -1) return -1;
    if (graph_behavior_keyword(parser->identifier) &&
        graph_increment(&function->behavior_count) == -1) return -1;
    if (graph_copy_name(parser->call_name, source + begin, length) == -1) {
        return -1;
    }
    parser->call_line = parser->line;
    parser->call_bare_pending = statement_start;
    if (strcmp(parser->identifier, "if") == 0 ||
        strcmp(parser->identifier, "while") == 0 ||
        strcmp(parser->identifier, "for") == 0 ||
        strcmp(parser->identifier, "switch") == 0) {
        parser->control_pending = true;
        parser->statement_start = false;
    } else if (strcmp(parser->identifier, "else") == 0 ||
               strcmp(parser->identifier, "do") == 0) {
        parser->statement_start = true;
        parser->call_bare_pending = false;
    } else {
        parser->statement_start = false;
    }
    parser->call_pending = true;
    parser->previous_token_is_punctuation = false;
    return 0;
}

static int graph_accept_identifier(gsh_canon_call_graph *graph,
                                   graph_parser *parser,
                                   const unsigned char *source,
                                   size_t begin, size_t end)
{
    size_t length;

    if (!require(graph != NULL && parser != NULL)) return -1;
    if (!require(source != NULL && begin <= end)) return -1;
    length = end - begin;
    if (graph_copy_name(parser->identifier, source + begin, length) == -1) {
        return -1;
    }
    if (parser->current_function != SIZE_MAX) {
        return graph_accept_body_identifier(graph, parser, source, begin,
                                            length);
    }
    if (parser->header_active && parser->header_parentheses == 1U &&
        !parser->parameter_array) {
        if (graph_copy_name(parser->parameter_candidate, source + begin,
                            length) == -1) {
            return -1;
        }
    } else if (parser->brace_depth == 0U && !parser->header_active) {
        if (strcmp(parser->identifier, "_Static_assert") == 0) {
            if (graph->static_assertion_count == SIZE_MAX) {
                errno = EOVERFLOW;
                return -1;
            }
            graph->static_assertion_count++;
        }
        if (strcmp(parser->identifier, "static") == 0) {
            parser->declaration_internal = true;
        } else {
            if (parser->top_identifier[0] != '\0') {
                (void)memcpy(parser->declaration_return_type,
                             parser->top_identifier,
                             sizeof(parser->declaration_return_type));
            }
            if (graph_copy_name(parser->top_identifier, source + begin,
                                length) == -1) {
                return -1;
            }
        }
        if (strcmp(parser->identifier, "void") == 0) {
            parser->declaration_void = true;
        }
    }
    return 0;
}

static int graph_accept_guard_punctuation(graph_parser *parser,
                                          gsh_canon_function *function,
                                          unsigned char byte,
                                          unsigned char next)
{
    if (!require(parser != NULL)) return -1;
    if (!require(function != NULL)) return -1;
    if (parser->guard_state == GRAPH_GUARD_IF_OPEN) {
        if (byte != '(') {
            graph_reset_guard(parser);
            return 0;
        }
        parser->guard_state = GRAPH_GUARD_CONDITION;
        parser->guard_parentheses = 1U;
        parser->guard_condition_terms = 1U;
        parser->guard_condition_side_effect_free = true;
        return 0;
    }
    if (parser->guard_state == GRAPH_GUARD_CONDITION) {
        if (byte == '(') {
            if (parser->call_pending &&
                strcmp(parser->call_name, "require") == 0) {
                parser->guard_condition_has_assertion = true;
            }
            if (parser->call_pending &&
                !graph_pure_condition_call(parser->call_name)) {
                parser->guard_condition_side_effect_free = false;
            }
            parser->guard_parentheses++;
        } else if (byte == ')') {
            if (parser->guard_parentheses == 0U) {
                errno = EPROTO;
                return -1;
            }
            parser->guard_parentheses--;
            if (parser->guard_parentheses == 0U) {
                parser->guard_state = GRAPH_GUARD_BODY;
            }
        } else {
            parser->guard_condition_has_token = true;
            if ((byte == '&' && next == '&') ||
                (byte == '|' && next == '|')) {
                parser->guard_condition_terms++;
            }
            if (!graph_pure_condition_operator(parser, byte, next)) {
                parser->guard_condition_side_effect_free = false;
            }
        }
        parser->guard_previous = byte;
        return 0;
    }
    if (parser->guard_state == GRAPH_GUARD_BODY && byte == '{') {
        parser->guard_state = GRAPH_GUARD_BRACED_BODY;
        return 0;
    }
    if (parser->guard_state == GRAPH_GUARD_BRACED_BODY) {
        if (byte == '{' || byte == '}') graph_reset_guard(parser);
        return 0;
    }
    if (parser->guard_state == GRAPH_GUARD_RETURN && byte == ';') {
        if (parser->guard_condition_has_token &&
            parser->guard_condition_side_effect_free &&
            !parser->guard_condition_has_assertion &&
            graph_increment_by(&function->assertion_count,
                               parser->guard_condition_terms) == -1) {
            return -1;
        }
        graph_reset_guard(parser);
        return 0;
    }
    if (parser->guard_state != GRAPH_GUARD_RETURN) {
        graph_reset_guard(parser);
    }
    return 0;
}

static int graph_accept_parameter_punctuation(
    gsh_canon_function *function, graph_parser *parser,
    unsigned char byte, unsigned char next)
{
    bool dereference;

    if (!require(function != NULL && parser != NULL)) return -1;
    dereference = parser->unevaluated_close_depth == 0U &&
                  (byte == '[' || (byte == '-' && next == '>'));
    if (graph_resolve_parameter_use(function, parser, dereference) == -1) {
        return -1;
    }
    if (byte == '(' && parser->call_pending &&
        (strcmp(parser->call_name, "sizeof") == 0 ||
         strcmp(parser->call_name, "_Alignof") == 0)) {
        parser->unevaluated_close_depth = parser->statement_parentheses + 1U;
    } else if (byte == ')' && parser->unevaluated_close_depth != 0U &&
               parser->statement_parentheses ==
                   parser->unevaluated_close_depth) {
        parser->unevaluated_close_depth = 0U;
    }
    return 0;
}

static void graph_accept_statement_punctuation(
    gsh_canon_call_graph *graph, graph_parser *parser,
    unsigned char byte)
{
    if (graph == NULL || parser == NULL) return;
    if (parser->bare_call_closed) {
        if (byte == ';' && parser->bare_call_index < graph->call_count) {
            graph->calls[parser->bare_call_index].bare_statement = true;
        }
        parser->bare_call_index = SIZE_MAX;
        parser->bare_call_closed = false;
    }
    if (byte == '(') {
        parser->statement_parentheses++;
        if (parser->control_pending) {
            parser->control_close_depth = parser->statement_parentheses;
            parser->control_pending = false;
        }
    } else if (byte == ')' && parser->statement_parentheses > 0U) {
        if (parser->bare_call_index < graph->call_count &&
            parser->statement_parentheses == parser->bare_call_depth) {
            parser->bare_call_closed = true;
        }
        if (parser->statement_parentheses == parser->control_close_depth) {
            parser->statement_start = true;
            parser->control_close_depth = 0U;
        }
        parser->statement_parentheses--;
    } else if (byte == '?' && parser->statement_parentheses == 0U) {
        parser->ternary_depth++;
    } else if (byte == ':' && parser->statement_parentheses == 0U &&
               parser->ternary_depth > 0U) {
        parser->ternary_depth--;
    } else if ((byte == ';' && parser->statement_parentheses == 0U) ||
               byte == '{' || byte == '}' ||
               (byte == ':' && parser->statement_parentheses == 0U)) {
        parser->statement_start = true;
    }
}

static int graph_accept_function_punctuation(gsh_canon_call_graph *graph,
                                             graph_parser *parser,
                                             unsigned char byte,
                                             unsigned char next)
{
    gsh_canon_function *function;

    if (!require(graph != NULL)) return -1;
    if (!require(parser != NULL)) return -1;
    if (!require(parser->current_function < graph->function_count)) return -1;
    function = &graph->functions[parser->current_function];

    if (graph_accept_parameter_punctuation(
            function, parser, byte, next) == -1) {
        return -1;
    }

    if (graph_accept_guard_punctuation(parser, function, byte, next) == -1) {
        return -1;
    }
    graph_accept_statement_punctuation(graph, parser, byte);
    if (byte == '(' && parser->call_pending &&
        graph_add_call(graph, parser) == -1) {
        return -1;
    }
    if (byte == '(' && parser->call_pending &&
        parser->call_bare_pending &&
        !graph_call_keyword(parser->call_name)) {
        parser->bare_call_index = graph->call_count - 1U;
        parser->bare_call_depth = parser->statement_parentheses;
    }
    if (byte == '(' && parser->call_pending &&
            strcmp(parser->call_name, "require") == 0 &&
        graph_increment(&function->assertion_count) == -1) {
        return -1;
    }
    if (byte == ';' && graph_increment(&function->behavior_count) == -1) {
        return -1;
    }
    parser->call_pending = false;
    parser->call_bare_pending = false;
    parser->guard_previous = byte;
    parser->previous_token_is_punctuation = true;
    if (byte == '{') {
        parser->brace_depth++;
    } else if (byte == '}') {
        if (parser->brace_depth == 0U) {
            errno = EPROTO;
            return -1;
        }
        parser->brace_depth--;
        if (parser->brace_depth == 0U) {
            parser->current_function = SIZE_MAX;
            graph_reset_declaration(parser);
        }
    }
    return 0;
}

static int graph_accept_header_punctuation(gsh_canon_call_graph *graph,
                                           graph_parser *parser,
                                           unsigned char byte,
                                           size_t file_index, size_t offset)
{
    if (!require(graph != NULL && parser != NULL)) return -1;
    if (!require(file_index < GSH_CANON_FILE_CAP)) return -1;
    if (byte == '(') {
        if (!parser->header_active && parser->brace_depth == 0U &&
            parser->top_identifier[0] != '\0') {
            if (strcmp(parser->top_identifier, "__attribute__") == 0) {
                parser->top_identifier[0] = '\0';
            } else {
                (void)memcpy(parser->function_name, parser->top_identifier,
                       sizeof(parser->function_name));
                parser->header_active = true;
                parser->header_parentheses = 1U;
                parser->parameter_count = 0U;
                parser->parameter_candidate[0] = '\0';
                parser->parameter_candidate_pointer = false;
                parser->parameter_array = false;
            }
        } else if (parser->header_active) {
            parser->header_parentheses++;
        }
    } else if (byte == ')' && parser->header_active) {
        if (parser->header_parentheses == 0U) {
            errno = EPROTO;
            return -1;
        }
        if (parser->header_parentheses == 1U &&
            graph_finish_parameter(parser) == -1) {
            return -1;
        }
        parser->header_parentheses--;
        parser->header_closed = parser->header_parentheses == 0U;
    } else if (byte == ',' && parser->header_active &&
               parser->header_parentheses == 1U) {
        if (graph_finish_parameter(parser) == -1) return -1;
    } else if ((byte == '*' || byte == '[') && parser->header_active &&
               parser->header_parentheses == 1U) {
        parser->parameter_candidate_pointer = true;
        if (byte == '[') parser->parameter_array = true;
    } else if (byte == '*' && !parser->header_active &&
               parser->brace_depth == 0U) {
        parser->declaration_pointer = true;
    } else if (byte == '{') {
        parser->brace_depth++;
        if (parser->header_active && parser->header_closed &&
            parser->brace_depth == 1U) {
            if (graph_add_function(graph, parser, file_index, offset) == -1) {
                return -1;
            }
        }
    } else if (byte == '}') {
        if (parser->brace_depth > 0U) {
            parser->brace_depth--;
        }
        if (parser->brace_depth == 0U) {
            graph_reset_declaration(parser);
        }
    } else if (byte == ';' && parser->brace_depth == 0U) {
        graph_reset_declaration(parser);
    } else if (byte != '*' && byte != '[' && byte != ']' && byte != ',') {
        if (!parser->header_active) {
            parser->top_identifier[0] = '\0';
        }
    }
    return 0;
}

static int graph_accept_punctuation(gsh_canon_call_graph *graph,
                                    graph_parser *parser,
                                    unsigned char byte, unsigned char next,
                                    size_t file_index, size_t offset)
{
    if (!require(graph != NULL && parser != NULL)) return -1;
    if (!require(file_index < GSH_CANON_FILE_CAP)) return -1;
    if (parser->current_function != SIZE_MAX) {
        return graph_accept_function_punctuation(graph, parser, byte, next);
    }
    return graph_accept_header_punctuation(graph, parser, byte, file_index,
                                           offset);
}

/* ── Direct Calls Form a Bounded Repository Graph ───────────────
 * Compiler warnings catch unused internal functions one translation unit at
 * a time, but they cannot prove that an exported helper belongs to any real
 * executable.  This lexer records fixed-capacity function and direct-call
 * tables across every target source, then starts reachability only at declared
 * process, test, fuzz, and approved signal roots.  Capacity failure rejects
 * the repository instead of silently truncating the proof.
 * ─────────────────────────────────────────────────────────────── */
static bool graph_consume_lexical_byte(graph_parser *parser,
                                       const unsigned char *source,
                                       size_t length, size_t *index)
{
    if (!require(parser != NULL)) return false;
    if (!require(source != NULL)) return false;
    if (!require(index != NULL && *index < length)) return false;
    unsigned char byte = source[*index];

    if (parser->state == GRAPH_LINE_COMMENT ||
        parser->state == GRAPH_PREPROCESSOR) {
        if (byte == '\n') {
            parser->state = GRAPH_CODE;
            parser->line_prefix = true;
            parser->line++;
        }
        (*index)++;
        return true;
    }
    if (parser->state == GRAPH_BLOCK_COMMENT) {
        if (byte == '*' && *index + 1U < length &&
            source[*index + 1U] == '/') {
            parser->state = GRAPH_CODE;
            *index += 2U;
        } else {
            if (byte == '\n') parser->line++;
            (*index)++;
        }
        return true;
    }
    if (parser->state == GRAPH_STRING ||
        parser->state == GRAPH_CHARACTER) {
        unsigned char terminator =
            parser->state == GRAPH_STRING ? '"' : '\'';

        if (!parser->escaped && byte == terminator) {
            parser->state = GRAPH_CODE;
        }
        parser->escaped = !parser->escaped && byte == '\\';
        if (byte != '\\') parser->escaped = false;
        if (byte == '\n') parser->line++;
        (*index)++;
        return true;
    }
    if (byte == '\n') {
        parser->line_prefix = true;
        parser->call_pending = false;
        parser->line++;
        (*index)++;
        return true;
    }
    if ((byte == ' ' || byte == '\t' || byte == '\r' ||
         byte == '\f' || byte == '\v') && parser->line_prefix) {
        (*index)++;
        return true;
    }
    if (byte == '#' && parser->line_prefix) {
        parser->state = GRAPH_PREPROCESSOR;
        (*index)++;
        return true;
    }
    parser->line_prefix = false;
    return false;
}

static bool graph_consume_code_delimiter(graph_parser *parser,
                                         const unsigned char *source,
                                         size_t length, size_t *index)
{
    if (!require(parser != NULL)) return false;
    if (!require(source != NULL)) return false;
    if (!require(index != NULL && *index < length)) return false;
    unsigned char byte = source[*index];

    if (byte == '/' && *index + 1U < length &&
        source[*index + 1U] == '/') {
        parser->state = GRAPH_LINE_COMMENT;
        *index += 2U;
        return true;
    }
    if (byte == '/' && *index + 1U < length &&
        source[*index + 1U] == '*') {
        parser->state = GRAPH_BLOCK_COMMENT;
        *index += 2U;
        return true;
    }
    if (byte == '"' || byte == '\'') {
        parser->state = byte == '"' ? GRAPH_STRING : GRAPH_CHARACTER;
        parser->escaped = false;
        parser->call_pending = false;
        (*index)++;
        return true;
    }
    return false;
}

int gsh_canon_call_graph_add(gsh_canon_call_graph *graph,
                             const unsigned char *source, size_t length,
                             const char *path)
{
    graph_parser parser = {
        .state = GRAPH_CODE,
        .current_function = SIZE_MAX,
        .line = 1U,
        .bare_call_index = SIZE_MAX,
        .pending_parameter = SIZE_MAX,
        .line_prefix = true,
    };
    size_t file_index;
    size_t index = 0U;

    if (!require(graph != NULL)) return -1;
    if (!require(source != NULL)) return -1;
    if (!require(path != NULL)) return -1;
    if (!require(graph->file_count <= GSH_CANON_FILE_CAP)) return -1;
    if (!require(graph->function_count <= GSH_CANON_FUNCTION_CAP)) return -1;
    if (!require(graph->call_count <= GSH_CANON_CALL_CAP)) return -1;
    if (graph_file_index(graph, path, &file_index) == -1) {
        errno = EINVAL;
        return -1;
    }
    while (index < length) {
        unsigned char byte = source[index];

        if (graph_consume_lexical_byte(&parser, source, length, &index) ||
            graph_consume_code_delimiter(&parser, source, length, &index)) {
            continue;
        }
        if (graph_identifier_start(byte)) {
            size_t begin = index;

            while (index < length &&
                   graph_identifier_continue(source[index])) {
                index++;
            }
            if (graph_accept_identifier(graph, &parser, source, begin,
                                        index) == -1) {
                return -1;
            }
            continue;
        }
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\f' ||
            byte == '\v') {
            index++;
            continue;
        }
        if (graph_accept_punctuation(
                graph, &parser, byte,
                index + 1U < length ? source[index + 1U] : 0U,
                file_index, index) ==
            -1) {
            return -1;
        }
        index++;
    }
    return parser.current_function == SIZE_MAX ? 0 : -1;
}

void gsh_canon_call_graph_initialize(gsh_canon_call_graph *graph)
{
    if (!require(graph != NULL)) return;
    (void)memset(graph, 0, sizeof(*graph));
}

static bool graph_call_resolves(const gsh_canon_call_graph *graph,
                                const gsh_canon_call *call, size_t target)
{
    const gsh_canon_function *caller;
    const gsh_canon_function *candidate;

    if (!require(graph != NULL)) return false;
    if (!require(call != NULL)) return false;
    if (!require(call->caller < graph->function_count &&
                 target < graph->function_count)) return false;
    caller = &graph->functions[call->caller];
    candidate = &graph->functions[target];

    if (strcmp(call->name, candidate->name) != 0) {
        return false;
    }
    if (candidate->internal) {
        return candidate->file == caller->file;
    }
    return true;
}

static bool library_call_returns_value(const char *name)
{
    static const char *const names[] = {
        "access", "chmod", "close", "closedir", "dup", "dup2", "fcntl",
        "fchdir", "fchmod", "fclose", "fflush", "fprintf", "fputs",
        "fread", "fstat", "fsync", "ftruncate", "fwrite", "kill",
        "lstat", "memcpy", "memmove", "memset", "mkstemp", "open",
        "pipe", "poll", "printf", "puts", "read", "readlink",
        "realpath", "recvmsg", "rename", "sendmsg", "setpgid",
        "sigaction", "sigaddset", "sigemptyset", "sigprocmask", "snprintf",
        "socketpair", "stat", "tcsetattr", "tcsetpgrp", "unlink",
        "vsnprintf", "waitpid", "write",
    };
    size_t index;

    if (!require(name != NULL && name[0] != '\0')) return false;
    for (index = 0U; index < sizeof(names) / sizeof(names[0]); index++) {
        if (strcmp(name, names[index]) == 0) return true;
    }
    return false;
}

static bool graph_call_returns_value(const gsh_canon_call_graph *graph,
                                     const gsh_canon_call *call)
{
    size_t function;
    bool resolved = false;

    if (!require(graph != NULL && call != NULL)) return false;
    if (!require(call->caller < graph->function_count)) return false;
    for (function = 0U; function < graph->function_count; function++) {
        if (graph_call_resolves(graph, call, function)) {
            resolved = true;
            if (!graph->functions[function].returns_void) return true;
        }
    }
    return !resolved && library_call_returns_value(call->name);
}

static size_t graph_count_unchecked_returns(
    const gsh_canon_call_graph *graph, bool verbose)
{
    size_t call;
    size_t unchecked = 0U;

    if (!require(graph != NULL)) return GSH_CANON_CALL_CAP + 1U;
    if (!require(graph->call_count <= GSH_CANON_CALL_CAP)) {
        return GSH_CANON_CALL_CAP + 1U;
    }
    for (call = 0U; call < graph->call_count; call++) {
        const gsh_canon_call *entry = &graph->calls[call];

        if (!entry->bare_statement ||
            !graph_call_returns_value(graph, entry)) continue;
        if (verbose) {
            (void)fprintf(stderr,
                    "source policy: unchecked return: %s in %s (%s:%zu)\n",
                    entry->name, graph->functions[entry->caller].name,
                    graph->files[graph->functions[entry->caller].file],
                    entry->line);
        }
        unchecked++;
    }
    return unchecked;
}

static size_t graph_count_unvalidated_parameters(
    const gsh_canon_call_graph *graph, bool verbose)
{
    size_t function;
    size_t unvalidated = 0U;

    if (!require(graph != NULL)) return GSH_CANON_FUNCTION_CAP + 1U;
    for (function = 0U; function < graph->function_count; function++) {
        const gsh_canon_function *entry = &graph->functions[function];

        if (verbose && entry->unvalidated_parameters != 0U) {
            size_t parameter;

            for (parameter = 0U;
                 parameter < entry->unvalidated_parameters; parameter++) {
                (void)fprintf(stderr,
                        "source policy: pointer parameter used before guard: "
                        "%s:%s:%s:%s (%s@%zu)\n",
                        entry->name,
                        entry->unvalidated_parameter_names[parameter],
                        entry->return_type[0] == '\0' ? "int" :
                                                       entry->return_type,
                        entry->returns_void ? "void" :
                            (entry->returns_pointer ? "pointer" : "value"),
                        graph->files[entry->file], entry->body_offset);
            }
        }
        unvalidated += entry->unvalidated_parameters;
    }
    return unvalidated;
}

enum {
    GRAPH_WORD_BITS = 64,
    GRAPH_WORDS = (GSH_CANON_FUNCTION_CAP + GRAPH_WORD_BITS - 1) /
                  GRAPH_WORD_BITS,
};

typedef struct {
    uint64_t closure[GSH_CANON_FUNCTION_CAP][GRAPH_WORDS];
    bool reachable[GSH_CANON_FUNCTION_CAP];
} graph_analysis_workspace;

static void graph_measure_functions(const gsh_canon_call_graph *graph,
                                    graph_analysis_workspace *workspace,
                                    size_t *oversized_functions,
                                    size_t *assertion_deficit, bool verbose)
{
    size_t function;
    size_t assertion_total = 0U;

    if (!require(graph != NULL)) return;
    if (!require(workspace != NULL)) return;
    if (!require(oversized_functions != NULL)) return;
    if (!require(assertion_deficit != NULL)) return;
    if (!require(graph->function_count <= GSH_CANON_FUNCTION_CAP)) return;
    if (!require(graph->file_count <= GSH_CANON_FILE_CAP)) return;
    *oversized_functions = 0U;
    *assertion_deficit = 2U * graph->function_count;
    for (function = 0U; function < graph->function_count; function++) {
        const gsh_canon_function *entry = &graph->functions[function];

        workspace->reachable[function] = entry->root;
        if (entry->behavior_count > 60U) {
            if (verbose) {
                (void)fprintf(stderr,
                        "source policy: oversized function: %s (%s, %u)\n",
                        entry->name, graph->files[entry->file],
                        entry->behavior_count);
            }
            (*oversized_functions)++;
        }
        if (verbose && entry->assertion_count < 2U) {
            (void)fprintf(stderr,
                    "source policy: assertion density: %s (%s, %u/2)\n",
                    entry->name, graph->files[entry->file],
                    entry->assertion_count);
        }
        assertion_total += entry->assertion_count;
        *assertion_deficit =
            entry->assertion_count >= *assertion_deficit
                ? 0U
                : *assertion_deficit - entry->assertion_count;
    }
    *assertion_deficit = graph->static_assertion_count >= *assertion_deficit
                             ? 0U
                             : *assertion_deficit -
                                   graph->static_assertion_count;
    if (verbose) {
        (void)fprintf(stderr,
                "source policy: assertion density total: %zu functions, "
                "%zu runtime guards, %zu static assertions\n",
                graph->function_count, assertion_total,
                graph->static_assertion_count);
    }
}

static bool graph_mark_reachable(const gsh_canon_call_graph *graph,
                                 graph_analysis_workspace *workspace)
{
    size_t pass;
    bool changed = true;

    if (!require(graph != NULL)) return false;
    if (!require(workspace != NULL)) return false;
    if (!require(graph->function_count <= GSH_CANON_FUNCTION_CAP)) {
        return false;
    }
    if (!require(graph->call_count <= GSH_CANON_CALL_CAP)) return false;
    for (pass = 0U; pass < graph->function_count && changed; pass++) {
        size_t call;

        changed = false;
        for (call = 0U; call < graph->call_count; call++) {
            size_t function;

            if (!workspace->reachable[graph->calls[call].caller]) continue;
            for (function = 0U; function < graph->function_count;
                 function++) {
                if (graph_call_resolves(graph, &graph->calls[call],
                                        function) &&
                    !workspace->reachable[function]) {
                    workspace->reachable[function] = true;
                    changed = true;
                }
            }
        }
    }
    return !changed;
}

static size_t graph_count_unreachable(
    const gsh_canon_call_graph *graph,
    const graph_analysis_workspace *workspace, bool verbose)
{
    size_t count = 0U;
    size_t function;

    if (!require(graph != NULL)) return 0U;
    if (!require(workspace != NULL)) return 0U;
    if (!require(graph->function_count <= GSH_CANON_FUNCTION_CAP)) return 0U;
    if (!require(graph->file_count <= GSH_CANON_FILE_CAP)) return 0U;
    for (function = 0U; function < graph->function_count; function++) {
        if (!workspace->reachable[function]) {
            if (verbose) {
                (void)fprintf(stderr,
                        "source policy: unreachable function: %s (%s)\n",
                        graph->functions[function].name,
                        graph->files[graph->functions[function].file]);
            }
            count++;
        }
    }
    return count;
}

static size_t graph_build_closure(const gsh_canon_call_graph *graph,
                                  graph_analysis_workspace *workspace,
                                  bool verbose)
{
    size_t direct_recursive_calls = 0U;
    size_t call;

    if (!require(graph != NULL)) return 0U;
    if (!require(workspace != NULL)) return 0U;
    if (!require(graph->function_count <= GSH_CANON_FUNCTION_CAP)) return 0U;
    if (!require(graph->call_count <= GSH_CANON_CALL_CAP)) return 0U;
    for (call = 0U; call < graph->call_count; call++) {
        size_t target;

        for (target = 0U; target < graph->function_count; target++) {
            if (graph_call_resolves(graph, &graph->calls[call], target)) {
                workspace->closure[graph->calls[call].caller]
                                  [target / GRAPH_WORD_BITS] |=
                    UINT64_C(1) << (target % GRAPH_WORD_BITS);
            }
        }
        if (graph_call_resolves(graph, &graph->calls[call],
                                graph->calls[call].caller)) {
            if (verbose) {
                (void)fprintf(stderr, "source policy: direct recursion: %s (%s)\n",
                        graph->functions[graph->calls[call].caller].name,
                        graph->files[graph->functions[
                            graph->calls[call].caller].file]);
            }
            direct_recursive_calls++;
        }
    }
    return direct_recursive_calls;
}

static void graph_close_transitively(const gsh_canon_call_graph *graph,
                                     graph_analysis_workspace *workspace)
{
    size_t function;

    if (!require(graph != NULL)) return;
    if (!require(workspace != NULL)) return;
    if (!require(graph->function_count <= GSH_CANON_FUNCTION_CAP)) return;
    for (function = 0U; function < graph->function_count; function++) {
        size_t caller;
        uint64_t mask = UINT64_C(1) << (function % GRAPH_WORD_BITS);

        for (caller = 0U; caller < graph->function_count; caller++) {
            size_t word;

            if ((workspace->closure[caller]
                                   [function / GRAPH_WORD_BITS] & mask) == 0U) {
                continue;
            }
            for (word = 0U; word < GRAPH_WORDS; word++) {
                workspace->closure[caller][word] |=
                    workspace->closure[function][word];
            }
        }
    }
}

static size_t graph_count_recursive(
    const gsh_canon_call_graph *graph,
    const graph_analysis_workspace *workspace, bool verbose)
{
    size_t count = 0U;
    size_t function;

    if (!require(graph != NULL)) return 0U;
    if (!require(workspace != NULL)) return 0U;
    if (!require(graph->function_count <= GSH_CANON_FUNCTION_CAP)) return 0U;
    for (function = 0U; function < graph->function_count; function++) {
        uint64_t mask = UINT64_C(1) << (function % GRAPH_WORD_BITS);

        if ((workspace->closure[function]
                               [function / GRAPH_WORD_BITS] & mask) != 0U) {
            if (verbose) {
                (void)fprintf(stderr,
                        "source policy: recursive call cycle: %s (%s)\n",
                        graph->functions[function].name,
                        graph->files[graph->functions[function].file]);
            }
            count++;
        }
    }
    return count;
}

static bool graph_function_recursive(
    const graph_analysis_workspace *workspace, size_t function)
{
    uint64_t mask;

    if (!require(workspace != NULL)) return false;
    if (!require(function < GSH_CANON_FUNCTION_CAP)) return false;
    mask = UINT64_C(1) << (function % GRAPH_WORD_BITS);
    return (workspace->closure[function][function / GRAPH_WORD_BITS] &
            mask) != 0U;
}

static void graph_report_recursive_edges(
    const gsh_canon_call_graph *graph,
    const graph_analysis_workspace *workspace)
{
    size_t call;

    if (!require(graph != NULL)) return;
    if (!require(workspace != NULL)) return;
    if (!require(graph->function_count <= GSH_CANON_FUNCTION_CAP)) return;
    if (!require(graph->file_count <= GSH_CANON_FILE_CAP)) return;
    for (call = 0U; call < graph->call_count; call++) {
        size_t caller = graph->calls[call].caller;
        size_t target;

        if (!graph_function_recursive(workspace, caller)) continue;
        for (target = 0U; target < graph->function_count; target++) {
            if (graph_function_recursive(workspace, target) &&
                graph_call_resolves(graph, &graph->calls[call], target)) {
                (void)fprintf(stderr, "source policy: recursive edge: %s -> %s\n",
                        graph->functions[caller].name,
                        graph->functions[target].name);
            }
        }
    }
}

int gsh_canon_call_graph_analyze(const gsh_canon_call_graph *graph,
                                 size_t *unreachable_functions,
                                 size_t *direct_recursive_calls,
                                 size_t *recursive_functions,
                                 size_t *oversized_functions,
                                 size_t *assertion_deficit,
                                 size_t *unchecked_returns,
                                 size_t *unvalidated_parameters,
                                 bool verbose)
{
    static graph_analysis_workspace workspace;

    if (graph == NULL || unreachable_functions == NULL ||
        direct_recursive_calls == NULL || recursive_functions == NULL ||
        oversized_functions == NULL || assertion_deficit == NULL ||
        unchecked_returns == NULL || unvalidated_parameters == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (!require(graph->file_count <= GSH_CANON_FILE_CAP)) return -1;
    if (!require(graph->function_count <= GSH_CANON_FUNCTION_CAP)) return -1;
    if (!require(graph->call_count <= GSH_CANON_CALL_CAP)) return -1;
    (void)memset(&workspace, 0, sizeof(workspace));
    graph_measure_functions(graph, &workspace, oversized_functions,
                            assertion_deficit, verbose);
    if (!graph_mark_reachable(graph, &workspace)) return -1;
    *unreachable_functions =
        graph_count_unreachable(graph, &workspace, verbose);
    *direct_recursive_calls = graph_build_closure(graph, &workspace, verbose);
    graph_close_transitively(graph, &workspace);
    *recursive_functions = graph_count_recursive(graph, &workspace, verbose);
    *unchecked_returns = graph_count_unchecked_returns(graph, verbose);
    *unvalidated_parameters =
        graph_count_unvalidated_parameters(graph, verbose);
    if (verbose && *recursive_functions != 0U) {
        graph_report_recursive_edges(graph, &workspace);
    }
    return 0;
}
