#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/ipc/ipc_stream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    unsigned char *bytes;
    size_t len;
    size_t sent;
} jw__ipc_queued_frame;

struct jw_ipc_stream {
    jw_ipc_client *client;
    unsigned char prefix[sizeof(uint32_t)];
    size_t prefix_used;
    char *payload;
    size_t payload_len;
    size_t payload_used;
    long long partial_deadline_ms;

    jw__ipc_queued_frame queue[JW_IPC_STREAM_QUEUE_MAX];
    int queue_head;
    int queue_count;
};

static void jw__ipc_stream_reset_input(jw_ipc_stream *stream) {
    memset(stream->prefix, 0, sizeof(stream->prefix));
    stream->prefix_used = 0;
    stream->payload = NULL;
    stream->payload_len = 0;
    stream->payload_used = 0;
    stream->partial_deadline_ms = 0;
}

int jw_ipc_stream_create(jw_ipc_client *client, jw_ipc_stream **out) {
    if (!client || !out || jw_ipc_client_fd(client) < 0) {
        return -1;
    }
    if (jw_ipc_client_set_nonblocking(client, true, 0) != 0) {
        return -1;
    }
    jw_ipc_stream *stream = calloc(1, sizeof(*stream));
    if (!stream) {
        (void)jw_ipc_client_set_nonblocking(client, false, 5000);
        return -1;
    }
    stream->client = client;
    *out = stream;
    return 0;
}

void jw_ipc_stream_destroy(jw_ipc_stream *stream) {
    if (!stream) {
        return;
    }
    free(stream->payload);
    for (int i = 0; i < JW_IPC_STREAM_QUEUE_MAX; i++) {
        free(stream->queue[i].bytes);
    }
    jw_ipc_client_close(stream->client);
    free(stream);
}

int jw_ipc_stream_fd(const jw_ipc_stream *stream) {
    return stream ? jw_ipc_client_fd(stream->client) : -1;
}

int jw_ipc_stream_peer_pid(jw_ipc_stream *stream, pid_t *out_pid) {
    return stream ? jw_ipc_client_peer_pid(stream->client, out_pid) : -1;
}

bool jw_ipc_stream_wants_write(const jw_ipc_stream *stream) {
    return stream && stream->queue_count > 0;
}

int jw_ipc_stream_queued(const jw_ipc_stream *stream) {
    return stream ? stream->queue_count : 0;
}

static int jw__ipc_read(int fd, void *buffer, size_t len, size_t *used) {
    for (;;) {
        ssize_t n = read(fd, (unsigned char *)buffer + *used, len - *used);
        if (n > 0) {
            *used += (size_t)n;
            return 1;
        }
        if (n == 0) {
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        return -1;
    }
}

int jw_ipc_stream_receive(jw_ipc_stream *stream, long long now_ms,
                          char **out_payload, size_t *out_len) {
    if (!stream || !out_payload || !out_len || now_ms < 0) {
        return -1;
    }
    *out_payload = NULL;
    *out_len = 0;
    if (stream->partial_deadline_ms > 0 &&
        now_ms >= stream->partial_deadline_ms) {
        return -3;
    }

    int fd = jw_ipc_stream_fd(stream);
    if (stream->prefix_used < sizeof(stream->prefix)) {
        size_t before = stream->prefix_used;
        int rc = jw__ipc_read(fd, stream->prefix, sizeof(stream->prefix),
                              &stream->prefix_used);
        if (rc < 0) {
            return -1;
        }
        if (stream->prefix_used > before && stream->partial_deadline_ms == 0) {
            stream->partial_deadline_ms =
                now_ms + JW_IPC_STREAM_PARTIAL_TIMEOUT_MS;
        }
        if (stream->prefix_used < sizeof(stream->prefix)) {
            return 0;
        }

        uint32_t network_len = 0;
        memcpy(&network_len, stream->prefix, sizeof(network_len));
        stream->payload_len = (size_t)ntohl(network_len);
        if (stream->payload_len > JW_IPC_MAX_FRAME) {
            return -2;
        }
        stream->payload = malloc(stream->payload_len + 1u);
        if (!stream->payload) {
            return -1;
        }
        if (stream->payload_len == 0) {
            stream->payload[0] = '\0';
        }
    }

    if (stream->payload_used < stream->payload_len) {
        int rc = jw__ipc_read(fd, stream->payload, stream->payload_len,
                              &stream->payload_used);
        if (rc < 0) {
            return -1;
        }
        if (stream->payload_used < stream->payload_len) {
            return 0;
        }
    }

    stream->payload[stream->payload_len] = '\0';
    *out_payload = stream->payload;
    *out_len = stream->payload_len;
    stream->payload = NULL;
    jw__ipc_stream_reset_input(stream);
    return 1;
}

int jw_ipc_stream_queue(jw_ipc_stream *stream, const char *payload, size_t len) {
    if (!stream || (!payload && len > 0) || len > JW_IPC_MAX_FRAME ||
        stream->queue_count >= JW_IPC_STREAM_QUEUE_MAX) {
        return -1;
    }
    if (len > SIZE_MAX - sizeof(uint32_t)) {
        return -1;
    }
    size_t framed_len = sizeof(uint32_t) + len;
    unsigned char *framed = malloc(framed_len);
    if (!framed) {
        return -1;
    }
    uint32_t network_len = htonl((uint32_t)len);
    memcpy(framed, &network_len, sizeof(network_len));
    if (len > 0) {
        memcpy(framed + sizeof(network_len), payload, len);
    }

    int slot = (stream->queue_head + stream->queue_count) %
               JW_IPC_STREAM_QUEUE_MAX;
    stream->queue[slot].bytes = framed;
    stream->queue[slot].len = framed_len;
    stream->queue[slot].sent = 0;
    stream->queue_count++;
    return 0;
}

static ssize_t jw__ipc_write_no_signal(int fd, const void *buffer, size_t len) {
#if defined(MSG_NOSIGNAL)
    return send(fd, buffer, len, MSG_NOSIGNAL);
#else
    return write(fd, buffer, len);
#endif
}

int jw_ipc_stream_flush(jw_ipc_stream *stream) {
    if (!stream) {
        return -1;
    }
    int fd = jw_ipc_stream_fd(stream);
    while (stream->queue_count > 0) {
        jw__ipc_queued_frame *frame = &stream->queue[stream->queue_head];
        ssize_t n = jw__ipc_write_no_signal(fd, frame->bytes + frame->sent,
                                            frame->len - frame->sent);
        if (n > 0) {
            frame->sent += (size_t)n;
            if (frame->sent < frame->len) {
                continue;
            }
            free(frame->bytes);
            memset(frame, 0, sizeof(*frame));
            stream->queue_head =
                (stream->queue_head + 1) % JW_IPC_STREAM_QUEUE_MAX;
            stream->queue_count--;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        }
        return -1;
    }
    return 0;
}

jw_ipc_client *jw_ipc_stream_detach_blocking(jw_ipc_stream *stream) {
    if (!stream || stream->queue_count != 0 || stream->payload ||
        stream->prefix_used != 0) {
        return NULL;
    }
    jw_ipc_client *client = stream->client;
    if (jw_ipc_client_set_nonblocking(client, false, 5000) != 0) {
        return NULL;
    }
    stream->client = NULL;
    free(stream);
    return client;
}
