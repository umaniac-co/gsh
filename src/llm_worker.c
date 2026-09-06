#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "llm_journal.h"
#include "llm_json.h"
#include "llm_repl.h"
#include "llm_command_policy.h"
#include "shell_config.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h> /* CANON-INCLUDE: macos */
#include <stdlib.h> /* CANON-INCLUDE: linux */
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h> /* CANON-INCLUDE: linux */
#include <termios.h>
#include <unistd.h>

enum {
    LLM_PROMPT_CAP = 4096,
    LLM_CONTEXT_CAP = 98304,
    LLM_BODY_CAP = 1048576,
    LLM_EVENT_CAP = 262144,
    LLM_ITEM_CAP = 32,
    LLM_ITEM_TEXT_CAP = 131072,
    LLM_ANSWER_CAP = 65536,
    LLM_TOOL_OUTPUT_CAP = 65536,
    LLM_TOOL_NAME_CAP = 64,
    LLM_CALL_ID_CAP = 256,
    LLM_TOOL_ARGUMENT_CAP = 65536,
    LLM_TOOL_ROUND_CAP = 8,
    LLM_HTTP_READ_CAP = 8192,
};

typedef struct {
    size_t length;
    char json[LLM_ITEM_TEXT_CAP];
} llm_item;

typedef struct {
    bool pending;
    char name[LLM_TOOL_NAME_CAP];
    char call_id[LLM_CALL_ID_CAP];
    char arguments[LLM_TOOL_ARGUMENT_CAP];
} llm_tool_call;

typedef struct {
    char prompt[LLM_PROMPT_CAP];
    char context[LLM_CONTEXT_CAP];
    char body[LLM_BODY_CAP];
    char event[LLM_EVENT_CAP];
    char answer[LLM_ANSWER_CAP];
    char tool_output[LLM_TOOL_OUTPUT_CAP];
    llm_item items[LLM_ITEM_CAP];
    size_t item_count;
    size_t answer_length;
    bool tools_enabled;
    bool record;
    int repl_fd;
    gsh_llm_repl_result repl_result;
    gsh_llm_command_policy command_policy;
    llm_tool_call tool;
} llm_workspace;

static int write_all(int descriptor, const char *text, size_t length)
{
    size_t offset = 0U;

    if (descriptor < 0 || text == NULL) return -1;
    while (offset < length) {
        ssize_t count = write(descriptor, text + offset, length - offset);

        if (count > 0) offset += (size_t)count;
        else if (count == -1 && errno == EINTR) continue;
        else return -1;
    }
    return 0;
}

static ssize_t read_prompt(int descriptor, char *prompt, size_t capacity)
{
    size_t used = 0U;

    if (descriptor < 0 || prompt == NULL || capacity < 2U) return -1;
    while (used + 1U < capacity) {
        ssize_t count = read(descriptor, prompt + used, capacity - used - 1U);

        if (count > 0) used += (size_t)count;
        else if (count == 0) { prompt[used] = '\0'; return (ssize_t)used; }
        else if (errno != EINTR) return -1;
    }
    errno = E2BIG;
    return -1;
}

