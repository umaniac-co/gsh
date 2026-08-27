#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh variable tests require the POSIX.1-2024 baseline"
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/shell_variables.h"

static int expect_value(const gsh_variable_store *store, const char *name,
                        const char *expected)
{
    bool found;
    const char *value =
        gsh_variables_lookup(store, name, strlen(name), &found);

    if (!found || strcmp(value, expected) != 0) {
        fprintf(stderr, "variables: %s: expected <%s>, got <%s>\n", name,
                expected, found ? value : "unset");
        return -1;
    }
    return 0;
}

static bool is_unset(const gsh_variable_store *store, const char *name)
{
    bool found;

    (void)gsh_variables_lookup(store, name, strlen(name), &found);
    return !found;
}

static int expect_state(const gsh_variable_store *store, const char *name,
                        bool set, unsigned int attributes)
{
    size_t index;

    for (index = 0; index < gsh_variables_count(store); index++) {
        unsigned int actual;
        const char *assignment =
            gsh_variables_assignment(store, index, &actual);
        const char *separator = strchr(assignment, '=');

        if (separator != NULL &&
            (size_t)(separator - assignment) == strlen(name) &&
            memcmp(assignment, name, strlen(name)) == 0) {
            if (gsh_variables_is_set(store, index) != set ||
                actual != attributes) {
                fprintf(stderr, "variables: state mismatch for %s\n", name);
                return -1;
            }
            return 0;
        }
    }
    fprintf(stderr, "variables: state missing for %s\n", name);
    return -1;
}

