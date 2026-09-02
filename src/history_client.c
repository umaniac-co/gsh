#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "history_client.h"

#include "history_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h> /* CANON-INCLUDE: linux */
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    CONNECT_ATTEMPT_CAP = 100,
    CONNECT_DELAY_MS = 10,
    HISTORY_PATH_CAP = 4096,
    IO_ATTEMPT_CAP = 4096,
};

/* ── Shells Share One Bounded History Agent ─────────────────────
 * Keeping the key in each shell made an infinite unlock lifetime ambiguous.
 * A detached per-user agent now owns that lifetime and serializes vault writes.
 * The shell speaks a fixed protocol over a mode-0600 local Unix socket.
 * Startup failure degrades to session history instead of blocking the editor.
 * Every request and retry remains bounded independently of vault contents.
 * ─────────────────────────────────────────────────────────────── */

static void encode_u64(unsigned char output[8], uint64_t value)
{
    if (output == NULL) {
        return;
    }
    size_t index;

    for (index = 0; index < 8U; index++) {
        output[7U - index] = (unsigned char)(value >> (index * 8U));
    }
}

static uint64_t decode_u64(const unsigned char input[8])
{
    if (input == NULL) {
        return 0U;
    }
    uint64_t value = 0;
    size_t index;

    for (index = 0; index < 8U; index++) {
        value = (value << 8U) | input[index];
    }
    return value;
}

static void wipe_bytes(void *buffer, size_t length)
{
    if (buffer == NULL) {
        return;
    }
    volatile unsigned char *bytes = buffer;
    size_t index;

    for (index = 0; index < length; index++) {
        bytes[index] = 0;
    }
}

static int write_all(int descriptor, const void *buffer, size_t length)
{
    if (buffer == NULL) {
        return -1;
    }
    size_t offset = 0;
    unsigned int attempts = 0;

    while (offset < length && attempts < IO_ATTEMPT_CAP) {
        ssize_t count = write(descriptor,
                              (const unsigned char *)buffer + offset,
                              length - offset);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count == -1 && errno != EINTR) {
            return -1;
        }
        attempts++;
    }
    if (offset != length) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int read_all(int descriptor, void *buffer, size_t length)
{
    if (buffer == NULL) {
        return -1;
    }
    size_t offset = 0;
    unsigned int attempts = 0;

    while (offset < length && attempts < IO_ATTEMPT_CAP) {
        ssize_t count = read(descriptor, (unsigned char *)buffer + offset,
                             length - offset);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count != -1 || errno != EINTR) {
            return -1;
        }
        attempts++;
    }
    if (offset != length) {
        errno = EIO;
        return -1;
    }
    return 0;
}

static int secure_history_directory(const char *path)
{
    struct stat status;

    if (mkdir(path, S_IRWXU) == -1 && errno != EEXIST) {
        return -1;
    }
    if (lstat(path, &status) == -1 || !S_ISDIR(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 0077) != 0) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static int configure_paths(gsh_history_client *client, const char *home,
                           const char *program_path)
{
    if (client == NULL) return -1;
    if (program_path == NULL) {
        return -1;
    }
    char directory[HISTORY_PATH_CAP];
    const char *separator = strrchr(program_path, '/');
    size_t parent_length;

    if (home == NULL || home[0] != '/' || program_path == NULL ||
        snprintf(directory, sizeof(directory), "%s/.gsh", home) >=
            (int)sizeof(directory) ||
        secure_history_directory(directory) == -1 ||
        snprintf(client->socket_path, sizeof(client->socket_path),
                 "%s/history.sock", directory) >=
            (int)sizeof(client->socket_path) ||
        snprintf(client->vault_path, sizeof(client->vault_path),
                 "%s/history.vault", directory) >=
            (int)sizeof(client->vault_path)) {
        return -1;
    }
    if (separator == NULL) {
        (void)memcpy(client->agent_path, "gsh-history-agent", 18U);
        return 0;
    }
    parent_length = (size_t)(separator - program_path) + 1U;
    if (parent_length + sizeof("gsh-history-agent") >
        sizeof(client->agent_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)memcpy(client->agent_path, program_path, parent_length);
    (void)memcpy(client->agent_path + parent_length, "gsh-history-agent", 18U);
    return 0;
}

static int connect_agent(const char *socket_path)
{
    struct sockaddr_un address;
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);

    if (descriptor == -1 || strlen(socket_path) >= sizeof(address.sun_path)) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)memcpy(address.sun_path, socket_path, strlen(socket_path) + 1U);
    if (connect(descriptor, (struct sockaddr *)&address,
                sizeof(address)) == -1) {
        (void)close(descriptor);
        return -1;
    }
    if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1 ||
        setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) == -1 ||
        setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) == -1) {
        (void)close(descriptor);
        return -1;
    }
    return descriptor;
}

