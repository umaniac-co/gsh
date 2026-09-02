#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "benchmark_report.h"

#include <errno.h>
#include <stdarg.h> /* CANON-INCLUDE: macos */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

enum {
    BENCHMARK_REPORT_FIELD_CAP = 4096,
    BENCHMARK_REPORT_FORMAT_CAP = 8192,
    BENCHMARK_REPORT_SCHEMA_VERSION = 1,
};

static int write_all(int descriptor, const char *data, size_t length)
{
    size_t written = 0;
    size_t attempts;

    if (descriptor < 0 || (length > 0U && data == NULL)) {
        errno = EINVAL;
        return -1;
    }
    for (attempts = 0; written < length && attempts <= length; attempts++) {
        ssize_t count = write(descriptor, data + written, length - written);

        if (count > 0) {
            written += (size_t)count;
        } else if (count != -1 || errno != EINTR) {
            return -1;
        }
    }
    if (written != length) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int write_text(int descriptor, const char *text)
{
    size_t length;

    if (text == NULL) {
        errno = EINVAL;
        return -1;
    }
    length = strnlen(text, BENCHMARK_REPORT_FORMAT_CAP);
    if (length == BENCHMARK_REPORT_FORMAT_CAP) {
        errno = EOVERFLOW;
        return -1;
    }
    return write_all(descriptor, text, length);
}

static int write_format(int descriptor, const char *format, ...)
{
    char buffer[BENCHMARK_REPORT_FORMAT_CAP];
    va_list arguments;
    int length;

    if (format == NULL) {
        errno = EINVAL;
        return -1;
    }
    va_start(arguments, format);
    length = vsnprintf(buffer, sizeof(buffer), format, arguments);
    va_end(arguments);
    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        errno = EOVERFLOW;
        return -1;
    }
    return write_all(descriptor, buffer, (size_t)length);
}

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
    (void)memcpy(destination, source, length + 1U);
    return 0;
}

static int write_csv_string(int descriptor, const char *value)
{
    size_t length;
    size_t index;
    bool formula_like;

    if (descriptor < 0 || value == NULL) {
        errno = EINVAL;
        return -1;
    }
    length = strnlen(value, BENCHMARK_REPORT_FIELD_CAP + 1U);
    formula_like = length > 0 &&
                   (value[0] == '=' || value[0] == '+' || value[0] == '-' ||
                    value[0] == '@');
    if (length > BENCHMARK_REPORT_FIELD_CAP ||
        write_all(descriptor, "\"", 1U) == -1 ||
        (formula_like && write_all(descriptor, "'", 1U) == -1)) {
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

            char escaped_text[2] = {'\\', (char)escaped};

            if (write_all(descriptor, escaped_text,
                          sizeof(escaped_text)) == -1) {
                return -1;
            }
        } else if (value[index] == '"' &&
                   write_all(descriptor, "\"", 1U) == -1) {
            return -1;
        } else if (write_all(descriptor, &value[index], 1U) == -1) {
            return -1;
        }
    }
    return write_all(descriptor, "\"", 1U);
}

static int write_header(int descriptor)
{
    static const char prefix[] =
        "schema_version,recorded_at_utc,revision,platform,cpu_count,compiler,"
        "suite,test,shell,shell_version,executable,measurement,unit,"
        "sample_count,p50,p95,p99,max,threshold,threshold_misses,peer,wins,"
        "total,status";
    size_t index;

    if (descriptor < 0) {
        errno = EINVAL;
        return -1;
    }
    if (write_text(descriptor, prefix) == -1) {
        return -1;
    }
    for (index = 0; index < BENCHMARK_REPORT_SAMPLE_CAP; index++) {
        if (write_format(descriptor, ",sample_%03zu", index + 1U) == -1) {
            return -1;
        }
    }
    return write_all(descriptor, "\n", 1U);
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
    (void)memset(report, 0, sizeof(*report));
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
    report->descriptor = descriptor;
    report->active = true;
    if (write_header(report->descriptor) == -1) {
        benchmark_report_abort(report);
        return -1;
    }
    return 0;
}

static void sort_u64_report(uint64_t *values, size_t count)
{
    if (values == NULL) {
        return;
    }
    size_t index;

    for (index = 1U; index < count; index++) {
        uint64_t value = values[index];
        size_t position = index;

        while (position > 0U && values[position - 1U] > value) {
            values[position] = values[position - 1U];
            position--;
        }
        values[position] = value;
    }
}

static uint64_t percentile(const uint64_t *samples, size_t count,
                           size_t numerator)
{
    if (samples == NULL) {
        return 0U;
    }
    size_t rank = (count * numerator + 99U) / 100U;

    return samples[rank == 0 ? 0 : rank - 1U];
}

