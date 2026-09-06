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
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "../src/llm_json.h"
#include "../src/llm_command_policy.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h> /* CANON-INCLUDE: macos */
#include <stdio.h>
#include <stdlib.h> /* CANON-INCLUDE: linux */
#include <string.h>
#include <sys/ioctl.h> /* CANON-INCLUDE: linux */
#include <sys/stat.h>
#include <sys/wait.h> /* CANON-INCLUDE: linux */
#include <time.h> /* CANON-INCLUDE: linux */
#include <unistd.h>

enum { REQUEST_CAP = 2 * 1024 * 1024, OUTPUT_CAP = 65536 };

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

static int create_listener(unsigned short *port)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int listener;
    int enabled = 1;

    if (port == NULL) return -1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return -1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof(enabled));
    (void)memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(listener, 2) == -1 ||
        getsockname(listener, (struct sockaddr *)&address, &length) == -1) {
        (void)close(listener);
        return -1;
    }
    *port = ntohs(address.sin_port);
    return listener;
}

static int create_listener_at(unsigned short port)
{
    struct sockaddr_in address;
    int listener;
    int enabled = 1;

    if (port == 0U) return -1;
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return -1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof(enabled));
    (void)memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        listen(listener, 4) == -1) {
        (void)close(listener);
        return -1;
    }
    return listener;
}

static size_t content_length(const char *request)
{
    static const char name[] = "Content-Length:";
    const char *field;
    size_t value = 0U;
    size_t digits;

    if (request == NULL) return 0U;
    field = strstr(request, name);
    if (field == NULL) return 0U;
    field += sizeof(name) - 1U;
    while (*field == ' ' || *field == '\t') field++;
    for (digits = 0U; digits < 12U && *field >= '0' && *field <= '9';
         digits++, field++) {
        if (value > REQUEST_CAP / 10U) return 0U;
        value = value * 10U + (size_t)(*field - '0');
    }
    return value;
}

static ssize_t read_request(int client, char *request, size_t capacity)
{
    size_t used = 0U;
    size_t wanted = 0U;
    unsigned int reads;

    if (client < 0 || request == NULL || capacity < 2U) return -1;
    for (reads = 0U; reads < 4096U && used + 1U < capacity; reads++) {
        ssize_t count = read(client, request + used, capacity - used - 1U);
        if (count > 0) used += (size_t)count;
        else if (count == -1 && errno == EINTR) { reads--; continue; }
        else break;
        request[used] = '\0';
        if (wanted == 0U) {
            char *body = strstr(request, "\r\n\r\n");
            if (body != NULL)
                wanted = (size_t)(body + 4U - request) +
                         content_length(request);
        }
        if (wanted != 0U && used >= wanted) break;
    }
    request[used] = '\0';
    return wanted != 0U && used >= wanted ? (ssize_t)used : -1;
}

static int send_sse(int client, const char *events)
{
    char header[256];
    int length;

    if (client < 0 || events == NULL) return -1;
    length = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      strlen(events));
    if (length < 0 || (size_t)length >= sizeof(header) ||
        write_all(client, header, (size_t)length) == -1 ||
        write_all(client, events, strlen(events)) == -1) return -1;
    return 0;
}

static int send_models(int client)
{
    static const char body[] =
        "{\"data\":[{\"id\":\"managed-model\"}]}";
    char header[256];
    int length;

    if (client < 0) return -1;
    length = snprintf(header, sizeof(header),
                      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                      sizeof(body) - 1U);
    if (length < 0 || (size_t)length >= sizeof(header) ||
        write_all(client, header, (size_t)length) == -1 ||
        write_all(client, body, sizeof(body) - 1U) == -1) return -1;
    return 0;
}

static int managed_runtime_server(unsigned short port, const char *marker)
{
    static const char events[] =
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\","
        "\"delta\":\"MANAGED_OK\"}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}\n\n";
    static char request[REQUEST_CAP];
    char process[64];
    int listener;
    int descriptor;
    int process_length;
    unsigned int attempts;

    if (port == 0U || marker == NULL || marker[0] != '/') return 125;
    listener = create_listener_at(port);
    if (listener < 0) return 125;
    descriptor = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
    process_length = snprintf(process, sizeof(process), "%ld\n", (long)getpid());
    if (descriptor < 0 || process_length < 0 ||
        (size_t)process_length >= sizeof(process) ||
        write_all(descriptor, process, (size_t)process_length) == -1 ||
        close(descriptor) == -1) {
        if (descriptor >= 0) (void)close(descriptor);
        (void)close(listener);
        return 125;
    }
    for (attempts = 0U; attempts < 32U; attempts++) {
        struct pollfd event = {.fd = listener, .events = POLLIN};
        int client;
        int result;

        if (poll(&event, 1U, 1000) <= 0) continue;
        client = accept(listener, NULL, NULL);
        if (client < 0) continue;
        result = read_request(client, request, sizeof(request)) < 0 ? -1 : 0;
        if (result == 0 && strstr(request, "POST /v1/responses") != NULL) {
            struct timespec generation = {.tv_sec = 2, .tv_nsec = 0};
            (void)nanosleep(&generation, NULL);
            result = send_sse(client, events);
        } else if (result == 0) result = send_models(client);
        (void)close(client);
        if (result == -1) { (void)close(listener); return 125; }
    }
    (void)close(listener);
    return 125;
}

