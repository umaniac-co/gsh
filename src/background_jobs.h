#ifndef GSH_BACKGROUND_JOBS_H
#define GSH_BACKGROUND_JOBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

enum { GSH_BACKGROUND_CAP = 128 };

typedef struct {
    pid_t pid;
    int wait_status;
    uint32_t job_id;
    bool known;
    bool done;
} gsh_background_entry;

typedef struct {
    gsh_background_entry entries[GSH_BACKGROUND_CAP];
    uint32_t next_job_id;
} gsh_background_table;

void gsh_background_initialize(gsh_background_table *table);
bool gsh_background_has_capacity(const gsh_background_table *table);
int gsh_background_add(gsh_background_table *table, pid_t pid,
                       uint32_t *job_id);
bool gsh_background_record(gsh_background_table *table, pid_t pid,
                           int wait_status);
bool gsh_background_get(const gsh_background_table *table, pid_t pid,
                        bool *done, int *wait_status);
pid_t gsh_background_job_pid(const gsh_background_table *table,
                             uint32_t job_id);
bool gsh_background_consume(gsh_background_table *table, pid_t pid,
                            int *wait_status);
size_t gsh_background_snapshot(const gsh_background_table *table,
                               pid_t output[GSH_BACKGROUND_CAP]);

#endif