static bool environment_name_valid(const char *name)
{
    size_t index;

    if (name == NULL || !((name[0] >= 'A' && name[0] <= 'Z') ||
                          (name[0] >= 'a' && name[0] <= 'z') ||
                          name[0] == '_')) return false;
    for (index = 1U; index < GSH_LLM_CREDENTIAL_CAP; index++) {
        unsigned char byte = (unsigned char)name[index];

        if (byte == '\0') return true;
        if (!((byte >= 'A' && byte <= 'Z') ||
              (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '_')) return false;
    }
    return false;
}

static bool loopback_host_suffix(const char *suffix)
{
    if (suffix == NULL) return false;
    return suffix[0] == '\0' || suffix[0] == ':' || suffix[0] == '/';
}

static bool endpoint_is_loopback(const char *url)
{
    static const char http[] = "http://";
    const char *host;

    if (url == NULL || strncmp(url, http, sizeof(http) - 1U) != 0)
        return false;
    host = url + sizeof(http) - 1U;
    if (strncmp(host, "127.0.0.1", 9U) == 0)
        return loopback_host_suffix(host + 9U);
    if (strncmp(host, "localhost", 9U) == 0)
        return loopback_host_suffix(host + 9U);
    return strncmp(host, "[::1]", 5U) == 0 &&
           loopback_host_suffix(host + 5U);
}

static bool endpoint_is_safe(const char *url, bool authenticated)
{
    if (url == NULL) return false;
    if (strncmp(url, "https://", 8U) == 0) return true;
    return endpoint_is_loopback(url) && (!authenticated ||
                                         endpoint_is_loopback(url));
}

static const char *provider_key(const gsh_llm_provider_config *provider)
{
    const char *name;

    if (provider == NULL || strcmp(provider->credential, "none") == 0)
        return NULL;
    if (strncmp(provider->credential, "env:", 4U) != 0) return NULL;
    name = provider->credential + 4U;
    return environment_name_valid(name) ? getenv(name) : NULL;
}

static bool provider_valid(const gsh_llm_provider_config *provider,
                           const char *key)
{
    bool expects_key;

    if (provider == NULL || provider->base_url[0] == '\0' ||
        provider->model[0] == '\0') return false;
    expects_key = strcmp(provider->credential, "none") != 0;
    if (expects_key && (key == NULL || key[0] == '\0')) return false;
    return endpoint_is_safe(provider->base_url, expects_key);
}

/* ── Stateless Responses Keep Providers Interchangeable ─────────
 * Server-side conversation identifiers differ across inference engines and
 * make provider failover ambiguous. The worker therefore replays a bounded
 * local context plus completed response items with store disabled each turn.
 * Standard Responses function calls cross one explicit tool harness, so DS4,
 * llama.cpp, and remote endpoints observe the same continuation contract.
 * ─────────────────────────────────────────────────────────────── */
static bool build_user_input(gsh_json_writer *writer,
                             const gsh_shell_config *config,
                             llm_workspace *workspace)
{
    static const char recent[] = "Recent gsh conversation:\n";
    static const char current[] = "\nCurrent request:\n";
    size_t history_length;
    size_t prompt_length;
    size_t used;

    if (writer == NULL || config == NULL || workspace == NULL) return false;
    (void)gsh_llm_journal_context(getenv("HOME"),
                                  config->llm_recent_exchanges,
                                  workspace->context,
                                  sizeof(workspace->context));
    history_length = strlen(workspace->context);
    prompt_length = strlen(workspace->prompt);
    if (sizeof(recent) - 1U + history_length + sizeof(current) - 1U +
            prompt_length >= sizeof(workspace->context)) return false;
    (void)memmove(workspace->context + sizeof(recent) - 1U,
                  workspace->context, history_length + 1U);
    (void)memcpy(workspace->context, recent, sizeof(recent) - 1U);
    used = sizeof(recent) - 1U + history_length;
    (void)memcpy(workspace->context + used, current, sizeof(current) - 1U);
    used += sizeof(current) - 1U;
    (void)memcpy(workspace->context + used, workspace->prompt,
                 prompt_length + 1U);
    used += prompt_length;
    if (!gsh_json_write_literal(writer, "{\"role\":\"user\",\"content\":"))
        return false;
    if (!gsh_json_write_string(writer, workspace->context, used)) return false;
    return gsh_json_write_literal(writer, "}");
}

static bool build_input_items(gsh_json_writer *writer,
                              const gsh_shell_config *config,
                              llm_workspace *workspace)
{
    size_t index;

    if (writer == NULL || config == NULL || workspace == NULL) return false;
    if (!gsh_json_write_literal(writer, "\"input\":[")) return false;
    if (!build_user_input(writer, config, workspace)) return false;
    for (index = 0U; index < workspace->item_count; index++) {
        if (!gsh_json_write_literal(writer, ",") ||
            !gsh_json_write_bytes(writer, workspace->items[index].json,
                                  workspace->items[index].length)) return false;
    }
    return gsh_json_write_literal(writer, "],");
}

static bool build_tools(gsh_json_writer *writer)
{
    static const char tools[] =
        "\"tools\":[{\"type\":\"function\",\"name\":\"history_search\","
        "\"description\":\"Search the retained gsh command, output, prompt, "
        "answer and tool journal by keyword.\",\"parameters\":{"
        "\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},"
        "\"required\":[\"query\"],\"additionalProperties\":false}},{"
        "\"type\":\"function\",\"name\":\"run_cli\",\"description\":"
        "\"Submit a shell script as a new command block in the user's gsh "
        "REPL. Shell state changes persist, including cd and variables. "
        "Returns the block's output, status and actual session directory. "
        "Only removal operations require user confirmation. All other "
        "commands, including writes and unfamiliar tools, run automatically.\","
        "\"parameters\":{\"type\":"
        "\"object\",\"properties\":{\"script\":{\"type\":\"string\"}},"
        "\"required\":[\"script\"],\"additionalProperties\":false}}],"
        "\"tool_choice\":\"auto\",\"parallel_tool_calls\":false,";

    return gsh_json_write_literal(writer, tools);
}

static bool build_request(const gsh_shell_config *config,
                          const gsh_llm_provider_config *provider,
                          llm_workspace *workspace)
{
    static const char instructions[] =
        "You are the native gsh terminal assistant. Be concise. Use "
        "history_search when older shell context is needed. Use run_cli when "
        "terminal work is required; never claim a command ran unless the tool "
        "returned an executed REPL block. Every command must use run_cli; "
        "each call creates a subsequent visible block and waits for it. "
        "Use the returned session directory for subsequent commands. "
        "Run non-removal commands directly without asking for approval. "
        "For removals use explicit removal CLI commands so the shell can "
        "show its confirmation; do not hide removals in custom code. "
        "If the session context changed, stop and explain that the command "
        "was not executed; do not retry automatically. "
        "Do not emit hidden shell commands.";
    gsh_json_writer writer;

    if (config == NULL || provider == NULL || workspace == NULL) return false;
    gsh_json_writer_initialize(&writer, workspace->body,
                               sizeof(workspace->body));
    if (!gsh_json_write_literal(&writer, "{\"model\":")) return false;
    if (!gsh_json_write_string(&writer, provider->model,
                               strlen(provider->model))) return false;
    if (!gsh_json_write_literal(&writer, ",\"instructions\":")) return false;
    if (!gsh_json_write_string(&writer, instructions,
                               sizeof(instructions) - 1U)) return false;
    if (!gsh_json_write_literal(&writer, ",") ||
        !build_input_items(&writer, config, workspace))
        return false;
    if (workspace->tools_enabled) {
        if (!build_tools(&writer)) return false;
    } else if (!gsh_json_write_literal(
                   &writer, "\"tools\":[],\"tool_choice\":\"none\","))
        return false;
    return gsh_json_write_literal(&writer,
                                  "\"store\":false,\"stream\":true}");
}

static int response_url(const gsh_llm_provider_config *provider, char *url,
                        size_t capacity)
{
    size_t length;
    int written;

    if (provider == NULL || url == NULL || capacity == 0U) return -1;
    length = strlen(provider->base_url);
    written = snprintf(url, capacity, "%s%sresponses", provider->base_url,
                       length > 0U && provider->base_url[length - 1U] == '/'
                           ? "" : "/");
    if (written < 0 || (size_t)written >= capacity) return -1;
    return 0;
}

static int models_url(const gsh_llm_provider_config *provider, char *url,
                      size_t capacity)
{
    size_t length;
    int written;

    if (provider == NULL || url == NULL || capacity == 0U) return -1;
    length = strlen(provider->base_url);
    written = snprintf(url, capacity, "%s%smodels", provider->base_url,
                       length > 0U && provider->base_url[length - 1U] == '/'
                           ? "" : "/");
    return written >= 0 && (size_t)written < capacity ? 0 : -1;
}

static int probe_provider_child(const gsh_llm_provider_config *provider,
                                const char *key)
{
    char url[GSH_LLM_BASE_URL_CAP + 32U];
    char authorization[GSH_LLM_CREDENTIAL_CAP + 4096U];
    struct curl_slist *headers = NULL;
    CURL *curl;
    CURLcode status;
    int sink;

    if (provider == NULL || models_url(provider, url, sizeof(url)) == -1 ||
        (sink = open("/dev/null", O_WRONLY | O_CLOEXEC)) == -1 ||
        dup2(sink, STDOUT_FILENO) == -1) return 125;
    (void)close(sink);
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 125;
    curl = curl_easy_init();
    if (curl == NULL) { curl_global_cleanup(); return 125; }
    if (key != NULL) {
        if (snprintf(authorization, sizeof(authorization),
                     "Authorization: Bearer %s", key) >=
            (int)sizeof(authorization)) return 125;
        headers = curl_slist_append(headers, authorization);
    }
    (void)curl_easy_setopt(curl, CURLOPT_URL, url);
    (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);
    (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    (void)curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    status = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return status == CURLE_OK ? 0 : 1;
}

static bool provider_available(const gsh_llm_provider_config *provider,
                               const char *key)
{
    pid_t pid;
    int status = 0;

    if (provider == NULL) return false;
    pid = fork();
    if (pid == 0) _exit(probe_provider_child(provider, key));
    if (pid < 0) return false;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ── Managed Runtimes Have One Bounded Lifecycle ─────────────────
 * A configured local engine may disappear between shell sessions. The first
 * worker that observes it missing holds an owner-only advisory lock, launches
 * the exact command stored in gshrc, and waits on the public models endpoint.
 * Request workers share an activity lease; the detached owner can therefore
 * stop its process group only after the configured idle interval and never
 * while another shell is generating or waiting on a tool confirmation.
 * ─────────────────────────────────────────────────────────────── */
static int runtime_directory(const char *home, char *directory,
                             size_t capacity)
{
    struct stat status;

    if (home == NULL || directory == NULL || capacity == 0U ||
        snprintf(directory, capacity, "%s/.genshell", home) >=
            (int)capacity) return -1;
    if (mkdir(directory, 0700) == -1 && errno != EEXIST) return -1;
    if (stat(directory, &status) == -1 || !S_ISDIR(status.st_mode) ||
        status.st_uid != geteuid()) return -1;
    return 0;
}

static int runtime_activity_path(const gsh_llm_provider_config *provider,
                                 const char *home, char *path,
                                 size_t capacity)
{
    char directory[4096];

    if (provider == NULL || path == NULL ||
        runtime_directory(home, directory, sizeof(directory)) == -1 ||
        snprintf(path, capacity, "%s/runtime.%s.activity", directory,
                 provider->name) >= (int)capacity) return -1;
    return 0;
}

static int runtime_activity_open(const gsh_llm_provider_config *provider,
                                 const char *home)
{
    char path[4096];
    struct stat status;
    int descriptor;

    if (runtime_activity_path(provider, home, path, sizeof(path)) == -1)
        return -1;
    descriptor = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor == -1 || fstat(descriptor, &status) == -1 ||
        !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 077U) != 0U) {
        if (descriptor >= 0) (void)close(descriptor);
        return -1;
    }
    return descriptor;
}

static int runtime_activity_begin(const gsh_llm_provider_config *provider,
                                  const char *home)
{
    struct flock lease;
    int descriptor = runtime_activity_open(provider, home);

    (void)memset(&lease, 0, sizeof(lease));
    lease.l_type = F_RDLCK;
    lease.l_whence = SEEK_SET;
    if (descriptor == -1 || fcntl(descriptor, F_SETLKW, &lease) == -1 ||
        futimens(descriptor, NULL) == -1) {
        if (descriptor >= 0) (void)close(descriptor);
        return -1;
    }
    return descriptor;
}

static void runtime_activity_finish(int descriptor)
{
    if (descriptor < 0) return;
    (void)futimens(descriptor, NULL);
    (void)close(descriptor);
}

static bool runtime_idle_expired(int descriptor, unsigned int timeout)
{
    struct stat status;
    struct timespec now;
    struct timespec modified;
    time_t elapsed;

    if (descriptor < 0 || timeout == 0U || fstat(descriptor, &status) == -1 ||
        clock_gettime(CLOCK_REALTIME, &now) == -1) return false;
#if defined(__APPLE__)
    modified = status.st_mtimespec;
#else
    modified = status.st_mtim;
#endif
    if (now.tv_sec < modified.tv_sec) return false;
    elapsed = now.tv_sec - modified.tv_sec;
    if (elapsed < (time_t)timeout) return false;
    return elapsed > (time_t)timeout || now.tv_nsec >= modified.tv_nsec;
}

static bool runtime_claim_if_idle(int descriptor, unsigned int timeout)
{
    struct flock exclusive = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    struct flock unlock = {.l_type = F_UNLCK, .l_whence = SEEK_SET};

    if (!runtime_idle_expired(descriptor, timeout) ||
        fcntl(descriptor, F_SETLK, &exclusive) == -1) return false;
    if (runtime_idle_expired(descriptor, timeout)) return true;
    (void)fcntl(descriptor, F_SETLK, &unlock);
    return false;
}

static int runtime_lock(const char *home)
{
    char directory[4096];
    char path[4096];
    struct stat status;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    int descriptor;

    if (runtime_directory(home, directory, sizeof(directory)) == -1 ||
        snprintf(path, sizeof(path), "%s/runtime.lock", directory) >=
            (int)sizeof(path)) return -1;
    if (stat(directory, &status) == -1 || status.st_uid != geteuid()) return -1;
    descriptor = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor == -1 || fcntl(descriptor, F_SETLKW, &lock) == -1) {
        if (descriptor >= 0) (void)close(descriptor);
        return -1;
    }
    return descriptor;
}

