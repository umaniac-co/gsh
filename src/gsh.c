#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "builtin_cd.h"
#include "builtin_alias.h"
#include "builtin_unalias.h"
#include "builtin_ulimit.h"
#include "builtin_umask.h"
#include "builtin_variables.h"
#include "builtin_set.h"
#include "builtin_shift.h"
#include "async_repl.h"
#include "background_jobs.h"
#include "alias_expansion.h"
#include "native_plan.h"
#include "positional_parameters.h"
#include "posix_lexer.h"
#include "posix_parser.h"
#include "shell_variables.h"
#include "shell_aliases.h"
#include "shell_functions.h"
#include "shell_options.h"

extern char **environ;

enum {
    LINE_CAP = 4096,
    OUTPUT_CAP = 65536,
    MAX_SIGNAL_REAPS = 16,
    SIMPLE_ARG_CAP = 128,
    EXEC_PATH_CAP = 4096,
    PATH_SCAN_CAP = 32768,
    PROMPT_BRANCH_CAP = 128,
    SECONDARY_PROMPT_CAP = 128,
    PROMPT_ANCESTOR_CAP = 32,
    PROMPT_PROTOCOL_VERSION = 1,
    PROMPT_REQUEST_BRANCH = 1,
    PROMPT_REQUEST_REDIRECTION = 2,
    GSH_NATIVE_JOB_MEMBER_CAP =
        GSH_NATIVE_PIPELINE_CAP + GSH_NATIVE_HEREDOC_CAP,
    CHILD_ENVIRONMENT_CAP = GSH_VARIABLE_ENVIRONMENT_CAP,
};

_Static_assert((unsigned int)GSH_POSITIONAL_CAP ==
                   (unsigned int)GSH_NATIVE_ARGUMENT_CAP,
               "positional and native argument limits must match");
_Static_assert((unsigned int)GSH_POSITIONAL_TEXT_CAP ==
                   (unsigned int)GSH_NATIVE_TEXT_CAP,
               "positional and native text limits must match");

static const char PROMPT[] = "$gsh> ";
static const uint64_t REACTOR_DEADLINE_NS = 5U * 1000U * 1000U;
static const uint64_t PROMPT_WORKER_DEADLINE_NS = 100U * 1000U * 1000U;

typedef enum {
    MODE_EDITOR,
    MODE_DISPATCH,
    MODE_FOREGROUND,
    MODE_ASYNC_WORKER,
    MODE_WAIT,
} run_mode;

typedef struct {
    bool active;
    bool foreground;
    bool stopped;
    bool silent;
    bool negated;
    pid_t pid;
    pid_t pgid;
    pid_t status_pid;
    pid_t members[GSH_NATIVE_JOB_MEMBER_CAP];
    unsigned char member_states[GSH_NATIVE_JOB_MEMBER_CAP];
    size_t member_count;
    size_t remaining;
    int pipeline_wait_status;
    bool pipeline_status_known;
    struct termios modes;
} job;

typedef struct {
    char *argv[SIMPLE_ARG_CAP + 1];
    size_t argc;
} simple_command;

typedef struct {
    uint32_t version;
    uint32_t type;
    uint64_t request_id;
    uint64_t generation;
    uint64_t deadline_ns;
    uint32_t operator_kind;
    uint32_t option_bits;
    uint32_t creation_mode;
    int32_t builtin_status;
    char directory[PATH_MAX];
} prompt_request;

typedef struct {
    uint32_t version;
    uint32_t type;
    uint64_t request_id;
    uint64_t generation;
    int32_t status;
    int32_t error;
    char branch[PROMPT_BRANCH_CAP];
} prompt_result;

typedef struct {
    int tty_fd;
    int signal_pipe[2];
    pid_t shell_pgid;
    struct termios original_modes;
    struct termios editor_modes;
    bool terminal_changed;
    bool running;
    run_mode mode;

    char line[LINE_CAP];
    size_t line_len;
    unsigned int escape_state;
    char pending_line[LINE_CAP];
    size_t pending_len;
    const char *pending_input;
    size_t pending_input_length;
    bool pending_alias_expanded;
    bool continuation_prompt;
    char default_path[EXEC_PATH_CAP];
    const char *parameter_zero;
    char current_directory[PATH_MAX];
    char prompt_branch[PROMPT_BRANCH_CAP];

    char output[OUTPUT_CAP];
    size_t output_offset;
    size_t output_len;
    gsh_async_repl *async_repl;
    int async_capture_cell;
    int async_state_cell;
    int async_dispatch_cell;

    job current_job;
    gsh_background_table background_jobs;
    long last_background_pid;
    pid_t wait_targets[GSH_BACKGROUND_CAP];
    size_t wait_target_count;
    bool wait_all;
    bool wait_negated;
    bool pending_list_active;
    size_t pending_list_next;
    bool pending_and_or_active;
    size_t pending_and_or_next;
    int last_status;
    gsh_shell_options options;

    int prompt_worker_fd;
    pid_t prompt_worker_pid;
    bool prompt_worker_alive;
    bool prompt_worker_busy;
    bool prompt_worker_restart_pending;
    bool prompt_request_pending;
    uint64_t prompt_next_request_id;
    uint64_t prompt_active_request_id;
    uint32_t prompt_active_request_type;
    uint64_t prompt_generation;
    uint64_t prompt_worker_deadline_ns;
    bool worker_pipeline_negated;
    char worker_redirection_target[PATH_MAX];
    gsh_parse_storage *parse_storage;
    gsh_parse_result pending_parse;
    gsh_native_pipeline *native_pipeline;
    gsh_variable_store *variables;
    gsh_variable_store *variable_scratch;
    gsh_variable_store *pipeline_variables;
    gsh_variable_journal *variable_commit;
    gsh_variable_journal *pipeline_changes;
    char *alias_expansion;
    gsh_alias_store *aliases;
    gsh_alias_store *alias_scratch;
    gsh_alias_journal *alias_commit;
    gsh_function_store *functions;
    gsh_function_store *function_scratch;
    gsh_function_snapshot_header function_commit_header;
    gsh_positional_store *positionals;
    gsh_positional_store *positional_commit;
    gsh_shell_options option_commit;
    bool positional_commit_expected;
    bool pending_positional_commit;
    bool alias_commit_expected;
    bool pending_alias_commit;
    bool function_commit_expected;
    bool pending_function_commit;
    bool function_commit_header_complete;
    int directory_commit_socket;
    int directory_commit_fd;
    bool directory_commit_expected;
    bool pending_directory_commit;
    uint64_t variable_generation;
    uint64_t alias_generation;
    uint64_t function_generation;
    int variable_commit_fd;
    size_t variable_commit_received;
    bool variable_commit_active;
    bool variable_commit_eof;
    bool variable_commit_invalid;

    uint64_t reactor_cycles;
    uint64_t reactor_misses;
    uint64_t reactor_max_ns;
    uint64_t overloads;
    uint64_t dispatch_cycles;
    uint64_t dispatch_misses;
    uint64_t dispatch_max_ns;
    uint64_t direct_dispatches;
    uint64_t native_pipeline_dispatches;
    uint64_t shell_dispatches;
    uint64_t parsed_dispatches;
    uint64_t parse_failures;
    uint64_t prompt_worker_timeouts;
    uint64_t prompt_worker_failures;
    uint64_t prompt_stale_results;
} shell_state;

static int g_signal_write_fd = -1;
static volatile sig_atomic_t g_sigchld_pending = 0;
static volatile sig_atomic_t g_sigint_pending = 0;
static volatile sig_atomic_t g_sigtstp_pending = 0;
static volatile sig_atomic_t g_sigwinch_pending = 0;
static volatile sig_atomic_t g_shutdown_pending = 0;
static gsh_positional_store g_interactive_positionals;

typedef struct pipeline_expansion_scope pipeline_expansion_scope;

static void reset_child_signals(void);
static void start_external(shell_state *state, simple_command *direct);
static void start_async_external(shell_state *state, simple_command *direct);
static void start_async_native_pipeline(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const pipeline_expansion_scope *scope);
static bool native_command_is_supported(shell_state *state);
static bool try_native_reactor_compound(shell_state *state);
static void start_native_compound(shell_state *state, size_t node_index);
static void continue_native_list(shell_state *state);
static void continue_native_and_or(shell_state *state);
static bool begin_native_list(shell_state *state);
static bool native_list_node_is_wait(const shell_state *state,
                                     size_t node_index);
static void abandon_pending_list(shell_state *state);
static const char *store_path_value(const gsh_variable_store *variables,
                                    const char *default_path);
static int open_redirect_path(const char *target,
                              gsh_token_kind operator_kind,
                              const gsh_shell_options *options,
                              mode_t creation_mode);

#ifdef GSH_FAULT_INJECTION
static char g_fault_name[64];
static unsigned long g_fault_trigger = 1;
static unsigned long g_fault_calls = 0;

static void initialize_fault_injection(void)
{
    const char *configuration = getenv("GSH_FAULT");
    const char *separator;
    size_t length;

    if (configuration == NULL || configuration[0] == '\0') {
        return;
    }
    separator = strchr(configuration, ':');
    length = separator != NULL ? (size_t)(separator - configuration)
                               : strlen(configuration);
    if (length == 0 || length >= sizeof(g_fault_name)) {
        return;
    }
    memcpy(g_fault_name, configuration, length);
    g_fault_name[length] = '\0';
    if (separator != NULL && separator[1] != '\0') {
        char *end;
        unsigned long trigger = strtoul(separator + 1, &end, 10);

        if (*end == '\0' && trigger > 0) {
            g_fault_trigger = trigger;
        }
    }
}

static bool fault_should_fail(const char *name, int error)
{
    if (g_fault_name[0] == '\0' || strcmp(g_fault_name, name) != 0) {
        return false;
    }
    g_fault_calls++;
    if (g_fault_calls != g_fault_trigger) {
        return false;
    }
    errno = error;
    return true;
}

static bool fault_injection_active(void)
{
    return g_fault_name[0] != '\0';
}
#else
static void initialize_fault_injection(void)
{
}

static bool fault_should_fail(const char *name, int error)
{
    (void)name;
    (void)error;
    return false;
}


static bool fault_injection_active(void)
{
    return false;
}
#endif

static int ensure_alias_state(shell_state *state, bool transaction)
{
    if (state->aliases == NULL) {
        state->aliases = fault_should_fail("alias-allocation", ENOMEM)
                             ? NULL
                             : malloc(sizeof(*state->aliases));
        if (state->aliases == NULL) {
            return -1;
        }
        gsh_aliases_initialize(state->aliases);
    }
    if (state->alias_expansion == NULL) {
        state->alias_expansion =
            fault_should_fail("alias-allocation", ENOMEM)
                ? NULL
                : malloc(GSH_ALIAS_EXPANSION_CAP);
        if (state->alias_expansion == NULL) {
            return -1;
        }
    }
    if (!transaction) {
        return 0;
    }
    if (state->alias_scratch == NULL) {
        state->alias_scratch =
            fault_should_fail("alias-transaction-allocation", ENOMEM)
                ? NULL
                : malloc(sizeof(*state->alias_scratch));
        if (state->alias_scratch == NULL) {
            return -1;
        }
    }
    if (state->alias_commit == NULL) {
        state->alias_commit =
            fault_should_fail("alias-transaction-allocation", ENOMEM)
                ? NULL
                : malloc(sizeof(*state->alias_commit));
        if (state->alias_commit == NULL) {
            return -1;
        }
    }
    return 0;
}

static int ensure_function_state(shell_state *state, bool scratch)
{
    if (state->functions == NULL) {
        state->functions = fault_should_fail("function-allocation", ENOMEM)
                               ? NULL
                               : malloc(sizeof(*state->functions));
        if (state->functions == NULL) {
            return -1;
        }
        gsh_functions_initialize(state->functions);
    }
    if (scratch && state->function_scratch == NULL) {
        state->function_scratch =
            fault_should_fail("function-compact-allocation", ENOMEM)
                ? NULL
                : malloc(sizeof(*state->function_scratch));
        if (state->function_scratch == NULL) {
            return -1;
        }
        gsh_functions_initialize(state->function_scratch);
    }
    return 0;
}

