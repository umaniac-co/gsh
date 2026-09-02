#ifndef GSH_RESOURCE_ACTIONS_H
#define GSH_RESOURCE_ACTIONS_H

#include "shell_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    GSH_RESOURCE_NATIVE = 1,
    GSH_RESOURCE_ADAPTER,
    GSH_RESOURCE_DETECTED,
} gsh_resource_provenance;

typedef enum {
    GSH_RESOURCE_UNKNOWN = 0,
    GSH_RESOURCE_REGULAR,
    GSH_RESOURCE_DIRECTORY,
    GSH_RESOURCE_SYMLINK,
} gsh_resource_type;

typedef struct {
    size_t begin;
    size_t end;
    size_t column_begin;
    size_t column_end;
    char path[4096];
    size_t line;
    size_t column;
    gsh_resource_provenance provenance;
    gsh_resource_type type;
    bool navigable_root;
} gsh_resource_candidate;

size_t gsh_resource_detect(const char *command, const char *launch_directory,
                           const char *text, size_t length,
                           gsh_path_detection_mode mode,
                           gsh_resource_candidate *candidates,
                           size_t capacity);

#endif
