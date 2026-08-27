#ifndef GSH_SHELL_VARIABLES_H
#define GSH_SHELL_VARIABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_VARIABLE_CAP = 1024,
    GSH_VARIABLE_HASH_CAP = 2048,
    GSH_VARIABLE_TEXT_CAP = 262144,
    GSH_VARIABLE_VALUE_CAP = 65535,
    GSH_VARIABLE_ENVIRONMENT_CAP = GSH_VARIABLE_CAP + 65,
    GSH_VARIABLE_JOURNAL_CAP = 64,
    GSH_VARIABLE_JOURNAL_TEXT_CAP = 65536,
    GSH_VARIABLE_JOURNAL_VERSION = 3,
};

typedef enum {
    GSH_VARIABLE_JOURNAL_SET = 0,
    GSH_VARIABLE_JOURNAL_ATTRIBUTES,
    GSH_VARIABLE_JOURNAL_UNSET,
    GSH_VARIABLE_JOURNAL_UNSET_ATTRIBUTES,
} gsh_variable_journal_operation;

typedef enum {
    GSH_VARIABLE_JOURNAL_VALUE_ABSENT = 0,
    GSH_VARIABLE_JOURNAL_VALUE_SET,
    GSH_VARIABLE_JOURNAL_VALUE_UNSET,
} gsh_variable_journal_value_state;

enum {
    GSH_VARIABLE_EXPORTED = 1U << 0,
    GSH_VARIABLE_READONLY = 1U << 1,
    GSH_VARIABLE_ATTRIBUTE_MASK =
        GSH_VARIABLE_EXPORTED | GSH_VARIABLE_READONLY,
};

typedef struct {
    uint32_t assignment_offset;
    uint32_t assignment_length;
    uint32_t hash;
    uint16_t name_length;
    uint16_t attributes;
} gsh_variable_entry;

typedef struct {
    gsh_variable_entry entries[GSH_VARIABLE_CAP];
    uint16_t hash_slots[GSH_VARIABLE_HASH_CAP];
    uint32_t count;
    uint32_t text_used;
    char text[GSH_VARIABLE_TEXT_CAP];
} gsh_variable_store;

typedef struct {
    uint32_t assignment_offset;
    uint32_t assignment_length;
    uint16_t name_length;
    uint16_t attribute_mask;
    uint16_t attributes;
    uint16_t scope;
    uint8_t operation;
    uint8_t reserved[3];
} gsh_variable_journal_entry;

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t text_used;
    uint32_t reserved;
    uint64_t base_generation;
    gsh_variable_journal_entry entries[GSH_VARIABLE_JOURNAL_CAP];
    char text[GSH_VARIABLE_JOURNAL_TEXT_CAP];
} gsh_variable_journal;

bool gsh_variable_name_is_valid(const char *name, size_t length);
void gsh_variables_initialize(gsh_variable_store *store);
int gsh_variables_import(gsh_variable_store *store,
                         char *const environment[]);
const char *gsh_variables_lookup(const gsh_variable_store *store,
                                 const char *name, size_t name_length,
                                 bool *found);
bool gsh_variables_get_state(const gsh_variable_store *store,
                             const char *name, size_t name_length,
                             bool *is_set, unsigned int *attributes);
int gsh_variables_set(gsh_variable_store *store, const char *name,
                      size_t name_length, const char *value,
                      size_t value_length, unsigned int attribute_mask,
                      unsigned int attributes);
int gsh_variables_set_attributes(gsh_variable_store *store,
                                 const char *name, size_t name_length,
                                 unsigned int attribute_mask,
                                 unsigned int attributes);
int gsh_variables_unset(gsh_variable_store *store, const char *name,
                        size_t name_length);
size_t gsh_variables_count(const gsh_variable_store *store);
const char *gsh_variables_assignment(const gsh_variable_store *store,
                                     size_t index,
                                     unsigned int *attributes);
bool gsh_variables_is_set(const gsh_variable_store *store, size_t index);

void gsh_variable_journal_initialize(gsh_variable_journal *journal,
                                     uint64_t base_generation);
int gsh_variable_journal_record(gsh_variable_journal *journal,
                                const char *name, size_t name_length,
                                const char *value, size_t value_length,
                                unsigned int attribute_mask,
                                unsigned int attributes);
int gsh_variable_journal_record_scoped(
    gsh_variable_journal *journal, unsigned int scope, const char *name,
    size_t name_length, const char *value, size_t value_length,
    unsigned int attribute_mask, unsigned int attributes);
int gsh_variable_journal_record_attributes(
    gsh_variable_journal *journal, const char *name, size_t name_length,
    unsigned int attribute_mask, unsigned int attributes);
int gsh_variable_journal_record_unset(gsh_variable_journal *journal,
                                      const char *name,
                                      size_t name_length);
const char *gsh_variable_journal_lookup(
    const gsh_variable_journal *journal, const char *name,
    size_t name_length, gsh_variable_journal_value_state *state);
const char *gsh_variable_journal_lookup_scoped(
    const gsh_variable_journal *journal, unsigned int scope,
    const char *name, size_t name_length,
    gsh_variable_journal_value_state *state);
bool gsh_variable_journal_validate(const gsh_variable_journal *journal);
int gsh_variables_can_apply_journal(
    const gsh_variable_store *store,
    const gsh_variable_journal *journal);
int gsh_variables_can_apply_journal_scope(
    const gsh_variable_store *store, const gsh_variable_journal *journal,
    unsigned int scope);
int gsh_variables_apply_journal_in_place(
    gsh_variable_store *store, const gsh_variable_journal *journal);
int gsh_variables_apply_journal_scope_in_place(
    gsh_variable_store *store, const gsh_variable_journal *journal,
    unsigned int scope);
int gsh_variables_apply_journal(gsh_variable_store *store,
                                const gsh_variable_journal *journal,
                                gsh_variable_store *scratch);

#endif
