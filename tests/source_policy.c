#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh policy checks require the POSIX.1-2024 feature-test baseline"
#endif

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const FORBIDDEN_SUFFIXES[] = {
    ".cc", ".cpp", ".cxx", ".go", ".js", ".py", ".rs", ".sh", ".ts",
};

static bool has_suffix(const char *path, const char *suffix)
{
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);

    return suffix_length <= path_length &&
           strcmp(path + path_length - suffix_length, suffix) == 0;
}

static bool buffer_contains(const unsigned char *data, size_t length,
                            const char *needle)
{
    size_t needle_length = strlen(needle);
    size_t index;

    if (needle_length == 0 || needle_length > length) {
        return false;
    }
    for (index = 0; index + needle_length <= length; index++) {
        if (memcmp(data + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static int check_c_baseline(const char *path)
{
    unsigned char data[2048];
    ssize_t length;
    int fd = open(path, O_RDONLY);

    if (fd == -1) {
        return -1;
    }
    length = read(fd, data, sizeof(data));
    close(fd);
    if (length < 0) {
        return -1;
    }
    if (!buffer_contains(data, (size_t)length, "_POSIX_C_SOURCE") ||
        !buffer_contains(data, (size_t)length, "202405L")) {
        fprintf(stderr, "source policy: missing POSIX.1-2024 baseline: %s\n",
                path);
        return 1;
    }
    return 0;
}

static int scan_directory(const char *path)
{
    struct dirent *entry;
    DIR *directory = opendir(path);
    int failed = 0;

    if (directory == NULL) {
        return errno == ENOENT ? 0 : -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        struct stat information;
        size_t suffix;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, "corpus") == 0) {
            continue;
        }
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >=
            (int)sizeof(child)) {
            failed = 1;
            continue;
        }
        if (lstat(child, &information) == -1) {
            failed = 1;
            continue;
        }
        if (S_ISDIR(information.st_mode)) {
            if (scan_directory(child) != 0) {
                failed = 1;
            }
            continue;
        }
        if (!S_ISREG(information.st_mode)) {
            fprintf(stderr, "source policy: non-regular source entry: %s\n",
                    child);
            failed = 1;
            continue;
        }
        for (suffix = 0;
             suffix < sizeof(FORBIDDEN_SUFFIXES) /
                          sizeof(FORBIDDEN_SUFFIXES[0]);
             suffix++) {
            if (has_suffix(child, FORBIDDEN_SUFFIXES[suffix])) {
                fprintf(stderr,
                        "source policy: forbidden first-party language: %s\n",
                        child);
                failed = 1;
            }
        }
        if (has_suffix(child, ".c") && check_c_baseline(child) != 0) {
            failed = 1;
        }
    }
    closedir(directory);
    return failed;
}

static int binary_excludes_fault_injection(const char *path)
{
    enum {
        BINARY_CHUNK_SIZE = 8192,
        BINARY_OVERLAP_SIZE = 128,
    };
    static const char *const forbidden[] = {
        "GSH_FAULT", "job-fork",
        "terminal-handoff", "pipeline-pipe", "pipeline-fork",
        "descriptor-dup", "redirect-open", "evaluator-gate",
        "evaluator-fork", "subshell-fork", "time-source-failure",
        "heredoc-pipe",
        "heredoc-fork", "heredoc-write", "substitution-pipe",
        "substitution-fork", "substitution-read",
        "source-workspace-exhaustion", "state-commit-pipe",
        "state-commit-write", "state-commit-read",
        "state-commit-malformed", "state-control-commit-malformed",
        "positional-allocation",
        "positional-commit-allocation",
        "positional-commit-malformed",
        "alias-allocation", "alias-transaction-allocation",
        "alias-commit-malformed",
    };
    unsigned char data[BINARY_CHUNK_SIZE + BINARY_OVERLAP_SIZE];
    size_t overlap_length = 0;
    int fd = open(path, O_RDONLY);

    if (fd == -1) {
        return -1;
    }
    for (;;) {
        ssize_t count = read(fd, data + overlap_length, BINARY_CHUNK_SIZE);
        size_t total;
        size_t index;

        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(fd);
            return count == 0 ? 0 : -1;
        }
        total = overlap_length + (size_t)count;
        for (index = 0; index < sizeof(forbidden) / sizeof(forbidden[0]);
             index++) {
            if (buffer_contains(data, total, forbidden[index])) {
                fprintf(stderr,
                        "source policy: production binary contains fault "
                        "hook %s\n",
                        forbidden[index]);
                close(fd);
                return 1;
            }
        }
        overlap_length = total < BINARY_OVERLAP_SIZE
                             ? total
                             : BINARY_OVERLAP_SIZE;
        memmove(data, data + total - overlap_length, overlap_length);
    }
}

int main(int argc, char **argv)
{
    char path[PATH_MAX];
    static const char *const directories[] = {"src", "tests", "tools",
                                               "bench"};
    size_t index;
    int failed = 0;

    if (argc != 3) {
        fprintf(stderr, "usage: source-policy repository-root gsh-binary\n");
        return 2;
    }
    for (index = 0; index < sizeof(directories) / sizeof(directories[0]);
         index++) {
        if (snprintf(path, sizeof(path), "%s/%s", argv[1],
                     directories[index]) >= (int)sizeof(path) ||
            scan_directory(path) != 0) {
            failed = 1;
        }
    }
    if (binary_excludes_fault_injection(argv[2]) != 0) {
        failed = 1;
    }
    if (failed) {
        return 1;
    }
    puts("source policy: C-only executable sources, POSIX baseline, and "
         "production fault isolation passed");
    return 0;
}
