#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh tests require the POSIX.1-2024 feature-test baseline"
#endif

#include "../src/positional_parameters.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    gsh_positional_store store;
    gsh_positional_store before;
    char *values[GSH_POSITIONAL_CAP];
    char *initial[] = {(char *)"a", (char *)"b c", (char *)""};
    char oversized[GSH_POSITIONAL_TEXT_CAP + 1];

    gsh_positionals_initialize(&store);
    if (!gsh_positionals_validate(&store) ||
        gsh_positionals_assign(&store, 3, initial) == -1 ||
        gsh_positionals_count(&store) != 3) {
        return 1;
    }
    gsh_positionals_view(&store, values);
    if (strcmp(values[0], "a") != 0 || strcmp(values[1], "b c") != 0 ||
        strcmp(values[2], "") != 0 ||
        gsh_positionals_shift(&store, 1) == -1) {
        return 1;
    }
    gsh_positionals_view(&store, values);
    if (gsh_positionals_count(&store) != 2 ||
        strcmp(values[0], "b c") != 0 || strcmp(values[1], "") != 0) {
        return 1;
    }
    before = store;
    if (gsh_positionals_shift(&store, 3) != -1 || errno != EINVAL ||
        memcmp(&store, &before, sizeof(store)) != 0) {
        return 1;
    }
    memset(oversized, 'x', GSH_POSITIONAL_TEXT_CAP);
    oversized[GSH_POSITIONAL_TEXT_CAP] = '\0';
    {
        char *too_large[] = {oversized};

        if (gsh_positionals_assign(&store, 1, too_large) != -1 ||
            errno != E2BIG || memcmp(&store, &before, sizeof(store)) != 0) {
            return 1;
        }
    }
    if (gsh_positionals_shift(&store, 2) == -1 ||
        gsh_positionals_count(&store) != 0 ||
        !gsh_positionals_validate(&store)) {
        return 1;
    }
    store.version++;
    if (gsh_positionals_validate(&store)) {
        return 1;
    }
    puts("positionals: atomic arena and O(1) shift passed");
    return 0;
}
