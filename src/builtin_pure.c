#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_pure.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h> /* CANON-INCLUDE: linux */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wchar.h>

enum {
    GSH_ESCAPE_BYTE_CAP = 8,
    GSH_PRINTF_FIELD_CAP = 4096,
    GSH_PRINTF_SPEC_CAP = 64,
    GSH_TEST_ARGUMENT_CAP = 128,
};

typedef struct {
    unsigned char bytes[GSH_ESCAPE_BYTE_CAP];
    size_t length;
    size_t consumed;
    bool stop;
} escape_result;

typedef struct {
    char flags[6];
    size_t flag_count;
    int width;
    int precision;
    bool width_set;
    bool precision_set;
    char conversion;
    size_t consumed;
} printf_conversion;

typedef enum {
    TEST_OPERATOR_LEFT,
    TEST_OPERATOR_NOT,
    TEST_OPERATOR_AND,
    TEST_OPERATOR_OR,
} test_operator;

static int emit_bytes(const gsh_builtin_io *io, int descriptor,
                      const void *bytes, size_t length)
{
    if (!gsh_builtin_io_valid(io) ||
        (bytes == NULL && length != 0)) {
        errno = EINVAL;
        return 1;
    }
    return gsh_builtin_output(io, descriptor, bytes, length) == 0 ? 0 : 1;
}

static int hex_digit(unsigned char byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
        return byte - 'A' + 10;
    }
    return -1;
}

static size_t encode_utf8(uint32_t value,
                          unsigned char output[GSH_ESCAPE_BYTE_CAP])
{
    if (output == NULL) {
        return 0U;
    }
    if (value <= 0x7fU) {
        output[0] = (unsigned char)value;
        return 1;
    }
    if (value <= 0x7ffU) {
        output[0] = (unsigned char)(0xc0U | (value >> 6));
        output[1] = (unsigned char)(0x80U | (value & 0x3fU));
        return 2;
    }
    if (value >= 0xd800U && value <= 0xdfffU) {
        value = 0xfffdU;
    }
    if (value <= 0xffffU) {
        output[0] = (unsigned char)(0xe0U | (value >> 12));
        output[1] = (unsigned char)(0x80U | ((value >> 6) & 0x3fU));
        output[2] = (unsigned char)(0x80U | (value & 0x3fU));
        return 3;
    }
    if (value > 0x10ffffU) {
        value = 0xfffdU;
    }
    output[0] = (unsigned char)(0xf0U | (value >> 18));
    output[1] = (unsigned char)(0x80U | ((value >> 12) & 0x3fU));
    output[2] = (unsigned char)(0x80U | ((value >> 6) & 0x3fU));
    output[3] = (unsigned char)(0x80U | (value & 0x3fU));
    return 4;
}

static void simple_escape(unsigned char byte, escape_result *result)
{
    if (result == NULL) {
        return;
    }
    switch (byte) {
    case 'a': result->bytes[0] = '\a'; break;
    case 'b': result->bytes[0] = '\b'; break;
    case 'e':
    case 'E': result->bytes[0] = 0x1bU; break;
    case 'f': result->bytes[0] = '\f'; break;
    case 'n': result->bytes[0] = '\n'; break;
    case 'r': result->bytes[0] = '\r'; break;
    case 't': result->bytes[0] = '\t'; break;
    case 'v': result->bytes[0] = '\v'; break;
    case '\\': result->bytes[0] = '\\'; break;
    default:
        result->bytes[0] = '\\';
        result->bytes[1] = byte;
        result->length = 2;
        return;
    }
    result->length = 1;
}

static void numeric_escape(const char *text, size_t available,
                           size_t digits, unsigned int base,
                           escape_result *result)
{
    if (result == NULL || text == NULL) {
        return;
    }
    uint32_t value = 0;
    size_t index;

    for (index = 0; index < digits && index < available; index++) {
        int digit = hex_digit((unsigned char)text[index]);

        if (digit < 0 || (unsigned int)digit >= base) {
            break;
        }
        value = value * base + (unsigned int)digit;
    }
    if (index == 0) {
        simple_escape((unsigned char)text[-1], result);
        return;
    }
    result->bytes[0] = (unsigned char)(value & 0xffU);
    result->length = 1;
    result->consumed += index;
}

static void unicode_escape(const char *text, size_t available,
                           size_t digits, escape_result *result)
{
    if (result == NULL || text == NULL) {
        return;
    }
    uint32_t value = 0;
    size_t index;

    for (index = 0; index < digits && index < available; index++) {
        int digit = hex_digit((unsigned char)text[index]);

        if (digit < 0) {
            break;
        }
        value = value * 16U + (unsigned int)digit;
    }
    if (index == 0) {
        simple_escape((unsigned char)text[-1], result);
        return;
    }
    result->length = encode_utf8(value, result->bytes);
    result->consumed += index;
}

