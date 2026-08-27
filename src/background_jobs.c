#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "background_jobs.h"

#include <errno.h>
#include <string.h>

void gsh_background_initialize(gsh_background_table *table)
{
    memset(table, 0, sizeof(*table));
    table->next_job_id = 1U;
}

bool gsh_background_has_capacity(const gsh_background_table *table)
{
    size_t index;

    for (index = 0; index < GSH_BACKGROUND_CAP; index++) {
        if (!table->entries[index].known) {
            return true;
        }
    }
    return false;
}

int gsh_background_add(gsh_background_table *table, pid_t pid,
                       uint32_t *job_id)
{
    gsh_background_entry *free_entry = NULL;
    size_t index;

    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0; index < GSH_BACKGROUND_CAP; index++) {
        gsh_background_entry *entry = &table->entries[index];

        if (entry->known && entry->pid == pid) {
            errno = EEXIST;
            return -1;
        }
        if (!entry->known && free_entry == NULL) {
            free_entry = entry;
        }
    }
    if (free_entry == NULL) {
        errno = EAGAIN;
        return -1;
    }
    free_entry->pid = pid;
    free_entry->wait_status = 0;
    free_entry->job_id = table->next_job_id++;
    if (table->next_job_id == 0) {
        table->next_job_id = 1U;
    }
    free_entry->known = true;
    free_entry->done = false;
    if (job_id != NULL) {
        *job_id = free_entry->job_id;
    }
    return 0;
}

bool gsh_background_record(gsh_background_table *table, pid_t pid,
                           int wait_status)
{
    size_t index;

    for (index = 0; index < GSH_BACKGROUND_CAP; index++) {
        gsh_background_entry *entry = &table->entries[index];

        if (entry->known && entry->pid == pid) {
            entry->wait_status = wait_status;
            entry->done = true;
            return true;
        }
    }
    return false;
}

bool gsh_background_get(const gsh_background_table *table, pid_t pid,
                        bool *done, int *wait_status)
{
    size_t index;

    for (index = 0; index < GSH_BACKGROUND_CAP; index++) {
        const gsh_background_entry *entry = &table->entries[index];

        if (entry->known && entry->pid == pid) {
            if (done != NULL) {
                *done = entry->done;
            }
            if (wait_status != NULL && entry->done) {
                *wait_status = entry->wait_status;
            }
            return true;
        }
    }
    return false;
}

pid_t gsh_background_job_pid(const gsh_background_table *table,
                             uint32_t job_id)
{
    size_t index;

    for (index = 0; index < GSH_BACKGROUND_CAP; index++) {
        if (table->entries[index].known &&
            table->entries[index].job_id == job_id) {
            return table->entries[index].pid;
        }
    }
    return -1;
}

bool gsh_background_consume(gsh_background_table *table, pid_t pid,
                            int *wait_status)
{
    size_t index;

    for (index = 0; index < GSH_BACKGROUND_CAP; index++) {
        gsh_background_entry *entry = &table->entries[index];

        if (entry->known && entry->pid == pid && entry->done) {
            if (wait_status != NULL) {
                *wait_status = entry->wait_status;
            }
            memset(entry, 0, sizeof(*entry));
            return true;
        }
    }
    return false;
}

size_t gsh_background_snapshot(const gsh_background_table *table,
                               pid_t output[GSH_BACKGROUND_CAP])
{
    size_t count = 0;
    size_t index;

    for (index = 0; index < GSH_BACKGROUND_CAP; index++) {
        if (table->entries[index].known) {
            output[count++] = table->entries[index].pid;
        }
    }
    return count;
}
