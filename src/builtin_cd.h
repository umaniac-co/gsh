#ifndef GSH_BUILTIN_CD_H
#define GSH_BUILTIN_CD_H

#include <stddef.h>

#include "builtin_common.h"
#include "shell_variables.h"

int gsh_builtin_cd(size_t argc, char *const argv[],
                   const gsh_variable_store *lookup_variables,
                   gsh_variable_store *variables,
                   gsh_variable_journal *journal,
                   unsigned int assignment_attributes,
                   const gsh_builtin_io *io, char *directory,
                   size_t directory_capacity);

#endif