static void stop_runtime_process(pid_t runtime)
{
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000000L};
    unsigned int attempt;
    int status = 0;

    if (runtime <= 0) return;
    if (kill(-runtime, SIGTERM) == -1 && errno != ESRCH) return;
    for (attempt = 0U; attempt < 300U; attempt++) {
        pid_t result = waitpid(runtime, &status, WNOHANG);
        if (result == runtime || (result == -1 && errno == ECHILD)) return;
        if (result == -1 && errno != EINTR) break;
        (void)nanosleep(&pause, NULL);
    }
    (void)kill(-runtime, SIGKILL);
    while (waitpid(runtime, &status, 0) == -1 && errno == EINTR) {
    }
}

_Noreturn static void supervise_runtime(
    const gsh_llm_provider_config *provider, const char *home, int input,
    int output, int inherited_lease, int inherited_lock)
{
    struct timespec pause = {.tv_sec = 1, .tv_nsec = 0};
    unsigned int tick;
    int activity;
    int status = 0;
    pid_t runtime;

    if (provider == NULL || home == NULL) _exit(125);
    if (inherited_lease >= 0) (void)close(inherited_lease);
    if (inherited_lock >= 0) (void)close(inherited_lock);
    if (setsid() == -1 || dup2(input, STDIN_FILENO) == -1 ||
        dup2(output, STDOUT_FILENO) == -1 ||
        dup2(output, STDERR_FILENO) == -1) _exit(125);
    (void)close(input);
    (void)close(output);
    activity = runtime_activity_open(provider, home);
    if (activity == -1) _exit(125);
    runtime = fork();
    if (runtime == 0) {
        (void)close(activity);
        if (setpgid(0, 0) == -1) _exit(125);
        execl("/bin/sh", "sh", "-c", provider->runtime_command,
              (char *)NULL);
        _exit(127);
    }
    if (runtime < 0) { (void)close(activity); _exit(125); }
    (void)setpgid(runtime, runtime);
    for (tick = 0U; tick < 315360000U; tick++) {
        pid_t result = waitpid(runtime, &status, WNOHANG);

        if (result == runtime || (result == -1 && errno == ECHILD)) {
            (void)close(activity);
            _exit(0);
        }
        if (result == -1 && errno != EINTR) {
            (void)close(activity);
            _exit(125);
        }
        if (runtime_claim_if_idle(activity,
                                  provider->idle_timeout_seconds)) {
            (void)write_all(STDERR_FILENO,
                            "[gsh runtime] idle timeout; stopping server\n",
                            44U);
            stop_runtime_process(runtime);
            (void)close(activity);
            _exit(0);
        }
        (void)nanosleep(&pause, NULL);
    }
    stop_runtime_process(runtime);
    (void)close(activity);
    _exit(0);
}

