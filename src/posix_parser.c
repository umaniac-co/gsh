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

#include <string.h>

#define require(condition) (condition)

typedef struct {
    const unsigned char *input;
    size_t length;
    gsh_parse_storage *storage;
    size_t position;
    gsh_parse_result result;
} parser;

enum {
    PARSE_FRAME_CAP = GSH_PARSE_NODE_CAP,
    PARSE_MACHINE_STEP_CAP = GSH_PARSE_TOKEN_CAP * 64,
    PARSE_INPUT_STEP_CAP = 1024 * 1024 + 1,
};

typedef enum {
    PARSE_FRAME_NONE,
    PARSE_FRAME_LIST,
    PARSE_FRAME_AND_OR,
    PARSE_FRAME_PIPELINE,
    PARSE_FRAME_COMMAND,
    PARSE_FRAME_SUBSHELL,
    PARSE_FRAME_BRACE,
    PARSE_FRAME_WHILE,
    PARSE_FRAME_UNTIL,
    PARSE_FRAME_IF,
    PARSE_FRAME_FOR,
    PARSE_FRAME_CASE,
    PARSE_FRAME_FUNCTION,
} parse_frame_kind;

enum {
    PARSE_STOP_RPAREN = 1U << 0,
    PARSE_STOP_RBRACE = 1U << 1,
    PARSE_STOP_DO = 1U << 2,
    PARSE_STOP_DONE = 1U << 3,
    PARSE_STOP_THEN = 1U << 4,
    PARSE_STOP_ELIF = 1U << 5,
    PARSE_STOP_ELSE = 1U << 6,
    PARSE_STOP_FI = 1U << 7,
    PARSE_STOP_ESAC = 1U << 8,
    PARSE_STOP_CASE_ITEM = 1U << 9,
};

typedef struct {
    parse_frame_kind kind;
    unsigned int stops;
    unsigned int phase;
    size_t node;
    size_t saved;
    size_t begin;
    gsh_token_kind connector;
} parse_frame;

typedef struct {
    parser *state;
    parse_frame frames[PARSE_FRAME_CAP];
    size_t depth;
    size_t value;
    bool returned;
} parse_machine;

static size_t parse_syntax(parser *state);

static gsh_parse_storage *parser_storage(const parser *state)
{
    if (!require(state != NULL)) return NULL;
    if (!require(state->storage != NULL)) return NULL;
    return state->storage;
}

static const gsh_token *current_token(const parser *state)
{
    if (state == NULL) {
        return NULL;
    }
    return &parser_storage(state)->tokens[state->position];
}

static const gsh_token *token_at(const parser *state, size_t lookahead)
{
    if (state == NULL) {
        return NULL;
    }
    size_t position = state->position + lookahead;

    if (position >= parser_storage(state)->token_count) {
        position = parser_storage(state)->token_count - 1U;
    }
    return &parser_storage(state)->tokens[position];
}

static void fail(parser *state, gsh_parse_status status)
{
    if (state == NULL) return;
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
    if (state == NULL || token == NULL || word == NULL) {
        return false;
    }
    size_t length = strlen(word);

    return token->kind == GSH_TOKEN_WORD &&
           token->end - token->begin == length &&
           memcmp(state->input + token->begin, word, length) == 0;
}

static bool current_word_is(const parser *state, const char *word)
{
    if (state == NULL || word == NULL) {
        return false;
    }
    return raw_word_equals(state, current_token(state), word);
}

