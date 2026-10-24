#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/ipc/ipc.h"
#include "internal/ipc/ipc_stream.h"

#include <arpa/inet.h>
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char dir[128];
    char path[160];
    jw_ipc_server *server;
    jw_ipc_client *sender;
    jw_ipc_stream *receiver;
} fixture;

static void fixture_open(fixture *f) {
    memset(f, 0, sizeof(*f));
    snprintf(f->dir, sizeof(f->dir), "/tmp/jw-ipc-stream-XXXXXX");
    assert(mkdtemp(f->dir));
    int n = snprintf(f->path, sizeof(f->path), "%s/socket", f->dir);
    assert(n > 0 && (size_t)n < sizeof(f->path));
    assert(jw_ipc_server_listen(f->path, &f->server) == 0);
    assert(jw_ipc_client_connect(f->path, &f->sender) == 0);
    jw_ipc_client *accepted = NULL;
    assert(jw_ipc_server_accept(f->server, &accepted, 1000) == 0);
    assert(jw_ipc_stream_create(accepted, &f->receiver) == 0);
}

static void fixture_close(fixture *f) {
    jw_ipc_stream_destroy(f->receiver);
    jw_ipc_client_close(f->sender);
    jw_ipc_server_close(f->server);
    assert(rmdir(f->dir) == 0);
}

static void write_all(int fd, const void *bytes, size_t len) {
    const unsigned char *p = bytes;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        assert(n > 0);
        p += (size_t)n;
        len -= (size_t)n;
    }
}

static void test_fragmented_frame(void) {
    fixture f;
    fixture_open(&f);
    uint32_t prefix = htonl(5);
    int fd = jw_ipc_client_fd(f.sender);
    write_all(fd, &prefix, 2);

    char *payload = NULL;
    size_t len = 0;
    assert(jw_ipc_stream_receive(f.receiver, 100, &payload, &len) == 0);
    write_all(fd, (unsigned char *)&prefix + 2, 2);
    write_all(fd, "hello", 5);
    assert(jw_ipc_stream_receive(f.receiver, 101, &payload, &len) == 1);
    assert(len == 5 && memcmp(payload, "hello", 5) == 0);
    free(payload);
    fixture_close(&f);
}

static void test_partial_timeout(void) {
    fixture f;
    fixture_open(&f);
    uint32_t prefix = htonl(32);
    write_all(jw_ipc_client_fd(f.sender), &prefix, 1);
    char *payload = NULL;
    size_t len = 0;
    assert(jw_ipc_stream_receive(f.receiver, 500, &payload, &len) == 0);
    assert(jw_ipc_stream_receive(f.receiver, 1499, &payload, &len) == 0);
    assert(jw_ipc_stream_receive(f.receiver, 1500, &payload, &len) == -3);
    fixture_close(&f);
}

static void test_invalid_length(void) {
    fixture f;
    fixture_open(&f);
    uint32_t prefix = htonl((uint32_t)JW_IPC_MAX_FRAME + 1u);
    write_all(jw_ipc_client_fd(f.sender), &prefix, sizeof(prefix));
    char *payload = NULL;
    size_t len = 0;
    assert(jw_ipc_stream_receive(f.receiver, 0, &payload, &len) == -2);
    fixture_close(&f);
}

static void test_bounded_outbound_queue(void) {
    fixture f;
    fixture_open(&f);
    for (int i = 0; i < JW_IPC_STREAM_QUEUE_MAX; i++) {
        char value[16];
        snprintf(value, sizeof(value), "message-%02d", i);
        assert(jw_ipc_stream_queue(f.receiver, value, strlen(value)) == 0);
    }
    assert(jw_ipc_stream_queued(f.receiver) == JW_IPC_STREAM_QUEUE_MAX);
    assert(jw_ipc_stream_queue(f.receiver, "overflow", 8) == -1);
    assert(jw_ipc_stream_flush(f.receiver) == 0);
    assert(jw_ipc_stream_queued(f.receiver) == 0);

    for (int i = 0; i < JW_IPC_STREAM_QUEUE_MAX; i++) {
        char *payload = NULL;
        size_t len = 0;
        assert(jw_ipc_client_recv(f.sender, &payload, &len) == 0);
        char expected[16];
        snprintf(expected, sizeof(expected), "message-%02d", i);
        assert(len == strlen(expected));
        assert(memcmp(payload, expected, len) == 0);
        free(payload);
    }
    fixture_close(&f);
}

static void test_detach_restores_one_shot_client(void) {
    fixture f;
    fixture_open(&f);
    assert(jw_ipc_client_send(f.sender, "request", 7) == 0);
    char *payload = NULL;
    size_t len = 0;
    assert(jw_ipc_stream_receive(f.receiver, 0, &payload, &len) == 1);
    assert(len == 7 && memcmp(payload, "request", 7) == 0);
    free(payload);

    jw_ipc_client *detached = jw_ipc_stream_detach_blocking(f.receiver);
    assert(detached);
    f.receiver = NULL;
    assert(jw_ipc_client_send(detached, "reply", 5) == 0);
    assert(jw_ipc_client_recv(f.sender, &payload, &len) == 0);
    assert(len == 5 && memcmp(payload, "reply", 5) == 0);
    free(payload);
    jw_ipc_client_close(detached);
    fixture_close(&f);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_fragmented_frame();
    test_partial_timeout();
    test_invalid_length();
    test_bounded_outbound_queue();
    test_detach_restores_one_shot_client();
    puts("PASS ipc-stream-test");
    return 0;
}