typedef enum {
    ESCAPE_ECHO,
    ESCAPE_PRINTF_FORMAT,
    ESCAPE_PRINTF_B,
} escape_mode;

static escape_result decode_escape(const char *text, size_t available,
                                   escape_mode mode)
{
    if (text == NULL) {
        return (escape_result){0};
    }
    escape_result result = {{0}, 0, 1, false};
    unsigned char kind;

    if (available < 2U) {
        result.bytes[0] = '\\';
        result.length = 1;
        return result;
    }
    kind = (unsigned char)text[1];
    result.consumed = 2;
    if (kind == 'c' && mode != ESCAPE_PRINTF_FORMAT) {
        result.stop = true;
    } else if (kind == '0') {
        if (available == 2U || text[2] < '0' || text[2] > '7') {
            result.bytes[0] = 0;
            result.length = 1U;
        } else {
            numeric_escape(text + 2, available - 2U, 3, 8U, &result);
        }
    } else if (mode == ESCAPE_PRINTF_FORMAT && kind >= '1' &&
               kind <= '7') {
        result.consumed = 1U;
        numeric_escape(text + 1, available - 1U, 3, 8U, &result);
    } else if (mode == ESCAPE_ECHO && kind == 'x') {
        numeric_escape(text + 2, available - 2U, 2, 16U, &result);
    } else if (mode == ESCAPE_ECHO && kind == 'u') {
        unicode_escape(text + 2, available - 2U, 4, &result);
    } else if (mode == ESCAPE_ECHO && kind == 'U') {
        unicode_escape(text + 2, available - 2U, 8, &result);
    } else {
        simple_escape(kind, &result);
    }
    return result;
}

static bool echo_option(const char *argument, bool *newline,
                        bool *escapes)
{
    if (argument == NULL) return false;
    if (escapes == NULL || newline == NULL) {
        return false;
    }
    size_t index;

    if (argument[0] != '-' || argument[1] == '\0') {
        return false;
    }
    for (index = 1; argument[index] != '\0'; index++) {
        if (argument[index] != 'n' && argument[index] != 'e' &&
            argument[index] != 'E') {
            return false;
        }
    }
    for (index = 1; argument[index] != '\0'; index++) {
        if (argument[index] == 'n') {
            *newline = false;
        } else {
            *escapes = argument[index] == 'e';
        }
    }
    return true;
}

static int echo_escaped_operand(const gsh_builtin_io *io,
                                const char *operand, bool *stopped)
{
    if (operand == NULL || stopped == NULL) {
        return -1;
    }
    size_t length = strlen(operand);
    size_t offset = 0;

    while (offset < length) {
        const char *slash = memchr(operand + offset, '\\', length - offset);
        size_t plain = slash == NULL ? length - offset
                                    : (size_t)(slash - operand - offset);

        if (plain != 0 && emit_bytes(io, STDOUT_FILENO,
                                     operand + offset, plain) != 0) {
            return 1;
        }
        offset += plain;
        if (slash == NULL) {
            break;
        }
        {
            escape_result result = decode_escape(
                operand + offset, length - offset, ESCAPE_ECHO);

            if (result.stop) {
                *stopped = true;
                return 0;
            }
            if (emit_bytes(io, STDOUT_FILENO, result.bytes,
                           result.length) != 0) {
                return 1;
            }
            offset += result.consumed;
        }
    }
    return 0;
}

static int builtin_echo(size_t argc, char *const argv[],
                        const gsh_builtin_io *io)
{
    if (argv == NULL) {
        return -1;
    }
    size_t index = 1;
    size_t operand_index;
    bool newline = true;
    bool escapes = false;
    bool stopped = false;

    while (index < argc && echo_option(argv[index], &newline, &escapes)) {
        index++;
    }
    operand_index = index;
    for (; index < argc && !stopped; index++) {
        if (index != operand_index &&
            emit_bytes(io, STDOUT_FILENO, " ", 1) != 0) {
            return 1;
        }
        if (escapes) {
            if (echo_escaped_operand(io, argv[index], &stopped) != 0) {
                return 1;
            }
        } else if (emit_bytes(io, STDOUT_FILENO, argv[index],
                              strlen(argv[index])) != 0) {
            return 1;
        }
    }
    return !stopped && newline
               ? emit_bytes(io, STDOUT_FILENO, "\n", 1)
               : 0;
}