static int serve_first_request(int listener)
{
    static const char events[] =
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{"
        "\"type\":\"function_call\",\"call_id\":\"call_1\","
        "\"name\":\"history_search\",\"arguments\":"
        "\"{\\\"query\\\":\\\"no-match-xyz\\\"}\"}}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}\n\n";
    static char request[REQUEST_CAP];
    int client = accept(listener, NULL, NULL);
    int result = 0;

    if (client < 0 || read_request(client, request, sizeof(request)) < 0 ||
        strstr(request, "POST /v1/responses") == NULL ||
        strstr(request, "\"store\":false") == NULL ||
        strstr(request, "\"history_search\"") == NULL ||
        send_sse(client, events) == -1) result = -1;
    if (client >= 0) (void)close(client);
    return result;
}

static int serve_second_request(int listener)
{
    static const char events[] =
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{"
        "\"type\":\"function_call\",\"call_id\":\"call_2\","
        "\"name\":\"run_cli\",\"arguments\":"
        "\"{\\\"script\\\":\\\"pwd\\\"}\"}}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}\n\n";
    static char request[REQUEST_CAP];
    int client = accept(listener, NULL, NULL);
    int result = 0;

    if (client < 0 || read_request(client, request, sizeof(request)) < 0 ||
        strstr(request, "\"type\":\"function_call_output\"") == NULL ||
        strstr(request, "No journal entries matched") == NULL ||
        send_sse(client, events) == -1) result = -1;
    if (client >= 0) (void)close(client);
    return result;
}

static int serve_third_request(int listener)
{
    static const char events[] =
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\","
        "\"delta\":\"GSH_OK\"}\n\n"
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{"
        "\"type\":\"message\",\"role\":\"assistant\",\"content\":[{"
        "\"type\":\"output_text\",\"text\":\"GSH_OK\"}]}}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}\n\n";
    static char request[REQUEST_CAP];
    int client = accept(listener, NULL, NULL);
    int result = 0;

    if (client < 0 || read_request(client, request, sizeof(request)) < 0 ||
        strstr(request, "\"call_id\":\"call_2\"") == NULL ||
        strstr(request, "Not executed: no owning REPL") == NULL ||
        send_sse(client, events) == -1) result = -1;
    if (client >= 0) (void)close(client);
    return result;
}

static int serve_pipeline_request(int listener)
{
    static const char events[] =
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\","
        "\"delta\":\"PIPE_OK\"}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}\n\n";
    static char request[REQUEST_CAP];
    int client = accept(listener, NULL, NULL);
    int result = 0;

    if (client < 0 || read_request(client, request, sizeof(request)) < 0 ||
        strstr(request, "pipeline-output") == NULL ||
        strstr(request, "summarize") == NULL ||
        strstr(request, "\"tools\":[]") == NULL ||
        strstr(request, "\"tool_choice\":\"none\"") == NULL ||
        send_sse(client, events) == -1) result = -1;
    if (client >= 0) (void)close(client);
    return result;
}

static int serve_error_request(int listener)
{
    static const char response[] =
        "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n"
        "Content-Length: 24\r\nConnection: close\r\n\r\n"
        "{\"error\":\"unauthorized\"}";
    static char request[REQUEST_CAP];
    int client = accept(listener, NULL, NULL);
    int result = 0;

    if (client < 0 || read_request(client, request, sizeof(request)) < 0 ||
        strstr(request, "POST /v1/responses") == NULL ||
        write_all(client, response, sizeof(response) - 1U) == -1) result = -1;
    if (client >= 0) (void)close(client);
    return result;
}

static int serve_confirmation_tool_request(int listener)
{
    static const char events[] =
        "event: response.output_item.done\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{"
        "\"type\":\"function_call\",\"call_id\":\"call_confirm\","
        "\"name\":\"run_cli\",\"arguments\":"
        "\"{\\\"script\\\":\\\"rm \\\\\\\"$HOME/removal-target\\\\\\\" "
        "&& printf TOOL_EXECUTED\\\"}\"}}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}\n\n";
    static char request[REQUEST_CAP];
    int client = accept(listener, NULL, NULL);
    int result = 0;

    if (client < 0 || read_request(client, request, sizeof(request)) < 0 ||
        strstr(request, "POST /v1/responses") == NULL ||
        send_sse(client, events) == -1) result = -1;
    if (client >= 0) (void)close(client);
    return result;
}

static int serve_confirmation_result(int listener, bool approved)
{
    static const char events[] =
        "event: response.output_text.delta\n"
        "data: {\"type\":\"response.output_text.delta\","
        "\"delta\":\"CONFIRM_OK\"}\n\n"
        "event: response.completed\n"
        "data: {\"type\":\"response.completed\",\"response\":{"
        "\"status\":\"completed\"}}\n\n";
    static char request[REQUEST_CAP];
    int client = accept(listener, NULL, NULL);
    int result = 0;

    if (client < 0 || read_request(client, request, sizeof(request)) < 0 ||
        (approved ? strstr(request, "exit status: 0") == NULL
                  : strstr(request, "Command cancelled by the user.") == NULL) ||
        send_sse(client, events) == -1) result = -1;
    if (client >= 0) (void)close(client);
    return result;
}

static int serve_repl_step(int listener, const char *script,
                           const char *expected)
{
    static char request[REQUEST_CAP];
    char arguments[8192];
    char events[16384];
    gsh_json_writer writer;
    int client;
    int result = -1;

    if (listener < 0 || expected == NULL) return -1;
    client = accept(listener, NULL, NULL);
    if (client < 0) return -1;
    if (read_request(client, request, sizeof(request)) < 0 ||
        strstr(request, expected) == NULL) {
        (void)fprintf(stderr, "REPL provider expected: %s\n", expected);
        (void)close(client);
        return -1;
    }
    if (strcmp(expected, "REPL") == 0) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 400000000L};
        (void)nanosleep(&pause, NULL);
    }
    if (script == NULL) {
        result = send_sse(client,
            "data: {\"type\":\"response.output_text.delta\","
            "\"delta\":\"REPL_DONE\"}\n\n"
            "data: {\"type\":\"response.completed\",\"response\":{"
            "\"status\":\"completed\"}}\n\n");
    } else {
        gsh_json_writer_initialize(&writer, arguments, sizeof(arguments));
        if (gsh_json_write_literal(&writer, "{\"script\":") &&
            gsh_json_write_string(&writer, script, strlen(script)) &&
            gsh_json_write_literal(&writer, "}")) {
            gsh_json_writer_initialize(&writer, events, sizeof(events));
            if (gsh_json_write_literal(&writer,
                    "data: {\"type\":\"response.output_item.done\",\"item\":{"
                    "\"type\":\"function_call\",\"name\":\"run_cli\","
                    "\"call_id\":\"repl_call\",\"arguments\":") &&
                gsh_json_write_string(&writer, arguments, strlen(arguments)) &&
                gsh_json_write_literal(&writer, "}}\n\n"
                    "data: {\"type\":\"response.completed\",\"response\":{"
                    "\"status\":\"completed\"}}\n\n"))
                result = send_sse(client, events);
        }
    }
    (void)close(client);
    return result;
}

static void repl_sequence_server(int listener, bool stale)
{
    int result;

    if (stale) {
        result = serve_repl_step(listener,
            "cd / && rm -f \"$HOME/removal-target\"", "REPL");
        if (result == 0)
            result = serve_repl_step(listener, NULL,
                                      "Not executed: session context changed");
    } else {
        result = serve_repl_step(listener, "pwd", "REPL");
        if (result == 0) result = serve_repl_step(listener,
            "cd / && export GSH_AI_REPL=kept && pwd", "exit status: 0");
        if (result == 0) result = serve_repl_step(listener,
            "printf '%s/%s' \"$PWD\" \"$GSH_AI_REPL\" | cat",
            "session directory: /");
        if (result == 0) result = serve_repl_step(listener,
            "cd /gsh-llm-missing-directory", "//kept");
        if (result == 0) result = serve_repl_step(listener,
            NULL, "exit status: 1");
    }
    (void)close(listener);
    _exit(result == 0 ? 0 : 1);
}

static void server_child(int listener)
{
    int result = serve_first_request(listener);
    if (result == 0) result = serve_second_request(listener);
    if (result == 0) result = serve_third_request(listener);
    if (result == 0) result = serve_pipeline_request(listener);
    if (result == 0) result = serve_error_request(listener);
    if (result == 0) result = serve_confirmation_tool_request(listener);
    if (result == 0) result = serve_confirmation_result(listener, true);
    if (result == 0) result = serve_confirmation_tool_request(listener);
    if (result == 0) result = serve_confirmation_result(listener, false);
    (void)close(listener);
    _exit(result == 0 ? 0 : 1);
}

static int write_configuration(const char *home, unsigned short port)
{
    char path[4096];
    char configuration[4096];
    int descriptor;
    int length;

    if (home == NULL ||
        snprintf(path, sizeof(path), "%s/.gshrc", home) >= (int)sizeof(path))
        return -1;
    length = snprintf(configuration, sizeof(configuration),
                      "config.version = 1\nllm.enabled = true\n"
                      "llm.default_provider = test\nllm.streaming = true\n"
                      "llm.auto_help = true\n"
                      "llm.context.recent_exchanges = 5\n"
                      "llm.request_timeout = 10s\n"
                      "llm.providers.test.type = responses\n"
                      "llm.providers.test.base_url = "
                      "\"http://127.0.0.1:%u/v1\"\n"
                      "llm.providers.test.model = test-model\n"
                      "llm.providers.test.credential = none\n",
                      (unsigned int)port);
    if (length < 0 || (size_t)length >= sizeof(configuration)) return -1;
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) return -1;
    if (write_all(descriptor, configuration, (size_t)length) == -1) {
        (void)close(descriptor);
        return -1;
    }
    return close(descriptor);
}

