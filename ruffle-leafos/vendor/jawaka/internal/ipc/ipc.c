/* strdup() is hidden by glibc under a bare -std=c11. Without this it was
 * implicitly declared, so gcc assumed `int` and truncated the returned
 * pointer to 32 bits -- the socket path would be a corrupt pointer on Linux
 * and MLP1. Must precede every #include, including the paired header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/ipc/ipc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>

struct jw_ipc_server {
    int fd;
    char *socket_path;
};

struct jw_ipc_client {
    int fd;
};

static void jw__set_cloexec(int fd) {
    int flags;
    if (fd < 0) {
        return;
    }

    flags = fcntl(fd, F_GETFD);
    if (flags >= 0) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

static void jw__set_io_timeout_ms(int fd, int timeout_ms) {
    struct timeval tv;
    if (fd < 0) {
        return;
    }

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int jw__write_all(int fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t written = write(fd, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        ptr += (size_t)written;
        remaining -= (size_t)written;
    }

    return 0;
}

static int jw__read_all(int fd, void *buf, size_t len) {
    char *ptr = (char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t nread = read(fd, ptr, remaining);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nread == 0) {
            return -1;
        }
        ptr += (size_t)nread;
        remaining -= (size_t)nread;
    }

    return 0;
}

static int jw__make_unix_addr(const char *socket_path, struct sockaddr_un *addr, socklen_t *addr_len) {
    if (!socket_path || !addr || !addr_len) {
        return -1;
    }

    if (strlen(socket_path) >= sizeof(addr->sun_path)) {
        return -1;
    }

    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    strcpy(addr->sun_path, socket_path);
    *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(addr->sun_path) + 1u);
    return 0;
}

int jw_ipc_server_listen(const char *socket_path, jw_ipc_server **out) {
    if (!socket_path || !out) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    jw__set_cloexec(fd);

    struct sockaddr_un addr;
    socklen_t addr_len = 0;
    if (jw__make_unix_addr(socket_path, &addr, &addr_len) != 0) {
        close(fd);
        return -1;
    }

    unlink(socket_path);

    if (bind(fd, (struct sockaddr *)&addr, addr_len) != 0) {
        close(fd);
        return -1;
    }

    if (chmod(socket_path, 0600) != 0) {
        /* Best effort: a wider mode is undesirable but not fatal. */
    }

    if (listen(fd, 8) != 0) {
        close(fd);
        unlink(socket_path);
        return -1;
    }

    jw_ipc_server *server = (jw_ipc_server *)calloc(1, sizeof(*server));
    if (!server) {
        close(fd);
        unlink(socket_path);
        return -1;
    }

    server->fd = fd;
    server->socket_path = strdup(socket_path);
    if (!server->socket_path) {
        close(fd);
        unlink(socket_path);
        free(server);
        return -1;
    }

    *out = server;
    return 0;
}

int jw_ipc_server_accept(jw_ipc_server *server, jw_ipc_client **out_client, int timeout_ms) {
    if (!server || !out_client) {
        return -1;
    }

    if (timeout_ms >= 0) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server->fd, &read_fds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        int ready = select(server->fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready == 0) {
            return 1;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                return 1;
            }
            return -1;
        }
    }

    int client_fd = accept(server->fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno == EINTR) {
            return 1;
        }
        return -1;
    }
    jw__set_cloexec(client_fd);
    jw__set_io_timeout_ms(client_fd, 5000);

    jw_ipc_client *client = (jw_ipc_client *)calloc(1, sizeof(*client));
    if (!client) {
        close(client_fd);
        return -1;
    }

    client->fd = client_fd;
    *out_client = client;
    return 0;
}

int jw_ipc_server_fd(const jw_ipc_server *server) {
    return server ? server->fd : -1;
}

void jw_ipc_server_close(jw_ipc_server *server) {
    if (!server) {
        return;
    }

    if (server->fd >= 0) {
        close(server->fd);
    }
    if (server->socket_path) {
        unlink(server->socket_path);
        free(server->socket_path);
    }
    free(server);
}

static int jw__ipc_client_connect_timeout(const char *socket_path,
                                          jw_ipc_client **out,
                                          int timeout_ms) {
    if (!socket_path || !out) {
        return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    jw__set_cloexec(fd);

    struct sockaddr_un addr;
    socklen_t addr_len = 0;
    if (jw__make_unix_addr(socket_path, &addr, &addr_len) != 0) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, addr_len) != 0) {
        close(fd);
        return -1;
    }

    /* Bound every request so a wedged peer cannot block the caller forever.
       General callers use 30s; render-thread status reads choose a much
       shorter deadline through jw_ipc_request_timeout(). */
    jw__set_io_timeout_ms(fd, timeout_ms);

    jw_ipc_client *client = (jw_ipc_client *)calloc(1, sizeof(*client));
    if (!client) {
        close(fd);
        return -1;
    }

    client->fd = fd;
    *out = client;
    return 0;
}

