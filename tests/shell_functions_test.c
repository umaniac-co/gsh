#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh function tests require the POSIX.1-2024 baseline"
#endif

#include "../src/shell_functions.h"

#include <stdio.h>
#include <string.h>

static size_t function_node(const gsh_parse_storage *storage,
                            const char *input, const char *name)
{
    if (input == NULL || name == NULL || storage == NULL) {
        return 0U;
    }
    size_t index;
    size_t length = strlen(name);

    for (index = 0; index < storage->node_count; index++) {
        const gsh_ast_node *node = &storage->nodes[index];

        if (node->kind == GSH_AST_FUNCTION && node->word_count == 1U) {
            gsh_word_ref word = storage->words[node->first_word];

            if (word.end - word.begin == length &&
                memcmp(input + word.begin, name, length) == 0) {
                return index;
            }
        }
    }
    return GSH_AST_NONE;
}

int main(void)
{
    static const char input[] =
        "alpha(){ /usr/bin/printf '%s' \"$1\"; } >\"$2\"; "
        "beta(){ :; }";
    static gsh_parse_storage parsed_storage;
    static gsh_function_store store_storage;
    static gsh_function_store clone_storage;
    gsh_parse_storage *parsed = &parsed_storage;
    gsh_function_store *store = &store_storage;
    gsh_function_store *clone = &clone_storage;
    gsh_function_snapshot_header header;
    gsh_parse_result result;
    size_t alpha;
    size_t beta;

    result = gsh_parse(input, sizeof(input) - 1U, parsed);
    alpha = function_node(parsed, input, "alpha");
    beta = function_node(parsed, input, "beta");
    gsh_functions_initialize(store);
    gsh_functions_initialize(clone);
    if (result.status != GSH_PARSE_OK || alpha == GSH_AST_NONE ||
        beta == GSH_AST_NONE ||
        gsh_functions_set(store, clone, input, sizeof(input) - 1U,
                          parsed, alpha, 0) == -1 ||
        gsh_functions_set(store, clone, input, sizeof(input) - 1U,
                          parsed, beta, 0) == -1 ||
        gsh_functions_count(store) != 2U ||
        !gsh_functions_clone(clone, store) ||
        gsh_functions_lookup(clone, "alpha", 5) == NULL ||
        gsh_functions_lookup(clone, "beta", 4) == NULL) {
        (void)fprintf(stderr, "function test: build or clone failed\n");
        return 1;
    }

    gsh_functions_snapshot_header(store, 17, &header);
    if (!gsh_functions_snapshot_header_valid(&header) ||
        header.base_generation != 17 ||
        gsh_functions_snapshot_payload_size(&header) == 0) {
        (void)fprintf(stderr, "function test: valid snapshot rejected\n");
        return 1;
    }
    header.version++;
    if (gsh_functions_snapshot_header_valid(&header)) {
        (void)fprintf(stderr, "function test: version corruption accepted\n");
        return 1;
    }
    gsh_functions_snapshot_header(store, 17, &header);
    header.node_count = GSH_PARSE_NODE_CAP + 1U;
    if (gsh_functions_snapshot_header_valid(&header)) {
        (void)fprintf(stderr, "function test: count corruption accepted\n");
        return 1;
    }

    if (!gsh_functions_clone(clone, store)) {
        return 1;
    }
    gsh_functions_snapshot_header(clone, 17, &header);
    clone->entries[0].source_offset = header.text_used;
    if (gsh_functions_snapshot_finalize(clone, &header)) {
        (void)fprintf(stderr, "function test: source corruption accepted\n");
        return 1;
    }
    if (!gsh_functions_clone(clone, store)) {
        return 1;
    }
    gsh_functions_snapshot_header(clone, 17, &header);
    clone->programs.nodes[clone->entries[0].node_offset].first_child =
        GSH_PARSE_NODE_CAP;
    if (gsh_functions_snapshot_finalize(clone, &header)) {
        (void)fprintf(stderr, "function test: AST corruption accepted\n");
        return 1;
    }
    if (!gsh_functions_clone(clone, store)) {
        return 1;
    }
    gsh_functions_snapshot_header(clone, 17, &header);
    clone->entries[1] = clone->entries[0];
    if (gsh_functions_snapshot_finalize(clone, &header)) {
        (void)fprintf(stderr, "function test: duplicate name accepted\n");
        return 1;
    }

    if (gsh_functions_unset(store, "alpha", 5) == -1 ||
        gsh_functions_lookup(store, "alpha", 5) != NULL ||
        gsh_functions_lookup(store, "beta", 4) == NULL) {
        (void)fprintf(stderr, "function test: unset damaged namespace\n");
        return 1;
    }
    (void)puts("function store: clone, snapshot validation, corruption, and unset passed");
    return 0;
}