static int write_managed_configuration(const char *home, unsigned short port,
                                       const char *executable)
{
    char path[4096];
    char marker[4096];
    char configuration[8192];
    int descriptor;
    int length;

    if (home == NULL || executable == NULL || executable[0] != '/' ||
        snprintf(path, sizeof(path), "%s/.gshrc", home) >= (int)sizeof(path) ||
        snprintf(marker, sizeof(marker), "%s/managed.started", home) >=
            (int)sizeof(marker)) return -1;
    length = snprintf(configuration, sizeof(configuration),
                      "config.version = 1\nllm.enabled = true\n"
                      "llm.default_provider = managed\nllm.streaming = true\n"
                      "llm.auto_help = false\n"
                      "llm.context.recent_exchanges = 5\n"
                      "llm.request_timeout = 10s\n"
                      "llm.providers.managed.type = responses\n"
                      "llm.providers.managed.base_url = "
                      "\"http://127.0.0.1:%u/v1\"\n"
                      "llm.providers.managed.model = managed-model\n"
                      "llm.providers.managed.credential = none\n"
                      "llm.providers.managed.runtime.managed = true\n"
                      "llm.providers.managed.runtime.idle_timeout = 1s\n"
                      "llm.providers.managed.runtime.command = "
                      "\"%s --managed-runtime %u %s\"\n",
                      (unsigned int)port, executable, (unsigned int)port,
                      marker);
    if (length < 0 || (size_t)length >= sizeof(configuration)) return -1;
    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) return -1;
    if (write_all(descriptor, configuration, (size_t)length) == -1) {
        (void)close(descriptor);
        return -1;
    }
    return close(descriptor);
}

static pid_t start_worker(const char *worker, const char *home,
                          int prompt_read, int prompt_write,
                          int output_read, int output_write, bool pipeline)
{
    pid_t pid;
    char descriptor[32];

    if (worker == NULL || home == NULL) return -1;
    pid = fork();
    if (pid != 0) return pid;
    (void)close(prompt_write);
    (void)close(output_read);
    if (dup2(output_write, STDOUT_FILENO) == -1 ||
        dup2(output_write, STDERR_FILENO) == -1 ||
        snprintf(descriptor, sizeof(descriptor), "%d", prompt_read) >=
            (int)sizeof(descriptor) || setenv("HOME", home, 1) == -1)
        _exit(125);
    (void)close(output_write);
    execl(worker, "gsh-llm-worker",
          pipeline ? "--pipeline-fd" : "--prompt-fd", descriptor,
          (char *)NULL);
    _exit(127);
}

