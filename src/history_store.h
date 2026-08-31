#ifndef GSH_HISTORY_STORE_H
#define GSH_HISTORY_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_HISTORY_CAP = 1024,
    GSH_HISTORY_ENTRY_CAP = 4096,
    GSH_HISTORY_SERIALIZED_CAP =
        32 + GSH_HISTORY_CAP * (4 + GSH_HISTORY_ENTRY_CAP),
};

typedef struct {
    uint16_t lengths[GSH_HISTORY_CAP];
    size_t start;
    size_t count;
    char entries[GSH_HISTORY_CAP][GSH_HISTORY_ENTRY_CAP];
} gsh_history_store;

void gsh_history_initialize(gsh_history_store *store);
int gsh_history_add(gsh_history_store *store, const char *command,
                    size_t length, bool deduplicate);
const char *gsh_history_from_newest(const gsh_history_store *store,
                                    size_t position, size_t *length);
int gsh_history_search_reverse(const gsh_history_store *store,
                               const char *query, size_t query_length,
                               size_t before, size_t *position);
size_t gsh_history_serialize(const gsh_history_store *store,
                             unsigned char *output, size_t capacity);
int gsh_history_deserialize(gsh_history_store *store,
                            const unsigned char *input, size_t length);
void gsh_history_clear(gsh_history_store *store);

#endif
