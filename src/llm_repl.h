#ifndef GSH_LLM_REPL_H
#define GSH_LLM_REPL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    GSH_LLM_REPL_VERSION = 2,
    GSH_LLM_REPL_SCRIPT_CAP = 4096,
    GSH_LLM_REPL_OUTPUT_CAP = 65536,
    GSH_LLM_REPL_DIRECTORY_CAP = 4096,
};

typedef enum {
    GSH_LLM_IDLE = 0,
    GSH_LLM_STARTING,
    GSH_LLM_GENERATING,
    GSH_LLM_CONFIRMATION,
} gsh_llm_activity;

typedef struct {
    uint32_t version;
    uint32_t length;
    uint32_t activity;
    char script[GSH_LLM_REPL_SCRIPT_CAP];
} gsh_llm_repl_request;

typedef struct {
    uint32_t version;
    int32_t status;
    uint64_t cell_id;
    char directory[GSH_LLM_REPL_DIRECTORY_CAP];
    char output[GSH_LLM_REPL_OUTPUT_CAP];
} gsh_llm_repl_result;

typedef struct {
    int fd;
    size_t received;
    size_t sent;
    bool replying;
    gsh_llm_repl_request request;
    gsh_llm_repl_result result;
} gsh_llm_repl_channel;

int gsh_llm_repl_open(gsh_llm_repl_channel *channel, int *peer);
void gsh_llm_repl_close(gsh_llm_repl_channel *channel);
int gsh_llm_repl_receive(gsh_llm_repl_channel *channel);
int gsh_llm_repl_flush(gsh_llm_repl_channel *channel);
int gsh_llm_repl_call(int descriptor, const char *script,
                      gsh_llm_repl_result *result);
int gsh_llm_repl_report_activity(int descriptor, gsh_llm_activity activity);

#endif
