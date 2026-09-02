#if defined(__linux__)
#define _GNU_SOURCE
#endif
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif
#if _POSIX_C_SOURCE < 202405L
#error "gsh requires the POSIX.1-2024 feature-test baseline"
#endif

#include "history_protocol.h"
#include "history_store.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sodium.h>
#include <string.h> /* CANON-INCLUDE: macos */
#include <sys/resource.h> /* CANON-INCLUDE: linux */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h> /* CANON-INCLUDE: macos */

enum {
    AGENT_CLIENT_CAP = 16,
    AGENT_IO_ATTEMPT_CAP = 4096,
    VAULT_HEADER_CAP = 76,
    AGENT_SECRET_CAP = 1024,
    AGENT_REQUEST_CAP = GSH_HISTORY_ENTRY_CAP + AGENT_SECRET_CAP + 32,
};

typedef struct {
    int listener;
    int lock_descriptor;
    int clients[AGENT_CLIENT_CAP];
    char socket_path[4096];
    char vault_path[4096];
    bool unlocked;
    bool vault_exists;
    bool running;
    bool key_memory_locked;
    unsigned char key[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
    unsigned char salt[crypto_pwhash_SALTBYTES];
    unsigned long long opslimit;
    size_t memlimit;
    uint64_t reminder_deadline_ns;
    gsh_history_store *store;
    unsigned char *plain;
    unsigned char *cipher;
} agent_state;

typedef struct {
    gsh_history_store store;
    unsigned char plain[GSH_HISTORY_SERIALIZED_CAP];
    unsigned char cipher[GSH_HISTORY_SERIALIZED_CAP +
                         crypto_aead_xchacha20poly1305_ietf_ABYTES];
} agent_storage;

static const unsigned char vault_magic[8] = {
    'G', 'S', 'H', 'V', 'A', 'U', 'L', '1'};

/* ── One Agent Owns Decrypted Persistent History ────────────
 * Independent shells previously had no safe way to share encrypted updates.
 * One per-user agent now owns the key, decrypted ring, and vault replacement.
 * A same-directory advisory lock prevents two racing agents from owning it.
 * Clients authenticate by kernel peer identity and never receive the key.
 * The agent accepts fixed-size messages and one bounded 1024-entry snapshot.
 * A failed agent leaves the shell usable with its in-process session history.
 * ─────────────────────────────────────────────── */

static void encode_u32(unsigned char output[4], uint32_t value)
{
    if (output == NULL) {
        return;
    }
    output[0] = (unsigned char)(value >> 24);
    output[1] = (unsigned char)(value >> 16);
    output[2] = (unsigned char)(value >> 8);
    output[3] = (unsigned char)value;
}

static uint32_t decode_u32(const unsigned char input[4])
{
    if (input == NULL) {
        return 0U;
    }
    return ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) |
           ((uint32_t)input[2] << 8) | (uint32_t)input[3];
}

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

