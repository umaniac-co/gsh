#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "../src/llm_json.h"

#include <stdio.h>
#include <string.h>

static int writer_case(void)
{
    char storage[128];
    gsh_json_writer writer;

    gsh_json_writer_initialize(&writer, storage, sizeof(storage));
    if (!gsh_json_write_literal(&writer, "{\"value\":")) return 1;
    if (!gsh_json_write_string(&writer, "a\n\"b", 4U)) return 1;
    if (!gsh_json_write_literal(&writer, "}")) return 1;
    return strcmp(storage, "{\"value\":\"a\\n\\\"b\"}") != 0;
}

static int reader_case(void)
{
    static const char json[] =
        "{\"type\":\"response.output_text.delta\",\"delta\":\"a\\n"
        "b\",\"item\":{\"type\":\"message\"}}";
    char value[64];
    const char *object;
    size_t object_length;

    if (!gsh_json_get_string(json, sizeof(json) - 1U, "delta", value,
                             sizeof(value)) || strcmp(value, "a\nb") != 0)
        return 1;
    if (!gsh_json_get_object(json, sizeof(json) - 1U, "item", &object,
                             &object_length) || object_length != 18U ||
        memcmp(object, "{\"type\":\"message\"}", object_length) != 0)
        return 1;
    return 0;
}

static int unicode_case(void)
{
    static const char json[] =
        "{\"delta\":\"caff\\u00e8 \\ud83d\\ude80\"}";
    static const char invalid[] = "{\"delta\":\"\\ud83dX\"}";
    char value[64];

    if (!gsh_json_get_string(json, sizeof(json) - 1U, "delta", value,
                             sizeof(value)) ||
        strcmp(value, "caff\xc3\xa8 \xf0\x9f\x9a\x80") != 0)
        return 1;
    return gsh_json_get_string(invalid, sizeof(invalid) - 1U, "delta",
                               value, sizeof(value)) ? 1 : 0;
}

int main(void)
{
    if (writer_case() != 0 || reader_case() != 0 || unicode_case() != 0) {
        (void)fputs("llm json: failed\n", stderr);
        return 1;
    }
    (void)puts("llm json: passed");
    return 0;
}
