#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "llm_json.h"

#include <string.h>

enum { GSH_JSON_SCAN_CAP = 1048576 };

void gsh_json_writer_initialize(gsh_json_writer *writer, char *data,
                                size_t capacity)
{
    if (writer == NULL) return;
    if (data == NULL || capacity == 0U) {
        writer->data = NULL;
        writer->capacity = 0U;
        writer->length = 0U;
        writer->failed = true;
        return;
    }
    writer->data = data;
    writer->capacity = capacity;
    writer->length = 0U;
    writer->failed = false;
    data[0] = '\0';
}

bool gsh_json_write_bytes(gsh_json_writer *writer, const char *text,
                          size_t length)
{
    if (writer == NULL || text == NULL || writer->failed ||
        length >= writer->capacity - writer->length) {
        if (writer != NULL) writer->failed = true;
        return false;
    }
    (void)memcpy(writer->data + writer->length, text, length);
    writer->length += length;
    writer->data[writer->length] = '\0';
    return true;
}

bool gsh_json_write_literal(gsh_json_writer *writer, const char *text)
{
    return text != NULL && gsh_json_write_bytes(writer, text, strlen(text));
}

static bool write_escape(gsh_json_writer *writer, unsigned char byte)
{
    static const char hexadecimal[] = "0123456789abcdef";
    char escape[6] = {'\\', 'u', '0', '0', '0', '0'};

    if (byte == '"' || byte == '\\') {
        escape[1] = (char)byte;
        return gsh_json_write_bytes(writer, escape, 2U);
    }
    if (byte == '\n') return gsh_json_write_literal(writer, "\\n");
    if (byte == '\r') return gsh_json_write_literal(writer, "\\r");
    if (byte == '\t') return gsh_json_write_literal(writer, "\\t");
    escape[4] = hexadecimal[(byte >> 4U) & 15U];
    escape[5] = hexadecimal[byte & 15U];
    return gsh_json_write_bytes(writer, escape, sizeof(escape));
}

bool gsh_json_write_string(gsh_json_writer *writer, const char *text,
                           size_t length)
{
    size_t index;

    if (writer == NULL || text == NULL ||
        !gsh_json_write_literal(writer, "\"")) return false;
    for (index = 0U; index < length && index < GSH_JSON_SCAN_CAP; index++) {
        unsigned char byte = (unsigned char)text[index];

        if (byte < 0x20U || byte == '"' || byte == '\\') {
            if (!write_escape(writer, byte)) return false;
        } else if (!gsh_json_write_bytes(writer, text + index, 1U)) {
            return false;
        }
    }
    if (index != length) { writer->failed = true; return false; }
    return gsh_json_write_literal(writer, "\"");
}

static int hexadecimal_value(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') return (int)(byte - '0');
    if (byte >= 'a' && byte <= 'f') return (int)(byte - 'a') + 10;
    if (byte >= 'A' && byte <= 'F') return (int)(byte - 'A') + 10;
    return -1;
}

static bool decode_hex_quad(const char *json, size_t length, size_t *index,
                            unsigned int *value)
{
    size_t digit;

    if (json == NULL || index == NULL || value == NULL ||
        *index + 4U > length) return false;
    *value = 0U;
    for (digit = 0U; digit < 4U; digit++) {
        int hexadecimal = hexadecimal_value(
            (unsigned char)json[*index + digit]);
        if (hexadecimal < 0) return false;
        *value = *value * 16U + (unsigned int)hexadecimal;
    }
    *index += 4U;
    return true;
}

static bool write_utf8(unsigned int value, char *output, size_t capacity,
                       size_t *used)
{
    size_t bytes;

    if (output == NULL || used == NULL || value > 0x10ffffU ||
        (value >= 0xd800U && value <= 0xdfffU)) return false;
    bytes = value <= 0x7fU ? 1U : value <= 0x7ffU ? 2U :
            value <= 0xffffU ? 3U : 4U;
    if (*used + bytes >= capacity) return false;
    if (bytes == 1U) output[(*used)++] = (char)value;
    else if (bytes == 2U) {
        output[(*used)++] = (char)(0xc0U | (value >> 6U));
        output[(*used)++] = (char)(0x80U | (value & 0x3fU));
    } else if (bytes == 3U) {
        output[(*used)++] = (char)(0xe0U | (value >> 12U));
        output[(*used)++] = (char)(0x80U | ((value >> 6U) & 0x3fU));
        output[(*used)++] = (char)(0x80U | (value & 0x3fU));
    } else {
        output[(*used)++] = (char)(0xf0U | (value >> 18U));
        output[(*used)++] = (char)(0x80U | ((value >> 12U) & 0x3fU));
        output[(*used)++] = (char)(0x80U | ((value >> 6U) & 0x3fU));
        output[(*used)++] = (char)(0x80U | (value & 0x3fU));
    }
    return true;
}

