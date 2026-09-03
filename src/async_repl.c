#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "async_repl.h"

#include <errno.h>
#include <signal.h> /* CANON-INCLUDE: macos */
#include <stdio.h> /* CANON-INCLUDE: linux */
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>

/* ── Cells Separate Output From Editor Ownership ─────────────────
 * Direct terminal output made the next prompt depend on job completion.
 * A cell now owns the immutable command, lifecycle, PTY, and captured bytes.
 * The editor is separate state and can therefore become active at Enter.
 * Captured output adds rows only when bytes arrive, so silent commands do not
 * reserve an empty row between their command and the next active editor.
 * Fixed cell, job, output, viewport, and render capacities bound every turn.
 * Reuse is allowed only after both the process and its output source settle.
 * ─────────────────────────────────────────────────────────────── */

static bool cell_terminal(const gsh_async_cell *cell)
{
    if (cell == NULL || !cell->occupied) {
        return false;
    }
    return cell->state == GSH_ASYNC_DONE ||
           cell->state == GSH_ASYNC_FAILED ||
           cell->state == GSH_ASYNC_CANCELLED ||
           cell->state == GSH_ASYNC_REJECTED;
}

static bool cell_index_valid(int cell_index)
{
    return cell_index >= 0 && cell_index < GSH_ASYNC_CELL_CAP;
}

static int oldest_reusable_cell(const gsh_async_repl *repl, bool control)
{
    uint64_t oldest = UINT64_MAX;
    size_t available = 0;
    int reusable = -1;
    int unused = -1;
    int index;

    if (repl == NULL || !repl->enabled) {
        return -1;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        const gsh_async_cell *cell = &repl->cells[index];

        if (!cell->occupied) {
            available++;
            if (unused < 0) {
                unused = index;
            }
        }
        if (cell_terminal(cell) && cell->output_closed) {
            available++;
            if (cell->id < oldest) {
                oldest = cell->id;
                reusable = index;
            }
        }
    }
    if (available < (control ? 1U : 2U)) {
        return -1;
    }
    return unused >= 0 ? unused : reusable;
}

static void reset_cell(gsh_async_cell *cell)
{
    if (cell == NULL) {
        return;
    }
    int descriptor;
    int resource_descriptor;

    descriptor = cell->pty_fd;
    resource_descriptor = cell->resource_fd;
    (void)memset(cell, 0, sizeof(*cell));
    cell->pty_fd = -1;
    cell->resource_fd = -1;
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    if (resource_descriptor >= 0) (void)close(resource_descriptor);
}

void gsh_async_repl_initialize(gsh_async_repl *repl, bool enabled)
{
    if (repl == NULL) {
        return;
    }
    int index;

    (void)memset(repl, 0, sizeof(*repl));
    repl->enabled = enabled;
    repl->next_id = 1;
    repl->terminal_rows = 24;
    repl->terminal_columns = 80;
    repl->view_columns = 80;
    repl->render_pending = enabled;
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        repl->cells[index].pty_fd = -1;
        repl->cells[index].resource_fd = -1;
    }
}

void gsh_async_repl_configure_actions(gsh_async_repl *repl, bool enabled,
                                      gsh_path_detection_mode detection)
{
    if (repl == NULL) return;
    repl->actions_enabled = enabled;
    repl->path_detection = detection;
    repl->render_pending = repl->enabled;
}

void gsh_async_repl_resize(gsh_async_repl *repl, size_t rows,
                           size_t columns)
{
    if (repl == NULL) {
        return;
    }
    repl->terminal_rows = rows == 0 ? 1 : rows;
    repl->terminal_columns = columns == 0 ? 1 : columns;
    if (repl->terminal_rows > GSH_ASYNC_TERMINAL_ROW_CAP) {
        repl->terminal_rows = GSH_ASYNC_TERMINAL_ROW_CAP;
    }
    if (repl->terminal_columns > GSH_ASYNC_VIEW_COLUMNS) {
        repl->terminal_columns = GSH_ASYNC_VIEW_COLUMNS;
    }
    repl->render_pending = repl->enabled;
}

void gsh_async_repl_scroll(gsh_async_repl *repl, long rows)
{
    size_t visible;
    size_t maximum;
    if (repl == NULL || rows == 0) return;
    visible = repl->terminal_rows < repl->view_count
                  ? repl->terminal_rows : repl->view_count;
    maximum = repl->view_count - visible;
    if (repl->scroll_offset > maximum) repl->scroll_offset = maximum;
    if (rows > 0) {
        size_t amount = (size_t)rows;
        repl->scroll_offset = amount > maximum - repl->scroll_offset
                                  ? maximum
                                  : repl->scroll_offset + amount;
    } else {
        size_t amount = (size_t)(-(rows + 1L)) + 1U;
        repl->scroll_offset = amount > repl->scroll_offset
                                  ? 0U : repl->scroll_offset - amount;
    }
    repl->render_pending = repl->enabled;
}

static int copy_cell_text(gsh_async_cell *cell, const char *prompt,
                          const char *command, size_t command_length,
                          const char *launch_directory)
{
    size_t prompt_length;

    if (cell == NULL || prompt == NULL || command == NULL ||
        launch_directory == NULL ||
        command_length >= sizeof(cell->command)) {
        errno = EINVAL;
        return -1;
    }
    prompt_length = strlen(prompt);
    if (prompt_length >= sizeof(cell->prompt)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)memcpy(cell->prompt, prompt, prompt_length + 1U);
    (void)memcpy(cell->command, command, command_length);
    cell->command[command_length] = '\0';
    cell->prompt_length = prompt_length;
    cell->command_length = command_length;
    if (strlen(launch_directory) >= sizeof(cell->launch_directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)memcpy(cell->launch_directory, launch_directory,
                 strlen(launch_directory) + 1U);
    return 0;
}

int gsh_async_repl_accept(gsh_async_repl *repl, const char *prompt,
                          const char *command, size_t command_length,
                          const char *launch_directory,
                          bool barrier, bool blocks_independent,
                          bool status_dependency, bool control)
{
    if (repl == NULL) {
        return -1;
    }
    int cell_index = oldest_reusable_cell(repl, control);
    gsh_async_cell *cell;

    if (cell_index < 0 || repl == NULL || repl->next_id == UINT64_MAX) {
        errno = ENOSPC;
        return -1;
    }
    cell = &repl->cells[cell_index];
    reset_cell(cell);
    if (copy_cell_text(cell, prompt, command, command_length,
                       launch_directory) == -1) {
        return -1;
    }
    cell->occupied = true;
    cell->barrier = barrier;
    cell->blocks_independent = blocks_independent;
    cell->status_dependency = status_dependency;
    cell->control = control;
    cell->id = repl->next_id++;
    cell->output_generation = 1U;
    cell->state = GSH_ASYNC_QUEUED;
    repl->render_pending = true;
    return cell_index;
}

static int previous_cell(const gsh_async_repl *repl, uint64_t id)
{
    uint64_t previous = 0;
    int selected = -1;
    int index;

    if (repl == NULL || id <= 1U) {
        return -1;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        const gsh_async_cell *cell = &repl->cells[index];

        if (cell->occupied && cell->id < id && cell->id > previous) {
            previous = cell->id;
            selected = index;
        }
    }
    return selected;
}

static bool cell_dependency_ready(const gsh_async_repl *repl, int index)
{
    if (repl == NULL) {
        return false;
    }
    const gsh_async_cell *cell = &repl->cells[index];
    int previous;

    if (!cell->status_dependency) {
        return true;
    }
    previous = previous_cell(repl, cell->id);
    return previous < 0 || cell_terminal(&repl->cells[previous]);
}

static bool earlier_blocker_unsettled(const gsh_async_repl *repl,
                                      uint64_t id)
{
    if (repl == NULL) {
        return false;
    }
    int index;

    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        const gsh_async_cell *cell = &repl->cells[index];

        if (cell->occupied && cell->id < id &&
            cell->blocks_independent &&
            !cell_terminal(cell)) {
            return true;
        }
    }
    return false;
}

static bool earlier_cell_unsettled(const gsh_async_repl *repl, uint64_t id)
{
    if (repl == NULL) {
        return false;
    }
    int index;

    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        const gsh_async_cell *cell = &repl->cells[index];

        if (cell->occupied && cell->id < id && !cell_terminal(cell)) {
            return true;
        }
    }
    return false;
}

size_t gsh_async_repl_job_count(const gsh_async_repl *repl)
{
    size_t count = 0;
    int index;

    if (repl == NULL) {
        return 0;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        if (repl->cells[index].occupied && repl->cells[index].pid > 0 &&
            !cell_terminal(&repl->cells[index])) {
            count++;
        }
    }
    return count;
}

