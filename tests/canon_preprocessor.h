#ifndef GSH_CANON_PREPROCESSOR_H
#define GSH_CANON_PREPROCESSOR_H

#include <stdbool.h>
#include <stddef.h>

int gsh_canon_preprocessor_analyze(const unsigned char *source,
                                   size_t length, const char *path,
                                   size_t *violations, bool report);

#endif
