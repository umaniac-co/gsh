#ifndef GSH_SHELL_FUNCTIONS_H
#define GSH_SHELL_FUNCTIONS_H

#include "posix_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_FUNCTION_CAP = 128,
    GSH_FUNCTION_HASH_CAP = 256,
    GSH_FUNCTION_DEPTH_CAP = 32,
    GSH_FUNCTION_SNAPSHOT_VERSION = 1,
};

#define GSH_FUNCTION_TEXT_CAP \
    (sizeof(((gsh_parse_storage *)0)->tokens))

typedef struct {
    uint32_t source_offset;
    uint32_t source_length;
    uint32_t source_capacity;
    uint32_t node_offset;
    uint32_t node_capacity;
    uint32_t word_offset;
    uint32_t word_capacity;
    uint32_t redirect_offset;
    uint32_t redirect_capacity;
    uint32_t hash;
    uint16_t name_length;
    uint16_t reserved;
} gsh_function_entry;

typedef struct {
    gsh_function_entry entries[GSH_FUNCTION_CAP];
    uint16_t hash_slots[GSH_FUNCTION_HASH_CAP];
    uint32_t count;
    uint32_t text_used;
    gsh_parse_storage programs;
} gsh_function_store;

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t text_used;
    uint32_t node_count;
    uint32_t word_count;
    uint32_t redirect_count;
    uint32_t reserved;
    uint64_t base_generation;
} gsh_function_snapshot_header;

void gsh_functions_initialize(gsh_function_store *store);
const gsh_function_entry *gsh_functions_lookup(
    const gsh_function_store *store, const char *name,
    size_t name_length);
int gsh_functions_set(gsh_function_store *store,
                      gsh_function_store *scratch,
                      const char *input, size_t input_length,
                      const gsh_parse_storage *storage,
                      size_t function_node,
                      int preserve_active_programs);
int gsh_functions_unset(gsh_function_store *store,
                        const char *name, size_t name_length);
size_t gsh_functions_count(const gsh_function_store *store);
const char *gsh_functions_text(const gsh_function_store *store);
void gsh_functions_snapshot_header(
    const gsh_function_store *store, uint64_t base_generation,
    gsh_function_snapshot_header *header);
bool gsh_functions_snapshot_header_valid(
    const gsh_function_snapshot_header *header);
size_t gsh_functions_snapshot_payload_size(
    const gsh_function_snapshot_header *header);
const void *gsh_functions_snapshot_source(
    const gsh_function_store *store,
    const gsh_function_snapshot_header *header, size_t offset,
    size_t *available);
void *gsh_functions_snapshot_destination(
    gsh_function_store *store,
    const gsh_function_snapshot_header *header, size_t offset,
    size_t *available);
bool gsh_functions_snapshot_finalize(
    gsh_function_store *store,
    const gsh_function_snapshot_header *header);
bool gsh_functions_clone(gsh_function_store *destination,
                         const gsh_function_store *source);

#endif