static ssize_t collect_output(int descriptor, char *output, size_t capacity)
{
    size_t used = 0U;
    unsigned int reads;

    if (descriptor < 0 || output == NULL || capacity < 2U) return -1;
    for (reads = 0U; reads < 4096U && used + 1U < capacity; reads++) {
        ssize_t count = read(descriptor, output + used, capacity - used - 1U);
        if (count > 0) used += (size_t)count;
        else if (count == -1 && errno == EINTR) reads--;
        else break;
    }
    output[used] = '\0';
    return (ssize_t)used;
}

static int exercise_worker(const char *worker, const char *home,
                           const char *prompt_text, size_t prompt_length,
                           bool pipeline, char *output, size_t output_capacity,
                           int *wait_status)
{
    int prompt[2] = {-1, -1};
    int channel[2] = {-1, -1};
    pid_t pid;
    int result = 0;

    if (worker == NULL || home == NULL || prompt_text == NULL ||
        output == NULL || wait_status == NULL || pipe(prompt) == -1)
        return -1;
    if (pipe(channel) == -1) {
        (void)close(prompt[0]);
        (void)close(prompt[1]);
        return -1;
    }
    pid = start_worker(worker, home, prompt[0], prompt[1], channel[0],
                       channel[1], pipeline);
    (void)close(prompt[0]);
    (void)close(channel[1]);
    if (pid < 0 || write_all(prompt[1], prompt_text, prompt_length) == -1)
        result = -1;
    (void)close(prompt[1]);
    if (collect_output(channel[0], output, output_capacity) < 0) result = -1;
    (void)close(channel[0]);
    if (pid < 0) return -1;
    while (waitpid(pid, wait_status, 0) == -1 && errno == EINTR) {
    }
    return result;
}

static void remove_fixture(const char *home)
{
    char path[4096];

    if (home == NULL) return;
    (void)snprintf(path, sizeof(path), "%s/managed.started", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.genshell/journal", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.genshell/journal.lock", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.genshell/runtime.log", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.genshell/runtime.lock", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path),
                   "%s/.genshell/runtime.managed.activity", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.genshell", home);
    (void)rmdir(path);
    (void)snprintf(path, sizeof(path), "%s/.gshrc", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.gsh_history", home);
    (void)unlink(path);
    (void)snprintf(path, sizeof(path), "%s/.gsh_history.lock", home);
    (void)unlink(path);
    (void)rmdir(home);
}

static int descriptor_argument(const char *text)
{
    unsigned int value = 0U;
    size_t index;

    if (text == NULL || text[0] == '\0') return -1;
    for (index = 0U; text[index] != '\0'; index++) {
        if (text[index] < '0' || text[index] > '9' || value > 1024U)
            return -1;
        value = value * 10U + (unsigned int)(text[index] - '0');
    }
    return value > STDERR_FILENO && value <= 1024U ? (int)value : -1;
}

static int port_argument(const char *text)
{
    unsigned int value = 0U;
    size_t index;

    if (text == NULL || text[0] == '\0') return -1;
    for (index = 0U; text[index] != '\0'; index++) {
        if (text[index] < '0' || text[index] > '9' || value > 65535U / 10U)
            return -1;
        value = value * 10U + (unsigned int)(text[index] - '0');
        if (value > 65535U) return -1;
    }
    return value > 0U ? (int)value : -1;
}

static pid_t read_marker_process(const char *path)
{
    char text[64];
    char *end = NULL;
    long value;
    ssize_t length;
    int descriptor;

    if (path == NULL || (descriptor = open(path, O_RDONLY)) == -1) return -1;
    length = read(descriptor, text, sizeof(text) - 1U);
    (void)close(descriptor);
    if (length <= 0) return -1;
    text[(size_t)length] = '\0';
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || (*end != '\n' && *end != '\0') ||
        value <= 1L) return -1;
    return (pid_t)value;
}

static int fake_repl_worker(int argc, char **argv)
{
    char payload[4096];
    size_t used = 0U;
    int descriptor;
    unsigned int reads;

    if (argc != 3 || argv == NULL || argv[1] == NULL || argv[2] == NULL ||
        (descriptor = descriptor_argument(argv[2])) < 0) return 125;
    for (reads = 0U; reads < 4096U && used < sizeof(payload); reads++) {
        ssize_t count = read(descriptor, payload + used,
                             sizeof(payload) - used);
        if (count > 0) used += (size_t)count;
        else if (count == -1 && errno == EINTR) reads--;
        else break;
    }
    (void)close(descriptor);
    if (strcmp(argv[1], "--prompt-fd") == 0)
        return write_all(STDOUT_FILENO, "FAKE_PROMPT=", 12U) == -1 ||
                       write_all(STDOUT_FILENO, payload, used) == -1 ||
                       write_all(STDOUT_FILENO, "\n", 1U) == -1;
    if (strcmp(argv[1], "--pipeline-fd") == 0) {
        char *separator = memchr(payload, '\0', used);
        size_t command_length;

        if (separator == NULL || separator + 1U >= payload + used) return 125;
        command_length = (size_t)(separator - payload);
        if (write_all(STDOUT_FILENO, "FAKE_PIPE_COMMAND=", 18U) == -1 ||
            write_all(STDOUT_FILENO, payload, command_length) == -1 ||
            write_all(STDOUT_FILENO, "\nFAKE_PIPE_INSTRUCTION=", 23U) == -1 ||
            write_all(STDOUT_FILENO, separator + 1U,
                      used - command_length - 1U) == -1 ||
            write_all(STDOUT_FILENO, "\n", 1U) == -1) return 1;
        return 0;
    }
    return 125;
}

