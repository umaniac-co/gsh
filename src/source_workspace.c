#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "source_workspace.h"

#include <assert.h>

/* ── One Bounded Arena Owns Nested Source State ────────────────
 * Command substitution once allocated up to nine large objects per entry,
 * making latency and failure depend on allocator state after initialization.
 * Eval, dot, trap actions, and executable fallback need the same parse state.
 * One initialization-time arena now owns persistent alias/function stores and
 * gives each active source a complete slot. Strict LIFO ownership makes
 * nesting deterministic and stale reuse detectable. The fixed depth bounds
 * address space while untouched pages stay uncommitted on supported platforms.
 * ─────────────────────────────────────────────── */
void gsh_source_workspaces_initialize(gsh_source_workspace_stack *stack)
{
    if (stack == NULL) {
        return;
    }
    stack->version = GSH_SOURCE_STACK_VERSION;
    stack->depth = 0;
    gsh_aliases_initialize(&stack->root_aliases);
    gsh_aliases_initialize(&stack->root_alias_scratch);
    gsh_functions_initialize(&stack->root_functions);
    gsh_functions_initialize(&stack->root_function_scratch);
}

gsh_source_workspace *gsh_source_workspace_acquire(
    gsh_source_workspace_stack *stack)
{
    gsh_source_workspace *workspace;

    if (stack == NULL || stack->version != GSH_SOURCE_STACK_VERSION ||
        stack->depth >= GSH_SOURCE_DEPTH_CAP) {
        return NULL;
    }
    workspace = &stack->workspaces[stack->depth];
    stack->depth++;
    assert(stack->depth > 0U);
    assert(workspace == &stack->workspaces[stack->depth - 1U]);
    return workspace;
}

bool gsh_source_workspace_release(gsh_source_workspace_stack *stack,
                                  gsh_source_workspace *workspace)
{
    if (stack == NULL || workspace == NULL ||
        stack->version != GSH_SOURCE_STACK_VERSION || stack->depth == 0U ||
        workspace != &stack->workspaces[stack->depth - 1U]) {
        return false;
    }
    assert(stack->depth <= GSH_SOURCE_DEPTH_CAP);
    assert(workspace == &stack->workspaces[stack->depth - 1U]);
    stack->depth--;
    return true;
}

size_t gsh_source_workspaces_depth(
    const gsh_source_workspace_stack *stack)
{
    if (stack == NULL || stack->version != GSH_SOURCE_STACK_VERSION ||
        stack->depth > GSH_SOURCE_DEPTH_CAP) {
        return GSH_SOURCE_DEPTH_CAP + 1U;
    }
    assert(stack->depth <= GSH_SOURCE_DEPTH_CAP);
    assert(stack->version == GSH_SOURCE_STACK_VERSION);
    return stack->depth;
}
