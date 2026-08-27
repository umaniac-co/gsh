#ifndef GSH_POSITIONAL_PARAMETERS_H
#define GSH_POSITIONAL_PARAMETERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_POSITIONAL_CAP = 128,
    GSH_POSITIONAL_TEXT_CAP = 16384,
    GSH_POSITIONAL_VERSION = 1,
};

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t start;
    uint32_t text_used;
    uint16_t offsets[GSH_POSITIONAL_CAP];
    char text[GSH_POSITIONAL_TEXT_CAP];
} gsh_positional_store;

void gsh_positionals_initialize(gsh_positional_store *store);
int gsh_positionals_assign(gsh_positional_store *store, size_t count,
                           char *const values[]);
int gsh_positionals_shift(gsh_positional_store *store, size_t amount);
size_t gsh_positionals_count(const gsh_positional_store *store);
void gsh_positionals_view(const gsh_positional_store *store,
                          char *values[GSH_POSITIONAL_CAP]);
bool gsh_positionals_validate(const gsh_positional_store *store);

#endif
