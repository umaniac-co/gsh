#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "builtin_unalias.h"

#include <errno.h>
#include <string.h>

int gsh_builtin_unalias(size_t argc, char *const argv[],
                        gsh_alias_store *aliases,
                        gsh_alias_journal *journal,
                        const gsh_builtin_io *io)
{
    if (aliases == NULL) {
        return -1;
    }
    size_t index = 1;
    bool all = false;
    int status = 0;

    if (argc == 0 || argv == NULL || !gsh_builtin_io_valid(io)) {
        errno = EINVAL;
        return 125;
    }
    if (index < argc && strcmp(argv[index], "-a") == 0) {
        all = true;
        index++;
    } else if (index < argc && strcmp(argv[index], "--") == 0) {
        index++;
    } else if (index < argc && argv[index][0] == '-' &&
               argv[index][1] != '\0') {
        return gsh_builtin_error(io, "unalias", "invalid option");
    }
    if (all) {
        if (index != argc) {
            return gsh_builtin_error(io, "unalias",
                                     "-a does not accept operands");
        }
        gsh_aliases_clear(aliases);
        if (journal != NULL &&
            gsh_alias_journal_record_clear(journal) == -1) {
            (void)gsh_builtin_error(
                io, "unalias", "alias transaction capacity exceeded");
            return 125;
        }
        return 0;
    }
    if (index == argc) {
        return gsh_builtin_error(io, "unalias", "missing alias name");
    }
    for (; index < argc; index++) {
        size_t length = strlen(argv[index]);

        if (!gsh_alias_name_is_valid(argv[index], length)) {
            status = gsh_builtin_error(io, "unalias",
                                       "invalid alias name");
        } else if (aliases == NULL ||
                   gsh_aliases_unset(aliases, argv[index], length) == -1) {
            status = gsh_builtin_error(io, "unalias",
                                       "alias is not defined");
        } else if (journal != NULL &&
                   gsh_alias_journal_record_unset(
                       journal, argv[index], length) == -1) {
            (void)gsh_builtin_error(
                io, "unalias", "alias transaction capacity exceeded");
            return 125;
        }
    }
    return status;
}