static int parse_decimal_field(const char *text, size_t *offset, int *value)
{
    if (offset == NULL || text == NULL || value == NULL) {
        return -1;
    }
    unsigned int parsed = 0;
    size_t consumed = 0;

    while (consumed < 10U && text[*offset] >= '0' &&
           text[*offset] <= '9') {
        unsigned int digit = (unsigned int)(text[*offset] - '0');

        if (parsed > ((unsigned int)GSH_PRINTF_FIELD_CAP - digit) / 10U) {
            return -1;
        }
        parsed = parsed * 10U + digit;
        (*offset)++;
        consumed++;
    }
    *value = (int)parsed;
    return 0;
}

static int parse_star_value(size_t argc, char *const argv[], size_t *argument,
                            int *value)
{
    if (argument == NULL) return -1;
    if (argv == NULL || value == NULL) {
        return -1;
    }
    char *end;
    long parsed;

    if (*argument >= argc) {
        *value = 0;
        return 0;
    }
    errno = 0;
    parsed = strtol(argv[(*argument)++], &end, 10);
    if (errno != 0 || *end != '\0' || parsed < -GSH_PRINTF_FIELD_CAP ||
        parsed > GSH_PRINTF_FIELD_CAP) {
        return -1;
    }
    *value = (int)parsed;
    return 0;
}

static int parse_printf_conversion(const char *format, size_t available,
                                   size_t argc, char *const argv[],
                                   size_t *argument,
                                   printf_conversion *conversion)
{
    if (argument == NULL || argv == NULL || conversion == NULL || format == NULL) {
        return -1;
    }
    size_t offset = 1;

    (void)memset(conversion, 0, sizeof(*conversion));
    conversion->precision = -1;
    while (offset < available && strchr("-+ #0'", format[offset]) != NULL &&
           conversion->flag_count < sizeof(conversion->flags)) {
        conversion->flags[conversion->flag_count++] = format[offset++];
    }
    if (offset < available && format[offset] == '*') {
        conversion->width_set = true;
        offset++;
        if (parse_star_value(argc, argv, argument, &conversion->width) != 0) {
            return -1;
        }
    } else if (offset < available && format[offset] >= '0' &&
               format[offset] <= '9') {
        conversion->width_set = true;
        if (parse_decimal_field(format, &offset, &conversion->width) != 0) {
            return -1;
        }
    }
    if (offset < available && format[offset] == '.') {
        conversion->precision_set = true;
        conversion->precision = 0;
        offset++;
        if (offset < available && format[offset] == '*') {
            offset++;
            if (parse_star_value(argc, argv, argument,
                                 &conversion->precision) != 0) {
                return -1;
            }
        } else if (parse_decimal_field(format, &offset,
                                       &conversion->precision) != 0) {
            return -1;
        }
    }
    if (offset >= available || strchr("bcdiFeEfgGaAosuxX%", format[offset]) ==
                                   NULL) {
        return -1;
    }
    conversion->conversion = format[offset];
    conversion->consumed = offset + 1U;
    return 0;
}

static int build_printf_spec(const printf_conversion *conversion,
                             char result[GSH_PRINTF_SPEC_CAP],
                             char final_conversion)
{
    if (conversion == NULL) return -1;
    if (result == NULL) {
        return -1;
    }
    size_t used = 0;
    int count;

    result[used++] = '%';
    if (conversion->flag_count != 0) {
        (void)memcpy(result + used, conversion->flags, conversion->flag_count);
        used += conversion->flag_count;
    }
    if (conversion->width_set) {
        count = snprintf(result + used, GSH_PRINTF_SPEC_CAP - used, "%d",
                         conversion->width);
        if (count < 0 || (size_t)count >= GSH_PRINTF_SPEC_CAP - used) {
            return -1;
        }
        used += (size_t)count;
    }
    if (conversion->precision_set && conversion->precision >= 0) {
        count = snprintf(result + used, GSH_PRINTF_SPEC_CAP - used, ".%d",
                         conversion->precision);
        if (count < 0 || (size_t)count >= GSH_PRINTF_SPEC_CAP - used) {
            return -1;
        }
        used += (size_t)count;
    }
    if (strchr("diouxX", final_conversion) != NULL) {
        result[used++] = 'j';
    } else if (strchr("FeEfgGaA", final_conversion) != NULL) {
        result[used++] = 'L';
    }
    if (used + 2U > GSH_PRINTF_SPEC_CAP) {
        return -1;
    }
    result[used++] = final_conversion;
    result[used] = '\0';
    return 0;
}

static uintmax_t printf_quoted_operand(const char *text, bool *valid,
                                      bool *quoted)
{
    if (quoted == NULL || text == NULL || valid == NULL) {
        return 0U;
    }
    mbstate_t state;
    wchar_t value;
    size_t length;

    *quoted = text[0] == '\'' || text[0] == '"';
    if (!*quoted) return 0;
    if (text[1] == '\0') return 0;
    (void)memset(&state, 0, sizeof(state));
    length = mbrtowc(&value, text + 1U, strlen(text + 1U), &state);
    if (length == (size_t)-1 || length == (size_t)-2) {
        *valid = false;
        return (unsigned char)text[1];
    }
    return (uintmax_t)value;
}

