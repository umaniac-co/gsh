#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "../src/builtin_files.h"

#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h> /* CANON-INCLUDE: linux */
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { CAPTURE_CAP = 32768, SPILL_ENTRIES = 400 };

static int make_file(const char *path, const char *contents)
{
    int descriptor;
    size_t length;
    if (path == NULL || contents == NULL) return -1;
    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) return -1;
    length = strlen(contents);
    if (write(descriptor, contents, length) != (ssize_t)length) {
        (void)close(descriptor);
        return -1;
    }
    return close(descriptor);
}

static int capture_builtin(gsh_file_builtin_kind kind, size_t argc,
                           char *const argv[], char output[CAPTURE_CAP],
                           size_t *length)
{
    int descriptors[2];
    gsh_builtin_io io;
    int status;
    ssize_t count;
    if (argv == NULL || output == NULL || length == NULL) return -1;
    if (pipe(descriptors) == -1) return -1;
    io = (gsh_builtin_io){.kind = GSH_BUILTIN_SINK_DESCRIPTORS,
                         .descriptors = {descriptors[1], descriptors[1]}};
    status = gsh_builtin_run_files(kind, argc, argv, &io);
    (void)close(descriptors[1]);
    count = read(descriptors[0], output, CAPTURE_CAP - 1U);
    (void)close(descriptors[0]);
    if (count < 0) return -1;
    *length = (size_t)count;
    output[*length] = '\0';
    return status;
}

static int listing_cases(void)
{
    char output[CAPTURE_CAP];
    size_t length;
    char *ls[] = {(char *)"ls", (char *)"-A1", NULL};
    char *ll[] = {(char *)"ll", NULL};
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 2U, ls, output, &length) != 0 ||
        strstr(output, ".hidden\n") == NULL || strstr(output, ".\n") != NULL ||
        strstr(output, "..\n") != NULL) return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LL, 1U, ll, output, &length) != 0 ||
        strstr(output, ".hidden") != NULL || strstr(output, "file2") == NULL ||
        strstr(output, "file10") == NULL ||
        strstr(output, "file2") > strstr(output, "file10")) return 1;
    return 0;
}

static int format_precedence_cases(void)
{
    char output[CAPTURE_CAP];
    size_t length;
    char *columns_last[] = {
        (char *)"ls", (char *)"-lC", (char *)"file2", NULL};
    char *long_last[] = {
        (char *)"ls", (char *)"-Cl", (char *)"file2", NULL};
    char *one_after_long[] = {
        (char *)"ls", (char *)"-l1", (char *)"file2", NULL};
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, columns_last, output,
                        &length) != 0 || strcmp(output, "file2\n") != 0)
        return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, long_last, output,
                        &length) != 0 || output[0] != '-') return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, one_after_long, output,
                        &length) != 0 || output[0] != '-') return 1;
    return 0;
}

static int classification_cases(void)
{
    char output[CAPTURE_CAP];
    size_t length;
    char *slash_last[] = {(char *)"ls", (char *)"-1dFp",
                          (char *)"executable", (char *)"subdir",
                          (char *)"dlink", NULL};
    char *classify_last[] = {(char *)"ls", (char *)"-1dpF",
                             (char *)"executable", (char *)"subdir",
                             (char *)"dlink", NULL};
    char *default_link[] = {
        (char *)"ls", (char *)"-1", (char *)"dlink", NULL};
    char *classify_link[] = {
        (char *)"ls", (char *)"-1F", (char *)"dlink", NULL};
    char *follow_link[] = {
        (char *)"ls", (char *)"-1H", (char *)"dlink", NULL};
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 5U, slash_last, output,
                        &length) != 0 || strstr(output, "dlink\n") == NULL ||
        strstr(output, "executable\n") == NULL ||
        strstr(output, "subdir/\n") == NULL ||
        strchr(output, '*') != NULL || strchr(output, '@') != NULL) return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 5U, classify_last, output,
                        &length) != 0 || strstr(output, "dlink@\n") == NULL ||
        strstr(output, "executable*\n") == NULL ||
        strstr(output, "subdir/\n") == NULL) return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, default_link, output,
                        &length) != 0 || strstr(output, "inside\n") == NULL)
        return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, classify_link, output,
                        &length) != 0 || strcmp(output, "dlink@\n") != 0)
        return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, follow_link, output,
                        &length) != 0 || strstr(output, "inside\n") == NULL)
        return 1;
    return 0;
}

