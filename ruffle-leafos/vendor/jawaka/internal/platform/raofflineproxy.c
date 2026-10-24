#define _POSIX_C_SOURCE 200809L

#include "internal/platform/raofflineproxy.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

/* The fixed body the patched Leaf service answers once requests can actually
 * complete (see proxy_service.leaf_health_response). */
#define JW_ROP_HEALTH_SERVICE_FIELD "\"service\":\"" JW_ROP_SERVICE_ID "\""
#define JW_ROP_HEALTH_PROTOCOL_FIELD "\"protocol\":\"leaf-health-1\""
#define JW_ROP_HEALTH_READY_FIELD "\"ready\":true"

static long long jw__rop_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000ll + ts.tv_nsec / 1000000ll;
}

static int jw__rop_wait_fd(int fd, short events, long long deadline_ms) {
    for (;;) {
        long long now = jw__rop_now_ms();
        int timeout = (int)(deadline_ms - now);
        if (timeout <= 0) {
            return 0;
        }
        struct pollfd pfd = {
            .fd = fd,
            .events = events,
            .revents = 0,
        };
        int rc = poll(&pfd, 1, timeout);
        if (rc > 0) {
            return 1;
        }
        if (rc == 0) {
            return 0;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

/* Portable memmem(3) (a GNU extension, absent on macOS). */
static bool jw__rop_buf_contains(const char *haystack, size_t haystack_len,
                                 const char *needle, size_t needle_len) {
    if (!haystack || !needle || needle_len == 0) {
        return false;
    }
    if (haystack_len < needle_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool jw__rop_body_is_ready(const char *buf, size_t len) {
    /* Fixed-shape response: substring membership is sufficient because the
     * three fields can only appear together in the health document, and we
     * never parse (or trust) any host or port from the reply. */
    bool status_ok =
        jw__rop_buf_contains(buf, len, "HTTP/1.0 200 ", 13) ||
        jw__rop_buf_contains(buf, len, "HTTP/1.1 200 ", 13);
    return status_ok &&
           jw__rop_buf_contains(buf, len, JW_ROP_HEALTH_SERVICE_FIELD,
                                strlen(JW_ROP_HEALTH_SERVICE_FIELD)) &&
           jw__rop_buf_contains(buf, len, JW_ROP_HEALTH_PROTOCOL_FIELD,
                                strlen(JW_ROP_HEALTH_PROTOCOL_FIELD)) &&
           jw__rop_buf_contains(buf, len, JW_ROP_HEALTH_READY_FIELD,
                                strlen(JW_ROP_HEALTH_READY_FIELD));
}

bool jw_raofflineproxy_health_ready(const char *host, uint16_t port,
                                    int timeout_ms) {
    if (!host || !host[0] || timeout_ms <= 0) {
        return false;
    }
    long long deadline_ms = jw__rop_now_ms() + timeout_ms;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        return false;
    }

    /* SOCK_NONBLOCK/SOCK_CLOEXEC at socket() time are Linux-only; use fcntl
     * for portability (macOS builds the same TU). */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            close(fd);
            return false;
        }
        int fdflags = fcntl(fd, F_GETFD, 0);
        if (fdflags >= 0) {
            (void)fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC);
        }
#if defined(SO_NOSIGPIPE)
        /* BSD/macOS: suppress SIGPIPE for every write on this socket, not just
         * the ones that remember a flag. A probe that kills jawakad because
         * the proxy closed early would be a spectacular way to fail a
         * readiness check. */
        int nosigpipe = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe,
                         sizeof(nosigpipe));
#endif
    }

    bool ready = false;
    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        goto out;
    }
    if (rc != 0 && jw__rop_wait_fd(fd, POLLOUT, deadline_ms) != 1) {
        goto out;
    }
    {
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 ||
            so_error != 0) {
            goto out;
        }
    }

    char request[160];
    int request_len = snprintf(request, sizeof(request),
                               "GET " JW_ROP_HEALTH_PATH " HTTP/1.0\r\n"
                               "Host: %s\r\n\r\n", host);
    if (request_len <= 0 || request_len >= (int)sizeof(request)) {
        goto out;
    }
    {
        size_t sent = 0;
        while (sent < (size_t)request_len) {
            if (jw__rop_wait_fd(fd, POLLOUT, deadline_ms) != 1) {
                goto out;
            }
            /* Guarded the way internal/ipc/ipc_stream.c already guards it.
             * Both platforms we build for do define MSG_NOSIGNAL -- this is
             * not fixing a broken build -- but one TU assuming it while its
             * neighbour does not is the kind of difference that only shows up
             * on the toolchain nobody tested. */
#if defined(MSG_NOSIGNAL)
            ssize_t n = send(fd, request + sent, (size_t)request_len - sent,
                             MSG_NOSIGNAL);
#else
            ssize_t n = send(fd, request + sent, (size_t)request_len - sent, 0);
#endif
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                goto out;
            }
            sent += (size_t)n;
        }
    }
    {
        /* The health answer is a tiny JSON document; a generous cap bounds a
         * misbehaving listener without parsing chunking. */
        char buf[4096];
        size_t used = 0;
        for (;;) {
            if (jw__rop_wait_fd(fd, POLLIN, deadline_ms) != 1) {
                goto out;
            }
            ssize_t n = recv(fd, buf + used, sizeof(buf) - used, 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                goto out;
            }
            if (n == 0) {
                break;
            }
            used += (size_t)n;
            if (jw__rop_body_is_ready(buf, used)) {
                ready = true;
                goto out;
            }
            if (used >= sizeof(buf)) {
                goto out;
            }
        }
    }

out:
    close(fd);
    return ready;
}

bool jw_raofflineproxy_entry_live(const jw_svc_supervised *entry) {
    return entry && entry->pak_present && entry->manifest_valid &&
           entry->pgid > 0 &&
           (entry->state == JW_SVC_STATE_RUNNING ||
            entry->state == JW_SVC_STATE_STARTING);
}
