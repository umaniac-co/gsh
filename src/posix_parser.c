#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "posix_parser.h"

#include <stdbool.h>
#include <string.h>

typedef struct {
    const unsigned char *input;
    size_t length;
    gsh_parse_storage *storage;
    size_t position;
    gsh_parse_command_probe_fn command_probe;
    void *command_probe_opaque;
    gsh_word_ref *command_candidate;
    gsh_parse_result result;
} parser;

static size_t parse_list(parser *state, const char *const *terminators,
                         size_t terminator_count, bool stop_at_rparen);
static size_t parse_command(parser *state);
static size_t parse_case_body(parser *state);

static const gsh_token *current_token(const parser *state)
{
    return &state->storage->tokens[state->position];
}

static const gsh_token *token_at(const parser *state, size_t lookahead)
{
    size_t position = state->position + lookahead;

    if (position >= state->storage->token_count) {
        position = state->storage->token_count - 1U;
    }
    return &state->storage->tokens[position];
}

static void fail(parser *state, gsh_parse_status status)
{
    if (state->result.status != GSH_PARSE_OK) {
        return;
    }
    state->result.status = status;
    state->result.error_offset = current_token(state)->begin;
    state->result.unexpected = current_token(state)->kind;
}

static bool raw_word_equals(const parser *state, const gsh_token *token,
                            const char *word)
{
    size_t length = strlen(word);

    return token->kind == GSH_TOKEN_WORD &&
           token->end - token->begin == length &&
           memcmp(state->input + token->begin, word, length) == 0;
}

static bool current_word_is(const parser *state, const char *word)
{
    return raw_word_equals(state, current_token(state), word);
}

static bool is_name_span(const parser *state, const gsh_token *token)
{
    size_t offset;

    if (token->kind != GSH_TOKEN_WORD || token->begin == token->end) {
        return false;
    }
    if (!((state->input[token->begin] >= 'A' &&
           state->input[token->begin] <= 'Z') ||
          (state->input[token->begin] >= 'a' &&
           state->input[token->begin] <= 'z') ||
          state->input[token->begin] == '_')) {
        return false;
    }
    for (offset = token->begin + 1U; offset < token->end; offset++) {
        unsigned char byte = state->input[offset];

        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '_')) {
            return false;
        }
    }
    return true;
}

static bool is_digit_span(const parser *state, const gsh_token *token)
{
    size_t offset;

    if (token->kind != GSH_TOKEN_WORD || token->begin == token->end) {
        return false;
    }
    for (offset = token->begin; offset < token->end; offset++) {
        if (state->input[offset] < '0' || state->input[offset] > '9') {
            return false;
        }
    }
    return true;
}

static bool is_assignment_span(const parser *state,
                               const gsh_token *token)
{
    size_t offset;

    if (token->kind != GSH_TOKEN_WORD || token->begin == token->end ||
        !((state->input[token->begin] >= 'A' &&
           state->input[token->begin] <= 'Z') ||
          (state->input[token->begin] >= 'a' &&
           state->input[token->begin] <= 'z') ||
          state->input[token->begin] == '_')) {
        return false;
    }
    for (offset = token->begin + 1U; offset < token->end; offset++) {
        unsigned char byte = state->input[offset];

        if (byte == '=') {
            return true;
        }
        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '_')) {
            return false;
        }
    }
    return false;
}

static bool probe_command_word(parser *state, const gsh_token *token)
{
    size_t length;

    if (state->command_probe == NULL || token->kind != GSH_TOKEN_WORD) {
        return false;
    }
    length = token->end - token->begin;
    if (!state->command_probe(state->command_probe_opaque,
                              (const char *)state->input + token->begin,
                              length)) {
        return false;
    }
    state->command_candidate->begin = token->begin;
    state->command_candidate->end = token->end;
    fail(state, GSH_PARSE_REWRITE);
    return true;
}

static bool is_redirect_operator(gsh_token_kind kind)
{
    return kind == GSH_TOKEN_LESS || kind == GSH_TOKEN_LESSAND ||
           kind == GSH_TOKEN_GREAT || kind == GSH_TOKEN_GREATAND ||
           kind == GSH_TOKEN_DGREAT || kind == GSH_TOKEN_LESSGREAT ||
           kind == GSH_TOKEN_CLOBBER || kind == GSH_TOKEN_DLESS ||
           kind == GSH_TOKEN_DLESSDASH;
}

static bool is_any_reserved(const parser *state, const gsh_token *token)
{
    static const char *const words[] = {
        "if",   "then", "else", "elif", "fi",   "do",   "done",
        "case", "esac", "while", "until", "for", "in",   "{",
        "}",    "!",
    };
    size_t index;

    for (index = 0; index < sizeof(words) / sizeof(words[0]); index++) {
        if (raw_word_equals(state, token, words[index])) {
            return true;
        }
    }
    return false;
}

static size_t new_node(parser *state, gsh_ast_kind kind, size_t begin)
{
    gsh_ast_node *node;
    size_t index;

    if (state->storage->node_count == GSH_PARSE_NODE_CAP) {
        fail(state, GSH_PARSE_LIMIT);
        return GSH_AST_NONE;
    }
    index = state->storage->node_count++;
    node = &state->storage->nodes[index];
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->connector = GSH_TOKEN_EOF;
    node->first_child = GSH_AST_NONE;
    node->last_child = GSH_AST_NONE;
    node->next_sibling = GSH_AST_NONE;
    node->first_word = state->storage->word_count;
    node->first_redirect = state->storage->redirect_count;
    node->begin = begin;
    node->end = begin;
    return index;
}

