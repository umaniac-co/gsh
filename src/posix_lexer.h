#ifndef GSH_POSIX_LEXER_H
#define GSH_POSIX_LEXER_H

#include <stddef.h>

typedef enum {
    GSH_TOKEN_EOF = 0,
    GSH_TOKEN_WORD,
    GSH_TOKEN_NEWLINE,
    GSH_TOKEN_AND_IF,
    GSH_TOKEN_OR_IF,
    GSH_TOKEN_DSEMI,
    GSH_TOKEN_SEMI_AND,
    GSH_TOKEN_DLESS,
    GSH_TOKEN_DGREAT,
    GSH_TOKEN_LESSAND,
    GSH_TOKEN_GREATAND,
    GSH_TOKEN_LESSGREAT,
    GSH_TOKEN_DLESSDASH,
    GSH_TOKEN_CLOBBER,
    GSH_TOKEN_AMPERSAND,
    GSH_TOKEN_SEMICOLON,
    GSH_TOKEN_PIPE,
    GSH_TOKEN_LPAREN,
    GSH_TOKEN_RPAREN,
    GSH_TOKEN_LESS,
    GSH_TOKEN_GREAT,
} gsh_token_kind;

typedef enum {
    GSH_LEX_OK = 0,
    GSH_LEX_INCOMPLETE,
    GSH_LEX_INVALID,
    GSH_LEX_LIMIT,
} gsh_lex_status;

typedef struct {
    gsh_token_kind kind;
    size_t begin;
    size_t end;
    size_t line;
    size_t column;
} gsh_token;

typedef struct {
    const unsigned char *input;
    size_t length;
    size_t offset;
    size_t line;
    size_t column;
    gsh_lex_status status;
    size_t error_offset;
} gsh_lexer;

void gsh_lexer_init(gsh_lexer *lexer, const void *input, size_t length);
gsh_lex_status gsh_lexer_next(gsh_lexer *lexer, gsh_token *token);
const char *gsh_token_kind_name(gsh_token_kind kind);

#endif
