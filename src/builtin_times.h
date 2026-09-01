#ifndef GSH_BUILTIN_TIMES_H
#define GSH_BUILTIN_TIMES_H

#include "builtin_common.h"

#include <stdbool.h>
#include <sys/times.h>

typedef struct {
    struct tms accumulated;
    struct tms process_origin;
    bool active;
} gsh_times_context;

int gsh_times_snapshot(struct tms *snapshot);
int gsh_times_rebase(gsh_times_context *context,
                     const struct tms *accumulated);
int gsh_builtin_times(size_t argc, char *const argv[],
                      const gsh_times_context *context,
                      const gsh_builtin_io *io);

#endif
