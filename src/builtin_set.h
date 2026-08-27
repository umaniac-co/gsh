#ifndef GSH_BUILTIN_SET_H
#define GSH_BUILTIN_SET_H

#include "builtin_common.h"
#include "positional_parameters.h"
#include "shell_options.h"
#include "shell_variables.h"

#include <stdbool.h>
#include <stddef.h>

bool gsh_builtin_set_mutates_positionals(size_t argc,
                                         char *const argv[]);
int gsh_builtin_set(size_t argc, char *const argv[],
                    gsh_variable_store *variables,
                    gsh_positional_store *positionals,
                    gsh_shell_options *options,
                    const gsh_builtin_io *io);

#endif
