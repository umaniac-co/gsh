#ifndef GSH_BUILTIN_FC_H
#define GSH_BUILTIN_FC_H

#include "builtin_common.h"
#include "history_store.h"
#include "shell_variables.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool execute;
    size_t command_length;
} gsh_fc_result;

int gsh_builtin_fc_prepare(size_t argc, char *const argv[],
                           const gsh_history_store *history,
                           const gsh_variable_store *variables,
                           bool exclude_newest, char *command,
                           size_t command_capacity,
                           gsh_fc_result *result,
                           const gsh_builtin_io *io);

#endif
