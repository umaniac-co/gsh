#ifndef GSH_LLM_JOURNAL_H
#define GSH_LLM_JOURNAL_H

#include <stddef.h>

enum {
    GSH_LLM_JOURNAL_COMMAND = 1,
    GSH_LLM_JOURNAL_COMMAND_OUTPUT,
    GSH_LLM_JOURNAL_PROMPT,
    GSH_LLM_JOURNAL_ANSWER,
    GSH_LLM_JOURNAL_TOOL,
};

typedef struct {
    int descriptor;
    size_t used;
    size_t sent;
    char pending[262144];
} gsh_llm_journal_queue;

int gsh_llm_journal_enqueue(gsh_llm_journal_queue *queue, const char *home,
                            unsigned int kind, const char *text, size_t length);
int gsh_llm_journal_flush(gsh_llm_journal_queue *queue);
int gsh_llm_journal_worker(int descriptor);
int gsh_llm_journal_append(const char *home, unsigned int kind,
                           const char *text, size_t length);
int gsh_llm_journal_context(const char *home, size_t exchanges,
                            char *output, size_t capacity);
int gsh_llm_journal_search(const char *home, const char *query, size_t limit,
                           char *output, size_t capacity);

#endif
