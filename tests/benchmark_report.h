#ifndef GSH_BENCHMARK_REPORT_H
#define GSH_BENCHMARK_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum {
    BENCHMARK_REPORT_SAMPLE_CAP = 500,
    BENCHMARK_REPORT_PATH_CAP = 4096,
    BENCHMARK_REPORT_TEMP_PATH_CAP = 4128,
};

typedef struct {
    FILE *stream;
    char final_path[BENCHMARK_REPORT_PATH_CAP];
    char temporary_path[BENCHMARK_REPORT_TEMP_PATH_CAP];
    char recorded_at_utc[32];
    char revision[128];
    char platform[256];
    char compiler[512];
    long cpu_count;
    bool active;
    bool failed;
} benchmark_report;

int benchmark_report_open(benchmark_report *report, const char *path);

int benchmark_report_write_metric(benchmark_report *report,
                                  const char *suite, const char *test,
                                  const char *shell,
                                  const char *shell_version,
                                  const char *executable,
                                  const char *measurement,
                                  const char *unit,
                                  const uint64_t *samples,
                                  size_t sample_count,
                                  uint64_t threshold);

int benchmark_report_write_gate(benchmark_report *report,
                                const char *test, const char *shell,
                                const char *shell_version,
                                const char *executable,
                                const char *peer, size_t wins,
                                size_t total);

int benchmark_report_commit(benchmark_report *report);
void benchmark_report_abort(benchmark_report *report);

#endif
