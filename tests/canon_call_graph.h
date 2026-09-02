#ifndef GSH_CANON_CALL_GRAPH_H
#define GSH_CANON_CALL_GRAPH_H

#include <stdbool.h>
#include <stddef.h>

enum {
    GSH_CANON_FILE_CAP = 256,
    GSH_CANON_FUNCTION_CAP = 2048,
    GSH_CANON_CALL_CAP = 65536,
    GSH_CANON_PARAMETER_CAP = 32,
    GSH_CANON_NAME_CAP = 96,
    GSH_CANON_PATH_CAP = 4096,
};

typedef struct {
    char name[GSH_CANON_NAME_CAP];
    char return_type[GSH_CANON_NAME_CAP];
    char unvalidated_parameter_names[GSH_CANON_PARAMETER_CAP]
                                    [GSH_CANON_NAME_CAP];
    size_t body_offset;
    unsigned short file;
    unsigned short behavior_count;
    unsigned short assertion_count;
    unsigned short unvalidated_parameters;
    bool internal;
    bool root;
    bool returns_void;
    bool returns_pointer;
} gsh_canon_function;

typedef struct {
    char name[GSH_CANON_NAME_CAP];
    size_t line;
    unsigned short caller;
    bool bare_statement;
} gsh_canon_call;

typedef struct {
    char files[GSH_CANON_FILE_CAP][GSH_CANON_PATH_CAP];
    gsh_canon_function functions[GSH_CANON_FUNCTION_CAP];
    gsh_canon_call calls[GSH_CANON_CALL_CAP];
    size_t file_count;
    size_t function_count;
    size_t call_count;
    size_t static_assertion_count;
} gsh_canon_call_graph;

void gsh_canon_call_graph_initialize(gsh_canon_call_graph *graph);

int gsh_canon_call_graph_add(gsh_canon_call_graph *graph,
                             const unsigned char *source, size_t length,
                             const char *path);

int gsh_canon_call_graph_analyze(const gsh_canon_call_graph *graph,
                                 size_t *unreachable_functions,
                                 size_t *direct_recursive_calls,
                                 size_t *recursive_functions,
                                 size_t *oversized_functions,
                                 size_t *assertion_deficit,
                                 size_t *unchecked_returns,
                                 size_t *unvalidated_parameters,
                                 bool verbose);

#endif
