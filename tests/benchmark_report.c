#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "benchmark_report.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum {
    BENCHMARK_REPORT_FIELD_CAP = 4096,
    BENCHMARK_REPORT_SCHEMA_VERSION = 1,
};

static int copy_field(char *destination, size_t capacity,
                      const char *source)
{
    size_t length;

    if (destination == NULL || capacity == 0 || source == NULL) {
        errno = EINVAL;
        return -1;
    }
    length = strnlen(source, capacity);
    if (length == capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(destination, source, length + 1U);
    return 0;
}

static int write_csv_string(FILE *stream, const char *value)
{
    size_t length;
    size_t index;
    bool formula_like;

    if (stream == NULL || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    length = strnlen(value, BENCHMARK_REPORT_FIELD_CAP + 1U);
    formula_like = length > 0 &&
                   (value[0] == '=' || value[0] == '+' || value[0] == '-' ||
                    value[0] == '@');
    if (length > BENCHMARK_REPORT_FIELD_CAP || fputc('"', stream) == EOF ||
        (formula_like && fputc('\'', stream) == EOF)) {
        errno = length > BENCHMARK_REPORT_FIELD_CAP ? EOVERFLOW : errno;
        return -1;
    }
    for (index = 0; index < BENCHMARK_REPORT_FIELD_CAP && index < length;
         index++) {
        if (value[index] == '\r' || value[index] == '\n' ||
            value[index] == '\t') {
            int escaped = value[index] == '\r' ? 'r'
                          : value[index] == '\n' ? 'n'
                                                 : 't';

            if (fputc('\\', stream) == EOF || fputc(escaped, stream) == EOF) {
                return -1;
            }
        } else if (value[index] == '"' && fputc('"', stream) == EOF) {
            return -1;
        } else if (fputc((unsigned char)value[index], stream) == EOF) {
            return -1;
        }
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

static int write_header(FILE *stream)
{
    static const char prefix[] =
        "schema_version,recorded_at_utc,revision,platform,cpu_count,compiler,"
        "suite,test,shell,shell_version,executable,measurement,unit,"
        "sample_count,p50,p95,p99,max,threshold,threshold_misses,peer,wins,"
        "total,status";
    size_t index;

    if (stream == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (fputs(prefix, stream) == EOF) {
        return -1;
    }
    for (index = 0; index < BENCHMARK_REPORT_SAMPLE_CAP; index++) {
        if (fprintf(stream, ",sample_%03zu", index + 1U) < 0) {
            return -1;
        }
    }
    return fputc('\n', stream) == EOF ? -1 : 0;
}

static int format_metadata(benchmark_report *report)
{
    struct utsname platform;
    struct tm recorded_at;
    const char *revision = getenv("GSH_BENCH_REVISION");
    const char *compiler = getenv("GSH_BENCH_COMPILER");
    time_t now = time(NULL);
    int length;

    if (report == NULL || now == (time_t)-1 ||
        gmtime_r(&now, &recorded_at) == NULL ||
        strftime(report->recorded_at_utc, sizeof(report->recorded_at_utc),
                 "%Y-%m-%dT%H:%M:%SZ", &recorded_at) == 0 ||
        uname(&platform) == -1) {
        return -1;
    }
    length = snprintf(report->platform, sizeof(report->platform), "%s %s %s",
                      platform.sysname, platform.release, platform.machine);
    if (length < 0 || (size_t)length >= sizeof(report->platform) ||
        copy_field(report->revision, sizeof(report->revision),
                   revision == NULL ? "unknown" : revision) == -1 ||
        copy_field(report->compiler, sizeof(report->compiler),
                   compiler == NULL ? "unknown" : compiler) == -1) {
        return -1;
    }
    report->cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    return report->cpu_count > 0 ? 0 : -1;
}

/* ── Atomic Publication Keeps Benchmark Evidence Whole ───────────
 * A benchmark used to rely on shell redirection, so cancellation could leave
 * a partial file that looked like a complete performance record.  The report
 * now owns a secure sibling temporary file and publishes it only after every
 * row is written, flushed, and synchronized.  Measurements finish before the
 * report is opened, keeping filesystem work outside every timed interval.
 * Failure removes only the temporary file and preserves the prior evidence.
 * ─────────────────────────────────────────────────────────────── */
int benchmark_report_open(benchmark_report *report, const char *path)
{
    int descriptor;
    int length;

    if (report == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(report, 0, sizeof(*report));
    if (copy_field(report->final_path, sizeof(report->final_path), path) ==
            -1 ||
        format_metadata(report) == -1) {
        return -1;
    }
    length = snprintf(report->temporary_path,
                      sizeof(report->temporary_path), "%s.tmp.XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof(report->temporary_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = mkstemp(report->temporary_path);
    if (descriptor == -1) {
        return -1;
    }
    report->stream = fdopen(descriptor, "w");
    if (report->stream == NULL) {
        int saved = errno;

        (void)close(descriptor);
        (void)unlink(report->temporary_path);
        errno = saved;
        return -1;
    }
    report->active = true;
    if (write_header(report->stream) == -1) {
        benchmark_report_abort(report);
        return -1;
    }
    return 0;
}

static int compare_u64_report(const void *left, const void *right)
{
    uint64_t first = *(const uint64_t *)left;
    uint64_t second = *(const uint64_t *)right;

    return first < second ? -1 : first > second ? 1 : 0;
}

static uint64_t percentile(const uint64_t *samples, size_t count,
                           size_t numerator)
{
    size_t rank = (count * numerator + 99U) / 100U;

    return samples[rank == 0 ? 0 : rank - 1U];
}

static int write_common_prefix(benchmark_report *report, const char *suite,
                               const char *test, const char *shell,
                               const char *shell_version,
                               const char *executable,
                               const char *measurement, const char *unit)
{
    FILE *stream = report->stream;

    if (fprintf(stream, "%d,", BENCHMARK_REPORT_SCHEMA_VERSION) < 0 ||
        write_csv_string(stream, report->recorded_at_utc) == -1 ||
        fputc(',', stream) == EOF ||
        write_csv_string(stream, report->revision) == -1 ||
        fputc(',', stream) == EOF ||
        write_csv_string(stream, report->platform) == -1 ||
        fprintf(stream, ",%ld,", report->cpu_count) < 0 ||
        write_csv_string(stream, report->compiler) == -1 ||
        fputc(',', stream) == EOF || write_csv_string(stream, suite) == -1 ||
        fputc(',', stream) == EOF || write_csv_string(stream, test) == -1 ||
        fputc(',', stream) == EOF || write_csv_string(stream, shell) == -1 ||
        fputc(',', stream) == EOF ||
        write_csv_string(stream, shell_version) == -1 ||
        fputc(',', stream) == EOF ||
        write_csv_string(stream, executable) == -1 ||
        fputc(',', stream) == EOF ||
        write_csv_string(stream, measurement) == -1 ||
        fputc(',', stream) == EOF || write_csv_string(stream, unit) == -1) {
        return -1;
    }
    return 0;
}

static int write_sample_columns(FILE *stream, const uint64_t *samples,
                                size_t count)
{
    size_t index;

    if (stream == NULL || count > BENCHMARK_REPORT_SAMPLE_CAP ||
        (count > 0 && samples == NULL)) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0; index < BENCHMARK_REPORT_SAMPLE_CAP; index++) {
        if (index < count) {
            if (fprintf(stream, ",%llu",
                        (unsigned long long)samples[index]) < 0) {
                return -1;
            }
        } else if (fputc(',', stream) == EOF) {
            return -1;
        }
    }
    return fputc('\n', stream) == EOF ? -1 : 0;
}

/* ── One Row Preserves One Comparable Measurement ────────────────
 * The previous stream mixed prose, percentiles, and variable-width raw rows.
 * That shape was readable only by a custom parser and obscured comparisons.
 * Each CSV row now identifies one test, shell, and measurement, places exact
 * integer summaries first, and retains every ordered raw sample in bounded
 * columns.  Statistics use a private copy, so sample order remains evidence.
 * ─────────────────────────────────────────────────────────────── */
int benchmark_report_write_metric(benchmark_report *report,
                                  const char *suite, const char *test,
                                  const char *shell,
                                  const char *shell_version,
                                  const char *executable,
                                  const char *measurement,
                                  const char *unit,
                                  const uint64_t *samples,
                                  size_t sample_count,
                                  uint64_t threshold)
{
    uint64_t sorted[BENCHMARK_REPORT_SAMPLE_CAP];
    size_t misses = 0;
    size_t index;

    if (report == NULL || !report->active || report->failed ||
        samples == NULL || sample_count == 0 ||
        sample_count > BENCHMARK_REPORT_SAMPLE_CAP) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0; index < BENCHMARK_REPORT_SAMPLE_CAP &&
                    index < sample_count;
         index++) {
        sorted[index] = samples[index];
        if (threshold > 0 && samples[index] > threshold) {
            misses++;
        }
    }
    qsort(sorted, sample_count, sizeof(sorted[0]), compare_u64_report);
    if (write_common_prefix(report, suite, test, shell, shell_version,
                            executable, measurement, unit) == -1 ||
        fprintf(report->stream,
                ",%zu,%llu,%llu,%llu,%llu,",
                sample_count,
                (unsigned long long)percentile(sorted, sample_count, 50),
                (unsigned long long)percentile(sorted, sample_count, 95),
                (unsigned long long)percentile(sorted, sample_count, 99),
                (unsigned long long)sorted[sample_count - 1U]) < 0 ||
        (threshold > 0 &&
         fprintf(report->stream, "%llu,%zu",
                 (unsigned long long)threshold, misses) < 0) ||
        (threshold == 0 && fputs(",", report->stream) == EOF) ||
        fputs(",,,,", report->stream) == EOF ||
        write_sample_columns(report->stream, samples, sample_count) == -1) {
        report->failed = true;
        return -1;
    }
    return 0;
}

int benchmark_report_write_gate(benchmark_report *report,
                                const char *test, const char *shell,
                                const char *shell_version,
                                const char *executable,
                                const char *peer, size_t wins,
                                size_t total)
{
    size_t minimum = total / 2U + 1U;
    const char *status = wins >= minimum ? "pass" : "fail";

    if (report == NULL || !report->active || report->failed || total == 0 ||
        wins > total) {
        errno = EINVAL;
        return -1;
    }
    if (write_common_prefix(report, "gate", test, shell, shell_version,
                            executable, "paired-wins", "count") == -1 ||
        fprintf(report->stream, ",,,,,,%zu,,", minimum) < 0 ||
        write_csv_string(report->stream, peer) == -1 ||
        fprintf(report->stream, ",%zu,%zu,", wins, total) < 0 ||
        write_csv_string(report->stream, status) == -1 ||
        write_sample_columns(report->stream, NULL, 0) == -1) {
        report->failed = true;
        return -1;
    }
    return 0;
}

int benchmark_report_commit(benchmark_report *report)
{
    int descriptor;
    int saved;

    if (report == NULL || !report->active || report->failed) {
        errno = EINVAL;
        return -1;
    }
    descriptor = fileno(report->stream);
    if (fflush(report->stream) == EOF || fsync(descriptor) == -1) {
        saved = errno;
        benchmark_report_abort(report);
        errno = saved;
        return -1;
    }
    if (fclose(report->stream) == EOF) {
        saved = errno;
        report->stream = NULL;
        (void)unlink(report->temporary_path);
        report->active = false;
        errno = saved;
        return -1;
    }
    report->stream = NULL;
    if (rename(report->temporary_path, report->final_path) == -1) {
        saved = errno;
        (void)unlink(report->temporary_path);
        report->active = false;
        errno = saved;
        return -1;
    }
    report->active = false;
    return 0;
}

void benchmark_report_abort(benchmark_report *report)
{
    if (report == NULL || !report->active) {
        return;
    }
    if (report->stream != NULL) {
        (void)fclose(report->stream);
        report->stream = NULL;
    }
    (void)unlink(report->temporary_path);
    report->active = false;
}
