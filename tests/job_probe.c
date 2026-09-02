#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh tests require the POSIX.1-2024 feature-test baseline"
#endif

#include <errno.h>
#include <signal.h>
#include <unistd.h>

static int write_marker(const char *marker, size_t length)
{
    if (marker == NULL) {
        return -1;
    }
    size_t offset = 0;

    while (offset < length) {
        ssize_t written = write(STDOUT_FILENO, marker + offset, length - offset);

        if (written > 0) {
            offset += (size_t)written;
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

int main(void)
{
    static const char ready[] = "GSH_PROBE_READY\n";
    static const char continued[] = "GSH_PROBE_CONTINUED\n";

    if (write_marker(ready, sizeof(ready) - 1U) == -1 ||
        raise(SIGSTOP) != 0 ||
        write_marker(continued, sizeof(continued) - 1U) == -1) {
        return 1;
    }
    /* Lifecycle loop: the PTY harness terminates this stopped-process probe;
     * only delivered signals can advance it. */
    while (pause() == -1 && errno == EINTR) {
    }
    return 1;
}