static void reset_pending_input(shell_state *state)
{
    state->pending_input = state->pending_line;
    state->pending_input_length = strlen(state->pending_line);
    state->pending_alias_expanded = false;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (fault_should_fail("time-source-failure", EIO) ||
        clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void signal_handler(int signo)
{
    int saved_errno = errno;
    unsigned char byte = (unsigned char)signo;

    if (signo == SIGCHLD) {
        g_sigchld_pending = 1;
    } else if (signo == SIGINT) {
        g_sigint_pending = 1;
    } else if (signo == SIGTSTP) {
        g_sigtstp_pending = 1;
    } else if (signo == SIGWINCH) {
        g_sigwinch_pending = 1;
    } else {
        g_shutdown_pending = 1;
    }

    if (g_signal_write_fd >= 0) {
        ssize_t notified = write(g_signal_write_fd, &byte, sizeof(byte));

        (void)notified;
    }
    errno = saved_errno;
}

static int set_fd_flags(int fd, int command, int flag)
{
    int value = fcntl(fd, command);

    if (value == -1 || fcntl(fd, command == F_GETFL ? F_SETFL : F_SETFD,
                             value | flag) == -1) {
        return -1;
    }
    return 0;
}

static int make_pipe(int descriptors[2], bool nonblocking,
                     const char *fault_name)
{
    if (fault_should_fail(fault_name, EMFILE) || pipe(descriptors) == -1) {
        return -1;
    }
    if (set_fd_flags(descriptors[0], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(descriptors[1], F_GETFD, FD_CLOEXEC) == -1 ||
        (nonblocking &&
         (set_fd_flags(descriptors[0], F_GETFL, O_NONBLOCK) == -1 ||
          set_fd_flags(descriptors[1], F_GETFL, O_NONBLOCK) == -1))) {
        int saved_errno = errno;
        close(descriptors[0]);
        close(descriptors[1]);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static bool raw_output_push(shell_state *state, const char *data,
                            size_t length)
{
    if (length > OUTPUT_CAP - state->output_len) {
        state->overloads++;
        return false;
    }

    if (state->output_offset + state->output_len + length > OUTPUT_CAP) {
        memmove(state->output, state->output + state->output_offset,
                state->output_len);
        state->output_offset = 0;
    }
    memcpy(state->output + state->output_offset + state->output_len, data,
           length);
    state->output_len += length;
    return true;
}

static bool output_push(shell_state *state, const char *data, size_t length)
{
    if (state->async_repl != NULL && state->async_repl->enabled &&
        state->async_capture_cell >= 0) {
        return gsh_async_repl_append(state->async_repl,
                                     state->async_capture_cell, data,
                                     length) >= 0;
    }
    return raw_output_push(state, data, length);
}

static int reactor_builtin_output(void *opaque, int descriptor,
                                  const char *text, size_t length)
{
    (void)descriptor;
    return output_push(opaque, text, length) ? 0 : 1;
}

static bool output_text(shell_state *state, const char *text)
{
    return output_push(state, text, strlen(text));
}

static void output_format(shell_state *state, const char *format, ...)
{
    char message[512];
    va_list arguments;
    int length;

    va_start(arguments, format);
    length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    if (length <= 0) {
        return;
    }
    if ((size_t)length >= sizeof(message)) {
        length = (int)sizeof(message) - 1;
    }
    (void)output_push(state, message, (size_t)length);
}

static void flush_output(shell_state *state)
{
    unsigned int writes = 0;

    while (state->output_len > 0 && writes < 4) {
        size_t chunk = state->output_len;
        ssize_t written;

        if (chunk > 4096) {
            chunk = 4096;
        }
        written = fault_should_fail("output-write", EIO)
                      ? -1
                      : write(state->tty_fd,
                              state->output + state->output_offset, chunk);
        if (written > 0) {
            state->output_offset += (size_t)written;
            state->output_len -= (size_t)written;
            writes++;
            if (state->output_len == 0) {
                state->output_offset = 0;
            }
            continue;
        }
        if (written == -1 && errno == EINTR) {
            writes++;
            continue;
        }
        if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        state->last_status = 1;
        state->running = false;
        return;
    }
}

static void queue_prompt(shell_state *state)
{
    if (state->async_repl != NULL && state->async_repl->enabled) {
        state->async_repl->render_pending = true;
        return;
    }
    if (state->continuation_prompt) {
        const char *secondary = getenv("PS2");
        size_t length = 0;

        if (secondary == NULL) {
            secondary = "> ";
        }
        while (length < SECONDARY_PROMPT_CAP &&
               secondary[length] != '\0') {
            length++;
        }
        if (length == SECONDARY_PROMPT_CAP) {
            secondary = "> ";
            length = 2;
        }
        (void)output_push(state, secondary, length);
        return;
    }
    if (state->prompt_branch[0] != '\0') {
        (void)output_text(state, "[");
        (void)output_text(state, state->prompt_branch);
        (void)output_text(state, "] ");
    }
    (void)output_text(state, PROMPT);
}

static void queue_redraw(shell_state *state)
{
    if (state->async_repl != NULL && state->async_repl->enabled) {
        state->async_repl->render_pending = true;
        return;
    }
    (void)output_text(state, "\r\033[2K");
    queue_prompt(state);
    (void)output_push(state, state->line, state->line_len);
}

static size_t active_prompt_text(shell_state *state,
                                 char prompt[GSH_ASYNC_PROMPT_CAP])
{
    size_t length = 0;
    const char *secondary;

    if (state->continuation_prompt) {
        secondary = getenv("PS2");
        if (secondary == NULL) {
            secondary = "> ";
        }
        length = strlen(secondary);
        if (length >= GSH_ASYNC_PROMPT_CAP) {
            secondary = "> ";
            length = 2;
        }
        memcpy(prompt, secondary, length);
    } else {
        if (state->prompt_branch[0] != '\0') {
            int written = snprintf(prompt, GSH_ASYNC_PROMPT_CAP,
                                   "[%s] ", state->prompt_branch);

            if (written < 0 || written >= GSH_ASYNC_PROMPT_CAP) {
                length = 0;
            } else {
                length = (size_t)written;
            }
        }
        if (sizeof(PROMPT) - 1U <= GSH_ASYNC_PROMPT_CAP - length) {
            memcpy(prompt + length, PROMPT, sizeof(PROMPT) - 1U);
            length += sizeof(PROMPT) - 1U;
        }
    }
    prompt[length] = '\0';
    return length;
}

static void prepare_managed_render(shell_state *state)
{
    char prompt[GSH_ASYNC_PROMPT_CAP];
    const char *render;
    size_t length;

    if (state->async_repl == NULL || !state->async_repl->enabled ||
        !state->async_repl->render_pending || state->output_len != 0) {
        return;
    }
    (void)active_prompt_text(state, prompt);
    if (gsh_async_repl_prepare_render(state->async_repl, prompt,
                                      state->line, state->line_len) == -1) {
        state->last_status = 1;
        state->running = false;
        return;
    }
    render = gsh_async_repl_render_data(state->async_repl);
    length = gsh_async_repl_render_length(state->async_repl);
    if (!raw_output_push(state, render, length)) {
        state->running = false;
        return;
    }
    gsh_async_repl_rendered(state->async_repl);
}

static void make_editor_modes(shell_state *state)
{
    state->editor_modes = state->original_modes;
    state->editor_modes.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    if (state->async_repl != NULL && state->async_repl->enabled) {
        state->editor_modes.c_lflag &= (tcflag_t)~ISIG;
        state->editor_modes.c_cc[VINTR] = _POSIX_VDISABLE;
        state->editor_modes.c_cc[VSUSP] = _POSIX_VDISABLE;
#ifdef VDSUSP
        state->editor_modes.c_cc[VDSUSP] = _POSIX_VDISABLE;
#endif
    }
    state->editor_modes.c_iflag &= (tcflag_t)~(ICRNL | IXON);
    state->editor_modes.c_cc[VMIN] = 1;
    state->editor_modes.c_cc[VTIME] = 0;
}

static int enter_editor(shell_state *state)
{
    if (tcsetpgrp(state->tty_fd, state->shell_pgid) == -1) {
        return -1;
    }
    if (tcsetattr(state->tty_fd, TCSANOW, &state->editor_modes) == -1) {
        return -1;
    }
    state->terminal_changed = true;
    state->mode = MODE_EDITOR;
    return 0;
}

static void restore_terminal(shell_state *state)
{
    if (state->tty_fd < 0 || !state->terminal_changed) {
        return;
    }
    (void)tcsetpgrp(state->tty_fd, state->shell_pgid);
    (void)tcsetattr(state->tty_fd, TCSANOW, &state->original_modes);
    state->terminal_changed = false;
}

static int install_handler(int signo, void (*handler)(int), int flags)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    action.sa_flags = flags;
    sigemptyset(&action.sa_mask);
    return sigaction(signo, &action, NULL);
}

static int install_signal_handlers(void)
{
    if (install_handler(SIGCHLD, signal_handler, SA_RESTART) == -1 ||
        install_handler(SIGINT, signal_handler, 0) == -1 ||
        install_handler(SIGWINCH, signal_handler, SA_RESTART) == -1 ||
        install_handler(SIGHUP, signal_handler, 0) == -1 ||
        install_handler(SIGTERM, signal_handler, 0) == -1 ||
        install_handler(SIGQUIT, SIG_IGN, 0) == -1 ||
        install_handler(SIGTSTP, signal_handler, 0) == -1 ||
        install_handler(SIGTTIN, SIG_IGN, 0) == -1 ||
        install_handler(SIGTTOU, SIG_IGN, 0) == -1 ||
        install_handler(SIGPIPE, SIG_IGN, 0) == -1) {
        return -1;
    }
    return 0;
}

static bool managed_repl_requested(void)
{
    const char *mode = getenv("GSH_REPL");

    return mode == NULL || strcmp(mode, "classic") != 0;
}

static void initialize_repl_size(shell_state *state)
{
    struct winsize size;

    if (state->async_repl == NULL || !state->async_repl->enabled) {
        return;
    }
    memset(&size, 0, sizeof(size));
    if (ioctl(state->tty_fd, TIOCGWINSZ, &size) == -1) {
        gsh_async_repl_resize(state->async_repl, 24, 80);
        return;
    }
    gsh_async_repl_resize(state->async_repl, size.ws_row, size.ws_col);
}

static int initialize_interactive(shell_state *state)
{
    const char *terminal_name;
    size_t default_path_size;
    pid_t foreground_group;
    pid_t current_group;

    memset(state, 0, sizeof(*state));
    state->pending_input = state->pending_line;
    state->tty_fd = -1;
    state->signal_pipe[0] = -1;
    state->signal_pipe[1] = -1;
    state->prompt_worker_fd = -1;
    state->prompt_worker_pid = -1;
    state->variable_commit_fd = -1;
    state->directory_commit_socket = -1;
    state->directory_commit_fd = -1;
    state->async_capture_cell = -1;
    state->async_state_cell = -1;
    state->async_dispatch_cell = -1;
    state->running = true;
    state->last_status = 0;
    gsh_options_initialize(&state->options, true);
    gsh_background_initialize(&state->background_jobs);
    state->prompt_generation = 1;
    state->prompt_next_request_id = 1;
    state->parse_storage = fault_should_fail("allocation", ENOMEM)
                               ? NULL
                               : malloc(sizeof(*state->parse_storage));
    if (state->parse_storage == NULL) {
        return -1;
    }
    state->native_pipeline = fault_should_fail("allocation", ENOMEM)
                                 ? NULL
                                 : malloc(sizeof(*state->native_pipeline));
    if (state->native_pipeline == NULL) {
        return -1;
    }
    state->variables = fault_should_fail("allocation", ENOMEM)
                           ? NULL
                           : malloc(sizeof(*state->variables));
    state->variable_scratch = fault_should_fail("allocation", ENOMEM)
                                  ? NULL
                                  : malloc(sizeof(*state->variable_scratch));
    state->pipeline_variables = fault_should_fail("allocation", ENOMEM)
                                    ? NULL
                                    : malloc(sizeof(*state->pipeline_variables));
    state->variable_commit = fault_should_fail("allocation", ENOMEM)
                                 ? NULL
                                 : malloc(sizeof(*state->variable_commit));
    state->pipeline_changes = fault_should_fail("allocation", ENOMEM)
                                  ? NULL
                                  : malloc(sizeof(*state->pipeline_changes));
    state->async_repl = fault_should_fail("allocation", ENOMEM)
                            ? NULL
                            : malloc(sizeof(*state->async_repl));
    if (state->variables == NULL || state->variable_scratch == NULL ||
        state->pipeline_variables == NULL ||
        state->variable_commit == NULL || state->pipeline_changes == NULL ||
        state->async_repl == NULL ||
        gsh_variables_import(state->variables, environ) == -1) {
        return -1;
    }
    gsh_async_repl_initialize(state->async_repl, managed_repl_requested());
    state->variable_generation = 1;
    state->alias_generation = 1;
    state->function_generation = 1;
    gsh_variable_journal_initialize(state->variable_commit,
                                    state->variable_generation);

    default_path_size =
        confstr(_CS_PATH, state->default_path, sizeof(state->default_path));
    if (default_path_size == 0 ||
        default_path_size > sizeof(state->default_path) ||
        state->default_path[0] == '\0') {
        memcpy(state->default_path, "/bin:/usr/bin", 14);
    }
    if (getcwd(state->current_directory,
               sizeof(state->current_directory)) == NULL) {
        state->current_directory[0] = '\0';
    }

    terminal_name = ttyname(STDIN_FILENO);
    if (terminal_name == NULL) {
        return -1;
    }
    state->tty_fd = fault_should_fail("tty-open", EMFILE)
                        ? -1
                        : open(terminal_name,
                               O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (state->tty_fd == -1 ||
        set_fd_flags(state->tty_fd, F_GETFD, FD_CLOEXEC) == -1) {
        return -1;
    }

    current_group = getpgrp();
    for (;;) {
        foreground_group = tcgetpgrp(state->tty_fd);
        if (foreground_group == -1) {
            return -1;
        }
        if (foreground_group == current_group) {
            break;
        }
        if (kill(-current_group, SIGTTIN) == -1) {
            return -1;
        }
        current_group = getpgrp();
    }

    state->shell_pgid = getpid();
    if (setpgid(0, state->shell_pgid) == -1 &&
        !(errno == EACCES || errno == EPERM)) {
        return -1;
    }
    if (tcsetpgrp(state->tty_fd, state->shell_pgid) == -1 ||
        tcgetattr(state->tty_fd, &state->original_modes) == -1) {
        return -1;
    }
    make_editor_modes(state);

    if (make_pipe(state->signal_pipe, true, "signal-pipe") == -1) {
        return -1;
    }
    g_signal_write_fd = state->signal_pipe[1];
    if (install_signal_handlers() == -1 || enter_editor(state) == -1) {
        return -1;
    }
    initialize_repl_size(state);
    return 0;
}

static void reset_child_signals(void)
{
    const int signals[] = {SIGCHLD, SIGINT,  SIGWINCH, SIGHUP, SIGTERM,
                           SIGQUIT, SIGTSTP, SIGTTIN,  SIGTTOU, SIGPIPE};
    size_t index;

    for (index = 0; index < sizeof(signals) / sizeof(signals[0]); index++) {
        struct sigaction action;

        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        (void)sigaction(signals[index], &action, NULL);
    }
}

static bool prompt_path(char destination[PATH_MAX], const char *directory,
                        const char *suffix)
{
    size_t directory_length = strnlen(directory, PATH_MAX);
    size_t suffix_length = strlen(suffix);
    bool needs_separator;

    if (directory_length == PATH_MAX) {
        return false;
    }
    needs_separator = directory_length == 0 ||
                      directory[directory_length - 1] != '/';
    if (directory_length + (needs_separator ? 1U : 0U) + suffix_length + 1U >
        PATH_MAX) {
        return false;
    }
    memcpy(destination, directory, directory_length);
    if (needs_separator) {
        destination[directory_length++] = '/';
    }
    memcpy(destination + directory_length, suffix, suffix_length + 1U);
    return true;
}

static void prompt_worker_find_branch(const char *requested_directory,
                                      char branch[PROMPT_BRANCH_CAP])
{
    static const char prefix[] = "ref: refs/heads/";
    char directory[PATH_MAX];
    unsigned int ancestor;

    branch[0] = '\0';
    if (strnlen(requested_directory, sizeof(directory)) >= sizeof(directory)) {
        return;
    }
    memcpy(directory, requested_directory, strlen(requested_directory) + 1U);

    for (ancestor = 0; ancestor < PROMPT_ANCESTOR_CAP; ancestor++) {
        char head_path[PATH_MAX];
        char head[512];
        ssize_t length;
        int fd;

        if (!prompt_path(head_path, directory, ".git/HEAD")) {
            return;
        }
        fd = fault_should_fail("worker-open", EIO)
                 ? -1
                 : open(head_path, O_RDONLY);
        if (fd >= 0) {
            do {
                length = read(fd, head, sizeof(head) - 1U);
            } while (length == -1 && errno == EINTR);
            close(fd);
            if (length > 0) {
                size_t prefix_length = sizeof(prefix) - 1U;
                size_t index;
                size_t output = 0;

                head[(size_t)length] = '\0';
                if ((size_t)length < prefix_length ||
                    memcmp(head, prefix, prefix_length) != 0) {
                    return;
                }
                for (index = prefix_length; index < (size_t)length &&
                                            output + 1U < PROMPT_BRANCH_CAP;
                     index++) {
                    unsigned char byte = (unsigned char)head[index];

                    if (byte == '\n' || byte == '\r') {
                        break;
                    }
                    if (byte < 0x20U || byte == 0x7fU) {
                        branch[0] = '\0';
                        return;
                    }
                    branch[output++] = (char)byte;
                }
                branch[output] = '\0';
                return;
            }
        }

        {
            char *separator = strrchr(directory, '/');

            if (separator == NULL) {
                return;
            }
            if (separator == directory) {
                if (directory[1] == '\0') {
                    return;
                }
                directory[1] = '\0';
            } else {
                *separator = '\0';
            }
        }
    }
}

static void prompt_worker_loop(int fd)
{
    for (;;) {
        prompt_request request;
        prompt_result result;
        ssize_t received;

        do {
            received = recv(fd, &request, sizeof(request), 0);
        } while (received == -1 && errno == EINTR);
        if (received != (ssize_t)sizeof(request)) {
            _exit(received == -1 ? 1 : 0);
        }
        if (request.version != PROMPT_PROTOCOL_VERSION ||
            (request.type != PROMPT_REQUEST_BRANCH &&
             request.type != PROMPT_REQUEST_REDIRECTION) ||
            memchr(request.directory, '\0', sizeof(request.directory)) ==
                NULL) {
            _exit(1);
        }
        if (fault_should_fail("worker-crash", EIO)) {
            _exit(70);
        }
        if (fault_should_fail("worker-close", EPIPE)) {
            _exit(0);
        }
        if (fault_should_fail("worker-stall", ETIMEDOUT)) {
            for (;;) {
                pause();
            }
        }

        memset(&result, 0, sizeof(result));
        result.version = PROMPT_PROTOCOL_VERSION;
        result.type = request.type;
        result.request_id = request.request_id;
        result.generation = request.generation;
        if (request.type == PROMPT_REQUEST_BRANCH &&
            monotonic_ns() < request.deadline_ns) {
            prompt_worker_find_branch(request.directory, result.branch);
        } else if (request.type == PROMPT_REQUEST_REDIRECTION) {
            gsh_shell_options options = {request.option_bits};
            int descriptor = open_redirect_path(
                request.directory, (gsh_token_kind)request.operator_kind,
                &options, (mode_t)request.creation_mode);

            if (descriptor == -1) {
                result.status = 1;
                result.error = errno;
            } else {
                close(descriptor);
                result.status = request.builtin_status;
            }
        }
        if (fault_should_fail("worker-malformed", EPROTO)) {
            result.version++;
        }
        if (fault_should_fail("worker-stale", ESTALE)) {
            result.generation++;
        }
        do {
            received = send(fd, &result, sizeof(result), 0);
        } while (received == -1 && errno == EINTR);
        if (received != (ssize_t)sizeof(result)) {
            _exit(1);
        }
    }
}

static void disable_prompt_worker(shell_state *state, bool terminate)
{
    if (state->prompt_worker_fd >= 0) {
        close(state->prompt_worker_fd);
        state->prompt_worker_fd = -1;
    }
    if (terminate && state->prompt_worker_pid > 0) {
        (void)kill(state->prompt_worker_pid, SIGKILL);
    }
    state->prompt_worker_alive = false;
    state->prompt_worker_busy = false;
    state->prompt_request_pending = false;
    state->prompt_worker_deadline_ns = 0;
    state->prompt_active_request_id = 0;
    state->prompt_active_request_type = 0;
}

static int start_prompt_worker(shell_state *state)
{
    int sockets[2];
    sigset_t blocked;
    sigset_t previous;
    pid_t pid;

    if (fault_should_fail("worker-socket", EMFILE) ||
        socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == -1) {
        return -1;
    }
    if (set_fd_flags(sockets[0], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(sockets[1], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(sockets[0], F_GETFL, O_NONBLOCK) == -1) {
        int saved_errno = errno;

        close(sockets[0]);
        close(sockets[1]);
        errno = saved_errno;
        return -1;
    }

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        int saved_errno = errno;

        close(sockets[0]);
        close(sockets[1]);
        errno = saved_errno;
        return -1;
    }

    pid = fault_should_fail("worker-fork", EAGAIN) ? -1 : fork();
    if (pid == 0) {
        close(sockets[0]);
        (void)setpgid(0, 0);
        reset_child_signals();
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        close(state->tty_fd);
        close(state->signal_pipe[0]);
        close(state->signal_pipe[1]);
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        (void)umask(0);
        prompt_worker_loop(sockets[1]);
    }

    close(sockets[1]);
    if (pid == -1) {
        int saved_errno = errno;

        close(sockets[0]);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        errno = saved_errno;
        return -1;
    }

    (void)setpgid(pid, pid);
    state->prompt_worker_fd = sockets[0];
    state->prompt_worker_pid = pid;
    state->prompt_worker_alive = true;
    state->prompt_request_pending = state->current_directory[0] != '\0';
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
    return 0;
}

static void schedule_prompt_refresh(shell_state *state)
{
    state->prompt_generation++;
    state->prompt_branch[0] = '\0';
    if (state->prompt_worker_alive && state->current_directory[0] != '\0') {
        state->prompt_request_pending = true;
    }
}

static void send_prompt_request(shell_state *state)
{
    prompt_request request;
    ssize_t sent;

    if (!state->prompt_worker_alive || state->prompt_worker_busy ||
        !state->prompt_request_pending || state->output_len != 0) {
        return;
    }
    memset(&request, 0, sizeof(request));
    request.version = PROMPT_PROTOCOL_VERSION;
    request.type = PROMPT_REQUEST_BRANCH;
    request.request_id = state->prompt_next_request_id++;
    request.generation = state->prompt_generation;
    request.deadline_ns = monotonic_ns() + PROMPT_WORKER_DEADLINE_NS;
    memcpy(request.directory, state->current_directory,
           strlen(state->current_directory) + 1U);

    do {
        sent = fault_should_fail("worker-send", EPIPE)
                   ? -1
                   : send(state->prompt_worker_fd, &request, sizeof(request),
                          0);
    } while (sent == -1 && errno == EINTR);
    if (sent == (ssize_t)sizeof(request)) {
        state->prompt_request_pending = false;
        state->prompt_worker_busy = true;
        state->prompt_active_request_id = request.request_id;
        state->prompt_active_request_type = request.type;
        state->prompt_worker_deadline_ns = request.deadline_ns;
    } else if (sent == -1 &&
               (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
        return;
    } else {
        state->prompt_worker_failures++;
        disable_prompt_worker(state, true);
    }
}

static void receive_prompt_result(shell_state *state)
{
    prompt_result result;
    ssize_t received;

    do {
        received = recv(state->prompt_worker_fd, &result, sizeof(result), 0);
    } while (received == -1 && errno == EINTR);
    if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    if (received != (ssize_t)sizeof(result) ||
        result.version != PROMPT_PROTOCOL_VERSION ||
        result.type != state->prompt_active_request_type ||
        result.request_id != state->prompt_active_request_id ||
        (result.type == PROMPT_REQUEST_BRANCH &&
         memchr(result.branch, '\0', sizeof(result.branch)) == NULL)) {
        bool command = state->mode == MODE_ASYNC_WORKER;

        state->prompt_worker_failures++;
        disable_prompt_worker(state, true);
        if (command) {
            output_text(state, "gsh: asynchronous redirection failed\r\n");
            state->last_status = 1;
            state->mode = MODE_EDITOR;
            queue_prompt(state);
        }
        return;
    }

    state->prompt_worker_busy = false;
    state->prompt_worker_deadline_ns = 0;
    state->prompt_active_request_id = 0;
    state->prompt_active_request_type = 0;
    if (result.type == PROMPT_REQUEST_REDIRECTION) {
        int status = result.status;

        if (result.error != 0) {
            output_format(state, "gsh: %s: %s\r\n",
                          state->worker_redirection_target,
                          strerror(result.error));
        }
        if (state->worker_pipeline_negated) {
            status = status == 0 ? 1 : 0;
        }
        state->last_status = status;
        state->mode = MODE_EDITOR;
        state->worker_redirection_target[0] = '\0';
        queue_prompt(state);
        return;
    }
    if (result.generation != state->prompt_generation) {
        state->prompt_stale_results++;
        return;
    }
    if (strcmp(state->prompt_branch, result.branch) != 0) {
        memcpy(state->prompt_branch, result.branch, sizeof(result.branch));
        if (state->mode == MODE_EDITOR) {
            queue_redraw(state);
        }
    }
}

static int prompt_poll_timeout(const shell_state *state)
{
    uint64_t now;
    uint64_t remaining;
    uint64_t milliseconds;

    if (!state->prompt_worker_busy) {
        return -1;
    }
    if (state->prompt_active_request_type ==
        PROMPT_REQUEST_REDIRECTION) {
        return -1;
    }
    now = monotonic_ns();
    if (now >= state->prompt_worker_deadline_ns) {
        return 0;
    }
    remaining = state->prompt_worker_deadline_ns - now;
    milliseconds = (remaining + 999999U) / 1000000U;
    return milliseconds > (uint64_t)INT_MAX ? INT_MAX : (int)milliseconds;
}

static void enforce_prompt_deadline(shell_state *state)
{
    if (state->prompt_worker_busy &&
        state->prompt_active_request_type == PROMPT_REQUEST_BRANCH &&
        monotonic_ns() >= state->prompt_worker_deadline_ns) {
        state->prompt_worker_timeouts++;
        disable_prompt_worker(state, true);
    }
}

static void reclaim_terminal(shell_state *state, bool save_job_modes)
{
    (void)tcsetpgrp(state->tty_fd, state->shell_pgid);
    if (save_job_modes) {
        (void)tcgetattr(state->tty_fd, &state->current_job.modes);
    }
    if (tcsetattr(state->tty_fd, TCSANOW, &state->editor_modes) == -1) {
        state->running = false;
        return;
    }
    state->terminal_changed = true;
    state->mode = MODE_EDITOR;
}

enum {
    JOB_MEMBER_RUNNING,
    JOB_MEMBER_STOPPED,
    JOB_MEMBER_DONE,
};

static void initialize_job(job *current, pid_t pgid, pid_t status_pid,
                           const pid_t *members, size_t member_count,
                           bool foreground, bool negated)
{
    size_t index;

    memset(current, 0, sizeof(*current));
    current->active = true;
    current->foreground = foreground;
    current->pid = pgid;
    current->pgid = pgid;
    current->status_pid = status_pid;
    current->member_count = member_count;
    current->remaining = member_count;
    current->negated = negated;
    for (index = 0; index < member_count; index++) {
        current->members[index] = members[index];
        current->member_states[index] = JOB_MEMBER_RUNNING;
    }
}

static size_t find_job_member(const job *current, pid_t pid)
{
    size_t index;

    for (index = 0; index < current->member_count; index++) {
        if (current->members[index] == pid) {
            return index;
        }
    }
    return GSH_NATIVE_JOB_MEMBER_CAP;
}

static bool all_remaining_members_stopped(const job *current)
{
    size_t index;

    if (current->remaining == 0) {
        return false;
    }
    for (index = 0; index < current->member_count; index++) {
        if (current->member_states[index] == JOB_MEMBER_RUNNING) {
            return false;
        }
    }
    return true;
}

static int wait_status_value(int status)
{
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

static int send_directory_descriptor(int socket)
{
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    struct iovec vector;
    struct msghdr message;
    struct cmsghdr *header;
    unsigned char marker = 1;
    int descriptor;
    ssize_t sent;

    descriptor = fault_should_fail("directory-commit-open", EMFILE)
                     ? -1
                     : open(".", O_RDONLY);
    if (descriptor == -1) {
        return -1;
    }
    memset(&control, 0, sizeof(control));
    memset(&message, 0, sizeof(message));
    vector.iov_base = &marker;
    vector.iov_len = sizeof(marker);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(descriptor));
    memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
    do {
        sent = fault_should_fail("directory-commit-send", EIO)
                   ? -1
                   : sendmsg(socket, &message, 0);
    } while (sent == -1 && errno == EINTR);
    close(descriptor);
    return sent == (ssize_t)sizeof(marker) ? 0 : -1;
}

static int receive_directory_descriptor(shell_state *state)
{
    union {
        struct cmsghdr alignment;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    struct iovec vector;
    struct msghdr message;
    struct cmsghdr *header;
    unsigned char marker = 0;
    int descriptor = -1;
    ssize_t received;

    if (!state->directory_commit_expected) {
        return 0;
    }
    if (state->directory_commit_socket < 0) {
        errno = EPROTO;
        return -1;
    }
    memset(&control, 0, sizeof(control));
    memset(&message, 0, sizeof(message));
    vector.iov_base = &marker;
    vector.iov_len = sizeof(marker);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    do {
        received = fault_should_fail("directory-commit-receive", EIO)
                       ? -1
                       : recvmsg(state->directory_commit_socket, &message,
                                 0);
    } while (received == -1 && errno == EINTR);
    close(state->directory_commit_socket);
    state->directory_commit_socket = -1;
    header = CMSG_FIRSTHDR(&message);
    if (header != NULL && header->cmsg_level == SOL_SOCKET &&
        header->cmsg_type == SCM_RIGHTS &&
        header->cmsg_len >= CMSG_LEN(sizeof(descriptor))) {
        memcpy(&descriptor, CMSG_DATA(header), sizeof(descriptor));
    }
    if (received != (ssize_t)sizeof(marker) || marker != 1 ||
        (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0 ||
        header == NULL || header->cmsg_level != SOL_SOCKET ||
        header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(descriptor)) ||
        CMSG_NXTHDR(&message, header) != NULL) {
        if (descriptor >= 0) {
            close(descriptor);
        }
        errno = EPROTO;
        return -1;
    }
    if (descriptor < 0 ||
        set_fd_flags(descriptor, F_GETFD, FD_CLOEXEC) == -1) {
        int saved_errno = errno;

        if (descriptor >= 0) {
            close(descriptor);
        }
        errno = saved_errno;
        return -1;
    }
    state->directory_commit_fd = descriptor;
    return 0;
}

static size_t state_commit_size(const shell_state *state)
{
    size_t size = sizeof(*state->variable_commit) +
                  (state->alias_commit_expected
                       ? sizeof(*state->alias_commit)
                       : 0U) +
                  (state->positional_commit_expected
                       ? sizeof(*state->positional_commit)
                       : 0U) +
                  sizeof(state->option_commit);

    if (state->function_commit_expected) {
        size += sizeof(state->function_commit_header);
        if (state->function_commit_header_complete &&
            gsh_functions_snapshot_header_valid(
                &state->function_commit_header)) {
            size += gsh_functions_snapshot_payload_size(
                &state->function_commit_header);
        }
    }
    return size;
}

static void close_variable_commit(shell_state *state)
{
    if (state->variable_commit_fd >= 0) {
        close(state->variable_commit_fd);
    }
    state->variable_commit_fd = -1;
    state->variable_commit_received = 0;
    state->variable_commit_active = false;
    state->variable_commit_eof = false;
    state->variable_commit_invalid = false;
    free(state->positional_commit);
    state->positional_commit = NULL;
    state->positional_commit_expected = false;
    state->pending_positional_commit = false;
    state->alias_commit_expected = false;
    state->pending_alias_commit = false;
    state->function_commit_expected = false;
    state->pending_function_commit = false;
    state->function_commit_header_complete = false;
    memset(&state->function_commit_header, 0,
           sizeof(state->function_commit_header));
    if (state->directory_commit_socket >= 0) {
        close(state->directory_commit_socket);
    }
    if (state->directory_commit_fd >= 0) {
        close(state->directory_commit_fd);
    }
    state->directory_commit_socket = -1;
    state->directory_commit_fd = -1;
    state->directory_commit_expected = false;
    state->pending_directory_commit = false;
}

static void receive_variable_commit(shell_state *state, bool drain_all)
{
    unsigned int reads = 0;
    unsigned int limit = drain_all ? 1024U : 4U;

    while (state->variable_commit_fd >= 0 && reads++ < limit) {
        unsigned char extra;
        void *destination;
        size_t capacity;
        ssize_t count;

        if (state->variable_commit_received < state_commit_size(state)) {
            size_t variable_size = sizeof(*state->variable_commit);
            size_t alias_size = state->alias_commit_expected
                                    ? sizeof(*state->alias_commit)
                                    : 0U;

            capacity = state_commit_size(state) -
                       state->variable_commit_received;
            if (capacity > 4096U) {
                capacity = 4096U;
            }
            size_t positional_size =
                state->positional_commit_expected
                    ? sizeof(*state->positional_commit)
                    : 0U;
            size_t option_begin = variable_size + alias_size +
                                  positional_size;
            size_t option_end = option_begin + sizeof(state->option_commit);

            if (state->variable_commit_received < variable_size) {
                size_t remaining = variable_size -
                                   state->variable_commit_received;

                if (capacity > remaining) {
                    capacity = remaining;
                }
                destination = (unsigned char *)state->variable_commit +
                              state->variable_commit_received;
            } else if (state->variable_commit_received <
                       variable_size + alias_size) {
                size_t remaining = variable_size + alias_size -
                                   state->variable_commit_received;

                if (capacity > remaining) {
                    capacity = remaining;
                }
                destination = (unsigned char *)state->alias_commit +
                              state->variable_commit_received -
                                  variable_size;
            } else if (state->variable_commit_received <
                       variable_size + alias_size + positional_size) {
                size_t remaining = variable_size + alias_size +
                                   positional_size -
                                   state->variable_commit_received;

                if (capacity > remaining) {
                    capacity = remaining;
                }
                destination = (unsigned char *)state->positional_commit +
                              state->variable_commit_received -
                                  variable_size - alias_size;
            } else if (state->variable_commit_received < option_end) {
                size_t remaining = option_end -
                                   state->variable_commit_received;

                if (capacity > remaining) {
                    capacity = remaining;
                }
                destination = (unsigned char *)&state->option_commit +
                              state->variable_commit_received -
                                  option_begin;
            } else if (state->function_commit_expected &&
                       state->variable_commit_received <
                           option_end +
                               sizeof(state->function_commit_header)) {
                size_t header_offset = state->variable_commit_received -
                                       option_end;
                size_t remaining = sizeof(state->function_commit_header) -
                                   header_offset;

                if (capacity > remaining) {
                    capacity = remaining;
                }
                destination =
                    (unsigned char *)&state->function_commit_header +
                    header_offset;
            } else if (state->function_commit_expected &&
                       state->function_commit_header_complete) {
                size_t payload_offset = state->variable_commit_received -
                                        option_end -
                                        sizeof(state->function_commit_header);
                size_t available;

                destination = gsh_functions_snapshot_destination(
                    state->function_scratch,
                    &state->function_commit_header, payload_offset,
                    &available);
                if (destination == NULL || available == 0) {
                    destination = &extra;
                    capacity = 1;
                    state->variable_commit_invalid = true;
                } else if (capacity > available) {
                    capacity = available;
                }
            } else {
                destination = &extra;
                capacity = 1;
                state->variable_commit_invalid = true;
            }
        } else {
            destination = &extra;
            capacity = 1;
        }
        count = fault_should_fail("state-commit-read", EIO)
                    ? -1
                    : read(state->variable_commit_fd, destination,
                           capacity);
        if (count > 0) {
            if (state->variable_commit_received >=
                state_commit_size(state)) {
                state->variable_commit_invalid = true;
            } else {
                state->variable_commit_received += (size_t)count;
                if (state->function_commit_expected &&
                    !state->function_commit_header_complete) {
                    size_t header_end = sizeof(*state->variable_commit) +
                        (state->alias_commit_expected
                             ? sizeof(*state->alias_commit)
                             : 0U) +
                        (state->positional_commit_expected
                             ? sizeof(*state->positional_commit)
                             : 0U) +
                        sizeof(state->option_commit) +
                        sizeof(state->function_commit_header);

                    if (state->variable_commit_received >= header_end) {
                        state->function_commit_header_complete = true;
                        if (!gsh_functions_snapshot_header_valid(
                                &state->function_commit_header) ||
                            state->function_commit_header.base_generation !=
                                state->function_generation) {
                            state->variable_commit_invalid = true;
                        }
                    }
                }
            }
            continue;
        }
        if (count == 0) {
            state->variable_commit_eof = true;
            close(state->variable_commit_fd);
            state->variable_commit_fd = -1;
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        state->variable_commit_invalid = true;
        close(state->variable_commit_fd);
        state->variable_commit_fd = -1;
        return;
    }
}

static bool finish_variable_commit(shell_state *state, int wait_status)
{
    bool valid;
    bool directory_changed = false;
    int previous_directory = -1;

    if (!state->variable_commit_active) {
        return true;
    }
    receive_variable_commit(state, true);
    valid = WIFEXITED(wait_status) && state->variable_commit_eof &&
            !state->variable_commit_invalid &&
            state->variable_commit_received == state_commit_size(state) &&
            gsh_variable_journal_validate(state->variable_commit) &&
            (!state->alias_commit_expected ||
             (gsh_alias_journal_validate(state->alias_commit) &&
              state->alias_commit->base_generation ==
                  state->alias_generation)) &&
            (!state->positional_commit_expected ||
             gsh_positionals_validate(state->positional_commit)) &&
            (!state->function_commit_expected ||
             (state->function_commit_header_complete &&
              state->function_commit_header.base_generation ==
                  state->function_generation &&
              gsh_functions_snapshot_finalize(
                  state->function_scratch,
                  &state->function_commit_header))) &&
            gsh_options_validate(&state->option_commit) &&
            gsh_options_enabled(&state->option_commit,
                                GSH_OPTION_INTERACTIVE) ==
                gsh_options_enabled(&state->options,
                                    GSH_OPTION_INTERACTIVE) &&
            state->variable_commit->base_generation ==
                state->variable_generation;
    if (valid && receive_directory_descriptor(state) == -1) {
        valid = false;
    }
    if (valid && state->directory_commit_expected) {
        previous_directory = open(".", O_RDONLY);
        if (previous_directory == -1 ||
            fault_should_fail("directory-commit-apply", EIO) ||
            fchdir(state->directory_commit_fd) == -1) {
            valid = false;
        } else {
            directory_changed = true;
        }
    }
    if (valid) {
        memcpy(state->variable_scratch, state->variables,
               sizeof(*state->variable_scratch));
        if (gsh_variables_apply_journal_in_place(
                state->variable_scratch, state->variable_commit) == -1) {
            valid = false;
        }
        if (valid && state->alias_commit_expected) {
            memcpy(state->alias_scratch, state->aliases,
                   sizeof(*state->alias_scratch));
            if (gsh_aliases_apply_journal_in_place(
                    state->alias_scratch, state->alias_commit) == -1) {
                valid = false;
            }
        }
    }
    if (valid) {
        memcpy(state->variables, state->variable_scratch,
               sizeof(*state->variables));
        if (state->alias_commit_expected) {
            memcpy(state->aliases, state->alias_scratch,
                   sizeof(*state->aliases));
            state->alias_generation++;
        }
        if (state->function_commit_expected) {
            gsh_function_store *previous = state->functions;

            state->functions = state->function_scratch;
            state->function_scratch = previous;
            state->function_generation++;
        }
        if (state->positional_commit_expected) {
            if (state->positionals == NULL) {
                state->positionals = &g_interactive_positionals;
            }
            memcpy(state->positionals, state->positional_commit,
                   sizeof(*state->positionals));
        }
        state->options = state->option_commit;
        if (directory_changed) {
            if (getcwd(state->current_directory,
                       sizeof(state->current_directory)) == NULL) {
                state->current_directory[0] = '\0';
            }
            schedule_prompt_refresh(state);
        }
        state->variable_generation++;
    } else if (WIFEXITED(wait_status) || state->variable_commit_invalid) {
        if (directory_changed) {
            if (fchdir(previous_directory) == -1) {
                int saved_errno = errno;

                output_format(state,
                              "gsh: directory rollback failed: %s\r\n",
                              strerror(saved_errno));
                state->running = false;
            }
        }
        output_text(state,
                    "gsh: state transaction rejected or incomplete\r\n");
        valid = false;
    }
    if (previous_directory >= 0) {
        close(previous_directory);
    }
    close_variable_commit(state);
    return valid;
}

static bool finish_background_wait(shell_state *state)
{
    size_t index;
    int status = state->wait_all ? 0 : 127;

    if (state->mode != MODE_WAIT) {
        return false;
    }
    for (index = 0; index < state->wait_target_count; index++) {
        bool done;

        if (state->wait_targets[index] > 0 &&
            gsh_background_get(&state->background_jobs,
                               state->wait_targets[index], &done, NULL) &&
            !done) {
            return false;
        }
    }
    for (index = 0; index < state->wait_target_count; index++) {
        int wait_status;

        if (state->wait_targets[index] > 0 &&
            gsh_background_consume(&state->background_jobs,
                                   state->wait_targets[index],
                                   &wait_status)) {
            if (!state->wait_all &&
                index + 1U == state->wait_target_count) {
                status = wait_status_value(wait_status);
            }
        } else if (!state->wait_all &&
                   index + 1U == state->wait_target_count) {
            status = 127;
        }
    }
    state->wait_target_count = 0;
    state->wait_all = false;
    state->last_status = state->wait_negated ? (status == 0 ? 1 : 0)
                                             : status;
    state->mode = MODE_EDITOR;
    state->wait_negated = false;
    if (state->pending_and_or_active) {
        continue_native_and_or(state);
    } else if (state->pending_list_active) {
        continue_native_list(state);
    } else {
        queue_prompt(state);
    }
    return true;
}

static void finish_job(shell_state *state)
{
    bool was_foreground = state->current_job.foreground;
    pid_t pid = state->current_job.pid;
    int wait_status = state->current_job.pipeline_wait_status;
    int status = state->current_job.pipeline_status_known
                     ? wait_status_value(wait_status)
                     : 1;
    bool silent = state->current_job.silent;

    if (state->current_job.pipeline_status_known &&
        !finish_variable_commit(state, wait_status)) {
        status = 125;
    }

    state->current_job.active = false;
    state->current_job.foreground = false;
    state->current_job.stopped = false;
    if (state->current_job.negated) {
        status = status == 0 ? 1 : 0;
    }
    state->last_status = status;

    if (state->async_repl != NULL && state->async_repl->enabled &&
        state->async_state_cell >= 0) {
        (void)gsh_async_repl_reap(state->async_repl, pid, wait_status);
    }

    if (was_foreground) {
        reclaim_terminal(state, false);
        if (state->current_job.pipeline_status_known &&
            WIFSIGNALED(wait_status)) {
            (void)output_text(state, "\r\n");
            if (WTERMSIG(wait_status) != SIGINT) {
                output_format(state, "[terminated by signal %d]\r\n",
                              WTERMSIG(wait_status));
            }
        }
        if (state->pending_and_or_active) {
            continue_native_and_or(state);
        } else if (state->pending_list_active) {
            continue_native_list(state);
        } else {
            queue_prompt(state);
        }
    } else if (!silent) {
        output_format(state, "\r\n[done %ld, status %d]\r\n", (long)pid,
                      state->last_status);
        if (state->mode == MODE_EDITOR) {
            queue_redraw(state);
        }
    }
}

static void update_job_state(shell_state *state, pid_t pid, int status)
{
    size_t member;

    if (!state->current_job.active) {
        return;
    }
    member = find_job_member(&state->current_job, pid);
    if (member == GSH_NATIVE_JOB_MEMBER_CAP) {
        return;
    }
    if (WIFSTOPPED(status)) {
        bool was_foreground;

        state->current_job.member_states[member] = JOB_MEMBER_STOPPED;
        if (!all_remaining_members_stopped(&state->current_job) ||
            state->current_job.stopped) {
            return;
        }
        was_foreground = state->current_job.foreground;
        state->current_job.stopped = true;
        state->current_job.foreground = false;
        state->last_status = 128 + WSTOPSIG(status);
        if (was_foreground) {
            reclaim_terminal(state, true);
            output_format(state, "\r\n[stopped %ld]\r\n",
                          (long)state->current_job.pid);
            queue_prompt(state);
        } else {
            output_format(state, "\r\n[stopped %ld]\r\n",
                          (long)state->current_job.pid);
            queue_redraw(state);
        }
        return;
    }
#ifdef WIFCONTINUED
    if (WIFCONTINUED(status)) {
        state->current_job.member_states[member] = JOB_MEMBER_RUNNING;
        state->current_job.stopped = false;
        return;
    }
#endif
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        if (state->current_job.member_states[member] != JOB_MEMBER_DONE) {
            state->current_job.member_states[member] = JOB_MEMBER_DONE;
            if (state->current_job.remaining > 0) {
                state->current_job.remaining--;
            }
        }
        if (pid == state->current_job.status_pid) {
            state->current_job.pipeline_wait_status = status;
            state->current_job.pipeline_status_known = true;
        }
        if (state->current_job.remaining == 0) {
            finish_job(state);
        }
    }
}

static void reap_children(shell_state *state)
{
    unsigned int count;
    int options = WNOHANG | WUNTRACED;

#ifdef WCONTINUED
    options |= WCONTINUED;
#endif
    for (count = 0; count < MAX_SIGNAL_REAPS; count++) {
        int status;
        pid_t pid = waitpid(-1, &status, options);

        if (pid > 0) {
            int async_cell = state->async_repl == NULL
                                 ? -1
                                 : gsh_async_repl_cell_for_pid(
                                       state->async_repl, pid);

            if (pid == state->prompt_worker_pid) {
                bool unexpected = state->prompt_worker_alive;
                bool command = state->mode == MODE_ASYNC_WORKER;
                bool restart = state->prompt_worker_restart_pending;

                disable_prompt_worker(state, false);
                state->prompt_worker_pid = -1;
                state->prompt_worker_restart_pending = false;
                if (unexpected) {
                    state->prompt_worker_failures++;
                }
                if (command) {
                    output_text(
                        state,
                        "gsh: asynchronous redirection worker exited\r\n");
                    state->last_status = 1;
                    state->mode = MODE_EDITOR;
                    queue_prompt(state);
                }
                if (restart && state->running &&
                    start_prompt_worker(state) == -1) {
                    state->prompt_worker_failures++;
                }
            } else if (state->current_job.active &&
                       find_job_member(&state->current_job, pid) !=
                           GSH_NATIVE_JOB_MEMBER_CAP) {
                update_job_state(state, pid, status);
            } else if (async_cell >= 0) {
                if (WIFSTOPPED(status)) {
                    gsh_async_repl_mark_stopped(state->async_repl,
                                                async_cell);
#ifdef WIFCONTINUED
                } else if (WIFCONTINUED(status)) {
                    gsh_async_repl_mark_running(state->async_repl,
                                                async_cell);
#endif
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    (void)gsh_async_repl_reap(state->async_repl, pid,
                                              status);
                }
            } else if ((WIFEXITED(status) || WIFSIGNALED(status)) &&
                       gsh_background_record(&state->background_jobs, pid,
                                             status)) {
                (void)finish_background_wait(state);
            }
            continue;
        }
        if (pid == -1 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (count == MAX_SIGNAL_REAPS) {
        unsigned char byte = (unsigned char)SIGCHLD;
        ssize_t notified;

        g_sigchld_pending = 1;
        notified = write(state->signal_pipe[1], &byte, sizeof(byte));
        (void)notified;
    }
}

static void resize_managed_jobs(shell_state *state)
{
    struct winsize size;
    int index;

    if (state->async_repl == NULL || !state->async_repl->enabled) {
        return;
    }
    memset(&size, 0, sizeof(size));
    if (ioctl(state->tty_fd, TIOCGWINSZ, &size) == -1) {
        return;
    }
    gsh_async_repl_resize(state->async_repl, size.ws_row, size.ws_col);
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        gsh_async_cell *cell = &state->async_repl->cells[index];

        if (cell->occupied && cell->pty_fd >= 0) {
            (void)ioctl(cell->pty_fd, TIOCSWINSZ, &size);
            if (cell->pgid > 0) {
                (void)kill(-cell->pgid, SIGWINCH);
            }
        }
    }
}

static void drain_signal_pipe(shell_state *state)
{
    unsigned char bytes[256];
    ssize_t drained;

    drained = read(state->signal_pipe[0], bytes, sizeof(bytes));
    (void)drained;
}

static void cancel_editor_line(shell_state *state)
{
    state->line_len = 0;
    state->line[0] = '\0';
    state->pending_len = 0;
    state->pending_line[0] = '\0';
    reset_pending_input(state);
    state->continuation_prompt = false;
    state->escape_state = 0;
    if (state->async_repl == NULL || !state->async_repl->enabled) {
        (void)output_text(state, "^C\r\n");
    }
    queue_prompt(state);
}

/* ── Managed Ctrl-Z Uses an Unconditional Stop ───────────────────
 * Each managed PTY job is a session leader, so its process group is orphaned.
 * POSIX permits an orphaned group to discard the terminal stop signal SIGTSTP.
 * The physical terminal never belongs to that group and cannot stop it for us.
 * SIGSTOP supplies the required managed Ctrl-Z transition deterministically.
 * TIOCSIG addresses the whole PTY group when cross-session kill is unavailable.
 * A leader fallback covers the short interval before that group is observable.
 * SIGCONT through bg or fg retains the ordinary observable resume behavior.
 * ─────────────────────────────────────────────────────────────── */
static pid_t managed_job_group(shell_state *state, int cell_index)
{
    gsh_async_cell *cell = &state->async_repl->cells[cell_index];
    pid_t foreground = cell->pty_fd < 0 ? -1 : tcgetpgrp(cell->pty_fd);

    if (foreground > 0) {
        cell->pgid = foreground;
    }
    return cell->pgid;
}

static int signal_managed_job(shell_state *state, int cell_index,
                              int signal_number)
{
    gsh_async_cell *cell = &state->async_repl->cells[cell_index];
    pid_t pgid = managed_job_group(state, cell_index);

#ifdef TIOCSIG
    if (cell->pty_fd >= 0 &&
        ioctl(cell->pty_fd, TIOCSIG, signal_number) == 0) {
        return 0;
    }
#endif
    if (pgid > 0 && kill(-pgid, signal_number) == 0) {
        return 0;
    }
    if (cell->pid > 0) {
        return kill(cell->pid, signal_number);
    }
    errno = ESRCH;
    return -1;
}

static void stop_managed_job(shell_state *state, int cell_index)
{
    if (signal_managed_job(state, cell_index, SIGSTOP) == 0) {
        gsh_async_repl_mark_stopped(state->async_repl, cell_index);
    } else {
        gsh_async_repl_unfocus(state->async_repl);
    }
}

static void process_pending_signals(shell_state *state)
{
    sigset_t signals;
    sigset_t previous;
    bool child;
    bool interrupt;
    bool suspend;
    bool resize;
    bool shutdown;

    sigemptyset(&signals);
    sigaddset(&signals, SIGCHLD);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTSTP);
    sigaddset(&signals, SIGWINCH);
    sigaddset(&signals, SIGHUP);
    sigaddset(&signals, SIGTERM);
    (void)sigprocmask(SIG_BLOCK, &signals, &previous);

    child = g_sigchld_pending != 0;
    interrupt = g_sigint_pending != 0;
    suspend = g_sigtstp_pending != 0;
    resize = g_sigwinch_pending != 0;
    shutdown = g_shutdown_pending != 0;
    g_sigchld_pending = 0;
    g_sigint_pending = 0;
    g_sigtstp_pending = 0;
    g_sigwinch_pending = 0;
    g_shutdown_pending = 0;

    (void)sigprocmask(SIG_SETMASK, &previous, NULL);

    if (child) {
        reap_children(state);
    }
    if (interrupt && state->async_repl != NULL &&
        state->async_repl->enabled) {
        int focused = gsh_async_repl_focused_job(state->async_repl);

        if (focused >= 0) {
            (void)signal_managed_job(state, focused, SIGINT);
        } else {
            cancel_editor_line(state);
        }
    } else if (interrupt && state->mode == MODE_EDITOR) {
        cancel_editor_line(state);
    } else if (interrupt && state->mode == MODE_DISPATCH) {
        state->mode = MODE_EDITOR;
        state->pending_line[0] = '\0';
        cancel_editor_line(state);
    } else if (interrupt && state->mode == MODE_ASYNC_WORKER) {
        disable_prompt_worker(state, true);
        state->prompt_worker_restart_pending = true;
        state->last_status = 130;
        state->mode = MODE_EDITOR;
        state->worker_redirection_target[0] = '\0';
        abandon_pending_list(state);
        cancel_editor_line(state);
    } else if (interrupt && state->mode == MODE_WAIT) {
        state->wait_target_count = 0;
        state->wait_all = false;
        state->last_status = 130;
        state->mode = MODE_EDITOR;
        state->wait_negated = false;
        abandon_pending_list(state);
        cancel_editor_line(state);
    }
    if (resize) {
        if (state->async_repl != NULL && state->async_repl->enabled) {
            resize_managed_jobs(state);
        } else if (state->mode == MODE_EDITOR) {
            queue_redraw(state);
        }
    }
    if (suspend && state->async_repl != NULL &&
        state->async_repl->enabled) {
        int focused = gsh_async_repl_focused_job(state->async_repl);

        if (focused >= 0) {
            stop_managed_job(state, focused);
        }
    }
    if (shutdown) {
        state->running = false;
    }
}

static bool is_simple_syntax_byte(unsigned char byte)
{
    if (byte < 0x20U && byte != '\t') {
        return false;
    }
    switch (byte) {
    case '\'':
    case '"':
    case '\\':
    case '$':
    case '`':
    case '|':
    case '&':
    case ';':
    case '<':
    case '>':
    case '(':
    case ')':
    case '*':
    case '?':
    case '[':
    case ']':
    case '{':
    case '}':
    case '~':
    case '#':
    case '!':
        return false;
    default:
        return true;
    }
}

static bool is_native_command_name(const char *name)
{
    static const char *const shell_only[] = {
        ".",      ":",       "alias",    "bg",       "break",
        "cd",     "command", "continue", "echo",     "eval",
        "exec",   "exit",    "export",   "false",    "fc",
        "fg",
        "getopts", "hash",   "help",     "jobs",     "kill",
        "printf", "pwd",     "read",     "readonly", "return",
        "rt",     "set",     "shift",    "test",     "times",
        "trap",   "true",    "type",
        "ulimit", "umask",   "unalias",  "unset",    "wait",
        "case",   "do",      "done",     "elif",     "else",
        "esac",   "fi",      "for",      "function", "if",
        "in",     "select",  "then",     "time",     "until",
        "while",  "[",       "[[",       "]]",
    };
    size_t index;

    for (index = 0; index < sizeof(shell_only) / sizeof(shell_only[0]);
         index++) {
        if (strcmp(name, shell_only[index]) == 0) {
            return false;
        }
    }
    return true;
}

static bool prepare_simple_command(const char *line, char storage[LINE_CAP],
                                   simple_command *command)
{
    size_t length = strlen(line);
    size_t index;
    gsh_lexer lexer;

    if (length == 0 || length >= LINE_CAP) {
        return false;
    }
    for (index = 0; index < length; index++) {
        unsigned char byte = (unsigned char)line[index];

        if (byte != ' ' && byte != '\t' && !is_simple_syntax_byte(byte)) {
            return false;
        }
    }

    memcpy(storage, line, length + 1);
    gsh_lexer_init(&lexer, line, length);
    for (;;) {
        gsh_token token;

        if (gsh_lexer_next(&lexer, &token) != GSH_LEX_OK) {
            return false;
        }
        if (token.kind == GSH_TOKEN_EOF) {
            break;
        }
        if (token.kind != GSH_TOKEN_WORD ||
            command->argc == SIMPLE_ARG_CAP) {
            return false;
        }
        command->argv[command->argc++] = storage + token.begin;
        storage[token.end] = '\0';
    }
    if (command->argc == 0) {
        return false;
    }
    command->argv[command->argc] = NULL;

    if (strchr(command->argv[0], '=') != NULL ||
        !is_native_command_name(command->argv[0])) {
        return false;
    }
    return true;
}

static size_t child_string_length(const char *text, size_t limit)
{
    size_t length = 0;

    while (length < limit && text[length] != '\0') {
        length++;
    }
    return length;
}

static bool child_string_contains(const char *text, char wanted)
{
    while (*text != '\0') {
        if (*text++ == wanted) {
            return true;
        }
    }
    return false;
}

static void child_copy_bytes(char *destination, const char *source,
                             size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        destination[index] = source[index];
    }
}

static void child_write_text(const char *text)
{
    size_t length = child_string_length(text, EXEC_PATH_CAP);

    while (length > 0) {
        ssize_t written = write(STDERR_FILENO, text, length);

        if (written > 0) {
            text += (size_t)written;
            length -= (size_t)written;
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static void child_exec_error(const char *name, int error)
{
    child_write_text("gsh: ");
    child_write_text(name);
    if (error == ENOENT || error == ENOTDIR) {
        child_write_text(": command not found\n");
        _exit(127);
    }
    if (error == EACCES) {
        child_write_text(": permission denied\n");
    } else {
        child_write_text(": execution failed\n");
    }
    _exit(126);
}

static void child_exec_script(const char *path, char *const arguments[],
                              char *const environment[])
{
    char *shell_arguments[SIMPLE_ARG_CAP + 2];
    size_t index = 1;

    shell_arguments[0] = (char *)"sh";
    shell_arguments[1] = (char *)path;
    while (arguments[index] != NULL && index < SIMPLE_ARG_CAP) {
        shell_arguments[index + 1] = arguments[index];
        index++;
    }
    shell_arguments[index + 1] = NULL;
    execve("/bin/sh", shell_arguments, environment);
}

static void child_try_exec(const char *path, char *const arguments[],
                           char *const environment[])
{
    execve(path, arguments, environment);
    if (errno == ENOEXEC) {
        child_exec_script(path, arguments, environment);
    }
}

static void child_exec_direct(char *const arguments[], const char *path_value,
                              char *const environment[])
{
    const char *name = arguments[0];
    size_t name_length = child_string_length(name, EXEC_PATH_CAP);
    const char *cursor;
    bool access_denied = false;

    if (name_length == 0 || name_length == EXEC_PATH_CAP) {
        child_exec_error(name, ENAMETOOLONG);
    }
    if (child_string_contains(name, '/')) {
        child_try_exec(name, arguments, environment);
        child_exec_error(name, errno);
    }

    cursor = path_value;
    for (;;) {
        char candidate[EXEC_PATH_CAP];
        const char *separator = cursor;
        size_t directory_length;
        size_t offset = 0;
        int error;

        while (*separator != '\0' && *separator != ':') {
            separator++;
        }
        directory_length = (size_t)(separator - cursor);
        if (directory_length == 0) {
            candidate[offset++] = '.';
        } else if (directory_length < EXEC_PATH_CAP) {
            child_copy_bytes(candidate, cursor, directory_length);
            offset = directory_length;
        } else {
            child_exec_error(name, ENAMETOOLONG);
        }
        if (offset + 1 + name_length + 1 > sizeof(candidate)) {
            child_exec_error(name, ENAMETOOLONG);
        }
        candidate[offset++] = '/';
        child_copy_bytes(candidate + offset, name, name_length + 1U);

        child_try_exec(candidate, arguments, environment);
        error = errno;
        if (error == EACCES) {
            access_denied = true;
        } else if (error != ENOENT && error != ENOTDIR) {
            child_exec_error(name, error);
        }
        if (*separator == '\0') {
            break;
        }
        cursor = separator + 1;
    }
    child_exec_error(name, access_denied ? EACCES : ENOENT);
}

static bool direct_path_is_bounded(const simple_command *command,
                                   const char *path_value)
{
    const char *cursor;
    size_t name_length;

    if (strchr(command->argv[0], '/') != NULL) {
        return true;
    }
    if (strnlen(path_value, PATH_SCAN_CAP + 1U) > PATH_SCAN_CAP) {
        return false;
    }

    name_length = strlen(command->argv[0]);
    cursor = path_value;
    for (;;) {
        const char *separator = cursor;
        size_t directory_length;

        while (*separator != '\0' && *separator != ':') {
            separator++;
        }
        directory_length = (size_t)(separator - cursor);
        if ((directory_length == 0 ? 1U : directory_length) + 1U +
                name_length + 1U >
            EXEC_PATH_CAP) {
            return false;
        }
        if (*separator == '\0') {
            return true;
        }
        cursor = separator + 1;
    }
}

static bool native_stateless_builtin(const gsh_native_command *command,
                                     int *status)
{
    const char *name;

    if (command->argc == 0) {
        return false;
    }
    name = command->argv[0];
    if (strcmp(name, ":") == 0 || strcmp(name, "true") == 0) {
        *status = 0;
        return true;
    }
    if (strcmp(name, "false") == 0) {
        *status = 1;
        return true;
    }
    return false;
}

static bool native_colon_builtin(const gsh_native_command *command)
{
    return command->argc > 0 && strcmp(command->argv[0], ":") == 0;
}

static bool native_pwd_builtin(const gsh_native_command *command)
{
    return command->argc > 0 && strcmp(command->argv[0], "pwd") == 0;
}

static bool native_cd_builtin(const gsh_native_command *command)
{
    return command->argc > 0 && strcmp(command->argv[0], "cd") == 0;
}

static bool native_environment_builtin(const gsh_native_command *command)
{
    return command->argc > 0 && command->assignment_count == 0 &&
           command->redirect_count == 0 &&
           (strcmp(command->argv[0], "ulimit") == 0 ||
            strcmp(command->argv[0], "umask") == 0);
}

static bool native_variable_builtin(const gsh_native_command *command)
{
    return command->argc > 0 &&
           (strcmp(command->argv[0], "export") == 0 ||
            strcmp(command->argv[0], "readonly") == 0 ||
            strcmp(command->argv[0], "unset") == 0);
}

static bool native_state_builtin(const gsh_native_command *command)
{
    return command->argc > 0 &&
           (strcmp(command->argv[0], "set") == 0 ||
            strcmp(command->argv[0], "shift") == 0);
}

static bool native_wait_builtin(const gsh_native_command *command)
{
    return command->argc > 0 && strcmp(command->argv[0], "wait") == 0;
}

static bool native_alias_builtin(const gsh_native_command *command)
{
    return command->argc > 0 &&
           (strcmp(command->argv[0], "alias") == 0 ||
            strcmp(command->argv[0], "unalias") == 0);
}

static bool native_return_builtin(const gsh_native_command *command)
{
    return command->argc > 0 && strcmp(command->argv[0], "return") == 0;
}

static bool special_builtin_name(const char *name, size_t length)
{
    static const char *const names[] = {
        ".",      ":",      "break",  "continue", "eval", "exec",
        "exit",   "export", "readonly", "return", "set",  "shift",
        "times",  "trap",   "unset",
    };
    size_t index;

    for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        if (strlen(names[index]) == length &&
            memcmp(names[index], name, length) == 0) {
            return true;
        }
    }
    return false;
}

static bool native_alias_mutates(const gsh_native_command *command)
{
    size_t index;

    if (!native_alias_builtin(command)) {
        return false;
    }
    if (strcmp(command->argv[0], "unalias") == 0) {
        return true;
    }
    for (index = 1; index < command->argc; index++) {
        if (strcmp(command->argv[index], "--") != 0 &&
            strchr(command->argv[index], '=') != NULL) {
            return true;
        }
    }
    return false;
}

static bool native_function_mutates(const gsh_native_command *command)
{
    size_t index = 1U;

    if (command->argc == 0 || strcmp(command->argv[0], "unset") != 0) {
        return false;
    }
    while (index < command->argc && command->argv[index][0] == '-' &&
           command->argv[index][1] != '\0') {
        const char *option = command->argv[index] + 1U;

        if (strcmp(command->argv[index], "--") == 0) {
            break;
        }
        while (*option != '\0') {
            if (*option++ == 'f') {
                return true;
            }
        }
        index++;
    }
    return false;
}

static bool native_set_variable_listing(const gsh_native_command *command)
{
    return command->argc == 1 && strcmp(command->argv[0], "set") == 0;
}

static bool native_variable_listing(const gsh_native_command *command)
{
    return command->argc == 2 && strcmp(command->argv[1], "-p") == 0 &&
           (strcmp(command->argv[0], "export") == 0 ||
            strcmp(command->argv[0], "readonly") == 0);
}

static bool native_pipeline_requires_evaluator(
    const gsh_native_pipeline *pipeline)
{
    const gsh_native_command *command;

    if (pipeline->command_count != 1) {
        return false;
    }
    command = &pipeline->commands[0];
    return (native_variable_builtin(command) &&
            (command->redirect_count != 0 ||
             (command->assignment_count != 0 &&
              native_variable_listing(command)))) ||
           (native_state_builtin(command) &&
            (command->redirect_count != 0 ||
             native_set_variable_listing(command))) ||
           (native_cd_builtin(command) &&
            command->redirect_count != 0) ||
           (native_alias_builtin(command) &&
            command->redirect_count != 0) ||
           (native_colon_builtin(command) &&
            command->assignment_count != 0 &&
            command->redirect_count != 0);
}

static unsigned int assignment_attributes(
    const gsh_shell_options *options)
{
    return gsh_options_enabled(options, GSH_OPTION_ALLEXPORT)
               ? GSH_VARIABLE_EXPORTED
               : 0U;
}

static int unset_native_functions(
    const gsh_native_command *command, gsh_function_store *functions,
    const gsh_builtin_io *io)
{
    size_t index = 1U;
    bool selected = false;
    int status = 0;

    if (strcmp(command->argv[0], "unset") != 0) {
        return 0;
    }
    while (index < command->argc && command->argv[index][0] == '-' &&
           command->argv[index][1] != '\0') {
        const char *option = command->argv[index] + 1U;

        if (strcmp(command->argv[index], "--") == 0) {
            index++;
            break;
        }
        while (*option != '\0') {
            if (*option == 'f') {
                selected = true;
            } else if (*option != 'v') {
                return 0;
            }
            option++;
        }
        index++;
    }
    if (!selected || functions == NULL) {
        return 0;
    }
    for (; index < command->argc; index++) {
        size_t length = strlen(command->argv[index]);

        if (!gsh_variable_name_is_valid(command->argv[index], length)) {
            status = gsh_builtin_error(io, "unset",
                                       "invalid function name");
        } else if (gsh_functions_unset(functions, command->argv[index],
                                       length) == -1) {
            status = gsh_builtin_error(io, "unset",
                                       "function update failed");
        }
    }
    return status;
}

static int run_native_variable_builtin(
    const gsh_native_command *command, gsh_variable_store *variables,
    gsh_variable_journal *journal, const gsh_shell_options *options,
    gsh_function_store *functions, const gsh_builtin_io *io)
{
    int status = gsh_builtin_variables(
        command->argc, command->argv, variables, journal,
        assignment_attributes(options), io);
    int function_status = unset_native_functions(command, functions, io);

    return status != 0 ? status : function_status;
}

static int run_native_state_builtin(
    const gsh_native_command *command, gsh_variable_store *variables,
    gsh_positional_store *positionals, gsh_shell_options *options,
    const gsh_builtin_io *io)
{
    return strcmp(command->argv[0], "set") == 0
               ? gsh_builtin_set(command->argc, command->argv, variables,
                                 positionals, options, io)
               : gsh_builtin_shift(command->argc, command->argv,
                                   positionals, io);
}

static int run_native_cd_builtin(
    const gsh_native_command *command,
    const gsh_variable_store *lookup_variables,
    gsh_variable_store *variables,
    gsh_variable_journal *journal, const gsh_shell_options *options,
    const gsh_builtin_io *io, char *directory,
    size_t directory_capacity)
{
    return gsh_builtin_cd(command->argc, command->argv, lookup_variables,
                          variables, journal, assignment_attributes(options),
                          io, directory, directory_capacity);
}

static int run_native_environment_builtin(const gsh_native_command *command,
                                          const gsh_builtin_io *io)
{
    return strcmp(command->argv[0], "ulimit") == 0
               ? gsh_builtin_ulimit(command->argc, command->argv, io)
               : gsh_builtin_umask(command->argc, command->argv, io);
}

static int run_native_alias_builtin(
    const gsh_native_command *command, gsh_alias_store *aliases,
    gsh_alias_journal *journal, const gsh_builtin_io *io)
{
    return strcmp(command->argv[0], "alias") == 0
               ? gsh_builtin_alias(command->argc, command->argv, aliases,
                                   journal, io)
               : gsh_builtin_unalias(command->argc, command->argv, aliases,
                                     journal, io);
}

static const gsh_builtin_io descriptor_builtin_io = {
    gsh_builtin_descriptor_output, NULL};

static void child_write_descriptor(int descriptor, const char *text,
                                   size_t length)
{
    while (length > 0) {
        ssize_t written = write(descriptor, text, length);

        if (written > 0) {
            text += (size_t)written;
            length -= (size_t)written;
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

static bool logical_pwd_is_valid(const char *path)
{
    const char *component;
    struct stat logical;
    struct stat physical;

    if (path == NULL || path[0] != '/' || stat(path, &logical) == -1 ||
        stat(".", &physical) == -1 || logical.st_dev != physical.st_dev ||
        logical.st_ino != physical.st_ino) {
        return false;
    }
    component = path + 1;
    for (;;) {
        const char *end = component;
        size_t length;

        while (*end != '\0' && *end != '/') {
            end++;
        }
        length = (size_t)(end - component);
        if ((length == 1 && component[0] == '.') ||
            (length == 2 && component[0] == '.' && component[1] == '.')) {
            return false;
        }
        if (*end == '\0') {
            return true;
        }
        component = end + 1;
    }
}

static int child_run_pwd(const gsh_native_command *command,
                         const gsh_variable_store *variables)
{
    char physical[PATH_MAX];
    const char *directory = NULL;
    bool logical = true;
    size_t index;

    for (index = 1; index < command->argc; index++) {
        if (strcmp(command->argv[index], "-L") == 0) {
            logical = true;
        } else if (strcmp(command->argv[index], "-P") == 0) {
            logical = false;
        } else {
            static const char message[] = "gsh: pwd: invalid operand\n";

            child_write_descriptor(STDERR_FILENO, message,
                                   sizeof(message) - 1U);
            return 1;
        }
    }
    if (logical) {
        bool found;
        const char *pwd =
            gsh_variables_lookup(variables, "PWD", 3, &found);

        if (found && logical_pwd_is_valid(pwd)) {
            directory = pwd;
        }
    }
    if (directory == NULL) {
        directory = getcwd(physical, sizeof(physical));
    }
    if (directory == NULL) {
        static const char message[] = "gsh: pwd: failed\n";

        child_write_descriptor(STDERR_FILENO, message,
                               sizeof(message) - 1U);
        return 1;
    }
    child_write_descriptor(STDOUT_FILENO, directory, strlen(directory));
    child_write_descriptor(STDOUT_FILENO, "\n", 1);
    return 0;
}

static bool assignment_overrides_environment(const char *assignment,
                                             const char *entry)
{
    size_t offset = 0;

    while (assignment[offset] != '\0' && assignment[offset] != '=') {
        if (entry[offset] == '\0' || entry[offset] != assignment[offset]) {
            return false;
        }
        offset++;
    }
    return assignment[offset] == '=' && entry[offset] == '=';
}

static char *const *child_command_environment(
    const gsh_variable_store *variables,
    const gsh_native_command *command,
    char *storage[CHILD_ENVIRONMENT_CAP])
{
    size_t source;
    size_t used = 0;
    size_t assignment_count =
        command != NULL ? command->assignment_count : 0;

    for (source = 0; source < gsh_variables_count(variables); source++) {
        size_t assignment;
        bool overridden = false;
        unsigned int attributes;
        const char *entry =
            gsh_variables_assignment(variables, source, &attributes);

        if (!gsh_variables_is_set(variables, source) ||
            (attributes & GSH_VARIABLE_EXPORTED) == 0) {
            continue;
        }
        for (assignment = 0; assignment < assignment_count; assignment++) {
            if (assignment_overrides_environment(
                    command->assignments[assignment], entry)) {
                overridden = true;
                break;
            }
        }
        if (!overridden) {
            if (used + 1U == CHILD_ENVIRONMENT_CAP) {
                child_exec_error("environment", E2BIG);
            }
            storage[used++] = (char *)entry;
        }
    }
    for (source = 0; source < assignment_count; source++) {
        if (used + 1U == CHILD_ENVIRONMENT_CAP) {
            child_exec_error("environment", E2BIG);
        }
        storage[used++] = command->assignments[source];
    }
    storage[used] = NULL;
    return storage;
}

struct pipeline_expansion_scope {
    const gsh_variable_store *base;
    gsh_variable_journal *changes;
    size_t command_count;
    size_t current_scope;
};

static const char *store_path_value(const gsh_variable_store *variables,
                                    const char *default_path);
static const char *command_path_value(
    const gsh_variable_store *variables, const gsh_native_command *command,
    const char *default_path);
static const char *scoped_command_path_value(
    const pipeline_expansion_scope *scope, unsigned int command_scope,
    const gsh_native_command *command, const char *default_path);

static bool native_planned_command_is_supported(
    const gsh_native_pipeline *pipeline, size_t index,
    const char *path_value)
{
    const gsh_native_command *native = &pipeline->commands[index];
    simple_command command = {0};
    size_t argument;
    int builtin_status;

    if (native->expansion_error) {
        return true;
    }
    if (native->argc == 0) {
        return pipeline->command_count == 1 &&
               native->assignment_count != 0 &&
               native->redirect_count == 0;
    }
    if (strchr(native->argv[0], '=') != NULL) {
        return false;
    }
    if (native_stateless_builtin(native, &builtin_status)) {
        return true;
    }
    if (native_pwd_builtin(native) && native->assignment_count == 0) {
        return true;
    }
    if (native_cd_builtin(native)) {
        return true;
    }
    if (native_environment_builtin(native)) {
        return true;
    }
    if (native_variable_builtin(native)) {
        return true;
    }
    if (native_state_builtin(native)) {
        return true;
    }
    if (native_wait_builtin(native)) {
        return native->assignment_count == 0 &&
               native->redirect_count == 0;
    }
    if (native_alias_builtin(native)) {
        return true;
    }
    if (native_return_builtin(native)) {
        return native->assignment_count == 0 && native->redirect_count == 0;
    }
    if (!is_native_command_name(native->argv[0])) {
        return false;
    }
    command.argc = native->argc;
    for (argument = 0; argument < command.argc; argument++) {
        command.argv[argument] = native->argv[argument];
    }
    command.argv[command.argc] = NULL;
    return direct_path_is_bounded(&command, path_value);
}

static bool native_pipeline_is_supported_scoped(
    const gsh_native_pipeline *pipeline, const char *default_path,
    const gsh_variable_store *variables,
    const pipeline_expansion_scope *scope)
{
    size_t index;

    for (index = 0; index < pipeline->command_count; index++) {
        const char *path = scope != NULL
                               ? scoped_command_path_value(
                                     scope, (unsigned int)index + 1U,
                                     &pipeline->commands[index],
                                     default_path)
                               : command_path_value(
                                     variables, &pipeline->commands[index],
                                     default_path);

        if (!native_planned_command_is_supported(
                pipeline, index,
                path)) {
            return false;
        }
    }
    return true;
}

enum {
    GSH_ASSIGNMENT_OK = 0,
    GSH_ASSIGNMENT_ERROR = -1,
    GSH_ASSIGNMENT_JOURNAL_ERROR = -2,
};

static int apply_native_assignments(gsh_variable_store *variables,
                                    gsh_variable_journal *journal,
                                    const gsh_native_command *command,
                                    const gsh_shell_options *options)
{
    unsigned int attributes = assignment_attributes(options);
    size_t index;

    for (index = 0; index < command->assignment_count; index++) {
        const char *assignment = command->assignments[index];
        const char *separator = strchr(assignment, '=');
        size_t name_length;
        size_t value_length;

        if (separator == NULL) {
            errno = EINVAL;
            return -1;
        }
        name_length = (size_t)(separator - assignment);
        if (!gsh_variable_name_is_valid(assignment, name_length)) {
            errno = E2BIG;
            return -1;
        }
        value_length = strlen(separator + 1U);
        if (gsh_variables_set(variables, assignment, name_length,
                              separator + 1U, value_length, attributes,
                              attributes) == -1) {
            return GSH_ASSIGNMENT_ERROR;
        }
        if (journal != NULL &&
            gsh_variable_journal_record(
                journal, assignment, name_length, separator + 1U,
                value_length, attributes, attributes) == -1) {
            return GSH_ASSIGNMENT_JOURNAL_ERROR;
        }
    }
    return 0;
}

static int child_duplicate_descriptor(int source, int destination)
{
    if (source == destination) {
        return fcntl(source, F_GETFD) == -1 ? -1 : destination;
    }
    if (fault_should_fail("descriptor-dup", EMFILE)) {
        return -1;
    }
    return dup2(source, destination);
}

typedef struct {
    int descriptor;
    int saved;
} gsh_saved_descriptor;

static int save_redirect_descriptors(
    const gsh_native_command *command,
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP], size_t *saved_count)
{
    int minimum = STDERR_FILENO + 1;
    size_t index;

    *saved_count = 0;
    for (index = 0; index < command->redirect_count; index++) {
        if (command->redirects[index].descriptor < 0) {
            errno = EINVAL;
            return -1;
        }
        if (command->redirects[index].descriptor >= minimum) {
            if (command->redirects[index].descriptor == INT_MAX) {
                errno = EMFILE;
                return -1;
            }
            minimum = command->redirects[index].descriptor + 1;
        }
    }
    for (index = 0; index < command->redirect_count; index++) {
        int descriptor = command->redirects[index].descriptor;
        size_t prior;

        for (prior = 0; prior < *saved_count; prior++) {
            if (saved[prior].descriptor == descriptor) {
                break;
            }
        }
        if (prior != *saved_count) {
            continue;
        }
        saved[*saved_count].descriptor = descriptor;
        saved[*saved_count].saved =
            fault_should_fail("descriptor-save", EMFILE)
                ? -1
                : fcntl(descriptor, F_DUPFD_CLOEXEC, minimum);
        if (saved[*saved_count].saved == -1 && errno != EBADF) {
            while (*saved_count > 0) {
                int prior_saved = saved[--(*saved_count)].saved;

                if (prior_saved >= 0) {
                    close(prior_saved);
                }
            }
            return -1;
        }
        (*saved_count)++;
    }
    return 0;
}

static int restore_redirect_descriptors(
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP], size_t saved_count)
{
    int status = 0;

    while (saved_count > 0) {
        gsh_saved_descriptor *entry = &saved[--saved_count];

        if (entry->descriptor < 0) {
            errno = EINVAL;
            status = -1;
            continue;
        }
        if (entry->saved >= 0) {
            if (dup2(entry->saved, entry->descriptor) == -1) {
                status = -1;
            }
            close(entry->saved);
        } else if (close(entry->descriptor) == -1 && errno != EBADF) {
            status = -1;
        }
    }
    return status;
}

static int materialize_heredoc(const gsh_native_pipeline *pipeline,
                               size_t heredoc_index)
{
    char path[] = "/tmp/gsh-heredoc-XXXXXX";
    const gsh_native_heredoc *heredoc;
    const char *cursor;
    size_t remaining;
    int descriptor;

    if (heredoc_index >= pipeline->heredoc_count) {
        errno = EINVAL;
        return -1;
    }
    descriptor = mkstemp(path);
    if (descriptor == -1) {
        return -1;
    }
    if (unlink(path) == -1) {
        int saved_errno = errno;

        close(descriptor);
        errno = saved_errno;
        return -1;
    }
    heredoc = &pipeline->heredocs[heredoc_index];
    cursor = heredoc->body;
    remaining = heredoc->length;
    while (remaining > 0) {
        ssize_t written = fault_should_fail("heredoc-write", EIO)
                              ? -1
                              : write(descriptor, cursor, remaining);

        if (written > 0) {
            cursor += (size_t)written;
            remaining -= (size_t)written;
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            int saved_errno = written == 0 ? EIO : errno;

            close(descriptor);
            errno = saved_errno;
            return -1;
        }
    }
    if (lseek(descriptor, 0, SEEK_SET) == -1) {
        int saved_errno = errno;

        close(descriptor);
        errno = saved_errno;
        return -1;
    }
    return descriptor;
}

static int open_redirect_path(const char *target,
                              gsh_token_kind operator_kind,
                              const gsh_shell_options *options,
                              mode_t creation_mode)
{
    int flags;

    if (fault_should_fail("redirect-open", EMFILE)) {
        return -1;
    }
    if (operator_kind == GSH_TOKEN_GREAT &&
        gsh_options_enabled(options, GSH_OPTION_NOCLOBBER)) {
        struct stat status;
        int descriptor = open(target, O_WRONLY | O_CREAT | O_EXCL,
                              creation_mode);

        if (descriptor >= 0 || errno != EEXIST) {
            return descriptor;
        }
        descriptor = open(target, O_WRONLY);
        if (descriptor == -1) {
            return -1;
        }
        if (fstat(descriptor, &status) == -1) {
            int saved_errno = errno;

            close(descriptor);
            errno = saved_errno;
            return -1;
        }
        if (S_ISREG(status.st_mode)) {
            close(descriptor);
            errno = EEXIST;
            return -1;
        }
        return descriptor;
    }
    if (operator_kind == GSH_TOKEN_LESS) {
        flags = O_RDONLY;
    } else if (operator_kind == GSH_TOKEN_DGREAT) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    } else if (operator_kind == GSH_TOKEN_LESSGREAT) {
        flags = O_RDWR | O_CREAT;
    } else if (operator_kind == GSH_TOKEN_GREAT ||
               operator_kind == GSH_TOKEN_CLOBBER) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else {
        errno = EINVAL;
        return -1;
    }
    return open(target, flags, creation_mode);
}

static int open_redirect_target(const gsh_native_redirect *redirect,
                                const gsh_shell_options *options)
{
    return open_redirect_path(redirect->target, redirect->operator_kind,
                              options, 0666);
}

static int apply_evaluator_redirects(
    const gsh_native_pipeline *pipeline, const gsh_native_command *command,
    const gsh_shell_options *options)
{
    size_t index;

    for (index = 0; index < command->redirect_count; index++) {
        const gsh_native_redirect *redirect = &command->redirects[index];
        int descriptor;

        if (redirect->close_descriptor) {
            if (close(redirect->descriptor) == -1 && errno != EBADF) {
                return -1;
            }
            continue;
        }
        if (redirect->operator_kind == GSH_TOKEN_LESSAND ||
            redirect->operator_kind == GSH_TOKEN_GREATAND) {
            if (child_duplicate_descriptor(redirect->duplicate_descriptor,
                                           redirect->descriptor) == -1) {
                return -1;
            }
            continue;
        }
        if (redirect->operator_kind == GSH_TOKEN_DLESS ||
            redirect->operator_kind == GSH_TOKEN_DLESSDASH) {
            descriptor = materialize_heredoc(
                pipeline, redirect->heredoc_index);
        } else {
            descriptor = open_redirect_target(redirect, options);
        }
        if (descriptor == -1 ||
            child_duplicate_descriptor(descriptor, redirect->descriptor) ==
                -1) {
            int saved_errno = errno;

            if (descriptor >= 0 && descriptor != redirect->descriptor) {
                close(descriptor);
            }
            errno = saved_errno;
            return -1;
        }
        if (descriptor != redirect->descriptor) {
            close(descriptor);
        }
    }
    return 0;
}

static int run_evaluator_variable_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_journal *journal, const gsh_shell_options *options,
    gsh_function_store *functions)
{
    const gsh_native_command *command = &pipeline->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    int status;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command, options) == -1) {
        int saved_errno = errno;

        errno = saved_errno;
        perror("gsh: redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    {
        int assignment_status =
            apply_native_assignments(variables, journal, command, options);

        if (assignment_status != GSH_ASSIGNMENT_OK) {
            perror("gsh: assignment");
            status = assignment_status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125
                                                                       : 1;
        } else {
            status = run_native_variable_builtin(
                command, variables, journal, options,
                functions, &descriptor_builtin_io);
        }
    }
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: redirection restore");
        return 125;
    }
    return status == 125 ? 125
                         : (pipeline->negated ? (status == 0 ? 1 : 0)
                                              : status);
}

static int run_evaluator_alias_builtin(
    const gsh_native_pipeline *pipeline, gsh_alias_store *aliases,
    gsh_alias_journal *journal, const gsh_shell_options *options)
{
    const gsh_native_command *command = &pipeline->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    int status;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: alias redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command, options) == -1) {
        perror("gsh: alias redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    status = run_native_alias_builtin(command, aliases, journal,
                                      &descriptor_builtin_io);
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: alias redirection restore");
        return 125;
    }
    return status == 125 ? 125
                         : (pipeline->negated ? (status == 0 ? 1 : 0)
                                              : status);
}

static int run_evaluator_state_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_journal *journal, gsh_positional_store *positionals,
    gsh_shell_options *options)
{
    const gsh_native_command *command = &pipeline->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    int status;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command, options) == -1) {
        perror("gsh: redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    status = apply_native_assignments(variables, journal, command, options);
    if (status != GSH_ASSIGNMENT_OK) {
        perror("gsh: assignment");
        status = status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125 : 1;
    } else {
        status = run_native_state_builtin(
            command, variables, positionals, options,
            &descriptor_builtin_io);
    }
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: redirection restore");
        return 125;
    }
    return status == 125 ? 125
                         : (pipeline->negated ? (status == 0 ? 1 : 0)
                                              : status);
}

static int run_evaluator_cd_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_store *scratch,
    gsh_variable_journal *journal, const gsh_shell_options *options,
    char *directory, size_t directory_capacity)
{
    const gsh_native_command *command = &pipeline->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    const gsh_variable_store *lookup_variables = variables;
    size_t saved_count;
    int status;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command, options) == -1) {
        perror("gsh: redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    if (command->assignment_count != 0) {
        memcpy(scratch, variables, sizeof(*scratch));
        status = apply_native_assignments(scratch, NULL, command, options);
        if (status != GSH_ASSIGNMENT_OK) {
            perror("gsh: assignment");
            status = 1;
            goto restore;
        }
        lookup_variables = scratch;
    }
    status = run_native_cd_builtin(
        command, lookup_variables, variables, journal, options,
        &descriptor_builtin_io, directory, directory_capacity);
restore:
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: redirection restore");
        return 125;
    }
    return pipeline->negated ? (status == 0 ? 1 : 0) : status;
}

