#ifndef GSH_BUILTIN_COMMON_H
#define GSH_BUILTIN_COMMON_H

#include <stddef.h>

typedef int (*gsh_builtin_output_fn)(void *opaque, int descriptor,
                                     const char *text, size_t length);

typedef struct {
    gsh_builtin_output_fn output;
    void *opaque;
} gsh_builtin_io;

int gsh_builtin_descriptor_output(void *opaque, int descriptor,
                                  const char *text, size_t length);
int gsh_builtin_error(const gsh_builtin_io *io, const char *name,
                      const char *message);

#endif
