#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "../src/async_repl.h"
#include <stdio.h>
#include <string.h>

static int detector_cases(void)
{
    static const char ls_row[] = "alpha.py";
    static const char grep_row[] = "src/a.c:12:3:error";
    static const char url[] = "https://example/x";
    static const char assignment[] = "ORDER_STATE=/";
    gsh_resource_candidate found[8];
    size_t count;

    count = gsh_resource_detect("/bin/ls -1", "/tmp", ls_row,
                                sizeof(ls_row) - 1U,
                                GSH_PATH_DETECTION_KNOWN, found, 8U);
    if (count != 1U || strcmp(found[0].path, "alpha.py") != 0 ||
        found[0].type != GSH_RESOURCE_UNKNOWN ||
        found[0].provenance != GSH_RESOURCE_ADAPTER) {
        (void)fprintf(stderr, "resource actions: ls adapter count=%zu path=%s type=%d provenance=%d\n",
                      count, count == 0U ? "" : found[0].path,
                      count == 0U ? 0 : (int)found[0].type,
                      count == 0U ? 0 : (int)found[0].provenance);
        return 1;
    }
    count = gsh_resource_detect("/bin/ls -1", "/tmp", ls_row,
                                sizeof(ls_row) - 1U,
                                GSH_PATH_DETECTION_OFF, found, 8U);
    if (count != 0U) return 1;
    count = gsh_resource_detect("rg error", "/tmp", grep_row,
                                sizeof(grep_row) - 1U,
                                GSH_PATH_DETECTION_KNOWN, found, 8U);
    if (count != 1U || strcmp(found[0].path, "src/a.c") != 0 ||
        found[0].line != 12U || found[0].column != 3U) {
        (void)fprintf(stderr, "resource actions: line detector count=%zu path=%s line=%zu column=%zu\n",
                      count, count == 0U ? "" : found[0].path,
                      count == 0U ? 0U : found[0].line,
                      count == 0U ? 0U : found[0].column);
        return 1;
    }
    count = gsh_resource_detect("printf", "/tmp", url,
                                sizeof(url) - 1U,
                                GSH_PATH_DETECTION_SAFE, found, 8U);
    if (count != 0U) (void)fprintf(stderr, "resource actions: URL detector false positive\n");
    if (count != 0U) return 1;
    count = gsh_resource_detect("printf", "/tmp", assignment,
                                sizeof(assignment) - 1U,
                                GSH_PATH_DETECTION_SAFE, found, 8U);
    if (count != 1U || strcmp(found[0].path, "/") != 0 ||
        found[0].begin != sizeof("ORDER_STATE=") - 1U) {
        (void)fprintf(stderr, "resource actions: assignment path mismatch\n");
        return 1;
    }
    return 0;
}

