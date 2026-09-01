#ifndef GSH_BUILTIN_REGISTRY_H
#define GSH_BUILTIN_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    GSH_BUILTIN_NONE = 0,
    GSH_BUILTIN_COLON,
    GSH_BUILTIN_TRUE,
    GSH_BUILTIN_FALSE,
    GSH_BUILTIN_ECHO,
    GSH_BUILTIN_PRINTF,
    GSH_BUILTIN_TEST,
    GSH_BUILTIN_BRACKET,
    GSH_BUILTIN_READ,
    GSH_BUILTIN_GETOPTS,
    GSH_BUILTIN_FC,
    GSH_BUILTIN_JOBS,
    GSH_BUILTIN_KILL,
    GSH_BUILTIN_OTHER,
} gsh_builtin_kind;

typedef enum {
    GSH_BUILTIN_PURE = 0,
    GSH_BUILTIN_STATEFUL,
    GSH_BUILTIN_BLOCKING,
    GSH_BUILTIN_JOB_CONTROL,
} gsh_builtin_class;

typedef struct {
    const char *name;
    size_t name_length;
    gsh_builtin_kind kind;
    gsh_builtin_class execution_class;
    bool special;
    bool implemented;
} gsh_builtin_descriptor;

const gsh_builtin_descriptor *gsh_builtin_lookup(const char *name,
                                                 size_t length);
size_t gsh_builtin_descriptor_count(void);
const gsh_builtin_descriptor *gsh_builtin_descriptor_at(size_t index);
bool gsh_builtin_regular_name(const char *name, size_t length);
bool gsh_builtin_pure_name(const char *name, size_t length);

#endif