static int ordering_cases(void)
{
    char output[CAPTURE_CAP];
    char first[CAPTURE_CAP];
    size_t length;
    size_t first_length;
    char *operands[] = {(char *)"ls", (char *)"-1", (char *)"subdir",
                        (char *)"file2", NULL};
    char *size[] = {(char *)"ls", (char *)"-1S", (char *)"ctime_old",
                    (char *)"ctime_new", NULL};
    char *ctime_last[] = {(char *)"ls", (char *)"-1tSc",
                          (char *)"ctime_old", (char *)"ctime_new", NULL};
    char *size_last[] = {(char *)"ls", (char *)"-1tcS",
                         (char *)"ctime_old", (char *)"ctime_new", NULL};
    char *plain_f[] = {(char *)"ls", (char *)"-1f", NULL};
    char *ignored_sort[] = {(char *)"ls", (char *)"-1frSt", NULL};
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 4U, operands, output,
                        &length) != 0 || strncmp(output, "file2\n\nsubdir:\n",
                                                 15U) != 0) return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 4U, size, output, &length) != 0 ||
        strncmp(output, "ctime_old\n", 10U) != 0) return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 4U, ctime_last, output,
                        &length) != 0 || strncmp(output, "ctime_new\n", 10U) != 0)
        return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 4U, size_last, output,
                        &length) != 0 || strncmp(output, "ctime_old\n", 10U) != 0)
        return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 2U, plain_f, first,
                        &first_length) != 0 || strstr(first, ".hidden\n") == NULL)
        return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 2U, ignored_sort, output,
                        &length) != 0 || length != first_length ||
        memcmp(output, first, length) != 0) return 1;
    return 0;
}

static int quoting_and_recursion_cases(bool invalid_created)
{
    char output[CAPTURE_CAP];
    size_t length;
    char *quoted[] = {(char *)"ls", (char *)"-1q", NULL};
    char *recursive[] = {
        (char *)"ls", (char *)"-1R", (char *)"subdir", NULL};
    char *loop[] = {
        (char *)"ls", (char *)"-1RL", (char *)"subdir", NULL};
    char *aliases[] = {
        (char *)"ls", (char *)"-1RL", (char *)"siblings", NULL};
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 2U, quoted, output,
                        &length) != 0 || strstr(output, "line?break\n") == NULL)
        return 1;
    if (invalid_created && strstr(output, "byte_?\n") == NULL) return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, recursive, output,
                        &length) != 0 || strstr(output, "subdir/deep:\n") == NULL ||
        strstr(output, "leaf\n") == NULL) return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, loop, output,
                        &length) != 1 || strstr(output, "Too many levels") == NULL)
        return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, aliases, output,
                        &length) != 0 || strstr(output, "siblings/a:\n") == NULL ||
        strstr(output, "siblings/b:\n") == NULL) return 1;
    return 0;
}

static int ll_layout_cases(void)
{
    char output[CAPTURE_CAP];
    size_t length;
    char *all[] = {(char *)"ll", (char *)"-a", NULL};
    char *plain[] = {(char *)"ll", NULL};
    if (capture_builtin(GSH_FILE_BUILTIN_LL, 2U, all, output, &length) != 0 ||
        strstr(output, ".hidden") == NULL) return 1;
    if (setenv("COLUMNS", "60", 1) == -1) return 1;
    if (capture_builtin(GSH_FILE_BUILTIN_LL, 1U, plain, output, &length) != 0 ||
        strstr(output, " FILE ") != NULL || strstr(output, "-rw") == NULL) {
        (void)unsetenv("COLUMNS");
        return 1;
    }
    if (setenv("COLUMNS", "72", 1) == -1 ||
        capture_builtin(GSH_FILE_BUILTIN_LL, 1U, plain, output, &length) != 0 ||
        strstr(output, "FILE") == NULL) {
        (void)unsetenv("COLUMNS");
        return 1;
    }
    return unsetenv("COLUMNS") == 0 ? 0 : 1;
}

