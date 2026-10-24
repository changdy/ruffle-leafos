#ifndef JW_IPC_CLIENT_H
#define JW_IPC_CLIENT_H

#include "internal/ipc/ipc.h"
#include "internal/db/db.h"
#include "internal/platform/device.h"
#include "internal/platform/input_shortcuts.h"
#include "internal/retroarch/command.h"

#include <stdbool.h>

#define JW_IPC_UPDATE_MAX_OPTIONS 20

typedef struct {
    bool active;
    bool command_ok;
    char command_result[32];
    char system[64];
    char rom_path[512];
    char core_path[512];
    char core_id[64];
    char core_config_folder[256];
    int disk_count;
    int disk_slot;
    bool savestate_supported;
    int state_slot;
} jw_ipc_retroarch_session_info;

typedef struct {
    jw_platform_audio_output output;
    unsigned available_outputs;
    int volume_percent[JW_PLATFORM_AUDIO_OUTPUT_COUNT];
    int test_playing;          /* 1 while the Test Sound clip is playing, else 0 */
} jw_ipc_audio_status;

typedef struct {
    bool supported;
    char name[JW_PLATFORM_PERF_VALUE_MAX];
    char governor[JW_PLATFORM_PERF_VALUE_MAX];
    int current_freq;
    int set_freq;
    char available_governors[JW_PLATFORM_PERF_LIST_MAX];
    char available_frequencies[JW_PLATFORM_PERF_LIST_MAX];
} jw_ipc_performance_domain_status;

typedef struct {
    bool supported;
    char active_profile[JW_PLATFORM_PERF_VALUE_MAX];
    char global_profile[JW_PLATFORM_PERF_VALUE_MAX];
    char session_profile[JW_PLATFORM_PERF_VALUE_MAX];
    bool session_override;
    int soc_temp_c;
    char message[256];
    char last_error[256];
    jw_ipc_performance_domain_status domains[JW_PLATFORM_PERF_DOMAIN_COUNT];
} jw_ipc_performance_status_info;

typedef struct {
    char source[32];
    char label[64];
    char mount_path[512];
    char message[256];
    bool present;
    bool mounted;
    bool busy;
    bool can_unmount;
    /* Health (storage-status v2). Empty strings when an older daemon answers. */
    char access[16];              /* "read-write" | "read-only" | "unknown" */
    char cause[24];               /* "filesystem-error" | "write-protected" | "unknown" */
    char repair[16];              /* "none" | "pending" | "running" | "failed" */
    char fs_type[32];
    char uuid[64];
    char volume_label[64];
    char kernel_message[256];
    bool dirty_at_boot;
    bool block_write_protected;
    bool repair_supported;
    char repair_unavailable_reason[64];
    char repair_mode[16];
    bool warning_pending;         /* a newly observed read-only state not yet acknowledged */
    int health_generation;
    bool external_power;
    int battery_percent;          /* -1 if unknown */
    bool last_repair_valid;
    bool last_repair_acknowledged;
    char last_repair_request_id[64];
    char last_repair_outcome[32];
    char last_repair_mount_state[32];
    char last_repair_mode[16];
    char last_repair_origin[32];
    char last_repair_trigger[32];
    char hold_trigger[32];        /* why a held card is held, e.g. paused-shutdown */
    bool last_repair_changes_complete;
    int last_repair_reported_changes;
} jw_ipc_storage_status_info;

typedef struct {
    int generation;
    bool scan_running;
    bool pending_rescan;
    bool library_populated;
    char scan_reason[96];
    char scan_error[160];
    int storage_health_generation;   /* -1 when the daemon does not report it */
    /* The scraper's state ("idle", "running", "paused-quota",
       "paused-storage"), or "" when the daemon does not report it. */
    char scrape_state[16];
} jw_ipc_library_status_info;

typedef struct {
    const char *source_id;
    const char *rom_relpath;
    const char *image_root_kind;
    const char *image_relpath;
} jw_ipc_relocation_identity;

