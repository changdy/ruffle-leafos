#ifndef JW_IPC_STREAM_H
#define JW_IPC_STREAM_H

#include "internal/ipc/ipc.h"

#include <stdbool.h>
#include <stddef.h>

#define JW_IPC_STREAM_QUEUE_MAX 16
#define JW_IPC_STREAM_PARTIAL_TIMEOUT_MS 1000

typedef struct jw_ipc_stream jw_ipc_stream;

/* Takes ownership of `client` on success. The stream is non-blocking from
 * this point onward. */
int jw_ipc_stream_create(jw_ipc_client *client, jw_ipc_stream **out);
void jw_ipc_stream_destroy(jw_ipc_stream *stream);

int jw_ipc_stream_fd(const jw_ipc_stream *stream);
int jw_ipc_stream_peer_pid(jw_ipc_stream *stream, pid_t *out_pid);
bool jw_ipc_stream_wants_write(const jw_ipc_stream *stream);
int jw_ipc_stream_queued(const jw_ipc_stream *stream);

/* Consume as much currently available input as is needed for one frame.
 * Returns 1 with an allocated payload, 0 for would-block/no complete frame,
 * -1 for peer/error, -2 for an invalid transport frame, and -3 when a partial
 * frame exceeded the fixed one-second deadline. The caller owns `out_payload`.
 */
int jw_ipc_stream_receive(jw_ipc_stream *stream, long long now_ms,
                          char **out_payload, size_t *out_len);

/* Queue one complete framed payload. The queue is exactly 16 messages; an
 * overflow returns -1 without altering the existing queue so the caller can
 * drop the connection as required by LIFE-1. */
int jw_ipc_stream_queue(jw_ipc_stream *stream, const char *payload, size_t len);

/* Flush queued bytes without blocking. Returns 0 while healthy, -1 when the
 * peer or socket failed. */
int jw_ipc_stream_flush(jw_ipc_stream *stream);

/* Used for a classified legacy one-shot request. Requires an empty outbound
 * queue, restores blocking I/O with a five-second timeout, and transfers
 * ownership back to the caller. */
jw_ipc_client *jw_ipc_stream_detach_blocking(jw_ipc_stream *stream);

#endif