static int spill_sort_case(void)
{
    char output[CAPTURE_CAP];
    char path[64];
    size_t created = 0U;
    size_t length;
    bool failed = false;
    char *listing[] = {(char *)"ls", (char *)"-1", (char *)"spill", NULL};
    if (mkdir("spill", 0700) == -1) return 1;
    while (created < SPILL_ENTRIES && !failed) {
        int count = snprintf(path, sizeof(path), "spill/item%03zu", created);
        if (count < 0 || (size_t)count >= sizeof(path) ||
            make_file(path, "x") == -1) failed = true;
        else created++;
    }
    if (!failed &&
        (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, listing, output,
                         &length) != 0 ||
         strncmp(output, "item000\n", 8U) != 0 ||
         strstr(output, "item383\nitem384\n") == NULL ||
         strstr(output, "item398\nitem399\n") == NULL)) failed = true;
    while (created > 0U) {
        int count;
        created--;
        count = snprintf(path, sizeof(path), "spill/item%03zu", created);
        if (count < 0 || (size_t)count >= sizeof(path) || unlink(path) == -1)
            failed = true;
    }
    if (rmdir("spill") == -1) failed = true;
    return failed ? 1 : 0;
}

static int view_cases(void)
{
    char output[CAPTURE_CAP];
    size_t length;
    char *view[] = {(char *)"view", (char *)"sample.py:2:3", NULL};
    char *directory[] = {(char *)"view", (char *)".", NULL};
    if (capture_builtin(GSH_FILE_BUILTIN_VIEW, 2U, view, output, &length) != 0 ||
        strcmp(output, "one\ntwo\n") != 0) return 1;
    return capture_builtin(GSH_FILE_BUILTIN_VIEW, 2U, directory, output,
                           &length) == 1 ? 0 : 1;
}

int main(void)
{
    char fixture[] = "/tmp/gsh-files-test-XXXXXX";
    char original[4096];
    char invalid_name[] = {'b', 'y', 't', 'e', '_', (char)0xff, '\0'};
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 20000000L};
    bool invalid_created;
    int failed = 0;
    (void)setlocale(LC_ALL, "C");
    if (getcwd(original, sizeof(original)) == NULL || mkdtemp(fixture) == NULL ||
        chdir(fixture) == -1 || make_file("file2", "2") == -1 ||
        make_file("file10", "10") == -1 || make_file(".hidden", "h") == -1 ||
        make_file("sample.py", "one\ntwo\n") == -1 ||
        make_file("executable", "#!/bin/sh\n") == -1 ||
        chmod("executable", 0700) == -1 || mkdir("subdir", 0700) == -1 ||
        mkdir("subdir/deep", 0700) == -1 ||
        mkdir("siblings", 0700) == -1 ||
        mkdir("siblings/target", 0700) == -1 ||
        make_file("subdir/inside", "inside") == -1 ||
        make_file("subdir/deep/leaf", "leaf") == -1 ||
        make_file("siblings/target/leaf", "leaf") == -1 ||
        symlink("subdir", "dlink") == -1 ||
        symlink("..", "subdir/deep/back") == -1 ||
        symlink("target", "siblings/a") == -1 ||
        symlink("target", "siblings/b") == -1 ||
        make_file("ctime_old", "a large old file") == -1 ||
        nanosleep(&pause, NULL) == -1 || make_file("ctime_new", "n") == -1 ||
        make_file("line\nbreak", "q") == -1) return 1;
    invalid_created = make_file(invalid_name, "q") == 0;
    if (listing_cases() != 0 || format_precedence_cases() != 0 ||
        classification_cases() != 0 || ordering_cases() != 0 ||
        quoting_and_recursion_cases(invalid_created) != 0 ||
        ll_layout_cases() != 0 ||
        spill_sort_case() != 0 ||
        view_cases() != 0) failed = 1;
    (void)unlink("file2");
    (void)unlink("file10");
    (void)unlink(".hidden");
    (void)unlink("sample.py");
    (void)unlink("executable");
    (void)unlink("dlink");
    (void)unlink("ctime_old");
    (void)unlink("ctime_new");
    (void)unlink("line\nbreak");
    if (invalid_created) (void)unlink(invalid_name);
    (void)unlink("subdir/inside");
    (void)unlink("subdir/deep/leaf");
    (void)unlink("subdir/deep/back");
    (void)unlink("siblings/a");
    (void)unlink("siblings/b");
    (void)unlink("siblings/target/leaf");
    (void)rmdir("siblings/target");
    (void)rmdir("siblings");
    (void)rmdir("subdir/deep");
    (void)rmdir("subdir");
    (void)chdir(original);
    (void)rmdir(fixture);
    if (failed) { (void)fputs("file builtins: failed\n", stderr); return 1; }
    (void)puts("file builtins: native ls, ll, and view cases passed");
    return 0;
}
