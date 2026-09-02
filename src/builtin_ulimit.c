#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_ulimit.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

typedef struct {
    char option;
    int resource;
    rlim_t unit;
    const char *label;
    const char *unit_label;
} limit_spec;

static const limit_spec limits[] = {
    {'c', RLIMIT_CORE, 512, "core file size", "blocks"},
    {'d', RLIMIT_DATA, 1024, "data segment size", "kbytes"},
    {'f', RLIMIT_FSIZE, 512, "file size", "blocks"},
    {'n', RLIMIT_NOFILE, 1, "open file descriptors", "count"},
    {'s', RLIMIT_STACK, 1024, "stack size", "kbytes"},
#ifdef RLIMIT_CPU
    {'t', RLIMIT_CPU, 1, "cpu time", "seconds"},
#endif
#ifdef RLIMIT_AS
    {'v', RLIMIT_AS, 1024, "address space", "kbytes"},
#endif
};

static int fail(const gsh_builtin_io *io, const char *message)
{
    if (io == NULL || message == NULL) {
        return -1;
    }
    return gsh_builtin_error(io, "ulimit", message);
}

static const limit_spec *find_limit(char option)
{
    size_t index;

    for (index = 0; index < sizeof(limits) / sizeof(limits[0]); index++) {
        if (limits[index].option == option) {
            return &limits[index];
        }
    }
    return NULL;
}

static int format_limit(char output[128], const limit_spec *spec,
                        rlim_t value, bool labelled)
{
    if (output == NULL || spec == NULL) {
        return -1;
    }
    char number[64];
    int length;

    if (value == RLIM_INFINITY) {
        (void)memcpy(number, "unlimited", sizeof("unlimited"));
    } else {
        length = snprintf(number, sizeof(number), "%" PRIuMAX,
                          (uintmax_t)(value / spec->unit));
        if (length < 0 || (size_t)length >= sizeof(number)) {
            return -1;
        }
    }
    length = labelled
                 ? snprintf(output, 128, "%s (%s, -%c) %s\n", spec->label,
                            spec->unit_label, spec->option, number)
                 : snprintf(output, 128, "%s\n", number);
    return length < 0 || length >= 128 ? -1 : length;
}

static int report_limit(const limit_spec *spec, bool hard, bool labelled,
                        const gsh_builtin_io *io)
{
    if (spec == NULL) return -1;
    if (io == NULL) {
        return -1;
    }
    char output[128];
    struct rlimit value;
    int length;

    if (getrlimit(spec->resource, &value) == -1) {
        return fail(io, strerror(errno));
    }
    length = format_limit(output, spec, hard ? value.rlim_max : value.rlim_cur,
                          labelled);
    return length < 0
               ? fail(io, "value cannot be formatted")
               : gsh_builtin_output(io, STDOUT_FILENO, output,
                            (size_t)length);
}

static int parse_limit(const char *text, const limit_spec *spec,
                       rlim_t *value)
{
    if (spec == NULL) return -1;
    if (value == NULL) {
        return -1;
    }
    char *end;
    uintmax_t number;

    if (strcmp(text, "unlimited") == 0) {
        *value = RLIM_INFINITY;
        return 0;
    }
    errno = 0;
    number = strtoumax(text, &end, 10);
    if (errno == ERANGE || *text == '\0' || *end != '\0' ||
        number > (uintmax_t)RLIM_INFINITY / (uintmax_t)spec->unit) {
        return -1;
    }
    *value = (rlim_t)(number * (uintmax_t)spec->unit);
    return 0;
}

static int set_limit(const limit_spec *spec, const char *operand,
                     bool hard, bool soft, const gsh_builtin_io *io)
{
    if (io == NULL) {
        return -1;
    }
    struct rlimit current;
    rlim_t value;

    if (parse_limit(operand, spec, &value) == -1 ||
        getrlimit(spec->resource, &current) == -1) {
        return fail(io, errno == 0 ? "invalid limit" : strerror(errno));
    }
    if (hard) {
        current.rlim_max = value;
    }
    if (soft) {
        current.rlim_cur = value;
    }
    return setrlimit(spec->resource, &current) == -1
               ? fail(io, strerror(errno))
               : 0;
}

int gsh_builtin_ulimit(size_t argc, char *const argv[],
                       const gsh_builtin_io *io)
{
    if (argv == NULL || io == NULL) {
        return -1;
    }
    const limit_spec *selected = NULL;
    const char *operand = NULL;
    bool all = false;
    bool hard = false;
    bool soft = false;
    size_t index;

    for (index = 1; index < argc; index++) {
        const char *argument = argv[index];

        if (strcmp(argument, "--") == 0) {
            index++;
            break;
        }
        if (argument[0] != '-' || argument[1] == '\0') {
            break;
        }
        if (argument[2] != '\0') {
            return fail(io, "options must be specified separately");
        }
        if (argument[1] == 'H') {
            hard = true;
        } else if (argument[1] == 'S') {
            soft = true;
        } else if (argument[1] == 'a') {
            all = true;
        } else {
            const limit_spec *candidate = find_limit(argument[1]);

            if (candidate == NULL || selected != NULL) {
                return fail(io, "invalid or repeated resource option");
            }
            selected = candidate;
        }
    }
    if (index < argc) {
        operand = argv[index++];
    }
    if (index != argc || (all && (selected != NULL || operand != NULL))) {
        return fail(io, "invalid operands");
    }
    if (selected == NULL && !all) {
        selected = find_limit('f');
    }
    if (operand != NULL) {
        return set_limit(selected, operand, hard || !soft, soft || !hard,
                         io);
    }
    if (all) {
        int status = 0;

        for (index = 0; index < sizeof(limits) / sizeof(limits[0]); index++) {
            status |= report_limit(&limits[index], hard && !soft, true, io);
        }
        return status;
    }
    return report_limit(selected, hard && !soft, false, io);
}