typedef struct {
    jw_ipc_relocation_identity old_identity;
    jw_ipc_relocation_identity new_identity;
} jw_ipc_relocation_item;

typedef struct {
    char operation_id[96];
    char state[16];
    int expected_generation;
    int mapping_generation;
    int scan_ticket_generation;
    int library_generation;
    int item_count;
} jw_ipc_relocation_status;

int jw_ipc_has_feature(const char *socket_path, const char *feature,
                       bool *out_supported);
int jw_ipc_relocation_prepare(const char *socket_path, const char *operation_id,
                              int expected_generation,
                              const jw_ipc_relocation_item *items, int item_count,
                              jw_ipc_relocation_status *out,
                              char *error, int error_len);
int jw_ipc_relocation_get_status(const char *socket_path, const char *operation_id,
                                 jw_ipc_relocation_status *out,
                                 char *error, int error_len);
int jw_ipc_relocation_commit(const char *socket_path, const char *operation_id,
                             jw_ipc_relocation_status *out,
                             char *error, int error_len);
int jw_ipc_relocation_revert(const char *socket_path, const char *operation_id,
                             jw_ipc_relocation_status *out,
                             char *error, int error_len);
int jw_ipc_relocation_abort(const char *socket_path, const char *operation_id,
                            jw_ipc_relocation_status *out,
                            char *error, int error_len);
int jw_ipc_relocation_finish(const char *socket_path, const char *operation_id,
                             jw_ipc_relocation_status *out,
                             char *error, int error_len);

typedef struct {
    int index;
    bool selected;
    bool installed;
    char release_id[128];
    char version[128];
    char published_at[64];
    char notes_url[512];
    char artifact_kind[64];
    char artifact_name[256];
    long long artifact_size;
    long long installed_size;
} jw_ipc_update_option_info;

typedef struct {
    char state[32];
    bool compatible;
    bool has_update;
    bool current_unknown;
    int installed_schema;
    char platform_id[32];
    char current_release_id[128];
    char current_version[128];
    char release_id[128];
    char version[128];
    char published_at[64];
    char source_manifest[512];
    char manifest_url[1024];
    char notes_url[512];
    char artifact_kind[64];
    char artifact_name[256];
    char artifact_url[1024];
    char artifact_sha256[65];
    long long artifact_size;
    long long installed_size;
    bool downloaded;
    bool download_active;
    long long download_received;
    long long download_total;
    int download_percent;
    char download_path[512];
    char handoff_type[64];
    char handoff_completion[64];
    char handoff_trigger_file[256];
    char recovery_name[256];
    char recovery_url[1024];
    int managed_apps_count;
    int migrations_count;
    bool install_ready;
    bool install_blocked;
    bool install_needs_confirmation;
    bool install_active;
    bool install_armed;
    bool install_idle;
    int install_battery_percent;
    int install_charging;
    long long install_required_free;
    long long install_available_free;
    char install_result_state[64];
    char install_result_release_id[128];
    char install_result_message[256];
    char install_request_path[512];
    char install_result_path[512];
    char install_reason[64];
    char install_message[256];
    char message[256];
    int selected_option;
    int option_count;
    bool options_complete;   /* every release in the list has been read */
    bool options_loading;    /* a release-list load is running */
    jw_ipc_update_option_info options[JW_IPC_UPDATE_MAX_OPTIONS];
} jw_ipc_update_status_info;

/* Send a "hello" handshake to jawakad.
 * role: "launcher" or "menu".
 * Returns 0 on success, -1 on failure. */
int jw_ipc_hello(const char *socket_path, const char *role);
/* hello, also telling jawakad this process handles SIGUSR2 as "the volume
   changed, re-read it" (the launcher's status bar). */
int jw_ipc_hello_levels_signal(const char *socket_path, const char *role);

/* Request a library rescan. Populates status[status_len] with a human-readable
 * result message. Returns 0 on success, -1 on failure. */