int jw_ipc_client_connect(const char *socket_path, jw_ipc_client **out) {
    return jw__ipc_client_connect_timeout(socket_path, out, 30000);
}

/* A peer that closed before reading is routine rather than a failure: the
   notification clients send and close without waiting (see ipc__notify).
   errno is whatever the failing write left, since jw__write_all does nothing
   between the failure and its return. */
static int jw__send_failed(void) {
    return (errno == EPIPE || errno == ECONNRESET) ? JW_IPC_PEER_GONE : -1;
}

int jw_ipc_client_send(jw_ipc_client *client, const char *json, size_t len) {
    if (!client || !json || len > JW_IPC_MAX_FRAME) {
        return -1;
    }

    uint32_t frame_len = htonl((uint32_t)len);
    if (jw__write_all(client->fd, &frame_len, sizeof(frame_len)) != 0) {
        return jw__send_failed();
    }
    if (jw__write_all(client->fd, json, len) != 0) {
        return jw__send_failed();
    }
    return 0;
}

int jw_ipc_client_recv(jw_ipc_client *client, char **out_json, size_t *out_len) {
    if (!client || !out_json || !out_len) {
        return -1;
    }

    uint32_t frame_len = 0;
    if (jw__read_all(client->fd, &frame_len, sizeof(frame_len)) != 0) {
        return -1;
    }

    uint32_t payload_len = ntohl(frame_len);
    if (payload_len > JW_IPC_MAX_FRAME) {
        return -1;
    }

    char *json = (char *)malloc((size_t)payload_len + 1u);
    if (!json) {
        return -1;
    }

    if (jw__read_all(client->fd, json, payload_len) != 0) {
        free(json);
        return -1;
    }

    json[payload_len] = '\0';
    *out_json = json;
    *out_len = (size_t)payload_len;
    return 0;
}

int jw_ipc_client_peer_pid(jw_ipc_client *client, pid_t *out_pid) {
    if (!client || !out_pid) return -1;
#if defined(__APPLE__) && defined(LOCAL_PEERPID)
    pid_t pid = 0;
    socklen_t len = sizeof(pid);
    if (getsockopt(client->fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) == 0 && pid > 0) {
        *out_pid = pid;
        return 0;
    }
#elif defined(SO_PEERCRED)
    struct {
        pid_t pid;
        uid_t uid;
        gid_t gid;
    } credentials;
    socklen_t len = sizeof(credentials);
    if (getsockopt(client->fd, SOL_SOCKET, SO_PEERCRED, &credentials, &len) == 0 &&
        credentials.pid > 0) {
        *out_pid = credentials.pid;
        return 0;
    }
#endif
    return -1;
}

int jw_ipc_client_fd(const jw_ipc_client *client) {
    return client ? client->fd : -1;
}

int jw_ipc_client_set_nonblocking(jw_ipc_client *client, bool nonblocking,
                                  int blocking_timeout_ms) {
    if (!client || client->fd < 0) {
        return -1;
    }
    int flags = fcntl(client->fd, F_GETFL);
    if (flags < 0) {
        return -1;
    }
    int wanted = nonblocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (wanted != flags && fcntl(client->fd, F_SETFL, wanted) != 0) {
        return -1;
    }
    if (!nonblocking) {
        if (blocking_timeout_ms < 0) {
            return -1;
        }
        jw__set_io_timeout_ms(client->fd, blocking_timeout_ms);
    }
    return 0;
}

void jw_ipc_client_close(jw_ipc_client *client) {
    if (!client) {
        return;
    }
    if (client->fd >= 0) {
        close(client->fd);
    }
    free(client);
}

int jw_ipc_request_timeout(const char *socket_path, const char *json, size_t len,
                           char **out_json, size_t *out_len, int timeout_ms) {
    jw_ipc_client *client = NULL;
    if (timeout_ms <= 0 ||
        jw__ipc_client_connect_timeout(socket_path, &client, timeout_ms) != 0) {
        return -1;
    }

    if (jw_ipc_client_send(client, json, len) != 0) {
        jw_ipc_client_close(client);
        return -1;
    }

    int rc = jw_ipc_client_recv(client, out_json, out_len);
    jw_ipc_client_close(client);
    return rc;
}

int jw_ipc_request(const char *socket_path, const char *json, size_t len,
                   char **out_json, size_t *out_len) {
    return jw_ipc_request_timeout(socket_path, json, len, out_json, out_len,
                                  30000);
}
