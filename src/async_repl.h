#ifndef GSH_ASYNC_REPL_H
#define GSH_ASYNC_REPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "resource_actions.h"

enum {
    GSH_ASYNC_CELL_CAP = 16,
    GSH_ASYNC_JOB_CAP = 8,
    GSH_ASYNC_COMMAND_CAP = 4096,
    GSH_ASYNC_PROMPT_CAP = 160,
    GSH_ASYNC_CELL_INPUT_CAP = 4096,
    GSH_ASYNC_CELL_OUTPUT_CAP = 65536,
    GSH_ASYNC_VIEW_ROWS = 10240,
    GSH_ASYNC_TERMINAL_ROW_CAP = 256,
    GSH_ASYNC_VIEW_COLUMNS = 256,
    GSH_ASYNC_VIEW_BYTES = GSH_ASYNC_VIEW_COLUMNS * 4 + 64,
    GSH_ASYNC_PASSTHROUGH_SEQUENCE_CAP = 64,
    GSH_ASYNC_RESOURCE_CAP = 256,
    GSH_ASYNC_NATIVE_RESOURCE_CAP = 256,
    GSH_ASYNC_RENDER_CAP =
        GSH_ASYNC_TERMINAL_ROW_CAP * (GSH_ASYNC_VIEW_BYTES + 80) + 256,
};

typedef enum {
    GSH_ASYNC_QUEUED = 1,
    GSH_ASYNC_STARTING,
    GSH_ASYNC_RUNNING,
    GSH_ASYNC_STOPPED,
    GSH_ASYNC_DONE,
    GSH_ASYNC_FAILED,
    GSH_ASYNC_CANCELLED,
    GSH_ASYNC_REJECTED,
} gsh_async_cell_state;

typedef struct {
    bool occupied;
    bool barrier;
    bool blocks_independent;
    bool status_dependency;
    bool control;
    bool output_closed;
    bool output_truncated;
    bool focused;
    bool input_requested;
    bool input_probe_pending;
    bool fullscreen;
    bool fullscreen_presented;
    bool fullscreen_recorded;
    bool native_preview;
    bool preview_split;
    bool preview_frame_active;
    bool preview_frame_placed;
    bool autofocus_suppressed;
    unsigned char escape_state;
    unsigned char passthrough_state;
    uint64_t id;
    uint64_t output_generation;
    gsh_async_cell_state state;
    pid_t pid;
    pid_t pgid;
    int pty_fd;
    int resource_fd;
    int wait_status;
    char prompt[GSH_ASYNC_PROMPT_CAP];
    size_t prompt_length;
    char command[GSH_ASYNC_COMMAND_CAP];
    size_t command_length;
    size_t preview_separator_column;
    uint32_t preview_frame_generation;
    uint32_t preview_frame_id;
    uint32_t preview_frame_format;
    uint32_t preview_frame_total;
    uint32_t preview_frame_received;
    uint32_t preview_frame_row;
    uint32_t preview_frame_column;
    uint32_t preview_frame_rows;
    uint32_t preview_frame_columns;
    char launch_directory[4096];
    char input[GSH_ASYNC_CELL_INPUT_CAP];
    size_t input_offset;
    size_t input_length;
    char output[GSH_ASYNC_CELL_OUTPUT_CAP];
    size_t output_length;
    size_t output_line_start;
    size_t output_cursor;
    char passthrough_utf8[4];
    unsigned char passthrough_utf8_length;
    unsigned char passthrough_utf8_expected;
    char passthrough_sequence[GSH_ASYNC_PASSTHROUGH_SEQUENCE_CAP];
    size_t passthrough_sequence_length;
} gsh_async_cell;

typedef struct {
    bool occupied;
    int cell_index;
    uint64_t cell_id;
    uint64_t generation;
    size_t view_row;
    size_t column_begin;
    size_t column_end;
    char path[4096];
    char launch_directory[4096];
    size_t line;
    size_t column;
    gsh_resource_provenance provenance;
    gsh_resource_type type;
    bool stderr_stream;
    bool navigable_root;
} gsh_async_resource_action;

typedef struct {
    bool occupied;
    int cell_index;
    uint64_t cell_id;
    size_t output_row;
    size_t byte_begin;
    size_t byte_end;
    size_t column_begin;
    size_t column_end;
    size_t label_length;
    char label[512];
    char path[4096];
    gsh_resource_type type;
    bool navigable_root;
} gsh_async_native_resource;

typedef struct {
    bool enabled;
    bool alternate_screen_entered;
    bool render_pending;
    bool actions_enabled;
    bool mouse_enabled;
    gsh_path_detection_mode path_detection;
    uint64_t next_id;
    size_t terminal_rows;
    size_t terminal_columns;
    size_t view_columns;
    gsh_async_cell cells[GSH_ASYNC_CELL_CAP];
    char view[GSH_ASYNC_VIEW_ROWS][GSH_ASYNC_VIEW_BYTES];
    size_t view_lengths[GSH_ASYNC_VIEW_ROWS];
    size_t view_start;
    size_t view_count;
    size_t scroll_offset;
    size_t screen_row_by_view[GSH_ASYNC_VIEW_ROWS];
    gsh_async_resource_action resources[GSH_ASYNC_RESOURCE_CAP];
    size_t resource_count;
    gsh_async_native_resource native_resources[
        GSH_ASYNC_NATIVE_RESOURCE_CAP];
    char render[GSH_ASYNC_RENDER_CAP];
    size_t render_length;
} gsh_async_repl;

