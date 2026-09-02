#ifndef GSH_NATIVE_VIEWER_H
#define GSH_NATIVE_VIEWER_H

#include "builtin_common.h"

#include <stddef.h>

enum {
    GSH_VIEWER_SPLIT_MIN_COLUMNS = 100,
    GSH_VIEWER_REPL_MIN_COLUMNS = 48,
};

int gsh_native_viewer(const char *path, size_t line, size_t column,
                      const gsh_builtin_io *io);

#endif
