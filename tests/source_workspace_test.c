#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "../src/source_workspace.h"

#include <stdio.h>

static int exercise_workspace_stack(void)
{
    static gsh_source_workspace_stack stack_storage;
    gsh_source_workspace_stack *stack = &stack_storage;
    gsh_source_workspace *slots[GSH_SOURCE_DEPTH_CAP];
    size_t acquired = 0;
    size_t index;
    int failed = 0;

    gsh_source_workspaces_initialize(stack);
    if (gsh_source_workspaces_depth(stack) != 0U ||
        gsh_aliases_count(&stack->root_aliases) != 0U ||
        gsh_functions_count(&stack->root_functions) != 0U) {
        failed = 1;
    }
    for (index = 0; index < GSH_SOURCE_DEPTH_CAP; index++) {
        slots[index] = gsh_source_workspace_acquire(stack);
        if (slots[index] == NULL ||
            gsh_source_workspaces_depth(stack) != index + 1U) {
            failed = 1;
            break;
        }
        acquired++;
    }
    if (acquired == GSH_SOURCE_DEPTH_CAP &&
        gsh_source_workspace_acquire(stack) != NULL) {
        failed = 1;
    }
    if (acquired > 1U &&
        gsh_source_workspace_release(stack, slots[0])) {
        failed = 1;
    }
    for (index = acquired; index > 0U; index--) {
        if (!gsh_source_workspace_release(stack, slots[index - 1U])) {
            failed = 1;
            break;
        }
    }
    if (acquired == 0U || gsh_source_workspaces_depth(stack) != 0U ||
        gsh_source_workspace_release(stack, slots[0])) {
        failed = 1;
    }
    return failed;
}

int main(void)
{
    if (exercise_workspace_stack() != 0) {
        (void)fputs("source workspace tests failed\n", stderr);
        return 1;
    }
    (void)puts("source workspace tests passed");
    return 0;
}