static intmax_t printf_signed_operand(const char *text, bool *valid)
{
    if (text == NULL || valid == NULL) {
        return -1;
    }
    char *end;
    intmax_t value;
    bool quoted;
    uintmax_t quoted_value = printf_quoted_operand(text, valid, &quoted);

    if (quoted) return (intmax_t)quoted_value;
    errno = 0;
    value = strtoimax(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        *valid = false;
    }
    return end == text ? 0 : value;
}

static uintmax_t printf_unsigned_operand(const char *text, bool *valid)
{
    if (text == NULL || valid == NULL) {
        return 0U;
    }
    char *end;
    uintmax_t value;
    bool quoted;
    uintmax_t quoted_value = printf_quoted_operand(text, valid, &quoted);

    if (quoted) return quoted_value;
    errno = 0;
    value = strtoumax(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        *valid = false;
    }
    return end == text ? 0 : value;
}

static long double printf_float_operand(const char *text, bool *valid)
{
    if (text == NULL || valid == NULL) {
        return 0.0;
    }
    char *end;
    long double value;
    bool quoted;
    uintmax_t quoted_value = printf_quoted_operand(text, valid, &quoted);

    if (quoted) return (long double)quoted_value;
    errno = 0;
    value = strtold(text, &end);
    if (errno != 0 || end == text || *end != '\0') {
        *valid = false;
    }
    return end == text ? 0.0L : value;
}

static int emit_printf_field(const gsh_builtin_io *io, const char *spec,
                             char conversion, const char *operand,
                             bool *valid)
{
    if (io == NULL || operand == NULL || spec == NULL || valid == NULL) {
        return -1;
    }
    char output[GSH_PRINTF_FIELD_CAP + 1U];
    int length;

    if (conversion == 'd' || conversion == 'i') {
        length = snprintf(output, sizeof(output), spec,
                          printf_signed_operand(operand, valid));
    } else if (strchr("ouxX", conversion) != NULL) {
        length = snprintf(output, sizeof(output), spec,
                          printf_unsigned_operand(operand, valid));
    } else if (strchr("FeEfgGaA", conversion) != NULL) {
        length = snprintf(output, sizeof(output), spec,
                          printf_float_operand(operand, valid));
    } else if (conversion == 'c') {
        length = snprintf(output, sizeof(output), spec,
                          operand[0] == '\0' ? 0 : (unsigned char)operand[0]);
    } else {
        length = snprintf(output, sizeof(output), spec, operand);
    }
    if (length < 0 || length > GSH_PRINTF_FIELD_CAP) {
        errno = EOVERFLOW;
        return 1;
    }
    return emit_bytes(io, STDOUT_FILENO, output, (size_t)length);
}

static int decode_printf_b(const char *operand, unsigned char *output,
                           size_t capacity, size_t *length, bool *stop)
{
    if (operand == NULL || output == NULL) {
        return -1;
    }
    size_t input_length = strlen(operand);
    size_t input = 0;
    size_t used = 0;

    while (input < input_length) {
        if (operand[input] != '\\') {
            if (used == capacity) {
                return -1;
            }
            output[used++] = (unsigned char)operand[input++];
        } else {
            escape_result result = decode_escape(
                operand + input, input_length - input, ESCAPE_PRINTF_B);

            if (result.stop) {
                *stop = true;
                break;
            }
            if (result.length > capacity - used) {
                return -1;
            }
            (void)memcpy(output + used, result.bytes, result.length);
            used += result.length;
            input += result.consumed;
        }
    }
    *length = used;
    return 0;
}

static int emit_padding(const gsh_builtin_io *io, size_t count)
{
    static const char spaces[] = "                                ";
    size_t emitted = 0;

    while (emitted < count) {
        size_t chunk = count - emitted;

        if (chunk > sizeof(spaces) - 1U) {
            chunk = sizeof(spaces) - 1U;
        }
        if (emit_bytes(io, STDOUT_FILENO, spaces, chunk) != 0) {
            return 1;
        }
        emitted += chunk;
    }
    return 0;
}

static bool printf_left_adjusted(const printf_conversion *conversion)
{
    if (conversion == NULL) return false;
    size_t index;

    if (conversion->width < 0) {
        return true;
    }
    for (index = 0; index < conversion->flag_count; index++) {
        if (conversion->flags[index] == '-') {
            return true;
        }
    }
    return false;
}

