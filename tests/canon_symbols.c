#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh symbol inventory requires the POSIX.1-2024 baseline"
#endif

#include "canon_symbols.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define require(condition) (condition)

typedef enum {
    SYMBOL_CODE,
    SYMBOL_LINE_COMMENT,
    SYMBOL_BLOCK_COMMENT,
    SYMBOL_STRING,
    SYMBOL_CHARACTER,
} symbol_lexical_state;

typedef enum {
    SYMBOL_AGGREGATE_NONE,
    SYMBOL_AGGREGATE_STRUCT,
    SYMBOL_AGGREGATE_ENUM,
} symbol_aggregate_kind;

typedef struct {
    symbol_aggregate_kind kind;
    size_t brace_depth;
    size_t bracket_depth;
    bool expect_name;
    char candidate[GSH_CANON_SYMBOL_NAME_CAP];
} symbol_aggregate;

typedef struct {
    symbol_lexical_state lexical;
    symbol_aggregate aggregates[GSH_CANON_AGGREGATE_CAP];
    size_t aggregate_count;
    size_t brace_depth;
    bool escaped;
    bool line_prefix;
    bool preprocessor;
    bool expect_directive;
    bool expect_macro_name;
    bool typedef_active;
    symbol_aggregate_kind aggregate_pending;
    char typedef_candidate[GSH_CANON_SYMBOL_NAME_CAP];
} symbol_parser;

static bool symbol_identifier_start(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') || byte == '_';
}

static bool symbol_identifier_continue(unsigned char byte)
{
    return symbol_identifier_start(byte) || (byte >= '0' && byte <= '9');
}

static int symbol_copy(char destination[GSH_CANON_SYMBOL_NAME_CAP],
                       const unsigned char *source, size_t length)
{
    if (!require(destination != NULL && source != NULL)) return -1;
    if (!require(length > 0U && length < GSH_CANON_SYMBOL_NAME_CAP)) return -1;
    (void)memcpy(destination, source, length);
    destination[length] = '\0';
    return 0;
}

static int symbol_record_occurrence(gsh_canon_symbol_inventory *inventory,
                                    const unsigned char *name, size_t length)
{
    size_t index;

    if (!require(inventory != NULL && name != NULL)) return -1;
    for (index = 0U; index < inventory->symbol_count; index++) {
        if (strlen(inventory->symbols[index].name) == length &&
            memcmp(inventory->symbols[index].name, name, length) == 0) {
            if (inventory->symbols[index].occurrences == SIZE_MAX) {
                errno = EOVERFLOW;
                return -1;
            }
            inventory->symbols[index].occurrences++;
            return 0;
        }
    }
    if (inventory->symbol_count == GSH_CANON_SYMBOL_CAP) {
        errno = ENOSPC;
        return -1;
    }
    if (symbol_copy(inventory->symbols[index].name, name, length) == -1) {
        return -1;
    }
    inventory->symbols[index].occurrences = 1U;
    inventory->symbol_count++;
    return 0;
}

static int symbol_record_declaration(gsh_canon_symbol_inventory *inventory,
                                     const char *name, const char *path,
                                     gsh_canon_declaration_kind kind)
{
    gsh_canon_declaration *declaration;
    size_t name_length;
    size_t path_length;

    if (!require(inventory != NULL && name != NULL && path != NULL)) return -1;
    name_length = strnlen(name, GSH_CANON_SYMBOL_NAME_CAP);
    path_length = strnlen(path, GSH_CANON_SYMBOL_PATH_CAP);
    if (name_length == 0U || name_length == GSH_CANON_SYMBOL_NAME_CAP ||
        path_length == GSH_CANON_SYMBOL_PATH_CAP ||
        inventory->declaration_count == GSH_CANON_DECLARATION_CAP) {
        errno = ENOSPC;
        return -1;
    }
    declaration = &inventory->declarations[inventory->declaration_count++];
    (void)memcpy(declaration->name, name, name_length + 1U);
    (void)memcpy(declaration->path, path, path_length + 1U);
    declaration->kind = kind;
    return 0;
}

