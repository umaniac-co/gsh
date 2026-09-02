#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "background_jobs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void gsh_background_initialize(gsh_background_table *table)
{
    if (table == NULL) return;
    table->used = 0;
    table->next_job_id = 1U;
    table->current_job_id = 0;
    table->previous_job_id = 0;
    table->next_sequence = 1U;
}

static bool entry_reclaimable(const gsh_background_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    return !entry->known ||
           (entry->state == GSH_JOB_DONE &&
            (entry->notified || entry->consumed));
}

bool gsh_background_has_capacity(const gsh_background_table *table)
{
    size_t index;

    if (table == NULL || table->used > GSH_BACKGROUND_CAP) return false;
    if (table->used < GSH_BACKGROUND_CAP) return true;
    for (index = 0; index < table->used; index++) {
        if (entry_reclaimable(&table->entries[index])) {
            return true;
        }
    }
    return false;
}

size_t gsh_background_active_count(const gsh_background_table *table)
{
    size_t count = 0;
    size_t index;

    if (table == NULL) {
        return 0;
    }
    if (table->used > GSH_BACKGROUND_CAP) return 0;
    for (index = 0; index < table->used; index++) {
        if (table->entries[index].known &&
            table->entries[index].state != GSH_JOB_DONE) {
            count++;
        }
    }
    return count;
}

static bool job_id_in_use(const gsh_background_table *table,
                          uint32_t job_id)
{
    if (table == NULL) return false;
    size_t index;

    if (table->used > GSH_BACKGROUND_CAP) return true;
    for (index = 0; index < table->used; index++) {
        if (table->entries[index].known &&
            table->entries[index].job_id == job_id) return true;
    }
    return false;
}

static int next_job_id(gsh_background_table *table, uint32_t *job_id)
{
    if (job_id == NULL || table == NULL) {
        return -1;
    }
    size_t checked;

    for (checked = 0; checked <= GSH_BACKGROUND_CAP; checked++) {
        uint32_t candidate = table->next_job_id++;

        if (table->next_job_id == 0) table->next_job_id = 1U;
        if (candidate != 0 && !job_id_in_use(table, candidate)) {
            *job_id = candidate;
            return 0;
        }
    }
    errno = EAGAIN;
    return -1;
}

static void refresh_current_jobs(gsh_background_table *table)
{
    if (table == NULL) {
        return;
    }
    uint64_t newest = 0;
    uint64_t prior = 0;
    uint32_t current = 0;
    uint32_t previous = 0;
    size_t index;

    for (index = 0; index < table->used; index++) {
        const gsh_background_entry *entry = &table->entries[index];

        if (!entry->known) continue;
        if (entry->sequence > newest) {
            prior = newest;
            previous = current;
            newest = entry->sequence;
            current = entry->job_id;
        } else if (entry->sequence > prior) {
            prior = entry->sequence;
            previous = entry->job_id;
        }
    }
    table->current_job_id = current;
    table->previous_job_id = previous;
}

static gsh_background_entry *available_entry(gsh_background_table *table)
{
    if (table == NULL) return NULL;
    gsh_background_entry *free_entry = NULL;
    size_t index;

    if (table->used > GSH_BACKGROUND_CAP) {
        errno = EINVAL;
        return NULL;
    }
    for (index = 0; index < table->used; index++) {
        gsh_background_entry *entry = &table->entries[index];

        if (entry_reclaimable(entry) && free_entry == NULL) {
            free_entry = entry;
        }
    }
    if (free_entry == NULL && table->used < GSH_BACKGROUND_CAP) {
        free_entry = &table->entries[table->used++];
    } else if (free_entry == NULL) {
        errno = EAGAIN;
        return NULL;
    }
    return free_entry;
}

static bool member_in_use(const gsh_background_table *table, pid_t pid)
{
    if (table == NULL) return false;
    size_t entry_index;

    if (table->used > GSH_BACKGROUND_CAP) return true;
    for (entry_index = 0; entry_index < table->used;
         entry_index++) {
        const gsh_background_entry *entry = &table->entries[entry_index];
        size_t member;

        if (!entry->known) continue;
        for (member = 0; member < entry->member_count; member++) {
            if (entry->members[member] == pid) return true;
        }
    }
    return false;
}

