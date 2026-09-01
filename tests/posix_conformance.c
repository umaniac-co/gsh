#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh conformance tests require the POSIX.1-2024 baseline"
#endif

#include "../src/source_workspace.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *section;
    const char *name;
    const char *command;
    int status;
    const char *diagnostic;
} syntax_case;

static int build_nested_substitution(char *command, size_t capacity,
                                     size_t depth)
{
    static const char prefix[] = "/usr/bin/printf '<%s>\\n' \"";
    static const char opening[] = "$(/usr/bin/printf '%s' \"";
    static const char closing[] = "\")";
    size_t used;
    size_t index;

    if (command == NULL || depth > GSH_SOURCE_DEPTH_CAP + 1U ||
        sizeof(prefix) > capacity) {
        return -1;
    }
    memcpy(command, prefix, sizeof(prefix));
    used = sizeof(prefix) - 1U;
    for (index = 0; index < GSH_SOURCE_DEPTH_CAP + 1U && index < depth;
         index++) {
        if (sizeof(opening) - 1U > capacity - used - 1U) {
            return -1;
        }
        memcpy(command + used, opening, sizeof(opening) - 1U);
        used += sizeof(opening) - 1U;
    }
    if (used + 1U >= capacity) {
        return -1;
    }
    command[used++] = 'x';
    for (index = 0; index < GSH_SOURCE_DEPTH_CAP + 1U && index < depth;
         index++) {
        if (sizeof(closing) - 1U > capacity - used - 1U) {
            return -1;
        }
        memcpy(command + used, closing, sizeof(closing) - 1U);
        used += sizeof(closing) - 1U;
    }
    if (used + 1U >= capacity) {
        return -1;
    }
    command[used++] = '"';
    command[used] = '\0';
    assert(used < capacity);
    assert(index == depth);
    return 0;
}

static int build_nested_eval(char *command, size_t capacity,
                             size_t final_index)
{
    size_t used = 0;
    size_t index;
    int length;

    length = snprintf(command, capacity, "GSH_EVAL_DEPTH_0=:");
    if (length < 0 || (size_t)length >= capacity) {
        return -1;
    }
    used = (size_t)length;
    for (index = 1U; index <= final_index; index++) {
        length = snprintf(
            command + used, capacity - used,
            "; GSH_EVAL_DEPTH_%zu='eval \"$GSH_EVAL_DEPTH_%zu\"'",
            index, index - 1U);
        if (length < 0 || (size_t)length >= capacity - used) {
            return -1;
        }
        used += (size_t)length;
    }
    length = snprintf(command + used, capacity - used,
                      "; eval \"$GSH_EVAL_DEPTH_%zu\"", final_index);
    return length < 0 || (size_t)length >= capacity - used ? -1 : 0;
}

