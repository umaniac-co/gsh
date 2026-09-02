#ifndef GSH_POSIX_PARSER_H
#define GSH_POSIX_PARSER_H

#include "posix_lexer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_PARSE_TOKEN_CAP = 4096,
    GSH_PARSE_NODE_CAP = 2048,
    GSH_PARSE_WORD_CAP = 4096,
    GSH_PARSE_REDIRECT_CAP = 1024,
    GSH_HEREDOC_DELIMITER_CAP = 4096,
};

#define GSH_AST_NONE SIZE_MAX

typedef enum {
    GSH_PARSE_OK = 0,
    GSH_PARSE_LEXICAL,
    GSH_PARSE_SYNTAX,
    GSH_PARSE_INCOMPLETE,
    GSH_PARSE_LIMIT,
} gsh_parse_status;

typedef enum {
    GSH_AST_PROGRAM,
    GSH_AST_LIST,
    GSH_AST_AND_OR,
    GSH_AST_PIPELINE,
    GSH_AST_SIMPLE,
    GSH_AST_SUBSHELL,
    GSH_AST_BRACE_GROUP,
    GSH_AST_IF,
    GSH_AST_IF_BRANCH,
    GSH_AST_WHILE,
    GSH_AST_UNTIL,
    GSH_AST_FOR,
    GSH_AST_CASE,
    GSH_AST_CASE_ITEM,
    GSH_AST_FUNCTION,
} gsh_ast_kind;

enum {
    GSH_AST_FLAG_ASYNC = 1U << 0,
    GSH_AST_FLAG_NEGATED = 1U << 1,
    GSH_AST_FLAG_FOR_HAS_IN = 1U << 2,
    GSH_AST_FLAG_CASE_FALLTHROUGH = 1U << 3,
};

enum {
    GSH_REDIRECT_HEREDOC = 1U << 0,
    GSH_REDIRECT_HEREDOC_STRIP_TABS = 1U << 1,
    GSH_REDIRECT_HEREDOC_QUOTED = 1U << 2,
};

typedef struct {
    size_t begin;
    size_t end;
} gsh_word_ref;

typedef struct {
    int descriptor;
    gsh_token_kind operator_kind;
    gsh_word_ref target;
    gsh_word_ref body;
    unsigned int flags;
} gsh_redirect;

typedef struct {
    gsh_word_ref delimiter;
    gsh_word_ref body;
    unsigned int flags;
} gsh_heredoc;

typedef struct {
    gsh_ast_kind kind;
    unsigned int flags;
    gsh_token_kind connector;
    size_t first_child;
    size_t last_child;
    size_t next_sibling;
    size_t first_word;
    size_t word_count;
    size_t first_redirect;
    size_t redirect_count;
    size_t begin;
    size_t end;
} gsh_ast_node;

typedef struct {
    gsh_token tokens[GSH_PARSE_TOKEN_CAP];
    gsh_ast_node nodes[GSH_PARSE_NODE_CAP];
    gsh_word_ref words[GSH_PARSE_WORD_CAP];
    gsh_redirect redirects[GSH_PARSE_REDIRECT_CAP];
    gsh_heredoc heredocs[GSH_PARSE_REDIRECT_CAP];
    gsh_word_ref command_words[GSH_PARSE_NODE_CAP];
    size_t token_count;
    size_t node_count;
    size_t word_count;
    size_t redirect_count;
    size_t heredoc_count;
    size_t command_word_count;
} gsh_parse_storage;

typedef struct {
    gsh_parse_status status;
    size_t root;
    size_t error_offset;
    gsh_token_kind unexpected;
} gsh_parse_result;

gsh_parse_result gsh_parse(const void *input, size_t length,
                           gsh_parse_storage *storage);
const char *gsh_parse_status_name(gsh_parse_status status);

#endif
