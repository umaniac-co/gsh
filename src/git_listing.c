#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "git_listing.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h> /* CANON-INCLUDE: linux */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h> /* CANON-INCLUDE: linux */
#include <sys/stat.h>
#include <sys/wait.h> /* CANON-INCLUDE: linux */
#include <unistd.h>

#define require(condition) (condition)

enum {
    GSH_GIT_DIRECTORY_CAP = 256,
    GSH_GIT_IO_CAP = 1024,
    GSH_GIT_RECORD_CAP = PATH_MAX + 512,
    GSH_GIT_OUTPUT_CAP = 64 * 1024 * 1024,
    GSH_GIT_SIZE_CAP = 65536,
};

static void reader_reset(gsh_git_reader *reader)
{
    if (!require(reader != NULL)) return;
    if (!require(reader->descriptor >= -1)) return;
    if (reader->descriptor >= 0) (void)close(reader->descriptor);
    (void)memset(reader, 0, sizeof(*reader));
    reader->descriptor = -1;
}

static int temporary_file(void)
{
    char pattern[] = "/tmp/gsh-git-listing-XXXXXX";
    int descriptor = mkstemp(pattern);

    if (!require(descriptor >= -1)) return -1;
    if (descriptor >= 0) {
        (void)unlink(pattern);
        (void)fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    }
    return descriptor;
}

static int copy_path(char destination[PATH_MAX], const char *source)
{
    size_t length;
    if (!require(destination != NULL)) return -1;
    if (!require(source != NULL)) return -1;
    length = strlen(source);
    if (length >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    (void)memcpy(destination, source, length + 1U);
    return 0;
}

static bool git_marker(const char *directory)
{
    char marker[PATH_MAX];
    struct stat status;
    int length;

    if (!require(directory != NULL)) return false;
    if (!require(directory[0] == '/')) return false;
    length = snprintf(marker, sizeof(marker), "%s%s.git", directory,
                      strcmp(directory, "/") == 0 ? "" : "/");
    if (length < 0 || (size_t)length >= sizeof(marker) ||
        lstat(marker, &status) == -1) return false;
    return S_ISDIR(status.st_mode) || S_ISREG(status.st_mode);
}

static bool parent_directory(char path[PATH_MAX])
{
    size_t length;
    size_t index;

    if (!require(path != NULL)) return false;
    if (!require(path[0] == '/')) return false;
    length = strlen(path);
    if (length <= 1U) return false;
    for (index = length; index > 1U; index--) {
        if (path[index - 1U] == '/') {
            path[index - 1U] = '\0';
            return true;
        }
    }
    path[1] = '\0';
    return true;
}

static int snapshot_prefix(gsh_git_snapshot *snapshot,
                           const char *resolved)
{
    size_t root_length;
    const char *relative;

    if (!require(snapshot != NULL && resolved != NULL)) return -1;
    if (!require(snapshot->root[0] == '/' && resolved[0] == '/')) return -1;
    root_length = strlen(snapshot->root);
    relative = resolved + root_length;
    if (strcmp(snapshot->root, "/") != 0 && relative[0] == '/') relative++;
    return copy_path(snapshot->prefix, relative);
}

static int find_repository(gsh_git_snapshot *snapshot,
                           const char *directory)
{
    char resolved[PATH_MAX];
    char current[PATH_MAX];
    struct stat status;
    size_t depth;

    if (!require(snapshot != NULL && directory != NULL)) return 0;
    if (!require(directory[0] != '\0')) return 0;
    if (realpath(directory, resolved) == NULL ||
        stat(resolved, &status) == -1 || !S_ISDIR(status.st_mode) ||
        copy_path(current, resolved) == -1) return 0;
    for (depth = 0U; depth < GSH_GIT_DIRECTORY_CAP; depth++) {
        if (git_marker(current)) {
            if (copy_path(snapshot->root, current) == -1 ||
                snapshot_prefix(snapshot, resolved) == -1) return 0;
            return 1;
        }
        if (!parent_directory(current)) return 0;
    }
    errno = EOVERFLOW;
    return 0;
}

static int child_redirect(int descriptor)
{
    struct rlimit limit;
    int null_descriptor;

    if (!require(descriptor >= 0)) return -1;
    limit.rlim_cur = (rlim_t)GSH_GIT_OUTPUT_CAP;
    limit.rlim_max = (rlim_t)GSH_GIT_OUTPUT_CAP;
    null_descriptor = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (null_descriptor < 0 || setrlimit(RLIMIT_FSIZE, &limit) == -1 ||
        dup2(descriptor, STDOUT_FILENO) == -1 ||
        dup2(null_descriptor, STDERR_FILENO) == -1) {
        if (null_descriptor >= 0) (void)close(null_descriptor);
        return -1;
    }
    (void)close(null_descriptor);
    if (descriptor != STDOUT_FILENO) (void)close(descriptor);
    return 0;
}

static int wait_git(pid_t process)
{
    int status = 0;
    size_t turn;

    if (!require(process > 0)) return -1;
    for (turn = 0U; turn < GSH_GIT_IO_CAP; turn++) {
        pid_t result = waitpid(process, &status, 0);
        if (result == process) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        }
        if (result == -1 && errno == EINTR) continue;
        return -1;
    }
    errno = EINTR;
    return -1;
}

