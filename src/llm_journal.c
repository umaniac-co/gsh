#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "llm_journal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h> /* CANON-INCLUDE: linux */
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
    JOURNAL_HEADER_CAP = 32,
    JOURNAL_RECORD_CAP = 65536,
    JOURNAL_PROMPT_CAP = 1024,
    JOURNAL_FILE_CAP = 4 * 1024 * 1024,
    JOURNAL_HOME_CAP = 4096,
    JOURNAL_CONTEXT_SLOT_CAP = 8192,
    JOURNAL_CONTEXT_SLOT_COUNT = 10,
    JOURNAL_SEARCH_SLOT_CAP = 2048,
    JOURNAL_SEARCH_SLOT_COUNT = 20,
};

typedef struct {
    unsigned int kind;
    uint32_t length;
    uint64_t sequence;
    uint32_t checksum;
} journal_header;

/* ── A Private Framed Journal Makes Context Recoverable ─────────
 * Plain concatenated transcripts cannot distinguish commands from model or
 * tool output and a torn write can corrupt every later entry. Fixed versioned
 * headers bound each record and carry a checksum, while owner-only paths keep
 * retained terminal context local. Readers stop at the first invalid frame;
 * rotation bounds the active prompt history without rewriting live records.
 * ─────────────────────────────────────────────────────────────── */

static uint32_t checksum_bytes(const char *text, size_t length)
{
    uint32_t value = 2166136261U;
    size_t index;

    if (text == NULL) return 0U;
    for (index = 0U; index < length && index < JOURNAL_RECORD_CAP; index++) {
        value ^= (unsigned char)text[index];
        value *= 16777619U;
    }
    return index == length ? value : 0U;
}

static void encode_u32(unsigned char *output, uint32_t value)
{
    unsigned int index;

    if (output == NULL) return;
    for (index = 0U; index < 4U; index++)
        output[index] = (unsigned char)(value >> (index * 8U));
}

static void encode_u64(unsigned char *output, uint64_t value)
{
    unsigned int index;

    if (output == NULL) return;
    for (index = 0U; index < 8U; index++)
        output[index] = (unsigned char)(value >> (index * 8U));
}

static uint32_t decode_u32(const unsigned char *input)
{
    uint32_t value = 0U;
    unsigned int index;

    if (input == NULL) return 0U;
    for (index = 0U; index < 4U; index++)
        value |= (uint32_t)input[index] << (index * 8U);
    return value;
}

static uint64_t decode_u64(const unsigned char *input)
{
    uint64_t value = 0U;
    unsigned int index;

    if (input == NULL) return 0U;
    for (index = 0U; index < 8U; index++)
        value |= (uint64_t)input[index] << (index * 8U);
    return value;
}

