#ifndef GSH_BUILTIN_TRAP_H
#define GSH_BUILTIN_TRAP_H

#include "builtin_common.h"
#include "shell_traps.h"

int gsh_builtin_trap(int argc, char *const argv[], gsh_trap_store *traps,
                     const gsh_builtin_io *io);

#endif
