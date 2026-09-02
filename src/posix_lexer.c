#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "posix_lexer.h"

#include <stdbool.h>
#include <string.h>

#define require(condition) (condition)

enum {
    CONTEXT_CAP = 128,
    LEXER_TRIVIA_STEP_CAP = 1024 * 1024 + 1,
};

typedef enum {
    CONTEXT_SINGLE,
    CONTEXT_DOUBLE,
    CONTEXT_DOLLAR_SINGLE,
    CONTEXT_BACKQUOTE,
    CONTEXT_PARAMETER,
    CONTEXT_COMMAND,
    CONTEXT_ARITHMETIC,
    CONTEXT_PARENTHESIS,
} context_kind;

typedef struct {
    context_kind items[CONTEXT_CAP];
    size_t length;
} context_stack;

typedef struct {
    const char *text;
    size_t length;
    gsh_token_kind kind;
} operator_definition;

static const operator_definition OPERATORS[] = {
    {"<<-", 3, GSH_TOKEN_DLESSDASH},
    {"&&", 2, GSH_TOKEN_AND_IF},
    {"||", 2, GSH_TOKEN_OR_IF},
    {";;", 2, GSH_TOKEN_DSEMI},
    {";&", 2, GSH_TOKEN_SEMI_AND},
    {"<<", 2, GSH_TOKEN_DLESS},
    {">>", 2, GSH_TOKEN_DGREAT},
    {"<&", 2, GSH_TOKEN_LESSAND},
    {">&", 2, GSH_TOKEN_GREATAND},
    {"<>", 2, GSH_TOKEN_LESSGREAT},
    {">|", 2, GSH_TOKEN_CLOBBER},
    {"&", 1, GSH_TOKEN_AMPERSAND},
    {";", 1, GSH_TOKEN_SEMICOLON},
    {"|", 1, GSH_TOKEN_PIPE},
    {"(", 1, GSH_TOKEN_LPAREN},
    {")", 1, GSH_TOKEN_RPAREN},
    {"<", 1, GSH_TOKEN_LESS},
    {">", 1, GSH_TOKEN_GREAT},
};

static void advance_one(gsh_lexer *lexer)
{
    if (lexer == NULL) return;
    if (lexer->input[lexer->offset] == '\n') {
        lexer->line++;
        lexer->column = 1;
    } else {
        lexer->column++;
    }
    lexer->offset++;
}

static void advance_count(gsh_lexer *lexer, size_t count)
{
    if (lexer == NULL) {
        return;
    }
    while (count-- > 0) {
        advance_one(lexer);
    }
}

static bool push_context(gsh_lexer *lexer, context_stack *stack,
                         context_kind context)
{
    if (stack == NULL) return false;
    if (lexer == NULL) {
        return false;
    }
    if (stack->length == CONTEXT_CAP) {
        lexer->status = GSH_LEX_LIMIT;
        lexer->error_offset = lexer->offset;
        return false;
    }
    stack->items[stack->length++] = context;
    return true;
}

static void pop_context(context_stack *stack)
{
    if (stack == NULL) return;
    if (stack->length > 0) {
        stack->length--;
    }
}

static bool starts_with(const gsh_lexer *lexer, const char *text,
                        size_t length)
{
    if (lexer == NULL || text == NULL) {
        return false;
    }
    return length <= lexer->length - lexer->offset &&
           memcmp(lexer->input + lexer->offset, text, length) == 0;
}

static const operator_definition *operator_at(const gsh_lexer *lexer)
{
    size_t index;

    for (index = 0; index < sizeof(OPERATORS) / sizeof(OPERATORS[0]);
         index++) {
        if (starts_with(lexer, OPERATORS[index].text,
                        OPERATORS[index].length)) {
            return &OPERATORS[index];
        }
    }
    return NULL;
}

static bool begin_substitution(gsh_lexer *lexer, context_stack *stack)
{
    if (stack == NULL) {
        return false;
    }
    if (starts_with(lexer, "$((", 3)) {
        if (!push_context(lexer, stack, CONTEXT_ARITHMETIC)) {
            return false;
        }
        advance_count(lexer, 3);
        return true;
    }
    if (starts_with(lexer, "$(", 2)) {
        if (!push_context(lexer, stack, CONTEXT_COMMAND)) {
            return false;
        }
        advance_count(lexer, 2);
        return true;
    }
    if (starts_with(lexer, "$" "{", 2)) {
        if (!push_context(lexer, stack, CONTEXT_PARAMETER)) {
            return false;
        }
        advance_count(lexer, 2);
        return true;
    }
    if (starts_with(lexer, "$'", 2)) {
        if (!push_context(lexer, stack, CONTEXT_DOLLAR_SINGLE)) {
            return false;
        }
        advance_count(lexer, 2);
        return true;
    }
    return false;
}