static int launch_repl(const char *gsh, const char *worker, const char *home,
                       const char *argument_zero, bool managed,
                       int *master, pid_t *pid)
{
    struct winsize size = {.ws_row = 24U, .ws_col = 120U};
    char slave_name[4096];
    char *name;

    if (gsh == NULL || home == NULL || argument_zero == NULL ||
        master == NULL ||
        pid == NULL || (*master = posix_openpt(O_RDWR | O_NOCTTY)) == -1 ||
        grantpt(*master) == -1 || unlockpt(*master) == -1 ||
        (name = ptsname(*master)) == NULL ||
        strlen(name) >= sizeof(slave_name)) return -1;
    (void)memcpy(slave_name, name, strlen(name) + 1U);
    *pid = fork();
    if (*pid != 0) return *pid < 0 ? -1 : 0;
    if (setsid() == -1) _exit(125);
    {
        int slave = open(slave_name, O_RDWR);
        if (slave == -1) _exit(125);
#ifdef TIOCSCTTY
        if (ioctl(slave, TIOCSCTTY, 0) == -1 && errno != EINVAL) _exit(125);
#endif
        if (ioctl(slave, TIOCSWINSZ, &size) == -1 ||
            dup2(slave, STDIN_FILENO) == -1 ||
            dup2(slave, STDOUT_FILENO) == -1 ||
            dup2(slave, STDERR_FILENO) == -1) _exit(125);
        if (slave > STDERR_FILENO) (void)close(slave);
    }
    (void)close(*master);
    if (setenv("HOME", home, 1) == -1 ||
        (managed ? unsetenv("GSH_REPL")
                 : setenv("GSH_REPL", "classic", 1)) == -1 ||
        (worker == NULL ? unsetenv("GSH_LLM_WORKER")
                        : setenv("GSH_LLM_WORKER", worker, 1)) == -1 ||
        (worker == NULL && setenv("PATH", "/usr/bin:/bin", 1) == -1))
        _exit(125);
    execl(gsh, argument_zero, (char *)NULL);
    _exit(127);
}

static bool read_until(int descriptor, const char *needle, char *output,
                       size_t capacity)
{
    size_t used = 0U;
    unsigned int polls;

    if (descriptor < 0 || needle == NULL || output == NULL || capacity < 2U)
        return false;
    output[0] = '\0';
    for (polls = 0U; polls < 100U && used + 1U < capacity; polls++) {
        struct pollfd event = {.fd = descriptor, .events = POLLIN};
        ssize_t count;

        if (poll(&event, 1U, 50) == -1 && errno == EINTR) { polls--; continue; }
        if ((event.revents & POLLIN) == 0) continue;
        count = read(descriptor, output + used, capacity - used - 1U);
        if (count <= 0) break;
        used += (size_t)count;
        output[used] = '\0';
        if (strstr(output, needle) != NULL) return true;
    }
    return false;
}

static bool wait_for_prompt(int descriptor, const char *prior)
{
    char output[OUTPUT_CAP];

    if (prior != NULL && strstr(prior, "> ") != NULL) return true;
    return read_until(descriptor, "> ", output, sizeof(output));
}

static bool interact_repl_sequence(int master, bool stale,
                                   char *output, size_t capacity)
{
    static const char request[] = "? REPL sequence\r";
    static const char user_probe[] =
        "printf 'USER=%s:%s:END' \"$PWD\" \"$GSH_AI_REPL\"\r";

    if (master < 0 || output == NULL ||
        !read_until(master, "> ", output, capacity) ||
        write_all(master, request, sizeof(request) - 1U) == -1) return false;
    if (!read_until(master, "gsh ai> \033[32m⠙\033[0m\r\n", output, capacity))
        return false;
    if (stale) {
        if (!read_until(master, "Execute? [y/N]", output, capacity) ||
            write_all(master, "\035cd /tmp\r", 9U) == -1 ||
            !read_until(master, "/tmp", output, capacity) ||
            write_all(master, "fg\ry\r", 5U) == -1) return false;
    }
    if (!read_until(master, "REPL_DONE", output, capacity)) return false;
    if (stale) return strstr(output, "[AI] cd /") == NULL;
    if (strstr(output, "Execute? [y/N]") != NULL ||
        strstr(output, "Awaiting confirmation") != NULL) return false;
    if (strstr(output, "[AI] cd /gsh-llm-missing-directory") == NULL ||
        strstr(output, "gsh ai> ") == NULL) return false;
    if (write_all(master, user_probe, sizeof(user_probe) - 1U) == -1 ||
        !read_until(master, "USER=/:kept:END", output, capacity)) return false;
    return true;
}

static bool generated_history_is_plain(const char *home)
{
    char path[4096];
    char output[16384];
    int descriptor;
    ssize_t count;

    if (home == NULL || snprintf(path, sizeof(path), "%s/.gsh_history", home)
            >= (int)sizeof(path)) return false;
    descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return false;
    count = read(descriptor, output, sizeof(output) - 1U);
    (void)close(descriptor);
    if (count <= 0) return false;
    output[(size_t)count] = '\0';
    return strstr(output, "cd / && export GSH_AI_REPL=kept && pwd") != NULL &&
           strstr(output, "[AI]") == NULL;
}