int gsh_background_add_job(gsh_background_table *table, pid_t pgid,
                           pid_t status_pid, const pid_t *members,
                           size_t member_count, const char *command,
                           size_t command_length, gsh_job_origin origin,
                           uint32_t *job_id)
{
    gsh_background_entry *free_entry;
    uint32_t assigned;
    size_t index;

    if (table == NULL || pgid <= 0 || status_pid <= 0 || members == NULL ||
        member_count == 0 || member_count > GSH_BACKGROUND_MEMBER_CAP ||
        command_length >= GSH_BACKGROUND_COMMAND_CAP ||
        (command_length != 0 && command == NULL) ||
        table->used > GSH_BACKGROUND_CAP) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0; index < member_count; index++) {
        if (members[index] <= 0 || member_in_use(table, members[index])) {
            errno = members[index] <= 0 ? EINVAL : EEXIST;
            return -1;
        }
    }
    if (next_job_id(table, &assigned) == -1) {
        return -1;
    }
    free_entry = available_entry(table);
    if (free_entry == NULL) {
        return -1;
    }
    free_entry->pid = status_pid;
    free_entry->pgid = pgid;
    free_entry->status_pid = status_pid;
    (void)memcpy(free_entry->members, members, member_count * sizeof(members[0]));
    (void)memset(free_entry->member_states, 0,
           member_count * sizeof(free_entry->member_states[0]));
    free_entry->member_count = member_count;
    free_entry->remaining = member_count;
    free_entry->wait_status = 0;
    free_entry->job_id = assigned;
    free_entry->sequence = table->next_sequence++;
    if (table->next_sequence == 0) table->next_sequence = 1U;
    free_entry->state = GSH_JOB_RUNNING;
    free_entry->origin = origin;
    free_entry->known = true;
    free_entry->done = false;
    free_entry->foreground = false;
    free_entry->terminal_owned = false;
    free_entry->notified = false;
    free_entry->consumed = false;
    if (command_length != 0) {
        (void)memcpy(free_entry->command, command, command_length);
    }
    free_entry->command[command_length] = '\0';
    free_entry->command_length = command_length;
    table->previous_job_id = table->current_job_id;
    table->current_job_id = assigned;
    if (job_id != NULL) {
        *job_id = assigned;
    }
    return 0;
}

int gsh_background_add(gsh_background_table *table, pid_t pid,
                       uint32_t *job_id)
{
    return gsh_background_add_job(
        table, pid, pid, &pid, 1U, NULL, 0,
        GSH_JOB_ORIGIN_ASYNC_LIST, job_id);
}

static bool all_members_stopped(const gsh_background_entry *entry)
{
    if (entry == NULL) return false;
    size_t index;

    if (entry->remaining == 0) return false;
    for (index = 0; index < entry->member_count; index++) {
        if (entry->member_states[index] == 0U) return false;
    }
    return true;
}

bool gsh_background_update_member(gsh_background_table *table, pid_t pid,
                                  int wait_status)
{
    size_t entry_index;

    if (table == NULL || table->used > GSH_BACKGROUND_CAP) return false;
    for (entry_index = 0; entry_index < table->used;
         entry_index++) {
        gsh_background_entry *entry = &table->entries[entry_index];
        size_t member;

        if (!entry->known) continue;
        for (member = 0; member < entry->member_count; member++) {
            if (entry->members[member] != pid) continue;
            if (WIFSTOPPED(wait_status)) {
                entry->member_states[member] = 1U;
                if (all_members_stopped(entry)) {
                    entry->state = GSH_JOB_STOPPED;
                    entry->notified = false;
                    if (table->current_job_id != entry->job_id) {
                        table->previous_job_id = table->current_job_id;
                        table->current_job_id = entry->job_id;
                    }
                }
#ifdef WIFCONTINUED
            } else if (WIFCONTINUED(wait_status)) {
                entry->member_states[member] = 0U;
                entry->state = GSH_JOB_RUNNING;
                entry->notified = false;
#endif
            } else if (WIFEXITED(wait_status) || WIFSIGNALED(wait_status)) {
                if (entry->member_states[member] != 2U) {
                    entry->member_states[member] = 2U;
                    if (entry->remaining > 0) entry->remaining--;
                }
                if (pid == entry->status_pid) {
                    entry->wait_status = wait_status;
                }
                if (entry->remaining == 0) {
                    entry->state = GSH_JOB_DONE;
                    entry->done = true;
                    entry->notified = false;
                }
            }
            return true;
        }
    }
    return false;
}

bool gsh_background_record(gsh_background_table *table, pid_t pid,
                           int wait_status)
{
    if (table == NULL) {
        return false;
    }
    return gsh_background_update_member(table, pid, wait_status);
}