static bool is_name_span(const parser *state, const gsh_token *token)
{
    if (token == NULL || state == NULL) return false;
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
    if (token == NULL || state == NULL) return false;
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
    if (token == NULL || state == NULL) return false;
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

static bool record_command_word(parser *state, const gsh_token *token)
{
    if (token == NULL) return false;
    gsh_word_ref *word;

    if (token->kind != GSH_TOKEN_WORD) {
        return false;
    }
    if (parser_storage(state)->command_word_count == GSH_PARSE_NODE_CAP) {
        fail(state, GSH_PARSE_LIMIT);
        return false;
    }
    word = &parser_storage(state)
                ->command_words[parser_storage(state)->command_word_count++];
    word->begin = token->begin;
    word->end = token->end;
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

    if (parser_storage(state)->node_count == GSH_PARSE_NODE_CAP) {
        fail(state, GSH_PARSE_LIMIT);
        return GSH_AST_NONE;
    }
    index = parser_storage(state)->node_count++;
    node = &parser_storage(state)->nodes[index];
    (void)memset(node, 0, sizeof(*node));
    node->kind = kind;
    node->connector = GSH_TOKEN_EOF;
    node->first_child = GSH_AST_NONE;
    node->last_child = GSH_AST_NONE;
    node->next_sibling = GSH_AST_NONE;
    node->first_word = parser_storage(state)->word_count;
    node->first_redirect = parser_storage(state)->redirect_count;
    node->begin = begin;
    node->end = begin;
    return index;
}

static bool append_child(parser *state, size_t parent_index,
                         size_t child_index)
{
    if (state == NULL) {
        return false;
    }
    gsh_ast_node *parent;

    if (parent_index == GSH_AST_NONE || child_index == GSH_AST_NONE) {
        return false;
    }
    parent = &parser_storage(state)->nodes[parent_index];
    if (parent->first_child == GSH_AST_NONE) {
        parent->first_child = child_index;
    } else {
        parser_storage(state)->nodes[parent->last_child].next_sibling = child_index;
    }
    parent->last_child = child_index;
    parent->end = parser_storage(state)->nodes[child_index].end;
    return true;
}

static bool append_word(parser *state, size_t node_index,
                        const gsh_token *token)
{
    if (token == NULL) {
        return false;
    }
    gsh_ast_node *node;

    if (parser_storage(state)->word_count == GSH_PARSE_WORD_CAP) {
        fail(state, GSH_PARSE_LIMIT);
        return false;
    }
    node = &parser_storage(state)->nodes[node_index];
    parser_storage(state)->words[parser_storage(state)->word_count].begin = token->begin;
    parser_storage(state)->words[parser_storage(state)->word_count].end = token->end;
    parser_storage(state)->word_count++;
    node->word_count++;
    node->end = token->end;
    return true;
}

static int descriptor_from_token(const parser *state, const gsh_token *token)
{
    if (state == NULL || token == NULL) {
        return -1;
    }
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
    if (target == NULL) {
        return false;
    }
    gsh_ast_node *node;
    gsh_redirect *redirect;

    if (parser_storage(state)->redirect_count == GSH_PARSE_REDIRECT_CAP) {
        fail(state, GSH_PARSE_LIMIT);
        return false;
    }
    node = &parser_storage(state)->nodes[node_index];
    redirect = &parser_storage(state)
                    ->redirects[parser_storage(state)->redirect_count++];
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

        for (heredoc = 0; heredoc < parser_storage(state)->heredoc_count;
             heredoc++) {
            const gsh_heredoc *source = &parser_storage(state)->heredocs[heredoc];

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
    if (state == NULL) {
        return;
    }
    while (current_token(state)->kind == GSH_TOKEN_NEWLINE) {
        state->position++;
    }
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
    if (state == NULL) {
        return 0U;
    }
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
            if (!record_command_word(state, current_token(state))) {
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

static size_t new_if_branch(parser *state, size_t begin, size_t condition,
                            size_t body)
{
    if (state == NULL) {
        return 0U;
    }
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

static bool redirect_starts(const parser *state)
{
    if (!require(state != NULL && state->storage != NULL)) return false;
    return is_redirect_operator(current_token(state)->kind) ||
           (is_digit_span(state, current_token(state)) &&
            is_redirect_operator(token_at(state, 1)->kind) &&
            current_token(state)->end == token_at(state, 1)->begin);
}

static parse_frame_kind compound_frame_kind(const parser *state)
{
    if (!require(state != NULL && state->storage != NULL)) {
        return PARSE_FRAME_NONE;
    }
    if (current_token(state)->kind == GSH_TOKEN_LPAREN) {
        return PARSE_FRAME_SUBSHELL;
    }
    if (current_word_is(state, "{")) return PARSE_FRAME_BRACE;
    if (current_word_is(state, "if")) return PARSE_FRAME_IF;
    if (current_word_is(state, "while")) return PARSE_FRAME_WHILE;
    if (current_word_is(state, "until")) return PARSE_FRAME_UNTIL;
    if (current_word_is(state, "for")) return PARSE_FRAME_FOR;
    if (current_word_is(state, "case")) return PARSE_FRAME_CASE;
    return PARSE_FRAME_NONE;
}

static bool list_stops(const parser *state, unsigned int stops)
{
    if (!require(state != NULL && state->storage != NULL)) return false;
    gsh_token_kind kind = current_token(state)->kind;

    if ((stops & PARSE_STOP_RPAREN) != 0U && kind == GSH_TOKEN_RPAREN) {
        return true;
    }
    if ((stops & PARSE_STOP_CASE_ITEM) != 0U &&
        (kind == GSH_TOKEN_DSEMI || kind == GSH_TOKEN_SEMI_AND)) {
        return true;
    }
    return ((stops & PARSE_STOP_RBRACE) != 0U &&
            current_word_is(state, "}")) ||
           ((stops & PARSE_STOP_DO) != 0U && current_word_is(state, "do")) ||
           ((stops & PARSE_STOP_DONE) != 0U &&
            current_word_is(state, "done")) ||
           ((stops & PARSE_STOP_THEN) != 0U &&
            current_word_is(state, "then")) ||
           ((stops & PARSE_STOP_ELIF) != 0U &&
            current_word_is(state, "elif")) ||
           ((stops & PARSE_STOP_ELSE) != 0U &&
            current_word_is(state, "else")) ||
           ((stops & PARSE_STOP_FI) != 0U && current_word_is(state, "fi")) ||
           ((stops & (PARSE_STOP_ESAC | PARSE_STOP_CASE_ITEM)) != 0U &&
            current_word_is(state, "esac"));
}

static bool machine_push(parse_machine *machine, parse_frame_kind kind,
                         unsigned int stops)
{
    parse_frame *frame;

    if (!require(machine != NULL && machine->state != NULL)) return false;
    if (!require(kind > PARSE_FRAME_NONE && kind <= PARSE_FRAME_FUNCTION) ||
        machine->depth >= PARSE_FRAME_CAP || machine->returned) {
        fail(machine->state, GSH_PARSE_LIMIT);
        return false;
    }
    frame = &machine->frames[machine->depth++];
    (void)memset(frame, 0, sizeof(*frame));
    frame->kind = kind;
    frame->stops = stops;
    frame->node = GSH_AST_NONE;
    frame->saved = GSH_AST_NONE;
    return true;
}

static void machine_complete(parse_machine *machine, size_t value)
{
    if (machine == NULL) {
        return;
    }
    machine->depth--;
    machine->value = value;
    machine->returned = true;
}

static bool machine_take(parse_machine *machine, size_t *value)
{
    if (!require(machine != NULL && value != NULL &&
                 machine->state != NULL)) return false;
    if (!require(machine->depth < PARSE_FRAME_CAP) || !machine->returned) {
        fail(machine->state, GSH_PARSE_SYNTAX);
        return false;
    }
    *value = machine->value;
    machine->returned = false;
    return *value != GSH_AST_NONE;
}

/* ── Grammar Nesting Uses Explicit, Bounded Frames ──────────────
 * Shell grammar nesting previously consumed the C call stack through the
 * list-to-command-to-compound cycle.  One fixed pushdown machine now stores
 * every suspended production in static parser storage.  Each token can cause
 * only a fixed number of transitions, and both frame and transition ceilings
 * fail as GSH_PARSE_LIMIT instead of exhausting the process stack.
 * ─────────────────────────────────────────────────────────────── */
static bool step_list(parse_machine *machine, parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->state != NULL)) return false;
    parser *state = machine->state;
    size_t child;
    bool separated = false;

    if (frame->phase == 0U) {
        skip_linebreak(state);
        if (list_stops(state, frame->stops) ||
            current_token(state)->kind == GSH_TOKEN_EOF) {
            fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                            ? GSH_PARSE_INCOMPLETE
                            : GSH_PARSE_SYNTAX);
            return false;
        }
        frame->node = new_node(state, GSH_AST_LIST,
                               current_token(state)->begin);
        frame->phase = 1U;
        return frame->node != GSH_AST_NONE &&
               machine_push(machine, PARSE_FRAME_AND_OR, 0U);
    }
    if (!machine_take(machine, &child) ||
        !append_child(state, frame->node, child)) return false;
    if (current_token(state)->kind == GSH_TOKEN_AMPERSAND ||
        current_token(state)->kind == GSH_TOKEN_SEMICOLON) {
        if (current_token(state)->kind == GSH_TOKEN_AMPERSAND) {
            parser_storage(state)->nodes[child].flags |= GSH_AST_FLAG_ASYNC;
        }
        state->position++;
        separated = true;
        skip_linebreak(state);
    } else if (current_token(state)->kind == GSH_TOKEN_NEWLINE) {
        skip_linebreak(state);
        separated = true;
    }
    if (list_stops(state, frame->stops)) {
        machine_complete(machine, frame->node);
        return true;
    }
    if (current_token(state)->kind == GSH_TOKEN_EOF) {
        if ((frame->stops & PARSE_STOP_CASE_ITEM) != 0U) {
            fail(state, GSH_PARSE_INCOMPLETE);
            return false;
        }
        machine_complete(machine, frame->node);
        return true;
    }
    if (!separated) {
        fail(state, GSH_PARSE_SYNTAX);
        return false;
    }
    return machine_push(machine, PARSE_FRAME_AND_OR, 0U);
}

static bool step_and_or(parse_machine *machine, parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->state != NULL)) return false;
    parser *state = machine->state;
    size_t pipeline;

    if (frame->phase == 0U) {
        frame->node = new_node(state, GSH_AST_AND_OR,
                               current_token(state)->begin);
        frame->phase = 1U;
        return frame->node != GSH_AST_NONE &&
               machine_push(machine, PARSE_FRAME_PIPELINE, 0U);
    }
    if (!machine_take(machine, &pipeline)) return false;
    if (frame->phase == 2U) {
        parser_storage(state)->nodes[pipeline].connector = frame->connector;
    }
    if (!append_child(state, frame->node, pipeline)) return false;
    if (current_token(state)->kind != GSH_TOKEN_AND_IF &&
        current_token(state)->kind != GSH_TOKEN_OR_IF) {
        machine_complete(machine, frame->node);
        return true;
    }
    frame->connector = current_token(state)->kind;
    frame->phase = 2U;
    state->position++;
    skip_linebreak(state);
    return machine_push(machine, PARSE_FRAME_PIPELINE, 0U);
}