static bool append_child(parser *state, size_t parent_index,
                         size_t child_index)
{
    gsh_ast_node *parent;

    if (parent_index == GSH_AST_NONE || child_index == GSH_AST_NONE) {
        return false;
    }
    parent = &state->storage->nodes[parent_index];
    if (parent->first_child == GSH_AST_NONE) {
        parent->first_child = child_index;
    } else {
        state->storage->nodes[parent->last_child].next_sibling = child_index;
    }
    parent->last_child = child_index;
    parent->end = state->storage->nodes[child_index].end;
    return true;
}

static bool append_word(parser *state, size_t node_index,
                        const gsh_token *token)
{
    gsh_ast_node *node;

    if (state->storage->word_count == GSH_PARSE_WORD_CAP) {
        fail(state, GSH_PARSE_LIMIT);
        return false;
    }
    node = &state->storage->nodes[node_index];
    state->storage->words[state->storage->word_count].begin = token->begin;
    state->storage->words[state->storage->word_count].end = token->end;
    state->storage->word_count++;
    node->word_count++;
    node->end = token->end;
    return true;
}

static int descriptor_from_token(const parser *state, const gsh_token *token)
{
    int descriptor = 0;
    size_t offset;

    for (offset = token->begin; offset < token->end; offset++) {
        unsigned int digit = (unsigned int)(state->input[offset] - '0');

        if (descriptor > 1000000) {
            return -2;
        }
        descriptor = descriptor * 10 + (int)digit;
    }
    return descriptor;
}

static bool append_redirect(parser *state, size_t node_index, int descriptor,
                            gsh_token_kind operator_kind,
                            const gsh_token *target)
{
    gsh_ast_node *node;
    gsh_redirect *redirect;

    if (state->storage->redirect_count == GSH_PARSE_REDIRECT_CAP) {
        fail(state, GSH_PARSE_LIMIT);
        return false;
    }
    node = &state->storage->nodes[node_index];
    redirect = &state->storage
                    ->redirects[state->storage->redirect_count++];
    redirect->descriptor = descriptor;
    redirect->operator_kind = operator_kind;
    redirect->target.begin = target->begin;
    redirect->target.end = target->end;
    redirect->body.begin = target->end;
    redirect->body.end = target->end;
    redirect->flags = 0;
    if (operator_kind == GSH_TOKEN_DLESS ||
        operator_kind == GSH_TOKEN_DLESSDASH) {
        size_t heredoc;
        bool found = false;

        for (heredoc = 0; heredoc < state->storage->heredoc_count;
             heredoc++) {
            const gsh_heredoc *source = &state->storage->heredocs[heredoc];

            if (source->delimiter.begin == target->begin &&
                source->delimiter.end == target->end) {
                redirect->body = source->body;
                redirect->flags = source->flags;
                found = true;
                break;
            }
        }
        if (!found) {
            fail(state, GSH_PARSE_INCOMPLETE);
            return false;
        }
    }
    node->redirect_count++;
    node->end = target->end;
    return true;
}

static void skip_linebreak(parser *state)
{
    while (current_token(state)->kind == GSH_TOKEN_NEWLINE) {
        state->position++;
    }
}

static bool at_terminator(const parser *state,
                          const char *const *terminators,
                          size_t terminator_count, bool stop_at_rparen)
{
    size_t index;

    if (stop_at_rparen &&
        current_token(state)->kind == GSH_TOKEN_RPAREN) {
        return true;
    }
    for (index = 0; index < terminator_count; index++) {
        if (current_word_is(state, terminators[index])) {
            return true;
        }
    }
    return false;
}

static bool expect_word(parser *state, const char *word)
{
    if (current_word_is(state, word)) {
        state->position++;
        return true;
    }
    fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                    ? GSH_PARSE_INCOMPLETE
                    : GSH_PARSE_SYNTAX);
    return false;
}

static bool parse_one_redirect(parser *state, size_t node_index)
{
    const gsh_token *operator_token;
    const gsh_token *target;
    int descriptor = -1;

    if (current_token(state)->kind == GSH_TOKEN_WORD &&
        is_digit_span(state, current_token(state)) &&
        is_redirect_operator(token_at(state, 1)->kind) &&
        current_token(state)->end == token_at(state, 1)->begin) {
        descriptor = descriptor_from_token(state, current_token(state));
        if (descriptor < 0) {
            fail(state, GSH_PARSE_LIMIT);
            return false;
        }
        state->position++;
    }
    if (!is_redirect_operator(current_token(state)->kind)) {
        return false;
    }
    operator_token = current_token(state);
    state->position++;
    target = current_token(state);
    if (target->kind != GSH_TOKEN_WORD) {
        fail(state, target->kind == GSH_TOKEN_EOF ? GSH_PARSE_INCOMPLETE
                                                  : GSH_PARSE_SYNTAX);
        return false;
    }
    state->position++;
    return append_redirect(state, node_index, descriptor,
                           operator_token->kind, target);
}

