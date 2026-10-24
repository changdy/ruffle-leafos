#ifndef JW_IPC_H
#define JW_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define JW_IPC_MAX_FRAME (16u * 1024u * 1024u)
/* CTL-1/LIFE-1 semantic payload ceiling inside the shared transport frame. */
#define JW_IPC_SERVICE_MAX_FRAME (64u * 1024u)

/* jw_ipc_client_send outcome: the peer closed before the reply could be
   delivered. NOT an error. Notification-style clients send and close without
   waiting for an answer by design (see ipc__notify in ipc_client.c), so the
   request was received and handled; only an answer nobody wanted went
   undelivered. Distinguished from -1 so callers can stop reporting the normal
   case as a failure. */
#define JW_IPC_PEER_GONE 1

typedef struct jw_ipc_server jw_ipc_server;
typedef struct jw_ipc_client jw_ipc_client;

int  jw_ipc_server_listen(const char *socket_path, jw_ipc_server **out);
/* Returns: 0 = client accepted (*out_client set)
 *          1 = no client yet (timeout or EINTR — caller should retry)
 *         -1 = unrecoverable error */
int  jw_ipc_server_accept(jw_ipc_server *server, jw_ipc_client **out_client, int timeout_ms);
/* Event-driven daemon integrations may poll the listening fd directly, then
 * call jw_ipc_server_accept(..., 0) when it is readable. The descriptor stays
 * owned by the server. */
int  jw_ipc_server_fd(const jw_ipc_server *server);
void jw_ipc_server_close(jw_ipc_server *server);

int  jw_ipc_client_connect(const char *socket_path, jw_ipc_client **out);
int  jw_ipc_client_send(jw_ipc_client *client, const char *json, size_t len);
int  jw_ipc_client_recv(jw_ipc_client *client, char **out_json, size_t *out_len);
/* Resolves the server-side Unix peer pid from kernel credentials. */
int  jw_ipc_client_peer_pid(jw_ipc_client *client, pid_t *out_pid);
/* Low-level access used by retained framed streams. The descriptor remains
 * owned by `client`. Switching back to blocking mode restores the selected
 * socket I/O timeout; zero clears it for an indefinitely idle subscriber. */
int  jw_ipc_client_fd(const jw_ipc_client *client);
int  jw_ipc_client_set_nonblocking(jw_ipc_client *client, bool nonblocking,
                                   int blocking_timeout_ms);
void jw_ipc_client_close(jw_ipc_client *client);

int  jw_ipc_request(const char *socket_path, const char *json, size_t len, char **out_json, size_t *out_len);
/* Same one-shot exchange with a caller-selected I/O timeout. Intended for
 * render-thread status reads that must fail quickly on a wedged daemon. */
int  jw_ipc_request_timeout(const char *socket_path, const char *json, size_t len,
                            char **out_json, size_t *out_len, int timeout_ms);

#endif