bool gsh_background_get(const gsh_background_table *table, pid_t pid,
                        bool *done, int *wait_status)
{
    size_t index;

    if (table == NULL || table->used > GSH_BACKGROUND_CAP) return false;
    for (index = 0; index < table->used; index++) {
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

    if (table == NULL || table->used > GSH_BACKGROUND_CAP) return -1;
    for (index = 0; index < table->used; index++) {
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

    if (table == NULL || table->used > GSH_BACKGROUND_CAP) return false;
    for (index = 0; index < table->used; index++) {
        gsh_background_entry *entry = &table->entries[index];

        if (entry->known && entry->pid == pid && entry->done) {
            if (wait_status != NULL) {
                *wait_status = entry->wait_status;
            }
            (void)memset(entry->command, 0, entry->command_length + 1U);
            entry->command_length = 0;
            entry->consumed = true;
            entry->known = false;
            while (table->used != 0 &&
                   !table->entries[table->used - 1U].known) {
                table->used--;
            }
            refresh_current_jobs(table);
            return true;
        }
    }
    return false;
}

bool gsh_background_consume_all_if_done(gsh_background_table *table)
{
    size_t index;

    if (table == NULL || table->used > GSH_BACKGROUND_CAP) return false;
    for (index = 0; index < table->used; index++) {
        const gsh_background_entry *entry = &table->entries[index];

        if (entry->known && entry->state != GSH_JOB_DONE) return false;
    }
    for (index = 0; index < table->used; index++) {
        table->entries[index].known = false;
        table->entries[index].consumed = true;
        table->entries[index].command_length = 0;
    }
    table->used = 0;
    table->current_job_id = 0;
    table->previous_job_id = 0;
    return true;
}

size_t gsh_background_snapshot(const gsh_background_table *table,
                               pid_t output[GSH_BACKGROUND_CAP])
{
    if (output == NULL) {
        return 0U;
    }
    size_t count = 0;
    size_t index;

    if (table == NULL || table->used > GSH_BACKGROUND_CAP) return 0;
    for (index = 0; index < table->used; index++) {
        if (table->entries[index].known) {
            output[count++] = table->entries[index].pid;
        }
    }
    return count;
}

size_t gsh_background_live_snapshot(
    const gsh_background_table *table,
    pid_t output[GSH_BACKGROUND_CAP])
{
    if (output == NULL) {
        return 0U;
    }
    size_t count = 0;
    size_t index;

    if (table == NULL || table->used > GSH_BACKGROUND_CAP) return 0;
    for (index = 0; index < table->used; index++) {
        if (table->entries[index].known &&
            table->entries[index].state != GSH_JOB_DONE) {
            output[count++] = table->entries[index].pid;
        }
    }
    return count;
}

const gsh_background_entry *gsh_background_entry_for_id(
    const gsh_background_table *table, uint32_t job_id)
{
    size_t index;

    if (table == NULL || job_id == 0) return NULL;
    if (table->used > GSH_BACKGROUND_CAP) return NULL;
    for (index = 0; index < table->used; index++) {
        if (table->entries[index].known &&
            table->entries[index].job_id == job_id) {
            return &table->entries[index];
        }
    }
    return NULL;
}

gsh_background_entry *gsh_background_mutable_entry_for_id(
    gsh_background_table *table, uint32_t job_id)
{
    if (table == NULL) {
        return NULL;
    }
    return (gsh_background_entry *)gsh_background_entry_for_id(table,
                                                               job_id);
}

const gsh_background_entry *gsh_background_entry_for_pid(
    const gsh_background_table *table, pid_t pid)
{
    size_t index;

    if (table == NULL || pid <= 0) return NULL;
    if (table->used > GSH_BACKGROUND_CAP) return NULL;
    for (index = 0; index < table->used; index++) {
        const gsh_background_entry *entry = &table->entries[index];
        size_t member;

        if (!entry->known) continue;
        if (entry->pid == pid || entry->pgid == pid) return entry;
        for (member = 0; member < entry->member_count; member++) {
            if (entry->members[member] == pid) return entry;
        }
    }
    return NULL;
}

static bool command_matches(const gsh_background_entry *entry,
                            const char *text, bool substring)
{
    if (entry == NULL) return false;
    if (text == NULL) {
        return false;
    }
    size_t length = strlen(text);

    if (length == 0 || entry->command_length == 0) return false;
    return substring ? strstr(entry->command, text) != NULL
                     : length <= entry->command_length &&
                           memcmp(entry->command, text, length) == 0;
}

static gsh_jobspec_status resolve_textual(
    const gsh_background_table *table, const char *text, bool substring,
    uint32_t *job_id)
{
    if (table == NULL) return GSH_JOBSPEC_INVALID;
    if (job_id == NULL) {
        return GSH_JOBSPEC_INVALID;
    }
    size_t matches = 0;
    size_t index;

    if (table->used > GSH_BACKGROUND_CAP) return GSH_JOBSPEC_INVALID;
    for (index = 0; index < table->used; index++) {
        const gsh_background_entry *entry = &table->entries[index];

        if (entry->known && command_matches(entry, text, substring)) {
            *job_id = entry->job_id;
            matches++;
        }
    }
    if (matches == 1U) return GSH_JOBSPEC_OK;
    return matches == 0 ? GSH_JOBSPEC_MISSING : GSH_JOBSPEC_AMBIGUOUS;
}

gsh_jobspec_status gsh_background_resolve(
    const gsh_background_table *table, const char *jobspec,
    uint32_t *job_id)
{
    char *end;
    unsigned long number;

    if (table == NULL || jobspec == NULL || job_id == NULL ||
        jobspec[0] != '%') return GSH_JOBSPEC_INVALID;
    if (strcmp(jobspec, "%%") == 0 || strcmp(jobspec, "%+") == 0) {
        *job_id = table->current_job_id;
    } else if (strcmp(jobspec, "%-") == 0) {
        *job_id = table->previous_job_id;
    } else if (jobspec[1] == '?') {
        return resolve_textual(table, jobspec + 2U, true, job_id);
    } else if (jobspec[1] >= '0' && jobspec[1] <= '9') {
        errno = 0;
        number = strtoul(jobspec + 1U, &end, 10);
        if (errno != 0 || *end != '\0' || number == 0 ||
            number > UINT32_MAX) return GSH_JOBSPEC_INVALID;
        *job_id = (uint32_t)number;
    } else {
        return resolve_textual(table, jobspec + 1U, false, job_id);
    }
    return *job_id != 0 &&
                   gsh_background_entry_for_id(table, *job_id) != NULL
               ? GSH_JOBSPEC_OK : GSH_JOBSPEC_MISSING;
}

bool gsh_background_mark_notified(gsh_background_table *table,
                                  uint32_t job_id)
{
    if (table == NULL) {
        return false;
    }
    gsh_background_entry *entry =
        gsh_background_mutable_entry_for_id(table, job_id);

    if (entry == NULL) return false;
    entry->notified = true;
    return true;
}

bool gsh_background_continue_job(gsh_background_table *table,
                                 uint32_t job_id)
{
    if (table == NULL) {
        return false;
    }
    gsh_background_entry *entry =
        gsh_background_mutable_entry_for_id(table, job_id);
    size_t member;

    if (entry == NULL || entry->state == GSH_JOB_DONE) return false;
    for (member = 0; member < entry->member_count; member++) {
        if (entry->member_states[member] == 1U) {
            entry->member_states[member] = 0U;
        }
    }
    entry->state = GSH_JOB_RUNNING;
    entry->notified = false;
    if (table->current_job_id != job_id) {
        table->previous_job_id = table->current_job_id;
        table->current_job_id = job_id;
    }
    return true;
}

bool gsh_background_stop_job(gsh_background_table *table,
                             uint32_t job_id)
{
    if (table == NULL) {
        return false;
    }
    gsh_background_entry *entry =
        gsh_background_mutable_entry_for_id(table, job_id);
    size_t member;

    if (entry == NULL || entry->state == GSH_JOB_DONE) return false;
    for (member = 0; member < entry->member_count; member++) {
        if (entry->member_states[member] != 2U) {
            entry->member_states[member] = 1U;
        }
    }
    entry->state = GSH_JOB_STOPPED;
    entry->notified = false;
    if (table->current_job_id != job_id) {
        table->previous_job_id = table->current_job_id;
        table->current_job_id = job_id;
    }
    return true;
}

bool gsh_background_remove_job(gsh_background_table *table,
                               uint32_t job_id)
{
    if (table == NULL) {
        return false;
    }
    gsh_background_entry *entry =
        gsh_background_mutable_entry_for_id(table, job_id);

    if (entry == NULL) return false;
    (void)memset(entry->command, 0, entry->command_length + 1U);
    entry->command_length = 0;
    entry->known = false;
    while (table->used != 0 &&
           !table->entries[table->used - 1U].known) {
        table->used--;
    }
    refresh_current_jobs(table);
    return true;
}

char gsh_background_marker(const gsh_background_table *table,
                           uint32_t job_id)
{
    if (table == NULL) return ' ';
    if (table->current_job_id == job_id) return '+';
    if (table->previous_job_id == job_id) return '-';
    return ' ';
}