static int configure_utf8_locale(void)
{
    const char *name;

    if (setlocale(LC_ALL, "C.UTF-8") != NULL) {
        name = "C.UTF-8";
    } else if (setlocale(LC_ALL, "en_US.UTF-8") != NULL) {
        name = "en_US.UTF-8";
    } else {
        errno = EINVAL;
        return -1;
    }
    return setenv("LC_ALL", name, 1);
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static bool bytes_contain(const char *data, size_t length, const char *needle)
{
    size_t needle_length = strlen(needle);
    size_t index;

    if (needle_length == 0) {
        return true;
    }
    for (index = 0; index + needle_length <= length; index++) {
        if (memcmp(data + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static int run_case_arguments(
    const char *executable, const syntax_case *test, bool syntax_only,
    const char *parameter_zero, const char *const *positionals,
    size_t positional_count)
{
    char diagnostic[65536];
    char discarded[4096];
    size_t diagnostic_length = 0;
    uint64_t deadline;
    int descriptors[2];
    int flags;
    int status = 0;
    pid_t pid;

    if (positional_count > 128U || pipe(descriptors) == -1) {
        return -1;
    }
    pid = fork();
    if (pid == 0) {
        char *arguments[134];
        size_t argument_count = 0;
        size_t index;

        arguments[argument_count++] = (char *)executable;
        if (syntax_only) {
            arguments[argument_count++] = (char *)"-n";
        } else {
            arguments[argument_count++] = (char *)"--native-only";
        }
        arguments[argument_count++] = (char *)"-c";
        arguments[argument_count++] = (char *)test->command;
        if (!syntax_only && parameter_zero != NULL) {
            arguments[argument_count++] = (char *)parameter_zero;
            for (index = 0; index < positional_count; index++) {
                arguments[argument_count++] = (char *)positionals[index];
            }
        }
        arguments[argument_count] = NULL;

        close(descriptors[0]);
        if (dup2(descriptors[1], STDERR_FILENO) == -1 ||
            dup2(descriptors[1], STDOUT_FILENO) == -1) {
            _exit(126);
        }
        close(descriptors[1]);
        execv(executable, arguments);
        _exit(127);
    }
    close(descriptors[1]);
    if (pid == -1) {
        close(descriptors[0]);
        return -1;
    }
    flags = fcntl(descriptors[0], F_GETFL);
    if (flags == -1 ||
        fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close(descriptors[0]);
        return -1;
    }

    deadline = monotonic_ns() + 2000000000ULL;
    for (;;) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        ssize_t count;

        do {
            char *destination = diagnostic_length < sizeof(diagnostic)
                                    ? diagnostic + diagnostic_length
                                    : discarded;
            size_t capacity = diagnostic_length < sizeof(diagnostic)
                                  ? sizeof(diagnostic) - diagnostic_length
                                  : sizeof(discarded);

            count = read(descriptors[0], destination, capacity);
            if (count > 0) {
                if (destination != discarded) {
                    diagnostic_length += (size_t)count;
                }
            }
        } while (count > 0);
        if (waited == pid) {
            break;
        }
        if (waited == -1 && errno != EINTR) {
            close(descriptors[0]);
            return -1;
        }
        if (monotonic_ns() >= deadline) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(descriptors[0]);
            fprintf(stderr, "conformance: timeout: %s\n", test->name);
            return 1;
        }
        {
            struct pollfd descriptor = {descriptors[0], POLLIN, 0};

            (void)poll(&descriptor, 1, 10);
        }
    }
    for (;;) {
        char *destination = diagnostic_length < sizeof(diagnostic)
                                ? diagnostic + diagnostic_length
                                : discarded;
        size_t capacity = diagnostic_length < sizeof(diagnostic)
                              ? sizeof(diagnostic) - diagnostic_length
                              : sizeof(discarded);
        ssize_t count = read(descriptors[0], destination, capacity);

        if (count > 0) {
            if (destination != discarded) {
                diagnostic_length += (size_t)count;
            }
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(descriptors[0]);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != test->status ||
        (test->diagnostic != NULL &&
         !bytes_contain(diagnostic, diagnostic_length, test->diagnostic))) {
        fprintf(stderr,
                "conformance: %s (%s): expected status %d and diagnostic "
                "%s\n",
                test->name, test->section, test->status,
                test->diagnostic != NULL ? test->diagnostic : "<none>");
        if (diagnostic_length > 0) {
            size_t safe_length = diagnostic_length < sizeof(diagnostic)
                                     ? diagnostic_length
                                     : sizeof(diagnostic);

            (void)fwrite(diagnostic, 1, safe_length, stderr);
        }
        return 1;
    }
    return 0;
}

static int run_case(const char *executable, const syntax_case *test,
                    bool syntax_only)
{
    return run_case_arguments(executable, test, syntax_only, NULL, NULL, 0);
}

static int no_execution_case(const char *executable)
{
    char directory[] = "/tmp/gsh-native-n-XXXXXX";
    char target[1024];
    char command[1200];
    syntax_case test = {"sh -n", "no execution", command, 0, NULL};
    int failed;

    if (mkdtemp(directory) == NULL ||
        snprintf(target, sizeof(target), "%s/effect", directory) >=
            (int)sizeof(target) ||
        snprintf(command, sizeof(command), ": > '%s'", target) >=
            (int)sizeof(command)) {
        return 1;
    }
    failed = run_case(executable, &test, true);
    if (access(target, F_OK) == 0 || errno != ENOENT) {
        fprintf(stderr, "conformance: -n executed a side effect\n");
        failed = 1;
        (void)unlink(target);
    }
    (void)rmdir(directory);
    return failed;
}

static int write_repeated_byte(int descriptor, unsigned char byte,
                               size_t count)
{
    unsigned char bytes[4096];
    size_t written = 0;

    memset(bytes, byte, sizeof(bytes));
    while (written < count) {
        size_t available = count - written;
        size_t request = available < sizeof(bytes) ? available
                                                   : sizeof(bytes);
        ssize_t result = write(descriptor, bytes, request);

        if (result > 0) {
            written += (size_t)result;
        } else if (result == -1 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

static int native_large_input_case(const char *executable,
                                   const char *directory)
{
    char path[1024];
    char command[4096];
    syntax_case test = {"sh/2.1", "streamed source exceeds one MiB",
                        command, 0, "STREAM_OK"};
    int descriptor;
    int failed = 0;

    if (snprintf(path, sizeof(path), "%s/large.sh", directory) >=
            (int)sizeof(path) ||
        snprintf(command, sizeof(command), "'%s' '%s'", executable,
                 path) >= (int)sizeof(command)) {
        return 1;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor == -1 || write(descriptor, "#", 1) != 1 ||
        write_repeated_byte(descriptor, 'x', 600000U) == -1 ||
        write(descriptor, "\n#", 2) != 2 ||
        write_repeated_byte(descriptor, 'y', 600000U) == -1 ||
        write(descriptor, "\n/usr/bin/printf STREAM_OK\n", 27) != 27) {
        failed = 1;
    }
    if (descriptor >= 0 && close(descriptor) == -1) {
        failed = 1;
    }
    if (!failed && run_case(executable, &test, false) != 0) {
        failed = 1;
    }
    (void)unlink(path);
    return failed;
}

static int native_seekable_input_case(const char *executable,
                                      const char *directory)
{
    static const char script[] = "/bin/cat\nseekable payload\n";
    char path[1024];
    char command[4096];
    syntax_case test = {
        "sh/2.1", "seekable standard input is rewound before execution",
        command, 0, "seekable payload\n"};
    int descriptor;
    int failed = 0;

    if (snprintf(path, sizeof(path), "%s/stdin.sh", directory) >=
            (int)sizeof(path) ||
        snprintf(command, sizeof(command), "'%s' -s < '%s'", executable,
                 path) >= (int)sizeof(command)) {
        return 1;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor == -1 ||
        write(descriptor, script, sizeof(script) - 1U) !=
            (ssize_t)(sizeof(script) - 1U)) {
        failed = 1;
    }
    if (descriptor >= 0 && close(descriptor) == -1) {
        failed = 1;
    }
    if (!failed && run_case(executable, &test, false) != 0) {
        failed = 1;
    }
    (void)unlink(path);
    return failed;
}

static int native_oversized_input_case(const char *executable)
{
    char directory[] = "/tmp/gsh-native-input-limit-XXXXXX";
    char path[1024];
    char command[4096];
    syntax_case test = {
        "sh/2.1", "single complete-command input limit", command, 2,
        "complete command exceeds input limit"};
    int descriptor = -1;
    int failed = 0;

    if (mkdtemp(directory) == NULL ||
        snprintf(path, sizeof(path), "%s/oversized.sh", directory) >=
            (int)sizeof(path) ||
        snprintf(command, sizeof(command), "'%s' '%s'", executable,
                 path) >= (int)sizeof(command)) {
        return 1;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor == -1 || write(descriptor, "#", 1) != 1 ||
        write_repeated_byte(descriptor, 'x', GSH_SOURCE_INPUT_CAP) == -1) {
        failed = 1;
    }
    if (descriptor >= 0 && close(descriptor) == -1) {
        failed = 1;
    }
    descriptor = -1;
    if (!failed && run_case(executable, &test, false) != 0) {
        failed = 1;
    }
    if (descriptor >= 0) {
        (void)close(descriptor);
    }
    (void)unlink(path);
    (void)rmdir(directory);
    return failed;
}

static int native_nonblocking_input_case(const char *executable)
{
    static const char script[] = "exit 0\n";
    int input[2];
    int flags;
    pid_t pid;
    uint64_t deadline;
    int status = 0;
    bool blocking = false;
    size_t attempts;

    if (pipe(input) == -1 ||
        (flags = fcntl(input[0], F_GETFL)) == -1 ||
        fcntl(input[0], F_SETFL, flags | O_NONBLOCK) == -1) {
        return 1;
    }
    pid = fork();
    if (pid == 0) {
        char *const arguments[] = {(char *)executable, "-s", NULL};

        close(input[1]);
        if (dup2(input[0], STDIN_FILENO) == -1) {
            _exit(126);
        }
        if (input[0] != STDIN_FILENO) {
            close(input[0]);
        }
        execv(executable, arguments);
        _exit(127);
    }
    if (pid == -1) {
        close(input[1]);
        close(input[0]);
        return 1;
    }
    deadline = monotonic_ns() + 2000000000ULL;
    while (monotonic_ns() < deadline) {
        flags = fcntl(input[0], F_GETFL);
        if (flags != -1 && (flags & O_NONBLOCK) == 0) {
            blocking = true;
            break;
        }
        (void)poll(NULL, 0, 1);
    }
    if (blocking &&
        write(input[1], script, sizeof(script) - 1U) !=
            (ssize_t)(sizeof(script) - 1U)) {
        blocking = false;
    }
    close(input[1]);
    close(input[0]);
    if (!blocking) {
        (void)kill(pid, SIGKILL);
    }
    for (attempts = 0; attempts < 1024U; attempts++) {
        pid_t waited = waitpid(pid, &status, 0);

        if (waited == pid) {
            break;
        }
        if (waited == -1 && errno != EINTR) {
            break;
        }
    }
    return blocking && attempts < 1024U && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0
               ? 0
               : 1;
}

static int native_invocation_cases(const char *executable)
{
    static const char script[] =
        "GSH_INVOCATION=file\n"
        "alias gsh_invocation=/usr/bin/printf\n"
        "gsh_invocation \"<%s:%s:%s:%s>\" \"$GSH_INVOCATION\" \"$0\" "
        "\"$1\" \"$2\"\n";
    char directory[] = "/tmp/gsh-native-input-XXXXXX";
    char path[1024];
    char command[4096];
    char expected[2048];
    syntax_case test = {"sh", "standard input with -s", command, 0,
                        expected};
    int descriptor = -1;
    int failed = 0;

    if (executable == NULL || executable[0] != '/' ||
        mkdtemp(directory) == NULL ||
        snprintf(path, sizeof(path), "%s/script.sh", directory) >=
            (int)sizeof(path)) {
        return 1;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor == -1 ||
        write(descriptor, script, sizeof(script) - 1U) !=
            (ssize_t)(sizeof(script) - 1U)) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        failed = 1;
    } else if (close(descriptor) == -1) {
        failed = 1;
    } else if (snprintf(
                   command, sizeof(command),
                   "/usr/bin/printf %%b 'GSH_INVOCATION=stdin\\n"
                   "/usr/bin/printf \"<%%s:%%s:%%s:%%s>\" "
                   "\"$GSH_INVOCATION\" \"$0\" \"$1\" \"$2\"' | "
                   "'%s' -s first 'second value'",
                   executable) >= (int)sizeof(command) ||
               snprintf(expected, sizeof(expected),
                        "<stdin:%s:first:second value>", executable) >=
                   (int)sizeof(expected) ||
               run_case(executable, &test, false) != 0) {
        failed = 1;
    }
    test.name = "implicit standard input";
    if (!failed &&
        (snprintf(command, sizeof(command),
                  "/usr/bin/printf %%b 'GSH_INVOCATION=implicit\\n"
                  "/usr/bin/printf \"<%%s:%%s:%%s:%%s>\" "
                  "\"$GSH_INVOCATION\" \"$0\" \"$1\" \"$2\"' | "
                  "'%s' - operand",
                  executable) >= (int)sizeof(command) ||
         snprintf(expected, sizeof(expected), "<implicit:%s:operand:>",
                  executable) >= (int)sizeof(expected) ||
         run_case(executable, &test, false) != 0)) {
        failed = 1;
    }
    test.name = "command file and positional arguments";
    if (!failed &&
        (snprintf(command, sizeof(command), "'%s' '%s' first 'second value'",
                  executable, path) >= (int)sizeof(command) ||
         snprintf(expected, sizeof(expected), "<file:%s:first:second value>",
                  path) >= (int)sizeof(expected) ||
         run_case(executable, &test, false) != 0)) {
        failed = 1;
    }
    test.name = "non-seekable standard input is not read ahead";
    test.diagnostic = "streamed payload\n";
    if (!failed &&
        (snprintf(command, sizeof(command),
                  "/usr/bin/printf '%%s\\n' '/bin/cat' "
                  "'streamed payload' | '%s' -s",
                  executable) >= (int)sizeof(command) ||
         run_case(executable, &test, false) != 0)) {
        failed = 1;
    }
    if (!failed && native_large_input_case(executable, directory) != 0) {
        failed = 1;
    }
    if (!failed && native_seekable_input_case(executable, directory) != 0) {
        failed = 1;
    }
    if (!failed && native_nonblocking_input_case(executable) != 0) {
        failed = 1;
    }
    test.name = "regular invocation rejects native gap";
    test.status = 2;
    test.diagnostic = "native execution unsupported";
    if (!failed &&
        (snprintf(command, sizeof(command),
                  "'%s' -c '{ :; } >/dev/null'", executable) >=
             (int)sizeof(command) ||
         run_case(executable, &test, false) != 0)) {
        failed = 1;
    }
    (void)unlink(path);
    (void)rmdir(directory);
    return failed;
}

static int create_source_fixture(const char *path, const char *text,
                                 mode_t mode)
{
    size_t length = strlen(text);
    size_t written = 0;
    size_t attempts;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);

    if (descriptor == -1) {
        return -1;
    }
    for (attempts = 0; written < length && attempts <= length; attempts++) {
        ssize_t count = write(descriptor, text + written,
                              length - written);

        if (count > 0) {
            written += (size_t)count;
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            (void)close(descriptor);
            return -1;
        }
    }
    {
        int status = written == length && fchmod(descriptor, mode) == 0
                         ? 0
                         : -1;

        if (close(descriptor) == -1) {
            status = -1;
        }
        return status;
    }
}

typedef struct {
    char directory[64];
    char state[1024];
    char return_script[1024];
    char eval_return[1024];
    char inner[1024];
    char outer[1024];
    char empty[1024];
    char false_script[1024];
    char cd[1024];
    char parse[1024];
    char print[1024];
    char output[1024];
} source_fixtures;

static int source_fixture_path(char output[1024], const char *directory,
                               const char *name)
{
    int length = snprintf(output, 1024, "%s/%s", directory, name);

    return length < 0 || length >= 1024 ? -1 : 0;
}

static int initialize_source_paths(source_fixtures *fixtures)
{
    static const char template[] = "/tmp/gsh-native-source-XXXXXX";

    memset(fixtures, 0, sizeof(*fixtures));
    memcpy(fixtures->directory, template, sizeof(template));
    if (mkdtemp(fixtures->directory) == NULL) {
        return -1;
    }
    return source_fixture_path(fixtures->state, fixtures->directory,
                               "state.sh") == -1 ||
                   source_fixture_path(fixtures->return_script,
                                       fixtures->directory,
                                       "return.sh") == -1 ||
                   source_fixture_path(fixtures->eval_return,
                                       fixtures->directory,
                                       "eval-return.sh") == -1 ||
                   source_fixture_path(fixtures->inner, fixtures->directory,
                                       "inner.sh") == -1 ||
                   source_fixture_path(fixtures->outer, fixtures->directory,
                                       "outer.sh") == -1 ||
                   source_fixture_path(fixtures->empty, fixtures->directory,
                                       "empty.sh") == -1 ||
                   source_fixture_path(fixtures->false_script,
                                       fixtures->directory,
                                       "false.sh") == -1 ||
                   source_fixture_path(fixtures->cd, fixtures->directory,
                                       "cd.sh") == -1 ||
                   source_fixture_path(fixtures->parse, fixtures->directory,
                                       "parse.sh") == -1 ||
                   source_fixture_path(fixtures->print, fixtures->directory,
                                       "print.sh") == -1 ||
                   source_fixture_path(fixtures->output, fixtures->directory,
                                       "output") == -1
               ? -1
               : 0;
}

static int initialize_source_files(const source_fixtures *fixtures)
{
    static const char state[] =
        "GSH_DOT_VALUE=loaded\n"
        "gsh_dot_function() { /usr/bin/printf function; }\n"
        "alias gsh_dot_alias=/usr/bin/printf\n";
    char outer[2300];
    int length = snprintf(
        outer, sizeof(outer),
        ". '%s'\n/usr/bin/printf 'outer:%%s' \"$?\"\n",
        fixtures->inner);

    if (length < 0 || length >= (int)sizeof(outer)) {
        return -1;
    }
    return create_source_fixture(fixtures->state, state, 0400) == -1 ||
                   create_source_fixture(
                       fixtures->return_script,
                       "/usr/bin/printf before\nreturn 7\n"
                       "/usr/bin/printf BAD\n",
                       0600) == -1 ||
                   create_source_fixture(
                       fixtures->eval_return,
                       "eval 'return 9'\n/usr/bin/printf BAD\n",
                       0600) == -1 ||
                   create_source_fixture(
                       fixtures->inner,
                       "/usr/bin/printf inner\nreturn 4\n"
                       "/usr/bin/printf BAD\n",
                       0600) == -1 ||
                   create_source_fixture(fixtures->outer, outer, 0600) ==
                       -1 ||
                   create_source_fixture(fixtures->empty, "", 0600) == -1 ||
                   create_source_fixture(fixtures->false_script, "false\n",
                                         0600) == -1 ||
                   create_source_fixture(fixtures->cd, "cd /\n", 0600) ==
                       -1 ||
                   create_source_fixture(fixtures->parse, "if\n", 0600) ==
                       -1 ||
                   create_source_fixture(fixtures->print,
                                         "/usr/bin/printf source\n",
                                         0600) == -1
               ? -1
               : 0;
}

static void destroy_source_fixtures(const source_fixtures *fixtures)
{
    const char *paths[] = {
        fixtures->output, fixtures->print, fixtures->parse, fixtures->cd,
        fixtures->false_script, fixtures->empty, fixtures->outer,
        fixtures->inner, fixtures->eval_return, fixtures->return_script,
        fixtures->state};
    size_t index;

    for (index = 0; index < sizeof(paths) / sizeof(paths[0]); index++) {
        if (paths[index][0] != '\0') {
            (void)unlink(paths[index]);
        }
    }
    if (fixtures->directory[0] != '\0') {
        (void)rmdir(fixtures->directory);
    }
}

static int source_state_cases(const char *executable,
                              const source_fixtures *fixtures)
{
    char command[8192];
    syntax_case test = {"dot", "slash pathname sources current state",
                        command, 0, "functionalias"};

    if (snprintf(command, sizeof(command),
                 ". '%s'\n/bin/test \"$GSH_DOT_VALUE\" = loaded; "
                 "gsh_dot_function; gsh_dot_alias alias",
                 fixtures->state) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "PATH search accepts a non-executable readable file";
    test.diagnostic = NULL;
    if (snprintf(command, sizeof(command),
                 "PATH='%s:/bin:/usr/bin' . -- state.sh; "
                 "/bin/test \"$GSH_DOT_VALUE:$PATH\" = "
                 "loaded:'%s:/bin:/usr/bin'",
                 fixtures->directory, fixtures->directory) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "command dot uses and restores its temporary PATH";
    if (snprintf(command, sizeof(command),
                 "GSH_DOT_PATH=$PATH; "
                 "PATH='%s:/bin:/usr/bin' command . state.sh; "
                 "/bin/test \"$GSH_DOT_VALUE\" = loaded; "
                 "/bin/test \"$PATH\" = \"$GSH_DOT_PATH\"",
                 fixtures->directory) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "subshell dot mutation is isolated";
    if (snprintf(command, sizeof(command),
                 "GSH_DOT_VALUE=parent; (. '%s'; "
                 "/bin/test \"$GSH_DOT_VALUE\" = loaded); "
                 "/bin/test \"$GSH_DOT_VALUE\" = parent",
                 fixtures->state) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "pipeline dot mutation is isolated";
    if (snprintf(command, sizeof(command),
                 "GSH_DOT_VALUE=parent; . '%s' | true; "
                 "/bin/test \"$GSH_DOT_VALUE\" = parent",
                 fixtures->state) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    return 0;
}

static int source_return_cases(const char *executable,
                               const source_fixtures *fixtures)
{
    char command[8192];
    syntax_case test = {"dot", "dot return stops its source", command, 0,
                        "beforeafter:7"};

    if (snprintf(command, sizeof(command),
                 ". '%s'; GSH_DOT_STATUS=$?; "
                 "/usr/bin/printf 'after:%%s' \"$GSH_DOT_STATUS\"",
                 fixtures->return_script) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "dot return does not return from its caller function";
    test.diagnostic = "beforecontinued";
    if (snprintf(command, sizeof(command),
                 "gsh_dot_call() { . '%s'; /usr/bin/printf continued; }; "
                 "gsh_dot_call",
                 fixtures->return_script) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "return propagates through eval to the dot boundary";
    test.diagnostic = "status:9";
    if (snprintf(command, sizeof(command),
                 ". '%s'; /usr/bin/printf 'status:%%s' \"$?\"",
                 fixtures->eval_return) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "nested dot return unwinds only its own source";
    test.diagnostic = "innerouter:4";
    if (snprintf(command, sizeof(command), ". '%s'", fixtures->outer) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    return 0;
}

static int source_status_cases(const char *executable,
                               const source_fixtures *fixtures)
{
    char command[8192];
    syntax_case test = {"dot", "dot redirection spans its source", command,
                        0, "DOT_REDIRECT_RESTORED"};

    if (snprintf(command, sizeof(command),
                 ". '%s' >'%s'; "
                 "/bin/test \"$(/bin/cat '%s')\" = source; "
                 "/usr/bin/printf DOT_REDIRECT_RESTORED",
                 fixtures->print, fixtures->output, fixtures->output) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "dot supplies a pipeline stage";
    test.diagnostic = "SOURCE";
    if (snprintf(command, sizeof(command),
                 ". '%s' | /usr/bin/tr a-z A-Z", fixtures->print) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "empty dot script returns zero";
    test.diagnostic = NULL;
    if (snprintf(command, sizeof(command), ". '%s'", fixtures->empty) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "dot returns the last command status";
    if (snprintf(command, sizeof(command),
                 ". '%s'; /bin/test \"$?\" -eq 1",
                 fixtures->false_script) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "dot supplies the last pipeline status";
    test.status = 1;
    test.diagnostic = NULL;
    if (snprintf(command, sizeof(command),
                 "/usr/bin/true | . '%s'", fixtures->false_script) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "directory mutation in dot persists";
    test.status = 0;
    if (snprintf(command, sizeof(command),
                 ". '%s'; /bin/test \"$PWD\" = /", fixtures->cd) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    return 0;
}

static int source_error_cases(const char *executable,
                              const source_fixtures *fixtures)
{
    char command[8192];
    syntax_case test = {"dot", "missing dot file aborts the shell", command,
                        1, "No such file"};

    if (snprintf(command, sizeof(command),
                 ". '%s/missing'; /usr/bin/printf BAD_DOT",
                 fixtures->directory) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "dot syntax error aborts a non-interactive shell";
    test.status = 2;
    test.diagnostic = "incomplete";
    if (snprintf(command, sizeof(command),
                 ". '%s'; /usr/bin/printf BAD_DOT", fixtures->parse) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "dot rejects extra operands";
    test.diagnostic = "exactly one file operand";
    if (snprintf(command, sizeof(command), ". '%s' extra",
                 fixtures->state) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "command suppresses dot special error semantics";
    test.status = 0;
    test.diagnostic = "DOT_RECOVERED";
    if (snprintf(command, sizeof(command),
                 "command . '%s/missing'; /usr/bin/printf DOT_RECOVERED",
                 fixtures->directory) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    return 0;
}

static int native_source_cases(const char *executable)
{
    source_fixtures fixtures;
    int failed;

    if (initialize_source_paths(&fixtures) == -1) {
        destroy_source_fixtures(&fixtures);
        return 1;
    }
    failed = initialize_source_files(&fixtures) == -1 ||
             source_state_cases(executable, &fixtures) != 0 ||
             source_return_cases(executable, &fixtures) != 0 ||
             source_status_cases(executable, &fixtures) != 0 ||
             source_error_cases(executable, &fixtures) != 0;
    destroy_source_fixtures(&fixtures);
    return failed;
}

static int native_exit_core_cases(const char *executable)
{
    static const syntax_case cases[] = {
        {"exit", "explicit status terminates the shell", "exit 7; :", 7,
         NULL},
        {"exit", "maximum portable status is preserved", "exit 255; :", 255,
         NULL},
        {"exit", "omitted status uses the preceding pipeline",
         "false; exit; :", 1, NULL},
        {"exit", "empty complete commands preserve the preceding status",
         "false\n\n# comment\nexit\n", 1, NULL},
        {"exit", "pipeline negation cannot outlive exit", "! exit 9; :", 9,
         NULL},
        {"exit", "exit crosses a function frame",
         "f(){ exit 11; }; f; :", 11, NULL},
        {"exit", "exit crosses an eval frame", "eval 'exit 12'; :", 12,
         NULL},
        {"exit", "subshell exit remains isolated",
         "(exit 13); /bin/test \"$?\" -eq 13", 0, NULL},
        {"exit", "pipeline-stage exit remains isolated",
         "exit 14 | /usr/bin/true; /bin/test \"$?\" -eq 0", 0, NULL},
        {"exit", "asynchronous exit status reaches wait",
         "exit 15 & wait \"$!\"; /bin/test \"$?\" -eq 15", 0, NULL},
        {"exit", "command wrapper still invokes exit", "command exit 16; :",
         16, NULL},
        {"exit", "command substitution exit remains isolated",
         "value=$(exit 17; /usr/bin/printf BAD); /bin/test -z \"$value\"",
         0, NULL},
        {"exit", "exit crosses a loop frame",
         "while :; do exit 18; done; :", 18, NULL},
        {"exit", "exit crosses a case frame",
         "case x in x) exit 19;; esac; :", 19, NULL},
        {"exit", "special-builtin operand error aborts",
         "exit 1 2; /usr/bin/printf BAD", 1, "too many operands"},
        {"exit", "out-of-range status is deterministic", "exit 256; :", 2,
         "invalid status"},
        {"exit", "command suppresses special error termination",
         "command exit 1 2; /usr/bin/printf EXIT_RECOVERED", 0,
         "EXIT_RECOVERED"},
    };
    size_t index;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (run_case(executable, &cases[index], false) != 0) {
            return 1;
        }
    }
    return 0;
}

static int native_exit_file_cases(const char *executable)
{
    char directory[] = "/tmp/gsh-native-exit-XXXXXX";
    char source[1024] = {0};
    char output[1024] = {0};
    char command[2300];
    struct stat information;
    syntax_case test = {"exit", "exit crosses a dot frame", command, 20,
                        NULL};
    int failed = 1;

    if (mkdtemp(directory) == NULL ||
        snprintf(source, sizeof(source), "%s/source", directory) >=
            (int)sizeof(source) ||
        snprintf(output, sizeof(output), "%s/output", directory) >=
            (int)sizeof(output) ||
        create_source_fixture(source, "exit 20\n:\n", 0600) == -1 ||
        snprintf(command, sizeof(command), ". '%s'; :", source) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        goto done;
    }
    test.name = "exit applies redirections before termination";
    test.status = 21;
    if (snprintf(command, sizeof(command), "exit 21 > '%s'; :", output) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0 ||
        stat(output, &information) == -1 ||
        !S_ISREG(information.st_mode) || information.st_size != 0) {
        goto done;
    }
    failed = 0;
done:
    if (output[0] != '\0') {
        (void)unlink(output);
    }
    if (source[0] != '\0') {
        (void)unlink(source);
    }
    (void)rmdir(directory);
    return failed;
}

static int native_exit_cases(const char *executable)
{
    return native_exit_core_cases(executable) != 0 ||
           native_exit_file_cases(executable) != 0;
}

static int enoexec_argument_cases(const char *executable,
                                  const char *directory,
                                  const char *script)
{
    char command[8192];
    char expected[2048];
    syntax_case test = {"2.9.1.6", "ENOEXEC script preserves operands",
                        command, 7, expected};

    if (snprintf(command, sizeof(command),
                 "GSH_ENOEXEC_STATUS=7 '%s' alpha 'beta gamma'", script) >=
            (int)sizeof(command) ||
        snprintf(expected, sizeof(expected),
                 "<script:%s:alpha:beta gamma:unset>\n", script) >=
            (int)sizeof(expected) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "PATH-found ENOEXEC script uses the resolved pathname";
    test.status = 3;
    if (snprintf(command, sizeof(command),
                 "PATH='%s:/bin:/usr/bin' GSH_ENOEXEC_STATUS=3 "
                 "probe one two",
                 directory) >= (int)sizeof(command) ||
        snprintf(expected, sizeof(expected),
                 "<script:%s/probe:one:two:unset>\n", directory) >=
            (int)sizeof(expected) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    return 0;
}

static int enoexec_environment_cases(const char *executable,
                                     const char *script)
{
    char command[8192];
    char expected[2048];
    syntax_case test = {"2.9.1.6", "ENOEXEC receives a temporary environment",
                        command, 0, expected};

    if (snprintf(command, sizeof(command),
                 "GSH_ENOEXEC_ENV=visible '%s'; "
                 "/bin/test -z \"${GSH_ENOEXEC_ENV+set}\"",
                 script) >= (int)sizeof(command) ||
        snprintf(expected, sizeof(expected),
                 "<script:%s:unset:unset:visible>\n", script) >=
            (int)sizeof(expected) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "ENOEXEC script executes as a pipeline stage";
    test.diagnostic = "SCRIPT";
    if (snprintf(command, sizeof(command),
                 "'%s' left right | /usr/bin/tr a-z A-Z", script) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "asynchronous ENOEXEC status reaches wait";
    test.diagnostic = "<script:";
    if (snprintf(command, sizeof(command),
                 "GSH_ENOEXEC_STATUS=9 '%s' & wait \"$!\"; "
                 "/bin/test \"$?\" -eq 9",
                 script) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "exec overlays with the native ENOEXEC interpreter";
    test.status = 11;
    if (snprintf(command, sizeof(command),
                 "GSH_ENOEXEC_STATUS=11 exec '%s' overlay operand", script) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    return 0;
}

static int native_enoexec_cases(const char *executable)
{
    static const char source[] =
        "/usr/bin/printf '<script:%s:%s:%s:%s>\\n' \"$0\" "
        "\"${1-unset}\" \"${2-unset}\" \"${GSH_ENOEXEC_ENV-unset}\"\n"
        "exit \"${GSH_ENOEXEC_STATUS:-0}\"\n";
    char directory[] = "/tmp/gsh-native-enoexec-XXXXXX";
    char script[1024] = {0};
    int length;
    int failed = 1;

    if (mkdtemp(directory) != NULL) {
        length = snprintf(script, sizeof(script), "%s/probe", directory);
        if (length >= 0 && length < (int)sizeof(script) &&
            create_source_fixture(script, source, 0700) == 0) {
            failed =
                enoexec_argument_cases(executable, directory, script) != 0 ||
                enoexec_environment_cases(executable, script) != 0;
        }
    }
    if (script[0] != '\0') {
        (void)unlink(script);
    }
    (void)rmdir(directory);
    return failed;
}

static int native_umask_creation_case(const char *executable)
{
    char directory[] = "/tmp/gsh-native-umask-XXXXXX";
    char target[1024];
    char command[1200];
    struct stat information;
    syntax_case test = {"umask", "creation mask affects redirection",
                        command, 0, NULL};
    int failed;

    if (mkdtemp(directory) == NULL ||
        snprintf(target, sizeof(target), "%s/created", directory) >=
            (int)sizeof(target) ||
        snprintf(command, sizeof(command), "umask 077; : > %s", target) >=
            (int)sizeof(command)) {
        return 1;
    }
    failed = run_case(executable, &test, false);
    if (stat(target, &information) == -1 ||
        (information.st_mode & 0777) != 0600) {
        fprintf(stderr, "conformance: umask did not affect redirection\n");
        failed = 1;
    }
    (void)unlink(target);
    (void)rmdir(directory);
    return failed;
}

static pid_t start_builtin_with_broken_output(const char *executable,
                                              const char *command)
{
    int descriptors[2];
    pid_t pid;

    if (pipe(descriptors) == -1) {
        return -1;
    }
    pid = fork();
    if (pid == 0) {
        char *arguments[] = {(char *)executable, (char *)"--native-only",
                             (char *)"-c", (char *)command, NULL};

        (void)close(descriptors[0]);
        if (signal(SIGPIPE, SIG_IGN) == SIG_ERR ||
            (descriptors[1] != STDOUT_FILENO &&
             dup2(descriptors[1], STDOUT_FILENO) == -1)) {
            _exit(126);
        }
        if (descriptors[1] != STDOUT_FILENO) {
            (void)close(descriptors[1]);
        }
        execv(executable, arguments);
        _exit(127);
    }
    (void)close(descriptors[0]);
    (void)close(descriptors[1]);
    return pid;
}

static int native_builtin_output_failure_cases(const char *executable)
{
    static const char *const commands[] = {
        "umask", "ulimit -S -n", "times", "export -p",
        "readonly GSH_CLOSED_OUTPUT=value; readonly -p",
        "alias gsh_closed_output=value\nalias gsh_closed_output"};
    size_t index;

    for (index = 0; index < sizeof(commands) / sizeof(commands[0]); index++) {
        uint64_t deadline;
        int status = 0;
        pid_t pid = start_builtin_with_broken_output(executable,
                                                     commands[index]);

        if (pid == -1) {
            return 1;
        }
        deadline = monotonic_ns() + 2000000000ULL;
        for (;;) {
            pid_t waited = waitpid(pid, &status, WNOHANG);

            if (waited == pid) {
                break;
            }
            if (waited == -1 && errno != EINTR) {
                return 1;
            }
            if (monotonic_ns() >= deadline) {
                (void)kill(pid, SIGKILL);
                (void)waitpid(pid, NULL, 0);
                fprintf(stderr, "conformance: builtin output failure timeout\n");
                return 1;
            }
            (void)poll(NULL, 0, 10);
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 1) {
            fprintf(stderr,
                    "conformance: builtin output failure was not reported: %s\n",
                    commands[index]);
            return 1;
        }
    }
    return 0;
}

static int native_redirection_cases(const char *executable)
{
    char directory[] = "/tmp/gsh-native-redirection-XXXXXX";
    char target[1024];
    char command[1400];
    char contents[64];
    ssize_t length;
    int descriptor;
    int failed = 0;
    syntax_case test = {"2.7", "native output redirection", command, 0,
                        NULL};

    if (mkdtemp(directory) == NULL ||
        snprintf(target, sizeof(target), "%s/output", directory) >=
            (int)sizeof(target)) {
        return 1;
    }
    if (snprintf(command, sizeof(command),
                 "/usr/bin/printf 'first\\n' > %s", target) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }
    test.name = "native append redirection";
    if (snprintf(command, sizeof(command),
                 "/usr/bin/printf 'second\\n' >> %s", target) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }
    descriptor = open(target, O_RDONLY);
    if (descriptor == -1) {
        failed = 1;
        goto done;
    }
    length = read(descriptor, contents, sizeof(contents));
    close(descriptor);
    if (length != 13 || memcmp(contents, "first\nsecond\n", 13) != 0) {
        fprintf(stderr, "conformance: native redirection content mismatch\n");
        failed = 1;
        goto done;
    }
    test.name = "native input redirection";
    test.diagnostic = "first\nsecond\n";
    if (snprintf(command, sizeof(command), "/bin/cat < %s", target) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }

    test.name = "native descriptor ordering to file";
    test.diagnostic = NULL;
    if (snprintf(command, sizeof(command),
                 "/usr/bin/printf routed 2>%s 1>&2", target) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }
    descriptor = open(target, O_RDONLY);
    if (descriptor == -1) {
        failed = 1;
        goto done;
    }
    length = read(descriptor, contents, sizeof(contents));
    close(descriptor);
    if (length != 6 || memcmp(contents, "routed", 6) != 0) {
        fprintf(stderr, "conformance: descriptor ordering mismatch\n");
        failed = 1;
        goto done;
    }

    test.name = "native descriptor ordering to original stderr";
    test.diagnostic = "visible";
    if (snprintf(command, sizeof(command),
                 "/usr/bin/printf visible 1>&2 2>%s", target) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }
    descriptor = open(target, O_RDONLY);
    if (descriptor == -1) {
        failed = 1;
        goto done;
    }
    length = read(descriptor, contents, sizeof(contents));
    close(descriptor);
    if (length != 0) {
        fprintf(stderr, "conformance: reversed descriptor ordering mismatch\n");
        failed = 1;
    }

done:
    (void)unlink(target);
    (void)rmdir(directory);
    return failed;
}

static int native_function_redirection_cases(const char *executable)
{
    char directory[] = "/tmp/gsh-native-function-redirection-XXXXXX";
    char command[4096];
    syntax_case test = {"2.9.5", "function redirection is deferred",
                        command, 0, NULL};
    size_t passed = 0;

    if (mkdtemp(directory) == NULL) {
        return 1;
    }
    if (snprintf(command, sizeof(command),
                 "p=%s/not-created; f(){ /usr/bin/printf body; } "
                 ">\"$p\"; /bin/test ! -e \"$p\"; p=%s/deferred; "
                 "f; /bin/test \"$(/bin/cat \"$p\")\" = body",
                 directory, directory) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        goto failed;
    }
    passed++;
    test.name = "function definition redirect uses invocation positionals";
    if (snprintf(command, sizeof(command),
                 "f(){ /usr/bin/printf '%%s' \"$1\"; } >\"$2\"; "
                 "f value %s/positional; "
                 "/bin/test \"$(/bin/cat %s/positional)\" = value",
                 directory, directory) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        goto failed;
    }
    passed++;
    test.name = "function call redirect";
    if (snprintf(command, sizeof(command),
                 "f(){ /usr/bin/printf call; }; f >%s/call; "
                 "/bin/test \"$(/bin/cat %s/call)\" = call",
                 directory, directory) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        goto failed;
    }
    passed++;
    test.name = "function definition redirect is nested inside call redirect";
    if (snprintf(command, sizeof(command),
                 "f(){ /usr/bin/printf inner; } >%s/definition; "
                 "f >%s/outer; /bin/test \"$(/bin/cat %s/definition)\" "
                 "= inner; /bin/test ! -s %s/outer",
                 directory, directory, directory, directory) >=
            (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        goto failed;
    }
    passed++;
    test.name = "function redirects restore descriptors";
    test.diagnostic = "outer\n";
    if (snprintf(command, sizeof(command),
                 "f(){ /usr/bin/printf inner; } >%s/restore; f; "
                 "/usr/bin/printf 'outer\\n'; "
                 "/bin/test \"$(/bin/cat %s/restore)\" = inner",
                 directory, directory) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        goto failed;
    }
    passed++;
    test.name = "function attached heredoc expands at invocation";
    test.diagnostic = "argument\n";
    test.command = "f(){ /bin/cat; } <<EOF\n$1\nEOF\nf argument";
    if (run_case(executable, &test, false) != 0) {
        goto failed;
    }
    passed++;
    test.name = "failed function body redirect reports failure";
    test.diagnostic = "function body redirection";
    test.command = "f(){ /usr/bin/printf BAD; } >/dev/null/missing; "
                   "f; s=$?; /bin/test \"$s\" -ne 0";
    if (run_case(executable, &test, false) != 0) {
        goto failed;
    }
    passed++;
    test.name = "failed call redirect suppresses function body";
    test.diagnostic = "function redirection";
    test.command = command;
    if (snprintf(command, sizeof(command),
                 "f(){ : >%s/effect; }; f >/dev/null/missing; s=$?; "
                 "/bin/test \"$s\" -ne 0; /bin/test ! -e %s/effect",
                 directory, directory) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        goto failed;
    }
    passed++;
    goto done;

failed:
    fprintf(stderr,
            "conformance: function redirection stopped after %zu cases\n",
            passed);
done:
    (void)snprintf(command, sizeof(command), "%s/not-created", directory);
    (void)unlink(command);
    {
        static const char *const names[] = {
            "deferred", "positional", "call", "definition",
            "outer", "restore", "effect"};
        size_t index;

        for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
            (void)snprintf(command, sizeof(command), "%s/%s", directory,
                           names[index]);
            (void)unlink(command);
        }
    }
    (void)rmdir(directory);
    return passed == 8U ? 0 : 1;
}

static int native_function_context_cases(const char *executable)
{
    static const syntax_case cases[] = {
        {"2.9.5/2.6.3", "function is visible in command substitution",
         "f(){ /usr/bin/printf '<%s>' \"$1\"; }; "
         "/bin/test \"$(f value)\" = '<value>'",
         0, NULL},
        {"2.9.5/2.13", "command substitution function redefinition is isolated",
         "f(){ /usr/bin/printf outer; }; "
         "x=$(f(){ /usr/bin/printf inner; }; f); y=$(f); "
         "/bin/test \"$x:$y\" = inner:outer",
         0, NULL},
        {"unset/2.13", "command substitution function unset is isolated",
         "f(){ /usr/bin/printf outer; }; "
         "x=$(unset -f f; f 2>/dev/null); /bin/test -z \"$x\"; "
         "/bin/test \"$(f)\" = outer",
         0, NULL},
        {"2.9.2/2.9.5", "function is a pipeline producer",
         "f(){ /usr/bin/printf 'alpha\\nbeta\\n'; }; "
         "f | /usr/bin/tr a-z A-Z",
         0, "ALPHA\nBETA\n"},
        {"2.9.2/2.9.5", "function is a pipeline consumer",
         "f(){ /usr/bin/tr a-z A-Z; }; /usr/bin/printf lower | f",
         0, "LOWER"},
        {"2.9.2/2.9.5", "functions compose as concurrent pipeline stages",
         "a(){ /usr/bin/printf alpha; }; "
         "b(){ /usr/bin/tr a-z A-Z; }; a | b",
         0, "ALPHA"},
        {"2.9.2/2.9.5", "last function stage supplies pipeline status",
         "f(){ return 7; }; /usr/bin/true | f", 7, NULL},
        {"2.9.2/2.13", "pipeline function mutations are isolated",
         "GSH_FUNCTION_PIPE=outer; "
         "f(){ GSH_FUNCTION_PIPE=inner; /usr/bin/printf data; }; "
         "f | /bin/cat; /bin/test \"$GSH_FUNCTION_PIPE\" = outer",
         0, "data"},
        {"2.9.4.1/2.13", "subshell function redefinition is isolated",
         "f(){ /usr/bin/printf outer; }; "
         "(f(){ /usr/bin/printf inner; }; f); f",
         0, "innerouter"},
        {"2.9.3/2.13", "asynchronous function mutations are isolated",
         "GSH_FUNCTION_ASYNC=outer; f(){ GSH_FUNCTION_ASYNC=inner; }; "
         "f & wait \"$!\"; "
         "/bin/test \"$GSH_FUNCTION_ASYNC\" = outer",
         0, NULL},
    };
    syntax_case zero = {
        "2.9.5", "function leaves parameter zero unchanged",
        "f(){ /usr/bin/printf '<%s:%s>' \"$0\" \"$1\"; }; f operand",
        0, "<gsh-script-zero:operand>"};
    size_t index;

    if (run_case_arguments(executable, &zero, false, "gsh-script-zero",
                           NULL, 0) != 0) {
        return 1;
    }
    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (run_case(executable, &cases[index], false) != 0) {
            return 1;
        }
    }
    return 0;
}

static int native_limit_cases(const char *executable)
{
    char command[70000];
    char directory[] = "/tmp/gsh-native-glob-XXXXXX";
    char path[1024];
    syntax_case test = {"limits", "native pipeline width", command, 2,
                        "native execution unsupported"};
    size_t used = 0;
    size_t index;

    if (native_oversized_input_case(executable) != 0) {
        return 1;
    }

    for (index = 0; index < 33; index++) {
        int length = snprintf(command + used, sizeof(command) - used,
                              "%s/usr/bin/true", index == 0 ? "" : " | ");

        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            return 1;
        }
        used += (size_t)length;
    }
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    memcpy(command, "/usr/bin/true", 14);
    used = 13;
    for (index = 0; index < 129; index++) {
        if (used + 2U >= sizeof(command)) {
            return 1;
        }
        command[used++] = ' ';
        command[used++] = 'x';
    }
    command[used] = '\0';
    test.name = "native argument limit";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    memcpy(command, "for item in", 12);
    used = 11;
    for (index = 0; index < 129; index++) {
        if (used + 2U >= sizeof(command)) {
            return 1;
        }
        command[used++] = ' ';
        command[used++] = 'x';
    }
    if (snprintf(command + used, sizeof(command) - used,
                 "; do :; done") >= (int)(sizeof(command) - used)) {
        return 1;
    }
    test.name = "native for item limit";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    memcpy(command, "/usr/bin/true", 14);
    used = 13;
    for (index = 0; index < 33; index++) {
        int length = snprintf(command + used, sizeof(command) - used,
                              " %zu>/dev/null", index + 3U);

        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            return 1;
        }
        used += (size_t)length;
    }
    test.name = "native redirection limit";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    memcpy(command, "/usr/bin/printf ", 17);
    used = 16;
    memset(command + used, 'a', 17000);
    used += 17000;
    command[used] = '\0';
    test.name = "native expansion storage limit";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    if (snprintf(command, sizeof(command),
                 "/usr/bin/printf '%%s' \"$(/usr/bin/yes x | "
                 "/usr/bin/head -c 17000)\"") >= (int)sizeof(command)) {
        return 1;
    }
    test.name = "native command substitution output limit";
    test.status = 125;
    test.diagnostic = NULL;
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    if (build_nested_substitution(command, sizeof(command),
                                  GSH_SOURCE_DEPTH_CAP) == -1) {
        return 1;
    }
    test.name = "native nested source depth boundary";
    test.status = 0;
    test.diagnostic = "<x>\n";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    if (build_nested_substitution(command, sizeof(command),
                                  GSH_SOURCE_DEPTH_CAP + 1U) == -1) {
        return 1;
    }
    test.name = "native nested source depth limit";
    test.status = 0;
    test.diagnostic = "nested source workspace limit exceeded";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.status = 2;
    test.diagnostic = "native execution unsupported";

    if (build_nested_eval(command, sizeof(command),
                          GSH_SOURCE_DEPTH_CAP - 1U) == -1) {
        return 1;
    }
    test.name = "native nested eval depth boundary";
    test.status = 0;
    test.diagnostic = NULL;
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    if (build_nested_eval(command, sizeof(command),
                          GSH_SOURCE_DEPTH_CAP) == -1) {
        return 1;
    }
    test.name = "native nested eval depth limit";
    test.status = 125;
    test.diagnostic = "nested source workspace limit exceeded";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.status = 2;
    test.diagnostic = "native execution unsupported";

    used = 0;
    {
        int length = snprintf(command, sizeof(command),
                              "/usr/bin/printf '%%s' ");

        if (length < 0 || (size_t)length >= sizeof(command)) {
            return 1;
        }
        used = (size_t)length;
    }
    for (index = 0; index < 33; index++) {
        static const char opening[] = "${GSH_CONFORMANCE_UNSET:-";

        if (sizeof(opening) - 1U > sizeof(command) - used - 1U) {
            return 1;
        }
        memcpy(command + used, opening, sizeof(opening) - 1U);
        used += sizeof(opening) - 1U;
    }
    command[used++] = 'x';
    for (index = 0; index < 33; index++) {
        command[used++] = '}';
    }
    command[used] = '\0';
    test.name = "native parameter expansion depth limit";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    memcpy(command, ": \"$((", 7);
    used = 6;
    for (index = 0; index < 33; index++) {
        int length = snprintf(command + used, sizeof(command) - used,
                              "GSH_ARITH_DEPTH_%zu=", index);

        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            return 1;
        }
        used += (size_t)length;
    }
    memcpy(command + used, "1))\"", 5);
    test.name = "native arithmetic assignment depth limit";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    memcpy(command, ":", 2);
    used = 1;
    for (index = 0; index < 65; index++) {
        int length = snprintf(command + used, sizeof(command) - used,
                              " ${GSH_SCOPE_%zu:=x}", index);

        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            return 1;
        }
        used += (size_t)length;
    }
    if (snprintf(command + used, sizeof(command) - used,
                 " | /usr/bin/true") >= (int)(sizeof(command) - used)) {
        return 1;
    }
    test.name = "native scoped variable journal limit";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    used = 0;
    for (index = 0; index < 1025; index++) {
        int length;

        if (index % 120U == 0) {
            length = snprintf(command + used, sizeof(command) - used,
                              "%sexport", index == 0 ? "" : "; ");
            if (length < 0 || (size_t)length >= sizeof(command) - used) {
                return 1;
            }
            used += (size_t)length;
        }
        length = snprintf(command + used, sizeof(command) - used,
                          " GSH_VARIABLE_CAP_%04zu=x", index);
        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            return 1;
        }
        used += (size_t)length;
    }
    test.name = "native variable store exhaustion";
    test.status = 1;
    test.diagnostic = "variable update failed";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.status = 2;
    test.diagnostic = "native execution unsupported";

    used = 0;
    for (index = 0; index < 34; index++) {
        int length;

        if (index == 0 || index == 17) {
            length = snprintf(command + used, sizeof(command) - used,
                              "%s/bin/cat", index == 0 ? "" : " | ");
            if (length < 0 || (size_t)length >= sizeof(command) - used) {
                return 1;
            }
            used += (size_t)length;
        }
        length = snprintf(command + used, sizeof(command) - used,
                          " <<D%zu", index);
        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            return 1;
        }
        used += (size_t)length;
    }
    if (used + 1U >= sizeof(command)) {
        return 1;
    }
    command[used++] = '\n';
    for (index = 0; index < 34; index++) {
        int length = snprintf(command + used, sizeof(command) - used,
                              "x\nD%zu\n", index);

        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            return 1;
        }
        used += (size_t)length;
    }
    test.name = "native here-document count limit";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    memcpy(command, "/bin/cat <<EOF\n", 16);
    used = 15;
    memset(command + used, 'h', 65537U);
    used += 65537U;
    memcpy(command + used, "\nEOF\n", 6);
    used += 5;
    command[used] = '\0';
    test.name = "native here-document storage limit";
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    if (mkdtemp(directory) == NULL) {
        return 1;
    }
    for (index = 0; index < 129; index++) {
        int descriptor;

        if (snprintf(path, sizeof(path), "%s/item-%03zu", directory,
                     index) >= (int)sizeof(path)) {
            break;
        }
        descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor == -1 || close(descriptor) == -1) {
            break;
        }
    }
    if (index == 129 &&
        snprintf(command, sizeof(command), "/usr/bin/printf x %s/*",
                 directory) < (int)sizeof(command)) {
        test.name = "native pathname result limit";
        test.status = 125;
        test.diagnostic = NULL;
        if (run_case(executable, &test, false) != 0) {
            used = 1;
        } else {
            used = 0;
        }
    } else {
        used = 1;
    }
    for (index = 0; index < 129; index++) {
        if (snprintf(path, sizeof(path), "%s/item-%03zu", directory,
                     index) < (int)sizeof(path)) {
            (void)unlink(path);
        }
    }
    (void)rmdir(directory);
    if (used != 0) {
        return 1;
    }
    return 0;
}

static int native_heredoc_stress_cases(const char *executable)
{
    char command[61000];
    const char *prefix;
    syntax_case test = {"2.7.4", "large native here-document",
                        command, 0, "60001"};
    size_t used;

    prefix = "/usr/bin/wc -c <<EOF\n";
    used = strlen(prefix);
    memcpy(command, prefix, used);
    memset(command + used, 'x', 60000);
    used += 60000;
    memcpy(command + used, "\nEOF\n", 6);
    used += 5;
    command[used] = '\0';
    if (run_case(executable, &test, false) != 0) {
        return 1;
    }

    prefix = "/usr/bin/true <<EOF\n";
    used = strlen(prefix);
    memcpy(command, prefix, used);
    memset(command + used, 'x', 60000);
    used += 60000;
    memcpy(command + used, "\nEOF\n", 6);
    used += 5;
    command[used] = '\0';
    test.name = "unread large here-document cleanup";
    test.diagnostic = NULL;
    return run_case(executable, &test, false);
}

static int native_pattern_stress_cases(const char *executable)
{
    char value[12001];
    syntax_case prefix = {
        "2.6.2", "bounded multi-star prefix removal",
        "/bin/test -z \"${GSH_PATTERN_STRESS#?*?*?*z}\"", 0, NULL};
    syntax_case suffix = {
        "2.6.2", "bounded multi-star suffix removal",
        "GSH_PATTERN_RESULT=${GSH_PATTERN_STRESS%?*?*?*z}; "
        "/bin/test \"${#GSH_PATTERN_RESULT}\" -eq 11996",
        0, NULL};
    int failed;

    memset(value, '0', sizeof(value) - 2U);
    value[sizeof(value) - 2U] = 'z';
    value[sizeof(value) - 1U] = '\0';
    if (setenv("GSH_PATTERN_STRESS", value, 1) == -1) {
        return 1;
    }
    failed = run_case(executable, &prefix, false);
    if (failed == 0) {
        failed = run_case(executable, &suffix, false);
    }
    if (unsetenv("GSH_PATTERN_STRESS") == -1) {
        failed = 1;
    }
    return failed;
}

static int native_loop_cases(const char *executable)
{
    char directory[] = "/tmp/gsh-native-loop-XXXXXX";
    char target[1024];
    char command[4096];
    syntax_case test = {"2.9.4.5", "native while iteration", command, 1,
                        NULL};
    int failed = 0;

    if (mkdtemp(directory) == NULL ||
        snprintf(target, sizeof(target), "%s/while-done", directory) >=
            (int)sizeof(target) ||
        snprintf(command, sizeof(command),
                 "while /bin/test ! -e %s; do /usr/bin/touch %s; "
                 "/usr/bin/false; done",
                 target, target) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0 ||
        access(target, F_OK) == -1) {
        failed = 1;
        goto done;
    }
    (void)unlink(target);
    if (snprintf(target, sizeof(target), "%s/until-done", directory) >=
            (int)sizeof(target) ||
        snprintf(command, sizeof(command),
                 "until /bin/test -e %s; do /usr/bin/touch %s; "
                 "/usr/bin/true; done",
                 target, target) >= (int)sizeof(command)) {
        failed = 1;
        goto done;
    }
    test.section = "2.9.4.6";
    test.name = "native until iteration";
    test.status = 0;
    if (run_case(executable, &test, false) != 0 ||
        access(target, F_OK) == -1) {
        failed = 1;
    }

done:
    (void)unlink(target);
    (void)rmdir(directory);
    return failed;
}

static int native_positional_cases(const char *executable)
{
    static const char *const ten[] = {
        "one two", "",      "three", "four", "five",
        "six",     "seven", "eight", "nine", "ten"};
    static const char *const three[] = {"a b", "", "c"};
    syntax_case test = {
        "2.5.1", "native numbered positional parameters",
        "/usr/bin/printf '%s\\n' "
        "\"$0|$#|$1|${08}|${10}|$10|${#1}\"",
        0, "command-name|10|one two|eight|ten|one two0|7\n"};

    if (run_case_arguments(executable, &test, false, "command-name", ten,
                           sizeof(ten) / sizeof(ten[0])) != 0) {
        return 1;
    }
    test.section = "2.5.2";
    test.name = "quoted at preserves positional fields";
    test.command =
        "/usr/bin/printf '<%s>\\n' pre \"$@\" post";
    test.diagnostic = "<pre>\n<a b>\n<>\n<c>\n<post>\n";
    if (run_case_arguments(executable, &test, false, "command-name", three,
                           sizeof(three) / sizeof(three[0])) != 0) {
        return 1;
    }
    test.name = "unquoted at splits and removes empty fields";
    test.command = "/usr/bin/printf '<%s>\\n' pre $@ post";
    test.diagnostic = "<pre>\n<a>\n<b>\n<c>\n<post>\n";
    if (run_case_arguments(executable, &test, false, "command-name", three,
                           sizeof(three) / sizeof(three[0])) != 0) {
        return 1;
    }
    test.name = "embedded quoted at joins edge fields";
    test.command = "/usr/bin/printf '<%s>\\n' x\"$@\"y";
    test.diagnostic = "<xa b>\n<>\n<cy>\n";
    if (run_case_arguments(executable, &test, false, "command-name", three,
                           sizeof(three) / sizeof(three[0])) != 0) {
        return 1;
    }
    test.name = "quoted star uses first IFS character";
    test.command = "IFS=:; /usr/bin/printf '<%s>\\n' \"$*\"";
    test.diagnostic = "<a b::c>\n";
    if (run_case_arguments(executable, &test, false, "command-name", three,
                           sizeof(three) / sizeof(three[0])) != 0) {
        return 1;
    }
    test.section = "2.9.4.2";
    test.name = "for without in iterates quoted positionals";
    test.command =
        "for item; do /usr/bin/printf '<%s>\\n' \"$item\"; done";
    test.diagnostic = "<a b>\n<>\n<c>\n";
    if (run_case_arguments(executable, &test, false, "command-name", three,
                           sizeof(three) / sizeof(three[0])) != 0) {
        return 1;
    }
    test.name = "for without positionals has zero iterations";
    test.command = "for item; do /usr/bin/false; done";
    test.diagnostic = NULL;
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.section = "2.7.4";
    test.name = "here-document positional expansion";
    test.command = "/bin/cat <<EOF\n$0|$#|$1|$*\nEOF\n";
    test.diagnostic = "command-name|3|a b|a b  c\n";
    if (run_case_arguments(executable, &test, false, "command-name", three,
                           sizeof(three) / sizeof(three[0])) != 0) {
        return 1;
    }
    test.section = "2.6.3";
    test.name = "command substitution inherits positionals";
    test.command =
        "/usr/bin/printf '<%s>\\n' "
        "\"$(/usr/bin/printf '%s' \"$1\")\"";
    test.diagnostic = "<a b>\n";
    if (run_case_arguments(executable, &test, false, "command-name",
                           three, sizeof(three) / sizeof(three[0])) != 0) {
        return 1;
    }
    test.section = "set";
    test.name = "set delimiter replaces positional parameters";
    test.command =
        "set -- 'one two' '' three; "
        "/usr/bin/printf '<%s>\\n' \"$0|$#|$1|$2|$3\"";
    test.diagnostic = "<command-name|3|one two||three>\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "set operands replace positional parameters";
    test.command =
        "set alpha beta; /usr/bin/printf '%s\\n' \"$#|$1|$2\"";
    test.diagnostic = "2|alpha|beta\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "set delimiter clears positional parameters";
    test.command =
        "set -- old; set --; /usr/bin/printf '%s\\n' \"$#|$1\"";
    test.diagnostic = "0|\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "set quoted at preserves every field";
    test.command =
        "set -- \"$@\"; /usr/bin/printf '<%s>\\n' \"$@\"";
    test.diagnostic = "<a b>\n<>\n<c>\n";
    if (run_case_arguments(executable, &test, false, "command-name", three,
                           sizeof(three) / sizeof(three[0])) != 0) {
        return 1;
    }
    test.name = "special builtin leading assignment persists";
    test.command =
        "GSH_SET_LEADING=value set -- item; "
        "/usr/bin/printf '%s\\n' \"$GSH_SET_LEADING|$1\"";
    test.diagnostic = "value|item\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "set redirection preserves mutation";
    test.command =
        "set -- redirected >/dev/null; "
        "/usr/bin/printf '%s\\n' \"$1\"";
    test.diagnostic = "redirected\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "failed redirection precedes set mutation";
    test.command =
        "set -- outer; if set -- inner 2>/dev/null "
        ">/gsh/no/such/directory/output; then :; fi; "
        "/usr/bin/printf '%s\\n' \"$1\"";
    test.diagnostic = "outer\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "subshell set is isolated";
    test.command =
        "set -- outer; (set -- inner); /usr/bin/printf '%s\\n' \"$1\"";
    test.diagnostic = "outer\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "command substitution set is isolated";
    test.command =
        "set -- outer; : \"$(set -- inner)\"; "
        "/usr/bin/printf '%s\\n' \"$1\"";
    test.diagnostic = "outer\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "pipeline set is isolated";
    test.command =
        "set -- outer; set -- inner | true; "
        "/usr/bin/printf '%s\\n' \"$1\"";
    test.diagnostic = "outer\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "implicit for sees replaced positionals";
    test.command =
        "set -- a 'b c' ''; for item; do "
        "/usr/bin/printf '<%s>\\n' \"$item\"; done";
    test.diagnostic = "<a>\n<b c>\n<>\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.section = "shift";
    test.name = "shift default removes one parameter";
    test.command =
        "set -- a b c; shift; "
        "/usr/bin/printf '%s\\n' \"$#|$1|$2\"";
    test.diagnostic = "2|b|c\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "shift parses unsigned decimal with leading zero";
    test.command =
        "set -- a b c; shift 02; "
        "/usr/bin/printf '%s\\n' \"$#|$1\"";
    test.diagnostic = "1|c\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "shift zero leaves parameters unchanged";
    test.command =
        "set -- a b; shift 0; "
        "/usr/bin/printf '%s\\n' \"$#|$1|$2\"";
    test.diagnostic = "2|a|b\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "excessive shift is atomic";
    test.command =
        "set -- a b; shift 3 2>/dev/null; "
        "/usr/bin/printf '%s\\n' \"$?|$#|$1|$2\"";
    test.diagnostic = "1|2|a|b\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "invalid shift operand is atomic";
    test.command =
        "set -- a b; shift -1 2>/dev/null; "
        "/usr/bin/printf '%s\\n' \"$?|$#|$1|$2\"";
    test.diagnostic = "1|2|a|b\n";
    if (run_case_arguments(executable, &test, false, "command-name", NULL,
                           0) != 0) {
        return 1;
    }
    test.name = "shift rejects extra operands";
    test.command = "set -- a b; shift 1 2";
    test.status = 1;
    test.diagnostic = "gsh: shift: invalid shift count\n";
    return run_case_arguments(executable, &test, false, "command-name",
                              NULL, 0);
}

static int native_set_option_cases(const char *executable)
{
    static const syntax_case cases[] = {
        {"set", "short options and positionals are one transaction",
         "set -aCfu -- a b; /usr/bin/printf '%s|%s|%s\\n' "
         "\"$-\" \"$#\" \"$1\"", 0, "aCfu|2|a\n"},
        {"set", "named options enable and disable",
         "set -o allexport -o noclobber -o noglob -o nounset; "
         "set +o allexport +o noclobber +o noglob +o nounset; "
         "/usr/bin/printf '<%s>\\n' \"$-\"", 0, "<>\n"},
        {"set", "option status report",
         "set -aC; set -o", 0, "allexport\ton\n"},
        {"set", "nounset option status report",
         "set -u; set -o", 0, "nounset\ton\n"},
        {"set", "reusable option status report",
         "set -f; set +o", 0, "set -o noglob\n"},
        {"set", "reusable allexport and nounset status report",
         "set -au; set +o", 0, "set -o allexport\n"},
        {"set", "invalid short option is atomic",
         "set -au; set +au -z 2>/dev/null; "
         "/usr/bin/printf '<%s>\\n' \"$-\"", 0, "<au>\n"},
        {"set", "invalid named option is atomic",
         "set -C; set +C -o unknown 2>/dev/null; "
         "/usr/bin/printf '<%s>\\n' \"$-\"", 0, "<C>\n"},
        {"2.6.6", "noglob preserves pathname pattern",
         "set -f; /usr/bin/printf '<%s>\\n' /dev/n[uo]ll", 0,
         "</dev/n[uo]ll>\n"},
        {"2.6.6", "disabling noglob restores pathname expansion",
         "set -f; set +f; /usr/bin/printf '<%s>\\n' /dev/n[uo]ll",
         0, "</dev/null>\n"},
        {"2.13", "subshell option mutation is isolated",
         "set +f; (set -f); case \"$-\" in *f*) false;; *) true;; esac",
         0, NULL},
        {"2.6.3", "command substitution option mutation is isolated",
         "set +f; : \"$(set -f)\"; "
         "case \"$-\" in *f*) false;; *) true;; esac", 0, NULL},
        {"2.9.2", "pipeline option mutation is isolated",
         "set +f; set -f | true; "
         "case \"$-\" in *f*) false;; *) true;; esac", 0, NULL},
        {"set", "variable listing is sorted and reusable",
         "GSH_SET_AAA=\"a'b\"; GSH_SET_AAB=last; "
         "set | /usr/bin/grep '^GSH_SET_AA'", 0,
         "GSH_SET_AAA='a'\\''b'\nGSH_SET_AAB='last'\n"},
        {"set", "options can precede positional clearing",
         "set -- old; set -f --; /usr/bin/printf '%s|%s\\n' "
         "\"$#\" \"$-\"", 0, "0|f\n"},
        {"set -a", "allexport marks a simple assignment",
         "unset GSH_OPTION_SIMPLE; set -a; GSH_OPTION_SIMPLE=value; set +a; "
         "/usr/bin/printenv GSH_OPTION_SIMPLE", 0, "value\n"},
        {"set -a", "allexport marks a special builtin assignment",
         "unset GSH_OPTION_COLON; set -a; GSH_OPTION_COLON=value :; set +a; "
         "/usr/bin/printenv GSH_OPTION_COLON", 0, "value\n"},
        {"set -a", "allexport special builtin survives redirection",
         "unset GSH_OPTION_COLON_REDIRECT; set -a; "
         "GSH_OPTION_COLON_REDIRECT=value : >/dev/null; set +a; "
         "/usr/bin/printenv GSH_OPTION_COLON_REDIRECT", 0, "value\n"},
        {"set -a", "allexport marks parameter assignment",
         "set -a; unset GSH_OPTION_DEFAULT; "
         ": \"${GSH_OPTION_DEFAULT:=value}\"; set +a; "
         "/usr/bin/printenv GSH_OPTION_DEFAULT", 0, "value\n"},
        {"set -a", "allexport marks loop control variable",
         "unset GSH_OPTION_FOR; set -a; "
         "for GSH_OPTION_FOR in first last; do :; done; set +a; "
         "/usr/bin/printenv GSH_OPTION_FOR", 0, "last\n"},
        {"set -a", "allexport marks readonly assignment",
         "unset GSH_OPTION_READONLY; set -a; "
         "readonly GSH_OPTION_READONLY=value; set +a; "
         "/usr/bin/printenv GSH_OPTION_READONLY", 0, "value\n"},
        {"set -a", "allexport marks cd assignments",
         "set -a; unset OLDPWD; cd /; set +a; "
         "/usr/bin/printenv OLDPWD >/dev/null", 0, NULL},
        {"cd", "command assignment supplies local HOME",
         "HOME=/ cd; /bin/test \"$PWD\" = /; "
         "/bin/test \"$HOME\" = /tmp/gsh-conformance-home", 0, NULL},
        {"set -a", "allexport cd command environment remains local",
         "unset GSH_OPTION_CD_LOCAL; set -a; "
         "GSH_OPTION_CD_LOCAL=value cd .; set +a; "
         "if /usr/bin/printenv GSH_OPTION_CD_LOCAL >/dev/null; "
         "then false; else true; fi", 0, NULL},
        {"set -a", "command environment remains local",
         "unset GSH_OPTION_LOCAL; set -a; GSH_OPTION_LOCAL=inside "
         "/usr/bin/printenv GSH_OPTION_LOCAL; set +a; "
         "/usr/bin/printf '<%s>\\n' \"${GSH_OPTION_LOCAL-unset}\"",
         0, "inside\n<unset>\n"},
        {"set -a", "allexport is not retroactive",
         "unset GSH_OPTION_OLD; GSH_OPTION_OLD=value; set -a; "
         "if /usr/bin/printenv GSH_OPTION_OLD >/dev/null; "
         "then false; else true; fi", 0, NULL},
        {"set -a", "subshell allexport mutation is isolated",
         "unset GSH_OPTION_SUBSHELL; set +a; "
         "(set -a; GSH_OPTION_SUBSHELL=value); "
         "GSH_OPTION_SUBSHELL=parent; "
         "if /usr/bin/printenv GSH_OPTION_SUBSHELL >/dev/null; "
         "then false; else true; fi", 0, NULL},
        {"set -u", "nounset rejects unbraced unset parameter",
         "unset GSH_OPTION_UNSET; set -u; "
         ": \"$GSH_OPTION_UNSET\"; /usr/bin/printf BAD", 1,
         "GSH_OPTION_UNSET: parameter null or not set\n"},
        {"set -u", "nounset rejects braced unset parameter",
         "unset GSH_OPTION_BRACED; set -u; : \"${GSH_OPTION_BRACED}\"", 1,
         "GSH_OPTION_BRACED: parameter null or not set\n"},
        {"set -u", "nounset rejects unset parameter length",
         "unset GSH_OPTION_LENGTH; set -u; : \"${#GSH_OPTION_LENGTH}\"", 1,
         "GSH_OPTION_LENGTH: parameter null or not set\n"},
        {"set -u", "nounset rejects unset pattern removal",
         "unset GSH_OPTION_PATTERN; set -u; : \"${GSH_OPTION_PATTERN#a*}\"",
         1, "GSH_OPTION_PATTERN: parameter null or not set\n"},
        {"set -u", "nounset rejects missing positional parameter",
         "set -- only; set -u; : \"$2\"", 1,
         "2: parameter null or not set\n"},
        {"set -u", "parameter operators test unset safely",
         "set -u; unset GSH_OPTION_MODIFIER; "
         "/usr/bin/printf '<%s|%s|%s>\\n' "
         "\"${GSH_OPTION_MODIFIER-default}\" "
         "\"${GSH_OPTION_MODIFIER+alternate}\" "
         "\"${GSH_OPTION_MODIFIER:=assigned}\"", 0,
         "<default||assigned>\n"},
        {"set -u", "empty at and star are defined",
         "set -u --; /usr/bin/printf '<%s|%s>\\n' \"$@\" \"$*\"",
         0, "<|>\n"},
        {"2.5.2", "unset background pid expands empty by default",
         "/usr/bin/printf '<%s>\\n' \"$!\"", 0, "<>\n"},
        {"set -u", "nounset rejects absent background pid",
         "set -u; : \"$!\"", 1,
         "!: parameter null or not set\n"},
        {"set -u", "set but empty parameter is defined",
         "GSH_OPTION_EMPTY=; set -u; "
         "/usr/bin/printf '<%s>\\n' \"$GSH_OPTION_EMPTY\"", 0,
         "<>\n"},
        {"set -u", "nounset rejects evaluated arithmetic variable",
         "unset GSH_OPTION_ARITHMETIC; set -u; "
         ": \"$((GSH_OPTION_ARITHMETIC + 1))\"", 1,
         "GSH_OPTION_ARITHMETIC: parameter null or not set\n"},
        {"set -u", "arithmetic short circuit skips bare variable",
         "unset GSH_OPTION_ARITHMETIC_SKIPPED; set -u; "
         "/usr/bin/printf '<%s>\\n' "
         "\"$((1 || GSH_OPTION_ARITHMETIC_SKIPPED))\"", 0, "<1>\n"},
        {"set -u", "explicit arithmetic expansion precedes short circuit",
         "unset GSH_OPTION_ARITHMETIC_EXPLICIT; set -u; "
         ": \"$((1 || $GSH_OPTION_ARITHMETIC_EXPLICIT))\"",
         1, "GSH_OPTION_ARITHMETIC_EXPLICIT: parameter null or not set\n"},
        {"set -u", "nounset applies to unquoted here-document",
         "unset GSH_OPTION_HEREDOC; set -u; "
         "/bin/cat <<EOF\n$GSH_OPTION_HEREDOC\nEOF\n"
         "/usr/bin/printf BAD", 1,
         "GSH_OPTION_HEREDOC: parameter null or not set\n"},
        {"set -u", "quoted here-document suppresses nounset expansion",
         "unset GSH_OPTION_HEREDOC; set -u; "
         "/bin/cat <<'EOF'\n$GSH_OPTION_HEREDOC\nEOF", 0,
         "$GSH_OPTION_HEREDOC\n"},
        {"set -u", "disabling nounset restores empty expansion",
         "unset GSH_OPTION_DISABLED; set -u; set +u; "
         "/usr/bin/printf '<%s>\\n' "
         "\"$GSH_OPTION_DISABLED\"", 0, "<>\n"},
        {"set -u", "pipeline expansion error is isolated",
         "unset GSH_OPTION_PIPE; set -u; "
         ": \"$GSH_OPTION_PIPE\" | true; "
         "/usr/bin/printf GSH_NOUNSET_PARENT", 0,
         "GSH_NOUNSET_PARENT"},
    };
    char directory[] = "/tmp/gsh-native-set-XXXXXX";
    char target[1024];
    char link[1024];
    char created[1024];
    char command[4096];
    syntax_case test = {"2.7.2", "noclobber rejects regular file",
                        command, 0, NULL};
    size_t index;
    int descriptor = -1;
    int failed = 0;

    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (run_case(executable, &cases[index], false) != 0) {
            return 1;
        }
    }
    if (mkdtemp(directory) == NULL ||
        snprintf(target, sizeof(target), "%s/target", directory) >=
            (int)sizeof(target) ||
        snprintf(link, sizeof(link), "%s/link", directory) >=
            (int)sizeof(link) ||
        snprintf(created, sizeof(created), "%s/created", directory) >=
            (int)sizeof(created) ||
        (descriptor = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0600)) ==
            -1 || write(descriptor, "original", 8) != 8 ||
        close(descriptor) == -1 || symlink(target, link) == -1) {
        if (descriptor >= 0) {
            close(descriptor);
        }
        return 1;
    }
    descriptor = -1;
    if (snprintf(command, sizeof(command),
                 "set -C; if /usr/bin/printf changed 2>/dev/null >%s; "
                 "then false; fi; /bin/test \"$(/bin/cat %s)\" = original",
                 target, target) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }
    test.name = "clobber operator overrides noclobber";
    if (snprintf(command, sizeof(command),
                 "set -C; /usr/bin/printf changed >|%s; "
                 "/bin/test \"$(/bin/cat %s)\" = changed",
                 target, target) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }
    test.name = "noclobber creates absent file atomically";
    if (snprintf(command, sizeof(command),
                 "set -C; /usr/bin/printf new >%s; "
                 "/bin/test \"$(/bin/cat %s)\" = new",
                 created, created) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }
    test.name = "append redirection ignores noclobber";
    if (snprintf(command, sizeof(command),
                 "/usr/bin/printf base >|%s; set -C; "
                 "/usr/bin/printf plus >>%s; "
                 "/bin/test \"$(/bin/cat %s)\" = baseplus",
                 target, target, target) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }
    test.name = "noclobber follows symlink to regular file";
    if (snprintf(command, sizeof(command),
                 "set -C; if /usr/bin/printf bad 2>/dev/null >%s; "
                 "then false; fi; /bin/test \"$(/bin/cat %s)\" = baseplus",
                 link, target) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }
    test.name = "failed redirection precedes option mutation";
    if (snprintf(command, sizeof(command),
                 "set -C; set +C 2>/dev/null >%s; "
                 "case \"$-\" in *C*) true;; *) false;; esac",
                 target) >= (int)sizeof(command) ||
        run_case(executable, &test, false) != 0) {
        failed = 1;
        goto done;
    }
    test.name = "noclobber permits non-regular output";
    test.command = "set -C; /usr/bin/printf x >/dev/null";
    if (run_case(executable, &test, false) != 0) {
        failed = 1;
    }

done:
    (void)unlink(link);
    (void)unlink(created);
    (void)unlink(target);
    (void)rmdir(directory);
    return failed;
}

static int native_pwd_cases(const char *executable)
{
    char directory[4096];
    char expected[4100];
    syntax_case test = {"pwd", "native logical pwd", "pwd", 0, expected};

    if (getcwd(directory, sizeof(directory)) == NULL ||
        snprintf(expected, sizeof(expected), "%s\n", directory) >=
            (int)sizeof(expected) ||
        run_case(executable, &test, false) != 0) {
        return 1;
    }
    test.name = "native physical pwd in pipeline";
    test.command = "pwd -P | /usr/bin/tail -n 1";
    return run_case(executable, &test, false);
}

int main(int argc, char **argv)
{
    static const syntax_case cases[] = {
        {"2.1", "empty program", "", 0, NULL},
        {"2.2", "single and double quoting", "printf '%s' \"a b\"", 0,
         NULL},
        {"2.2.4", "dollar single quotes", "printf $'a\\n'", 0, NULL},
        {"2.3", "comment and line joining", "# comment\ntrue \\\n&& false",
         0, NULL},
        {"2.7", "descriptor redirections", "cmd 2>err 3<>data 4>&1 >|out",
         0, NULL},
        {"2.9.2", "pipeline and negation", "! a |\n b | c", 0, NULL},
        {"2.9.3", "and-or and async lists", "a && b || c; d &\n", 0,
         NULL},
        {"2.9.4.1", "subshell", "(a; b) | c", 0, NULL},
        {"2.9.4.1", "brace group", "{ a; b; } >out", 0, NULL},
        {"2.9.4.4", "if elif else",
         "if a; then b; elif c; then d; else e; fi", 0, NULL},
        {"2.9.4.5", "while loop", "while a; do b; done", 0, NULL},
        {"2.9.4.6", "until loop", "until a; do b; done", 0, NULL},
        {"2.9.4.2", "for explicit items",
         "for item in a b; do echo $item; done", 0, NULL},
        {"2.9.4.2", "for positional items", "for item; do echo $item; done",
         0, NULL},
        {"2.9.5", "function definition", "f() { echo x; } 2>err", 0,
         NULL},
        {"2.9.1", "assignments and command", "A=1 B=$A command arg", 0,
         NULL},
        {"2.2", "unterminated quote", "echo 'x", 2, "incomplete"},
        {"2.9.2", "dangling pipeline", "a |", 2, "incomplete"},
        {"2.9.4.4", "missing fi", "if a; then b", 2, "incomplete"},
        {"2.9.4.5", "missing do", "while a; b; done", 2, "syntax"},
        {"2.9.4.3", "case clause", "case x in x) :;; esac", 0, NULL},
        {"2.9.4.3", "empty case", "case x in esac", 0, NULL},
        {"2.9.4.3", "case patterns and fallthrough",
         "case x in (a|b) : ;& x) :;; esac", 0, NULL},
        {"2.9.4.3", "case final clause without terminator",
         "case x in x) :\nesac", 0, NULL},
        {"2.7.4", "here-document", "cat <<EOF\ntext\nEOF\n", 0,
         NULL},
        {"2.7.4", "quoted here-document delimiter",
         "cat <<'EOF'\ntext\nEOF\n", 0, NULL},
        {"2.7.4", "tab-stripped here-document",
         "cat <<-EOF\n\ttext\n\tEOF\n", 0, NULL},
        {"2.7.4", "multiple here-documents",
         "cat <<FIRST <<SECOND\none\nFIRST\ntwo\nSECOND\n", 0, NULL},
        {"2.7.4", "unterminated here-document", "cat <<EOF\ntext\n", 2,
         "incomplete"},
    };
    static const syntax_case execution_cases[] = {
        {"2.1", "native empty program", "", 0, NULL},
        {"2.2", "native quote removal",
         "/usr/bin/printf '<%s>\\n' 'a b'", 0, "<a b>\n"},
        {"2.2.4", "native dollar-single escapes",
         "/usr/bin/printf '<%s>\\n' $'a\\nb'", 0, "<a\nb>\n"},
        {"2.2.4", "native dollar-single numeric escapes",
         "/usr/bin/printf '<%s>\\n' $'\\x41\\101'", 0, "<AA>\n"},
        {"2.2.3", "dollar-single marker literal in double quotes",
         "/usr/bin/printf '<%s>\\n' \"$'x'\"", 0, "<$'x'>\n"},
        {"2.2.4", "dollar-single NUL policy rejected",
         "/usr/bin/printf x $'\\0discard'", 2,
         "native execution unsupported"},
        {"2.9.2", "native concurrent pipeline",
         "/usr/bin/yes parallel | /usr/bin/head -n 1", 0,
         "parallel\n"},
        {"2.9.2", "native pipeline transform",
         "/usr/bin/printf x | /usr/bin/tr x y", 0, "y"},
        {"2.9.2", "pipeline status from last command",
         "/usr/bin/false | /usr/bin/true", 0, NULL},
        {"2.9.2", "pipeline nonzero status",
         "/usr/bin/true | /usr/bin/false", 1, NULL},
        {"2.13", "pipeline stage parameter isolation",
         ": \"${GSH_PIPE_LEFT:=left}\" | "
         "/bin/test -z \"$GSH_PIPE_LEFT\"; "
         "/bin/test -z \"$GSH_PIPE_LEFT\"",
         0, NULL},
        {"2.13", "pipeline same-stage parameter visibility",
         "/usr/bin/printf '<%s><%s>\\n' "
         "\"${GSH_PIPE_SAME:=left}\" \"$GSH_PIPE_SAME\" | /bin/cat",
         0, "<left><left>\n"},
        {"2.8.1", "non-last pipeline expansion error is isolated",
         ": \"${GSH_PIPE_ERROR:?left failed}\" | /usr/bin/true",
         0, "left failed"},
        {"2.8.1", "last pipeline expansion error sets status",
         "/usr/bin/true | : \"${GSH_PIPE_LAST:?last failed}\"",
         1, "last failed"},
        {"2.9.2", "negated pipeline", "! /usr/bin/true", 1, NULL},
        {"2.9.3", "native AND short circuit",
         "/usr/bin/false && /usr/bin/true", 1, NULL},
        {"2.9.3", "native OR short circuit",
         "/usr/bin/true || /usr/bin/false", 0, NULL},
        {"2.9.3", "native AND right operand",
         "/usr/bin/true && /usr/bin/false", 1, NULL},
        {"2.9.3", "native OR right operand",
         "/usr/bin/false || /usr/bin/true", 0, NULL},
        {"2.9.3", "native sequential list",
         "/usr/bin/false; /usr/bin/true", 0, NULL},
        {"2.9.3.1", "native asynchronous PID and wait",
         "/bin/sleep 0.02 & pid=$!; /bin/test \"$pid\" -gt 0; "
         "wait \"$pid\"",
         0, NULL},
        {"wait", "wait returns asynchronous status",
         "/bin/sh -c 'exit 7' & pid=$!; wait \"$pid\"; "
         "/bin/test \"$?\" -eq 7",
         0, NULL},
        {"wait", "wait unknown PID status",
         "wait 999999; /bin/test \"$?\" -eq 127", 0, NULL},
        {"wait", "wait multiple returns last status",
         "/bin/sh -c 'exit 3' & first=$!; "
         "/bin/sh -c 'exit 7' & second=$!; "
         "wait \"$first\" \"$second\"; /bin/test \"$?\" -eq 7",
         0, NULL},
        {"wait", "wait all returns zero",
         "/usr/bin/false & wait; /bin/test \"$?\" -eq 0", 0, NULL},
        {"2.13", "asynchronous variable isolation",
         "GSH_ASYNC_SCOPE=parent; GSH_ASYNC_SCOPE=child : & wait; "
         "/bin/test \"$GSH_ASYNC_SCOPE\" = parent",
         0, NULL},
        {"2.9.3.1", "asynchronous standard input is null",
         "/bin/cat & wait \"$!\"", 0, NULL},
        {"wait", "wait accepts background job notation",
         "/usr/bin/true & wait %1", 0, NULL},
        {"2.9.4.1", "native brace group",
         "{ /usr/bin/false; /usr/bin/true; }", 0, NULL},
        {"2.9.4.1", "native subshell",
         "( /usr/bin/printf subshell )", 0, "subshell"},
        {"2.9.4.4", "native if else",
         "if /usr/bin/false; then /usr/bin/false; else /usr/bin/true; fi",
         0, NULL},
        {"2.9.4.4", "native selected branch status",
         "if /usr/bin/true; then /usr/bin/false; else /usr/bin/true; fi",
         1, NULL},
        {"2.9.4.4", "native if without selected branch",
         "if /usr/bin/false; then /usr/bin/true; fi", 0, NULL},
        {"2.9.4.5", "native while with no iteration",
         "while /usr/bin/false; do /usr/bin/false; done", 0, NULL},
        {"2.9.4.6", "native until with no iteration",
         "until /usr/bin/true; do /usr/bin/false; done", 0, NULL},
        {"2.9.4.2", "native for explicit fields",
         "for item in a 'b c' ''; do "
         "/usr/bin/printf '<%s>\\n' \"$item\"; done",
         0, "<a>\n<b c>\n<>\n"},
        {"2.9.4.2", "native for empty item list",
         "for item in; do /usr/bin/false; done", 0, NULL},
        {"2.9.4.2", "native for status is last body status",
         "for item in a b; do /bin/test \"$item\" = a; done", 1,
         NULL},
        {"2.9.4.2", "native for leaves final variable value",
         "for item in a b; do :; done; /bin/test \"$item\" = b", 0,
         NULL},
        {"2.9.4.2", "native for expands item list once",
         "GSH_FOR_EXPANDED=; "
         "for item in ${GSH_FOR_EXPANDED:=a} \"$GSH_FOR_EXPANDED\"; "
         "do /usr/bin/printf '<%s>\\n' \"$item\"; done",
         0, "<a>\n<a>\n"},
        {"2.9.4.2", "native nested for loops",
         "for outer in a b; do for inner in 1 2; do "
         "/usr/bin/printf '%s%s\\n' \"$outer\" \"$inner\"; "
         "done; done",
         0, "a1\na2\nb1\nb2\n"},
        {"break", "break leaves the current loop",
         "for item in a b; do "
         "/usr/bin/printf '<%s>' \"$item\"; break; "
         "/usr/bin/printf BAD; done; /usr/bin/printf END",
         0, "<a>END"},
        {"continue", "continue skips the remaining loop body",
         "for item in a b; do /usr/bin/printf '<%s>' \"$item\"; "
         "continue; /usr/bin/printf BAD; done; /usr/bin/printf END",
         0, "<a><b>END"},
        {"break", "oversized break count leaves the outermost loop",
         "for outer in a b; do for inner in 1 2; do "
         "/usr/bin/printf '<%s%s>' \"$outer\" \"$inner\"; break 99; "
         "done; /usr/bin/printf BAD; done; /usr/bin/printf END",
         0, "<a1>END"},
        {"continue", "continue count advances the selected outer loop",
         "for outer in a b; do for inner in 1 2; do "
         "/usr/bin/printf '<%s%s>' \"$outer\" \"$inner\"; continue 2; "
         "done; /usr/bin/printf BAD; done; /usr/bin/printf END",
         0, "<a1><b1>END"},
        {"break", "break assignment persists in the shell",
         "for item in one; do GSH_BREAK_STATE=value break; done; "
         "/usr/bin/printf '%s' \"$GSH_BREAK_STATE\"",
         0, "value"},
        {"break", "break applies and restores redirections",
         "GSH_BREAK_PATH=/tmp/gsh-break-redirection-$$; "
         "for item in one; do break >\"$GSH_BREAK_PATH\"; done; "
         "/bin/test -f \"$GSH_BREAK_PATH\"; GSH_BREAK_FILE_STATUS=$?; "
         "/bin/rm -f \"$GSH_BREAK_PATH\"; "
         "/bin/test \"$GSH_BREAK_FILE_STATUS\" -eq 0",
         0, NULL},
        {"continue", "continue propagates through conditional lists",
         "for item in a b; do /usr/bin/true && continue; "
         "/usr/bin/printf BAD; done; /usr/bin/printf END",
         0, "END"},
        {"continue", "continue advances a while loop",
         "GSH_WHILE_COUNT=0; while /bin/test \"$GSH_WHILE_COUNT\" -lt 3; "
         "do GSH_WHILE_COUNT=$((GSH_WHILE_COUNT + 1)); "
         "if /bin/test \"$GSH_WHILE_COUNT\" -eq 2; then continue; fi; "
         "/usr/bin/printf '<%s>' \"$GSH_WHILE_COUNT\"; done; "
         "/usr/bin/printf END",
         0, "<1><3>END"},
        {"break", "function-local break leaves a function-local loop",
         "stop_first() { for item in a b; do "
         "/usr/bin/printf '<%s>' \"$item\"; break; done; }; "
         "stop_first; /usr/bin/printf END",
         0, "<a>END"},
        {"2.9.4.3", "native case wildcard",
         "case atom in a*) /usr/bin/true;; *) /usr/bin/false;; esac", 0,
         NULL},
        {"2.9.4.3", "native case multiple patterns",
         "case beta in alpha|beta) /usr/bin/true;; *) /usr/bin/false;; "
         "esac",
         0, NULL},
        {"2.9.4.3", "native case expanded subject",
         "case \"$GSH_CONFORMANCE_ATOM\" in atom) /usr/bin/true;; *) "
         "/usr/bin/false;; esac",
         0, NULL},
        {"2.9.4.3", "native case no match status",
         "case x in y) /usr/bin/false;; esac", 0, NULL},
        {"2.9.4.3", "native case selected status",
         "case x in x) /usr/bin/false;; esac", 1, NULL},
        {"2.9.4.3", "native case fallthrough",
         "case x in x) /usr/bin/false ;& y) /usr/bin/true;; esac", 0,
         NULL},
        {"2.9.4.3", "quoted case pattern",
         "case x in 'x') /usr/bin/true;; esac", 0, NULL},
        {"2.9.4.3", "quoted wildcard is literal",
         "case abc in \"*\") /usr/bin/false;; *) /usr/bin/true;; esac",
         0, NULL},
        {"2.9.4.3", "quoted wildcard literal match",
         "case '*' in \"*\") /usr/bin/true;; *) /usr/bin/false;; esac",
         0, NULL},
        {"2.9.4.3", "escaped case metacharacter",
         "case '?' in \\?) /usr/bin/true;; *) /usr/bin/false;; esac",
         0, NULL},
        {"2.9.1", "native command-not-found status",
         "gsh-command-that-does-not-exist", 127, "command not found"},
        {"2.6.3", "native quoted environment parameter",
         "/usr/bin/printf '<%s>\\n' \"$GSH_CONFORMANCE_VALUE\"", 0,
         "<alpha beta>\n"},
        {"2.6.3", "native braced scalar parameter",
         "/usr/bin/printf '<%s>\\n' ${GSH_CONFORMANCE_ATOM}", 0,
         "<atom>\n"},
        {"2.6.3", "native quoted unset parameter",
         "/usr/bin/printf '<%s>\\n' \"$GSH_CONFORMANCE_UNSET\"", 0,
         "<>\n"},
        {"2.6.1", "native tilde expansion",
         "/usr/bin/printf '<%s>\\n' ~/leaf", 0,
         "</tmp/gsh-conformance-home/leaf>\n"},
        {"2.6.1", "quoted tilde remains literal",
         "/usr/bin/printf '<%s>\\n' \"~/leaf\"", 0, "<~/leaf>\n"},
        {"2.6.1", "embedded tilde remains literal",
         "/usr/bin/printf '<%s>\\n' prefix~suffix", 0,
         "<prefix~suffix>\n"},
        {"2.6.1", "tilde in command assignment",
         "GSH_ASSIGN_HOME=~/leaf /usr/bin/env", 0,
         "GSH_ASSIGN_HOME=/tmp/gsh-conformance-home/leaf\n"},
        {"2.6.5", "native field splitting",
         "/usr/bin/printf '<%s>\\n' $GSH_CONFORMANCE_VALUE", 0,
         "<alpha>\n<beta>\n"},
        {"2.6.5", "unset unquoted expansion removes field",
         "/usr/bin/printf '<%s>\\n' before $GSH_CONFORMANCE_UNSET after",
         0, "<before>\n<after>\n"},
        {"2.6.5", "command substitution field splitting",
         "/usr/bin/printf '<%s>\\n' "
         "$(/usr/bin/printf 'one two')",
         0, "<one>\n<two>\n"},
        {"2.6", "mixed-word expansion provenance",
         "/usr/bin/printf '<%s>\\n' "
         "\"pre\"$GSH_CONFORMANCE_VALUE\"post\"",
         0, "<prealpha>\n<betapost>\n"},
        {"2.6.5", "literal IFS bytes are not split",
         "IFS=:; GSH_MIXED='a:b'; "
         "/usr/bin/printf '<%s>\\n' pre:$GSH_MIXED:post",
         0, "<pre:a>\n<b:post>\n"},
        {"2.6.5", "trailing quoted empty field",
         "IFS=' '; GSH_MIXED='a '; "
         "/usr/bin/printf '<%s>\\n' $GSH_MIXED\"\"",
         0, "<a>\n<>\n"},
        {"2.6.5", "leading quote does not delimit empty field",
         "IFS=' '; GSH_MIXED=' a'; "
         "/usr/bin/printf '<%s>\\n' \"\"$GSH_MIXED",
         0, "<a>\n"},
        {"2.6", "mixed command substitution fields",
         "/usr/bin/printf '<%s>\\n' "
         "pre$(/usr/bin/printf 'one two')post",
         0, "<preone>\n<twopost>\n"},
        {"2.6", "mixed arithmetic expansion",
         "/usr/bin/printf '<%s>\\n' x$((2 + 3))y", 0, "<x5y>\n"},
        {"2.6.2", "unquoted unset in mixed word",
         "/usr/bin/printf '<%s>\\n' "
         "pre${GSH_CONFORMANCE_UNSET}post",
         0, "<prepost>\n"},
        {"2.6.2", "parameter default for unset",
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_CONFORMANCE_UNSET:-default}\"",
         0, "<default>\n"},
        {"2.6.2", "parameter colon null distinction",
         "GSH_PARAMETER_EMPTY=; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PARAMETER_EMPTY:-colon}\" "
         "\"${GSH_PARAMETER_EMPTY-nocolon}\"",
         0, "<colon>\n<>\n"},
        {"2.6.2", "parameter alternative values",
         "GSH_PARAMETER_EMPTY=; GSH_PARAMETER_SET=value; "
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_PARAMETER_EMPTY:+colon}\" "
         "\"${GSH_PARAMETER_EMPTY+set}\" "
         "\"${GSH_PARAMETER_SET:+alternate}\"",
         0, "<>\n<set>\n<alternate>\n"},
        {"2.6.2", "nested parameter default",
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_CONFORMANCE_UNSET:-"
         "${GSH_CONFORMANCE_UNSET_TOO:-nested}}\"",
         0, "<nested>\n"},
        {"2.6.2", "lazy parameter word",
         "GSH_PARAMETER_SET=value; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PARAMETER_SET:-$(/usr/bin/printf BAD)}\"",
         0, "<value>\n"},
        {"2.6.2", "quoted default suppresses splitting",
         "/usr/bin/printf '<%s>\\n' "
         "${GSH_CONFORMANCE_UNSET:-\"alpha beta\"}",
         0, "<alpha beta>\n"},
        {"2.6.2", "unquoted default participates in splitting",
         "/usr/bin/printf '<%s>\\n' "
         "pre${GSH_CONFORMANCE_UNSET:-alpha beta}post",
         0, "<prealpha>\n<betapost>\n"},
        {"2.6.2", "parameter word command substitution",
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_CONFORMANCE_UNSET:-$(/usr/bin/printf dynamic)}\"",
         0, "<dynamic>\n"},
        {"2.6.2", "parameter character length",
         "/usr/bin/printf '<%s>\\n' "
         "\"${#GSH_CONFORMANCE_UTF8}\"",
         0, "<2>\n"},
        {"2.6.2", "four parameter pattern removals",
         "GSH_PATTERN_VALUE=abcabc; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#*b}\" "
         "\"${GSH_PATTERN_VALUE##*b}\" "
         "\"${GSH_PATTERN_VALUE%c*}\" "
         "\"${GSH_PATTERN_VALUE%%c*}\"",
         0, "<cabc>\n<c>\n<abcab>\n<ab>\n"},
        {"2.6.2", "empty and unmatched removal patterns",
         "GSH_PATTERN_VALUE=abc; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#z*}\" "
         "\"${GSH_PATTERN_VALUE%z*}\" "
         "\"${GSH_PATTERN_VALUE#}\" "
         "\"${GSH_PATTERN_VALUE%}\"",
         0, "<abc>\n<abc>\n<abc>\n<abc>\n"},
        {"2.6.2", "star pattern shortest and longest matches",
         "GSH_PATTERN_VALUE=abc; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#*}\" "
         "\"${GSH_PATTERN_VALUE##*}\" "
         "\"${GSH_PATTERN_VALUE%*}\" "
         "\"${GSH_PATTERN_VALUE%%*}\"",
         0, "<abc>\n<>\n<abc>\n<>\n"},
        {"2.6.2", "anchored single-star removal",
         "GSH_PATTERN_VALUE=preXXsufpreYYsuf; "
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#pre*suf}\" "
         "\"${GSH_PATTERN_VALUE##pre*suf}\" "
         "\"${GSH_PATTERN_VALUE%pre*suf}\" "
         "\"${GSH_PATTERN_VALUE%%pre*suf}\"",
         0, "<preYYsuf>\n<>\n<preXXsuf>\n<>\n"},
        {"2.6.2", "single-star suffix begins after subject start",
         "GSH_PATTERN_VALUE=xxpreAsufpreBsuf; "
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE%pre*suf}\" "
         "\"${GSH_PATTERN_VALUE%%pre*suf}\"",
         0, "<xxpreAsuf>\n<xx>\n"},
        {"2.6.2", "multi-star shortest removals",
         "GSH_PATTERN_VALUE=abacadTAIL; "
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#*a*d}\" "
         "\"${GSH_PATTERN_VALUE%a*c*}\" "
         "\"${GSH_PATTERN_VALUE#a**b}\"",
         0, "<TAIL>\n<ab>\n<acadTAIL>\n"},
        {"2.6.2", "outer and inner pattern quoting",
         "GSH_PATTERN_VALUE='*abc'; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#'*'}\" "
         "\"${GSH_PATTERN_VALUE#*}\"; "
         "GSH_PATTERN_VALUE=abc; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#*b}\" "
         "\"${GSH_PATTERN_VALUE#\"*b\"}\"",
         0, "<abc>\n<*abc>\n<c>\n<abc>\n"},
        {"2.6.2", "expanded pattern quoting",
         "GSH_PATTERN_VALUE=abcabc; GSH_PATTERN_WORD='a*'; "
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#$GSH_PATTERN_WORD}\" "
         "\"${GSH_PATTERN_VALUE#\"$GSH_PATTERN_WORD\"}\"",
         0, "<bcabc>\n<abcabc>\n"},
        {"2.6.2", "bracket removal patterns",
         "GSH_PATTERN_VALUE=abc123; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#[a-c][a-c]}\" "
         "\"${GSH_PATTERN_VALUE%%[0-9]*}\"",
         0, "<c123>\n<abc>\n"},
        {"2.6.2", "pattern word assignment preserves subject",
         "GSH_PATTERN_VALUE=abcabc; unset GSH_PATTERN_WORD; "
         "/usr/bin/printf '<%s|%s>\\n' "
         "\"${GSH_PATTERN_VALUE#${GSH_PATTERN_WORD:=a*}}\" "
         "\"$GSH_PATTERN_WORD\"",
         0, "<bcabc|a*>\n"},
        {"2.6.2", "command and arithmetic pattern expansion",
         "GSH_PATTERN_VALUE=abcabc; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#$(/usr/bin/printf 'a*')}\"; "
         "GSH_PATTERN_VALUE=1234; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#$((1))*}\"",
         0, "<bcabc>\n<234>\n"},
        {"2.6.2", "pattern removal observes character boundaries",
         "GSH_PATTERN_VALUE='ééx'; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#?}\" "
         "\"${GSH_PATTERN_VALUE%?}\"",
         0, "<éx>\n<éé>\n"},
        {"2.6.2", "unquoted removal result field splitting",
         "GSH_PATTERN_VALUE='a b c'; GSH_PATTERN_WORD='a '; "
         "/usr/bin/printf '<%s>\\n' ${GSH_PATTERN_VALUE#$GSH_PATTERN_WORD}; "
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_VALUE#$GSH_PATTERN_WORD}\"",
         0, "<b>\n<c>\n<b c>\n"},
        {"2.6.2", "unset pattern subject is empty without nounset",
         "unset GSH_PATTERN_UNSET; /usr/bin/printf '<%s>\\n' "
         "\"${GSH_PATTERN_UNSET#*}\"",
         0, "<>\n"},
        {"2.6.2", "parameter assign default persists",
         ": \"${GSH_PARAMETER_ASSIGNED:=assigned}\"; "
         "/usr/bin/printf '<%s>\\n' \"$GSH_PARAMETER_ASSIGNED\"",
         0, "<assigned>\n"},
        {"2.6.2", "parameter assign colon null distinction",
         "GSH_PARAMETER_ASSIGN_EMPTY=; "
         ": \"${GSH_PARAMETER_ASSIGN_EMPTY=ignored}\"; "
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_PARAMETER_ASSIGN_EMPTY:=filled}\" "
         "\"$GSH_PARAMETER_ASSIGN_EMPTY\"",
         0, "<filled>\n<filled>\n"},
        {"2.6.2", "parameter assign word is lazy",
         "GSH_PARAMETER_ASSIGN_SET=value; "
         ": \"${GSH_PARAMETER_ASSIGN_SET:="
         "$(/usr/bin/printf BAD)}\"; "
         "/usr/bin/printf '<%s>\\n' \"$GSH_PARAMETER_ASSIGN_SET\"",
         0, "<value>\n"},
        {"2.6.2", "parameter question accepts set null",
         "GSH_PARAMETER_QUESTION_EMPTY=; "
         "/usr/bin/printf '<%s>\\n' "
         "\"${GSH_PARAMETER_QUESTION_EMPTY?bad}\"",
         0, "<>\n"},
        {"2.6.2", "parameter question rejects unset",
         ": \"${GSH_PARAMETER_QUESTION_UNSET?required value}\"; "
         "/usr/bin/printf BAD",
         1, "required value"},
        {"2.6.2", "parameter colon question rejects null",
         "GSH_PARAMETER_QUESTION_NULL=; "
         ": \"${GSH_PARAMETER_QUESTION_NULL:?must not be null}\"; "
         "/usr/bin/printf BAD",
         1, "must not be null"},
        {"2.13", "subshell variable assignment isolation",
         "(GSH_SUBSHELL_LOCAL=hidden); "
         "/bin/test -z \"$GSH_SUBSHELL_LOCAL\"",
         0, NULL},
        {"2.13", "subshell parameter assignment isolation",
         "(: \"${GSH_SUBSHELL_PARAMETER:=hidden}\"); "
         "/bin/test -z \"$GSH_SUBSHELL_PARAMETER\"",
         0, NULL},
        {"2.13", "command substitution assignment isolation",
         ": \"$(GSH_SUBSTITUTION_LOCAL=hidden)\"; "
         "/bin/test -z \"$GSH_SUBSTITUTION_LOCAL\"",
         0, NULL},
        {"2.6", "literal dollar at word end",
         "/usr/bin/printf '<%s>\\n' $", 0, "<$>\n"},
        {"2.6.7", "quoted empty word is retained",
         "/usr/bin/printf '<%s>\\n' \"\"", 0, "<>\n"},
        {"2.6.6", "native pathname expansion",
         "/usr/bin/printf '<%s>\\n' /dev/n[uo]ll", 0,
         "</dev/null>\n"},
        {"2.6.6", "quoted pathname pattern remains literal",
         "/usr/bin/printf '<%s>\\n' '/dev/n[uo]ll'", 0,
         "</dev/n[uo]ll>\n"},
        {"2.6.6", "unmatched pathname pattern remains literal",
         "/usr/bin/printf '<%s>\\n' /gsh-no-match-*.none", 0,
         "</gsh-no-match-*.none>\n"},
        {"2.6.6", "pathname expansion from parameter result",
         "/usr/bin/printf '<%s>\\n' $GSH_CONFORMANCE_PATTERN", 0,
         "</dev/null>\n"},
        {"2.6.6", "mixed parameter and pathname pattern",
         "GSH_MIXED=/dev/n; "
         "/usr/bin/printf '<%s>\\n' $GSH_MIXED[uo]ll",
         0, "</dev/null>\n"},
        {"2.6.6", "quoted metacharacter in mixed pattern",
         "/usr/bin/printf '<%s>\\n' /dev/n[uo]'*'", 0,
         "</dev/n[uo]*>\n"},
        {"2.6.3", "native quoted command substitution",
         "/usr/bin/printf '%s\\n' \"$(/usr/bin/printf x)\"", 0,
         "x\n"},
        {"2.6.3", "command substitution trims trailing newlines",
         "/usr/bin/printf '<%s>\\n' "
         "\"$(/usr/bin/printf 'x\\n\\n')\"",
         0, "<x>\n"},
        {"2.6.3", "nested command substitution",
         "/usr/bin/printf '<%s>\\n' "
         "\"$(/usr/bin/printf '%s' "
         "\"$(/usr/bin/printf nested)\")\"",
         0, "<nested>\n"},
        {"2.6.3", "command substitution in assignment value",
         "GSH_SUBSTITUTED=\"$(/usr/bin/printf assigned)\" /usr/bin/env",
         0, "GSH_SUBSTITUTED=assigned\n"},
        {"2.9.1/2.6.3", "assignment-only status follows substitution",
         "GSH_SUBSTITUTED=$(exit 6); /bin/test \"$?\" -eq 6", 0, NULL},
        {"2.9.1/2.6.3", "last substitution determines assignment status",
         "GSH_SUBSTITUTED=$(exit 4)$(exit 5); "
         "/bin/test \"$?\" -eq 5",
         0, NULL},
        {"2.9.1/2.6.3", "negation uses assignment substitution status",
         "! GSH_SUBSTITUTED=$(exit 8); /bin/test \"$?\" -eq 0", 0,
         NULL},
        {"2.6.3", "native backquoted command substitution",
         "/usr/bin/printf '<%s>\\n' `/usr/bin/printf legacy`", 0,
         "<legacy>\n"},
        {"2.9.1/2.6.3", "backquote determines assignment status",
         "GSH_SUBSTITUTED=`exit 7`; /bin/test \"$?\" -eq 7", 0, NULL},
        {"2.6.3", "quoted backquote trims trailing newlines",
         "/usr/bin/printf '<%s>\\n' "
         "\"`/usr/bin/printf 'x\\n\\n'`\"",
         0, "<x>\n"},
        {"2.6.3", "nested escaped backquotes",
         "/usr/bin/printf '<%s>\\n' "
         "`/usr/bin/printf '%s' \\`/usr/bin/printf nested\\``",
         0, "<nested>\n"},
        {"2.6.4", "native arithmetic precedence",
         "/usr/bin/printf '<%s>\\n' \"$((1 + 2 * 3))\"", 0,
         "<7>\n"},
        {"2.6.4", "native arithmetic environment name",
         "/usr/bin/printf '<%s>\\n' \"$((GSH_CONFORMANCE_NUMBER * 6))\"",
         0, "<42>\n"},
        {"2.6.4", "native arithmetic parameter token",
         "/usr/bin/printf '<%s>\\n' \"$(($GSH_CONFORMANCE_NUMBER + 1))\"",
         0, "<8>\n"},
        {"2.6.4", "native arithmetic constants and shift",
         "/usr/bin/printf '<%s>\\n' \"$((0x10 + 010 + (1 << 2)))\"",
         0, "<28>\n"},
        {"2.6.4", "arithmetic assignment is right associative",
         "unset GSH_ARITH_A GSH_ARITH_B; "
         "/usr/bin/printf '<%s:%s:%s>\\n' "
         "\"$((GSH_ARITH_A = GSH_ARITH_B = 7))\" "
         "\"$GSH_ARITH_A\" \"$GSH_ARITH_B\"",
         0, "<7:7:7>\n"},
        {"2.6.4", "arithmetic compound assignment operators",
         "GSH_ARITH=24; /usr/bin/printf "
         "'<%s:%s:%s:%s:%s:%s:%s:%s:%s:%s>\\n' "
         "\"$((GSH_ARITH /= 3))\" \"$((GSH_ARITH %= 5))\" "
         "\"$((GSH_ARITH += 9))\" \"$((GSH_ARITH -= 2))\" "
         "\"$((GSH_ARITH <<= 2))\" \"$((GSH_ARITH >>= 1))\" "
         "\"$((GSH_ARITH &= 14))\" \"$((GSH_ARITH ^= 3))\" "
         "\"$((GSH_ARITH |= 16))\" \"$GSH_ARITH\"",
         0, "<8:3:12:10:40:20:4:7:23:23>\n"},
        {"2.6.4", "arithmetic compound assignment preserves base",
         "GSH_ARITH=010; /usr/bin/printf '<%s:%s>\\n' "
         "\"$((GSH_ARITH += 1))\" \"$GSH_ARITH\"",
         0, "<9:9>\n"},
        {"2.6.4", "arithmetic assignment does not read unset target",
         "set -u; unset GSH_ARITH; /usr/bin/printf '<%s:%s>\\n' "
         "\"$((GSH_ARITH = 4))\" \"$GSH_ARITH\"",
         0, "<4:4>\n"},
        {"2.6.4", "arithmetic skipped branches do not mutate",
         "GSH_ARITH=1; /usr/bin/printf '<%s:%s:%s:%s>\\n' "
         "\"$((0 && (GSH_ARITH = 2)))\" \"$GSH_ARITH\" "
         "\"$((1 || (GSH_ARITH = 3)))\" \"$GSH_ARITH\"",
         0, "<0:1:1:1>\n"},
        {"2.6.4", "arithmetic conditional mutates selected branch",
         "GSH_ARITH=1; /usr/bin/printf '<%s:%s>\\n' "
         "\"$((0 ? (GSH_ARITH = 2) : (GSH_ARITH = 3)))\" "
         "\"$GSH_ARITH\"",
         0, "<3:3>\n"},
        {"2.6.4", "arithmetic parenthesized assignment target",
         "GSH_ARITH=2; /usr/bin/printf '<%s:%s>\\n' "
         "\"$(((GSH_ARITH) *= 5))\" \"$GSH_ARITH\"",
         0, "<10:10>\n"},
        {"2.6.4", "arithmetic expands parameter operators first",
         "unset GSH_ARITH; /usr/bin/printf '<%s>\\n' "
         "\"$(( ${GSH_ARITH:-2} + 3 ))\"",
         0, "<5>\n"},
        {"2.6.4", "arithmetic expands command substitution first",
         "/usr/bin/printf '<%s>\\n' "
         "\"$(( $(/usr/bin/printf 4) + 3 ))\"",
         0, "<7>\n"},
        {"2.6.4", "arithmetic command substitution can supply operator",
         "/usr/bin/printf '<%s>\\n' "
         "\"$((1 $(/usr/bin/printf +) 2))\"",
         0, "<3>\n"},
        {"2.6.4", "arithmetic expands backquote substitution first",
         "/usr/bin/printf '<%s>\\n' "
         "\"$(( `/usr/bin/printf 5` + 3 ))\"",
         0, "<8>\n"},
        {"2.6.4", "arithmetic expansion can nest",
         "/usr/bin/printf '<%s>\\n' \"$(( $((2 + 3)) * 2 ))\"",
         0, "<10>\n"},
        {"2.6.4", "expanded arithmetic assignment target",
         "GSH_ARITH_TARGET=GSH_ARITH_VALUE; GSH_ARITH_VALUE=1; "
         "/usr/bin/printf '<%s:%s>\\n' "
         "\"$(( $GSH_ARITH_TARGET = 5 ))\" \"$GSH_ARITH_VALUE\"",
         0, "<5:5>\n"},
        {"2.6.4", "readonly arithmetic target rejects mutation",
         "readonly GSH_ARITH_READONLY=1; "
         ": \"$((GSH_ARITH_READONLY = 2))\"",
         1, "gsh: parameter assignment failed\n"},
        {"2.6.4", "pipeline arithmetic assignment is isolated",
         "GSH_ARITH=1; /usr/bin/printf '%s\\n' "
         "\"$((GSH_ARITH = 2))\" | /bin/cat; "
         "/bin/test \"$GSH_ARITH\" = 1",
         0, "2\n"},
        {"2.6.4", "native arithmetic short circuit",
         "/usr/bin/printf '<%s:%s>\\n' \"$((0 && 1 / 0))\" "
         "\"$((1 ? 9 : 1 / 0))\"",
         0, "<0:9>\n"},
        {"2.6.4", "invalid arithmetic expression is diagnosed",
         ": \"$((1 +))\"", 1,
         "gsh: arithmetic expansion: invalid expression\n"},
        {"2.6.4", "invalid arithmetic variable value is diagnosed",
         "GSH_ARITH_BAD='1+2'; : \"$((GSH_ARITH_BAD))\"", 1,
         "gsh: arithmetic expansion: variable value is not a valid "
         "integer\n"},
        {"2.6.4", "arithmetic division by zero is diagnosed",
         ": \"$((1 / 0))\"; /usr/bin/true", 1,
         "gsh: arithmetic expansion: division by zero\n"},
        {"2.6.4", "arithmetic overflow is diagnosed",
         ": \"$((0x7fffffffffffffff + 1))\"", 1,
         "gsh: arithmetic expansion: integer overflow\n"},
        {"2.6.4", "invalid arithmetic shift is diagnosed",
         ": \"$((1 << 999))\"", 1,
         "gsh: arithmetic expansion: shift count or value is out of "
         "range\n"},
        {"2.8.1", "unselected command does not expand arithmetic",
         "/usr/bin/false && : \"$((1 / 0))\"; /usr/bin/true", 0,
         NULL},
        {"2.8.1", "pipeline arithmetic error is isolated",
         "GSH_ARITH=1; : \"$((GSH_ARITH=2, 1 / 0))\" | "
         "/usr/bin/true; /bin/test \"$GSH_ARITH\" = 1",
         0, "gsh: arithmetic expansion: division by zero\n"},
        {"2.15", "native colon builtin", ":", 0, NULL},
        {"2.15", "native true builtin", "true", 0, NULL},
        {"2.15", "native false builtin", "false", 1, NULL},
        {"command", "command -v identifies regular builtin",
         "command -v true", 0, "true\n"},
        {"command", "command -V describes regular builtin",
         "command -V true", 0, "true is a regular builtin\n"},
        {"command", "command -V identifies reserved word",
         "command -V if", 0, "if is a shell reserved word\n"},
        {"command", "command inspection finds shell function",
         "gsh_command_function() { :; }; "
         "command -V gsh_command_function",
         0, "gsh_command_function is a shell function\n"},
        {"command", "command inspection finds alias definition",
         "alias 'gsh_command_alias=echo one'\n"
         "command -v gsh_command_alias",
         0, "alias gsh_command_alias='echo one'\n"},
        {"command", "command inspection uses command-local PATH",
         "PATH=/bin command -v sh", 0, "/bin/sh\n"},
        {"command", "command -p ignores a missing current PATH",
         "PATH=/definitely/missing command -p -v sh >/dev/null", 0,
         NULL},
        {"command", "missing command inspection returns nonzero",
         "command -v gsh_definitely_missing", 1, NULL},
        {"command", "command inspection assignment does not persist",
         "GSH_COMMAND_TEMP=value command -v true >/dev/null; "
         "/bin/test -z \"${GSH_COMMAND_TEMP+set}\"",
         0, NULL},
        {"command", "command inspection redirection is native",
         "command -v true >/tmp/gsh-command-inspection; "
         "/bin/cat /tmp/gsh-command-inspection",
         0, "true\n"},
        {"command", "command inspection pipeline stage is native",
         "command -v true | /bin/cat", 0, "true\n"},
        {"command", "command executes an external utility",
         "command /bin/echo executed", 0, "executed\n"},
        {"command", "command propagates utility status",
         "command false", 1, NULL},
        {"command", "command suppresses shell function lookup",
         "true() { /bin/echo function; }; true; command true",
         0, "function\n"},
        {"command", "function named command keeps ordinary precedence",
         "command() { /bin/echo function-command; }; command -v true",
         0, "function-command\n"},
        {"command", "command missing utility returns 127",
         "command gsh_definitely_missing", 127, "command not found"},
        {"command", "command passes assignment to external utility",
         "GSH_COMMAND_ENV=value command /usr/bin/printenv GSH_COMMAND_ENV",
         0, "value\n"},
        {"command", "command makes special assignment temporary",
         "GSH_COMMAND_SPECIAL=temp command :; "
         "/bin/test -z \"${GSH_COMMAND_SPECIAL+set}\"",
         0, NULL},
        {"command", "command return keeps prefix assignment temporary",
         "gsh_command_return() { "
         "GSH_COMMAND_RETURN=temp command return 7; /bin/echo BAD; }; "
         "gsh_command_return; /bin/test \"$?\" = 7; "
         "/bin/test -z \"${GSH_COMMAND_RETURN+set}\"",
         0, NULL},
        {"command", "command preserves builtin operand effects",
         "GSH_COMMAND_PREFIX=temp command export GSH_COMMAND_EFFECT=value; "
         "/bin/test -z \"${GSH_COMMAND_PREFIX+set}\"; "
         "/bin/test \"$GSH_COMMAND_EFFECT\" = value",
         0, NULL},
        {"command", "command preserves declaration assignment context",
         "GSH_COMMAND_SOURCE='alpha beta'; "
         "command export GSH_COMMAND_DECL=$GSH_COMMAND_SOURCE; "
         "/usr/bin/printf '<%s>\n' \"$GSH_COMMAND_DECL\"",
         0, "<alpha beta>\n"},
        {"command", "command declaration assignment suppresses globbing",
         "GSH_COMMAND_SOURCE='/dev/n[uo]ll'; "
         "command readonly GSH_COMMAND_GLOB=$GSH_COMMAND_SOURCE; "
         "/usr/bin/printf '<%s>\n' \"$GSH_COMMAND_GLOB\"",
         0, "</dev/n[uo]ll>\n"},
        {"command", "command -p executes with the default PATH",
         "PATH=/definitely/missing command -p sh -c 'exit 0'", 0, NULL},
        {"command", "nested command wrappers remain native",
         "command command true", 0, NULL},
        {"command", "command execution redirection is native",
         "command /bin/echo redirected >/tmp/gsh-command-execution; "
         "/bin/cat /tmp/gsh-command-execution",
         0, "redirected\n"},
        {"command", "command execution pipeline stage is native",
         "command /bin/echo pipeline | /bin/cat", 0, "pipeline\n"},
        {"type", "type shares command resolution",
         "type true", 0, "true is a regular builtin\n"},
        {"hash", "hash remembers an explicit utility location",
         "PATH=/bin; hash sh; hash", 0, "sh=/bin/sh\n"},
        {"hash", "command inspection identifies hash as a builtin",
         "command -V hash", 0, "hash is a regular builtin\n"},
        {"hash", "shell function keeps ordinary hash precedence",
         "hash() { /usr/bin/printf FUNCTION_HASH; }; hash",
         0, "FUNCTION_HASH"},
        {"hash", "command suppresses a function named hash",
         "hash() { /usr/bin/printf BAD_HASH; }; PATH=/bin; "
         "command hash sh; command hash",
         0, "sh=/bin/sh\n"},
        {"hash", "normal command search populates the hash table",
         "PATH=/bin; sh -c 'exit 0'; hash", 0, "sh=/bin/sh\n"},
        {"hash", "compound external search populates the hash table",
         "PATH=/bin; if true; then sh -c 'exit 0'; fi; hash",
         0, "sh=/bin/sh\n"},
        {"hash", "hash -r forgets remembered locations",
         "GSH_HASH_FILE=/tmp/gsh-hash-reset-$$; PATH=/bin; hash sh; "
         "hash -r; hash >\"$GSH_HASH_FILE\"; "
         "/bin/test ! -s \"$GSH_HASH_FILE\"; "
         "/bin/rm -f \"$GSH_HASH_FILE\"",
         0, NULL},
        {"hash", "PATH assignment clears remembered locations",
         "GSH_HASH_FILE=/tmp/gsh-hash-path-$$; PATH=/bin; hash sh; "
         "PATH=$PATH; hash >\"$GSH_HASH_FILE\"; "
         "/bin/test ! -s \"$GSH_HASH_FILE\"; "
         "/bin/rm -f \"$GSH_HASH_FILE\"",
         0, NULL},
        {"hash", "hash does not report intrinsic builtins",
         "GSH_HASH_FILE=/tmp/gsh-hash-builtin-$$; PATH=/bin; hash cd; "
         "hash >\"$GSH_HASH_FILE\"; /bin/test ! -s \"$GSH_HASH_FILE\"; "
         "/bin/rm -f \"$GSH_HASH_FILE\"",
         0, NULL},
        {"hash", "hash does not report shell functions",
         "GSH_HASH_FILE=/tmp/gsh-hash-function-$$; "
         "gsh_hash_function() { :; }; hash gsh_hash_function; "
         "hash >\"$GSH_HASH_FILE\"; /bin/test ! -s \"$GSH_HASH_FILE\"; "
         "/bin/rm -f \"$GSH_HASH_FILE\"",
         0, NULL},
        {"hash", "hash accepts the option terminator",
         "PATH=/bin; hash -- sh; hash", 0, "sh=/bin/sh\n"},
        {"hash", "hash -r accepts the option terminator",
         "PATH=/bin; hash sh; hash -r --; "
         "GSH_HASH_FILE=/tmp/gsh-hash-r-end-$$; "
         "hash >\"$GSH_HASH_FILE\"; /bin/test ! -s \"$GSH_HASH_FILE\"; "
         "/bin/rm -f \"$GSH_HASH_FILE\"",
         0, NULL},
        {"hash", "hash diagnoses an unknown utility",
         "hash gsh_definitely_missing", 1, "utility not found"},
        {"hash", "hash rejects operands after -r",
         "hash -r sh", 1, "-r does not accept operands"},
        {"hash", "hash entry accelerates utility execution",
         "PATH=/bin; hash sh; sh -c 'exit 7'", 7, NULL},
        {"hash", "hash redirection preserves the remembered location",
         "PATH=/bin; hash sh >/dev/null; hash", 0, "sh=/bin/sh\n"},
        {"hash", "failed hash redirection has no cache side effect",
         "GSH_HASH_FILE=/tmp/gsh-hash-redir-$$; PATH=/bin; "
         "hash sh >/tmp/gsh-hash-missing-$$/out; GSH_HASH_STATUS=$?; "
         "hash >\"$GSH_HASH_FILE\"; "
         "/bin/test \"$GSH_HASH_STATUS\" -ne 0; "
         "/bin/test ! -s \"$GSH_HASH_FILE\"; "
         "/bin/rm -f \"$GSH_HASH_FILE\"",
         0, NULL},
        {"hash", "pipeline hash mutation is isolated",
         "GSH_HASH_FILE=/tmp/gsh-hash-pipeline-$$; PATH=/bin; "
         "hash sh | :; hash >\"$GSH_HASH_FILE\"; "
         "/bin/test ! -s \"$GSH_HASH_FILE\"; "
         "/bin/rm -f \"$GSH_HASH_FILE\"",
         0, NULL},
        {"hash", "subshell hash mutation is isolated",
         "GSH_HASH_FILE=/tmp/gsh-hash-subshell-$$; PATH=/bin; "
         "(hash sh); hash >\"$GSH_HASH_FILE\"; "
         "/bin/test ! -s \"$GSH_HASH_FILE\"; "
         "/bin/rm -f \"$GSH_HASH_FILE\"",
         0, NULL},
        {"hash", "temporary PATH hashing is cleared on scope exit",
         "GSH_HASH_FILE=/tmp/gsh-hash-temporary-$$; PATH=/bin; "
         "PATH=/definitely/missing hash sh >/dev/null 2>&1; "
         "hash >\"$GSH_HASH_FILE\"; /bin/test ! -s \"$GSH_HASH_FILE\"; "
         "/bin/rm -f \"$GSH_HASH_FILE\"",
         0, NULL},
        {"hash", "command -p does not replace hash's PATH environment",
         "PATH=/definitely/missing; command -p hash sh >/dev/null 2>&1; "
         "/bin/test $? -ne 0",
         0, NULL},
        {"hash", "stale locations repeat PATH search and update inspection",
         "GSH_HASH_ROOT=/tmp/gsh-hash-failover-$$; "
         "/bin/mkdir -p \"$GSH_HASH_ROOT/first\" "
         "\"$GSH_HASH_ROOT/second\"; "
         "/bin/cp /bin/sh \"$GSH_HASH_ROOT/second/probe\"; "
         "PATH=$GSH_HASH_ROOT/first:$GSH_HASH_ROOT/second; hash probe; "
         "/bin/cp /bin/sh \"$GSH_HASH_ROOT/first/probe\"; "
         "command -v probe >\"$GSH_HASH_ROOT/before\"; "
         "/usr/bin/grep -q \"$GSH_HASH_ROOT/second/probe\" "
         "\"$GSH_HASH_ROOT/before\"; "
         "/bin/mv \"$GSH_HASH_ROOT/second/probe\" "
         "\"$GSH_HASH_ROOT/retired\"; probe -c 'exit 0'; "
         "command -v probe >\"$GSH_HASH_ROOT/after\"; "
         "/usr/bin/grep -q \"$GSH_HASH_ROOT/first/probe\" "
         "\"$GSH_HASH_ROOT/after\"; GSH_HASH_STATUS=$?; "
         "/bin/rm -rf \"$GSH_HASH_ROOT\"; "
         "/bin/test \"$GSH_HASH_STATUS\" -eq 0",
         0, NULL},
        {"eval", "eval is identified as a special builtin",
         "command -V eval", 0, "eval is a special builtin\n"},
        {"eval", "eval without arguments succeeds", "eval", 0, NULL},
        {"eval", "eval accepts the option terminator",
         "eval -- '/usr/bin/printf EVAL_END'", 0, "EVAL_END"},
        {"eval", "eval concatenates operands with spaces",
         "eval '/usr/bin/printf' '\"<%s>\\n\"' '\"alpha beta\"'",
         0, "<alpha beta>\n"},
        {"eval", "eval mutates the current variable environment",
         "GSH_EVAL_VALUE=before; eval 'GSH_EVAL_VALUE=after'; "
         "/bin/test \"$GSH_EVAL_VALUE\" = after",
         0, NULL},
        {"eval", "leading special assignment persists through eval",
         "GSH_EVAL_LEADING=visible eval "
         "'/bin/test \"$GSH_EVAL_LEADING\" = visible'; "
         "/bin/test \"$GSH_EVAL_LEADING\" = visible",
         0, NULL},
        {"eval", "eval returns its evaluated command status",
         "eval false; /bin/test \"$?\" -eq 1", 0, NULL},
        {"eval", "negation applies to the complete eval",
         "! eval true", 1, NULL},
        {"eval", "eval defines a persistent function",
         "eval 'gsh_eval_function() { /usr/bin/printf function; }'; "
         "gsh_eval_function",
         0, "function"},
        {"eval", "eval defines an alias for the next complete command",
         "eval 'alias gsh_eval_alias=/usr/bin/printf'\n"
         "gsh_eval_alias alias",
         0, "alias"},
        {"eval", "eval executes inside a conditional",
         "if eval true; then /usr/bin/printf selected; else false; fi",
         0, "selected"},
        {"eval", "eval supplies a pipeline stage",
         "eval '/usr/bin/printf pipeline' | /usr/bin/tr a-z A-Z",
         0, "PIPELINE"},
        {"eval", "pipeline eval mutation is isolated",
         "GSH_EVAL_PIPE=parent; eval 'GSH_EVAL_PIPE=child' | true; "
         "/bin/test \"$GSH_EVAL_PIPE\" = parent",
         0, NULL},
        {"eval", "subshell eval mutation is isolated",
         "GSH_EVAL_SUBSHELL=parent; (eval 'GSH_EVAL_SUBSHELL=child'); "
         "/bin/test \"$GSH_EVAL_SUBSHELL\" = parent",
         0, NULL},
        {"eval", "asynchronous eval mutation is isolated",
         "GSH_EVAL_ASYNC=parent; eval 'GSH_EVAL_ASYNC=child' & wait; "
         "/bin/test \"$GSH_EVAL_ASYNC\" = parent",
         0, NULL},
        {"eval", "eval runs inside command substitution",
         "/bin/test \"$(eval '/usr/bin/printf substituted')\" = "
         "substituted",
         0, NULL},
        {"eval", "eval loop control retains the caller context",
         "GSH_EVAL_LOOPS=0; for item in one two; do "
         "GSH_EVAL_LOOPS=$((GSH_EVAL_LOOPS + 1)); eval break; done; "
         "/bin/test \"$GSH_EVAL_LOOPS\" -eq 1",
         0, NULL},
        {"eval", "return propagates through eval to its function",
         "gsh_eval_return() { eval 'return 7'; /usr/bin/printf BAD; }; "
         "gsh_eval_return; /bin/test \"$?\" -eq 7",
         0, NULL},
        {"eval", "eval can replace positional parameters",
         "set -- old; eval 'set -- alpha beta'; "
         "/bin/test \"$#:$1:$2\" = 2:alpha:beta",
         0, NULL},
        {"eval", "eval redirection spans evaluated commands and restores",
         "GSH_EVAL_FILE=/tmp/gsh-eval-redirection-$$; "
         "eval '/usr/bin/printf inner' >\"$GSH_EVAL_FILE\"; "
         "/usr/bin/printf outer; /bin/cat \"$GSH_EVAL_FILE\"; "
         "/bin/rm -f \"$GSH_EVAL_FILE\"",
         0, "outerinner"},
        {"eval", "eval syntax error aborts a non-interactive shell",
         "eval 'if'; /usr/bin/printf BAD_EVAL", 2, "incomplete"},
        {"eval", "command suppresses eval special error semantics",
         "command eval 'if'; /usr/bin/printf EVAL_RECOVERED", 0,
         "EVAL_RECOVERED"},
        {"eval", "command eval exposes but restores its prefix assignment",
         "unset GSH_COMMAND_EVAL; "
         "GSH_COMMAND_EVAL=temp command eval "
         "'/usr/bin/printenv GSH_COMMAND_EVAL; "
         "GSH_COMMAND_EVAL=changed; GSH_EVAL_EFFECT=kept'; "
         "/bin/test -z \"${GSH_COMMAND_EVAL+set}\"; "
         "/bin/test \"$GSH_EVAL_EFFECT\" = kept",
         0, "temp\n"},
        {"eval", "command eval atomically restores a readonly prefix",
         "GSH_COMMAND_EVAL=before; "
         "GSH_COMMAND_EVAL=temp command eval "
         "'readonly GSH_COMMAND_EVAL; GSH_EVAL_READONLY_EFFECT=kept'; "
         "/bin/test \"$GSH_COMMAND_EVAL\" = before; "
         "/bin/test \"$GSH_EVAL_READONLY_EFFECT\" = kept; "
         "GSH_COMMAND_EVAL=after; "
         "/bin/test \"$GSH_COMMAND_EVAL\" = after",
         0, NULL},
        {"dot", "dot is identified as a special builtin",
         "command -V .", 0, ". is a special builtin\n"},
        {"exec", "exec is identified as a special builtin",
         "command -V exec", 0, "exec is a special builtin\n"},
        {"exec", "exec overlays the shell with an external utility",
         "exec /usr/bin/printf '<%s>\\n' replaced; "
         "/usr/bin/false",
         0, "<replaced>\n"},
        {"exec", "exec leading assignment reaches the utility",
         "GSH_EXEC_VALUE='alpha beta' exec /usr/bin/printenv GSH_EXEC_VALUE",
         0, "alpha beta\n"},
        {"exec", "exec uses its leading PATH assignment",
         "PATH=/bin exec sh -c 'exit 7'", 7, NULL},
        {"exec", "exec accepts the option terminator",
         "exec -- /usr/bin/printf EXEC_END", 0, "EXEC_END"},
        {"exec", "exec without a utility commits descriptors",
         "GSH_EXEC_FILE=/tmp/gsh-exec-descriptor-$$; "
         "exec 9>\"$GSH_EXEC_FILE\"; /usr/bin/printf persisted >&9; "
         "exec 9>&-; /bin/cat \"$GSH_EXEC_FILE\"; "
         "/bin/rm -f \"$GSH_EXEC_FILE\"",
         0, "persisted"},
        {"exec", "failed regular exec keeps successful redirections",
         "GSH_EXEC_FILE=/tmp/gsh-exec-failure-$$; "
         "command exec 2>\"$GSH_EXEC_FILE\" /definitely/missing; "
         "/usr/bin/printf after >&2; /bin/cat \"$GSH_EXEC_FILE\"; "
         "/bin/rm -f \"$GSH_EXEC_FILE\"",
         0, "after"},
        {"exec", "exec failure aborts a non-interactive shell",
         "exec /definitely/missing; /usr/bin/printf BAD_EXEC", 127,
         "command not found"},
        {"exec", "exec reports a non-executable utility as 126",
         "exec /; /usr/bin/printf BAD_EXEC", 126,
         "permission denied"},
        {"exec", "negation does not suppress special exec failure",
         "! exec /definitely/missing; /usr/bin/printf BAD_EXEC", 127,
         "command not found"},
        {"exec", "command suppresses exec special error semantics",
         "command exec /definitely/missing; /usr/bin/printf RECOVERED",
         0, "RECOVERED"},
        {"exec", "exec rejects unsupported options",
         "exec -x; /usr/bin/printf BAD_EXEC", 2,
         "unsupported option"},
        {"exec", "exec overlays a pipeline stage",
         "/usr/bin/printf x | exec /usr/bin/tr x y", 0, "y"},
        {"exec", "subshell exec does not replace its parent shell",
         "(exec /usr/bin/printf sub); /usr/bin/printf parent", 0,
         "subparent"},
        {"exec", "asynchronous exec replaces only its child environment",
         "exec /bin/sh -c 'exit 9' & wait \"$!\"; "
         "/bin/test \"$?\" -eq 9",
         0, NULL},
        {"exec", "command substitution exec replaces only its child",
         "/usr/bin/printf '<%s>\n' "
         "\"$(exec /usr/bin/printf substituted)\"",
         0, "<substituted>\n"},
        {"exec", "exec inside a function overlays the shell",
         "gsh_exec_function() { exec /usr/bin/printf function; }; "
         "gsh_exec_function; /usr/bin/false",
         0, "function"},
        {"times", "times is identified as a special builtin",
         "command -V times", 0, "times is a special builtin\n"},
        {"times", "times writes two POSIX timing rows",
         "GSH_TIMES_FILE=/tmp/gsh-times-output-$$; times >\"$GSH_TIMES_FILE\"; "
         "/bin/test \"$(/usr/bin/wc -l <\"$GSH_TIMES_FILE\")\" -eq 2; "
         "GSH_TIMES_STATUS=$?; /bin/rm -f \"$GSH_TIMES_FILE\"; "
         "/bin/test \"$GSH_TIMES_STATUS\" -eq 0",
         0, NULL},
        {"times", "times assignment persists in the current environment",
         "GSH_TIMES_VALUE=before; GSH_TIMES_VALUE=after times >/dev/null; "
         "/bin/test \"$GSH_TIMES_VALUE\" = after",
         0, NULL},
        {"times", "times redirection works inside a compound command",
         "GSH_TIMES_FILE=/tmp/gsh-times-compound-$$; "
         "if true; then times >\"$GSH_TIMES_FILE\"; fi; "
         "/bin/test \"$(/usr/bin/wc -l <\"$GSH_TIMES_FILE\")\" -eq 2; "
         "GSH_TIMES_STATUS=$?; /bin/rm -f \"$GSH_TIMES_FILE\"; "
         "/bin/test \"$GSH_TIMES_STATUS\" -eq 0",
         0, NULL},
        {"times", "times runs in a pipeline subshell",
         "times | /usr/bin/awk 'END { print NR }'", 0, "2\n"},
        {"times", "times operand error aborts a non-interactive shell",
         "times unexpected; /usr/bin/printf BAD_TIMES", 1,
         "does not accept operands"},
        {"times", "command suppresses times special error semantics",
         "command times unexpected; /usr/bin/printf TIMES_RECOVERED",
         0, "TIMES_RECOVERED"},
        {"export", "native export assignment",
         "export GSH_EXPORT_VALUE='alpha beta'; "
         "/usr/bin/printenv GSH_EXPORT_VALUE",
         0, "alpha beta\n"},
        {"export", "declaration assignment suppresses field splitting",
         "GSH_EXPORT_SOURCE='alpha beta'; "
         "export GSH_EXPORT_VALUE=$GSH_EXPORT_SOURCE; "
         "/usr/bin/printf '<%s>\n' \"$GSH_EXPORT_VALUE\"",
         0, "<alpha beta>\n"},
        {"export", "declaration assignment suppresses pathname expansion",
         "GSH_EXPORT_SOURCE='/dev/n[uo]ll'; "
         "export GSH_EXPORT_VALUE=$GSH_EXPORT_SOURCE; "
         "/usr/bin/printf '<%s>\n' \"$GSH_EXPORT_VALUE\"",
         0, "</dev/n[uo]ll>\n"},
        {"export", "unset export attribute survives later assignment",
         "export GSH_EXPORT_LATER; GSH_EXPORT_LATER=later; "
         "/usr/bin/printenv GSH_EXPORT_LATER",
         0, "later\n"},
        {"export", "export listing quotes apostrophes",
         "export GSH_EXPORT_QUOTE=\"a'b\"; export -p", 0,
         "export GSH_EXPORT_QUOTE='a'\\''b'\n"},
        {"export", "export listing represents unset variables",
         "export GSH_EXPORT_UNSET; export -p", 0,
         "export GSH_EXPORT_UNSET\n"},
        {"2.9.1.2", "leading special-builtin assignment persists",
         "GSH_SPECIAL_LEADING=one export GSH_SPECIAL_OPERAND=two; "
         "/bin/test \"$GSH_SPECIAL_LEADING:$GSH_SPECIAL_OPERAND\" = "
         "one:two",
         0, NULL},
        {"export", "pipeline export is isolated",
         "export GSH_EXPORT_PIPE=before; "
         "export GSH_EXPORT_PIPE=inside | true; "
         "/bin/test \"$GSH_EXPORT_PIPE\" = before",
         0, NULL},
        {"export", "special-builtin redirection preserves state",
         "export GSH_EXPORT_REDIRECT=value >/dev/null; "
         "/usr/bin/printf '<%s>\n' \"$GSH_EXPORT_REDIRECT\"",
         0, "<value>\n"},
        {"export", "listing redirection preserves leading assignment",
         "GSH_EXPORT_LIST_ASSIGN=value export -p >/dev/null; "
         "/bin/test \"$GSH_EXPORT_LIST_ASSIGN\" = value",
         0, NULL},
        {"export", "failed redirection precedes special assignment",
         "GSH_EXPORT_REDIRECT_ORDER=before; "
         "if GSH_EXPORT_REDIRECT_ORDER=changed export GSH_UNUSED=x "
         ">/dev/null/child; then false; fi; "
         "/bin/test \"$GSH_EXPORT_REDIRECT_ORDER\" = before",
         0, "redirection"},
        {"export", "special builtin accepts here-document",
         "export GSH_EXPORT_HEREDOC=value <<EOF\nignored\nEOF\n"
         "/bin/test \"$GSH_EXPORT_HEREDOC\" = value",
         0, NULL},
        {"readonly", "native readonly assignment",
         "readonly GSH_READONLY_VALUE=locked; "
         "/usr/bin/printf '%s\n' \"$GSH_READONLY_VALUE\"",
         0, "locked\n"},
        {"readonly", "readonly listing represents unset variables",
         "readonly GSH_READONLY_UNSET; readonly -p", 0,
         "readonly GSH_READONLY_UNSET\n"},
        {"readonly", "readonly rejects later assignment",
         "readonly GSH_READONLY_LOCK=locked; "
         "GSH_READONLY_LOCK=changed && /usr/bin/printf BAD",
         1, "assignment"},
        {"readonly", "readonly rejects unset",
         "readonly GSH_READONLY_UNSET_FAIL=locked; "
         "unset GSH_READONLY_UNSET_FAIL",
         1, "readonly"},
        {"export", "bare export accepts readonly variable",
         "readonly GSH_READONLY_EXPORT=locked; "
         "export GSH_READONLY_EXPORT; "
         "/usr/bin/printenv GSH_READONLY_EXPORT",
         0, "locked\n"},
        {"unset", "unset removes value and attributes",
         "export GSH_UNSET_VALUE=gone; unset GSH_UNSET_VALUE; "
         "/bin/test -z \"${GSH_UNSET_VALUE+set}\"",
         0, NULL},
        {"unset", "empty assignment remains set",
         "GSH_UNSET_EMPTY=; /bin/test \"${GSH_UNSET_EMPTY+set}\" = set",
         0, NULL},
        {"unset", "unset absent function succeeds", "unset -f GSH_NO_FUNC",
         0, NULL},
        {"unset", "combined function-variable unset",
         "GSH_UNSET_BOTH=value; unset -fv GSH_UNSET_BOTH; "
         "/bin/test -z \"${GSH_UNSET_BOTH+set}\"",
         0, NULL},
        {"unset", "explicit variable unset",
         "GSH_UNSET_OPTION=value; unset -v GSH_UNSET_OPTION; "
         "/bin/test -z \"${GSH_UNSET_OPTION+set}\"",
         0, NULL},
        {"export", "invalid export option", "export -x", 1,
         "invalid option"},
        {"readonly", "invalid readonly option", "readonly -x", 1,
         "invalid option"},
        {"unset", "invalid unset name", "unset bad-name", 1,
         "invalid variable name"},
        {"2.9.5", "function expansion is deferred until invocation",
         "GSH_FUNCTION_VALUE=one; "
         "f() { /usr/bin/printf '%s\\n' \"$GSH_FUNCTION_VALUE\"; }; "
         "GSH_FUNCTION_VALUE=two; f",
         0, "two\n"},
        {"2.9.5", "function operands become positional parameters",
         "f() { /usr/bin/printf '<%s:%s:%s>\\n' \"$#\" \"$1\" "
         "\"$2\"; }; f alpha 'beta gamma'",
         0, "<2:alpha:beta gamma>\n"},
        {"2.9.5", "function restores caller positional parameters",
         "set -- outer one; f() { set -- inner two; }; f arg; "
         "/usr/bin/printf '<%s:%s:%s>\\n' \"$#\" \"$1\" \"$2\"",
         0, "<2:outer:one>\n"},
        {"2.9.5", "function body mutation persists",
         "GSH_FUNCTION_MUTATION=before; "
         "f() { GSH_FUNCTION_MUTATION=after; }; f; "
         "/usr/bin/printf '%s\\n' \"$GSH_FUNCTION_MUTATION\"",
         0, "after\n"},
        {"2.9.1.2", "function prefix assignment is visible and persists",
         "f() { /usr/bin/printf '%s\\n' \"$GSH_FUNCTION_PREFIX\"; }; "
         "GSH_FUNCTION_PREFIX=prefix f; "
         "/usr/bin/printf '%s\\n' \"$GSH_FUNCTION_PREFIX\"",
         0, "prefix\nprefix\n"},
        {"2.9.5", "function exit status is its last command",
         "f() { false; }; f", 1, NULL},
        {"return", "explicit return stops the function body",
         "f() { return 7; /usr/bin/printf BAD; }; f; "
         "/bin/test \"$?\" = 7",
         0, NULL},
        {"return", "operandless return uses current status",
         "f() { false; return; }; f", 1, NULL},
        {"2.9.5", "function and variable namespaces are separate",
         "f=value; f() { /usr/bin/printf function; }; f; "
         "/usr/bin/printf ':%s\\n' \"$f\"",
         0, "function:value\n"},
        {"2.9.5", "function redefinition replaces its body",
         "f() { /usr/bin/printf first; }; f; "
         "f() { /usr/bin/printf second; }; f",
         0, "firstsecond"},
        {"unset", "unset function removes only the function",
         "f=value; f() { :; }; unset -f f; "
         "/bin/test \"$f\" = value; f",
         127, "gsh: f: command not found\n"},
        {"ulimit", "native soft descriptor report", "ulimit -S -n", 0,
         "\n"},
        {"ulimit", "native soft descriptor setting",
         "ulimit -S -n 32; /bin/test \"$(ulimit -S -n)\" = 32", 0,
         NULL},
        {"ulimit", "pipeline resource isolation",
         "GSH_LIMIT=\"$(ulimit -S -n)\"; "
         "ulimit -S -n 32 | :; "
         "/bin/test \"$(ulimit -S -n)\" = \"$GSH_LIMIT\"",
         0, NULL},
        {"ulimit", "native all-resource report", "ulimit -a", 0,
         "open file descriptors"},
        {"ulimit", "invalid resource option", "ulimit -x", 1,
         "invalid"},
        {"umask", "native symbolic report",
         "umask 022; umask -S", 0, "u=rwx,g=rx,o=rx\n"},
        {"umask", "symbolic plus clears mask bits",
         "umask 022; umask +w; umask", 0, "0000\n"},
        {"umask", "symbolic leading minus operand",
         "umask 022; umask -- -w; umask", 0, "0222\n"},
        {"umask", "symbolic permission copy",
         "umask 022; umask u=g; umask", 0, "0222\n"},
        {"umask", "symbolic X uses initially clear execute bits",
         "umask 777; umask a+x-X; umask", 0, "0666\n"},
        {"umask", "symbolic X uses initially set execute bits",
         "umask 666; umask a-x+X; umask", 0, "0666\n"},
        {"umask", "pipeline mask isolation",
         "GSH_MASK=\"$(umask)\"; umask 077 | :; "
         "/bin/test \"$(umask)\" = \"$GSH_MASK\"",
         0, NULL},
        {"umask", "invalid symbolic mask", "umask xyz", 1,
         "invalid mask"},
        {"2.5.2", "native previous status parameter",
         "false; /usr/bin/printf '%s\\n' \"$?\"", 0, "1\n"},
        {"2.5.2", "native shell pid parameter",
         "/bin/test \"$$\" -gt 1", 0, NULL},
        {"2.9.1.2", "native command environment assignments",
         "GSH_ASSIGN_ONE=one GSH_ASSIGN_TWO='two words' /usr/bin/env", 0,
         "GSH_ASSIGN_TWO=two words\n"},
        {"2.9.1.2", "assignment wildcard remains literal",
         "GSH_ASSIGN_STAR='*' /usr/bin/env", 0, "GSH_ASSIGN_STAR=*\n"},
        {"2.9.1.2", "command assignment does not persist",
         "GSH_ASSIGN_TEMP=temporary /usr/bin/true; "
         "/bin/test -z \"$GSH_ASSIGN_TEMP\"",
         0, NULL},
        {"2.9.1.3", "native assignment-only command",
         "GSH_ASSIGN_ONLY=value", 0, NULL},
        {"2.9.1.3", "assignment persists through native list",
         "GSH_ASSIGN_ONLY=value; "
         "/bin/test \"$GSH_ASSIGN_ONLY\" = value",
         0, NULL},
        {"2.7.4", "native here-document",
         "/bin/cat <<EOF\nhello\nEOF\n", 0, "hello\n"},
        {"2.7.4", "quoted here-document suppresses expansion",
         "/bin/cat <<'EOF'\n$GSH_CONFORMANCE_VALUE\nEOF\n", 0,
         "$GSH_CONFORMANCE_VALUE\n"},
        {"2.7.4", "here-document parameter expansion",
         "/bin/cat <<EOF\n$GSH_CONFORMANCE_VALUE\nEOF\n", 0,
         "alpha beta\n"},
        {"2.7.4", "here-document backslash semantics",
         "/bin/cat <<EOF\n"
         "\\$GSH_CONFORMANCE_ATOM \\\\ \"$GSH_CONFORMANCE_ATOM\"\n"
         "joined\\\nline\nEOF\n",
         0, "$GSH_CONFORMANCE_ATOM \\ \"atom\"\njoinedline\n"},
        {"2.7.4", "tab-stripped here-document execution",
         "/bin/cat <<-EOF\n\tone\n\t\ttwo\n\tEOF\n", 0,
         "one\ntwo\n"},
        {"2.7.4", "multiple here-document ordering",
         "/bin/cat <<FIRST <<SECOND\none\nFIRST\ntwo\nSECOND\n", 0,
         "two\n"},
        {"2.7.4", "explicit here-document descriptor",
         "/bin/cat 3<<EOF <&3\nfd-three\nEOF\n", 0,
         "fd-three\n"},
        {"2.7.4", "here-document in pipeline",
         "/bin/cat <<EOF | /usr/bin/tr a-z A-Z\npipeline\nEOF\n", 0,
         "PIPELINE\n"},
        {"2.7.4", "continued line delays delimiter",
         "/bin/cat <<EOF\nfoo\\\nEOF\nbar\nEOF\n", 0,
         "fooEOF\nbar\n"},
        {"2.7.4", "here-document command substitution",
         "/bin/cat <<EOF\n$(/usr/bin/printf dynamic)\nEOF\n", 0,
         "dynamic\n"},
        {"2.7.4", "here-document arithmetic expansion",
         "/bin/cat <<EOF\nvalue=$((2 << 4))\nEOF\n", 0,
         "value=32\n"},
        {"2.7.4", "here-document expands arithmetic operands first",
         "GSH_HEREDOC_ARITH=2; /bin/cat <<EOF\n"
         "$(( $GSH_HEREDOC_ARITH + $(/usr/bin/printf 3) ))\nEOF\n",
         0, "5\n"},
        {"2.7.4", "here-document backquoted substitution",
         "/bin/cat <<EOF\n`/usr/bin/printf legacy`\nEOF\n", 0,
         "legacy\n"},
        {"2.3.1/alias", "alias takes effect at next complete command",
         "alias gsh_a=/bin/echo\ngsh_a next", 0, "next\n"},
        {"2.3.1/alias", "alias does not take effect out of order",
         "alias gsh_a=/bin/echo; gsh_a current\nalias gsh_a", 0,
         "command not found"},
        {"alias", "alias replacement is visible to named query",
         "alias gsh_a=/usr/bin/true\nalias gsh_a", 0,
         "gsh_a='/usr/bin/true'\n"},
        {"alias", "operandless alias lists definitions",
         "alias gsh_b=/usr/bin/false\nalias gsh_a=/usr/bin/true\nalias", 0,
         "gsh_a='/usr/bin/true'\ngsh_b='/usr/bin/false'\n"},
        {"alias", "alias listing quotes apostrophes for reinput",
         "alias \"gsh_a=a'b\"\nalias gsh_a", 0,
         "gsh_a='a'\\''b'\n"},
        {"alias", "undefined alias returns nonzero",
         "alias gsh_missing", 1, "alias is not defined"},
        {"alias", "invalid alias option returns nonzero",
         "alias -x", 1, "invalid option"},
        {"alias", "portable punctuation alias name",
         "alias 'gsh-a=/bin/echo'\ngsh-a portable", 0,
         "portable\n"},
        {"2.3.1/alias", "quoted alias builtin initializes native state",
         "'alias' gsh_a=/bin/echo\ngsh_a quoted", 0, "quoted\n"},
        {"2.6.2/alias", "expanded alias builtin initializes native state",
         "GSH_ALIAS_COMMAND=alias\n"
         "\"$GSH_ALIAS_COMMAND\" gsh_a=/bin/echo\ngsh_a expanded",
         0, "expanded\n"},
        {"alias/1.7", "alias is intrinsic and bypasses PATH search",
         "PATH=/definitely/missing alias gsh_a=/usr/bin/true\nalias gsh_a",
         0, "gsh_a='/usr/bin/true'\n"},
        {"2.9.1/alias", "alias prefix assignment does not persist",
         "GSH_ALIAS_TEMP=value alias gsh_a=/usr/bin/true\n"
         "/bin/test \"${GSH_ALIAS_TEMP+set}\" != set",
         0, NULL},
        {"unalias", "unalias removes current definition",
         "alias gsh_a=/usr/bin/true\nunalias gsh_a\nalias gsh_a", 1,
         "alias is not defined"},
        {"unalias", "unalias removes every named operand",
         "alias gsh_a=/usr/bin/true\nalias gsh_b=/usr/bin/true\n"
         "unalias gsh_a gsh_b\nalias gsh_b",
         1, "alias is not defined"},
        {"unalias", "unalias -a removes every definition",
         "alias gsh_a=/usr/bin/true\nalias gsh_b=/usr/bin/false\nunalias -a\n"
         "alias gsh_a", 1, "alias is not defined"},
        {"unalias", "undefined unalias returns nonzero",
         "unalias gsh_missing", 1, "alias is not defined"},
        {"unalias", "invalid unalias option returns nonzero",
         "unalias -x", 1, "invalid option"},
        {"2.3.1", "quoted command word is not alias substituted",
         "alias gsh_a=/usr/bin/true\n'gsh_a'", 127,
         "command not found"},
        {"2.3.1", "alias is not substituted in argument position",
         "alias gsh_a=/usr/bin/false\n/bin/echo gsh_a", 0, "gsh_a\n"},
        {"2.3.1", "direct recursive alias terminates",
         "alias 'gsh_a=gsh_a -n'\ngsh_a value", 127,
         "command not found"},
        {"2.3.1", "indirect recursive alias terminates",
         "alias gsh_a=gsh_b\nalias gsh_b=gsh_a\ngsh_a value", 127,
         "command not found"},
        {"2.3.1", "trailing blank makes following token eligible",
         "alias 'gsh_a=gsh_b '\nalias gsh_b=/bin/echo\ngsh_a forced",
         0, "forced\n"},
        {"2.3.1/2.4", "reserved word is not alias substituted",
         "alias if=/usr/bin/false\nif true; then /usr/bin/true; fi", 0,
         NULL},
        {"2.3.1/2.13", "subshell inherits aliases",
         "alias gsh_a=/bin/echo\n(gsh_a inherited)", 0,
         "inherited\n"},
        {"2.3.1/2.13", "subshell alias mutation is isolated",
         "alias gsh_a=/usr/bin/true\n(unalias gsh_a)\ngsh_a", 0, NULL},
        {"2.3.1/2.13", "pipeline alias mutation is isolated",
         "alias gsh_a=/usr/bin/true\nunalias gsh_a | /bin/cat\ngsh_a", 0,
         NULL},
        {"2.3.1/2.13", "asynchronous alias mutation is isolated",
         "alias gsh_a=/usr/bin/true\nunalias gsh_a &\nwait\ngsh_a", 0,
         NULL},
        {"2.3.1/2.13", "utility environment does not inherit aliases",
         "alias gsh_a=/usr/bin/true\n/bin/sh -c "
         "'alias gsh_a >/dev/null 2>&1; test $? -ne 0'", 0, NULL},
        {"2.3.1/2.6.3", "command substitution inherits alias snapshot",
         "alias gsh_a=/bin/echo\n/usr/bin/printf '<%s>\\n' "
         "\"$(gsh_a nested)\"", 0, "<nested>\n"},
        {"pwd", "native pwd invalid operand", "pwd extra", 1,
         "gsh: pwd: invalid operand"},
    };
    size_t index;
    size_t passed = 0;
    size_t execution_passed = 0;
    size_t limit_passed = 0;
    size_t unsupported = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: posix-conformance /absolute/path/to/gsh\n");
        return 2;
    }
    if (configure_utf8_locale() == -1 ||
        setenv("IFS", " \t\n", 1) == -1 ||
        setenv("GSH_CONFORMANCE_VALUE", "alpha beta", 1) == -1 ||
        setenv("GSH_CONFORMANCE_ATOM", "atom", 1) == -1 ||
        setenv("GSH_CONFORMANCE_NUMBER", "7", 1) == -1 ||
        setenv("GSH_CONFORMANCE_PATTERN", "/dev/n[uo]ll", 1) == -1 ||
        setenv("GSH_CONFORMANCE_UTF8", "\xC3\xA9" "a", 1) == -1 ||
        setenv("HOME", "/tmp/gsh-conformance-home", 1) == -1 ||
        unsetenv("GSH_CONFORMANCE_UNSET") == -1 ||
        unsetenv("GSH_ASSIGN_TEMP") == -1 ||
        unsetenv("GSH_ASSIGN_ONLY") == -1 ||
        unsetenv("GSH_PARAMETER_ASSIGNED") == -1 ||
        unsetenv("GSH_PARAMETER_QUESTION_UNSET") == -1 ||
        unsetenv("GSH_SUBSHELL_LOCAL") == -1 ||
        unsetenv("GSH_SUBSHELL_PARAMETER") == -1 ||
        unsetenv("GSH_SUBSTITUTION_LOCAL") == -1 ||
        unsetenv("GSH_PIPE_LEFT") == -1 ||
        unsetenv("GSH_PIPE_SAME") == -1 ||
        unsetenv("GSH_PIPE_ERROR") == -1 ||
        unsetenv("GSH_PIPE_LAST") == -1) {
        perror("conformance: environment");
        return 1;
    }
    for (index = 0; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (run_case(argv[1], &cases[index], true) != 0) {
            return 1;
        }
        passed++;
        if (cases[index].diagnostic != NULL &&
            strcmp(cases[index].diagnostic, "unsupported") == 0) {
            unsupported++;
        }
    }
    if (no_execution_case(argv[1]) != 0) {
        return 1;
    }
    if (native_invocation_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 8U;
    for (index = 0;
         index < sizeof(execution_cases) / sizeof(execution_cases[0]);
         index++) {
        if (run_case(argv[1], &execution_cases[index], false) != 0) {
            return 1;
        }
        execution_passed++;
        if (execution_cases[index].diagnostic != NULL &&
            strcmp(execution_cases[index].diagnostic,
                   "native execution unsupported") == 0) {
            unsupported++;
        }
    }
    if (native_source_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 19U;
    if (native_exit_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 19U;
    if (native_enoexec_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 6U;
    if (native_redirection_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 5U;
    if (native_function_redirection_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 8U;
    if (native_function_context_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 11U;
    if (native_loop_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 4U;
    if (native_positional_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 26U;
    if (native_set_option_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 50U;
    if (native_pwd_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 5U;
    if (native_umask_creation_case(argv[1]) != 0) {
        return 1;
    }
    execution_passed++;
    if (native_builtin_output_failure_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 2U;
    if (native_heredoc_stress_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 2U;
    if (native_pattern_stress_cases(argv[1]) != 0) {
        return 1;
    }
    execution_passed += 2U;
    if (native_limit_cases(argv[1]) != 0) {
        return 1;
    }
    limit_passed = 18;
    printf("POSIX native tranche: syntax=%zu execution=%zu limits=%zu "
           "unsupported=%zu delegated=0\n",
           passed + 1U, execution_passed, limit_passed, unsupported);
    return 0;
}