bool gsh_async_repl_all_settled_except(const gsh_async_repl *repl,
                                       int ignored_cell)
{
    int index;

    if (repl == NULL) {
        return true;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        const gsh_async_cell *cell = &repl->cells[index];

        if (index != ignored_cell && cell->occupied &&
            (!cell_terminal(cell) || !cell->output_closed ||
             cell->pty_fd >= 0 || cell->input_offset < cell->input_length ||
             cell->input_requested || cell->input_probe_pending)) {
            return false;
        }
    }
    return true;
}

bool gsh_async_repl_all_settled(const gsh_async_repl *repl)
{
    if (repl == NULL) {
        return false;
    }
    return gsh_async_repl_all_settled_except(repl, -1);
}

bool gsh_async_repl_prompt_settled(const gsh_async_repl *repl)
{
    int index;
    if (repl == NULL) return false;
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        const gsh_async_cell *cell = &repl->cells[index];
        bool suspended_preview = cell->native_preview && cell->fullscreen &&
                                 !cell->focused &&
                                 cell->state == GSH_ASYNC_RUNNING;
        if (cell->occupied && !suspended_preview &&
            (!cell_terminal(cell) || !cell->output_closed ||
             cell->pty_fd >= 0 || cell->input_offset < cell->input_length ||
             cell->input_requested || cell->input_probe_pending)) {
            return false;
        }
    }
    return true;
}

int gsh_async_repl_next(gsh_async_repl *repl, bool state_lane_busy)
{
    uint64_t selected_id = UINT64_MAX;
    int selected = -1;
    int index;

    if (repl == NULL || !repl->enabled) {
        return -1;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        gsh_async_cell *cell = &repl->cells[index];

        if (!cell->occupied || cell->state != GSH_ASYNC_QUEUED ||
            cell->id >= selected_id ||
            (!cell->control &&
             (gsh_async_repl_job_count(repl) >= GSH_ASYNC_JOB_CAP ||
              !cell_dependency_ready(repl, index) ||
              earlier_blocker_unsettled(repl, cell->id) ||
              (cell->barrier &&
               (state_lane_busy ||
                earlier_cell_unsettled(repl, cell->id)))))) {
            continue;
        }
        selected = index;
        selected_id = cell->id;
    }
    return selected;
}

void gsh_async_repl_starting(gsh_async_repl *repl, int cell_index)
{
    if (repl == NULL || !cell_index_valid(cell_index) ||
        !repl->cells[cell_index].occupied) {
        return;
    }
    repl->cells[cell_index].state = GSH_ASYNC_STARTING;
    repl->render_pending = true;
}

int gsh_async_repl_attach(gsh_async_repl *repl, int cell_index, pid_t pid,
                          pid_t pgid, int pty_fd, int resource_fd)
{
    gsh_async_cell *cell;

    if (repl == NULL || !cell_index_valid(cell_index) || pid <= 0 ||
        pgid <= 0 || pty_fd < 0 || resource_fd < -1) {
        errno = EINVAL;
        return -1;
    }
    cell = &repl->cells[cell_index];
    if (!cell->occupied || cell->state != GSH_ASYNC_STARTING) {
        errno = EPROTO;
        return -1;
    }
    cell->pid = pid;
    cell->pgid = pgid;
    cell->pty_fd = pty_fd;
    cell->resource_fd = resource_fd;
    cell->state = GSH_ASYNC_RUNNING;
    repl->render_pending = true;
    return 0;
}

void gsh_async_repl_finish(gsh_async_repl *repl, int cell_index,
                           int wait_status, bool launched)
{
    gsh_async_cell *cell;

    if (repl == NULL || !cell_index_valid(cell_index)) {
        return;
    }
    cell = &repl->cells[cell_index];
    if (!cell->occupied || cell_terminal(cell)) {
        return;
    }
    cell->wait_status = wait_status;
    cell->state = launched && WIFEXITED(wait_status) &&
                          WEXITSTATUS(wait_status) == 0
                      ? GSH_ASYNC_DONE
                      : GSH_ASYNC_FAILED;
    cell->output_closed = cell->pty_fd < 0;
    repl->render_pending = true;
}

int gsh_async_repl_cell_for_pid(const gsh_async_repl *repl, pid_t pid)
{
    int index;

    if (repl == NULL || pid <= 0) {
        return -1;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        if (repl->cells[index].occupied && repl->cells[index].pid == pid) {
            return index;
        }
    }
    return -1;
}

int gsh_async_repl_cell_for_fd(const gsh_async_repl *repl, int descriptor)
{
    int index;

    if (repl == NULL || descriptor < 0) {
        return -1;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        if (repl->cells[index].occupied &&
            repl->cells[index].pty_fd == descriptor) {
            return index;
        }
    }
    return -1;
}

int gsh_async_repl_cell_for_resource_fd(const gsh_async_repl *repl,
                                        int descriptor)
{
    int index;
    if (repl == NULL || descriptor < 0) return -1;
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        if (repl->cells[index].occupied &&
            repl->cells[index].resource_fd == descriptor) return index;
    }
    return -1;
}

void gsh_async_repl_close_resource(gsh_async_repl *repl, int cell_index)
{
    if (repl == NULL || !cell_index_valid(cell_index)) return;
    if (repl->cells[cell_index].resource_fd >= 0)
        (void)close(repl->cells[cell_index].resource_fd);
    repl->cells[cell_index].resource_fd = -1;
}

int gsh_async_repl_set_preview_layout(gsh_async_repl *repl, int cell_index,
                                      size_t separator_column)
{
    gsh_async_cell *cell;
    bool split;
    if (repl == NULL || !cell_index_valid(cell_index)) return -1;
    cell = &repl->cells[cell_index];
    split = separator_column > 1U &&
            separator_column < repl->terminal_columns;
    if (!cell->occupied || cell_terminal(cell) ||
        (separator_column != 0U && !split)) return -1;
    if (!cell->native_preview || cell->preview_split != split ||
        cell->preview_separator_column != separator_column) {
        cell->fullscreen_presented = false;
        repl->render_pending = true;
    }
    cell->native_preview = true;
    cell->preview_split = split;
    cell->preview_separator_column = split ? separator_column : 0U;
    return 0;
}

int gsh_async_repl_split_preview(const gsh_async_repl *repl,
                                 size_t *separator_column)
{
    uint64_t newest = 0U;
    int selected = -1;
    int index;
    if (repl == NULL || separator_column == NULL) return -1;
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        const gsh_async_cell *cell = &repl->cells[index];
        if (cell->occupied && cell->native_preview && cell->preview_split &&
            cell->fullscreen && !cell_terminal(cell) &&
            cell->preview_separator_column < repl->terminal_columns &&
            cell->id > newest) {
            newest = cell->id;
            selected = index;
        }
    }
    if (selected < 0) return -1;
    *separator_column = repl->cells[selected].preview_separator_column;
    return selected;
}

int gsh_async_repl_add_native_resource(
    gsh_async_repl *repl, int cell_index, size_t output_row,
    size_t byte_begin, size_t byte_end, size_t column_begin,
    size_t column_end, const char *label,
    size_t label_length, const char *path, size_t path_length,
    gsh_resource_type type, bool navigable_root)
{
    size_t index;
    gsh_async_native_resource *resource = NULL;
    const gsh_async_cell *cell;
    if (repl == NULL || !cell_index_valid(cell_index) || label == NULL ||
        path == NULL || label_length == 0U ||
        label_length >= sizeof(repl->native_resources[0].label) ||
        path_length == 0U ||
        path_length >= sizeof(repl->native_resources[0].path) ||
        byte_begin >= byte_end || byte_end - byte_begin != label_length ||
        column_begin >= column_end ||
        type < GSH_RESOURCE_REGULAR || type > GSH_RESOURCE_SYMLINK) return -1;
    cell = &repl->cells[cell_index];
    if (!cell->occupied) return -1;
    for (index = 0U; index < GSH_ASYNC_NATIVE_RESOURCE_CAP; index++) {
        gsh_async_native_resource *candidate = &repl->native_resources[index];
        if (!candidate->occupied || !cell_index_valid(candidate->cell_index) ||
            !repl->cells[candidate->cell_index].occupied ||
            repl->cells[candidate->cell_index].id != candidate->cell_id) {
            resource = candidate;
            break;
        }
    }
    if (resource == NULL) { errno = ENOBUFS; return -1; }
    (void)memset(resource, 0, sizeof(*resource));
    resource->occupied = true;
    resource->cell_index = cell_index;
    resource->cell_id = cell->id;
    resource->output_row = output_row;
    resource->byte_begin = byte_begin;
    resource->byte_end = byte_end;
    resource->column_begin = column_begin;
    resource->column_end = column_end;
    resource->label_length = label_length;
    (void)memcpy(resource->label, label, label_length);
    resource->label[label_length] = '\0';
    (void)memcpy(resource->path, path, path_length);
    resource->path[path_length] = '\0';
    resource->type = type;
    resource->navigable_root = navigable_root;
    repl->render_pending = true;
    return 0;
}

