#ifndef GSH_BUILTIN_UMASK_H
#define GSH_BUILTIN_UMASK_H

#include "builtin_common.h"

int gsh_builtin_umask(size_t argc, char *const argv[],
                      const gsh_builtin_io *io);

#endif