static int run_evaluator_colon_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_journal *journal, const gsh_shell_options *options)
{
    const gsh_native_command *command = &pipeline->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    int status;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command, options) == -1) {
        perror("gsh: redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    status = apply_native_assignments(variables, journal, command, options);
    if (status != GSH_ASSIGNMENT_OK) {
        perror("gsh: assignment");
        status = status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125 : 1;
    } else {
        status = 0;
    }
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: redirection restore");
        return 125;
    }
    return status == 125 ? 125
                         : (pipeline->negated ? (status == 0 ? 1 : 0)
                                              : status);
}

static void child_apply_redirects(
    const gsh_native_command *command,
    int heredoc_descriptors[GSH_NATIVE_HEREDOC_CAP][2],
    size_t heredoc_count, const gsh_shell_options *options)
{
    size_t index;

    for (index = 0; index < command->redirect_count; index++) {
        const gsh_native_redirect *redirect = &command->redirects[index];
        int descriptor;

        if (redirect->close_descriptor) {
            (void)close(redirect->descriptor);
            continue;
        }
        if (redirect->operator_kind == GSH_TOKEN_LESSAND ||
            redirect->operator_kind == GSH_TOKEN_GREATAND) {
            if (child_duplicate_descriptor(redirect->duplicate_descriptor,
                                           redirect->descriptor) == -1) {
                child_exec_error("redirection", errno);
            }
            continue;
        }
        if (redirect->operator_kind == GSH_TOKEN_DLESS ||
            redirect->operator_kind == GSH_TOKEN_DLESSDASH) {
            if (redirect->heredoc_index >= heredoc_count ||
                heredoc_descriptors[redirect->heredoc_index][0] < 0) {
                child_exec_error("here-document", EINVAL);
            }
            descriptor =
                heredoc_descriptors[redirect->heredoc_index][0];
            if (child_duplicate_descriptor(descriptor,
                                           redirect->descriptor) == -1) {
                child_exec_error("here-document", errno);
            }
            if (descriptor == redirect->descriptor) {
                heredoc_descriptors[redirect->heredoc_index][0] = -1;
            }
            continue;
        }
        descriptor = open_redirect_target(redirect, options);
        if (descriptor == -1 ||
            child_duplicate_descriptor(descriptor, redirect->descriptor) ==
                -1) {
            int saved_errno = errno;

            if (descriptor >= 0 && descriptor != redirect->descriptor) {
                close(descriptor);
            }
            child_exec_error(redirect->target, saved_errno);
        }
        if (descriptor != redirect->descriptor) {
            close(descriptor);
        }
    }
}

static void initialize_heredoc_descriptors(
    int descriptors[GSH_NATIVE_HEREDOC_CAP][2])
{
    size_t index;

    for (index = 0; index < GSH_NATIVE_HEREDOC_CAP; index++) {
        descriptors[index][0] = -1;
        descriptors[index][1] = -1;
    }
}

static void close_heredoc_descriptors(
    int descriptors[GSH_NATIVE_HEREDOC_CAP][2], size_t count)
{
    size_t index;

    for (index = 0; index < count; index++) {
        if (descriptors[index][0] >= 0) {
            close(descriptors[index][0]);
            descriptors[index][0] = -1;
        }
        if (descriptors[index][1] >= 0) {
            close(descriptors[index][1]);
            descriptors[index][1] = -1;
        }
    }
}

