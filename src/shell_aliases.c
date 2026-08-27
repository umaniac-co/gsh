#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "shell_aliases.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

static bool alias_byte(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9') || byte == '!' || byte == '%' ||
           byte == ',' || byte == '-' || byte == '@' || byte == '_';
}

bool gsh_alias_name_is_valid(const char *name, size_t length)
{
    size_t offset;

    if (name == NULL || length == 0) {
        return false;
    }
    for (offset = 0; offset < length; offset++) {
        if (!alias_byte((unsigned char)name[offset])) {
            return false;
        }
    }
    return true;
}

static uint32_t alias_hash(const char *name, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    size_t offset;

    for (offset = 0; offset < length; offset++) {
        hash ^= (unsigned char)name[offset];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

void gsh_aliases_initialize(gsh_alias_store *store)
{
    memset(store, 0, sizeof(*store));
}

static bool entry_matches(const gsh_alias_store *store,
                          const gsh_alias_entry *entry, const char *name,
                          size_t name_length, uint32_t hash)
{
    const char *assignment;

    if (entry->hash != hash || entry->name_length != name_length ||
        entry->assignment_offset >= store->text_used) {
        return false;
    }
    assignment = store->text + entry->assignment_offset;
    return memcmp(assignment, name, name_length) == 0 &&
           assignment[name_length] == '=';
}

static size_t alias_index(const gsh_alias_store *store, const char *name,
                          size_t name_length, uint32_t hash)
{
    size_t slot = hash & (GSH_ALIAS_HASH_CAP - 1U);
    size_t probes;

    for (probes = 0; probes < GSH_ALIAS_HASH_CAP; probes++) {
        uint16_t encoded = store->hash_slots[slot];

        if (encoded == 0) {
            return GSH_ALIAS_CAP;
        }
        if ((size_t)(encoded - 1U) < store->count &&
            entry_matches(store, &store->entries[encoded - 1U], name,
                          name_length, hash)) {
            return (size_t)(encoded - 1U);
        }
        slot = (slot + 1U) & (GSH_ALIAS_HASH_CAP - 1U);
    }
    return GSH_ALIAS_CAP;
}

static int insert_hash(gsh_alias_store *store, size_t index)
{
    size_t slot = store->entries[index].hash & (GSH_ALIAS_HASH_CAP - 1U);
    size_t probes;

    for (probes = 0; probes < GSH_ALIAS_HASH_CAP; probes++) {
        if (store->hash_slots[slot] == 0) {
            store->hash_slots[slot] = (uint16_t)(index + 1U);
            return 0;
        }
        slot = (slot + 1U) & (GSH_ALIAS_HASH_CAP - 1U);
    }
    errno = ENOSPC;
    return -1;
}

static int rebuild_hash(gsh_alias_store *store)
{
    size_t index;

    memset(store->hash_slots, 0, sizeof(store->hash_slots));
    for (index = 0; index < store->count; index++) {
        if (insert_hash(store, index) == -1) {
            return -1;
        }
    }
    return 0;
}

const char *gsh_aliases_lookup(const gsh_alias_store *store,
                               const char *name, size_t name_length,
                               size_t *index)
{
    size_t found;

    if (index != NULL) {
        *index = GSH_ALIAS_CAP;
    }
    if (store == NULL || !gsh_alias_name_is_valid(name, name_length)) {
        return NULL;
    }
    found = alias_index(store, name, name_length,
                        alias_hash(name, name_length));
    if (found == GSH_ALIAS_CAP) {
        return NULL;
    }
    if (index != NULL) {
        *index = found;
    }
    return store->text + store->entries[found].assignment_offset +
           name_length + 1U;
}

int gsh_aliases_set(gsh_alias_store *store, const char *name,
                    size_t name_length, const char *value,
                    size_t value_length)
{
    size_t index;
    size_t length;
    uint32_t hash;
    bool inserted = false;

    if (store == NULL || value == NULL ||
        !gsh_alias_name_is_valid(name, name_length) ||
        name_length > UINT16_MAX || value_length > GSH_ALIAS_VALUE_CAP ||
        name_length > SIZE_MAX - value_length - 2U) {
        errno = EINVAL;
        return -1;
    }
    length = name_length + value_length + 2U;
    if (length > GSH_ALIAS_TEXT_CAP) {
        errno = E2BIG;
        return -1;
    }
    hash = alias_hash(name, name_length);
    index = alias_index(store, name, name_length, hash);
    if (index != GSH_ALIAS_CAP) {
        gsh_alias_entry *entry = &store->entries[index];
        size_t old_offset = entry->assignment_offset;
        size_t old_length = entry->assignment_length;
        size_t tail_offset = old_offset + old_length;
        size_t tail_length = store->text_used - tail_offset;
        size_t other;

        if (length > old_length &&
            length - old_length > GSH_ALIAS_TEXT_CAP - store->text_used) {
            errno = ENOSPC;
            return -1;
        }
        memmove(store->text + old_offset + length,
                store->text + tail_offset, tail_length);
        if (length != old_length) {
            ptrdiff_t delta = (ptrdiff_t)length - (ptrdiff_t)old_length;

            for (other = 0; other < store->count; other++) {
                if (other != index &&
                    store->entries[other].assignment_offset > old_offset) {
                    store->entries[other].assignment_offset =
                        (uint32_t)((ptrdiff_t)store->entries[other]
                                       .assignment_offset +
                                   delta);
                }
            }
            store->text_used =
                (uint32_t)((ptrdiff_t)store->text_used + delta);
        }
        entry->assignment_length = (uint32_t)length;
    } else {
        if (store->count == GSH_ALIAS_CAP ||
            length > GSH_ALIAS_TEXT_CAP - store->text_used) {
            errno = ENOSPC;
            return -1;
        }
        index = store->count++;
        store->entries[index].assignment_offset = store->text_used;
        store->entries[index].assignment_length = (uint32_t)length;
        store->entries[index].hash = hash;
        store->entries[index].name_length = (uint16_t)name_length;
        store->entries[index].reserved = 0;
        store->text_used += (uint32_t)length;
        inserted = true;
    }
    {
        char *assignment = store->text +
                           store->entries[index].assignment_offset;

        memcpy(assignment, name, name_length);
        assignment[name_length] = '=';
        memcpy(assignment + name_length + 1U, value, value_length);
        assignment[name_length + value_length + 1U] = '\0';
    }
    return inserted ? insert_hash(store, index) : 0;
}

int gsh_aliases_unset(gsh_alias_store *store, const char *name,
                      size_t name_length)
{
    size_t index;
    size_t offset;
    size_t length;
    size_t other;

    if (store == NULL || !gsh_alias_name_is_valid(name, name_length)) {
        errno = EINVAL;
        return -1;
    }
    index = alias_index(store, name, name_length,
                        alias_hash(name, name_length));
    if (index == GSH_ALIAS_CAP) {
        errno = ENOENT;
        return -1;
    }
    offset = store->entries[index].assignment_offset;
    length = store->entries[index].assignment_length;
    memmove(store->text + offset, store->text + offset + length,
            store->text_used - offset - length);
    for (other = 0; other < store->count; other++) {
        if (other != index &&
            store->entries[other].assignment_offset > offset) {
            store->entries[other].assignment_offset -= (uint32_t)length;
        }
    }
    memmove(store->entries + index, store->entries + index + 1U,
            (store->count - index - 1U) * sizeof(store->entries[0]));
    store->count--;
    store->text_used -= (uint32_t)length;
    memset(store->text + store->text_used, 0, length);
    memset(&store->entries[store->count], 0, sizeof(store->entries[0]));
    return rebuild_hash(store);
}

void gsh_aliases_clear(gsh_alias_store *store)
{
    if (store != NULL) {
        memset(store, 0, sizeof(*store));
    }
}

size_t gsh_aliases_count(const gsh_alias_store *store)
{
    return store == NULL ? 0U : store->count;
}

const char *gsh_aliases_assignment(const gsh_alias_store *store,
                                   size_t index)
{
    if (store == NULL || index >= store->count) {
        return NULL;
    }
    return store->text + store->entries[index].assignment_offset;
}

void gsh_alias_journal_initialize(gsh_alias_journal *journal,
                                  uint64_t base_generation)
{
    memset(journal, 0, sizeof(*journal));
    journal->version = GSH_ALIAS_JOURNAL_VERSION;
    journal->base_generation = base_generation;
}

static int record_alias(gsh_alias_journal *journal, unsigned int operation,
                        const char *name, size_t name_length,
                        const char *value, size_t value_length)
{
    gsh_alias_journal_entry *entry;
    size_t length = operation == GSH_ALIAS_JOURNAL_CLEAR
                        ? 0U
                        : name_length + 1U +
                              (operation == GSH_ALIAS_JOURNAL_SET
                                   ? value_length + 1U
                                   : 0U);

    if (journal == NULL || journal->version != GSH_ALIAS_JOURNAL_VERSION ||
        journal->count >= GSH_ALIAS_JOURNAL_CAP ||
        length > GSH_ALIAS_JOURNAL_TEXT_CAP - journal->text_used ||
        (operation != GSH_ALIAS_JOURNAL_CLEAR &&
         !gsh_alias_name_is_valid(name, name_length)) ||
        (operation == GSH_ALIAS_JOURNAL_SET &&
         (value == NULL || value_length > GSH_ALIAS_VALUE_CAP))) {
        errno = ENOSPC;
        return -1;
    }
    entry = &journal->entries[journal->count++];
    entry->text_offset = journal->text_used;
    entry->text_length = (uint32_t)length;
    entry->name_length = (uint16_t)name_length;
    entry->operation = (uint8_t)operation;
    entry->reserved = 0;
    if (operation != GSH_ALIAS_JOURNAL_CLEAR) {
        char *text = journal->text + journal->text_used;

        memcpy(text, name, name_length);
        text[name_length] = operation == GSH_ALIAS_JOURNAL_SET ? '=' : '\0';
        if (operation == GSH_ALIAS_JOURNAL_SET) {
            memcpy(text + name_length + 1U, value, value_length);
            text[name_length + value_length + 1U] = '\0';
        }
        journal->text_used += (uint32_t)length;
    }
    return 0;
}

int gsh_alias_journal_record_set(gsh_alias_journal *journal,
                                 const char *name, size_t name_length,
                                 const char *value, size_t value_length)
{
    return record_alias(journal, GSH_ALIAS_JOURNAL_SET, name, name_length,
                        value, value_length);
}

int gsh_alias_journal_record_unset(gsh_alias_journal *journal,
                                   const char *name, size_t name_length)
{
    return record_alias(journal, GSH_ALIAS_JOURNAL_UNSET, name, name_length,
                        NULL, 0);
}

int gsh_alias_journal_record_clear(gsh_alias_journal *journal)
{
    return record_alias(journal, GSH_ALIAS_JOURNAL_CLEAR, NULL, 0, NULL, 0);
}

bool gsh_alias_journal_validate(const gsh_alias_journal *journal)
{
    size_t index;
    size_t used = 0;

    if (journal == NULL || journal->version != GSH_ALIAS_JOURNAL_VERSION ||
        journal->reserved != 0 || journal->count > GSH_ALIAS_JOURNAL_CAP ||
        journal->text_used > GSH_ALIAS_JOURNAL_TEXT_CAP) {
        return false;
    }
    for (index = 0; index < journal->count; index++) {
        const gsh_alias_journal_entry *entry = &journal->entries[index];
        const char *text;

        if (entry->reserved != 0 || entry->text_offset != used ||
            entry->operation > GSH_ALIAS_JOURNAL_CLEAR ||
            entry->text_length > journal->text_used - used) {
            return false;
        }
        if (entry->operation == GSH_ALIAS_JOURNAL_CLEAR) {
            if (entry->text_length != 0 || entry->name_length != 0) {
                return false;
            }
            continue;
        }
        text = journal->text + used;
        if (entry->name_length == 0 ||
            entry->name_length >= entry->text_length ||
            !gsh_alias_name_is_valid(text, entry->name_length)) {
            return false;
        }
        if (entry->operation == GSH_ALIAS_JOURNAL_SET) {
            if (text[entry->name_length] != '=' ||
                text[entry->text_length - 1U] != '\0') {
                return false;
            }
        } else if (entry->text_length != entry->name_length + 1U ||
                   text[entry->name_length] != '\0') {
            return false;
        }
        used += entry->text_length;
    }
    return used == journal->text_used;
}

int gsh_aliases_apply_journal_in_place(
    gsh_alias_store *store, const gsh_alias_journal *journal)
{
    size_t index;

    if (store == NULL || !gsh_alias_journal_validate(journal)) {
        errno = EPROTO;
        return -1;
    }
    for (index = 0; index < journal->count; index++) {
        const gsh_alias_journal_entry *entry = &journal->entries[index];
        const char *text = journal->text + entry->text_offset;

        if (entry->operation == GSH_ALIAS_JOURNAL_CLEAR) {
            gsh_aliases_clear(store);
        } else if (entry->operation == GSH_ALIAS_JOURNAL_SET) {
            if (gsh_aliases_set(store, text, entry->name_length,
                                text + entry->name_length + 1U,
                                entry->text_length - entry->name_length -
                                    2U) == -1) {
                return -1;
            }
        } else if (gsh_aliases_unset(store, text,
                                     entry->name_length) == -1) {
            return -1;
        }
    }
    return 0;
}

int gsh_aliases_apply_journal(gsh_alias_store *store,
                              const gsh_alias_journal *journal,
                              gsh_alias_store *scratch)
{
    if (store == NULL || scratch == NULL) {
        errno = EPROTO;
        return -1;
    }
    memcpy(scratch, store, sizeof(*scratch));
    if (gsh_aliases_apply_journal_in_place(scratch, journal) == -1) {
        return -1;
    }
    memcpy(store, scratch, sizeof(*store));
    return 0;
}