void gsh_async_repl_initialize(gsh_async_repl *repl, bool enabled);
void gsh_async_repl_configure_actions(gsh_async_repl *repl, bool enabled,
                                      gsh_path_detection_mode detection);
void gsh_async_repl_resize(gsh_async_repl *repl, size_t rows,
                           size_t columns);
void gsh_async_repl_scroll(gsh_async_repl *repl, long rows);
int gsh_async_repl_accept(gsh_async_repl *repl, const char *prompt,
                          const char *command, size_t command_length,
                          const char *launch_directory,
                          bool barrier, bool blocks_independent,
                          bool status_dependency, bool control);
int gsh_async_repl_next(gsh_async_repl *repl, bool state_lane_busy);
void gsh_async_repl_starting(gsh_async_repl *repl, int cell_index);
int gsh_async_repl_attach(gsh_async_repl *repl, int cell_index, pid_t pid,
                          pid_t pgid, int pty_fd, int resource_fd);
void gsh_async_repl_finish(gsh_async_repl *repl, int cell_index,
                           int wait_status, bool launched);
int gsh_async_repl_reap(gsh_async_repl *repl, pid_t pid, int wait_status);
int gsh_async_repl_cell_for_pid(const gsh_async_repl *repl, pid_t pid);
int gsh_async_repl_cell_for_fd(const gsh_async_repl *repl, int descriptor);
int gsh_async_repl_cell_for_resource_fd(const gsh_async_repl *repl,
                                        int descriptor);
void gsh_async_repl_close_resource(gsh_async_repl *repl, int cell_index);
int gsh_async_repl_set_preview_layout(gsh_async_repl *repl, int cell_index,
                                      size_t separator_column);
int gsh_async_repl_split_preview(const gsh_async_repl *repl,
                                 size_t *separator_column);
int gsh_async_repl_add_native_resource(
    gsh_async_repl *repl, int cell_index, size_t output_row,
    size_t byte_begin, size_t byte_end, size_t column_begin,
    size_t column_end, const char *label,
    size_t label_length, const char *path, size_t path_length,
    gsh_resource_type type, bool navigable_root);
int gsh_async_repl_append(gsh_async_repl *repl, int cell_index,
                          const char *bytes, size_t length);
int gsh_async_repl_queue_input(gsh_async_repl *repl, int cell_index,
                               const char *bytes, size_t length);
bool gsh_async_repl_input_pending(const gsh_async_repl *repl,
                                  int cell_index);
int gsh_async_repl_flush_input(gsh_async_repl *repl, int cell_index);
void gsh_async_repl_close_output(gsh_async_repl *repl, int cell_index);
size_t gsh_async_repl_job_count(const gsh_async_repl *repl);
bool gsh_async_repl_all_settled(const gsh_async_repl *repl);
bool gsh_async_repl_prompt_settled(const gsh_async_repl *repl);
bool gsh_async_repl_all_settled_except(const gsh_async_repl *repl,
                                       int ignored_cell);
int gsh_async_repl_focused_job(const gsh_async_repl *repl);
int gsh_async_repl_focus(gsh_async_repl *repl, int cell_index);
int gsh_async_repl_request_input(gsh_async_repl *repl, int cell_index,
                                 bool fullscreen);
int gsh_async_repl_autofocus(gsh_async_repl *repl);
int gsh_async_repl_filter_fullscreen(gsh_async_repl *repl, int cell_index,
                                     const char *bytes, size_t length,
                                     char *output, size_t output_capacity,
                                     size_t *output_length);
void gsh_async_repl_unfocus(gsh_async_repl *repl);
void gsh_async_repl_mark_stopped(gsh_async_repl *repl, int cell_index);
void gsh_async_repl_mark_running(gsh_async_repl *repl, int cell_index);
int gsh_async_repl_previous_status(const gsh_async_repl *repl,
                                   int cell_index, int *status);
int gsh_async_repl_prepare_render(gsh_async_repl *repl,
                                  const char *active_prompt,
                                  const char *editor, size_t editor_length,
                                  size_t editor_cursor);
const char *gsh_async_repl_render_data(const gsh_async_repl *repl);
size_t gsh_async_repl_render_length(const gsh_async_repl *repl);
void gsh_async_repl_rendered(gsh_async_repl *repl);
void gsh_async_repl_close(gsh_async_repl *repl);
int gsh_async_repl_resource_at(const gsh_async_repl *repl, size_t row,
                               size_t column,
                               gsh_async_resource_action *action);
int gsh_async_repl_suspended_fullscreen(const gsh_async_repl *repl);

#endif
