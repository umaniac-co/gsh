#ifndef GSH_BACKGROUND_JOBS_H
#define GSH_BACKGROUND_JOBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum {
    GSH_BACKGROUND_CAP = 128,
    GSH_BACKGROUND_MEMBER_CAP = 64,
    GSH_BACKGROUND_COMMAND_CAP = 4096,
};

typedef enum {
    GSH_JOB_RUNNING,
    GSH_JOB_STOPPED,
    GSH_JOB_DONE,
} gsh_job_state;

typedef enum {
    GSH_JOB_ORIGIN_CLASSIC,
    GSH_JOB_ORIGIN_MANAGED,
    GSH_JOB_ORIGIN_ASYNC_LIST,
    GSH_JOB_ORIGIN_EVALUATOR,
} gsh_job_origin;

typedef enum {
    GSH_JOBSPEC_OK,
    GSH_JOBSPEC_MISSING,
    GSH_JOBSPEC_AMBIGUOUS,
    GSH_JOBSPEC_INVALID,
} gsh_jobspec_status;

typedef struct {
    pid_t pid;
    pid_t pgid;
    pid_t status_pid;
    pid_t members[GSH_BACKGROUND_MEMBER_CAP];
    unsigned char member_states[GSH_BACKGROUND_MEMBER_CAP];
    size_t member_count;
    size_t remaining;
    int wait_status;
    uint32_t job_id;
    uint64_t sequence;
    gsh_job_state state;
    gsh_job_origin origin;
    bool known;
    bool done;
    bool foreground;
    bool terminal_owned;
    bool notified;
    bool consumed;
    size_t command_length;
    char command[GSH_BACKGROUND_COMMAND_CAP];
} gsh_background_entry;

typedef struct {
    gsh_background_entry entries[GSH_BACKGROUND_CAP];
    size_t used;
    uint32_t next_job_id;
    uint32_t current_job_id;
    uint32_t previous_job_id;
    uint64_t next_sequence;
} gsh_background_table;

void gsh_background_initialize(gsh_background_table *table);
bool gsh_background_has_capacity(const gsh_background_table *table);
size_t gsh_background_active_count(const gsh_background_table *table);
int gsh_background_add(gsh_background_table *table, pid_t pid,
                       uint32_t *job_id);
int gsh_background_add_job(gsh_background_table *table, pid_t pgid,
                           pid_t status_pid, const pid_t *members,
                           size_t member_count, const char *command,
                           size_t command_length, gsh_job_origin origin,
                           uint32_t *job_id);
bool gsh_background_record(gsh_background_table *table, pid_t pid,
                           int wait_status);
bool gsh_background_update_member(gsh_background_table *table, pid_t pid,
                                  int wait_status);
bool gsh_background_get(const gsh_background_table *table, pid_t pid,
                        bool *done, int *wait_status);
pid_t gsh_background_job_pid(const gsh_background_table *table,
                             uint32_t job_id);
bool gsh_background_consume(gsh_background_table *table, pid_t pid,
                            int *wait_status);
bool gsh_background_consume_all_if_done(gsh_background_table *table);
size_t gsh_background_snapshot(const gsh_background_table *table,
                               pid_t output[GSH_BACKGROUND_CAP]);
size_t gsh_background_live_snapshot(
    const gsh_background_table *table,
    pid_t output[GSH_BACKGROUND_CAP]);
gsh_jobspec_status gsh_background_resolve(
    const gsh_background_table *table, const char *jobspec,
    uint32_t *job_id);
const gsh_background_entry *gsh_background_entry_for_id(
    const gsh_background_table *table, uint32_t job_id);
gsh_background_entry *gsh_background_mutable_entry_for_id(
    gsh_background_table *table, uint32_t job_id);
const gsh_background_entry *gsh_background_entry_for_pid(
    const gsh_background_table *table, pid_t pid);
bool gsh_background_mark_notified(gsh_background_table *table,
                                  uint32_t job_id);
bool gsh_background_continue_job(gsh_background_table *table,
                                 uint32_t job_id);
bool gsh_background_stop_job(gsh_background_table *table,
                             uint32_t job_id);
bool gsh_background_remove_job(gsh_background_table *table,
                               uint32_t job_id);
char gsh_background_marker(const gsh_background_table *table,
                           uint32_t job_id);

#endif