static void consume_escape(gsh_lexer *lexer)
{
    if (lexer == NULL) {
        return;
    }
    advance_one(lexer);
    if (lexer->offset < lexer->length) {
        advance_one(lexer);
    }
}

static bool scan_plain_word_byte(gsh_lexer *lexer, context_stack *stack,
                                 unsigned char byte)
{
    if (!require(lexer != NULL && stack != NULL)) return false;
    if (!require(stack->length == 0U)) return false;
    if (byte == '\\') {
        consume_escape(lexer);
    } else if (byte == '\'') {
        if (!push_context(lexer, stack, CONTEXT_SINGLE)) return false;
        advance_one(lexer);
    } else if (byte == '"') {
        if (!push_context(lexer, stack, CONTEXT_DOUBLE)) return false;
        advance_one(lexer);
    } else if (byte == 0x60U) {
        if (!push_context(lexer, stack, CONTEXT_BACKQUOTE)) return false;
        advance_one(lexer);
    } else if (byte != '$' || !begin_substitution(lexer, stack)) {
        advance_one(lexer);
    }
    return true;
}

static bool scan_quoted_word_byte(gsh_lexer *lexer, context_stack *stack,
                                  context_kind context,
                                  unsigned char byte)
{
    if (!require(lexer != NULL && stack != NULL)) return false;
    if (!require(stack->length > 0U)) return false;
    if (context == CONTEXT_SINGLE) {
        advance_one(lexer);
        if (byte == '\'') pop_context(stack);
    } else if (context == CONTEXT_DOLLAR_SINGLE) {
        if (byte == '\\') consume_escape(lexer);
        else {
            advance_one(lexer);
            if (byte == '\'') pop_context(stack);
        }
    } else if (context == CONTEXT_BACKQUOTE) {
        if (byte == '\\') consume_escape(lexer);
        else {
            advance_one(lexer);
            if (byte == 0x60U) pop_context(stack);
        }
    } else if (byte == '"') {
        advance_one(lexer);
        pop_context(stack);
    } else if (byte == '\\') {
        consume_escape(lexer);
    } else if (byte == 0x60U) {
        if (!push_context(lexer, stack, CONTEXT_BACKQUOTE)) return false;
        advance_one(lexer);
    } else if (byte != '$' || !begin_substitution(lexer, stack)) {
        advance_one(lexer);
    }
    return true;
}

static bool scan_nested_word_byte(gsh_lexer *lexer, context_stack *stack,
                                  context_kind context,
                                  unsigned char byte)
{
    if (!require(lexer != NULL && stack != NULL)) return false;
    if (!require(stack->length > 0U)) return false;
    if (byte == '\\') consume_escape(lexer);
    else if (byte == '\'') {
        if (!push_context(lexer, stack, CONTEXT_SINGLE)) return false;
        advance_one(lexer);
    } else if (byte == '"') {
        if (!push_context(lexer, stack, CONTEXT_DOUBLE)) return false;
        advance_one(lexer);
    } else if (byte == 0x60U) {
        if (!push_context(lexer, stack, CONTEXT_BACKQUOTE)) return false;
        advance_one(lexer);
    } else if (byte == '$' && begin_substitution(lexer, stack)) return true;
    else if (context == CONTEXT_PARAMETER && byte == '}') {
        advance_one(lexer);
        pop_context(stack);
    } else if ((context == CONTEXT_COMMAND ||
                context == CONTEXT_PARENTHESIS) && byte == '(') {
        if (!push_context(lexer, stack, CONTEXT_PARENTHESIS)) return false;
        advance_one(lexer);
    } else if ((context == CONTEXT_PARENTHESIS ||
                context == CONTEXT_COMMAND) && byte == ')') {
        advance_one(lexer);
        pop_context(stack);
    } else if (context == CONTEXT_ARITHMETIC &&
               starts_with(lexer, "))", 2U)) {
        advance_count(lexer, 2U);
        pop_context(stack);
    } else if (context == CONTEXT_ARITHMETIC && byte == '(') {
        if (!push_context(lexer, stack, CONTEXT_PARENTHESIS)) return false;
        advance_one(lexer);
    } else advance_one(lexer);
    return true;
}

