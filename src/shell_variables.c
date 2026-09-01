#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "shell_variables.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

enum {
    GSH_VARIABLE_IMPORT_SCAN_CAP = 4096,
    GSH_VARIABLE_INTERNAL_UNSET = 1U << 15,
};

static bool name_start(unsigned char byte)
{
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') || byte == '_';
}

static bool name_byte(unsigned char byte)
{
    return name_start(byte) || (byte >= '0' && byte <= '9');
}

bool gsh_variable_name_is_valid(const char *name, size_t length)
{
    size_t offset;

    if (name == NULL || length == 0 || !name_start((unsigned char)name[0])) {
        return false;
    }
    for (offset = 1; offset < length; offset++) {
        if (!name_byte((unsigned char)name[offset])) {
            return false;
        }
    }
    return true;
}

static uint32_t variable_hash(const char *name, size_t length)
{
    uint32_t hash = UINT32_C(2166136261);
    size_t offset;

    for (offset = 0; offset < length; offset++) {
        hash ^= (unsigned char)name[offset];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

void gsh_variables_initialize(gsh_variable_store *store)
{
    memset(store, 0, sizeof(*store));
}

static bool entry_name_matches(const gsh_variable_store *store,
                               const gsh_variable_entry *entry,
                               const char *name, size_t name_length,
                               uint32_t hash)
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

static size_t variable_index(const gsh_variable_store *store,
                             const char *name, size_t name_length,
                             uint32_t hash)
{
    size_t slot = hash & (GSH_VARIABLE_HASH_CAP - 1U);
    size_t probes;

    for (probes = 0; probes < GSH_VARIABLE_HASH_CAP; probes++) {
        uint16_t encoded = store->hash_slots[slot];

        if (encoded == 0) {
            return GSH_VARIABLE_CAP;
        }
        if ((size_t)(encoded - 1U) < store->count &&
            entry_name_matches(store, &store->entries[encoded - 1U], name,
                               name_length, hash)) {
            return (size_t)(encoded - 1U);
        }
        slot = (slot + 1U) & (GSH_VARIABLE_HASH_CAP - 1U);
    }
    return GSH_VARIABLE_CAP;
}

static int insert_hash_slot(gsh_variable_store *store, size_t index)
{
    size_t slot = store->entries[index].hash &
                  (GSH_VARIABLE_HASH_CAP - 1U);
    size_t probes;

    for (probes = 0; probes < GSH_VARIABLE_HASH_CAP; probes++) {
        if (store->hash_slots[slot] == 0) {
            store->hash_slots[slot] = (uint16_t)(index + 1U);
            return 0;
        }
        slot = (slot + 1U) & (GSH_VARIABLE_HASH_CAP - 1U);
    }
    errno = ENOSPC;
    return -1;
}

const char *gsh_variables_lookup(const gsh_variable_store *store,
                                 const char *name, size_t name_length,
                                 bool *found)
{
    uint32_t hash;
    size_t index;

    *found = false;
    if (!gsh_variable_name_is_valid(name, name_length)) {
        return "";
    }
    hash = variable_hash(name, name_length);
    index = variable_index(store, name, name_length, hash);
    if (index == GSH_VARIABLE_CAP ||
        (store->entries[index].attributes &
         GSH_VARIABLE_INTERNAL_UNSET) != 0) {
        return "";
    }
    *found = true;
    return store->text + store->entries[index].assignment_offset +
           name_length + 1U;
}

bool gsh_variables_get_state(const gsh_variable_store *store,
                             const char *name, size_t name_length,
                             bool *is_set, unsigned int *attributes)
{
    size_t index;

    *is_set = false;
    *attributes = 0;
    if (!gsh_variable_name_is_valid(name, name_length)) {
        return false;
    }
    index = variable_index(store, name, name_length,
                           variable_hash(name, name_length));
    if (index == GSH_VARIABLE_CAP) {
        return false;
    }
    *is_set = (store->entries[index].attributes &
               GSH_VARIABLE_INTERNAL_UNSET) == 0;
    *attributes = store->entries[index].attributes &
                  GSH_VARIABLE_ATTRIBUTE_MASK;
    return true;
}

static int write_assignment(char *destination, const char *name,
                            size_t name_length, const char *value,
                            size_t value_length)
{
    memcpy(destination, name, name_length);
    destination[name_length] = '=';
    memcpy(destination + name_length + 1U, value, value_length);
    destination[name_length + 1U + value_length] = '\0';
    return 0;
}

static int successful_update(gsh_variable_store *store, const char *name,
                             size_t name_length, bool set_value)
{
    if (set_value && name_length == 4U && memcmp(name, "PATH", 4) == 0) {
        store->path_generation++;
    }
    return 0;
}

static int set_variable(gsh_variable_store *store, const char *name,
                        size_t name_length, const char *value,
                        size_t value_length, unsigned int attribute_mask,
                        unsigned int attributes, bool set_value)
{
    uint32_t hash;
    size_t index;
    size_t new_length;

    if (!gsh_variable_name_is_valid(name, name_length) || value == NULL ||
        name_length > UINT16_MAX || value_length > GSH_VARIABLE_VALUE_CAP ||
        attribute_mask > GSH_VARIABLE_ATTRIBUTE_MASK ||
        attributes > GSH_VARIABLE_ATTRIBUTE_MASK ||
        (attributes & ~attribute_mask) != 0) {
        errno = EINVAL;
        return -1;
    }
    if (name_length > SIZE_MAX - value_length - 2U) {
        errno = E2BIG;
        return -1;
    }
    new_length = name_length + value_length + 2U;
    if (new_length > GSH_VARIABLE_TEXT_CAP) {
        errno = E2BIG;
        return -1;
    }
    hash = variable_hash(name, name_length);
    index = variable_index(store, name, name_length, hash);
    if (index != GSH_VARIABLE_CAP) {
        gsh_variable_entry *entry = &store->entries[index];
        size_t old_offset = entry->assignment_offset;
        size_t old_length = entry->assignment_length;
        size_t tail_offset = old_offset + old_length;
        size_t tail_length = store->text_used - tail_offset;
        size_t other;

        if (((entry->attributes & GSH_VARIABLE_READONLY) != 0 &&
             set_value) ||
            ((entry->attributes & GSH_VARIABLE_READONLY) != 0 &&
             (attribute_mask & GSH_VARIABLE_READONLY) != 0 &&
             (attributes & GSH_VARIABLE_READONLY) == 0)) {
            errno = EROFS;
            return -1;
        }
        if (!set_value) {
            entry->attributes = (uint16_t)(
                (entry->attributes & ~attribute_mask) |
                (attributes & attribute_mask));
            return 0;
        }
        if ((entry->attributes & GSH_VARIABLE_INTERNAL_UNSET) == 0 &&
            old_length == new_length &&
            memcmp(store->text + old_offset + name_length + 1U, value,
                   value_length) == 0) {
            entry->attributes = (uint16_t)(
                (entry->attributes & ~attribute_mask) |
                (attributes & attribute_mask));
            return successful_update(store, name, name_length, set_value);
        }
        if (new_length > old_length &&
            new_length - old_length >
                GSH_VARIABLE_TEXT_CAP - store->text_used) {
            errno = ENOSPC;
            return -1;
        }
        memmove(store->text + old_offset + new_length,
                store->text + tail_offset, tail_length);
        if (new_length != old_length) {
            ptrdiff_t delta = (ptrdiff_t)new_length -
                              (ptrdiff_t)old_length;

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
        write_assignment(store->text + old_offset, name, name_length, value,
                         value_length);
        entry->assignment_length = (uint32_t)new_length;
        entry->attributes = (uint16_t)(
            (entry->attributes & ~attribute_mask) |
            (attributes & attribute_mask));
        entry->attributes &= (uint16_t)~GSH_VARIABLE_INTERNAL_UNSET;
        return successful_update(store, name, name_length, set_value);
    }
    if (store->count == GSH_VARIABLE_CAP) {
        errno = ENOSPC;
        return -1;
    }
    if (new_length > GSH_VARIABLE_TEXT_CAP - store->text_used) {
        errno = ENOSPC;
        return -1;
    }
    index = store->count;
    store->entries[index].assignment_offset = store->text_used;
    store->entries[index].assignment_length = (uint32_t)new_length;
    store->entries[index].hash = hash;
    store->entries[index].name_length = (uint16_t)name_length;
    store->entries[index].attributes =
        (uint16_t)((attributes & attribute_mask) |
                   (set_value ? 0U : GSH_VARIABLE_INTERNAL_UNSET));
    write_assignment(store->text + store->text_used, name, name_length,
                     value, value_length);
    store->text_used += (uint32_t)new_length;
    store->count++;
    if (insert_hash_slot(store, index) == -1) {
        store->count--;
        store->text_used -= (uint32_t)new_length;
        return -1;
    }
    return successful_update(store, name, name_length, set_value);
}

int gsh_variables_set(gsh_variable_store *store, const char *name,
                      size_t name_length, const char *value,
                      size_t value_length, unsigned int attribute_mask,
                      unsigned int attributes)
{
    return set_variable(store, name, name_length, value, value_length,
                        attribute_mask, attributes, true);
}

int gsh_variables_set_attributes(gsh_variable_store *store,
                                 const char *name, size_t name_length,
                                 unsigned int attribute_mask,
                                 unsigned int attributes)
{
    return set_variable(store, name, name_length, "", 0, attribute_mask,
                        attributes, false);
}

static void rebuild_hash(gsh_variable_store *store)
{
    size_t index;

    memset(store->hash_slots, 0, sizeof(store->hash_slots));
    for (index = 0; index < store->count; index++) {
        (void)insert_hash_slot(store, index);
    }
}

int gsh_variables_unset(gsh_variable_store *store, const char *name,
                        size_t name_length)
{
    uint32_t hash;
    size_t index;
    size_t text_offset;
    size_t text_length;
    size_t tail_length;
    size_t other;

    if (!gsh_variable_name_is_valid(name, name_length)) {
        errno = EINVAL;
        return -1;
    }
    hash = variable_hash(name, name_length);
    index = variable_index(store, name, name_length, hash);
    if (index == GSH_VARIABLE_CAP) {
        return successful_update(store, name, name_length, true);
    }
    if ((store->entries[index].attributes & GSH_VARIABLE_READONLY) != 0) {
        errno = EROFS;
        return -1;
    }
    text_offset = store->entries[index].assignment_offset;
    text_length = store->entries[index].assignment_length;
    tail_length = store->text_used - text_offset - text_length;
    memmove(store->text + text_offset,
            store->text + text_offset + text_length, tail_length);
    memmove(store->entries + index, store->entries + index + 1U,
            (store->count - index - 1U) * sizeof(store->entries[0]));
    store->count--;
    store->text_used -= (uint32_t)text_length;
    for (other = 0; other < store->count; other++) {
        if (store->entries[other].assignment_offset > text_offset) {
            store->entries[other].assignment_offset -=
                (uint32_t)text_length;
        }
    }
    rebuild_hash(store);
    return successful_update(store, name, name_length, true);
}

int gsh_variables_import(gsh_variable_store *store,
                         char *const environment[])
{
    size_t index;
    bool saw_end = false;

    gsh_variables_initialize(store);
    for (index = 0; index < GSH_VARIABLE_IMPORT_SCAN_CAP; index++) {
        const char *assignment = environment[index];
        const char *separator;
        size_t name_length;
        size_t value_length;

        if (assignment == NULL) {
            saw_end = true;
            break;
        }
        separator = strchr(assignment, '=');
        if (separator == NULL) {
            continue;
        }
        name_length = (size_t)(separator - assignment);
        if (!gsh_variable_name_is_valid(assignment, name_length)) {
            continue;
        }
        value_length = strlen(separator + 1U);
        if (gsh_variables_set(store, assignment, name_length,
                              separator + 1U, value_length,
                              GSH_VARIABLE_EXPORTED,
                              GSH_VARIABLE_EXPORTED) == -1) {
            return -1;
        }
    }
    if (!saw_end) {
        errno = E2BIG;
        return -1;
    }
    if (gsh_variables_set(store, "IFS", 3, " \t\n", 3, 0, 0) == -1) {
        return -1;
    }
    return 0;
}

size_t gsh_variables_count(const gsh_variable_store *store)
{
    return store->count;
}

const char *gsh_variables_assignment(const gsh_variable_store *store,
                                     size_t index,
                                     unsigned int *attributes)
{
    if (index >= store->count) {
        return NULL;
    }
    if (attributes != NULL) {
        *attributes = store->entries[index].attributes &
                      GSH_VARIABLE_ATTRIBUTE_MASK;
    }
    return store->text + store->entries[index].assignment_offset;
}

bool gsh_variables_is_set(const gsh_variable_store *store, size_t index)
{
    return index < store->count &&
           (store->entries[index].attributes &
            GSH_VARIABLE_INTERNAL_UNSET) == 0;
}

uint64_t gsh_variables_path_generation(const gsh_variable_store *store)
{
    return store == NULL ? 0U : store->path_generation;
}

void gsh_variable_journal_initialize(gsh_variable_journal *journal,
                                     uint64_t base_generation)
{
    memset(journal, 0, sizeof(*journal));
    journal->version = GSH_VARIABLE_JOURNAL_VERSION;
    journal->base_generation = base_generation;
}

static size_t journal_name_index(const gsh_variable_journal *journal,
                                 unsigned int scope, const char *name,
                                 size_t name_length)
{
    size_t index;

    for (index = 0; index < journal->count; index++) {
        const gsh_variable_journal_entry *entry = &journal->entries[index];
        const char *assignment = journal->text + entry->assignment_offset;

        if (entry->scope == scope && entry->name_length == name_length &&
            memcmp(assignment, name, name_length) == 0 &&
            assignment[name_length] == '=') {
            return index;
        }
    }
    return GSH_VARIABLE_JOURNAL_CAP;
}

static int journal_record(
    gsh_variable_journal *journal, unsigned int scope, const char *name,
    size_t name_length, const char *value, size_t value_length,
    unsigned int attribute_mask, unsigned int attributes,
    gsh_variable_journal_operation operation);

const char *gsh_variable_journal_lookup(
    const gsh_variable_journal *journal, const char *name,
    size_t name_length, gsh_variable_journal_value_state *state)
{
    return gsh_variable_journal_lookup_scoped(journal, 0, name,
                                              name_length, state);
}

const char *gsh_variable_journal_lookup_scoped(
    const gsh_variable_journal *journal, unsigned int scope,
    const char *name, size_t name_length,
    gsh_variable_journal_value_state *state)
{
    size_t index;

    if (scope > UINT16_MAX) {
        *state = GSH_VARIABLE_JOURNAL_VALUE_ABSENT;
        return "";
    }
    index = journal_name_index(journal, scope, name, name_length);
    if (index == GSH_VARIABLE_JOURNAL_CAP ||
        journal->entries[index].operation ==
            GSH_VARIABLE_JOURNAL_ATTRIBUTES) {
        *state = GSH_VARIABLE_JOURNAL_VALUE_ABSENT;
        return "";
    }
    if (journal->entries[index].operation != GSH_VARIABLE_JOURNAL_SET) {
        *state = GSH_VARIABLE_JOURNAL_VALUE_UNSET;
        return "";
    }
    *state = GSH_VARIABLE_JOURNAL_VALUE_SET;
    return journal->text + journal->entries[index].assignment_offset +
           name_length + 1U;
}

int gsh_variable_journal_record(gsh_variable_journal *journal,
                                const char *name, size_t name_length,
                                const char *value, size_t value_length,
                                unsigned int attribute_mask,
                                unsigned int attributes)
{
    return gsh_variable_journal_record_scoped(
        journal, 0, name, name_length, value, value_length,
        attribute_mask, attributes);
}

int gsh_variable_journal_record_scoped(
    gsh_variable_journal *journal, unsigned int scope, const char *name,
    size_t name_length, const char *value, size_t value_length,
    unsigned int attribute_mask, unsigned int attributes)
{
    return journal_record(journal, scope, name, name_length, value,
                          value_length, attribute_mask, attributes,
                          GSH_VARIABLE_JOURNAL_SET);
}

int gsh_variable_journal_record_attributes(
    gsh_variable_journal *journal, const char *name, size_t name_length,
    unsigned int attribute_mask, unsigned int attributes)
{
    return journal_record(journal, 0, name, name_length, "", 0,
                          attribute_mask, attributes,
                          GSH_VARIABLE_JOURNAL_ATTRIBUTES);
}

int gsh_variable_journal_record_unset(gsh_variable_journal *journal,
                                      const char *name,
                                      size_t name_length)
{
    return journal_record(journal, 0, name, name_length, "", 0, 0, 0,
                          GSH_VARIABLE_JOURNAL_UNSET);
}

static int journal_record(
    gsh_variable_journal *journal, unsigned int scope, const char *name,
    size_t name_length, const char *value, size_t value_length,
    unsigned int attribute_mask, unsigned int attributes,
    gsh_variable_journal_operation operation)
{
    size_t index;
    size_t new_length;

    if (journal->version != GSH_VARIABLE_JOURNAL_VERSION ||
        !gsh_variable_name_is_valid(name, name_length) || value == NULL ||
        scope > UINT16_MAX || name_length > UINT16_MAX ||
        value_length > GSH_VARIABLE_VALUE_CAP ||
        attribute_mask > GSH_VARIABLE_ATTRIBUTE_MASK ||
        attributes > GSH_VARIABLE_ATTRIBUTE_MASK ||
        (attributes & ~attribute_mask) != 0 ||
        operation > GSH_VARIABLE_JOURNAL_UNSET ||
        (operation == GSH_VARIABLE_JOURNAL_UNSET &&
         (value_length != 0 || attribute_mask != 0)) ||
        (operation == GSH_VARIABLE_JOURNAL_ATTRIBUTES && value_length != 0) ||
        name_length > SIZE_MAX - value_length - 2U) {
        errno = EINVAL;
        return -1;
    }
    new_length = name_length + value_length + 2U;
    if (new_length > GSH_VARIABLE_JOURNAL_TEXT_CAP) {
        errno = E2BIG;
        return -1;
    }
    index = journal_name_index(journal, scope, name, name_length);
    if (index != GSH_VARIABLE_JOURNAL_CAP) {
        gsh_variable_journal_entry *entry = &journal->entries[index];
        size_t old_offset = entry->assignment_offset;
        size_t old_length = entry->assignment_length;
        size_t tail_offset = old_offset + old_length;
        size_t tail_length = journal->text_used - tail_offset;
        size_t other;

        if ((entry->attributes & GSH_VARIABLE_READONLY) != 0 &&
            (operation != GSH_VARIABLE_JOURNAL_ATTRIBUTES ||
             ((attribute_mask & GSH_VARIABLE_READONLY) != 0 &&
              (attributes & GSH_VARIABLE_READONLY) == 0))) {
            errno = EROFS;
            return -1;
        }
        if (operation == GSH_VARIABLE_JOURNAL_ATTRIBUTES) {
            if (entry->operation == GSH_VARIABLE_JOURNAL_UNSET) {
                entry->operation = GSH_VARIABLE_JOURNAL_UNSET_ATTRIBUTES;
            }
            entry->attribute_mask |= (uint16_t)attribute_mask;
            entry->attributes = (uint16_t)(
                (entry->attributes & ~attribute_mask) |
                (attributes & attribute_mask));
            return 0;
        }
        if (new_length > old_length &&
            new_length - old_length >
                GSH_VARIABLE_JOURNAL_TEXT_CAP - journal->text_used) {
            errno = ENOSPC;
            return -1;
        }
        memmove(journal->text + old_offset + new_length,
                journal->text + tail_offset, tail_length);
        if (new_length != old_length) {
            ptrdiff_t delta = (ptrdiff_t)new_length -
                              (ptrdiff_t)old_length;

            for (other = 0; other < journal->count; other++) {
                if (other != index &&
                    journal->entries[other].assignment_offset > old_offset) {
                    journal->entries[other].assignment_offset =
                        (uint32_t)((ptrdiff_t)journal->entries[other]
                                       .assignment_offset +
                                   delta);
                }
            }
            journal->text_used =
                (uint32_t)((ptrdiff_t)journal->text_used + delta);
        }
        write_assignment(journal->text + old_offset, name, name_length,
                         value, value_length);
        entry->assignment_length = (uint32_t)new_length;
        entry->operation = (uint8_t)operation;
        if (operation == GSH_VARIABLE_JOURNAL_UNSET) {
            entry->attribute_mask = 0;
            entry->attributes = 0;
        } else {
            entry->attribute_mask |= (uint16_t)attribute_mask;
            entry->attributes = (uint16_t)(
                (entry->attributes & ~attribute_mask) |
                (attributes & attribute_mask));
        }
        return 0;
    }
    if (journal->count == GSH_VARIABLE_JOURNAL_CAP) {
        errno = ENOSPC;
        return -1;
    }
    if (new_length > GSH_VARIABLE_JOURNAL_TEXT_CAP - journal->text_used) {
        errno = ENOSPC;
        return -1;
    }
    index = journal->count;
    journal->entries[index].assignment_offset = journal->text_used;
    journal->entries[index].assignment_length = (uint32_t)new_length;
    journal->entries[index].name_length = (uint16_t)name_length;
    journal->entries[index].attribute_mask = (uint16_t)attribute_mask;
    journal->entries[index].attributes =
        (uint16_t)(attributes & attribute_mask);
    journal->entries[index].scope = (uint16_t)scope;
    journal->entries[index].operation = (uint8_t)operation;
    memset(journal->entries[index].reserved, 0,
           sizeof(journal->entries[index].reserved));
    write_assignment(journal->text + journal->text_used, name, name_length,
                     value, value_length);
    journal->text_used += (uint32_t)new_length;
    journal->count++;
    return 0;
}

bool gsh_variable_journal_validate(const gsh_variable_journal *journal)
{
    size_t index;

    if (journal->version != GSH_VARIABLE_JOURNAL_VERSION ||
        journal->reserved != 0 ||
        journal->count > GSH_VARIABLE_JOURNAL_CAP ||
        journal->text_used > GSH_VARIABLE_JOURNAL_TEXT_CAP) {
        return false;
    }
    for (index = 0; index < journal->count; index++) {
        const gsh_variable_journal_entry *entry = &journal->entries[index];
        const char *assignment;
        size_t value_offset;
        bool value_operation;

        value_operation = entry->operation == GSH_VARIABLE_JOURNAL_SET;
        if (entry->name_length == 0 ||
            entry->assignment_length < (size_t)entry->name_length + 2U ||
            entry->assignment_offset > journal->text_used ||
            entry->assignment_length >
                journal->text_used - entry->assignment_offset ||
            entry->attribute_mask > GSH_VARIABLE_ATTRIBUTE_MASK ||
            entry->attributes > GSH_VARIABLE_ATTRIBUTE_MASK ||
            (entry->attributes & ~entry->attribute_mask) != 0 ||
            entry->operation > GSH_VARIABLE_JOURNAL_UNSET_ATTRIBUTES ||
            entry->reserved[0] != 0 || entry->reserved[1] != 0 ||
            entry->reserved[2] != 0 ||
            (entry->operation == GSH_VARIABLE_JOURNAL_UNSET &&
             (entry->attribute_mask != 0 || entry->attributes != 0)) ||
            (!value_operation &&
             entry->assignment_length !=
                 (size_t)entry->name_length + 2U)) {
            return false;
        }
        assignment = journal->text + entry->assignment_offset;
        value_offset = (size_t)entry->name_length + 1U;
        if (!gsh_variable_name_is_valid(assignment, entry->name_length) ||
            assignment[entry->name_length] != '=' ||
            assignment[entry->assignment_length - 1U] != '\0' ||
            memchr(assignment + value_offset, '\0',
                   entry->assignment_length - value_offset - 1U) != NULL) {
            return false;
        }
    }
    return true;
}

static int can_apply_journal_scope(const gsh_variable_store *store,
                                   const gsh_variable_journal *journal,
                                   unsigned int scope,
                                   bool require_only_scope)
{
    size_t final_count = store->count;
    size_t final_text = store->text_used;
    size_t index;

    if (scope > UINT16_MAX || !gsh_variable_journal_validate(journal)) {
        errno = EPROTO;
        return -1;
    }
    for (index = 0; index < journal->count; index++) {
        const gsh_variable_journal_entry *change =
            &journal->entries[index];
        const char *assignment =
            journal->text + change->assignment_offset;
        uint32_t hash;
        size_t current;

        if (change->scope != scope) {
            if (require_only_scope) {
                errno = EPROTO;
                return -1;
            }
            continue;
        }
        hash = variable_hash(assignment, change->name_length);
        current = variable_index(store, assignment, change->name_length,
                                 hash);

        if (current == GSH_VARIABLE_CAP) {
            if (change->operation != GSH_VARIABLE_JOURNAL_UNSET) {
                final_count++;
                final_text += change->assignment_length;
            }
        } else {
            const gsh_variable_entry *entry = &store->entries[current];
            bool clears_readonly =
                change->operation == GSH_VARIABLE_JOURNAL_ATTRIBUTES &&
                (change->attribute_mask & GSH_VARIABLE_READONLY) != 0 &&
                (change->attributes & GSH_VARIABLE_READONLY) == 0;

            if ((entry->attributes & GSH_VARIABLE_READONLY) != 0 &&
                (change->operation != GSH_VARIABLE_JOURNAL_ATTRIBUTES ||
                 clears_readonly)) {
                errno = EROFS;
                return -1;
            }
            if (change->operation == GSH_VARIABLE_JOURNAL_UNSET) {
                final_count--;
                final_text -= entry->assignment_length;
            } else if (change->operation !=
                       GSH_VARIABLE_JOURNAL_ATTRIBUTES) {
                final_text -= entry->assignment_length;
                final_text += change->assignment_length;
            }
        }
        if (final_count > GSH_VARIABLE_CAP ||
            final_text > GSH_VARIABLE_TEXT_CAP) {
            errno = ENOSPC;
            return -1;
        }
    }
    return 0;
}

int gsh_variables_can_apply_journal(
    const gsh_variable_store *store,
    const gsh_variable_journal *journal)
{
    return can_apply_journal_scope(store, journal, 0, true);
}

int gsh_variables_can_apply_journal_scope(
    const gsh_variable_store *store, const gsh_variable_journal *journal,
    unsigned int scope)
{
    return can_apply_journal_scope(store, journal, scope, false);
}

static int apply_journal_entry(
    gsh_variable_store *store, const gsh_variable_journal_entry *entry,
    const char *assignment)
{
    const char *value = assignment + entry->name_length + 1U;
    size_t value_length = entry->assignment_length - entry->name_length - 2U;

    switch ((gsh_variable_journal_operation)entry->operation) {
    case GSH_VARIABLE_JOURNAL_SET:
        return gsh_variables_set(store, assignment, entry->name_length,
                                 value, value_length,
                                 entry->attribute_mask, entry->attributes);
    case GSH_VARIABLE_JOURNAL_ATTRIBUTES:
        return gsh_variables_set_attributes(
            store, assignment, entry->name_length, entry->attribute_mask,
            entry->attributes);
    case GSH_VARIABLE_JOURNAL_UNSET:
        return gsh_variables_unset(store, assignment, entry->name_length);
    case GSH_VARIABLE_JOURNAL_UNSET_ATTRIBUTES:
        if (gsh_variables_unset(store, assignment, entry->name_length) == -1) {
            return -1;
        }
        return gsh_variables_set_attributes(
            store, assignment, entry->name_length, entry->attribute_mask,
            entry->attributes);
    }
    errno = EPROTO;
    return -1;
}

int gsh_variables_apply_journal_in_place(
    gsh_variable_store *store, const gsh_variable_journal *journal)
{
    size_t index;

    if (gsh_variables_can_apply_journal(store, journal) == -1) {
        return -1;
    }
    for (index = 0; index < journal->count; index++) {
        const gsh_variable_journal_entry *entry = &journal->entries[index];
        const char *assignment =
            journal->text + entry->assignment_offset;
        if (apply_journal_entry(store, entry, assignment) == -1) {
            return -1;
        }
    }
    return 0;
}

int gsh_variables_apply_journal_scope_in_place(
    gsh_variable_store *store, const gsh_variable_journal *journal,
    unsigned int scope)
{
    size_t index;

    if (can_apply_journal_scope(store, journal, scope, false) == -1) {
        return -1;
    }
    for (index = 0; index < journal->count; index++) {
        const gsh_variable_journal_entry *entry = &journal->entries[index];
        const char *assignment;

        if (entry->scope != scope) {
            continue;
        }
        assignment = journal->text + entry->assignment_offset;
        if (apply_journal_entry(store, entry, assignment) == -1) {
            return -1;
        }
    }
    return 0;
}

int gsh_variables_apply_journal(gsh_variable_store *store,
                                const gsh_variable_journal *journal,
                                gsh_variable_store *scratch)
{
    if (scratch == NULL || !gsh_variable_journal_validate(journal)) {
        errno = EPROTO;
        return -1;
    }
    memcpy(scratch, store, sizeof(*scratch));
    if (gsh_variables_apply_journal_in_place(scratch, journal) == -1) {
        return -1;
    }
    memcpy(store, scratch, sizeof(*store));
    return 0;
}