static uint64_t journal_sequence(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_REALTIME, &now) == -1) return 1U;
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static int journal_paths(const char *home, char *directory,
                         size_t directory_capacity, char *path,
                         size_t path_capacity)
{
    int directory_length;
    int path_length;

    if (home == NULL || home[0] != '/' || directory == NULL || path == NULL)
        return -1;
    directory_length = snprintf(directory, directory_capacity,
                                "%s/.genshell", home);
    path_length = snprintf(path, path_capacity, "%s/journal", directory);
    if (directory_length < 0 || (size_t)directory_length >= directory_capacity ||
        path_length < 0 || (size_t)path_length >= path_capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static bool secure_file(int descriptor)
{
    struct stat status;

    return descriptor >= 0 && fstat(descriptor, &status) == 0 &&
           S_ISREG(status.st_mode) && status.st_uid == geteuid() &&
           (status.st_mode & 0077) == 0 && status.st_nlink == 1;
}

static int read_all(int descriptor, char *data, size_t length)
{
    size_t offset = 0U;

    if (descriptor < 0 || data == NULL) return -1;
    while (offset < length) {
        ssize_t count = read(descriptor, data + offset, length - offset);

        if (count > 0) offset += (size_t)count;
        else if (count == 0) return offset == 0U ? 1 : -1;
        else if (errno != EINTR) return -1;
    }
    return 0;
}

static int write_all(int descriptor, const char *data, size_t length)
{
    size_t offset = 0U;

    if (descriptor < 0 || data == NULL) return -1;
    while (offset < length) {
        ssize_t count = write(descriptor, data + offset, length - offset);

        if (count > 0) offset += (size_t)count;
        else if (count == -1 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static void encode_header(unsigned char output[JOURNAL_HEADER_CAP],
                          unsigned int kind, size_t length,
                          uint64_t sequence, uint32_t checksum)
{
    if (output == NULL) return;
    (void)memset(output, 0, JOURNAL_HEADER_CAP);
    (void)memcpy(output, "GSHJ", 4U);
    output[4] = 1U;
    output[5] = (unsigned char)kind;
    encode_u32(output + 8U, (uint32_t)length);
    encode_u64(output + 12U, sequence);
    encode_u64(output + 20U, (uint64_t)time(NULL));
    encode_u32(output + 28U, checksum);
}

static bool decode_header(const unsigned char input[JOURNAL_HEADER_CAP],
                          journal_header *header)
{
    if (input == NULL || header == NULL || memcmp(input, "GSHJ", 4U) != 0 ||
        input[4] != 1U || input[5] < GSH_LLM_JOURNAL_COMMAND ||
        input[5] > GSH_LLM_JOURNAL_TOOL) return false;
    header->kind = input[5];
    header->length = decode_u32(input + 8U);
    header->sequence = decode_u64(input + 12U);
    header->checksum = decode_u32(input + 28U);
    return header->length <= JOURNAL_RECORD_CAP;
}

static int open_journal(const char *home, int flags)
{
    char directory[4096];
    char path[4096];
    int descriptor;

    if (journal_paths(home, directory, sizeof(directory), path,
                      sizeof(path)) == -1) return -1;
    if ((flags & O_CREAT) != 0 && mkdir(directory, 0700) == -1 &&
        errno != EEXIST) return -1;
    descriptor = open(path, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0 && !secure_file(descriptor)) {
        (void)close(descriptor);
        errno = EPERM;
        return -1;
    }
    return descriptor;
}

static int open_journal_lock(const char *home)
{
    char directory[4096];
    char path[4096];
    char lock_path[4096];
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    int descriptor;
    int length;

    if (journal_paths(home, directory, sizeof(directory), path,
                      sizeof(path)) == -1 ||
        (mkdir(directory, 0700) == -1 && errno != EEXIST)) return -1;
    length = snprintf(lock_path, sizeof(lock_path), "%s.lock", path);
    if (length < 0 || (size_t)length >= sizeof(lock_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                      0600);
    if (descriptor < 0 || !secure_file(descriptor)) {
        if (descriptor >= 0) (void)close(descriptor);
        errno = EPERM;
        return -1;
    }
    while (fcntl(descriptor, F_SETLKW, &lock) == -1) {
        if (errno != EINTR) {
            (void)close(descriptor);
            return -1;
        }
    }
    return descriptor;
}

static int next_record(int descriptor, journal_header *header, char *text,
                       size_t capacity)
{
    unsigned char encoded[JOURNAL_HEADER_CAP];
    int status;

    if (header == NULL || text == NULL || capacity == 0U) return -1;
    status = read_all(descriptor, (char *)encoded, sizeof(encoded));
    if (status != 0) return status;
    if (!decode_header(encoded, header) || header->length >= capacity) return -1;
    status = read_all(descriptor, text, header->length);
    if (status != 0 || checksum_bytes(text, header->length) != header->checksum)
        return -1;
    text[header->length] = '\0';
    return 0;
}

/* ── Every Accepted Record Must Fit The Rotation Reader ─────────
 * A 4 KiB scan buffer stopped at larger valid output records, hiding every
 * later prompt from rotation. The scanner now shares the format's full
 * payload bound and rotates before either the byte or record limit is hit.
 * A torn tail also rotates, so new records never remain behind corrupt data.
 * All scanning and durability work belongs to isolated writer processes.
 * ─────────────────────────────────────────────────────────────── */
static bool journal_needs_rotation(const char *home, size_t length)
{
    char text[JOURNAL_RECORD_CAP + 1U];
    journal_header header;
    struct stat info;
    size_t prompts = 0U;
    int descriptor = open_journal(home, O_RDONLY);
    size_t records;
    bool rotate = false;

    if (descriptor < 0) return false;
    if (fstat(descriptor, &info) == -1 || info.st_size < 0 ||
        (uintmax_t)info.st_size + JOURNAL_HEADER_CAP + length > JOURNAL_FILE_CAP)
        rotate = true;
    for (records = 0U; records < 8192U; records++) {
        if (rotate) break;
        int status = next_record(descriptor, &header, text, sizeof(text));

        if (status != 0) { rotate = status < 0; break; }
        if (header.kind == GSH_LLM_JOURNAL_PROMPT) prompts++;
        if (prompts >= JOURNAL_PROMPT_CAP) break;
    }
    (void)close(descriptor);
    return rotate || prompts >= JOURNAL_PROMPT_CAP || records == 8192U;
}

static int rotate_journal(const char *home)
{
    char directory[4096];
    char path[4096];
    char previous[4096];
    int length;

    if (journal_paths(home, directory, sizeof(directory), path,
                      sizeof(path)) == -1) return -1;
    length = snprintf(previous, sizeof(previous), "%s.previous", path);
    if (length < 0 || (size_t)length >= sizeof(previous)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (unlink(previous) == -1 && errno != ENOENT) return -1;
    if (rename(path, previous) == -1 && errno != ENOENT) return -1;
    return 0;
}

int gsh_llm_journal_append(const char *home, unsigned int kind,
                           const char *text, size_t length)
{
    unsigned char header[JOURNAL_HEADER_CAP];
    int lock_descriptor;
    int descriptor;
    int result;

    if (home == NULL || text == NULL || length > JOURNAL_RECORD_CAP ||
        kind < GSH_LLM_JOURNAL_COMMAND || kind > GSH_LLM_JOURNAL_TOOL) {
        errno = EINVAL;
        return -1;
    }
    lock_descriptor = open_journal_lock(home);
    if (lock_descriptor < 0) return -1;
    if (journal_needs_rotation(home, length) &&
        rotate_journal(home) == -1) {
        (void)close(lock_descriptor);
        return -1;
    }
    descriptor = open_journal(home, O_WRONLY | O_APPEND | O_CREAT);
    if (descriptor < 0) {
        (void)close(lock_descriptor);
        return -1;
    }
    encode_header(header, kind, length, journal_sequence(),
                  checksum_bytes(text, length));
    result = write_all(descriptor, (const char *)header, sizeof(header));
    if (result == 0) result = write_all(descriptor, text, length);
    if (result == 0 && fsync(descriptor) == -1) result = -1;
    if (close(descriptor) == -1) result = -1;
    if (close(lock_descriptor) == -1) result = -1;
    return result;
}

/* ── The Reactor Queues Bytes And Never Waits For Journal Storage ─
 * Contended advisory locks and fsync once ran in the editor's owner thread.
 * A persistent child now receives ordered, bounded frames through a pipe.
 * The owner retains at most 256 KiB, writes at most 8 KiB per reactor turn,
 * and reports saturation without delaying shell execution or normal history.
 * Each frame carries its HOME so later variable changes cannot reroute it.
 * ─────────────────────────────────────────────────────────────── */
int gsh_llm_journal_enqueue(gsh_llm_journal_queue *queue, const char *home,
                            unsigned int kind, const char *text, size_t length)
{
    size_t home_length;
    size_t frame_length;
    unsigned char header[12];

    if (queue == NULL || queue->descriptor < 0 || home == NULL || text == NULL ||
        queue->sent > queue->used || queue->used > sizeof(queue->pending) ||
        kind < GSH_LLM_JOURNAL_COMMAND || kind > GSH_LLM_JOURNAL_TOOL ||
        length > JOURNAL_RECORD_CAP || home[0] != '/') return -1;
    home_length = strnlen(home, JOURNAL_HOME_CAP);
    if (home_length >= JOURNAL_HOME_CAP) return -1;
    frame_length = sizeof(header) + home_length + 1U + length;
    if (frame_length > sizeof(queue->pending) - (queue->used - queue->sent)) {
        errno = ENOBUFS;
        return -1;
    }
    if (frame_length > sizeof(queue->pending) - queue->used) {
        (void)memmove(queue->pending, queue->pending + queue->sent,
                       queue->used - queue->sent);
        queue->used -= queue->sent;
        queue->sent = 0U;
    }
    encode_u32(header, kind);
    encode_u32(header + 4U, (uint32_t)home_length + 1U);
    encode_u32(header + 8U, (uint32_t)length);
    (void)memcpy(queue->pending + queue->used, header, sizeof(header));
    queue->used += sizeof(header);
    (void)memcpy(queue->pending + queue->used, home, home_length + 1U);
    queue->used += home_length + 1U;
    (void)memcpy(queue->pending + queue->used, text, length);
    queue->used += length;
    return 0;
}

int gsh_llm_journal_flush(gsh_llm_journal_queue *queue)
{
    size_t length;
    ssize_t written;

    if (queue == NULL || queue->descriptor < 0 || queue->sent > queue->used ||
        queue->used > sizeof(queue->pending)) return -1;
    length = queue->used - queue->sent;
    if (length == 0U) return 0;
    if (length > 8192U) length = 8192U;
    written = write(queue->descriptor, queue->pending + queue->sent, length);
    if (written < 0 && (errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK)) return 0;
    if (written <= 0) return -1;
    queue->sent += (size_t)written;
    if (queue->sent == queue->used) queue->sent = queue->used = 0U;
    return 0;
}

int gsh_llm_journal_worker(int descriptor)
{
    unsigned char header[12];
    char home[JOURNAL_HOME_CAP];
    char text[JOURNAL_RECORD_CAP];
    bool connected = descriptor >= 0;

    /* Lifecycle loop: EOF or any invalid frame terminates this isolated
     * writer; each iteration consumes one bounded record and one append. */
    while (connected) {
        int status = read_all(descriptor, (char *)header, sizeof(header));
        size_t home_length = status == 0 ? decode_u32(header + 4U) : 0U;
        size_t length = status == 0 ? decode_u32(header + 8U) : 0U;

        if (status == 1) return 0;
        if (status != 0 || home_length < 2U || home_length > sizeof(home) ||
            length > sizeof(text)) return 1;
        if (read_all(descriptor, home, home_length) != 0 ||
            home[home_length - 1U] != '\0' || home[0] != '/' ||
            memchr(home, '\0', home_length - 1U) != NULL ||
            read_all(descriptor, text, length) != 0 ||
            gsh_llm_journal_append(home, decode_u32(header), text, length) == -1)
            return 1;
    }
    return 1;
}

static const char *kind_label(unsigned int kind)
{
    if (kind == GSH_LLM_JOURNAL_COMMAND) return "command";
    if (kind == GSH_LLM_JOURNAL_COMMAND_OUTPUT) return "output";
    if (kind == GSH_LLM_JOURNAL_PROMPT) return "user";
    if (kind == GSH_LLM_JOURNAL_ANSWER) return "assistant";
    return "tool";
}

static bool append_labeled(char *output, size_t capacity, size_t *used,
                           unsigned int kind, const char *text, size_t length)
{
    int prefix;
    size_t retained;

    if (output == NULL || used == NULL || text == NULL || *used >= capacity)
        return false;
    prefix = snprintf(output + *used, capacity - *used, "[%s] ",
                      kind_label(kind));
    if (prefix < 0 || (size_t)prefix >= capacity - *used ||
        capacity - *used - (size_t)prefix < 2U) {
        output[*used] = '\0';
        return false;
    }
    *used += (size_t)prefix;
    retained = length < capacity - *used - 2U ? length : capacity - *used - 2U;
    (void)memcpy(output + *used, text, retained);
    *used += retained;
    output[(*used)++] = '\n';
    output[*used] = '\0';
    return retained == length;
}

int gsh_llm_journal_context(const char *home, size_t exchanges,
                            char *output, size_t capacity)
{
    static char slots[JOURNAL_CONTEXT_SLOT_COUNT][JOURNAL_CONTEXT_SLOT_CAP];
    unsigned int kinds[JOURNAL_CONTEXT_SLOT_COUNT] = {0U};
    size_t lengths[JOURNAL_CONTEXT_SLOT_COUNT] = {0U};
    journal_header header;
    char record[JOURNAL_RECORD_CAP + 1U];
    size_t count = 0U;
    size_t retained;
    size_t used = 0U;
    int descriptor;
    size_t scans;

    if (output == NULL || capacity == 0U || exchanges > 5U) return -1;
    output[0] = '\0';
    descriptor = open_journal(home, O_RDONLY);
    if (descriptor < 0) return errno == ENOENT ? 0 : -1;
    for (scans = 0U; scans < 8192U; scans++) {
        int status = next_record(descriptor, &header, record, sizeof(record));
        size_t slot;

        if (status != 0) break;
        if (header.kind != GSH_LLM_JOURNAL_PROMPT &&
            header.kind != GSH_LLM_JOURNAL_ANSWER) continue;
        slot = count % JOURNAL_CONTEXT_SLOT_COUNT;
        lengths[slot] = header.length < JOURNAL_CONTEXT_SLOT_CAP - 1U
                            ? header.length : JOURNAL_CONTEXT_SLOT_CAP - 1U;
        (void)memcpy(slots[slot], record, lengths[slot]);
        slots[slot][lengths[slot]] = '\0';
        kinds[slot] = header.kind;
        count++;
    }
    (void)close(descriptor);
    retained = count < exchanges * 2U ? count : exchanges * 2U;
    for (size_t offset = retained; offset > 0U; offset--) {
        size_t absolute = count - offset;
        size_t slot = absolute % JOURNAL_CONTEXT_SLOT_COUNT;
        (void)append_labeled(output, capacity, &used, kinds[slot], slots[slot],
                             lengths[slot]);
    }
    return (int)used;
}

static unsigned char ascii_lower(unsigned char byte)
{
    return byte >= 'A' && byte <= 'Z' ? (unsigned char)(byte + 32U) : byte;
}

static bool ascii_contains(const char *text, size_t length, const char *query)
{
    size_t query_length;
    size_t begin;

    if (text == NULL || query == NULL) return false;
    query_length = strlen(query);
    if (query_length == 0U || query_length > length) return false;
    for (begin = 0U; begin + query_length <= length; begin++) {
        size_t index;
        for (index = 0U; index < query_length; index++) {
            if (ascii_lower((unsigned char)text[begin + index]) !=
                ascii_lower((unsigned char)query[index])) break;
        }
        if (index == query_length) return true;
    }
    return false;
}

int gsh_llm_journal_search(const char *home, const char *query, size_t limit,
                           char *output, size_t capacity)
{
    static char slots[JOURNAL_SEARCH_SLOT_COUNT][JOURNAL_SEARCH_SLOT_CAP];
    unsigned int kinds[JOURNAL_SEARCH_SLOT_COUNT] = {0U};
    size_t lengths[JOURNAL_SEARCH_SLOT_COUNT] = {0U};
    journal_header header;
    char record[JOURNAL_RECORD_CAP + 1U];
    size_t matches = 0U;
    size_t retained;
    size_t used = 0U;
    int descriptor;
    size_t scans;

    if (output == NULL || capacity == 0U || query == NULL || limit == 0U ||
        limit > JOURNAL_SEARCH_SLOT_COUNT) return -1;
    output[0] = '\0';
    descriptor = open_journal(home, O_RDONLY);
    if (descriptor < 0) return errno == ENOENT ? 0 : -1;
    for (scans = 0U; scans < 8192U; scans++) {
        int status = next_record(descriptor, &header, record, sizeof(record));
        size_t slot;

        if (status != 0) break;
        if (!ascii_contains(record, header.length, query)) continue;
        slot = matches % JOURNAL_SEARCH_SLOT_COUNT;
        lengths[slot] = header.length < JOURNAL_SEARCH_SLOT_CAP - 1U
                            ? header.length : JOURNAL_SEARCH_SLOT_CAP - 1U;
        (void)memcpy(slots[slot], record, lengths[slot]);
        slots[slot][lengths[slot]] = '\0';
        kinds[slot] = header.kind;
        matches++;
    }
    (void)close(descriptor);
    retained = matches < limit ? matches : limit;
    for (size_t offset = 0U; offset < retained; offset++) {
        size_t absolute = matches - offset - 1U;
        size_t slot = absolute % JOURNAL_SEARCH_SLOT_COUNT;
        (void)append_labeled(output, capacity, &used, kinds[slot], slots[slot],
                             lengths[slot]);
    }
    return (int)used;
}
