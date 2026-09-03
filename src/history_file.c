#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "history_file.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define require(condition) (condition)

enum {
    HISTORY_FILE_BYTE_CAP =
        GSH_HISTORY_CAP * (2 * GSH_HISTORY_ENTRY_CAP + 64),
    HISTORY_FILE_LINE_CAP = 2 * GSH_HISTORY_ENTRY_CAP + 64,
    HISTORY_FILE_CHUNK_CAP = 4096,
    HISTORY_LOCK_ATTEMPT_CAP = 200,
    HISTORY_LOCK_DELAY_MS = 10,
    HISTORY_IO_RETRY_CAP = 16,
};

typedef struct {
    int descriptor;
    unsigned char chunk[HISTORY_FILE_CHUNK_CAP];
    size_t offset;
    size_t length;
    size_t remaining;
} history_reader;

/* ── Plain History Keeps Shell Startup Non-Interactive ────────────────
 * History follows the established shell lifecycle: read a user-only text file
 * at startup and merge this session into it at clean shutdown. A sidecar
 * advisory lock makes concurrent exits additive, while replacement keeps a
 * torn file invisible. No filesystem operation is performed in an interactive
 * reactor turn.
 * ────────────────────────────────────────────── */

static int secure_regular_file(int descriptor)
{
    struct stat status;

    if (!require(descriptor >= 0)) return -1;
    if (fstat(descriptor, &status) == -1 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 0077) != 0 ||
        status.st_nlink != 1) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int configure_paths(gsh_history_file *file, const char *home)
{
    if (!require(file != NULL)) return -1;
    if (!require(home != NULL)) return -1;
    if (home[0] != '/' ||
        snprintf(file->path, sizeof(file->path), "%s/.gsh_history", home) >=
            (int)sizeof(file->path) ||
        snprintf(file->lock_path, sizeof(file->lock_path),
                 "%s/.gsh_history.lock", home) >=
            (int)sizeof(file->lock_path)) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int acquire_lock(const gsh_history_file *file)
{
    struct flock lock;
    unsigned int attempt;
    int descriptor;

    if (!require(file != NULL)) return -1;
    descriptor = open(file->lock_path,
                      O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                      S_IRUSR | S_IWUSR);
    if (descriptor == -1 || secure_regular_file(descriptor) == -1) {
        if (descriptor >= 0) (void)close(descriptor);
        return -1;
    }
    (void)memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    for (attempt = 0; attempt < HISTORY_LOCK_ATTEMPT_CAP; attempt++) {
        if (fcntl(descriptor, F_SETLK, &lock) == 0) return descriptor;
        if (errno != EACCES && errno != EAGAIN && errno != EINTR) break;
        (void)poll(NULL, 0, HISTORY_LOCK_DELAY_MS);
    }
    (void)close(descriptor);
    errno = EBUSY;
    return -1;
}

static int release_lock(int descriptor)
{
    struct flock lock;
    int result = 0;

    if (!require(descriptor >= 0)) return -1;
    (void)memset(&lock, 0, sizeof(lock));
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(descriptor, F_SETLK, &lock) == -1) result = -1;
    if (close(descriptor) == -1) result = -1;
    return result;
}

static int open_history(const gsh_history_file *file, off_t *start,
                        size_t *length)
{
    struct stat status;
    int descriptor;

    if (!require(file != NULL && start != NULL)) return -1;
    if (!require(length != NULL)) return -1;
    descriptor = open(file->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor == -1) return errno == ENOENT ? -2 : -1;
    if (secure_regular_file(descriptor) == -1 ||
        fstat(descriptor, &status) == -1 || status.st_size < 0) {
        (void)close(descriptor);
        return -1;
    }
    *start = status.st_size > (off_t)HISTORY_FILE_BYTE_CAP
                 ? status.st_size - (off_t)HISTORY_FILE_BYTE_CAP
                 : 0;
    *length = (size_t)(status.st_size - *start);
    if (lseek(descriptor, *start, SEEK_SET) == (off_t)-1) {
        (void)close(descriptor);
        return -1;
    }
    return descriptor;
}

