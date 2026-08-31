#ifndef GSH_SHELL_CONFIG_H
#define GSH_SHELL_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { GSH_CONFIG_DIAGNOSTIC_CAP = 256 };

typedef struct {
    bool async_repl_enabled;
    bool history_enabled;
    bool history_deduplicate;
    bool history_store_failed;
    bool history_ignore_space;
    size_t history_max_entries;
    bool history_unlock_infinite;
    uint64_t history_reminder_min_ns;
    uint64_t history_reminder_max_ns;
    char path[4096];
    char diagnostic[GSH_CONFIG_DIAGNOSTIC_CAP];
} gsh_shell_config;

void gsh_config_defaults(gsh_shell_config *config);
int gsh_config_load(gsh_shell_config *config, const char *home,
                    bool create_missing);

#endif
