#ifndef GSH_SOURCE_WORKSPACE_H
#define GSH_SOURCE_WORKSPACE_H

#include "alias_expansion.h"
#include "native_plan.h"
#include "shell_functions.h"
#include "shell_variables.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_SOURCE_DEPTH_CAP = 8,
    GSH_SOURCE_INPUT_CAP = 1024 * 1024,
    GSH_SOURCE_STACK_VERSION = 1,
};

typedef struct {
    gsh_parse_storage storage;
    gsh_native_pipeline pipeline;
    gsh_variable_store variables;
    gsh_variable_store scope_base;
    gsh_variable_journal scope_changes;
    gsh_function_store functions;
    gsh_function_store function_scratch;
    char alias_expansion[GSH_ALIAS_EXPANSION_CAP];
    char input[GSH_SOURCE_INPUT_CAP + 1U];
} gsh_source_workspace;

typedef struct {
    uint32_t version;
    uint32_t depth;
    gsh_alias_store root_aliases;
    gsh_alias_store root_alias_scratch;
    gsh_alias_journal root_alias_commit;
    gsh_function_store root_functions;
    gsh_function_store root_function_scratch;
    char root_alias_expansion[GSH_ALIAS_EXPANSION_CAP];
    gsh_source_workspace workspaces[GSH_SOURCE_DEPTH_CAP];
} gsh_source_workspace_stack;

void gsh_source_workspaces_initialize(gsh_source_workspace_stack *stack);
gsh_source_workspace *gsh_source_workspace_acquire(
    gsh_source_workspace_stack *stack);
bool gsh_source_workspace_release(gsh_source_workspace_stack *stack,
                                  gsh_source_workspace *workspace);
size_t gsh_source_workspaces_depth(
    const gsh_source_workspace_stack *stack);

#endif