int jw_ipc_scan_library(const char *socket_path, char *status, int status_len);
int jw_ipc_library_status_full(const char *socket_path, jw_ipc_library_status_info *out);
int jw_ipc_library_status(const char *socket_path, int *out_generation);
int jw_ipc_get_storage_status(const char *socket_path, const char *source,
                              jw_ipc_storage_status_info *out,
                              char *status, int status_len);
int jw_ipc_safe_unmount_storage(const char *socket_path, const char *source,
                                char *status, int status_len);
/* Acknowledge the read-only warning for the card currently behind source. */
int jw_ipc_storage_warning_ack(const char *socket_path, const char *source);
/* Commit a reboot repair (mode "repair") or offline check (mode "check") for
   source and restart the device. status receives the daemon's refusal text. */
int jw_ipc_storage_repair_request(const char *socket_path, const char *source,
                                  const char *mode, bool allow_battery,
                                  char *status, int status_len);
/* Mark a repair result as seen so it is shown once. */
int jw_ipc_storage_repair_result_ack(const char *socket_path, const char *request_id);

/* Ask jawakad to show the menu overlay. Returns 0 on success, -1 on failure. */
int jw_ipc_open_menu(const char *socket_path);

/* Fire a haptic rumble by naming a semantic UI event. The daemon owns the
 * vocabulary (single/double/triple tick) and the UI rumble gate.
 * event: "nav" | "select" | "commit" | "blocked". Fire-and-forget; the daemon
 * queues the burst and replies immediately. Returns 0 on success, -1 on failure. */
int jw_ipc_rumble(const char *socket_path, const char *event);

/* Live one-tick preview at an exact strength (0-100), for the settings slider.
 * Ignores the stored strength and the enabled gate. Returns 0/-1. */
int jw_ipc_rumble_preview(const char *socket_path, int strength);

/* Hand jawakad the complete in-game shortcut state after Settings has
 * persisted it: the three bindings plus the two capture opt-ins that gate
 * them.
 *
 * The whole state, not the one row that changed: the daemon replaces its copy
 * in a single assignment, so there is no window in which two actions hold the
 * same button or a binding is live without its feature flag. Buttons travel as
 * their persisted names, so the wire and the settings table cannot disagree
 * about what "l1" means.
 *
 * The opt-ins ride along because they are the other half of the same question
 * -- whether a chord does anything -- and the daemon reads both from memory on
 * the input path. Without this it would have to poll the database for them.
 *
 * Returns 0 when the daemon accepted it. A non-zero result means the running
 * daemon still has the previous state -- the durable values are already
 * written either way, so the caller should say the change takes effect after a
 * daemon restart rather than rolling anything back. */
int jw_ipc_set_input_shortcuts(const char *socket_path,
                               const jw_input_shortcuts *shortcuts,
                               bool screenshots_enabled,
                               bool recording_enabled);

/* Ask jawakad to launch a game through the daemon-owned RetroArch process.
 * rom_path may be absolute or relative to the SD-card root.
 * Populates status[status_len] with a human-readable result when provided. */
int jw_ipc_launch_game(const char *socket_path, const char *system,
                       const char *rom_path, char *status, int status_len);

/* Same launch request, but asks jawakad to resume the switcher-preferred state
 * after RetroArch starts. Normal game browser launches should not use this. */
int jw_ipc_launch_game_switcher(const char *socket_path, const char *system,
                                const char *rom_path, char *status,
                                int status_len);

/* Ask jawakad to launch an app pak as a foreground child.
 * pak_dir may be absolute or relative to the SD-card root.
 * Populates status[status_len] with a human-readable result when provided. */
int jw_ipc_launch_app(const char *socket_path, const char *pak_dir,
                      char *status, int status_len);

/* Ask jawakad to open the in-game game switcher overlay for the active RetroArch
 * session. Reversible: the daemon pauses + overlays only; it does not save or
 * quit. Replies error when there is no active session. */
int jw_ipc_open_switcher(const char *socket_path, char *status, int status_len);