/* ── Git Is a Bounded Metadata Provider ──────────────────────────
 * The file builtins remain the owners of traversal, sorting, and output.
 * Reimplementing Git's index, packed objects, worktrees, and ignore engine
 * would create a second incompatible repository implementation inside gsh.
 * One direct Git process therefore produces each NUL-delimited snapshot.
 * A file-size limit and fixed parser bounds turn provider failure into the
 * ordinary un-enriched listing instead of changing filesystem semantics.
 * ─────────────────────────────────────────────────────────────── */
static int run_git(gsh_git_snapshot *snapshot, bool status_command,
                   gsh_git_reader *reader)
{
    char *arguments[18];
    const char *pathspec;
    size_t count = 0U;
    int descriptor;
    pid_t process;

    if (!require(snapshot != NULL && reader != NULL)) return -1;
    if (!require(snapshot->root[0] == '/')) return -1;
    descriptor = temporary_file();
    if (descriptor < 0) return -1;
    pathspec = snapshot->prefix[0] == '\0' ? "." : snapshot->prefix;
    arguments[count++] = (char *)"git";
    arguments[count++] = (char *)"--no-optional-locks";
    arguments[count++] = (char *)"-c";
    arguments[count++] = (char *)"color.status=false";
    arguments[count++] = (char *)"-C";
    arguments[count++] = snapshot->root;
    arguments[count++] = status_command ? (char *)"status" : (char *)"ls-files";
    if (status_command) {
        arguments[count++] = (char *)"--porcelain=v1";
        arguments[count++] = (char *)"-z";
        arguments[count++] = (char *)"-b";
        arguments[count++] = (char *)"--renames";
        arguments[count++] = (char *)"--ignored=matching";
        arguments[count++] = (char *)"--untracked-files=all";
    } else {
        arguments[count++] = (char *)"-z";
        arguments[count++] = (char *)"--cached";
        arguments[count++] =
            (char *)"--format=%(objectmode) %(objectname) %(stage)%x09%(path)";
    }
    arguments[count++] = (char *)"--";
    arguments[count++] = (char *)pathspec;
    arguments[count] = NULL;
    process = fork();
    if (process == 0) {
        if (child_redirect(descriptor) == -1) _exit(125);
        execvp(arguments[0], arguments); /* C-PROCESS-ABI */
        _exit(127);
    }
    if (process < 0 || wait_git(process) == -1 ||
        lseek(descriptor, 0, SEEK_SET) == (off_t)-1) {
        (void)close(descriptor);
        return -1;
    }
    reader_reset(reader);
    reader->descriptor = descriptor;
    return 0;
}

