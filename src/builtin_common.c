#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_common.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static int descriptor_output(int descriptor, const char *text,
                             size_t length)
{
    if (text == NULL) {
        return -1;
    }
    while (length > 0) {
        ssize_t written = write(descriptor, text, length);

        if (written > 0) {
            text += (size_t)written;
            length -= (size_t)written;
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            return 1;
        }
    }
    return 0;
}

static int bounded_buffer_output(const gsh_builtin_io *io,
                                 const char *text, size_t length)
{
    if (io == NULL || text == NULL) {
        return -1;
    }
    size_t *offset = io->buffer.offset;
    size_t *used = io->buffer.length;

    if (io->buffer.async_repl != NULL) {
        return gsh_async_repl_append(io->buffer.async_repl,
                                     io->buffer.async_cell, text,
                                     length) >= 0
                   ? 0
                   : 1;
    }
    if (io->buffer.bytes == NULL || offset == NULL || used == NULL ||
        *used > io->buffer.capacity || length > io->buffer.capacity - *used) {
        if (io->buffer.overloads != NULL) {
            (*io->buffer.overloads)++;
        }
        return 1;
    }
    if (*offset + *used + length > io->buffer.capacity) {
        (void)memmove(io->buffer.bytes, io->buffer.bytes + *offset, *used);
        *offset = 0;
    }
    (void)memcpy(io->buffer.bytes + *offset + *used, text, length);
    *used += length;
    return 0;
}

bool gsh_builtin_io_valid(const gsh_builtin_io *io)
{
    if (io == NULL) {
        return false;
    }
    if (io->kind == GSH_BUILTIN_SINK_DESCRIPTORS) {
        return io->descriptors.output >= 0 && io->descriptors.error >= 0;
    }
    return io->kind == GSH_BUILTIN_SINK_BUFFER &&
           (io->buffer.async_repl != NULL ||
            (io->buffer.bytes != NULL && io->buffer.offset != NULL &&
             io->buffer.length != NULL));
}

int gsh_builtin_output(const gsh_builtin_io *io, int descriptor,
                       const char *text, size_t length)
{
    if (!gsh_builtin_io_valid(io) || text == NULL) {
        errno = EINVAL;
        return 1;
    }
    if (io->kind == GSH_BUILTIN_SINK_DESCRIPTORS) {
        int target = descriptor == STDERR_FILENO ? io->descriptors.error
                                                  : io->descriptors.output;

        return descriptor_output(target, text, length);
    }
    if (io->kind == GSH_BUILTIN_SINK_BUFFER) {
        return bounded_buffer_output(io, text, length);
    }
    errno = EINVAL;
    return 1;
}

int gsh_builtin_error(const gsh_builtin_io *io, const char *name,
                      const char *message)
{
    if (io == NULL || message == NULL || name == NULL) {
        return -1;
    }
    (void)gsh_builtin_output(io, STDERR_FILENO, "gsh: ", 5);
    (void)gsh_builtin_output(io, STDERR_FILENO, name, strlen(name));
    (void)gsh_builtin_output(io, STDERR_FILENO, ": ", 2);
    (void)gsh_builtin_output(io, STDERR_FILENO, message, strlen(message));
    (void)gsh_builtin_output(io, STDERR_FILENO, "\n", 1);
    return 1;
}