static int fill_reader(history_reader *reader)
{
    unsigned int attempt;

    if (!require(reader != NULL)) return -1;
    if (!require(reader->descriptor >= 0)) return -1;
    if (reader->remaining == 0U) return 0;
    for (attempt = 0; attempt < HISTORY_IO_RETRY_CAP; attempt++) {
        size_t requested = reader->remaining < sizeof(reader->chunk)
                               ? reader->remaining
                               : sizeof(reader->chunk);
        ssize_t count = read(reader->descriptor, reader->chunk, requested);

        if (count > 0) {
            reader->offset = 0U;
            reader->length = (size_t)count;
            reader->remaining -= (size_t)count;
            return 1;
        }
        if (count == 0) {
            reader->remaining = 0U;
            return 0;
        }
        if (errno != EINTR) return -1;
    }
    errno = EINTR;
    return -1;
}

static int next_byte(history_reader *reader, unsigned char *byte)
{
    int filled;

    if (!require(reader != NULL && byte != NULL)) return -1;
    if (!require(reader->offset <= reader->length)) return -1;
    if (reader->offset == reader->length) {
        filled = fill_reader(reader);
        if (filled <= 0) return filled;
    }
    *byte = reader->chunk[reader->offset++];
    return 1;
}

static size_t extended_payload(const char *line, size_t length)
{
    size_t index = 2U;
    size_t step;

    if (!require(line != NULL)) return SIZE_MAX;
    if (length < 7U || line[0] != ':' || line[1] != ' ') return SIZE_MAX;
    for (step = 0U; step < HISTORY_FILE_LINE_CAP && index < length &&
                    line[index] >= '0' && line[index] <= '9'; step++) {
        index++;
    }
    if (index == 2U || index >= length || line[index++] != ':')
        return SIZE_MAX;
    {
        size_t elapsed = index;

        for (step = 0U; step < HISTORY_FILE_LINE_CAP && index < length &&
                        line[index] >= '0' && line[index] <= '9'; step++) {
            index++;
        }
        if (index == elapsed || index >= length || line[index++] != ';')
            return SIZE_MAX;
    }
    return index;
}

static int decode_entry(const char *line, size_t length, size_t payload,
                        char output[GSH_HISTORY_ENTRY_CAP], size_t *used)
{
    size_t index;
    size_t output_length = 0U;

    if (!require(line != NULL && output != NULL)) return -1;
    if (!require(used != NULL && payload <= length)) return -1;
    for (index = payload; index < length && index < HISTORY_FILE_LINE_CAP;
         index++) {
        unsigned char byte = (unsigned char)line[index];

        if (byte == '\\' && index + 1U < length && line[index + 1U] == 'n') {
            byte = '\n';
            index++;
        } else if (byte == '\\' && index + 1U < length &&
                   line[index + 1U] == 'r') {
            byte = '\r';
            index++;
        } else if (byte == '\\' && index + 1U < length &&
                   line[index + 1U] == '\\') {
            index++;
        }
        if (output_length + 1U >= GSH_HISTORY_ENTRY_CAP) {
            errno = EOVERFLOW;
            return -1;
        }
        output[output_length++] = (char)byte;
    }
    output[output_length] = '\0';
    *used = output_length;
    return index == length ? 0 : -1;
}

static void accept_line(gsh_history_store *history, const char *line,
                        size_t length, bool overflow)
{
    char decoded[GSH_HISTORY_ENTRY_CAP];
    size_t payload;
    size_t used;

    if (!require(history != NULL && line != NULL)) return;
    if (overflow || length == 0U) return;
    if (line[length - 1U] == '\r') length--;
    if (length == 0U) return;
    payload = extended_payload(line, length);
    if (payload != SIZE_MAX) {
        if (decode_entry(line, length, payload, decoded, &used) == 0 &&
            used != 0U) {
            (void)gsh_history_add(history, decoded, used, false);
        }
    } else if (length < GSH_HISTORY_ENTRY_CAP) {
        (void)gsh_history_add(history, line, length, false);
    }
}

