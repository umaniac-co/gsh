#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh trap tests require the POSIX.1-2024 baseline"
#endif

#include "../src/shell_traps.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static size_t condition_named(const char *name)
{
    size_t condition = GSH_TRAP_CONDITION_CAP;

    assert(gsh_traps_parse_condition(name, &condition));
    assert(condition < gsh_traps_condition_count());
    assert(strcmp(gsh_traps_condition_name(condition), name) == 0);
    return condition;
}

static void assert_action(const gsh_trap_store *store, size_t condition,
                          const char *expected)
{
    size_t length = 0;
    const char *action = gsh_traps_action(store, condition, &length);

    assert(action != NULL);
    assert(length == strlen(expected));
    assert(memcmp(action, expected, length) == 0);
}

static void test_compact_action_arena(gsh_trap_store *store,
                                      size_t exit_condition,
                                      size_t usr1_condition)
{
    assert(gsh_traps_configure(store, exit_condition, "alpha", 5U) == 0);
    assert(gsh_traps_configure(store, usr1_condition, "beta", 4U) == 0);
    assert(store->text_used == 9U);
    assert(store->action_count == 2U);
    assert(gsh_traps_configure(store, exit_condition, "x", 1U) == 0);
    assert(store->text_used == 5U);
    assert_action(store, exit_condition, "x");
    assert_action(store, usr1_condition, "beta");
}

static void test_pending_signal(gsh_trap_store *store,
                                size_t usr1_condition)
{
    size_t pending_condition = GSH_TRAP_CONDITION_CAP;

    assert(raise(SIGUSR1) == 0);
    assert(gsh_traps_have_pending(store));
    assert(gsh_traps_pending_signal(store) == SIGUSR1);
    assert(gsh_traps_take_pending(store, &pending_condition));
    assert(pending_condition == usr1_condition);
    assert(!gsh_traps_have_pending(store));
}

static void test_subshell_snapshot(gsh_trap_store *store,
                                   size_t exit_condition,
                                   size_t usr1_condition)
{
    size_t length = 0;
    const char *action;

    assert(gsh_traps_enter_subshell(store) == 0);
    assert(gsh_traps_state(store, exit_condition) == GSH_TRAP_DEFAULT);
    assert(gsh_traps_state(store, usr1_condition) == GSH_TRAP_DEFAULT);
    action = gsh_traps_query_action(store, exit_condition, &length);
    assert(action != NULL && length == 1U && action[0] == 'x');
    assert(gsh_traps_configure(store, exit_condition, NULL, 0U) == 0);
    assert(gsh_traps_query_state(store, exit_condition) ==
           GSH_TRAP_DEFAULT);
}

static void test_inherited_ignore(size_t usr2_condition)
{
    gsh_trap_store store;
    struct sigaction ignore;

    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    assert(sigaction(SIGUSR2, &ignore, NULL) == 0);
    assert(gsh_traps_initialize(&store) == 0);
    assert(gsh_traps_state(&store, usr2_condition) ==
           GSH_TRAP_INHERITED_IGNORE);
    assert(gsh_traps_configure(&store, usr2_condition, "bad", 3U) == 0);
    assert(gsh_traps_state(&store, usr2_condition) ==
           GSH_TRAP_INHERITED_IGNORE);
}

int main(void)
{
    gsh_trap_store store;
    size_t exit_condition = condition_named("EXIT");
    size_t usr1_condition = condition_named("USR1");
    size_t usr2_condition = condition_named("USR2");

    assert(gsh_traps_initialize(&store) == 0);
    assert(exit_condition == 0U);
    assert(gsh_traps_condition_signal(exit_condition) == 0);
    test_compact_action_arena(&store, exit_condition, usr1_condition);
    test_pending_signal(&store, usr1_condition);
    test_subshell_snapshot(&store, exit_condition, usr1_condition);
    test_inherited_ignore(usr2_condition);
    puts("shell traps: initialization, compaction, pending delivery, "
         "subshell reset, and inherited ignore passed");
    return 0;
}
