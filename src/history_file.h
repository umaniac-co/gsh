#ifndef GSH_HISTORY_FILE_H
#define GSH_HISTORY_FILE_H

#include "history_store.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char path[4096];
    char lock_path[4096];
    bool ready;
} gsh_history_file;

int gsh_history_file_initialize(gsh_history_file *file, const char *home,
                                gsh_history_store *history);
int gsh_history_file_save(gsh_history_file *file,
                          gsh_history_store *persisted,
                          const gsh_history_store *session,
                          size_t max_entries, bool deduplicate);
void gsh_history_file_close(gsh_history_file *file);

#endif
