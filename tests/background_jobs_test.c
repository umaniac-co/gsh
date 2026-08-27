#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "../src/background_jobs.h"

#include <errno.h>
#include <stdio.h>

int main(void)
{
    gsh_background_table table;
    pid_t snapshot[GSH_BACKGROUND_CAP];
    uint32_t first_job = 0;
    uint32_t job;
    bool done;
    int wait_status;
    size_t index;

    gsh_background_initialize(&table);
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
    puts("background jobs: capacity, identity, completion and reuse passed");
    return 0;
}