static size_t parse_simple(parser *state)
{
    size_t node = new_node(state, GSH_AST_SIMPLE, current_token(state)->begin);
    bool consumed = false;
    bool command_word = false;

    if (node == GSH_AST_NONE) {
        return node;
    }
    while (state->result.status == GSH_PARSE_OK) {
        if (is_redirect_operator(current_token(state)->kind) ||
            (current_token(state)->kind == GSH_TOKEN_WORD &&
             is_digit_span(state, current_token(state)) &&
             is_redirect_operator(token_at(state, 1)->kind) &&
             current_token(state)->end == token_at(state, 1)->begin)) {
            if (!parse_one_redirect(state, node)) {
                return GSH_AST_NONE;
            }
            consumed = true;
            continue;
        }
        if (current_token(state)->kind != GSH_TOKEN_WORD) {
            break;
        }
        if (!command_word && !is_assignment_span(state,
                                                  current_token(state))) {
            if (probe_command_word(state, current_token(state))) {
                return GSH_AST_NONE;
            }
            command_word = true;
        }
        if (!append_word(state, node, current_token(state))) {
            return GSH_AST_NONE;
        }
        state->position++;
        consumed = true;
    }
    if (!consumed) {
        fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                        ? GSH_PARSE_INCOMPLETE
                        : GSH_PARSE_SYNTAX);
        return GSH_AST_NONE;
    }
    return node;
}

static size_t parse_subshell(parser *state)
{
    size_t node = new_node(state, GSH_AST_SUBSHELL,
                           current_token(state)->begin);
    size_t body;

    state->position++;
    body = parse_list(state, NULL, 0, true);
    if (body == GSH_AST_NONE || !append_child(state, node, body)) {
        return GSH_AST_NONE;
    }
    if (current_token(state)->kind != GSH_TOKEN_RPAREN) {
        fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                        ? GSH_PARSE_INCOMPLETE
                        : GSH_PARSE_SYNTAX);
        return GSH_AST_NONE;
    }
    state->storage->nodes[node].end = current_token(state)->end;
    state->position++;
    return node;
}

static size_t parse_brace_group(parser *state)
{
    static const char *const terminators[] = {"}"};
    size_t node = new_node(state, GSH_AST_BRACE_GROUP,
                           current_token(state)->begin);
    size_t body;

    state->position++;
    body = parse_list(state, terminators, 1, false);
    if (body == GSH_AST_NONE || !append_child(state, node, body) ||
        !expect_word(state, "}")) {
        return GSH_AST_NONE;
    }
    state->storage->nodes[node].end =
        state->storage->tokens[state->position - 1U].end;
    return node;
}

static size_t parse_loop(parser *state, bool until)
{
    static const char *const do_terminator[] = {"do"};
    static const char *const done_terminator[] = {"done"};
    size_t node = new_node(state, until ? GSH_AST_UNTIL : GSH_AST_WHILE,
                           current_token(state)->begin);
    size_t condition;
    size_t body;

    state->position++;
    condition = parse_list(state, do_terminator, 1, false);
    if (condition == GSH_AST_NONE || !append_child(state, node, condition) ||
        !expect_word(state, "do")) {
        return GSH_AST_NONE;
    }
    body = parse_list(state, done_terminator, 1, false);
    if (body == GSH_AST_NONE || !append_child(state, node, body) ||
        !expect_word(state, "done")) {
        return GSH_AST_NONE;
    }
    state->storage->nodes[node].end =
        state->storage->tokens[state->position - 1U].end;
    return node;
}

static size_t new_if_branch(parser *state, size_t begin, size_t condition,
                            size_t body)
{
    size_t branch = new_node(state, GSH_AST_IF_BRANCH, begin);

    if (branch == GSH_AST_NONE) {
        return branch;
    }
    if (condition != GSH_AST_NONE && !append_child(state, branch, condition)) {
        return GSH_AST_NONE;
    }
    if (!append_child(state, branch, body)) {
        return GSH_AST_NONE;
    }
    return branch;
}

static size_t parse_if(parser *state)
{
    static const char *const then_terminator[] = {"then"};
    static const char *const branch_terminators[] = {"elif", "else", "fi"};
    static const char *const fi_terminator[] = {"fi"};
    size_t node = new_node(state, GSH_AST_IF, current_token(state)->begin);

    state->position++;
    for (;;) {
        size_t begin = current_token(state)->begin;
        size_t condition =
            parse_list(state, then_terminator, 1, false);
        size_t body;
        size_t branch;

        if (condition == GSH_AST_NONE || !expect_word(state, "then")) {
            return GSH_AST_NONE;
        }
        body = parse_list(state, branch_terminators, 3, false);
        if (body == GSH_AST_NONE) {
            return GSH_AST_NONE;
        }
        branch = new_if_branch(state, begin, condition, body);
        if (branch == GSH_AST_NONE || !append_child(state, node, branch)) {
            return GSH_AST_NONE;
        }
        if (!current_word_is(state, "elif")) {
            break;
        }
        state->position++;
    }
    if (current_word_is(state, "else")) {
        size_t begin = current_token(state)->begin;
        size_t body;
        size_t branch;

        state->position++;
        body = parse_list(state, fi_terminator, 1, false);
        if (body == GSH_AST_NONE) {
            return GSH_AST_NONE;
        }
        branch = new_if_branch(state, begin, GSH_AST_NONE, body);
        if (branch == GSH_AST_NONE || !append_child(state, node, branch)) {
            return GSH_AST_NONE;
        }
    }
    if (!expect_word(state, "fi")) {
        return GSH_AST_NONE;
    }
    state->storage->nodes[node].end =
        state->storage->tokens[state->position - 1U].end;
    return node;
}

