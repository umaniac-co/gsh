#ifndef GSH_BUILTIN_FILES_H
#define GSH_BUILTIN_FILES_H

#include "builtin_common.h"

#include <stddef.h>

typedef enum {
    GSH_FILE_BUILTIN_LS = 1,
    GSH_FILE_BUILTIN_LL,
    GSH_FILE_BUILTIN_VIEW,
} gsh_file_builtin_kind;

int gsh_builtin_run_files(gsh_file_builtin_kind kind, size_t argc,
                          char *const argv[], const gsh_builtin_io *io);

#endif
