#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "../src/background_jobs.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    gsh_background_table table;
    pid_t snapshot[GSH_BACKGROUND_CAP];
    uint32_t first_job = 0;
    uint32_t alpha_job = 0;
    uint32_t beta_job = 0;
    uint32_t job;
    bool done;
    int wait_status;
    size_t index;

    gsh_background_initialize(&table);
    {
        const pid_t alpha_members[] = {2001, 2002};
        const pid_t beta_members[] = {3001};
        const gsh_background_entry *entry;
        int stopped = (SIGTSTP << 8) | 0x7f;

        if (gsh_background_add_job(
                &table, 2001, 2002, alpha_members, 2U,
                "sleep alpha", strlen("sleep alpha"),
                GSH_JOB_ORIGIN_CLASSIC, &alpha_job) == -1 ||
            gsh_background_add_job(
                &table, 3001, 3001, beta_members, 1U,
                "sleep beta", strlen("sleep beta"),
                GSH_JOB_ORIGIN_MANAGED, &beta_job) == -1 ||
            gsh_background_resolve(&table, "%%", &job) != GSH_JOBSPEC_OK ||
            job != beta_job ||
            gsh_background_resolve(&table, "%-", &job) != GSH_JOBSPEC_OK ||
            job != alpha_job ||
            gsh_background_resolve(&table, "%sleep", &job) !=
                GSH_JOBSPEC_AMBIGUOUS ||
            gsh_background_resolve(&table, "%?alpha", &job) !=
                GSH_JOBSPEC_OK ||
            job != alpha_job ||
            !gsh_background_update_member(&table, 2001, stopped) ||
            !gsh_background_update_member(&table, 2002, stopped)) {
            return 1;
        }
        entry = gsh_background_entry_for_id(&table, alpha_job);
        if (entry == NULL || entry->state != GSH_JOB_STOPPED ||
            gsh_background_marker(&table, alpha_job) != '+' ||
            !gsh_background_continue_job(&table, alpha_job) ||
            entry->state != GSH_JOB_RUNNING ||
            entry->member_states[0] != 0U ||
            entry->member_states[1] != 0U ||
            !gsh_background_stop_job(&table, alpha_job) ||
            entry->state != GSH_JOB_STOPPED ||
            entry->member_states[0] != 1U ||
            entry->member_states[1] != 1U ||
            !gsh_background_continue_job(&table, alpha_job) ||
            !gsh_background_mark_notified(&table, alpha_job) ||
            !gsh_background_update_member(&table, 2001, 0) ||
            !gsh_background_update_member(&table, 2002, 7 << 8)) {
            return 1;
        }
        entry = gsh_background_entry_for_id(&table, alpha_job);
        if (entry == NULL || entry->state != GSH_JOB_DONE ||
            entry->notified || entry->wait_status != 7 << 8 ||
            !gsh_background_remove_job(&table, alpha_job) ||
            !gsh_background_remove_job(&table, beta_job)) {
            return 1;
        }
    }
    for (index = 0; index < GSH_BACKGROUND_CAP; index++) {
        if (!gsh_background_has_capacity(&table) ||
            gsh_background_add(&table, (pid_t)(1000 + index), &job) == -1 ||
            (index == 0 && ((first_job = job) == 0))) {
            return 1;
        }
    }
    errno = 0;
    if (gsh_background_has_capacity(&table) ||
        gsh_background_add(&table, 9999, NULL) != -1 || errno != EAGAIN ||
        gsh_background_snapshot(&table, snapshot) != GSH_BACKGROUND_CAP ||
        snapshot[0] != 1000 ||
        gsh_background_job_pid(&table, first_job) != 1000) {
        return 1;
    }
    if (!gsh_background_record(&table, 1000, 7 << 8) ||
        !gsh_background_get(&table, 1000, &done, &wait_status) || !done ||
        wait_status != 7 << 8 ||
        !gsh_background_consume(&table, 1000, &wait_status) ||
        wait_status != 7 << 8 ||
        gsh_background_get(&table, 1000, NULL, NULL) ||
        !gsh_background_has_capacity(&table)) {
        return 1;
    }
    errno = 0;
    if (gsh_background_add(&table, 1001, NULL) != -1 || errno != EEXIST ||
        gsh_background_add(&table, 9999, &job) == -1 || job == first_job) {
        return 1;
    }
    if (gsh_background_consume_all_if_done(&table)) return 1;
    {
        size_t count = gsh_background_snapshot(&table, snapshot);

        for (index = 0; index < count; index++) {
            if (!gsh_background_record(&table, snapshot[index], 0)) {
                return 1;
            }
        }
    }
    if (!gsh_background_consume_all_if_done(&table) ||
        gsh_background_snapshot(&table, snapshot) != 0 ||
        !gsh_background_has_capacity(&table)) {
        return 1;
    }
    (void)puts("background jobs: jobspec, transitions, notification, capacity, "
         "identity, completion, atomic wait-all and reuse passed");
    return 0;
}
