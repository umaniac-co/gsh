#ifndef GSH_HISTORY_CLIENT_H
#define GSH_HISTORY_CLIENT_H

#include "history_store.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { GSH_HISTORY_SECRET_CAP = 1024 };

typedef struct {
    int descriptor;
    bool connected;
    char socket_path[4096];
    char vault_path[4096];
    char agent_path[4096];
    unsigned char *snapshot;
    size_t snapshot_capacity;
} gsh_history_client;

int gsh_history_client_initialize(gsh_history_client *client,
                                  const char *home,
                                  const char *program_path,
                                  unsigned char *snapshot,
                                  size_t snapshot_capacity);
int gsh_history_client_status(gsh_history_client *client,
                              uint64_t reminder_min_ns,
                              uint64_t reminder_max_ns,
                              gsh_history_store *store, int *status,
                              uint64_t *reminder_ns);
int gsh_history_client_unlock(gsh_history_client *client,
                              const char *passphrase, size_t length,
                              uint64_t reminder_min_ns,
                              uint64_t reminder_max_ns, bool reset,
                              gsh_history_store *store,
                              uint64_t *reminder_ns);
int gsh_history_client_verify(gsh_history_client *client,
                              const char *passphrase, size_t length,
                              uint64_t reminder_min_ns,
                              uint64_t reminder_max_ns,
                              uint64_t *reminder_ns);
int gsh_history_client_add(gsh_history_client *client,
                           const char *command, size_t length);
int gsh_history_client_control(gsh_history_client *client, bool shutdown);
void gsh_history_client_close(gsh_history_client *client);

#endif
