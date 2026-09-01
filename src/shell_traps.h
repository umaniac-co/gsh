#ifndef GSH_SHELL_TRAPS_H
#define GSH_SHELL_TRAPS_H

#include <stdbool.h>
#include <stddef.h>

enum {
    GSH_TRAP_CONDITION_CAP = 32,
    GSH_TRAP_OPERAND_CAP = 128,
    GSH_TRAP_TEXT_CAP = 1024 * 1024,
};

typedef enum {
    GSH_TRAP_DEFAULT = 0,
    GSH_TRAP_IGNORE,
    GSH_TRAP_ACTION,
    GSH_TRAP_INHERITED_IGNORE,
} gsh_trap_state;

typedef struct {
    size_t offset;
    size_t length;
    gsh_trap_state state;
} gsh_trap_entry;

typedef struct {
    gsh_trap_entry entries[GSH_TRAP_CONDITION_CAP];
    gsh_trap_entry snapshot_entries[GSH_TRAP_CONDITION_CAP];
    size_t text_used;
    size_t snapshot_text_used;
    size_t nondefault_count;
    size_t action_count;
    bool snapshot_valid;
    char text[GSH_TRAP_TEXT_CAP + 1U];
} gsh_trap_store;

int gsh_traps_initialize(gsh_trap_store *store);
size_t gsh_traps_condition_count(void);
const char *gsh_traps_condition_name(size_t condition);
int gsh_traps_condition_signal(size_t condition);
bool gsh_traps_parse_condition(const char *text, size_t *condition);
gsh_trap_state gsh_traps_state(const gsh_trap_store *store,
                               size_t condition);
const char *gsh_traps_action(const gsh_trap_store *store,
                             size_t condition, size_t *length);
gsh_trap_state gsh_traps_query_state(const gsh_trap_store *store,
                                     size_t condition);
const char *gsh_traps_query_action(const gsh_trap_store *store,
                                   size_t condition, size_t *length);
int gsh_traps_configure(gsh_trap_store *store, size_t condition,
                        const char *action, size_t action_length);
int gsh_traps_enter_subshell(gsh_trap_store *store);
bool gsh_traps_have_pending(const gsh_trap_store *store);
int gsh_traps_pending_signal(const gsh_trap_store *store);
bool gsh_traps_take_pending(gsh_trap_store *store, size_t *condition);

#endif