static size_t parse_for(parser *state)
{
    static const char *const done_terminator[] = {"done"};
    size_t node = new_node(state, GSH_AST_FOR, current_token(state)->begin);
    bool has_in = false;
    bool had_separator = false;
    size_t body;

    state->position++;
    if (!is_name_span(state, current_token(state)) ||
        !append_word(state, node, current_token(state))) {
        fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                        ? GSH_PARSE_INCOMPLETE
                        : GSH_PARSE_SYNTAX);
        return GSH_AST_NONE;
    }
    state->position++;
    skip_linebreak(state);
    if (current_word_is(state, "in")) {
        has_in = true;
        state->storage->nodes[node].flags |= GSH_AST_FLAG_FOR_HAS_IN;
        state->position++;
        while (current_token(state)->kind == GSH_TOKEN_WORD &&
               !current_word_is(state, "do")) {
            if (!append_word(state, node, current_token(state))) {
                return GSH_AST_NONE;
            }
            state->position++;
        }
    }
    if (current_token(state)->kind == GSH_TOKEN_SEMICOLON) {
        state->position++;
        had_separator = true;
        skip_linebreak(state);
    } else if (current_token(state)->kind == GSH_TOKEN_NEWLINE) {
        skip_linebreak(state);
        had_separator = true;
    }
    if (has_in && !had_separator) {
        fail(state, GSH_PARSE_SYNTAX);
        return GSH_AST_NONE;
    }
    if (!expect_word(state, "do")) {
        return GSH_AST_NONE;
    }
    body = parse_list(state, done_terminator, 1, false);
    if (body == GSH_AST_NONE || !append_child(state, node, body) ||
        !expect_word(state, "done")) {
        return GSH_AST_NONE;
    }
    state->storage->nodes[node].end =
        state->storage->tokens[state->position - 1U].end;
    return node;
}

static size_t parse_case(parser *state)
{
    size_t node = new_node(state, GSH_AST_CASE,
                           current_token(state)->begin);

    state->position++;
    if (current_token(state)->kind != GSH_TOKEN_WORD ||
        !append_word(state, node, current_token(state))) {
        fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                        ? GSH_PARSE_INCOMPLETE
                        : GSH_PARSE_SYNTAX);
        return GSH_AST_NONE;
    }
    state->position++;
    skip_linebreak(state);
    if (!expect_word(state, "in")) {
        return GSH_AST_NONE;
    }
    skip_linebreak(state);
    while (!current_word_is(state, "esac")) {
        size_t item = new_node(state, GSH_AST_CASE_ITEM,
                               current_token(state)->begin);
        size_t body;

        if (item == GSH_AST_NONE) {
            return GSH_AST_NONE;
        }
        if (current_token(state)->kind == GSH_TOKEN_LPAREN) {
            state->position++;
        }
        if (current_token(state)->kind != GSH_TOKEN_WORD) {
            fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                            ? GSH_PARSE_INCOMPLETE
                            : GSH_PARSE_SYNTAX);
            return GSH_AST_NONE;
        }
        for (;;) {
            if (!append_word(state, item, current_token(state))) {
                return GSH_AST_NONE;
            }
            state->position++;
            if (current_token(state)->kind != GSH_TOKEN_PIPE) {
                break;
            }
            state->position++;
            if (current_token(state)->kind != GSH_TOKEN_WORD) {
                fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                                ? GSH_PARSE_INCOMPLETE
                                : GSH_PARSE_SYNTAX);
                return GSH_AST_NONE;
            }
        }
        if (current_token(state)->kind != GSH_TOKEN_RPAREN) {
            fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                            ? GSH_PARSE_INCOMPLETE
                            : GSH_PARSE_SYNTAX);
            return GSH_AST_NONE;
        }
        state->position++;
        skip_linebreak(state);
        if (current_token(state)->kind != GSH_TOKEN_DSEMI &&
            current_token(state)->kind != GSH_TOKEN_SEMI_AND &&
            !current_word_is(state, "esac")) {
            body = parse_case_body(state);
            if (body == GSH_AST_NONE || !append_child(state, item, body)) {
                return GSH_AST_NONE;
            }
        }
        if (current_token(state)->kind == GSH_TOKEN_DSEMI ||
            current_token(state)->kind == GSH_TOKEN_SEMI_AND) {
            if (current_token(state)->kind == GSH_TOKEN_SEMI_AND) {
                state->storage->nodes[item].flags |=
                    GSH_AST_FLAG_CASE_FALLTHROUGH;
            }
            state->storage->nodes[item].end = current_token(state)->end;
            state->position++;
            skip_linebreak(state);
        } else if (!current_word_is(state, "esac")) {
            fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                            ? GSH_PARSE_INCOMPLETE
                            : GSH_PARSE_SYNTAX);
            return GSH_AST_NONE;
        }
        if (!append_child(state, node, item)) {
            return GSH_AST_NONE;
        }
        if (current_token(state)->kind == GSH_TOKEN_EOF) {
            fail(state, GSH_PARSE_INCOMPLETE);
            return GSH_AST_NONE;
        }
    }
    if (!expect_word(state, "esac")) {
        return GSH_AST_NONE;
    }
    state->storage->nodes[node].end =
        state->storage->tokens[state->position - 1U].end;
    return node;
}

