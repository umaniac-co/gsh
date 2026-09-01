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

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#endif

enum {
    CAPTURE_CAP = 65536,
    TEST_TIMEOUT_MS = 3000,
    BENCH_SHELLS = 3,
    BENCH_STARTUP_SAMPLES = 120,
    BENCH_KEY_SAMPLES = 500,
    BENCH_EXEC_SAMPLES = 300,
    BENCH_MEMORY_SAMPLES = 20,
    BENCH_MEMORY_WORKLOADS = 26,
};

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

    for (;;) {
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
                close(descriptor);
                closed = true;
            }
        }
        if (!closed) {
            return 0;
        }
    }
#elif defined(__linux__)
    struct dirent *entry;
    DIR *directory = opendir("/proc/self/fd");
    int directory_fd;
    int failed = 0;

    if (directory == NULL) {
        return -1;
    }
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
    close(descriptor);
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
    return find_bytes(session->capture, session->capture_length, text) != NULL;
}

static void dump_capture(const pty_session *session)
{
    if (session->capture_length == 0) {
        return;
    }
    fputs("pty capture follows:\n", stderr);
    (void)fwrite(session->capture, 1, session->capture_length, stderr);
    fputc('\n', stderr);
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);

    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return -1;
    }
    return 0;
}

static int start_session(pty_session *session, const char *executable,
                         const char *directory, shell_kind kind)
{
    char slave_name[256];
    const char *name;
    int mode_pipe[2];
    int master;
    pid_t pid;

    memset(session, 0, sizeof(*session));
    session->master = -1;
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master == -1 || grantpt(master) == -1 || unlockpt(master) == -1) {
        if (master >= 0) {
            close(master);
        }
        return -1;
    }
    name = ptsname(master);
    if (name == NULL || strlen(name) + 1U > sizeof(slave_name)) {
        close(master);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(slave_name, name, strlen(name) + 1U);
    memcpy(session->slave_name, slave_name, strlen(slave_name) + 1U);
    if (pipe(mode_pipe) == -1) {
        close(master);
        return -1;
    }
    pid = fork();
    if (pid == 0) {
        struct winsize size;
        struct termios initial_modes;
        int slave;

        close(mode_pipe[0]);
        close(master);
        if (setsid() == -1) {
            _exit(120);
        }
        slave = open(slave_name, O_RDWR);
        if (slave == -1) {
            _exit(121);
        }
        memset(&size, 0, sizeof(size));
        size.ws_row = 24;
        size.ws_col = 80;
        if (ioctl(slave, TIOCSWINSZ, &size) == -1) {
            _exit(122);
        }
        if (tcgetattr(slave, &initial_modes) == -1 ||
            write(mode_pipe[1], &initial_modes, sizeof(initial_modes)) !=
                (ssize_t)sizeof(initial_modes)) {
            _exit(123);
        }
        close(mode_pipe[1]);
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
            close(slave);
        }
        if (tcsetpgrp(STDIN_FILENO, getpgrp()) == -1 ||
            chdir(directory) == -1) {
            _exit(126);
        }
        (void)setenv("PATH", "/usr/bin:/bin", 1);
        (void)setenv("TERM", "xterm-256color", 1);
        (void)setenv("PS1", kind == SHELL_BASH ? "\\$gsh> " : "$gsh> ",
                     1);
        (void)setenv("PS2", "GSH_MORE> ", 1);
        (void)setenv("PROMPT", "$gsh> ", 1);
        (void)setenv("RPROMPT", "", 1);
        if (kind == SHELL_GSH) {
            const char *managed = getenv("GSH_HARNESS_MANAGED");
            const char *history = getenv("GSH_HARNESS_HISTORY");
            const char *repl = getenv("GSH_HARNESS_REPL");

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
        {
            const char *limit_text = getenv("GSH_HARNESS_NOFILE");

            if (limit_text != NULL && limit_text[0] != '\0') {
                char *end;
                unsigned long value = strtoul(limit_text, &end, 10);
                struct rlimit limit;

                if (*end != '\0' || value == 0 ||
                    getrlimit(RLIMIT_NOFILE, &limit) == -1) {
                    _exit(127);
                }
                limit.rlim_cur = (rlim_t)value;
                if (limit.rlim_cur > limit.rlim_max) {
                    limit.rlim_cur = limit.rlim_max;
                }
                if (setrlimit(RLIMIT_NOFILE, &limit) == -1) {
                    _exit(127);
                }
            }
        }
#ifdef RLIMIT_NPROC
        {
            const char *limit_text = getenv("GSH_HARNESS_NPROC");

            if (limit_text != NULL && limit_text[0] != '\0') {
                char *end;
                unsigned long value = strtoul(limit_text, &end, 10);
                struct rlimit limit;

                if (*end == '\0' && value > 0 &&
                    getrlimit(RLIMIT_NPROC, &limit) == 0) {
                    limit.rlim_cur = (rlim_t)value;
                    if (limit.rlim_cur > limit.rlim_max) {
                        limit.rlim_cur = limit.rlim_max;
                    }
                    (void)setrlimit(RLIMIT_NPROC, &limit);
                }
            }
        }
#endif
        {
            const char *limit_text = getenv("GSH_HARNESS_DATA");

            if (limit_text != NULL && limit_text[0] != '\0') {
                char *end;
                unsigned long value = strtoul(limit_text, &end, 10);
                struct rlimit limit;

                if (*end != '\0' || value == 0 ||
                    getrlimit(RLIMIT_DATA, &limit) == -1) {
                    _exit(127);
                }
                limit.rlim_cur = (rlim_t)value;
                if (limit.rlim_cur > limit.rlim_max) {
                    limit.rlim_cur = limit.rlim_max;
                }
                if (setrlimit(RLIMIT_DATA, &limit) == -1) {
                    _exit(127);
                }
            }
        }
        /* ── Internal-Descriptor Tests Need a Stable First Slot ──
         * CI launchers may leak an unrelated descriptor 3 without CLOEXEC.
         * The exec-protection test deliberately targets the shell's first
         * internal descriptor, so make that slot deterministic immediately
         * before overlaying the child. Standard descriptors remain intact.
         * ─────────────────────────────────────────────────────── */
        (void)close(STDERR_FILENO + 1);
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
    close(mode_pipe[1]);
    if (pid == -1) {
        close(mode_pipe[0]);
        close(master);
        return -1;
    }
    {
        size_t offset = 0;

        while (offset < sizeof(session->initial_modes)) {
            ssize_t count = read(mode_pipe[0],
                                 (unsigned char *)&session->initial_modes +
                                     offset,
                                 sizeof(session->initial_modes) - offset);

            if (count > 0) {
                offset += (size_t)count;
            } else if (count == -1 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(mode_pipe[0]);
        if (offset != sizeof(session->initial_modes)) {
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, NULL, 0);
            close(master);
            errno = EIO;
            return -1;
        }
        session->initial_modes_valid = true;
    }
    if (set_nonblocking(master) == -1) {
        int saved_errno = errno;

        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
        close(master);
        errno = saved_errno;
        return -1;
    }
    session->pid = pid;
    session->master = master;
    return 0;
}

static int start_managed_session(pty_session *session,
                                 const char *executable,
                                 const char *directory)
{
    int result;

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
    uint64_t deadline = monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;

    for (;;) {
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
    memmove(session->capture, session->capture + consumed,
            session->capture_length - consumed);
    session->capture_length -= consumed;
    return 0;
}

static int send_bytes(pty_session *session, const char *bytes, size_t length)
{
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
    return send_bytes(session, text, strlen(text));
}

static bool terminal_modes_match(const struct termios *left,
                                 const struct termios *right)
{
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
    struct termios final_modes;
    int slave;
    bool restored;

    if (!session->initial_modes_valid) {
        return false;
    }
    slave = open(session->slave_name, O_RDWR | O_NOCTTY);
    if (slave == -1 || tcgetattr(slave, &final_modes) == -1) {
        if (slave >= 0) {
            close(slave);
        }
        return false;
    }
    close(slave);
    restored = terminal_modes_match(&session->initial_modes, &final_modes);
    return restored;
}

static int resize_session(const pty_session *session, unsigned short rows,
                          unsigned short columns)
{
    struct winsize size;
    int slave = open(session->slave_name, O_RDWR | O_NOCTTY);
    int result;

    if (slave == -1) {
        return -1;
    }
    memset(&size, 0, sizeof(size));
    size.ws_row = rows;
    size.ws_col = columns;
    result = ioctl(slave, TIOCSWINSZ, &size);
    close(slave);
    return result;
}

static int stop_session(pty_session *session)
{
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
    for (;;) {
        pid_t result = waitpid(session->pid, &status, WNOHANG);

        if (result == session->pid) {
            break;
        }
        if (result == -1 && errno != EINTR) {
            status = -1;
            break;
        }
        if (monotonic_ns() >= deadline) {
            fprintf(stderr, "pty stop: shell exit deadline exceeded\n");
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
    if (child_count > 0) {
        bool alive = true;

        deadline = monotonic_ns() + 1000000000ULL;
        while (alive && monotonic_ns() < deadline) {
            int index;

            alive = false;
            for (index = 0; index < child_count; index++) {
                if (kill(children[index], 0) == 0 || errno != ESRCH) {
                    alive = true;
                }
            }
            if (alive) {
                (void)poll(NULL, 0, 10);
            }
        }
        if (alive) {
            int index;

            for (index = 0; index < child_count; index++) {
                if (kill(children[index], 0) == 0 || errno != ESRCH) {
                    fprintf(stderr, "pty stop: child %ld survived shutdown\n",
                            (long)children[index]);
                    (void)kill(children[index], SIGKILL);
                }
            }
            status = -1;
        }
    }
    if (!terminal_was_restored(session)) {
        fprintf(stderr, "pty stop: terminal modes were not restored\n");
        status = -1;
    }
    close(session->master);
    session->master = -1;
    session->pid = -1;
    return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0
                                                                        : -1;
}

static int wait_session_result(pty_session *session, int *result_status,
                               int timeout_ms)
{
    uint64_t deadline = monotonic_ns() + (uint64_t)timeout_ms * 1000000ULL;
    int status = 0;

    while (monotonic_ns() < deadline) {
        pid_t result = waitpid(session->pid, &status, WNOHANG);

        if (result == session->pid) {
            bool restored = terminal_was_restored(session);

            for (;;) {
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

            close(session->master);
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
    close(session->master);
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
    fprintf(stderr,
            "pty harness: exit mismatch expected=%d exited=%d actual=%d\n",
            expected_status, WIFEXITED(status) ? 1 : 0,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
}

static int wait_for_diagnostics(pty_session *session, const char *first,
                                const char *second)
{
    uint64_t deadline = monotonic_ns() + 2000000000ULL;

    while (monotonic_ns() < deadline) {
        session->capture_length = 0;
        if (send_text(session, "rt\r") == -1 ||
            wait_for_output(session, "reactor cycles=", TEST_TIMEOUT_MS) ==
                -1 ||
            wait_for_output(session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
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

static int write_head(const char *root, bool blocking)
{
    char git_directory[1024];
    char head_path[1024];
    int fd;

    if (snprintf(git_directory, sizeof(git_directory), "%s/.git", root) >=
            (int)sizeof(git_directory) ||
        snprintf(head_path, sizeof(head_path), "%s/.git/HEAD", root) >=
            (int)sizeof(head_path) ||
        mkdir(git_directory, 0700) == -1) {
        return -1;
    }
    if (blocking) {
        return mkfifo(head_path, 0600);
    }
    fd = open(head_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd == -1) {
        return -1;
    }
    if (write(fd, "ref: refs/heads/bench\n", 22) != 22) {
        close(fd);
        return -1;
    }
    return close(fd);
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
    if (snprintf(path, sizeof(path), "%s/.git/HEAD", root) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
    if (snprintf(path, sizeof(path), "%s/.git", root) < (int)sizeof(path)) {
        (void)rmdir(path);
    }
    (void)rmdir(root);
}

static int write_history_config(const char *home)
{
    static const char configuration[] =
        "config.version = 1\n\n"
        "shell.history.enabled = true\n"
        "shell.history.max_entries = 1024\n"
        "shell.history.deduplicate = false\n"
        "shell.history.store_failed = true\n"
        "shell.history.ignore_space = true\n"
        "shell.history.unlock_ttl = infinite\n"
        "shell.history.reminder_min = 3s\n"
        "shell.history.reminder_max = 3s\n";
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

    if (snprintf(path, sizeof(path), "%s/.gsh/history.sock", home) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
    if (snprintf(path, sizeof(path), "%s/.gsh/history.sock.lock", home) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
    if (snprintf(path, sizeof(path), "%s/.gsh/history.vault", home) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
    if (snprintf(path, sizeof(path), "%s/.gsh", home) < (int)sizeof(path)) {
        (void)rmdir(path);
    }
    if (snprintf(path, sizeof(path), "%s/.gshrc", home) <
        (int)sizeof(path)) {
        (void)unlink(path);
    }
    (void)rmdir(home);
}

static bool history_vault_is_encrypted(const char *home)
{
    static const char plaintext[] = "HISTORY_ALPHA";
    unsigned char bytes[8192 + sizeof(plaintext)];
    char path[PATH_MAX];
    struct stat status;
    size_t carry = 0;
    unsigned int reads;
    int descriptor;
    bool encrypted = true;

    if (snprintf(path, sizeof(path), "%s/.gsh/history.vault", home) >=
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
            encrypted = count == 0;
            break;
        }
        total = carry + (size_t)count;
        if (find_bytes(bytes, total, plaintext) != NULL) {
            encrypted = false;
            break;
        }
        carry = total < sizeof(plaintext) - 1U
                    ? total
                    : sizeof(plaintext) - 1U;
        memmove(bytes, bytes + total - carry, carry);
    }
    (void)close(descriptor);
    return encrypted;
}

static int create_history_vault(pty_session *session)
{
    static const char passphrase[] = "history test passphrase";

    if (consume_through(session, "New history passphrase: ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "history test passphrase\r") == -1 ||
        consume_through(session, "Confirm history passphrase: ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "history test passphrase\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        capture_contains(session, passphrase)) {
        return -1;
    }
    return 0;
}

static int exercise_history_editor(pty_session *session)
{
    static const char alpha[] = "/usr/bin/printf 'HISTORY_ALPHA\\n'";
    static const char beta[] = "/usr/bin/printf 'HISTORY_BETA\\n'";

    if (send_text(session, "/usr/bin/printf 'HISTORY_ALPHA\\n'\r") == -1 ||
        consume_through(session, "HISTORY_ALPHA\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/usr/bin/printf 'HISTORY_BETA\\n'\r") == -1 ||
        consume_through(session, "HISTORY_BETA\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
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
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  " /usr/bin/printf 'HISTORY_PRIVATE\\n' \r") == -1 ||
        consume_through(session, "HISTORY_PRIVATE\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033[A", 3) == -1 ||
        consume_through(session, alpha, TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        send_text(session, "history status\r") == -1 ||
        consume_through(session, "entries=4 max=1024 unlock=infinite",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int shutdown_history_agent(pty_session *session)
{
    return send_bytes(session, "\025", 1) == -1 ||
                   send_text(session, "history shutdown\r") == -1 ||
                   consume_through(session, "history agent stopped",
                                   TEST_TIMEOUT_MS) == -1
               ? -1
               : 0;
}

static int verify_unlocked_reuse(pty_session *session)
{
    if (consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033[A", 3) == -1 ||
        consume_through(session, "exit 0", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025\022ALPHA", 7) == -1 ||
        consume_through(session, "(reverse-i-search)`ALPHA': ",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "HISTORY_ALPHA", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033x", 2) == -1 ||
        shutdown_history_agent(session) == -1) {
        return -1;
    }
    return 0;
}

static int verify_fresh_agent_unlock(pty_session *session)
{
    if (consume_through(session, "History passphrase: ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "history test passphrase\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\033[A", 3) == -1 ||
        consume_through(session, "history shutdown", TEST_TIMEOUT_MS) == -1 ||
        shutdown_history_agent(session) == -1) {
        return -1;
    }
    return 0;
}

static int run_initial_history_session(const char *executable,
                                       const char *home)
{
    pty_session session;
    int failed = 0;

    if (start_session(&session, executable, home, SHELL_GSH) == -1) {
        return -1;
    }
    if (create_history_vault(&session) == -1 ||
        exercise_history_editor(&session) == -1 ||
        send_bytes(&session, "\025", 1) == -1 ||
        consume_through(&session, "History reminder", 5000) == -1 ||
        send_text(&session, "wrong history passphrase\r") == -1 ||
        consume_through(&session, "history remains unlocked",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "History reminder", 5000) == -1 ||
        send_text(&session, "history test passphrase\r") == -1 ||
        consume_through(&session, "passphrase remembered",
                        TEST_TIMEOUT_MS) == -1) {
        failed = -1;
    }
    if (stop_session(&session) == -1) {
        failed = -1;
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
    if (verify_unlocked_reuse(&session) == -1) {
        failed = -1;
    }
    if (stop_session(&session) == -1) {
        failed = -1;
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
    if (verify_fresh_agent_unlock(&session) == -1) {
        failed = -1;
    }
    if (stop_session(&session) == -1) {
        failed = -1;
    }
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
        memcpy(saved_home, current_home, strlen(current_home) + 1U);
    }
    if (mkdtemp(home) == NULL || write_history_config(home) == -1 ||
        setenv("HOME", home, 1) == -1 ||
        setenv("GSH_HARNESS_HISTORY", "1", 1) == -1) {
        failed = 1;
    } else if (run_initial_history_session(executable, home) == -1 ||
               !history_vault_is_encrypted(home)) {
        failed = 1;
    }
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
        fprintf(stderr, "pty smoke: probe path is too long\n");
        return 1;
    }
    memcpy(probe, executable, strlen(executable) + 1U);
    separator = strrchr(probe, '/');
    if (separator == NULL ||
        (size_t)(separator - probe) + sizeof("/job-probe") > sizeof(probe)) {
        fprintf(stderr, "pty smoke: cannot derive job probe path\n");
        return 1;
    }
    memcpy(separator, "/job-probe", sizeof("/job-probe"));
    if (snprintf(command, sizeof(command), "%s\r", probe) >=
        (int)sizeof(command)) {
        fprintf(stderr, "pty smoke: probe command is too long\n");
        return 1;
    }

    if (mkdtemp(fixture) == NULL ||
        snprintf(spaced_directory, sizeof(spaced_directory), "%s/with space",
                 fixture) >= (int)sizeof(spaced_directory) ||
        mkdir(spaced_directory, 0700) == -1 ||
        write_head(fixture, false) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty smoke: ordinary setup");
        remove_fixture(fixture);
        return 1;
    }

    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "\033[90m[bench]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/true\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf X | /usr/bin/tr X Y\r") == -1 ||
        consume_through(&session, "\nY", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/yes X | /usr/bin/head -n 1\r") == -1 ||
        consume_through(&session, "\nX\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '%s\\n' GSH_NATIVE_REDIRECT_OUTPUT "
                  "> native.out\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/cat native.out\r") == -1 ||
        consume_through(&session, "\nGSH_NATIVE_REDIRECT_OUTPUT\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/false && /usr/bin/printf BAD || "
                  "/usr/bin/printf GSH_NATIVE_AND_OR\r") == -1 ||
        consume_through(&session, "\nGSH_NATIVE_AND_OR",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if /usr/bin/false; then /usr/bin/printf BAD; else "
                  "/usr/bin/printf GSH_NATIVE_IF; fi\r") == -1 ||
        consume_through(&session, "\nGSH_NATIVE_IF", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "cd 'with space'\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/pwd\r") == -1 ||
        consume_through(&session, "/with space\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "cd ..\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/cat <<EOF\r") == -1 ||
        consume_through(&session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "GSH_PTY_HEREDOC status=$?\r") == -1 ||
        consume_through(&session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "EOF\r") == -1 ||
        consume_through(&session, "\nGSH_PTY_HEREDOC status=0\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/cat <<CANCEL\r") == -1 ||
        consume_through(&session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "^C", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"$(/usr/bin/printf GSH_SUBSTITUTION)\"\r") == -1 ||
        consume_through(&session, "<GSH_SUBSTITUTION>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"$(/usr/bin/printf 'GSH_SUB_%s' START 1>&2; "
                  "/bin/sleep 5; /usr/bin/printf late)\"\r") == -1 ||
        consume_through(&session, "GSH_SUB_START", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "GSH_PERSIST=value\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PERSIST\"\r") == -1 ||
        consume_through(&session, "<value>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, ": \"${GSH_TRANSACTION:=committed}\"\r") ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_TRANSACTION\"\r") ==
            -1 ||
        consume_through(&session, "<committed>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s:%s>\\n' "
                  "\"$((GSH_ARITHMETIC = 7))\" "
                  "\"$GSH_ARITHMETIC\"\r") == -1 ||
        consume_through(&session, "<7:7>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' native.*\r") == -1 ||
        consume_through(&session, "<native.out>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "rt\r") == -1 ||
        wait_for_output(&session,
                        "direct=3 native=16 shell=0 parsed=19 parse_failures=0 "
                        "job=idle worker=on",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "GSH_PROBE_READY", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "[stopped ", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "fg\r") == -1 ||
        consume_through(&session, "GSH_PROBE_CONTINUED", TEST_TIMEOUT_MS) ==
            -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty smoke: ordinary flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        fprintf(stderr,
                "pty smoke: completed pipelines left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        fprintf(stderr, "pty smoke: ordinary shell did not exit cleanly\n");
        failed = 1;
    }
    remove_fixture(fixture);
    return failed;
}

static int managed_repl_concurrency(pty_session *session)
{
    uint64_t start;

    if (send_text(session, "/bin/sleep 1\r") == -1 ||
        consume_through(session,
                        "/bin/sleep 1\r\n\r\n\033[90m[○]\033[0m $gsh> ",
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

static int managed_repl_indicator(pty_session *session)
{
    static const char command[] =
        "/bin/sh -c 'sleep .3; printf INDICATOR_DONE'\r";

    session->capture_length = 0;
    if (send_text(session, command) == -1 ||
        wait_for_output(session, "\033[90m[○]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "STYLE_PRESERVED") == -1 ||
        wait_for_output(session,
                        "\033[90m[○]\033[0m $gsh> STYLE_PRESERVED",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        wait_for_output(session,
                        "INDICATOR_DONE\r\n\033[90m[○]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session,
                        "INDICATOR_DONE\r\n\033[90m[●]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_terminal_outcomes(pty_session *session)
{
    session->capture_length = 0;
    if (send_text(session, "/bin/sh -c 'sleep .2; exit 7'\r") == -1 ||
        wait_for_output(session, "\033[90m[○]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "\033[90m[●]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    session->capture_length = 0;
    if (send_text(session,
                  "/bin/sh -c 'sleep .2; kill -TERM $$'\r") == -1 ||
        wait_for_output(session, "\033[90m[○]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "\033[90m[●]\033[0m $gsh> ",
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
                        "done\r\n\r\n\033[90m[○]\033[0m $gsh> ",
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
                        "done\r\n\r\n\033[90m[○]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    start = monotonic_ns();
    if (send_text(session, "/bin/pwd\r") == -1 ||
        consume_through(session, "/bin/pwd\r\n/", TEST_TIMEOUT_MS) == -1 ||
        monotonic_ns() - start < 700000000ULL) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int managed_repl_preserves_edit(pty_session *session)
{
    if (send_text(session,
                  "/bin/sh -c 'sleep 0.2; printf LATE'\r") == -1 ||
        consume_through(session,
                        "printf LATE'\r\n\r\n\033[90m[○]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "PRESERVED") == -1 ||
        consume_through(session, "LATE", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "$gsh> PRESERVED", TEST_TIMEOUT_MS) == -1 ||
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
                        "/bin/cat\r\n\r\n\033[90m[○]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "fg\r") == -1 ||
        consume_through(session, "[focused cell ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "BEFORE_STOP\r") == -1 ||
        consume_through(session, "BEFORE_STOP",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\032", 1) == -1 ||
        consume_through(session, "[stopped]", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "\033[90m[○]\033[0m $gsh> ",
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
    static const char command[] =
        "/bin/sh -c 'sleep .2; stty -echo; echo PRIVATE_INPUT; read x; "
        "stty echo; echo PRIVATE_ACCEPTED'\r";
    static const char secret[] = "PRIVATE_SECRET_42\r";

    session->capture_length = 0;
    if (send_text(session, command) == -1 ||
        consume_through(session,
                        "\r\n\r\n\033[90m[○]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "PRESERVED") == -1 ||
        wait_for_output(session, "\r\nPRIVATE_INPUT\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session,
                        "\033[90m[○]\033[0m $gsh> PRESERVED",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, secret) == -1 ||
        wait_for_output(session, "PRIVATE_ACCEPTED", TEST_TIMEOUT_MS) == -1 ||
        capture_contains(session, "PRIVATE_SECRET_42") ||
        consume_through(session, "PRIVATE_ACCEPTED", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session,
                        "\n\033[90m[●]\033[0m $gsh> PRESERVED",
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
        wait_for_output(session, "$gsh> DETACHED_EDITOR",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        send_text(session, "fg\r") == -1 ||
        wait_for_output(session, "[focused cell ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "q") == -1 ||
        consume_through(session, "FULLSCREEN_EXITED",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "\033[90m[●]\033[0m $gsh> ",
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
                        "| /bin/cat\r\n\r\n\033[90m[○]\033[0m $gsh> ",
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
        consume_through(session, "\r\nORDER_STATE=/",
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

static int managed_repl_resize(pty_session *session)
{
    if (send_text(session, "RESIZE_KEEP") == -1 ||
        consume_through(session, "$gsh> RESIZE_KEEP",
                        TEST_TIMEOUT_MS) == -1 ||
        resize_session(session, 12, 40) == -1 ||
        consume_through(session, "$gsh> RESIZE_KEEP",
                        TEST_TIMEOUT_MS) == -1 ||
        resize_session(session, 24, 80) == -1 ||
        consume_through(session, "$gsh> RESIZE_KEEP",
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
        consume_through(session, "$gsh> /bin/sleep 30",
                        TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1) {
        return -1;
    }
    return 0;
}

static int managed_repl_toggle(pty_session *session)
{
    uint64_t start;

    session->capture_length = 0;
    if (send_text(session, "/bin/sleep 2\r") == -1 ||
        wait_for_output(session, "\033[90m[○]\033[0m $gsh> ",
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
        wait_for_output(session, "\033[90m[●]\033[0m $gsh> ",
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
        wait_for_output(session, "\033[90m[●]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        capture_contains(session, "async repl: on pending")) {
        return -1;
    }
    session->capture_length = 0;
    if (send_text(session, "/async\r") == -1 ||
        wait_for_output(session, "\033[?1049lasync repl: off",
                        TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/bin/sleep 1 &\r") == -1 ||
        wait_for_output(session, "[1] ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/async\r") == -1 ||
        wait_for_output(session, "async repl: on pending",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "\033[?1049h", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "async repl: on", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(session, "\033[90m[●]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1) {
        return -1;
    }
    return 0;
}

static int managed_async_repl_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-managed-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        start_managed_session(&session, executable, fixture) == -1) {
        perror("pty managed: setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "\033[90m[●]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        managed_repl_indicator(&session) == -1 ||
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
        managed_repl_resize(&session) == -1 ||
        managed_repl_toggle(&session) == -1 ||
        managed_repl_saturation(&session) == -1) {
        perror("pty managed: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        fprintf(stderr, "pty managed: shell did not exit cleanly\n");
        failed = 1;
    }
    {
        char config_path[PATH_MAX];

        if (snprintf(config_path, sizeof(config_path), "%s/.gshrc", fixture) >=
                (int)sizeof(config_path) ||
            access(config_path, F_OK) == 0 || errno != ENOENT) {
            fprintf(stderr, "pty managed: /async modified configuration\n");
            failed = 1;
        }
    }
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "export GSH_PTY_EXPORT='alpha beta'\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printenv GSH_PTY_EXPORT\r") == -1 ||
        consume_through(&session, "\nalpha beta\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_PTY_COMPOUND=committed export -p >/dev/null; :\r") ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PTY_COMPOUND\"\r") ==
            -1 ||
        consume_through(&session, "<committed>\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "readonly GSH_PTY_READONLY=locked\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unset GSH_PTY_READONLY\r") == -1 ||
        consume_through(&session, "gsh: unset: variable is readonly",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PTY_READONLY\"\r") ==
            -1 ||
        consume_through(&session, "<locked>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unset GSH_PTY_EXPORT\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"${GSH_PTY_EXPORT+set}\"\r") == -1 ||
        consume_through(&session, "<>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "export GSH_PTY_PIPE=before; "
                  "export GSH_PTY_PIPE=inside | true\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PTY_PIPE\"\r") == -1 ||
        consume_through(&session, "<before>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "  for GSH_PTY_FOR in first last; do :; done  \r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PTY_FOR\"\r") == -1 ||
        consume_through(&session, "<last>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "for GSH_PTY_STATUS in one two; do false; done\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' \"$?\"\r") == -1 ||
        consume_through(&session, "<1>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "for GSH_PTY_QUOTED in 'a b' ''; do :; done\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$GSH_PTY_QUOTED\"\r") ==
            -1 ||
        consume_through(&session, "<>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- 'one two' '' three\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$#|$1|$2|$3\"\r") == -1 ||
        consume_through(&session, "<3|one two||three>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "shift\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$#|$1|$2\"\r") == -1 ||
        consume_through(&session, "<2||three>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- compound; :\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$#|$1\"\r") == -1 ||
        consume_through(&session, "<1|compound>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- redirected >/dev/null\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$1\"\r") == -1 ||
        consume_through(&session, "<redirected>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- inside | true\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$1\"\r") == -1 ||
        consume_through(&session, "<redirected>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -f\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' \"$-\"\r") ==
            -1 ||
        consume_through(&session, "<fi>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "if true; then set -C; fi\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' \"$-\"\r") ==
            -1 ||
        consume_through(&session, "<Cfi>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +Cf\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -f | true\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' \"$-\"\r") ==
            -1 ||
        consume_through(&session, "<i>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unset GSH_PTY_AUTO; set -a\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "GSH_PTY_AUTO=value\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +a\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printenv GSH_PTY_AUTO\r") == -1 ||
        consume_through(&session, "\nvalue\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unset GSH_PTY_NOUNSET; set -u\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, ": \"$GSH_PTY_NOUNSET\"\r") == -1 ||
        consume_through(&session,
                        "GSH_PTY_NOUNSET: parameter null or not set",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf '<%s>\\n' \"$-\"\r") ==
            -1 ||
        consume_through(&session, "<iu>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +u\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "GSH_PTY_ARITH=1\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  ": \"$((GSH_PTY_ARITH=2, 1 / 0))\"\r") == -1 ||
        consume_through(&session,
                        "gsh: arithmetic expansion: division by zero",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"$?|$GSH_PTY_ARITH\"\r") == -1 ||
        consume_through(&session, "<1|1>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "if true; then cd ..; fi\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/test \"$PWD\" = \"$(/bin/pwd)\" && "
                  "/usr/bin/printf GSH_CD_COMMITTED\r") == -1 ||
        consume_through(&session, "GSH_CD_COMMITTED",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- before\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "set -auf -- after; /usr/bin/printf 'GSH_SET_%s' READY; "
                  "/bin/sleep 5\r") == -1 ||
        consume_through(&session, "GSH_SET_READY", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$1|$-\"\r") == -1 ||
        consume_through(&session, "<before|i>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty variables: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        fprintf(stderr, "pty variables: command left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        fprintf(stderr, "pty variables: shell did not exit cleanly\n");
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_say=/bin/echo\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_say interactive\r") == -1 ||
        consume_through(&session, "\ninteractive\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_say\r") == -1 ||
        consume_through(&session, "gsh_say='/bin/echo'\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then alias gsh_compound=/bin/echo; fi\r") ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_compound committed\r") == -1 ||
        consume_through(&session, "\ncommitted\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "(unalias gsh_compound)\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_compound isolated\r") == -1 ||
        consume_through(&session, "\nisolated\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unalias gsh_say | /bin/cat\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_say pipeline-isolated\r") == -1 ||
        consume_through(&session, "\npipeline-isolated\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias 'gsh_prefix=gsh_say '\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_prefix forced\r") == -1 ||
        consume_through(&session, "\nforced\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unalias gsh_say > alias.out\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/test ! -s alias.out\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_say\r") == -1 ||
        consume_through(&session, "alias is not defined", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unalias -a\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty alias: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        fprintf(stderr, "pty alias: command left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        fprintf(stderr, "pty alias: shell did not exit cleanly\n");
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "PATH=/bin; hash sh >hash.out\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash\r") == -1 ||
        consume_through(&session, "sh=/bin/sh\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "(hash -r)\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash\r") == -1 ||
        consume_through(&session, "sh=/bin/sh\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "PATH=$PATH\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "hash >after.out; /bin/test ! -s after.out && "
                  "/usr/bin/printf GSH_HASH_PATH_CLEAR\r") == -1 ||
        consume_through(&session, "GSH_HASH_PATH_CLEAR",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then sh -c 'exit 0'; fi\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash\r") == -1 ||
        consume_through(&session, "sh=/bin/sh\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash -r >clear.out\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "hash >clear.out; /bin/test ! -s clear.out && "
                  "/usr/bin/printf GSH_HASH_COMMIT_CLEAR\r") == -1 ||
        consume_through(&session, "GSH_HASH_COMMIT_CLEAR",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty hash: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        fprintf(stderr, "pty hash: command left child processes\n");
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "times >direct.out; "
                  "/bin/test \"$(/usr/bin/wc -l <direct.out)\" -eq 2 && "
                  "/usr/bin/printf GSH_TIMES_DIRECT\r") == -1 ||
        consume_through(&session, "GSH_TIMES_DIRECT", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then times >compound.out; fi; "
                  "/bin/test \"$(/usr/bin/wc -l <compound.out)\" -eq 2 && "
                  "/usr/bin/printf GSH_TIMES_COMPOUND\r") == -1 ||
        consume_through(&session, "GSH_TIMES_COMPOUND", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_TIMES_VALUE=before; "
                  "GSH_TIMES_VALUE=after times >/dev/null; "
                  "/bin/test \"$GSH_TIMES_VALUE\" = after && "
                  "/usr/bin/printf GSH_TIMES_ASSIGN\r") == -1 ||
        consume_through(&session, "GSH_TIMES_ASSIGN", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "times unexpected\r") == -1 ||
        consume_through(&session, "does not accept operands",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/printf GSH_TIMES_RECOVERED\r") == -1 ||
        consume_through(&session, "GSH_TIMES_RECOVERED", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty times: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        fprintf(stderr, "pty times: command left child processes\n");
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
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        (setup != NULL &&
         (send_text(&session, setup) == -1 ||
          consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1)) ||
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
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then exec >managed.out; fi\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf GSH_MANAGED_EXEC_DESCRIPTOR\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "exec 1>/dev/tty\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/cat managed.out\r") == -1 ||
        consume_through(&session, "GSH_MANAGED_EXEC_DESCRIPTOR",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
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
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "command exec 9>&3\r") == -1 ||
        consume_through(&session, "gsh: exec redirection:",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/true\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_EXEC_KEEP=value exec 3>descriptor.out\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf persisted >&3; exec 3>&-; "
                  "/bin/test \"$GSH_EXEC_KEEP\" = value && "
                  "/bin/cat descriptor.out\r") == -1 ||
        consume_through(&session, "persisted", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then GSH_EXEC_COMPOUND=value "
                  "exec 4>compound.out; fi\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf compound >&4; exec 4>&-; "
                  "/bin/test \"$GSH_EXEC_COMPOUND\" = value && "
                  "/bin/cat compound.out\r") == -1 ||
        consume_through(&session, "compound", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if true; then command exec 2>compound.err "
                  "/definitely/missing; fi\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf GSH_EXEC_COMPOUND_REDIRECT >&2; "
                  "/bin/cat compound.err\r") == -1 ||
        consume_through(&session, "GSH_EXEC_COMPOUND_REDIRECT",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "command exec 2>failure.err /definitely/missing\r") ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf GSH_EXEC_REDIRECT_PERSISTED >&2; "
                  "/bin/cat failure.err\r") == -1 ||
        consume_through(&session, "GSH_EXEC_REDIRECT_PERSISTED",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "gsh_fn() { GSH_FN_VALUE=after; "
                  "/usr/bin/printf '<%s:%s>\\n' \"$#\" \"$1\"; "
                  "return 7; /usr/bin/printf BAD; }\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_fn value\r") == -1 ||
        consume_through(&session, "<1:value>\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"$?|$GSH_FN_VALUE\"\r") == -1 ||
        consume_through(&session, "<7|after>\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unset -f gsh_fn\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_fn\r") == -1 ||
        consume_through(&session, "command not found", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "{ gsh_tx(){ /usr/bin/printf TX1; }; }\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_tx\r") == -1 ||
        consume_through(&session, "TX1", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "{ gsh_tx(){ /usr/bin/printf TX2; }; }\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_tx\r") == -1 ||
        consume_through(&session, "TX2", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "{ unset -f gsh_tx; }\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_tx\r") == -1 ||
        consume_through(&session, "command not found", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty function: flow");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        fprintf(stderr, "pty function: command left child processes\n");
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        fprintf(stderr, "pty function: shell did not exit cleanly\n");
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
    memset(subject, '0', sizeof(subject) - 2U);
    subject[sizeof(subject) - 2U] = 'z';
    subject[sizeof(subject) - 1U] = '\0';
    fixture_length = strlen(fixture);
    if (12000U + fixture_length + 1U > sizeof(directory_value)) {
        (void)rmdir(fixture);
        return 1;
    }
    memset(directory_value, '@', 12000U);
    memcpy(directory_value + 12000U, fixture, fixture_length + 1U);
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_PTY_DEFERRED="
                  "${GSH_PTY_PATTERN_LONG##?*?*?*z}\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' "
                  "\"$GSH_PTY_DEFERRED\"\r") == -1 ||
        consume_through(&session, "<>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "cd \"${GSH_PTY_PATTERN_CD##*@*@}\"\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/pwd\r") == -1 ||
        consume_through(&session, fixture, TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty async: base prompt");
        failed = 1;
        goto done;
    }

    prompt_start = monotonic_ns();
    if (send_text(&session, "/bin/sleep 1 &\r") == -1 ||
        consume_through(&session, "] ", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty async: nonblocking launch");
        failed = 1;
        goto done;
    }
    prompt_duration = monotonic_ns() - prompt_start;
    if (prompt_duration >= 700000000ULL ||
        send_text(&session,
                  "/bin/test \"$!\" -gt 0 && "
                  "/usr/bin/printf GSH_ASYNC_PID\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_PID", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait \"$!\"\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/test \"$?\" -eq 0 && "
                  "/usr/bin/printf GSH_ASYNC_WAIT\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_WAIT", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/sh -c 'exit 7' &\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait \"$!\"\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/test \"$?\" -eq 7 && "
                  "/usr/bin/printf GSH_ASYNC_STATUS\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_STATUS", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/sleep 0.02 & /usr/bin/printf "
                  "'GSH_ASYNC_MIXED:%s\\n' \"$!\"\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_MIXED:", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/sh -c 'exit 9' & wait \"$!\"; "
                  "/bin/test \"$?\" -eq 9 && "
                  "/usr/bin/printf GSH_ASYNC_SEQUENCE\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_SEQUENCE",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/sh -c 'exit 11' & wait \"$!\" && false || "
                  "/usr/bin/printf GSH_ASYNC_ANDOR\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_ANDOR",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/true & wait \"$!\" && "
                  "/usr/bin/printf GSH_ASYNC_AND\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_AND", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        ((prompt_start = monotonic_ns()),
         send_text(&session,
                   "/bin/sleep 5 & wait \"$(/bin/sleep 2; "
                   "/usr/bin/printf 1)\"\r") == -1) ||
        consume_through(&session,
                        "wait expansion requires isolated continuation",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        monotonic_ns() - prompt_start >= 700000000ULL ||
        send_text(&session, "/bin/kill \"$!\"\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait \"$!\"\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/sleep 5 &\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait \"$!\"\r") == -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "^C", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/bin/test \"$?\" -eq 130 && "
                  "/usr/bin/printf GSH_ASYNC_CANCEL\r") == -1 ||
        consume_through(&session, "GSH_ASYNC_CANCEL", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/bin/kill \"$!\"\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "wait \"$!\"\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty async: semantics");
        dump_capture(&session);
        failed = 1;
        goto done;
    }
    if (process_child_count(session.pid) != 1) {
        fprintf(stderr, "pty async: completed jobs left child processes\n");
        failed = 1;
    }

done:
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "umask 077\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        snprintf(command, sizeof(command), ": >%s\r", created) >=
            (int)sizeof(command) || send_text(&session, command) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        stat(created, &information) == -1 ||
        (information.st_mode & 0777) != 0600 ||
        send_text(&session, "set -C\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        snprintf(command, sizeof(command), ": >%s\r", fifo) >=
            (int)sizeof(command) ||
        send_text(&session, command) == -1 ||
        consume_through(&session, fifo, TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\003", 1) == -1 ||
        consume_through(&session, "^C", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "worker=on", "busy=0") == -1) {
        perror("pty redirection: cancellation");
        dump_capture(&session);
        failed = 1;
    }
    if (!failed && process_child_count(session.pid) != 1) {
        fprintf(stderr, "pty redirection: worker was not recovered\n");
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

static int stalled_worker_flow(const char *executable)
{
    char fixture[] = "/tmp/gsh-pty-stall-XXXXXX";
    pty_session session;
    uint64_t key_start;
    uint64_t key_duration;
    uint64_t query_deadline;
    bool disabled = false;
    int failed = 0;

    if (mkdtemp(fixture) == NULL || write_head(fixture, true) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty smoke: stall setup");
        remove_fixture(fixture);
        return 1;
    }
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty smoke: base prompt");
        failed = 1;
        goto done;
    }

    key_start = monotonic_ns();
    if (send_text(&session, "x") == -1 ||
        consume_through(&session, "x", TEST_TIMEOUT_MS) == -1) {
        perror("pty smoke: key during stalled worker");
        failed = 1;
        goto done;
    }
    key_duration = monotonic_ns() - key_start;
    if (key_duration >= 100000000ULL) {
        fprintf(stderr,
                "pty smoke: stalled worker held key output for %.3f ms\n",
                (double)key_duration / 1000000.0);
        failed = 1;
    }

    if (send_bytes(&session, "\025", 1) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
        goto done;
    }
    query_deadline = monotonic_ns() + 2000000000ULL;
    while (monotonic_ns() < query_deadline) {
        session.capture_length = 0;
        if (send_text(&session, "rt\r") == -1 ||
            wait_for_output(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
            failed = 1;
            break;
        }
        if (capture_contains(&session, "worker=off") &&
            capture_contains(&session, "timeouts=1")) {
            disabled = true;
            break;
        }
        session.capture_length = 0;
    }
    if (!disabled) {
        fprintf(stderr, "pty smoke: stalled worker was not disabled\n");
        failed = 1;
    }

done:
    if (stop_session(&session) == -1) {
        fprintf(stderr, "pty smoke: stalled-worker shell did not exit cleanly\n");
        failed = 1;
    }
    remove_fixture(fixture);
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "x") == -1 ||
        consume_through(&session, "x", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\025", 1) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "worker=off", counter) == -1) {
        fprintf(stderr, "pty fault: worker case failed: %s\n", fault);
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        fprintf(stderr, "pty fault: worker cleanup failed: %s\n", fault);
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int worker_surviving_fault_case(const char *executable,
                                       const char *fault,
                                       const char *counter)
{
    char fixture[] = "/tmp/gsh-fault-stale-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL ||
        setenv("GSH_FAULT", fault, 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fault: stale setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "worker=on", counter) == -1) {
        fprintf(stderr, "pty fault: surviving case failed: %s\n", fault);
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
                              const char *command, const char *diagnostic)
{
    char fixture[] = "/tmp/gsh-fault-command-XXXXXX";
    pty_session session;
    int failed = 0;

    if (mkdtemp(fixture) == NULL || setenv("GSH_FAULT", fault, 1) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty fault: command setup");
        (void)unsetenv("GSH_FAULT");
        (void)rmdir(fixture);
        return 1;
    }
    (void)unsetenv("GSH_FAULT");
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, diagnostic, TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "job=idle", NULL) == -1) {
        fprintf(stderr, "pty fault: command case failed: %s\n", fault);
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        fprintf(stderr, "pty fault: command cleanup failed: %s\n", fault);
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- before\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set -- after; :\r") == -1 ||
        consume_through(&session, diagnostic, TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s>\\n' \"$#|$1\"\r") == -1 ||
        consume_through(&session, "<1|before>\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        fprintf(stderr, "pty fault: positional case failed: %s\n", fault);
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if /usr/bin/true; then cd /; fi\r") == -1 ||
        consume_through(&session, diagnostic, TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "GSH_DIRECTORY_ROLLED_BACK",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        fprintf(stderr, "pty fault: directory case failed: %s\n", fault);
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_keep=/bin/echo\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "if /usr/bin/true; then "
                  "alias gsh_keep=/usr/bin/false; "
                  "alias gsh_new=/usr/bin/true; fi\r") == -1 ||
        consume_through(&session, "gsh: state transaction rejected",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_keep rolled-back\r") == -1 ||
        consume_through(&session, "\nrolled-back\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_new\r") == -1 ||
        consume_through(&session, "alias is not defined", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        fprintf(stderr, "pty fault: alias commit rollback failed\n");
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "PATH=/bin\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash sh\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "if true; then hash -r; fi\r") == -1 ||
        consume_through(&session, "gsh: state transaction rejected",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "hash\r") == -1 ||
        consume_through(&session, "sh=/bin/sh\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        fprintf(stderr, "pty fault: command cache rollback failed\n");
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "gsh_keep(){ /usr/bin/printf ORIGINAL; }\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "{ gsh_keep(){ /usr/bin/printf CHANGED; }; "
                  "gsh_new(){ :; }; }\r") == -1 ||
        consume_through(&session, "gsh: state transaction rejected",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_keep\r") == -1 ||
        consume_through(&session, "ORIGINAL", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_new\r") == -1 ||
        consume_through(&session, "command not found", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        fprintf(stderr, "pty fault: function commit rollback failed: %s\n",
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
    return command_fault_case(executable, fault,
                              "/bin/cat <<EOF\rvalue\rEOF\r",
                              diagnostic);
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
        fprintf(stderr, "pty fault: fatal case failed: %s\n", fault);
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int wait_fault_child(pid_t pid, int *status)
{
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

static int noninteractive_fault_case(const char *executable,
                                     const char *fault,
                                     const char *command, int expected,
                                     const char *diagnostic)
{
    FILE *capture = tmpfile();
    char output[4096];
    size_t length = 0;
    int status = 0;
    pid_t pid;

    if (capture == NULL) {
        return 1;
    }
    pid = fork();
    if (pid == 0) {
        int descriptor = fileno(capture);

        if (setenv("GSH_FAULT", fault, 1) == -1 ||
            dup2(descriptor, STDOUT_FILENO) == -1 ||
            dup2(descriptor, STDERR_FILENO) == -1) {
            _exit(126);
        }
        execl(executable, executable, "--native-only", "-c", command,
              (char *)NULL);
        _exit(127);
    }
    if (pid == -1 || wait_fault_child(pid, &status) == -1) {
        (void)fclose(capture);
        return 1;
    }
    if (fseek(capture, 0, SEEK_SET) == 0) {
        length = fread(output, 1, sizeof(output), capture);
    }
    (void)fclose(capture);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != expected ||
        find_bytes((const unsigned char *)output, length, diagnostic) == NULL) {
        fprintf(stderr, "pty fault: non-interactive case failed: %s\n",
                fault);
        return 1;
    }
    return 0;
}

static int fault_injection_flow(const char *executable)
{
    static const struct {
        const char *name;
        const char *counter;
    } worker_cases[] = {
        {"worker-socket", "failures=1"},
        {"worker-fork", "failures=1"},
        {"worker-send", "failures=1"},
        {"worker-crash", "failures=1"},
        {"worker-close", "failures=1"},
        {"worker-malformed", "failures=1"},
        {"worker-stall", "timeouts=1"},
    };
    static const struct {
        const char *name;
        const char *command;
        const char *diagnostic;
    } command_cases[] = {
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
        {"exec-outcome-pipe",
         "if true; then exec /usr/bin/true; fi\r",
         "gsh: exec outcome pipe:"},
        {"exec-descriptor-socket",
         "if true; then exec /usr/bin/true; fi\r",
         "gsh: exec descriptor socket:"},
        {"exec-owner-descriptor-relocation",
         "exec 3>/dev/null\r",
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
                                     command_cases[index].diagnostic);
    }
    failed |= worker_surviving_fault_case(executable, "worker-stale",
                                          "stale=1");
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
        executable, "descriptor-save", "eval : >/dev/null", 125,
        "source redirection save");
    failed |= noninteractive_fault_case(
        executable, "redirect-open", "eval : >/dev/null", 1,
        "source redirection");
    failed |= noninteractive_fault_case(
        executable, "descriptor-dup", "eval : 1>&2", 1,
        "source redirection");
    for (index = 0; index < sizeof(fatal_cases) / sizeof(fatal_cases[0]);
         index++) {
        failed |= fatal_fault_case(executable, fatal_cases[index]);
    }
    if (!failed) {
        puts("pty fault: 81 deterministic boundary failures passed");
    }
    return failed;
}

static unsigned long diagnostic_counter(const pty_session *session,
                                        const char *name, bool *found)
{
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "ulimit -S -n 6\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/true\r") == -1 ||
        consume_through(&session, "gsh: pipe:", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, restore) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/true\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "job=idle", NULL) == -1) {
        fprintf(stderr, "pty resource: descriptor recovery failed\n");
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

    memset(input, 'a', sizeof(input));
    if (mkdtemp(fixture) == NULL ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty resource: input setup");
        (void)rmdir(fixture);
        return 1;
    }
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, input, sizeof(input)) == -1 ||
        consume_through(&session, "\a", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\025", 1) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty resource: bounded line failed");
        dump_capture(&session);
        failed = 1;
        goto done;
    }

    session.capture_length = 0;
    if (send_text(&session, "rt\r") == -1 ||
        wait_for_output(&session, "reactor cycles=", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
        goto done;
    }
    overloads = diagnostic_counter(&session, "overloads=", &found);
    if (!found || overloads < 1) {
        fprintf(stderr, "pty resource: line overload was not recorded\n");
        failed = 1;
        goto done;
    }

    session.capture_length = 0;
    if (send_text(&session, "kept") == -1 ||
        consume_through(&session, "kept", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
        goto done;
    }
    for (signal_count = 0; signal_count < 10000; signal_count++) {
        if (kill(session.pid, SIGWINCH) == -1 && errno != ESRCH) {
            failed = 1;
            goto done;
        }
    }
    if (consume_through(&session, "\033[2K", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "kept", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(&session, "\025", 1) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        fprintf(stderr, "pty resource: signal storm corrupted the editor\n");
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
         consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
         send_text(&session, "/usr/bin/true\r") == -1 ||
         consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1)) {
        fprintf(stderr,
                "pty resource: multiline input recovery failed\n");
        dump_capture(&session);
        failed = 1;
    }

done:
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
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
        fprintf(stderr,
                "pty resource: initialization exhaustion did not restore "
                "the terminal\n");
        failed = 1;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
        puts("pty resource: initialization RLIMIT_NOFILE=enforced");
    } else if (capture_contains(&session,
                                "error while loading shared libraries") ||
               capture_contains(&session, "rosetta error:")) {
        puts("pty resource: initialization "
             "RLIMIT_NOFILE=unsupported-pre-exec");
    } else if (linux_is_translated()) {
        puts("pty resource: initialization "
             "RLIMIT_NOFILE=unsupported-emulated");
    } else {
        fprintf(stderr,
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
        goto done;
    }
    session.capture_length = 0;
    if (send_text(&session, "/usr/bin/true\r") == -1 ||
        wait_for_output(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
        goto done;
    }
    enforced = capture_contains(&session, "gsh: fork:");
    if (wait_for_diagnostics(&session, "job=idle", NULL) == -1) {
        failed = 1;
        goto done;
    }
    printf("pty resource: RLIMIT_NPROC=%s\n",
           enforced ? "enforced" : "unsupported-or-unenforced");

done:
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
#else
    (void)executable;
    puts("pty resource: RLIMIT_NPROC=unsupported");
    return 0;
#endif
}

static int data_exhaustion_case(const char *executable)
{
    char fixture[] = "/tmp/gsh-resource-data-XXXXXX";
    char restore[64];
    pty_session session;
    struct rlimit original;
    bool enforced;
    int failed = 0;

    if (linux_is_translated()) {
        (void)executable;
        puts("pty resource: RLIMIT_DATA=unsupported-emulated");
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
        goto done;
    }
    session.capture_length = 0;
    if (send_text(&session, "ulimit -S -d 1024\r") == -1 ||
        wait_for_output(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        fprintf(stderr, "pty resource: data limit setup failed\n");
        failed = 1;
        goto done;
    }
    if (capture_contains(&session, "gsh: ulimit:")) {
        puts("pty resource: RLIMIT_DATA=unsupported");
        goto done;
    }
    session.capture_length = 0;
    if (send_text(&session, "/usr/bin/true\r") == -1 ||
        wait_for_output(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        fprintf(stderr, "pty resource: data exhaustion was not contained\n");
        failed = 1;
        goto done;
    }
    enforced = capture_contains(&session, "Cannot allocate memory") ||
               capture_contains(&session, "cannot allocate memory") ||
               capture_contains(&session, "error while loading") ||
               capture_contains(&session, "rosetta error:");
    session.capture_length = 0;
    if (send_text(&session, restore) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "/usr/bin/true\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "job=idle", NULL) == -1) {
        fprintf(stderr, "pty resource: data recovery failed\n");
        failed = 1;
        goto done;
    }
    printf("pty resource: RLIMIT_DATA=%s\n",
           enforced ? "enforced" : "unsupported-or-unenforced");

done:
    if (session.pid > 0 && stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "gsh: assignment:",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printenv GSH_RESOURCE_JOURNAL_0\r") == -1 ||
        consume_through(&session, "\nx\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "/usr/bin/printf '<%s:%s>\\n' "
                  "\"$GSH_RESOURCE_JOURNAL_0\" "
                  "\"${GSH_RESOURCE_JOURNAL_64+set}\"\r") == -1 ||
        consume_through(&session, "<x:>\r\n", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "set +a\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        fprintf(stderr, "pty resource: variable journal recovery failed\n");
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "gsh: parameter assignment failed",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "GSH_RESOURCE_ARITH_RECOVERY=7; "
                  "/bin/test \"$GSH_RESOURCE_ARITH_RECOVERY\" = 7\r") ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        fprintf(stderr,
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, first) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, second) == -1 ||
        consume_through(&session, "alias capacity exceeded",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "unalias -a\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_recovered=/usr/bin/true\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_recovered\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        fprintf(stderr, "pty resource: alias store recovery failed\n");
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_atomic_keep=/bin/echo\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, command) == -1 ||
        consume_through(&session, "alias transaction capacity exceeded",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "gsh: state transaction rejected",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_atomic_keep rolled-back\r") == -1 ||
        consume_through(&session, "\nrolled-back\r\n", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_atomic_00\r") == -1 ||
        consume_through(&session, "alias is not defined", TEST_TIMEOUT_MS) ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "alias gsh_atomic_recovery=/usr/bin/true\r") ==
            -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_atomic_recovery\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        fprintf(stderr, "pty resource: alias journal rollback failed\n");
        dump_capture(&session);
        failed = 1;
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
        goto done;
    }
    for (index = 0; index < 128U; index++) {
        if (snprintf(command, sizeof(command),
                     "gsh_resource_fn_%03zu() { :; }\r", index) >=
                (int)sizeof(command) ||
            send_text(&session, command) == -1 ||
            consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
            failed = 1;
            goto done;
        }
    }
    if (send_text(&session, "gsh_resource_fn_over() { :; }\r") == -1 ||
        consume_through(&session, "function definition: No space",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
        goto done;
    }
    for (index = 0; index < 2U; index++) {
        size_t name;

        used = (size_t)snprintf(command, sizeof(command), "unset -f");
        for (name = index * 64U; name < (index + 1U) * 64U; name++) {
            int length = snprintf(command + used, sizeof(command) - used,
                                  " gsh_resource_fn_%03zu", name);

            if (length < 0 || (size_t)length >= sizeof(command) - used) {
                failed = 1;
                goto done;
            }
            used += (size_t)length;
        }
        command[used++] = '\r';
        command[used] = '\0';
        if (send_text(&session, command) == -1 ||
            consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
            failed = 1;
            goto done;
        }
    }
    if (send_text(&session, "gsh_resource_recovered() { :; }\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session,
                  "gsh_resource_recursive() { "
                  "gsh_resource_recursive; }\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_resource_recursive\r") == -1 ||
        consume_through(&session, "function resource limit exceeded",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(&session, "gsh_resource_recovered\r") == -1 ||
        consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        process_child_count(session.pid) != 1) {
        failed = 1;
    }

done:
    if (failed) {
        fprintf(stderr, "pty resource: function recovery failed\n");
        dump_capture(&session);
    }
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
}

static int resource_exhaustion_flow(const char *executable)
{
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
        puts("pty resource: descriptor, input, process, data, variable, "
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
    closedir(directory);
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
    close(descriptor);
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
    close(fd);
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
    close(descriptor);
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

static int64_t process_tree_memory_bytes_at(pid_t pid, unsigned int depth)
{
    process_memory memory;
    pid_t children[256];
    int child_count;
    int64_t total;
    int index;

    if (depth == 8 || process_memory_bytes(pid, &memory) == -1 ||
        memory.current > INT64_MAX) {
        return -1;
    }
    total = (int64_t)memory.current;
    child_count = process_child_pids(
        pid, children, sizeof(children) / sizeof(children[0]));
    if (child_count < 0) {
        return depth == 0 ? -1 : total;
    }
    for (index = 0; index < child_count; index++) {
        int64_t child = process_tree_memory_bytes_at(children[index],
                                                     depth + 1U);

        if (child >= 0 && child <= INT64_MAX - total) {
            total += child;
        }
    }
    return total;
}

static int64_t process_tree_memory_bytes(pid_t pid)
{
    return process_tree_memory_bytes_at(pid, 0);
}

static int soak_iteration(pty_session *session)
{
    if (send_text(session, "edit") == -1 ||
        consume_through(session, "edit", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/usr/bin/true\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "/usr/bin/printf GSH_SOAK_OUTPUT | /bin/cat\r") == -1 ||
        consume_through(session, "\nGSH_SOAK_OUTPUT", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "GSH_SOAK_STATE=value\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "alias GSH_SOAK_ALIAS=:\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "GSH_SOAK_ALIAS\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "unalias GSH_SOAK_ALIAS\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "GSH_SOAK_ARITH=010; "
                  ": \"$((GSH_SOAK_ARITH += 1))\"; "
                  "/bin/test \"$GSH_SOAK_ARITH\" = 9\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  ": \"${GSH_SOAK_PIPE:=stage}\" | /usr/bin/true\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "/bin/test -z \"$GSH_SOAK_PIPE\" && "
                  "/usr/bin/printf GSH_SOAK_ISOLATED\r") == -1 ||
        consume_through(session, "\nGSH_SOAK_ISOLATED",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  ": \"$(/usr/bin/printf GSH_SOAK_SUBSTITUTION)\"\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/usr/bin/true &\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "wait \"$!\"\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "for GSH_SOAK_ITEM in a 'b c' ''; do :; done\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "set -- a 'b c' ''; shift\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session,
                  "set -aCfu; GSH_SOAK_AUTO=value; "
                  "/usr/bin/printenv GSH_SOAK_AUTO >/dev/null; "
                  ": \"${GSH_SOAK_KNOWN:=known}\"; set +aCfu\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "/bin/cat <<EOF\r") == -1 ||
        consume_through(session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "GSH_SOAK_HEREDOC\r") == -1 ||
        consume_through(session, "GSH_MORE> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "EOF\r") == -1 ||
        consume_through(session, "\nGSH_SOAK_HEREDOC\r\n",
                        TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "cancel") == -1 ||
        consume_through(session, "cancel", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\003", 1) == -1 ||
        consume_through(session, "^C", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "kept") == -1 ||
        consume_through(session, "kept", TEST_TIMEOUT_MS) == -1 ||
        kill(session->pid, SIGWINCH) == -1 ||
        consume_through(session, "\033[2K", TEST_TIMEOUT_MS) == -1 ||
        consume_through(session, "kept", TEST_TIMEOUT_MS) == -1 ||
        send_bytes(session, "\025", 1) == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        send_text(session, "cd .\r") == -1 ||
        consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(session, "busy=0", "job=idle") == -1) {
        return -1;
    }
    return 0;
}

static int soak_flow(const char *executable, unsigned long seconds)
{
    char fixture[] = "/tmp/gsh-soak-XXXXXX";
    pty_session session;
    uint64_t deadline;
    uint64_t iterations = 0;
    unsigned long reactor_cycles;
    unsigned long misses;
    int64_t initial_rss;
    int64_t maximum_rss;
    int64_t final_rss;
    int initial_fds;
    int maximum_fds;
    int final_fds;
    int initial_children;
    int maximum_children;
    int final_children;
    bool found;
    int failed = 0;

    if (seconds == 0 || seconds > 86400 ||
        unsetenv("GSH_SOAK_STATE") == -1 ||
        unsetenv("GSH_SOAK_ARITH") == -1 ||
        unsetenv("GSH_SOAK_PIPE") == -1 || mkdtemp(fixture) == NULL ||
        write_head(fixture, false) == -1 ||
        start_session(&session, executable, fixture, SHELL_GSH) == -1) {
        perror("pty soak: setup");
        remove_fixture(fixture);
        return 1;
    }
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1 ||
        consume_through(&session, "\033[90m[bench]\033[0m $gsh> ",
                        TEST_TIMEOUT_MS) == -1 ||
        wait_for_diagnostics(&session, "busy=0", "job=idle") == -1) {
        perror("pty soak: initial prompt");
        failed = 1;
        goto done;
    }

    initial_rss = process_rss_bytes(session.pid);
    initial_fds = process_fd_count(session.pid);
    initial_children = process_child_count(session.pid);
    if (initial_rss < 0 || initial_fds < 0 || initial_children < 0) {
        fprintf(stderr, "pty soak: platform process metrics unavailable\n");
        failed = 1;
        goto done;
    }
    maximum_rss = initial_rss;
    maximum_fds = initial_fds;
    maximum_children = initial_children;
    deadline = monotonic_ns() + (uint64_t)seconds * 1000000000ULL;
    while (monotonic_ns() < deadline) {
        int64_t rss;
        int descriptors;
        int children;

        if (soak_iteration(&session) == -1) {
            perror("pty soak: iteration");
            dump_capture(&session);
            failed = 1;
            goto done;
        }
        iterations++;
        rss = process_rss_bytes(session.pid);
        descriptors = process_fd_count(session.pid);
        children = process_child_count(session.pid);
        if (rss < 0 || descriptors < 0 || children < 0) {
            failed = 1;
            goto done;
        }
        if (rss > maximum_rss) {
            maximum_rss = rss;
        }
        if (descriptors > maximum_fds) {
            maximum_fds = descriptors;
        }
        if (children > maximum_children) {
            maximum_children = children;
        }
    }

    session.capture_length = 0;
    if (send_text(&session, "rt\r") == -1 ||
        wait_for_output(&session, "reactor cycles=", TEST_TIMEOUT_MS) == -1 ||
        wait_for_output(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        failed = 1;
        goto done;
    }
    reactor_cycles = diagnostic_counter(&session, "cycles=", &found);
    if (!found) {
        failed = 1;
        goto done;
    }
    misses = diagnostic_counter(&session, "misses=", &found);
    if (!found) {
        failed = 1;
        goto done;
    }
    final_rss = process_rss_bytes(session.pid);
    final_fds = process_fd_count(session.pid);
    final_children = process_child_count(session.pid);
    if (final_rss < 0 || final_fds < 0 || final_fds > initial_fds + 1 ||
        final_rss > initial_rss + 4 * 1024 * 1024 ||
        final_children < 0 || final_children > initial_children) {
        fprintf(stderr, "pty soak: resource growth exceeded bounds\n");
        failed = 1;
        goto done;
    }
    printf("pty soak: seconds=%lu iterations=%llu reactor_cycles=%lu "
           "misses=%lu rss_initial=%lld rss_max=%lld rss_final=%lld "
           "fds_initial=%d fds_max=%d fds_final=%d "
           "children_initial=%d children_max=%d children_final=%d\n",
           seconds, (unsigned long long)iterations, reactor_cycles, misses,
           (long long)initial_rss, (long long)maximum_rss,
           (long long)final_rss, initial_fds, maximum_fds, final_fds,
           initial_children, maximum_children, final_children);

done:
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    remove_fixture(fixture);
    return failed;
}

static void discard_ready_output(pty_session *session)
{
    unsigned char discard[4096];

    session->capture_length = 0;
    for (;;) {
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
            wait_for_output(session, " timeouts=", TEST_TIMEOUT_MS) == -1) {
            return -1;
        }
        busy = find_bytes(session->capture, session->capture_length, "busy=");
        idle = busy != NULL &&
               (size_t)(busy - session->capture) + sizeof("busy=") - 1U <
                   session->capture_length &&
               busy[sizeof("busy=") - 1U] == '0';
        if (consume_through(session, "busy=", TEST_TIMEOUT_MS) == -1 ||
            consume_through(session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
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

static int compare_u64(const void *left, const void *right)
{
    uint64_t first = *(const uint64_t *)left;
    uint64_t second = *(const uint64_t *)right;

    return first < second ? -1 : first > second ? 1 : 0;
}

static double percentile_ms(const uint64_t *samples, size_t count,
                            size_t numerator, size_t denominator)
{
    size_t rank = (count * numerator + denominator - 1U) / denominator;

    if (rank == 0) {
        rank = 1;
    }
    return (double)samples[rank - 1U] / 1000000.0;
}

static void print_metric(const char *label, uint64_t *samples, size_t count,
                         uint64_t deadline_ns)
{
    size_t misses = 0;
    size_t index;

    for (index = 0; index < count; index++) {
        if (samples[index] > deadline_ns) {
            misses++;
        }
    }
    qsort(samples, count, sizeof(samples[0]), compare_u64);
    printf("  %-23s n=%zu p50=%7.3f p95=%7.3f p99=%7.3f max=%7.3f "
           ">5ms=%zu\n",
           label, count, percentile_ms(samples, count, 50, 100),
           percentile_ms(samples, count, 95, 100),
           percentile_ms(samples, count, 99, 100),
           (double)samples[count - 1U] / 1000000.0, misses);
}

static int start_benchmark_session(pty_session *session,
                                   const shell_spec *spec,
                                   const char *directory, bool stabilize)
{
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
                fprintf(stderr, "pty benchmark: %s %s setup failed\n",
                        specs[shell].name, label);
                return -1;
            }
            discard_ready_output(&sessions[shell]);
            start = monotonic_ns();
            if (send_text(&sessions[shell], command) == -1 ||
                consume_through(&sessions[shell], specs[shell].prompt,
                                TEST_TIMEOUT_MS) == -1) {
                fprintf(stderr, "pty benchmark: %s %s sample failed\n",
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
                fprintf(stderr, "pty benchmark: %s %s setup failed\n",
                        specs[shell].name, label);
                return -1;
            }
            discard_ready_output(&sessions[shell]);
            start = monotonic_ns();
            if (send_text(&sessions[shell], command) == -1 ||
                consume_through(&sessions[shell], specs[shell].prompt,
                                TEST_TIMEOUT_MS) == -1) {
                fprintf(stderr, "pty benchmark: %s %s sample failed\n",
                        specs[shell].name, label);
                return -1;
            }
            samples[shell][sample] = monotonic_ns() - start;
        }
    }
    return 0;
}

static void print_raw_samples(const char *shell, const char *metric,
                              const uint64_t *samples, size_t count)
{
    size_t index;

    printf("raw-ns,%s,%s", shell, metric);
    for (index = 0; index < count; index++) {
        printf(",%llu", (unsigned long long)samples[index]);
    }
    putchar('\n');
}

static void print_raw_memory(const char *shell, const char *metric,
                             const uint64_t *samples, size_t count)
{
    size_t index;

    printf("raw-bytes,%s,%s", shell, metric);
    for (index = 0; index < count; index++) {
        printf(",%llu", (unsigned long long)samples[index]);
    }
    putchar('\n');
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

static int wait_for_output_sampling_tree(pty_session *session,
                                         const char *marker,
                                         uint64_t *peak)
{
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

static void print_memory_metric(const char *label, uint64_t *samples,
                                size_t count)
{
    qsort(samples, count, sizeof(samples[0]), compare_u64);
    printf("  %-32s n=%zu p50=%7.3f p95=%7.3f p99=%7.3f "
           "max=%7.3f MiB\n",
           label, count,
           (double)samples[(count * 50U + 99U) / 100U - 1U] /
               (1024.0 * 1024.0),
           (double)samples[(count * 95U + 99U) / 100U - 1U] /
               (1024.0 * 1024.0),
           (double)samples[(count * 99U + 99U) / 100U - 1U] /
               (1024.0 * 1024.0),
           (double)samples[count - 1U] / (1024.0 * 1024.0));
}

static int memory_benchmark(const shell_spec specs[BENCH_SHELLS],
                            const char *directory, size_t workload_begin,
                            size_t workload_count)
{
    static const memory_workload workloads[BENCH_MEMORY_WORKLOADS] = {
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
        {"export-assignment", NULL,
         "export GSH_BENCH_EXPORT=value\r", NULL},
        {"unset-variable", "GSH_BENCH_UNSET=value\r",
         "unset GSH_BENCH_UNSET\r", NULL},
        {"readonly-existing", "readonly GSH_BENCH_READONLY\r",
         "readonly GSH_BENCH_READONLY\r", NULL},
        {"for-explicit", NULL,
         "for GSH_BENCH_ITEM in a b c; do :; done\r", NULL},
        {"set-positionals", NULL, "set -- a b c\r", NULL},
        {"shift-positionals", "set -- a b c\r", "shift\r", NULL},
        {"set-options", "set +aCfu\r", "set -aCfu\r", NULL},
        {"allexport-assignment",
         "set +Cfu -a\r",
         "GSH_BENCH_ASSIGN=value\r", NULL},
        {"nounset-defined-lookup",
         "set +aCf -u\r",
         ": \"${GSH_BENCH_VALUE}\"\r", NULL},
        {"parameter-pattern-removal",
         NULL,
         ": \"${GSH_BENCH_PATTERN##*b}\"\r", NULL},
        {"parameter-pattern-multistar",
         NULL,
         ": \"${GSH_BENCH_PATTERN_MULTI#*a*d}\"\r", NULL},
        {"noglob-expansion", "set +aCu -f\r",
         "/usr/bin/printf '' /dev/n[uo]ll\r", NULL},
        {"noclobber-nonregular-redirection", "set +afu -C\r",
         ": >/dev/null\r", NULL},
        {"async-external-held", NULL, "/bin/sleep 0.05 &\r", "wait\r"},
        {"wait-completed", "/usr/bin/true &\r", "wait\r", NULL},
        {"alias-definition", NULL,
         "alias GSH_BENCH_ALIAS=:\r", NULL},
        {"alias-expansion", "alias GSH_BENCH_ALIAS=:\r",
         "GSH_BENCH_ALIAS\r", NULL},
        {"unalias", "alias GSH_BENCH_ALIAS=:\r",
         "unalias GSH_BENCH_ALIAS\r", NULL},
    };
    benchmark_memory results;
    size_t workload;
    size_t sample;
    size_t offset;
    size_t workload_end = workload_begin + workload_count;

    if (workload_begin > BENCH_MEMORY_WORKLOADS ||
        workload_count > BENCH_MEMORY_WORKLOADS - workload_begin) {
        return -1;
    }
    memset(&results, 0, sizeof(results));
    for (sample = 0; sample < BENCH_MEMORY_SAMPLES; sample++) {
        for (offset = 0; offset < BENCH_SHELLS; offset++) {
            size_t shell = (sample + offset) % BENCH_SHELLS;
            pty_session session;
            process_memory memory;
            int64_t tree;

            if (start_benchmark_session(&session, &specs[shell], directory,
                                        true) == -1 ||
                process_memory_bytes(session.pid, &memory) == -1) {
                fprintf(stderr, "pty benchmark: %s idle memory failed\n",
                        specs[shell].name);
                return -1;
            }
            tree = process_tree_memory_bytes(session.pid);
            if (tree < 0) {
                (void)stop_session(&session);
                return -1;
            }
            results.idle[shell][sample] = memory.current;
            results.idle_tree[shell][sample] = (uint64_t)tree;
            if (stop_session(&session) == -1) {
                return -1;
            }
        }
    }
    for (workload = workload_begin; workload < workload_end; workload++) {
        for (sample = 0; sample < BENCH_MEMORY_SAMPLES; sample++) {
            for (offset = 0; offset < BENCH_SHELLS; offset++) {
                size_t shell = (sample + offset) % BENCH_SHELLS;
                pty_session session;
                process_memory before;
                process_memory after;
                int64_t before_tree;
                int64_t tree;
                uint64_t tree_peak;

                if (start_benchmark_session(&session, &specs[shell],
                                            directory, true) == -1) {
                    return -1;
                }
                if (workloads[workload].setup != NULL &&
                    (send_text(&session, workloads[workload].setup) == -1 ||
                     consume_through(&session, specs[shell].prompt,
                                     TEST_TIMEOUT_MS) == -1)) {
                    (void)stop_session(&session);
                    return -1;
                }
                discard_ready_output(&session);
                if (process_memory_bytes(session.pid, &before) == -1) {
                    (void)stop_session(&session);
                    return -1;
                }
                before_tree = process_tree_memory_bytes(session.pid);
                if (before_tree < 0) {
                    (void)stop_session(&session);
                    return -1;
                }
                tree_peak = (uint64_t)before_tree;
                if (send_text(&session, workloads[workload].command) == -1 ||
                    consume_through(&session, specs[shell].prompt,
                                    TEST_TIMEOUT_MS) == -1) {
                    (void)stop_session(&session);
                    return -1;
                }
                if (workloads[workload].held_command != NULL) {
                    discard_ready_output(&session);
                    if (send_text(&session,
                                  workloads[workload].held_command) == -1 ||
                        wait_for_output_sampling_tree(
                            &session, specs[shell].prompt, &tree_peak) == -1) {
                        (void)stop_session(&session);
                        return -1;
                    }
                }
                if (process_memory_bytes(session.pid, &after) == -1) {
                    (void)stop_session(&session);
                    return -1;
                }
                tree = process_tree_memory_bytes(session.pid);
                if (tree >= 0 && (uint64_t)tree > tree_peak) {
                    tree_peak = (uint64_t)tree;
                }
                results.shell_peak[workload][shell][sample] = after.peak;
                results.shell_delta[workload][shell][sample] =
                    after.peak > before.current
                        ? after.peak - before.current
                        : 0;
                results.shell_growth[workload][shell][sample] =
                    after.peak > before.peak ? after.peak - before.peak : 0;
                results.tree_peak[workload][shell][sample] = tree_peak;
                results.tree_delta[workload][shell][sample] =
                    tree_peak > (uint64_t)before_tree
                        ? tree_peak - (uint64_t)before_tree
                        : 0;
                if (stop_session(&session) == -1) {
                    return -1;
                }
            }
        }
    }

#if defined(__APPLE__)
    puts("memory units: bytes raw, MiB summary; macOS physical footprint "
         "and lifetime maximum");
#elif defined(__linux__)
    puts("memory units: bytes raw, MiB summary; Linux VmRSS and VmHWM");
#else
    puts("memory units: bytes raw, MiB summary; current RSS only");
#endif
    puts("memory tree peak: aggregate current memory sampled every 1 ms; "
         "external and pipeline children are held for 20 ms");
    for (offset = 0; offset < BENCH_SHELLS; offset++) {
        printf("memory %s:\n", specs[offset].name);
        print_raw_memory(specs[offset].name, "startup-idle",
                         results.idle[offset], BENCH_MEMORY_SAMPLES);
        print_memory_metric("startup-idle", results.idle[offset],
                            BENCH_MEMORY_SAMPLES);
        print_raw_memory(specs[offset].name, "startup-idle-tree",
                         results.idle_tree[offset], BENCH_MEMORY_SAMPLES);
        print_memory_metric("startup-idle-tree", results.idle_tree[offset],
                            BENCH_MEMORY_SAMPLES);
        for (workload = workload_begin; workload < workload_end; workload++) {
            char label[96];

#define PRINT_MEMORY_FIELD(field, suffix)                                  \
    do {                                                                    \
        (void)snprintf(label, sizeof(label), "%s-%s",                    \
                       workloads[workload].label, suffix);                  \
        print_raw_memory(specs[offset].name, label,                         \
                         results.field[workload][offset],                   \
                         BENCH_MEMORY_SAMPLES);                             \
        print_memory_metric(label, results.field[workload][offset],         \
                            BENCH_MEMORY_SAMPLES);                          \
    } while (0)
            PRINT_MEMORY_FIELD(shell_peak, "shell-peak");
            PRINT_MEMORY_FIELD(shell_delta, "shell-delta-over-idle");
            PRINT_MEMORY_FIELD(shell_growth, "shell-peak-growth");
            PRINT_MEMORY_FIELD(tree_peak, "tree-peak");
            PRINT_MEMORY_FIELD(tree_delta, "tree-delta-over-idle");
#undef PRINT_MEMORY_FIELD
        }
    }
    return 0;
}

static int latency_benchmark(const char *gsh, const char *bash,
                             const char *zsh)
{
    shell_spec specs[BENCH_SHELLS] = {
        {"gsh", gsh, "$gsh> ", SHELL_GSH},
        {"bash", bash, "$gsh> ", SHELL_BASH},
        {"zsh", zsh, "$gsh> ", SHELL_ZSH},
    };
    uint64_t startup[BENCH_SHELLS][BENCH_STARTUP_SAMPLES];
    uint64_t key[BENCH_SHELLS][BENCH_KEY_SAMPLES];
    uint64_t execution[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
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
    uint64_t noglob_expansion[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t noclobber_redirection[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t async_launch[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t async_external_launch[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t wait_completed[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t alias_definition[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t alias_expansion[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t unalias_command[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    pty_session sessions[BENCH_SHELLS];
    bool started[BENCH_SHELLS] = {false, false, false};
    char directory[4096];
    struct utsname platform;
    size_t sample;
    size_t offset;
    int failed = 0;

    if (getcwd(directory, sizeof(directory)) == NULL ||
        setenv("GSH_BENCH_VALUE", "value", 1) == -1 ||
        setenv("GSH_BENCH_PATTERN", "abcabc", 1) == -1 ||
        setenv("GSH_BENCH_PATTERN_MULTI", "abacadTAIL", 1) == -1) {
        perror("pty benchmark: current directory");
        return 1;
    }
    for (offset = 0; offset < BENCH_SHELLS; offset++) {
        if (access(specs[offset].executable, X_OK) == -1) {
            fprintf(stderr, "pty benchmark: %s is not executable: %s\n",
                    specs[offset].name, specs[offset].executable);
            return 1;
        }
        for (sample = 0; sample < 8; sample++) {
            pty_session warm;

            if (start_benchmark_session(&warm, &specs[offset], directory,
                                        false) == -1 ||
                stop_session(&warm) == -1) {
                fprintf(stderr, "pty benchmark: %s warmup failed\n",
                        specs[offset].name);
                return 1;
            }
        }
    }

    for (sample = 0; sample < BENCH_STARTUP_SAMPLES; sample++) {
        for (offset = 0; offset < BENCH_SHELLS; offset++) {
            size_t shell = (sample + offset) % BENCH_SHELLS;
            pty_session fresh;
            uint64_t start = monotonic_ns();

            if (start_benchmark_session(&fresh, &specs[shell], directory,
                                        false) == -1) {
                fprintf(stderr, "pty benchmark: %s startup failed\n",
                        specs[shell].name);
                return 1;
            }
            startup[shell][sample] = monotonic_ns() - start;
            if (stop_session(&fresh) == -1) {
                fprintf(stderr, "pty benchmark: %s shutdown failed\n",
                        specs[shell].name);
                return 1;
            }
        }
    }

    for (offset = 0; offset < BENCH_SHELLS; offset++) {
        memset(&sessions[offset], 0, sizeof(sessions[offset]));
        sessions[offset].master = -1;
        if (start_benchmark_session(&sessions[offset], &specs[offset],
                                    directory, true) == -1) {
            fprintf(stderr, "pty benchmark: %s session failed\n",
                    specs[offset].name);
            failed = 1;
            goto done;
        }
        started[offset] = true;
    }

    for (sample = 0; sample < BENCH_KEY_SAMPLES; sample++) {
        for (offset = 0; offset < BENCH_SHELLS; offset++) {
            size_t shell = (sample + offset) % BENCH_SHELLS;
            uint64_t start;

            discard_ready_output(&sessions[shell]);
            start = monotonic_ns();
            if (send_text(&sessions[shell], "x") == -1 ||
                consume_through(&sessions[shell], "x", TEST_TIMEOUT_MS) ==
                    -1) {
                fprintf(stderr, "pty benchmark: %s key sample failed\n",
                        specs[shell].name);
                failed = 1;
                goto done;
            }
            key[shell][sample] = monotonic_ns() - start;
            if (send_bytes(&sessions[shell], "\025", 1) == -1) {
                failed = 1;
                goto done;
            }
        }
    }

    if (benchmark_prompt_command(sessions, specs, execution, NULL,
                                 "/usr/bin/true\r", "external true") ==
            -1 ||
        benchmark_prompt_command(
            sessions, specs, lookup, "GSH_BENCH_VALUE=value\r",
            ": \"${GSH_BENCH_VALUE}\"\r", "variable lookup") == -1 ||
        benchmark_prompt_command(
            sessions, specs, command_lookup, NULL,
            "command -v true\r", "command lookup") == -1 ||
        benchmark_prompt_command(
            sessions, specs, command_path_cache, "hash sh\r",
            "command -v sh\r", "cached command path lookup") == -1 ||
        benchmark_prompt_command(sessions, specs, assignment, NULL,
                                 "GSH_BENCH_ASSIGN=value\r",
                                 "variable assignment") == -1 ||
        benchmark_prompt_command(
            sessions, specs, assign_default, "GSH_BENCH_DEFAULT=\r",
            ": \"${GSH_BENCH_DEFAULT:=value}\"\r",
            "assign-default expansion") == -1 ||
        benchmark_prompt_command(
            sessions, specs, arithmetic_assignment,
            "GSH_BENCH_ARITH=1\r",
            ": \"$((GSH_BENCH_ARITH += 1))\"\r",
            "arithmetic assignment") == -1 ||
        benchmark_prompt_command(
            sessions, specs, pipeline_assignment, "GSH_BENCH_PIPE=\r",
            ": \"${GSH_BENCH_PIPE:=value}\" | /usr/bin/true\r",
            "pipeline scoped assignment") == -1 ||
        benchmark_prompt_command(sessions, specs, resource_limit, NULL,
                                 "ulimit -S -n\r", "ulimit soft nofile") ==
            -1 ||
        benchmark_prompt_command(sessions, specs, creation_mask, NULL,
                                 "umask\r", "umask report") ==
            -1 ||
        benchmark_prompt_command(sessions, specs, process_times, NULL,
                                 "times\r", "process times") == -1 ||
        benchmark_prompt_command(
            sessions, specs, exec_descriptor, "exec 9>&-\r",
            "exec 9>/dev/null\r", "exec descriptor commit") == -1 ||
        benchmark_prompt_command(
            sessions, specs, export_assignment, NULL,
            "export GSH_BENCH_EXPORT=value\r", "export assignment") == -1 ||
        benchmark_prompt_command(
            sessions, specs, unset_variable,
            "GSH_BENCH_UNSET=value\r", "unset GSH_BENCH_UNSET\r",
            "unset variable") == -1 ||
        benchmark_prompt_command(
            sessions, specs, readonly_existing,
            "readonly GSH_BENCH_READONLY\r",
            "readonly GSH_BENCH_READONLY\r", "readonly existing") == -1 ||
        benchmark_prompt_command(
            sessions, specs, for_explicit, NULL,
            "for GSH_BENCH_ITEM in a b c; do :; done\r",
            "for explicit items") == -1 ||
        benchmark_prompt_command(
            sessions, specs, set_positionals, NULL,
            "set -- a b c\r", "set positional parameters") == -1 ||
        benchmark_prompt_command(
            sessions, specs, shift_positionals, "set -- a b c\r",
            "shift\r", "shift positional parameters") == -1 ||
        benchmark_prompt_command(
            sessions, specs, set_options, "set +aCfu\r", "set -aCfu\r",
            "set shell options") == -1 ||
        benchmark_prompt_command(
            sessions, specs, allexport_assignment,
            "set +Cfu -a\r",
            "GSH_BENCH_ASSIGN=value\r", "allexport assignment") == -1 ||
        benchmark_prompt_command(
            sessions, specs, nounset_lookup,
            "set +aCf -u\r",
            ": \"${GSH_BENCH_VALUE}\"\r", "nounset defined lookup") ==
            -1 ||
        benchmark_prompt_command(
            sessions, specs, pattern_removal,
            NULL,
            ": \"${GSH_BENCH_PATTERN##*b}\"\r",
            "parameter pattern removal") == -1 ||
        benchmark_prompt_command(
            sessions, specs, pattern_multistar,
            NULL,
            ": \"${GSH_BENCH_PATTERN_MULTI#*a*d}\"\r",
            "parameter multi-star removal") == -1 ||
        benchmark_prompt_command(
            sessions, specs, noglob_expansion,
            "set +aCu -f\r",
            "/usr/bin/printf '' /dev/n[uo]ll\r",
            "noglob expansion") == -1 ||
        benchmark_prompt_command(
            sessions, specs, noclobber_redirection,
            "set +afu -C\r",
            ": >/dev/null\r", "noclobber nonregular redirection") ==
            -1 ||
        benchmark_prompt_command(
            sessions, specs, alias_definition,
            "alias GSH_BENCH_ALIAS=:\r",
            "alias GSH_BENCH_ALIAS=true\r", "alias definition update") ==
            -1 ||
        benchmark_prompt_command(
            sessions, specs, alias_expansion,
            "alias GSH_BENCH_ALIAS=:\r", "GSH_BENCH_ALIAS\r",
            "alias lookup expansion") == -1 ||
        benchmark_prompt_command(
            sessions, specs, unalias_command,
            "alias GSH_BENCH_ALIAS=:\r", "unalias GSH_BENCH_ALIAS\r",
            "unalias definition") ==
            -1) {
        failed = 1;
        goto done;
    }
    for (offset = 0; offset < BENCH_SHELLS; offset++) {
        if (stop_session(&sessions[offset]) == -1) {
            failed = 1;
            goto done;
        }
        started[offset] = false;
        if (start_benchmark_session(&sessions[offset], &specs[offset],
                                    directory, true) == -1) {
            failed = 1;
            goto done;
        }
        started[offset] = true;
    }
    if (
        benchmark_synchronized_prompt_command(
            sessions, specs, async_launch,
            "wait; /usr/bin/printf __GSH_BENCH_SYNC__\r", ": &\r",
            "asynchronous builtin launch") == -1 ||
        benchmark_synchronized_prompt_command(
            sessions, specs, async_external_launch,
            "wait; /usr/bin/printf __GSH_BENCH_SYNC__\r",
            "/bin/sleep 0.05 &\r",
            "asynchronous held external launch") == -1 ||
        benchmark_synchronized_prompt_command(
            sessions, specs, wait_completed,
            "wait; : & /bin/sleep 0.01; "
            "/usr/bin/printf __GSH_BENCH_SYNC__\r",
            "wait\r",
            "wait completed job") ==
            -1) {
        failed = 1;
        goto done;
    }

done:
    for (offset = 0; offset < BENCH_SHELLS; offset++) {
        if (started[offset] && stop_session(&sessions[offset]) == -1) {
            failed = 1;
        }
    }
    if (failed) {
        return 1;
    }

    if (uname(&platform) == 0) {
        printf("platform: %s %s %s, cpus=%ld, terminal=80x24, "
               "TERM=xterm-256color\n",
               platform.sysname, platform.release, platform.machine,
               sysconf(_SC_NPROCESSORS_ONLN));
    }
    puts("metric units: milliseconds; deadline column counts samples > 5 ms");
    for (offset = 0; offset < BENCH_SHELLS; offset++) {
        printf("%s: %s\n", specs[offset].name, specs[offset].executable);
        print_raw_samples(specs[offset].name, "startup-to-base-prompt",
                          startup[offset], BENCH_STARTUP_SAMPLES);
        print_raw_samples(specs[offset].name, "idle-key-to-output",
                          key[offset], BENCH_KEY_SAMPLES);
        print_raw_samples(specs[offset].name, "true-enter-to-prompt",
                          execution[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "variable-lookup",
                          lookup[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "command-lookup",
                          command_lookup[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "command-path-cache",
                          command_path_cache[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "variable-assignment",
                          assignment[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "parameter-assign-default",
                          assign_default[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "arithmetic-assignment",
                          arithmetic_assignment[offset],
                          BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name,
                          "pipeline-scoped-assignment",
                          pipeline_assignment[offset],
                          BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "ulimit-soft-nofile",
                          resource_limit[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "umask-report",
                          creation_mask[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "process-times",
                          process_times[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "exec-descriptor-commit",
                          exec_descriptor[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "export-assignment",
                          export_assignment[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "unset-variable",
                          unset_variable[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "readonly-existing",
                          readonly_existing[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "for-explicit",
                          for_explicit[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "set-positionals",
                          set_positionals[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "shift-positionals",
                          shift_positionals[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "set-options",
                          set_options[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "allexport-assignment",
                          allexport_assignment[offset],
                          BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "nounset-defined-lookup",
                          nounset_lookup[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "parameter-pattern-removal",
                          pattern_removal[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name,
                          "parameter-pattern-multistar",
                          pattern_multistar[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "noglob-expansion",
                          noglob_expansion[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "noclobber-redirection",
                          noclobber_redirection[offset],
                          BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "async-builtin-launch",
                          async_launch[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "async-external-held-launch",
                          async_external_launch[offset],
                          BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "wait-completed",
                          wait_completed[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "alias-definition-update",
                          alias_definition[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "alias-lookup-expansion",
                          alias_expansion[offset], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[offset].name, "unalias-definition",
                          unalias_command[offset], BENCH_EXEC_SAMPLES);
        print_metric("startup-to-base-prompt", startup[offset],
                     BENCH_STARTUP_SAMPLES, 5000000ULL);
        print_metric("idle-key-to-output", key[offset], BENCH_KEY_SAMPLES,
                     5000000ULL);
        print_metric("true-enter-to-prompt", execution[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("variable-lookup", lookup[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("command-lookup", command_lookup[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("command-path-cache", command_path_cache[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("variable-assignment", assignment[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("parameter-assign-default", assign_default[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("arithmetic-assignment", arithmetic_assignment[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("pipeline-scoped-assignment",
                     pipeline_assignment[offset], BENCH_EXEC_SAMPLES,
                     5000000ULL);
        print_metric("ulimit-soft-nofile", resource_limit[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("umask-report", creation_mask[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("process-times", process_times[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("exec-descriptor-commit", exec_descriptor[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("export-assignment", export_assignment[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("unset-variable", unset_variable[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("readonly-existing", readonly_existing[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("for-explicit", for_explicit[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("set-positionals", set_positionals[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("shift-positionals", shift_positionals[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("set-options", set_options[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("allexport-assignment", allexport_assignment[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("nounset-defined-lookup", nounset_lookup[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("parameter-pattern-removal", pattern_removal[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("parameter-pattern-multistar",
                     pattern_multistar[offset], BENCH_EXEC_SAMPLES,
                     5000000ULL);
        print_metric("noglob-expansion", noglob_expansion[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("noclobber-redirection", noclobber_redirection[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("async-builtin-launch", async_launch[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("async-external-held-launch",
                     async_external_launch[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("wait-completed", wait_completed[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("alias-definition-update", alias_definition[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("alias-lookup-expansion", alias_expansion[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("unalias-definition", unalias_command[offset],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
    }
    return memory_benchmark(specs, directory, 0,
                            BENCH_MEMORY_WORKLOADS) == 0
               ? 0
               : 1;
}

static int alias_benchmark(const char *gsh, const char *bash,
                           const char *zsh)
{
    shell_spec specs[BENCH_SHELLS] = {
        {"gsh", gsh, "$gsh> ", SHELL_GSH},
        {"bash", bash, "$gsh> ", SHELL_BASH},
        {"zsh", zsh, "$gsh> ", SHELL_ZSH},
    };
    uint64_t definition[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t expansion[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    uint64_t removal[BENCH_SHELLS][BENCH_EXEC_SAMPLES];
    pty_session sessions[BENCH_SHELLS];
    bool started[BENCH_SHELLS] = {false, false, false};
    char directory[4096];
    struct utsname platform;
    size_t shell;
    int failed = 0;

    if (getcwd(directory, sizeof(directory)) == NULL) {
        return 1;
    }
    for (shell = 0; shell < BENCH_SHELLS; shell++) {
        if (access(specs[shell].executable, X_OK) == -1 ||
            start_benchmark_session(&sessions[shell], &specs[shell],
                                    directory, true) == -1) {
            failed = 1;
            goto done;
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
done:
    for (shell = 0; shell < BENCH_SHELLS; shell++) {
        if (started[shell] && stop_session(&sessions[shell]) == -1) {
            failed = 1;
        }
    }
    if (failed) {
        return 1;
    }
    if (uname(&platform) == 0) {
        printf("platform: %s %s %s, cpus=%ld, terminal=80x24, "
               "TERM=xterm-256color\n",
               platform.sysname, platform.release, platform.machine,
               sysconf(_SC_NPROCESSORS_ONLN));
    }
    puts("metric units: milliseconds; deadline column counts samples > 5 ms");
    for (shell = 0; shell < BENCH_SHELLS; shell++) {
        printf("%s: %s\n", specs[shell].name, specs[shell].executable);
        print_raw_samples(specs[shell].name, "alias-definition-update",
                          definition[shell], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[shell].name, "alias-lookup-expansion",
                          expansion[shell], BENCH_EXEC_SAMPLES);
        print_raw_samples(specs[shell].name, "unalias-definition",
                          removal[shell], BENCH_EXEC_SAMPLES);
        print_metric("alias-definition-update", definition[shell],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("alias-lookup-expansion", expansion[shell],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
        print_metric("unalias-definition", removal[shell],
                     BENCH_EXEC_SAMPLES, 5000000ULL);
    }
    return memory_benchmark(specs, directory,
                            BENCH_MEMORY_WORKLOADS - 3U, 3U) == 0
               ? 0
               : 1;
}

static uint64_t pty_fuzz_next(uint64_t *state)
{
    uint64_t value = *state;

    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    *state = value;
    return value * 0x2545f4914f6cdd1dULL;
}

static int pty_byte_fuzz(const char *executable, unsigned long cases)
{
    static const char *const semantic_commands[] = {
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
        ("for GSH_FUZZ_OUTER in a b; do "
         "for GSH_FUZZ_INNER in 1 2; do :; done; done\r"),
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
        "GSH_FUZZ_ARITH=1; : \"$((0 && "
        "(GSH_FUZZ_ARITH = 2)))\"; "
        "/bin/test \"$GSH_FUZZ_ARITH\" = 1\r",
        "GSH_FUZZ_COLON=value :\r",
        "if /usr/bin/true; then cd .; fi\r",
        "set -f; /usr/bin/printf '%s' /dev/n[uo]ll; set +f\r",
        "set -C; /usr/bin/printf x >/dev/null; set +C\r",
        "/usr/bin/true &\r",
        "wait \"$!\"\r",
    };
    char fixture[] = "/tmp/gsh-pty-fuzz-XXXXXX";
    unsigned char bytes[512];
    pty_session session;
    uint64_t random_state = 0x6a09e667f3bcc909ULL;
    int initial_fds;
    int initial_children;
    unsigned long index;
    int failed = 0;

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
    if (consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
        perror("pty fuzz: initial prompt");
        failed = 1;
        goto done;
    }
    initial_fds = process_fd_count(session.pid);
    initial_children = process_child_count(session.pid);
    if (initial_fds < 0 || initial_children < 0) {
        fprintf(stderr, "pty fuzz: process metrics unavailable\n");
        failed = 1;
        goto done;
    }
    for (index = 0; index < cases; index++) {
        if ((index & 7U) == 0) {
            const char *command = semantic_commands[
                pty_fuzz_next(&random_state) %
                (sizeof(semantic_commands) /
                 sizeof(semantic_commands[0]))];

            if (send_text(&session, command) == -1 ||
                consume_through(&session, "$gsh> ",
                                TEST_TIMEOUT_MS) == -1) {
                fprintf(stderr,
                        "pty fuzz: semantic seed failed at case %lu\n",
                        index);
                failed = 1;
                goto done;
            }
        }
        size_t length = (size_t)(pty_fuzz_next(&random_state) %
                                 sizeof(bytes));
        size_t offset;

        for (offset = 0; offset < length; offset++) {
            uint64_t choice = pty_fuzz_next(&random_state);

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
                bytes[offset] = (unsigned char)(0x80U +
                    (choice >> 8U) % 0x80U);
                break;
            default:
                bytes[offset] = (unsigned char)(0x20U +
                    (choice >> 8U) % 0x5fU);
                if (bytes[offset] == '^') {
                    bytes[offset] = '_';
                }
                break;
            }
        }
        if (send_bytes(&session, (const char *)bytes, length) == -1 ||
            ((pty_fuzz_next(&random_state) & 7U) == 0 &&
             kill(session.pid, SIGWINCH) == -1) ||
            send_bytes(&session, "\003", 1) == -1 ||
            consume_through(&session, "^C", TEST_TIMEOUT_MS) == -1 ||
            consume_through(&session, "$gsh> ", TEST_TIMEOUT_MS) == -1) {
            fprintf(stderr, "pty fuzz: failed at case %lu\n", index);
            dump_capture(&session);
            failed = 1;
            goto done;
        }
    }
    {
        int final_fds = process_fd_count(session.pid);
        int final_children = process_child_count(session.pid);

        if (final_fds < 0 || final_fds > initial_fds + 1 ||
            final_children < 0 || final_children > initial_children) {
            fprintf(stderr, "pty fuzz: resource growth exceeded bounds\n");
            failed = 1;
            goto done;
        }
        printf("pty byte fuzz: cases=%lu seed=0x6a09e667f3bcc909 "
               "fds_initial=%d fds_final=%d children_initial=%d "
               "children_final=%d passed\n",
               cases, initial_fds, final_fds, initial_children,
               final_children);
    }

done:
    if (stop_session(&session) == -1) {
        failed = 1;
    }
    (void)rmdir(fixture);
    return failed;
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

    if (argc == 5 && strcmp(argv[1], "--benchmark") == 0) {
        return latency_benchmark(argv[2], argv[3], argv[4]);
    }
    if (argc == 5 && strcmp(argv[1], "--benchmark-alias") == 0) {
        return alias_benchmark(argv[2], argv[3], argv[4]);
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
            fprintf(stderr, "pty-harness: soak seconds must be 1..86400\n");
            return 2;
        }
        return soak_flow(argv[2], seconds);
    }
    if (argc == 4 && strcmp(argv[1], "--fuzz-pty") == 0) {
        char *end;
        unsigned long cases = strtoul(argv[3], &end, 10);

        if (*end != '\0' || cases == 0 || cases > 1000000) {
            fprintf(stderr, "pty-harness: fuzz cases must be 1..1000000\n");
            return 2;
        }
        return pty_byte_fuzz(argv[2], cases);
    }
    if (argc != 2) {
        fprintf(stderr,
                "usage: pty-harness /absolute/path/to/gsh\n"
                "       pty-harness --fault /absolute/path/to/gsh-fault\n"
                "       pty-harness --resource /absolute/path/to/gsh\n"
                "       pty-harness --soak /absolute/path/to/gsh seconds\n"
                "       pty-harness --fuzz-pty /absolute/path/to/gsh cases\n"
                "       pty-harness --benchmark gsh bash zsh\n"
                "       pty-harness --benchmark-alias gsh bash zsh\n");
        return 2;
    }
    if (argv[1][0] == '/') {
        if (strlen(argv[1]) + 1U > sizeof(executable)) {
            fprintf(stderr, "pty smoke: executable path is too long\n");
            return 2;
        }
        memcpy(executable, argv[1], strlen(argv[1]) + 1U);
    } else if (getcwd(executable, sizeof(executable)) == NULL ||
               strlen(executable) + strlen(argv[1]) + 2U >
                   sizeof(executable)) {
        perror("pty smoke: executable path");
        return 2;
    } else {
        size_t length = strlen(executable);

        executable[length++] = '/';
        memcpy(executable + length, argv[1], strlen(argv[1]) + 1U);
    }

    if (ordinary_flow(executable) != 0 ||
        history_flow(executable) != 0 ||
        managed_async_repl_flow(executable) != 0 ||
        variable_builtin_flow(executable) != 0 ||
        alias_builtin_flow(executable) != 0 ||
        command_hash_flow(executable) != 0 ||
        times_builtin_flow(executable) != 0 ||
        exec_builtin_flow(executable) != 0 ||
        function_builtin_flow(executable) != 0 ||
        deferred_pattern_flow(executable) != 0 ||
        asynchronous_list_flow(executable) != 0 ||
        async_redirection_flow(executable) != 0 ||
        stalled_worker_flow(executable) != 0) {
        return 1;
    }
    puts("pty smoke: exec paths, encrypted history, editor recall/search, "
         "managed async REPL, job control, async prompt/redirection, "
         "cancellation, and worker deadline passed");
    return 0;
}