static int repl_sequence_case(const char *gsh, const char *worker,
                              bool managed, bool stale)
{
    char home[] = "/tmp/gsh-llm-repl-XXXXXX";
    char output[OUTPUT_CAP] = {0};
    unsigned short port = 0U;
    int listener;
    int master = -1;
    pid_t pid = -1;
    pid_t server;
    int status = 0;
    int server_status = 0;
    bool passed;

    if (gsh == NULL || worker == NULL || mkdtemp(home) == NULL ||
        (listener = create_listener(&port)) < 0 ||
        write_configuration(home, port) == -1) return 1;
    server = fork();
    if (server == 0) repl_sequence_server(listener, stale);
    (void)close(listener);
    passed = server > 0 && launch_repl(gsh, worker, home, "gsh", managed,
                                      &master, &pid) == 0 &&
             interact_repl_sequence(master, stale, output, sizeof(output));
    if (passed) (void)write_all(master, "\035exit 0\r", 8U);
    else {
        (void)fprintf(stderr, "REPL sequence managed=%d stale=%d: %s\n",
                       managed ? 1 : 0, stale ? 1 : 0, output);
        if (pid > 0) (void)kill(pid, SIGKILL);
        if (server > 0) (void)kill(server, SIGKILL);
    }
    if (pid > 0)
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
        }
    if (master >= 0) (void)close(master);
    if (server > 0)
        while (waitpid(server, &server_status, 0) == -1 && errno == EINTR) {
        }
    if (passed && !stale && !generated_history_is_plain(home)) {
        (void)fputs("REPL history: generated command or plain recall missing\n",
                    stderr);
        passed = false;
    }
    remove_fixture(home);
    return passed && status == 0 && server_status == 0 ? 0 : 1;
}

static int managed_confirmation_case(const char *gsh, const char *worker,
                                     const char *home, bool approved)
{
    static const char request[] = "? confirmation test\r";
    static const char detach_and_exit[] = "\035exit\r";
    char output[OUTPUT_CAP];
    int master = -1;
    pid_t pid = -1;
    int status = 0;
    int stage = 0;
    bool okay;
    bool passed = false;
    char target[4096];
    int fixture;

    if (home == NULL || snprintf(target, sizeof(target), "%s/removal-target", home)
            >= (int)sizeof(target)) return 1;
    fixture = open(target, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fixture < 0 || close(fixture) == -1) return 1;
    output[0] = '\0';
    okay = launch_repl(gsh, worker, home, "gsh", true,
                       &master, &pid) == 0;
    if (okay) { stage = 1; okay = read_until(
        master, "> ", output, sizeof(output)); }
    if (okay) { stage = 2; okay = write_all(
        master, request, sizeof(request) - 1U) == 0; }
    if (okay) { stage = 3; okay = read_until(
        master, "Execute? [y/N]", output, sizeof(output)); }
    if (okay && strstr(output,
            "\033[38;5;245mAwaiting confirmation\033[0m") == NULL)
        okay = read_until(master,
            "\033[38;5;245mAwaiting confirmation\033[0m", output, sizeof(output));
    if (okay && access(target, F_OK) != 0) okay = false;
    if (okay) { stage = 4; okay = write_all(
        master, approved ? "y\r" : "n\r", 2U) == 0; }
    if (okay) { stage = 5; okay = read_until(
        master, "CONFIRM_OK", output, sizeof(output)); }
    if (okay) okay = approved ? access(target, F_OK) == -1
                              : access(target, F_OK) == 0;
    if (!approved) (void)unlink(target);
    if (!okay) {
        if (pid > 0) (void)kill(pid, SIGKILL);
    } else {
        passed = true;
        (void)write_all(master, detach_and_exit,
                        sizeof(detach_and_exit) - 1U);
    }
    if (pid > 0)
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
        }
    if (master >= 0) (void)close(master);
    if (!passed || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        (void)fprintf(stderr,
                      "managed confirmation: stage=%d passed=%d status=%d "
                      "output=%s\n", stage, passed ? 1 : 0, status, output);
        return 1;
    }
    return 0;
}

static int managed_launcher_case(const char *gsh, const char *executable,
                                 const char *home)
{
    static const char change_directory[] = "cd /tmp\n";
    static const char command[] = "? managed test\n";
    char marker[4096];
    char log_path[4096];
    char output[OUTPUT_CAP] = {0};
    unsigned short port = 0U;
    int reservation;
    int master = -1;
    pid_t pid = -1;
    pid_t runtime = -1;
    int status = 0;
    unsigned int attempt;
    bool passed;
    bool stopped = false;

    if (gsh == NULL || executable == NULL || home == NULL ||
        (reservation = create_listener(&port)) < 0) return 1;
    (void)close(reservation);
    if (write_managed_configuration(home, port, executable) == -1 ||
        launch_repl(gsh, NULL, home, "./build/gsh", false,
                    &master, &pid) == -1 ||
        !read_until(master, "> ", output, sizeof(output)) ||
        write_all(master, change_directory,
                  sizeof(change_directory) - 1U) == -1 ||
        !read_until(master, "> ", output, sizeof(output)) ||
        write_all(master, command, sizeof(command) - 1U) == -1 ||
        !read_until(master, "MANAGED_OK", output, sizeof(output))) {
        if (pid > 0) (void)kill(pid, SIGKILL);
        if (master >= 0) (void)close(master);
        if (pid > 0)
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
            }
        (void)fprintf(stderr, "managed launcher: failed: %s\n", output);
        return 1;
    }
    passed = strstr(output, "\033[38;5;245m Starting...\033[0m") != NULL;
    if (wait_for_prompt(master, output))
        (void)write_all(master, "exit\n", sizeof("exit\n") - 1U);
    (void)close(master);
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    if (snprintf(marker, sizeof(marker), "%s/managed.started", home) >=
            (int)sizeof(marker) ||
        snprintf(log_path, sizeof(log_path), "%s/.genshell/runtime.log",
                 home) >= (int)sizeof(log_path) ||
        !passed || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        (runtime = read_marker_process(marker)) <= 1 ||
        access(log_path, F_OK) == -1) {
        (void)fprintf(stderr, "managed launcher: invalid final state: %s\n",
                      output);
        return 1;
    }
    for (attempt = 0U; attempt < 120U; attempt++) {
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 50000000L};

        if (kill(runtime, 0) == -1 && errno == ESRCH) {
            stopped = true;
            break;
        }
        (void)nanosleep(&pause, NULL);
    }
    if (!stopped) {
        (void)kill(-runtime, SIGKILL);
        (void)fprintf(stderr, "managed launcher: idle runtime stayed active\n");
        return 1;
    }
    return 0;
}

