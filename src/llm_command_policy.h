#ifndef GSH_LLM_COMMAND_POLICY_H
#define GSH_LLM_COMMAND_POLICY_H

#include "posix_parser.h"

enum { GSH_LLM_POLICY_SCRIPT_CAP = 4096, GSH_LLM_POLICY_NESTING_CAP = 16 };

typedef struct {
    gsh_parse_storage parse;
    char scripts[GSH_LLM_POLICY_NESTING_CAP][GSH_LLM_POLICY_SCRIPT_CAP];
    size_t count;
} gsh_llm_command_policy;

bool gsh_llm_command_removes(const char *script,
                             gsh_llm_command_policy *policy);

#endif
