#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "history_store.h"

#include <errno.h>
#include <string.h>

static const unsigned char history_magic_v1[8] = {
    'G', 'S', 'H', 'H', 'I', 'S', 'T', '1'};
static const unsigned char history_magic_v2[8] = {
    'G', 'S', 'H', 'H', 'I', 'S', 'T', '2'};

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

static void encode_u64(unsigned char output[8], uint64_t value)
{
    size_t index;

    for (index = 0; index < 8U; index++) {
        output[7U - index] = (unsigned char)(value >> (index * 8U));
    }
}

static uint64_t decode_u64(const unsigned char input[8])
{
    uint64_t value = 0;
    size_t index;

    for (index = 0; index < 8U; index++) {
        value = (value << 8U) | input[index];
    }
    return value;
}

void gsh_history_initialize(gsh_history_store *store)
{
    if (store != NULL) {
        memset(store->lengths, 0, sizeof(store->lengths));
        store->start = 0;
        store->count = 0;
        store->next_event = 1U;
    }
}

void gsh_history_clear(gsh_history_store *store)
{
    if (store != NULL) {
        memset(store, 0, sizeof(*store));
        store->next_event = 1U;
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
        length >= GSH_HISTORY_ENTRY_CAP || store->count > GSH_HISTORY_CAP ||
        store->next_event == 0 || store->next_event <= store->count) {
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
    if (store->next_event == UINT64_MAX) {
        errno = EOVERFLOW;
        return -1;
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
    store->next_event++;
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

uint64_t gsh_history_oldest_event(const gsh_history_store *store)
{
    return store == NULL || store->count == 0 ||
                   store->next_event <= store->count
               ? 0
               : store->next_event - store->count;
}

uint64_t gsh_history_newest_event(const gsh_history_store *store)
{
    return store == NULL || store->count == 0 || store->next_event == 0
               ? 0
               : store->next_event - 1U;
}

const char *gsh_history_event(const gsh_history_store *store,
                              uint64_t event, size_t *length)
{
    uint64_t oldest = gsh_history_oldest_event(store);
    size_t chronological;
    size_t slot;

    if (length != NULL) {
        *length = 0;
    }
    if (store == NULL || length == NULL || oldest == 0 || event < oldest ||
        event >= store->next_event) {
        return NULL;
    }
    chronological = (size_t)(event - oldest);
    slot = physical_index(store, chronological);
    *length = store->lengths[slot];
    return store->entries[slot];
}

int gsh_history_find_prefix(const gsh_history_store *store,
                            const char *prefix, size_t prefix_length,
                            uint64_t before_event, uint64_t *event)
{
    uint64_t oldest = gsh_history_oldest_event(store);
    uint64_t candidate;
    size_t checked;

    if (store == NULL || prefix == NULL || event == NULL ||
        prefix_length >= GSH_HISTORY_ENTRY_CAP || oldest == 0) {
        errno = EINVAL;
        return -1;
    }
    candidate = before_event >= store->next_event
                    ? store->next_event - 1U : before_event;
    for (checked = 0; checked < store->count && candidate >= oldest;
         checked++, candidate--) {
        size_t length;
        const char *entry = gsh_history_event(store, candidate, &length);

        if (entry != NULL && prefix_length <= length &&
            memcmp(entry, prefix, prefix_length) == 0) {
            *event = candidate;
            return 0;
        }
    }
    errno = ENOENT;
    return -1;
}

size_t gsh_history_serialize(const gsh_history_store *store,
                             unsigned char *output, size_t capacity)
{
    size_t used = 20;
    size_t index;

    if (store == NULL || output == NULL || store->count > GSH_HISTORY_CAP ||
        store->next_event == 0 || store->next_event <= store->count ||
        capacity < used) {
        errno = EINVAL;
        return 0;
    }
    memcpy(output, history_magic_v2, sizeof(history_magic_v2));
    encode_u32(output + 8, (uint32_t)store->count);
    encode_u64(output + 12, store->next_event);
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
    uint64_t restored_next = 0;
    size_t offset;
    size_t index;

    if (store == NULL || input == NULL || length < 12U ||
        (memcmp(input, history_magic_v1, sizeof(history_magic_v1)) != 0 &&
         memcmp(input, history_magic_v2, sizeof(history_magic_v2)) != 0)) {
        errno = EPROTO;
        return -1;
    }
    count = decode_u32(input + 8);
    if (count > GSH_HISTORY_CAP) {
        errno = EOVERFLOW;
        return -1;
    }
    offset = memcmp(input, history_magic_v2, sizeof(history_magic_v2)) == 0
                 ? 20U : 12U;
    if (length < offset) {
        errno = EPROTO;
        return -1;
    }
    gsh_history_initialize(store);
    if (offset == 20U) {
        restored_next = decode_u64(input + 12);
        if (restored_next == 0 || restored_next <= count) {
            errno = EPROTO;
            return -1;
        }
    }
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
    store->next_event = restored_next != 0
                            ? restored_next
                            : (uint64_t)store->count + 1U;
    return 0;
}