static void execute_agent(const gsh_history_client *client)
{
    if (client == NULL) {
        return;
    }
    int null_descriptor;
    pid_t child;

    if (setsid() == -1) {
        _exit(120);
    }
    child = fork();
    if (child < 0) {
        _exit(121);
    }
    if (child > 0) {
        _exit(0);
    }
    null_descriptor = open("/dev/null", O_RDWR);
    if (null_descriptor >= 0) {
        (void)dup2(null_descriptor, STDIN_FILENO);
        (void)dup2(null_descriptor, STDOUT_FILENO);
        (void)dup2(null_descriptor, STDERR_FILENO);
        if (null_descriptor > STDERR_FILENO) {
            (void)close(null_descriptor);
        }
    }
    execl(client->agent_path, client->agent_path, client->socket_path,
          client->vault_path, (char *)NULL);
    _exit(127);
}

static int start_agent(const gsh_history_client *client)
{
    if (client == NULL) {
        return -1;
    }
    pid_t child = fork();
    unsigned int attempts = 0;

    if (child == 0) {
        execute_agent(client);
    }
    if (child < 0) {
        return -1;
    }
    while (attempts < 8U && waitpid(child, NULL, 0) == -1) {
        if (errno != EINTR) {
            return -1;
        }
        attempts++;
    }
    if (attempts == 8U) {
        errno = EINTR;
        return -1;
    }
    return 0;
}