static bool symbol_type_keyword(const char *name)
{
    static const char *const keywords[] = {
        "_Atomic", "_Bool", "const", "double", "enum", "float", "int",
        "long", "restrict", "short", "signed", "struct", "union",
        "unsigned", "void", "volatile",
    };
    size_t index;

    if (!require(name != NULL)) return false;
    for (index = 0U; index < sizeof(keywords) / sizeof(keywords[0]); index++) {
        if (strcmp(name, keywords[index]) == 0) return true;
    }
    return false;
}

static int symbol_accept_preprocessor(
    gsh_canon_symbol_inventory *inventory, symbol_parser *parser,
    const char *identifier, const char *path)
{
    if (parser == NULL) return -1;
    if (identifier == NULL || inventory == NULL || path == NULL) {
        return -1;
    }
    if (parser->expect_directive) {
        parser->expect_directive = false;
        parser->expect_macro_name = strcmp(identifier, "define") == 0;
    } else if (parser->expect_macro_name) {
        parser->expect_macro_name = false;
        if (strcmp(identifier, "_DARWIN_C_SOURCE") == 0 ||
            strcmp(identifier, "_GNU_SOURCE") == 0 ||
            strcmp(identifier, "_POSIX_C_SOURCE") == 0) {
            return 0;
        }
        return symbol_record_declaration(
            inventory, identifier, path, GSH_CANON_MACRO_DECLARATION);
    }
    return 0;
}

static int symbol_accept_aggregate_identifier(
    gsh_canon_symbol_inventory *inventory, symbol_parser *parser,
    const char *identifier, const char *path)
{
    if (parser == NULL) return -1;
    if (identifier == NULL || inventory == NULL || path == NULL) {
        return -1;
    }
    symbol_aggregate *aggregate;

    if (parser->aggregate_count == 0U) return 0;
    aggregate = &parser->aggregates[parser->aggregate_count - 1U];
    if (parser->brace_depth != aggregate->brace_depth) return 0;
    if (aggregate->kind == SYMBOL_AGGREGATE_ENUM && aggregate->expect_name) {
        aggregate->expect_name = false;
        return symbol_record_declaration(
            inventory, identifier, path, GSH_CANON_ENUMERATOR_DECLARATION);
    }
    if (aggregate->kind == SYMBOL_AGGREGATE_STRUCT &&
        aggregate->bracket_depth == 0U && !symbol_type_keyword(identifier)) {
        (void)memcpy(aggregate->candidate, identifier,
               GSH_CANON_SYMBOL_NAME_CAP);
    }
    return 0;
}

static int symbol_accept_identifier(
    gsh_canon_symbol_inventory *inventory, symbol_parser *parser,
    const unsigned char *source, size_t begin, size_t end, const char *path)
{
    if (parser == NULL) return -1;
    char identifier[GSH_CANON_SYMBOL_NAME_CAP];

    if (!require(begin < end)) return -1;
    if (!require(path != NULL)) return -1;
    if (symbol_record_occurrence(inventory, source + begin, end - begin) == -1 ||
        symbol_copy(identifier, source + begin, end - begin) == -1) return -1;
    if (parser->preprocessor) {
        return symbol_accept_preprocessor(inventory, parser, identifier, path);
    }
    if (strcmp(identifier, "typedef") == 0) {
        parser->typedef_active = true;
        parser->typedef_candidate[0] = '\0';
    } else if (strcmp(identifier, "enum") == 0) {
        parser->aggregate_pending = SYMBOL_AGGREGATE_ENUM;
    } else if (strcmp(identifier, "struct") == 0 ||
               strcmp(identifier, "union") == 0) {
        parser->aggregate_pending = SYMBOL_AGGREGATE_STRUCT;
    }
    if (symbol_accept_aggregate_identifier(
            inventory, parser, identifier, path) == -1) return -1;
    if (parser->typedef_active && parser->aggregate_count == 0U &&
        !symbol_type_keyword(identifier) && strcmp(identifier, "typedef") != 0) {
        (void)memcpy(parser->typedef_candidate, identifier,
               sizeof(parser->typedef_candidate));
    }
    return 0;
}

static int symbol_finish_field(gsh_canon_symbol_inventory *inventory,
                               symbol_aggregate *aggregate,
                               const char *path)
{
    int result = 0;

    if (!require(inventory != NULL)) return -1;
    if (!require(aggregate != NULL && path != NULL)) return -1;
    if (aggregate->candidate[0] != '\0') {
        result = symbol_record_declaration(
            inventory, aggregate->candidate, path,
            GSH_CANON_FIELD_DECLARATION);
    }
    aggregate->candidate[0] = '\0';
    return result;
}