static int repl_routing_case(const char *gsh, const char *worker,
                             const char *home)
{
    static const char prompt_command[] = "? hello\n";
    static const char pipeline_command[] =
        "printf pipeline-output | ? summarize\n";
    char output[OUTPUT_CAP];
    int master = -1;
    pid_t pid = -1;
    int status = 0;
    bool passed;

    if (launch_repl(gsh, worker, home, "gsh", false,
                    &master, &pid) == -1 ||
        !read_until(master, "> ", output, sizeof(output)) ||
        write_all(master, prompt_command, sizeof(prompt_command) - 1U) == -1 ||
        !read_until(master, "FAKE_PROMPT=hello", output, sizeof(output)) ||
        !wait_for_prompt(master, output) ||
        write_all(master, pipeline_command,
                  sizeof(pipeline_command) - 1U) == -1 ||
        !read_until(master, "FAKE_PIPE_INSTRUCTION=summarize", output,
                    sizeof(output))) {
        (void)fprintf(stderr, "REPL routing: initial steps: %s\n", output);
        if (pid > 0) (void)kill(pid, SIGKILL);
        if (master >= 0) (void)close(master);
        if (pid > 0)
            while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
            }
        return 1;
    }
    passed = strstr(output,
                    "FAKE_PIPE_COMMAND=printf pipeline-output") != NULL;
    if (!passed || !wait_for_prompt(master, output) ||
        write_all(master, "cd /gsh-auto-help-missing\n",
                  sizeof("cd /gsh-auto-help-missing\n") - 1U) == -1 ||
        !read_until(master,
                    "FAKE_PROMPT=A top-level gsh command failed",
                    output, sizeof(output))) {
        (void)fprintf(stderr, "REPL routing: error help: %s\n", output);
        (void)kill(pid, SIGKILL);
        (void)close(master);
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
        }
        return 1;
    }
    if (wait_for_prompt(master, output))
        (void)write_all(master, "exit 0\n", sizeof("exit 0\n") - 1U);
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
    }
    (void)close(master);
    if (status != 0)
        (void)fprintf(stderr, "REPL routing: exit status=%d\n", status);
    return passed && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1;
}

/* ── Another Session's Journal Lock Cannot Own The Terminal ─────
 * Unit append tests intentionally wait for a lock, but the shell reactor
 * must continue accepting commands while its separate writer waits there.
 * Holding the lock through both command execution and exit verifies that
 * neither editing mode nor shutdown can regress to synchronous journal I/O.
 * The fixture uses no provider request and releases every lock on failure.
 * ─────────────────────────────────────────────────────────────── */
static int journal_contention_case(const char *gsh, bool managed)
{
    static const char command[] = "printf '%s%s\\n' JOURNAL _RESPONSIVE\r";
    char home[] = "/tmp/gsh-journal-pty-XXXXXX";
    char path[4096];
    char output[OUTPUT_CAP];
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    int descriptor;
    int master = -1;
    pid_t pid = -1;
    pid_t waited = -1;
    int status = 0;
    bool passed;
    unsigned int attempt;

    if (gsh == NULL || mkdtemp(home) == NULL ||
        write_configuration(home, 9U) == -1 ||
        snprintf(path, sizeof(path), "%s/.genshell", home) >= (int)sizeof(path) ||
        mkdir(path, 0700) == -1 ||
        snprintf(path, sizeof(path), "%s/.genshell/journal.lock", home)
            >= (int)sizeof(path)) return 1;
    descriptor = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) { remove_fixture(home); return 1; }
    passed = fcntl(descriptor, F_SETLK, &lock) == 0 &&
        launch_repl(gsh, NULL, home, "gsh", managed, &master, &pid) == 0 &&
        read_until(master, "> ", output, sizeof(output)) &&
        write_all(master, command, sizeof(command) - 1U) == 0 &&
        read_until(master, "JOURNAL_RESPONSIVE", output, sizeof(output)) &&
        write_all(master, "exit 0\r", 7U) == 0;
    for (attempt = 0U; passed && attempt < 200U; attempt++) {
        waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) break;
        (void)poll(NULL, 0U, 5);
    }
    if (waited != pid) passed = false;
    (void)close(descriptor);
    if (pid > 0 && waited != pid) {
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, &status, 0) == -1 && errno == EINTR) {
        }
    }
    if (master >= 0) (void)close(master);
    remove_fixture(home);
    if (!passed || status != 0) {
        (void)fprintf(stderr, "journal contention: managed=%d status=%d\n",
                       managed ? 1 : 0, status);
        return 1;
    }
    return 0;
}