static int emit_printf_b(const gsh_builtin_io *io,
                         const printf_conversion *conversion,
                         const char *operand, bool *stop)
{
    if (conversion == NULL) {
        return -1;
    }
    unsigned char decoded[GSH_PRINTF_FIELD_CAP];
    size_t length;
    size_t visible;
    size_t width;
    size_t padding;
    bool left;

    if (decode_printf_b(operand, decoded, sizeof(decoded), &length, stop) !=
        0) {
        return 1;
    }
    visible = conversion->precision_set && conversion->precision >= 0 &&
                      (size_t)conversion->precision < length
                  ? (size_t)conversion->precision
                  : length;
    width = conversion->width < 0
                ? (size_t)(-conversion->width)
                : (size_t)conversion->width;
    padding = width > visible ? width - visible : 0;
    left = printf_left_adjusted(conversion);
    if ((!left && emit_padding(io, padding) != 0) ||
        emit_bytes(io, STDOUT_FILENO, decoded, visible) != 0 ||
        (left && emit_padding(io, padding) != 0)) {
        return 1;
    }
    return 0;
}

static int emit_format_escape(const gsh_builtin_io *io, const char *format,
                              size_t available, size_t *consumed)
{
    if (consumed == NULL || format == NULL || io == NULL) {
        return -1;
    }
    escape_result result = decode_escape(format, available,
                                         ESCAPE_PRINTF_FORMAT);

    *consumed = result.consumed;
    return emit_bytes(io, STDOUT_FILENO, result.bytes, result.length);
}

static int run_printf_conversion(const gsh_builtin_io *io,
                                 const printf_conversion *conversion,
                                 size_t argc, char *const argv[],
                                 size_t *argument, bool *valid, bool *stop)
{
    if (conversion == NULL) return -1;
    if (argument == NULL || argv == NULL || io == NULL || stop == NULL || valid == NULL) {
        return -1;
    }
    const char *operand;
    bool missing;
    char spec[GSH_PRINTF_SPEC_CAP];

    if (conversion->conversion == '%') {
        return emit_bytes(io, STDOUT_FILENO, "%", 1);
    }
    missing = *argument >= argc;
    operand = missing ? "" : argv[(*argument)++];
    if (missing && strchr("diouxXFeEfgGaA", conversion->conversion) != NULL) {
        operand = "0";
    }
    if (conversion->conversion == 'b') {
        return emit_printf_b(io, conversion, operand, stop);
    }
    if (build_printf_spec(conversion, spec, conversion->conversion) != 0) {
        return 1;
    }
    return emit_printf_field(io, spec, conversion->conversion, operand,
                             valid);
}

static int run_printf_format(const gsh_builtin_io *io, const char *format,
                             size_t argc, char *const argv[],
                             size_t *argument, bool *valid, bool *stop,
                             bool *used_operand)
{
    if (argument == NULL || format == NULL || io == NULL || stop == NULL || used_operand == NULL) {
        return -1;
    }
    size_t length = strlen(format);
    size_t offset = 0;

    while (offset < length && !*stop) {
        if (format[offset] == '\\') {
            size_t consumed;

            if (emit_format_escape(io, format + offset, length - offset,
                                   &consumed) != 0) {
                return 1;
            }
            offset += consumed;
        } else if (format[offset] != '%') {
            size_t begin = offset;

            while (offset < length && format[offset] != '%' &&
                   format[offset] != '\\') offset++;
            if (emit_bytes(io, STDOUT_FILENO, format + begin,
                           offset - begin) != 0) {
                return 1;
            }
        } else {
            printf_conversion conversion;
            size_t before = *argument;

            if (parse_printf_conversion(format + offset, length - offset,
                                        argc, argv, argument,
                                        &conversion) != 0 ||
                run_printf_conversion(io, &conversion, argc, argv, argument,
                                      valid, stop) != 0) {
                return 1;
            }
            *used_operand = *used_operand || *argument != before;
            offset += conversion.consumed;
        }
    }
    return 0;
}

static int builtin_printf(size_t argc, char *const argv[],
                          const gsh_builtin_io *io)
{
    if (argv == NULL || io == NULL) {
        return -1;
    }
    size_t format_index = argc > 1U && strcmp(argv[1], "--") == 0 ? 2U : 1U;
    size_t argument = format_index + 1U;
    bool valid = true;
    bool stop = false;
    bool repeat;

    if (format_index >= argc) {
        return gsh_builtin_error(io, "printf", "missing format operand");
    }
    do {
        size_t before = argument;
        bool used_operand = false;

        if (run_printf_format(io, argv[format_index], argc, argv, &argument,
                              &valid, &stop, &used_operand) != 0) {
            return gsh_builtin_error(io, "printf", "invalid format or value");
        }
        repeat = !stop && used_operand && argument < argc && argument > before;
    } while (repeat);
    if (!valid) {
        (void)gsh_builtin_error(io, "printf", "invalid numeric operand");
        return 1;
    }
    return 0;
}

