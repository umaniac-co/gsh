#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <locale.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h> /* CANON-INCLUDE: macos */
#include <stdarg.h> /* CANON-INCLUDE: gcc */
#include <stdio.h> /* CANON-INCLUDE: linux */
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h> /* CANON-INCLUDE: linux */
#include <sys/stat.h>
#include <sys/wait.h> /* CANON-INCLUDE: linux */
#include <termios.h>
#include <time.h> /* CANON-INCLUDE: linux */
#include <unistd.h>
#include <wchar.h>

#include "builtin_cd.h"
#include "builtin_alias.h"
#include "builtin_command.h"
#include "builtin_fc.h"
#include "builtin_files.h"
#include "builtin_job_control.h"
#include "builtin_pure.h"
#include "builtin_stateful.h"
#include "builtin_times.h"
#include "builtin_trap.h"
#include "builtin_unalias.h"
#include "builtin_ulimit.h"
#include "builtin_umask.h"
#include "builtin_variables.h"
#include "builtin_set.h"
#include "builtin_shift.h"
#include "completion.h"
#include "fault_injection.h"
#include "history_file.h"
#include "llm_journal.h"
#include "resource_protocol.h"
#include "shell_invocation.h"
#include "source_workspace.h"

#define require(condition) (condition)

extern char **environ;

enum {
    LINE_CAP = 4096,
    OUTPUT_CAP = 65536,
    MAX_SIGNAL_REAPS = 16,
    MAX_INPUT_BYTES_PER_TURN = 1024,
    SIMPLE_ARG_CAP = 128,
    EXEC_PATH_CAP = 4096,
    PATH_SCAN_CAP = 32768,
    REDIRECTION_WORKER_PROTOCOL_VERSION = 1,
    NONINTERACTIVE_INPUT_FAST_CAP = GSH_SOURCE_INPUT_CAP,
    GSH_NATIVE_JOB_MEMBER_CAP =
        GSH_NATIVE_PIPELINE_CAP + GSH_NATIVE_HEREDOC_CAP,
    CHILD_ENVIRONMENT_CAP = GSH_VARIABLE_ENVIRONMENT_CAP,
    CHILD_WRITE_ATTEMPT_CAP = OUTPUT_CAP,
    EVALUATOR_WAIT_RETRY_CAP = 1024,
    AST_WALK_STEP_CAP = GSH_PARSE_NODE_CAP + 1,
    GSH_PROMPT_IDENTITY_CAP = 256,
};

_Static_assert((unsigned int)GSH_POSITIONAL_CAP ==
                   (unsigned int)GSH_NATIVE_ARGUMENT_CAP,
               "positional and native argument limits must match");
_Static_assert((unsigned int)GSH_POSITIONAL_TEXT_CAP ==
                   (unsigned int)GSH_NATIVE_TEXT_CAP,
               "positional and native text limits must match");
_Static_assert(sizeof(off_t) >= sizeof(int64_t),
               "descriptor-backed source offsets require 64-bit off_t");
_Static_assert(GSH_ASYNC_PROMPT_CAP >=
                   PATH_MAX + (2 * GSH_PROMPT_IDENTITY_CAP) + 128,
               "the prompt must hold PATH_MAX plus identity and SGR bytes");

static const char PROMPT_GREEN[] = "\033[38;5;114m";
static const char PROMPT_BLUE[] = "\033[38;5;75m";
static const char PROMPT_MUTED[] = "\033[38;5;245m";
static const char PROMPT_YELLOW[] = "\033[38;5;221m";
static const char PROMPT_RESET[] = "\033[0m";
static const uint64_t REACTOR_DEADLINE_NS = 5U * 1000U * 1000U;

typedef struct {
    char path[EXEC_PATH_CAP];
    dev_t device;
    ino_t inode;
} shell_executable_identity;

_Static_assert(sizeof(((shell_executable_identity *)0)->path) ==
                   EXEC_PATH_CAP,
               "shell executable identity must retain the full path cap");

static shell_executable_identity *shell_executable_storage(void)
{
    static shell_executable_identity identity;

    if (!require(identity.path[EXEC_PATH_CAP - 1U] == '\0')) return NULL;
    return &identity;
}

typedef enum {
    MODE_EDITOR,
    MODE_DISPATCH,
    MODE_FOREGROUND,
    MODE_ASYNC_REDIRECTION,
    MODE_WAIT,
} run_mode;

typedef enum {
    GSH_TERMINAL_IMAGE_NONE = GSH_RESOURCE_IMAGE_NONE,
    GSH_TERMINAL_IMAGE_KITTY = GSH_RESOURCE_IMAGE_KITTY,
    GSH_TERMINAL_IMAGE_ITERM = GSH_RESOURCE_IMAGE_ITERM,
    GSH_TERMINAL_IMAGE_SIXEL = GSH_RESOURCE_IMAGE_SIXEL,
} gsh_terminal_image_protocol;

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
    gsh_pipeline_status pipeline_status;
    struct termios modes;
} job;

typedef struct {
    char *argv[SIMPLE_ARG_CAP + 1];
    size_t argc;
} simple_command;

typedef struct {
    uint32_t version;
    uint64_t request_id;
    uint32_t operator_kind;
    uint32_t option_bits;
    uint32_t creation_mode;
    int32_t builtin_status;
    char directory[PATH_MAX];
} redirection_request;

typedef struct {
    uint32_t version;
    uint64_t request_id;
    int32_t status;
    int32_t error;
} redirection_result;

enum { GSH_COMMAND_CACHE_COMMIT_VERSION = 1 };

enum {
    GSH_STATE_CONTROL_COMMIT_VERSION = 1,
    GSH_STATE_CONTROL_EXIT = 1U,
};

enum {
    GSH_EXEC_DESCRIPTOR_COMMIT_VERSION = 1,
    GSH_EXEC_DESCRIPTOR_COMMIT_CAP = 128,
};

enum {
    GSH_JOB_SERVICE_VERSION = 1,
    GSH_JOB_SERVICE_JOBS = 1,
    GSH_JOB_SERVICE_KILL = 2,
    GSH_JOB_SERVICE_WAIT = 3,
    GSH_JOB_SERVICE_RIGHTS = 3,
    GSH_JOB_SERVICE_BATCH = 8,
};

typedef struct {
    uint32_t version;
    uint32_t type;
    uint32_t argc;
    uint32_t text_length;
    uint32_t offsets[GSH_NATIVE_ARGUMENT_CAP];
    char text[GSH_NATIVE_TEXT_CAP];
} job_service_request;

typedef struct {
    uint32_t version;
    int32_t status;
    int32_t error;
    uint32_t reserved;
} job_service_reply;

typedef struct {
    uint32_t version;
    uint32_t count;
    uint32_t open_count;
    uint32_t reserved;
    int32_t targets[GSH_EXEC_DESCRIPTOR_COMMIT_CAP];
    unsigned char open[GSH_EXEC_DESCRIPTOR_COMMIT_CAP];
} exec_descriptor_commit;

typedef struct {
    uint32_t version;
    uint32_t reserved;
    uint64_t base_generation;
    uint64_t final_path_generation;
} command_cache_commit_header;

typedef struct {
    uint32_t version;
    uint32_t flags;
    int32_t exit_status;
    uint32_t reserved;
} state_control_commit;

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
    size_t line_cursor;
    unsigned int escape_state;
    char editor_sequence[16];
    size_t editor_sequence_length;
    bool bracketed_paste;
    unsigned int paste_end_match;
    bool paste_last_was_cr;
    bool paste_overflow_reported;
    char mouse_sequence[64];
    size_t mouse_sequence_length;
    int focus_escape_cell;
    unsigned int focus_escape_state;
    uint64_t focus_escape_deadline_ns;
    uint64_t editor_escape_deadline_ns;
    uint64_t completion_deadline_ns;
    int completion_fd;
    pid_t completion_pid;
    uint64_t completion_next_request_id;
    uint64_t completion_active_request_id;
    uint64_t completion_variable_generation;
    uint64_t completion_alias_generation;
    uint64_t completion_function_generation;
    size_t completion_line_length;
    size_t completion_cursor;
    size_t completion_query_length;
    size_t completion_query_cursor;
    size_t completion_result_received;
    uint32_t completion_selection_index;
    gsh_completion_result completion_result;
    char completion_line[LINE_CAP];
    char completion_query_line[LINE_CAP];
    char completion_directory[PATH_MAX];
    bool completion_cycle_active;
    size_t completion_cycle_line_length;
    size_t completion_cycle_cursor;
    size_t completion_cycle_begin;
    size_t completion_cycle_end;
    uint32_t completion_cycle_next_index;
    uint32_t completion_cycle_candidate_count;
    char completion_cycle_line[LINE_CAP];
    size_t completion_menu_length;
    char completion_menu[GSH_COMPLETION_TEXT_CAP];
    gsh_history_store *history;
    gsh_history_store *session_history;
    gsh_history_file history_file;
    gsh_shell_config config;
    gsh_llm_journal_queue journal;
    pid_t journal_pid;
    bool journal_warning;
    bool current_job_llm;
    gsh_llm_repl_channel llm_repl;
    int llm_repl_peer;
    int llm_owner_cell;
    int llm_command_cell;
    bool auto_help_eligible;
    bool auto_help_pending;
    gsh_terminal_image_protocol image_protocol;
    bool config_error;
    bool history_persistent;
    bool history_navigation;
    size_t history_position;
    char history_draft[LINE_CAP];
    size_t history_draft_length;
    bool history_search;
    char history_search_query[LINE_CAP];
    size_t history_search_query_length;
    size_t history_search_position;
    char history_search_draft[LINE_CAP];
    size_t history_search_draft_length;
    bool classic_redraw_pending;
    bool classic_clear_pending;
    size_t classic_cursor_row;
    size_t classic_cursor_column;
    char pending_line[LINE_CAP];
    size_t pending_len;
    const char *pending_input;
    size_t pending_input_length;
    bool pending_alias_expanded;
    bool continuation_prompt;
    char default_path[EXEC_PATH_CAP];
    const char *parameter_zero;
    char current_directory[PATH_MAX];
    char prompt_user[GSH_PROMPT_IDENTITY_CAP];
    char prompt_host[GSH_PROMPT_IDENTITY_CAP];

    char output[OUTPUT_CAP];
    size_t output_offset;
    size_t output_len;
    gsh_async_repl *async_repl;
    int async_capture_cell;
    int async_state_cell;
    int async_dispatch_cell;
    bool async_desired;
    bool async_transition_pending;

    job current_job;
    gsh_background_table background_jobs;
    int job_service_socket;
    bool pending_job_service;
    int job_service_wait_reply_fd;
    pid_t job_service_wait_targets[GSH_BACKGROUND_CAP];
    size_t job_service_wait_target_count;
    bool job_service_wait_all;
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

    int redirection_worker_fd;
    pid_t redirection_worker_pid;
    bool redirection_worker_alive;
    bool redirection_worker_busy;
    bool redirection_worker_restart_pending;
    uint64_t redirection_next_request_id;
    uint64_t redirection_active_request_id;
    bool redirection_pipeline_negated;
    char redirection_target[PATH_MAX];
    gsh_parse_storage *parse_storage;
    gsh_parse_result pending_parse;
    gsh_native_pipeline *native_pipeline;
    gsh_command_cache *command_cache;
    gsh_command_cache *command_cache_scratch;
    gsh_variable_store *variables;
    gsh_variable_store *variable_scratch;
    gsh_variable_store *pipeline_variables;
    gsh_variable_journal *variable_commit;
    gsh_variable_journal *pipeline_changes;
    gsh_source_workspace_stack *source_workspaces;
    char *alias_expansion;
    gsh_alias_store *aliases;
    gsh_alias_store *alias_scratch;
    gsh_alias_journal *alias_commit;
    gsh_function_store *functions;
    gsh_function_store *function_scratch;
    gsh_function_snapshot_header function_commit_header;
    gsh_positional_store *positionals;
    gsh_positional_store *positional_storage;
    gsh_positional_store *positional_commit;
    gsh_shell_options option_commit;
    state_control_commit control_commit;
    bool committed_exit_requested;
    int committed_exit_status;
    bool positional_commit_expected;
    bool pending_positional_commit;
    bool alias_commit_expected;
    bool pending_alias_commit;
    bool function_commit_expected;
    bool pending_function_commit;
    bool function_commit_header_complete;
    command_cache_commit_header command_cache_commit_header;
    uint64_t command_cache_generation;
    bool pending_command_cache_commit;
    bool command_cache_commit_expected;
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
    int exec_outcome_fd;
    bool pending_exec_possible;
    int exec_descriptor_socket;
    int pending_exec_descriptors[GSH_EXEC_DESCRIPTOR_COMMIT_CAP];
    size_t pending_exec_descriptor_count;
    int pending_exec_protected_descriptors[
        GSH_EXEC_DESCRIPTOR_COMMIT_CAP];
    size_t pending_exec_protected_descriptor_count;
    bool exec_standard_descriptor_changed[3];

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
    uint64_t protected_bridge_dispatches;
    uint64_t parsed_dispatches;
    uint64_t parse_failures;
    uint64_t redirection_worker_failures;
} shell_state;

typedef struct {
    size_t row;
    size_t column;
} classic_editor_position;

static gsh_async_repl *state_async_repl(const shell_state *state)
{
    if (!require(state != NULL)) return NULL;
    if (!require(state->async_repl != NULL)) return NULL;
    return state->async_repl;
}

static gsh_parse_storage *state_parse_storage(const shell_state *state)
{
    if (!require(state != NULL)) return NULL;
    if (!require(state->parse_storage != NULL)) return NULL;
    return state->parse_storage;
}

static gsh_source_workspace_stack *state_source_workspaces(
    const shell_state *state)
{
    if (!require(state != NULL)) return NULL;
    if (!require(state->source_workspaces != NULL)) return NULL;
    return state->source_workspaces;
}

static gsh_native_pipeline *state_native_pipeline(const shell_state *state)
{
    if (!require(state != NULL)) return NULL;
    if (!require(state->native_pipeline != NULL)) return NULL;
    return state->native_pipeline;
}

static gsh_variable_store *state_variables(const shell_state *state)
{
    if (!require(state != NULL)) return NULL;
    if (!require(state->variables != NULL)) return NULL;
    return state->variables;
}

static gsh_history_store *state_history(const shell_state *state)
{
    if (!require(state != NULL)) return NULL;
    if (!require(state->history != NULL)) return NULL;
    return state->history;
}

static gsh_variable_journal *state_variable_commit(const shell_state *state)
{
    if (!require(state != NULL)) return NULL;
    if (!require(state->variable_commit != NULL)) return NULL;
    return state->variable_commit;
}

static gsh_alias_journal *state_alias_commit(const shell_state *state)
{
    if (!require(state != NULL)) return NULL;
    if (!require(state->alias_commit != NULL)) return NULL;
    return state->alias_commit;
}

typedef struct {
    gsh_history_store history;
    gsh_history_store session_history;
    gsh_async_repl async_repl;
    gsh_parse_storage parse_storage;
    gsh_native_pipeline native_pipeline;
    gsh_command_cache command_cache;
    gsh_command_cache command_cache_scratch;
    gsh_variable_store variables;
    gsh_variable_store variable_scratch;
    gsh_variable_store pipeline_variables;
    gsh_variable_journal variable_commit;
    gsh_variable_journal pipeline_changes;
    gsh_source_workspace_stack source_workspaces;
    gsh_positional_store positionals;
    gsh_positional_store positional_commit;
} interactive_storage;

static volatile sig_atomic_t g_signal_write_fd = -1;
static volatile sig_atomic_t g_sigchld_pending = 0;
static volatile sig_atomic_t g_sigint_pending = 0;
static volatile sig_atomic_t g_sigtstp_pending = 0;
static volatile sig_atomic_t g_sigwinch_pending = 0;
static volatile sig_atomic_t g_shutdown_pending = 0;

typedef struct pipeline_expansion_scope pipeline_expansion_scope;

static void reset_child_signals(void);
static void cancel_completion_request(shell_state *state, bool terminate);
static void receive_completion_result(shell_state *state);
static void start_external(shell_state *state, simple_command *direct);
static void start_async_external(shell_state *state, simple_command *direct);
static void start_async_llm(shell_state *state, int cell_index);
static void close_worker_child_descriptors(shell_state *state, int retained);
static void finish_journal_worker(shell_state *state);
static void service_llm_repl(shell_state *state);
static bool llm_command_context_current(const shell_state *state);
static void start_classic_llm(shell_state *state, const char *prompt_text,
                              size_t prompt_length);
static void start_async_native_pipeline(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const pipeline_expansion_scope *scope);
static int create_resource_socket(int descriptors[2]);
static bool command_uses_persistent_path(
    const gsh_native_command *command);
static uint64_t command_cache_path_generation(
    const gsh_variable_store *variables,
    const gsh_native_command *command);
static const char *hash_command_path_value(
    const gsh_variable_store *variables,
    const gsh_native_command *command, const char *default_path);
static uint64_t hash_command_path_generation(
    const gsh_variable_store *variables,
    const gsh_native_command *command);
static bool cache_planned_external(
    gsh_command_cache *cache, const gsh_variable_store *variables,
    const gsh_native_command *command, const char *default_path,
    const gsh_function_store *functions);
static bool native_command_is_supported(shell_state *state);
static bool try_native_reactor_compound(shell_state *state);
static void start_native_compound(shell_state *state, size_t node_index);
static void continue_native_list(shell_state *state);
static void continue_native_and_or(shell_state *state);
static bool finish_job_service_wait(shell_state *state);
static bool begin_native_list(shell_state *state);
static bool native_list_node_is_wait(const shell_state *state,
                                     size_t node_index);
static void abandon_pending_list(shell_state *state);
static void queue_redraw(shell_state *state);
static size_t active_prompt_text(shell_state *state,
                                 char prompt[GSH_ASYNC_PROMPT_CAP]);
static void advance_classic_position(const char *text, size_t length,
                                     size_t columns,
                                     classic_editor_position *position);
static bool async_transition_can_start_now(const shell_state *state);
static void leave_managed_fullscreen(shell_state *state, int cell_index);
static void handle_mouse_event(shell_state *state, unsigned char final);
static const char *store_path_value(const gsh_variable_store *variables,
                                    const char *default_path);
static int open_redirect_path(const char *target,
                              gsh_token_kind operator_kind,
                              const gsh_shell_options *options,
                              mode_t creation_mode);
static bool literal_command_word_is(const char *input, gsh_word_ref word,
                                    const char *text);
static bool fallback_mentions_protected_builtin(const char *input,
                                                size_t length);
static bool reactor_literal_word(const char *input, gsh_word_ref word);
static int apply_native_assignments(gsh_variable_store *variables,
                                    gsh_variable_journal *journal,
                                    const gsh_native_command *command,
                                    const gsh_shell_options *options);

static void configure_expansion_assignment_fault(
    gsh_native_variable_state *state)
{
    if (!require(state != NULL)) return;
    state->assignment_fault_enabled =
        gsh_fault_selected(GSH_FAULT_EXPANSION_ASSIGNMENT);
    state->assignment_fault_trigger = gsh_fault_trigger();
    state->assignment_fault_calls = gsh_fault_counter();
}

static int ensure_alias_state(shell_state *state, bool transaction)
{
    if (state == NULL) return -1;
    if (gsh_fault_should_fail(GSH_FAULT_ALIAS_ALLOCATION, ENOMEM) ||
        state->aliases == NULL || state->alias_expansion == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (!transaction) {
        return 0;
    }
    if (gsh_fault_should_fail(GSH_FAULT_ALIAS_TRANSACTION_ALLOCATION, ENOMEM) ||
        state->alias_scratch == NULL || state->alias_commit == NULL) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static int ensure_function_state(shell_state *state, bool scratch)
{
    if (state == NULL) return -1;
    if (gsh_fault_should_fail(GSH_FAULT_FUNCTION_ALLOCATION, ENOMEM) ||
        state->functions == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (scratch &&
        (gsh_fault_should_fail(GSH_FAULT_FUNCTION_COMPACT_ALLOCATION, ENOMEM) ||
         state->function_scratch == NULL)) {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static void reset_pending_input(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    state->pending_input = state->pending_line;
    state->pending_input_length = strlen(state->pending_line);
    state->pending_alias_expanded = false;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (gsh_fault_should_fail(GSH_FAULT_TIME_SOURCE_FAILURE, EIO) ||
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
                     gsh_fault_point fault_point)
{
    if (gsh_fault_should_fail(fault_point, EMFILE) ||
        pipe(descriptors) == -1) {
        return -1;
    }
    if (set_fd_flags(descriptors[0], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(descriptors[1], F_GETFD, FD_CLOEXEC) == -1 ||
        (nonblocking &&
         (set_fd_flags(descriptors[0], F_GETFL, O_NONBLOCK) == -1 ||
          set_fd_flags(descriptors[1], F_GETFL, O_NONBLOCK) == -1))) {
        int saved_errno = errno;
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static bool raw_output_push(shell_state *state, const char *data,
                            size_t length)
{
    if (state == NULL) return false;
    if (data == NULL) {
        return false;
    }
    if (length > OUTPUT_CAP - state->output_len) {
        state->overloads++;
        return false;
    }

    if (state->output_offset + state->output_len + length > OUTPUT_CAP) {
        (void)memmove(state->output, state->output + state->output_offset,
                state->output_len);
        state->output_offset = 0;
    }
    (void)memcpy(state->output + state->output_offset + state->output_len, data,
           length);
    state->output_len += length;
    return true;
}

static bool output_push(shell_state *state, const char *data, size_t length)
{
    if (state == NULL) return false;
    if (data == NULL) {
        return false;
    }
    if (state->async_repl != NULL && state_async_repl(state)->enabled &&
        state->async_capture_cell >= 0) {
        return gsh_async_repl_append(state->async_repl,
                                     state->async_capture_cell, data,
                                     length) >= 0;
    }
    return raw_output_push(state, data, length);
}

static gsh_builtin_io reactor_builtin_sink(shell_state *state)
{
    if (state == NULL) return (gsh_builtin_io){0};
    gsh_builtin_io io;

    (void)memset(&io, 0, sizeof(io));
    io.kind = GSH_BUILTIN_SINK_BUFFER;
    if (state->async_repl != NULL && state_async_repl(state)->enabled &&
        state->async_capture_cell >= 0) {
        io.buffer.async_repl = state->async_repl;
        io.buffer.async_cell = state->async_capture_cell;
    } else {
        io.buffer.bytes = state->output;
        io.buffer.capacity = sizeof(state->output);
        io.buffer.offset = &state->output_offset;
        io.buffer.length = &state->output_len;
        io.buffer.overloads = &state->overloads;
    }
    return io;
}

static bool output_text(shell_state *state, const char *text)
{
    if (state == NULL || text == NULL) {
        return false;
    }
    return output_push(state, text, strlen(text));
}

static void output_format(shell_state *state, const char *format, ...)
{
    if (format == NULL || state == NULL) {
        return;
    }
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
    if (state == NULL) {
        return;
    }
    unsigned int writes = 0;

    while (state->output_len > 0 && writes < 4) {
        size_t chunk = state->output_len;
        ssize_t written;

        if (chunk > 4096) {
            chunk = 4096;
        }
        written = gsh_fault_should_fail(GSH_FAULT_OUTPUT_WRITE, EIO)
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

/* ── History Is Ready Before the Reactor Starts ────────────────
 * Startup loads the bounded text file before the reactor begins, just as
 * traditional interactive shells do. A malformed or inaccessible file
 * degrades to in-process session history without delaying the editor.
 * ─────────────────────────────────────────────────────────────── */
static void initialize_history(shell_state *state)
{
    if (!require(state != NULL)) return;
    if (!require(state->history != NULL)) return;

    if (state->config_error) {
        (void)output_format(state, "gsh: %s; history disabled\r\n",
                            state->config.diagnostic);
        return;
    }
    if (!state->config.history_enabled) return;
    if (gsh_history_file_initialize(&state->history_file, getenv("HOME"),
                                    state->history) == -1) {
        (void)output_text(
            state,
            "gsh: persistent history unavailable; using session history\r\n");
        return;
    }
    state->history_persistent = true;
}

static void sanitize_prompt_component(char *text, size_t capacity)
{
    size_t index;

    if (text == NULL || capacity == 0U) return;
    for (index = 0U; index < capacity; index++) {
        unsigned char byte = (unsigned char)text[index];

        if (byte == '\0') return;
        if (byte < 0x20U || byte == 0x7fU) text[index] = '?';
    }
    text[capacity - 1U] = '\0';
}

static bool prompt_home_suffix(const shell_state *state,
                               const char **suffix)
{
    const char *home;
    size_t home_length;
    bool found;

    if (state == NULL || suffix == NULL) return false;
    home = gsh_variables_lookup(state->variables, "HOME", 4U, &found);
    home_length = found ? strlen(home) : 0U;
    while (home_length > 1U && home[home_length - 1U] == '/') home_length--;
    if (!found || home_length == 0U || home[0] != '/' ||
        strncmp(state->current_directory, home, home_length) != 0 ||
        (home_length != 1U && state->current_directory[home_length] != '\0' &&
         state->current_directory[home_length] != '/')) return false;
    if (home_length == 1U && state->current_directory[1] != '\0')
        *suffix = state->current_directory;
    else
        *suffix = state->current_directory + home_length;
    return true;
}

static bool prompt_directory_text(const shell_state *state, char *directory,
                                  size_t capacity)
{
    const char *source;
    const char *suffix;
    size_t length;

    if (state == NULL || directory == NULL || capacity == 0U) return false;
    if (prompt_home_suffix(state, &suffix)) {
        length = strlen(suffix);
        if (length + 2U > capacity) return false;
        directory[0] = '~';
        (void)memcpy(directory + 1U, suffix, length + 1U);
    } else {
        source = state->current_directory[0] == '\0'
                     ? "?" : state->current_directory;
        length = strlen(source);
        if (length + 1U > capacity) return false;
        (void)memcpy(directory, source, length + 1U);
    }
    sanitize_prompt_component(directory, capacity);
    return true;
}

static size_t primary_prompt_text(const shell_state *state, char *prompt,
                                  size_t capacity, bool include_async_state)
{
    char directory[PATH_MAX + 2U];
    bool busy;
    int written;

    if (state == NULL || prompt == NULL || capacity == 0U ||
        !prompt_directory_text(state, directory, sizeof(directory))) return 0U;
    busy = include_async_state &&
           !gsh_async_repl_prompt_settled(state->async_repl);
    written = busy
                  ? snprintf(prompt, capacity, "%s%s@%s%s %s%s%s %sgsh%s*%s> ",
                             PROMPT_GREEN, state->prompt_user,
                             state->prompt_host, PROMPT_RESET, PROMPT_BLUE,
                             directory, PROMPT_RESET, PROMPT_MUTED,
                             PROMPT_YELLOW, PROMPT_RESET)
                  : snprintf(prompt, capacity, "%s%s@%s%s %s%s%s %sgsh$%s> ",
                             PROMPT_GREEN, state->prompt_user,
                             state->prompt_host, PROMPT_RESET, PROMPT_BLUE,
                             directory, PROMPT_RESET, PROMPT_MUTED,
                             PROMPT_RESET);
    return written > 0 && (size_t)written < capacity ? (size_t)written : 0U;
}

/* ── Deferred Job Notices Precede The Next Prompt ────────────────
 * Without `set -b`, POSIX lets a completed background job remain quiet only
 * until the next prompt. The reactor retains that state in the bounded job
 * table, then reports it here without polling, allocation, or command-path
 * lookup. Enabling notify moves the same emission to the SIGCHLD path.
 * ────────────────────────────────────────────────────────── */
static void emit_job_notification(shell_state *state,
                                  const gsh_background_entry *entry)
{
    if (state == NULL || entry == NULL || entry->state != GSH_JOB_DONE ||
        entry->notified) {
        return;
    }
    output_format(state, "\r\n[%u]%c Done ", entry->job_id,
                  gsh_background_marker(&state->background_jobs,
                                        entry->job_id));
    (void)output_push(state,
                      entry->command_length == 0 ? "(command)"
                                                 : entry->command,
                      entry->command_length == 0 ? sizeof("(command)") - 1U
                                                 : entry->command_length);
    (void)output_text(state, "\r\n");
    (void)gsh_background_mark_notified(&state->background_jobs,
                                       entry->job_id);
}

static void emit_deferred_job_notifications(shell_state *state)
{
    size_t index;

    if (state == NULL || state->background_jobs.used > GSH_BACKGROUND_CAP) {
        return;
    }
    for (index = 0; index < state->background_jobs.used; index++) {
        emit_job_notification(state, &state->background_jobs.entries[index]);
    }
}

static bool defer_classic_auto_help(shell_state *state)
{
    if (state == NULL || !state->auto_help_eligible) return false;
    state->auto_help_eligible = false;
    if (!state->config.llm_enabled || !state->config.llm_auto_help ||
        state->last_status == 0 || state->mode != MODE_EDITOR ||
        state->pending_line[0] == '\0') return false;
    state->auto_help_pending = true;
    return true;
}

static void queue_prompt(shell_state *state)
{
    char prompt[GSH_ASYNC_PROMPT_CAP];
    classic_editor_position position = {0U, 0U};
    size_t length;
    size_t columns;

    if (state == NULL) return;
    if (state->async_repl != NULL && state_async_repl(state)->enabled) {
        state_async_repl(state)->render_pending = true;
        return;
    }
    if (defer_classic_auto_help(state)) return;
    emit_deferred_job_notifications(state);
    (void)output_text(state, "\033[?2004h");
    state->classic_redraw_pending = false;
    state->classic_clear_pending = false;
    length = active_prompt_text(state, prompt);
    (void)output_push(state, prompt, length);
    columns = state->async_repl == NULL
                  ? 80U : state_async_repl(state)->terminal_columns;
    if (columns == 0U) columns = 1U;
    advance_classic_position(prompt, length, columns, &position);
    state->classic_cursor_row = position.row;
    state->classic_cursor_column = position.column;
}

static void queue_redraw(shell_state *state)
{
    if (state == NULL) return;
    if (state->async_repl != NULL && state_async_repl(state)->enabled) {
        state_async_repl(state)->render_pending = true;
        return;
    }
    state->classic_redraw_pending = true;
}

static void queue_clear_redraw(shell_state *state)
{
    if (state == NULL) return;
    if (state->async_repl != NULL && state_async_repl(state)->enabled) {
        state_async_repl(state)->render_pending = true;
        return;
    }
    state->classic_clear_pending = true;
    state->classic_redraw_pending = true;
}

static size_t classic_sgr_length(const char *text, size_t length,
                                 size_t offset)
{
    size_t index;
    size_t turn;

    if (text == NULL || offset + 2U >= length || text[offset] != '\033' ||
        text[offset + 1U] != '[') return 0U;
    index = offset + 2U;
    for (turn = 0U; turn < 32U && index < length; turn++) {
        unsigned char byte = (unsigned char)text[index++];

        if (byte == 'm') return index - offset;
        if (!((byte >= '0' && byte <= '9') || byte == ';')) return 0U;
    }
    return 0U;
}

static size_t classic_character_width(const char *text, size_t length,
                                      size_t offset, size_t column,
                                      size_t *bytes)
{
    mbstate_t conversion;
    wchar_t character;
    size_t converted;
    int width;

    if (text == NULL || bytes == NULL || offset >= length) return 0U;
    if (text[offset] == '\t') {
        *bytes = 1U;
        return 8U - column % 8U;
    }
    (void)memset(&conversion, 0, sizeof(conversion));
    converted = mbrtowc(&character, text + offset, length - offset,
                        &conversion);
    if (converted == (size_t)-1 || converted == (size_t)-2 ||
        converted == 0U) {
        *bytes = 1U;
        return 1U;
    }
    *bytes = converted;
    width = wcwidth(character);
    return width < 0 ? 1U : (size_t)width;
}

static void advance_classic_position(const char *text, size_t length,
                                     size_t columns,
                                     classic_editor_position *position)
{
    size_t offset = 0U;

    if (text == NULL || position == NULL || columns == 0U) return;
    while (offset < length) {
        size_t bytes;
        size_t width;
        size_t sgr = classic_sgr_length(text, length, offset);

        if (sgr != 0U) { offset += sgr; continue; }
        if (text[offset] == '\r') {
            position->column = 0U;
            offset++;
            continue;
        }
        if (text[offset] == '\n') {
            position->row++;
            position->column = 0U;
            offset++;
            continue;
        }
        width = classic_character_width(text, length, offset,
                                        position->column, &bytes);
        if (width > columns) width = columns;
        if (position->column != 0U &&
            position->column + width > columns) {
            position->row++;
            position->column = 0U;
        } else if (position->column >= columns) {
            position->row++;
            position->column = 0U;
        }
        position->column += width;
        offset += bytes;
    }
}

static classic_editor_position append_classic_completion_menu(
    shell_state *state, classic_editor_position end, size_t columns)
{
    static const char newline[] = "\r\n";

    if (state == NULL || columns == 0U ||
        state->completion_menu_length == 0U) return end;
    (void)output_push(state, newline, sizeof(newline) - 1U);
    (void)output_push(state, state->completion_menu,
                      state->completion_menu_length);
    advance_classic_position(newline, sizeof(newline) - 1U, columns, &end);
    advance_classic_position(state->completion_menu,
                             state->completion_menu_length, columns, &end);
    return end;
}

static classic_editor_position classic_editor_position_at(
    shell_state *state, size_t editor_offset)
{
    classic_editor_position position = {0U, 0U};
    char prompt[GSH_ASYNC_PROMPT_CAP];
    size_t columns;
    size_t prompt_length;

    if (state == NULL || editor_offset > state->line_len) return position;
    columns = state->async_repl == NULL
                  ? 80U : state_async_repl(state)->terminal_columns;
    if (columns == 0U) columns = 1U;
    prompt_length = active_prompt_text(state, prompt);
    advance_classic_position(prompt, prompt_length, columns, &position);
    advance_classic_position(state->line, editor_offset, columns, &position);
    return position;
}

static void output_classic_vertical(shell_state *state, size_t rows,
                                    unsigned char direction)
{
    char sequence[32];
    int length;

    if (state == NULL || rows == 0U) return;
    length = snprintf(sequence, sizeof(sequence), "\033[%zu%c", rows,
                      direction);
    if (length > 0 && (size_t)length < sizeof(sequence))
        (void)output_push(state, sequence, (size_t)length);
}

static void position_classic_cursor(shell_state *state,
                                    classic_editor_position target)
{
    size_t column;

    if (state == NULL || state->async_repl == NULL ||
        state_async_repl(state)->enabled) return;
    column = target.column;
    if (column >= state_async_repl(state)->terminal_columns && column != 0U)
        column = state_async_repl(state)->terminal_columns - 1U;
    if (state->classic_cursor_row == target.row &&
        state->classic_cursor_column == column) return;
    (void)output_text(state, "\r");
    if (state->classic_cursor_row > target.row) {
        output_classic_vertical(state,
                                state->classic_cursor_row - target.row, 'A');
    } else if (target.row > state->classic_cursor_row) {
        output_classic_vertical(state,
                                target.row - state->classic_cursor_row, 'B');
    }
    if (column != 0U) output_classic_vertical(state, column, 'C');
    state->classic_cursor_row = target.row;
    state->classic_cursor_column = column;
}

static void finish_classic_editor(shell_state *state)
{
    if (state == NULL || state->async_repl == NULL ||
        state_async_repl(state)->enabled) return;
    position_classic_cursor(
        state, classic_editor_position_at(state, state->line_len));
}

/* ── Classic Redraws Are Latest-State Frames ─────────────────────
 * Editing controls can request hundreds of redraws in one ready input burst.
 * Materializing every intermediate frame wastes work and can crowd the final
 * Ctrl-C acknowledgement out of the bounded output queue. The reactor keeps
 * only the newest editor state and emits one frame before its bounded flush.
 * A pending clear is folded into that same frame.
 * ─────────────────────────────────────────────────────────────── */
static void prepare_classic_redraw(shell_state *state)
{
    if (state == NULL) return;
    char prompt[GSH_ASYNC_PROMPT_CAP];
    classic_editor_position cursor;
    classic_editor_position end;
    size_t columns;
    size_t prompt_length;
    bool clear;

    if (!state->classic_redraw_pending ||
        (state->async_repl != NULL && state_async_repl(state)->enabled)) {
        return;
    }
    clear = state->classic_clear_pending;
    state->classic_redraw_pending = false;
    state->classic_clear_pending = false;
    if (clear) {
        (void)output_text(state, "\033[2J\033[H");
        state->classic_cursor_row = 0U;
        state->classic_cursor_column = 0U;
    } else {
        (void)output_text(state, "\r");
        output_classic_vertical(state, state->classic_cursor_row, 'A');
        (void)output_text(state, "\033[2K\033[J");
        state->classic_cursor_row = 0U;
        state->classic_cursor_column = 0U;
    }
    prompt_length = active_prompt_text(state, prompt);
    (void)output_text(state, "\033[?2004h");
    (void)output_push(state, prompt, prompt_length);
    (void)output_push(state, state->line, state->line_len);
    end = classic_editor_position_at(state, state->line_len);
    columns = state->async_repl == NULL
                  ? 80U : state_async_repl(state)->terminal_columns;
    if (columns == 0U) columns = 1U;
    end = append_classic_completion_menu(state, end, columns);
    state->classic_cursor_row = end.row;
    state->classic_cursor_column = end.column;
    cursor = classic_editor_position_at(state, state->line_cursor);
    position_classic_cursor(state, cursor);
}

static size_t active_prompt_text(shell_state *state,
                                 char prompt[GSH_ASYNC_PROMPT_CAP])
{
    if (state == NULL) return 0U;
    if (prompt == NULL) {
        return 0U;
    }
    size_t length = 0;
    const char *secondary;

    if (state->history_search) {
        int written = snprintf(prompt, GSH_ASYNC_PROMPT_CAP,
                               "(reverse-i-search)`%.*s': ", 96,
                               state->history_search_query);

        length = written > 0 && written < GSH_ASYNC_PROMPT_CAP
                     ? (size_t)written
                     : 0;
    } else if (state->continuation_prompt) {
        secondary = getenv("PS2");
        if (secondary == NULL) {
            secondary = "> ";
        }
        while (length < GSH_ASYNC_PROMPT_CAP && secondary[length] != '\0')
            length++;
        if (length == GSH_ASYNC_PROMPT_CAP) {
            secondary = "> ";
            length = 2;
        }
        (void)memcpy(prompt, secondary, length);
    } else {
        length = primary_prompt_text(state, prompt, GSH_ASYNC_PROMPT_CAP,
                                     state->async_repl != NULL &&
                                         state_async_repl(state)->enabled);
    }
    prompt[length] = '\0';
    return length;
}

static void prepare_managed_render(shell_state *state)
{
    if (state == NULL) return;
    char prompt[GSH_ASYNC_PROMPT_CAP];
    const char *render;
    size_t length;
    int focused;

    if (state->async_repl != NULL && state_async_repl(state)->enabled)
        gsh_async_repl_tick(state->async_repl, monotonic_ns());
    if (state->async_repl == NULL || !state_async_repl(state)->enabled ||
        !state_async_repl(state)->render_pending || state->output_len != 0) {
        return;
    }
    focused = gsh_async_repl_focused_job(state->async_repl);
    if (focused >= 0 && state_async_repl(state)->cells[focused].fullscreen &&
        state_async_repl(state)->cells[focused].fullscreen_presented) {
        return;
    }
    (void)active_prompt_text(state, prompt);
    if (gsh_async_repl_prepare_render_with_completion(
            state->async_repl, prompt, state->line, state->line_len,
            state->line_cursor, state->completion_menu,
            state->completion_menu_length) == -1) {
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
    if (state == NULL) {
        return;
    }
    state->editor_modes = state->original_modes;
    state->editor_modes.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    if (state->async_repl != NULL && state_async_repl(state)->enabled) {
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
    if (state == NULL) return -1;
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
    if (state == NULL) return;
    static const char disable_paste[] = "\033[?2004l";
    size_t offset = 0U;
    unsigned int attempts;

    if (state->tty_fd < 0 || !state->terminal_changed) {
        return;
    }
    for (attempts = 0U;
         attempts < 4U && offset < sizeof(disable_paste) - 1U;
         attempts++) {
        ssize_t written = write(state->tty_fd, disable_paste + offset,
                                sizeof(disable_paste) - 1U - offset);

        if (written > 0) offset += (size_t)written;
        else if (written != -1 || errno != EINTR) break;
    }
    (void)tcsetpgrp(state->tty_fd, state->shell_pgid);
    (void)tcsetattr(state->tty_fd, TCSANOW, &state->original_modes);
    state->terminal_changed = false;
}

/* ── POSIX Owns the Signal-Handler Pointer Shape ─────────────────
 * The reactor needs signal events, but repository code may not dispatch work
 * through callbacks. CANON-EXCEPTION: POSIX-SIGNAL-DISPOSITION confines the
 * required function pointer to this sigaction adapter and its fixed call list.
 * Handlers only set sig_atomic_t flags; the reactor performs all state changes.
 * Signal, trap, fault, and sanitizer gates exercise the alternative guarantee.
 * ─────────────────────────────────────────────────────────────── */
static int install_handler(int signo, void (*handler)(int), int flags)
{
    struct sigaction action;

    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = handler;
    action.sa_flags = flags;
    (void)sigemptyset(&action.sa_mask);
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

static bool managed_repl_requested(const gsh_shell_config *config)
{
    if (config == NULL) {
        return false;
    }
    const char *mode = getenv("GSH_REPL");

    if (mode != NULL) {
        return strcmp(mode, "classic") != 0;
    }
    return config->async_repl_enabled;
}

static bool terminal_actions_requested(const gsh_shell_config *config,
                                       bool managed)
{
    /* ── Automatic Actions Restore Direct File Navigation ───────
     * Automatic mode originally made managed resources directly clickable.
     * Disabling its SGR channel also removed the underline and preview hitbox.
     * A usable terminal can carry those bounded mouse reports, while dumb
     * terminals degrade safely and explicit off remains the selection opt-out.
     * ─────────────────────────────────────────────────────────────── */
    const char *terminal;
    if (config == NULL || !managed) return false;
    if (config->terminal_actions == GSH_TERMINAL_ACTIONS_OFF) return false;
    if (config->terminal_actions == GSH_TERMINAL_ACTIONS_ON) return true;
    terminal = getenv("TERM");
    return terminal != NULL && terminal[0] != '\0' &&
           strcmp(terminal, "dumb") != 0;
}

static bool terminal_feature_present(const char *features,
                                     const char *wanted)
{
    size_t wanted_length;
    size_t length;

    if (features == NULL || wanted == NULL) return false;
    wanted_length = strlen(wanted);
    length = strnlen(features, 256U);
    if (wanted_length == 0U || length == 256U) return false;
    for (size_t offset = 0U; offset + wanted_length <= length; offset++) {
        bool before = offset == 0U || features[offset - 1U] == ',' ||
                      features[offset - 1U] == ';' ||
                      features[offset - 1U] == ':' ||
                      features[offset - 1U] == ' ';
        size_t after_offset = offset + wanted_length;
        bool after = after_offset == length || features[after_offset] == ',' ||
                     features[after_offset] == ';' ||
                     features[after_offset] == ':' ||
                     features[after_offset] == ' ';

        if (before && after &&
            memcmp(features + offset, wanted, wanted_length) == 0)
            return true;
    }
    return false;
}

static bool local_terminal_identity(const char *program, const char *session)
{
    const char *terminal_program;

    if (program == NULL || session == NULL || session[0] == '\0') return false;
    if (getenv("SSH_CONNECTION") != NULL || getenv("TMUX") != NULL ||
        getenv("STY") != NULL) return false;
    terminal_program = getenv("TERM_PROGRAM");
    return terminal_program != NULL &&
           strcmp(terminal_program, program) == 0;
}

/* ── Graphics Require End-To-End Evidence ───────────────────────
 * Terminal names once enabled image escapes optimistically, which corrupted
 * remote and multiplexer sessions that filtered the corresponding replies.
 * TERM_FEATURES represents a completed capability exchange and is therefore
 * authoritative across a chain.  A local emulator identity is accepted only
 * when neither SSH nor a multiplexer can stand between gsh and that emulator.
 * Unknown sessions retain the same geometry through ordinary text frames.
 * ─────────────────────────────────────────────────────────────── */
static gsh_terminal_image_protocol terminal_image_protocol(
    const gsh_shell_config *config)
{
    const char *features;

    if (config == NULL ||
        config->terminal_images == GSH_TERMINAL_IMAGES_OFF)
        return GSH_TERMINAL_IMAGE_NONE;
    features = getenv("TERM_FEATURES");
    if (terminal_feature_present(features, "K"))
        return GSH_TERMINAL_IMAGE_KITTY;
    if (terminal_feature_present(features, "F"))
        return GSH_TERMINAL_IMAGE_ITERM;
    if (terminal_feature_present(features, "Sx"))
        return GSH_TERMINAL_IMAGE_SIXEL;
    if (local_terminal_identity("iTerm.app", getenv("ITERM_SESSION_ID")))
        return GSH_TERMINAL_IMAGE_ITERM;
    if (local_terminal_identity("kitty", getenv("KITTY_WINDOW_ID")))
        return GSH_TERMINAL_IMAGE_KITTY;
    if (local_terminal_identity("ghostty", getenv("TERM_PROGRAM_VERSION")))
        return GSH_TERMINAL_IMAGE_KITTY;
    return GSH_TERMINAL_IMAGE_NONE;
}

static bool terminal_reply_contains(const char *reply, size_t length,
                                    const char *wanted)
{
    size_t wanted_length;

    if (reply == NULL || wanted == NULL) return false;
    wanted_length = strlen(wanted);
    if (wanted_length == 0U || wanted_length > length) return false;
    for (size_t offset = 0U; offset <= length - wanted_length; offset++) {
        if (memcmp(reply + offset, wanted, wanted_length) == 0) return true;
    }
    return false;
}

static bool terminal_reply_has_sixel_da(const char *reply, size_t length)
{
    if (reply == NULL) return false;
    for (size_t begin = 0U; begin + 3U < length; begin++) {
        if (reply[begin] != '\033' || reply[begin + 1U] != '[' ||
            reply[begin + 2U] != '?') continue;
        for (size_t offset = begin + 3U; offset < length; offset++) {
            size_t value = 0U;
            bool digits = false;

            while (offset < length && reply[offset] >= '0' &&
                   reply[offset] <= '9') {
                if (value <= 10000U) {
                    value = value * 10U +
                            (size_t)(reply[offset] - '0');
                }
                digits = true;
                offset++;
            }
            if (digits && value == 4U) return true;
            if (offset >= length || reply[offset] == 'c') break;
            if (reply[offset] != ';') break;
        }
    }
    return false;
}

static gsh_terminal_image_protocol terminal_image_active_probe(
    shell_state *state)
{
    static const char query[] =
        "\033_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\033\\"
        "\033]1337;ReportCellSize\a\033[c";
    char reply[1024];
    size_t used = 0U;
    size_t sent = 0U;

    if (state == NULL || state->tty_fd < 0 ||
        state->config.terminal_images != GSH_TERMINAL_IMAGES_ON)
        return GSH_TERMINAL_IMAGE_NONE;
    while (sent < sizeof(query) - 1U) {
        ssize_t count = write(state->tty_fd, query + sent,
                              sizeof(query) - 1U - sent);
        if (count > 0) sent += (size_t)count;
        else if (count == -1 && errno == EINTR) continue;
        else return GSH_TERMINAL_IMAGE_NONE;
    }
    for (size_t turn = 0U; turn < 5U && used < sizeof(reply); turn++) {
        struct pollfd input = {state->tty_fd, POLLIN, 0};
        int ready = poll(&input, 1U, 20);
        if (ready == -1 && errno == EINTR) { turn--; continue; }
        if (ready <= 0 || (input.revents & POLLIN) == 0) continue;
        { ssize_t count = read(state->tty_fd, reply + used,
                               sizeof(reply) - used);
          if (count > 0) used += (size_t)count; }
    }
    if (terminal_reply_contains(reply, used, "\033_Gi=31;OK"))
        return GSH_TERMINAL_IMAGE_KITTY;
    if (terminal_reply_contains(reply, used, "\033[4;"))
        return GSH_TERMINAL_IMAGE_ITERM;
    return terminal_reply_has_sixel_da(reply, used)
               ? GSH_TERMINAL_IMAGE_SIXEL : GSH_TERMINAL_IMAGE_NONE;
}

static void initialize_repl_size(shell_state *state)
{
    if (state == NULL) return;
    struct winsize size;

    if (state->async_repl == NULL || !state_async_repl(state)->enabled) {
        return;
    }
    (void)memset(&size, 0, sizeof(size));
    if (ioctl(state->tty_fd, TIOCGWINSZ, &size) == -1) {
        gsh_async_repl_resize(state->async_repl, 24, 80);
        return;
    }
    gsh_async_repl_resize(state->async_repl, size.ws_row, size.ws_col);
}

/* ── Interactive State Has One Bounded Owner ─────────────────────
 * The shell used to acquire its long-lived stores piecemeal from the heap.
 * Their capacities are compile-time contracts, so a function-local static
 * owner is both stricter and simpler: every pointer below is a view into that
 * owner and can never outlive it.  Fault hooks remain at the old acquisition
 * boundaries so the resource-failure suite still exercises each response.
 * ─────────────────────────────────────────────────────────────── */
static int bind_interactive_storage(shell_state *state,
                                    interactive_storage *storage)
{
    if (state == NULL || storage == NULL) {
        return -1;
    }
    (void)memset(storage, 0, sizeof(*storage));
    if (gsh_fault_should_fail(GSH_FAULT_HISTORY_ALLOCATION, ENOMEM)) return -1;
    state->history = &storage->history;
    state->session_history = &storage->session_history;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->parse_storage = &storage->parse_storage;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->native_pipeline = &storage->native_pipeline;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->command_cache = &storage->command_cache;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->command_cache_scratch = &storage->command_cache_scratch;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->variables = &storage->variables;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->variable_scratch = &storage->variable_scratch;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->pipeline_variables = &storage->pipeline_variables;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->variable_commit = &storage->variable_commit;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->pipeline_changes = &storage->pipeline_changes;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->source_workspaces = &storage->source_workspaces;
    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) return -1;
    state->async_repl = &storage->async_repl;
    state->positional_storage = &storage->positionals;
    state->positional_commit = &storage->positional_commit;
    return 0;
}

static void initialize_shell_state(shell_state *state,
                                   const char *program_path)
{
    if (!require(state != NULL)) return;
    if (!require(program_path != NULL)) return;
    (void)memset(state, 0, sizeof(*state));
    state->parameter_zero = program_path;
    state->pending_input = state->pending_line;
    state->tty_fd = -1;
    state->signal_pipe[0] = -1;
    state->signal_pipe[1] = -1;
    state->redirection_worker_fd = -1;
    state->redirection_worker_pid = -1;
    state->completion_fd = -1;
    state->llm_repl.fd = -1;
    state->journal.descriptor = -1;
    state->journal_pid = -1;
    state->llm_repl_peer = -1;
    state->llm_owner_cell = -1;
    state->llm_command_cell = -1;
    state->completion_pid = -1;
    state->completion_next_request_id = 1U;
    state->variable_commit_fd = -1;
    state->job_service_socket = -1;
    state->job_service_wait_reply_fd = -1;
    state->exec_outcome_fd = -1;
    state->exec_descriptor_socket = -1;
    state->directory_commit_socket = -1;
    state->directory_commit_fd = -1;
    state->async_capture_cell = -1;
    state->async_state_cell = -1;
    state->async_dispatch_cell = -1;
    state->focus_escape_cell = -1;
    state->running = true;
}

static void load_interactive_config(shell_state *state)
{
    const char *history_override;
    const char *home;

    if (!require(state != NULL)) return;
    if (!require(state->config.diagnostic[0] == '\0')) return;
    gsh_config_defaults(&state->config);
    history_override = getenv("GSH_HISTORY");
    home = getenv("HOME");
    if (history_override != NULL && strcmp(history_override, "off") == 0) {
        state->config.history_enabled = false;
    } else if (home == NULL || home[0] != '/') {
        state->config_error = true;
        state->config.history_enabled = false;
        (void)snprintf(state->config.diagnostic,
                       sizeof(state->config.diagnostic),
                       "HOME is not an absolute path");
    } else if (gsh_config_load(&state->config, home, true) == -1) {
        state->config_error = true;
        state->config.history_enabled = false;
    }
}

static int initialize_interactive_stores(shell_state *state,
                                         interactive_storage *storage)
{
    if (!require(state != NULL)) return -1;
    if (!require(storage != NULL)) return -1;
    if (bind_interactive_storage(state, storage) == -1) return -1;
    gsh_history_initialize(state->history);
    gsh_history_initialize(state->session_history);
    gsh_options_initialize(&state->options, true);
    gsh_background_initialize(&state->background_jobs);
    state->redirection_next_request_id = 1;
    if (gsh_variables_import(state->variables, environ) == -1) return -1;
    gsh_source_workspaces_initialize(state->source_workspaces);
    state->aliases = &state_source_workspaces(state)->root_aliases;
    state->alias_expansion = state_source_workspaces(state)->root_alias_expansion;
    state->alias_scratch = &state_source_workspaces(state)->root_alias_scratch;
    state->alias_commit = &state_source_workspaces(state)->root_alias_commit;
    state->functions = &state_source_workspaces(state)->root_functions;
    state->function_scratch = &state_source_workspaces(state)->root_function_scratch;
    gsh_command_cache_initialize(
        state->command_cache,
        gsh_variables_path_generation(state->variables));
    gsh_command_cache_initialize(
        state->command_cache_scratch,
        gsh_variables_path_generation(state->variables));
    state->command_cache_generation = 1;
    state->async_desired = managed_repl_requested(&state->config);
    gsh_async_repl_initialize(state->async_repl, state->async_desired);
    gsh_async_repl_configure_actions(
        state->async_repl,
        terminal_actions_requested(&state->config, state->async_desired),
        state->config.path_detection);
    state->image_protocol = terminal_image_protocol(&state->config);
    state->variable_generation = 1;
    state->alias_generation = 1;
    state->function_generation = 1;
    gsh_variable_journal_initialize(state->variable_commit,
                                    state->variable_generation);
    return 0;
}

static void initialize_prompt_identity(shell_state *state)
{
    struct passwd *account;
    size_t index;
    size_t user_length;

    if (!require(state != NULL)) return;
    account = getpwuid(geteuid());
    user_length = account == NULL || account->pw_name == NULL ||
                          account->pw_name[0] == '\0'
                      ? GSH_PROMPT_IDENTITY_CAP
                      : strnlen(account->pw_name, GSH_PROMPT_IDENTITY_CAP);
    if (user_length < GSH_PROMPT_IDENTITY_CAP) {
        (void)memcpy(state->prompt_user, account->pw_name, user_length + 1U);
    } else {
        (void)snprintf(state->prompt_user, sizeof(state->prompt_user),
                       "uid%lu", (unsigned long)geteuid());
    }
    sanitize_prompt_component(state->prompt_user, sizeof(state->prompt_user));
    if (gethostname(state->prompt_host, sizeof(state->prompt_host) - 1U) == -1)
        state->prompt_host[0] = '\0';
    state->prompt_host[sizeof(state->prompt_host) - 1U] = '\0';
    for (index = 0U; index < sizeof(state->prompt_host); index++)
        if (state->prompt_host[index] == '.' ||
            state->prompt_host[index] == '\0') {
            state->prompt_host[index] = '\0';
            break;
        }
    if (state->prompt_host[0] == '\0')
        (void)memcpy(state->prompt_host, "unknown", sizeof("unknown"));
    sanitize_prompt_component(state->prompt_host, sizeof(state->prompt_host));
}

static void initialize_interactive_paths(shell_state *state)
{
    size_t default_path_size;

    if (!require(state != NULL)) return;
    if (!require(state->default_path[sizeof(state->default_path) - 1U] ==
                 '\0')) return;
    default_path_size =
        confstr(_CS_PATH, state->default_path, sizeof(state->default_path));
    if (default_path_size == 0 ||
        default_path_size > sizeof(state->default_path) ||
        state->default_path[0] == '\0')
        (void)memcpy(state->default_path, "/bin:/usr/bin", 14U);
    if (getcwd(state->current_directory,
               sizeof(state->current_directory)) == NULL)
        state->current_directory[0] = '\0';
    initialize_prompt_identity(state);
}

static int claim_interactive_terminal(shell_state *state)
{
    const char *terminal_name;
    pid_t foreground_group;
    pid_t current_group;

    if (!require(state != NULL)) return -1;
    if (!require(state->tty_fd == -1)) return -1;
    terminal_name = ttyname(STDIN_FILENO);
    if (terminal_name == NULL) return -1;
    state->tty_fd = gsh_fault_should_fail(GSH_FAULT_TTY_OPEN, EMFILE)
                        ? -1
                        : open(terminal_name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (state->tty_fd == -1 ||
        set_fd_flags(state->tty_fd, F_GETFD, FD_CLOEXEC) == -1) {
        return -1;
    }
    current_group = getpgrp();
    foreground_group = tcgetpgrp(state->tty_fd);
    while (foreground_group != current_group) {
        if (foreground_group == -1) return -1;
        if (kill(-current_group, SIGTTIN) == -1) return -1;
        current_group = getpgrp();
        foreground_group = tcgetpgrp(state->tty_fd);
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
    return 0;
}

static int activate_interactive_signals(shell_state *state)
{
    if (!require(state != NULL)) return -1;
    if (!require(state->signal_pipe[0] == -1 &&
                 state->signal_pipe[1] == -1)) {
        return -1;
    }
    if (make_pipe(state->signal_pipe, true, GSH_FAULT_SIGNAL_PIPE) == -1) {
        return -1;
    }
    g_signal_write_fd = state->signal_pipe[1];
    if (install_signal_handlers() == -1 || enter_editor(state) == -1) {
        return -1;
    }
    initialize_repl_size(state);
    return 0;
}

static int initialize_interactive(shell_state *state,
                                  interactive_storage *storage,
                                  const char *program_path)
{
    if (!require(state != NULL)) return -1;
    if (!require(storage != NULL && program_path != NULL)) return -1;
    initialize_shell_state(state, program_path);
    load_interactive_config(state);
    if (initialize_interactive_stores(state, storage) == -1) {
        return -1;
    }
    initialize_interactive_paths(state);
    if (claim_interactive_terminal(state) == -1 ||
        activate_interactive_signals(state) == -1) return -1;
    if (state->image_protocol == GSH_TERMINAL_IMAGE_NONE)
        state->image_protocol = terminal_image_active_probe(state);
    return 0;
}

static void reset_child_signals(void)
{
    const int signals[] = {SIGCHLD, SIGINT,  SIGWINCH, SIGHUP, SIGTERM,
                           SIGQUIT, SIGTSTP, SIGTTIN,  SIGTTOU, SIGPIPE};
    size_t index;

    for (index = 0; index < sizeof(signals) / sizeof(signals[0]); index++) {
        struct sigaction action;

        (void)memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        (void)sigemptyset(&action.sa_mask);
        (void)sigaction(signals[index], &action, NULL);
    }
}

static void redirection_worker_loop(int fd)
{
    bool connection_open = true;

    /* Lifecycle loop: the socket peer owns termination; every iteration
     * consumes exactly one versioned request or exits the worker. */
    while (connection_open) {
        redirection_request request;
        redirection_result result;
        ssize_t received;

        do {
            received = recv(fd, &request, sizeof(request), 0);
        } while (received == -1 && errno == EINTR);
        if (received != (ssize_t)sizeof(request)) {
            _exit(received == -1 ? 1 : 0);
        }
        if (request.version != REDIRECTION_WORKER_PROTOCOL_VERSION ||
            memchr(request.directory, '\0', sizeof(request.directory)) ==
                NULL) {
            _exit(1);
        }

        (void)memset(&result, 0, sizeof(result));
        result.version = REDIRECTION_WORKER_PROTOCOL_VERSION;
        result.request_id = request.request_id;
        {
            gsh_shell_options options = {request.option_bits, 1U, 1U, 0U};
            int descriptor = open_redirect_path(
                request.directory, (gsh_token_kind)request.operator_kind,
                &options, (mode_t)request.creation_mode);

            if (descriptor == -1) {
                result.status = 1;
                result.error = errno;
            } else {
                (void)close(descriptor);
                result.status = request.builtin_status;
            }
        }
        do {
            received = send(fd, &result, sizeof(result), 0);
        } while (received == -1 && errno == EINTR);
        if (received != (ssize_t)sizeof(result)) {
            _exit(1);
        }
    }
}

static void disable_redirection_worker(shell_state *state, bool terminate)
{
    if (state == NULL) return;
    if (state->redirection_worker_fd >= 0) {
        (void)close(state->redirection_worker_fd);
        state->redirection_worker_fd = -1;
    }
    if (terminate && state->redirection_worker_pid > 0) {
        (void)kill(state->redirection_worker_pid, SIGKILL);
    }
    state->redirection_worker_alive = false;
    state->redirection_worker_busy = false;
    state->redirection_active_request_id = 0;
}

static int start_redirection_worker(shell_state *state)
{
    if (state == NULL) {
        return -1;
    }
    int sockets[2];
    sigset_t blocked;
    sigset_t previous;
    pid_t pid;

    if (gsh_fault_should_fail(GSH_FAULT_WORKER_SOCKET, EMFILE) ||
        socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets) == -1) {
        return -1;
    }
    if (set_fd_flags(sockets[0], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(sockets[1], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(sockets[0], F_GETFL, O_NONBLOCK) == -1) {
        int saved_errno = errno;

        (void)close(sockets[0]);
        (void)close(sockets[1]);
        errno = saved_errno;
        return -1;
    }

    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        int saved_errno = errno;

        (void)close(sockets[0]);
        (void)close(sockets[1]);
        errno = saved_errno;
        return -1;
    }

    pid = gsh_fault_should_fail(GSH_FAULT_WORKER_FORK, EAGAIN) ? -1 : fork();
    if (pid == 0) {
        (void)close(sockets[0]);
        (void)setpgid(0, 0);
        reset_child_signals();
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        (void)close(state->tty_fd);
        (void)close(state->signal_pipe[0]);
        (void)close(state->signal_pipe[1]);
        (void)close(STDIN_FILENO);
        (void)close(STDOUT_FILENO);
        (void)close(STDERR_FILENO);
        (void)umask(0);
        redirection_worker_loop(sockets[1]);
    }

    (void)close(sockets[1]);
    if (pid == -1) {
        int saved_errno = errno;

        (void)close(sockets[0]);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        errno = saved_errno;
        return -1;
    }

    (void)setpgid(pid, pid);
    state->redirection_worker_fd = sockets[0];
    state->redirection_worker_pid = pid;
    state->redirection_worker_alive = true;
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
    return 0;
}

static void report_journal_failure(shell_state *state)
{
    if (state == NULL || state->journal_warning) return;
    state->journal_warning = true;
    (void)output_text(state,
        "gsh: AI journal unavailable or full; some records were not saved\r\n");
}

static void close_journal_pipe(shell_state *state)
{
    if (state == NULL) return;
    if (state->journal.descriptor >= 0)
        (void)close(state->journal.descriptor);
    state->journal.descriptor = -1;
    state->journal.used = state->journal.sent = 0U;
}

static void service_journal_descriptor(shell_state *state,
                                        const struct pollfd *descriptor)
{
    if (state == NULL || descriptor == NULL || state->journal.descriptor < 0 ||
        descriptor->fd != state->journal.descriptor) return;
    if ((descriptor->revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) != 0 &&
        (gsh_llm_journal_flush(&state->journal) == -1 ||
         (descriptor->revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)) {
        close_journal_pipe(state);
        report_journal_failure(state);
    }
}

static void start_journal_worker(shell_state *state)
{
    int descriptors[2];
    pid_t pid;

    if (state == NULL || !state->config.llm_enabled) return;
    if (pipe(descriptors) == -1) { report_journal_failure(state); return; }
    if (set_fd_flags(descriptors[0], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(descriptors[1], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(descriptors[1], F_GETFL, O_NONBLOCK) == -1) {
        (void)close(descriptors[0]);
        (void)close(descriptors[1]);
        report_journal_failure(state);
        return;
    }
    pid = fork();
    if (pid == 0) {
        (void)close(descriptors[1]);
        (void)setpgid(0, 0);
        reset_child_signals();
        close_worker_child_descriptors(state, descriptors[0]);
        _exit(gsh_llm_journal_worker(descriptors[0]));
    }
    (void)close(descriptors[0]);
    if (pid < 0) {
        (void)close(descriptors[1]);
        report_journal_failure(state);
        return;
    }
    (void)setpgid(pid, pid);
    state->journal.descriptor = descriptors[1];
    state->journal_pid = pid;
}

static void enqueue_journal_record(shell_state *state, unsigned int kind,
                                    const char *text, size_t length)
{
    if (state == NULL || text == NULL) return;
    if (gsh_llm_journal_enqueue(&state->journal, getenv("HOME"), kind,
                                text, length) == -1)
        report_journal_failure(state);
}

static void receive_redirection_result(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    redirection_result result;
    ssize_t received;

    do {
        received = recv(state->redirection_worker_fd, &result,
                        sizeof(result), 0);
    } while (received == -1 && errno == EINTR);
    if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    if (received != (ssize_t)sizeof(result) ||
        result.version != REDIRECTION_WORKER_PROTOCOL_VERSION ||
        result.request_id != state->redirection_active_request_id) {
        bool command = state->mode == MODE_ASYNC_REDIRECTION;

        state->redirection_worker_failures++;
        disable_redirection_worker(state, true);
        if (command) {
            (void)output_text(state, "gsh: asynchronous redirection failed\r\n");
            state->last_status = 1;
            state->mode = MODE_EDITOR;
            queue_prompt(state);
        }
        return;
    }

    state->redirection_worker_busy = false;
    state->redirection_active_request_id = 0;
    if (result.error != 0) {
        output_format(state, "gsh: %s: %s\r\n",
                      state->redirection_target,
                      strerror(result.error));
    }
    if (state->redirection_pipeline_negated) {
        result.status = result.status == 0 ? 1 : 0;
    }
    state->last_status = result.status;
    state->mode = MODE_EDITOR;
    state->redirection_target[0] = '\0';
    queue_prompt(state);
}

static int bounded_deadline_timeout(uint64_t deadline, int current)
{
    uint64_t now;
    uint64_t remaining;
    uint64_t milliseconds;
    int timeout;
    if (deadline == 0U) return current;
    now = monotonic_ns();
    if (now >= deadline) return 0;
    remaining = deadline - now;
    milliseconds = (remaining + 999999U) / 1000000U;
    timeout = milliseconds > (uint64_t)INT_MAX ? INT_MAX : (int)milliseconds;
    return current < 0 || timeout < current ? timeout : current;
}

static int reactor_poll_timeout(const shell_state *state)
{
    int timeout = -1;
    if (state == NULL) return -1;
    timeout = bounded_deadline_timeout(state->focus_escape_deadline_ns,
                                       timeout);
    timeout = bounded_deadline_timeout(state->editor_escape_deadline_ns,
                                       timeout);
    if (state->async_repl != NULL)
        timeout = bounded_deadline_timeout(
            state_async_repl(state)->ai_animation_deadline_ns, timeout);
    return bounded_deadline_timeout(state->completion_deadline_ns, timeout);
}

static void reclaim_terminal(shell_state *state, bool save_job_modes)
{
    if (state == NULL) {
        return;
    }
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
                           size_t command_count, bool pipefail,
                           bool foreground, bool negated)
{
    if (current == NULL || members == NULL) {
        return;
    }
    size_t index;

    (void)memset(current, 0, sizeof(*current));
    current->active = true;
    current->foreground = foreground;
    current->pid = pgid;
    current->pgid = pgid;
    current->status_pid = status_pid;
    current->member_count = member_count;
    current->remaining = member_count;
    current->negated = negated;
    gsh_pipeline_status_initialize(&current->pipeline_status,
                                   command_count, pipefail);
    for (index = 0; index < member_count; index++) {
        current->members[index] = members[index];
        current->member_states[index] = JOB_MEMBER_RUNNING;
    }
}

enum {
    ISOLATED_JOB_TABLE_CAP =
        GSH_FUNCTION_DEPTH_CAP + GSH_SOURCE_DEPTH_CAP + 4,
};

typedef struct {
    gsh_background_table tables[ISOLATED_JOB_TABLE_CAP];
    bool used[ISOLATED_JOB_TABLE_CAP];
} isolated_job_pool;

static isolated_job_pool *process_isolated_job_pool(void)
{
    static isolated_job_pool pool;

    return &pool;
}

static gsh_background_table *allocate_isolated_job_table(void)
{
    isolated_job_pool *pool = process_isolated_job_pool();
    size_t index;

    if (gsh_fault_should_fail(GSH_FAULT_JOB_TABLE_ALLOCATION, ENOMEM)) {
        return NULL;
    }
    for (index = 0; index < ISOLATED_JOB_TABLE_CAP; index++) {
        if (!pool->used[index]) {
            pool->used[index] = true;
            gsh_background_initialize(&pool->tables[index]);
            return &pool->tables[index];
        }
    }
    errno = ENOSPC;
    return NULL;
}

static void release_isolated_job_table(gsh_background_table *table)
{
    isolated_job_pool *pool = process_isolated_job_pool();
    size_t index;

    for (index = 0; index < ISOLATED_JOB_TABLE_CAP; index++) {
        if (table == &pool->tables[index]) {
            (void)memset(table, 0, sizeof(*table));
            pool->used[index] = false;
            return;
        }
    }
    if (table != NULL) errno = EINVAL;
}

static size_t find_job_member(const job *current, pid_t pid)
{
    if (current == NULL) {
        return 0U;
    }
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
    if (current == NULL) return false;
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

    descriptor = gsh_fault_should_fail(GSH_FAULT_DIRECTORY_COMMIT_OPEN, EMFILE)
                     ? -1
                     : open(".", O_RDONLY);
    if (descriptor < 0) {
        return -1;
    }
    (void)memset(&control, 0, sizeof(control));
    (void)memset(&message, 0, sizeof(message));
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
    (void)memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
    do {
        sent = gsh_fault_should_fail(GSH_FAULT_DIRECTORY_COMMIT_SEND, EIO)
                   ? -1
                   : sendmsg(socket, &message, 0);
    } while (sent == -1 && errno == EINTR);
    (void)close(descriptor);
    return sent == (ssize_t)sizeof(marker) ? 0 : -1;
}

static int receive_directory_descriptor(shell_state *state)
{
    if (state == NULL) return -1;
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
    (void)memset(&control, 0, sizeof(control));
    (void)memset(&message, 0, sizeof(message));
    vector.iov_base = &marker;
    vector.iov_len = sizeof(marker);
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    do {
        received = gsh_fault_should_fail(GSH_FAULT_DIRECTORY_COMMIT_RECEIVE, EIO)
                       ? -1
                       : recvmsg(state->directory_commit_socket, &message,
                                 0);
    } while (received == -1 && errno == EINTR);
    (void)close(state->directory_commit_socket);
    state->directory_commit_socket = -1;
    header = CMSG_FIRSTHDR(&message);
    if (header != NULL && header->cmsg_level == SOL_SOCKET &&
        header->cmsg_type == SCM_RIGHTS &&
        header->cmsg_len >= CMSG_LEN(sizeof(descriptor))) {
        (void)memcpy(&descriptor, CMSG_DATA(header), sizeof(descriptor));
    }
    if (received != (ssize_t)sizeof(marker) || marker != 1 ||
        (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0 ||
        header == NULL || header->cmsg_level != SOL_SOCKET ||
        header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(descriptor)) ||
        CMSG_NXTHDR(&message, header) != NULL) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        errno = EPROTO;
        return -1;
    }
    if (descriptor < 0 ||
        set_fd_flags(descriptor, F_GETFD, FD_CLOEXEC) == -1) {
        int saved_errno = errno;

        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        errno = saved_errno;
        return -1;
    }
    state->directory_commit_fd = descriptor;
    return 0;
}

static size_t state_commit_size(const shell_state *state)
{
    if (state == NULL) {
        return 0U;
    }
    size_t size = sizeof(*state->variable_commit) +
                  (state->alias_commit_expected
                       ? sizeof(*state->alias_commit)
                       : 0U) +
                  (state->positional_commit_expected
                       ? sizeof(*state->positional_commit)
                       : 0U) +
                  (state->command_cache_commit_expected
                       ? sizeof(state->command_cache_commit_header) +
                             sizeof(*state->command_cache_scratch)
                       : 0U) +
                  sizeof(state->option_commit) +
                  sizeof(state->control_commit);

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

/* ── Control Flow Commits with the State It Observed ───────────
 * An evaluator child can execute in the current shell environment even
 * though process isolation keeps blocking work out of the reactor. State
 * journals alone could not tell the owner that an inner eval, dot script, or
 * compound command executed exit. A fixed versioned record now travels in
 * the same transaction, so the owner either commits both the final state and
 * termination request or rejects both; subshell processes never send it.
 * ─────────────────────────────────────────────────────────────── */
static bool state_control_commit_valid(const state_control_commit *commit)
{
    if (commit == NULL ||
        commit->version != GSH_STATE_CONTROL_COMMIT_VERSION ||
        (commit->flags & ~GSH_STATE_CONTROL_EXIT) != 0U ||
        commit->reserved != 0U) {
        return false;
    }
    return (commit->flags & GSH_STATE_CONTROL_EXIT) != 0U
               ? commit->exit_status >= 0 && commit->exit_status <= 255
               : commit->exit_status == 0;
}

static void close_variable_commit(shell_state *state)
{
    if (state == NULL) return;
    if (state->variable_commit_fd >= 0) {
        (void)close(state->variable_commit_fd);
    }
    state->variable_commit_fd = -1;
    state->variable_commit_received = 0;
    state->variable_commit_active = false;
    state->variable_commit_eof = false;
    state->variable_commit_invalid = false;
    if (state->job_service_socket >= 0) {
        (void)close(state->job_service_socket);
    }
    state->job_service_socket = -1;
    state->pending_job_service = false;
    if (state->job_service_wait_reply_fd >= 0) {
        (void)close(state->job_service_wait_reply_fd);
    }
    state->job_service_wait_reply_fd = -1;
    state->job_service_wait_target_count = 0;
    state->job_service_wait_all = false;
    if (state->exec_outcome_fd >= 0) {
        (void)close(state->exec_outcome_fd);
    }
    state->exec_outcome_fd = -1;
    state->pending_exec_possible = false;
    if (state->exec_descriptor_socket >= 0) {
        (void)close(state->exec_descriptor_socket);
    }
    state->exec_descriptor_socket = -1;
    state->pending_exec_descriptor_count = 0;
    state->pending_exec_protected_descriptor_count = 0;
    state->positional_commit_expected = false;
    state->pending_positional_commit = false;
    state->alias_commit_expected = false;
    state->pending_alias_commit = false;
    state->function_commit_expected = false;
    state->pending_function_commit = false;
    state->function_commit_header_complete = false;
    (void)memset(&state->function_commit_header, 0,
           sizeof(state->function_commit_header));
    state->command_cache_commit_expected = false;
    state->pending_command_cache_commit = false;
    (void)memset(&state->command_cache_commit_header, 0,
           sizeof(state->command_cache_commit_header));
    (void)memset(&state->control_commit, 0, sizeof(state->control_commit));
    if (state->directory_commit_socket >= 0) {
        (void)close(state->directory_commit_socket);
    }
    if (state->directory_commit_fd >= 0) {
        (void)close(state->directory_commit_fd);
    }
    state->directory_commit_socket = -1;
    state->directory_commit_fd = -1;
    state->directory_commit_expected = false;
    state->pending_directory_commit = false;
}

typedef struct {
    unsigned char *data;
    size_t size;
} commit_span;

static size_t initialize_commit_spans(shell_state *state,
                                      commit_span spans[8])
{
    if (!require(state != NULL)) return 0U;
    if (!require(spans != NULL)) return 0U;
    spans[0] = (commit_span){(unsigned char *)state->variable_commit,
                             sizeof(*state->variable_commit)};
    spans[1] = (commit_span){(unsigned char *)state->alias_commit,
                             state->alias_commit_expected
                                 ? sizeof(*state->alias_commit)
                                 : 0U};
    spans[2] = (commit_span){(unsigned char *)state->positional_commit,
                             state->positional_commit_expected
                                 ? sizeof(*state->positional_commit)
                                 : 0U};
    spans[3] = (commit_span){
        (unsigned char *)&state->command_cache_commit_header,
        state->command_cache_commit_expected
            ? sizeof(state->command_cache_commit_header)
            : 0U};
    spans[4] = (commit_span){(unsigned char *)state->command_cache_scratch,
                             state->command_cache_commit_expected
                                 ? sizeof(*state->command_cache_scratch)
                                 : 0U};
    spans[5] = (commit_span){(unsigned char *)&state->option_commit,
                             sizeof(state->option_commit)};
    spans[6] = (commit_span){(unsigned char *)&state->control_commit,
                             sizeof(state->control_commit)};
    spans[7] = (commit_span){
        (unsigned char *)&state->function_commit_header,
        state->function_commit_expected
            ? sizeof(state->function_commit_header)
            : 0U};
    return 8U;
}

static void *select_commit_destination(shell_state *state,
                                       unsigned char *extra,
                                       size_t *capacity)
{
    commit_span spans[8];
    size_t span_count;
    size_t offset;
    size_t index;

    if (!require(state != NULL && extra != NULL)) return NULL;
    if (!require(capacity != NULL)) return NULL;
    *capacity = state_commit_size(state) - state->variable_commit_received;
    if (*capacity > 4096U) *capacity = 4096U;
    offset = state->variable_commit_received;
    span_count = initialize_commit_spans(state, spans);
    for (index = 0; index < 8U && index < span_count; index++) {
        if (offset < spans[index].size) {
            size_t remaining = spans[index].size - offset;

            if (*capacity > remaining) *capacity = remaining;
            return spans[index].data + offset;
        }
        offset -= spans[index].size;
    }
    if (state->function_commit_expected &&
        state->function_commit_header_complete) {
        size_t available;
        void *destination = gsh_functions_snapshot_destination(
            state->function_scratch, &state->function_commit_header, offset,
            &available);

        if (destination != NULL && available > 0U) {
            if (*capacity > available) *capacity = available;
            return destination;
        }
    }
    state->variable_commit_invalid = true;
    *capacity = 1U;
    return extra;
}

static void accept_variable_commit_bytes(shell_state *state, size_t count)
{
    size_t header_end;

    if (!require(state != NULL)) return;
    if (!require(count > 0U)) return;
    if (state->variable_commit_received >= state_commit_size(state)) {
        state->variable_commit_invalid = true;
        return;
    }
    state->variable_commit_received += count;
    if (!state->function_commit_expected ||
        state->function_commit_header_complete) {
        return;
    }
    header_end = sizeof(*state->variable_commit) +
        (state->alias_commit_expected ? sizeof(*state->alias_commit) : 0U) +
        (state->positional_commit_expected
             ? sizeof(*state->positional_commit)
             : 0U) +
        (state->command_cache_commit_expected
             ? sizeof(state->command_cache_commit_header) +
                   sizeof(*state->command_cache_scratch)
             : 0U) +
        sizeof(state->option_commit) + sizeof(state->control_commit) +
        sizeof(state->function_commit_header);
    if (state->variable_commit_received < header_end) return;
    state->function_commit_header_complete = true;
    if (!gsh_functions_snapshot_header_valid(&state->function_commit_header) ||
        state->function_commit_header.base_generation !=
            state->function_generation) {
        state->variable_commit_invalid = true;
    }
}

static void receive_variable_commit(shell_state *state, bool drain_all)
{
    unsigned int reads = 0;
    unsigned int limit = drain_all ? 1024U : 4U;

    if (!require(state != NULL)) return;
    if (!require(limit == 4U || limit == 1024U)) return;
    while (state->variable_commit_fd >= 0 && reads++ < limit) {
        unsigned char extra;
        void *destination;
        size_t capacity;
        ssize_t count;

        if (state->variable_commit_received < state_commit_size(state)) {
            destination = select_commit_destination(state, &extra, &capacity);
            if (destination == NULL) return;
        } else {
            destination = &extra;
            capacity = 1U;
        }
        count = gsh_fault_should_fail(GSH_FAULT_STATE_COMMIT_READ, EIO)
                    ? -1
                    : read(state->variable_commit_fd, destination,
                           capacity);
        if (count > 0) {
            accept_variable_commit_bytes(state, (size_t)count);
            continue;
        }
        if (count == 0) {
            state->variable_commit_eof = true;
            (void)close(state->variable_commit_fd);
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
        (void)close(state->variable_commit_fd);
        state->variable_commit_fd = -1;
        return;
    }
}

static void reject_variable_commit(shell_state *state,
                                   bool directory_changed,
                                   int previous_directory)
{
    if (!require(state != NULL)) return;
    if (!require(!directory_changed || previous_directory >= 0)) return;
    if (directory_changed && fchdir(previous_directory) == -1) {
        int saved_errno = errno;

        output_format(state, "gsh: directory rollback failed: %s\r\n",
                      strerror(saved_errno));
        state->running = false;
    }
    (void)output_text(state, "gsh: state transaction rejected or incomplete\r\n");
}

static bool stage_variable_commit(shell_state *state)
{
    if (state == NULL) return false;
    (void)memcpy(state->variable_scratch, state->variables,
                 sizeof(*state->variable_scratch));
    if (gsh_variables_apply_journal_in_place(
            state->variable_scratch, state->variable_commit) == -1) {
        return false;
    }
    if (state->alias_commit_expected) {
        (void)memcpy(state->alias_scratch, state->aliases,
                     sizeof(*state->alias_scratch));
        if (gsh_aliases_apply_journal_in_place(
                state->alias_scratch, state->alias_commit) == -1) {
            return false;
        }
    }
    if (state->command_cache_commit_expected) {
        gsh_command_cache_rebind(
            state->command_cache_scratch,
            state->command_cache_commit_header.final_path_generation,
            gsh_variables_path_generation(state->variable_scratch));
    }
    return true;
}

static bool finish_variable_commit(shell_state *state, int wait_status)
{
    if (state == NULL) return false;
    bool valid;
    bool directory_changed = false;
    int previous_directory = -1;

    if (!state->variable_commit_active) {
        return true;
    }
    state->committed_exit_requested = false;
    state->committed_exit_status = 0;
    receive_variable_commit(state, true);
    valid = WIFEXITED(wait_status) && state->variable_commit_eof &&
            !state->variable_commit_invalid &&
            state->variable_commit_received == state_commit_size(state) &&
            gsh_variable_journal_validate(state->variable_commit) &&
            (!state->alias_commit_expected ||
             (gsh_alias_journal_validate(state->alias_commit) &&
              state_alias_commit(state)->base_generation ==
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
            (!state->command_cache_commit_expected ||
             (state->command_cache_commit_header.version ==
                  GSH_COMMAND_CACHE_COMMIT_VERSION &&
              state->command_cache_commit_header.reserved == 0 &&
              state->command_cache_commit_header.base_generation ==
                  state->command_cache_generation &&
              gsh_command_cache_validate(
                  state->command_cache_scratch))) &&
            gsh_options_validate(&state->option_commit) &&
            gsh_options_enabled(&state->option_commit,
                                GSH_OPTION_INTERACTIVE) ==
                gsh_options_enabled(&state->options,
                                    GSH_OPTION_INTERACTIVE) &&
            state_control_commit_valid(&state->control_commit) &&
            state_variable_commit(state)->base_generation ==
                state->variable_generation;
    if (valid && receive_directory_descriptor(state) == -1) {
        valid = false;
    }
    if (valid && state->directory_commit_expected) {
        previous_directory = open(".", O_RDONLY);
        if (previous_directory == -1 ||
            gsh_fault_should_fail(GSH_FAULT_DIRECTORY_COMMIT_APPLY, EIO) ||
            fchdir(state->directory_commit_fd) == -1) {
            valid = false;
        } else {
            directory_changed = true;
        }
    }
    if (valid) valid = stage_variable_commit(state);
    if (valid) {
        (void)memcpy(state->variables, state->variable_scratch,
               sizeof(*state->variables));
        if (state->alias_commit_expected) {
            (void)memcpy(state->aliases, state->alias_scratch,
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
                state->positionals = state->positional_storage;
            }
            (void)memcpy(state->positionals, state->positional_commit,
                   sizeof(*state->positionals));
        }
        if (state->command_cache_commit_expected) {
            (void)memcpy(state->command_cache, state->command_cache_scratch,
                   sizeof(*state->command_cache));
            state->command_cache_generation++;
        }
        state->options = state->option_commit;
        state->committed_exit_requested =
            (state->control_commit.flags & GSH_STATE_CONTROL_EXIT) != 0U;
        state->committed_exit_status = state->control_commit.exit_status;
        if (directory_changed) {
            if (getcwd(state->current_directory,
                       sizeof(state->current_directory)) == NULL) {
                state->current_directory[0] = '\0';
            }
        }
        state->variable_generation++;
    } else if (WIFEXITED(wait_status) || state->variable_commit_invalid) {
        reject_variable_commit(state, directory_changed,
                               previous_directory);
        valid = false;
    }
    if (previous_directory >= 0) {
        (void)close(previous_directory);
    }
    close_variable_commit(state);
    return valid;
}

static void schedule_pending_continuation(shell_state *state)
{
    if (!require(state != NULL)) return;
    if (!require(state->mode != MODE_FOREGROUND)) return;
    if (state->pending_and_or_active || state->pending_list_active) {
        state->mode = MODE_DISPATCH;
    } else {
        state->mode = MODE_EDITOR;
        queue_prompt(state);
    }
}

static bool finish_background_wait(shell_state *state)
{
    if (state == NULL) {
        return false;
    }
    size_t index;
    int status = state->wait_all ? 0 : 127;

    if (state->mode != MODE_WAIT) {
        return false;
    }
    if (state->wait_all) {
        if (!gsh_background_consume_all_if_done(
                &state->background_jobs)) {
            return false;
        }
    } else {
        for (index = 0; index < state->wait_target_count; index++) {
            bool done;

            if (state->wait_targets[index] > 0 &&
                gsh_background_get(&state->background_jobs,
                                   state->wait_targets[index], &done,
                                   NULL) &&
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
                if (index + 1U == state->wait_target_count) {
                    status = wait_status_value(wait_status);
                }
            } else if (index + 1U == state->wait_target_count) {
                status = 127;
            }
        }
    }
    state->wait_target_count = 0;
    state->wait_all = false;
    state->last_status = state->wait_negated ? (status == 0 ? 1 : 0)
                                             : status;
    state->wait_negated = false;
    schedule_pending_continuation(state);
    return true;
}

enum {
    GSH_EXEC_OUTCOME_INVALID = -1,
    GSH_EXEC_OUTCOME_NONE = 0,
    GSH_EXEC_OUTCOME_OVERLAID = 1,
    GSH_EXEC_OUTCOME_FAILED = 2,
};

static int finish_exec_outcome(shell_state *state)
{
    if (state == NULL) return -1;
    unsigned char outcomes[3];
    size_t received = 0;

    if (state->exec_outcome_fd < 0) {
        return GSH_EXEC_OUTCOME_NONE;
    }
    while (received < sizeof(outcomes)) {
        ssize_t count = read(state->exec_outcome_fd, outcomes + received,
                             sizeof(outcomes) - received);

        if (count > 0) {
            received += (size_t)count;
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else if (count == 0) {
            break;
        } else {
            received = sizeof(outcomes);
            break;
        }
    }
    (void)close(state->exec_outcome_fd);
    state->exec_outcome_fd = -1;
    if (received == 0) {
        return GSH_EXEC_OUTCOME_NONE;
    }
    if (received == 1U && outcomes[0] == 'A') {
        return GSH_EXEC_OUTCOME_OVERLAID;
    }
    if (received == 2U && outcomes[0] == 'A' && outcomes[1] == 'F') {
        return GSH_EXEC_OUTCOME_FAILED;
    }
    return GSH_EXEC_OUTCOME_INVALID;
}

static bool exec_commit_targets_valid(
    const shell_state *state, const exec_descriptor_commit *commit)
{
    size_t index;
    size_t open_count = 0;

    if (state == NULL || commit == NULL ||
        state->pending_exec_descriptor_count >
            GSH_EXEC_DESCRIPTOR_COMMIT_CAP) return false;
    if (commit->version != GSH_EXEC_DESCRIPTOR_COMMIT_VERSION ||
        commit->reserved != 0 ||
        commit->count != state->pending_exec_descriptor_count ||
        commit->count > GSH_EXEC_DESCRIPTOR_COMMIT_CAP) {
        return false;
    }
    for (index = 0; index < commit->count; index++) {
        if (commit->targets[index] !=
                state->pending_exec_descriptors[index] ||
            commit->open[index] > 1U) {
            return false;
        }
        open_count += commit->open[index];
    }
    return open_count == commit->open_count;
}

static bool exec_commit_contains_target(
    const exec_descriptor_commit *commit, int descriptor)
{
    if (commit == NULL) {
        return false;
    }
    size_t index;

    for (index = 0; index < commit->count; index++) {
        if (commit->targets[index] == descriptor) {
            return true;
        }
    }
    return false;
}

static bool exec_targets_contain(const int32_t targets[], size_t count,
                                 int descriptor)
{
    if (targets == NULL) return false;
    size_t index;

    for (index = 0; index < count; index++) {
        if (targets[index] == descriptor) {
            return true;
        }
    }
    return false;
}

static int relocate_exec_owner_fd(const int32_t targets[], size_t count,
                                  int minimum, int *descriptor)
{
    if (descriptor == NULL) return -1;
    int duplicate;

    if (*descriptor < 0 ||
        !exec_targets_contain(targets, count, *descriptor)) {
        return 0;
    }
    duplicate = gsh_fault_should_fail(GSH_FAULT_EXEC_OWNER_DESCRIPTOR_RELOCATION,
                                  EMFILE)
                    ? -1
                    : fcntl(*descriptor, F_DUPFD_CLOEXEC, minimum);
    if (duplicate == -1) {
        return -1;
    }
    (void)close(*descriptor);
    *descriptor = duplicate;
    return 0;
}

static int relocate_exec_signal_fd(const int32_t targets[], size_t count,
                                   int minimum, int *descriptor)
{
    if (descriptor == NULL) {
        return -1;
    }
    int previous = *descriptor;
    int duplicate;

    if (previous < 0 ||
        !exec_targets_contain(targets, count, previous)) {
        return 0;
    }
    duplicate = gsh_fault_should_fail(GSH_FAULT_EXEC_OWNER_DESCRIPTOR_RELOCATION,
                                  EMFILE)
                    ? -1
                    : fcntl(previous, F_DUPFD_CLOEXEC, minimum);
    if (duplicate == -1) {
        return -1;
    }
    g_signal_write_fd = duplicate;
    *descriptor = duplicate;
    (void)close(previous);
    return 0;
}

/* ── User Descriptors Never Own Reactor Resources ─────────────────
 * Interactive shells keep terminal, signal, worker, and PTY descriptors in
 * the same process that owns persistent `exec` redirections. A user target
 * can numerically collide with any of them, especially descriptors 3 and 4.
 * Before committing, matching resources move above every target with CLOEXEC;
 * the user receives the requested number while the reactor retains ownership.
 * The fixed owner and cell sets bound both work and partial-failure recovery.
 * ─────────────────────────────────────────────────────────────── */
static int protect_exec_owner_descriptors(
    shell_state *state, const int32_t targets[], size_t count)
{
    if (targets == NULL) return -1;
    if (state == NULL) {
        return -1;
    }
    int *owned[] = {
        &state->tty_fd, &state->signal_pipe[0],
        &state->redirection_worker_fd,
        &state->variable_commit_fd, &state->exec_outcome_fd,
        &state->exec_descriptor_socket, &state->directory_commit_socket,
        &state->directory_commit_fd,
        &state->completion_fd, &state->job_service_socket,
        &state->job_service_wait_reply_fd, &state->llm_repl.fd,
        &state->llm_repl_peer, &state->journal.descriptor,
    };
    int minimum = STDERR_FILENO + 1;
    size_t index;

    for (index = 0; index < count; index++) {
        if (targets[index] < 0 || targets[index] == INT_MAX) {
            errno = EINVAL;
            return -1;
        }
        if (targets[index] >= minimum) {
            minimum = targets[index] + 1;
        }
    }
    for (index = 0; index < sizeof(owned) / sizeof(owned[0]); index++) {
        if (relocate_exec_owner_fd(targets, count, minimum,
                                   owned[index]) == -1) {
            return -1;
        }
    }
    if (relocate_exec_signal_fd(targets, count, minimum,
                                &state->signal_pipe[1]) == -1) {
        return -1;
    }
    for (index = 0; state->async_repl != NULL &&
                    index < GSH_ASYNC_CELL_CAP; index++) {
        if (relocate_exec_owner_fd(
                targets, count, minimum,
                &state_async_repl(state)->cells[index].pty_fd) == -1 ||
            relocate_exec_owner_fd(
                targets, count, minimum,
                &state_async_repl(state)->cells[index].resource_fd) == -1) {
            return -1;
        }
    }
    return 0;
}

static void close_exec_commit_fds(int descriptors[], size_t count)
{
    if (descriptors == NULL) return;
    size_t index;

    for (index = 0; index < count; index++) {
        if (descriptors[index] >= 0) {
            (void)close(descriptors[index]);
            descriptors[index] = -1;
        }
    }
}

static int stabilize_exec_commit_fds(
    const exec_descriptor_commit *commit, int descriptors[], size_t count)
{
    if (descriptors == NULL) return -1;
    int reservations[GSH_EXEC_DESCRIPTOR_COMMIT_CAP];
    size_t reservation_count = 0;
    size_t index;

    for (index = 0; index < count; index++) {
        size_t attempt;

        if (!exec_commit_contains_target(commit, descriptors[index])) {
            continue;
        }
        for (attempt = 0; attempt <= GSH_EXEC_DESCRIPTOR_COMMIT_CAP;
             attempt++) {
            int duplicate =
                gsh_fault_should_fail(GSH_FAULT_EXEC_DESCRIPTOR_STABILIZE, EMFILE)
                    ? -1
                    : fcntl(descriptors[index], F_DUPFD_CLOEXEC,
                            STDERR_FILENO + 1);

            if (duplicate == -1) {
                close_exec_commit_fds(reservations, reservation_count);
                return -1;
            }
            if (!exec_commit_contains_target(commit, duplicate)) {
                (void)close(descriptors[index]);
                descriptors[index] = duplicate;
                break;
            }
            if (reservation_count == GSH_EXEC_DESCRIPTOR_COMMIT_CAP) {
                (void)close(duplicate);
                close_exec_commit_fds(reservations, reservation_count);
                errno = EMFILE;
                return -1;
            }
            reservations[reservation_count++] = duplicate;
        }
        if (attempt > GSH_EXEC_DESCRIPTOR_COMMIT_CAP) {
            close_exec_commit_fds(reservations, reservation_count);
            errno = EMFILE;
            return -1;
        }
    }
    close_exec_commit_fds(reservations, reservation_count);
    return 0;
}

static int apply_exec_descriptor_commit(
    shell_state *state, const exec_descriptor_commit *commit,
    int descriptors[])
{
    if (state == NULL) {
        return -1;
    }
    size_t index;
    size_t open_index = 0;
    int status = 0;

    if (stabilize_exec_commit_fds(
            commit, descriptors, commit->open_count) == -1) {
        close_exec_commit_fds(descriptors, commit->open_count);
        return -1;
    }
    for (index = 0; index < commit->count; index++) {
        int target = commit->targets[index];

        if (gsh_fault_should_fail(GSH_FAULT_EXEC_DESCRIPTOR_APPLY, EIO)) {
            status = -1;
            if (commit->open[index] != 0) {
                open_index++;
            }
            continue;
        }
        if (commit->open[index] != 0) {
            if (dup2(descriptors[open_index++], target) == -1) {
                status = -1;
            } else if (target <= STDERR_FILENO) {
                state->exec_standard_descriptor_changed[target] = true;
            }
        } else if (close(target) == -1 && errno != EBADF) {
            status = -1;
        } else if (target <= STDERR_FILENO) {
            state->exec_standard_descriptor_changed[target] = true;
        }
    }
    close_exec_commit_fds(descriptors, commit->open_count);
    return status;
}

static size_t receive_exec_commit_fds(
    struct msghdr *message, int descriptors[], bool *control_valid)
{
    if (control_valid == NULL || descriptors == NULL || message == NULL) {
        return 0U;
    }
    struct cmsghdr *header = CMSG_FIRSTHDR(message);
    size_t rights_count = 0;

    *control_valid = header == NULL;
    if (header == NULL || header->cmsg_level != SOL_SOCKET ||
        header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len < CMSG_LEN(0)) {
        return 0;
    }
    {
        size_t rights_bytes = header->cmsg_len - CMSG_LEN(0);

        if (rights_bytes % sizeof(int) != 0 ||
            rights_bytes / sizeof(int) >
                GSH_EXEC_DESCRIPTOR_COMMIT_CAP) {
            return 0;
        }
        rights_count = rights_bytes / sizeof(int);
        if (rights_count == 0) {
            return 0;
        }
        (void)memcpy(descriptors, CMSG_DATA(header), rights_bytes);
    }
    *control_valid = CMSG_NXTHDR(message, header) == NULL;
    return rights_count;
}

static int receive_exec_descriptor_commit(shell_state *state)
{
    if (state == NULL) return -1;
    exec_descriptor_commit commit;
    int descriptors[GSH_EXEC_DESCRIPTOR_COMMIT_CAP];
    unsigned char control[
        CMSG_SPACE(sizeof(int) * GSH_EXEC_DESCRIPTOR_COMMIT_CAP)];
    struct iovec payload = {&commit, sizeof(commit)};
    struct msghdr message;
    ssize_t received;
    size_t rights_count = 0;
    int receive_error = EIO;
    bool control_valid;

    if (state->exec_descriptor_socket < 0) {
        return 0;
    }
    (void)memset(&commit, 0, sizeof(commit));
    (void)memset(descriptors, -1, sizeof(descriptors));
    (void)memset(control, 0, sizeof(control));
    (void)memset(&message, 0, sizeof(message));
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    if (gsh_fault_should_fail(GSH_FAULT_EXEC_DESCRIPTOR_RECEIVE, EIO)) {
        received = -1;
    } else {
        received = recvmsg(state->exec_descriptor_socket, &message,
                           MSG_DONTWAIT);
        if (received == -1) {
            receive_error = errno;
        }
    }
    (void)close(state->exec_descriptor_socket);
    state->exec_descriptor_socket = -1;
    if (received == -1 &&
        (receive_error == EAGAIN || receive_error == EWOULDBLOCK)) {
        return 0;
    }
    control_valid = false;
    if (received >= 0) {
        rights_count = receive_exec_commit_fds(
            &message, descriptors, &control_valid);
    }
    if (received != (ssize_t)sizeof(commit) ||
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        !exec_commit_targets_valid(state, &commit) ||
        rights_count != commit.open_count ||
        !control_valid) {
        close_exec_commit_fds(descriptors, rights_count);
        return -1;
    }
    if (protect_exec_owner_descriptors(
            state, commit.targets, commit.count) == -1) {
        close_exec_commit_fds(descriptors, rights_count);
        return -1;
    }
    return apply_exec_descriptor_commit(state, &commit, descriptors);
}

static void finish_job_auto_help(shell_state *state, bool completed_llm,
                                 bool status_known, int wait_status)
{
    if (state == NULL) return;
    state->current_job_llm = false;
    if (completed_llm || !status_known || !WIFEXITED(wait_status))
        state->auto_help_eligible = false;
}

static void finish_job(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    bool was_foreground = state->current_job.foreground;
    pid_t pid = state->current_job.pid;
    int wait_status = 0;
    bool pipeline_status_known = gsh_pipeline_status_result(
        &state->current_job.pipeline_status, &wait_status);
    int status = pipeline_status_known ? wait_status_value(wait_status) : 1;
    bool silent = state->current_job.silent;
    bool completed_llm = state->current_job_llm;
    int exec_outcome = finish_exec_outcome(state);
    bool overlaid = exec_outcome == GSH_EXEC_OUTCOME_OVERLAID;
    bool current_environment_exit = false;
    int descriptor_commit_status =
        overlaid ? 0 : receive_exec_descriptor_commit(state);
    const gsh_background_entry *tracked =
        gsh_background_entry_for_pid(&state->background_jobs, pid);

    if (overlaid) {
        close_variable_commit(state);
    } else if (pipeline_status_known) {
        bool commit_active = state->variable_commit_active;

        if (!finish_variable_commit(state, wait_status)) {
            status = 125;
        } else if (commit_active && state->committed_exit_requested) {
            current_environment_exit = true;
            status = state->committed_exit_status;
        }
    }
    if (exec_outcome == GSH_EXEC_OUTCOME_INVALID ||
        descriptor_commit_status == -1) {
        if (descriptor_commit_status == -1) {
            (void)output_text(state,
                        "gsh: exec descriptor transaction rejected\r\n");
        }
        status = 125;
    }

    state->current_job.active = false;
    state->current_job.foreground = false;
    state->current_job.stopped = false;
    finish_job_auto_help(state, completed_llm, pipeline_status_known,
                         wait_status);
    if (state->current_job.negated && !current_environment_exit) {
        status = status == 0 ? 1 : 0;
    }
    state->last_status = status;
    if (was_foreground && tracked != NULL) {
        (void)gsh_background_remove_job(&state->background_jobs,
                                        tracked->job_id);
    }

    if (state->async_repl != NULL && state_async_repl(state)->enabled &&
        state->async_state_cell >= 0) {
        leave_managed_fullscreen(state, state->async_state_cell);
        (void)gsh_async_repl_reap(state->async_repl, pid,
            status == wait_status_value(wait_status) ? wait_status : status << 8);
    }

    if (overlaid || current_environment_exit) {
        if (was_foreground) {
            reclaim_terminal(state, false);
        }
        abandon_pending_list(state);
        state->running = false;
        return;
    }

    if (was_foreground) {
        reclaim_terminal(state, false);
        if (pipeline_status_known && WIFSIGNALED(wait_status)) {
            (void)output_text(state, "\r\n");
            if (WTERMSIG(wait_status) != SIGINT) {
                output_format(state, "[terminated by signal %d]\r\n",
                              WTERMSIG(wait_status));
            }
        }
        schedule_pending_continuation(state);
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
    if (state == NULL) return;
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
        uint32_t job_id = 0;

        state->current_job.member_states[member] = JOB_MEMBER_STOPPED;
        if (!all_remaining_members_stopped(&state->current_job) ||
            state->current_job.stopped) {
            return;
        }
        was_foreground = state->current_job.foreground;
        state->current_job.stopped = true;
        state->current_job.foreground = false;
        state->last_status = 128 + WSTOPSIG(status);
        {
            const gsh_background_entry *tracked =
                gsh_background_entry_for_pid(&state->background_jobs,
                                             state->current_job.pgid);

            if (tracked != NULL) {
                job_id = tracked->job_id;
            } else if (gsh_background_add_job(
                           &state->background_jobs,
                           state->current_job.pgid,
                           state->current_job.status_pid,
                           state->current_job.members,
                           state->current_job.member_count,
                           state->current_job.pipeline_status.command_count,
                           state->current_job.pipeline_status.pipefail,
                           state->pending_input,
                           state->pending_input_length,
                           GSH_JOB_ORIGIN_CLASSIC, &job_id) == -1) {
                (void)output_text(state,
                            "gsh: stopped job registry exhausted\r\n");
            } else {
                gsh_background_entry *entry =
                    gsh_background_mutable_entry_for_id(
                        &state->background_jobs, job_id);

                if (entry != NULL) {
                    (void)memcpy(entry->member_states,
                           state->current_job.member_states,
                           state->current_job.member_count *
                               sizeof(entry->member_states[0]));
                    entry->remaining = state->current_job.remaining;
                    entry->pipeline_status =
                        state->current_job.pipeline_status;
                    entry->state = GSH_JOB_STOPPED;
                }
            }
        }
        if (was_foreground) {
            reclaim_terminal(state, true);
            output_format(state, "\r\n[%u]+ Stopped %s\r\n", job_id,
                          state->pending_input);
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
        if (member < state->current_job.pipeline_status.command_count) {
            (void)gsh_pipeline_status_record(
                &state->current_job.pipeline_status, member, status);
        }
        if (state->current_job.remaining == 0) {
            finish_job(state);
        }
    }
}

static void reap_redirection_worker(shell_state *state)
{
    bool unexpected;
    bool command;
    bool restart;

    if (!require(state != NULL)) return;
    if (!require(state->redirection_worker_pid > 0)) return;
    unexpected = state->redirection_worker_alive;
    command = state->mode == MODE_ASYNC_REDIRECTION;
    restart = state->redirection_worker_restart_pending;
    disable_redirection_worker(state, false);
    state->redirection_worker_pid = -1;
    state->redirection_worker_restart_pending = false;
    if (unexpected) state->redirection_worker_failures++;
    if (command) {
        (void)output_text(state, "gsh: asynchronous redirection worker exited\r\n");
        state->last_status = 1;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
    }
    if (restart && state->running &&
        start_redirection_worker(state) == -1) {
        state->redirection_worker_failures++;
    }
}

static void requeue_child_signal(shell_state *state)
{
    unsigned char byte = (unsigned char)SIGCHLD;
    ssize_t notified;

    if (!require(state != NULL)) return;
    if (!require(state->signal_pipe[1] >= 0)) return;
    g_sigchld_pending = 1;
    notified = write(state->signal_pipe[1], &byte, sizeof(byte));
    (void)notified;
}

static bool reap_completion_child(shell_state *state, pid_t pid, int status)
{
    if (!require(state != NULL)) return false;
    if (!require(pid > 0)) return false;
    if (pid != state->completion_pid) return false;
    if (WIFEXITED(status) || WIFSIGNALED(status))
        state->completion_pid = -1;
    return true;
}

static void capture_llm_context(shell_state *state, int cell_index)
{
    gsh_async_cell *cell;

    if (state == NULL || cell_index < 0 ||
        cell_index >= GSH_ASYNC_CELL_CAP) return;
    cell = &state_async_repl(state)->cells[cell_index];
    cell->ai_context_id = cell->id;
    cell->ai_directory_generation =
        gsh_variables_value_generation(state->variables, "PWD", 3U);
}

static void enqueue_managed_error_help(shell_state *state, int cell_index,
                                       int status)
{
    char active_prompt[GSH_ASYNC_PROMPT_CAP];
    char prompt[GSH_ASYNC_COMMAND_CAP];
    const gsh_async_cell *cell;
    size_t output_begin;
    int length;

    if (state == NULL || cell_index < 0 || cell_index >= GSH_ASYNC_CELL_CAP ||
        status == 0 || !state->config.llm_enabled ||
        !state->config.llm_auto_help) return;
    cell = &state_async_repl(state)->cells[cell_index];
    if (cell->ai || cell->ai_request_id != 0U ||
        cell->state == GSH_ASYNC_CANCELLED ||
        (cell->command_length >= 2U && cell->command[0] == ' ' &&
         cell->command[cell->command_length - 1U] == ' ')) return;
    output_begin = cell->output_length > 2048U
                       ? cell->output_length - 2048U : 0U;
    length = snprintf(
        prompt, sizeof(prompt),
        "? A top-level gsh command failed with exit status %d. Diagnose it and "
        "suggest the smallest safe fix. Do not run commands unless needed.\n\n"
        "Command:\n%.*s\n\nLast output:\n%.*s",
        status, (int)cell->command_length, cell->command,
        (int)(cell->output_length - output_begin), cell->output + output_begin);
    if (length <= 0 || (size_t)length >= sizeof(prompt)) return;
    (void)active_prompt_text(state, active_prompt);
    capture_llm_context(state, gsh_async_repl_accept_ai(
        state->async_repl, active_prompt, prompt, (size_t)length,
        cell->launch_directory));
}

static void record_managed_journal_output(shell_state *state, int cell_index,
                                          bool ai)
{
    const gsh_async_cell *cell;
    bool private;

    if (state == NULL || cell_index < 0 || cell_index >= GSH_ASYNC_CELL_CAP ||
        ai || !state->config.llm_enabled) return;
    cell = &state_async_repl(state)->cells[cell_index];
    private = cell->ai_private || (state->config.history_ignore_space &&
              cell->command_length >= 2U && cell->command[0] == ' ' &&
              cell->command[cell->command_length - 1U] == ' ');
    if (!private && cell->output_length != 0U)
        enqueue_journal_record(
            state, GSH_LLM_JOURNAL_COMMAND_OUTPUT,
            cell->output, cell->output_length);
}

static void reap_finished_managed_cell(shell_state *state, int cell_index,
                                       pid_t pid, int status,
                                       uint32_t tracked_job_id)
{
    bool cancelled;
    bool ai;
    int exit_status;

    if (state == NULL || cell_index < 0 ||
        cell_index >= GSH_ASYNC_CELL_CAP) return;
    cancelled = state_async_repl(state)->cells[cell_index].state ==
                GSH_ASYNC_CANCELLED;
    ai = state_async_repl(state)->cells[cell_index].ai;
    exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
    leave_managed_fullscreen(state, cell_index);
    (void)gsh_async_repl_reap(state->async_repl, pid, status);
    record_managed_journal_output(state, cell_index, ai);
    if (!cancelled && !ai && WIFEXITED(status) && exit_status != 0)
        enqueue_managed_error_help(state, cell_index, exit_status);
    if (tracked_job_id != 0U)
        (void)gsh_background_mark_notified(&state->background_jobs,
                                           tracked_job_id);
}

static bool reap_journal_worker(shell_state *state, pid_t pid, int status)
{
    if (state == NULL || pid <= 0 || pid != state->journal_pid) return false;
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        state->journal_pid = -1;
        close_journal_pipe(state);
        report_journal_failure(state);
    } else if (WIFSTOPPED(status)) {
        (void)kill(pid, SIGCONT);
    }
    return true;
}

static void reap_children(shell_state *state)
{
    if (state == NULL) {
        return;
    }
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

            if (reap_journal_worker(state, pid, status)) {
            } else if (pid == state->redirection_worker_pid) {
                reap_redirection_worker(state);
            } else if (reap_completion_child(state, pid, status)) {
            } else if (state->current_job.active &&
                       find_job_member(&state->current_job, pid) !=
                           GSH_NATIVE_JOB_MEMBER_CAP) {
                (void)gsh_background_update_member(
                    &state->background_jobs, pid, status);
                update_job_state(state, pid, status);
            } else if (async_cell >= 0) {
                const gsh_background_entry *tracked;
                uint32_t tracked_job_id;

                (void)gsh_background_update_member(
                    &state->background_jobs, pid, status);
                tracked = gsh_background_entry_for_pid(
                    &state->background_jobs, pid);
                tracked_job_id = tracked == NULL ? 0U : tracked->job_id;
                if (WIFSTOPPED(status)) {
                    leave_managed_fullscreen(state, async_cell);
                    gsh_async_repl_mark_stopped(state->async_repl,
                                                async_cell);
#ifdef WIFCONTINUED
                } else if (WIFCONTINUED(status)) {
                    gsh_async_repl_mark_running(state->async_repl,
                                                async_cell);
#endif
                } else if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    reap_finished_managed_cell(state, async_cell, pid, status,
                                               tracked_job_id);
                }
            } else if (gsh_background_update_member(
                           &state->background_jobs, pid, status)) {
                const gsh_background_entry *tracked =
                    gsh_background_entry_for_pid(
                        &state->background_jobs, pid);

                if (tracked != NULL && WIFSTOPPED(status)) {
                    output_format(state, "\r\n[%u]%c Stopped %s\r\n",
                                  tracked->job_id,
                                  gsh_background_marker(
                                      &state->background_jobs,
                                      tracked->job_id),
                                  tracked->command_length == 0
                                      ? "(command)" : tracked->command);
                    (void)gsh_background_mark_notified(
                        &state->background_jobs, tracked->job_id);
                    if (state->mode == MODE_EDITOR) queue_redraw(state);
                } else if (tracked != NULL &&
                           tracked->state == GSH_JOB_DONE &&
                           state->mode != MODE_WAIT &&
                           gsh_options_enabled(&state->options,
                                               GSH_OPTION_NOTIFY)) {
                    emit_job_notification(state, tracked);
                    if (state->mode == MODE_EDITOR) queue_redraw(state);
                }
                (void)finish_background_wait(state);
            }
            (void)finish_job_service_wait(state);
            continue;
        }
        if (pid == -1 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (count == MAX_SIGNAL_REAPS) {
        requeue_child_signal(state);
    }
}

static void resize_managed_jobs(shell_state *state)
{
    if (state == NULL) return;
    struct winsize size;
    int index;

    if (state->async_repl == NULL || !state_async_repl(state)->enabled) {
        return;
    }
    (void)memset(&size, 0, sizeof(size));
    if (ioctl(state->tty_fd, TIOCGWINSZ, &size) == -1) {
        return;
    }
    gsh_async_repl_resize(state->async_repl, size.ws_row, size.ws_col);
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        gsh_async_cell *cell = &state_async_repl(state)->cells[index];

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
    if (state == NULL) {
        return;
    }
    unsigned char bytes[256];
    ssize_t drained;

    drained = read(state->signal_pipe[0], bytes, sizeof(bytes));
    (void)drained;
}

/* ── One Editor State Machine Owns Recall and Search ────────────
 * Escape sequences were previously consumed without changing the editor.
 * Recall now saves the draft once, then moves through a newest-first ring.
 * Incremental search keeps a separate query and draft while showing a match.
 * Accepting a match returns it to ordinary editing; cancellation restores text.
 * All copies share the 4096-byte input bound and never allocate while typing.
 * ─────────────────────────────────────────────────────────────── */
static size_t available_history(const shell_state *state)
{
    if (state == NULL) return 0U;
    size_t available;

    if (state->history == NULL) {
        return 0;
    }
    available = state_history(state)->count;
    if (available > state->config.history_max_entries) {
        available = state->config.history_max_entries;
    }
    return available;
}

static bool load_history_position(shell_state *state, size_t position)
{
    if (state == NULL) {
        return false;
    }
    size_t length = 0;
    const char *entry = gsh_history_from_newest(state->history, position,
                                                &length);

    if (entry == NULL || length >= sizeof(state->line)) {
        return false;
    }
    (void)memcpy(state->line, entry, length);
    state->line[length] = '\0';
    state->line_len = length;
    state->line_cursor = length;
    return true;
}

static void reset_history_editor(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    state->history_navigation = false;
    state->history_position = 0;
    state->history_draft_length = 0;
    state->history_draft[0] = '\0';
    state->history_search = false;
    state->history_search_query_length = 0;
    state->history_search_query[0] = '\0';
    state->history_search_position = 0;
    state->history_search_draft_length = 0;
    state->history_search_draft[0] = '\0';
}

static void history_previous(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    size_t available = available_history(state);

    if (available == 0) {
        (void)output_text(state, "\a");
        return;
    }
    if (!state->history_navigation) {
        (void)memcpy(state->history_draft, state->line, state->line_len + 1U);
        state->history_draft_length = state->line_len;
        state->history_position = 0;
        state->history_navigation = true;
    } else if (state->history_position + 1U < available) {
        state->history_position++;
    } else {
        (void)output_text(state, "\a");
    }
    (void)load_history_position(state, state->history_position);
    queue_redraw(state);
}

static void history_next(shell_state *state)
{
    if (state == NULL) return;
    if (!state->history_navigation) {
        (void)output_text(state, "\a");
        return;
    }
    if (state->history_position != 0) {
        state->history_position--;
        (void)load_history_position(state, state->history_position);
    } else {
        (void)memcpy(state->line, state->history_draft,
               state->history_draft_length + 1U);
        state->line_len = state->history_draft_length;
        state->line_cursor = state->line_len;
        state->history_navigation = false;
    }
    queue_redraw(state);
}

static bool find_history_match(shell_state *state, size_t before)
{
    if (state == NULL) return false;
    size_t position;

    if (gsh_history_search_reverse(
            state->history, state->history_search_query,
            state->history_search_query_length, before, &position) == -1 ||
        position >= available_history(state) ||
        !load_history_position(state, position)) {
        (void)output_text(state, "\a");
        return false;
    }
    state->history_search_position = position;
    return true;
}

static void search_history(shell_state *state)
{
    size_t before = 0;

    if (available_history(state) == 0) {
        (void)output_text(state, "\a");
        return;
    }
    if (!state->history_search) {
        (void)memcpy(state->history_search_draft, state->line,
               state->line_len + 1U);
        state->history_search_draft_length = state->line_len;
        state->history_search_query_length = 0;
        state->history_search_query[0] = '\0';
        state->history_search = true;
    } else {
        before = state->history_search_position + 1U;
    }
    (void)find_history_match(state, before);
    queue_redraw(state);
}

static void update_history_search(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    (void)find_history_match(state, 0);
    queue_redraw(state);
}

static void cancel_history_search(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    (void)memcpy(state->line, state->history_search_draft,
           state->history_search_draft_length + 1U);
    state->line_len = state->history_search_draft_length;
    state->line_cursor = state->line_len;
    state->history_search = false;
    state->history_search_query_length = 0;
    state->history_search_query[0] = '\0';
    queue_redraw(state);
}

static void accept_history_search(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    state->history_search = false;
    state->history_search_query_length = 0;
    state->history_search_query[0] = '\0';
    state->history_navigation = false;
    queue_redraw(state);
}

static void cancel_editor_line(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    finish_classic_editor(state);
    state->line_len = 0;
    state->line_cursor = 0U;
    state->line[0] = '\0';
    state->pending_len = 0;
    state->pending_line[0] = '\0';
    reset_pending_input(state);
    state->continuation_prompt = false;
    state->escape_state = 0;
    state->editor_sequence_length = 0U;
    state->bracketed_paste = false;
    state->paste_end_match = 0U;
    state->paste_last_was_cr = false;
    state->paste_overflow_reported = false;
    reset_history_editor(state);
    if (state->async_repl == NULL || !state_async_repl(state)->enabled) {
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
    if (state == NULL) {
        return -1;
    }
    gsh_async_cell *cell = &state_async_repl(state)->cells[cell_index];
    pid_t foreground = cell->pty_fd < 0 ? -1 : tcgetpgrp(cell->pty_fd);

    if (foreground > 0) {
        cell->pgid = foreground;
    }
    return cell->pgid;
}

static int signal_managed_job(shell_state *state, int cell_index,
                              int signal_number)
{
    if (state == NULL) {
        return -1;
    }
    gsh_async_cell *cell = &state_async_repl(state)->cells[cell_index];
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
        const gsh_background_entry *entry =
            gsh_background_entry_for_pid(
                &state->background_jobs,
                state_async_repl(state)->cells[cell_index].pid);

        if (entry != NULL) {
            (void)gsh_background_stop_job(&state->background_jobs,
                                          entry->job_id);
        }
        gsh_async_repl_mark_stopped(state->async_repl, cell_index);
    } else {
        gsh_async_repl_unfocus(state->async_repl);
    }
}

typedef struct {
    bool child;
    bool interrupt;
    bool suspend;
    bool resize;
    bool shutdown;
} pending_signal_batch;

static void take_pending_signals(pending_signal_batch *pending)
{
    sigset_t signals;
    sigset_t previous;

    if (!require(pending != NULL)) return;
    if (!require(g_signal_write_fd >= -1)) return;
    (void)sigemptyset(&signals);
    (void)sigaddset(&signals, SIGCHLD);
    (void)sigaddset(&signals, SIGINT);
    (void)sigaddset(&signals, SIGTSTP);
    (void)sigaddset(&signals, SIGWINCH);
    (void)sigaddset(&signals, SIGHUP);
    (void)sigaddset(&signals, SIGTERM);
    (void)sigprocmask(SIG_BLOCK, &signals, &previous);
    pending->child = g_sigchld_pending != 0;
    pending->interrupt = g_sigint_pending != 0;
    pending->suspend = g_sigtstp_pending != 0;
    pending->resize = g_sigwinch_pending != 0;
    pending->shutdown = g_shutdown_pending != 0;
    g_sigchld_pending = 0;
    g_sigint_pending = 0;
    g_sigtstp_pending = 0;
    g_sigwinch_pending = 0;
    g_shutdown_pending = 0;
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
}

static void process_pending_signals(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    pending_signal_batch pending = {0};

    take_pending_signals(&pending);
    if (pending.child) {
        reap_children(state);
    }
    if (pending.interrupt && state->async_repl != NULL &&
        state_async_repl(state)->enabled) {
        int focused = gsh_async_repl_focused_job(state->async_repl);

        if (focused >= 0) {
            (void)signal_managed_job(state, focused, SIGINT);
        } else {
            cancel_editor_line(state);
        }
    } else if (pending.interrupt && state->mode == MODE_EDITOR) {
        cancel_editor_line(state);
    } else if (pending.interrupt && state->mode == MODE_DISPATCH) {
        state->mode = MODE_EDITOR;
        state->pending_line[0] = '\0';
        cancel_editor_line(state);
    } else if (pending.interrupt &&
               state->mode == MODE_ASYNC_REDIRECTION) {
        disable_redirection_worker(state, true);
        state->redirection_worker_restart_pending = true;
        state->last_status = 130;
        state->mode = MODE_EDITOR;
        state->redirection_target[0] = '\0';
        abandon_pending_list(state);
        cancel_editor_line(state);
    } else if (pending.interrupt && state->mode == MODE_WAIT) {
        state->wait_target_count = 0;
        state->wait_all = false;
        state->last_status = 130;
        state->mode = MODE_EDITOR;
        state->wait_negated = false;
        abandon_pending_list(state);
        cancel_editor_line(state);
    }
    if (pending.resize) {
        if (state->async_repl != NULL && state_async_repl(state)->enabled) {
            resize_managed_jobs(state);
        } else if (!pending.interrupt && state->mode == MODE_EDITOR) {
            /* Ctrl-C already emitted a complete prompt for the empty line.
             * Folding a simultaneous resize into that frame prevents a
             * second prompt from escaping after the caller synchronized. */
            queue_redraw(state);
        }
    }
    if (pending.suspend && state->async_repl != NULL &&
        state_async_repl(state)->enabled) {
        int focused = gsh_async_repl_focused_job(state->async_repl);

        if (focused >= 0) {
            stop_managed_job(state, focused);
        }
    }
    if (pending.shutdown) {
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
        "getopts", "hash",   "help",     "history",  "jobs",
        "kill",   "ll",     "ls",
        "printf", "pwd",     "read",     "readonly", "return",
        "rt",     "set",     "shift",    "test",     "times",
        "trap",   "true",    "type",
        "ulimit", "umask",   "unalias",  "unset",    "view",
        "wait",
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
    if (command == NULL) return false;
    if (line == NULL || storage == NULL) {
        return false;
    }
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

    (void)memcpy(storage, line, length + 1);
    gsh_lexer_init(&lexer, line, length);
    for (index = 0; index <= SIMPLE_ARG_CAP; index++) {
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
    if (index > SIMPLE_ARG_CAP) return false;
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
    if (text == NULL) {
        return 0U;
    }
    size_t length = 0;

    while (length < limit && text[length] != '\0') {
        length++;
    }
    return length;
}

static bool child_string_contains(const char *text, char wanted)
{
    if (text == NULL) {
        return false;
    }
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
    if (destination == NULL || source == NULL) {
        return;
    }
    size_t index;

    for (index = 0; index < length; index++) {
        destination[index] = source[index];
    }
}

static void child_write_text(const char *text)
{
    if (text == NULL) {
        return;
    }
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

static int write_exec_error(const char *name, int error)
{
    if (name == NULL) {
        return -1;
    }
    child_write_text("gsh: ");
    child_write_text(name);
    if (error == ENOENT || error == ENOTDIR) {
        child_write_text(": command not found\n");
        return 127;
    }
    if (error == EACCES) {
        child_write_text(": permission denied\n");
    } else {
        child_write_text(": execution failed\n");
    }
    return 126;
}

_Noreturn static void child_exec_error(const char *name, int error)
{
    if (name == NULL) {
        _exit(125);
    }
    _exit(write_exec_error(name, error));
}

static int resolve_launch_candidate(const char *path)
{
    shell_executable_identity *identity = shell_executable_storage();

    if (!require(identity != NULL)) return -1;
    if (access(path, X_OK) == -1 ||
        realpath(path, identity->path) == NULL) {
        return -1;
    }
    return 0;
}

static int resolve_launch_argument(const char *argument_zero)
{
    const char *path;
    const char *cursor;
    size_t name_length;
    size_t scanned = 0;

    if (argument_zero == NULL || argument_zero[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (strchr(argument_zero, '/') != NULL) {
        return resolve_launch_candidate(argument_zero);
    }
    path = getenv("PATH");
    cursor = path == NULL ? "/bin:/usr/bin" : path;
    name_length = strnlen(argument_zero, EXEC_PATH_CAP);
    if (name_length == EXEC_PATH_CAP) {
        errno = ENAMETOOLONG;
        return -1;
    }
    while (scanned <= PATH_SCAN_CAP) {
        char candidate[EXEC_PATH_CAP];
        size_t available = PATH_SCAN_CAP - scanned;
        size_t remaining = strnlen(cursor, available + 1U);
        const char *separator;
        size_t directory_length;
        size_t offset;

        if (remaining > available) {
            errno = E2BIG;
            return -1;
        }
        separator = memchr(cursor, ':', remaining);
        directory_length = separator == NULL
                               ? remaining
                               : (size_t)(separator - cursor);
        offset = directory_length == 0 ? 1U : directory_length;
        scanned += directory_length + (separator == NULL ? 0U : 1U);
        if (offset + 1U + name_length + 1U <= sizeof(candidate)) {
            if (directory_length == 0) {
                candidate[0] = '.';
            } else {
                (void)memcpy(candidate, cursor, directory_length);
            }
            candidate[offset++] = '/';
            (void)memcpy(candidate + offset, argument_zero, name_length + 1U);
            if (resolve_launch_candidate(candidate) == 0) {
                return 0;
            }
        }
        if (separator == NULL) {
            break;
        }
        cursor = separator + 1U;
    }
    errno = ENOENT;
    return -1;
}

static int initialize_shell_executable(const char *argument_zero)
{
    if (argument_zero == NULL) {
        return -1;
    }
    shell_executable_identity *identity = shell_executable_storage();
    int resolved = -1;
    struct stat information;

    if (!require(identity != NULL)) return -1;

#if defined(__APPLE__)
    char candidate[EXEC_PATH_CAP];
    uint32_t capacity = (uint32_t)sizeof(candidate);

    if (_NSGetExecutablePath(candidate, &capacity) == 0 &&
        resolve_launch_candidate(candidate) == 0) {
        resolved = 0;
    }
#elif defined(__linux__)
    ssize_t length = readlink("/proc/self/exe", identity->path,
                              sizeof(identity->path) - 1U);

    if (length > 0 && (size_t)length < sizeof(identity->path) - 1U) {
        identity->path[length] = '\0';
        resolved = 0;
    }
#endif
    if (resolved == -1) {
        resolved = resolve_launch_argument(argument_zero);
    }
    if (resolved == -1 || stat(identity->path, &information) == -1 ||
        !S_ISREG(information.st_mode)) {
        return -1;
    }
    identity->device = information.st_dev;
    identity->inode = information.st_ino;
    return 0;
}

/* ── ENOEXEC Re-enters the Native Shell ────────────────────────
 * POSIX requires an executable text file without a recognized image format
 * to be interpreted as a shell script. Re-execing this process through its
 * initialization-time canonical path preserves the child PID, process group,
 * descriptors, environment, script `$0`, and operands while ensuring every
 * command still passes through gsh's bounded first-party evaluator.
 * CANON-EXCEPTION: C-PROCESS-ABI applies only to null-terminated argv/envp.
 * The open descriptor and two identity checks prevent an already-replaced
 * interpreter from being selected before the final path-based execve.
 * ─────────────────────────────────────────────────────────────── */
static void child_exec_script(const char *path, char *const arguments[],
                              char *const environment[])
{
    if (arguments == NULL || environment == NULL || path == NULL) {
        return;
    }
    const shell_executable_identity *identity = shell_executable_storage();
    char *shell_arguments[SIMPLE_ARG_CAP + 2];
    struct stat information;
    struct stat current;
    size_t index = 1;
    int descriptor;
    int error;

    if (!require(identity != NULL)) return;
    shell_arguments[0] = (char *)identity->path;
    shell_arguments[1] = (char *)path;
    while (arguments[index] != NULL && index < SIMPLE_ARG_CAP) {
        shell_arguments[index + 1] = arguments[index];
        index++;
    }
    shell_arguments[index + 1] = NULL;
    descriptor = gsh_fault_should_fail(GSH_FAULT_ENOEXEC_INTERPRETER_OPEN, EIO)
                     ? -1
                     : open(identity->path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return;
    }
    if (fstat(descriptor, &information) == -1) {
        error = errno;
        (void)close(descriptor);
        errno = error;
        return;
    }
    if (information.st_dev != identity->device ||
        information.st_ino != identity->inode) {
        error = EIO;
        (void)close(descriptor);
        errno = error;
        return;
    }
    if (stat(identity->path, &current) == -1) {
        error = errno;
        (void)close(descriptor);
        errno = error;
        return;
    }
    if (current.st_dev != information.st_dev ||
        current.st_ino != information.st_ino) {
        error = EIO;
        (void)close(descriptor);
        errno = error;
        return;
    }
    execve(identity->path, shell_arguments, environment);
    error = errno;
    (void)close(descriptor);
    errno = error;
}

static void child_try_exec(const char *path, char *const arguments[],
                           char *const environment[])
{
    if (arguments == NULL || environment == NULL || path == NULL) {
        return;
    }
    execve(path, arguments, environment);
    if (errno == ENOEXEC) {
        child_exec_script(path, arguments, environment);
    }
}

static int exec_direct_error(char *const arguments[], const char *path_value,
                             char *const environment[],
                             const gsh_command_cache *cache,
                             uint64_t path_generation, bool cacheable)
{
    if (arguments == NULL || cache == NULL || environment == NULL || path_value == NULL) {
        return -1;
    }
    const char *name = arguments[0];
    size_t name_length = child_string_length(name, EXEC_PATH_CAP);
    const char *cursor;
    bool access_denied = false;

    if (name_length == 0 || name_length == EXEC_PATH_CAP) {
        return ENAMETOOLONG;
    }
    if (child_string_contains(name, '/')) {
        child_try_exec(name, arguments, environment);
        return errno;
    }
    if (cacheable) {
        const char *cached = gsh_command_cache_lookup(
            cache, path_generation, name);

        if (cached != NULL) {
            child_try_exec(cached, arguments, environment);
            if (errno == EACCES) {
                access_denied = true;
            } else if (errno != ENOENT && errno != ENOTDIR) {
                return errno;
            }
        }
    }

    cursor = path_value;
    for (size_t component = 0; component <= PATH_SCAN_CAP; component++) {
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
            return ENAMETOOLONG;
        }
        if (offset + 1 + name_length + 1 > sizeof(candidate)) {
            return ENAMETOOLONG;
        }
        candidate[offset++] = '/';
        child_copy_bytes(candidate + offset, name, name_length + 1U);

        child_try_exec(candidate, arguments, environment);
        error = errno;
        if (error == EACCES) {
            access_denied = true;
        } else if (error != ENOENT && error != ENOTDIR) {
            return error;
        }
        if (*separator == '\0') {
            return access_denied ? EACCES : ENOENT;
        }
        cursor = separator + 1;
    }
    return ENAMETOOLONG;
}

static void child_exec_direct(char *const arguments[], const char *path_value,
                              char *const environment[],
                              const gsh_command_cache *cache,
                              uint64_t path_generation, bool cacheable)
{
    if (arguments == NULL || cache == NULL || environment == NULL || path_value == NULL) {
        return;
    }
    int error = exec_direct_error(arguments, path_value, environment, cache,
                                  path_generation, cacheable);

    child_exec_error(arguments[0], error);
}

static bool direct_path_is_bounded(const simple_command *command,
                                   const char *path_value)
{
    if (!require(command != NULL && path_value != NULL)) return false;
    if (!require(command->argc > 0U && command->argv[0] != NULL)) {
        return false;
    }
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
    for (size_t component = 0; component <= PATH_SCAN_CAP; component++) {
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
    return false;
}

static bool native_stateless_builtin(const gsh_native_command *command,
                                     int *status)
{
    if (command == NULL) return false;
    if (status == NULL) {
        return false;
    }
    const gsh_builtin_descriptor *descriptor;
    const char *name;
    size_t length;

    if (command->argc == 0) {
        return false;
    }
    name = command->argv[0];
    if (name[0] != ':' && name[0] != 't' && name[0] != 'f') {
        return false;
    }
    length = strlen(name);
    if (!((length == 1U && name[0] == ':') ||
          (length == 4U && name[0] == 't') ||
          (length == 5U && name[0] == 'f'))) {
        return false;
    }
    descriptor = gsh_builtin_lookup(name, length);
    if (descriptor == NULL || !descriptor->implemented) {
        return false;
    }
    if (descriptor->kind == GSH_BUILTIN_COLON ||
        descriptor->kind == GSH_BUILTIN_TRUE) {
        *status = 0;
        return true;
    }
    if (descriptor->kind == GSH_BUILTIN_FALSE) {
        *status = 1;
        return true;
    }
    return false;
}

static gsh_builtin_kind native_pure_kind(
    const gsh_native_command *command)
{
    const gsh_builtin_descriptor *descriptor;
    const char *name;
    size_t length;

    if (command == NULL || command->argc == 0) {
        return GSH_BUILTIN_NONE;
    }
    name = command->argv[0];
    if (name[0] != ':' && name[0] != '[' && name[0] != 'e' &&
        name[0] != 't' && name[0] != 'f' && name[0] != 'p') {
        return GSH_BUILTIN_NONE;
    }
    length = strlen(name);
    if (!((length == 1U && (name[0] == ':' || name[0] == '[')) ||
          (length == 4U && (name[0] == 'e' || name[0] == 't')) ||
          (length == 5U && name[0] == 'f') ||
          (length == 6U && name[0] == 'p'))) {
        return GSH_BUILTIN_NONE;
    }
    descriptor = gsh_builtin_lookup(name, length);
    return descriptor != NULL && descriptor->implemented &&
                   descriptor->execution_class == GSH_BUILTIN_PURE
               ? descriptor->kind
               : GSH_BUILTIN_NONE;
}

static bool native_pure_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    gsh_builtin_kind kind = native_pure_kind(command);

    return kind != GSH_BUILTIN_NONE && kind != GSH_BUILTIN_OTHER;
}

static int run_native_pure_builtin(const gsh_native_command *command,
                                   const gsh_builtin_io *io)
{
    if (command == NULL || io == NULL) {
        return -1;
    }
    return gsh_builtin_run_pure(native_pure_kind(command), command->argc,
                                command->argv, io);
}

static bool native_colon_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], ":") == 0;
}

static bool native_pwd_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "pwd") == 0;
}

static bool native_cd_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "cd") == 0;
}

static bool native_environment_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 &&
           (strcmp(command->argv[0], "ulimit") == 0 ||
            strcmp(command->argv[0], "umask") == 0);
}

static bool native_variable_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 &&
           (strcmp(command->argv[0], "export") == 0 ||
            strcmp(command->argv[0], "readonly") == 0 ||
            strcmp(command->argv[0], "unset") == 0);
}

static bool native_state_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 &&
           (strcmp(command->argv[0], "set") == 0 ||
            strcmp(command->argv[0], "shift") == 0);
}

static bool native_getopts_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "getopts") == 0;
}

static bool native_read_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "read") == 0;
}

static bool native_fc_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "fc") == 0;
}

static gsh_file_builtin_kind native_file_builtin_kind(
    const gsh_native_command *command)
{
    if (command == NULL || command->argc == 0U) return 0;
    if (strcmp(command->argv[0], "ls") == 0) return GSH_FILE_BUILTIN_LS;
    if (strcmp(command->argv[0], "ll") == 0) return GSH_FILE_BUILTIN_LL;
    if (strcmp(command->argv[0], "view") == 0) return GSH_FILE_BUILTIN_VIEW;
    return 0;
}

static bool native_file_builtin(const gsh_native_command *command)
{
    return native_file_builtin_kind(command) != 0;
}

static int run_native_file_builtin(const gsh_native_command *command,
                                   const gsh_builtin_io *io)
{
    if (command == NULL || io == NULL) return 1;
    return gsh_builtin_run_files(native_file_builtin_kind(command),
                                 command->argc, command->argv, io);
}

static bool native_jobs_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "jobs") == 0;
}

static bool native_kill_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "kill") == 0;
}

static bool native_fg_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "fg") == 0;
}

static bool native_bg_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "bg") == 0;
}

static bool native_snapshot_job_control_builtin(
    const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return native_jobs_builtin(command) || native_kill_builtin(command);
}

static bool native_job_control_builtin(
    const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return native_snapshot_job_control_builtin(command) ||
           native_fg_builtin(command) || native_bg_builtin(command);
}

static bool native_posix_stateful_builtin(
    const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return native_getopts_builtin(command) || native_read_builtin(command);
}

static bool native_wait_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "wait") == 0;
}

static bool native_alias_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 &&
           (strcmp(command->argv[0], "alias") == 0 ||
            strcmp(command->argv[0], "unalias") == 0);
}

static bool native_hash_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "hash") == 0;
}

static bool native_times_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "times") == 0;
}

static bool native_trap_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "trap") == 0;
}

static bool native_exec_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "exec") == 0;
}

static bool native_exit_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "exit") == 0;
}

static bool native_eval_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "eval") == 0;
}

static bool native_dot_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], ".") == 0;
}

static bool native_source_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return native_eval_builtin(command) || native_dot_builtin(command);
}

static bool native_command_inspection_builtin(
    const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && gsh_command_is_inspection_builtin(
                                      command->argc, command->argv);
}

/* ── Command Wrappers Become Explicit Execution Policy ───────────────
 * Leaving `command` as an argv prefix would require every dispatcher to
 * reinterpret its options and would make nested wrappers disagree.  After
 * expansion, a bounded normalization removes execution-form wrappers and
 * records the three semantic differences on the planned command.  A function
 * named command still wins ordinary lookup; once a wrapper is active, later
 * function lookup is deliberately suppressed as POSIX requires.
 * ─────────────────────────────────────────────────────────────── */
static void normalize_command_invocations(
    gsh_native_pipeline *pipeline, const gsh_function_store *functions)
{
    if (pipeline == NULL) {
        return;
    }
    size_t command_index;

    for (command_index = 0;
         command_index < pipeline->command_count &&
         command_index < GSH_NATIVE_PIPELINE_CAP; command_index++) {
        gsh_native_command *command = &pipeline->commands[command_index];
        size_t wrappers = 0;

        while (command->argc != 0 &&
               wrappers < GSH_NATIVE_ARGUMENT_CAP &&
               strcmp(command->argv[0], "command") == 0) {
            gsh_command_invocation invocation;
            size_t remaining;

            if (!command->command_suppresses_functions &&
                functions != NULL &&
                gsh_functions_lookup(functions, "command", 7U) != NULL) {
                break;
            }
            invocation = gsh_command_parse(command->argc, command->argv);
            if (invocation.form != GSH_COMMAND_FORM_EXECUTE) {
                break;
            }
            remaining = command->argc - invocation.first_operand;
            (void)memmove(command->argv,
                    command->argv + invocation.first_operand,
                    remaining * sizeof(command->argv[0]));
            command->argc = remaining;
            command->argv[remaining] = NULL;
            command->command_suppresses_functions = true;
            command->command_uses_default_path =
                invocation.use_default_path;
            command->command_regular_context = true;
            wrappers++;
        }
    }
}

static bool native_return_builtin(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 && strcmp(command->argv[0], "return") == 0;
}

static bool native_loop_control_builtin(
    const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc > 0 &&
           (strcmp(command->argv[0], "break") == 0 ||
            strcmp(command->argv[0], "continue") == 0);
}

static bool parse_exit_status(const gsh_native_command *command,
                              int last_status, int *status)
{
    if (command == NULL) return false;
    if (status == NULL) {
        return false;
    }
    unsigned int value = (unsigned int)(last_status & 255);
    const char *cursor;

    if (command->argc > 2U) {
        (void)fputs("gsh: exit: too many operands\n", stderr);
        *status = 1;
        return false;
    }
    if (command->argc == 1U) {
        *status = (int)value;
        return true;
    }
    value = 0;
    cursor = command->argv[1];
    if (*cursor == '\0') {
        (void)fputs("gsh: exit: invalid status\n", stderr);
        *status = 2;
        return false;
    }
    while (*cursor != '\0') {
        if (*cursor < '0' || *cursor > '9' || value > 25U) {
            (void)fputs("gsh: exit: invalid status\n", stderr);
            *status = 2;
            return false;
        }
        value = value * 10U + (unsigned int)(*cursor++ - '0');
    }
    if (value > 255U) {
        (void)fputs("gsh: exit: invalid status\n", stderr);
        *status = 2;
        return false;
    }
    *status = (int)value;
    return true;
}

static bool special_builtin_name(const char *name, size_t length)
{
    if (name == NULL) {
        return false;
    }
    return gsh_command_special_builtin_name(name, length);
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
    if (command == NULL) return false;
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
    if (command == NULL) {
        return false;
    }
    return command->argc == 1 && strcmp(command->argv[0], "set") == 0;
}

static bool native_variable_listing(const gsh_native_command *command)
{
    if (command == NULL) {
        return false;
    }
    return command->argc == 2 && strcmp(command->argv[1], "-p") == 0 &&
           (strcmp(command->argv[0], "export") == 0 ||
            strcmp(command->argv[0], "readonly") == 0);
}

static bool native_pipeline_requires_evaluator(
    const gsh_native_pipeline *pipeline)
{
    if (pipeline == NULL) {
        return false;
    }
    const gsh_native_command *command;
    size_t index;

    for (index = 0; index < pipeline->command_count; index++) {
        if (native_fc_builtin(&pipeline->commands[index]) ||
            native_job_control_builtin(&pipeline->commands[index])) {
            return true;
        }
    }
    if (pipeline->command_count != 1) {
        return false;
    }
    command = &pipeline->commands[0];
    return native_exec_builtin(command) ||
           native_source_builtin(command) ||
           native_fc_builtin(command) ||
           native_read_builtin(command) ||
           (native_getopts_builtin(command) &&
            (command->assignment_count != 0 ||
             command->redirect_count != 0)) ||
           (native_variable_builtin(command) &&
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
           (native_hash_builtin(command) &&
            command->redirect_count != 0) ||
           (native_times_builtin(command) &&
            (command->assignment_count != 0 ||
             command->redirect_count != 0)) ||
           (native_command_inspection_builtin(command) &&
            command->redirect_count != 0) ||
           (native_colon_builtin(command) &&
            command->assignment_count != 0 &&
            command->redirect_count != 0);
}

static unsigned int assignment_attributes(
    const gsh_shell_options *options)
{
    if (options == NULL) {
        return -1;
    }
    return gsh_options_enabled(options, GSH_OPTION_ALLEXPORT)
               ? GSH_VARIABLE_EXPORTED
               : 0U;
}

static int unset_native_functions(
    const gsh_native_command *command, gsh_function_store *functions,
    const gsh_builtin_io *io)
{
    if (command == NULL) return -1;
    if (io == NULL) {
        return -1;
    }
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
    if (command == NULL || io == NULL || options == NULL ||
        variables == NULL) {
        return -1;
    }
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
    if (command == NULL || io == NULL || options == NULL || variables == NULL) {
        return -1;
    }
    bool set_builtin = strcmp(command->argv[0], "set") == 0;

    if (positionals == NULL &&
        (!set_builtin ||
         gsh_builtin_set_mutates_positionals(command->argc,
                                             command->argv))) {
        return -1;
    }
    return set_builtin
               ? gsh_builtin_set(command->argc, command->argv, variables,
                                 positionals, options, io)
               : gsh_builtin_shift(command->argc, command->argv,
                                   positionals, io);
}

static int run_native_posix_stateful_builtin(
    const gsh_native_command *command,
    const gsh_variable_store *lookup_variables,
    gsh_variable_store *variables, gsh_variable_store *scratch,
    gsh_variable_journal *journal,
    const gsh_positional_store *positionals, gsh_shell_options *options,
    const gsh_builtin_io *io)
{
    if (command == NULL || io == NULL || lookup_variables == NULL ||
        options == NULL || positionals == NULL || scratch == NULL ||
        variables == NULL) {
        return -1;
    }
    return native_read_builtin(command)
               ? gsh_builtin_read(command->argc, command->argv,
                                  lookup_variables, variables, scratch,
                                  journal, io)
               : gsh_builtin_getopts(command->argc, command->argv,
                                     lookup_variables, variables, scratch,
                                     journal, positionals, options, io);
}

static int run_native_cd_builtin(
    const gsh_native_command *command,
    const gsh_variable_store *lookup_variables,
    gsh_variable_store *variables,
    gsh_variable_journal *journal, const gsh_shell_options *options,
    const gsh_builtin_io *io, char *directory,
    size_t directory_capacity)
{
    if (command == NULL || io == NULL || lookup_variables == NULL ||
        options == NULL || variables == NULL ||
        (directory_capacity != 0U && directory == NULL)) {
        return -1;
    }
    return gsh_builtin_cd(command->argc, command->argv, lookup_variables,
                          variables, journal, assignment_attributes(options),
                          io, directory, directory_capacity);
}

static int run_native_environment_builtin(const gsh_native_command *command,
                                          const gsh_builtin_io *io)
{
    if (command == NULL || io == NULL) {
        return -1;
    }
    return strcmp(command->argv[0], "ulimit") == 0
               ? gsh_builtin_ulimit(command->argc, command->argv, io)
               : gsh_builtin_umask(command->argc, command->argv, io);
}

static int run_native_alias_builtin(
    const gsh_native_command *command, gsh_alias_store *aliases,
    gsh_alias_journal *journal, const gsh_builtin_io *io)
{
    if (aliases == NULL || command == NULL || io == NULL) {
        return -1;
    }
    return strcmp(command->argv[0], "alias") == 0
               ? gsh_builtin_alias(command->argc, command->argv, aliases,
                                   journal, io)
               : gsh_builtin_unalias(command->argc, command->argv, aliases,
                                     journal, io);
}

static int run_native_command_inspection(
    const gsh_native_command *command, const char *path,
    const char *default_path, const gsh_alias_store *aliases,
    const gsh_function_store *functions, gsh_command_cache *cache,
    uint64_t path_generation, bool cacheable, bool *cache_changed,
    const gsh_builtin_io *io)
{
    if (aliases == NULL || cache == NULL || command == NULL ||
        default_path == NULL || functions == NULL || io == NULL ||
        path == NULL) {
        return -1;
    }
    if (strcmp(command->argv[0], "type") == 0) {
        return gsh_builtin_type(command->argc, command->argv, path, aliases,
                                functions, cache, path_generation,
                                cache_changed, io);
    }
    return gsh_builtin_command_inspect(
        command->argc, command->argv, path, default_path, aliases,
        functions, cache, path_generation, cacheable, cache_changed, io);
}

static int run_native_hash_builtin(
    const gsh_native_command *command, const char *path,
    const gsh_function_store *functions, gsh_command_cache *cache,
    uint64_t path_generation, bool *cache_changed,
    const gsh_builtin_io *io)
{
    if (cache == NULL || command == NULL || functions == NULL || io == NULL ||
        path == NULL) {
        return -1;
    }
    return gsh_builtin_hash(command->argc, command->argv, path, functions,
                            cache, path_generation, cache_changed, io);
}

static int run_native_times_builtin(
    const gsh_native_command *command,
    const gsh_times_context *times_context,
    const gsh_builtin_io *io)
{
    if (command == NULL || io == NULL) {
        return -1;
    }
    return gsh_builtin_times(command->argc, command->argv, times_context,
                             io);
}

static const gsh_builtin_io descriptor_builtin_io = {
    .kind = GSH_BUILTIN_SINK_DESCRIPTORS,
    .descriptors = {STDOUT_FILENO, STDERR_FILENO},
};

static int emit_verbose_input(const gsh_builtin_io *io, const char *text,
                              size_t length)
{
    if (!require(io != NULL && text != NULL)) return -1;
    if (gsh_builtin_output(io, STDERR_FILENO, text, length) != 0) return -1;
    return length == 0U || text[length - 1U] == '\n'
               ? 0
               : gsh_builtin_output(io, STDERR_FILENO, "\n", 1U);
}

static bool input_line_continues(const char *input, size_t length)
{
    size_t end;
    size_t slashes;

    if (input == NULL || length == 0U) return false;
    end = input[length - 1U] == '\n' ? length - 1U : length;
    for (slashes = 0U;
         slashes < end && input[end - slashes - 1U] == '\\';
         slashes++) {
    }
    return (slashes & 1U) != 0U;
}

static bool trace_word_is_safe(const char *word)
{
    size_t index;

    if (!require(word != NULL)) return false;
    if (word[0] == '\0') return false;
    for (index = 0U; index < GSH_NATIVE_TEXT_CAP && word[index] != '\0';
         index++) {
        unsigned char byte = (unsigned char)word[index];
        bool safe = (byte >= 'a' && byte <= 'z') ||
                    (byte >= 'A' && byte <= 'Z') ||
                    (byte >= '0' && byte <= '9') ||
                    strchr("_@%+=:,./-", byte) != NULL;

        if (!safe) return false;
    }
    return index < GSH_NATIVE_TEXT_CAP;
}

static int emit_trace_word(const gsh_builtin_io *io, const char *word)
{
    static const char escaped_quote[] = "'\\''";
    size_t begin = 0U;
    size_t index;

    if (!require(io != NULL && word != NULL)) return -1;
    if (trace_word_is_safe(word)) {
        return gsh_builtin_output(io, STDERR_FILENO, word, strlen(word));
    }
    if (gsh_builtin_output(io, STDERR_FILENO, "'", 1U) != 0) return -1;
    for (index = 0U; index < GSH_NATIVE_TEXT_CAP && word[index] != '\0';
         index++) {
        if (word[index] != '\'') continue;
        if (gsh_builtin_output(io, STDERR_FILENO, word + begin,
                               index - begin) != 0 ||
            gsh_builtin_output(io, STDERR_FILENO, escaped_quote,
                               sizeof(escaped_quote) - 1U) != 0) return -1;
        begin = index + 1U;
    }
    if (index == GSH_NATIVE_TEXT_CAP ||
        gsh_builtin_output(io, STDERR_FILENO, word + begin,
                           index - begin) != 0) return -1;
    return gsh_builtin_output(io, STDERR_FILENO, "'", 1U);
}

static int emit_trace_assignment(const gsh_builtin_io *io,
                                 const char *assignment)
{
    size_t equals;

    if (!require(io != NULL && assignment != NULL)) return -1;
    for (equals = 0U;
         equals < GSH_NATIVE_TEXT_CAP && assignment[equals] != '=' &&
         assignment[equals] != '\0';
         equals++) {
    }
    if (equals == GSH_NATIVE_TEXT_CAP || assignment[equals] != '=') {
        errno = EINVAL;
        return -1;
    }
    if (gsh_builtin_output(io, STDERR_FILENO, assignment, equals + 1U) != 0) {
        return -1;
    }
    return emit_trace_word(io, assignment + equals + 1U);
}

static int emit_command_trace(const gsh_builtin_io *io,
                              const gsh_native_command *command,
                              const char *prefix)
{
    size_t field = 0U;
    size_t index;

    if (!require(io != NULL && command != NULL && prefix != NULL)) return -1;
    if (gsh_builtin_output(io, STDERR_FILENO, prefix, strlen(prefix)) != 0) {
        return -1;
    }
    for (index = 0U; index < command->assignment_count; index++) {
        if ((field++ != 0U && gsh_builtin_output(
                io, STDERR_FILENO, " ", 1U) != 0) ||
            emit_trace_assignment(io, command->assignments[index]) != 0) {
            return -1;
        }
    }
    for (index = 0U; index < command->argc; index++) {
        if ((field++ != 0U && gsh_builtin_output(
                io, STDERR_FILENO, " ", 1U) != 0) ||
            emit_trace_word(io, command->argv[index]) != 0) return -1;
    }
    return gsh_builtin_output(io, STDERR_FILENO, "\n", 1U);
}

static int emit_pipeline_trace(const gsh_builtin_io *io,
                               const gsh_native_pipeline *pipeline,
                               const gsh_variable_store *variables)
{
    const char *prefix;
    bool found;
    size_t index;

    if (!require(io != NULL && pipeline != NULL && variables != NULL)) {
        return -1;
    }
    prefix = gsh_variables_lookup(variables, "PS4", 3U, &found);
    if (!found) prefix = "+ ";
    for (index = 0U; index < pipeline->command_count; index++) {
        if (emit_command_trace(io, &pipeline->commands[index], prefix) != 0) {
            return -1;
        }
    }
    return 0;
}

static void child_write_descriptor(int descriptor, const char *text,
                                   size_t length)
{
    if (text == NULL) {
        return;
    }
    size_t attempt;

    for (attempt = 0; attempt < CHILD_WRITE_ATTEMPT_CAP && length > 0;
         attempt++) {
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
    for (size_t part = 0; part <= PATH_MAX; part++) {
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
    return false;
}

static int child_run_pwd(const gsh_native_command *command,
                         const gsh_variable_store *variables)
{
    if (command == NULL || variables == NULL) {
        return -1;
    }
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
    if (entry == NULL) return false;
    if (assignment == NULL) {
        return false;
    }
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
    if (storage == NULL || variables == NULL) {
        return NULL;
    }
    size_t source;
    size_t used = 0;
    size_t assignment_count = 0U;

    if (command != NULL) {
        assignment_count = command->assignment_count;
    }

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
    if (pipeline == NULL) {
        return false;
    }
    const gsh_native_command *native = &pipeline->commands[index];
    simple_command command = {0};
    size_t argument;

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
    if (native_pure_builtin(native)) {
        return true;
    }
    if (native_pwd_builtin(native)) {
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
    if (native_posix_stateful_builtin(native)) {
        return true;
    }
    if (native_fc_builtin(native) || native_file_builtin(native)) {
        return true;
    }
    if (native_job_control_builtin(native)) {
        return true;
    }
    if (native_wait_builtin(native)) {
        return (native->assignment_count == 0 ||
                native->command_regular_context) &&
               native->redirect_count == 0;
    }
    if (native_alias_builtin(native)) {
        return true;
    }
    if (native_hash_builtin(native)) {
        return true;
    }
    if (native_times_builtin(native)) {
        return true;
    }
    if (native_trap_builtin(native)) {
        return true;
    }
    if (native_exec_builtin(native)) {
        return true;
    }
    if (native_exit_builtin(native)) {
        return true;
    }
    if (native_source_builtin(native)) {
        return true;
    }
    if (native_command_inspection_builtin(native)) {
        return true;
    }
    if (native_return_builtin(native)) {
        return (native->assignment_count == 0 ||
                native->command_regular_context) &&
               native->redirect_count == 0;
    }
    if (native_loop_control_builtin(native)) {
        return pipeline->command_count == 1U;
    }
    if (!is_native_command_name(native->argv[0])) {
        return false;
    }
    command.argc = native->argc;
    for (argument = 0; argument < command.argc; argument++) {
        command.argv[argument] = native->argv[argument];
    }
    command.argv[command.argc] = NULL;
    return path_value != NULL && direct_path_is_bounded(&command, path_value);
}

static bool native_pipeline_is_supported_scoped(
    const gsh_native_pipeline *pipeline, const char *default_path,
    const gsh_variable_store *variables,
    const pipeline_expansion_scope *scope)
{
    if (default_path == NULL || pipeline == NULL || variables == NULL) {
        return false;
    }
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

static int apply_native_assignments_with_attributes(
    gsh_variable_store *variables, gsh_variable_journal *journal,
    const gsh_native_command *command, unsigned int attributes)
{
    if (command == NULL || variables == NULL) {
        return -1;
    }
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

static int apply_native_assignments(gsh_variable_store *variables,
                                    gsh_variable_journal *journal,
                                    const gsh_native_command *command,
                                    const gsh_shell_options *options)
{
    if (command == NULL || options == NULL || variables == NULL) {
        return -1;
    }
    return apply_native_assignments_with_attributes(
        variables, journal, command, assignment_attributes(options));
}

static int child_run_posix_stateful_builtin(
    const gsh_native_command *command, gsh_variable_store *variables,
    gsh_variable_store *scratch,
    const gsh_positional_store *positionals, gsh_shell_options *options)
{
    if (command == NULL) return -1;
    if (options == NULL || positionals == NULL || scratch == NULL || variables == NULL) {
        return -1;
    }
    const gsh_variable_store *lookup = variables;

    if (command->assignment_count != 0) {
        (void)memcpy(scratch, variables, sizeof(*scratch));
        if (apply_native_assignments(scratch, NULL, command, options) !=
            GSH_ASSIGNMENT_OK) {
            return 1;
        }
        lookup = scratch;
    }
    return run_native_posix_stateful_builtin(
        command, lookup, variables, scratch, NULL, positionals, options,
        &descriptor_builtin_io);
}

static int apply_special_builtin_assignments(
    gsh_variable_store *variables, gsh_variable_journal *journal,
    const gsh_native_command *command,
    const gsh_shell_options *options)
{
    if (command == NULL || options == NULL || variables == NULL) {
        return -1;
    }
    return command->command_regular_context
               ? GSH_ASSIGNMENT_OK
               : apply_native_assignments(variables, journal, command,
                                          options);
}

static int child_duplicate_descriptor(int source, int destination)
{
    if (source == destination) {
        return fcntl(source, F_GETFD) == -1 ? -1 : destination;
    }
    if (gsh_fault_should_fail(GSH_FAULT_DESCRIPTOR_DUP, EMFILE)) {
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
    if (saved == NULL) return -1;
    if (command == NULL || saved_count == NULL) {
        return -1;
    }
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
            gsh_fault_should_fail(GSH_FAULT_DESCRIPTOR_SAVE, EMFILE)
                ? -1
                : fcntl(descriptor, F_DUPFD_CLOEXEC, minimum);
        if (saved[*saved_count].saved == -1 && errno != EBADF) {
            while (*saved_count > 0) {
                int prior_saved = saved[--(*saved_count)].saved;

                if (prior_saved >= 0) {
                    (void)close(prior_saved);
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
    if (saved == NULL) {
        return -1;
    }
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
            (void)close(entry->saved);
        } else if (close(entry->descriptor) == -1 && errno != EBADF) {
            status = -1;
        }
    }
    return status;
}

static int materialize_heredoc(const gsh_native_pipeline *pipeline,
                               size_t heredoc_index)
{
    if (pipeline == NULL) return -1;
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

        (void)close(descriptor);
        errno = saved_errno;
        return -1;
    }
    heredoc = &pipeline->heredocs[heredoc_index];
    cursor = heredoc->body;
    remaining = heredoc->length;
    while (remaining > 0) {
        ssize_t written = gsh_fault_should_fail(GSH_FAULT_HEREDOC_WRITE, EIO)
                              ? -1
                              : write(descriptor, cursor, remaining);

        if (written > 0) {
            cursor += (size_t)written;
            remaining -= (size_t)written;
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            int saved_errno = written == 0 ? EIO : errno;

            (void)close(descriptor);
            errno = saved_errno;
            return -1;
        }
    }
    if (lseek(descriptor, 0, SEEK_SET) == -1) {
        int saved_errno = errno;

        (void)close(descriptor);
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
    if (target == NULL) {
        return -1;
    }
    int flags;

    if (gsh_fault_should_fail(GSH_FAULT_REDIRECT_OPEN, EMFILE)) {
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

            (void)close(descriptor);
            errno = saved_errno;
            return -1;
        }
        if (S_ISREG(status.st_mode)) {
            (void)close(descriptor);
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
    if (options == NULL || redirect == NULL) {
        return -1;
    }
    return open_redirect_path(redirect->target, redirect->operator_kind,
                              options, 0666);
}

static int apply_evaluator_redirects_record(
    const gsh_native_pipeline *pipeline, const gsh_native_command *command,
    const gsh_shell_options *options, bool standard_changes[3])
{
    if (command == NULL || options == NULL || pipeline == NULL) {
        return -1;
    }
    size_t index;

    for (index = 0; index < command->redirect_count; index++) {
        const gsh_native_redirect *redirect = &command->redirects[index];
        int descriptor;

        if (redirect->close_descriptor) {
            if (close(redirect->descriptor) == -1 && errno != EBADF) {
                return -1;
            }
            if (standard_changes != NULL && redirect->descriptor >= 0 &&
                redirect->descriptor <= 2) {
                standard_changes[redirect->descriptor] = true;
            }
            continue;
        }
        if (redirect->operator_kind == GSH_TOKEN_LESSAND ||
            redirect->operator_kind == GSH_TOKEN_GREATAND) {
            if (child_duplicate_descriptor(redirect->duplicate_descriptor,
                                           redirect->descriptor) == -1) {
                return -1;
            }
            if (standard_changes != NULL && redirect->descriptor >= 0 &&
                redirect->descriptor <= 2) {
                standard_changes[redirect->descriptor] = true;
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
                (void)close(descriptor);
            }
            errno = saved_errno;
            return -1;
        }
        if (descriptor != redirect->descriptor) {
            (void)close(descriptor);
        }
        if (standard_changes != NULL && redirect->descriptor >= 0 &&
            redirect->descriptor <= 2) {
            standard_changes[redirect->descriptor] = true;
        }
    }
    return 0;
}

static int apply_evaluator_redirects(
    const gsh_native_pipeline *pipeline, const gsh_native_command *command,
    const gsh_shell_options *options)
{
    if (command == NULL || options == NULL || pipeline == NULL) {
        return -1;
    }
    return apply_evaluator_redirects_record(
        pipeline, command, options, NULL);
}

/* ── `exec` Is a Descriptor Commit, Then an Overlay ──────────────────
 * Ordinary builtins borrow redirected descriptors and restore them.  POSIX
 * gives `exec` the opposite transaction boundary: every successful redirect
 * is committed before option handling or utility lookup, including failures.
 * The search helper returns only on failure, so the successful path adds no
 * cleanup, allocation, or post-exec branch.
 * ─────────────────────────────────────────────────────────────── */
static int exec_utility_index(const gsh_native_command *command,
                              size_t *utility_index)
{
    if (command == NULL) return -1;
    if (utility_index == NULL) {
        return -1;
    }
    size_t index = 1U;

    if (index < command->argc && strcmp(command->argv[index], "--") == 0) {
        index++;
    } else if (index < command->argc && command->argv[index][0] == '-' &&
               command->argv[index][1] != '\0') {
        child_write_text("gsh: exec: unsupported option: ");
        child_write_text(command->argv[index]);
        child_write_text("\n");
        return -1;
    }
    *utility_index = index;
    return 0;
}

static int notify_exec_outcome(int descriptor, unsigned char outcome)
{
    unsigned int attempts;

    if (descriptor <= STDERR_FILENO) {
        return 0;
    }
    for (attempts = 0; attempts < 16U; attempts++) {
        ssize_t written = write(descriptor, &outcome, sizeof(outcome));

        if (written == (ssize_t)sizeof(outcome)) {
            return 0;
        }
        if (written == -1 && errno == EINTR) {
            continue;
        }
        errno = written == 0 ? EIO : errno;
        return -1;
    }
    errno = EINTR;
    return -1;
}

static int prepare_interactive_exec(shell_state *owner)
{
    static const char leave_managed_screen[] =
        "\033[0m\033[?25h\033[?1049l";
    pid_t worker_pid;

    if (owner == NULL) {
        return 0;
    }
    flush_output(owner);
    if (owner->output_len != 0 ||
        tcsetattr(owner->tty_fd, TCSANOW, &owner->original_modes) == -1) {
        child_write_text("gsh: exec: terminal handoff failed\n");
        return -1;
    }
    if (owner->async_repl != NULL && state_async_repl(owner)->enabled) {
        child_write_descriptor(owner->tty_fd, leave_managed_screen,
                               sizeof(leave_managed_screen) - 1U);
    }
    owner->terminal_changed = false;
    /* ── An Exec Overlay Must Not Abandon Internal Workers ───────
     * A datagram peer does not receive EOF when exec closes its owner socket.
     * The old redirection worker therefore outlived the replacement utility.
     * Reap internal persistence workers before overlay; a failed exec restores
     * fresh workers with the editor, preserving both PID identity and recovery.
     * ─────────────────────────────────────────────────────────────── */
    worker_pid = owner->redirection_worker_pid;
    owner->redirection_worker_restart_pending = false;
    disable_redirection_worker(owner, true);
    if (worker_pid > 0)
        while (waitpid(worker_pid, NULL, 0) == -1 && errno == EINTR) {
        }
    owner->redirection_worker_pid = -1;
    finish_journal_worker(owner);
    reset_child_signals();
    return 0;
}

static int restore_interactive_exec(shell_state *owner)
{
    if (owner == NULL) {
        return 0;
    }
    if (install_signal_handlers() == -1 ||
        tcsetattr(owner->tty_fd, TCSANOW, &owner->editor_modes) == -1) {
        owner->running = false;
        return -1;
    }
    owner->terminal_changed = true;
    if (start_redirection_worker(owner) == -1)
        owner->redirection_worker_failures++;
    start_journal_worker(owner);
    if (owner->async_repl != NULL && state_async_repl(owner)->enabled) {
        state_async_repl(owner)->render_pending = true;
    }
    return 0;
}

static int protect_interactive_exec(
    shell_state *owner, const gsh_native_command *command)
{
    int32_t targets[GSH_NATIVE_REDIRECT_CAP * 2U];
    size_t count = 0;
    size_t index;

    if (owner == NULL) {
        return 0;
    }
    if (command == NULL ||
        command->redirect_count > GSH_NATIVE_REDIRECT_CAP) return -1;
    if (command->redirect_count == 0U) return 0;
    for (index = 0; index < command->redirect_count; index++) {
        const gsh_native_redirect *redirect = &command->redirects[index];

        targets[count++] = redirect->descriptor;
        if (redirect->duplicate_descriptor >= 0) {
            targets[count++] = redirect->duplicate_descriptor;
        }
    }
    if (count > GSH_NATIVE_REDIRECT_CAP * 2U) return -1;
    return protect_exec_owner_descriptors(owner, targets, count);
}

static int exec_external_utility(
    const gsh_native_command *command, size_t utility_index,
    const gsh_variable_store *variables, const char *default_path,
    gsh_command_cache *cache, int outcome_fd)
{
    if (cache == NULL || command == NULL || default_path == NULL || variables == NULL) {
        return -1;
    }
    char *environment_storage[CHILD_ENVIRONMENT_CAP];
    char *const *environment;
    int exec_error;

    if (gsh_fault_should_fail(GSH_FAULT_EXEC, EIO)) {
        exec_error = errno;
    } else {
        environment = child_command_environment(
            variables, command, environment_storage);
        exec_error = exec_direct_error(
            command->argv + utility_index,
            command_path_value(variables, command, default_path),
            environment, cache,
            command_cache_path_generation(variables, command),
            command_uses_persistent_path(command));
    }
    if (notify_exec_outcome(outcome_fd, 'F') == -1) {
        return 125;
    }
    return write_exec_error(command->argv[utility_index], exec_error);
}

static int run_evaluator_exec_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_journal *journal, const gsh_shell_options *options,
    const char *default_path, gsh_command_cache *cache,
    shell_state *interactive_owner, int outcome_fd,
    bool *descriptors_dirty, bool *builtin_failed)
{
    if (builtin_failed == NULL || cache == NULL || default_path == NULL ||
        pipeline == NULL || variables == NULL || options == NULL) {
        return -1;
    }
    const gsh_native_command *command = &pipeline->commands[0];
    size_t utility_index;
    int assignment_status;
    int status;

    *builtin_failed = true;
    if (descriptors_dirty != NULL && command->redirect_count != 0) {
        *descriptors_dirty = true;
    }
    if (protect_interactive_exec(interactive_owner, command) == -1) {
        perror("gsh: exec descriptor protection");
        return 125;
    }
    if (apply_evaluator_redirects_record(
            pipeline, command, options,
            interactive_owner == NULL
                ? NULL
                : interactive_owner->exec_standard_descriptor_changed) ==
        -1) {
        perror("gsh: exec redirection");
        return 1;
    }
    assignment_status = apply_special_builtin_assignments(
        variables, journal, command, options);
    if (assignment_status != GSH_ASSIGNMENT_OK) {
        perror("gsh: exec assignment");
        return assignment_status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125 : 1;
    }
    if (exec_utility_index(command, &utility_index) == -1) {
        return 2;
    }
    if (utility_index == command->argc) {
        *builtin_failed = false;
        return pipeline->negated ? 1 : 0;
    }
    if (notify_exec_outcome(outcome_fd, 'A') == -1) {
        perror("gsh: exec outcome");
        return 125;
    }
    if (prepare_interactive_exec(interactive_owner) == -1) {
        return 125;
    }
    status = exec_external_utility(
        command, utility_index, variables, default_path, cache, outcome_fd);
    if (restore_interactive_exec(interactive_owner) == -1) {
        return 125;
    }
    return command->command_regular_context && pipeline->negated
               ? (status == 0 ? 1 : 0)
               : status;
}

static int run_evaluator_variable_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_journal *journal, const gsh_shell_options *options,
    gsh_function_store *functions)
{
    if (pipeline == NULL || variables == NULL || options == NULL) {
        return -1;
    }
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
        int assignment_status = apply_special_builtin_assignments(
            variables, journal, command, options);

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
    if (aliases == NULL || pipeline == NULL || options == NULL) {
        return -1;
    }
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

static int run_evaluator_hash_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    const char *default_path, const gsh_function_store *functions,
    gsh_command_cache *cache, const gsh_shell_options *options)
{
    if (cache == NULL || default_path == NULL || functions == NULL || pipeline == NULL || variables == NULL) {
        return -1;
    }
    const gsh_native_command *command = &pipeline->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    const char *path;
    int status;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: hash redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command, options) == -1) {
        perror("gsh: hash redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    path = hash_command_path_value(variables, command, default_path);
    status = run_native_hash_builtin(
        command, path, functions, cache,
        hash_command_path_generation(variables, command), NULL,
        &descriptor_builtin_io);
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: hash redirection restore");
        return 125;
    }
    return status == 125 ? 125
                         : (pipeline->negated ? (status == 0 ? 1 : 0)
                                              : status);
}

static int run_evaluator_times_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_journal *journal, const gsh_shell_options *options,
    const gsh_times_context *times_context, bool *builtin_failed)
{
    if (builtin_failed == NULL || pipeline == NULL || variables == NULL ||
        options == NULL) {
        return -1;
    }
    const gsh_native_command *command = &pipeline->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    int status;

    *builtin_failed = false;
    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: times redirection save");
        *builtin_failed = true;
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command, options) == -1) {
        perror("gsh: times redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        *builtin_failed = true;
        return 1;
    }
    status = apply_special_builtin_assignments(
        variables, journal, command, options);
    if (status != GSH_ASSIGNMENT_OK) {
        perror("gsh: times assignment");
        status = status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125 : 1;
    } else {
        status = run_native_times_builtin(
            command, times_context, &descriptor_builtin_io);
    }
    *builtin_failed = status != 0;
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: times redirection restore");
        *builtin_failed = true;
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
    if (pipeline == NULL || variables == NULL || options == NULL) {
        return -1;
    }
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
    status = apply_special_builtin_assignments(
        variables, journal, command, options);
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

static int run_evaluator_posix_stateful_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_store *scratch, gsh_variable_journal *journal,
    const gsh_positional_store *positionals, gsh_shell_options *options)
{
    if (pipeline == NULL || positionals == NULL || scratch == NULL ||
        variables == NULL || options == NULL) {
        return -1;
    }
    const gsh_native_command *command = &pipeline->commands[0];
    const gsh_variable_store *lookup = variables;
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    int status;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: builtin redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command, options) == -1) {
        perror("gsh: builtin redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    if (command->assignment_count != 0) {
        (void)memcpy(scratch, variables, sizeof(*scratch));
        if (apply_native_assignments(scratch, NULL, command, options) !=
            GSH_ASSIGNMENT_OK) {
            perror("gsh: builtin assignment");
            (void)restore_redirect_descriptors(saved, saved_count);
            return 1;
        }
        lookup = scratch;
    }
    status = run_native_posix_stateful_builtin(
        command, lookup, variables, scratch, journal, positionals, options,
        &descriptor_builtin_io);
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: builtin redirection restore");
        return 125;
    }
    return status == 125 ? 125
                         : (pipeline->negated ? (status == 0 ? 1 : 0)
                                              : status);
}

static int open_job_service_temp(void)
{
    char path[] = "/tmp/gsh-job-service-XXXXXX";
    int descriptor = mkstemp(path);

    if (descriptor == -1) return -1;
    if (unlink(path) == -1 ||
        set_fd_flags(descriptor, F_GETFD, FD_CLOEXEC) == -1) {
        int saved_errno = errno;

        (void)close(descriptor);
        errno = saved_errno;
        return -1;
    }
    return descriptor;
}

static int pack_job_service_request(
    const gsh_native_command *command, job_service_request *request)
{
    size_t argument;
    size_t used = 0;

    if (command == NULL || request == NULL || command->argc == 0 ||
        command->argc > GSH_NATIVE_ARGUMENT_CAP) {
        errno = EINVAL;
        return -1;
    }
    (void)memset(request, 0, sizeof(*request));
    request->version = GSH_JOB_SERVICE_VERSION;
    request->type = native_jobs_builtin(command)
                        ? GSH_JOB_SERVICE_JOBS
                        : (native_kill_builtin(command)
                               ? GSH_JOB_SERVICE_KILL
                               : GSH_JOB_SERVICE_WAIT);
    request->argc = (uint32_t)command->argc;
    for (argument = 0; argument < command->argc; argument++) {
        size_t available = sizeof(request->text) - used;
        size_t length = strnlen(command->argv[argument], available);

        if (length == available) {
            errno = E2BIG;
            return -1;
        }
        request->offsets[argument] = (uint32_t)used;
        (void)memcpy(request->text + used, command->argv[argument], length + 1U);
        used += length + 1U;
    }
    request->text_length = (uint32_t)used;
    return 0;
}

static int copy_job_service_output(int source, int target)
{
    char buffer[4096];
    const gsh_builtin_io io = {
        .kind = GSH_BUILTIN_SINK_DESCRIPTORS,
        .descriptors = {target, target},
    };
    size_t total = 0;
    const size_t limit = GSH_BACKGROUND_CAP *
                         (GSH_BACKGROUND_COMMAND_CAP + 128U);

    if (lseek(source, 0, SEEK_SET) == (off_t)-1) return -1;
    while (total <= limit) {
        ssize_t count = read(source, buffer, sizeof(buffer));

        if (count > 0) {
            if ((size_t)count > limit - total ||
                gsh_builtin_output(
                    &io, target, buffer, (size_t)count) != 0) {
                errno = EFBIG;
                return -1;
            }
            total += (size_t)count;
        } else if (count == 0) {
            return 0;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    errno = EFBIG;
    return -1;
}

/* Keep the 16 KiB protocol record out of recursive evaluator frames. Both
 * matrix compilers honor noinline; without it their cost model may reserve
 * this cold RPC frame in every shell-function invocation. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static int close_job_service_request(int outputs[2], int response[2],
                                     int status)
{
    if (outputs == NULL || response == NULL) return -1;
    if (outputs[0] >= 0) (void)close(outputs[0]);
    if (outputs[1] >= 0) (void)close(outputs[1]);
    if (response[0] >= 0) (void)close(response[0]);
    if (response[1] >= 0) (void)close(response[1]);
    return status;
}

static int finish_job_service_reply(ssize_t received,
                                    const job_service_reply *reply,
                                    int outputs[2], int response[2])
{
    int status = 125;

    if (!require(reply != NULL && outputs != NULL && response != NULL)) {
        return close_job_service_request(outputs, response, status);
    }
    if (!require(received >= -1)) {
        return close_job_service_request(outputs, response, status);
    }
    if (received != (ssize_t)sizeof(*reply) ||
        reply->version != GSH_JOB_SERVICE_VERSION || reply->reserved != 0U ||
        reply->status < 0 || reply->status > 255) {
        errno = EPROTO;
        return close_job_service_request(outputs, response, status);
    }
    if (copy_job_service_output(outputs[0], STDOUT_FILENO) == -1 ||
        copy_job_service_output(outputs[1], STDERR_FILENO) == -1) {
        return close_job_service_request(outputs, response, status);
    }
    return close_job_service_request(outputs, response, reply->status);
}

static int request_reactor_job_service(
    int service_socket, const gsh_native_command *command)
{
    job_service_request request;
    job_service_reply reply;
    int outputs[2] = {-1, -1};
    int response[2] = {-1, -1};
    int rights[GSH_JOB_SERVICE_RIGHTS];
    unsigned char control[CMSG_SPACE(sizeof(rights))];
    struct iovec payload = {&request, sizeof(request)};
    struct msghdr message;
    struct cmsghdr *header;
    struct pollfd ready;
    ssize_t sent;
    ssize_t received;
    size_t request_size;
    int status = 125;

    if (pack_job_service_request(command, &request) == -1) return 125;
    request_size = offsetof(job_service_request, text) +
                   request.text_length;
    payload.iov_len = request_size;
    outputs[0] = open_job_service_temp();
    outputs[1] = open_job_service_temp();
    if (outputs[0] == -1 || outputs[1] == -1 ||
        socketpair(AF_UNIX, SOCK_DGRAM, 0, response) == -1 ||
        set_fd_flags(response[0], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(response[1], F_GETFD, FD_CLOEXEC) == -1) {
        return close_job_service_request(outputs, response, status);
    }
    rights[0] = outputs[0];
    rights[1] = outputs[1];
    rights[2] = response[1];
    (void)memset(control, 0, sizeof(control));
    (void)memset(&message, 0, sizeof(message));
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(rights));
    (void)memcpy(CMSG_DATA(header), rights, sizeof(rights));
    do {
        sent = sendmsg(service_socket, &message, 0);
    } while (sent == -1 && errno == EINTR);
    if (sent != (ssize_t)request_size) {
        return close_job_service_request(outputs, response, status);
    }
    (void)close(response[1]);
    response[1] = -1;
    ready.fd = response[0];
    ready.events = POLLIN;
    ready.revents = 0;
    do {
        received = poll(&ready, 1U,
                        request.type == GSH_JOB_SERVICE_WAIT ? -1 : 5000);
    } while (received == -1 && errno == EINTR);
    if (received != 1 || (ready.revents & POLLIN) == 0) {
        if (received == 0) errno = ETIMEDOUT;
        return close_job_service_request(outputs, response, status);
    }
    do {
        received = recv(response[0], &reply, sizeof(reply), 0);
    } while (received == -1 && errno == EINTR);
    return finish_job_service_reply(received, &reply, outputs, response);
}

static int run_evaluator_job_control_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_store *scratch, const gsh_shell_options *options,
    gsh_background_table *jobs, int job_service_socket,
    bool job_service_available)
{
    if (jobs == NULL || pipeline == NULL || scratch == NULL || variables == NULL) {
        return -1;
    }
    const gsh_native_command *command = &pipeline->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    int status;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: job builtin redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command, options) == -1) {
        perror("gsh: job builtin redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    if (command->assignment_count != 0) {
        if (scratch == NULL) {
            status = 125;
        } else {
            (void)memcpy(scratch, variables, sizeof(*scratch));
            status = apply_native_assignments(
                scratch, NULL, command, options) == GSH_ASSIGNMENT_OK
                         ? 0 : 1;
        }
    } else {
        status = 0;
    }
    if (status == 0) {
        if (job_service_available &&
            native_snapshot_job_control_builtin(command)) {
            status = request_reactor_job_service(job_service_socket,
                                                 command);
        } else if (native_jobs_builtin(command)) {
            status = gsh_builtin_jobs(command->argc, command->argv, jobs,
                                      &descriptor_builtin_io);
        } else if (native_kill_builtin(command)) {
            status = gsh_builtin_kill(command->argc, command->argv, jobs,
                                      &descriptor_builtin_io);
        } else {
            status = gsh_builtin_error(
                &descriptor_builtin_io, command->argv[0],
                "not available outside the interactive reactor");
        }
    }
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: job builtin redirection restore");
        return 125;
    }
    return status == 125 ? 125
                         : (pipeline->negated ? (status == 0 ? 1 : 0)
                                              : status);
}

static int run_evaluator_trap_builtin(
    const gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_journal *journal, const gsh_shell_options *options,
    gsh_trap_store *traps, bool *builtin_failed)
{
    if (builtin_failed == NULL || pipeline == NULL || variables == NULL ||
        options == NULL) {
        return -1;
    }
    const gsh_native_command *command = &pipeline->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    int status;

    *builtin_failed = false;
    if (traps == NULL ||
        save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: trap redirection save");
        *builtin_failed = true;
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command, options) == -1) {
        perror("gsh: trap redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        *builtin_failed = true;
        return 1;
    }
    status = apply_special_builtin_assignments(
        variables, journal, command, options);
    if (status != GSH_ASSIGNMENT_OK) {
        perror("gsh: trap assignment");
        status = status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125 : 1;
        *builtin_failed = true;
    } else {
        status = gsh_builtin_trap((int)command->argc, command->argv,
                                  traps, &descriptor_builtin_io);
        *builtin_failed = status == 2 || status == 125;
    }
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: trap redirection restore");
        *builtin_failed = true;
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
    if (pipeline == NULL || variables == NULL || options == NULL ||
        (directory_capacity != 0U && directory == NULL)) {
        return -1;
    }
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
        if (scratch == NULL) {
            return 125;
        }
        (void)memcpy(scratch, variables, sizeof(*scratch));
        status = apply_native_assignments(scratch, NULL, command, options);
        if (status != GSH_ASSIGNMENT_OK) {
            perror("gsh: assignment");
            status = 1;
        } else {
            lookup_variables = scratch;
            status = run_native_cd_builtin(
                command, lookup_variables, variables, journal, options,
                &descriptor_builtin_io, directory, directory_capacity);
        }
    } else {
        status = run_native_cd_builtin(
            command, lookup_variables, variables, journal, options,
            &descriptor_builtin_io, directory, directory_capacity);
    }
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
    if (pipeline == NULL || variables == NULL || options == NULL) {
        return -1;
    }
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
    status = apply_special_builtin_assignments(
        variables, journal, command, options);
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
    if (command == NULL || heredoc_descriptors == NULL || options == NULL) {
        return;
    }
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
                (void)close(descriptor);
            }
            child_exec_error(redirect->target, saved_errno);
        }
        if (descriptor != redirect->descriptor) {
            (void)close(descriptor);
        }
    }
}

static void initialize_heredoc_descriptors(
    int descriptors[GSH_NATIVE_HEREDOC_CAP][2])
{
    if (descriptors == NULL) {
        return;
    }
    size_t index;

    for (index = 0; index < GSH_NATIVE_HEREDOC_CAP; index++) {
        descriptors[index][0] = -1;
        descriptors[index][1] = -1;
    }
}

static void close_heredoc_descriptors(
    int descriptors[GSH_NATIVE_HEREDOC_CAP][2], size_t count)
{
    if (descriptors == NULL) return;
    size_t index;

    for (index = 0; index < count; index++) {
        if (descriptors[index][0] >= 0) {
            (void)close(descriptors[index][0]);
            descriptors[index][0] = -1;
        }
        if (descriptors[index][1] >= 0) {
            (void)close(descriptors[index][1]);
            descriptors[index][1] = -1;
        }
    }
}

static void child_write_heredoc(
    const gsh_native_pipeline *pipeline, size_t heredoc_index,
    int descriptors[GSH_NATIVE_HEREDOC_CAP][2])
{
    if (descriptors == NULL || pipeline == NULL) {
        return;
    }
    const gsh_native_heredoc *heredoc =
        &pipeline->heredocs[heredoc_index];
    int descriptor = descriptors[heredoc_index][1];
    const char *cursor = heredoc->body;
    size_t remaining = heredoc->length;
    size_t index;

    for (index = 0; index < pipeline->heredoc_count; index++) {
        if (descriptors[index][0] >= 0) {
            (void)close(descriptors[index][0]);
        }
        if (index != heredoc_index && descriptors[index][1] >= 0) {
            (void)close(descriptors[index][1]);
        }
    }
    while (remaining > 0) {
        ssize_t written = gsh_fault_should_fail(GSH_FAULT_HEREDOC_WRITE, EIO)
                              ? -1
                              : write(descriptor, cursor, remaining);

        if (written > 0) {
            cursor += (size_t)written;
            remaining -= (size_t)written;
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else if (written == -1 && errno == EPIPE) {
            (void)close(descriptor);
            _exit(0);
        } else {
            int saved_errno = written == 0 ? EIO : errno;

            (void)close(descriptor);
            child_exec_error("here-document", saved_errno);
        }
    }
    (void)close(descriptor);
    _exit(0);
}

static void close_pipeline_descriptors(
    int descriptors[GSH_NATIVE_PIPELINE_CAP - 1][2], size_t count)
{
    if (descriptors == NULL) return;
    size_t index;

    for (index = 0; index < count; index++) {
        if (descriptors[index][0] >= 0) {
            (void)close(descriptors[index][0]);
            descriptors[index][0] = -1;
        }
        if (descriptors[index][1] >= 0) {
            (void)close(descriptors[index][1]);
            descriptors[index][1] = -1;
        }
    }
}

static void initialize_pipeline_descriptors(
    int descriptors[GSH_NATIVE_PIPELINE_CAP - 1][2])
{
    if (descriptors == NULL) {
        return;
    }
    size_t index;

    for (index = 0; index < GSH_NATIVE_PIPELINE_CAP - 1U; index++) {
        descriptors[index][0] = -1;
        descriptors[index][1] = -1;
    }
}

typedef struct {
    int pipes[GSH_NATIVE_PIPELINE_CAP - 1][2];
    int heredoc_pipes[GSH_NATIVE_HEREDOC_CAP][2];
    int gate[2];
    pid_t members[GSH_NATIVE_JOB_MEMBER_CAP];
    size_t pipe_count;
    size_t created_pipes;
    size_t created_heredocs;
    size_t launched;
    pid_t pgid;
    pid_t status_pid;
    sigset_t previous;
} native_pipeline_launch;

static void initialize_native_pipeline_launch(
    native_pipeline_launch *launch, size_t command_count)
{
    if (!require(launch != NULL)) return;
    initialize_pipeline_descriptors(launch->pipes);
    initialize_heredoc_descriptors(launch->heredoc_pipes);
    launch->gate[0] = -1;
    launch->gate[1] = -1;
    launch->pipe_count = command_count > 0U ? command_count - 1U : 0U;
    launch->created_pipes = 0U;
    launch->created_heredocs = 0U;
    launch->launched = 0U;
    launch->pgid = 0;
    launch->status_pid = -1;
    if (!require(command_count > 0U)) return;
}

static void cache_native_pipeline_commands(
    shell_state *state, const gsh_native_pipeline *pipeline)
{
    bool cache_changed = false;
    size_t index;

    if (!require(state != NULL)) return;
    if (!require(pipeline != NULL)) return;
    for (index = 0; index < pipeline->command_count; index++) {
        cache_changed = cache_planned_external(
                            state->command_cache, state->variables,
                            &pipeline->commands[index],
                            state->default_path, state->functions) ||
                        cache_changed;
    }
    if (cache_changed) {
        state->command_cache_generation++;
    }
}

static bool native_pipeline_capacity_is_available(shell_state *state)
{
    if (!require(state != NULL)) return false;
    if (!require(state->mode == MODE_DISPATCH)) return false;
    if (state->current_job.active) {
        (void)output_text(state,
                    "gsh: this MVP supports one job at a time; use fg or wait "
                    "for it\r\n");
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return false;
    }
    if (!gsh_background_has_capacity(&state->background_jobs)) {
        (void)output_text(state, "gsh: job registry full\r\n");
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return false;
    }
    return true;
}

static void reject_native_pipeline_setup(
    shell_state *state, native_pipeline_launch *launch,
    const char *operation, int error)
{
    if (operation == NULL) {
        return;
    }
    if (!require(state != NULL)) return;
    if (!require(launch != NULL)) return;
    output_format(state, "gsh: %s: %s\r\n", operation, strerror(error));
    if (launch->gate[0] >= 0) {
        (void)close(launch->gate[0]);
    }
    if (launch->gate[1] >= 0) {
        (void)close(launch->gate[1]);
    }
    close_pipeline_descriptors(launch->pipes, launch->created_pipes);
    close_heredoc_descriptors(launch->heredoc_pipes,
                              launch->created_heredocs);
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

static bool create_native_pipeline_descriptors(
    shell_state *state, const gsh_native_pipeline *pipeline,
    native_pipeline_launch *launch)
{
    if (launch == NULL) {
        return false;
    }
    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL)) return false;
    for (launch->created_heredocs = 0;
         launch->created_heredocs < pipeline->heredoc_count;
         launch->created_heredocs++) {
        if (make_pipe(launch->heredoc_pipes[launch->created_heredocs], false,
                      GSH_FAULT_HEREDOC_PIPE) == -1) {
            reject_native_pipeline_setup(state, launch,
                                         "here-document pipe", errno);
            return false;
        }
    }
    for (launch->created_pipes = 0;
         launch->created_pipes < launch->pipe_count;
         launch->created_pipes++) {
        if (make_pipe(launch->pipes[launch->created_pipes], false,
                      GSH_FAULT_PIPELINE_PIPE) == -1) {
            reject_native_pipeline_setup(state, launch,
                                         "pipeline pipe", errno);
            return false;
        }
    }
    if (make_pipe(launch->gate, false, GSH_FAULT_JOB_PIPE) == -1) {
        reject_native_pipeline_setup(state, launch, "launch gate", errno);
        return false;
    }
    return true;
}

static bool block_pipeline_child_notifications(
    shell_state *state, native_pipeline_launch *launch)
{
    sigset_t blocked;

    if (!require(state != NULL)) return false;
    if (!require(launch != NULL)) return false;
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &launch->previous) == -1) {
        reject_native_pipeline_setup(state, launch, "sigprocmask", errno);
        return false;
    }
    return true;
}

static bool primary_pipeline_builtin_status(
    shell_state *state, const gsh_native_command *command, int *status)
{
    if (!require(state != NULL)) return false;
    if (!require(command != NULL && status != NULL)) return false;
    if (native_pure_builtin(command)) {
        *status = run_native_pure_builtin(command, &descriptor_builtin_io);
        return true;
    }
    if (native_file_builtin(command)) {
        *status = run_native_file_builtin(command, &descriptor_builtin_io);
        return true;
    }
    if (native_posix_stateful_builtin(command)) {
        *status = child_run_posix_stateful_builtin(
            command, state->variables, state->pipeline_variables,
            state->positionals, &state->options);
        return true;
    }
    if (native_exit_builtin(command)) {
        if (apply_special_builtin_assignments(
                state->variables, NULL, command, &state->options) !=
            GSH_ASSIGNMENT_OK) {
            child_exec_error("exit assignment", errno);
        }
        (void)parse_exit_status(command, state->last_status, status);
        *status &= 255;
        return true;
    }
    if (native_pwd_builtin(command)) {
        *status = child_run_pwd(command, state->variables);
        return true;
    }
    if (native_cd_builtin(command)) {
        if (apply_native_assignments(state->variables, NULL, command,
                                     &state->options) != GSH_ASSIGNMENT_OK) {
            child_exec_error("assignment", errno);
        }
        *status = run_native_cd_builtin(
            command, state->variables, state->variables, NULL,
            &state->options, &descriptor_builtin_io, NULL, 0);
        return true;
    }
    if (native_environment_builtin(command)) {
        *status = run_native_environment_builtin(
            command, &descriptor_builtin_io);
        return true;
    }
    return false;
}

static bool state_pipeline_builtin_status(
    shell_state *state, const gsh_native_command *command, int *status)
{
    if (status == NULL) {
        return false;
    }
    if (!require(state != NULL)) return false;
    if (!require(command != NULL)) return false;
    if (native_variable_builtin(command)) {
        if (apply_special_builtin_assignments(
                state->variables, NULL, command, &state->options) !=
            GSH_ASSIGNMENT_OK) {
            child_exec_error("assignment", errno);
        }
        *status = run_native_variable_builtin(
            command, state->variables, NULL, &state->options, NULL,
            &descriptor_builtin_io);
        return true;
    }
    if (native_state_builtin(command)) {
        gsh_positional_store empty;
        gsh_positional_store *positionals = state->positionals;

        if (positionals == NULL) {
            gsh_positionals_initialize(&empty);
            positionals = &empty;
        }
        if (apply_special_builtin_assignments(
                state->variables, NULL, command, &state->options) !=
            GSH_ASSIGNMENT_OK) {
            child_exec_error("assignment", errno);
        }
        *status = run_native_state_builtin(
            command, state->variables, positionals, &state->options,
            &descriptor_builtin_io);
        return true;
    }
    if (native_job_control_builtin(command)) {
        if (native_jobs_builtin(command)) {
            *status = gsh_builtin_jobs(
                command->argc, command->argv, &state->background_jobs,
                &descriptor_builtin_io);
        } else if (native_kill_builtin(command)) {
            *status = gsh_builtin_kill(
                command->argc, command->argv, &state->background_jobs,
                &descriptor_builtin_io);
        } else {
            *status = gsh_builtin_error(
                &descriptor_builtin_io, command->argv[0],
                "not available in a pipeline");
        }
        return true;
    }
    if (native_wait_builtin(command)) {
        *status = command->argc == 1 ? 0 : 127;
        return true;
    }
    if (native_alias_builtin(command)) {
        *status = run_native_alias_builtin(
            command, state->aliases, NULL, &descriptor_builtin_io);
        return true;
    }
    return false;
}

static bool inspection_pipeline_builtin_status(
    shell_state *state, const gsh_native_command *command, int *status)
{
    if (status == NULL) {
        return false;
    }
    if (!require(state != NULL)) return false;
    if (!require(command != NULL)) return false;
    if (native_hash_builtin(command)) {
        const char *path = hash_command_path_value(
            state->variables, command, state->default_path);

        *status = run_native_hash_builtin(
            command, path, state->functions, state->command_cache,
            hash_command_path_generation(state->variables, command),
            NULL, &descriptor_builtin_io);
        return true;
    }
    if (native_times_builtin(command)) {
        if (apply_special_builtin_assignments(
                state->variables, NULL, command, &state->options) !=
            GSH_ASSIGNMENT_OK) {
            child_exec_error("assignment", errno);
        }
        *status = run_native_times_builtin(
            command, NULL, &descriptor_builtin_io);
        return true;
    }
    if (native_command_inspection_builtin(command)) {
        const char *path = command_path_value(
            state->variables, command, state->default_path);

        *status = run_native_command_inspection(
            command, path, state->default_path, state->aliases,
            state->functions, state->command_cache,
            gsh_variables_path_generation(state->variables),
            command_uses_persistent_path(command), NULL,
            &descriptor_builtin_io);
        return true;
    }
    return false;
}

static bool pipeline_builtin_status(
    shell_state *state, const gsh_native_command *command, int *status)
{
    if (!require(state != NULL)) return false;
    if (!require(status != NULL)) return false;
    if (primary_pipeline_builtin_status(state, command, status)) {
        return true;
    }
    if (state_pipeline_builtin_status(state, command, status)) {
        return true;
    }
    return inspection_pipeline_builtin_status(state, command, status);
}

static void close_pipeline_child_reactor(shell_state *state)
{
    if (!require(state != NULL)) _exit(125);
    if (!require(state->tty_fd >= 0)) _exit(125);
    (void)close(state->tty_fd);
    (void)close(state->signal_pipe[0]);
    (void)close(state->signal_pipe[1]);
    if (state->redirection_worker_fd >= 0) {
        (void)close(state->redirection_worker_fd);
    }
}

_Noreturn static void execute_native_pipeline_child(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const pipeline_expansion_scope *scope,
    native_pipeline_launch *launch, size_t index)
{
    if (launch == NULL || pipeline == NULL) {
        _exit(125);
    }
    const gsh_native_command *command = &pipeline->commands[index];
    char release;
    size_t close_index;
    int builtin_status;

    if (!require(state != NULL)) _exit(125);
    if (!require(index < pipeline->command_count)) _exit(125);
    (void)close(launch->gate[1]);
    (void)setpgid(0, launch->pgid == 0 ? 0 : launch->pgid);
    reset_child_signals();
    (void)sigprocmask(SIG_SETMASK, &launch->previous, NULL);
    if (command->expansion_error) {
        _exit(1);
    }
    if (scope != NULL &&
        gsh_variables_apply_journal_scope_in_place(
            state->variables, scope->changes, index + 1U) == -1) {
        child_exec_error("pipeline variable scope", errno);
    }
    if (index > 0 &&
        child_duplicate_descriptor(launch->pipes[index - 1U][0],
                                   STDIN_FILENO) == -1) {
        child_exec_error("pipeline input", errno);
    }
    if (index + 1U < pipeline->command_count &&
        child_duplicate_descriptor(launch->pipes[index][1],
                                   STDOUT_FILENO) == -1) {
        child_exec_error("pipeline output", errno);
    }
    child_apply_redirects(command, launch->heredoc_pipes,
                          pipeline->heredoc_count, &state->options);
    for (close_index = 0; close_index < launch->created_pipes;
         close_index++) {
        (void)close(launch->pipes[close_index][0]);
        (void)close(launch->pipes[close_index][1]);
    }
    close_heredoc_descriptors(launch->heredoc_pipes,
                              pipeline->heredoc_count);
    while (read(launch->gate[0], &release, sizeof(release)) == -1 &&
           errno == EINTR) {
    }
    (void)close(launch->gate[0]);
    close_pipeline_child_reactor(state);
    if (gsh_fault_should_fail(GSH_FAULT_EXEC, EIO)) {
        child_exec_error(command->argv[0], errno);
    }
    if (pipeline_builtin_status(state, command, &builtin_status)) {
        _exit(builtin_status);
    }
    {
        char *environment_storage[CHILD_ENVIRONMENT_CAP];
        char *const *environment = child_command_environment(
            state->variables, command, environment_storage);

        child_exec_direct(
            command->argv,
            command_path_value(state->variables, command,
                               state->default_path),
            environment, state->command_cache,
            command_cache_path_generation(state->variables, command),
            command_uses_persistent_path(command));
    }
    _exit(126);
}

static void reject_native_pipeline_fork(
    shell_state *state, native_pipeline_launch *launch,
    const char *operation, int error)
{
    if (operation == NULL) {
        return;
    }
    if (!require(state != NULL)) return;
    if (!require(launch != NULL)) return;
    close_pipeline_descriptors(launch->pipes, launch->created_pipes);
    close_heredoc_descriptors(launch->heredoc_pipes,
                              launch->created_heredocs);
    (void)close(launch->gate[0]);
    (void)close(launch->gate[1]);
    if (launch->pgid > 0) {
        (void)kill(-launch->pgid, SIGKILL);
        initialize_job(&state->current_job, launch->pgid,
                       launch->members[launch->launched - 1U],
                       launch->members, launch->launched,
                       launch->launched, false, false, false);
        state->current_job.silent = true;
        state->current_job.modes = state->original_modes;
    }
    (void)sigprocmask(SIG_SETMASK, &launch->previous, NULL);
    output_format(state, "gsh: %s: %s\r\n", operation, strerror(error));
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

/* ── Pipeline Group Assignment Accepts An Already Completed Handoff ──
 * Parent and child both establish the job's group before its launch gate opens.
 * Darwin can return EPERM to the second caller even when membership is correct.
 * Killing that child discarded pipeline output or a here-document at random.
 * Confirm the requested group after EPERM; a different group still fails closed.
 * ─────────────────────────────────────────────────────────────── */
static bool launch_native_pipeline_commands(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const pipeline_expansion_scope *scope,
    native_pipeline_launch *launch)
{
    size_t index;

    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL && launch != NULL)) return false;
    for (index = 0; index < pipeline->command_count; index++) {
        pid_t pid = gsh_fault_should_fail(GSH_FAULT_PIPELINE_FORK, EAGAIN) ? -1 : fork();

        if (pid == 0) {
            execute_native_pipeline_child(state, pipeline, scope, launch,
                                          index);
        }
        if (pid == -1) {
            int saved_errno = errno;

            reject_native_pipeline_fork(state, launch, "pipeline fork",
                                        saved_errno);
            return false;
        }
        if (launch->pgid == 0) {
            launch->pgid = pid;
        }
        launch->members[launch->launched++] = pid;
        if (setpgid(pid, launch->pgid) == -1 && errno != EACCES &&
            errno != ESRCH &&
            !(errno == EPERM && getpgid(pid) == launch->pgid)) {
            (void)kill(pid, SIGKILL);
        }
    }
    launch->status_pid = launch->members[pipeline->command_count - 1U];
    return true;
}

_Noreturn static void execute_pipeline_heredoc_child(
    shell_state *state, const gsh_native_pipeline *pipeline,
    native_pipeline_launch *launch, size_t index)
{
    if (pipeline == NULL) _exit(125);
    if (launch == NULL) {
        _exit(125);
    }
    char release;

    if (!require(state != NULL)) _exit(125);
    if (!require(index < pipeline->heredoc_count)) _exit(125);
    (void)close(launch->gate[1]);
    (void)setpgid(0, launch->pgid);
    reset_child_signals();
    (void)sigprocmask(SIG_SETMASK, &launch->previous, NULL);
    close_pipeline_descriptors(launch->pipes, launch->created_pipes);
    while (read(launch->gate[0], &release, sizeof(release)) == -1 &&
           errno == EINTR) {
    }
    (void)close(launch->gate[0]);
    close_pipeline_child_reactor(state);
    child_write_heredoc(pipeline, index, launch->heredoc_pipes);
    _exit(126);
}

static bool launch_pipeline_heredoc_writers(
    shell_state *state, const gsh_native_pipeline *pipeline,
    native_pipeline_launch *launch)
{
    size_t index;

    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL && launch != NULL)) return false;
    for (index = 0; index < pipeline->heredoc_count; index++) {
        pid_t pid = gsh_fault_should_fail(GSH_FAULT_HEREDOC_FORK, EAGAIN) ? -1 : fork();

        if (pid == 0) {
            execute_pipeline_heredoc_child(state, pipeline, launch, index);
        }
        if (pid == -1) {
            int saved_errno = errno;

            reject_native_pipeline_fork(state, launch,
                                        "here-document fork", saved_errno);
            return false;
        }
        launch->members[launch->launched++] = pid;
        if (setpgid(pid, launch->pgid) == -1 && errno != EACCES &&
            errno != ESRCH &&
            !(errno == EPERM && getpgid(pid) == launch->pgid)) {
            (void)kill(pid, SIGKILL);
        }
    }
    return true;
}

static void handoff_native_pipeline(
    shell_state *state, const gsh_native_pipeline *pipeline,
    native_pipeline_launch *launch)
{
    if (launch == NULL) {
        return;
    }
    if (!require(state != NULL)) return;
    if (!require(pipeline != NULL)) return;
    close_pipeline_descriptors(launch->pipes, launch->created_pipes);
    close_heredoc_descriptors(launch->heredoc_pipes,
                              launch->created_heredocs);
    (void)close(launch->gate[0]);
    initialize_job(&state->current_job, launch->pgid, launch->status_pid,
                   launch->members, launch->launched,
                   pipeline->command_count,
                   gsh_options_enabled(&state->options,
                                       GSH_OPTION_PIPEFAIL),
                   true,
                   pipeline->negated);
    state->current_job.modes = state->original_modes;
    if (gsh_fault_should_fail(GSH_FAULT_TERMINAL_HANDOFF, EIO) ||
        tcsetattr(state->tty_fd, TCSANOW, &state->original_modes) == -1 ||
        tcsetpgrp(state->tty_fd, launch->pgid) == -1) {
        int saved_errno = errno;

        state->current_job.foreground = false;
        state->current_job.silent = true;
        (void)kill(-launch->pgid, SIGKILL);
        (void)close(launch->gate[1]);
        (void)sigprocmask(SIG_SETMASK, &launch->previous, NULL);
        (void)enter_editor(state);
        output_format(state, "gsh: terminal handoff: %s\r\n",
                      strerror(saved_errno));
        queue_prompt(state);
        return;
    }
    state->terminal_changed = false;
    state->mode = MODE_FOREGROUND;
    (void)close(launch->gate[1]);
    (void)sigprocmask(SIG_SETMASK, &launch->previous, NULL);
}

static void start_native_pipeline(shell_state *state,
                                  const gsh_native_pipeline *pipeline,
                                  const pipeline_expansion_scope *scope)
{
    native_pipeline_launch launch;

    if (!require(state != NULL)) return;
    if (!require(pipeline != NULL)) return;
    if (scope == NULL) {
        cache_native_pipeline_commands(state, pipeline);
    }
    if (state->async_repl != NULL && state_async_repl(state)->enabled) {
        start_async_native_pipeline(state, pipeline, scope);
        return;
    }
    initialize_native_pipeline_launch(&launch, pipeline->command_count);
    if (!native_pipeline_capacity_is_available(state) ||
        !create_native_pipeline_descriptors(state, pipeline, &launch) ||
        !block_pipeline_child_notifications(state, &launch)) {
        return;
    }
    if (!launch_native_pipeline_commands(state, pipeline, scope, &launch) ||
        !launch_pipeline_heredoc_writers(state, pipeline, &launch)) {
        return;
    }
    handoff_native_pipeline(state, pipeline, &launch);
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
    (void)memcpy(pty->slave, name, strlen(name) + 1U);
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
    if (state == NULL) return;
    int index;

    gsh_llm_repl_close(&state->llm_repl);
    close_journal_pipe(state);

    if (state->tty_fd >= 0 && state->tty_fd != retained) {
        (void)close(state->tty_fd);
    }
    (void)close(state->signal_pipe[0]);
    (void)close(state->signal_pipe[1]);
    if (state->redirection_worker_fd >= 0) {
        (void)close(state->redirection_worker_fd);
    }
    if (state->async_repl == NULL) {
        return;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        int descriptor = state_async_repl(state)->cells[index].pty_fd;
        int resource_descriptor =
            state_async_repl(state)->cells[index].resource_fd;

        if (descriptor >= 0 && descriptor != retained) {
            (void)close(descriptor);
        }
        if (resource_descriptor >= 0 && resource_descriptor != retained) {
            (void)close(resource_descriptor);
        }
    }
}

static int attach_child_pty(shell_state *state, const managed_pty *pty)
{
    if (state == NULL) return -1;
    if (pty == NULL) {
        return -1;
    }
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
    (void)memset(&size, 0, sizeof(size));
    if (ioctl(state->tty_fd, TIOCGWINSZ, &size) == 0) {
        (void)ioctl(slave, TIOCSWINSZ, &size);
    }
    if (tcsetattr(slave, TCSANOW, &state->original_modes) == -1 ||
        tcsetpgrp(slave, getpgrp()) == -1 ||
        (!state->exec_standard_descriptor_changed[STDIN_FILENO] &&
         dup2(slave, STDIN_FILENO) == -1) ||
        (!state->exec_standard_descriptor_changed[STDOUT_FILENO] &&
         dup2(slave, STDOUT_FILENO) == -1) ||
        (!state->exec_standard_descriptor_changed[STDERR_FILENO] &&
         dup2(slave, STDERR_FILENO) == -1)) {
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
    if (state == NULL) {
        return;
    }
    char *environment_storage[CHILD_ENVIRONMENT_CAP];
    char *const *environment = child_command_environment(
        state->variables, NULL, environment_storage);
    char *shell_arguments[] = {(char *)"sh", (char *)"-c",
                               (char *)state->pending_input, NULL};

    if (direct != NULL) {
        child_exec_direct(direct->argv,
                          store_path_value(state->variables,
                                           state->default_path),
                          environment, state->command_cache,
                          gsh_variables_path_generation(state->variables),
                          true);
    }
    execve("/bin/sh", shell_arguments, environment);
    child_exec_error("/bin/sh", errno);
}

static void managed_external_child(shell_state *state, managed_pty *pty,
                                   int gate_read, int gate_write,
                                   const sigset_t *previous,
                                   simple_command *direct)
{
    if (direct == NULL || previous == NULL || pty == NULL) {
        return;
    }
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

static int register_managed_job(shell_state *state, int cell_index,
                                pid_t pid, pid_t pgid)
{
    const gsh_async_cell *cell;

    if (state == NULL || state->async_repl == NULL || cell_index < 0 ||
        cell_index >= GSH_ASYNC_CELL_CAP) {
        errno = EINVAL;
        return -1;
    }
    cell = &state_async_repl(state)->cells[cell_index];
    return gsh_background_add_job(
        &state->background_jobs, pgid, pid, &pid, 1U, 1U, false,
        cell->command, cell->command_length, GSH_JOB_ORIGIN_MANAGED, NULL);
}

static void start_async_external(shell_state *state, simple_command *direct)
{
    if (state == NULL) return;
    if (direct == NULL) {
        return;
    }
    managed_pty pty = {.master = -1, .slave_hold = -1};
    int gate[2] = {-1, -1};
    sigset_t blocked;
    sigset_t previous;
    pid_t pid;

    if (!gsh_background_has_capacity(&state->background_jobs)) {
        (void)output_text(state, "gsh: managed job registry full\r\n");
        state->mode = MODE_EDITOR;
        return;
    }
    if (open_managed_pty(&pty) == -1 ||
        make_pipe(gate, false, GSH_FAULT_JOB_PIPE) == -1) {
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
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
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
                         pid, pty.master, -1) == -1 ||
        register_managed_job(state, state->async_dispatch_cell,
                             pid, pid) == -1) {
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

static simple_command *prepare_external_direct(shell_state *state,
                                               simple_command *direct,
                                               const char *path_value)
{
    char resolved[GSH_COMMAND_PATH_CAP];
    bool cache_changed = false;

    if (!require(state != NULL && path_value != NULL)) return NULL;
    if (!require(state->command_cache != NULL)) return NULL;
    if (direct == NULL || !direct_path_is_bounded(direct, path_value)) {
        return NULL;
    }
    (void)gsh_command_cache_resolve(
        state->command_cache,
        gsh_variables_path_generation(state->variables), direct->argv[0],
        path_value, false, &cache_changed, resolved);
    if (cache_changed) state->command_cache_generation++;
    return direct;
}

static void execute_external_child(shell_state *state, simple_command *direct,
                                   const char *path_value, int gate_read,
                                   int gate_write, const sigset_t *previous)
{
    if (previous == NULL || state == NULL) {
        return;
    }
    char release;
    char *environment_storage[CHILD_ENVIRONMENT_CAP];
    char *const *environment;
    char *shell_arguments[] = {(char *)"sh", (char *)"-c",
                               (char *)state->pending_input, NULL};

    if (!require(state != NULL && path_value != NULL)) _exit(125);
    if (!require(gate_read >= 0 && gate_write >= 0)) _exit(125);
    environment = child_command_environment(state->variables, NULL,
                                            environment_storage);
    (void)close(gate_write);
    (void)setpgid(0, 0);
    reset_child_signals();
    (void)sigprocmask(SIG_SETMASK, previous, NULL);
    while (read(gate_read, &release, sizeof(release)) == -1 &&
           errno == EINTR) {
    }
    (void)close(gate_read);
    (void)close(state->tty_fd);
    (void)close(state->signal_pipe[0]);
    (void)close(state->signal_pipe[1]);
    if (state->redirection_worker_fd >= 0) {
        (void)close(state->redirection_worker_fd);
    }
    if (direct != NULL) {
        if (gsh_fault_should_fail(GSH_FAULT_EXEC, EIO)) {
            child_exec_error(direct->argv[0], errno);
        }
        child_exec_direct(
            direct->argv, path_value, environment, state->command_cache,
            gsh_variables_path_generation(state->variables), true);
    }
    if (gsh_fault_should_fail(GSH_FAULT_EXEC, EIO)) child_exec_error("/bin/sh", errno);
    execve("/bin/sh", shell_arguments, environment);
    child_write_text("gsh: cannot execute /bin/sh\n");
    _exit(127);
}

static void reject_external_fork(shell_state *state, int gate_write,
                                 const sigset_t *previous, int saved_errno)
{
    if (!require(state != NULL && previous != NULL)) return;
    if (!require(gate_write >= 0)) return;
    (void)close(gate_write);
    (void)sigprocmask(SIG_SETMASK, previous, NULL);
    output_format(state, "gsh: fork: %s\r\n", strerror(saved_errno));
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

static bool handoff_external_job(shell_state *state, pid_t pid,
                                 int gate_write,
                                 const sigset_t *previous)
{
    if (!require(state != NULL && previous != NULL)) return false;
    if (!require(pid > 0 && gate_write >= 0)) return false;
    initialize_job(&state->current_job, pid, pid, &pid, 1U, 1U,
                   false, true, false);
    state->current_job.modes = state->original_modes;
    (void)setpgid(pid, pid);
    if (gsh_fault_should_fail(GSH_FAULT_TERMINAL_HANDOFF, EIO) ||
        tcsetattr(state->tty_fd, TCSANOW, &state->original_modes) == -1 ||
        tcsetpgrp(state->tty_fd, pid) == -1) {
        int saved_errno = errno;

        state->current_job.foreground = false;
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        (void)close(gate_write);
        (void)sigprocmask(SIG_SETMASK, previous, NULL);
        (void)enter_editor(state);
        output_format(state, "gsh: terminal handoff: %s\r\n",
                      strerror(saved_errno));
        queue_prompt(state);
        return false;
    }
    state->terminal_changed = false;
    state->mode = MODE_FOREGROUND;
    (void)close(gate_write);
    (void)sigprocmask(SIG_SETMASK, previous, NULL);
    return true;
}

static void start_external(shell_state *state, simple_command *direct)
{
    if (direct == NULL) {
        return;
    }
    int gate[2];
    sigset_t blocked;
    sigset_t previous;
    const char *path_value;
    pid_t pid;

    if (!require(state != NULL)) return;
    if (!require(state->variables != NULL)) return;
    path_value = store_path_value(state->variables, state->default_path);
    if (!gsh_background_has_capacity(&state->background_jobs)) {
        (void)output_text(state, "gsh: job registry full\r\n");
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    direct = prepare_external_direct(state, direct, path_value);
    if (state->async_repl != NULL && state_async_repl(state)->enabled) {
        start_async_external(state, direct);
        return;
    }

    if (state->current_job.active) {
        (void)output_text(state,
                    "gsh: this MVP supports one job at a time; use fg or wait "
                    "for it\r\n");
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (make_pipe(gate, false, GSH_FAULT_JOB_PIPE) == -1) {
        output_format(state, "gsh: pipe: %s\r\n", strerror(errno));
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        (void)close(gate[0]);
        (void)close(gate[1]);
        output_format(state, "gsh: sigprocmask: %s\r\n", strerror(errno));
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    pid = gsh_fault_should_fail(GSH_FAULT_JOB_FORK, EAGAIN) ? -1 : fork();
    if (pid == 0) {
        execute_external_child(state, direct, path_value, gate[0], gate[1],
                               &previous);
    }

    (void)close(gate[0]);
    if (pid == -1) {
        int saved_errno = errno;

        reject_external_fork(state, gate[1], &previous, saved_errno);
        return;
    }
    (void)handoff_external_job(state, pid, gate[1], &previous);
}

/* ── Model Latency Never Enters The Reactor ─────────────────────
 * A network client inside the editor would let DNS, TLS, or inference stall
 * terminal input and job control. Each AI cell instead owns one worker process
 * and PTY, while a private inherited descriptor carries the prompt off argv.
 * The same boundary contains generated commands and lets normal job signals
 * cancel a request without giving the provider access to shell-owned state.
 * ─────────────────────────────────────────────────────────────── */
static int llm_worker_path(char *output, size_t capacity)
{
    const shell_executable_identity *identity = shell_executable_storage();
    const char *override = getenv("GSH_LLM_WORKER");
    const char *slash;
    size_t directory_length;
    int length;

    if (identity == NULL || output == NULL || capacity == 0U) return -1;
    if (override != NULL && override[0] != '\0') {
        length = snprintf(output, capacity, "%s", override);
        return length >= 0 && (size_t)length < capacity ? 0 : -1;
    }
    slash = strrchr(identity->path, '/');
    if (slash == NULL) {
        length = snprintf(output, capacity, "gsh-llm-worker");
        return length >= 0 && (size_t)length < capacity ? 0 : -1;
    }
    directory_length = (size_t)(slash - identity->path + 1);
    if (directory_length + sizeof("gsh-llm-worker") > capacity) return -1;
    (void)memcpy(output, identity->path, directory_length);
    (void)memcpy(output + directory_length, "gsh-llm-worker",
                 sizeof("gsh-llm-worker"));
    return 0;
}

static int prepare_llm_prompt_fd(int descriptor)
{
    int flags;

    if (descriptor <= STDERR_FILENO) return -1;
    flags = fcntl(descriptor, F_GETFD);
    if (flags == -1 || fcntl(descriptor, F_SETFD,
                             flags & ~FD_CLOEXEC) == -1) return -1;
    return 0;
}

static bool prepare_llm_repl_environment(shell_state *state)
{
    char descriptor[32];

    if (state == NULL) return false;
    if (state->llm_repl_peer < 0) return unsetenv("GSH_LLM_REPL_FD") == 0;
    if (prepare_llm_prompt_fd(state->llm_repl_peer) == -1 ||
        snprintf(descriptor, sizeof(descriptor), "%d", state->llm_repl_peer)
            >= (int)sizeof(descriptor)) return false;
    return setenv("GSH_LLM_REPL_FD", descriptor, 1) == 0;
}

_Noreturn static void exec_llm_worker(shell_state *state, int prompt_fd,
                                       const char *directory, bool pipeline,
                                       bool private)
{
    char worker[PATH_MAX];
    char descriptor[32];

    if (state == NULL || directory == NULL ||
        !prepare_llm_repl_environment(state) ||
        prepare_llm_prompt_fd(prompt_fd) == -1 ||
        llm_worker_path(worker, sizeof(worker)) == -1 ||
        snprintf(descriptor, sizeof(descriptor), "%d", prompt_fd) >=
            (int)sizeof(descriptor) || chdir(directory) == -1 ||
        (private ? setenv("GSH_LLM_PRIVATE", "1", 1)
                 : unsetenv("GSH_LLM_PRIVATE")) == -1) _exit(125);
    if (strchr(worker, '/') == NULL)
        execlp(worker, "gsh-llm-worker",
               pipeline ? "--pipeline-fd" : "--prompt-fd", descriptor,
               (char *)NULL);
    else
        execl(worker, "gsh-llm-worker",
              pipeline ? "--pipeline-fd" : "--prompt-fd", descriptor,
              (char *)NULL);
    child_exec_error(worker, errno);
}

static bool send_llm_prompt(int descriptor, const char *prompt,
                            size_t length)
{
    size_t offset = 0U;

    if (descriptor < 0 || prompt == NULL) return false;
    while (offset < length) {
        ssize_t count = write(descriptor, prompt + offset, length - offset);

        if (count > 0) offset += (size_t)count;
        else if (count == -1 && errno == EINTR) continue;
        else break;
    }
    return close(descriptor) == 0 && offset == length;
}

static bool llm_pipeline_parts(const char *line, size_t length,
                               size_t *command_length,
                               size_t *instruction_offset,
                               size_t *instruction_length)
{
    unsigned char quote = 0U;
    bool escaped = false;
    size_t pipe_offset = length;
    size_t index;
    size_t end;

    if (line == NULL || command_length == NULL ||
        instruction_offset == NULL || instruction_length == NULL)
        return false;
    for (index = 0U; index < length; index++) {
        unsigned char byte = (unsigned char)line[index];

        if (escaped) { escaped = false; continue; }
        if (quote == '\'') { if (byte == '\'') quote = 0U; continue; }
        if (byte == '\\') { escaped = true; continue; }
        if (quote == '"') { if (byte == '"') quote = 0U; continue; }
        if (byte == '\'' || byte == '"') { quote = byte; continue; }
        if (byte == '|' && (index == 0U || line[index - 1U] != '|') &&
            (index + 1U >= length || line[index + 1U] != '|'))
            pipe_offset = index;
    }
    if (quote != 0U || escaped || pipe_offset == length) return false;
    index = pipe_offset + 1U;
    while (index < length && (line[index] == ' ' || line[index] == '\t'))
        index++;
    if (index >= length || line[index] != '?' ||
        (index + 1U < length && line[index + 1U] == '?')) return false;
    index++;
    while (index < length && (line[index] == ' ' || line[index] == '\t'))
        index++;
    end = length;
    while (end > index && (line[end - 1U] == ' ' || line[end - 1U] == '\t'))
        end--;
    while (pipe_offset > 0U &&
           (line[pipe_offset - 1U] == ' ' || line[pipe_offset - 1U] == '\t'))
        pipe_offset--;
    if (pipe_offset == 0U || index == end) return false;
    *command_length = pipe_offset;
    *instruction_offset = index;
    *instruction_length = end - index;
    return true;
}

static bool build_llm_pipeline_payload(const char *line, size_t length,
                                       char *payload, size_t capacity,
                                       size_t *payload_length)
{
    size_t command_length;
    size_t instruction_offset;
    size_t instruction_length;

    if (payload == NULL || payload_length == NULL ||
        !llm_pipeline_parts(line, length, &command_length,
                            &instruction_offset, &instruction_length) ||
        command_length + 1U + instruction_length > capacity) return false;
    (void)memcpy(payload, line, command_length);
    payload[command_length] = '\0';
    (void)memcpy(payload + command_length + 1U,
                 line + instruction_offset, instruction_length);
    *payload_length = command_length + 1U + instruction_length;
    return true;
}

static void managed_llm_child(shell_state *state, managed_pty *pty,
                              int prompt_read, int prompt_write,
                              const sigset_t *previous,
                              const char *directory, bool pipeline,
                              bool private)
{
    if (state == NULL || pty == NULL || previous == NULL ||
        directory == NULL) _exit(125);
    (void)close(prompt_write);
    (void)close(pty->master);
    reset_child_signals();
    if (attach_child_pty(state, pty) == -1) _exit(125);
    (void)sigprocmask(SIG_SETMASK, previous, NULL);
    close_child_reactor_descriptors(state, prompt_read);
    exec_llm_worker(state, prompt_read, directory, pipeline, private);
}

static void reject_managed_llm(shell_state *state, managed_pty *pty,
                               int prompt_read, int prompt_write,
                               const sigset_t *previous, int saved_errno)
{
    if (pty != NULL && pty->master >= 0) (void)close(pty->master);
    if (pty != NULL && pty->slave_hold >= 0) (void)close(pty->slave_hold);
    if (prompt_read >= 0) (void)close(prompt_read);
    if (prompt_write >= 0) (void)close(prompt_write);
    if (previous != NULL) (void)sigprocmask(SIG_SETMASK, previous, NULL);
    if (state == NULL) return;
    gsh_llm_repl_close(&state->llm_repl);
    if (state->llm_repl_peer >= 0) (void)close(state->llm_repl_peer);
    state->llm_repl_peer = -1;
    state->llm_owner_cell = -1;
    output_format(state, "gsh: LLM launch: %s\r\n", strerror(saved_errno));
    gsh_async_repl_finish(state->async_repl, state->async_dispatch_cell,
                          125 << 8, false);
    state->mode = MODE_EDITOR;
}

static bool managed_llm_payload(const gsh_async_cell *cell, char *payload,
                                size_t capacity, const char **text,
                                size_t *length)
{
    size_t offset;

    if (cell == NULL || payload == NULL || text == NULL || length == NULL)
        return false;
    if (cell->ai_pipeline) {
        *text = payload;
        return build_llm_pipeline_payload(
            cell->command, cell->command_length, payload, capacity, length);
    }
    offset = cell->command_length > 1U && cell->command[1] == '?' ? 2U : 1U;
    while (offset < cell->command_length &&
           (cell->command[offset] == ' ' || cell->command[offset] == '\t'))
        offset++;
    *text = cell->command + offset;
    *length = cell->command_length - offset;
    return true;
}

static void start_async_llm(shell_state *state, int cell_index)
{
    managed_pty pty = {.master = -1, .slave_hold = -1};
    int prompt[2] = {-1, -1};
    sigset_t blocked;
    sigset_t previous;
    pid_t pid;
    gsh_async_cell *cell;
    char payload[GSH_ASYNC_COMMAND_CAP];
    const char *prompt_text;
    size_t prompt_length;
    bool private;

    if (state == NULL || cell_index < 0 ||
        cell_index >= GSH_ASYNC_CELL_CAP) return;
    cell = &state_async_repl(state)->cells[cell_index];
    private = state->config.history_ignore_space &&
              cell->command_length >= 2U && cell->command[0] == ' ' &&
              cell->command[cell->command_length - 1U] == ' ';
    if (!managed_llm_payload(cell, payload, sizeof(payload), &prompt_text,
                             &prompt_length)) {
        (void)output_text(state, "gsh: invalid LLM pipeline\r\n");
        gsh_async_repl_finish(state->async_repl, cell_index, 2 << 8, false);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    gsh_llm_repl_close(&state->llm_repl);
    state->llm_owner_cell = cell_index;
    state->llm_command_cell = -1;
    if (gsh_llm_repl_open(&state->llm_repl, &state->llm_repl_peer) == -1 ||
        open_managed_pty(&pty) == -1 ||
        make_pipe(prompt, false, GSH_FAULT_JOB_PIPE) == -1) {
        reject_managed_llm(state, &pty, prompt[0], prompt[1], NULL, errno);
        return;
    }
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        reject_managed_llm(state, &pty, prompt[0], prompt[1], NULL, errno);
        return;
    }
    pid = fork();
    if (pid == 0)
        managed_llm_child(state, &pty, prompt[0], prompt[1], &previous,
                          cell->launch_directory, cell->ai_pipeline, private);
    if (state->llm_repl_peer >= 0) (void)close(state->llm_repl_peer);
    state->llm_repl_peer = -1;
    (void)close(pty.slave_hold);
    (void)close(prompt[0]);
    if (pid < 0 || gsh_async_repl_attach(state->async_repl, cell_index, pid,
                                         pid, pty.master, -1) == -1 ||
        register_managed_job(state, cell_index, pid, pid) == -1 ||
        !send_llm_prompt(prompt[1], prompt_text, prompt_length)) {
        int saved_errno = errno;
        if (pid > 0) (void)kill(pid, SIGKILL);
        reject_managed_llm(state, &pty, -1, -1, &previous, saved_errno);
        return;
    }
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

/* ── AI Execution Uses Cells In Every Interactive Session ───────
 * A foreground provider occupied the classic job slot, forcing its tools
 * into disposable child shells. Entering the existing managed view gives
 * every generated command a normal REPL cell and leaves the reactor free
 * to execute it. This changes only the current view, never configuration;
 * /async remains the explicit way to return to classic terminal handoff.
 * ─────────────────────────────────────────────────────────────── */
static void start_classic_llm_request(shell_state *state,
                                      const char *prompt_text,
                                      size_t prompt_length, bool pipeline,
                                      bool private)
{
    char command[GSH_ASYNC_COMMAND_CAP];
    char prompt[GSH_ASYNC_PROMPT_CAP];
    int cell;
    int length;

    if (state == NULL || prompt_text == NULL ||
        prompt_length >= sizeof(command) - 8U) return;
    if (pipeline) {
        const char *separator = memchr(prompt_text, '\0', prompt_length);
        if (separator == NULL || separator + 1U >= prompt_text + prompt_length)
            return;
        length = snprintf(command, sizeof(command), "%s%.*s | ? %s%s",
                           private ? " " : "", (int)(separator - prompt_text),
                           prompt_text, separator + 1U, private ? " " : "");
    } else {
        length = snprintf(command, sizeof(command), "? %.*s",
                           (int)prompt_length, prompt_text);
    }
    if (length <= 0 || (size_t)length >= sizeof(command)) return;
    gsh_async_repl_initialize(state->async_repl, true);
    gsh_async_repl_configure_actions(state->async_repl,
        terminal_actions_requested(&state->config, true),
        state->config.path_detection);
    state->async_desired = true;
    state->async_transition_pending = false;
    initialize_repl_size(state);
    make_editor_modes(state);
    if (enter_editor(state) == -1) { state->running = false; return; }
    (void)active_prompt_text(state, prompt);
    cell = pipeline
        ? gsh_async_repl_accept_ai_pipeline(state->async_repl, prompt, command,
                                            (size_t)length,
                                            state->current_directory)
        : gsh_async_repl_accept_ai(state->async_repl, prompt, command,
                                   (size_t)length, state->current_directory);
    capture_llm_context(state, cell);
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

static void start_classic_llm(shell_state *state, const char *prompt_text,
                              size_t prompt_length)
{
    start_classic_llm_request(state, prompt_text, prompt_length, false,
                              false);
}

static bool dispatch_classic_auto_help(shell_state *state)
{
    char prompt[LINE_CAP];
    int length;

    if (state == NULL || !state->auto_help_pending) return false;
    if (state->output_len > 0U) {
        flush_output(state);
        if (state->output_len > 0U) return true;
    }
    state->auto_help_pending = false;
    length = snprintf(
        prompt, sizeof(prompt),
        "The following top-level command failed with exit status %d. "
        "Diagnose it and suggest the smallest safe fix. Do not run commands "
        "unless needed.\n\nCommand:\n%s", state->last_status,
        state->pending_line);
    if (length <= 0 || (size_t)length >= sizeof(prompt)) {
        queue_prompt(state);
        return true;
    }
    start_classic_llm(state, prompt, (size_t)length);
    return true;
}

static char *trim_command(char *command)
{
    if (command == NULL) {
        return NULL;
    }
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
    if (default_path == NULL || variables == NULL) {
        return NULL;
    }
    bool found;
    const char *path =
        gsh_variables_lookup(variables, "PATH", 4, &found);

    return found ? path : default_path;
}

static const char *command_path_override(
    const gsh_native_command *command, const char *path)
{
    if (path == NULL) {
        return NULL;
    }
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

static bool command_uses_persistent_path(
    const gsh_native_command *command)
{
    size_t index;

    if (command == NULL || command->command_uses_default_path) {
        return false;
    }
    for (index = 0; index < command->assignment_count; index++) {
        if (memcmp(command->assignments[index], "PATH=", 5) == 0) {
            return false;
        }
    }
    return true;
}

static uint64_t command_cache_path_generation(
    const gsh_variable_store *variables,
    const gsh_native_command *command)
{
    if (variables == NULL) {
        return 0U;
    }
    uint64_t generation = gsh_variables_path_generation(variables);

    if (command_uses_persistent_path(command)) {
        return generation;
    }
    return ~generation;
}

static const char *hash_command_path_value(
    const gsh_variable_store *variables,
    const gsh_native_command *command, const char *default_path)
{
    if (command == NULL || default_path == NULL || variables == NULL) {
        return NULL;
    }
    return command_path_override(
        command, store_path_value(variables, default_path));
}

static uint64_t hash_command_path_generation(
    const gsh_variable_store *variables,
    const gsh_native_command *command)
{
    if (command == NULL || variables == NULL) {
        return 0U;
    }
    uint64_t generation = gsh_variables_path_generation(variables);
    size_t index;

    for (index = 0; index < command->assignment_count; index++) {
        if (memcmp(command->assignments[index], "PATH=", 5) == 0) {
            return ~generation;
        }
    }
    return generation;
}

static bool command_can_populate_cache(
    const gsh_native_command *command,
    const gsh_function_store *functions)
{
    if (functions == NULL) {
        return false;
    }
    const char *name;
    size_t length;

    if (command == NULL || command->argc == 0 ||
        !command_uses_persistent_path(command)) {
        return false;
    }
    name = command->argv[0];
    length = strnlen(name, GSH_COMMAND_PATH_CAP);
    return length != 0 && length != GSH_COMMAND_PATH_CAP &&
           strchr(name, '/') == NULL &&
           memchr(name, '\n', length) == NULL &&
           !gsh_command_intrinsic_name(name, length) &&
           (command->command_suppresses_functions || functions == NULL ||
            gsh_functions_lookup(functions, name, length) == NULL);
}

static bool cache_planned_external(
    gsh_command_cache *cache, const gsh_variable_store *variables,
    const gsh_native_command *command, const char *default_path,
    const gsh_function_store *functions)
{
    if (default_path == NULL || variables == NULL) {
        return false;
    }
    char resolved[GSH_COMMAND_PATH_CAP];
    const char *name;
    const char *path;
    bool changed = false;

    if (cache == NULL ||
        !command_can_populate_cache(command, functions)) {
        return false;
    }
    name = command->argv[0];
    path = command_path_value(variables, command, default_path);
    (void)gsh_command_cache_resolve(
        cache, gsh_variables_path_generation(variables), name, path,
        false, &changed, resolved);
    return changed;
}

static const char *command_path_value(
    const gsh_variable_store *variables, const gsh_native_command *command,
    const char *default_path)
{
    if (default_path == NULL || variables == NULL) {
        return NULL;
    }
    if (command != NULL && command->command_uses_default_path) {
        return default_path;
    }
    return command_path_override(
        command, store_path_value(variables, default_path));
}

static const char *scoped_command_path_value(
    const pipeline_expansion_scope *scope, unsigned int command_scope,
    const gsh_native_command *command, const char *default_path)
{
    if (default_path == NULL || scope == NULL) {
        return NULL;
    }
    gsh_variable_journal_value_state state;
    const char *path = gsh_variable_journal_lookup_scoped(
        scope->changes, command_scope, "PATH", 4, &state);

    if (command != NULL && command->command_uses_default_path) {
        return default_path;
    }
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

static shell_state *main_transaction_state(
    const main_expansion_transaction *transaction)
{
    if (!require(transaction != NULL)) return NULL;
    if (!require(transaction->state != NULL)) return NULL;
    return transaction->state;
}

static void commit_main_transaction(main_expansion_transaction *transaction)
{
    if (transaction == NULL) return;
    if (transaction->isolated || !transaction->mutated) {
        return;
    }
    if (gsh_variables_apply_journal_in_place(
            main_transaction_state(transaction)->variables,
            main_transaction_state(transaction)->variable_commit) == 0) {
        main_transaction_state(transaction)->variable_generation++;
    } else {
        (void)output_text(transaction->state,
                    "gsh: variable transaction commit failed\r\n");
    }
}

static int ensure_main_positionals(shell_state *state)
{
    if (state == NULL) return -1;
    if (state->positionals != NULL) {
        return 0;
    }
    if (gsh_fault_should_fail(GSH_FAULT_POSITIONAL_ALLOCATION, ENOMEM)) {
        return -1;
    }
    state->positionals = state->positional_storage;
    gsh_positionals_initialize(state->positionals);
    return 0;
}

static bool start_async_stateless_redirection(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const gsh_native_command *command)
{
    if (state == NULL || command == NULL) return false;
    if (pipeline == NULL) {
        return false;
    }
    const gsh_native_redirect *redirect;
    redirection_request request;
    mode_t mask;
    int builtin_status;
    int length;
    ssize_t sent;

    if (!state->redirection_worker_alive || state->redirection_worker_busy ||
        gsh_fault_active() || command->assignment_count != 0 ||
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
    (void)memset(&request, 0, sizeof(request));
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
    request.version = REDIRECTION_WORKER_PROTOCOL_VERSION;
    request.request_id = state->redirection_next_request_id++;
    request.operator_kind = (uint32_t)redirect->operator_kind;
    request.option_bits = state->options.enabled;
    request.creation_mode = (uint32_t)(0666 & ~mask);
    request.builtin_status = builtin_status;
    do {
        sent = send(state->redirection_worker_fd, &request,
                    sizeof(request), 0);
    } while (sent == -1 && errno == EINTR);
    if (sent != (ssize_t)sizeof(request)) {
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                           errno == ENOBUFS)) {
            return false;
        }
        state->redirection_worker_failures++;
        disable_redirection_worker(state, true);
        return false;
    }
    state->redirection_worker_busy = true;
    state->redirection_active_request_id = request.request_id;
    state->redirection_pipeline_negated = pipeline->negated;
    (void)memcpy(state->redirection_target, request.directory,
           (size_t)length + 1U);
    state->mode = MODE_ASYNC_REDIRECTION;
    return true;
}

static void begin_background_wait(shell_state *state,
                                  const gsh_native_pipeline *pipeline)
{
    if (pipeline == NULL || state == NULL) {
        return;
    }
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

            if (text[0] == '%') {
                uint32_t job_id;

                if (gsh_background_resolve(&state->background_jobs, text,
                                           &job_id) == GSH_JOBSPEC_OK) {
                    target = gsh_background_job_pid(
                        &state->background_jobs, job_id);
                }
            } else {
                errno = 0;
                number = strtoul(text, &end, 10);
                if (errno == 0 && *text != '\0' && *end == '\0' &&
                    number > 0 && number <= (unsigned long)INT_MAX) {
                    target = (pid_t)number;
                }
            }
            state->wait_targets[state->wait_target_count++] = target;
        }
    }
    state->mode = MODE_WAIT;
    (void)finish_background_wait(state);
}

static void run_fg(shell_state *state,
                   const gsh_native_command *command, bool negated);
static void run_bg(shell_state *state,
                   const gsh_native_command *command, bool negated);

static bool finish_main_builtin(shell_state *state,
                                const gsh_native_pipeline *pipeline,
                                int status)
{
    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL)) return false;
    state->last_status = pipeline->negated ? (status == 0 ? 1 : 0) : status;
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return true;
}

static bool run_main_simple_builtin(shell_state *state,
                                    const gsh_native_pipeline *pipeline,
                                    const gsh_native_command *command)
{
    if (pipeline == NULL) {
        return false;
    }
    if (!require(state != NULL)) return false;
    if (!require(command != NULL)) return false;
    if (native_times_builtin(command) && command->redirect_count == 0 &&
        command->assignment_count == 0) {
        const gsh_builtin_io io = reactor_builtin_sink(state);
        int status = run_native_times_builtin(command, NULL, &io);

        return finish_main_builtin(state, pipeline, status);
    }
    if (!pipeline->negated && command->redirect_count == 0 &&
        command->assignment_count == 0 &&
        native_environment_builtin(command)) {
        const gsh_builtin_io io = reactor_builtin_sink(state);
        int status = run_native_environment_builtin(command, &io);

        return finish_main_builtin(state, pipeline, status);
    }
    return false;
}

static bool run_main_job_control_builtin(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const gsh_native_command *command)
{
    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL && command != NULL)) return false;
    const gsh_builtin_io io = reactor_builtin_sink(state);
    int status = 0;

    if (!native_job_control_builtin(command) ||
        command->redirect_count != 0) {
        return false;
    }
    if (command->assignment_count != 0) {
        (void)memcpy(state->variable_scratch, state->variables,
               sizeof(*state->variable_scratch));
        if (apply_native_assignments(state->variable_scratch, NULL, command,
                                     &state->options) != GSH_ASSIGNMENT_OK) {
            status = 1;
        }
    }
    if (status == 0 && native_fg_builtin(command)) {
        run_fg(state, command, pipeline->negated);
        return true;
    }
    if (status == 0 && native_bg_builtin(command)) {
        run_bg(state, command, pipeline->negated);
        return true;
    }
    if (status == 0) {
        status = native_jobs_builtin(command)
                     ? gsh_builtin_jobs(command->argc, command->argv,
                                        &state->background_jobs, &io)
                     : gsh_builtin_kill(command->argc, command->argv,
                                        &state->background_jobs, &io);
    }
    return finish_main_builtin(state, pipeline, status);
}

static bool run_main_getopts_or_exec(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const gsh_native_command *command)
{
    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL && command != NULL)) return false;
    if (native_getopts_builtin(command) && command->assignment_count == 0 &&
        command->redirect_count == 0) {
        const gsh_builtin_io io = reactor_builtin_sink(state);
        int status = run_native_posix_stateful_builtin(
            command, state->variables, state->variables,
            state->variable_scratch, NULL, state->positionals,
            &state->options, &io);

        state->variable_generation++;
        return finish_main_builtin(state, pipeline, status);
    }
    if (native_exec_builtin(command)) {
        bool builtin_failed;
        int status = run_evaluator_exec_builtin(
            pipeline, state->variables, NULL, &state->options,
            state->default_path, state->command_cache, state,
            -1, NULL, &builtin_failed);

        (void)builtin_failed;
        if (command->assignment_count != 0) {
            state->variable_generation++;
        }
        state->last_status = status;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    return false;
}

static bool run_main_alias_builtin(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const gsh_native_command *command)
{
    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL && command != NULL)) return false;
    const gsh_builtin_io io = reactor_builtin_sink(state);
    bool mutates;
    int status;

    if (!native_alias_builtin(command) || command->redirect_count != 0) {
        return false;
    }
    mutates = native_alias_mutates(command);
    if (mutates && ensure_alias_state(state, false) == -1) {
        output_format(state, "gsh: alias allocation: %s\r\n",
                      strerror(errno));
        status = 125;
    } else if (command->assignment_count == 0) {
        status = run_native_alias_builtin(command, state->aliases, NULL,
                                          &io);
    } else {
        (void)memcpy(state->variable_scratch, state->variables,
               sizeof(*state->variable_scratch));
        if (apply_native_assignments(state->variable_scratch, NULL, command,
                                     &state->options) != GSH_ASSIGNMENT_OK) {
            output_format(state, "gsh: assignment: %s\r\n",
                          strerror(errno));
            status = 1;
        } else {
            status = run_native_alias_builtin(command, state->aliases, NULL,
                                              &io);
        }
    }
    if (mutates && state->aliases != NULL) {
        state->alias_generation++;
    }
    return finish_main_builtin(state, pipeline, status);
}

static bool run_main_inspection_builtin(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const gsh_native_command *command)
{
    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL && command != NULL)) return false;
    const gsh_builtin_io io = reactor_builtin_sink(state);
    bool cache_changed = false;
    int status;

    if (native_hash_builtin(command) && command->redirect_count == 0) {
        const char *path = hash_command_path_value(
            state->variables, command, state->default_path);

        status = run_native_hash_builtin(
            command, path, state->functions, state->command_cache,
            hash_command_path_generation(state->variables, command),
            &cache_changed, &io);
    } else if (native_command_inspection_builtin(command) &&
               command->redirect_count == 0 &&
               gsh_functions_lookup(state->functions, command->argv[0],
                                    strlen(command->argv[0])) == NULL) {
        const char *path = command_path_value(
            state->variables, command, state->default_path);

        status = run_native_command_inspection(
            command, path, state->default_path, state->aliases,
            state->functions, state->command_cache,
            gsh_variables_path_generation(state->variables),
            command_uses_persistent_path(command), &cache_changed, &io);
    } else {
        return false;
    }
    if (cache_changed) {
        state->command_cache_generation++;
    }
    return finish_main_builtin(state, pipeline, status);
}

static bool run_main_state_builtin(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const gsh_native_command *command)
{
    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL && command != NULL)) return false;
    const gsh_builtin_io io = reactor_builtin_sink(state);
    int status;

    if (!native_state_builtin(command) || command->redirect_count != 0) {
        return false;
    }
    if (ensure_main_positionals(state) == -1) {
        (void)output_text(state, "gsh: positional parameter allocation failed\r\n");
        status = 125;
    } else if (apply_special_builtin_assignments(
                   state->variables, NULL, command, &state->options) !=
               GSH_ASSIGNMENT_OK) {
        output_format(state, "gsh: assignment: %s\r\n", strerror(errno));
        status = 1;
    } else {
        status = run_native_state_builtin(
            command, state->variables, state->positionals, &state->options,
            &io);
    }
    state->variable_generation++;
    return finish_main_builtin(state, pipeline, status);
}

static bool run_main_variable_builtin(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const gsh_native_command *command)
{
    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL && command != NULL)) return false;
    const gsh_builtin_io io = reactor_builtin_sink(state);
    bool function_mutates;
    int status;

    if (!native_variable_builtin(command) || command->redirect_count != 0 ||
        native_variable_listing(command)) {
        return false;
    }
    function_mutates = native_function_mutates(command);
    if (apply_special_builtin_assignments(
            state->variables, NULL, command, &state->options) !=
        GSH_ASSIGNMENT_OK) {
        output_format(state, "gsh: assignment: %s\r\n", strerror(errno));
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
    return finish_main_builtin(state, pipeline, status);
}

static bool run_main_assignment_builtin(
    shell_state *state, const gsh_native_pipeline *pipeline,
    const gsh_native_command *command)
{
    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL && command != NULL)) return false;
    if (native_colon_builtin(command) && command->assignment_count != 0 &&
        command->redirect_count == 0) {
        int status = apply_special_builtin_assignments(
            state->variables, NULL, command, &state->options);

        state->last_status = status == GSH_ASSIGNMENT_OK
                                 ? (pipeline->negated ? 1 : 0)
                                 : 1;
        if (status != GSH_ASSIGNMENT_OK) {
            output_format(state, "gsh: assignment: %s\r\n",
                          strerror(errno));
        }
        state->variable_generation++;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    if (pipeline->negated || command->redirect_count != 0 ||
        command->argc != 0 || command->assignment_count == 0) {
        return false;
    }
    if (apply_native_assignments(state->variables, NULL, command,
                                 &state->options) != GSH_ASSIGNMENT_OK) {
        output_format(state, "gsh: assignment: %s\r\n", strerror(errno));
        state->last_status = 1;
    } else {
        state->last_status = command->command_substitution_performed
                                 ? command->command_substitution_status
                                 : 0;
    }
    state->variable_generation++;
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return true;
}

static bool run_main_cd_builtin(shell_state *state,
                                const gsh_native_command *command)
{
    if (!require(state != NULL)) return false;
    if (!require(command != NULL)) return false;
    const gsh_builtin_io io = reactor_builtin_sink(state);
    const gsh_variable_store *lookup_variables = state->variables;

    if (!native_cd_builtin(command)) return false;
    if (command->assignment_count != 0) {
        (void)memcpy(state->variable_scratch, state->variables,
               sizeof(*state->variable_scratch));
        if (apply_native_assignments(state->variable_scratch, NULL, command,
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
        command, lookup_variables, state->variables, NULL, &state->options,
        &io, state->current_directory, sizeof(state->current_directory));
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return true;
}

static bool run_main_exit_builtin(shell_state *state,
                                  const gsh_native_command *command)
{
    if (!require(state != NULL)) return false;
    if (!require(command != NULL)) return false;
    if (command->assignment_count != 0 || command->argc == 0 ||
        strcmp(command->argv[0], "exit") != 0) {
        return false;
    }
    if (command->argc > 2) {
        (void)output_text(state, "gsh: exit: too many operands\r\n");
        state->last_status = 1;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    if (command->argc == 2) {
        char *end;
        long status = strtol(command->argv[1], &end, 10);

        if (*end != '\0') {
            (void)output_text(state, "gsh: exit: numeric argument required\r\n");
            state->last_status = 2;
            state->running = false;
            return true;
        }
        state->last_status = (int)((unsigned long)status & 255U);
    }
    state->running = false;
    return true;
}

static bool run_planned_main_builtin(shell_state *state,
                                     const gsh_native_pipeline *pipeline)
{
    const gsh_native_command *command;

    if (!require(state != NULL)) return false;
    if (!require(pipeline != NULL)) return false;
    if (pipeline->command_count != 1) {
        return false;
    }
    command = &pipeline->commands[0];
    if (command->argc != 0 &&
        !command->command_suppresses_functions &&
        !special_builtin_name(command->argv[0],
                              strlen(command->argv[0])) &&
        gsh_functions_lookup(state->functions, command->argv[0],
                             strlen(command->argv[0])) != NULL) {
        return false;
    }
    if (start_async_stateless_redirection(state, pipeline, command)) {
        return true;
    }
    if (native_wait_builtin(command) && command->redirect_count == 0) {
        begin_background_wait(state, pipeline);
        return true;
    }
    if (run_main_simple_builtin(state, pipeline, command) ||
        run_main_job_control_builtin(state, pipeline, command) ||
        run_main_getopts_or_exec(state, pipeline, command) ||
        run_main_alias_builtin(state, pipeline, command) ||
        run_main_inspection_builtin(state, pipeline, command) ||
        run_main_state_builtin(state, pipeline, command) ||
        run_main_variable_builtin(state, pipeline, command) ||
        run_main_assignment_builtin(state, pipeline, command)) {
        return true;
    }
    if (pipeline->negated || command->redirect_count != 0) return false;
    if (run_main_cd_builtin(state, command)) return true;
    if (native_environment_builtin(command)) {
        const gsh_builtin_io io = reactor_builtin_sink(state);

        state->last_status = run_native_environment_builtin(command, &io);
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return true;
    }
    return run_main_exit_builtin(state, command);
}

static const gsh_background_entry *resolve_reactor_job(
    shell_state *state, const gsh_native_command *command,
    const char *builtin_name, uint32_t *job_id)
{
    if (command == NULL) return NULL;
    if (builtin_name == NULL || job_id == NULL || state == NULL) {
        return NULL;
    }
    const char *jobspec;
    gsh_jobspec_status status;

    if (command->argc > 2U) {
        output_format(state, "gsh: %s: too many operands\r\n",
                      builtin_name);
        return NULL;
    }
    jobspec = command->argc == 2U ? command->argv[1] : "%%";
    status = gsh_background_resolve(&state->background_jobs, jobspec,
                                    job_id);
    if (status == GSH_JOBSPEC_OK) {
        const gsh_background_entry *entry =
            gsh_background_entry_for_id(&state->background_jobs, *job_id);

        if (entry != NULL && entry->state != GSH_JOB_DONE) return entry;
        status = GSH_JOBSPEC_MISSING;
    }
    if (status == GSH_JOBSPEC_AMBIGUOUS) {
        output_format(state,
                      "gsh: %s: ambiguous job specification\r\n",
                      builtin_name);
    } else if (status == GSH_JOBSPEC_INVALID) {
        output_format(state, "gsh: %s: invalid job specification\r\n",
                      builtin_name);
    } else {
        output_format(state, "gsh: %s: no such job\r\n", builtin_name);
    }
    return NULL;
}

static void finish_immediate_job_builtin(shell_state *state, int status,
                                         bool negated)
{
    if (state == NULL) {
        return;
    }
    state->last_status = negated ? (status == 0 ? 1 : 0) : status;
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

static void load_current_job_from_service(
    shell_state *state, const gsh_background_entry *entry, bool negated)
{
    if (entry == NULL || state == NULL) {
        return;
    }
    initialize_job(&state->current_job, entry->pgid, entry->status_pid,
                   entry->members, entry->member_count,
                   entry->pipeline_status.command_count,
                   entry->pipeline_status.pipefail, true, negated);
    (void)memcpy(state->current_job.member_states, entry->member_states,
           entry->member_count * sizeof(entry->member_states[0]));
    state->current_job.remaining = entry->remaining;
    state->current_job.stopped = entry->state == GSH_JOB_STOPPED;
    state->current_job.pipeline_status = entry->pipeline_status;
    state->current_job.modes = state->original_modes;
}

static void run_managed_fg(shell_state *state,
                           const gsh_background_entry *entry,
                           uint32_t job_id, bool negated)
{
    int cell_index;

    if (!require(state != NULL && entry != NULL)) return;
    if (!require(entry->origin == GSH_JOB_ORIGIN_MANAGED)) return;
    cell_index = gsh_async_repl_cell_for_pid(state->async_repl,
                                             entry->status_pid);
    if (cell_index < 0 ||
        gsh_async_repl_focus(state->async_repl, cell_index) == -1) {
        (void)output_text(state, "gsh: fg: managed job unavailable\r\n");
        finish_immediate_job_builtin(state, 1, negated);
        return;
    }
    if (entry->state == GSH_JOB_STOPPED &&
        signal_managed_job(state, cell_index, SIGCONT) == -1) {
        output_format(state, "gsh: fg: %s\r\n", strerror(errno));
        finish_immediate_job_builtin(state, 1, negated);
        return;
    }
    if (entry->state == GSH_JOB_STOPPED) {
        (void)gsh_background_continue_job(&state->background_jobs, job_id);
        gsh_async_repl_mark_running(state->async_repl, cell_index);
    }
    if (state_async_repl(state)->cells[cell_index].fullscreen) {
        (void)signal_managed_job(state, cell_index, SIGWINCH);
    }
    output_format(state, "[focused cell %llu; Ctrl-] returns to editor]\r\n",
                  (unsigned long long)
                      state_async_repl(state)->cells[cell_index].id);
    finish_immediate_job_builtin(state, 0, negated);
}

static void run_fg(shell_state *state,
                   const gsh_native_command *command, bool negated)
{
    if (command == NULL || state == NULL) {
        return;
    }
    const gsh_background_entry *entry;
    gsh_background_entry *mutable_entry;
    uint32_t job_id = 0;
    bool reconstructed = false;

    entry = resolve_reactor_job(state, command, "fg", &job_id);
    if (entry == NULL) {
        finish_immediate_job_builtin(state, 1, negated);
        return;
    }
    if (entry->origin == GSH_JOB_ORIGIN_MANAGED) {
        run_managed_fg(state, entry, job_id, negated);
        return;
    }
    if (state->current_job.active &&
        state->current_job.pgid != entry->pgid) {
        if (state->variable_commit_active ||
            state->pending_and_or_active || state->pending_list_active) {
            (void)output_text(state,
                        "gsh: fg: another foreground transaction is "
                        "suspended\r\n");
            finish_immediate_job_builtin(state, 1, negated);
            return;
        }
        (void)memset(&state->current_job, 0, sizeof(state->current_job));
    }
    if (!state->current_job.active) {
        load_current_job_from_service(state, entry, negated);
        reconstructed = true;
    } else {
        state->current_job.negated = negated;
    }
    if (entry->command_length != 0) {
        (void)output_push(state, entry->command, entry->command_length);
        (void)output_text(state, "\r\n");
    }
    if (tcsetattr(state->tty_fd, TCSANOW, &state->current_job.modes) == -1 ||
        tcsetpgrp(state->tty_fd, state->current_job.pgid) == -1) {
        int saved_errno = errno;

        if (reconstructed) {
            (void)memset(&state->current_job, 0, sizeof(state->current_job));
        }
        output_format(state, "gsh: fg: %s\r\n", strerror(saved_errno));
        (void)enter_editor(state);
        finish_immediate_job_builtin(state, 1, negated);
        return;
    }
    state->terminal_changed = false;
    state->current_job.foreground = true;
    if (state->current_job.stopped) {
        size_t member;

        if (kill(-state->current_job.pgid, SIGCONT) == -1) {
            int saved_errno = errno;

            reclaim_terminal(state, false);
            output_format(state, "gsh: fg: %s\r\n",
                          strerror(saved_errno));
            finish_immediate_job_builtin(state, 1, negated);
            return;
        }
        state->current_job.stopped = false;
        for (member = 0; member < state->current_job.member_count;
             member++) {
            if (state->current_job.member_states[member] ==
                JOB_MEMBER_STOPPED) {
                state->current_job.member_states[member] =
                    JOB_MEMBER_RUNNING;
            }
        }
        (void)gsh_background_continue_job(&state->background_jobs, job_id);
    }
    mutable_entry = gsh_background_mutable_entry_for_id(
        &state->background_jobs, job_id);
    if (mutable_entry != NULL) {
        mutable_entry->foreground = true;
        mutable_entry->terminal_owned = true;
    }
    state->mode = MODE_FOREGROUND;
}

static void run_bg(shell_state *state,
                   const gsh_native_command *command, bool negated)
{
    if (command == NULL || state == NULL) {
        return;
    }
    const gsh_background_entry *entry;
    gsh_background_entry *mutable_entry;
    uint32_t job_id = 0;

    entry = resolve_reactor_job(state, command, "bg", &job_id);
    if (entry == NULL) {
        finish_immediate_job_builtin(state, 1, negated);
        return;
    }
    if (entry->state != GSH_JOB_STOPPED) {
        (void)output_text(state, "gsh: bg: job is not stopped\r\n");
        finish_immediate_job_builtin(state, 1, negated);
        return;
    }
    if (entry->origin == GSH_JOB_ORIGIN_MANAGED) {
        int cell_index = gsh_async_repl_cell_for_pid(
            state->async_repl, entry->status_pid);

        if (cell_index < 0 ||
            signal_managed_job(state, cell_index, SIGCONT) == -1) {
            output_format(state, "gsh: bg: %s\r\n",
                          cell_index < 0 ? "managed job unavailable"
                                         : strerror(errno));
            finish_immediate_job_builtin(state, 1, negated);
            return;
        }
        (void)gsh_background_continue_job(&state->background_jobs, job_id);
        gsh_async_repl_mark_running(state->async_repl, cell_index);
        (void)output_text(state, "[continued]\r\n");
        finish_immediate_job_builtin(state, 0, negated);
        return;
    }
    if (kill(-entry->pgid, SIGCONT) == -1) {
        output_format(state, "gsh: bg: %s\r\n", strerror(errno));
        finish_immediate_job_builtin(state, 1, negated);
        return;
    }
    (void)gsh_background_continue_job(&state->background_jobs, job_id);
    mutable_entry = gsh_background_mutable_entry_for_id(
        &state->background_jobs, job_id);
    if (mutable_entry != NULL) {
        mutable_entry->foreground = false;
        mutable_entry->terminal_owned = false;
    }
    if (state->current_job.active &&
        state->current_job.pgid == entry->pgid) {
        size_t member;

        state->current_job.stopped = false;
        state->current_job.foreground = false;
        for (member = 0; member < state->current_job.member_count;
             member++) {
            if (state->current_job.member_states[member] ==
                JOB_MEMBER_STOPPED) {
                state->current_job.member_states[member] =
                    JOB_MEMBER_RUNNING;
            }
        }
        if (!state->variable_commit_active &&
            !state->pending_and_or_active && !state->pending_list_active) {
            (void)memset(&state->current_job, 0, sizeof(state->current_job));
        }
    }
    output_format(state, "[continued %ld]\r\n", (long)entry->pgid);
    finish_immediate_job_builtin(state, 0, negated);
}

static bool begin_native_list(shell_state *state)
{
    if (state == NULL) return false;
    const gsh_ast_node *program;
    const gsh_ast_node *list;
    size_t child;
    bool parent_owned = false;

    if (state->pending_parse.status != GSH_PARSE_OK ||
        state->pending_parse.root >= state_parse_storage(state)->node_count) {
        return false;
    }
    program = &state_parse_storage(state)->nodes[state->pending_parse.root];
    if (program->kind != GSH_AST_PROGRAM ||
        program->first_child == GSH_AST_NONE ||
        state_parse_storage(state)->nodes[program->first_child].next_sibling !=
            GSH_AST_NONE) {
        return false;
    }
    list = &state_parse_storage(state)->nodes[program->first_child];
    if (list->kind != GSH_AST_LIST) {
        return false;
    }
    child = list->first_child;
    while (child != GSH_AST_NONE) {
        if ((state_parse_storage(state)->nodes[child].flags &
             GSH_AST_FLAG_ASYNC) != 0) {
            parent_owned = true;
        }
        if (native_list_node_is_wait(state, child)) {
            parent_owned = true;
        }
        child = state_parse_storage(state)->nodes[child].next_sibling;
    }
    if (!parent_owned || !native_command_is_supported(state)) {
        return false;
    }
    state->pending_list_active = true;
    state->pending_list_next = list->first_child;
    state->pending_and_or_active = false;
    state->pending_and_or_next = GSH_AST_NONE;
    state->mode = MODE_DISPATCH;
    return true;
}

static size_t single_function_definition(const shell_state *state)
{
    if (state == NULL) {
        return 0U;
    }
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

        if (node >= state_parse_storage(state)->node_count) {
            return GSH_AST_NONE;
        }
        current = &state_parse_storage(state)->nodes[node];
        if (current->kind != wrappers[index] ||
            current->first_child == GSH_AST_NONE ||
            state_parse_storage(state)->nodes[current->first_child].next_sibling !=
                GSH_AST_NONE ||
            (current->flags & (GSH_AST_FLAG_ASYNC |
                               GSH_AST_FLAG_NEGATED)) != 0) {
            return GSH_AST_NONE;
        }
        node = current->first_child;
    }
    return node < state_parse_storage(state)->node_count &&
                   state_parse_storage(state)->nodes[node].kind == GSH_AST_FUNCTION
               ? node
               : GSH_AST_NONE;
}

static bool run_function_definition(shell_state *state)
{
    if (state == NULL) {
        return false;
    }
    size_t node_index = single_function_definition(state);
    const gsh_ast_node *node;
    gsh_word_ref name;
    int changed;

    if (node_index == GSH_AST_NONE) {
        return false;
    }
    node = &state_parse_storage(state)->nodes[node_index];
    name = state_parse_storage(state)->words[node->first_word];
    if (special_builtin_name(state->pending_input + name.begin,
                             name.end - name.begin)) {
        (void)output_text(state,
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
    if (entry == NULL || functions == NULL || status == NULL) {
        return false;
    }
    const gsh_parse_storage *storage = &functions->programs;
    const char *input = gsh_functions_text(functions);
    size_t node_index = entry->node_offset;
    bool negated = false;
    size_t step;

    for (step = 0; step < AST_WALK_STEP_CAP; step++) {
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
    return false;
}

static bool run_pure_function(shell_state *state,
                              const gsh_native_pipeline *pipeline)
{
    if (pipeline == NULL) return false;
    if (state == NULL) {
        return false;
    }
    const gsh_native_command *command;
    const gsh_function_entry *function;
    int status;

    if (gsh_fault_active() || pipeline->command_count != 1U) {
        return false;
    }
    command = &pipeline->commands[0];
    function = command->argc == 0 ||
                       command->command_suppresses_functions
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

static void request_async_transition(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    bool effective = state->async_repl != NULL &&
                     state_async_repl(state)->enabled;
    bool immediate = false;

    if (state->async_transition_pending) {
        state->async_desired = effective;
        state->async_transition_pending = false;
        output_format(state,
                      "async repl: transition cancelled; remains %s\r\n",
                      effective ? "on" : "off");
    } else {
        state->async_desired = !effective;
        state->async_transition_pending = true;
        immediate = async_transition_can_start_now(state);
        if (!immediate) {
            output_format(state, "async repl: %s pending\r\n",
                          state->async_desired ? "on" : "off");
        }
    }
    state->last_status = 0;
    state->mode = MODE_EDITOR;
    if (!immediate) {
        queue_prompt(state);
    }
}

static bool dispatch_history_control(shell_state *state, const char *command)
{
    if (!require(state != NULL)) return false;
    if (!require(command != NULL)) return false;
    if (strcmp(command, "history status") == 0 ||
        strcmp(command, "history") == 0) {
        output_format(state,
                      "history enabled=%s persistent=%s entries=%zu "
                      "max=%zu file=%s\r\n",
                      state->config.history_enabled ? "yes" : "no",
                      state->history_persistent ? "yes" : "no",
                      state->history == NULL ? 0U : state_history(state)->count,
                      state->config.history_max_entries,
                      state->history_file.path[0] == '\0'
                          ? "-" : state->history_file.path);
        state->last_status = 0;
    } else if (strncmp(command, "history ", 8U) == 0) {
        (void)output_text(state, "usage: history [status]\r\n");
        state->last_status = 2;
    } else {
        return false;
    }
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return true;
}

static bool dispatch_reactor_control(shell_state *state, const char *command)
{
    if (!require(state != NULL)) return false;
    if (!require(command != NULL)) return false;
    if (strcmp(command, "rt") == 0) {
        output_format(state,
                      "reactor cycles=%llu max=%.3fms deadline=5.000ms "
                      "misses=%llu dispatches=%llu dispatch_max=%.3fms "
                      "dispatch_misses=%llu overloads=%llu direct=%llu "
                      "native=%llu shell=%llu parsed=%llu parse_failures=%llu "
                      "job=%s worker=%s busy=%u failures=%llu "
                      "async_jobs=%zu focus=%s protected_bridge=%llu\r\n",
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
                      state->redirection_worker_alive ? "on" : "off",
                      state->redirection_worker_busy ? 1U : 0U,
                      (unsigned long long)state->redirection_worker_failures,
                      gsh_async_repl_job_count(state->async_repl),
                      gsh_async_repl_focused_job(state->async_repl) >= 0
                          ? "job"
                          : "editor",
                      (unsigned long long)state->protected_bridge_dispatches);
    } else if (strcmp(command, "help") == 0) {
        (void)output_text(state,
                    "builtins: cd [path], exit [status], fg, bg, rt, help, "
                    "/async, history [status]\r\n"
                    "non-canonical PTYs receive contained full-screen focus; "
                    "Ctrl-] returns to the editor\r\n"
                    "simple commands use native execve; shell syntax falls "
                    "back to /bin/sh -c\r\n");
    } else {
        return false;
    }
    state->last_status = 0;
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return true;
}

typedef struct {
    main_expansion_transaction transaction;
    char *positional_view[GSH_POSITIONAL_CAP];
    char option_flags[GSH_OPTION_FLAG_CAP];
    gsh_parse_result parsed;
    gsh_native_plan_status plan_status;
    bool deferred_work;
    gsh_builtin_io diagnostic_io;
    gsh_native_variable_state variable_state;
    gsh_native_expansion_context expansion;
} pending_native_dispatch;

static void initialize_pending_native_dispatch(
    shell_state *state, pending_native_dispatch *pending)
{
    size_t positional_count;

    if (!require(state != NULL)) return;
    if (!require(pending != NULL)) return;
    (void)memset(pending, 0, sizeof(*pending));
    positional_count = gsh_positionals_count(state->positionals);
    pending->transaction.state = state;
    pending->parsed = state->pending_parse;
    pending->plan_status = GSH_NATIVE_PLAN_UNSUPPORTED;
    pending->diagnostic_io = reactor_builtin_sink(state);
    pending->variable_state.mode = GSH_NATIVE_VARIABLE_OVERLAY;
    pending->variable_state.variables = state->variables;
    pending->variable_state.journal = state->variable_commit;
    pending->variable_state.journal_generation = state->variable_generation;
    pending->variable_state.scope_changes = state->pipeline_changes;
    pending->variable_state.attributes = assignment_attributes(&state->options);
    pending->variable_state.diagnostic_io = &pending->diagnostic_io;
    pending->variable_state.carriage_return = true;
    pending->expansion.last_status = state->last_status;
    pending->expansion.shell_pid = (long)state->shell_pgid;
    pending->expansion.last_background_pid = state->last_background_pid;
    pending->expansion.parameter_zero = state->parameter_zero;
    pending->expansion.positional_parameters =
        positional_count == 0 ? NULL : pending->positional_view;
    pending->expansion.positional_count = positional_count;
    pending->expansion.option_flags = pending->option_flags;
    pending->expansion.variable_state = &pending->variable_state;
    pending->expansion.pathname_mode =
        gsh_options_enabled(&state->options, GSH_OPTION_NOGLOB)
            ? GSH_NATIVE_PATHNAME_PREFLIGHT
            : GSH_NATIVE_PATHNAME_REJECT;
    pending->expansion.defer_complex_patterns = true;
    pending->expansion.deferred_work = &pending->deferred_work;
    pending->expansion.nounset =
        gsh_options_enabled(&state->options, GSH_OPTION_NOUNSET);
    gsh_options_flags(&state->options, pending->option_flags);
    gsh_positionals_view(state->positionals, pending->positional_view);
}

static bool plan_pending_native_dispatch(shell_state *state,
                                         pending_native_dispatch *pending)
{
    if (!require(state != NULL)) return true;
    if (!require(pending != NULL)) return true;
    if (pending->parsed.status == GSH_PARSE_OK) {
        state->parsed_dispatches++;
        if (run_function_definition(state) || begin_native_list(state) ||
            (!gsh_options_enabled(&state->options, GSH_OPTION_XTRACE) &&
             try_native_reactor_compound(state))) {
            state->native_pipeline_dispatches++;
            return true;
        }
        pending->plan_status = gsh_native_plan_pipeline_with_context(
            state->pending_input, state->parse_storage, pending->parsed.root,
            &pending->expansion, state->native_pipeline);
        if (pending->plan_status == GSH_NATIVE_PLAN_OK) {
            normalize_command_invocations(state->native_pipeline,
                                          state->functions);
            if (!pending->deferred_work &&
                gsh_options_enabled(&state->options, GSH_OPTION_XTRACE) &&
                emit_pipeline_trace(&pending->diagnostic_io,
                                    state->native_pipeline,
                                    state->variables) == -1) {
                pending->plan_status = GSH_NATIVE_PLAN_ERROR;
            }
        }
    } else {
        state->parse_failures++;
    }
    pending->transaction.mutated = pending->variable_state.mutated;
    pending->transaction.isolated = pending->variable_state.isolated;
    pending->transaction.scope.base = pending->variable_state.scope_base;
    pending->transaction.scope.changes = pending->variable_state.scope_changes;
    pending->transaction.scope.command_count =
        pending->variable_state.command_count;
    if (pending->plan_status != GSH_NATIVE_PLAN_ERROR) return false;
    state->last_status = 1;
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return true;
}

static bool planned_pipeline_is_supported(
    shell_state *state, const pending_native_dispatch *pending)
{
    const gsh_native_pipeline *pipeline;
    bool single_function;

    if (!require(state != NULL && pending != NULL)) return false;
    pipeline = state->native_pipeline;
    if (!require(pipeline != NULL)) return false;
    single_function =
        pipeline->command_count == 1U && pipeline->commands[0].argc != 0 &&
        !pipeline->commands[0].command_suppresses_functions &&
        gsh_functions_lookup(state->functions, pipeline->commands[0].argv[0],
                             strlen(pipeline->commands[0].argv[0])) != NULL;
    return pending->plan_status == GSH_NATIVE_PLAN_OK &&
           !pending->deferred_work &&
           !native_pipeline_requires_evaluator(pipeline) && !single_function &&
           native_pipeline_is_supported_scoped(
               pipeline, state->default_path, state->variables,
               pending->transaction.isolated ? &pending->transaction.scope
                                             : NULL);
}

static bool run_pending_pure_builtin(shell_state *state)
{
    const gsh_native_pipeline *pipeline;
    gsh_builtin_io io;
    int status;

    if (!require(state != NULL)) return false;
    pipeline = state->native_pipeline;
    if (!require(pipeline != NULL)) return false;
    io = reactor_builtin_sink(state);
    if (pipeline->command_count != 1U ||
        pipeline->commands[0].redirect_count != 0U ||
        !native_pure_builtin(&pipeline->commands[0])) {
        return false;
    }
    status = run_native_pure_builtin(&pipeline->commands[0], &io);
    if (pipeline->negated) status = status == 0 ? 1 : 0;
    state->last_status = status;
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return true;
}

static bool dispatch_planned_native(shell_state *state,
                                    pending_native_dispatch *pending)
{
    char direct_storage[LINE_CAP];
    simple_command direct = {0};

    if (!require(state != NULL)) return false;
    if (!require(pending != NULL)) return false;
    if (prepare_simple_command(state->pending_input, direct_storage, &direct) &&
        gsh_functions_lookup(state->functions, direct.argv[0],
                             strlen(direct.argv[0])) == NULL) {
        state->direct_dispatches++;
        start_external(state, &direct);
        return true;
    }
    if (pending->plan_status == GSH_NATIVE_PLAN_OK &&
        !pending->deferred_work) {
        commit_main_transaction(&pending->transaction);
        if (run_planned_main_builtin(state, state->native_pipeline) ||
            run_pure_function(state, state->native_pipeline)) {
            state->native_pipeline_dispatches++;
            return true;
        }
    }
    if (planned_pipeline_is_supported(state, pending)) {
        state->native_pipeline_dispatches++;
        if (run_pending_pure_builtin(state)) return true;
        start_native_pipeline(
            state, state->native_pipeline,
            pending->transaction.isolated ? &pending->transaction.scope
                                          : NULL);
        return true;
    }
    if (pending->parsed.status == GSH_PARSE_OK &&
        native_command_is_supported(state)) {
        state->native_pipeline_dispatches++;
        start_native_compound(state, pending->parsed.root);
        return true;
    }
    return false;
}

static void dispatch_pending(shell_state *state)
{
    pending_native_dispatch pending;
    char *command;

    if (!require(state != NULL)) return;
    if (!require(state->pending_input != NULL)) return;
    if (state->pending_and_or_active) {
        continue_native_and_or(state);
        return;
    }
    if (state->pending_list_active) {
        continue_native_list(state);
        return;
    }
    command = trim_command((char *)state->pending_input);

    if (*command == '\0') {
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }

    if (!state->pending_alias_expanded &&
        state->pending_input == state->pending_line &&
        strcmp(state->pending_line, "/async") == 0) {
        request_async_transition(state);
        return;
    }

    if (dispatch_history_control(state, command) ||
        dispatch_reactor_control(state, command)) {
        return;
    }

    initialize_pending_native_dispatch(state, &pending);
    if (plan_pending_native_dispatch(state, &pending)) return;
    if (dispatch_planned_native(state, &pending)) return;
    if (state->pending_alias_expanded) {
        (void)output_text(state,
                    "gsh: native alias expansion produced an unsupported "
                    "command\r\n");
        state->last_status = 2;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (fallback_mentions_protected_builtin(
            state->pending_input, state->pending_input_length)) {
        (void)output_text(state,
                    "gsh: native builtin ownership prevents compatibility "
                    "fallback\r\n");
        state->last_status = 2;
        state->mode = MODE_EDITOR;
        queue_prompt(state);
        return;
    }
    if (state->protected_bridge_dispatches != 0U) {
        state->last_status = 125;
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
    if (command == NULL) {
        return false;
    }
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
    if (state == NULL) {
        return false;
    }
    size_t node_index = state->pending_parse.root;
    unsigned int depth;

    for (depth = 0; depth < 6U; depth++) {
        const gsh_ast_node *node;
        size_t child;

        if (node_index == GSH_AST_NONE ||
            node_index >= state_parse_storage(state)->node_count) {
            return false;
        }
        node = &state_parse_storage(state)->nodes[node_index];
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
            child >= state_parse_storage(state)->node_count) {
            return false;
        }
        if (state_parse_storage(state)->nodes[child].next_sibling != GSH_AST_NONE) {
            return node->kind == GSH_AST_PIPELINE;
        }
        node_index = child;
    }
    return false;
}

static size_t managed_assignment_name_length(const char *input,
                                             gsh_word_ref word)
{
    if (input == NULL) return 0U;
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
    if (input == NULL) {
        return false;
    }
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
    if (input == NULL || text == NULL) {
        return false;
    }
    size_t length = strlen(text);

    return managed_plain_word(input, word) &&
           word.end - word.begin == length &&
           memcmp(input + word.begin, text, length) == 0;
}

static bool managed_variable_affects_launch(const shell_state *state,
                                            gsh_word_ref word,
                                            size_t name_length)
{
    if (state == NULL) {
        return false;
    }
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
    if (node == NULL) return false;
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

    if (node->first_word > state_parse_storage(state)->word_count ||
        node->word_count >
            state_parse_storage(state)->word_count - node->first_word) {
        return true;
    }
    while (assignment_count < node->word_count) {
        gsh_word_ref word = state_parse_storage(state)->words[
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
    command = state_parse_storage(state)->words[
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
    if (state == NULL) return false;
    size_t stack[GSH_PARSE_NODE_CAP];
    size_t stack_count = 0;
    size_t visited = 0;

    if (state->pending_parse.status != GSH_PARSE_OK ||
        state->pending_alias_expanded ||
        state->pending_parse.root >= state_parse_storage(state)->node_count) {
        return true;
    }
    stack[stack_count++] = state->pending_parse.root;
    while (stack_count != 0 && visited++ < GSH_PARSE_NODE_CAP) {
        size_t node_index = stack[--stack_count];
        const gsh_ast_node *node;
        size_t child;
        size_t sibling_count = 0;

        if (node_index >= state_parse_storage(state)->node_count) {
            return true;
        }
        node = &state_parse_storage(state)->nodes[node_index];
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
                node->first_word >= state_parse_storage(state)->word_count) {
                return true;
            }
            name = state_parse_storage(state)->words[node->first_word];
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
            node->first_child >= state_parse_storage(state)->node_count) {
            return true;
        }
        if (node->kind == GSH_AST_PIPELINE &&
            node->first_child != GSH_AST_NONE &&
            state_parse_storage(state)->nodes[node->first_child].next_sibling !=
                GSH_AST_NONE) {
            continue;
        }
        child = node->first_child;
        while (child != GSH_AST_NONE &&
               sibling_count++ < GSH_PARSE_NODE_CAP) {
            if (child >= state_parse_storage(state)->node_count ||
                stack_count == GSH_PARSE_NODE_CAP) {
                return true;
            }
            stack[stack_count++] = child;
            child = state_parse_storage(state)->nodes[child].next_sibling;
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
    if (state == NULL) return false;
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

    if (length == sizeof("/async") - 1U &&
        memcmp(command, "/async", sizeof("/async") - 1U) == 0) {
        return true;
    }

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
    if (state == NULL) {
        return -1;
    }
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
        state->current_directory,
        barrier, blocks_independent, status_dependency, control);
    if (cell_index < 0) {
        state->overloads++;
        state->last_status = 125;
        return -1;
    }
    return cell_index;
}

static bool history_submission_is_private(const shell_state *state,
                                          size_t length)
{
    if (state == NULL) {
        return false;
    }
    return state->config.history_ignore_space && length >= 2U &&
           state->pending_line[0] == ' ' &&
           state->pending_line[length - 1U] == ' ';
}

static void record_history_text(shell_state *state, const char *text,
                                size_t length, gsh_parse_status parse_status)
{
    if (state == NULL || text == NULL) return;
    int added;

    if (length == 0 || length >= GSH_HISTORY_ENTRY_CAP ||
        (state->config.history_ignore_space && length >= 2U &&
         text[0] == ' ' && text[length - 1U] == ' ')) return;
    if (state->config.llm_enabled)
        enqueue_journal_record(state, GSH_LLM_JOURNAL_COMMAND, text, length);
    if (state->history == NULL ||
        (!state->config.history_store_failed &&
         parse_status != GSH_PARSE_OK)) {
        return;
    }
    added = gsh_history_add(state->history, text, length,
                            state->config.history_deduplicate);
    if (added > 0 && state->session_history != NULL &&
        gsh_history_add(state->session_history, text,
                        length, false) == -1) {
        state->history_persistent = false;
    }
}

static void record_history_submission(shell_state *state, size_t length,
                                      gsh_parse_status parse_status)
{
    if (state == NULL) return;
    record_history_text(state, state->pending_line, length, parse_status);
}

static void record_classic_submission(shell_state *state, size_t length,
                                      gsh_parse_status parse_status)
{
    if (state == NULL) return;
    state->auto_help_eligible = !history_submission_is_private(state, length);
    record_history_submission(state, length, parse_status);
}

static gsh_parse_result parse_pending_line(shell_state *state,
                                           size_t candidate_length)
{
    gsh_parse_result parsed;

    if (!require(state != NULL && state->parse_storage != NULL)) {
        return (gsh_parse_result){.status = GSH_PARSE_LIMIT,
                                  .root = GSH_AST_NONE};
    }
    if (!require(candidate_length < sizeof(state->pending_line))) {
        return (gsh_parse_result){.status = GSH_PARSE_LIMIT,
                                  .root = GSH_AST_NONE};
    }
    if (gsh_aliases_count(state->aliases) == 0U) {
        reset_pending_input(state);
        return gsh_parse(state->pending_line, candidate_length,
                         state->parse_storage);
    }
    parsed = gsh_alias_parse(
        state->pending_line, candidate_length, state->aliases,
        state->alias_expansion, GSH_ALIAS_EXPANSION_CAP,
        state->parse_storage, &state->pending_input,
        &state->pending_input_length);
    state->pending_alias_expanded =
        state->pending_input_length != candidate_length ||
        memcmp(state->pending_input, state->pending_line,
               candidate_length) != 0;
    return parsed;
}

static void emit_accepted_verbose_line(shell_state *state, size_t offset,
                                       size_t length)
{
    if (state == NULL ||
        !gsh_options_enabled(&state->options, GSH_OPTION_VERBOSE)) return;
    gsh_builtin_io io = reactor_builtin_sink(state);

    (void)emit_verbose_input(&io, state->pending_line + offset, length);
}

static bool accept_managed_line(shell_state *state, size_t length,
                                gsh_parse_status parse_status)
{
    if (state == NULL || state->async_repl == NULL ||
        !state_async_repl(state)->enabled) return false;
    if (length == 0U) {
        queue_prompt(state);
        return true;
    }
    if (accept_managed_submission(state, length) == -1) {
        (void)raw_output_push(state, "\a", 1);
        if (memchr(state->pending_line, '\n', length) == NULL) {
            (void)memcpy(state->line, state->pending_line, length + 1U);
            state->line_len = length;
            state->line_cursor = length;
        }
    } else {
        record_history_submission(state, length, parse_status);
    }
    state->pending_line[0] = '\0';
    reset_pending_input(state);
    queue_prompt(state);
    return true;
}

static size_t llm_prompt_offset(const char *line, size_t length,
                                bool *queued)
{
    size_t offset;

    if (line == NULL || queued == NULL || length == 0U || line[0] != '?')
        return 0U;
    *queued = length > 1U && line[1] == '?';
    offset = *queued ? 2U : 1U;
    while (offset < length && (line[offset] == ' ' || line[offset] == '\t'))
        offset++;
    return offset;
}

static bool accept_llm_line(shell_state *state, size_t length)
{
    bool queued = false;
    size_t prompt_offset;
    char prompt[GSH_ASYNC_PROMPT_CAP];
    int cell;

    if (state == NULL || length == 0U || state->pending_line[0] != '?')
        return false;
    prompt_offset = llm_prompt_offset(state->pending_line, length, &queued);
    state->line_len = 0U;
    state->line_cursor = 0U;
    state->line[0] = '\0';
    state->pending_len = 0U;
    state->continuation_prompt = false;
    reset_history_editor(state);
    (void)output_text(state, "\r\n");
    if (!state->config.llm_enabled) {
        (void)output_text(state,
                          "gsh: LLM is not configured; run gsh-setup\r\n");
        state->last_status = 2;
        state->mode = MODE_EDITOR;
        state->pending_line[0] = '\0';
        reset_pending_input(state);
        queue_prompt(state);
        return true;
    }
    if (prompt_offset >= length) {
        (void)output_text(state, "gsh: '?' requires a prompt\r\n");
        state->last_status = 2;
        state->mode = MODE_EDITOR;
        state->pending_line[0] = '\0';
        reset_pending_input(state);
        queue_prompt(state);
        return true;
    }
    if (state->async_repl == NULL || !state_async_repl(state)->enabled) {
        start_classic_llm(state, state->pending_line + prompt_offset,
                          length - prompt_offset);
        state->pending_line[0] = '\0';
        reset_pending_input(state);
        return true;
    }
    (void)active_prompt_text(state, prompt);
    if (!queued) (void)gsh_async_repl_cancel_active_ai(state->async_repl);
    cell = gsh_async_repl_accept_ai(
        state->async_repl, prompt, state->pending_line, length,
        state->current_directory);
    capture_llm_context(state, cell);
    if (cell < 0) {
        state->overloads++;
        state->last_status = 125;
        (void)raw_output_push(state, "\a", 1U);
    }
    state->pending_line[0] = '\0';
    reset_pending_input(state);
    queue_prompt(state);
    return true;
}

static bool accept_llm_pipeline_line(shell_state *state, size_t length)
{
    char payload[GSH_ASYNC_COMMAND_CAP];
    char prompt[GSH_ASYNC_PROMPT_CAP];
    size_t payload_length;
    int cell;

    if (state == NULL ||
        !build_llm_pipeline_payload(state->pending_line, length, payload,
                                    sizeof(payload), &payload_length))
        return false;
    state->line_len = 0U;
    state->line_cursor = 0U;
    state->line[0] = '\0';
    state->pending_len = 0U;
    state->continuation_prompt = false;
    reset_history_editor(state);
    (void)output_text(state, "\r\n");
    if (!state->config.llm_enabled) {
        (void)output_text(state,
                          "gsh: LLM is not configured; run gsh-setup\r\n");
        state->last_status = 2;
        state->mode = MODE_EDITOR;
        state->pending_line[0] = '\0';
        reset_pending_input(state);
        queue_prompt(state);
        return true;
    }
    if (state->async_repl == NULL || !state_async_repl(state)->enabled) {
        start_classic_llm_request(
            state, payload, payload_length, true,
            state->config.history_ignore_space && length >= 2U &&
                state->pending_line[0] == ' ' &&
                state->pending_line[length - 1U] == ' ');
        state->pending_line[0] = '\0';
        reset_pending_input(state);
        return true;
    }
    (void)active_prompt_text(state, prompt);
    (void)gsh_async_repl_cancel_active_ai(state->async_repl);
    cell = gsh_async_repl_accept_ai_pipeline(
        state->async_repl, prompt, state->pending_line, length,
        state->current_directory);
    if (cell < 0) {
        state->overloads++;
        state->last_status = 125;
        (void)raw_output_push(state, "\a", 1U);
    }
    state->pending_line[0] = '\0';
    reset_pending_input(state);
    queue_prompt(state);
    return true;
}

static bool copy_and_accept_llm_line(shell_state *state,
                                     size_t candidate_length)
{
    if (state == NULL || candidate_length >= sizeof(state->pending_line))
        return false;
    (void)memcpy(state->pending_line + state->pending_len, state->line,
                 state->line_len);
    state->pending_line[candidate_length] = '\0';
    return state->pending_len == 0U &&
           (accept_llm_line(state, candidate_length) ||
            accept_llm_pipeline_line(state, candidate_length));
}

static void accept_line(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    size_t candidate_length = state->pending_len + state->line_len;
    size_t accepted_offset = state->pending_len;
    size_t accepted_length = state->line_len;
    gsh_parse_result parsed;

    finish_classic_editor(state);

    if (candidate_length >= sizeof(state->pending_line)) {
        state->overloads++;
        state->pending_len = 0;
        state->pending_line[0] = '\0';
        reset_pending_input(state);
        state->line_len = 0;
        state->line_cursor = 0U;
        state->line[0] = '\0';
        state->continuation_prompt = false;
        (void)output_text(state,
                          "\r\ngsh: command exceeds input limit\r\n");
        queue_prompt(state);
        return;
    }
    if (copy_and_accept_llm_line(state, candidate_length)) return;
    parsed = parse_pending_line(state, candidate_length);
    if (parsed.status == GSH_PARSE_OK &&
        input_line_continues(state->pending_line, candidate_length)) {
        parsed.status = GSH_PARSE_INCOMPLETE;
    }
    state->pending_parse = parsed;
    state->line_len = 0;
    state->line_cursor = 0U;
    state->line[0] = '\0';
    state->escape_state = 0;
    state->editor_sequence_length = 0U;
    state->bracketed_paste = false;
    state->paste_end_match = 0U;
    state->paste_last_was_cr = false;
    state->paste_overflow_reported = false;
    reset_history_editor(state);
    (void)output_text(state, "\r\n");
    emit_accepted_verbose_line(state, accepted_offset, accepted_length);
    if (parsed.status == GSH_PARSE_INCOMPLETE) {
        if (candidate_length + 1U >= sizeof(state->pending_line)) {
            state->overloads++;
            state->pending_len = 0;
            state->pending_line[0] = '\0';
            reset_pending_input(state);
            state->continuation_prompt = false;
            (void)output_text(state, "gsh: command exceeds input limit\r\n");
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
    if (accept_managed_line(state, candidate_length, parsed.status)) return;
    record_classic_submission(state, candidate_length, parsed.status);
    state->mode = MODE_DISPATCH;
}

static size_t previous_editor_character(const char *line, size_t cursor)
{
    if (line == NULL || cursor == 0U) return 0U;
    cursor--;
    while (cursor > 0U &&
           ((unsigned char)line[cursor] & 0xc0U) == 0x80U) cursor--;
    return cursor;
}

static size_t next_editor_character(const char *line, size_t length,
                                    size_t cursor)
{
    if (line == NULL || cursor >= length) return length;
    cursor++;
    while (cursor < length &&
           ((unsigned char)line[cursor] & 0xc0U) == 0x80U) cursor++;
    return cursor;
}

static void move_editor_cursor(shell_state *state, bool right)
{
    size_t next;

    if (state == NULL) return;
    next = right ? next_editor_character(state->line, state->line_len,
                                         state->line_cursor)
                 : previous_editor_character(state->line,
                                             state->line_cursor);
    if (next == state->line_cursor) {
        (void)output_text(state, "\a");
        return;
    }
    state->line_cursor = next;
    if (state->async_repl != NULL)
        state_async_repl(state)->scroll_offset = 0U;
    queue_redraw(state);
}

static void erase_previous_character(shell_state *state)
{
    size_t begin;
    size_t removed;

    if (state == NULL) return;
    if (state->line_cursor == 0U) {
        (void)output_text(state, "\a");
        return;
    }
    begin = previous_editor_character(state->line, state->line_cursor);
    removed = state->line_cursor - begin;
    (void)memmove(state->line + begin, state->line + state->line_cursor,
                  state->line_len - state->line_cursor + 1U);
    state->line_len -= removed;
    state->line_cursor = begin;
    if (state->async_repl != NULL)
        state_async_repl(state)->scroll_offset = 0U;
    queue_redraw(state);
}

static void clear_focused_escape(shell_state *state)
{
    if (state == NULL) return;
    state->focus_escape_cell = -1;
    state->focus_escape_state = 0U;
    state->focus_escape_deadline_ns = 0U;
    state->mouse_sequence_length = 0U;
}

static int queue_focused_escape(shell_state *state, int focused,
                                unsigned char byte, bool include_byte)
{
    char sequence[4U + sizeof(state->mouse_sequence)];
    size_t length = 0U;
    if (state == NULL) return -1;
    sequence[length++] = '\033';
    if (state->focus_escape_state >= 1U) sequence[length++] = '[';
    if (state->focus_escape_state >= 2U) {
        sequence[length++] = '<';
        (void)memcpy(sequence + length, state->mouse_sequence,
                     state->mouse_sequence_length);
        length += state->mouse_sequence_length;
    }
    if (include_byte) sequence[length++] = (char)byte;
    clear_focused_escape(state);
    return gsh_async_repl_queue_input(state->async_repl, focused,
                                      sequence, length);
}

static bool consume_focused_escape(shell_state *state, int focused,
                                   unsigned char byte)
{
    if (state == NULL || state->focus_escape_cell != focused) return false;
    state->focus_escape_deadline_ns = monotonic_ns() + 30000000ULL;
    if (state->focus_escape_state == 0U && byte == '[') {
        state->focus_escape_state = 1U;
        return true;
    }
    if (state->focus_escape_state == 1U && byte == '<') {
        state->focus_escape_state = 2U;
        state->mouse_sequence_length = 0U;
        return true;
    }
    if (state->focus_escape_state == 2U &&
        ((byte >= '0' && byte <= '9') || byte == ';')) {
        if (state->mouse_sequence_length < sizeof(state->mouse_sequence)) {
            state->mouse_sequence[state->mouse_sequence_length++] = (char)byte;
            return true;
        }
    } else if (state->focus_escape_state == 2U &&
               (byte == 'M' || byte == 'm')) {
        handle_mouse_event(state, byte);
        clear_focused_escape(state);
        return true;
    }
    if (queue_focused_escape(state, focused, byte, true) == -1)
        gsh_async_repl_unfocus(state->async_repl);
    return true;
}

static bool route_focused_input(shell_state *state, unsigned char byte)
{
    if (state == NULL) {
        return false;
    }
    int focused = state->async_repl == NULL
                      ? -1
                      : gsh_async_repl_focused_job(state->async_repl);

    if (focused < 0) {
        return false;
    }
    if (consume_focused_escape(state, focused, byte)) return true;
    if (byte == 0x1bU &&
        state_async_repl(state)->cells[focused].fullscreen) {
        state->focus_escape_cell = focused;
        state->focus_escape_state = 0U;
        state->focus_escape_deadline_ns = monotonic_ns() + 30000000ULL;
    } else if (byte == 0x1dU) {
        leave_managed_fullscreen(state, focused);
        gsh_async_repl_unfocus(state->async_repl);
        queue_redraw(state);
    } else if (byte == 0x03U) {
        (void)signal_managed_job(state, focused, SIGINT);
    } else if (byte == 0x1aU) {
        leave_managed_fullscreen(state, focused);
        stop_managed_job(state, focused);
    } else if (gsh_async_repl_queue_input(
                   state->async_repl, focused, (const char *)&byte,
                   sizeof(byte)) == -1) {
        gsh_async_repl_unfocus(state->async_repl);
        (void)raw_output_push(state, "\a", 1);
    }
    return true;
}

static void complete_pending_escapes(shell_state *state)
{
    uint64_t now;
    if (state == NULL) return;
    if (state->focus_escape_deadline_ns == 0U &&
        state->editor_escape_deadline_ns == 0U) return;
    now = monotonic_ns();
    if (state->focus_escape_deadline_ns != 0U &&
        now >= state->focus_escape_deadline_ns) {
        int focused = gsh_async_repl_focused_job(state->async_repl);
        if (focused == state->focus_escape_cell) {
            if (state->focus_escape_state == 0U) {
                leave_managed_fullscreen(state, focused);
                gsh_async_repl_unfocus(state->async_repl);
                queue_redraw(state);
            } else if (queue_focused_escape(state, focused, 0U, false) == -1) {
                gsh_async_repl_unfocus(state->async_repl);
            }
        }
        clear_focused_escape(state);
    }
    if (state->editor_escape_deadline_ns != 0U &&
        now >= state->editor_escape_deadline_ns) {
        int suspended;
        state->editor_escape_deadline_ns = 0U;
        if (state->escape_state != 1U || state->async_repl == NULL) return;
        state->escape_state = 0U;
        suspended = gsh_async_repl_suspended_fullscreen(state->async_repl);
        if (suspended >= 0 &&
            gsh_async_repl_focus(state->async_repl, suspended) == 0) {
            static const char redraw = '\f';
            (void)gsh_async_repl_queue_input(state->async_repl, suspended,
                                             &redraw, 1U);
        } else if (state->history_search) cancel_history_search(state);
    }
}

static void resource_notice(shell_state *state, const char *message)
{
    static const char command[] = "[resource action]";
    char prompt[GSH_ASYNC_PROMPT_CAP];
    int cell;
    if (state == NULL || message == NULL || state->async_repl == NULL) return;
    (void)active_prompt_text(state, prompt);
    cell = gsh_async_repl_accept(state->async_repl, prompt, command,
                                 sizeof(command) - 1U,
                                 state->current_directory, false, false,
                                 false, true);
    if (cell < 0) { (void)raw_output_push(state, "\a", 1U); return; }
    gsh_async_repl_starting(state->async_repl, cell);
    (void)gsh_async_repl_append(state->async_repl, cell, message,
                                strlen(message));
    (void)gsh_async_repl_append(state->async_repl, cell, "\n", 1U);
    gsh_async_repl_finish(state->async_repl, cell, 0, true);
}

static int absolute_resource_path(const gsh_async_resource_action *action,
                                  const char *relative,
                                  char output[PATH_MAX])
{
    const char *home;
    int length;
    if (action == NULL || relative == NULL || output == NULL) return -1;
    if (relative[0] == '/') length = snprintf(output, PATH_MAX, "%s", relative);
    else if (relative[0] == '~' && relative[1] == '/') {
        home = getenv("HOME");
        if (home == NULL || home[0] != '/') { errno = ENOENT; return -1; }
        length = snprintf(output, PATH_MAX, "%s/%s", home, relative + 2U);
    } else length = snprintf(output, PATH_MAX, "%s/%s",
                             action->launch_directory, relative);
    if (length < 0 || length >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    return 0;
}

static int shell_quote_resource(const char *path, char *output,
                                size_t capacity)
{
    size_t source;
    size_t used = 0U;
    if (path == NULL || output == NULL || capacity < 3U) return -1;
    output[used++] = '\'';
    for (source = 0U; path[source] != '\0'; source++) {
        static const char quote[] = "'\\''";
        if (path[source] == '\'') {
            if (sizeof(quote) - 1U > capacity - used) return -1;
            (void)memcpy(output + used, quote, sizeof(quote) - 1U);
            used += sizeof(quote) - 1U;
        } else {
            if (used + 1U >= capacity) return -1;
            output[used++] = path[source];
        }
    }
    if (used + 2U > capacity) return -1;
    output[used++] = '\'';
    output[used] = '\0';
    return 0;
}

static int build_resource_command(gsh_resource_type type,
                                  const char *path, const char *located,
                                  char command[LINE_CAP])
{
    char quoted_path[LINE_CAP];
    char quoted_location[LINE_CAP];
    int length;
    if (path == NULL || located == NULL || command == NULL ||
        shell_quote_resource(path, quoted_path, sizeof(quoted_path)) == -1 ||
        shell_quote_resource(located, quoted_location,
                             sizeof(quoted_location)) == -1) return -1;
    if (type == GSH_RESOURCE_REGULAR) {
        length = snprintf(command, LINE_CAP, "view -- %s", quoted_location);
    } else if (type == GSH_RESOURCE_DIRECTORY) {
        length = snprintf(command, LINE_CAP, "cd -- %s && ll", quoted_path);
    } else {
        length = snprintf(command, LINE_CAP,
                          "if test -d %s; then cd -- %s && ll; "
                          "else view -- %s; fi",
                          quoted_path, quoted_path, quoted_location);
    }
    if (length < 0 || (size_t)length >= LINE_CAP) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

static int submit_resource_command(shell_state *state, const char *command,
                                   bool directory)
{
    char prompt[GSH_ASYNC_PROMPT_CAP];
    size_t length;
    int cell;
    if (state == NULL || command == NULL || state->async_repl == NULL) return -1;
    length = strlen(command);
    (void)active_prompt_text(state, prompt);
    cell = gsh_async_repl_accept(state->async_repl, prompt, command, length,
                                 state->current_directory, directory,
                                 directory, false, false);
    if (cell < 0) return -1;
    state_async_repl(state)->scroll_offset = 0U;
    state_async_repl(state)->render_pending = true;
    return 0;
}

static void perform_resource_action(shell_state *state,
                                    const gsh_async_resource_action *action)
{
    char path[PATH_MAX];
    char located[PATH_MAX + 64U];
    char command[LINE_CAP];
    int length;
    if (state == NULL || action == NULL) return;
    errno = 0;
    if (absolute_resource_path(action, action->path, path) == -1) {
        char message[PATH_MAX + 128U];
        (void)snprintf(message, sizeof(message), "resource unavailable: %s: %s",
                       action->path, strerror(errno));
        resource_notice(state, message);
        return;
    }
    if (action->type != GSH_RESOURCE_DIRECTORY && action->line != 0U) {
        length = snprintf(located, sizeof(located), "%s:%zu:%zu", path,
                          action->line, action->column == 0U ? 1U : action->column);
        if (length < 0 || (size_t)length >= sizeof(located)) return;
    } else (void)snprintf(located, sizeof(located), "%s", path);
    if (build_resource_command(action->type, path, located, command) == -1 ||
        submit_resource_command(state, command,
                                action->type != GSH_RESOURCE_REGULAR) == -1) {
        resource_notice(state, "cannot queue resource action");
    }
}

static bool parse_mouse_numbers(const char *text, size_t length,
                                size_t values[3])
{
    size_t part = 0U;
    size_t index;
    if (text == NULL || values == NULL) return false;
    (void)memset(values, 0, 3U * sizeof(values[0]));
    for (index = 0U; index < length; index++) {
        if (text[index] == ';') { if (++part >= 3U) return false; continue; }
        if (text[index] < '0' || text[index] > '9' ||
            values[part] > (SIZE_MAX - (size_t)(text[index] - '0')) / 10U) return false;
        values[part] = values[part] * 10U + (size_t)(text[index] - '0');
    }
    return part == 2U;
}

static void handle_mouse_event(shell_state *state, unsigned char final)
{
    size_t values[3];
    size_t separator_column = 0U;
    gsh_async_resource_action action;
    int focused;
    int preview;
    if (state == NULL ||
        !parse_mouse_numbers(state->mouse_sequence,
                             state->mouse_sequence_length, values) ||
        state->async_repl == NULL) return;
    preview = gsh_async_repl_split_preview(state->async_repl,
                                           &separator_column);
    focused = gsh_async_repl_focused_job(state->async_repl);
    if (final == 'M' && (values[0] & 64U) != 0U) {
        if (preview >= 0 && focused == preview &&
            values[1] > separator_column) {
            static const char up = 0x10;
            static const char down = 0x0e;
            const char *motion = (values[0] & 1U) == 0U ? &up : &down;
            (void)gsh_async_repl_queue_input(state->async_repl, preview,
                                             motion, 1U);
        } else {
            gsh_async_repl_scroll(state->async_repl,
                                  (values[0] & 1U) == 0U ? 3L : -3L);
        }
        return;
    }
    if (final != 'M' || (values[0] & 32U) != 0U ||
        (values[0] & 3U) != 0U) return;
    if (preview >= 0 && values[1] > separator_column &&
        focused != preview &&
        gsh_async_repl_focus(state->async_repl, preview) == 0) {
        static const char redraw = '\f';
        (void)gsh_async_repl_queue_input(state->async_repl, preview,
                                         &redraw, 1U);
        return;
    }
    if (gsh_async_repl_resource_at(state->async_repl, values[2], values[1],
                                   &action) == 0) {
        if (preview >= 0 && focused == preview) {
            static const char close = 'q';
            if (gsh_async_repl_queue_input(state->async_repl, preview,
                                           &close, 1U) == -1) {
                (void)raw_output_push(state, "\a", 1U);
                return;
            }
            leave_managed_fullscreen(state, preview);
            gsh_async_repl_unfocus(state->async_repl);
        }
        perform_resource_action(state, &action);
    }
}

static bool process_managed_editor_signal(shell_state *state,
                                          unsigned char byte)
{
    if (state == NULL) return false;
    if (state->async_repl == NULL || !state_async_repl(state)->enabled) {
        return false;
    }
    if (byte == 0x03U) {
        cancel_editor_line(state);
        return true;
    }
    if (byte == 0x1aU) {
        (void)raw_output_push(state, "\a", 1);
        return true;
    }
    return false;
}

static void finish_editor_sequence(shell_state *state, unsigned char byte)
{
    if (state == NULL) return;
    if (byte == '~' && strcmp(state->editor_sequence, "200") == 0) {
        state->bracketed_paste = true;
        state->paste_end_match = 0U;
        state->paste_last_was_cr = false;
        state->paste_overflow_reported = false;
    } else if (byte == '~' && strcmp(state->editor_sequence, "5") == 0 &&
               state->async_repl != NULL) {
        gsh_async_repl_scroll(state->async_repl,
                              (long)state_async_repl(state)->terminal_rows);
    } else if (byte == '~' && strcmp(state->editor_sequence, "6") == 0 &&
               state->async_repl != NULL) {
        gsh_async_repl_scroll(state->async_repl,
                              -(long)state_async_repl(state)->terminal_rows);
    } else if (state->editor_sequence_length == 0U && byte == 'A') {
        if (state->history_search) search_history(state);
        else history_previous(state);
    } else if (state->editor_sequence_length == 0U && byte == 'B') {
        if (state->history_search) accept_history_search(state);
        else history_next(state);
    } else if (state->editor_sequence_length == 0U && byte == 'C') {
        if (state->history_search) accept_history_search(state);
        else move_editor_cursor(state, true);
    } else if (state->editor_sequence_length == 0U && byte == 'D' &&
               !state->history_search) {
        move_editor_cursor(state, false);
    }
    state->editor_sequence_length = 0U;
    state->editor_sequence[0] = '\0';
}

static bool process_escape_input(shell_state *state, unsigned char byte)
{
    if (state == NULL) return false;
    if (state->escape_state == 1) {
        state->editor_escape_deadline_ns = 0U;
        if (byte == '[' || byte == 'O') {
            state->escape_state = 2U;
            state->editor_sequence_length = 0U;
            state->editor_sequence[0] = '\0';
        } else {
            state->escape_state = 0;
            if (state->history_search) {
                cancel_history_search(state);
            }
        }
        return true;
    }
    if (state->escape_state == 2) {
        if (byte == '<' && state->editor_sequence_length == 0U) {
            state->escape_state = 3U;
            state->mouse_sequence_length = 0U;
            return true;
        }
        if (byte >= 0x20U && byte <= 0x3fU) {
            if (state->editor_sequence_length <
                sizeof(state->editor_sequence) - 1U) {
                state->editor_sequence[state->editor_sequence_length++] =
                    (char)byte;
                state->editor_sequence[state->editor_sequence_length] = '\0';
            } else {
                state->escape_state = 0U;
                state->editor_sequence_length = 0U;
            }
            return true;
        }
        if (byte >= 0x40U && byte <= 0x7eU) {
            state->escape_state = 0;
            finish_editor_sequence(state, byte);
        }
        return true;
    }
    if (state->escape_state == 3U) {
        if (byte == 'M' || byte == 'm') {
            handle_mouse_event(state, byte);
            state->escape_state = 0U;
            state->mouse_sequence_length = 0U;
        } else if ((byte >= '0' && byte <= '9') || byte == ';') {
            if (state->mouse_sequence_length < sizeof(state->mouse_sequence))
                state->mouse_sequence[state->mouse_sequence_length++] = (char)byte;
            else state->escape_state = 0U;
        } else state->escape_state = 0U;
        return true;
    }
    return false;
}

static void erase_history_query(shell_state *state)
{
    if (state == NULL) return;
    if (state->history_search_query_length == 0) {
        (void)output_text(state, "\a");
        return;
    }
    state->history_search_query_length--;
    while (state->history_search_query_length != 0 &&
           ((unsigned char)state->history_search_query[
                state->history_search_query_length] &
            0xc0U) == 0x80U) {
        state->history_search_query_length--;
    }
    state->history_search_query[state->history_search_query_length] = '\0';
    update_history_search(state);
}

static bool process_history_search_input(shell_state *state,
                                         unsigned char byte)
{
    if (state == NULL) {
        return false;
    }
    if (byte == 0x12U) {
        search_history(state);
        return true;
    }
    if (!state->history_search) {
        return false;
    }
    if (byte == '\r' || byte == '\n') {
        state->history_search = false;
        state->history_search_query_length = 0;
        state->history_search_query[0] = '\0';
        accept_line(state);
    } else if (byte == 0x7fU || byte == 0x08U) {
        erase_history_query(state);
    } else if (byte == 0x15U) {
        state->history_search_query_length = 0;
        state->history_search_query[0] = '\0';
        update_history_search(state);
    } else if (byte >= 0x20U || byte == '\t') {
        if (state->history_search_query_length < LINE_CAP - 1U) {
            state->history_search_query[
                state->history_search_query_length++] = (char)byte;
            state->history_search_query[
                state->history_search_query_length] = '\0';
            update_history_search(state);
        } else {
            (void)output_text(state, "\a");
        }
    } else {
        return false;
    }
    return true;
}

static void cancel_completion_request(shell_state *state, bool terminate)
{
    if (!require(state != NULL)) return;
    if (!require(state->completion_fd >= -1)) return;
    if (state->completion_fd >= 0) {
        (void)close(state->completion_fd);
        state->completion_fd = -1;
    }
    if (terminate && state->completion_pid > 0)
        (void)kill(state->completion_pid, SIGKILL);
    state->completion_active_request_id = 0U;
    state->completion_deadline_ns = 0U;
    state->completion_line_length = 0U;
    state->completion_cursor = 0U;
    state->completion_query_length = 0U;
    state->completion_query_cursor = 0U;
    state->completion_result_received = 0U;
    state->completion_selection_index = GSH_COMPLETION_SELECT_MENU;
    state->completion_line[0] = '\0';
    state->completion_query_line[0] = '\0';
    state->completion_directory[0] = '\0';
}

static void clear_completion_cycle(shell_state *state)
{
    if (!require(state != NULL)) return;
    state->completion_cycle_active = false;
    state->completion_cycle_line_length = 0U;
    state->completion_cycle_cursor = 0U;
    state->completion_cycle_begin = 0U;
    state->completion_cycle_end = 0U;
    state->completion_cycle_next_index = 0U;
    state->completion_cycle_candidate_count = 0U;
    state->completion_cycle_line[0] = '\0';
    state->completion_menu_length = 0U;
    state->completion_menu[0] = '\0';
}

static bool completion_context_is_current(
    const shell_state *state, const gsh_completion_result *result)
{
    if (!require(state != NULL && result != NULL)) return false;
    if (!require(state->completion_line_length < sizeof(state->line)))
        return false;
    return result->request_id == state->completion_active_request_id &&
           result->end == state->completion_query_cursor &&
           state->line_len == state->completion_line_length &&
           state->line_cursor == state->completion_cursor &&
           memcmp(state->line, state->completion_line,
                  state->line_len + 1U) == 0 &&
           strcmp(state->current_directory,
                  state->completion_directory) == 0 &&
           state->variable_generation ==
               state->completion_variable_generation &&
           state->alias_generation == state->completion_alias_generation &&
           state->function_generation ==
               state->completion_function_generation;
}

static bool replace_completion_text(shell_state *state, size_t begin,
                                    size_t end, const char *text,
                                    size_t text_length)
{
    size_t removed;
    size_t completed_length;

    if (!require(state != NULL && text != NULL)) return false;
    if (!require(begin <= end && end <= state->line_len)) return false;
    removed = end - begin;
    completed_length = state->line_len - removed + text_length;
    if (completed_length >= sizeof(state->line)) return false;
    (void)memmove(state->line + begin + text_length, state->line + end,
                  state->line_len - end + 1U);
    (void)memcpy(state->line + begin, text, text_length);
    state->line_len = completed_length;
    state->line_cursor = begin + text_length;
    if (state->async_repl != NULL)
        state_async_repl(state)->scroll_offset = 0U;
    queue_redraw(state);
    return true;
}

static bool store_completion_menu(shell_state *state,
                                  const gsh_completion_result *result)
{
    if (!require(state != NULL && result != NULL)) return false;
    if (result->menu_length == 0U ||
        result->menu_length >= sizeof(state->completion_menu)) return false;
    state->completion_menu_length = result->menu_length;
    (void)memcpy(state->completion_menu, result->menu,
                 result->menu_length + 1U);
    return true;
}

static bool begin_completion_cycle(shell_state *state,
                                   const gsh_completion_result *result)
{
    if (!require(state != NULL && result != NULL)) return false;
    if (!require(result->begin <= result->end)) return false;
    if (result->candidate_count == 0U || result->menu_length == 0U ||
        state->completion_query_length >= sizeof(state->completion_cycle_line))
        return false;
    state->completion_cycle_active = true;
    state->completion_cycle_line_length = state->completion_query_length;
    state->completion_cycle_cursor = state->completion_query_cursor;
    state->completion_cycle_begin = result->begin;
    state->completion_cycle_end = result->end;
    state->completion_cycle_next_index = 0U;
    state->completion_cycle_candidate_count = result->candidate_count;
    (void)memcpy(state->completion_cycle_line,
                 state->completion_query_line,
                 state->completion_query_length + 1U);
    if (!store_completion_menu(state, result)) {
        clear_completion_cycle(state);
        return false;
    }
    queue_redraw(state);
    return true;
}

static bool apply_cycle_candidate(shell_state *state,
                                  const gsh_completion_result *result)
{
    uint32_t next;

    if (!require(state != NULL && result != NULL)) return false;
    if (!state->completion_cycle_active || result->candidate_count == 0U ||
        state->completion_cycle_begin > state->completion_cycle_end)
        return false;
    if (!replace_completion_text(
            state, state->completion_cycle_begin, state->completion_cycle_end,
            result->text, result->text_length)) return false;
    if (!store_completion_menu(state, result)) return false;
    state->completion_cycle_end = state->completion_cycle_begin +
                                  result->text_length;
    state->completion_cycle_candidate_count = result->candidate_count;
    next = result->selected_index + 1U;
    state->completion_cycle_next_index =
        next < result->candidate_count ? next : 0U;
    return true;
}

static bool apply_completion_result(shell_state *state,
                                    const gsh_completion_result *result)
{
    if (!require(state != NULL && result != NULL)) return false;
    if (!require(result->begin <= result->end)) return false;
    if (result->status == GSH_COMPLETION_MENU)
        return begin_completion_cycle(state, result);
    if (result->status == GSH_COMPLETION_CYCLE)
        return apply_cycle_candidate(state, result);
    if (result->status != GSH_COMPLETION_EDIT ||
        result->end > state->line_len) return false;
    clear_completion_cycle(state);
    return replace_completion_text(state, result->begin, result->end,
                                   result->text, result->text_length);
}

static void receive_completion_result(shell_state *state)
{
    unsigned int attempts;
    bool closed = false;
    bool current;

    if (!require(state != NULL)) return;
    if (!require(state->completion_fd >= -1)) return;
    if (state->completion_fd < 0) return;
    for (attempts = 0U; attempts < 4U &&
         state->completion_result_received < sizeof(state->completion_result);
         attempts++) {
        ssize_t received = recv(
            state->completion_fd,
            (char *)&state->completion_result +
                state->completion_result_received,
            sizeof(state->completion_result) -
                state->completion_result_received,
            0);

        if (received > 0) state->completion_result_received += (size_t)received;
        else if (received == -1 && errno == EINTR) continue;
        else if (received == -1 &&
                 (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { closed = true; break; }
    }
    if (state->completion_result_received < sizeof(state->completion_result) &&
        !closed && state->completion_pid > 0) return;
    if (state->completion_result_received == sizeof(state->completion_result) &&
        state->completion_pid > 0) return;
    current = state->completion_result_received ==
                  sizeof(state->completion_result) &&
              gsh_completion_result_valid(&state->completion_result) &&
              completion_context_is_current(state,
                                            &state->completion_result);
    if (!current ||
        !apply_completion_result(state, &state->completion_result)) {
        clear_completion_cycle(state);
        if (current) (void)output_text(state, "\a");
    }
    cancel_completion_request(state, false);
}

static bool send_completion_result(int descriptor,
                                   const gsh_completion_result *result)
{
    size_t offset = 0U;
    size_t attempts;

    if (!require(descriptor >= 0)) return false;
    if (!require(result != NULL)) return false;
    for (attempts = 0U; attempts <= sizeof(*result) &&
         offset < sizeof(*result); attempts++) {
        ssize_t sent = send(descriptor, (const char *)result + offset,
                            sizeof(*result) - offset, 0);

        if (sent > 0) offset += (size_t)sent;
        else if (!(sent == -1 && errno == EINTR)) break;
    }
    return offset == sizeof(*result);
}

static void close_worker_child_descriptors(shell_state *state,
                                               int retained)
{
    if (!require(state != NULL)) _exit(125);
    if (!require(retained >= 0)) _exit(125);
    close_child_reactor_descriptors(state, retained);
    if (state->variable_commit_fd >= 0) (void)close(state->variable_commit_fd);
    if (state->job_service_socket >= 0) (void)close(state->job_service_socket);
    if (state->job_service_wait_reply_fd >= 0)
        (void)close(state->job_service_wait_reply_fd);
    if (state->exec_outcome_fd >= 0) (void)close(state->exec_outcome_fd);
    if (state->exec_descriptor_socket >= 0)
        (void)close(state->exec_descriptor_socket);
    if (state->directory_commit_socket >= 0)
        (void)close(state->directory_commit_socket);
    if (state->directory_commit_fd >= 0)
        (void)close(state->directory_commit_fd);
    (void)close(STDIN_FILENO);
    (void)close(STDOUT_FILENO);
    (void)close(STDERR_FILENO);
}

_Noreturn static void run_completion_child(
    shell_state *state, int descriptor, const sigset_t *previous,
    uint64_t request_id)
{
    gsh_completion_result result;
    const char *path;
    const char *home;
    bool home_found = false;
    bool delivered;

    if (!require(state != NULL && previous != NULL)) _exit(125);
    if (!require(descriptor >= 0 && request_id != 0U)) _exit(125);
    (void)setpgid(0, 0);
    reset_child_signals();
    (void)sigprocmask(SIG_SETMASK, previous, NULL);
    close_worker_child_descriptors(state, descriptor);
    path = store_path_value(state->variables, state->default_path);
    home = gsh_variables_lookup(state->variables, "HOME", 4U, &home_found);
    if (gsh_completion_generate(
            state->completion_query_line, state->completion_query_length,
            state->completion_query_cursor, path,
            home_found ? home : NULL, state->aliases, state->functions,
            state->variables,
            state_async_repl(state)->terminal_columns,
            state->completion_selection_index, request_id, &result) == -1)
        _exit(1);
    delivered = send_completion_result(descriptor, &result);
    (void)close(descriptor);
    _exit(delivered ? 0 : 1);
}

/* ── Tab Completion Runs Outside the Editor Reactor ────────────
 * Directory and PATH discovery can block on a slow filesystem, so Tab only
 * snapshots its bounded editor context and forks one isolated query worker.
 * The reactor keeps accepting keys while polling the stream result channel.
 * Any edit cancels the query, and generation plus directory checks reject a
 * result computed against state that changed before it was delivered.
 * A 50 ms deadline bounds optional worker ownership without delaying typing.
 * ─────────────────────────────────────────────────────────────── */
static void prepare_completion_query(shell_state *state)
{
    if (!require(state != NULL)) return;
    state->completion_line_length = state->line_len;
    state->completion_cursor = state->line_cursor;
    (void)memcpy(state->completion_line, state->line, state->line_len + 1U);
    if (state->completion_cycle_active &&
        state->completion_cycle_line_length <
            sizeof(state->completion_query_line) &&
        state->completion_cycle_cursor <=
            state->completion_cycle_line_length) {
        state->completion_query_length = state->completion_cycle_line_length;
        state->completion_query_cursor = state->completion_cycle_cursor;
        state->completion_selection_index =
            state->completion_cycle_next_index;
        (void)memcpy(state->completion_query_line,
                     state->completion_cycle_line,
                     state->completion_cycle_line_length + 1U);
    } else {
        clear_completion_cycle(state);
        state->completion_query_length = state->line_len;
        state->completion_query_cursor = state->line_cursor;
        state->completion_selection_index = GSH_COMPLETION_SELECT_MENU;
        (void)memcpy(state->completion_query_line, state->line,
                     state->line_len + 1U);
    }
}

static void start_completion_request(shell_state *state)
{
    int sockets[2] = {-1, -1};
    sigset_t blocked;
    sigset_t previous;
    pid_t pid;
    uint64_t request_id;

    if (!require(state != NULL)) return;
    if (!require(state->completion_fd >= -1 && state->completion_pid >= -1))
        return;
    if (!state->config.completion_enabled || state->completion_fd >= 0 ||
        state->completion_pid > 0) { (void)output_text(state, "\a"); return; }
    prepare_completion_query(state);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == -1 ||
        set_fd_flags(sockets[0], F_GETFL, O_NONBLOCK) == -1 ||
        set_fd_flags(sockets[0], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(sockets[1], F_GETFD, FD_CLOEXEC) == -1) {
        if (sockets[0] >= 0) (void)close(sockets[0]);
        if (sockets[1] >= 0) (void)close(sockets[1]);
        (void)output_text(state, "\a");
        return;
    }
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        (void)close(sockets[0]); (void)close(sockets[1]);
        (void)output_text(state, "\a"); return;
    }
    request_id = state->completion_next_request_id++;
    if (request_id == 0U) request_id = state->completion_next_request_id++;
    pid = fork();
    if (pid == 0) {
        (void)close(sockets[0]);
        run_completion_child(state, sockets[1], &previous, request_id);
    }
    (void)close(sockets[1]);
    if (pid == -1) {
        (void)close(sockets[0]);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        (void)output_text(state, "\a"); return;
    }
    (void)setpgid(pid, pid);
    state->completion_fd = sockets[0];
    state->completion_pid = pid;
    state->completion_active_request_id = request_id;
    state->completion_result_received = 0U;
    state->completion_deadline_ns = monotonic_ns() + 50000000ULL;
    (void)memcpy(state->completion_directory, state->current_directory,
                 strlen(state->current_directory) + 1U);
    state->completion_variable_generation = state->variable_generation;
    state->completion_alias_generation = state->alias_generation;
    state->completion_function_generation = state->function_generation;
    (void)sigprocmask(SIG_SETMASK, &previous, NULL);
}

static void expire_completion_request(shell_state *state)
{
    if (!require(state != NULL)) return;
    if (!require(state->completion_deadline_ns <= UINT64_MAX)) return;
    if (state->completion_deadline_ns != 0U &&
        monotonic_ns() >= state->completion_deadline_ns) {
        cancel_completion_request(state, true);
        clear_completion_cycle(state);
        (void)output_text(state, "\a");
    }
}

static bool process_editor_control(shell_state *state, unsigned char byte)
{
    if (state == NULL) {
        return false;
    }
    if (byte == 0x1bU) {
        state->escape_state = 1;
        state->editor_escape_deadline_ns = monotonic_ns() + 30000000ULL;
        return true;
    }
    if (byte == '\r' || byte == '\n') {
        accept_line(state);
        return true;
    }
    if (byte == '\t') {
        start_completion_request(state);
        return true;
    }
    if (byte == 0x04U) {
        if (state->line_len == 0) {
            if (state->continuation_prompt) {
                cancel_editor_line(state);
                return true;
            }
            if (state->current_job.active ||
                gsh_async_repl_job_count(state->async_repl) != 0) {
                (void)output_text(state, "\r\ngsh: a job is still active\r\n");
                queue_prompt(state);
            } else if (gsh_options_enabled(
                           &state->options, GSH_OPTION_IGNOREEOF)) {
                (void)output_text(state, "\r\n");
                queue_prompt(state);
            } else {
                state->running = false;
            }
        }
        return true;
    }
    if (byte == 0x7fU || byte == 0x08U) {
        erase_previous_character(state);
        return true;
    }
    if (byte == 0x15U) {
        state->line_len = 0;
        state->line_cursor = 0U;
        state->line[0] = '\0';
        if (state->async_repl != NULL)
            state_async_repl(state)->scroll_offset = 0U;
        queue_redraw(state);
        return true;
    }
    if (byte == 0x0cU) {
        queue_clear_redraw(state);
        return true;
    }
    return false;
}

static void insert_editor_byte(shell_state *state, unsigned char byte)
{
    bool direct;

    if (state == NULL) return;
    if ((byte >= 0x20U || byte == '\t' ||
         (state->bracketed_paste && byte == '\n')) &&
        state->line_len < LINE_CAP - 1U) {
        direct = !state->bracketed_paste &&
                 state->line_cursor == state->line_len;
        if (!direct)
            (void)memmove(state->line + state->line_cursor + 1U,
                          state->line + state->line_cursor,
                          state->line_len - state->line_cursor + 1U);
        state->line[state->line_cursor++] = (char)byte;
        state->line_len++;
        state->line[state->line_len] = '\0';
        if (state->async_repl != NULL && state_async_repl(state)->enabled) {
            state_async_repl(state)->scroll_offset = 0U;
            state_async_repl(state)->render_pending = true;
        } else if (direct) {
            (void)output_push(state, (const char *)&byte, 1);
            if (byte < 0x80U) {
                size_t columns = state_async_repl(state)->terminal_columns;
                size_t width = byte == '\t'
                                   ? 8U - state->classic_cursor_column % 8U
                                   : 1U;

                if (state->classic_cursor_column >= columns ||
                    state->classic_cursor_column + width > columns) {
                    state->classic_cursor_row++;
                    state->classic_cursor_column = 0U;
                }
                state->classic_cursor_column += width;
            } else {
                classic_editor_position position =
                    classic_editor_position_at(state, state->line_cursor);

                state->classic_cursor_row = position.row;
                state->classic_cursor_column = position.column;
            }
        } else {
            queue_redraw(state);
        }
    } else if (state->line_len >= LINE_CAP - 1) {
        if (!state->bracketed_paste || !state->paste_overflow_reported) {
            state->overloads++;
            state->paste_overflow_reported = state->bracketed_paste;
            (void)output_text(state, "\a");
        }
    }
}

/* ── A Paste Is One Bounded Editor Transaction Boundary ─────────
 * Bracketed-paste markers keep embedded newlines in the editor instead of
 * dispatching partially received commands. The six-byte terminator is matched
 * incrementally across reactor turns; a mismatch returns through the normal
 * editor control-byte filter. CRLF is normalized once and overflow is reported
 * once while the remaining record is still drained deterministically.
 * ─────────────────────────────────────────────────────────────── */
static bool process_bracketed_paste_input(shell_state *state,
                                          unsigned char byte)
{
    static const unsigned char end[] = {'\033', '[', '2', '0', '1', '~'};
    unsigned int prefix;

    if (state == NULL || !state->bracketed_paste) return false;
    if (byte == end[state->paste_end_match]) {
        state->paste_end_match++;
        if (state->paste_end_match == sizeof(end)) {
            state->bracketed_paste = false;
            state->paste_end_match = 0U;
            state->paste_last_was_cr = false;
            queue_redraw(state);
        }
        return true;
    }
    for (prefix = 0U; prefix < state->paste_end_match; prefix++)
        insert_editor_byte(state, end[prefix]);
    state->paste_end_match = 0U;
    if (byte == end[0]) {
        state->paste_end_match = 1U;
        return true;
    }
    if (byte == '\n' && state->paste_last_was_cr) {
        state->paste_last_was_cr = false;
        return true;
    }
    if (byte == '\r') {
        byte = '\n';
        state->paste_last_was_cr = true;
    } else {
        state->paste_last_was_cr = false;
    }
    insert_editor_byte(state, byte);
    return true;
}

static void process_input(shell_state *state)
{
    unsigned int handled;

    if (state == NULL || state->tty_fd < 0) {
        return;
    }
    /* ── Input Batches Collapse Redundant Full-Screen Frames ─────
     * The reactor originally consumed one byte and redrew the whole managed
     * viewport before reading the next byte. Linux PTYs often expose a paste
     * one byte at a time, multiplying one command into hundreds of frames.
     * A fixed batch drains already-ready bytes, while a line boundary returns
     * ownership so dispatch and focus changes occur before later input.
     * The cap preserves a statically bounded reactor turn and editor latency.
     * ─────────────────────────────────────────────────────────────── */
    for (handled = 0; handled < MAX_INPUT_BYTES_PER_TURN; handled++) {
        unsigned char byte;
        ssize_t count = read(state->tty_fd, &byte, sizeof(byte));

        if (count == 0) {
            state->running = false;
            break;
        }
        if (count == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                state->running = false;
            }
            break;
        }
        if (byte != '\t' ||
            gsh_async_repl_focused_job(state->async_repl) >= 0) {
            if (state->completion_fd >= 0)
                cancel_completion_request(state, true);
            clear_completion_cycle(state);
        }
        if (!(route_focused_input(state, byte) ||
              process_managed_editor_signal(state, byte) ||
              process_bracketed_paste_input(state, byte) ||
              process_escape_input(state, byte) ||
              process_history_search_input(state, byte) ||
              process_editor_control(state, byte))) {
            insert_editor_byte(state, byte);
        }
        if (!state->running ||
            ((byte == '\r' || byte == '\n') &&
             !state->bracketed_paste)) {
            break;
        }
    }
}

static int load_managed_submission(shell_state *state, int cell_index)
{
    if (state == NULL) {
        return -1;
    }
    const gsh_async_cell *cell = &state_async_repl(state)->cells[cell_index];

    if (!cell->occupied || cell->command_length >= sizeof(state->pending_line)) {
        errno = EINVAL;
        return -1;
    }
    (void)memcpy(state->pending_line, cell->command, cell->command_length + 1U);
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
    if (state == NULL) {
        return false;
    }
    return state->async_state_cell >= 0 || state->current_job.active ||
           state->mode == MODE_ASYNC_REDIRECTION || state->mode == MODE_WAIT ||
           state->pending_list_active || state->pending_and_or_active;
}

static void finish_managed_state_cell(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    int cell_index = state->async_state_cell;

    if (cell_index < 0 || state->mode != MODE_EDITOR ||
        state->current_job.active ||
        state->mode == MODE_WAIT || state->pending_list_active ||
        state->pending_and_or_active) {
        return;
    }
    if (state_async_repl(state)->cells[cell_index].state == GSH_ASYNC_STARTING) {
        gsh_async_repl_finish(state->async_repl, cell_index,
                              state->last_status << 8, true);
    }
    state->async_state_cell = -1;
    state->async_capture_cell = -1;
    state->async_dispatch_cell = -1;
}

static void record_dispatch_duration(shell_state *state, uint64_t start)
{
    if (state == NULL) {
        return;
    }
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
    if (cell == NULL) {
        return false;
    }
    return cell->state == GSH_ASYNC_DONE ||
           cell->state == GSH_ASYNC_FAILED ||
           cell->state == GSH_ASYNC_CANCELLED ||
           cell->state == GSH_ASYNC_REJECTED;
}

static void finalize_managed_dispatch(shell_state *state, int cell_index)
{
    if (state == NULL) {
        return;
    }
    gsh_async_cell *cell = &state_async_repl(state)->cells[cell_index];

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
        enqueue_managed_error_help(state, cell_index, state->last_status);
        state->async_capture_cell = -1;
        state->async_dispatch_cell = -1;
        return;
    }
    state->async_state_cell = cell_index;
}

static void dispatch_managed_cell(shell_state *state, int cell_index)
{
    if (state == NULL) {
        return;
    }
    int prior_state_cell = state->async_state_cell;
    int previous_status;
    uint64_t start;

    gsh_async_repl_starting(state->async_repl, cell_index);
    state->async_dispatch_cell = cell_index;
    state->async_capture_cell = cell_index;
    if (cell_index == state->llm_command_cell &&
        !llm_command_context_current(state)) {
        (void)output_text(state,
            "Not executed: session context changed. Request the command again.\n");
        gsh_async_repl_finish(state->async_repl, cell_index, 125 << 8, false);
        finalize_managed_dispatch(state, cell_index);
        return;
    }
    if (state_async_repl(state)->cells[cell_index].ai) {
        start_async_llm(state, cell_index);
        finalize_managed_dispatch(state, cell_index);
        return;
    }
    if (state_async_repl(state)->cells[cell_index].status_dependency &&
        gsh_async_repl_previous_status(state->async_repl, cell_index,
                                       &previous_status) == 0) {
        state->last_status = previous_status;
    }
    if (load_managed_submission(state, cell_index) == -1) {
        (void)output_text(state, "gsh: invalid managed submission\r\n");
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
    if (state == NULL) return;
    unsigned int dispatched;

    if (state->async_repl == NULL || !state_async_repl(state)->enabled) {
        return;
    }
    (void)gsh_async_repl_autofocus(state->async_repl);
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
        service_llm_repl(state);
    }
}

/* ── Provider Work Cannot Become A Shell Execution Shortcut ─────
 * The worker submits exactly the approved script to a new ordinary cell.
 * Its barrier orders execution against other commands, while conversational
 * AI cells are excluded from shell dependencies to avoid waiting on their
 * own tool calls. A receipt requires terminal state and closed output, so
 * directory and variable commits finish before the model can claim success.
 * ─────────────────────────────────────────────────────────────── */
static bool llm_command_context_current(const shell_state *state)
{
    const gsh_async_cell *owner;

    if (state == NULL || state->llm_owner_cell < 0 ||
        state->llm_owner_cell >= GSH_ASYNC_CELL_CAP ||
        state->pending_len != 0U) return false;
    owner = &state_async_repl(state)->cells[state->llm_owner_cell];
    if (owner->state != GSH_ASYNC_RUNNING || owner->pid <= 0 ||
        owner->ai_directory_generation !=
            gsh_variables_value_generation(state->variables, "PWD", 3U))
        return false;
    if (state->llm_command_cell < 0 &&
        state_async_repl(state)->last_command_id > owner->ai_context_id)
        return false;
    return strcmp(owner->launch_directory, state->current_directory) == 0;
}

static void close_llm_repl(shell_state *state)
{
    if (state == NULL) return;
    if (state->llm_owner_cell >= 0 &&
        state->llm_owner_cell < GSH_ASYNC_CELL_CAP) {
        state_async_repl(state)->cells[state->llm_owner_cell].ai_activity =
            GSH_LLM_IDLE;
        state_async_repl(state)->render_pending = true;
    }
    if (state->llm_command_cell >= 0 &&
        state->llm_command_cell < GSH_ASYNC_CELL_CAP) {
        gsh_async_cell *cell =
            &state_async_repl(state)->cells[state->llm_command_cell];
        cell->ai_result_pending = false;
        if (!managed_cell_terminal(cell)) {
            if (cell->pgid > 0) (void)kill(-cell->pgid, SIGTERM);
            gsh_async_repl_finish(state->async_repl, state->llm_command_cell,
                                  125 << 8, false);
        }
    }
    gsh_llm_repl_close(&state->llm_repl);
    state->llm_owner_cell = -1;
    state->llm_command_cell = -1;
}

static void llm_repl_reject(shell_state *state, const char *reason)
{
    gsh_llm_repl_result *result;

    if (state == NULL || reason == NULL) return;
    result = &state->llm_repl.result;
    (void)memset(result, 0, sizeof(*result));
    result->version = GSH_LLM_REPL_VERSION;
    result->status = 125;
    (void)snprintf(result->directory, sizeof(result->directory), "%s",
                   state->current_directory);
    (void)snprintf(result->output, sizeof(result->output),
                   "Not executed: %s", reason);
    state->llm_repl.replying = true;
}

static void accept_llm_command(shell_state *state)
{
    char prompt[GSH_ASYNC_PROMPT_CAP];
    const gsh_llm_repl_request *request;
    gsh_async_cell *cell;
    int index;

    if (state == NULL || state->llm_owner_cell < 0) return;
    state_async_repl(state)->cells[state->llm_owner_cell].ai_activity =
        GSH_LLM_IDLE;
    state_async_repl(state)->render_pending = true;
    if (state_async_repl(state)->cells[state->llm_owner_cell].ai_pipeline) {
        llm_repl_reject(state, "execution tools are disabled for pipeline requests.");
        return;
    }
    if (!llm_command_context_current(state)) {
        llm_repl_reject(state,
            "session context changed. Ask the user to request the command again.");
        return;
    }
    request = &state->llm_repl.request;
    (void)active_prompt_text(state, prompt);
    {
        size_t used = strlen(prompt);
        if (used + sizeof("[AI] ") > sizeof(prompt)) {
            llm_repl_reject(state, "prompt exceeds the REPL limit.");
            return;
        }
        (void)memcpy(prompt + used, "[AI] ", sizeof("[AI] "));
    }
    index = gsh_async_repl_accept(state->async_repl, prompt, request->script,
        request->length, state->current_directory, true, true, true, false);
    if (index < 0) {
        llm_repl_reject(state, "REPL queue is full.");
        return;
    }
    state->llm_command_cell = index;
    cell = &state_async_repl(state)->cells[index];
    cell->ai_result_pending = true;
    cell->ai_request_id =
        state_async_repl(state)->cells[state->llm_owner_cell].ai_request_id;
    if (cell->ai_request_id == 0U)
        cell->ai_request_id = state_async_repl(state)->cells[state->llm_owner_cell].id;
    cell->ai_private =
        state_async_repl(state)->cells[state->llm_owner_cell].ai_private;
    if (!cell->ai_private)
        record_history_text(state, request->script, request->length,
                             GSH_PARSE_OK);
    queue_prompt(state);
}

static bool continue_llm_cell(shell_state *state)
{
    gsh_async_cell *previous;
    gsh_async_cell *next;
    int index;

    if (state == NULL || state->llm_owner_cell < 0) return false;
    previous = &state_async_repl(state)->cells[state->llm_owner_cell];
    index = gsh_async_repl_accept_ai(state->async_repl, "gsh ai> ", "", 0U,
                                     state->current_directory);
    if (index < 0) return false;
    next = &state_async_repl(state)->cells[index];
    next->ai_activity = GSH_LLM_GENERATING;
    next->ai_private = previous->ai_private;
    next->ai_request_id = previous->ai_request_id != 0U
        ? previous->ai_request_id : previous->id;
    gsh_async_repl_starting(state->async_repl, index);
    if (gsh_async_repl_attach(state->async_repl, index, previous->pid,
                              previous->pgid, previous->pty_fd, -1) == -1)
        return false;
    next->ai_context_id = state->llm_command_cell < 0
        ? previous->ai_context_id
        : state_async_repl(state)->cells[state->llm_command_cell].id;
    next->ai_directory_generation =
        gsh_variables_value_generation(state->variables, "PWD", 3U);
    previous->pid = 0;
    previous->pgid = 0;
    previous->pty_fd = -1;
    previous->focused = false;
    previous->input_requested = false;
    gsh_async_repl_finish(state->async_repl, state->llm_owner_cell, 0, true);
    state->llm_owner_cell = index;
    return true;
}

static void finish_llm_command(shell_state *state)
{
    const gsh_async_cell *cell;
    gsh_llm_repl_result *result;

    if (state == NULL || state->llm_command_cell < 0 ||
        state->llm_repl.replying) return;
    cell = &state_async_repl(state)->cells[state->llm_command_cell];
    if (!managed_cell_terminal(cell) || !cell->output_closed ||
        cell->pty_fd >= 0 || state->async_state_cell == state->llm_command_cell)
        return;
    result = &state->llm_repl.result;
    (void)memset(result, 0, sizeof(*result));
    result->version = GSH_LLM_REPL_VERSION;
    result->status = wait_status_value(cell->wait_status);
    result->cell_id = cell->id;
    (void)snprintf(result->directory, sizeof(result->directory), "%s",
                   state->current_directory);
    (void)snprintf(result->output, sizeof(result->output), "%.*s%s",
        60000, cell->output,
        cell->output_truncated || cell->output_length > 60000U
            ? "\n[output truncated]" : "");
    if (!continue_llm_cell(state)) {
        close_llm_repl(state);
        return;
    }
    state->llm_repl.replying = true;
}

static void service_llm_repl(shell_state *state)
{
    int received;

    if (state == NULL || state->llm_repl.fd < 0) return;
    if (state->llm_owner_cell < 0 ||
        managed_cell_terminal(
            &state_async_repl(state)->cells[state->llm_owner_cell])) {
        close_llm_repl(state);
        return;
    }
    if (state->llm_repl.replying) {
        int sent = gsh_llm_repl_flush(&state->llm_repl);
        if (sent < 0) close_llm_repl(state);
        else if (sent > 0 && state->llm_command_cell >= 0) {
            state_async_repl(state)->cells[state->llm_command_cell]
                .ai_result_pending = false;
            state->llm_command_cell = -1;
        }
        return;
    }
    received = gsh_llm_repl_receive(&state->llm_repl);
    if (received < 0) { close_llm_repl(state); return; }
    if (received > 0 && state->llm_repl.request.length == 0U) {
        state_async_repl(state)->cells[state->llm_owner_cell].ai_activity =
            (gsh_llm_activity)state->llm_repl.request.activity;
        state_async_repl(state)->render_pending = true;
        state->llm_repl.received = 0U;
        return;
    }
    if (received > 0) accept_llm_command(state);
    finish_llm_command(state);
}

static size_t receive_job_service_rights(
    struct msghdr *message, int rights[GSH_JOB_SERVICE_RIGHTS])
{
    if (message == NULL || rights == NULL) {
        return 0U;
    }
    struct cmsghdr *header = CMSG_FIRSTHDR(message);

    if (header == NULL || header->cmsg_level != SOL_SOCKET ||
        header->cmsg_type != SCM_RIGHTS ||
        header->cmsg_len != CMSG_LEN(sizeof(int) *
                                    GSH_JOB_SERVICE_RIGHTS) ||
        CMSG_NXTHDR(message, header) != NULL) {
        return 0;
    }
    (void)memcpy(rights, CMSG_DATA(header),
           sizeof(int) * GSH_JOB_SERVICE_RIGHTS);
    return GSH_JOB_SERVICE_RIGHTS;
}

static bool validate_job_service_request(
    job_service_request *request,
    char *argv[GSH_NATIVE_ARGUMENT_CAP + 1U])
{
    if (request == NULL) return false;
    if (argv == NULL) {
        return false;
    }
    size_t argument;
    size_t expected = 0;

    if (request->version != GSH_JOB_SERVICE_VERSION ||
        (request->type != GSH_JOB_SERVICE_JOBS &&
         request->type != GSH_JOB_SERVICE_KILL &&
         request->type != GSH_JOB_SERVICE_WAIT) ||
        request->argc == 0 || request->argc > GSH_NATIVE_ARGUMENT_CAP ||
        request->text_length == 0 ||
        request->text_length > GSH_NATIVE_TEXT_CAP) {
        return false;
    }
    for (argument = 0; argument < request->argc; argument++) {
        size_t length;

        if (request->offsets[argument] != expected ||
            expected >= request->text_length) return false;
        argv[argument] = request->text + expected;
        length = strnlen(argv[argument], request->text_length - expected);
        if (length == request->text_length - expected) return false;
        expected += length + 1U;
    }
    argv[request->argc] = NULL;
    return expected == request->text_length &&
           ((request->type == GSH_JOB_SERVICE_JOBS &&
             strcmp(argv[0], "jobs") == 0) ||
            (request->type == GSH_JOB_SERVICE_KILL &&
             strcmp(argv[0], "kill") == 0) ||
            (request->type == GSH_JOB_SERVICE_WAIT &&
             strcmp(argv[0], "wait") == 0));
}

static bool finish_job_service_wait(shell_state *state)
{
    if (state == NULL) return false;
    job_service_reply reply = {GSH_JOB_SERVICE_VERSION, 127, 0, 0};
    size_t target;

    if (state->job_service_wait_reply_fd < 0) return false;
    if (state->job_service_wait_all) {
        if (!gsh_background_consume_all_if_done(
                &state->background_jobs)) {
            return false;
        }
        reply.status = 0;
    } else {
        for (target = 0; target < state->job_service_wait_target_count;
             target++) {
            bool done;

            if (state->job_service_wait_targets[target] > 0 &&
                gsh_background_get(
                    &state->background_jobs,
                    state->job_service_wait_targets[target], &done,
                    NULL) &&
                !done) {
                return false;
            }
        }
        reply.status = 127;
        for (target = 0; target < state->job_service_wait_target_count;
             target++) {
            int wait_status;

            if (state->job_service_wait_targets[target] > 0 &&
                gsh_background_consume(
                    &state->background_jobs,
                    state->job_service_wait_targets[target],
                    &wait_status) &&
                target + 1U == state->job_service_wait_target_count) {
                reply.status = wait_status_value(wait_status);
            }
        }
    }
    (void)send(state->job_service_wait_reply_fd, &reply, sizeof(reply), 0);
    (void)close(state->job_service_wait_reply_fd);
    state->job_service_wait_reply_fd = -1;
    state->job_service_wait_target_count = 0;
    state->job_service_wait_all = false;
    return true;
}

static int begin_job_service_wait(shell_state *state, uint32_t argc,
                                  char *const argv[], int reply_fd)
{
    if (state == NULL) return -1;
    if (argv == NULL) {
        return -1;
    }
    size_t argument;

    if (state->job_service_wait_reply_fd >= 0 || argc == 0 ||
        argc - 1U > GSH_BACKGROUND_CAP || reply_fd < 0) {
        errno = state->job_service_wait_reply_fd >= 0 ? EBUSY : EINVAL;
        return -1;
    }
    state->job_service_wait_all = argc == 1U;
    state->job_service_wait_target_count = 0;
    if (state->job_service_wait_all) {
        state->job_service_wait_target_count = gsh_background_snapshot(
            &state->background_jobs, state->job_service_wait_targets);
    } else {
        for (argument = 1; argument < argc; argument++) {
            const char *text = argv[argument];
            pid_t selected = -1;

            if (text[0] == '%') {
                uint32_t job_id;

                if (gsh_background_resolve(
                        &state->background_jobs, text,
                        &job_id) == GSH_JOBSPEC_OK) {
                    selected = gsh_background_job_pid(
                        &state->background_jobs, job_id);
                }
            } else {
                char *end;
                unsigned long number;

                errno = 0;
                number = strtoul(text, &end, 10);
                if (errno == 0 && *text != '\0' && *end == '\0' &&
                    number > 0 && number <= (unsigned long)INT_MAX) {
                    selected = (pid_t)number;
                }
            }
            state->job_service_wait_targets[
                state->job_service_wait_target_count++] = selected;
        }
    }
    state->job_service_wait_reply_fd = reply_fd;
    (void)finish_job_service_wait(state);
    return 0;
}

static void service_job_requests(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    unsigned int serviced;

    for (serviced = 0; serviced < GSH_JOB_SERVICE_BATCH; serviced++) {
        job_service_request request;
        job_service_reply reply = {GSH_JOB_SERVICE_VERSION, 125, 0, 0};
        char *argv[GSH_NATIVE_ARGUMENT_CAP + 1U];
        int rights[GSH_JOB_SERVICE_RIGHTS] = {-1, -1, -1};
        unsigned char control[
            CMSG_SPACE(sizeof(int) * GSH_JOB_SERVICE_RIGHTS)];
        struct iovec payload = {&request, sizeof(request)};
        struct msghdr message;
        size_t rights_count;
        ssize_t received;
        bool reply_deferred = false;

        (void)memset(&request, 0, sizeof(request));
        (void)memset(control, 0, sizeof(control));
        (void)memset(&message, 0, sizeof(message));
        message.msg_iov = &payload;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        received = recvmsg(state->job_service_socket, &message,
                           MSG_DONTWAIT);
        if (received == -1 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (received == -1 && errno == EINTR) {
            continue;
        }
        if (received == -1) return;
        rights_count = receive_job_service_rights(&message, rights);
        if (received >= (ssize_t)offsetof(job_service_request, text) &&
            request.text_length <= GSH_NATIVE_TEXT_CAP &&
            received == (ssize_t)(offsetof(job_service_request, text) +
                                  request.text_length) &&
            (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0 &&
            rights_count == GSH_JOB_SERVICE_RIGHTS &&
            validate_job_service_request(&request, argv) &&
            ftruncate(rights[0], 0) == 0 &&
            ftruncate(rights[1], 0) == 0 &&
            lseek(rights[0], 0, SEEK_SET) != (off_t)-1 &&
            lseek(rights[1], 0, SEEK_SET) != (off_t)-1) {
            const gsh_builtin_io io = {
                .kind = GSH_BUILTIN_SINK_DESCRIPTORS,
                .descriptors = {rights[0], rights[1]},
            };

            if (request.type == GSH_JOB_SERVICE_JOBS) {
                reply.status = gsh_builtin_jobs(
                    request.argc, argv, &state->background_jobs, &io);
            } else if (request.type == GSH_JOB_SERVICE_KILL) {
                reply.status = gsh_builtin_kill(
                    request.argc, argv, &state->background_jobs, &io);
            } else if (begin_job_service_wait(
                           state, request.argc, argv, rights[2]) == 0) {
                reply_deferred = true;
                rights[2] = -1;
            }
            reply.error = errno;
        } else {
            reply.error = EPROTO;
        }
        if (rights_count == GSH_JOB_SERVICE_RIGHTS && rights[2] >= 0 &&
            !reply_deferred) {
            (void)send(rights[2], &reply, sizeof(reply), 0);
        }
        while (rights_count > 0) {
            int descriptor = rights[--rights_count];

            if (descriptor >= 0) (void)close(descriptor);
        }
    }
}

enum { GSH_REACTOR_BASE_FDS = 6 };

static size_t add_managed_poll_descriptors(
    shell_state *state,
    struct pollfd descriptors[
        GSH_REACTOR_BASE_FDS + 2U * GSH_ASYNC_CELL_CAP + 2U])
{
    if (state == NULL) return 0U;
    if (descriptors == NULL) {
        return 0U;
    }
    size_t count = GSH_REACTOR_BASE_FDS;
    int index;

    if (state->async_repl == NULL || !state_async_repl(state)->enabled) {
        return count;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        int descriptor = state_async_repl(state)->cells[index].resource_fd;

        if (descriptor >= 0) {
            descriptors[count] = (struct pollfd){descriptor, POLLIN, 0};
            count++;
        }
        descriptor = state_async_repl(state)->cells[index].pty_fd;
        if (descriptor >= 0) {
            descriptors[count].fd = descriptor;
            descriptors[count].events = POLLIN;
            if (gsh_async_repl_input_pending(state->async_repl, index)) {
                descriptors[count].events |= POLLOUT;
            }
            descriptors[count].revents = 0;
            count++;
        }
    }
    return count;
}

static void note_managed_private_input(shell_state *state, int cell_index)
{
    if (state == NULL) {
        return;
    }
    gsh_async_cell *cell;
    struct termios modes;
    const char *slave_name;
    int probe = -1;
    bool echo_disabled = false;
    bool noncanonical = false;

    if (cell_index < 0) {
        return;
    }
    cell = &state_async_repl(state)->cells[cell_index];
    if (cell->pty_fd < 0 ||
        (cell->fullscreen &&
         (cell->input_requested || cell->autofocus_suppressed))) {
        return;
    }
    if (tcgetattr(cell->pty_fd, &modes) == 0) {
        echo_disabled = (modes.c_lflag & ECHO) == 0;
        noncanonical = (modes.c_lflag & ICANON) == 0;
    } else {
        slave_name = ptsname(cell->pty_fd);
        if (slave_name != NULL) {
            probe = open(slave_name,
                         O_RDONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        }
        if (probe >= 0) {
            if (tcgetattr(probe, &modes) == 0) {
                echo_disabled = (modes.c_lflag & ECHO) == 0;
                noncanonical = (modes.c_lflag & ICANON) == 0;
            }
            (void)close(probe);
        }
    }
    if (echo_disabled || noncanonical) {
        (void)gsh_async_repl_request_input(state->async_repl, cell_index,
                                           noncanonical);
        cell->input_probe_pending = false;
    }
}

static bool output_requests_fullscreen(const char *bytes, size_t length)
{
    static const char *const requests[] = {
        "\033[?47h", "\033[?1047h", "\033[?1049h"};
    size_t request;
    if (bytes == NULL) return false;
    for (request = 0U;
         request < sizeof(requests) / sizeof(requests[0]); request++) {
        size_t wanted = strlen(requests[request]);
        size_t offset;
        for (offset = 0U; offset + wanted <= length; offset++) {
            if (memcmp(bytes + offset, requests[request], wanted) == 0)
                return true;
        }
    }
    return false;
}

static bool push_managed_preview_base(shell_state *state)
{
    char prompt[GSH_ASYNC_PROMPT_CAP];
    const char *render;
    size_t length;
    if (state == NULL || state->async_repl == NULL) return false;
    (void)active_prompt_text(state, prompt);
    if (gsh_async_repl_prepare_render_with_completion(
            state->async_repl, prompt, state->line, state->line_len,
            state->line_cursor, state->completion_menu,
            state->completion_menu_length) == -1)
        return false;
    render = gsh_async_repl_render_data(state->async_repl);
    length = gsh_async_repl_render_length(state->async_repl);
    if (!raw_output_push(state, render, length)) return false;
    gsh_async_repl_rendered(state->async_repl);
    return true;
}

static void leave_managed_fullscreen(shell_state *state, int cell_index)
{
    if (state == NULL) return;
    static const char restore[] =
        "\033[0m\033[?25h\033[?1000l\033[?1002l\033[?1003l"
        "\033[?1004l\033[?1006l\033[?1015l\033[?2004l\033[?2026l"
        "\033[>4;0m\033[<u\033>\033[H\033[2J\033[3J";
    static const char split_restore[] = "\033[0m\033[?25h";
    gsh_async_cell *cell;

    if (state->async_repl == NULL || cell_index < 0 ||
        cell_index >= GSH_ASYNC_CELL_CAP) {
        return;
    }
    cell = &state_async_repl(state)->cells[cell_index];
    if (!cell->fullscreen_presented) {
        return;
    }
    if (cell->preview_split)
        (void)raw_output_push(state, split_restore,
                              sizeof(split_restore) - 1U);
    else (void)raw_output_push(state, restore, sizeof(restore) - 1U);
    cell->fullscreen_presented = false;
    cell->passthrough_state = 0;
    cell->passthrough_utf8_length = 0;
    cell->passthrough_utf8_expected = 0;
    cell->passthrough_sequence_length = 0;
    state_async_repl(state)->render_pending = true;
}

static void present_managed_fullscreen(shell_state *state, int cell_index,
                                       const char *bytes, size_t length)
{
    if (state == NULL) {
        return;
    }
    static const char begin[] = "\033[0m\033[H\033[2J";
    static const char split_begin[] =
        "\033[0m\033[?25l\033[?1000h\033[?1006h";
    char filtered[4096 + GSH_ASYNC_PASSTHROUGH_SEQUENCE_CAP];
    gsh_async_cell *cell = &state_async_repl(state)->cells[cell_index];
    size_t filtered_length = 0;

    if (!cell->focused || !cell->fullscreen) return;
    if (!cell->fullscreen_presented) {
        if (cell->preview_split && !push_managed_preview_base(state)) return;
        if (!raw_output_push(state,
                             cell->preview_split ? split_begin : begin,
                             cell->preview_split ? sizeof(split_begin) - 1U
                                                 : sizeof(begin) - 1U)) {
            return;
        }
        if (cell->preview_split) state_async_repl(state)->mouse_enabled = true;
        cell->fullscreen_presented = true;
    }
    if (gsh_async_repl_filter_fullscreen(
            state->async_repl, cell_index, bytes, length, filtered,
            sizeof(filtered), &filtered_length) == 0 &&
        filtered_length != 0) {
        (void)raw_output_push(state, filtered, filtered_length);
    }
}

static void preflight_managed_input_focus(
    shell_state *state,
    struct pollfd descriptors[
        GSH_REACTOR_BASE_FDS + 2U * GSH_ASYNC_CELL_CAP + 2U],
    size_t count)
{
    if (state == NULL) return;
    if (descriptors == NULL) {
        return;
    }
    size_t index;

    if (state->async_repl == NULL || !state_async_repl(state)->enabled) {
        return;
    }
    for (index = GSH_REACTOR_BASE_FDS; index < count; index++) {
        int cell_index = gsh_async_repl_cell_for_fd(
            state->async_repl, descriptors[index].fd);

        if (cell_index >= 0 &&
            ((descriptors[index].revents & POLLIN) != 0 ||
             state_async_repl(state)->cells[cell_index].input_probe_pending)) {
            note_managed_private_input(state, cell_index);
            if (!state_async_repl(state)->cells[cell_index].input_requested) {
                state_async_repl(state)->cells[cell_index].input_probe_pending =
                    false;
            }
        }
    }
    (void)gsh_async_repl_autofocus(state->async_repl);
}

static void read_managed_output(shell_state *state, struct pollfd *descriptor)
{
    if (descriptor == NULL || state == NULL) {
        return;
    }
    char bytes[4096];
    gsh_async_cell *cell;
    int cell_index = gsh_async_repl_cell_for_fd(
        state->async_repl, descriptor->fd);
    unsigned int reads;

    if (cell_index < 0) {
        return;
    }
    for (reads = 0; reads < 4U; reads++) {
        ssize_t count = read(descriptor->fd, bytes, sizeof(bytes));

        if (count > 0) {
            cell = &state_async_repl(state)->cells[cell_index];
            if (!cell->fullscreen &&
                output_requests_fullscreen(bytes, (size_t)count)) {
                (void)gsh_async_repl_request_input(
                    state->async_repl, cell_index, true);
            }
            note_managed_private_input(state, cell_index);
            (void)gsh_async_repl_autofocus(state->async_repl);
            cell = &state_async_repl(state)->cells[cell_index];
            if (cell->fullscreen) {
                present_managed_fullscreen(state, cell_index, bytes,
                                           (size_t)count);
            } else {
                (void)gsh_async_repl_append(state->async_repl, cell_index,
                                            bytes, (size_t)count);
            }
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        leave_managed_fullscreen(state, cell_index);
        gsh_async_repl_close_output(state->async_repl, cell_index);
        break;
    }
    cell = &state_async_repl(state)->cells[cell_index];
    if (cell->pty_fd >= 0 && !cell->fullscreen) {
        /* A private terminal read disables echo before presenting its prompt.
         * Probe once per bounded PTY service turn: no text matching, no thread,
         * and no periodic wakeup or unbounded scan on the editor path. */
        cell->input_probe_pending = true;
        note_managed_private_input(state, cell_index);
    }
}

static bool drain_managed_output_before_frame(shell_state *state,
                                               int cell_index)
{
    struct pollfd descriptor;
    gsh_async_cell *cell;

    if (state == NULL || state->async_repl == NULL || cell_index < 0 ||
        cell_index >= GSH_ASYNC_CELL_CAP) return false;
    cell = &state_async_repl(state)->cells[cell_index];
    for (size_t turn = 0U; turn < 16U && cell->pty_fd >= 0; turn++) {
        int ready;

        descriptor = (struct pollfd){cell->pty_fd, POLLIN, 0};
        ready = poll(&descriptor, 1U, 0);
        if (ready == 0) return true;
        if (ready == -1 && errno == EINTR) { turn--; continue; }
        if (ready < 0) return false;
        read_managed_output(state, &descriptor);
        if (state->output_len > OUTPUT_CAP / 2U) flush_output(state);
        if (state->output_len > OUTPUT_CAP / 2U) return false;
    }
    if (cell->pty_fd < 0) return false;
    descriptor = (struct pollfd){cell->pty_fd, POLLIN, 0};
    return poll(&descriptor, 1U, 0) == 0;
}

static bool accept_view_datagram(shell_state *state, int cell_index,
                                 const char *message, size_t length)
{
    static const char clear_editor_screen[] = "\033[H\033[2J\033[3J";
    gsh_resource_view_record view;
    gsh_async_cell *cell;
    bool returning_from_editor;
    if (state == NULL || message == NULL ||
        length != sizeof(view) || cell_index < 0) return false;
    (void)memcpy(&view, message, sizeof(view));
    if (view.version == GSH_RESOURCE_PROTOCOL_VERSION &&
        view.size == sizeof(view) &&
        ((view.event == GSH_RESOURCE_PROTOCOL_VIEW_FULL &&
          view.separator_column == 0U) ||
         (view.event == GSH_RESOURCE_PROTOCOL_VIEW_SPLIT &&
          view.separator_column > 1U))) {
        cell = &state_async_repl(state)->cells[cell_index];
        returning_from_editor = cell->native_preview &&
                                !cell->preview_split &&
                                view.event == GSH_RESOURCE_PROTOCOL_VIEW_SPLIT;
        if (returning_from_editor)
            (void)raw_output_push(state, clear_editor_screen,
                                  sizeof(clear_editor_screen) - 1U);
        (void)gsh_async_repl_set_preview_layout(
            state->async_repl, cell_index, (size_t)view.separator_column);
    }
    return true;
}

static size_t preview_base64(const unsigned char *input, size_t length,
                             char *output, size_t capacity)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t used = 0U;

    if (input == NULL || output == NULL ||
        length > GSH_PREVIEW_FRAME_CHUNK_CAP) return 0U;
    for (size_t offset = 0U; offset < length;
         offset += 3U) {
        uint32_t value = (uint32_t)input[offset] << 16U;
        size_t remaining = length - offset;

        if (remaining > 1U) value |= (uint32_t)input[offset + 1U] << 8U;
        if (remaining > 2U) value |= input[offset + 2U];
        if (capacity - used < 4U) return 0U;
        output[used++] = alphabet[(value >> 18U) & 63U];
        output[used++] = alphabet[(value >> 12U) & 63U];
        output[used++] = remaining > 1U ? alphabet[(value >> 6U) & 63U] : '=';
        output[used++] = remaining > 2U ? alphabet[value & 63U] : '=';
    }
    return used;
}

static bool preview_frame_format_supported(
    gsh_terminal_image_protocol protocol, uint32_t format)
{
    if (protocol == GSH_TERMINAL_IMAGE_ITERM)
        return format == GSH_RESOURCE_PROTOCOL_FRAME_PNG ||
               format == GSH_RESOURCE_PROTOCOL_FRAME_JPEG ||
               format == GSH_RESOURCE_PROTOCOL_FRAME_GIF;
    if (protocol == GSH_TERMINAL_IMAGE_KITTY)
        return format == GSH_RESOURCE_PROTOCOL_FRAME_PNG;
    if (protocol == GSH_TERMINAL_IMAGE_SIXEL)
        return format == GSH_RESOURCE_PROTOCOL_FRAME_SIXEL;
    return false;
}

static void reset_preview_frame(gsh_async_cell *cell)
{
    if (cell == NULL) return;
    cell->preview_frame_active = false;
    cell->preview_frame_placed = false;
    cell->preview_frame_format = 0U;
    cell->preview_frame_total = 0U;
    cell->preview_frame_received = 0U;
    cell->preview_frame_row = 0U;
    cell->preview_frame_column = 0U;
    cell->preview_frame_rows = 0U;
    cell->preview_frame_columns = 0U;
}

static void erase_iterm_preview_rectangle(shell_state *state,
                                           const gsh_async_cell *cell)
{
    static const char save[] = "\033" "7";
    static const char restore[] = "\033" "8";
    char sequence[96];

    if (state == NULL || cell == NULL) return;
    if (!raw_output_push(state, save, sizeof(save) - 1U)) return;
    for (uint32_t row = 0U; row < cell->preview_frame_rows; row++) {
        int length = snprintf(sequence, sizeof(sequence),
            "\033[%u;%uH\033[%uX", cell->preview_frame_row + row,
            cell->preview_frame_column, cell->preview_frame_columns);

        if (length < 0 || (size_t)length >= sizeof(sequence) ||
            !raw_output_push(state, sequence, (size_t)length)) break;
    }
    (void)raw_output_push(state, restore, sizeof(restore) - 1U);
}

static void delete_preview_frame(shell_state *state, gsh_async_cell *cell)
{
    static const char iterm_end[] = "\033]1337;FileEnd\a\033" "8";
    static const char sixel_end[] = "\033\\\033" "8";
    char sequence[96];
    int length;

    if (state == NULL || cell == NULL) return;
    if ((cell->preview_frame_active || cell->preview_frame_placed) &&
        state->image_protocol == GSH_TERMINAL_IMAGE_KITTY) {
        length = snprintf(sequence, sizeof(sequence),
                          "\033_Ga=d,d=i,i=%u,q=2\033\\",
                          cell->preview_frame_id);
        if (length > 0 && (size_t)length < sizeof(sequence))
            (void)raw_output_push(state, sequence, (size_t)length);
    } else if ((cell->preview_frame_active || cell->preview_frame_placed) &&
               state->image_protocol == GSH_TERMINAL_IMAGE_ITERM) {
        if (cell->preview_frame_active)
            (void)raw_output_push(state, iterm_end, sizeof(iterm_end) - 1U);
        erase_iterm_preview_rectangle(state, cell);
    } else if (cell->preview_frame_active &&
               state->image_protocol == GSH_TERMINAL_IMAGE_SIXEL) {
        (void)raw_output_push(state, sixel_end, sizeof(sixel_end) - 1U);
    }
    reset_preview_frame(cell);
}

static bool preview_frame_rectangle_valid(const shell_state *state,
                                           const gsh_preview_frame_record *frame)
{
    size_t rows;
    size_t columns;

    if (state == NULL || frame == NULL || state->async_repl == NULL)
        return false;
    rows = state_async_repl(state)->terminal_rows;
    columns = state_async_repl(state)->terminal_columns;
    return frame->cell_row > 0U && frame->cell_column > 0U &&
           frame->cell_rows > 0U && frame->cell_columns > 0U &&
           frame->cell_row <= rows && frame->cell_column <= columns &&
           frame->cell_rows <= rows - frame->cell_row + 1U &&
           frame->cell_columns <= columns - frame->cell_column + 1U;
}

static bool begin_preview_frame(shell_state *state, int cell_index,
                                const gsh_preview_frame_record *frame)
{
    char sequence[256];
    gsh_async_cell *cell;
    int length = 0;

    if (state == NULL || frame == NULL || cell_index < 0 ||
        cell_index >= GSH_ASYNC_CELL_CAP || !preview_frame_rectangle_valid(
            state, frame)) return false;
    cell = &state_async_repl(state)->cells[cell_index];
    if (frame->generation <= cell->preview_frame_generation) return false;
    delete_preview_frame(state, cell);
    if (!drain_managed_output_before_frame(state, cell_index)) return false;
    cell->preview_frame_active = true;
    cell->preview_frame_generation = frame->generation;
    cell->preview_frame_id = frame->frame_id;
    cell->preview_frame_format = frame->format;
    cell->preview_frame_total = frame->total_length;
    cell->preview_frame_row = frame->cell_row;
    cell->preview_frame_column = frame->cell_column;
    cell->preview_frame_rows = frame->cell_rows;
    cell->preview_frame_columns = frame->cell_columns;
    if (!preview_frame_format_supported(state->image_protocol,
                                        frame->format)) return true;
    if (state->image_protocol == GSH_TERMINAL_IMAGE_ITERM) {
        const char *name = frame->format == GSH_RESOURCE_PROTOCOL_FRAME_JPEG
                               ? "Z3NoLXByZXZpZXcuanBn"
                           : frame->format == GSH_RESOURCE_PROTOCOL_FRAME_GIF
                               ? "Z3NoLXByZXZpZXcuZ2lm"
                               : "Z3NoLXByZXZpZXcucG5n";
        length = snprintf(sequence, sizeof(sequence),
            "\033" "7\033[%u;%uH\033]1337;MultipartFile=name=%s;size=%u;inline=1;"
            "width=%u;height=%u;preserveAspectRatio=1\a",
            frame->cell_row, frame->cell_column, name, frame->total_length,
            frame->cell_columns, frame->cell_rows);
    } else if (state->image_protocol == GSH_TERMINAL_IMAGE_SIXEL) {
        length = snprintf(sequence, sizeof(sequence),
                          "\033" "7\033[%u;%uH\033Pq",
                          frame->cell_row, frame->cell_column);
    }
    if (length != 0 && ((size_t)length >= sizeof(sequence) ||
        !raw_output_push(state, sequence, (size_t)length))) {
        reset_preview_frame(cell);
        return false;
    }
    return true;
}

static bool preview_frame_matches(const gsh_async_cell *cell,
                                  const gsh_preview_frame_record *frame)
{
    if (cell == NULL || frame == NULL) return false;
    return cell->preview_frame_generation == frame->generation &&
           cell->preview_frame_id == frame->frame_id &&
           cell->preview_frame_format == frame->format &&
           cell->preview_frame_row == frame->cell_row &&
           cell->preview_frame_column == frame->cell_column &&
           cell->preview_frame_rows == frame->cell_rows &&
           cell->preview_frame_columns == frame->cell_columns;
}

static bool preview_sixel_payload_valid(const unsigned char *bytes,
                                        size_t length)
{
    if (bytes == NULL || length > GSH_PREVIEW_FRAME_CHUNK_CAP) return false;
    for (size_t index = 0U; index < length; index++) {
        if (bytes[index] < 0x20U || bytes[index] > 0x7eU ||
            bytes[index] == 0x1bU) return false;
    }
    return true;
}

static bool emit_preview_chunk(shell_state *state, gsh_async_cell *cell,
                               const unsigned char *bytes, size_t length,
                               bool final)
{
    char encoded[(GSH_PREVIEW_FRAME_CHUNK_CAP + 2U) / 3U * 4U];
    char sequence[sizeof(encoded) + 256U];
    size_t encoded_length;
    int prefix;
    size_t used;

    if (state == NULL || cell == NULL || bytes == NULL) return false;
    if (!preview_frame_format_supported(state->image_protocol,
                                        cell->preview_frame_format))
        return true;
    if (state->image_protocol == GSH_TERMINAL_IMAGE_SIXEL)
        return preview_sixel_payload_valid(bytes, length) &&
               raw_output_push(state, (const char *)bytes, length);
    encoded_length = preview_base64(bytes, length, encoded, sizeof(encoded));
    if (encoded_length == 0U) return false;
    if (state->image_protocol == GSH_TERMINAL_IMAGE_ITERM) {
        prefix = snprintf(sequence, sizeof(sequence),
                          "\033]1337;FilePart=");
        if (prefix < 0) return false;
        used = (size_t)prefix;
        if (sizeof(sequence) - used < encoded_length + 1U) return false;
        (void)memcpy(sequence + used, encoded, encoded_length);
        used += encoded_length;
        sequence[used++] = '\a';
    } else {
        prefix = snprintf(sequence, sizeof(sequence),
            "\033[%u;%uH\033_Ga=T,f=100,t=d,i=%u,q=2,m=%u,c=%u,r=%u;",
            cell->preview_frame_row, cell->preview_frame_column,
            cell->preview_frame_id, final ? 0U : 1U,
            cell->preview_frame_columns, cell->preview_frame_rows);
        if (prefix < 0) return false;
        used = (size_t)prefix;
        if (sizeof(sequence) - used < encoded_length + 2U) return false;
        (void)memcpy(sequence + used, encoded, encoded_length);
        used += encoded_length;
        sequence[used++] = '\033';
        sequence[used++] = '\\';
    }
    return raw_output_push(state, sequence, used);
}

static bool accept_preview_chunk(shell_state *state, int cell_index,
                                 const gsh_preview_frame_record *frame,
                                 const unsigned char *bytes)
{
    gsh_async_cell *cell;
    bool final;

    if (state == NULL || frame == NULL || bytes == NULL || cell_index < 0 ||
        cell_index >= GSH_ASYNC_CELL_CAP) return false;
    cell = &state_async_repl(state)->cells[cell_index];
    if (!cell->preview_frame_active ||
        !preview_frame_matches(cell, frame) ||
        cell->preview_frame_received != frame->offset ||
        cell->preview_frame_received > cell->preview_frame_total ||
        frame->chunk_length == 0U ||
        frame->chunk_length > GSH_PREVIEW_FRAME_CHUNK_CAP ||
        frame->chunk_length > cell->preview_frame_total -
            cell->preview_frame_received) return false;
    final = frame->chunk_length == cell->preview_frame_total -
                                  cell->preview_frame_received;
    if (!emit_preview_chunk(state, cell, bytes, frame->chunk_length, final)) {
        delete_preview_frame(state, cell);
        return false;
    }
    cell->preview_frame_received += frame->chunk_length;
    return true;
}

static bool end_preview_frame(shell_state *state, int cell_index,
                              const gsh_preview_frame_record *frame)
{
    static const char iterm_end[] = "\033]1337;FileEnd\a\033" "8";
    static const char sixel_end[] = "\033\\\033" "8";
    gsh_async_cell *cell;
    bool emitted = true;

    if (state == NULL || frame == NULL || cell_index < 0 ||
        cell_index >= GSH_ASYNC_CELL_CAP) return false;
    cell = &state_async_repl(state)->cells[cell_index];
    if (!cell->preview_frame_active ||
        !preview_frame_matches(cell, frame) ||
        cell->preview_frame_received != cell->preview_frame_total)
        return false;
    if (preview_frame_format_supported(state->image_protocol,
                                       cell->preview_frame_format)) {
        if (state->image_protocol == GSH_TERMINAL_IMAGE_ITERM)
            emitted = raw_output_push(state, iterm_end,
                                      sizeof(iterm_end) - 1U);
        else if (state->image_protocol == GSH_TERMINAL_IMAGE_SIXEL)
            emitted = raw_output_push(state, sixel_end,
                                      sizeof(sixel_end) - 1U);
    }
    if (!emitted) {
        delete_preview_frame(state, cell);
        return false;
    }
    cell->preview_frame_active = false;
    cell->preview_frame_placed = emitted &&
        preview_frame_format_supported(state->image_protocol,
                                       cell->preview_frame_format);
    return true;
}

/* ── Typed Frames Keep Escape Ownership In The Compositor ────────
 * A preview worker can decode an untrusted document, but it never earns the
 * right to write a terminal graphics protocol.  It sends bounded raw chunks
 * on the already authenticated first-party datagram channel.  The reactor
 * validates generation, rectangle, format, offsets, and total size before it
 * wraps those bytes in complete protocol records.  Saturation drops one frame
 * while the canonical text placeholder and the shell continue unchanged.
 * ─────────────────────────────────────────────────────────────── */
static bool accept_preview_datagram(shell_state *state, int cell_index,
                                    const char *message, size_t length)
{
    gsh_preview_frame_record frame;
    gsh_async_cell *cell;

    if (state == NULL || message == NULL || cell_index < 0 ||
        length < sizeof(frame)) return false;
    (void)memcpy(&frame, message, sizeof(frame));
    if (frame.version != GSH_RESOURCE_PROTOCOL_VERSION ||
        frame.size != length || frame.reserved != 0U ||
        frame.generation == 0U || frame.frame_id == 0U ||
        frame.format < GSH_RESOURCE_PROTOCOL_FRAME_PNG ||
        frame.format > GSH_RESOURCE_PROTOCOL_FRAME_SIXEL ||
        frame.pixel_width == 0U || frame.pixel_height == 0U ||
        frame.pixel_width > 16384U || frame.pixel_height > 16384U)
        return false;
    cell = &state_async_repl(state)->cells[cell_index];
    if (frame.event == GSH_RESOURCE_PROTOCOL_FRAME_DELETE &&
        length == sizeof(frame) && frame.total_length == 0U &&
        frame.offset == 0U && frame.chunk_length == 0U) {
        if (preview_frame_matches(cell, &frame))
            delete_preview_frame(state, cell);
        return true;
    }
    if (frame.event == GSH_RESOURCE_PROTOCOL_FRAME_BEGIN &&
        length == sizeof(frame) && frame.total_length > 0U &&
        frame.total_length <= GSH_PREVIEW_FRAME_TOTAL_CAP &&
        frame.chunk_length == 0U && frame.offset == 0U)
        return begin_preview_frame(state, cell_index, &frame);
    if (frame.event == GSH_RESOURCE_PROTOCOL_FRAME_CHUNK &&
        frame.total_length == 0U &&
        frame.chunk_length <= GSH_PREVIEW_FRAME_CHUNK_CAP &&
        sizeof(frame) + frame.chunk_length == length)
        return accept_preview_chunk(state, cell_index, &frame,
            (const unsigned char *)message + sizeof(frame));
    if (frame.event == GSH_RESOURCE_PROTOCOL_FRAME_END &&
        length == sizeof(frame) && frame.total_length == 0U &&
        frame.offset == 0U && frame.chunk_length == 0U)
        return end_preview_frame(state, cell_index, &frame);
    return false;
}

static void accept_resource_datagram(shell_state *state, int cell_index,
                                     const char *message, size_t length)
{
    gsh_resource_record_header header;
    const char *path;
    const char *label;
    if (accept_view_datagram(state, cell_index, message, length) ||
        accept_preview_datagram(state, cell_index, message, length)) return;
    if (state == NULL || message == NULL ||
        length < sizeof(header) || cell_index < 0) return;
    (void)memcpy(&header, message, sizeof(header));
    if (header.version != GSH_RESOURCE_PROTOCOL_VERSION ||
        header.size != length ||
        header.path_length == 0U ||
        header.path_length >= GSH_RESOURCE_PROTOCOL_PATH_CAP ||
        header.label_length == 0U ||
        header.label_length >= GSH_RESOURCE_PROTOCOL_LABEL_CAP ||
        sizeof(header) + (size_t)header.path_length +
                (size_t)header.label_length != length ||
        header.byte_begin >= header.byte_end ||
        header.byte_end - header.byte_begin != header.label_length ||
        header.column_begin >= header.column_end ||
        header.type < GSH_RESOURCE_REGULAR ||
        header.type > GSH_RESOURCE_SYMLINK ||
        (header.flags & ~(GSH_RESOURCE_PROTOCOL_NAVIGABLE |
                          GSH_RESOURCE_PROTOCOL_MUTED)) != 0U) return;
    path = message + sizeof(header);
    label = path + header.path_length;
    if (memchr(path, '\0', header.path_length) != NULL ||
        memchr(label, '\0', header.label_length) != NULL) return;
    (void)gsh_async_repl_add_native_resource(
        state->async_repl, cell_index, header.row, header.byte_begin,
        header.byte_end, header.column_begin, header.column_end,
        label, header.label_length, path,
        header.path_length, (gsh_resource_type)header.type,
        (header.flags & GSH_RESOURCE_PROTOCOL_NAVIGABLE) != 0U,
        (header.flags & GSH_RESOURCE_PROTOCOL_MUTED) != 0U);
}

static void read_managed_resources(shell_state *state,
                                   const struct pollfd *descriptor,
                                   int cell_index)
{
    char message[sizeof(gsh_resource_record_header) +
                 GSH_RESOURCE_PROTOCOL_PATH_CAP +
                 GSH_RESOURCE_PROTOCOL_LABEL_CAP];
    unsigned int reads;
    bool close_channel = false;
    if (state == NULL || descriptor == NULL || cell_index < 0) return;
    for (reads = 0U; reads < 16U; reads++) {
        ssize_t count = recv(descriptor->fd, message, sizeof(message),
                             MSG_DONTWAIT);
        if (count > 0) {
            accept_resource_datagram(state, cell_index, message,
                                     (size_t)count);
            continue;
        }
        if (count == -1 && errno == EINTR) continue;
        if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        close_channel = true;
        break;
    }
    if ((descriptor->revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        close_channel = true;
    if (close_channel) {
        delete_preview_frame(state,
            &state_async_repl(state)->cells[cell_index]);
        gsh_async_repl_close_resource(state->async_repl, cell_index);
    }
}

static void process_managed_descriptors(
    shell_state *state,
    struct pollfd descriptors[
        GSH_REACTOR_BASE_FDS + 2U * GSH_ASYNC_CELL_CAP + 2U],
    size_t count)
{
    if (state == NULL) return;
    if (descriptors == NULL) {
        return;
    }
    size_t index;

    if (state->async_repl == NULL || !state_async_repl(state)->enabled) {
        return;
    }
    for (index = GSH_REACTOR_BASE_FDS; index < count; index++) {
        short events = descriptors[index].revents;
        int resource_cell = gsh_async_repl_cell_for_resource_fd(
            state->async_repl, descriptors[index].fd);

        if (resource_cell >= 0) {
            if ((events & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0)
                read_managed_resources(state, &descriptors[index],
                                       resource_cell);
            continue;
        }

        if ((events & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0) {
            read_managed_output(state, &descriptors[index]);
        }
        if ((events & POLLOUT) != 0) {
            int cell_index = gsh_async_repl_cell_for_fd(
                state->async_repl, descriptors[index].fd);

            if (cell_index >= 0 &&
                gsh_async_repl_flush_input(state->async_repl,
                                           cell_index) == -1) {
                leave_managed_fullscreen(state, cell_index);
                state_async_repl(state)->cells[cell_index].focused = false;
                gsh_async_repl_close_output(state->async_repl, cell_index);
            }
        }
    }
    (void)gsh_async_repl_autofocus(state->async_repl);
}

static bool editor_accepts_input(const shell_state *state)
{
    if (state == NULL) {
        return false;
    }
    return state->async_repl != NULL && state_async_repl(state)->enabled
               ? true
               : state->mode == MODE_EDITOR;
}

static bool async_transition_has_live_shell_state(const shell_state *state)
{
    if (state == NULL) {
        return false;
    }
    return state->current_job.active ||
           gsh_background_active_count(&state->background_jobs) != 0 ||
           state->async_state_cell >= 0 || state->variable_commit_active ||
           state->variable_commit_fd >= 0 ||
           state->exec_outcome_fd >= 0 ||
           state->exec_descriptor_socket >= 0 ||
           state->directory_commit_socket >= 0 ||
           state->directory_commit_fd >= 0 || state->pending_list_active ||
           state->pending_and_or_active || state->wait_target_count != 0 ||
           state->wait_all || state->pending_positional_commit ||
           state->pending_alias_commit || state->pending_function_commit ||
           state->pending_command_cache_commit ||
           state->pending_directory_commit || state->pending_exec_possible ||
           state->positional_commit_expected || state->alias_commit_expected ||
           state->function_commit_expected ||
           state->command_cache_commit_expected ||
           state->directory_commit_expected;
}

static bool async_transition_can_start_now(const shell_state *state)
{
    if (state == NULL) {
        return false;
    }
    bool managed = state->async_repl != NULL && state_async_repl(state)->enabled;
    int control_cell = managed ? state->async_dispatch_cell : -1;

    if (async_transition_has_live_shell_state(state)) {
        return false;
    }
    if (!managed) {
        return state->async_capture_cell < 0 &&
               state->async_dispatch_cell < 0;
    }
    if (control_cell < 0 || control_cell >= GSH_ASYNC_CELL_CAP ||
        state->async_capture_cell != control_cell ||
        !state_async_repl(state)->cells[control_cell].control) {
        return false;
    }
    return gsh_async_repl_all_settled_except(state->async_repl,
                                              control_cell);
}

static bool async_transition_quiescent(const shell_state *state)
{
    if (state == NULL) {
        return false;
    }
    bool managed = state->async_repl != NULL &&
                   state_async_repl(state)->enabled;

    if (state->mode != MODE_EDITOR || state->output_len != 0 ||
        async_transition_has_live_shell_state(state) ||
        state->async_capture_cell >= 0 || state->async_state_cell >= 0 ||
        state->async_dispatch_cell >= 0) {
        return false;
    }
    return !managed ||
           (gsh_async_repl_all_settled(state->async_repl) &&
            !state_async_repl(state)->render_pending);
}

static int seed_async_enabled_notice(shell_state *state)
{
    if (state == NULL) {
        return -1;
    }
    static const char command[] = "/async";
    static const char notice[] = "async repl: on\n";
    char prompt[GSH_ASYNC_PROMPT_CAP];
    int cell_index;

    (void)active_prompt_text(state, prompt);
    cell_index = gsh_async_repl_accept(
        state->async_repl, prompt, command, sizeof(command) - 1U,
        state->current_directory,
        false, false, false, true);
    if (cell_index < 0) {
        return -1;
    }
    gsh_async_repl_starting(state->async_repl, cell_index);
    if (gsh_async_repl_append(state->async_repl, cell_index, notice,
                              sizeof(notice) - 1U) == -1) {
        return -1;
    }
    gsh_async_repl_finish(state->async_repl, cell_index, 0, true);
    return 0;
}

static void apply_async_transition(shell_state *state)
{
    if (state == NULL) return;
    static const char leave_screen[] = "\033[?1049l";

    if (!state->async_transition_pending ||
        !async_transition_quiescent(state)) {
        return;
    }
    if (!state->async_desired) {
        (void)raw_output_push(state, leave_screen,
                              sizeof(leave_screen) - 1U);
        gsh_async_repl_initialize(state->async_repl, false);
        state->async_capture_cell = -1;
        state->async_state_cell = -1;
        state->async_dispatch_cell = -1;
        state->async_transition_pending = false;
        make_editor_modes(state);
        if (enter_editor(state) == -1) {
            state->last_status = 1;
            state->running = false;
            return;
        }
        (void)raw_output_push(state, "async repl: off\r\n", 17);
        queue_redraw(state);
        return;
    }

    gsh_async_repl_initialize(state->async_repl, true);
    gsh_async_repl_configure_actions(
        state->async_repl, terminal_actions_requested(&state->config, true),
        state->config.path_detection);
    initialize_repl_size(state);
    make_editor_modes(state);
    if (enter_editor(state) == -1 || seed_async_enabled_notice(state) == -1) {
        int saved_errno = errno;

        gsh_async_repl_initialize(state->async_repl, false);
        state->async_desired = false;
        state->async_transition_pending = false;
        make_editor_modes(state);
        if (enter_editor(state) == -1) {
            state->running = false;
            return;
        }
        output_format(state, "gsh: cannot enable async repl: %s\r\n",
                      strerror(saved_errno));
        state->last_status = 1;
        queue_redraw(state);
        return;
    }
    state->async_transition_pending = false;
    state_async_repl(state)->render_pending = true;
}

static bool dispatch_classic_pending(shell_state *state)
{
    uint64_t start;
    uint64_t end;
    uint64_t duration;

    if (!require(state != NULL)) return false;
    if (!require(state->async_repl != NULL)) return false;
    if (state_async_repl(state)->enabled || state->mode != MODE_DISPATCH ||
        state->output_len != 0U) {
        return false;
    }
    start = monotonic_ns();
    dispatch_pending(state);
    end = monotonic_ns();
    duration = end >= start ? end - start : 0U;
    state->dispatch_cycles++;
    if (duration > state->dispatch_max_ns) state->dispatch_max_ns = duration;
    if (duration > REACTOR_DEADLINE_NS) state->dispatch_misses++;
    return true;
}

static size_t prepare_reactor_descriptors(
    shell_state *state,
    struct pollfd descriptors[
        GSH_REACTOR_BASE_FDS + 2U * GSH_ASYNC_CELL_CAP + 2U])
{
    if (!require(state != NULL)) return 0U;
    if (!require(descriptors != NULL)) return 0U;
    descriptors[0] = (struct pollfd){state->signal_pipe[0], POLLIN, 0};
    descriptors[1] = (struct pollfd){state->tty_fd, 0, 0};
    if (editor_accepts_input(state)) descriptors[1].events |= POLLIN;
    if (state->output_len > 0U) descriptors[1].events |= POLLOUT;
    descriptors[2] = (struct pollfd){
        state->redirection_worker_alive ? state->redirection_worker_fd : -1,
        state->redirection_worker_alive ? POLLIN : 0, 0};
    descriptors[3] = (struct pollfd){
        state->variable_commit_active ? state->variable_commit_fd : -1,
        state->variable_commit_active ? POLLIN : 0, 0};
    descriptors[4] = (struct pollfd){state->job_service_socket,
                                     state->job_service_socket >= 0 ? POLLIN
                                                                    : 0,
                                     0};
    descriptors[5] = (struct pollfd){state->completion_fd,
                                     state->completion_fd >= 0 ? POLLIN : 0,
                                     0};
    size_t count = add_managed_poll_descriptors(state, descriptors);
    if (state->llm_repl.fd >= 0) {
        descriptors[count++] = (struct pollfd){state->llm_repl.fd,
            state->llm_repl.replying ? POLLOUT :
            state->llm_repl.received < sizeof(state->llm_repl.request)
                ? POLLIN : 0, 0};
    }
    if (state->journal.descriptor >= 0)
        descriptors[count++] = (struct pollfd){state->journal.descriptor,
            state->journal.used > state->journal.sent ? POLLOUT : 0, 0};
    return count;
}

static void service_reactor_descriptors(
    shell_state *state, struct pollfd *descriptors, size_t descriptor_count)
{
    if (!require(state != NULL && descriptors != NULL)) return;
    if (!require(descriptor_count >= GSH_REACTOR_BASE_FDS)) return;
    if (state->variable_commit_active &&
        (descriptors[3].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) !=
            0) {
        receive_variable_commit(state, false);
    }
    if (state->job_service_socket >= 0 &&
        (descriptors[4].revents & POLLIN) != 0) {
        service_job_requests(state);
    }
    if ((descriptors[0].revents & POLLIN) != 0) {
        drain_signal_pipe(state);
        process_pending_signals(state);
    }
    if (editor_accepts_input(state) &&
        (descriptors[1].revents & POLLIN) != 0) {
        /* Ownership preflight closes the private-PTY race before editor I/O. */
        preflight_managed_input_focus(state, descriptors, descriptor_count);
        process_input(state);
    }
    if (editor_accepts_input(state) &&
        (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        state->running = false;
    }
    if (state->completion_fd >= 0 &&
        ((descriptors[5].revents &
          (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0 ||
         state->completion_pid < 0)) receive_completion_result(state);
    process_managed_descriptors(state, descriptors, descriptor_count);
    service_llm_repl(state);
    service_journal_descriptor(state, &descriptors[descriptor_count - 1U]);
    if (state->redirection_worker_alive &&
        (descriptors[2].revents & POLLIN) != 0) {
        receive_redirection_result(state);
    }
    if (state->redirection_worker_alive &&
        (descriptors[2].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        bool command = state->mode == MODE_ASYNC_REDIRECTION;

        state->redirection_worker_failures++;
        disable_redirection_worker(state, true);
        if (command) {
            (void)output_text(state, "gsh: asynchronous redirection failed\r\n");
            state->last_status = 1;
            state->mode = MODE_EDITOR;
            queue_prompt(state);
        }
    }
    schedule_managed_submissions(state);
    apply_async_transition(state);
    prepare_classic_redraw(state);
    prepare_managed_render(state);
    if (state->output_len > 0U) flush_output(state);
}

static void record_reactor_duration(shell_state *state, uint64_t start,
                                    uint64_t end)
{
    uint64_t duration = end >= start ? end - start : 0U;

    if (!require(state != NULL)) return;
    if (!require(state->reactor_cycles < UINT64_MAX)) return;
    state->reactor_cycles++;
    if (duration > state->reactor_max_ns) state->reactor_max_ns = duration;
    if (duration > REACTOR_DEADLINE_NS) state->reactor_misses++;
}

static int run_reactor(shell_state *state)
{
    if (!require(state != NULL)) return 1;
    if (!require(state->signal_pipe[0] >= 0 && state->tty_fd >= 0)) return 1;
    queue_prompt(state);

    while (state->running) {
        struct pollfd descriptors[
            GSH_REACTOR_BASE_FDS + 2U * GSH_ASYNC_CELL_CAP + 2U];
        size_t descriptor_count;
        int result;
        uint64_t service_start;

        complete_pending_escapes(state);
        expire_completion_request(state);
        service_llm_repl(state);
        schedule_managed_submissions(state);
        apply_async_transition(state);
        prepare_classic_redraw(state);
        prepare_managed_render(state);

        if (dispatch_classic_auto_help(state)) continue;
        if (dispatch_classic_pending(state)) continue;
        descriptor_count = prepare_reactor_descriptors(state, descriptors);

        result = gsh_fault_should_fail(GSH_FAULT_POLL, EIO)
                     ? -1
                     : poll(descriptors, descriptor_count,
                            reactor_poll_timeout(state));
        if (result == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("gsh: poll");
            state->last_status = 1;
            break;
        }

        complete_pending_escapes(state);
        expire_completion_request(state);
        service_start = monotonic_ns();
        service_reactor_descriptors(state, descriptors, descriptor_count);
        record_reactor_duration(state, service_start, monotonic_ns());
    }
    return state->last_status;
}

static void leave_managed_screen(shell_state *state)
{
    if (state == NULL) return;
    static const char sequence[] =
        "\033[0m\033[?25h\033[?1000l\033[?1002l\033[?1003l"
        "\033[?1004l\033[?1006l\033[?1015l\033[?2004l\033[?2026l"
        "\033[>4;0m\033[<u\033>\033[?1049l";
    unsigned int attempts;

    if (state->async_repl == NULL ||
        !state_async_repl(state)->alternate_screen_entered || state->tty_fd < 0) {
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
    if (pids == NULL) return 0U;
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
    if (pids == NULL || original_groups == NULL || terminal_groups == NULL) return;
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

static void clear_shell_workspaces(shell_state *state)
{
    if (!require(state != NULL)) return;
    if (!require(state->signal_pipe[0] >= -1 &&
                 state->signal_pipe[1] >= -1)) return;
    state->history = NULL;
    state->session_history = NULL;
    state->parse_storage = NULL;
    state->native_pipeline = NULL;
    state->command_cache = NULL;
    state->command_cache_scratch = NULL;
    state->variables = NULL;
    state->variable_scratch = NULL;
    state->pipeline_variables = NULL;
    state->variable_commit = NULL;
    state->pipeline_changes = NULL;
    state->async_repl = NULL;
    state->alias_expansion = NULL;
    state->aliases = NULL;
    state->alias_scratch = NULL;
    state->alias_commit = NULL;
    state->functions = NULL;
    state->function_scratch = NULL;
    state->source_workspaces = NULL;
    state->positionals = NULL;
    state->positional_storage = NULL;
    state->positional_commit = NULL;
}

static void persist_history(shell_state *state)
{
    int saved_errno;

    if (!require(state != NULL)) return;
    if (!require(state->history != NULL && state->session_history != NULL))
        return;
    if (state->history_persistent &&
        gsh_history_file_save(&state->history_file, state->history,
                              state->session_history,
                              state->config.history_max_entries,
                              state->config.history_deduplicate) == -1) {
        saved_errno = errno;
        (void)fprintf(stderr, "gsh: cannot save %s: %s\n",
                      state->history_file.path, strerror(saved_errno));
    }
    gsh_history_file_close(&state->history_file);
    gsh_history_clear(state->session_history);
    gsh_history_clear(state->history);
}

/* ── Journal Shutdown Has A Finite Drain Budget ─────────────────
 * Closing a shell must not wait forever for another session's journal lock.
 * Once terminal ownership is restored, at most 250 ms drains queued frames
 * and lets the writer finish its durability work. A stalled writer is killed
 * and reaped; unfinished records are reported without changing shell status.
 * ─────────────────────────────────────────────────────────────── */
static void finish_journal_worker(shell_state *state)
{
    unsigned int attempt;

    if (state == NULL || state->journal_pid <= 0) return;
    for (attempt = 0U; attempt < 50U; attempt++) {
        int status = 0;
        pid_t result;
        struct pollfd ready = {state->journal.descriptor, POLLOUT, 0};

        if (state->journal.descriptor >= 0 &&
            (gsh_llm_journal_flush(&state->journal) == -1 ||
             state->journal.used == 0U)) close_journal_pipe(state);
        result = waitpid(state->journal_pid, &status, WNOHANG);
        if (result == state->journal_pid || (result < 0 && errno == ECHILD)) {
            state->journal_pid = -1;
            close_journal_pipe(state);
            if (result > 0 && (!WIFEXITED(status) || WEXITSTATUS(status) != 0))
                (void)fputs("gsh: AI journal writer failed\n", stderr);
            return;
        }
        ready.fd = state->journal.descriptor;
        (void)poll(&ready, 1U, 5);
    }
    close_journal_pipe(state);
    (void)kill(state->journal_pid, SIGKILL);
    while (waitpid(state->journal_pid, NULL, 0) == -1 && errno == EINTR) {
    }
    state->journal_pid = -1;
    (void)fputs("gsh: AI journal shutdown incomplete; pending records lost\n",
                 stderr);
}

static void cleanup(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    pid_t worker_pid = state->redirection_worker_pid;
    pid_t completion_pid = state->completion_pid;
    pid_t managed_pids[GSH_ASYNC_CELL_CAP] = {0};
    pid_t managed_groups[GSH_ASYNC_CELL_CAP] = {0};
    pid_t managed_terminal_groups[GSH_ASYNC_CELL_CAP] = {0};
    pid_t background_pids[GSH_BACKGROUND_CAP];
    size_t background_count = gsh_background_live_snapshot(
        &state->background_jobs, background_pids);
    size_t background;
    int managed;

    if (state->async_repl != NULL) {
        for (managed = 0; managed < GSH_ASYNC_CELL_CAP; managed++) {
            managed_pids[managed] =
                state_async_repl(state)->cells[managed].pid;
            managed_groups[managed] =
                state_async_repl(state)->cells[managed].pgid;
        }
        gsh_async_repl_close(state->async_repl);
        for (managed = 0; managed < GSH_ASYNC_CELL_CAP; managed++) {
            managed_terminal_groups[managed] =
                state_async_repl(state)->cells[managed].pgid;
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
    state->redirection_worker_restart_pending = false;
    disable_redirection_worker(state, true);
    cancel_completion_request(state, true);
    close_llm_repl(state);
    close_variable_commit(state);
    terminate_managed_children(managed_pids, managed_groups,
                               managed_terminal_groups);
    if (worker_pid > 0) {
        while (waitpid(worker_pid, NULL, 0) == -1 && errno == EINTR) {
        }
        state->redirection_worker_pid = -1;
    }
    if (completion_pid > 0) {
        while (waitpid(completion_pid, NULL, 0) == -1 && errno == EINTR) {
        }
        state->completion_pid = -1;
    }
    leave_managed_screen(state);
    restore_terminal(state);
    finish_journal_worker(state);
    g_signal_write_fd = -1;
    if (state->signal_pipe[0] >= 0) {
        (void)close(state->signal_pipe[0]);
    }
    if (state->signal_pipe[1] >= 0) {
        (void)close(state->signal_pipe[1]);
    }
    persist_history(state);
    if (state->tty_fd >= 0) {
        (void)close(state->tty_fd);
    }
    clear_shell_workspaces(state);
}

static void print_usage(FILE *stream)
{
    if (stream == NULL) {
        return;
    }
    (void)fprintf(stream,
            "usage: gsh [options] [command_file [argument ...]]\n"
            "       gsh [options] -s [argument ...]\n"
            "       gsh [options] -c command_string "
            "[command_name [argument ...]]\n"
            "       gsh --native-only -c command_string "
            "[command_name [argument ...]]\n\n"
            "Options: -a -b -C -f -h -n -u -v -x, their + forms, and "
            "-o/+o option.\n"
            "Use --help for this text.\n\n"
            "Run the interactive reactor, a command file, standard input, "
            "or one command string.\n"
            "The async REPL is enabled by default; set GSH_REPL=classic or "
            "shell.async_repl.enabled=false in ~/.gshrc to start in classic "
            "mode.\n"
            "Enter /async as an exact interactive line to toggle it for the "
            "current session.\n");
}

static int check_native_syntax(const char *input)
{
    if (input == NULL) {
        return -1;
    }
    static gsh_parse_storage storage;
    gsh_parse_result result;

    if (gsh_fault_should_fail(GSH_FAULT_ALLOCATION, ENOMEM)) {
        perror("gsh: syntax allocation");
        return 2;
    }
    result = gsh_parse(input, strlen(input), &storage);
    if (result.status == GSH_PARSE_OK) {
        return 0;
    }
    (void)fprintf(stderr, "gsh: %s at byte %zu\n",
            gsh_parse_status_name(result.status), result.error_offset);
    return 2;
}

static bool storage_has_function(const gsh_parse_storage *storage)
{
    size_t index;

    if (storage == NULL || storage->node_count > GSH_PARSE_NODE_CAP) {
        return false;
    }
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

static const gsh_builtin_io *evaluator_file_builtin_io(
    native_evaluator *evaluator);
static void close_evaluator_exec_transaction(native_evaluator *evaluator);
static gsh_command_cache *evaluator_command_cache(
    native_evaluator *evaluator);
static const gsh_times_context *evaluator_times_context(
    native_evaluator *evaluator);
static int evaluator_last_status(const native_evaluator *evaluator);
static gsh_background_table *evaluator_backgrounds(
    native_evaluator *evaluator);
static int evaluator_job_service_socket(
    const native_evaluator *evaluator);
static bool evaluator_job_service_available(
    const native_evaluator *evaluator);
static gsh_trap_store *evaluator_trap_store(native_evaluator *evaluator);
typedef enum {
    NATIVE_TRAPS_PENDING,
    NATIVE_TRAPS_EXIT,
} native_trap_run_kind;
static int run_native_traps(native_evaluator *evaluator, int status,
                            native_trap_run_kind kind);
static void enter_native_subshell_or_exit(native_evaluator *evaluator);

typedef enum {
    PIPELINE_ISOLATED_FUNCTION,
    PIPELINE_ISOLATED_FC,
    PIPELINE_ISOLATED_SOURCE,
} pipeline_isolated_kind;

enum {
    GSH_EVALUATOR_SOURCE_REQUEST = 256,
    GSH_EVALUATOR_FUNCTION_REQUEST = 257,
    GSH_EVALUATOR_PIPELINE_CHILD_REQUEST = 258,
    GSH_EVALUATOR_PIPELINE_EXIT_REQUEST = 259,
    GSH_EVALUATOR_TRAP_REQUEST = 260,
};

static int run_pipeline_isolated_command(
    native_evaluator *parent, gsh_native_pipeline *pipeline,
    size_t command_index, gsh_variable_store *variables,
    pipeline_isolated_kind kind, bool errexit_suppressed, bool *handled);
static int request_pipeline_exit(native_evaluator *evaluator, int status);
static int evaluate_loop_control(native_evaluator *evaluator,
                                 const gsh_native_pipeline *pipeline);

static int noninteractive_assignment_error(int assignment_status,
                                           const char *operation)
{
    if (!require(operation != NULL)) return 125;
    if (!require(assignment_status != GSH_ASSIGNMENT_OK)) return 125;
    perror(operation);
    return assignment_status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125 : 1;
}

static bool noninteractive_simple_builtin_status(
    gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    gsh_variable_journal *journal, gsh_alias_store *aliases,
    gsh_alias_journal *alias_journal, gsh_shell_options *options,
    int *status)
{
    const gsh_native_command *command;
    int builtin_status;

    if (!require(pipeline != NULL && variables != NULL)) return false;
    if (!require(options != NULL && status != NULL)) return false;
    if (pipeline->command_count != 1U ||
        pipeline->commands[0].redirect_count != 0U) {
        return false;
    }
    command = &pipeline->commands[0];
    if (native_pure_builtin(command)) {
        if (native_colon_builtin(command) && command->assignment_count != 0) {
            int assignment_status = apply_special_builtin_assignments(
                variables, journal, command, options);

            if (assignment_status != GSH_ASSIGNMENT_OK) {
                *status = noninteractive_assignment_error(
                    assignment_status, "gsh: assignment");
                return true;
            }
        }
        builtin_status = run_native_pure_builtin(
            command, &descriptor_builtin_io);
    } else if (native_environment_builtin(command)) {
        builtin_status = run_native_environment_builtin(
            command, &descriptor_builtin_io);
    } else if (native_variable_builtin(command)) {
        int assignment_status = apply_special_builtin_assignments(
            variables, journal, command, options);

        if (assignment_status != GSH_ASSIGNMENT_OK) {
            *status = noninteractive_assignment_error(
                assignment_status, "gsh: assignment");
            return true;
        }
        builtin_status = run_native_variable_builtin(
            command, variables, journal, options, NULL,
            &descriptor_builtin_io);
    } else if (native_alias_builtin(command)) {
        if (aliases == NULL || alias_journal == NULL) return false;
        builtin_status = run_native_alias_builtin(
            command, aliases, alias_journal, &descriptor_builtin_io);
    } else {
        return false;
    }
    *status = builtin_status == 125
                  ? 125
                  : (pipeline->negated ? (builtin_status == 0 ? 1 : 0)
                                       : builtin_status);
    return true;
}

static bool noninteractive_inspection_builtin_status(
    gsh_native_pipeline *pipeline, const char *default_path,
    gsh_variable_store *variables, gsh_variable_journal *journal,
    gsh_alias_store *aliases, gsh_shell_options *options,
    gsh_function_store *functions, native_evaluator *evaluator,
    int *status)
{
    if (default_path == NULL || evaluator == NULL || functions == NULL) {
        return false;
    }
    const gsh_native_command *command;
    int builtin_status;

    if (!require(pipeline != NULL && variables != NULL)) return false;
    if (!require(options != NULL && status != NULL)) return false;
    if (pipeline->command_count != 1U ||
        pipeline->commands[0].redirect_count != 0U) {
        return false;
    }
    command = &pipeline->commands[0];
    if (native_hash_builtin(command)) {
        const char *path = hash_command_path_value(
            variables, command, default_path);

        builtin_status = run_native_hash_builtin(
            command, path, functions, evaluator_command_cache(evaluator),
            hash_command_path_generation(variables, command), NULL,
            &descriptor_builtin_io);
    } else if (native_times_builtin(command)) {
        int assignment_status = apply_special_builtin_assignments(
            variables, journal, command, options);

        if (assignment_status != GSH_ASSIGNMENT_OK) {
            *status = noninteractive_assignment_error(
                assignment_status, "gsh: times assignment");
            return true;
        }
        builtin_status = run_native_times_builtin(
            command, evaluator_times_context(evaluator),
            &descriptor_builtin_io);
    } else if (native_command_inspection_builtin(command)) {
        if (aliases == NULL) return false;
        const char *path = command_path_value(
            variables, command, default_path);

        builtin_status = run_native_command_inspection(
            command, path, default_path, aliases, functions,
            evaluator_command_cache(evaluator),
            gsh_variables_path_generation(variables),
            command_uses_persistent_path(command), NULL,
            &descriptor_builtin_io);
    } else {
        return false;
    }
    *status = builtin_status == 125
                  ? 125
                  : (pipeline->negated ? (builtin_status == 0 ? 1 : 0)
                                       : builtin_status);
    return true;
}

static bool noninteractive_direct_status(
    gsh_native_pipeline *pipeline, const char *default_path,
    gsh_variable_store *variables, gsh_variable_journal *journal,
    gsh_alias_store *aliases, gsh_alias_journal *alias_journal,
    gsh_positional_store *positionals, gsh_shell_options *options,
    gsh_function_store *functions, gsh_variable_store *scratch,
    native_evaluator *evaluator, int *status)
{
    if (default_path == NULL || functions == NULL) {
        return false;
    }
    const gsh_native_command *command;

    if (!require(pipeline != NULL && variables != NULL)) return false;
    if (!require(options != NULL && status != NULL)) return false;
    if (pipeline->command_count != 1U) return false;
    command = &pipeline->commands[0];
    if (command->argc == 0 && command->assignment_count > 0 &&
        command->redirect_count == 0) {
        int assignment_status = apply_native_assignments(
            variables, journal, command, options);

        if (assignment_status != GSH_ASSIGNMENT_OK) {
            *status = noninteractive_assignment_error(
                assignment_status, "gsh: assignment");
        } else {
            int value = command->command_substitution_performed
                            ? command->command_substitution_status
                            : 0;

            *status = pipeline->negated ? (value == 0 ? 1 : 0) : value;
        }
        return true;
    }
    if (evaluator != NULL && native_loop_control_builtin(command)) {
        *status = evaluate_loop_control(evaluator, pipeline);
        return true;
    }
    if (native_exec_builtin(command)) {
        bool builtin_failed;

        *status = run_evaluator_exec_builtin(
            pipeline, variables, journal, options, default_path,
            evaluator_command_cache(evaluator), NULL, -1, NULL,
            &builtin_failed);
        (void)builtin_failed;
        return true;
    }
    if (evaluator != NULL && native_posix_stateful_builtin(command)) {
        if (positionals == NULL || scratch == NULL) {
            *status = 125;
            return true;
        }
        *status = run_evaluator_posix_stateful_builtin(
            pipeline, variables, scratch, journal, positionals, options);
        return true;
    }
    if (evaluator != NULL && native_job_control_builtin(command)) {
        if (scratch == NULL) {
            *status = 125;
            return true;
        }
        *status = run_evaluator_job_control_builtin(
            pipeline, variables, scratch, options,
            evaluator_backgrounds(evaluator),
            evaluator_job_service_socket(evaluator),
            evaluator_job_service_available(evaluator));
        return true;
    }
    if (native_variable_builtin(command) && command->redirect_count != 0) {
        *status = run_evaluator_variable_builtin(
            pipeline, variables, journal, options, NULL);
        return true;
    }
    if (native_colon_builtin(command) && command->assignment_count != 0 &&
        command->redirect_count != 0) {
        *status = run_evaluator_colon_builtin(
            pipeline, variables, journal, options);
        return true;
    }
    return noninteractive_simple_builtin_status(
               pipeline, variables, journal, aliases, alias_journal,
               options, status) ||
           noninteractive_inspection_builtin_status(
               pipeline, default_path, variables, journal, aliases,
               options, functions, evaluator, status);
}

typedef struct {
    int pipes[GSH_NATIVE_PIPELINE_CAP - 1][2];
    int heredoc_pipes[GSH_NATIVE_HEREDOC_CAP][2];
    pid_t members[GSH_NATIVE_JOB_MEMBER_CAP];
    size_t pipe_count;
    size_t created_pipes;
    size_t created_heredocs;
    size_t launched;
    gsh_pipeline_status pipeline_status;
} noninteractive_pipeline_launch;

static void initialize_noninteractive_launch(
    noninteractive_pipeline_launch *launch, size_t command_count,
    bool pipefail)
{
    if (!require(launch != NULL)) return;
    (void)memset(launch->members, 0, sizeof(launch->members));
    initialize_pipeline_descriptors(launch->pipes);
    initialize_heredoc_descriptors(launch->heredoc_pipes);
    launch->pipe_count = command_count > 0U ? command_count - 1U : 0U;
    launch->created_pipes = 0U;
    launch->created_heredocs = 0U;
    launch->launched = 0U;
    gsh_pipeline_status_initialize(&launch->pipeline_status,
                                   command_count, pipefail);
    if (!require(command_count > 0U)) return;
}

static bool create_noninteractive_descriptors(
    const gsh_native_pipeline *pipeline,
    noninteractive_pipeline_launch *launch)
{
    if (!require(pipeline != NULL)) return false;
    if (!require(launch != NULL)) return false;
    for (launch->created_heredocs = 0;
         launch->created_heredocs < pipeline->heredoc_count;
         launch->created_heredocs++) {
        if (make_pipe(launch->heredoc_pipes[launch->created_heredocs], false,
                      GSH_FAULT_HEREDOC_PIPE) == -1) {
            perror("gsh: here-document pipe");
            close_heredoc_descriptors(launch->heredoc_pipes,
                                      launch->created_heredocs);
            return false;
        }
    }
    for (launch->created_pipes = 0;
         launch->created_pipes < launch->pipe_count;
         launch->created_pipes++) {
        if (make_pipe(launch->pipes[launch->created_pipes], false,
                      GSH_FAULT_PIPELINE_PIPE) == -1) {
            perror("gsh: pipeline pipe");
            close_pipeline_descriptors(launch->pipes,
                                       launch->created_pipes);
            close_heredoc_descriptors(launch->heredoc_pipes,
                                      launch->created_heredocs);
            return false;
        }
    }
    return true;
}

static void terminate_noninteractive_launch(
    noninteractive_pipeline_launch *launch, const char *operation,
    int error)
{
    size_t index;

    if (!require(launch != NULL)) return;
    if (!require(operation != NULL)) return;
    close_pipeline_descriptors(launch->pipes, launch->created_pipes);
    close_heredoc_descriptors(launch->heredoc_pipes,
                              launch->created_heredocs);
    for (index = 0; index < launch->launched; index++) {
        (void)kill(launch->members[index], SIGKILL);
    }
    for (index = 0; index < launch->launched; index++) {
        while (waitpid(launch->members[index], NULL, 0) == -1 &&
               errno == EINTR) {
        }
    }
    errno = error;
    perror(operation);
}

static void prepare_noninteractive_pipeline_child(
    gsh_native_pipeline *pipeline, gsh_variable_store *variables,
    const pipeline_expansion_scope *scope, gsh_shell_options *options,
    native_evaluator *evaluator, noninteractive_pipeline_launch *launch,
    size_t index)
{
    if (evaluator == NULL || options == NULL) {
        return;
    }
    size_t close_index;

    if (!require(pipeline != NULL && variables != NULL)) _exit(125);
    if (!require(launch != NULL && index < pipeline->command_count)) {
        _exit(125);
    }
    reset_child_signals();
    enter_native_subshell_or_exit(evaluator);
    close_evaluator_exec_transaction(evaluator);
    if (pipeline->commands[index].expansion_error) _exit(1);
    if (scope != NULL &&
        gsh_variables_apply_journal_scope_in_place(
            variables, scope->changes, index + 1U) == -1) {
        child_exec_error("pipeline variable scope", errno);
    }
    if (index > 0 &&
        child_duplicate_descriptor(launch->pipes[index - 1U][0],
                                   STDIN_FILENO) == -1) {
        child_exec_error("pipeline input", errno);
    }
    if (index + 1U < pipeline->command_count &&
        child_duplicate_descriptor(launch->pipes[index][1],
                                   STDOUT_FILENO) == -1) {
        child_exec_error("pipeline output", errno);
    }
    child_apply_redirects(&pipeline->commands[index], launch->heredoc_pipes,
                          pipeline->heredoc_count, options);
    for (close_index = 0; close_index < launch->created_pipes;
         close_index++) {
        (void)close(launch->pipes[close_index][0]);
        (void)close(launch->pipes[close_index][1]);
    }
    close_heredoc_descriptors(launch->heredoc_pipes,
                              pipeline->heredoc_count);
}

static bool noninteractive_child_job_status(
    const gsh_native_command *command, gsh_variable_store *variables,
    gsh_shell_options *options, native_evaluator *evaluator, int *status)
{
    if (!require(command != NULL && variables != NULL)) return false;
    if (!require(options != NULL && status != NULL)) return false;
    if (!native_job_control_builtin(command)) return false;
    if (apply_native_assignments(variables, NULL, command, options) !=
        GSH_ASSIGNMENT_OK) {
        child_exec_error("job builtin assignment", errno);
    }
    if (evaluator_job_service_available(evaluator) &&
        native_snapshot_job_control_builtin(command)) {
        *status = request_reactor_job_service(
            evaluator_job_service_socket(evaluator), command);
    } else if (native_jobs_builtin(command)) {
        *status = gsh_builtin_jobs(
            command->argc, command->argv,
            evaluator_backgrounds(evaluator), &descriptor_builtin_io);
    } else if (native_kill_builtin(command)) {
        *status = gsh_builtin_kill(
            command->argc, command->argv,
            evaluator_backgrounds(evaluator), &descriptor_builtin_io);
    } else {
        *status = gsh_builtin_error(
            &descriptor_builtin_io, command->argv[0],
            "not available outside the interactive reactor");
    }
    return true;
}

static bool noninteractive_child_primary_status(
    const gsh_native_command *command, gsh_variable_store *variables,
    gsh_variable_store *scratch, gsh_positional_store *positionals,
    gsh_shell_options *options, native_evaluator *evaluator, int *status)
{
    if (!require(command != NULL && variables != NULL)) return false;
    if (!require(options != NULL && status != NULL)) return false;
    if (native_pure_builtin(command)) {
        *status = run_native_pure_builtin(command, &descriptor_builtin_io);
    } else if (native_file_builtin(command)) {
        *status = run_native_file_builtin(
            command, evaluator_file_builtin_io(evaluator));
    } else if (native_posix_stateful_builtin(command)) {
        if (positionals == NULL || scratch == NULL) {
            *status = 125;
            return true;
        }
        *status = child_run_posix_stateful_builtin(
            command, variables, scratch, positionals, options);
    } else if (noninteractive_child_job_status(
                   command, variables, options, evaluator, status)) {
        return true;
    } else if (native_exit_builtin(command)) {
        if (apply_special_builtin_assignments(
                variables, NULL, command, options) != GSH_ASSIGNMENT_OK) {
            child_exec_error("exit assignment", errno);
        }
        (void)parse_exit_status(
            command, evaluator_last_status(evaluator), status);
    } else if (native_pwd_builtin(command)) {
        *status = child_run_pwd(command, variables);
    } else if (native_cd_builtin(command)) {
        if (apply_native_assignments(variables, NULL, command, options) !=
            GSH_ASSIGNMENT_OK) {
            child_exec_error("assignment", errno);
        }
        *status = run_native_cd_builtin(
            command, variables, variables, NULL, options,
            &descriptor_builtin_io, NULL, 0);
    } else if (native_environment_builtin(command)) {
        *status = run_native_environment_builtin(
            command, &descriptor_builtin_io);
    } else {
        return false;
    }
    return true;
}

static bool noninteractive_child_state_status(
    const gsh_native_command *command, const char *default_path,
    gsh_variable_store *variables, gsh_alias_store *aliases,
    gsh_positional_store *positionals, gsh_shell_options *options,
    gsh_function_store *functions, native_evaluator *evaluator, int *status)
{
    if (!require(command != NULL && variables != NULL)) return false;
    if (!require(options != NULL && status != NULL)) return false;
    if (native_variable_builtin(command)) {
        if (apply_special_builtin_assignments(
                variables, NULL, command, options) != GSH_ASSIGNMENT_OK) {
            child_exec_error("assignment", errno);
        }
        *status = run_native_variable_builtin(
            command, variables, NULL, options, NULL, &descriptor_builtin_io);
    } else if (native_state_builtin(command)) {
        if (positionals == NULL) {
            *status = 125;
            return true;
        }
        if (apply_special_builtin_assignments(
                variables, NULL, command, options) != GSH_ASSIGNMENT_OK) {
            child_exec_error("assignment", errno);
        }
        *status = run_native_state_builtin(
            command, variables, positionals, options,
            &descriptor_builtin_io);
    } else if (native_wait_builtin(command)) {
        *status = command->argc == 1U ? 0 : 127;
    } else if (native_alias_builtin(command)) {
        if (aliases == NULL) {
            *status = 125;
            return true;
        }
        *status = run_native_alias_builtin(
            command, aliases, NULL, &descriptor_builtin_io);
    } else if (native_hash_builtin(command)) {
        if (default_path == NULL || evaluator == NULL || functions == NULL) {
            *status = 125;
            return true;
        }
        const char *path = hash_command_path_value(
            variables, command, default_path);

        *status = run_native_hash_builtin(
            command, path, functions, evaluator_command_cache(evaluator),
            hash_command_path_generation(variables, command), NULL,
            &descriptor_builtin_io);
    } else {
        return false;
    }
    return true;
}

static bool noninteractive_child_inspection_status(
    const gsh_native_command *command, const char *default_path,
    gsh_variable_store *variables, gsh_alias_store *aliases,
    gsh_shell_options *options, gsh_function_store *functions,
    native_evaluator *evaluator, int *status)
{
    if (aliases == NULL || default_path == NULL || evaluator == NULL || functions == NULL) {
        return false;
    }
    if (!require(command != NULL && variables != NULL)) return false;
    if (!require(options != NULL && status != NULL)) return false;
    if (native_times_builtin(command)) {
        if (apply_special_builtin_assignments(
                variables, NULL, command, options) != GSH_ASSIGNMENT_OK) {
            child_exec_error("assignment", errno);
        }
        *status = run_native_times_builtin(
            command, NULL, &descriptor_builtin_io);
    } else if (native_command_inspection_builtin(command)) {
        const char *path = command_path_value(
            variables, command, default_path);

        *status = run_native_command_inspection(
            command, path, default_path, aliases, functions,
            evaluator_command_cache(evaluator),
            gsh_variables_path_generation(variables),
            command_uses_persistent_path(command), NULL,
            &descriptor_builtin_io);
    } else {
        return false;
    }
    return true;
}

static bool noninteractive_child_exec_status(
    const gsh_native_command *command, const char *default_path,
    gsh_variable_store *variables, native_evaluator *evaluator, int *status)
{
    if (default_path == NULL || evaluator == NULL) {
        return false;
    }
    size_t utility_index;

    if (!require(command != NULL && variables != NULL)) return false;
    if (!require(status != NULL)) return false;
    if (!native_exec_builtin(command)) return false;
    if (exec_utility_index(command, &utility_index) == -1) {
        *status = 2;
        return true;
    }
    if (utility_index == command->argc) {
        *status = 0;
        return true;
    }
    {
        char *environment_storage[CHILD_ENVIRONMENT_CAP];
        char *const *environment = child_command_environment(
            variables, command, environment_storage);

        child_exec_direct(
            command->argv + utility_index,
            command_path_value(variables, command, default_path),
            environment, evaluator_command_cache(evaluator),
            command_cache_path_generation(variables, command),
            command_uses_persistent_path(command));
    }
    *status = 126;
    return true;
}

static bool noninteractive_child_leaf_status(
    const gsh_native_command *command, const char *default_path,
    gsh_variable_store *variables, gsh_variable_store *scratch,
    gsh_alias_store *aliases, gsh_positional_store *positionals,
    gsh_shell_options *options, gsh_function_store *functions,
    native_evaluator *evaluator, int *status)
{
    if (default_path == NULL || evaluator == NULL) {
        return false;
    }
    if (!require(command != NULL && variables != NULL)) return false;
    if (!require(options != NULL && status != NULL)) return false;
    return noninteractive_child_primary_status(
               command, variables, scratch, positionals, options,
               evaluator, status) ||
           noninteractive_child_state_status(
               command, default_path, variables, aliases, positionals,
               options, functions, evaluator, status) ||
           noninteractive_child_exec_status(
               command, default_path, variables, evaluator, status) ||
           noninteractive_child_inspection_status(
               command, default_path, variables, aliases, options,
               functions, evaluator, status);
}

static void execute_noninteractive_external(
    const gsh_native_command *command, const char *default_path,
    gsh_variable_store *variables, native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return;
    }
    char *environment_storage[CHILD_ENVIRONMENT_CAP];
    char *const *environment;

    if (!require(command != NULL && variables != NULL)) _exit(125);
    if (!require(default_path != NULL)) _exit(125);
    environment = child_command_environment(
        variables, command, environment_storage);
    child_exec_direct(
        command->argv,
        command_path_value(variables, command, default_path), environment,
        evaluator_command_cache(evaluator),
        command_cache_path_generation(variables, command),
        command_uses_persistent_path(command));
}

static bool launch_noninteractive_heredocs(
    const gsh_native_pipeline *pipeline,
    noninteractive_pipeline_launch *launch)
{
    size_t index;

    if (!require(pipeline != NULL)) return false;
    if (!require(launch != NULL)) return false;
    for (index = 0; index < pipeline->heredoc_count; index++) {
        pid_t pid = gsh_fault_should_fail(GSH_FAULT_HEREDOC_FORK, EAGAIN) ? -1 : fork();

        if (pid == 0) {
            reset_child_signals();
            close_pipeline_descriptors(launch->pipes,
                                       launch->created_pipes);
            child_write_heredoc(pipeline, index, launch->heredoc_pipes);
        }
        if (pid == -1) {
            int saved_errno = errno;

            terminate_noninteractive_launch(
                launch, "gsh: here-document fork", saved_errno);
            return false;
        }
        launch->members[launch->launched++] = pid;
    }
    return true;
}

static int wait_for_noninteractive_pipeline(
    const gsh_native_pipeline *pipeline,
    noninteractive_pipeline_launch *launch)
{
    int selected_wait_status = 0;
    size_t index;

    if (!require(pipeline != NULL)) return 125;
    if (!require(launch != NULL)) return 125;
    close_pipeline_descriptors(launch->pipes, launch->created_pipes);
    close_heredoc_descriptors(launch->heredoc_pipes,
                              launch->created_heredocs);
    for (index = 0; index < launch->launched; index++) {
        int status;
        pid_t waited;

        do {
            waited = waitpid(launch->members[index], &status, 0);
        } while (waited == -1 && errno == EINTR);
        if (waited == -1) {
            perror("gsh: waitpid");
            return 125;
        }
        if (index < pipeline->command_count &&
            !gsh_pipeline_status_record(
                &launch->pipeline_status, index, status)) {
            return 125;
        }
    }
    return gsh_pipeline_status_result(
               &launch->pipeline_status, &selected_wait_status)
               ? native_wait_status_value(selected_wait_status,
                                          pipeline->negated)
               : 125;
}

typedef struct {
    gsh_native_pipeline *pipeline;
    const char *default_path;
    gsh_variable_store *variables;
    gsh_alias_store *aliases;
    const pipeline_expansion_scope *scope;
    gsh_positional_store *positionals;
    gsh_shell_options *options;
    gsh_function_store *functions;
    gsh_variable_store *scratch;
    native_evaluator *evaluator;
    noninteractive_pipeline_launch *launch;
    bool errexit_suppressed;
} noninteractive_child_context;

static gsh_native_pipeline *noninteractive_context_pipeline(
    const noninteractive_child_context *context)
{
    if (!require(context != NULL)) return NULL;
    if (!require(context->pipeline != NULL)) return NULL;
    return context->pipeline;
}

static int run_noninteractive_pipeline_child(
    const noninteractive_child_context *context, size_t index)
{
    if (!require(context != NULL && context->pipeline != NULL)) return 125;
    if (!require(context->evaluator != NULL && context->launch != NULL)) {
        return 125;
    }
    const gsh_native_command *command = &noninteractive_context_pipeline(context)->commands[index];
    bool handled = false;
    int status;

    prepare_noninteractive_pipeline_child(
        context->pipeline, context->variables, context->scope,
        context->options, context->evaluator, context->launch, index);
    status = run_pipeline_isolated_command(
        context->evaluator, context->pipeline, index, context->variables,
        PIPELINE_ISOLATED_FUNCTION, context->errexit_suppressed, &handled);
    if (handled) {
        return status == GSH_EVALUATOR_PIPELINE_CHILD_REQUEST
                   ? status
                   : request_pipeline_exit(context->evaluator, status);
    }
    if (gsh_fault_should_fail(GSH_FAULT_EXEC, EIO)) {
        child_exec_error(command->argv[0], errno);
    }
    if (noninteractive_child_leaf_status(
            command, context->default_path, context->variables,
            context->scratch, context->aliases, context->positionals,
            context->options, context->functions, context->evaluator,
            &status)) {
        return status;
    }
    if (native_fc_builtin(command) || native_source_builtin(command)) {
        pipeline_isolated_kind kind = native_fc_builtin(command)
                                          ? PIPELINE_ISOLATED_FC
                                          : PIPELINE_ISOLATED_SOURCE;

        status = run_pipeline_isolated_command(
            context->evaluator, context->pipeline, index,
            context->variables, kind, context->errexit_suppressed, &handled);
        if (!handled) return 125;
        return status == GSH_EVALUATOR_PIPELINE_CHILD_REQUEST
                   ? status
                   : request_pipeline_exit(context->evaluator, status);
    }
    if (native_trap_builtin(command)) {
        if (apply_special_builtin_assignments(
                context->variables, NULL, command, context->options) !=
            GSH_ASSIGNMENT_OK) {
            child_exec_error("trap assignment", errno);
        }
        status = gsh_builtin_trap(
            (int)command->argc, command->argv,
            evaluator_trap_store(context->evaluator),
            &descriptor_builtin_io);
        return request_pipeline_exit(context->evaluator, status);
    }
    execute_noninteractive_external(
        command, context->default_path, context->variables,
        context->evaluator);
    return 126;
}

static int run_native_noninteractive_pipeline(
    gsh_native_pipeline *pipeline, const char *default_path,
    gsh_variable_store *variables, gsh_variable_journal *journal,
    gsh_alias_store *aliases, gsh_alias_journal *alias_journal,
    const pipeline_expansion_scope *scope,
    gsh_positional_store *positionals, gsh_shell_options *options,
    gsh_function_store *functions, gsh_variable_store *scratch,
    native_evaluator *evaluator, bool errexit_suppressed)
{
    if (aliases == NULL || default_path == NULL || evaluator == NULL ||
        functions == NULL || options == NULL || pipeline == NULL ||
        variables == NULL) {
        return -1;
    }
    noninteractive_pipeline_launch launch;
    noninteractive_child_context child_context = {
        pipeline, default_path, variables, aliases, scope, positionals,
        options, functions, scratch, evaluator, &launch,
        errexit_suppressed};
    size_t index;

    if (!require(pipeline != NULL && variables != NULL)) return 125;
    if (!require(options != NULL)) return 125;
    initialize_noninteractive_launch(
        &launch, pipeline->command_count,
        gsh_options_enabled(options, GSH_OPTION_PIPEFAIL));

    if (scope == NULL && evaluator_command_cache(evaluator) != NULL) {
        for (index = 0; index < pipeline->command_count; index++) {
            (void)cache_planned_external(
                evaluator_command_cache(evaluator), variables,
                &pipeline->commands[index], default_path, functions);
        }
    }

    {
        int direct_status;

        if (noninteractive_direct_status(
                pipeline, default_path, variables, journal, aliases,
                alias_journal, positionals, options, functions, scratch,
                evaluator, &direct_status)) {
            return direct_status;
        }
    }

    if (!create_noninteractive_descriptors(pipeline, &launch)) return 125;
    for (index = 0; index < pipeline->command_count; index++) {
        pid_t pid = gsh_fault_should_fail(GSH_FAULT_PIPELINE_FORK, EAGAIN) ? -1 : fork();

        if (pid == 0) {
            int child_status = run_noninteractive_pipeline_child(
                &child_context, index);

            if (child_status > 255) return child_status;
            _exit(child_status & 255);
        }
        if (pid == -1) {
            int saved_errno = errno;

            terminate_noninteractive_launch(
                &launch, "gsh: pipeline fork", saved_errno);
            return 125;
        }
        launch.members[launch.launched++] = pid;
    }
    if (!launch_noninteractive_heredocs(pipeline, &launch)) return 125;
    return wait_for_noninteractive_pipeline(pipeline, &launch);
}

typedef enum {
    NATIVE_LOOP_CONTROL_NONE,
    NATIVE_LOOP_CONTROL_BREAK,
    NATIVE_LOOP_CONTROL_CONTINUE,
} native_loop_control;

typedef struct function_evaluation_frame function_evaluation_frame;

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
    const gsh_history_store *history;
    bool history_exclude_newest;
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
    gsh_command_cache *command_cache;
    uint64_t command_cache_base_generation;
    const gsh_times_context *times_context;
    gsh_variable_store *scope_base;
    gsh_variable_journal *scope_changes;
    gsh_source_workspace_stack *source_workspaces;
    gsh_trap_store *traps;
    pipeline_expansion_scope *pipeline_scope;
    size_t source_depth;
    size_t dot_depth;
    size_t function_depth;
    bool returning;
    int return_status;
    bool exiting;
    int exit_status;
    bool exit_trap_running;
    bool exit_trap_complete;
    size_t active_loops;
    native_loop_control loop_control;
    size_t loop_levels;
    bool preflight;
    bool fatal_error;
    bool static_for_items;
    bool tail_exec_single;
    bool positional_mutation_possible;
    bool directory_mutation_possible;
    bool alias_mutation_possible;
    bool function_mutation_possible;
    bool command_cache_mutation_possible;
    bool job_service_possible;
    bool job_service_available;
    int job_service_socket;
    bool exec_possible;
    int exec_descriptors[GSH_EXEC_DESCRIPTOR_COMMIT_CAP];
    size_t exec_descriptor_count;
    int exec_protected_descriptors[GSH_EXEC_DESCRIPTOR_COMMIT_CAP];
    size_t exec_protected_descriptor_count;
    bool exec_descriptors_dirty;
    bool state_commit_invalid;
    int exec_outcome_fd;
    int exec_descriptor_socket;
    bool source_request_active;
    bool source_request_dot;
    bool source_request_negated;
    bool source_request_temporary_variables;
    gsh_source_workspace *source_request_workspace;
    const gsh_native_command *source_request_command;
    const char *source_request_input;
    size_t source_request_input_length;
    size_t source_request_root;
    gsh_saved_descriptor source_request_saved[
        GSH_NATIVE_REDIRECT_CAP];
    size_t source_request_saved_count;
    gsh_background_table *backgrounds;
    gsh_native_variable_state expansion_variables;
    gsh_native_substitution_state substitutions;
    function_evaluation_frame *function_request_frame;
    size_t function_request_root;
    void *substitution_child_execution;
    void *pipeline_child_request;
    void *trap_request;
    int pipeline_exit_status;
    bool suppress_async_once;
    bool file_resources_enabled;
    gsh_builtin_resource_sink file_resource_sink;
    gsh_builtin_io file_builtin_io;
};

static const gsh_builtin_io *evaluator_file_builtin_io(
    native_evaluator *evaluator)
{
    if (evaluator == NULL || !evaluator->file_resources_enabled)
        return &descriptor_builtin_io;
    return &evaluator->file_builtin_io;
}

static const gsh_parse_storage *evaluator_storage(
    const native_evaluator *evaluator)
{
    if (!require(evaluator != NULL)) return NULL;
    if (!require(evaluator->storage != NULL)) return NULL;
    return evaluator->storage;
}

static gsh_native_pipeline *evaluator_pipeline(
    const native_evaluator *evaluator)
{
    if (!require(evaluator != NULL)) return NULL;
    if (!require(evaluator->pipeline != NULL)) return NULL;
    return evaluator->pipeline;
}

static gsh_function_store *evaluator_functions(
    const native_evaluator *evaluator)
{
    if (!require(evaluator != NULL)) return NULL;
    if (!require(evaluator->functions != NULL)) return NULL;
    return evaluator->functions;
}

static pipeline_expansion_scope *evaluator_pipeline_scope(
    const native_evaluator *evaluator)
{
    if (!require(evaluator != NULL)) return NULL;
    if (!require(evaluator->pipeline_scope != NULL)) return NULL;
    return evaluator->pipeline_scope;
}

static gsh_source_workspace_stack *evaluator_source_workspaces(
    const native_evaluator *evaluator)
{
    if (!require(evaluator != NULL)) return NULL;
    if (!require(evaluator->source_workspaces != NULL)) return NULL;
    return evaluator->source_workspaces;
}

static const gsh_native_command *evaluator_source_request_command(
    const native_evaluator *evaluator)
{
    if (!require(evaluator != NULL)) return NULL;
    if (!require(evaluator->source_request_command != NULL)) return NULL;
    return evaluator->source_request_command;
}

static gsh_trap_store *evaluator_trap_store(native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return NULL;
    }
    return evaluator == NULL ? NULL : evaluator->traps;
}

static void enter_native_subshell_or_exit(native_evaluator *evaluator)
{
    if (evaluator != NULL && evaluator->traps != NULL &&
        gsh_traps_enter_subshell(evaluator->traps) == -1) {
        child_exec_error("trap subshell reset", errno);
    }
}

typedef struct {
    const char *input;
    size_t input_length;
    const gsh_parse_storage *storage;
    gsh_native_pipeline *pipeline;
    gsh_variable_store *variables;
    gsh_variable_store *scope_base;
    gsh_variable_journal *journal;
    size_t source_depth;
    size_t dot_depth;
    gsh_source_workspace *workspace;
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    const gsh_native_command *temporary_command;
    bool consume_return;
    bool negated;
    bool temporary_variables;
} native_source_frame;

static gsh_source_workspace *source_frame_workspace(
    const native_source_frame *frame)
{
    if (!require(frame != NULL)) return NULL;
    if (!require(frame->workspace != NULL)) return NULL;
    return frame->workspace;
}

typedef struct {
    native_evaluator child;
    native_source_frame frame;
    gsh_background_table *backgrounds;
    size_t root;
} pipeline_source_evaluation;

typedef enum {
    PIPELINE_CHILD_FUNCTION,
    PIPELINE_CHILD_SOURCE,
} pipeline_child_kind;

typedef struct pipeline_child_request {
    pipeline_child_kind kind;
    native_evaluator child;
    function_evaluation_frame *function_frame;
    pipeline_source_evaluation source;
    gsh_background_table *backgrounds;
    size_t root;
    bool errexit_suppressed;
    bool used;
} pipeline_child_request;

typedef struct native_trap_request native_trap_request;

static bool native_preflight_node(native_evaluator *evaluator,
                                  size_t node_index, size_t depth);

static int read_source_descriptor(int descriptor, char *input,
                                  size_t *input_length)
{
    size_t used = 0;
    size_t attempts;

    if (descriptor < 0 || input == NULL || input_length == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (attempts = 0; attempts <= GSH_SOURCE_INPUT_CAP; attempts++) {
        size_t available = GSH_SOURCE_INPUT_CAP - used;
        ssize_t count = read(descriptor, input + used,
                             available == 0 ? 1U : available);

        if (count > 0) {
            if (available == 0 || memchr(input + used, '\0',
                                         (size_t)count) != NULL) {
                errno = available == 0 ? EFBIG : EILSEQ;
                return -1;
            }
            used += (size_t)count;
        } else if (count == 0) {
            input[used] = '\0';
            *input_length = used;
            return 0;
        } else if (errno != EINTR) {
            return -1;
        }
    }
    errno = EINTR;
    return -1;
}

static int dot_open_candidate(const char *directory, size_t directory_length,
                              const char *name, size_t name_length)
{
    char candidate[EXEC_PATH_CAP];
    size_t offset = 0;

    if (directory == NULL || name == NULL || name_length == 0) {
        errno = EINVAL;
        return -1;
    }
    if (directory_length == 0) {
        candidate[offset++] = '.';
    } else if (directory_length >= sizeof(candidate)) {
        errno = ENAMETOOLONG;
        return -1;
    } else {
        (void)memcpy(candidate, directory, directory_length);
        offset = directory_length;
    }
    if (offset + 1U + name_length + 1U > sizeof(candidate)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    candidate[offset++] = '/';
    (void)memcpy(candidate + offset, name, name_length + 1U);
    return open(candidate, O_RDONLY | O_CLOEXEC);
}

static int open_dot_source(const char *name, const char *path)
{
    if (path == NULL) {
        return -1;
    }
    size_t name_length;
    const char *cursor = path;
    size_t components;
    int remembered_error = ENOENT;

    if (name == NULL || path == NULL) {
        errno = EINVAL;
        return -1;
    }
    name_length = strnlen(name, EXEC_PATH_CAP);
    if (name_length == 0 || name_length == EXEC_PATH_CAP) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (strchr(name, '/') != NULL) {
        return open(name, O_RDONLY | O_CLOEXEC);
    }
    for (components = 0; components <= GSH_VARIABLE_VALUE_CAP;
         components++) {
        size_t remaining = strnlen(cursor, GSH_VARIABLE_VALUE_CAP + 1U);
        const char *separator;
        size_t directory_length;
        int descriptor;

        if (remaining > GSH_VARIABLE_VALUE_CAP) {
            errno = E2BIG;
            return -1;
        }
        separator = memchr(cursor, ':', remaining);
        directory_length =
            separator == NULL ? remaining
                              : (size_t)(separator - cursor);
        descriptor = dot_open_candidate(
            cursor, directory_length, name, name_length);

        if (descriptor >= 0) {
            return descriptor;
        }
        if (errno == EACCES) {
            remembered_error = EACCES;
        } else if (errno != ENOENT && errno != ENOTDIR) {
            return -1;
        }
        if (separator == NULL) {
            errno = remembered_error;
            return -1;
        }
        cursor = separator + 1U;
    }
    errno = E2BIG;
    return -1;
}

static int concatenate_eval_source(const gsh_native_command *command,
                                   char *input, size_t *input_length)
{
    size_t used = 0;
    size_t argument;
    size_t first_argument;

    if (command == NULL || input == NULL || input_length == NULL ||
        command->argc > GSH_NATIVE_ARGUMENT_CAP) {
        errno = EINVAL;
        return -1;
    }
    first_argument = command->argc > 1U &&
                             strcmp(command->argv[1], "--") == 0
                         ? 2U
                         : 1U;
    argument = first_argument;
    for (; argument < command->argc; argument++) {
        size_t length = strnlen(command->argv[argument],
                                GSH_SOURCE_INPUT_CAP + 1U);
        size_t separator = argument == first_argument ? 0U : 1U;

        if (length > GSH_SOURCE_INPUT_CAP ||
            separator + length > GSH_SOURCE_INPUT_CAP - used) {
            errno = E2BIG;
            return -1;
        }
        if (separator != 0) {
            input[used++] = ' ';
        }
        (void)memcpy(input + used, command->argv[argument], length);
        used += length;
    }
    input[used] = '\0';
    *input_length = used;
    return 0;
}

static int load_dot_source(native_evaluator *evaluator,
                           const gsh_native_command *command,
                           const gsh_variable_store *variables,
                           gsh_source_workspace *workspace,
                           size_t *input_length)
{
    if (command == NULL || workspace == NULL) return -1;
    if (evaluator == NULL || variables == NULL) {
        return -1;
    }
    size_t operand = 1U;
    int descriptor;
    int source_error = 0;
    int status;

    if (operand < command->argc &&
        strcmp(command->argv[operand], "--") == 0) {
        operand++;
    }
    if (command->argc - operand != 1U) {
        (void)fputs("gsh: .: exactly one file operand required\n", stderr);
        return 2;
    }
    descriptor = open_dot_source(
        command->argv[operand],
        store_path_value(variables, evaluator->default_path));
    if (descriptor < 0) {
        (void)fprintf(stderr, "gsh: .: %s: %s\n", command->argv[operand],
                strerror(errno));
        return 1;
    }
    if (read_source_descriptor(descriptor, workspace->input,
                               input_length) == -1) {
        source_error = errno;
    }
    status = source_error == 0 ? 0 : 1;
    if (close(descriptor) == -1 && status == 0) {
        source_error = errno;
        status = 1;
    }
    if (status != 0) {
        (void)fprintf(stderr, "gsh: .: %s: %s\n", command->argv[operand],
                strerror(source_error));
    }
    return status;
}

static int request_pipeline_exit(native_evaluator *evaluator, int status)
{
    if (!require(evaluator != NULL)) return 125;
    if (!require(status >= 0)) return 125;
    evaluator->pipeline_exit_status = status;
    return GSH_EVALUATOR_PIPELINE_EXIT_REQUEST;
}

static bool source_program_is_supported(
    native_evaluator *evaluator, gsh_source_workspace *workspace,
    const gsh_variable_store *source_variables, const char *input,
    size_t input_length, size_t root)
{
    if (evaluator == NULL || input == NULL || workspace == NULL) {
        return false;
    }
    native_evaluator preflight = *evaluator;
    gsh_variable_store *preflight_scope = &workspace->scope_base;

    if (source_variables == &workspace->scope_base) {
        preflight_scope = evaluator->scope_base;
    }
    if (source_variables == NULL || preflight_scope == NULL ||
        preflight_scope == &workspace->variables) {
        return false;
    }
    (void)memcpy(&workspace->variables, source_variables,
           sizeof(workspace->variables));
    preflight.input = input;
    preflight.input_length = input_length;
    preflight.storage = &workspace->storage;
    preflight.pipeline = &workspace->pipeline;
    preflight.variables = &workspace->variables;
    preflight.journal = NULL;
    preflight.alias_journal = NULL;
    preflight.scope_base = preflight_scope;
    preflight.scope_changes = &workspace->scope_changes;
    preflight.source_depth = evaluator->source_depth + 1U;
    preflight.preflight = true;
    preflight.fatal_error = false;
    preflight.source_request_active = false;
    if (evaluator->functions != NULL &&
        storage_has_function(&workspace->storage)) {
        gsh_functions_initialize(&workspace->function_scratch);
        if (!gsh_functions_clone(&workspace->functions,
                                 evaluator->functions)) {
            return false;
        }
        preflight.functions = &workspace->functions;
        preflight.function_scratch = &workspace->function_scratch;
    } else {
        preflight.functions = evaluator->functions;
        preflight.function_scratch = evaluator->function_scratch;
    }
    return native_preflight_node(&preflight, root, 0);
}

static int prepare_source_request(
    native_evaluator *evaluator, gsh_source_workspace *workspace,
    const gsh_variable_store *source_variables, size_t input_length,
    const char *name, const gsh_native_command *temporary_command,
    const gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP],
    size_t saved_count, bool dot)
{
    const char *parsed_input;
    size_t parsed_length = input_length;
    gsh_parse_result parsed;

    if (evaluator == NULL || workspace == NULL ||
        source_variables == NULL || name == NULL || saved == NULL ||
        input_length > GSH_SOURCE_INPUT_CAP ||
        saved_count > GSH_NATIVE_REDIRECT_CAP) {
        errno = EINVAL;
        return 125;
    }
    parsed_input = workspace->input;
    parsed = gsh_alias_parse(
        workspace->input, input_length, evaluator->aliases,
        workspace->alias_expansion, GSH_ALIAS_EXPANSION_CAP,
        &workspace->storage, &parsed_input, &parsed_length);

    if (parsed.status != GSH_PARSE_OK) {
        (void)fprintf(stderr, "gsh: %s: %s at byte %zu\n", name,
                gsh_parse_status_name(parsed.status), parsed.error_offset);
        return 2;
    }
    if (!source_program_is_supported(
            evaluator, workspace, source_variables, parsed_input,
            parsed_length, parsed.root)) {
        (void)fprintf(stderr, "gsh: %s: native source unsupported\n", name);
        return 125;
    }
    evaluator->source_request_active = true;
    evaluator->source_request_dot = dot;
    evaluator->source_request_temporary_variables =
        temporary_command != NULL;
    evaluator->source_request_workspace = workspace;
    evaluator->source_request_command = temporary_command;
    evaluator->source_request_input = parsed_input;
    evaluator->source_request_input_length = parsed_length;
    evaluator->source_request_root = parsed.root;
    evaluator->source_request_saved_count = saved_count;
    (void)memcpy(evaluator->source_request_saved, saved,
           saved_count * sizeof(saved[0]));
    return GSH_EVALUATOR_SOURCE_REQUEST;
}

static int prepare_source_variable_view(
    native_evaluator *evaluator, const gsh_native_command *command,
    gsh_source_workspace *workspace,
    const gsh_variable_store **source_variables,
    const gsh_native_command **temporary_command)
{
    if (command == NULL) return -1;
    if (evaluator == NULL || source_variables == NULL || temporary_command == NULL || workspace == NULL) {
        return -1;
    }
    int status;

    *source_variables = evaluator->variables;
    *temporary_command = NULL;
    if (!command->command_regular_context ||
        command->assignment_count == 0U) {
        return 0;
    }
    (void)memcpy(&workspace->scope_base, evaluator->variables,
           sizeof(workspace->scope_base));
    status = apply_native_assignments_with_attributes(
        &workspace->scope_base, NULL, command,
        assignment_attributes(&evaluator->options) | GSH_VARIABLE_EXPORTED);
    if (status != GSH_ASSIGNMENT_OK) {
        perror("gsh: source assignment");
        return 1;
    }
    *source_variables = &workspace->scope_base;
    *temporary_command = command;
    return 0;
}

static int load_builtin_source_text(
    native_evaluator *evaluator, const gsh_native_command *command,
    const gsh_variable_store *source_variables,
    gsh_source_workspace *workspace, size_t *input_length)
{
    if (evaluator == NULL || input_length == NULL || source_variables == NULL || workspace == NULL) {
        return -1;
    }
    if (native_dot_builtin(command)) {
        return load_dot_source(evaluator, command, source_variables,
                               workspace, input_length);
    }
    if (concatenate_eval_source(command, workspace->input,
                                input_length) == -1) {
        (void)fprintf(stderr, "gsh: eval: %s\n", strerror(errno));
        return 125;
    }
    return 0;
}

static int request_builtin_source(
    native_evaluator *evaluator, const gsh_native_command *command,
    const gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP],
    size_t saved_count)
{
    if (evaluator == NULL) return -1;
    if (command == NULL || saved == NULL) {
        return -1;
    }
    gsh_source_workspace *workspace;
    const gsh_variable_store *source_variables;
    const gsh_native_command *temporary_command;
    size_t input_length = 0;
    bool dot = native_dot_builtin(command);
    int status;

    if (!dot && command->argc == 1U) {
        return 0;
    }
    if (evaluator->source_workspaces == NULL ||
        evaluator->source_depth != gsh_source_workspaces_depth(
                                       evaluator->source_workspaces) ||
        gsh_fault_should_fail(GSH_FAULT_SOURCE_WORKSPACE_EXHAUSTION, EAGAIN)) {
        (void)fputs("gsh: nested source workspace limit exceeded\n", stderr);
        return 125;
    }
    workspace = gsh_source_workspace_acquire(evaluator->source_workspaces);
    if (workspace == NULL) {
        (void)fputs("gsh: nested source workspace limit exceeded\n", stderr);
        return 125;
    }
    status = prepare_source_variable_view(
        evaluator, command, workspace, &source_variables,
        &temporary_command);
    if (status == 0) {
        status = load_builtin_source_text(
            evaluator, command, source_variables, workspace,
            &input_length);
    }
    if (status == 0) {
        status = prepare_source_request(
            evaluator, workspace, source_variables, input_length,
            command->argv[0], temporary_command, saved, saved_count, dot);
    }
    if (status != GSH_EVALUATOR_SOURCE_REQUEST &&
        !gsh_source_workspace_release(evaluator->source_workspaces,
                                      workspace)) {
        return 125;
    }
    return status;
}

static int request_fc_source(
    native_evaluator *evaluator, const gsh_native_command *command,
    const gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP],
    size_t saved_count)
{
    if (command == NULL) return -1;
    if (evaluator == NULL || saved == NULL) {
        return -1;
    }
    gsh_source_workspace *workspace;
    const gsh_variable_store *lookup = evaluator->variables;
    gsh_fc_result result;
    int status;

    if (evaluator->source_workspaces == NULL ||
        evaluator->source_depth != gsh_source_workspaces_depth(
                                       evaluator->source_workspaces)) {
        return gsh_builtin_error(&descriptor_builtin_io, "fc",
                                 "nested source workspace limit exceeded");
    }
    workspace = gsh_source_workspace_acquire(evaluator->source_workspaces);
    if (workspace == NULL) {
        return gsh_builtin_error(&descriptor_builtin_io, "fc",
                                 "nested source workspace limit exceeded");
    }
    if (command->assignment_count != 0) {
        (void)memcpy(&workspace->scope_base, evaluator->variables,
               sizeof(workspace->scope_base));
        status = apply_native_assignments(
            &workspace->scope_base, NULL, command, &evaluator->options);
        lookup = &workspace->scope_base;
    } else {
        status = 0;
    }
    if (status == GSH_ASSIGNMENT_OK) {
        status = gsh_builtin_fc_prepare(
            command->argc, command->argv, evaluator->history, lookup,
            evaluator->history_exclude_newest, workspace->input,
            sizeof(workspace->input), &result, &descriptor_builtin_io);
    } else {
        (void)gsh_builtin_error(&descriptor_builtin_io, "fc",
                                "assignment limit exceeded");
        status = status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125 : 1;
    }
    if (status == 0 && result.execute) {
        status = prepare_source_request(
            evaluator, workspace, evaluator->variables,
            result.command_length, "fc", NULL, saved, saved_count, false);
    }
    if (status != GSH_EVALUATOR_SOURCE_REQUEST &&
        !gsh_source_workspace_release(evaluator->source_workspaces,
                                      workspace)) {
        return 125;
    }
    return status;
}

static int run_evaluator_fc_builtin(native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return -1;
    }
    const gsh_native_command *command = &evaluator_pipeline(evaluator)->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count = 0;
    int status;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: fc redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(evaluator->pipeline, command,
                                  &evaluator->options) == -1) {
        perror("gsh: fc redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    status = request_fc_source(evaluator, command, saved, saved_count);
    if (status == GSH_EVALUATOR_SOURCE_REQUEST) {
        evaluator->source_request_negated = evaluator_pipeline(evaluator)->negated;
        return status;
    }
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: fc redirection restore");
        return 125;
    }
    return status == 125 ? 125
                         : (evaluator_pipeline(evaluator)->negated
                                ? (status == 0 ? 1 : 0)
                                : status);
}

static int run_evaluator_source_builtin(native_evaluator *evaluator,
                                        bool *builtin_failed)
{
    if (builtin_failed == NULL || evaluator == NULL) {
        return -1;
    }
    const gsh_native_command *command = &evaluator_pipeline(evaluator)->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count = 0;
    int assignment_status;
    int status;

    *builtin_failed = true;
    if (evaluator->source_request_active) return 125;
    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: source redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(evaluator->pipeline, command,
                                  &evaluator->options) == -1) {
        perror("gsh: source redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    assignment_status = apply_special_builtin_assignments(
        evaluator->variables, evaluator->journal, command,
        &evaluator->options);
    if (assignment_status != GSH_ASSIGNMENT_OK) {
        perror("gsh: source assignment");
        status = assignment_status == GSH_ASSIGNMENT_JOURNAL_ERROR
                     ? 125
                     : 1;
    } else {
        status = request_builtin_source(evaluator, command, saved,
                                        saved_count);
    }
    if (status == GSH_EVALUATOR_SOURCE_REQUEST) {
        evaluator->source_request_negated = evaluator_pipeline(evaluator)->negated;
        *builtin_failed = false;
        return status;
    }
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: source redirection restore");
        return 125;
    }
    *builtin_failed = status != 0;
    return status == 125 ? 125
                         : (evaluator_pipeline(evaluator)->negated
                                ? (status == 0 ? 1 : 0)
                                : status);
}

static void close_evaluator_exec_transaction(native_evaluator *evaluator)
{
    if (evaluator == NULL) return;
    if (evaluator->exec_outcome_fd > STDERR_FILENO) {
        (void)close(evaluator->exec_outcome_fd);
    }
    if (evaluator->exec_descriptor_socket > STDERR_FILENO) {
        (void)close(evaluator->exec_descriptor_socket);
    }
    evaluator->exec_outcome_fd = -1;
    evaluator->exec_descriptor_socket = -1;
}

static gsh_command_cache *evaluator_command_cache(
    native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return NULL;
    }
    return evaluator == NULL ? NULL : evaluator->command_cache;
}

static const gsh_times_context *evaluator_times_context(
    native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return NULL;
    }
    return evaluator == NULL ? NULL : evaluator->times_context;
}

static int evaluator_last_status(const native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return -1;
    }
    return evaluator == NULL ? 0 : evaluator->last_status;
}

static gsh_background_table *evaluator_backgrounds(
    native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return NULL;
    }
    return evaluator == NULL ? NULL : evaluator->backgrounds;
}

static int evaluator_job_service_socket(
    const native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return -1;
    }
    return evaluator == NULL ? -1 : evaluator->job_service_socket;
}

static bool evaluator_job_service_available(
    const native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return false;
    }
    return evaluator != NULL && evaluator->job_service_available;
}

static bool native_preflight_node(native_evaluator *evaluator,
                                  size_t node_index, size_t depth);
static int native_evaluate_node(native_evaluator *evaluator,
                                size_t node_index, size_t depth);
static int native_evaluate_node_inner(native_evaluator *evaluator,
                                      size_t node_index, size_t depth);
static bool source_request_is_valid(const native_evaluator *evaluator);
static int abandon_source_request(native_evaluator *evaluator);
static bool enter_source_frame(native_evaluator *evaluator,
                               native_source_frame *frame, size_t *root);
static int leave_source_frame(native_evaluator *evaluator,
                              native_source_frame *frame, int status);
typedef enum {
    NATIVE_ASYNC_ERROR,
    NATIVE_ASYNC_PARENT,
    NATIVE_ASYNC_CHILD,
} native_async_start;
static native_async_start start_native_async(
    native_evaluator *evaluator, size_t node_index,
    native_evaluator *child, int *status);
static bool take_substitution_child(
    native_evaluator **evaluator, size_t *node_index, size_t *depth);

static bool async_node_has_single_pipeline(const native_evaluator *evaluator,
                                           size_t node_index)
{
    const gsh_ast_node *node;
    size_t child;

    if (node_index >= evaluator_storage(evaluator)->node_count) {
        return false;
    }
    node = &evaluator_storage(evaluator)->nodes[node_index];
    if (node->kind != GSH_AST_AND_OR || node->first_child == GSH_AST_NONE) {
        return false;
    }
    child = node->first_child;
    return evaluator_storage(evaluator)->nodes[child].kind == GSH_AST_PIPELINE &&
           evaluator_storage(evaluator)->nodes[child].next_sibling == GSH_AST_NONE;
}

static gsh_native_expansion_context native_expansion_context(
    native_evaluator *evaluator, bool execute_substitutions,
    bool *deferred_work);
static gsh_native_plan_status execute_command_substitution(
    void *opaque, const char *commands, size_t command_length, char *output,
    size_t output_capacity, size_t *output_length, int *exit_status);

static gsh_native_plan_status assign_evaluator_variable(
    native_evaluator *evaluator, const char *name, size_t name_length,
    const char *value, size_t value_length)
{
    if (evaluator == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    unsigned int attributes = assignment_attributes(&evaluator->options);
    int journal_status = 0;

    if ((!evaluator->preflight &&
         gsh_fault_should_fail(GSH_FAULT_EXPANSION_ASSIGNMENT, ENOSPC)) ||
        gsh_variables_set(evaluator->variables, name, name_length, value,
                          value_length, attributes, attributes) == -1) {
        journal_status = -1;
    } else if (evaluator->pipeline_scope != NULL) {
        journal_status = gsh_variable_journal_record_scoped(
            evaluator_pipeline_scope(evaluator)->changes,
            evaluator->expansion_variables.current_scope, name,
            name_length, value, value_length, attributes, attributes);
    } else if (evaluator->journal != NULL) {
        journal_status = gsh_variable_journal_record(
            evaluator->journal, name, name_length, value, value_length,
            attributes, attributes);
    }
    if (journal_status == 0) {
        return GSH_NATIVE_PLAN_OK;
    }
    if (!evaluator->preflight) {
        child_write_descriptor(STDERR_FILENO,
                               "gsh: parameter assignment failed\n", 33);
    }
    evaluator->fatal_error = evaluator->pipeline_scope == NULL;
    return errno == ENOSPC || errno == E2BIG ? GSH_NATIVE_PLAN_LIMIT
                                             : GSH_NATIVE_PLAN_ERROR;
}

static gsh_native_expansion_context native_expansion_context(
    native_evaluator *evaluator, bool execute_substitutions,
    bool *deferred_work)
{
    if (evaluator == NULL) {
        return (gsh_native_expansion_context){0};
    }
    size_t positional_count =
        gsh_positionals_count(evaluator->positionals);
    gsh_native_variable_state *variables =
        &evaluator->expansion_variables;

    (void)memset(variables, 0, sizeof(*variables));
    variables->mode = GSH_NATIVE_VARIABLE_LIVE;
    variables->variables = evaluator->variables;
    variables->journal = evaluator->journal;
    variables->attributes = assignment_attributes(&evaluator->options);
    variables->preflight = evaluator->preflight;
    if (!evaluator->preflight) {
        configure_expansion_assignment_fault(variables);
    }
    variables->fatal_error = &evaluator->fatal_error;
    variables->diagnostic_io = &descriptor_builtin_io;
    if (evaluator->pipeline_scope != NULL) {
        variables->scope_base = evaluator_pipeline_scope(evaluator)->base;
        variables->scope_changes = evaluator_pipeline_scope(evaluator)->changes;
        variables->command_count =
            evaluator_pipeline_scope(evaluator)->command_count;
    }
    gsh_native_substitutions_initialize(&evaluator->substitutions,
                                        execute_substitutions);
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
        .variable_state = variables,
        .substitutions = &evaluator->substitutions,
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
    if (storage == NULL) return 0U;
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

enum { SUBSTITUTION_SNAPSHOT_CAP = GSH_SOURCE_DEPTH_CAP + 2 };

typedef struct {
    gsh_variable_store variables;
    gsh_variable_journal journal;
    gsh_variable_journal scope_changes;
    gsh_native_pipeline pipeline;
    gsh_native_variable_state variable_state;
    bool fatal_error;
    bool has_journal;
    bool has_scope_changes;
    bool used;
} substitution_snapshot;

static substitution_snapshot *acquire_substitution_snapshot(void)
{
    static substitution_snapshot snapshots[SUBSTITUTION_SNAPSHOT_CAP];
    size_t index;

    for (index = 0; index < SUBSTITUTION_SNAPSHOT_CAP; index++) {
        if (!snapshots[index].used) {
            snapshots[index].used = true;
            return &snapshots[index];
        }
    }
    return NULL;
}

static void capture_substitution_snapshot(
    substitution_snapshot *snapshot, native_evaluator *evaluator,
    const gsh_native_expansion_context *context)
{
    if (context == NULL || evaluator == NULL || snapshot == NULL) {
        return;
    }
    (void)memcpy(&snapshot->variables, evaluator->variables,
           sizeof(snapshot->variables));
    (void)memcpy(&snapshot->pipeline, evaluator->pipeline,
           sizeof(snapshot->pipeline));
    snapshot->has_journal = evaluator->journal != NULL;
    if (snapshot->has_journal) {
        (void)memcpy(&snapshot->journal, evaluator->journal,
               sizeof(snapshot->journal));
    }
    snapshot->has_scope_changes =
        evaluator->pipeline_scope != NULL &&
        evaluator_pipeline_scope(evaluator)->changes != NULL;
    if (snapshot->has_scope_changes) {
        (void)memcpy(&snapshot->scope_changes,
               evaluator_pipeline_scope(evaluator)->changes,
               sizeof(snapshot->scope_changes));
    }
    snapshot->variable_state = *context->variable_state;
    snapshot->fatal_error = evaluator->fatal_error;
}

static void restore_substitution_snapshot(
    const substitution_snapshot *snapshot, native_evaluator *evaluator,
    gsh_native_expansion_context *context)
{
    if (context == NULL || evaluator == NULL || snapshot == NULL) {
        return;
    }
    (void)memcpy(evaluator->variables, &snapshot->variables,
           sizeof(snapshot->variables));
    (void)memcpy(evaluator->pipeline, &snapshot->pipeline,
           sizeof(snapshot->pipeline));
    if (snapshot->has_journal) {
        (void)memcpy(evaluator->journal, &snapshot->journal,
               sizeof(snapshot->journal));
    }
    if (snapshot->has_scope_changes) {
        (void)memcpy(evaluator_pipeline_scope(evaluator)->changes,
               &snapshot->scope_changes,
               sizeof(snapshot->scope_changes));
    }
    *context->variable_state = snapshot->variable_state;
    evaluator->fatal_error = snapshot->fatal_error;
}

static void release_substitution_snapshot(substitution_snapshot *snapshot)
{
    if (snapshot != NULL) {
        (void)memset(snapshot, 0, sizeof(*snapshot));
    }
}

typedef enum {
    EVALUATOR_EXPAND_PIPELINE,
    EVALUATOR_EXPAND_SCALAR,
    EVALUATOR_EXPAND_WORDS,
    EVALUATOR_EXPAND_REDIRECTS,
} evaluator_expansion_kind;

typedef struct {
    evaluator_expansion_kind kind;
    size_t node_index;
    gsh_word_ref word;
    const gsh_word_ref *words;
    size_t word_count;
    char **scalar;
    char **expanded;
    size_t *expanded_count;
} evaluator_expansion_request;

static gsh_native_plan_status attempt_evaluator_expansion(
    native_evaluator *evaluator, gsh_native_expansion_context *context,
    const evaluator_expansion_request *request)
{
    if (!require(evaluator != NULL && context != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(request != NULL && evaluator->pipeline != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (request->kind == EVALUATOR_EXPAND_PIPELINE) {
        return gsh_native_plan_pipeline_node_with_context(
            evaluator->input, evaluator->storage, request->node_index,
            context, evaluator->pipeline);
    }
    if (request->kind == EVALUATOR_EXPAND_SCALAR) {
        return gsh_native_expand_scalar(
            evaluator->input, request->word, context,
            evaluator->pipeline, request->scalar);
    }
    if (request->kind == EVALUATOR_EXPAND_WORDS) {
        return gsh_native_expand_words(
            evaluator->input, request->words, request->word_count,
            context, evaluator->pipeline, request->expanded,
            request->expanded_count);
    }
    return request->kind == EVALUATOR_EXPAND_REDIRECTS
               ? gsh_native_plan_redirects_with_context(
                     evaluator->input, evaluator->storage,
                     request->node_index, context, evaluator->pipeline)
               : GSH_NATIVE_PLAN_UNSUPPORTED;
}

/* ── One Tagged Expansion Engine Owns Substitution Replay ───────
 * Pipeline, scalar, word-list, and redirect expansion once duplicated the
 * same transactional replay loop.  Their copies could restore variables or
 * journals differently after a nested command failed.  A concrete request
 * tag now selects the first-party expansion operation without callbacks.
 * One bounded loop owns snapshots, substitution execution, and rollback.
 * ─────────────────────────────────────────────────────────────── */
static gsh_native_plan_status run_evaluator_expansion(
    native_evaluator *evaluator, gsh_native_expansion_context *context,
    const evaluator_expansion_request *request)
{
    if (!require(evaluator != NULL && context != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(request != NULL && context->substitutions != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    substitution_snapshot *snapshot = acquire_substitution_snapshot();
    size_t attempt;

    if (snapshot == NULL) return GSH_NATIVE_PLAN_LIMIT;
    capture_substitution_snapshot(snapshot, evaluator, context);
    for (attempt = 0U; attempt <= GSH_NATIVE_SUBSTITUTION_CAP; attempt++) {
        const char *commands;
        char *output;
        size_t command_length;
        size_t output_capacity;
        size_t output_length = 0U;
        int exit_status = 125;
        gsh_native_plan_status status;

        gsh_native_substitutions_rewind(context->substitutions);
        status = attempt_evaluator_expansion(evaluator, context, request);
        if (status != GSH_NATIVE_PLAN_DEFERRED) {
            release_substitution_snapshot(snapshot);
            return status;
        }
        commands = gsh_native_substitution_request(
            context->substitutions, &command_length);
        output = gsh_native_substitution_output(
            context->substitutions, &output_capacity);
        if (!require(commands != NULL && output != NULL)) {
            status = GSH_NATIVE_PLAN_ERROR;
        } else {
            status = execute_command_substitution(
                evaluator, commands, command_length, output,
                output_capacity, &output_length, &exit_status);
            if (status == GSH_NATIVE_PLAN_OK) {
                status = gsh_native_substitution_complete(
                    context->substitutions, output_length, exit_status);
            }
        }
        if (status != GSH_NATIVE_PLAN_OK) {
            restore_substitution_snapshot(snapshot, evaluator, context);
            release_substitution_snapshot(snapshot);
            return status;
        }
        restore_substitution_snapshot(snapshot, evaluator, context);
    }
    release_substitution_snapshot(snapshot);
    return GSH_NATIVE_PLAN_LIMIT;
}

static gsh_native_plan_status begin_evaluator_pipeline_plan(
    native_evaluator *evaluator, size_t node_index,
    bool execute_substitutions, pipeline_expansion_scope *scope,
    bool *scoped, bool *deferred_work,
    gsh_native_expansion_context *expansion)
{
    if (!require(evaluator != NULL && evaluator->storage != NULL &&
                 scope != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(scoped != NULL && expansion != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    size_t count = pipeline_command_count(evaluator->storage, node_index);

    *scoped = count > 1U;
    if (count > 1U) {
        if (evaluator->scope_base == NULL ||
            evaluator->scope_changes == NULL) {
            return GSH_NATIVE_PLAN_LIMIT;
        }
        (void)memcpy(evaluator->scope_base, evaluator->variables,
               sizeof(*evaluator->scope_base));
        gsh_variable_journal_initialize(evaluator->scope_changes, 0);
        scope->base = evaluator->scope_base;
        scope->changes = evaluator->scope_changes;
        scope->command_count = count;
    }
    evaluator->pipeline_scope = *scoped ? scope : NULL;
    if (deferred_work != NULL) {
        *deferred_work = false;
    }
    *expansion = native_expansion_context(evaluator, execute_substitutions,
                                          deferred_work);
    return GSH_NATIVE_PLAN_OK;
}

static gsh_native_plan_status finish_evaluator_pipeline_plan(
    native_evaluator *evaluator, pipeline_expansion_scope *scope,
    bool scoped, gsh_native_plan_status status)
{
    if (!require(evaluator != NULL && scope != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(evaluator->pipeline != NULL &&
                 evaluator->variables != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (status == GSH_NATIVE_PLAN_OK) {
        normalize_command_invocations(evaluator->pipeline,
                                      evaluator->functions);
    }
    evaluator->pipeline_scope = NULL;
    if (scoped) {
        (void)memcpy(evaluator->variables, scope->base,
               sizeof(*evaluator->variables));
    }
    return status;
}

/* ── Preflight Never Enters the Substitution Executor ───────────
 * The old shared planner selected execution with a boolean, but its static
 * call graph still connected preflight to the nested evaluator.  Preflight
 * already models substitutions with bounded placeholder text and records the
 * deferred mutation surface.  Giving that mode its own entry point preserves
 * the conservative proof while removing an execution edge that cannot occur.
 * ─────────────────────────────────────────────────────────────── */
static gsh_native_plan_status plan_evaluator_pipeline_preflight(
    native_evaluator *evaluator, size_t node_index,
    pipeline_expansion_scope *scope, bool *scoped, bool *deferred_work)
{
    gsh_native_expansion_context expansion;
    gsh_native_plan_status status;

    if (!require(evaluator != NULL && scope != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(scoped != NULL && deferred_work != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    *scoped = false;
    status = begin_evaluator_pipeline_plan(
        evaluator, node_index, false, scope, scoped, deferred_work,
        &expansion);
    if (status != GSH_NATIVE_PLAN_OK) return status;
    status = gsh_native_plan_pipeline_node_with_context(
        evaluator->input, evaluator->storage, node_index, &expansion,
        evaluator->pipeline);
    return finish_evaluator_pipeline_plan(evaluator, scope, *scoped,
                                          status);
}

static bool native_case_pattern(const char *input, gsh_word_ref pattern,
                                char output[GSH_NATIVE_TEXT_CAP])
{
    if (input == NULL || output == NULL) {
        return false;
    }
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
                                  const gsh_ast_node *node)
{
    if (!require(evaluator != NULL && node != NULL)) return false;
    if (!require(evaluator->storage != NULL &&
                 evaluator->pipeline != NULL)) return false;
    bool deferred_work = false;
    gsh_native_expansion_context expansion =
        native_expansion_context(evaluator, false, &deferred_work);
    char *subject;
    size_t item_index;

    if (node->word_count != 1 ||
        gsh_native_expand_scalar(
            evaluator->input,
            evaluator_storage(evaluator)->words[node->first_word], &expansion,
            evaluator->pipeline, &subject) != GSH_NATIVE_PLAN_OK) {
        return false;
    }
    (void)subject;
    item_index = node->first_child;
    while (item_index != GSH_AST_NONE) {
        const gsh_ast_node *item =
            &evaluator_storage(evaluator)->nodes[item_index];
        size_t pattern;

        if (item->kind != GSH_AST_CASE_ITEM || item->word_count == 0) {
            return false;
        }
        for (pattern = 0; pattern < item->word_count; pattern++) {
            char pattern_text[GSH_NATIVE_TEXT_CAP];

            if (!native_case_pattern(
                    evaluator->input,
                    evaluator_storage(evaluator)->words[item->first_word + pattern],
                    pattern_text)) {
                return false;
            }
        }
        item_index = item->next_sibling;
    }
    return true;
}

static bool native_preflight_for(native_evaluator *evaluator,
                                 const gsh_ast_node *node)
{
    if (!require(evaluator != NULL && node != NULL)) return false;
    if (!require(evaluator->storage != NULL &&
                 evaluator->pipeline != NULL)) return false;
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
            evaluator_storage(evaluator)->words + node->first_word + 1U,
            node->word_count - 1U, &expansion, evaluator->pipeline, items,
            &item_count) != GSH_NATIVE_PLAN_OK) {
        return false;
    }
    (void)item_count;
    return true;
}

static const gsh_function_entry *evaluator_function(
    const native_evaluator *evaluator,
    const gsh_native_command *command)
{
    if (command == NULL || evaluator == NULL) {
        return NULL;
    }
    return evaluator->functions == NULL || command->argc == 0 ||
                   command->command_suppresses_functions
               ? NULL
               : gsh_functions_lookup(evaluator->functions,
                                      command->argv[0],
                                      strlen(command->argv[0]));
}

static bool define_evaluator_function(native_evaluator *evaluator,
                                      size_t node_index)
{
    if (evaluator == NULL) {
        return false;
    }
    const gsh_ast_node *node = &evaluator_storage(evaluator)->nodes[node_index];
    gsh_word_ref name = evaluator_storage(evaluator)->words[node->first_word];

    if (evaluator->functions == NULL || node->word_count != 1U ||
        (evaluator->preflight && evaluator->storage ==
                                     &evaluator_functions(evaluator)->programs) ||
        special_builtin_name(evaluator->input + name.begin,
                             name.end - name.begin)) {
        errno = EINVAL;
        return false;
    }
    return gsh_functions_set(
               evaluator->functions, evaluator->function_scratch,
               evaluator->input, evaluator->input_length,
               evaluator->storage, node_index,
               evaluator->storage == &evaluator_functions(evaluator)->programs) == 0;
}

static bool preflight_evaluator_function(
    native_evaluator *evaluator, const gsh_native_command *command,
    const gsh_function_entry *entry, size_t depth);

static bool preflight_record_exec_descriptor(
    int descriptors[GSH_EXEC_DESCRIPTOR_COMMIT_CAP], size_t *count,
    int descriptor)
{
    size_t prior;

    if (!require(descriptors != NULL && count != NULL)) return false;
    if (!require(*count <= GSH_EXEC_DESCRIPTOR_COMMIT_CAP)) return false;
    for (prior = 0; prior < *count; prior++) {
        if (descriptors[prior] == descriptor) {
            return true;
        }
    }
    if (descriptor < 0 || *count == GSH_EXEC_DESCRIPTOR_COMMIT_CAP) {
        return false;
    }
    descriptors[(*count)++] = descriptor;
    return true;
}

static bool preflight_record_exec_descriptors(
    native_evaluator *evaluator, const gsh_native_command *command)
{
    if (!require(evaluator != NULL && command != NULL)) return false;
    if (!require(command->redirect_count <= GSH_NATIVE_REDIRECT_CAP)) {
        return false;
    }
    size_t redirect;

    for (redirect = 0; redirect < command->redirect_count; redirect++) {
        const gsh_native_redirect *entry = &command->redirects[redirect];

        if (!preflight_record_exec_descriptor(
                evaluator->exec_descriptors,
                &evaluator->exec_descriptor_count, entry->descriptor) ||
            !preflight_record_exec_descriptor(
                evaluator->exec_protected_descriptors,
                &evaluator->exec_protected_descriptor_count,
                entry->descriptor) ||
            (entry->duplicate_descriptor >= 0 &&
             !preflight_record_exec_descriptor(
                 evaluator->exec_protected_descriptors,
                 &evaluator->exec_protected_descriptor_count,
                 entry->duplicate_descriptor))) {
            return false;
        }
    }
    return true;
}

static bool preflight_planned_command(
    native_evaluator *evaluator, const pipeline_expansion_scope *scope,
    bool scoped, size_t index, size_t depth)
{
    if (!require(evaluator != NULL && scope != NULL)) return false;
    if (!require(evaluator->pipeline != NULL &&
                 index < evaluator_pipeline(evaluator)->command_count)) return false;
    const gsh_native_command *planned =
        &evaluator_pipeline(evaluator)->commands[index];
    const gsh_function_entry *function =
        evaluator_function(evaluator, planned);
    const char *path;

    if (function != NULL) {
        return preflight_evaluator_function(evaluator, planned, function,
                                            depth);
    }
    path = scoped ? scoped_command_path_value(
                        scope, (unsigned int)index + 1U, planned,
                        evaluator->default_path)
                  : command_path_value(evaluator->variables, planned,
                                       evaluator->default_path);
    return native_planned_command_is_supported(evaluator->pipeline, index,
                                               path) &&
           (!native_trap_builtin(planned) || evaluator->traps != NULL);
}

static bool record_preflight_command(native_evaluator *evaluator,
                                     const gsh_native_command *command)
{
    if (!require(evaluator != NULL && command != NULL)) return false;
    if (!require(evaluator->pipeline != NULL &&
                 command->argc <= GSH_NATIVE_ARGUMENT_CAP)) {
        return false;
    }
    bool single = evaluator_pipeline(evaluator)->command_count == 1U;

    if (native_state_builtin(command) &&
        (strcmp(command->argv[0], "shift") == 0 ||
         gsh_builtin_set_mutates_positionals(command->argc, command->argv))) {
        evaluator->positional_mutation_possible = true;
    }
    if (single && native_cd_builtin(command)) {
        evaluator->directory_mutation_possible = true;
    }
    if (single && native_alias_mutates(command)) {
        evaluator->alias_mutation_possible = true;
    }
    if (single && native_function_mutates(command)) {
        evaluator->function_mutation_possible = true;
    }
    if (single && native_exec_builtin(command)) {
        evaluator->exec_possible = true;
        if (!preflight_record_exec_descriptors(evaluator, command)) {
            return false;
        }
    }
    if (native_snapshot_job_control_builtin(command) ||
        native_wait_builtin(command)) evaluator->job_service_possible = true;
    if (single && native_source_builtin(command)) {
        evaluator->positional_mutation_possible = true;
        evaluator->directory_mutation_possible = true;
        evaluator->alias_mutation_possible = true;
        evaluator->function_mutation_possible = true;
        evaluator->command_cache_mutation_possible = true;
    }
    if (single && (native_hash_builtin(command) ||
                   native_command_inspection_builtin(command) ||
                   command_can_populate_cache(command,
                                              evaluator->functions))) {
        evaluator->command_cache_mutation_possible = true;
    }
    return true;
}

static void record_deferred_preflight(native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return;
    }
    evaluator->positional_mutation_possible = true;
    evaluator->directory_mutation_possible = true;
    evaluator->alias_mutation_possible = true;
    evaluator->function_mutation_possible = true;
    evaluator->command_cache_mutation_possible = true;
}

static bool native_preflight_pipeline(native_evaluator *evaluator,
                                      size_t node_index, size_t depth)
{
    if (!require(evaluator != NULL && evaluator->storage != NULL)) {
        return false;
    }
    if (!require(node_index < evaluator_storage(evaluator)->node_count)) return false;
    pipeline_expansion_scope scope;
    bool deferred_work;
    bool scoped;
    size_t index;

    if (plan_evaluator_pipeline_preflight(
            evaluator, node_index, &scope, &scoped,
            &deferred_work) != GSH_NATIVE_PLAN_OK) {
        return false;
    }
    if (!deferred_work) {
        for (index = 0U; index < evaluator_pipeline(evaluator)->command_count; index++) {
            if (!preflight_planned_command(evaluator, &scope, scoped, index,
                                           depth)) return false;
        }
    }
    for (index = 0U; index < evaluator_pipeline(evaluator)->command_count; index++) {
        if (!record_preflight_command(
                evaluator, &evaluator_pipeline(evaluator)->commands[index])) {
            return false;
        }
    }
    if (deferred_work) record_deferred_preflight(evaluator);
    return true;
}

typedef struct {
    size_t node_index;
    size_t next_child;
    size_t depth;
    size_t visited;
    size_t saved_exec_descriptor_count;
    size_t saved_exec_protected_descriptor_count;
    bool saved_exec_possible;
    bool isolated_exec;
    bool initialized;
} preflight_walk_frame;

enum {
    PREFLIGHT_WALK_DEPTH_CAP = 129,
    PREFLIGHT_WORKSPACE_CAP = GSH_FUNCTION_DEPTH_CAP + GSH_SOURCE_DEPTH_CAP + 2,
};

typedef struct {
    preflight_walk_frame frames[PREFLIGHT_WALK_DEPTH_CAP];
    bool used;
} preflight_workspace;

static preflight_workspace *acquire_preflight_workspace(void)
{
    static preflight_workspace workspaces[PREFLIGHT_WORKSPACE_CAP];
    size_t index;

    for (index = 0U; index < PREFLIGHT_WORKSPACE_CAP; index++) {
        if (!workspaces[index].used) {
            workspaces[index].used = true;
            return &workspaces[index];
        }
    }
    return NULL;
}

static void release_preflight_workspace(preflight_workspace *workspace)
{
    if (workspace != NULL) workspace->used = false;
}

static bool preflight_walk_kind(gsh_ast_kind kind)
{
    if (!require(kind >= GSH_AST_PROGRAM && kind <= GSH_AST_FUNCTION)) {
        return false;
    }
    return kind == GSH_AST_PROGRAM || kind == GSH_AST_LIST ||
           kind == GSH_AST_AND_OR || kind == GSH_AST_PIPELINE ||
           kind == GSH_AST_SUBSHELL || kind == GSH_AST_BRACE_GROUP ||
           kind == GSH_AST_IF || kind == GSH_AST_IF_BRANCH ||
           kind == GSH_AST_WHILE || kind == GSH_AST_UNTIL ||
           kind == GSH_AST_FOR || kind == GSH_AST_CASE ||
           kind == GSH_AST_CASE_ITEM;
}

static void restore_preflight_isolation(native_evaluator *evaluator,
                                        const preflight_walk_frame *frame)
{
    if (frame == NULL) return;
    if (evaluator == NULL) {
        return;
    }
    if (!frame->isolated_exec) return;
    evaluator->exec_possible = frame->saved_exec_possible;
    evaluator->exec_descriptor_count = frame->saved_exec_descriptor_count;
    evaluator->exec_protected_descriptor_count =
        frame->saved_exec_protected_descriptor_count;
}

typedef enum {
    PREFLIGHT_FRAME_INVALID,
    PREFLIGHT_FRAME_PIPELINE,
    PREFLIGHT_FRAME_FUNCTION,
    PREFLIGHT_FRAME_READY,
} preflight_frame_action;

static preflight_frame_action classify_preflight_frame(
    native_evaluator *evaluator, preflight_walk_frame *frame)
{
    if (!require(evaluator != NULL && frame != NULL)) {
        return PREFLIGHT_FRAME_INVALID;
    }
    if (!require(evaluator->storage != NULL)) return PREFLIGHT_FRAME_INVALID;
    const gsh_ast_node *node;
    const gsh_ast_node *command;

    if (frame->depth > 128U || frame->node_index == GSH_AST_NONE ||
        frame->node_index >= evaluator_storage(evaluator)->node_count) {
        return PREFLIGHT_FRAME_INVALID;
    }
    node = &evaluator_storage(evaluator)->nodes[frame->node_index];
    if (node->redirect_count != 0U && node->kind != GSH_AST_FUNCTION) {
        return PREFLIGHT_FRAME_INVALID;
    }
    if (node->kind == GSH_AST_PIPELINE) {
        if (node->first_child == GSH_AST_NONE) {
            return PREFLIGHT_FRAME_INVALID;
        }
        command = &evaluator_storage(evaluator)->nodes[node->first_child];
        if (command->next_sibling != GSH_AST_NONE ||
            command->kind == GSH_AST_SIMPLE) {
            return PREFLIGHT_FRAME_PIPELINE;
        }
    }
    if (node->kind == GSH_AST_CASE && !native_preflight_case(evaluator, node)) {
        return PREFLIGHT_FRAME_INVALID;
    }
    if (node->kind == GSH_AST_FOR && !native_preflight_for(evaluator, node)) {
        return PREFLIGHT_FRAME_INVALID;
    }
    if (node->kind == GSH_AST_FUNCTION) {
        return PREFLIGHT_FRAME_FUNCTION;
    }
    if (!preflight_walk_kind(node->kind) ||
        (node->kind != GSH_AST_PROGRAM && node->kind != GSH_AST_CASE_ITEM &&
         node->first_child == GSH_AST_NONE)) {
        return PREFLIGHT_FRAME_INVALID;
    }
    frame->next_child = node->first_child;
    frame->isolated_exec = node->kind == GSH_AST_SUBSHELL ||
                           (node->flags & GSH_AST_FLAG_ASYNC) != 0U;
    frame->saved_exec_possible = evaluator->exec_possible;
    frame->saved_exec_descriptor_count = evaluator->exec_descriptor_count;
    frame->saved_exec_protected_descriptor_count =
        evaluator->exec_protected_descriptor_count;
    frame->initialized = true;
    return PREFLIGHT_FRAME_READY;
}

/* ── Preflight Traversal Has One Explicit Owner Stack ───────────
 * AST validation used to recurse once for every compound node and again for
 * case and for bodies.  A fixed DFS workspace now owns suspension and the
 * subshell exec-state snapshot.  Function bodies may start a nested preflight,
 * so workspaces come from a bounded static pool rather than the C stack.
 * ─────────────────────────────────────────────────────────────── */
static bool native_preflight_node(native_evaluator *evaluator,
                                  size_t node_index, size_t depth)
{
    if (!require(evaluator != NULL && evaluator->storage != NULL)) {
        return false;
    }
    if (!require(depth <= 128U && node_index != GSH_AST_NONE)) return false;
    preflight_workspace *workspace = acquire_preflight_workspace();
    size_t stack_depth = 1U;
    bool supported = workspace != NULL;

    if (!supported) return false;
    (void)memset(workspace->frames, 0, sizeof(workspace->frames));
    workspace->frames[0].node_index = node_index;
    workspace->frames[0].depth = depth;
    while (supported && stack_depth > 0U) {
        preflight_walk_frame *frame = &workspace->frames[stack_depth - 1U];
        size_t child;

        if (!frame->initialized) {
            preflight_frame_action action =
                classify_preflight_frame(evaluator, frame);

            if (action == PREFLIGHT_FRAME_PIPELINE) {
                supported = native_preflight_pipeline(
                    evaluator, frame->node_index, frame->depth);
            } else if (action == PREFLIGHT_FRAME_FUNCTION) {
                evaluator->function_mutation_possible = true;
                supported = define_evaluator_function(
                    evaluator, frame->node_index);
            } else if (action == PREFLIGHT_FRAME_INVALID) {
                supported = false;
            }
            if (!supported) break;
            if (action != PREFLIGHT_FRAME_READY) {
                stack_depth--;
                continue;
            }
        }
        if (frame->next_child == GSH_AST_NONE) {
            restore_preflight_isolation(evaluator, frame);
            stack_depth--;
            continue;
        }
        child = frame->next_child;
        if (child >= evaluator_storage(evaluator)->node_count ||
            ++frame->visited > evaluator_storage(evaluator)->node_count ||
            stack_depth == PREFLIGHT_WALK_DEPTH_CAP) {
            supported = false;
            break;
        }
        frame->next_child = evaluator_storage(evaluator)->nodes[child].next_sibling;
        (void)memset(&workspace->frames[stack_depth], 0,
               sizeof(workspace->frames[stack_depth]));
        workspace->frames[stack_depth].node_index = child;
        workspace->frames[stack_depth].depth =
            frame->depth + (evaluator_storage(evaluator)->nodes[child].kind ==
                                    GSH_AST_CASE_ITEM
                                ? 0U
                                : 1U);
        stack_depth++;
    }
    release_preflight_workspace(workspace);
    return supported;
}

typedef struct {
    uint16_t entry_index;
    uint16_t depth;
    gsh_positional_store positionals;
} function_preflight_task;

typedef struct {
    function_preflight_task tasks[GSH_FUNCTION_CAP];
    bool queued[GSH_FUNCTION_CAP];
    size_t count;
    size_t next;
    bool used;
} function_preflight_workspace;

static function_preflight_workspace *acquire_function_preflight_workspace(void)
{
    static function_preflight_workspace workspace;

    if (!require(workspace.count <= GSH_FUNCTION_CAP)) return NULL;
    if (!require(workspace.next <= workspace.count)) return NULL;
    if (workspace.used) return NULL;
    (void)memset(workspace.queued, 0, sizeof(workspace.queued));
    workspace.count = 0U;
    workspace.next = 0U;
    workspace.used = true;
    return &workspace;
}

static void release_function_preflight_workspace(
    function_preflight_workspace *workspace)
{
    if (!require(workspace != NULL)) return;
    if (!require(workspace->count <= GSH_FUNCTION_CAP)) return;
    workspace->count = 0U;
    workspace->next = 0U;
    workspace->used = false;
}

static bool queue_function_preflight(
    native_evaluator *evaluator, const gsh_native_command *command,
    const gsh_function_entry *entry, size_t depth,
    function_preflight_workspace *workspace)
{
    if (!require(evaluator != NULL && command != NULL && entry != NULL)) {
        return false;
    }
    if (!require(workspace != NULL && evaluator->functions != NULL)) {
        return false;
    }
    size_t index = (size_t)(entry - evaluator_functions(evaluator)->entries);
    function_preflight_task *task;

    if (depth > GSH_FUNCTION_DEPTH_CAP || index >= GSH_FUNCTION_CAP) {
        return false;
    }
    if (workspace->queued[index]) return true;
    if (workspace->count == GSH_FUNCTION_CAP ||
        apply_native_assignments(evaluator->variables, NULL, command,
                                 &evaluator->options) !=
            GSH_ASSIGNMENT_OK) {
        return false;
    }
    task = &workspace->tasks[workspace->count];
    if (gsh_positionals_assign(&task->positionals, command->argc - 1U,
                               command->argv + 1U) == -1) {
        return false;
    }
    task->entry_index = (uint16_t)index;
    task->depth = (uint16_t)depth;
    workspace->queued[index] = true;
    workspace->count++;
    return true;
}

static bool preflight_function_command(
    native_evaluator *evaluator, const pipeline_expansion_scope *scope,
    bool scoped, size_t command_index, size_t depth,
    function_preflight_workspace *workspace)
{
    if (!require(evaluator != NULL && scope != NULL)) return false;
    if (!require(evaluator->pipeline != NULL && workspace != NULL)) {
        return false;
    }
    const gsh_native_command *command =
        &evaluator_pipeline(evaluator)->commands[command_index];
    const gsh_function_entry *function =
        evaluator_function(evaluator, command);
    const char *path;

    if (function != NULL) {
        return queue_function_preflight(
            evaluator, command, function, depth + 1U, workspace);
    }
    path = scoped ? scoped_command_path_value(
                        scope, (unsigned int)command_index + 1U, command,
                        evaluator->default_path)
                  : command_path_value(evaluator->variables, command,
                                       evaluator->default_path);
    return native_planned_command_is_supported(
               evaluator->pipeline, command_index, path) &&
           (!native_trap_builtin(command) || evaluator->traps != NULL);
}

static bool preflight_function_pipeline(
    native_evaluator *evaluator, size_t node_index, size_t depth,
    function_preflight_workspace *workspace)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(evaluator->storage != NULL &&
                 node_index < evaluator_storage(evaluator)->node_count)) return false;
    pipeline_expansion_scope scope;
    bool deferred_work;
    bool scoped;
    size_t index;

    if (plan_evaluator_pipeline_preflight(
            evaluator, node_index, &scope, &scoped,
            &deferred_work) != GSH_NATIVE_PLAN_OK) {
        return false;
    }
    if (!deferred_work) {
        for (index = 0U; index < evaluator_pipeline(evaluator)->command_count;
             index++) {
            if (!preflight_function_command(
                    evaluator, &scope, scoped, index, depth, workspace)) {
                return false;
            }
        }
    }
    for (index = 0U; index < evaluator_pipeline(evaluator)->command_count; index++) {
        if (!record_preflight_command(
                evaluator, &evaluator_pipeline(evaluator)->commands[index])) {
            return false;
        }
    }
    if (deferred_work) record_deferred_preflight(evaluator);
    return true;
}

static bool preflight_function_node(
    native_evaluator *evaluator, size_t node_index, size_t depth,
    function_preflight_workspace *functions)
{
    if (!require(evaluator != NULL && evaluator->storage != NULL)) {
        return false;
    }
    if (!require(functions != NULL && node_index != GSH_AST_NONE)) {
        return false;
    }
    preflight_workspace *workspace = acquire_preflight_workspace();
    size_t stack_depth = 1U;
    bool supported = workspace != NULL;

    if (!supported) return false;
    (void)memset(workspace->frames, 0, sizeof(workspace->frames));
    workspace->frames[0].node_index = node_index;
    workspace->frames[0].depth = depth;
    while (supported && stack_depth > 0U) {
        preflight_walk_frame *frame = &workspace->frames[stack_depth - 1U];
        size_t child;

        if (!frame->initialized) {
            preflight_frame_action action =
                classify_preflight_frame(evaluator, frame);

            if (action == PREFLIGHT_FRAME_PIPELINE) {
                supported = preflight_function_pipeline(
                    evaluator, frame->node_index, frame->depth, functions);
            } else if (action != PREFLIGHT_FRAME_READY) {
                supported = false;
            }
            if (!supported) break;
            if (action == PREFLIGHT_FRAME_PIPELINE) {
                stack_depth--;
                continue;
            }
        }
        if (frame->next_child == GSH_AST_NONE) {
            restore_preflight_isolation(evaluator, frame);
            stack_depth--;
            continue;
        }
        child = frame->next_child;
        if (child >= evaluator_storage(evaluator)->node_count ||
            ++frame->visited > evaluator_storage(evaluator)->node_count ||
            stack_depth == PREFLIGHT_WALK_DEPTH_CAP) {
            supported = false;
            break;
        }
        frame->next_child = evaluator_storage(evaluator)->nodes[child].next_sibling;
        (void)memset(&workspace->frames[stack_depth], 0,
               sizeof(workspace->frames[stack_depth]));
        workspace->frames[stack_depth].node_index = child;
        workspace->frames[stack_depth].depth = frame->depth + 1U;
        stack_depth++;
    }
    release_preflight_workspace(workspace);
    return supported;
}

/* ── Function Preflight Uses a Reachability Worklist ────────────
 * Re-entering the AST preflight for every function call put the verifier in
 * the same recursive component as execution.  A fixed worklist now records
 * each reachable function once, including its bounded positional arguments.
 * Bodies use an independent DFS and enqueue nested calls without using the C
 * stack; exec descriptors and mutation flags still flow into the caller.
 * ─────────────────────────────────────────────────────────────── */
static bool preflight_evaluator_function(
    native_evaluator *evaluator, const gsh_native_command *command,
    const gsh_function_entry *entry, size_t depth)
{
    if (!require(evaluator != NULL && command != NULL)) return false;
    if (!require(entry != NULL && evaluator->functions != NULL)) return false;
    const char *saved_input = evaluator->input;
    size_t saved_input_length = evaluator->input_length;
    const gsh_parse_storage *saved_storage = evaluator->storage;
    gsh_positional_store *saved_positionals = evaluator->positionals;
    size_t saved_function_depth = evaluator->function_depth;
    function_preflight_workspace *workspace =
        acquire_function_preflight_workspace();
    bool supported = workspace != NULL;

    if (depth > 128U) supported = false;
    if (supported) {
        supported = queue_function_preflight(
            evaluator, command, entry, saved_function_depth + 1U,
            workspace);
    }
    evaluator->input = gsh_functions_text(evaluator->functions);
    evaluator->input_length = evaluator_functions(evaluator)->text_used;
    evaluator->storage = &evaluator_functions(evaluator)->programs;
    while (supported && workspace->next < workspace->count) {
        function_preflight_task *task =
            &workspace->tasks[workspace->next++];
        const gsh_function_entry *selected =
            &evaluator_functions(evaluator)->entries[task->entry_index];
        const gsh_ast_node *definition =
            &evaluator_storage(evaluator)->nodes[selected->node_offset];

        evaluator->positionals = &task->positionals;
        evaluator->function_depth = task->depth;
        supported = definition->kind == GSH_AST_FUNCTION &&
                    definition->first_child != GSH_AST_NONE;
        if (supported && definition->redirect_count != 0U) {
            bool deferred_work = false;
            gsh_native_expansion_context expansion =
                native_expansion_context(evaluator, false,
                                         &deferred_work);

            supported = gsh_native_plan_redirects_with_context(
                            evaluator->input, evaluator->storage,
                            selected->node_offset, &expansion,
                            evaluator->pipeline) == GSH_NATIVE_PLAN_OK &&
                        !deferred_work;
        }
        if (supported) {
            supported = preflight_function_node(
                evaluator, definition->first_child, 0U, workspace);
        }
    }
    evaluator->function_depth = saved_function_depth;
    evaluator->positionals = saved_positionals;
    evaluator->storage = saved_storage;
    evaluator->input_length = saved_input_length;
    evaluator->input = saved_input;
    if (workspace != NULL) release_function_preflight_workspace(workspace);
    return supported;
}

typedef struct {
    size_t used;
    int error;
    bool overflow;
    bool contains_null;
    bool failed;
} substitution_capture_result;

static void initialize_substitution_functions(
    native_evaluator *parent, gsh_source_workspace *workspace,
    native_evaluator *nested)
{
    if (workspace == NULL) return;
    if (nested == NULL || parent == NULL) {
        return;
    }
    if (!storage_has_function(&workspace->storage)) {
        nested->functions = parent->functions;
        nested->function_scratch = parent->function_scratch;
        return;
    }
    if (parent->functions == NULL) {
        gsh_functions_initialize(&workspace->functions);
    } else {
        (void)memcpy(&workspace->functions, parent->functions,
               sizeof(workspace->functions));
    }
    gsh_functions_initialize(&workspace->function_scratch);
    nested->functions = &workspace->functions;
    nested->function_scratch = &workspace->function_scratch;
}

static void initialize_substitution_evaluator(
    native_evaluator *parent, gsh_source_workspace *workspace,
    const char *input, size_t input_length, native_evaluator *nested,
    gsh_background_table *backgrounds)
{
    if (backgrounds == NULL || input == NULL || nested == NULL || parent == NULL || workspace == NULL) {
        return;
    }
    (void)memset(nested, 0, sizeof(*nested));
    nested->exec_outcome_fd = parent->exec_outcome_fd;
    nested->exec_descriptor_socket = parent->exec_descriptor_socket;
    nested->input = input;
    nested->input_length = input_length;
    nested->storage = &workspace->storage;
    nested->pipeline = &workspace->pipeline;
    nested->default_path = parent->default_path;
    nested->last_status = parent->last_status;
    nested->shell_pid = parent->shell_pid;
    nested->parameter_zero = parent->parameter_zero;
    nested->history = parent->history;
    nested->history_exclude_newest = parent->history_exclude_newest;
    nested->positionals = parent->positionals;
    nested->options = parent->options;
    (void)memcpy(&workspace->variables, parent->variables,
           sizeof(workspace->variables));
    nested->variables = &workspace->variables;
    nested->aliases = parent->aliases;
    nested->command_cache = parent->command_cache;
    nested->command_cache_base_generation =
        parent->command_cache_base_generation;
    nested->scope_base = &workspace->scope_base;
    nested->scope_changes = &workspace->scope_changes;
    nested->source_workspaces = parent->source_workspaces;
    nested->traps = parent->traps;
    nested->source_depth = parent->source_depth + 1U;
    nested->preflight = true;
    gsh_background_initialize(backgrounds);
    nested->backgrounds = backgrounds;
    initialize_substitution_functions(parent, workspace, nested);
}

static gsh_native_plan_status prepare_substitution(
    native_evaluator *parent, const char *commands, size_t command_length,
    gsh_source_workspace *workspace, native_evaluator *nested,
    gsh_background_table *backgrounds, gsh_parse_result *parsed)
{
    if (backgrounds == NULL || commands == NULL || nested == NULL || parent == NULL || parsed == NULL || workspace == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    const char *input = commands;
    size_t input_length = command_length;

    *parsed = gsh_alias_parse(
        commands, command_length, parent->aliases,
        workspace->alias_expansion, GSH_ALIAS_EXPANSION_CAP,
        &workspace->storage, &input, &input_length);
    if (parsed->status != GSH_PARSE_OK) {
        return parsed->status == GSH_PARSE_LIMIT ? GSH_NATIVE_PLAN_LIMIT
                                                 : GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    initialize_substitution_evaluator(parent, workspace, input, input_length,
                                      nested, backgrounds);
    if (!native_preflight_node(nested, parsed->root, 0)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    (void)memcpy(&workspace->variables, parent->variables,
           sizeof(workspace->variables));
    nested->preflight = false;
    nested->fatal_error = false;
    return GSH_NATIVE_PLAN_OK;
}

static substitution_capture_result read_substitution_output(
    int descriptor, char *output, size_t output_capacity)
{
    enum { READ_RETRY_CAP = 64 };
    substitution_capture_result result = {0};
    size_t attempt;
    bool complete = false;

    if (descriptor < 0 || output == NULL) {
        result.failed = true;
        result.error = EINVAL;
        return result;
    }

    for (attempt = 0;
         attempt < GSH_NATIVE_HEREDOC_TEXT_CAP + READ_RETRY_CAP &&
         !complete;
         attempt++) {
        unsigned char bytes[4096];
        ssize_t count = gsh_fault_should_fail(GSH_FAULT_SUBSTITUTION_READ, EIO)
                            ? -1
                            : read(descriptor, bytes, sizeof(bytes));

        if (count > 0) {
            size_t amount = (size_t)count;

            result.contains_null = memchr(bytes, '\0', amount) != NULL;
            result.overflow = amount > output_capacity - result.used;
            if (result.contains_null || result.overflow) {
                complete = true;
            } else {
                (void)memcpy(output + result.used, bytes, amount);
                result.used += amount;
            }
        } else if (count == 0) {
            complete = true;
        } else if (errno != EINTR) {
            result.failed = true;
            result.error = errno;
            complete = true;
        }
    }
    if (!complete) {
        result.failed = true;
        result.error = EINTR;
    }
    return result;
}

static bool wait_substitution_child(pid_t pid, int *wait_status)
{
    enum { WAIT_RETRY_CAP = 1024 };
    unsigned int attempt;

    if (pid <= 0 || wait_status == NULL) {
        errno = EINVAL;
        return false;
    }

    for (attempt = 0; attempt < WAIT_RETRY_CAP; attempt++) {
        pid_t waited = waitpid(pid, wait_status, 0);

        if (waited == pid) {
            return true;
        }
        if (waited == -1 && errno == EINTR) {
            continue;
        }
        return false;
    }
    errno = EINTR;
    return false;
}

static gsh_native_plan_status finish_substitution_capture(
    pid_t pid, int descriptor, char *output, size_t output_capacity,
    size_t *output_length, int *exit_status)
{
    if (!require(pid > 0 && descriptor >= 0)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(output != NULL && output_length != NULL &&
                 exit_status != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    int wait_status = 0;
    substitution_capture_result result = read_substitution_output(
        descriptor, output, output_capacity);
    size_t trim;

    (void)close(descriptor);
    if (result.overflow || result.contains_null || result.failed) {
        (void)kill(pid, SIGKILL);
    }
    if (!wait_substitution_child(pid, &wait_status)) {
        result.failed = true;
        result.error = errno;
    }
    if (result.overflow) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    if (result.contains_null || result.failed) {
        if (result.failed) {
            errno = result.error != 0 ? result.error : EIO;
            perror("gsh: command substitution read");
        } else {
            (void)fputs("gsh: command substitution contains a null byte\n",
                  stderr);
        }
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    for (trim = 0; trim < result.used &&
                   output[result.used - 1U] == '\n'; trim++) {
        result.used--;
    }
    *output_length = result.used;
    *exit_status = wait_status_value(wait_status);
    return GSH_NATIVE_PLAN_OK;
}

typedef struct {
    gsh_source_workspace *workspace;
    native_evaluator nested;
    gsh_background_table *backgrounds;
    size_t root;
    bool used;
} command_substitution_execution;

static command_substitution_execution *acquire_substitution_execution(void)
{
    static command_substitution_execution
        executions[SUBSTITUTION_SNAPSHOT_CAP];
    size_t index;

    for (index = 0U; index < SUBSTITUTION_SNAPSHOT_CAP; index++) {
        if (!executions[index].used) {
            (void)memset(&executions[index], 0, sizeof(executions[index]));
            executions[index].used = true;
            return &executions[index];
        }
    }
    errno = ENOSPC;
    return NULL;
}

static void release_substitution_execution(
    command_substitution_execution *execution)
{
    if (!require(execution != NULL)) return;
    if (!require(execution->used)) return;
    (void)memset(execution, 0, sizeof(*execution));
}

static gsh_native_plan_status prepare_command_substitution(
    native_evaluator *parent, const char *commands, size_t command_length,
    char *output, size_t output_capacity, size_t *output_length,
    int *exit_status, command_substitution_execution *execution,
    bool *ready)
{
    if (!require(parent != NULL && commands != NULL && output != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(output_length != NULL && exit_status != NULL &&
                 execution != NULL && ready != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    gsh_parse_result parsed = {0};
    gsh_native_plan_status status;

    *ready = false;
    *output_length = 0U;
    *exit_status = 125;
    if (output_capacity > GSH_NATIVE_HEREDOC_TEXT_CAP ||
        parent->source_workspaces == NULL ||
        parent->source_depth !=
            gsh_source_workspaces_depth(parent->source_workspaces) ||
        gsh_fault_should_fail(GSH_FAULT_SOURCE_WORKSPACE_EXHAUSTION, EAGAIN)) {
        (void)fputs("gsh: nested source workspace limit exceeded\n", stderr);
        return GSH_NATIVE_PLAN_LIMIT;
    }
    execution->workspace =
        gsh_source_workspace_acquire(parent->source_workspaces);
    if (execution->workspace == NULL) {
        (void)fputs("gsh: nested source workspace limit exceeded\n", stderr);
        return GSH_NATIVE_PLAN_LIMIT;
    }
    execution->backgrounds = allocate_isolated_job_table();
    if (execution->backgrounds == NULL) {
        (void)fputs("gsh: command substitution job state unavailable\n", stderr);
        return GSH_NATIVE_PLAN_LIMIT;
    }
    status = prepare_substitution(
        parent, commands, command_length, execution->workspace,
        &execution->nested, execution->backgrounds, &parsed);
    execution->root = parsed.root;
    *ready = status == GSH_NATIVE_PLAN_OK;
    return status;
}

static gsh_native_plan_status finish_command_substitution(
    native_evaluator *parent, command_substitution_execution *execution,
    gsh_native_plan_status status)
{
    if (!require(parent != NULL && execution != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    bool released;

    if (execution->backgrounds != NULL) {
        release_isolated_job_table(execution->backgrounds);
    }
    if (execution->workspace == NULL) {
        if (execution->backgrounds != NULL ||
            status == GSH_NATIVE_PLAN_OK) {
            status = GSH_NATIVE_PLAN_UNSUPPORTED;
        }
        release_substitution_execution(execution);
        return status;
    }
    released = gsh_source_workspace_release(
        parent->source_workspaces, execution->workspace);
    if (!released) {
        (void)fputs("gsh: nested source workspace ownership failure\n", stderr);
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (parent->source_depth !=
        gsh_source_workspaces_depth(parent->source_workspaces)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    release_substitution_execution(execution);
    return status;
}

static gsh_native_plan_status execute_command_substitution(
    void *opaque, const char *commands, size_t command_length, char *output,
    size_t output_capacity, size_t *output_length, int *exit_status)
{
    if (opaque == NULL) {
        return GSH_NATIVE_PLAN_LIMIT;
    }
    native_evaluator *parent = opaque;
    command_substitution_execution *execution;
    int capture[2] = {-1, -1};
    pid_t pid;
    gsh_native_plan_status status;
    bool ready = false;

    if (!require(parent != NULL && commands != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    if (!require(output != NULL && output_length != NULL &&
                 exit_status != NULL)) {
        return GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    execution = acquire_substitution_execution();
    if (execution == NULL) return GSH_NATIVE_PLAN_LIMIT;
    status = prepare_command_substitution(
        parent, commands, command_length, output, output_capacity,
        output_length, exit_status, execution, &ready);
    if (!ready) {
        return finish_command_substitution(parent, execution, status);
    }
    if (make_pipe(capture, false, GSH_FAULT_SUBSTITUTION_PIPE) == -1) {
        perror("gsh: command substitution pipe");
        status = GSH_NATIVE_PLAN_UNSUPPORTED;
    } else {
        pid = gsh_fault_should_fail(GSH_FAULT_SUBSTITUTION_FORK, EAGAIN) ? -1 : fork();
        if (pid == 0) {
            (void)close(capture[0]);
            reset_child_signals();
            enter_native_subshell_or_exit(&execution->nested);
            close_evaluator_exec_transaction(&execution->nested);
            if (child_duplicate_descriptor(capture[1], STDOUT_FILENO) == -1) {
                child_exec_error("command substitution output", errno);
            }
            if (capture[1] != STDOUT_FILENO) (void)close(capture[1]);
            parent->substitution_child_execution = execution;
            return GSH_NATIVE_PLAN_DEFERRED;
        }
        (void)close(capture[1]);
        if (pid == -1) {
            perror("gsh: command substitution fork");
            (void)close(capture[0]);
            status = GSH_NATIVE_PLAN_UNSUPPORTED;
        } else {
            status = pid > 0 && capture[0] >= 0
                         ? finish_substitution_capture(
                               pid, capture[0], output, output_capacity,
                               output_length, exit_status)
                         : GSH_NATIVE_PLAN_UNSUPPORTED;
        }
    }
    if (*output_length > output_capacity) {
        status = GSH_NATIVE_PLAN_UNSUPPORTED;
    }
    return finish_command_substitution(parent, execution, status);
}

/* ── Exit Propagates to the Current Execution-Environment Boundary ──
 * A return belongs to one function or dot frame; exit belongs to the shell
 * environment containing all of those frames.  Recording it separately lets
 * ordinary bounded unwinding restore redirects and source slots before the
 * top-level process terminates.  Forked evaluators inherit their own record,
 * naturally confining exit to subshells, substitutions, pipelines, and jobs.
 * ─────────────────────────────────────────────────────────────── */
static int evaluate_exit(native_evaluator *evaluator,
                         const gsh_native_command *command)
{
    if (evaluator == NULL) return -1;
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count = 0;
    int status = 125;
    bool valid = false;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: exit redirection save");
    } else if (apply_evaluator_redirects(
                   evaluator->pipeline, command,
                   &evaluator->options) == -1) {
        perror("gsh: exit redirection");
        status = 1;
    } else {
        int assignment = apply_special_builtin_assignments(
            evaluator->variables, evaluator->journal, command,
            &evaluator->options);

        if (assignment != GSH_ASSIGNMENT_OK) {
            perror("gsh: exit assignment");
            status = assignment == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125 : 1;
        } else {
            valid = parse_exit_status(command, evaluator->last_status,
                                      &status);
        }
    }
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: exit redirection restore");
        status = 125;
        valid = false;
    }
    if (valid) {
        evaluator->exiting = true;
        evaluator->exit_status = status;
    } else if (!command->command_regular_context) {
        evaluator->fatal_error = true;
    }
    return status;
}

static int evaluate_return(native_evaluator *evaluator,
                           const gsh_native_command *command)
{
    if (command == NULL) return -1;
    if (evaluator == NULL) {
        return -1;
    }
    unsigned int status = (unsigned int)(evaluator->last_status & 255);
    const char *cursor;

    if (command->argc > 2U ||
        (command->assignment_count != 0 &&
         !command->command_regular_context) ||
        command->redirect_count != 0 ||
        (evaluator->function_depth == 0 && evaluator->dot_depth == 0)) {
        (void)fputs("gsh: return: invalid context or operands\n", stderr);
        return 1;
    }
    if (command->argc == 2U) {
        status = 0;
        cursor = command->argv[1];
        if (*cursor == '\0') {
            (void)fputs("gsh: return: invalid status\n", stderr);
            return 1;
        }
        while (*cursor != '\0') {
            if (*cursor < '0' || *cursor > '9' || status > 25U) {
                (void)fputs("gsh: return: invalid status\n", stderr);
                return 1;
            }
            status = status * 10U + (unsigned int)(*cursor++ - '0');
        }
        if (status > 255U) {
            (void)fputs("gsh: return: invalid status\n", stderr);
            return 1;
        }
    }
    evaluator->returning = true;
    evaluator->return_status = (int)status;
    return (int)status;
}

static bool parse_loop_control_count(const char *text, size_t limit,
                                     size_t *result)
{
    if (result == NULL || text == NULL) {
        return false;
    }
    size_t length = strnlen(text, GSH_NATIVE_TEXT_CAP);
    size_t value = 0;
    size_t index;
    bool positive = false;

    if (length == 0 || length == GSH_NATIVE_TEXT_CAP || limit == 0) {
        return false;
    }
    for (index = 0; index < length; index++) {
        size_t digit;

        if (text[index] < '0' || text[index] > '9') {
            return false;
        }
        digit = (size_t)(text[index] - '0');
        positive = positive || digit != 0;
        if (value < limit) {
            value = digit > limit || value > (limit - digit) / 10U
                        ? limit
                        : value * 10U + digit;
        }
    }
    *result = value;
    return positive;
}

static int evaluate_loop_control(native_evaluator *evaluator,
                                 const gsh_native_pipeline *pipeline)
{
    if (evaluator == NULL) return -1;
    if (pipeline == NULL) {
        return -1;
    }
    const gsh_native_command *command = &pipeline->commands[0];
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    size_t count = 1U;
    int status;

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: loop control redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(pipeline, command,
                                  &evaluator->options) == -1) {
        perror("gsh: loop control redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    status = apply_special_builtin_assignments(
        evaluator->variables, evaluator->journal, command,
        &evaluator->options);
    if (status != GSH_ASSIGNMENT_OK) {
        perror("gsh: loop control assignment");
        status = status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125 : 1;
    } else if (evaluator->active_loops == 0 || command->argc > 2U ||
               (command->argc == 2U &&
                !parse_loop_control_count(command->argv[1],
                                          evaluator->active_loops,
                                          &count))) {
        status = gsh_builtin_error(&descriptor_builtin_io,
                                   command->argv[0],
                                   "invalid context or operand");
    } else {
        evaluator->loop_control = strcmp(command->argv[0], "break") == 0
                                      ? NATIVE_LOOP_CONTROL_BREAK
                                      : NATIVE_LOOP_CONTROL_CONTINUE;
        evaluator->loop_levels = count;
        status = 0;
    }
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: loop control redirection restore");
        return 125;
    }
    return status == 125 ? 125
                         : (pipeline->negated ? (status == 0 ? 1 : 0)
                                              : status);
}

struct function_evaluation_frame {
    const char *saved_input;
    size_t saved_input_length;
    const gsh_parse_storage *saved_storage;
    gsh_positional_store *saved_positionals;
    size_t saved_active_loops;
    native_loop_control saved_loop_control;
    size_t saved_loop_levels;
    gsh_positional_store positionals;
    gsh_saved_descriptor call_saved[GSH_NATIVE_REDIRECT_CAP];
    gsh_saved_descriptor definition_saved[GSH_NATIVE_REDIRECT_CAP];
    size_t call_saved_count;
    size_t definition_saved_count;
    bool negated;
    bool used;
};

static function_evaluation_frame *acquire_function_evaluation_frame(void)
{
    static function_evaluation_frame frames[GSH_FUNCTION_DEPTH_CAP];
    size_t index;

    for (index = 0U; index < GSH_FUNCTION_DEPTH_CAP; index++) {
        if (!frames[index].used) {
            frames[index].used = true;
            return &frames[index];
        }
    }
    errno = ENOSPC;
    return NULL;
}

static void release_function_evaluation_frame(
    function_evaluation_frame *frame)
{
    if (!require(frame != NULL)) return;
    if (!require(frame->used)) return;
    (void)memset(frame, 0, sizeof(*frame));
}

static void initialize_function_evaluation_frame(
    native_evaluator *evaluator, function_evaluation_frame *frame)
{
    if (!require(evaluator != NULL)) return;
    if (!require(frame != NULL && frame->used)) return;
    (void)memset(frame, 0, sizeof(*frame));
    frame->used = true;
    frame->saved_input = evaluator->input;
    frame->saved_input_length = evaluator->input_length;
    frame->saved_storage = evaluator->storage;
    frame->saved_positionals = evaluator->positionals;
    frame->saved_active_loops = evaluator->active_loops;
    frame->saved_loop_control = evaluator->loop_control;
    frame->saved_loop_levels = evaluator->loop_levels;
}

static bool prepare_function_call(
    native_evaluator *evaluator, const gsh_native_command *command,
    bool apply_call_redirects, function_evaluation_frame *frame,
    int *status)
{
    if (!require(evaluator != NULL && command != NULL)) return false;
    if (!require(frame != NULL && status != NULL)) return false;

    if (apply_call_redirects &&
        save_redirect_descriptors(command, frame->call_saved,
                                  &frame->call_saved_count) == -1) {
        perror("gsh: function redirection save");
        *status = 125;
        return false;
    }
    if (apply_call_redirects &&
        apply_evaluator_redirects(evaluator->pipeline, command,
                                  &evaluator->options) == -1) {
        perror("gsh: function redirection");
        (void)restore_redirect_descriptors(frame->call_saved,
                                           frame->call_saved_count);
        frame->call_saved_count = 0U;
        *status = 1;
        return false;
    }
    if (apply_native_assignments(evaluator->variables, evaluator->journal,
                                 command, &evaluator->options) !=
            GSH_ASSIGNMENT_OK ||
        gsh_positionals_assign(&frame->positionals, command->argc - 1U,
                               command->argv + 1U) == -1) {
        perror("gsh: function arguments");
        if (restore_redirect_descriptors(frame->call_saved,
                                         frame->call_saved_count) == -1) {
            perror("gsh: function redirection restore");
        }
        frame->call_saved_count = 0U;
        *status = 125;
        return false;
    }
    return true;
}

static void enter_function_evaluation(
    native_evaluator *evaluator, function_evaluation_frame *frame)
{
    if (!require(evaluator != NULL)) return;
    if (!require(frame != NULL)) return;
    evaluator->input = gsh_functions_text(evaluator->functions);
    evaluator->input_length = evaluator_functions(evaluator)->text_used;
    evaluator->storage = &evaluator_functions(evaluator)->programs;
    evaluator->positionals = &frame->positionals;
    evaluator->function_depth++;
    evaluator->active_loops = 0;
    evaluator->loop_control = NATIVE_LOOP_CONTROL_NONE;
    evaluator->loop_levels = 0;
}

static int leave_function_evaluation(
    native_evaluator *evaluator, function_evaluation_frame *frame,
    int status)
{
    if (!require(evaluator != NULL)) return 125;
    if (!require(frame != NULL)) return 125;
    if (restore_redirect_descriptors(frame->definition_saved,
                                     frame->definition_saved_count) == -1) {
        perror("gsh: function body redirection restore");
        status = 125;
    }
    evaluator->loop_levels = frame->saved_loop_levels;
    evaluator->loop_control = frame->saved_loop_control;
    evaluator->active_loops = frame->saved_active_loops;
    evaluator->function_depth--;
    evaluator->positionals = frame->saved_positionals;
    evaluator->storage = frame->saved_storage;
    evaluator->input_length = frame->saved_input_length;
    evaluator->input = frame->saved_input;
    if (restore_redirect_descriptors(frame->call_saved,
                                     frame->call_saved_count) == -1) {
        perror("gsh: function redirection restore");
        status = 125;
    }
    return status;
}

static int request_evaluator_function(native_evaluator *evaluator,
                                      const gsh_native_command *command,
                                      const gsh_function_entry *entry,
                                      bool apply_call_redirects,
                                      bool negated)
{
    function_evaluation_frame *frame;
    const gsh_ast_node *definition;
    bool body_ready = true;
    int status = 125;

    if (!require(evaluator != NULL && command != NULL)) return 125;
    if (!require(entry != NULL)) return 125;
    if (evaluator->function_depth >= GSH_FUNCTION_DEPTH_CAP) {
        (void)fputs("gsh: function resource limit exceeded\n", stderr);
        return 125;
    }
    frame = acquire_function_evaluation_frame();
    if (frame == NULL) return 125;
    initialize_function_evaluation_frame(evaluator, frame);
    if (!prepare_function_call(evaluator, command, apply_call_redirects,
                               frame, &status)) {
        release_function_evaluation_frame(frame);
        return status;
    }
    enter_function_evaluation(evaluator, frame);
    definition = &evaluator_storage(evaluator)->nodes[entry->node_offset];
    if (definition->redirect_count != 0) {
        gsh_native_expansion_context expansion =
            native_expansion_context(evaluator, true, NULL);
        evaluator_expansion_request request = {
            .kind = EVALUATOR_EXPAND_REDIRECTS,
            .node_index = entry->node_offset,
        };
        gsh_native_plan_status plan_status =
            run_evaluator_expansion(evaluator, &expansion, &request);
        const gsh_native_command *redirects =
            &evaluator_pipeline(evaluator)->commands[0];

        if (plan_status != GSH_NATIVE_PLAN_OK) {
            status = plan_status == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
            body_ready = false;
        } else if (save_redirect_descriptors(
                       redirects, frame->definition_saved,
                       &frame->definition_saved_count) == -1) {
            perror("gsh: function body redirection save");
            body_ready = false;
        } else if (apply_evaluator_redirects(
                       evaluator->pipeline, redirects,
                       &evaluator->options) == -1) {
            perror("gsh: function body redirection");
            (void)restore_redirect_descriptors(
                frame->definition_saved, frame->definition_saved_count);
            frame->definition_saved_count = 0U;
            status = 1;
            body_ready = false;
        }
    }
    if (body_ready) {
        evaluator->returning = false;
        frame->negated = negated;
        evaluator->function_request_frame = frame;
        evaluator->function_request_root = definition->first_child;
        return GSH_EVALUATOR_FUNCTION_REQUEST;
    }
    status = leave_function_evaluation(evaluator, frame, status);
    release_function_evaluation_frame(frame);
    return status;
}

static int finish_evaluator_function(
    native_evaluator *evaluator, function_evaluation_frame *frame,
    int status)
{
    if (!require(evaluator != NULL && frame != NULL)) return 125;
    if (!require(frame->used)) return 125;
    if (evaluator->returning) {
        status = evaluator->return_status;
        evaluator->returning = false;
    }
    status = leave_function_evaluation(evaluator, frame, status);
    if (frame->negated && status != 125) status = status == 0 ? 1 : 0;
    release_function_evaluation_frame(frame);
    return status;
}

static bool evaluator_pipeline_commands_are_supported(
    native_evaluator *evaluator, const pipeline_expansion_scope *scope,
    bool scoped)
{
    if (scoped && scope == NULL) {
        return false;
    }
    size_t index;

    if (!require(evaluator != NULL)) return false;
    if (!require(evaluator->pipeline != NULL)) return false;
    for (index = 0; index < evaluator_pipeline(evaluator)->command_count; index++) {
        const gsh_native_command *planned =
            &evaluator_pipeline(evaluator)->commands[index];
        const char *path;

        if (evaluator_function(evaluator, planned) != NULL) continue;
        path = scoped ? scoped_command_path_value(
                            scope, (unsigned int)index + 1U, planned,
                            evaluator->default_path)
                      : command_path_value(
                            evaluator->variables, planned,
                            evaluator->default_path);
        if (!native_planned_command_is_supported(evaluator->pipeline,
                                                 index, path)) {
            return false;
        }
    }
    return true;
}

static bool evaluator_tail_is_external(
    const gsh_native_command *command,
    const gsh_function_entry *function)
{
    if (!require(command != NULL)) return false;
    if (!require(command->argc <= GSH_NATIVE_ARGUMENT_CAP)) return false;
    return command->argc != 0 && !native_pure_builtin(command) &&
           !native_file_builtin(command) &&
           !native_pwd_builtin(command) && !native_cd_builtin(command) &&
           !native_environment_builtin(command) &&
           !native_variable_builtin(command) &&
           !native_state_builtin(command) &&
           !native_posix_stateful_builtin(command) &&
           !native_fc_builtin(command) &&
           !native_job_control_builtin(command) &&
           !native_wait_builtin(command) && !native_alias_builtin(command) &&
           !native_hash_builtin(command) && !native_times_builtin(command) &&
           !native_trap_builtin(command) &&
           !native_source_builtin(command) && !native_exec_builtin(command) &&
           !native_exit_builtin(command) &&
           !native_command_inspection_builtin(command) &&
           !native_return_builtin(command) &&
           !native_loop_control_builtin(command) && function == NULL;
}

static void maybe_tail_exec_pipeline(
    native_evaluator *evaluator, const gsh_function_entry *function)
{
    const gsh_native_command *tail;
    int heredoc_descriptors[GSH_NATIVE_HEREDOC_CAP][2];
    char *environment_storage[CHILD_ENVIRONMENT_CAP];
    char *const *environment;

    if (!require(evaluator != NULL)) return;
    if (!require(evaluator->pipeline != NULL)) return;
    if (!evaluator->tail_exec_single || evaluator_pipeline(evaluator)->negated ||
        evaluator_pipeline(evaluator)->command_count != 1U ||
        evaluator_pipeline(evaluator)->heredoc_count != 0U) {
        return;
    }
    tail = &evaluator_pipeline(evaluator)->commands[0];
    if (!evaluator_tail_is_external(tail, function)) return;
    initialize_heredoc_descriptors(heredoc_descriptors);
    if (tail->expansion_error) _exit(1);
    child_apply_redirects(tail, heredoc_descriptors, 0,
                          &evaluator->options);
    if (gsh_fault_should_fail(GSH_FAULT_EXEC, EIO)) {
        child_exec_error(tail->argv[0], errno);
    }
    environment = child_command_environment(
        evaluator->variables, tail, environment_storage);
    child_exec_direct(
        tail->argv,
        command_path_value(evaluator->variables, tail,
                           evaluator->default_path),
        environment, evaluator->command_cache,
        command_cache_path_generation(evaluator->variables, tail),
        command_uses_persistent_path(tail));
}

static pid_t evaluator_wait_target(gsh_background_table *backgrounds,
                                   const char *text)
{
    char *end;
    unsigned long number;

    if (!require(backgrounds != NULL)) return -1;
    if (!require(text != NULL)) return -1;
    if (text[0] == '%') {
        uint32_t job_id;

        return gsh_background_resolve(backgrounds, text, &job_id) ==
                       GSH_JOBSPEC_OK
                   ? gsh_background_job_pid(backgrounds, job_id)
                   : -1;
    }
    errno = 0;
    number = strtoul(text, &end, 10);
    return errno == 0 && *text != '\0' && *end == '\0' && number > 0 &&
                   number <= (unsigned long)INT_MAX
               ? (pid_t)number
               : -1;
}

static size_t collect_evaluator_wait_targets(
    native_evaluator *evaluator, const gsh_native_command *command,
    pid_t targets[GSH_BACKGROUND_CAP])
{
    if (targets == NULL) {
        return 0U;
    }
    size_t target_count = 0U;
    size_t argument;

    if (!require(evaluator != NULL && command != NULL)) return 0U;
    if (!require(command->argc <= GSH_BACKGROUND_CAP)) return 0U;
    if (command->argc == 1U) {
        return gsh_background_snapshot(evaluator->backgrounds, targets);
    }
    for (argument = 1U; argument < command->argc; argument++) {
        targets[target_count++] = evaluator_wait_target(
            evaluator->backgrounds, command->argv[argument]);
    }
    return target_count;
}

static bool wait_for_evaluator_target(native_evaluator *evaluator,
                                      pid_t target, bool *done,
                                      int *wait_status, int *status)
{
    bool known;

    if (!require(evaluator != NULL && done != NULL)) return false;
    if (!require(wait_status != NULL && status != NULL)) return false;
    known = target > 0 && gsh_background_get(
                              evaluator->backgrounds, target, done,
                              wait_status);
    if (known && !*done) {
        pid_t waited = -1;
        size_t attempt;

        for (attempt = 0; attempt < EVALUATOR_WAIT_RETRY_CAP; attempt++) {
            int trapped_signal = gsh_traps_pending_signal(evaluator->traps);

            if (trapped_signal != 0) {
                *status = 128 + trapped_signal;
                return false;
            }
            waited = waitpid(target, wait_status, 0);
            trapped_signal = gsh_traps_pending_signal(evaluator->traps);
            if (trapped_signal != 0) {
                *status = 128 + trapped_signal;
                return false;
            }
            if (!(waited == -1 && errno == EINTR)) break;
        }
        if (attempt == EVALUATOR_WAIT_RETRY_CAP) errno = EINTR;
        if (waited == target) {
            (void)gsh_background_record(evaluator->backgrounds, target,
                                        *wait_status);
            *done = true;
        } else {
            known = false;
        }
    }
    return known;
}

static int wait_on_evaluator_backgrounds(
    native_evaluator *evaluator, const gsh_native_command *command)
{
    pid_t targets[GSH_BACKGROUND_CAP];
    size_t target_count;
    size_t argument;
    int status;

    if (!require(evaluator != NULL)) return 125;
    if (!require(command != NULL)) return 125;
    status = command->argc == 1U ? 0 : 127;
    if (evaluator->backgrounds == NULL) return status;
    target_count = collect_evaluator_wait_targets(
        evaluator, command, targets);
    for (argument = 0; argument < target_count; argument++) {
        bool done;
        int wait_status;
        bool known = wait_for_evaluator_target(
            evaluator, targets[argument], &done, &wait_status, &status);

        if (!known && status != 127) break;
        if (known && done &&
            gsh_background_consume(evaluator->backgrounds,
                                   targets[argument], &wait_status)) {
            if (command->argc != 1U && argument + 1U == target_count) {
                status = wait_status_value(wait_status);
            }
        } else if (command->argc != 1U && argument + 1U == target_count) {
            status = 127;
        }
    }
    return status;
}

static int evaluate_pipeline_wait_builtin(native_evaluator *evaluator)
{
    gsh_saved_descriptor saved[GSH_NATIVE_REDIRECT_CAP];
    size_t saved_count;
    int status;

    if (!require(evaluator != NULL)) return 125;
    if (!require(evaluator->pipeline != NULL)) return 125;
    const gsh_native_command *command = &evaluator_pipeline(evaluator)->commands[0];

    if (save_redirect_descriptors(command, saved, &saved_count) == -1) {
        perror("gsh: wait redirection save");
        return 125;
    }
    if (apply_evaluator_redirects(evaluator->pipeline, command,
                                  &evaluator->options) == -1) {
        perror("gsh: wait redirection");
        (void)restore_redirect_descriptors(saved, saved_count);
        return 1;
    }
    status = evaluator->job_service_available
                 ? request_reactor_job_service(
                       evaluator->job_service_socket, command)
                 : wait_on_evaluator_backgrounds(evaluator, command);
    if (restore_redirect_descriptors(saved, saved_count) == -1) {
        perror("gsh: wait redirection restore");
        status = 125;
    }
    return evaluator_pipeline(evaluator)->negated && status != 125
               ? (status == 0 ? 1 : 0)
               : status;
}

static bool evaluate_pipeline_state_leaf(native_evaluator *evaluator,
                                         int *status)
{
    if (!require(evaluator != NULL)) return false;
    if (!require(evaluator->pipeline != NULL && status != NULL)) return false;
    const gsh_native_command *command = &evaluator_pipeline(evaluator)->commands[0];

    if (native_job_control_builtin(command)) {
        *status = run_evaluator_job_control_builtin(
            evaluator->pipeline, evaluator->variables,
            evaluator->scope_base, &evaluator->options,
            evaluator->backgrounds, evaluator->job_service_socket,
            evaluator->job_service_available);
    } else if (native_variable_builtin(command)) {
        *status = run_evaluator_variable_builtin(
            evaluator->pipeline, evaluator->variables, evaluator->journal,
            &evaluator->options, evaluator->functions);
    } else if (native_state_builtin(command)) {
        *status = run_evaluator_state_builtin(
            evaluator->pipeline, evaluator->variables, evaluator->journal,
            evaluator->positionals, &evaluator->options);
    } else if (native_cd_builtin(command)) {
        *status = run_evaluator_cd_builtin(
            evaluator->pipeline, evaluator->variables,
            evaluator->scope_base, evaluator->journal, &evaluator->options,
            NULL, 0);
    } else if (native_alias_builtin(command)) {
        *status = run_evaluator_alias_builtin(
            evaluator->pipeline, evaluator->aliases,
            evaluator->alias_journal, &evaluator->options);
        if (*status == 125 && evaluator->alias_journal != NULL) {
            evaluator->state_commit_invalid = true;
        }
    } else if (native_hash_builtin(command)) {
        *status = run_evaluator_hash_builtin(
            evaluator->pipeline, evaluator->variables,
            evaluator->default_path, evaluator->functions,
            evaluator->command_cache, &evaluator->options);
    } else {
        return false;
    }
    return true;
}

static void record_special_builtin_failure(native_evaluator *evaluator,
                                           const gsh_native_command *command,
                                           bool builtin_failed)
{
    if (!require(evaluator != NULL)) return;
    if (!require(command != NULL)) return;
    if (builtin_failed && !command->command_regular_context &&
        !gsh_options_enabled(&evaluator->options, GSH_OPTION_INTERACTIVE)) {
        evaluator->fatal_error = true;
    }
}

static bool evaluate_pipeline_service_leaf(native_evaluator *evaluator,
                                           int *status)
{
    if (!require(evaluator != NULL)) return false;
    if (!require(evaluator->pipeline != NULL && status != NULL)) return false;
    const gsh_native_command *command = &evaluator_pipeline(evaluator)->commands[0];
    bool builtin_failed = false;

    if (native_times_builtin(command)) {
        *status = run_evaluator_times_builtin(
            evaluator->pipeline, evaluator->variables, evaluator->journal,
            &evaluator->options, evaluator->times_context, &builtin_failed);
    } else if (native_trap_builtin(command)) {
        *status = run_evaluator_trap_builtin(
            evaluator->pipeline, evaluator->variables, evaluator->journal,
            &evaluator->options, evaluator->traps, &builtin_failed);
    } else if (native_exec_builtin(command)) {
        *status = run_evaluator_exec_builtin(
            evaluator->pipeline, evaluator->variables, evaluator->journal,
            &evaluator->options, evaluator->default_path,
            evaluator->command_cache, NULL, evaluator->exec_outcome_fd,
            &evaluator->exec_descriptors_dirty, &builtin_failed);
    } else {
        return false;
    }
    record_special_builtin_failure(evaluator, command, builtin_failed);
    return true;
}

static bool evaluator_status_is_fatal(native_evaluator *evaluator,
                                      int status)
{
    const gsh_native_command *command;

    if (!require(evaluator != NULL)) return false;
    if (!require(evaluator->pipeline != NULL)) return false;
    if (status != 125 || evaluator->exiting ||
        evaluator_pipeline(evaluator)->command_count != 1U) {
        return false;
    }
    command = &evaluator_pipeline(evaluator)->commands[0];
    return native_variable_builtin(command) || native_state_builtin(command) ||
           native_posix_stateful_builtin(command) || native_fc_builtin(command) ||
           native_job_control_builtin(command) || native_cd_builtin(command) ||
           native_alias_builtin(command) || native_hash_builtin(command) ||
           native_times_builtin(command) || native_trap_builtin(command) ||
           native_source_builtin(command) || native_exec_builtin(command) ||
           native_exit_builtin(command) || native_loop_control_builtin(command) ||
           (command->argc == 0 && command->assignment_count != 0);
}

static int native_evaluate_pipeline(native_evaluator *evaluator,
                                    size_t node_index,
                                    bool errexit_suppressed)
{
    if (evaluator == NULL) {
        return -1;
    }
    const gsh_ast_node *node = &evaluator_storage(evaluator)->nodes[node_index];
    const gsh_ast_node *command =
        &evaluator_storage(evaluator)->nodes[node->first_child];
    pipeline_expansion_scope scope;
    gsh_native_expansion_context expansion;
    evaluator_expansion_request request = {
        .kind = EVALUATOR_EXPAND_PIPELINE,
        .node_index = node_index,
    };
    bool scoped;
    int status;
    gsh_native_plan_status plan_status;
    const gsh_function_entry *function;

    if (command->next_sibling == GSH_AST_NONE &&
        command->kind != GSH_AST_SIMPLE) return 125;
    scoped = false;
    plan_status = begin_evaluator_pipeline_plan(
        evaluator, node_index, true, &scope, &scoped, NULL, &expansion);
    if (plan_status == GSH_NATIVE_PLAN_OK) {
        plan_status = run_evaluator_expansion(
            evaluator, &expansion, &request);
        plan_status = finish_evaluator_pipeline_plan(
            evaluator, &scope, scoped, plan_status);
    }
    if (plan_status != GSH_NATIVE_PLAN_OK) {
        return plan_status == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
    }
    if (gsh_options_enabled(&evaluator->options, GSH_OPTION_XTRACE) &&
        emit_pipeline_trace(&descriptor_builtin_io,
                            evaluator->pipeline,
                            evaluator->variables) == -1) {
        return 125;
    }
    function = evaluator_pipeline(evaluator)->command_count == 1U
                   ? evaluator_function(
                         evaluator, &evaluator_pipeline(evaluator)->commands[0])
                   : NULL;
    if (function == NULL &&
        !evaluator_pipeline_commands_are_supported(evaluator, &scope,
                                                   scoped)) {
        return 125;
    }
    maybe_tail_exec_pipeline(evaluator, function);
    if (function != NULL) {
        status = request_evaluator_function(
            evaluator, &evaluator_pipeline(evaluator)->commands[0], function, true,
            evaluator_pipeline(evaluator)->negated);
    } else if (evaluator_pipeline(evaluator)->command_count == 1 &&
               native_exit_builtin(
                   &evaluator_pipeline(evaluator)->commands[0])) {
        status = evaluate_exit(
            evaluator, &evaluator_pipeline(evaluator)->commands[0]);
    } else if (evaluator_pipeline(evaluator)->command_count == 1 &&
               native_return_builtin(
                   &evaluator_pipeline(evaluator)->commands[0])) {
        status = evaluate_return(
            evaluator, &evaluator_pipeline(evaluator)->commands[0]);
        if (evaluator_pipeline(evaluator)->negated && status != 125) {
            status = status == 0 ? 1 : 0;
            evaluator->return_status = status;
        }
    } else if (evaluator_pipeline(evaluator)->command_count == 1 &&
        native_wait_builtin(&evaluator_pipeline(evaluator)->commands[0])) {
        status = evaluate_pipeline_wait_builtin(evaluator);
    } else if (evaluator_pipeline(evaluator)->command_count == 1 &&
               native_fc_builtin(&evaluator_pipeline(evaluator)->commands[0])) {
        status = run_evaluator_fc_builtin(evaluator);
    } else if (evaluator_pipeline(evaluator)->command_count == 1 &&
               evaluate_pipeline_state_leaf(evaluator, &status)) {
        /* The leaf owns status and any transactional invalidation. */
    } else if (evaluator_pipeline(evaluator)->command_count == 1 &&
               native_source_builtin(
                   &evaluator_pipeline(evaluator)->commands[0])) {
        const gsh_native_command *source_command =
            &evaluator_pipeline(evaluator)->commands[0];
        bool builtin_failed;

        status = run_evaluator_source_builtin(evaluator,
                                              &builtin_failed);
        if (builtin_failed && !source_command->command_regular_context &&
            !gsh_options_enabled(&evaluator->options,
                                 GSH_OPTION_INTERACTIVE)) {
            evaluator->fatal_error = true;
        }
    } else if (evaluator_pipeline(evaluator)->command_count == 1 &&
               evaluate_pipeline_service_leaf(evaluator, &status)) {
        /* The leaf owns fatal special-builtin error propagation. */
    } else {
        status = run_native_noninteractive_pipeline(
            evaluator->pipeline, evaluator->default_path,
            evaluator->variables, evaluator->journal,
            evaluator->aliases, evaluator->alias_journal,
            scoped ? &scope : NULL, evaluator->positionals,
            &evaluator->options, evaluator->functions,
            evaluator->scope_base, evaluator, errexit_suppressed);
    }
    if (evaluator_status_is_fatal(evaluator, status)) {
        evaluator->fatal_error = true;
    }
    return status;
}

static bool native_case_item_matches(native_evaluator *evaluator,
                                     const gsh_ast_node *item,
                                     const char *subject,
                                     bool fallthrough, bool *matched)
{
    if (!require(evaluator != NULL && item != NULL)) return false;
    if (!require(subject != NULL && matched != NULL)) return false;
    size_t pattern;

    *matched = fallthrough;
    for (pattern = 0U; pattern < item->word_count && !*matched; pattern++) {
        gsh_word_ref reference = evaluator_storage(evaluator)->words[
            item->first_word + pattern];
        char pattern_text[GSH_NATIVE_TEXT_CAP];

        if (!native_case_pattern(evaluator->input, reference,
                                 pattern_text)) {
            return false;
        }
        *matched = fnmatch(pattern_text, subject, 0) == 0;
    }
    return true;
}

enum { LOOP_CONTROL_NONE, LOOP_CONTROL_NEXT, LOOP_CONTROL_LEAVE };

enum { FOR_ITEM_TEXT_DEPTH_CAP = 129 };

typedef struct {
    char text[FOR_ITEM_TEXT_DEPTH_CAP][GSH_NATIVE_TEXT_CAP];
    bool used[FOR_ITEM_TEXT_DEPTH_CAP];
} for_item_text_pool;

static for_item_text_pool *process_for_item_text_pool(void)
{
    static for_item_text_pool pool;

    return &pool;
}

static char *acquire_for_item_text(void)
{
    for_item_text_pool *pool = process_for_item_text_pool();
    size_t index;

    if (gsh_fault_should_fail(GSH_FAULT_FOR_ALLOCATION, ENOMEM)) {
        return NULL;
    }
    for (index = 0; index < FOR_ITEM_TEXT_DEPTH_CAP; index++) {
        if (!pool->used[index]) {
            pool->used[index] = true;
            return pool->text[index];
        }
    }
    errno = ENOSPC;
    return NULL;
}

static void release_for_item_text(char *text)
{
    for_item_text_pool *pool = process_for_item_text_pool();
    size_t index;

    if (text == NULL) {
        return;
    }
    for (index = 0; index < FOR_ITEM_TEXT_DEPTH_CAP; index++) {
        if (text == pool->text[index]) {
            (void)memset(text, 0, GSH_NATIVE_TEXT_CAP);
            pool->used[index] = false;
            return;
        }
    }
    errno = EINVAL;
}

static bool preserve_for_expansion_items(
    const gsh_native_pipeline *pipeline,
    char *items[GSH_NATIVE_ARGUMENT_CAP], size_t item_count,
    char **item_text)
{
    if (!require(pipeline != NULL && items != NULL)) return false;
    if (!require(item_text != NULL && item_count <= GSH_NATIVE_ARGUMENT_CAP)) {
        return false;
    }
    size_t index;

    *item_text = acquire_for_item_text();
    if (*item_text == NULL) return false;
    (void)memcpy(*item_text, pipeline->text, pipeline->text_used);
    for (index = 0U; index < item_count; index++) {
        items[index] = *item_text + (items[index] - pipeline->text);
    }
    return true;
}

static int consume_loop_control(native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return -1;
    }
    native_loop_control control = evaluator->loop_control;

    if (evaluator->loop_levels == 0) {
        return LOOP_CONTROL_NONE;
    }
    evaluator->loop_levels--;
    if (evaluator->loop_levels == 0) {
        evaluator->loop_control = NATIVE_LOOP_CONTROL_NONE;
    }
    return control == NATIVE_LOOP_CONTROL_CONTINUE &&
                   evaluator->loop_levels == 0
               ? LOOP_CONTROL_NEXT
               : LOOP_CONTROL_LEAVE;
}

static bool evaluator_control_status(const native_evaluator *evaluator,
                                     size_t depth, int *status)
{
    if (!require(evaluator != NULL)) return true;
    if (!require(status != NULL)) return true;
    if (depth > 128U) {
        *status = 125;
        return true;
    }
    if (evaluator->exiting) {
        *status = evaluator->exit_status;
        return true;
    }
    if (evaluator->returning) {
        *status = evaluator->return_status;
        return true;
    }
    if (evaluator->loop_levels != 0U) {
        *status = evaluator->last_status;
        return true;
    }
    return false;
}

static bool evaluate_definition_node(native_evaluator *evaluator,
                                     size_t node_index, int *status)
{
    if (!require(evaluator != NULL)) return false;
    if (!require(status != NULL)) return false;
    if (evaluator_storage(evaluator)->nodes[node_index].kind != GSH_AST_FUNCTION) {
        return false;
    }
    if (!define_evaluator_function(evaluator, node_index)) {
        perror("gsh: function definition");
        *status = errno == ENOSPC ? 125 : 1;
    } else {
        *status = 0;
    }
    return true;
}

static bool static_for_pipeline_status(native_evaluator *evaluator,
                                       const gsh_ast_node *node,
                                       int *status)
{
    const gsh_ast_node *command;
    gsh_word_ref word;
    size_t length;
    int direct_status;

    if (!require(evaluator != NULL && node != NULL)) return false;
    if (!require(status != NULL)) return false;
    if (!evaluator->static_for_items || node->kind != GSH_AST_PIPELINE ||
        node->first_child == GSH_AST_NONE) {
        return false;
    }
    command = &evaluator_storage(evaluator)->nodes[node->first_child];
    if (command->kind != GSH_AST_SIMPLE ||
        command->next_sibling != GSH_AST_NONE || command->word_count != 1U ||
        command->redirect_count != 0U) {
        return false;
    }
    word = evaluator_storage(evaluator)->words[command->first_word];
    length = word.end - word.begin;
    direct_status =
        (length == 1U && evaluator->input[word.begin] == ':') ||
                (length == 4U &&
                 memcmp(evaluator->input + word.begin, "true", 4) == 0)
            ? 0
            : 1;
    *status = (node->flags & GSH_AST_FLAG_NEGATED) != 0
                  ? (direct_status == 0 ? 1 : 0)
                  : direct_status;
    return true;
}

static void initialize_native_subshell_evaluator(
    const native_evaluator *evaluator, native_evaluator *child)
{
    if (!require(evaluator != NULL)) _exit(125);
    if (!require(child != NULL)) _exit(125);
    *child = *evaluator;
    enter_native_subshell_or_exit(child);
    child->active_loops = 0;
    child->loop_control = NATIVE_LOOP_CONTROL_NONE;
    child->loop_levels = 0;
    child->times_context = NULL;
    close_evaluator_exec_transaction(child);
}

static int wait_for_native_subshell(pid_t pid)
{
    size_t attempt;

    if (!require(pid > 0)) return 125;
    if (!require(pid <= INT_MAX)) return 125;
    for (attempt = 0; attempt < EVALUATOR_WAIT_RETRY_CAP; attempt++) {
        int wait_status;
        pid_t waited = waitpid(pid, &wait_status, 0);

        if (waited == pid) return wait_status_value(wait_status);
        if (waited == -1 && errno == EINTR) continue;
        perror("gsh: subshell waitpid");
        return 125;
    }
    errno = EINTR;
    perror("gsh: subshell waitpid");
    return 125;
}

static int evaluator_control_result(const native_evaluator *evaluator,
                                    int status)
{
    if (!require(evaluator != NULL)) return 125;
    if (!require(status >= 0)) return 125;
    return evaluator->exiting
               ? evaluator->exit_status
               : evaluator->returning ? evaluator->return_status : status;
}

static bool sequence_child_is_selected(const gsh_ast_node *parent,
                                       const gsh_ast_node *child,
                                       int status)
{
    if (!require(parent != NULL)) return false;
    if (!require(child != NULL)) return false;
    if (parent->kind != GSH_AST_AND_OR) return true;
    if (child->connector == GSH_TOKEN_AND_IF) return status == 0;
    if (child->connector == GSH_TOKEN_OR_IF) return status != 0;
    return true;
}

typedef struct {
    size_t node_index;
    size_t next_child;
    size_t depth;
    int status;
    bool errexit_suppressed;
} sequence_evaluation_frame;

enum { SEQUENCE_EVALUATION_CAP = 129 };

typedef struct {
    sequence_evaluation_frame frames[SEQUENCE_EVALUATION_CAP];
    size_t frame_count;
    size_t steps;
    bool waiting;
} sequence_evaluation;

static bool evaluator_sequence_kind(gsh_ast_kind kind)
{
    if (!require(kind >= GSH_AST_PROGRAM)) return false;
    if (!require(kind <= GSH_AST_FUNCTION)) return false;
    return kind == GSH_AST_PROGRAM || kind == GSH_AST_LIST ||
           kind == GSH_AST_AND_OR || kind == GSH_AST_BRACE_GROUP ||
           kind == GSH_AST_IF_BRANCH || kind == GSH_AST_CASE_ITEM;
}

/* ── Sequence Nodes Share One Bounded Runtime Walk ───────────────
 * Program, list, and/or, and grouping nodes once consumed one C frame for
 * every syntactic layer.  Their execution rule is the same ordered sibling
 * walk with one connector filter.  A fixed stack now retains each cursor and
 * status explicitly, while compound leaves keep their specialized owners.
 * Parser depth and the 129-frame ceiling jointly bound every traversal.
 * ─────────────────────────────────────────────────────────────── */
static bool initialize_sequence_evaluation(
    native_evaluator *evaluator, size_t node_index, size_t depth,
    bool errexit_suppressed, sequence_evaluation *evaluation)
{
    if (!require(evaluator != NULL && evaluator->storage != NULL)) return false;
    if (!require(node_index < evaluator_storage(evaluator)->node_count &&
                 depth <= 128U && evaluation != NULL)) return false;
    (void)memset(evaluation, 0, sizeof(*evaluation));
    evaluation->frame_count = 1U;
    evaluation->frames[0] = (sequence_evaluation_frame){
        node_index, evaluator_storage(evaluator)->nodes[node_index].first_child,
        depth, 0, errexit_suppressed};
    return true;
}

/* ── Errexit Context Travels With Scheduled Work ────────────────
 * Failure policy belongs to the syntactic command context, not to a global
 * status check. Each bounded machine task therefore carries the inherited
 * suppression bit through functions, sources, subshells, and pipeline-local
 * evaluators. AND-OR tests, conditions, and `!` set it before any child runs.
 * ─────────────────────────────────────────────────────────────── */
static bool sequence_child_suppresses_errexit(
    const gsh_ast_node *parent, const gsh_ast_node *child)
{
    if (!require(parent != NULL && child != NULL)) return true;
    return parent->kind == GSH_AST_AND_OR &&
           child->next_sibling != GSH_AST_NONE;
}

static bool next_sequence_evaluation(
    native_evaluator *evaluator, sequence_evaluation *evaluation,
    bool completed, int *status, size_t *child_index, size_t *child_depth,
    bool *child_errexit_suppressed, bool *finished)
{
    if (!require(evaluator != NULL && evaluation != NULL)) return false;
    if (!require(status != NULL && child_index != NULL &&
                 child_depth != NULL && child_errexit_suppressed != NULL &&
                 finished != NULL)) return false;

    *finished = false;
    if (completed) {
        if (!evaluation->waiting || evaluation->frame_count == 0U) {
            *status = 125;
            *finished = true;
            return false;
        }
        evaluation->frames[evaluation->frame_count - 1U].status = *status;
        evaluation->waiting = false;
    }
    while (evaluation->frame_count > 0U &&
           evaluation->steps++ < 2U * GSH_PARSE_NODE_CAP) {
        sequence_evaluation_frame *frame =
            &evaluation->frames[evaluation->frame_count - 1U];
        const gsh_ast_node *parent =
            &evaluator_storage(evaluator)->nodes[frame->node_index];
        const gsh_ast_node *child;

        if (frame->next_child == GSH_AST_NONE) {
            *status = frame->status;
            evaluation->frame_count--;
            if (evaluation->frame_count > 0U) {
                evaluation->frames[evaluation->frame_count - 1U].status =
                    *status;
            }
            continue;
        }
        *child_index = frame->next_child;
        child = &evaluator_storage(evaluator)->nodes[*child_index];
        frame->next_child = child->next_sibling;
        if (!sequence_child_is_selected(parent, child, frame->status)) {
            continue;
        }
        if (evaluator_sequence_kind(child->kind) &&
            (child->flags & GSH_AST_FLAG_ASYNC) == 0U) {
            if (evaluation->frame_count == SEQUENCE_EVALUATION_CAP) {
                *status = 125;
                *finished = true;
                return false;
            }
            evaluation->frames[evaluation->frame_count++] =
                (sequence_evaluation_frame){
                    *child_index, child->first_child,
                    frame->depth + 1U, 0,
                    frame->errexit_suppressed ||
                        sequence_child_suppresses_errexit(parent, child)};
            continue;
        }
        *child_depth = frame->depth + 1U;
        *child_errexit_suppressed =
            frame->errexit_suppressed ||
            sequence_child_suppresses_errexit(parent, child);
        evaluation->waiting = true;
        return true;
    }
    *finished = evaluation->frame_count == 0U;
    if (!*finished) *status = 125;
    return false;
}

static int native_evaluate_leaf(native_evaluator *evaluator,
                                size_t node_index, size_t depth,
                                bool errexit_suppressed)
{
    const gsh_ast_node *node;
    int status = 0;

    if (!require(evaluator != NULL)) return 125;
    if (!require(evaluator->storage != NULL &&
                 node_index < evaluator_storage(evaluator)->node_count)) return 125;
    if (evaluator_control_status(evaluator, depth, &status)) return status;
    node = &evaluator_storage(evaluator)->nodes[node_index];
    if (evaluate_definition_node(evaluator, node_index, &status)) return status;
    if (static_for_pipeline_status(evaluator, node, &status)) return status;
    if (node->kind == GSH_AST_PIPELINE) {
        return native_evaluate_pipeline(
            evaluator, node_index, errexit_suppressed);
    }
    if (node->kind == GSH_AST_SUBSHELL) return 125;
    if (node->kind == GSH_AST_IF || node->kind == GSH_AST_CASE) return 125;
    if (node->kind == GSH_AST_FOR) return 125;
    if (node->kind == GSH_AST_WHILE || node->kind == GSH_AST_UNTIL) return 125;
    return evaluator_sequence_kind(node->kind) ? 125 : status;
}

typedef enum {
    NATIVE_MACHINE_NODE,
    NATIVE_MACHINE_SEQUENCE,
    NATIVE_MACHINE_NEGATE,
    NATIVE_MACHINE_COMPLETE,
    NATIVE_MACHINE_IF,
    NATIVE_MACHINE_CASE,
    NATIVE_MACHINE_CASE_AFTER,
    NATIVE_MACHINE_FOR,
    NATIVE_MACHINE_FOR_AFTER,
    NATIVE_MACHINE_WHILE_CONDITION,
    NATIVE_MACHINE_WHILE_BODY,
    NATIVE_MACHINE_PIPELINE_CHILD_FINISH,
    NATIVE_MACHINE_TRAP_FINISH,
    NATIVE_MACHINE_FUNCTION_FINISH,
    NATIVE_MACHINE_SOURCE_FINISH,
} native_machine_task_kind;

typedef struct {
    native_machine_task_kind kind;
    size_t node_index;
    size_t depth;
    size_t cursor;
    size_t auxiliary;
    unsigned int phase;
    bool fallthrough;
    bool errexit_suppressed;
    int result_status;
    char *text;
    char *items[GSH_NATIVE_ARGUMENT_CAP];
    size_t item_count;
    size_t item_index;
    gsh_word_ref name;
    pipeline_child_request *pipeline_child;
    native_trap_request *trap_request;
    sequence_evaluation sequence;
    function_evaluation_frame *function;
    native_source_frame source;
} native_machine_task;

enum {
    NATIVE_MACHINE_TASK_CAP =
        2 * (GSH_FUNCTION_DEPTH_CAP + GSH_SOURCE_DEPTH_CAP) + 8,
    NATIVE_MACHINE_WORKSPACE_CAP = GSH_SOURCE_DEPTH_CAP + 4,
};

typedef struct {
    native_machine_task tasks[NATIVE_MACHINE_TASK_CAP];
    native_evaluator children[SEQUENCE_EVALUATION_CAP];
    size_t count;
    size_t child_count;
    bool used;
} native_machine_workspace;

static bool take_pipeline_child_request(
    native_evaluator **evaluator, native_machine_workspace *workspace,
    native_machine_task *task, bool *child_process);
static int finish_pipeline_child_request(
    native_evaluator *evaluator, pipeline_child_request *request,
    int status);
static bool take_native_trap_request(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task *task);
static int finish_native_trap_request(
    native_evaluator *evaluator, native_trap_request *request,
    int status);

static native_machine_workspace *acquire_native_machine_workspace(
    const native_evaluator *evaluator)
{
    static native_machine_workspace
        workspaces[NATIVE_MACHINE_WORKSPACE_CAP];
    size_t index;

    if (!require(evaluator != NULL)) return NULL;
    if (!require(evaluator->storage != NULL)) return NULL;
    for (index = 0U; index < NATIVE_MACHINE_WORKSPACE_CAP; index++) {
        if (!workspaces[index].used) {
            (void)memset(&workspaces[index], 0, sizeof(workspaces[index]));
            workspaces[index].used = true;
            return &workspaces[index];
        }
    }
    errno = ENOSPC;
    return NULL;
}

static void release_native_machine_workspace(
    const native_evaluator *evaluator, native_machine_workspace *workspace)
{
    if (!require(evaluator != NULL)) return;
    if (!require(workspace != NULL && workspace->used)) return;
    (void)memset(workspace, 0, sizeof(*workspace));
}

static bool push_native_machine_task(
    native_machine_workspace *workspace, const native_machine_task *task)
{
    if (!require(workspace != NULL && workspace->used)) return false;
    if (!require(task != NULL &&
                 workspace->count <= NATIVE_MACHINE_TASK_CAP)) return false;
    if (workspace->count == NATIVE_MACHINE_TASK_CAP) return false;
    workspace->tasks[workspace->count++] = *task;
    return true;
}

static native_evaluator *acquire_native_machine_child(
    native_evaluator *evaluator, native_machine_workspace *workspace)
{
    if (!require(evaluator != NULL && workspace != NULL)) return NULL;
    if (!require(workspace->child_count <= SEQUENCE_EVALUATION_CAP)) {
        return NULL;
    }
    if (workspace->child_count == SEQUENCE_EVALUATION_CAP) return NULL;
    return &workspace->children[workspace->child_count++];
}

typedef enum {
    NATIVE_MACHINE_PROCESS_STATUS,
    NATIVE_MACHINE_PROCESS_SWITCH,
    NATIVE_MACHINE_PROCESS_ERROR,
} native_machine_process_result;

static native_machine_process_result start_native_machine_async(
    native_evaluator **evaluator, native_machine_workspace *workspace,
    native_machine_task *task, int *status, bool *child_process)
{
    if (!require(evaluator != NULL && *evaluator != NULL)) {
        return NATIVE_MACHINE_PROCESS_ERROR;
    }
    if (!require(workspace != NULL && task != NULL && status != NULL &&
                 child_process != NULL)) {
        return NATIVE_MACHINE_PROCESS_ERROR;
    }
    native_evaluator *child = acquire_native_machine_child(
        *evaluator, workspace);
    native_async_start started;

    if (child == NULL) {
        *status = 125;
        return NATIVE_MACHINE_PROCESS_ERROR;
    }
    started = start_native_async(
        *evaluator, task->node_index, child, status);
    if (started != NATIVE_ASYNC_CHILD) {
        workspace->child_count--;
        return started == NATIVE_ASYNC_PARENT
                   ? NATIVE_MACHINE_PROCESS_STATUS
                   : NATIVE_MACHINE_PROCESS_ERROR;
    }
    *evaluator = child;
    (*evaluator)->suppress_async_once = true;
    workspace->count = 0U;
    *child_process = true;
    return push_native_machine_task(workspace, task)
               ? NATIVE_MACHINE_PROCESS_SWITCH
               : NATIVE_MACHINE_PROCESS_ERROR;
}

static native_machine_process_result start_native_machine_subshell(
    native_evaluator **evaluator, native_machine_workspace *workspace,
    const native_machine_task *task, int *status, bool *child_process)
{
    if (!require(evaluator != NULL && *evaluator != NULL)) {
        return NATIVE_MACHINE_PROCESS_ERROR;
    }
    if (!require(workspace != NULL && task != NULL && status != NULL &&
                 child_process != NULL)) {
        return NATIVE_MACHINE_PROCESS_ERROR;
    }
    const gsh_ast_node *node =
        &(*evaluator)->storage->nodes[task->node_index];
    native_evaluator *child = acquire_native_machine_child(
        *evaluator, workspace);
    pid_t pid;

    if (child == NULL) return NATIVE_MACHINE_PROCESS_ERROR;
    pid = gsh_fault_should_fail(GSH_FAULT_SUBSHELL_FORK, EAGAIN) ? -1 : fork();
    if (pid == 0) {
        native_machine_task body = {
            .kind = NATIVE_MACHINE_NODE,
            .node_index = node->first_child,
            .depth = task->depth + 1U,
            .errexit_suppressed = task->errexit_suppressed,
        };

        initialize_native_subshell_evaluator(*evaluator, child);
        *evaluator = child;
        workspace->count = 0U;
        *child_process = true;
        return push_native_machine_task(workspace, &body)
                   ? NATIVE_MACHINE_PROCESS_SWITCH
                   : NATIVE_MACHINE_PROCESS_ERROR;
    }
    if (pid == -1) {
        workspace->child_count--;
        perror("gsh: subshell fork");
        *status = 125;
        return NATIVE_MACHINE_PROCESS_ERROR;
    }
    *status = wait_for_native_subshell(pid);
    workspace->child_count--;
    return NATIVE_MACHINE_PROCESS_STATUS;
}

static int schedule_native_machine_request(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    int *status, bool errexit_suppressed)
{
    if (!require(evaluator != NULL && workspace != NULL)) return -1;
    if (!require(status != NULL)) return -1;
    native_machine_task finish = {0};
    native_machine_task body = {
        .kind = NATIVE_MACHINE_NODE,
        .errexit_suppressed = errexit_suppressed,
    };

    finish.errexit_suppressed = errexit_suppressed;

    if (*status == GSH_EVALUATOR_FUNCTION_REQUEST) {
        finish.kind = NATIVE_MACHINE_FUNCTION_FINISH;
        finish.function = evaluator->function_request_frame;
        body.node_index = evaluator->function_request_root;
        if (finish.function == NULL) return -1;
        evaluator->function_request_frame = NULL;
    } else if (*status == GSH_EVALUATOR_SOURCE_REQUEST) {
        finish.kind = NATIVE_MACHINE_SOURCE_FINISH;
        if (!source_request_is_valid(evaluator) ||
            !enter_source_frame(evaluator, &finish.source,
                                &body.node_index)) {
            *status = abandon_source_request(evaluator);
            return 0;
        }
    } else {
        return 0;
    }
    if (!push_native_machine_task(workspace, &finish) ||
        !push_native_machine_task(workspace, &body)) {
        *status = 125;
        return -1;
    }
    return 1;
}

static void apply_native_errexit(native_evaluator *evaluator, int status,
                                 bool suppressed)
{
    if (evaluator == NULL || suppressed || status == 0 ||
        evaluator->exiting || evaluator->returning ||
        gsh_options_enabled(&evaluator->options, GSH_OPTION_INTERACTIVE)) {
        return;
    }
    if (gsh_options_enabled(&evaluator->options, GSH_OPTION_ERREXIT)) {
        evaluator->fatal_error = true;
    }
}

static bool complete_native_machine_node(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    int *status, bool errexit_suppressed)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(status != NULL && *status >= 0 && *status <= 255)) {
        return false;
    }
    *status = run_native_traps(
        evaluator, *status, NATIVE_TRAPS_PENDING);
    if (*status == GSH_EVALUATOR_TRAP_REQUEST) {
        native_machine_task task = {
            .errexit_suppressed = errexit_suppressed,
        };

        return take_native_trap_request(evaluator, workspace, &task);
    }
    evaluator->last_status = *status;
    apply_native_errexit(evaluator, *status, errexit_suppressed);
    return true;
}

static bool prepare_native_machine_case(
    native_evaluator *evaluator, const gsh_ast_node *node,
    native_machine_task *task, int *status)
{
    if (!require(evaluator != NULL && node != NULL)) return false;
    if (!require(task != NULL && status != NULL)) return false;
    gsh_native_expansion_context expansion =
        native_expansion_context(evaluator, true, NULL);
    char *subject;
    evaluator_expansion_request request = {
        .kind = EVALUATOR_EXPAND_SCALAR,
        .word = evaluator_storage(evaluator)->words[node->first_word],
        .scalar = &subject,
    };
    size_t length;

    if (run_evaluator_expansion(evaluator, &expansion, &request) !=
        GSH_NATIVE_PLAN_OK) {
        *status = 125;
        return false;
    }
    length = strnlen(subject, GSH_NATIVE_TEXT_CAP);
    task->text = acquire_for_item_text();
    if (task->text == NULL || length == GSH_NATIVE_TEXT_CAP) {
        release_for_item_text(task->text);
        task->text = NULL;
        *status = 125;
        return false;
    }
    (void)memcpy(task->text, subject, length + 1U);
    task->kind = NATIVE_MACHINE_CASE;
    task->cursor = node->first_child;
    task->result_status = 0;
    return true;
}

static bool run_native_machine_if(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task task, int *status)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(status != NULL && task.kind == NATIVE_MACHINE_IF)) {
        return false;
    }
    native_machine_task child = {
        .kind = NATIVE_MACHINE_NODE,
        .depth = task.depth + 1U,
    };

    if (task.phase != 0U) {
        if (task.auxiliary == GSH_AST_NONE || evaluator->exiting ||
            evaluator->returning || evaluator->loop_levels != 0U) {
            task.kind = NATIVE_MACHINE_COMPLETE;
            return push_native_machine_task(workspace, &task);
        }
        if (*status == 0) {
            task.kind = NATIVE_MACHINE_COMPLETE;
            child.node_index = task.auxiliary;
            child.errexit_suppressed = task.errexit_suppressed;
            return push_native_machine_task(workspace, &task) &&
                   push_native_machine_task(workspace, &child);
        }
    }
    if (task.cursor == GSH_AST_NONE) {
        *status = 0;
        task.kind = NATIVE_MACHINE_COMPLETE;
        return push_native_machine_task(workspace, &task);
    }
    {
        const gsh_ast_node *branch =
            &evaluator_storage(evaluator)->nodes[task.cursor];
        size_t first = branch->first_child;

        task.phase = 1U;
        task.auxiliary = evaluator_storage(evaluator)->nodes[first].next_sibling;
        task.cursor = branch->next_sibling;
        child.node_index = first;
        child.errexit_suppressed = true;
    }
    return push_native_machine_task(workspace, &task) &&
           push_native_machine_task(workspace, &child);
}

static bool run_native_machine_case(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task task, int *status)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(status != NULL && task.text != NULL)) return false;

    if (task.kind == NATIVE_MACHINE_CASE_AFTER) {
        task.result_status = *status;
        if (evaluator->exiting || evaluator->returning ||
            evaluator->loop_levels != 0U || !task.fallthrough) {
            release_for_item_text(task.text);
            task.text = NULL;
            task.kind = NATIVE_MACHINE_COMPLETE;
            return push_native_machine_task(workspace, &task);
        }
        task.kind = NATIVE_MACHINE_CASE;
    }
    while (task.cursor != GSH_AST_NONE) {
        const gsh_ast_node *item =
            &evaluator_storage(evaluator)->nodes[task.cursor];
        bool matched;

        if (!native_case_item_matches(
                evaluator, item, task.text, task.fallthrough, &matched)) {
            release_for_item_text(task.text);
            *status = 125;
            return true;
        }
        task.cursor = item->next_sibling;
        if (!matched) continue;
        task.fallthrough =
            (item->flags & GSH_AST_FLAG_CASE_FALLTHROUGH) != 0U;
        if (item->first_child == GSH_AST_NONE) {
            task.result_status = 0;
            if (task.fallthrough) continue;
            break;
        }
        task.kind = NATIVE_MACHINE_CASE_AFTER;
        {
            native_machine_task child = {
                .kind = NATIVE_MACHINE_NODE,
                .node_index = item->first_child,
                .depth = task.depth + 1U,
                .errexit_suppressed = task.errexit_suppressed,
            };

            return push_native_machine_task(workspace, &task) &&
                   push_native_machine_task(workspace, &child);
        }
    }
    release_for_item_text(task.text);
    *status = task.result_status;
    task.text = NULL;
    task.kind = NATIVE_MACHINE_COMPLETE;
    return push_native_machine_task(workspace, &task);
}

static bool prepare_native_machine_for(
    native_evaluator *evaluator, const gsh_ast_node *node,
    native_machine_task *task, int *status)
{
    if (!require(evaluator != NULL && node != NULL)) return false;
    if (!require(task != NULL && status != NULL)) return false;

    task->kind = NATIVE_MACHINE_FOR;
    task->name = evaluator_storage(evaluator)->words[node->first_word];
    task->result_status = 0;
    task->item_index = 0U;
    if (evaluator->static_for_items &&
        (node->flags & GSH_AST_FLAG_FOR_HAS_IN) != 0U) {
        task->phase = 1U;
        task->item_index = 1U;
        task->item_count = node->word_count;
    } else if ((node->flags & GSH_AST_FLAG_FOR_HAS_IN) != 0U) {
        gsh_native_expansion_context expansion =
            native_expansion_context(evaluator, true, NULL);
        evaluator_expansion_request request = {
            .kind = EVALUATOR_EXPAND_WORDS,
            .words = evaluator_storage(evaluator)->words + node->first_word + 1U,
            .word_count = node->word_count - 1U,
            .expanded = task->items,
            .expanded_count = &task->item_count,
        };
        gsh_native_plan_status expansion_status =
            run_evaluator_expansion(evaluator, &expansion, &request);

        if (expansion_status != GSH_NATIVE_PLAN_OK) {
            *status = expansion_status == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
            return false;
        }
        if (task->item_count != 0U &&
            !preserve_for_expansion_items(
                evaluator->pipeline, task->items, task->item_count,
                &task->text)) {
            perror("gsh: for items");
            *status = 125;
            return false;
        }
    } else {
        task->item_count = gsh_positionals_count(evaluator->positionals);
        gsh_positionals_view(evaluator->positionals, task->items);
    }
    evaluator->active_loops++;
    return true;
}

static bool finish_native_machine_for(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task *task, int *status)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(task != NULL && status != NULL)) return false;

    evaluator->active_loops--;
    release_for_item_text(task->text);
    task->text = NULL;
    *status = task->result_status;
    task->kind = NATIVE_MACHINE_COMPLETE;
    return push_native_machine_task(workspace, task);
}

static bool run_native_machine_for(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task task, int *status)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(status != NULL && task.node_index <
                                  evaluator_storage(evaluator)->node_count)) {
        return false;
    }
    const gsh_ast_node *node =
        &evaluator_storage(evaluator)->nodes[task.node_index];
    const char *value;
    size_t value_length;

    if (task.kind == NATIVE_MACHINE_FOR_AFTER) {
        task.result_status = *status;
        if (evaluator->fatal_error || evaluator->exiting ||
            evaluator->returning ||
            consume_loop_control(evaluator) == LOOP_CONTROL_LEAVE) {
            return finish_native_machine_for(
                evaluator, workspace, &task, status);
        }
        task.kind = NATIVE_MACHINE_FOR;
    }
    if (task.item_index >= task.item_count) {
        return finish_native_machine_for(
            evaluator, workspace, &task, status);
    }
    if (task.phase == 1U) {
        gsh_word_ref item = evaluator_storage(evaluator)->words[
            node->first_word + task.item_index++];

        value = evaluator->input + item.begin;
        value_length = item.end - item.begin;
    } else {
        value = task.items[task.item_index++];
        value_length = strlen(value);
    }
    {
        gsh_native_plan_status assignment = assign_evaluator_variable(
            evaluator, evaluator->input + task.name.begin,
            task.name.end - task.name.begin, value, value_length);

        if (assignment != GSH_NATIVE_PLAN_OK) {
            task.result_status =
                assignment == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
            return finish_native_machine_for(
                evaluator, workspace, &task, status);
        }
    }
    task.kind = NATIVE_MACHINE_FOR_AFTER;
    {
        native_machine_task child = {
            .kind = NATIVE_MACHINE_NODE,
            .node_index = node->first_child,
            .depth = task.depth + 1U,
            .errexit_suppressed = task.errexit_suppressed,
        };

        return push_native_machine_task(workspace, &task) &&
               push_native_machine_task(workspace, &child);
    }
}

static bool finish_native_machine_while(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task *task, int *status)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(task != NULL && status != NULL)) return false;

    evaluator->active_loops--;
    *status = evaluator_control_result(evaluator, task->result_status);
    task->kind = NATIVE_MACHINE_COMPLETE;
    return push_native_machine_task(workspace, task);
}

static bool schedule_native_machine_while_condition(
    native_machine_workspace *workspace, native_machine_task *task,
    const gsh_ast_node *node)
{
    if (!require(workspace != NULL && task != NULL)) return false;
    if (!require(node != NULL && node->first_child != GSH_AST_NONE)) {
        return false;
    }
    native_machine_task child = {
        .kind = NATIVE_MACHINE_NODE,
        .node_index = node->first_child,
        .depth = task->depth + 1U,
        .errexit_suppressed = true,
    };

    task->kind = NATIVE_MACHINE_WHILE_CONDITION;
    return push_native_machine_task(workspace, task) &&
           push_native_machine_task(workspace, &child);
}

static bool run_native_machine_while(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task task, int *status)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(status != NULL && task.node_index <
                                  evaluator_storage(evaluator)->node_count)) {
        return false;
    }
    const gsh_ast_node *node =
        &evaluator_storage(evaluator)->nodes[task.node_index];
    size_t condition = node->first_child;
    size_t body = evaluator_storage(evaluator)->nodes[condition].next_sibling;
    int control;

    if (task.kind == NATIVE_MACHINE_WHILE_BODY) {
        task.result_status = *status;
        if (evaluator->fatal_error || evaluator->exiting ||
            evaluator->returning) {
            return finish_native_machine_while(
                evaluator, workspace, &task, status);
        }
        control = consume_loop_control(evaluator);
        if (control == LOOP_CONTROL_LEAVE) {
            return finish_native_machine_while(
                evaluator, workspace, &task, status);
        }
        return schedule_native_machine_while_condition(
            workspace, &task, node);
    }
    if (evaluator->fatal_error || evaluator->exiting ||
        evaluator->returning) {
        return finish_native_machine_while(
            evaluator, workspace, &task, status);
    }
    control = consume_loop_control(evaluator);
    if (control == LOOP_CONTROL_LEAVE) {
        task.result_status = *status;
        return finish_native_machine_while(
            evaluator, workspace, &task, status);
    }
    if (control == LOOP_CONTROL_NEXT) {
        task.result_status = *status;
        return schedule_native_machine_while_condition(
            workspace, &task, node);
    }
    if ((node->kind == GSH_AST_WHILE && *status != 0) ||
        (node->kind == GSH_AST_UNTIL && *status == 0)) {
        return finish_native_machine_while(
            evaluator, workspace, &task, status);
    }
    task.kind = NATIVE_MACHINE_WHILE_BODY;
    {
        native_machine_task child = {
            .kind = NATIVE_MACHINE_NODE,
            .node_index = body,
            .depth = task.depth + 1U,
            .errexit_suppressed = task.errexit_suppressed,
        };

        return push_native_machine_task(workspace, &task) &&
               push_native_machine_task(workspace, &child);
    }
}

static bool run_native_machine_sequence(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task task, int *status)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(status != NULL &&
                 task.kind == NATIVE_MACHINE_SEQUENCE)) return false;
    size_t child;
    size_t child_depth;
    bool child_errexit_suppressed;
    bool finished;
    bool completed = task.sequence.waiting;

    if (completed && (evaluator->fatal_error || evaluator->exiting ||
                      evaluator->returning ||
                      evaluator->loop_levels != 0U)) {
        return complete_native_machine_node(
            evaluator, workspace, status, task.errexit_suppressed);
    }
    if (!next_sequence_evaluation(
            evaluator, &task.sequence, completed, status, &child,
            &child_depth, &child_errexit_suppressed, &finished)) {
        if (!finished) *status = 125;
        return !finished ||
               complete_native_machine_node(
                   evaluator, workspace, status, task.errexit_suppressed);
    }
    if (!push_native_machine_task(workspace, &task)) {
        *status = 125;
        return false;
    }
    task = (native_machine_task){
        .kind = NATIVE_MACHINE_NODE,
        .node_index = child,
        .depth = child_depth,
        .errexit_suppressed = child_errexit_suppressed,
    };
    return push_native_machine_task(workspace, &task);
}

static bool run_native_machine_continuation(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task task, int *status)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(status != NULL && task.kind != NATIVE_MACHINE_NODE)) {
        return false;
    }

    if (task.kind == NATIVE_MACHINE_NEGATE) {
        *status = *status == 0 ? 1 : 0;
        return complete_native_machine_node(
            evaluator, workspace, status, task.errexit_suppressed);
    }
    if (task.kind == NATIVE_MACHINE_COMPLETE) {
        return complete_native_machine_node(
            evaluator, workspace, status, task.errexit_suppressed);
    }
    if (task.kind == NATIVE_MACHINE_IF) {
        return run_native_machine_if(evaluator, workspace, task, status);
    }
    if (task.kind == NATIVE_MACHINE_CASE ||
        task.kind == NATIVE_MACHINE_CASE_AFTER) {
        return run_native_machine_case(evaluator, workspace, task, status);
    }
    if (task.kind == NATIVE_MACHINE_FOR ||
        task.kind == NATIVE_MACHINE_FOR_AFTER) {
        return run_native_machine_for(evaluator, workspace, task, status);
    }
    if (task.kind == NATIVE_MACHINE_WHILE_CONDITION ||
        task.kind == NATIVE_MACHINE_WHILE_BODY) {
        return run_native_machine_while(evaluator, workspace, task, status);
    }
    if (task.kind == NATIVE_MACHINE_PIPELINE_CHILD_FINISH) {
        *status = finish_pipeline_child_request(
            evaluator, task.pipeline_child, *status);
        return true;
    }
    if (task.kind == NATIVE_MACHINE_TRAP_FINISH) {
        bool errexit_suppressed = task.errexit_suppressed;

        *status = finish_native_trap_request(
            evaluator, task.trap_request, *status);
        if (*status != GSH_EVALUATOR_TRAP_REQUEST) {
            return complete_native_machine_node(
                evaluator, workspace, status, errexit_suppressed);
        }
        task = (native_machine_task){
            .errexit_suppressed = errexit_suppressed,
        };
        return take_native_trap_request(evaluator, workspace, &task);
    }
    if (task.kind == NATIVE_MACHINE_FUNCTION_FINISH) {
        *status = finish_evaluator_function(
            evaluator, task.function, *status);
        return complete_native_machine_node(
            evaluator, workspace, status, task.errexit_suppressed);
    }
    if (task.kind == NATIVE_MACHINE_SOURCE_FINISH) {
        *status = leave_source_frame(evaluator, &task.source, *status);
        return complete_native_machine_node(
            evaluator, workspace, status, task.errexit_suppressed);
    }
    return task.kind == NATIVE_MACHINE_SEQUENCE &&
           run_native_machine_sequence(evaluator, workspace, task, status);
}

typedef enum {
    NATIVE_MACHINE_STEP_STATUS,
    NATIVE_MACHINE_STEP_SCHEDULED,
    NATIVE_MACHINE_STEP_LEAF,
    NATIVE_MACHINE_STEP_ERROR,
} native_machine_step_result;

static native_machine_step_result schedule_native_machine_compound(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task *task, const gsh_ast_node *node, int *status)
{
    if (!require(evaluator != NULL && workspace != NULL)) {
        return NATIVE_MACHINE_STEP_ERROR;
    }
    if (!require(task != NULL && node != NULL && status != NULL)) {
        return NATIVE_MACHINE_STEP_ERROR;
    }

    if (node->kind == GSH_AST_CASE) {
        if (!prepare_native_machine_case(evaluator, node, task, status)) {
            if (evaluator->substitution_child_execution != NULL) {
                return NATIVE_MACHINE_STEP_STATUS;
            }
            task->kind = NATIVE_MACHINE_COMPLETE;
        }
        return push_native_machine_task(workspace, task)
                   ? NATIVE_MACHINE_STEP_SCHEDULED
                   : NATIVE_MACHINE_STEP_ERROR;
    }
    if (node->kind == GSH_AST_FOR) {
        if (!prepare_native_machine_for(evaluator, node, task, status)) {
            if (evaluator->substitution_child_execution != NULL) {
                return NATIVE_MACHINE_STEP_STATUS;
            }
            task->kind = NATIVE_MACHINE_COMPLETE;
        }
        return push_native_machine_task(workspace, task)
                   ? NATIVE_MACHINE_STEP_SCHEDULED
                   : NATIVE_MACHINE_STEP_ERROR;
    }
    if (node->kind == GSH_AST_WHILE || node->kind == GSH_AST_UNTIL) {
        task->result_status = 0;
        evaluator->active_loops++;
        if (schedule_native_machine_while_condition(
                workspace, task, node)) {
            return NATIVE_MACHINE_STEP_SCHEDULED;
        }
        evaluator->active_loops--;
        return NATIVE_MACHINE_STEP_ERROR;
    }
    if (node->kind == GSH_AST_PIPELINE &&
        node->first_child != GSH_AST_NONE &&
        evaluator_storage(evaluator)->nodes[node->first_child].kind != GSH_AST_SIMPLE &&
        evaluator_storage(evaluator)->nodes[node->first_child].next_sibling ==
            GSH_AST_NONE) {
        native_machine_task child = {
            .kind = NATIVE_MACHINE_NODE,
            .node_index = node->first_child,
            .depth = task->depth + 1U,
            .errexit_suppressed =
                task->errexit_suppressed ||
                (node->flags & GSH_AST_FLAG_NEGATED) != 0U,
        };
        bool scheduled = true;

        if ((node->flags & GSH_AST_FLAG_NEGATED) != 0U) {
            task->kind = NATIVE_MACHINE_NEGATE;
            scheduled = push_native_machine_task(workspace, task);
        }
        return scheduled && push_native_machine_task(workspace, &child)
                   ? NATIVE_MACHINE_STEP_SCHEDULED
                   : NATIVE_MACHINE_STEP_ERROR;
    }
    return NATIVE_MACHINE_STEP_LEAF;
}

static bool evaluator_noexec(const native_evaluator *evaluator)
{
    if (!require(evaluator != NULL)) return false;
    return (evaluator->options.enabled &
            (GSH_OPTION_NOEXEC | GSH_OPTION_INTERACTIVE)) ==
           GSH_OPTION_NOEXEC;
}

static native_machine_step_result run_native_machine_node_task(
    native_evaluator **evaluator, native_machine_workspace *workspace,
    native_machine_task *task, int *status, bool *child_process)
{
    if (!require(evaluator != NULL && *evaluator != NULL)) {
        return NATIVE_MACHINE_STEP_ERROR;
    }
    if (!require(workspace != NULL && task != NULL && status != NULL &&
                 child_process != NULL)) return NATIVE_MACHINE_STEP_ERROR;
    const gsh_ast_node *node =
        &(*evaluator)->storage->nodes[task->node_index];
    int control_status;
    bool async_consumed;

    if (node->kind == GSH_AST_PIPELINE &&
        (node->flags & GSH_AST_FLAG_NEGATED) != 0U) {
        task->errexit_suppressed = true;
    }

    if (evaluator_control_status(
            *evaluator, task->depth, &control_status)) {
        *status = control_status;
        return NATIVE_MACHINE_STEP_STATUS;
    }
    if (evaluator_noexec(*evaluator)) {
        *status = 0;
        return NATIVE_MACHINE_STEP_STATUS;
    }
    async_consumed = (*evaluator)->suppress_async_once;
    (*evaluator)->suppress_async_once = false;
    if ((node->flags & GSH_AST_FLAG_ASYNC) != 0U && !async_consumed) {
        native_machine_process_result result = start_native_machine_async(
            evaluator, workspace, task, status, child_process);

        return result == NATIVE_MACHINE_PROCESS_SWITCH
                   ? NATIVE_MACHINE_STEP_SCHEDULED
             : result == NATIVE_MACHINE_PROCESS_STATUS
                   ? NATIVE_MACHINE_STEP_STATUS
                   : NATIVE_MACHINE_STEP_ERROR;
    }
    if (node->kind == GSH_AST_SUBSHELL) {
        native_machine_process_result result = start_native_machine_subshell(
            evaluator, workspace, task, status, child_process);

        return result == NATIVE_MACHINE_PROCESS_SWITCH
                   ? NATIVE_MACHINE_STEP_SCHEDULED
             : result == NATIVE_MACHINE_PROCESS_STATUS
                   ? NATIVE_MACHINE_STEP_STATUS
                   : NATIVE_MACHINE_STEP_ERROR;
    }
    if (evaluator_sequence_kind(node->kind)) {
        task->kind = NATIVE_MACHINE_SEQUENCE;
        return initialize_sequence_evaluation(
                   *evaluator, task->node_index, task->depth,
                   task->errexit_suppressed, &task->sequence) &&
                       push_native_machine_task(workspace, task)
                   ? NATIVE_MACHINE_STEP_SCHEDULED
                   : NATIVE_MACHINE_STEP_ERROR;
    }
    if (node->kind == GSH_AST_IF) {
        task->kind = NATIVE_MACHINE_IF;
        task->cursor = node->first_child;
        task->phase = 0U;
        return push_native_machine_task(workspace, task)
                   ? NATIVE_MACHINE_STEP_SCHEDULED
                   : NATIVE_MACHINE_STEP_ERROR;
    }
    native_machine_step_result result = schedule_native_machine_compound(
        *evaluator, workspace, task, node, status);

    if (result != NATIVE_MACHINE_STEP_LEAF) return result;
    *status = native_evaluate_leaf(
        *evaluator, task->node_index, task->depth,
        task->errexit_suppressed);
    return NATIVE_MACHINE_STEP_STATUS;
}

static bool finish_native_machine_status(
    native_evaluator **evaluator, native_machine_workspace *workspace,
    native_machine_task *task, int *status, bool *child_process)
{
    if (!require(evaluator != NULL && *evaluator != NULL)) return false;
    if (!require(workspace != NULL && task != NULL && status != NULL &&
                 child_process != NULL)) return false;

    if (*status == GSH_EVALUATOR_PIPELINE_CHILD_REQUEST) {
        *task = (native_machine_task){0};
        return take_pipeline_child_request(
            evaluator, workspace, task, child_process);
    }
    if (*status == GSH_EVALUATOR_PIPELINE_EXIT_REQUEST) {
        *status = (*evaluator)->pipeline_exit_status;
        workspace->count = 0U;
        *child_process = true;
        return true;
    }
    if ((*evaluator)->substitution_child_execution != NULL) {
        size_t child_node = 0U;
        size_t child_depth = 0U;
        bool errexit_suppressed = task->errexit_suppressed;

        if (!take_substitution_child(
                evaluator, &child_node, &child_depth)) return false;
        workspace->count = 0U;
        *task = (native_machine_task){
            .kind = NATIVE_MACHINE_NODE,
            .node_index = child_node,
            .depth = child_depth,
            .errexit_suppressed = errexit_suppressed,
        };
        *child_process = true;
        return push_native_machine_task(workspace, task);
    }
    {
        int scheduled = schedule_native_machine_request(
            *evaluator, workspace, status, task->errexit_suppressed);

        return scheduled > 0 ||
               (scheduled == 0 && complete_native_machine_node(
                                      *evaluator, workspace, status,
                                      task->errexit_suppressed));
    }
}

static int native_evaluate_node_inner(native_evaluator *evaluator,
                                      size_t node_index, size_t depth)
{
    if (!require(evaluator != NULL && evaluator->storage != NULL)) return 125;
    if (!require(node_index < evaluator_storage(evaluator)->node_count)) return 125;
    if (!require(depth <= 128U)) return 125;
    native_machine_workspace *workspace =
        acquire_native_machine_workspace(evaluator);
    native_machine_task task = {
        .kind = NATIVE_MACHINE_NODE,
        .node_index = node_index,
        .depth = depth,
    };
    int status = 125;
    bool active = workspace != NULL &&
                  push_native_machine_task(workspace, &task);
    bool child_process = false;
    bool exit_traps_started = false;

    while (active) {
        if (workspace->count == 0U) {
            if (child_process && !exit_traps_started) {
                status = run_native_traps(
                    evaluator, status, NATIVE_TRAPS_EXIT);
                exit_traps_started = true;
                if (status == GSH_EVALUATOR_TRAP_REQUEST) {
                    task = (native_machine_task){0};
                    active = take_native_trap_request(
                        evaluator, workspace, &task);
                    continue;
                }
            }
            active = false;
            continue;
        }
        task = workspace->tasks[--workspace->count];
        if (task.kind != NATIVE_MACHINE_NODE) {
            active = run_native_machine_continuation(
                evaluator, workspace, task, &status);
            continue;
        }
        {
            native_machine_step_result result = run_native_machine_node_task(
                &evaluator, workspace, &task, &status, &child_process);

            if (result == NATIVE_MACHINE_STEP_ERROR) {
                status = 125;
                active = false;
            } else if (result == NATIVE_MACHINE_STEP_STATUS) {
                active = finish_native_machine_status(
                    &evaluator, workspace, &task, &status, &child_process);
            }
        }
    }
    if (child_process) {
        release_native_machine_workspace(evaluator, workspace);
        _exit(status & 255);
    }
    release_native_machine_workspace(evaluator, workspace);
    return status;
}
static bool command_assigns_variable(
    const gsh_native_command *command, const char *name,
    size_t name_length)
{
    if (command == NULL) {
        return false;
    }
    size_t index;

    for (index = 0; index < command->assignment_count; index++) {
        const char *assignment = command->assignments[index];
        const char *separator = strchr(assignment, '=');

        if (separator != NULL &&
            (size_t)(separator - assignment) == name_length &&
            memcmp(assignment, name, name_length) == 0) {
            return true;
        }
    }
    return false;
}

static int copy_variable_entry(gsh_variable_store *destination,
                               const gsh_variable_store *source,
                               size_t index)
{
    if (destination == NULL || source == NULL) {
        return -1;
    }
    const char *assignment;
    const char *separator;
    unsigned int attributes;
    size_t name_length;
    size_t value_length;

    assignment = gsh_variables_assignment(source, index, &attributes);
    separator = assignment == NULL ? NULL : strchr(assignment, '=');
    if (separator == NULL) {
        errno = EPROTO;
        return -1;
    }
    name_length = (size_t)(separator - assignment);
    if (!gsh_variables_is_set(source, index)) {
        return gsh_variables_set_attributes(
            destination, assignment, name_length,
            GSH_VARIABLE_ATTRIBUTE_MASK, attributes);
    }
    value_length = strnlen(separator + 1U, GSH_VARIABLE_VALUE_CAP + 1U);
    if (value_length > GSH_VARIABLE_VALUE_CAP) {
        errno = EPROTO;
        return -1;
    }
    return gsh_variables_set(
        destination, assignment, name_length, separator + 1U,
        value_length, GSH_VARIABLE_ATTRIBUTE_MASK, attributes);
}

static int copy_selected_variables(
    gsh_variable_store *destination, const gsh_variable_store *source,
    const gsh_native_command *command, bool assigned)
{
    if (command == NULL || source == NULL) {
        return -1;
    }
    size_t index;

    for (index = 0; index < gsh_variables_count(source); index++) {
        const char *assignment =
            gsh_variables_assignment(source, index, NULL);
        const char *separator =
            assignment == NULL ? NULL : strchr(assignment, '=');
        bool selected;

        if (separator == NULL) {
            errno = EPROTO;
            return -1;
        }
        selected = command_assigns_variable(
            command, assignment, (size_t)(separator - assignment));
        if (selected == assigned &&
            copy_variable_entry(destination, source, index) == -1) {
            return -1;
        }
    }
    return 0;
}

/* ── Regular-Builtin Prefixes Are an Atomic Variable Overlay ───
 * `command eval` and `command .` suppress special-builtin assignment
 * persistence. The sourced program still runs in the current shell and may
 * mutate every other variable. Rebuilding into a spare fixed store commits
 * those mutations while restoring prefix names even if the temporary value
 * became readonly; failure leaves the caller's store byte-for-byte intact.
 * ─────────────────────────────────────────────────────────────── */
static int merge_temporary_source_variables(
    gsh_variable_store *original, const gsh_variable_store *evaluated,
    gsh_variable_store *scratch, const gsh_native_command *command)
{
    if (command == NULL || evaluated == NULL || original == NULL) {
        return -1;
    }
    bool temporary_path = command_assigns_variable(command, "PATH", 4U);
    uint64_t path_generation =
        temporary_path ? original->path_generation
                       : evaluated->path_generation;

    if (original == evaluated || original == scratch ||
        evaluated == scratch) {
        errno = EINVAL;
        return -1;
    }
    gsh_variables_initialize(scratch);
    if (copy_selected_variables(scratch, evaluated, command, false) == -1 ||
        copy_selected_variables(scratch, original, command, true) == -1) {
        return -1;
    }
    scratch->path_generation = path_generation;
    (void)memcpy(original, scratch, sizeof(*original));
    return 0;
}

static void clear_source_request(native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return;
    }
    evaluator->source_request_active = false;
    evaluator->source_request_dot = false;
    evaluator->source_request_negated = false;
    evaluator->source_request_temporary_variables = false;
    evaluator->source_request_workspace = NULL;
    evaluator->source_request_command = NULL;
    evaluator->source_request_input = NULL;
    evaluator->source_request_input_length = 0;
    evaluator->source_request_root = GSH_AST_NONE;
    evaluator->source_request_saved_count = 0;
}

static bool source_request_is_valid(const native_evaluator *evaluator)
{
    const gsh_source_workspace *workspace;
    bool input_owned;

    if (evaluator == NULL || evaluator->storage == NULL ||
        evaluator->pipeline == NULL) {
        return false;
    }
    workspace = evaluator->source_request_workspace;
    input_owned = workspace != NULL &&
                  (evaluator->source_request_input == workspace->input ||
                   evaluator->source_request_input ==
                       workspace->alias_expansion);

    return evaluator->source_request_active && input_owned &&
           evaluator->source_workspaces != NULL &&
           evaluator->source_depth < GSH_SOURCE_DEPTH_CAP &&
           evaluator->source_depth + 1U == gsh_source_workspaces_depth(
                                                evaluator->source_workspaces) &&
           evaluator->source_request_root < workspace->storage.node_count &&
           evaluator->source_request_input_length <= GSH_SOURCE_INPUT_CAP &&
           evaluator->source_request_saved_count <=
               GSH_NATIVE_REDIRECT_CAP &&
           (!evaluator->source_request_temporary_variables ||
            (evaluator->source_request_command != NULL &&
             evaluator_source_request_command(evaluator)->command_regular_context &&
             evaluator_source_request_command(evaluator)->assignment_count != 0U));
}

static int abandon_source_request(native_evaluator *evaluator)
{
    if (evaluator == NULL) {
        return -1;
    }
    gsh_source_workspace *workspace =
        evaluator->source_request_workspace;
    size_t saved_count = evaluator->source_request_saved_count <=
                                 GSH_NATIVE_REDIRECT_CAP
                             ? evaluator->source_request_saved_count
                             : GSH_NATIVE_REDIRECT_CAP;
    int status = 125;

    if (restore_redirect_descriptors(evaluator->source_request_saved,
                                     saved_count) ==
        -1) {
        perror("gsh: source redirection abandon");
    }
    if (!gsh_source_workspace_release(evaluator->source_workspaces,
                                      workspace)) {
        (void)fputs("gsh: source workspace ownership failure\n", stderr);
    }
    clear_source_request(evaluator);
    evaluator->fatal_error = true;
    return status;
}

static bool enter_source_frame(native_evaluator *evaluator,
                               native_source_frame *frame,
                               size_t *node_index)
{
    const gsh_parse_storage *source_storage;

    if (evaluator == NULL || frame == NULL || node_index == NULL ||
        !source_request_is_valid(evaluator) ||
        evaluator->source_request_workspace == NULL) {
        errno = EINVAL;
        return false;
    }
    frame->input = evaluator->input;
    frame->input_length = evaluator->input_length;
    frame->storage = evaluator->storage;
    frame->pipeline = evaluator->pipeline;
    frame->variables = evaluator->variables;
    frame->scope_base = evaluator->scope_base;
    frame->journal = evaluator->journal;
    frame->source_depth = evaluator->source_depth;
    frame->dot_depth = evaluator->dot_depth;
    frame->workspace = evaluator->source_request_workspace;
    frame->saved_count = evaluator->source_request_saved_count;
    frame->temporary_command = evaluator->source_request_command;
    frame->consume_return = evaluator->source_request_dot;
    frame->negated = evaluator->source_request_negated;
    frame->temporary_variables =
        evaluator->source_request_temporary_variables;
    source_storage = &source_frame_workspace(frame)->storage;
    (void)memcpy(frame->saved, evaluator->source_request_saved,
           frame->saved_count * sizeof(frame->saved[0]));
    evaluator->input = evaluator->source_request_input;
    evaluator->input_length = evaluator->source_request_input_length;
    evaluator->storage = source_storage;
    evaluator->pipeline = &source_frame_workspace(frame)->pipeline;
    if (frame->temporary_variables) {
        evaluator->variables = &source_frame_workspace(frame)->scope_base;
        evaluator->scope_base = &source_frame_workspace(frame)->variables;
        evaluator->journal = NULL;
    }
    evaluator->source_depth++;
    evaluator->dot_depth += frame->consume_return ? 1U : 0U;
    *node_index = evaluator->source_request_root;
    clear_source_request(evaluator);
    if (evaluator->source_depth !=
            gsh_source_workspaces_depth(evaluator->source_workspaces) ||
        *node_index >= source_storage->node_count) {
        errno = EINVAL;
        return false;
    }
    return true;
}

static int leave_source_frame(native_evaluator *evaluator,
                              native_source_frame *frame, int status)
{
    if (evaluator == NULL || frame == NULL) {
        return -1;
    }
    bool merged =
        !frame->temporary_variables ||
        merge_temporary_source_variables(
            frame->variables, evaluator->variables,
            &source_frame_workspace(frame)->variables, frame->temporary_command) == 0;
    bool restored = restore_redirect_descriptors(
                        frame->saved, frame->saved_count) == 0;
    bool released = gsh_source_workspace_release(
        evaluator->source_workspaces, frame->workspace);

    if (evaluator->exiting) {
        status = evaluator->exit_status;
    } else if (frame->consume_return && evaluator->returning) {
        status = evaluator->return_status;
        evaluator->returning = false;
    }
    if (!evaluator->fatal_error && !evaluator->exiting &&
        frame->negated && status != 125) {
        status = status == 0 ? 1 : 0;
    }
    evaluator->dot_depth = frame->dot_depth;
    evaluator->source_depth = frame->source_depth;
    evaluator->pipeline = frame->pipeline;
    evaluator->storage = frame->storage;
    evaluator->journal = frame->journal;
    evaluator->scope_base = frame->scope_base;
    evaluator->variables = frame->variables;
    evaluator->input_length = frame->input_length;
    evaluator->input = frame->input;
    if (!merged || !restored || !released) {
        (void)fputs(!merged
                  ? "gsh: source variable commit failed\n"
                  : !restored
                        ? "gsh: source redirection restore failed\n"
                        : "gsh: source workspace ownership failure\n",
              stderr);
        evaluator->fatal_error = true;
        status = 125;
    }
    if (released) {
        if (evaluator->source_depth != gsh_source_workspaces_depth(
                                           evaluator->source_workspaces)) {
            evaluator->fatal_error = true;
            status = 125;
        }
    }
    return status;
}

static int prepare_pipeline_source_evaluation(
    native_evaluator *parent, gsh_native_pipeline *pipeline,
    size_t command_index, gsh_variable_store *variables,
    bool history_source, pipeline_source_evaluation *evaluation,
    bool *ready)
{
    if (!require(parent != NULL && pipeline != NULL)) return 125;
    if (!require(variables != NULL && evaluation != NULL && ready != NULL)) {
        return 125;
    }
    gsh_saved_descriptor no_saved[GSH_NATIVE_REDIRECT_CAP] = {{0}};
    const gsh_native_command *command;
    int status;

    *ready = false;
    (void)memset(evaluation, 0, sizeof(*evaluation));
    evaluation->root = GSH_AST_NONE;
    if (command_index >= pipeline->command_count) return 125;
    evaluation->child = *parent;
    command = &pipeline->commands[command_index];
    evaluation->child.pipeline = pipeline;
    evaluation->child.variables = variables;
    evaluation->child.journal = NULL;
    evaluation->child.alias_journal = NULL;
    evaluation->child.pipeline_scope = NULL;
    evaluation->child.tail_exec_single = false;
    evaluation->child.exec_outcome_fd = -1;
    evaluation->child.exec_descriptor_socket = -1;
    evaluation->child.fatal_error = false;
    clear_source_request(&evaluation->child);
    if (!history_source) {
        evaluation->backgrounds = allocate_isolated_job_table();
        if (evaluation->backgrounds == NULL) return 125;
        evaluation->child.backgrounds = evaluation->backgrounds;
        evaluation->child.returning = false;
        evaluation->child.exiting = false;
        status = apply_special_builtin_assignments(
            variables, NULL, command, &evaluation->child.options);
    } else {
        status = GSH_ASSIGNMENT_OK;
    }
    if (status == GSH_ASSIGNMENT_OK) {
        status = history_source
                     ? request_fc_source(&evaluation->child, command,
                                         no_saved, 0)
                     : request_builtin_source(&evaluation->child, command,
                                              no_saved, 0);
    } else {
        status = status == GSH_ASSIGNMENT_JOURNAL_ERROR ? 125 : 1;
    }
    if (status != GSH_EVALUATOR_SOURCE_REQUEST) return status;
    evaluation->child.source_request_negated = false;
    if (!source_request_is_valid(&evaluation->child) ||
        !enter_source_frame(&evaluation->child, &evaluation->frame,
                            &evaluation->root)) {
        return abandon_source_request(&evaluation->child);
    }
    *ready = true;
    return 0;
}

static pipeline_child_request *acquire_pipeline_child_request(
    const native_evaluator *evaluator)
{
    static pipeline_child_request requests[NATIVE_MACHINE_WORKSPACE_CAP];
    size_t index;

    if (!require(evaluator != NULL)) return NULL;
    if (!require(evaluator->pipeline_child_request == NULL)) return NULL;
    for (index = 0U; index < NATIVE_MACHINE_WORKSPACE_CAP; index++) {
        if (!requests[index].used) {
            (void)memset(&requests[index], 0, sizeof(requests[index]));
            requests[index].used = true;
            return &requests[index];
        }
    }
    errno = ENOSPC;
    return NULL;
}

static void release_pipeline_child_request(
    native_evaluator *evaluator, pipeline_child_request *request)
{
    if (!require(evaluator != NULL && request != NULL)) return;
    if (!require(request->used)) return;
    (void)memset(request, 0, sizeof(*request));
}

static bool take_pipeline_child_request(
    native_evaluator **evaluator, native_machine_workspace *workspace,
    native_machine_task *task, bool *child_process)
{
    if (!require(evaluator != NULL && *evaluator != NULL)) return false;
    if (!require(workspace != NULL && task != NULL &&
                 child_process != NULL)) return false;
    pipeline_child_request *request = (*evaluator)->pipeline_child_request;
    native_machine_task body = {
        .kind = NATIVE_MACHINE_NODE,
        .errexit_suppressed = request != NULL
                                  ? request->errexit_suppressed
                                  : false,
    };

    if (request == NULL || !request->used) return false;
    (*evaluator)->pipeline_child_request = NULL;
    *evaluator = request->kind == PIPELINE_CHILD_FUNCTION
                     ? &request->child
                     : &request->source.child;
    body.node_index = request->root;
    task->kind = NATIVE_MACHINE_PIPELINE_CHILD_FINISH;
    task->pipeline_child = request;
    workspace->count = 0U;
    *child_process = true;
    return push_native_machine_task(workspace, task) &&
           push_native_machine_task(workspace, &body);
}

static int finish_pipeline_child_request(
    native_evaluator *evaluator, pipeline_child_request *request,
    int status)
{
    if (!require(evaluator != NULL && request != NULL)) return 125;
    if (!require(request->used)) return 125;

    if (request->kind == PIPELINE_CHILD_FUNCTION) {
        status = finish_evaluator_function(
            evaluator, request->function_frame, status);
        if (request->backgrounds != NULL) {
            release_isolated_job_table(request->backgrounds);
        }
    } else {
        status = leave_source_frame(
            evaluator, &request->source.frame, status);
        if (request->source.backgrounds != NULL) {
            release_isolated_job_table(request->source.backgrounds);
        }
    }
    release_pipeline_child_request(evaluator, request);
    return status;
}

/* ── Pipeline-Local Evaluation Has One Isolation Driver ─────────
 * Functions, dot, eval, and fc once maintained parallel child evaluators
 * whose cleanup rules could drift.  One concrete tag now selects the bounded
 * setup while a single owner releases the isolated job state after execution.
 * Function redirects and source frames still use their specialized owners;
 * all mutations remain confined to the already-forked pipeline process.
 * ─────────────────────────────────────────────────────────────── */
static int request_pipeline_isolated_function(
    native_evaluator *parent, gsh_native_pipeline *pipeline,
    size_t command_index, gsh_variable_store *variables,
    bool errexit_suppressed, bool *handled)
{
    if (!require(parent != NULL && pipeline != NULL)) return 125;
    if (!require(variables != NULL && handled != NULL)) return 125;
    const gsh_native_command *command = &pipeline->commands[command_index];
    const gsh_function_entry *entry = evaluator_function(parent, command);
    pipeline_child_request *request;
    int status;

    if (entry == NULL) return 127;
    *handled = true;
    request = acquire_pipeline_child_request(parent);
    if (request == NULL) return 125;
    request->kind = PIPELINE_CHILD_FUNCTION;
    request->errexit_suppressed = errexit_suppressed;
    request->backgrounds = allocate_isolated_job_table();
    if (request->backgrounds == NULL) {
        release_pipeline_child_request(parent, request);
        return 125;
    }
    request->child = *parent;
    request->child.pipeline = pipeline;
    request->child.variables = variables;
    request->child.journal = NULL;
    request->child.alias_journal = NULL;
    request->child.pipeline_scope = NULL;
    request->child.tail_exec_single = false;
    request->child.exec_outcome_fd = -1;
    request->child.returning = false;
    request->child.exiting = false;
    request->child.fatal_error = false;
    request->child.backgrounds = request->backgrounds;
    status = request_evaluator_function(
        &request->child, command, entry, false, false);
    if (status == GSH_EVALUATOR_FUNCTION_REQUEST) {
        request->function_frame = request->child.function_request_frame;
        request->root = request->child.function_request_root;
        request->child.function_request_frame = NULL;
        parent->pipeline_child_request = request;
        return GSH_EVALUATOR_PIPELINE_CHILD_REQUEST;
    }
    release_isolated_job_table(request->backgrounds);
    release_pipeline_child_request(parent, request);
    return status;
}

static int request_pipeline_isolated_source(
    native_evaluator *parent, gsh_native_pipeline *pipeline,
    size_t command_index, gsh_variable_store *variables,
    pipeline_isolated_kind kind, bool errexit_suppressed)
{
    if (!require(parent != NULL && pipeline != NULL)) return 125;
    if (!require(variables != NULL)) return 125;
    pipeline_child_request *request =
        acquire_pipeline_child_request(parent);
    bool ready = false;
    int status;

    if (request == NULL) return 125;
    request->kind = PIPELINE_CHILD_SOURCE;
    request->errexit_suppressed = errexit_suppressed;
    status = prepare_pipeline_source_evaluation(
        parent, pipeline, command_index, variables,
        kind == PIPELINE_ISOLATED_FC, &request->source, &ready);
    if (ready) {
        request->root = request->source.root;
        parent->pipeline_child_request = request;
        return GSH_EVALUATOR_PIPELINE_CHILD_REQUEST;
    }
    if (request->source.backgrounds != NULL) {
        release_isolated_job_table(request->source.backgrounds);
    }
    release_pipeline_child_request(parent, request);
    return status;
}

static int run_pipeline_isolated_command(
    native_evaluator *parent, gsh_native_pipeline *pipeline,
    size_t command_index, gsh_variable_store *variables,
    pipeline_isolated_kind kind, bool errexit_suppressed, bool *handled)
{
    if (!require(parent != NULL && pipeline != NULL)) return 125;
    if (!require(variables != NULL && handled != NULL)) return 125;
    *handled = false;
    if (command_index >= pipeline->command_count) return 125;
    if (kind == PIPELINE_ISOLATED_FUNCTION) {
        return request_pipeline_isolated_function(
            parent, pipeline, command_index, variables,
            errexit_suppressed, handled);
    }
    if (kind != PIPELINE_ISOLATED_FC &&
        kind != PIPELINE_ISOLATED_SOURCE) {
        return 125;
    }
    *handled = true;
    return request_pipeline_isolated_source(
        parent, pipeline, command_index, variables, kind,
        errexit_suppressed);
}

static bool bounded_job_command(const char *input, size_t input_length,
                                const gsh_ast_node *node,
                                const char **command, size_t *length)
{
    size_t begin;
    size_t end;

    if (input == NULL || node == NULL || command == NULL || length == NULL ||
        node->begin > node->end || node->end > input_length) {
        return false;
    }
    begin = node->begin;
    end = node->end;
    while (begin < end && (input[begin] == ' ' || input[begin] == '\t' ||
                           input[begin] == '\n')) begin++;
    while (end > begin && (input[end - 1U] == ' ' ||
                           input[end - 1U] == '\t' ||
                           input[end - 1U] == '\n' ||
                           input[end - 1U] == '&')) end--;
    if (end - begin >= GSH_BACKGROUND_COMMAND_CAP) return false;
    *command = input + begin;
    *length = end - begin;
    return true;
}

static native_async_start start_native_async(
    native_evaluator *evaluator, size_t node_index,
    native_evaluator *child, int *status)
{
    if (!require(evaluator != NULL && child != NULL)) {
        return NATIVE_ASYNC_ERROR;
    }
    if (!require(status != NULL && evaluator->storage != NULL)) {
        return NATIVE_ASYNC_ERROR;
    }
    const char *command;
    size_t command_length;
    pid_t pid;

    if (evaluator->backgrounds == NULL ||
        !gsh_background_has_capacity(evaluator->backgrounds) ||
        node_index >= evaluator_storage(evaluator)->node_count ||
        !bounded_job_command(
            evaluator->input, evaluator->input_length,
            &evaluator_storage(evaluator)->nodes[node_index], &command,
            &command_length)) {
        errno = EAGAIN;
        perror("gsh: asynchronous list");
        *status = 125;
        return NATIVE_ASYNC_ERROR;
    }
    pid = gsh_fault_should_fail(GSH_FAULT_ASYNC_FORK, EAGAIN) ? -1 : fork();
    if (pid == 0) {
        gsh_background_table *child_backgrounds;
        int null_descriptor;

        (void)setpgid(0, 0);
        reset_child_signals();
        *child = *evaluator;
        enter_native_subshell_or_exit(child);
        null_descriptor = open("/dev/null", O_RDONLY);
        if (null_descriptor == -1 ||
            child_duplicate_descriptor(null_descriptor, STDIN_FILENO) ==
                -1) {
            child_exec_error("asynchronous standard input", errno);
        }
        if (null_descriptor != STDIN_FILENO) {
            (void)close(null_descriptor);
        }
        child_backgrounds = allocate_isolated_job_table();
        if (child_backgrounds == NULL) {
            child_exec_error("asynchronous job state", errno);
        }
        child->backgrounds = child_backgrounds;
        child->last_background_pid = 0;
        child->journal = NULL;
        child->alias_journal = NULL;
        child->active_loops = 0;
        child->loop_control = NATIVE_LOOP_CONTROL_NONE;
        child->loop_levels = 0;
        child->tail_exec_single =
            async_node_has_single_pipeline(evaluator, node_index);
        close_evaluator_exec_transaction(child);
        return NATIVE_ASYNC_CHILD;
    }
    if (pid == -1) {
        perror("gsh: asynchronous fork");
        *status = 125;
        return NATIVE_ASYNC_ERROR;
    }
    (void)setpgid(pid, pid);
    if (gsh_background_add_job(
            evaluator->backgrounds, pid, pid, &pid, 1U, 1U, false,
            command, command_length, GSH_JOB_ORIGIN_EVALUATOR,
            NULL) == -1) {
        int saved_errno = errno;

        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) == -1 && errno == EINTR) {
        }
        errno = saved_errno;
        perror("gsh: asynchronous registry");
        *status = 125;
        return NATIVE_ASYNC_ERROR;
    }
    evaluator->last_background_pid = (long)pid;
    *status = 0;
    return NATIVE_ASYNC_PARENT;
}

typedef struct {
    const char *input;
    size_t input_length;
    const gsh_parse_storage *storage;
    gsh_native_pipeline *pipeline;
    gsh_variable_store *variables;
    gsh_function_store *functions;
    gsh_function_store *function_scratch;
    size_t source_depth;
    bool preflight;
} native_trap_frame;

static void enter_native_trap_frame(native_evaluator *evaluator,
                                    gsh_source_workspace *workspace,
                                    const char *input,
                                    size_t input_length,
                                    native_trap_frame *frame)
{
    if (evaluator == NULL || frame == NULL || input == NULL || workspace == NULL) {
        return;
    }
    frame->input = evaluator->input;
    frame->input_length = evaluator->input_length;
    frame->storage = evaluator->storage;
    frame->pipeline = evaluator->pipeline;
    frame->variables = evaluator->variables;
    frame->functions = evaluator->functions;
    frame->function_scratch = evaluator->function_scratch;
    frame->source_depth = evaluator->source_depth;
    frame->preflight = evaluator->preflight;
    evaluator->input = input;
    evaluator->input_length = input_length;
    evaluator->storage = &workspace->storage;
    evaluator->pipeline = &workspace->pipeline;
    evaluator->source_depth++;
}

static void leave_native_trap_frame(native_evaluator *evaluator,
                                    const native_trap_frame *frame)
{
    if (evaluator == NULL || frame == NULL) {
        return;
    }
    evaluator->preflight = frame->preflight;
    evaluator->function_scratch = frame->function_scratch;
    evaluator->functions = frame->functions;
    evaluator->variables = frame->variables;
    evaluator->source_depth = frame->source_depth;
    evaluator->pipeline = frame->pipeline;
    evaluator->storage = frame->storage;
    evaluator->input_length = frame->input_length;
    evaluator->input = frame->input;
}

static bool preflight_native_trap_action(
    native_evaluator *evaluator, gsh_source_workspace *workspace,
    size_t root, const native_trap_frame *frame)
{
    if (evaluator == NULL || frame == NULL || workspace == NULL) {
        return false;
    }
    bool defines_functions = storage_has_function(&workspace->storage);
    bool supported;

    (void)memcpy(&workspace->variables, frame->variables,
           sizeof(workspace->variables));
    evaluator->variables = &workspace->variables;
    if (defines_functions) {
        if (frame->functions == NULL) {
            gsh_functions_initialize(&workspace->functions);
        } else {
            (void)memcpy(&workspace->functions, frame->functions,
                   sizeof(workspace->functions));
        }
        gsh_functions_initialize(&workspace->function_scratch);
        evaluator->functions = &workspace->functions;
        evaluator->function_scratch = &workspace->function_scratch;
    }
    evaluator->preflight = true;
    supported = native_preflight_node(evaluator, root, 0);
    evaluator->preflight = false;
    evaluator->function_scratch = frame->function_scratch;
    evaluator->functions = frame->functions;
    evaluator->variables = frame->variables;
    return supported;
}

typedef struct {
    gsh_source_workspace *workspace;
    native_trap_frame frame;
    size_t root;
    bool entered;
} native_trap_execution;

static gsh_source_workspace *trap_execution_workspace(
    const native_trap_execution *execution)
{
    if (!require(execution != NULL)) return NULL;
    if (!require(execution->workspace != NULL)) return NULL;
    return execution->workspace;
}

struct native_trap_request {
    native_trap_execution execution;
    native_trap_run_kind kind;
    int prior_status;
    int saved_exit_status;
    bool was_exiting;
    bool was_fatal;
    bool used;
};

static native_trap_request *acquire_native_trap_request(
    const native_evaluator *evaluator)
{
    static native_trap_request requests[GSH_TRAP_CONDITION_CAP];
    size_t index;

    if (!require(evaluator != NULL)) return NULL;
    if (!require(evaluator->trap_request == NULL)) return NULL;
    for (index = 0U; index < GSH_TRAP_CONDITION_CAP; index++) {
        if (!requests[index].used) {
            (void)memset(&requests[index], 0, sizeof(requests[index]));
            requests[index].used = true;
            return &requests[index];
        }
    }
    errno = ENOSPC;
    return NULL;
}

static void release_native_trap_request(
    native_evaluator *evaluator, native_trap_request *request)
{
    if (!require(evaluator != NULL && request != NULL)) return;
    if (!require(request->used)) return;
    (void)memset(request, 0, sizeof(*request));
}

static int prepare_native_trap_execution(
    native_evaluator *evaluator, size_t condition, int prior_status,
    native_trap_execution *execution, bool *ready)
{
    if (!require(evaluator != NULL && execution != NULL)) return 125;
    if (!require(ready != NULL && evaluator->traps != NULL)) return 125;
    gsh_parse_result parsed;
    const char *action;
    const char *input;
    size_t action_length;
    size_t input_length;

    (void)memset(execution, 0, sizeof(*execution));
    *ready = false;
    action = gsh_traps_action(evaluator->traps, condition, &action_length);
    if (action == NULL || action_length > GSH_SOURCE_INPUT_CAP ||
        evaluator->source_depth != gsh_source_workspaces_depth(
                                       evaluator->source_workspaces)) {
        if (action != NULL) {
            (void)fputs("gsh: trap source workspace ownership failure\n", stderr);
            evaluator->fatal_error = true;
        }
        return action == NULL ? prior_status : 125;
    }
    execution->workspace =
        gsh_fault_should_fail(GSH_FAULT_TRAP_WORKSPACE_EXHAUSTION, EAGAIN)
            ? NULL
            : gsh_source_workspace_acquire(evaluator->source_workspaces);
    if (execution->workspace == NULL) {
        (void)fputs("gsh: trap source workspace limit exceeded\n", stderr);
        evaluator->fatal_error = true;
        return 125;
    }
    (void)memcpy(trap_execution_workspace(execution)->input, action, action_length);
    trap_execution_workspace(execution)->input[action_length] = '\0';
    input = trap_execution_workspace(execution)->input;
    input_length = action_length;
    parsed = gsh_alias_parse(
        trap_execution_workspace(execution)->input, action_length, evaluator->aliases,
        trap_execution_workspace(execution)->alias_expansion, GSH_ALIAS_EXPANSION_CAP,
        &trap_execution_workspace(execution)->storage, &input, &input_length);
    enter_native_trap_frame(evaluator, execution->workspace, input,
                            input_length, &execution->frame);
    execution->entered = true;
    evaluator->last_status = prior_status;
    if (parsed.status != GSH_PARSE_OK) {
        (void)fprintf(stderr, "gsh: trap action %s at byte %zu\n",
                gsh_parse_status_name(parsed.status), parsed.error_offset);
        evaluator->fatal_error = true;
        return 2;
    }
    if (!preflight_native_trap_action(
            evaluator, execution->workspace, parsed.root,
            &execution->frame)) {
        (void)fputs("gsh: trap action execution unsupported\n", stderr);
        evaluator->fatal_error = true;
        return 2;
    }
    execution->root = parsed.root;
    *ready = true;
    return prior_status;
}

static int finish_native_trap_execution(
    native_evaluator *evaluator, native_trap_execution *execution,
    int status)
{
    if (!require(evaluator != NULL && execution != NULL)) return 125;
    if (!require(!execution->entered || execution->workspace != NULL)) {
        return 125;
    }
    if (execution->entered) {
        leave_native_trap_frame(evaluator, &execution->frame);
    }
    if (execution->workspace != NULL &&
        !gsh_source_workspace_release(evaluator->source_workspaces,
                                      execution->workspace)) {
        (void)fputs("gsh: trap source workspace ownership failure\n", stderr);
        evaluator->fatal_error = true;
        status = 125;
    }
    if (evaluator->source_depth != gsh_source_workspaces_depth(
                                       evaluator->source_workspaces)) {
        return 125;
    }
    return status;
}

/* ── One Trap Driver Owns Pending and EXIT Re-entry ──────────────
 * Pending conditions and EXIT once passed through separate wrappers around
 * the same parsed-action evaluator.  Those wrappers obscured a recursive
 * call-graph edge and let state restoration rules evolve independently.
 * A concrete mode now selects bounded pending dispatch or one EXIT action;
 * the same loop owns each nested source frame and its deterministic unwind.
 * ─────────────────────────────────────────────────────────────── */
static int run_native_traps(native_evaluator *evaluator, int status,
                            native_trap_run_kind kind)
{
    if (!require(evaluator != NULL)) return 125;
    if (!require(kind == NATIVE_TRAPS_PENDING ||
                 kind == NATIVE_TRAPS_EXIT)) return 125;
    native_trap_request *request;
    size_t condition = 0U;
    bool ready = false;
    int action_status;

    if (evaluator->traps == NULL || evaluator->exit_trap_running) {
        return status;
    }
    if (kind == NATIVE_TRAPS_PENDING &&
        (evaluator->preflight || evaluator->exiting ||
         !gsh_traps_have_pending(evaluator->traps))) {
        return status;
    }
    if (kind == NATIVE_TRAPS_EXIT &&
        (evaluator->exit_trap_complete ||
         gsh_traps_state(evaluator->traps, 0U) != GSH_TRAP_ACTION)) {
        return status;
    }
    if (kind == NATIVE_TRAPS_PENDING &&
        !gsh_traps_take_pending(evaluator->traps, &condition)) return status;
    request = acquire_native_trap_request(evaluator);
    if (request == NULL) return 125;
    request->kind = kind;
    request->prior_status = status;
    request->was_exiting = evaluator->exiting;
    request->was_fatal = evaluator->fatal_error;
    request->saved_exit_status = evaluator->exit_status;
    if (kind == NATIVE_TRAPS_EXIT) {
        evaluator->exit_trap_running = true;
        evaluator->exiting = false;
        evaluator->fatal_error = false;
        evaluator->last_status = status;
    }
    action_status = prepare_native_trap_execution(
        evaluator, condition, status, &request->execution, &ready);
    if (ready) {
        evaluator->trap_request = request;
        return GSH_EVALUATOR_TRAP_REQUEST;
    }
    action_status = finish_native_trap_execution(
        evaluator, &request->execution, action_status);
    if (kind == NATIVE_TRAPS_PENDING) {
        evaluator->last_status = status;
        release_native_trap_request(evaluator, request);
        return evaluator->exiting ? evaluator->exit_status
             : evaluator->fatal_error ? 2
                                      : status;
    }
    evaluator->exit_trap_complete = true;
    evaluator->exit_trap_running = false;
    if (evaluator->exiting) {
        status = evaluator->exit_status;
    } else if (evaluator->fatal_error && action_status != status) {
        status = action_status;
        evaluator->exiting = request->was_exiting;
        evaluator->exit_status = status;
    } else {
        evaluator->exiting = request->was_exiting;
        evaluator->exit_status = request->saved_exit_status;
    }
    evaluator->fatal_error = request->was_fatal || evaluator->fatal_error;
    evaluator->last_status = status;
    release_native_trap_request(evaluator, request);
    return status;
}

static bool take_native_trap_request(
    native_evaluator *evaluator, native_machine_workspace *workspace,
    native_machine_task *task)
{
    if (!require(evaluator != NULL && workspace != NULL)) return false;
    if (!require(task != NULL)) return false;
    native_trap_request *request = evaluator->trap_request;
    native_machine_task body = {.kind = NATIVE_MACHINE_NODE};

    if (request == NULL || !request->used) return false;
    evaluator->trap_request = NULL;
    task->kind = NATIVE_MACHINE_TRAP_FINISH;
    task->trap_request = request;
    body.node_index = request->execution.root;
    return push_native_machine_task(workspace, task) &&
           push_native_machine_task(workspace, &body);
}

static int finish_native_trap_request(
    native_evaluator *evaluator, native_trap_request *request,
    int status)
{
    if (!require(evaluator != NULL && request != NULL)) return 125;
    if (!require(request->used)) return 125;
    int action_status = finish_native_trap_execution(
        evaluator, &request->execution, status);
    int result = request->prior_status;

    if (request->kind == NATIVE_TRAPS_PENDING) {
        evaluator->last_status = result;
        if (evaluator->exiting) result = evaluator->exit_status;
        else if (evaluator->fatal_error) result = 2;
    } else {
        evaluator->exit_trap_complete = true;
        evaluator->exit_trap_running = false;
        if (evaluator->exiting) {
            result = evaluator->exit_status;
        } else if (evaluator->fatal_error &&
                   action_status != request->prior_status) {
            result = action_status;
            evaluator->exiting = request->was_exiting;
            evaluator->exit_status = result;
        } else {
            evaluator->exiting = request->was_exiting;
            evaluator->exit_status = request->saved_exit_status;
        }
        evaluator->fatal_error =
            request->was_fatal || evaluator->fatal_error;
        evaluator->last_status = result;
    }
    release_native_trap_request(evaluator, request);
    return result;
}

static int evaluate_native_traps_top(native_evaluator *evaluator,
                                     int status,
                                     native_trap_run_kind kind)
{
    if (!require(evaluator != NULL)) return 125;
    if (!require(kind == NATIVE_TRAPS_PENDING ||
                 kind == NATIVE_TRAPS_EXIT)) return 125;
    size_t dispatch;

    for (dispatch = 0U; dispatch <= GSH_TRAP_CONDITION_CAP; dispatch++) {
        native_trap_request *request;
        int action_status;

        status = run_native_traps(evaluator, status, kind);
        if (status != GSH_EVALUATOR_TRAP_REQUEST) return status;
        request = evaluator->trap_request;
        if (request == NULL) return 125;
        evaluator->trap_request = NULL;
        action_status = native_evaluate_node(
            evaluator, request->execution.root, 0U);
        status = finish_native_trap_request(
            evaluator, request, action_status);
        if (kind == NATIVE_TRAPS_EXIT) return status;
    }
    return 125;
}

static bool take_substitution_child(
    native_evaluator **evaluator, size_t *node_index, size_t *depth)
{
    if (!require(evaluator != NULL && *evaluator != NULL)) return false;
    if (!require(node_index != NULL && depth != NULL)) return false;
    command_substitution_execution *execution =
        (*evaluator)->substitution_child_execution;

    if (execution == NULL) return false;
    (*evaluator)->substitution_child_execution = NULL;
    *evaluator = &execution->nested;
    *node_index = execution->root;
    *depth = 0U;
    return true;
}

static int native_evaluate_node(native_evaluator *evaluator,
                                size_t node_index, size_t depth)
{
    if (!require(evaluator != NULL && evaluator->storage != NULL)) return 125;
    if (!require(node_index < evaluator_storage(evaluator)->node_count)) return 125;
    if (!require(evaluator_storage(evaluator)->node_count <= GSH_PARSE_NODE_CAP)) {
        return 125;
    }
    if (!require(depth <= 128U)) return 125;
    return native_evaluate_node_inner(evaluator, node_index, depth);
}

static void clear_pending_native_commit(shell_state *state)
{
    if (!require(state != NULL)) return;
    if (!require(state->pending_exec_descriptor_count <=
                 GSH_EXEC_DESCRIPTOR_COMMIT_CAP)) {
        state->pending_exec_descriptor_count = 0;
    }
    state->pending_directory_commit = false;
    state->pending_positional_commit = false;
    state->pending_alias_commit = false;
    state->pending_function_commit = false;
    state->pending_command_cache_commit = false;
    state->pending_job_service = false;
    state->pending_exec_possible = false;
    state->pending_exec_descriptor_count = 0;
    state->pending_exec_protected_descriptor_count = 0;
}

static bool prepare_preflight_functions(shell_state *state,
                                        bool definitions)
{
    if (!require(state != NULL)) return false;
    if (!require(state->parse_storage != NULL)) return false;
    if (!definitions) return true;
    if (ensure_function_state(state, true) == -1 ||
        !gsh_functions_clone(state->function_scratch, state->functions)) {
        state->pending_function_commit = false;
        return false;
    }
    return true;
}

static void initialize_native_preflight_evaluator(
    shell_state *state, native_evaluator *evaluator, bool definitions)
{
    if (!require(state != NULL)) return;
    if (!require(evaluator != NULL)) return;
    (void)memset(evaluator, 0, sizeof(*evaluator));
    (void)memcpy(state->variable_scratch, state->variables,
           sizeof(*state->variable_scratch));
    evaluator->input = state->pending_input;
    evaluator->input_length = state->pending_input_length;
    evaluator->storage = state->parse_storage;
    evaluator->pipeline = state->native_pipeline;
    evaluator->default_path = state->default_path;
    evaluator->last_status = state->last_status;
    evaluator->shell_pid = (long)state->shell_pgid;
    evaluator->last_background_pid = state->last_background_pid;
    evaluator->parameter_zero = state->parameter_zero;
    evaluator->history = state->history;
    evaluator->history_exclude_newest = true;
    evaluator->positionals = state->positionals;
    evaluator->options = state->options;
    evaluator->variables = state->variable_scratch;
    evaluator->functions = definitions ? state->function_scratch
                                       : state->functions;
    evaluator->function_scratch = definitions ? NULL
                                               : state->function_scratch;
    evaluator->aliases = state->aliases;
    evaluator->command_cache = state->command_cache;
    evaluator->command_cache_base_generation =
        state->command_cache_generation;
    evaluator->scope_base = state->pipeline_variables;
    evaluator->scope_changes = state->pipeline_changes;
    evaluator->source_workspaces = state->source_workspaces;
    evaluator->preflight = true;
    evaluator->job_service_socket = -1;
    evaluator->exec_outcome_fd = -1;
    evaluator->exec_descriptor_socket = -1;
    evaluator->backgrounds = &state->background_jobs;
}

static void capture_pending_native_commit(shell_state *state,
                                          const native_evaluator *evaluator)
{
    if (!require(state != NULL)) return;
    if (!require(evaluator != NULL)) return;
    state->pending_directory_commit =
        evaluator->directory_mutation_possible;
    state->pending_positional_commit =
        evaluator->positional_mutation_possible;
    state->pending_alias_commit = evaluator->alias_mutation_possible;
    state->pending_function_commit = evaluator->function_mutation_possible;
    state->pending_command_cache_commit =
        evaluator->command_cache_mutation_possible;
    state->pending_job_service = evaluator->job_service_possible;
    state->pending_exec_possible = evaluator->exec_possible;
    state->pending_exec_descriptor_count = evaluator->exec_descriptor_count;
    if (state->pending_exec_descriptor_count >
        GSH_EXEC_DESCRIPTOR_COMMIT_CAP) {
        state->pending_exec_descriptor_count = 0U;
        state->pending_exec_possible = false;
        return;
    }
    (void)memcpy(state->pending_exec_descriptors, evaluator->exec_descriptors,
           evaluator->exec_descriptor_count *
               sizeof(evaluator->exec_descriptors[0]));
    state->pending_exec_protected_descriptor_count =
        evaluator->exec_protected_descriptor_count;
    if (state->pending_exec_protected_descriptor_count >
        GSH_EXEC_DESCRIPTOR_COMMIT_CAP) {
        state->pending_exec_protected_descriptor_count = 0U;
        state->pending_exec_possible = false;
        return;
    }
    (void)memcpy(state->pending_exec_protected_descriptors,
           evaluator->exec_protected_descriptors,
           evaluator->exec_protected_descriptor_count *
               sizeof(evaluator->exec_protected_descriptors[0]));
}

static bool native_node_is_supported(shell_state *state, size_t node_index)
{
    native_evaluator evaluator = {0};
    bool definitions;

    if (!require(state != NULL)) return false;
    if (!require(state->parse_storage != NULL)) return false;
    definitions = storage_has_function(state->parse_storage);
    if (!prepare_preflight_functions(state, definitions)) return false;
    initialize_native_preflight_evaluator(state, &evaluator, definitions);
    state->pending_positional_commit =
        state_parse_storage(state)->node_count > node_index &&
        native_preflight_node(&evaluator, node_index, 0);
    if (!state->pending_positional_commit) {
        clear_pending_native_commit(state);
        return false;
    }
    capture_pending_native_commit(state, &evaluator);
    return true;
}

static bool native_command_is_supported(shell_state *state)
{
    if (state == NULL) {
        return false;
    }
    return native_node_is_supported(state, state->pending_parse.root);
}

enum { GSH_REACTOR_EVALUATION_BUDGET = 16 };

static bool reactor_literal_word(const char *input, gsh_word_ref word)
{
    if (input == NULL) {
        return false;
    }
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
    if (input == NULL || text == NULL) {
        return false;
    }
    size_t length = strlen(text);

    return word.end - word.begin == length &&
           memcmp(input + word.begin, text, length) == 0;
}

static bool literal_command_word_is(const char *input, gsh_word_ref word,
                                    const char *text)
{
    if (text == NULL) return false;
    if (input == NULL) {
        return false;
    }
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

static bool protected_builtin_literal(const char *input, gsh_word_ref word)
{
    size_t index;

    for (index = 0; index < gsh_builtin_descriptor_count(); index++) {
        const gsh_builtin_descriptor *descriptor =
            gsh_builtin_descriptor_at(index);
        bool protected = descriptor != NULL &&
                         descriptor->kind >= GSH_BUILTIN_ECHO &&
                         descriptor->kind <= GSH_BUILTIN_KILL;

        if (protected &&
            literal_command_word_is(input, word, descriptor->name)) {
            return true;
        }
    }
    return false;
}

/* A compatibility hand-off cannot inspect the expanded command name without
 * already delegating it.  Reject literal protected names anywhere in the
 * unsupported command, including command wrappers, and reject dynamic words
 * conservatively because they could expand to one of those names. */
static bool fallback_mentions_protected_builtin(const char *input,
                                                size_t length)
{
    gsh_lexer lexer;
    gsh_token token;

    if (input == NULL) return false;
    gsh_lexer_init(&lexer, input, length);
    for (size_t step = 0; step <= GSH_PARSE_TOKEN_CAP; step++) {
        gsh_lex_status status = gsh_lexer_next(&lexer, &token);

        if (status != GSH_LEX_OK || token.kind == GSH_TOKEN_EOF) break;
        if (token.kind == GSH_TOKEN_WORD) {
            gsh_word_ref word = {token.begin, token.end};
            size_t offset;

            if (protected_builtin_literal(input, word)) return true;
            for (offset = word.begin; offset < word.end; offset++) {
                if (input[offset] == '$' ||
                    (unsigned char)input[offset] == 0x60U) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool reactor_for_budget(shell_state *state,
                               const gsh_ast_node *node,
                               size_t body_budget, size_t *budget)
{
    if (node == NULL) return false;
    if (budget == NULL || state == NULL) {
        return false;
    }
    size_t iterations;
    size_t index;
    size_t maximum_value_length = 0U;
    size_t old_length = 0U;
    size_t new_length;
    gsh_word_ref name;
    bool is_set;
    unsigned int attributes;
    bool exists;

    if ((node->flags & GSH_AST_FLAG_FOR_HAS_IN) == 0 ||
        node->word_count == 0U || node->first_child == GSH_AST_NONE) {
        return false;
    }
    name = state_parse_storage(state)->words[node->first_word];
    exists = gsh_variables_get_state(
        state->variables, state->pending_input + name.begin,
        name.end - name.begin, &is_set, &attributes);
    if (exists && (attributes & GSH_VARIABLE_READONLY) != 0) {
        return false;
    }
    for (index = 1U; index < node->word_count; index++) {
        gsh_word_ref item =
            state_parse_storage(state)->words[node->first_word + index];
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
    if (iterations != 0U) {
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
        } else if (state_variables(state)->count == GSH_VARIABLE_CAP) {
            return false;
        }
        new_length = name_length + maximum_value_length + 2U;
        if (new_length > old_length + GSH_VARIABLE_TEXT_CAP -
                                       state_variables(state)->text_used) {
            return false;
        }
        if (body_budget + 1U >
                (GSH_REACTOR_EVALUATION_BUDGET - 1U) / iterations) {
            return false;
        }
    }
    *budget = 1U + iterations * (body_budget + 1U);
    return *budget <= GSH_REACTOR_EVALUATION_BUDGET;
}

static bool reactor_compute_node(shell_state *state, size_t node_index,
                                 const size_t budgets[GSH_PARSE_NODE_CAP],
                                 const bool valid[GSH_PARSE_NODE_CAP],
                                 const bool nested_for[GSH_PARSE_NODE_CAP],
                                 size_t *budget, bool *contains_for)
{
    if (budget == NULL || budgets == NULL || contains_for == NULL || nested_for == NULL || state == NULL || valid == NULL) {
        return false;
    }
    const gsh_ast_node *node = &state_parse_storage(state)->nodes[node_index];
    size_t child;
    size_t total = 0U;
    size_t visited = 0U;

    *contains_for = false;
    if (node->kind == GSH_AST_SIMPLE) {
        gsh_word_ref word;

        if (node->word_count != 1U) return false;
        word = state_parse_storage(state)->words[node->first_word];
        if (!reactor_word_is(state->pending_input, word, ":") &&
            !reactor_word_is(state->pending_input, word, "true") &&
            !reactor_word_is(state->pending_input, word, "false")) {
            return false;
        }
        *budget = 1U;
        return true;
    }
    child = node->first_child;
    if (node->kind == GSH_AST_PIPELINE) {
        if (child == GSH_AST_NONE ||
            child >= state_parse_storage(state)->node_count || !valid[child] ||
            state_parse_storage(state)->nodes[child].next_sibling != GSH_AST_NONE) {
            return false;
        }
        *budget = budgets[child];
        *contains_for = nested_for[child];
        return true;
    }
    if (node->kind == GSH_AST_FOR) {
        if (child == GSH_AST_NONE ||
            child >= state_parse_storage(state)->node_count || !valid[child]) {
            return false;
        }
        *contains_for = true;
        return reactor_for_budget(state, node, budgets[child], budget);
    }
    if (node->kind != GSH_AST_PROGRAM && node->kind != GSH_AST_LIST &&
        node->kind != GSH_AST_AND_OR) {
        return false;
    }
    while (child != GSH_AST_NONE && visited < state_parse_storage(state)->node_count) {
        if (child >= state_parse_storage(state)->node_count || !valid[child] ||
            budgets[child] > GSH_REACTOR_EVALUATION_BUDGET - total) {
            return false;
        }
        total += budgets[child];
        *contains_for = *contains_for || nested_for[child];
        child = state_parse_storage(state)->nodes[child].next_sibling;
        visited++;
    }
    if (child != GSH_AST_NONE) return false;
    *budget = total;
    return total != 0U;
}

static bool reactor_safe_node(shell_state *state, size_t node_index,
                              size_t *budget, bool *contains_for)
{
    if (budget == NULL || contains_for == NULL) {
        return false;
    }
    typedef struct {
        size_t node;
        size_t next_child;
        size_t depth;
        bool entered;
    } reactor_frame;
    static reactor_frame frames[GSH_PARSE_NODE_CAP];
    static size_t budgets[GSH_PARSE_NODE_CAP];
    static bool nested_for[GSH_PARSE_NODE_CAP];
    static bool valid[GSH_PARSE_NODE_CAP];
    size_t frame_count = 1U;
    size_t steps;

    if (node_index >= state_parse_storage(state)->node_count) return false;
    (void)memset(valid, 0, sizeof(valid));
    frames[0] = (reactor_frame){node_index, GSH_AST_NONE, 0U, false};
    for (steps = 0;
         frame_count > 0U && steps < 2U * GSH_PARSE_NODE_CAP; steps++) {
        reactor_frame *frame = &frames[frame_count - 1U];
        const gsh_ast_node *node = &state_parse_storage(state)->nodes[frame->node];

        if (!frame->entered) {
            if (frame->depth > 32U ||
                (node->flags & GSH_AST_FLAG_ASYNC) != 0 ||
                node->redirect_count != 0U) return false;
            frame->next_child = node->first_child;
            frame->entered = true;
        } else if (frame->next_child != GSH_AST_NONE) {
            size_t child = frame->next_child;

            if (child >= state_parse_storage(state)->node_count ||
                frame_count == GSH_PARSE_NODE_CAP) return false;
            frame->next_child =
                node->kind == GSH_AST_PIPELINE || node->kind == GSH_AST_FOR
                    ? GSH_AST_NONE
                    : state_parse_storage(state)->nodes[child].next_sibling;
            frames[frame_count] = (reactor_frame){
                child, GSH_AST_NONE, frame->depth + 1U, false};
            frame_count++;
        } else {
            valid[frame->node] = reactor_compute_node(
                state, frame->node, budgets, valid, nested_for,
                &budgets[frame->node], &nested_for[frame->node]);
            if (!valid[frame->node]) return false;
            frame_count--;
        }
    }
    if (frame_count != 0U || !valid[node_index]) return false;
    *budget = budgets[node_index];
    *contains_for = nested_for[node_index];
    return true;
}

static void initialize_interactive_evaluator(native_evaluator *evaluator,
                                             shell_state *state,
                                             gsh_variable_store *variables)
{
    if (evaluator == NULL || state == NULL || variables == NULL) {
        return;
    }
    (void)memset(evaluator, 0, sizeof(*evaluator));
    evaluator->exec_outcome_fd = -1;
    evaluator->exec_descriptor_socket = -1;
    evaluator->input = state->pending_input;
    evaluator->input_length = state->pending_input_length;
    evaluator->storage = state->parse_storage;
    evaluator->pipeline = state->native_pipeline;
    evaluator->default_path = state->default_path;
    evaluator->last_status = state->last_status;
    evaluator->shell_pid = (long)state->shell_pgid;
    evaluator->last_background_pid = state->last_background_pid;
    evaluator->parameter_zero = state->parameter_zero;
    evaluator->history = state->history;
    evaluator->history_exclude_newest = true;
    evaluator->positionals = state->positionals;
    evaluator->options = state->options;
    evaluator->variables = variables;
    evaluator->journal = NULL;
    evaluator->aliases = state->aliases;
    evaluator->alias_journal = NULL;
    evaluator->functions = state->functions;
    evaluator->function_scratch = state->function_scratch;
    evaluator->command_cache = state->command_cache;
    evaluator->command_cache_base_generation =
        state->command_cache_generation;
    evaluator->scope_base = state->pipeline_variables;
    evaluator->scope_changes = state->pipeline_changes;
    evaluator->source_workspaces = state->source_workspaces;
    evaluator->pipeline_scope = NULL;
    evaluator->source_depth = 0;
    evaluator->preflight = false;
    evaluator->fatal_error = false;
    evaluator->static_for_items = true;
    evaluator->tail_exec_single = false;
    evaluator->positional_mutation_possible = false;
    evaluator->directory_mutation_possible = false;
    evaluator->alias_mutation_possible = false;
    evaluator->function_mutation_possible = false;
    evaluator->command_cache_mutation_possible = false;
    evaluator->state_commit_invalid = false;
    evaluator->backgrounds = &state->background_jobs;
}

static void managed_pipeline_child(
    shell_state *state, managed_pty *pty, int gate_read, int gate_write,
    int resource_read, int resource_write,
    const sigset_t *previous, const gsh_native_pipeline *pipeline,
    const pipeline_expansion_scope *scope)
{
    if (pipeline == NULL || previous == NULL || pty == NULL || state == NULL) {
        return;
    }
    native_evaluator evaluator = {0};
    gsh_background_table *backgrounds;
    gsh_shell_options options = state->options;
    char release;
    int status;

    (void)close(gate_write);
    if (resource_read >= 0) (void)close(resource_read);
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
    if (resource_write >= 0) {
        evaluator.file_resource_sink.descriptor = resource_write;
        evaluator.file_resource_sink.image_protocol =
            (uint32_t)state->image_protocol;
        evaluator.file_builtin_io = descriptor_builtin_io;
        evaluator.file_builtin_io.resources =
            &evaluator.file_resource_sink;
        evaluator.file_resources_enabled = true;
    }
    backgrounds = allocate_isolated_job_table();
    if (backgrounds == NULL) child_exec_error("managed job state", errno);
    evaluator.backgrounds = backgrounds;
    status = run_native_noninteractive_pipeline(
        (gsh_native_pipeline *)pipeline, state->default_path,
        state->variables, NULL, state->aliases, NULL, scope,
        state->positionals, &options, state->functions,
        state->pipeline_variables, &evaluator, false);
    _exit(status & 255);
}

static bool managed_pipeline_opens_viewer(
    const gsh_native_pipeline *pipeline)
{
    const gsh_native_command *command;
    if (pipeline == NULL || pipeline->command_count != 1U) return false;
    command = &pipeline->commands[0];
    return command->redirect_count == 0U &&
           native_file_builtin_kind(command) == GSH_FILE_BUILTIN_VIEW;
}

static void managed_pipeline_parent(shell_state *state, managed_pty *pty,
                                    int gate_write, int resource_read,
                                    int resource_write, pid_t pid, bool viewer,
                                    const sigset_t *previous)
{
    if (state == NULL || pty == NULL) return;
    if (previous == NULL) {
        return;
    }
    if (resource_write >= 0) (void)close(resource_write);
    if (pid == -1 ||
        gsh_async_repl_attach(state->async_repl,
                              state->async_dispatch_cell, pid, pid,
                              pty->master, resource_read) == -1 ||
        register_managed_job(state, state->async_dispatch_cell,
                             pid, pid) == -1) {
        int saved_errno = errno;

        if (pid > 0) {
            (void)kill(pid, SIGKILL);
        }
        (void)close(pty->master);
        if (resource_read >= 0) (void)close(resource_read);
        output_format(state, "gsh: managed pipeline fork: %s\r\n",
                      strerror(saved_errno));
        gsh_async_repl_finish(state->async_repl,
                              state->async_dispatch_cell, 125 << 8, false);
    } else if (viewer) {
        (void)gsh_async_repl_request_input(
            state->async_repl, state->async_dispatch_cell, true);
        (void)gsh_async_repl_autofocus(state->async_repl);
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
    if (pipeline == NULL || state == NULL) {
        return;
    }
    managed_pty pty = {.master = -1, .slave_hold = -1};
    int gate[2] = {-1, -1};
    int resource[2] = {-1, -1};
    sigset_t blocked;
    sigset_t previous;
    bool viewer;
    pid_t pid;

    if (!gsh_background_has_capacity(&state->background_jobs)) {
        (void)output_text(state, "gsh: managed job registry full\r\n");
        state->mode = MODE_EDITOR;
        return;
    }
    viewer = managed_pipeline_opens_viewer(pipeline);
    if (open_managed_pty(&pty) == -1 ||
        make_pipe(gate, false, GSH_FAULT_JOB_PIPE) == -1 ||
        ((state_async_repl(state)->actions_enabled || viewer) &&
         create_resource_socket(resource) == -1)) {
        output_format(state, "gsh: managed pipeline: %s\r\n",
                      strerror(errno));
        if (pty.master >= 0) {
            (void)close(pty.master);
        }
        if (pty.slave_hold >= 0) {
            (void)close(pty.slave_hold);
        }
        if (gate[0] >= 0) (void)close(gate[0]);
        if (gate[1] >= 0) (void)close(gate[1]);
        if (resource[0] >= 0) (void)close(resource[0]);
        if (resource[1] >= 0) (void)close(resource[1]);
        state->mode = MODE_EDITOR;
        return;
    }
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        output_format(state, "gsh: managed pipeline mask: %s\r\n",
                      strerror(errno));
        (void)close(pty.master);
        (void)close(pty.slave_hold);
        (void)close(gate[0]);
        (void)close(gate[1]);
        if (resource[0] >= 0) (void)close(resource[0]);
        if (resource[1] >= 0) (void)close(resource[1]);
        state->mode = MODE_EDITOR;
        return;
    }
    pid = fork();
    if (pid == 0) {
        managed_pipeline_child(state, &pty, gate[0], gate[1], resource[0],
                               resource[1], &previous, pipeline, scope);
        _exit(125);
    }
    (void)close(pty.slave_hold);
    pty.slave_hold = -1;
    (void)close(gate[0]);
    managed_pipeline_parent(state, &pty, gate[1], resource[0], resource[1],
                            pid, viewer, &previous);
}

static bool native_pipeline_node_is_wait(const shell_state *state,
                                         size_t pipeline)
{
    const gsh_ast_node *pipeline_node;
    const gsh_ast_node *command;

    if (pipeline >= state_parse_storage(state)->node_count) {
        return false;
    }
    pipeline_node = &state_parse_storage(state)->nodes[pipeline];
    if (pipeline_node->kind != GSH_AST_PIPELINE ||
        pipeline_node->first_child == GSH_AST_NONE ||
        state_parse_storage(state)->nodes[pipeline_node->first_child].next_sibling !=
            GSH_AST_NONE) {
        return false;
    }
    command = &state_parse_storage(state)->nodes[pipeline_node->first_child];
    return command->kind == GSH_AST_SIMPLE && command->word_count != 0 &&
           literal_command_word_is(
               state->pending_input,
               state_parse_storage(state)->words[command->first_word], "wait");
}

static bool native_list_node_is_wait(const shell_state *state,
                                     size_t node_index)
{
    const gsh_ast_node *node;
    size_t pipeline;

    if (node_index >= state_parse_storage(state)->node_count) {
        return false;
    }
    node = &state_parse_storage(state)->nodes[node_index];
    if (node->kind != GSH_AST_AND_OR) {
        return false;
    }
    pipeline = node->first_child;
    while (pipeline != GSH_AST_NONE) {
        const gsh_ast_node *pipeline_node =
            &state_parse_storage(state)->nodes[pipeline];

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
    if (state == NULL) {
        return false;
    }
    const gsh_ast_node *pipeline_node =
        &state_parse_storage(state)->nodes[pipeline];
    const gsh_ast_node *command =
        &state_parse_storage(state)->nodes[pipeline_node->first_child];
    size_t word_index;

    for (word_index = 1U; word_index < command->word_count; word_index++) {
        gsh_word_ref word = state_parse_storage(state)
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
    native_evaluator evaluator = {0};
    pipeline_expansion_scope scope = {0};
    gsh_native_plan_status plan_status;
    bool deferred_work = false;
    bool scoped = false;

    if (wait_pipeline_has_substitution(state, pipeline)) {
        (void)output_text(state,
                    "gsh: wait expansion requires isolated continuation"
                    "\r\n");
        state->last_status = 125;
        return false;
    }
    initialize_interactive_evaluator(&evaluator, state, state->variables);
    plan_status = plan_evaluator_pipeline_preflight(
        &evaluator, pipeline, &scope, &scoped, &deferred_work);
    if (plan_status != GSH_NATIVE_PLAN_OK || deferred_work || scoped ||
        state_native_pipeline(state)->command_count != 1 ||
        !native_wait_builtin(&state_native_pipeline(state)->commands[0]) ||
        state_native_pipeline(state)->commands[0].assignment_count != 0 ||
        state_native_pipeline(state)->commands[0].redirect_count != 0) {
        state->last_status = plan_status == GSH_NATIVE_PLAN_ERROR ? 1 : 125;
        return false;
    }
    state->variable_generation++;
    begin_background_wait(state, state->native_pipeline);
    return true;
}

static bool register_background_node(
    shell_state *state, pid_t pid, const char *command,
    size_t command_length, const sigset_t *previous)
{
    uint32_t job_id;

    if (!require(state != NULL && command != NULL && previous != NULL)) {
        return false;
    }
    if (!require(command_length < GSH_BACKGROUND_COMMAND_CAP)) return false;
    if (pid == -1) {
        int saved_errno = errno;

        (void)sigprocmask(SIG_SETMASK, previous, NULL);
        output_format(state, "gsh: asynchronous fork: %s\r\n",
                      strerror(saved_errno));
        return false;
    }
    (void)setpgid(pid, pid);
    if (gsh_background_add_job(
            &state->background_jobs, pid, pid, &pid, 1U, 1U, false, command,
            command_length, GSH_JOB_ORIGIN_ASYNC_LIST, &job_id) == -1) {
        int saved_errno = errno;

        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
        (void)sigprocmask(SIG_SETMASK, previous, NULL);
        output_format(state, "gsh: asynchronous registry: %s\r\n",
                      strerror(saved_errno));
        return false;
    }
    state->last_background_pid = (long)pid;
    state->last_status = 0;
    output_format(state, "[%u] %ld\r\n", job_id, (long)pid);
    (void)sigprocmask(SIG_SETMASK, previous, NULL);
    return true;
}

static bool start_background_node(shell_state *state, size_t node_index)
{
    if (state == NULL) return false;
    const char *command;
    size_t command_length;
    sigset_t blocked;
    sigset_t previous;
    pid_t pid;

    if (!gsh_background_has_capacity(&state->background_jobs) ||
        node_index >= state_parse_storage(state)->node_count ||
        !bounded_job_command(
            state->pending_input, state->pending_input_length,
            &state_parse_storage(state)->nodes[node_index], &command,
            &command_length)) {
        (void)output_text(state, "gsh: asynchronous registry full\r\n");
        return false;
    }
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous) == -1) {
        output_format(state, "gsh: asynchronous sigprocmask: %s\r\n",
                      strerror(errno));
        return false;
    }
    pid = gsh_fault_should_fail(GSH_FAULT_ASYNC_FORK, EAGAIN) ? -1 : fork();
    if (pid == 0) {
        native_evaluator evaluator = {0};
        gsh_background_table *child_backgrounds;
        int null_descriptor;
        int status;

        (void)memset(&evaluator, 0, sizeof(evaluator));
        evaluator.exec_outcome_fd = -1;
        evaluator.exec_descriptor_socket = -1;
        (void)setpgid(0, 0);
        reset_child_signals();
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        (void)close(state->tty_fd);
        (void)close(state->signal_pipe[0]);
        (void)close(state->signal_pipe[1]);
        if (state->redirection_worker_fd >= 0) {
            (void)close(state->redirection_worker_fd);
        }
        if (state->variable_commit_fd >= 0) {
            (void)close(state->variable_commit_fd);
        }
        if (state->directory_commit_socket >= 0) {
            (void)close(state->directory_commit_socket);
        }
        null_descriptor = open("/dev/null", O_RDONLY);
        if (null_descriptor == -1 ||
            child_duplicate_descriptor(null_descriptor, STDIN_FILENO) ==
                -1) {
            child_exec_error("asynchronous standard input", errno);
        }
        if (null_descriptor != STDIN_FILENO) {
            (void)close(null_descriptor);
        }
        initialize_interactive_evaluator(&evaluator, state,
                                         state->variables);
        child_backgrounds = allocate_isolated_job_table();
        if (child_backgrounds == NULL) {
            child_exec_error("asynchronous job state", errno);
        }
        evaluator.backgrounds = child_backgrounds;
        evaluator.last_background_pid = 0;
        evaluator.static_for_items = false;
        evaluator.suppress_async_once = true;
        evaluator.tail_exec_single =
            async_node_has_single_pipeline(&evaluator, node_index);
        status = native_evaluate_node_inner(&evaluator, node_index, 0);
        _exit(status & 255);
    }
    return register_background_node(state, pid, command, command_length,
                                    &previous);
}

static void continue_native_and_or(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    while (state->pending_and_or_active &&
           state->pending_and_or_next != GSH_AST_NONE) {
        size_t pipeline = state->pending_and_or_next;
        const gsh_ast_node *node =
            &state_parse_storage(state)->nodes[pipeline];

        state->pending_and_or_next = node->next_sibling;
        if ((node->connector == GSH_TOKEN_AND_IF &&
             state->last_status != 0) ||
            (node->connector == GSH_TOKEN_OR_IF &&
             state->last_status == 0)) {
            continue;
        }
        if (!native_node_is_supported(state, pipeline)) {
            (void)output_text(state,
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
        state->mode = MODE_DISPATCH;
    } else {
        state->mode = MODE_EDITOR;
        queue_prompt(state);
    }
}

static void continue_native_list(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    while (state->pending_list_active &&
           state->pending_list_next != GSH_AST_NONE) {
        size_t node_index = state->pending_list_next;
        const gsh_ast_node *node =
            &state_parse_storage(state)->nodes[node_index];

        state->pending_list_next = node->next_sibling;
        if (!native_node_is_supported(state, node_index)) {
            (void)output_text(state,
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
            state->mode = MODE_DISPATCH;
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
    if (state == NULL) {
        return 0U;
    }
    size_t child = state_parse_storage(state)->nodes[node_index].first_child;

    return child != GSH_AST_NONE &&
                   state_parse_storage(state)->nodes[child].next_sibling ==
                       GSH_AST_NONE
               ? child
               : GSH_AST_NONE;
}

static bool reactor_pure_status(const shell_state *state, size_t node_index,
                                int *status)
{
    if (state == NULL || status == NULL) {
        return false;
    }
    bool negated = false;
    const gsh_ast_node *node;
    size_t step;

    for (step = 0; step < AST_WALK_STEP_CAP; step++) {
        size_t child;

        node = &state_parse_storage(state)->nodes[node_index];
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
    if (step == AST_WALK_STEP_CAP) return false;
    if (node->kind == GSH_AST_SIMPLE && node->word_count == 1U) {
        gsh_word_ref word =
            state_parse_storage(state)->words[node->first_word];
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
    if (state == NULL) {
        return false;
    }
    const gsh_ast_node *node;
    size_t child;
    gsh_word_ref name;
    gsh_word_ref value;
    size_t step;

    for (step = 0; step < AST_WALK_STEP_CAP; step++) {
        node = &state_parse_storage(state)->nodes[node_index];
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
    if (step == AST_WALK_STEP_CAP) return false;
    if (node->kind != GSH_AST_FOR ||
        (node->flags & GSH_AST_FLAG_FOR_HAS_IN) == 0 ||
        !reactor_pure_status(state, node->first_child, status)) {
        return false;
    }
    if (node->word_count == 1U) {
        *status = 0;
        return true;
    }
    name = state_parse_storage(state)->words[node->first_word];
    value = state_parse_storage(state)->words[node->first_word +
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
    native_evaluator evaluator = {0};
    size_t budget;
    bool contains_for;
    int status;

    if (gsh_fault_active() ||
        !reactor_safe_node(state, 0, &budget, &contains_for) ||
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

enum {
    GSH_STATE_COMMIT_PART_CAP = 7,
    GSH_FUNCTION_COMMIT_SECTION_CAP = 5,
    GSH_STATE_COMMIT_IO_BOUND = 8 * 1024 * 1024,
};

_Static_assert(sizeof(gsh_variable_journal) <= GSH_STATE_COMMIT_IO_BOUND,
               "variable commit exceeds the bounded writer");
_Static_assert(sizeof(gsh_alias_journal) <= GSH_STATE_COMMIT_IO_BOUND,
               "alias commit exceeds the bounded writer");
_Static_assert(sizeof(gsh_positional_store) <= GSH_STATE_COMMIT_IO_BOUND,
               "positional commit exceeds the bounded writer");
_Static_assert(sizeof(gsh_command_cache) <= GSH_STATE_COMMIT_IO_BOUND,
               "cache commit exceeds the bounded writer");
_Static_assert(sizeof(gsh_function_store) <= GSH_STATE_COMMIT_IO_BOUND,
               "function commit exceeds the bounded writer");

static int write_commit_bytes(int descriptor, const void *source,
                              size_t length, gsh_fault_point fault)
{
    if (source == NULL) {
        return -1;
    }
    const unsigned char *cursor = source;
    size_t remaining = length;
    size_t attempts;

    if (descriptor < 0 || source == NULL || fault <= GSH_FAULT_NONE ||
        fault >= GSH_FAULT_COUNT || length > GSH_STATE_COMMIT_IO_BOUND) {
        errno = EINVAL;
        return -1;
    }
    for (attempts = 0;
         remaining != 0 && attempts < GSH_STATE_COMMIT_IO_BOUND;
         attempts++) {
        ssize_t written = gsh_fault_should_fail(fault, EIO)
                              ? -1
                              : write(descriptor, cursor, remaining);

        if (written > 0) {
            cursor += (size_t)written;
            remaining -= (size_t)written;
        } else if (!(written == -1 && errno == EINTR)) {
            return -1;
        }
    }
    if (remaining != 0) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

static int write_function_commit(int descriptor,
                                 const gsh_function_store *functions,
                                 uint64_t generation)
{
    gsh_function_snapshot_header header;
    size_t offset = 0;
    size_t total;
    size_t section;

    if (functions == NULL) {
        return 0;
    }
    gsh_functions_snapshot_header(functions, generation, &header);
    if (gsh_fault_should_fail(GSH_FAULT_FUNCTION_COMMIT_MALFORMED, EPROTO)) {
        header.reserved = 1;
    }
    if (write_commit_bytes(descriptor, &header, sizeof(header),
                           GSH_FAULT_STATE_COMMIT_WRITE) == -1) {
        return -1;
    }
    total = gsh_functions_snapshot_payload_size(&header);
    for (section = 0;
         offset < total && section < GSH_FUNCTION_COMMIT_SECTION_CAP;
         section++) {
        size_t available;
        const void *source = gsh_functions_snapshot_source(
            functions, &header, offset, &available);

        if (source == NULL || available == 0) {
            errno = EPROTO;
            return -1;
        }
        if (write_commit_bytes(descriptor, source, available,
                               GSH_FAULT_FUNCTION_COMMIT_WRITE) == -1) {
            return -1;
        }
        offset += available;
    }
    if (offset != total) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

static void inject_state_commit_faults(
    gsh_variable_journal *journal, gsh_alias_journal *alias_journal,
    const gsh_positional_store *positionals,
    const gsh_command_cache *command_cache,
    command_cache_commit_header *cache_header,
    const gsh_shell_options *options, state_control_commit *control)
{
    if (cache_header == NULL || control == NULL || journal == NULL || options == NULL) {
        return;
    }
    if (gsh_fault_should_fail(GSH_FAULT_STATE_COMMIT_MALFORMED, EPROTO)) {
        journal->version++;
    }
    if (positionals != NULL &&
        gsh_fault_should_fail(GSH_FAULT_POSITIONAL_COMMIT_MALFORMED, EPROTO)) {
        ((gsh_positional_store *)positionals)->version++;
    }
    if (alias_journal != NULL &&
        gsh_fault_should_fail(GSH_FAULT_ALIAS_COMMIT_MALFORMED, EPROTO)) {
        alias_journal->version++;
    }
    if (gsh_fault_should_fail(GSH_FAULT_OPTION_COMMIT_MALFORMED, EPROTO)) {
        ((gsh_shell_options *)options)->enabled |= 1U << 29;
    }
    if (command_cache != NULL &&
        gsh_fault_should_fail(GSH_FAULT_COMMAND_CACHE_COMMIT_MALFORMED, EPROTO)) {
        cache_header->reserved = 1U;
    }
    if (gsh_fault_should_fail(GSH_FAULT_STATE_CONTROL_COMMIT_MALFORMED, EPROTO)) {
        control->reserved = 1U;
    }
}

static int write_variable_commit(
    int descriptor, gsh_variable_journal *journal,
    gsh_alias_journal *alias_journal,
    const gsh_positional_store *positionals,
    const gsh_command_cache *command_cache,
    uint64_t command_cache_generation, uint64_t final_path_generation,
    const gsh_shell_options *options, bool exiting, int exit_status,
    const gsh_function_store *functions, uint64_t function_generation)
{
    if (journal == NULL || options == NULL) {
        return -1;
    }
    command_cache_commit_header cache_header = {
        GSH_COMMAND_CACHE_COMMIT_VERSION, 0,
        command_cache_generation, final_path_generation};
    state_control_commit control = {
        GSH_STATE_CONTROL_COMMIT_VERSION,
        exiting ? GSH_STATE_CONTROL_EXIT : 0U,
        exiting ? exit_status : 0, 0};
    const void *parts[GSH_STATE_COMMIT_PART_CAP] = {
        journal, alias_journal, positionals,
        command_cache == NULL ? NULL : &cache_header,
        command_cache, options, &control};
    const size_t lengths[GSH_STATE_COMMIT_PART_CAP] = {
        sizeof(*journal), sizeof(*alias_journal), sizeof(*positionals),
        sizeof(cache_header), sizeof(*command_cache), sizeof(*options),
        sizeof(control)};
    size_t part;

    if (journal == NULL || options == NULL ||
        (exiting && (exit_status < 0 || exit_status > 255))) {
        errno = EINVAL;
        return -1;
    }
    inject_state_commit_faults(journal, alias_journal, positionals,
                               command_cache, &cache_header, options,
                               &control);
    for (part = 0; part < GSH_STATE_COMMIT_PART_CAP; part++) {
        if (parts[part] != NULL &&
            write_commit_bytes(descriptor, parts[part], lengths[part],
                               GSH_FAULT_STATE_COMMIT_WRITE) == -1) {
            return -1;
        }
    }
    return write_function_commit(descriptor, functions,
                                 function_generation);
}

static int build_exec_descriptor_commit(
    const native_evaluator *evaluator, exec_descriptor_commit *commit,
    int rights[GSH_EXEC_DESCRIPTOR_COMMIT_CAP])
{
    if (evaluator == NULL) return -1;
    if (commit == NULL || rights == NULL) {
        return -1;
    }
    size_t index;

    if (evaluator->exec_descriptor_count >
        GSH_EXEC_DESCRIPTOR_COMMIT_CAP) {
        errno = EOVERFLOW;
        return -1;
    }
    (void)memset(commit, 0, sizeof(*commit));
    commit->version = GSH_EXEC_DESCRIPTOR_COMMIT_VERSION;
    commit->count = (uint32_t)evaluator->exec_descriptor_count;
    for (index = 0; index < evaluator->exec_descriptor_count; index++) {
        int target = evaluator->exec_descriptors[index];

        commit->targets[index] = target;
        if (fcntl(target, F_GETFD) >= 0) {
            commit->open[index] = 1U;
            rights[commit->open_count++] = target;
        } else if (errno != EBADF) {
            return -1;
        }
    }
    return 0;
}

static int send_exec_descriptor_commit(
    int socket, const native_evaluator *evaluator)
{
    if (evaluator == NULL) return -1;
    exec_descriptor_commit commit;
    int rights[GSH_EXEC_DESCRIPTOR_COMMIT_CAP];
    unsigned char control[
        CMSG_SPACE(sizeof(int) * GSH_EXEC_DESCRIPTOR_COMMIT_CAP)];
    struct iovec payload = {&commit, sizeof(commit)};
    struct msghdr message;
    unsigned int attempts;

    if (!evaluator->exec_descriptors_dirty || socket < 0) {
        return 0;
    }
    if (build_exec_descriptor_commit(evaluator, &commit, rights) == -1) {
        return -1;
    }
    (void)memset(&message, 0, sizeof(message));
    message.msg_iov = &payload;
    message.msg_iovlen = 1;
    if (commit.open_count != 0) {
        struct cmsghdr *header;
        size_t control_size =
            CMSG_SPACE(sizeof(int) * commit.open_count);

        (void)memset(control, 0, control_size);
        message.msg_control = control;
        message.msg_controllen = control_size;
        header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int) * commit.open_count);
        (void)memcpy(CMSG_DATA(header), rights,
               sizeof(int) * commit.open_count);
    }
    for (attempts = 0; attempts < 16U; attempts++) {
        ssize_t sent = gsh_fault_should_fail(GSH_FAULT_EXEC_DESCRIPTOR_SEND, EIO)
                           ? -1
                           : sendmsg(socket, &message, 0);

        if (sent == (ssize_t)sizeof(commit)) {
            return 0;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        errno = sent >= 0 ? EIO : errno;
        return -1;
    }
    errno = EINTR;
    return -1;
}

static void abandon_pending_list(shell_state *state)
{
    if (state == NULL) {
        return;
    }
    state->pending_list_active = false;
    state->pending_list_next = GSH_AST_NONE;
    state->pending_and_or_active = false;
    state->pending_and_or_next = GSH_AST_NONE;
    state->pending_exec_possible = false;
    state->pending_job_service = false;
    state->pending_exec_descriptor_count = 0;
    state->pending_exec_protected_descriptor_count = 0;
}

static bool pending_exec_protects_descriptor(const shell_state *state,
                                             int descriptor)
{
    if (state == NULL) {
        return false;
    }
    size_t index;

    for (index = 0;
         index < state->pending_exec_protected_descriptor_count; index++) {
        if (state->pending_exec_protected_descriptors[index] ==
            descriptor) {
            return true;
        }
    }
    return false;
}

static int protect_exec_transaction_descriptors(
    const shell_state *state, int gate[2], int commit[2], int directory[2],
    int outcome[2], int descriptors[2], int job_service[2], int resource[2])
{
    if (commit == NULL || descriptors == NULL || directory == NULL ||
        gate == NULL || job_service == NULL || outcome == NULL ||
        resource == NULL) {
        return -1;
    }
    int *child_descriptors[] = {
        &gate[0], &commit[1], &directory[1], &outcome[1], &descriptors[1],
        &job_service[1], &resource[1]};
    int reservations[GSH_EXEC_DESCRIPTOR_COMMIT_CAP];
    size_t reservation_count = 0;
    size_t index;

    if (state == NULL || state->pending_exec_protected_descriptor_count >
                             GSH_EXEC_DESCRIPTOR_COMMIT_CAP) return -1;
    for (index = 0;
         index < sizeof(child_descriptors) / sizeof(child_descriptors[0]);
         index++) {
        int *descriptor = child_descriptors[index];
        size_t attempt;

        if (*descriptor < 0 ||
            !pending_exec_protects_descriptor(state, *descriptor)) {
            continue;
        }
        for (attempt = 0; attempt <= GSH_EXEC_DESCRIPTOR_COMMIT_CAP;
             attempt++) {
            int duplicate =
                gsh_fault_should_fail(GSH_FAULT_TRANSACTION_DESCRIPTOR_RELOCATION,
                                  EMFILE)
                    ? -1
                    : fcntl(*descriptor, F_DUPFD_CLOEXEC,
                            STDERR_FILENO + 1);

            if (duplicate == -1) {
                close_exec_commit_fds(reservations, reservation_count);
                return -1;
            }
            if (!pending_exec_protects_descriptor(state, duplicate)) {
                (void)close(*descriptor);
                *descriptor = duplicate;
                break;
            }
            if (reservation_count == GSH_EXEC_DESCRIPTOR_COMMIT_CAP) {
                (void)close(duplicate);
                close_exec_commit_fds(reservations, reservation_count);
                errno = EMFILE;
                return -1;
            }
            reservations[reservation_count++] = duplicate;
        }
        if (attempt > GSH_EXEC_DESCRIPTOR_COMMIT_CAP) {
            close_exec_commit_fds(reservations, reservation_count);
            errno = EMFILE;
            return -1;
        }
    }
    close_exec_commit_fds(reservations, reservation_count);
    return 0;
}

typedef struct {
    int gate[2];
    int commit[2];
    int directory[2];
    int exec_outcome[2];
    int exec_descriptors[2];
    int job_service[2];
    int resource[2];
    managed_pty pty;
    bool managed;
    bool signals_blocked;
    sigset_t previous;
    struct tms owner_times;
    bool owner_times_valid;
    pid_t pid;
} compound_launch;

static void initialize_compound_launch(compound_launch *launch,
                                       bool managed)
{
    if (!require(launch != NULL)) return;
    (void)memset(launch, 0, sizeof(*launch));
    launch->gate[0] = launch->gate[1] = -1;
    launch->commit[0] = launch->commit[1] = -1;
    launch->directory[0] = launch->directory[1] = -1;
    launch->exec_outcome[0] = launch->exec_outcome[1] = -1;
    launch->exec_descriptors[0] = launch->exec_descriptors[1] = -1;
    launch->job_service[0] = launch->job_service[1] = -1;
    launch->resource[0] = launch->resource[1] = -1;
    launch->pty.master = -1;
    launch->pty.slave_hold = -1;
    launch->managed = managed;
    launch->pid = -1;
}

static void close_compound_pair(int descriptors[2])
{
    if (!require(descriptors != NULL)) return;
    if (!require(descriptors[0] >= -1 && descriptors[1] >= -1)) return;
    if (descriptors[0] >= 0) (void)close(descriptors[0]);
    if (descriptors[1] >= 0) (void)close(descriptors[1]);
    descriptors[0] = -1;
    descriptors[1] = -1;
}

static void close_compound_channels(compound_launch *launch)
{
    if (!require(launch != NULL)) return;
    if (!require(launch->pty.master >= -1)) return;
    close_compound_pair(launch->gate);
    close_compound_pair(launch->commit);
    close_compound_pair(launch->directory);
    close_compound_pair(launch->exec_outcome);
    close_compound_pair(launch->exec_descriptors);
    close_compound_pair(launch->job_service);
    close_compound_pair(launch->resource);
    if (launch->pty.master >= 0) (void)close(launch->pty.master);
    if (launch->pty.slave_hold >= 0) (void)close(launch->pty.slave_hold);
    launch->pty.master = -1;
    launch->pty.slave_hold = -1;
}

static void clear_compound_expectations(shell_state *state)
{
    if (!require(state != NULL)) return;
    if (!require(state->pending_exec_descriptor_count <=
                 GSH_EXEC_DESCRIPTOR_COMMIT_CAP)) {
        state->pending_exec_descriptor_count = 0U;
    }
    state->positional_commit_expected = false;
    state->pending_positional_commit = false;
    state->alias_commit_expected = false;
    state->pending_alias_commit = false;
    state->function_commit_expected = false;
    state->pending_function_commit = false;
    state->command_cache_commit_expected = false;
    state->pending_command_cache_commit = false;
    state->directory_commit_expected = false;
    state->pending_directory_commit = false;
    state->pending_exec_possible = false;
    state->pending_job_service = false;
}

static void reject_compound_start(shell_state *state,
                                  compound_launch *launch,
                                  const char *operation, int error)
{
    if (!require(state != NULL && launch != NULL)) return;
    if (!require(operation != NULL)) return;
    close_compound_channels(launch);
    if (launch->signals_blocked) {
        (void)sigprocmask(SIG_SETMASK, &launch->previous, NULL);
        launch->signals_blocked = false;
    }
    clear_compound_expectations(state);
    output_format(state, "gsh: %s: %s\r\n", operation, strerror(error));
    abandon_pending_list(state);
    state->mode = MODE_EDITOR;
    queue_prompt(state);
}

static bool compound_capacity_is_available(shell_state *state)
{
    if (!require(state != NULL)) return false;
    if (!require(state->mode == MODE_DISPATCH)) return false;
    if (state->current_job.active) {
        (void)output_text(state,
                    "gsh: this MVP supports one job at a time; use fg or wait "
                    "for it\r\n");
    } else if (!gsh_background_has_capacity(&state->background_jobs)) {
        (void)output_text(state, "gsh: job registry full\r\n");
    } else if (state->pending_alias_commit &&
               ensure_alias_state(state, true) == -1) {
        output_format(state, "gsh: alias transaction allocation: %s\r\n",
                      strerror(errno));
        state->pending_alias_commit = false;
    } else if (state->pending_function_commit &&
               ensure_function_state(state, true) == -1) {
        output_format(state, "gsh: function transaction allocation: %s\r\n",
                      strerror(errno));
        state->pending_function_commit = false;
    } else {
        return true;
    }
    abandon_pending_list(state);
    state->mode = MODE_EDITOR;
    queue_prompt(state);
    return false;
}

static int create_compound_socket(int descriptors[2],
                                  gsh_fault_point fault_point)
{
    if (!require(descriptors != NULL)) return -1;
    if (!require(fault_point > GSH_FAULT_NONE &&
                 fault_point < GSH_FAULT_COUNT)) return -1;
    if (gsh_fault_should_fail(fault_point, EMFILE) ||
        socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors) == -1 ||
        set_fd_flags(descriptors[0], F_GETFL, O_NONBLOCK) == -1 ||
        set_fd_flags(descriptors[0], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(descriptors[1], F_GETFD, FD_CLOEXEC) == -1) {
        return -1;
    }
    return 0;
}

static int create_resource_socket(int descriptors[2])
{
    if (!require(descriptors != NULL)) return -1;
    if (gsh_fault_should_fail(GSH_FAULT_RESOURCE_ACTION_SOCKET, EMFILE) ||
        socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors) == -1 ||
        set_fd_flags(descriptors[0], F_GETFL, O_NONBLOCK) == -1 ||
        set_fd_flags(descriptors[1], F_GETFL, O_NONBLOCK) == -1 ||
        set_fd_flags(descriptors[0], F_GETFD, FD_CLOEXEC) == -1 ||
        set_fd_flags(descriptors[1], F_GETFD, FD_CLOEXEC) == -1) return -1;
    return 0;
}

static bool create_compound_channels(shell_state *state,
                                     compound_launch *launch)
{
    if (!require(state != NULL)) return false;
    if (!require(launch != NULL)) return false;
    if (make_pipe(launch->gate, false, GSH_FAULT_EVALUATOR_GATE) == -1) {
        reject_compound_start(state, launch, "evaluator gate", errno);
        return false;
    }
    if (make_pipe(launch->commit, false, GSH_FAULT_STATE_COMMIT_PIPE) == -1 ||
        set_fd_flags(launch->commit[0], F_GETFL, O_NONBLOCK) == -1) {
        reject_compound_start(state, launch, "state transaction pipe",
                              errno);
        return false;
    }
    state->directory_commit_expected = state->pending_directory_commit;
    if (state->directory_commit_expected &&
        create_compound_socket(launch->directory,
                               GSH_FAULT_DIRECTORY_COMMIT_SOCKET) == -1) {
        reject_compound_start(state, launch,
                              "directory transaction socket", errno);
        return false;
    }
    if (state->pending_exec_possible &&
        make_pipe(launch->exec_outcome, false,
                  GSH_FAULT_EXEC_OUTCOME_PIPE) == -1) {
        reject_compound_start(state, launch, "exec outcome pipe", errno);
        return false;
    }
    if (state->pending_exec_possible &&
        create_compound_socket(launch->exec_descriptors,
                               GSH_FAULT_EXEC_DESCRIPTOR_SOCKET) == -1) {
        reject_compound_start(state, launch, "exec descriptor socket",
                              errno);
        return false;
    }
    if (state->pending_job_service &&
        create_compound_socket(launch->job_service,
                               GSH_FAULT_JOB_SERVICE_SOCKET) == -1) {
        reject_compound_start(state, launch, "job service socket", errno);
        return false;
    }
    if (launch->managed && state_async_repl(state)->actions_enabled &&
        create_resource_socket(launch->resource) == -1) {
        reject_compound_start(state, launch, "resource action socket", errno);
        return false;
    }
    if (state->pending_exec_possible &&
        protect_exec_transaction_descriptors(
            state, launch->gate, launch->commit, launch->directory,
            launch->exec_outcome, launch->exec_descriptors,
            launch->job_service, launch->resource) == -1) {
        reject_compound_start(state, launch,
                              "exec descriptor protection", errno);
        return false;
    }
    return true;
}

static bool block_compound_signals(shell_state *state,
                                   compound_launch *launch)
{
    sigset_t blocked;

    if (!require(state != NULL)) return false;
    if (!require(launch != NULL)) return false;
    (void)sigemptyset(&blocked);
    (void)sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &launch->previous) == -1) {
        reject_compound_start(state, launch, "sigprocmask", errno);
        return false;
    }
    launch->signals_blocked = true;
    return true;
}

static bool initialize_compound_commits(shell_state *state,
                                        compound_launch *launch)
{
    if (!require(state != NULL)) return false;
    if (!require(launch != NULL && launch->signals_blocked)) return false;
    gsh_variable_journal_initialize(state->variable_commit,
                                    state->variable_generation);
    state->alias_commit_expected = state->pending_alias_commit;
    if (state->alias_commit_expected) {
        gsh_alias_journal_initialize(state->alias_commit,
                                     state->alias_generation);
    }
    state->function_commit_expected = state->pending_function_commit;
    state->function_commit_header_complete = false;
    (void)memset(&state->function_commit_header, 0,
           sizeof(state->function_commit_header));
    state->command_cache_commit_expected =
        state->pending_command_cache_commit;
    (void)memset(&state->command_cache_commit_header, 0,
           sizeof(state->command_cache_commit_header));
    state->positional_commit_expected = state->pending_positional_commit;
    if (!state->positional_commit_expected) return true;
    if (gsh_fault_should_fail(GSH_FAULT_POSITIONAL_COMMIT_ALLOCATION, ENOMEM)) {
        reject_compound_start(state, launch, "positional transaction",
                              errno);
        return false;
    }
    if (state->positionals == NULL) {
        gsh_positionals_initialize(state->positional_commit);
    } else {
        (void)memcpy(state->positional_commit, state->positionals,
               sizeof(*state->positional_commit));
    }
    return true;
}

static void close_compound_parent_ends(compound_launch *launch)
{
    if (!require(launch != NULL)) return;
    if (!require(launch->gate[0] >= -1)) return;
    if (launch->gate[1] >= 0) (void)close(launch->gate[1]);
    if (launch->commit[0] >= 0) (void)close(launch->commit[0]);
    if (launch->exec_outcome[0] >= 0) (void)close(launch->exec_outcome[0]);
    if (launch->exec_descriptors[0] >= 0) {
        (void)close(launch->exec_descriptors[0]);
    }
    if (launch->directory[0] >= 0) (void)close(launch->directory[0]);
    if (launch->job_service[0] >= 0) (void)close(launch->job_service[0]);
    if (launch->resource[0] >= 0) (void)close(launch->resource[0]);
    launch->gate[1] = -1;
    launch->commit[0] = -1;
    launch->exec_outcome[0] = -1;
    launch->exec_descriptors[0] = -1;
    launch->directory[0] = -1;
    launch->job_service[0] = -1;
    launch->resource[0] = -1;
}

static void prepare_compound_child(shell_state *state,
                                   compound_launch *launch)
{
    char release;

    if (!require(state != NULL)) _exit(125);
    if (!require(launch != NULL && launch->signals_blocked)) _exit(125);
    if (!require(launch->gate[0] >= 0)) _exit(125);
    close_compound_parent_ends(launch);
    if (launch->managed) {
        (void)close(launch->pty.master);
        launch->pty.master = -1;
        if (attach_child_pty(state, &launch->pty) == -1) {
            child_exec_error("managed evaluator PTY", errno);
        }
    } else {
        (void)setpgid(0, 0);
    }
    reset_child_signals();
    (void)sigprocmask(SIG_SETMASK, &launch->previous, NULL);
    while (read(launch->gate[0], &release, sizeof(release)) == -1 &&
           errno == EINTR) {
    }
    (void)close(launch->gate[0]);
    launch->gate[0] = -1;
    close_child_reactor_descriptors(state, -1);
}

static void initialize_compound_evaluator(
    shell_state *state, compound_launch *launch,
    native_evaluator *evaluator, gsh_times_context *times_context)
{
    if (!require(state != NULL && launch != NULL)) return;
    if (!require(evaluator != NULL && times_context != NULL)) return;
    (void)memset(evaluator, 0, sizeof(*evaluator));
    evaluator->input = state->pending_input;
    evaluator->input_length = state->pending_input_length;
    evaluator->storage = state->parse_storage;
    evaluator->pipeline = state->native_pipeline;
    evaluator->default_path = state->default_path;
    evaluator->last_status = state->last_status;
    evaluator->shell_pid = (long)state->shell_pgid;
    evaluator->last_background_pid = state->last_background_pid;
    evaluator->parameter_zero = state->parameter_zero;
    evaluator->history = state->history;
    evaluator->history_exclude_newest = true;
    evaluator->positionals = state->positional_commit_expected
                                ? state->positional_commit
                                : state->positionals;
    evaluator->options = state->options;
    evaluator->variables = state->variables;
    evaluator->journal = state->variable_commit;
    evaluator->aliases = state->aliases;
    evaluator->alias_journal = state->alias_commit_expected
                                   ? state->alias_commit
                                   : NULL;
    evaluator->functions = state->functions;
    evaluator->function_scratch = state->function_scratch;
    evaluator->command_cache = state->command_cache;
    evaluator->command_cache_base_generation =
        state->command_cache_generation;
    (void)memset(times_context, 0, sizeof(*times_context));
    if (launch->owner_times_valid) {
        (void)gsh_times_rebase(times_context, &launch->owner_times);
    }
    evaluator->times_context = times_context;
    evaluator->scope_base = state->pipeline_variables;
    evaluator->scope_changes = state->pipeline_changes;
    evaluator->source_workspaces = state->source_workspaces;
    evaluator->job_service_available = launch->job_service[1] >= 0;
    evaluator->job_service_socket = launch->job_service[1];
    evaluator->exec_outcome_fd = launch->exec_outcome[1];
    evaluator->exec_descriptor_socket = launch->exec_descriptors[1];
    evaluator->exec_descriptor_count =
        state->pending_exec_descriptor_count;
    (void)memcpy(evaluator->exec_descriptors, state->pending_exec_descriptors,
           evaluator->exec_descriptor_count *
               sizeof(evaluator->exec_descriptors[0]));
    evaluator->file_resource_sink.descriptor = launch->resource[1];
    evaluator->file_resource_sink.image_protocol =
        (uint32_t)state->image_protocol;
    evaluator->file_builtin_io = descriptor_builtin_io;
    if (launch->resource[1] >= 0) {
        evaluator->file_resources_enabled = true;
        evaluator->file_builtin_io.resources =
            &evaluator->file_resource_sink;
    }
}

static int commit_compound_evaluator(shell_state *state,
                                     compound_launch *launch,
                                     native_evaluator *evaluator,
                                     int status)
{
    if (!require(state != NULL && launch != NULL)) return 125;
    if (!require(evaluator != NULL)) return 125;
    if (send_exec_descriptor_commit(
            launch->exec_descriptors[1], evaluator) == -1) {
        perror("gsh: exec descriptor commit");
        status = 125;
    }
    if (state->directory_commit_expected &&
        send_directory_descriptor(launch->directory[1]) == -1) {
        status = 125;
    }
    if (launch->directory[1] >= 0) {
        (void)close(launch->directory[1]);
        launch->directory[1] = -1;
    }
    if (evaluator->state_commit_invalid ||
        write_variable_commit(
            launch->commit[1], state->variable_commit,
            state->alias_commit_expected ? state->alias_commit : NULL,
            state->positional_commit_expected ? state->positional_commit
                                              : NULL,
            state->command_cache_commit_expected ? state->command_cache
                                                 : NULL,
            state->command_cache_generation,
            gsh_variables_path_generation(evaluator->variables),
            &evaluator->options, evaluator->exiting,
            evaluator->exit_status,
            state->function_commit_expected ? state->functions : NULL,
            state->function_generation) == -1) {
        status = 125;
    }
    return status;
}

_Noreturn static void run_compound_child(shell_state *state,
                                         compound_launch *launch,
                                         size_t node_index)
{
    native_evaluator evaluator = {0};
    gsh_background_table *backgrounds;
    gsh_times_context times_context;
    int status;

    if (!require(state != NULL && launch != NULL)) _exit(125);
    if (!require(node_index < state_parse_storage(state)->node_count)) _exit(125);
    prepare_compound_child(state, launch);
    initialize_compound_evaluator(state, launch, &evaluator, &times_context);
    backgrounds = allocate_isolated_job_table();
    if (backgrounds == NULL) child_exec_error("evaluator job state", errno);
    evaluator.backgrounds = backgrounds;
    status = native_evaluate_node(&evaluator, node_index, 0);
    status = commit_compound_evaluator(state, launch, &evaluator, status);
    if (launch->commit[1] >= 0) (void)close(launch->commit[1]);
    if (launch->exec_outcome[1] >= 0) (void)close(launch->exec_outcome[1]);
    if (launch->exec_descriptors[1] >= 0) {
        (void)close(launch->exec_descriptors[1]);
    }
    if (launch->job_service[1] >= 0) (void)close(launch->job_service[1]);
    if (launch->resource[1] >= 0) (void)close(launch->resource[1]);
    _exit(status & 255);
}

static void close_compound_child_ends(compound_launch *launch)
{
    if (!require(launch != NULL)) return;
    if (!require(launch->gate[1] >= -1)) return;
    if (launch->gate[0] >= 0) (void)close(launch->gate[0]);
    if (launch->commit[1] >= 0) (void)close(launch->commit[1]);
    if (launch->exec_outcome[1] >= 0) (void)close(launch->exec_outcome[1]);
    if (launch->exec_descriptors[1] >= 0) {
        (void)close(launch->exec_descriptors[1]);
    }
    if (launch->directory[1] >= 0) (void)close(launch->directory[1]);
    if (launch->job_service[1] >= 0) (void)close(launch->job_service[1]);
    if (launch->resource[1] >= 0) (void)close(launch->resource[1]);
    launch->gate[0] = -1;
    launch->commit[1] = -1;
    launch->exec_outcome[1] = -1;
    launch->exec_descriptors[1] = -1;
    launch->directory[1] = -1;
    launch->job_service[1] = -1;
    launch->resource[1] = -1;
}

static void initialize_compound_parent_commit(shell_state *state,
                                              compound_launch *launch)
{
    if (!require(state != NULL)) return;
    if (!require(launch != NULL)) return;
    state->variable_commit_fd = launch->commit[0];
    state->job_service_socket = launch->job_service[0];
    state->exec_outcome_fd = launch->exec_outcome[0];
    state->exec_descriptor_socket = launch->exec_descriptors[0];
    state->directory_commit_socket = launch->directory[0];
    launch->commit[0] = -1;
    launch->job_service[0] = -1;
    launch->exec_outcome[0] = -1;
    launch->exec_descriptors[0] = -1;
    launch->directory[0] = -1;
    state->variable_commit_received = 0U;
    state->variable_commit_active = true;
    state->variable_commit_eof = false;
    state->variable_commit_invalid = false;
    (void)memset(state->variable_commit, 0, sizeof(*state->variable_commit));
    if (state->alias_commit_expected) {
        (void)memset(state->alias_commit, 0, sizeof(*state->alias_commit));
    }
    if (state->function_commit_expected) {
        (void)memset(&state->function_commit_header, 0,
               sizeof(state->function_commit_header));
        state->function_commit_header_complete = false;
    }
    if (state->command_cache_commit_expected) {
        (void)memset(&state->command_cache_commit_header, 0,
               sizeof(state->command_cache_commit_header));
        (void)memset(state->command_cache_scratch, 0,
               sizeof(*state->command_cache_scratch));
    }
    (void)memset(&state->option_commit, 0, sizeof(state->option_commit));
    (void)memset(&state->control_commit, 0, sizeof(state->control_commit));
    state->committed_exit_requested = false;
    state->committed_exit_status = 0;
}

static void release_compound_launch(compound_launch *launch)
{
    if (!require(launch != NULL)) return;
    if (!require(launch->signals_blocked)) return;
    if (launch->gate[1] >= 0) (void)close(launch->gate[1]);
    launch->gate[1] = -1;
    (void)sigprocmask(SIG_SETMASK, &launch->previous, NULL);
    launch->signals_blocked = false;
}

static bool handoff_managed_compound(shell_state *state,
                                     compound_launch *launch)
{
    int error;

    if (!require(state != NULL)) return false;
    if (!require(launch != NULL && launch->managed)) return false;
    state->current_job.foreground = false;
    state->current_job.silent = true;
    if (gsh_async_repl_attach(state->async_repl, state->async_dispatch_cell,
                              launch->pid, launch->pid,
                              launch->pty.master,
                              launch->resource[0]) == -1 ||
        register_managed_job(state, state->async_dispatch_cell,
                             launch->pid, launch->pid) == -1) {
        error = errno;
        (void)kill(launch->pid, SIGKILL);
        release_compound_launch(launch);
        close_variable_commit(state);
        output_format(state, "gsh: managed evaluator: %s\r\n",
                      strerror(error));
        gsh_async_repl_finish(state->async_repl,
                              state->async_dispatch_cell, 125 << 8, false);
        state->current_job.active = false;
        state->mode = MODE_EDITOR;
        abandon_pending_list(state);
        return false;
    }
    launch->pty.master = -1;
    launch->resource[0] = -1;
    state->mode = MODE_EDITOR;
    release_compound_launch(launch);
    queue_prompt(state);
    return true;
}

static bool handoff_foreground_compound(shell_state *state,
                                        compound_launch *launch)
{
    if (!require(state != NULL)) return false;
    if (!require(launch != NULL && !launch->managed)) return false;
    (void)setpgid(launch->pid, launch->pid);
    if (gsh_fault_should_fail(GSH_FAULT_TERMINAL_HANDOFF, EIO) ||
        tcsetattr(state->tty_fd, TCSANOW, &state->original_modes) == -1 ||
        tcsetpgrp(state->tty_fd, launch->pid) == -1) {
        int error = errno;

        state->current_job.foreground = false;
        state->current_job.silent = true;
        (void)kill(-launch->pid, SIGKILL);
        (void)kill(launch->pid, SIGKILL);
        release_compound_launch(launch);
        close_variable_commit(state);
        (void)enter_editor(state);
        output_format(state, "gsh: terminal handoff: %s\r\n",
                      strerror(error));
        abandon_pending_list(state);
        queue_prompt(state);
        return false;
    }
    state->terminal_changed = false;
    state->mode = MODE_FOREGROUND;
    release_compound_launch(launch);
    return true;
}

static void start_native_compound(shell_state *state, size_t node_index)
{
    compound_launch launch;
    bool managed;

    if (!require(state != NULL)) return;
    if (!require(state->parse_storage != NULL)) return;
    managed = state->async_repl != NULL && state_async_repl(state)->enabled;
    initialize_compound_launch(&launch, managed);
    if (!compound_capacity_is_available(state) ||
        !create_compound_channels(state, &launch) ||
        !block_compound_signals(state, &launch)) return;
    if (!initialize_compound_commits(state, &launch)) return;
    launch.owner_times_valid = gsh_times_snapshot(&launch.owner_times) == 0;
    launch.pid = managed && open_managed_pty(&launch.pty) == -1
              ? -1
              : (gsh_fault_should_fail(GSH_FAULT_EVALUATOR_FORK, EAGAIN) ? -1
                                                               : fork());
    if (launch.pid == 0) {
        run_compound_child(state, &launch, node_index);
    }
    close_compound_child_ends(&launch);
    if (launch.pid == -1) {
        int saved_errno = errno;

        reject_compound_start(state, &launch, "evaluator fork",
                              saved_errno);
        return;
    }

    if (managed && launch.pty.slave_hold >= 0) {
        (void)close(launch.pty.slave_hold);
        launch.pty.slave_hold = -1;
    }

    initialize_compound_parent_commit(state, &launch);
    initialize_job(&state->current_job, launch.pid, launch.pid,
                   &launch.pid, 1, 1, false, true, false);
    state->current_job.modes = state->original_modes;
    if (managed) {
        (void)handoff_managed_compound(state, &launch);
        return;
    }
    (void)handoff_foreground_compound(state, &launch);
}

typedef struct {
    native_evaluator evaluator;
    gsh_background_table backgrounds;
} native_script_session;

static void initialize_native_script_session(
    native_script_session *session, const char *parameter_zero,
    gsh_parse_storage *storage, gsh_native_pipeline *pipeline,
    gsh_variable_store *variables, gsh_variable_store *scope_base,
    gsh_variable_journal *scope_changes,
    gsh_positional_store *positionals, gsh_command_cache *command_cache,
    gsh_source_workspace_stack *source_workspaces,
    gsh_trap_store *traps,
    const char *default_path)
{
    if (command_cache == NULL || default_path == NULL || parameter_zero == NULL || pipeline == NULL || positionals == NULL || scope_base == NULL || scope_changes == NULL || session == NULL || source_workspaces == NULL || storage == NULL || traps == NULL || variables == NULL) {
        return;
    }
    native_evaluator *evaluator = &session->evaluator;

    (void)memset(session, 0, sizeof(*session));
    evaluator->exec_outcome_fd = -1;
    evaluator->exec_descriptor_socket = -1;
    evaluator->storage = storage;
    evaluator->pipeline = pipeline;
    evaluator->default_path = default_path;
    evaluator->shell_pid = (long)getpid();
    evaluator->parameter_zero = parameter_zero;
    evaluator->positionals = positionals;
    evaluator->command_cache = command_cache;
    evaluator->command_cache_base_generation = 1;
    gsh_options_initialize(&evaluator->options, false);
    evaluator->variables = variables;
    evaluator->aliases = &source_workspaces->root_aliases;
    evaluator->functions = &source_workspaces->root_functions;
    evaluator->function_scratch =
        &source_workspaces->root_function_scratch;
    evaluator->scope_base = scope_base;
    evaluator->scope_changes = scope_changes;
    evaluator->source_workspaces = source_workspaces;
    evaluator->traps = traps;
    gsh_background_initialize(&session->backgrounds);
    evaluator->backgrounds = &session->backgrounds;
}

static gsh_parse_result parse_native_script_text(
    native_script_session *session, const char *input, size_t input_length,
    char *alias_expansion, size_t alias_expansion_capacity,
    const char **parsed_input, size_t *parsed_length)
{
    if (alias_expansion == NULL || input == NULL || parsed_input == NULL || parsed_length == NULL || session == NULL) {
        return (gsh_parse_result){.status = GSH_PARSE_LIMIT};
    }
    native_evaluator *evaluator = &session->evaluator;

    if (gsh_aliases_count(evaluator->aliases) == 0U) {
        *parsed_input = input;
        *parsed_length = input_length;
        return gsh_parse(input, input_length,
                         (gsh_parse_storage *)evaluator->storage);
    }
    return gsh_alias_parse(
        input, input_length, evaluator->aliases,
        alias_expansion, alias_expansion_capacity,
        (gsh_parse_storage *)evaluator->storage, parsed_input,
        parsed_length);
}

static bool native_parsed_script_is_empty(
    const native_script_session *session, size_t root)
{
    if (session == NULL) {
        return false;
    }
    const gsh_parse_storage *storage = session->evaluator.storage;

    return root < storage->node_count &&
           storage->nodes[root].kind == GSH_AST_PROGRAM &&
           storage->nodes[root].first_child == GSH_AST_NONE;
}

static int execute_native_parsed(
    native_script_session *session, const char *input, size_t input_length,
    size_t root, gsh_variable_store *variables,
    gsh_variable_store *scratch)
{
    if (input == NULL || scratch == NULL || session == NULL || variables == NULL) {
        return -1;
    }
    native_evaluator *evaluator = &session->evaluator;

    (void)memcpy(scratch, variables, sizeof(*scratch));
    evaluator->input = input;
    evaluator->input_length = input_length;
    evaluator->variables = scratch;
    evaluator->journal = NULL;
    evaluator->alias_journal = NULL;
    evaluator->pipeline_scope = NULL;
    evaluator->source_depth = 0;
    evaluator->preflight = true;
    evaluator->fatal_error = false;
    evaluator->static_for_items = false;
    evaluator->tail_exec_single = false;
    evaluator->positional_mutation_possible = false;
    evaluator->directory_mutation_possible = false;
    evaluator->alias_mutation_possible = false;
    evaluator->function_mutation_possible = false;
    evaluator->command_cache_mutation_possible = false;
    evaluator->state_commit_invalid = false;
    if (!native_preflight_node(evaluator, root, 0)) {
        (void)fprintf(stderr, "gsh: native execution unsupported\n");
        return 2;
    }
    evaluator->variables = variables;
    evaluator->preflight = false;
    evaluator->fatal_error = false;
    return native_evaluate_node(evaluator, root, 0);
}

static int execute_native_script(
    const char *input, size_t input_length,
    native_script_session *session, gsh_parse_storage *storage,
    gsh_variable_store *variables, gsh_variable_store *scratch,
    size_t source_offset)
{
    if (session == NULL) {
        return -1;
    }
    native_evaluator *evaluator = &session->evaluator;
    size_t offset = 0;
    size_t complete_commands;
    int status = evaluator->last_status;

    if (input == NULL || session == NULL || storage == NULL ||
        variables == NULL || scratch == NULL) {
        errno = EINVAL;
        return 125;
    }

    /* ── Complete Commands Commit in Source Order ────────────────
     * Parsing a whole script hid the POSIX rule that earlier complete
     * commands run before a later syntax error. Alias definitions also become
     * visible only after their defining complete command has executed.
     * This bounded loop grows a candidate by physical lines until the parser
     * reports a complete command, then evaluates it against persistent state.
     * SIZE_MAX is the representable source bound; storage failures, not a
     * shell-selected line length, provide the operational resource ceiling.
     * ─────────────────────────────────────────────────────────────── */
    for (complete_commands = 0;
         offset < input_length &&
         complete_commands < SIZE_MAX;
         complete_commands++) {
        const char *parsed_input = input + offset;
        size_t parsed_length = 0;
        size_t end = offset;
        size_t lines;
        gsh_parse_result parsed;

        (void)memset(&parsed, 0, sizeof(parsed));
        for (lines = 0; lines < SIZE_MAX; lines++) {
            const char *newline = memchr(input + end, '\n',
                                         input_length - end);

            end = newline == NULL ? input_length
                                  : (size_t)(newline - input) + 1U;
            parsed = parse_native_script_text(
                session, input + offset, end - offset,
                evaluator_source_workspaces(evaluator)->root_alias_expansion,
                GSH_ALIAS_EXPANSION_CAP, &parsed_input, &parsed_length);
            if ((parsed.status != GSH_PARSE_INCOMPLETE &&
                 !input_line_continues(input + offset, end - offset)) ||
                end == input_length) {
                break;
            }
        }
        if (parsed.status != GSH_PARSE_OK) {
            (void)fprintf(stderr, "gsh: %s at byte %zu\n",
                    gsh_parse_status_name(parsed.status),
                    source_offset + offset + parsed.error_offset);
            status = 2;
            break;
        }
        if (gsh_options_enabled(&evaluator->options, GSH_OPTION_VERBOSE) &&
            emit_verbose_input(&descriptor_builtin_io, input + offset,
                               end - offset) == -1) {
            status = 125;
            break;
        }
        if (native_parsed_script_is_empty(session, parsed.root)) {
            offset = end;
            continue;
        }
        status = execute_native_parsed(session, parsed_input, parsed_length,
                                       parsed.root, variables, scratch);
        if (evaluator->fatal_error || evaluator->exiting) {
            break;
        }
        offset = end;
    }
    return status;
}

typedef struct {
    gsh_parse_storage *storage;
    gsh_native_pipeline *pipeline;
    gsh_variable_store *variables;
    gsh_variable_store *scratch;
    gsh_variable_store *scope_base;
    gsh_variable_journal *scope_changes;
    gsh_positional_store *positionals;
    gsh_command_cache *command_cache;
    gsh_source_workspace_stack *source_workspaces;
    gsh_trap_store *traps;
    char default_path[EXEC_PATH_CAP];
    native_script_session session;
} native_script_resources;

typedef struct {
    gsh_parse_storage storage;
    gsh_native_pipeline pipeline;
    gsh_variable_store variables;
    gsh_variable_store scratch;
    gsh_variable_store scope_base;
    gsh_variable_journal scope_changes;
    gsh_positional_store positionals;
    gsh_command_cache command_cache;
    gsh_source_workspace_stack source_workspaces;
    gsh_trap_store traps;
} native_script_storage;

static void release_native_script_resources(
    native_script_resources *resources)
{
    if (resources == NULL) {
        return;
    }
    (void)memset(resources, 0, sizeof(*resources));
}

static int initialize_native_script_resources(
    native_script_resources *resources, native_script_storage *storage,
    const char *parameter_zero,
    char *const *positional_parameters, size_t positional_count)
{
    if (parameter_zero == NULL || resources == NULL || storage == NULL) {
        return -1;
    }
    size_t default_path_length;

    (void)memset(resources, 0, sizeof(*resources));
    (void)memset(storage, 0, sizeof(*storage));
    resources->storage = &storage->storage;
    resources->pipeline = &storage->pipeline;
    resources->variables = &storage->variables;
    resources->scratch = &storage->scratch;
    resources->scope_base = &storage->scope_base;
    resources->scope_changes = &storage->scope_changes;
    resources->positionals = &storage->positionals;
    resources->command_cache = &storage->command_cache;
    resources->source_workspaces = &storage->source_workspaces;
    resources->traps = &storage->traps;
    if (gsh_variables_import(resources->variables, environ) == -1 ||
        gsh_positionals_assign(resources->positionals, positional_count,
                               positional_parameters) == -1 ||
        gsh_traps_initialize(resources->traps) == -1) {
        release_native_script_resources(resources);
        return -1;
    }
    gsh_command_cache_initialize(
        resources->command_cache,
        gsh_variables_path_generation(resources->variables));
    gsh_source_workspaces_initialize(resources->source_workspaces);
    default_path_length = confstr(
        _CS_PATH, resources->default_path, sizeof(resources->default_path));
    if (default_path_length == 0 ||
        default_path_length > sizeof(resources->default_path)) {
        (void)memcpy(resources->default_path, "/bin:/usr/bin", 14);
    }
    initialize_native_script_session(
        &resources->session, parameter_zero, resources->storage,
        resources->pipeline, resources->variables, resources->scope_base,
        resources->scope_changes, resources->positionals,
        resources->command_cache, resources->source_workspaces,
        resources->traps,
        resources->default_path);
    return 0;
}

static int execute_native_noninteractive(
    const char *input, size_t input_length,
    const char *parameter_zero, char *const *positional_parameters,
    size_t positional_count, const gsh_shell_options *options)
{
    if (input == NULL || options == NULL ||
        !gsh_options_validate(options)) {
        return -1;
    }
    static native_script_resources resources;
    static native_script_storage storage;
    int status;

    if (initialize_native_script_resources(
            &resources, &storage, parameter_zero, positional_parameters,
            positional_count) == -1) {
        perror("gsh: native allocation");
        return 125;
    }
    resources.session.evaluator.options = *options;
    status = execute_native_script(
        input, input_length, &resources.session, resources.storage,
        resources.variables, resources.scratch, 0);
    status = evaluate_native_traps_top(
        &resources.session.evaluator, status, NATIVE_TRAPS_EXIT);
    release_native_script_resources(&resources);
    return status;
}

enum { NATIVE_INPUT_READ_CAP = 16384 };

enum {
    NATIVE_INPUT_SLOW_CAP = 4 * GSH_SOURCE_INPUT_CAP,
    NATIVE_INPUT_ALIAS_CAP =
        NATIVE_INPUT_SLOW_CAP +
        GSH_ALIAS_EXPANSION_LIMIT * (GSH_ALIAS_VALUE_CAP + 1U) + 1U,
};

enum {
    NATIVE_INPUT_EOF = 0,
    NATIVE_INPUT_BYTE = 1,
    NATIVE_INPUT_INTERRUPTED = 2,
};

typedef struct {
    int descriptor;
    unsigned char bytes[NATIVE_INPUT_READ_CAP];
    size_t next;
    size_t used;
    size_t source_offset;
    bool shares_command_input;
    bool buffered;
} native_input_reader;

typedef enum {
    GSH_SOURCE_VIEW_DIRECT,
    GSH_SOURCE_VIEW_FILE,
} gsh_source_view_kind;

typedef struct {
    gsh_source_view_kind kind;
    const char *memory;
    size_t length;
    int descriptor;
    char *window;
    size_t window_capacity;
} gsh_source_view;

typedef struct {
    char source_window[NATIVE_INPUT_SLOW_CAP + 1U];
    char alias_expansion[NATIVE_INPUT_ALIAS_CAP];
} native_input_storage;

/* ── System Resources Replace Shell Line Limits ──────────────────
 * The original fixed command buffer rejected valid long input even when the
 * host still had memory and storage. Ordinary commands retain that preallocated
 * fast path. A command that outgrows it spills into one unlinked 0600 file with
 * close-on-exec ownership and is mapped only while parsing and executing that
 * complete command. Alias rewriting receives a private copy-on-write view of
 * the same file. This is the narrow post-initialization VM exception required
 * by POSIX unlimited lines; each resource and failure has one bounded owner.
 * ─────────────────────────────────────────────────────────────── */
typedef struct {
    char *text;
    native_input_storage *storage;
    size_t length;
    size_t source_offset;
    const char *parsed_text;
    size_t parsed_length;
    gsh_parse_result parsed;
    int spill_descriptor;
    unsigned char spill[NATIVE_INPUT_READ_CAP];
    size_t spill_used;
    gsh_source_view source_view;
    char *alias_expansion;
    size_t alias_capacity;
    bool alias_active;
    bool collecting;
} native_input_command;

static bool initialize_native_input_command(native_input_command *command,
                                            char *text,
                                            native_input_storage *storage)
{
    if (command == NULL || text == NULL || storage == NULL) return false;
    command->text = text;
    command->storage = storage;
    command->length = 0;
    command->source_offset = 0;
    command->parsed_text = NULL;
    command->parsed_length = 0;
    (void)memset(&command->parsed, 0, sizeof(command->parsed));
    command->spill_descriptor = -1;
    command->spill_used = 0;
    command->source_view.kind = GSH_SOURCE_VIEW_DIRECT;
    command->source_view.memory = text;
    command->source_view.length = 0;
    command->source_view.descriptor = -1;
    command->source_view.window = storage->source_window;
    command->source_view.window_capacity = sizeof(storage->source_window);
    command->alias_expansion = storage->alias_expansion;
    command->alias_capacity = sizeof(storage->alias_expansion);
    command->alias_active = false;
    command->collecting = false;
    return true;
}

static int release_native_input_views(native_input_command *command)
{
    int status = 0;

    if (command == NULL || command->source_view.window == NULL ||
        command->alias_expansion == NULL) return -1;
    if (command->alias_active) {
        (void)memset(command->alias_expansion, 0, command->alias_capacity);
        command->alias_active = false;
    }
    if (command->source_view.kind == GSH_SOURCE_VIEW_FILE) {
        (void)memset(command->source_view.window, 0,
               command->source_view.window_capacity);
        command->source_view.kind = GSH_SOURCE_VIEW_DIRECT;
        command->source_view.memory = command->text;
        command->source_view.length = 0;
        command->source_view.descriptor = -1;
    }
    return status;
}

static int release_native_input_command(native_input_command *command)
{
    char *text;
    native_input_storage *storage;
    int status;

    if (command == NULL || command->spill_descriptor < -1) return -1;
    if (!require(command->text != NULL)) return -1;
    if (!require(command->storage != NULL)) return -1;
    text = command->text;
    storage = command->storage;
    status = release_native_input_views(command);
    if (command->spill_descriptor >= 0) {
        (void)memset(command->spill, 0, sizeof(command->spill));
        if (close(command->spill_descriptor) == -1) {
            status = -1;
        }
    }
    return initialize_native_input_command(command, text, storage)
               ? status
               : -1;
}

static int write_native_input_spill(int descriptor,
                                    const unsigned char *bytes,
                                    size_t length)
{
    size_t attempts;
    size_t written = 0;

    if (descriptor < 0 || bytes == NULL) {
        errno = EINVAL;
        return -1;
    }
    for (attempts = 0; written < length && attempts < SIZE_MAX;
         attempts++) {
        ssize_t count = gsh_fault_should_fail(GSH_FAULT_INPUT_SPILL_WRITE, EIO)
                            ? -1
                            : write(descriptor, bytes + written,
                                    length - written);

        if (count > 0) {
            written += (size_t)count;
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            errno = count == 0 ? EIO : errno;
            return -1;
        }
    }
    if (written != length) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

static int open_native_input_spill(native_input_command *command)
{
    char path[] = "/tmp/gsh-input-XXXXXX";
    int descriptor;

    if (command == NULL || command->spill_descriptor != -1 ||
        command->length != NONINTERACTIVE_INPUT_FAST_CAP) {
        errno = EINVAL;
        return -1;
    }
    descriptor = gsh_fault_should_fail(GSH_FAULT_INPUT_SPILL_OPEN, EMFILE)
                     ? -1
                     : mkstemp(path);
    if (descriptor == -1) {
        return -1;
    }
    if (gsh_fault_should_fail(GSH_FAULT_INPUT_SPILL_CLOEXEC, EIO) ||
        set_fd_flags(descriptor, F_GETFD, FD_CLOEXEC) == -1) {
        int saved_errno = errno;

        (void)unlink(path);
        (void)close(descriptor);
        errno = saved_errno;
        return -1;
    }
    if (gsh_fault_should_fail(GSH_FAULT_INPUT_SPILL_UNLINK, EIO)) {
        int saved_errno = errno;

        (void)unlink(path);
        (void)close(descriptor);
        errno = saved_errno;
        return -1;
    }
    if (unlink(path) == -1) {
        int saved_errno = errno;

        (void)close(descriptor);
        errno = saved_errno;
        return -1;
    }
    command->spill_descriptor = descriptor;
    if (write_native_input_spill(
            descriptor, (const unsigned char *)command->text,
            command->length) == -1) {
        return -1;
    }
    return 0;
}

static int flush_native_input_spill(native_input_command *command)
{
    if (command == NULL || command->spill_used > sizeof(command->spill)) {
        errno = EINVAL;
        return -1;
    }
    if (command->spill_used == 0) {
        return 0;
    }
    if (command->spill_descriptor < 0 ||
        write_native_input_spill(command->spill_descriptor,
                                 command->spill,
                                 command->spill_used) == -1) {
        return -1;
    }
    command->spill_used = 0;
    return 0;
}

static int append_native_input_byte(native_input_command *command,
                                    unsigned char byte)
{
    if (command == NULL || command->spill_used > sizeof(command->spill)) {
        errno = EINVAL;
        return -1;
    }
    if (command->length == NATIVE_INPUT_SLOW_CAP) {
        errno = EFBIG;
        return -1;
    }
    if (command->spill_descriptor == -1 &&
        command->length < NONINTERACTIVE_INPUT_FAST_CAP) {
        command->text[command->length] = (char)byte;
    } else {
        if (command->spill_descriptor == -1 &&
            open_native_input_spill(command) == -1) {
            return -1;
        }
        if (command->spill_used == sizeof(command->spill) &&
            flush_native_input_spill(command) == -1) {
            return -1;
        }
        command->spill[command->spill_used++] = byte;
    }
    command->length++;
    return 0;
}

static int initialize_native_input_reader(native_input_reader *reader,
                                          int descriptor,
                                          bool shares_command_input)
{
    off_t offset;

    if (!require(reader != NULL)) return -1;
    if (!require(descriptor >= 0)) return -1;
    (void)memset(reader, 0, sizeof(*reader));
    reader->descriptor = descriptor;
    reader->shares_command_input = shares_command_input;
    if (shares_command_input) {
        int flags = gsh_fault_should_fail(GSH_FAULT_INPUT_MODE, EIO)
                        ? -1
                        : fcntl(descriptor, F_GETFL);

        if (flags == -1 ||
            ((flags & O_NONBLOCK) != 0 &&
             fcntl(descriptor, F_SETFL, flags & ~O_NONBLOCK) == -1)) {
            return -1;
        }
    }
    offset = lseek(descriptor, 0, SEEK_CUR);
    reader->buffered = !shares_command_input || offset != (off_t)-1;
    return 0;
}

static int read_native_input_byte(native_input_reader *reader,
                                  unsigned char *byte)
{
    if (!require(reader != NULL)) return -1;
    if (!require(byte != NULL)) return -1;
    if (!require(reader->next <= reader->used &&
                 reader->used <= sizeof(reader->bytes))) return -1;
    if (reader->next < reader->used) {
        *byte = reader->bytes[reader->next++];
        return NATIVE_INPUT_BYTE;
    }
    reader->next = 0;
    reader->used = 0;
    {
        size_t capacity = reader->buffered ? sizeof(reader->bytes) : 1U;
        ssize_t count = gsh_fault_should_fail(GSH_FAULT_INPUT_READ, EIO)
                            ? -1
                            : read(reader->descriptor, reader->bytes,
                                   capacity);

        if (count > 0) {
            reader->used = (size_t)count;
            *byte = reader->bytes[reader->next++];
            return NATIVE_INPUT_BYTE;
        }
        if (count == 0) {
            return NATIVE_INPUT_EOF;
        }
        return errno == EINTR ? NATIVE_INPUT_INTERRUPTED : -1;
    }
}

static int synchronize_native_input(native_input_reader *reader)
{
    size_t unread;

    if (!require(reader != NULL)) return -1;
    if (!require(reader->next <= reader->used)) return -1;
    if (!reader->shares_command_input || !reader->buffered) {
        return 0;
    }
    unread = reader->used - reader->next;
    if (unread != 0 &&
        lseek(reader->descriptor, -(off_t)unread, SEEK_CUR) == (off_t)-1) {
        return -1;
    }
    reader->next = 0;
    reader->used = 0;
    return 0;
}

static int resize_native_input_spill(native_input_command *command,
                                     size_t length)
{
    if (command == NULL || command->spill_descriptor < 0 ||
        length > (size_t)INT64_MAX) {
        errno = length > (size_t)INT64_MAX ? EFBIG : EINVAL;
        return -1;
    }
    if (gsh_fault_should_fail(GSH_FAULT_INPUT_SPILL_RESIZE, ENOSPC) ||
        ftruncate(command->spill_descriptor, (off_t)length) == -1) {
        return -1;
    }
    return 0;
}

static int map_native_input_source(native_input_command *command)
{
    size_t offset = 0;
    size_t attempts;

    if (command == NULL || command->spill_descriptor < 0 ||
        command->length <= NONINTERACTIVE_INPUT_FAST_CAP) {
        errno = EINVAL;
        return -1;
    }
    if (flush_native_input_spill(command) == -1 ||
        resize_native_input_spill(command, command->length) == -1) {
        return -1;
    }
    if (gsh_fault_should_fail(GSH_FAULT_INPUT_MAP, ENOMEM) ||
        command->length >= command->source_view.window_capacity) {
        errno = command->length >= command->source_view.window_capacity
                    ? EFBIG
                    : errno;
        return -1;
    }
    for (attempts = 0;
         offset < command->length &&
         attempts < NATIVE_INPUT_SLOW_CAP / NATIVE_INPUT_READ_CAP + 1U;
         attempts++) {
        ssize_t count = pread(command->spill_descriptor,
                              command->source_view.window + offset,
                              command->length - offset, (off_t)offset);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count != -1 || errno != EINTR) {
            return -1;
        }
    }
    if (offset != command->length) {
        errno = EIO;
        return -1;
    }
    command->source_view.window[command->length] = '\0';
    command->source_view.kind = GSH_SOURCE_VIEW_FILE;
    command->source_view.memory = command->source_view.window;
    command->source_view.length = command->length;
    command->source_view.descriptor = command->spill_descriptor;
    return 0;
}

static int map_native_alias_expansion(native_input_command *command)
{
    const size_t maximum_growth =
        (size_t)GSH_ALIAS_EXPANSION_LIMIT *
        ((size_t)GSH_ALIAS_VALUE_CAP + 1U);
    size_t capacity;

    if (command == NULL || command->spill_descriptor < 0 ||
        command->source_view.kind != GSH_SOURCE_VIEW_FILE) {
        errno = EINVAL;
        return -1;
    }
    if (command->length > SIZE_MAX - maximum_growth - 1U) {
        errno = EOVERFLOW;
        return -1;
    }
    capacity = command->length + maximum_growth + 1U;
    if (resize_native_input_spill(command, capacity) == -1) {
        return -1;
    }
    if (gsh_fault_should_fail(GSH_FAULT_INPUT_ALIAS_MAP, ENOMEM) ||
        capacity > command->alias_capacity) {
        errno = capacity > command->alias_capacity ? EFBIG : errno;
        return -1;
    }
    command->alias_active = true;
    return 0;
}

static int parse_native_input_command(native_input_command *command,
                                      native_script_session *session)
{
    const char *input;
    char *alias_expansion;
    size_t alias_capacity;

    if (command == NULL || session == NULL ||
        command->source_view.kind != GSH_SOURCE_VIEW_DIRECT ||
        command->alias_active) {
        errno = EINVAL;
        return -1;
    }
    if (!require(session->evaluator.source_workspaces != NULL)) return -1;
    input = command->text;
    alias_expansion =
        session->evaluator.source_workspaces->root_alias_expansion;
    alias_capacity = GSH_ALIAS_EXPANSION_CAP;
    if (command->spill_descriptor >= 0) {
        if (map_native_input_source(command) == -1) {
            return -1;
        }
        input = command->source_view.memory;
        if (gsh_aliases_count(session->evaluator.aliases) != 0) {
            if (map_native_alias_expansion(command) == -1) {
                return -1;
            }
            alias_expansion = command->alias_expansion;
            alias_capacity = command->alias_capacity;
        }
    } else {
        command->text[command->length] = '\0';
    }
    command->parsed_text = input;
    command->parsed_length = command->length;
    command->parsed = parse_native_script_text(
        session, input, command->length, alias_expansion, alias_capacity,
        &command->parsed_text, &command->parsed_length);
    if (command->alias_active &&
        command->parsed_text != command->alias_expansion) {
        (void)memset(command->alias_expansion, 0, command->alias_capacity);
        command->alias_active = false;
    }
    return 0;
}

static void begin_native_input_command(native_input_reader *reader,
                                       native_input_command *command)
{
    if (reader == NULL || command == NULL) return;
    if (!command->collecting) {
        if (command->spill_descriptor != -1 || command->length != 0U) return;
        command->source_offset = reader->source_offset;
        command->collecting = true;
    }
}

static int append_native_reader_byte(native_input_reader *reader,
                                     native_input_command *command,
                                     unsigned char byte)
{
    if (!require(reader != NULL)) return -1;
    if (!require(command != NULL)) return -1;
    if (byte == '\0') {
        errno = EILSEQ;
        return -1;
    }
    if (append_native_input_byte(command, byte) == -1) {
        return -1;
    }
    if (reader->source_offset == SIZE_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    reader->source_offset++;
    return 0;
}

static int read_native_input_command(
    native_input_reader *reader, native_script_session *session,
    native_input_command *command)
{
    size_t scanned;

    if (reader == NULL || session == NULL || command == NULL ||
        command->source_view.kind != GSH_SOURCE_VIEW_DIRECT ||
        command->alias_active) {
        errno = EINVAL;
        return -1;
    }
    begin_native_input_command(reader, command);
    for (scanned = 0; scanned < SIZE_MAX; scanned++) {
        unsigned char byte = 0;
        int read_status = read_native_input_byte(reader, &byte);
        bool parse_now = read_status == NATIVE_INPUT_EOF || byte == '\n';

        if (read_status == -1) {
            return -1;
        }
        if (read_status == NATIVE_INPUT_INTERRUPTED) {
            return NATIVE_INPUT_INTERRUPTED;
        }
        if (read_status != NATIVE_INPUT_EOF &&
            read_status != NATIVE_INPUT_BYTE) {
            errno = EIO;
            return -1;
        }
        if (read_status == NATIVE_INPUT_BYTE &&
            append_native_reader_byte(reader, command, byte) == -1) {
            return -1;
        }
        if (!parse_now) {
            continue;
        }
        if (command->length == 0 && read_status == NATIVE_INPUT_EOF) {
            command->collecting = false;
            return NATIVE_INPUT_EOF;
        }
        if (parse_native_input_command(command, session) == -1) {
            return -1;
        }
        if ((command->parsed.status == GSH_PARSE_INCOMPLETE ||
             input_line_continues(command->source_view.memory,
                                  command->length)) &&
            read_status != NATIVE_INPUT_EOF) {
            if (release_native_input_views(command) == -1) {
                return -1;
            }
            continue;
        }
        command->collecting = false;
        return NATIVE_INPUT_BYTE;
    }
    errno = EOVERFLOW;
    return -1;
}

static int report_native_input_error(const char *source,
                                     native_input_command *command,
                                     int input_errno)
{
    if (!require(source != NULL && command != NULL)) return 125;
    if (!require(input_errno != 0)) return 125;
    (void)release_native_input_command(command);
    errno = input_errno;
    if (input_errno == EILSEQ) {
        (void)fprintf(stderr, "gsh: %s contains a null byte\n", source);
        return 2;
    }
    (void)fprintf(stderr, "gsh: %s: %s\n", source, strerror(input_errno));
    return 125;
}

static int finish_native_descriptor_session(
    native_script_resources *resources, native_input_command *command,
    const char *source, int status)
{
    if (!require(resources != NULL && command != NULL)) return 125;
    if (!require(source != NULL)) return 125;
    if (release_native_input_command(command) == -1 && status == 0) {
        (void)fprintf(stderr, "gsh: %s input cleanup: %s\n", source,
                strerror(errno));
        status = 125;
    }
    status = evaluate_native_traps_top(
        &resources->session.evaluator, status, NATIVE_TRAPS_EXIT);
    release_native_script_resources(resources);
    return status;
}

static int evaluate_native_input_command(
    native_script_resources *resources,
    const native_input_command *command,
    const gsh_shell_options *options, int preceding_status)
{
    if (!require(resources != NULL && command != NULL)) return 125;
    if (!require(options != NULL)) return 125;
    if (gsh_options_enabled(options, GSH_OPTION_NOEXEC)) {
        return preceding_status;
    }
    return execute_native_parsed(
        &resources->session, command->parsed_text, command->parsed_length,
        command->parsed.root, resources->variables, resources->scratch);
}

static bool emit_native_input_verbose(const gsh_shell_options *options,
                                      native_input_command *command,
                                      int *status)
{
    if (!require(options != NULL && command != NULL && status != NULL)) {
        return false;
    }
    if (!gsh_options_enabled(options, GSH_OPTION_VERBOSE) ||
        emit_verbose_input(&descriptor_builtin_io,
                           command->source_view.memory,
                           command->length) == 0) return true;
    *status = 125;
    (void)release_native_input_command(command);
    return false;
}

static bool accept_native_input_parse(const char *source,
                                      native_input_command *command,
                                      int *status)
{
    if (!require(source != NULL && command != NULL && status != NULL)) {
        return false;
    }
    if (command->parsed.status == GSH_PARSE_OK) return true;
    (void)fprintf(stderr, "gsh: %s at byte %zu\n",
                  gsh_parse_status_name(command->parsed.status),
                  command->source_offset + command->parsed.error_offset);
    *status = 2;
    (void)release_native_input_command(command);
    return false;
}

static int execute_native_descriptor(
    int descriptor, const char *source, const char *parameter_zero,
    char *const *positional_parameters, size_t positional_count,
    bool shares_command_input, const gsh_shell_options *options)
{
    static native_script_resources resources;
    static native_script_storage storage;
    static native_input_storage input_storage;
    native_input_reader reader;
    native_input_command command;
    size_t complete_commands;
    int status = 0;

    if (descriptor < 0 || source == NULL || parameter_zero == NULL ||
        options == NULL || !gsh_options_validate(options)) {
        errno = EINVAL;
        return 125;
    }
    if (initialize_native_script_resources(
            &resources, &storage, parameter_zero, positional_parameters,
            positional_count) == -1) {
        perror("gsh: native allocation");
        return 125;
    }
    resources.session.evaluator.options = *options;
    if (!initialize_native_input_command(
            &command, resources.source_workspaces->root_input,
            &input_storage)) {
        release_native_script_resources(&resources);
        return 125;
    }
    if (initialize_native_input_reader(&reader, descriptor,
                                       shares_command_input) == -1) {
        (void)fprintf(stderr, "gsh: %s input mode: %s\n", source,
                strerror(errno));
        release_native_script_resources(&resources);
        return 125;
    }
    for (complete_commands = 0; complete_commands < SIZE_MAX;
         complete_commands++) {
        int read_status = read_native_input_command(
            &reader, &resources.session, &command);

        if (read_status == NATIVE_INPUT_EOF) {
            break;
        }
        /* ── Idle Input Does Not Delay Traps ─────────────────────
         * Retrying an interrupted descriptor read kept a non-interactive
         * shell asleep until another byte arrived.  The input accumulator now
         * retains its partial complete command across EINTR and yields to the
         * evaluator.  Trap code runs with the preceding command's status, then
         * the same byte stream resumes without read-ahead or source loss.
         * ─────────────────────────────────────────────────────────────── */
        if (read_status == NATIVE_INPUT_INTERRUPTED) {
            status = evaluate_native_traps_top(
                &resources.session.evaluator, status,
                NATIVE_TRAPS_PENDING);
            if (resources.session.evaluator.fatal_error ||
                resources.session.evaluator.exiting) {
                break;
            }
            continue;
        }
        if (read_status == -1) {
            int input_errno = errno;

            status = report_native_input_error(source, &command,
                                               input_errno);
            break;
        }
        if (!accept_native_input_parse(source, &command, &status)) break;
        if (!emit_native_input_verbose(
                &resources.session.evaluator.options,
                &command, &status)) break;
        if (native_parsed_script_is_empty(
                &resources.session, command.parsed.root)) {
            if (release_native_input_command(&command) == -1) {
                (void)fprintf(stderr, "gsh: %s input cleanup: %s\n", source,
                        strerror(errno));
                status = 125;
                break;
            }
            continue;
        }
        if (synchronize_native_input(&reader) == -1) {
            (void)fprintf(stderr, "gsh: %s input synchronization: %s\n",
                    source, strerror(errno));
            status = 125;
            (void)release_native_input_command(&command);
            break;
        }
        status = evaluate_native_input_command(
            &resources, &command, options, status);
        if (release_native_input_command(&command) == -1) {
            (void)fprintf(stderr, "gsh: %s input cleanup: %s\n", source,
                    strerror(errno));
            status = 125;
            break;
        }
        if (resources.session.evaluator.fatal_error ||
            resources.session.evaluator.exiting) {
            break;
        }
    }
    return finish_native_descriptor_session(&resources, &command, source,
                                            status);
}

static int execute_native_file(
    const char *path, char *const *positional_parameters,
    size_t positional_count, const gsh_shell_options *options)
{
    if ((positional_count != 0U && positional_parameters == NULL) ||
        options == NULL) {
        return -1;
    }
    int descriptor;
    int status;

    if (path == NULL || path[0] == '\0') {
        (void)fprintf(stderr, "gsh: empty command file\n");
        return 2;
    }
    descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor == -1) {
        (void)fprintf(stderr, "gsh: %s: %s\n", path, strerror(errno));
        return 2;
    }
    status = execute_native_descriptor(
        descriptor, path, path, positional_parameters, positional_count,
        false, options);
    if (close(descriptor) == -1 && status == 0) {
        (void)fprintf(stderr, "gsh: %s: %s\n", path, strerror(errno));
        status = 125;
    }
    return status;
}

static int exec_noninteractive(int argc, char **argv,
                               const gsh_invocation *invocation)
{
    const char *parameter_zero;
    char **positionals;
    size_t positional_count;

    if (argc <= 0 || argv == NULL || invocation == NULL) return -1;
    parameter_zero = invocation->parameter_zero_index < (size_t)argc
                         ? argv[invocation->parameter_zero_index] : argv[0];
    positionals = invocation->positional_index < (size_t)argc
                      ? argv + invocation->positional_index : NULL;
    positional_count = invocation->positional_index < (size_t)argc
                           ? (size_t)argc - invocation->positional_index : 0U;
    if (invocation->mode == GSH_INVOCATION_COMMAND) {
        if (gsh_options_enabled(&invocation->options, GSH_OPTION_NOEXEC)) {
            return check_native_syntax(argv[invocation->source_index]);
        }
        return execute_native_noninteractive(
            argv[invocation->source_index],
            strlen(argv[invocation->source_index]), parameter_zero,
            positionals, positional_count, &invocation->options);
    }
    if (invocation->mode == GSH_INVOCATION_FILE) {
        return execute_native_file(
            argv[invocation->source_index], positionals, positional_count,
            &invocation->options);
    }
    return execute_native_descriptor(
        STDIN_FILENO, "standard input", argv[0], positionals,
        positional_count, true, &invocation->options);
}

/* ── Process ABI Is Converted at the Entry Boundary ─────────────
 * C supplies argc and argv with pointer shapes the shell cannot redefine.
 * CANON-EXCEPTION: C-PROCESS-ABI ends at this entry adapter: option branches
 * validate argc before indexing and pass explicit counts into bounded stores.
 * Interactive state and storage are static, so argv is never retained there.
 * Invocation and conformance gates exercise every accepted entry form.
 * ─────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    static shell_state state;
    static interactive_storage storage;
    gsh_invocation invocation;
    gsh_shell_options native_only_options;
    int status;

    (void)setlocale(LC_ALL, "");
    gsh_fault_initialize();
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(stdout);
        return 0;
    }
    if (gsh_fault_should_fail(GSH_FAULT_SHELL_EXECUTABLE_RESOLUTION, EIO) ||
        initialize_shell_executable(argv[0]) == -1) {
        perror("gsh: executable resolution");
        return 125;
    }
    if (argc >= 4 && strcmp(argv[1], "--native-only") == 0 &&
        strcmp(argv[2], "-c") == 0) {
        const char *parameter_zero = argc >= 5 ? argv[4] : argv[0];
        char **positionals = argc >= 6 ? argv + 5 : NULL;
        size_t positional_count = argc >= 6 ? (size_t)argc - 5U : 0;

        gsh_options_initialize(&native_only_options, false);
        return execute_native_noninteractive(
            argv[3], strlen(argv[3]), parameter_zero, positionals,
            positional_count, &native_only_options);
    }
    if (gsh_invocation_parse(argc, argv, isatty(STDIN_FILENO) != 0,
                             &invocation) == -1) {
        print_usage(stderr);
        return 2;
    }
    if (invocation.mode != GSH_INVOCATION_INTERACTIVE) {
        return exec_noninteractive(argc, argv, &invocation);
    }
    if (initialize_interactive(&state, &storage, argv[0]) == -1) {
        perror("gsh: interactive initialization");
        cleanup(&state);
        return 1;
    }
    state.options = invocation.options;
    state.options.enabled |= GSH_OPTION_INTERACTIVE;
    initialize_history(&state);
    if (start_redirection_worker(&state) == -1) {
        state.redirection_worker_failures++;
    }
    start_journal_worker(&state);

    status = run_reactor(&state);
    cleanup(&state);
    return status;
}