int gsh_async_repl_reap(gsh_async_repl *repl, pid_t pid, int wait_status)
{
    if (repl == NULL) {
        return -1;
    }
    int cell_index = gsh_async_repl_cell_for_pid(repl, pid);
    gsh_async_cell *cell;

    if (cell_index < 0) {
        return -1;
    }
    cell = &repl->cells[cell_index];
    cell->wait_status = wait_status;
    cell->state = WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0
                      ? GSH_ASYNC_DONE
                      : GSH_ASYNC_FAILED;
    cell->focused = false;
    cell->input_requested = false;
    cell->input_probe_pending = false;
    cell->fullscreen_presented = false;
    cell->native_preview = false;
    cell->preview_split = false;
    cell->preview_separator_column = 0U;
    cell->passthrough_state = 0;
    cell->passthrough_utf8_length = 0;
    cell->passthrough_utf8_expected = 0;
    cell->passthrough_sequence_length = 0;
    (void)memset(cell->input, 0, sizeof(cell->input));
    cell->input_offset = 0;
    cell->input_length = 0;
    cell->pid = 0;
    cell->pgid = 0;
    cell->output_closed = cell->pty_fd < 0;
    repl->render_pending = true;
    return cell_index;
}

/* ── Carriage Returns Rewrite One Logical Row ──────────────────
 * Progress producers such as Git use carriage return to revisit one row.
 * Treating that byte as a newline retained every percentage as scrollback.
 * Each cell now keeps a bounded cursor inside its current logical output row.
 * Carriage return resets that cursor, visible bytes overwrite in place, and
 * only newline commits another row, matching the terminal behavior we retain.
 * Invalid internal offsets recover to the bounded buffer end before use.
 * ─────────────────────────────────────────────────────────────── */
static void recover_output_cursor(gsh_async_cell *cell)
{
    if (cell == NULL) return;
    if (cell->output_length >= sizeof(cell->output)) {
        cell->output_length = sizeof(cell->output) - 1U;
        cell->output[cell->output_length] = '\0';
        cell->output_truncated = true;
    }
    if (cell->output_line_start > cell->output_length ||
        cell->output_cursor < cell->output_line_start ||
        cell->output_cursor > cell->output_length) {
        cell->output_line_start = cell->output_length;
        cell->output_cursor = cell->output_length;
    }
}

static void write_visible_byte(gsh_async_cell *cell, unsigned char byte)
{
    if (cell == NULL) {
        return;
    }
    recover_output_cursor(cell);
    if (cell->output_cursor < cell->output_length) {
        cell->output[cell->output_cursor++] = (char)byte;
        return;
    }
    if (cell->output_length >= sizeof(cell->output) - 1U) {
        cell->output_truncated = true;
        return;
    }
    cell->output[cell->output_length++] = (char)byte;
    cell->output_cursor = cell->output_length;
    cell->output[cell->output_length] = '\0';
}

static void append_visible_newline(gsh_async_cell *cell)
{
    if (cell == NULL) {
        return;
    }
    recover_output_cursor(cell);
    if (cell->output_length >= sizeof(cell->output) - 1U) {
        cell->output_truncated = true;
        cell->output_cursor = cell->output_length;
        return;
    }
    cell->output[cell->output_length++] = '\n';
    cell->output[cell->output_length] = '\0';
    cell->output_line_start = cell->output_length;
    cell->output_cursor = cell->output_length;
}

static void append_terminal_byte(gsh_async_cell *cell, unsigned char byte)
{
    if (cell == NULL) return;
    if (cell->escape_state == 1U) {
        if (byte == '[') cell->escape_state = 2U;
        else if (byte == ']') cell->escape_state = 3U;
        else if (byte == 'P' || byte == 'X' || byte == '^' || byte == '_')
            cell->escape_state = 5U;
        else cell->escape_state = 0U;
        return;
    }
    if (cell->escape_state == 2U) {
        if (byte >= 0x40U && byte <= 0x7eU) {
            cell->escape_state = 0;
        }
        return;
    }
    if (cell->escape_state == 3U || cell->escape_state == 4U) {
        if (byte == 0x07U || byte == 0x9cU ||
            (cell->escape_state == 4U && byte == '\\')) {
            cell->escape_state = 0;
        } else {
            cell->escape_state = byte == 0x1bU ? 4U : 3U;
        }
        return;
    }
    if (cell->escape_state == 5U || cell->escape_state == 6U) {
        if (byte == 0x9cU ||
            (cell->escape_state == 6U && byte == '\\')) {
            cell->escape_state = 0U;
        } else {
            cell->escape_state = byte == 0x1bU ? 6U : 5U;
        }
        return;
    }
    if (byte == 0x1bU) {
        cell->escape_state = 1U;
    } else if (byte == 0x9bU) {
        cell->escape_state = 2U;
    } else if (byte == 0x9dU) {
        cell->escape_state = 3U;
    } else if (byte == 0x90U || byte == 0x98U || byte == 0x9eU ||
               byte == 0x9fU) {
        cell->escape_state = 5U;
    } else if (byte == '\r') {
        recover_output_cursor(cell);
        cell->output_cursor = cell->output_line_start;
    } else if (byte == '\n') {
        append_visible_newline(cell);
    } else if (byte == '\t') {
        write_visible_byte(cell, ' ');
    } else if (byte >= 0x20U) {
        write_visible_byte(cell, byte);
    }
}

int gsh_async_repl_append(gsh_async_repl *repl, int cell_index,
                          const char *bytes, size_t length)
{
    size_t offset;
    gsh_async_cell *cell;

    if (repl == NULL || !cell_index_valid(cell_index) || bytes == NULL ||
        length > GSH_ASYNC_CELL_OUTPUT_CAP) {
        errno = EINVAL;
        return -1;
    }
    cell = &repl->cells[cell_index];
    if (!cell->occupied) {
        errno = ENOENT;
        return -1;
    }
    for (offset = 0; offset < length; offset++) {
        append_terminal_byte(cell, (unsigned char)bytes[offset]);
    }
    if (cell->output_generation != UINT64_MAX) cell->output_generation++;
    repl->render_pending = true;
    return cell->output_truncated ? 1 : 0;
}

int gsh_async_repl_queue_input(gsh_async_repl *repl, int cell_index,
                               const char *bytes, size_t length)
{
    gsh_async_cell *cell;

    if (repl == NULL || !cell_index_valid(cell_index) || bytes == NULL ||
        length > GSH_ASYNC_CELL_INPUT_CAP) {
        errno = EINVAL;
        return -1;
    }
    cell = &repl->cells[cell_index];
    if (!cell->occupied || cell->pty_fd < 0) {
        errno = EPIPE;
        return -1;
    }
    if (length > sizeof(cell->input) - cell->input_length &&
        cell->input_offset > 0) {
        size_t remaining = cell->input_length - cell->input_offset;

        (void)memmove(cell->input, cell->input + cell->input_offset,
                remaining);
        (void)memset(cell->input + remaining, 0,
               cell->input_length - remaining);
        cell->input_length = remaining;
        cell->input_offset = 0;
    }
    if (length > sizeof(cell->input) - cell->input_length) {
        errno = ENOSPC;
        return -1;
    }
    (void)memcpy(cell->input + cell->input_length, bytes, length);
    cell->input_length += length;
    return 0;
}

bool gsh_async_repl_input_pending(const gsh_async_repl *repl,
                                  int cell_index)
{
    if (repl == NULL) {
        return false;
    }
    return repl != NULL && cell_index_valid(cell_index) &&
           repl->cells[cell_index].occupied &&
           repl->cells[cell_index].input_offset <
               repl->cells[cell_index].input_length;
}