/* Commit a switch from the in-game switcher to a different game: jawakad saves
 * the current game when supported, quits it, and spawns the selected game
 * directly (no launcher in between). Requires an active RetroArch session. */
int jw_ipc_switch_game(const char *socket_path, const char *system,
                       const char *rom_path, char *status, int status_len);

/* Fetch the daemon-owned RetroArch session and command-interface state. */
int jw_ipc_get_retroarch_session(const char *socket_path,
                                 jw_ipc_retroarch_session_info *out,
                                 char *status, int status_len);

/* Ask jawakad to perform a RetroArch action for the active session.
 * value is action-specific; pass 0 when unused. */
int jw_ipc_retroarch_action(const char *socket_path, const char *action,
                            int value, char *status, int status_len);

typedef struct {
    jw_ra_result result;
    jw_ra_shader_outcome outcome;
    char path[1024];
} jw_ipc_retroarch_shader_reply;

/* Closed, typed client side of jawakad's retroarch-shader request. */
int jw_ipc_retroarch_shader_get(const char *socket_path,
                                jw_ipc_retroarch_shader_reply *out,
                                char *status, int status_len);
int jw_ipc_retroarch_shader_set(const char *socket_path, const char *path,
                                jw_ipc_retroarch_shader_reply *out,
                                char *status, int status_len);
int jw_ipc_retroarch_shader_restore(const char *socket_path, const char *path,
                                    jw_ipc_retroarch_shader_reply *out,
                                    char *status, int status_len);
int jw_ipc_retroarch_shader_clear(const char *socket_path,
                                  jw_ipc_retroarch_shader_reply *out,
                                  char *status, int status_len);
int jw_ipc_retroarch_shader_save(const char *socket_path,
                                 jw_ra_shader_scope scope,
                                 jw_ipc_retroarch_shader_reply *out,
                                 char *status, int status_len);
int jw_ipc_retroarch_shader_remove(const char *socket_path,
                                   jw_ra_shader_scope scope,
                                   jw_ipc_retroarch_shader_reply *out,
                                   char *status, int status_len);

/* Reset the shared RetroArch config back to packaged platform defaults. */
int jw_ipc_reset_retroarch_config(const char *socket_path,
                                  char *status, int status_len);

/* Ask jawakad to shut down. Returns 0 on success. */
int jw_ipc_shutdown(const char *socket_path);

/* Ask jawakad to exit Leaf mode and pass this boot to the stock launcher. */
int jw_ipc_exit_stock(const char *socket_path);

/* Notify jawakad that a frontend process has rendered enough to be considered
 * ready. role is usually "launcher" or "menu". Returns 0 on success. */
int jw_ipc_frontend_ready(const char *socket_path, const char *role);

/* Send a platform-action request to jawakad (e.g. "poweroff", "reboot").
 * Returns 0 on success. */
int jw_ipc_platform_action(const char *socket_path, const char *action, int value);

/* Brightness and volume from jawakad's cache (platform-levels), cheap enough to
 * poll: platform-status re-reads the audio route and volume with several
 * process spawns. jw_ipc_platform_brightness() and jw_ipc_platform_volume() are
 * the one-value forms. Either output may be NULL; -1 means unknown. */
int jw_ipc_platform_levels(const char *socket_path, int *out_brightness,
                           int *out_volume);
/* The same, plus the seconds left on an armed HDMI 1080p120 revert (0 = none;
   -1 = the daemon does not say, ask jw_ipc_hdmi_revert_status). */
int jw_ipc_platform_levels_full(const char *socket_path, int *out_brightness,
                                int *out_volume, int *out_hdmi_revert_seconds);
int jw_ipc_platform_brightness(const char *socket_path, int *out_percent);
/* platform-status's battery, charging, volume and HDMI revert seconds in one
   request; each -1 when unknown. Any output may be NULL. */
int jw_ipc_platform_power_status_full(const char *socket_path, int *out_battery_percent,
                                      int *out_charging, int *out_volume_percent,
                                      int *out_hdmi_revert_seconds);
