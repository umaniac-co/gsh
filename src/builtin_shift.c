#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_shift.h"

#include <errno.h>
#include <stdint.h>

int gsh_builtin_shift(size_t argc, char *const argv[],
                      gsh_positional_store *positionals,
                      const gsh_builtin_io *io)
{
    size_t amount = 1;
    size_t index;

    if (argc > 2U || (argc == 2U && argv[1][0] == '\0')) {
        return gsh_builtin_error(io, "shift", "invalid shift count");
    }
    if (argc == 2U) {
        amount = 0;
        for (index = 0; argv[1][index] != '\0'; index++) {
            unsigned char byte = (unsigned char)argv[1][index];
            size_t digit;

            if (byte < '0' || byte > '9') {
                return gsh_builtin_error(io, "shift",
                                         "invalid shift count");
            }
            digit = byte - (unsigned char)'0';
            if (amount > (SIZE_MAX - digit) / 10U) {
                return gsh_builtin_error(io, "shift",
                                         "invalid shift count");
            }
            amount = amount * 10U + digit;
        }
    }
    if (gsh_positionals_shift(positionals, amount) == -1) {
        return gsh_builtin_error(
            io, "shift", "shift count exceeds positional parameters");
    }
    return 0;
}
