#ifndef GSH_BUILTIN_SHIFT_H
#define GSH_BUILTIN_SHIFT_H

#include "builtin_common.h"
#include "positional_parameters.h"

#include <stddef.h>

int gsh_builtin_shift(size_t argc, char *const argv[],
                      gsh_positional_store *positionals,
                      const gsh_builtin_io *io);

#endif