int gsh_async_repl_flush_input(gsh_async_repl *repl, int cell_index)
{
    gsh_async_cell *cell;
    ssize_t written;

    if (!gsh_async_repl_input_pending(repl, cell_index)) {
        return 0;
    }
    cell = &repl->cells[cell_index];
    written = write(cell->pty_fd, cell->input + cell->input_offset,
                    cell->input_length - cell->input_offset);
    if (written > 0) {
        (void)memset(cell->input + cell->input_offset, 0, (size_t)written);
        cell->input_offset += (size_t)written;
        if (cell->input_offset == cell->input_length) {
            cell->input_offset = 0;
            cell->input_length = 0;
            return 0;
        }
        return 1;
    }
    if (written == -1 &&
        (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
        return 1;
    }
    return -1;
}

void gsh_async_repl_close_output(gsh_async_repl *repl, int cell_index)
{
    gsh_async_cell *cell;

    if (repl == NULL || !cell_index_valid(cell_index)) {
        return;
    }
    cell = &repl->cells[cell_index];
    if (cell->pty_fd >= 0) {
        (void)close(cell->pty_fd);
        cell->pty_fd = -1;
    }
    cell->output_closed = true;
    cell->focused = false;
    cell->input_requested = false;
    cell->input_probe_pending = false;
    cell->fullscreen_presented = false;
    cell->passthrough_state = 0;
    cell->passthrough_utf8_length = 0;
    cell->passthrough_utf8_expected = 0;
    cell->passthrough_sequence_length = 0;
    (void)memset(cell->input, 0, sizeof(cell->input));
    cell->input_offset = 0;
    cell->input_length = 0;
    repl->render_pending = true;
}

int gsh_async_repl_focused_job(const gsh_async_repl *repl)
{
    if (repl == NULL) return -1;
    int index;

    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        if (repl->cells[index].occupied && repl->cells[index].pgid > 0 &&
            repl->cells[index].state == GSH_ASYNC_RUNNING &&
            repl->cells[index].focused) {
            return index;
        }
    }
    return -1;
}

int gsh_async_repl_focus(gsh_async_repl *repl, int cell_index)
{
    if (repl == NULL) {
        return -1;
    }
    int focused = gsh_async_repl_focused_job(repl);

    if (repl == NULL || !cell_index_valid(cell_index) ||
        repl->cells[cell_index].pgid <= 0 ||
        cell_terminal(&repl->cells[cell_index])) {
        errno = EINVAL;
        return -1;
    }
    if (focused >= 0) {
        repl->cells[focused].focused = false;
        repl->cells[focused].input_requested = false;
        repl->cells[focused].autofocus_suppressed = true;
        repl->cells[focused].fullscreen_presented = false;
    }
    repl->cells[cell_index].focused = true;
    repl->cells[cell_index].input_requested = true;
    repl->cells[cell_index].autofocus_suppressed = false;
    repl->cells[cell_index].fullscreen_presented = false;
    repl->cells[cell_index].passthrough_state = 0;
    repl->cells[cell_index].passthrough_utf8_length = 0;
    repl->cells[cell_index].passthrough_utf8_expected = 0;
    repl->cells[cell_index].passthrough_sequence_length = 0;
    repl->render_pending = true;
    return 0;
}

int gsh_async_repl_autofocus(gsh_async_repl *repl)
{
    uint64_t oldest = UINT64_MAX;
    int selected = -1;
    int index;

    if (repl == NULL) {
        return -1;
    }
    index = gsh_async_repl_focused_job(repl);
    if (index >= 0) {
        return index;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        const gsh_async_cell *cell = &repl->cells[index];

        if (cell->occupied && cell->input_requested &&
            cell->state == GSH_ASYNC_RUNNING && cell->pgid > 0 &&
            cell->id < oldest) {
            oldest = cell->id;
            selected = index;
        }
    }
    if (selected >= 0 && gsh_async_repl_focus(repl, selected) == -1) {
        return -1;
    }
    return selected;
}