static bool parse_test_integer(const char *text, intmax_t *value)
{
    if (text == NULL || value == NULL) {
        return false;
    }
    char *end;

    errno = 0;
    *value = strtoimax(text, &end, 10);
    return errno == 0 && end != text && *end == '\0';
}

static bool test_file_unary(const char *operator, const char *operand,
                            bool *recognized)
{
    if (operand == NULL || operator == NULL || recognized == NULL) {
        return false;
    }
    struct stat information;
    int stat_status = (strcmp(operator, "-h") == 0 ||
                       strcmp(operator, "-L") == 0)
                          ? lstat(operand, &information)
                          : stat(operand, &information);

    *recognized = true;
    if (strcmp(operator, "-e") == 0) return stat_status == 0;
    if (strcmp(operator, "-b") == 0) return stat_status == 0 && S_ISBLK(information.st_mode);
    if (strcmp(operator, "-c") == 0) return stat_status == 0 && S_ISCHR(information.st_mode);
    if (strcmp(operator, "-d") == 0) return stat_status == 0 && S_ISDIR(information.st_mode);
    if (strcmp(operator, "-f") == 0) return stat_status == 0 && S_ISREG(information.st_mode);
    if (strcmp(operator, "-g") == 0) return stat_status == 0 && (information.st_mode & S_ISGID) != 0;
    if (strcmp(operator, "-h") == 0 || strcmp(operator, "-L") == 0) return stat_status == 0 && S_ISLNK(information.st_mode);
    if (strcmp(operator, "-p") == 0) return stat_status == 0 && S_ISFIFO(information.st_mode);
    if (strcmp(operator, "-r") == 0) return access(operand, R_OK) == 0;
    if (strcmp(operator, "-S") == 0) return stat_status == 0 && S_ISSOCK(information.st_mode);
    if (strcmp(operator, "-s") == 0) return stat_status == 0 && information.st_size > 0;
    if (strcmp(operator, "-u") == 0) return stat_status == 0 && (information.st_mode & S_ISUID) != 0;
    if (strcmp(operator, "-w") == 0) return access(operand, W_OK) == 0;
    if (strcmp(operator, "-x") == 0) return access(operand, X_OK) == 0;
    *recognized = false;
    return false;
}

static bool test_unary(const char *operator, const char *operand,
                       bool *recognized)
{
    if (operand == NULL || recognized == NULL) {
        return false;
    }
    if (strcmp(operator, "-n") == 0) {
        *recognized = true;
        return operand[0] != '\0';
    }
    if (strcmp(operator, "-z") == 0) {
        *recognized = true;
        return operand[0] == '\0';
    }
    if (strcmp(operator, "-t") == 0) {
        intmax_t descriptor;

        *recognized = true;
        return parse_test_integer(operand, &descriptor) && descriptor >= 0 &&
               descriptor <= INT_MAX && isatty((int)descriptor) != 0;
    }
    return test_file_unary(operator, operand, recognized);
}

static bool test_file_binary(const char *left, const char *operator,
                             const char *right, bool *recognized)
{
    if (left == NULL || recognized == NULL || right == NULL) {
        return false;
    }
    struct stat first;
    struct stat second;
    bool first_exists = stat(left, &first) == 0;
    bool second_exists = stat(right, &second) == 0;

    *recognized = true;
    if (strcmp(operator, "-ef") == 0) {
        return first_exists && second_exists && first.st_dev == second.st_dev &&
               first.st_ino == second.st_ino;
    }
    if (strcmp(operator, "-nt") == 0 || strcmp(operator, "-ot") == 0) {
        int comparison = 0;

        if (first_exists && second_exists) {
#if defined(__APPLE__)
            if (first.st_mtime != second.st_mtime) {
                comparison = first.st_mtime < second.st_mtime ? -1 : 1;
            } else if (first.st_mtimensec != second.st_mtimensec) {
                comparison = first.st_mtimensec < second.st_mtimensec
                                 ? -1 : 1;
            }
#else
            if (first.st_mtim.tv_sec != second.st_mtim.tv_sec) {
                comparison = first.st_mtim.tv_sec < second.st_mtim.tv_sec
                                 ? -1 : 1;
            } else if (first.st_mtim.tv_nsec != second.st_mtim.tv_nsec) {
                comparison = first.st_mtim.tv_nsec < second.st_mtim.tv_nsec
                                 ? -1 : 1;
            }
#endif
        }
        return strcmp(operator, "-nt") == 0
                   ? first_exists && (!second_exists || comparison > 0)
                   : second_exists && (!first_exists || comparison < 0);
    }
    *recognized = false;
    return false;
}

