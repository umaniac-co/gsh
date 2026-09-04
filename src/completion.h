#ifndef GSH_COMPLETION_H
#define GSH_COMPLETION_H

#include "shell_aliases.h"
#include "shell_functions.h"
#include "shell_variables.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_COMPLETION_VERSION = 3,
    GSH_COMPLETION_TEXT_CAP = 4096,
};

#define GSH_COMPLETION_SELECT_MENU UINT32_MAX

enum {
    GSH_COMPLETION_EDIT = 1,
    GSH_COMPLETION_MENU,
    GSH_COMPLETION_CYCLE,
    GSH_COMPLETION_LIMIT,
};

typedef struct {
    uint32_t version;
    uint32_t status;
    uint64_t request_id;
    uint32_t begin;
    uint32_t end;
    uint32_t text_length;
    uint32_t menu_length;
    uint32_t candidate_count;
    uint32_t selected_index;
    uint32_t reserved;
    char text[GSH_COMPLETION_TEXT_CAP];
    char menu[GSH_COMPLETION_TEXT_CAP];
} gsh_completion_result;

int gsh_completion_generate(
    const char *line, size_t line_length, size_t cursor,
    const char *path, const char *home,
    const gsh_alias_store *aliases,
    const gsh_function_store *functions,
    const gsh_variable_store *variables,
    size_t terminal_columns, uint32_t selection_index,
    uint64_t request_id, gsh_completion_result *result);
bool gsh_completion_result_valid(const gsh_completion_result *result);

#endif
