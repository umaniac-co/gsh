#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "builtin_cd.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int cd_error(const gsh_builtin_io *io, const char *path,
                    const char *message)
{
    if (io == NULL || message == NULL) {
        return -1;
    }
    (void)gsh_builtin_output(io, STDERR_FILENO, "gsh: cd: ", 9);
    if (path != NULL) {
        (void)gsh_builtin_output(io, STDERR_FILENO, path, strlen(path));
        (void)gsh_builtin_output(io, STDERR_FILENO, ": ", 2);
    }
    (void)gsh_builtin_output(io, STDERR_FILENO, message, strlen(message));
    (void)gsh_builtin_output(io, STDERR_FILENO, "\n", 1);
    return 1;
}

static int set_directory_variable(
    gsh_variable_store *variables, gsh_variable_journal *journal,
    const char *name, size_t name_length, const char *value,
    unsigned int attributes)
{
    if (value == NULL) {
        return -1;
    }
    size_t value_length = strlen(value);

    if (gsh_variables_set(variables, name, name_length, value,
                          value_length, attributes, attributes) == -1 ||
        (journal != NULL &&
         gsh_variable_journal_record(journal, name, name_length, value,
                                     value_length, attributes,
                                     attributes) == -1)) {
        return -1;
    }
    return 0;
}

static int rollback_directory(const gsh_builtin_io *io, int descriptor,
                              int operation_error)
{
    if (io == NULL) {
        return -1;
    }
    if (fchdir(descriptor) == -1) {
        int rollback_error = errno;

        (void)close(descriptor);
        (void)cd_error(io, "rollback", strerror(rollback_error));
        return 125;
    }
    (void)close(descriptor);
    return cd_error(io, NULL, strerror(operation_error));
}

int gsh_builtin_cd(size_t argc, char *const argv[],
                   const gsh_variable_store *lookup_variables,
                   gsh_variable_store *variables,
                   gsh_variable_journal *journal,
                   unsigned int assignment_attributes,
                   const gsh_builtin_io *io, char *directory,
                   size_t directory_capacity)
{
    if (argc == 0U || argv == NULL || io == NULL ||
        lookup_variables == NULL) {
        return -1;
    }
    const char *argument;
    const char *destination;
    char old_directory[PATH_MAX];
    char new_directory[PATH_MAX];
    size_t index = 1U;
    bool found;
    int previous_directory;

    if (index < argc && strcmp(argv[index], "--") == 0) index++;
    if (argc - index > 1U) {
        return cd_error(io, NULL, "too many operands");
    }
    argument = index == argc ? "" : argv[index];
    destination = argument;
    if (*destination == '\0' || strcmp(destination, "~") == 0) {
        destination = gsh_variables_lookup(lookup_variables, "HOME", 4,
                                           &found);
        if (!found) {
            return cd_error(io, NULL, "HOME is not set");
        }
    } else if (strcmp(destination, "-") == 0) {
        destination = gsh_variables_lookup(lookup_variables, "OLDPWD", 6,
                                           &found);
        if (!found) {
            return cd_error(io, NULL, "OLDPWD is not set");
        }
    }
    previous_directory = open(".", O_RDONLY);
    if (previous_directory == -1) {
        return cd_error(io, NULL, strerror(errno));
    }
    if (getcwd(old_directory, sizeof(old_directory)) == NULL) {
        int saved_errno = errno;

        (void)close(previous_directory);
        return cd_error(io, NULL, strerror(saved_errno));
    }
    if (chdir(destination) == -1) {
        int saved_errno = errno;

        (void)close(previous_directory);
        return cd_error(io, destination, strerror(saved_errno));
    }
    if (getcwd(new_directory, sizeof(new_directory)) == NULL) {
        int saved_errno = errno;

        return rollback_directory(io, previous_directory, saved_errno);
    }
    if (directory != NULL && strlen(new_directory) >= directory_capacity) {
        return rollback_directory(io, previous_directory, ENAMETOOLONG);
    }
    if (set_directory_variable(variables, journal, "OLDPWD", 6,
                               old_directory,
                               assignment_attributes) == -1 ||
        set_directory_variable(variables, journal, "PWD", 3,
                               new_directory,
                               assignment_attributes) == -1) {
        int saved_errno = errno;

        return rollback_directory(io, previous_directory, saved_errno);
    }
    if (directory != NULL) {
        size_t length = strlen(new_directory);

        (void)memcpy(directory, new_directory, length + 1U);
    }
    (void)close(previous_directory);
    if (strcmp(argument, "-") == 0 &&
        (gsh_builtin_output(io, STDOUT_FILENO, new_directory,
                    strlen(new_directory)) != 0 ||
         gsh_builtin_output(io, STDOUT_FILENO, "\n", 1) != 0)) {
        return 1;
    }
    return 0;
}
