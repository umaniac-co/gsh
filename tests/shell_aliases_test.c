#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh alias tests require the POSIX.1-2024 baseline"
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/alias_expansion.h"
#include "../src/shell_aliases.h"

static int expect_alias(const gsh_alias_store *store, const char *name,
                        const char *expected)
{
    const char *value = gsh_aliases_lookup(store, name, strlen(name), NULL);

    if (value == NULL || strcmp(value, expected) != 0) {
        fprintf(stderr, "aliases: %s: expected <%s>, got <%s>\n", name,
                expected, value == NULL ? "undefined" : value);
        return -1;
    }
    return 0;
}

static int expect_parse(const char *input, const gsh_alias_store *aliases,
                        gsh_parse_status expected)
{
    gsh_parse_storage *storage = malloc(sizeof(*storage));
    char *expanded = malloc(GSH_ALIAS_EXPANSION_CAP);
    const char *parsed_input;
    size_t parsed_length;
    gsh_parse_result result;

    if (storage == NULL || expanded == NULL) {
        free(storage);
        free(expanded);
        return -1;
    }
    result = gsh_alias_parse(input, strlen(input), aliases, expanded,
                             GSH_ALIAS_EXPANSION_CAP, storage,
                             &parsed_input, &parsed_length);
    (void)parsed_input;
    (void)parsed_length;
    free(storage);
    free(expanded);
    if (result.status != expected) {
        fprintf(stderr, "aliases: parse <%s>: expected %s, got %s\n", input,
                gsh_parse_status_name(expected),
                gsh_parse_status_name(result.status));
        return -1;
    }
    return 0;
}

static int expect_expansion(const char *input,
                            const gsh_alias_store *aliases,
                            const char *expected)
{
    gsh_parse_storage *storage = malloc(sizeof(*storage));
    char *expanded = malloc(GSH_ALIAS_EXPANSION_CAP);
    const char *parsed_input;
    size_t parsed_length;
    gsh_parse_result result;
    int status = 0;

    if (storage == NULL || expanded == NULL) {
        free(storage);
        free(expanded);
        return -1;
    }
    result = gsh_alias_parse(input, strlen(input), aliases, expanded,
                             GSH_ALIAS_EXPANSION_CAP, storage,
                             &parsed_input, &parsed_length);
    if (result.status != GSH_PARSE_OK ||
        parsed_length != strlen(expected) ||
        memcmp(parsed_input, expected, parsed_length) != 0) {
        fprintf(stderr, "aliases: expand <%s>: expected <%s>, got <%.*s>\n",
                input, expected, (int)parsed_length, parsed_input);
        status = -1;
    }
    free(storage);
    free(expanded);
    return status;
}