int gsh_async_repl_request_input(gsh_async_repl *repl, int cell_index,
                                 bool fullscreen)
{
    static const char marker[] = "[full-screen session]\n";
    gsh_async_cell *cell;

    if (repl == NULL || !cell_index_valid(cell_index)) {
        errno = EINVAL;
        return -1;
    }
    cell = &repl->cells[cell_index];
    if (!cell->occupied || cell->state != GSH_ASYNC_RUNNING ||
        cell->pgid <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (!cell->autofocus_suppressed) {
        cell->input_requested = true;
    }
    if (fullscreen) {
        cell->fullscreen = true;
        if (!cell->fullscreen_recorded) {
            size_t offset;

            cell->output_length = 0;
            cell->output_line_start = 0;
            cell->output_cursor = 0;
            cell->output[0] = '\0';
            cell->output_truncated = false;
            cell->escape_state = 0;
            for (offset = 0; offset < sizeof(marker) - 1U; offset++) {
                append_terminal_byte(cell, (unsigned char)marker[offset]);
            }
            cell->fullscreen_recorded = true;
        }
    }
    repl->render_pending = true;
    return 0;
}

static bool fullscreen_csi_blocked(const char *sequence, size_t length)
{
    size_t offset;
    unsigned int parameter = 0;
    bool private_mode;
    unsigned char final;

    if (sequence == NULL || length < 3U || sequence[0] != '\033' ||
        sequence[1] != '[') {
        return true;
    }
    final = (unsigned char)sequence[length - 1U];
    if (final == 't') {
        return true;
    }
    private_mode = length > 3U && sequence[2] == '?';
    if (!private_mode || (final != 'h' && final != 'l')) {
        return false;
    }
    for (offset = 3U;
         offset + 1U < length &&
         offset < GSH_ASYNC_PASSTHROUGH_SEQUENCE_CAP;
         offset++) {
        unsigned char byte = (unsigned char)sequence[offset];

        if (byte >= '0' && byte <= '9') {
            parameter = parameter * 10U + (unsigned int)(byte - '0');
        } else if (byte == ';') {
            if (parameter == 3U || parameter == 40U || parameter == 47U ||
                parameter == 1047U || parameter == 1049U) {
                return true;
            }
            parameter = 0;
        } else {
            return true;
        }
    }
    return parameter == 3U || parameter == 40U || parameter == 47U ||
           parameter == 1047U || parameter == 1049U;
}

static int fullscreen_emit(char *output, size_t output_capacity,
                           size_t *used, const char *bytes, size_t length)
{
    if (used == NULL) return -1;
    if (bytes == NULL || output == NULL) {
        return -1;
    }
    if (*used > output_capacity || length > output_capacity - *used) {
        errno = ENOBUFS;
        return -1;
    }
    (void)memcpy(output + *used, bytes, length);
    *used += length;
    return 0;
}

static unsigned char fullscreen_utf8_expected(unsigned char byte)
{
    if (byte >= 0xc2U && byte <= 0xdfU) {
        return 2U;
    }
    if (byte >= 0xe0U && byte <= 0xefU) {
        return 3U;
    }
    if (byte >= 0xf0U && byte <= 0xf4U) {
        return 4U;
    }
    return 0U;
}

static bool fullscreen_utf8_continuation(const gsh_async_cell *cell,
                                         unsigned char byte)
{
    unsigned char lead;

    if (cell == NULL || cell->passthrough_utf8_length == 0U ||
        (byte & 0xc0U) != 0x80U) {
        return false;
    }
    if (cell->passthrough_utf8_length != 1U) {
        return true;
    }
    lead = (unsigned char)cell->passthrough_utf8[0];
    return !((lead == 0xe0U && byte < 0xa0U) ||
             (lead == 0xedU && byte > 0x9fU) ||
             (lead == 0xf0U && byte < 0x90U) ||
             (lead == 0xf4U && byte > 0x8fU));
}

static int fullscreen_consume_utf8(gsh_async_cell *cell,
                                   unsigned char byte, char *output,
                                   size_t output_capacity, size_t *used,
                                   bool relay)
{
    unsigned char expected;

    if (cell == NULL || output == NULL || used == NULL ||
        (cell->passthrough_utf8_length == 0U) !=
            (cell->passthrough_utf8_expected == 0U) ||
        cell->passthrough_utf8_expected > sizeof(cell->passthrough_utf8) ||
        (cell->passthrough_utf8_length != 0U &&
         cell->passthrough_utf8_length >=
             cell->passthrough_utf8_expected)) {
        errno = EINVAL;
        return -1;
    }
    if (cell->passthrough_utf8_length != 0U) {
        if (fullscreen_utf8_continuation(cell, byte)) {
            cell->passthrough_utf8[cell->passthrough_utf8_length++] =
                (char)byte;
            if (cell->passthrough_utf8_length ==
                cell->passthrough_utf8_expected) {
                bool c1 = (unsigned char)cell->passthrough_utf8[0] == 0xc2U &&
                          (unsigned char)cell->passthrough_utf8[1] <= 0x9fU;
                int result = c1 || !relay ? 0 : fullscreen_emit(
                    output, output_capacity, used, cell->passthrough_utf8,
                    cell->passthrough_utf8_length);

                cell->passthrough_utf8_length = 0;
                cell->passthrough_utf8_expected = 0;
                return result == -1 ? -1 : 1;
            }
            return 1;
        }
        cell->passthrough_utf8_length = 0;
        cell->passthrough_utf8_expected = 0;
    }
    expected = fullscreen_utf8_expected(byte);
    if (expected == 0U) {
        return 0;
    }
    cell->passthrough_utf8[0] = (char)byte;
    cell->passthrough_utf8_length = 1U;
    cell->passthrough_utf8_expected = expected;
    return 1;
}

static bool fullscreen_discard_control(gsh_async_cell *cell,
                                       unsigned char byte)
{
    if (cell == NULL) return false;
    if (cell->passthrough_state == 3U ||
        cell->passthrough_state == 4U) {
        if (byte == 0x07U || byte == 0x9cU ||
            (cell->passthrough_state == 4U && byte == '\\')) {
            cell->passthrough_state = 0U;
        } else {
            cell->passthrough_state = byte == 0x1bU ? 4U : 3U;
        }
        return true;
    }
    if (cell->passthrough_state == 5U) {
        if (byte >= 0x40U && byte <= 0x7eU) {
            cell->passthrough_state = 0U;
        }
        return true;
    }
    return false;
}

static int fullscreen_consume_escape(gsh_async_cell *cell,
                                     unsigned char byte, char *output,
                                     size_t output_capacity, size_t *used)
{
    if (cell == NULL) return -1;
    if (cell->passthrough_sequence_length >=
        sizeof(cell->passthrough_sequence)) {
        cell->passthrough_sequence_length = 0;
        cell->passthrough_state = 5U;
        return 0;
    }
    cell->passthrough_sequence[cell->passthrough_sequence_length++] =
        (char)byte;
    if (cell->passthrough_state == 1U) {
        if (byte == '[') {
            cell->passthrough_state = 2U;
        } else if (byte == ']' || byte == 'P' || byte == 'X' ||
                   byte == '^' || byte == '_') {
            cell->passthrough_sequence_length = 0;
            cell->passthrough_state = 3U;
        } else if (byte == 'c') {
            cell->passthrough_sequence_length = 0;
            cell->passthrough_state = 0U;
        } else {
            if (fullscreen_emit(output, output_capacity, used,
                                cell->passthrough_sequence,
                                cell->passthrough_sequence_length) == -1) {
                return -1;
            }
            cell->passthrough_sequence_length = 0;
            cell->passthrough_state = 0U;
        }
    } else if (byte >= 0x40U && byte <= 0x7eU) {
        if (!fullscreen_csi_blocked(cell->passthrough_sequence,
                                    cell->passthrough_sequence_length) &&
            fullscreen_emit(output, output_capacity, used,
                            cell->passthrough_sequence,
                            cell->passthrough_sequence_length) == -1) {
            return -1;
        }
        cell->passthrough_sequence_length = 0;
        cell->passthrough_state = 0U;
    }
    return 0;
}

/* ── Raw Focus Preserves TUIs Without Giving Away the Terminal ────
 * Cell rendering originally erased the cursor protocol emitted by curses apps.
 * Direct terminal handoff would restore the TUI but break concurrent ownership.
 * Raw focus instead relays one bounded PTY batch while the reactor keeps input.
 * UTF-8 scalars remain atomic across reads, so continuation bytes are not C1.
 * OSC, DCS, window operations, and alternate-screen changes are discarded.
 * Exit restoration then makes the managed compositor authoritative again.
 * ─────────────────────────────────────────────── */
int gsh_async_repl_filter_fullscreen(gsh_async_repl *repl, int cell_index,
                                     const char *bytes, size_t length,
                                     char *output, size_t output_capacity,
                                     size_t *output_length)
{
    gsh_async_cell *cell;
    size_t used = 0;
    size_t offset;

    if (repl == NULL || !cell_index_valid(cell_index) || bytes == NULL ||
        output == NULL || output_length == NULL ||
        length > GSH_ASYNC_CELL_INPUT_CAP ||
        length > output_capacity ||
        output_capacity - length < GSH_ASYNC_PASSTHROUGH_SEQUENCE_CAP) {
        errno = EINVAL;
        return -1;
    }
    cell = &repl->cells[cell_index];
    if (!cell->occupied || !cell->fullscreen) {
        errno = EINVAL;
        return -1;
    }
    for (offset = 0;
         offset < length && offset < GSH_ASYNC_CELL_INPUT_CAP; offset++) {
        unsigned char byte = (unsigned char)bytes[offset];

        if (cell->passthrough_state == 0U) {
            int utf8 = fullscreen_consume_utf8(
                cell, byte, output, output_capacity, &used, true);

            if (utf8 == -1) {
                return -1;
            }
            if (utf8 == 1) {
                continue;
            }
            if (byte == 0x1bU) {
                cell->passthrough_sequence[0] = (char)byte;
                cell->passthrough_sequence_length = 1U;
                cell->passthrough_state = 1U;
            } else if (byte == 0x9bU) {
                cell->passthrough_sequence[0] = '\033';
                cell->passthrough_sequence[1] = '[';
                cell->passthrough_sequence_length = 2U;
                cell->passthrough_state = 2U;
            } else if (byte >= 0x80U && byte <= 0x9fU) {
                if (byte == 0x90U || byte == 0x98U || byte == 0x9dU ||
                    byte == 0x9eU || byte == 0x9fU) {
                    cell->passthrough_state = 3U;
                }
            } else if (fullscreen_emit(output, output_capacity, &used,
                                       bytes + offset, 1U) == -1) {
                return -1;
            }
            continue;
        }
        if (cell->passthrough_state == 3U ||
            cell->passthrough_state == 4U) {
            int utf8 = fullscreen_consume_utf8(
                cell, byte, output, output_capacity, &used, false);

            if (utf8 == -1) {
                return -1;
            }
            if (utf8 == 1) {
                continue;
            }
        }
        if (fullscreen_discard_control(cell, byte)) {
            continue;
        }
        if (fullscreen_consume_escape(cell, byte, output, output_capacity,
                                      &used) == -1) {
            return -1;
        }
    }
    *output_length = used;
    return 0;
}

void gsh_async_repl_unfocus(gsh_async_repl *repl)
{
    if (repl == NULL) {
        return;
    }
    int focused = gsh_async_repl_focused_job(repl);

    if (repl == NULL || focused < 0) {
        return;
    }
    repl->cells[focused].input_requested = false;
    repl->cells[focused].focused = false;
    repl->cells[focused].autofocus_suppressed = true;
    repl->cells[focused].fullscreen_presented = false;
    repl->cells[focused].passthrough_state = 0;
    repl->cells[focused].passthrough_utf8_length = 0;
    repl->cells[focused].passthrough_utf8_expected = 0;
    repl->cells[focused].passthrough_sequence_length = 0;
    repl->render_pending = true;
}

void gsh_async_repl_mark_stopped(gsh_async_repl *repl, int cell_index)
{
    static const char marker[] = "[stopped]\n";
    bool was_stopped;

    if (repl == NULL || !cell_index_valid(cell_index)) {
        return;
    }
    was_stopped = repl->cells[cell_index].state == GSH_ASYNC_STOPPED;
    repl->cells[cell_index].state = GSH_ASYNC_STOPPED;
    repl->cells[cell_index].focused = false;
    repl->cells[cell_index].input_requested = false;
    repl->cells[cell_index].autofocus_suppressed = true;
    if (!was_stopped) {
        (void)gsh_async_repl_append(repl, cell_index, marker,
                                    sizeof(marker) - 1U);
    }
    repl->render_pending = true;
}

void gsh_async_repl_mark_running(gsh_async_repl *repl, int cell_index)
{
    static const char marker[] = "[continued]\n";
    bool was_stopped;

    if (repl == NULL || !cell_index_valid(cell_index)) {
        return;
    }
    was_stopped = repl->cells[cell_index].state == GSH_ASYNC_STOPPED;
    repl->cells[cell_index].state = GSH_ASYNC_RUNNING;
    if (was_stopped) {
        (void)gsh_async_repl_append(repl, cell_index, marker,
                                    sizeof(marker) - 1U);
    }
    repl->render_pending = true;
}

int gsh_async_repl_previous_status(const gsh_async_repl *repl,
                                   int cell_index, int *status)
{
    int previous;
    const gsh_async_cell *cell;

    if (repl == NULL || !cell_index_valid(cell_index) || status == NULL) {
        errno = EINVAL;
        return -1;
    }
    cell = &repl->cells[cell_index];
    previous = previous_cell(repl, cell->id);
    if (previous < 0 || !cell_terminal(&repl->cells[previous])) {
        errno = EAGAIN;
        return -1;
    }
    if (WIFEXITED(repl->cells[previous].wait_status)) {
        *status = WEXITSTATUS(repl->cells[previous].wait_status);
    } else if (WIFSIGNALED(repl->cells[previous].wait_status)) {
        *status = 128 + WTERMSIG(repl->cells[previous].wait_status);
    } else {
        *status = repl->cells[previous].wait_status & 255;
    }
    return 0;
}

static void clear_view(gsh_async_repl *repl)
{
    if (repl == NULL) {
        return;
    }
    repl->view_start = 0;
    repl->view_count = 0;
    (void)memset(repl->view_lengths, 0, sizeof(repl->view_lengths));
    (void)memset(repl->screen_row_by_view, 0,
                 sizeof(repl->screen_row_by_view));
    repl->resource_count = 0U;
    (void)memset(repl->resources, 0, sizeof(repl->resources));
}

static size_t ansi_token_length(const char *text, size_t length,
                                size_t offset, bool *reset)
{
    size_t cursor;

    if (text == NULL || reset == NULL || offset >= length ||
        length - offset < 3U || (unsigned char)text[offset] != 0x1bU ||
        text[offset + 1U] != '[') {
        return 0;
    }
    for (cursor = offset + 2U; cursor < length; cursor++) {
        unsigned char byte = (unsigned char)text[cursor];

        if (byte >= 0x40U && byte <= 0x7eU) {
            if (byte != 'm') {
                return 0;
            }
            *reset = cursor == offset + 3U && text[offset + 2U] == '0';
            return cursor - offset + 1U;
        }
        if (!((byte >= '0' && byte <= '9') || byte == ';')) {
            return 0;
        }
    }
    return 0;
}

static size_t utf8_token_length(const char *text, size_t length,
                                size_t offset)
{
    unsigned char first;
    size_t wanted;
    size_t index;

    if (text == NULL || offset >= length) {
        return 0;
    }
    first = (unsigned char)text[offset];
    if (first < 0x80U) {
        return 1;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
        wanted = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
        wanted = 3;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        wanted = 4;
    } else {
        return 1;
    }
    if (wanted > length - offset) {
        return 1;
    }
    for (index = 1; index < wanted; index++) {
        if (((unsigned char)text[offset + index] & 0xc0U) != 0x80U) {
            return 1;
        }
    }
    return wanted;
}

static size_t display_token_width(const char *text, size_t length,
                                  size_t offset, size_t column,
                                  size_t token_length)
{
    mbstate_t state;
    wchar_t character;
    size_t bytes;
    int columns;

    if (text == NULL || offset >= length || token_length == 0U) return 0U;
    if (text[offset] == '\t') return 8U - column % 8U;
    (void)memset(&state, 0, sizeof(state));
    bytes = mbrtowc(&character, text + offset, token_length, &state);
    if (bytes == (size_t)-1 || bytes == (size_t)-2 || bytes == 0U)
        return 1U;
    columns = wcwidth(character);
    return columns < 0 ? 1U : (size_t)columns;
}

static void discard_row_resources(gsh_async_repl *repl, size_t row)
{
    size_t source;
    size_t used = 0U;
    if (repl == NULL) return;
    for (source = 0U; source < repl->resource_count; source++) {
        if (repl->resources[source].occupied &&
            repl->resources[source].view_row != row) {
            if (used != source) repl->resources[used] = repl->resources[source];
            used++;
        }
    }
    repl->resource_count = used;
}

static size_t push_view_row(gsh_async_repl *repl, const char *text,
                            size_t length)
{
    if (repl == NULL) return SIZE_MAX;
    if (text == NULL) {
        return SIZE_MAX;
    }
    static const char reset_style[] = "\033[0m";
    char clipped[GSH_ASYNC_VIEW_BYTES];
    size_t source = 0;
    size_t used = 0;
    size_t columns = 0;
    size_t tokens = 0;
    bool style_active = false;
    size_t row;

    while (source < length && tokens < GSH_ASYNC_VIEW_BYTES) {
        bool reset = false;
        size_t token = ansi_token_length(text, length, source, &reset);
        size_t width = 0;

        if (token != 0) {
            style_active = !reset;
        } else {
            token = utf8_token_length(text, length, source);
            if (token == 0) {
                break;
            }
            width = display_token_width(text, length, source, columns,
                                        token);
            if (columns + width > repl->view_columns) {
                break;
            }
        }
        if (token > sizeof(clipped) - 1U - used) {
            break;
        }
        (void)memcpy(clipped + used, text + source, token);
        used += token;
        source += token;
        columns += width;
        tokens++;
    }
    if (style_active && sizeof(reset_style) - 1U <=
                            sizeof(clipped) - 1U - used) {
        (void)memcpy(clipped + used, reset_style, sizeof(reset_style) - 1U);
        used += sizeof(reset_style) - 1U;
    }
    if (repl->view_count < GSH_ASYNC_VIEW_ROWS) {
        row = (repl->view_start + repl->view_count) % GSH_ASYNC_VIEW_ROWS;
        repl->view_count++;
    } else {
        row = repl->view_start;
        repl->view_start = (repl->view_start + 1U) % GSH_ASYNC_VIEW_ROWS;
    }
    discard_row_resources(repl, row);
    if (used != 0) {
        (void)memcpy(repl->view[row], clipped, used);
    }
    repl->view[row][used] = '\0';
    repl->view_lengths[row] = used;
    return row;
}

static const char *resource_style(gsh_resource_type type)
{
    if (type == GSH_RESOURCE_DIRECTORY) return "\033[4;38;5;75m";
    if (type == GSH_RESOURCE_SYMLINK) return "\033[4;38;5;176m";
    return "\033[4;38;5;81m";
}

static size_t native_row_candidates(
    const gsh_async_repl *repl, int cell_index, size_t output_row,
    const char *text, size_t length, gsh_resource_candidate *candidates,
    size_t capacity)
{
    size_t resource_index;
    size_t count = 0U;
    if (repl == NULL || text == NULL || candidates == NULL ||
        !cell_index_valid(cell_index)) return 0U;
    for (resource_index = 0U;
         resource_index < GSH_ASYNC_NATIVE_RESOURCE_CAP && count < capacity;
         resource_index++) {
        const gsh_async_native_resource *native =
            &repl->native_resources[resource_index];
        size_t position;
        if (!native->occupied || native->cell_index != cell_index ||
            native->cell_id != repl->cells[cell_index].id ||
            native->output_row != output_row ||
            native->byte_end > length ||
            native->byte_end - native->byte_begin != native->label_length ||
            memcmp(text + native->byte_begin, native->label,
                   native->label_length) != 0) continue;
        position = count;
        while (position > 0U &&
               candidates[position - 1U].begin > native->byte_begin) {
            candidates[position] = candidates[position - 1U];
            position--;
        }
        (void)memset(&candidates[position], 0, sizeof(candidates[position]));
        candidates[position].begin = native->byte_begin;
        candidates[position].end = native->byte_end;
        candidates[position].column_begin = native->column_begin;
        candidates[position].column_end = native->column_end;
        (void)memcpy(candidates[position].path, native->path,
                     strlen(native->path) + 1U);
        candidates[position].provenance = GSH_RESOURCE_NATIVE;
        candidates[position].type = native->type;
        candidates[position].navigable_root = native->navigable_root;
        count++;
    }
    return count;
}

static int register_resource(gsh_async_repl *repl, int cell_index,
                             size_t view_row,
                             const gsh_resource_candidate *candidate)
{
    gsh_async_resource_action *action;
    const gsh_async_cell *cell;

    if (repl == NULL || candidate == NULL ||
        !cell_index_valid(cell_index) ||
        repl->resource_count >= GSH_ASYNC_RESOURCE_CAP) return -1;
    cell = &repl->cells[cell_index];
    action = &repl->resources[repl->resource_count++];
    (void)memset(action, 0, sizeof(*action));
    action->occupied = true;
    action->cell_index = cell_index;
    action->cell_id = cell->id;
    action->generation = cell->output_generation;
    action->view_row = view_row;
    action->column_begin = candidate->column_begin + 1U;
    action->column_end = candidate->column_end;
    (void)memcpy(action->path, candidate->path,
                 strlen(candidate->path) + 1U);
    (void)memcpy(action->launch_directory, cell->launch_directory,
                 strlen(cell->launch_directory) + 1U);
    action->line = candidate->line;
    action->column = candidate->column;
    action->provenance = candidate->provenance;
    action->type = candidate->type;
    action->stderr_stream = false;
    action->navigable_root = candidate->navigable_root;
    return 0;
}

static size_t style_resource_row(gsh_async_repl *repl, int cell_index,
                                 size_t output_row, const char *text,
                                 size_t length,
                                 char *styled, size_t capacity,
                                 gsh_resource_candidate *candidates,
                                 size_t *candidate_count)
{
    size_t count;
    size_t source = 0U;
    size_t used = 0U;
    size_t index;
    const gsh_async_cell *cell;

    if (repl == NULL || text == NULL || styled == NULL ||
        candidates == NULL || candidate_count == NULL ||
        !cell_index_valid(cell_index)) return 0U;
    cell = &repl->cells[cell_index];
    count = native_row_candidates(repl, cell_index, output_row, text, length,
                                  candidates, 16U);
    if (count == 0U)
        count = gsh_resource_detect(cell->command, cell->launch_directory,
                                    text, length, repl->path_detection,
                                    candidates, 16U);
    for (index = 0U; index < count; index++) {
        const char *style = resource_style(candidates[index].type);
        size_t style_length = strlen(style);
        size_t plain = candidates[index].begin >= source
                           ? candidates[index].begin - source : 0U;
        size_t marked = candidates[index].end - candidates[index].begin;
        if (plain + style_length + marked + 4U > capacity - used) break;
        (void)memcpy(styled + used, text + source, plain);
        used += plain;
        (void)memcpy(styled + used, style, style_length);
        used += style_length;
        (void)memcpy(styled + used, text + candidates[index].begin, marked);
        used += marked;
        (void)memcpy(styled + used, "\033[0m", 4U);
        used += 4U;
        source = candidates[index].end;
    }
    if (length - source <= capacity - used) {
        (void)memcpy(styled + used, text + source, length - source);
        used += length - source;
    }
    *candidate_count = index;
    return used;
}

static void push_resource_row(gsh_async_repl *repl, int cell_index,
                              size_t output_row, const char *text,
                              size_t length)
{
    char styled[GSH_ASYNC_VIEW_BYTES * 4U];
    gsh_resource_candidate candidates[16];
    size_t count = 0U;
    size_t styled_length;
    size_t row;
    size_t index;

    if (repl == NULL || text == NULL) return;
    if (!repl->actions_enabled) {
        (void)push_view_row(repl, text, length);
        return;
    }
    styled_length = style_resource_row(repl, cell_index, output_row,
                                       text, length,
                                       styled, sizeof(styled), candidates,
                                       &count);
    row = push_view_row(repl, count == 0U ? text : styled,
                        count == 0U ? length : styled_length);
    if (row == SIZE_MAX) return;
    for (index = 0U; index < count; index++) {
        if (candidates[index].begin < repl->view_columns) {
            if (candidates[index].end > repl->view_columns)
                candidates[index].end = repl->view_columns;
            (void)register_resource(repl, cell_index, row,
                                    &candidates[index]);
        }
    }
}

typedef struct {
    size_t view_row;
    size_t column;
} editor_render_cursor;

/* ── Editor Text Becomes Physical Rows Before Composition ───────
 * The mutable command is one bounded byte buffer, but explicit newlines and
 * terminal-width wrapping are physical rows. Splitting here keeps every row
 * in the same ring as job output and records the cursor before scrolling.
 * The compositor can repaint and position atomically without rescanning the
 * editor or depending on a terminal's implicit wrap state.
 * ─────────────────────────────────────────────────────────────── */
static int push_editor_rows(gsh_async_repl *repl, const char *prefix,
                            const char *text, size_t length,
                            size_t editor_cursor,
                            editor_render_cursor *cursor)
{
    char combined[GSH_ASYNC_PROMPT_CAP + GSH_ASYNC_COMMAND_CAP];
    size_t prefix_length;
    size_t total;
    size_t wanted_cursor;
    size_t source = 0U;
    bool cursor_recorded = false;

    if (prefix == NULL || repl == NULL || text == NULL || cursor == NULL ||
        editor_cursor > length) return -1;
    prefix_length = strnlen(prefix, GSH_ASYNC_PROMPT_CAP);
    if (prefix_length == GSH_ASYNC_PROMPT_CAP ||
        length > sizeof(combined) - prefix_length) return -1;
    (void)memcpy(combined, prefix, prefix_length);
    if (length != 0U)
        (void)memcpy(combined + prefix_length, text, length);
    total = prefix_length + length;
    wanted_cursor = prefix_length + editor_cursor;

    do {
        size_t begin = source;
        size_t columns = 0U;
        size_t cursor_column = SIZE_MAX;
        bool newline = false;
        size_t row;

        if (source == wanted_cursor) cursor_column = 0U;
        while (source < total) {
            size_t token;
            size_t width;

            if (combined[source] == '\n') {
                newline = true;
                break;
            }
            token = utf8_token_length(combined, total, source);
            if (token == 0U) break;
            width = display_token_width(combined, total, source, columns,
                                        token);
            if (width > repl->view_columns) width = repl->view_columns;
            if (columns != 0U &&
                columns + width > repl->view_columns) break;
            source += token;
            columns += width;
            if (source == wanted_cursor) cursor_column = columns;
            if (columns >= repl->view_columns) break;
        }
        if (!newline && source < total && source == wanted_cursor)
            cursor_column = SIZE_MAX;
        if (!newline && source == total && columns >= repl->view_columns &&
            source == wanted_cursor)
            cursor_column = SIZE_MAX;
        row = push_view_row(repl, combined + begin, source - begin);
        if (row == SIZE_MAX) return -1;
        if (cursor_column != SIZE_MAX) {
            cursor->view_row = row;
            cursor->column = cursor_column < repl->view_columns
                                 ? cursor_column
                                 : repl->view_columns - 1U;
            cursor_recorded = true;
        }
        if (newline) source++;
    } while (source < total || !cursor_recorded);
    return cursor_recorded ? 0 : -1;
}

static void push_output_rows(gsh_async_repl *repl,
                             const gsh_async_cell *cell, int cell_index)
{
    if (cell == NULL || repl == NULL) {
        return;
    }
    size_t start = 0;
    size_t offset;
    size_t output_row = 0U;

    for (offset = 0; offset < cell->output_length; offset++) {
        if (cell->output[offset] == '\n') {
            push_resource_row(repl, cell_index, output_row,
                              cell->output + start,
                              offset - start);
            start = offset + 1U;
            output_row++;
        }
    }
    if (start < cell->output_length) {
        push_resource_row(repl, cell_index, output_row,
                          cell->output + start,
                          cell->output_length - start);
    }
}

static void push_cell(gsh_async_repl *repl, const gsh_async_cell *cell,
                      int cell_index)
{
    if (cell == NULL || repl == NULL) {
        return;
    }
    char command_row[GSH_ASYNC_PROMPT_CAP + GSH_ASYNC_COMMAND_CAP] = {0};
    size_t command_length = cell->prompt_length + cell->command_length;

    if (command_length > sizeof(command_row) - 1U) {
        command_length = sizeof(command_row) - 1U;
    }
    if (cell->prompt_length < sizeof(command_row)) {
        (void)memcpy(command_row, cell->prompt, cell->prompt_length);
    }
    if (command_length > cell->prompt_length) {
        (void)memcpy(command_row + cell->prompt_length, cell->command,
               command_length - cell->prompt_length);
    }
    (void)push_view_row(repl, command_row, command_length);
    push_output_rows(repl, cell, cell_index);
    if (cell->output_truncated) {
        (void)push_view_row(repl, "[output truncated]", 18);
    }
}

static size_t ordered_cells(const gsh_async_repl *repl,
                            int ordered[GSH_ASYNC_CELL_CAP])
{
    if (repl == NULL) return 0U;
    if (ordered == NULL) {
        return 0U;
    }
    size_t count = 0;
    int index;

    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        size_t position;

        if (!repl->cells[index].occupied) {
            continue;
        }
        position = count;
        while (position > 0 &&
               repl->cells[ordered[position - 1U]].id >
                   repl->cells[index].id) {
            ordered[position] = ordered[position - 1U];
            position--;
        }
        ordered[position] = index;
        count++;
    }
    return count;
}

