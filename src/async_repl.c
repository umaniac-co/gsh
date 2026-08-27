#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "async_repl.h"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

enum { GSH_ASYNC_INITIAL_OUTPUT_ROWS = 1 };

/* ── Cells Separate Output From Editor Ownership ─────────────────
 * Direct terminal output made the next prompt depend on job completion.
 * A cell now owns the immutable command, lifecycle, PTY, and captured bytes.
 * The editor is separate state and can therefore become active at Enter.
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
    int descriptor;

    if (cell == NULL) {
        return;
    }
    descriptor = cell->pty_fd;
    memset(cell, 0, sizeof(*cell));
    cell->pty_fd = -1;
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
}

void gsh_async_repl_initialize(gsh_async_repl *repl, bool enabled)
{
    int index;

    if (repl == NULL) {
        return;
    }
    memset(repl, 0, sizeof(*repl));
    repl->enabled = enabled;
    repl->next_id = 1;
    repl->terminal_rows = 24;
    repl->terminal_columns = 80;
    repl->render_pending = enabled;
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        repl->cells[index].pty_fd = -1;
    }
}

void gsh_async_repl_resize(gsh_async_repl *repl, size_t rows,
                           size_t columns)
{
    if (repl == NULL || !repl->enabled) {
        return;
    }
    repl->terminal_rows = rows == 0 ? 1 : rows;
    repl->terminal_columns = columns == 0 ? 1 : columns;
    if (repl->terminal_rows > GSH_ASYNC_VIEW_ROWS) {
        repl->terminal_rows = GSH_ASYNC_VIEW_ROWS;
    }
    if (repl->terminal_columns > GSH_ASYNC_VIEW_COLUMNS) {
        repl->terminal_columns = GSH_ASYNC_VIEW_COLUMNS;
    }
    repl->render_pending = true;
}

static int copy_cell_text(gsh_async_cell *cell, const char *prompt,
                          const char *command, size_t command_length)
{
    size_t prompt_length;

    if (cell == NULL || prompt == NULL || command == NULL ||
        command_length >= sizeof(cell->command)) {
        errno = EINVAL;
        return -1;
    }
    prompt_length = strlen(prompt);
    if (prompt_length >= sizeof(cell->prompt)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(cell->prompt, prompt, prompt_length + 1U);
    memcpy(cell->command, command, command_length);
    cell->command[command_length] = '\0';
    cell->prompt_length = prompt_length;
    cell->command_length = command_length;
    return 0;
}

int gsh_async_repl_accept(gsh_async_repl *repl, const char *prompt,
                          const char *command, size_t command_length,
                          bool barrier, bool blocks_independent,
                          bool status_dependency, bool control)
{
    int cell_index = oldest_reusable_cell(repl, control);
    gsh_async_cell *cell;

    if (cell_index < 0 || repl == NULL || repl->next_id == UINT64_MAX) {
        errno = ENOSPC;
        return -1;
    }
    cell = &repl->cells[cell_index];
    reset_cell(cell);
    if (copy_cell_text(cell, prompt, command, command_length) == -1) {
        return -1;
    }
    cell->occupied = true;
    cell->barrier = barrier;
    cell->blocks_independent = blocks_independent;
    cell->status_dependency = status_dependency;
    cell->control = control;
    cell->id = repl->next_id++;
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
                          pid_t pgid, int pty_fd)
{
    gsh_async_cell *cell;

    if (repl == NULL || !cell_index_valid(cell_index) || pid <= 0 ||
        pgid <= 0 || pty_fd < 0) {
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

int gsh_async_repl_reap(gsh_async_repl *repl, pid_t pid, int wait_status)
{
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
    cell->pid = 0;
    cell->pgid = 0;
    cell->output_closed = cell->pty_fd < 0;
    repl->render_pending = true;
    return cell_index;
}

static int append_visible_byte(gsh_async_cell *cell, unsigned char byte)
{
    if (cell->output_length >= sizeof(cell->output) - 1U) {
        cell->output_truncated = true;
        return 0;
    }
    cell->output[cell->output_length++] = (char)byte;
    cell->output[cell->output_length] = '\0';
    return 0;
}

static void append_terminal_byte(gsh_async_cell *cell, unsigned char byte)
{
    if (cell->escape_state == 1U) {
        cell->escape_state = byte == '[' ? 2U : (byte == ']' ? 3U : 0U);
        return;
    }
    if (cell->escape_state == 2U) {
        if (byte >= 0x40U && byte <= 0x7eU) {
            cell->escape_state = 0;
        }
        return;
    }
    if (cell->escape_state == 3U || cell->escape_state == 4U) {
        if (byte == 0x07U || (cell->escape_state == 4U && byte == '\\')) {
            cell->escape_state = 0;
        } else {
            cell->escape_state = byte == 0x1bU ? 4U : 3U;
        }
        return;
    }
    if (byte == 0x1bU) {
        cell->escape_state = 1U;
    } else if (byte == '\r') {
        (void)append_visible_byte(cell, '\n');
        cell->last_was_cr = true;
    } else if (byte == '\n') {
        if (!cell->last_was_cr) {
            (void)append_visible_byte(cell, '\n');
        }
        cell->last_was_cr = false;
    } else if (byte == '\t') {
        (void)append_visible_byte(cell, ' ');
        cell->last_was_cr = false;
    } else if (byte >= 0x20U) {
        (void)append_visible_byte(cell, byte);
        cell->last_was_cr = false;
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
        memmove(cell->input, cell->input + cell->input_offset,
                cell->input_length - cell->input_offset);
        cell->input_length -= cell->input_offset;
        cell->input_offset = 0;
    }
    if (length > sizeof(cell->input) - cell->input_length) {
        errno = ENOSPC;
        return -1;
    }
    memcpy(cell->input + cell->input_length, bytes, length);
    cell->input_length += length;
    return 0;
}

bool gsh_async_repl_input_pending(const gsh_async_repl *repl,
                                  int cell_index)
{
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
    cell->input_offset = 0;
    cell->input_length = 0;
    repl->render_pending = true;
}

int gsh_async_repl_latest_job(const gsh_async_repl *repl)
{
    uint64_t latest = 0;
    int selected = -1;
    int index;

    if (repl == NULL) {
        return -1;
    }
    for (index = 0; index < GSH_ASYNC_CELL_CAP; index++) {
        const gsh_async_cell *cell = &repl->cells[index];

        if (cell->occupied && cell->pgid > 0 && !cell_terminal(cell) &&
            cell->id > latest) {
            latest = cell->id;
            selected = index;
        }
    }
    return selected;
}

int gsh_async_repl_focused_job(const gsh_async_repl *repl)
{
    int index;

    if (repl == NULL) {
        return -1;
    }
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
    int focused = gsh_async_repl_focused_job(repl);

    if (repl == NULL || !cell_index_valid(cell_index) ||
        repl->cells[cell_index].pgid <= 0 ||
        cell_terminal(&repl->cells[cell_index])) {
        errno = EINVAL;
        return -1;
    }
    if (focused >= 0) {
        repl->cells[focused].focused = false;
    }
    repl->cells[cell_index].focused = true;
    repl->render_pending = true;
    return 0;
}

void gsh_async_repl_unfocus(gsh_async_repl *repl)
{
    int focused = gsh_async_repl_focused_job(repl);

    if (repl == NULL || focused < 0) {
        return;
    }
    repl->cells[focused].focused = false;
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
    repl->view_start = 0;
    repl->view_count = 0;
    memset(repl->view_lengths, 0, sizeof(repl->view_lengths));
}

static void push_view_row(gsh_async_repl *repl, const char *text,
                          size_t length)
{
    size_t row;

    if (length > repl->terminal_columns) {
        length = repl->terminal_columns;
    }
    if (repl->view_count < GSH_ASYNC_VIEW_ROWS) {
        row = (repl->view_start + repl->view_count) % GSH_ASYNC_VIEW_ROWS;
        repl->view_count++;
    } else {
        row = repl->view_start;
        repl->view_start = (repl->view_start + 1U) % GSH_ASYNC_VIEW_ROWS;
    }
    if (length != 0) {
        memcpy(repl->view[row], text, length);
    }
    repl->view[row][length] = '\0';
    repl->view_lengths[row] = length;
}

static void push_prefixed_row(gsh_async_repl *repl, const char *prefix,
                              const char *text, size_t length)
{
    char row[GSH_ASYNC_VIEW_COLUMNS + 1U];
    size_t prefix_length = strlen(prefix);
    size_t available;

    if (prefix_length > repl->terminal_columns) {
        prefix_length = repl->terminal_columns;
    }
    memcpy(row, prefix, prefix_length);
    available = repl->terminal_columns - prefix_length;
    if (length > available) {
        length = available;
    }
    if (length != 0) {
        memcpy(row + prefix_length, text, length);
    }
    push_view_row(repl, row, prefix_length + length);
}

static size_t push_output_rows(gsh_async_repl *repl,
                               const gsh_async_cell *cell)
{
    size_t start = 0;
    size_t rows = 0;
    size_t offset;

    for (offset = 0; offset < cell->output_length; offset++) {
        if (cell->output[offset] == '\n') {
            push_view_row(repl, cell->output + start, offset - start);
            rows++;
            start = offset + 1U;
        }
    }
    if (start < cell->output_length) {
        push_view_row(repl, cell->output + start,
                      cell->output_length - start);
        rows++;
    }
    return rows;
}

static void push_cell(gsh_async_repl *repl, const gsh_async_cell *cell)
{
    char command_row[GSH_ASYNC_VIEW_COLUMNS + 1U];
    size_t command_length = cell->prompt_length + cell->command_length;
    size_t output_rows;

    if (command_length > sizeof(command_row) - 1U) {
        command_length = sizeof(command_row) - 1U;
    }
    if (cell->prompt_length < sizeof(command_row)) {
        memcpy(command_row, cell->prompt, cell->prompt_length);
    }
    if (command_length > cell->prompt_length) {
        memcpy(command_row + cell->prompt_length, cell->command,
               command_length - cell->prompt_length);
    }
    push_view_row(repl, command_row, command_length);
    output_rows = push_output_rows(repl, cell);
    while (output_rows < GSH_ASYNC_INITIAL_OUTPUT_ROWS) {
        push_view_row(repl, "", 0);
        output_rows++;
    }
    if (cell->output_truncated) {
        push_view_row(repl, "[output truncated]", 18);
    }
}

static size_t ordered_cells(const gsh_async_repl *repl,
                            int ordered[GSH_ASYNC_CELL_CAP])
{
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
    if (length > sizeof(repl->render) - repl->render_length) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(repl->render + repl->render_length, text, length);
    repl->render_length += length;
    return 0;
}

static int compose_render(gsh_async_repl *repl)
{
    size_t visible = repl->terminal_rows;
    size_t skip;
    size_t row_index;

    /* ── One Compositor Owns the Physical Terminal ───────────────
     * Per-job PTYs prevent a child from moving or erasing the real editor.
     * Captured bytes become bounded logical rows before they reach this path.
     * Each refresh rebuilds one viewport from cells plus the mutable editor.
     * Alternate-screen ownership makes that redraw atomic to terminal users.
     * Unsupported child control sequences degrade to contained plain text.
     * ─────────────────────────────────────────────────────────────── */
    repl->render_length = 0;
    if (!repl->alternate_screen_entered &&
        render_append(repl, "\033[?1049h", 8) == -1) {
        return -1;
    }
    if (render_append(repl, "\033[H\033[2J", 7) == -1) {
        return -1;
    }
    if (visible > repl->view_count) {
        visible = repl->view_count;
    }
    skip = repl->view_count - visible;
    for (row_index = skip; row_index < repl->view_count; row_index++) {
        size_t row = (repl->view_start + row_index) % GSH_ASYNC_VIEW_ROWS;

        if (render_append(repl, repl->view[row], repl->view_lengths[row]) ==
                -1 ||
            (row_index + 1U < repl->view_count &&
             render_append(repl, "\n", 1) == -1)) {
            return -1;
        }
    }
    return 0;
}

int gsh_async_repl_prepare_render(gsh_async_repl *repl,
                                  const char *active_prompt,
                                  const char *editor, size_t editor_length)
{
    int ordered[GSH_ASYNC_CELL_CAP];
    size_t count;
    size_t index;

    if (repl == NULL || active_prompt == NULL || editor == NULL ||
        editor_length >= GSH_ASYNC_COMMAND_CAP || !repl->enabled) {
        errno = EINVAL;
        return -1;
    }
    clear_view(repl);
    count = ordered_cells(repl, ordered);
    for (index = 0; index < count; index++) {
        push_cell(repl, &repl->cells[ordered[index]]);
    }
    push_prefixed_row(repl, active_prompt, editor, editor_length);
    if (compose_render(repl) == -1) {
        return -1;
    }
    repl->alternate_screen_entered = true;
    return 0;
}

const char *gsh_async_repl_render_data(const gsh_async_repl *repl)
{
    return repl == NULL ? NULL : repl->render;
}

size_t gsh_async_repl_render_length(const gsh_async_repl *repl)
{
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
    }
}