static int read_all(int descriptor, void *buffer, size_t length)
{
    if (buffer == NULL) {
        return -1;
    }
    size_t offset = 0;
    unsigned int attempts = 0;

    while (offset < length && attempts < AGENT_IO_ATTEMPT_CAP) {
        ssize_t count = read(descriptor, (unsigned char *)buffer + offset,
                             length - offset);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
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

static int write_all(int descriptor, const void *buffer, size_t length)
{
    if (buffer == NULL) {
        return -1;
    }
    size_t offset = 0;
    unsigned int attempts = 0;

    while (offset < length && attempts < AGENT_IO_ATTEMPT_CAP) {
        ssize_t count = write(descriptor,
                              (const unsigned char *)buffer + offset,
                              length - offset);

        if (count > 0) {
            offset += (size_t)count;
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
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

static int send_response(int descriptor, int status, const void *payload,
                         size_t length)
{
    if (length != 0U && payload == NULL) {
        return -1;
    }
    gsh_history_message message = {
        .magic = GSH_HISTORY_PROTOCOL_MAGIC,
        .version = GSH_HISTORY_PROTOCOL_VERSION,
        .type = GSH_HISTORY_MESSAGE_RESPONSE,
        .length = (uint32_t)length,
        .status = status,
    };

    if (length > GSH_HISTORY_SERIALIZED_CAP + 8U ||
        write_all(descriptor, &message, sizeof(message)) == -1) {
        return -1;
    }
    return length == 0 || write_all(descriptor, payload, length) == 0 ? 0
                                                                      : -1;
}

static bool peer_allowed(int descriptor)
{
#if defined(__APPLE__)
    uid_t uid;
    gid_t gid;

    return getpeereid(descriptor, &uid, &gid) == 0 && uid == geteuid();
#elif defined(__linux__)
    struct ucred credentials;
    socklen_t length = sizeof(credentials);

    return getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                      &length) == 0 && credentials.uid == geteuid();
#else
    (void)descriptor;
    return false;
#endif
}

static int secure_vault_descriptor(int descriptor)
{
    struct stat status;

    if (fstat(descriptor, &status) == -1 || !S_ISREG(status.st_mode) ||
        status.st_uid != geteuid() || (status.st_mode & 0077) != 0 ||
        status.st_nlink != 1) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

static void build_header(agent_state *state, unsigned char *header,
                         size_t plain_length)
{
    if (header == NULL || state == NULL) {
        return;
    }
    (void)memset(header, 0, VAULT_HEADER_CAP);
    (void)memcpy(header, vault_magic, sizeof(vault_magic));
    encode_u32(header + 8, 1U);
    encode_u64(header + 12, state->opslimit);
    encode_u64(header + 20, (uint64_t)state->memlimit);
    (void)memcpy(header + 28, state->salt, crypto_pwhash_SALTBYTES);
    randombytes_buf(header + 44,
                    crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
    encode_u64(header + 68, (uint64_t)plain_length);
}

static int atomic_replace(agent_state *state, const unsigned char *header,
                          size_t cipher_length)
{
    if (state == NULL) return -1;
    char temporary[4096];
    int descriptor;
    int result = 0;

    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld",
                 state->vault_path, (long)getpid()) >=
        (int)sizeof(temporary)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                     O_NOFOLLOW,
                      S_IRUSR | S_IWUSR);
    if (descriptor == -1) {
        return -1;
    }
    if (write_all(descriptor, header, VAULT_HEADER_CAP) == -1 ||
        write_all(descriptor, state->cipher, cipher_length) == -1 ||
        fsync(descriptor) == -1) {
        result = -1;
    }
    if (close(descriptor) == -1) {
        result = -1;
    }
    if (result == 0 && rename(temporary, state->vault_path) == -1) {
        result = -1;
    }
    if (result == -1) {
        (void)unlink(temporary);
    }
    return result;
}

static int save_vault(agent_state *state)
{
    if (state == NULL) {
        return -1;
    }
    unsigned char header[VAULT_HEADER_CAP];
    unsigned long long cipher_length = 0;
    size_t plain_length = gsh_history_serialize(
        state->store, state->plain, GSH_HISTORY_SERIALIZED_CAP);

    if (plain_length == 0) {
        return -1;
    }
    build_header(state, header, plain_length);
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(
            state->cipher, &cipher_length, state->plain, plain_length,
            header, VAULT_HEADER_CAP, NULL, header + 44, state->key) != 0 ||
        cipher_length != plain_length +
                             crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        errno = EIO;
        return -1;
    }
    if (atomic_replace(state, header, (size_t)cipher_length) == -1) {
        return -1;
    }
    state->vault_exists = true;
    return 0;
}

static int derive_key(const char *passphrase, size_t length,
                      const unsigned char *salt,
                      unsigned long long opslimit, size_t memlimit,
                      unsigned char *key)
{
    if (length == 0 || length >= AGENT_SECRET_CAP ||
        crypto_pwhash(key, crypto_aead_xchacha20poly1305_ietf_KEYBYTES,
                      passphrase, length, salt, opslimit, memlimit,
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

static int read_vault_file(agent_state *state, unsigned char *header,
                           size_t *cipher_length)
{
    if (cipher_length == NULL || state == NULL) {
        return -1;
    }
    struct stat status;
    int descriptor = open(state->vault_path,
                          O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (descriptor == -1 || secure_vault_descriptor(descriptor) == -1 ||
        fstat(descriptor, &status) == -1 ||
        status.st_size < VAULT_HEADER_CAP ||
        (uint64_t)status.st_size >
            VAULT_HEADER_CAP + GSH_HISTORY_SERIALIZED_CAP +
                crypto_aead_xchacha20poly1305_ietf_ABYTES ||
        read_all(descriptor, header, VAULT_HEADER_CAP) == -1) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return -1;
    }
    *cipher_length = (size_t)status.st_size - VAULT_HEADER_CAP;
    if (read_all(descriptor, state->cipher, *cipher_length) == -1) {
        (void)close(descriptor);
        return -1;
    }
    return close(descriptor);
}

static int validate_header(agent_state *state, const unsigned char *header,
                           size_t cipher_length, size_t *plain_length)
{
    if (plain_length == NULL || state == NULL) {
        return -1;
    }
    uint64_t encoded_length;

    if (memcmp(header, vault_magic, sizeof(vault_magic)) != 0 ||
        decode_u32(header + 8) != 1U) {
        errno = EPROTO;
        return -1;
    }
    state->opslimit = decode_u64(header + 12);
    state->memlimit = (size_t)decode_u64(header + 20);
    encoded_length = decode_u64(header + 68);
    if (state->opslimit < crypto_pwhash_OPSLIMIT_MIN ||
        state->opslimit > crypto_pwhash_OPSLIMIT_MODERATE ||
        state->memlimit < crypto_pwhash_MEMLIMIT_MIN ||
        state->memlimit > crypto_pwhash_MEMLIMIT_MODERATE ||
        encoded_length > GSH_HISTORY_SERIALIZED_CAP ||
        encoded_length + crypto_aead_xchacha20poly1305_ietf_ABYTES !=
            cipher_length) {
        errno = EPROTO;
        return -1;
    }
    (void)memcpy(state->salt, header + 28, crypto_pwhash_SALTBYTES);
    *plain_length = (size_t)encoded_length;
    return 0;
}

static int unlock_existing(agent_state *state, const char *passphrase,
                           size_t length)
{
    unsigned char header[VAULT_HEADER_CAP];
    unsigned char candidate[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
    unsigned long long decrypted = 0;
    size_t cipher_length;
    size_t plain_length;
    int result = -1;

    if (read_vault_file(state, header, &cipher_length) == -1 ||
        validate_header(state, header, cipher_length, &plain_length) == -1 ||
        derive_key(passphrase, length, state->salt, state->opslimit,
                   state->memlimit, candidate) == -1) {
        sodium_memzero(candidate, sizeof(candidate));
        return -1;
    }
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(
            state->plain, &decrypted, NULL, state->cipher, cipher_length,
            header, VAULT_HEADER_CAP, header + 44, candidate) == 0 &&
        decrypted == plain_length &&
        gsh_history_deserialize(state->store, state->plain,
                                plain_length) == 0) {
        (void)memcpy(state->key, candidate, sizeof(state->key));
        state->unlocked = true;
        result = 0;
    } else {
        errno = EACCES;
    }
    sodium_memzero(candidate, sizeof(candidate));
    return result;
}

static int create_vault(agent_state *state, const char *passphrase,
                        size_t length)
{
    if (state == NULL) {
        return -1;
    }
    randombytes_buf(state->salt, sizeof(state->salt));
    state->opslimit = crypto_pwhash_OPSLIMIT_INTERACTIVE;
    state->memlimit = crypto_pwhash_MEMLIMIT_INTERACTIVE;
    if (derive_key(passphrase, length, state->salt, state->opslimit,
                   state->memlimit, state->key) == -1) {
        return -1;
    }
    gsh_history_clear(state->store);
    state->unlocked = true;
    if (save_vault(state) == -1) {
        sodium_memzero(state->key, sizeof(state->key));
        state->unlocked = false;
        return -1;
    }
    return 0;
}

static uint64_t reminder_interval(const unsigned char *request,
                                  size_t length)
{
    if (request == NULL) {
        return 0U;
    }
    uint64_t minimum;
    uint64_t maximum;
    uint64_t seconds;
    uint32_t range;

    if (length < 16U) {
        return 0;
    }
    minimum = decode_u64(request);
    maximum = decode_u64(request + 8);
    if (minimum == 0 || maximum < minimum) {
        return 0;
    }
    seconds = (maximum - minimum) / 1000000000ULL;
    range = seconds >= UINT32_MAX ? UINT32_MAX : (uint32_t)seconds + 1U;
    return minimum + (uint64_t)randombytes_uniform(range) * 1000000000ULL;
}

static uint64_t agent_monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static uint64_t renew_reminder(agent_state *state,
                               const unsigned char *request, size_t length)
{
    if (request == NULL || state == NULL) {
        return 0U;
    }
    uint64_t interval = reminder_interval(request, length);
    uint64_t now = agent_monotonic_ns();

    if (interval == 0 || now == 0 || UINT64_MAX - now < interval) {
        state->reminder_deadline_ns = UINT64_MAX;
    } else {
        state->reminder_deadline_ns = now + interval;
    }
    return interval;
}

static uint64_t reminder_remaining(agent_state *state,
                                   const unsigned char *request,
                                   size_t length)
{
    if (state == NULL) return 0U;
    if (request == NULL) {
        return 0U;
    }
    uint64_t now;

    if (state->reminder_deadline_ns == 0) {
        return renew_reminder(state, request, length);
    }
    now = agent_monotonic_ns();
    if (now == 0 || now >= state->reminder_deadline_ns) {
        return 0;
    }
    return state->reminder_deadline_ns - now;
}

static int snapshot_response(agent_state *state, int descriptor,
                             const unsigned char *request,
                             size_t request_length)
{
    if (request == NULL || state == NULL) {
        return -1;
    }
    size_t serialized = gsh_history_serialize(
        state->store, state->plain + 8, GSH_HISTORY_SERIALIZED_CAP - 8U);

    if (serialized == 0) {
        return send_response(descriptor, errno, NULL, 0);
    }
    encode_u64(state->plain,
               reminder_remaining(state, request, request_length));
    return send_response(descriptor, GSH_HISTORY_STATUS_OK, state->plain,
                         serialized + 8U);
}

static int handle_unlock(agent_state *state, int descriptor,
                         const unsigned char *request, size_t length,
                         bool reset)
{
    if (state == NULL) return -1;
    if (request == NULL) {
        return -1;
    }
    const char *passphrase;
    size_t passphrase_length;
    int result;

    if (length <= 16U || length - 16U >= AGENT_SECRET_CAP) {
        return send_response(descriptor, EINVAL, NULL, 0);
    }
    passphrase = (const char *)request + 16;
    passphrase_length = length - 16U;
    if (reset || !state->vault_exists) {
        result = create_vault(state, passphrase, passphrase_length);
    } else {
        result = unlock_existing(state, passphrase, passphrase_length);
    }
    if (result == -1) {
        return send_response(descriptor, errno, NULL, 0);
    }
    (void)renew_reminder(state, request, 16U);
    return snapshot_response(state, descriptor, request, 16U);
}

static int handle_verify(agent_state *state, int descriptor,
                         const unsigned char *request, size_t length)
{
    if (state == NULL) return -1;
    unsigned char candidate[crypto_aead_xchacha20poly1305_ietf_KEYBYTES];
    int status = EACCES;

    if (state->unlocked && length > 16U && length - 16U < AGENT_SECRET_CAP &&
        derive_key((const char *)request + 16, length - 16U, state->salt,
                   state->opslimit, state->memlimit, candidate) == 0 &&
        sodium_memcmp(candidate, state->key, sizeof(candidate)) == 0) {
        status = GSH_HISTORY_STATUS_OK;
    }
    sodium_memzero(candidate, sizeof(candidate));
    if (status != GSH_HISTORY_STATUS_OK) {
        return send_response(descriptor, status, NULL, 0);
    }
    encode_u64(state->plain, renew_reminder(state, request, 16U));
    return send_response(descriptor, status, state->plain, 8U);
}

static void lock_agent(agent_state *state)
{
    if (state == NULL) {
        return;
    }
    sodium_memzero(state->key, sizeof(state->key));
    sodium_memzero(state->plain, GSH_HISTORY_SERIALIZED_CAP);
    gsh_history_clear(state->store);
    state->unlocked = false;
    state->reminder_deadline_ns = 0;
}

static int handle_message(agent_state *state, int descriptor,
                          const gsh_history_message *message,
                          unsigned char *request)
{
    if (message == NULL) return -1;
    if (request == NULL || state == NULL) {
        return -1;
    }
    if (message->type == GSH_HISTORY_MESSAGE_STATUS) {
        if (state->unlocked) {
            return snapshot_response(state, descriptor, request,
                                     message->length);
        }
        return send_response(descriptor,
                             state->vault_exists ? GSH_HISTORY_STATUS_LOCKED
                                                 : GSH_HISTORY_STATUS_NEW,
                             NULL, 0);
    }
    if (message->type == GSH_HISTORY_MESSAGE_UNLOCK) {
        return handle_unlock(state, descriptor, request, message->length,
                             false);
    }
    if (message->type == GSH_HISTORY_MESSAGE_RESET) {
        return handle_unlock(state, descriptor, request, message->length,
                             true);
    }
    if (message->type == GSH_HISTORY_MESSAGE_VERIFY) {
        return handle_verify(state, descriptor, request, message->length);
    }
    if (message->type == GSH_HISTORY_MESSAGE_LOCK) {
        lock_agent(state);
        return send_response(descriptor, GSH_HISTORY_STATUS_OK, NULL, 0);
    }
    if (message->type == GSH_HISTORY_MESSAGE_SHUTDOWN) {
        lock_agent(state);
        state->running = false;
        return send_response(descriptor, GSH_HISTORY_STATUS_OK, NULL, 0);
    }
    if (message->type == GSH_HISTORY_MESSAGE_ADD && state->unlocked &&
        message->length > 0 && message->length < GSH_HISTORY_ENTRY_CAP) {
        int added = gsh_history_add(state->store, (const char *)request,
                                    message->length, false);

        if (added >= 0 && save_vault(state) == 0) {
            return 0;
        }
    }
    errno = EPROTO;
    return -1;
}

static int service_client(agent_state *state, int descriptor,
                          unsigned char *request)
{
    if (state == NULL) {
        return -1;
    }
    gsh_history_message message;
    int result;

    if (read_all(descriptor, &message, sizeof(message)) == -1 ||
        message.magic != GSH_HISTORY_PROTOCOL_MAGIC ||
        message.version != GSH_HISTORY_PROTOCOL_VERSION ||
        message.length > AGENT_REQUEST_CAP ||
        read_all(descriptor, request, message.length) == -1) {
        sodium_memzero(request, AGENT_REQUEST_CAP);
        return -1;
    }
    result = handle_message(state, descriptor, &message, request);
    sodium_memzero(request, AGENT_REQUEST_CAP);
    return result;
}

static int make_listener(agent_state *state)
{
    if (state == NULL) return -1;
    struct sockaddr_un address;
    int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);

    if (descriptor == -1 || strlen(state->socket_path) >=
                                sizeof(address.sun_path)) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void)memcpy(address.sun_path, state->socket_path,
           strlen(state->socket_path) + 1U);
    (void)unlink(state->socket_path);
    if (bind(descriptor, (struct sockaddr *)&address, sizeof(address)) == -1 ||
        chmod(state->socket_path, S_IRUSR | S_IWUSR) == -1 ||
        listen(descriptor, AGENT_CLIENT_CAP) == -1) {
        (void)close(descriptor);
        (void)unlink(state->socket_path);
        return -1;
    }
    state->listener = descriptor;
    return 0;
}

static int acquire_agent_lock(agent_state *state)
{
    if (state == NULL) return -1;
    char lock_path[4096];
    struct flock lock;
    int descriptor;

    if (snprintf(lock_path, sizeof(lock_path), "%s.lock",
                 state->socket_path) >= (int)sizeof(lock_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    descriptor = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                      S_IRUSR | S_IWUSR);
    if (descriptor == -1 || secure_vault_descriptor(descriptor) == -1) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return -1;
    }
    (void)memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(descriptor, F_SETLK, &lock) == -1) {
        (void)close(descriptor);
        return -1;
    }
    state->lock_descriptor = descriptor;
    return 0;
}

static void accept_client(agent_state *state)
{
    if (state == NULL) {
        return;
    }
    int descriptor = accept(state->listener, NULL, NULL);
    int index;
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};

    if (descriptor < 0 || !peer_allowed(descriptor)) {
        if (descriptor >= 0) {
            (void)close(descriptor);
        }
        return;
    }
    if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1 ||
        setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) == -1 ||
        setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) == -1) {
        (void)close(descriptor);
        return;
    }
    for (index = 0; index < AGENT_CLIENT_CAP; index++) {
        if (state->clients[index] < 0) {
            state->clients[index] = descriptor;
            return;
        }
    }
    (void)close(descriptor);
}