static int launch_runtime(const gsh_llm_provider_config *provider,
                          const char *home, int activity_lease,
                          int startup_lock)
{
    char log_path[4096];
    int input;
    int output;
    pid_t pid;

    if (provider == NULL || home == NULL || provider->runtime_command[0] == '\0' ||
        snprintf(log_path, sizeof(log_path), "%s/.genshell/runtime.log", home) >=
            (int)sizeof(log_path)) return -1;
    input = open("/dev/null", O_RDONLY | O_CLOEXEC);
    output = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (input == -1 || output == -1) {
        if (input >= 0) (void)close(input);
        if (output >= 0) (void)close(output);
        return -1;
    }
    pid = fork();
    if (pid == 0) {
        if (provider->idle_timeout_seconds != 0U)
            supervise_runtime(provider, home, input, output, activity_lease,
                              startup_lock);
        if (activity_lease >= 0) (void)close(activity_lease);
        if (startup_lock >= 0) (void)close(startup_lock);
        if (setsid() == -1 || dup2(input, STDIN_FILENO) == -1 ||
            dup2(output, STDOUT_FILENO) == -1 ||
            dup2(output, STDERR_FILENO) == -1) _exit(125);
        (void)close(input);
        (void)close(output);
        execl("/bin/sh", "sh", "-c", provider->runtime_command,
              (char *)NULL);
        _exit(127);
    }
    (void)close(input);
    (void)close(output);
    return pid < 0 ? -1 : 0;
}

static bool ensure_managed_runtime(
    const gsh_llm_provider_config *provider, const gsh_shell_config *config,
    const char *key, const char *home, int activity_lease, int repl_fd)
{
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 500000000L};
    unsigned int attempts;
    unsigned int limit;
    int lock;
    bool available;

    if (provider == NULL || config == NULL || !provider->managed)
        return true;
    if (provider_available(provider, key)) return true;
    lock = runtime_lock(home);
    if (lock == -1) return false;
    if (provider_available(provider, key)) {
        (void)close(lock);
        return true;
    }
    if (repl_fd < 0)
        (void)write_all(STDOUT_FILENO, "[gsh ai] Starting...\n", 21U);
    if (launch_runtime(provider, home, activity_lease, lock) == -1) {
        (void)close(lock);
        return false;
    }
    limit = config->llm_request_timeout_seconds * 2U;
    if (limit > 7200U) limit = 7200U;
    available = false;
    for (attempts = 0U; attempts < 7200U && attempts < limit; attempts++) {
        if (provider_available(provider, key)) { available = true; break; }
        (void)nanosleep(&pause, NULL);
    }
    (void)close(lock);
    return available;
}

