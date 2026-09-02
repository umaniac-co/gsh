#ifndef GSH_CANON_SYMBOLS_H
#define GSH_CANON_SYMBOLS_H

#include <stdbool.h>
#include <stddef.h>

enum {
    GSH_CANON_SYMBOL_CAP = 16384,
    GSH_CANON_DECLARATION_CAP = 8192,
    GSH_CANON_SYMBOL_NAME_CAP = 96,
    GSH_CANON_SYMBOL_PATH_CAP = 4096,
    GSH_CANON_AGGREGATE_CAP = 32,
};

typedef enum {
    GSH_CANON_TYPE_DECLARATION,
    GSH_CANON_FIELD_DECLARATION,
    GSH_CANON_ENUMERATOR_DECLARATION,
    GSH_CANON_MACRO_DECLARATION,
} gsh_canon_declaration_kind;

typedef struct {
    char name[GSH_CANON_SYMBOL_NAME_CAP];
    size_t occurrences;
} gsh_canon_symbol;

typedef struct {
    char name[GSH_CANON_SYMBOL_NAME_CAP];
    char path[GSH_CANON_SYMBOL_PATH_CAP];
    gsh_canon_declaration_kind kind;
} gsh_canon_declaration;

typedef struct {
    gsh_canon_symbol symbols[GSH_CANON_SYMBOL_CAP];
    gsh_canon_declaration declarations[GSH_CANON_DECLARATION_CAP];
    size_t symbol_count;
    size_t declaration_count;
} gsh_canon_symbol_inventory;

void gsh_canon_symbols_initialize(gsh_canon_symbol_inventory *inventory);

int gsh_canon_symbols_add(gsh_canon_symbol_inventory *inventory,
                          const unsigned char *source, size_t length,
                          const char *path);

int gsh_canon_symbols_analyze(const gsh_canon_symbol_inventory *inventory,
                              size_t *unused_declarations, bool verbose);

#endif
