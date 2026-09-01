#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_times.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { GSH_TIMES_DECIMAL_CAP = 6 };

typedef struct {
    uintmax_t minutes;
    uintmax_t seconds;
    uintmax_t fraction;
} gsh_time_parts;

int gsh_times_snapshot(struct tms *snapshot)
{
    if (snapshot == NULL) {
        errno = EINVAL;
        return -1;
    }
    return times(snapshot) == (clock_t)-1 ? -1 : 0;
}

int gsh_times_rebase(gsh_times_context *context,
                     const struct tms *accumulated)
{
    if (context == NULL || accumulated == NULL) {
        errno = EINVAL;
        return -1;
    }
    memset(context, 0, sizeof(*context));
    context->accumulated = *accumulated;
    if (gsh_times_snapshot(&context->process_origin) == -1) {
        return -1;
    }
    context->active = true;
    return 0;
}

static int add_elapsed(clock_t accumulated, clock_t current,
                       clock_t origin, uintmax_t *result)
{
    uintmax_t base;
    uintmax_t elapsed;

    if (accumulated < (clock_t)0 || current < origin ||
        origin < (clock_t)0 || result == NULL) {
        errno = ERANGE;
        return -1;
    }
    base = (uintmax_t)accumulated;
    elapsed = (uintmax_t)(current - origin);
    if (elapsed > UINTMAX_MAX - base) {
        errno = EOVERFLOW;
        return -1;
    }
    *result = base + elapsed;
    return 0;
}

static unsigned int decimal_places(uintmax_t ticks_per_second)
{
    uintmax_t scale = 1;
    unsigned int places;

    for (places = 0; places < GSH_TIMES_DECIMAL_CAP; places++) {
        if (scale >= ticks_per_second) {
            break;
        }
        scale *= 10U;
    }
    return places == 0 ? 1U : places;
}

static gsh_time_parts split_ticks(uintmax_t ticks, uintmax_t rate,
                                  unsigned int places)
{
    gsh_time_parts parts;
    uintmax_t whole = ticks / rate;
    uintmax_t remainder = ticks % rate;
    uintmax_t scale = 1;
    unsigned int index;
    long double scaled;

    assert(rate > 0U);
    assert(places > 0U && places <= GSH_TIMES_DECIMAL_CAP);
    for (index = 0; index < places; index++) {
        scale *= 10U;
    }
    scaled = ((long double)remainder * (long double)scale) /
             (long double)rate;
    parts.fraction = (uintmax_t)(scaled + 0.5L);
    if (parts.fraction == scale) {
        whole++;
        parts.fraction = 0;
    }
    parts.minutes = whole / 60U;
    parts.seconds = whole % 60U;
    return parts;
}

/* ── Rebased Counters Preserve Logical Shell Accounting ──────────
 * times() describes the calling process, while gsh evaluates a compound
 * interactive command in a short-lived child that owns no earlier CPU time.
 * The parent therefore snapshots its counters and the child adds only its
 * post-fork delta; a true subshell deliberately receives no context. A failed
 * rebase stays inactive and is reported instead of printing child-only data.
 * Tick arithmetic stays integral until bounded, locale-neutral formatting.
 * ─────────────────────────────────────────────────────────────── */
static int logical_ticks(const struct tms *current,
                         const gsh_times_context *context,
                         uintmax_t values[4])
{
    clock_t live[4];
    clock_t accumulated[4] = {0, 0, 0, 0};
    clock_t origin[4] = {0, 0, 0, 0};
    size_t index;

    assert(current != NULL);
    assert(values != NULL);
    live[0] = current->tms_utime;
    live[1] = current->tms_stime;
    live[2] = current->tms_cutime;
    live[3] = current->tms_cstime;
    if (context != NULL && !context->active) {
        errno = EIO;
        return -1;
    }
    if (context != NULL) {
        accumulated[0] = context->accumulated.tms_utime;
        accumulated[1] = context->accumulated.tms_stime;
        accumulated[2] = context->accumulated.tms_cutime;
        accumulated[3] = context->accumulated.tms_cstime;
        origin[0] = context->process_origin.tms_utime;
        origin[1] = context->process_origin.tms_stime;
        origin[2] = context->process_origin.tms_cutime;
        origin[3] = context->process_origin.tms_cstime;
    }
    for (index = 0; index < 4U; index++) {
        if (add_elapsed(accumulated[index], live[index], origin[index],
                        &values[index]) == -1) {
            return -1;
        }
    }
    return 0;
}

int gsh_builtin_times(size_t argc, char *const argv[],
                      const gsh_times_context *context,
                      const gsh_builtin_io *io)
{
    struct tms current;
    uintmax_t ticks[4];
    gsh_time_parts parts[4];
    long rate;
    unsigned int places;
    char output[256];
    int length;
    size_t index;

    if (argv == NULL || io == NULL || io->output == NULL) {
        errno = EINVAL;
        return 125;
    }
    if (argc != 1 || argv[0] == NULL || strcmp(argv[0], "times") != 0) {
        return gsh_builtin_error(io, "times", "does not accept operands");
    }
    rate = sysconf(_SC_CLK_TCK);
    if (rate <= 0 || gsh_times_snapshot(&current) == -1 ||
        logical_ticks(&current, context, ticks) == -1) {
        return gsh_builtin_error(io, "times", "process timing failed");
    }
    places = decimal_places((uintmax_t)rate);
    for (index = 0; index < 4U; index++) {
        parts[index] = split_ticks(ticks[index], (uintmax_t)rate, places);
    }
    length = snprintf(
        output, sizeof(output),
        "%" PRIuMAX "m%" PRIuMAX ".%0*" PRIuMAX "s "
        "%" PRIuMAX "m%" PRIuMAX ".%0*" PRIuMAX "s\n"
        "%" PRIuMAX "m%" PRIuMAX ".%0*" PRIuMAX "s "
        "%" PRIuMAX "m%" PRIuMAX ".%0*" PRIuMAX "s\n",
        parts[0].minutes, parts[0].seconds, (int)places, parts[0].fraction,
        parts[1].minutes, parts[1].seconds, (int)places, parts[1].fraction,
        parts[2].minutes, parts[2].seconds, (int)places, parts[2].fraction,
        parts[3].minutes, parts[3].seconds, (int)places, parts[3].fraction);
    if (length < 0 || (size_t)length >= sizeof(output)) {
        return gsh_builtin_error(io, "times", "process timing overflow");
    }
    return io->output(io->opaque, STDOUT_FILENO, output, (size_t)length);
}