int main(void)
{
    gsh_alias_store *store = malloc(sizeof(*store));
    gsh_alias_store *scratch = malloc(sizeof(*scratch));
    gsh_alias_journal *journal = malloc(sizeof(*journal));
    bool failed = false;

    if (store == NULL || scratch == NULL || journal == NULL) {
        failed = true;
        goto done;
    }
    gsh_aliases_initialize(store);
    if (!gsh_alias_name_is_valid("A0!%,-@_", 8) ||
        gsh_alias_name_is_valid("", 0) ||
        gsh_alias_name_is_valid("bad/name", 8) ||
        gsh_aliases_set(store, "ll", 2, "printf '%s\\n'", 13) == -1 ||
        gsh_aliases_set(store, "ll", 2, "echo", 4) == -1 ||
        gsh_aliases_set(store, "keep", 4, "stable", 6) == -1 ||
        expect_alias(store, "ll", "echo") == -1 ||
        expect_alias(store, "keep", "stable") == -1 ||
        gsh_aliases_unset(store, "ll", 2) == -1 ||
        gsh_aliases_lookup(store, "ll", 2, NULL) != NULL ||
        expect_alias(store, "keep", "stable") == -1) {
        failed = true;
        goto done;
    }

    gsh_alias_journal_initialize(journal, 7);
    if (gsh_alias_journal_record_set(journal, "one", 3, "echo 1", 6) ==
            -1 ||
        gsh_alias_journal_record_set(journal, "two", 3, "echo 2", 6) ==
            -1 ||
        gsh_alias_journal_record_unset(journal, "keep", 4) == -1 ||
        !gsh_alias_journal_validate(journal) ||
        gsh_aliases_apply_journal(store, journal, scratch) == -1 ||
        expect_alias(store, "one", "echo 1") == -1 ||
        expect_alias(store, "two", "echo 2") == -1 ||
        gsh_aliases_lookup(store, "keep", 4, NULL) != NULL) {
        failed = true;
        goto done;
    }
    gsh_alias_journal_initialize(journal, 8);
    if (gsh_alias_journal_record_clear(journal) == -1 ||
        gsh_alias_journal_record_set(journal, "after", 5, "true", 4) ==
            -1 ||
        gsh_aliases_apply_journal(store, journal, scratch) == -1 ||
        gsh_aliases_count(store) != 1 ||
        expect_alias(store, "after", "true") == -1) {
        failed = true;
        goto done;
    }
    journal->entries[0].reserved = 1;
    if (gsh_alias_journal_validate(journal)) {
        failed = true;
        goto done;
    }

    gsh_aliases_clear(store);
    if (gsh_aliases_set(store, "say", 3, "echo", 4) == -1 ||
        gsh_aliases_set(store, "self", 4, "self -n", 7) == -1 ||
        gsh_aliases_set(store, "a", 1, "b", 1) == -1 ||
        gsh_aliases_set(store, "b", 1, "a", 1) == -1 ||
        gsh_aliases_set(store, "prefix", 6, "prefix ", 7) == -1 ||
        gsh_aliases_set(store, "target", 6, "echo target", 11) == -1 ||
        gsh_aliases_set(store, "cmd", 3, "command ", 8) == -1 ||
        gsh_aliases_set(store, "branch", 6, "if true; then", 13) == -1 ||
        gsh_aliases_set(store, "if", 2, "echo reserved", 13) == -1 ||
        expect_parse("say ok", store, GSH_PARSE_OK) == -1 ||
        expect_parse("X=1 say ok", store, GSH_PARSE_OK) == -1 ||
        expect_parse(">out say ok", store, GSH_PARSE_OK) == -1 ||
        expect_parse("'say' ok", store, GSH_PARSE_OK) == -1 ||
        expect_parse("self value", store, GSH_PARSE_OK) == -1 ||
        expect_parse("a value", store, GSH_PARSE_OK) == -1 ||
        expect_parse("prefix target", store, GSH_PARSE_OK) == -1 ||
        expect_parse("branch echo ok; fi", store, GSH_PARSE_OK) == -1 ||
        expect_parse("if true; then true; fi", store, GSH_PARSE_OK) == -1) {
        failed = true;
        goto done;
    }
    if (expect_expansion("say ok", store, "echo  ok") == -1 ||
        expect_expansion("X=1 say ok", store, "X=1 echo  ok") == -1 ||
        expect_expansion(">out say ok", store, ">out echo  ok") == -1 ||
        expect_expansion("'say' ok", store, "'say' ok") == -1 ||
        expect_expansion("command say", store, "command say") == -1 ||
        expect_expansion("prefix target", store,
                         "prefix   echo target ") == -1 ||
        expect_expansion("cmd say", store, "command   say") == -1) {
        failed = true;
        goto done;
    }

    gsh_aliases_clear(store);
    {
        size_t index;
        char name[16];

        for (index = 0; index < GSH_ALIAS_CAP; index++) {
            int length = snprintf(name, sizeof(name), "n%zu", index);

            if (length <= 0 || (size_t)length >= sizeof(name) ||
                gsh_aliases_set(store, name, (size_t)length, "x", 1) ==
                    -1) {
                failed = true;
                goto done;
            }
        }
        errno = 0;
        if (gsh_aliases_set(store, "overflow", 8, "x", 1) != -1 ||
            errno != ENOSPC || gsh_aliases_count(store) != GSH_ALIAS_CAP) {
            failed = true;
            goto done;
        }
    }
    gsh_alias_journal_initialize(journal, 9);
    {
        size_t index;

        for (index = 0; index < GSH_ALIAS_JOURNAL_CAP; index++) {
            if (gsh_alias_journal_record_set(journal, "same", 4, "x", 1) ==
                -1) {
                failed = true;
                goto done;
            }
        }
        errno = 0;
        if (gsh_alias_journal_record_set(journal, "same", 4, "x", 1) !=
                -1 ||
            errno != ENOSPC || !gsh_alias_journal_validate(journal)) {
            failed = true;
            goto done;
        }
    }
    gsh_aliases_clear(store);
    {
        enum { LARGE_VALUE = 15000 };
        char *value = malloc(LARGE_VALUE + 1U);

        if (value == NULL) {
            failed = true;
            goto done;
        }
        memcpy(value, "/usr/bin/true ", 14);
        memset(value + 14, 'x', LARGE_VALUE - 15U);
        value[LARGE_VALUE - 1U] = ' ';
        value[LARGE_VALUE] = '\0';
        if (gsh_aliases_set(store, "p", 1, value, LARGE_VALUE) == -1) {
            failed = true;
            free(value);
            goto done;
        }
        free(value);
        if (expect_parse("p p p p p", store, GSH_PARSE_LIMIT) == -1) {
            failed = true;
            goto done;
        }
    }

done:
    free(store);
    free(scratch);
    free(journal);
    if (failed) {
        return 1;
    }
    puts("aliases: hash arena, recursive parser rewrite, and atomic journal passed");
    return 0;
}
