#ifndef GSH_SHELL_OPTIONS_H
#define GSH_SHELL_OPTIONS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_OPTION_FLAG_CAP = 16,
    GSH_OPTION_NOCLOBBER = 1U << 0,
    GSH_OPTION_NOGLOB = 1U << 1,
    GSH_OPTION_ALLEXPORT = 1U << 2,
    GSH_OPTION_NOUNSET = 1U << 3,
    GSH_OPTION_INTERACTIVE = 1U << 30,
};

typedef struct {
    uint32_t enabled;
} gsh_shell_options;

void gsh_options_initialize(gsh_shell_options *options, bool interactive);
bool gsh_options_enabled(const gsh_shell_options *options,
                         unsigned int option);
bool gsh_options_validate(const gsh_shell_options *options);
int gsh_options_update_letter(gsh_shell_options *options, char letter,
                              bool enabled);
int gsh_options_update_name(gsh_shell_options *options, const char *name,
                            bool enabled);
void gsh_options_flags(const gsh_shell_options *options,
                       char output[GSH_OPTION_FLAG_CAP]);
size_t gsh_options_count(void);
const char *gsh_options_name(size_t index);
bool gsh_options_index_enabled(const gsh_shell_options *options,
                               size_t index);

#endif
