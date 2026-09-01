#ifndef GSH_BUILTIN_COMMAND_H
#define GSH_BUILTIN_COMMAND_H

#include "builtin_common.h"
#include "command_cache.h"
#include "shell_aliases.h"
#include "shell_functions.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    GSH_COMMAND_FORM_EXECUTE = 0,
    GSH_COMMAND_FORM_INSPECT,
    GSH_COMMAND_FORM_EMPTY,
    GSH_COMMAND_FORM_ERROR,
} gsh_command_form;

typedef struct {
    gsh_command_form form;
    size_t first_operand;
    bool use_default_path;
    bool verbose;
} gsh_command_invocation;

gsh_command_invocation gsh_command_parse(size_t argc,
                                         char *const argv[]);
bool gsh_command_is_inspection_builtin(size_t argc,
                                       char *const argv[]);
bool gsh_command_special_builtin_name(const char *name, size_t length);
bool gsh_command_intrinsic_name(const char *name, size_t length);

int gsh_builtin_command_inspect(
    size_t argc, char *const argv[], const char *path,
    const char *default_path, const gsh_alias_store *aliases,
    const gsh_function_store *functions, gsh_command_cache *cache,
    uint64_t path_generation, bool cacheable, bool *cache_changed,
    const gsh_builtin_io *io);
int gsh_builtin_type(size_t argc, char *const argv[], const char *path,
                     const gsh_alias_store *aliases,
                     const gsh_function_store *functions,
                     gsh_command_cache *cache, uint64_t path_generation,
                     bool *cache_changed,
                     const gsh_builtin_io *io);
int gsh_builtin_hash(size_t argc, char *const argv[], const char *path,
                     const gsh_function_store *functions,
                     gsh_command_cache *cache, uint64_t path_generation,
                     bool *cache_changed, const gsh_builtin_io *io);

#endif
