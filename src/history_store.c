#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "history_store.h"

#include <errno.h>
#include <string.h>

static const unsigned char history_magic[8] = {
    'G', 'S', 'H', 'H', 'I', 'S', 'T', '1'};

/* ── Fixed Slots Make History Cost Provable ─────────────────────
 * A linked list made command retention depend on allocator health and age.
 * History now owns exactly 1024 command slots with the editor's input bound.
 * Overwrite advances one ring index, so admission never allocates or scans.
 * Serialization carries only live entries and validates every length on load.
 * The fixed representation keeps corruption and resource recovery explicit.
 * ─────────────────────────────────────────────── */

static void encode_u32(unsigned char output[4], uint32_t value)
{
    output[0] = (unsigned char)(value >> 24);
    output[1] = (unsigned char)(value >> 16);
    output[2] = (unsigned char)(value >> 8);
    output[3] = (unsigned char)value;
}

static uint32_t decode_u32(const unsigned char input[4])
{
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | (uint32_t)input[3];
}

void gsh_history_initialize(gsh_history_store *store)
{
    if (store != NULL) {
        memset(store, 0, sizeof(*store));
    }
}

void gsh_history_clear(gsh_history_store *store)
{
    if (store != NULL) {
        memset(store, 0, sizeof(*store));
    }
}

static size_t physical_index(const gsh_history_store *store,
                             size_t chronological)
{
    return (store->start + chronological) % GSH_HISTORY_CAP;
}

int gsh_history_add(gsh_history_store *store, const char *command,
                    size_t length, bool deduplicate)
{
    size_t slot;

    if (store == NULL || command == NULL || length == 0 ||
        length >= GSH_HISTORY_ENTRY_CAP || store->count > GSH_HISTORY_CAP) {
        errno = EINVAL;
        return -1;
    }
    if (deduplicate && store->count != 0) {
        size_t latest = physical_index(store, store->count - 1U);

        if (store->lengths[latest] == length &&
            memcmp(store->entries[latest], command, length) == 0) {
            return 0;
        }
    }
    if (store->count < GSH_HISTORY_CAP) {
        slot = physical_index(store, store->count++);
    } else {
        slot = store->start;
        store->start = (store->start + 1U) % GSH_HISTORY_CAP;
    }
    memcpy(store->entries[slot], command, length);
    store->entries[slot][length] = '\0';
    store->lengths[slot] = (uint16_t)length;
    return 1;
}

const char *gsh_history_from_newest(const gsh_history_store *store,
                                    size_t position, size_t *length)
{
    size_t slot;

    if (length != NULL) {
        *length = 0;
    }
    if (store == NULL || length == NULL || position >= store->count ||
        store->count > GSH_HISTORY_CAP) {
        return NULL;
    }
    slot = physical_index(store, store->count - position - 1U);
    *length = store->lengths[slot];
    return store->entries[slot];
}

static bool contains(const char *text, size_t text_length,
                     const char *query, size_t query_length)
{
    size_t offset;

    if (query_length == 0) {
        return true;
    }
    if (query_length > text_length) {
        return false;
    }
    for (offset = 0; offset + query_length <= text_length; offset++) {
        if (memcmp(text + offset, query, query_length) == 0) {
            return true;
        }
    }
    return false;
}

int gsh_history_search_reverse(const gsh_history_store *store,
                               const char *query, size_t query_length,
                               size_t before, size_t *position)
{
    size_t candidate;

    if (store == NULL || query == NULL || position == NULL ||
        query_length >= GSH_HISTORY_ENTRY_CAP || before > store->count ||
        store->count > GSH_HISTORY_CAP) {
        errno = EINVAL;
        return -1;
    }
    for (candidate = before; candidate < store->count; candidate++) {
        size_t length;
        const char *entry = gsh_history_from_newest(
            store, candidate, &length);

        if (entry != NULL && contains(entry, length, query, query_length)) {
            *position = candidate;
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

size_t gsh_history_serialize(const gsh_history_store *store,
                             unsigned char *output, size_t capacity)
{
    size_t used = 12;
    size_t index;

    if (store == NULL || output == NULL || store->count > GSH_HISTORY_CAP ||
        capacity < used) {
        errno = EINVAL;
        return 0;
    }
    memcpy(output, history_magic, sizeof(history_magic));
    encode_u32(output + 8, (uint32_t)store->count);
    for (index = 0; index < store->count; index++) {
        size_t slot = physical_index(store, index);
        size_t length = store->lengths[slot];

        if (length == 0 || length >= GSH_HISTORY_ENTRY_CAP ||
            length + 4U > capacity - used) {
            errno = ENOBUFS;
            return 0;
        }
        encode_u32(output + used, (uint32_t)length);
        used += 4U;
        memcpy(output + used, store->entries[slot], length);
        used += length;
    }
    return used;
}

int gsh_history_deserialize(gsh_history_store *store,
                            const unsigned char *input, size_t length)
{
    uint32_t count;
    size_t offset = 12;
    size_t index;

    if (store == NULL || input == NULL || length < offset ||
        memcmp(input, history_magic, sizeof(history_magic)) != 0) {
        errno = EPROTO;
        return -1;
    }
    count = decode_u32(input + 8);
    if (count > GSH_HISTORY_CAP) {
        errno = EOVERFLOW;
        return -1;
    }
    gsh_history_initialize(store);
    for (index = 0; index < count; index++) {
        uint32_t entry_length;

        if (offset + 4U > length) {
            errno = EPROTO;
            return -1;
        }
        entry_length = decode_u32(input + offset);
        offset += 4U;
        if (entry_length == 0 || entry_length >= GSH_HISTORY_ENTRY_CAP ||
            entry_length > length - offset ||
            gsh_history_add(store, (const char *)input + offset,
                            entry_length, false) == -1) {
            errno = EPROTO;
            return -1;
        }
        offset += entry_length;
    }
    if (offset != length) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}
