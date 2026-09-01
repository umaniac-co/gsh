#ifndef GSH_COMMAND_CACHE_H
#define GSH_COMMAND_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_COMMAND_CACHE_CAP = 128,
    GSH_COMMAND_CACHE_HASH_CAP = 256,
    GSH_COMMAND_CACHE_TEXT_CAP = 65536,
    GSH_COMMAND_PATH_CAP = 4096,
    GSH_COMMAND_PATH_SCAN_CAP = 32768,
    GSH_COMMAND_CACHE_VERSION = 1,
};

typedef struct {
    uint32_t name_offset;
    uint32_t path_offset;
    uint32_t hash;
    uint16_t name_length;
    uint16_t path_length;
} gsh_command_cache_entry;

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t text_used;
    uint32_t reserved;
    uint64_t observed_path_generation;
    gsh_command_cache_entry entries[GSH_COMMAND_CACHE_CAP];
    uint16_t hash_slots[GSH_COMMAND_CACHE_HASH_CAP];
    char text[GSH_COMMAND_CACHE_TEXT_CAP];
} gsh_command_cache;

void gsh_command_cache_initialize(gsh_command_cache *cache,
                                  uint64_t path_generation);
void gsh_command_cache_clear(gsh_command_cache *cache,
                             uint64_t path_generation);
bool gsh_command_cache_sync(gsh_command_cache *cache,
                            uint64_t path_generation);
bool gsh_command_cache_validate(const gsh_command_cache *cache);

const char *gsh_command_cache_lookup(const gsh_command_cache *cache,
                                     uint64_t path_generation,
                                     const char *name);
int gsh_command_cache_resolve(gsh_command_cache *cache,
                              uint64_t path_generation,
                              const char *name, const char *path,
                              bool require_remember, bool *changed,
                              char output[GSH_COMMAND_PATH_CAP]);
int gsh_command_search_external(const char *name, const char *path,
                                char output[GSH_COMMAND_PATH_CAP]);

size_t gsh_command_cache_count(const gsh_command_cache *cache);
const char *gsh_command_cache_name(const gsh_command_cache *cache,
                                   size_t index);
const char *gsh_command_cache_path(const gsh_command_cache *cache,
                                   size_t index);
void gsh_command_cache_rebind(gsh_command_cache *cache,
                              uint64_t final_path_generation,
                              uint64_t parent_path_generation);

#endif
