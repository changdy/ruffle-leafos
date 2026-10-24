#ifndef JW_RETROARCH_COMMAND_H
#define JW_RETROARCH_COMMAND_H

#include <stdbool.h>
#include <stddef.h>

#define JW_RA_DEFAULT_HOST "127.0.0.1"
#define JW_RA_DEFAULT_PORT 55355u
#define JW_RA_DEFAULT_TIMEOUT_MS 750u
#define JW_RA_REPLY_MAX 1024u

/* Shader-specific, because a first apply compiles and links on the GPU while
 * every other command answers immediately. Measured on MLP1 over 25 first
 * applies of the shipped recommendations plus one shader that fails to link:
 * round trip p50 98ms, p95 115ms, max 117ms (including ~13ms of process spawn
 * in the harness); the shader work alone is p50 85ms, p95 102ms, max 104ms.
 * 1000ms is roughly 9x the measured p95 and still bounds the worst-case UI
 * stall at one second. Unrelated commands keep JW_RA_DEFAULT_TIMEOUT_MS; there
 * is no evidence to change those. */
#define JW_RA_SHADER_TIMEOUT_MS 1000u

/* Bounded lowercase hex, generated per exchange. */
#define JW_RA_REQUEST_ID_MAX 17u

typedef enum {
    JW_RA_OK = 0,
    JW_RA_TIMEOUT,
    JW_RA_UNSUPPORTED,
    JW_RA_PARSE_ERROR,
    JW_RA_SOCKET_ERROR
} jw_ra_result;

typedef enum {
    JW_RA_STATE_UNKNOWN = 0,
    JW_RA_STATE_CONTENTLESS,
    JW_RA_STATE_PLAYING,
    JW_RA_STATE_PAUSED,
    JW_RA_STATE_MENU
} jw_ra_play_state;

typedef struct {
    const char *host;
    unsigned port;
    unsigned timeout_ms;
} jw_ra_client;

/* The four automatic-preset scopes RetroArch itself understands. Kept as an
 * enum so a caller cannot pass a path where a scope belongs. */
typedef enum {
    JW_RA_SHADER_SCOPE_GAME = 0,
    JW_RA_SHADER_SCOPE_PARENT,
    JW_RA_SHADER_SCOPE_CORE,
    JW_RA_SHADER_SCOPE_GLOBAL
} jw_ra_shader_scope;

/* What RetroArch reported. Distinct from jw_ra_result, which says whether the
 * exchange itself worked: a shader that fails to link is a successful exchange
 * carrying JW_RA_SHADER_ERR_APPLY. */
typedef enum {
    JW_RA_SHADER_OK = 0,
    JW_RA_SHADER_NONE,            /* GET: nothing is loaded */
    JW_RA_SHADER_ABSENT,          /* REMOVE: there was nothing to remove */
    JW_RA_SHADER_ERR_MISSING,     /* SET: the preset is not on disk */
    JW_RA_SHADER_ERR_UNSUPPORTED, /* SET: not a preset this driver can load */
    JW_RA_SHADER_ERR_APPLY,       /* SET/CLEAR: compile, link or apply failed */
    JW_RA_SHADER_ERR             /* SAVE/REMOVE failed */
} jw_ra_shader_outcome;

typedef struct {
    jw_ra_play_state state;
    char system[64];
    char content[256];
    char raw[JW_RA_REPLY_MAX];
} jw_ra_status;

typedef struct {
    int disk_count;
    int disk_slot;
    bool savestate_supported;
    int state_slot;
    char raw[JW_RA_REPLY_MAX];
} jw_ra_info;

const char *jw_ra_result_string(jw_ra_result result);
const char *jw_ra_play_state_string(jw_ra_play_state state);

jw_ra_client jw_ra_client_default(void);

bool jw_ra_raw_command_supported(const char *command);
jw_ra_result jw_ra_send_raw(const jw_ra_client *client, const char *command);
jw_ra_result jw_ra_request_raw(const jw_ra_client *client, const char *command,
                               char *reply, size_t reply_size);

jw_ra_result jw_ra_get_status(const jw_ra_client *client, jw_ra_status *status);
jw_ra_result jw_ra_get_info(const jw_ra_client *client, jw_ra_info *info);
jw_ra_result jw_ra_pause(const jw_ra_client *client);
jw_ra_result jw_ra_resume(const jw_ra_client *client);
jw_ra_result jw_ra_pause_direct(const jw_ra_client *client);
jw_ra_result jw_ra_resume_direct(const jw_ra_client *client);
jw_ra_result jw_ra_menu_toggle(const jw_ra_client *client);
jw_ra_result jw_ra_open_menu(const jw_ra_client *client);
jw_ra_result jw_ra_open_shader_menu(const jw_ra_client *client);
jw_ra_result jw_ra_quit(const jw_ra_client *client);
jw_ra_result jw_ra_reset(const jw_ra_client *client);
jw_ra_result jw_ra_audio_reinit(const jw_ra_client *client);
/* Ask RetroArch to write a screenshot of the current frame to its configured
 * screenshot_directory. Fire-and-forget; RA writes the PNG asynchronously. */
