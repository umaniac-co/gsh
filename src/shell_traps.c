#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "shell_traps.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h> /* CANON-INCLUDE: linux */
#include <string.h>

typedef struct {
    const char *name;
    int signal_number;
} trap_condition;

/* ── The Platform Defines the Portable Signal Vocabulary ────────
 * POSIX names conditions through the symbolic constants exposed by
 * <signal.h>, while several members are optional on conforming targets.
 * A fixed table keeps lookup and handler work bounded; guarded entries are
 * the narrow portability mechanism that lets one source cover macOS and
 * Linux without inventing signal numbers or accepting unsupported names.
 * ─────────────────────────────────────────────────────────────── */
static const trap_condition CONDITIONS[] = {
    {"EXIT", 0},
#ifdef SIGABRT
    {"ABRT", SIGABRT},
#endif
#ifdef SIGALRM
    {"ALRM", SIGALRM},
#endif
#ifdef SIGBUS
    {"BUS", SIGBUS},
#endif
#ifdef SIGCHLD
    {"CHLD", SIGCHLD},
#endif
#ifdef SIGCONT
    {"CONT", SIGCONT},
#endif
#ifdef SIGFPE
    {"FPE", SIGFPE},
#endif
#ifdef SIGHUP
    {"HUP", SIGHUP},
#endif
#ifdef SIGILL
    {"ILL", SIGILL},
#endif
#ifdef SIGINT
    {"INT", SIGINT},
#endif
#ifdef SIGKILL
    {"KILL", SIGKILL},
#endif
#ifdef SIGPIPE
    {"PIPE", SIGPIPE},
#endif
#ifdef SIGPOLL
    {"POLL", SIGPOLL},
#endif
#ifdef SIGPROF
    {"PROF", SIGPROF},
#endif
#ifdef SIGQUIT
    {"QUIT", SIGQUIT},
#endif
#ifdef SIGSEGV
    {"SEGV", SIGSEGV},
#endif
#ifdef SIGSTOP
    {"STOP", SIGSTOP},
#endif
#ifdef SIGSYS
    {"SYS", SIGSYS},
#endif
#ifdef SIGTERM
    {"TERM", SIGTERM},
#endif
#ifdef SIGTRAP
    {"TRAP", SIGTRAP},
#endif
#ifdef SIGTSTP
    {"TSTP", SIGTSTP},
#endif
#ifdef SIGTTIN
    {"TTIN", SIGTTIN},
#endif
#ifdef SIGTTOU
    {"TTOU", SIGTTOU},
#endif
#ifdef SIGURG
    {"URG", SIGURG},
#endif
#ifdef SIGUSR1
    {"USR1", SIGUSR1},
#endif
#ifdef SIGUSR2
    {"USR2", SIGUSR2},
#endif
#ifdef SIGVTALRM
    {"VTALRM", SIGVTALRM},
#endif
#ifdef SIGXCPU
    {"XCPU", SIGXCPU},
#endif
#ifdef SIGXFSZ
    {"XFSZ", SIGXFSZ},
#endif
};

_Static_assert(sizeof(CONDITIONS) / sizeof(CONDITIONS[0]) <=
                   GSH_TRAP_CONDITION_CAP,
               "trap condition storage must cover POSIX signals");

static volatile sig_atomic_t pending[GSH_TRAP_CONDITION_CAP];
static volatile sig_atomic_t any_pending;

/* ── Signal Handlers Report; the Evaluator Executes ─────────────
 * Evaluating shell language from an asynchronous handler would corrupt the
 * parser and variable transactions. The handler therefore writes one
 * sig_atomic_t slot selected from an immutable condition table and returns.
 * The evaluator drains those coalesced reports only at command boundaries,
 * preserving both async-signal safety and POSIX foreground deferral.
 * ─────────────────────────────────────────────────────────────── */
static void trap_signal_handler(int signal_number)
{
    int saved_errno = errno;
    size_t condition;

    for (condition = 1U;
         condition < sizeof(CONDITIONS) / sizeof(CONDITIONS[0]);
         condition++) {
        if (CONDITIONS[condition].signal_number == signal_number) {
            pending[condition] = 1;
            any_pending = 1;
            break;
        }
    }
    errno = saved_errno;
}

static bool uncatchable_signal(int signal_number)
{
#ifdef SIGKILL
    if (signal_number == SIGKILL) {
        return true;
    }
#endif
#ifdef SIGSTOP
    if (signal_number == SIGSTOP) {
        return true;
    }
#endif
    return false;
}

