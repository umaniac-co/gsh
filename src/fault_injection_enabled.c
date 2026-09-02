#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "fault_injection.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define require(condition) (condition)

typedef struct {
    gsh_fault_point selected;
    unsigned long trigger;
    unsigned long calls;
} fault_configuration;

static fault_configuration *fault_storage(void)
{
    static fault_configuration configuration = {GSH_FAULT_NONE, 1U, 0U};

    if (!require(configuration.selected < GSH_FAULT_COUNT)) return NULL;
    if (!require(configuration.trigger > 0U)) return NULL;
    return &configuration;
}

static const char *const FAULT_NAMES[GSH_FAULT_COUNT] = {
    [GSH_FAULT_NONE] = "",
    [GSH_FAULT_ALIAS_ALLOCATION] = "alias-allocation",
    [GSH_FAULT_ALIAS_COMMIT_MALFORMED] = "alias-commit-malformed",
    [GSH_FAULT_ALIAS_TRANSACTION_ALLOCATION] = "alias-transaction-allocation",
    [GSH_FAULT_ALLOCATION] = "allocation",
    [GSH_FAULT_ASYNC_FORK] = "async-fork",
    [GSH_FAULT_COMMAND_CACHE_COMMIT_MALFORMED] = "command-cache-commit-malformed",
    [GSH_FAULT_DESCRIPTOR_DUP] = "descriptor-dup",
    [GSH_FAULT_DESCRIPTOR_SAVE] = "descriptor-save",
    [GSH_FAULT_DIRECTORY_COMMIT_APPLY] = "directory-commit-apply",
    [GSH_FAULT_DIRECTORY_COMMIT_OPEN] = "directory-commit-open",
    [GSH_FAULT_DIRECTORY_COMMIT_RECEIVE] = "directory-commit-receive",
    [GSH_FAULT_DIRECTORY_COMMIT_SEND] = "directory-commit-send",
    [GSH_FAULT_DIRECTORY_COMMIT_SOCKET] = "directory-commit-socket",
    [GSH_FAULT_ENOEXEC_INTERPRETER_OPEN] = "enoexec-interpreter-open",
    [GSH_FAULT_EVALUATOR_FORK] = "evaluator-fork",
    [GSH_FAULT_EVALUATOR_GATE] = "evaluator-gate",
    [GSH_FAULT_EXEC] = "exec",
    [GSH_FAULT_EXEC_DESCRIPTOR_APPLY] = "exec-descriptor-apply",
    [GSH_FAULT_EXEC_DESCRIPTOR_RECEIVE] = "exec-descriptor-receive",
    [GSH_FAULT_EXEC_DESCRIPTOR_SEND] = "exec-descriptor-send",
    [GSH_FAULT_EXEC_DESCRIPTOR_SOCKET] = "exec-descriptor-socket",
    [GSH_FAULT_EXEC_DESCRIPTOR_STABILIZE] = "exec-descriptor-stabilize",
    [GSH_FAULT_EXEC_OUTCOME_PIPE] = "exec-outcome-pipe",
    [GSH_FAULT_EXEC_OWNER_DESCRIPTOR_RELOCATION] = "exec-owner-descriptor-relocation",
    [GSH_FAULT_EXPANSION_ASSIGNMENT] = "expansion-assignment",
    [GSH_FAULT_FOR_ALLOCATION] = "for-allocation",
    [GSH_FAULT_FUNCTION_ALLOCATION] = "function-allocation",
    [GSH_FAULT_FUNCTION_COMMIT_MALFORMED] = "function-commit-malformed",
    [GSH_FAULT_FUNCTION_COMMIT_WRITE] = "function-commit-write",
    [GSH_FAULT_FUNCTION_COMPACT_ALLOCATION] = "function-compact-allocation",
    [GSH_FAULT_HEREDOC_FORK] = "heredoc-fork",
    [GSH_FAULT_HEREDOC_PIPE] = "heredoc-pipe",
    [GSH_FAULT_HEREDOC_WRITE] = "heredoc-write",
    [GSH_FAULT_HISTORY_ALLOCATION] = "history-allocation",
    [GSH_FAULT_INPUT_ALIAS_MAP] = "input-alias-map",
    [GSH_FAULT_INPUT_MAP] = "input-map",
    [GSH_FAULT_INPUT_MODE] = "input-mode",
    [GSH_FAULT_INPUT_READ] = "input-read",
    [GSH_FAULT_INPUT_SPILL_CLOEXEC] = "input-spill-cloexec",
    [GSH_FAULT_INPUT_SPILL_OPEN] = "input-spill-open",
    [GSH_FAULT_INPUT_SPILL_RESIZE] = "input-spill-resize",
    [GSH_FAULT_INPUT_SPILL_UNLINK] = "input-spill-unlink",
    [GSH_FAULT_INPUT_SPILL_WRITE] = "input-spill-write",
    [GSH_FAULT_JOB_FORK] = "job-fork",
    [GSH_FAULT_JOB_PIPE] = "job-pipe",
    [GSH_FAULT_JOB_SERVICE_SOCKET] = "job-service-socket",
    [GSH_FAULT_JOB_TABLE_ALLOCATION] = "job-table-allocation",
    [GSH_FAULT_OPTION_COMMIT_MALFORMED] = "option-commit-malformed",
    [GSH_FAULT_OUTPUT_WRITE] = "output-write",
    [GSH_FAULT_PIPELINE_FORK] = "pipeline-fork",
    [GSH_FAULT_PIPELINE_PIPE] = "pipeline-pipe",
    [GSH_FAULT_POLL] = "poll",
    [GSH_FAULT_POSITIONAL_ALLOCATION] = "positional-allocation",
    [GSH_FAULT_POSITIONAL_COMMIT_ALLOCATION] = "positional-commit-allocation",
    [GSH_FAULT_POSITIONAL_COMMIT_MALFORMED] = "positional-commit-malformed",
    [GSH_FAULT_REDIRECT_OPEN] = "redirect-open",
    [GSH_FAULT_RESOURCE_ACTION_SOCKET] = "resource-action-socket",
    [GSH_FAULT_SHELL_EXECUTABLE_RESOLUTION] = "shell-executable-resolution",
    [GSH_FAULT_SIGNAL_PIPE] = "signal-pipe",
    [GSH_FAULT_SOURCE_WORKSPACE_EXHAUSTION] = "source-workspace-exhaustion",
    [GSH_FAULT_STATE_COMMIT_MALFORMED] = "state-commit-malformed",
    [GSH_FAULT_STATE_COMMIT_PIPE] = "state-commit-pipe",
    [GSH_FAULT_STATE_COMMIT_READ] = "state-commit-read",
    [GSH_FAULT_STATE_COMMIT_WRITE] = "state-commit-write",
    [GSH_FAULT_STATE_CONTROL_COMMIT_MALFORMED] = "state-control-commit-malformed",
    [GSH_FAULT_SUBSHELL_FORK] = "subshell-fork",
    [GSH_FAULT_SUBSTITUTION_FORK] = "substitution-fork",
    [GSH_FAULT_SUBSTITUTION_PIPE] = "substitution-pipe",
    [GSH_FAULT_SUBSTITUTION_READ] = "substitution-read",
    [GSH_FAULT_TERMINAL_HANDOFF] = "terminal-handoff",
    [GSH_FAULT_TIME_SOURCE_FAILURE] = "time-source-failure",
    [GSH_FAULT_TRANSACTION_DESCRIPTOR_RELOCATION] = "transaction-descriptor-relocation",
    [GSH_FAULT_TRAP_WORKSPACE_EXHAUSTION] = "trap-workspace-exhaustion",
    [GSH_FAULT_TTY_OPEN] = "tty-open",
    [GSH_FAULT_WORKER_FORK] = "worker-fork",
    [GSH_FAULT_WORKER_SOCKET] = "worker-socket",
};