static void child_write_heredoc(
    const gsh_native_pipeline *pipeline, size_t heredoc_index,
    int descriptors[GSH_NATIVE_HEREDOC_CAP][2])
{
    const gsh_native_heredoc *heredoc =
        &pipeline->heredocs[heredoc_index];
    int descriptor = descriptors[heredoc_index][1];
    const char *cursor = heredoc->body;
    size_t remaining = heredoc->length;
    size_t index;

    for (index = 0; index < pipeline->heredoc_count; index++) {
        if (descriptors[index][0] >= 0) {
            close(descriptors[index][0]);
        }
        if (index != heredoc_index && descriptors[index][1] >= 0) {
            close(descriptors[index][1]);
        }
    }
    while (remaining > 0) {
        ssize_t written = fault_should_fail("heredoc-write", EIO)
                              ? -1
                              : write(descriptor, cursor, remaining);

        if (written > 0) {
            cursor += (size_t)written;
            remaining -= (size_t)written;
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else if (written == -1 && errno == EPIPE) {
            close(descriptor);
            _exit(0);
        } else {
            int saved_errno = written == 0 ? EIO : errno;

            close(descriptor);
            child_exec_error("here-document", saved_errno);
        }
    }
    close(descriptor);
    _exit(0);
}

static void close_pipeline_descriptors(
    int descriptors[GSH_NATIVE_PIPELINE_CAP - 1][2], size_t count)
{
    size_t index;

    for (index = 0; index < count; index++) {
        if (descriptors[index][0] >= 0) {
            close(descriptors[index][0]);
            descriptors[index][0] = -1;
        }
        if (descriptors[index][1] >= 0) {
            close(descriptors[index][1]);
            descriptors[index][1] = -1;
        }
    }
}

static void initialize_pipeline_descriptors(
    int descriptors[GSH_NATIVE_PIPELINE_CAP - 1][2])
{
    size_t index;

    for (index = 0; index < GSH_NATIVE_PIPELINE_CAP - 1U; index++) {
        descriptors[index][0] = -1;
        descriptors[index][1] = -1;
    }
}

static void start_native_pipeline(shell_state *state,
                                  const gsh_native_pipeline *pipeline,
                                  const pipeline_expansion_scope *scope)
{
    int pipes[GSH_NATIVE_PIPELINE_CAP - 1][2];
    int heredoc_pipes[GSH_NATIVE_HEREDOC_CAP][2];
    int gate[2] = {-1, -1};
    pid_t members[GSH_NATIVE_JOB_MEMBER_CAP];
    size_t pipe_count = pipeline->command_count - 1U;
    size_t created_pipes = 0;
    size_t created_heredocs = 0;
    size_t launched = 0;
    pid_t pgid = 0;
    pid_t status_pid = -1;
    sigset_t blocked;
    sigset_t previous;
    size_t index;

    if (state->async_repl != NULL && state->async_repl->enabled) {
        start_async_native_pipeline(state, pipeline, scope);
        return;
    }
    initialize_pipeline_descriptors(pipes);
    initialize_heredoc_descriptors(heredoc_pipes);
    if (state->current_job.active) {
        output_text(state,
                    "gsh: this MVP supports one job at a time; use fg or wait "
                    "for it\r\n");
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    for (created_heredocs = 0;
         created_heredocs < pipeline->heredoc_count;
         created_heredocs++) {
        if (make_pipe(heredoc_pipes[created_heredocs], false,
                      "heredoc-pipe") == -1) {
            output_format(state, "gsh: here-document pipe: %s\r\n",
                          strerror(errno));
            close_heredoc_descriptors(heredoc_pipes,
                                      created_heredocs);
            state->mode = MODE_EDITOR;
            queue_prompt(state);
            return;
        }
    }
    for (created_pipes = 0; created_pipes < pipe_count; created_pipes++) {
        if (make_pipe(pipes[created_pipes], false, "pipeline-pipe") == -1) {
            output_format(state, "gsh: pipeline pipe: %s\r\n",
                          strerror(errno));
            close_pipeline_descriptors(pipes, created_pipes);
            close_heredoc_descriptors(heredoc_pipes,
                                      created_heredocs);
            state->mode = MODE_EDITOR;
            queue_prompt(state);
            return;
        }
    }
    if (make_pipe(gate, false, "job-pipe") == -1) {
        output_format(state, "gsh: launch gate: %s\r\n", strerror(errno));
        close_pipeline_descriptors(pipes, created_pipes);
        close_heredoc_descriptors(heredoc_pipes, created_heredocs);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        output_format(state, "gsh: sigprocmask: %s\r\n", strerror(errno));
        close(gate[0]);
        close(gate[1]);
        close_pipeline_descriptors(pipes, created_pipes);
        close_heredoc_descriptors(heredoc_pipes, created_heredocs);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    for (index = 0; index < pipeline->command_count; index++) {
        pid_t pid = fault_should_fail("pipeline-fork", EAGAIN) ? -1 : fork();

        if (pid == 0) {
            char release;
            size_t close_index;

            close(gate[1]);
            (void)setpgid(0, pgid == 0 ? 0 : pgid);
            reset_child_signals();
            (void)sigprocmask(SIG_SETMASK, &previous, NULL);
            if (pipeline->commands[index].expansion_error) {
                _exit(1);
            }
            if (scope != NULL &&
                gsh_variables_apply_journal_scope_in_place(
                    state->variables, scope->changes, index + 1U) == -1) {
                child_exec_error("pipeline variable scope", errno);
            }
            if (index > 0 &&
                child_duplicate_descriptor(pipes[index - 1U][0],
                                           STDIN_FILENO) == -1) {
                child_exec_error("pipeline input", errno);
            }
            if (index + 1U < pipeline->command_count &&
                child_duplicate_descriptor(pipes[index][1],
                                           STDOUT_FILENO) == -1) {
                child_exec_error("pipeline output", errno);
            }
            child_apply_redirects(&pipeline->commands[index],
                                  heredoc_pipes,
                                  pipeline->heredoc_count,
                                  &state->options);
            for (close_index = 0; close_index < created_pipes;
                 close_index++) {
                close(pipes[close_index][0]);
                close(pipes[close_index][1]);
            }
            close_heredoc_descriptors(heredoc_pipes,
                                      pipeline->heredoc_count);
            while (read(gate[0], &release, sizeof(release)) == -1 &&
                   errno == EINTR) {
            }
            close(gate[0]);
            close(state->tty_fd);
            close(state->signal_pipe[0]);
            close(state->signal_pipe[1]);
            if (state->prompt_worker_fd >= 0) {
                close(state->prompt_worker_fd);
            }
            if (fault_should_fail("exec", EIO)) {
                child_exec_error(pipeline->commands[index].argv[0], errno);
            }
            {
                int builtin_status;

                if (native_stateless_builtin(&pipeline->commands[index],
                                             &builtin_status)) {
                    _exit(builtin_status);
                }
                if (native_pwd_builtin(&pipeline->commands[index])) {
                    _exit(child_run_pwd(&pipeline->commands[index],
                                        state->variables));
                }
                if (native_cd_builtin(&pipeline->commands[index])) {
                    if (apply_native_assignments(
                            state->variables, NULL,
                            &pipeline->commands[index], &state->options) !=
                        GSH_ASSIGNMENT_OK) {
                        child_exec_error("assignment", errno);
                    }
                    _exit(run_native_cd_builtin(
                        &pipeline->commands[index], state->variables,
                        state->variables, NULL,
                        &state->options, &descriptor_builtin_io, NULL, 0));
                }
                if (native_environment_builtin(
                        &pipeline->commands[index])) {
                    _exit(run_native_environment_builtin(
                        &pipeline->commands[index], &descriptor_builtin_io));
                }
                if (native_variable_builtin(
                        &pipeline->commands[index])) {
                    if (apply_native_assignments(
                            state->variables, NULL,
                            &pipeline->commands[index], &state->options) !=
                        GSH_ASSIGNMENT_OK) {
                        child_exec_error("assignment", errno);
                    }
                    _exit(run_native_variable_builtin(
                        &pipeline->commands[index], state->variables, NULL,
                        &state->options, NULL, &descriptor_builtin_io));
                }
                if (native_state_builtin(
                        &pipeline->commands[index])) {
                    gsh_positional_store empty;
                    gsh_positional_store *positionals = state->positionals;

                    if (positionals == NULL) {
                        gsh_positionals_initialize(&empty);
                        positionals = &empty;
                    }
                    if (apply_native_assignments(
                            state->variables, NULL,
                            &pipeline->commands[index], &state->options) !=
                        GSH_ASSIGNMENT_OK) {
                        child_exec_error("assignment", errno);
                    }
                    _exit(run_native_state_builtin(
                        &pipeline->commands[index], state->variables,
                        positionals, &state->options,
                        &descriptor_builtin_io));
                }
                if (native_wait_builtin(&pipeline->commands[index])) {
                    _exit(pipeline->commands[index].argc == 1 ? 0 : 127);
                }
                if (native_alias_builtin(&pipeline->commands[index])) {
                    _exit(run_native_alias_builtin(
                        &pipeline->commands[index], state->aliases, NULL,
                        &descriptor_builtin_io));
                }
            }
            {
                char *environment_storage[CHILD_ENVIRONMENT_CAP];
                char *const *environment = child_command_environment(
                    state->variables, &pipeline->commands[index],
                    environment_storage);

                child_exec_direct(
                    pipeline->commands[index].argv,
                    command_path_value(state->variables,
                                       &pipeline->commands[index],
                                       state->default_path),
                    environment);
            }
        }
        if (pid == -1) {
            int saved_errno = errno;

            close_pipeline_descriptors(pipes, created_pipes);
            close_heredoc_descriptors(heredoc_pipes,
                                      created_heredocs);
            close(gate[0]);
            close(gate[1]);
            if (pgid > 0) {
                (void)kill(-pgid, SIGKILL);
                initialize_job(&state->current_job, pgid,
                               members[launched - 1U], members, launched,
                               false, false);
                state->current_job.silent = true;
                state->current_job.modes = state->original_modes;
            }
            (void)sigprocmask(SIG_SETMASK, &previous, NULL);
            output_format(state, "gsh: pipeline fork: %s\r\n",
                          strerror(saved_errno));
            state->mode = MODE_EDITOR;
            queue_prompt(state);
            return;
        }
        if (pgid == 0) {
            pgid = pid;
        }
        members[launched++] = pid;
        if (setpgid(pid, pgid) == -1 && errno != EACCES && errno != ESRCH) {
            (void)kill(pid, SIGKILL);
        }
    }

    status_pid = members[pipeline->command_count - 1U];
    for (index = 0; index < pipeline->heredoc_count; index++) {
        pid_t pid = fault_should_fail("heredoc-fork", EAGAIN) ? -1 : fork();

        if (pid == 0) {
            char release;

            close(gate[1]);
            (void)setpgid(0, pgid);
            reset_child_signals();
            (void)sigprocmask(SIG_SETMASK, &previous, NULL);
            close_pipeline_descriptors(pipes, created_pipes);
            while (read(gate[0], &release, sizeof(release)) == -1 &&
                   errno == EINTR) {
            }
            close(gate[0]);
            close(state->tty_fd);
            close(state->signal_pipe[0]);
            close(state->signal_pipe[1]);
            if (state->prompt_worker_fd >= 0) {
                close(state->prompt_worker_fd);
            }
            child_write_heredoc(pipeline, index, heredoc_pipes);
        }
        if (pid == -1) {
            int saved_errno = errno;

            close_pipeline_descriptors(pipes, created_pipes);
            close_heredoc_descriptors(heredoc_pipes,
                                      created_heredocs);
            close(gate[0]);
            close(gate[1]);
            (void)kill(-pgid, SIGKILL);
            initialize_job(&state->current_job, pgid, status_pid,
                           members, launched, false, false);
            state->current_job.silent = true;
            state->current_job.modes = state->original_modes;
            (void)sigprocmask(SIG_SETMASK, &previous, NULL);
            output_format(state, "gsh: here-document fork: %s\r\n",
                          strerror(saved_errno));
            state->mode = MODE_EDITOR;
            queue_prompt(state);
            return;
        }
        members[launched++] = pid;
        if (setpgid(pid, pgid) == -1 && errno != EACCES && errno != ESRCH) {
            (void)kill(pid, SIGKILL);
        }
    }

    close_pipeline_descriptors(pipes, created_pipes);
    close_heredoc_descriptors(heredoc_pipes, created_heredocs);
    close(gate[0]);
    initialize_job(&state->current_job, pgid, status_pid,
                   members, launched, true, pipeline->negated);
    state->current_job.modes = state->original_modes;
    if (fault_should_fail("terminal-handoff", EIO) ||
        tcsetattr(state->tty_fd, TCSANOW, &state->original_modes) == -1 ||
        tcsetpgrp(state->tty_fd, pgid) == -1) {
        int saved_errno = errno;

        state->current_job.foreground = false;
        state->current_job.silent = true;
        (void)kill(-pgid, SIGKILL);
        close(gate[1]);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        (void)enter_editor(state);
        output_format(state, "gsh: terminal handoff: %s\r\n",
                      strerror(saved_errno));
        queue_prompt(state);
        return;
    }
    state->terminal_changed = false;
    state->mode = MODE_FOREGROUND;
    close(gate[1]);
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
}

typedef struct {
    int master;
    int slave_hold;
    char slave[PATH_MAX];
} managed_pty;

/* ── A Held Slave Closes the PTY Startup Race ────────────────────
 * A new master reports hangup while no slave descriptor is open.
 * The reactor could observe that window before a forked child attached.
 * Opening one no-ctty slave before fork keeps the pair alive across startup.
 * The child closes the inherited hold only after its controlling slave works.
 * The parent can then poll immediately without mistaking startup for exit.
 * ─────────────────────────────────────────────────────────────── */

static int open_managed_pty(managed_pty *pty)
{
    const char *name;

    if (pty == NULL) {
        errno = EINVAL;
        return -1;
    }
    pty->master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (pty->master == -1 || grantpt(pty->master) == -1 ||
        unlockpt(pty->master) == -1) {
        if (pty->master >= 0) {
            (void)close(pty->master);
        }
        pty->master = -1;
        return -1;
    }
    name = ptsname(pty->master);
    if (name == NULL || strlen(name) + 1U > sizeof(pty->slave)) {
        (void)close(pty->master);
        pty->master = -1;
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(pty->slave, name, strlen(name) + 1U);
    pty->slave_hold = open(pty->slave, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (pty->slave_hold == -1 ||
        set_fd_flags(pty->master, F_GETFD, FD_CLOEXEC) == -1) {
        int saved_errno = errno;

        if (pty->slave_hold >= 0) {
            (void)close(pty->slave_hold);
        }
        (void)close(pty->master);
        pty->master = -1;
        pty->slave_hold = -1;
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static void close_child_reactor_descriptors(shell_state *state,
                                            int retained)
{
    int index;

    if (state->tty_fd >= 0 && state->tty_fd != retained) {
        (void)close(state->tty_fd);
    }
    (void)close(state->signal_pipe[0]);
    (void)close(state->signal_pipe[1]);
    if (state->prompt_worker_fd >= 0) {
        (void)close(state->prompt_worker_fd);
    }
    if (state->async_repl == NULL) {
        return;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        int descriptor = state->async_repl->cells[index].pty_fd;

        if (descriptor >= 0 && descriptor != retained) {
            (void)close(descriptor);
        }
    }
}

static int attach_child_pty(shell_state *state, const managed_pty *pty)
{
    struct winsize size;
    int slave;

    if (setsid() == -1) {
        return -1;
    }
    slave = open(pty->slave, O_RDWR);
    if (slave == -1) {
        return -1;
    }
#ifdef TIOCSCTTY
    if (ioctl(slave, TIOCSCTTY, 0) == -1 && errno != EINVAL) {
        (void)close(slave);
        return -1;
    }
#endif
    memset(&size, 0, sizeof(size));
    if (ioctl(state->tty_fd, TIOCGWINSZ, &size) == 0) {
        (void)ioctl(slave, TIOCSWINSZ, &size);
    }
    if (tcsetattr(slave, TCSANOW, &state->original_modes) == -1 ||
        tcsetpgrp(slave, getpgrp()) == -1 ||
        dup2(slave, STDIN_FILENO) == -1 ||
        dup2(slave, STDOUT_FILENO) == -1 ||
        dup2(slave, STDERR_FILENO) == -1) {
        (void)close(slave);
        return -1;
    }
    if (slave > STDERR_FILENO) {
        (void)close(slave);
    }
    if (pty->slave_hold >= 0) {
        (void)close(pty->slave_hold);
    }
    return 0;
}

static void child_exec_managed_external(shell_state *state,
                                        simple_command *direct)
{
    char *environment_storage[CHILD_ENVIRONMENT_CAP];
    char *const *environment = child_command_environment(
        state->variables, NULL, environment_storage);
    char *shell_arguments[] = {(char *)"sh", (char *)"-c",
                               (char *)state->pending_input, NULL};

    if (direct != NULL) {
        child_exec_direct(direct->argv,
                          store_path_value(state->variables,
                                           state->default_path),
                          environment);
    }
    execve("/bin/sh", shell_arguments, environment);
    child_exec_error("/bin/sh", errno);
}

static void managed_external_child(shell_state *state, managed_pty *pty,
                                   int gate_read, int gate_write,
                                   const sigset_t *previous,
                                   simple_command *direct)
{
    char release;

    (void)close(gate_write);
    (void)close(pty->master);
    reset_child_signals();
    if (attach_child_pty(state, pty) == -1) {
        child_exec_error("managed PTY", errno);
    }
    (void)sigprocmask(SIG_SETMASK, previous, NULL);
    close_child_reactor_descriptors(state, -1);
    while (read(gate_read, &release, sizeof(release)) == -1 &&
           errno == EINTR) {
    }
    (void)close(gate_read);
    child_exec_managed_external(state, direct);
}

static void start_async_external(shell_state *state, simple_command *direct)
{
    managed_pty pty = {.master = -1, .slave_hold = -1};
    int gate[2] = {-1, -1};
    sigset_t blocked;
    sigset_t previous;
    pid_t pid;

    if (open_managed_pty(&pty) == -1 ||
        make_pipe(gate, false, "job-pipe") == -1) {
        output_format(state, "gsh: managed launch: %s\r\n",
                      strerror(errno));
        if (pty.master >= 0) {
            (void)close(pty.master);
        }
        if (pty.slave_hold >= 0) {
            (void)close(pty.slave_hold);
        }
        state->mode = MODE_EDITOR;
        return;
    }
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        output_format(state, "gsh: managed sigprocmask: %s\r\n",
                      strerror(errno));
        (void)close(pty.master);
        (void)close(pty.slave_hold);
        (void)close(gate[0]);
        (void)close(gate[1]);
        state->mode = MODE_EDITOR;
        return;
    }
    pid = fork();
    if (pid == 0) {
        managed_external_child(state, &pty, gate[0], gate[1], &previous,
                               direct);
        _exit(127);
    }
    (void)close(pty.slave_hold);
    pty.slave_hold = -1;
    (void)close(gate[0]);
    if (pid == -1 || gsh_async_repl_attach(
                         state->async_repl, state->async_dispatch_cell, pid,
                         pid, pty.master) == -1) {
        int saved_errno = errno;

        if (pid > 0) {
            (void)kill(pid, SIGKILL);
        }
        (void)close(pty.master);
        output_format(state, "gsh: managed fork: %s\r\n",
                      strerror(saved_errno));
        gsh_async_repl_finish(state->async_repl,
                              state->async_dispatch_cell, 125 << 8, false);
    }
    (void)close(gate[1]);
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

static void start_external(shell_state *state, simple_command *direct)
{
    int gate[2];
    sigset_t blocked;
    sigset_t previous;
    const char *path_value =
        store_path_value(state->variables, state->default_path);
    pid_t pid;

    if (direct != NULL && !direct_path_is_bounded(direct, path_value)) {
        direct = NULL;
    }
    if (state->async_repl != NULL && state->async_repl->enabled) {
        start_async_external(state, direct);
        return;
    }

    if (state->current_job.active) {
        output_text(state,
                    "gsh: this MVP supports one job at a time; use fg or wait "
                    "for it\r\n");
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (make_pipe(gate, false, "job-pipe") == -1) {
        output_format(state, "gsh: pipe: %s\r\n", strerror(errno));
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        close(gate[0]);
        close(gate[1]);
        output_format(state, "gsh: sigprocmask: %s\r\n", strerror(errno));
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    pid = fault_should_fail("job-fork", EAGAIN) ? -1 : fork();
    if (pid == 0) {
        char release;
        char *environment_storage[CHILD_ENVIRONMENT_CAP];
        char *const *environment = child_command_environment(
            state->variables, NULL, environment_storage);
        char *shell_arguments[] = {(char *)"sh", (char *)"-c",
                                   (char *)state->pending_input, NULL};

        close(gate[1]);
        (void)setpgid(0, 0);
        reset_child_signals();
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        while (read(gate[0], &release, sizeof(release)) == -1 &&
               errno == EINTR) {
        }
        close(gate[0]);
        close(state->tty_fd);
        close(state->signal_pipe[0]);
        close(state->signal_pipe[1]);
        if (state->prompt_worker_fd >= 0) {
            close(state->prompt_worker_fd);
        }
        if (direct != NULL) {
            if (fault_should_fail("exec", EIO)) {
                child_exec_error(direct->argv[0], errno);
            }
            child_exec_direct(direct->argv, path_value, environment);
        }
        if (fault_should_fail("exec", EIO)) {
            child_exec_error("/bin/sh", errno);
        }
        execve("/bin/sh", shell_arguments, environment);
        {
            ssize_t reported = write(
                STDERR_FILENO, "gsh: cannot execute /bin/sh\n", 28);

            (void)reported;
        }
        _exit(127);
    }

    close(gate[0]);
    if (pid == -1) {
        int saved_errno = errno;

        close(gate[1]);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        output_format(state, "gsh: fork: %s\r\n", strerror(saved_errno));
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    initialize_job(&state->current_job, pid, pid, &pid, 1, true, false);
    state->current_job.modes = state->original_modes;

    (void)setpgid(pid, pid);
    if (fault_should_fail("terminal-handoff", EIO) ||
        tcsetattr(state->tty_fd, TCSANOW, &state->original_modes) == -1 ||
        tcsetpgrp(state->tty_fd, pid) == -1) {
        int saved_errno = errno;

        state->current_job.foreground = false;
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        close(gate[1]);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        (void)enter_editor(state);
        output_format(state, "gsh: terminal handoff: %s\r\n",
                      strerror(saved_errno));
        queue_prompt(state);
        return;
    }

    state->terminal_changed = false;
    state->mode = MODE_FOREGROUND;
    close(gate[1]);
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
}

static char *trim_command(char *command)
{
    char *start = command;
    char *end;

    while (*start == ' ' || *start == '\t') {
        start++;
    }
    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return start;
}

static const char *store_path_value(const gsh_variable_store *variables,
                                    const char *default_path)
{
    bool found;
    const char *path =
        gsh_variables_lookup(variables, "PATH", 4, &found);

    return found ? path : default_path;
}

static const char *command_path_override(
    const gsh_native_command *command, const char *path)
{
    size_t index;

    if (command == NULL) {
        return path;
    }
    for (index = 0; index < command->assignment_count; index++) {
        if (memcmp(command->assignments[index], "PATH=", 5) == 0) {
            path = command->assignments[index] + 5;
        }
    }
    return path;
}

static const char *command_path_value(
    const gsh_variable_store *variables, const gsh_native_command *command,
    const char *default_path)
{
    return command_path_override(
        command, store_path_value(variables, default_path));
}

static const char *scoped_command_path_value(
    const pipeline_expansion_scope *scope, unsigned int command_scope,
    const gsh_native_command *command, const char *default_path)
{
    gsh_variable_journal_value_state state;
    const char *path = gsh_variable_journal_lookup_scoped(
        scope->changes, command_scope, "PATH", 4, &state);

    if (state == GSH_VARIABLE_JOURNAL_VALUE_ABSENT) {
        path = store_path_value(scope->base, default_path);
    } else if (state == GSH_VARIABLE_JOURNAL_VALUE_UNSET) {
        path = default_path;
    }
    return command_path_override(command, path);
}

typedef struct {
    shell_state *state;
    pipeline_expansion_scope scope;
    bool mutated;
    bool isolated;
} main_expansion_transaction;

static gsh_native_plan_status main_transaction_command_begin(
    void *opaque, size_t command_index, size_t command_count)
{
    main_expansion_transaction *transaction = opaque;

    transaction->isolated = command_count > 1U;
    if (transaction->isolated) {
        if (command_index == 0) {
            gsh_variable_journal_initialize(
                transaction->state->pipeline_changes, 0);
            transaction->scope.base = transaction->state->variables;
            transaction->scope.changes =
                transaction->state->pipeline_changes;
            transaction->scope.command_count = command_count;
        }
        transaction->scope.current_scope = command_index + 1U;
    }
    return GSH_NATIVE_PLAN_OK;
}

static const char *main_transaction_lookup(void *opaque, const char *name,
                                           size_t name_length, bool *found)
{
    main_expansion_transaction *transaction = opaque;
    const char *value;
    gsh_variable_journal_value_state state;

    if (transaction->isolated) {
        value = gsh_variable_journal_lookup_scoped(
            transaction->scope.changes,
            transaction->scope.current_scope, name, name_length, &state);
        if (state != GSH_VARIABLE_JOURNAL_VALUE_ABSENT) {
            *found = state == GSH_VARIABLE_JOURNAL_VALUE_SET;
            return value;
        }
        return gsh_variables_lookup(transaction->scope.base, name,
                                    name_length, found);
    }
    if (transaction->mutated) {
        value = gsh_variable_journal_lookup(
            transaction->state->variable_commit, name, name_length, &state);
        if (state != GSH_VARIABLE_JOURNAL_VALUE_ABSENT) {
            *found = state == GSH_VARIABLE_JOURNAL_VALUE_SET;
            return value;
        }
    }
    return gsh_variables_lookup(transaction->state->variables, name,
                                name_length, found);
}

static gsh_native_plan_status main_transaction_assign(
    void *opaque, const char *name, size_t name_length, const char *value,
    size_t value_length)
{
    main_expansion_transaction *transaction = opaque;
    unsigned int attributes = assignment_attributes(
        &transaction->state->options);

    if (transaction->isolated) {
        if (gsh_variable_journal_record_scoped(
                transaction->scope.changes,
                transaction->scope.current_scope, name, name_length, value,
                value_length, attributes, attributes) == -1 ||
            gsh_variables_can_apply_journal_scope(
                transaction->scope.base, transaction->scope.changes,
                transaction->scope.current_scope) == -1) {
            output_text(transaction->state,
                        "gsh: pipeline parameter assignment failed\r\n");
            return errno == ENOSPC || errno == E2BIG
                       ? GSH_NATIVE_PLAN_LIMIT
                       : GSH_NATIVE_PLAN_ERROR;
        }
        return GSH_NATIVE_PLAN_OK;
    }
    if (!transaction->mutated) {
        gsh_variable_journal_initialize(
            transaction->state->variable_commit,
            transaction->state->variable_generation);
    }
    if (gsh_variable_journal_record(transaction->state->variable_commit,
                                    name, name_length, value, value_length,
                                    attributes, attributes) == -1 ||
        gsh_variables_can_apply_journal(
            transaction->state->variables,
            transaction->state->variable_commit) == -1) {
        output_text(transaction->state,
                    "gsh: parameter assignment failed\r\n");
        return GSH_NATIVE_PLAN_ERROR;
    }
    transaction->mutated = true;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status main_transaction_parameter_error(
    void *opaque, const char *name, size_t name_length, const char *message,
    size_t message_length, bool default_message)
{
    main_expansion_transaction *transaction = opaque;
    shell_state *state = transaction->state;

    output_text(state, "gsh: ");
    output_push(state, name, name_length);
    output_text(state, ": ");
    if (default_message) {
        output_text(state, "parameter null or not set");
    } else {
        output_push(state, message, message_length);
    }
    output_text(state, "\r\n");
    return GSH_NATIVE_PLAN_ERROR;
}

static gsh_native_plan_status main_transaction_expansion_error(
    void *opaque, const char *message, size_t message_length)
{
    main_expansion_transaction *transaction = opaque;

    output_text(transaction->state, "gsh: arithmetic expansion: ");
    output_push(transaction->state, message, message_length);
    output_text(transaction->state, "\r\n");
    return GSH_NATIVE_PLAN_ERROR;
}

static void commit_main_transaction(main_expansion_transaction *transaction)
{
    if (transaction->isolated || !transaction->mutated) {
        return;
    }
    if (gsh_variables_apply_journal_in_place(
            transaction->state->variables,
            transaction->state->variable_commit) == 0) {
        transaction->state->variable_generation++;
    } else {
        output_text(transaction->state,
                    "gsh: variable transaction commit failed\r\n");
    }
}

static int ensure_main_positionals(shell_state *state)
{
    if (state->positionals != NULL) {
        return 0;
    }
    if (fault_should_fail("positional-allocation", ENOMEM)) {
        return -1;
    }
    state->positionals = &g_interactive_positionals;
    gsh_positionals_initialize(state->positionals);
    return 0;
}

static bool start_async_stateless_redirection(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const gsh_native_command *command)
{
    const gsh_native_redirect *redirect;
    prompt_request request;
    mode_t mask;
    int builtin_status;
    int length;
    ssize_t sent;

    if (!state->prompt_worker_alive || state->prompt_worker_busy ||
        fault_injection_active() || command->assignment_count != 0 ||
        command->redirect_count != 1U ||
        !native_stateless_builtin(command, &builtin_status)) {
        return false;
    }
    redirect = &command->redirects[0];
    if (redirect->operator_kind != GSH_TOKEN_GREAT &&
        redirect->operator_kind != GSH_TOKEN_CLOBBER &&
        redirect->operator_kind != GSH_TOKEN_DGREAT) {
        return false;
    }
    memset(&request, 0, sizeof(request));
    if (redirect->target[0] == '/') {
        length = snprintf(request.directory, sizeof(request.directory),
                          "%s", redirect->target);
    } else if (state->current_directory[0] != '\0') {
        length = snprintf(request.directory, sizeof(request.directory),
                          "%s/%s", state->current_directory,
                          redirect->target);
    } else {
        return false;
    }
    if (length < 0 || (size_t)length >= sizeof(request.directory)) {
        return false;
    }
    mask = umask(0);
    (void)umask(mask);
    request.version = PROMPT_PROTOCOL_VERSION;
    request.type = PROMPT_REQUEST_REDIRECTION;
    request.request_id = state->prompt_next_request_id++;
    request.operator_kind = (uint32_t)redirect->operator_kind;
    request.option_bits = state->options.enabled;
    request.creation_mode = (uint32_t)(0666 & ~mask);
    request.builtin_status = builtin_status;
    do {
        sent = send(state->prompt_worker_fd, &request, sizeof(request), 0);
    } while (sent == -1 && errno == EINTR);
    if (sent != (ssize_t)sizeof(request)) {
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                           errno == ENOBUFS)) {
            return false;
        }
        state->prompt_worker_failures++;
        disable_prompt_worker(state, true);
        return false;
    }
    state->prompt_worker_busy = true;
    state->prompt_active_request_id = request.request_id;
    state->prompt_active_request_type = request.type;
    state->prompt_worker_deadline_ns = 0;
    state->worker_pipeline_negated = pipeline->negated;
    memcpy(state->worker_redirection_target, request.directory,
           (size_t)length + 1U);
    state->mode = MODE_ASYNC_WORKER;
    return true;
}

static void begin_background_wait(shell_state *state,
                                  const gsh_native_pipeline *pipeline)
{
    const gsh_native_command *command = &pipeline->commands[0];
    size_t argument;

    state->wait_target_count = 0;
    state->wait_all = command->argc == 1;
    state->wait_negated = pipeline->negated;
    if (state->wait_all) {
        state->wait_target_count = gsh_background_snapshot(
            &state->background_jobs, state->wait_targets);
    } else {
        for (argument = 1; argument < command->argc; argument++) {
            const char *text = command->argv[argument];
            char *end;
            unsigned long number;
            pid_t target = -1;

            errno = 0;
            number = strtoul(text[0] == '%' ? text + 1U : text, &end, 10);
            if (errno == 0 && *text != '\0' &&
                !(text[0] == '%' && text[1] == '\0') && *end == '\0' &&
                number > 0 && number <= (unsigned long)INT_MAX) {
                target = text[0] == '%'
                             ? gsh_background_job_pid(
                                   &state->background_jobs,
                                   (uint32_t)number)
                             : (pid_t)number;
            }
            state->wait_targets[state->wait_target_count++] = target;
        }
    }
    state->mode = MODE_WAIT;
    (void)finish_background_wait(state);
}

static bool run_planned_main_builtin(shell_state *state,
                                     const gsh_native_pipeline *pipeline)
{
    const gsh_native_command *command;

    if (pipeline->command_count != 1) {
        return false;
    }
    command = &pipeline->commands[0];
    if (start_async_stateless_redirection(state, pipeline, command)) {
        return true;
    }
    if (native_wait_builtin(command) && command->redirect_count == 0) {
        begin_background_wait(state, pipeline);
        return true;
    }
    if (native_alias_builtin(command) && command->redirect_count == 0) {
        const gsh_builtin_io io = {reactor_builtin_output, state};
        bool mutates = native_alias_mutates(command);
        int status;

        if (mutates && ensure_alias_state(state, false) == -1) {
            output_format(state, "gsh: alias allocation: %s\r\n",
                          strerror(errno));
            status = 125;
        } else if (command->assignment_count == 0) {
            status = run_native_alias_builtin(
                command, state->aliases, NULL, &io);
        } else {
            memcpy(state->variable_scratch, state->variables,
                   sizeof(*state->variable_scratch));
            if (apply_native_assignments(
                    state->variable_scratch, NULL, command,
                    &state->options) != GSH_ASSIGNMENT_OK) {
                output_format(state, "gsh: assignment: %s\r\n",
                              strerror(errno));
                status = 1;
            } else {
                status = run_native_alias_builtin(
                    command, state->aliases, NULL, &io);
            }
        }
        if (mutates && state->aliases != NULL) {
            state->alias_generation++;
        }
        state->last_status = pipeline->negated
                                 ? (status == 0 ? 1 : 0)
                                 : status;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    if (native_state_builtin(command) &&
        command->redirect_count == 0) {
        const gsh_builtin_io io = {reactor_builtin_output, state};
        int status;

        if (ensure_main_positionals(state) == -1) {
            output_text(state,
                        "gsh: positional parameter allocation failed\r\n");
            status = 125;
        } else if (apply_native_assignments(
                       state->variables, NULL, command, &state->options) !=
                   GSH_ASSIGNMENT_OK) {
            output_format(state, "gsh: assignment: %s\r\n",
                          strerror(errno));
            status = 1;
        } else {
            status = run_native_state_builtin(
                command, state->variables, state->positionals,
                &state->options, &io);
        }
        state->variable_generation++;
        state->last_status = pipeline->negated
                                 ? (status == 0 ? 1 : 0)
                                 : status;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    if (native_variable_builtin(command) &&
        command->redirect_count == 0 &&
        !native_variable_listing(command)) {
        const gsh_builtin_io io = {reactor_builtin_output, state};
        bool function_mutates = native_function_mutates(command);
        int status;

        if (apply_native_assignments(state->variables, NULL, command,
                                     &state->options) !=
            GSH_ASSIGNMENT_OK) {
            output_format(state, "gsh: assignment: %s\r\n",
                          strerror(errno));
            status = 1;
        } else {
            status = run_native_variable_builtin(
                command, state->variables, NULL, &state->options,
                state->functions, &io);
        }
        if (status == 0 && function_mutates && state->functions != NULL) {
            state->function_generation++;
        }
        state->variable_generation++;
        state->last_status = pipeline->negated
                                 ? (status == 0 ? 1 : 0)
                                 : status;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    if (native_colon_builtin(command) &&
        command->assignment_count != 0 &&
        command->redirect_count == 0) {
        int assignment_status = apply_native_assignments(
            state->variables, NULL, command, &state->options);

        if (assignment_status != GSH_ASSIGNMENT_OK) {
            output_format(state, "gsh: assignment: %s\r\n",
                          strerror(errno));
            state->last_status = 1;
        } else {
            state->last_status = pipeline->negated ? 1 : 0;
        }
        state->variable_generation++;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    if (pipeline->negated || command->redirect_count != 0) {
        return false;
    }
    if (command->argc == 0 && command->assignment_count > 0) {
        if (apply_native_assignments(state->variables, NULL, command,
                                     &state->options) !=
            GSH_ASSIGNMENT_OK) {
            output_format(state, "gsh: assignment: %s\r\n",
                          strerror(errno));
            state->last_status = 1;
        } else {
            state->last_status = 0;
        }
        state->variable_generation++;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    if (native_cd_builtin(command)) {
        const gsh_builtin_io io = {reactor_builtin_output, state};
        const gsh_variable_store *lookup_variables = state->variables;

        if (command->assignment_count != 0) {
            memcpy(state->variable_scratch, state->variables,
                   sizeof(*state->variable_scratch));
            if (apply_native_assignments(
                    state->variable_scratch, NULL, command,
                    &state->options) != GSH_ASSIGNMENT_OK) {
                output_format(state, "gsh: assignment: %s\r\n",
                              strerror(errno));
                state->last_status = 1;
                state->mode = MODE_EDITOR;
                queue_prompt(state);
                return true;
            }
            lookup_variables = state->variable_scratch;
        }

        state->last_status = run_native_cd_builtin(
            command, lookup_variables, state->variables, NULL,
            &state->options, &io, state->current_directory,
            sizeof(state->current_directory));
        if (state->last_status == 0) {
            schedule_prompt_refresh(state);
        }
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    if (command->assignment_count != 0 || command->argc == 0) {
        return false;
    }
    if (native_environment_builtin(command)) {
        const gsh_builtin_io io = {reactor_builtin_output, state};

        state->last_status = run_native_environment_builtin(command, &io);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    if (strcmp(command->argv[0], "exit") == 0) {
        if (command->argc > 2) {
            output_text(state, "gsh: exit: too many operands\r\n");
            state->last_status = 1;
            state->mode = MODE_EDITOR;
            queue_prompt(state);
            return true;
        }
        if (command->argc == 2) {
            char *end;
            long status = strtol(command->argv[1], &end, 10);

            if (*end != '\0') {
                output_text(state,
                            "gsh: exit: numeric argument required\r\n");
                state->last_status = 2;
                state->running = false;
                return true;
            }
            state->last_status = (int)((unsigned long)status & 255U);
        }
        state->running = false;
        return true;
    }
    return false;
}

static void run_fg(shell_state *state)
{
    if (state->async_repl != NULL && state->async_repl->enabled) {
        int cell_index = gsh_async_repl_latest_job(state->async_repl);

        if (cell_index < 0 ||
            gsh_async_repl_focus(state->async_repl, cell_index) == -1) {
            output_text(state, "gsh: fg: no current job\r\n");
            state->last_status = 1;
        } else {
            gsh_async_cell *cell = &state->async_repl->cells[cell_index];

            if (cell->state == GSH_ASYNC_STOPPED) {
                (void)signal_managed_job(state, cell_index, SIGCONT);
                gsh_async_repl_mark_running(state->async_repl, cell_index);
                (void)gsh_async_repl_focus(state->async_repl, cell_index);
            }
            output_format(state,
                          "[focused cell %llu; Ctrl-] returns to editor]"
                          "\r\n",
                          (unsigned long long)cell->id);
            state->last_status = 0;
        }
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (!state->current_job.active) {
        output_text(state, "gsh: fg: no current job\r\n");
        state->last_status = 1;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (tcsetattr(state->tty_fd, TCSANOW, &state->current_job.modes) == -1 ||
        tcsetpgrp(state->tty_fd, state->current_job.pgid) == -1) {
        output_format(state, "gsh: fg: %s\r\n", strerror(errno));
        (void)enter_editor(state);
        queue_prompt(state);
        return;
    }
    state->terminal_changed = false;
    state->current_job.foreground = true;
    state->mode = MODE_FOREGROUND;
    if (state->current_job.stopped) {
        size_t index;

        state->current_job.stopped = false;
        for (index = 0; index < state->current_job.member_count; index++) {
            if (state->current_job.member_states[index] ==
                JOB_MEMBER_STOPPED) {
                state->current_job.member_states[index] =
                    JOB_MEMBER_RUNNING;
            }
        }
        (void)kill(-state->current_job.pgid, SIGCONT);
    }
}

static void run_bg(shell_state *state)
{
    if (state->async_repl != NULL && state->async_repl->enabled) {
        int cell_index = gsh_async_repl_latest_job(state->async_repl);

        if (cell_index < 0 ||
            state->async_repl->cells[cell_index].state !=
                GSH_ASYNC_STOPPED) {
            output_text(state, "gsh: bg: no stopped job\r\n");
            state->last_status = 1;
        } else {
            (void)signal_managed_job(state, cell_index, SIGCONT);
            gsh_async_repl_mark_running(state->async_repl, cell_index);
            state->last_status = 0;
        }
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (!state->current_job.active || !state->current_job.stopped) {
        output_text(state, "gsh: bg: no stopped job\r\n");
        state->last_status = 1;
    } else if (kill(-state->current_job.pgid, SIGCONT) == -1) {
        output_format(state, "gsh: bg: %s\r\n", strerror(errno));
        state->last_status = 1;
    } else {
        size_t index;

        state->current_job.stopped = false;
        state->current_job.foreground = false;
        for (index = 0; index < state->current_job.member_count; index++) {
            if (state->current_job.member_states[index] ==
                JOB_MEMBER_STOPPED) {
                state->current_job.member_states[index] =
                    JOB_MEMBER_RUNNING;
            }
        }
        state->last_status = 0;
        output_format(state, "[continued %ld]\r\n",
                      (long)state->current_job.pid);
    }
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

static bool begin_native_list(shell_state *state)
{
    const gsh_ast_node *program;
    const gsh_ast_node *list;
    size_t child;
    bool parent_owned = false;

    if (state->pending_parse.status != GSH_PARSE_OK ||
        state->pending_parse.root >= state->parse_storage->node_count) {
        return false;
    }
    program = &state->parse_storage->nodes[state->pending_parse.root];
    if (program->kind != GSH_AST_PROGRAM ||
        program->first_child == GSH_AST_NONE ||
        state->parse_storage->nodes[program->first_child].next_sibling !=
            GSH_AST_NONE) {
        return false;
    }
    list = &state->parse_storage->nodes[program->first_child];
    if (list->kind != GSH_AST_LIST) {
        return false;
    }
    child = list->first_child;
    while (child != GSH_AST_NONE) {
        if ((state->parse_storage->nodes[child].flags &
             GSH_AST_FLAG_ASYNC) != 0) {
            parent_owned = true;
        }
        if (native_list_node_is_wait(state, child)) {
            parent_owned = true;
        }
        child = state->parse_storage->nodes[child].next_sibling;
    }
    if (!parent_owned || !native_command_is_supported(state)) {
        return false;
    }
    state->pending_list_active = true;
    state->pending_list_next = list->first_child;
    state->pending_and_or_active = false;
    state->pending_and_or_next = GSH_AST_NONE;
    continue_native_list(state);
    return true;
}

static size_t single_function_definition(const shell_state *state)
{
    size_t node = state->pending_parse.root;
    static const gsh_ast_kind wrappers[] = {
        GSH_AST_PROGRAM, GSH_AST_LIST, GSH_AST_AND_OR, GSH_AST_PIPELINE,
    };
    size_t index;

    if (state->pending_parse.status != GSH_PARSE_OK) {
        return GSH_AST_NONE;
    }
    for (index = 0; index < sizeof(wrappers) / sizeof(wrappers[0]);
         index++) {
        const gsh_ast_node *current;

        if (node >= state->parse_storage->node_count) {
            return GSH_AST_NONE;
        }
        current = &state->parse_storage->nodes[node];
        if (current->kind != wrappers[index] ||
            current->first_child == GSH_AST_NONE ||
            state->parse_storage->nodes[current->first_child].next_sibling !=
                GSH_AST_NONE ||
            (current->flags & (GSH_AST_FLAG_ASYNC |
                               GSH_AST_FLAG_NEGATED)) != 0) {
            return GSH_AST_NONE;
        }
        node = current->first_child;
    }
    return node < state->parse_storage->node_count &&
                   state->parse_storage->nodes[node].kind == GSH_AST_FUNCTION
               ? node
               : GSH_AST_NONE;
}

static bool run_function_definition(shell_state *state)
{
    size_t node_index = single_function_definition(state);
    const gsh_ast_node *node;
    gsh_word_ref name;
    int changed;

    if (node_index == GSH_AST_NONE) {
        return false;
    }
    node = &state->parse_storage->nodes[node_index];
    name = state->parse_storage->words[node->first_word];
    if (special_builtin_name(state->pending_input + name.begin,
                             name.end - name.begin)) {
        output_text(state,
                    "gsh: function name is a special builtin\r\n");
        state->last_status = 1;
    } else if (ensure_function_state(state, false) == -1) {
        output_format(state, "gsh: function allocation: %s\r\n",
                      strerror(errno));
        state->last_status = 125;
    } else {
        changed = gsh_functions_set(
            state->functions, state->function_scratch,
            state->pending_input, state->pending_input_length,
            state->parse_storage, node_index, false);
        if (changed == -1 && errno == ENOSPC &&
            ensure_function_state(state, true) == 0) {
            changed = gsh_functions_set(
                state->functions, state->function_scratch,
                state->pending_input, state->pending_input_length,
                state->parse_storage, node_index, false);
        }
        if (changed == -1) {
            output_format(state, "gsh: function definition: %s\r\n",
                          strerror(errno));
            state->last_status = errno == ENOSPC ? 125 : 1;
        } else {
            state->last_status = 0;
            state->function_generation++;
        }
    }
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return true;
}

static bool reactor_word_is(const char *input, gsh_word_ref word,
                            const char *literal);

static bool pure_function_status(const gsh_function_store *functions,
                                 const gsh_function_entry *entry,
                                 int *status)
{
    const gsh_parse_storage *storage = &functions->programs;
    const char *input = gsh_functions_text(functions);
    size_t node_index = entry->node_offset;
    bool negated = false;

    for (;;) {
        const gsh_ast_node *node;
        size_t child;

        if (node_index >= storage->node_count) {
            return false;
        }
        node = &storage->nodes[node_index];
        if ((node->flags & GSH_AST_FLAG_ASYNC) != 0 ||
            node->redirect_count != 0) {
            return false;
        }
        if (node->kind == GSH_AST_FUNCTION) {
            child = node->first_child;
        } else if (node->kind == GSH_AST_BRACE_GROUP ||
                   node->kind == GSH_AST_PROGRAM ||
                   node->kind == GSH_AST_LIST ||
                   node->kind == GSH_AST_AND_OR ||
                   node->kind == GSH_AST_PIPELINE) {
            child = node->first_child;
            if (node->kind == GSH_AST_PIPELINE &&
                (node->flags & GSH_AST_FLAG_NEGATED) != 0) {
                negated = !negated;
            }
        } else if (node->kind == GSH_AST_SIMPLE &&
                   node->word_count == 1U) {
            gsh_word_ref word = storage->words[node->first_word];
            int value;

            if (reactor_word_is(input, word, ":") ||
                reactor_word_is(input, word, "true")) {
                value = 0;
            } else if (reactor_word_is(input, word, "false")) {
                value = 1;
            } else {
                return false;
            }
            *status = negated ? (value == 0 ? 1 : 0) : value;
            return true;
        } else {
            return false;
        }
        if (child == GSH_AST_NONE || child >= storage->node_count ||
            storage->nodes[child].next_sibling != GSH_AST_NONE) {
            return false;
        }
        node_index = child;
    }
}

static bool run_pure_function(shell_state *state,
                              const gsh_native_pipeline *pipeline)
{
    const gsh_native_command *command;
    const gsh_function_entry *function;
    int status;

    if (fault_injection_active() || pipeline->command_count != 1U) {
        return false;
    }
    command = &pipeline->commands[0];
    function = command->argc == 0
                   ? NULL
                   : gsh_functions_lookup(state->functions,
                                          command->argv[0],
                                          strlen(command->argv[0]));
    if (function == NULL || command->redirect_count != 0 ||
        !pure_function_status(state->functions, function, &status)) {
        return false;
    }
    if (apply_native_assignments(state->variables, NULL, command,
                                 &state->options) !=
        GSH_ASSIGNMENT_OK) {
        output_format(state, "gsh: function assignment: %s\r\n",
                      strerror(errno));
        status = 1;
    } else if (command->assignment_count != 0) {
        state->variable_generation++;
    }
    if (pipeline->negated) {
        status = status == 0 ? 1 : 0;
    }
    state->last_status = status;
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return true;
}

static void dispatch_pending(shell_state *state)
{
    char direct_storage[LINE_CAP];
    simple_command direct = {0};
    char *command = trim_command((char *)state->pending_input);

    if (*command == '\0') {
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    if (strcmp(command, "fg") == 0) {
        run_fg(state);
        return;
    }
    if (strcmp(command, "bg") == 0) {
        run_bg(state);
        return;
    }
    if (strcmp(command, "rt") == 0) {
        output_format(state,
                      "reactor cycles=%llu max=%.3fms deadline=5.000ms "
                      "misses=%llu dispatches=%llu dispatch_max=%.3fms "
                      "dispatch_misses=%llu overloads=%llu direct=%llu "
                      "native=%llu "
                      "shell=%llu "
                      "parsed=%llu parse_failures=%llu job=%s worker=%s "
                      "busy=%u timeouts=%llu failures=%llu stale=%llu "
                      "async_jobs=%zu focus=%s\r\n",
                      (unsigned long long)state->reactor_cycles,
                      (double)state->reactor_max_ns / 1000000.0,
                      (unsigned long long)state->reactor_misses,
                      (unsigned long long)state->dispatch_cycles,
                      (double)state->dispatch_max_ns / 1000000.0,
                      (unsigned long long)state->dispatch_misses,
                      (unsigned long long)state->overloads,
                      (unsigned long long)state->direct_dispatches,
                      (unsigned long long)state->native_pipeline_dispatches,
                      (unsigned long long)state->shell_dispatches,
                      (unsigned long long)state->parsed_dispatches,
                      (unsigned long long)state->parse_failures,
                      state->current_job.active ? "active" : "idle",
                      state->prompt_worker_alive ? "on" : "off",
                      state->prompt_worker_busy ? 1U : 0U,
                      (unsigned long long)state->prompt_worker_timeouts,
                      (unsigned long long)state->prompt_worker_failures,
                      (unsigned long long)state->prompt_stale_results,
                      gsh_async_repl_job_count(state->async_repl),
                      gsh_async_repl_focused_job(state->async_repl) >= 0
                          ? "job"
                          : "editor");
        state->last_status = 0;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (strcmp(command, "help") == 0) {
        output_text(state,
                    "builtins: cd [path], exit [status], fg, bg, rt, help\r\n"
                    "simple commands use native execve; shell syntax falls "
                    "back to /bin/sh -c\r\n");
        state->last_status = 0;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    {
        main_expansion_transaction transaction = {.state = state};
        char *positional_view[GSH_POSITIONAL_CAP];
        char option_flags[GSH_OPTION_FLAG_CAP];
        size_t positional_count =
            gsh_positionals_count(state->positionals);
        gsh_parse_result parsed = state->pending_parse;
        gsh_native_plan_status plan_status = GSH_NATIVE_PLAN_UNSUPPORTED;
        bool deferred_work = false;
        gsh_native_expansion_context expansion = {
            .last_status = state->last_status,
            .shell_pid = (long)state->shell_pgid,
            .last_background_pid = state->last_background_pid,
            .parameter_zero = state->parameter_zero,
            .positional_parameters = positional_count == 0
                                         ? NULL
                                         : positional_view,
            .positional_count = positional_count,
            .option_flags = option_flags,
            .environment = NULL,
            .variable_lookup = main_transaction_lookup,
            .variable_assign = main_transaction_assign,
            .parameter_error = main_transaction_parameter_error,
            .expansion_error = main_transaction_expansion_error,
            .variable_opaque = &transaction,
            .command_begin = main_transaction_command_begin,
            .command_opaque = &transaction,
            .pathname_mode =
                gsh_options_enabled(&state->options, GSH_OPTION_NOGLOB)
                    ? GSH_NATIVE_PATHNAME_PREFLIGHT
                    : GSH_NATIVE_PATHNAME_REJECT,
            .defer_complex_patterns = true,
            .deferred_work = &deferred_work,
            .nounset = gsh_options_enabled(&state->options,
                                            GSH_OPTION_NOUNSET),
        };

        gsh_options_flags(&state->options, option_flags);
        gsh_positionals_view(state->positionals, positional_view);
        if (parsed.status == GSH_PARSE_OK) {
            state->parsed_dispatches++;
            if (run_function_definition(state)) {
                state->native_pipeline_dispatches++;
                return;
            }
            if (begin_native_list(state)) {
                state->native_pipeline_dispatches++;
                return;
            }
            if (try_native_reactor_compound(state)) {
                state->native_pipeline_dispatches++;
                return;
            }
            plan_status = gsh_native_plan_pipeline_with_context(
                state->pending_input, state->parse_storage, parsed.root,
                &expansion, state->native_pipeline);
        } else {
            state->parse_failures++;
        }
        if (plan_status == GSH_NATIVE_PLAN_ERROR) {
            state->last_status = 1;
            state->mode = MODE_EDITOR;
            queue_prompt(state);
            return;
        }
        if (prepare_simple_command(state->pending_input, direct_storage,
                                   &direct) &&
            gsh_functions_lookup(state->functions, direct.argv[0],
                                 strlen(direct.argv[0])) == NULL) {
            state->direct_dispatches++;
            start_external(state, &direct);
            return;
        }
        if (plan_status == GSH_NATIVE_PLAN_OK && !deferred_work) {
            commit_main_transaction(&transaction);
        }
        if (plan_status == GSH_NATIVE_PLAN_OK && !deferred_work &&
            run_planned_main_builtin(state, state->native_pipeline)) {
            state->native_pipeline_dispatches++;
            return;
        }
        if (plan_status == GSH_NATIVE_PLAN_OK && !deferred_work &&
            run_pure_function(state, state->native_pipeline)) {
            state->native_pipeline_dispatches++;
            return;
        }
        if (plan_status == GSH_NATIVE_PLAN_OK && !deferred_work &&
            !native_pipeline_requires_evaluator(state->native_pipeline) &&
            !(state->native_pipeline->command_count == 1U &&
              state->native_pipeline->commands[0].argc != 0 &&
              gsh_functions_lookup(
                  state->functions,
                  state->native_pipeline->commands[0].argv[0],
                  strlen(state->native_pipeline->commands[0].argv[0])) !=
                  NULL) &&
            native_pipeline_is_supported_scoped(
                state->native_pipeline, state->default_path,
                state->variables,
                transaction.isolated ? &transaction.scope : NULL)) {
            int builtin_status;

            state->native_pipeline_dispatches++;
            if (state->native_pipeline->command_count == 1 &&
                state->native_pipeline->commands[0].redirect_count == 0 &&
                native_stateless_builtin(
                    &state->native_pipeline->commands[0],
                    &builtin_status)) {
                if (state->native_pipeline->negated) {
                    builtin_status = builtin_status == 0 ? 1 : 0;
                }
                state->last_status = builtin_status;
                state->mode = MODE_EDITOR;
                queue_prompt(state);
                return;
            }
            start_native_pipeline(
                state, state->native_pipeline,
                transaction.isolated ? &transaction.scope : NULL);
            return;
        }
        if (parsed.status == GSH_PARSE_OK &&
            native_command_is_supported(state)) {
            state->native_pipeline_dispatches++;
            start_native_compound(state, parsed.root);
            return;
        }
    }
    if (state->pending_alias_expanded) {
        output_text(state,
                    "gsh: native alias expansion produced an unsupported "
                    "command\r\n");
        state->last_status = 2;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    state->shell_dispatches++;
    start_external(state, NULL);
}

static bool command_has_status_dependency(const char *command,
                                          size_t length)
{
    size_t offset;
    bool single_quoted = false;

    for (offset = 0; offset + 1U < length; offset++) {
        unsigned char byte = (unsigned char)command[offset];

        if (byte == '\\' && !single_quoted) {
            offset++;
            continue;
        }
        if (byte == '\'') {
            single_quoted = !single_quoted;
            continue;
        }
        if (!single_quoted && byte == '$' && command[offset + 1U] == '?') {
            return true;
        }
    }
    return false;
}

static bool command_has_isolated_execution(const shell_state *state)
{
    size_t node_index = state->pending_parse.root;
    unsigned int depth;

    for (depth = 0; depth < 6U; depth++) {
        const gsh_ast_node *node;
        size_t child;

        if (node_index == GSH_AST_NONE ||
            node_index >= state->parse_storage->node_count) {
            return false;
        }
        node = &state->parse_storage->nodes[node_index];
        if ((node->flags & GSH_AST_FLAG_ASYNC) != 0) {
            return false;
        }
        if (node->kind == GSH_AST_SUBSHELL) {
            return true;
        }
        if (node->kind != GSH_AST_PROGRAM && node->kind != GSH_AST_LIST &&
            node->kind != GSH_AST_AND_OR && node->kind != GSH_AST_PIPELINE) {
            return false;
        }
        child = node->first_child;
        if (child == GSH_AST_NONE ||
            child >= state->parse_storage->node_count) {
            return false;
        }
        if (state->parse_storage->nodes[child].next_sibling != GSH_AST_NONE) {
            return node->kind == GSH_AST_PIPELINE;
        }
        node_index = child;
    }
    return false;
}

static size_t managed_assignment_name_length(const char *input,
                                             gsh_word_ref word)
{
    size_t offset;

    for (offset = word.begin; offset < word.end; offset++) {
        if (input[offset] == '=') {
            size_t length = offset - word.begin;

            return gsh_variable_name_is_valid(input + word.begin, length)
                       ? length
                       : 0;
        }
    }
    return 0;
}

static bool managed_plain_word(const char *input, gsh_word_ref word)
{
    size_t offset;

    if (word.begin >= word.end) {
        return false;
    }
    for (offset = word.begin; offset < word.end; offset++) {
        unsigned char byte = (unsigned char)input[offset];

        if (byte == '\\' || byte == '\'' || byte == '"' || byte == '$' ||
            byte == 0x60U || byte == '~') {
            return false;
        }
    }
    return true;
}

static bool managed_word_is(const char *input, gsh_word_ref word,
                            const char *text)
{
    size_t length = strlen(text);

    return managed_plain_word(input, word) &&
           word.end - word.begin == length &&
           memcmp(input + word.begin, text, length) == 0;
}

static bool managed_variable_affects_launch(const shell_state *state,
                                            gsh_word_ref word,
                                            size_t name_length)
{
    bool is_set;
    unsigned int attributes;
    const char *name = state->pending_input + word.begin;

    if (name_length == 4U && memcmp(name, "PATH", 4) == 0) {
        return true;
    }
    if (gsh_options_enabled(&state->options, GSH_OPTION_ALLEXPORT)) {
        return true;
    }
    return gsh_variables_get_state(state->variables, name, name_length,
                                   &is_set, &attributes) &&
           (attributes & GSH_VARIABLE_EXPORTED) != 0;
}

static bool managed_simple_blocks_independent(const shell_state *state,
                                              const gsh_ast_node *node)
{
    static const char *const launch_mutators[] = {
        ".",       "alias",  "cd",       "command", "eval",
        "exec",    "export", "getopts",  "read",    "readonly",
        "set",     "shift",  "trap",     "ulimit",  "umask",
        "unalias", "unset",
    };
    size_t assignment_count = 0;
    size_t index;
    gsh_word_ref command;
    bool assignment_blocks = false;

    if (node->first_word > state->parse_storage->word_count ||
        node->word_count >
            state->parse_storage->word_count - node->first_word) {
        return true;
    }
    while (assignment_count < node->word_count) {
        gsh_word_ref word = state->parse_storage->words[
            node->first_word + assignment_count];

        if (word.begin > word.end ||
            word.end > state->pending_input_length) {
            return true;
        }
        {
            size_t name_length = managed_assignment_name_length(
                state->pending_input, word);

            if (name_length == 0) {
                break;
            }
            if (managed_variable_affects_launch(state, word,
                                                name_length)) {
                assignment_blocks = true;
            }
        }
        assignment_count++;
    }
    if (assignment_count == node->word_count) {
        return assignment_blocks;
    }
    if (assignment_blocks) {
        return true;
    }
    command = state->parse_storage->words[
        node->first_word + assignment_count];
    if (!managed_plain_word(state->pending_input, command)) {
        return true;
    }
    for (index = 0;
         index < sizeof(launch_mutators) / sizeof(launch_mutators[0]);
         index++) {
        if (managed_word_is(state->pending_input, command,
                            launch_mutators[index])) {
            return true;
        }
    }
    return gsh_functions_lookup(
               state->functions, state->pending_input + command.begin,
               command.end - command.begin) != NULL;
}

static bool command_blocks_independent(shell_state *state)
{
    size_t stack[GSH_PARSE_NODE_CAP];
    size_t stack_count = 0;
    size_t visited = 0;

    if (state->pending_parse.status != GSH_PARSE_OK ||
        state->pending_alias_expanded ||
        state->pending_parse.root >= state->parse_storage->node_count) {
        return true;
    }
    stack[stack_count++] = state->pending_parse.root;
    while (stack_count != 0 && visited++ < GSH_PARSE_NODE_CAP) {
        size_t node_index = stack[--stack_count];
        const gsh_ast_node *node;
        size_t child;
        size_t sibling_count = 0;

        if (node_index >= state->parse_storage->node_count) {
            return true;
        }
        node = &state->parse_storage->nodes[node_index];
        if ((node->flags & GSH_AST_FLAG_ASYNC) != 0 ||
            node->kind == GSH_AST_SUBSHELL) {
            continue;
        }
        if (node->kind == GSH_AST_FUNCTION) {
            return true;
        }
        if (node->kind == GSH_AST_FOR) {
            gsh_word_ref name;
            size_t name_length;

            if (node->word_count == 0 ||
                node->first_word >= state->parse_storage->word_count) {
                return true;
            }
            name = state->parse_storage->words[node->first_word];
            if (name.begin > name.end ||
                name.end > state->pending_input_length) {
                return true;
            }
            name_length = name.end - name.begin;
            if (managed_variable_affects_launch(state, name,
                                                name_length)) {
                return true;
            }
        }
        if (node->kind == GSH_AST_SIMPLE &&
            managed_simple_blocks_independent(state, node)) {
            return true;
        }
        if (node->kind == GSH_AST_PIPELINE &&
            node->first_child != GSH_AST_NONE &&
            node->first_child >= state->parse_storage->node_count) {
            return true;
        }
        if (node->kind == GSH_AST_PIPELINE &&
            node->first_child != GSH_AST_NONE &&
            state->parse_storage->nodes[node->first_child].next_sibling !=
                GSH_AST_NONE) {
            continue;
        }
        child = node->first_child;
        while (child != GSH_AST_NONE &&
               sibling_count++ < GSH_PARSE_NODE_CAP) {
            if (child >= state->parse_storage->node_count ||
                stack_count == GSH_PARSE_NODE_CAP) {
                return true;
            }
            stack[stack_count++] = child;
            child = state->parse_storage->nodes[child].next_sibling;
        }
        if (child != GSH_AST_NONE) {
            return true;
        }
    }
    return stack_count != 0;
}

/* ── Control Bypasses Work, State Commits Stay Ordered ───────────
 * Completion order cannot decide the shell's directory or variable state.
 * Barrier cells therefore wait for every older cell before they can commit.
 * A compound form alone is not a fence for independent external work.
 * Only a pending mutation that can alter launch state blocks that later work.
 * REPL controls such as fg must still reach the job that causes that wait.
 * Their explicit control class bypasses work dependencies without mutating them.
 * ─────────────────────────────────────────────────────────────── */

static bool command_is_session_barrier(shell_state *state)
{
    char storage[LINE_CAP];
    simple_command command = {0};

    if (state->pending_parse.status != GSH_PARSE_OK ||
        state->pending_alias_expanded) {
        return true;
    }
    if (command_has_isolated_execution(state)) {
        return false;
    }
    if (!prepare_simple_command(state->pending_input, storage, &command)) {
        return true;
    }
    return gsh_functions_lookup(state->functions, command.argv[0],
                                strlen(command.argv[0])) != NULL;
}

static bool command_is_managed_control(const char *command, size_t length)
{
    static const char *const controls[] = {"fg", "bg", "rt"};
    size_t begin = 0;
    size_t end = length;
    size_t index;

    while (begin < end &&
           (command[begin] == ' ' || command[begin] == '\t')) {
        begin++;
    }
    while (end > begin &&
           (command[end - 1U] == ' ' || command[end - 1U] == '\t')) {
        end--;
    }
    if (end - begin >= 4U && memcmp(command + begin, "exit", 4) == 0 &&
        (end - begin == 4U || command[begin + 4U] == ' ' ||
         command[begin + 4U] == '\t')) {
        return true;
    }
    for (index = 0; index < sizeof(controls) / sizeof(controls[0]); index++) {
        size_t control_length = strlen(controls[index]);

        if (end - begin == control_length &&
            memcmp(command + begin, controls[index], control_length) == 0) {
            return true;
        }
    }
    return false;
}

static int accept_managed_submission(shell_state *state,
                                     size_t command_length)
{
    char prompt[GSH_ASYNC_PROMPT_CAP];
    bool status_dependency;
    bool barrier;
    bool blocks_independent;
    bool control;
    int cell_index;

    (void)active_prompt_text(state, prompt);
    status_dependency = command_has_status_dependency(
        state->pending_line, command_length);
    control = command_is_managed_control(state->pending_line,
                                         command_length);
    barrier = !control &&
              (status_dependency || command_is_session_barrier(state));
    blocks_independent = barrier && command_blocks_independent(state);
    cell_index = gsh_async_repl_accept(
        state->async_repl, prompt, state->pending_line, command_length,
        barrier, blocks_independent, status_dependency, control);
    if (cell_index < 0) {
        state->overloads++;
        state->last_status = 125;
        return -1;
    }
    return cell_index;
}

static void accept_line(shell_state *state)
{
    size_t candidate_length = state->pending_len + state->line_len;
    gsh_parse_result parsed;

    if (candidate_length >= sizeof(state->pending_line)) {
        state->overloads++;
        state->pending_len = 0;
        state->pending_line[0] = '\0';
        reset_pending_input(state);
        state->line_len = 0;
        state->line[0] = '\0';
        state->continuation_prompt = false;
        (void)output_text(state,
                          "\r\ngsh: command exceeds input limit\r\n");
        queue_prompt(state);
        return;
    }
    memcpy(state->pending_line + state->pending_len, state->line,
           state->line_len);
    state->pending_line[candidate_length] = '\0';
    if (gsh_aliases_count(state->aliases) != 0) {
        parsed = gsh_alias_parse(
            state->pending_line, candidate_length, state->aliases,
            state->alias_expansion, GSH_ALIAS_EXPANSION_CAP,
        state->parse_storage, &state->pending_input,
            &state->pending_input_length);
        state->pending_alias_expanded =
            state->pending_input_length != candidate_length ||
            memcmp(state->pending_input, state->pending_line,
                   candidate_length) != 0;
    } else {
        reset_pending_input(state);
        parsed = gsh_parse(state->pending_line, candidate_length,
                           state->parse_storage);
    }
    state->pending_parse = parsed;
    state->line_len = 0;
    state->line[0] = '\0';
    state->escape_state = 0;
    (void)output_text(state, "\r\n");
    if (parsed.status == GSH_PARSE_INCOMPLETE) {
        if (candidate_length + 1U >= sizeof(state->pending_line)) {
            state->overloads++;
            state->pending_len = 0;
            state->pending_line[0] = '\0';
            reset_pending_input(state);
            state->continuation_prompt = false;
            output_text(state, "gsh: command exceeds input limit\r\n");
            queue_prompt(state);
            return;
        }
        state->pending_line[candidate_length++] = '\n';
        state->pending_line[candidate_length] = '\0';
        state->pending_len = candidate_length;
        reset_pending_input(state);
        state->continuation_prompt = true;
        queue_prompt(state);
        return;
    }
    state->pending_len = 0;
    state->continuation_prompt = false;
    if (state->async_repl != NULL && state->async_repl->enabled) {
        if (candidate_length == 0) {
            queue_prompt(state);
            return;
        }
        if (accept_managed_submission(state, candidate_length) == -1) {
            (void)raw_output_push(state, "\a", 1);
            if (memchr(state->pending_line, '\n', candidate_length) == NULL) {
                memcpy(state->line, state->pending_line,
                       candidate_length + 1U);
                state->line_len = candidate_length;
            }
        }
        state->pending_line[0] = '\0';
        reset_pending_input(state);
        queue_prompt(state);
        return;
    }
    state->mode = MODE_DISPATCH;
}

static void erase_last_character(shell_state *state)
{
    if (state->line_len == 0) {
        (void)output_text(state, "\a");
        return;
    }
    state->line_len--;
    while (state->line_len > 0 &&
           ((unsigned char)state->line[state->line_len] & 0xc0U) == 0x80U) {
        state->line_len--;
    }
    state->line[state->line_len] = '\0';
    queue_redraw(state);
}

static void process_input(shell_state *state)
{
    unsigned char byte;
    ssize_t count = read(state->tty_fd, &byte, sizeof(byte));
    int focused;

    if (count == 0) {
        state->running = false;
        return;
    }
    if (count == -1) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            state->running = false;
        }
        return;
    }

    focused = state->async_repl == NULL
                  ? -1
                  : gsh_async_repl_focused_job(state->async_repl);
    if (focused >= 0) {
        if (byte == 0x1dU) {
            gsh_async_repl_unfocus(state->async_repl);
            queue_redraw(state);
        } else if (byte == 0x03U) {
            (void)signal_managed_job(state, focused, SIGINT);
        } else if (byte == 0x1aU) {
            stop_managed_job(state, focused);
        } else if (gsh_async_repl_queue_input(
                       state->async_repl, focused, (const char *)&byte,
                       sizeof(byte)) == -1) {
            gsh_async_repl_unfocus(state->async_repl);
            (void)raw_output_push(state, "\a", 1);
        }
        return;
    }

    if (state->async_repl != NULL && state->async_repl->enabled &&
        byte == 0x03U) {
        cancel_editor_line(state);
        return;
    }
    if (state->async_repl != NULL && state->async_repl->enabled &&
        byte == 0x1aU) {
        (void)raw_output_push(state, "\a", 1);
        return;
    }

    if (state->escape_state == 1) {
        state->escape_state = (byte == '[' || byte == 'O') ? 2U : 0U;
        return;
    }
    if (state->escape_state == 2) {
        if (byte >= 0x40U && byte <= 0x7eU) {
            state->escape_state = 0;
        }
        return;
    }
    if (byte == 0x1bU) {
        state->escape_state = 1;
        return;
    }
    if (byte == '\r' || byte == '\n') {
        accept_line(state);
        return;
    }
    if (byte == 0x04U) {
        if (state->line_len == 0) {
            if (state->continuation_prompt) {
                cancel_editor_line(state);
                return;
            }
            if (state->current_job.active ||
                gsh_async_repl_job_count(state->async_repl) != 0) {
                output_text(state, "\r\ngsh: a job is still active\r\n");
                queue_prompt(state);
            } else {
                state->running = false;
            }
        }
        return;
    }
    if (byte == 0x7fU || byte == 0x08U) {
        erase_last_character(state);
        return;
    }
    if (byte == 0x15U) {
        state->line_len = 0;
        state->line[0] = '\0';
        queue_redraw(state);
        return;
    }
    if (byte == 0x0cU) {
        if (state->async_repl != NULL && state->async_repl->enabled) {
            state->async_repl->render_pending = true;
            return;
        }
        (void)output_text(state, "\033[2J\033[H");
        queue_prompt(state);
        (void)output_push(state, state->line, state->line_len);
        return;
    }
    if ((byte >= 0x20U || byte == '\t') && state->line_len < LINE_CAP - 1) {
        state->line[state->line_len++] = (char)byte;
        state->line[state->line_len] = '\0';
        if (state->async_repl != NULL && state->async_repl->enabled) {
            state->async_repl->render_pending = true;
        } else {
            (void)output_push(state, (const char *)&byte, 1);
        }
    } else if (state->line_len >= LINE_CAP - 1) {
        state->overloads++;
        (void)output_text(state, "\a");
    }
}

static int load_managed_submission(shell_state *state, int cell_index)
{
    const gsh_async_cell *cell = &state->async_repl->cells[cell_index];

    if (!cell->occupied || cell->command_length >= sizeof(state->pending_line)) {
        errno = EINVAL;
        return -1;
    }
    memcpy(state->pending_line, cell->command, cell->command_length + 1U);
    state->pending_len = 0;
    if (gsh_aliases_count(state->aliases) != 0) {
        state->pending_parse = gsh_alias_parse(
            state->pending_line, cell->command_length, state->aliases,
            state->alias_expansion, GSH_ALIAS_EXPANSION_CAP,
            state->parse_storage, &state->pending_input,
            &state->pending_input_length);
        state->pending_alias_expanded =
            state->pending_input_length != cell->command_length ||
            memcmp(state->pending_input, state->pending_line,
                   cell->command_length) != 0;
    } else {
        reset_pending_input(state);
        state->pending_parse = gsh_parse(
            state->pending_line, cell->command_length,
            state->parse_storage);
    }
    return 0;
}

static bool managed_state_lane_busy(const shell_state *state)
{
    return state->async_state_cell >= 0 || state->current_job.active ||
           state->mode == MODE_ASYNC_WORKER || state->mode == MODE_WAIT ||
           state->pending_list_active || state->pending_and_or_active;
}

static void finish_managed_state_cell(shell_state *state)
{
    int cell_index = state->async_state_cell;

    if (cell_index < 0 || state->mode != MODE_EDITOR ||
        state->current_job.active || state->mode == MODE_ASYNC_WORKER ||
        state->mode == MODE_WAIT || state->pending_list_active ||
        state->pending_and_or_active) {
        return;
    }
    if (state->async_repl->cells[cell_index].state == GSH_ASYNC_STARTING) {
        gsh_async_repl_finish(state->async_repl, cell_index,
                              state->last_status << 8, true);
    }
    state->async_state_cell = -1;
    state->async_capture_cell = -1;
    state->async_dispatch_cell = -1;
}

static void record_dispatch_duration(shell_state *state, uint64_t start)
{
    uint64_t end = monotonic_ns();
    uint64_t duration = end >= start ? end - start : 0;

    state->dispatch_cycles++;
    if (duration > state->dispatch_max_ns) {
        state->dispatch_max_ns = duration;
    }
    if (duration > REACTOR_DEADLINE_NS) {
        state->dispatch_misses++;
    }
}

static bool managed_cell_terminal(const gsh_async_cell *cell)
{
    return cell->state == GSH_ASYNC_DONE ||
           cell->state == GSH_ASYNC_FAILED ||
           cell->state == GSH_ASYNC_CANCELLED ||
           cell->state == GSH_ASYNC_REJECTED;
}

static void finalize_managed_dispatch(shell_state *state, int cell_index)
{
    gsh_async_cell *cell = &state->async_repl->cells[cell_index];

    if (state->current_job.active &&
        state->current_job.pid == cell->pid) {
        state->async_state_cell = cell_index;
        return;
    }
    if (cell->state == GSH_ASYNC_RUNNING || managed_cell_terminal(cell)) {
        state->async_capture_cell = -1;
        state->async_dispatch_cell = -1;
        return;
    }
    if (state->mode == MODE_EDITOR && !state->current_job.active) {
        gsh_async_repl_finish(state->async_repl, cell_index,
                              state->last_status << 8, true);
        state->async_capture_cell = -1;
        state->async_dispatch_cell = -1;
        return;
    }
    state->async_state_cell = cell_index;
}

static void dispatch_managed_cell(shell_state *state, int cell_index)
{
    int prior_state_cell = state->async_state_cell;
    int previous_status;
    uint64_t start;

    gsh_async_repl_starting(state->async_repl, cell_index);
    state->async_dispatch_cell = cell_index;
    state->async_capture_cell = cell_index;
    if (state->async_repl->cells[cell_index].status_dependency &&
        gsh_async_repl_previous_status(state->async_repl, cell_index,
                                       &previous_status) == 0) {
        state->last_status = previous_status;
    }
    if (load_managed_submission(state, cell_index) == -1) {
        output_text(state, "gsh: invalid managed submission\r\n");
        state->last_status = 125;
        state->mode = MODE_EDITOR;
        finalize_managed_dispatch(state, cell_index);
        return;
    }
    state->mode = MODE_DISPATCH;
    start = monotonic_ns();
    dispatch_pending(state);
    record_dispatch_duration(state, start);
    finalize_managed_dispatch(state, cell_index);
    if (prior_state_cell >= 0 && state->async_state_cell == prior_state_cell) {
        state->async_capture_cell = prior_state_cell;
    }
}

static void schedule_managed_submissions(shell_state *state)
{
    unsigned int dispatched;

    if (state->async_repl == NULL || !state->async_repl->enabled) {
        return;
    }
    finish_managed_state_cell(state);
    for (dispatched = 0; dispatched < 4U; dispatched++) {
        int cell_index;

        if (state->mode != MODE_EDITOR) {
            break;
        }
        cell_index = gsh_async_repl_next(
            state->async_repl, managed_state_lane_busy(state));
        if (cell_index < 0) {
            break;
        }
        dispatch_managed_cell(state, cell_index);
    }
}

static size_t add_managed_poll_descriptors(
    shell_state *state, struct pollfd descriptors[4 + GSH_ASYNC_CELL_CAP])
{
    size_t count = 4;
    int index;

    if (state->async_repl == NULL || !state->async_repl->enabled) {
        return count;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        int descriptor = state->async_repl->cells[index].pty_fd;

        if (descriptor < 0) {
            continue;
        }
        descriptors[count].fd = descriptor;
        descriptors[count].events = POLLIN;
        if (gsh_async_repl_input_pending(state->async_repl, index)) {
            descriptors[count].events |= POLLOUT;
        }
        descriptors[count].revents = 0;
        count++;
    }
    return count;
}

static void read_managed_output(shell_state *state, struct pollfd *descriptor)
{
    char bytes[4096];
    int cell_index = gsh_async_repl_cell_for_fd(
        state->async_repl, descriptor->fd);
    unsigned int reads;

    if (cell_index < 0) {
        return;
    }
    for (reads = 0; reads < 4U; reads++) {
        ssize_t count = read(descriptor->fd, bytes, sizeof(bytes));

        if (count > 0) {
            (void)gsh_async_repl_append(state->async_repl, cell_index,
                                        bytes, (size_t)count);
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        gsh_async_repl_close_output(state->async_repl, cell_index);
        return;
    }
}

static void process_managed_descriptors(
    shell_state *state,
    struct pollfd descriptors[4 + GSH_ASYNC_CELL_CAP], size_t count)
{
    size_t index;

    if (state->async_repl == NULL || !state->async_repl->enabled) {
        return;
    }
    for (index = 4; index < count; index++) {
        short events = descriptors[index].revents;

        if ((events & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
            read_managed_output(state, &descriptors[index]);
        }
        if ((events & POLLOUT) != 0) {
            int cell_index = gsh_async_repl_cell_for_fd(
                state->async_repl, descriptors[index].fd);

            if (cell_index >= 0 &&
                gsh_async_repl_flush_input(state->async_repl,
                                           cell_index) == -1) {
                state->async_repl->cells[cell_index].focused = false;
                gsh_async_repl_close_output(state->async_repl, cell_index);
            }
        }
    }
}

static bool editor_accepts_input(const shell_state *state)
{
    return state->async_repl != NULL && state->async_repl->enabled
               ? true
               : state->mode == MODE_EDITOR;
}

static int run_reactor(shell_state *state)
{
    queue_prompt(state);

    while (state->running) {
        struct pollfd descriptors[4 + GSH_ASYNC_CELL_CAP];
        size_t descriptor_count;
        int result;
        uint64_t service_start;
        uint64_t service_end;
        uint64_t service_duration;

        schedule_managed_submissions(state);
        prepare_managed_render(state);

        if ((state->async_repl == NULL || !state->async_repl->enabled) &&
            state->mode == MODE_DISPATCH && state->output_len == 0) {
            uint64_t dispatch_start = monotonic_ns();
            uint64_t dispatch_end;
            uint64_t dispatch_duration;

            dispatch_pending(state);
            dispatch_end = monotonic_ns();
            dispatch_duration = dispatch_end >= dispatch_start
                                    ? dispatch_end - dispatch_start
                                    : 0;
            state->dispatch_cycles++;
            if (dispatch_duration > state->dispatch_max_ns) {
                state->dispatch_max_ns = dispatch_duration;
            }
            if (dispatch_duration > REACTOR_DEADLINE_NS) {
                state->dispatch_misses++;
            }
            continue;
        }

        descriptors[0].fd = state->signal_pipe[0];
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = state->tty_fd;
        descriptors[1].events = 0;
        descriptors[1].revents = 0;
        if (editor_accepts_input(state)) {
            descriptors[1].events |= POLLIN;
        }
        if (state->output_len > 0) {
            descriptors[1].events |= POLLOUT;
        }

        descriptors[2].fd = state->prompt_worker_alive
                                ? state->prompt_worker_fd
                                : -1;
        descriptors[2].events = state->prompt_worker_alive ? POLLIN : 0;
        descriptors[2].revents = 0;
        descriptors[3].fd = state->variable_commit_active
                                ? state->variable_commit_fd
                                : -1;
        descriptors[3].events = state->variable_commit_active ? POLLIN : 0;
        descriptors[3].revents = 0;
        descriptor_count = add_managed_poll_descriptors(state, descriptors);

        result = fault_should_fail("poll", EIO)
                     ? -1
                     : poll(descriptors, descriptor_count,
                            prompt_poll_timeout(state));
        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("gsh: poll");
            state->last_status = 1;
            break;
        }

        service_start = monotonic_ns();
        if (state->variable_commit_active &&
            (descriptors[3].revents &
             (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
            receive_variable_commit(state, false);
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            drain_signal_pipe(state);
            process_pending_signals(state);
        }
        if (editor_accepts_input(state) &&
            (descriptors[1].revents & POLLIN) != 0) {
            process_input(state);
        }
        if (editor_accepts_input(state) &&
            (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            state->running = false;
        }
        process_managed_descriptors(state, descriptors, descriptor_count);
        if (state->prompt_worker_alive &&
            (descriptors[2].revents & POLLIN) != 0) {
            receive_prompt_result(state);
        }
        if (state->prompt_worker_alive &&
            (descriptors[2].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            bool command = state->mode == MODE_ASYNC_WORKER;

            state->prompt_worker_failures++;
            disable_prompt_worker(state, true);
            if (command) {
                output_text(state,
                            "gsh: asynchronous redirection failed\r\n");
                state->last_status = 1;
                state->mode = MODE_EDITOR;
                queue_prompt(state);
            }
        }
        enforce_prompt_deadline(state);
        schedule_managed_submissions(state);
        prepare_managed_render(state);
        if (state->output_len > 0) {
            flush_output(state);
        }
        send_prompt_request(state);

        service_end = monotonic_ns();
        service_duration = service_end >= service_start
                               ? service_end - service_start
                               : 0;
        state->reactor_cycles++;
        if (service_duration > state->reactor_max_ns) {
            state->reactor_max_ns = service_duration;
        }
        if (service_duration > REACTOR_DEADLINE_NS) {
            state->reactor_misses++;
        }
    }
    return state->last_status;
}

static void leave_managed_screen(shell_state *state)
{
    static const char sequence[] = "\033[?1049l";
    unsigned int attempts;

    if (state->async_repl == NULL ||
        !state->async_repl->alternate_screen_entered || state->tty_fd < 0) {
        return;
    }
    for (attempts = 0; attempts < 2U; attempts++) {
        ssize_t written = write(state->tty_fd, sequence,
                                sizeof(sequence) - 1U);

        if (written == (ssize_t)(sizeof(sequence) - 1U) ||
            (written == -1 && errno != EINTR)) {
            break;
        }
    }
}

static size_t reap_managed_children(pid_t pids[GSH_ASYNC_CELL_CAP])
{
    size_t remaining = 0;
    int index;

    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        pid_t result;

        if (pids[index] <= 0) {
            continue;
        }
        result = waitpid(pids[index], NULL, WNOHANG);
        if (result == pids[index] ||
            (result == -1 && errno == ECHILD)) {
            pids[index] = 0;
        } else {
            remaining++;
        }
    }
    return remaining;
}

static void terminate_managed_children(
    pid_t pids[GSH_ASYNC_CELL_CAP],
    const pid_t original_groups[GSH_ASYNC_CELL_CAP],
    const pid_t terminal_groups[GSH_ASYNC_CELL_CAP])
{
    uint64_t deadline = monotonic_ns() + 100000000ULL;
    int index;

    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        if (pids[index] > 0) {
            (void)kill(pids[index], SIGHUP);
            (void)kill(pids[index], SIGCONT);
        }
    }
    while (reap_managed_children(pids) != 0 &&
           monotonic_ns() < deadline) {
        (void)poll(NULL, 0, 1);
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        if (pids[index] <= 0) {
            continue;
        }
        if (original_groups[index] > 0) {
            (void)kill(-original_groups[index], SIGKILL);
        }
        if (terminal_groups[index] > 0 &&
            terminal_groups[index] != original_groups[index]) {
            (void)kill(-terminal_groups[index], SIGKILL);
        }
        (void)kill(pids[index], SIGKILL);
    }
    deadline = monotonic_ns() + 500000000ULL;
    while (reap_managed_children(pids) != 0 &&
           monotonic_ns() < deadline) {
        (void)poll(NULL, 0, 1);
    }
}

static void cleanup(shell_state *state)
{
    pid_t worker_pid = state->prompt_worker_pid;
    pid_t managed_pids[GSH_ASYNC_CELL_CAP] = {0};
    pid_t managed_groups[GSH_ASYNC_CELL_CAP] = {0};
    pid_t managed_terminal_groups[GSH_ASYNC_CELL_CAP] = {0};
    pid_t background_pids[GSH_BACKGROUND_CAP];
    size_t background_count = gsh_background_snapshot(
        &state->background_jobs, background_pids);
    size_t background;
    int managed;

    if (state->async_repl != NULL) {
        for (managed = 0; managed < GSH_ASYNC_CELL_CAP; managed++) {
            managed_pids[managed] =
                state->async_repl->cells[managed].pid;
            managed_groups[managed] =
                state->async_repl->cells[managed].pgid;
        }
        gsh_async_repl_close(state->async_repl);
        for (managed = 0; managed < GSH_ASYNC_CELL_CAP; managed++) {
            managed_terminal_groups[managed] =
                state->async_repl->cells[managed].pgid;
        }
    }
    if (state->current_job.active) {
        (void)kill(-state->current_job.pgid, SIGHUP);
        if (state->current_job.stopped) {
            (void)kill(-state->current_job.pgid, SIGCONT);
        }
    }
    for (background = 0; background < background_count; background++) {
        (void)kill(-background_pids[background], SIGHUP);
        (void)kill(background_pids[background], SIGHUP);
        (void)kill(-background_pids[background], SIGCONT);
        (void)kill(background_pids[background], SIGCONT);
    }
    state->prompt_worker_restart_pending = false;
    disable_prompt_worker(state, true);
    close_variable_commit(state);
    terminate_managed_children(managed_pids, managed_groups,
                               managed_terminal_groups);
    if (worker_pid > 0) {
        while (waitpid(worker_pid, NULL, 0) == -1 && errno == EINTR) {
        }
        state->prompt_worker_pid = -1;
    }
    leave_managed_screen(state);
    restore_terminal(state);
    g_signal_write_fd = -1;
    if (state->signal_pipe[0] >= 0) {
        close(state->signal_pipe[0]);
    }
    if (state->signal_pipe[1] >= 0) {
        close(state->signal_pipe[1]);
    }
    if (state->tty_fd >= 0) {
        close(state->tty_fd);
    }
    free(state->parse_storage);
    state->parse_storage = NULL;
    free(state->native_pipeline);
    state->native_pipeline = NULL;
    free(state->variables);
    state->variables = NULL;
    free(state->variable_scratch);
    state->variable_scratch = NULL;
    free(state->pipeline_variables);
    state->pipeline_variables = NULL;
    free(state->variable_commit);
    state->variable_commit = NULL;
    free(state->pipeline_changes);
    state->pipeline_changes = NULL;
    free(state->async_repl);
    state->async_repl = NULL;
    free(state->alias_expansion);
    state->alias_expansion = NULL;
    free(state->aliases);
    state->aliases = NULL;
    free(state->alias_scratch);
    state->alias_scratch = NULL;
    free(state->alias_commit);
    state->alias_commit = NULL;
    free(state->functions);
    state->functions = NULL;
    free(state->function_scratch);
    state->function_scratch = NULL;
    if (state->positionals != &g_interactive_positionals) {
        free(state->positionals);
    }
    state->positionals = NULL;
}

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "usage: gsh\n"
            "       gsh -c command\n"
            "       gsh -n -c command\n"
            "       gsh --native-only -c command\n\n"
            "Run the minimal interactive gsh reactor, or execute one command "
            "non-interactively.\n");
}

static int check_native_syntax(const char *input)
{
    gsh_parse_storage *storage = fault_should_fail("allocation", ENOMEM)
                                     ? NULL
                                     : malloc(sizeof(*storage));
    gsh_parse_result result;

    if (storage == NULL) {
        perror("gsh: syntax allocation");
        return 2;
    }
    result = gsh_parse(input, strlen(input), storage);
    free(storage);
    if (result.status == GSH_PARSE_OK) {
        return 0;
    }
    fprintf(stderr, "gsh: %s at byte %zu\n",
            gsh_parse_status_name(result.status), result.error_offset);
    return 2;
}

static bool alias_command_probe(void *opaque, const char *word,
                                size_t length)
{
    bool *found = opaque;

    if ((length == 5U && memcmp(word, "alias", 5) == 0) ||
        (length == 7U && memcmp(word, "unalias", 7) == 0)) {
        *found = true;
    }
    return false;
}

static bool input_contains_alias_builtin(
    const char *input, size_t length, gsh_parse_storage *storage)
{
    gsh_word_ref candidate;
    bool found = false;
    gsh_parse_result result = gsh_parse_command_probe(
        input, length, storage, alias_command_probe, &found, &candidate);

    (void)candidate;
    return result.status == GSH_PARSE_OK && found;
}

static bool storage_has_function(const gsh_parse_storage *storage)
{
    size_t index;

    for (index = 0; index < storage->node_count; index++) {
        if (storage->nodes[index].kind == GSH_AST_FUNCTION) {
            return true;
        }
    }
    return false;
}

static int native_wait_status_value(int status, bool negated)
{
    int value = wait_status_value(status);

    return negated ? (value == 0 ? 1 : 0) : value;
}

typedef struct native_evaluator native_evaluator;

static int run_pipeline_function(native_evaluator *parent,
                                 gsh_native_pipeline *pipeline,
                                 size_t command_index,
                                 gsh_variable_store *variables,
                                 bool *found);

static int run_native_noninteractive_pipeline(
    gsh_native_pipeline *pipeline, const char *default_path,
    gsh_variable_store *variables, gsh_variable_journal *journal,
    gsh_alias_store *aliases, gsh_alias_journal *alias_journal,
    const pipeline_expansion_scope *scope,
    gsh_positional_store *positionals, gsh_shell_options *options,
    native_evaluator *evaluator)
{
    int pipes[GSH_NATIVE_PIPELINE_CAP - 1][2];
    int heredoc_pipes[GSH_NATIVE_HEREDOC_CAP][2];
    pid_t members[GSH_NATIVE_JOB_MEMBER_CAP];
    size_t pipe_count = pipeline->command_count - 1U;
    size_t created_pipes = 0;
    size_t created_heredocs = 0;
    size_t launched = 0;
    size_t index;
    pid_t status_pid = -1;
    int last_wait_status = 0;
    bool last_status_known = false;

    if (pipeline->command_count == 1 &&
        pipeline->commands[0].argc == 0 &&
        pipeline->commands[0].assignment_count > 0 &&
        pipeline->commands[0].redirect_count == 0) {
        int assignment_status = apply_native_assignments(
            variables, journal, &pipeline->commands[0], options);

        if (assignment_status != GSH_ASSIGNMENT_OK) {
            perror("gsh: assignment");
            return assignment_status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125
                                                                     : 1;
        }
        return pipeline->negated ? 1 : 0;
    }
    if (pipeline->command_count == 1 &&
        native_variable_builtin(&pipeline->commands[0]) &&
        pipeline->commands[0].redirect_count != 0) {
        return run_evaluator_variable_builtin(pipeline, variables, journal,
                                              options, NULL);
    }
    if (pipeline->command_count == 1 &&
        native_colon_builtin(&pipeline->commands[0]) &&
        pipeline->commands[0].assignment_count != 0 &&
        pipeline->commands[0].redirect_count != 0) {
        return run_evaluator_colon_builtin(pipeline, variables, journal,
                                           options);
    }
    if (pipeline->command_count == 1 &&
        pipeline->commands[0].redirect_count == 0) {
        int builtin_status;

        if (native_stateless_builtin(&pipeline->commands[0],
                                     &builtin_status)) {
            if (native_colon_builtin(&pipeline->commands[0]) &&
                pipeline->commands[0].assignment_count != 0) {
                int assignment_status = apply_native_assignments(
                    variables, journal, &pipeline->commands[0], options);

                if (assignment_status != GSH_ASSIGNMENT_OK) {
                    perror("gsh: assignment");
                    return assignment_status == GSH_ASSIGNMENT_JOURNAL_ERROR
                               ? 125
                               : 1;
                }
            }
            return pipeline->negated ? (builtin_status == 0 ? 1 : 0)
                                     : builtin_status;
        }
        if (native_environment_builtin(&pipeline->commands[0])) {
            builtin_status = run_native_environment_builtin(
                &pipeline->commands[0], &descriptor_builtin_io);
            return pipeline->negated ? (builtin_status == 0 ? 1 : 0)
                                     : builtin_status;
        }
        if (native_variable_builtin(&pipeline->commands[0])) {
            int assignment_status = apply_native_assignments(
                variables, journal, &pipeline->commands[0], options);

            if (assignment_status != GSH_ASSIGNMENT_OK) {
                perror("gsh: assignment");
                return assignment_status == GSH_ASSIGNMENT_JOURNAL_ERROR
                           ? 125
                           : 1;
            }
            builtin_status = run_native_variable_builtin(
                &pipeline->commands[0], variables, journal,
                options, NULL, &descriptor_builtin_io);
            if (builtin_status == 125) {
                return 125;
            }
            return pipeline->negated ? (builtin_status == 0 ? 1 : 0)
                                     : builtin_status;
        }
        if (native_alias_builtin(&pipeline->commands[0])) {
            builtin_status = run_native_alias_builtin(
                &pipeline->commands[0], aliases, alias_journal,
                &descriptor_builtin_io);
            return builtin_status == 125
                       ? 125
                       : (pipeline->negated
                              ? (builtin_status == 0 ? 1 : 0)
                              : builtin_status);
        }
    }

    initialize_pipeline_descriptors(pipes);
    initialize_heredoc_descriptors(heredoc_pipes);
    for (created_heredocs = 0;
         created_heredocs < pipeline->heredoc_count;
         created_heredocs++) {
        if (make_pipe(heredoc_pipes[created_heredocs], false,
                      "heredoc-pipe") == -1) {
            perror("gsh: here-document pipe");
            close_heredoc_descriptors(heredoc_pipes,
                                      created_heredocs);
            return 125;
        }
    }
    for (created_pipes = 0; created_pipes < pipe_count; created_pipes++) {
        if (make_pipe(pipes[created_pipes], false, "pipeline-pipe") == -1) {
            perror("gsh: pipeline pipe");
            close_pipeline_descriptors(pipes, created_pipes);
            close_heredoc_descriptors(heredoc_pipes,
                                      created_heredocs);
            return 125;
        }
    }
    for (index = 0; index < pipeline->command_count; index++) {
        pid_t pid = fault_should_fail("pipeline-fork", EAGAIN) ? -1 : fork();

        if (pid == 0) {
            size_t close_index;

            reset_child_signals();
            if (pipeline->commands[index].expansion_error) {
                _exit(1);
            }
            if (scope != NULL &&
                gsh_variables_apply_journal_scope_in_place(
                    variables, scope->changes, index + 1U) == -1) {
                child_exec_error("pipeline variable scope", errno);
            }
            if (index > 0 &&
                child_duplicate_descriptor(pipes[index - 1U][0],
                                           STDIN_FILENO) == -1) {
                child_exec_error("pipeline input", errno);
            }
            if (index + 1U < pipeline->command_count &&
                child_duplicate_descriptor(pipes[index][1],
                                           STDOUT_FILENO) == -1) {
                child_exec_error("pipeline output", errno);
            }
            child_apply_redirects(&pipeline->commands[index],
                                  heredoc_pipes,
                                  pipeline->heredoc_count, options);
            for (close_index = 0; close_index < created_pipes;
                 close_index++) {
                close(pipes[close_index][0]);
                close(pipes[close_index][1]);
            }
            close_heredoc_descriptors(heredoc_pipes,
                                      pipeline->heredoc_count);
            {
                bool function_found;
                int function_status = run_pipeline_function(
                    evaluator, pipeline, index, variables,
                    &function_found);

                if (function_found) {
                    _exit(function_status & 255);
                }
            }
            if (fault_should_fail("exec", EIO)) {
                child_exec_error(pipeline->commands[index].argv[0], errno);
            }
            {
                int builtin_status;

                if (native_stateless_builtin(&pipeline->commands[index],
                                             &builtin_status)) {
                    _exit(builtin_status);
                }
                if (native_pwd_builtin(&pipeline->commands[index])) {
                    _exit(child_run_pwd(&pipeline->commands[index],
                                        variables));
                }
                if (native_cd_builtin(&pipeline->commands[index])) {
                    if (apply_native_assignments(
                            variables, NULL, &pipeline->commands[index],
                            options) != GSH_ASSIGNMENT_OK) {
                        child_exec_error("assignment", errno);
                    }
                    _exit(run_native_cd_builtin(
                        &pipeline->commands[index], variables, variables,
                        NULL,
                        options, &descriptor_builtin_io, NULL, 0));
                }
                if (native_environment_builtin(
                        &pipeline->commands[index])) {
                    _exit(run_native_environment_builtin(
                        &pipeline->commands[index], &descriptor_builtin_io));
                }
                if (native_variable_builtin(&pipeline->commands[index])) {
                    if (apply_native_assignments(
                            variables, NULL,
                            &pipeline->commands[index], options) !=
                        GSH_ASSIGNMENT_OK) {
                        child_exec_error("assignment", errno);
                    }
                    _exit(run_native_variable_builtin(
                        &pipeline->commands[index], variables, NULL,
                        options, NULL, &descriptor_builtin_io));
                }
                if (native_state_builtin(
                        &pipeline->commands[index])) {
                    if (apply_native_assignments(
                            variables, NULL,
                            &pipeline->commands[index], options) !=
                        GSH_ASSIGNMENT_OK) {
                        child_exec_error("assignment", errno);
                    }
                    _exit(run_native_state_builtin(
                        &pipeline->commands[index], variables,
                        positionals, options, &descriptor_builtin_io));
                }
                if (native_wait_builtin(&pipeline->commands[index])) {
                    _exit(pipeline->commands[index].argc == 1 ? 0 : 127);
                }
                if (native_alias_builtin(&pipeline->commands[index])) {
                    _exit(run_native_alias_builtin(
                        &pipeline->commands[index], aliases, NULL,
                        &descriptor_builtin_io));
                }
            }
            {
                char *environment_storage[CHILD_ENVIRONMENT_CAP];
                char *const *environment = child_command_environment(
                    variables, &pipeline->commands[index],
                    environment_storage);

                child_exec_direct(
                    pipeline->commands[index].argv,
                    command_path_value(variables,
                                       &pipeline->commands[index],
                                       default_path),
                    environment);
            }
        }
        if (pid == -1) {
            int saved_errno = errno;
            size_t terminate;

            close_pipeline_descriptors(pipes, created_pipes);
            close_heredoc_descriptors(heredoc_pipes,
                                      created_heredocs);
            for (terminate = 0; terminate < launched; terminate++) {
                (void)kill(members[terminate], SIGKILL);
            }
            for (terminate = 0; terminate < launched; terminate++) {
                while (waitpid(members[terminate], NULL, 0) == -1 &&
                       errno == EINTR) {
                }
            }
            errno = saved_errno;
            perror("gsh: pipeline fork");
            return 125;
        }
        members[launched++] = pid;
    }
    status_pid = members[pipeline->command_count - 1U];
    for (index = 0; index < pipeline->heredoc_count; index++) {
        pid_t pid = fault_should_fail("heredoc-fork", EAGAIN) ? -1 : fork();

        if (pid == 0) {
            reset_child_signals();
            close_pipeline_descriptors(pipes, created_pipes);
            child_write_heredoc(pipeline, index, heredoc_pipes);
        }
        if (pid == -1) {
            int saved_errno = errno;
            size_t terminate;

            close_pipeline_descriptors(pipes, created_pipes);
            close_heredoc_descriptors(heredoc_pipes,
                                      created_heredocs);
            for (terminate = 0; terminate < launched; terminate++) {
                (void)kill(members[terminate], SIGKILL);
            }
            for (terminate = 0; terminate < launched; terminate++) {
                while (waitpid(members[terminate], NULL, 0) == -1 &&
                       errno == EINTR) {
                }
            }
            errno = saved_errno;
            perror("gsh: here-document fork");
            return 125;
        }
        members[launched++] = pid;
    }
    close_pipeline_descriptors(pipes, created_pipes);
    close_heredoc_descriptors(heredoc_pipes, created_heredocs);

    for (index = 0; index < launched; index++) {
        int status;
        pid_t waited;

        do {
            waited = waitpid(members[index], &status, 0);
        } while (waited == -1 && errno == EINTR);
        if (waited == -1) {
            perror("gsh: waitpid");
            return 125;
        }
        if (members[index] == status_pid) {
            last_wait_status = status;
            last_status_known = true;
        }
    }
    return last_status_known
               ? native_wait_status_value(last_wait_status,
                                          pipeline->negated)
               : 125;
}

struct native_evaluator {
    const char *input;
    size_t input_length;
    const gsh_parse_storage *storage;
    gsh_native_pipeline *pipeline;
    const char *default_path;
    int last_status;
    long shell_pid;
    long last_background_pid;
    const char *parameter_zero;
    gsh_positional_store *positionals;
    char *positional_view[GSH_POSITIONAL_CAP + 1];
    gsh_shell_options options;
    char option_flags[GSH_OPTION_FLAG_CAP];
    gsh_variable_store *variables;
    gsh_variable_journal *journal;
    gsh_alias_store *aliases;
    gsh_alias_journal *alias_journal;
    gsh_function_store *functions;
    gsh_function_store *function_scratch;
    gsh_variable_store *scope_base;
    gsh_variable_journal *scope_changes;
    pipeline_expansion_scope *pipeline_scope;
    size_t substitution_depth;
    size_t function_depth;
    bool function_active[GSH_FUNCTION_CAP];
    bool returning;
    int return_status;
    bool preflight;
    bool fatal_error;
    bool static_for_items;
    bool tail_exec_single;
    bool positional_mutation_possible;
    bool directory_mutation_possible;
    bool alias_mutation_possible;
    bool function_mutation_possible;
    bool state_commit_invalid;
    gsh_background_table *backgrounds;
};

static bool native_preflight_node(native_evaluator *evaluator,
                                  size_t node_index, size_t depth);
static int native_evaluate_node(native_evaluator *evaluator,
                                size_t node_index, size_t depth);
static int native_evaluate_node_inner(native_evaluator *evaluator,
                                      size_t node_index, size_t depth);

static bool async_node_has_single_pipeline(const native_evaluator *evaluator,
                                           size_t node_index)
{
    const gsh_ast_node *node;
    size_t child;

    if (node_index >= evaluator->storage->node_count) {
        return false;
    }
    node = &evaluator->storage->nodes[node_index];
    if (node->kind != GSH_AST_AND_OR || node->first_child == GSH_AST_NONE) {
        return false;
    }
    child = node->first_child;
    return evaluator->storage->nodes[child].kind == GSH_AST_PIPELINE &&
           evaluator->storage->nodes[child].next_sibling == GSH_AST_NONE;
}

static gsh_native_plan_status preflight_command_substitution(
    void *opaque, const char *commands, size_t command_length, char *output,
    size_t output_capacity, size_t *output_length, int *exit_status)
{
    (void)opaque;
    (void)commands;
    (void)command_length;
    if (output_capacity == 0) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    output[0] = '0';
    *output_length = 1;
    *exit_status = 0;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_expansion_context native_expansion_context(
    native_evaluator *evaluator, bool execute_substitutions,
    bool *deferred_work);
static gsh_native_plan_status execute_command_substitution(
    void *opaque, const char *commands, size_t command_length, char *output,
    size_t output_capacity, size_t *output_length, int *exit_status);

static const char *evaluator_variable_lookup(void *opaque,
                                             const char *name,
                                             size_t name_length,
                                             bool *found)
{
    native_evaluator *evaluator = opaque;

    return gsh_variables_lookup(evaluator->variables, name, name_length,
                                found);
}

static gsh_native_plan_status evaluator_variable_assign(
    void *opaque, const char *name, size_t name_length, const char *value,
    size_t value_length)
{
    native_evaluator *evaluator = opaque;
    unsigned int attributes = assignment_attributes(&evaluator->options);
    int journal_status = 0;

    if ((!evaluator->preflight &&
         fault_should_fail("expansion-assignment", ENOSPC)) ||
        gsh_variables_set(evaluator->variables, name, name_length, value,
                          value_length, attributes, attributes) == -1) {
        journal_status = -1;
    } else if (evaluator->pipeline_scope != NULL) {
        journal_status = gsh_variable_journal_record_scoped(
            evaluator->pipeline_scope->changes,
            evaluator->pipeline_scope->current_scope, name, name_length,
            value, value_length, attributes, attributes);
    } else if (evaluator->journal != NULL) {
        journal_status = gsh_variable_journal_record(
            evaluator->journal, name, name_length, value, value_length,
            attributes, attributes);
    }

    if (journal_status == -1) {
        if (!evaluator->preflight) {
            child_write_descriptor(STDERR_FILENO,
                                   "gsh: parameter assignment failed\n",
                                   33);
        }
        evaluator->fatal_error = evaluator->pipeline_scope == NULL;
        return errno == ENOSPC || errno == E2BIG
                   ? GSH_NATIVE_PLAN_LIMIT
                   : GSH_NATIVE_PLAN_ERROR;
    }
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status evaluator_parameter_error(
    void *opaque, const char *name, size_t name_length, const char *message,
    size_t message_length, bool default_message)
{
    native_evaluator *evaluator = opaque;

    if (evaluator->preflight) {
        return GSH_NATIVE_PLAN_OK;
    }
    child_write_descriptor(STDERR_FILENO, "gsh: ", 5);
    child_write_descriptor(STDERR_FILENO, name, name_length);
    child_write_descriptor(STDERR_FILENO, ": ", 2);
    if (default_message) {
        child_write_descriptor(STDERR_FILENO,
                               "parameter null or not set", 25);
    } else {
        child_write_descriptor(STDERR_FILENO, message, message_length);
    }
    child_write_descriptor(STDERR_FILENO, "\n", 1);
    evaluator->fatal_error = evaluator->pipeline_scope == NULL;
    return GSH_NATIVE_PLAN_ERROR;
}

static gsh_native_plan_status evaluator_expansion_error(
    void *opaque, const char *message, size_t message_length)
{
    native_evaluator *evaluator = opaque;

    if (evaluator->preflight) {
        return GSH_NATIVE_PLAN_OK;
    }
    child_write_descriptor(STDERR_FILENO,
                           "gsh: arithmetic expansion: ", 27);
    child_write_descriptor(STDERR_FILENO, message, message_length);
    child_write_descriptor(STDERR_FILENO, "\n", 1);
    evaluator->fatal_error = evaluator->pipeline_scope == NULL;
    return GSH_NATIVE_PLAN_ERROR;
}

static gsh_native_plan_status evaluator_pipeline_command_begin(
    void *opaque, size_t command_index, size_t command_count)
{
    native_evaluator *evaluator = opaque;
    pipeline_expansion_scope *scope = evaluator->pipeline_scope;

    if (scope == NULL || scope->command_count != command_count ||
        command_index >= command_count || command_index >= UINT16_MAX) {
        return GSH_NATIVE_PLAN_ERROR;
    }
    memcpy(evaluator->variables, scope->base,
           sizeof(*evaluator->variables));
    scope->current_scope = command_index + 1U;
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_expansion_context native_expansion_context(
    native_evaluator *evaluator, bool execute_substitutions,
    bool *deferred_work)
{
    size_t positional_count =
        gsh_positionals_count(evaluator->positionals);
    gsh_native_expansion_context context = {
        .last_status = evaluator->last_status,
        .shell_pid = evaluator->shell_pid,
        .last_background_pid = evaluator->last_background_pid,
        .parameter_zero = evaluator->parameter_zero,
        .positional_parameters = positional_count == 0
                                     ? NULL
                                     : evaluator->positional_view,
        .positional_count = positional_count,
        .option_flags = evaluator->option_flags,
        .variable_lookup = evaluator_variable_lookup,
        .variable_assign = evaluator_variable_assign,
        .parameter_error = evaluator_parameter_error,
        .expansion_error = evaluator_expansion_error,
        .variable_opaque = evaluator,
        .command_begin = evaluator->pipeline_scope != NULL
                             ? evaluator_pipeline_command_begin
                             : NULL,
        .command_opaque = evaluator,
        .command_substitute =
            execute_substitutions ? execute_command_substitution
                                  : preflight_command_substitution,
        .command_substitute_opaque = evaluator,
        .pathname_mode = execute_substitutions &&
                                 !gsh_options_enabled(
                                     &evaluator->options,
                                     GSH_OPTION_NOGLOB)
                             ? GSH_NATIVE_PATHNAME_EXECUTE
                             : GSH_NATIVE_PATHNAME_PREFLIGHT,
        .defer_complex_patterns = evaluator->preflight,
        .deferred_work = deferred_work,
        .nounset = gsh_options_enabled(&evaluator->options,
                                       GSH_OPTION_NOUNSET),
        .preflight = evaluator->preflight,
    };

    gsh_options_flags(&evaluator->options, evaluator->option_flags);
    gsh_positionals_view(evaluator->positionals,
                         evaluator->positional_view);
    return context;
}

static size_t pipeline_command_count(const gsh_parse_storage *storage,
                                     size_t node_index)
{
    const gsh_ast_node *node;
    size_t child;
    size_t count = 0;

    if (node_index >= storage->node_count) {
        return 0;
    }
    node = &storage->nodes[node_index];
    if (node->kind != GSH_AST_PIPELINE) {
        return 0;
    }
    child = node->first_child;
    while (child != GSH_AST_NONE && count <= GSH_NATIVE_PIPELINE_CAP) {
        if (child >= storage->node_count) {
            return 0;
        }
        count++;
        child = storage->nodes[child].next_sibling;
    }
    return child == GSH_AST_NONE ? count : 0;
}

static gsh_native_plan_status plan_evaluator_pipeline(
    native_evaluator *evaluator, size_t node_index,
    bool execute_substitutions, pipeline_expansion_scope *scope,
    bool *scoped, bool *deferred_work)
{
    gsh_native_expansion_context expansion;
    gsh_native_plan_status status;
    size_t count = pipeline_command_count(evaluator->storage, node_index);

    *scoped = count > 1U;
    if (count > 1U) {
        if (evaluator->scope_base == NULL ||
            evaluator->scope_changes == NULL) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        memcpy(evaluator->scope_base, evaluator->variables,
               sizeof(*evaluator->scope_base));
        gsh_variable_journal_initialize(evaluator->scope_changes, 0);
        scope->base = evaluator->scope_base;
        scope->changes = evaluator->scope_changes;
        scope->command_count = count;
        scope->current_scope = 0;
    }
    evaluator->pipeline_scope = *scoped ? scope : NULL;
    if (deferred_work != NULL) {
        *deferred_work = false;
    }
    expansion = native_expansion_context(evaluator, execute_substitutions,
                                         deferred_work);
    status = gsh_native_plan_pipeline_node_with_context(
        evaluator->input, evaluator->storage, node_index, &expansion,
        evaluator->pipeline);
    evaluator->pipeline_scope = NULL;
    if (*scoped) {
        memcpy(evaluator->variables, scope->base,
               sizeof(*evaluator->variables));
    }
    if (status != GSH_NATIVE_PLAN_OK) {
        return status;
    }
    return GSH_NATIVE_PLAN_OK;
}

static bool native_case_pattern(const char *input, gsh_word_ref pattern,
                                char output[GSH_NATIVE_TEXT_CAP])
{
    enum {
        PATTERN_QUOTE_NONE,
        PATTERN_QUOTE_SINGLE,
        PATTERN_QUOTE_DOUBLE,
    } quote = PATTERN_QUOTE_NONE;
    size_t offset;
    size_t used = 0;

    if (pattern.end - pattern.begin >= GSH_NATIVE_TEXT_CAP) {
        return false;
    }
    for (offset = pattern.begin; offset < pattern.end; offset++) {
        unsigned char byte = (unsigned char)input[offset];
        bool literal = quote != PATTERN_QUOTE_NONE;

        if (quote == PATTERN_QUOTE_SINGLE) {
            if (byte == '\'') {
                quote = PATTERN_QUOTE_NONE;
                continue;
            }
        } else if (quote == PATTERN_QUOTE_DOUBLE) {
            if (byte == '"') {
                quote = PATTERN_QUOTE_NONE;
                continue;
            }
            if (byte == '\\' && offset + 1U < pattern.end) {
                unsigned char next = (unsigned char)input[offset + 1U];

                if (next == '$' || next == 0x60U || next == '"' ||
                    next == '\\' || next == '\n') {
                    offset++;
                    if (next == '\n') {
                        continue;
                    }
                    byte = next;
                }
            }
        } else if (byte == '\'') {
            quote = PATTERN_QUOTE_SINGLE;
            continue;
        } else if (byte == '"') {
            quote = PATTERN_QUOTE_DOUBLE;
            continue;
        } else if (byte == '\\') {
            if (offset + 1U == pattern.end) {
                return false;
            }
            byte = (unsigned char)input[++offset];
            if (byte == '\n') {
                continue;
            }
            literal = true;
        } else if (byte == '$' || byte == 0x60U || byte == '~') {
            return false;
        }
        if (literal &&
            (byte == '\\' || byte == '*' || byte == '?' || byte == '[')) {
            if (used + 1U >= GSH_NATIVE_TEXT_CAP) {
                return false;
            }
            output[used++] = '\\';
        }
        if (used + 1U >= GSH_NATIVE_TEXT_CAP) {
            return false;
        }
        output[used++] = (char)byte;
    }
    if (quote != PATTERN_QUOTE_NONE) {
        return false;
    }
    output[used] = '\0';
    return true;
}

static bool native_preflight_case(native_evaluator *evaluator,
                                  const gsh_ast_node *node, size_t depth)
{
    bool deferred_work = false;
    gsh_native_expansion_context expansion =
        native_expansion_context(evaluator, false, &deferred_work);
    char *subject;
    size_t item_index;

    if (node->word_count != 1 ||
        gsh_native_expand_scalar(
            evaluator->input,
            evaluator->storage->words[node->first_word], &expansion,
            evaluator->pipeline, &subject) != GSH_NATIVE_PLAN_OK) {
        return false;
    }
    (void)subject;
    item_index = node->first_child;
    while (item_index != GSH_AST_NONE) {
        const gsh_ast_node *item =
            &evaluator->storage->nodes[item_index];
        size_t pattern;

        if (item->kind != GSH_AST_CASE_ITEM || item->word_count == 0) {
            return false;
        }
        for (pattern = 0; pattern < item->word_count; pattern++) {
            char pattern_text[GSH_NATIVE_TEXT_CAP];

            if (!native_case_pattern(
                    evaluator->input,
                    evaluator->storage->words[item->first_word + pattern],
                    pattern_text)) {
                return false;
            }
        }
        if (item->first_child != GSH_AST_NONE &&
            !native_preflight_node(evaluator, item->first_child,
                                   depth + 1U)) {
            return false;
        }
        item_index = item->next_sibling;
    }
    return true;
}

static bool native_preflight_for(native_evaluator *evaluator,
                                 const gsh_ast_node *node, size_t depth)
{
    bool deferred_work = false;
    gsh_native_expansion_context expansion =
        native_expansion_context(evaluator, false, &deferred_work);
    char *items[GSH_NATIVE_ARGUMENT_CAP];
    size_t item_count = 0;

    if (node->word_count == 0 || node->first_child == GSH_AST_NONE ||
        ((node->flags & GSH_AST_FLAG_FOR_HAS_IN) == 0 &&
         gsh_positionals_count(evaluator->positionals) >
             GSH_NATIVE_ARGUMENT_CAP)) {
        return false;
    }
    if ((node->flags & GSH_AST_FLAG_FOR_HAS_IN) != 0 &&
        gsh_native_expand_words(
            evaluator->input,
            evaluator->storage->words + node->first_word + 1U,
            node->word_count - 1U, &expansion, evaluator->pipeline, items,
            &item_count) != GSH_NATIVE_PLAN_OK) {
        return false;
    }
    (void)item_count;
    return native_preflight_node(evaluator, node->first_child,
                                 depth + 1U);
}

static const gsh_function_entry *evaluator_function(
    const native_evaluator *evaluator,
    const gsh_native_command *command)
{
    return evaluator->functions == NULL || command->argc == 0
               ? NULL
               : gsh_functions_lookup(evaluator->functions,
                                      command->argv[0],
                                      strlen(command->argv[0]));
}

static bool define_evaluator_function(native_evaluator *evaluator,
                                      size_t node_index)
{
    const gsh_ast_node *node = &evaluator->storage->nodes[node_index];
    gsh_word_ref name = evaluator->storage->words[node->first_word];

    if (evaluator->functions == NULL || node->word_count != 1U ||
        (evaluator->preflight && evaluator->storage ==
                                     &evaluator->functions->programs) ||
        special_builtin_name(evaluator->input + name.begin,
                             name.end - name.begin)) {
        errno = EINVAL;
        return false;
    }
    return gsh_functions_set(
               evaluator->functions, evaluator->function_scratch,
               evaluator->input, evaluator->input_length,
               evaluator->storage, node_index,
               evaluator->storage == &evaluator->functions->programs) == 0;
}

static bool preflight_evaluator_function(
    native_evaluator *evaluator, const gsh_native_command *command,
    const gsh_function_entry *entry, size_t depth)
{
    const char *saved_input = evaluator->input;
    size_t saved_input_length = evaluator->input_length;
    const gsh_parse_storage *saved_storage = evaluator->storage;
    gsh_positional_store *saved_positionals = evaluator->positionals;
    gsh_positional_store positionals;
    const gsh_ast_node *definition;
    size_t index = (size_t)(entry - evaluator->functions->entries);
    bool supported;

    if (depth > 128U || evaluator->function_depth ==
                            GSH_FUNCTION_DEPTH_CAP ||
        index >= GSH_FUNCTION_CAP) {
        return false;
    }
    if (evaluator->function_active[index]) {
        return true;
    }
    if (apply_native_assignments(evaluator->variables, NULL, command,
                                 &evaluator->options) !=
            GSH_ASSIGNMENT_OK ||
        gsh_positionals_assign(&positionals, command->argc - 1U,
                               command->argv + 1U) == -1) {
        return false;
    }
    evaluator->input = gsh_functions_text(evaluator->functions);
    evaluator->input_length = evaluator->functions->text_used;
    evaluator->storage = &evaluator->functions->programs;
    evaluator->positionals = &positionals;
    evaluator->function_depth++;
    evaluator->function_active[index] = true;
    definition = &evaluator->storage->nodes[entry->node_offset];
    supported = definition->kind == GSH_AST_FUNCTION &&
                definition->first_child != GSH_AST_NONE;
    if (supported && definition->redirect_count != 0) {
        bool deferred_work = false;
        gsh_native_expansion_context expansion =
            native_expansion_context(evaluator, false, &deferred_work);

        supported = gsh_native_plan_redirects_with_context(
                        evaluator->input, evaluator->storage,
                        entry->node_offset, &expansion,
                        evaluator->pipeline) == GSH_NATIVE_PLAN_OK &&
                    !deferred_work;
    }
    supported = supported && native_preflight_node(
                                 evaluator, definition->first_child, 0);
    evaluator->function_active[index] = false;
    evaluator->function_depth--;
    evaluator->positionals = saved_positionals;
    evaluator->storage = saved_storage;
    evaluator->input_length = saved_input_length;
    evaluator->input = saved_input;
    return supported;
}

static bool native_preflight_node(native_evaluator *evaluator,
                                  size_t node_index, size_t depth)
{
    const gsh_ast_node *node;
    size_t child;
    size_t visited = 0;

    if (depth > 128 || node_index == GSH_AST_NONE ||
        node_index >= evaluator->storage->node_count) {
        return false;
    }
    node = &evaluator->storage->nodes[node_index];
    if (node->redirect_count != 0 && node->kind != GSH_AST_FUNCTION) {
        return false;
    }
    if (node->kind == GSH_AST_PIPELINE) {
        const gsh_ast_node *command;
        pipeline_expansion_scope scope;
        gsh_native_plan_status plan_status;
        bool deferred_work;
        bool scoped;
        bool supported;

        if (node->first_child == GSH_AST_NONE) {
            return false;
        }
        command = &evaluator->storage->nodes[node->first_child];
        if (command->next_sibling == GSH_AST_NONE &&
            command->kind != GSH_AST_SIMPLE) {
            return native_preflight_node(evaluator, node->first_child,
                                         depth + 1U);
        }
        plan_status = plan_evaluator_pipeline(evaluator, node_index, false,
                                              &scope, &scoped,
                                              &deferred_work);
        if (plan_status != GSH_NATIVE_PLAN_OK) {
            return false;
        }
        supported = true;
        if (!deferred_work) {
            size_t index;

            for (index = 0;
                 supported && index < evaluator->pipeline->command_count;
                 index++) {
                const gsh_native_command *planned =
                    &evaluator->pipeline->commands[index];
                const gsh_function_entry *function =
                    evaluator_function(evaluator, planned);

                if (function != NULL) {
                    supported = preflight_evaluator_function(
                        evaluator, planned, function, depth);
                } else {
                    const char *path =
                        scoped ? scoped_command_path_value(
                                     &scope, (unsigned int)index + 1U,
                                     planned, evaluator->default_path)
                               : command_path_value(
                                     evaluator->variables, planned,
                                     evaluator->default_path);

                    supported = native_planned_command_is_supported(
                        evaluator->pipeline, index, path);
                }
            }
        }
        if (supported) {
            size_t index;

            for (index = 0;
                 index < evaluator->pipeline->command_count; index++) {
                const gsh_native_command *command =
                    &evaluator->pipeline->commands[index];

                if (native_state_builtin(command) &&
                    (strcmp(command->argv[0], "shift") == 0 ||
                     gsh_builtin_set_mutates_positionals(
                         command->argc, command->argv))) {
                    evaluator->positional_mutation_possible = true;
                }
                if (evaluator->pipeline->command_count == 1U &&
                    native_cd_builtin(command)) {
                    evaluator->directory_mutation_possible = true;
                }
                if (evaluator->pipeline->command_count == 1U &&
                    native_alias_mutates(command)) {
                    evaluator->alias_mutation_possible = true;
                }
                if (evaluator->pipeline->command_count == 1U &&
                    native_function_mutates(command)) {
                    evaluator->function_mutation_possible = true;
                }
            }
            if (deferred_work) {
                evaluator->positional_mutation_possible = true;
                evaluator->directory_mutation_possible = true;
                evaluator->alias_mutation_possible = true;
                evaluator->function_mutation_possible = true;
            }
        }
        return supported;
    }
    if (node->kind == GSH_AST_CASE) {
        return native_preflight_case(evaluator, node, depth);
    }
    if (node->kind == GSH_AST_FOR) {
        return native_preflight_for(evaluator, node, depth);
    }
    if (node->kind == GSH_AST_FUNCTION) {
        evaluator->function_mutation_possible = true;
        return define_evaluator_function(evaluator, node_index);
    }
    if (node->kind == GSH_AST_SIMPLE) {
        return false;
    }
    if (node->kind != GSH_AST_PROGRAM && node->kind != GSH_AST_LIST &&
        node->kind != GSH_AST_AND_OR &&
        node->kind != GSH_AST_SUBSHELL &&
        node->kind != GSH_AST_BRACE_GROUP && node->kind != GSH_AST_IF &&
        node->kind != GSH_AST_IF_BRANCH && node->kind != GSH_AST_WHILE &&
        node->kind != GSH_AST_UNTIL) {
        return false;
    }
    child = node->first_child;
    if (node->kind != GSH_AST_PROGRAM && child == GSH_AST_NONE) {
        return false;
    }
    while (child != GSH_AST_NONE) {
        if (!native_preflight_node(evaluator, child, depth + 1U)) {
            return false;
        }
        child = evaluator->storage->nodes[child].next_sibling;
        if (++visited > evaluator->storage->node_count) {
            return false;
        }
    }
    return true;
}

static gsh_native_plan_status execute_command_substitution(
    void *opaque, const char *commands, size_t command_length, char *output,
    size_t output_capacity, size_t *output_length, int *exit_status)
{
    native_evaluator *parent = opaque;
    gsh_parse_storage *storage;
    gsh_native_pipeline *pipeline;
    gsh_variable_store *variables;
    gsh_variable_store *scope_base;
    gsh_variable_journal *scope_changes;
    gsh_function_store *local_functions = NULL;
    gsh_function_store *local_function_scratch = NULL;
    char *alias_expanded = NULL;
    const char *nested_input = commands;
    size_t nested_length = command_length;
    gsh_parse_result parsed;
    native_evaluator nested;
    gsh_background_table nested_backgrounds;
    int capture[2] = {-1, -1};
    pid_t pid;
    size_t used = 0;
    bool overflow = false;
    bool contains_null = false;
    bool read_failed = false;
    int read_error = 0;
    int wait_status = 0;

    *output_length = 0;
    *exit_status = 125;
    if (parent == NULL || parent->substitution_depth >= 32U) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (fault_should_fail("substitution-allocation", ENOMEM)) {
        errno = ENOMEM;
        perror("gsh: command substitution allocation");
        return GSH_NATIVE_PLAN_LIMIT;
    }
    storage = malloc(sizeof(*storage));
    pipeline = malloc(sizeof(*pipeline));
    variables = malloc(sizeof(*variables));
    scope_base = malloc(sizeof(*scope_base));
    scope_changes = malloc(sizeof(*scope_changes));
    if (gsh_aliases_count(parent->aliases) != 0) {
        alias_expanded = malloc(GSH_ALIAS_EXPANSION_CAP);
    }
    if (storage == NULL || pipeline == NULL || variables == NULL ||
        scope_base == NULL || scope_changes == NULL ||
        (gsh_aliases_count(parent->aliases) != 0 &&
         alias_expanded == NULL)) {
        int saved_errno = errno;

        free(storage);
        free(pipeline);
        free(variables);
        free(scope_base);
        free(scope_changes);
        free(alias_expanded);
        errno = saved_errno;
        perror("gsh: command substitution allocation");
        return GSH_NATIVE_PLAN_LIMIT;
    }
    parsed = gsh_alias_parse(
        commands, command_length, parent->aliases, alias_expanded,
        GSH_ALIAS_EXPANSION_CAP, storage, &nested_input, &nested_length);
    if (parsed.status != GSH_PARSE_OK) {
        free(storage);
        free(pipeline);
        free(variables);
        free(scope_base);
        free(scope_changes);
        free(alias_expanded);
        return parsed.status == GSH_PARSE_LIMIT ? GSH_NATIVE_PLAN_LIMIT
                                                : GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (storage_has_function(storage)) {
        local_functions =
            fault_should_fail("substitution-function-allocation", ENOMEM)
                ? NULL
                : malloc(sizeof(*local_functions));
        local_function_scratch = malloc(sizeof(*local_function_scratch));
        if (local_functions == NULL || local_function_scratch == NULL) {
            int saved_errno = errno;

            free(storage);
            free(pipeline);
            free(variables);
            free(scope_base);
            free(scope_changes);
            free(alias_expanded);
            free(local_functions);
            free(local_function_scratch);
            errno = saved_errno;
            perror("gsh: command substitution function allocation");
            return GSH_NATIVE_PLAN_LIMIT;
        }
        if (parent->functions == NULL) {
            gsh_functions_initialize(local_functions);
        } else {
            memcpy(local_functions, parent->functions,
                   sizeof(*local_functions));
        }
        gsh_functions_initialize(local_function_scratch);
    }
    memset(&nested, 0, sizeof(nested));
    nested.input = nested_input;
    nested.input_length = nested_length;
    nested.storage = storage;
    nested.pipeline = pipeline;
    nested.default_path = parent->default_path;
    nested.last_status = parent->last_status;
    nested.shell_pid = parent->shell_pid;
    nested.last_background_pid = 0;
    nested.parameter_zero = parent->parameter_zero;
    nested.positionals = parent->positionals;
    nested.options = parent->options;
    memcpy(variables, parent->variables, sizeof(*variables));
    nested.variables = variables;
    nested.journal = NULL;
    nested.aliases = parent->aliases;
    nested.alias_journal = NULL;
    nested.functions = local_functions != NULL ? local_functions
                                                : parent->functions;
    nested.function_scratch = local_function_scratch != NULL
                                  ? local_function_scratch
                                  : parent->function_scratch;
    nested.scope_base = scope_base;
    nested.scope_changes = scope_changes;
    nested.pipeline_scope = NULL;
    nested.substitution_depth = parent->substitution_depth + 1U;
    nested.preflight = true;
    nested.fatal_error = false;
    nested.static_for_items = false;
    nested.tail_exec_single = false;
    nested.positional_mutation_possible = false;
    nested.directory_mutation_possible = false;
    nested.alias_mutation_possible = false;
    nested.state_commit_invalid = false;
    gsh_background_initialize(&nested_backgrounds);
    nested.backgrounds = &nested_backgrounds;
    if (!native_preflight_node(&nested, parsed.root, 0)) {
        free(storage);
        free(pipeline);
        free(variables);
        free(scope_base);
        free(scope_changes);
        free(alias_expanded);
        free(local_functions);
        free(local_function_scratch);
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    memcpy(variables, parent->variables, sizeof(*variables));
    nested.preflight = false;
    nested.fatal_error = false;
    if (make_pipe(capture, false, "substitution-pipe") == -1) {
        perror("gsh: command substitution pipe");
        free(storage);
        free(pipeline);
        free(variables);
        free(scope_base);
        free(scope_changes);
        free(alias_expanded);
        free(local_functions);
        free(local_function_scratch);
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    pid = fault_should_fail("substitution-fork", EAGAIN) ? -1 : fork();
    if (pid == 0) {
        int status;

        close(capture[0]);
        reset_child_signals();
        if (child_duplicate_descriptor(capture[1], STDOUT_FILENO) == -1) {
            child_exec_error("command substitution output", errno);
        }
        if (capture[1] != STDOUT_FILENO) {
            close(capture[1]);
        }
        status = native_evaluate_node(&nested, parsed.root, 0);
        _exit(status & 255);
    }
    close(capture[1]);
    capture[1] = -1;
    if (pid == -1) {
        perror("gsh: command substitution fork");
        close(capture[0]);
        free(storage);
        free(pipeline);
        free(variables);
        free(scope_base);
        free(scope_changes);
        free(alias_expanded);
        free(local_functions);
        free(local_function_scratch);
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    for (;;) {
        unsigned char bytes[4096];
        ssize_t count = fault_should_fail("substitution-read", EIO)
                            ? -1
                            : read(capture[0], bytes, sizeof(bytes));

        if (count > 0) {
            size_t amount = (size_t)count;

            if (memchr(bytes, '\0', amount) != NULL) {
                contains_null = true;
                break;
            }
            if (amount > output_capacity - used) {
                overflow = true;
                break;
            }
            memcpy(output + used, bytes, amount);
            used += amount;
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            read_failed = true;
            read_error = errno;
            break;
        }
    }
    close(capture[0]);
    if (overflow || contains_null || read_failed) {
        (void)kill(pid, SIGKILL);
    }
    while (waitpid(pid, &wait_status, 0) == -1) {
        if (errno != EINTR) {
            read_failed = true;
            if (read_error == 0) {
                read_error = errno;
            }
            break;
        }
    }
    free(storage);
    free(pipeline);
    free(variables);
    free(scope_base);
    free(scope_changes);
    free(alias_expanded);
    free(local_functions);
    free(local_function_scratch);
    if (overflow) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (contains_null || read_failed) {
        if (read_failed) {
            errno = read_error != 0 ? read_error : EIO;
            perror("gsh: command substitution read");
        } else {
            fputs("gsh: command substitution contains a null byte\n",
                  stderr);
        }
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    while (used > 0 && output[used - 1U] == '\n') {
        used--;
    }
    *output_length = used;
    *exit_status = wait_status_value(wait_status);
    return GSH_NATIVE_PLAN_OK;
}

static int evaluate_return(native_evaluator *evaluator,
                           const gsh_native_command *command)
{
    unsigned int status = (unsigned int)(evaluator->last_status & 255);
    const char *cursor;

    if (command->argc > 2U || command->assignment_count != 0 ||
        command->redirect_count != 0 || evaluator->function_depth == 0) {
        fputs("gsh: return: invalid context or operands\n", stderr);
        return 1;
    }
    if (command->argc == 2U) {
        status = 0;
        cursor = command->argv[1];
        if (*cursor == '\0') {
            fputs("gsh: return: invalid status\n", stderr);
            return 1;
        }
        while (*cursor != '\0') {
            if (*cursor < '0' || *cursor > '9' || status > 25U) {
                fputs("gsh: return: invalid status\n", stderr);
                return 1;
            }
            status = status * 10U + (unsigned int)(*cursor++ - '0');
        }
        if (status > 255U) {
            fputs("gsh: return: invalid status\n", stderr);
            return 1;
        }
    }
    evaluator->returning = true;
    evaluator->return_status = (int)status;
    return (int)status;
}

static int evaluate_function(native_evaluator *evaluator,
                             const gsh_native_command *command,
                             const gsh_function_entry *entry,
                             bool apply_call_redirects)
{
    const char *saved_input = evaluator->input;
    size_t saved_input_length = evaluator->input_length;
    const gsh_parse_storage *saved_storage = evaluator->storage;
    gsh_positional_store *saved_positionals = evaluator->positionals;
    gsh_positional_store positionals;
    gsh_saved_descriptor call_saved[GSH_NATIVE_REDIRECT_CAP];
    gsh_saved_descriptor definition_saved[GSH_NATIVE_REDIRECT_CAP];
    size_t call_saved_count = 0;
    size_t definition_saved_count = 0;
    const gsh_ast_node *definition;
    int status = 125;

    if (evaluator->function_depth == GSH_FUNCTION_DEPTH_CAP) {
        fputs("gsh: function resource limit exceeded\n", stderr);
        return 125;
    }
    if (apply_call_redirects &&
        save_redirect_descriptors(command, call_saved,
                                  &call_saved_count) == -1) {
        perror("gsh: function redirection save");
        return 125;
    }
    if (apply_call_redirects &&
        apply_evaluator_redirects(evaluator->pipeline, command,
                                  &evaluator->options) == -1) {
        perror("gsh: function redirection");
        (void)restore_redirect_descriptors(call_saved, call_saved_count);
        return 1;
    }
    if (apply_native_assignments(evaluator->variables, evaluator->journal,
                                 command, &evaluator->options) !=
            GSH_ASSIGNMENT_OK ||
        gsh_positionals_assign(&positionals, command->argc - 1U,
                               command->argv + 1U) == -1) {
        perror("gsh: function arguments");
        goto restore_call;
    }
    evaluator->input = gsh_functions_text(evaluator->functions);
    evaluator->input_length = evaluator->functions->text_used;
    evaluator->storage = &evaluator->functions->programs;
    evaluator->positionals = &positionals;
    evaluator->function_depth++;
    definition = &evaluator->storage->nodes[entry->node_offset];
    if (definition->redirect_count != 0) {
        gsh_native_expansion_context expansion =
            native_expansion_context(evaluator, true, NULL);
        gsh_native_plan_status plan_status =
            gsh_native_plan_redirects_with_context(
                evaluator->input, evaluator->storage,
                entry->node_offset, &expansion, evaluator->pipeline);
        const gsh_native_command *redirects =
            &evaluator->pipeline->commands[0];

        if (plan_status != GSH_NATIVE_PLAN_OK) {
            status = plan_status == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
            goto leave_function;
        }
        if (save_redirect_descriptors(
                redirects, definition_saved,
                &definition_saved_count) == -1) {
            perror("gsh: function body redirection save");
            goto leave_function;
        }
        if (apply_evaluator_redirects(
                evaluator->pipeline, redirects,
                &evaluator->options) == -1) {
            perror("gsh: function body redirection");
            (void)restore_redirect_descriptors(
                definition_saved, definition_saved_count);
            definition_saved_count = 0;
            status = 1;
            goto leave_function;
        }
    }
    evaluator->returning = false;
    status = native_evaluate_node(evaluator, definition->first_child, 0);
    if (evaluator->returning) {
        status = evaluator->return_status;
        evaluator->returning = false;
    }
    if (restore_redirect_descriptors(definition_saved,
                                     definition_saved_count) == -1) {
        perror("gsh: function body redirection restore");
        status = 125;
    }
leave_function:
    evaluator->function_depth--;
    evaluator->positionals = saved_positionals;
    evaluator->storage = saved_storage;
    evaluator->input_length = saved_input_length;
    evaluator->input = saved_input;
restore_call:
    if (restore_redirect_descriptors(call_saved, call_saved_count) == -1) {
        perror("gsh: function redirection restore");
        status = 125;
    }
    return status;
}

static int run_pipeline_function(native_evaluator *parent,
                                 gsh_native_pipeline *pipeline,
                                 size_t command_index,
                                 gsh_variable_store *variables,
                                 bool *found)
{
    const gsh_native_command *command = &pipeline->commands[command_index];
    const gsh_function_entry *entry =
        parent == NULL ? NULL : evaluator_function(parent, command);
    native_evaluator child;
    gsh_background_table backgrounds;

    *found = entry != NULL;
    if (entry == NULL) {
        return 127;
    }
    child = *parent;
    child.pipeline = pipeline;
    child.variables = variables;
    child.journal = NULL;
    child.alias_journal = NULL;
    child.pipeline_scope = NULL;
    child.tail_exec_single = false;
    child.returning = false;
    child.fatal_error = false;
    gsh_background_initialize(&backgrounds);
    child.backgrounds = &backgrounds;
    return evaluate_function(&child, command, entry, false);
}

static int native_evaluate_pipeline(native_evaluator *evaluator,
                                    size_t node_index, size_t depth)
{
    const gsh_ast_node *node = &evaluator->storage->nodes[node_index];
    const gsh_ast_node *command =
        &evaluator->storage->nodes[node->first_child];
    pipeline_expansion_scope scope;
    bool deferred_work;
    bool scoped;
    int status;
    gsh_native_plan_status plan_status;
    const gsh_function_entry *function;

    if (command->next_sibling == GSH_AST_NONE &&
        command->kind != GSH_AST_SIMPLE) {
        status = native_evaluate_node(evaluator, node->first_child,
                                      depth + 1U);
        return (node->flags & GSH_AST_FLAG_NEGATED) != 0
                   ? (status == 0 ? 1 : 0)
                   : status;
    }
    plan_status = plan_evaluator_pipeline(evaluator, node_index, true,
                                          &scope, &scoped,
                                          &deferred_work);
    if (plan_status != GSH_NATIVE_PLAN_OK) {
        return plan_status == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
    }
    function = evaluator->pipeline->command_count == 1U
                   ? evaluator_function(
                         evaluator, &evaluator->pipeline->commands[0])
                   : NULL;
    if (function == NULL) {
        size_t index;

        for (index = 0; index < evaluator->pipeline->command_count;
             index++) {
            const gsh_native_command *planned =
                &evaluator->pipeline->commands[index];
            const char *path;

            if (evaluator_function(evaluator, planned) != NULL) {
                continue;
            }
            path = scoped ? scoped_command_path_value(
                                &scope, (unsigned int)index + 1U,
                                planned, evaluator->default_path)
                          : command_path_value(
                                evaluator->variables, planned,
                                evaluator->default_path);
            if (!native_planned_command_is_supported(
                    evaluator->pipeline, index, path)) {
                return 125;
            }
        }
    }
    if (evaluator->tail_exec_single && !evaluator->pipeline->negated &&
        evaluator->pipeline->command_count == 1U &&
        evaluator->pipeline->heredoc_count == 0U) {
        const gsh_native_command *tail = &evaluator->pipeline->commands[0];
        int builtin_status;

        if (tail->argc != 0 &&
            !native_stateless_builtin(tail, &builtin_status) &&
            !native_pwd_builtin(tail) && !native_cd_builtin(tail) &&
            !native_environment_builtin(tail) &&
            !native_variable_builtin(tail) && !native_state_builtin(tail) &&
            !native_wait_builtin(tail) && !native_alias_builtin(tail) &&
            !native_return_builtin(tail) && function == NULL) {
            int heredoc_descriptors[GSH_NATIVE_HEREDOC_CAP][2];
            char *environment_storage[CHILD_ENVIRONMENT_CAP];
            char *const *environment;

            initialize_heredoc_descriptors(heredoc_descriptors);
            if (tail->expansion_error) {
                _exit(1);
            }
            child_apply_redirects(tail, heredoc_descriptors, 0,
                                  &evaluator->options);
            if (fault_should_fail("exec", EIO)) {
                child_exec_error(tail->argv[0], errno);
            }
            environment = child_command_environment(
                evaluator->variables, tail, environment_storage);
            child_exec_direct(
                tail->argv,
                command_path_value(evaluator->variables, tail,
                                   evaluator->default_path),
                environment);
        }
    }
    if (function != NULL) {
        status = evaluate_function(
            evaluator, &evaluator->pipeline->commands[0], function, true);
        if (evaluator->pipeline->negated && status != 125) {
            status = status == 0 ? 1 : 0;
        }
    } else if (evaluator->pipeline->command_count == 1 &&
               native_return_builtin(
                   &evaluator->pipeline->commands[0])) {
        status = evaluate_return(
            evaluator, &evaluator->pipeline->commands[0]);
        if (evaluator->pipeline->negated && status != 125) {
            status = status == 0 ? 1 : 0;
            evaluator->return_status = status;
        }
    } else if (evaluator->pipeline->command_count == 1 &&
        native_wait_builtin(&evaluator->pipeline->commands[0])) {
        const gsh_native_command *wait_command =
            &evaluator->pipeline->commands[0];
        gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
        size_t saved_count;

        if (save_redirect_descriptors(wait_command, saved,
                                      &saved_count) == -1) {
            perror("gsh: wait redirection save");
            status = 125;
        } else if (apply_evaluator_redirects(
                       evaluator->pipeline, wait_command,
                       &evaluator->options) == -1) {
            perror("gsh: wait redirection");
            (void)restore_redirect_descriptors(saved, saved_count);
            status = 1;
        } else {
            size_t argument;

            status = wait_command->argc == 1 ? 0 : 127;
            if (evaluator->backgrounds != NULL) {
                pid_t targets[GSH_BACKGROUND_CAP];
                size_t target_count = 0;

                if (wait_command->argc == 1) {
                    target_count = gsh_background_snapshot(
                        evaluator->backgrounds, targets);
                }
                for (argument = 1; argument < wait_command->argc;
                     argument++) {
                    const char *text = wait_command->argv[argument];
                    char *end;
                    unsigned long number;
                    pid_t target = -1;

                    errno = 0;
                    number = strtoul(text[0] == '%' ? text + 1U : text,
                                     &end, 10);
                    if (errno == 0 && *text != '\0' &&
                        !(text[0] == '%' && text[1] == '\0') &&
                        *end == '\0' && number > 0 &&
                        number <= (unsigned long)INT_MAX) {
                        target = text[0] == '%'
                                     ? gsh_background_job_pid(
                                           evaluator->backgrounds,
                                           (uint32_t)number)
                                     : (pid_t)number;
                    }
                    targets[target_count++] = target;
                }
                for (argument = 0; argument < target_count; argument++) {
                    bool done;
                    int wait_status;
                    bool known = targets[argument] > 0 &&
                                 gsh_background_get(
                                     evaluator->backgrounds,
                                     targets[argument], &done,
                                     &wait_status);

                    if (known && !done) {
                        pid_t waited;

                        do {
                            waited = waitpid(targets[argument], &wait_status,
                                             0);
                        } while (waited == -1 && errno == EINTR);
                        if (waited == targets[argument]) {
                            (void)gsh_background_record(
                                evaluator->backgrounds, targets[argument],
                                wait_status);
                            done = true;
                        } else {
                            known = false;
                        }
                    }
                    if (known && done &&
                        gsh_background_consume(evaluator->backgrounds,
                                               targets[argument],
                                               &wait_status)) {
                        if (wait_command->argc != 1 &&
                            argument + 1U == target_count) {
                            status = wait_status_value(wait_status);
                        }
                    } else if (wait_command->argc != 1 &&
                               argument + 1U == target_count) {
                        status = 127;
                    }
                }
                if (wait_command->argc == 1) {
                    status = 0;
                }
            }
            if (restore_redirect_descriptors(saved, saved_count) == -1) {
                perror("gsh: wait redirection restore");
                status = 125;
            }
            if (evaluator->pipeline->negated && status != 125) {
                status = status == 0 ? 1 : 0;
            }
        }
    } else if (evaluator->pipeline->command_count == 1 &&
               native_variable_builtin(
                   &evaluator->pipeline->commands[0])) {
        status = run_evaluator_variable_builtin(
            evaluator->pipeline, evaluator->variables,
            evaluator->journal, &evaluator->options,
            evaluator->functions);
    } else if (evaluator->pipeline->command_count == 1 &&
        native_state_builtin(&evaluator->pipeline->commands[0])) {
        status = run_evaluator_state_builtin(
            evaluator->pipeline, evaluator->variables,
            evaluator->journal, evaluator->positionals,
            &evaluator->options);
    } else if (evaluator->pipeline->command_count == 1 &&
               native_cd_builtin(&evaluator->pipeline->commands[0])) {
        status = run_evaluator_cd_builtin(
            evaluator->pipeline, evaluator->variables,
            evaluator->scope_base,
            evaluator->journal, &evaluator->options, NULL, 0);
    } else if (evaluator->pipeline->command_count == 1 &&
               native_alias_builtin(&evaluator->pipeline->commands[0])) {
        status = run_evaluator_alias_builtin(
            evaluator->pipeline, evaluator->aliases,
            evaluator->alias_journal, &evaluator->options);
        if (status == 125 && evaluator->alias_journal != NULL) {
            evaluator->state_commit_invalid = true;
        }
    } else {
        status = run_native_noninteractive_pipeline(
            evaluator->pipeline, evaluator->default_path,
            evaluator->variables, evaluator->journal,
            evaluator->aliases, evaluator->alias_journal,
            scoped ? &scope : NULL, evaluator->positionals,
            &evaluator->options, evaluator);
    }
    if (status == 125 && evaluator->pipeline->command_count == 1 &&
        (native_variable_builtin(&evaluator->pipeline->commands[0]) ||
         native_state_builtin(&evaluator->pipeline->commands[0]) ||
         native_cd_builtin(&evaluator->pipeline->commands[0]) ||
         native_alias_builtin(&evaluator->pipeline->commands[0]) ||
         (evaluator->pipeline->commands[0].argc == 0 &&
          evaluator->pipeline->commands[0].assignment_count != 0))) {
        evaluator->fatal_error = true;
    }
    return status;
}

static int native_evaluate_if(native_evaluator *evaluator,
                              const gsh_ast_node *node, size_t depth)
{
    size_t branch_index = node->first_child;

    while (branch_index != GSH_AST_NONE) {
        const gsh_ast_node *branch =
            &evaluator->storage->nodes[branch_index];
        size_t first = branch->first_child;
        size_t second = evaluator->storage->nodes[first].next_sibling;

        if (second == GSH_AST_NONE) {
            return native_evaluate_node(evaluator, first, depth + 1U);
        }
        {
            int condition = native_evaluate_node(evaluator, first,
                                                 depth + 1U);

            if (evaluator->returning) {
                return evaluator->return_status;
            }
            if (condition == 0) {
                return native_evaluate_node(evaluator, second,
                                            depth + 1U);
            }
        }
        branch_index = branch->next_sibling;
    }
    return 0;
}

static int native_evaluate_case(native_evaluator *evaluator,
                                const gsh_ast_node *node, size_t depth)
{
    gsh_native_expansion_context expansion =
        native_expansion_context(evaluator, true, NULL);
    char *subject;
    size_t item_index = node->first_child;
    bool fallthrough = false;
    int status = 0;

    if (gsh_native_expand_scalar(
            evaluator->input,
            evaluator->storage->words[node->first_word], &expansion,
            evaluator->pipeline, &subject) != GSH_NATIVE_PLAN_OK) {
        return 125;
    }
    while (item_index != GSH_AST_NONE) {
        const gsh_ast_node *item =
            &evaluator->storage->nodes[item_index];
        bool matched = fallthrough;
        size_t pattern;

        if (!matched) {
            for (pattern = 0; pattern < item->word_count; pattern++) {
                gsh_word_ref reference = evaluator->storage
                                             ->words[item->first_word +
                                                     pattern];
                char pattern_text[GSH_NATIVE_TEXT_CAP];

                if (!native_case_pattern(evaluator->input, reference,
                                         pattern_text)) {
                    return 125;
                }
                if (fnmatch(pattern_text, subject, 0) == 0) {
                    matched = true;
                    break;
                }
            }
        }
        if (matched) {
            status = item->first_child == GSH_AST_NONE
                         ? 0
                         : native_evaluate_node(evaluator,
                                                item->first_child,
                                                depth + 1U);
            if (evaluator->returning) {
                return evaluator->return_status;
            }
            if ((item->flags & GSH_AST_FLAG_CASE_FALLTHROUGH) == 0) {
                return status;
            }
            fallthrough = true;
        }
        item_index = item->next_sibling;
    }
    return status;
}

static int native_evaluate_for(native_evaluator *evaluator,
                               const gsh_ast_node *node, size_t depth)
{
    gsh_word_ref name =
        evaluator->storage->words[node->first_word];
    char *items[GSH_NATIVE_ARGUMENT_CAP];
    char *item_text = NULL;
    size_t item_count;
    size_t index;
    int status = 0;

    if (evaluator->static_for_items &&
        (node->flags & GSH_AST_FLAG_FOR_HAS_IN) != 0) {
        for (index = 1U; index < node->word_count; index++) {
            gsh_word_ref item = evaluator->storage
                                    ->words[node->first_word + index];
            gsh_native_plan_status assignment;

            assignment = evaluator_variable_assign(
                evaluator, evaluator->input + name.begin,
                name.end - name.begin, evaluator->input + item.begin,
                item.end - item.begin);
            if (assignment != GSH_NATIVE_PLAN_OK) {
                return assignment == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
            }
            status = native_evaluate_node(evaluator, node->first_child,
                                          depth + 1U);
            if (evaluator->fatal_error || evaluator->returning) {
                break;
            }
        }
        return status;
    }
    if ((node->flags & GSH_AST_FLAG_FOR_HAS_IN) != 0) {
        gsh_native_expansion_context expansion =
            native_expansion_context(evaluator, true, NULL);
        gsh_native_plan_status expansion_status;

        expansion_status = gsh_native_expand_words(
            evaluator->input,
            evaluator->storage->words + node->first_word + 1U,
            node->word_count - 1U, &expansion, evaluator->pipeline, items,
            &item_count);
        if (expansion_status != GSH_NATIVE_PLAN_OK) {
            return expansion_status == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
        }
        if (item_count == 0) {
            return 0;
        }
        item_text = fault_should_fail("for-allocation", ENOMEM)
                        ? NULL
                        : malloc(evaluator->pipeline->text_used);
        if (item_text == NULL) {
            perror("gsh: for items");
            return 125;
        }
        memcpy(item_text, evaluator->pipeline->text,
               evaluator->pipeline->text_used);
        for (index = 0; index < item_count; index++) {
            items[index] = item_text +
                           (items[index] - evaluator->pipeline->text);
        }
    } else {
        item_count = gsh_positionals_count(evaluator->positionals);
        gsh_positionals_view(evaluator->positionals, items);
    }
    for (index = 0; index < item_count; index++) {
        gsh_native_plan_status assignment = evaluator_variable_assign(
            evaluator, evaluator->input + name.begin,
            name.end - name.begin, items[index], strlen(items[index]));

        if (assignment != GSH_NATIVE_PLAN_OK) {
            status = assignment == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
            break;
        }
        status = native_evaluate_node(evaluator, node->first_child,
                                      depth + 1U);
        if (evaluator->fatal_error || evaluator->returning) {
            break;
        }
    }
    free(item_text);
    return status;
}

static int native_evaluate_node_inner(native_evaluator *evaluator,
                                      size_t node_index, size_t depth)
{
    const gsh_ast_node *node = &evaluator->storage->nodes[node_index];
    size_t child;
    int status = 0;

    if (depth > 128) {
        return 125;
    }
    if (evaluator->returning) {
        return evaluator->return_status;
    }
    if (node->kind == GSH_AST_FUNCTION) {
        if (!define_evaluator_function(evaluator, node_index)) {
            perror("gsh: function definition");
            return errno == ENOSPC ? 125 : 1;
        }
        return 0;
    }
    if (evaluator->static_for_items && node->kind == GSH_AST_PIPELINE &&
        node->first_child != GSH_AST_NONE) {
        const gsh_ast_node *command =
            &evaluator->storage->nodes[node->first_child];

        if (command->kind == GSH_AST_SIMPLE &&
            command->next_sibling == GSH_AST_NONE &&
            command->word_count == 1U && command->redirect_count == 0) {
            gsh_word_ref word =
                evaluator->storage->words[command->first_word];
            size_t length = word.end - word.begin;
            int direct_status =
                (length == 1U && evaluator->input[word.begin] == ':') ||
                        (length == 4U &&
                         memcmp(evaluator->input + word.begin, "true", 4) ==
                             0)
                    ? 0
                    : 1;

            return (node->flags & GSH_AST_FLAG_NEGATED) != 0
                       ? (direct_status == 0 ? 1 : 0)
                       : direct_status;
        }
    }
    if (node->kind == GSH_AST_PIPELINE) {
        return native_evaluate_pipeline(evaluator, node_index, depth);
    }
    if (node->kind == GSH_AST_SUBSHELL) {
        pid_t pid = fault_should_fail("subshell-fork", EAGAIN) ? -1 : fork();

        if (pid == 0) {
            int child_status = native_evaluate_node(
                evaluator, node->first_child, depth + 1U);

            _exit(child_status & 255);
        }
        if (pid == -1) {
            perror("gsh: subshell fork");
            return 125;
        }
        for (;;) {
            int wait_status;
            pid_t waited = waitpid(pid, &wait_status, 0);

            if (waited == pid) {
                return wait_status_value(wait_status);
            }
            if (waited == -1 && errno == EINTR) {
                continue;
            }
            perror("gsh: subshell waitpid");
            return 125;
        }
    }
    if (node->kind == GSH_AST_IF) {
        return native_evaluate_if(evaluator, node, depth);
    }
    if (node->kind == GSH_AST_CASE) {
        return native_evaluate_case(evaluator, node, depth);
    }
    if (node->kind == GSH_AST_FOR) {
        return native_evaluate_for(evaluator, node, depth);
    }
    if (node->kind == GSH_AST_WHILE || node->kind == GSH_AST_UNTIL) {
        size_t condition = node->first_child;
        size_t body = evaluator->storage->nodes[condition].next_sibling;
        int body_status = 0;

        for (;;) {
            int condition_status = native_evaluate_node(
                evaluator, condition, depth + 1U);
            if (evaluator->returning) {
                return evaluator->return_status;
            }
            bool selected = node->kind == GSH_AST_WHILE
                                ? condition_status == 0
                                : condition_status != 0;

            if (!selected) {
                return body_status;
            }
            body_status = native_evaluate_node(evaluator, body, depth + 1U);
            if (evaluator->returning) {
                return evaluator->return_status;
            }
        }
    }
    child = node->first_child;
    while (child != GSH_AST_NONE) {
        const gsh_ast_node *child_node =
            &evaluator->storage->nodes[child];

        if (node->kind == GSH_AST_AND_OR) {
            if (child_node->connector == GSH_TOKEN_AND_IF && status != 0) {
                child = child_node->next_sibling;
                continue;
            }
            if (child_node->connector == GSH_TOKEN_OR_IF && status == 0) {
                child = child_node->next_sibling;
                continue;
            }
        }
        status = native_evaluate_node(evaluator, child, depth + 1U);
        if (evaluator->returning) {
            return evaluator->return_status;
        }
        child = child_node->next_sibling;
    }
    return status;
}

static int native_evaluate_async(native_evaluator *evaluator,
                                 size_t node_index, size_t depth)
{
    pid_t pid;

    if (evaluator->backgrounds == NULL ||
        !gsh_background_has_capacity(evaluator->backgrounds)) {
        errno = EAGAIN;
        perror("gsh: asynchronous list");
        return 125;
    }
    pid = fault_should_fail("async-fork", EAGAIN) ? -1 : fork();
    if (pid == 0) {
        gsh_background_table child_backgrounds;
        native_evaluator child = *evaluator;
        int null_descriptor;
        int status;

        (void)setpgid(0, 0);
        reset_child_signals();
        null_descriptor = open("/dev/null", O_RDONLY);
        if (null_descriptor == -1 ||
            child_duplicate_descriptor(null_descriptor, STDIN_FILENO) ==
                -1) {
            child_exec_error("asynchronous standard input", errno);
        }
        if (null_descriptor != STDIN_FILENO) {
            close(null_descriptor);
        }
        gsh_background_initialize(&child_backgrounds);
        child.backgrounds = &child_backgrounds;
        child.last_background_pid = 0;
        child.journal = NULL;
        child.alias_journal = NULL;
        child.tail_exec_single =
            async_node_has_single_pipeline(evaluator, node_index);
        status = native_evaluate_node_inner(&child, node_index, depth);
        _exit(status & 255);
    }
    if (pid == -1) {
        perror("gsh: asynchronous fork");
        return 125;
    }
    (void)setpgid(pid, pid);
    if (gsh_background_add(evaluator->backgrounds, pid, NULL) == -1) {
        int saved_errno = errno;

        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {
        }
        errno = saved_errno;
        perror("gsh: asynchronous registry");
        return 125;
    }
    evaluator->last_background_pid = (long)pid;
    return 0;
}

static int native_evaluate_node(native_evaluator *evaluator,
                                size_t node_index, size_t depth)
{
    int status;

    if (evaluator->fatal_error) {
        return 1;
    }
    status = (evaluator->storage->nodes[node_index].flags &
              GSH_AST_FLAG_ASYNC) != 0
                 ? native_evaluate_async(evaluator, node_index, depth)
                 : native_evaluate_node_inner(evaluator, node_index, depth);

    evaluator->last_status = status;
    return status;
}

static bool native_node_is_supported(shell_state *state, size_t node_index)
{
    native_evaluator evaluator;
    bool definitions = storage_has_function(state->parse_storage);

    if (definitions &&
        (ensure_function_state(state, true) == -1 ||
         !gsh_functions_clone(state->function_scratch,
                              state->functions))) {
        state->pending_function_commit = false;
        return false;
    }

    memset(&evaluator, 0, sizeof(evaluator));
    memcpy(state->variable_scratch, state->variables,
           sizeof(*state->variable_scratch));
    evaluator.input = state->pending_input;
    evaluator.input_length = state->pending_input_length;
    evaluator.storage = state->parse_storage;
    evaluator.pipeline = state->native_pipeline;
    evaluator.default_path = state->default_path;
    evaluator.last_status = state->last_status;
    evaluator.shell_pid = (long)state->shell_pgid;
    evaluator.last_background_pid = state->last_background_pid;
    evaluator.parameter_zero = state->parameter_zero;
    evaluator.positionals = state->positionals;
    evaluator.options = state->options;
    evaluator.variables = state->variable_scratch;
    evaluator.journal = NULL;
    evaluator.aliases = state->aliases;
    evaluator.alias_journal = NULL;
    evaluator.functions = definitions ? state->function_scratch
                                      : state->functions;
    evaluator.function_scratch = definitions ? NULL
                                              : state->function_scratch;
    evaluator.scope_base = state->pipeline_variables;
    evaluator.scope_changes = state->pipeline_changes;
    evaluator.pipeline_scope = NULL;
    evaluator.substitution_depth = 0;
    evaluator.preflight = true;
    evaluator.fatal_error = false;
    evaluator.static_for_items = false;
    evaluator.tail_exec_single = false;
    evaluator.positional_mutation_possible = false;
    evaluator.directory_mutation_possible = false;
    evaluator.alias_mutation_possible = false;
    evaluator.state_commit_invalid = false;
    evaluator.backgrounds = &state->background_jobs;
    state->pending_positional_commit =
        state->parse_storage->node_count > node_index &&
        native_preflight_node(&evaluator, node_index, 0);
    if (!state->pending_positional_commit) {
        state->pending_directory_commit = false;
        state->pending_alias_commit = false;
        state->pending_function_commit = false;
        return false;
    }
    state->pending_directory_commit =
        evaluator.directory_mutation_possible;
    state->pending_positional_commit =
        evaluator.positional_mutation_possible;
    state->pending_alias_commit = evaluator.alias_mutation_possible;
    state->pending_function_commit = evaluator.function_mutation_possible;
    return true;
}

static bool native_command_is_supported(shell_state *state)
{
    return native_node_is_supported(state, state->pending_parse.root);
}

enum { GSH_REACTOR_EVALUATION_BUDGET = 16 };

static bool reactor_literal_word(const char *input, gsh_word_ref word)
{
    size_t offset;

    for (offset = word.begin; offset < word.end; offset++) {
        unsigned char byte = (unsigned char)input[offset];

        if (byte == '\\' || byte == '\'' || byte == '"' || byte == '$' ||
            byte == 0x60U || byte == '~' || byte == '*' || byte == '?' ||
            byte == '[') {
            return false;
        }
    }
    return true;
}

static bool reactor_word_is(const char *input, gsh_word_ref word,
                            const char *text)
{
    size_t length = strlen(text);

    return word.end - word.begin == length &&
           memcmp(input + word.begin, text, length) == 0;
}

static bool literal_command_word_is(const char *input, gsh_word_ref word,
                                    const char *text)
{
    enum { QUOTE_NONE, QUOTE_SINGLE, QUOTE_DOUBLE } quote = QUOTE_NONE;
    size_t expected = 0;
    size_t offset;

    for (offset = word.begin; offset < word.end; offset++) {
        unsigned char byte = (unsigned char)input[offset];

        if (quote == QUOTE_SINGLE) {
            if (byte == '\'') {
                quote = QUOTE_NONE;
                continue;
            }
        } else if (quote == QUOTE_DOUBLE) {
            if (byte == '"') {
                quote = QUOTE_NONE;
                continue;
            }
            if (byte == '$' || byte == 0x60U) {
                return false;
            }
            if (byte == '\\' && offset + 1U < word.end &&
                (input[offset + 1U] == '$' ||
                 input[offset + 1U] == 0x60 ||
                 input[offset + 1U] == '"' ||
                 input[offset + 1U] == '\\')) {
                byte = (unsigned char)input[++offset];
            }
        } else if (byte == '\'') {
            quote = QUOTE_SINGLE;
            continue;
        } else if (byte == '"') {
            quote = QUOTE_DOUBLE;
            continue;
        } else if (byte == '\\' && offset + 1U < word.end) {
            byte = (unsigned char)input[++offset];
        } else if (byte == '$' || byte == 0x60U || byte == '~') {
            return false;
        }
        if (text[expected] == '\0' ||
            byte != (unsigned char)text[expected++]) {
            return false;
        }
    }
    return quote == QUOTE_NONE && text[expected] == '\0';
}

static bool reactor_safe_node(shell_state *state, size_t node_index,
                              size_t depth, size_t *budget,
                              bool *contains_for)
{
    const gsh_ast_node *node;
    size_t child;
    size_t total = 0;

    if (depth > 32U || node_index >= state->parse_storage->node_count) {
        return false;
    }
    node = &state->parse_storage->nodes[node_index];
    *contains_for = false;
    if ((node->flags & GSH_AST_FLAG_ASYNC) != 0 ||
        node->redirect_count != 0) {
        return false;
    }
    if (node->kind == GSH_AST_SIMPLE) {
        gsh_word_ref word;

        if (node->word_count != 1U) {
            return false;
        }
        word = state->parse_storage->words[node->first_word];
        if (!reactor_word_is(state->pending_input, word, ":") &&
            !reactor_word_is(state->pending_input, word, "true") &&
            !reactor_word_is(state->pending_input, word, "false")) {
            return false;
        }
        *budget = 1U;
        return true;
    }
    if (node->kind == GSH_AST_PIPELINE) {
        if (node->first_child == GSH_AST_NONE ||
            state->parse_storage->nodes[node->first_child].next_sibling !=
                GSH_AST_NONE) {
            return false;
        }
        return reactor_safe_node(state, node->first_child, depth + 1U,
                                 budget, contains_for);
    }
    if (node->kind == GSH_AST_FOR) {
        size_t iterations;
        size_t body_budget;
        bool body_contains_for;
        size_t index;
        size_t maximum_value_length = 0;
        size_t old_length = 0;
        size_t new_length;
        gsh_word_ref name;
        bool is_set;
        unsigned int attributes;
        bool exists;

        if ((node->flags & GSH_AST_FLAG_FOR_HAS_IN) == 0 ||
            node->word_count == 0 || node->first_child == GSH_AST_NONE) {
            return false;
        }
        name = state->parse_storage->words[node->first_word];
        exists = gsh_variables_get_state(
            state->variables, state->pending_input + name.begin,
            name.end - name.begin, &is_set, &attributes);
        if (exists && (attributes & GSH_VARIABLE_READONLY) != 0) {
            return false;
        }
        for (index = 1U; index < node->word_count; index++) {
            gsh_word_ref item = state->parse_storage
                                    ->words[node->first_word + index];
            size_t length = item.end - item.begin;

            if (length >= GSH_NATIVE_TEXT_CAP ||
                !reactor_literal_word(state->pending_input, item)) {
                return false;
            }
            if (length > maximum_value_length) {
                maximum_value_length = length;
            }
        }
        iterations = node->word_count - 1U;
        if (iterations != 0) {
            size_t name_length = name.end - name.begin;

            if (exists) {
                bool found;
                const char *value = gsh_variables_lookup(
                    state->variables, state->pending_input + name.begin,
                    name_length, &found);

                if (value == NULL) {
                    return false;
                }
                old_length = name_length +
                             (is_set && found ? strlen(value) : 0U) + 2U;
            } else if (state->variables->count == GSH_VARIABLE_CAP) {
                return false;
            }
            new_length = name_length + maximum_value_length + 2U;
            if (new_length > old_length + GSH_VARIABLE_TEXT_CAP -
                                           state->variables->text_used) {
                return false;
            }
        }
        if (!reactor_safe_node(state, node->first_child, depth + 1U,
                               &body_budget, &body_contains_for)) {
            return false;
        }
        if (iterations != 0 &&
            body_budget + 1U >
                (GSH_REACTOR_EVALUATION_BUDGET - 1U) / iterations) {
            return false;
        }
        *budget = 1U + iterations * (body_budget + 1U);
        *contains_for = true;
        (void)body_contains_for;
        return *budget <= GSH_REACTOR_EVALUATION_BUDGET;
    }
    if (node->kind != GSH_AST_PROGRAM && node->kind != GSH_AST_LIST &&
        node->kind != GSH_AST_AND_OR) {
        return false;
    }
    child = node->first_child;
    while (child != GSH_AST_NONE) {
        size_t child_budget;
        bool child_contains_for;

        if (!reactor_safe_node(state, child, depth + 1U, &child_budget,
                               &child_contains_for) ||
            child_budget > GSH_REACTOR_EVALUATION_BUDGET - total) {
            return false;
        }
        total += child_budget;
        *contains_for = *contains_for || child_contains_for;
        child = state->parse_storage->nodes[child].next_sibling;
    }
    *budget = total;
    return total != 0;
}

static void initialize_interactive_evaluator(native_evaluator *evaluator,
                                             shell_state *state,
                                             gsh_variable_store *variables)
{
    memset(evaluator, 0, sizeof(*evaluator));
    evaluator->input = state->pending_input;
    evaluator->input_length = state->pending_input_length;
    evaluator->storage = state->parse_storage;
    evaluator->pipeline = state->native_pipeline;
    evaluator->default_path = state->default_path;
    evaluator->last_status = state->last_status;
    evaluator->shell_pid = (long)state->shell_pgid;
    evaluator->last_background_pid = state->last_background_pid;
    evaluator->parameter_zero = state->parameter_zero;
    evaluator->positionals = state->positionals;
    evaluator->options = state->options;
    evaluator->variables = variables;
    evaluator->journal = NULL;
    evaluator->aliases = state->aliases;
    evaluator->alias_journal = NULL;
    evaluator->functions = state->functions;
    evaluator->function_scratch = state->function_scratch;
    evaluator->scope_base = state->pipeline_variables;
    evaluator->scope_changes = state->pipeline_changes;
    evaluator->pipeline_scope = NULL;
    evaluator->substitution_depth = 0;
    evaluator->preflight = false;
    evaluator->fatal_error = false;
    evaluator->static_for_items = true;
    evaluator->tail_exec_single = false;
    evaluator->positional_mutation_possible = false;
    evaluator->directory_mutation_possible = false;
    evaluator->alias_mutation_possible = false;
    evaluator->state_commit_invalid = false;
    evaluator->backgrounds = &state->background_jobs;
}

static void managed_pipeline_child(
    shell_state *state, managed_pty *pty, int gate_read, int gate_write,
    const sigset_t *previous, const gsh_native_pipeline *pipeline,
    const pipeline_expansion_scope *scope)
{
    native_evaluator evaluator;
    gsh_background_table backgrounds;
    gsh_shell_options options = state->options;
    char release;
    int status;

    (void)close(gate_write);
    (void)close(pty->master);
    reset_child_signals();
    if (attach_child_pty(state, pty) == -1) {
        child_exec_error("managed pipeline PTY", errno);
    }
    (void)sigprocmask(SIG_SETMASK, previous, NULL);
    close_child_reactor_descriptors(state, -1);
    while (read(gate_read, &release, sizeof(release)) == -1 &&
           errno == EINTR) {
    }
    (void)close(gate_read);
    initialize_interactive_evaluator(&evaluator, state, state->variables);
    gsh_background_initialize(&backgrounds);
    evaluator.backgrounds = &backgrounds;
    status = run_native_noninteractive_pipeline(
        (gsh_native_pipeline *)pipeline, state->default_path,
        state->variables, NULL, state->aliases, NULL, scope,
        state->positionals, &options, &evaluator);
    _exit(status & 255);
}

static void managed_pipeline_parent(shell_state *state, managed_pty *pty,
                                    int gate_write, pid_t pid,
                                    const sigset_t *previous)
{
    if (pid == -1 ||
        gsh_async_repl_attach(state->async_repl,
                              state->async_dispatch_cell, pid, pid,
                              pty->master) == -1) {
        int saved_errno = errno;

        if (pid > 0) {
            (void)kill(pid, SIGKILL);
        }
        (void)close(pty->master);
        output_format(state, "gsh: managed pipeline fork: %s\r\n",
                      strerror(saved_errno));
        gsh_async_repl_finish(state->async_repl,
                              state->async_dispatch_cell, 125 << 8, false);
    }
    (void)close(gate_write);
    (void)sigprocmask(SIG_SETMASK, previous, NULL);
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

static void start_async_native_pipeline(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const pipeline_expansion_scope *scope)
{
    managed_pty pty = {.master = -1, .slave_hold = -1};
    int gate[2] = {-1, -1};
    sigset_t blocked;
    sigset_t previous;
    pid_t pid;

    if (open_managed_pty(&pty) == -1 ||
        make_pipe(gate, false, "job-pipe") == -1) {
        output_format(state, "gsh: managed pipeline: %s\r\n",
                      strerror(errno));
        if (pty.master >= 0) {
            (void)close(pty.master);
        }
        if (pty.slave_hold >= 0) {
            (void)close(pty.slave_hold);
        }
        state->mode = MODE_EDITOR;
        return;
    }
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        output_format(state, "gsh: managed pipeline mask: %s\r\n",
                      strerror(errno));
        (void)close(pty.master);
        (void)close(pty.slave_hold);
        (void)close(gate[0]);
        (void)close(gate[1]);
        state->mode = MODE_EDITOR;
        return;
    }
    pid = fork();
    if (pid == 0) {
        managed_pipeline_child(state, &pty, gate[0], gate[1], &previous,
                               pipeline, scope);
        _exit(125);
    }
    (void)close(pty.slave_hold);
    pty.slave_hold = -1;
    (void)close(gate[0]);
    managed_pipeline_parent(state, &pty, gate[1], pid, &previous);
}

static bool native_pipeline_node_is_wait(const shell_state *state,
                                         size_t pipeline)
{
    const gsh_ast_node *pipeline_node;
    const gsh_ast_node *command;

    if (pipeline >= state->parse_storage->node_count) {
        return false;
    }
    pipeline_node = &state->parse_storage->nodes[pipeline];
    if (pipeline_node->kind != GSH_AST_PIPELINE ||
        pipeline_node->first_child == GSH_AST_NONE ||
        state->parse_storage->nodes[pipeline_node->first_child].next_sibling !=
            GSH_AST_NONE) {
        return false;
    }
    command = &state->parse_storage->nodes[pipeline_node->first_child];
    return command->kind == GSH_AST_SIMPLE && command->word_count != 0 &&
           literal_command_word_is(
               state->pending_input,
               state->parse_storage->words[command->first_word], "wait");
}

static bool native_list_node_is_wait(const shell_state *state,
                                     size_t node_index)
{
    const gsh_ast_node *node;
    size_t pipeline;

    if (node_index >= state->parse_storage->node_count) {
        return false;
    }
    node = &state->parse_storage->nodes[node_index];
    if (node->kind != GSH_AST_AND_OR) {
        return false;
    }
    pipeline = node->first_child;
    while (pipeline != GSH_AST_NONE) {
        const gsh_ast_node *pipeline_node =
            &state->parse_storage->nodes[pipeline];

        if (native_pipeline_node_is_wait(state, pipeline)) {
            return true;
        }
        pipeline = pipeline_node->next_sibling;
    }
    return false;
}

static bool wait_pipeline_has_substitution(const shell_state *state,
                                           size_t pipeline)
{
    const gsh_ast_node *pipeline_node =
        &state->parse_storage->nodes[pipeline];
    const gsh_ast_node *command =
        &state->parse_storage->nodes[pipeline_node->first_child];
    size_t word_index;

    for (word_index = 1U; word_index < command->word_count; word_index++) {
        gsh_word_ref word = state->parse_storage
                                ->words[command->first_word + word_index];
        enum { WAIT_QUOTE_NONE, WAIT_QUOTE_SINGLE, WAIT_QUOTE_DOUBLE } quote =
            WAIT_QUOTE_NONE;
        size_t offset;

        for (offset = word.begin; offset < word.end; offset++) {
            unsigned char byte = (unsigned char)state->pending_input[offset];

            if (quote == WAIT_QUOTE_SINGLE) {
                if (byte == '\'') {
                    quote = WAIT_QUOTE_NONE;
                }
                continue;
            }
            if (byte == '\\' && offset + 1U < word.end) {
                offset++;
                continue;
            }
            if (byte == '"') {
                quote = quote == WAIT_QUOTE_DOUBLE ? WAIT_QUOTE_NONE
                                                   : WAIT_QUOTE_DOUBLE;
                continue;
            }
            if (quote == WAIT_QUOTE_NONE && byte == '\'') {
                quote = WAIT_QUOTE_SINGLE;
                continue;
            }
            if (byte == 0x60U ||
                (byte == '$' && offset + 1U < word.end &&
                 state->pending_input[offset + 1U] == '(')) {
                return true;
            }
        }
    }
    return false;
}

static bool start_list_wait(shell_state *state, size_t pipeline)
{
    native_evaluator evaluator;
    pipeline_expansion_scope scope;
    gsh_native_plan_status plan_status;
    bool deferred_work;
    bool scoped;

    if (wait_pipeline_has_substitution(state, pipeline)) {
        output_text(state,
                    "gsh: wait expansion requires isolated continuation"
                    "\r\n");
        state->last_status = 125;
        return false;
    }
    initialize_interactive_evaluator(&evaluator, state, state->variables);
    plan_status = plan_evaluator_pipeline(&evaluator, pipeline, false, &scope,
                                          &scoped, &deferred_work);
    if (plan_status != GSH_NATIVE_PLAN_OK || deferred_work || scoped ||
        state->native_pipeline->command_count != 1 ||
        !native_wait_builtin(&state->native_pipeline->commands[0]) ||
        state->native_pipeline->commands[0].assignment_count != 0 ||
        state->native_pipeline->commands[0].redirect_count != 0) {
        state->last_status = plan_status == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
        return false;
    }
    state->variable_generation++;
    begin_background_wait(state, state->native_pipeline);
    return true;
}

static bool start_background_node(shell_state *state, size_t node_index)
{
    sigset_t blocked;
    sigset_t previous;
    pid_t pid;
    uint32_t job_id;

    if (!gsh_background_has_capacity(&state->background_jobs)) {
        output_text(state, "gsh: asynchronous registry full\r\n");
        return false;
    }
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        output_format(state, "gsh: asynchronous sigprocmask: %s\r\n",
                      strerror(errno));
        return false;
    }
    pid = fault_should_fail("async-fork", EAGAIN) ? -1 : fork();
    if (pid == 0) {
        native_evaluator evaluator;
        gsh_background_table child_backgrounds;
        int null_descriptor;
        int status;

        memset(&evaluator, 0, sizeof(evaluator));
        (void)setpgid(0, 0);
        reset_child_signals();
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        close(state->tty_fd);
        close(state->signal_pipe[0]);
        close(state->signal_pipe[1]);
        if (state->prompt_worker_fd >= 0) {
            close(state->prompt_worker_fd);
        }
        if (state->variable_commit_fd >= 0) {
            close(state->variable_commit_fd);
        }
        if (state->directory_commit_socket >= 0) {
            close(state->directory_commit_socket);
        }
        null_descriptor = open("/dev/null", O_RDONLY);
        if (null_descriptor == -1 ||
            child_duplicate_descriptor(null_descriptor, STDIN_FILENO) ==
                -1) {
            child_exec_error("asynchronous standard input", errno);
        }
        if (null_descriptor != STDIN_FILENO) {
            close(null_descriptor);
        }
        initialize_interactive_evaluator(&evaluator, state,
                                         state->variables);
        gsh_background_initialize(&child_backgrounds);
        evaluator.backgrounds = &child_backgrounds;
        evaluator.last_background_pid = 0;
        evaluator.static_for_items = false;
        evaluator.tail_exec_single =
            async_node_has_single_pipeline(&evaluator, node_index);
        status = native_evaluate_node_inner(&evaluator, node_index, 0);
        _exit(status & 255);
    }
    if (pid == -1) {
        int saved_errno = errno;

        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        output_format(state, "gsh: asynchronous fork: %s\r\n",
                      strerror(saved_errno));
        return false;
    }
    (void)setpgid(pid, pid);
    if (gsh_background_add(&state->background_jobs, pid, &job_id) == -1) {
        int saved_errno = errno;

        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        output_format(state, "gsh: asynchronous registry: %s\r\n",
                      strerror(saved_errno));
        return false;
    }
    state->last_background_pid = (long)pid;
    state->last_status = 0;
    output_format(state, "[%u] %ld\r\n", job_id, (long)pid);
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
    return true;
}

static void continue_native_and_or(shell_state *state)
{
    while (state->pending_and_or_active &&
           state->pending_and_or_next != GSH_AST_NONE) {
        size_t pipeline = state->pending_and_or_next;
        const gsh_ast_node *node =
            &state->parse_storage->nodes[pipeline];

        state->pending_and_or_next = node->next_sibling;
        if ((node->connector == GSH_TOKEN_AND_IF &&
             state->last_status != 0) ||
            (node->connector == GSH_TOKEN_OR_IF &&
             state->last_status == 0)) {
            continue;
        }
        if (!native_node_is_supported(state, pipeline)) {
            output_text(state,
                        "gsh: native AND-OR continuation unsupported\r\n");
            state->last_status = 125;
            state->pending_and_or_active = false;
            break;
        }
        if (native_pipeline_node_is_wait(state, pipeline)) {
            if (!start_list_wait(state, pipeline)) {
                continue;
            }
            return;
        }
        state->mode = MODE_DISPATCH;
        start_native_compound(state, pipeline);
        return;
    }
    state->pending_and_or_active = false;
    state->pending_and_or_next = GSH_AST_NONE;
    if (state->pending_list_active) {
        continue_native_list(state);
    } else {
        state->mode = MODE_EDITOR;
        queue_prompt(state);
    }
}

static void continue_native_list(shell_state *state)
{
    while (state->pending_list_active &&
           state->pending_list_next != GSH_AST_NONE) {
        size_t node_index = state->pending_list_next;
        const gsh_ast_node *node =
            &state->parse_storage->nodes[node_index];

        state->pending_list_next = node->next_sibling;
        if (!native_node_is_supported(state, node_index)) {
            output_text(state,
                        "gsh: native asynchronous continuation unsupported"
                        "\r\n");
            state->last_status = 125;
            state->pending_list_active = false;
            break;
        }
        if ((node->flags & GSH_AST_FLAG_ASYNC) != 0) {
            if (!start_background_node(state, node_index)) {
                state->last_status = 125;
            }
            continue;
        }
        if (native_list_node_is_wait(state, node_index)) {
            state->pending_and_or_active = true;
            state->pending_and_or_next = node->first_child;
            continue_native_and_or(state);
            return;
        }
        state->mode = MODE_DISPATCH;
        start_native_compound(state, node_index);
        return;
    }
    state->pending_list_active = false;
    state->pending_list_next = GSH_AST_NONE;
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

static size_t reactor_only_child(const shell_state *state, size_t node_index)
{
    size_t child = state->parse_storage->nodes[node_index].first_child;

    return child != GSH_AST_NONE &&
                   state->parse_storage->nodes[child].next_sibling ==
                       GSH_AST_NONE
               ? child
               : GSH_AST_NONE;
}

static bool reactor_pure_status(const shell_state *state, size_t node_index,
                                int *status)
{
    bool negated = false;
    const gsh_ast_node *node;

    for (;;) {
        size_t child;

        node = &state->parse_storage->nodes[node_index];
        if (node->kind != GSH_AST_PROGRAM && node->kind != GSH_AST_LIST &&
            node->kind != GSH_AST_AND_OR && node->kind != GSH_AST_PIPELINE) {
            break;
        }
        if (node->kind == GSH_AST_PIPELINE &&
            (node->flags & GSH_AST_FLAG_NEGATED) != 0) {
            negated = !negated;
        }
        child = reactor_only_child(state, node_index);
        if (child == GSH_AST_NONE) {
            return false;
        }
        node_index = child;
    }
    if (node->kind == GSH_AST_SIMPLE && node->word_count == 1U) {
        gsh_word_ref word =
            state->parse_storage->words[node->first_word];
        int value = reactor_word_is(state->pending_input, word, "false")
                        ? 1
                        : 0;

        *status = negated ? (value == 0 ? 1 : 0) : value;
        return true;
    }
    return false;
}

static bool reactor_reduce_for(shell_state *state, size_t node_index,
                               int *status)
{
    const gsh_ast_node *node;
    size_t child;
    gsh_word_ref name;
    gsh_word_ref value;

    for (;;) {
        node = &state->parse_storage->nodes[node_index];
        if (node->kind != GSH_AST_PROGRAM && node->kind != GSH_AST_LIST &&
            node->kind != GSH_AST_AND_OR && node->kind != GSH_AST_PIPELINE) {
            break;
        }
        if (node->kind == GSH_AST_PIPELINE &&
            (node->flags & GSH_AST_FLAG_NEGATED) != 0) {
            return false;
        }
        child = reactor_only_child(state, node_index);
        if (child == GSH_AST_NONE) {
            return false;
        }
        node_index = child;
    }
    if (node->kind != GSH_AST_FOR ||
        (node->flags & GSH_AST_FLAG_FOR_HAS_IN) == 0 ||
        !reactor_pure_status(state, node->first_child, status)) {
        return false;
    }
    if (node->word_count == 1U) {
        *status = 0;
        return true;
    }
    name = state->parse_storage->words[node->first_word];
    value = state->parse_storage->words[node->first_word +
                                        node->word_count - 1U];
    if (gsh_variables_set(state->variables,
                          state->pending_input + name.begin,
                          name.end - name.begin,
                          state->pending_input + value.begin,
                          value.end - value.begin,
                          assignment_attributes(&state->options),
                          assignment_attributes(&state->options)) == -1) {
        output_format(state, "gsh: for assignment: %s\r\n", strerror(errno));
        *status = 125;
    }
    return true;
}

static bool try_native_reactor_compound(shell_state *state)
{
    native_evaluator evaluator;
    size_t budget;
    bool contains_for;
    int status;

    if (fault_injection_active() ||
        !reactor_safe_node(state, 0, 0, &budget, &contains_for) ||
        !contains_for) {
        return false;
    }
    if (!reactor_reduce_for(state, 0, &status)) {
        initialize_interactive_evaluator(&evaluator, state,
                                         state->variables);
        status = native_evaluate_node(&evaluator, 0, 0);
    }
    if (budget > 1U) {
        state->variable_generation++;
    }
    state->last_status = status;
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return true;
}

static int write_variable_commit(
    int descriptor, gsh_variable_journal *journal,
    gsh_alias_journal *alias_journal,
    const gsh_positional_store *positionals,
    const gsh_shell_options *options,
    const gsh_function_store *functions,
    uint64_t function_generation)
{
    unsigned int part;

    if (fault_should_fail("state-commit-malformed", EPROTO)) {
        journal->version++;
    }
    if (positionals != NULL &&
        fault_should_fail("positional-commit-malformed", EPROTO)) {
        ((gsh_positional_store *)positionals)->version++;
    }
    if (alias_journal != NULL &&
        fault_should_fail("alias-commit-malformed", EPROTO)) {
        alias_journal->version++;
    }
    if (fault_should_fail("option-commit-malformed", EPROTO)) {
        ((gsh_shell_options *)options)->enabled |= 1U << 29;
    }
    for (part = 0; part < 4U; part++) {
        const unsigned char *cursor;
        size_t remaining;

        if (part == 0) {
            cursor = (const unsigned char *)journal;
            remaining = sizeof(*journal);
        } else if (part == 1) {
            if (alias_journal == NULL) {
                continue;
            }
            cursor = (const unsigned char *)alias_journal;
            remaining = sizeof(*alias_journal);
        } else if (part == 2) {
            if (positionals == NULL) {
                continue;
            }
            cursor = (const unsigned char *)positionals;
            remaining = sizeof(*positionals);
        } else {
            cursor = (const unsigned char *)options;
            remaining = sizeof(*options);
        }
        while (remaining > 0) {
            ssize_t written = fault_should_fail("state-commit-write", EIO)
                                  ? -1
                                  : write(descriptor, cursor, remaining);

            if (written > 0) {
                cursor += (size_t)written;
                remaining -= (size_t)written;
            } else if (written == -1 && errno == EINTR) {
                continue;
            } else {
                return -1;
            }
        }
    }
    if (functions != NULL) {
        gsh_function_snapshot_header header;
        size_t offset = 0;
        size_t total;
        const unsigned char *cursor;
        size_t remaining;

        gsh_functions_snapshot_header(functions, function_generation,
                                      &header);
        if (fault_should_fail("function-commit-malformed", EPROTO)) {
            header.reserved = 1;
        }
        cursor = (const unsigned char *)&header;
        remaining = sizeof(header);
        while (remaining > 0) {
            ssize_t written = fault_should_fail("state-commit-write", EIO)
                                  ? -1
                                  : write(descriptor, cursor, remaining);

            if (written > 0) {
                cursor += (size_t)written;
                remaining -= (size_t)written;
            } else if (written == -1 && errno == EINTR) {
                continue;
            } else {
                return -1;
            }
        }
        total = gsh_functions_snapshot_payload_size(&header);
        while (offset < total) {
            size_t available;
            const void *source = gsh_functions_snapshot_source(
                functions, &header, offset, &available);
            ssize_t written;

            if (source == NULL || available == 0) {
                errno = EPROTO;
                return -1;
            }
            written = fault_should_fail("function-commit-write", EIO)
                          ? -1
                          : write(descriptor, source, available);
            if (written > 0) {
                offset += (size_t)written;
            } else if (written == -1 && errno == EINTR) {
                continue;
            } else {
                return -1;
            }
        }
    }
    return 0;
}

static void abandon_pending_list(shell_state *state)
{
    state->pending_list_active = false;
    state->pending_list_next = GSH_AST_NONE;
    state->pending_and_or_active = false;
    state->pending_and_or_next = GSH_AST_NONE;
}

static void start_native_compound(shell_state *state, size_t node_index)
{
    int gate[2];
    int commit[2] = {-1, -1};
    int directory[2] = {-1, -1};
    managed_pty pty = {.master = -1, .slave_hold = -1};
    bool managed = state->async_repl != NULL && state->async_repl->enabled;
    sigset_t blocked;
    sigset_t previous;
    pid_t pid;

    if (state->current_job.active) {
        output_text(state,
                    "gsh: this MVP supports one job at a time; use fg or wait "
                    "for it\r\n");
        abandon_pending_list(state);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (state->pending_alias_commit &&
        ensure_alias_state(state, true) == -1) {
        output_format(state, "gsh: alias transaction allocation: %s\r\n",
                      strerror(errno));
        state->pending_alias_commit = false;
        abandon_pending_list(state);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (state->pending_function_commit &&
        ensure_function_state(state, true) == -1) {
        output_format(state, "gsh: function transaction allocation: %s\r\n",
                      strerror(errno));
        state->pending_function_commit = false;
        abandon_pending_list(state);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (make_pipe(gate, false, "evaluator-gate") == -1) {
        output_format(state, "gsh: evaluator gate: %s\r\n",
                      strerror(errno));
        abandon_pending_list(state);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (make_pipe(commit, false, "state-commit-pipe") == -1 ||
        set_fd_flags(commit[0], F_GETFL, O_NONBLOCK) == -1) {
        int saved_errno = errno;

        close(gate[0]);
        close(gate[1]);
        if (commit[0] >= 0) {
            close(commit[0]);
            close(commit[1]);
        }
        output_format(state, "gsh: state transaction pipe: %s\r\n",
                      strerror(saved_errno));
        abandon_pending_list(state);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    state->directory_commit_expected = state->pending_directory_commit;
    if (state->directory_commit_expected &&
        (fault_should_fail("directory-commit-socket", EMFILE) ||
         socketpair(AF_UNIX, SOCK_DGRAM, 0, directory) == -1 ||
         set_fd_flags(directory[0], F_GETFL, O_NONBLOCK) == -1 ||
         set_fd_flags(directory[0], F_GETFD, FD_CLOEXEC) == -1 ||
         set_fd_flags(directory[1], F_GETFD, FD_CLOEXEC) == -1)) {
        int saved_errno = errno;

        close(gate[0]);
        close(gate[1]);
        close(commit[0]);
        close(commit[1]);
        if (directory[0] >= 0) {
            close(directory[0]);
            close(directory[1]);
        }
        state->directory_commit_expected = false;
        state->pending_directory_commit = false;
        output_format(state, "gsh: directory transaction socket: %s\r\n",
                      strerror(saved_errno));
        abandon_pending_list(state);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        close(gate[0]);
        close(gate[1]);
        close(commit[0]);
        close(commit[1]);
        if (directory[0] >= 0) {
            close(directory[0]);
            close(directory[1]);
        }
        state->directory_commit_expected = false;
        state->pending_directory_commit = false;
        output_format(state, "gsh: sigprocmask: %s\r\n", strerror(errno));
        abandon_pending_list(state);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    gsh_variable_journal_initialize(state->variable_commit,
                                    state->variable_generation);
    state->alias_commit_expected = state->pending_alias_commit;
    if (state->alias_commit_expected) {
        gsh_alias_journal_initialize(state->alias_commit,
                                     state->alias_generation);
    }
    state->function_commit_expected = state->pending_function_commit;
    state->function_commit_header_complete = false;
    memset(&state->function_commit_header, 0,
           sizeof(state->function_commit_header));
    state->positional_commit_expected = state->pending_positional_commit;
    if (state->positional_commit_expected) {
        state->positional_commit =
            fault_should_fail("positional-commit-allocation", ENOMEM)
                ? NULL
                : malloc(sizeof(*state->positional_commit));
        if (state->positional_commit == NULL) {
            int saved_errno = errno;

            close(gate[0]);
            close(gate[1]);
            close(commit[0]);
            close(commit[1]);
            if (directory[0] >= 0) {
                close(directory[0]);
                close(directory[1]);
            }
            (void)sigprocmask(SIG_SETMASK, &previous, NULL);
            state->positional_commit_expected = false;
            state->pending_positional_commit = false;
            state->alias_commit_expected = false;
            state->pending_alias_commit = false;
            state->function_commit_expected = false;
            state->pending_function_commit = false;
            state->directory_commit_expected = false;
            state->pending_directory_commit = false;
            output_format(state, "gsh: positional transaction: %s\r\n",
                          strerror(saved_errno));
            abandon_pending_list(state);
            state->mode = MODE_EDITOR;
            queue_prompt(state);
            return;
        }
        if (state->positionals == NULL) {
            gsh_positionals_initialize(state->positional_commit);
        } else {
            memcpy(state->positional_commit, state->positionals,
                   sizeof(*state->positional_commit));
        }
    }
    pid = managed && open_managed_pty(&pty) == -1
              ? -1
              : (fault_should_fail("evaluator-fork", EAGAIN) ? -1
                                                               : fork());
    if (pid == 0) {
        char release;
        native_evaluator evaluator;
        gsh_background_table evaluator_backgrounds;
        int status;

        memset(&evaluator, 0, sizeof(evaluator));
        close(gate[1]);
        close(commit[0]);
        if (directory[0] >= 0) {
            close(directory[0]);
        }
        if (managed) {
            (void)close(pty.master);
            if (attach_child_pty(state, &pty) == -1) {
                child_exec_error("managed evaluator PTY", errno);
            }
        } else {
            (void)setpgid(0, 0);
        }
        reset_child_signals();
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        while (read(gate[0], &release, sizeof(release)) == -1 &&
               errno == EINTR) {
        }
        close(gate[0]);
        close_child_reactor_descriptors(state, -1);
        evaluator.input = state->pending_input;
        evaluator.input_length = state->pending_input_length;
        evaluator.storage = state->parse_storage;
        evaluator.pipeline = state->native_pipeline;
        evaluator.default_path = state->default_path;
        evaluator.last_status = state->last_status;
        evaluator.shell_pid = (long)state->shell_pgid;
        evaluator.last_background_pid = state->last_background_pid;
        evaluator.parameter_zero = state->parameter_zero;
        evaluator.positionals = state->positional_commit_expected
                                    ? state->positional_commit
                                    : state->positionals;
        evaluator.options = state->options;
        evaluator.variables = state->variables;
        evaluator.journal = state->variable_commit;
        evaluator.aliases = state->aliases;
        evaluator.alias_journal = state->alias_commit_expected
                                      ? state->alias_commit
                                      : NULL;
        evaluator.functions = state->functions;
        evaluator.function_scratch = state->function_scratch;
        evaluator.scope_base = state->pipeline_variables;
        evaluator.scope_changes = state->pipeline_changes;
        evaluator.pipeline_scope = NULL;
        evaluator.substitution_depth = 0;
        evaluator.preflight = false;
        evaluator.fatal_error = false;
        evaluator.static_for_items = false;
        evaluator.tail_exec_single = false;
        evaluator.positional_mutation_possible = false;
        evaluator.directory_mutation_possible = false;
        evaluator.alias_mutation_possible = false;
        evaluator.state_commit_invalid = false;
        gsh_background_initialize(&evaluator_backgrounds);
        evaluator.backgrounds = &evaluator_backgrounds;
        status = native_evaluate_node(&evaluator, node_index, 0);
        if (state->directory_commit_expected &&
            send_directory_descriptor(directory[1]) == -1) {
            status = 125;
        }
        if (directory[1] >= 0) {
            close(directory[1]);
        }
        if (evaluator.state_commit_invalid ||
            write_variable_commit(
                commit[1], state->variable_commit,
                state->alias_commit_expected ? state->alias_commit : NULL,
                state->positional_commit_expected
                    ? state->positional_commit
                    : NULL,
                &evaluator.options,
                state->function_commit_expected ? state->functions : NULL,
                state->function_generation) == -1) {
            status = 125;
        }
        close(commit[1]);
        _exit(status & 255);
    }
    close(gate[0]);
    close(commit[1]);
    if (directory[1] >= 0) {
        close(directory[1]);
    }
    if (pid == -1) {
        int saved_errno = errno;

        close(gate[1]);
        close(commit[0]);
        if (pty.master >= 0) {
            (void)close(pty.master);
        }
        if (pty.slave_hold >= 0) {
            (void)close(pty.slave_hold);
        }
        if (directory[0] >= 0) {
            close(directory[0]);
        }
        free(state->positional_commit);
        state->positional_commit = NULL;
        state->positional_commit_expected = false;
        state->pending_positional_commit = false;
        state->alias_commit_expected = false;
        state->pending_alias_commit = false;
        state->function_commit_expected = false;
        state->pending_function_commit = false;
        state->directory_commit_expected = false;
        state->pending_directory_commit = false;
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        output_format(state, "gsh: evaluator fork: %s\r\n",
                      strerror(saved_errno));
        abandon_pending_list(state);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    if (managed && pty.slave_hold >= 0) {
        (void)close(pty.slave_hold);
        pty.slave_hold = -1;
    }

    state->variable_commit_fd = commit[0];
    state->directory_commit_socket = directory[0];
    state->variable_commit_received = 0;
    state->variable_commit_active = true;
    state->variable_commit_eof = false;
    state->variable_commit_invalid = false;
    memset(state->variable_commit, 0, sizeof(*state->variable_commit));
    if (state->alias_commit_expected) {
        memset(state->alias_commit, 0, sizeof(*state->alias_commit));
    }
    if (state->function_commit_expected) {
        memset(&state->function_commit_header, 0,
               sizeof(state->function_commit_header));
        state->function_commit_header_complete = false;
    }
    state->option_commit.enabled = 0;

    initialize_job(&state->current_job, pid, pid, &pid, 1, true, false);
    state->current_job.modes = state->original_modes;
    if (managed) {
        state->current_job.foreground = false;
        state->current_job.silent = true;
        if (gsh_async_repl_attach(state->async_repl,
                                  state->async_dispatch_cell, pid, pid,
                                  pty.master) == -1) {
            int saved_errno = errno;

            (void)kill(pid, SIGKILL);
            close(gate[1]);
            close_variable_commit(state);
            (void)sigprocmask(SIG_SETMASK, &previous, NULL);
            output_format(state, "gsh: managed evaluator: %s\r\n",
                          strerror(saved_errno));
            gsh_async_repl_finish(state->async_repl,
                                  state->async_dispatch_cell, 125 << 8,
                                  false);
            state->current_job.active = false;
            state->mode = MODE_EDITOR;
            abandon_pending_list(state);
            return;
        }
        state->mode = MODE_EDITOR;
        close(gate[1]);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        queue_prompt(state);
        return;
    }
    (void)setpgid(pid, pid);
    if (fault_should_fail("terminal-handoff", EIO) ||
        tcsetattr(state->tty_fd, TCSANOW, &state->original_modes) == -1 ||
        tcsetpgrp(state->tty_fd, pid) == -1) {
        int saved_errno = errno;

        state->current_job.foreground = false;
        state->current_job.silent = true;
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        close(gate[1]);
        close_variable_commit(state);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        (void)enter_editor(state);
        output_format(state, "gsh: terminal handoff: %s\r\n",
                      strerror(saved_errno));
        abandon_pending_list(state);
        queue_prompt(state);
        return;
    }
    state->terminal_changed = false;
    state->mode = MODE_FOREGROUND;
    close(gate[1]);
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
}

static int execute_native_alias_script(
    const char *input, bool *handled, const char *parameter_zero,
    gsh_parse_storage *storage, gsh_native_pipeline *pipeline,
    gsh_variable_store *variables, gsh_variable_store *scratch,
    gsh_variable_store *scope_base, gsh_variable_journal *scope_changes,
    gsh_positional_store *positionals, const char *default_path)
{
    gsh_alias_store *aliases;
    gsh_function_store *functions = NULL;
    gsh_function_store *function_scratch = NULL;
    char *expanded;
    native_evaluator evaluator;
    gsh_background_table backgrounds;
    size_t input_length = strlen(input);
    size_t offset = 0;
    int status = 0;

    *handled = true;
    aliases = fault_should_fail("alias-allocation", ENOMEM)
                  ? NULL
                  : malloc(sizeof(*aliases));
    expanded = fault_should_fail("alias-allocation", ENOMEM)
                   ? NULL
                   : malloc(GSH_ALIAS_EXPANSION_CAP);
    if (aliases == NULL || expanded == NULL) {
        int saved_errno = errno;

        free(aliases);
        free(expanded);
        errno = saved_errno;
        perror("gsh: alias allocation");
        return 125;
    }
    gsh_aliases_initialize(aliases);
    memset(&evaluator, 0, sizeof(evaluator));
    evaluator.storage = storage;
    evaluator.pipeline = pipeline;
    evaluator.default_path = default_path;
    evaluator.shell_pid = (long)getpid();
    evaluator.parameter_zero = parameter_zero;
    evaluator.positionals = positionals;
    gsh_options_initialize(&evaluator.options, false);
    evaluator.aliases = aliases;
    evaluator.scope_base = scope_base;
    evaluator.scope_changes = scope_changes;
    gsh_background_initialize(&backgrounds);
    evaluator.backgrounds = &backgrounds;

    while (offset < input_length) {
        const char *parsed_input = input + offset;
        size_t parsed_length = 0;
        size_t end = offset;
        gsh_parse_result parsed;

        do {
            const char *newline = memchr(input + end, '\n',
                                         input_length - end);

            end = newline == NULL ? input_length
                                  : (size_t)(newline - input) + 1U;
            parsed = gsh_alias_parse(
                input + offset, end - offset, aliases, expanded,
                GSH_ALIAS_EXPANSION_CAP, storage, &parsed_input,
                &parsed_length);
        } while (parsed.status == GSH_PARSE_INCOMPLETE &&
                 end < input_length);
        if (parsed.status != GSH_PARSE_OK) {
            fprintf(stderr, "gsh: %s at byte %zu\n",
                    gsh_parse_status_name(parsed.status),
                    offset + parsed.error_offset);
            status = 2;
            break;
        }
        if (storage_has_function(storage) && functions == NULL) {
            functions = fault_should_fail("function-allocation", ENOMEM)
                            ? NULL
                            : malloc(sizeof(*functions));
            function_scratch =
                fault_should_fail("function-allocation", ENOMEM)
                    ? NULL
                    : malloc(sizeof(*function_scratch));
            if (functions == NULL || function_scratch == NULL) {
                perror("gsh: function allocation");
                status = 125;
                break;
            }
            gsh_functions_initialize(functions);
            gsh_functions_initialize(function_scratch);
        }

        memcpy(scratch, variables, sizeof(*scratch));
        evaluator.input = parsed_input;
        evaluator.input_length = parsed_length;
        evaluator.variables = scratch;
        evaluator.journal = NULL;
        evaluator.alias_journal = NULL;
        evaluator.functions = functions;
        evaluator.function_scratch = function_scratch;
        evaluator.pipeline_scope = NULL;
        evaluator.substitution_depth = 0;
        evaluator.preflight = true;
        evaluator.fatal_error = false;
        evaluator.static_for_items = false;
        evaluator.tail_exec_single = false;
        evaluator.positional_mutation_possible = false;
        evaluator.directory_mutation_possible = false;
        evaluator.alias_mutation_possible = false;
        evaluator.state_commit_invalid = false;
        if (!native_preflight_node(&evaluator, parsed.root, 0)) {
            fprintf(stderr, "gsh: native execution unsupported\n");
            status = 2;
            break;
        }
        evaluator.variables = variables;
        evaluator.preflight = false;
        evaluator.fatal_error = false;
        status = native_evaluate_node(&evaluator, parsed.root, 0);
        if (evaluator.fatal_error) {
            break;
        }
        offset = end;
    }
    free(aliases);
    free(expanded);
    free(functions);
    free(function_scratch);
    return status;
}

static int execute_native_noninteractive(
    const char *input, bool required, bool *handled,
    const char *parameter_zero, char *const *positional_parameters,
    size_t positional_count)
{
    char default_path[EXEC_PATH_CAP];
    gsh_parse_storage *storage = malloc(sizeof(*storage));
    gsh_native_pipeline *pipeline = malloc(sizeof(*pipeline));
    gsh_variable_store *variables = malloc(sizeof(*variables));
    gsh_variable_store *scratch = malloc(sizeof(*scratch));
    gsh_variable_store *scope_base = malloc(sizeof(*scope_base));
    gsh_variable_journal *scope_changes = malloc(sizeof(*scope_changes));
    gsh_positional_store *positionals = malloc(sizeof(*positionals));
    gsh_function_store *functions = NULL;
    gsh_function_store *function_scratch = NULL;
    gsh_parse_result parsed;
    native_evaluator evaluator;
    gsh_background_table backgrounds;
    int status;

    memset(&evaluator, 0, sizeof(evaluator));
    *handled = false;
    if (storage == NULL || pipeline == NULL || variables == NULL ||
        scratch == NULL || scope_base == NULL || scope_changes == NULL ||
        positionals == NULL ||
        gsh_variables_import(variables, environ) == -1 ||
        gsh_positionals_assign(positionals, positional_count,
                               positional_parameters) == -1) {
        free(storage);
        free(pipeline);
        free(variables);
        free(scratch);
        free(scope_base);
        free(scope_changes);
        free(positionals);
        perror("gsh: native allocation");
        *handled = true;
        return 125;
    }
    parsed = gsh_parse(input, strlen(input), storage);
    if (parsed.status != GSH_PARSE_OK) {
        if (required) {
            fprintf(stderr, "gsh: %s at byte %zu\n",
                    gsh_parse_status_name(parsed.status),
                    parsed.error_offset);
            *handled = true;
        }
        free(storage);
        free(pipeline);
        free(variables);
        free(scratch);
        free(scope_base);
        free(scope_changes);
        free(positionals);
        return 2;
    }
    {
        size_t size = confstr(_CS_PATH, default_path, sizeof(default_path));

        if (size == 0 || size > sizeof(default_path)) {
            memcpy(default_path, "/bin:/usr/bin", 14);
        }
    }
    if (input_contains_alias_builtin(input, strlen(input), storage)) {
        status = execute_native_alias_script(
            input, handled, parameter_zero, storage, pipeline, variables,
            scratch, scope_base, scope_changes, positionals, default_path);
        free(storage);
        free(pipeline);
        free(variables);
        free(scratch);
        free(scope_base);
        free(scope_changes);
        free(positionals);
        return status;
    }
    if (storage_has_function(storage)) {
        functions = fault_should_fail("function-allocation", ENOMEM)
                        ? NULL
                        : malloc(sizeof(*functions));
        function_scratch =
            fault_should_fail("function-allocation", ENOMEM)
                ? NULL
                : malloc(sizeof(*function_scratch));
        if (functions == NULL || function_scratch == NULL) {
            free(storage);
            free(pipeline);
            free(variables);
            free(scratch);
            free(scope_base);
            free(scope_changes);
            free(positionals);
            free(functions);
            free(function_scratch);
            perror("gsh: function allocation");
            *handled = true;
            return 125;
        }
        gsh_functions_initialize(functions);
        gsh_functions_initialize(function_scratch);
    }
    evaluator.input = input;
    evaluator.input_length = strlen(input);
    evaluator.storage = storage;
    evaluator.pipeline = pipeline;
    evaluator.default_path = default_path;
    evaluator.last_status = 0;
    evaluator.shell_pid = (long)getpid();
    evaluator.last_background_pid = 0;
    evaluator.parameter_zero = parameter_zero;
    evaluator.positionals = positionals;
    gsh_options_initialize(&evaluator.options, false);
    memcpy(scratch, variables, sizeof(*scratch));
    evaluator.variables = scratch;
    evaluator.journal = NULL;
    evaluator.aliases = NULL;
    evaluator.alias_journal = NULL;
    evaluator.functions = functions;
    evaluator.function_scratch = function_scratch;
    evaluator.scope_base = scope_base;
    evaluator.scope_changes = scope_changes;
    evaluator.pipeline_scope = NULL;
    evaluator.substitution_depth = 0;
    evaluator.preflight = true;
    evaluator.fatal_error = false;
    evaluator.static_for_items = false;
    evaluator.tail_exec_single = false;
    evaluator.positional_mutation_possible = false;
    evaluator.directory_mutation_possible = false;
    evaluator.alias_mutation_possible = false;
    evaluator.state_commit_invalid = false;
    gsh_background_initialize(&backgrounds);
    evaluator.backgrounds = &backgrounds;
    if (!native_preflight_node(&evaluator, parsed.root, 0)) {
        if (required) {
            fprintf(stderr, "gsh: native execution unsupported\n");
            *handled = true;
        }
        free(storage);
        free(pipeline);
        free(variables);
        free(scratch);
        free(scope_base);
        free(scope_changes);
        free(positionals);
        free(functions);
        free(function_scratch);
        return 2;
    }
    *handled = true;
    if (functions != NULL) {
        gsh_functions_initialize(functions);
        gsh_functions_initialize(function_scratch);
    }
    evaluator.variables = variables;
    evaluator.preflight = false;
    evaluator.fatal_error = false;
    status = native_evaluate_node(&evaluator, parsed.root, 0);
    free(storage);
    free(pipeline);
    free(variables);
    free(scratch);
    free(scope_base);
    free(scope_changes);
    free(positionals);
    free(functions);
    free(function_scratch);
    return status;
}

static int exec_noninteractive(int argc, char **argv)
{
    char *script_arguments[] = {(char *)"sh", NULL};

    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        bool handled;
        const char *parameter_zero = argc >= 4 ? argv[3] : argv[0];
        char **positionals = argc >= 5 ? argv + 4 : NULL;
        size_t positional_count = argc >= 5 ? (size_t)argc - 4U : 0;
        int status = execute_native_noninteractive(
            argv[2], false, &handled, parameter_zero, positionals,
            positional_count);

        if (handled) {
            return status;
        }
        argv[0] = (char *)"sh";
        execve("/bin/sh", argv, environ);
    } else if (argc == 1 && !isatty(STDIN_FILENO)) {
        execve("/bin/sh", script_arguments, environ);
    } else {
        print_usage(stderr);
        return 2;
    }
    perror("gsh: /bin/sh");
    return 127;
}

int main(int argc, char **argv)
{
    shell_state state;
    int status;

    (void)setlocale(LC_ALL, "");
    initialize_fault_injection();
    if (argc == 2 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout);
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "-n") == 0 &&
        strcmp(argv[2], "-c") == 0) {
        return check_native_syntax(argv[3]);
    }
    if (argc >= 4 && strcmp(argv[1], "--native-only") == 0 &&
        strcmp(argv[2], "-c") == 0) {
        bool handled;
        const char *parameter_zero = argc >= 5 ? argv[4] : argv[0];
        char **positionals = argc >= 6 ? argv + 5 : NULL;
        size_t positional_count = argc >= 6 ? (size_t)argc - 5U : 0;

        return execute_native_noninteractive(
            argv[3], true, &handled, parameter_zero, positionals,
            positional_count);
    }
    if (argc != 1 || !isatty(STDIN_FILENO)) {
        return exec_noninteractive(argc, argv);
    }
    if (initialize_interactive(&state) == -1) {
        perror("gsh: interactive initialization");
        cleanup(&state);
        return 1;
    }
    state.parameter_zero = argv[0];
    if (start_prompt_worker(&state) == -1) {
        state.prompt_worker_failures++;
    }

    status = run_reactor(&state);
    cleanup(&state);
    return status;
}