static size_t parse_compound(parser *state)
{
    if (current_token(state)->kind == GSH_TOKEN_LPAREN) {
        return parse_subshell(state);
    }
    if (current_word_is(state, "{")) {
        return parse_brace_group(state);
    }
    if (current_word_is(state, "if")) {
        return parse_if(state);
    }
    if (current_word_is(state, "while")) {
        return parse_loop(state, false);
    }
    if (current_word_is(state, "until")) {
        return parse_loop(state, true);
    }
    if (current_word_is(state, "for")) {
        return parse_for(state);
    }
    if (current_word_is(state, "case")) {
        return parse_case(state);
    }
    return GSH_AST_NONE;
}

static size_t parse_function(parser *state)
{
    size_t node = new_node(state, GSH_AST_FUNCTION,
                           current_token(state)->begin);
    const gsh_token *name = current_token(state);
    size_t body;

    if (!append_word(state, node, name)) {
        return GSH_AST_NONE;
    }
    state->position += 3U;
    skip_linebreak(state);
    body = parse_compound(state);
    if (body == GSH_AST_NONE || !append_child(state, node, body)) {
        if (state->result.status == GSH_PARSE_OK) {
            fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                            ? GSH_PARSE_INCOMPLETE
                            : GSH_PARSE_SYNTAX);
        }
        return GSH_AST_NONE;
    }
    while (is_redirect_operator(current_token(state)->kind) ||
           (is_digit_span(state, current_token(state)) &&
            is_redirect_operator(token_at(state, 1)->kind) &&
            current_token(state)->end == token_at(state, 1)->begin)) {
        if (!parse_one_redirect(state, node)) {
            return GSH_AST_NONE;
        }
    }
    return node;
}

static size_t parse_command(parser *state)
{
    size_t node;

    if (current_token(state)->kind == GSH_TOKEN_WORD &&
        is_name_span(state, current_token(state)) &&
        token_at(state, 1)->kind == GSH_TOKEN_LPAREN &&
        token_at(state, 2)->kind == GSH_TOKEN_RPAREN) {
        return parse_function(state);
    }
    node = parse_compound(state);
    if (node != GSH_AST_NONE) {
        while (is_redirect_operator(current_token(state)->kind) ||
               (is_digit_span(state, current_token(state)) &&
                is_redirect_operator(token_at(state, 1)->kind) &&
                current_token(state)->end == token_at(state, 1)->begin)) {
            if (!parse_one_redirect(state, node)) {
                return GSH_AST_NONE;
            }
        }
        return node;
    }
    if (state->result.status != GSH_PARSE_OK) {
        return GSH_AST_NONE;
    }
    if (is_any_reserved(state, current_token(state))) {
        fail(state, GSH_PARSE_SYNTAX);
        return GSH_AST_NONE;
    }
    return parse_simple(state);
}

static size_t parse_pipeline(parser *state)
{
    size_t node = new_node(state, GSH_AST_PIPELINE,
                           current_token(state)->begin);
    size_t command;

    if (current_word_is(state, "!")) {
        state->storage->nodes[node].flags |= GSH_AST_FLAG_NEGATED;
        state->position++;
        skip_linebreak(state);
    }
    command = parse_command(state);
    if (command == GSH_AST_NONE || !append_child(state, node, command)) {
        return GSH_AST_NONE;
    }
    while (current_token(state)->kind == GSH_TOKEN_PIPE) {
        state->position++;
        skip_linebreak(state);
        command = parse_command(state);
        if (command == GSH_AST_NONE || !append_child(state, node, command)) {
            return GSH_AST_NONE;
        }
    }
    return node;
}

static size_t parse_and_or(parser *state)
{
    size_t node = new_node(state, GSH_AST_AND_OR,
                           current_token(state)->begin);
    size_t pipeline = parse_pipeline(state);

    if (pipeline == GSH_AST_NONE || !append_child(state, node, pipeline)) {
        return GSH_AST_NONE;
    }
    while (current_token(state)->kind == GSH_TOKEN_AND_IF ||
           current_token(state)->kind == GSH_TOKEN_OR_IF) {
        gsh_token_kind connector = current_token(state)->kind;

        state->position++;
        skip_linebreak(state);
        pipeline = parse_pipeline(state);
        if (pipeline == GSH_AST_NONE) {
            return GSH_AST_NONE;
        }
        state->storage->nodes[pipeline].connector = connector;
        if (!append_child(state, node, pipeline)) {
            return GSH_AST_NONE;
        }
    }
    return node;
}

static bool at_case_body_terminator(const parser *state)
{
    return current_token(state)->kind == GSH_TOKEN_DSEMI ||
           current_token(state)->kind == GSH_TOKEN_SEMI_AND ||
           current_word_is(state, "esac");
}

static size_t parse_case_body(parser *state)
{
    size_t node;

    skip_linebreak(state);
    if (at_case_body_terminator(state) ||
        current_token(state)->kind == GSH_TOKEN_EOF) {
        return GSH_AST_NONE;
    }
    node = new_node(state, GSH_AST_LIST, current_token(state)->begin);
    while (state->result.status == GSH_PARSE_OK) {
        size_t command = parse_and_or(state);
        bool separated = false;

        if (command == GSH_AST_NONE || !append_child(state, node, command)) {
            return GSH_AST_NONE;
        }
        if (current_token(state)->kind == GSH_TOKEN_AMPERSAND ||
            current_token(state)->kind == GSH_TOKEN_SEMICOLON) {
            if (current_token(state)->kind == GSH_TOKEN_AMPERSAND) {
                state->storage->nodes[command].flags |= GSH_AST_FLAG_ASYNC;
            }
            state->position++;
            separated = true;
            skip_linebreak(state);
        } else if (current_token(state)->kind == GSH_TOKEN_NEWLINE) {
            skip_linebreak(state);
            separated = true;
        }
        if (at_case_body_terminator(state)) {
            break;
        }
        if (current_token(state)->kind == GSH_TOKEN_EOF) {
            fail(state, GSH_PARSE_INCOMPLETE);
            return GSH_AST_NONE;
        }
        if (!separated) {
            fail(state, GSH_PARSE_SYNTAX);
            return GSH_AST_NONE;
        }
    }
    return node;
}

