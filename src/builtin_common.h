#ifndef GSH_BUILTIN_COMMON_H
#define GSH_BUILTIN_COMMON_H

#include "async_repl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GSH_BUILTIN_SINK_DESCRIPTORS,
    GSH_BUILTIN_SINK_BUFFER,
} gsh_builtin_sink_kind;

typedef struct {
    int descriptor;
    size_t row;
    size_t column;
    size_t visual_column;
    const char *base_directory;
    bool navigable_root;
} gsh_builtin_resource_sink;

typedef struct {
    gsh_builtin_sink_kind kind;
    struct {
        int output;
        int error;
    } descriptors;
    struct {
        char *bytes;
        size_t capacity;
        size_t *offset;
        size_t *length;
        uint64_t *overloads;
        gsh_async_repl *async_repl;
        int async_cell;
    } buffer;
    gsh_builtin_resource_sink *resources;
} gsh_builtin_io;

int gsh_builtin_output(const gsh_builtin_io *io, int descriptor,
                       const char *text, size_t length);
bool gsh_builtin_io_valid(const gsh_builtin_io *io);
int gsh_builtin_error(const gsh_builtin_io *io, const char *name,
                      const char *message);

#endif