static int load_history(const gsh_history_file *file,
                        gsh_history_store *history)
{
    char line[HISTORY_FILE_LINE_CAP];
    off_t start;
    size_t length;
    size_t line_length = 0U;
    size_t step;
    int descriptor;
    bool discard;
    bool overflow = false;

    if (!require(file != NULL && history != NULL)) return -1;
    descriptor = open_history(file, &start, &length);
    if (descriptor == -2) return 0;
    if (descriptor == -1) return -1;
    history_reader reader = {.descriptor = descriptor, .remaining = length};
    discard = start != 0;
    for (step = 0U; step <= HISTORY_FILE_BYTE_CAP; step++) {
        unsigned char byte;
        int available = next_byte(&reader, &byte);

        if (available < 0) {
            (void)close(descriptor);
            return -1;
        }
        if (available == 0) {
            if (!discard) accept_line(history, line, line_length, overflow);
            return close(descriptor);
        }
        if (byte == '\n') {
            if (!discard) accept_line(history, line, line_length, overflow);
            discard = false;
            overflow = false;
            line_length = 0U;
        } else if (!discard && !overflow && line_length < sizeof(line)) {
            line[line_length++] = (char)byte;
        } else if (!discard) {
            overflow = true;
        }
    }
    (void)close(descriptor);
    errno = EFBIG;
    return -1;
}

static int write_all(int descriptor, const char *buffer, size_t length)
{
    size_t offset = 0U;
    size_t attempt;

    if (!require(descriptor >= 0 && buffer != NULL)) return -1;
    if (!require(length <= HISTORY_FILE_LINE_CAP)) return -1;
    for (attempt = 0U; attempt < HISTORY_FILE_LINE_CAP && offset < length;
         attempt++) {
        ssize_t count = write(descriptor, buffer + offset, length - offset);

        if (count > 0) offset += (size_t)count;
        else if (count != -1 || errno != EINTR) return -1;
    }
    if (offset != length) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int write_entry(int descriptor, const char *command, size_t length,
                       unsigned long long timestamp)
{
    char line[HISTORY_FILE_LINE_CAP];
    size_t used;
    size_t index;
    int header;

    if (!require(descriptor >= 0 && command != NULL)) return -1;
    if (!require(length > 0U && length < GSH_HISTORY_ENTRY_CAP)) return -1;
    header = snprintf(line, sizeof(line), ": %llu:0;", timestamp);
    if (header < 0 || (size_t)header >= sizeof(line)) return -1;
    used = (size_t)header;
    for (index = 0U; index < GSH_HISTORY_ENTRY_CAP && index < length;
         index++) {
        unsigned char byte = (unsigned char)command[index];

        if (byte == '\\' || byte == '\n' || byte == '\r') {
            if (used + 2U >= sizeof(line)) return -1;
            line[used++] = '\\';
            line[used++] = byte == '\n' ? 'n' : byte == '\r' ? 'r' : '\\';
        } else {
            if (used + 1U >= sizeof(line)) return -1;
            line[used++] = (char)byte;
        }
    }
    if (index != length || used + 1U > sizeof(line)) return -1;
    line[used++] = '\n';
    return write_all(descriptor, line, used);
}

static int write_store(int descriptor, const gsh_history_store *history,
                       size_t max_entries)
{
    time_t now;
    unsigned long long timestamp;
    size_t retained;
    size_t first;
    size_t index;

    if (!require(descriptor >= 0 && history != NULL)) return -1;
    if (!require(max_entries > 0U && max_entries <= GSH_HISTORY_CAP)) return -1;
    retained = history->count < max_entries ? history->count : max_entries;
    first = history->count - retained;
    now = time(NULL);
    timestamp = now > (time_t)0 ? (unsigned long long)now : 0U;
    for (index = 0U; index < GSH_HISTORY_CAP && index < retained; index++) {
        uint64_t event = gsh_history_oldest_event(history) + first + index;
        size_t length;
        const char *entry = gsh_history_event(history, event, &length);

        if (entry == NULL ||
            write_entry(descriptor, entry, length, timestamp) == -1) return -1;
    }
    return index == retained ? 0 : -1;
}

static int replace_history(const gsh_history_file *file,
                           const gsh_history_store *history,
                           size_t max_entries)
{
    char temporary[4096];
    struct timespec now;
    long nanoseconds = 0;
    int descriptor;
    int result = 0;

    if (!require(file != NULL && history != NULL)) return -1;
    if (clock_gettime(CLOCK_REALTIME, &now) == 0) nanoseconds = now.tv_nsec;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld.%ld", file->path,
                 (long)getpid(), nanoseconds) >= (int)sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                     O_NOFOLLOW,
                      S_IRUSR | S_IWUSR);
    if (descriptor == -1) return -1;
    if (secure_regular_file(descriptor) == -1 ||
        write_store(descriptor, history, max_entries) == -1 ||
        fsync(descriptor) == -1) result = -1;
    if (close(descriptor) == -1) result = -1;
    if (result == 0 && rename(temporary, file->path) == -1) result = -1;
    if (result == -1) (void)unlink(temporary);
    return result;
}