static int curl_request_child(int output,
                              const gsh_llm_provider_config *provider,
                              const gsh_shell_config *config, const char *key,
                              const llm_workspace *workspace)
{
    char url[GSH_LLM_BASE_URL_CAP + 32U];
    char authorization[GSH_LLM_CREDENTIAL_CAP + 4096U];
    struct curl_slist *headers = NULL;
    CURL *curl;
    CURLcode status;

    if (provider == NULL || config == NULL || workspace == NULL ||
        dup2(output, STDOUT_FILENO) == -1 || close(output) == -1 ||
        setvbuf(stdout, NULL, _IONBF, 0) != 0 ||
        response_url(provider, url, sizeof(url)) == -1) return 125;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return 125;
    curl = curl_easy_init();
    if (curl == NULL) { curl_global_cleanup(); return 125; }
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    if (key != NULL) {
        if (snprintf(authorization, sizeof(authorization),
                     "Authorization: Bearer %s", key) >=
            (int)sizeof(authorization)) return 125;
        headers = curl_slist_append(headers, authorization);
    }
    (void)curl_easy_setopt(curl, CURLOPT_URL, url);
    (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, workspace->body);
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                           (curl_off_t)strlen(workspace->body));
    (void)curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                           (long)config->llm_request_timeout_seconds);
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    status = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return status == CURLE_OK ? 0 : (int)status;
}

static bool append_answer(llm_workspace *workspace, const char *text)
{
    size_t length;
    size_t retained;

    if (workspace == NULL || text == NULL) return false;
    length = strlen(text);
    retained = length < sizeof(workspace->answer) - workspace->answer_length - 1U
                   ? length
                   : sizeof(workspace->answer) - workspace->answer_length - 1U;
    (void)memcpy(workspace->answer + workspace->answer_length, text, retained);
    workspace->answer_length += retained;
    workspace->answer[workspace->answer_length] = '\0';
    return retained == length;
}

static bool save_output_item(llm_workspace *workspace, const char *json,
                             size_t length)
{
    llm_item *item;

    if (workspace == NULL || json == NULL || length == 0U ||
        length >= LLM_ITEM_TEXT_CAP || workspace->item_count >= LLM_ITEM_CAP)
        return false;
    item = &workspace->items[workspace->item_count++];
    (void)memcpy(item->json, json, length);
    item->json[length] = '\0';
    item->length = length;
    return true;
}

static void accept_function_call(llm_workspace *workspace, const char *item,
                                 size_t length)
{
    char type[32];

    if (workspace == NULL || item == NULL || workspace->tool.pending ||
        !gsh_json_get_string(item, length, "type", type, sizeof(type)) ||
        strcmp(type, "function_call") != 0) return;
    if (!gsh_json_get_string(item, length, "name", workspace->tool.name,
                             sizeof(workspace->tool.name)) ||
        !gsh_json_get_string(item, length, "call_id", workspace->tool.call_id,
                             sizeof(workspace->tool.call_id)) ||
        !gsh_json_get_string(item, length, "arguments",
                             workspace->tool.arguments,
                             sizeof(workspace->tool.arguments))) return;
    workspace->tool.pending = true;
}

static int process_sse_event(llm_workspace *workspace, const char *json,
                             size_t length)
{
    char type[96];

    if (workspace == NULL || json == NULL || length == 0U ||
        !gsh_json_get_string(json, length, "type", type, sizeof(type)))
        return 0;
    if (strcmp(type, "response.output_text.delta") == 0) {
        if (!gsh_json_get_string(json, length, "delta",
                                 workspace->tool_output,
                                 sizeof(workspace->tool_output))) return -1;
        if (write_all(STDOUT_FILENO, workspace->tool_output,
                      strlen(workspace->tool_output)) == -1) return -1;
        (void)append_answer(workspace, workspace->tool_output);
    } else if (strcmp(type, "response.output_item.done") == 0) {
        const char *item;
        size_t item_length;

        if (!gsh_json_get_object(json, length, "item", &item, &item_length) ||
            !save_output_item(workspace, item, item_length)) return -1;
        accept_function_call(workspace, item, item_length);
    } else if (strcmp(type, "response.failed") == 0 ||
               strcmp(type, "response.incomplete") == 0) {
        return -1;
    } else if (strcmp(type, "response.completed") == 0) {
        return 1;
    }
    return 0;
}

static int accept_stream_line(llm_workspace *workspace, char *line,
                              size_t length, bool *sse_seen)
{
    const char *json = line;

    if (workspace == NULL || line == NULL || sse_seen == NULL) return -1;
    while (length > 0U && (line[length - 1U] == '\r' ||
                           line[length - 1U] == '\n')) line[--length] = '\0';
    if (length == 0U || strncmp(line, "event:", 6U) == 0) return 0;
    if (length >= 5U && memcmp(line, "data:", 5U) == 0) {
        json = line + 5U;
        length -= 5U;
        if (length > 0U && *json == ' ') { json++; length--; }
        *sse_seen = true;
    } else if (*sse_seen) {
        return 0;
    }
    return process_sse_event(workspace, json, length);
}

