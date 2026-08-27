#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "shell_options.h"

#include <errno.h>
#include <string.h>

typedef struct {
    const char *name;
    unsigned int bit;
    char letter;
} option_descriptor;

static const option_descriptor descriptors[] = {
    {"allexport", GSH_OPTION_ALLEXPORT, 'a'},
    {"noclobber", GSH_OPTION_NOCLOBBER, 'C'},
    {"noglob", GSH_OPTION_NOGLOB, 'f'},
    {"nounset", GSH_OPTION_NOUNSET, 'u'},
};

void gsh_options_initialize(gsh_shell_options *options, bool interactive)
{
    options->enabled = interactive ? GSH_OPTION_INTERACTIVE : 0U;
}

bool gsh_options_enabled(const gsh_shell_options *options,
                         unsigned int option)
{
    return options != NULL && (options->enabled & option) != 0U;
}

bool gsh_options_validate(const gsh_shell_options *options)
{
    const unsigned int known = GSH_OPTION_NOCLOBBER | GSH_OPTION_NOGLOB |
                               GSH_OPTION_ALLEXPORT | GSH_OPTION_NOUNSET |
                               GSH_OPTION_INTERACTIVE;

    return options != NULL && (options->enabled & ~known) == 0U;
}

static int update(gsh_shell_options *options,
                  const option_descriptor *descriptor, bool enabled)
{
    if (enabled) {
        options->enabled |= descriptor->bit;
    } else {
        options->enabled &= ~descriptor->bit;
    }
    return 0;
}

int gsh_options_update_letter(gsh_shell_options *options, char letter,
                              bool enabled)
{
    size_t index;

    for (index = 0; index < gsh_options_count(); index++) {
        if (descriptors[index].letter == letter) {
            return update(options, &descriptors[index], enabled);
        }
    }
    errno = EINVAL;
    return -1;
}

int gsh_options_update_name(gsh_shell_options *options, const char *name,
                            bool enabled)
{
    size_t index;

    for (index = 0; index < gsh_options_count(); index++) {
        if (strcmp(descriptors[index].name, name) == 0) {
            return update(options, &descriptors[index], enabled);
        }
    }
    errno = EINVAL;
    return -1;
}

void gsh_options_flags(const gsh_shell_options *options,
                       char output[GSH_OPTION_FLAG_CAP])
{
    static const char order[] = "abCefhimnuvx";
    size_t source;
    size_t used = 0;

    for (source = 0; source < sizeof(order) - 1U; source++) {
        size_t option;

        if (order[source] == 'i' &&
            gsh_options_enabled(options, GSH_OPTION_INTERACTIVE)) {
            output[used++] = 'i';
        }
        for (option = 0; option < gsh_options_count(); option++) {
            if (descriptors[option].letter == order[source] &&
                gsh_options_index_enabled(options, option)) {
                output[used++] = order[source];
            }
        }
    }
    output[used] = '\0';
}

size_t gsh_options_count(void)
{
    return sizeof(descriptors) / sizeof(descriptors[0]);
}

const char *gsh_options_name(size_t index)
{
    return index < gsh_options_count() ? descriptors[index].name : NULL;
}

bool gsh_options_index_enabled(const gsh_shell_options *options,
                               size_t index)
{
    return index < gsh_options_count() &&
           gsh_options_enabled(options, descriptors[index].bit);
}