static size_t parse_list(parser *state, const char *const *terminators,
                         size_t terminator_count, bool stop_at_rparen)
{
    size_t node;

    skip_linebreak(state);
    if (at_terminator(state, terminators, terminator_count, stop_at_rparen) ||
        current_token(state)->kind == GSH_TOKEN_EOF) {
        fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                        ? GSH_PARSE_INCOMPLETE
                        : GSH_PARSE_SYNTAX);
        return GSH_AST_NONE;
    }
    node = new_node(state, GSH_AST_LIST, current_token(state)->begin);
    while (state->result.status == GSH_PARSE_OK) {
        size_t command = parse_and_or(state);
        bool separated = false;

        if (command == GSH_AST_NONE || !append_child(state, node, command)) {
            return GSH_AST_NONE;
        }
        if (current_token(state)->kind == GSH_TOKEN_AMPERSAND ||
            current_token(state)->kind == GSH_TOKEN_SEMICOLON) {
            if (current_token(state)->kind == GSH_TOKEN_AMPERSAND) {
                state->storage->nodes[command].flags |= GSH_AST_FLAG_ASYNC;
            }
            state->position++;
            separated = true;
            skip_linebreak(state);
        } else if (current_token(state)->kind == GSH_TOKEN_NEWLINE) {
            skip_linebreak(state);
            separated = true;
        }
        if (at_terminator(state, terminators, terminator_count,
                          stop_at_rparen) ||
            current_token(state)->kind == GSH_TOKEN_EOF) {
            break;
        }
        if (!separated) {
            fail(state, GSH_PARSE_SYNTAX);
            return GSH_AST_NONE;
        }
    }
    return node;
}

typedef struct {
    gsh_word_ref delimiter;
    bool strip_tabs;
} pending_heredoc;

static bool heredoc_byte(char output[GSH_HEREDOC_DELIMITER_CAP],
                         size_t *length, unsigned char byte)
{
    if (*length == GSH_HEREDOC_DELIMITER_CAP) {
        return false;
    }
    output[(*length)++] = (char)byte;
    return true;
}

static int heredoc_hexadecimal(unsigned char byte)
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

static bool build_heredoc_delimiter(
    const parser *state, gsh_word_ref reference,
    char output[GSH_HEREDOC_DELIMITER_CAP], size_t *length, bool *quoted)
{
    enum {
        HEREDOC_QUOTE_NONE,
        HEREDOC_QUOTE_SINGLE,
        HEREDOC_QUOTE_DOUBLE,
        HEREDOC_QUOTE_DOLLAR,
    } quote = HEREDOC_QUOTE_NONE;
    size_t offset = reference.begin;

    *length = 0;
    *quoted = false;
    while (offset < reference.end) {
        unsigned char byte = state->input[offset++];

        if (quote == HEREDOC_QUOTE_SINGLE) {
            if (byte == '\'') {
                quote = HEREDOC_QUOTE_NONE;
            } else if (!heredoc_byte(output, length, byte)) {
                return false;
            }
            continue;
        }
        if (quote == HEREDOC_QUOTE_DOUBLE) {
            if (byte == '"') {
                quote = HEREDOC_QUOTE_NONE;
                continue;
            }
            if (byte == '\\' && offset < reference.end) {
                unsigned char next = state->input[offset];

                if (next == '$' || next == 0x60U || next == '"' ||
                    next == '\\' || next == '\n') {
                    offset++;
                    if (next == '\n') {
                        continue;
                    }
                    byte = next;
                }
            }
            if (!heredoc_byte(output, length, byte)) {
                return false;
            }
            continue;
        }
        if (quote == HEREDOC_QUOTE_DOLLAR) {
            if (byte == '\'') {
                quote = HEREDOC_QUOTE_NONE;
                continue;
            }
            if (byte == '\\') {
                unsigned int value;
                unsigned char escape;

                if (offset == reference.end) {
                    return false;
                }
                escape = state->input[offset++];
                if (escape == 'n') {
                    byte = '\n';
                } else if (escape == 'r') {
                    byte = '\r';
                } else if (escape == 't') {
                    byte = '\t';
                } else if (escape == 'a') {
                    byte = '\a';
                } else if (escape == 'b') {
                    byte = '\b';
                } else if (escape == 'e') {
                    byte = 0x1bU;
                } else if (escape == 'f') {
                    byte = '\f';
                } else if (escape == 'v') {
                    byte = '\v';
                } else if (escape == '\\' || escape == '\'' ||
                           escape == '"') {
                    byte = escape;
                } else if (escape == 'x') {
                    size_t digits = 0;
                    int digit;

                    value = 0;
                    while (offset < reference.end && digits < 2U &&
                           (digit = heredoc_hexadecimal(
                                state->input[offset])) >= 0) {
                        value = value * 16U + (unsigned int)digit;
                        offset++;
                        digits++;
                    }
                    if (digits == 0 || value == 0) {
                        return false;
                    }
                    byte = (unsigned char)value;
                } else if (escape >= '0' && escape <= '7') {
                    size_t digits = 1;

                    value = (unsigned int)(escape - '0');
                    while (offset < reference.end && digits < 3U &&
                           state->input[offset] >= '0' &&
                           state->input[offset] <= '7') {
                        value = value * 8U +
                                (unsigned int)(state->input[offset] - '0');
                        offset++;
                        digits++;
                    }
                    if (value == 0 || value > 255U) {
                        return false;
                    }
                    byte = (unsigned char)value;
                } else {
                    return false;
                }
            }
            if (!heredoc_byte(output, length, byte)) {
                return false;
            }
            continue;
        }

        if (byte == '\'') {
            *quoted = true;
            quote = HEREDOC_QUOTE_SINGLE;
        } else if (byte == '"') {
            *quoted = true;
            quote = HEREDOC_QUOTE_DOUBLE;
        } else if (byte == '\\') {
            *quoted = true;
            if (offset == reference.end) {
                return false;
            }
            byte = state->input[offset++];
            if (byte != '\n' && !heredoc_byte(output, length, byte)) {
                return false;
            }
        } else if (byte == '$' && offset < reference.end &&
                   state->input[offset] == '\'') {
            *quoted = true;
            offset++;
            quote = HEREDOC_QUOTE_DOLLAR;
        } else if (!heredoc_byte(output, length, byte)) {
            return false;
        }
    }
    return quote == HEREDOC_QUOTE_NONE;
}