int main(void)
{
    char *environment[] = {(char *)"ALPHA=one", (char *)"PATH=/bin",
                           (char *)"INVALID-NAME=ignored",
                           (char *)"IFS=:", NULL};
    gsh_variable_store *store = malloc(sizeof(*store));
    gsh_variable_store *scratch = malloc(sizeof(*scratch));
    gsh_variable_journal *journal = malloc(sizeof(*journal));
    gsh_variable_journal_value_state value_state;
    unsigned int attributes = 0;
    const char *assignment;
    bool failed = false;

    if (store == NULL || scratch == NULL || journal == NULL ||
        gsh_variables_import(store, environment) == -1 ||
        expect_value(store, "ALPHA", "one") == -1 ||
        expect_value(store, "IFS", " \t\n") == -1 ||
        !is_unset(store, "INVALID-NAME")) {
        failed = true;
        goto done;
    }
    assignment = gsh_variables_assignment(store, 0, &attributes);
    if (assignment == NULL ||
        (attributes & GSH_VARIABLE_EXPORTED) == 0) {
        failed = true;
        goto done;
    }
    if (gsh_variables_set(store, "X", 1, "short", 5, 0, 0) == -1 ||
        gsh_variables_set(store, "Y", 1, "stable", 6, 0, 0) == -1 ||
        gsh_variables_set(store, "X", 1, "a-longer-value", 14, 0, 0) ==
            -1 ||
        expect_value(store, "X", "a-longer-value") == -1 ||
        expect_value(store, "Y", "stable") == -1 ||
        gsh_variables_set(store, "X", 1, "z", 1, 0, 0) == -1 ||
        expect_value(store, "X", "z") == -1 ||
        expect_value(store, "Y", "stable") == -1) {
        failed = true;
        goto done;
    }

    gsh_variable_journal_initialize(journal, 7);
    if (gsh_variable_journal_record(journal, "X", 1, "first", 5, 0, 0) ==
            -1 ||
        gsh_variable_journal_record(journal, "X", 1, "final", 5, 0, 0) ==
            -1 ||
        gsh_variable_journal_record(journal, "NEW", 3, "created", 7, 0,
                                    0) == -1 ||
        journal->count != 2 || !gsh_variable_journal_validate(journal) ||
        gsh_variables_apply_journal(store, journal, scratch) == -1 ||
        expect_value(store, "X", "final") == -1 ||
        expect_value(store, "NEW", "created") == -1) {
        failed = true;
        goto done;
    }

    gsh_variable_journal_initialize(journal, 8);
    if (gsh_variable_journal_record_scoped(
            journal, 1, "X", 1, "stage-one", 9, 0, 0) == -1 ||
        gsh_variable_journal_record_scoped(
            journal, 2, "X", 1, "stage-two", 9, 0, 0) == -1 ||
        journal->count != 2 || !gsh_variable_journal_validate(journal)) {
        failed = true;
        goto done;
    }
    memcpy(scratch, store, sizeof(*scratch));
    if (gsh_variables_apply_journal_scope_in_place(scratch, journal, 1) ==
            -1 ||
        expect_value(scratch, "X", "stage-one") == -1 ||
        expect_value(store, "X", "final") == -1) {
        failed = true;
        goto done;
    }
    memcpy(scratch, store, sizeof(*scratch));
    if (gsh_variables_apply_journal_scope_in_place(scratch, journal, 2) ==
            -1 ||
        expect_value(scratch, "X", "stage-two") == -1 ||
        gsh_variables_apply_journal_in_place(store, journal) != -1 ||
        errno != EPROTO || expect_value(store, "X", "final") == -1) {
        failed = true;
        goto done;
    }

    if (gsh_variables_set(store, "LOCKED", 6, "fixed", 5,
                          GSH_VARIABLE_READONLY,
                          GSH_VARIABLE_READONLY) == -1) {
        failed = true;
        goto done;
    }
    if (gsh_variables_set(store, "JATTR", 5, "kept", 4, 0, 0) == -1) {
        failed = true;
        goto done;
    }
    gsh_variable_journal_initialize(journal, 9);
    if (gsh_variable_journal_record_attributes(
            journal, "JATTR", 5, GSH_VARIABLE_EXPORTED,
            GSH_VARIABLE_EXPORTED) == -1 ||
        gsh_variable_journal_lookup(journal, "JATTR", 5, &value_state) ==
            NULL ||
        value_state != GSH_VARIABLE_JOURNAL_VALUE_ABSENT ||
        gsh_variables_apply_journal(store, journal, scratch) == -1 ||
        expect_value(store, "JATTR", "kept") == -1 ||
        expect_state(store, "JATTR", true, GSH_VARIABLE_EXPORTED) == -1) {
        failed = true;
        goto done;
    }
    gsh_variable_journal_initialize(journal, 10);
    if (gsh_variable_journal_record_unset(journal, "JATTR", 5) == -1 ||
        gsh_variable_journal_record_attributes(
            journal, "JATTR", 5, GSH_VARIABLE_EXPORTED,
            GSH_VARIABLE_EXPORTED) == -1 ||
        gsh_variable_journal_lookup(journal, "JATTR", 5, &value_state) ==
            NULL ||
        value_state != GSH_VARIABLE_JOURNAL_VALUE_UNSET ||
        gsh_variables_apply_journal(store, journal, scratch) == -1 ||
        expect_state(store, "JATTR", false, GSH_VARIABLE_EXPORTED) == -1) {
        failed = true;
        goto done;
    }
    gsh_variable_journal_initialize(journal, 11);
    if (gsh_variable_journal_record(journal, "JATTR", 5, "again", 5, 0,
                                    0) == -1 ||
        gsh_variables_apply_journal(store, journal, scratch) == -1 ||
        expect_value(store, "JATTR", "again") == -1 ||
        expect_state(store, "JATTR", true, GSH_VARIABLE_EXPORTED) == -1) {
        failed = true;
        goto done;
    }
    gsh_variable_journal_initialize(journal, 12);
    errno = 0;
    if (gsh_variable_journal_record(journal, "SEALED", 6, "first", 5, 0,
                                    0) == -1 ||
        gsh_variable_journal_record_attributes(
            journal, "SEALED", 6, GSH_VARIABLE_READONLY,
            GSH_VARIABLE_READONLY) == -1 ||
        gsh_variable_journal_record(journal, "SEALED", 6, "second", 6, 0,
                                    0) != -1 ||
        errno != EROFS ||
        gsh_variables_apply_journal(store, journal, scratch) == -1 ||
        expect_value(store, "SEALED", "first") == -1 ||
        expect_state(store, "SEALED", true, GSH_VARIABLE_READONLY) == -1) {
        failed = true;
        goto done;
    }
    if (gsh_variables_set_attributes(store, "DECLARED", 8,
                                     GSH_VARIABLE_EXPORTED,
                                     GSH_VARIABLE_EXPORTED) == -1 ||
        !is_unset(store, "DECLARED") ||
        expect_state(store, "DECLARED", false,
                     GSH_VARIABLE_EXPORTED) == -1 ||
        gsh_variables_set(store, "DECLARED", 8, "now-set", 7, 0, 0) ==
            -1 ||
        expect_value(store, "DECLARED", "now-set") == -1 ||
        expect_state(store, "DECLARED", true,
                     GSH_VARIABLE_EXPORTED) == -1 ||
        gsh_variables_set_attributes(store, "LOCKED", 6,
                                     GSH_VARIABLE_EXPORTED,
                                     GSH_VARIABLE_EXPORTED) == -1 ||
        expect_state(store, "LOCKED", true,
                     GSH_VARIABLE_EXPORTED | GSH_VARIABLE_READONLY) == -1) {
        failed = true;
        goto done;
    }
    if (gsh_variables_set(store, "REMOVE", 6, "value", 5,
                          GSH_VARIABLE_EXPORTED,
                          GSH_VARIABLE_EXPORTED) == -1 ||
        gsh_variables_unset(store, "REMOVE", 6) == -1 ||
        !is_unset(store, "REMOVE") ||
        gsh_variables_unset(store, "REMOVE", 6) == -1 ||
        expect_value(store, "DECLARED", "now-set") == -1) {
        failed = true;
        goto done;
    }
    errno = 0;
    if (gsh_variables_unset(store, "LOCKED", 6) != -1 ||
        errno != EROFS || expect_value(store, "LOCKED", "fixed") == -1) {
        failed = true;
        goto done;
    }
    errno = 0;
    if (gsh_variables_set(store, "LOCKED", 6, "changed", 7, 0, 0) != -1 ||
        errno != EROFS) {
        failed = true;
        goto done;
    }
    gsh_variable_journal_initialize(journal, 13);
    if (gsh_variable_journal_record(journal, "ATOMIC", 6, "must-not-land",
                                    13, 0, 0) == -1 ||
        gsh_variable_journal_record(journal, "LOCKED", 6, "changed", 7, 0,
                                    0) == -1 ||
        gsh_variables_apply_journal(store, journal, scratch) != -1 ||
        !is_unset(store, "ATOMIC") ||
        expect_value(store, "LOCKED", "fixed") == -1) {
        failed = true;
        goto done;
    }
    journal->version++;
    if (gsh_variable_journal_validate(journal)) {
        failed = true;
    }

done:
    free(store);
    free(scratch);
    free(journal);
    if (failed) {
        fputs("variables: invariant test failed\n", stderr);
        return 1;
    }
    puts("variables: hash arena, attributes, and atomic journal passed");
    return 0;
}