static bool step_pipeline(parse_machine *machine, parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->state != NULL)) return false;
    parser *state = machine->state;
    size_t command;

    if (frame->phase == 0U) {
        frame->node = new_node(state, GSH_AST_PIPELINE,
                               current_token(state)->begin);
        if (frame->node == GSH_AST_NONE) return false;
        if (current_word_is(state, "!")) {
            parser_storage(state)->nodes[frame->node].flags |= GSH_AST_FLAG_NEGATED;
            state->position++;
            skip_linebreak(state);
        }
        frame->phase = 1U;
        return machine_push(machine, PARSE_FRAME_COMMAND, 0U);
    }
    if (!machine_take(machine, &command) ||
        !append_child(state, frame->node, command)) return false;
    if (current_token(state)->kind != GSH_TOKEN_PIPE) {
        machine_complete(machine, frame->node);
        return true;
    }
    state->position++;
    skip_linebreak(state);
    return machine_push(machine, PARSE_FRAME_COMMAND, 0U);
}

static bool command_is_function(const parser *state)
{
    if (!require(state != NULL && state->storage != NULL)) return false;
    return current_token(state)->kind == GSH_TOKEN_WORD &&
           is_name_span(state, current_token(state)) &&
           token_at(state, 1)->kind == GSH_TOKEN_LPAREN &&
           token_at(state, 2)->kind == GSH_TOKEN_RPAREN;
}