static int read_http_stream(llm_workspace *workspace, int descriptor)
{
    char chunk[LLM_HTTP_READ_CAP];
    size_t used = 0U;
    bool sse_seen = false;
    unsigned int reads;
    int final = 0;

    if (workspace == NULL || descriptor < 0) return -1;
    for (reads = 0U; reads < 1048576U && final == 0; reads++) {
        ssize_t count = read(descriptor, chunk, sizeof(chunk));
        size_t index;

        if (count == -1 && errno == EINTR) { reads--; continue; }
        if (count <= 0) break;
        for (index = 0U; index < (size_t)count; index++) {
            if (used + 1U >= sizeof(workspace->event)) return -1;
            workspace->event[used++] = chunk[index];
            if (chunk[index] == '\n') {
                workspace->event[used] = '\0';
                final = accept_stream_line(workspace, workspace->event, used,
                                           &sse_seen);
                used = 0U;
                if (final != 0) break;
            }
        }
    }
    if (final == 0 && used > 0U) {
        workspace->event[used] = '\0';
        final = accept_stream_line(workspace, workspace->event, used,
                                   &sse_seen);
    }
    return final;
}

static int perform_response(const gsh_llm_provider_config *provider,
                            const gsh_shell_config *config, const char *key,
                            llm_workspace *workspace)
{
    int stream[2] = {-1, -1};
    pid_t pid;
    int parsed;
    int status = 0;

    if (provider == NULL || config == NULL || workspace == NULL ||
        pipe(stream) == -1) return -1;
    pid = fork();
    if (pid == 0) {
        (void)close(stream[0]);
        _exit(curl_request_child(stream[1], provider, config, key, workspace));
    }
    (void)close(stream[1]);
    if (pid < 0) { (void)close(stream[0]); return -1; }
    parsed = read_http_stream(workspace, stream[0]);
    (void)close(stream[0]);
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return parsed == 1 ? 0 : -1;
}

static void show_script(const char *script)
{
    if (script == NULL) return;
    (void)write_all(STDOUT_FILENO, "\n[gsh ai] command:\n$ ", 21U);
    (void)write_all(STDOUT_FILENO, script, strlen(script));
    (void)write_all(STDOUT_FILENO, "\n", 1U);
}

static bool confirm_script(int repl_fd)
{
    struct termios original;
    struct termios private;
    char answer[16];
    size_t used = 0U;
    bool changed = false;
    bool reported;

    /* ── Input Ownership Must Precede The Visible Question ─────────────
     * The managed compositor discovers private input by observing PTY echo.
     * Printing first exposed a race where a fast answer still reached the
     * editor, leaving the worker to read an empty or unrelated line. Change
     * the mode first, then publish the prompt, so visibility implies focus.
     * Restoring the exact original modes returns ownership after the answer.
     * ─────────────────────────────────────────────── */
    if (tcgetattr(STDIN_FILENO, &original) == 0) {
        private = original;
        private.c_lflag &= (tcflag_t)~ECHO;
        changed = tcsetattr(STDIN_FILENO, TCSANOW, &private) == 0;
    }
    reported = gsh_llm_repl_report_activity(
        repl_fd, GSH_LLM_CONFIRMATION) == 0;
    if (reported) (void)write_all(STDOUT_FILENO, "Execute? [y/N] ", 15U);
    while (reported && used + 1U < sizeof(answer)) {
        ssize_t count = read(STDIN_FILENO, answer + used, 1U);
        if (count == 1 && (answer[used] == '\n' || answer[used] == '\r'))
            break;
        if (count == 1) used++;
        else if (count == -1 && errno == EINTR) continue;
        else break;
    }
    if (changed) (void)tcsetattr(STDIN_FILENO, TCSANOW, &original);
    (void)write_all(STDOUT_FILENO, "\n", 1U);
    answer[used] = '\0';
    return strcmp(answer, "y") == 0 || strcmp(answer, "yes") == 0 ||
           strcmp(answer, "Y") == 0 || strcmp(answer, "YES") == 0;
}

static void append_tool_bytes(llm_workspace *workspace, const char *bytes,
                              size_t length, size_t *retained)
{
    size_t room;
    size_t accepted;

    if (workspace == NULL || bytes == NULL || retained == NULL ||
        *retained >= LLM_TOOL_OUTPUT_CAP)
        return;
    room = LLM_TOOL_OUTPUT_CAP - *retained - 1U;
    accepted = length < room ? length : room;
    (void)memcpy(workspace->tool_output + *retained, bytes, accepted);
    *retained += accepted;
    workspace->tool_output[*retained] = '\0';
}

static int execute_script(llm_workspace *workspace, const char *script)
{
    char bytes[4096];
    int output[2] = {-1, -1};
    pid_t pid;
    int status = 0;
    size_t retained = 0U;
    unsigned int reads;

    if (workspace == NULL || script == NULL || pipe(output) == -1) return 125;
    pid = fork();
    if (pid == 0) {
        (void)close(output[0]);
        if (dup2(output[1], STDOUT_FILENO) == -1 ||
            dup2(output[1], STDERR_FILENO) == -1) _exit(125);
        (void)close(output[1]);
        execl("/bin/sh", "sh", "-c", script, (char *)NULL);
        _exit(127);
    }
    (void)close(output[1]);
    if (pid < 0) { (void)close(output[0]); return 125; }
    for (reads = 0U; reads < 1048576U; reads++) {
        ssize_t count = read(output[0], bytes, sizeof(bytes));
        if (count > 0) {
            (void)write_all(STDOUT_FILENO, bytes, (size_t)count);
            append_tool_bytes(workspace, bytes, (size_t)count, &retained);
        } else if (count == -1 && errno == EINTR) reads--;
        else break;
    }
    (void)close(output[0]);
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    if (!WIFEXITED(status)) return 125;
    return WEXITSTATUS(status);
}