static void advance_lexer_to(gsh_lexer *lexer, size_t new_offset)
{
    while (lexer->offset < new_offset) {
        if (lexer->input[lexer->offset] == '\n') {
            lexer->line++;
            lexer->column = 1;
        } else {
            lexer->column++;
        }
        lexer->offset++;
    }
}

static gsh_parse_status collect_heredocs(
    parser *state, gsh_lexer *lexer, const pending_heredoc *pending,
    size_t pending_count)
{
    size_t pending_index;

    for (pending_index = 0; pending_index < pending_count; pending_index++) {
        char delimiter[GSH_HEREDOC_DELIMITER_CAP];
        char comparison[GSH_HEREDOC_DELIMITER_CAP];
        size_t delimiter_length;
        bool quoted;
        size_t body_begin = lexer->offset;
        bool found = false;

        if (!build_heredoc_delimiter(state, pending[pending_index].delimiter,
                                     delimiter, &delimiter_length, &quoted)) {
            return GSH_PARSE_LIMIT;
        }
        while (lexer->offset <= lexer->length) {
            size_t line_begin = lexer->offset;
            size_t scan = line_begin;
            size_t comparison_length = 0;
            size_t after_line;
            bool comparison_overflow = false;

            for (;;) {
                size_t segment_begin = scan;
                size_t segment_end;
                size_t trailing_backslashes = 0;
                bool continuation;
                size_t copy_end;

                if (pending[pending_index].strip_tabs &&
                    comparison_length == 0 && !comparison_overflow) {
                    while (segment_begin < lexer->length &&
                           lexer->input[segment_begin] == '\t') {
                        segment_begin++;
                    }
                }
                segment_end = segment_begin;
                while (segment_end < lexer->length &&
                       lexer->input[segment_end] != '\n') {
                    segment_end++;
                }
                if (!quoted && segment_end < lexer->length) {
                    size_t cursor = segment_end;

                    while (cursor > segment_begin &&
                           lexer->input[cursor - 1U] == '\\') {
                        cursor--;
                        trailing_backslashes++;
                    }
                }
                continuation = !quoted && segment_end < lexer->length &&
                               trailing_backslashes % 2U != 0;
                copy_end = continuation ? segment_end - 1U : segment_end;
                if (!comparison_overflow) {
                    size_t segment_length = copy_end - segment_begin;

                    if (segment_length > sizeof(comparison) -
                                             comparison_length) {
                        comparison_overflow = true;
                    } else {
                        memcpy(comparison + comparison_length,
                               lexer->input + segment_begin,
                               segment_length);
                        comparison_length += segment_length;
                    }
                }
                after_line = segment_end < lexer->length
                                 ? segment_end + 1U
                                 : segment_end;
                if (!continuation) {
                    break;
                }
                scan = after_line;
            }
            if (!comparison_overflow &&
                comparison_length == delimiter_length &&
                memcmp(comparison, delimiter, delimiter_length) == 0) {
                gsh_heredoc *heredoc;

                if (state->storage->heredoc_count ==
                    GSH_PARSE_REDIRECT_CAP) {
                    return GSH_PARSE_LIMIT;
                }
                heredoc = &state->storage
                               ->heredocs[state->storage->heredoc_count++];
                heredoc->delimiter = pending[pending_index].delimiter;
                heredoc->body.begin = body_begin;
                heredoc->body.end = line_begin;
                heredoc->flags = GSH_REDIRECT_HEREDOC |
                                 (pending[pending_index].strip_tabs
                                      ? GSH_REDIRECT_HEREDOC_STRIP_TABS
                                      : 0U) |
                                 (quoted ? GSH_REDIRECT_HEREDOC_QUOTED : 0U);
                advance_lexer_to(lexer, after_line);
                found = true;
                break;
            }
            if (after_line == lexer->length) {
                break;
            }
            advance_lexer_to(lexer, after_line);
        }
        if (!found) {
            state->result.error_offset = lexer->offset;
            return GSH_PARSE_INCOMPLETE;
        }
    }
    return GSH_PARSE_OK;
}

