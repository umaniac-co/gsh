#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_job_control.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *name;
    int number;
} signal_name;

static const signal_name signal_names[] = {
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT},
    {"ILL", SIGILL}, {"ABRT", SIGABRT}, {"FPE", SIGFPE},
    {"KILL", SIGKILL}, {"SEGV", SIGSEGV}, {"PIPE", SIGPIPE},
    {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"USR1", SIGUSR1},
    {"USR2", SIGUSR2}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT},
    {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN},
    {"TTOU", SIGTTOU},
#ifdef SIGBUS
    {"BUS", SIGBUS},
#endif
#ifdef SIGPOLL
    {"POLL", SIGPOLL},
#endif
#ifdef SIGPROF
    {"PROF", SIGPROF},
#endif
#ifdef SIGSYS
    {"SYS", SIGSYS},
#endif
#ifdef SIGTRAP
    {"TRAP", SIGTRAP},
#endif
#ifdef SIGURG
    {"URG", SIGURG},
#endif
#ifdef SIGVTALRM
    {"VTALRM", SIGVTALRM},
#endif
#ifdef SIGWINCH
    {"WINCH", SIGWINCH},
#endif
#ifdef SIGXCPU
    {"XCPU", SIGXCPU},
#endif
#ifdef SIGXFSZ
    {"XFSZ", SIGXFSZ},
#endif
};

enum {
    GSH_JOB_OPERAND_CAP = 128,
    GSH_SIGNAL_NUMBER_CAP = 127,
};

typedef struct {
    pid_t target;
    bool process_group;
} kill_target;

static int emit_text(const gsh_builtin_io *io, int descriptor,
                     const char *text, size_t length)
{
    return io == NULL || io->output == NULL ||
                   io->output(io->opaque, descriptor, text, length) != 0
               ? 1 : 0;
}

static int job_error(const gsh_builtin_io *io, const char *name,
                     const char *message)
{
    return gsh_builtin_error(io, name, message);
}

static const char *job_state_name(gsh_job_state state)
{
    if (state == GSH_JOB_STOPPED) return "Stopped";
    if (state == GSH_JOB_DONE) return "Done";
    return "Running";
}

static int write_job(const gsh_background_table *table,
                     const gsh_background_entry *entry, bool long_format,
                     bool process_group_only, const gsh_builtin_io *io)
{
    char line[GSH_BACKGROUND_COMMAND_CAP + 96U];
    const char *command = entry->command_length == 0 ? "(command)"
                                                     : entry->command;
    int length;

    if (process_group_only) {
        length = snprintf(line, sizeof(line), "%ld\n", (long)entry->pgid);
    } else if (long_format) {
        length = snprintf(line, sizeof(line), "[%u]%c %ld %-8s %s\n",
                          entry->job_id,
                          gsh_background_marker(table, entry->job_id),
                          (long)entry->pgid, job_state_name(entry->state),
                          command);
    } else {
        length = snprintf(line, sizeof(line), "[%u]%c %-8s %s\n",
                          entry->job_id,
                          gsh_background_marker(table, entry->job_id),
                          job_state_name(entry->state), command);
    }
    if (length < 0 || (size_t)length >= sizeof(line)) {
        return job_error(io, "jobs", "job description exceeds limit");
    }
    return emit_text(io, STDOUT_FILENO, line, (size_t)length);
}

static int resolve_job_operand(const gsh_background_table *table,
                               const char *operand, uint32_t *job_id,
                               const gsh_builtin_io *io)
{
    gsh_jobspec_status status =
        gsh_background_resolve(table, operand, job_id);

    if (status == GSH_JOBSPEC_OK) return 0;
    if (status == GSH_JOBSPEC_AMBIGUOUS) {
        return job_error(io, "jobs", "ambiguous job specification");
    }
    if (status == GSH_JOBSPEC_MISSING) {
        return job_error(io, "jobs", "no such job");
    }
    return job_error(io, "jobs", "invalid job specification");
}

