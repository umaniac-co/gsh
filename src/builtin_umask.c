#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_umask.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { GSH_UMASK_TEXT_CAP = 64 };

static int fail(const gsh_builtin_io *io, const char *message)
{
    if (io == NULL || message == NULL) {
        return -1;
    }
    return gsh_builtin_error(io, "umask", message);
}

static mode_t who_bits(unsigned int who)
{
    mode_t bits = 0;

    if ((who & 1U) != 0) {
        bits |= 0700;
    }
    if ((who & 2U) != 0) {
        bits |= 0070;
    }
    if ((who & 4U) != 0) {
        bits |= 0007;
    }
    return bits;
}

static mode_t permission_bits(unsigned int who, unsigned int permissions)
{
    mode_t bits = 0;

    if ((who & 1U) != 0) {
        bits |= (mode_t)permissions << 6U;
    }
    if ((who & 2U) != 0) {
        bits |= (mode_t)permissions << 3U;
    }
    if ((who & 4U) != 0) {
        bits |= (mode_t)permissions;
    }
    return bits;
}

static int parse_octal(const char *text, mode_t *mask)
{
    if (text == NULL) return -1;
    if (mask == NULL) {
        return -1;
    }
    mode_t value = 0;

    if (*text == '\0') {
        return -1;
    }
    while (*text != '\0') {
        if (*text < '0' || *text > '7' || value > 0777) {
            return -1;
        }
        value = (mode_t)(value * 8U + (unsigned int)(*text - '0'));
        text++;
    }
    if (value > 07777) {
        return -1;
    }
    *mask = value;
    return 0;
}

static int parse_symbolic(const char *text, mode_t initial, mode_t *result)
{
    if (result == NULL || text == NULL) {
        return -1;
    }
    mode_t mask = initial;
    const char *cursor = text;
    size_t clause;

    if (*cursor == '\0') {
        return -1;
    }
    for (clause = 0; clause < GSH_UMASK_TEXT_CAP; clause++) {
        unsigned int who = 0;
        bool action = false;

        while (*cursor == 'u' || *cursor == 'g' || *cursor == 'o' ||
               *cursor == 'a') {
            who |= *cursor == 'u' ? 1U
                   : *cursor == 'g' ? 2U
                   : *cursor == 'o' ? 4U
                                    : 7U;
            cursor++;
        }
        if (who == 0) {
            who = 7U;
        }
        while (*cursor == '+' || *cursor == '-' || *cursor == '=') {
            char operation = *cursor++;
            unsigned int permissions = 0;
            mode_t bits;
            mode_t allowed;

            action = true;
            if ((*cursor == 'u' || *cursor == 'g' || *cursor == 'o') &&
                (cursor[1] == '\0' || cursor[1] == ',' ||
                 cursor[1] == '+' || cursor[1] == '-' ||
                 cursor[1] == '=')) {
                unsigned int shift = *cursor == 'u' ? 6U
                                     : *cursor == 'g' ? 3U
                                                      : 0U;

                permissions = (unsigned int)(((~mask) >> shift) & 7U);
                cursor++;
            } else {
                while (*cursor == 'r' || *cursor == 'w' || *cursor == 'x' ||
                       *cursor == 'X' || *cursor == 's' || *cursor == 't') {
                    if (*cursor == 'r') {
                        permissions |= 4U;
                    } else if (*cursor == 'w') {
                        permissions |= 2U;
                    } else if (*cursor == 'x' ||
                               (*cursor == 'X' &&
                                ((~initial) & 0111) != 0)) {
                        permissions |= 1U;
                    }
                    cursor++;
                }
            }
            bits = permission_bits(who, permissions);
            allowed = who_bits(who);
            if (operation == '+') {
                mask &= ~bits;
            } else if (operation == '-') {
                mask |= bits;
            } else {
                mask = (mask & ~allowed) | (allowed & ~bits);
            }
        }
        if (!action) {
            return -1;
        }
        if (*cursor == '\0') {
            *result = mask;
            return 0;
        }
        if (*cursor++ != ',' || *cursor == '\0') {
            return -1;
        }
    }
    return -1;
}

static int report_mask(mode_t mask, bool symbolic,
                       const gsh_builtin_io *io)
{
    if (io == NULL) {
        return -1;
    }
    char output[64];
    int length;

    if (symbolic) {
        static const mode_t bits[3][3] = {
            {0400, 0200, 0100}, {0040, 0020, 0010}, {0004, 0002, 0001}};
        static const char names[] = "rwx";
        char permissions[3][4];
        size_t group;

        for (group = 0; group < 3; group++) {
            size_t input;
            size_t used = 0;

            for (input = 0; input < 3; input++) {
                if ((mask & bits[group][input]) == 0) {
                    permissions[group][used++] = names[input];
                }
            }
            permissions[group][used] = '\0';
        }
        length = snprintf(output, sizeof(output), "u=%s,g=%s,o=%s\n",
                          permissions[0], permissions[1], permissions[2]);
    } else {
        length = snprintf(output, sizeof(output), "%04o\n",
                          (unsigned int)mask);
    }
    return length < 0 || (size_t)length >= sizeof(output)
               ? fail(io, "value cannot be formatted")
               : gsh_builtin_output(io, STDOUT_FILENO, output,
                            (size_t)length);
}

int gsh_builtin_umask(size_t argc, char *const argv[],
                      const gsh_builtin_io *io)
{
    if (argv == NULL) return 125;
    if (io == NULL) {
        return -1;
    }
    const char *operand = NULL;
    bool symbolic = false;
    mode_t current;
    mode_t changed;
    size_t index;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--") == 0) {
            index++;
            break;
        }
        if (strcmp(argv[index], "-S") == 0) {
            symbolic = true;
        } else if (argv[index][0] == '-') {
            return fail(io, "invalid option");
        } else {
            break;
        }
    }
    if (index < argc) {
        operand = argv[index++];
    }
    if (index != argc) {
        return fail(io, "too many operands");
    }
    current = umask(0);
    (void)umask(current);
    if (operand == NULL) {
        return report_mask(current, symbolic, io);
    }
    if (parse_octal(operand, &changed) == -1 &&
        parse_symbolic(operand, current, &changed) == -1) {
        return fail(io, "invalid mask");
    }
    (void)umask(changed);
    return 0;
}