static int run_agent(agent_state *state)
{
    if (state == NULL) {
        return -1;
    }
    unsigned char request[AGENT_REQUEST_CAP];
    struct pollfd descriptors[AGENT_CLIENT_CAP + 1U];
    int index;

    state->running = true;
    while (state->running) {
        descriptors[0].fd = state->listener;
        descriptors[0].events = POLLIN;
        for (index = 0; index < AGENT_CLIENT_CAP; index++) {
            descriptors[index + 1].fd = state->clients[index];
            descriptors[index + 1].events = POLLIN;
        }
        if (poll(descriptors, AGENT_CLIENT_CAP + 1U, -1) == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            accept_client(state);
        }
        for (index = 0; index < AGENT_CLIENT_CAP; index++) {
            if (state->clients[index] >= 0 &&
                (descriptors[index + 1].revents &
                 (POLLIN | POLLERR | POLLHUP | POLLNVAL)) != 0 &&
                service_client(state, state->clients[index], request) == -1) {
                (void)close(state->clients[index]);
                state->clients[index] = -1;
            }
        }
    }
    return 0;
}

static int initialize_agent(agent_state *state, agent_storage *storage,
                            const char *socket_path, const char *vault_path)
{
    if (state == NULL || storage == NULL) {
        return -1;
    }
    int index;

    (void)memset(state, 0, sizeof(*state));
    state->listener = -1;
    state->lock_descriptor = -1;
    for (index = 0; index < AGENT_CLIENT_CAP; index++) {
        state->clients[index] = -1;
    }
    if (strlen(socket_path) >= sizeof(state->socket_path) ||
        strlen(vault_path) >= sizeof(state->vault_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    (void)memcpy(state->socket_path, socket_path, strlen(socket_path) + 1U);
    (void)memcpy(state->vault_path, vault_path, strlen(vault_path) + 1U);
    state->vault_exists = access(vault_path, F_OK) == 0;
    (void)memset(storage, 0, sizeof(*storage));
    state->store = &storage->store;
    state->plain = storage->plain;
    state->cipher = storage->cipher;
    if (sodium_init() < 0) {
        errno = ENOMEM;
        return -1;
    }
    state->key_memory_locked = sodium_mlock(state->key,
                                            sizeof(state->key)) == 0;
    gsh_history_initialize(state->store);
    if (acquire_agent_lock(state) == -1) {
        return -1;
    }
    return make_listener(state);
}

static void close_agent(agent_state *state)
{
    if (state == NULL) {
        return;
    }
    int index;

    lock_agent(state);
    for (index = 0; index < AGENT_CLIENT_CAP; index++) {
        if (state->clients[index] >= 0) {
            (void)close(state->clients[index]);
        }
    }
    if (state->listener >= 0) {
        (void)close(state->listener);
    }
    if (state->lock_descriptor >= 0) {
        (void)close(state->lock_descriptor);
    }
    (void)unlink(state->socket_path);
    if (state->key_memory_locked) {
        (void)sodium_munlock(state->key, sizeof(state->key));
    }
    sodium_memzero(state->cipher, GSH_HISTORY_SERIALIZED_CAP +
                                      crypto_aead_xchacha20poly1305_ietf_ABYTES);
    state->store = NULL;
    state->plain = NULL;
    state->cipher = NULL;
}

/* ── The Agent Ignores Signals Through One POSIX Adapter ─────────
 * History persistence must not terminate on a stale client hangup or write.
 * CANON-EXCEPTION: POSIX-SIGNAL-DISPOSITION permits only SIG_IGN assignments
 * in this adapter; no repository callback is installed or retained.
 * sigaction failures abort startup instead of leaving a partial disposition.
 * Agent lifecycle tests cover disconnects and clean encrypted persistence.
 * ─────────────────────────────────────────────────────────────── */
static int ignore_agent_signal(int signo)
{
    struct sigaction action;

    (void)memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_IGN;
    (void)sigemptyset(&action.sa_mask);
    return sigaction(signo, &action, NULL);
}

/* ── Agent Arguments Become Bounded Initialization Inputs ───────
 * The operating system supplies the history path and session token via argv.
 * CANON-EXCEPTION: C-PROCESS-ABI is confined to this main entry point.
 * argc is validated before either element is read, and initialize_agent copies
 * both values into fixed-capacity storage rather than retaining the pointers.
 * Restart and hostile-initialization tests exercise this boundary contract.
 * ─────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    static agent_state state;
    static agent_storage storage;
    struct rlimit core_limit = {0, 0};
    int status;

    (void)setrlimit(RLIMIT_CORE, &core_limit);
    if (argc != 3 ||
        initialize_agent(&state, &storage, argv[1], argv[2]) == -1) {
        return 1;
    }
    if (ignore_agent_signal(SIGHUP) == -1 ||
        ignore_agent_signal(SIGPIPE) == -1) {
        close_agent(&state);
        return 1;
    }
    status = run_agent(&state);
    close_agent(&state);
    return status == 0 ? 0 : 1;
}
