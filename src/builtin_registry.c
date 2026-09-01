#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_registry.h"

#include <string.h>

/* ── One Table Makes Native Ownership Auditable ─────────────────────
 * Builtin identity used to be repeated in command inspection and dispatch.
 * A new builtin could therefore execute natively but still resolve externally.
 * This fixed table records ownership, state effects, and implementation status.
 * Callers use the enum and an explicit switch; no callback hides the call graph.
 * A bounded linear scan is faster than process delegation at this small scale.
 * ────────────────────────────────────────────── */
#define BUILTIN(name_, kind_, class_, special_)                            \
    {name_, sizeof(name_) - 1U, kind_, class_, special_, true}

static const gsh_builtin_descriptor builtin_table[] = {
    BUILTIN(".", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN(":", GSH_BUILTIN_COLON, GSH_BUILTIN_PURE, true),
    BUILTIN("[", GSH_BUILTIN_BRACKET, GSH_BUILTIN_PURE, false),
    BUILTIN("alias", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, false),
    BUILTIN("bg", GSH_BUILTIN_OTHER, GSH_BUILTIN_JOB_CONTROL, false),
    BUILTIN("break", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("cd", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, false),
    BUILTIN("command", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, false),
    BUILTIN("continue", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("echo", GSH_BUILTIN_ECHO, GSH_BUILTIN_PURE, false),
    BUILTIN("eval", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("exec", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("exit", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("export", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("false", GSH_BUILTIN_FALSE, GSH_BUILTIN_PURE, false),
    BUILTIN("fc", GSH_BUILTIN_FC, GSH_BUILTIN_BLOCKING, false),
    BUILTIN("fg", GSH_BUILTIN_OTHER, GSH_BUILTIN_JOB_CONTROL, false),
    BUILTIN("getopts", GSH_BUILTIN_GETOPTS, GSH_BUILTIN_STATEFUL, false),
    BUILTIN("hash", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, false),
    BUILTIN("help", GSH_BUILTIN_OTHER, GSH_BUILTIN_PURE, false),
    BUILTIN("jobs", GSH_BUILTIN_JOBS, GSH_BUILTIN_JOB_CONTROL, false),
    BUILTIN("kill", GSH_BUILTIN_KILL, GSH_BUILTIN_JOB_CONTROL, false),
    BUILTIN("printf", GSH_BUILTIN_PRINTF, GSH_BUILTIN_PURE, false),
    BUILTIN("pwd", GSH_BUILTIN_OTHER, GSH_BUILTIN_PURE, false),
    BUILTIN("read", GSH_BUILTIN_READ, GSH_BUILTIN_BLOCKING, false),
    BUILTIN("readonly", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("return", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("rt", GSH_BUILTIN_OTHER, GSH_BUILTIN_PURE, false),
    BUILTIN("set", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("shift", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("test", GSH_BUILTIN_TEST, GSH_BUILTIN_PURE, false),
    BUILTIN("times", GSH_BUILTIN_OTHER, GSH_BUILTIN_PURE, true),
    BUILTIN("trap", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("true", GSH_BUILTIN_TRUE, GSH_BUILTIN_PURE, false),
    BUILTIN("type", GSH_BUILTIN_OTHER, GSH_BUILTIN_PURE, false),
    BUILTIN("ulimit", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, false),
    BUILTIN("umask", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, false),
    BUILTIN("unalias", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, false),
    BUILTIN("unset", GSH_BUILTIN_OTHER, GSH_BUILTIN_STATEFUL, true),
    BUILTIN("wait", GSH_BUILTIN_OTHER, GSH_BUILTIN_JOB_CONTROL, false),
};

#undef BUILTIN

const gsh_builtin_descriptor *gsh_builtin_lookup(const char *name,
                                                 size_t length)
{
    size_t left = 0;
    size_t right = sizeof(builtin_table) / sizeof(builtin_table[0]);

    if (name == NULL || length == 0) {
        return NULL;
    }
    while (left < right) {
        size_t middle = left + (right - left) / 2U;
        const gsh_builtin_descriptor *candidate = &builtin_table[middle];
        size_t shared = length < candidate->name_length
                            ? length : candidate->name_length;
        int order = memcmp(name, candidate->name, shared);

        if (order == 0) {
            if (length == candidate->name_length) return candidate;
            order = length < candidate->name_length ? -1 : 1;
        }
        if (order < 0) right = middle;
        else left = middle + 1U;
    }
    return NULL;
}

size_t gsh_builtin_descriptor_count(void)
{
    return sizeof(builtin_table) / sizeof(builtin_table[0]);
}

const gsh_builtin_descriptor *gsh_builtin_descriptor_at(size_t index)
{
    return index < gsh_builtin_descriptor_count() ? &builtin_table[index]
                                                   : NULL;
}

bool gsh_builtin_regular_name(const char *name, size_t length)
{
    const gsh_builtin_descriptor *descriptor =
        gsh_builtin_lookup(name, length);

    return descriptor != NULL && descriptor->implemented &&
           !descriptor->special;
}

bool gsh_builtin_pure_name(const char *name, size_t length)
{
    const gsh_builtin_descriptor *descriptor =
        gsh_builtin_lookup(name, length);

    return descriptor != NULL && descriptor->implemented &&
           descriptor->execution_class == GSH_BUILTIN_PURE &&
           descriptor->kind != GSH_BUILTIN_OTHER;
}
