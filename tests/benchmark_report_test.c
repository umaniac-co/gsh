#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "benchmark_report.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { TEST_CAPTURE_CAP = 65536 };

static int read_capture(const char *path, char *capture, size_t capacity)
{
    int descriptor = open(path, O_RDONLY);
    ssize_t count;

    if (descriptor == -1) {
        return -1;
    }
    count = read(descriptor, capture, capacity - 1U);
    if (count < 0 || close(descriptor) == -1) {
        return -1;
    }
    capture[count] = '\0';
    return 0;
}

static int report_round_trip(const char *path)
{
    static const uint64_t samples[] = {30, 10, 20};
    benchmark_report report;

    if (benchmark_report_open(&report, path) == -1 ||
        benchmark_report_write_metric(
            &report, "latency", "fixture", "gsh", "=revision",
            "/tmp/gsh\nnext", "duration", "ns", samples, 3U, 25U) == -1 ||
        benchmark_report_write_gate(&report, "fixture-gate", "gsh",
                                    "revision", "/tmp/gsh", "bash", 2U,
                                    3U) == -1 ||
        benchmark_report_commit(&report) == -1) {
        benchmark_report_abort(&report);
        return -1;
    }
    return 0;
}

static int verify_report(const char *capture)
{
    if (strstr(capture, "sample_001") == NULL ||
        strstr(capture, "sample_500") == NULL ||
        strstr(capture, "\"latency\",\"fixture\",\"gsh\"") == NULL ||
        strstr(capture, "\"'=revision\",\"/tmp/gsh\\nnext\"") == NULL ||
        strstr(capture, ",3,20,30,30,30,25,1,,,,,30,10,20,") == NULL ||
        strstr(capture, "\"gate\",\"fixture-gate\",\"gsh\"") == NULL ||
        strstr(capture, "\"bash\",2,3,\"pass\"") == NULL) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int verify_column_counts(const char *capture)
{
    size_t columns = 1;
    size_t rows = 0;
    size_t index;
    bool quoted = false;

    for (index = 0; index < TEST_CAPTURE_CAP && capture[index] != '\0';
         index++) {
        if (capture[index] == '"') {
            if (quoted && capture[index + 1U] == '"') {
                index++;
            } else {
                quoted = !quoted;
            }
        } else if (!quoted && capture[index] == ',') {
            columns++;
        } else if (!quoted && capture[index] == '\n') {
            if (columns != 24U + BENCHMARK_REPORT_SAMPLE_CAP) {
                errno = EINVAL;
                return -1;
            }
            rows++;
            columns = 1;
        }
    }
    return !quoted && rows == 3U ? 0 : -1;
}

static int abort_preserves_previous(const char *path)
{
    static const char original[] = "previous-complete-report\n";
    static const uint64_t samples[] = {1};
    benchmark_report report;
    char capture[64];
    int descriptor = open(path, O_WRONLY | O_TRUNC);

    if (descriptor == -1 ||
        write(descriptor, original, sizeof(original) - 1U) !=
            (ssize_t)(sizeof(original) - 1U) ||
        close(descriptor) == -1 ||
        benchmark_report_open(&report, path) == -1 ||
        benchmark_report_write_metric(
            &report, "latency", "aborted", "gsh", "revision", "/tmp/gsh",
            "duration", "ns", samples, 1U, 0) == -1) {
        return -1;
    }
    benchmark_report_abort(&report);
    return read_capture(path, capture, sizeof(capture)) == 0 &&
                   strcmp(capture, original) == 0
               ? 0
               : -1;
}

static int capacity_failure_preserves_previous(const char *path)
{
    static const char original[] = "previous-capacity-report\n";
    static const uint64_t sample = 1;
    benchmark_report report;
    char capture[64];
    int descriptor = open(path, O_WRONLY | O_TRUNC);

    if (descriptor == -1 ||
        write(descriptor, original, sizeof(original) - 1U) !=
            (ssize_t)(sizeof(original) - 1U) ||
        close(descriptor) == -1 ||
        benchmark_report_open(&report, path) == -1) {
        return -1;
    }
    if (benchmark_report_write_metric(
            &report, "latency", "overflow", "gsh", "revision", "/tmp/gsh",
            "duration", "ns", &sample,
            BENCHMARK_REPORT_SAMPLE_CAP + 1U, 0) != -1) {
        benchmark_report_abort(&report);
        errno = EINVAL;
        return -1;
    }
    benchmark_report_abort(&report);
    return read_capture(path, capture, sizeof(capture)) == 0 &&
                   strcmp(capture, original) == 0
               ? 0
               : -1;
}

int main(void)
{
    char directory[] = "/tmp/gsh-benchmark-report.XXXXXX";
    char path[256];
    char capture[TEST_CAPTURE_CAP];
    int failed = 0;

    if (mkdtemp(directory) == NULL ||
        snprintf(path, sizeof(path), "%s/report.csv", directory) < 0 ||
        report_round_trip(path) == -1 ||
        read_capture(path, capture, sizeof(capture)) == -1 ||
        verify_report(capture) == -1 || verify_column_counts(capture) == -1 ||
        abort_preserves_previous(path) == -1 ||
        capacity_failure_preserves_previous(path) == -1) {
        perror("benchmark report test");
        failed = 1;
    }
    (void)unlink(path);
    (void)rmdir(directory);
    if (!failed) {
        puts("benchmark report: tabular schema and atomic publication passed");
    }
    return failed;
}