int jw_ipc_platform_power_status(const char *socket_path,
                                 int *out_battery_percent,
                                 int *out_charging);
int jw_ipc_set_brightness(const char *socket_path, int percent,
                          int *out_percent, char *status, int status_len);

int jw_ipc_platform_volume(const char *socket_path, int *out_percent);
/* HDMI 1080p120 auto-revert: seconds left before revert (0 = none pending), and
   the user's "keep this mode" confirmation that cancels the revert. */
int jw_ipc_hdmi_revert_status(const char *socket_path, int *out_seconds);
int jw_ipc_hdmi_revert_keep(const char *socket_path);
int jw_ipc_set_volume(const char *socket_path, int percent,
                      int *out_percent, char *status, int status_len);
int jw_ipc_platform_audio_status(const char *socket_path,
                                 jw_ipc_audio_status *out_status);
int jw_ipc_set_audio_output(const char *socket_path,
                            jw_platform_audio_output output,
                            char *status, int status_len);

int jw_ipc_get_performance_status(const char *socket_path,
                                  jw_ipc_performance_status_info *out,
                                  char *status, int status_len);
int jw_ipc_set_performance_profile(const char *socket_path,
                                   const char *scope,
                                   const char *profile,
                                   char *status, int status_len);
int jw_ipc_set_performance_custom(const char *socket_path,
                                  const jw_platform_perf_request *request,
                                  char *status, int status_len);
int jw_ipc_reset_performance_session(const char *socket_path,
                                     char *status, int status_len);

int jw_ipc_update_status(const char *socket_path,
                         jw_ipc_update_status_info *out,
                         char *status, int status_len);
int jw_ipc_update_check(const char *socket_path,
                        const char *manifest_path,
                        jw_ipc_update_status_info *out,
                        char *status, int status_len);
/* Load every release in the list for the picker (the routine check only reads
   the newest). Returns at once; poll status until options_loading clears.
   refresh reloads a list that is already complete. */
int jw_ipc_update_releases(const char *socket_path,
                           bool refresh,
                           jw_ipc_update_status_info *out,
                           char *status, int status_len);
int jw_ipc_update_select(const char *socket_path,
                         int option_index,
                         jw_ipc_update_status_info *out,
                         char *status, int status_len);
int jw_ipc_update_download(const char *socket_path,
                           jw_ipc_update_status_info *out,
                           char *status, int status_len);
int jw_ipc_update_cancel(const char *socket_path,
                         jw_ipc_update_status_info *out,
                         char *status, int status_len);
int jw_ipc_update_install_preflight(const char *socket_path,
                                    bool confirm_unknown_battery,
                                    jw_ipc_update_status_info *out,
                                    char *status, int status_len);
int jw_ipc_update_install(const char *socket_path,
                          bool confirm_unknown_battery,
                          jw_ipc_update_status_info *out,
                          char *status, int status_len);

int jw_ipc_get_adb(const char *socket_path, int *out_enabled,
                   int *out_intent_enabled, bool *out_supported);
int jw_ipc_set_adb(const char *socket_path, int enabled,
                   char *status, int status_len);
int jw_ipc_get_boot_splash(const char *socket_path, int *out_enabled,
                           bool *out_supported);
int jw_ipc_set_boot_splash(const char *socket_path, int enabled,
                           char *status, int status_len);
int jw_ipc_get_refresh_rate(const char *socket_path, int *out_hz,
                            bool *out_supported);

/* The rows jw_ipc_get_refresh_rate(), jw_ipc_get_color_temp() and
 * jw_ipc_get_hdmi_status() each answer, from one platform-status round trip
 * instead of three. Returns 0 when the daemon answered; unknown values stay
 * -1 / false. */