int gsh_builtin_jobs(size_t argc, char *const argv[],
                     gsh_background_table *jobs,
                     const gsh_builtin_io *io)
{
    uint32_t selected[GSH_JOB_OPERAND_CAP];
    size_t selected_count = 0;
    size_t index = 1U;
    bool long_format = false;
    bool process_group_only = false;
    int result = 0;

    if (jobs == NULL) return job_error(io, "jobs", "job service unavailable");
    while (index < argc && argv[index][0] == '-' && argv[index][1] != '\0') {
        const char *option = argv[index] + 1U;

        if (strcmp(argv[index], "--") == 0) {
            index++;
            break;
        }
        while (*option != '\0') {
            if (*option == 'l') long_format = true;
            else if (*option == 'p') process_group_only = true;
            else return job_error(io, "jobs", "invalid option");
            option++;
        }
        index++;
    }
    if (argc - index > GSH_JOB_OPERAND_CAP) {
        return job_error(io, "jobs", "too many operands");
    }
    while (index < argc) {
        if (resolve_job_operand(jobs, argv[index++],
                                &selected[selected_count], io) != 0) {
            return 1;
        }
        selected_count++;
    }
    if (selected_count == 0) {
        for (index = 0; index < GSH_BACKGROUND_CAP; index++) {
            if (!jobs->entries[index].known) continue;
            if (write_job(jobs, &jobs->entries[index], long_format,
                          process_group_only, io) != 0) {
                result = 1;
            } else {
                (void)gsh_background_mark_notified(
                    jobs, jobs->entries[index].job_id);
            }
        }
    } else {
        for (index = 0; index < selected_count; index++) {
            const gsh_background_entry *entry =
                gsh_background_entry_for_id(jobs, selected[index]);

            if (entry == NULL || write_job(
                    jobs, entry, long_format, process_group_only, io) != 0) {
                result = 1;
            } else {
                (void)gsh_background_mark_notified(jobs, selected[index]);
            }
        }
    }
    return result;
}

static bool signal_text_equal(const char *left, const char *right)
{
    size_t index;

    for (index = 0; left[index] != '\0' && right[index] != '\0'; index++) {
        if (toupper((unsigned char)left[index]) !=
            toupper((unsigned char)right[index])) return false;
    }
    return left[index] == '\0' && right[index] == '\0';
}

static int parse_signal_name(const char *text, int *number)
{
    const char *name = text;
    char *end;
    long parsed;
    size_t index;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno == 0 && end != text && *end == '\0' && parsed >= 0 &&
        parsed <= GSH_SIGNAL_NUMBER_CAP) {
        *number = (int)parsed;
        return 0;
    }
    if ((text[0] == 'S' || text[0] == 's') &&
        (text[1] == 'I' || text[1] == 'i') &&
        (text[2] == 'G' || text[2] == 'g')) name += 3U;
    for (index = 0; index < sizeof(signal_names) / sizeof(signal_names[0]);
         index++) {
        if (signal_text_equal(name, signal_names[index].name)) {
            *number = signal_names[index].number;
            return 0;
        }
    }
    return -1;
}

static const char *signal_number_name(int number)
{
    size_t index;

    for (index = 0; index < sizeof(signal_names) / sizeof(signal_names[0]);
         index++) {
        if (signal_names[index].number == number) {
            return signal_names[index].name;
        }
    }
    return NULL;
}

static int list_signals(size_t argc, char *const argv[], size_t index,
                        const gsh_builtin_io *io)
{
    char line[512];
    size_t used = 0;
    size_t signal_index;

    if (argc - index > 1U) return job_error(io, "kill", "too many operands");
    if (index < argc) {
        char *end;
        long status;

        errno = 0;
        status = strtol(argv[index], &end, 10);
        if (errno == 0 && end != argv[index] && *end == '\0') {
            int number = status > 128 ? (int)(status - 128) : (int)status;
            const char *name = signal_number_name(number);

            if (name == NULL) return job_error(io, "kill", "unknown signal");
            return emit_text(io, STDOUT_FILENO, name, strlen(name)) ||
                   emit_text(io, STDOUT_FILENO, "\n", 1U);
        } else {
            int number;
            char line[32];
            int length;

            if (parse_signal_name(argv[index], &number) == -1) {
                return job_error(io, "kill", "unknown signal");
            }
            length = snprintf(line, sizeof(line), "%d\n", number);
            if (length < 0 || (size_t)length >= sizeof(line)) {
                return job_error(io, "kill", "signal number exceeds limit");
            }
            return emit_text(io, STDOUT_FILENO, line, (size_t)length);
        }
    }
    for (signal_index = 0;
         signal_index < sizeof(signal_names) / sizeof(signal_names[0]);
         signal_index++) {
        int length = snprintf(line + used, sizeof(line) - used, "%s%s",
                              used == 0 ? "" : " ",
                              signal_names[signal_index].name);

        if (length < 0 || (size_t)length >= sizeof(line) - used) {
            return job_error(io, "kill", "signal list exceeds limit");
        }
        used += (size_t)length;
    }
    line[used++] = '\n';
    return emit_text(io, STDOUT_FILENO, line, used);
}

