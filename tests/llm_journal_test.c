#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "../src/llm_journal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h> /* CANON-INCLUDE: macos */
#include <stdio.h>
#include <stdlib.h> /* CANON-INCLUDE: linux */
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h> /* CANON-INCLUDE: linux */
#include <unistd.h>

static void journal_writer(const char *home, char marker)
{
    char record[2048];
    unsigned int index;

    (void)memset(record, marker, sizeof(record));
    for (index = 0U; index < 32U; index++)
        if (gsh_llm_journal_append(home, GSH_LLM_JOURNAL_COMMAND,
                                   record, sizeof(record)) == -1) _exit(1);
    _exit(0);
}

static int concurrent_append_case(const char *home, char *output,
                                  size_t capacity)
{
    static const char final[] = "FINAL-AFTER-CONCURRENCY";
    pid_t first = fork();
    pid_t second;
    int first_status = 0;
    int second_status = 0;

    if (first == 0) journal_writer(home, 'A');
    if (first < 0) return -1;
    second = fork();
    if (second == 0) journal_writer(home, 'B');
    if (second < 0) {
        (void)kill(first, SIGKILL);
        while (waitpid(first, &first_status, 0) == -1 && errno == EINTR) {
        }
        return -1;
    }
    while (waitpid(first, &first_status, 0) == -1 && errno == EINTR) {
    }
    while (waitpid(second, &second_status, 0) == -1 && errno == EINTR) {
    }
    if (!WIFEXITED(first_status) || WEXITSTATUS(first_status) != 0 ||
        !WIFEXITED(second_status) || WEXITSTATUS(second_status) != 0 ||
        gsh_llm_journal_append(home, GSH_LLM_JOURNAL_COMMAND, final,
                               sizeof(final) - 1U) == -1)
        return -1;
    return gsh_llm_journal_search(home, final, 20U, output, capacity) > 0
               ? 0 : -1;
}

static int small_output_case(const char *home)
{
    char output[32];
    size_t capacity;

    if (home == NULL) return -1;
    for (capacity = 1U; capacity < sizeof(output); capacity++) {
        (void)memset(output, 'X', sizeof(output));
        if (gsh_llm_journal_context(home, 1U, output, capacity) < 0 ||
            output[capacity] != 'X' || memchr(output, '\0', capacity) == NULL)
            return -1;
        (void)memset(output, 'X', sizeof(output));
        if (gsh_llm_journal_search(home, "keyword", 1U, output, capacity) < 0 ||
            output[capacity] != 'X' || memchr(output, '\0', capacity) == NULL)
            return -1;
    }
    return 0;
}

/* ── Rotation Must Survive Full Payloads And Interrupted Writes ──
 * Tiny prompt fixtures missed the mismatch between 64 KiB records and the
 * former 4 KiB rotation reader. These cases place a maximum output before
 * the prompt threshold, fill the byte budget, then tear a header deliberately.
 * New records must remain searchable and both retained files stay bounded.
 * ─────────────────────────────────────────────────────────────── */
static int rotation_case(const char *home)
{
    char payload[65536];
    char path[4096];
    char output[128];
    struct stat info;
    int descriptor;
    unsigned int index;

    if (home == NULL) return -1;
    (void)memset(payload, 'x', sizeof(payload));
    if (gsh_llm_journal_append(home, GSH_LLM_JOURNAL_COMMAND_OUTPUT,
                               payload, sizeof(payload)) == -1) return -1;
    for (index = 0U; index < 1025U; index++)
        if (gsh_llm_journal_append(home, GSH_LLM_JOURNAL_PROMPT,
                                   "prompt", 6U) == -1) return -1;
    if (snprintf(path, sizeof(path), "%s/.genshell/journal.previous", home)
            >= (int)sizeof(path) || stat(path, &info) == -1) return -1;
    for (index = 0U; index < 70U; index++)
        if (gsh_llm_journal_append(home, GSH_LLM_JOURNAL_COMMAND_OUTPUT,
                                   payload, sizeof(payload)) == -1) return -1;
    if (snprintf(path, sizeof(path), "%s/.genshell/journal", home)
            >= (int)sizeof(path) || stat(path, &info) == -1 ||
        info.st_size > 4 * 1024 * 1024) return -1;
    descriptor = open(path, O_WRONLY | O_APPEND);
    if (descriptor < 0) return -1;
    if (write(descriptor, "GSH", 3U) != 3) {
        (void)close(descriptor);
        return -1;
    }
    if (close(descriptor) == -1 ||
        gsh_llm_journal_append(home, GSH_LLM_JOURNAL_PROMPT,
                               "after-torn-header", 17U) == -1 ||
        gsh_llm_journal_context(home, 1U, output, sizeof(output)) <= 0 ||
        strstr(output, "after-torn-header") == NULL) return -1;
    return 0;
}