static int write_common_prefix(benchmark_report *report, const char *suite,
                               const char *test, const char *shell,
                               const char *shell_version,
                               const char *executable,
                               const char *measurement, const char *unit)
{
    if (report == NULL) {
        return -1;
    }
    int descriptor = report->descriptor;

    if (write_format(descriptor, "%d,", BENCHMARK_REPORT_SCHEMA_VERSION) ==
            -1 ||
        write_csv_string(descriptor, report->recorded_at_utc) == -1 ||
        write_all(descriptor, ",", 1U) == -1 ||
        write_csv_string(descriptor, report->revision) == -1 ||
        write_all(descriptor, ",", 1U) == -1 ||
        write_csv_string(descriptor, report->platform) == -1 ||
        write_format(descriptor, ",%ld,", report->cpu_count) == -1 ||
        write_csv_string(descriptor, report->compiler) == -1 ||
        write_all(descriptor, ",", 1U) == -1 ||
        write_csv_string(descriptor, suite) == -1 ||
        write_all(descriptor, ",", 1U) == -1 ||
        write_csv_string(descriptor, test) == -1 ||
        write_all(descriptor, ",", 1U) == -1 ||
        write_csv_string(descriptor, shell) == -1 ||
        write_all(descriptor, ",", 1U) == -1 ||
        write_csv_string(descriptor, shell_version) == -1 ||
        write_all(descriptor, ",", 1U) == -1 ||
        write_csv_string(descriptor, executable) == -1 ||
        write_all(descriptor, ",", 1U) == -1 ||
        write_csv_string(descriptor, measurement) == -1 ||
        write_all(descriptor, ",", 1U) == -1 ||
        write_csv_string(descriptor, unit) == -1) {
        return -1;
    }
    return 0;
}

static int write_sample_columns(int descriptor, const uint64_t *samples,
                                size_t count)
{
    size_t index;

    if (descriptor < 0 || count > BENCHMARK_REPORT_SAMPLE_CAP ||
        (count > 0 && samples == NULL)) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0; index < BENCHMARK_REPORT_SAMPLE_CAP; index++) {
        if (index < count) {
            if (write_format(descriptor, ",%llu",
                             (unsigned long long)samples[index]) == -1) {
                return -1;
            }
        } else if (write_all(descriptor, ",", 1U) == -1) {
            return -1;
        }
    }
    return write_all(descriptor, "\n", 1U);
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
    sort_u64_report(sorted, sample_count);
    if (write_common_prefix(report, suite, test, shell, shell_version,
                            executable, measurement, unit) == -1 ||
        write_format(report->descriptor, ",%zu,%llu,%llu,%llu,%llu,",
                     sample_count,
                     (unsigned long long)percentile(sorted, sample_count, 50),
                     (unsigned long long)percentile(sorted, sample_count, 95),
                     (unsigned long long)percentile(sorted, sample_count, 99),
                     (unsigned long long)sorted[sample_count - 1U]) == -1 ||
        (threshold > 0 &&
         write_format(report->descriptor, "%llu,%zu",
                      (unsigned long long)threshold, misses) == -1) ||
        (threshold == 0 && write_all(report->descriptor, ",", 1U) == -1) ||
        write_text(report->descriptor, ",,,,") == -1 ||
        write_sample_columns(report->descriptor, samples, sample_count) ==
            -1) {
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
        write_format(report->descriptor, ",,,,,,%zu,,", minimum) == -1 ||
        write_csv_string(report->descriptor, peer) == -1 ||
        write_format(report->descriptor, ",%zu,%zu,", wins, total) == -1 ||
        write_csv_string(report->descriptor, status) == -1 ||
        write_sample_columns(report->descriptor, NULL, 0) == -1) {
        report->failed = true;
        return -1;
    }
    return 0;
}

int benchmark_report_commit(benchmark_report *report)
{
    int saved;

    if (report == NULL || !report->active || report->failed) {
        errno = EINVAL;
        return -1;
    }
    if (fsync(report->descriptor) == -1) {
        saved = errno;
        benchmark_report_abort(report);
        errno = saved;
        return -1;
    }
    if (close(report->descriptor) == -1) {
        saved = errno;
        report->descriptor = -1;
        (void)unlink(report->temporary_path);
        report->active = false;
        errno = saved;
        return -1;
    }
    report->descriptor = -1;
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
    if (report->descriptor >= 0) {
        (void)close(report->descriptor);
        report->descriptor = -1;
    }
    (void)unlink(report->temporary_path);
    report->active = false;
}
