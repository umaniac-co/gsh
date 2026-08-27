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

int gsh_builtin_descriptor_output(void *opaque, int descriptor,
                                  const char *text, size_t length)
{
    (void)opaque;
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

int gsh_builtin_error(const gsh_builtin_io *io, const char *name,
                      const char *message)
{
    (void)io->output(io->opaque, STDERR_FILENO, "gsh: ", 5);
    (void)io->output(io->opaque, STDERR_FILENO, name, strlen(name));
    (void)io->output(io->opaque, STDERR_FILENO, ": ", 2);
    (void)io->output(io->opaque, STDERR_FILENO, message, strlen(message));
    (void)io->output(io->opaque, STDERR_FILENO, "\n", 1);
    return 1;
}