static bool decode_unicode_escape(const char *json, size_t length,
                                  size_t *index, char *output,
                                  size_t capacity, size_t *used)
{
    unsigned int value;
    unsigned int low;

    if (!decode_hex_quad(json, length, index, &value)) return false;
    if (value >= 0xd800U && value <= 0xdbffU) {
        if (*index + 6U > length || json[*index] != '\\' ||
            json[*index + 1U] != 'u') return false;
        *index += 2U;
        if (!decode_hex_quad(json, length, index, &low) || low < 0xdc00U ||
            low > 0xdfffU) return false;
        value = 0x10000U + ((value - 0xd800U) << 10U) + (low - 0xdc00U);
    }
    return write_utf8(value, output, capacity, used);
}

static bool decode_escape(const char *json, size_t length, size_t *index,
                          char *output, size_t capacity, size_t *used)
{
    char byte;

    if (json == NULL || index == NULL || *index >= length || output == NULL ||
        used == NULL) return false;
    byte = json[(*index)++];
    if (byte == 'u')
        return decode_unicode_escape(json, length, index, output, capacity,
                                     used);
    if (*used + 1U >= capacity) return false;
    if (byte == 'n') output[(*used)++] = '\n';
    else if (byte == 'r') output[(*used)++] = '\r';
    else if (byte == 't') output[(*used)++] = '\t';
    else if (byte == 'b') output[(*used)++] = '\b';
    else if (byte == 'f') output[(*used)++] = '\f';
    else if (byte == '"' || byte == '\\' || byte == '/')
        output[(*used)++] = byte;
    else return false;
    return true;
}

static bool key_matches(const char *json, size_t begin, size_t end,
                        const char *key)
{
    size_t key_length;

    if (json == NULL || key == NULL || begin > end) return false;
    key_length = strlen(key);
    return end - begin == key_length &&
           memcmp(json + begin, key, key_length) == 0;
}

static bool locate_value(const char *json, size_t length, const char *key,
                         size_t *value)
{
    size_t index;

    if (json == NULL || key == NULL || value == NULL ||
        length > GSH_JSON_SCAN_CAP) return false;
    for (index = 0U; index < length; index++) {
        size_t begin;
        bool escaped = false;

        if (json[index] != '"') continue;
        begin = ++index;
        while (index < length && (escaped || json[index] != '"')) {
            escaped = !escaped && json[index] == '\\';
            if (json[index] != '\\') escaped = false;
            index++;
        }
        if (index >= length || !key_matches(json, begin, index, key)) continue;
        index++;
        while (index < length && (json[index] == ' ' || json[index] == '\t' ||
                                  json[index] == '\r' || json[index] == '\n'))
            index++;
        if (index >= length || json[index++] != ':') continue;
        while (index < length && (json[index] == ' ' || json[index] == '\t' ||
                                  json[index] == '\r' || json[index] == '\n'))
            index++;
        if (index < length) { *value = index; return true; }
    }
    return false;
}

bool gsh_json_get_string(const char *json, size_t length, const char *key,
                         char *output, size_t capacity)
{
    size_t index;
    size_t used = 0U;

    if (output == NULL || capacity == 0U ||
        !locate_value(json, length, key, &index) || json[index++] != '"')
        return false;
    while (index < length && json[index] != '"') {
        char byte = json[index++];

        if (byte == '\\') {
            if (!decode_escape(json, length, &index, output, capacity, &used))
                return false;
        } else {
            if ((unsigned char)byte < 0x20U || used + 1U >= capacity)
                return false;
            output[used++] = byte;
        }
    }
    if (index >= length || json[index] != '"') return false;
    output[used] = '\0';
    return true;
}

static bool scan_composite(const char *json, size_t length, size_t begin,
                           size_t *end)
{
    char open;
    char close;
    size_t depth = 0U;
    bool string = false;
    bool escaped = false;
    size_t index;

    if (json == NULL || end == NULL || begin >= length) return false;
    open = json[begin];
    close = open == '{' ? '}' : ']';
    if (open != '{' && open != '[') return false;
    for (index = begin; index < length && index - begin < GSH_JSON_SCAN_CAP;
         index++) {
        char byte = json[index];

        if (string) {
            if (!escaped && byte == '"') string = false;
            escaped = !escaped && byte == '\\';
            if (byte != '\\') escaped = false;
        } else if (byte == '"') string = true;
        else if (byte == open) depth++;
        else if (byte == close && --depth == 0U) {
            *end = index + 1U;
            return true;
        }
    }
    return false;
}

bool gsh_json_get_object(const char *json, size_t length, const char *key,
                         const char **object, size_t *object_length)
{
    size_t begin;
    size_t end;

    if (object == NULL || object_length == NULL ||
        !locate_value(json, length, key, &begin) ||
        !scan_composite(json, length, begin, &end)) return false;
    *object = json + begin;
    *object_length = end - begin;
    return true;
}
