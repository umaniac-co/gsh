#ifndef GSH_NATIVE_PLAN_H
#define GSH_NATIVE_PLAN_H

#include "posix_parser.h"
#include "builtin_common.h"
#include "shell_variables.h"

#include <stdbool.h>
#include <stddef.h>

enum {
    GSH_NATIVE_PIPELINE_CAP = 32,
    GSH_NATIVE_ARGUMENT_CAP = 128,
    GSH_NATIVE_ASSIGNMENT_CAP = 64,
    GSH_NATIVE_REDIRECT_CAP = 32,
    GSH_NATIVE_TEXT_CAP = 16384,
    GSH_NATIVE_HEREDOC_CAP = 32,
    GSH_NATIVE_HEREDOC_TEXT_CAP = 65536,
    GSH_NATIVE_ENVIRONMENT_CAP = 4096,
};

typedef enum {
    GSH_NATIVE_PLAN_OK = 0,
    GSH_NATIVE_PLAN_UNSUPPORTED,
    GSH_NATIVE_PLAN_LIMIT,
    GSH_NATIVE_PLAN_ERROR,
    GSH_NATIVE_PLAN_DEFERRED,
} gsh_native_plan_status;

enum { GSH_NATIVE_SUBSTITUTION_CAP = 32 };

typedef struct {
    uint32_t offset;
    uint32_t length;
    int32_t exit_status;
} gsh_native_substitution_result;

typedef struct {
    gsh_native_substitution_result results[GSH_NATIVE_SUBSTITUTION_CAP];
    size_t count;
    size_t cursor;
    size_t text_used;
    char text[GSH_NATIVE_HEREDOC_TEXT_CAP];
    char request[GSH_NATIVE_TEXT_CAP];
    size_t request_length;
    bool execute;
    bool pending;
} gsh_native_substitution_state;

typedef enum {
    GSH_NATIVE_VARIABLE_OVERLAY,
    GSH_NATIVE_VARIABLE_LIVE,
} gsh_native_variable_mode;

typedef struct {
    gsh_native_variable_mode mode;
    gsh_variable_store *variables;
    gsh_variable_journal *journal;
    uint64_t journal_generation;
    const gsh_variable_store *scope_base;
    gsh_variable_journal *scope_changes;
    size_t command_count;
    size_t current_scope;
    unsigned int attributes;
    bool isolated;
    bool mutated;
    bool preflight;
    bool assignment_fault_enabled;
    unsigned long assignment_fault_trigger;
    unsigned long *assignment_fault_calls;
    bool *fatal_error;
    const gsh_builtin_io *diagnostic_io;
    bool carriage_return;
} gsh_native_variable_state;

typedef enum {
    GSH_NATIVE_PATHNAME_REJECT = 0,
    GSH_NATIVE_PATHNAME_PREFLIGHT,
    GSH_NATIVE_PATHNAME_EXECUTE,
} gsh_native_pathname_mode;

typedef struct {
    int last_status;
    long shell_pid;
    long last_background_pid;
    const char *parameter_zero;
    char *const *positional_parameters;
    size_t positional_count;
    const char *option_flags;
    char *const *environment;
    gsh_native_variable_state *variable_state;
    gsh_native_substitution_state *substitutions;
    bool *command_substitution_performed;
    int *command_substitution_status;
    gsh_native_pathname_mode pathname_mode;
    bool defer_complex_patterns;
    bool *deferred_work;
    bool nounset;
    bool preflight;
} gsh_native_expansion_context;

typedef struct {
    int descriptor;
    gsh_token_kind operator_kind;
    char *target;
    int duplicate_descriptor;
    bool close_descriptor;
    size_t heredoc_index;
} gsh_native_redirect;

typedef struct {
    char *body;
    size_t length;
} gsh_native_heredoc;

typedef struct {
    char *argv[GSH_NATIVE_ARGUMENT_CAP + 1];
    size_t argc;
    char *assignments[GSH_NATIVE_ASSIGNMENT_CAP];
    size_t assignment_count;
    gsh_native_redirect redirects[GSH_NATIVE_REDIRECT_CAP];
    size_t redirect_count;
    bool expansion_error;
    bool command_suppresses_functions;
    bool command_uses_default_path;
    bool command_regular_context;
    bool command_substitution_performed;
    int command_substitution_status;
} gsh_native_command;

typedef struct {
    gsh_native_command commands[GSH_NATIVE_PIPELINE_CAP];
    size_t command_count;
    gsh_native_heredoc heredocs[GSH_NATIVE_HEREDOC_CAP];
    size_t heredoc_count;
    bool negated;
    char text[GSH_NATIVE_TEXT_CAP];
    unsigned char provenance[GSH_NATIVE_TEXT_CAP];
    size_t text_used;
    char heredoc_text[GSH_NATIVE_HEREDOC_TEXT_CAP];
    size_t heredoc_text_used;
} gsh_native_pipeline;

gsh_native_plan_status gsh_native_plan_pipeline_with_context(
    const char *input, const gsh_parse_storage *storage, size_t root,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline);
gsh_native_plan_status gsh_native_plan_pipeline_node_with_context(
    const char *input, const gsh_parse_storage *storage,
    size_t pipeline_node, const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline);
gsh_native_plan_status gsh_native_expand_scalar(
    const char *input, gsh_word_ref word,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *scratch, char **expanded);
gsh_native_plan_status gsh_native_expand_words(
    const char *input, const gsh_word_ref *words, size_t word_count,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *scratch,
    char *expanded[GSH_NATIVE_ARGUMENT_CAP], size_t *expanded_count);
gsh_native_plan_status gsh_native_plan_redirects_with_context(
    const char *input, const gsh_parse_storage *storage, size_t node_index,
    const gsh_native_expansion_context *context,
    gsh_native_pipeline *pipeline);
void gsh_native_substitutions_initialize(
    gsh_native_substitution_state *state, bool execute);
void gsh_native_substitutions_rewind(gsh_native_substitution_state *state);
const char *gsh_native_substitution_request(
    const gsh_native_substitution_state *state, size_t *length);
char *gsh_native_substitution_output(gsh_native_substitution_state *state,
                                     size_t *capacity);
gsh_native_plan_status gsh_native_substitution_complete(
    gsh_native_substitution_state *state, size_t length, int exit_status);

#endif
