#ifndef GSH_LLM_JSON_H
#define GSH_LLM_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool failed;
} gsh_json_writer;

void gsh_json_writer_initialize(gsh_json_writer *writer, char *data,
                                size_t capacity);
bool gsh_json_write_literal(gsh_json_writer *writer, const char *text);
bool gsh_json_write_bytes(gsh_json_writer *writer, const char *text,
                          size_t length);
bool gsh_json_write_string(gsh_json_writer *writer, const char *text,
                           size_t length);
bool gsh_json_get_string(const char *json, size_t length, const char *key,
                         char *output, size_t capacity);
bool gsh_json_get_object(const char *json, size_t length, const char *key,
                         const char **object, size_t *object_length);

#endif