static bool scan_word(gsh_lexer *lexer)
{
    context_stack stack = {{0}, 0};

    if (!require(lexer != NULL && lexer->input != NULL)) return false;
    if (!require(lexer->offset <= lexer->length)) return false;
    while (lexer->offset < lexer->length) {
        unsigned char byte = lexer->input[lexer->offset];
        context_kind context = stack.length > 0
                                   ? stack.items[stack.length - 1U]
                                   : CONTEXT_PARENTHESIS;

        if (byte == '\0') {
            lexer->status = GSH_LEX_INVALID;
            lexer->error_offset = lexer->offset;
            return false;
        }

        if (stack.length == 0) {
            if (byte == ' ' || byte == '\t' || byte == '\n' ||
                operator_at(lexer) != NULL) {
                return true;
            }
            if (!scan_plain_word_byte(lexer, &stack, byte)) return false;
            continue;
        }

        if (context == CONTEXT_SINGLE ||
            context == CONTEXT_DOLLAR_SINGLE ||
            context == CONTEXT_BACKQUOTE || context == CONTEXT_DOUBLE) {
            if (!scan_quoted_word_byte(lexer, &stack, context, byte)) {
                return false;
            }
            continue;
        }
        if (!scan_nested_word_byte(lexer, &stack, context, byte)) return false;
    }

    if (stack.length != 0) {
        lexer->status = GSH_LEX_INCOMPLETE;
        lexer->error_offset = lexer->offset;
        return false;
    }
    return true;
}

void gsh_lexer_init(gsh_lexer *lexer, const void *input, size_t length)
{
    if (input == NULL || lexer == NULL) {
        return;
    }
    lexer->input = input;
    lexer->length = length;
    lexer->offset = 0;
    lexer->line = 1;
    lexer->column = 1;
    lexer->status = GSH_LEX_OK;
    lexer->error_offset = 0;
}

gsh_lex_status gsh_lexer_next(gsh_lexer *lexer, gsh_token *token)
{
    if (lexer == NULL) return GSH_LEX_INVALID;
    if (token == NULL) {
        return GSH_LEX_LIMIT;
    }
    const operator_definition *operator;
    size_t trivia_step;

    if (lexer->status != GSH_LEX_OK) {
        return lexer->status;
    }
    for (trivia_step = 0; trivia_step < LEXER_TRIVIA_STEP_CAP;
         trivia_step++) {
        while (lexer->offset < lexer->length &&
               (lexer->input[lexer->offset] == ' ' ||
                lexer->input[lexer->offset] == '\t')) {
            advance_one(lexer);
        }
        if (lexer->offset + 1U < lexer->length &&
            lexer->input[lexer->offset] == '\\' &&
            lexer->input[lexer->offset + 1U] == '\n') {
            advance_count(lexer, 2);
            continue;
        }
        if (lexer->offset < lexer->length &&
            lexer->input[lexer->offset] == '#') {
            while (lexer->offset < lexer->length &&
                   lexer->input[lexer->offset] != '\n') {
                advance_one(lexer);
            }
            continue;
        }
        break;
    }
    if (trivia_step == LEXER_TRIVIA_STEP_CAP) {
        lexer->status = GSH_LEX_LIMIT;
        lexer->error_offset = lexer->offset;
        return lexer->status;
    }

    token->begin = lexer->offset;
    token->end = lexer->offset;
    token->line = lexer->line;
    token->column = lexer->column;
    if (lexer->offset == lexer->length) {
        token->kind = GSH_TOKEN_EOF;
        return GSH_LEX_OK;
    }
    if (lexer->input[lexer->offset] == '\0') {
        lexer->status = GSH_LEX_INVALID;
        lexer->error_offset = lexer->offset;
        return lexer->status;
    }
    if (lexer->input[lexer->offset] == '\n') {
        token->kind = GSH_TOKEN_NEWLINE;
        advance_one(lexer);
        token->end = lexer->offset;
        return GSH_LEX_OK;
    }
    operator = operator_at(lexer);
    if (operator != NULL) {
        token->kind = operator->kind;
        advance_count(lexer, operator->length);
        token->end = lexer->offset;
        return GSH_LEX_OK;
    }

    token->kind = GSH_TOKEN_WORD;
    if (!scan_word(lexer)) {
        return lexer->status;
    }
    token->end = lexer->offset;
    if (token->end == token->begin) {
        lexer->status = GSH_LEX_INVALID;
        lexer->error_offset = lexer->offset;
        return lexer->status;
    }
    return GSH_LEX_OK;
}

const char *gsh_token_kind_name(gsh_token_kind kind)
{
    static const char *const names[] = {
        "EOF",       "WORD",      "NEWLINE", "AND_IF",  "OR_IF",
        "DSEMI",     "SEMI_AND",  "DLESS",   "DGREAT",  "LESSAND",
        "GREATAND",  "LESSGREAT", "DLESSDASH", "CLOBBER", "&",
        ";",         "|",         "(",        ")",       "<",
        ">",
    };

    if ((size_t)kind >= sizeof(names) / sizeof(names[0])) {
        return "INVALID";
    }
    return names[kind];
}
