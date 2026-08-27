#ifndef GSH_BUILTIN_ALIAS_H
#define GSH_BUILTIN_ALIAS_H

#include "builtin_common.h"
#include "shell_aliases.h"

#include <stddef.h>

int gsh_builtin_alias(size_t argc, char *const argv[],
                      gsh_alias_store *aliases,
                      gsh_alias_journal *journal,
                      const gsh_builtin_io *io);

#endif