static bool step_command(parse_machine *machine, parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->state != NULL)) return false;
    parser *state = machine->state;
    parse_frame_kind kind;
    size_t node;

    if (frame->phase == 0U) {
        kind = command_is_function(state) ? PARSE_FRAME_FUNCTION
                                          : compound_frame_kind(state);
        if (kind == PARSE_FRAME_NONE) {
            if (is_any_reserved(state, current_token(state))) {
                fail(state, GSH_PARSE_SYNTAX);
                return false;
            }
            node = parse_simple(state);
            machine_complete(machine, node);
            return node != GSH_AST_NONE;
        }
        frame->phase = 1U;
        return machine_push(machine, kind, 0U);
    }
    if (!machine_take(machine, &node)) return false;
    while (redirect_starts(state)) {
        if (!parse_one_redirect(state, node)) return false;
    }
    machine_complete(machine, node);
    return true;
}

static bool step_group(parse_machine *machine, parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->state != NULL)) return false;
    parser *state = machine->state;
    bool subshell = frame->kind == PARSE_FRAME_SUBSHELL;
    size_t body;

    if (frame->phase == 0U) {
        frame->node = new_node(state,
                               subshell ? GSH_AST_SUBSHELL
                                        : GSH_AST_BRACE_GROUP,
                               current_token(state)->begin);
        state->position++;
        frame->phase = 1U;
        return frame->node != GSH_AST_NONE && machine_push(
            machine, PARSE_FRAME_LIST,
            subshell ? PARSE_STOP_RPAREN : PARSE_STOP_RBRACE);
    }
    if (!machine_take(machine, &body) ||
        !append_child(state, frame->node, body)) return false;
    if (subshell) {
        if (current_token(state)->kind != GSH_TOKEN_RPAREN) {
            fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                            ? GSH_PARSE_INCOMPLETE
                            : GSH_PARSE_SYNTAX);
            return false;
        }
        parser_storage(state)->nodes[frame->node].end = current_token(state)->end;
        state->position++;
    } else if (!expect_word(state, "}")) {
        return false;
    } else {
        parser_storage(state)->nodes[frame->node].end =
            parser_storage(state)->tokens[state->position - 1U].end;
    }
    machine_complete(machine, frame->node);
    return true;
}

static bool step_loop(parse_machine *machine, parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->state != NULL)) return false;
    parser *state = machine->state;
    bool until = frame->kind == PARSE_FRAME_UNTIL;
    size_t child;

    if (frame->phase == 0U) {
        frame->node = new_node(state, until ? GSH_AST_UNTIL : GSH_AST_WHILE,
                               current_token(state)->begin);
        state->position++;
        frame->phase = 1U;
        return frame->node != GSH_AST_NONE &&
               machine_push(machine, PARSE_FRAME_LIST, PARSE_STOP_DO);
    }
    if (!machine_take(machine, &child) ||
        !append_child(state, frame->node, child)) return false;
    if (frame->phase == 1U) {
        if (!expect_word(state, "do")) return false;
        frame->phase = 2U;
        return machine_push(machine, PARSE_FRAME_LIST, PARSE_STOP_DONE);
    }
    if (!expect_word(state, "done")) return false;
    parser_storage(state)->nodes[frame->node].end =
        parser_storage(state)->tokens[state->position - 1U].end;
    machine_complete(machine, frame->node);
    return true;
}

static bool append_if_branch(parser *state, parse_frame *frame, size_t body)
{
    if (!require(state != NULL && frame != NULL &&
                 frame->node != GSH_AST_NONE)) return false;
    size_t branch = new_if_branch(state, frame->begin, frame->saved, body);

    return branch != GSH_AST_NONE && append_child(state, frame->node, branch);
}

static bool step_if(parse_machine *machine, parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->state != NULL)) return false;
    parser *state = machine->state;
    size_t child;

    if (frame->phase == 0U) {
        frame->node = new_node(state, GSH_AST_IF,
                               current_token(state)->begin);
        state->position++;
        frame->begin = current_token(state)->begin;
        frame->phase = 1U;
        return frame->node != GSH_AST_NONE &&
               machine_push(machine, PARSE_FRAME_LIST, PARSE_STOP_THEN);
    }
    if (!machine_take(machine, &child)) return false;
    if (frame->phase == 1U) {
        frame->saved = child;
        if (!expect_word(state, "then")) return false;
        frame->phase = 2U;
        return machine_push(machine, PARSE_FRAME_LIST,
                            PARSE_STOP_ELIF | PARSE_STOP_ELSE |
                                PARSE_STOP_FI);
    }
    if (frame->phase == 3U) {
        frame->saved = GSH_AST_NONE;
        if (!append_if_branch(state, frame, child) ||
            !expect_word(state, "fi")) return false;
        parser_storage(state)->nodes[frame->node].end =
            parser_storage(state)->tokens[state->position - 1U].end;
        machine_complete(machine, frame->node);
        return true;
    }
    if (!append_if_branch(state, frame, child)) return false;
    if (current_word_is(state, "elif")) {
        state->position++;
        frame->begin = current_token(state)->begin;
        frame->phase = 1U;
        return machine_push(machine, PARSE_FRAME_LIST, PARSE_STOP_THEN);
    }
    if (current_word_is(state, "else")) {
        frame->begin = current_token(state)->begin;
        state->position++;
        frame->phase = 3U;
        return machine_push(machine, PARSE_FRAME_LIST, PARSE_STOP_FI);
    }
    if (!expect_word(state, "fi")) return false;
    parser_storage(state)->nodes[frame->node].end =
        parser_storage(state)->tokens[state->position - 1U].end;
    machine_complete(machine, frame->node);
    return true;
}