static int run_git_sizes(gsh_git_snapshot *snapshot)
{
    char *arguments[10];
    int descriptor;
    pid_t process;

    if (!require(snapshot != NULL && snapshot->size_input >= 0)) return 0;
    if (!require(snapshot->root[0] == '/')) return 0;
    descriptor = temporary_file();
    if (descriptor < 0 || lseek(snapshot->size_input, 0, SEEK_SET) == (off_t)-1) {
        if (descriptor >= 0) (void)close(descriptor);
        return 0;
    }
    arguments[0] = (char *)"git";
    arguments[1] = (char *)"--no-optional-locks";
    arguments[2] = (char *)"--no-lazy-fetch";
    arguments[3] = (char *)"-C";
    arguments[4] = snapshot->root;
    arguments[5] = (char *)"cat-file";
    arguments[6] = (char *)"--batch-check=%(objectsize)";
    arguments[7] = NULL;
    process = fork();
    if (process == 0) {
        if (dup2(snapshot->size_input, STDIN_FILENO) == -1 ||
            child_redirect(descriptor) == -1) _exit(125);
        execvp(arguments[0], arguments); /* C-PROCESS-ABI */
        _exit(127);
    }
    if (process < 0 || wait_git(process) == -1 ||
        lseek(descriptor, 0, SEEK_SET) == (off_t)-1) {
        (void)close(descriptor);
        return 0;
    }
    reader_reset(&snapshot->sizes);
    snapshot->sizes.descriptor = descriptor;
    return 1;
}

static int reader_fill(gsh_git_reader *reader)
{
    size_t turn;
    if (!require(reader != NULL && reader->descriptor >= 0)) return -1;
    if (!require(reader->begin <= reader->end)) return -1;
    for (turn = 0U; turn < GSH_GIT_IO_CAP; turn++) {
        ssize_t amount = read(reader->descriptor, reader->bytes,
                              sizeof(reader->bytes));
        if (amount > 0) {
            reader->begin = 0U;
            reader->end = (size_t)amount;
            return 1;
        }
        if (amount == 0) { reader->eof = true; return 0; }
        if (errno != EINTR) return -1;
    }
    errno = EINTR;
    return -1;
}

static int reader_byte(gsh_git_reader *reader, unsigned char *byte)
{
    int filled;
    if (!require(reader != NULL && byte != NULL)) return -1;
    if (!require(reader->begin <= reader->end)) return -1;
    if (reader->begin == reader->end) {
        if (reader->eof) return 0;
        filled = reader_fill(reader);
        if (filled <= 0) return filled;
    }
    *byte = reader->bytes[reader->begin++];
    return 1;
}

static int reader_record(gsh_git_reader *reader,
                         char record[GSH_GIT_RECORD_CAP], size_t *length)
{
    size_t used = 0U;
    size_t turn;

    if (!require(reader != NULL && record != NULL)) return -1;
    if (!require(length != NULL && reader->descriptor >= 0)) return -1;
    for (turn = 0U; turn < GSH_GIT_RECORD_CAP; turn++) {
        unsigned char byte = 0U;
        int result = reader_byte(reader, &byte);
        if (result == 0 && used == 0U) return 0;
        if (result <= 0 || used + 1U >= GSH_GIT_RECORD_CAP) return -1;
        if (byte == 0U) {
            record[used] = '\0';
            *length = used;
            return 1;
        }
        record[used++] = (char)byte;
    }
    errno = EOVERFLOW;
    return -1;
}

static int reader_line(gsh_git_reader *reader,
                       char line[GSH_GIT_RECORD_CAP], size_t *length)
{
    size_t used = 0U;
    size_t turn;

    if (!require(reader != NULL && line != NULL)) return -1;
    if (!require(length != NULL && reader->descriptor >= 0)) return -1;
    for (turn = 0U; turn < GSH_GIT_RECORD_CAP; turn++) {
        unsigned char byte = 0U;
        int result = reader_byte(reader, &byte);
        if (result == 0 && used == 0U) return 0;
        if (result <= 0 || used + 1U >= GSH_GIT_RECORD_CAP) return -1;
        if (byte == '\n') {
            line[used] = '\0';
            *length = used;
            return 1;
        }
        line[used++] = (char)byte;
    }
    errno = EOVERFLOW;
    return -1;
}

static int read_small_file(const char *path, char *text, size_t capacity)
{
    int descriptor;
    size_t turn;
    if (!require(path != NULL && text != NULL)) return -1;
    if (!require(capacity > 1U)) return -1;
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return -1;
    for (turn = 0U; turn < GSH_GIT_IO_CAP; turn++) {
        ssize_t amount = read(descriptor, text, capacity - 1U);
        if (amount >= 0) {
            text[(size_t)amount] = '\0';
            (void)close(descriptor);
            return 0;
        }
        if (errno != EINTR) break;
    }
    (void)close(descriptor);
    return -1;
}

