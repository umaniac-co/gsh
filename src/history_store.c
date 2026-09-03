#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "history_store.h"

#include <errno.h>
#include <string.h>

/* ── Fixed Slots Make History Cost Provable ────────────────────
 * A linked list made command retention depend on allocator health and age.
 * History now owns exactly 1024 command slots with the editor's input bound.
 * Overwrite advances one ring index, so admission never allocates or scans.
 * Disk persistence is a separate text adapter that admits validated entries.
 * The fixed representation keeps corruption and resource recovery explicit.
 * ────────────────────────────────────────────── */

void gsh_history_initialize(gsh_history_store *store)
{
    if (store != NULL) {
        (void)memset(store->lengths, 0, sizeof(store->lengths));
        store->start = 0;
        store->count = 0;
        store->next_event = 1U;
    }
}

void gsh_history_clear(gsh_history_store *store)
{
    if (store != NULL) {
        (void)memset(store, 0, sizeof(*store));
        store->next_event = 1U;
    }
}

static size_t physical_index(const gsh_history_store *store,
                             size_t chronological)
{
    if (store == NULL) {
        return 0U;
    }
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
    (void)memcpy(store->entries[slot], command, length);
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
    if (store == NULL) {
        return 0U;
    }
    return store->count == 0 || store->next_event <= store->count
               ? 0
               : store->next_event - store->count;
}

uint64_t gsh_history_newest_event(const gsh_history_store *store)
{
    if (store == NULL) {
        return 0U;
    }
    return store->count == 0 || store->next_event == 0
               ? 0
               : store->next_event - 1U;
}

const char *gsh_history_event(const gsh_history_store *store,
                              uint64_t event, size_t *length)
{
    uint64_t oldest;
    size_t chronological;
    size_t slot;

    if (store == NULL) {
        return NULL;
    }
    oldest = gsh_history_oldest_event(store);
    if (length != NULL) {
        *length = 0;
    }
    if (length == NULL || oldest == 0 || event < oldest ||
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
    uint64_t oldest;
    uint64_t candidate;
    size_t checked;

    if (store == NULL) {
        return -1;
    }
    oldest = gsh_history_oldest_event(store);
    if (prefix == NULL || event == NULL ||
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
