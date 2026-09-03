#ifndef GSH_SHELL_INVOCATION_H
#define GSH_SHELL_INVOCATION_H

#include "shell_options.h"

#include <stdbool.h>
#include <stddef.h>

enum { GSH_INVOCATION_ARGUMENT_CAP = 256 };

typedef enum {
    GSH_INVOCATION_INTERACTIVE,
    GSH_INVOCATION_COMMAND,
    GSH_INVOCATION_FILE,
    GSH_INVOCATION_STDIN,
} gsh_invocation_mode;

typedef struct {
    gsh_shell_options options;
    gsh_invocation_mode mode;
    size_t source_index;
    size_t parameter_zero_index;
    size_t positional_index;
} gsh_invocation;

int gsh_invocation_parse(int argc, char *const argv[], bool stdin_is_tty,
                         gsh_invocation *invocation);

#endif
