#ifndef GSH_BUILTIN_JOB_CONTROL_H
#define GSH_BUILTIN_JOB_CONTROL_H

#include "background_jobs.h"
#include "builtin_common.h"

#include <stddef.h>

int gsh_builtin_jobs(size_t argc, char *const argv[],
                     gsh_background_table *jobs,
                     const gsh_builtin_io *io);
int gsh_builtin_kill(size_t argc, char *const argv[],
                     const gsh_background_table *jobs,
                     const gsh_builtin_io *io);

#endif
