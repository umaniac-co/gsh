#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh command cache tests require the POSIX.1-2024 baseline"
#endif

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h> /* CANON-INCLUDE: linux */
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/command_cache.h"

static int make_path(char output[PATH_MAX], const char *directory,
                     const char *leaf)
{
    if (directory == NULL || leaf == NULL || output == NULL) {
        return -1;
    }
    int length = snprintf(output, PATH_MAX, "%s/%s", directory, leaf);

    return length < 0 || length >= PATH_MAX ? -1 : 0;
}

static int prepare_fixture(char root[PATH_MAX], char first[PATH_MAX],
                           char second[PATH_MAX], char first_probe[PATH_MAX],
                           char second_probe[PATH_MAX],
                           char search_path[GSH_COMMAND_PATH_CAP])
{
    if (search_path == NULL) {
        return -1;
    }
    int length;

    if (snprintf(root, PATH_MAX, "%s", "/tmp/gsh-command-cache-XXXXXX") <
            0 ||
        mkdtemp(root) == NULL || make_path(first, root, "first") == -1 ||
        make_path(second, root, "second") == -1 ||
        mkdir(first, 0700) == -1 || mkdir(second, 0700) == -1 ||
        make_path(first_probe, first, "probe") == -1 ||
        make_path(second_probe, second, "probe") == -1 ||
        symlink("/bin/sh", second_probe) == -1) {
        return -1;
    }
    length = snprintf(search_path, GSH_COMMAND_PATH_CAP, "%s:%s", first,
                      second);
    return length < 0 || length >= GSH_COMMAND_PATH_CAP ? -1 : 0;
}

static void clean_fixture(const char *root, const char *first,
                          const char *second, const char *first_probe,
                          const char *second_probe)
{
    if (first == NULL || first_probe == NULL || root == NULL || second == NULL || second_probe == NULL) {
        return;
    }
    (void)unlink(first_probe);
    (void)unlink(second_probe);
    (void)rmdir(first);
    (void)rmdir(second);
    (void)rmdir(root);
}

static int finish_fixture(const char *root, const char *first,
                          const char *second, const char *first_probe,
                          const char *second_probe, bool failed)
{
    if (root == NULL) return -1;
    if (first_probe == NULL || second == NULL || second_probe == NULL) {
        return -1;
    }
    size_t index;

    if (root[0] == '/') {
        for (index = 0; index <= GSH_COMMAND_CACHE_CAP; index++) {
            char name[16];
            char candidate[PATH_MAX];

            if (snprintf(name, sizeof(name), "tool%03zu", index) > 0 &&
                make_path(candidate, first, name) == 0) {
                (void)unlink(candidate);
            }
        }
        clean_fixture(root, first, second, first_probe, second_probe);
    }
    if (failed) {
        (void)fputs("command cache: invariant test failed\n", stderr);
        return 1;
    }
    (void)puts("command cache: bounded lookup, invalidation, and failover passed");
    return 0;
}

int main(void)
{
    static gsh_command_cache cache_storage;
    static gsh_command_cache scratch_storage;
    gsh_command_cache *cache = &cache_storage;
    gsh_command_cache *scratch = &scratch_storage;
    char root[PATH_MAX] = {0};
    char first[PATH_MAX] = {0};
    char second[PATH_MAX] = {0};
    char first_probe[PATH_MAX] = {0};
    char second_probe[PATH_MAX] = {0};
    char search_path[GSH_COMMAND_PATH_CAP];
    char resolved[GSH_COMMAND_PATH_CAP];
    bool changed = false;
    bool failed = false;
    size_t index;

    if (prepare_fixture(root, first, second, first_probe, second_probe,
                        search_path) == -1) {
        perror("command cache fixture");
        return finish_fixture(root, first, second, first_probe,
                              second_probe, true);
    }
    gsh_command_cache_initialize(cache, 7U);
    errno = 0;
    if (gsh_command_cache_resolve(cache, 7U, NULL, search_path, true,
                                  &changed, resolved) != -1 ||
        errno != EINVAL || !gsh_command_cache_validate(cache) ||
        gsh_command_cache_resolve(cache, 7U, "probe", search_path, true,
                                  &changed, resolved) != 1 ||
        !changed || strcmp(resolved, second_probe) != 0 ||
        gsh_command_cache_count(cache) != 1U ||
        strcmp(gsh_command_cache_lookup(cache, 7U, "probe"),
               second_probe) != 0 ||
        !gsh_command_cache_validate(cache)) {
        return finish_fixture(root, first, second, first_probe,
                              second_probe, true);
    }
    changed = true;
    if (symlink("/bin/sh", first_probe) == -1 ||
        gsh_command_cache_resolve(cache, 7U, "probe", search_path, true,
                                  &changed, resolved) != 1 ||
        changed || strcmp(resolved, second_probe) != 0) {
        return finish_fixture(root, first, second, first_probe,
                              second_probe, true);
    }
    if (unlink(second_probe) == -1) {
        return finish_fixture(root, first, second, first_probe,
                              second_probe, true);
    }
    changed = false;
    if (gsh_command_cache_resolve(cache, 7U, "probe", search_path, true,
                                  &changed, resolved) != 1 ||
        !changed || strcmp(resolved, first_probe) != 0 ||
        !gsh_command_cache_validate(cache)) {
        return finish_fixture(root, first, second, first_probe,
                              second_probe, true);
    }
    (void)memcpy(scratch, cache, sizeof(*scratch));
    gsh_command_cache_rebind(scratch, 7U, 11U);
    if (gsh_command_cache_lookup(scratch, 11U, "probe") == NULL ||
        !gsh_command_cache_validate(scratch)) {
        return finish_fixture(root, first, second, first_probe,
                              second_probe, true);
    }
    gsh_command_cache_rebind(scratch, 12U, 13U);
    if (gsh_command_cache_count(scratch) != 0U ||
        !gsh_command_cache_validate(scratch) ||
        !gsh_command_cache_sync(cache, 8U) ||
        gsh_command_cache_count(cache) != 0U ||
        gsh_command_cache_sync(cache, 8U)) {
        return finish_fixture(root, first, second, first_probe,
                              second_probe, true);
    }
    for (index = 0; index <= GSH_COMMAND_CACHE_CAP; index++) {
        char name[16];
        char candidate[PATH_MAX];
        int length = snprintf(name, sizeof(name), "tool%03zu", index);
        int result;

        if (length < 0 || (size_t)length >= sizeof(name) ||
            make_path(candidate, first, name) == -1 ||
            symlink("/bin/sh", candidate) == -1) {
            failed = true;
            break;
        }
        errno = 0;
        result = gsh_command_cache_resolve(
            cache, 8U, name, search_path, true, &changed, resolved);
        if ((index < GSH_COMMAND_CACHE_CAP && result != 1) ||
            (index == GSH_COMMAND_CACHE_CAP &&
             (result != -1 || errno != ENOSPC))) {
            failed = true;
            break;
        }
    }
    if (!failed && (gsh_command_cache_count(cache) !=
                        GSH_COMMAND_CACHE_CAP ||
                    !gsh_command_cache_validate(cache))) {
        failed = true;
    }

    return finish_fixture(root, first, second, first_probe, second_probe,
                          failed);
}
