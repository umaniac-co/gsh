#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "../src/builtin_files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h> /* CANON-INCLUDE: linux */
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h> /* CANON-INCLUDE: linux */
#include <termios.h>
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

static int terminal_descriptors(int *master, int *slave)
{
    const char *name;
    struct termios attributes;
    if (master == NULL || slave == NULL) return -1;
    *master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (*master < 0 || grantpt(*master) == -1 || unlockpt(*master) == -1) {
        if (*master >= 0) (void)close(*master);
        return -1;
    }
    name = ptsname(*master);
    if (name == NULL) { (void)close(*master); return -1; }
    *slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (*slave < 0) { (void)close(*master); return -1; }
    if (tcgetattr(*slave, &attributes) == 0) {
        attributes.c_oflag &= (tcflag_t)~OPOST;
        if (tcsetattr(*slave, TCSANOW, &attributes) == -1) {
            (void)close(*slave);
            (void)close(*master);
            return -1;
        }
    }
    return 0;
}

static int read_terminal(int master, char output[CAPTURE_CAP], size_t *length)
{
    size_t used = 0U;
    size_t turn;
    if (master < 0 || output == NULL || length == NULL) return -1;
    for (turn = 0U; turn < CAPTURE_CAP; turn++) {
        ssize_t amount = read(master, output + used, CAPTURE_CAP - 1U - used);
        if (amount > 0) {
            used += (size_t)amount;
            if (used == CAPTURE_CAP - 1U) break;
            continue;
        }
        if (amount == 0 || (amount == -1 && errno == EIO)) break;
        if (amount == -1 && errno == EINTR) continue;
        return -1;
    }
    output[used] = '\0';
    *length = used;
    return 0;
}

static int capture_builtin_terminal(gsh_file_builtin_kind kind, size_t argc,
                                    char *const argv[],
                                    char output[CAPTURE_CAP], size_t *length)
{
    int capture[2] = {-1, -1};
    int master = -1;
    int slave = -1;
    int saved = -1;
    int status;
    gsh_builtin_io io;

    if (argv == NULL || output == NULL || length == NULL) return -1;
    if (terminal_descriptors(&master, &slave) == -1 || pipe(capture) == -1) {
        if (slave >= 0) (void)close(slave);
        if (master >= 0) (void)close(master);
        return -1;
    }
    saved = dup(STDOUT_FILENO);
    if (saved < 0 || dup2(slave, STDOUT_FILENO) == -1) {
        if (saved >= 0) (void)close(saved);
        (void)close(capture[0]);
        (void)close(capture[1]);
        (void)close(slave);
        (void)close(master);
        return -1;
    }
    io = (gsh_builtin_io){.kind = GSH_BUILTIN_SINK_DESCRIPTORS,
                          .descriptors = {capture[1], capture[1]}};
    status = gsh_builtin_run_files(kind, argc, argv, &io);
    if (dup2(saved, STDOUT_FILENO) == -1) status = -1;
    (void)close(saved);
    (void)close(capture[1]);
    (void)close(slave);
    (void)close(master);
    if (read_terminal(capture[0], output, length) == -1) status = -1;
    (void)close(capture[0]);
    return status;
}

static void strip_terminal_styles(char text[CAPTURE_CAP])
{
    size_t source = 0U;
    size_t destination = 0U;
    if (text == NULL) return;
    while (source < CAPTURE_CAP && text[source] != '\0') {
        if ((unsigned char)text[source] == 0x1bU && text[source + 1U] == '[') {
            size_t turn;
            source += 2U;
            for (turn = 0U; turn < 32U && source < CAPTURE_CAP; turn++) {
                if (text[source++] == 'm') break;
            }
        } else if (text[source] == '\r') source++;
        else text[destination++] = text[source++];
    }
    text[destination] = '\0';
}