static int parse_kill_target(const gsh_background_table *jobs,
                             const char *operand, kill_target *target,
                             const gsh_builtin_io *io)
{
    if (operand[0] == '%') {
        uint32_t job_id;
        gsh_jobspec_status status =
            gsh_background_resolve(jobs, operand, &job_id);
        const gsh_background_entry *entry;

        if (status == GSH_JOBSPEC_AMBIGUOUS) {
            return job_error(io, "kill", "ambiguous job specification");
        }
        if (status != GSH_JOBSPEC_OK) {
            return job_error(io, "kill", "no such job");
        }
        entry = gsh_background_entry_for_id(jobs, job_id);
        if (entry == NULL || entry->pgid <= 0 ||
            entry->state == GSH_JOB_DONE) {
            return job_error(io, "kill", "job has no process group");
        }
        target->target = entry->pgid;
        target->process_group = true;
        return 0;
    }
    {
        char *end;
        long parsed;

        errno = 0;
        parsed = strtol(operand, &end, 10);
        if (errno != 0 || end == operand || *end != '\0' || parsed < 0 ||
            parsed > INT_MAX) {
            return job_error(io, "kill", "invalid process ID");
        }
        target->target = (pid_t)parsed;
        target->process_group = false;
    }
    return 0;
}

int gsh_builtin_kill(size_t argc, char *const argv[],
                     const gsh_background_table *jobs,
                     const gsh_builtin_io *io)
{
    kill_target targets[GSH_JOB_OPERAND_CAP];
    size_t index = 1U;
    size_t target_count = 0;
    int signal_number = SIGTERM;
    int result = 0;

    memset(targets, 0, sizeof(targets));
    if (jobs == NULL) return job_error(io, "kill", "job service unavailable");
    if (index < argc && strcmp(argv[index], "-l") == 0) {
        return list_signals(argc, argv, index + 1U, io);
    }
    if (index < argc && strcmp(argv[index], "-s") == 0) {
        if (++index >= argc ||
            parse_signal_name(argv[index++], &signal_number) == -1) {
            return job_error(io, "kill", "unknown signal");
        }
    } else if (index < argc && strcmp(argv[index], "--") == 0) {
        index++;
    } else if (index < argc && argv[index][0] == '-' &&
               argv[index][1] != '\0') {
        if (parse_signal_name(argv[index++] + 1U, &signal_number) == -1) {
            return job_error(io, "kill", "unknown signal");
        }
    }
    if (index < argc && strcmp(argv[index], "--") == 0) index++;
    if (index >= argc) return job_error(io, "kill", "missing operand");
    if (argc - index > GSH_JOB_OPERAND_CAP) {
        return job_error(io, "kill", "too many operands");
    }
    while (index < argc) {
        if (parse_kill_target(jobs, argv[index++], &targets[target_count],
                              io) != 0) return 1;
        target_count++;
    }
    for (index = 0; index < target_count; index++) {
        pid_t target = targets[index].process_group
                           ? -targets[index].target : targets[index].target;

        if (kill(target, signal_number) == -1) {
            char message[160];
            int length = snprintf(message, sizeof(message),
                                  "cannot signal %ld: %s", (long)target,
                                  strerror(errno));

            if (length > 0 && (size_t)length < sizeof(message)) {
                (void)job_error(io, "kill", message);
            }
            result = 1;
        }
    }
    return result;
}
