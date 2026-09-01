#ifndef GSH_BUILTIN_PURE_H
#define GSH_BUILTIN_PURE_H

#include "builtin_common.h"
#include "builtin_registry.h"

#include <stddef.h>

int gsh_builtin_run_pure(gsh_builtin_kind kind, size_t argc,
                         char *const argv[], const gsh_builtin_io *io);

#endif