static int run_program(char *const argv[], int expected_status)
{
    pid_t process;
    int status = 0;
    size_t turn;
    if (argv == NULL || argv[0] == NULL || expected_status < 0) return -1;
    process = fork();
    if (process == 0) {
        int null_descriptor = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_descriptor < 0 ||
            dup2(null_descriptor, STDOUT_FILENO) == -1 ||
            dup2(null_descriptor, STDERR_FILENO) == -1) _exit(125);
        (void)close(null_descriptor);
        execvp(argv[0], argv); /* C-PROCESS-ABI */
        _exit(127);
    }
    if (process < 0) return -1;
    for (turn = 0U; turn < 1024U; turn++) {
        pid_t waited = waitpid(process, &status, 0);
        if (waited == process) {
            return WIFEXITED(status) && WEXITSTATUS(status) == expected_status
                       ? 0 : -1;
        }
        if (waited == -1 && errno == EINTR) continue;
        return -1;
    }
    return -1;
}

static bool git_size_lookup_supported(void)
{
    char *probe[] = {
        (char *)"git", (char *)"--no-lazy-fetch", (char *)"--version", NULL};
    return run_program(probe, 0) == 0;
}

static int ansi_resource_cursor_case(void)
{
    static const char first[] = "\033[31mA·\033[0m \n";
    static const char second[] = "\033[36mbranch\033[0m";
    char bytes[128];
    size_t offset = 0U;
    size_t length = 0U;
    uint64_t overloads = 0U;
    gsh_builtin_resource_sink resources = {.descriptor = -1};
    gsh_builtin_io io = {
        .kind = GSH_BUILTIN_SINK_BUFFER,
        .buffer = {.bytes = bytes, .capacity = sizeof(bytes),
                   .offset = &offset, .length = &length,
                   .overloads = &overloads}};
    io.resources = &resources;
    if (gsh_builtin_output(&io, STDOUT_FILENO, first,
                           sizeof(first) - 1U) != 0 ||
        resources.row != 1U || resources.column != 0U ||
        resources.visual_column != 0U ||
        gsh_builtin_output(&io, STDOUT_FILENO, second,
                           sizeof(second) - 1U) != 0 ||
        resources.column != sizeof(second) - 1U ||
        resources.visual_column != 6U || overloads != 0U) return 1;
    return 0;
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

static int git_baseline_files(void)
{
    if (mkdir("mixed", 0700) == -1 || mkdir("clean-dir", 0700) == -1 ||
        mkdir("ignored-dir", 0700) == -1 ||
        mkdir("tracked-container", 0700) == -1 ||
        make_file(".gitignore", "*.log\nignored-dir/\n") == -1 ||
        make_file("clean", "clean") == -1 ||
        make_file("staged", "base") == -1 ||
        make_file("unstaged", "base") == -1 ||
        make_file("both", "base") == -1 ||
        make_file("deleted", "base") == -1 ||
        make_file("staged-deleted", "base") == -1 ||
        make_file(".hidden_deleted", "base") == -1 ||
        make_file("renamed-old", "base") == -1 ||
        make_file("conflict", "base\n") == -1 ||
        make_file("clean-dir/inside", "base") == -1 ||
        make_file("ignored-dir/hidden", "ignored") == -1 ||
        make_file("tracked-container/inside", "base") == -1 ||
        make_file("tracked-container/ignored.log", "ignored") == -1 ||
        make_file("mixed/modified", "base") == -1 ||
        make_file("ignored.log", "ignored") == -1) return -1;
    return 0;
}

static int git_initialize_repository(void)
{
    char *initialize[] = {
        (char *)"git", (char *)"init", (char *)"-q", (char *)"-b",
        (char *)"main", NULL};
    char *add[] = {(char *)"git", (char *)"add", (char *)".", NULL};
    char *commit[] = {
        (char *)"git", (char *)"-c", (char *)"user.name=gsh",
        (char *)"-c", (char *)"user.email=gsh@example.test",
        (char *)"commit", (char *)"-qm", (char *)"baseline", NULL};
    if (run_program(initialize, 0) == -1 || git_baseline_files() == -1 ||
        run_program(add, 0) == -1 || run_program(commit, 0) == -1) return -1;
    return 0;
}

static int git_detached_case(void)
{
    char output[CAPTURE_CAP];
    size_t length;
    char *detach[] = {
        (char *)"git", (char *)"checkout", (char *)"-q",
        (char *)"--detach", NULL};
    char *main_branch[] = {
        (char *)"git", (char *)"checkout", (char *)"-q",
        (char *)"main", NULL};
    char *listing[] = {
        (char *)"ls", (char *)"-l", (char *)"clean", NULL};
    if (run_program(detach, 0) == -1 ||
        capture_builtin_terminal(GSH_FILE_BUILTIN_LS, 3U, listing, output,
                                 &length) != 0) return -1;
    strip_terminal_styles(output);
    if (strncmp(output, "branch: detached@", 17U) != 0 ||
        strstr(output, "detached@unknown") != NULL ||
        run_program(main_branch, 0) == -1) return -1;
    return 0;
}

static int git_conflict_fixture(void)
{
    char *branch[] = {
        (char *)"git", (char *)"branch", (char *)"side", NULL};
    char *side[] = {
        (char *)"git", (char *)"checkout", (char *)"-q",
        (char *)"side", NULL};
    char *main_branch[] = {
        (char *)"git", (char *)"checkout", (char *)"-q",
        (char *)"main", NULL};
    char *add[] = {
        (char *)"git", (char *)"add", (char *)"conflict", NULL};
    char *side_commit[] = {
        (char *)"git", (char *)"-c", (char *)"user.name=gsh", (char *)"-c",
        (char *)"user.email=gsh@example.test", (char *)"commit", (char *)"-qm",
        (char *)"side", NULL};
    char *main_commit[] = {
        (char *)"git", (char *)"-c", (char *)"user.name=gsh", (char *)"-c",
        (char *)"user.email=gsh@example.test", (char *)"commit", (char *)"-qm",
        (char *)"main", NULL};
    char *merge[] = {
        (char *)"git", (char *)"-c", (char *)"user.name=gsh", (char *)"-c",
        (char *)"user.email=gsh@example.test", (char *)"merge",
        (char *)"side", NULL};
    if (run_program(branch, 0) == -1 || run_program(side, 0) == -1 ||
        make_file("conflict", "side\n") == -1 || run_program(add, 0) == -1 ||
        run_program(side_commit, 0) == -1 ||
        run_program(main_branch, 0) == -1 ||
        make_file("conflict", "main\n") == -1 || run_program(add, 0) == -1 ||
        run_program(main_commit, 0) == -1 || run_program(merge, 1) == -1)
        return -1;
    return 0;
}

static int git_dirty_files(void)
{
    char unicode_name[] = {
        'u', 'n', 'i', 'c', 'o', 'd', 'e', '_', (char)0xc3, (char)0xa9, '\0'};
    char *add_staged[] = {
        (char *)"git", (char *)"add", (char *)"staged", NULL};
    char *add_both[] = {
        (char *)"git", (char *)"add", (char *)"both", NULL};
    char *add_deleted[] = {
        (char *)"git", (char *)"add", (char *)"-u", (char *)"--",
        (char *)"staged-deleted", NULL};
    char *rename[] = {
        (char *)"git", (char *)"mv", (char *)"--", (char *)"renamed-old",
        (char *)"renamed-new", NULL};
    char *add_mixed[] = {
        (char *)"git", (char *)"add", (char *)"mixed/added", NULL};
    if (make_file("staged", "staged") == -1 ||
        run_program(add_staged, 0) == -1 ||
        make_file("unstaged", "unstaged") == -1 ||
        make_file("both", "staged") == -1 || run_program(add_both, 0) == -1 ||
        make_file("both", "working") == -1 || unlink("deleted") == -1 ||
        unlink("staged-deleted") == -1 || run_program(add_deleted, 0) == -1 ||
        unlink(".hidden_deleted") == -1 || run_program(rename, 0) == -1 ||
        make_file("added", "new") == -1 ||
        make_file("mixed/added", "new") == -1 ||
        run_program(add_mixed, 0) == -1 ||
        make_file("mixed/modified", "changed") == -1 ||
        make_file("mixed/untracked", "new") == -1 ||
        make_file("-dash", "new") == -1 ||
        make_file("un tracked", "new") == -1 ||
        make_file(unicode_name, "new") == -1) return -1;
    {
        char *add_new[] = {
            (char *)"git", (char *)"add", (char *)"added", NULL};
        if (run_program(add_new, 0) == -1) return -1;
    }
    return 0;
}

static int git_long_listing_case(void)
{
    char output[CAPTURE_CAP];
    size_t length;
    char *listing[] = {(char *)"ls", (char *)"-lG", NULL};
    if (capture_builtin_terminal(GSH_FILE_BUILTIN_LS, 2U, listing, output,
                                 &length) != 0 ||
        strstr(output, "\033[38;5;81mbranch: main\033[0m") == NULL ||
        strstr(output, "\033[38;5;114mA·\033[0m") == NULL ||
        strstr(output, "\033[38;5;221mMM\033[0m") == NULL ||
        strstr(output, "\033[38;5;203m·D\033[0m") == NULL ||
        strstr(output, "\033[38;5;81mR·\033[0m") == NULL ||
        strstr(output, "\033[38;5;177m??\033[0m") == NULL ||
        strstr(output, "\033[2;38;5;250mII\033[0m") == NULL ||
        strstr(output, "\033[38;5;245mignored-dir\033[0m") == NULL ||
        strstr(output, "\033[38;5;245mtracked-container") != NULL ||
        strstr(output, "\033[9;38;5;203mdeleted\033[0m") == NULL) return -1;
    strip_terminal_styles(output);
    if (strncmp(output, "branch: main\n", 13U) != 0 ||
        strstr(output, "A· added") == NULL || strstr(output, "MM both") == NULL ||
        strstr(output, "✓  clean") == NULL || strstr(output, "✓  clean-dir") == NULL ||
        strstr(output, "UU conflict") == NULL ||
        strstr(output, "·D deleted") == NULL ||
        strstr(output, "II ignored.log") == NULL ||
        strstr(output, "II ignored-dir") != NULL ||
        strstr(output, "[II] tracked-container") == NULL ||
        strstr(output, "[A· ·M ??] mixed") == NULL ||
        strstr(output, "R· renamed-new") == NULL ||
        strstr(output, "M· staged") == NULL || strstr(output, "D· staged-deleted") == NULL ||
        strstr(output, "·M unstaged") == NULL || strstr(output, "?? un tracked") == NULL ||
        strstr(output, "?? -dash") == NULL ||
        strstr(output, ".hidden_deleted") != NULL) return -1;
    if (git_size_lookup_supported() &&
        strstr(output, "-rw-r--r-- — — — 4 — ·D deleted") == NULL) return -1;
    return 0;
}

static int git_ll_listing_case(void)
{
    char output[CAPTURE_CAP];
    size_t length;
    char *listing[] = {(char *)"ll", (char *)"-a", NULL};
    char *nested[] = {(char *)"ll", (char *)"mixed", NULL};
    if (capture_builtin_terminal(GSH_FILE_BUILTIN_LL, 2U, listing, output,
                                 &length) != 0 ||
        strstr(output, "\033[38;5;245mignored-dir") == NULL ||
        strstr(output, "\033[9;38;5;203mdeleted\033[0m") == NULL) return -1;
    strip_terminal_styles(output);
    if (strncmp(output, "branch: main\n", 13U) != 0 ||
        strstr(output, ".hidden_deleted") == NULL ||
        strstr(output, "[A· ·M ??]  DIR") == NULL ||
        strstr(output, "deleted") == NULL || strstr(output, "  ·D  ") == NULL)
        return -1;
    if (git_size_lookup_supported() &&
        strstr(output, "-rw-r--r--       4B  —") == NULL) return -1;
    if (capture_builtin_terminal(GSH_FILE_BUILTIN_LL, 2U, nested, output,
                                 &length) != 0) return -1;
    strip_terminal_styles(output);
    if (strncmp(output, "branch: main\n", 13U) != 0 ||
        strstr(output, "A·") == NULL || strstr(output, "·M") == NULL ||
        strstr(output, "??") == NULL) return -1;
    return 0;
}

static int git_compatibility_cases(void)
{
    char output[CAPTURE_CAP];
    size_t length;
    char *piped[] = {(char *)"ls", (char *)"-l", (char *)"clean", NULL};
    char *sorted[] = {(char *)"ls", (char *)"-latr", NULL};
    char *size_sorted[] = {(char *)"ls", (char *)"-lS", NULL};
    char *size_reversed[] = {(char *)"ls", (char *)"-lSr", NULL};
    if (capture_builtin(GSH_FILE_BUILTIN_LS, 3U, piped, output, &length) != 0 ||
        strstr(output, "branch:") != NULL || strchr(output, '\033') != NULL ||
        strstr(output, " clean\n") == NULL) return -1;
    if (capture_builtin_terminal(GSH_FILE_BUILTIN_LS, 2U, sorted, output,
                                 &length) != 0) return -1;
    strip_terminal_styles(output);
    if (strncmp(output, "branch: main\n", 13U) != 0 ||
        strstr(output, ".hidden_deleted") == NULL ||
        strstr(output, "deleted") == NULL) return -1;
    if (capture_builtin_terminal(GSH_FILE_BUILTIN_LS, 2U, size_sorted,
                                 output, &length) != 0) return -1;
    strip_terminal_styles(output);
    if (strstr(output, "staged-deleted") == NULL ||
        strstr(output, "un tracked") == NULL ||
        strstr(output, "staged-deleted") < strstr(output, "un tracked"))
        return -1;
    if (capture_builtin_terminal(GSH_FILE_BUILTIN_LS, 2U, size_reversed,
                                 output, &length) != 0) return -1;
    strip_terminal_styles(output);
    if (strstr(output, "staged-deleted") == NULL ||
        strstr(output, "un tracked") == NULL ||
        strstr(output, "staged-deleted") > strstr(output, "un tracked"))
        return -1;
    return 0;
}

static size_t count_text(const char *text, const char *needle)
{
    const char *position = text;
    size_t count = 0U;
    size_t turn;
    if (text == NULL || needle == NULL || needle[0] == '\0') return 0U;
    for (turn = 0U; turn < CAPTURE_CAP && position != NULL; turn++) {
        position = strstr(position, needle);
        if (position != NULL) { count++; position += strlen(needle); }
    }
    return count;
}

static int git_multiple_repository_case(const char *primary)
{
    char secondary[] = "/tmp/gsh-git-second-XXXXXX";
    char output[CAPTURE_CAP];
    size_t length;
    char *initialize[] = {
        (char *)"git", (char *)"init", (char *)"-q", (char *)"-b",
        (char *)"otherbranch", NULL};
    char *listing[] = {
        (char *)"ls", (char *)"-l", (char *)primary, secondary, NULL};
    char *remove[] = {(char *)"/bin/rm", (char *)"-rf", (char *)"--",
                      secondary, NULL};
    char original[PATH_MAX];
    int result = -1;
    if (primary == NULL || getcwd(original, sizeof(original)) == NULL ||
        mkdtemp(secondary) == NULL || chdir(secondary) == -1 ||
        run_program(initialize, 0) == -1 || make_file("new", "new") == -1 ||
        chdir(original) == -1) return -1;
    if (capture_builtin_terminal(GSH_FILE_BUILTIN_LS, 4U, listing, output,
                                 &length) == 0) {
        strip_terminal_styles(output);
        if (strstr(output, "branch: main") != NULL &&
            strstr(output, "branch: otherbranch") != NULL &&
            count_text(output, "branch: ") == 2U) result = 0;
    }
    if (run_program(remove, 0) == -1) result = -1;
    return result;
}

static int git_listing_cases(void)
{
    char repository[] = "/tmp/gsh-git-listing-test-XXXXXX";
    char original[PATH_MAX];
    char *remove[] = {(char *)"/bin/rm", (char *)"-rf", (char *)"--",
                      repository, NULL};
    int failed = 0;
    if (getcwd(original, sizeof(original)) == NULL ||
        mkdtemp(repository) == NULL || chdir(repository) == -1) return 1;
    if (git_initialize_repository() == -1 || git_detached_case() == -1 ||
        git_conflict_fixture() == -1 || git_dirty_files() == -1 ||
        git_long_listing_case() == -1 || git_ll_listing_case() == -1 ||
        git_compatibility_cases() == -1 ||
        git_multiple_repository_case(repository) == -1) failed = 1;
    if (chdir(original) == -1 || run_program(remove, 0) == -1) failed = 1;
    return failed;
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
    if (ansi_resource_cursor_case() != 0 || listing_cases() != 0 ||
        format_precedence_cases() != 0 ||
        classification_cases() != 0 || ordering_cases() != 0 ||
        quoting_and_recursion_cases(invalid_created) != 0 ||
        ll_layout_cases() != 0 ||
        spill_sort_case() != 0 ||
        git_listing_cases() != 0 ||
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
