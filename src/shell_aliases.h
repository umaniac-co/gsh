#ifndef GSH_SHELL_ALIASES_H
#define GSH_SHELL_ALIASES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_ALIAS_CAP = 128,
    GSH_ALIAS_HASH_CAP = 256,
    GSH_ALIAS_TEXT_CAP = 32768,
    GSH_ALIAS_VALUE_CAP = 16384,
    GSH_ALIAS_JOURNAL_CAP = 64,
    GSH_ALIAS_JOURNAL_TEXT_CAP = 16384,
    GSH_ALIAS_JOURNAL_VERSION = 1,
};

typedef struct {
    uint32_t assignment_offset;
    uint32_t assignment_length;
    uint32_t hash;
    uint16_t name_length;
    uint16_t reserved;
} gsh_alias_entry;

typedef struct {
    gsh_alias_entry entries[GSH_ALIAS_CAP];
    uint16_t hash_slots[GSH_ALIAS_HASH_CAP];
    uint32_t count;
    uint32_t text_used;
    char text[GSH_ALIAS_TEXT_CAP];
} gsh_alias_store;

enum {
    GSH_ALIAS_JOURNAL_SET = 0,
    GSH_ALIAS_JOURNAL_UNSET,
    GSH_ALIAS_JOURNAL_CLEAR,
};

typedef struct {
    uint32_t text_offset;
    uint32_t text_length;
    uint16_t name_length;
    uint8_t operation;
    uint8_t reserved;
} gsh_alias_journal_entry;

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t text_used;
    uint32_t reserved;
    uint64_t base_generation;
    gsh_alias_journal_entry entries[GSH_ALIAS_JOURNAL_CAP];
    char text[GSH_ALIAS_JOURNAL_TEXT_CAP];
} gsh_alias_journal;

bool gsh_alias_name_is_valid(const char *name, size_t length);
void gsh_aliases_initialize(gsh_alias_store *store);
const char *gsh_aliases_lookup(const gsh_alias_store *store,
                               const char *name, size_t name_length,
                               size_t *index);
int gsh_aliases_set(gsh_alias_store *store, const char *name,
                    size_t name_length, const char *value,
                    size_t value_length);
int gsh_aliases_unset(gsh_alias_store *store, const char *name,
                      size_t name_length);
void gsh_aliases_clear(gsh_alias_store *store);
size_t gsh_aliases_count(const gsh_alias_store *store);
const char *gsh_aliases_assignment(const gsh_alias_store *store,
                                   size_t index);

void gsh_alias_journal_initialize(gsh_alias_journal *journal,
                                  uint64_t base_generation);
int gsh_alias_journal_record_set(gsh_alias_journal *journal,
                                 const char *name, size_t name_length,
                                 const char *value, size_t value_length);
int gsh_alias_journal_record_unset(gsh_alias_journal *journal,
                                   const char *name, size_t name_length);
int gsh_alias_journal_record_clear(gsh_alias_journal *journal);
bool gsh_alias_journal_validate(const gsh_alias_journal *journal);
int gsh_aliases_apply_journal_in_place(
    gsh_alias_store *store, const gsh_alias_journal *journal);
int gsh_aliases_apply_journal(gsh_alias_store *store,
                              const gsh_alias_journal *journal,
                              gsh_alias_store *scratch);

#endif
