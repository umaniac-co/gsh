#ifndef GSH_GIT_LISTING_H
#define GSH_GIT_LISTING_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

enum {
    GSH_GIT_BRANCH_CAP = 256,
    GSH_GIT_OID_CAP = 65,
    GSH_GIT_READER_CAP = 8192,
};

typedef struct {
    int descriptor;
    unsigned char bytes[GSH_GIT_READER_CAP];
    size_t begin;
    size_t end;
    bool eof;
} gsh_git_reader;

typedef struct {
    gsh_git_reader status;
    gsh_git_reader tracked;
    gsh_git_reader sizes;
    int size_input;
    size_t size_requests;
    char root[PATH_MAX];
    char prefix[PATH_MAX];
    char branch[GSH_GIT_BRANCH_CAP];
    bool active;
} gsh_git_snapshot;

typedef struct {
    char path[PATH_MAX];
    char original[PATH_MAX];
    char index_status;
    char worktree_status;
    bool renamed;
} gsh_git_change;

typedef struct {
    char path[PATH_MAX];
    char object_id[GSH_GIT_OID_CAP];
    mode_t mode;
} gsh_git_tracked;

int gsh_git_snapshot_open(gsh_git_snapshot *snapshot,
                          const char *directory);
void gsh_git_snapshot_close(gsh_git_snapshot *snapshot);
int gsh_git_snapshot_next_change(gsh_git_snapshot *snapshot,
                                 gsh_git_change *change);
int gsh_git_snapshot_next_tracked(gsh_git_snapshot *snapshot,
                                  gsh_git_tracked *tracked);
int gsh_git_snapshot_request_size(gsh_git_snapshot *snapshot,
                                  const char *object_id);
int gsh_git_snapshot_begin_sizes(gsh_git_snapshot *snapshot);
int gsh_git_snapshot_next_size(gsh_git_snapshot *snapshot, off_t *size);

#endif