static int adapter_cases(void)
{
    static const char find_row[] = "README with space";
    static const char tree_row[] = "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 \xc3\xa9.txt";
    static const char spaced_ls_row[] = "name with space.py";
    static const char long_ls_row[] = "-rw-r--r-- 1 me staff 1 Jan  1 00:00 file";
    static const char rename_row[] = "R  \"old name\" -> \"new name\"";
    static const char header[] = "On branch main";
    static const char quoted[] = "\"./dir/file name.py\",";
    gsh_resource_candidate found[8];
    size_t count;

    count = gsh_resource_detect("find .", "/tmp", find_row,
                                sizeof(find_row) - 1U,
                                GSH_PATH_DETECTION_KNOWN, found, 8U);
    if (count != 1U || strcmp(found[0].path, find_row) != 0) return 1;
    count = gsh_resource_detect("/bin/ls -1", "/tmp", spaced_ls_row,
                                sizeof(spaced_ls_row) - 1U,
                                GSH_PATH_DETECTION_KNOWN, found, 8U);
    if (count != 1U || strcmp(found[0].path, spaced_ls_row) != 0) return 1;
    count = gsh_resource_detect("/bin/ls -l", "/tmp", long_ls_row,
                                sizeof(long_ls_row) - 1U,
                                GSH_PATH_DETECTION_KNOWN, found, 8U);
    if (count != 0U) return 1;
    count = gsh_resource_detect("tree", "/tmp", tree_row,
                                sizeof(tree_row) - 1U,
                                GSH_PATH_DETECTION_KNOWN, found, 8U);
    if (count != 1U || strcmp(found[0].path, "\xc3\xa9.txt") != 0) return 1;
    count = gsh_resource_detect("git status --short", "/tmp", rename_row,
                                sizeof(rename_row) - 1U,
                                GSH_PATH_DETECTION_KNOWN, found, 8U);
    if (count != 1U || strcmp(found[0].path, "new name") != 0) return 1;
    count = gsh_resource_detect("git status", "/tmp", header,
                                sizeof(header) - 1U,
                                GSH_PATH_DETECTION_KNOWN, found, 8U);
    if (count != 0U) return 1;
    count = gsh_resource_detect("printf", "/tmp", quoted,
                                sizeof(quoted) - 1U,
                                GSH_PATH_DETECTION_SAFE, found, 8U);
    if (count != 1U || strcmp(found[0].path, "./dir/file name.py") != 0)
        return 1;
    count = gsh_resource_detect("printf", "/tmp", quoted,
                                sizeof(quoted) - 1U,
                                GSH_PATH_DETECTION_KNOWN, found, 8U);
    return count == 0U ? 0 : 1;
}

static int compositor_cases(gsh_async_repl *repl)
{
    static const char row[] = "alpha.py  FILE   -rw-r--r--\n";
    static const char label[] = "alpha.py";
    static const char path[] = "/tmp/alpha.py";
    gsh_async_resource_action action;
    int cell;

    if (repl == NULL) return 1;
    gsh_async_repl_initialize(repl, true);
    gsh_async_repl_configure_actions(repl, true, GSH_PATH_DETECTION_SAFE);
    gsh_async_repl_resize(repl, 24U, 100U);
    cell = gsh_async_repl_accept(repl, "$ ", "ll", 2U, "/tmp", false,
                                 false, false, false);
    if (cell < 0) return 1;
    gsh_async_repl_starting(repl, cell);
    if (gsh_async_repl_append(repl, cell, row, sizeof(row) - 1U) == -1 ||
        gsh_async_repl_add_native_resource(
            repl, cell, 0U, 0U, sizeof(label) - 1U,
            0U, sizeof(label) - 1U, label, sizeof(label) - 1U,
            path, sizeof(path) - 1U, GSH_RESOURCE_REGULAR, true) == -1 ||
        gsh_async_repl_prepare_render(repl, "$ ", "", 0U, 0U) == -1 ||
        strstr(gsh_async_repl_render_data(repl), "\033[?1000h") == NULL ||
        strstr(gsh_async_repl_render_data(repl), "\033[4;38;5;81m") == NULL ||
        gsh_async_repl_resource_at(repl, 2U, 2U, &action) == -1 ||
        strcmp(action.path, path) != 0 ||
        strcmp(action.launch_directory, "/tmp") != 0) {
        (void)fprintf(stderr, "resource actions: compositor mismatch count=%zu path=%s cwd=%s\n",
                      repl->resource_count, action.path, action.launch_directory);
        return 1;
    }
    gsh_async_repl_close(repl);
    return 0;
}