static bool parse_for_header(parser *state, size_t node)
{
    if (!require(state != NULL && state->storage != NULL &&
                 node < parser_storage(state)->node_count)) return false;
    bool has_in = false;
    bool separated = false;
    size_t words;

    state->position++;
    if (!is_name_span(state, current_token(state)) ||
        !append_word(state, node, current_token(state))) {
        fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                        ? GSH_PARSE_INCOMPLETE
                        : GSH_PARSE_SYNTAX);
        return false;
    }
    state->position++;
    skip_linebreak(state);
    if (current_word_is(state, "in")) {
        has_in = true;
        parser_storage(state)->nodes[node].flags |= GSH_AST_FLAG_FOR_HAS_IN;
        state->position++;
        for (words = 0U; words < GSH_PARSE_TOKEN_CAP &&
                         current_token(state)->kind == GSH_TOKEN_WORD &&
                         !current_word_is(state, "do");
             words++) {
            if (!append_word(state, node, current_token(state))) return false;
            state->position++;
        }
    }
    if (current_token(state)->kind == GSH_TOKEN_SEMICOLON) {
        state->position++;
        separated = true;
        skip_linebreak(state);
    } else if (current_token(state)->kind == GSH_TOKEN_NEWLINE) {
        skip_linebreak(state);
        separated = true;
    }
    if (has_in && !separated) {
        fail(state, GSH_PARSE_SYNTAX);
        return false;
    }
    return expect_word(state, "do");
}

static bool step_for(parse_machine *machine, parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->state != NULL)) return false;
    parser *state = machine->state;
    size_t body;

    if (frame->phase == 0U) {
        frame->node = new_node(state, GSH_AST_FOR,
                               current_token(state)->begin);
        if (frame->node == GSH_AST_NONE ||
            !parse_for_header(state, frame->node)) return false;
        frame->phase = 1U;
        return machine_push(machine, PARSE_FRAME_LIST, PARSE_STOP_DONE);
    }
    if (!machine_take(machine, &body) ||
        !append_child(state, frame->node, body) ||
        !expect_word(state, "done")) return false;
    parser_storage(state)->nodes[frame->node].end =
        parser_storage(state)->tokens[state->position - 1U].end;
    machine_complete(machine, frame->node);
    return true;
}

static bool parse_case_header(parser *state, parse_frame *frame)
{
    if (!require(state != NULL && frame != NULL &&
                 state->storage != NULL)) return false;
    frame->node = new_node(state, GSH_AST_CASE,
                           current_token(state)->begin);
    state->position++;
    if (frame->node == GSH_AST_NONE ||
        current_token(state)->kind != GSH_TOKEN_WORD ||
        !append_word(state, frame->node, current_token(state))) {
        fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                        ? GSH_PARSE_INCOMPLETE
                        : GSH_PARSE_SYNTAX);
        return false;
    }
    state->position++;
    skip_linebreak(state);
    if (!expect_word(state, "in")) return false;
    skip_linebreak(state);
    return true;
}

static bool parse_case_patterns(parser *state, size_t item)
{
    if (!require(state != NULL && state->storage != NULL &&
                 item < parser_storage(state)->node_count)) return false;
    size_t patterns;

    if (current_token(state)->kind == GSH_TOKEN_LPAREN) state->position++;
    if (current_token(state)->kind != GSH_TOKEN_WORD) {
        fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                        ? GSH_PARSE_INCOMPLETE
                        : GSH_PARSE_SYNTAX);
        return false;
    }
    for (patterns = 0U; patterns < GSH_PARSE_TOKEN_CAP; patterns++) {
        if (!append_word(state, item, current_token(state))) return false;
        state->position++;
        if (current_token(state)->kind != GSH_TOKEN_PIPE) break;
        state->position++;
        if (current_token(state)->kind != GSH_TOKEN_WORD) {
            fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                            ? GSH_PARSE_INCOMPLETE
                            : GSH_PARSE_SYNTAX);
            return false;
        }
    }
    if (current_token(state)->kind != GSH_TOKEN_RPAREN) {
        fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                        ? GSH_PARSE_INCOMPLETE
                        : GSH_PARSE_SYNTAX);
        return false;
    }
    state->position++;
    skip_linebreak(state);
    return true;
}

static bool finish_case_item(parser *state, parse_frame *frame)
{
    if (!require(state != NULL && frame != NULL &&
                 frame->saved != GSH_AST_NONE)) return false;
    gsh_token_kind kind = current_token(state)->kind;

    if (kind == GSH_TOKEN_DSEMI || kind == GSH_TOKEN_SEMI_AND) {
        if (kind == GSH_TOKEN_SEMI_AND) {
            parser_storage(state)->nodes[frame->saved].flags |=
                GSH_AST_FLAG_CASE_FALLTHROUGH;
        }
        parser_storage(state)->nodes[frame->saved].end = current_token(state)->end;
        state->position++;
        skip_linebreak(state);
    } else if (!current_word_is(state, "esac")) {
        fail(state, kind == GSH_TOKEN_EOF ? GSH_PARSE_INCOMPLETE
                                          : GSH_PARSE_SYNTAX);
        return false;
    }
    if (!append_child(state, frame->node, frame->saved)) return false;
    if (current_token(state)->kind == GSH_TOKEN_EOF) {
        fail(state, GSH_PARSE_INCOMPLETE);
        return false;
    }
    return true;
}