typedef struct {
    int  refresh_rate_hz;
    bool refresh_rate_supported;
    int  color_temp_kelvin;
    bool color_temp_supported;
    int  hdmi_connected;
    int  hdmi_output_mode;
    bool hdmi_supported;
} jw_ipc_display_status;
int jw_ipc_get_display_status(const char *socket_path, jw_ipc_display_status *out);
int jw_ipc_set_refresh_rate(const char *socket_path, int hz,
                            char *status, int status_len);
int jw_ipc_get_color_temp(const char *socket_path, int *out_kelvin,
                          bool *out_supported);
int jw_ipc_set_color_temp(const char *socket_path, int kelvin,
                          char *status, int status_len);
/* UI language. The daemon persists it and restarts the launcher, which comes
   back with the matching font and string table -- there is no live switch, and
   no getter, because the launcher reads the setting itself at startup. */
/* `keep_running` asks the daemon to persist the language without restarting the
   launcher, so a caller that can reload its own string table and font stays put
   (Settings does this, and falls back to the restart when a reload fails). */
int jw_ipc_set_language_ex(const char *socket_path, const char *lang,
                           bool keep_running, char *status, int status_len);

int jw_ipc_set_language(const char *socket_path, const char *lang,
                        char *status, int status_len);
/* A font family or size was persisted. The daemon restarts the OSD when its
   banner font no longer matches; fire-and-forget like the haptics. */
int jw_ipc_refresh_osd_appearance(const char *socket_path);
/* HDMI output: status (connected/current mode/supported) + set (0 off/1 4:3/2 stretch). */
int jw_ipc_get_hdmi_status(const char *socket_path, int *out_connected,
                           int *out_mode, bool *out_supported);
int jw_ipc_set_hdmi_output(const char *socket_path, int mode,
                           char *status, int status_len);

typedef struct {
    bool valid;           /* credentials confirmed by ScreenScraper */
    bool rejected;        /* explicit credential rejection (vs transport error) */
    int  max_threads;
    int  requests_today;
    int  max_requests;
    int  user_level;
    char message[256];    /* daemon-side error detail when !valid */
} jw_ipc_scrape_validate_info;

/* Ask jawakad to validate ScreenScraper user credentials against the API
   (the daemon owns the dev-credential half of the auth). Returns 0 when the
   request completed (check out->valid / out->rejected), -1 when the daemon
   was unreachable or replied malformed. */
int jw_ipc_scrape_validate(const char *socket_path, const char *username,
                           const char *password,
                           jw_ipc_scrape_validate_info *out);

typedef struct {
    char state[16];           /* "idle" | "running" | "paused-quota" | "paused-storage" */
    int  total;
    int  done;
    int  found;
    int  not_found;
    int  failed;
    int  cancelled;
    int  queued;
    int  active;
    char current_name[256];
    char current_system[64];
    char message[256];
} jw_ipc_scrape_status_info;

#define JW_IPC_SCRAPE_QUEUE_MAX_ROWS 256

typedef enum {
    JW_IPC_SCRAPE_ROW_QUEUED = 0,
    JW_IPC_SCRAPE_ROW_HASH,
    JW_IPC_SCRAPE_ROW_SEARCH,
    JW_IPC_SCRAPE_ROW_DOWNLOAD,
    JW_IPC_SCRAPE_ROW_SAVE,
    JW_IPC_SCRAPE_ROW_DONE,
    JW_IPC_SCRAPE_ROW_NOT_FOUND,
    JW_IPC_SCRAPE_ROW_ERROR,
    JW_IPC_SCRAPE_ROW_CANCELLED,
} jw_ipc_scrape_row_state;

typedef struct {
    unsigned id;
    jw_ipc_scrape_row_state state;
    char display_name[256];
    char system[64];
    char rom_path[512];
    char output_path[512];
    char message[256];
} jw_ipc_scrape_queue_row;

typedef struct {
    char state[16];           /* "idle" | "running" | "paused-quota" | "paused-storage" */
    int total;
    int done;
    int found;
    int not_found;
    int failed;
    int cancelled;
    int queued;
    int active;
    int requests_today;
    int max_requests;
    int max_threads;
    int permits;
    int eta_seconds;          /* -1 when unavailable */
    int row_count;
    char message[256];
    jw_ipc_scrape_queue_row rows[JW_IPC_SCRAPE_QUEUE_MAX_ROWS];
} jw_ipc_scrape_queue_info;