jw_ra_result jw_ra_screenshot(const jw_ra_client *client);
jw_ra_result jw_ra_save_state(const jw_ra_client *client);
jw_ra_result jw_ra_load_state(const jw_ra_client *client);
/* slot == -1 loads RetroArch's auto state by selecting slot -1, then using
 * LOAD_STATE. slot >= 0 uses LOAD_STATE_SLOT. */
jw_ra_result jw_ra_load_state_slot(const jw_ra_client *client, int slot,
                                   char *reply, size_t reply_size);
jw_ra_result jw_ra_set_state_slot(const jw_ra_client *client, int slot);
jw_ra_result jw_ra_get_state_slot(const jw_ra_client *client, int *out_slot,
                                  bool *out_supported);
jw_ra_result jw_ra_save_state_slot(const jw_ra_client *client, int slot,
                                   char *reply, size_t reply_size);
jw_ra_result jw_ra_state_slot_plus(const jw_ra_client *client);
jw_ra_result jw_ra_state_slot_minus(const jw_ra_client *client);
jw_ra_result jw_ra_get_disk_count(const jw_ra_client *client, int *out_count);
jw_ra_result jw_ra_get_disk_slot(const jw_ra_client *client, int *out_slot);
jw_ra_result jw_ra_set_disk_slot(const jw_ra_client *client, int slot);
jw_ra_result jw_ra_disk_eject_toggle(const jw_ra_client *client);
jw_ra_result jw_ra_disk_next(const jw_ra_client *client);
jw_ra_result jw_ra_disk_prev(const jw_ra_client *client);
jw_ra_result jw_ra_get_path(const jw_ra_client *client, const char *kind,
                            char *out, size_t out_size);
jw_ra_result jw_ra_get_savestate_path(const jw_ra_client *client,
                                      char *out, size_t out_size);
jw_ra_result jw_ra_show_message(const jw_ra_client *client, const char *message);
jw_ra_result jw_ra_load_content_current_core(const jw_ra_client *client,
                                             const char *content_path,
                                             char *reply, size_t reply_size);


/* Namespaced shader commands. Each generates one request ID, validates the
 * reply source, ignores replies carrying any other ID until a single absolute
 * deadline, and parses only the exact documented reply forms. */
jw_ra_result jw_ra_get_shader(const jw_ra_client *client,
                              jw_ra_shader_outcome *outcome,
                              char *path, size_t path_size);
jw_ra_result jw_ra_set_shader(const jw_ra_client *client,
                              const char *preset_path,
                              jw_ra_shader_outcome *outcome);
jw_ra_result jw_ra_clear_shader(const jw_ra_client *client,
                                jw_ra_shader_outcome *outcome);
jw_ra_result jw_ra_save_shader_preset(const jw_ra_client *client,
                                      jw_ra_shader_scope scope,
                                      jw_ra_shader_outcome *outcome);
jw_ra_result jw_ra_remove_shader_preset(const jw_ra_client *client,
                                        jw_ra_shader_scope scope,
                                        jw_ra_shader_outcome *outcome);

/* ---- Power-hold save: synchronous temporary-file state save (protocol 1) ----
 * GET_STATE_SAVE_INFO -> "GET_STATE_SAVE_INFO 1 <bytes> <compressed 0|1>" or
 * "GET_STATE_SAVE_INFO 1 NO". SAVE_STATE_SYNC <id> <slot> <max bytes>
 * <start-by CLOCK_MONOTONIC ms> -> "SAVE_STATE_SYNC <id> TMP_READY <bytes>
 * <path>" or "SAVE_STATE_SYNC <id> ERROR <code>". RetroArch never publishes
 * the temporary file; the caller renames it. */
#define JW_RA_SYNC_SAVE_PROTOCOL 1
#define JW_RA_SYNC_SAVE_PATH_MAX 4096u
#define JW_RA_SYNC_SAVE_ERROR_MAX 32u

typedef struct {
    bool supported;      /* protocol 1 and the running core can save states */
    bool compressed;     /* savestate_file_compression is on */
    unsigned long long bytes;
} jw_ra_state_save_info;