static int git_head_path(const gsh_git_snapshot *snapshot,
                         char head[PATH_MAX])
{
    char marker[PATH_MAX];
    char contents[PATH_MAX];
    struct stat status;
    int length;

    if (!require(snapshot != NULL && head != NULL)) return -1;
    if (!require(snapshot->root[0] == '/')) return -1;
    length = snprintf(marker, sizeof(marker), "%s%s.git", snapshot->root,
                      strcmp(snapshot->root, "/") == 0 ? "" : "/");
    if (length < 0 || (size_t)length >= sizeof(marker) ||
        lstat(marker, &status) == -1) return -1;
    if (S_ISDIR(status.st_mode)) {
        length = snprintf(head, PATH_MAX, "%s/HEAD", marker);
    } else if (S_ISREG(status.st_mode) &&
               read_small_file(marker, contents, sizeof(contents)) == 0 &&
               strncmp(contents, "gitdir: ", 8U) == 0) {
        size_t end = strcspn(contents + 8U, "\r\n");
        contents[8U + end] = '\0';
        length = contents[8] == '/'
                     ? snprintf(head, PATH_MAX, "%s/HEAD", contents + 8U)
                     : snprintf(head, PATH_MAX, "%s/%s/HEAD", snapshot->root,
                                contents + 8U);
    } else return -1;
    return length >= 0 && length < PATH_MAX ? 0 : -1;
}

static void detached_branch(const gsh_git_snapshot *snapshot,
                            char branch[GSH_GIT_BRANCH_CAP])
{
    char head_path[PATH_MAX];
    char head[GSH_GIT_BRANCH_CAP];
    size_t length;
    size_t index;

    if (!require(snapshot != NULL && branch != NULL)) return;
    if (git_head_path(snapshot, head_path) == -1 ||
        read_small_file(head_path, head, sizeof(head)) == -1) {
        (void)snprintf(branch, GSH_GIT_BRANCH_CAP, "detached@unknown");
        return;
    }
    length = strcspn(head, "\r\n");
    for (index = 0U; index < length; index++) {
        unsigned char byte = (unsigned char)head[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f') ||
              (byte >= 'A' && byte <= 'F'))) break;
    }
    if (index != length || length < 8U) {
        (void)snprintf(branch, GSH_GIT_BRANCH_CAP, "detached@unknown");
    } else {
        (void)snprintf(branch, GSH_GIT_BRANCH_CAP, "detached@%.8s", head);
    }
}

static int branch_name(gsh_git_snapshot *snapshot, const char *record,
                       size_t length)
{
    static const char unborn[] = "No commits yet on ";
    static const char initial[] = "Initial commit on ";
    const char *name;
    size_t available;
    size_t used;

    if (!require(snapshot != NULL && record != NULL)) return -1;
    if (!require(length == strlen(record))) return -1;
    if (length < 4U || strncmp(record, "## ", 3U) != 0) return -1;
    name = record + 3U;
    if (strncmp(name, unborn, sizeof(unborn) - 1U) == 0)
        name += sizeof(unborn) - 1U;
    else if (strncmp(name, initial, sizeof(initial) - 1U) == 0)
        name += sizeof(initial) - 1U;
    if (strncmp(name, "HEAD (no branch)", 16U) == 0) {
        detached_branch(snapshot, snapshot->branch);
        return 0;
    }
    available = strlen(name);
    used = 0U;
    while (used < available && used + 1U < sizeof(snapshot->branch) &&
           name[used] != ' ' &&
           !(name[used] == '.' && used + 2U < available &&
             name[used + 1U] == '.' && name[used + 2U] == '.')) used++;
    if (used == 0U || used >= sizeof(snapshot->branch)) return -1;
    (void)memcpy(snapshot->branch, name, used);
    snapshot->branch[used] = '\0';
    return 0;
}

