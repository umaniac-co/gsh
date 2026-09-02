#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "command_cache.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

_Static_assert((GSH_COMMAND_CACHE_HASH_CAP &
                (GSH_COMMAND_CACHE_HASH_CAP - 1U)) == 0,
               "command cache hash capacity must be a power of two");
_Static_assert(GSH_COMMAND_CACHE_HASH_CAP >= GSH_COMMAND_CACHE_CAP * 2U,
               "command cache hash table needs bounded spare capacity");
_Static_assert(GSH_COMMAND_PATH_SCAN_CAP >= GSH_COMMAND_PATH_CAP,
               "PATH scan budget must cover one command path");

static uint32_t command_hash(const char *name, size_t length)
{
    if (name == NULL) {
        return 0U;
    }
    uint32_t hash = UINT32_C(2166136261);
    size_t offset;

    for (offset = 0; offset < length; offset++) {
        hash ^= (unsigned char)name[offset];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool executable_candidate(const char *candidate)
{
    if (candidate == NULL) {
        return false;
    }
    struct stat status;

    return candidate != NULL && access(candidate, X_OK) == 0 &&
           stat(candidate, &status) == 0 && !S_ISDIR(status.st_mode);
}

static bool copy_candidate(const char *directory, size_t directory_length,
                           const char *name, size_t name_length,
                           char output[GSH_COMMAND_PATH_CAP])
{
    if (directory == NULL || name == NULL || output == NULL) {
        return false;
    }
    size_t used = directory_length == 0 ? 1U : directory_length;

    if (used + 1U + name_length + 1U > GSH_COMMAND_PATH_CAP) {
        return false;
    }
    if (directory_length == 0) {
        output[0] = '.';
    } else {
        (void)memcpy(output, directory, directory_length);
    }
    output[used++] = '/';
    (void)memcpy(output + used, name, name_length + 1U);
    return true;
}

/* -- One Search Primitive Serves Inspection, Hashing, and Execution -----
 * PATH is scanned once, with a byte budget independent of its component
 * count. Empty components deliberately name the current directory. The
 * result is only accepted when it is executable and is not a directory.
 * -------------------------------------------------------------------- */
int gsh_command_search_external(const char *name, const char *path,
                                char output[GSH_COMMAND_PATH_CAP])
{
    size_t name_length;
    size_t path_length;
    size_t offset = 0;
    size_t components = 0;

    if (name == NULL || path == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }
    name_length = strnlen(name, GSH_COMMAND_PATH_CAP);
    if (name_length == 0 || name_length == GSH_COMMAND_PATH_CAP) {
        errno = name_length == 0 ? EINVAL : ENAMETOOLONG;
        return -1;
    }
    if (strchr(name, '/') != NULL) {
        if (!executable_candidate(name)) {
            return 0;
        }
        (void)memcpy(output, name, name_length + 1U);
        return 1;
    }
    path_length = strnlen(path, GSH_COMMAND_PATH_SCAN_CAP + 1U);
    if (path_length > GSH_COMMAND_PATH_SCAN_CAP) {
        errno = E2BIG;
        return -1;
    }
    while (offset <= path_length &&
           components <= GSH_COMMAND_PATH_SCAN_CAP) {
        size_t end = offset;

        while (end < path_length && path[end] != ':') {
            end++;
        }
        if (end - offset < GSH_COMMAND_PATH_CAP &&
            copy_candidate(path + offset, end - offset, name, name_length,
                           output) && executable_candidate(output)) {
            return 1;
        }
        if (end == path_length) {
            return 0;
        }
        offset = end + 1U;
        components++;
    }
    errno = E2BIG;
    return -1;
}

void gsh_command_cache_initialize(gsh_command_cache *cache,
                                  uint64_t path_generation)
{
    if (cache == NULL) {
        return;
    }
    (void)memset(cache, 0, sizeof(*cache));
    cache->version = GSH_COMMAND_CACHE_VERSION;
    cache->observed_path_generation = path_generation;
}

void gsh_command_cache_clear(gsh_command_cache *cache,
                             uint64_t path_generation)
{
    if (cache == NULL) {
        return;
    }
    gsh_command_cache_initialize(cache, path_generation);
}

bool gsh_command_cache_sync(gsh_command_cache *cache,
                            uint64_t path_generation)
{
    if (cache == NULL ||
        cache->observed_path_generation == path_generation) {
        return false;
    }
    gsh_command_cache_clear(cache, path_generation);
    return true;
}

static bool entry_matches(const gsh_command_cache *cache,
                          const gsh_command_cache_entry *entry,
                          const char *name, size_t name_length,
                          uint32_t hash)
{
    if (cache == NULL || entry == NULL || name == NULL) {
        return false;
    }
    return entry->hash == hash && entry->name_length == name_length &&
           entry->name_offset < cache->text_used &&
           memcmp(cache->text + entry->name_offset, name, name_length) == 0;
}

static size_t cache_index(const gsh_command_cache *cache, const char *name,
                          size_t name_length, uint32_t hash)
{
    if (cache == NULL) {
        return 0U;
    }
    size_t slot = hash & (GSH_COMMAND_CACHE_HASH_CAP - 1U);
    size_t probes;

    for (probes = 0; probes < GSH_COMMAND_CACHE_HASH_CAP; probes++) {
        uint16_t encoded = cache->hash_slots[slot];

        if (encoded == 0) {
            return GSH_COMMAND_CACHE_CAP;
        }
        if ((size_t)(encoded - 1U) < cache->count &&
            entry_matches(cache, &cache->entries[encoded - 1U], name,
                          name_length, hash)) {
            return (size_t)(encoded - 1U);
        }
        slot = (slot + 1U) & (GSH_COMMAND_CACHE_HASH_CAP - 1U);
    }
    return GSH_COMMAND_CACHE_CAP;
}

static int insert_hash_slot(gsh_command_cache *cache, size_t index)
{
    if (cache == NULL) {
        return -1;
    }
    size_t slot = cache->entries[index].hash &
                  (GSH_COMMAND_CACHE_HASH_CAP - 1U);
    size_t probes;

    for (probes = 0; probes < GSH_COMMAND_CACHE_HASH_CAP; probes++) {
        if (cache->hash_slots[slot] == 0) {
            cache->hash_slots[slot] = (uint16_t)(index + 1U);
            return 0;
        }
        slot = (slot + 1U) & (GSH_COMMAND_CACHE_HASH_CAP - 1U);
    }
    errno = ENOSPC;
    return -1;
}

static void rebuild_hash(gsh_command_cache *cache)
{
    if (cache == NULL) {
        return;
    }
    size_t index;

    (void)memset(cache->hash_slots, 0, sizeof(cache->hash_slots));
    for (index = 0; index < cache->count; index++) {
        (void)insert_hash_slot(cache, index);
    }
}

static void remove_entry(gsh_command_cache *cache, size_t index)
{
    if (cache == NULL) {
        return;
    }
    size_t begin = cache->entries[index].name_offset;
    size_t end = cache->entries[index].path_offset +
                 cache->entries[index].path_length + 1U;
    size_t removed = end - begin;
    size_t other;

    (void)memmove(cache->text + begin, cache->text + end,
            cache->text_used - end);
    (void)memmove(cache->entries + index, cache->entries + index + 1U,
            (cache->count - index - 1U) * sizeof(cache->entries[0]));
    cache->count--;
    cache->text_used -= (uint32_t)removed;
    for (other = 0; other < cache->count; other++) {
        if (cache->entries[other].name_offset > begin) {
            cache->entries[other].name_offset -= (uint32_t)removed;
            cache->entries[other].path_offset -= (uint32_t)removed;
        }
    }
    (void)memset(&cache->entries[cache->count], 0,
           sizeof(cache->entries[0]));
    (void)memset(cache->text + cache->text_used, 0, removed);
    rebuild_hash(cache);
}

static int remember_path(gsh_command_cache *cache, const char *name,
                         size_t name_length, const char *path,
                         size_t path_length)
{
    if (cache == NULL) {
        return -1;
    }
    size_t required = name_length + path_length + 2U;
    size_t index;

    if (name_length == 0 || name_length > UINT16_MAX ||
        path_length == 0 || path_length > UINT16_MAX ||
        strchr(name, '/') != NULL || memchr(name, '\n', name_length) != NULL ||
        memchr(path, '\n', path_length) != NULL ||
        required > GSH_COMMAND_CACHE_TEXT_CAP) {
        errno = EINVAL;
        return -1;
    }
    index = cache_index(cache, name, name_length,
                        command_hash(name, name_length));
    if (index != GSH_COMMAND_CACHE_CAP) {
        return 0;
    }
    if (cache->count == GSH_COMMAND_CACHE_CAP ||
        required > GSH_COMMAND_CACHE_TEXT_CAP - cache->text_used) {
        errno = ENOSPC;
        return -1;
    }
    index = cache->count++;
    cache->entries[index].name_offset = cache->text_used;
    cache->entries[index].path_offset =
        cache->text_used + (uint32_t)name_length + 1U;
    cache->entries[index].hash = command_hash(name, name_length);
    cache->entries[index].name_length = (uint16_t)name_length;
    cache->entries[index].path_length = (uint16_t)path_length;
    (void)memcpy(cache->text + cache->text_used, name, name_length + 1U);
    (void)memcpy(cache->text + cache->entries[index].path_offset, path,
           path_length + 1U);
    cache->text_used += (uint32_t)required;
    if (insert_hash_slot(cache, index) == -1) {
        cache->count--;
        cache->text_used -= (uint32_t)required;
        (void)memset(&cache->entries[index], 0, sizeof(cache->entries[index]));
        return -1;
    }
    return 0;
}

const char *gsh_command_cache_lookup(const gsh_command_cache *cache,
                                     uint64_t path_generation,
                                     const char *name)
{
    size_t length;
    size_t index;

    if (cache == NULL || name == NULL ||
        cache->version != GSH_COMMAND_CACHE_VERSION ||
        cache->observed_path_generation != path_generation) {
        return NULL;
    }
    length = strnlen(name, GSH_COMMAND_PATH_CAP);
    if (length == 0 || length == GSH_COMMAND_PATH_CAP ||
        strchr(name, '/') != NULL) {
        return NULL;
    }
    index = cache_index(cache, name, length, command_hash(name, length));
    return index == GSH_COMMAND_CACHE_CAP
               ? NULL
               : cache->text + cache->entries[index].path_offset;
}

int gsh_command_cache_resolve(gsh_command_cache *cache,
                              uint64_t path_generation,
                              const char *name, const char *path,
                              bool require_remember, bool *changed,
                              char output[GSH_COMMAND_PATH_CAP])
{
    size_t name_length;
    size_t index;
    int found;
    int search_errno;

    if (changed != NULL) {
        *changed = false;
    }
    if (name == NULL || path == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (cache == NULL) {
        return gsh_command_search_external(name, path, output);
    }
    if (cache->version != GSH_COMMAND_CACHE_VERSION ||
        cache->reserved != 0 || cache->count > GSH_COMMAND_CACHE_CAP ||
        cache->text_used > GSH_COMMAND_CACHE_TEXT_CAP) {
        errno = EPROTO;
        return -1;
    }
    if (gsh_command_cache_sync(cache, path_generation)) {
        if (changed != NULL) {
            *changed = true;
        }
    }
    name_length = strnlen(name, GSH_COMMAND_PATH_CAP);
    if (name_length == 0 || name_length == GSH_COMMAND_PATH_CAP ||
        strchr(name, '/') != NULL) {
        return gsh_command_search_external(name, path, output);
    }
    index = cache_index(cache, name, name_length,
                        command_hash(name, name_length));
    if (index != GSH_COMMAND_CACHE_CAP) {
        const char *cached = cache->text + cache->entries[index].path_offset;

        if (executable_candidate(cached)) {
            (void)memcpy(output, cached, cache->entries[index].path_length + 1U);
            return 1;
        }
        remove_entry(cache, index);
        if (changed != NULL) {
            *changed = true;
        }
    }
    found = gsh_command_search_external(name, path, output);
    if (found != 1) {
        return found;
    }
    search_errno = errno;
    if (remember_path(cache, name, name_length, output, strlen(output)) ==
        -1) {
        if (require_remember) {
            return -1;
        }
        errno = search_errno;
        return 1;
    }
    if (changed != NULL) {
        *changed = true;
    }
    return 1;
}

size_t gsh_command_cache_count(const gsh_command_cache *cache)
{
    if (cache == NULL) {
        return 0U;
    }
    return cache == NULL ? 0U : cache->count;
}

const char *gsh_command_cache_name(const gsh_command_cache *cache,
                                   size_t index)
{
    if (cache == NULL) {
        return NULL;
    }
    return cache == NULL || index >= cache->count
               ? NULL
               : cache->text + cache->entries[index].name_offset;
}

const char *gsh_command_cache_path(const gsh_command_cache *cache,
                                   size_t index)
{
    if (cache == NULL) {
        return NULL;
    }
    return cache == NULL || index >= cache->count
               ? NULL
               : cache->text + cache->entries[index].path_offset;
}

bool gsh_command_cache_validate(const gsh_command_cache *cache)
{
    size_t index;
    size_t expected = 0;
    size_t populated = 0;

    if (cache == NULL || cache->version != GSH_COMMAND_CACHE_VERSION ||
        cache->reserved != 0 || cache->count > GSH_COMMAND_CACHE_CAP ||
        cache->text_used > GSH_COMMAND_CACHE_TEXT_CAP) {
        return false;
    }
    for (index = 0; index < GSH_COMMAND_CACHE_HASH_CAP; index++) {
        if (cache->hash_slots[index] != 0) {
            if ((size_t)(cache->hash_slots[index] - 1U) >= cache->count) {
                return false;
            }
            populated++;
        }
    }
    if (populated != cache->count) {
        return false;
    }
    for (index = 0; index < cache->count; index++) {
        const gsh_command_cache_entry *entry = &cache->entries[index];
        const char *name;
        const char *path;

        if (entry->name_offset != expected || entry->name_length == 0 ||
            entry->path_offset != expected + entry->name_length + 1U ||
            entry->path_length == 0 ||
            entry->path_offset > cache->text_used ||
            (size_t)entry->path_length + 1U >
                cache->text_used - entry->path_offset) {
            return false;
        }
        name = cache->text + entry->name_offset;
        path = cache->text + entry->path_offset;
        if (name[entry->name_length] != '\0' ||
            path[entry->path_length] != '\0' ||
            memchr(name, '\0', entry->name_length) != NULL ||
            memchr(path, '\0', entry->path_length) != NULL ||
            memchr(name, '\n', entry->name_length) != NULL ||
            memchr(path, '\n', entry->path_length) != NULL ||
            strchr(name, '/') != NULL ||
            entry->hash != command_hash(name, entry->name_length) ||
            cache_index(cache, name, entry->name_length, entry->hash) !=
                index) {
            return false;
        }
        expected = entry->path_offset + entry->path_length + 1U;
    }
    return expected == cache->text_used;
}

void gsh_command_cache_rebind(gsh_command_cache *cache,
                              uint64_t final_path_generation,
                              uint64_t parent_path_generation)
{
    if (cache == NULL) {
        return;
    }
    if (cache->observed_path_generation != final_path_generation) {
        gsh_command_cache_clear(cache, parent_path_generation);
    } else {
        cache->observed_path_generation = parent_path_generation;
    }
}