static int structured_resource_case(gsh_async_repl *repl)
{
    static const char row[] = "name with space\n";
    static const char label[] = "name with space";
    static const char path[] = "./name with space";
    gsh_async_resource_action action;
    int cell;

    if (repl == NULL) return 1;
    gsh_async_repl_initialize(repl, true);
    gsh_async_repl_configure_actions(repl, true, GSH_PATH_DETECTION_SAFE);
    gsh_async_repl_resize(repl, 24U, 100U);
    cell = gsh_async_repl_accept(repl, "$ ", "ls -1", 5U, "/tmp",
                                 false, false, false, false);
    if (cell < 0) return 1;
    gsh_async_repl_starting(repl, cell);
    if (gsh_async_repl_append(repl, cell, row, sizeof(row) - 1U) == -1 ||
        gsh_async_repl_add_native_resource(
            repl, cell, 0U, 0U, sizeof(label) - 1U,
            0U, sizeof(label) - 1U, label,
            sizeof(label) - 1U, path, sizeof(path) - 1U,
            GSH_RESOURCE_REGULAR, true) == -1 ||
        gsh_async_repl_prepare_render(repl, "$ ", "", 0U, 0U) == -1 ||
        gsh_async_repl_resource_at(repl, 2U, 2U, &action) == -1 ||
        strcmp(action.path, path) != 0 ||
        action.type != GSH_RESOURCE_REGULAR || !action.navigable_root ||
        repl->resource_count != 1U) {
        (void)fputs("resource actions: structured record mismatch\n", stderr);
        return 1;
    }
    gsh_async_repl_close(repl);
    return 0;
}

static int unicode_hitbox_case(gsh_async_repl *repl)
{
    static const char row[] = "\xce\xb1.py\n";
    static const char label[] = "\xce\xb1.py";
    static const char path[] = "/tmp/\xce\xb1.py";
    gsh_async_resource_action action;
    int cell;

    if (repl == NULL) return 1;
    gsh_async_repl_initialize(repl, true);
    gsh_async_repl_configure_actions(repl, true, GSH_PATH_DETECTION_SAFE);
    gsh_async_repl_resize(repl, 24U, 100U);
    cell = gsh_async_repl_accept(repl, "$ ", "ls -1", 5U, "/tmp",
                                 false, false, false, false);
    if (cell < 0) return 1;
    gsh_async_repl_starting(repl, cell);
    if (gsh_async_repl_append(repl, cell, row, sizeof(row) - 1U) == -1 ||
        gsh_async_repl_add_native_resource(
            repl, cell, 0U, 0U, sizeof(label) - 1U, 0U, 4U,
            label, sizeof(label) - 1U, path, sizeof(path) - 1U,
            GSH_RESOURCE_REGULAR, true) == -1 ||
        gsh_async_repl_prepare_render(repl, "$ ", "", 0U, 0U) == -1 ||
        gsh_async_repl_resource_at(repl, 2U, 4U, &action) == -1 ||
        gsh_async_repl_resource_at(repl, 2U, 5U, &action) != -1) {
        (void)fputs("resource actions: Unicode hitbox mismatch\n", stderr);
        return 1;
    }
    gsh_async_repl_close(repl);
    return 0;
}

static int hostile_terminal_case(gsh_async_repl *repl)
{
    static const char first[] = "\033P./hidden";
    static const char second[] = "\033\\\033]8;;file:///tmp/hidden\007link";
    static const char third[] = "\033]8;;\007\nshown\n";
    static const char c1[] = {
        (char)0x90, '.', '/', 'a', 'l', 's', 'o', (char)0x9c, '\n'};
    int cell;

    if (repl == NULL) return 1;
    gsh_async_repl_initialize(repl, true);
    gsh_async_repl_configure_actions(repl, true, GSH_PATH_DETECTION_SAFE);
    cell = gsh_async_repl_accept(repl, "$ ", "producer", 8U, "/tmp",
                                 false, false, false, false);
    if (cell < 0) return 1;
    gsh_async_repl_starting(repl, cell);
    if (gsh_async_repl_append(repl, cell, first, sizeof(first) - 1U) == -1 ||
        gsh_async_repl_append(repl, cell, second, sizeof(second) - 1U) == -1 ||
        gsh_async_repl_append(repl, cell, third, sizeof(third) - 1U) == -1 ||
        gsh_async_repl_append(repl, cell, c1, sizeof(c1)) == -1 ||
        gsh_async_repl_prepare_render(repl, "$ ", "", 0U, 0U) == -1 ||
        repl->resource_count != 0U ||
        strstr(gsh_async_repl_render_data(repl), "./hidden") != NULL ||
        strstr(gsh_async_repl_render_data(repl), "./also") != NULL ||
        strstr(gsh_async_repl_render_data(repl), "shown") == NULL) {
        (void)fputs("resource actions: hostile terminal sequence leaked\n",
                    stderr);
        return 1;
    }
    gsh_async_repl_close(repl);
    return 0;
}

