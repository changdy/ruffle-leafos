#ifndef JW_IPC_LIFE1_H
#define JW_IPC_LIFE1_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define JW_LIFE1_VERSION 1
#define JW_LIFE1_MAX_PAYLOAD (64u * 1024u)
#define JW_LIFE1_ACK_MS_MAX 1000
#define JW_LIFE1_WAIT_MS_MAX 15000

typedef enum {
    JW_LIFE1_REQUEST_NONE = 0,
    JW_LIFE1_REQUEST_SUBSCRIBE,
    JW_LIFE1_REQUEST_GAME_STATE,
} jw_life1_request_kind;

typedef enum {
    JW_LIFE1_MODE_NOTIFY = 0,
    JW_LIFE1_MODE_STOP,
} jw_life1_mode;

typedef struct {
    jw_life1_request_kind kind;
    char *id;
    char *service_id;
    jw_life1_mode mode;
    int ack_ms;
    int wait_ms;
    bool check_before_stop;
} jw_life1_request;

typedef enum {
    JW_LIFE1_PARSE_NOT_LIFE1 = 0,
    JW_LIFE1_PARSE_OK = 1,
    JW_LIFE1_PARSE_INVALID = -1,
} jw_life1_parse_result;

typedef enum {
    JW_LIFE1_STATUS_NONE = 0,
    JW_LIFE1_STATUS_WAITING,
    JW_LIFE1_STATUS_READY,
    JW_LIFE1_STATUS_STOP,
    JW_LIFE1_STATUS_ERROR,
} jw_life1_status_kind;

typedef struct {
    jw_life1_status_kind kind;
    char *launch_id;
    int pending_items;
    int64_t pending_bytes;
    bool has_pending_bytes;
    char *reason;
} jw_life1_status;

/* Recognizes the two client requests owned by LIFE-1. A request whose `op`
 * is not `subscribe` or `game.state` is returned as NOT_LIFE1 so the daemon
 * can preserve its existing one-shot protocol. Once a LIFE-1 op is claimed,
 * its object is closed and malformed/unknown fields fail with a stable slug.
 */
jw_life1_parse_result jw_life1_parse_request(const char *payload,
                                             size_t payload_len,
                                             jw_life1_request *request,
                                             char *error_code,
                                             size_t error_code_size);
void jw_life1_request_destroy(jw_life1_request *request);

/* Strict parser for service replies on an authenticated subscriber stream.
 * waiting/ready/stop/error objects are closed; unknown or malformed fields
 * fail. The exchange owner validates whether waiting carries pending_bytes and
 * whether ready or stop is the valid terminal status for that exchange. */
jw_life1_parse_result jw_life1_parse_status(const char *payload,
                                            size_t payload_len,
                                            jw_life1_status *status,
                                            char *error_code,
                                            size_t error_code_size);
void jw_life1_status_destroy(jw_life1_status *status);

/* Canonical compact replies. Returned strings use cJSON's allocator and must
 * be released with cJSON_free(). */
char *jw_life1_build_ok(const char *id);
char *jw_life1_build_error(const char *id, const char *code,
                           const char *message);
char *jw_life1_build_game_state_inactive(const char *id);
char *jw_life1_build_game_state_active(const char *id,
                                       const char *launch_id,
                                       const char *source_id,
                                       const char *saves_path,
                                       const char *states_path);
char *jw_life1_build_game_start(const char *launch_id,
                                const char *source_id,
                                const char *saves_path,
                                const char *states_path,
                                int wait_budget_ms);
char *jw_life1_build_game_check(const char *launch_id,
                                const char *source_id,
                                const char *saves_path,
                                const char *states_path,
                                int wait_budget_ms);
char *jw_life1_build_game_cancel(const char *launch_id);
char *jw_life1_build_game_abort(const char *launch_id);
char *jw_life1_build_game_finish(const char *launch_id);

#endif