static int queue_case(const char *home)
{
    static gsh_llm_journal_queue queue;
    char payload[65536];
    char output[128];
    int descriptors[2];
    pid_t pid;
    int status = 0;
    unsigned int index;
    int failed = 0;

    if (home == NULL || pipe(descriptors) == -1) return -1;
    (void)memset(&queue, 0, sizeof(queue));
    queue.descriptor = descriptors[1];
    (void)memset(payload, 'Q', sizeof(payload));
    if (fcntl(descriptors[1], F_SETFL, O_NONBLOCK) == -1) failed = 1;
    for (index = 0U; index < 3U; index++)
        if (gsh_llm_journal_enqueue(&queue, home, GSH_LLM_JOURNAL_COMMAND,
                                    payload, sizeof(payload)) == -1) failed = 1;
    if (gsh_llm_journal_enqueue(&queue, home, GSH_LLM_JOURNAL_COMMAND,
                                payload, sizeof(payload)) != -1 ||
        errno != ENOBUFS) failed = 1;
    for (index = 0U; index < 64U; index++)
        if (gsh_llm_journal_flush(&queue) == -1) failed = 1;
    if (queue.used == 0U || queue.sent == 0U) failed = 1;
    pid = fork();
    if (pid == 0) {
        (void)close(descriptors[1]);
        _exit(gsh_llm_journal_worker(descriptors[0]));
    }
    (void)close(descriptors[0]);
    if (pid < 0) { (void)close(descriptors[1]); return -1; }
    if (gsh_llm_journal_enqueue(&queue, home, GSH_LLM_JOURNAL_PROMPT,
                                "after-queue-pressure", 20U) == -1) failed = 1;
    for (index = 0U; index < 1000U && queue.used != 0U; index++) {
        struct pollfd ready = {queue.descriptor, POLLOUT, 0};
        if (poll(&ready, 1U, 10) < 0 && errno != EINTR) { failed = 1; break; }
        if (gsh_llm_journal_flush(&queue) == -1) { failed = 1; break; }
    }
    if (queue.used != 0U) failed = 1;
    (void)close(descriptors[1]);
    if (failed) (void)kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    if (status != 0 ||
        gsh_llm_journal_context(home, 1U, output, sizeof(output)) <= 0 ||
        strstr(output, "after-queue-pressure") == NULL) failed = 1;
    return failed ? -1 : 0;
}

int main(void)
{
    char home[] = "/tmp/gsh-journal-XXXXXX";
    char output[16384];
    char path[4096];
    int failed = 0;

    if (mkdtemp(home) == NULL) return 1;
    if (gsh_llm_journal_append(home, GSH_LLM_JOURNAL_PROMPT,
                               "first prompt", 12U) == -1 ||
        gsh_llm_journal_append(home, GSH_LLM_JOURNAL_ANSWER,
                               "first answer", 12U) == -1 ||
        gsh_llm_journal_append(home, GSH_LLM_JOURNAL_PROMPT,
                               "second keyword", 14U) == -1 ||
        gsh_llm_journal_context(home, 1U, output, sizeof(output)) <= 0 ||
        strstr(output, "second keyword") == NULL ||
        strstr(output, "first prompt") != NULL ||
        gsh_llm_journal_search(home, "KEYWORD", 20U, output,
                               sizeof(output)) <= 0 ||
        strstr(output, "second keyword") == NULL ||
        small_output_case(home) == -1 ||
        concurrent_append_case(home, output, sizeof(output)) == -1 ||
        rotation_case(home) == -1 || queue_case(home) == -1) failed = 1;
    (void)snprintf(path, sizeof(path), "%s/.genshell/journal", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.genshell/journal.previous", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.genshell/journal.lock", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.genshell", home);
    (void)rmdir(path);
    (void)rmdir(home);
    if (failed) {
        (void)fputs("llm journal: failed\n", stderr);
        return 1;
    }
    (void)puts("llm journal: passed");
    return 0;
}