static int compositor_scroll_case(gsh_async_repl *repl)
{
    static const char rows[] = "one\ntwo\nthree\nfour\nfive\n";
    const char *render;
    int cell;

    if (repl == NULL || GSH_ASYNC_VIEW_ROWS != 10240) return 1;
    gsh_async_repl_initialize(repl, true);
    gsh_async_repl_configure_actions(repl, false,
                                     GSH_PATH_DETECTION_OFF);
    gsh_async_repl_resize(repl, 3U, 80U);
    cell = gsh_async_repl_accept(repl, "$ ", "list", 4U, "/tmp",
                                 false, false, false, false);
    if (cell < 0) return 1;
    gsh_async_repl_starting(repl, cell);
    if (gsh_async_repl_append(repl, cell, rows, sizeof(rows) - 1U) == -1 ||
        gsh_async_repl_prepare_render(repl, "$ ", "", 0U, 0U) == -1)
        return 1;
    render = gsh_async_repl_render_data(repl);
    if (strstr(render, "five") == NULL || strstr(render, "one") != NULL ||
        strstr(render, "\033[?1000h") == NULL) return 1;
    gsh_async_repl_scroll(repl, 2L);
    if (gsh_async_repl_prepare_render(repl, "$ ", "", 0U, 0U) == -1)
        return 1;
    render = gsh_async_repl_render_data(repl);
    if (strstr(render, "two") == NULL || strstr(render, "five") != NULL)
        return 1;
    gsh_async_repl_scroll(repl, -2L);
    if (gsh_async_repl_prepare_render(repl, "$ ", "", 0U, 0U) == -1 ||
        strstr(gsh_async_repl_render_data(repl), "five") == NULL) return 1;
    gsh_async_repl_close(repl);
    return 0;
}

static int multiline_editor_case(gsh_async_repl *repl)
{
    static const char editor[] = "ab\ncd";
    const char *render;

    if (repl == NULL) return 1;
    gsh_async_repl_initialize(repl, true);
    gsh_async_repl_resize(repl, 6U, 8U);
    if (gsh_async_repl_prepare_render(repl, "$ ", editor,
                                      sizeof(editor) - 1U, 4U) == -1) {
        return 1;
    }
    render = gsh_async_repl_render_data(repl);
    if (strstr(render, "$ ab\ncd") == NULL ||
        strstr(render, "\033[2;2H") == NULL ||
        strstr(render, "\033[?2004h") == NULL ||
        gsh_async_repl_prepare_render(repl, "$ ", editor,
                                      sizeof(editor) - 1U,
                                      sizeof(editor)) != -1) {
        (void)fputs("resource actions: multiline editor mismatch\n", stderr);
        return 1;
    }
    gsh_async_repl_close(repl);
    return 0;
}

int main(void)
{
    static gsh_async_repl repl;
    int failed;
    failed = detector_cases() != 0 || adapter_cases() != 0 ||
        compositor_cases(&repl) != 0 ||
        structured_resource_case(&repl) != 0 ||
        unicode_hitbox_case(&repl) != 0 || hostile_terminal_case(&repl) != 0;
    if (!failed) failed = compositor_scroll_case(&repl) != 0;
    if (!failed) failed = multiline_editor_case(&repl) != 0;
    if (failed) {
        (void)fputs("resource actions: failed\n", stderr);
        return 1;
    }
    (void)puts("resource actions: detector and compositor cases passed");
    return 0;
}