static int symbol_open_aggregate(symbol_parser *parser)
{
    if (parser == NULL) return -1;
    symbol_aggregate *aggregate;

    if (parser->aggregate_count == GSH_CANON_AGGREGATE_CAP) {
        errno = ENOSPC;
        return -1;
    }
    aggregate = &parser->aggregates[parser->aggregate_count++];
    (void)memset(aggregate, 0, sizeof(*aggregate));
    aggregate->kind = parser->aggregate_pending;
    aggregate->brace_depth = parser->brace_depth + 1U;
    aggregate->expect_name = aggregate->kind == SYMBOL_AGGREGATE_ENUM;
    parser->aggregate_pending = SYMBOL_AGGREGATE_NONE;
    return 0;
}

static int symbol_accept_punctuation(
    gsh_canon_symbol_inventory *inventory, symbol_parser *parser,
    unsigned char byte, const char *path)
{
    if (!require(inventory != NULL && parser != NULL && path != NULL)) {
        return -1;
    }
    symbol_aggregate *aggregate = parser->aggregate_count == 0U
                                      ? NULL
                                      : &parser->aggregates[
                                            parser->aggregate_count - 1U];

    if (byte == '{') {
        if (parser->aggregate_pending != SYMBOL_AGGREGATE_NONE &&
            symbol_open_aggregate(parser) == -1) return -1;
        parser->brace_depth++;
        return 0;
    }
    if (byte == '}' && parser->brace_depth > 0U) {
        if (aggregate != NULL && parser->brace_depth == aggregate->brace_depth) {
            parser->aggregate_count--;
        }
        parser->brace_depth--;
        return 0;
    }
    if (aggregate != NULL && parser->brace_depth == aggregate->brace_depth) {
        if (byte == '[') aggregate->bracket_depth++;
        if (byte == ']' && aggregate->bracket_depth > 0U) {
            aggregate->bracket_depth--;
        }
        if (byte == ',' && aggregate->kind == SYMBOL_AGGREGATE_ENUM) {
            aggregate->expect_name = true;
        } else if ((byte == ',' || byte == ';') &&
                   aggregate->kind == SYMBOL_AGGREGATE_STRUCT &&
                   aggregate->bracket_depth == 0U &&
                   symbol_finish_field(inventory, aggregate, path) == -1) {
            return -1;
        }
    }
    if (byte == ';' && parser->typedef_active &&
        parser->aggregate_count == 0U) {
        if (parser->typedef_candidate[0] != '\0' &&
            symbol_record_declaration(
                inventory, parser->typedef_candidate, path,
                GSH_CANON_TYPE_DECLARATION) == -1) return -1;
        parser->typedef_active = false;
        parser->typedef_candidate[0] = '\0';
    }
    return 0;
}

static bool symbol_consume_non_code(symbol_parser *parser,
                                    const unsigned char *source,
                                    size_t length, size_t *index)
{
    if (parser == NULL) return false;
    if (index == NULL || source == NULL) {
        return false;
    }
    unsigned char byte = source[*index];

    if (parser->lexical == SYMBOL_LINE_COMMENT) {
        if (byte == '\n') parser->lexical = SYMBOL_CODE;
        (*index)++;
        return true;
    }
    if (parser->lexical == SYMBOL_BLOCK_COMMENT) {
        if (byte == '*' && *index + 1U < length && source[*index + 1U] == '/') {
            parser->lexical = SYMBOL_CODE;
            *index += 2U;
        } else {
            (*index)++;
        }
        return true;
    }
    if (parser->lexical == SYMBOL_STRING ||
        parser->lexical == SYMBOL_CHARACTER) {
        unsigned char end = parser->lexical == SYMBOL_STRING ? '"' : '\'';

        if (!parser->escaped && byte == end) parser->lexical = SYMBOL_CODE;
        parser->escaped = !parser->escaped && byte == '\\';
        if (byte != '\\') parser->escaped = false;
        (*index)++;
        return true;
    }
    return false;
}