static int render_append(gsh_async_repl *repl, const char *text,
                         size_t length)
{
    if (text == NULL) {
        return -1;
    }
    if (length > sizeof(repl->render) - repl->render_length) {
        errno = ENOBUFS;
        return -1;
    }
    (void)memcpy(repl->render + repl->render_length, text, length);
    repl->render_length += length;
    return 0;
}

static int render_cursor(gsh_async_repl *repl, size_t row, size_t column)
{
    char sequence[64];
    int length;
    if (repl == NULL || row == 0U || column == 0U) return -1;
    length = snprintf(sequence, sizeof(sequence), "\033[%zu;%zuH", row,
                      column);
    if (length < 0 || (size_t)length >= sizeof(sequence)) return -1;
    return render_append(repl, sequence, (size_t)length);
}

static int render_clear_split(gsh_async_repl *repl,
                              size_t separator_column)
{
    size_t row;
    char erase[64];
    int length;
    if (repl == NULL || separator_column <= 1U) return -1;
    length = snprintf(erase, sizeof(erase), "\033[%zuX",
                      separator_column - 1U);
    if (length < 0 || (size_t)length >= sizeof(erase)) return -1;
    for (row = 1U; row <= repl->terminal_rows; row++) {
        if (render_cursor(repl, row, 1U) == -1 ||
            render_append(repl, erase, (size_t)length) == -1) return -1;
    }
    return 0;
}