static int set_signal_state(int signal_number, gsh_trap_state state)
{
    struct sigaction disposition;

    if (signal_number == 0 || uncatchable_signal(signal_number)) {
        return 0;
    }
    (void)memset(&disposition, 0, sizeof(disposition));
    (void)sigemptyset(&disposition.sa_mask);
    disposition.sa_handler = state == GSH_TRAP_ACTION
                                 ? trap_signal_handler
                                 : state == GSH_TRAP_IGNORE ||
                                           state == GSH_TRAP_INHERITED_IGNORE
                                       ? SIG_IGN
                                       : SIG_DFL;
    return sigaction(signal_number, &disposition, NULL);
}

static int block_condition(size_t condition, sigset_t *previous)
{
    if (previous == NULL) {
        return -1;
    }
    sigset_t blocked;
    int signal_number = CONDITIONS[condition].signal_number;

    (void)sigemptyset(&blocked);
    if (signal_number != 0) {
        (void)sigaddset(&blocked, signal_number);
    }
    return sigprocmask(SIG_BLOCK, &blocked, previous);
}

static void remove_action_text(gsh_trap_store *store, size_t condition)
{
    if (store == NULL || condition >= GSH_TRAP_CONDITION_CAP) return;
    gsh_trap_entry *entry = &store->entries[condition];
    size_t following;
    size_t index;

    if (entry->state != GSH_TRAP_ACTION ||
        entry->offset > store->text_used ||
        entry->length > store->text_used - entry->offset) return;
    following = store->text_used - entry->offset - entry->length;
    (void)memmove(store->text + entry->offset,
            store->text + entry->offset + entry->length, following);
    store->text_used -= entry->length;
    for (index = 0; index < gsh_traps_condition_count(); index++) {
        gsh_trap_entry *candidate = &store->entries[index];

        if (candidate->state == GSH_TRAP_ACTION &&
            candidate->offset > entry->offset) {
            candidate->offset -= entry->length;
        }
    }
}

static void commit_trap_entry(gsh_trap_store *store, size_t condition,
                              gsh_trap_state state, const char *action,
                              size_t action_length)
{
    if (store == NULL || condition >= gsh_traps_condition_count() ||
        (state == GSH_TRAP_ACTION && action == NULL)) {
        return;
    }
    gsh_trap_entry *entry = &store->entries[condition];
    bool was_nondefault = entry->state != GSH_TRAP_DEFAULT;
    bool was_action = entry->state == GSH_TRAP_ACTION;

    if (was_action) {
        remove_action_text(store, condition);
    }
    entry->offset = 0;
    entry->length = 0;
    entry->state = state;
    if (state == GSH_TRAP_ACTION) {
        entry->offset = store->text_used;
        entry->length = action_length;
        (void)memcpy(store->text + store->text_used, action, action_length);
        store->text_used += action_length;
        store->text[store->text_used] = '\0';
    }
    store->nondefault_count -= was_nondefault ? 1U : 0U;
    store->nondefault_count += state != GSH_TRAP_DEFAULT ? 1U : 0U;
    store->action_count -= was_action ? 1U : 0U;
    store->action_count += state == GSH_TRAP_ACTION ? 1U : 0U;
}

static void discard_subshell_snapshot(gsh_trap_store *store)
{
    if (store == NULL) return;
    size_t condition;

    if (!store->snapshot_valid) {
        return;
    }
    if (store->action_count != 0U) return;
    store->snapshot_valid = false;
    store->snapshot_text_used = 0;
    store->text_used = 0;
    for (condition = 0; condition < GSH_TRAP_CONDITION_CAP;
         condition++) {
        store->snapshot_entries[condition].offset = 0;
        store->snapshot_entries[condition].length = 0;
        store->snapshot_entries[condition].state = GSH_TRAP_DEFAULT;
    }
}

int gsh_traps_initialize(gsh_trap_store *store)
{
    size_t condition;

    if (store == NULL) {
        errno = EINVAL;
        return -1;
    }
    store->text_used = 0;
    store->snapshot_text_used = 0;
    store->nondefault_count = 0;
    store->action_count = 0;
    store->snapshot_valid = false;
    any_pending = 0;
    for (condition = 0; condition < GSH_TRAP_CONDITION_CAP;
         condition++) {
        store->entries[condition].offset = 0;
        store->entries[condition].length = 0;
        store->entries[condition].state = GSH_TRAP_DEFAULT;
        store->snapshot_entries[condition].offset = 0;
        store->snapshot_entries[condition].length = 0;
        store->snapshot_entries[condition].state = GSH_TRAP_DEFAULT;
        pending[condition] = 0;
    }
    for (condition = 1U; condition < gsh_traps_condition_count();
         condition++) {
        struct sigaction inherited;

        if (uncatchable_signal(CONDITIONS[condition].signal_number)) {
            continue;
        }
        if (sigaction(CONDITIONS[condition].signal_number, NULL,
                      &inherited) == -1) {
            return -1;
        }
        if (inherited.sa_handler == SIG_IGN) {
            store->entries[condition].state = GSH_TRAP_INHERITED_IGNORE;
            store->nondefault_count++;
        }
    }
    return 0;
}