static int run_cli_tool(llm_workspace *workspace, const char *arguments)
{
    char script[LLM_TOOL_ARGUMENT_CAP];
    const gsh_llm_repl_result *result;

    if (workspace == NULL || arguments == NULL ||
        !gsh_json_get_string(arguments, strlen(arguments), "script", script,
                             sizeof(script)) || script[0] == '\0') return -1;
    if (!workspace->tools_enabled || workspace->repl_fd < 0 ||
        strlen(script) >= GSH_LLM_REPL_SCRIPT_CAP) {
        (void)snprintf(workspace->tool_output, sizeof(workspace->tool_output),
                       "Not executed: no owning REPL or command exceeds "
                       "the REPL input limit. No session state changed.");
        return 0;
    }
    if (gsh_llm_command_removes(script, &workspace->command_policy)) {
        show_script(script);
        if (!confirm_script(workspace->repl_fd)) {
            (void)snprintf(workspace->tool_output, sizeof(workspace->tool_output),
                           "Command cancelled by the user.");
            return 0;
        }
    }
    if (gsh_llm_repl_call(workspace->repl_fd, script,
                          &workspace->repl_result) == -1) return -1;
    result = &workspace->repl_result;
    (void)snprintf(workspace->tool_output, sizeof(workspace->tool_output),
                   "[REPL block: %llu; exit status: %d]\n"
                   "[session directory: %s]\n%.*s",
                   (unsigned long long)result->cell_id, result->status,
                   result->directory, 60000, result->output);
    return 0;
}

static int inherited_repl_descriptor(void)
{
    const char *text = getenv("GSH_LLM_REPL_FD");
    unsigned int value = 0U;
    size_t index;

    if (text == NULL || text[0] == '\0') return -1;
    for (index = 0U; index < 8U && text[index] != '\0'; index++) {
        if (text[index] < '0' || text[index] > '9' || value > 1024U)
            return -1;
        value = value * 10U + (unsigned int)(text[index] - '0');
    }
    if (index == 8U || value <= STDERR_FILENO || value > 1024U ||
        fcntl((int)value, F_SETFD, FD_CLOEXEC) == -1) return -1;
    return (int)value;
}

static int history_search_tool(llm_workspace *workspace,
                               const char *arguments)
{
    char query[1024];
    int length;

    if (workspace == NULL || arguments == NULL ||
        !gsh_json_get_string(arguments, strlen(arguments), "query", query,
                             sizeof(query)) || query[0] == '\0') return -1;
    length = gsh_llm_journal_search(getenv("HOME"), query, 20U,
                                    workspace->tool_output,
                                    sizeof(workspace->tool_output));
    if (length < 0) return -1;
    if (length == 0)
        (void)snprintf(workspace->tool_output, sizeof(workspace->tool_output),
                       "No journal entries matched.");
    return 0;
}

static bool append_tool_result(llm_workspace *workspace)
{
    gsh_json_writer writer;
    llm_item *item;

    if (workspace == NULL || workspace->item_count >= LLM_ITEM_CAP)
        return false;
    item = &workspace->items[workspace->item_count];
    gsh_json_writer_initialize(&writer, item->json, sizeof(item->json));
    if (!gsh_json_write_literal(&writer,
                                "{\"type\":\"function_call_output\","
                                "\"call_id\":")) return false;
    if (!gsh_json_write_string(&writer, workspace->tool.call_id,
                               strlen(workspace->tool.call_id)) ||
        !gsh_json_write_literal(&writer, ",\"output\":")) return false;
    if (!gsh_json_write_string(&writer, workspace->tool_output,
                               strlen(workspace->tool_output)) ||
        !gsh_json_write_literal(&writer, "}")) return false;
    item->length = writer.length;
    workspace->item_count++;
    return true;
}

static int service_tool(llm_workspace *workspace)
{
    int result;

    if (workspace == NULL || !workspace->tool.pending) return 0;
    if (strcmp(workspace->tool.name, "history_search") == 0)
        result = history_search_tool(workspace, workspace->tool.arguments);
    else if (strcmp(workspace->tool.name, "run_cli") == 0)
        result = run_cli_tool(workspace, workspace->tool.arguments);
    else result = -1;
    if (result == -1)
        (void)snprintf(workspace->tool_output, sizeof(workspace->tool_output),
                       "Tool arguments were invalid or the tool failed.");
    if (workspace->record)
        (void)gsh_llm_journal_append(getenv("HOME"), GSH_LLM_JOURNAL_TOOL,
                                 workspace->tool_output,
                                 strlen(workspace->tool_output));
    if (!append_tool_result(workspace)) return -1;
    (void)memset(&workspace->tool, 0, sizeof(workspace->tool));
    return 1;
}

static int run_agent(const gsh_shell_config *config,
                     const gsh_llm_provider_config *provider,
                     const char *key, llm_workspace *workspace)
{
    unsigned int round;

    for (round = 0U; round < LLM_TOOL_ROUND_CAP; round++) {
        int tool;

        if (workspace == NULL ||
            gsh_llm_repl_report_activity(workspace->repl_fd,
                                          GSH_LLM_GENERATING) == -1 ||
            !build_request(config, provider, workspace) ||
            perform_response(provider, config, key, workspace) == -1)
            return -1;
        tool = service_tool(workspace);
        if (tool < 0) return -1;
        if (tool == 0) return 0;
    }
    errno = ELOOP;
    return -1;
}