static bool relative_path(const gsh_git_snapshot *snapshot,
                          const char *path, char output[PATH_MAX])
{
    const char *relative = path;
    size_t prefix_length;
    size_t length;

    if (!require(snapshot != NULL && path != NULL)) return false;
    if (!require(output != NULL)) return false;
    prefix_length = strlen(snapshot->prefix);
    if (prefix_length != 0U) {
        if (strncmp(path, snapshot->prefix, prefix_length) != 0 ||
            path[prefix_length] != '/') return false;
        relative = path + prefix_length + 1U;
    }
    length = strlen(relative);
    while (length > 0U && relative[length - 1U] == '/') length--;
    if (length == 0U || length >= PATH_MAX) return false;
    (void)memcpy(output, relative, length);
    output[length] = '\0';
    return true;
}

int gsh_git_snapshot_open(gsh_git_snapshot *snapshot,
                          const char *directory)
{
    char record[GSH_GIT_RECORD_CAP];
    size_t length = 0U;
    int present;

    if (!require(snapshot != NULL)) return 0;
    if (!require(directory != NULL)) return 0;
    (void)memset(snapshot, 0, sizeof(*snapshot));
    snapshot->status.descriptor = -1;
    snapshot->tracked.descriptor = -1;
    snapshot->sizes.descriptor = -1;
    snapshot->size_input = -1;
    present = find_repository(snapshot, directory);
    if (present != 1 || run_git(snapshot, true, &snapshot->status) == -1 ||
        reader_record(&snapshot->status, record, &length) != 1 ||
        branch_name(snapshot, record, length) == -1) {
        gsh_git_snapshot_close(snapshot);
        return 0;
    }
    if (run_git(snapshot, false, &snapshot->tracked) == -1) {
        gsh_git_snapshot_close(snapshot);
        return 0;
    }
    snapshot->active = true;
    return 1;
}

void gsh_git_snapshot_close(gsh_git_snapshot *snapshot)
{
    if (!require(snapshot != NULL)) return;
    if (!require(snapshot->status.descriptor >= -1 &&
                 snapshot->tracked.descriptor >= -1 &&
                 snapshot->sizes.descriptor >= -1)) return;
    reader_reset(&snapshot->status);
    reader_reset(&snapshot->tracked);
    reader_reset(&snapshot->sizes);
    if (snapshot->size_input >= 0) (void)close(snapshot->size_input);
    snapshot->size_input = -1;
    snapshot->size_requests = 0U;
    snapshot->active = false;
}

int gsh_git_snapshot_next_change(gsh_git_snapshot *snapshot,
                                 gsh_git_change *change)
{
    char record[GSH_GIT_RECORD_CAP];
    char original[GSH_GIT_RECORD_CAP];
    size_t length = 0U;
    size_t original_length = 0U;
    int result;

    if (!require(snapshot != NULL && change != NULL)) return -1;
    if (!require(snapshot->active && snapshot->status.descriptor >= 0)) return -1;
    result = reader_record(&snapshot->status, record, &length);
    if (result <= 0) return result;
    if (length < 4U || record[2] != ' ') return -1;
    (void)memset(change, 0, sizeof(*change));
    change->index_status = record[0] == '!' ? 'I' : record[0];
    change->worktree_status = record[1] == '!' ? 'I' : record[1];
    if (!relative_path(snapshot, record + 3U, change->path)) return -1;
    change->renamed = record[0] == 'R' || record[0] == 'C' ||
                      record[1] == 'R' || record[1] == 'C';
    if (change->renamed) {
        if (reader_record(&snapshot->status, original, &original_length) != 1)
            return -1;
        (void)original_length;
        (void)relative_path(snapshot, original, change->original);
    }
    return 1;
}

static bool hexadecimal_id(const char *text, size_t length)
{
    size_t index;
    if (!require(text != NULL)) return false;
    if (!require(length == 40U || length == 64U)) return false;
    for (index = 0U; index < length; index++) {
        unsigned char byte = (unsigned char)text[index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f') ||
              (byte >= 'A' && byte <= 'F'))) return false;
    }
    return true;
}

