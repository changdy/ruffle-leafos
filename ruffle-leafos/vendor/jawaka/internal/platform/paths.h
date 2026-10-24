#ifndef JW_PLATFORM_PATHS_H
#define JW_PLATFORM_PATHS_H

#include <stdbool.h>
#include <stddef.h>

char *jw_runtime_dir(void);
char *jw_sdcard_root(void);
char *jw_state_dir(void);
char *jw_socket_path(void);
char *jw_osd_socket_path(void);
/* Runtime file selecting which surface the resident in-game UI shows on its
   next reveal: "menu" (default) or "switcher". Written by
   the daemon before SIGUSR1, read by jawaka-menu --in-game on each show. */
char *jw_ingame_ui_mode_path(void);
char *jw_db_path(void);
char *jw_retroarch_bin_path(void);
char *jw_retroarch_shader_manifest_path(void);
char *jw_retroarch_recommended_shaders_dir(void);
/* <system>/launcher/res -- shared identity art for binaries outside the launcher. */
char *jw_launcher_res_dir(void);
bool jw_retroarch_shader_path_is_recommended(const char *candidate,
                                             char *resolved, size_t resolved_size,
                                             char *relative, size_t relative_size);
/* Restore is narrower than a general shader load, but broader than preview:
   it may reapply the path RetroArch reported from the durable shader,
   automatic-preset, or effective video_shader_dir roots. */
bool jw_retroarch_shader_path_is_restorable(const char *candidate,
                                            const char *runtime_config_path,
                                            char *resolved, size_t resolved_size,
                                            char *relative, size_t relative_size);
/* Release RetroArch cores directory (CORES_PATH or probed). Caller frees. */
char *jw_retroarch_cores_dir(void);
char *jw_retroarch_core_path_for_system(const char *system);
char *jw_retroarch_core_path_for_system_choice(const char *system,
                                               const char *preferred_core_id,
                                               char *out_core_id,
                                               size_t out_core_id_size,
                                               char *out_config_folder,
                                               size_t out_config_folder_size,
                                               char *out_info_dir,
                                               size_t out_info_dir_size,
                                               char *diagnostic,
                                               size_t diagnostic_size);
bool jw_sdcard_exec_available_for_path(const char *path, char *error, size_t error_size);
/* How a value can be written to a RetroArch config line so RetroArch's
 * escape-free parser reads back exactly that value. QUOTED is `key = "v"`,
 * BARE is `key = v` (the only spelling for a value holding '"'), UNWRITABLE
 * has no exact spelling: a '"' together with whitespace, '#', a non-ASCII
 * byte or a line break, or a leading '"'. Settings uses it to refuse such a
 * RetroAchievements password up front. See paths.c. */
typedef enum {
    JW_RA_CFG_QUOTED,
    JW_RA_CFG_BARE,
    JW_RA_CFG_UNWRITABLE,
} jw_ra_cfg_form;
jw_ra_cfg_form jw_retroarch_cfg_value_form(const char *value);

/* player_joypad_indices: per-user SDL joypad indices for players 1-4 from the
 * Jawaka input roster (-1 = unused player). Non-NULL also raises the generated
 * input_max_users to 4 on MLP1, and every generated index is protected from
 * persisted-config overrides because the values describe the launch roster,
 * not a user preference. NULL keeps the legacy single-player behavior (no
 * index keys, input_max_users 1). */
/* proxied_cheevos: when true this launch was routed through RAOfflineProxy, so
 * cheevos_custom_host and cheevos_hardcore_mode_enable are stripped from the
 * merged defaults/shared text and emitted exactly once with the proxy values.
 * Emitting them once is required, not cosmetic: RetroArch's config parser
 * keeps the FIRST occurrence of a key and drops every later duplicate
 * (libretro-common/file/config_file.c -- "Only add entry to the map if an
 * entry with the specified value does not already exist"), so an override
 * appended after a value merged in from the shared config would be silently
 * ignored and the launch would talk straight to retroachievements.org. */
char *jw_prepare_retroarch_config(const char *runtime_dir, const char *sdcard_root,
                                   const char *core_path,
                                   const int player_joypad_indices[4],
                                   bool persist_changes,
                                   bool proxied_cheevos,
                                   char *error, size_t error_size);

/* What a LEAF-RA-1 frontend (a content pak's own RetroArch, launched in place
   of Leaf's) cannot be assumed to have, so the protected block must not ask
   for it. NULL means Leaf's own RetroArch, which has everything. */
typedef struct {
    bool no_recording;  /* no FFmpeg: record_driver "null", nothing else */
    bool pulse_audio;   /* no BlueALSA plugin: PulseAudio even on Bluetooth */
} jw_retroarch_frontend_profile;

/* Every LEAF-RA-1 frontend gets this profile. */
extern const jw_retroarch_frontend_profile jw_retroarch_frontend_profile_leaf_ra_1;

/* Per-launch variant used when the core was selected from an immutable
   effective-catalog snapshot. explicit_info_dir must come from that same
   snapshot, so an in-session generation swap cannot leave INFO_PATH stale.
   frontend is NULL for Leaf's own RetroArch. */
