#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "../src/shell_config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h> /* CANON-INCLUDE: linux */
#include <string.h>
#include <unistd.h>

static int write_configuration(const char *home, const char *text)
{
    if (text == NULL) {
        return -1;
    }
    char path[4096];
    size_t length = strlen(text);
    size_t offset = 0;
    int descriptor;

    if (snprintf(path, sizeof(path), "%s/.gshrc", home) >=
        (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor == -1) {
        return -1;
    }
    while (offset < length) {
        ssize_t count = write(descriptor, text + offset, length - offset);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            (void)close(descriptor);
            return -1;
        }
    }
    return close(descriptor);
}

static bool initial_file_has_async_default(const char *home)
{
    static const char expected[] = "shell.async_repl.enabled = true";
    char path[4096];
    char contents[4096];
    ssize_t count;
    int descriptor;

    if (snprintf(path, sizeof(path), "%s/.gshrc", home) >=
        (int)sizeof(path)) {
        return false;
    }
    descriptor = open(path, O_RDONLY);
    if (descriptor == -1) {
        return false;
    }
    count = read(descriptor, contents, sizeof(contents) - 1U);
    (void)close(descriptor);
    if (count < 0) {
        return false;
    }
    contents[(size_t)count] = '\0';
    return strstr(contents, expected) != NULL;
}

static bool initial_file_has_action_defaults(const char *home)
{
    static const char actions[] = "terminal.actions = auto";
    static const char detection[] = "terminal.actions.path_detection = safe";
    static const char editor[] = "shell.preview.editor = auto";
    char path[4096];
    char contents[4096];
    ssize_t count;
    int descriptor;
    if (home == NULL || snprintf(path, sizeof(path), "%s/.gshrc", home) >=
                            (int)sizeof(path)) return false;
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return false;
    count = read(descriptor, contents, sizeof(contents) - 1U);
    (void)close(descriptor);
    if (count < 0) return false;
    contents[(size_t)count] = '\0';
    return strstr(contents, actions) != NULL &&
           strstr(contents, detection) != NULL &&
           strstr(contents, editor) != NULL;
}

static int action_configuration_cases(const char *home,
                                      gsh_shell_config *config)
{
    static const char explicit[] =
        "config.version = 1\n"
        "terminal.actions = off\n"
        "terminal.actions.path_detection = known\n"
        "shell.preview.editor = [\"nvim\", \"--\", \"{file}\"]\n";
    static const char invalid[] =
        "config.version = 1\n"
        "shell.preview.editor = [\"vim\", \"--\"]\n";
    if (home == NULL || config == NULL) return 1;
    if (write_configuration(home, explicit) == -1 ||
        gsh_config_load(config, home, false) == -1 ||
        config->terminal_actions != GSH_TERMINAL_ACTIONS_OFF ||
        config->path_detection != GSH_PATH_DETECTION_KNOWN ||
        config->preview_editor_auto || config->preview_editor_argc != 3U ||
        strcmp(gsh_config_editor_argument(config, 0U), "nvim") != 0 ||
        strcmp(gsh_config_editor_argument(config, 2U), "{file}") != 0) return 1;
    if (write_configuration(home, invalid) == -1 ||
        gsh_config_load(config, home, false) != -1) return 1;
    return 0;
}

int main(void)
{
    static const char absent[] =
        "config.version = 1\n"
        "shell.history.enabled = false\n";
    static const char disabled[] =
        "config.version = 1\n"
        "shell.async_repl.enabled = false\n"
        "shell.history.enabled = false\n";
    static const char enabled[] =
        "config.version = 1\n"
        "shell.async_repl.enabled = true\n"
        "shell.history.enabled = false\n";
    static const char invalid[] =
        "config.version = 1\n"
        "shell.async_repl.enabled = yes\n"
        "shell.history.enabled = false\n";
    char home[] = "/tmp/gsh-config-XXXXXX";
    char path[4096];
    gsh_shell_config config;
    int failed = 0;

    if (mkdtemp(home) == NULL) {
        perror("shell config: fixture");
        return 1;
    }
    gsh_config_defaults(&config);
    if (!config.async_repl_enabled ||
        gsh_config_load(&config, home, true) == -1 ||
        !config.async_repl_enabled ||
        config.terminal_actions != GSH_TERMINAL_ACTIONS_AUTO ||
        config.path_detection != GSH_PATH_DETECTION_SAFE ||
        !config.preview_editor_auto ||
        !initial_file_has_async_default(home) ||
        !initial_file_has_action_defaults(home)) {
        failed = 1;
    }
    if (!failed &&
        (write_configuration(home, absent) == -1 ||
         gsh_config_load(&config, home, false) == -1 ||
         !config.async_repl_enabled)) {
        failed = 1;
    }
    if (!failed &&
        (write_configuration(home, disabled) == -1 ||
         gsh_config_load(&config, home, false) == -1 ||
         config.async_repl_enabled)) {
        failed = 1;
    }
    if (!failed &&
        (write_configuration(home, enabled) == -1 ||
         gsh_config_load(&config, home, false) == -1 ||
         !config.async_repl_enabled)) {
        failed = 1;
    }
    config.async_repl_enabled = false;
    if (!failed &&
        (write_configuration(home, invalid) == -1 ||
         gsh_config_load(&config, home, false) != -1 ||
         config.async_repl_enabled ||
         strstr(config.diagnostic, "invalid configuration") == NULL)) {
        failed = 1;
    }
    if (!failed && action_configuration_cases(home, &config) != 0) failed = 1;
    if (snprintf(path, sizeof(path), "%s/.gshrc", home) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
    (void)rmdir(home);
    if (failed) {
        (void)fprintf(stderr, "shell config: async repl cases failed\n");
        return 1;
    }
    (void)puts("shell config: async repl defaults and validation passed");
    return 0;
}
