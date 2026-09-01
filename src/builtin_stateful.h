#ifndef GSH_BUILTIN_STATEFUL_H
#define GSH_BUILTIN_STATEFUL_H

#include "builtin_common.h"
#include "positional_parameters.h"
#include "shell_options.h"
#include "shell_variables.h"

#include <stddef.h>

int gsh_builtin_read(size_t argc, char *const argv[],
                     const gsh_variable_store *lookup_variables,
                     gsh_variable_store *variables,
                     gsh_variable_store *scratch,
                     gsh_variable_journal *journal,
                     const gsh_builtin_io *io);
int gsh_builtin_getopts(size_t argc, char *const argv[],
                        const gsh_variable_store *lookup_variables,
                        gsh_variable_store *variables,
                        gsh_variable_store *scratch,
                        gsh_variable_journal *journal,
                        const gsh_positional_store *positionals,
                        gsh_shell_options *options,
                        const gsh_builtin_io *io);

#endif