size_t gsh_traps_condition_count(void)
{
    return sizeof(CONDITIONS) / sizeof(CONDITIONS[0]);
}

const char *gsh_traps_condition_name(size_t condition)
{
    return condition < gsh_traps_condition_count()
               ? CONDITIONS[condition].name
               : NULL;
}

int gsh_traps_condition_signal(size_t condition)
{
    return condition < gsh_traps_condition_count()
               ? CONDITIONS[condition].signal_number
               : -1;
}

bool gsh_traps_parse_condition(const char *text, size_t *condition)
{
    size_t index;

    if (text == NULL || condition == NULL || *text == '\0') {
        return false;
    }
    if (strcmp(text, "0") == 0) {
        *condition = 0;
        return true;
    }
    for (index = 0; index < gsh_traps_condition_count(); index++) {
        if (strcmp(text, CONDITIONS[index].name) == 0) {
            *condition = index;
            return true;
        }
    }
    {
        unsigned long value = 0;
        const char *cursor = text;

        while (*cursor >= '0' && *cursor <= '9') {
            unsigned long digit = (unsigned long)(*cursor++ - '0');

            if (value > (ULONG_MAX - digit) / 10UL) {
                return false;
            }
            value = value * 10UL + digit;
        }
        if (*cursor != '\0' || value > (unsigned long)INT_MAX) {
            return false;
        }
        if (value == 0UL) {
            *condition = 0;
            return true;
        }
        for (index = 1U; index < gsh_traps_condition_count(); index++) {
            if ((unsigned long)CONDITIONS[index].signal_number == value) {
                *condition = index;
                return true;
            }
        }
    }
    return false;
}

gsh_trap_state gsh_traps_state(const gsh_trap_store *store,
                               size_t condition)
{
    if (store == NULL) {
        return (gsh_trap_state){0};
    }
    return store != NULL && condition < gsh_traps_condition_count()
               ? store->entries[condition].state
               : GSH_TRAP_DEFAULT;
}

const char *gsh_traps_action(const gsh_trap_store *store,
                             size_t condition, size_t *length)
{
    const gsh_trap_entry *entry;

    if (store == NULL || condition >= gsh_traps_condition_count() ||
        store->entries[condition].state != GSH_TRAP_ACTION) {
        return NULL;
    }
    entry = &store->entries[condition];
    if (entry->offset > store->text_used ||
        entry->length > store->text_used - entry->offset) return NULL;
    if (length != NULL) {
        *length = entry->length;
    }
    return store->text + entry->offset;
}

gsh_trap_state gsh_traps_query_state(const gsh_trap_store *store,
                                     size_t condition)
{
    if (store == NULL || condition >= gsh_traps_condition_count()) {
        return GSH_TRAP_DEFAULT;
    }
    return store->snapshot_valid
               ? store->snapshot_entries[condition].state
               : store->entries[condition].state;
}

const char *gsh_traps_query_action(const gsh_trap_store *store,
                                   size_t condition, size_t *length)
{
    const gsh_trap_entry *entry;
    size_t text_used;

    if (store == NULL || condition >= gsh_traps_condition_count()) {
        return NULL;
    }
    entry = store->snapshot_valid ? &store->snapshot_entries[condition]
                                  : &store->entries[condition];
    text_used = store->snapshot_valid ? store->snapshot_text_used
                                      : store->text_used;
    if (entry->state != GSH_TRAP_ACTION || entry->offset > text_used ||
        entry->length > text_used - entry->offset) {
        return NULL;
    }
    if (length != NULL) {
        *length = entry->length;
    }
    return store->text + entry->offset;
}

int gsh_traps_configure(gsh_trap_store *store, size_t condition,
                        const char *action, size_t action_length)
{
    gsh_trap_state state = action == NULL          ? GSH_TRAP_DEFAULT
                           : action_length == 0U   ? GSH_TRAP_IGNORE
                                                  : GSH_TRAP_ACTION;
    gsh_trap_entry *entry;
    sigset_t previous;
    size_t retained;
    int saved_errno;

    if (store == NULL || condition >= gsh_traps_condition_count() ||
        (action == NULL && action_length != 0U)) {
        errno = EINVAL;
        return -1;
    }
    entry = &store->entries[condition];
    discard_subshell_snapshot(store);
    if (entry->state == GSH_TRAP_INHERITED_IGNORE) {
        return 0;
    }
    retained = store->text_used -
               (entry->state == GSH_TRAP_ACTION ? entry->length : 0U);
    if (action_length > GSH_TRAP_TEXT_CAP - retained) {
        errno = ENOSPC;
        return -1;
    }
    if (action != NULL &&
        (uintptr_t)action >= (uintptr_t)store->text &&
        (uintptr_t)action <=
            (uintptr_t)(store->text + GSH_TRAP_TEXT_CAP)) {
        errno = EINVAL;
        return -1;
    }
    if (block_condition(condition, &previous) == -1) {
        return -1;
    }
    if (set_signal_state(CONDITIONS[condition].signal_number, state) ==
        -1) {
        saved_errno = errno;
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        errno = saved_errno;
        return -1;
    }
    pending[condition] = 0;
    commit_trap_entry(store, condition, state, action, action_length);
    if (sigprocmask(SIG_SETMASK, &previous, NULL) == -1) {
        return -1;
    }
    return 0;
}