typedef struct {
    int requested;
    int enqueued;
    int already_queued;
    int skipped_existing;
    bool queue_full;
} jw_ipc_scrape_start_info;

/* Queue scraping. scope "game" needs rom_path (always re-fetches/replaces);
   scope "system" honors missing_only. On success returns 0 and sets
   *out_enqueued; on failure returns -1 with a daemon message in
   status[status_len]. */
int jw_ipc_scrape_start(const char *socket_path, const char *scope,
                        const char *system, const char *rom_path,
                        bool missing_only, int *out_enqueued,
                        char *status, int status_len);
/* Rich scrape-start response. Missing fields read as zero against older
   daemons. scope "game" still reports queue-full through the error/status path
   for legacy single-game launcher behavior. */
int jw_ipc_scrape_start_full(const char *socket_path, const char *scope,
                             const char *system, const char *rom_path,
                             bool missing_only,
                             jw_ipc_scrape_start_info *out,
                             char *status, int status_len);
int jw_ipc_scrape_status(const char *socket_path,
                         jw_ipc_scrape_status_info *out);
int jw_ipc_scrape_queue(const char *socket_path, int offset, int limit,
                        jw_ipc_scrape_queue_info *out);
/* True in *out_pending when the system (rom_path NULL) or game has queued
   or in-flight scrape work. */
int jw_ipc_scrape_pending(const char *socket_path, const char *system,
                          const char *rom_path, bool *out_pending);
/* scope "all" | "system" | "game"; system/rom_path as required by scope. */
int jw_ipc_scrape_cancel(const char *socket_path, const char *scope,
                         const char *system, const char *rom_path,
                         int *out_removed);
int jw_ipc_scrape_stop_all(const char *socket_path, int *out_stopped);
int jw_ipc_scrape_clear_done(const char *socket_path, int *out_cleared);

#define JW_IPC_MISSING_MAX_SYSTEMS 256
typedef struct {
    char system[64];   /* system code (e.g. "GB") */
    int  missing;      /* games still needing art */
    int  total;        /* games in this system */
} jw_ipc_scrape_missing_row;
typedef struct {
    int total_missing;
    int system_count;
    jw_ipc_scrape_missing_row systems[JW_IPC_MISSING_MAX_SYSTEMS];
} jw_ipc_scrape_missing_info;
/* Per-system missing/total art counts (mapped systems with games only). */
int jw_ipc_scrape_missing_counts(const char *socket_path,
                                 jw_ipc_scrape_missing_info *out);

/* LED ring. set-led applies + persists in jawakad; get-led reads the cached
   state back from platform-status. mode is "FOREVER"/"BREATH"/"RAINBOW". */
int jw_ipc_set_led(const char *socket_path, int enabled, const char *mode,
                   int r, int g, int b, int brightness, int speed,
                   char *status, int status_len);
int jw_ipc_get_led(const char *socket_path, int *enabled, char *mode, int mode_len,
                   int *r, int *g, int *b, int *brightness, int *speed);

/* ---- CTL-1: service control (control-ipc-v1) ---- */

#define JW_IPC_SVC_ID_MAX 128
#define JW_IPC_SVC_STATE_MAX 32
#define JW_IPC_SVC_LIST_MAX 32

typedef struct {
    char id[JW_IPC_SVC_ID_MAX + 1];
    char state[JW_IPC_SVC_STATE_MAX + 1];
    bool desired_enabled;
} jw_ipc_service_info;

int jw_ipc_service_list(const char *socket_path, jw_ipc_service_info *out,
                        int max, int *out_count);
int jw_ipc_service_ctl(const char *socket_path, const char *op,
                       const char *service_id, char *status, int status_len);

#endif /* JW_IPC_CLIENT_H */