static int parse_tracked_metadata(const char *record, mode_t *mode,
                                  char object_id[GSH_GIT_OID_CAP])
{
    uintmax_t mode_value = 0U;
    size_t object_begin;
    size_t object_length;
    size_t index;

    if (!require(record != NULL && mode != NULL)) return -1;
    if (!require(object_id != NULL && record[0] != '\0')) return -1;
    for (index = 0U; index < 8U && record[index] != ' '; index++) {
        unsigned char byte = (unsigned char)record[index];
        if (byte < '0' || byte > '7') return -1;
        mode_value = mode_value * 8U + (uintmax_t)(byte - '0');
    }
    if (index == 0U || index >= 8U || record[index] != ' ') return -1;
    object_begin = ++index;
    while (record[index] != '\0' && record[index] != ' ' &&
           index - object_begin < GSH_GIT_OID_CAP) index++;
    object_length = index - object_begin;
    if (record[index] != ' ' ||
        !hexadecimal_id(record + object_begin, object_length)) return -1;
    *mode = (mode_t)mode_value;
    (void)memcpy(object_id, record + object_begin, object_length);
    object_id[object_length] = '\0';
    return 0;
}

int gsh_git_snapshot_next_tracked(gsh_git_snapshot *snapshot,
                                  gsh_git_tracked *tracked)
{
    char record[GSH_GIT_RECORD_CAP];
    const char *separator;
    size_t length = 0U;
    int result;

    if (!require(snapshot != NULL && tracked != NULL)) return -1;
    if (!require(snapshot->active)) return -1;
    if (snapshot->tracked.descriptor < 0) return 0;
    result = reader_record(&snapshot->tracked, record, &length);
    if (result <= 0) return result;
    separator = memchr(record, '\t', length);
    if (separator == NULL || separator + 1U >= record + length ||
        parse_tracked_metadata(record, &tracked->mode,
                               tracked->object_id) == -1 ||
        !relative_path(snapshot, separator + 1U, tracked->path)) return -1;
    return 1;
}

static int write_size_bytes(int descriptor, const char *bytes, size_t length)
{
    size_t offset = 0U;
    size_t turn;
    if (!require(descriptor >= 0 && bytes != NULL)) return -1;
    if (!require(length > 0U && length <= GSH_GIT_OID_CAP)) return -1;
    for (turn = 0U; turn < GSH_GIT_IO_CAP && offset < length; turn++) {
        ssize_t amount = write(descriptor, bytes + offset, length - offset);
        if (amount > 0) offset += (size_t)amount;
        else if (amount == -1 && errno == EINTR) continue;
        else return -1;
    }
    return offset == length ? 0 : -1;
}

int gsh_git_snapshot_request_size(gsh_git_snapshot *snapshot,
                                  const char *object_id)
{
    size_t length;
    if (!require(snapshot != NULL && object_id != NULL)) return -1;
    if (!require(snapshot->active &&
                 snapshot->size_requests <= GSH_GIT_SIZE_CAP)) return -1;
    length = strlen(object_id);
    if (!hexadecimal_id(object_id, length) ||
        snapshot->size_requests == GSH_GIT_SIZE_CAP) return -1;
    if (snapshot->size_input < 0) snapshot->size_input = temporary_file();
    if (snapshot->size_input < 0 ||
        write_size_bytes(snapshot->size_input, object_id, length) == -1 ||
        write_size_bytes(snapshot->size_input, "\n", 1U) == -1) return -1;
    snapshot->size_requests++;
    return 0;
}

int gsh_git_snapshot_begin_sizes(gsh_git_snapshot *snapshot)
{
    if (!require(snapshot != NULL)) return 0;
    if (!require(snapshot->active)) return 0;
    if (snapshot->size_requests == 0U) return 0;
    return run_git_sizes(snapshot);
}

int gsh_git_snapshot_next_size(gsh_git_snapshot *snapshot, off_t *size)
{
    char line[GSH_GIT_RECORD_CAP];
    uintmax_t value = 0U;
    size_t length = 0U;
    size_t index;
    int result;

    if (!require(snapshot != NULL && size != NULL)) return -1;
    if (!require(snapshot->active && snapshot->sizes.descriptor >= 0)) return -1;
    result = reader_line(&snapshot->sizes, line, &length);
    if (result <= 0) return result;
    if (length == 0U) return -1;
    for (index = 0U; index < length; index++) {
        unsigned char byte = (unsigned char)line[index];
        if (byte < '0' || byte > '9' ||
            value > (UINTMAX_MAX - (uintmax_t)(byte - '0')) / 10U) return -1;
        value = value * 10U + (uintmax_t)(byte - '0');
    }
    *size = (off_t)value;
    return *size >= 0 && (uintmax_t)*size == value ? 1 : -1;
}