_Static_assert(sizeof(FAULT_NAMES) / sizeof(FAULT_NAMES[0]) ==
                   GSH_FAULT_COUNT,
               "every fault point must have one bounded name slot");

static gsh_fault_point fault_point_named(const char *name, size_t length)
{
    size_t point;

    if (!require(name != NULL)) return GSH_FAULT_NONE;
    if (!require(length > 0U && length < 64U)) return GSH_FAULT_NONE;
    for (point = 1U; point < GSH_FAULT_COUNT; point++) {
        if (strlen(FAULT_NAMES[point]) == length &&
            memcmp(FAULT_NAMES[point], name, length) == 0) {
            return (gsh_fault_point)point;
        }
    }
    return GSH_FAULT_NONE;
}

void gsh_fault_initialize(void)
{
    fault_configuration *configuration = fault_storage();
    const char *setting = getenv("GSH_FAULT");
    const char *separator;
    size_t length;

    if (!require(configuration != NULL)) return;
    if (setting == NULL || setting[0] == '\0') return;
    separator = strchr(setting, ':');
    length = separator == NULL ? strlen(setting)
                               : (size_t)(separator - setting);
    configuration->selected = fault_point_named(setting, length);
    if (configuration->selected == GSH_FAULT_NONE) return;
    if (separator != NULL && separator[1] != '\0') {
        char *end;
        unsigned long trigger = strtoul(separator + 1, &end, 10);

        if (*end == '\0' && trigger > 0U) configuration->trigger = trigger;
    }
}

bool gsh_fault_should_fail(gsh_fault_point point, int error)
{
    fault_configuration *configuration = fault_storage();

    if (!require(configuration != NULL)) return false;
    if (!require(point > GSH_FAULT_NONE && point < GSH_FAULT_COUNT &&
                 error != 0)) return false;
    if (configuration->selected != point) return false;
    configuration->calls++;
    if (configuration->calls != configuration->trigger) return false;
    errno = error;
    return true;
}

bool gsh_fault_active(void)
{
    const fault_configuration *configuration = fault_storage();

    if (!require(configuration != NULL)) return false;
    return configuration->selected != GSH_FAULT_NONE;
}

bool gsh_fault_selected(gsh_fault_point point)
{
    const fault_configuration *configuration = fault_storage();

    if (!require(configuration != NULL)) return false;
    if (!require(point > GSH_FAULT_NONE && point < GSH_FAULT_COUNT)) {
        return false;
    }
    return configuration->selected == point;
}

unsigned long gsh_fault_trigger(void)
{
    const fault_configuration *configuration = fault_storage();

    if (!require(configuration != NULL)) return 0U;
    if (!require(configuration->trigger > 0U)) return 0U;
    return configuration->trigger;
}

unsigned long *gsh_fault_counter(void)
{
    fault_configuration *configuration = fault_storage();

    if (!require(configuration != NULL)) return NULL;
    return &configuration->calls;
}