static gsh_parse_status tokenize(parser *state)
{
    gsh_lexer lexer;
    pending_heredoc pending[GSH_PARSE_REDIRECT_CAP];
    size_t pending_count = 0;
    bool awaiting_heredoc_delimiter = false;
    bool pending_strip_tabs = false;

    gsh_lexer_init(&lexer, state->input, state->length);
    for (;;) {
        gsh_lex_status status;
        gsh_token *token;

        if (state->storage->token_count == GSH_PARSE_TOKEN_CAP) {
            state->result.error_offset = lexer.offset;
            return GSH_PARSE_LIMIT;
        }
        token = &state->storage->tokens[state->storage->token_count];
        status = gsh_lexer_next(&lexer, token);
        if (status != GSH_LEX_OK) {
            state->result.error_offset = lexer.error_offset;
            if (status == GSH_LEX_INCOMPLETE) {
                return GSH_PARSE_INCOMPLETE;
            }
            if (status == GSH_LEX_LIMIT) {
                return GSH_PARSE_LIMIT;
            }
            return GSH_PARSE_LEXICAL;
        }
        state->storage->token_count++;
        if (awaiting_heredoc_delimiter) {
            if (token->kind != GSH_TOKEN_WORD ||
                pending_count == GSH_PARSE_REDIRECT_CAP) {
                state->result.error_offset = token->begin;
                return token->kind == GSH_TOKEN_EOF ? GSH_PARSE_INCOMPLETE
                                                    : GSH_PARSE_SYNTAX;
            }
            pending[pending_count].delimiter.begin = token->begin;
            pending[pending_count].delimiter.end = token->end;
            pending[pending_count].strip_tabs = pending_strip_tabs;
            pending_count++;
            awaiting_heredoc_delimiter = false;
        } else if (token->kind == GSH_TOKEN_DLESS ||
                   token->kind == GSH_TOKEN_DLESSDASH) {
            awaiting_heredoc_delimiter = true;
            pending_strip_tabs = token->kind == GSH_TOKEN_DLESSDASH;
        }
        if (token->kind == GSH_TOKEN_NEWLINE && pending_count > 0 &&
            !awaiting_heredoc_delimiter) {
            gsh_parse_status collected = collect_heredocs(
                state, &lexer, pending, pending_count);

            if (collected != GSH_PARSE_OK) {
                return collected;
            }
            pending_count = 0;
        }
        if (token->kind == GSH_TOKEN_EOF) {
            if (awaiting_heredoc_delimiter || pending_count > 0) {
                return GSH_PARSE_INCOMPLETE;
            }
            return GSH_PARSE_OK;
        }
    }
}

static gsh_parse_result parse_input(
    const void *input, size_t length, gsh_parse_storage *storage,
    gsh_parse_command_probe_fn probe, void *opaque,
    gsh_word_ref *candidate)
{
    parser state;
    size_t program;
    size_t body;

    storage->token_count = 0;
    storage->node_count = 0;
    storage->word_count = 0;
    storage->redirect_count = 0;
    storage->heredoc_count = 0;
    memset(&state, 0, sizeof(state));
    state.input = input;
    state.length = length;
    state.storage = storage;
    state.command_probe = probe;
    state.command_probe_opaque = opaque;
    state.command_candidate = candidate;
    state.result.status = GSH_PARSE_OK;
    state.result.root = GSH_AST_NONE;
    state.result.unexpected = GSH_TOKEN_EOF;
    state.result.status = tokenize(&state);
    if (state.result.status != GSH_PARSE_OK) {
        return state.result;
    }

    program = new_node(&state, GSH_AST_PROGRAM, 0);
    if (program == GSH_AST_NONE) {
        return state.result;
    }
    skip_linebreak(&state);
    if (current_token(&state)->kind == GSH_TOKEN_EOF) {
        storage->nodes[program].end = length;
        state.result.root = program;
        return state.result;
    }
    body = parse_list(&state, NULL, 0, false);
    if (body == GSH_AST_NONE || !append_child(&state, program, body)) {
        return state.result;
    }
    skip_linebreak(&state);
    if (current_token(&state)->kind != GSH_TOKEN_EOF) {
        fail(&state, GSH_PARSE_SYNTAX);
        return state.result;
    }
    storage->nodes[program].end = length;
    state.result.root = program;
    return state.result;
}

gsh_parse_result gsh_parse(const void *input, size_t length,
                           gsh_parse_storage *storage)
{
    return parse_input(input, length, storage, NULL, NULL, NULL);
}

gsh_parse_result gsh_parse_command_probe(
    const void *input, size_t length, gsh_parse_storage *storage,
    gsh_parse_command_probe_fn probe, void *opaque,
    gsh_word_ref *candidate)
{
    if (probe == NULL || candidate == NULL) {
        return gsh_parse(input, length, storage);
    }
    candidate->begin = 0;
    candidate->end = 0;
    return parse_input(input, length, storage, probe, opaque, candidate);
}

const char *gsh_parse_status_name(gsh_parse_status status)
{
    static const char *const names[] = {
        "ok", "lexical", "syntax", "incomplete", "limit", "unsupported",
        "rewrite",
    };

    if ((size_t)status >= sizeof(names) / sizeof(names[0])) {
        return "invalid";
    }
    return names[status];
}