/* ── A Pristine Subshell Can Reproduce Its Parent's Traps ───────
 * Subshell execution resets caught signals, yet Issue 8 requires an
 * operandless trap query before any trap mutation to report the entry state.
 * The compact text arena therefore doubles as an immutable query snapshot
 * until the first mutation.  Discarding that snapshot reclaims the whole
 * arena at once; ignored dispositions remain live throughout the transition.
 * ─────────────────────────────────────────────────────────────── */
int gsh_traps_enter_subshell(gsh_trap_store *store)
{
    sigset_t blocked;
    sigset_t previous;
    size_t condition;

    if (store == NULL) {
        return 0;
    }
    discard_subshell_snapshot(store);
    (void)memcpy(store->snapshot_entries, store->entries,
           gsh_traps_condition_count() * sizeof(store->entries[0]));
    store->snapshot_text_used = store->text_used;
    store->snapshot_valid = true;
    (void)sigemptyset(&blocked);
    for (condition = 1U; condition < gsh_traps_condition_count();
         condition++) {
        (void)sigaddset(&blocked, CONDITIONS[condition].signal_number);
    }
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        return -1;
    }
    for (condition = 0; condition < gsh_traps_condition_count();
         condition++) {
        gsh_trap_state state = store->entries[condition].state;

        pending[condition] = 0;
        if (set_signal_state(
                CONDITIONS[condition].signal_number,
                state == GSH_TRAP_ACTION ? GSH_TRAP_DEFAULT : state) ==
            -1) {
            int saved_errno = errno;

            (void)sigprocmask(SIG_SETMASK, &previous, NULL);
            errno = saved_errno;
            return -1;
        }
        if (state == GSH_TRAP_ACTION) {
            store->entries[condition].offset = 0;
            store->entries[condition].length = 0;
            store->entries[condition].state = GSH_TRAP_DEFAULT;
        }
    }
    store->text_used = 0;
    store->action_count = 0;
    store->nondefault_count = 0;
    for (condition = 0; condition < gsh_traps_condition_count();
         condition++) {
        if (store->entries[condition].state != GSH_TRAP_DEFAULT) {
            store->nondefault_count++;
        }
    }
    any_pending = 0;
    return sigprocmask(SIG_SETMASK, &previous, NULL);
}

bool gsh_traps_have_pending(const gsh_trap_store *store)
{
    if (store == NULL) {
        return false;
    }
    return store != NULL && store->action_count != 0U &&
           any_pending != 0;
}

int gsh_traps_pending_signal(const gsh_trap_store *store)
{
    size_t condition;

    if (!gsh_traps_have_pending(store)) {
        return 0;
    }
    for (condition = 1U; condition < gsh_traps_condition_count();
         condition++) {
        if (pending[condition] != 0 &&
            store->entries[condition].state == GSH_TRAP_ACTION) {
            return CONDITIONS[condition].signal_number;
        }
    }
    return 0;
}

bool gsh_traps_take_pending(gsh_trap_store *store, size_t *condition)
{
    sigset_t blocked;
    sigset_t previous;
    size_t index;
    size_t selected = GSH_TRAP_CONDITION_CAP;
    bool more = false;

    if (store == NULL || condition == NULL ||
        !gsh_traps_have_pending(store)) {
        return false;
    }
    (void)sigemptyset(&blocked);
    for (index = 1U; index < gsh_traps_condition_count(); index++) {
        if (store->entries[index].state == GSH_TRAP_ACTION) {
            (void)sigaddset(&blocked, CONDITIONS[index].signal_number);
        }
    }
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        return false;
    }
    for (index = 1U; index < gsh_traps_condition_count(); index++) {
        if (pending[index] != 0 &&
            store->entries[index].state == GSH_TRAP_ACTION) {
            if (selected == GSH_TRAP_CONDITION_CAP) {
                selected = index;
                pending[index] = 0;
            } else {
                more = true;
            }
        }
    }
    any_pending = more ? 1 : 0;
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
    if (selected == GSH_TRAP_CONDITION_CAP) {
        return false;
    }
    *condition = selected;
    return true;
}