static bool visible_resource_exists(const gsh_async_repl *repl, size_t skip,
                                    size_t visible)
{
    size_t index;
    if (repl == NULL) return false;
    for (index = 0U; index < repl->resource_count; index++) {
        const gsh_async_resource_action *resource = &repl->resources[index];
        size_t position;
        if (!resource->occupied ||
            resource->view_row >= GSH_ASYNC_VIEW_ROWS) continue;
        position = (resource->view_row + GSH_ASYNC_VIEW_ROWS -
                    repl->view_start) % GSH_ASYNC_VIEW_ROWS;
        if (position >= skip && position < skip + visible) return true;
    }
    return false;
}

static int compose_render(gsh_async_repl *repl,
                          const editor_render_cursor *editor_cursor)
{
    if (repl == NULL) {
        return -1;
    }
    size_t visible = repl->terminal_rows;
    size_t maximum_offset;
    size_t skip;
    size_t row_index;
    size_t separator_column = 0U;
    bool split;
    bool have_visible_resource;

    /* ── One Compositor Owns the Physical Terminal ───────────────
     * Per-job PTYs prevent a child from moving or erasing the real editor.
     * Captured bytes become bounded logical rows before they reach this path.
     * Each refresh rebuilds one viewport from cells plus the mutable editor.
     * Alternate-screen ownership makes that redraw atomic to terminal users.
     * Unsupported child control sequences degrade to contained plain text.
     * ─────────────────────────────────────────────────────────────── */
    repl->render_length = 0;
    if (render_append(repl, "\033[?2004h", 8U) == -1 ||
        (!repl->alternate_screen_entered &&
         render_append(repl, "\033[?1049h", 8U) == -1)) {
        return -1;
    }
    split = gsh_async_repl_split_preview(repl, &separator_column) >= 0;
    if ((!repl->alternate_screen_entered || !split) &&
        render_append(repl, "\033[H\033[2J", 7) == -1) return -1;
    if (repl->alternate_screen_entered && split &&
        render_clear_split(repl, separator_column) == -1) return -1;
    if (visible > repl->view_count) {
        visible = repl->view_count;
    }
    maximum_offset = repl->view_count - visible;
    if (repl->scroll_offset > maximum_offset)
        repl->scroll_offset = maximum_offset;
    skip = maximum_offset - repl->scroll_offset;
    if (editor_cursor != NULL && repl->scroll_offset == 0U) {
        size_t cursor_position =
            (editor_cursor->view_row + GSH_ASYNC_VIEW_ROWS -
             repl->view_start) % GSH_ASYNC_VIEW_ROWS;

        if (cursor_position < repl->view_count && cursor_position < skip) {
            skip = cursor_position;
            repl->scroll_offset = maximum_offset - skip;
        }
    }
    have_visible_resource = visible_resource_exists(repl, skip, visible);
    if (have_visible_resource || maximum_offset != 0U || split) {
        if (render_append(repl, "\033[?1000h\033[?1006h", 16U) == -1)
            return -1;
        repl->mouse_enabled = true;
    } else if (repl->mouse_enabled) {
        if (render_append(repl, "\033[?1000l\033[?1006l", 16U) == -1)
            return -1;
        repl->mouse_enabled = false;
    }
    for (row_index = skip; row_index < skip + visible; row_index++) {
        size_t row = (repl->view_start + row_index) % GSH_ASYNC_VIEW_ROWS;

        repl->screen_row_by_view[row] = row_index - skip + 1U;

        if ((split && render_cursor(repl, row_index - skip + 1U, 1U) == -1) ||
            render_append(repl, repl->view[row], repl->view_lengths[row]) ==
                -1 ||
            (!split && row_index + 1U < skip + visible &&
             render_append(repl, "\n", 1U) == -1)) {
            return -1;
        }
    }
    if (editor_cursor != NULL) {
        size_t screen_row = repl->screen_row_by_view[editor_cursor->view_row];

        if (screen_row != 0U &&
            render_cursor(repl, screen_row, editor_cursor->column + 1U) ==
                -1) return -1;
    }
    return 0;
}