static int merge_session(gsh_history_store *persisted,
                         const gsh_history_store *session, bool deduplicate)
{
    uint64_t oldest;
    size_t index;

    if (!require(persisted != NULL && session != NULL)) return -1;
    if (!require(session->count <= GSH_HISTORY_CAP)) return -1;
    oldest = gsh_history_oldest_event(session);
    for (index = 0U; index < GSH_HISTORY_CAP && index < session->count;
         index++) {
        size_t length;
        const char *entry = gsh_history_event(session, oldest + index,
                                              &length);

        if (entry == NULL ||
            gsh_history_add(persisted, entry, length, deduplicate) == -1)
            return -1;
    }
    return index == session->count ? 0 : -1;
}

int gsh_history_file_initialize(gsh_history_file *file, const char *home,
                                gsh_history_store *history)
{
    int lock_descriptor;
    int result;

    if (!require(file != NULL && home != NULL)) return -1;
    if (!require(history != NULL)) return -1;
    (void)memset(file, 0, sizeof(*file));
    if (configure_paths(file, home) == -1) return -1;
    lock_descriptor = acquire_lock(file);
    if (lock_descriptor == -1) return -1;
    result = load_history(file, history);
    if (release_lock(lock_descriptor) == -1) result = -1;
    file->ready = result == 0;
    return result;
}

int gsh_history_file_save(gsh_history_file *file,
                          gsh_history_store *persisted,
                          const gsh_history_store *session,
                          size_t max_entries, bool deduplicate)
{
    int lock_descriptor;
    int result;

    if (!require(file != NULL && persisted != NULL)) return -1;
    if (!require(session != NULL && file->ready)) return -1;
    if (session->count == 0U) return 0;
    lock_descriptor = acquire_lock(file);
    if (lock_descriptor == -1) return -1;
    gsh_history_initialize(persisted);
    result = load_history(file, persisted);
    if (result == 0) result = merge_session(persisted, session, deduplicate);
    if (result == 0) result = replace_history(file, persisted, max_entries);
    if (release_lock(lock_descriptor) == -1) result = -1;
    return result;
}

void gsh_history_file_close(gsh_history_file *file)
{
    if (!require(file != NULL)) return;
    if (!require(file->path[sizeof(file->path) - 1U] == '\0')) return;
    file->ready = false;
}