static bool test_binary(const char *left, const char *operator,
                        const char *right, bool *recognized, bool *valid)
{
    if (left == NULL || recognized == NULL || right == NULL || valid == NULL) {
        return false;
    }
    intmax_t first;
    intmax_t second;

    *recognized = true;
    if (strcmp(operator, "=") == 0) return strcmp(left, right) == 0;
    if (strcmp(operator, "!=") == 0) return strcmp(left, right) != 0;
    if (strcmp(operator, "<") == 0) return strcmp(left, right) < 0;
    if (strcmp(operator, ">") == 0) return strcmp(left, right) > 0;
    if (operator[0] == '-' &&
        (strcmp(operator, "-eq") == 0 || strcmp(operator, "-ne") == 0 ||
         strcmp(operator, "-gt") == 0 || strcmp(operator, "-ge") == 0 ||
         strcmp(operator, "-lt") == 0 || strcmp(operator, "-le") == 0)) {
        *valid = parse_test_integer(left, &first) &&
                 parse_test_integer(right, &second);
        if (!*valid) return false;
        if (strcmp(operator, "-eq") == 0) return first == second;
        if (strcmp(operator, "-ne") == 0) return first != second;
        if (strcmp(operator, "-gt") == 0) return first > second;
        if (strcmp(operator, "-ge") == 0) return first >= second;
        if (strcmp(operator, "-lt") == 0) return first < second;
        return first <= second;
    }
    return test_file_binary(left, operator, right, recognized);
}

static int test_small_expression(size_t count, char *const operands[],
                                 bool *value)
{
    if (operands == NULL || value == NULL) {
        return -1;
    }
    bool recognized = false;
    bool valid = true;

    if (count == 0) *value = false;
    else if (count == 1) *value = operands[0][0] != '\0';
    else if (count == 2 && strcmp(operands[0], "!") == 0)
        *value = operands[1][0] == '\0';
    else if (count == 2)
        *value = test_unary(operands[0], operands[1], &recognized);
    else if (count == 3 && strcmp(operands[1], "-a") == 0) {
        recognized = true;
        *value = operands[0][0] != '\0' && operands[2][0] != '\0';
    } else if (count == 3 && strcmp(operands[1], "-o") == 0) {
        recognized = true;
        *value = operands[0][0] != '\0' || operands[2][0] != '\0';
    } else if (count == 3 && strcmp(operands[0], "!") == 0) {
        bool nested;

        if (strcmp(operands[1], "!") == 0) {
            nested = operands[2][0] == '\0';
            recognized = true;
        } else {
            nested = test_unary(operands[1], operands[2], &recognized);
        }
        *value = !nested;
    } else if (count == 3 && strcmp(operands[0], "(") == 0 &&
               strcmp(operands[2], ")") == 0) *value = operands[1][0] != '\0';
    else if (count == 3)
        *value = test_binary(operands[0], operands[1], operands[2],
                             &recognized, &valid);
    else return -1;
    return (count < 2 || recognized ||
            (count == 2 && strcmp(operands[0], "!") == 0)) && valid ? 0 : -1;
}

static int test_precedence(test_operator operator)
{
    if (operator == TEST_OPERATOR_NOT) return 3;
    if (operator == TEST_OPERATOR_AND) return 2;
    if (operator == TEST_OPERATOR_OR) return 1;
    return 0;
}

static bool apply_test_operator(bool values[GSH_TEST_ARGUMENT_CAP],
                                size_t *value_count,
                                test_operator operator)
{
    if (value_count == NULL) return false;
    if (values == NULL) {
        return false;
    }
    if (operator == TEST_OPERATOR_NOT && *value_count >= 1U) {
        values[*value_count - 1U] = !values[*value_count - 1U];
        return true;
    }
    if ((operator == TEST_OPERATOR_AND || operator == TEST_OPERATOR_OR) &&
        *value_count >= 2U) {
        bool right = values[--*value_count];
        bool left = values[*value_count - 1U];

        values[*value_count - 1U] = operator == TEST_OPERATOR_AND
                                        ? left && right : left || right;
        return true;
    }
    return false;
}

static bool reduce_test_operators(bool values[GSH_TEST_ARGUMENT_CAP],
                                  size_t *value_count,
                                  test_operator operators[GSH_TEST_ARGUMENT_CAP],
                                  size_t *operator_count, int precedence)
{
    if (operator_count == NULL || operators == NULL) {
        return false;
    }
    while (*operator_count > 0 &&
           operators[*operator_count - 1U] != TEST_OPERATOR_LEFT &&
           test_precedence(operators[*operator_count - 1U]) >= precedence) {
        if (!apply_test_operator(values, value_count,
                                 operators[--*operator_count])) {
            return false;
        }
    }
    return true;
}