static int removal_policy_cases(void)
{
    static const struct { const char *script; bool removes; } cases[] = {
        {"cd .. && cd life.c && pwd && ls -la", false},
        {"printf hello > result.txt; export ANSWER=42", false},
        {"make install && npm install && custom-cli publish", false},
        {"git add . && git commit -m update && git push", false},
        {"mv old new && cp new copy && mkdir directory", false},
        {"printf '%s' 'rm -rf directory'", false},
        {"echo rm file; grep rm README.md; cat rm", false},
        {"# rm file\npwd", false},
        {"cat <<'EOF'\nrm -rf /\nEOF\n", false},
        {"find . -name rm -print", false},
        {"find . -name '-delete' -print", false},
        {"find . -exec echo -delete \\;", false},
        {"npm run remove", false},
        {"git clean -ndf", false},
        {"git rm --dry-run file", false},
        {"git branch -a; git tag -l", false},
        {"command -v rm rmdir", false},
        {"rm --help; rm --version", false},
        {"rm", false},
        {"sh -c 'printf rm'", false},
        {"echo '$(rm file)'", false},
        {"rm file", true},
        {"rm -rf -- directory", true},
        {"/bin/rm 'file with spaces'", true},
        {"'rm' file; r\\mdir empty", true},
        {"unlink file", true},
        {"cd /tmp && rm file", true},
        {"printf x | rm file", true},
        {"if true; then rm file; fi", true},
        {"for f in a b; do rm \"$f\"; done", true},
        {"sudo -u root /bin/rm file", true},
        {"env TEST=value command rm file", true},
        {"xargs -0 -I {} rm -- {}", true},
        {"find . -type f -delete", true},
        {"find . -exec echo {} \\; -delete", true},
        {"find . -exec /bin/rm {} \\;", true},
        {"git rm file", true},
        {"git -C repository clean -fd", true},
        {"git branch -D topic; git tag --delete release", true},
        {"git stash clear", true},
        {"docker container rm test", true},
        {"docker image prune -a", true},
        {"kubectl delete pod test", true},
        {"kubectl -n test delete pod test", true},
        {"brew uninstall package", true},
        {"sh -c 'rm file'", true},
        {"eval 'rm file'", true},
        {"printf '%s' \"$(rm file)\"", true},
        {"echo `rm file`", true},
        {"echo $(printf '%s' \"$(rm file)\")", true},
    };
    static gsh_llm_command_policy policy;
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); index++) {
        if (gsh_llm_command_removes(cases[index].script, &policy) !=
            cases[index].removes) {
            (void)fprintf(stderr, "removal policy mismatch: %s\n", cases[index].script);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const char pipeline_prompt[] =
        "printf pipeline-output\0summarize";
    char home[] = "/tmp/gsh-llm-worker-XXXXXX";
    char managed_home[] = "/tmp/gsh-llm-managed-XXXXXX";
    char output[OUTPUT_CAP];
    char pipeline_output[OUTPUT_CAP] = {0};
    char error_output[OUTPUT_CAP] = {0};
    unsigned short port = 0U;
    int listener;
    pid_t server;
    int server_status = 0;
    int worker_status = 0;
    int pipeline_status = 0;
    int error_status = 0;
    int failed = 0;

    if (argc == 4 && argv != NULL && argv[1] != NULL && argv[2] != NULL &&
        argv[3] != NULL && strcmp(argv[1], "--managed-runtime") == 0) {
        int managed_port = port_argument(argv[2]);
        return managed_port < 0
                   ? 125
                   : managed_runtime_server((unsigned short)managed_port,
                                            argv[3]);
    }
    if (argc == 3 && argv != NULL && argv[1] != NULL &&
        (strcmp(argv[1], "--prompt-fd") == 0 ||
         strcmp(argv[1], "--pipeline-fd") == 0))
        return fake_repl_worker(argc, argv);
    if (argc != 3 || argv == NULL || argv[1] == NULL || argv[2] == NULL ||
        removal_policy_cases() != 0 ||
        mkdtemp(home) == NULL ||
        (listener = create_listener(&port)) < 0 ||
        write_configuration(home, port) == -1) return 1;
    server = fork();
    if (server == 0) server_child(listener);
    (void)close(listener);
    if (server < 0 || exercise_worker(
            argv[1], home, "integration test", 16U, false, output,
            sizeof(output), &worker_status) == -1) failed = 1;
    if (exercise_worker(argv[1], home, pipeline_prompt,
                        sizeof(pipeline_prompt) - 1U, true, pipeline_output,
                        sizeof(pipeline_output), &pipeline_status) == -1)
        failed = 1;
    if (exercise_worker(argv[1], home, "error test", 10U, false,
                        error_output, sizeof(error_output),
                        &error_status) == -1) failed = 1;
    if (managed_confirmation_case(argv[2], argv[1], home, true) != 0 ||
        managed_confirmation_case(argv[2], argv[1], home, false) != 0) failed = 1;
    if (failed && server > 0) (void)kill(server, SIGKILL);
    if (server > 0)
        while (waitpid(server, &server_status, 0) == -1 && errno == EINTR) {
        }
    if (failed || !WIFEXITED(worker_status) || WEXITSTATUS(worker_status) != 0 ||
        !WIFEXITED(server_status) || WEXITSTATUS(server_status) != 0 ||
        !WIFEXITED(pipeline_status) || WEXITSTATUS(pipeline_status) != 0 ||
        !WIFEXITED(error_status) || WEXITSTATUS(error_status) != 1 ||
        strstr(output, "GSH_OK") == NULL ||
        strstr(output, "[gsh ai] command:\n$ pwd\n") != NULL ||
        strstr(pipeline_output, "pipeline-output") == NULL ||
        strstr(pipeline_output, "PIPE_OK") == NULL ||
        strstr(error_output, "gsh llm: request failed") == NULL ||
        repl_routing_case(argv[2], argv[0], home) != 0) failed = 1;
    if (mkdtemp(managed_home) == NULL ||
        managed_launcher_case(argv[2], argv[0], managed_home) != 0)
        failed = 1;
    remove_fixture(home);
    remove_fixture(managed_home);
    if (failed) {
        (void)fprintf(stderr, "llm worker: failed: worker=%d server=%d "
            "pipeline=%d error=%d: %s / %s\n", worker_status, server_status,
            pipeline_status, error_status, output, pipeline_output);
        return 1;
    }
    if (repl_sequence_case(argv[2], argv[1], true, false) != 0 ||
        repl_sequence_case(argv[2], argv[1], false, false) != 0 ||
        repl_sequence_case(argv[2], argv[1], true, true) != 0 ||
        journal_contention_case(argv[2], false) != 0 ||
        journal_contention_case(argv[2], true) != 0) return 1;
    (void)puts("llm worker: streaming, tools, managed launch, HTTP failure, "
               "pipeline and REPL routing passed");
    return 0;
}