int gsh_async_repl_prepare_render(gsh_async_repl *repl,
                                  const char *active_prompt,
                                  const char *editor, size_t editor_length,
                                  size_t editor_cursor)
{
    int ordered[GSH_ASYNC_CELL_CAP];
    editor_render_cursor cursor;
    size_t count;
    size_t index;
    size_t separator_column = 0U;

    if (repl == NULL || active_prompt == NULL || editor == NULL ||
        editor_length >= GSH_ASYNC_COMMAND_CAP ||
        editor_cursor > editor_length || !repl->enabled) {
        errno = EINVAL;
        return -1;
    }
    repl->view_columns =
        gsh_async_repl_split_preview(repl, &separator_column) >= 0
            ? separator_column - 1U : repl->terminal_columns;
    clear_view(repl);
    count = ordered_cells(repl, ordered);
    for (index = 0; index < count; index++) {
        push_cell(repl, &repl->cells[ordered[index]], ordered[index]);
    }
    if (push_editor_rows(repl, active_prompt, editor, editor_length,
                         editor_cursor, &cursor) == -1 ||
        compose_render(repl, &cursor) == -1) {
        return -1;
    }
    repl->alternate_screen_entered = true;
    return 0;
}

const char *gsh_async_repl_render_data(const gsh_async_repl *repl)
{
    if (repl == NULL) {
        return NULL;
    }
    return repl == NULL ? NULL : repl->render;
}

size_t gsh_async_repl_render_length(const gsh_async_repl *repl)
{
    if (repl == NULL) {
        return 0U;
    }
    return repl == NULL ? 0 : repl->render_length;
}

void gsh_async_repl_rendered(gsh_async_repl *repl)
{
    if (repl != NULL) {
        repl->render_pending = false;
    }
}

static void signal_cell_job(gsh_async_cell *cell, int signal_number)
{
    if (cell == NULL) return;
#ifdef TIOCSIG
    if (cell->pty_fd >= 0 &&
        ioctl(cell->pty_fd, TIOCSIG, signal_number) == 0) {
        return;
    }
#endif
    if (cell->pgid > 0 && kill(-cell->pgid, signal_number) == 0) {
        return;
    }
    if (cell->pid > 0) {
        (void)kill(cell->pid, signal_number);
    }
}

void gsh_async_repl_close(gsh_async_repl *repl)
{
    int index;

    if (repl == NULL) {
        return;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        gsh_async_cell *cell = &repl->cells[index];

        if (cell->pgid > 0 && !cell_terminal(cell)) {
            pid_t foreground = cell->pty_fd < 0
                                   ? -1
                                   : tcgetpgrp(cell->pty_fd);

            if (foreground > 0) {
                cell->pgid = foreground;
            }
            signal_cell_job(cell, SIGHUP);
            signal_cell_job(cell, SIGCONT);
        }
        if (cell->pty_fd >= 0) {
            (void)close(cell->pty_fd);
            cell->pty_fd = -1;
        }
        if (cell->resource_fd >= 0) {
            (void)close(cell->resource_fd);
            cell->resource_fd = -1;
        }
    }
}

int gsh_async_repl_resource_at(const gsh_async_repl *repl, size_t row,
                               size_t column,
                               gsh_async_resource_action *action)
{
    size_t index;
    if (repl == NULL || action == NULL || row == 0U || column == 0U) {
        errno = EINVAL;
        return -1;
    }
    for (index = 0U; index < repl->resource_count; index++) {
        const gsh_async_resource_action *candidate = &repl->resources[index];
        const gsh_async_cell *cell;
        if (!candidate->occupied ||
            candidate->view_row >= GSH_ASYNC_VIEW_ROWS ||
            repl->screen_row_by_view[candidate->view_row] != row ||
            column < candidate->column_begin ||
            column > candidate->column_end ||
            !cell_index_valid(candidate->cell_index)) continue;
        cell = &repl->cells[candidate->cell_index];
        if (!cell->occupied || cell->id != candidate->cell_id ||
            cell->output_generation != candidate->generation) continue;
        *action = *candidate;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

int gsh_async_repl_suspended_fullscreen(const gsh_async_repl *repl)
{
    uint64_t newest = 0U;
    int selected = -1;
    int index;
    if (repl == NULL) return -1;
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        const gsh_async_cell *cell = &repl->cells[index];
        if (cell->occupied && cell->fullscreen && !cell->focused &&
            cell->state == GSH_ASYNC_RUNNING && cell->id > newest) {
            newest = cell->id;
            selected = index;
        }
    }
    return selected;
}