static int test_primary(size_t count, char *const operands[], size_t *offset,
                        bool *value)
{
    if (offset == NULL) return -1;
    if (operands == NULL || value == NULL) {
        return -1;
    }
    bool recognized = false;
    bool valid = true;

    if (*offset + 2U < count) {
        *value = test_binary(operands[*offset], operands[*offset + 1U],
                             operands[*offset + 2U], &recognized, &valid);
    }
    if (recognized) {
        if (!valid) return -1;
        *offset += 3U;
        return 0;
    }
    if (*offset + 1U < count) {
        *value = test_unary(operands[*offset], operands[*offset + 1U],
                            &recognized);
        if (recognized) {
            *offset += 2U;
            return 0;
        }
    }
    *value = operands[(*offset)++][0] != '\0';
    return 0;
}

static int test_large_expression(size_t count, char *const operands[],
                                 bool *result)
{
    if (operands == NULL || result == NULL) {
        return -1;
    }
    bool values[GSH_TEST_ARGUMENT_CAP];
    test_operator operators[GSH_TEST_ARGUMENT_CAP];
    size_t value_count = 0;
    size_t operator_count = 0;
    size_t offset = 0;
    bool expecting_value = true;

    while (offset < count && offset < GSH_TEST_ARGUMENT_CAP) {
        const char *token = operands[offset];

        if (expecting_value && strcmp(token, "!") == 0) {
            operators[operator_count++] = TEST_OPERATOR_NOT;
            offset++;
        } else if (expecting_value && strcmp(token, "(") == 0) {
            operators[operator_count++] = TEST_OPERATOR_LEFT;
            offset++;
        } else if (!expecting_value && strcmp(token, ")") == 0) {
            if (!reduce_test_operators(values, &value_count, operators,
                                       &operator_count, 0) ||
                operator_count == 0) return -1;
            operator_count--;
            offset++;
        } else if (!expecting_value &&
                   (strcmp(token, "-a") == 0 || strcmp(token, "-o") == 0)) {
            test_operator operation = strcmp(token, "-a") == 0
                                          ? TEST_OPERATOR_AND : TEST_OPERATOR_OR;
            if (!reduce_test_operators(values, &value_count, operators,
                                       &operator_count,
                                       test_precedence(operation))) return -1;
            operators[operator_count++] = operation;
            offset++;
            expecting_value = true;
        } else if (expecting_value) {
            if (test_primary(count, operands, &offset,
                             &values[value_count++]) != 0 ||
                !reduce_test_operators(values, &value_count, operators,
                                       &operator_count, 3)) return -1;
            expecting_value = false;
        } else return -1;
    }
    if (expecting_value || offset != count ||
        !reduce_test_operators(values, &value_count, operators,
                               &operator_count, 0) ||
        operator_count != 0 || value_count != 1U) return -1;
    *result = values[0];
    return 0;
}

static int builtin_test(gsh_builtin_kind kind, size_t argc,
                        char *const argv[], const gsh_builtin_io *io)
{
    if (argv == NULL || io == NULL) {
        return -1;
    }
    size_t first = 1U;
    size_t count;
    bool value = false;
    int status;

    if (kind == GSH_BUILTIN_BRACKET) {
        if (argc < 2U || strcmp(argv[argc - 1U], "]") != 0) {
            (void)gsh_builtin_error(io, "[", "missing ]");
            return 2;
        }
        argc--;
    }
    count = argc - first;
    status = count <= 3U
                 ? test_small_expression(count, argv + first, &value)
                 : test_large_expression(count, argv + first, &value);
    if (status != 0) {
        (void)gsh_builtin_error(io, argv[0], "invalid expression");
        return 2;
    }
    return value ? 0 : 1;
}

int gsh_builtin_run_pure(gsh_builtin_kind kind, size_t argc,
                         char *const argv[], const gsh_builtin_io *io)
{
    if (argc == 0 || argv == NULL || !gsh_builtin_io_valid(io)) {
        errno = EINVAL;
        return 125;
    }
    switch (kind) {
    case GSH_BUILTIN_COLON:
    case GSH_BUILTIN_TRUE:
        return 0;
    case GSH_BUILTIN_FALSE:
        return 1;
    case GSH_BUILTIN_ECHO:
        return builtin_echo(argc, argv, io);
    case GSH_BUILTIN_PRINTF:
        return builtin_printf(argc, argv, io);
    case GSH_BUILTIN_TEST:
    case GSH_BUILTIN_BRACKET:
        return builtin_test(kind, argc, argv, io);
    default:
        errno = EINVAL;
        return 125;
    }
}
