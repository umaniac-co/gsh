#ifndef GSH_SHELL_CONFIG_H
#define GSH_SHELL_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

enum {
    GSH_CONFIG_DIAGNOSTIC_CAP = 256,
    GSH_CONFIG_EDITOR_ARG_CAP = 16,
    GSH_CONFIG_EDITOR_STORAGE_CAP = 4096,
};

typedef enum {
    GSH_TERMINAL_ACTIONS_AUTO = 0,
    GSH_TERMINAL_ACTIONS_ON,
    GSH_TERMINAL_ACTIONS_OFF,
} gsh_terminal_actions_mode;

typedef enum {
    GSH_TERMINAL_IMAGES_AUTO = 0,
    GSH_TERMINAL_IMAGES_ON,
    GSH_TERMINAL_IMAGES_OFF,
} gsh_terminal_images_mode;

typedef enum {
    GSH_PATH_DETECTION_OFF = 0,
    GSH_PATH_DETECTION_KNOWN,
    GSH_PATH_DETECTION_SAFE,
} gsh_path_detection_mode;

typedef struct {
    bool async_repl_enabled;
    bool completion_enabled;
    bool history_enabled;
    bool history_deduplicate;
    bool history_store_failed;
    bool history_ignore_space;
    size_t history_max_entries;
    gsh_terminal_actions_mode terminal_actions;
    gsh_terminal_images_mode terminal_images;
    gsh_path_detection_mode path_detection;
    bool preview_editor_auto;
    size_t preview_editor_argc;
    size_t preview_editor_offsets[GSH_CONFIG_EDITOR_ARG_CAP];
    char preview_editor_storage[GSH_CONFIG_EDITOR_STORAGE_CAP];
    char path[4096];
    char diagnostic[GSH_CONFIG_DIAGNOSTIC_CAP];
} gsh_shell_config;

void gsh_config_defaults(gsh_shell_config *config);
int gsh_config_load(gsh_shell_config *config, const char *home,
                    bool create_missing);
const char *gsh_config_editor_argument(const gsh_shell_config *config,
                                       size_t index);

#endif