static bool step_case(parse_machine *machine, parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->state != NULL)) return false;
    parser *state = machine->state;
    size_t body;

    if (frame->phase == 0U) {
        if (!parse_case_header(state, frame)) return false;
        frame->phase = 1U;
    }
    if (frame->phase == 2U) {
        if (!machine_take(machine, &body) ||
            !append_child(state, frame->saved, body)) return false;
        frame->phase = 3U;
    }
    if (frame->phase == 3U) {
        if (!finish_case_item(state, frame)) return false;
        frame->phase = 1U;
    }
    if (current_word_is(state, "esac")) {
        if (!expect_word(state, "esac")) return false;
        parser_storage(state)->nodes[frame->node].end =
            parser_storage(state)->tokens[state->position - 1U].end;
        machine_complete(machine, frame->node);
        return true;
    }
    frame->saved = new_node(state, GSH_AST_CASE_ITEM,
                            current_token(state)->begin);
    if (frame->saved == GSH_AST_NONE ||
        !parse_case_patterns(state, frame->saved)) return false;
    frame->phase = 3U;
    if (!list_stops(state, PARSE_STOP_CASE_ITEM) &&
        current_token(state)->kind != GSH_TOKEN_EOF) {
        frame->phase = 2U;
        return machine_push(machine, PARSE_FRAME_LIST,
                            PARSE_STOP_CASE_ITEM);
    }
    return true;
}

static bool step_function(parse_machine *machine, parse_frame *frame)
{
    if (!require(machine != NULL && frame != NULL &&
                 machine->state != NULL)) return false;
    parser *state = machine->state;
    parse_frame_kind kind;
    size_t body;

    if (frame->phase == 0U) {
        frame->node = new_node(state, GSH_AST_FUNCTION,
                               current_token(state)->begin);
        if (frame->node == GSH_AST_NONE ||
            !append_word(state, frame->node, current_token(state))) {
            return false;
        }
        state->position += 3U;
        skip_linebreak(state);
        kind = compound_frame_kind(state);
        if (kind == PARSE_FRAME_NONE) {
            fail(state, current_token(state)->kind == GSH_TOKEN_EOF
                            ? GSH_PARSE_INCOMPLETE
                            : GSH_PARSE_SYNTAX);
            return false;
        }
        frame->phase = 1U;
        return machine_push(machine, kind, 0U);
    }
    if (!machine_take(machine, &body) ||
        !append_child(state, frame->node, body)) return false;
    machine_complete(machine, frame->node);
    return true;
}

static bool machine_step(parse_machine *machine)
{
    if (!require(machine != NULL && machine->depth > 0U &&
                 machine->depth <= PARSE_FRAME_CAP)) return false;
    parse_frame *frame = &machine->frames[machine->depth - 1U];

    switch (frame->kind) {
        case PARSE_FRAME_LIST:
            return step_list(machine, frame);
        case PARSE_FRAME_AND_OR:
            return step_and_or(machine, frame);
        case PARSE_FRAME_PIPELINE:
            return step_pipeline(machine, frame);
        case PARSE_FRAME_COMMAND:
            return step_command(machine, frame);
        case PARSE_FRAME_SUBSHELL:
        case PARSE_FRAME_BRACE:
            return step_group(machine, frame);
        case PARSE_FRAME_WHILE:
        case PARSE_FRAME_UNTIL:
            return step_loop(machine, frame);
        case PARSE_FRAME_IF:
            return step_if(machine, frame);
        case PARSE_FRAME_FOR:
            return step_for(machine, frame);
        case PARSE_FRAME_CASE:
            return step_case(machine, frame);
        case PARSE_FRAME_FUNCTION:
            return step_function(machine, frame);
        case PARSE_FRAME_NONE:
            break;
    }
    fail(machine->state, GSH_PARSE_SYNTAX);
    return false;
}

static size_t parse_syntax(parser *state)
{
    static parse_machine machine;
    size_t steps;

    if (!require(state != NULL && state->storage != NULL)) {
        return GSH_AST_NONE;
    }
    (void)memset(&machine, 0, sizeof(machine));
    machine.state = state;
    if (!machine_push(&machine, PARSE_FRAME_LIST, 0U)) {
        machine.state = NULL;
        return GSH_AST_NONE;
    }
    for (steps = 0U; steps < PARSE_MACHINE_STEP_CAP && machine.depth > 0U;
         steps++) {
        if (!machine_step(&machine)) {
            machine.state = NULL;
            return GSH_AST_NONE;
        }
    }
    if (machine.depth != 0U || !machine.returned) {
        fail(state, GSH_PARSE_LIMIT);
        machine.state = NULL;
        return GSH_AST_NONE;
    }
    {
        size_t value = machine.value;

        machine.state = NULL;
        return value;
    }
}

typedef struct {
    gsh_word_ref delimiter;
    bool strip_tabs;
} pending_heredoc;