static bool symbol_open_non_code(symbol_parser *parser,
                                 const unsigned char *source,
                                 size_t length, size_t *index)
{
    if (index == NULL || parser == NULL || source == NULL) {
        return false;
    }
    unsigned char byte = source[*index];

    if (byte == '/' && *index + 1U < length && source[*index + 1U] == '/') {
        parser->lexical = SYMBOL_LINE_COMMENT;
        *index += 2U;
        return true;
    }
    if (byte == '/' && *index + 1U < length && source[*index + 1U] == '*') {
        parser->lexical = SYMBOL_BLOCK_COMMENT;
        *index += 2U;
        return true;
    }
    if (byte == '"' || byte == '\'') {
        parser->lexical = byte == '"' ? SYMBOL_STRING : SYMBOL_CHARACTER;
        parser->escaped = false;
        (*index)++;
        return true;
    }
    return false;
}

void gsh_canon_symbols_initialize(gsh_canon_symbol_inventory *inventory)
{
    if (!require(inventory != NULL)) return;
    (void)memset(inventory, 0, sizeof(*inventory));
}

int gsh_canon_symbols_add(gsh_canon_symbol_inventory *inventory,
                          const unsigned char *source, size_t length,
                          const char *path)
{
    symbol_parser parser = {.lexical = SYMBOL_CODE, .line_prefix = true};
    size_t index = 0U;

    if (!require(inventory != NULL && source != NULL)) return -1;
    if (!require(path != NULL)) return -1;
    if (!require(length > 0U)) return -1;
    if (!require(path[0] != '\0')) return -1;
    while (index < length) {
        unsigned char byte = source[index];

        if (symbol_consume_non_code(&parser, source, length, &index) ||
            symbol_open_non_code(&parser, source, length, &index)) continue;
        if (byte == '\n') {
            parser.line_prefix = true;
            parser.preprocessor = false;
            parser.expect_directive = false;
            parser.expect_macro_name = false;
            index++;
            continue;
        }
        if ((byte == ' ' || byte == '\t' || byte == '\r' ||
             byte == '\f' || byte == '\v') && parser.line_prefix) {
            index++;
            continue;
        }
        if (byte == '#' && parser.line_prefix) {
            parser.preprocessor = true;
            parser.expect_directive = true;
            parser.line_prefix = false;
            index++;
            continue;
        }
        parser.line_prefix = false;
        if (symbol_identifier_start(byte)) {
            size_t begin = index;

            while (index < length &&
                   symbol_identifier_continue(source[index])) index++;
            if (symbol_accept_identifier(
                    inventory, &parser, source, begin, index, path) == -1) {
                return -1;
            }
            continue;
        }
        if (!parser.preprocessor &&
            symbol_accept_punctuation(inventory, &parser, byte, path) == -1) {
            return -1;
        }
        index++;
    }
    return 0;
}

static const char *symbol_kind_name(gsh_canon_declaration_kind kind)
{
    static const char *const names[] = {"type", "field", "enumerator",
                                        "macro"};

    return (size_t)kind < sizeof(names) / sizeof(names[0])
               ? names[kind]
               : "unknown";
}

int gsh_canon_symbols_analyze(const gsh_canon_symbol_inventory *inventory,
                              size_t *unused_declarations, bool verbose)
{
    size_t declaration;

    if (!require(inventory != NULL && unused_declarations != NULL)) return -1;
    if (!require(inventory->symbol_count <= GSH_CANON_SYMBOL_CAP)) return -1;
    if (!require(inventory->declaration_count <=
                 GSH_CANON_DECLARATION_CAP)) return -1;
    *unused_declarations = 0U;
    for (declaration = 0U; declaration < inventory->declaration_count;
         declaration++) {
        const gsh_canon_declaration *candidate =
            &inventory->declarations[declaration];
        size_t symbol;

        for (symbol = 0U; symbol < inventory->symbol_count; symbol++) {
            if (strcmp(candidate->name, inventory->symbols[symbol].name) == 0) {
                if (inventory->symbols[symbol].occurrences == 1U) {
                    if (verbose) {
                        (void)fprintf(stderr, "source policy: unused %s: %s (%s)\n",
                                symbol_kind_name(candidate->kind),
                                candidate->name, candidate->path);
                    }
                    (*unused_declarations)++;
                }
                break;
            }
        }
    }
    return 0;
}
