#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_common.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

static size_t resource_text_width(const char *text, size_t length,
                                  size_t initial)
{
    mbstate_t state;
    size_t offset = 0U;
    size_t width = 0U;
    if (text == NULL) return 0U;
    (void)memset(&state, 0, sizeof(state));
    while (offset < length) {
        wchar_t character;
        size_t bytes;
        int columns;
        if (text[offset] == '\t') {
            size_t current = initial + width;
            width += 8U - current % 8U;
            offset++;
            continue;
        }
        bytes = mbrtowc(&character, text + offset, length - offset, &state);
        if (bytes == (size_t)-1 || bytes == (size_t)-2 || bytes == 0U) {
            width++;
            offset++;
            (void)memset(&state, 0, sizeof(state));
            continue;
        }
        columns = wcwidth(character);
        width += columns < 0 ? 1U : (size_t)columns;
        offset += bytes;
    }
    return width;
}

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

static void advance_resource_cursor(const gsh_builtin_io *io,
                                    int descriptor,
                                    const char *text, size_t length)
{
    size_t begin = 0U;
    size_t index;
    gsh_builtin_resource_sink *resources;
    if (io == NULL || io->resources == NULL || descriptor == STDERR_FILENO ||
        text == NULL) return;
    resources = io->resources;
    for (index = 0U; index < length; index++) {
        if (text[index] == '\n') {
            resources->visual_column += resource_text_width(
                text + begin, index - begin, resources->visual_column);
            resources->column += index - begin;
            resources->row++;
            resources->column = 0U;
            resources->visual_column = 0U;
            begin = index + 1U;
        } else if (text[index] == '\r') {
            resources->visual_column += resource_text_width(
                text + begin, index - begin, resources->visual_column);
            resources->column += index - begin;
            resources->column = 0U;
            resources->visual_column = 0U;
            begin = index + 1U;
        }
    }
    resources->visual_column += resource_text_width(
        text + begin, length - begin, resources->visual_column);
    resources->column += length - begin;
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
        int result = descriptor_output(target, text, length);
        if (result == 0) advance_resource_cursor(io, descriptor, text, length);
        return result;
    }
    if (io->kind == GSH_BUILTIN_SINK_BUFFER) {
        int result = bounded_buffer_output(io, text, length);
        if (result == 0) advance_resource_cursor(io, descriptor, text, length);
        return result;
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