static bool heredoc_byte(char output[GSH_HEREDOC_DELIMITER_CAP],
                         size_t *length, unsigned char byte)
{
    if (length == NULL) return false;
    if (output == NULL) {
        return false;
    }
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

typedef enum {
    HEREDOC_QUOTE_NONE,
    HEREDOC_QUOTE_SINGLE,
    HEREDOC_QUOTE_DOUBLE,
    HEREDOC_QUOTE_DOLLAR,
} heredoc_quote;

static bool heredoc_simple_escape(unsigned char escape,
                                  unsigned char *byte)
{
    if (!require(byte != NULL)) return false;
    if (!require(escape != 0U)) return false;
    if (escape == 'n') *byte = '\n';
    else if (escape == 'r') *byte = '\r';
    else if (escape == 't') *byte = '\t';
    else if (escape == 'a') *byte = '\a';
    else if (escape == 'b') *byte = '\b';
    else if (escape == 'e') *byte = 0x1bU;
    else if (escape == 'f') *byte = '\f';
    else if (escape == 'v') *byte = '\v';
    else if (escape == '\\' || escape == '\'' || escape == '"') {
        *byte = escape;
    } else {
        return false;
    }
    return true;
}

static bool heredoc_dollar_escape(const parser *state,
                                  gsh_word_ref reference, size_t *offset,
                                  unsigned char *byte)
{
    unsigned char escape;
    unsigned int value;
    size_t digits;

    if (!require(state != NULL && state->input != NULL)) return false;
    if (!require(offset != NULL && byte != NULL)) return false;
    if (*offset == reference.end) return false;
    escape = state->input[(*offset)++];
    if (heredoc_simple_escape(escape, byte)) return true;
    if (escape == 'x') {
        value = 0U;
        for (digits = 0U; *offset < reference.end && digits < 2U; digits++) {
            int digit = heredoc_hexadecimal(state->input[*offset]);

            if (digit < 0) break;
            value = value * 16U + (unsigned int)digit;
            (*offset)++;
        }
        if (digits == 0U || value == 0U) return false;
        *byte = (unsigned char)value;
    } else if (escape >= '0' && escape <= '7') {
        value = (unsigned int)(escape - '0');
        for (digits = 1U; *offset < reference.end && digits < 3U &&
                          state->input[*offset] >= '0' &&
                          state->input[*offset] <= '7';
             digits++) {
            value = value * 8U +
                    (unsigned int)(state->input[(*offset)++] - '0');
        }
        if (value == 0U || value > 255U) return false;
        *byte = (unsigned char)value;
    } else return false;
    return true;
}

static bool heredoc_quoted_byte(
    const parser *state, gsh_word_ref reference, size_t *offset,
    heredoc_quote *quote, unsigned char *byte,
    char output[GSH_HEREDOC_DELIMITER_CAP], size_t *length)
{
    if (!require(state != NULL && offset != NULL && quote != NULL)) {
        return false;
    }
    if (!require(byte != NULL && output != NULL && length != NULL)) {
        return false;
    }
    if (*quote == HEREDOC_QUOTE_SINGLE) {
        if (*byte == '\'') *quote = HEREDOC_QUOTE_NONE;
        else if (!heredoc_byte(output, length, *byte)) return false;
        return true;
    }
    if (*quote == HEREDOC_QUOTE_DOUBLE) {
        if (*byte == '"') {
            *quote = HEREDOC_QUOTE_NONE;
            return true;
        }
        if (*byte == '\\' && *offset < reference.end) {
            unsigned char next = state->input[*offset];

            if (next == '$' || next == 0x60U || next == '"' ||
                next == '\\' || next == '\n') {
                (*offset)++;
                if (next == '\n') return true;
                *byte = next;
            }
        }
        return heredoc_byte(output, length, *byte);
    }
    if (*byte == '\'') {
        *quote = HEREDOC_QUOTE_NONE;
        return true;
    }
    if (*byte == '\\' &&
        !heredoc_dollar_escape(state, reference, offset, byte)) return false;
    return heredoc_byte(output, length, *byte);
}

static bool build_heredoc_delimiter(
    const parser *state, gsh_word_ref reference,
    char output[GSH_HEREDOC_DELIMITER_CAP], size_t *length, bool *quoted)
{
    if (length == NULL || output == NULL || quoted == NULL || state == NULL) {
        return false;
    }
    heredoc_quote quote = HEREDOC_QUOTE_NONE;
    size_t offset = reference.begin;

    *length = 0;
    *quoted = false;
    while (offset < reference.end) {
        unsigned char byte = state->input[offset++];

        if (quote != HEREDOC_QUOTE_NONE) {
            if (!heredoc_quoted_byte(state, reference, &offset, &quote,
                                     &byte, output, length)) return false;
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
    if (lexer == NULL) {
        return;
    }
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

typedef struct {
    size_t line_begin;
    size_t after_line;
    size_t comparison_length;
    bool comparison_overflow;
} heredoc_line_scan;

static bool scan_heredoc_line(
    const gsh_lexer *lexer, bool strip_tabs, bool quoted,
    char comparison[GSH_HEREDOC_DELIMITER_CAP], heredoc_line_scan *result)
{
    size_t scan;
    size_t step;

    if (!require(lexer != NULL && lexer->input != NULL)) return false;
    if (!require(comparison != NULL && result != NULL)) return false;
    (void)memset(result, 0, sizeof(*result));
    result->line_begin = lexer->offset;
    scan = result->line_begin;
    for (step = 0; step < PARSE_INPUT_STEP_CAP; step++) {
        size_t segment_begin = scan;
        size_t segment_end;
        size_t trailing_backslashes = 0U;
        bool continuation;
        size_t copy_end;

        if (strip_tabs && result->comparison_length == 0U &&
            !result->comparison_overflow) {
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
                       trailing_backslashes % 2U != 0U;
        copy_end = continuation ? segment_end - 1U : segment_end;
        if (!result->comparison_overflow) {
            size_t segment_length = copy_end - segment_begin;

            if (segment_length > GSH_HEREDOC_DELIMITER_CAP -
                                     result->comparison_length) {
                result->comparison_overflow = true;
            } else {
                (void)memcpy(comparison + result->comparison_length,
                       lexer->input + segment_begin, segment_length);
                result->comparison_length += segment_length;
            }
        }
        result->after_line = segment_end < lexer->length
                                 ? segment_end + 1U
                                 : segment_end;
        if (!continuation) return true;
        scan = result->after_line;
    }
    return false;
}

static gsh_parse_status record_heredoc(
    parser *state, const pending_heredoc *entry, size_t body_begin,
    size_t line_begin, bool quoted)
{
    gsh_heredoc *heredoc;

    if (!require(state != NULL && state->storage != NULL)) {
        return GSH_PARSE_LIMIT;
    }
    if (!require(entry != NULL && body_begin <= line_begin)) {
        return GSH_PARSE_LIMIT;
    }
    if (parser_storage(state)->heredoc_count == GSH_PARSE_REDIRECT_CAP) {
        return GSH_PARSE_LIMIT;
    }
    heredoc = &parser_storage(state)->heredocs[parser_storage(state)->heredoc_count++];
    heredoc->delimiter = entry->delimiter;
    heredoc->body.begin = body_begin;
    heredoc->body.end = line_begin;
    heredoc->flags = GSH_REDIRECT_HEREDOC |
                     (entry->strip_tabs ? GSH_REDIRECT_HEREDOC_STRIP_TABS
                                        : 0U) |
                     (quoted ? GSH_REDIRECT_HEREDOC_QUOTED : 0U);
    return GSH_PARSE_OK;
}

static gsh_parse_status collect_heredocs(
    parser *state, gsh_lexer *lexer, const pending_heredoc *pending,
    size_t pending_count)
{
    if (pending == NULL) return GSH_PARSE_LIMIT;
    if (lexer == NULL) {
        return GSH_PARSE_LIMIT;
    }
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
            heredoc_line_scan line;

            if (!scan_heredoc_line(lexer,
                                   pending[pending_index].strip_tabs,
                                   quoted, comparison, &line)) {
                return GSH_PARSE_LIMIT;
            }
            if (!line.comparison_overflow &&
                line.comparison_length == delimiter_length &&
                memcmp(comparison, delimiter, delimiter_length) == 0) {
                gsh_parse_status recorded = record_heredoc(
                    state, &pending[pending_index], body_begin,
                    line.line_begin, quoted);

                if (recorded != GSH_PARSE_OK) return recorded;
                advance_lexer_to(lexer, line.after_line);
                found = true;
                break;
            }
            if (line.after_line == lexer->length) break;
            advance_lexer_to(lexer, line.after_line);
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
    if (state == NULL) {
        return GSH_PARSE_LIMIT;
    }
    gsh_lexer lexer;
    pending_heredoc pending[GSH_PARSE_REDIRECT_CAP];
    size_t pending_count = 0;
    bool awaiting_heredoc_delimiter = false;
    bool pending_strip_tabs = false;
    size_t step;

    gsh_lexer_init(&lexer, state->input, state->length);
    for (step = 0; step <= GSH_PARSE_TOKEN_CAP; step++) {
        gsh_lex_status status;
        gsh_token *token;

        if (parser_storage(state)->token_count == GSH_PARSE_TOKEN_CAP) {
            state->result.error_offset = lexer.offset;
            return GSH_PARSE_LIMIT;
        }
        token = &parser_storage(state)->tokens[parser_storage(state)->token_count];
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
        parser_storage(state)->token_count++;
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
    state->result.error_offset = lexer.offset;
    return GSH_PARSE_LIMIT;
}

static gsh_parse_result parse_input(
    const void *input, size_t length, gsh_parse_storage *storage)
{
    if (input == NULL || storage == NULL) {
        return (gsh_parse_result){.status = GSH_PARSE_LIMIT};
    }
    parser state;
    size_t program;
    size_t body;

    storage->token_count = 0;
    storage->node_count = 0;
    storage->word_count = 0;
    storage->redirect_count = 0;
    storage->heredoc_count = 0;
    storage->command_word_count = 0;
    (void)memset(&state, 0, sizeof(state));
    state.input = input;
    state.length = length;
    state.storage = storage;
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
    body = parse_syntax(&state);
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
    if (input == NULL || storage == NULL) {
        return (gsh_parse_result){.status = GSH_PARSE_LIMIT};
    }
    return parse_input(input, length, storage);
}

const char *gsh_parse_status_name(gsh_parse_status status)
{
    static const char *const names[] = {
        "ok", "lexical", "syntax", "incomplete", "limit", "unsupported",
    };

    if ((size_t)status >= sizeof(names) / sizeof(names[0])) {
        return "invalid";
    }
    return names[status];
}
