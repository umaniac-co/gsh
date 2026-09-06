#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 202405L
#endif

#include "llm_repl.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── Generated Commands Return Through The Owning REPL ──────────
 * A script executed by the provider worker could report a successful cd
 * while leaving the user's shell unchanged. This channel carries proposals
 * to the reactor and receipts back only after ordinary command cells settle.
 * Fixed records and one in-flight command bound storage; reactor transfers
 * consume at most 8 KiB per call and never wait for a provider or command.
 * ─────────────────────────────────────────────────────────────── */
int gsh_llm_repl_open(gsh_llm_repl_channel *channel, int *peer)
{
    int sockets[2];

    if (channel == NULL || peer == NULL || channel->fd >= 0) return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == -1) return -1;
    if (fcntl(sockets[0], F_SETFD, FD_CLOEXEC) == -1 ||
        fcntl(sockets[1], F_SETFD, FD_CLOEXEC) == -1 ||
        fcntl(sockets[0], F_SETFL, O_NONBLOCK) == -1) {
        (void)close(sockets[0]);
        (void)close(sockets[1]);
        return -1;
    }
    (void)memset(channel, 0, sizeof(*channel));
    channel->fd = sockets[0];
    *peer = sockets[1];
    return 0;
}

void gsh_llm_repl_close(gsh_llm_repl_channel *channel)
{
    if (channel == NULL) return;
    if (channel->fd >= 0) (void)close(channel->fd);
    channel->fd = -1;
    channel->received = 0U;
    channel->sent = 0U;
    channel->replying = false;
}

int gsh_llm_repl_receive(gsh_llm_repl_channel *channel)
{
    ssize_t count;

    if (channel == NULL || channel->fd < 0 ||
        channel->received > sizeof(channel->request)) return -1;
    if (channel->replying || channel->received == sizeof(channel->request))
        return 0;
    count = read(channel->fd, (char *)&channel->request + channel->received,
                 sizeof(channel->request) - channel->received);
    if (count == -1 && (errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK)) return 0;
    if (count <= 0) return -1;
    channel->received += (size_t)count;
    if (channel->received != sizeof(channel->request)) return 0;
    if (channel->request.version != GSH_LLM_REPL_VERSION ||
        channel->request.activity > GSH_LLM_CONFIRMATION ||
        (channel->request.length != 0U &&
         channel->request.activity != GSH_LLM_IDLE) ||
        channel->request.length >= sizeof(channel->request.script) ||
        channel->request.script[channel->request.length] != '\0' ||
        memchr(channel->request.script, '\0', channel->request.length) != NULL)
        return -1;
    return 1;
}

int gsh_llm_repl_flush(gsh_llm_repl_channel *channel)
{
    size_t remaining;
    ssize_t count;

    if (channel == NULL || channel->fd < 0 ||
        channel->sent >= sizeof(channel->result)) return -1;
    if (!channel->replying) return 0;
    remaining = sizeof(channel->result) - channel->sent;
    if (remaining > 8192U) remaining = 8192U;
    count = write(channel->fd, (char *)&channel->result + channel->sent,
                  remaining);
    if (count == -1 && (errno == EINTR || errno == EAGAIN ||
                        errno == EWOULDBLOCK)) return 0;
    if (count <= 0) return -1;
    channel->sent += (size_t)count;
    if (channel->sent != sizeof(channel->result)) return 0;
    channel->sent = 0U;
    channel->received = 0U;
    channel->replying = false;
    return 1;
}

static int transfer_record(int descriptor, void *record, size_t length,
                           bool sending)
{
    size_t offset = 0U;
    size_t attempt;

    if (descriptor < 0 || record == NULL || length == 0U) return -1;
    for (attempt = 0U; attempt < length + 16U && offset < length; attempt++) {
        ssize_t count = sending
            ? write(descriptor, (char *)record + offset, length - offset)
            : read(descriptor, (char *)record + offset, length - offset);

        if (count > 0) offset += (size_t)count;
        else if (!(count == -1 && errno == EINTR)) return -1;
    }
    return offset == length ? 0 : -1;
}

/* ── Activity Travels Outside Command Output ─────────────────────
 * Provider silence cannot distinguish startup, inference, and a question.
 * Empty request records report these phases over the existing private
 * channel; they carry no script and require no execution receipt. The
 * compositor owns animation, keeping UI frames out of captured output,
 * conversation context, and history even when the provider streams text.
 * ─────────────────────────────────────────────────────────────── */
int gsh_llm_repl_report_activity(int descriptor, gsh_llm_activity activity)
{
    gsh_llm_repl_request request = {.version = GSH_LLM_REPL_VERSION};

    if (activity < GSH_LLM_IDLE || activity > GSH_LLM_CONFIRMATION)
        return -1;
    if (descriptor < 0) return 0;
    request.activity = (uint32_t)activity;
    return transfer_record(descriptor, &request, sizeof(request), true);
}

int gsh_llm_repl_call(int descriptor, const char *script,
                      gsh_llm_repl_result *result)
{
    gsh_llm_repl_request request = {.version = GSH_LLM_REPL_VERSION};
    size_t length;

    if (descriptor < 0 || script == NULL || result == NULL) return -1;
    length = strlen(script);
    if (length == 0U || length >= sizeof(request.script)) return -1;
    request.length = (uint32_t)length;
    (void)memcpy(request.script, script, length + 1U);
    if (transfer_record(descriptor, &request, sizeof(request), true) == -1 ||
        transfer_record(descriptor, result, sizeof(*result), false) == -1 ||
        result->version != GSH_LLM_REPL_VERSION ||
        memchr(result->directory, '\0', sizeof(result->directory)) == NULL ||
        memchr(result->output, '\0', sizeof(result->output)) == NULL)
        return -1;
    return 0;
}
