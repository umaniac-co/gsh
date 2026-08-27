#ifndef GSH_ALIAS_EXPANSION_H
#define GSH_ALIAS_EXPANSION_H

#include "posix_parser.h"
#include "shell_aliases.h"

#include <stddef.h>

enum {
    GSH_ALIAS_EXPANSION_CAP = 65536,
    GSH_ALIAS_EXPANSION_LIMIT = 128,
};

gsh_parse_result gsh_alias_parse(
    const char *input, size_t length, const gsh_alias_store *aliases,
    char *expanded, size_t expanded_capacity, gsh_parse_storage *storage,
    const char **parsed_input, size_t *parsed_length);

#endif