static int parse_prompt_descriptor(int argc, char **argv, bool *pipeline)
{
    unsigned int value = 0U;
    size_t index;

    if (argc != 3 || argv == NULL || argv[1] == NULL || argv[2] == NULL ||
        pipeline == NULL || argv[2][0] == '\0' ||
        (strcmp(argv[1], "--prompt-fd") != 0 &&
         strcmp(argv[1], "--pipeline-fd") != 0)) return -1;
    *pipeline = strcmp(argv[1], "--pipeline-fd") == 0;
    for (index = 0U; argv[2][index] != '\0'; index++) {
        if (argv[2][index] < '0' || argv[2][index] > '9' || value > 1024U)
            return -1;
        value = value * 10U + (unsigned int)(argv[2][index] - '0');
    }
    return value > STDERR_FILENO && value <= 1024U ? (int)value : -1;
}

static int prepare_pipeline_prompt(llm_workspace *workspace,
                                   size_t payload_length, const char *home,
                                   bool record)
{
    char *separator;
    const char *instruction;
    size_t command_length;
    int status;
    int length;

    if (workspace == NULL || home == NULL) return -1;
    separator = memchr(workspace->prompt, '\0', payload_length);
    if (separator == NULL || separator == workspace->prompt ||
        separator + 1U >= workspace->prompt + payload_length) return -1;
    command_length = (size_t)(separator - workspace->prompt);
    instruction = separator + 1U;
    if (record)
        (void)gsh_llm_journal_append(home, GSH_LLM_JOURNAL_COMMAND,
                                     workspace->prompt, command_length);
    workspace->tool_output[0] = '\0';
    status = execute_script(workspace, workspace->prompt);
    if (record)
        (void)gsh_llm_journal_append(home, GSH_LLM_JOURNAL_COMMAND_OUTPUT,
                                     workspace->tool_output,
                                     strlen(workspace->tool_output));
    length = snprintf(workspace->context, sizeof(workspace->context),
                      "Apply this instruction to the command output below. "
                      "The command has already run; do not rerun it.\n\n"
                      "Instruction:\n%s\n\nCommand:\n%.*s\n\n"
                      "Exit status: %d\nOutput:\n%s",
                      instruction, (int)command_length, workspace->prompt,
                      status, workspace->tool_output);
    if (length < 0 || (size_t)length >= sizeof(workspace->context) ||
        (size_t)length >= sizeof(workspace->prompt)) return -1;
    (void)memcpy(workspace->prompt, workspace->context, (size_t)length + 1U);
    return 0;
}

int main(int argc, char **argv)
{
    static llm_workspace workspace;
    gsh_shell_config config;
    const gsh_llm_provider_config *provider;
    const char *home = getenv("HOME");
    const char *private_setting = getenv("GSH_LLM_PRIVATE");
    const char *key;
    bool pipeline = false;
    bool record = private_setting == NULL ||
                  strcmp(private_setting, "1") != 0;
    int prompt_fd = parse_prompt_descriptor(argc, argv, &pipeline);
    int activity_lease = -1;
    ssize_t prompt_length;
    int result;

    if (prompt_fd < 0 || home == NULL || home[0] != '/') {
        (void)fputs("gsh llm: invalid launch context\n", stderr);
        return 2;
    }
    prompt_length = read_prompt(prompt_fd, workspace.prompt,
                                sizeof(workspace.prompt));
    (void)close(prompt_fd);
    gsh_config_defaults(&config);
    if (prompt_length <= 0 || gsh_config_load(&config, home, false) == -1 ||
        !config.llm_enabled) {
        (void)fputs("gsh llm: LLM is disabled or misconfigured\n", stderr);
        return 2;
    }
    workspace.tools_enabled = !pipeline;
    workspace.record = record;
    workspace.repl_fd = inherited_repl_descriptor();
    if (gsh_llm_repl_report_activity(workspace.repl_fd,
            pipeline ? GSH_LLM_IDLE : GSH_LLM_STARTING) == -1) return 2;
    if (pipeline && prepare_pipeline_prompt(
                        &workspace, (size_t)prompt_length, home,
                        record) == -1) {
        (void)fputs("gsh llm: invalid pipeline request\n", stderr);
        return 2;
    }
    if (pipeline) prompt_length = (ssize_t)strlen(workspace.prompt);
    provider = gsh_config_llm_provider(&config, config.llm_default_provider);
    key = provider_key(provider);
    if (!provider_valid(provider, key)) {
        (void)fputs("gsh llm: provider or credential is invalid\n", stderr);
        return 2;
    }
    if (provider->managed && provider->idle_timeout_seconds != 0U) {
        activity_lease = runtime_activity_begin(provider, home);
        if (activity_lease == -1) {
            (void)fputs("gsh llm: cannot acquire runtime activity lease\n",
                        stderr);
            return 2;
        }
    }
    if (gsh_llm_repl_report_activity(workspace.repl_fd,
                                      GSH_LLM_STARTING) == -1 ||
        !ensure_managed_runtime(provider, &config, key, home,
                                 activity_lease, workspace.repl_fd)) {
        runtime_activity_finish(activity_lease);
        (void)fputs("gsh llm: managed inference runtime did not start\n",
                    stderr);
        return 2;
    }
    if (record)
        (void)gsh_llm_journal_append(home, GSH_LLM_JOURNAL_PROMPT,
                                     workspace.prompt,
                                     (size_t)prompt_length);
    result = run_agent(&config, provider, key, &workspace);
    if (record && workspace.answer_length > 0U)
        (void)gsh_llm_journal_append(home, GSH_LLM_JOURNAL_ANSWER,
                                     workspace.answer,
                                     workspace.answer_length);
    runtime_activity_finish(activity_lease);
    if (result == -1) {
        (void)fputs("\ngsh llm: request failed\n", stderr);
        return 1;
    }
    if (workspace.answer_length == 0U ||
        workspace.answer[workspace.answer_length - 1U] != '\n')
        (void)write_all(STDOUT_FILENO, "\n", 1U);
    return 0;
}