char *jw_prepare_retroarch_config_with_info(
    const char *runtime_dir, const char *sdcard_root, const char *core_path,
    const char *explicit_info_dir, const int player_joypad_indices[4],
    bool persist_changes, bool proxied_cheevos,
    const jw_retroarch_frontend_profile *frontend,
    char *error, size_t error_size);

/* RAOfflineProxy transient launch bridge (umrk-workspace/plans/RAOfflineProxy).
 * When a game launch is routed through the offline-achievements proxy, the
 * daemon injects cheevos_custom_host/cheevos_hardcore_mode_enable only into
 * the owner-only per-launch config and snapshots the exact prior shared lines
 * (including absence) here. The snapshot rides into the post-exit backup,
 * which restores the prior lines byte-identically instead of persisting the
 * injected runtime values. proxied=false marks an ordinary direct launch and
 * makes the snapshot a strict no-op everywhere. */
#define JW_RETROARCH_SNAPSHOT_LINE_MAX 512
typedef struct {
    bool proxied;
    bool custom_host_present;
    char custom_host_line[JW_RETROARCH_SNAPSHOT_LINE_MAX];
    bool hardcore_present;
    char hardcore_line[JW_RETROARCH_SNAPSHOT_LINE_MAX];
} jw_retroarch_launch_snapshot;

void jw_retroarch_launch_snapshot_init(jw_retroarch_launch_snapshot *snapshot);
/* Capture the exact shared-config lines for cheevos_custom_host and
 * cheevos_hardcore_mode_enable (presence included). Does not mark the launch
 * proxied; the caller sets proxied only once routing actually chose the
 * proxy. */
void jw_retroarch_launch_snapshot_capture(jw_retroarch_launch_snapshot *snapshot,
                                           const char *shared_text);
/* Durable shared Hardcore flag. Such launches always go direct.
 *
 * Fails CLOSED: true when the shared config says "true", and also true when
 * the config exists but cannot be read. A read failure must not be reported as
 * "Hardcore off", because that routes a hardcore session through the
 * casual-only proxy and quietly earns its achievements as casual. Going direct
 * when we cannot tell costs a casual user offline achievements for that
 * launch; guessing the other way costs a hardcore user their run. */
bool jw_retroarch_shared_hardcore_enabled(const char *sdcard_root);

typedef enum {
    JW_SHARED_CFG_OK = 0,       /* text loaded; caller frees */
    JW_SHARED_CFG_ABSENT,       /* no shared config on the card */
    JW_SHARED_CFG_UNREADABLE,   /* it exists, but could not be read */
} jw_shared_config_status;

/* Heap copy of the shared RetroArch config text.
 *
 * Absent and unreadable are reported separately because they demand opposite
 * responses: a device with no shared config yet has no durable settings to
 * honor, while one whose config cannot be read has settings we must not
 * assume anything about. Callers that conflate them make the safe case and
 * the dangerous case look identical. */
char *jw_retroarch_shared_config_read_status(const char *sdcard_root,
                                             jw_shared_config_status *status);
/* Convenience wrapper for callers that genuinely do not care why: NULL for
 * both absent and unreadable. */
char *jw_retroarch_shared_config_read(const char *sdcard_root);

int jw_backup_retroarch_config(const char *runtime_config_path, const char *sdcard_root,
                               const jw_retroarch_launch_snapshot *snapshot,
                               char *error, size_t error_size);
int jw_reset_retroarch_shared_config(const char *sdcard_root,
                                     char *status, size_t status_size);
char *jw_retroarch_state_dir(const char *sdcard_root);
/* The one primary-owned gameplay-capture directory. Callers must use this
 * instead of composing their own Recordings path so RetroArch, post-processing,
 * and child apps cannot diverge when the active content card is secondary. */
bool jw_primary_recordings_path(char *out, size_t out_size,
                                const char *primary_sdcard_root);
/* Pin the emulated controller type for cores whose default pad lacks hardware the
 * user expects -- today only PS1, whose rumble needs a DualShock rather than the
 * digital pad RetroArch hands it. No-op for every other core. Best effort: a
 * failure costs that hardware, never the launch. See paths.c for why this cannot
 * live in retroarch.cfg. */
void jw_retroarch_pin_core_device(const char *ra_home, const char *core_id,
                                  const char *core_config_folder,
                                  const char *rom_path);

/* Install the per-core option profile a core needs before its first launch --
 * today only FlyCast Fast UMRK, whose tuned settings must be on disk before the
 * core initializes. Copies the release template under
 * $UMRK_PLATFORM_PATH/defaults/retroarch/core-options/<folder>/<folder>.opt to
 * the effective RetroArch config dir, and only while the core's .opt file is
 * absent; an existing file is never rewritten. A versioned stamp beside
 * ra_home records completion, so a deliberate later deletion stays deleted.
 * Returns 0 when the file is in place or the core is not seeded, -1 on failure
 * with a message in error. See paths.c for the ownership rules. */
int jw_retroarch_seed_core_options(const char *ra_home, const char *sdcard_root,
                                   const char *core_id,
                                   const char *core_config_folder,
                                   char *error, size_t error_size);

#endif