static int await_agent(const gsh_history_client *client)
{
    if (client == NULL) {
        return -1;
    }
    unsigned int attempt;

    for (attempt = 0; attempt < CONNECT_ATTEMPT_CAP; attempt++) {
        int descriptor = connect_agent(client->socket_path);

        if (descriptor >= 0) {
            return descriptor;
        }
        (void)poll(NULL, 0, CONNECT_DELAY_MS);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int send_message(gsh_history_client *client, uint32_t type,
                        const void *payload, size_t length)
{
    if (client == NULL) return -1;
    gsh_history_message message = {
        .magic = GSH_HISTORY_PROTOCOL_MAGIC,
        .version = GSH_HISTORY_PROTOCOL_VERSION,
        .type = type,
        .length = (uint32_t)length,
        .status = 0,
    };

    if (!client->connected || length > GSH_HISTORY_SERIALIZED_CAP ||
        write_all(client->descriptor, &message, sizeof(message)) == -1 ||
        (length != 0 &&
         write_all(client->descriptor, payload, length) == -1)) {
        client->connected = false;
        return -1;
    }
    return 0;
}

static int receive_response(gsh_history_client *client, int *status,
                            size_t *length)
{
    if (client == NULL) return -1;
    if (status == NULL) {
        return -1;
    }
    gsh_history_message response;

    if (read_all(client->descriptor, &response, sizeof(response)) == -1 ||
        response.magic != GSH_HISTORY_PROTOCOL_MAGIC ||
        response.version != GSH_HISTORY_PROTOCOL_VERSION ||
        response.type != GSH_HISTORY_MESSAGE_RESPONSE ||
        response.length > GSH_HISTORY_SERIALIZED_CAP) {
        client->connected = false;
        errno = EPROTO;
        return -1;
    }
    if (response.length != 0 &&
        read_all(client->descriptor, client->snapshot,
                 response.length) == -1) {
        client->connected = false;
        return -1;
    }
    *status = response.status;
    *length = response.length;
    return 0;
}

static int request_snapshot(gsh_history_client *client, uint32_t type,
                            const void *request, size_t request_length,
                            gsh_history_store *store, int *status,
                            uint64_t *reminder_ns)
{
    if (reminder_ns == NULL) {
        return -1;
    }
    size_t response_length;

    if (send_message(client, type, request, request_length) == -1 ||
        receive_response(client, status, &response_length) == -1) {
        wipe_bytes(client->snapshot, GSH_HISTORY_SERIALIZED_CAP);
        return -1;
    }
    if (*status != GSH_HISTORY_STATUS_OK) {
        wipe_bytes(client->snapshot, GSH_HISTORY_SERIALIZED_CAP);
        return 0;
    }
    if (response_length < 20U ||
        gsh_history_deserialize(store, client->snapshot + 8,
                                response_length - 8U) == -1) {
        wipe_bytes(client->snapshot, GSH_HISTORY_SERIALIZED_CAP);
        errno = EPROTO;
        return -1;
    }
    *reminder_ns = decode_u64(client->snapshot);
    wipe_bytes(client->snapshot, GSH_HISTORY_SERIALIZED_CAP);
    return 0;
}

int gsh_history_client_initialize(gsh_history_client *client,
                                  const char *home,
                                  const char *program_path,
                                  unsigned char *snapshot,
                                  size_t snapshot_capacity)
{
    int descriptor;

    if (client == NULL || snapshot == NULL ||
        snapshot_capacity < GSH_HISTORY_SERIALIZED_CAP) {
        errno = EINVAL;
        return -1;
    }
    (void)memset(client, 0, sizeof(*client));
    client->descriptor = -1;
    client->snapshot = snapshot;
    client->snapshot_capacity = snapshot_capacity;
    if (configure_paths(client, home, program_path) == -1) {
        return -1;
    }
    descriptor = connect_agent(client->socket_path);
    if (descriptor < 0) {
        if (start_agent(client) == -1 ||
            (descriptor = await_agent(client)) < 0) {
            return -1;
        }
    }
    client->descriptor = descriptor;
    client->connected = true;
    return 0;
}

int gsh_history_client_status(gsh_history_client *client,
                              uint64_t reminder_min_ns,
                              uint64_t reminder_max_ns,
                              gsh_history_store *store, int *status,
                              uint64_t *reminder_ns)
{
    unsigned char request[16];

    if (client == NULL || store == NULL || status == NULL ||
        reminder_ns == NULL) {
        errno = EINVAL;
        return -1;
    }
    encode_u64(request, reminder_min_ns);
    encode_u64(request + 8, reminder_max_ns);
    return request_snapshot(client, GSH_HISTORY_MESSAGE_STATUS, request,
                            sizeof(request), store, status, reminder_ns);
}

int gsh_history_client_unlock(gsh_history_client *client,
                              const char *passphrase, size_t length,
                              uint64_t reminder_min_ns,
                              uint64_t reminder_max_ns, bool reset,
                              gsh_history_store *store,
                              uint64_t *reminder_ns)
{
    unsigned char request[16 + GSH_HISTORY_SECRET_CAP];
    int status = EACCES;
    int result;

    if (client == NULL || passphrase == NULL || store == NULL ||
        reminder_ns == NULL || length == 0 ||
        length >= GSH_HISTORY_SECRET_CAP) {
        errno = EINVAL;
        return -1;
    }
    encode_u64(request, reminder_min_ns);
    encode_u64(request + 8, reminder_max_ns);
    (void)memcpy(request + 16, passphrase, length);
    result = request_snapshot(client,
                              reset ? GSH_HISTORY_MESSAGE_RESET
                                    : GSH_HISTORY_MESSAGE_UNLOCK,
                              request, length + 16U, store, &status,
                              reminder_ns);
    wipe_bytes(request, sizeof(request));
    if (result == -1 || status != GSH_HISTORY_STATUS_OK) {
        errno = status > 0 ? status : EACCES;
        return -1;
    }
    return 0;
}

int gsh_history_client_verify(gsh_history_client *client,
                              const char *passphrase, size_t length,
                              uint64_t reminder_min_ns,
                              uint64_t reminder_max_ns,
                              uint64_t *reminder_ns)
{
    unsigned char request[16 + GSH_HISTORY_SECRET_CAP];
    size_t response_length;
    int status = EACCES;
    int result;

    if (client == NULL || passphrase == NULL || reminder_ns == NULL ||
        length == 0 || length >= GSH_HISTORY_SECRET_CAP) {
        errno = EINVAL;
        return -1;
    }
    encode_u64(request, reminder_min_ns);
    encode_u64(request + 8, reminder_max_ns);
    (void)memcpy(request + 16, passphrase, length);
    result = send_message(client, GSH_HISTORY_MESSAGE_VERIFY, request,
                          length + 16U);
    wipe_bytes(request, sizeof(request));
    if (result == -1 ||
        receive_response(client, &status, &response_length) == -1 ||
        status != GSH_HISTORY_STATUS_OK || response_length != 8U) {
        wipe_bytes(client->snapshot, GSH_HISTORY_SERIALIZED_CAP);
        errno = status > 0 ? status : EACCES;
        return -1;
    }
    *reminder_ns = decode_u64(client->snapshot);
    wipe_bytes(client->snapshot, GSH_HISTORY_SERIALIZED_CAP);
    return 0;
}

int gsh_history_client_add(gsh_history_client *client,
                           const char *command, size_t length)
{
    gsh_history_message message = {
        .magic = GSH_HISTORY_PROTOCOL_MAGIC,
        .version = GSH_HISTORY_PROTOCOL_VERSION,
        .type = GSH_HISTORY_MESSAGE_ADD,
        .length = (uint32_t)length,
        .status = 0,
    };
    struct iovec vectors[2];
    struct msghdr request;
    ssize_t sent;

    if (client == NULL || command == NULL || length == 0 ||
        length >= GSH_HISTORY_ENTRY_CAP || !client->connected) {
        errno = EINVAL;
        return -1;
    }
    (void)memset(&request, 0, sizeof(request));
    vectors[0].iov_base = &message;
    vectors[0].iov_len = sizeof(message);
    vectors[1].iov_base = (void *)command;
    vectors[1].iov_len = length;
    request.msg_iov = vectors;
    request.msg_iovlen = 2;
    sent = sendmsg(client->descriptor, &request, MSG_DONTWAIT);
    if (sent != (ssize_t)(sizeof(message) + length)) {
        client->connected = false;
        errno = sent < 0 ? errno : EIO;
        return -1;
    }
    return 0;
}

int gsh_history_client_control(gsh_history_client *client, bool shutdown)
{
    size_t response_length;
    int status = EIO;

    if (client == NULL ||
        send_message(client,
                     shutdown ? GSH_HISTORY_MESSAGE_SHUTDOWN
                              : GSH_HISTORY_MESSAGE_LOCK,
                     NULL, 0) == -1 ||
        receive_response(client, &status, &response_length) == -1 ||
        status != GSH_HISTORY_STATUS_OK || response_length != 0) {
        errno = status > 0 ? status : EIO;
        return -1;
    }
    return 0;
}

void gsh_history_client_close(gsh_history_client *client)
{
    if (client == NULL) {
        return;
    }
    if (client->descriptor >= 0) {
        (void)close(client->descriptor);
    }
    if (client->snapshot != NULL) {
        wipe_bytes(client->snapshot, client->snapshot_capacity);
    }
    client->snapshot = NULL;
    client->snapshot_capacity = 0;
    client->descriptor = -1;
    client->connected = false;
}
