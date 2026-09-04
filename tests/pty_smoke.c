#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh tests require the POSIX.1-2024 feature-test baseline"
#endif

#include "../src/source_workspace.h"
#include "benchmark_report.h"

#include <dirent.h> /* CANON-INCLUDE: linux */
#include <errno.h>
#include <fcntl.h>
#include <limits.h> /* CANON-INCLUDE: linux */
#include <locale.h>
#include <poll.h>
#include <signal.h> /* CANON-INCLUDE: macos */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h> /* CANON-INCLUDE: linux */
#include <sys/stat.h> /* CANON-INCLUDE: linux */
#include <sys/wait.h> /* CANON-INCLUDE: linux */
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h> /* CANON-INCLUDE: linux */
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#endif

enum {
    CAPTURE_CAP = 65536,
    TEST_TIMEOUT_MS = 3000,
    JOB_TRANSITION_TIMEOUT_MS = 10000,
    BENCH_SHELLS = 3,
    BENCH_STARTUP_SAMPLES = 120,
    BENCH_KEY_SAMPLES = 500,
    BENCH_EXEC_SAMPLES = 300,
    BENCH_MEMORY_SAMPLES = 20,
    BENCH_MEMORY_WORKLOADS = 26,
    BENCH_LATENCY_WORKLOADS = 37,
    PTY_DESCRIPTOR_CLOSE_PASS_CAP = 1024,
    PTY_WAIT_ATTEMPT_CAP = 65536,
    PTY_DRAIN_ATTEMPT_CAP = CAPTURE_CAP,
};

_Static_assert((size_t)BENCH_STARTUP_SAMPLES <=
                   (size_t)BENCHMARK_REPORT_SAMPLE_CAP,
               "startup samples must fit the CSV schema");
_Static_assert((size_t)BENCH_KEY_SAMPLES <=
                   (size_t)BENCHMARK_REPORT_SAMPLE_CAP,
               "key samples must fit the CSV schema");
_Static_assert((size_t)BENCH_EXEC_SAMPLES <=
                   (size_t)BENCHMARK_REPORT_SAMPLE_CAP,
               "command samples must fit the CSV schema");
_Static_assert((size_t)BENCH_MEMORY_SAMPLES <=
                   (size_t)BENCHMARK_REPORT_SAMPLE_CAP,
               "memory samples must fit the CSV schema");

static bool require(bool condition)
{
    if (!condition) {
        errno = EINVAL;
        return false;
    }
    return true;
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

typedef enum {
    SHELL_GSH,
    SHELL_BASH,
    SHELL_ZSH,
} shell_kind;

typedef struct {
    const char *name;
    const char *executable;
    const char *prompt;
    shell_kind kind;
} shell_spec;

static int close_inherited_descriptors(void)
{
#if defined(__APPLE__)
    struct proc_fdinfo descriptors[64];
    size_t pass;

    for (pass = 0; pass < PTY_DESCRIPTOR_CLOSE_PASS_CAP; pass++) {
        int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, descriptors,
                                 (int)sizeof(descriptors));
        int count;
        int index;
        bool closed = false;

        if (bytes < 0) {
            return -1;
        }
        count = bytes / (int)sizeof(descriptors[0]);
        for (index = 0; index < count; index++) {
            int descriptor = descriptors[index].proc_fd;

            if (descriptor > STDERR_FILENO) {
                (void)close(descriptor);
                closed = true;
            }
        }
        if (!closed) {
            return 0;
        }
    }
    errno = EMFILE;
    return -1;
#elif defined(__linux__)
    struct dirent *entry;
    DIR *directory = opendir("/proc/self/fd");
    int directory_fd;
    int failed = 0;

    directory_fd = dirfd(directory);
    while ((entry = readdir(directory)) != NULL) {
        char *end;
        long descriptor;

        errno = 0;
        descriptor = strtol(entry->d_name, &end, 10);
        if (errno == 0 && *end == '\0' && descriptor > STDERR_FILENO &&
            descriptor != directory_fd &&
            close((int)descriptor) == -1 && errno != EBADF) {
            failed = -1;
        }
    }
    if (closedir(directory) == -1) {
        failed = -1;
    }
    return failed;
#else
    return 0;
#endif
}

static bool linux_is_translated(void)
{
#if defined(__linux__)
    char contents[4096];
    ssize_t count;
    int descriptor = open("/proc/cpuinfo", O_RDONLY);

    if (descriptor == -1) {
        return false;
    }
    count = read(descriptor, contents, sizeof(contents) - 1U);
    (void)close(descriptor);
    if (count <= 0) {
        return false;
    }
    contents[(size_t)count] = '\0';
    return strstr(contents, "vendor_id\t: VirtualApple") != NULL;
#else
    return false;
#endif
}

typedef struct {
    pid_t pid;
    int master;
    char slave_name[256];
    struct termios initial_modes;
    bool initial_modes_valid;
    unsigned char capture[CAPTURE_CAP];
    size_t capture_length;
} pty_session;

static int process_child_count(pid_t pid);
static int process_child_pids(pid_t pid, pid_t *children, size_t capacity);

static uint64_t monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static const unsigned char *find_bytes(const unsigned char *haystack,
                                       size_t haystack_length,
                                       const char *needle)
{
    if (needle == NULL) {
        return NULL;
    }
    size_t needle_length = strlen(needle);
    size_t index;

    if (needle_length == 0 || needle_length > haystack_length) {
        return NULL;
    }
    for (index = 0; index + needle_length <= haystack_length; index++) {
        if (memcmp(haystack + index, needle, needle_length) == 0) {
            return haystack + index;
        }
    }
    return NULL;
}

static bool capture_contains(const pty_session *session, const char *text)
{
    if (session == NULL || text == NULL) {
        return false;
    }
    return find_bytes(session->capture, session->capture_length, text) != NULL;
}

static bool capture_ordered(const pty_session *session, const char *first,
                            const char *second)
{
    const unsigned char *first_at;
    const unsigned char *second_at;

    if (session == NULL || first == NULL || second == NULL) return false;
    first_at = find_bytes(session->capture, session->capture_length, first);
    second_at = find_bytes(session->capture, session->capture_length, second);
    return first_at != NULL && second_at != NULL && first_at < second_at;
}

static void dump_capture(const pty_session *session)
{
    if (session == NULL) return;
    if (session->capture_length == 0 ||
        session->capture_length > sizeof(session->capture)) {
        return;
    }
    (void)fputs("pty capture follows:\n", stderr);
    (void)fwrite(session->capture, 1, session->capture_length, stderr);
    (void)fputc('\n', stderr);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
    return 0;
}

static int copy_slave_name(pty_session *session, int master,
                           char slave_name[256])
{
    const char *name;
    size_t length;

    if (!require(session != NULL) || !require(master >= 0)) {
        return -1;
    }
    name = ptsname(master);
    if (name == NULL) return -1;
    length = strlen(name) + 1U;
    if (length > sizeof(session->slave_name)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)memcpy(slave_name, name, length);
    (void)memcpy(session->slave_name, name, length);
    return 0;
}

static int apply_harness_limit(const char *name, int resource,
                               bool invalid_is_error)
{
    if (name == NULL) {
        return -1;
    }
    const char *limit_text = getenv(name);
    struct rlimit limit;
    char *end;
    unsigned long value;

    if (!require(name != NULL) || !require(resource >= 0)) {
        return -1;
    }
    if (limit_text == NULL || limit_text[0] == '\0') {
        return 0;
    }
    value = strtoul(limit_text, &end, 10);
    if (*end != '\0' || value == 0 || getrlimit(resource, &limit) == -1) {
        return invalid_is_error ? -1 : 0;
    }
    limit.rlim_cur = (rlim_t)value;
    if (limit.rlim_cur > limit.rlim_max) {
        limit.rlim_cur = limit.rlim_max;
    }
    if (setrlimit(resource, &limit) == -1 && invalid_is_error) {
        return -1;
    }
    return 0;
}

static int configure_child_limits(void)
{
    if (!require(RLIMIT_NOFILE >= 0) || !require(RLIMIT_DATA >= 0)) {
        return -1;
    }
    if (apply_harness_limit("GSH_HARNESS_NOFILE", RLIMIT_NOFILE, true) ==
            -1 ||
        apply_harness_limit("GSH_HARNESS_DATA", RLIMIT_DATA, true) == -1) {
        return -1;
    }
#ifdef RLIMIT_NPROC
    (void)apply_harness_limit("GSH_HARNESS_NPROC", RLIMIT_NPROC, false);
#endif
    return 0;
}

static void configure_child_environment(shell_kind kind)
{
    const char *features = getenv("GSH_HARNESS_TERM_FEATURES");
    const char *home = getenv("GSH_HARNESS_HOME");
    const char *managed;
    const char *history;
    const char *path = getenv("GSH_HARNESS_PATH");
    const char *repl;

    if (!require(kind >= SHELL_GSH) || !require(kind <= SHELL_ZSH)) {
        return;
    }
    (void)setenv("PATH", path == NULL ? "/usr/bin:/bin" : path, 1);
    if (home != NULL) (void)setenv("HOME", home, 1);
    (void)setenv("TERM", "xterm-256color", 1);
    (void)setenv("PS1", "gsh$ ", 1);
    (void)setenv("PS2", "GSH_MORE> ", 1);
    (void)setenv("PROMPT", "gsh$ ", 1);
    (void)setenv("RPROMPT", "", 1);
    (void)unsetenv("TERM_PROGRAM");
    (void)unsetenv("ITERM_SESSION_ID");
    (void)unsetenv("KITTY_WINDOW_ID");
    if (features == NULL) (void)unsetenv("TERM_FEATURES");
    else (void)setenv("TERM_FEATURES", features, 1);
    if (kind != SHELL_GSH) {
        return;
    }
    managed = getenv("GSH_HARNESS_MANAGED");
    history = getenv("GSH_HARNESS_HISTORY");
    repl = getenv("GSH_HARNESS_REPL");
    if (history != NULL && strcmp(history, "1") == 0) {
        (void)unsetenv("GSH_HISTORY");
    } else {
        (void)setenv("GSH_HISTORY", "off", 1);
    }
    if (repl != NULL) {
        (void)setenv("GSH_REPL", repl, 1);
    } else if (managed != NULL && strcmp(managed, "1") == 0) {
        (void)unsetenv("GSH_REPL");
    } else {
        (void)setenv("GSH_REPL", "classic", 1);
    }
}

static void execute_session_child(int master, int mode_write,
                                  const char *slave_name,
                                  const char *executable,
                                  const char *directory, shell_kind kind)
{
    if (executable == NULL) {
        return;
    }
    struct winsize size;
    struct termios initial_modes;
    int slave;

    if (!require(master >= 0) || !require(mode_write >= 0)) {
        _exit(119);
    }
    (void)close(master);
    if (setsid() == -1 || (slave = open(slave_name, O_RDWR)) == -1) {
        _exit(121);
    }
    (void)memset(&size, 0, sizeof(size));
    size.ws_row = 24;
    size.ws_col = 80;
    if (ioctl(slave, TIOCSWINSZ, &size) == -1 ||
        tcgetattr(slave, &initial_modes) == -1 ||
        write(mode_write, &initial_modes, sizeof(initial_modes)) !=
            (ssize_t)sizeof(initial_modes)) {
        _exit(123);
    }
    (void)close(mode_write);
#ifdef TIOCSCTTY
    if (ioctl(slave, TIOCSCTTY, 0) == -1 && errno != EINVAL) {
        _exit(124);
    }
#endif
    if (dup2(slave, STDIN_FILENO) == -1 ||
        dup2(slave, STDOUT_FILENO) == -1 ||
        dup2(slave, STDERR_FILENO) == -1) {
        _exit(125);
    }
    if (slave > STDERR_FILENO) {
        (void)close(slave);
    }
    if (tcsetpgrp(STDIN_FILENO, getpgrp()) == -1 || chdir(directory) == -1) {
        _exit(126);
    }
    configure_child_environment(kind);
    if (configure_child_limits() == -1) {
        _exit(127);
    }
    if (kind == SHELL_BASH) {
        execl(executable, executable, "--noprofile", "--norc", "-i",
              (char *)NULL);
    } else if (kind == SHELL_ZSH) {
        execl(executable, executable, "-f", (char *)NULL);
    } else {
        execl(executable, executable, (char *)NULL);
    }
    _exit(127);
}

static int read_initial_modes(int descriptor, struct termios *modes)
{
    size_t offset = 0;

    if (!require(descriptor >= 0) || !require(modes != NULL)) {
        return -1;
    }
    while (offset < sizeof(*modes)) {
        ssize_t count = read(descriptor, (unsigned char *)modes + offset,
                             sizeof(*modes) - offset);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count != -1 || errno != EINTR) {
            break;
        }
    }
    (void)close(descriptor);
    return offset == sizeof(*modes) ? 0 : -1;
}

static int fail_session_start(pid_t pid, int master, int saved_errno)
{
    if (!require(pid > 0) || !require(master >= 0)) {
        return -1;
    }
    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    (void)close(master);
    errno = saved_errno;
    return -1;
}

static int start_session(pty_session *session, const char *executable,
                         const char *directory, shell_kind kind)
{
    if (session == NULL) return -1;
    char slave_name[256];
    int mode_pipe[2];
    int master;
    pid_t pid;

    (void)memset(session, 0, sizeof(*session));
    session->master = -1;
    if (directory == NULL || executable == NULL) return -1;
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (!require(master >= 0)) return -1;
    if (grantpt(master) == -1 || unlockpt(master) == -1) {
        (void)close(master);
        return -1;
    }
    if (copy_slave_name(session, master, slave_name) == -1) {
        (void)close(master);
        return -1;
    }
    if (pipe(mode_pipe) == -1) {
        (void)close(master);
        return -1;
    }
    pid = fork();
    if (pid == 0) {
        (void)close(mode_pipe[0]);
        execute_session_child(master, mode_pipe[1], slave_name, executable,
                              directory, kind);
    }
    (void)close(mode_pipe[1]);
    if (pid == -1) {
        (void)close(mode_pipe[0]);
        (void)close(master);
        return -1;
    }
    if (read_initial_modes(mode_pipe[0], &session->initial_modes) == -1) {
        return fail_session_start(pid, master, EIO);
    }
    session->initial_modes_valid = true;
    if (set_nonblocking(master) == -1) {
        int saved_errno = errno;

        return fail_session_start(pid, master, saved_errno);
    }
    session->pid = pid;
    session->master = master;
    return 0;
}

static int start_managed_session(pty_session *session,
                                 const char *executable,
                                 const char *directory)
{
    if (session == NULL) return -1;
    int result;

    (void)memset(session, 0, sizeof(*session));
    session->master = -1;
    if (directory == NULL || executable == NULL) return -1;
    if (setenv("GSH_HARNESS_MANAGED", "1", 1) == -1 ||
        setenv("GSH_HARNESS_REPL", "async", 1) == -1) {
        return -1;
    }
    result = start_session(session, executable, directory, SHELL_GSH);
    (void)unsetenv("GSH_HARNESS_MANAGED");
    (void)unsetenv("GSH_HARNESS_REPL");
    return result;
}

