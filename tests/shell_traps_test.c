#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh trap tests require the POSIX.1-2024 baseline"
#endif

#include "../src/shell_traps.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

static bool require_at(bool condition, int line)
{
    if (!condition) {
        (void)fprintf(stderr, "shell traps: check failed at line %d\n", line);
        return false;
    }
    return true;
}

#define require(condition) require_at((condition), __LINE__)

static size_t condition_named(const char *name)
{
    size_t condition = GSH_TRAP_CONDITION_CAP;

    if (!require(gsh_traps_parse_condition(name, &condition))) {
        return GSH_TRAP_CONDITION_CAP;
    }
    if (!require(condition < gsh_traps_condition_count())) {
        return GSH_TRAP_CONDITION_CAP;
    }
    if (!require(strcmp(gsh_traps_condition_name(condition), name) == 0)) {
        return GSH_TRAP_CONDITION_CAP;
    }
    return condition;
}

static int assert_action(const gsh_trap_store *store, size_t condition,
                         const char *expected)
{
    if (store == NULL) {
        return -1;
    }
    size_t length = 0;
    const char *action = gsh_traps_action(store, condition, &length);

    if (!require(action != NULL)) return 1;
    if (!require(length == strlen(expected))) return 1;
    if (!require(memcmp(action, expected, length) == 0)) return 1;
    return 0;
}

static int test_compact_action_arena(gsh_trap_store *store,
                                     size_t exit_condition,
                                     size_t usr1_condition)
{
    if (!require(gsh_traps_configure(
            store, exit_condition, "alpha", 5U) == 0)) return 1;
    if (!require(gsh_traps_configure(
            store, usr1_condition, "beta", 4U) == 0)) return 1;
    if (!require(store->text_used == 9U)) return 1;
    if (!require(store->action_count == 2U)) return 1;
    if (!require(gsh_traps_configure(
            store, exit_condition, "x", 1U) == 0)) return 1;
    if (!require(store->text_used == 5U)) return 1;
    if (assert_action(store, exit_condition, "x") != 0) return 1;
    return assert_action(store, usr1_condition, "beta");
}

static int test_pending_signal(gsh_trap_store *store,
                               size_t usr1_condition)
{
    size_t pending_condition = GSH_TRAP_CONDITION_CAP;

    if (!require(raise(SIGUSR1) == 0)) return 1;
    if (!require(gsh_traps_have_pending(store))) return 1;
    if (!require(gsh_traps_pending_signal(store) == SIGUSR1)) return 1;
    if (!require(gsh_traps_take_pending(store, &pending_condition))) return 1;
    if (!require(pending_condition == usr1_condition)) return 1;
    if (!require(!gsh_traps_have_pending(store))) return 1;
    return 0;
}

static int test_subshell_snapshot(gsh_trap_store *store,
                                  size_t exit_condition,
                                  size_t usr1_condition)
{
    size_t length = 0;
    const char *action;

    if (!require(gsh_traps_enter_subshell(store) == 0)) return 1;
    if (!require(gsh_traps_state(
            store, exit_condition) == GSH_TRAP_DEFAULT)) return 1;
    if (!require(gsh_traps_state(
            store, usr1_condition) == GSH_TRAP_DEFAULT)) return 1;
    action = gsh_traps_query_action(store, exit_condition, &length);
    if (!require(action != NULL && length == 1U && action[0] == 'x')) {
        return 1;
    }
    if (!require(gsh_traps_configure(
            store, exit_condition, NULL, 0U) == 0)) return 1;
    if (!require(gsh_traps_query_state(
            store, exit_condition) == GSH_TRAP_DEFAULT)) return 1;
    return 0;
}

static int test_inherited_ignore(size_t usr2_condition)
{
    gsh_trap_store store;
    struct sigaction ignore;

    (void)memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    (void)sigemptyset(&ignore.sa_mask);
    if (!require(sigaction(SIGUSR2, &ignore, NULL) == 0)) return 1;
    if (!require(gsh_traps_initialize(&store) == 0)) return 1;
    if (!require(gsh_traps_state(
            &store, usr2_condition) == GSH_TRAP_INHERITED_IGNORE)) return 1;
    if (!require(gsh_traps_configure(
            &store, usr2_condition, "bad", 3U) == 0)) return 1;
    if (!require(gsh_traps_state(
            &store, usr2_condition) == GSH_TRAP_INHERITED_IGNORE)) return 1;
    return 0;
}

int main(void)
{
    gsh_trap_store store;
    size_t exit_condition = condition_named("EXIT");
    size_t usr1_condition = condition_named("USR1");
    size_t usr2_condition = condition_named("USR2");

    if (!require(gsh_traps_initialize(&store) == 0)) return 1;
    if (!require(exit_condition == 0U)) return 1;
    if (!require(gsh_traps_condition_signal(exit_condition) == 0)) return 1;
    if (test_compact_action_arena(
            &store, exit_condition, usr1_condition) != 0 ||
        test_pending_signal(&store, usr1_condition) != 0 ||
        test_subshell_snapshot(
            &store, exit_condition, usr1_condition) != 0 ||
        test_inherited_ignore(usr2_condition) != 0) return 1;
    (void)puts("shell traps: initialization, compaction, pending delivery, "
         "subshell reset, and inherited ignore passed");
    return 0;
}
