#ifndef GSH_BUILTIN_VARIABLES_H
#define GSH_BUILTIN_VARIABLES_H

#include "builtin_common.h"
#include "shell_variables.h"

#include <stddef.h>

int gsh_builtin_variables(size_t argc, char *const argv[],
                          gsh_variable_store *variables,
                          gsh_variable_journal *journal,
                          unsigned int assignment_attributes,
                          const gsh_builtin_io *io);

#endif