static int wait_for_output(pty_session *session, const char *marker,
                           int timeout_ms)
{
    if (marker == NULL || session == NULL) {
        return -1;
    }
    uint64_t deadline = monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    size_t attempt;

    for (attempt = 0; attempt < PTY_WAIT_ATTEMPT_CAP; attempt++) {
        struct pollfd descriptor;
        const unsigned char *found;
        uint64_t now;
        int remaining_ms;
        int result;

        found = find_bytes(session->capture, session->capture_length, marker);
        if (found != NULL) {
            return 0;
        }
        now = monotonic_ns();
        if (now >= deadline) {
            errno = ETIMEDOUT;
            return -1;
        }
        remaining_ms = (int)((deadline - now + 999999ULL) / 1000000ULL);
        descriptor.fd = session->master;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        result = poll(&descriptor, 1, remaining_ms);
        if (result == -1 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            errno = result == 0 ? ETIMEDOUT : errno;
            return -1;
        }
        if ((descriptor.revents & POLLIN) != 0) {
            ssize_t count;

            if (session->capture_length == sizeof(session->capture)) {
                errno = ENOBUFS;
                return -1;
            }
            count = read(session->master,
                         session->capture + session->capture_length,
                         sizeof(session->capture) - session->capture_length);
            if (count > 0) {
                session->capture_length += (size_t)count;
                continue;
            }
            if (count == -1 &&
                (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                continue;
            }
        }
        errno = EIO;
        return -1;
    }
    errno = EAGAIN;
    return -1;
}

static int consume_through(pty_session *session, const char *marker,
                           int timeout_ms)
{
    const unsigned char *found;
    size_t consumed;

    if (wait_for_output(session, marker, timeout_ms) == -1) {
        return -1;
    }
    found = find_bytes(session->capture, session->capture_length, marker);
    consumed = (size_t)(found - session->capture) + strlen(marker);
    (void)memmove(session->capture, session->capture + consumed,
            session->capture_length - consumed);
    session->capture_length -= consumed;
    return 0;
}

static int send_bytes(pty_session *session, const char *bytes, size_t length)
{
    if (bytes == NULL || session == NULL) {
        return -1;
    }
    size_t offset = 0;

    while (offset < length) {
        ssize_t count = write(session->master, bytes + offset, length - offset);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else if (count == -1 &&
                   (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd descriptor = {session->master, POLLOUT, 0};

            if (poll(&descriptor, 1, TEST_TIMEOUT_MS) > 0) {
                continue;
            }
            return -1;
        } else {
            return -1;
        }
    }
    return 0;
}

static int send_text(pty_session *session, const char *text)
{
    if (session == NULL || text == NULL) {
        return -1;
    }
    return send_bytes(session, text, strlen(text));
}

static bool terminal_modes_match(const struct termios *left,
                                 const struct termios *right)
{
    if (left == NULL || right == NULL) {
        return false;
    }
    return left->c_iflag == right->c_iflag &&
           left->c_oflag == right->c_oflag &&
           left->c_cflag == right->c_cflag &&
           left->c_lflag == right->c_lflag &&
           memcmp(left->c_cc, right->c_cc, sizeof(left->c_cc)) == 0 &&
           cfgetispeed(left) == cfgetispeed(right) &&
           cfgetospeed(left) == cfgetospeed(right);
}

static bool terminal_was_restored(const pty_session *session)
{
    if (session == NULL) return false;
    struct termios final_modes;
    int slave;
    bool restored;

    if (!session->initial_modes_valid) {
        return false;
    }
    slave = open(session->slave_name, O_RDWR | O_NOCTTY);
    if (slave == -1 || tcgetattr(slave, &final_modes) == -1) {
        if (slave >= 0) {
            (void)close(slave);
        }
        return false;
    }
    (void)close(slave);
    restored = terminal_modes_match(&session->initial_modes, &final_modes);
    return restored;
}

static int resize_session(const pty_session *session, unsigned short rows,
                          unsigned short columns)
{
    if (session == NULL) {
        return -1;
    }
    struct winsize size;
    int slave = open(session->slave_name, O_RDWR | O_NOCTTY);
    int result;

    if (slave == -1) {
        return -1;
    }
    (void)memset(&size, 0, sizeof(size));
    size.ws_row = rows;
    size.ws_col = columns;
    result = ioctl(slave, TIOCSWINSZ, &size);
    (void)close(slave);
    return result;
}

static bool wait_for_session_children(const pid_t children[256],
                                      int child_count)
{
    uint64_t deadline;
    bool alive = true;

    if (children == NULL) return false;
    if (child_count <= 0) return true;
    deadline = monotonic_ns() + 1000000000ULL;
    while (alive && monotonic_ns() < deadline) {
        int index;

        alive = false;
        for (index = 0; index < child_count; index++) {
            if (kill(children[index], 0) == 0 || errno != ESRCH) alive = true;
        }
        if (alive) (void)poll(NULL, 0, 10);
    }
    if (!alive) return true;
    for (int index = 0; index < child_count; index++) {
        if (kill(children[index], 0) == 0 || errno != ESRCH) {
            (void)fprintf(stderr,
                          "pty stop: child %ld survived shutdown\n",
                          (long)children[index]);
            (void)kill(children[index], SIGKILL);
        }
    }
    return false;
}

static int stop_session(pty_session *session)
{
    if (session == NULL) return -1;
    pid_t children[256];
    int child_count;
    uint64_t deadline;
    int status = 0;

    if (session->pid <= 0) {
        return 0;
    }
    child_count = process_child_pids(
        session->pid, children, sizeof(children) / sizeof(children[0]));
    (void)send_text(session, "exit 0\r");
    deadline = monotonic_ns() + 2000000000ULL;
    for (size_t attempt = 0; attempt < PTY_WAIT_ATTEMPT_CAP; attempt++) {
        pid_t result = waitpid(session->pid, &status, WNOHANG);

        if (result == session->pid) {
            break;
        }
        if (result == -1 && errno != EINTR) {
            status = -1;
            break;
        }
        if (monotonic_ns() >= deadline) {
            (void)fprintf(stderr, "pty stop: shell exit deadline exceeded\n");
            (void)kill(session->pid, SIGKILL);
            (void)waitpid(session->pid, &status, 0);
            status = -1;
            break;
        }
        {
            struct pollfd descriptor = {session->master, POLLIN, 0};

            if (poll(&descriptor, 1, 20) > 0) {
                unsigned char discard[1024];
                ssize_t discarded;

                discarded = read(session->master, discard, sizeof(discard));
                (void)discarded;
            }
        }
    }
    if (!wait_for_session_children(children, child_count)) status = -1;
    if (!terminal_was_restored(session)) {
        (void)fprintf(stderr, "pty stop: terminal modes were not restored\n");
        status = -1;
    }
    (void)close(session->master);
    session->master = -1;
    session->pid = -1;
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0
                                                                        : -1;
}

static int wait_session_result(pty_session *session, int *result_status,
                               int timeout_ms)
{
    if (result_status == NULL || session == NULL) {
        return -1;
    }
    uint64_t deadline = monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    int status = 0;

    while (monotonic_ns() < deadline) {
        pid_t result = waitpid(session->pid, &status, WNOHANG);

        if (result == session->pid) {
            bool restored = terminal_was_restored(session);

            for (size_t attempt = 0; attempt < PTY_DRAIN_ATTEMPT_CAP;
                 attempt++) {
                ssize_t count;

                if (session->capture_length == sizeof(session->capture)) {
                    break;
                }
                count = read(session->master,
                             session->capture + session->capture_length,
                             sizeof(session->capture) -
                                 session->capture_length);
                if (count > 0) {
                    session->capture_length += (size_t)count;
                } else if (count == -1 && errno == EINTR) {
                    continue;
                } else {
                    break;
                }
            }

            (void)close(session->master);
            session->master = -1;
            session->pid = -1;
            *result_status = status;
            return restored ? 0 : -1;
        }
        if (result == -1 && errno != EINTR) {
            break;
        }
        {
            struct pollfd descriptor = {session->master, POLLIN, 0};

            if (poll(&descriptor, 1, 20) > 0) {
                ssize_t count;

                if (session->capture_length < sizeof(session->capture)) {
                    count = read(session->master,
                                 session->capture + session->capture_length,
                                 sizeof(session->capture) -
                                     session->capture_length);
                    if (count > 0) {
                        session->capture_length += (size_t)count;
                    }
                }
            }
        }
    }
    (void)kill(session->pid, SIGKILL);
    (void)waitpid(session->pid, NULL, 0);
    (void)close(session->master);
    session->master = -1;
    session->pid = -1;
    return -1;
}

static int wait_session_exit(pty_session *session, int expected_status,
                             int timeout_ms)
{
    int status = 0;

    if (wait_session_result(session, &status, timeout_ms) == 0 &&
        WIFEXITED(status) && WEXITSTATUS(status) == expected_status) {
        return 0;
    }
    (void)fprintf(stderr,
            "pty harness: exit mismatch expected=%d exited=%d actual=%d\n",
            expected_status, WIFEXITED(status) ? 1 : 0,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
}

static int wait_for_diagnostics(pty_session *session, const char *first,
                                const char *second)
{
    if (session == NULL) {
        return -1;
    }
    uint64_t deadline = monotonic_ns() + 2000000000ULL;

    while (monotonic_ns() < deadline) {
        session->capture_length = 0;
        if (send_text(session, "rt\r") == -1 ||
            wait_for_output(session, "reactor cycles=", TEST_TIMEOUT_MS) ==
                -1 ||
            wait_for_output(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
            return -1;
        }
        if (capture_contains(session, first) &&
            (second == NULL || capture_contains(session, second))) {
            session->capture_length = 0;
            return 0;
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

static int write_text_file(const char *path, const char *text, mode_t mode)
{
    if (path == NULL || text == NULL) {
        return -1;
    }
    size_t length = strlen(text);
    size_t offset = 0;
    size_t attempts;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    int status = 0;

    if (descriptor == -1) {
        return -1;
    }
    for (attempts = 0; offset < length && attempts <= length; attempts++) {
        ssize_t count = write(descriptor, text + offset, length - offset);

        if (count > 0) {
            offset += (size_t)count;
        } else if (!(count == -1 && errno == EINTR)) {
            status = -1;
            break;
        }
    }
    if (offset != length) {
        status = -1;
    }
    if (close(descriptor) == -1) {
        status = -1;
    }
    return status;
}

static int write_binary_file(const char *path, const unsigned char *bytes,
                             size_t length, mode_t mode)
{
    size_t offset = 0U;
    int descriptor;
    int status = 0;

    if (path == NULL || bytes == NULL || length == 0U) return -1;
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (descriptor < 0) return -1;
    for (size_t attempt = 0U; offset < length && attempt <= length;
         attempt++) {
        ssize_t count = write(descriptor, bytes + offset, length - offset);

        if (count > 0) offset += (size_t)count;
        else if (!(count == -1 && errno == EINTR)) { status = -1; break; }
    }
    if (offset != length || close(descriptor) == -1) status = -1;
    return status;
}

static void remove_fixture(const char *root)
{
    char path[1024];

    if (snprintf(path, sizeof(path), "%s/native.out", root) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
    if (snprintf(path, sizeof(path), "%s/with space", root) <
        (int)sizeof(path)) {
        (void)rmdir(path);
    }
    (void)rmdir(root);
}

/* ── Session Owners Replace Cleanup Jumps ───────────────────────
 * PTY scenarios once jumped to local labels after any failed interaction,
 * which obscured whether a child and its terminal were always reclaimed.
 * These small owners make the cleanup contract callable from every early
 * return and preserve the original status if shutdown also reports failure.
 * Flat and populated fixtures remain separate, explicit policies.
 * ─────────────────────────────────────────────────────────────── */
static int finish_session_directory(pty_session *session,
                                    const char *directory, int failed)
{
    if (session == NULL) return -1;
    if (directory == NULL) {
        return -1;
    }
    if (session->pid > 0 && stop_session(session) == -1) {
        failed = 1;
    }
    (void)rmdir(directory);
    return failed;
}

static int finish_session_tree(pty_session *session, const char *directory,
                               int failed)
{
    if (session == NULL) return -1;
    if (directory == NULL) {
        return -1;
    }
    if (session->pid > 0 && stop_session(session) == -1) {
        failed = 1;
    }
    remove_fixture(directory);
    return failed;
}

static int write_history_config(const char *home)
{
    static const char configuration[] =
        "config.version = 1\n\n"
        "shell.history.enabled = true\n"
        "shell.history.max_entries = 1024\n"
        "shell.history.deduplicate = false\n"
        "shell.history.store_failed = true\n"
        "shell.history.ignore_space = true\n";
    char path[PATH_MAX];
    int descriptor;
    ssize_t written;

    if (snprintf(path, sizeof(path), "%s/.gshrc", home) >=
        (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor == -1) {
        return -1;
    }
    written = write(descriptor, configuration, sizeof(configuration) - 1U);
    if (written != (ssize_t)(sizeof(configuration) - 1U) ||
        fsync(descriptor) == -1 || close(descriptor) == -1) {
        return -1;
    }
    return 0;
}

static void remove_history_fixture(const char *home)
{
    char path[PATH_MAX];

    if (snprintf(path, sizeof(path), "%s/.gsh_history", home) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
    if (snprintf(path, sizeof(path), "%s/.gsh_history.lock", home) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
    if (snprintf(path, sizeof(path), "%s/.gshrc", home) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
    (void)rmdir(home);
}

static bool history_file_is_plaintext(const char *home)
{
    static const char plaintext[] = "HISTORY_ALPHA";
    unsigned char bytes[8192 + sizeof(plaintext)];
    char path[PATH_MAX];
    struct stat status;
    size_t carry = 0;
    unsigned int reads;
    int descriptor;
    bool found = false;

    if (snprintf(path, sizeof(path), "%s/.gsh_history", home) >=
        (int)sizeof(path)) {
        return false;
    }
    descriptor = open(path, O_RDONLY);
    if (descriptor == -1 || fstat(descriptor, &status) == -1 ||
        !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0077) != 0) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return false;
    }
    for (reads = 0; reads < 1024U; reads++) {
        ssize_t count = read(descriptor, bytes + carry, 8192U);
        size_t total;

        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        total = carry + (size_t)count;
        if (find_bytes(bytes, total, plaintext) != NULL) {
            found = true;
            break;
        }
        carry = total < sizeof(plaintext) - 1U
                    ? total
                    : sizeof(plaintext) - 1U;
        (void)memmove(bytes, bytes + total - carry, carry);
    }
    (void)close(descriptor);
    return found;
}

static int exercise_history_editor(pty_session *session)
{
    static const char alpha[] = "/usr/bin/printf 'HISTORY_ALPHA\\n'";
    static const char beta[] = "/usr/bin/printf 'HISTORY_BETA\\n'";
    static const char multiline[] =
        "\033[200~/usr/bin/printf 'HISTORY_MULTI_A\\n'\n"
        "/usr/bin/printf 'HISTORY_MULTI_B\\n'\033[201~\r";

    if (send_text(session, "/usr/bin/printf 'HISTORY_ALPHA\\n'\r") == -1 ||
        consume_through(session, "HISTORY_ALPHA\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/usr/bin/printf 'HISTORY_BETA\\n'\r") == -1 ||
        consume_through(session, "HISTORY_BETA\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033[A", 3) == -1 ||
        consume_through(session, beta, TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033[A", 3) == -1 ||
        consume_through(session, alpha, TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033[B", 3) == -1 ||
        consume_through(session, beta, TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025\022ALPHA", 7) == -1 ||
        consume_through(session, "(reverse-i-search)`ALPHA': ",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, alpha, TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "\r") == -1 ||
        consume_through(session, "HISTORY_ALPHA\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  " /usr/bin/printf 'HISTORY_PRIVATE\\n' \r") == -1 ||
        consume_through(session, "HISTORY_PRIVATE\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033[A", 3) == -1 ||
        consume_through(session, alpha, TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        send_text(session, "history status\r") == -1 ||
        consume_through(session, "entries=4 max=1024 file=",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, multiline) == -1 ||
        consume_through(session, "HISTORY_MULTI_A\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "HISTORY_MULTI_B\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int verify_history_reuse(pty_session *session)
{
    if (consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033[A", 3) == -1 ||
        consume_through(session, "exit 0", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025\022ALPHA", 7) == -1 ||
        consume_through(session, "(reverse-i-search)`ALPHA': ",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "HISTORY_ALPHA", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033x", 2) == -1 ||
        send_bytes(session, "\025\022MULTI_A", 9) == -1 ||
        consume_through(session, "(reverse-i-search)`MULTI_A': ",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "HISTORY_MULTI_A", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "\r") == -1 ||
        consume_through(session, "HISTORY_MULTI_A\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "HISTORY_MULTI_B\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        send_text(session, "history status\r") == -1 ||
        consume_through(session, "persistent=yes", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int verify_fresh_history(pty_session *session)
{
    if (consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033[A", 3) == -1 ||
        consume_through(session, "exit 0", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025\022CONCURRENT_A", 14) == -1 ||
        consume_through(session, "HISTORY_CONCURRENT_A",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033x\025\022CONCURRENT_B", 16) == -1 ||
        consume_through(session, "HISTORY_CONCURRENT_B",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033x\025", 3) == -1) {
        return -1;
    }
    return 0;
}

static int history_session_failure(pty_session *session, const char *stage)
{
    if (!require(session != NULL && stage != NULL)) return -1;
    (void)fprintf(stderr, "pty smoke: history %s failed\n", stage);
    dump_capture(session);
    return -1;
}

static int run_initial_history_session(const char *executable,
                                       const char *home)
{
    pty_session session = {0};
    int failed = 0;

    if (start_session(&session, executable, home, SHELL_GSH) == -1) {
        return -1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        failed = history_session_failure(&session, "initial prompt");
    } else if (exercise_history_editor(&session) == -1) {
        failed = history_session_failure(&session, "editor exercise");
    }
    if (stop_session(&session) == -1) {
        failed = history_session_failure(&session, "initial shutdown");
    }
    return failed;
}

static int run_reused_history_session(const char *executable,
                                      const char *home)
{
    pty_session session;
    int failed = 0;

    if (start_session(&session, executable, home, SHELL_GSH) == -1) {
        return -1;
    }
    if (verify_history_reuse(&session) == -1) {
        failed = history_session_failure(&session, "managed reuse");
    }
    if (stop_session(&session) == -1) {
        failed = history_session_failure(&session, "managed shutdown");
    }
    return failed;
}

static int run_fresh_history_session(const char *executable,
                                     const char *home)
{
    pty_session session;
    int failed = 0;

    if (start_session(&session, executable, home, SHELL_GSH) == -1) {
        return -1;
    }
    if (verify_fresh_history(&session) == -1) {
        failed = history_session_failure(&session, "fresh reuse");
    }
    if (stop_session(&session) == -1) {
        failed = history_session_failure(&session, "fresh shutdown");
    }
    return failed;
}

static int run_concurrent_history_sessions(const char *executable,
                                           const char *home)
{
    pty_session first = {0};
    pty_session second = {0};
    int failed = 0;

    if (!require(executable != NULL)) return -1;
    if (!require(home != NULL)) return -1;
    if (start_session(&first, executable, home, SHELL_GSH) == -1 ||
        start_session(&second, executable, home, SHELL_GSH) == -1) {
        failed = -1;
    } else if (consume_through(&first, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
               consume_through(&second, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
               send_text(&first,
                   "/usr/bin/printf 'HISTORY_CONCURRENT_A\\n'\r") == -1 ||
               consume_through(&first, "HISTORY_CONCURRENT_A\r\n",
                               TEST_TIMEOUT_MS) == -1 ||
               consume_through(&first, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
               send_text(&second,
                   "/usr/bin/printf 'HISTORY_CONCURRENT_B\\n'\r") == -1 ||
               consume_through(&second, "HISTORY_CONCURRENT_B\r\n",
                               TEST_TIMEOUT_MS) == -1 ||
               consume_through(&second, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        failed = -1;
    }
    if (first.pid > 0 && stop_session(&first) == -1) failed = -1;
    if (second.pid > 0 && stop_session(&second) == -1) failed = -1;
    return failed;
}

static int history_flow(const char *executable)
{
    char home[] = "/tmp/gsh-history-XXXXXX";
    char saved_home[PATH_MAX];
    const char *current_home = getenv("HOME");
    int failed = 0;

    saved_home[0] = '\0';
    if (current_home != NULL && strlen(current_home) < sizeof(saved_home)) {
        (void)memcpy(saved_home, current_home, strlen(current_home) + 1U);
    }
    if (mkdtemp(home) == NULL || write_history_config(home) == -1 ||
        setenv("HOME", home, 1) == -1 ||
        setenv("GSH_HARNESS_HISTORY", "1", 1) == -1) {
        failed = 1;
    } else if (run_initial_history_session(executable, home) == -1 ||
               !history_file_is_plaintext(home)) {
        failed = 1;
    }
    if (!failed &&
        run_concurrent_history_sessions(executable, home) == -1) failed = 1;
    if (!failed &&
        (setenv("GSH_HARNESS_MANAGED", "1", 1) == -1 ||
         run_reused_history_session(executable, home) == -1)) {
        failed = 1;
    }
    (void)unsetenv("GSH_HARNESS_MANAGED");
    if (!failed && run_fresh_history_session(executable, home) == -1) {
        failed = 1;
    }
    (void)unsetenv("GSH_HARNESS_HISTORY");
    if (saved_home[0] != '\0') {
        (void)setenv("HOME", saved_home, 1);
    } else {
        (void)unsetenv("HOME");
    }
    remove_history_fixture(home);
    return failed;
}

static int exercise_editor_navigation(pty_session *session)
{
    static const char paste[] =
        "\033[200~value=ONE\nvalue=\"${value}_TWO\"\n"
        "printf 'GSH_PASTE_RESULT=<%s>\\n' \"$value\"\033[201~";

    if (session == NULL ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "printf 'GSH_CURSOR=<%s>\\n' ac") == -1 ||
        send_bytes(session, "\033[D\033[D\033[C", 9U) == -1 ||
        send_text(session, "b\r") == -1 ||
        consume_through(session, "GSH_CURSOR=<abc>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "printf 'GSH_DELETE=<%s>\\n' axbc") == -1 ||
        send_bytes(session, "\033[D\033[D\177", 7U) == -1 ||
        send_text(session, "\r") == -1 ||
        consume_through(session, "GSH_DELETE=<abc>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "printf 'GSH_UTF8=<%s>\\n' aé") == -1 ||
        send_bytes(session, "\033[D", 3U) == -1 ||
        send_text(session, "x\r") == -1 ||
        consume_through(session, "GSH_UTF8=<axé>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "ech") == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "echo ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "GSH_TAB_COMMAND\r") == -1 ||
        consume_through(session, "GSH_TAB_COMMAND\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "printf 'GSH_TAB_PATH=<%s>\\n' completion\\ a") == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "completion\\ alpha ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "\r") == -1 ||
        consume_through(session, "GSH_TAB_PATH=<completion alpha>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "printf 'GSH_TAB_LCP=<%s>\\n' am") == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "amber-", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "one\r") == -1 ||
        consume_through(session, "GSH_TAB_LCP=<amber-one>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "printf 'GSH_TAB_DIR=<%s>\\n' completion-d") == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "completion-dir/", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "\r") == -1 ||
        consume_through(session, "GSH_TAB_DIR=<", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, ">\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "cd completion") == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "cd completion-dir/",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1U) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "cd ~") == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "cd ~/", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1U) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "cd ") == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "completion-dir/", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "cycle-alpha/", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "cycle-beta\\ space/",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "cd completion-dir/",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "\033[7mcompletion-dir/\033[0m",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "cd cycle-alpha/", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "\033[7mcycle-alpha/\033[0m",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "cd cycle-beta\\ space/",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "\033[7mcycle-beta\\ space/\033[0m",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1U) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "echo $PA") == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "$PAGER", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "$PATH", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "echo $PAGER", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "echo $PATH", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1U) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "git sta") == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "stash", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "status", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "git stash ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "git status ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1U) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "ls -A") == -1 ||
        send_bytes(session, "\t", 1U) == -1 ||
        consume_through(session, "ls -A ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1U) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    session->capture_length = 0U;
    if (send_text(session, paste) == -1 ||
        wait_for_output(session, "printf 'GSH_PASTE_RESULT=<%s>\\n'",
                        TEST_TIMEOUT_MS) == -1 ||
        !capture_contains(session, "value=ONE") ||
        !capture_contains(session, "value=\"${value}_TWO\"") ||
        capture_contains(session, "GSH_PASTE_RESULT=<ONE_TWO>\r\n") ||
        send_text(session, "\r") == -1 ||
        consume_through(session, "GSH_PASTE_RESULT=<ONE_TWO>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static void remove_editor_fixture(const char *root, const char *completion,
                                  const char *amber_one,
                                  const char *amber_two,
                                  const char *completion_directory,
                                  const char *cycle_alpha,
                                  const char *cycle_beta)
{
    if (!require(root != NULL && completion != NULL)) return;
    if (!require(amber_one != NULL && amber_two != NULL)) return;
    if (!require(completion_directory != NULL && cycle_alpha != NULL &&
                 cycle_beta != NULL)) return;
    (void)unlink(completion);
    (void)unlink(amber_one);
    (void)unlink(amber_two);
    (void)rmdir(completion_directory);
    (void)rmdir(cycle_alpha);
    (void)rmdir(cycle_beta);
    (void)rmdir(root);
}

static int editor_navigation_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-editor-flow-XXXXXX";
    char completion[PATH_MAX];
    char completion_directory[PATH_MAX];
    char cycle_alpha[PATH_MAX];
    char cycle_beta[PATH_MAX];
    char amber_one[PATH_MAX];
    char amber_two[PATH_MAX];
    pty_session session;
    int failed = 0;

    if (executable == NULL || mkdtemp(fixture) == NULL) return 1;
    if (snprintf(completion, sizeof(completion), "%s/completion alpha",
                 fixture) >= (int)sizeof(completion) ||
        snprintf(amber_one, sizeof(amber_one), "%s/amber-one", fixture) >=
            (int)sizeof(amber_one) ||
        snprintf(amber_two, sizeof(amber_two), "%s/amber-two", fixture) >=
            (int)sizeof(amber_two) ||
        snprintf(completion_directory, sizeof(completion_directory),
                 "%s/completion-dir", fixture) >=
            (int)sizeof(completion_directory) ||
        snprintf(cycle_alpha, sizeof(cycle_alpha), "%s/cycle-alpha",
                 fixture) >= (int)sizeof(cycle_alpha) ||
        snprintf(cycle_beta, sizeof(cycle_beta), "%s/cycle-beta space",
                 fixture) >= (int)sizeof(cycle_beta)) {
        (void)rmdir(fixture);
        return 1;
    }
    if (write_text_file(completion, "completion\n", 0600) == -1 ||
        write_text_file(amber_one, "one\n", 0600) == -1 ||
        write_text_file(amber_two, "two\n", 0600) == -1 ||
        mkdir(completion_directory, 0700) == -1 ||
        mkdir(cycle_alpha, 0700) == -1 ||
        mkdir(cycle_beta, 0700) == -1) {
        remove_editor_fixture(fixture, completion, amber_one, amber_two,
                              completion_directory, cycle_alpha, cycle_beta);
        return 1;
    }
    if (start_session(&session, executable, fixture, SHELL_GSH) == -1 ||
        exercise_editor_navigation(&session) == -1) {
        perror("pty editor: classic flow");
        dump_capture(&session);
        failed = 1;
    }
    if (session.master >= 0 && stop_session(&session) == -1) failed = 1;
    if (!failed &&
        (start_managed_session(&session, executable, fixture) == -1 ||
         exercise_editor_navigation(&session) == -1)) {
        perror("pty editor: managed flow");
        dump_capture(&session);
        failed = 1;
    }
    if (session.master >= 0 && stop_session(&session) == -1) failed = 1;
    remove_editor_fixture(fixture, completion, amber_one, amber_two,
                          completion_directory, cycle_alpha, cycle_beta);
    return failed;
}

static int ordinary_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-flow-XXXXXX";
    char probe[4096];
    char command[4100];
    char spaced_directory[1024];
    char *separator;
    pty_session session;
    int failed = 0;

    if (strlen(executable) + 1U > sizeof(probe)) {
        (void)fprintf(stderr, "pty smoke: probe path is too long\n");
        return 1;
    }
    (void)memcpy(probe, executable, strlen(executable) + 1U);
    separator = strrchr(probe, '/');
    if (separator == NULL ||
        (size_t)(separator - probe) + sizeof("/job-probe") > sizeof(probe)) {
        (void)fprintf(stderr, "pty smoke: cannot derive job probe path\n");
        return 1;
    }
    (void)memcpy(separator, "/job-probe", sizeof("/job-probe"));
    if (snprintf(command, sizeof(command), "%s\r", probe) >=
        (int)sizeof(command)) {
        (void)fprintf(stderr, "pty smoke: probe command is too long\n");
        return 1;
    }

    if (mkdtemp(fixture) == NULL ||
        snprintf(spaced_directory, sizeof(spaced_directory), "%s/with space",
                 fixture) >= (int)sizeof(spaced_directory) ||
        mkdir(spaced_directory, 0700) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty smoke: ordinary setup");
        remove_fixture(fixture);
        return 1;
    }

    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/true\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' one\\\r") == -1 ||
        consume_through(&session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "two\r") == -1 ||
        consume_through(&session, "<onetwo>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf X | /usr/bin/tr X Y\r") == -1 ||
        consume_through(&session, "\nY", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/yes X | /usr/bin/head -n 1\r") == -1 ||
        consume_through(&session, "\nX\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '%s\\n' GSH_NATIVE_REDIRECT_OUTPUT "
                  "> native.out\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/cat native.out\r") == -1 ||
        consume_through(&session, "\nGSH_NATIVE_REDIRECT_OUTPUT\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/false && /usr/bin/printf BAD || "
                  "/usr/bin/printf GSH_NATIVE_AND_OR\r") == -1 ||
        consume_through(&session, "\nGSH_NATIVE_AND_OR",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if /usr/bin/false; then /usr/bin/printf BAD; else "
                  "/usr/bin/printf GSH_NATIVE_IF; fi\r") == -1 ||
        consume_through(&session, "\nGSH_NATIVE_IF", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "cd 'with space'\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/pwd\r") == -1 ||
        consume_through(&session, "/with space\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "cd ..\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/cat <<EOF\r") == -1 ||
        consume_through(&session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "GSH_PTY_HEREDOC status=$?\r") == -1 ||
        consume_through(&session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "EOF\r") == -1 ||
        consume_through(&session, "\nGSH_PTY_HEREDOC status=0\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/cat <<CANCEL\r") == -1 ||
        consume_through(&session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "^C", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"$(/usr/bin/printf GSH_SUBSTITUTION)\"\r") == -1 ||
        consume_through(&session, "<GSH_SUBSTITUTION>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"$(/usr/bin/printf 'GSH_SUB_%s' START 1>&2; "
                  "/bin/sleep 5; /usr/bin/printf late)\"\r") == -1 ||
        consume_through(&session, "GSH_SUB_START", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "GSH_PERSIST=value\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PERSIST\"\r") == -1 ||
        consume_through(&session, "<value>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, ": \"${GSH_TRANSACTION:=committed}\"\r") ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_TRANSACTION\"\r") ==
            -1 ||
        consume_through(&session, "<committed>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s:%s>\\n' "
                  "\"$((GSH_ARITHMETIC = 7))\" "
                  "\"$GSH_ARITHMETIC\"\r") == -1 ||
        consume_through(&session, "<7:7>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' native.*\r") == -1 ||
        consume_through(&session, "<native.out>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "rt\r") == -1 ||
        wait_for_output(&session,
                        "direct=3 native=17 shell=0 parsed=20 parse_failures=0 "
                        "job=idle worker=on",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "GSH_PROBE_READY", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "]+ Stopped ", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "fg\r") == -1 ||
        consume_through(&session, "GSH_PROBE_CONTINUED", TEST_TIMEOUT_MS) ==
            -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty smoke: ordinary flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        (void)fprintf(stderr,
                "pty smoke: completed pipelines left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        (void)fprintf(stderr, "pty smoke: ordinary shell did not exit cleanly\n");
        failed = 1;
    }
    remove_fixture(fixture);
    return failed;
}

static int job_flow_failure(pty_session *session, const char *stage)
{
    if (session == NULL || stage == NULL) {
        return -1;
    }
    int saved_errno = errno;

    (void)fprintf(stderr, "pty jobs: %s: %s\n", stage, strerror(saved_errno));
    dump_capture(session);
    errno = saved_errno;
    return -1;
}

static int job_expect(pty_session *session, const char *marker,
                      const char *stage)
{
    if (stage == NULL) {
        return -1;
    }
    if (consume_through(session, marker, JOB_TRANSITION_TIMEOUT_MS) == -1) {
        return job_flow_failure(session, stage);
    }
    return 0;
}

static int job_observe(pty_session *session, const char *marker,
                       const char *stage)
{
    if (stage == NULL) {
        return -1;
    }
    if (wait_for_output(session, marker, JOB_TRANSITION_TIMEOUT_MS) == -1) {
        return job_flow_failure(session, stage);
    }
    return 0;
}

static int job_send(pty_session *session, const char *command,
                    const char *stage)
{
    if (stage == NULL) {
        return -1;
    }
    if (send_text(session, command) == -1) {
        return job_flow_failure(session, stage);
    }
    return 0;
}

static int job_start_stopped(pty_session *session, const char *command,
                             const char *job_marker)
{
    if (command == NULL || job_marker == NULL || session == NULL) {
        return -1;
    }
    return job_send(session, command, "launch stopped probe") == -1 ||
                   job_expect(session, job_marker, "observe job id") == -1 ||
                   job_expect(session, "GSH_PROBE_READY",
                              "observe probe readiness") == -1 ||
                   job_expect(session, "]+ Stopped ",
                              "observe stopped notification") == -1 ||
                   job_expect(session, "gsh$ ",
                              "observe prompt after stop") == -1
               ? -1
               : 0;
}

/* ── Job Markers And Prompts Synchronize Independently ───────────
 * Sanitized macOS runs exposed that resumed output can lead or trail notices.
 * Consuming any event first could discard another one already in the capture.
 * The harness now observes both markers without assigning them an order.
 * Clearing the captured pair prevents that prompt from satisfying a later step.
 * A dedicated bounded deadline still turns a lost transition into a failure.
 * ─────────────────────────────────────────────────────────────── */
static int job_resume_background(pty_session *session)
{
    if (job_send(session, "jobs %1 | /bin/cat\r", "send jobs pipeline") ==
            -1 ||
        job_expect(session, "Stopped ", "observe jobs state") == -1 ||
        job_expect(session, "gsh$ ", "observe prompt after jobs") == -1 ||
        job_send(session, "bg %1\r", "resume background job") == -1 ||
        job_observe(session, "[continued ", "observe continued notice") == -1 ||
        job_observe(session, "GSH_PROBE_CONTINUED",
                    "observe resumed probe") == -1 ||
        job_observe(session, "gsh$ ", "observe prompt after bg") == -1) {
        return -1;
    }
    session->capture_length = 0;
    return 0;
}

static int job_foreground_second(pty_session *session, const char *command)
{
    if (job_start_stopped(session, command, "[2] ") == -1 ||
        job_send(session, "fg %2\r", "foreground second job") == -1 ||
        job_expect(session, "GSH_PROBE_CONTINUED",
                   "observe foreground resume") == -1) {
        return -1;
    }
    if (send_bytes(session, "\003", 1U) == -1) {
        return job_flow_failure(session, "interrupt foreground job");
    }
    return job_expect(session, "gsh$ ", "observe prompt after interrupt");
}

static int job_finish_background(pty_session *session)
{
    if (session == NULL) {
        return -1;
    }
    return job_send(
               session,
               "kill %1; wait %1; printf 'GSH_WAIT=%s\\n' \"$?\"\r",
               "terminate background job") == -1 ||
                   job_expect(session, "GSH_WAIT=143",
                              "observe wait status") == -1 ||
                   job_expect(session, "gsh$ ",
                              "observe final prompt") == -1
               ? -1
               : 0;
}

static int job_service_control_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-job-service-XXXXXX";
    char probe[4096];
    char command[4100];
    char *separator;
    pty_session session;
    int failed = 0;

    if (strlen(executable) + 1U > sizeof(probe)) {
        (void)fprintf(stderr, "pty jobs: probe path is too long\n");
        return 1;
    }
    (void)memcpy(probe, executable, strlen(executable) + 1U);
    separator = strrchr(probe, '/');
    if (separator == NULL ||
        (size_t)(separator - probe) + sizeof("/job-probe") > sizeof(probe)) {
        (void)fprintf(stderr, "pty jobs: cannot derive job probe path\n");
        return 1;
    }
    (void)memcpy(separator, "/job-probe", sizeof("/job-probe"));
    if (snprintf(command, sizeof(command), "%s &\r", probe) >=
        (int)sizeof(command)) {
        (void)fprintf(stderr, "pty jobs: probe command is too long\n");
        return 1;
    }

    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty jobs: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (job_expect(&session, "gsh$ ", "observe initial prompt") == -1 ||
        job_start_stopped(&session, command, "[1] ") == -1 ||
        job_resume_background(&session) == -1 ||
        job_foreground_second(&session, command) == -1 ||
        job_finish_background(&session) == -1) {
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty jobs: child process leaked\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) failed = 1;
    (void)rmdir(fixture);
    return failed;
}

static int job_notification_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-job-notify-XXXXXX";
    pty_session session;
    int failed = 0;

    if (executable == NULL || mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty notify: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/sleep .1 &\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(&session, " Done ", 300) != -1 ||
        errno != ETIMEDOUT || send_text(&session, ":\r") == -1 ||
        consume_through(&session, " Done ", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -b\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/sleep .1 &\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, " Done ", TEST_TIMEOUT_MS) == -1) {
        perror("pty notify: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) failed = 1;
    (void)rmdir(fixture);
    return failed;
}

static int interactive_errexit_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-errexit-XXXXXX";
    pty_session session;
    int failed = 0;

    if (executable == NULL || mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty errexit: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -e\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "false\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "printf GSH_ERREXIT_ALIVE\r") == -1 ||
        consume_through(&session, "GSH_ERREXIT_ALIVE",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty errexit: recovery");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) failed = 1;
    (void)rmdir(fixture);
    return failed;
}

static int fc_builtin_flow(const char *executable)
{
    static const char editor_body[] =
        "#!/bin/sh\n"
        "/usr/bin/printf 'echo GSH_FC_EDITED\\n' > \"$1\"\n";
    char fixture[] = "/tmp/gsh-fc-flow-XXXXXX";
    char editor[1024];
    char command[1200];
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        snprintf(editor, sizeof(editor), "%s/editor", fixture) >=
            (int)sizeof(editor) ||
        write_text_file(editor, editor_body, 0700) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1 ||
        snprintf(command, sizeof(command), "FCEDIT='%s' fc echo\r",
                 editor) >= (int)sizeof(command)) {
        perror("pty fc: setup");
        (void)unlink(editor);
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "echo GSH_FC_ORIGINAL\r") == -1 ||
        consume_through(&session, "GSH_FC_ORIGINAL\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "fc -s ORIGINAL=REPLAY echo\r") == -1 ||
        consume_through(&session, "GSH_FC_REPLAY\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "fc -ln echo\r") == -1 ||
        consume_through(&session, "echo GSH_FC_ORIGINAL\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "GSH_FC_EDITED\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "FCEDIT=/no/such/editor fc echo\r") == -1 ||
        consume_through(&session, "gsh: fc: editor failed",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        perror("pty fc: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) failed = 1;
    (void)unlink(editor);
    (void)rmdir(fixture);
    return failed;
}

static int protected_bridge_flow(const char *executable)
{
    static const char *const commands[] = {
        "{ echo bridge; } >/dev/null\r",
        "{ printf bridge; } >/dev/null\r",
        "{ test x = x; } >/dev/null\r",
        "{ [ x = x ]; } >/dev/null\r",
        "{ read value; } </dev/null\r",
        "{ getopts a option; } >/dev/null\r",
        "{ fc -l; } >/dev/null\r",
        "{ jobs; } >/dev/null\r",
        "{ kill -l; } >/dev/null\r",
        "name=echo; { \"$name\" bridge; } >/dev/null\r",
    };
    char fixture[] = "/tmp/gsh-bridge-flow-XXXXXX";
    pty_session session;
    size_t index;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty bridge guard: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
    }
    for (index = 0; !failed &&
                    index < sizeof(commands) / sizeof(commands[0]); index++) {
        if (send_text(&session, commands[index]) == -1 ||
            consume_through(
                &session,
                "native builtin ownership prevents compatibility fallback",
                TEST_TIMEOUT_MS) == -1 ||
            consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
            failed = 1;
        }
    }
    if (!failed &&
        (send_text(&session, "rt\r") == -1 ||
         consume_through(&session, "protected_bridge=0",
                         TEST_TIMEOUT_MS) == -1 ||
         process_child_count(session.pid) != 1)) {
        failed = 1;
    }
    if (failed) {
        perror("pty bridge guard: flow");
        dump_capture(&session);
    }
    if (stop_session(&session) == -1) failed = 1;
    (void)rmdir(fixture);
    return failed;
}

static int managed_repl_concurrency(pty_session *session)
{
    uint64_t start;

    if (send_text(session, "/bin/sleep 1\r") == -1 ||
        consume_through(session,
                        "/bin/sleep 1\r\ngsh* ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    start = monotonic_ns();
    if (send_text(session, "/usr/bin/printf FAST\r") == -1 ||
        consume_through(session, "/usr/bin/printf FAST\r\nFAST",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    if (monotonic_ns() - start >= 800000000ULL) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int managed_repl_prompt_state(pty_session *session)
{
    if (session == NULL) {
        return -1;
    }
    static const char command[] =
        "/bin/sh -c 'sleep .3; printf PROMPT_STATE_DONE'\r";

    session->capture_length = 0;
    if (send_text(session, command) == -1 ||
        wait_for_output(session, "gsh* ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "EDIT_PRESERVED") == -1 ||
        wait_for_output(session,
                        "gsh* EDIT_PRESERVED",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        wait_for_output(session,
                        "PROMPT_STATE_DONE\r\ngsh* ",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session,
                        "PROMPT_STATE_DONE\r\ngsh$ ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_terminal_outcomes(pty_session *session)
{
    if (session == NULL) {
        return -1;
    }
    session->capture_length = 0;
    if (send_text(session, "/bin/sh -c 'sleep .2; exit 7'\r") == -1 ||
        wait_for_output(session, "gsh* ",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "gsh$ ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    session->capture_length = 0;
    if (send_text(session,
                  "/bin/sh -c 'sleep .2; kill -TERM $$'\r") == -1 ||
        wait_for_output(session, "gsh* ",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "gsh$ ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_compound_overtake(pty_session *session)
{
    static const char loop[] =
        "for GSH_ASYNC_I in 1; do /bin/sleep 1; "
        "echo \"TICK $GSH_ASYNC_I\"; done\r";
    uint64_t start;

    if (send_text(session, loop) == -1 ||
        consume_through(session,
                        "done\r\ngsh* ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    start = monotonic_ns();
    if (send_text(session, "/usr/bin/printf COMPOUND_FAST\r") == -1 ||
        consume_through(session,
                        "/usr/bin/printf COMPOUND_FAST\r\nCOMPOUND_FAST",
                        TEST_TIMEOUT_MS) == -1 ||
        monotonic_ns() - start >= 800000000ULL ||
        consume_through(session, "TICK 1", TEST_TIMEOUT_MS) == -1) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int managed_repl_launch_state_fence(pty_session *session)
{
    static const char loop[] =
        "for GSH_ASYNC_J in 1; do /bin/sleep 1; cd .; done\r";
    uint64_t start;

    if (send_text(session, loop) == -1 ||
        consume_through(session,
                        "done\r\ngsh* ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    start = monotonic_ns();
    if (send_text(session, "/bin/pwd\r") == -1 ||
        consume_through(session, "gsh-pty-managed-", TEST_TIMEOUT_MS) == -1 ||
        monotonic_ns() - start < 700000000ULL) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int managed_repl_native_resources(pty_session *session)
{
    static const char styled[] =
        "\033[4;38;5;81mname with space.py\033[0m";
    if (session == NULL) return -1;
    session->capture_length = 0U;
    if (send_text(session, "ls -1\r") == -1 ||
        wait_for_output(session, styled, TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) return -1;
    return 0;
}

typedef struct {
    char directory[PATH_MAX];
    char first[PATH_MAX];
    char second[PATH_MAX];
    char tools[PATH_MAX];
    char editor[PATH_MAX];
    pty_session session;
} resource_action_fixture;

static void cleanup_resource_action_fixture(resource_action_fixture *fixture)
{
    if (fixture == NULL) return;
    if (fixture->first[0] != '\0') (void)unlink(fixture->first);
    if (fixture->second[0] != '\0') (void)unlink(fixture->second);
    if (fixture->editor[0] != '\0') (void)unlink(fixture->editor);
    if (fixture->tools[0] != '\0') (void)rmdir(fixture->tools);
    if (fixture->directory[0] != '\0') (void)rmdir(fixture->directory);
}

static int setup_resource_action_fixture(resource_action_fixture *fixture,
                                         const char *executable)
{
    char test_path[PATH_MAX * 2U];
    if (fixture == NULL || executable == NULL) return -1;
    (void)memset(fixture, 0, sizeof(*fixture));
    (void)snprintf(fixture->directory, sizeof(fixture->directory),
                   "/tmp/gsh-resource-action-XXXXXX");
    if (mkdtemp(fixture->directory) == NULL ||
        snprintf(fixture->first, sizeof(fixture->first), "%s/%s",
                 fixture->directory, "name with space.py") >=
            (int)sizeof(fixture->first) ||
        write_text_file(fixture->first, "print('resource')\n", 0600) == -1 ||
        snprintf(fixture->second, sizeof(fixture->second), "%s/%s",
                 fixture->directory, "second.py") >=
            (int)sizeof(fixture->second) ||
        write_text_file(fixture->second, "SECOND_PREVIEW = True\n", 0600) == -1 ||
        snprintf(fixture->tools, sizeof(fixture->tools), "%s/.tools",
                 fixture->directory) >= (int)sizeof(fixture->tools) ||
        mkdir(fixture->tools, 0700) == -1 ||
        snprintf(fixture->editor, sizeof(fixture->editor), "%s/nvim",
                 fixture->tools) >= (int)sizeof(fixture->editor) ||
        write_text_file(fixture->editor,
                        "#!/bin/sh\nprintf 'FAKE_EDITOR_FRAME:%s\\n' \"$*\"\n",
                        0700) == -1 ||
        snprintf(test_path, sizeof(test_path), "%s:/usr/bin:/bin",
                 fixture->tools) >= (int)sizeof(test_path) ||
        setenv("GSH_HARNESS_PATH", test_path, 1) == -1 ||
        start_managed_session(&fixture->session, executable,
                              fixture->directory) == -1) {
        (void)unsetenv("GSH_HARNESS_PATH");
        return -1;
    }
    (void)unsetenv("GSH_HARNESS_PATH");
    return 0;
}

static int open_and_replace_resource_preview(pty_session *session, int *stage)
{
    static const char styled[] =
        "\033[4;38;5;81mname with space.py\033[0m";
    static const char click[] = "\033[<0;2;2M";
    static const char replacement_click[] = "\033[<0;2;3M";
    static const char split_origin[] = "\033[1;50H";
    if (session == NULL || stage == NULL) return -1;
    if (resize_session(session, 24U, 110U) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "ls -1\r") == -1 ||
        wait_for_output(session, styled, TEST_TIMEOUT_MS) == -1) return -1;
    *stage = 1;
    if (send_text(session, click) == -1 ||
        wait_for_output(session, split_origin, TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "Esc: panel", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "print", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "\033[38;5;114m", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "-rw-------", TEST_TIMEOUT_MS) == -1)
        return -1;
    *stage = 6;
    session->capture_length = 0U;
    if (send_text(session, replacement_click) == -1 ||
        wait_for_output(session, "SECOND_PREVIEW", TEST_TIMEOUT_MS) == -1)
        return -1;
    *stage = 7;
    return 0;
}

static int exercise_resource_editor(pty_session *session, int *stage)
{
    static const char scrollback_purge[] = "\033[H\033[2J\033[3J";
    if (session == NULL || stage == NULL) return -1;
    (void)poll(NULL, 0U, 100);
    session->capture_length = 0U;
    if (send_text(session, "e") == -1 ||
        consume_through(session, "FAKE_EDITOR_FRAME", TEST_TIMEOUT_MS) == -1)
        return -1;
    *stage = 71;
    if (wait_for_output(session, "+1 -c set number norelativenumber --",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, scrollback_purge, TEST_TIMEOUT_MS) == -1)
        return -1;
    *stage = 72;
    if (wait_for_output(session, "SECOND_PREVIEW", TEST_TIMEOUT_MS) == -1)
        return -1;
    *stage = 8;
    return 0;
}

static int exercise_resource_focus(pty_session *session, int *stage)
{
    static const char right_panel_click[] = "\033[<0;80;2M";
    static const char split_origin[] = "\033[1;50H";
    if (session == NULL || stage == NULL) return -1;
    session->capture_length = 0U;
    if (send_text(session, "\033") == -1 ||
        wait_for_output(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) return -1;
    *stage = 9;
    if (send_text(session, "q") == -1 ||
        wait_for_output(session, "gsh$ q", TEST_TIMEOUT_MS) == -1) return -1;
    *stage = 10;
    if (send_bytes(session, "\025", 1U) == -1) return -1;
    *stage = 11;
    (void)poll(NULL, 0U, 40);
    session->capture_length = 0U;
    if (send_text(session, right_panel_click) == -1 ||
        wait_for_output(session, split_origin, TEST_TIMEOUT_MS) == -1)
        return -1;
    *stage = 12;
    if (send_text(session, "q") == -1 ||
        wait_for_output(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) return -1;
    *stage = 13;
    return 0;
}

static int managed_resource_action_flow(const char *executable)
{
    resource_action_fixture fixture = {0};
    int failed;
    int stage = 0;
    if (setup_resource_action_fixture(&fixture, executable) == -1) {
        perror("pty resource action: setup");
        cleanup_resource_action_fixture(&fixture);
        return 1;
    }
    failed = open_and_replace_resource_preview(&fixture.session, &stage) == -1 ||
             exercise_resource_editor(&fixture.session, &stage) == -1 ||
             exercise_resource_focus(&fixture.session, &stage) == -1;
    (void)poll(NULL, 0U, 100);
    if (failed) {
        (void)fprintf(stderr, "pty resource action: stage %d: %s\n", stage,
                      strerror(errno));
        dump_capture(&fixture.session);
    }
    if (stop_session(&fixture.session) == -1) failed = 1;
    cleanup_resource_action_fixture(&fixture);
    return failed;
}

static int managed_directory_action_flow(const char *executable)
{
    static const char styled[] = "\033[4;38;5;75mchild\033[0m";
    static const char back_styled[] =
        "\033[4;38;5;75m<-   \033[0m";
    static const char nested_styled[] =
        "\033[4;38;5;75mperformance\033[0m";
    static const char nested_back_styled[] =
        "\033[4;38;5;75m<-  \033[0m";
    static const char first_click[] = "\033[<0;2;3M";
    static const char second_click[] = "\033[<0;2;6M";
    static const char back_click[] = "\033[<0;2;8M";
    char fixture[] = "/tmp/gsh-directory-action-XXXXXX";
    char child[PATH_MAX] = {0};
    char nested[PATH_MAX] = {0};
    char canonical[PATH_MAX];
    char committed[PATH_MAX] = {0};
    char probe[PATH_MAX * 2U + 256U];
    pty_session session;
    int failed = 0;

    if (executable == NULL || mkdtemp(fixture) == NULL ||
        snprintf(child, sizeof(child), "%s/child", fixture) >=
            (int)sizeof(child) ||
        mkdir(child, 0700) == -1 ||
        snprintf(nested, sizeof(nested), "%s/performance", child) >=
            (int)sizeof(nested) ||
        mkdir(nested, 0700) == -1 ||
        realpath(fixture, canonical) == NULL ||
        snprintf(committed, sizeof(committed), "%s/committed", child) >=
            (int)sizeof(committed) ||
        snprintf(probe, sizeof(probe),
                 "status=$?; /bin/test \"$status\" -eq 0 && "
                 "/bin/test \"$PWD\" = '%s/child' && "
                 "/bin/test \"$OLDPWD\" = '%s/child/performance' && "
                 "/usr/bin/touch committed && "
                 "/usr/bin/printf '%%s%%s\\n' GSH_DIRECTORY_ACTION_ OK\r",
                 canonical, canonical) >= (int)sizeof(probe) ||
        start_managed_session(&session, executable, fixture) == -1) {
        perror("pty directory action: setup");
        if (nested[0] != '\0') (void)rmdir(nested);
        if (child[0] != '\0') (void)rmdir(child);
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "ll\r") == -1 ||
        wait_for_output(&session, back_styled, TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(&session, styled, TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, first_click) == -1 ||
        wait_for_output(&session, nested_styled, TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, second_click) == -1 ||
        wait_for_output(&session, "/child/performance",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(&session, nested_back_styled,
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, back_click) == -1 ||
        wait_for_output(&session, "/child' && ll",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, probe) == -1 ||
        wait_for_output(&session, "GSH_DIRECTORY_ACTION_OK",
                        TEST_TIMEOUT_MS) == -1) {
        perror("pty directory action: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1 || access(committed, F_OK) == -1)
        failed = 1;
    (void)unlink(committed);
    (void)rmdir(nested);
    (void)rmdir(child);
    (void)rmdir(fixture);
    return failed;
}

static int managed_scroll_flow(const char *executable)
{
    static const char output[] =
        "/usr/bin/printf 'SCROLL_01\\nSCROLL_02\\nSCROLL_03\\n"
        "SCROLL_04\\nSCROLL_05\\nSCROLL_06\\nSCROLL_07\\n"
        "SCROLL_08\\nSCROLL_09\\nSCROLL_10\\n'\r";
    static const char wheel_up[] = "\033[<64;1;1M";
    static const char wheel_down[] = "\033[<65;1;1M";
    char fixture[] = "/tmp/gsh-scroll-action-XXXXXX";
    pty_session session;
    int failed = 0;

    if (executable == NULL || mkdtemp(fixture) == NULL ||
        start_managed_session(&session, executable, fixture) == -1) {
        perror("pty managed scroll: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (resize_session(&session, 6U, 80U) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, output) == -1 ||
        wait_for_output(&session, "SCROLL_10", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
    }
    session.capture_length = 0U;
    if (!failed &&
        (send_text(&session, wheel_up) == -1 ||
         wait_for_output(&session, "SCROLL_03", TEST_TIMEOUT_MS) == -1 ||
         capture_contains(&session, "SCROLL_10"))) failed = 1;
    session.capture_length = 0U;
    if (!failed &&
        (send_text(&session, wheel_down) == -1 ||
         wait_for_output(&session, "SCROLL_10", TEST_TIMEOUT_MS) == -1))
        failed = 1;
    if (failed) {
        perror("pty managed scroll: flow");
        dump_capture(&session);
    }
    if (stop_session(&session) == -1) failed = 1;
    (void)rmdir(fixture);
    return failed;
}

static int managed_detected_action_flow(const char *executable)
{
    static const char styled[] =
        "\033[4;38;5;81m./detected.py\033[0m:2:3";
    static const char click[] = "\033[<0;2;2M";
    char fixture[] = "/tmp/gsh-detected-action-XXXXXX";
    char path[PATH_MAX] = {0};
    pty_session session;
    int failed = 0;

    if (executable == NULL || mkdtemp(fixture) == NULL ||
        snprintf(path, sizeof(path), "%s/detected.py", fixture) >=
            (int)sizeof(path) ||
        write_text_file(path, "first\nprint('detected')", 0600) == -1 ||
        start_managed_session(&session, executable, fixture) == -1) {
        perror("pty detected action: setup");
        if (path[0] != '\0') (void)unlink(path);
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf './detected.py:2:3\\n'\r") == -1 ||
        wait_for_output(&session, styled, TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, click) == -1 ||
        wait_for_output(&session, "Esc: switch panel", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(&session, "print", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(&session, "2/2", TEST_TIMEOUT_MS) == -1) {
        perror("pty detected action: flow");
        dump_capture(&session);
        failed = 1;
    }
    (void)poll(NULL, 0U, 100);
    session.capture_length = 0U;
    if (!failed &&
        (send_text(&session, "q") == -1 ||
         wait_for_output(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1)) {
        perror("pty detected action: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) failed = 1;
    (void)unlink(path);
    (void)rmdir(fixture);
    return failed;
}

static int exercise_markdown_preview(pty_session *session)
{
    static const char frame[] =
        "MultipartFile=name=Z3NoLXByZXZpZXcucG5n;";
    static const char wheel_down[] = "\033[<65;80;2M";

    if (session == NULL) return -1;
    if (resize_session(session, 24U, 110U) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "view -- document.md\r") == -1 ||
        wait_for_output(session, "\033[1;38;5;81m>\033[0m ",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "Preview title", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "(1)/(2)", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "Name", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "image preview unavailable",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, frame,
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "FileEnd\a\033" "8",
                        TEST_TIMEOUT_MS) == -1 ||
        !capture_ordered(session, "image preview unavailable", frame))
        return -1;
    session->capture_length = 0U;
    if (send_text(session, wheel_down) == -1 ||
        wait_for_output(session, "4/", TEST_TIMEOUT_MS) == -1 ||
        capture_contains(session, "jjj") ||
        send_text(session, "q") == -1 ||
        consume_through(session, "\033[?1006l", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) return -1;
    (void)poll(NULL, 0U, 100);
    return 0;
}

static int exercise_generic_pdf_viewer(pty_session *session)
{
    if (session == NULL ||
        send_text(session, "view -- document.pdf\r") == -1 ||
        wait_for_output(session, "hex 1/1", TEST_TIMEOUT_MS) == -1 ||
        capture_contains(session, "PDF · page") ||
        send_text(session, "q") == -1 ||
        consume_through(session, "\033[?1006l", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) return -1;
    (void)poll(NULL, 0U, 100);
    return 0;
}

static int managed_markdown_preview_flow(const char *executable)
{
    static const char markdown[] =
        "# Preview title\n\nEquation: $\\alpha + \\sqrt{x}$\n\n"
        "$$\\frac{1}{2} \\le 1$$\n\n"
        "| Name | Value |\n| --- | ---: |\n| answer | 42 |\n\n"
        "```python\ndef answer():\n    return 42\n```\n\n"
        "![diagram](image.png)\n![missing](missing.png)\n";
    static const unsigned char png[] = {
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
        0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
        0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
        0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x1fU, 0x15U, 0xc4U,
        0x89U, 0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x44U, 0x41U,
        0x54U, 0x08U, 0xd7U, 0x63U, 0xf8U, 0xcfU, 0xc0U, 0xf0U,
        0x1fU, 0x00U, 0x05U, 0x00U, 0x01U, 0xffU, 0x89U, 0x99U,
        0x3dU, 0x1dU, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U,
        0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U,
    };
    static const unsigned char pdf[] = {
        '%', 'P', 'D', 'F', '-', '1', '.', '7', '\n', 0U,
    };
    char fixture[] = "/tmp/gsh-markdown-preview-XXXXXX";
    char markdown_path[PATH_MAX] = {0};
    char pdf_path[PATH_MAX] = {0};
    char image_path[PATH_MAX] = {0};
    pty_session session = {0};
    int failed = 0;

    if (executable == NULL || mkdtemp(fixture) == NULL ||
        snprintf(markdown_path, sizeof(markdown_path), "%s/document.md",
                 fixture) >= (int)sizeof(markdown_path) ||
        snprintf(pdf_path, sizeof(pdf_path), "%s/document.pdf", fixture) >=
            (int)sizeof(pdf_path) ||
        snprintf(image_path, sizeof(image_path), "%s/image.png", fixture) >=
            (int)sizeof(image_path) ||
        write_text_file(markdown_path, markdown, 0600) == -1 ||
        write_binary_file(pdf_path, pdf, sizeof(pdf), 0600) == -1 ||
        write_binary_file(image_path, png, sizeof(png), 0600) == -1 ||
        setenv("GSH_HARNESS_TERM_FEATURES", "F", 1) == -1 ||
        start_managed_session(&session, executable, fixture) == -1) {
        perror("pty markdown preview: setup");
        failed = 1;
    }
    (void)unsetenv("GSH_HARNESS_TERM_FEATURES");
    if (!failed && exercise_markdown_preview(&session) == -1) {
        (void)fputs("pty markdown preview: flow failed\n", stderr);
        failed = 1;
    }
    if (!failed && exercise_generic_pdf_viewer(&session) == -1) {
        (void)fputs("pty generic PDF viewer: flow failed\n", stderr);
        failed = 1;
    }
    if (failed && session.pid > 0) {
        perror("pty markdown preview: flow");
        dump_capture(&session);
    }
    if (session.pid > 0 && stop_session(&session) == -1) failed = 1;
    if (markdown_path[0] != '\0') (void)unlink(markdown_path);
    if (pdf_path[0] != '\0') (void)unlink(pdf_path);
    if (image_path[0] != '\0') (void)unlink(image_path);
    (void)rmdir(fixture);
    return failed;
}

static int managed_image_probe_flow(const char *executable)
{
    static const char configuration[] =
        "config.version = 1\n"
        "shell.history.enabled = false\n"
        "terminal.images = on\n";
    static const char markdown[] = "![probe](probe.png)\n";
    static const unsigned char png[] = {
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU,
        0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U,
        0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U,
    };
    static const char kitty_reply[] = "\033_Gi=31;OK\033\\";
    char fixture[] = "/tmp/gsh-image-probe-XXXXXX";
    char config_path[PATH_MAX] = {0};
    char markdown_path[PATH_MAX] = {0};
    char image_path[PATH_MAX] = {0};
    pty_session session = {0};
    int failed = 0;

    if (executable == NULL || mkdtemp(fixture) == NULL ||
        snprintf(config_path, sizeof(config_path), "%s/.gshrc", fixture) >=
            (int)sizeof(config_path) ||
        snprintf(markdown_path, sizeof(markdown_path), "%s/probe.md", fixture) >=
            (int)sizeof(markdown_path) ||
        snprintf(image_path, sizeof(image_path), "%s/probe.png", fixture) >=
            (int)sizeof(image_path) ||
        write_text_file(config_path, configuration, 0600) == -1 ||
        write_text_file(markdown_path, markdown, 0600) == -1 ||
        write_binary_file(image_path, png, sizeof(png), 0600) == -1 ||
        setenv("GSH_HARNESS_HOME", fixture, 1) == -1 ||
        setenv("GSH_HARNESS_HISTORY", "1", 1) == -1 ||
        start_managed_session(&session, executable, fixture) == -1) {
        failed = 1;
    }
    (void)unsetenv("GSH_HARNESS_HOME");
    (void)unsetenv("GSH_HARNESS_HISTORY");
    if (!failed &&
        (wait_for_output(&session, "\033_Gi=31", TEST_TIMEOUT_MS) == -1 ||
         send_text(&session, kitty_reply) == -1 ||
         consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
         send_text(&session, "view -- probe.md\r") == -1 ||
         wait_for_output(&session, "\033_Ga=T,f=100", TEST_TIMEOUT_MS) == -1 ||
         send_text(&session, "q") == -1 ||
         consume_through(&session, "\033[?1006l", TEST_TIMEOUT_MS) == -1 ||
         consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1))
        failed = 1;
    if (failed && session.pid > 0) dump_capture(&session);
    if (session.pid > 0 && stop_session(&session) == -1) failed = 1;
    (void)unlink(image_path);
    (void)unlink(markdown_path);
    (void)unlink(config_path);
    (void)rmdir(fixture);
    return failed;
}

static int managed_repl_preserves_edit(pty_session *session)
{
    if (send_text(session,
                  "/bin/sh -c 'sleep 0.2; printf LATE'\r") == -1 ||
        consume_through(session,
                        "printf LATE'\r\ngsh* ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "PRESERVED") == -1 ||
        consume_through(session, "LATE", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ PRESERVED", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_focus(pty_session *session)
{
    if (send_text(session, "/usr/bin/seq 1 6\r") == -1 ||
        consume_through(session, "\r\n1\r\n2\r\n3\r\n4\r\n5\r\n6",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/bin/cat\r") == -1 ||
        consume_through(session,
                        "/bin/cat\r\ngsh* ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "fg\r") == -1 ||
        consume_through(session, "[focused cell ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "BEFORE_STOP\r") == -1 ||
        consume_through(session, "BEFORE_STOP",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\032", 1) == -1 ||
        consume_through(session, "[stopped]", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh* ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "bg\r") == -1 ||
        consume_through(session, "[continued]",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "fg\r") == -1 ||
        consume_through(session, "[focused cell ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "AFTER_CONTINUE\r") == -1 ||
        consume_through(session, "AFTER_CONTINUE",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\004\035", 2) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_private_input_autofocus(pty_session *session)
{
    if (session == NULL) {
        return -1;
    }
    static const char command[] =
        "/bin/sh -c 'sleep .2; stty -echo; echo PRIVATE_INPUT; read x; "
        "stty echo; echo PRIVATE_ACCEPTED'\r";
    static const char secret[] = "PRIVATE_SECRET_42\r";

    session->capture_length = 0;
    if (send_text(session, command) == -1 ||
        consume_through(session,
                        "\r\ngsh* ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "PRESERVED") == -1 ||
        wait_for_output(session, "\r\nPRIVATE_INPUT\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session,
                        "gsh* PRESERVED",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, secret) == -1 ||
        wait_for_output(session, "PRIVATE_ACCEPTED", TEST_TIMEOUT_MS) == -1 ||
        capture_contains(session, "PRIVATE_SECRET_42") ||
        consume_through(session, "PRIVATE_ACCEPTED", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session,
                        "\ngsh$ PRESERVED",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        send_text(session, "/usr/bin/printf FOCUS_RETURNED\r") == -1 ||
        consume_through(session, "FOCUS_RETURNED", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_fullscreen_focus(pty_session *session)
{
    if (session == NULL) {
        return -1;
    }
    static const char command[] =
        "/bin/sh -c 'trap \"\" WINCH; saved=$(/bin/stty -g); "
        "/bin/stty -echo -icanon min 1 time 0; "
        "/usr/bin/printf \"\\033[?1049h\\033[31mUTF8=\\342\"; "
        "/bin/sleep 0.05; "
        "/usr/bin/printf \"\\225\\255\\342\\224\\200\\342\\225\\256 "
        "\\342\\200\\242 FULLSCREEN_READY\"; "
        "/bin/dd of=/dev/null bs=1 count=1 2>/dev/null; "
        "/bin/stty \"$saved\"; "
        "/usr/bin/printf \"\\033[0mFULLSCREEN_EXITED\\033[?1049l\"'\r";

    session->capture_length = 0;
    if (send_text(session, command) == -1 ||
        wait_for_output(session, "\033[31mUTF8=╭─╮ • FULLSCREEN_READY",
                        TEST_TIMEOUT_MS) == -1 ||
        capture_contains(session, "\033[?1049h") ||
        send_bytes(session, "\035", 1) == -1 ||
        wait_for_output(session, "[full-screen session]",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "DETACHED_EDITOR") == -1 ||
        wait_for_output(session, "gsh* DETACHED_EDITOR",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        send_text(session, "fg\r") == -1 ||
        wait_for_output(session, "[focused cell ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "q") == -1 ||
        consume_through(session, "FULLSCREEN_EXITED",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "gsh$ ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/usr/bin/printf AFTER_FULLSCREEN\r") == -1 ||
        wait_for_output(session, "AFTER_FULLSCREEN", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_pipeline(pty_session *session)
{
    uint64_t start;

    if (send_text(session,
                  "/bin/sh -c 'sleep 1; printf PIPE_SLOW' | /bin/cat\r") ==
            -1 ||
        consume_through(session,
                        "| /bin/cat\r\ngsh* ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    start = monotonic_ns();
    if (send_text(session, "/usr/bin/printf PIPE_FAST\r") == -1 ||
        consume_through(session, "/usr/bin/printf PIPE_FAST\r\nPIPE_FAST",
                        TEST_TIMEOUT_MS) == -1 ||
        monotonic_ns() - start >= 800000000ULL ||
        consume_through(session, "PIPE_SLOW", TEST_TIMEOUT_MS) == -1) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int managed_repl_ordering(pty_session *session)
{
    if (send_text(session,
                  "/bin/sh -c 'sleep 0.3; printf ORDER_FIRST'\r") == -1 ||
        send_text(session, "cd /\r") == -1 ||
        send_text(session,
                  "/usr/bin/printf 'ORDER_STATE=%s' \"$PWD\"\r") == -1 ||
        consume_through(session, "\r\nORDER_FIRST", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(session, "\r\nORDER_STATE=",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "/",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/usr/bin/false\r") == -1 ||
        send_text(session,
                  "/usr/bin/printf 'ORDER_STATUS=%s' \"$?\"\r") == -1 ||
        consume_through(session, "\r\nORDER_STATUS=1",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_contains_output(pty_session *session)
{
    if (send_text(session,
                  "/usr/bin/printf '\033[2JFORGED\033[H'\r") == -1 ||
        consume_through(session, "\r\nFORGED", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_rewrites_progress(pty_session *session)
{
    if (send_text(session,
                  "/bin/sh -c 'for n in 10 20 DONE; do "
                  "printf \"GSH_PROGRESS_%s\\r\" \"$n\"; "
                  "sleep 0.05; done; printf \"\\n\"'\r") == -1 ||
        wait_for_output(session, "GSH_PROGRESS_DONE",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    session->capture_length = 0;
    if (send_text(session, "X") == -1 ||
        wait_for_output(session, "gsh$ X", TEST_TIMEOUT_MS) == -1 ||
        !capture_contains(session, "GSH_PROGRESS_DONE") ||
        capture_contains(session, "GSH_PROGRESS_10") ||
        capture_contains(session, "GSH_PROGRESS_20") ||
        send_bytes(session, "\025", 1) == -1) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int managed_repl_resize(pty_session *session)
{
    if (send_text(session, "RESIZE_KEEP") == -1 ||
        consume_through(session, "gsh$ RESIZE_KEEP",
                        TEST_TIMEOUT_MS) == -1 ||
        resize_session(session, 12, 40) == -1 ||
        consume_through(session, "gsh$ RESIZE_KEEP",
                        TEST_TIMEOUT_MS) == -1 ||
        resize_session(session, 24, 80) == -1 ||
        consume_through(session, "gsh$ RESIZE_KEEP",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_saturation(pty_session *session)
{
    unsigned int submission;

    for (submission = 0; submission < 15U; submission++) {
        if (send_text(session, "/bin/sleep 30\r") == -1 ||
            consume_through(session, "/bin/sleep 30\r\n",
                            TEST_TIMEOUT_MS) == -1) {
            return -1;
        }
    }
    if (send_text(session, "/bin/sleep 30\r") == -1 ||
        consume_through(session, "\a", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh* /bin/sleep 30",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_toggle(pty_session *session)
{
    if (session == NULL) {
        return -1;
    }
    uint64_t start;

    session->capture_length = 0;
    if (send_text(session, "/bin/sleep 2\r") == -1 ||
        wait_for_output(session, "gsh* ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/async\r") == -1 ||
        wait_for_output(session, "async repl: off pending",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    start = monotonic_ns();
    if (send_text(session, "/usr/bin/printf TOGGLE_FAST\r") == -1 ||
        wait_for_output(session, "TOGGLE_FAST", TEST_TIMEOUT_MS) == -1 ||
        monotonic_ns() - start >= 800000000ULL ||
        send_text(session, "/async\r") == -1 ||
        wait_for_output(session,
                        "async repl: transition cancelled; remains on",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/async\r") == -1 ||
        wait_for_output(session, "async repl: off pending",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "\033[?1049lasync repl: off",
                        TEST_TIMEOUT_MS) == -1) {
        errno = ETIMEDOUT;
        return -1;
    }
    session->capture_length = 0;
    if (send_text(session, "/async\r") == -1 ||
        wait_for_output(session, "\033[?1049h", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "async repl: on",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "gsh$ ",
                        TEST_TIMEOUT_MS) == -1 ||
        capture_contains(session, "async repl: on pending")) {
        errno = ETIMEDOUT;
        return -1;
    }
    session->capture_length = 0;
    if (send_text(session, "/async\r") == -1 ||
        wait_for_output(session, "\033[?1049lasync repl: off",
                        TEST_TIMEOUT_MS) == -1 ||
        capture_contains(session, "async repl: off pending")) {
        return -1;
    }
    session->capture_length = 0;
    if (send_text(session, "/async\r") == -1 ||
        wait_for_output(session, "\033[?1049h", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "async repl: on", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "gsh$ ",
                        TEST_TIMEOUT_MS) == -1 ||
        capture_contains(session, "async repl: on pending")) {
        return -1;
    }
    session->capture_length = 0;
    if (send_text(session, "/async\r") == -1 ||
        wait_for_output(session, "\033[?1049lasync repl: off",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/bin/sleep 1 &\r") == -1 ||
        wait_for_output(session, "] ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/async\r") == -1 ||
        wait_for_output(session, "async repl: on pending",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "\033[?1049h", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "async repl: on", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "gsh$ ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int managed_async_repl_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-managed-XXXXXX";
    char resource_path[PATH_MAX];
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        snprintf(resource_path, sizeof(resource_path), "%s/%s", fixture,
                 "name with space.py") >= (int)sizeof(resource_path) ||
        write_text_file(resource_path, "print('resource')\n", 0600) == -1 ||
        start_managed_session(&session, executable, fixture) == -1) {
        perror("pty managed: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ",
                        TEST_TIMEOUT_MS) == -1 ||
        managed_repl_native_resources(&session) == -1 ||
        managed_repl_prompt_state(&session) == -1 ||
        managed_repl_terminal_outcomes(&session) == -1 ||
        managed_repl_concurrency(&session) == -1 ||
        managed_repl_compound_overtake(&session) == -1 ||
        managed_repl_launch_state_fence(&session) == -1 ||
        managed_repl_preserves_edit(&session) == -1 ||
        managed_repl_private_input_autofocus(&session) == -1 ||
        managed_repl_fullscreen_focus(&session) == -1 ||
        managed_repl_focus(&session) == -1 ||
        managed_repl_pipeline(&session) == -1 ||
        managed_repl_ordering(&session) == -1 ||
        managed_repl_contains_output(&session) == -1 ||
        managed_repl_rewrites_progress(&session) == -1 ||
        managed_repl_resize(&session) == -1 ||
        managed_repl_toggle(&session) == -1 ||
        managed_repl_saturation(&session) == -1) {
        perror("pty managed: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        (void)fprintf(stderr, "pty managed: shell did not exit cleanly\n");
        failed = 1;
    }
    {
        char config_path[PATH_MAX];

        if (snprintf(config_path, sizeof(config_path), "%s/.gshrc", fixture) >=
                (int)sizeof(config_path) ||
            access(config_path, F_OK) == 0 || errno != ENOENT) {
            (void)fprintf(stderr, "pty managed: /async modified configuration\n");
            failed = 1;
        }
    }
    (void)unlink(resource_path);
    (void)rmdir(fixture);
    return failed;
}

static int variable_builtin_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-variables-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty variables: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "export GSH_PTY_EXPORT='alpha beta'\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printenv GSH_PTY_EXPORT\r") == -1 ||
        consume_through(&session, "\nalpha beta\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_PTY_COMPOUND=committed export -p >/dev/null; :\r") ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PTY_COMPOUND\"\r") ==
            -1 ||
        consume_through(&session, "<committed>\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "readonly GSH_PTY_READONLY=locked\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unset GSH_PTY_READONLY\r") == -1 ||
        consume_through(&session, "gsh: unset: variable is readonly",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PTY_READONLY\"\r") ==
            -1 ||
        consume_through(&session, "<locked>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unset GSH_PTY_EXPORT\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"${GSH_PTY_EXPORT+set}\"\r") == -1 ||
        consume_through(&session, "<>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "export GSH_PTY_PIPE=before; "
                  "export GSH_PTY_PIPE=inside | true\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PTY_PIPE\"\r") == -1 ||
        consume_through(&session, "<before>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "  for GSH_PTY_FOR in first last; do :; done  \r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PTY_FOR\"\r") == -1 ||
        consume_through(&session, "<last>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "for GSH_PTY_STATUS in one two; do false; done\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' \"$?\"\r") == -1 ||
        consume_through(&session, "<1>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "for GSH_PTY_QUOTED in 'a b' ''; do :; done\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PTY_QUOTED\"\r") ==
            -1 ||
        consume_through(&session, "<>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- 'one two' '' three\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$#|$1|$2|$3\"\r") == -1 ||
        consume_through(&session, "<3|one two||three>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "shift\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$#|$1|$2\"\r") == -1 ||
        consume_through(&session, "<2||three>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- compound; :\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$#|$1\"\r") == -1 ||
        consume_through(&session, "<1|compound>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- redirected >/dev/null\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$1\"\r") == -1 ||
        consume_through(&session, "<redirected>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- inside | true\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$1\"\r") == -1 ||
        consume_through(&session, "<redirected>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -f\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' \"$-\"\r") ==
            -1 ||
        consume_through(&session, "<fi>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "if true; then set -C; fi\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' \"$-\"\r") ==
            -1 ||
        consume_through(&session, "<Cfi>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +Cf\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -f | true\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' \"$-\"\r") ==
            -1 ||
        consume_through(&session, "<i>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unset GSH_PTY_AUTO; set -a\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "GSH_PTY_AUTO=value\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +a\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printenv GSH_PTY_AUTO\r") == -1 ||
        consume_through(&session, "\nvalue\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unset GSH_PTY_NOUNSET; set -u\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, ": \"$GSH_PTY_NOUNSET\"\r") == -1 ||
        consume_through(&session,
                        "GSH_PTY_NOUNSET: parameter null or not set",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' \"$-\"\r") ==
            -1 ||
        consume_through(&session, "<iu>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +u\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -o ignoreeof\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\004", 1) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +o ignoreeof\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "GSH_PTY_ARITH=1\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  ": \"$((GSH_PTY_ARITH=2, 1 / 0))\"\r") == -1 ||
        consume_through(&session,
                        "gsh: arithmetic expansion: division by zero",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"$?|$GSH_PTY_ARITH\"\r") == -1 ||
        consume_through(&session, "<1|1>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "if true; then cd ..; fi\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/test \"$PWD\" = \"$(/bin/pwd)\" && "
                  "/usr/bin/printf GSH_CD_COMMITTED\r") == -1 ||
        consume_through(&session, "GSH_CD_COMMITTED",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- before\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "set -auf -- after; /usr/bin/printf 'GSH_SET_%s' READY; "
                  "/bin/sleep 5\r") == -1 ||
        consume_through(&session, "GSH_SET_READY", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$1|$-\"\r") == -1 ||
        consume_through(&session, "<before|i>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty variables: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty variables: command left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        (void)fprintf(stderr, "pty variables: shell did not exit cleanly\n");
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int alias_builtin_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-alias-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty alias: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_say=/bin/echo\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_say interactive\r") == -1 ||
        consume_through(&session, "\ninteractive\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_say\r") == -1 ||
        consume_through(&session, "gsh_say='/bin/echo'\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then alias gsh_compound=/bin/echo; fi\r") ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_compound committed\r") == -1 ||
        consume_through(&session, "\ncommitted\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "(unalias gsh_compound)\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_compound isolated\r") == -1 ||
        consume_through(&session, "\nisolated\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unalias gsh_say | /bin/cat\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_say pipeline-isolated\r") == -1 ||
        consume_through(&session, "\npipeline-isolated\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias 'gsh_prefix=gsh_say '\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_prefix forced\r") == -1 ||
        consume_through(&session, "\nforced\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unalias gsh_say > alias.out\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/test ! -s alias.out\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_say\r") == -1 ||
        consume_through(&session, "alias is not defined", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unalias -a\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty alias: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty alias: command left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        (void)fprintf(stderr, "pty alias: shell did not exit cleanly\n");
        failed = 1;
    }
    remove_fixture(fixture);
    return failed;
}

static void remove_hash_fixture(const char *fixture)
{
    static const char *const names[] = {
        "hash.out", "after.out", "clear.out"};
    char path[PATH_MAX];
    size_t index;

    for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        if (snprintf(path, sizeof(path), "%s/%s", fixture,
                     names[index]) < (int)sizeof(path)) {
            (void)unlink(path);
        }
    }
    (void)rmdir(fixture);
}

static int command_hash_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-hash-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty hash: setup");
        remove_hash_fixture(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "PATH=/bin; hash sh >hash.out\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash\r") == -1 ||
        consume_through(&session, "sh=/bin/sh\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "(hash -r)\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash\r") == -1 ||
        consume_through(&session, "sh=/bin/sh\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "PATH=$PATH\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "hash >after.out; /bin/test ! -s after.out && "
                  "/usr/bin/printf GSH_HASH_PATH_CLEAR\r") == -1 ||
        consume_through(&session, "GSH_HASH_PATH_CLEAR",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then sh -c 'exit 0'; fi\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash\r") == -1 ||
        consume_through(&session, "sh=/bin/sh\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash -r >clear.out\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "hash >clear.out; /bin/test ! -s clear.out && "
                  "/usr/bin/printf GSH_HASH_COMMIT_CLEAR\r") == -1 ||
        consume_through(&session, "GSH_HASH_COMMIT_CLEAR",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty hash: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty hash: command left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    remove_hash_fixture(fixture);
    return failed;
}

static void remove_times_fixture(const char *fixture)
{
    static const char *const names[] = {"direct.out", "compound.out"};
    char path[PATH_MAX];
    size_t index;

    for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        if (snprintf(path, sizeof(path), "%s/%s", fixture,
                     names[index]) < (int)sizeof(path)) {
            (void)unlink(path);
        }
    }
    (void)rmdir(fixture);
}

static int times_builtin_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-times-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty times: setup");
        remove_times_fixture(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "times >direct.out; "
                  "/bin/test \"$(/usr/bin/wc -l <direct.out)\" -eq 2 && "
                  "/usr/bin/printf GSH_TIMES_DIRECT\r") == -1 ||
        consume_through(&session, "GSH_TIMES_DIRECT", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then times >compound.out; fi; "
                  "/bin/test \"$(/usr/bin/wc -l <compound.out)\" -eq 2 && "
                  "/usr/bin/printf GSH_TIMES_COMPOUND\r") == -1 ||
        consume_through(&session, "GSH_TIMES_COMPOUND", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_TIMES_VALUE=before; "
                  "GSH_TIMES_VALUE=after times >/dev/null; "
                  "/bin/test \"$GSH_TIMES_VALUE\" = after && "
                  "/usr/bin/printf GSH_TIMES_ASSIGN\r") == -1 ||
        consume_through(&session, "GSH_TIMES_ASSIGN", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "times unexpected\r") == -1 ||
        consume_through(&session, "does not accept operands",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf GSH_TIMES_RECOVERED\r") == -1 ||
        consume_through(&session, "GSH_TIMES_RECOVERED", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty times: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty times: command left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    remove_times_fixture(fixture);
    return failed;
}

static void remove_exec_fixture(const char *fixture)
{
    static const char *const names[] = {
        "descriptor.out", "compound.out", "compound.err", "failure.err",
        "managed.out"};
    char path[PATH_MAX];
    size_t index;

    for (index = 0; index < sizeof(names) / sizeof(names[0]); index++) {
        if (snprintf(path, sizeof(path), "%s/%s", fixture,
                     names[index]) < (int)sizeof(path)) {
            (void)unlink(path);
        }
    }
    (void)rmdir(fixture);
}

static int interactive_exec_overlay_case(
    const char *executable, const char *fixture, bool managed,
    const char *setup, const char *command, int expected_status)
{
    pty_session session;

    if ((managed ? start_managed_session(&session, executable, fixture)
                 : start_session(&session, executable, fixture, SHELL_GSH)) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        (setup != NULL &&
         (send_text(&session, setup) == -1 ||
          consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1)) ||
        send_text(&session, command) == -1 ||
        wait_session_exit(&session, expected_status, TEST_TIMEOUT_MS) == -1) {
        if (session.master >= 0) {
            dump_capture(&session);
            (void)stop_session(&session);
        }
        return 1;
    }
    return 0;
}

static int managed_exec_descriptor_case(const char *executable,
                                        const char *fixture)
{
    pty_session session;
    int failed = 0;

    if (start_managed_session(&session, executable, fixture) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then exec >managed.out; fi\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf GSH_MANAGED_EXEC_DESCRIPTOR\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "exec 1>/dev/tty\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/cat managed.out\r") == -1 ||
        consume_through(&session, "GSH_MANAGED_EXEC_DESCRIPTOR",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    return failed;
}

static int exec_internal_descriptor_case(const char *executable,
                                         const char *fixture)
{
    pty_session session;
    int failed = 0;

    if (start_session(&session, executable, fixture, SHELL_GSH) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "command exec 100>&3 101>&4 102>&5 103>&6 104>&7 "
                  "105>&8 106>&9 107>&10 108>&11 109>&12 110>&13 "
                  "111>&14 112>&15 113>&16 114>&17 115>&18 116>&19 "
                  "117>&20 118>&21 119>&22 120>&23 121>&24 122>&25 "
                  "123>&26 124>&27 125>&28 126>&29 127>&30 128>&31 "
                  "129>&32 130>&33 131>&34\r") == -1 ||
        consume_through(&session, "gsh: exec redirection:",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/true\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    return failed;
}

static int exec_builtin_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-exec-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty exec: setup");
        remove_exec_fixture(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_EXEC_KEEP=value exec 3>descriptor.out\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf persisted >&3; exec 3>&-; "
                  "/bin/test \"$GSH_EXEC_KEEP\" = value && "
                  "/bin/cat descriptor.out\r") == -1 ||
        consume_through(&session, "persisted", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then GSH_EXEC_COMPOUND=value "
                  "exec 4>compound.out; fi\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf compound >&4; exec 4>&-; "
                  "/bin/test \"$GSH_EXEC_COMPOUND\" = value && "
                  "/bin/cat compound.out\r") == -1 ||
        consume_through(&session, "compound", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then command exec 2>compound.err "
                  "/definitely/missing; fi\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf GSH_EXEC_COMPOUND_REDIRECT >&2; "
                  "/bin/cat compound.err\r") == -1 ||
        consume_through(&session, "GSH_EXEC_COMPOUND_REDIRECT",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "command exec 2>failure.err /definitely/missing\r") ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf GSH_EXEC_REDIRECT_PERSISTED >&2; "
                  "/bin/cat failure.err\r") == -1 ||
        consume_through(&session, "GSH_EXEC_REDIRECT_PERSISTED",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_EXEC_INTERACTIVE=value exec /bin/sh -c "
                  "'/bin/test \"$GSH_EXEC_INTERACTIVE\" = value || exit 9; "
                  "exit 7'\r") == -1 ||
        wait_session_exit(&session, 7, TEST_TIMEOUT_MS) == -1) {
        perror("pty exec: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (session.master >= 0 && stop_session(&session) == -1) {
        failed = 1;
    }
    if (!failed &&
        (exec_internal_descriptor_case(executable, fixture) != 0 ||
         managed_exec_descriptor_case(executable, fixture) != 0 ||
         interactive_exec_overlay_case(
             executable, fixture, true, NULL,
             "exec /bin/sh -c 'exit 11'\r", 11) != 0 ||
         interactive_exec_overlay_case(
             executable, fixture, false, NULL,
             "if true; then exec /bin/sh -c 'exit 13'; fi\r", 13) != 0 ||
         interactive_exec_overlay_case(
             executable, fixture, true, NULL,
             "if true; then exec /bin/sh -c 'exit 14'; fi\r", 14) != 0 ||
         interactive_exec_overlay_case(
             executable, fixture, false,
             "gsh_exec_function() { exec /bin/sh -c 'exit 15'; }\r",
             "gsh_exec_function\r", 15) != 0)) {
        perror("pty exec: overlay cases");
        failed = 1;
    }
    remove_exec_fixture(fixture);
    return failed;
}

static int write_enoexec_fixture(const char *directory, char path[PATH_MAX])
{
    static const char source[] =
        "/usr/bin/printf 'GSH_ENOEXEC_INTERACTIVE:<%s>\\n' \"$1\"\n"
        "exit 6\n";
    size_t written = 0;
    size_t attempts;
    int descriptor;
    int status;

    if (snprintf(path, PATH_MAX, "%s/probe", directory) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
    if (descriptor == -1) {
        return -1;
    }
    for (attempts = 0;
         written < sizeof(source) - 1U && attempts < sizeof(source);
         attempts++) {
        ssize_t count = write(descriptor, source + written,
                              sizeof(source) - 1U - written);

        if (count > 0) {
            written += (size_t)count;
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    status = written == sizeof(source) - 1U &&
                     fchmod(descriptor, 0700) == 0
                 ? 0
                 : -1;
    if (close(descriptor) == -1) {
        status = -1;
    }
    return status;
}

static int interactive_enoexec_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-enoexec-XXXXXX";
    char script[PATH_MAX] = {0};
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        write_enoexec_fixture(fixture, script) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty ENOEXEC: setup");
        if (script[0] != '\0') {
            (void)unlink(script);
        }
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "./probe interactive\r") == -1 ||
        consume_through(&session, "GSH_ENOEXEC_INTERACTIVE:<interactive>",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/test \"$?\" -eq 6 && "
                  "/usr/bin/printf GSH_ENOEXEC_STATUS\r") == -1 ||
        consume_through(&session, "GSH_ENOEXEC_STATUS", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty ENOEXEC: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    if (script[0] != '\0') {
        (void)unlink(script);
    }
    (void)rmdir(fixture);
    return failed;
}

static int finish_source_fixture(const char *fixture, const char *state_path,
                                 const char *exit_path,
                                 const char *managed_path,
                                 const char *child_path, int failed)
{
    if (state_path == NULL || exit_path == NULL || managed_path == NULL ||
        child_path == NULL) return -1;
    if (fixture == NULL) {
        return -1;
    }
    if (state_path[0] != '\0') (void)unlink(state_path);
    if (exit_path[0] != '\0') (void)unlink(exit_path);
    if (managed_path[0] != '\0') (void)unlink(managed_path);
    if (child_path[0] != '\0') (void)rmdir(child_path);
    (void)rmdir(fixture);
    return failed;
}

static int interactive_source_state_case(const char *executable,
                                         const char *fixture)
{
    char child[PATH_MAX];
    char canonical_child[PATH_MAX];
    pty_session session;
    int failed = 0;

    if (snprintf(child, sizeof(child), "%s/child", fixture) >=
            (int)sizeof(child) ||
        realpath(child, canonical_child) == NULL ||
        setenv("GSH_SOURCE_CHILD", canonical_child, 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty source: state setup");
        (void)unsetenv("GSH_SOURCE_CHILD");
        return 1;
    }
    (void)unsetenv("GSH_SOURCE_CHILD");
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "eval 'GSH_EVAL_VALUE=committed; "
                  "set -- eval-one \"eval two\"; set -f; "
                  "alias gsh_eval_alias=\"/usr/bin/printf GSH_EVAL_ALIAS\"; "
                  "gsh_eval_fn(){ /usr/bin/printf GSH_EVAL_FUNCTION; }; "
                  "cd child'\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if /bin/test \"$GSH_EVAL_VALUE\" = committed && "
                  "/bin/test \"$#\" -eq 2 && "
                  "/bin/test \"$1\" = eval-one && "
                  "/bin/test \"$2\" = 'eval two' && "
                  "/bin/test \"$(/bin/pwd)\" = \"$GSH_SOURCE_CHILD\"; "
                  "then case $- in *f*) /usr/bin/printf GSH_EVAL_STATE;; "
                  "esac; fi\r") == -1 ||
        consume_through(&session, "\r\nGSH_EVAL_STATE",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_eval_alias\r") == -1 ||
        consume_through(&session, "GSH_EVAL_ALIAS", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_eval_fn\r") == -1 ||
        consume_through(&session, "GSH_EVAL_FUNCTION", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +f; cd ..\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, ". ./source-state.sh\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_DOT_STATUS=$?; "
                  "if /bin/test \"$GSH_DOT_STATUS\" -eq 6 && "
                  "/bin/test \"$GSH_DOT_VALUE\" = committed && "
                  "/bin/test \"$#\" -eq 2 && "
                  "/bin/test \"$1\" = dot-one && "
                  "/bin/test \"$2\" = 'dot two' && "
                  "/bin/test \"$(/bin/pwd)\" = \"$GSH_SOURCE_CHILD\"; "
                  "then case $- in *f*) /usr/bin/printf GSH_DOT_STATE;; "
                  "esac; fi\r") == -1 ||
        consume_through(&session, "\r\nGSH_DOT_STATE",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_dot_alias\r") == -1 ||
        consume_through(&session, "GSH_DOT_ALIAS", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_dot_fn\r") == -1 ||
        consume_through(&session, "GSH_DOT_FUNCTION", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +f; cd ..; eval 'if'\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf GSH_EVAL_RECOVERED\r") == -1 ||
        consume_through(&session, "\r\nGSH_EVAL_RECOVERED",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "(eval 'exit 7'); /bin/test \"$?\" -eq 7 && "
                  "/usr/bin/printf GSH_SUBSHELL_EXIT\r") == -1 ||
        consume_through(&session, "\r\nGSH_SUBSHELL_EXIT",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "eval 'exit 8' | /bin/cat; "
                  "/usr/bin/printf GSH_PIPELINE_EXIT\r") == -1 ||
        consume_through(&session, "\r\nGSH_PIPELINE_EXIT",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "eval 'exit 9' & wait \"$!\"; "
                  "/bin/test \"$?\" -eq 9 && "
                  "/usr/bin/printf GSH_ASYNC_SOURCE_EXIT\r") == -1 ||
        consume_through(&session, "\r\nGSH_ASYNC_SOURCE_EXIT",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty source: state flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty source: state flow left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    return failed;
}

static int interactive_managed_source_case(const char *executable,
                                           const char *fixture)
{
    pty_session session;
    int failed = 0;

    if (start_managed_session(&session, executable, fixture) == -1) {
        perror("pty source: managed setup");
        return 1;
    }
    if (consume_through(&session, "gsh$ ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "eval 'GSH_MANAGED_EVAL=committed'; "
                  ". ./managed-source.sh\r") == -1 ||
        consume_through(&session, "gsh$ ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/test \"$GSH_MANAGED_EVAL:$GSH_MANAGED_DOT\" = "
                  "committed:committed && "
                  "/usr/bin/printf GSH_MANAGED_SOURCE_STATE\r") == -1 ||
        consume_through(&session, "\r\nGSH_MANAGED_SOURCE_STATE",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ",
                        TEST_TIMEOUT_MS) == -1) {
        perror("pty source: managed flow");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    return failed;
}

static int interactive_exit_case(const char *executable,
                                 const char *fixture, bool managed,
                                 const char *setup, const char *command,
                                 int expected_status)
{
    pty_session session;

    if ((managed ? start_managed_session(&session, executable, fixture)
                 : start_session(&session, executable, fixture, SHELL_GSH)) ==
        -1) {
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        (setup != NULL &&
         (send_text(&session, setup) == -1 ||
          consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1)) ||
        send_text(&session, command) == -1 ||
        wait_session_exit(&session, expected_status, TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr, "pty source: exit case failed status=%d\n",
                expected_status);
        dump_capture(&session);
        return 1;
    }
    return 0;
}

static int interactive_source_flow(const char *executable)
{
    if (executable == NULL) {
        return -1;
    }
    static const char state_source[] =
        "GSH_DOT_VALUE=committed\n"
        "set -- dot-one 'dot two'\n"
        "set -f\n"
        "alias gsh_dot_alias='/usr/bin/printf GSH_DOT_ALIAS'\n"
        "gsh_dot_fn() { /usr/bin/printf GSH_DOT_FUNCTION; }\n"
        "cd child\n"
        "return 6\n"
        "/usr/bin/printf GSH_DOT_BAD\n";
    char fixture[] = "/tmp/gsh-pty-source-XXXXXX";
    char state_path[PATH_MAX] = {0};
    char exit_path[PATH_MAX] = {0};
    char managed_path[PATH_MAX] = {0};
    char child_path[PATH_MAX] = {0};
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        snprintf(state_path, sizeof(state_path), "%s/source-state.sh",
                 fixture) >= (int)sizeof(state_path) ||
        snprintf(exit_path, sizeof(exit_path), "%s/source-exit.sh",
                 fixture) >= (int)sizeof(exit_path) ||
        snprintf(managed_path, sizeof(managed_path), "%s/managed-source.sh",
                 fixture) >= (int)sizeof(managed_path) ||
        snprintf(child_path, sizeof(child_path), "%s/child", fixture) >=
            (int)sizeof(child_path) ||
        mkdir(child_path, 0700) == -1 ||
        write_text_file(state_path, state_source, 0600) == -1 ||
        write_text_file(exit_path, "GSH_DOT_EXIT=committed\nexit 17\n",
                        0600) == -1 ||
        write_text_file(managed_path, "GSH_MANAGED_DOT=committed\n",
                        0600) == -1) {
        perror("pty source: fixture");
        return finish_source_fixture(fixture, state_path, exit_path,
                                     managed_path, child_path, 1);
    }
    failed |= interactive_source_state_case(executable, fixture);
    failed |= interactive_managed_source_case(executable, fixture);
    failed |= interactive_exit_case(
        executable, fixture, false, NULL,
        "if true; then exit 23; fi\r", 23);
    failed |= interactive_exit_case(
        executable, fixture, false, NULL, "eval 'exit 19'\r", 19);
    failed |= interactive_exit_case(
        executable, fixture, false, NULL, ". ./source-exit.sh\r", 17);
    failed |= interactive_exit_case(
        executable, fixture, false,
        "gsh_exit_fn(){ exit 21; }\r", "gsh_exit_fn\r", 21);
    failed |= interactive_exit_case(
        executable, fixture, false, NULL, "! { exit 26; }\r", 26);
    failed |= interactive_exit_case(
        executable, fixture, true, NULL, "eval 'exit 24'\r", 24);

    return finish_source_fixture(fixture, state_path, exit_path,
                                 managed_path, child_path, failed);
}

static int function_builtin_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-function-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty function: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "gsh_fn() { GSH_FN_VALUE=after; "
                  "/usr/bin/printf '<%s:%s>\\n' \"$#\" \"$1\"; "
                  "return 7; /usr/bin/printf BAD; }\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_fn value\r") == -1 ||
        consume_through(&session, "<1:value>\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"$?|$GSH_FN_VALUE\"\r") == -1 ||
        consume_through(&session, "<7|after>\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unset -f gsh_fn\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_fn\r") == -1 ||
        consume_through(&session, "command not found", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "{ gsh_tx(){ /usr/bin/printf TX1; }; }\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_tx\r") == -1 ||
        consume_through(&session, "TX1", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "{ gsh_tx(){ /usr/bin/printf TX2; }; }\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_tx\r") == -1 ||
        consume_through(&session, "TX2", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "{ unset -f gsh_tx; }\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_tx\r") == -1 ||
        consume_through(&session, "command not found", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty function: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty function: command left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        (void)fprintf(stderr, "pty function: shell did not exit cleanly\n");
        failed = 1;
    }
    remove_fixture(fixture);
    return failed;
}

static int deferred_pattern_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-pattern-XXXXXX";
    char subject[12001];
    char directory_value[13024];
    size_t fixture_length;
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL) {
        perror("pty pattern: fixture");
        return 1;
    }
    (void)memset(subject, '0', sizeof(subject) - 2U);
    subject[sizeof(subject) - 2U] = 'z';
    subject[sizeof(subject) - 1U] = '\0';
    fixture_length = strlen(fixture);
    if (12000U + fixture_length + 1U > sizeof(directory_value)) {
        (void)rmdir(fixture);
        return 1;
    }
    (void)memset(directory_value, '@', 12000U);
    (void)memcpy(directory_value + 12000U, fixture, fixture_length + 1U);
    if (setenv("GSH_PTY_PATTERN_LONG", subject, 1) == -1 ||
        setenv("GSH_PTY_PATTERN_CD", directory_value, 1) == -1 ||
        start_session(&session, executable, "/tmp", SHELL_GSH) == -1) {
        perror("pty pattern: setup");
        (void)unsetenv("GSH_PTY_PATTERN_LONG");
        (void)unsetenv("GSH_PTY_PATTERN_CD");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_PTY_PATTERN_LONG");
    (void)unsetenv("GSH_PTY_PATTERN_CD");
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_PTY_DEFERRED="
                  "${GSH_PTY_PATTERN_LONG##?*?*?*z}\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"$GSH_PTY_DEFERRED\"\r") == -1 ||
        consume_through(&session, "<>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "cd \"${GSH_PTY_PATTERN_CD##*@*@}\"\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/pwd\r") == -1 ||
        consume_through(&session, fixture, TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty pattern: deferred flow");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int asynchronous_list_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-async-XXXXXX";
    pty_session session;
    uint64_t prompt_start;
    uint64_t prompt_duration;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty async: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty async: base prompt");
        return finish_session_directory(&session, fixture, 1);
    }

    prompt_start = monotonic_ns();
    if (send_text(&session, "/bin/sleep 1 &\r") == -1 ||
        consume_through(&session, "] ", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty async: nonblocking launch");
        return finish_session_directory(&session, fixture, 1);
    }
    prompt_duration = monotonic_ns() - prompt_start;
    if (prompt_duration >= 700000000ULL ||
        send_text(&session,
                  "/bin/test \"$!\" -gt 0 && "
                  "/usr/bin/printf GSH_ASYNC_PID\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_PID", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait \"$!\"\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/test \"$?\" -eq 0 && "
                  "/usr/bin/printf GSH_ASYNC_WAIT\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_WAIT", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/sh -c 'exit 7' &\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait \"$!\"\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/test \"$?\" -eq 7 && "
                  "/usr/bin/printf GSH_ASYNC_STATUS\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_STATUS", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/sleep 0.02 & /usr/bin/printf "
                  "'GSH_ASYNC_MIXED:%s\\n' \"$!\"\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_MIXED:", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/sh -c 'exit 9' & wait \"$!\"; "
                  "/bin/test \"$?\" -eq 9 && "
                  "/usr/bin/printf GSH_ASYNC_SEQUENCE\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_SEQUENCE",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/sh -c 'exit 11' & wait \"$!\" && false || "
                  "/usr/bin/printf GSH_ASYNC_ANDOR\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_ANDOR",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/true & wait \"$!\" && "
                  "/usr/bin/printf GSH_ASYNC_AND\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_AND", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        ((prompt_start = monotonic_ns()),
         send_text(&session,
                   "/bin/sleep 5 & wait \"$(/bin/sleep 2; "
                   "/usr/bin/printf 1)\"\r") == -1) ||
        consume_through(&session,
                        "wait expansion requires isolated continuation",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        monotonic_ns() - prompt_start >= 700000000ULL ||
        send_text(&session, "/bin/kill \"$!\"\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait \"$!\"\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/sleep 5 &\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait \"$!\"\r") == -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "^C", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/test \"$?\" -eq 130 && "
                  "/usr/bin/printf GSH_ASYNC_CANCEL\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_CANCEL", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/kill \"$!\"\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait \"$!\"\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty async: semantics");
        dump_capture(&session);
        return finish_session_directory(&session, fixture, 1);
    }
    if (process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty async: completed jobs left child processes\n");
        failed = 1;
    }

    return finish_session_directory(&session, fixture, failed);
}

static int async_redirection_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-redirection-XXXXXX";
    char fifo[1024];
    char created[1024];
    char command[1200];
    struct stat information;
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        snprintf(fifo, sizeof(fifo), "%s/output", fixture) >=
            (int)sizeof(fifo) || mkfifo(fifo, 0600) == -1 ||
        snprintf(created, sizeof(created), "%s/created", fixture) >=
            (int)sizeof(created) ||
        snprintf(command, sizeof(command), ": >%s\r", fifo) >=
            (int)sizeof(command) ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty redirection: setup");
        (void)unlink(fifo);
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "umask 077\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        snprintf(command, sizeof(command), ": >%s\r", created) >=
            (int)sizeof(command) || send_text(&session, command) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        stat(created, &information) == -1 ||
        (information.st_mode & 0777) != 0600 ||
        send_text(&session, "set -C\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        snprintf(command, sizeof(command), ": >%s\r", fifo) >=
            (int)sizeof(command) ||
        send_text(&session, command) == -1 ||
        consume_through(&session, fifo, TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "^C", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "worker=on", "busy=0") == -1) {
        perror("pty redirection: cancellation");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty redirection: worker was not recovered\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)unlink(created);
    (void)unlink(fifo);
    (void)rmdir(fixture);
    return failed;
}

static int worker_fault_case(const char *executable, const char *fault,
                             const char *counter)
{
    char fixture[] = "/tmp/gsh-fault-worker-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL || setenv("GSH_FAULT", fault, 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fault: worker setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "x") == -1 ||
        consume_through(&session, "x", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\025", 1) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "worker=off", counter) == -1) {
        (void)fprintf(stderr, "pty fault: worker case failed: %s\n", fault);
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        (void)fprintf(stderr, "pty fault: worker cleanup failed: %s\n", fault);
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int worker_surviving_fault_case(const char *executable,
                                       const char *fault,
                                       const char *counter)
{
    char fixture[] = "/tmp/gsh-fault-surviving-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        setenv("GSH_FAULT", fault, 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fault: surviving setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "worker=on", counter) == -1) {
        (void)fprintf(stderr, "pty fault: surviving case failed: %s\n", fault);
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int command_fault_case(const char *executable, const char *fault,
                              const char *command, const char *diagnostic,
                              bool managed)
{
    char fixture[] = "/tmp/gsh-fault-command-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL || setenv("GSH_FAULT", fault, 1) == -1 ||
        (managed ? start_managed_session(&session, executable, fixture)
                 : start_session(&session, executable, fixture, SHELL_GSH)) ==
            -1) {
        perror("pty fault: command setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if ((managed && resize_session(&session, 24U, 512U) == -1) ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, diagnostic, TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "job=idle", NULL) == -1) {
        (void)fprintf(stderr, "pty fault: command case failed: %s\n", fault);
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        (void)fprintf(stderr, "pty fault: command cleanup failed: %s\n", fault);
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int positional_commit_fault_case(const char *executable,
                                        const char *fault,
                                        const char *diagnostic)
{
    char fixture[] = "/tmp/gsh-fault-positionals-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL || setenv("GSH_FAULT", fault, 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fault: positional setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- before\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- after; :\r") == -1 ||
        consume_through(&session, diagnostic, TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$#|$1\"\r") == -1 ||
        consume_through(&session, "<1|before>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr, "pty fault: positional case failed: %s\n", fault);
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int directory_commit_fault_case(const char *executable,
                                       const char *fault,
                                       const char *diagnostic)
{
    char fixture[] = "/tmp/gsh-fault-directory-XXXXXX";
    char command[1200];
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        snprintf(command, sizeof(command),
                 "/bin/test \"$(/bin/pwd)\" = \"%s\" && "
                 "/usr/bin/printf GSH_DIRECTORY_ROLLED_BACK\r",
                 fixture) >= (int)sizeof(command) ||
        setenv("GSH_FAULT", fault, 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fault: directory setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if /usr/bin/true; then cd /; fi\r") == -1 ||
        consume_through(&session, diagnostic, TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "GSH_DIRECTORY_ROLLED_BACK",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr, "pty fault: directory case failed: %s\n", fault);
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int alias_commit_fault_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-fault-alias-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        setenv("GSH_FAULT", "alias-commit-malformed", 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fault: alias commit setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_keep=/bin/echo\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if /usr/bin/true; then "
                  "alias gsh_keep=/usr/bin/false; "
                  "alias gsh_new=/usr/bin/true; fi\r") == -1 ||
        consume_through(&session, "gsh: state transaction rejected",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_keep rolled-back\r") == -1 ||
        consume_through(&session, "\nrolled-back\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_new\r") == -1 ||
        consume_through(&session, "alias is not defined", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr, "pty fault: alias commit rollback failed\n");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int command_cache_commit_fault_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-fault-command-cache-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        setenv("GSH_FAULT", "command-cache-commit-malformed", 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fault: command cache commit setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "PATH=/bin\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash sh\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "if true; then hash -r; fi\r") == -1 ||
        consume_through(&session, "gsh: state transaction rejected",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash\r") == -1 ||
        consume_through(&session, "sh=/bin/sh\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr, "pty fault: command cache rollback failed\n");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int function_commit_fault_case(const char *executable,
                                      const char *fault)
{
    char fixture[] = "/tmp/gsh-fault-function-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL || setenv("GSH_FAULT", fault, 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fault: function commit setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "gsh_keep(){ /usr/bin/printf ORIGINAL; }\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "{ gsh_keep(){ /usr/bin/printf CHANGED; }; "
                  "gsh_new(){ :; }; }\r") == -1 ||
        consume_through(&session, "gsh: state transaction rejected",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_keep\r") == -1 ||
        consume_through(&session, "ORIGINAL", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_new\r") == -1 ||
        consume_through(&session, "command not found", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr, "pty fault: function commit rollback failed: %s\n",
                fault);
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int heredoc_fault_case(const char *executable, const char *fault,
                              const char *diagnostic)
{
    if (diagnostic == NULL || executable == NULL || fault == NULL) {
        return -1;
    }
    return command_fault_case(executable, fault,
                              "/bin/cat <<EOF\rvalue\rEOF\r",
                              diagnostic, false);
}

static int fatal_fault_case(const char *executable, const char *fault)
{
    char fixture[] = "/tmp/gsh-fault-fatal-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL || setenv("GSH_FAULT", fault, 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fault: fatal setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if (wait_session_exit(&session, 1, TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr, "pty fault: fatal case failed: %s\n", fault);
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int wait_fault_child(pid_t pid, int *status)
{
    if (status == NULL) {
        return -1;
    }
    uint64_t deadline = monotonic_ns() + 2000000000ULL;
    size_t attempts;

    for (attempts = 0; attempts < 201U; attempts++) {
        pid_t waited = waitpid(pid, status, WNOHANG);

        if (waited == pid) {
            return 0;
        }
        if (waited == -1 && errno != EINTR) {
            return -1;
        }
        if (monotonic_ns() >= deadline) {
            break;
        }
        (void)poll(NULL, 0, 10);
    }
    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, status, 0);
    return -1;
}

static int open_scratch_descriptor(void)
{
    char path[] = "/tmp/gsh-pty-scratch-XXXXXX";
    int descriptor = mkstemp(path);

    if (descriptor == -1) {
        return -1;
    }
    if (unlink(path) == -1) {
        int saved = errno;

        (void)close(descriptor);
        errno = saved;
        return -1;
    }
    return descriptor;
}

static size_t read_scratch_descriptor(int descriptor, char *output,
                                      size_t capacity)
{
    size_t length = 0;
    size_t attempts;

    if (descriptor < 0 || output == NULL || capacity == 0U ||
        lseek(descriptor, 0, SEEK_SET) == (off_t)-1) {
        return 0U;
    }
    for (attempts = 0; length < capacity && attempts <= capacity;
         attempts++) {
        ssize_t count = read(descriptor, output + length,
                             capacity - length);

        if (count > 0) {
            length += (size_t)count;
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            return 0U;
        }
    }
    return length;
}

static int noninteractive_fault_case(const char *executable,
                                     const char *fault,
                                     const char *command, int expected,
                                     const char *diagnostic)
{
    if (command == NULL || executable == NULL || fault == NULL) {
        return -1;
    }
    int capture = open_scratch_descriptor();
    char output[4096];
    size_t length = 0;
    int status = 0;
    pid_t pid;

    if (capture == -1) {
        return 1;
    }
    pid = fork();
    if (pid == 0) {
        if (setenv("GSH_FAULT", fault, 1) == -1 ||
            dup2(capture, STDOUT_FILENO) == -1 ||
            dup2(capture, STDERR_FILENO) == -1) {
            _exit(126);
        }
        execl(executable, executable, "--native-only", "-c", command,
              (char *)NULL);
        _exit(127);
    }
    if (pid == -1 || wait_fault_child(pid, &status) == -1) {
        (void)close(capture);
        return 1;
    }
    length = read_scratch_descriptor(capture, output, sizeof(output));
    (void)close(capture);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected ||
        find_bytes((const unsigned char *)output, length, diagnostic) == NULL) {
        (void)fprintf(stderr, "pty fault: non-interactive case failed: %s\n",
                fault);
        return 1;
    }
    return 0;
}

static int prepare_noninteractive_fault_input(int descriptor)
{
    static const char prefix[] = "alias fault_line=':'\nfault_line #";
    unsigned char bytes[4096];
    size_t written = 0;
    size_t attempts;

    if (descriptor < 0 || write(descriptor, prefix, sizeof(prefix) - 1U) !=
            (ssize_t)(sizeof(prefix) - 1U)) {
        return -1;
    }
    (void)memset(bytes, 'x', sizeof(bytes));
    for (attempts = 0;
         written < GSH_SOURCE_INPUT_CAP && attempts < SIZE_MAX;
         attempts++) {
        size_t remaining = GSH_SOURCE_INPUT_CAP - written;
        size_t request = remaining < sizeof(bytes) ? remaining
                                                   : sizeof(bytes);
        ssize_t count = write(descriptor, bytes, request);

        if (count > 0) {
            written += (size_t)count;
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    if (written != GSH_SOURCE_INPUT_CAP ||
        write(descriptor, "\n", 1) != 1 ||
        lseek(descriptor, 0, SEEK_SET) == (off_t)-1) {
        return -1;
    }
    return 0;
}

static int noninteractive_input_fault_case(const char *executable,
                                           const char *fault,
                                           const char *diagnostic)
{
    if (executable == NULL || fault == NULL) {
        return -1;
    }
    int input = open_scratch_descriptor();
    int capture = open_scratch_descriptor();
    char output[4096];
    size_t length = 0;
    int status = 0;
    pid_t pid;

    if (input == -1 || capture == -1 ||
        prepare_noninteractive_fault_input(input) == -1) {
        if (input != -1) {
            (void)close(input);
        }
        if (capture != -1) {
            (void)close(capture);
        }
        return 1;
    }
    pid = fork();
    if (pid == 0) {
        if (setenv("GSH_FAULT", fault, 1) == -1 ||
            dup2(input, STDIN_FILENO) == -1 ||
            dup2(capture, STDOUT_FILENO) == -1 ||
            dup2(capture, STDERR_FILENO) == -1) {
            _exit(126);
        }
        execl(executable, executable, "-s", (char *)NULL);
        _exit(127);
    }
    if (pid == -1 || wait_fault_child(pid, &status) == -1) {
        (void)close(input);
        (void)close(capture);
        return 1;
    }
    length = read_scratch_descriptor(capture, output, sizeof(output));
    (void)close(input);
    (void)close(capture);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 125 ||
        find_bytes((const unsigned char *)output, length, diagnostic) ==
            NULL) {
        (void)fprintf(stderr, "pty fault: non-interactive input failed: %s\n",
                fault);
        return 1;
    }
    return 0;
}

static int enoexec_fault_case(const char *executable)
{
    if (executable == NULL) {
        return -1;
    }
    char fixture[] = "/tmp/gsh-fault-enoexec-XXXXXX";
    char script[PATH_MAX] = {0};
    char command[PATH_MAX + 4U];
    int length;
    int failed = 1;

    if (mkdtemp(fixture) != NULL &&
        write_enoexec_fixture(fixture, script) == 0) {
        length = snprintf(command, sizeof(command), "'%s'", script);
        if (length >= 0 && length < (int)sizeof(command)) {
            failed = noninteractive_fault_case(
                executable, "enoexec-interpreter-open", command, 126,
                "execution failed");
        }
    }
    if (script[0] != '\0') {
        (void)unlink(script);
    }
    (void)rmdir(fixture);
    return failed;
}

static int fault_injection_flow(const char *executable)
{
    if (executable == NULL) {
        return -1;
    }
    static const struct {
        const char *name;
        const char *counter;
    } worker_cases[] = {
        {"worker-socket", "failures=1"},
        {"worker-fork", "failures=1"},
    };
    static const struct {
        const char *name;
        const char *command;
        const char *diagnostic;
    } command_cases[] = {
        {"resource-action-socket", "ls -1\r", "gsh: managed pipeline:"},
        {"job-pipe", "/usr/bin/true\r", "gsh: pipe:"},
        {"job-fork", "/usr/bin/true\r", "gsh: fork:"},
        {"terminal-handoff", "/usr/bin/true\r",
         "gsh: terminal handoff:"},
        {"exec", "/usr/bin/true\r", "execution failed"},
        {"pipeline-pipe", "/usr/bin/printf X | /bin/cat\r",
         "gsh: pipeline pipe:"},
        {"pipeline-fork", "/usr/bin/printf X | /bin/cat\r",
         "gsh: pipeline fork:"},
        {"pipeline-fork:2", "/usr/bin/printf X | /bin/cat\r",
         "gsh: pipeline fork:"},
        {"descriptor-dup", "/usr/bin/printf X | /bin/cat\r",
         "pipeline"},
        {"redirect-open", "/usr/bin/printf X > native.out\r",
         "native.out: execution failed"},
        {"descriptor-save", "export GSH_FAULT_EXPORT=value >/dev/null\r",
         "gsh: redirection save:"},
        {"descriptor-dup", "export GSH_FAULT_EXPORT=value 1>&2\r",
         "gsh: redirection:"},
        {"redirect-open", "export GSH_FAULT_EXPORT=value > native.out\r",
         "gsh: redirection:"},
        {"heredoc-write",
         "export GSH_FAULT_EXPORT=value <<EOF\rvalue\rEOF\r",
         "gsh: redirection:"},
        {"evaluator-gate", "/usr/bin/true && /usr/bin/true\r",
         "gsh: evaluator gate:"},
        {"evaluator-fork", "/usr/bin/true && /usr/bin/true\r",
         "gsh: evaluator fork:"},
        {"job-service-socket", "jobs; /usr/bin/true\r",
         "gsh: job service socket:"},
        {"job-table-allocation", "/usr/bin/true && /usr/bin/true\r",
         "evaluator job state"},
        {"exec-outcome-pipe",
         "if true; then exec /usr/bin/true; fi\r",
         "gsh: exec outcome pipe:"},
        {"exec-descriptor-socket",
         "if true; then exec /usr/bin/true; fi\r",
         "gsh: exec descriptor socket:"},
        {"exec-owner-descriptor-relocation",
         "exec 3>/dev/null 4>/dev/null 5>/dev/null 6>/dev/null "
         "7>/dev/null 8>/dev/null 9>/dev/null 10>/dev/null "
         "11>/dev/null 12>/dev/null 13>/dev/null 14>/dev/null "
         "15>/dev/null 16>/dev/null 17>/dev/null 18>/dev/null "
         "19>/dev/null 20>/dev/null 21>/dev/null 22>/dev/null "
         "23>/dev/null 24>/dev/null 25>/dev/null 26>/dev/null "
         "27>/dev/null 28>/dev/null 29>/dev/null 30>/dev/null "
         "31>/dev/null 32>/dev/null 33>/dev/null 34>/dev/null\r",
         "gsh: exec descriptor protection:"},
        {"transaction-descriptor-relocation",
         "if true; then exec 3>/dev/null 4>/dev/null 5>/dev/null "
         "6>/dev/null 7>/dev/null 8>/dev/null 9>/dev/null 10>/dev/null "
         "11>/dev/null 12>/dev/null 13>/dev/null 14>/dev/null "
         "15>/dev/null 16>/dev/null 17>/dev/null 18>/dev/null "
         "19>/dev/null 20>/dev/null 21>/dev/null 22>/dev/null "
         "23>/dev/null 24>/dev/null 25>/dev/null 26>/dev/null "
         "27>/dev/null 28>/dev/null 29>/dev/null 30>/dev/null "
         "31>/dev/null 32>/dev/null 33>/dev/null 34>/dev/null; fi\r",
         "gsh: exec descriptor protection:"},
        {"exec-descriptor-send",
         "if true; then exec 8>/dev/null; fi\r",
         "gsh: exec descriptor commit:"},
        {"exec-descriptor-receive",
         "if true; then exec 8>/dev/null; fi\r",
         "gsh: exec descriptor transaction rejected"},
        {"exec-descriptor-apply",
         "if true; then exec 8>/dev/null; fi\r",
         "gsh: exec descriptor transaction rejected"},
        {"exec-descriptor-stabilize",
         "if true; then exec 3>/dev/null 4>/dev/null 5>/dev/null "
         "6>/dev/null 7>/dev/null 8>/dev/null 9>/dev/null 10>/dev/null "
         "11>/dev/null 12>/dev/null 13>/dev/null 14>/dev/null "
         "15>/dev/null 16>/dev/null 17>/dev/null 18>/dev/null "
         "19>/dev/null 20>/dev/null 21>/dev/null 22>/dev/null "
         "23>/dev/null 24>/dev/null 25>/dev/null 26>/dev/null "
         "27>/dev/null 28>/dev/null 29>/dev/null 30>/dev/null "
         "31>/dev/null 32>/dev/null 33>/dev/null 34>/dev/null; fi\r",
         "gsh: exec descriptor transaction rejected"},
        {"async-fork", "/usr/bin/true &\r",
         "gsh: asynchronous fork:"},
        {"expansion-assignment",
         "if /usr/bin/true; then : \"$((GSH_FAULT_ARITH = 7))\"; fi\r",
         "gsh: parameter assignment failed"},
        {"state-commit-pipe",
         "if /usr/bin/true; then : \"${GSH_FAULT_STATE:=value}\"; fi\r",
         "gsh: state transaction pipe:"},
        {"state-commit-write",
         "if /usr/bin/true; then : \"${GSH_FAULT_STATE:=value}\"; fi\r",
         "gsh: state transaction rejected"},
        {"state-commit-read",
         "if /usr/bin/true; then : \"${GSH_FAULT_STATE:=value}\"; fi\r",
         "gsh: state transaction rejected"},
        {"state-commit-malformed",
         "if /usr/bin/true; then : \"${GSH_FAULT_STATE:=value}\"; fi\r",
         "gsh: state transaction rejected"},
        {"state-control-commit-malformed",
         "if /usr/bin/true; then exit 7; fi\r",
         "gsh: state transaction rejected"},
        {"option-commit-malformed",
         "if /usr/bin/true; then set -f; fi\r",
         "gsh: state transaction rejected"},
        {"alias-allocation", "alias gsh_fault=/usr/bin/true\r",
         "gsh: alias allocation:"},
        {"alias-transaction-allocation",
         "if /usr/bin/true; then alias gsh_fault=/usr/bin/true; fi\r",
         "gsh: alias transaction allocation:"},
        {"function-allocation", "gsh_fault() { :; }\r",
         "gsh: function allocation:"},
        {"subshell-fork", "( /usr/bin/true )\r",
         "gsh: subshell fork:"},
        {"substitution-pipe",
         "/usr/bin/printf '%s' \"$(/usr/bin/printf x)\"\r",
         "gsh: command substitution pipe:"},
        {"substitution-fork",
         "/usr/bin/printf '%s' \"$(/usr/bin/printf x)\"\r",
         "gsh: command substitution fork:"},
        {"substitution-read",
         "/usr/bin/printf '%s' \"$(/usr/bin/printf x)\"\r",
         "gsh: command substitution read:"},
        {"source-workspace-exhaustion",
         "/usr/bin/printf '%s' \"$(/usr/bin/printf x)\"\r",
         "gsh: nested source workspace limit exceeded"},
        {"for-allocation", "for item in a b; do :; done\r",
         "gsh: for items:"},
        {"positional-allocation", "set -- value\r",
         "gsh: positional parameter allocation failed"},
    };
    static const char *const fatal_cases[] = {
        "allocation",
        "allocation:2",
        "allocation:3",
        "allocation:4",
        "allocation:5",
        "allocation:6",
        "allocation:7",
        "allocation:8",
        "allocation:9",
        "allocation:10",
        "allocation:11",
        "tty-open",
        "signal-pipe",
        "poll",
        "output-write",
    };
    size_t index;
    int failed = 0;

    for (index = 0; index < sizeof(worker_cases) / sizeof(worker_cases[0]);
         index++) {
        failed |= worker_fault_case(executable, worker_cases[index].name,
                                    worker_cases[index].counter);
    }
    for (index = 0; index < sizeof(command_cases) / sizeof(command_cases[0]);
         index++) {
        failed |= command_fault_case(executable, command_cases[index].name,
                                     command_cases[index].command,
                                     command_cases[index].diagnostic,
                                     strcmp(command_cases[index].name,
                                            "resource-action-socket") == 0);
    }
    failed |= worker_surviving_fault_case(executable, "time-source-failure",
                                          "misses=1");
    failed |= heredoc_fault_case(executable, "heredoc-pipe",
                                 "gsh: here-document pipe:");
    failed |= heredoc_fault_case(executable, "heredoc-fork",
                                 "gsh: here-document fork:");
    failed |= heredoc_fault_case(executable, "heredoc-write",
                                 "here-document: execution failed");
    failed |= positional_commit_fault_case(
        executable, "positional-commit-allocation",
        "gsh: positional transaction:");
    failed |= positional_commit_fault_case(
        executable, "positional-commit-malformed",
        "gsh: state transaction rejected");
    failed |= directory_commit_fault_case(
        executable, "directory-commit-socket",
        "gsh: directory transaction socket:");
    failed |= directory_commit_fault_case(
        executable, "directory-commit-open",
        "gsh: state transaction rejected");
    failed |= directory_commit_fault_case(
        executable, "directory-commit-send",
        "gsh: state transaction rejected");
    failed |= directory_commit_fault_case(
        executable, "directory-commit-receive",
        "gsh: state transaction rejected");
    failed |= directory_commit_fault_case(
        executable, "directory-commit-apply",
        "gsh: state transaction rejected");
    failed |= alias_commit_fault_case(executable);
    failed |= command_cache_commit_fault_case(executable);
    failed |= function_commit_fault_case(executable,
                                         "function-commit-malformed");
    failed |= function_commit_fault_case(executable,
                                         "function-commit-write");
    failed |= noninteractive_fault_case(
        executable, "source-workspace-exhaustion", "eval :", 125,
        "nested source workspace limit exceeded");
    failed |= noninteractive_fault_case(
        executable, "trap-workspace-exhaustion", "trap ':' EXIT; :", 125,
        "trap source workspace limit exceeded");
    failed |= noninteractive_fault_case(
        executable, "descriptor-save", "eval : >/dev/null", 125,
        "source redirection save");
    failed |= noninteractive_fault_case(
        executable, "redirect-open", "eval : >/dev/null", 1,
        "source redirection");
    failed |= noninteractive_fault_case(
        executable, "descriptor-dup", "eval : 1>&2", 1,
        "source redirection");
    failed |= noninteractive_fault_case(
        executable, "shell-executable-resolution", ":", 125,
        "executable resolution");
    failed |= noninteractive_input_fault_case(
        executable, "input-mode", "standard input input mode");
    failed |= noninteractive_input_fault_case(
        executable, "input-read", "standard input: Input/output error");
    failed |= noninteractive_input_fault_case(
        executable, "input-spill-open", "standard input: Too many open files");
    failed |= noninteractive_input_fault_case(
        executable, "input-spill-cloexec", "standard input: Input/output error");
    failed |= noninteractive_input_fault_case(
        executable, "input-spill-unlink", "standard input: Input/output error");
    failed |= noninteractive_input_fault_case(
        executable, "input-spill-write", "standard input: Input/output error");
    failed |= noninteractive_input_fault_case(
        executable, "input-spill-resize",
        "standard input: No space left on device");
    failed |= noninteractive_input_fault_case(
        executable, "input-map", "standard input: Cannot allocate memory");
    failed |= noninteractive_input_fault_case(
        executable, "input-alias-map",
        "standard input: Cannot allocate memory");
    failed |= enoexec_fault_case(executable);
    for (index = 0; index < sizeof(fatal_cases) / sizeof(fatal_cases[0]);
         index++) {
        failed |= fatal_fault_case(executable, fatal_cases[index]);
    }
    if (!failed) {
        (void)puts("pty fault: 89 deterministic boundary failures passed");
    }
    return failed;
}

static unsigned long diagnostic_counter(const pty_session *session,
                                        const char *name, bool *found)
{
    if (found == NULL || name == NULL || session == NULL) {
        return -1;
    }
    const unsigned char *position =
        find_bytes(session->capture, session->capture_length, name);
    unsigned long value = 0;
    size_t offset;

    *found = false;
    if (position == NULL) {
        return 0;
    }
    offset = (size_t)(position - session->capture) + strlen(name);
    if (offset >= session->capture_length || session->capture[offset] < '0' ||
        session->capture[offset] > '9') {
        return 0;
    }
    while (offset < session->capture_length &&
           session->capture[offset] >= '0' &&
           session->capture[offset] <= '9') {
        unsigned int digit = (unsigned int)(session->capture[offset] - '0');

        if (value > (ULONG_MAX - digit) / 10U) {
            return 0;
        }
        value = value * 10U + digit;
        offset++;
    }
    *found = true;
    return value;
}

static int descriptor_exhaustion_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-resource-fd-XXXXXX";
    char restore[64];
    pty_session session;
    struct rlimit original;
    int failed = 0;

    if (getrlimit(RLIMIT_NOFILE, &original) == -1) {
        return 1;
    }
    if ((original.rlim_cur == RLIM_INFINITY
             ? snprintf(restore, sizeof(restore),
                        "ulimit -S -n unlimited\r")
             : snprintf(restore, sizeof(restore), "ulimit -S -n %llu\r",
                        (unsigned long long)original.rlim_cur)) >=
        (int)sizeof(restore)) {
        return 1;
    }
    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: descriptor setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "ulimit -S -n 6\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/true\r") == -1 ||
        consume_through(&session, "gsh: pipe:", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, restore) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/true\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "job=idle", NULL) == -1) {
        (void)fprintf(stderr, "pty resource: descriptor recovery failed\n");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int bounded_input_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-resource-input-XXXXXX";
    char input[5000];
    pty_session session;
    bool found;
    unsigned long overloads;
    size_t signal_count;
    int failed = 0;

    (void)memset(input, 'a', sizeof(input));
    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: input setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, input, sizeof(input)) == -1 ||
        consume_through(&session, "\a", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\025", 1) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty resource: bounded line failed");
        dump_capture(&session);
        return finish_session_directory(&session, fixture, 1);
    }

    session.capture_length = 0;
    if (send_text(&session, "rt\r") == -1 ||
        wait_for_output(&session, "reactor cycles=", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return finish_session_directory(&session, fixture, 1);
    }
    overloads = diagnostic_counter(&session, "overloads=", &found);
    if (!found || overloads < 1) {
        (void)fprintf(stderr, "pty resource: line overload was not recorded\n");
        return finish_session_directory(&session, fixture, 1);
    }

    session.capture_length = 0;
    if (send_text(&session, "kept") == -1 ||
        consume_through(&session, "kept", TEST_TIMEOUT_MS) == -1) {
        return finish_session_directory(&session, fixture, 1);
    }
    for (signal_count = 0; signal_count < 10000; signal_count++) {
        if (kill(session.pid, SIGWINCH) == -1 && errno != ESRCH) {
            return finish_session_directory(&session, fixture, 1);
        }
    }
    if (consume_through(&session, "\033[2K", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "kept", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\025", 1) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr, "pty resource: signal storm corrupted the editor\n");
        failed = 1;
    }
    if (!failed &&
        (send_text(&session, "/bin/cat <<EOF\r") == -1 ||
         consume_through(&session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
         send_bytes(&session, input, 3000) == -1 ||
         send_text(&session, "\r") == -1 ||
         consume_through(&session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
         send_bytes(&session, input, 1500) == -1 ||
         send_text(&session, "\r") == -1 ||
         consume_through(&session, "gsh: command exceeds input limit",
                         TEST_TIMEOUT_MS) == -1 ||
         consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
         send_text(&session, "/usr/bin/true\r") == -1 ||
         consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1)) {
        (void)fprintf(stderr,
                "pty resource: multiline input recovery failed\n");
        dump_capture(&session);
        failed = 1;
    }

    return finish_session_directory(&session, fixture, failed);
}

static int initialization_exhaustion_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-resource-init-XXXXXX";
    pty_session session;
    int status = 0;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        setenv("GSH_HARNESS_NOFILE", "4", 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: initialization setup");
        (void)unsetenv("GSH_HARNESS_NOFILE");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_HARNESS_NOFILE");
    if (wait_session_result(&session, &status, TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr,
                "pty resource: initialization exhaustion did not restore "
                "the terminal\n");
        failed = 1;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
        (void)puts("pty resource: initialization RLIMIT_NOFILE=enforced");
    } else if (capture_contains(&session,
                                "error while loading shared libraries") ||
               capture_contains(&session, "rosetta error:")) {
        (void)puts("pty resource: initialization "
             "RLIMIT_NOFILE=unsupported-pre-exec");
    } else if (linux_is_translated()) {
        (void)puts("pty resource: initialization "
             "RLIMIT_NOFILE=unsupported-emulated");
    } else {
        (void)fprintf(stderr,
                "pty resource: initialization limit ended unexpectedly\n");
        dump_capture(&session);
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int process_exhaustion_case(const char *executable)
{
#ifdef RLIMIT_NPROC
    char fixture[] = "/tmp/gsh-resource-process-XXXXXX";
    pty_session session;
    bool enforced;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        setenv("GSH_HARNESS_NPROC", "1", 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: process setup");
        (void)unsetenv("GSH_HARNESS_NPROC");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_HARNESS_NPROC");
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return finish_session_directory(&session, fixture, 1);
    }
    session.capture_length = 0;
    if (send_text(&session, "/usr/bin/true\r") == -1 ||
        wait_for_output(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return finish_session_directory(&session, fixture, 1);
    }
    enforced = capture_contains(&session, "gsh: fork:");
    if (wait_for_diagnostics(&session, "job=idle", NULL) == -1) {
        return finish_session_directory(&session, fixture, 1);
    }
    (void)printf("pty resource: RLIMIT_NPROC=%s\n",
           enforced ? "enforced" : "unsupported-or-unenforced");

    return finish_session_directory(&session, fixture, failed);
#else
    (void)executable;
    (void)puts("pty resource: RLIMIT_NPROC=unsupported");
    return 0;
#endif
}

static int data_exhaustion_case(const char *executable)
{
    if (executable == NULL) {
        return -1;
    }
    char fixture[] = "/tmp/gsh-resource-data-XXXXXX";
    char restore[64];
    pty_session session;
    struct rlimit original;
    bool enforced;
    int failed = 0;

    if (linux_is_translated()) {
        (void)executable;
        (void)puts("pty resource: RLIMIT_DATA=unsupported-emulated");
        return 0;
    }
    if (getrlimit(RLIMIT_DATA, &original) == -1) {
        return 1;
    }
    if ((original.rlim_cur == RLIM_INFINITY
             ? snprintf(restore, sizeof(restore),
                        "ulimit -S -d unlimited\r")
             : snprintf(restore, sizeof(restore), "ulimit -S -d %llu\r",
                        (unsigned long long)(original.rlim_cur / 1024U))) >=
        (int)sizeof(restore)) {
        return 1;
    }
    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: data setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return finish_session_directory(&session, fixture, 1);
    }
    session.capture_length = 0;
    if (send_text(&session, "ulimit -S -d 1024\r") == -1 ||
        wait_for_output(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr, "pty resource: data limit setup failed\n");
        return finish_session_directory(&session, fixture, 1);
    }
    if (capture_contains(&session, "gsh: ulimit:")) {
        (void)puts("pty resource: RLIMIT_DATA=unsupported");
        return finish_session_directory(&session, fixture, 0);
    }
    session.capture_length = 0;
    if (send_text(&session, "/usr/bin/true\r") == -1 ||
        wait_for_output(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        (void)fprintf(stderr, "pty resource: data exhaustion was not contained\n");
        return finish_session_directory(&session, fixture, 1);
    }
    enforced = capture_contains(&session, "Cannot allocate memory") ||
               capture_contains(&session, "cannot allocate memory") ||
               capture_contains(&session, "error while loading") ||
               capture_contains(&session, "rosetta error:");
    session.capture_length = 0;
    if (send_text(&session, restore) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/true\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "job=idle", NULL) == -1) {
        (void)fprintf(stderr, "pty resource: data recovery failed\n");
        return finish_session_directory(&session, fixture, 1);
    }
    (void)printf("pty resource: RLIMIT_DATA=%s\n",
           enforced ? "enforced" : "unsupported-or-unenforced");

    return finish_session_directory(&session, fixture, failed);
}

static int variable_journal_exhaustion_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-resource-journal-XXXXXX";
    char command[4096];
    size_t used;
    size_t index;
    pty_session session;
    int failed = 0;

    used = (size_t)snprintf(command, sizeof(command),
                            "if /usr/bin/true; then set -a;");
    for (index = 0; index < 65 && used < sizeof(command); index++) {
        int length = snprintf(command + used, sizeof(command) - used,
                              " GSH_RESOURCE_JOURNAL_%zu=x;", index);

        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            return 1;
        }
        used += (size_t)length;
    }
    if (snprintf(command + used, sizeof(command) - used, " fi\r") >=
        (int)(sizeof(command) - used)) {
        return 1;
    }
    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: variable journal setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "gsh: assignment:",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printenv GSH_RESOURCE_JOURNAL_0\r") == -1 ||
        consume_through(&session, "\nx\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s:%s>\\n' "
                  "\"$GSH_RESOURCE_JOURNAL_0\" "
                  "\"${GSH_RESOURCE_JOURNAL_64+set}\"\r") == -1 ||
        consume_through(&session, "<x:>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +a\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty resource: variable journal recovery failed\n");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int arithmetic_journal_exhaustion_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-resource-arithmetic-XXXXXX";
    char command[4096];
    size_t used;
    size_t index;
    pty_session session;
    int failed = 0;

    used = (size_t)snprintf(command, sizeof(command),
                            "if /usr/bin/true; then : \"$((");
    for (index = 0; index < 65 && used < sizeof(command); index++) {
        int length = snprintf(command + used, sizeof(command) - used,
                              "%sGSH_RESOURCE_ARITH_%zu=1",
                              index == 0 ? "" : ",", index);

        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            return 1;
        }
        used += (size_t)length;
    }
    if (snprintf(command + used, sizeof(command) - used,
                 "))\"; fi\r") >= (int)(sizeof(command) - used)) {
        return 1;
    }
    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: arithmetic journal setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "gsh: parameter assignment failed",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_RESOURCE_ARITH_RECOVERY=7; "
                  "/bin/test \"$GSH_RESOURCE_ARITH_RECOVERY\" = 7\r") ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        (void)fprintf(stderr,
                "pty resource: arithmetic journal recovery failed\n");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int alias_store_exhaustion_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-resource-alias-XXXXXX";
    char first[4096] = "alias";
    char second[2048] = "alias";
    size_t first_used = 5;
    size_t second_used = 5;
    size_t index;
    pty_session session;
    int failed = 0;

    for (index = 0; index < 129U; index++) {
        char *command = index < 100U ? first : second;
        size_t *used = index < 100U ? &first_used : &second_used;
        size_t capacity = index < 100U ? sizeof(first) : sizeof(second);
        int length = snprintf(command + *used, capacity - *used,
                              " gsh_resource_%03zu=x", index);

        if (length < 0 || (size_t)length >= capacity - *used) {
            return 1;
        }
        *used += (size_t)length;
    }
    first[first_used++] = '\r';
    first[first_used] = '\0';
    second[second_used++] = '\r';
    second[second_used] = '\0';
    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: alias setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, first) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, second) == -1 ||
        consume_through(&session, "alias capacity exceeded",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unalias -a\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_recovered=/usr/bin/true\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_recovered\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty resource: alias store recovery failed\n");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int alias_journal_exhaustion_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-resource-alias-journal-XXXXXX";
    char command[4096];
    size_t used;
    size_t index;
    pty_session session;
    int failed = 0;

    used = (size_t)snprintf(
        command, sizeof(command),
        "if /usr/bin/true; then alias gsh_atomic_keep=/usr/bin/false;");
    for (index = 0; index < 64U; index++) {
        int length = snprintf(command + used, sizeof(command) - used,
                              " alias gsh_atomic_%02zu=/usr/bin/true;",
                              index);

        if (length < 0 || (size_t)length >= sizeof(command) - used) {
            return 1;
        }
        used += (size_t)length;
    }
    if (snprintf(command + used, sizeof(command) - used, " fi\r") >=
        (int)(sizeof(command) - used)) {
        return 1;
    }
    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: alias journal setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_atomic_keep=/bin/echo\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "alias transaction capacity exceeded",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh: state transaction rejected",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_atomic_keep rolled-back\r") == -1 ||
        consume_through(&session, "\nrolled-back\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_atomic_00\r") == -1 ||
        consume_through(&session, "alias is not defined", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_atomic_recovery=/usr/bin/true\r") ==
            -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_atomic_recovery\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        (void)fprintf(stderr, "pty resource: alias journal rollback failed\n");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int finish_function_resource(pty_session *session,
                                    const char *fixture, int failed)
{
    if (fixture == NULL || session == NULL) {
        return -1;
    }
    if (failed) {
        (void)fprintf(stderr, "pty resource: function recovery failed\n");
        dump_capture(session);
    }
    return finish_session_directory(session, fixture, failed);
}

static int function_resource_exhaustion_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-resource-function-XXXXXX";
    char command[4096];
    size_t used;
    size_t index;
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: function setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return finish_function_resource(&session, fixture, 1);
    }
    for (index = 0; index < 128U; index++) {
        if (snprintf(command, sizeof(command),
                     "gsh_resource_fn_%03zu() { :; }\r", index) >=
                (int)sizeof(command) ||
            send_text(&session, command) == -1 ||
            consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
            return finish_function_resource(&session, fixture, 1);
        }
    }
    if (send_text(&session, "gsh_resource_fn_over() { :; }\r") == -1 ||
        consume_through(&session, "function definition: No space",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return finish_function_resource(&session, fixture, 1);
    }
    for (index = 0; index < 2U; index++) {
        size_t name;

        used = (size_t)snprintf(command, sizeof(command), "unset -f");
        for (name = index * 64U; name < (index + 1U) * 64U; name++) {
            int length = snprintf(command + used, sizeof(command) - used,
                                  " gsh_resource_fn_%03zu", name);

            if (length < 0 || (size_t)length >= sizeof(command) - used) {
                return finish_function_resource(&session, fixture, 1);
            }
            used += (size_t)length;
        }
        command[used++] = '\r';
        command[used] = '\0';
        if (send_text(&session, command) == -1 ||
            consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
            return finish_function_resource(&session, fixture, 1);
        }
    }
    if (send_text(&session, "gsh_resource_recovered() { :; }\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "gsh_resource_recursive() { "
                  "gsh_resource_recursive; }\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_resource_recursive\r") == -1 ||
        consume_through(&session, "function resource limit exceeded",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_resource_recovered\r") == -1 ||
        consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        failed = 1;
    }

    return finish_function_resource(&session, fixture, failed);
}

static int resource_exhaustion_flow(const char *executable)
{
    if (executable == NULL) {
        return -1;
    }
    int failed = 0;

    failed |= descriptor_exhaustion_case(executable);
    failed |= bounded_input_case(executable);
    failed |= initialization_exhaustion_case(executable);
    failed |= process_exhaustion_case(executable);
    failed |= data_exhaustion_case(executable);
    failed |= variable_journal_exhaustion_case(executable);
    failed |= arithmetic_journal_exhaustion_case(executable);
    failed |= alias_store_exhaustion_case(executable);
    failed |= alias_journal_exhaustion_case(executable);
    failed |= function_resource_exhaustion_case(executable);
    if (!failed) {
        (void)puts("pty resource: descriptor, input, process, data, variable, "
             "arithmetic, alias, and function limits recovered");
    }
    return failed;
}

static int process_fd_count(pid_t pid)
{
#if defined(__APPLE__)
    struct proc_fdinfo descriptors[256];
    int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, descriptors,
                             (int)sizeof(descriptors));

    return bytes < 0 ? -1 : bytes / (int)sizeof(descriptors[0]);
#elif defined(__linux__)
    char path[64];
    struct dirent *entry;
    DIR *directory;
    int count = 0;

    if (snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid) >=
        (int)sizeof(path)) {
        return -1;
    }
    directory = opendir(path);
    if (directory == NULL) {
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
            count++;
        }
    }
    (void)closedir(directory);
    return count;
#else
    (void)pid;
    return -1;
#endif
}

static int process_child_pids(pid_t pid, pid_t *children, size_t capacity)
{
#if defined(__APPLE__)
    int count;

    if (capacity > (size_t)INT_MAX / sizeof(*children)) {
        capacity = (size_t)INT_MAX / sizeof(*children);
    }
    count = proc_listchildpids(
        pid, children, (int)(capacity * sizeof(*children)));
    return count < 0 ? -1 : count;
#elif defined(__linux__)
    char path[96];
    char contents[4096];
    ssize_t length;
    size_t offset = 0;
    int descriptor;
    size_t count = 0;

    if (snprintf(path, sizeof(path), "/proc/%ld/task/%ld/children",
                 (long)pid, (long)pid) >= (int)sizeof(path)) {
        return -1;
    }
    descriptor = open(path, O_RDONLY);
    if (descriptor == -1) {
        return -1;
    }
    length = read(descriptor, contents, sizeof(contents) - 1U);
    (void)close(descriptor);
    if (length < 0) {
        return -1;
    }
    contents[(size_t)length] = '\0';
    while (offset < (size_t)length) {
        char *end;

        while (offset < (size_t)length &&
               (contents[offset] == ' ' || contents[offset] == '\n' ||
                contents[offset] == '\t')) {
            offset++;
        }
        if (offset == (size_t)length) {
            break;
        }
        errno = 0;
        {
            long value = strtol(contents + offset, &end, 10);

            if (errno != 0 || end == contents + offset || value <= 0 ||
                value > INT_MAX || count == capacity) {
                return -1;
            }
            children[count++] = (pid_t)value;
        }
        offset = (size_t)(end - contents);
    }
    return (int)count;
#else
    (void)pid;
    (void)children;
    (void)capacity;
    return -1;
#endif
}

static int process_child_count(pid_t pid)
{
    pid_t children[256];

    return process_child_pids(
        pid, children, sizeof(children) / sizeof(children[0]));
}

static int64_t process_rss_bytes(pid_t pid)
{
#if defined(__APPLE__)
    struct proc_taskinfo information;
    int bytes = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &information,
                             (int)sizeof(information));

    return bytes == (int)sizeof(information)
               ? (int64_t)information.pti_resident_size
               : -1;
#elif defined(__linux__)
    char path[64];
    char contents[128];
    char *cursor;
    char *end;
    long pages;
    long page_size;
    ssize_t count;
    int fd;

    if (snprintf(path, sizeof(path), "/proc/%ld/statm", (long)pid) >=
        (int)sizeof(path)) {
        return -1;
    }
    fd = open(path, O_RDONLY);
    if (fd == -1) {
        return -1;
    }
    count = read(fd, contents, sizeof(contents) - 1U);
    (void)close(fd);
    if (count <= 0) {
        return -1;
    }
    contents[(size_t)count] = '\0';
    cursor = contents;
    errno = 0;
    (void)strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE) {
        return -1;
    }
    cursor = end;
    while (*cursor == ' ' || *cursor == '\t') {
        cursor++;
    }
    errno = 0;
    pages = strtol(cursor, &end, 10);
    if (end == cursor || errno == ERANGE || pages < 0) {
        return -1;
    }
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 ||
        (uint64_t)pages > (uint64_t)INT64_MAX / (uint64_t)page_size) {
        return -1;
    }
    return (int64_t)pages * page_size;
#else
    (void)pid;
    return -1;
#endif
}

typedef struct {
    uint64_t current;
    uint64_t peak;
} process_memory;

static int process_memory_bytes(pid_t pid, process_memory *memory)
{
    if (memory == NULL) {
        return -1;
    }
#if defined(__APPLE__)
    struct rusage_info_v4 information;

    if (proc_pid_rusage(pid, RUSAGE_INFO_V4,
                        (rusage_info_t *)&information) == -1) {
        return -1;
    }
    memory->current = information.ri_phys_footprint;
    memory->peak = information.ri_lifetime_max_phys_footprint;
    return 0;
#elif defined(__linux__)
    char path[64];
    char contents[8192];
    char *current;
    char *peak;
    unsigned long long current_kb;
    unsigned long long peak_kb;
    ssize_t count;
    int descriptor;

    if (snprintf(path, sizeof(path), "/proc/%ld/status", (long)pid) >=
        (int)sizeof(path)) {
        return -1;
    }
    descriptor = open(path, O_RDONLY);
    if (descriptor == -1) {
        return -1;
    }
    count = read(descriptor, contents, sizeof(contents) - 1U);
    (void)close(descriptor);
    if (count <= 0) {
        return -1;
    }
    contents[(size_t)count] = '\0';
    current = strstr(contents, "VmRSS:");
    peak = strstr(contents, "VmHWM:");
    if (current == NULL || peak == NULL ||
        sscanf(current, "VmRSS: %llu kB", &current_kb) != 1 ||
        sscanf(peak, "VmHWM: %llu kB", &peak_kb) != 1 ||
        current_kb > UINT64_MAX / 1024U ||
        peak_kb > UINT64_MAX / 1024U) {
        return -1;
    }
    memory->current = (uint64_t)current_kb * 1024U;
    memory->peak = (uint64_t)peak_kb * 1024U;
    return 0;
#else
    int64_t current = process_rss_bytes(pid);

    if (current < 0) {
        return -1;
    }
    memory->current = (uint64_t)current;
    memory->peak = memory->current;
    return 0;
#endif
}

static int64_t process_tree_memory_bytes(pid_t pid)
{
    enum { PROCESS_TREE_CAP = 2048, PROCESS_TREE_DEPTH_CAP = 8 };
    pid_t pending[PROCESS_TREE_CAP];
    unsigned char depths[PROCESS_TREE_CAP];
    size_t read_index = 0U;
    size_t write_index = 1U;
    int64_t total = 0;
    size_t steps;

    pending[0] = pid;
    depths[0] = 0U;
    for (steps = 0; read_index < write_index && steps < PROCESS_TREE_CAP;
         steps++) {
        process_memory memory;
        pid_t children[256];
        unsigned int depth = depths[read_index];
        pid_t current = pending[read_index++];
        int child_count;
        int index;

        if (depth == PROCESS_TREE_DEPTH_CAP ||
            process_memory_bytes(current, &memory) == -1 ||
            memory.current > INT64_MAX) {
            if (depth == 0U) {
                return -1;
            }
            continue;
        }
        if ((int64_t)memory.current > INT64_MAX - total) {
            errno = EOVERFLOW;
            return -1;
        }
        total += (int64_t)memory.current;
        child_count = process_child_pids(
            current, children, sizeof(children) / sizeof(children[0]));
        if (child_count < 0) {
            if (depth == 0U) {
                return -1;
            }
            continue;
        }
        if ((size_t)child_count > PROCESS_TREE_CAP - write_index) {
            errno = ENOSPC;
            return -1;
        }
        for (index = 0; index < child_count; index++) {
            pending[write_index] = children[index];
            depths[write_index] = (unsigned char)(depth + 1U);
            write_index++;
        }
    }
    if (read_index != write_index) {
        errno = ENOSPC;
        return -1;
    }
    return total;
}

static int soak_iteration(pty_session *session)
{
    if (send_text(session, "edit") == -1 ||
        consume_through(session, "edit", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/usr/bin/true\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "/usr/bin/printf GSH_SOAK_OUTPUT | /bin/cat\r") == -1 ||
        consume_through(session, "\nGSH_SOAK_OUTPUT", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "GSH_SOAK_STATE=value\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "alias GSH_SOAK_ALIAS=:\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "GSH_SOAK_ALIAS\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "unalias GSH_SOAK_ALIAS\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "GSH_SOAK_ARITH=010; "
                  ": \"$((GSH_SOAK_ARITH += 1))\"; "
                  "/bin/test \"$GSH_SOAK_ARITH\" = 9\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  ": \"${GSH_SOAK_PIPE:=stage}\" | /usr/bin/true\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "/bin/test -z \"$GSH_SOAK_PIPE\" && "
                  "/usr/bin/printf GSH_SOAK_ISOLATED\r") == -1 ||
        consume_through(session, "\nGSH_SOAK_ISOLATED",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  ": \"$(/usr/bin/printf GSH_SOAK_SUBSTITUTION)\"\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/usr/bin/true &\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "wait \"$!\"\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "for GSH_SOAK_ITEM in a 'b c' ''; do :; done\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "set -- a 'b c' ''; shift\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "set -aCfu; GSH_SOAK_AUTO=value; "
                  "/usr/bin/printenv GSH_SOAK_AUTO >/dev/null; "
                  ": \"${GSH_SOAK_KNOWN:=known}\"; set +aCfu\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/bin/cat <<EOF\r") == -1 ||
        consume_through(session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "GSH_SOAK_HEREDOC\r") == -1 ||
        consume_through(session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "EOF\r") == -1 ||
        consume_through(session, "\nGSH_SOAK_HEREDOC\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "cancel") == -1 ||
        consume_through(session, "cancel", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\003", 1) == -1 ||
        consume_through(session, "^C", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "kept") == -1 ||
        consume_through(session, "kept", TEST_TIMEOUT_MS) == -1 ||
        kill(session->pid, SIGWINCH) == -1 ||
        consume_through(session, "\033[2K", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "kept", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "cd .\r") == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(session, "busy=0", "job=idle") == -1) {
        return -1;
    }
    return 0;
}

typedef struct {
    int64_t initial_rss;
    int64_t maximum_rss;
    int64_t final_rss;
    int initial_fds;
    int maximum_fds;
    int final_fds;
    int initial_children;
    int maximum_children;
    int final_children;
} soak_metrics;

static int initialize_soak_metrics(const pty_session *session,
                                   soak_metrics *metrics)
{
    if (!require(session != NULL) || !require(metrics != NULL)) {
        return -1;
    }
    metrics->initial_rss = process_rss_bytes(session->pid);
    metrics->initial_fds = process_fd_count(session->pid);
    metrics->initial_children = process_child_count(session->pid);
    if (metrics->initial_rss < 0 || metrics->initial_fds < 0 ||
        metrics->initial_children < 0) {
        return -1;
    }
    metrics->maximum_rss = metrics->initial_rss;
    metrics->maximum_fds = metrics->initial_fds;
    metrics->maximum_children = metrics->initial_children;
    return 0;
}

static int sample_soak_metrics(const pty_session *session,
                               soak_metrics *metrics)
{
    if (!require(session != NULL) || !require(metrics != NULL)) {
        return -1;
    }
    int64_t rss = process_rss_bytes(session->pid);
    int descriptors = process_fd_count(session->pid);
    int children = process_child_count(session->pid);

    if (rss < 0 || descriptors < 0 || children < 0) {
        return -1;
    }
    if (rss > metrics->maximum_rss) {
        metrics->maximum_rss = rss;
    }
    if (descriptors > metrics->maximum_fds) {
        metrics->maximum_fds = descriptors;
    }
    if (children > metrics->maximum_children) {
        metrics->maximum_children = children;
    }
    return 0;
}

static int run_soak_iterations(pty_session *session, unsigned long seconds,
                               soak_metrics *metrics, uint64_t *iterations)
{
    if (!require(session != NULL) || !require(metrics != NULL) ||
        !require(iterations != NULL) || !require(seconds <= 86400U)) {
        return -1;
    }
    uint64_t deadline = monotonic_ns() + (uint64_t)seconds * 1000000000ULL;

    while (monotonic_ns() < deadline) {
        if (soak_iteration(session) == -1) {
            perror("pty soak: iteration");
            dump_capture(session);
            return -1;
        }
        (*iterations)++;
        if (sample_soak_metrics(session, metrics) == -1) {
            return -1;
        }
    }
    return 0;
}

static int read_soak_diagnostics(pty_session *session,
                                 unsigned long *reactor_cycles,
                                 unsigned long *misses)
{
    bool found;

    if (!require(session != NULL) || !require(reactor_cycles != NULL) ||
        !require(misses != NULL)) {
        return -1;
    }
    session->capture_length = 0;
    if (send_text(session, "rt\r") == -1 ||
        wait_for_output(session, "reactor cycles=", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    *reactor_cycles = diagnostic_counter(session, "cycles=", &found);
    if (!found) {
        return -1;
    }
    *misses = diagnostic_counter(session, "misses=", &found);
    return found ? 0 : -1;
}

static int finish_soak_metrics(const pty_session *session,
                               soak_metrics *metrics)
{
    if (!require(session != NULL) || !require(metrics != NULL)) {
        return -1;
    }
    metrics->final_rss = process_rss_bytes(session->pid);
    metrics->final_fds = process_fd_count(session->pid);
    metrics->final_children = process_child_count(session->pid);
    if (metrics->final_rss < 0 || metrics->final_fds < 0 ||
        metrics->final_fds > metrics->initial_fds + 1 ||
        metrics->final_rss > metrics->initial_rss + 4 * 1024 * 1024 ||
        metrics->final_children < 0 ||
        metrics->final_children > metrics->initial_children) {
        return -1;
    }
    return 0;
}

static void report_soak(unsigned long seconds, uint64_t iterations,
                        unsigned long reactor_cycles, unsigned long misses,
                        const soak_metrics *metrics)
{
    if (!require(metrics != NULL) || !require(seconds > 0U)) {
        return;
    }
    (void)printf("pty soak: seconds=%lu iterations=%llu reactor_cycles=%lu "
           "misses=%lu rss_initial=%lld rss_max=%lld rss_final=%lld "
           "fds_initial=%d fds_max=%d fds_final=%d "
           "children_initial=%d children_max=%d children_final=%d\n",
           seconds, (unsigned long long)iterations, reactor_cycles, misses,
           (long long)metrics->initial_rss, (long long)metrics->maximum_rss,
           (long long)metrics->final_rss, metrics->initial_fds,
           metrics->maximum_fds, metrics->final_fds,
           metrics->initial_children, metrics->maximum_children,
           metrics->final_children);
}

static int soak_flow(const char *executable, unsigned long seconds)
{
    char fixture[] = "/tmp/gsh-soak-XXXXXX";
    pty_session session;
    uint64_t iterations = 0;
    unsigned long reactor_cycles;
    unsigned long misses;
    soak_metrics metrics;

    if (seconds == 0 || seconds > 86400 ||
        unsetenv("GSH_SOAK_STATE") == -1 ||
        unsetenv("GSH_SOAK_ARITH") == -1 ||
        unsetenv("GSH_SOAK_PIPE") == -1 || mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty soak: setup");
        remove_fixture(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "busy=0", "job=idle") == -1) {
        perror("pty soak: initial prompt");
        return finish_session_tree(&session, fixture, 1);
    }
    if (soak_iteration(&session) == -1) {
        perror("pty soak: warm-up iteration");
        dump_capture(&session);
        return finish_session_tree(&session, fixture, 1);
    }

    if (initialize_soak_metrics(&session, &metrics) == -1) {
        (void)fprintf(stderr, "pty soak: platform process metrics unavailable\n");
        return finish_session_tree(&session, fixture, 1);
    }
    if (run_soak_iterations(&session, seconds, &metrics, &iterations) == -1 ||
        read_soak_diagnostics(&session, &reactor_cycles, &misses) == -1) {
        return finish_session_tree(&session, fixture, 1);
    }
    if (finish_soak_metrics(&session, &metrics) == -1) {
        (void)fprintf(stderr,
                "pty soak: resource growth exceeded bounds "
                "rss_initial=%lld rss_max=%lld rss_final=%lld "
                "fds_initial=%d fds_max=%d fds_final=%d "
                "children_initial=%d children_max=%d children_final=%d\n",
                (long long)metrics.initial_rss,
                (long long)metrics.maximum_rss,
                (long long)metrics.final_rss, metrics.initial_fds,
                metrics.maximum_fds, metrics.final_fds,
                metrics.initial_children, metrics.maximum_children,
                metrics.final_children);
        return finish_session_tree(&session, fixture, 1);
    }
    report_soak(seconds, iterations, reactor_cycles, misses, &metrics);
    return finish_session_tree(&session, fixture, 0);
}

static void discard_ready_output(pty_session *session)
{
    if (session == NULL) {
        return;
    }
    unsigned char discard[4096];

    session->capture_length = 0;
    for (size_t attempt = 0; attempt < PTY_DRAIN_ATTEMPT_CAP; attempt++) {
        ssize_t count = read(session->master, discard, sizeof(discard));

        if (count > 0 || (count == -1 && errno == EINTR)) {
            continue;
        }
        break;
    }
}

static int stabilize_gsh(pty_session *session)
{
    uint64_t deadline = monotonic_ns() + 2000000000ULL;

    while (monotonic_ns() < deadline) {
        const unsigned char *busy;
        bool idle;

        if (send_text(session, "rt\r") == -1 ||
            wait_for_output(session, "busy=", TEST_TIMEOUT_MS) == -1 ||
            wait_for_output(session, " failures=", TEST_TIMEOUT_MS) == -1) {
            return -1;
        }
        busy = find_bytes(session->capture, session->capture_length, "busy=");
        idle = busy != NULL &&
               (size_t)(busy - session->capture) + sizeof("busy=") - 1U <
                   session->capture_length &&
               busy[sizeof("busy=") - 1U] == '0';
        if (consume_through(session, "busy=", TEST_TIMEOUT_MS) == -1 ||
            consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
            return -1;
        }
        if (idle) {
            discard_ready_output(session);
            return 0;
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

static int start_benchmark_session(pty_session *session,
                                   const shell_spec *spec,
                                   const char *directory, bool stabilize)
{
    if (spec == NULL) return -1;
    if (start_session(session, spec->executable, directory, spec->kind) == -1 ||
        consume_through(session, spec->prompt, TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    if (stabilize && spec->kind == SHELL_GSH &&
        stabilize_gsh(session) == -1) {
        return -1;
    }
    discard_ready_output(session);
    return 0;
}

static int benchmark_prompt_command(
    pty_session sessions[BENCH_SHELLS],
    const shell_spec specs[BENCH_SHELLS],
    uint64_t samples[BENCH_SHELLS][BENCH_EXEC_SAMPLES],
    const char *setup, const char *command, const char *label)
{
    if (specs == NULL) return -1;
    if (label == NULL || samples == NULL || sessions == NULL) {
        return -1;
    }
    size_t sample;
    size_t offset;

    for (sample = 0; sample < BENCH_EXEC_SAMPLES; sample++) {
        for (offset = 0; offset < BENCH_SHELLS; offset++) {
            size_t shell = (sample + offset) % BENCH_SHELLS;
            uint64_t start;

            discard_ready_output(&sessions[shell]);
            if (setup != NULL &&
                (send_text(&sessions[shell], setup) == -1 ||
                 consume_through(&sessions[shell], specs[shell].prompt,
                                 TEST_TIMEOUT_MS) == -1)) {
                (void)fprintf(stderr, "pty benchmark: %s %s setup failed\n",
                        specs[shell].name, label);
                return -1;
            }
            discard_ready_output(&sessions[shell]);
            start = monotonic_ns();
            if (send_text(&sessions[shell], command) == -1 ||
                consume_through(&sessions[shell], specs[shell].prompt,
                                TEST_TIMEOUT_MS) == -1) {
                (void)fprintf(stderr, "pty benchmark: %s %s sample failed\n",
                        specs[shell].name, label);
                return -1;
            }
            samples[shell][sample] = monotonic_ns() - start;
        }
    }
    return 0;
}

static int benchmark_synchronized_prompt_command(
    pty_session sessions[BENCH_SHELLS],
    const shell_spec specs[BENCH_SHELLS],
    uint64_t samples[BENCH_SHELLS][BENCH_EXEC_SAMPLES],
    const char *setup, const char *command, const char *label)
{
    if (specs == NULL) return -1;
    if (label == NULL || samples == NULL || sessions == NULL) {
        return -1;
    }
    static const char marker[] = "__GSH_BENCH_SYNC__";
    size_t sample;
    size_t offset;

    for (sample = 0; sample < BENCH_EXEC_SAMPLES; sample++) {
        for (offset = 0; offset < BENCH_SHELLS; offset++) {
            size_t shell = (sample + offset) % BENCH_SHELLS;
            uint64_t start;

            discard_ready_output(&sessions[shell]);
            if (send_text(&sessions[shell], setup) == -1 ||
                consume_through(&sessions[shell], marker,
                                TEST_TIMEOUT_MS) == -1 ||
                consume_through(&sessions[shell], specs[shell].prompt,
                                TEST_TIMEOUT_MS) == -1) {
                (void)fprintf(stderr, "pty benchmark: %s %s setup failed\n",
                        specs[shell].name, label);
                return -1;
            }
            discard_ready_output(&sessions[shell]);
            start = monotonic_ns();
            if (send_text(&sessions[shell], command) == -1 ||
                consume_through(&sessions[shell], specs[shell].prompt,
                                TEST_TIMEOUT_MS) == -1) {
                (void)fprintf(stderr, "pty benchmark: %s %s sample failed\n",
                        specs[shell].name, label);
                return -1;
            }
            samples[shell][sample] = monotonic_ns() - start;
        }
    }
    return 0;
}

static size_t paired_win_count(const uint64_t *candidate,
                               const uint64_t *peer, size_t count)
{
    if (candidate == NULL || peer == NULL) return 0U;
    size_t wins = 0;
    size_t index;

    for (index = 0; index < count; index++) {
        if (candidate[index] < peer[index]) wins++;
    }
    return wins;
}

static int direct_builtin_performance_gate(
    uint64_t echo_samples[BENCH_SHELLS][BENCH_EXEC_SAMPLES],
    uint64_t printf_samples[BENCH_SHELLS][BENCH_EXEC_SAMPLES],
    uint64_t test_samples[BENCH_SHELLS][BENCH_EXEC_SAMPLES],
    size_t *bash_wins_result, size_t *zsh_wins_result)
{
    if (echo_samples == NULL || printf_samples == NULL || test_samples == NULL) {
        return -1;
    }
    size_t bash_wins =
        paired_win_count(echo_samples[0], echo_samples[1],
                         BENCH_EXEC_SAMPLES) +
        paired_win_count(printf_samples[0], printf_samples[1],
                         BENCH_EXEC_SAMPLES) +
        paired_win_count(test_samples[0], test_samples[1],
                         BENCH_EXEC_SAMPLES);
    size_t zsh_wins =
        paired_win_count(echo_samples[0], echo_samples[2],
                         BENCH_EXEC_SAMPLES) +
        paired_win_count(printf_samples[0], printf_samples[2],
                         BENCH_EXEC_SAMPLES) +
        paired_win_count(test_samples[0], test_samples[2],
                         BENCH_EXEC_SAMPLES);
    size_t total = 3U * BENCH_EXEC_SAMPLES;

    if (bash_wins_result == NULL || zsh_wins_result == NULL) {
        errno = EINVAL;
        return -1;
    }
    *bash_wins_result = bash_wins;
    *zsh_wins_result = zsh_wins;
    if (bash_wins * 2U <= total || zsh_wins * 2U <= total) {
        (void)fprintf(stderr,
                "pty benchmark: direct builtins did not clear the >50%% "
                "paired-win gate\n");
        return -1;
    }
    return 0;
}

typedef struct {
    const char *label;
    const char *setup;
    const char *command;
    const char *held_command;
} memory_workload;

typedef struct {
    uint64_t idle[BENCH_SHELLS][BENCH_MEMORY_SAMPLES];
    uint64_t idle_tree[BENCH_SHELLS][BENCH_MEMORY_SAMPLES];
    uint64_t shell_peak[BENCH_MEMORY_WORKLOADS][BENCH_SHELLS]
                       [BENCH_MEMORY_SAMPLES];
    uint64_t shell_delta[BENCH_MEMORY_WORKLOADS][BENCH_SHELLS]
                        [BENCH_MEMORY_SAMPLES];
    uint64_t shell_growth[BENCH_MEMORY_WORKLOADS][BENCH_SHELLS]
                         [BENCH_MEMORY_SAMPLES];
    uint64_t tree_peak[BENCH_MEMORY_WORKLOADS][BENCH_SHELLS]
                      [BENCH_MEMORY_SAMPLES];
    uint64_t tree_delta[BENCH_MEMORY_WORKLOADS][BENCH_SHELLS]
                       [BENCH_MEMORY_SAMPLES];
} benchmark_memory;

static const memory_workload
    benchmark_memory_workloads[BENCH_MEMORY_WORKLOADS] = {
        {"external-true", NULL, "/usr/bin/true\r", "/bin/sleep 0.02\r"},
        {"variable-lookup", "GSH_BENCH_VALUE=value\r",
         ": \"${GSH_BENCH_VALUE}\"\r", NULL},
        {"variable-assignment", NULL, "GSH_BENCH_ASSIGN=value\r", NULL},
        {"parameter-assign-default", "GSH_BENCH_DEFAULT=\r",
         ": \"${GSH_BENCH_DEFAULT:=value}\"\r", NULL},
        {"arithmetic-assignment", "GSH_BENCH_ARITH=1\r",
         ": \"$((GSH_BENCH_ARITH += 1))\"\r", NULL},
        {"pipeline-scoped-assignment", "GSH_BENCH_PIPE=\r",
         ": \"${GSH_BENCH_PIPE:=value}\" | /usr/bin/true\r",
         ": \"${GSH_BENCH_PIPE:=value}\" | /bin/sleep 0.02\r"},
        {"ulimit-soft-nofile", NULL, "ulimit -S -n\r", NULL},
        {"umask-report", NULL, "umask\r", NULL},
        {"export-assignment", NULL, "export GSH_BENCH_EXPORT=value\r", NULL},
        {"unset-variable", "GSH_BENCH_UNSET=value\r",
         "unset GSH_BENCH_UNSET\r", NULL},
        {"readonly-existing", "readonly GSH_BENCH_READONLY\r",
         "readonly GSH_BENCH_READONLY\r", NULL},
        {"for-explicit", NULL,
         "for GSH_BENCH_ITEM in a b c; do :; done\r", NULL},
        {"set-positionals", NULL, "set -- a b c\r", NULL},
        {"shift-positionals", "set -- a b c\r", "shift\r", NULL},
        {"set-options", "set +aCfu\r", "set -aCfu\r", NULL},
        {"allexport-assignment", "set +Cfu -a\r",
         "GSH_BENCH_ASSIGN=value\r", NULL},
        {"nounset-defined-lookup", "set +aCf -u\r",
         ": \"${GSH_BENCH_VALUE}\"\r", NULL},
        {"parameter-pattern-removal", NULL,
         ": \"${GSH_BENCH_PATTERN##*b}\"\r", NULL},
        {"parameter-pattern-multistar", NULL,
         ": \"${GSH_BENCH_PATTERN_MULTI#*a*d}\"\r", NULL},
        {"noglob-expansion", "set +aCu -f\r",
         "/usr/bin/printf '' /dev/n[uo]ll\r", NULL},
        {"noclobber-nonregular-redirection", "set +afu -C\r",
         ": >/dev/null\r", NULL},
        {"async-external-held", NULL, "/bin/sleep 0.05 &\r", "wait\r"},
        {"wait-completed", "/usr/bin/true &\r", "wait\r", NULL},
        {"alias-definition", NULL, "alias GSH_BENCH_ALIAS=:\r", NULL},
        {"alias-expansion", "alias GSH_BENCH_ALIAS=:\r",
         "GSH_BENCH_ALIAS\r", NULL},
        {"unalias", "alias GSH_BENCH_ALIAS=:\r",
         "unalias GSH_BENCH_ALIAS\r", NULL},
};

static int wait_for_output_sampling_tree(pty_session *session,
                                         const char *marker,
                                         uint64_t *peak)
{
    if (peak == NULL) return -1;
    if (session == NULL) {
        return -1;
    }
    uint64_t deadline = monotonic_ns() +
                        (uint64_t)TEST_TIMEOUT_MS * 1000000ULL;

    while (monotonic_ns() < deadline) {
        int64_t current = process_tree_memory_bytes(session->pid);

        if (current >= 0 && (uint64_t)current > *peak) {
            *peak = (uint64_t)current;
        }
        if (wait_for_output(session, marker, 1) == 0) {
            return 0;
        }
        if (errno != ETIMEDOUT) {
            return -1;
        }
    }
    errno = ETIMEDOUT;
    return -1;
}

static int measure_idle_memory_sample(const shell_spec *spec,
                                      const char *directory,
                                      uint64_t *shell_current,
                                      uint64_t *tree_current)
{
    pty_session session;
    process_memory memory;
    int64_t tree;
    bool failed = false;

    if (spec == NULL || directory == NULL || shell_current == NULL ||
        tree_current == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (start_benchmark_session(&session, spec, directory, true) == -1) {
        return -1;
    }
    if (process_memory_bytes(session.pid, &memory) == -1) {
        failed = true;
    }
    tree = failed ? -1 : process_tree_memory_bytes(session.pid);
    if (tree < 0) {
        failed = true;
    } else {
        *shell_current = memory.current;
        *tree_current = (uint64_t)tree;
    }
    if (stop_session(&session) == -1) {
        failed = true;
    }
    return failed ? -1 : 0;
}

static int prepare_memory_sample(pty_session *session,
                                 const shell_spec *spec,
                                 const char *directory, size_t workload,
                                 process_memory *before,
                                 int64_t *before_tree)
{
    const memory_workload *definition;

    if (session == NULL || spec == NULL || directory == NULL ||
        before == NULL || before_tree == NULL ||
        workload >= BENCH_MEMORY_WORKLOADS) {
        errno = EINVAL;
        return -1;
    }
    definition = &benchmark_memory_workloads[workload];
    if (start_benchmark_session(session, spec, directory, true) == -1) {
        return -1;
    }
    if (definition->setup != NULL &&
        (send_text(session, definition->setup) == -1 ||
         consume_through(session, spec->prompt, TEST_TIMEOUT_MS) == -1)) {
        (void)stop_session(session);
        return -1;
    }
    discard_ready_output(session);
    if (process_memory_bytes(session->pid, before) == -1 ||
        (*before_tree = process_tree_memory_bytes(session->pid)) < 0) {
        (void)stop_session(session);
        return -1;
    }
    return 0;
}

static int execute_memory_sample(pty_session *session,
                                 const shell_spec *spec, size_t workload,
                                 uint64_t *tree_peak)
{
    const memory_workload *definition;

    if (session == NULL || spec == NULL || tree_peak == NULL ||
        workload >= BENCH_MEMORY_WORKLOADS) {
        errno = EINVAL;
        return -1;
    }
    definition = &benchmark_memory_workloads[workload];
    if (send_text(session, definition->command) == -1 ||
        consume_through(session, spec->prompt, TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    if (definition->held_command == NULL) {
        return 0;
    }
    discard_ready_output(session);
    return send_text(session, definition->held_command) == -1 ||
                   wait_for_output_sampling_tree(session, spec->prompt,
                                                 tree_peak) == -1
               ? -1
               : 0;
}

static int store_memory_sample(benchmark_memory *results, size_t workload,
                               size_t shell, size_t sample,
                               const process_memory *before,
                               const process_memory *after,
                               int64_t before_tree, uint64_t tree_peak)
{
    if (results == NULL || before == NULL || after == NULL ||
        workload >= BENCH_MEMORY_WORKLOADS || shell >= BENCH_SHELLS ||
        sample >= BENCH_MEMORY_SAMPLES || before_tree < 0) {
        errno = EINVAL;
        return -1;
    }
    results->shell_peak[workload][shell][sample] = after->peak;
    results->shell_delta[workload][shell][sample] =
        after->peak > before->current ? after->peak - before->current : 0;
    results->shell_growth[workload][shell][sample] =
        after->peak > before->peak ? after->peak - before->peak : 0;
    results->tree_peak[workload][shell][sample] = tree_peak;
    results->tree_delta[workload][shell][sample] =
        tree_peak > (uint64_t)before_tree
            ? tree_peak - (uint64_t)before_tree
            : 0;
    return 0;
}

static int measure_workload_memory_sample(
    const shell_spec *spec, const char *directory, size_t workload,
    size_t shell, size_t sample, benchmark_memory *results)
{
    pty_session session;
    process_memory before;
    process_memory after;
    int64_t before_tree;
    int64_t tree;
    uint64_t tree_peak;
    bool failed = false;

    if (prepare_memory_sample(&session, spec, directory, workload, &before,
                              &before_tree) == -1) {
        return -1;
    }
    tree_peak = (uint64_t)before_tree;
    if (execute_memory_sample(&session, spec, workload, &tree_peak) == -1 ||
        process_memory_bytes(session.pid, &after) == -1) {
        failed = true;
    }
    tree = failed ? -1 : process_tree_memory_bytes(session.pid);
    if (!failed && tree >= 0 && (uint64_t)tree > tree_peak) {
        tree_peak = (uint64_t)tree;
    }
    if (!failed &&
        store_memory_sample(results, workload, shell, sample, &before, &after,
                            before_tree, tree_peak) == -1) {
        failed = true;
    }
    if (stop_session(&session) == -1) {
        failed = true;
    }
    return failed ? -1 : 0;
}

static int memory_benchmark(const shell_spec specs[BENCH_SHELLS],
                            const char *directory, size_t workload_begin,
                            size_t workload_count,
                            benchmark_memory *results)
{
    if (specs == NULL) return -1;
    size_t workload;
    size_t sample;
    size_t offset;
    size_t workload_end;

    if (results == NULL || workload_begin > BENCH_MEMORY_WORKLOADS ||
        workload_count > BENCH_MEMORY_WORKLOADS - workload_begin) {
        return -1;
    }
    workload_end = workload_begin + workload_count;
    (void)memset(results, 0, sizeof(*results));
    for (sample = 0; sample < BENCH_MEMORY_SAMPLES; sample++) {
        for (offset = 0; offset < BENCH_SHELLS; offset++) {
            size_t shell = (sample + offset) % BENCH_SHELLS;

            if (measure_idle_memory_sample(
                    &specs[shell], directory, &results->idle[shell][sample],
                    &results->idle_tree[shell][sample]) == -1) {
                (void)fprintf(stderr, "pty benchmark: %s idle memory failed\n",
                        specs[shell].name);
                return -1;
            }
        }
    }
    for (workload = workload_begin; workload < BENCH_MEMORY_WORKLOADS &&
                                    workload < workload_end;
         workload++) {
        for (sample = 0; sample < BENCH_MEMORY_SAMPLES; sample++) {
            for (offset = 0; offset < BENCH_SHELLS; offset++) {
                size_t shell = (sample + offset) % BENCH_SHELLS;

                if (measure_workload_memory_sample(
                        &specs[shell], directory, workload, shell, sample,
                        results) == -1) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

typedef struct {
    const char *test;
    const uint64_t *samples;
    size_t sample_count;
} benchmark_latency_metric;

static const char *benchmark_shell_version(const shell_spec *spec)
{
    const char *version;

    if (spec == NULL) {
        return "unknown";
    }
    if (spec->kind == SHELL_BASH) {
        version = getenv("GSH_BENCH_BASH_VERSION");
    } else if (spec->kind == SHELL_ZSH) {
        version = getenv("GSH_BENCH_ZSH_VERSION");
    } else {
        version = getenv("GSH_BENCH_REVISION");
    }
    return version == NULL ? "unknown" : version;
}

static int write_latency_report(
    benchmark_report *report, const shell_spec specs[BENCH_SHELLS],
    const benchmark_latency_metric *metrics,
    size_t metric_count)
{
    if (specs == NULL) return -1;
    size_t metric;
    size_t shell;

    if (report == NULL || metrics == NULL || metric_count == 0 ||
        metric_count > BENCH_LATENCY_WORKLOADS) {
        errno = EINVAL;
        return -1;
    }
    for (metric = 0; metric < BENCH_LATENCY_WORKLOADS &&
                     metric < metric_count;
         metric++) {
        for (shell = 0; shell < BENCH_SHELLS; shell++) {
            const uint64_t *samples =
                metrics[metric].samples + shell * metrics[metric].sample_count;

            if (benchmark_report_write_metric(
                    report, "latency", metrics[metric].test,
                    specs[shell].name, benchmark_shell_version(&specs[shell]),
                    specs[shell].executable, "duration", "ns", samples,
                    metrics[metric].sample_count, 5000000ULL) == -1) {
                return -1;
            }
        }
    }
    return 0;
}

static int write_memory_metric(benchmark_report *report,
                               const shell_spec *spec, const char *test,
                               const char *measurement,
                               const uint64_t samples[BENCH_MEMORY_SAMPLES])
{
    if (measurement == NULL || test == NULL) {
        return -1;
    }
    if (report == NULL || spec == NULL || samples == NULL) {
        errno = EINVAL;
        return -1;
    }
    return benchmark_report_write_metric(
        report, "memory", test, spec->name, benchmark_shell_version(spec),
        spec->executable, measurement, "bytes", samples,
        BENCH_MEMORY_SAMPLES, 0);
}

static int write_memory_report(benchmark_report *report,
                               const shell_spec specs[BENCH_SHELLS],
                               const benchmark_memory *results,
                               size_t workload_begin, size_t workload_count)
{
    if (specs == NULL) return -1;
    size_t workload_end;
    size_t shell;
    size_t workload;

    if (report == NULL || results == NULL ||
        workload_begin > BENCH_MEMORY_WORKLOADS ||
        workload_count > BENCH_MEMORY_WORKLOADS - workload_begin) {
        errno = EINVAL;
        return -1;
    }
    workload_end = workload_begin + workload_count;
    for (shell = 0; shell < BENCH_SHELLS; shell++) {
        if (write_memory_metric(report, &specs[shell], "startup-idle",
                                "shell-current", results->idle[shell]) == -1 ||
            write_memory_metric(report, &specs[shell], "startup-idle",
                                "tree-current",
                                results->idle_tree[shell]) == -1) {
            return -1;
        }
        for (workload = workload_begin; workload < workload_end; workload++) {
            const char *test = benchmark_memory_workloads[workload].label;

            if (write_memory_metric(report, &specs[shell], test, "shell-peak",
                                    results->shell_peak[workload][shell]) ==
                    -1 ||
                write_memory_metric(
                    report, &specs[shell], test, "shell-delta-over-idle",
                    results->shell_delta[workload][shell]) == -1 ||
                write_memory_metric(
                    report, &specs[shell], test, "shell-peak-growth",
                    results->shell_growth[workload][shell]) == -1 ||
                write_memory_metric(report, &specs[shell], test, "tree-peak",
                                    results->tree_peak[workload][shell]) == -1 ||
                write_memory_metric(
                    report, &specs[shell], test, "tree-delta-over-idle",
                    results->tree_delta[workload][shell]) == -1) {
                return -1;
            }
        }
    }
    return 0;
}

static int finish_benchmark_sessions(
    pty_session sessions[BENCH_SHELLS], bool started[BENCH_SHELLS],
    int failed)
{
    if (started == NULL || sessions == NULL) return -1;
    size_t shell;

    for (shell = 0; shell < BENCH_SHELLS; shell++) {
        if (started[shell] && stop_session(&sessions[shell]) == -1) {
            failed = 1;
        }
        started[shell] = false;
    }
    return failed;
}

typedef struct {
    uint64_t startup[BENCH_SHELLS][BENCH_STARTUP_SAMPLES];
    uint64_t key[BENCH_SHELLS][BENCH_KEY_SAMPLES];
    uint64_t execution[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t direct_echo[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t direct_printf[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t direct_test[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t lookup[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t command_lookup[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t command_path_cache[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t assignment[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t assign_default[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t arithmetic_assignment[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t pipeline_assignment[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t resource_limit[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t creation_mask[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t process_times[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t exec_descriptor[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t export_assignment[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t unset_variable[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t readonly_existing[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t for_explicit[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t set_positionals[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t shift_positionals[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t set_options[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t allexport_assignment[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t nounset_lookup[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t pattern_removal[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t pattern_multistar[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t pathname_expansion[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t noglob_expansion[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t noclobber_redirection[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t async_launch[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t async_external_launch[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t wait_completed[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t alias_definition[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t alias_expansion[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t unalias_command[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
} benchmark_latency;

static int prepare_benchmark_shells(const shell_spec specs[BENCH_SHELLS],
                                    const char *directory)
{
    size_t shell;

    if (!require(specs != NULL) || !require(directory != NULL)) {
        return -1;
    }
    for (shell = 0; shell < BENCH_SHELLS; shell++) {
        size_t sample;

        if (access(specs[shell].executable, X_OK) == -1) {
            (void)fprintf(stderr, "pty benchmark: %s is not executable: %s\n",
                    specs[shell].name, specs[shell].executable);
            return -1;
        }
        for (sample = 0; sample < 8U; sample++) {
            pty_session warm;

            if (start_benchmark_session(&warm, &specs[shell], directory,
                                        false) == -1 ||
                stop_session(&warm) == -1) {
                (void)fprintf(stderr, "pty benchmark: %s warmup failed\n",
                        specs[shell].name);
                return -1;
            }
        }
    }
    return 0;
}

static int measure_benchmark_startup(const shell_spec specs[BENCH_SHELLS],
                                     const char *directory,
                                     benchmark_latency *latency)
{
    size_t sample;

    if (!require(specs != NULL) || !require(directory != NULL) ||
        !require(latency != NULL)) {
        return -1;
    }
    for (sample = 0; sample < BENCH_STARTUP_SAMPLES; sample++) {
        size_t offset;

        for (offset = 0; offset < BENCH_SHELLS; offset++) {
            size_t shell = (sample + offset) % BENCH_SHELLS;
            pty_session fresh;
            uint64_t start = monotonic_ns();

            if (start_benchmark_session(&fresh, &specs[shell], directory,
                                        false) == -1) {
                return -1;
            }
            latency->startup[shell][sample] = monotonic_ns() - start;
            if (stop_session(&fresh) == -1) {
                return -1;
            }
        }
    }
    return 0;
}

static int open_benchmark_sessions(pty_session sessions[BENCH_SHELLS],
                                   bool started[BENCH_SHELLS],
                                   const shell_spec specs[BENCH_SHELLS],
                                   const char *directory)
{
    size_t shell;

    if (!require(sessions != NULL) || !require(started != NULL) ||
        !require(specs != NULL) || !require(directory != NULL)) {
        return -1;
    }
    for (shell = 0; shell < BENCH_SHELLS; shell++) {
        (void)memset(&sessions[shell], 0, sizeof(sessions[shell]));
        sessions[shell].master = -1;
        if (start_benchmark_session(&sessions[shell], &specs[shell],
                                    directory, true) == -1) {
            return -1;
        }
        started[shell] = true;
    }
    return 0;
}

static int warm_benchmark_builtins(pty_session sessions[BENCH_SHELLS],
                                   const shell_spec specs[BENCH_SHELLS])
{
    size_t sample;

    if (!require(sessions != NULL) || !require(specs != NULL)) {
        return -1;
    }
    for (sample = 0; sample < 8U; sample++) {
        size_t shell;

        for (shell = 0; shell < BENCH_SHELLS; shell++) {
            if (send_text(&sessions[shell],
                          "echo GSH_BUILTIN\rprintf '%s\\n' "
                          "GSH_BUILTIN\rtest x = x\r") == -1 ||
                consume_through(&sessions[shell], specs[shell].prompt,
                                TEST_TIMEOUT_MS) == -1 ||
                consume_through(&sessions[shell], specs[shell].prompt,
                                TEST_TIMEOUT_MS) == -1 ||
                consume_through(&sessions[shell], specs[shell].prompt,
                                TEST_TIMEOUT_MS) == -1) {
                return -1;
            }
            discard_ready_output(&sessions[shell]);
        }
    }
    return 0;
}

static int measure_benchmark_keys(pty_session sessions[BENCH_SHELLS],
                                  benchmark_latency *latency)
{
    size_t sample;

    if (!require(sessions != NULL) || !require(latency != NULL)) {
        return -1;
    }
    for (sample = 0; sample < BENCH_KEY_SAMPLES; sample++) {
        size_t offset;

        for (offset = 0; offset < BENCH_SHELLS; offset++) {
            size_t shell = (sample + offset) % BENCH_SHELLS;
            uint64_t start;

            discard_ready_output(&sessions[shell]);
            start = monotonic_ns();
            if (send_text(&sessions[shell], "x") == -1 ||
                consume_through(&sessions[shell], "x", TEST_TIMEOUT_MS) ==
                    -1) {
                return -1;
            }
            latency->key[shell][sample] = monotonic_ns() - start;
            if (send_bytes(&sessions[shell], "\025", 1) == -1) {
                return -1;
            }
        }
    }
    return 0;
}

static int benchmark_command_group_one(
    pty_session sessions[BENCH_SHELLS],
    const shell_spec specs[BENCH_SHELLS], benchmark_latency *latency)
{
    if (!require(sessions != NULL) || !require(specs != NULL) ||
        !require(latency != NULL)) {
        return -1;
    }
    if (benchmark_prompt_command(sessions, specs, latency->execution, NULL,
                                 "/usr/bin/true\r", "external true") == -1 ||
        benchmark_prompt_command(sessions, specs, latency->direct_echo, NULL,
                                 "echo GSH_BUILTIN\r",
                                 "direct echo builtin") == -1 ||
        benchmark_prompt_command(sessions, specs, latency->direct_printf, NULL,
                                 "printf '%s\\n' GSH_BUILTIN\r",
                                 "direct printf builtin") == -1 ||
        benchmark_prompt_command(sessions, specs, latency->direct_test, NULL,
                                 "test x = x\r", "direct test builtin") ==
            -1 ||
        benchmark_prompt_command(sessions, specs, latency->lookup,
                                 "GSH_BENCH_VALUE=value\r",
                                 ": \"${GSH_BENCH_VALUE}\"\r",
                                 "variable lookup") == -1 ||
        benchmark_prompt_command(sessions, specs, latency->command_lookup,
                                 NULL, "command -v true\r",
                                 "command lookup") == -1 ||
        benchmark_prompt_command(sessions, specs,
                                 latency->command_path_cache, "hash sh\r",
                                 "command -v sh\r",
                                 "cached command path lookup") == -1 ||
        benchmark_prompt_command(sessions, specs, latency->assignment, NULL,
                                 "GSH_BENCH_ASSIGN=value\r",
                                 "variable assignment") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->assign_default,
            "GSH_BENCH_DEFAULT=\r", ": \"${GSH_BENCH_DEFAULT:=value}\"\r",
            "assign-default expansion") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->arithmetic_assignment,
            "GSH_BENCH_ARITH=1\r", ": \"$((GSH_BENCH_ARITH += 1))\"\r",
            "arithmetic assignment") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->pipeline_assignment,
            "GSH_BENCH_PIPE=\r",
            ": \"${GSH_BENCH_PIPE:=value}\" | /usr/bin/true\r",
            "pipeline scoped assignment") == -1) {
        return -1;
    }
    return 0;
}

static int benchmark_command_group_two(
    pty_session sessions[BENCH_SHELLS],
    const shell_spec specs[BENCH_SHELLS], benchmark_latency *latency)
{
    if (!require(sessions != NULL) || !require(specs != NULL) ||
        !require(latency != NULL)) {
        return -1;
    }
    if (benchmark_prompt_command(sessions, specs, latency->resource_limit,
                                 NULL, "ulimit -S -n\r",
                                 "ulimit soft nofile") == -1 ||
        benchmark_prompt_command(sessions, specs, latency->creation_mask,
                                 NULL, "umask\r", "umask report") == -1 ||
        benchmark_prompt_command(sessions, specs, latency->process_times,
                                 NULL, "times\r", "process times") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->exec_descriptor, "exec 9>&-\r",
            "exec 9>/dev/null\r", "exec descriptor commit") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->export_assignment, NULL,
            "export GSH_BENCH_EXPORT=value\r", "export assignment") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->unset_variable,
            "GSH_BENCH_UNSET=value\r", "unset GSH_BENCH_UNSET\r",
            "unset variable") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->readonly_existing,
            "readonly GSH_BENCH_READONLY\r",
            "readonly GSH_BENCH_READONLY\r", "readonly existing") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->for_explicit, NULL,
            "for GSH_BENCH_ITEM in a b c; do :; done\r",
            "for explicit items") == -1 ||
        benchmark_prompt_command(sessions, specs, latency->set_positionals,
                                 NULL, "set -- a b c\r",
                                 "set positional parameters") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->shift_positionals, "set -- a b c\r",
            "shift\r", "shift positional parameters") == -1 ||
        benchmark_prompt_command(sessions, specs, latency->set_options,
                                 "set +aCfu\r", "set -aCfu\r",
                                 "set shell options") == -1) {
        return -1;
    }
    return 0;
}

static int benchmark_command_group_three(
    pty_session sessions[BENCH_SHELLS],
    const shell_spec specs[BENCH_SHELLS], benchmark_latency *latency)
{
    if (!require(sessions != NULL) || !require(specs != NULL) ||
        !require(latency != NULL)) {
        return -1;
    }
    if (benchmark_prompt_command(
            sessions, specs, latency->allexport_assignment, "set +Cfu -a\r",
            "GSH_BENCH_ASSIGN=value\r", "allexport assignment") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->nounset_lookup, "set +aCf -u\r",
            ": \"${GSH_BENCH_VALUE}\"\r", "nounset defined lookup") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->pattern_removal, NULL,
            ": \"${GSH_BENCH_PATTERN##*b}\"\r",
            "parameter pattern removal") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->pattern_multistar, NULL,
            ": \"${GSH_BENCH_PATTERN_MULTI#*a*d}\"\r",
            "parameter multi-star removal") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->pathname_expansion, "set +aCuf\r",
            ": /dev/n[uo]ll\r", "pathname expansion") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->noglob_expansion, "set +aCu -f\r",
            "/usr/bin/printf '' /dev/n[uo]ll\r", "noglob expansion") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->noclobber_redirection, "set +afu -C\r",
            ": >/dev/null\r", "noclobber nonregular redirection") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->alias_definition,
            "alias GSH_BENCH_ALIAS=:\r",
            "alias GSH_BENCH_ALIAS=true\r", "alias definition update") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->alias_expansion,
            "alias GSH_BENCH_ALIAS=:\r", "GSH_BENCH_ALIAS\r",
            "alias lookup expansion") == -1 ||
        benchmark_prompt_command(
            sessions, specs, latency->unalias_command,
            "alias GSH_BENCH_ALIAS=:\r", "unalias GSH_BENCH_ALIAS\r",
            "unalias definition") == -1) {
        return -1;
    }
    return 0;
}

static int restart_benchmark_sessions(
    pty_session sessions[BENCH_SHELLS], bool started[BENCH_SHELLS],
    const shell_spec specs[BENCH_SHELLS], const char *directory)
{
    size_t shell;

    if (!require(sessions != NULL) || !require(started != NULL) ||
        !require(specs != NULL) || !require(directory != NULL)) {
        return -1;
    }
    for (shell = 0; shell < BENCH_SHELLS; shell++) {
        if (stop_session(&sessions[shell]) == -1) {
            return -1;
        }
        started[shell] = false;
        if (start_benchmark_session(&sessions[shell], &specs[shell],
                                    directory, true) == -1) {
            return -1;
        }
        started[shell] = true;
    }
    return 0;
}

static int benchmark_async_commands(
    pty_session sessions[BENCH_SHELLS],
    const shell_spec specs[BENCH_SHELLS], benchmark_latency *latency)
{
    if (!require(sessions != NULL) || !require(specs != NULL) ||
        !require(latency != NULL)) {
        return -1;
    }
    if (benchmark_synchronized_prompt_command(
            sessions, specs, latency->async_launch,
            "wait; /usr/bin/printf __GSH_BENCH_SYNC__\r", ": &\r",
            "asynchronous builtin launch") == -1 ||
        benchmark_synchronized_prompt_command(
            sessions, specs, latency->async_external_launch,
            "wait; /usr/bin/printf __GSH_BENCH_SYNC__\r",
            "/bin/sleep 0.05 &\r",
            "asynchronous held external launch") == -1 ||
        benchmark_synchronized_prompt_command(
            sessions, specs, latency->wait_completed,
            "wait; : & /bin/sleep 0.01; "
            "/usr/bin/printf __GSH_BENCH_SYNC__\r",
            "wait\r", "wait completed job") == -1) {
        return -1;
    }
    return 0;
}

static int write_latency_group_one(
    benchmark_report *report, const shell_spec specs[BENCH_SHELLS],
    const benchmark_latency *latency)
{
    if (!require(report != NULL) || !require(specs != NULL) ||
        !require(latency != NULL)) {
        return -1;
    }
    const benchmark_latency_metric metrics[] = {
        {"startup-to-base-prompt", &latency->startup[0][0],
         BENCH_STARTUP_SAMPLES},
        {"idle-key-to-output", &latency->key[0][0], BENCH_KEY_SAMPLES},
        {"true-enter-to-prompt", &latency->execution[0][0],
         BENCH_EXEC_SAMPLES},
        {"direct-echo", &latency->direct_echo[0][0], BENCH_EXEC_SAMPLES},
        {"direct-printf", &latency->direct_printf[0][0], BENCH_EXEC_SAMPLES},
        {"direct-test", &latency->direct_test[0][0], BENCH_EXEC_SAMPLES},
        {"variable-lookup", &latency->lookup[0][0], BENCH_EXEC_SAMPLES},
        {"command-lookup", &latency->command_lookup[0][0],
         BENCH_EXEC_SAMPLES},
        {"command-path-cache", &latency->command_path_cache[0][0],
         BENCH_EXEC_SAMPLES},
        {"variable-assignment", &latency->assignment[0][0],
         BENCH_EXEC_SAMPLES},
        {"parameter-assign-default", &latency->assign_default[0][0],
         BENCH_EXEC_SAMPLES},
        {"arithmetic-assignment", &latency->arithmetic_assignment[0][0],
         BENCH_EXEC_SAMPLES},
    };

    return write_latency_report(report, specs, metrics,
                                sizeof(metrics) / sizeof(metrics[0]));
}

static int write_latency_group_two(
    benchmark_report *report, const shell_spec specs[BENCH_SHELLS],
    const benchmark_latency *latency)
{
    if (!require(report != NULL) || !require(specs != NULL) ||
        !require(latency != NULL)) {
        return -1;
    }
    const benchmark_latency_metric metrics[] = {
        {"pipeline-scoped-assignment", &latency->pipeline_assignment[0][0],
         BENCH_EXEC_SAMPLES},
        {"ulimit-soft-nofile", &latency->resource_limit[0][0],
         BENCH_EXEC_SAMPLES},
        {"umask-report", &latency->creation_mask[0][0], BENCH_EXEC_SAMPLES},
        {"process-times", &latency->process_times[0][0], BENCH_EXEC_SAMPLES},
        {"exec-descriptor-commit", &latency->exec_descriptor[0][0],
         BENCH_EXEC_SAMPLES},
        {"export-assignment", &latency->export_assignment[0][0],
         BENCH_EXEC_SAMPLES},
        {"unset-variable", &latency->unset_variable[0][0],
         BENCH_EXEC_SAMPLES},
        {"readonly-existing", &latency->readonly_existing[0][0],
         BENCH_EXEC_SAMPLES},
        {"for-explicit", &latency->for_explicit[0][0], BENCH_EXEC_SAMPLES},
        {"set-positionals", &latency->set_positionals[0][0],
         BENCH_EXEC_SAMPLES},
        {"shift-positionals", &latency->shift_positionals[0][0],
         BENCH_EXEC_SAMPLES},
        {"set-options", &latency->set_options[0][0], BENCH_EXEC_SAMPLES},
    };

    return write_latency_report(report, specs, metrics,
                                sizeof(metrics) / sizeof(metrics[0]));
}

static int write_latency_group_three(
    benchmark_report *report, const shell_spec specs[BENCH_SHELLS],
    const benchmark_latency *latency)
{
    if (!require(report != NULL) || !require(specs != NULL) ||
        !require(latency != NULL)) {
        return -1;
    }
    const benchmark_latency_metric metrics[] = {
        {"allexport-assignment", &latency->allexport_assignment[0][0],
         BENCH_EXEC_SAMPLES},
        {"nounset-defined-lookup", &latency->nounset_lookup[0][0],
         BENCH_EXEC_SAMPLES},
        {"parameter-pattern-removal", &latency->pattern_removal[0][0],
         BENCH_EXEC_SAMPLES},
        {"parameter-pattern-multistar", &latency->pattern_multistar[0][0],
         BENCH_EXEC_SAMPLES},
        {"pathname-expansion", &latency->pathname_expansion[0][0],
         BENCH_EXEC_SAMPLES},
        {"noglob-expansion", &latency->noglob_expansion[0][0],
         BENCH_EXEC_SAMPLES},
        {"noclobber-redirection", &latency->noclobber_redirection[0][0],
         BENCH_EXEC_SAMPLES},
        {"async-builtin-launch", &latency->async_launch[0][0],
         BENCH_EXEC_SAMPLES},
        {"async-external-held-launch", &latency->async_external_launch[0][0],
         BENCH_EXEC_SAMPLES},
        {"wait-completed", &latency->wait_completed[0][0],
         BENCH_EXEC_SAMPLES},
        {"alias-definition-update", &latency->alias_definition[0][0],
         BENCH_EXEC_SAMPLES},
        {"alias-lookup-expansion", &latency->alias_expansion[0][0],
         BENCH_EXEC_SAMPLES},
        {"unalias-definition", &latency->unalias_command[0][0],
         BENCH_EXEC_SAMPLES},
    };

    return write_latency_report(report, specs, metrics,
                                sizeof(metrics) / sizeof(metrics[0]));
}

static int commit_latency_report(
    const char *output_path, const shell_spec specs[BENCH_SHELLS],
    const benchmark_latency *latency, const benchmark_memory *memory_results,
    size_t bash_wins, size_t zsh_wins)
{
    benchmark_report report;

    if (!require(output_path != NULL) || !require(specs != NULL) ||
        !require(latency != NULL) || !require(memory_results != NULL)) {
        return -1;
    }
    if (benchmark_report_open(&report, output_path) == -1 ||
        write_latency_group_one(&report, specs, latency) == -1 ||
        write_latency_group_two(&report, specs, latency) == -1 ||
        write_latency_group_three(&report, specs, latency) == -1 ||
        benchmark_report_write_gate(
            &report, "direct-builtins", specs[0].name,
            benchmark_shell_version(&specs[0]), specs[0].executable,
            specs[1].name, bash_wins, 3U * BENCH_EXEC_SAMPLES) == -1 ||
        benchmark_report_write_gate(
            &report, "direct-builtins", specs[0].name,
            benchmark_shell_version(&specs[0]), specs[0].executable,
            specs[2].name, zsh_wins, 3U * BENCH_EXEC_SAMPLES) == -1 ||
        write_memory_report(&report, specs, memory_results, 0,
                            BENCH_MEMORY_WORKLOADS) == -1 ||
        benchmark_report_commit(&report) == -1) {
        perror("pty benchmark: CSV report");
        benchmark_report_abort(&report);
        return -1;
    }
    return 0;
}

static int latency_benchmark(const char *gsh, const char *bash,
                             const char *zsh, const char *output_path)
{
    if (bash == NULL || gsh == NULL || zsh == NULL) {
        return -1;
    }
    shell_spec specs[BENCH_SHELLS] = {
        {"gsh", gsh, "gsh$ ", SHELL_GSH},
        {"bash", bash, "gsh$ ", SHELL_BASH},
        {"zsh", zsh, "gsh$ ", SHELL_ZSH},
    };
    static benchmark_latency latency;
    pty_session sessions[BENCH_SHELLS];
    bool started[BENCH_SHELLS] = {false, false, false};
    benchmark_memory memory_results;
    char directory[4096];
    size_t bash_wins;
    size_t zsh_wins;

    if (getcwd(directory, sizeof(directory)) == NULL ||
        setenv("GSH_BENCH_VALUE", "value", 1) == -1 ||
        setenv("GSH_BENCH_PATTERN", "abcabc", 1) == -1 ||
        setenv("GSH_BENCH_PATTERN_MULTI", "abacadTAIL", 1) == -1) {
        perror("pty benchmark: current directory");
        return 1;
    }
    if (prepare_benchmark_shells(specs, directory) == -1 ||
        measure_benchmark_startup(specs, directory, &latency) == -1) {
        return 1;
    }
    if (open_benchmark_sessions(sessions, started, specs, directory) == -1 ||
        warm_benchmark_builtins(sessions, specs) == -1 ||
        measure_benchmark_keys(sessions, &latency) == -1) {
        return finish_benchmark_sessions(sessions, started, 1);
    }

    if (benchmark_command_group_one(sessions, specs, &latency) == -1 ||
        benchmark_command_group_two(sessions, specs, &latency) == -1 ||
        benchmark_command_group_three(sessions, specs, &latency) == -1) {
        return finish_benchmark_sessions(sessions, started, 1);
    }
    if (restart_benchmark_sessions(sessions, started, specs, directory) == -1 ||
        benchmark_async_commands(sessions, specs, &latency) == -1) {
        return finish_benchmark_sessions(sessions, started, 1);
    }

    if (finish_benchmark_sessions(sessions, started, 0) != 0) {
        return 1;
    }
    if (direct_builtin_performance_gate(
            latency.direct_echo, latency.direct_printf, latency.direct_test,
            &bash_wins,
            &zsh_wins) == -1 ||
        memory_benchmark(specs, directory, 0, BENCH_MEMORY_WORKLOADS,
                         &memory_results) == -1) {
        return 1;
    }
    if (commit_latency_report(output_path, specs, &latency, &memory_results,
                              bash_wins, zsh_wins) == -1) {
        return 1;
    }
    (void)printf("benchmark: pass | latency=%d | memory=%d+startup | rows=%zu\n",
           BENCH_LATENCY_WORKLOADS, BENCH_MEMORY_WORKLOADS,
           (size_t)BENCH_LATENCY_WORKLOADS * BENCH_SHELLS +
               (2U + 5U * BENCH_MEMORY_WORKLOADS) * BENCH_SHELLS + 2U);
    (void)printf("direct-builtins: pass | bash=%zu/%u | zsh=%zu/%u\n",
           bash_wins, 3U * BENCH_EXEC_SAMPLES, zsh_wins,
           3U * BENCH_EXEC_SAMPLES);
    (void)printf("csv: %s\n", output_path);
    return 0;
}

static int alias_benchmark(const char *gsh, const char *bash,
                           const char *zsh, const char *output_path)
{
    if (bash == NULL || gsh == NULL || zsh == NULL) {
        return -1;
    }
    shell_spec specs[BENCH_SHELLS] = {
        {"gsh", gsh, "gsh$ ", SHELL_GSH},
        {"bash", bash, "gsh$ ", SHELL_BASH},
        {"zsh", zsh, "gsh$ ", SHELL_ZSH},
    };
    uint64_t definition[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t expansion[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t removal[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    pty_session sessions[BENCH_SHELLS];
    bool started[BENCH_SHELLS] = {false, false, false};
    benchmark_memory memory_results;
    benchmark_report report;
    char directory[4096];
    size_t shell;
    int failed = 0;

    if (getcwd(directory, sizeof(directory)) == NULL) {
        return 1;
    }
    for (shell = 0; shell < BENCH_SHELLS; shell++) {
        if (access(specs[shell].executable, X_OK) == -1 ||
            start_benchmark_session(&sessions[shell], &specs[shell],
                                    directory, true) == -1) {
            return finish_benchmark_sessions(sessions, started, 1);
        }
        started[shell] = true;
    }
    if (benchmark_prompt_command(
            sessions, specs, definition, "alias GSH_BENCH_ALIAS=:\r",
            "alias GSH_BENCH_ALIAS=true\r", "alias definition update") ==
            -1 ||
        benchmark_prompt_command(
            sessions, specs, expansion, "alias GSH_BENCH_ALIAS=:\r",
            "GSH_BENCH_ALIAS\r", "alias lookup expansion") == -1 ||
        benchmark_prompt_command(
            sessions, specs, removal, "alias GSH_BENCH_ALIAS=:\r",
            "unalias GSH_BENCH_ALIAS\r", "unalias definition") == -1) {
        failed = 1;
    }
    failed = finish_benchmark_sessions(sessions, started, failed);
    if (failed) {
        return 1;
    }
    if (memory_benchmark(specs, directory, BENCH_MEMORY_WORKLOADS - 3U, 3U,
                         &memory_results) == -1) {
        return 1;
    }
    {
        const benchmark_latency_metric metrics[3] = {
            {"alias-definition-update", &definition[0][0],
             BENCH_EXEC_SAMPLES},
            {"alias-lookup-expansion", &expansion[0][0],
             BENCH_EXEC_SAMPLES},
            {"unalias-definition", &removal[0][0], BENCH_EXEC_SAMPLES},
        };

        if (benchmark_report_open(&report, output_path) == -1 ||
            write_latency_report(&report, specs, metrics, 3U) == -1 ||
            write_memory_report(&report, specs, &memory_results,
                                BENCH_MEMORY_WORKLOADS - 3U, 3U) == -1 ||
            benchmark_report_commit(&report) == -1) {
            perror("pty alias benchmark: CSV report");
            benchmark_report_abort(&report);
            return 1;
        }
    }
    (void)printf("alias benchmark: pass | latency=3 | memory=3+startup | rows=%u\n",
           3U * BENCH_SHELLS + (2U + 5U * 3U) * BENCH_SHELLS);
    (void)printf("csv: %s\n", output_path);
    return 0;
}

static uint64_t pty_fuzz_next(uint64_t *state)
{
    if (state == NULL) {
        return 0U;
    }
    uint64_t value = *state;

    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * 0x2545f4914f6cdd1dULL;
}

static const char *const pty_fuzz_semantic_commands[] = {
    ": \"${GSH_FUZZ_PIPE:=left}\" | /usr/bin/true\r",
    "/usr/bin/printf x | /usr/bin/tr x y\r",
    "if /usr/bin/true; then : \"${GSH_FUZZ_STATE:=value}\"; fi\r",
    ": \"$(/usr/bin/printf nested)\"\r",
    "GSH_FUZZ_ASSIGN=value\r",
    "alias GSH_FUZZ_ALIAS=/usr/bin/true\r",
    "GSH_FUZZ_ALIAS\r",
    "unalias GSH_FUZZ_ALIAS\r",
    "if /usr/bin/true; then alias GSH_FUZZ_COMPOUND=/usr/bin/true; fi\r",
    ": \"${GSH_FUZZ_ERROR:?expected}\" | /usr/bin/true\r",
    "ulimit -S -n\r",
    "umask -S\r",
    "export GSH_FUZZ_EXPORT=value\r",
    "readonly GSH_FUZZ_READONLY\r",
    "export GSH_FUZZ_QUOTE=\"a'b\"; export -p >/dev/null\r",
    "GSH_FUZZ_UNSET=value; unset GSH_FUZZ_UNSET\r",
    "unset GSH_FUZZ_MISSING\r",
    "for GSH_FUZZ_ITEM in a 'b c' ''; do :; done\r",
    "for GSH_FUZZ_OUTER in a b; do "
    "for GSH_FUZZ_INNER in 1 2; do :; done; done\r",
    "set -- a 'b c' ''; shift\r",
    "if /usr/bin/true; then set -- x y; shift 0; fi\r",
    "set -aCfu; GSH_FUZZ_AUTO=value; "
    ": \"${GSH_FUZZ_KNOWN:=known}\"; set +aCfu\r",
    "GSH_FUZZ_DEFINED=value; set -u; "
    ": \"$GSH_FUZZ_DEFINED\"; set +u\r",
    "unset GSH_FUZZ_DEFAULT; set -u; "
    ": \"${GSH_FUZZ_DEFAULT:=value}\"; set +u\r",
    "GSH_FUZZ_ARITH=010; : \"$((GSH_FUZZ_ARITH += 1))\"; "
    "/bin/test \"$GSH_FUZZ_ARITH\" = 9\r",
    "GSH_FUZZ_ARITH=1; : \"$((0 && (GSH_FUZZ_ARITH = 2)))\"; "
    "/bin/test \"$GSH_FUZZ_ARITH\" = 1\r",
    "GSH_FUZZ_COLON=value :\r",
    "if /usr/bin/true; then cd .; fi\r",
    "set -f; /usr/bin/printf '%s' /dev/n[uo]ll; set +f\r",
    "set -C; /usr/bin/printf x >/dev/null; set +C\r",
    "/usr/bin/true &\r",
    "wait \"$!\"\r",
};

static int send_fuzz_semantic_seed(pty_session *session, uint64_t *state)
{
    size_t count = sizeof(pty_fuzz_semantic_commands) /
                   sizeof(pty_fuzz_semantic_commands[0]);
    const char *command;

    if (!require(session != NULL) || !require(state != NULL)) {
        return -1;
    }
    command = pty_fuzz_semantic_commands[
        (size_t)(pty_fuzz_next(state) % count)];
    if (send_text(session, command) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static void generate_fuzz_bytes(unsigned char bytes[512], size_t length,
                                uint64_t *state)
{
    size_t offset;

    if (!require(bytes != NULL) || !require(state != NULL) ||
        !require(length <= 512U)) {
        return;
    }
    for (offset = 0; offset < length; offset++) {
        uint64_t choice = pty_fuzz_next(state);

        switch (choice & 15U) {
        case 0:
            bytes[offset] = 0;
            break;
        case 1:
            bytes[offset] = 0x08U;
            break;
        case 2:
            bytes[offset] = 0x0cU;
            break;
        case 3:
            bytes[offset] = 0x15U;
            break;
        case 4:
            bytes[offset] = 0x1bU;
            break;
        case 5:
        case 6:
            bytes[offset] =
                (unsigned char)(0x80U + (choice >> 8U) % 0x80U);
            break;
        default:
            bytes[offset] =
                (unsigned char)(0x20U + (choice >> 8U) % 0x5fU);
            if (bytes[offset] == '^') {
                bytes[offset] = '_';
            }
            break;
        }
    }
}

static int run_pty_fuzz_case(pty_session *session, unsigned char bytes[512],
                             uint64_t *state, unsigned long index)
{
    size_t length;

    if (!require(session != NULL) || !require(bytes != NULL) ||
        !require(state != NULL) || !require(index <= 1000000U)) {
        return -1;
    }
    if ((index & 7U) == 0 && send_fuzz_semantic_seed(session, state) == -1) {
        return -1;
    }
    length = (size_t)(pty_fuzz_next(state) % 512U);
    generate_fuzz_bytes(bytes, length, state);
    if (send_bytes(session, (const char *)bytes, length) == -1 ||
        ((pty_fuzz_next(state) & 7U) == 0 &&
         kill(session->pid, SIGWINCH) == -1) ||
        send_bytes(session, "\003", 1) == -1 ||
        consume_through(session, "^C", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int report_pty_fuzz_resources(const pty_session *session,
                                     unsigned long cases, int initial_fds,
                                     int initial_children)
{
    if (!require(session != NULL) || !require(cases > 0U)) {
        return -1;
    }
    int final_fds = process_fd_count(session->pid);
    int final_children = process_child_count(session->pid);

    if (final_fds < 0 || final_fds > initial_fds + 1 ||
        final_children < 0 || final_children > initial_children) {
        return -1;
    }
    (void)printf("pty byte fuzz: cases=%lu seed=0x6a09e667f3bcc909 "
           "fds_initial=%d fds_final=%d children_initial=%d "
           "children_final=%d passed\n",
           cases, initial_fds, final_fds, initial_children, final_children);
    return 0;
}

static int pty_byte_fuzz(const char *executable, unsigned long cases)
{
    char fixture[] = "/tmp/gsh-pty-fuzz-XXXXXX";
    unsigned char bytes[512];
    pty_session session;
    uint64_t random_state = 0x6a09e667f3bcc909ULL;
    int initial_fds;
    int initial_children;
    unsigned long index;

    if (cases == 0 || cases > 1000000 ||
        unsetenv("GSH_FUZZ_PIPE") == -1 ||
        unsetenv("GSH_FUZZ_STATE") == -1 ||
        unsetenv("GSH_FUZZ_ASSIGN") == -1 ||
        unsetenv("GSH_FUZZ_ARITH") == -1 ||
        unsetenv("GSH_FUZZ_ERROR") == -1 || mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fuzz: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "gsh$ ", TEST_TIMEOUT_MS) == -1) {
        perror("pty fuzz: initial prompt");
        return finish_session_directory(&session, fixture, 1);
    }
    initial_fds = process_fd_count(session.pid);
    initial_children = process_child_count(session.pid);
    if (initial_fds < 0 || initial_children < 0) {
        (void)fprintf(stderr, "pty fuzz: process metrics unavailable\n");
        return finish_session_directory(&session, fixture, 1);
    }
    for (index = 0; index < cases; index++) {
        if (run_pty_fuzz_case(&session, bytes, &random_state, index) == -1) {
            (void)fprintf(stderr, "pty fuzz: failed at case %lu\n", index);
            dump_capture(&session);
            return finish_session_directory(&session, fixture, 1);
        }
    }
    if (report_pty_fuzz_resources(&session, cases, initial_fds,
                                  initial_children) == -1) {
        (void)fprintf(stderr, "pty fuzz: resource growth exceeded bounds\n");
        return finish_session_directory(&session, fixture, 1);
    }
    return finish_session_directory(&session, fixture, 0);
}

static int smoke_flow_failure(const char *name)
{
    if (!require(name != NULL)) return 1;
    (void)fprintf(stderr, "pty smoke: %s flow failed\n", name);
    return 1;
}

static int run_primary_smoke_flows(const char *executable)
{
    if (!require(executable != NULL)) return 1;
    if (ordinary_flow(executable) != 0) {
        return smoke_flow_failure("ordinary");
    }
    if (job_service_control_flow(executable) != 0) {
        return smoke_flow_failure("job service control");
    }
    if (job_notification_flow(executable) != 0) {
        return smoke_flow_failure("job notification");
    }
    if (interactive_errexit_flow(executable) != 0) {
        return smoke_flow_failure("interactive errexit");
    }
    if (fc_builtin_flow(executable) != 0) {
        return smoke_flow_failure("fc builtin");
    }
    if (protected_bridge_flow(executable) != 0) {
        return smoke_flow_failure("protected bridge");
    }
    if (history_flow(executable) != 0) {
        return smoke_flow_failure("history");
    }
    if (editor_navigation_flow(executable) != 0) {
        return smoke_flow_failure("editor navigation/paste");
    }
    if (managed_async_repl_flow(executable) != 0) {
        return smoke_flow_failure("managed async REPL");
    }
    if (managed_resource_action_flow(executable) != 0) {
        return smoke_flow_failure("managed resource action");
    }
    if (managed_directory_action_flow(executable) != 0) {
        return smoke_flow_failure("managed directory action");
    }
    if (managed_detected_action_flow(executable) != 0) {
        return smoke_flow_failure("managed detected action");
    }
    if (managed_markdown_preview_flow(executable) != 0) {
        return smoke_flow_failure("managed Markdown preview");
    }
    if (managed_image_probe_flow(executable) != 0) {
        return smoke_flow_failure("managed image capability probe");
    }
    if (managed_scroll_flow(executable) != 0) {
        return smoke_flow_failure("managed wheel scrolling");
    }
    return 0;
}

static int run_builtin_smoke_flows(const char *executable)
{
    if (!require(executable != NULL)) return 1;
    if (variable_builtin_flow(executable) != 0) {
        return smoke_flow_failure("variable builtin");
    }
    if (alias_builtin_flow(executable) != 0) {
        return smoke_flow_failure("alias builtin");
    }
    if (command_hash_flow(executable) != 0) {
        return smoke_flow_failure("command hash");
    }
    if (times_builtin_flow(executable) != 0) {
        return smoke_flow_failure("times builtin");
    }
    if (exec_builtin_flow(executable) != 0) {
        return smoke_flow_failure("exec builtin");
    }
    return 0;
}

static int run_language_smoke_flows(const char *executable)
{
    if (!require(executable != NULL)) return 1;
    if (interactive_enoexec_flow(executable) != 0) {
        return smoke_flow_failure("interactive ENOEXEC");
    }
    if (interactive_source_flow(executable) != 0) {
        return smoke_flow_failure("interactive source");
    }
    if (function_builtin_flow(executable) != 0) {
        return smoke_flow_failure("function builtin");
    }
    if (deferred_pattern_flow(executable) != 0) {
        return smoke_flow_failure("deferred pattern");
    }
    if (asynchronous_list_flow(executable) != 0) {
        return smoke_flow_failure("asynchronous list");
    }
    if (async_redirection_flow(executable) != 0) {
        return smoke_flow_failure("async redirection");
    }
    return 0;
}

int main(int argc, char **argv)
{
    char executable[4096];

    if (close_inherited_descriptors() == -1) {
        perror("pty harness: inherited descriptors");
        return 1;
    }
    if (configure_utf8_locale() == -1) {
        perror("pty harness: UTF-8 locale");
        return 1;
    }

    if (argc == 6 && strcmp(argv[1], "--benchmark") == 0) {
        return latency_benchmark(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 6 && strcmp(argv[1], "--benchmark-alias") == 0) {
        return alias_benchmark(argv[2], argv[3], argv[4], argv[5]);
    }
    if (argc == 3 && strcmp(argv[1], "--fault") == 0) {
        return fault_injection_flow(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "--resource") == 0) {
        return resource_exhaustion_flow(argv[2]);
    }
    if (argc == 4 && strcmp(argv[1], "--soak") == 0) {
        char *end;
        unsigned long seconds = strtoul(argv[3], &end, 10);

        if (*end != '\0' || seconds == 0 || seconds > 86400) {
            (void)fprintf(stderr, "pty-harness: soak seconds must be 1..86400\n");
            return 2;
        }
        return soak_flow(argv[2], seconds);
    }
    if (argc == 4 && strcmp(argv[1], "--fuzz-pty") == 0) {
        char *end;
        unsigned long cases = strtoul(argv[3], &end, 10);

        if (*end != '\0' || cases == 0 || cases > 1000000) {
            (void)fprintf(stderr, "pty-harness: fuzz cases must be 1..1000000\n");
            return 2;
        }
        return pty_byte_fuzz(argv[2], cases);
    }
    if (argc != 2) {
        (void)fprintf(stderr,
                "usage: pty-harness /absolute/path/to/gsh\n"
                "       pty-harness --fault /absolute/path/to/gsh-fault\n"
                "       pty-harness --resource /absolute/path/to/gsh\n"
                "       pty-harness --soak /absolute/path/to/gsh seconds\n"
                "       pty-harness --fuzz-pty /absolute/path/to/gsh cases\n"
                "       pty-harness --benchmark gsh bash zsh report.csv\n"
                "       pty-harness --benchmark-alias gsh bash zsh "
                "report.csv\n");
        return 2;
    }
    if (argv[1][0] == '/') {
        if (strlen(argv[1]) + 1U > sizeof(executable)) {
            (void)fprintf(stderr, "pty smoke: executable path is too long\n");
            return 2;
        }
        (void)memcpy(executable, argv[1], strlen(argv[1]) + 1U);
    } else if (getcwd(executable, sizeof(executable)) == NULL ||
               strlen(executable) + strlen(argv[1]) + 2U >
                   sizeof(executable)) {
        perror("pty smoke: executable path");
        return 2;
    } else {
        size_t length = strlen(executable);

        executable[length++] = '/';
        (void)memcpy(executable + length, argv[1], strlen(argv[1]) + 1U);
    }

    if (run_primary_smoke_flows(executable) != 0 ||
        run_builtin_smoke_flows(executable) != 0 ||
        run_language_smoke_flows(executable) != 0) {
        return 1;
    }
    (void)puts("pty smoke: exec paths, native fc, protected bridge, plain history, "
         "editor cursor/paste/recall/search, managed async REPL, job control, "
         "async prompt/redirection and cancellation passed");
    return 0;
}