typedef struct {
    int fd;              /* connected UDP socket, -1 when closed */
    char request_id[JW_RA_REQUEST_ID_MAX];
} jw_ra_sync_save;

typedef struct {
    bool ready;          /* TMP_READY; otherwise error holds the code */
    unsigned long long bytes;
    char tmp_path[JW_RA_SYNC_SAVE_PATH_MAX];
    char error[JW_RA_SYNC_SAVE_ERROR_MAX];
} jw_ra_sync_save_reply;

/* Blocking probe bounded by client->timeout_ms. JW_RA_TIMEOUT means the
   running RetroArch does not answer it (older build): treat as unsupported. */
jw_ra_result jw_ra_get_state_save_info(const jw_ra_client *client,
                                       jw_ra_state_save_info *info);

void jw_ra_sync_save_init(jw_ra_sync_save *save);
/* Send one SAVE_STATE_SYNC on a fresh connected socket and return at once. */
jw_ra_result jw_ra_sync_save_send(const jw_ra_client *client, jw_ra_sync_save *save,
                                  int slot, unsigned long long max_bytes,
                                  long long start_by_ms);
/* Non-blocking. JW_RA_TIMEOUT while no matching reply has arrived (replies for
   other request IDs are discarded); JW_RA_OK with *reply filled once it has;
   JW_RA_PARSE_ERROR for a malformed matching reply. */
jw_ra_result jw_ra_sync_save_poll(jw_ra_sync_save *save, jw_ra_sync_save_reply *reply);
void jw_ra_sync_save_close(jw_ra_sync_save *save);

/* ---- Boot resume: synchronous state load ----
 * LOAD_STATE_SYNC <id> <slot> -> "LOAD_STATE_SYNC <id> OK <bytes>" or
 * "LOAD_STATE_SYNC <id> ERROR <code>". RetroArch applies the state inside the
 * command handler, so OK means the file is the running state. The id is the
 * caller's: a boot resume reuses the id of the save it loads, so one id names
 * both halves in the logs. The exchange handle is the sync save's (one
 * connected socket and an id); close it with jw_ra_sync_save_close(). A
 * RetroArch without the command never answers. */
typedef struct {
    bool loaded;         /* OK; otherwise error holds the code */
    unsigned long long bytes;
    char error[JW_RA_SYNC_SAVE_ERROR_MAX];
} jw_ra_sync_load_reply;

/* Blocking, bounded by client->timeout_ms: LOAD_STATE_SYNC <id> -1, which a
   RetroArch that has the command refuses at once with "<id> ERROR BAD_ARGS"
   and touches nothing. JW_RA_OK means the command exists; JW_RA_TIMEOUT means
   this RetroArch predates it. */
jw_ra_result jw_ra_load_state_sync_probe(const jw_ra_client *client,
                                         const char *request_id);
/* Send one LOAD_STATE_SYNC on a fresh connected socket and return at once.
   `request_id` is 1-16 of [A-Za-z0-9_-]. */
jw_ra_result jw_ra_load_state_sync(const jw_ra_client *client, jw_ra_sync_save *exchange,
                                   const char *request_id, int slot);
/* Non-blocking, as jw_ra_sync_save_poll(): JW_RA_TIMEOUT until the reply for
   this id arrives, JW_RA_OK with *reply filled, JW_RA_PARSE_ERROR for a
   malformed matching reply, JW_RA_SOCKET_ERROR once RetroArch is gone. */
jw_ra_result jw_ra_load_state_sync_poll(jw_ra_sync_save *exchange,
                                        jw_ra_sync_load_reply *reply);

/* Exposed for tests. */
jw_ra_result jw_ra_parse_state_save_info_reply(const char *reply,
                                               jw_ra_state_save_info *info);
/* JW_RA_TIMEOUT when the reply belongs to another request ID. */
jw_ra_result jw_ra_parse_load_state_sync_reply(const char *reply, const char *request_id,
                                               jw_ra_sync_load_reply *out);
/* JW_RA_TIMEOUT when the reply belongs to another request ID. */
jw_ra_result jw_ra_parse_sync_save_reply(const char *reply, const char *request_id,
                                         jw_ra_sync_save_reply *out);
const char *jw_ra_shader_scope_token(jw_ra_shader_scope scope);
jw_ra_result jw_ra_parse_status_reply(const char *reply, jw_ra_status *status);
jw_ra_result jw_ra_parse_shader_reply(const char *reply,
                                      const char *request_id,
                                      const char *operation,
                                      jw_ra_shader_outcome *outcome,
                                      char *path, size_t path_size);

#endif
