#include "internal/platform/power_request.h"
#include "cJSON.h"
#include "internal/core/log.h"
#include "internal/db/db.h"
#include "internal/db/relocation.h"
#include "internal/discovery/discovery.h"
#include "internal/storage/health.h"
#include "internal/focus/focus.h"
#include "internal/ipc/ctl1.h"
#include "internal/ipc/ipc.h"
#include "internal/ipc/ipc_stream.h"
#include "internal/ipc/life1.h"
#include "internal/launcher/active_game.h"
#include "internal/launcher/bios.h"
#include "internal/launcher/core_selection.h"
#include "internal/launcher/standalone_policy.h"
#include "internal/launcher/ra_account.h"
#include "internal/launcher/menu_escape.h"
#include "internal/launcher/pico8.h"
#include "cmd/jawakad/osd_client.h"
#include "internal/platform/external_input_monitor.h"
#include "internal/platform/bluetooth.h"
#include "internal/platform/device.h"
#include "internal/platform/input_proxy.h"
#include "internal/i18n/i18n.h"
#include "internal/platform/input_shortcuts.h"
#include "internal/platform/input_roster.h"
#include "internal/platform/ledd_spawn.h"
#include "internal/platform/paths.h"
#include "internal/platform/perf_policy.h"
#include "internal/platform/raofflineproxy.h"
#include "internal/platform/weston_initd.h"
#include "internal/platform/wifi.h"
#include "internal/power/boot_resume.h"
#include "internal/power/power_hold_save.h"
#include "internal/power/power_hold_save_io.h"
#include "internal/power/power_hard_cut.h"
#include "internal/power/suspend_inhibit.h"
#include "internal/retroarch/command.h"
#include "internal/catalog/effective.h"
#include "internal/retroarch/catalog.h"
#include "internal/retroarch/legacy_migration.h"
#include "internal/retroarch/states.h"
#include "internal/scrape/scrape_worker.h"
#include "internal/discovery/delete_job.h"
#include "internal/scrape/ss_client.h"
#include "internal/services/log_redact.h"
#include "internal/services/launch.h"
#include "internal/services/ownership.h"
#include "internal/services/supervisor.h"
#include "internal/settings/appearance.h"
#include "internal/storage/sources.h"
#include "internal/store/pakrat_recovery.h"
#include "internal/store/pakrat_txn.h"
#include "internal/update/update.h"

#include "miniz.h"   /* tdefl_write_image_to_png_file_in_memory (screenshot encode) */

#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* CLOCK_BOOTTIME (counts time spent suspended) is Linux-only. The native/mock
   build never suspends, so fall back to CLOCK_MONOTONIC there: the boot-minus-
   monotonic gap is then always ~0 ("never suspended"), which is the correct
   off-device behavior and keeps the resume-detection code portable. */
#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif

#define JW_SWITCHER_RESUME_RETRY_MS 100LL
#define JW_SWITCHER_RESUME_MAX_ATTEMPTS 40
/* LEAF-RA-1 probe: GET_INFO every 250 ms from one second after spawn, for up
   to 20 s. A 32-bit frontend loading a disc image can take several seconds to
   open its command port. */
#define JW_FRONTEND_PROBE_FIRST_MS 1000LL
#define JW_FRONTEND_PROBE_RETRY_MS 250LL
#define JW_FRONTEND_PROBE_MAX_ATTEMPTS 76
/* VERSION after the probe: RetroArch answers once per frame, and a frontend
   still loading its content can miss a 100 ms window. */
#define JW_FRONTEND_VERSION_TIMEOUT_MS 250u
#define JW_FRONTEND_VERSION_MAX_ATTEMPTS 8
/* The load-state command does real work (RA reads + applies the state) and can
   need longer than the readiness probe, especially in the first second of boot.
   Generous enough to catch a slow reply, short enough not to stall the daemon. */
#define JW_SWITCHER_RESUME_LOAD_TIMEOUT_MS 400u
#define JW_INGAME_MENU_PREWARM_DELAY_MS 1200LL
#define JW_INGAME_MENU_PREWARM_AFTER_RESUME_MS 250LL
#define JW_RETROARCH_AUDIO_REINIT_RETRY_MS 500LL
#define JW_RETROARCH_AUDIO_REINIT_TIMEOUT_MS 10000LL
#define JW_RETROARCH_QUIT_GRACE_MS 700LL
#define JW_RETROARCH_KILL_GRACE_MS 700LL
/* platform-levels answers from the brightness/volume cache. jawakad writes
   both values itself and caches what it wrote, so the cache is only re-read
   from the hardware this often, which bounds how long a change jawakad did not
   make (a Bluetooth headset's own buttons) can go unseen. An unknown value is
   retried sooner. */
#define JW_PLATFORM_LEVELS_TTL_MS 5000LL
#define JW_PLATFORM_LEVELS_RETRY_MS 1000LL
/* A quit the menu asked for that RetroArch never honors, seen when a savestate
   write stalls on a read-only card: it then ignores SIGTERM as well. */
#define JW_RETROARCH_STUCK_QUIT_MS 15000LL
#define JW_RESIDENT_SWITCH_MAX_DEFAULT (-1)
#define JW_RESIDENT_SWITCH_MAX_DEFAULT_LABEL "unlimited"
#define JW_PERF_SETTING_KEY "platform.performance.game_profile"
#define JW_CONTENT_SETTING_CORE_ID "core_id"
#define JW_CONTENT_SETTING_PERFORMANCE_PROFILE "performance_profile"
#define JW_STARTUP_MAINT_GRACE_MS 500LL    /* after frontend-ready */
#define JW_STARTUP_MAINT_FALLBACK_MS 15000LL /* if frontend-ready never arrives */

/* What library.db looked like on disk the last time a reader acted on it.
   Idle readers compare this instead of opening the database: an open applies
   the schema and takes ~60 fcntl locks, which on a 500 ms timer was nearly
   all of the daemon's idle file I/O. */
typedef struct {
    bool valid;
    long long db[4];        /* ino, size, mtime ns, ctime ns */
    long long journal[2];   /* size, mtime ns (-1 when absent) */
    long long wal[2];
} jw_db_file_sig;

typedef enum {
    JW_CHILD_NONE = 0,
    JW_CHILD_LAUNCHER,
    JW_CHILD_MENU,
    JW_CHILD_RETROARCH,
    JW_CHILD_EMULATOR,
    JW_CHILD_APP,
    /* The RetroArch.pak runner. A generic .pak app is killed on shutdown; this
       one is asked to stop and given time to promote RetroArch's settings back
       to the durable shared config first (see the runner's QUIT path). Kept
       distinct so no other app inherits that grace. */
    JW_CHILD_RETROARCH_APP,
    /* Splore can have a downloader child; reap only after its group clears. */
    JW_CHILD_PICO8_APP,
    JW_CHILD_PICO8_IMPORT
} jw_child_kind;

static bool jw__child_kind_is_app(jw_child_kind kind);

typedef enum {
    JW_LAUNCH_TARGET_NONE = 0,
    JW_LAUNCH_TARGET_RETROARCH,
    JW_LAUNCH_TARGET_STANDALONE
} jw_launch_target_kind;

typedef enum {
    JW_STANDBY_NONE = 0,
    JW_STANDBY_AUTOSLEEP,
    JW_STANDBY_POWER_CHARGING,
    JW_STANDBY_INHIBITED_POWER
} jw_standby_reason;

typedef struct {
    bool active;
    pid_t pid;
    int game_id;
    time_t started_at;
    char system[64];
    char rom_path[PATH_MAX];
    char db_rom_path[PATH_MAX];
    char source_root[PATH_MAX];
    char core_path[PATH_MAX];
    char core_id[64];
    /* Standalone sessions only: the launch target's resolved policy and the
       catalog core's menu capability. Routing reads these, never core_id or
       core_path. Zero (release NONE, not provider-bound) for RetroArch. */
    jw_standalone_policy standalone_policy;
    bool supports_menu;
    char core_config_folder[256];
    char config_path[PATH_MAX];
    /* RAOfflineProxy transient launch bridge: proxied sessions carry the
       exact pre-launch shared lines so the exit-time backup restores them
       instead of persisting the injected proxy overrides. Zeroed by
       session clear; proxied=false means ordinary direct launch. */
    jw_retroarch_launch_snapshot config_snapshot;
    int resident_switches;
    bool persist_config;
    bool audio_bluetooth;
    bool warning_pending;
    int warning_attempts;
    long long warning_next_ms;
    char warning[256];
    /* What a power-hold save arms Resume Game on Boot with, pinned when the
       RetroArch session starts so arming reads no database and no card.
       Empty fingerprint: the card has no persistent identity, so no resume. */
    char source_fingerprint[JW_BOOT_RESUME_FINGERPRINT_MAX];
    char rom_relpath[JW_BOOT_RESUME_ROM_PATH_MAX];
    char states_path[PATH_MAX];
    char provider[JW_BOOT_RESUME_PROVIDER_MAX];
    /* A boot-resume session until LOAD_STATE_SYNC answers OK: no play is
       recorded for it, since the player never got their game back. */
    bool resume_unconfirmed;
    /* LEAF-RA-1: a content pak's own RetroArch runs in place of Leaf's.
       frontend_probe is 0 until known, 1 once it answered GET_INFO (it speaks
       Leaf's command set, so the in-game menu may pause it), -1 when it never
       did (Menu then opens its own menu). frontend_version_ok: its VERSION is
       Leaf's RetroArch version, the only case its config is promoted into the
       shared one. */
    bool frontend;
    int frontend_probe;
    int frontend_probe_attempts;
    long long frontend_probe_next_ms;
    bool frontend_version_done;
    int frontend_version_attempts;
    bool frontend_version_ok;
} jw_retroarch_session;

/* Resume Game on Boot, from the startup request to the one LOAD_STATE_SYNC
   reply. The plan's rule: the saved state loads, or RetroArch quits and the
   launcher opens with a notice. Never a retry, another slot or a cold start. */
typedef enum {
    JW_BOOT_RESUME_PHASE_NONE = 0,
    /* The save went through RAOfflineProxy: hold the launch until that
       service is up, so the game starts casual again and can take its state. */
    JW_BOOT_RESUME_PHASE_WAITING_PROXY,
    JW_BOOT_RESUME_PHASE_LAUNCHING,  /* requested; RetroArch not spawned yet */
    JW_BOOT_RESUME_PHASE_PROBING,    /* spawned; waiting for its command port */
    JW_BOOT_RESUME_PHASE_LOADING,    /* LOAD_STATE_SYNC sent; one reply pending */
} jw_boot_resume_phase;

typedef struct {
    jw_boot_resume_phase phase;
    int game_id;
    char core_id[JW_BOOT_RESUME_CORE_ID_MAX];
    char core_config_folder[JW_BOOT_RESUME_CORE_FOLDER_MAX];
    char provider[JW_BOOT_RESUME_PROVIDER_MAX];
    char source_fingerprint[JW_BOOT_RESUME_FINGERPRINT_MAX];
    char state_path[PATH_MAX];       /* absolute; checked again before spawn */
    char request_id[JW_BOOT_RESUME_REQUEST_ID_MAX];
    char system[JW_BOOT_RESUME_SYSTEM_MAX];
    char db_rom_path[PATH_MAX];      /* the library row's own key */
    char rom_abs[PATH_MAX];          /* the ROM on the card as mounted this boot */
    unsigned long long state_bytes;
    bool offline_proxy;              /* must launch through RAOfflineProxy */
    int attempts;
    int silent_probes;               /* state probes unanswered in a row */
    long long next_ms;
    long long deadline_ms;
    long long started_ms;
    jw_ra_sync_save load;            /* the one LOAD_STATE_SYNC exchange */
} jw_boot_resume_launch;

typedef struct {
    jw_launch_target_kind kind;
    char path[PATH_MAX];
    char core_id[64];
    char core_config_folder[256];
    /* RetroArch targets: the merged info directory of the catalog snapshot
       the core was selected from. */
    char info_dir[PATH_MAX];
    /* RetroArch targets: the pak's LEAF-RA-1 frontend, launched in place of
       Leaf's own RetroArch; "" for every other core. */
    char frontend[PATH_MAX];
    /* Standalone targets only, resolved once from the catalog core by
       jw__try_path_core() before any eligibility check. */
    jw_standalone_policy standalone_policy;
    /* Catalog provenance (Apps-relative, "" for release cores), retained for
       account-target authorization; policy embeds no provider string. */
    char provider[128];
    bool supports_menu;
    bool requires_direct_drm;
    bool native_pico8;
    char diagnostic[256];
} jw_launch_target;

#define JW_SCAN_TITLE_PROVIDER_MAX 96
#define JW_SCAN_TITLE_MAX 256

typedef struct {
    char   provider[JW_SCAN_TITLE_PROVIDER_MAX];
    char   title[JW_SCAN_TITLE_MAX];
    char **rom_paths;
    int    rom_path_count;
} jw_scan_title_group;

typedef struct {
    jw_scan_title_group *groups;
    int                  group_count;
} jw_scan_title_list;

typedef struct {
    pthread_t       thread;
    pthread_mutex_t mu;
    bool            initialized;
    bool            thread_started;
    bool            running;
    bool            completed;
    bool            ok;
    bool            pending_rescan;
    char            reason[96];
    char            pending_reason[96];
    char            db_path[PATH_MAX];
    char            sdcard_root[PATH_MAX];
    long long       started_ms;
    long long       finished_ms;
    jw_scan_result  result;
    jw_scan_title_list titles;
    jw_scan_title_list pending_titles;
    jw_db_imported_title_result title_result;
    char            title_error[160];
    char            error[160];
} jw_scan_job;

#define JW_DAEMON_IPC_CONNECTION_MAX 32

typedef enum {
    JW_GAME_EXCHANGE_NONE = 0,
    JW_GAME_EXCHANGE_AWAITING,
    JW_GAME_EXCHANGE_DECISION,
    JW_GAME_EXCHANGE_WAITING,
    JW_GAME_EXCHANGE_ACK,
} jw_game_exchange_phase;

typedef enum {
    JW_GAME_EXCHANGE_KIND_NONE = 0,
    JW_GAME_EXCHANGE_KIND_NOTIFY,
    JW_GAME_EXCHANGE_KIND_CHECK,
} jw_game_exchange_kind;

typedef struct {
    jw_ipc_stream *stream;
    bool subscribed;
    bool reconciled;
    bool close_after_flush;
    bool stop_after_flush;
    char service_id[JW_SVC_SUPERVISOR_ID_BUF];
    jw_life1_mode mode;
    int ack_ms;
    int wait_ms;
    bool check_before_stop;
    jw_svc_subscriber_binding binding;
    jw_game_exchange_kind exchange_kind;
    jw_game_exchange_phase exchange_phase;
    long long exchange_started_ms;
    long long exchange_wait_deadline_ms;
    long long exchange_ack_deadline_ms;
    long long exchange_total_deadline_ms;
    bool exchange_pending_seen;
    int exchange_pending_items;
    int64_t exchange_pending_bytes;
} jw_daemon_ipc_connection;

typedef struct {
    char *runtime_dir;
    char *sdcard_root;
    char *socket_path;
    char *osd_socket_path;
    char *db_path;
    char *state_dir;
    char  bin_dir[PATH_MAX];
    sqlite3 *db;
    jw_ipc_server *server;
    jw_daemon_ipc_connection ipc_connections[JW_DAEMON_IPC_CONNECTION_MAX];
    jw_delete_job *delete_job;
    int delete_connection;
    bool delete_pending_scan;
    jw_platform_context platform;
    jw_input_proxy input_proxy;
    jw_input_menu_config session_menu_config;
    uint64_t menu_session_generation;
    jw_menu_escape menu_escape;
    /* Which button performs which Leaf action, resolved once and held in
       memory. The input path reads this and only this -- see
       jw__on_shortcut_chord() for why it must never reach SQLite.
       Phase 6 loads it from the settings table at startup and replaces it on
       an IPC push from Settings; until then it is the compiled-in defaults,
       which are the fixed chords this feature replaces. */
    jw_input_shortcuts shortcuts;
    /* Non-grabbing observers for paired wireless controllers: activity feeds
       the same idle clock the proxy drives; Guide equals the Menu button. */
    jw_external_input_monitor external_input;
    pid_t child_pid;          /* foreground launcher, normal menu, RetroArch, or app */
    pid_t child_pgid;         /* reserved writer group for RETROARCH/EMULATOR and
                                 the RetroArch app runner; -1 for foreground
                                 paths with no group of their own */
    jw_child_kind child_kind;
    /* Stop grace bookkeeping for JW_CHILD_RETROARCH_APP: the shutdown branch
       runs every loop pass, so the SIGTERM must be sent once and the deadline
       remembered rather than re-signalled and re-slept each time. */
    bool     child_stop_signalled;
    long long child_stop_deadline_ms;
    /* When the group-absence wait for an exited-but-not-absent group began, so
       the escalation to SIGKILL is bounded. */
    long long child_group_wait_started_ms;
    /* Rate limit for the "survived SIGKILL" error, which is re-evaluated on
       every daemon tick for as long as the group refuses to clear. */
    long long child_group_kill_logged_ms;
    /* Per-launch private /dev/input view (plans/paired-wireless-controllers-mlp1):
       /run/jawaka/input-<child-pid>, removed when the child exits. Empty when
       the child launched without input isolation (desktop, generic apps). */
    char    child_input_ns_dir[PATH_MAX];
    uint64_t last_screenshot_ms;    /* debounce for the Menu+L1 screenshot hotkey */
    uint64_t last_record_toggle_ms; /* debounce for the Menu+R1 record hotkey */
    /* Capture opt-ins. Loaded once at startup and replaced by the Settings IPC
       push, exactly like the bindings beside them -- never polled. The input
       callback reads these and never the database. */
    bool     screenshots_enabled_cached;
    bool     recording_enabled_cached;
    /* Rumble/haptics settings, TTL-cached like the screenshot flag (read on the
       input tick, so avoid a sqlite open per event). */
    bool     rumble_ui_cached;           /* every interface buzz, cursor ticks included */
    int      rumble_strength_cached;     /* 0-100 %  */
    bool     rumble_game_cached;         /* hand the motor to emulators in-game */
    uint64_t rumble_checked_ms;
    pid_t menu_pid;           /* resident warm-standby in-game menu while RetroArch is alive */
    bool menu_in_game;
    bool menu_visible;        /* standby menu is currently shown (RetroArch paused under it) */
    int menu_standby_attempts;/* respawn guard for a crashing standby within one session */
    bool advanced_shader_pending;   /* Track direct entry into RetroArch Shaders until close */
    bool advanced_shader_menu_seen; /* RetroArch's initial menu surface was observed */
    bool advanced_shader_destination_sent; /* Shaders was requested after menu initialization */
    long long advanced_shader_started_ms;
    long long advanced_shader_next_poll_ms;
    long long standalone_quit_request_ms; /* Menu-tap quit sent to a standalone
                                 emulator without a native menu signal (0 = none);
                                 a second tap after the grace period escalates
                                 to SIGKILL */
    long long pico8_exit_confirm_until_ms;
    long long pico8_quit_request_ms;
    bool retroarch_resume_on_menu_exit;
    pid_t osd_pid;
    int osd_ready_fd;           /* readiness pipe from the OSD, -1 when none */
    bool osd_ready;             /* fonts, backend and socket are all up */
    long long osd_spawned_ms;
    /* The appearance the running OSD opened its font with. */
    char osd_language[JW_I18N_CODE_MAX];
    char osd_font_path[256];
    char osd_font_bump[16];
    bool direct_drm_active;
    bool direct_drm_weston_stopped;
    pid_t ledd_pid;            /* jawaka-ledd custom LED effect engine, -1 when idle */
    long long ledd_started_ms; /* monotonic start of the running ledd */
    jw_ledd_restarts ledd_restarts;
    int cached_brightness_percent;
    int cached_volume_percent;
    long long platform_cache_read_ms;  /* last full hardware read, 0 = never */
    long long audio_reconcile_last_ms;
    jw_led_config cached_led;
    bool led_configured;       /* true once a user LED setting has been persisted/applied */
    jw_retroarch_session retroarch_session;
    int library_generation;
    bool library_populated;
    jw_scan_job scan_job;
    /* Startup maintenance (wifi restore/harden + library scan) deferred past
       the launcher's first frame so it doesn't sit between boot animation and
       launcher; phase 0 = wifi, phase 1 = scan, fired on separate loop
       iterations so a queued launcher IPC poll can drain between them. */
    bool startup_maintenance_pending;
    int startup_maintenance_phase;
    long long startup_maintenance_next_ms;
    bool library_scanned_since_boot;
    bool pending_menu;
    bool pending_launch;
    int pending_launch_game_id;
    char pending_launch_system[64];
    char pending_launch_rom_path[PATH_MAX];
    char pending_launch_core_id[64];
    /* This launch's one core selection, made when the request is accepted and
       reused through coordination, continuation and spawn. An owned copy with
       no catalog pointers. Kept while the launch is pending or blocked for an
       override; dropped when it is cancelled, fails, starts or is replaced. */
    jw_launch_target pending_launch_target;
    bool pending_launch_target_valid;
    bool pending_launch_resume_switcher;
    /* Standalone emulator asked (via Menu+Select marker) to reopen the launcher
       straight into the switcher carousel, seeded on the just-exited game. */
    bool launcher_open_switcher;
    char launcher_switcher_system[64];
    char launcher_switcher_rom[PATH_MAX];
    /* A standalone session that ended non-zero within seconds of starting is
       the "wrapper died before the emulator appeared" signature -- the Leaf
       0.11 black flash. The respawned launcher surfaces it in the status line
       instead of coming back silently. */
    bool launch_input_failed; /* surfaced by the next launcher */
    bool launch_status_pending;
    char launch_status_name[64];
    int launch_status_code;
    bool post_launch_resume_pending;
    int post_launch_resume_attempts;
    long long post_launch_resume_next_ms;
    bool in_game_menu_prewarm_pending;
    long long in_game_menu_prewarm_next_ms;
    /* Deferred motor reclaim after opening an in-game surface: RetroArch is
       told to pause over a fire-and-forget UDP command, so it can still run a
       frame or two and re-assert rumble after we force the motor off. */
    long long rumble_reclaim_ms;
    bool retroarch_audio_reinit_pending;
    long long retroarch_audio_reinit_next_ms;
    long long retroarch_audio_reinit_deadline_ms;
    char retroarch_audio_reinit_reason[64];
    bool pending_app;
    char pending_app_pak_dir[PATH_MAX];
    bool daemon_only;
    bool shutdown_requested;
    bool power_transition_requested;
    jw_platform_action power_transition_action;
    /* Auto-sleep: idle → screen off (bl_power) → suspend (mem). */
    int       autosleep_timeout_s;        /* cached from DB; 0 = disabled */
    int       autosleep_platform_synced_s;/* last value mirrored to stock power policy */
    long long autosleep_setting_next_ms;  /* throttle for re-reading the DB setting */
    /* Force Off Hold (PMIC power-button cut), read in the same DB poll as
       auto-sleep and applied through the platform backend. */
    int       power_hard_cut_desired_s;   /* from the DB; the 10 s default when unset */
    int       power_hard_cut_applied_s;   /* last value handed to the platform; 0 = never */
    int       power_hard_cut_effective_s; /* hold believed in force: OK -> value, failed -> stock 6 s,
                                             0 = unknown/unsupported. For policies that wait on
                                             the button (power-hold save). */
    jw_standby_reason standby_reason;     /* screen-off standby state, or NONE when lit */
    long long standby_entered_ms;         /* monotonic ms for wake-input detection */
    bool      autosleep_charging_logged;  /* log the charging hold once per standby */
    bool      autosleep_screen_inhibit_logged; /* log the screen-lease hold once per hold */
    bool      autosleep_screen_inhibit_held;   /* a screen lease was held on the last tick */
    int       charging_cached;            /* -1 unknown, 0 unplugged, 1 charging */
    long long charging_next_poll_ms;       /* throttle platform status reads in standby */
    bool      power_sleep_armed;          /* power pressed while screen on → sleep on release */
    bool      power_held;                 /* power key currently held (for long-press detect) */
    long long power_down_ms;              /* when the current power press started */
    long long last_resume_ms;             /* monotonic ms of the latest detected resume */
    /* Save before power-button shutdown: on an opt-in RetroArch session, a
       long press waits for the release, then RetroArch writes a fsync'd
       temporary state that jawakad renames over the switcher slot before
       child teardown. Policy: internal/power/power_hold_save.h. */
    bool power_hold_save_enabled;         /* cached setting; Off unless refreshed */
    bool power_hold_settings_unread;      /* the last poll found the library busy */
    jw_power_hold_save power_hold_save;
    jw_ra_sync_save power_hold_save_request;
    unsigned long long power_hold_save_bytes;   /* probed size, rounded up to a MiB */
    long long power_hold_save_started_ms;       /* long press recognized */
    char power_hold_save_final[PATH_MAX];       /* pinned slot path */
    char power_hold_save_thumb[PATH_MAX];
    char power_hold_save_tmp[PATH_MAX];         /* pinned temporary path */
    bool power_hold_save_tmp_owned;             /* sent, not yet published or removed */
    bool power_hold_save_reported;              /* outcome logged */
    int  power_hold_save_release_notice_tries;  /* OSD prompt attempts (max 3) */
    bool power_hold_save_release_notice_ok;     /* OSD acknowledged the prompt */
    long long power_hold_save_release_notice_next_ms;
    /* Saved outcome only: QUIT sent at this time (0 = not sent) and the bound
       on waiting for RetroArch's own exit before the kill sequence. */
    long long power_hold_save_quit_sent_ms;
    long long power_hold_save_quit_deadline_ms;
    bool power_hold_save_quit_expired;          /* grace over or QUIT failed: kill */
    /* Host-build test hooks (scripts/power-hold-save-ipc-smoke.sh), read once
       at start-up and never compiled into the MLP1 daemon: a Force Off Hold
       the mock platform cannot apply, and the platform gate opened so the
       mock can stand in for the MLP1. */
    int  test_power_hard_cut_s;                 /* 0 = none */
    bool test_power_hold_save_any_platform;
    /* Resume Game on Boot: cached like the save setting, the boot this daemon
       runs in (read once; "" when unknown), the resume in flight, and the
       failure notice the next launcher shows. */
    bool boot_resume_enabled;                   /* cached; Off unless refreshed */
    char boot_id[JW_BOOT_RESUME_BOOT_ID_MAX];
    jw_boot_resume_launch boot_resume;
    bool boot_resume_notice_pending;
    /* The pending notice says Hardcore refused the load, so the player knows
       what to turn off. */
    bool boot_resume_notice_hardcore;
    /* Host-build test hook (scripts/boot-resume-ipc-smoke.sh): the mock card
       has no uuid:/fat: identity, so the primary source reads as this one. */
    char test_source_fingerprint[JW_BOOT_RESUME_FINGERPRINT_MAX];
    jw_suspend_inhibitor suspend_inhibitor;
    jw_suspend_policy    suspend_policy;
    int       hdmi_last_connected;        /* -1 unknown, 0/1; for hotplug edge detection */
    long long hdmi_next_poll_ms;          /* throttle for the HDMI hotplug poll */
    long long lut_sync_not_before_ms;     /* 0 = idle; else swap the color-temp LUT for the current output once this passes */
    long long lut_sync_deadline_ms;       /* ...and give up if it still fails after this */
    bool      lut_sync_external;          /* the output to swap for: true = external display */
    long long hdmi_revert_deadline_ms;    /* 0 = none; auto-revert 1080p120 if not kept */
    int       hdmi_was_120;               /* live-1080p120 edge tracker */
    jw_platform_perf_profile perf_global_profile;
    jw_platform_perf_profile perf_active_profile;
    jw_platform_perf_profile perf_session_profile;
    bool perf_session_override;
    bool perf_custom_valid;
    jw_platform_perf_request perf_custom_request;
    char perf_last_error[JW_PLATFORM_MAX_MESSAGE];
    jw_update_status update_status;
    jw_update_download_job update_download_job;
    jw_update_install_job update_install_job;
    jw_update_check_job update_check_job;
    /* Direct/generic update runners replace live package trees. PKG-1 keeps
       the service barrier held until the runner exits and the replacement
       manifests have been rescanned. Stock reboot handoffs do not use it. */
    bool update_package_quiesce_active;
    bool update_package_quiesce_release_warned;
    /* 5-Game Mode (focus mode): resolved once at boot from the persisted config
       + the SD recovery lock file. Later phases render the focus screen and gate
       the launcher off this. See
       umrk-workspace/plans/Jawaka/five-game-mode.md. */
    jw_focus_boot_decision focus_boot;
    jw_focus_config        focus_cfg;
    /* app-services-v1 (SVC-1) service supervisor; NULL when it could not be
       opened (a supervisor failure must never take down the launcher). */
    jw_svc_supervisor *services;
    /* When the Pak Rat client dies inside TXN-1, the daemon adopts the now-
       available flock and either completes a confirmed uninstall forward or
       runs P1 recovery before releasing the supervisor's target gate. */
    jw_pakrat_mutation_lock mutation_recovery_lock;
    unsigned mutation_recovery_sequence;
    long long mutation_recovery_next_ms;
    bool pakrat_startup_recovery_needed;
    /* LIFE-1's authoritative runtime record. It is loaded before service
       scan/autostart and remains active+uncertain on a corrupt recovery. */
    jw_active_game active_game;
    bool active_game_writer_started;
    bool game_coordination_pending;
    bool game_coordination_ready;
    int game_coordination_exchanges;
    long long game_launch_started_ms;
    /* A stop/stale reservation that cannot be proved absent is fail-closed,
       but not silent. Preserve the requested game until the respawned
       launcher explicitly cancels it or asks to launch despite the named
       possible writer. */
    bool game_launch_blocked;
    bool game_launch_blocked_resume_switcher;
    bool pending_launch_override_unverified;
    bool pending_launch_skip_check;
    /* RAOfflineProxy routing outcome for the current pending RetroArch
       launch (proxied=false for direct/standalone/non-gated launches). */
    jw_retroarch_launch_snapshot pending_rop_snapshot;
    bool game_launch_blocked_requires_verified_stop;
    bool game_check_decision;
    int game_check_pending_items;
    int64_t game_check_pending_bytes;
    char game_check_service_id[JW_SVC_SUPERVISOR_ID_BUF];
    char game_launch_blocked_service_id[JW_SVC_SUPERVISOR_ID_BUF];
    char game_launch_blocked_reason[JW_SVC_REASON_BUF];
    /* Set when a safe-unmount already applied the storage-change policy, so
       the storage tick it provokes does not stop the same services again. */
    bool services_storage_stop_done;
    long long retroarch_quit_deadline_ms;   /* 0 = no quit outstanding */
    /* SD card health: read-only detection, warning acknowledgement, and the
       reboot repair request. Refreshed on the main loop at most every 2 s. */
    jw_storage_health_monitor storage_monitor;
    long long storage_next_probe_ms;
    bool storage_db_read_only;       /* library DB was not writable at startup */
    bool storage_scan_deferred;      /* a scan waits for the DB to be writable */
    bool storage_repair_rescan_done; /* post-repair rescan considered this run */
    bool storage_log_redirected;
    jw_db_file_sig mutation_recovery_db_sig;  /* library.db when no uninstall was pending */
    jw_db_file_sig autosleep_db_sig;          /* library.db when the setting was read */
    /* The running launcher said in its hello that it handles SIGUSR2 as
       "re-read platform-levels": the volume changed or an HDMI revert was
       armed (see jw__notify_launcher_levels). */
    bool launcher_levels_signal;
    bool reap_by_signal;   /* the SIGCHLD pipe is installed */
    bool sigchld_seen;     /* a SIGCHLD arrived since the last exit check */
} jw_daemon_state;

static void jw__scan_title_list_free(jw_scan_title_list *list) {
    if (!list) return;
    for (int i = 0; i < list->group_count; i++) {
        for (int p = 0; p < list->groups[i].rom_path_count; p++) {
            free(list->groups[i].rom_paths[p]);
        }
        free(list->groups[i].rom_paths);
    }
    free(list->groups);
    memset(list, 0, sizeof(*list));
}

static size_t jw__scan_title_list_bytes(const jw_scan_title_list *list) {
    /* This is a conservative serialized-JSON upper bound, not merely the
       allocation size. A quoted byte can expand to a six-byte \u00xx escape;
       structural overhead is charged per group/path. This keeps merged queued
       requests inside the same cap as one IPC frame. */
    size_t total = 32u;
    if (!list) return 0;
    for (int i = 0; i < list->group_count; i++) {
        const jw_scan_title_group *group = &list->groups[i];
        size_t provider_len = strlen(group->provider);
        size_t title_len = strlen(group->title);
        if (total > SIZE_MAX - 64u) return SIZE_MAX;
        if (provider_len > (SIZE_MAX - total - 64u) / 6u) return SIZE_MAX;
        total += provider_len * 6u + 64u;
        if (title_len > (SIZE_MAX - total) / 6u) return SIZE_MAX;
        total += title_len * 6u;
        for (int p = 0; p < group->rom_path_count; p++) {
            size_t path_len = strlen(group->rom_paths[p]);
            if (total > SIZE_MAX - 8u) return SIZE_MAX;
            if (path_len > (SIZE_MAX - total - 8u) / 6u) return SIZE_MAX;
            total += path_len * 6u + 8u;
        }
    }
    return total;
}

static int jw__scan_title_list_path_count(const jw_scan_title_list *list) {
    int total = 0;
    if (!list) return 0;
    for (int i = 0; i < list->group_count; i++) {
        if (list->groups[i].rom_path_count > INT_MAX - total) return INT_MAX;
        total += list->groups[i].rom_path_count;
    }
    return total;
}

static int jw__scan_title_list_append(jw_scan_title_list *list,
                                      const jw_scan_title_group *source) {
    if (!list || !source || !source->provider[0] || !source->title[0] ||
        !source->rom_paths || source->rom_path_count <= 0) {
        return -1;
    }
    jw_scan_title_group *groups = realloc(
        list->groups, (size_t)(list->group_count + 1) * sizeof(*groups));
    if (!groups) return -1;
    list->groups = groups;
    jw_scan_title_group *dest = &list->groups[list->group_count];
    memset(dest, 0, sizeof(*dest));
    snprintf(dest->provider, sizeof(dest->provider), "%s", source->provider);
    snprintf(dest->title, sizeof(dest->title), "%s", source->title);
    dest->rom_paths = calloc((size_t)source->rom_path_count,
                             sizeof(*dest->rom_paths));
    if (!dest->rom_paths) return -1;
    for (int p = 0; p < source->rom_path_count; p++) {
        dest->rom_paths[p] = strdup(source->rom_paths[p]);
        if (!dest->rom_paths[p]) {
            for (int n = 0; n < p; n++) free(dest->rom_paths[n]);
            free(dest->rom_paths);
            memset(dest, 0, sizeof(*dest));
            return -1;
        }
        dest->rom_path_count++;
    }
    list->group_count++;
    return 0;
}

static void jw__scan_title_list_remove_path(jw_scan_title_list *list,
                                            const char *rom_path) {
    if (!list || !rom_path) return;
    for (int i = 0; i < list->group_count;) {
        jw_scan_title_group *group = &list->groups[i];
        for (int p = 0; p < group->rom_path_count;) {
            if (strcmp(group->rom_paths[p], rom_path) == 0) {
                free(group->rom_paths[p]);
                memmove(&group->rom_paths[p], &group->rom_paths[p + 1],
                        (size_t)(group->rom_path_count - p - 1) *
                            sizeof(*group->rom_paths));
                group->rom_path_count--;
                continue;
            }
            p++;
        }
        if (group->rom_path_count == 0) {
            free(group->rom_paths);
            memmove(&list->groups[i], &list->groups[i + 1],
                    (size_t)(list->group_count - i - 1) * sizeof(*list->groups));
            list->group_count--;
            continue;
        }
        i++;
    }
}

static int jw__scan_title_list_clone(jw_scan_title_list *dest,
                                     const jw_scan_title_list *source) {
    memset(dest, 0, sizeof(*dest));
    if (!source) return 0;
    for (int i = 0; i < source->group_count; i++) {
        if (jw__scan_title_list_append(dest, &source->groups[i]) != 0) {
            jw__scan_title_list_free(dest);
            return -1;
        }
    }
    return 0;
}

/* Merge by normalized ROM path. Later groups win, including when several scan
   requests arrive while another scan is already running. */
static int jw__scan_title_list_merge(jw_scan_title_list *dest,
                                     const jw_scan_title_list *source) {
    jw_scan_title_list merged;
    if (jw__scan_title_list_clone(&merged, dest) != 0) return -1;
    if (source) {
        for (int i = 0; i < source->group_count; i++) {
            const jw_scan_title_group *group = &source->groups[i];
            for (int p = 0; p < group->rom_path_count; p++) {
                jw__scan_title_list_remove_path(&merged, group->rom_paths[p]);
            }
            if (jw__scan_title_list_append(&merged, group) != 0) {
                jw__scan_title_list_free(&merged);
                return -1;
            }
        }
    }
    if (jw__scan_title_list_bytes(&merged) > JW_IPC_MAX_FRAME) {
        jw__scan_title_list_free(&merged);
        return -1;
    }
    jw__scan_title_list_free(dest);
    *dest = merged;
    return 0;
}

static void jw__scan_title_list_move(jw_scan_title_list *dest,
                                     jw_scan_title_list *source) {
    *dest = *source;
    memset(source, 0, sizeof(*source));
}

static volatile sig_atomic_t g_shutdown_requested = 0;
/* SIGCHLD self-pipe. The handler writes a byte; the read end sits in the
   daemon's poll set, and the loop runs its child-exit handlers only after one
   instead of calling waitpid on every tracked child on every pass. */
static int g_sigchld_pipe[2] = {-1, -1};

static bool jw__request_power_transition(jw_daemon_state *state,
                                         jw_platform_action action,
                                         const char *reason) {
    if (!state) {
        return false;
    }
    if (state->power_transition_requested) return true;
#ifdef PLATFORM_MLP1
    /* Commit to this supervisor generation before cleanup or an IPC success.
       Old supervisors cannot fall back to the former timed SysRq path. */
    jw_platform_result result;
    jw_platform_perform_action(&state->platform, action, 0, &result);
    if (result.code != JW_PLATFORM_RESULT_OK) {
        jw_log_error("power handoff refused: %s", result.message);
        return false;
    }
    /* Informational only: the supervisor's screens and log. */
    if (jw_power_request_reason(reason) != 0) {
        jw_log_warn("power handoff: could not record reason %s", reason ? reason : "(none)");
    }
#else
    (void)reason;
#endif
    state->power_transition_requested = true;
    jw_input_proxy_cancel_menu(&state->input_proxy);
    jw_menu_escape_cancel(&state->menu_escape);
    state->power_transition_action = action;
    state->shutdown_requested = true;
    return true;
}

static bool jw__has_retroarch_session(const jw_daemon_state *state) {
    return state &&
           state->retroarch_session.active &&
           state->child_kind == JW_CHILD_RETROARCH;
}

static bool jw__has_standalone_session(const jw_daemon_state *state) {
    return state &&
           state->retroarch_session.active &&
           state->child_kind == JW_CHILD_EMULATOR;
}

/* Release identity comes only from the policy resolved at launch, which is
   NONE for every provider-bound core. Never re-match core_id or core_path. */
static bool jw__standalone_session_is(const jw_daemon_state *state,
                                      jw_standalone_release release) {
    return jw__has_standalone_session(state) &&
           state->retroarch_session.standalone_policy.release == release;
}

static bool jw__standalone_session_is_ppsspp(const jw_daemon_state *state) {
    return jw__standalone_session_is(state, JW_STANDALONE_RELEASE_PPSSPP);
}

static bool jw__standalone_session_is_drastic(const jw_daemon_state *state) {
    return jw__standalone_session_is(state, JW_STANDALONE_RELEASE_DRASTIC);
}

static bool jw__standalone_session_is_mupen64plus(const jw_daemon_state *state) {
    return jw__standalone_session_is(state, JW_STANDALONE_RELEASE_MUPEN64PLUS);
}

static bool jw__standalone_session_is_flycast(const jw_daemon_state *state) {
    return jw__standalone_session_is(state, JW_STANDALONE_RELEASE_FLYCAST);
}

static bool jw__standalone_session_is_fun_drastic(const jw_daemon_state *state) {
    return jw__standalone_session_is(state, JW_STANDALONE_RELEASE_FUN_DRASTIC);
}

static bool jw__standalone_session_is_yabasanshiro(const jw_daemon_state *state) {
    return jw__standalone_session_is(state, JW_STANDALONE_RELEASE_YABASANSHIRO);
}

static bool jw__standalone_target_is(const jw_launch_target *target,
                                     jw_standalone_release release) {
    return target && target->kind == JW_LAUNCH_TARGET_STANDALONE &&
           target->standalone_policy.release == release;
}

static bool jw__standalone_target_is_mupen64plus(const jw_launch_target *target) {
    return jw__standalone_target_is(target, JW_STANDALONE_RELEASE_MUPEN64PLUS);
}

static bool jw__env_is_disabled(const char *name);
static bool jw__env_is_truthy(const char *name);

static bool jw__standalone_target_is_yabasanshiro(const jw_launch_target *target) {
    return jw__standalone_target_is(target, JW_STANDALONE_RELEASE_YABASANSHIRO);
}

static bool jw__standalone_target_is_ports(const jw_launch_target *target) {
    return jw__standalone_target_is(target, JW_STANDALONE_RELEASE_PORTS);
}

static bool jw__file_contains_text(const char *path, const char *needle,
                                   size_t max_bytes) {
    if (!path || !needle || !needle[0]) {
        return false;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }

    char line[512];
    size_t total = 0;
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        total += strlen(line);
        if (strstr(line, needle)) {
            found = true;
            break;
        }
        if (max_bytes > 0 && total >= max_bytes) {
            break;
        }
    }
    fclose(fp);
    return found;
}

static bool jw__portmaster_drm_rotate_available(const jw_daemon_state *state) {
    if (!state || !state->sdcard_root || !state->sdcard_root[0]) {
        return false;
    }

    const char *platform = state->platform.platform_id[0]
        ? state->platform.platform_id
        : "mlp1";

    char hook[PATH_MAX];
    char shim[PATH_MAX];
    int hook_rc = snprintf(hook, sizeof(hook),
                           "%s/.userdata/%s/portmaster/PortMaster/leaf-armhf-env.sh",
                           state->sdcard_root, platform);
    int shim_rc = snprintf(shim, sizeof(shim),
                           "%s/.userdata/%s/portmaster/compat/drm/aarch64/leaf-drm-rotate.so",
                           state->sdcard_root, platform);
    if (hook_rc < 0 || hook_rc >= (int)sizeof(hook) ||
        shim_rc < 0 || shim_rc >= (int)sizeof(shim)) {
        return false;
    }

    return access(hook, R_OK) == 0 && access(shim, R_OK) == 0;
}

static bool jw__standalone_target_requests_direct_drm(
        const jw_daemon_state *state,
        const jw_launch_target *target,
        const char *rom_abs) {
    if (!state || !target || target->kind != JW_LAUNCH_TARGET_STANDALONE) {
        return false;
    }
    if (jw__env_is_disabled("JAWAKA_DIRECT_DRM")) {
        return false;
    }
    if (strcmp(state->platform.platform_id, "mlp1") != 0) {
        return false;
    }
    if (jw_standalone_policy_requires_direct_drm(
            &target->standalone_policy, target->requires_direct_drm)) {
        return true;
    }
    if (jw__env_is_truthy("JAWAKA_DIRECT_DRM")) {
        return true;
    }
    if (!jw__standalone_target_is_ports(target) || !rom_abs) {
        return false;
    }
    if (!jw__portmaster_drm_rotate_available(state)) {
        return false;
    }

    return jw__file_contains_text(
        rom_abs,
        "LEAF_PM_RUNTIME_COMPAT_GOTHIC_MACHISMO_VULKAN_ROTATE=1",
        128 * 1024);
}

static bool jw__standalone_target_uses_calibrated_virtual_input(
        const jw_launch_target *target) {
    return target && target->kind == JW_LAUNCH_TARGET_STANDALONE &&
           (target->native_pico8 || jw_standalone_policy_uses_calibrated_virtual_input(
               &target->standalone_policy));
}

static long long jw__monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static void jw__handle_signal(int signo) {
    (void)signo;
    g_shutdown_requested = 1;
}

static void jw__handle_sigchld(int signo) {
    (void)signo;
    int saved_errno = errno;
    if (g_sigchld_pipe[1] >= 0) {
        ssize_t ignored = write(g_sigchld_pipe[1], "c", 1);
        (void)ignored;
    }
    errno = saved_errno;
}

/* Called before anything is spawned. On failure the loop falls back to
   checking every child on every pass, as it did before the pipe. */
static bool jw__install_sigchld_pipe(void) {
    if (pipe(g_sigchld_pipe) != 0) {
        g_sigchld_pipe[0] = g_sigchld_pipe[1] = -1;
        return false;
    }
    for (int i = 0; i < 2; i++) {
        (void)fcntl(g_sigchld_pipe[i], F_SETFD, FD_CLOEXEC);
        int flags = fcntl(g_sigchld_pipe[i], F_GETFL);
        if (flags >= 0) {
            (void)fcntl(g_sigchld_pipe[i], F_SETFL, flags | O_NONBLOCK);
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = jw__handle_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) != 0) {
        close(g_sigchld_pipe[0]);
        close(g_sigchld_pipe[1]);
        g_sigchld_pipe[0] = g_sigchld_pipe[1] = -1;
        return false;
    }
    return true;
}

/* usleep that sleeps the whole time. With a SIGCHLD handler installed, a
   child exiting mid-sleep ends a plain usleep early, which would shorten the
   grace windows and "N tries" waits in this file. */
static void jw__usleep(unsigned long usec) {
    struct timespec left = { (time_t)(usec / 1000000UL),
                             (long)(usec % 1000000UL) * 1000L };
    while (nanosleep(&left, &left) != 0 && errno == EINTR) {
    }
}

static int jw__path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static int jw__resident_switch_max(void) {
    static bool cached = false;
    static int max_switches = JW_RESIDENT_SWITCH_MAX_DEFAULT;

    if (cached) {
        return max_switches;
    }
    cached = true;

    const char *value = getenv("JAWAKA_RESIDENT_SWITCH_MAX");
    if (!value || !value[0]) {
        return max_switches;
    }
    if (strcmp(value, "unlimited") == 0 || strcmp(value, "-1") == 0) {
        max_switches = -1;
        return max_switches;
    }

    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (errno == 0 && end && *end == '\0' && parsed >= 0 && parsed <= 1000000L) {
        max_switches = (int)parsed;
    } else {
        jw_log_warn("invalid JAWAKA_RESIDENT_SWITCH_MAX=%s; using default=%s",
                    value, JW_RESIDENT_SWITCH_MAX_DEFAULT_LABEL);
    }
    return max_switches;
}

static const char *jw__env_value(const char *name) {
    const char *value = getenv(name);
    return (value && value[0]) ? value : NULL;
}

static void jw__setenv_default(const char *name, const char *value) {
    if (!name || !value || jw__env_value(name)) {
        return;
    }
    setenv(name, value, 0);
}

static void jw__setenvf_default(const char *name, const char *fmt, ...) {
    if (!name || jw__env_value(name) || !fmt) {
        return;
    }

    char value[PATH_MAX];
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(value, sizeof(value), fmt, args);
    va_end(args);
    if (needed >= 0 && needed < (int)sizeof(value)) {
        setenv(name, value, 0);
    }
}

static bool jw__format_default_system_path(char *out, size_t out_size,
                                           const char *sdcard_root,
                                           const char *platform) {
    if (!out || out_size == 0 || !sdcard_root || !sdcard_root[0] ||
        !platform || !platform[0]) {
        return false;
    }

    int needed = snprintf(out, out_size, "%s/.system/leaf/platforms/%s",
                          sdcard_root, platform);
    return needed >= 0 && needed < (int)out_size;
}

static int jw__env_or_join(char *out, size_t out_size,
                           const char *path_env,
                           const char *base_env_a,
                           const char *base_env_b,
                           const char *fallback_base,
                           const char *leaf) {
    const char *path = jw__env_value(path_env);
    if (path) {
        return snprintf(out, out_size, "%s", path) < (int)out_size ? 0 : -1;
    }

    const char *base = jw__env_value(base_env_a);
    if (!base && base_env_b) {
        base = jw__env_value(base_env_b);
    }
    if (!base) {
        base = fallback_base;
    }
    if (!base || !leaf) {
        return -1;
    }
    return snprintf(out, out_size, "%s/%s", base, leaf) < (int)out_size ? 0 : -1;
}

static int jw__is_regular_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int jw__set_bin_dir(char *argv0, char *out, size_t out_size) {
    char resolved[PATH_MAX];
    if (!realpath(argv0, resolved)) {
        return -1;
    }

    char temp[PATH_MAX];
    snprintf(temp, sizeof(temp), "%s", resolved);
    char *dir = dirname(temp);
    if (!dir) {
        return -1;
    }

    snprintf(out, out_size, "%s", dir);
    return 0;
}

static void jw__print_usage(FILE *stream) {
    fprintf(stream, "Usage: jawakad [--daemon-only] [--help]\n");
}

/* True if Roms/<name>/ exists as a directory. */
static bool jw__roms_has_dir(const char *roms, const char *name) {
    if (!name || !name[0]) return false;
    char probe[PATH_MAX];
    if (snprintf(probe, sizeof(probe), "%s/%s", roms, name) >= (int)sizeof(probe))
        return false;
    struct stat st;
    return stat(probe, &st) == 0 && S_ISDIR(st.st_mode);
}

/* First-run seed: create the empty per-system Roms/ folder for every catalogued
   system so a fresh card carries all the drop-in folders and users don't have to
   consult the docs and make each by hand. The folder created is the RECOMMENDED
   name (systems.json rom_root, e.g. FC -> "Roms/NES"), NOT the internal id, and a
   system is skipped when the user already has a folder for it under the
   recommended name, the id, or any accepted alias pattern (never a second folder
   for a system they've set up). One-time — gated by a hidden marker in Roms/ (the
   scanner skips hidden entries and non-directories, so it's inert), so a folder
   the user deletes stays gone. Best-effort: a missing catalog or an un-writable
   card just skips. */
/* ---- Rumble / haptics (PWM motor) --------------------------------------
 * The MLP1 rumble motor is a single PWM channel (pwmchip0/pwm0, 1 kHz), not a
 * Linux force-feedback device. We configure it ONCE and then only modulate
 * duty_cycle, and which duty means "off" depends on the polarity the driver
 * actually accepted -- this one REJECTS "normal" and stays "inversed", where off
 * is duty=period and duty=0 is FULL ON. Never assume: jw__rumble_off_duty() reads
 * the polarity back. We never toggle enable and never unexport -- both latch the
 * motor on for this driver, which is how it got stranded during bring-up.
 * Amplitude only differentiates across a narrow band (see the floors below), so
 * the vocabulary is 1/2/3 burst PATTERNS by event weight. See
 * umrk-workspace/plans/Jawaka/rumble.md. */
#define JW_RUMBLE_CHIP    "/sys/class/pwm/pwmchip0"
#define JW_RUMBLE_PWM     JW_RUMBLE_CHIP "/pwm0"
#define JW_RUMBLE_PERIOD  1000000L   /* ns, 1 kHz -- matches stock S50loong */
/* Floors and tick length are MEASURED on Puff (2026-07-24), not guessed. This
   motor spins up slowly, so perceptibility is duty x duration, not either alone:
   a 40 ms pulse needs ~75% duty, 90 ms needs ~60%, 350 ms needs only ~20-23%.
   Two independent ladders landed on the same (60%, 90 ms) threshold corner, so a
   ~100 ms tick at a 60% floor is the shortest crisp tick this motor can produce.
   Sustained rumble is a different regime, hence the lower game floor below.
   Coast-down, by contrast, is fast -- 60 ms already separates a double cleanly,
   so the 80 ms gap keeps the 1/2/3-burst vocabulary legible with margin. */
#define JW_RUMBLE_FLOOR   60         /* % of period: min duty a short tick can be felt at */
#define JW_RUMBLE_GAME_FLOOR 25      /* % of period: min for SUSTAINED (game) rumble */
#define JW_RUMBLE_TICK_MS 100
/* Nav is the one event that can fire continuously: Catastrophe repeats a held
   direction every CAT_INPUT_REPEAT_RATE (100 ms), so a 100 ms tick would leave no
   gap and a held scroll would read as one unbroken buzz. A shorter nav tick keeps
   a tick per move -- honest one-for-one feedback -- while leaving the motor a gap
   to fall into. 60% is still the floor at this length, so no separate floor. */
#define JW_RUMBLE_NAV_TICK_MS 70
#define JW_RUMBLE_GAP_MS  80
/* Longest a single force-feedback effect is held before it lapses. SDL clamps
   its own rumble to 0x7FFF ms and resends to keep a continuous effect alive, so
   anything genuinely still playing is refreshed well inside this. */
#define JW_RUMBLE_FF_MAX_HOLD_MS 0x7FFFu
#define JW_RUMBLE_CACHE_TTL_MS 1000

/* pthread_condattr_setclock is POSIX but absent on Darwin, where a condvar can
   only wait on the realtime clock. Prefer CLOCK_MONOTONIC where it exists: this
   handheld has no battery-backed RTC, so the first time-sync after boot steps
   the wall clock by hours and would fire every deadline instantly. Whichever
   clock the condvars end up on, the deadlines must be generated from that SAME
   clock -- a monotonic deadline handed to a realtime condvar is the same bug in
   a different disguise. */
#if defined(_POSIX_CLOCK_SELECTION) && _POSIX_CLOCK_SELECTION > 0 && !defined(__APPLE__)
#define JW_RUMBLE_HAVE_CONDATTR_CLOCK 1
#endif

static pthread_mutex_t g_rumble_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_rumble_cv   = PTHREAD_COND_INITIALIZER;
/* Initialized exactly once in jw__rumble_init -- deliberately NOT given a static
   initializer, because handing an already-initialized condvar to
   pthread_cond_init is undefined behavior. */
static pthread_cond_t  g_rumble_idle_cv;
static pthread_cond_t  g_rumble_wake_cv;
static bool g_rumble_cv_ready  = false;  /* the two condvars above are usable */
static clockid_t g_rumble_clock = CLOCK_REALTIME;  /* clock the condvars wait on */
static bool g_rumble_busy     = false; /* worker is mid-pattern (see quiesce) */
static unsigned g_rumble_gen  = 0;     /* bumped to abort the pattern in flight */
/* Screen off/suspended: swallow all events. Written only via jw__rumble_set_gated
   and read only under g_rumble_lock -- the force-feedback thread reads it, so a
   plain bool here would be a data race with no guarantee the store is ever
   observed, which loses the gate in one direction and rumble for the rest of the
   session in the other. */
static bool g_rumble_gated    = false;
static int  g_rumble_bursts   = 0;   /* pending pattern: 1/2/3, 0 = idle */
static long g_rumble_duty     = 0;   /* on-strength duty (ns), pre-polarity */
static int  g_rumble_tick_ms  = JW_RUMBLE_TICK_MS;  /* per-pattern burst length */
static bool g_rumble_ready    = false; /* channel configured */

/* Polarity is a THREE-state value on purpose. Treating "could not read it" as
   "normal" is how this fails dangerously: under normal, off is duty 0, but the
   MLP1 driver actually runs inversed, where duty 0 is FULL ON. A transient
   sysfs failure would strand the motor at full power. Unknown must disable
   rumble, not pick a default. */
typedef enum {
    JW_RUMBLE_POLARITY_UNKNOWN = 0,
    JW_RUMBLE_POLARITY_NORMAL,
    JW_RUMBLE_POLARITY_INVERSED,
} jw_rumble_polarity;

static jw_rumble_polarity g_rumble_polarity = JW_RUMBLE_POLARITY_UNKNOWN;

/* Returns false on ANY failure, and says why. These writes configure a motor
   that can be left physically energised, so "it probably worked" is not good
   enough -- init refuses to mark the channel ready if one of them fails. */
static bool jw__rumble_write(const char *path, const char *val) {
    size_t len = strlen(val);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        jw_log_warn("rumble: open %s failed: %s", path, strerror(errno));
        return false;
    }
    ssize_t n;
    do {
        n = write(fd, val, len);
    } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (n < 0) {
        jw_log_warn("rumble: write %s failed: %s", path, strerror(saved));
        return false;
    }
    if ((size_t)n != len) {
        jw_log_warn("rumble: short write to %s (%zd of %zu)", path, n, len);
        return false;
    }
    return true;
}

static bool jw__rumble_write_long(const char *path, long v) {
    char b[24];
    int n = snprintf(b, sizeof(b), "%ld", v);
    if (n <= 0 || n >= (int)sizeof(b)) return false;
    return jw__rumble_write(path, b);
}

/* Read a long from a sysfs attribute. Returns -1 when it cannot be read. */
static long jw__rumble_read_long(const char *path) {
    char buf[32];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do {
        n = read(fd, buf, sizeof(buf) - 1);
    } while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    errno = 0;
    char *end = NULL;
    long v = strtol(buf, &end, 10);
    if (errno != 0 || end == buf || v < 0) return -1;
    return v;
}

/* Read the channel's polarity. Anything we cannot positively identify as
   "normal" or "inversed" comes back UNKNOWN -- see the enum for why guessing
   here is the dangerous option. */
static jw_rumble_polarity jw__rumble_read_polarity(void) {
    char pol[32];
    int fd = open(JW_RUMBLE_PWM "/polarity", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        jw_log_warn("rumble: open polarity failed: %s", strerror(errno));
        return JW_RUMBLE_POLARITY_UNKNOWN;
    }
    ssize_t n;
    do {
        n = read(fd, pol, sizeof(pol) - 1);
    } while (n < 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    if (n <= 0) {
        jw_log_warn("rumble: read polarity failed: %s",
                    n < 0 ? strerror(saved) : "empty");
        return JW_RUMBLE_POLARITY_UNKNOWN;
    }
    pol[n] = '\0';
    while (n > 0 && (pol[n - 1] == '\n' || pol[n - 1] == '\r' ||
                     pol[n - 1] == ' '  || pol[n - 1] == '\t')) {
        pol[--n] = '\0';
    }
    if (strcmp(pol, "inversed") == 0) return JW_RUMBLE_POLARITY_INVERSED;
    if (strcmp(pol, "normal")   == 0) return JW_RUMBLE_POLARITY_NORMAL;
    jw_log_warn("rumble: unrecognised polarity value \"%s\"", pol);
    return JW_RUMBLE_POLARITY_UNKNOWN;
}

/* The "off" duty for the polarity actually in effect: normal -> 0, inversed ->
   period. NEVER assume -- a boot race can leave the stock inversed default, and
   under inversed a duty of 0 is FULL ON (this stranded the motor on once). */
static long jw__rumble_off_duty(void) {
    return g_rumble_polarity == JW_RUMBLE_POLARITY_INVERSED ? JW_RUMBLE_PERIOD : 0;
}

/* Create the two timed-wait condvars exactly once, on the best clock this
   platform offers, and record which clock that was so the deadlines match. */
static bool jw__rumble_sync_init(void) {
    pthread_condattr_t attr;
    pthread_condattr_t *use = NULL;
    clockid_t clk = CLOCK_REALTIME;

    if (pthread_condattr_init(&attr) == 0) {
        use = &attr;
#ifdef JW_RUMBLE_HAVE_CONDATTR_CLOCK
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0) {
            clk = CLOCK_MONOTONIC;
        } else {
            jw_log_warn("rumble: condattr_setclock failed; using realtime deadlines");
        }
#endif
    }

    bool ok = pthread_cond_init(&g_rumble_idle_cv, use) == 0;
    if (ok && pthread_cond_init(&g_rumble_wake_cv, use) != 0) {
        pthread_cond_destroy(&g_rumble_idle_cv);
        ok = false;
    }
    if (use) pthread_condattr_destroy(use);

    if (!ok) {
        jw_log_warn("rumble: condvar init failed; haptics disabled");
        return false;
    }
    g_rumble_clock    = clk;
    g_rumble_cv_ready = true;
    return true;
}

/* One-time channel setup. Fails closed at every step: nothing is written to the
   motor until the polarity is positively known, and the channel is only marked
   ready once every safety-critical write has succeeded. Never disables or
   unexports as cleanup -- both latch this driver ON. */
static void jw__rumble_init(void) {
    struct stat st;
    if (stat(JW_RUMBLE_PWM, &st) != 0)
        jw__rumble_write(JW_RUMBLE_CHIP "/export", "0");
    if (stat(JW_RUMBLE_PWM, &st) != 0) {
        jw_log_warn("rumble: pwm0 not available; haptics disabled");
        return;
    }
    if (!jw__rumble_sync_init()) return;

    /* Read BEFORE writing anything. An unknown polarity means we cannot name a
       duty that is guaranteed to be off, so the only safe move is to leave the
       channel alone entirely. */
    g_rumble_polarity = jw__rumble_read_polarity();
    if (g_rumble_polarity == JW_RUMBLE_POLARITY_UNKNOWN) {
        jw_log_warn("rumble: polarity unreadable; haptics disabled "
                    "(refusing to guess an off duty)");
        return;
    }

    /* PERIOD FIRST, and this ordering is load-bearing. On a freshly exported
       channel period is 0, and this driver then rejects duty_cycle, enable AND
       polarity writes with EINVAL -- measured, not assumed. Configuring in any
       other order means the "force it off for safety" write silently fails and
       leaves duty at 0, which under inversed polarity is FULL ON. Period is the
       write that makes every other one legal. */
    if (!jw__rumble_write_long(JW_RUMBLE_PWM "/period", JW_RUMBLE_PERIOD)) goto fail;
    if (!jw__rumble_write_long(JW_RUMBLE_PWM "/duty_cycle", jw__rumble_off_duty())) goto fail;

    /* Now that the channel is legal, normalize the polarity. "normal" DOES take,
       as long as period was written first -- the old belief that this driver
       refused it came from writing polarity before period, which fails with
       EINVAL and reads back as inversed. Both writes stay best-effort because we
       re-read below and adapt to whatever actually stuck. */
    (void)jw__rumble_write(JW_RUMBLE_PWM "/enable", "0");
    (void)jw__rumble_write(JW_RUMBLE_PWM "/polarity", "normal");

    /* Re-read rather than trusting the write: a channel another process left
       inversed still has to be handled, and acting on the value we asked for
       instead of the one in effect is exactly how the motor got stranded during
       bring-up. Under inversed, duty 0 is FULL ON. */
    g_rumble_polarity = jw__rumble_read_polarity();
    if (g_rumble_polarity == JW_RUMBLE_POLARITY_UNKNOWN) {
        jw_log_warn("rumble: polarity unreadable after configure; haptics disabled");
        return;
    }
    if (!jw__rumble_write_long(JW_RUMBLE_PWM "/duty_cycle", jw__rumble_off_duty())) goto fail;
    if (!jw__rumble_write(JW_RUMBLE_PWM "/enable", "1")) goto fail;   /* enabled, resting OFF */

    g_rumble_ready = true;
    jw_log_info("rumble: motor ready (polarity=%s, %ldns period, %s deadlines)",
                g_rumble_polarity == JW_RUMBLE_POLARITY_INVERSED ? "inversed" : "normal",
                JW_RUMBLE_PERIOD,
                g_rumble_clock == CLOCK_REALTIME ? "realtime" : "monotonic");
    return;

fail:
    /* Polarity is known here, so the off duty is the right value -- but it can
       only be written if period is non-zero, or the kernel rejects it and the
       channel stays wherever it was. */
    if (jw__rumble_read_long(JW_RUMBLE_PWM "/period") > 0) {
        (void)jw__rumble_write_long(JW_RUMBLE_PWM "/duty_cycle", jw__rumble_off_duty());
    } else {
        jw_log_warn("rumble: period unset, cannot express an off duty");
    }
    jw_log_warn("rumble: channel setup failed; haptics disabled");
}

/* Bulletproof off: drive the polarity-correct 0% (never disable/unexport). */
static void jw__rumble_off(void) {
    if (g_rumble_ready) jw__rumble_write_long(JW_RUMBLE_PWM "/duty_cycle", jw__rumble_off_duty());
}

/* Map a 0-100 strength onto [FLOOR,100]% of the period (below FLOOR the motor
   won't reliably move). Returns duty in ns. */
static long jw__rumble_duty_for(int strength_pct) {
    if (strength_pct <= 0) return 0;
    if (strength_pct > 100) strength_pct = 100;
    int eff = JW_RUMBLE_FLOOR + (100 - JW_RUMBLE_FLOOR) * strength_pct / 100;
    return JW_RUMBLE_PERIOD * eff / 100;
}

/* Turn a strength duty into the value to actually write, for the polarity in
   effect (under inversed a high duty is weak, so it mirrors around the period). */
static long jw__rumble_polarize(long duty) {
    return g_rumble_polarity == JW_RUMBLE_POLARITY_INVERSED
               ? (JW_RUMBLE_PERIOD - duty) : duty;
}

static void jw__rumble_deadline(struct timespec *ts, int ms) {
    clock_gettime(g_rumble_clock, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/* Sleep inside a pattern, but wake at once if a quiesce bumps the generation.
   Returns true if this pattern has been cancelled and must stop now. A plain
   nanosleep here is what let a timed-out quiesce be undone: the worker would
   wake after the force-off and re-energise the motor. */
static bool jw__rumble_pattern_wait(int ms, unsigned my_gen) {
    struct timespec deadline;
    jw__rumble_deadline(&deadline, ms);
    bool cancelled;
    pthread_mutex_lock(&g_rumble_lock);
    while (g_rumble_gen == my_gen) {
        if (pthread_cond_timedwait(&g_rumble_wake_cv, &g_rumble_lock,
                                   &deadline) != 0) {
            break;   /* slept the full tick/gap */
        }
    }
    cancelled = g_rumble_gen != my_gen;
    pthread_mutex_unlock(&g_rumble_lock);
    return cancelled;
}

/* Worker: waits for a pattern request and pulses duty off the input path so the
   IPC loop never blocks. A new request coalesces (latest wins) rather than
   queueing, so rapid input can't back up a buzz train. */
static void *jw__rumble_worker(void *arg) {
    (void)arg;
    for (;;) {
        int bursts, tick_ms; long duty;
        pthread_mutex_lock(&g_rumble_lock);
        while (g_rumble_bursts == 0)
            pthread_cond_wait(&g_rumble_cv, &g_rumble_lock);
        bursts = g_rumble_bursts; duty = g_rumble_duty; tick_ms = g_rumble_tick_ms;
        unsigned my_gen = g_rumble_gen;
        g_rumble_bursts = 0;
        g_rumble_busy = true;
        pthread_mutex_unlock(&g_rumble_lock);
        long on  = jw__rumble_polarize(duty);
        long off = jw__rumble_off_duty();
        for (int i = 0; i < bursts; i++) {
            jw__rumble_write_long(JW_RUMBLE_PWM "/duty_cycle", on);
            bool cancelled = jw__rumble_pattern_wait(tick_ms, my_gen);
            /* Always write off before reacting to a cancel, so the motor is
               never left energised on the way out. */
            jw__rumble_write_long(JW_RUMBLE_PWM "/duty_cycle", off);
            if (cancelled) break;
            if (i + 1 < bursts &&
                jw__rumble_pattern_wait(JW_RUMBLE_GAP_MS, my_gen)) break;
        }
        pthread_mutex_lock(&g_rumble_lock);
        g_rumble_busy = false;
        pthread_cond_signal(&g_rumble_idle_cv);
        pthread_mutex_unlock(&g_rumble_lock);
    }
    return NULL;
}

/* Stop cleanly before the motor changes hands or the system freezes. A plain
   force-off races the worker -- suspend halts every thread, so a pattern caught
   mid-burst stays energised for the whole sleep, the exact way it got stranded
   during bring-up. Bumping the generation cancels the pattern in flight and
   wakes the worker out of its tick, so this genuinely stops it rather than
   hoping it finishes first. The bounded wait is belt and braces. */
static void jw__rumble_quiesce(void) {
    if (!g_rumble_ready) return;
    struct timespec deadline;
    jw__rumble_deadline(&deadline, 1000);
    pthread_mutex_lock(&g_rumble_lock);
    g_rumble_bursts = 0;                       /* nothing new starts */
    g_rumble_gen++;                            /* cancel what is running */
    pthread_cond_broadcast(&g_rumble_wake_cv);
    while (g_rumble_busy) {
        if (pthread_cond_timedwait(&g_rumble_idle_cv, &g_rumble_lock,
                                   &deadline) != 0) {
            jw_log_warn("rumble: worker did not stop within 1s; forcing off");
            break;
        }
    }
    pthread_mutex_unlock(&g_rumble_lock);
    jw__rumble_off();
}

/* Raise or drop the gate. Under the lock so the force-feedback thread cannot
   slip a hold past a gate-then-quiesce, and so it is guaranteed to see the
   change at all. */
static void jw__rumble_set_gated(bool gated) {
    pthread_mutex_lock(&g_rumble_lock);
    g_rumble_gated = gated;
    pthread_mutex_unlock(&g_rumble_lock);
}

static void jw__rumble_queue(int bursts, long duty, int tick_ms) {
    if (!g_rumble_ready || bursts <= 0 || duty <= 0) return;
    pthread_mutex_lock(&g_rumble_lock);
    if (g_rumble_gated) {
        pthread_mutex_unlock(&g_rumble_lock);
        return;
    }
    g_rumble_bursts  = bursts;   /* latest wins */
    g_rumble_duty    = duty;
    g_rumble_tick_ms = tick_ms;
    pthread_cond_signal(&g_rumble_cv);
    pthread_mutex_unlock(&g_rumble_lock);
}


/* Refresh the TTL-cached rumble settings from the DB (mirrors the screenshot
   flag: cheap on the input path, picks up changes within the TTL). */
static void jw__rumble_refresh_cache(jw_daemon_state *state, uint64_t now_ms) {
    if (state->rumble_checked_ms != 0 &&
        now_ms - state->rumble_checked_ms < (uint64_t)JW_RUMBLE_CACHE_TTL_MS)
        return;
    jw_rumble_settings rs;
    (void)jw_db_load_rumble_settings(state->db_path, &rs);  /* defaults on failure */
    state->rumble_ui_cached       = rs.ui != 0;
    state->rumble_strength_cached = rs.strength;
    state->rumble_game_cached     = rs.game != 0;
    state->rumble_checked_ms = now_ms;
}

/* Map a named UI event to a pattern and queue it, honouring the cached
   settings. nav = single, select = single, commit = double, blocked = triple;
   UI rumble gates all four. */
static void jw__rumble_event(jw_daemon_state *state, const char *event) {
    if (!g_rumble_ready || !event) return;
    jw__rumble_refresh_cache(state, (uint64_t)jw__monotonic_ms());
    if (!state->rumble_ui_cached) return;
    int bursts, tick_ms = JW_RUMBLE_TICK_MS;
    if (strcmp(event, "nav") == 0) {
        bursts  = 1;
        tick_ms = JW_RUMBLE_NAV_TICK_MS;   /* short enough to survive held repeat */
    } else if (strcmp(event, "select") == 0) {
        bursts = 1;
    } else if (strcmp(event, "commit") == 0) {
        bursts = 2;
    } else if (strcmp(event, "blocked") == 0) {
        bursts = 3;
    } else {
        return;
    }
    jw__rumble_queue(bursts, jw__rumble_duty_for(state->rumble_strength_cached), tick_ms);
}

/* ---- Game rumble (Phase 2) ----------------------------------------------
 * One route, deliberately. Every game reaches the motor through force feedback
 * on jawakad's own virtual gamepad: the pad advertises FF_RUMBLE, so an emulator
 * rumbling through plain SDL or evdev lands in jw__rumble_ff below and needs no
 * patch of its own. That covers RetroArch and every standalone alike.
 *
 * There used to be a second route -- endpoints handed out in RUMBLE_PWM_* so a
 * patched emulator could write duty_cycle itself -- and it is gone on purpose.
 * Nothing consumed it once force feedback worked, and leaving it published was
 * an invitation to a second writer on a channel that only tolerates one: an
 * emulator writing from its own thread while the worker writes from another,
 * the two disagreeing about when to stop.
 *
 * jawakad keeps owning the channel (exported, enabled, resting off), so nothing
 * downstream needs to know the polarity, the period or the motor's stiction
 * floor. MAX is the user's strength setting, so the slider acts as the in-game
 * intensity ceiling. Endpoints are held unpolarized; the worker polarizes. */
typedef struct {
    bool on;
    long min, max;   /* magnitude endpoints, pre-polarity */
} jw_rumble_game_env;

/* The endpoints the FF path maps a magnitude onto, published for the life of a
   game session. Read by the force-feedback thread and written by the main loop,
   so they live under g_rumble_lock like everything else the worker shares. */
static bool g_rumble_ff_on  = false;
static long g_rumble_ff_min = 0;
static long g_rumble_ff_max = 0;
/* Set while the in-game menu is up. Separate from the endpoints because this is
   a pause, not a teardown -- the session is still live and must resume with the
   same numbers. See jw__rumble_ff_suspend for why a quiesce alone is not enough. */
static bool g_rumble_ff_suspended = false;

static void jw__rumble_resolve_game_env(jw_daemon_state *state,
                                        jw_rumble_game_env *out) {
    memset(out, 0, sizeof(*out));
    if (!g_rumble_ready || !state) return;
    jw__rumble_refresh_cache(state, (uint64_t)jw__monotonic_ms());
    if (!state->rumble_game_cached) return;
    long max = jw__rumble_duty_for(state->rumble_strength_cached);
    if (max <= 0) return;
    /* Game rumble is SUSTAINED, so it uses the lower sustained floor -- holding it
       to the short-tick floor would make a core's weakest effect feel near-full and
       throw away most of the magnitude range the core is asking for. */
    out->min = JW_RUMBLE_PERIOD * JW_RUMBLE_GAME_FLOOR / 100;
    out->max = max;
    out->on  = true;
}

/* Open or close the force-feedback route. Called with the resolved endpoints
   when a game starts and with NULL when it ends -- closing it also stops any
   effect still running, so a game killed mid-rumble cannot strand the motor. */
static void jw__rumble_publish_ff(const jw_rumble_game_env *env) {
    bool was_on;
    pthread_mutex_lock(&g_rumble_lock);
    was_on = g_rumble_ff_on;
    g_rumble_ff_on  = env && env->on;
    g_rumble_ff_min = (env && env->on) ? env->min : 0;
    g_rumble_ff_max = (env && env->on) ? env->max : 0;
    pthread_mutex_unlock(&g_rumble_lock);
    if (was_on && !(env && env->on)) jw__rumble_quiesce();
}

/* Hold the force-feedback route shut while the in-game menu is up.
 *
 * Quiescing the motor is not enough on its own, and the reason is subtle: the
 * emulator's SDL still believes its effect is playing, because nothing told it
 * otherwise. SDL periodically resends a live effect to keep it alive, and a
 * resend arriving after the reclaim would start the motor up again behind the
 * menu -- the same "rumble runs non-stop in the menu" bug as before, just on a
 * longer fuse and only when the menu is opened mid-effect.
 *
 * Refusing the resend outright is deterministic; timing the reclaim to outlast
 * SDL's resend interval would be a guess about someone else's internals. */
static void jw__rumble_ff_suspend(bool suspended) {
    pthread_mutex_lock(&g_rumble_lock);
    g_rumble_ff_suspended = suspended;
    pthread_mutex_unlock(&g_rumble_lock);
}

/* Approve, scale and queue a force-feedback request in ONE critical section.
 *
 * Sustained rather than patterned: a single burst held for `ms` IS a continuous
 * buzz, so this reuses the worker and inherits its auto-stop and its
 * cancel-on-suspend. Unlike jw__rumble_queue it preempts -- queueing is "latest
 * wins" only at the next pattern boundary, which suits UI ticks and not a game
 * changing intensity, which has to be heard now.
 *
 * Splitting those was the bug: reading "the route is open" under the lock,
 * dropping it, and queueing afterwards is check-then-act against a main thread
 * that closes the route and quiesces as a game exits or the menu opens. The FF
 * thread would then energise the motor on an approval that had already expired,
 * behind a menu or after the session it belonged to was gone.
 *
 * The endpoints are read here rather than passed in for the same reason -- they
 * belong to the route being approved, and a caller holding its own copy is
 * holding a stale one by definition.
 *
 * Returns 1 queued (with *out_duty set), 0 refused, -1 no motor. Logging is left
 * to the caller so it happens outside the lock. */
static int jw__rumble_hold_ff(uint16_t magnitude, int ms, long *out_duty) {
    if (!g_rumble_ready || ms <= 0) return -1;

    pthread_mutex_lock(&g_rumble_lock);
    if (g_rumble_gated || !g_rumble_ff_on || g_rumble_ff_suspended ||
        g_rumble_ff_max <= 0) {
        pthread_mutex_unlock(&g_rumble_lock);
        return 0;
    }

    long duty = g_rumble_ff_min +
                (g_rumble_ff_max - g_rumble_ff_min) * (long)magnitude / 0xFFFF;
    if (duty <= 0) {
        pthread_mutex_unlock(&g_rumble_lock);
        return 0;
    }

    g_rumble_bursts  = 1;
    g_rumble_duty    = duty;
    g_rumble_tick_ms = ms;
    g_rumble_gen++;                              /* cancel the level in flight */
    if (g_rumble_cv_ready) pthread_cond_broadcast(&g_rumble_wake_cv);
    pthread_cond_signal(&g_rumble_cv);
    pthread_mutex_unlock(&g_rumble_lock);

    *out_duty = duty;
    return 1;
}

/* An emulator played (or stopped) an FF_RUMBLE effect on the virtual pad.
   Runs on the proxy's force-feedback thread, so it touches nothing but the
   lock-guarded endpoints and the worker. */
static void jw__rumble_ff(void *userdata, uint16_t magnitude, uint32_t duration_ms) {
    (void)userdata;
    /* Logged on transitions only, never per magnitude change: a game can retune
       intensity every frame, and this is the input path. Enough to answer "is
       the emulator asking for rumble at all", which is the question that costs
       an hour when a new emulator turns out to be silent. */
    static bool logged_on = false;

    if (magnitude == 0) {
        if (logged_on) {
            logged_on = false;
            jw_log_info("rumble: force feedback stopped");
        }
        jw__rumble_quiesce();
        return;
    }

    /* A zero replay length means "until stopped" in evdev, but the motor must
       never be left with no deadline at all -- if the emulator dies mid-effect
       the stop never comes. Hold it for the same span SDL uses between its own
       rumble resends, so a genuinely continuous effect is refreshed long before
       this runs out and an abandoned one dies on its own. */
    uint32_t ms = duration_ms ? duration_ms : JW_RUMBLE_FF_MAX_HOLD_MS;
    if (ms > JW_RUMBLE_FF_MAX_HOLD_MS) ms = JW_RUMBLE_FF_MAX_HOLD_MS;

    long duty = 0;
    int rc = jw__rumble_hold_ff(magnitude, (int)ms, &duty);
    if (rc <= 0) {
        /* Worth saying out loud: an emulator asking for rumble and getting
           silence looks identical to one that never asked. */
        if (!logged_on) {
            logged_on = true;
            jw_log_info("rumble: force feedback requested but the route is closed "
                        "(no game session, or game rumble is off)");
        }
        return;
    }
    if (!logged_on) {
        logged_on = true;
        jw_log_info("rumble: force feedback started magnitude=%u duty=%ld",
                    (unsigned)magnitude, duty);
    }
}

static void jw__seed_rom_folders(const jw_daemon_state *state) {
    const char *roms = getenv("ROMS_PATH");
    if (!roms || !roms[0]) return;

    char marker[PATH_MAX];
    if (snprintf(marker, sizeof(marker), "%s/.leaf_seeded", roms) >= (int)sizeof(marker))
        return;
    if (access(marker, F_OK) == 0) return;   /* already seeded once */

    char error[256] = "";
    const jw_ra_catalog *catalog = jw_ra_catalog_get(state->sdcard_root,
                                                     error, sizeof(error));
    if (!catalog) return;   /* no system list -> nothing to seed */

    mkdir(roms, 0777);   /* ensure Roms/ itself (best-effort; usually exists) */

    int made = 0;
    bool all_ok = true;   /* a real (non-EEXIST) mkdir failure holds off the marker */
    for (size_t i = 0; i < catalog->system_count; i++) {
        const jw_ra_system *s = &catalog->systems[i];
        if (!s->id || !s->id[0] || s->id[0] == '_') continue;  /* skip pseudo ids (_tools) */

        /* Recommended folder = last path element of rom_root ("Roms/NES" -> "NES"),
           falling back to the id. */
        const char *rec = s->id;
        if (s->rom_root && s->rom_root[0]) {
            const char *slash = strrchr(s->rom_root, '/');
            rec = (slash && slash[1]) ? slash + 1 : s->rom_root;
        }

        /* Already have a folder for this system under any accepted name? Skip. */
        bool have = jw__roms_has_dir(roms, rec) || jw__roms_has_dir(roms, s->id);
        for (size_t j = 0; !have && j < s->patterns.count; j++)
            have = jw__roms_has_dir(roms, s->patterns.items[j]);
        if (have) continue;

        char path[PATH_MAX];
        if (snprintf(path, sizeof(path), "%s/%s", roms, rec) >= (int)sizeof(path))
            continue;
        if (mkdir(path, 0777) == 0) made++;
        else if (errno != EEXIST) all_ok = false;   /* transient/permission failure -> retry */
    }

    /* Only mark done once every eligible folder exists, so a transient failure
       (card briefly read-only, etc.) is retried on the next boot rather than
       skipped forever. */
    if (all_ok) {
        FILE *f = fopen(marker, "w");
        if (f) { fputs("1\n", f); fclose(f); }
    }
    jw_log_info("seeded %d ROM folder(s) under %s%s", made, roms,
                all_ok ? "" : " (some failed; will retry next boot)");
}

typedef enum {
    JW_SOURCE_PATH_ROOT = 0,
    JW_SOURCE_PATH_USERDATA,
    JW_SOURCE_PATH_SHARED_USERDATA,
    JW_SOURCE_PATH_ROMS,
    JW_SOURCE_PATH_IMAGES,
    JW_SOURCE_PATH_MUSIC,
    JW_SOURCE_PATH_APPS,
    JW_SOURCE_PATH_BIOS,
    JW_SOURCE_PATH_SAVES,
    JW_SOURCE_PATH_STATES,
    JW_SOURCE_PATH_CHEATS,
} jw_source_path_kind;

static const char *jw__source_path_value(const jw_storage_source *source,
                                         jw_source_path_kind kind) {
    if (!source) return NULL;
    switch (kind) {
        case JW_SOURCE_PATH_ROOT: return source->root;
        case JW_SOURCE_PATH_USERDATA: return source->userdata_path;
        case JW_SOURCE_PATH_SHARED_USERDATA: return source->shared_userdata_path;
        case JW_SOURCE_PATH_ROMS: return source->roms_path;
        case JW_SOURCE_PATH_IMAGES: return source->images_path;
        case JW_SOURCE_PATH_MUSIC: return source->music_path;
        case JW_SOURCE_PATH_APPS: return source->apps_path;
        case JW_SOURCE_PATH_BIOS: return source->bios_path;
        case JW_SOURCE_PATH_SAVES: return source->saves_path;
        case JW_SOURCE_PATH_STATES: return source->states_path;
        case JW_SOURCE_PATH_CHEATS: return source->cheats_path;
        default: return NULL;
    }
}

static int jw__publish_source_path_list(const char *name,
                                        const jw_storage_source_list *sources,
                                        jw_source_path_kind kind) {
    char value[JW_STORAGE_PATH_MAX * JW_STORAGE_MAX_SOURCES];
    size_t used = 0;
    if (!name || !sources || sources->count <= 0) return -1;
    value[0] = '\0';
    for (int i = 0; i < sources->count; i++) {
        const char *path = jw__source_path_value(&sources->sources[i], kind);
        if (!path || !path[0]) return -1;
        int n = snprintf(value + used, sizeof(value) - used, "%s%s",
                         i == 0 ? "" : ":", path);
        if (n < 0 || (size_t)n >= sizeof(value) - used) return -1;
        used += (size_t)n;
    }
    jw__setenv_default(name, value);
    return 0;
}

static void jw__publish_runtime_path_env(const jw_daemon_state *state) {
    if (!state || !state->runtime_dir || !state->sdcard_root) {
        return;
    }

    const char *platform = state->platform.platform_id[0]
        ? state->platform.platform_id
        : "mac";

    jw__setenv_default("PLATFORM", platform);
    jw__setenv_default("DEVICE", platform);
    jw__setenv_default("SDCARD_PATH", state->sdcard_root);
    jw__setenv_default("UMRK_RUNTIME_PATH", state->runtime_dir);
    char system_path[PATH_MAX];
    if (jw__format_default_system_path(system_path, sizeof(system_path),
                                       state->sdcard_root, platform)) {
        jw__setenv_default("SYSTEM_PATH", system_path);
    }
    if (getenv("SYSTEM_PATH")) {
        jw__setenv_default("UMRK_PLATFORM_PATH", getenv("SYSTEM_PATH"));
    }

    char launcher_path[PATH_MAX];
    if (snprintf(launcher_path, sizeof(launcher_path), "%s", state->bin_dir) <
        (int)sizeof(launcher_path)) {
        char *slash = strrchr(launcher_path, '/');
        if (slash && strcmp(slash + 1, "bin") == 0) {
            *slash = '\0';
            jw__setenv_default("UMRK_LAUNCHER_PATH", launcher_path);
        }
    }
    jw__setenv_default("UMRK_BIN_PATH", state->bin_dir);
    if (getenv("UMRK_LAUNCHER_PATH")) {
        jw__setenvf_default("UMRK_ENV_FILE", "%s/env.sh",
                            getenv("UMRK_LAUNCHER_PATH"));
    }

    if (getenv("SYSTEM_PATH")) {
        jw__setenvf_default("UMRK_MARKER_PATH", "%s/enabled",
                            getenv("SYSTEM_PATH"));
    }
    /* Durable data lives at the SD root, split by ownership, not under the
       release-managed .system payload (matches device/umrk-env.sh). */
    jw__setenvf_default("USERDATA_PATH", "%s/.userdata/%s",
                        state->sdcard_root, platform);
    jw__setenvf_default("UMRK_INTERNAL_DATA_PATH", "%s/.umrk/%s",
                        state->sdcard_root, platform);
    jw__setenvf_default("SHARED_USERDATA_PATH", "%s/.userdata/shared",
                        state->sdcard_root);
    if (getenv("USERDATA_PATH")) {
        jw__setenvf_default("LOGS_PATH", "%s/logs", getenv("USERDATA_PATH"));
    }
    if (getenv("UMRK_INTERNAL_DATA_PATH")) {
        jw__setenvf_default("UMRK_ADB_MARKER_PATH", "%s/adb-enabled",
                            getenv("UMRK_INTERNAL_DATA_PATH"));
    }
    jw__setenvf_default("ROMS_PATH", "%s/Roms", state->sdcard_root);
    jw__setenvf_default("IMAGES_PATH", "%s/Images", state->sdcard_root);
    jw__setenvf_default("MUSIC_PATH", "%s/Music", state->sdcard_root);
    jw__setenvf_default("VIDEO_PATH", "%s/Videos", state->sdcard_root);
    char recordings_path[PATH_MAX];
    if (jw_primary_recordings_path(recordings_path, sizeof(recordings_path),
                                   state->sdcard_root)) {
        /* This is primary-owned state, not a caller override: the RetroArch
           config and conversion dispatcher below resolve the same helper. */
        setenv("RECORDINGS_PATH", recordings_path, 1);
    }
    jw__setenvf_default("APPS_PATH", "%s/Apps", state->sdcard_root);
    jw__setenvf_default("BIOS_PATH", "%s/BIOS", state->sdcard_root);
    jw__setenvf_default("SAVES_PATH", "%s/Saves", state->sdcard_root);
    jw__setenvf_default("STATES_PATH", "%s/States", state->sdcard_root);
    jw__setenvf_default("CHEATS_PATH", "%s/Cheats", state->sdcard_root);
    if (strcmp(platform, "mlp1") == 0) {
        jw__setenv_default("UMRK_SECONDARY_SDCARD_PATH", "/media/sdcard1");
    }
    if (!getenv("SDCARD_PATHS")) {
        const char *secondary = getenv("UMRK_SECONDARY_SDCARD_PATH");
        if (secondary && secondary[0] && strcmp(secondary, state->sdcard_root) != 0) {
            jw__setenvf_default("SDCARD_PATHS", "%s:%s",
                                state->sdcard_root, secondary);
        } else {
            jw__setenv_default("SDCARD_PATHS", state->sdcard_root);
        }
    }

    jw_storage_source_list sources;
    bool complete_source_env =
        jw_storage_sources_resolve(state->sdcard_root, &sources) == 0 &&
        jw__publish_source_path_list("SDCARD_PATHS", &sources,
                                     JW_SOURCE_PATH_ROOT) == 0 &&
        jw__publish_source_path_list("USERDATA_PATHS", &sources,
                                     JW_SOURCE_PATH_USERDATA) == 0 &&
        jw__publish_source_path_list("SHARED_USERDATA_PATHS", &sources,
                                     JW_SOURCE_PATH_SHARED_USERDATA) == 0 &&
        jw__publish_source_path_list("ROMS_PATHS", &sources,
                                     JW_SOURCE_PATH_ROMS) == 0 &&
        jw__publish_source_path_list("IMAGES_PATHS", &sources,
                                     JW_SOURCE_PATH_IMAGES) == 0 &&
        jw__publish_source_path_list("MUSIC_PATHS", &sources,
                                     JW_SOURCE_PATH_MUSIC) == 0 &&
        jw__publish_source_path_list("APPS_PATHS", &sources,
                                     JW_SOURCE_PATH_APPS) == 0 &&
        jw__publish_source_path_list("BIOS_PATHS", &sources,
                                     JW_SOURCE_PATH_BIOS) == 0 &&
        jw__publish_source_path_list("SAVES_PATHS", &sources,
                                     JW_SOURCE_PATH_SAVES) == 0 &&
        jw__publish_source_path_list("STATES_PATHS", &sources,
                                     JW_SOURCE_PATH_STATES) == 0 &&
        jw__publish_source_path_list("CHEATS_PATHS", &sources,
                                     JW_SOURCE_PATH_CHEATS) == 0;
    if (complete_source_env) {
        jw__setenv_default("UMRK_ENV_VERSION", "2");
    } else {
        jw__setenv_default("UMRK_ENV_VERSION", "1");
    }
    if (getenv("SYSTEM_PATH")) {
        jw__setenvf_default("CORES_PATH", "%s/cores", getenv("SYSTEM_PATH"));
    }

    char *retroarch_bin = jw_retroarch_bin_path();
    if (retroarch_bin) {
        jw__setenv_default("UMRK_RETROARCH_BIN", retroarch_bin);
        free(retroarch_bin);
    }

    setenv("JAWAKA_RUNTIME_DIR", state->runtime_dir, 1);
    setenv("JAWAKA_SDCARD_ROOT", state->sdcard_root, 1);
    jw__setenv_default("UMRK_DAEMON_SOCKET", state->socket_path);
    if (getenv("UMRK_DAEMON_SOCKET")) {
        setenv("JAWAKA_SOCKET_PATH", getenv("UMRK_DAEMON_SOCKET"), 1);
    }
    if (getenv("UMRK_RETROARCH_BIN")) {
        setenv("JAWAKA_RETROARCH_BIN", getenv("UMRK_RETROARCH_BIN"), 1);
    }
    if (getenv("CORES_PATH")) {
        setenv("JAWAKA_RETROARCH_CORES_DIR", getenv("CORES_PATH"), 1);
    }
}

/* Bring up the SVC-1 service supervisor: open the control-state store
   (clearing session Run per the contract), then scan the configured Apps/
   roots for service-bearing paks. Runs after jw__publish_runtime_path_env
   so $USERDATA_PATH / $LOGS_PATH / $APPS_PATH are already resolved. Any
   failure here is non-fatal to the daemon: services are simply unsupervised
   this run rather than taking down the launcher. */
static void jw__services_init(jw_daemon_state *state) {
    if (!state || state->services) {
        return;
    }

    const char *platform = getenv("PLATFORM");
    if (!platform || !platform[0]) {
        platform = jw_platform_compiled_id();
    }
    if (!platform || !platform[0]) {
        jw_log_warn("services: no platform id; supervisor disabled");
        return;
    }
    const char *logs = getenv("LOGS_PATH");
    jw_storage_source_list sources;
    if (jw_storage_sources_resolve(state->sdcard_root, &sources) != 0 ||
        sources.count <= 0) {
        jw_log_warn("services: storage sources unavailable; supervisor disabled");
        return;
    }

    char scan_paths[JW_STORAGE_MAX_SOURCES * 2][PATH_MAX];
    const char *primary_roots[JW_STORAGE_MAX_SOURCES * 2 + 1];
    const char *secondary_roots[JW_STORAGE_MAX_SOURCES * 2 + 1];
    int path_count = 0;
    int primary_count = 0;
    int secondary_count = 0;
    for (int i = 0; i < sources.count; i++) {
        const jw_storage_source *source = &sources.sources[i];
        const char *names[] = {platform, "shared"};
        for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
            if (path_count >= JW_STORAGE_MAX_SOURCES * 2 ||
                !jw_svc_supervisor_join_scan_root(
                    scan_paths[path_count], sizeof(scan_paths[path_count]),
                    source->apps_path, names[n])) {
                jw_log_warn("services: Apps scan root is invalid; supervisor disabled");
                return;
            }
            if (source->primary) {
                primary_roots[primary_count++] = scan_paths[path_count];
            } else {
                secondary_roots[secondary_count++] = scan_paths[path_count];
            }
            path_count++;
        }
    }
    primary_roots[primary_count] = NULL;
    secondary_roots[secondary_count] = NULL;
    const jw_storage_source *primary = jw_storage_sources_primary(&sources);
    if (!primary || primary_count == 0) {
        jw_log_warn("services: Primary Apps root unavailable; supervisor disabled");
        return;
    }

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = jw_svc_supervisor_open(
        state->runtime_dir, logs ? logs : "", state->state_dir,
        primary_roots, secondary_count > 0 ? secondary_roots : NULL,
        primary->userdata_path, reason, sizeof(reason));
    if (!sup) {
        jw_log_warn("services: supervisor unavailable (%s); "
                    "no services supervised this run", reason);
        return;
    }

    if (primary->userdata_path[0]) {
        char legacy_ssh_config[PATH_MAX];
        if (snprintf(legacy_ssh_config, sizeof(legacy_ssh_config),
                     "%s/umrk-ssh-server/config.ini", primary->userdata_path) >=
            (int)sizeof(legacy_ssh_config)) {
            jw_log_warn("services: legacy SSH intent migration path is too long; "
                        "leaving persistent intent disabled for this boot");
        } else {
            jw_svc_legacy_ssh_migration_report migration;
            if (!jw_svc_supervisor_migrate_legacy_ssh_intent(
                    sup, legacy_ssh_config, &migration,
                    reason, sizeof(reason))) {
                jw_log_warn("services: legacy SSH intent migration failed (%s); "
                            "will retry next boot", reason);
            } else if (migration.applied && migration.enabled) {
                jw_log_info("services: migrated configured SSH install to "
                            "Start with Leaf");
            } else if (migration.applied && migration.config_present &&
                       !migration.config_valid) {
                jw_log_warn("services: legacy SSH config is invalid; "
                            "completed one-time migration disabled");
            } else if (migration.applied) {
                jw_log_info("services: no legacy SSH config; completed "
                            "one-time migration disabled");
            }
        }
    }
    state->services = sup;
    jw_svc_supervisor_set_exit_notify(sup, state->reap_by_signal);
    if (state->active_game.active) {
        jw_svc_supervisor_game_set_active(sup, true);
        jw_log_warn("services: game-sensitive starts suppressed by %s active-game record",
                    state->active_game.uncertain ? "uncertain recovered" :
                                                   "recovered");
    }
    int found = jw_svc_supervisor_scan(sup);
    if (found > 0) {
        jw_log_info("services: supervising %d service(s)", found);
    }
}

static int jw__reply_json(jw_ipc_client *client, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        cJSON_Delete(root);
        return -1;
    }

    int rc = jw_ipc_client_send(client, json, strlen(json));
    cJSON_free(json);
    cJSON_Delete(root);
    /* The request was handled. A peer that closed before reading is a
       notification client behaving exactly as designed, and reporting it as a
       failure is what buried the session log under 7724 warnings: every haptic
       tick is one of these. */
    return rc == JW_IPC_PEER_GONE ? 0 : rc;
}

static int jw__reply_hello_ok(jw_ipc_client *client) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello-ok");
    cJSON_AddStringToObject(root, "version", "0.0.1");
    cJSON *features = cJSON_AddArrayToObject(root, "features");
    cJSON_AddItemToArray(features, cJSON_CreateString("relocate-games-v1"));
    cJSON_AddItemToArray(features, cJSON_CreateString("package-quiesce-v1"));
    cJSON_AddItemToArray(features, cJSON_CreateString("package-mutation-v1"));
    if (jw_storage_source_paths_v2_valid()) {
        cJSON_AddItemToArray(features, cJSON_CreateString("source-paths-v2"));
    }
    if (jw_input_roster_supported() && jw_storage_source_paths_v2_valid()) {
        cJSON_AddItemToArray(features, cJSON_CreateString(JW_PICO8_CAPABILITY));
    }
    return jw__reply_json(client, root);
}

static int jw__reply_ok(jw_ipc_client *client, const char *action, const jw_scan_result *scan_result) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", action);
    if (scan_result) {
        cJSON_AddNumberToObject(root, "game_count", scan_result->game_count);
        cJSON_AddNumberToObject(root, "app_count", scan_result->app_count);
        cJSON_AddNumberToObject(root, "system_count", scan_result->system_count);
    }
    return jw__reply_json(client, root);
}

static int jw__reply_scan_ok(jw_ipc_client *client, const char *action,
                             int title_hints_accepted) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddStringToObject(root, "message", action);
    cJSON_AddNumberToObject(root, "title_hints_accepted", title_hints_accepted);
    return jw__reply_json(client, root);
}

static int jw__reply_error(jw_ipc_client *client, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "message", message);
    return jw__reply_json(client, root);
}

/* Retry re-runs the same launch through every gate, so it is only offered
   where the block is transient and re-checkable without a decision from the
   user: RAOfflineProxy not ready yet. Every other block keeps its two-way
   Cancel / Play Anyway contract. */
static bool jw__game_launch_retry_allowed(const jw_daemon_state *state) {
    return state && state->game_launch_blocked &&
           !state->game_check_decision &&
           !state->game_launch_blocked_requires_verified_stop &&
           strcmp(state->game_launch_blocked_reason,
                  "raofflineproxy-not-ready") == 0;
}

static int jw__reply_game_launch_blocked(jw_daemon_state *state,
                                         jw_ipc_client *client) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }
    /* Recovery records and a record whose durable removal failed both require
       operator recovery, never the data-loss override. The latter can arise
       in-process after a coordination failure, so checking `recovered` alone
       would advertise an override that the action correctly refuses. */
    bool conservative_active = state->active_game.active &&
                               (state->active_game.recovered ||
                                state->active_game.uncertain);
    bool blocked = state->game_launch_blocked || state->game_check_decision ||
                   conservative_active;
    cJSON_AddStringToObject(root, "type", "game-launch-blocked-status");
    cJSON_AddBoolToObject(root, "blocked", blocked);
    cJSON_AddBoolToObject(root, "override_allowed",
                          state->game_launch_blocked && !conservative_active);
    cJSON_AddBoolToObject(root, "retry_allowed",
                          jw__game_launch_retry_allowed(state) &&
                              !conservative_active);
    cJSON_AddBoolToObject(
        root, "requires_verified_stop",
        state->game_launch_blocked_requires_verified_stop);
    cJSON_AddBoolToObject(root, "sync_pending", state->game_check_decision);
    if (state->game_check_decision) {
        cJSON_AddStringToObject(root, "service_id",
                               state->game_check_service_id);
        cJSON_AddNumberToObject(root, "pending_items",
                               state->game_check_pending_items);
        cJSON_AddNumberToObject(root, "pending_bytes",
                               (double)state->game_check_pending_bytes);
    }
    if (blocked && !state->game_check_decision) {
        cJSON_AddStringToObject(
            root, "service_id",
            state->game_launch_blocked_service_id);
        cJSON_AddStringToObject(
            root, "reason",
            conservative_active
                ? (state->active_game.uncertain
                       ? "active-launch-recovery-uncertain"
                       : "active-launch-recovered")
                : state->game_launch_blocked_reason);
        if (state->game_check_pending_items > 0 ||
            state->game_check_pending_bytes > 0) {
            cJSON_AddNumberToObject(root, "pending_items",
                                   state->game_check_pending_items);
            cJSON_AddNumberToObject(root, "pending_bytes",
                                   (double)state->game_check_pending_bytes);
        }
    }
    return jw__reply_json(client, root);
}

static void jw__json_add_int_or_null(cJSON *root, const char *name, int value) {
    if (value < 0) {
        cJSON_AddNullToObject(root, name);
    } else {
        cJSON_AddNumberToObject(root, name, value);
    }
}

static cJSON *jw__platform_capabilities_json(const jw_platform_capabilities *cap) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "battery", cap && cap->battery);
    cJSON_AddBoolToObject(root, "charging", cap && cap->charging);
    cJSON_AddBoolToObject(root, "sleep", cap && cap->sleep);
    cJSON_AddBoolToObject(root, "poweroff", cap && cap->poweroff);
    cJSON_AddBoolToObject(root, "reboot", cap && cap->reboot);
    cJSON_AddBoolToObject(root, "brightness", cap && cap->brightness);
    cJSON_AddBoolToObject(root, "volume", cap && cap->volume);
    cJSON_AddBoolToObject(root, "wifi", cap && cap->wifi);
    cJSON_AddBoolToObject(root, "bluetooth", cap && cap->bluetooth);
    cJSON_AddBoolToObject(root, "adb", cap && cap->adb);
    cJSON_AddBoolToObject(root, "boot_splash", cap && cap->boot_splash);
    cJSON_AddBoolToObject(root, "refresh_rate", cap && cap->refresh_rate);
    cJSON_AddBoolToObject(root, "color_temperature", cap && cap->color_temperature);
    cJSON_AddBoolToObject(root, "hdmi_output", cap && cap->hdmi_output);
    cJSON_AddBoolToObject(root, "led", cap && cap->led);
    cJSON_AddBoolToObject(root, "performance", cap && cap->performance);
    cJSON_AddBoolToObject(root, "source-paths-v2",
                          jw_storage_source_paths_v2_valid());
    return root;
}

static cJSON *jw__platform_status_json(const jw_platform_status *status) {
    cJSON *root = cJSON_CreateObject();
    if (!status) {
        return root;
    }

    jw__json_add_int_or_null(root, "battery_percent", status->battery_percent);
    jw__json_add_int_or_null(root, "charging", status->charging);
    jw__json_add_int_or_null(root, "brightness_percent", status->brightness_percent);
    jw__json_add_int_or_null(root, "volume_percent", status->volume_percent);
    if (status->audio_output >= 0 &&
        status->audio_output < JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
        cJSON_AddStringToObject(root, "audio_output",
                                jw_platform_audio_output_name(status->audio_output));
    } else {
        cJSON_AddNullToObject(root, "audio_output");
    }
    cJSON *audio_outputs = cJSON_CreateArray();
    if (audio_outputs) {
        for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
            if (status->audio_available_outputs & JW_PLATFORM_AUDIO_OUTPUT_BIT(i)) {
                cJSON_AddItemToArray(audio_outputs,
                                     cJSON_CreateString(jw_platform_audio_output_name((jw_platform_audio_output)i)));
            }
        }
        cJSON_AddItemToObject(root, "audio_available_outputs", audio_outputs);
    }
    cJSON *audio_volumes = cJSON_CreateObject();
    if (audio_volumes) {
        for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
            const char *name = jw_platform_audio_output_name((jw_platform_audio_output)i);
            if (status->audio_volume_percent[i] >= 0) {
                cJSON_AddNumberToObject(audio_volumes, name,
                                        status->audio_volume_percent[i]);
            } else {
                cJSON_AddNullToObject(audio_volumes, name);
            }
        }
        cJSON_AddItemToObject(root, "audio_volumes", audio_volumes);
    }
    cJSON_AddNumberToObject(root, "audio_test_playing", status->audio_test_playing);
    jw__json_add_int_or_null(root, "wifi_connected", status->wifi_connected);
    jw__json_add_int_or_null(root, "wifi_strength", status->wifi_strength);
    jw__json_add_int_or_null(root, "bluetooth_connected", status->bluetooth_connected);
    jw__json_add_int_or_null(root, "adb_enabled", status->adb_enabled);
    jw__json_add_int_or_null(root, "adb_intent_enabled", status->adb_intent_enabled);
    jw__json_add_int_or_null(root, "boot_splash_enabled", status->boot_splash_enabled);
    jw__json_add_int_or_null(root, "refresh_rate_hz", status->refresh_rate_hz);
    jw__json_add_int_or_null(root, "color_temp_kelvin", status->color_temp_kelvin);
    jw__json_add_int_or_null(root, "hdmi_connected", status->hdmi_connected);
    jw__json_add_int_or_null(root, "hdmi_output_mode", status->hdmi_output_mode);
    return root;
}

static void jw__perf_request_init(jw_platform_perf_request *request) {
    if (!request) {
        return;
    }
    memset(request, 0, sizeof(*request));
    for (int i = 0; i < JW_PLATFORM_PERF_DOMAIN_COUNT; i++) {
        request->domains[i].frequency = -1;
    }
}

static void jw__perf_request_set(jw_platform_perf_request *request,
                                 jw_platform_perf_domain domain,
                                 const char *governor,
                                 int frequency) {
    if (!request || domain < 0 || domain >= JW_PLATFORM_PERF_DOMAIN_COUNT) {
        return;
    }
    jw_platform_perf_domain_request *d = &request->domains[domain];
    snprintf(d->governor, sizeof(d->governor), "%s", governor ? governor : "");
    d->frequency = frequency;
}

static void jw__perf_request_for_profile(jw_platform_perf_profile profile,
                                         jw_platform_perf_request *request) {
    jw__perf_request_init(request);
    switch (profile) {
        case JW_PLATFORM_PERF_PROFILE_PERFORMANCE:
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_CPU,
                                 "performance", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_GPU,
                                 "performance", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_DMC,
                                 "performance", -1);
            break;
        case JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER:
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_CPU,
                                 "userspace", 600000);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_GPU,
                                 "userspace", 300000000);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_DMC,
                                 "userspace", 528000000);
            break;
        case JW_PLATFORM_PERF_PROFILE_SLEEP:
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_CPU,
                                 "powersave", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_GPU,
                                 "powersave", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_DMC,
                                 "powersave", -1);
            break;
        case JW_PLATFORM_PERF_PROFILE_FRONTEND:
        case JW_PLATFORM_PERF_PROFILE_BALANCED:
        case JW_PLATFORM_PERF_PROFILE_AUTO:
        case JW_PLATFORM_PERF_PROFILE_CUSTOM:
        default:
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_CPU,
                                 "schedutil", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_GPU,
                                 "simple_ondemand", -1);
            jw__perf_request_set(request, JW_PLATFORM_PERF_DOMAIN_DMC,
                                 "dmc_ondemand", -1);
            break;
    }
}

static jw_platform_perf_profile jw__perf_resolve_game_profile(
        jw_daemon_state *state,
        jw_platform_perf_profile profile,
        const char *system) {
    /* One owner for the whole rule (internal/platform/perf_policy.c): the AUTO
       preference, and the Dreamcast-family contract that performance wins over
       a session, game, system or global request for another profile. Every
       Flycast choice on DC, NAOMI and ATOMISWAVE resolves through here, which
       is why the rule is keyed on the system and not the core. The live
       refresh rate feeds AUTO: above 60 Hz every game gets performance. */
    jw_platform_status status;
    jw_platform_get_status(&state->platform, &status);
    return jw_platform_perf_game_profile(system, status.refresh_rate_hz, profile);
}

static jw_platform_perf_profile jw__perf_current_requested_profile(
        const jw_daemon_state *state) {
    if (!state) {
        return JW_PLATFORM_PERF_PROFILE_AUTO;
    }
    return state->perf_session_override
        ? state->perf_session_profile
        : state->perf_global_profile;
}

static int jw__perf_apply_profile(jw_daemon_state *state,
                                  jw_platform_perf_profile requested,
                                  const char *system,
                                  const char *reason) {
    if (!state || !state->platform.capabilities.performance) {
        return 0;
    }

    jw_platform_perf_profile profile =
        jw__perf_resolve_game_profile(state, requested, system);
    jw_platform_perf_request request;
    if (profile == JW_PLATFORM_PERF_PROFILE_CUSTOM && state->perf_custom_valid) {
        request = state->perf_custom_request;
    } else {
        if (profile == JW_PLATFORM_PERF_PROFILE_CUSTOM) {
            profile = JW_PLATFORM_PERF_PROFILE_BALANCED;
        }
        jw__perf_request_for_profile(profile, &request);
    }

    jw_platform_result result;
    jw_platform_apply_performance(&state->platform, &request, &result);
    if (result.code == JW_PLATFORM_RESULT_OK) {
        state->perf_active_profile = profile;
        state->perf_last_error[0] = '\0';
        jw_log_info("performance: applied profile=%s requested=%s reason=%s system=%s",
                    jw_platform_perf_profile_name(profile),
                    jw_platform_perf_profile_name(requested),
                    reason ? reason : "unknown",
                    system && system[0] ? system : "(none)");
        return 0;
    }

    snprintf(state->perf_last_error, sizeof(state->perf_last_error), "%s",
             result.message[0] ? result.message
                               : jw_platform_result_code_name(result.code));
    jw_log_warn("performance: apply failed profile=%s reason=%s: %s",
                jw_platform_perf_profile_name(profile),
                reason ? reason : "unknown",
                state->perf_last_error);
    return -1;
}

static int jw__perf_apply_game(jw_daemon_state *state, const char *system,
                               const char *reason) {
    return jw__perf_apply_profile(state,
                                  jw__perf_current_requested_profile(state),
                                  system, reason);
}

static int jw__perf_apply_frontend(jw_daemon_state *state, const char *reason) {
    return jw__perf_apply_profile(state, JW_PLATFORM_PERF_PROFILE_FRONTEND,
                                  NULL, reason);
}

static int jw__perf_apply_current_context(jw_daemon_state *state,
                                          const char *reason) {
    if (state && state->retroarch_session.active) {
        return jw__perf_apply_game(state, state->retroarch_session.system, reason);
    }
    if (state && state->child_kind == JW_CHILD_PICO8_APP) {
        return jw__perf_apply_game(state, "PICO8", reason);
    }
    return jw__perf_apply_frontend(state, reason);
}

static void jw__perf_load_global(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    state->perf_global_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    char value[64];
    if (state->db_path &&
        jw_db_get_setting(state->db_path, JW_PERF_SETTING_KEY,
                          value, sizeof(value)) == 0 &&
        value[0]) {
        jw_platform_perf_profile parsed;
        if (jw_platform_parse_perf_profile(value, &parsed) &&
            parsed != JW_PLATFORM_PERF_PROFILE_CUSTOM &&
            parsed != JW_PLATFORM_PERF_PROFILE_SLEEP &&
            parsed != JW_PLATFORM_PERF_PROFILE_FRONTEND) {
            state->perf_global_profile = parsed;
        }
    }
}

static int jw__perf_persist_global(jw_daemon_state *state,
                                   jw_platform_perf_profile profile) {
    if (!state || !state->db_path) {
        return -1;
    }
    return jw_db_set_setting(state->db_path, JW_PERF_SETTING_KEY,
                             jw_platform_perf_profile_name(profile));
}

static cJSON *jw__perf_domain_json(const jw_platform_perf_domain_status *domain) {
    cJSON *root = cJSON_CreateObject();
    if (!domain) {
        return root;
    }
    cJSON_AddBoolToObject(root, "supported", domain->supported);
    cJSON_AddStringToObject(root, "name", domain->name);
    cJSON_AddStringToObject(root, "governor", domain->governor);
    jw__json_add_int_or_null(root, "current_freq", domain->current_freq);
    jw__json_add_int_or_null(root, "set_freq", domain->set_freq);
    cJSON_AddStringToObject(root, "available_governors",
                            domain->available_governors);
    cJSON_AddStringToObject(root, "available_frequencies",
                            domain->available_frequencies);
    return root;
}

static int jw__reply_performance_status(jw_daemon_state *state,
                                        jw_ipc_client *client) {
    jw_platform_perf_status status;
    jw_platform_get_performance_status(&state->platform, &status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "performance-status");
    cJSON_AddBoolToObject(root, "supported", status.supported);
    cJSON_AddStringToObject(root, "active_profile",
                            jw_platform_perf_profile_name(state->perf_active_profile));
    cJSON_AddStringToObject(root, "global_profile",
                            jw_platform_perf_profile_name(state->perf_global_profile));
    cJSON_AddStringToObject(root, "session_profile",
                            jw_platform_perf_profile_name(state->perf_session_profile));
    cJSON_AddBoolToObject(root, "session_override", state->perf_session_override);
    cJSON_AddStringToObject(root, "message", status.message);
    cJSON_AddStringToObject(root, "last_error", state->perf_last_error);
    jw__json_add_int_or_null(root, "soc_temp_c", status.soc_temp_c);

    cJSON *domains = cJSON_CreateObject();
    if (domains) {
        cJSON_AddItemToObject(domains, "cpu",
                              jw__perf_domain_json(&status.domains[JW_PLATFORM_PERF_DOMAIN_CPU]));
        cJSON_AddItemToObject(domains, "gpu",
                              jw__perf_domain_json(&status.domains[JW_PLATFORM_PERF_DOMAIN_GPU]));
        cJSON_AddItemToObject(domains, "dmc",
                              jw__perf_domain_json(&status.domains[JW_PLATFORM_PERF_DOMAIN_DMC]));
        cJSON_AddItemToObject(root, "domains", domains);
    }
    return jw__reply_json(client, root);
}

static int jw__handle_performance_set_profile(jw_daemon_state *state,
                                              jw_ipc_client *client,
                                              cJSON *root) {
    cJSON *profile_json = cJSON_GetObjectItemCaseSensitive(root, "profile");
    if (!cJSON_IsString(profile_json) || !profile_json->valuestring) {
        return jw__reply_error(client, "missing performance profile");
    }

    jw_platform_perf_profile profile;
    if (!jw_platform_parse_perf_profile(profile_json->valuestring, &profile)) {
        return jw__reply_error(client, "unknown performance profile");
    }

    cJSON *scope_json = cJSON_GetObjectItemCaseSensitive(root, "scope");
    const char *scope = cJSON_IsString(scope_json) && scope_json->valuestring
                      ? scope_json->valuestring
                      : "session";

    if (strcmp(scope, "global") == 0) {
        if (profile == JW_PLATFORM_PERF_PROFILE_CUSTOM ||
            profile == JW_PLATFORM_PERF_PROFILE_SLEEP ||
            profile == JW_PLATFORM_PERF_PROFILE_FRONTEND) {
            return jw__reply_error(client, "profile cannot be global");
        }
        state->perf_global_profile = profile;
        (void)jw__perf_persist_global(state, profile);
        if (state->retroarch_session.active && !state->perf_session_override) {
            (void)jw__perf_apply_game(state, state->retroarch_session.system,
                                      "global-profile");
        }
        return jw__reply_ok(client, "performance-set-profile", NULL);
    }

    if (strcmp(scope, "session") != 0) {
        return jw__reply_error(client, "unknown performance scope");
    }
    if (profile == JW_PLATFORM_PERF_PROFILE_FRONTEND ||
        profile == JW_PLATFORM_PERF_PROFILE_SLEEP) {
        return jw__reply_error(client, "profile cannot be a session override");
    }
    state->perf_session_profile = profile;
    state->perf_session_override = true;
    state->perf_custom_valid = state->perf_custom_valid &&
                               profile == JW_PLATFORM_PERF_PROFILE_CUSTOM;
    if (state->retroarch_session.active) {
        (void)jw__perf_apply_game(state, state->retroarch_session.system,
                                  "session-profile");
    }
    return jw__reply_ok(client, "performance-set-profile", NULL);
}

static void jw__perf_parse_domain_request(cJSON *root,
                                          const char *prefix,
                                          jw_platform_perf_domain_request *out) {
    if (!root || !prefix || !out) {
        return;
    }
    char key[64];
    snprintf(key, sizeof(key), "%s_governor", prefix);
    cJSON *governor = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(governor) && governor->valuestring) {
        snprintf(out->governor, sizeof(out->governor), "%s", governor->valuestring);
    }
    snprintf(key, sizeof(key), "%s_frequency", prefix);
    cJSON *frequency = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(frequency)) {
        out->frequency = frequency->valueint;
    }
}

static int jw__handle_performance_set_custom(jw_daemon_state *state,
                                             jw_ipc_client *client,
                                             cJSON *root) {
    jw_platform_perf_request request;
    jw__perf_request_init(&request);
    jw__perf_parse_domain_request(root, "cpu",
                                  &request.domains[JW_PLATFORM_PERF_DOMAIN_CPU]);
    jw__perf_parse_domain_request(root, "gpu",
                                  &request.domains[JW_PLATFORM_PERF_DOMAIN_GPU]);
    jw__perf_parse_domain_request(root, "dmc",
                                  &request.domains[JW_PLATFORM_PERF_DOMAIN_DMC]);
    state->perf_custom_request = request;
    state->perf_custom_valid = true;
    state->perf_session_profile = JW_PLATFORM_PERF_PROFILE_CUSTOM;
    state->perf_session_override = true;
    if (state->retroarch_session.active) {
        (void)jw__perf_apply_game(state, state->retroarch_session.system,
                                  "session-custom");
    }
    return jw__reply_ok(client, "performance-set-custom", NULL);
}

static int jw__handle_performance_reset_session(jw_daemon_state *state,
                                                jw_ipc_client *client) {
    state->perf_session_override = false;
    state->perf_session_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    state->perf_custom_valid = false;
    jw__perf_request_init(&state->perf_custom_request);
    (void)jw__perf_apply_current_context(state, "session-reset");
    return jw__reply_ok(client, "performance-reset-session", NULL);
}

static void jw__platform_sleep_with_performance(jw_daemon_state *state,
                                               jw_platform_result *out) {
    /* Audio first, while the clocks are still up: a stream refill caught by
       the drop to sleep clocks can trip PulseAudio's realtime limit. */
    jw_platform_sleep_audio(&state->platform);
    (void)jw__perf_apply_profile(state, JW_PLATFORM_PERF_PROFILE_SLEEP,
                                 NULL, "sleep");
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SLEEP, 0, out);
    (void)jw__perf_apply_current_context(state, "wake");
    /* Only now, at wake clocks, may a stream suspended for the sleep refill
       its buffer. */
    jw_platform_wake_audio(&state->platform);
}

/* The launcher's status bar shows the volume, and the launcher puts up the
   HDMI keep-or-revert prompt; it re-reads both from platform-levels when told
   to, otherwise its status worker checks only every 5 s. Only a launcher whose
   hello said it handles SIGUSR2 is told (the signal would kill one that does
   not). */
static void jw__notify_launcher_levels(jw_daemon_state *state) {
    if (state && state->launcher_levels_signal && state->child_pid > 0 &&
        state->child_kind == JW_CHILD_LAUNCHER) {
        (void)kill(state->child_pid, SIGUSR2);
    }
}

static void jw__cache_platform_status(jw_daemon_state *state,
                                      const jw_platform_status *status) {
    if (!state || !status) {
        return;
    }
    if (status->brightness_percent >= 0) {
        state->cached_brightness_percent =
            jw_platform_clamp_brightness_percent(status->brightness_percent);
    }
    if (status->volume_percent >= 0) {
        int volume = status->volume_percent;
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        state->cached_volume_percent = volume;
    }
}

static void jw__refresh_platform_cache(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    jw_platform_status status;
    jw_platform_get_status(&state->platform, &status);
    jw__cache_platform_status(state, &status);
    state->platform_cache_read_ms = jw__monotonic_ms();
}

/* Stored RetroAchievements credentials (Settings > Games > Accounts). Resolved from the
   DB in the daemon parent as one coherent account snapshot (state, validated
   credentials, revision), then applied to the environment of the forked child
   only — never the long-lived daemon — so the plaintext password is not
   inherited by the launcher, OSD, ledd, or app-store apps. The same snapshot
   drives two handoffs: the existing RetroArch per-launch config (JAWAKA_CHEEVOS_*)
   and the standalone-ra-account-v1 child-only snapshot (UMRK_RA_ACCOUNT_*). */
typedef jw_ra_account jw_cheevos_creds;

/* Read the account from the DB. Opening SQLite happens in the parent before
   fork(); the resulting struct is applied child-side via jw__cheevos_apply_env
   (RetroArch) or jw_ra_account_prepare_standalone_env (authorized
   standalones). A legacy saved pair without a revision is initialized
   exactly once here, in the parent, with the same checked write discipline
   as an account save; if that cannot happen the snapshot is unreadable (or
   invalid for malformed rows), never configured with a guessed revision. */
static void jw__cheevos_resolve(jw_daemon_state *state, jw_cheevos_creds *creds) {
    jw_db_resolve_ra_account_handoff(state ? state->db_path : NULL, creds);
}

/* Apply the credentials to the CURRENT process environment. The RetroArch
   session config writer (jw_prepare_retroarch_config) reads JAWAKA_CHEEVOS_* via
   getenv to put cheevos_username/password into the per-launch config that
   RetroArch validates at launch. Any account state that is not a complete
   configured pair clears the vars, and the session config then carries no
   account (cheevos_* are protected keys, never merged in from the shared
   config). Because the writer runs in the daemon parent,
   callers there must clear the env again right after the config is written
   so the plaintext password does not persist. */
static void jw__cheevos_apply_env(const jw_cheevos_creds *creds) {
    jw_ra_account_apply_retroarch_env(creds);
}

/* Drop any cheevos credentials from the current process environment. Paired with
   jw__cheevos_apply_env around a config write in the parent. */
static void jw__cheevos_clear_env(void) {
    jw_ra_account_clear_retroarch_env();
}

/* Drop the standalone-ra-account-v1 snapshot from the current process
   environment. Every standalone and app child starts from a cleared set; only
   an authorized target gets a fresh snapshot applied right after. */
static void jw__ra_account_clear_env(void) {
    jw_ra_account_clear_env();
}

/* Drop the bundled-Flycast proxy route intent and Flycast's developer probe
   channel from the CURRENT process environment. UMRK_FLYCAST_RA_ROUTE is
   per-launch child state for one authorized target; FLYCAST_PROBE and
   FLYCAST_CONFIG_OVERRIDES belong to explicit developer probes run outside
   the daemon and must never ride along on a managed launch, where they could
   inject an achievement host or Hardcore override. */
static void jw__flycast_route_clear_env(void) {
    unsetenv(JW_FLYCAST_RA_ROUTE_ENV);
    unsetenv("FLYCAST_PROBE");
    unsetenv("FLYCAST_CONFIG_OVERRIDES");
}

/* The RAOfflineProxy service is live when the supervisor holds a positive
   PGID in RUNNING or STARTING for a present, valid pak; see
   jw_raofflineproxy_entry_live, which the RetroArch gate
   (jw__raofflineproxy_route) and the Flycast route intent share. */
static bool jw__raofflineproxy_entry_live(const jw_svc_supervised *entry) {
    return jw_raofflineproxy_entry_live(entry);
}

static bool jw__raofflineproxy_service_live(const jw_daemon_state *state) {
    return state && state->services &&
           jw__raofflineproxy_entry_live(
               jw_svc_supervisor_find(state->services, JW_ROP_SERVICE_ID));
}

/* True when the pak dir refers to the bundled RetroArch app — the one app whose
   runner builds its own RetroArch config and so legitimately consumes the
   cheevos credentials. Case-insensitive match on "retroarch". */
static bool jw__pak_dir_is_retroarch(const char *pak_dir) {
    if (!pak_dir || !pak_dir[0]) {
        return false;
    }
    for (const char *p = pak_dir; *p; p++) {
        if ((*p == 'r' || *p == 'R') && strncasecmp(p, "retroarch", 9) == 0) {
            return true;
        }
    }
    return false;
}

static void jw__publish_audio_env(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    jw_platform_status status;
    jw_platform_get_audio_status(&state->platform, &status);
    jw__cache_platform_status(state, &status);

    jw_platform_audio_output output = status.audio_output;
    if (output < 0 || output >= JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
        output = JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    }
    const char *name = jw_platform_audio_output_name(output);
    const char *device = "default";
    setenv("UMRK_AUDIO_OUTPUT", name, 1);
    setenv("JAWAKA_AUDIO_OUTPUT", name, 1);
    setenv("UMRK_AUDIO_DEVICE", device, 1);
    setenv("JAWAKA_AUDIO_DEVICE", device, 1);
}

#define JW_AUDIO_RECONCILE_WAKE_THROTTLE_MS 1000

static void jw__reconcile_audio(jw_daemon_state *state, const char *reason,
                                bool throttle) {
    if (!state) {
        return;
    }

    long long now = jw__monotonic_ms();
    if (throttle) {
        if (state->audio_reconcile_last_ms > 0 &&
            now - state->audio_reconcile_last_ms < JW_AUDIO_RECONCILE_WAKE_THROTTLE_MS) {
            return;
        }
        state->audio_reconcile_last_ms = now;
    }
    jw_platform_audio_reconcile(&state->platform, reason);
}

static bool jw__retroarch_bluetooth_audio_active(jw_daemon_state *state) {
    if (!state) {
        return false;
    }

    jw_platform_status status;
    jw_platform_get_audio_status(&state->platform, &status);
    return status.audio_output == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH;
}

static void jw__schedule_retroarch_audio_reinit(jw_daemon_state *state,
                                                const char *reason) {
    if (!state || !jw__has_retroarch_session(state) ||
        !state->retroarch_session.audio_bluetooth) {
        return;
    }

    long long now = jw__monotonic_ms();
    state->retroarch_audio_reinit_pending = true;
    state->retroarch_audio_reinit_next_ms = now;
    state->retroarch_audio_reinit_deadline_ms =
        now + JW_RETROARCH_AUDIO_REINIT_TIMEOUT_MS;
    snprintf(state->retroarch_audio_reinit_reason,
             sizeof(state->retroarch_audio_reinit_reason),
             "%s", (reason && reason[0]) ? reason : "unknown");
    jw_log_info("RetroArch audio: scheduled reinit after %s",
                state->retroarch_audio_reinit_reason);
}

static void jw__schedule_retroarch_audio_reinit_if_bluetooth(jw_daemon_state *state,
                                                             const char *reason) {
    if (!state || !jw__has_retroarch_session(state) ||
        !state->retroarch_session.audio_bluetooth) {
        return;
    }
    if (jw__retroarch_bluetooth_audio_active(state)) {
        jw__schedule_retroarch_audio_reinit(state, reason);
    }
}

/* Reclaim the motor once the PAUSE we sent has certainly been acted on. Without
   this a game paused mid-rumble keeps the motor running for the whole time the
   menu is up: the core stops being called, so nothing ever writes the duty back
   down. */
static void jw__tick_rumble_reclaim(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    /* Reconcile the force-feedback suspension against the menu rather than
       trusting the close paths to unset it. The menu is left by a dozen routes
       (resume, exit, switcher, game swap, crash), and a flag that got stuck on
       would silently kill rumble for the rest of the session -- a worse bug than
       the one suspending it prevents. menu_visible is cleared on all of them, so
       deriving from it cannot stick. */
    jw__rumble_ff_suspend(state->menu_visible);

    if (state->rumble_reclaim_ms <= 0) {
        return;
    }
    if (jw__monotonic_ms() < state->rumble_reclaim_ms) {
        return;
    }
    state->rumble_reclaim_ms = 0;
    jw__rumble_quiesce();
}

static void jw__tick_retroarch_audio_reinit(jw_daemon_state *state) {
    if (!state || !state->retroarch_audio_reinit_pending) {
        return;
    }
    if (!jw__has_retroarch_session(state) ||
        !state->retroarch_session.audio_bluetooth) {
        state->retroarch_audio_reinit_pending = false;
        return;
    }

    long long now = jw__monotonic_ms();
    if (now < state->retroarch_audio_reinit_next_ms) {
        return;
    }
    if (state->retroarch_audio_reinit_deadline_ms > 0 &&
        now > state->retroarch_audio_reinit_deadline_ms) {
        jw_log_warn("RetroArch audio: Bluetooth reinit timed out after %s",
                    state->retroarch_audio_reinit_reason[0]
                        ? state->retroarch_audio_reinit_reason
                        : "unknown");
        state->retroarch_audio_reinit_pending = false;
        return;
    }

    jw__reconcile_audio(state, "retroarch-audio-reinit", false);
    if (!jw__retroarch_bluetooth_audio_active(state)) {
        state->retroarch_audio_reinit_next_ms =
            now + JW_RETROARCH_AUDIO_REINIT_RETRY_MS;
        return;
    }

    jw_ra_client client = jw_ra_client_default();
    jw_ra_result result = jw_ra_audio_reinit(&client);
    if (result == JW_RA_OK) {
        jw_log_info("RetroArch audio: reinit sent after %s",
                    state->retroarch_audio_reinit_reason[0]
                        ? state->retroarch_audio_reinit_reason
                        : "unknown");
        state->retroarch_audio_reinit_pending = false;
        return;
    }

    jw_log_warn("RetroArch audio: reinit failed result=%s",
                jw_ra_result_string(result));
    state->retroarch_audio_reinit_next_ms =
        now + JW_RETROARCH_AUDIO_REINIT_RETRY_MS;
}

/* Publish the live panel refresh (read from the active DRM mode) so the
   RetroArch config writer can pin video_refresh_rate to it. RA's pacing and
   Black Frame Insertion break when it believes 60Hz while the panel runs
   100/120. Re-read per launch so a runtime refresh-rate change is reflected. */
/* Publish Leaf's UI language for the emulator that is about to launch.
   RetroArch already carries the translations -- HAVE_LANGEXTRA is its default
   and was never disabled -- so for it this is the only thing standing between a
   Chinese Leaf and a Chinese RetroArch; PPSSPP reads the same variable from its
   launch.sh. Re-read per launch, like the refresh rate, so switching language
   takes effect on the next game without a reboot.

   Deliberately its own function rather than part of the display env: standalone
   emulators need the language but must not inherit refresh/BFI publishing,
   which is RetroArch's config writer alone. */
static void jw__publish_language_env(jw_daemon_state *state) {
    if (!state || !state->db_path) {
        unsetenv("UMRK_LANGUAGE");
        unsetenv("JAWAKA_LANGUAGE");
        return;
    }
    char lang[JW_I18N_CODE_MAX] = "";
    if (jw_db_get_setting(state->db_path, "language", lang, sizeof(lang)) == 0 &&
        lang[0]) {
        /* UMRK_LANGUAGE is canonical; JAWAKA_LANGUAGE is its indefinite
           compatibility alias (PPSSPP and released launchers read it), so the
           two are always set or cleared together with the same value. The
           child-side jw_appearance_apply_env() re-exports the resolved
           language for ordinary app launches with overwrite semantics and is
           authoritative there. */
        setenv("UMRK_LANGUAGE", lang, 1);
        setenv("JAWAKA_LANGUAGE", lang, 1);
    } else {
        unsetenv("UMRK_LANGUAGE");
        unsetenv("JAWAKA_LANGUAGE");
    }
}

static void jw__publish_display_env(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    jw_platform_status status;
    jw_platform_get_status(&state->platform, &status);
    if (status.refresh_rate_hz > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", status.refresh_rate_hz);
        setenv("JAWAKA_REFRESH_RATE_HZ", buf, 1);
    } else {
        unsetenv("JAWAKA_REFRESH_RATE_HZ");
    }

    /* Black Frame Insertion: publish only when the user enabled it AND the panel
       runs at twice a content rate, so there is a spare refresh to blank (120Hz
       for 60fps NTSC, 100Hz for 50fps PAL). The RA config writer turns
       JAWAKA_BFI=1 into video_black_frame_insertion; RetroArch itself imposes no
       rate restriction, since it simply targets refresh/(bfi+1). At 60Hz there is
       no spare refresh, so BFI stays off regardless of the setting. */
    bool bfi_enabled = false;
    if (state->db_path) {
        char val[8] = "";
        if (jw_db_get_setting(state->db_path, "bfi_enabled", val, sizeof(val)) == 0 &&
            val[0] && strcmp(val, "0") != 0) {
            bfi_enabled = true;
        }
    }
    if (bfi_enabled && jw_bfi_content_fps(status.refresh_rate_hz) > 0) {
        setenv("JAWAKA_BFI", "1", 1);
    } else {
        unsetenv("JAWAKA_BFI");
    }

    jw__publish_language_env(state);
}

/* Seconds left before an armed 1080p120 revert, 0 when none is armed. The
   launcher's status worker reads it from platform-levels and platform-status,
   so it does not have to ask hdmi-revert-status every second. */
static int jw__hdmi_revert_seconds(const jw_daemon_state *state) {
    long long now = jw__monotonic_ms();
    if (state->hdmi_revert_deadline_ms == 0 || state->hdmi_revert_deadline_ms <= now) {
        return 0;
    }
    return (int)((state->hdmi_revert_deadline_ms - now + 999) / 1000);
}

static int jw__reply_platform_status(jw_daemon_state *state, jw_ipc_client *client) {
    jw_platform_status status;
    jw_platform_get_status(&state->platform, &status);
    jw__cache_platform_status(state, &status);
    state->platform_cache_read_ms = jw__monotonic_ms();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "platform-status");
    cJSON_AddStringToObject(root, "platform_id", state->platform.platform_id);
    cJSON_AddStringToObject(root, "platform_name", state->platform.platform_name);
    cJSON_AddStringToObject(root, "script_dir", state->platform.script_dir);
    cJSON_AddItemToObject(root, "capabilities",
                          jw__platform_capabilities_json(&state->platform.capabilities));
    cJSON *status_json = jw__platform_status_json(&status);
    if (state->led_configured) {
        cJSON *led = cJSON_CreateObject();
        cJSON_AddBoolToObject(led, "enabled", state->cached_led.enabled);
        cJSON_AddStringToObject(led, "mode", jw_led_mode_name(state->cached_led.mode));
        cJSON_AddNumberToObject(led, "r", state->cached_led.r);
        cJSON_AddNumberToObject(led, "g", state->cached_led.g);
        cJSON_AddNumberToObject(led, "b", state->cached_led.b);
        cJSON_AddNumberToObject(led, "brightness", state->cached_led.brightness);
        cJSON_AddNumberToObject(led, "speed", state->cached_led.speed);
        cJSON_AddItemToObject(status_json, "led", led);
    }
    cJSON_AddItemToObject(root, "status", status_json);
    cJSON_AddNumberToObject(root, "hdmi_revert_seconds", jw__hdmi_revert_seconds(state));
    return jw__reply_json(client, root);
}

/* Brightness and volume alone, for the launcher's status-bar and Display &
   Sound polls. platform-status carries the same two numbers but re-derives the
   audio route and volume through three or four pactl/amixer spawns, ~100 ms on
   this loop -- the loop that forwards the pad -- and the launcher asks every
   second, or every 300 ms with Display & Sound open. Serve the cache instead
   (see JW_PLATFORM_LEVELS_TTL_MS). */
static int jw__reply_platform_levels(jw_daemon_state *state, jw_ipc_client *client) {
    long long now = jw__monotonic_ms();
    long long age = now - state->platform_cache_read_ms;
    bool unknown = state->cached_brightness_percent < 0 ||
                   state->cached_volume_percent < 0;
    if (state->platform_cache_read_ms == 0 || age >= JW_PLATFORM_LEVELS_TTL_MS ||
        (unknown && age >= JW_PLATFORM_LEVELS_RETRY_MS)) {
        jw__refresh_platform_cache(state);
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "platform-levels");
    cJSON_AddNumberToObject(root, "brightness_percent", state->cached_brightness_percent);
    cJSON_AddNumberToObject(root, "volume_percent", state->cached_volume_percent);
    cJSON_AddNumberToObject(root, "hdmi_revert_seconds", jw__hdmi_revert_seconds(state));
    return jw__reply_json(client, root);
}

static int jw__reply_platform_audio_status(jw_daemon_state *state, jw_ipc_client *client) {
    jw_platform_status status;
    jw_platform_get_audio_status(&state->platform, &status);
    jw__cache_platform_status(state, &status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "platform-audio-status");
    cJSON_AddItemToObject(root, "status", jw__platform_status_json(&status));
    return jw__reply_json(client, root);
}

static void jw__poll_update_install(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    jw_update_install_poll(&state->update_status, &state->update_install_job);
    if (!state->update_package_quiesce_active ||
        state->update_install_job.active) {
        return;
    }

    char reason[JW_SVC_REASON_BUF] = {0};
    bool ended = state->services &&
        jw_svc_supervisor_package_end(state->services, "leaf-update",
                                      reason, sizeof(reason));
    state->update_package_quiesce_active =
        state->services &&
        jw_svc_supervisor_package_active(state->services);
    if (!ended) {
        if (!state->update_package_quiesce_release_warned) {
            jw_log_error("update install: package quiesce release failed (%s)",
                         reason[0] ? reason : "unknown");
            state->update_package_quiesce_release_warned = true;
        }
        state->update_status.status = JW_UPDATE_STATUS_ERROR;
        snprintf(state->update_status.message,
                 sizeof(state->update_status.message),
                 "Update finished but service restore failed: %s",
                 reason[0] ? reason : "unknown");
    } else {
        state->update_package_quiesce_release_warned = false;
        jw_log_info("update install: package quiesce released after rescan");
    }
}

static int jw__reply_update_status(jw_daemon_state *state, jw_ipc_client *client) {
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw__poll_update_install(state);
    jw_update_refresh_installed(&state->update_status, state->state_dir);
    if (!state->update_install_job.active) {
        jw_update_refresh_install_result(&state->update_status, state->state_dir,
                                         state->sdcard_root);
    }
    cJSON *root = jw_update_status_to_json(&state->update_status);
    return jw__reply_json(client, root);
}

static const char *jw__scrape_state_name(jw_scrape_state state) {
    return state == JW_SCRAPE_RUNNING ? "running" :
           state == JW_SCRAPE_PAUSED_QUOTA ? "paused-quota" :
           state == JW_SCRAPE_PAUSED_STORAGE ? "paused-storage" : "idle";
}

static int jw__reply_scrape_status(jw_ipc_client *client, cJSON *request) {
    jw_scrape_status_info info;
    jw_scrape_status(&info);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "scrape-status");
    cJSON_AddStringToObject(root, "state", jw__scrape_state_name(info.state));
    cJSON_AddNumberToObject(root, "total", info.total);
    cJSON_AddNumberToObject(root, "done", info.done);
    cJSON_AddNumberToObject(root, "found", info.found);
    cJSON_AddNumberToObject(root, "not_found", info.not_found);
    cJSON_AddNumberToObject(root, "failed", info.failed);
    cJSON_AddNumberToObject(root, "cancelled", info.cancelled);
    cJSON_AddNumberToObject(root, "queued", info.queued);
    cJSON_AddNumberToObject(root, "active", info.active);
    cJSON_AddStringToObject(root, "current_name", info.current_name);
    cJSON_AddStringToObject(root, "current_system", info.current_system);
    cJSON_AddStringToObject(root, "message", info.message);

    /* Optional target: with "system" (and optionally "rom_path") in the
       request, also report whether that target has queued/in-flight work —
       drives the Actions menu's Scrape vs Cancel entries. */
    cJSON *system = request ? cJSON_GetObjectItemCaseSensitive(request, "system") : NULL;
    if (cJSON_IsString(system) && system->valuestring[0]) {
        cJSON *rom = cJSON_GetObjectItemCaseSensitive(request, "rom_path");
        bool pending = (cJSON_IsString(rom) && rom->valuestring[0])
            ? jw_scrape_is_pending_game(system->valuestring, rom->valuestring)
            : jw_scrape_is_pending_system(system->valuestring);
        cJSON_AddBoolToObject(root, "pending", pending);
    }
    return jw__reply_json(client, root);
}

static const char *jw__scrape_row_state_name(jw_scrape_row_state state) {
    switch (state) {
        case JW_SCRAPE_ROW_QUEUED:    return "queued";
        case JW_SCRAPE_ROW_HASH:      return "hashing";
        case JW_SCRAPE_ROW_SEARCH:    return "searching";
        case JW_SCRAPE_ROW_DOWNLOAD:  return "downloading";
        case JW_SCRAPE_ROW_SAVE:      return "saving";
        case JW_SCRAPE_ROW_DONE:      return "done";
        case JW_SCRAPE_ROW_NOT_FOUND: return "not-found";
        case JW_SCRAPE_ROW_ERROR:     return "error";
        case JW_SCRAPE_ROW_CANCELLED: return "cancelled";
        default:                      return "queued";
    }
}

static int jw__reply_scrape_queue(jw_ipc_client *client, cJSON *request) {
    int offset = 0;
    int limit = JW_SCRAPE_QUEUE_SNAPSHOT_MAX;
    cJSON *offset_json = request
        ? cJSON_GetObjectItemCaseSensitive(request, "offset") : NULL;
    cJSON *limit_json = request
        ? cJSON_GetObjectItemCaseSensitive(request, "limit") : NULL;
    if (cJSON_IsNumber(offset_json)) offset = offset_json->valueint;
    if (cJSON_IsNumber(limit_json)) limit = limit_json->valueint;

    jw_scrape_queue_info *info =
        (jw_scrape_queue_info *)calloc(1, sizeof(*info));
    if (!info) {
        return jw__reply_error(client, "out of memory");
    }
    jw_scrape_queue_snapshot(info, offset, limit);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "scrape-queue");
    const char *state_name =
        info->state == JW_SCRAPE_RUNNING ? "running" :
        info->state == JW_SCRAPE_PAUSED_QUOTA ? "paused-quota" :
        info->state == JW_SCRAPE_PAUSED_STORAGE ? "paused-storage" : "idle";
    cJSON_AddStringToObject(root, "state", state_name);
    cJSON_AddNumberToObject(root, "total", info->total);
    cJSON_AddNumberToObject(root, "done", info->done);
    cJSON_AddNumberToObject(root, "found", info->found);
    cJSON_AddNumberToObject(root, "not_found", info->not_found);
    cJSON_AddNumberToObject(root, "failed", info->failed);
    cJSON_AddNumberToObject(root, "cancelled", info->cancelled);
    cJSON_AddNumberToObject(root, "queued", info->queued);
    cJSON_AddNumberToObject(root, "active", info->active);
    cJSON_AddNumberToObject(root, "requests_today", info->requests_today);
    cJSON_AddNumberToObject(root, "max_requests", info->max_requests);
    cJSON_AddNumberToObject(root, "max_threads", info->max_threads);
    cJSON_AddNumberToObject(root, "permits", info->permits);
    cJSON_AddNumberToObject(root, "eta_seconds", info->eta_seconds);
    cJSON_AddStringToObject(root, "message", info->message);
    cJSON_AddNumberToObject(root, "offset", offset);
    cJSON_AddNumberToObject(root, "row_count", info->row_count);

    cJSON *rows = cJSON_CreateArray();
    for (int i = 0; i < info->row_count; i++) {
        jw_scrape_queue_row *row = &info->rows[i];
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "id", (double)row->id);
        cJSON_AddStringToObject(r, "state",
                                jw__scrape_row_state_name(row->state));
        cJSON_AddStringToObject(r, "display_name", row->display_name);
        cJSON_AddStringToObject(r, "system", row->system);
        cJSON_AddStringToObject(r, "rom_path", row->rom_path);
        cJSON_AddStringToObject(r, "output_path", row->output_path);
        cJSON_AddStringToObject(r, "message", row->message);
        cJSON_AddItemToArray(rows, r);
    }
    cJSON_AddItemToObject(root, "rows", rows);
    int rc = jw__reply_json(client, root);
    free(info);
    return rc;
}

static int jw__handle_scrape_start(jw_daemon_state *state,
                                   jw_ipc_client *client, cJSON *request) {
    if (jw_delete_job_busy(state->delete_job))
        return jw__reply_error(client, "ROM deletion is preparing or committing");
    cJSON *scope = cJSON_GetObjectItemCaseSensitive(request, "scope");
    cJSON *system = cJSON_GetObjectItemCaseSensitive(request, "system");
    cJSON *rom_path = cJSON_GetObjectItemCaseSensitive(request, "rom_path");
    cJSON *mode = cJSON_GetObjectItemCaseSensitive(request, "mode");
    if (!cJSON_IsString(scope)) {
        return jw__reply_error(client, "missing scope");
    }
    bool scope_all = strcmp(scope->valuestring, "all") == 0;
    if (!scope_all && (!cJSON_IsString(system) || !system->valuestring[0])) {
        return jw__reply_error(client, "missing system");
    }

    /* Gate on connectivity when the platform exposes Wi-Fi state, instead of
       letting the whole batch burn down as transport errors. */
    if (jw_wifi_available()) {
        jw_wifi_status_t wifi;
        if (jw_wifi_status(&wifi) == 0 && wifi.valid && !wifi.connected) {
            return jw__reply_error(client, "Wi-Fi is not connected");
        }
    }

    const char *error = NULL;
    int enqueued = -1;
    jw_scrape_enqueue_result result;
    memset(&result, 0, sizeof(result));
    if (strcmp(scope->valuestring, "game") == 0) {
        if (!cJSON_IsString(rom_path) || !rom_path->valuestring[0]) {
            return jw__reply_error(client, "missing rom_path");
        }
        enqueued = jw_scrape_enqueue_game(system->valuestring,
                                          rom_path->valuestring, &error);
        if (enqueued >= 0) {
            result.requested = 1;
            result.enqueued = enqueued;
            result.already_queued = enqueued == 0 ? 1 : 0;
        }
    } else if (strcmp(scope->valuestring, "system") == 0) {
        bool missing_only = !cJSON_IsString(mode) ||
                            strcmp(mode->valuestring, "all") != 0;
        enqueued = jw_scrape_enqueue_system_full(system->valuestring,
                                                 missing_only, &result,
                                                 &error);
    } else if (scope_all) {
        bool missing_only = !cJSON_IsString(mode) ||
                            strcmp(mode->valuestring, "all") != 0;
        enqueued = jw_scrape_enqueue_all_full(missing_only, &result, &error);
    } else {
        return jw__reply_error(client, "unknown scope");
    }

    if (enqueued < 0) {
        return jw__reply_error(client, error ? error : "scrape-start failed");
    }
    jw_log_info("scrape-start scope=%s system=%s requested=%d enqueued=%d already=%d skipped=%d full=%d",
                scope->valuestring,
                (cJSON_IsString(system) && system->valuestring[0])
                    ? system->valuestring : "*",
                result.requested, result.enqueued, result.already_queued,
                result.skipped_existing, result.queue_full ? 1 : 0);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", "scrape-start");
    cJSON_AddNumberToObject(root, "requested", result.requested);
    cJSON_AddNumberToObject(root, "enqueued", result.enqueued);
    cJSON_AddNumberToObject(root, "already_queued", result.already_queued);
    cJSON_AddNumberToObject(root, "skipped_existing", result.skipped_existing);
    cJSON_AddBoolToObject(root, "queue_full", result.queue_full);
    return jw__reply_json(client, root);
}

static int jw__handle_scrape_missing_counts(jw_daemon_state *state,
                                            jw_ipc_client *client,
                                            cJSON *request) {
    (void)state;
    (void)request;
    enum { JW__MISSING_CAP = 256 };
    jw_scrape_missing_row *rows = calloc(JW__MISSING_CAP, sizeof(*rows));
    if (!rows) return jw__reply_error(client, "out of memory");

    int count = 0, total_missing = 0;
    if (jw_scrape_missing_counts(rows, JW__MISSING_CAP, &count,
                                 &total_missing) != 0) {
        free(rows);
        return jw__reply_error(client, "could not compute missing counts");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", "scrape-missing-counts");
    cJSON_AddNumberToObject(root, "total_missing", total_missing);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "system", rows[i].system);
        cJSON_AddNumberToObject(o, "missing", rows[i].missing);
        cJSON_AddNumberToObject(o, "total", rows[i].total);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddItemToObject(root, "systems", arr);
    free(rows);
    return jw__reply_json(client, root);
}

static int jw__handle_scrape_cancel(jw_daemon_state *state,
                                    jw_ipc_client *client, cJSON *request) {
    (void)state;
    cJSON *scope = cJSON_GetObjectItemCaseSensitive(request, "scope");
    cJSON *system = cJSON_GetObjectItemCaseSensitive(request, "system");
    cJSON *rom_path = cJSON_GetObjectItemCaseSensitive(request, "rom_path");

    int removed = 0;
    if (!cJSON_IsString(scope) || strcmp(scope->valuestring, "all") == 0) {
        removed = jw_scrape_cancel_all();
    } else if (strcmp(scope->valuestring, "system") == 0 &&
               cJSON_IsString(system) && system->valuestring[0]) {
        removed = jw_scrape_cancel_system(system->valuestring);
    } else if (strcmp(scope->valuestring, "game") == 0 &&
               cJSON_IsString(system) && system->valuestring[0] &&
               cJSON_IsString(rom_path) && rom_path->valuestring[0]) {
        removed = jw_scrape_cancel_game(system->valuestring,
                                        rom_path->valuestring);
    } else {
        return jw__reply_error(client, "invalid cancel scope");
    }

    jw_log_info("scrape-cancel removed=%d", removed);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", "scrape-cancel");
    cJSON_AddNumberToObject(root, "removed", removed);
    return jw__reply_json(client, root);
}

static int jw__handle_scrape_stop_all(jw_daemon_state *state,
                                      jw_ipc_client *client) {
    (void)state;
    int stopped = jw_scrape_stop_all();
    jw_log_info("scrape-stop-all stopped=%d", stopped);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", "scrape-stop-all");
    cJSON_AddNumberToObject(root, "stopped", stopped);
    return jw__reply_json(client, root);
}

static int jw__handle_scrape_clear_done(jw_daemon_state *state,
                                        jw_ipc_client *client) {
    (void)state;
    int cleared = jw_scrape_clear_done();
    jw_log_info("scrape-clear-done cleared=%d", cleared);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", "scrape-clear-done");
    cJSON_AddNumberToObject(root, "cleared", cleared);
    return jw__reply_json(client, root);
}

/* Validate ScreenScraper user credentials for the settings UI. Synchronous
   network call in the handler, same trade-off as update-check: a few seconds
   of daemon-loop blocking for a rare, user-initiated action. */
static int jw__handle_scrape_validate(jw_daemon_state *state,
                                      jw_ipc_client *client,
                                      cJSON *request) {
    (void)state;
    cJSON *user = cJSON_GetObjectItemCaseSensitive(request, "username");
    cJSON *pass = cJSON_GetObjectItemCaseSensitive(request, "password");
    if (!cJSON_IsString(user) || !user->valuestring[0] ||
        !cJSON_IsString(pass) || !pass->valuestring[0]) {
        return jw__reply_error(client, "missing credentials");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "scrape-validate");

    if (!jw_ss_available()) {
        cJSON_AddBoolToObject(root, "valid", false);
        cJSON_AddBoolToObject(root, "rejected", false);
        cJSON_AddStringToObject(root, "message",
                                "scraping unavailable in this build");
        return jw__reply_json(client, root);
    }

    jw_ss_client ss = {0};
    snprintf(ss.username, sizeof(ss.username), "%s", user->valuestring);
    snprintf(ss.password, sizeof(ss.password), "%s", pass->valuestring);

    jw_ss_user info;
    int rc = jw_ss_validate_user(&ss, &info);

    cJSON_AddBoolToObject(root, "valid", rc == 0);
    cJSON_AddBoolToObject(root, "rejected", rc == 1);
    if (rc == 0) {
        cJSON_AddNumberToObject(root, "max_threads", info.max_threads);
        cJSON_AddNumberToObject(root, "requests_today", info.requests_today);
        cJSON_AddNumberToObject(root, "max_requests", info.max_requests);
        cJSON_AddNumberToObject(root, "user_level", info.user_level);
        jw_log_info("scrape-validate ok user=%s maxthreads=%d",
                    ss.username, info.max_threads);
    } else {
        const char *msg = jw_ss_last_error();
        cJSON_AddStringToObject(root, "message",
                                msg ? msg : "validation failed");
        jw_log_info("scrape-validate failed user=%s rc=%d", ss.username, rc);
    }
    return jw__reply_json(client, root);
}

static int jw__reply_update_status_raw(jw_daemon_state *state,
                                       jw_ipc_client *client) {
    /* Used only while an async release check is active: preserve CHECKING without
       refreshing installed/result fields or touching download/install state. */
    cJSON *reply = jw_update_status_to_json(&state->update_status);
    return jw__reply_json(client, reply);
}

static bool jw__update_check_busy(jw_daemon_state *state) {
    if (!state) {
        return false;
    }
    jw_update_check_poll(&state->update_status, &state->update_check_job);
    return state->update_check_job.active;
}

/* Update channel (Stable = Leaf repo, Beta = Leaf-beta repo) from the persisted
   setting; default Stable. */
static jw_update_channel jw__update_channel(jw_daemon_state *state) {
    char chan[16] = "";
    if (state->db_path[0] &&
        jw_db_get_setting(state->db_path, "update_channel", chan, sizeof(chan)) == 0 &&
        strcmp(chan, "beta") == 0) {
        return JW_UPDATE_CHANNEL_BETA;
    }
    return JW_UPDATE_CHANNEL_STABLE;
}

static int jw__handle_update_check(jw_daemon_state *state,
                                   jw_ipc_client *client,
                                   cJSON *request) {
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw__poll_update_install(state);
    jw__update_check_busy(state);
    if (state->update_download_job.active) {
        return jw__reply_update_status(state, client);
    }
    if (state->update_install_job.active) {
        return jw__reply_update_status(state, client);
    }
    if (state->update_check_job.active) {
        /* A check is already running; report CHECKING without starting another. */
        return jw__reply_update_status_raw(state, client);
    }

    cJSON *manifest_json = cJSON_GetObjectItemCaseSensitive(request, "manifest_path");
    const char *manifest_path = NULL;
    if (cJSON_IsString(manifest_json) && manifest_json->valuestring &&
        manifest_json->valuestring[0]) {
        manifest_path = manifest_json->valuestring;
    } else {
        const char *env = getenv("JAWAKA_UPDATE_MANIFEST");
        if (env && env[0]) {
            manifest_path = env;
        }
    }

    if (manifest_path && manifest_path[0]) {
        jw_update_check_local_manifest(&state->update_status,
                                       state->state_dir,
                                       state->platform.platform_id,
                                       manifest_path);
    } else {
        /* Run the GitHub release check on a worker thread so the blocking fetch
           doesn't freeze the launcher; reply immediately with state=checking and
           let the launcher's status poll pick up the result. */
        jw_update_check_start(&state->update_status,
                              &state->update_check_job,
                              state->state_dir,
                              jw__update_channel(state),
                              JW_UPDATE_SCOPE_LATEST);
    }
    cJSON *root = jw_update_status_to_json(&state->update_status);
    return jw__reply_json(client, root);
}

/* Load every release in the list for the release picker. The routine check only
   reads the newest one; this fills in the rest without touching the selected
   candidate. "refresh": true reloads a list that is already complete. */
static int jw__handle_update_releases(jw_daemon_state *state,
                                      jw_ipc_client *client,
                                      cJSON *request) {
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw__poll_update_install(state);
    jw__update_check_busy(state);
    if (state->update_download_job.active || state->update_install_job.active) {
        return jw__reply_update_status(state, client);
    }

    const char *manifest_env = getenv("JAWAKA_UPDATE_MANIFEST");
    bool local_manifest = manifest_env && manifest_env[0];
    bool refresh = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request, "refresh"));
    if (!local_manifest &&
        (refresh || !state->update_status.options_complete)) {
        jw_update_check_start(&state->update_status,
                              &state->update_check_job,
                              state->state_dir,
                              jw__update_channel(state),
                              JW_UPDATE_SCOPE_ALL);
    }
    if (state->update_check_job.active) {
        return jw__reply_update_status_raw(state, client);
    }
    return jw__reply_update_status(state, client);
}

static int jw__handle_update_download(jw_daemon_state *state,
                                      jw_ipc_client *client) {
    if (jw__update_check_busy(state)) {
        return jw__reply_update_status_raw(state, client);
    }
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw__poll_update_install(state);
    if (!state->update_download_job.active &&
        !state->update_install_job.active &&
        state->update_status.status != JW_UPDATE_STATUS_DOWNLOADED) {
        jw_update_download_start(&state->update_status,
                                 &state->update_download_job,
                                 state->state_dir);
    }
    return jw__reply_update_status(state, client);
}

static int jw__handle_update_select(jw_daemon_state *state,
                                    jw_ipc_client *client,
                                    cJSON *request) {
    if (jw__update_check_busy(state)) {
        return jw__reply_update_status_raw(state, client);
    }
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw__poll_update_install(state);
    if (state->update_download_job.active || state->update_install_job.active) {
        return jw__reply_update_status(state, client);
    }

    const cJSON *index_json = cJSON_GetObjectItemCaseSensitive(request, "option_index");
    int option_index = cJSON_IsNumber(index_json) ? index_json->valueint : -1;
    jw_update_select_option(&state->update_status, option_index);
    return jw__reply_update_status(state, client);
}

static int jw__handle_update_cancel(jw_daemon_state *state,
                                    jw_ipc_client *client) {
    if (jw__update_check_busy(state)) {
        return jw__reply_update_status_raw(state, client);
    }
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw__poll_update_install(state);
    jw_update_download_cancel(&state->update_status, &state->update_download_job);
    return jw__reply_update_status(state, client);
}

static bool jw__update_install_idle(const jw_daemon_state *state) {
    if (!state) {
        return false;
    }
    if (state->shutdown_requested ||
        state->pending_launch ||
        state->pending_app ||
        state->post_launch_resume_pending ||
        state->in_game_menu_prewarm_pending ||
        state->retroarch_session.active ||
        state->menu_in_game ||
        state->menu_visible ||
        state->update_download_job.active ||
        state->update_install_job.active ||
        state->update_check_job.active) {
        return false;
    }

    return state->child_kind == JW_CHILD_NONE ||
           state->child_kind == JW_CHILD_LAUNCHER ||
           state->child_kind == JW_CHILD_MENU;
}

static bool jw__update_handoff_replaces_live_payload(
    const jw_update_status *status) {
    if (!status) {
        return false;
    }
    return strcmp(status->handoff_type, "direct_runner") == 0 ||
           strcmp(status->handoff_type, "generic_runner") == 0 ||
           strcmp(status->handoff_type, "jawaka_c_runner") == 0;
}

static int jw__handle_update_install_preflight(jw_daemon_state *state,
                                               jw_ipc_client *client,
                                               cJSON *request) {
    if (jw__update_check_busy(state)) {
        return jw__reply_update_status_raw(state, client);
    }
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw__poll_update_install(state);

    bool confirm_unknown_battery =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,
                                                      "confirm_unknown_battery"));
    jw_platform_status platform_status;
    jw_platform_get_status(&state->platform, &platform_status);
    jw__cache_platform_status(state, &platform_status);

    jw_update_install_preflight(&state->update_status,
                                state->state_dir,
                                state->sdcard_root,
                                jw__update_install_idle(state),
                                platform_status.battery_percent,
                                platform_status.charging,
                                confirm_unknown_battery);
    return jw__reply_update_status(state, client);
}

static int jw__handle_update_install(jw_daemon_state *state,
                                     jw_ipc_client *client,
                                     cJSON *request) {
    if (jw__update_check_busy(state)) {
        return jw__reply_update_status_raw(state, client);
    }
    jw_update_download_poll(&state->update_status, &state->update_download_job);
    jw__poll_update_install(state);
    if (state->update_install_job.active) {
        return jw__reply_update_status(state, client);
    }

    bool confirm_unknown_battery =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,
                                                      "confirm_unknown_battery"));
    jw_platform_status platform_status;
    jw_platform_get_status(&state->platform, &platform_status);
    jw__cache_platform_status(state, &platform_status);

    jw_update_install_preflight(&state->update_status,
                                state->state_dir,
                                state->sdcard_root,
                                jw__update_install_idle(state),
                                platform_status.battery_percent,
                                platform_status.charging,
                                confirm_unknown_battery);

    if (state->update_status.install_ready) {
        char runner_path[PATH_MAX];
        if (snprintf(runner_path, sizeof(runner_path),
                     "%s/jawaka-update-runner", state->bin_dir) <
            (int)sizeof(runner_path)) {
            bool needs_quiesce =
                jw__update_handoff_replaces_live_payload(
                    &state->update_status);
            bool quiesced = false;
            if (needs_quiesce) {
                char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
                char reason[JW_SVC_REASON_BUF] = {0};
                quiesced = state->services &&
                    jw_svc_supervisor_package_begin(
                        state->services, "leaf-update", stuck, sizeof(stuck),
                        reason, sizeof(reason));
                state->update_package_quiesce_release_warned = false;
                state->update_package_quiesce_active =
                    state->services &&
                    jw_svc_supervisor_package_active(state->services);
                if (!quiesced) {
                    state->update_status.status = JW_UPDATE_STATUS_ERROR;
                    if (!state->services) {
                        snprintf(state->update_status.message,
                                 sizeof(state->update_status.message), "%s",
                                 "Service supervisor unavailable; update not started");
                    } else if (stuck[0]) {
                        snprintf(state->update_status.message,
                                 sizeof(state->update_status.message),
                                 "Service quiesce failed: %s (%s)",
                                 stuck, reason[0] ? reason : "unknown");
                    } else {
                        snprintf(state->update_status.message,
                                 sizeof(state->update_status.message),
                                 "Service quiesce failed: %s",
                                 reason[0] ? reason : "unknown");
                    }
                }
            }
            if (!needs_quiesce || quiesced) {
                int start_rc = jw_update_install_start(
                    &state->update_status, &state->update_install_job,
                    state->state_dir, state->sdcard_root, runner_path);
                if (start_rc != 0 && quiesced) {
                    char release_reason[JW_SVC_REASON_BUF] = {0};
                    (void)jw_svc_supervisor_package_end(
                        state->services, "leaf-update", release_reason,
                        sizeof(release_reason));
                    state->update_package_quiesce_active =
                        jw_svc_supervisor_package_active(state->services);
                }
            }
        } else {
            state->update_status.status = JW_UPDATE_STATUS_ERROR;
            snprintf(state->update_status.message,
                     sizeof(state->update_status.message),
                     "%s", "Update runner path is too long");
        }
    }

    cJSON *root = jw_update_status_to_json(&state->update_status);
    return jw__reply_json(client, root);
}

static const char *jw__package_operation_id(cJSON *request) {
    cJSON *value = request
        ? cJSON_GetObjectItemCaseSensitive(request, "operation_id") : NULL;
    return cJSON_IsString(value) && value->valuestring
               ? value->valuestring : NULL;
}

static int jw__handle_package_quiesce_begin(jw_daemon_state *state,
                                            jw_ipc_client *client,
                                            cJSON *request) {
    const char *operation_id = jw__package_operation_id(request);
    if (!state->services) {
        return jw__reply_error(client, "service supervisor unavailable");
    }
    if (state->shutdown_requested || state->pending_app ||
        jw__child_kind_is_app(state->child_kind) ||
        state->update_install_job.active) {
        return jw__reply_error(client,
                               "foreground or update operation in progress");
    }
    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    char reason[JW_SVC_REASON_BUF] = {0};
    if (!jw_svc_supervisor_package_begin(
            state->services, operation_id, stuck, sizeof(stuck), reason,
            sizeof(reason))) {
        char message[320];
        if (stuck[0]) {
            snprintf(message, sizeof(message),
                     "package quiesce failed: %.127s (%.127s)", stuck,
                     reason[0] ? reason : "unknown");
        } else {
            snprintf(message, sizeof(message),
                     "package quiesce failed: %.127s",
                     reason[0] ? reason : "unknown");
        }
        return jw__reply_error(client, message);
    }
    jw_log_info("package quiesce: begin operation=%s", operation_id);
    return jw__reply_ok(client, "package-quiesce-begin", NULL);
}

static int jw__handle_package_quiesce_end(jw_daemon_state *state,
                                          jw_ipc_client *client,
                                          cJSON *request) {
    const char *operation_id = jw__package_operation_id(request);
    if (!state->services) {
        return jw__reply_error(client, "service supervisor unavailable");
    }
    char reason[JW_SVC_REASON_BUF] = {0};
    if (!jw_svc_supervisor_package_end(state->services, operation_id,
                                       reason, sizeof(reason))) {
        char message[256];
        snprintf(message, sizeof(message),
                 "package quiesce release failed: %.127s",
                 reason[0] ? reason : "unknown");
        return jw__reply_error(client, message);
    }
    jw_log_info("package quiesce: end operation=%s", operation_id);
    return jw__reply_ok(client, "package-quiesce-end", NULL);
}

static const char *jw__package_request_string(cJSON *request,
                                              const char *key) {
    cJSON *value = request
        ? cJSON_GetObjectItemCaseSensitive(request, key) : NULL;
    return cJSON_IsString(value) && value->valuestring
               ? value->valuestring : NULL;
}

static int jw__installed_service_state(jw_daemon_state *state,
                                       const char *target_path,
                                       const char *package_id,
                                       bool *out_has_service) {
    if (!state || !target_path || !package_id || !out_has_service) {
        return -1;
    }
    *out_has_service = false;
    jw_pakrat_install install;
    int install_rc = jw_db_pakrat_get_install(state->db_path, package_id,
                                               &install);
    if (install_rc == 1) {
        return 0;
    }
    if (install_rc != 0 || strcmp(install.install_path, target_path) != 0) {
        return -1;
    }
    jw_pakrat_txn_metadata metadata;
    int metadata_rc = jw_pakrat_txn_metadata_get(
        state->db_path, package_id, &metadata);
    if (metadata_rc != 0) {
        return -1;
    }
    int valid = strcmp(metadata.store_id, package_id) == 0 &&
                strcmp(metadata.package_id, package_id) == 0 &&
                strcmp(metadata.install_path, target_path) == 0;
    if (valid) {
        *out_has_service = metadata.has_service;
    }
    jw_pakrat_txn_metadata_destroy(&metadata);
    return valid ? 0 : -1;
}

static int jw__handle_package_mutation_begin(jw_daemon_state *state,
                                             jw_ipc_client *client,
                                             cJSON *request) {
    const char *operation_id = jw__package_operation_id(request);
    const char *target_path = jw__package_request_string(request, "target_path");
    const char *package_id = jw__package_request_string(request, "package_id");
    if (!state->services) {
        return jw__reply_error(client, "service supervisor unavailable");
    }
    if (state->shutdown_requested || state->pending_app ||
        jw__child_kind_is_app(state->child_kind) ||
        state->update_install_job.active) {
        return jw__reply_error(client,
                               "foreground or update operation in progress");
    }
    if (!jw_pakrat_mutation_lock_is_held(
            state->runtime_dir, operation_id, package_id, target_path)) {
        return jw__reply_error(client, "mutation lock is not held");
    }
    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    char reason[JW_SVC_REASON_BUF] = {0};
    if (!jw_svc_supervisor_mutation_begin(
            state->services, operation_id, target_path, package_id,
            stuck, sizeof(stuck), reason, sizeof(reason))) {
        char message[320];
        if (stuck[0]) {
            snprintf(message, sizeof(message),
                     "package mutation failed: %.127s (%.127s)", stuck,
                     reason[0] ? reason : "unknown");
        } else {
            snprintf(message, sizeof(message),
                     "package mutation failed: %.127s",
                     reason[0] ? reason : "unknown");
        }
        return jw__reply_error(client, message);
    }
    jw_log_info("package mutation: begin operation=%s target=%s package=%s",
                operation_id, target_path, package_id);
    return jw__reply_ok(client, "package-mutation-begin", NULL);
}

static int jw__handle_package_mutation_end(jw_daemon_state *state,
                                           jw_ipc_client *client,
                                           cJSON *request) {
    const char *requested_operation = jw__package_operation_id(request);
    char operation_id[JW_SVC_PACKAGE_OPERATION_ID_MAX + 1] = {0};
    char target_path[JW_PAKRAT_TXN_TARGET_MAX + 1] = {0};
    char package_id[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    if (!state->services) {
        return jw__reply_error(client, "service supervisor unavailable");
    }
    if (!jw_svc_supervisor_mutation_info(
            state->services, operation_id, sizeof(operation_id),
            target_path, sizeof(target_path), package_id,
            sizeof(package_id))) {
        return jw__reply_error(client, "no package mutation in progress");
    }
    if (!requested_operation ||
        strcmp(requested_operation, operation_id) != 0) {
        return jw__reply_error(client, "package mutation operation mismatch");
    }
    if (!jw_pakrat_mutation_lock_is_held(
            state->runtime_dir, operation_id, package_id, target_path)) {
        return jw__reply_error(client, "mutation lock is not held");
    }
    bool installed_has_service = false;
    if (jw__installed_service_state(state, target_path, package_id,
                                    &installed_has_service) != 0) {
        return jw__reply_error(client,
                               "installed package metadata is unavailable");
    }
    char reason[JW_SVC_REASON_BUF] = {0};
    if (!jw_svc_supervisor_mutation_end(
            state->services, operation_id, installed_has_service,
            reason, sizeof(reason))) {
        char message[256];
        snprintf(message, sizeof(message),
                 "package mutation release failed: %.127s",
                 reason[0] ? reason : "unknown");
        return jw__reply_error(client, message);
    }
    jw_log_info("package mutation: end operation=%s target=%s package=%s",
                operation_id, target_path, package_id);
    return jw__reply_ok(client, "package-mutation-end", NULL);
}

static void jw__daemon_pakrat_context(const jw_daemon_state *state,
                                      jw_pakrat_context *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->platform, sizeof(ctx->platform), "%s",
             jw_platform_compiled_id());
    snprintf(ctx->sdcard_root, sizeof(ctx->sdcard_root), "%s",
             state->sdcard_root);
    snprintf(ctx->state_dir, sizeof(ctx->state_dir), "%s", state->state_dir);
    snprintf(ctx->db_path, sizeof(ctx->db_path), "%s", state->db_path);
    snprintf(ctx->runtime_dir, sizeof(ctx->runtime_dir), "%s",
             state->runtime_dir);
    snprintf(ctx->socket_path, sizeof(ctx->socket_path), "%s",
             state->socket_path);
    const char *platform_root = getenv("UMRK_PLATFORM_PATH");
    if (platform_root) {
        snprintf(ctx->platform_root, sizeof(ctx->platform_root), "%s",
                 platform_root);
    }
}

static int jw__run_pakrat_p1_recovery(const jw_daemon_state *state) {
    jw_pakrat_recovery_context recovery;
    memset(&recovery, 0, sizeof(recovery));
    snprintf(recovery.platform, sizeof(recovery.platform), "%s",
             jw_platform_compiled_id());
    snprintf(recovery.sdcard_root, sizeof(recovery.sdcard_root), "%s",
             state->sdcard_root);
    snprintf(recovery.state_dir, sizeof(recovery.state_dir), "%s",
             state->state_dir);
    snprintf(recovery.db_path, sizeof(recovery.db_path), "%s",
             state->db_path);
    return jw_pakrat_recover_installs(&recovery);
}

static void jw__free_pending_uninstalls(jw_pakrat_pending_uninstall *items,
                                        int count) {
    for (int i = 0; i < count; i++) {
        jw_pakrat_pending_uninstall_destroy(&items[i]);
    }
    free(items);
}

/* Returns 0 when no TXN-1 work remains, 1 when a live owner or an absent
   source requires a later retry, and -1 on a recoverable local error. A daemon-
   owned lock is deliberately retained across nonzero returns so no service or
   foreground launch can race incomplete recovery. */
static long long jw__stat_time_ns(const struct stat *st, bool change) {
#if defined(__APPLE__)
    const struct timespec *ts = change ? &st->st_ctimespec : &st->st_mtimespec;
#else
    const struct timespec *ts = change ? &st->st_ctim : &st->st_mtim;
#endif
    return (long long)ts->tv_sec * 1000000000LL + (long long)ts->tv_nsec;
}

static void jw__db_side_file_sig(const char *db_path, const char *suffix,
                                 long long out[2]) {
    char path[PATH_MAX];
    struct stat st;
    if (snprintf(path, sizeof(path), "%s%s", db_path, suffix) < (int)sizeof(path) &&
        stat(path, &st) == 0) {
        out[0] = (long long)st.st_size;
        out[1] = jw__stat_time_ns(&st, false);
    } else {
        out[0] = out[1] = -1;
    }
}

/* True when library.db (or its rollback journal or WAL) changed since *sig
   was last taken, or on the first call; refreshes *sig. A failed stat counts
   as a change so the caller falls back to reading. */
static bool jw__db_file_changed(const char *db_path, jw_db_file_sig *sig) {
    jw_db_file_sig now;
    memset(&now, 0, sizeof(now));
    struct stat st;
    if (!db_path || stat(db_path, &st) != 0) {
        sig->valid = false;
        return true;
    }
    now.valid = true;
    now.db[0] = (long long)st.st_ino;
    now.db[1] = (long long)st.st_size;
    now.db[2] = jw__stat_time_ns(&st, false);
    now.db[3] = jw__stat_time_ns(&st, true);
    jw__db_side_file_sig(db_path, "-journal", now.journal);
    jw__db_side_file_sig(db_path, "-wal", now.wal);
    bool changed = !sig->valid;
    for (int i = 0; i < 4 && !changed; i++) changed = now.db[i] != sig->db[i];
    for (int i = 0; i < 2 && !changed; i++) {
        changed = now.journal[i] != sig->journal[i] || now.wal[i] != sig->wal[i];
    }
    *sig = now;
    return changed;
}

static int jw__recover_package_mutations(jw_daemon_state *state) {
    if (!state || !state->services) {
        return -1;
    }
    jw_pakrat_context ctx;
    jw__daemon_pakrat_context(state, &ctx);

    for (int pass = 0; pass < 1024; pass++) {
        if (jw_svc_supervisor_mutation_active(state->services)) {
            char operation_id[JW_SVC_PACKAGE_OPERATION_ID_MAX + 1] = {0};
            char target_path[JW_PAKRAT_TXN_TARGET_MAX + 1] = {0};
            char package_id[JW_SVC_SUPERVISOR_ID_BUF] = {0};
            if (!jw_svc_supervisor_mutation_info(
                    state->services, operation_id, sizeof(operation_id),
                    target_path, sizeof(target_path), package_id,
                    sizeof(package_id))) {
                return -1;
            }
            if (state->mutation_recovery_lock.fd < 0) {
                if (jw_pakrat_mutation_lock_is_held(
                        state->runtime_dir, operation_id, package_id,
                        target_path)) {
                    return 1;
                }
                char reason[JW_SVC_REASON_BUF] = {0};
                if (jw_pakrat_mutation_lock_acquire(
                        state->runtime_dir, operation_id, package_id,
                        target_path, &state->mutation_recovery_lock,
                        reason, sizeof(reason)) != 0) {
                    return 1;
                }
                jw_log_warn("package mutation: adopting abandoned operation=%s",
                            operation_id);
            }

            jw_pakrat_pending_uninstall pending;
            int pending_rc = jw_pakrat_txn_pending_get(
                state->db_path, package_id, &pending);
            bool installed_has_service = false;
            if (pending_rc == 0) {
                int complete_rc = jw_pakrat_txn_complete_uninstall(&ctx,
                                                                    &pending);
                jw_pakrat_pending_uninstall_destroy(&pending);
                if (complete_rc != 0) {
                    return complete_rc > 0 ? 1 : -1;
                }
            } else if (pending_rc == 1) {
                if (jw__run_pakrat_p1_recovery(state) != 0 ||
                    jw__installed_service_state(
                        state, target_path, package_id,
                        &installed_has_service) != 0) {
                    return -1;
                }
            } else {
                return -1;
            }

            char reason[JW_SVC_REASON_BUF] = {0};
            if (!jw_svc_supervisor_mutation_end(
                    state->services, operation_id, installed_has_service,
                    reason, sizeof(reason))) {
                jw_log_warn("package mutation: recovery release failed (%s)",
                            reason[0] ? reason : "unknown");
                return -1;
            }
            jw_pakrat_mutation_lock_release(
                &state->mutation_recovery_lock);
            jw_log_info("package mutation: recovery complete operation=%s",
                        operation_id);
            continue;
        }

        /* Pending uninstalls only appear through a library.db write. Take the
           file's signature first, so a write that lands during the query is
           seen as a change next time. */
        jw_db_file_sig before = state->mutation_recovery_db_sig;
        if (!jw__db_file_changed(state->db_path, &state->mutation_recovery_db_sig)) {
            return 0;
        }
        jw_pakrat_pending_uninstall *pending = NULL;
        int pending_count = 0;
        if (jw_pakrat_txn_pending_list(state->db_path, &pending,
                                       &pending_count) != 0) {
            state->mutation_recovery_db_sig = before;
            state->mutation_recovery_db_sig.valid = false;
            return -1;
        }
        if (pending_count == 0) {
            free(pending);
            return 0;
        }
        /* Work to do: forget the signature so the pass after it re-checks. */
        state->mutation_recovery_db_sig.valid = false;

        char operation_id[JW_SVC_PACKAGE_OPERATION_ID_MAX + 1];
        int operation_size = snprintf(
            operation_id, sizeof(operation_id), "recover-%ld-%u",
            (long)getpid(), ++state->mutation_recovery_sequence);
        char lock_reason[JW_SVC_REASON_BUF] = {0};
        char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
        char supervisor_reason[JW_SVC_REASON_BUF] = {0};
        bool started = operation_size > 0 &&
                       operation_size < (int)sizeof(operation_id) &&
                       jw_pakrat_mutation_lock_acquire(
                           state->runtime_dir, operation_id,
                           pending[0].metadata.package_id,
                           pending[0].metadata.install_path,
                           &state->mutation_recovery_lock, lock_reason,
                           sizeof(lock_reason)) == 0;
        if (started && !jw_svc_supervisor_mutation_begin(
                           state->services, operation_id,
                           pending[0].metadata.install_path,
                           pending[0].metadata.package_id, stuck,
                           sizeof(stuck), supervisor_reason,
                           sizeof(supervisor_reason))) {
            jw_pakrat_mutation_lock_release(
                &state->mutation_recovery_lock);
            started = false;
        }
        jw__free_pending_uninstalls(pending, pending_count);
        if (!started) {
            jw_log_warn("package mutation: pending uninstall recovery deferred (%s)",
                        supervisor_reason[0]
                            ? supervisor_reason
                            : (lock_reason[0] ? lock_reason : "unknown"));
            return 1;
        }
        /* The next loop pass completes the active operation while retaining
           the daemon-owned flock. */
    }
    return -1;
}

static void jw__tick_package_mutation_recovery(jw_daemon_state *state) {
    if (!state || !state->services) {
        return;
    }
    long long now = jw__monotonic_ms();
    if (now < state->mutation_recovery_next_ms) {
        return;
    }
    state->mutation_recovery_next_ms = now + 500;
    int rc = jw__recover_package_mutations(state);
    if (rc == 0 && state->pakrat_startup_recovery_needed) {
        int recovery_rc = jw__run_pakrat_p1_recovery(state);
        if (recovery_rc == JW_PAKRAT_RECOVERY_OK) {
            state->pakrat_startup_recovery_needed = false;
            if (jw_svc_supervisor_scan(state->services) < 0) {
                jw_log_warn("pakrat: post-recovery service rescan failed");
            }
        } else if (recovery_rc == JW_PAKRAT_RECOVERY_REPAIR_REQUIRED) {
            state->pakrat_startup_recovery_needed = false;
            jw_log_error("pakrat: install-transition recovery requires manual repair; automatic retries stopped");
        } else {
            jw_log_warn("pakrat: install-transition recovery will retry");
        }
    }
}

static int jw__reply_platform_result(jw_ipc_client *client, const char *action,
                                     const jw_platform_result *result) {
    bool ok = result && result->code == JW_PLATFORM_RESULT_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", ok ? "ok" : "error");
    if (action) {
        cJSON_AddStringToObject(root, "action", action);
    }
    cJSON_AddStringToObject(root, "code",
                            jw_platform_result_code_name(result ? result->code
                                                                : JW_PLATFORM_RESULT_FAILED));
    cJSON_AddStringToObject(root, "message", result ? result->message : "platform action failed");
    if (result && result->has_value) {
        cJSON_AddNumberToObject(root, "value", result->value);
    }
    return jw__reply_json(client, root);
}

static void jw__load_library_generation(jw_daemon_state *state) {
    if (!state || !state->db_path) {
        return;
    }
    char value[32];
    if (jw_db_get_setting(state->db_path, "library.generation",
                          value, sizeof(value)) == 0 && value[0]) {
        state->library_generation = atoi(value);
        if (state->library_generation < 0) {
            state->library_generation = 0;
        }
    }
}

static void jw__persist_library_generation(jw_daemon_state *state) {
    if (!state || !state->db_path) {
        return;
    }
    char value[32];
    snprintf(value, sizeof(value), "%d", state->library_generation);
    if (jw_db_set_setting(state->db_path, "library.generation", value) != 0) {
        jw_log_warn("could not persist library generation");
    }
}

static void jw__bump_library_generation(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    state->library_generation += 1;
    if (state->library_generation <= 0) {
        state->library_generation = 1;
    }
    jw__persist_library_generation(state);
}

static bool jw__scan_title_provider_valid(const char *provider) {
    if (!provider || !provider[0] ||
        strlen(provider) >= JW_SCAN_TITLE_PROVIDER_MAX ||
        !isalnum((unsigned char)provider[0])) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)provider; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-') return false;
    }
    return true;
}

static bool jw__scan_title_text_valid(const char *title) {
    if (!title || !title[0] || strlen(title) >= JW_SCAN_TITLE_MAX) return false;
    for (const unsigned char *p = (const unsigned char *)title; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
        if (*p < 0x80) continue;
        int need = 0;
        unsigned int value = 0;
        unsigned int minimum = 0;
        if ((*p & 0xe0) == 0xc0) {
            need = 1; value = *p & 0x1f; minimum = 0x80;
        } else if ((*p & 0xf0) == 0xe0) {
            need = 2; value = *p & 0x0f; minimum = 0x800;
        } else if ((*p & 0xf8) == 0xf0) {
            need = 3; value = *p & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        for (int i = 0; i < need; i++) {
            p++;
            if ((*p & 0xc0) != 0x80) return false;
            value = (value << 6) | (*p & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

static bool jw__scan_path_within(const char *path, const char *root) {
    if (!path || !root || !root[0]) return false;
    size_t root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/') root_len--;
    return strncmp(path, root, root_len) == 0 &&
           (path[root_len] == '/' || path[root_len] == '\0');
}

static int jw__normalize_scan_title_path(const jw_storage_source_list *sources,
                                         const char *requested,
                                         char *out, size_t out_size) {
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];
    struct stat st;
    if (!sources || !requested || !requested[0] || !out || out_size == 0 ||
        jw_storage_resolve_path(sources, requested, candidate,
                                sizeof(candidate)) != 0 ||
        !realpath(candidate, resolved) || stat(resolved, &st) != 0 ||
        !S_ISREG(st.st_mode)) {
        return -1;
    }

    const jw_storage_source *matched = NULL;
    char matched_root[PATH_MAX] = "";
    char matched_roms[PATH_MAX] = "";
    for (int i = 0; i < sources->count; i++) {
        char root[PATH_MAX];
        char roms[PATH_MAX];
        const jw_storage_source *source = &sources->sources[i];
        if (!realpath(source->root, root) || !realpath(source->roms_path, roms)) {
            continue;
        }
        if (jw__scan_path_within(resolved, roms) &&
            (!matched || strlen(root) > strlen(matched_root))) {
            matched = source;
            snprintf(matched_root, sizeof(matched_root), "%s", root);
            snprintf(matched_roms, sizeof(matched_roms), "%s", roms);
        }
    }
    if (!matched || !jw__scan_path_within(resolved, matched_root)) return -1;

    const char *inside_roms = resolved + strlen(matched_roms);
    while (*inside_roms == '/') inside_roms++;
    char relative[PATH_MAX];
    char scanner_absolute[PATH_MAX];
    int rel_len = snprintf(relative, sizeof(relative), "Roms/%s", inside_roms);
    int abs_len = snprintf(scanner_absolute, sizeof(scanner_absolute), "%s/%s",
                           matched->roms_path, inside_roms);
    if (rel_len < 0 || rel_len >= (int)sizeof(relative) ||
        abs_len < 0 || abs_len >= (int)sizeof(scanner_absolute)) {
        return -1;
    }
    return jw_storage_db_path_for_source(matched, relative, scanner_absolute,
                                         out, out_size);
}

static int jw__parse_scan_title_groups(jw_daemon_state *state, cJSON *request,
                                       jw_scan_title_list *out,
                                       const char **out_error) {
    memset(out, 0, sizeof(*out));
    cJSON *groups_json = cJSON_GetObjectItemCaseSensitive(request, "title_groups");
    if (!groups_json) return 0;
    if (!cJSON_IsArray(groups_json)) {
        if (out_error) *out_error = "title_groups must be an array";
        return -1;
    }

    jw_storage_source_list sources;
    if (!state || jw_storage_sources_resolve(state->sdcard_root, &sources) != 0) {
        if (out_error) *out_error = "storage sources unavailable";
        return -1;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, groups_json) {
        cJSON *provider = cJSON_GetObjectItemCaseSensitive(item, "provider");
        cJSON *title = cJSON_GetObjectItemCaseSensitive(item, "title");
        cJSON *paths = cJSON_GetObjectItemCaseSensitive(item, "rom_paths");
        if (!cJSON_IsObject(item) || !cJSON_IsString(provider) ||
            !jw__scan_title_provider_valid(provider->valuestring) ||
            !cJSON_IsString(title) || !jw__scan_title_text_valid(title->valuestring) ||
            !cJSON_IsArray(paths) || cJSON_GetArraySize(paths) <= 0) {
            if (out_error) *out_error = "invalid title group";
            goto fail;
        }

        jw_scan_title_group group;
        memset(&group, 0, sizeof(group));
        snprintf(group.provider, sizeof(group.provider), "%s", provider->valuestring);
        snprintf(group.title, sizeof(group.title), "%s", title->valuestring);
        int requested_count = cJSON_GetArraySize(paths);
        group.rom_paths = calloc((size_t)requested_count, sizeof(*group.rom_paths));
        if (!group.rom_paths) {
            if (out_error) *out_error = "out of memory";
            goto fail;
        }

        cJSON *path_json = NULL;
        cJSON_ArrayForEach(path_json, paths) {
            char normalized[PATH_MAX];
            if (!cJSON_IsString(path_json) || !path_json->valuestring[0] ||
                jw__normalize_scan_title_path(&sources, path_json->valuestring,
                                              normalized, sizeof(normalized)) != 0) {
                if (out_error) *out_error = "title path is not a mounted ROM file";
                for (int p = 0; p < group.rom_path_count; p++) free(group.rom_paths[p]);
                free(group.rom_paths);
                goto fail;
            }
            bool duplicate = false;
            for (int p = 0; p < group.rom_path_count; p++) {
                if (strcmp(group.rom_paths[p], normalized) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) continue;
            group.rom_paths[group.rom_path_count] = strdup(normalized);
            if (!group.rom_paths[group.rom_path_count]) {
                if (out_error) *out_error = "out of memory";
                for (int p = 0; p < group.rom_path_count; p++) free(group.rom_paths[p]);
                free(group.rom_paths);
                goto fail;
            }
            group.rom_path_count++;
        }
        if (group.rom_path_count == 0) {
            if (out_error) *out_error = "title group has no ROM paths";
            free(group.rom_paths);
            goto fail;
        }

        jw_scan_title_list one = {0};
        if (jw__scan_title_list_append(&one, &group) != 0 ||
            jw__scan_title_list_merge(out, &one) != 0) {
            if (out_error) *out_error = "title groups exceed the IPC budget";
            jw__scan_title_list_free(&one);
            for (int p = 0; p < group.rom_path_count; p++) free(group.rom_paths[p]);
            free(group.rom_paths);
            goto fail;
        }
        jw__scan_title_list_free(&one);
        for (int p = 0; p < group.rom_path_count; p++) free(group.rom_paths[p]);
        free(group.rom_paths);
    }
    return 0;

fail:
    jw__scan_title_list_free(out);
    return -1;
}

/* CAT-1: publish the effective catalog. Full provenance validation lives
   here and ONLY here -- readers do structural validation per load and never
   rehash a catalog input on a request path.
   See umrk-workspace/plans/pak-rat/content-paks-contract.md#cat-1. */
static void jw__catalog_refresh(const char *sdcard_root, const char *when) {
    char generation[JW_CAT_GEN_NAME_MAX] = "";
    char reason[JW_CAT_REASON_MAX] = "";
    int rc = jw_discovery_catalog_refresh(sdcard_root, generation,
                                          sizeof(generation), reason,
                                          sizeof(reason));
    if (rc == 0) {
        jw_log_info("catalog: %s %s (%s)", reason, generation, when);
    } else {
        /* Loud on purpose. Running on release defaults is correct and safe,
           but it means a content pak silently does nothing, and the only
           other place that says so is diagnostics.json. */
        jw_log_warn("catalog: running on release defaults reason=%s (%s)",
                    reason[0] ? reason : "unknown", when);
    }
}

static void jw__catalog_init(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    /* Abandoned temp directories only. A finalized generation is never
       pruned in v1: a CentralScrutinizer process can outlive a jawakad
       crash, so even daemon startup is not a reader-free window. */
    size_t removed = 0;
    if (jw_catalog_cleanup_temps(state->sdcard_root, &removed) == 0 && removed > 0) {
        jw_log_info("catalog: removed %zu abandoned temp director%s",
                    removed, removed == 1 ? "y" : "ies");
    }
    jw__catalog_refresh(state->sdcard_root, "startup");
}

static void jw__scan_job_init(jw_daemon_state *state) {
    if (!state || state->scan_job.initialized) {
        return;
    }
    pthread_mutex_init(&state->scan_job.mu, NULL);
    state->scan_job.initialized = true;
}

static void *jw__scan_job_main(void *arg) {
    jw_scan_job *job = (jw_scan_job *)arg;
    char db_path[PATH_MAX];
    char sdcard_root[PATH_MAX];
    char reason[96];
    jw_scan_title_list titles = {0};

    pthread_mutex_lock(&job->mu);
    snprintf(db_path, sizeof(db_path), "%s", job->db_path);
    snprintf(sdcard_root, sizeof(sdcard_root), "%s", job->sdcard_root);
    snprintf(reason, sizeof(reason), "%s", job->reason);
    jw__scan_title_list_move(&titles, &job->titles);
    pthread_mutex_unlock(&job->mu);

    jw_scan_result result;
    memset(&result, 0, sizeof(result));
    jw_db_imported_title_result title_result = {0};
    title_result.groups = titles.group_count;
    title_result.paths = jw__scan_title_list_path_count(&titles);
    char title_error[160] = "";
    char error[160] = "";
    int ok = 0;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0) {
        snprintf(error, sizeof(error), "%s", "could not open scan database");
        if (db) {
            jw_db_close(db);
        }
        ok = 0;
    } else {
        const char *test_delay = getenv("JAWAKA_SCAN_TEST_DELAY_MS");
        if (test_delay && test_delay[0]) {
            char *end = NULL;
            long delay_ms = strtol(test_delay, &end, 10);
            if (end && *end == '\0' && delay_ms > 0 && delay_ms <= 5000)
                jw__usleep((useconds_t)delay_ms * 1000u);
        }
        if (jw_scan_library(db, sdcard_root, &result) != 0) {
        snprintf(error, sizeof(error), "scan failed reason=%s",
                 reason[0] ? reason : "unknown");
        jw_db_close(db);
        ok = 0;
        } else {
        jw_storage_source_list pico8_sources;
        jw_pico8_paths pico8_paths;
        char pico8_report[PATH_MAX];
        if (jw_storage_sources_resolve(sdcard_root, &pico8_sources) == 0 &&
            jw_pico8_resolve_paths(&pico8_sources, NULL, &pico8_paths) &&
            snprintf(pico8_report, sizeof(pico8_report), "%s/splore-library.tsv", pico8_paths.home) < (int)sizeof(pico8_report) &&
            jw_pico8_apply_library(db, pico8_report) != 0) {
            jw_log_warn("PICO-8 import metadata could not be applied; next scan will retry");
        }
        if (titles.group_count > 0) {
            jw_db_imported_title_group *groups =
                calloc((size_t)titles.group_count, sizeof(*groups));
            if (!groups) {
                snprintf(title_error, sizeof(title_error), "%s",
                         "could not allocate imported-title groups");
            } else {
                for (int i = 0; i < titles.group_count; i++) {
                    groups[i].provider = titles.groups[i].provider;
                    groups[i].title = titles.groups[i].title;
                    groups[i].rom_paths = (const char *const *)titles.groups[i].rom_paths;
                    groups[i].rom_path_count = titles.groups[i].rom_path_count;
                }
                jw_db_imported_title_result applied = {0};
                if (jw_db_apply_imported_title_groups(db, groups,
                                                      titles.group_count,
                                                      &applied) != 0) {
                    snprintf(title_error, sizeof(title_error), "%s",
                             "could not apply imported titles");
                } else {
                    title_result = applied;
                }
                free(groups);
            }
        }
        jw_db_close(db);
        ok = 1;
        }
    }
    jw__scan_title_list_free(&titles);

    pthread_mutex_lock(&job->mu);
    job->result = result;
    job->title_result = title_result;
    snprintf(job->title_error, sizeof(job->title_error), "%s", title_error);
    job->ok = ok != 0;
    snprintf(job->error, sizeof(job->error), "%s",
             error[0] ? error : (ok ? "" : "scan failed"));
    job->finished_ms = jw__monotonic_ms();
    job->running = false;
    job->completed = true;
    pthread_mutex_unlock(&job->mu);
    return NULL;
}

static int jw__start_scan_job_with_titles(jw_daemon_state *state,
                                          const char *reason,
                                          const jw_scan_title_list *titles) {
    if (!state || !state->scan_job.initialized) {
        return -1;
    }
    if (jw_delete_job_busy(state->delete_job)) {
        state->delete_pending_scan = true;
        pthread_mutex_lock(&state->scan_job.mu);
        int rc = jw__scan_title_list_merge(&state->scan_job.pending_titles, titles);
        pthread_mutex_unlock(&state->scan_job.mu);
        return rc == 0 ? 1 : -1;
    }
    /* Discovery writes the library DB. On a read-only card it would only
       produce write failures; defer until storage health sees it writable. */
    if (state->db_path && !jw_storage_path_writable(state->db_path, NULL, 0)) {
        if (!state->storage_scan_deferred) {
            jw_log_warn("scan-library deferred reason=%s: library database is not writable",
                        reason && reason[0] ? reason : "requested");
        }
        state->storage_scan_deferred = true;
        return 1;
    }

    jw_scan_job *job = &state->scan_job;
    pthread_mutex_lock(&job->mu);
    if (job->running || job->completed) {
        if (jw__scan_title_list_merge(&job->pending_titles, titles) != 0) {
            pthread_mutex_unlock(&job->mu);
            return -1;
        }
        job->pending_rescan = true;
        snprintf(job->pending_reason, sizeof(job->pending_reason), "%s",
                 reason && reason[0] ? reason : "pending");
        pthread_mutex_unlock(&job->mu);
        jw_log_info("scan-library queued reason=%s",
                    reason && reason[0] ? reason : "pending");
        return 1;
    }

    if (jw__scan_title_list_merge(&job->titles, titles) != 0) {
        pthread_mutex_unlock(&job->mu);
        return -1;
    }

    memset(&job->result, 0, sizeof(job->result));
    memset(&job->title_result, 0, sizeof(job->title_result));
    job->ok = false;
    job->completed = false;
    job->running = true;
    job->thread_started = true;
    job->started_ms = jw__monotonic_ms();
    job->finished_ms = 0;
    job->error[0] = '\0';
    job->title_error[0] = '\0';
    snprintf(job->reason, sizeof(job->reason), "%s",
             reason && reason[0] ? reason : "requested");
    snprintf(job->db_path, sizeof(job->db_path), "%s", state->db_path);
    snprintf(job->sdcard_root, sizeof(job->sdcard_root), "%s", state->sdcard_root);

    int rc = pthread_create(&job->thread, NULL, jw__scan_job_main, job);
    if (rc != 0) {
        job->running = false;
        job->thread_started = false;
        jw__scan_title_list_free(&job->titles);
        snprintf(job->error, sizeof(job->error), "%s", "could not start scan worker");
        pthread_mutex_unlock(&job->mu);
        return -1;
    }
    char started_reason[96];
    snprintf(started_reason, sizeof(started_reason), "%s", job->reason);
    pthread_mutex_unlock(&job->mu);

    jw_log_info("scan-library started reason=%s", started_reason);
    return 0;
}

static int jw__start_scan_job(jw_daemon_state *state, const char *reason) {
    return jw__start_scan_job_with_titles(state, reason, NULL);
}

static void jw__tick_scan_job(jw_daemon_state *state) {
    if (!state || !state->scan_job.initialized) {
        return;
    }

    jw_scan_job *job = &state->scan_job;
    pthread_t thread;
    bool join = false;
    bool ok = false;
    bool pending = false;
    jw_scan_result result;
    jw_db_imported_title_result title_result = {0};
    jw_scan_title_list pending_titles = {0};
    char reason[96];
    char pending_reason[96];
    char error[160];
    char title_error[160];
    long long elapsed_ms = 0;

    pthread_mutex_lock(&job->mu);
    if (job->completed && job->thread_started) {
        thread = job->thread;
        join = true;
        ok = job->ok;
        result = job->result;
        title_result = job->title_result;
        snprintf(reason, sizeof(reason), "%s", job->reason);
        snprintf(error, sizeof(error), "%s", job->error);
        snprintf(title_error, sizeof(title_error), "%s", job->title_error);
        pending = job->pending_rescan;
        jw__scan_title_list_move(&pending_titles, &job->pending_titles);
        snprintf(pending_reason, sizeof(pending_reason), "%s",
                 job->pending_reason[0] ? job->pending_reason : "pending");
        elapsed_ms = job->finished_ms > job->started_ms
                         ? job->finished_ms - job->started_ms
                         : 0;

        job->thread_started = false;
        job->completed = false;
        job->pending_rescan = false;
        job->pending_reason[0] = '\0';
    }
    pthread_mutex_unlock(&job->mu);

    if (!join) {
        return;
    }

    pthread_join(thread, NULL);

    if (ok) {
        jw__bump_library_generation(state);
        if (jw_db_relocation_note_scan(state->db, state->library_generation) != 0) {
            jw_log_warn("could not publish relocation reconciliation scan");
        }
        state->library_scanned_since_boot = true;
        state->library_populated = result.game_count > 0 || result.app_count > 0;
        jw_log_info("scan-library %s", reason[0] ? reason : "completed");
        jw_log_info("scan-library indexed %d games across %d systems and %d apps generation=%d",
                    result.game_count, result.system_count, result.app_count,
                    state->library_generation);
        jw_log_info("scan-library timings reason=%s total_ms=%lld",
                    reason[0] ? reason : "completed", elapsed_ms);
        if (title_result.paths > 0) {
            if (title_error[0]) {
                jw_log_warn("scan-library imported-titles accepted=%d applied=%d unmatched=%d error=%s",
                            title_result.paths, title_result.applied,
                            title_result.unmatched, title_error);
            } else {
                jw_log_info("scan-library imported-titles accepted=%d applied=%d unmatched=%d",
                            title_result.paths, title_result.applied,
                            title_result.unmatched);
            }
        }
    } else {
        jw_log_warn("scan-library failed reason=%s error=%s",
                    reason[0] ? reason : "unknown",
                    error[0] ? error : "unknown");
        if (title_result.paths > 0) {
            jw_log_warn("scan-library imported-titles accepted=%d applied=0 scan-failed",
                        title_result.paths);
        }
    }

    if (pending && !state->shutdown_requested) {
        if (jw__start_scan_job_with_titles(state, pending_reason,
                                           &pending_titles) < 0) {
            jw_log_warn("scan-library queued request could not start");
        }
    }
    jw__scan_title_list_free(&pending_titles);
}

static void jw__scan_job_shutdown(jw_daemon_state *state) {
    if (!state || !state->scan_job.initialized) {
        return;
    }

    jw_scan_job *job = &state->scan_job;
    pthread_t thread;
    bool join = false;
    pthread_mutex_lock(&job->mu);
    if (job->thread_started) {
        thread = job->thread;
        join = true;
        job->thread_started = false;
    }
    pthread_mutex_unlock(&job->mu);

    if (join) {
        jw_log_info("waiting for scan worker to finish");
        pthread_join(thread, NULL);
    }

    pthread_mutex_lock(&job->mu);
    jw__scan_title_list_free(&job->titles);
    jw__scan_title_list_free(&job->pending_titles);
    pthread_mutex_unlock(&job->mu);
    pthread_mutex_destroy(&job->mu);
    job->initialized = false;
}

static int jw__reply_library_status(jw_daemon_state *state, jw_ipc_client *client) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "library-status");
    /* The launcher's status worker asks for scrape-status only while this is
       not "idle", so an idle status bar costs one request instead of two. */
    jw_scrape_status_info scrape;
    jw_scrape_status(&scrape);
    cJSON_AddStringToObject(root, "scrape_state", jw__scrape_state_name(scrape.state));
    cJSON_AddNumberToObject(root, "generation", state ? state->library_generation : 0);
    cJSON_AddNumberToObject(root, "storage_health_generation",
                            state ? (double)state->storage_monitor.generation : 0);
    if (state && state->scan_job.initialized) {
        bool running = false;
        bool pending = false;
        char reason[96] = "";
        char error[160] = "";
        pthread_mutex_lock(&state->scan_job.mu);
        /* completed still awaits the main-loop join/generation publish and is
           therefore not idle from a relocation client's perspective. */
        running = state->scan_job.running || state->scan_job.completed;
        pending = state->scan_job.pending_rescan;
        snprintf(reason, sizeof(reason), "%s", state->scan_job.reason);
        snprintf(error, sizeof(error), "%s", state->scan_job.error);
        pthread_mutex_unlock(&state->scan_job.mu);
        cJSON_AddBoolToObject(root, "scan_running", running);
        cJSON_AddBoolToObject(root, "pending_rescan", pending);
        cJSON_AddStringToObject(root, "scan_reason", reason);
        cJSON_AddStringToObject(root, "scan_error", error);
        cJSON_AddBoolToObject(root, "library_populated", state->library_populated);
    }
    return jw__reply_json(client, root);
}

static void jw__storage_status_add_health(jw_daemon_state *state, cJSON *root,
                                          const char *source_id);

static int jw__reply_storage_status(jw_daemon_state *state, jw_ipc_client *client,
                                    const char *source_id) {
    jw_platform_storage_status status;
    jw_platform_get_storage_status(&state->platform, source_id, &status);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "storage-status");
    cJSON_AddStringToObject(root, "source", status.source_id);
    cJSON_AddStringToObject(root, "label", status.label);
    cJSON_AddStringToObject(root, "mount_path", status.mount_path);
    cJSON_AddStringToObject(root, "device_path", status.device_path);
    cJSON_AddBoolToObject(root, "present", status.present);
    cJSON_AddBoolToObject(root, "mounted", status.mounted);
    cJSON_AddBoolToObject(root, "busy", status.busy);
    cJSON_AddBoolToObject(root, "can_unmount", status.can_unmount);
    cJSON_AddStringToObject(root, "message", status.message);
    jw__storage_status_add_health(state, root, status.source_id);
    return jw__reply_json(client, root);
}

static int jw__handle_storage_action(jw_daemon_state *state, jw_ipc_client *client,
                                     cJSON *root) {
    if (jw_delete_job_busy(state->delete_job))
        return jw__reply_error(client, "Wait for ROM deletion before unmounting storage");
    cJSON *source_json = cJSON_GetObjectItemCaseSensitive(root, "source");
    cJSON *action_json = cJSON_GetObjectItemCaseSensitive(root, "action");
    const char *source = cJSON_IsString(source_json) && source_json->valuestring
        ? source_json->valuestring
        : "secondary_sd";
    if (!cJSON_IsString(action_json) || !action_json->valuestring ||
        strcmp(action_json->valuestring, "safe-unmount") != 0) {
        return jw__reply_error(client, "missing storage action");
    }

    jw_platform_result result;
    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    int stopped = 0;
    /* This request owns the latch outcome. A refusal or platform failure must
       not consume a later real removal event. */
    state->services_storage_stop_done = false;
    /* Check BEFORE stopping anything. SVC-1 makes safe unmount the one caller
       that must fail rather than proceed, so a refusal has to leave the system
       as it found it -- stopping every other storage-sensitive service and
       only then discovering the blocker would bounce healthy services for an
       operation that never happened. */
    if (state->services &&
        jw_svc_supervisor_storage_change_blocked(state->services, stuck,
                                                 sizeof(stuck))) {
        char message[256];
        snprintf(message, sizeof(message),
                 "Cannot unmount: service %s could not be verified stopped",
                 stuck);
        memset(&result, 0, sizeof(result));
        result.code = JW_PLATFORM_RESULT_FAILED;
        snprintf(result.message, sizeof(result.message), "%s", message);
        jw_log_warn("safe-unmount: refused; service %s is still live", stuck);
        return jw__reply_platform_result(client, action_json->valuestring,
                                         &result);
    }
    if (state->services) {
        stopped = jw_svc_supervisor_storage_change_begin(
            state->services, stuck, sizeof(stuck));
        if (stopped > 0) {
            jw_log_info("safe-unmount: stopped %d storage-sensitive service(s)",
                        stopped);
        }
    }
    if (stuck[0]) {
        /* A group that survived its full stop sequence can only be discovered
           by running it. Unavoidable side effect, and still a refusal. */
        char message[256];
        snprintf(message, sizeof(message),
                 "Cannot unmount: service %s could not be verified stopped",
                 stuck);
        memset(&result, 0, sizeof(result));
        result.code = JW_PLATFORM_RESULT_FAILED;
        snprintf(result.message, sizeof(result.message), "%s", message);
        jw_log_warn("safe-unmount: refused; service %s is still live", stuck);
        return jw__reply_platform_result(client, action_json->valuestring,
                                         &result);
    }
    jw_platform_safe_unmount_storage(&state->platform, source, &result);
    state->services_storage_stop_done =
        jw_svc_storage_should_suppress_followup_tick(
            stopped, result.code == JW_PLATFORM_RESULT_OK);
    if (state->services && result.code != JW_PLATFORM_RESULT_OK) {
        /* The topology did not change, so undo the restart hold. A successful
         * unmount stays held until a later mounted + rescanned storage tick. */
        jw_svc_supervisor_storage_change_resume(state->services);
    }
    if (result.code == JW_PLATFORM_RESULT_OK) {
        if (state->services && jw_svc_supervisor_scan(state->services) < 0) {
            jw_log_warn("safe-unmount: service rescan failed");
        }
        int scan_rc = jw__start_scan_job(state, "after safe-unmount");
        if (scan_rc < 0) {
            jw_log_warn("safe-unmount: library rescan could not start");
        } else {
            snprintf(result.message, sizeof(result.message), "%s",
                     scan_rc > 0 ? "Secondary SD unmounted; library rescan queued"
                                 : "Secondary SD unmounted; library update started");
        }
    }

    jw_log_info("storage-action requested source=%s action=%s code=%s",
                source, action_json->valuestring,
                jw_platform_result_code_name(result.code));
    return jw__reply_platform_result(client, action_json->valuestring, &result);
}

static const char *jw__child_name(jw_child_kind kind) {
    switch (kind) {
        case JW_CHILD_LAUNCHER: return "jawaka-launcher";
        case JW_CHILD_MENU: return "jawaka-menu";
        case JW_CHILD_RETROARCH: return "RetroArch";
        case JW_CHILD_EMULATOR: return "standalone emulator";
        case JW_CHILD_APP: return "app";
        /* Deliberately the same binary name as JW_CHILD_APP: jw__spawn_child()
           rejects both kinds, and the launcher path that maps a name to a
           binary never sees either. */
        case JW_CHILD_RETROARCH_APP: return "app";
        case JW_CHILD_PICO8_APP: return "PICO-8 app";
        case JW_CHILD_PICO8_IMPORT: return "PICO-8 import";
        default: return NULL;
    }
}

/* Everything an app tile is: input-proxy handover, package-mutation blocking,
   and returning to the launcher on exit. Both app kinds share all of it. */
static bool jw__child_kind_is_app(jw_child_kind kind) {
    return kind == JW_CHILD_APP || kind == JW_CHILD_RETROARCH_APP ||
           kind == JW_CHILD_PICO8_APP || kind == JW_CHILD_PICO8_IMPORT;
}

static bool jw__child_kind_has_writer_barrier(jw_child_kind kind) {
    return kind == JW_CHILD_RETROARCH || kind == JW_CHILD_EMULATOR;
}

/* Kinds whose whole process group must be proved absent before the leader is
   reaped. For a writer this is the LIFE-1 barrier; for the RetroArch app it
   stops a crashed runner from leaving RetroArch orphaned and still holding the
   card. It is NOT a writer barrier: no active-game record is involved. */
static bool jw__child_kind_has_group_barrier(jw_child_kind kind) {
    return jw__child_kind_has_writer_barrier(kind) ||
           kind == JW_CHILD_RETROARCH_APP || kind == JW_CHILD_PICO8_APP || kind == JW_CHILD_PICO8_IMPORT;
}

/* Only the RetroArch app runner gets a stop grace: it has to talk RetroArch
   into saving before it can copy the config back. */
static bool jw__child_kind_has_stop_grace(jw_child_kind kind) {
    return kind == JW_CHILD_RETROARCH_APP;
}

/* Must stay ABOVE the runner's own worst case -- JW_RUNNER_STOP_CEILING_MS in
   cmd/jawaka-retroarch-runner/main.c, currently 7000 ms (1000 promote + 4000
   QUIT + 1000 TERM + 1000 KILL). Setting this equal to that ceiling, as an
   earlier revision did, means a wedged RetroArch gets its group killed at the
   exact moment the runner makes its final save attempt, which defeats the
   grace. 9000 ms leaves 2 s of headroom.
   This only costs a slow shutdown when RetroArch is actually wedged: measured
   on an MLP1 (2026-08-26), a real reboot with the app tile open used about
   2 s of this budget end to end. */
#define JW_RA_APP_STOP_GRACE_MS 9000
/* Bounded fallback for the group barrier above: past this the group is killed.
   Normal operation keeps the leader unreaped until absence is proven; shutdown
   may reap so a wedged group cannot hang the platform transition. */
#define JW_RA_APP_GROUP_WAIT_MS 5000
/* How often to repeat the survived-SIGKILL error while a group refuses to
   clear, so a wedged teardown is visible without flooding the log. */
#define JW_RA_APP_GROUP_KILL_LOG_MS 5000
/* The runner's distinct "app ran, settings could not be saved" status. */
#define JW_RA_APP_EXIT_SAVE_FAILED 90

/* Both sides call setpgid to close the fork/exec race. The child call happens
 * before it can execute a writer; the parent confirms the exact group before
 * publishing the launch as active. */
static bool jw__reserve_game_process_group(pid_t pid) {
    if (pid <= 0) {
        return false;
    }
    if (setpgid(pid, pid) == 0) {
        return true;
    }
    if (errno == EACCES || errno == EPERM || errno == ESRCH) {
        pid_t pgid = getpgid(pid);
        if (pgid == pid) {
            return true;
        }
    }
    return false;
}

static int jw__game_child_set_own_group(void) {
    /* The parent calls setpgid(pid, pid) concurrently. Darwin can refuse
       this call with EPERM while the parent's is in flight, so accept the
       group the parent made (the parent's own check, mirrored) and retry
       briefly before failing closed. */
    for (int attempt = 0; attempt < 20; attempt++) {
        if (setpgid(0, 0) == 0 || getpgrp() == getpid()) {
            return 0;
        }
        jw__usleep(1000);
    }
    return -1;
}

static int jw__signal_tracked_game_group(jw_daemon_state *state, int signal) {
    if (!state || state->child_pid <= 0) {
        errno = ESRCH;
        return -1;
    }
    pid_t target = state->child_pgid > 0 ? -state->child_pgid
                                         : state->child_pid;
    return kill(target, signal);
}

static int jw__spawn_child(jw_daemon_state *state, jw_child_kind kind);
static int jw__spawn_in_game_menu(jw_daemon_state *state, bool show_now);
static int jw__spawn_osd(jw_daemon_state *state);
static int jw__spawn_retroarch(jw_daemon_state *state,
                               const jw_launch_target *target);
static int jw__spawn_standalone_emulator(jw_daemon_state *state,
                                         const jw_launch_target *target);
static int jw__spawn_pending_game(jw_daemon_state *state);
static void jw__set_session_menu(jw_daemon_state *state, bool provider_bound);
static int jw__spawn_authorized_pending_game(jw_daemon_state *state);
static int jw__usable_subscription(jw_daemon_state *state,
                                   const char *service_id);
static int jw__spawn_app(jw_daemon_state *state);
static int jw__request_open_in_game_menu(jw_daemon_state *state);
static int jw__request_open_in_game_switcher(jw_daemon_state *state);
static int jw__request_close_in_game_menu(jw_daemon_state *state);
static void jw__handle_child_exit(jw_daemon_state *state);
static void jw__game_coordination_tick(jw_daemon_state *state);
static void jw__game_coordination_connection_failed(jw_daemon_state *state,
                                                     int index,
                                                     const char *reason);
static bool jw__game_coordination_start_now(jw_daemon_state *state,
                                            const char *reason);
static bool jw__game_check_wait(jw_daemon_state *state);
static bool jw__game_check_play_anyway(jw_daemon_state *state);
static bool jw__game_check_cancel(jw_daemon_state *state);
static void jw__active_game_finish(jw_daemon_state *state);
static bool jw__game_launch_blocked_dismiss(jw_daemon_state *state);
/* Resume Game on Boot; defined after the post-launch resume tick. */
static bool jw__boot_resume_unconfirmed(const jw_daemon_state *state);
static void jw__boot_resume_fail(jw_daemon_state *state, const char *why, bool quit);
static void jw__boot_resume_arm(jw_daemon_state *state);
static bool jw__boot_resume_spawn_matches(jw_daemon_state *state,
                                          const jw_storage_source *source,
                                          const jw_launch_target *target,
                                          const char *rom_abs);
static void jw__retroarch_session_pin_resume(jw_daemon_state *state,
                                             const jw_storage_source *source,
                                             const char *rom_relpath,
                                             const char *provider);

static void jw__schedule_in_game_menu_prewarm(jw_daemon_state *state,
                                              long long delay_ms) {
    if (!state || state->menu_pid > 0) {
        return;
    }
    state->in_game_menu_prewarm_pending = true;
    state->in_game_menu_prewarm_next_ms = jw__monotonic_ms() + delay_ms;
    jw_log_info("scheduled in-game menu prewarm delay_ms=%lld", delay_ms);
}

static void jw__cancel_in_game_menu_prewarm(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    state->in_game_menu_prewarm_pending = false;
    state->in_game_menu_prewarm_next_ms = 0;
}

static void jw__tick_in_game_menu_prewarm(jw_daemon_state *state) {
    if (!state || !state->in_game_menu_prewarm_pending) {
        return;
    }
    if (!state->retroarch_session.active || state->child_kind != JW_CHILD_RETROARCH) {
        jw__cancel_in_game_menu_prewarm(state);
        return;
    }
    if (state->menu_pid > 0) {
        jw__cancel_in_game_menu_prewarm(state);
        return;
    }

    long long now = jw__monotonic_ms();
    if (state->in_game_menu_prewarm_next_ms > now) {
        return;
    }

    jw__cancel_in_game_menu_prewarm(state);
    if (jw__spawn_in_game_menu(state, false) != 0) {
        jw_log_warn("could not pre-spawn standby in-game menu; will spawn on demand");
    }
}

static void jw__publish_retroarch_input_env(jw_daemon_state *state) {
    if (!state || !state->input_proxy.enabled ||
        !state->input_proxy.virtual_event_path[0]) {
        unsetenv("CAT_INPUT_WAKE_EVENT");
        unsetenv("JAWAKA_INPUT_VIRTUAL_EVENT");
        unsetenv("JAWAKA_RETROARCH_VIRTUAL_EVENT");
        unsetenv("JAWAKA_RETROARCH_INPUT_DEVICE");
        unsetenv("JAWAKA_RETROARCH_JOYPAD_INDEX");
        return;
    }

    setenv("CAT_INPUT_WAKE_EVENT", state->input_proxy.virtual_event_path, 1);
    setenv("JAWAKA_INPUT_VIRTUAL_EVENT",
           state->input_proxy.virtual_event_path, 1);
    setenv("JAWAKA_RETROARCH_VIRTUAL_EVENT",
           state->input_proxy.virtual_event_path, 1);
    if (state->input_proxy.device_name[0]) {
        setenv("JAWAKA_RETROARCH_INPUT_DEVICE",
               state->input_proxy.device_name, 1);
    } else {
        unsetenv("JAWAKA_RETROARCH_INPUT_DEVICE");
    }

    int joypad_index = jw_input_proxy_retroarch_joypad_index(&state->input_proxy);
    if (joypad_index >= 0) {
        char joypad_text[16];
        snprintf(joypad_text, sizeof(joypad_text), "%d", joypad_index);
        setenv("JAWAKA_RETROARCH_JOYPAD_INDEX", joypad_text, 1);
        jw_log_info("RetroArch input proxy env: virtual=%s joypad_index=%d",
                    state->input_proxy.virtual_event_path, joypad_index);
    } else {
        unsetenv("JAWAKA_RETROARCH_JOYPAD_INDEX");
        jw_log_warn("RetroArch input proxy env: could not resolve joypad index for %s",
                    state->input_proxy.virtual_event_path);
    }
}

static void jw__publish_direct_input_env(void) {
    unsetenv("CAT_INPUT_WAKE_EVENT");
    unsetenv("JAWAKA_INPUT_VIRTUAL_EVENT");
    unsetenv("JAWAKA_RETROARCH_VIRTUAL_EVENT");
    unsetenv("JAWAKA_RETROARCH_INPUT_DEVICE");
    setenv("JAWAKA_RETROARCH_JOYPAD_INDEX", "0", 1);
}

static void jw__retroarch_session_clear(jw_retroarch_session *session) {
    if (!session) {
        return;
    }

    memset(session, 0, sizeof(*session));
}

static void jw__advanced_shader_clear(jw_daemon_state *state) {
    state->advanced_shader_pending = false;
    state->advanced_shader_menu_seen = false;
    state->advanced_shader_destination_sent = false;
    state->advanced_shader_started_ms = 0;
    state->advanced_shader_next_poll_ms = 0;
}

/* A hard cut during a power-hold save can leave "<stem>.state99.tmp-<id>" in
   the pinned States/<core> folder. Discovery ignores the name; sweep it when
   this game is launched again, built exactly like the eligibility pin. Only
   RetroArch game sessions, never at boot, never for other games. */
static void jw__power_hold_save_remove_orphans(const jw_retroarch_session *session) {
    if (!session->rom_path[0] || !session->source_root[0] ||
        !session->core_config_folder[0]) {
        return;
    }
    char states_dir[PATH_MAX];
    char final_path[PATH_MAX];
    if (snprintf(states_dir, sizeof(states_dir), "%s/States", session->source_root) >=
            (int)sizeof(states_dir) ||
        !jw_ra_slot_state_path_for_core(states_dir, session->core_config_folder,
                                        session->rom_path, JW_RA_GAME_SWITCHER_STATE_SLOT,
                                        false, final_path, sizeof(final_path))) {
        return;
    }
    int removed = jw_power_hold_save_remove_orphans(final_path);
    if (removed > 0) {
        jw_log_info("power-hold save: removed %d orphan temporary state%s for %s",
                    removed, removed == 1 ? "" : "s", final_path);
    }
}

static void jw__retroarch_session_start(jw_daemon_state *state, pid_t pid,
                                        int game_id,
                                        const char *system, const char *rom_path,
                                        const char *db_rom_path,
                                        const char *source_root,
                                        const char *core_path,
                                        const char *core_id,
                                        const char *core_config_folder,
                                        const char *config_path,
                                        bool persist_config,
                                        bool audio_bluetooth,
                                        const char *warning) {
    if (!state || pid <= 0) {
        return;
    }

    jw_retroarch_session *session = &state->retroarch_session;
    jw__retroarch_session_clear(session);
    session->active = true;
    session->pid = pid;
    session->game_id = game_id;
    session->started_at = time(NULL);
    snprintf(session->system, sizeof(session->system), "%s", system ? system : "");
    snprintf(session->rom_path, sizeof(session->rom_path), "%s", rom_path ? rom_path : "");
    snprintf(session->db_rom_path, sizeof(session->db_rom_path), "%s",
             db_rom_path ? db_rom_path : "");
    snprintf(session->source_root, sizeof(session->source_root), "%s",
             source_root ? source_root : "");
    snprintf(session->core_path, sizeof(session->core_path), "%s", core_path ? core_path : "");
    snprintf(session->core_id, sizeof(session->core_id), "%s", core_id ? core_id : "");
    snprintf(session->core_config_folder, sizeof(session->core_config_folder), "%s",
             core_config_folder ? core_config_folder : "");
    snprintf(session->config_path, sizeof(session->config_path), "%s",
             config_path ? config_path : "");
    session->persist_config = persist_config;
    session->audio_bluetooth = audio_bluetooth;
    snprintf(session->warning, sizeof(session->warning), "%s",
             warning ? warning : "");
    session->warning_pending = session->warning[0] != '\0';

    /* Fresh session: no menu shown yet, reset the standby respawn guard. */
    state->menu_visible = false;
    state->menu_standby_attempts = 0;
    jw__advanced_shader_clear(state);

    jw_log_info("RetroArch session started pid=%d system=%s source=%s core=%s core_id=%s core_folder=%s config=%s rom=%s",
                (int)pid, session->system, session->source_root,
                session->core_path, session->core_id[0] ? session->core_id : "(unknown)",
                session->core_config_folder[0] ? session->core_config_folder : "(unavailable)",
                session->config_path, session->rom_path);
    jw__power_hold_save_remove_orphans(session);
}

static long jw__retroarch_session_runtime_s(const jw_retroarch_session *session) {
    if (!session || session->started_at <= 0) {
        return 0;
    }

    time_t ended_at = time(NULL);
    if (ended_at == (time_t)-1 || ended_at < session->started_at) {
        return 0;
    }
    return (long)(ended_at - session->started_at);
}

static void jw__retroarch_session_record_play(jw_daemon_state *state,
                                              const jw_retroarch_session *session,
                                              long runtime_s) {
    if (!state || !session || runtime_s <= 0 ||
        !state->db_path || session->game_id <= 0) {
        return;
    }

    if (jw_db_record_play_by_id(state->db_path, session->game_id,
                                (int)runtime_s) == 0) {
        jw_log_info("recorded play game_id=%d duration_s=%ld",
                    session->game_id, runtime_s);
    } else {
        jw_log_warn("could not record play for game_id=%d", session->game_id);
    }
}

/* On a standalone (mupen64plus) Menu+Select quit, the emulator drops
   "<runtime_dir>/standalone-switcher-request" (system + absolute ROM, one per
   line) before it exits. Consume it here, *before* the session is cleared:
   always unlink so a leftover can't leak to a later/foreign session, and honor
   it only when it belongs to the still-active mupen64plus session that just
   exited (core, ROM, and a mtime newer than the session start). On honor, arm
   the launcher to reopen straight into the switcher carousel seeded on this
   game, and make sure the game is at the front of Recents even for an instant
   chord (record_play skips runtime_s <= 0, so touch with duration 0 to bump
   last_opened without inflating playtime). */
static void jw__consume_standalone_switcher_marker(jw_daemon_state *state, pid_t exited_pid) {
    if (!state || !state->runtime_dir) {
        return;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/standalone-switcher-request",
                 state->runtime_dir) >= (int)sizeof(path)) {
        return;
    }

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return;
    }

    char marker_system[64] = {0};
    char marker_rom[PATH_MAX] = {0};
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(marker_system, sizeof(marker_system), f)) {
            marker_system[strcspn(marker_system, "\r\n")] = '\0';
        }
        if (fgets(marker_rom, sizeof(marker_rom), f)) {
            marker_rom[strcspn(marker_rom, "\r\n")] = '\0';
        }
        fclose(f);
    }
    /* Consume unconditionally so it can never affect a later or foreign exit. */
    unlink(path);

    /* NOTE: jw__handle_child_exit() has already reset child_kind to
       JW_CHILD_NONE by the time we run, so jw__standalone_session_is_mupen64plus()
       (which gates on child_kind == JW_CHILD_EMULATOR) would wrongly report
       false. Check the still-intact session's core directly instead. */
    const jw_retroarch_session *session = &state->retroarch_session;
    bool session_is_mupen64plus = session->active &&
        session->standalone_policy.release == JW_STANDALONE_RELEASE_MUPEN64PLUS;
    if (!session_is_mupen64plus || session->pid != exited_pid) {
        return;
    }

    bool rom_matches = marker_rom[0] &&
        (strcmp(marker_rom, session->rom_path) == 0 ||
         strcmp(marker_rom, session->db_rom_path) == 0);
    if (!rom_matches || st.st_mtime < session->started_at) {
        jw_log_warn("switcher marker rejected rom='%s' session_rom='%s' mtime=%ld started_at=%ld",
                    marker_rom, session->rom_path, (long)st.st_mtime,
                    (long)session->started_at);
        return;
    }

    if (jw__retroarch_session_runtime_s(session) <= 0 && state->db_path) {
        if (session->game_id > 0) {
            jw_db_record_play_by_id(state->db_path, session->game_id, 0);
        }
    }

    state->launcher_open_switcher = true;
    snprintf(state->launcher_switcher_system, sizeof(state->launcher_switcher_system),
             "%s", marker_system[0] ? marker_system : session->system);
    snprintf(state->launcher_switcher_rom, sizeof(state->launcher_switcher_rom),
             "%s", session->rom_path);
    jw_log_info("switcher marker honored: reopen launcher in switcher system=%s rom=%s",
                state->launcher_switcher_system, state->launcher_switcher_rom);
}

static void jw__retroarch_session_retarget(jw_daemon_state *state,
                                           int game_id,
                                           const char *system,
                                           const char *rom_path,
                                           const char *db_rom_path,
                                           const char *source_root,
                                           const char *core_path,
                                           const char *core_id,
                                           const char *core_config_folder,
                                           const char *warning) {
    if (!state || !state->retroarch_session.active) {
        return;
    }

    jw_retroarch_session *session = &state->retroarch_session;
    long runtime_s = jw__retroarch_session_runtime_s(session);
    jw__retroarch_session_record_play(state, session, runtime_s);
    int resident_switches = session->resident_switches + 1;

    session->started_at = time(NULL);
    session->game_id = game_id;
    snprintf(session->system, sizeof(session->system), "%s", system ? system : "");
    snprintf(session->rom_path, sizeof(session->rom_path), "%s", rom_path ? rom_path : "");
    snprintf(session->db_rom_path, sizeof(session->db_rom_path), "%s",
             db_rom_path ? db_rom_path : "");
    snprintf(session->source_root, sizeof(session->source_root), "%s",
             source_root ? source_root : "");
    snprintf(session->core_path, sizeof(session->core_path), "%s", core_path ? core_path : "");
    snprintf(session->core_id, sizeof(session->core_id), "%s", core_id ? core_id : "");
    snprintf(session->core_config_folder, sizeof(session->core_config_folder), "%s",
             core_config_folder ? core_config_folder : "");
    snprintf(session->warning, sizeof(session->warning), "%s",
             warning ? warning : "");
    session->warning_pending = session->warning[0] != '\0';
    session->warning_attempts = 0;
    session->warning_next_ms = 0;
    session->resident_switches = resident_switches;

    state->post_launch_resume_pending = false;
    state->post_launch_resume_attempts = 0;
    state->post_launch_resume_next_ms = 0;
    state->retroarch_resume_on_menu_exit = false;
    state->menu_visible = false;
    state->menu_standby_attempts = 0;
    jw__advanced_shader_clear(state);

    jw_log_info("RetroArch session retargeted in-process pid=%d runtime_s=%ld resident_switches=%d system=%s source=%s core=%s core_id=%s core_folder=%s rom=%s",
                (int)session->pid, runtime_s, session->resident_switches,
                session->system, session->source_root,
                session->core_path, session->core_id[0] ? session->core_id : "(unknown)",
                session->core_config_folder[0] ? session->core_config_folder : "(unavailable)",
                session->rom_path);
    jw__power_hold_save_remove_orphans(session);
}

static int jw__platform_path(char *out, size_t out_size, const jw_daemon_state *state);

/* Hand this session's captures to the post-process pass.

   Recording captures FLAC in Matroska because that is what stays cheap enough to
   run underneath a game -- real-time AAC breaks audio output on FCEUmm, and
   Matroska survives a capture cut short where MP4 would leave nothing playable.
   Neither travels, so the shareable MP4 is made here instead, once the game is
   gone and nothing is competing for the CPU.

   Detached on purpose, and never waited on. The launcher has to come back
   immediately; conversion costs roughly 1.5s per 15s of capture on this hardware,
   almost entirely in the AAC encoder.

   Double-fork is deliberate: jawakad only ever reaps the specific pids it tracks
   (child_pid, osd_pid, ledd), never waitpid(-1), so a child spawned here would sit
   as a zombie for the life of the daemon. The grandchild is orphaned to init,
   which reaps it. The intermediate child exits at once and is waited for here.

   Silent when the feature is not installed: a device without the ffmpeg payload
   should behave exactly as it did before, not log a warning after every game. */
static void jw__record_convert_spawn(const jw_daemon_state *state) {
    if (!state || jw__env_is_disabled("JAWAKA_RECORD_CONVERT")) {
        return;
    }

    /* Never during shutdown. jw__cleanup() SIGTERMs a running RetroArch and calls
       jw__retroarch_session_finish(), which lands here -- so this used to orphan an
       ffmpeg to init that would spend seconds to minutes writing multi-megabyte
       files to the SD card while the system was rebooting out from under it. A
       dirty FAT32 at reboot can flip the whole card read-only. Nothing is lost by
       declining: the pass is idempotent and sweeps everything on the next exit. */
    if (state->shutdown_requested || g_shutdown_requested) {
        return;
    }

    if (!state->db_path) {
        return;
    }

    /* All three settings in ONE open. jw_db_get_setting() opens a fresh connection
       and re-applies the schema per call, with a 2s busy timeout, and this runs on
       the main loop -- the same loop that drains evdev. Three separate calls could
       park the daemon for seconds at game exit while the kernel's input buffer
       overflowed and dropped events, which is exactly what "the launcher has to come
       back immediately" was supposed to avoid.
       An absent key leaves out[] empty and found==0, so split/keep default ON by
       testing against "0" rather than for "1" -- neither key exists yet on any
       device that has not toggled them. */
    char en[8] = "", sp[8] = "", ks[8] = "";
    jw_db_setting_query q[3] = {
        { "recording_enabled",     en, sizeof(en), 0 },
        { "recording_split",       sp, sizeof(sp), 0 },
        { "recording_keep_source", ks, sizeof(ks), 0 },
    };
    (void)jw_db_get_settings(state->db_path, q, 3);

    /* Opt-in gate, matching the hotkey. Without it the daemon forked and exec'd a
       shell after EVERY game exit for every user, including those who never enabled
       recording -- the stat() guard below cannot catch that, because the config
       generator creates Recordings/ on every launch, so it always exists. */
    if (strcmp(en, "1") != 0) {
        return;
    }

    char platform_path[PATH_MAX];
    if (jw__platform_path(platform_path, sizeof(platform_path), state) != 0) {
        return;
    }

    char script[PATH_MAX];
    if (snprintf(script, sizeof(script), "%s/bin/leaf-record-convert.sh",
                 platform_path) >= (int)sizeof(script)) {
        return;
    }
    if (access(script, X_OK) != 0) {
        return; /* payload not installed on this device */
    }

    char recordings[PATH_MAX];
    if (!jw_primary_recordings_path(recordings, sizeof(recordings),
                                    state->sdcard_root)) {
        return;
    }

    struct stat st;
    if (stat(recordings, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return; /* nothing was ever recorded */
    }

    bool split = (strcmp(sp, "0") != 0);
    bool keep_source = (strcmp(ks, "0") != 0);

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_warn("record convert fork failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        pid_t inner = fork();
        if (inner < 0) {
            _exit(127);
        }
        if (inner > 0) {
            _exit(0); /* parent of the grandchild; jawakad reaps this one */
        }

        setsid();
        /* jawakad ignores SIGPIPE, and an IGNORED disposition survives execve
           where a handler would be reset. The conversion pass is a shell script
           built out of pipelines whose readers exit early (`du -k ... | awk
           '{print $1; exit}'`, `... | sed -n ... | tail -1`), and with SIGPIPE
           ignored such a writer is not killed -- it collects EPIPE on every write
           and keeps going. Hand the script the default it expects.

           This covers the conversion child only. Every other exec site in this
           daemon still passes SIG_IGN down, which is worth fixing separately --
           pak launch.sh scripts are third-party shell and the likeliest place for
           it to matter. */
        signal(SIGPIPE, SIG_DFL);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) {
                close(devnull);
            }
        }
        char *argv[5];
        int argc = 0;
        argv[argc++] = script;
        argv[argc++] = recordings;
        if (split) {
            argv[argc++] = (char *)"--split";
        }
        if (!keep_source) {
            argv[argc++] = (char *)"--delete-source";
        }
        argv[argc] = NULL;
        execv(script, argv);
        _exit(127);
    }

    /* The intermediate child _exit(0)s straight after its own fork, so this returns
       almost at once; the loop is only here because a handler installed without
       SA_RESTART would otherwise leave it a zombie, and jawakad never waitpid(-1)s.
       A non-zero status means that inner fork failed -- worth saying so, since the
       success line below would otherwise claim a conversion that never started. */
    int spawn_status = 0;
    while (waitpid(pid, &spawn_status, 0) < 0 && errno == EINTR) {
        /* retry */
    }
    if (WIFEXITED(spawn_status) && WEXITSTATUS(spawn_status) != 0) {
        jw_log_warn("record convert failed to start (status=%d)",
                    WEXITSTATUS(spawn_status));
        return;
    }
    jw_log_info("record convert dispatched for %s split=%s keep_source=%s",
                recordings, split ? "true" : "false",
                keep_source ? "true" : "false");
}

static void jw__retroarch_session_finish(jw_daemon_state *state, pid_t pid, int status) {
    if (!state) {
        return;
    }

    jw__cancel_in_game_menu_prewarm(state);

    jw_retroarch_session *session = &state->retroarch_session;
    if (!session->active) {
        jw_log_warn("RetroArch child exited without active session pid=%d", (int)pid);
        return;
    }

    long runtime_s = jw__retroarch_session_runtime_s(session);
    if (session->pid != pid) {
        jw_log_warn("RetroArch session pid mismatch tracked=%d exited=%d",
                    (int)session->pid, (int)pid);
    }

    if (WIFEXITED(status)) {
        jw_log_info("RetroArch session ended pid=%d runtime_s=%ld status=%d system=%s rom=%s",
                    (int)pid, runtime_s, WEXITSTATUS(status),
                    session->system, session->rom_path);
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("RetroArch session terminated pid=%d runtime_s=%ld signal=%d system=%s rom=%s",
                    (int)pid, runtime_s, WTERMSIG(status),
                    session->system, session->rom_path);
    } else {
        jw_log_warn("RetroArch session changed state pid=%d runtime_s=%ld status=%d system=%s rom=%s",
                    (int)pid, runtime_s, status, session->system, session->rom_path);
    }

    bool switcher_transition = state->pending_launch && state->pending_launch_resume_switcher;
    /* A boot resume whose state never loaded was not a session the player
       configured anything in; writing its config back would only replace
       their lines with RetroArch's defaults (Hardcore among them). */
    if (session->resume_unconfirmed && session->config_path[0]) {
        jw_log_info("boot resume: RetroArch config not kept from a session whose state never loaded");
    } else if (session->frontend && !session->frontend_version_ok && session->config_path[0]) {
        /* A different RetroArch version may write keys Leaf's does not know or
           read differently; only a matching frontend shares the config back. */
        jw_log_info("RetroArch shared config not kept from frontend session "
                    "(version does not match Leaf's RetroArch, or never probed)");
    } else if (session->config_path[0] && session->persist_config && !switcher_transition) {
        char error[256];
        if (jw_backup_retroarch_config(session->config_path, state->sdcard_root,
                                       &session->config_snapshot,
                                       error, sizeof(error)) != 0) {
            jw_log_warn("RetroArch shared config backup failed: %s",
                        error[0] ? error : session->config_path);
        } else {
            jw_log_info("RetroArch shared config backed up from %s", session->config_path);
        }
    } else if (session->config_path[0]) {
        jw_log_info("RetroArch shared config backup skipped persist=%s switcher_transition=%s",
                    session->persist_config ? "true" : "false",
                    switcher_transition ? "true" : "false");
    }

    /* Drop the per-session runtime config; it holds the plaintext cheevos
       password and is regenerated on the next launch. Ignore errors (the file
       may already be gone). */
    if (session->config_path[0]) {
        (void)unlink(session->config_path);
    }

    /* Record recents + playtime for real sessions only. A crash at launch gives
       runtime_s=0, so it never pollutes the list or playtime totals. The session
       retains games.id so mountpoint/cache changes cannot misattribute play.
       A boot resume whose state never loaded was not the player's game: it
       records nothing, and its launcher carries the notice. */
    if (jw__boot_resume_unconfirmed(state)) {
        jw__boot_resume_fail(state, "RetroArch exited before the state loaded", false);
    }
    if (session->resume_unconfirmed) {
        jw_log_info("boot resume: no play recorded for a session whose state never loaded");
    } else {
        jw__retroarch_session_record_play(state, session, runtime_s);
    }

    state->post_launch_resume_pending = false;
    state->post_launch_resume_attempts = 0;
    jw__advanced_shader_clear(state);
    jw__retroarch_session_clear(session);
    if (!state->pending_launch) {
        state->perf_session_override = false;
        state->perf_session_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
        state->perf_custom_valid = false;
        jw__perf_request_init(&state->perf_custom_request);
        (void)jw__perf_apply_frontend(state, "retroarch-exit");

        /* Only once we are actually going back to the launcher. A switcher
           transition is launching the next game right now, and the conversion
           would be competing with it for the CPU it needs to start cleanly.
           Nothing is lost by waiting -- the pass is idempotent and picks up
           every unconverted capture whenever it next runs. */
        jw__record_convert_spawn(state);
    }
}

static void jw__standalone_session_start(jw_daemon_state *state, pid_t pid,
                                         int game_id,
                                         const char *system, const char *rom_path,
                                         const char *db_rom_path,
                                         const char *source_root,
                                         const jw_launch_target *target) {
    if (!state || pid <= 0 || !target) {
        return;
    }

    state->launch_status_pending = false;
    state->launch_status_name[0] = '\0';
    state->launch_status_code = 0;

    jw_retroarch_session *session = &state->retroarch_session;
    jw__retroarch_session_clear(session);
    session->active = true;
    session->pid = pid;
    session->game_id = game_id;
    session->started_at = time(NULL);
    snprintf(session->system, sizeof(session->system), "%s", system ? system : "");
    snprintf(session->rom_path, sizeof(session->rom_path), "%s", rom_path ? rom_path : "");
    snprintf(session->db_rom_path, sizeof(session->db_rom_path), "%s",
             db_rom_path ? db_rom_path : "");
    snprintf(session->source_root, sizeof(session->source_root), "%s",
             source_root ? source_root : "");
    snprintf(session->core_path, sizeof(session->core_path), "%s", target->path);
    snprintf(session->core_id, sizeof(session->core_id), "%s", target->core_id);
    session->standalone_policy = target->standalone_policy;
    session->supports_menu = target->supports_menu;

    state->menu_visible = false;
    state->menu_standby_attempts = 0;
    state->standalone_quit_request_ms = 0;

    jw_log_info("standalone emulator session started pid=%d system=%s source=%s launcher=%s core_id=%s rom=%s",
                (int)pid, session->system, session->source_root,
                session->core_path, session->core_id[0] ? session->core_id : "(unknown)",
                session->rom_path);
}

/* The name the launcher status line uses for a standalone launcher: the core
   id with the _standalone suffix dropped and underscores spaced, or the
   emulator directory name when the id is empty. */
static void jw__standalone_display_name(const char *core_id,
                                        const char *launcher_path,
                                        char *out, size_t out_size) {
    const char *source = (core_id && core_id[0]) ? core_id : NULL;
    char derived[64];
    if (!source && launcher_path && launcher_path[0]) {
        /* ".../emulators/ports/launch.sh" -> "ports" */
        const char *emulators = strstr(launcher_path, "/emulators/");
        if (emulators) {
            const char *start = emulators + strlen("/emulators/");
            const char *end = strchr(start, '/');
            size_t len = end ? (size_t)(end - start) : strlen(start);
            if (len > 0 && len < sizeof(derived)) {
                snprintf(derived, sizeof(derived), "%.*s", (int)len, start);
                source = derived;
            }
        }
    }
    if (!source) {
        snprintf(out, out_size, "%s", "emulator");
        return;
    }
    snprintf(out, out_size, "%s", source);
    char *suffix = strstr(out, "_standalone");
    if (suffix) {
        *suffix = '\0';
    }
    for (char *p = out; *p; p++) {
        if (*p == '_') {
            *p = ' ';
        }
    }
}

/* A wrapper that dies before the emulator appears reports within seconds; a
   session long enough to be real play is the user's own quit and stays
   silent. Only an actual exit status is surfaced: a signal already has the
   supervisor's crash handling, and a shell wrapper reports a signal that ended
   its game as 128+N (a quick quit from the in-game menu is 143), which is not
   a failed launch. */
#define JW_STANDALONE_LAUNCH_FAILURE_MAX_RUNTIME_S 15

static void jw__standalone_session_finish(jw_daemon_state *state, pid_t pid, int status) {
    if (!state) {
        return;
    }
    state->standalone_quit_request_ms = 0;
    jw__set_session_menu(state, false);

    jw_retroarch_session *session = &state->retroarch_session;
    if (!session->active) {
        jw_log_warn("standalone emulator child exited without active session pid=%d", (int)pid);
        return;
    }

    long runtime_s = jw__retroarch_session_runtime_s(session);
    if (session->pid != pid) {
        jw_log_warn("standalone emulator session pid mismatch tracked=%d exited=%d",
                    (int)session->pid, (int)pid);
    }

    if (WIFEXITED(status)) {
        jw_log_info("standalone emulator session ended pid=%d runtime_s=%ld status=%d system=%s rom=%s",
                    (int)pid, runtime_s, WEXITSTATUS(status),
                    session->system, session->rom_path);
        if (WEXITSTATUS(status) != 0 && WEXITSTATUS(status) < 128 &&
            runtime_s < JW_STANDALONE_LAUNCH_FAILURE_MAX_RUNTIME_S) {
            state->launch_status_pending = true;
            jw__standalone_display_name(session->core_id, session->core_path,
                                        state->launch_status_name,
                                        sizeof(state->launch_status_name));
            state->launch_status_code = WEXITSTATUS(status);
            jw_log_info("standalone launcher surfaced: %s exited (status %d) after %ld s",
                        state->launch_status_name, state->launch_status_code,
                        runtime_s);
        }
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("standalone emulator session terminated pid=%d runtime_s=%ld signal=%d system=%s rom=%s",
                    (int)pid, runtime_s, WTERMSIG(status),
                    session->system, session->rom_path);
    } else {
        jw_log_warn("standalone emulator session changed state pid=%d runtime_s=%ld status=%d system=%s rom=%s",
                    (int)pid, runtime_s, status, session->system, session->rom_path);
    }

    jw__retroarch_session_record_play(state, session, runtime_s);
    jw__retroarch_session_clear(session);
    state->perf_session_override = false;
    state->perf_session_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    state->perf_custom_valid = false;
    jw__perf_request_init(&state->perf_custom_request);
    (void)jw__perf_apply_frontend(state, "standalone-emulator-exit");
}

static int jw__request_open_menu(jw_daemon_state *state) {
    if (!state) {
        return -1;
    }

    if (state->child_pid <= 0) {
        return jw__spawn_child(state, JW_CHILD_MENU);
    }

    state->pending_menu = true;
    return 0;
}

/* Select which surface the resident in-game UI shows on its next reveal. The
   menu process reads this on each show wake; missing/invalid defaults to "menu".
   Written before SIGUSR1 (and before a cold spawn) so the reveal picks it up. */
static void jw__write_ingame_ui_mode(const char *mode) {
    char *path = jw_ingame_ui_mode_path();
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(mode, f);
        fclose(f);
    } else {
        jw_log_warn("in-game ui: could not write mode file %s: %s",
                    path, strerror(errno));
    }
    free(path);
}

/* Reveal the resident in-game UI in menu or switcher mode.
   Pauses RetroArch, records the desired mode, then reveals the warm standby
   (SIGUSR1) or cold-spawns it. Reversible: this never saves or quits. */
static int jw__request_open_in_game_ui(jw_daemon_state *state, const char *mode) {
    long long start_ms = jw__monotonic_ms();
    if (!jw__has_retroarch_session(state)) {
        return -1;
    }
    /* A frontend not known to speak Leaf's commands may ignore PAUSE, which
       would leave the menu over a running game. */
    if (state->retroarch_session.frontend && state->retroarch_session.frontend_probe <= 0) {
        jw_log_info("in-game %s: RetroArch frontend has not answered the probe", mode);
        return -1;
    }

    /* Already showing: ignore the request (the surface is closed with B/Continue
       or the Menu toggle). */
    if (state->menu_visible) {
        return 0;
    }

    jw_ra_client client = jw_ra_client_default();
    long long pause_start_ms = jw__monotonic_ms();
    jw_ra_result pause_result = jw_ra_pause_direct(&client);
    long long pause_done_ms = jw__monotonic_ms();
    if (pause_result != JW_RA_OK) {
        jw_log_warn("in-game menu: pause failed result=%s",
                    jw_ra_result_string(pause_result));
        return -1;
    }

    /* Pausing stops the core asking for rumble, but it does not clear whatever
       duty the core last set, so a game paused mid-effect would leave the motor
       running behind the menu. Reclaim now AND again shortly: PAUSE goes out as
       a fire-and-forget datagram, so RetroArch may still run a frame after this
       and re-assert the duty it was last driving. */
    /* Shut the route BEFORE quiescing, not after. The other order leaves a
       window where the quiesce has already run and the route is still open, so
       a resend arriving in between starts the motor behind the menu -- the very
       thing the suspend exists to prevent, just narrower. Immediately rather
       than on the next tick, because a resend can land inside the 50 ms the
       reconcile would take to notice. */
    jw__rumble_ff_suspend(true);
    jw__rumble_quiesce();
    state->rumble_reclaim_ms = jw__monotonic_ms() + 250;

    /* Tell the resident UI which surface to show before we wake it. */
    jw__write_ingame_ui_mode(mode);

    /* No screenshot round-trip: the resident menu grabs the paused frame itself
       from the DRM scanout (kmsgrab) before it maps, so the background is in the
       first visible frame. RetroArch is already paused above. */
    bool warm = state->menu_pid > 0;
    long long show_start_ms = jw__monotonic_ms();
    if (warm) {
        /* Warm standby is resident: reveal it with a show signal. Near-instant —
           no fork/exec, no cat_init. This is the common path. */
        state->menu_visible = true;
        if (kill(state->menu_pid, SIGUSR1) != 0) {
            jw_log_warn("in-game menu: show signal failed pid=%d: %s",
                        (int)state->menu_pid, strerror(errno));
            state->menu_visible = false;
            jw_ra_resume_direct(&client);
            return -1;
        }
    } else {
        /* No standby (pre-spawn failed or it crashed): spawn on demand and let
           it reveal itself once initialized. Pays the old cold-start cost, but
           only in this rare fallback. */
        if (jw__spawn_in_game_menu(state, true) != 0) {
            jw_ra_resume_direct(&client);
            return -1;
        }
        state->menu_visible = true;
    }
    long long done_ms = jw__monotonic_ms();
    jw_log_info("in-game ui open timings: mode=%s pause_ms=%lld show_ms=%lld total_ms=%lld standby=%d",
                mode,
                pause_done_ms - pause_start_ms,
                done_ms - show_start_ms,
                done_ms - start_ms,
                warm);

    /* The surface tick for this one belongs to the daemon: nothing in the
       launcher or the menu sees the reveal, because it is a signal to a resident
       process rather than a button press either of them handled. Safe here even
       though the motor was just quiesced -- the FF route is shut and the daemon
       is the only writer while the menu is up, and a tick finishes long before
       the 250 ms reclaim above. */
    jw__rumble_event(state, "select");

    return 0;
}

static int jw__request_open_in_game_menu(jw_daemon_state *state) {
    return jw__request_open_in_game_ui(state, "menu");
}

static int jw__request_open_in_game_switcher(jw_daemon_state *state) {
    return jw__request_open_in_game_ui(state, "switcher");
}

/* Unpause the game when leaving the in-game menu. First release any button still
   held on the virtual pad — the press that triggered the menu action (e.g. A on
   Save State) is usually still physically down, and without this the core reads
   it as a fresh in-game input on resume (the "character jumps after save state"
   bug). evdev is edge-based, so a still-held physical button won't re-fire until
   it is released and pressed again. */
static jw_ra_result jw__resume_game_after_menu(jw_daemon_state *state, jw_ra_client *ra) {
    jw_input_proxy_release_buttons(&state->input_proxy);
    return jw_ra_resume_direct(ra);
}

/* Close the visible standby menu (the Menu button toggles it shut): resume the
   game and tell the menu to hide back to standby. Mirror image of the open
   path; uses explicit UNPAUSE so the game always resumes. */
static int jw__request_close_in_game_menu(jw_daemon_state *state) {
    if (!state || !state->menu_visible) {
        return -1;
    }

    jw_ra_client client = jw_ra_client_default();
    /* Tick BEFORE resuming: once the game is running again the core owns the
       motor, and a UI tick queued after that races whatever it starts driving. */
    jw__rumble_event(state, "select");
    jw__resume_game_after_menu(state, &client);
    state->menu_visible = false;
    if (state->menu_pid > 0) {
        kill(state->menu_pid, SIGUSR2);
    }
    jw_log_info("in-game menu closed via Menu toggle");
    return 0;
}

#define JW_ADVANCED_SHADER_POLL_MS 25
#define JW_ADVANCED_SHADER_SETTLE_MS 100
#define JW_ADVANCED_SHADER_OPEN_TIMEOUT_MS 2000

static void jw__tick_advanced_shader(jw_daemon_state *state) {
    long long now;
    jw_ra_client client;
    jw_ra_status status;

    if (!state || !state->advanced_shader_pending) {
        return;
    }
    if (!jw__has_retroarch_session(state)) {
        jw__advanced_shader_clear(state);
        return;
    }

    now = jw__monotonic_ms();
    if (now < state->advanced_shader_next_poll_ms) {
        return;
    }
    state->advanced_shader_next_poll_ms = now + JW_ADVANCED_SHADER_POLL_MS;
    client = jw_ra_client_default();
    if (jw_ra_get_status(&client, &status) != JW_RA_OK) {
        return;
    }
    if (status.state == JW_RA_STATE_MENU) {
        if (!state->advanced_shader_menu_seen) {
            state->advanced_shader_menu_seen = true;
            state->advanced_shader_next_poll_ms =
                now + JW_ADVANCED_SHADER_SETTLE_MS;
            return;
        }
        if (!state->advanced_shader_destination_sent) {
            jw_ra_result result = jw_ra_open_shader_menu(&client);
            if (result != JW_RA_OK) {
                jw_log_warn("advanced shader destination failed result=%s",
                            jw_ra_result_string(result));
                jw__advanced_shader_clear(state);
                return;
            }
            state->advanced_shader_destination_sent = true;
            jw_log_info("advanced shader menu observed; opened shader screen");
        }
        return;
    }
    if (!state->advanced_shader_menu_seen) {
        if (now - state->advanced_shader_started_ms >=
            JW_ADVANCED_SHADER_OPEN_TIMEOUT_MS) {
            jw_log_warn("advanced shader menu was not observed; shader handoff cancelled");
            jw__advanced_shader_clear(state);
        }
        return;
    }
    if (!state->advanced_shader_destination_sent) {
        jw__advanced_shader_clear(state);
        return;
    }

    jw__advanced_shader_clear(state);
    /* Scope is now selected directly in jawaka-menu with Left/Right. Once
       RetroArch's advanced shader screen closes, simply resume the game; the
       launcher menu will be opened normally when requested. */
    (void)jw_ra_resume_direct(&client);
}

static int jw__resolve_rom_path(const jw_daemon_state *state, const char *rom_path,
                                char *out, size_t out_size) {
    if (!state || !rom_path || !rom_path[0] || !out || out_size == 0) {
        return -1;
    }

    char candidate[PATH_MAX];
    if (rom_path[0] == '/') {
        snprintf(candidate, sizeof(candidate), "%s", rom_path);
    } else {
        if (snprintf(candidate, sizeof(candidate), "%s/%s", state->sdcard_root, rom_path) >= (int)sizeof(candidate)) {
            return -1;
        }
    }

    char resolved[PATH_MAX];
    if (!realpath(candidate, resolved)) {
        return -1;
    }

    if (snprintf(out, out_size, "%s", resolved) >= (int)out_size) {
        return -1;
    }
    return 0;
}

static int jw__storage_sources(const jw_daemon_state *state, jw_storage_source_list *out) {
    if (!state || !out) {
        return -1;
    }
    return jw_storage_sources_resolve(state->sdcard_root, out);
}

static bool jw__same_resolved_path(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    if (strcmp(a, b) == 0) {
        return true;
    }

    char real_a[PATH_MAX];
    char real_b[PATH_MAX];
    if (realpath(a, real_a) && realpath(b, real_b)) {
        return strcmp(real_a, real_b) == 0;
    }
    return false;
}

static int jw__wait_for_retroarch_content(const jw_ra_client *ra,
                                          const char *rom_abs,
                                          long long timeout_ms) {
    if (!ra || !rom_abs || !rom_abs[0]) {
        return -1;
    }

    long long deadline = jw__monotonic_ms() + timeout_ms;
    for (;;) {
        jw_ra_client poll = *ra;
        poll.timeout_ms = 100u;

        char content[PATH_MAX];
        jw_ra_result result = jw_ra_get_path(&poll, "content",
                                             content, sizeof(content));
        if (result == JW_RA_OK && jw__same_resolved_path(content, rom_abs)) {
            return 0;
        }

        long long now = jw__monotonic_ms();
        if (now >= deadline) {
            jw_log_warn("resident switch: content wait timed out target=%s last_result=%s last_content=%s",
                        rom_abs, jw_ra_result_string(result),
                        result == JW_RA_OK ? content : "");
            return -1;
        }
        jw__usleep(50000);
    }
}

static void jw__publish_source_content_env(const jw_storage_source *source) {
    if (!source) {
        return;
    }
    /* Keep SDCARD_PATH/JAWAKA_SDCARD_ROOT pointed at the primary card so app
       durable state follows USERDATA_PATH. Only content roots become source-specific. */
    setenv("ROMS_PATH", source->roms_path, 1);
    setenv("IMAGES_PATH", source->images_path, 1);
    setenv("MUSIC_PATH", source->music_path, 1);
    setenv("VIDEO_PATH", source->video_path, 1);
    setenv("APPS_PATH", source->apps_path, 1);
    setenv("BIOS_PATH", source->bios_path, 1);
    setenv("SAVES_PATH", source->saves_path, 1);
    setenv("STATES_PATH", source->states_path, 1);
    setenv("CHEATS_PATH", source->cheats_path, 1);
}

typedef struct {
    const char *name;
    char *value;
    bool had_value;
} jw_saved_env;

static void jw__save_env(jw_saved_env *saved, const char *name) {
    if (!saved || !name) {
        return;
    }
    saved->name = name;
    const char *value = getenv(name);
    saved->had_value = value != NULL;
    saved->value = value ? strdup(value) : NULL;
}

static void jw__restore_env(jw_saved_env *saved, int count) {
    if (!saved || count <= 0) {
        return;
    }
    for (int i = count - 1; i >= 0; i--) {
        if (!saved[i].name) {
            continue;
        }
        if (saved[i].had_value) {
            setenv(saved[i].name, saved[i].value ? saved[i].value : "", 1);
        } else {
            unsetenv(saved[i].name);
        }
        free(saved[i].value);
        saved[i].value = NULL;
        saved[i].had_value = false;
    }
}

static void jw__publish_retroarch_source_dirs(const jw_storage_source *source,
                                              const char *system) {
    if (!source) {
        return;
    }
    if (system && strcasecmp(system, "SATURN") == 0) {
        char saturn_bios_path[JW_STORAGE_PATH_MAX +
                              sizeof(JW_BIOS_SATURN_SUBDIR) + 1u];
        int written = snprintf(saturn_bios_path, sizeof(saturn_bios_path),
                               "%s/%s", source->bios_path,
                               JW_BIOS_SATURN_SUBDIR);
        if (written >= 0 && written < (int)sizeof(saturn_bios_path)) {
            setenv("BIOS_PATH", saturn_bios_path, 1);
        }
    } else {
        setenv("BIOS_PATH", source->bios_path, 1);
    }
    setenv("SAVES_PATH", source->saves_path, 1);
    setenv("STATES_PATH", source->states_path, 1);
}

static int jw__path_is_within(const char *path, const char *root) {
    if (!path || !root) {
        return 0;
    }
    size_t root_len = strlen(root);
    return strncmp(path, root, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/');
}

static int jw__resolve_app_launch_path(jw_daemon_state *state, const char *pak_dir,
                                       char *pak_abs, size_t pak_abs_size,
                                       char *launch_abs, size_t launch_abs_size,
                                       const char **out_error) {
    if (!state || !pak_dir || !pak_dir[0] || !pak_abs || !launch_abs) {
        if (out_error) *out_error = "missing app payload";
        return -1;
    }

    jw_storage_source_list sources;
    if (jw__storage_sources(state, &sources) != 0) {
        if (out_error) *out_error = "storage sources unavailable";
        return -1;
    }
    const jw_storage_source *source = NULL;
    char candidate[PATH_MAX];
    if (pak_dir[0] == '/') {
        snprintf(candidate, sizeof(candidate), "%s", pak_dir);
    } else {
        source = jw_storage_sources_primary(&sources);
        if (!source) {
            if (out_error) *out_error = "primary storage source missing";
            return -1;
        }
        const char *rel = strncmp(pak_dir, "Apps/", 5) == 0 ? pak_dir + 5 : pak_dir;
        if (snprintf(candidate, sizeof(candidate), "%s/%s", source->apps_path, rel) >=
            (int)sizeof(candidate)) {
            if (out_error) *out_error = "app path too long";
            return -1;
        }
    }

    char resolved_pak[PATH_MAX];
    if (!realpath(candidate, resolved_pak)) {
        if (out_error) *out_error = "app pak missing";
        return -1;
    }

    if (!source) {
        source = jw_storage_sources_find_for_path(&sources, resolved_pak);
    }
    if (!source) {
        if (out_error) *out_error = "app storage source missing";
        return -1;
    }

    char apps_abs[PATH_MAX];
    if (!realpath(source->apps_path, apps_abs)) {
        if (out_error) *out_error = "Apps directory missing";
        return -1;
    }

    if (!jw__path_is_within(resolved_pak, apps_abs)) {
        if (out_error) *out_error = "app pak outside Apps";
        return -1;
    }

    char launch_candidate[PATH_MAX];
    if (snprintf(launch_candidate, sizeof(launch_candidate), "%s/launch.sh", resolved_pak) >= (int)sizeof(launch_candidate)) {
        if (out_error) *out_error = "app launch path too long";
        return -1;
    }

    if (!jw__is_regular_file(launch_candidate)) {
        if (out_error) *out_error = "app launch.sh missing or not executable";
        return -1;
    }

    char exec_error[256];
    if (!jw_sdcard_exec_available_for_path(launch_candidate, exec_error, sizeof(exec_error))) {
        jw_log_error("cannot launch app from SD: %s", exec_error);
        if (out_error) *out_error = "SD-card mounted noexec; switcher remount failed or regressed";
        return -1;
    }

    if (access(launch_candidate, X_OK) != 0) {
        if (out_error) *out_error = "app launch.sh missing or not executable";
        return -1;
    }

    if (snprintf(pak_abs, pak_abs_size, "%s", resolved_pak) >= (int)pak_abs_size ||
        snprintf(launch_abs, launch_abs_size, "%s", launch_candidate) >= (int)launch_abs_size) {
        if (out_error) *out_error = "app path too long";
        return -1;
    }

    return 0;
}

static int jw__lookup_launch_game(jw_daemon_state *state, const char *rom_path,
                                  jw_game_entry *out) {
    if (!state || !state->db_path || !rom_path || !rom_path[0] || !out) {
        return -1;
    }
    if (jw_db_get_game_by_rom_path(state->db_path, rom_path, out) == 0) {
        return 0;
    }

    char rel_path[PATH_MAX];
    if (state->sdcard_root && state->sdcard_root[0]) {
        size_t root_len = strlen(state->sdcard_root);
        if (strncmp(rom_path, state->sdcard_root, root_len) == 0 &&
            rom_path[root_len] == '/') {
            snprintf(rel_path, sizeof(rel_path), "%s", rom_path + root_len + 1);
            return jw_db_get_game_by_rom_path(state->db_path, rel_path, out);
        }
    }
    return -1;
}

static int jw__resolve_library_game(jw_daemon_state *state, int game_id,
                                    jw_game_entry *out_game,
                                    jw_storage_source_list *out_sources,
                                    const jw_storage_source **out_source,
                                    char *out_path, size_t out_path_size) {
    if (!state || game_id <= 0 || !out_game || !out_sources || !out_source ||
        !out_path || out_path_size == 0 ||
        jw_db_get_game_by_id(state->db_path, game_id, out_game) != 0 ||
        jw__storage_sources(state, out_sources) != 0) {
        return -1;
    }
    const jw_storage_source *source =
        jw_storage_sources_find_by_id(out_sources, out_game->source_id);
    if (!source ||
        jw_storage_resolve_rom(source, out_game->rom_relpath, true,
                               out_path, out_path_size) != 0) {
        return -1;
    }
    *out_source = source;
    return 0;
}

static const char *jw__launch_system_key(const char *requested_system,
                                         const jw_game_entry *game) {
    if (game && game->system[0]) {
        return game->system;
    }
    return requested_system;
}

static char *jw__resolve_launch_core_path(jw_daemon_state *state,
                                          const char *system,
                                          const char *rom_path,
                                          char *out_core_id,
                                          size_t out_core_id_size,
                                          char *out_config_folder,
                                          size_t out_config_folder_size,
                                          char *out_info_dir,
                                          size_t out_info_dir_size,
                                          char *diagnostic,
                                          size_t diagnostic_size) {
    if (out_core_id && out_core_id_size > 0) {
        out_core_id[0] = '\0';
    }
    if (diagnostic && diagnostic_size > 0) {
        diagnostic[0] = '\0';
    }
    if (out_config_folder && out_config_folder_size > 0) {
        out_config_folder[0] = '\0';
    }
    if (out_info_dir && out_info_dir_size > 0) {
        out_info_dir[0] = '\0';
    }

    jw_game_entry game;
    memset(&game, 0, sizeof(game));
    bool have_game = jw__lookup_launch_game(state, rom_path, &game) == 0;
    const char *system_key = jw__launch_system_key(system, have_game ? &game : NULL);

    char preferred[64];
    preferred[0] = '\0';
    const char *source = NULL;
    if (have_game && game.id > 0 && state && state->db_path) {
        if (jw_db_get_game_setting(state->db_path, game.id,
                                   JW_CONTENT_SETTING_CORE_ID,
                                   preferred, sizeof(preferred)) == 0 &&
            preferred[0]) {
            source = "game";
        }
    }
    if (!preferred[0] && state && state->db_path && system_key && system_key[0]) {
        if (jw_db_get_system_setting(state->db_path, system_key,
                                     JW_CONTENT_SETTING_CORE_ID,
                                     preferred, sizeof(preferred)) == 0 &&
            preferred[0]) {
            source = "system";
        }
    }

    char *core = jw_retroarch_core_path_for_system_choice(system_key,
                                                          preferred[0] ? preferred : NULL,
                                                          out_core_id,
                                                          out_core_id_size,
                                                          out_config_folder,
                                                          out_config_folder_size,
                                                          out_info_dir,
                                                          out_info_dir_size,
                                                          diagnostic,
                                                          diagnostic_size);
    if (core && source && preferred[0]) {
        jw_log_info("launch resolver: %s core override system=%s rom=%s preferred=%s effective=%s",
                    source, system_key ? system_key : "(none)",
                    rom_path ? rom_path : "(none)", preferred,
                    out_core_id && out_core_id[0] ? out_core_id : "(unknown)");
    }
    if (core && diagnostic && diagnostic[0]) {
        jw_log_warn("launch resolver: %s", diagnostic);
    }
    return core;
}

static bool jw__catalog_system_allows_core(const jw_ra_system *system,
                                           const char *core_id) {
    if (!system || !core_id || !core_id[0]) {
        return false;
    }
    if (system->default_core && strcmp(system->default_core, core_id) == 0) {
        return true;
    }
    return jw_ra_string_list_contains(&system->alternate_cores, core_id);
}

static bool jw__core_is_packaged_path(const jw_ra_core *core) {
    return core &&
           core->type &&
           strcmp(core->type, "path") == 0 &&
           core->path &&
           core->path[0];
}

static int jw__platform_path(char *out, size_t out_size, const jw_daemon_state *state) {
    const char *platform_path = jw__env_value("UMRK_PLATFORM_PATH");
    if (!platform_path) {
        platform_path = jw__env_value("SYSTEM_PATH");
    }
    if (platform_path) {
        return snprintf(out, out_size, "%s", platform_path) < (int)out_size ? 0 : -1;
    }
    if (!state || !state->sdcard_root || !state->sdcard_root[0]) {
        return -1;
    }
    return snprintf(out, out_size, "%s/.system/leaf/platforms/mlp1",
                    state->sdcard_root) < (int)out_size ? 0 : -1;
}

static int jw__resolve_path_core_executable(const jw_daemon_state *state,
                                            const jw_ra_catalog *catalog,
                                            const jw_ra_core *core,
                                            char *out,
                                            size_t out_size) {
    if (!jw__core_is_packaged_path(core) || !out || out_size == 0) {
        return -1;
    }

    char platform_path[PATH_MAX];
    if (jw__platform_path(platform_path, sizeof(platform_path), state) != 0 ||
        jw_ra_catalog_resolve_core_path(catalog, core, NULL, platform_path,
                                        true, out, out_size) != 0) {
        return -1;
    }
    return 0;
}

static const jw_ra_system *jw__catalog_find_launch_system(const jw_ra_catalog *catalog,
                                                          const char *system_id) {
    const jw_ra_system *system = jw_ra_catalog_find_system(catalog, system_id);
    if (!system) {
        system = jw_ra_catalog_match_system_folder(catalog, system_id);
    }
    return system;
}

static bool jw__try_path_core(const jw_daemon_state *state,
                              const jw_ra_catalog *catalog,
                              const jw_ra_core *core,
                              const char *rom_path,
                              jw_launch_target *target) {
    if (!target || !jw__core_is_packaged_path(core)) {
        return false;
    }

    char exec_path[PATH_MAX];
    if (jw__resolve_path_core_executable(state, catalog, core,
                                         exec_path, sizeof(exec_path)) != 0) {
        return false;
    }

    /* Resolve release identity once, from catalog provenance and the
       executable path, before any eligibility check; everything downstream
       reads this result. */
    jw_standalone_policy policy =
        jw_standalone_policy_resolve(core->id, exec_path, core->provider);
    if (!jw_standalone_policy_supports_content(&policy, rom_path)) {
        return false;
    }

    bool native_pico8 = jw_pico8_core_matches(core->id, core->provider, core->path);
    if (core->id && strcmp(core->id, JW_PICO8_CORE) == 0) {
        jw_storage_source_list sources;
        jw_pico8_paths paths;
        if (!native_pico8 || !jw_input_roster_supported() ||
            !jw_storage_source_paths_v2_valid() ||
            jw__storage_sources(state, &sources) != 0 ||
            !jw_pico8_resolve_paths(&sources, NULL, &paths) ||
            !jw_pico8_preflight(exec_path, &paths)) {
            jw_log_warn("PICO-8 preflight unavailable; using normal RetroArch fallback (saved core preference unchanged)");
            return false;
        }
    }
    memset(target, 0, sizeof(*target));
    target->native_pico8 = native_pico8;
    target->kind = JW_LAUNCH_TARGET_STANDALONE;
    snprintf(target->path, sizeof(target->path), "%s", exec_path);
    snprintf(target->core_id, sizeof(target->core_id), "%s", core->id ? core->id : "");
    snprintf(target->provider, sizeof(target->provider), "%s",
             core->provider ? core->provider : "");
    target->standalone_policy = policy;
    target->supports_menu = core->supports_menu;
    target->requires_direct_drm = core->requires_direct_drm;
    return true;
}

/* One packaged RetroArch core, exactly: its own file must exist. The target
   keeps the resolved file, configuration folder and info directory from this
   catalog snapshot, so the launch never asks another resolver. */
static bool jw__try_retroarch_core(const jw_ra_catalog *catalog,
                                   const jw_ra_core *core,
                                   const char *cores_dir,
                                   jw_launch_target *target) {
    char core_path[PATH_MAX];
    if (!target || !cores_dir || !cores_dir[0] ||
        !jw_ra_core_is_packaged_retroarch(core) ||
        jw_ra_catalog_resolve_core_path(catalog, core, cores_dir, NULL, false,
                                        core_path, sizeof(core_path)) != 0) {
        return false;
    }

    memset(target, 0, sizeof(*target));
    target->kind = JW_LAUNCH_TARGET_RETROARCH;
    snprintf(target->path, sizeof(target->path), "%s", core_path);
    snprintf(target->core_id, sizeof(target->core_id), "%s", core->id);
    if (jw_ra_core_folder_is_safe(core->config_folder) &&
        strlen(core->config_folder) < sizeof(target->core_config_folder)) {
        snprintf(target->core_config_folder, sizeof(target->core_config_folder),
                 "%s", core->config_folder);
    }
    (void)jw_ra_catalog_info_dir(catalog, target->info_dir,
                                 sizeof(target->info_dir));
    /* A core that names a frontend only runs in it: its library may not even
       load in Leaf's RetroArch (Yabadabadoo is 32-bit), so a missing frontend
       makes the core unavailable rather than falling back. */
    if (core->frontend_path && core->frontend_path[0] &&
        jw_ra_catalog_resolve_frontend_path(catalog, core, target->frontend,
                                            sizeof(target->frontend)) != 0) {
        jw_log_warn("core %s: frontend %s missing or not executable; core unavailable",
                    core->id, core->frontend_path);
        memset(target, 0, sizeof(*target));
        return false;
    }
    return true;
}

typedef struct {
    jw_daemon_state *state;
    const jw_ra_catalog *catalog;
    const char *rom_path;
    const char *cores_dir;
    /* Explicit PICO-8 fallback: only RetroArch candidates may launch. */
    bool retroarch_only;
    jw_launch_target *target;
} jw__launch_candidates;

/* Availability of one candidate for jw_core_select(). A successful check
   fills the target, and selection stops at the first success, so the target
   always describes the selected core. */
static bool jw__launch_candidate_available(void *userdata, const char *core_id) {
    jw__launch_candidates *candidates = (jw__launch_candidates *)userdata;
    const jw_ra_core *core = jw_ra_catalog_find_core(candidates->catalog, core_id);
    if (!core) {
        return false;
    }
    if (jw__core_is_packaged_path(core)) {
        return !candidates->retroarch_only &&
               jw__try_path_core(candidates->state, candidates->catalog, core,
                                 candidates->rom_path, candidates->target);
    }
    return jw__try_retroarch_core(candidates->catalog, core,
                                  candidates->cores_dir, candidates->target);
}

typedef enum {
    JW__CATALOG_LAUNCH_SELECTED = 0,
    /* The catalog describes this system and none of its cores can launch. */
    JW__CATALOG_LAUNCH_UNAVAILABLE,
    /* No catalog, or the system is not in it. */
    JW__CATALOG_LAUNCH_NO_METADATA,
} jw__catalog_launch_result;

static jw__catalog_launch_result jw__resolve_catalog_launch_target(
        jw_daemon_state *state,
        const char *system,
        const char *rom_path,
        bool retroarch_only,
        jw_launch_target *target) {
    if (!state || !target || !system || !system[0]) {
        return JW__CATALOG_LAUNCH_NO_METADATA;
    }

    jw_game_entry game;
    memset(&game, 0, sizeof(game));
    bool have_game = jw__lookup_launch_game(state, rom_path, &game) == 0;
    const char *system_key = jw__launch_system_key(system, have_game ? &game : NULL);

    char game_choice[64];
    char system_choice[64];
    game_choice[0] = '\0';
    system_choice[0] = '\0';
    if (have_game && game.id > 0 && state->db_path &&
        jw_db_get_game_setting(state->db_path, game.id,
                               JW_CONTENT_SETTING_CORE_ID,
                               game_choice, sizeof(game_choice)) != 0) {
        game_choice[0] = '\0';
    }
    if (state->db_path && system_key && system_key[0] &&
        jw_db_get_system_setting(state->db_path, system_key,
                                 JW_CONTENT_SETTING_CORE_ID,
                                 system_choice, sizeof(system_choice)) != 0) {
        system_choice[0] = '\0';
    }

    char error[256];
    const jw_ra_catalog *catalog = jw_ra_catalog_get(state->sdcard_root,
                                                     error, sizeof(error));
    if (!catalog) {
        if (error[0]) {
            jw_log_warn("launch metadata unavailable: %s", error);
        }
        return JW__CATALOG_LAUNCH_NO_METADATA;
    }
    const jw_ra_system *ra_system = jw__catalog_find_launch_system(catalog, system_key);
    if (!ra_system) {
        return JW__CATALOG_LAUNCH_NO_METADATA;
    }

    char *cores_dir = jw_retroarch_cores_dir();
    jw__launch_candidates candidates = {
        .state = state,
        .catalog = catalog,
        .rom_path = rom_path,
        .cores_dir = cores_dir,
        .retroarch_only = retroarch_only,
        .target = target,
    };
    const char *saved = jw_core_selection_saved_choice(game_choice, system_choice);
    jw_core_selection selection = jw_core_select(
        saved, ra_system->default_core,
        (const char *const *)ra_system->alternate_cores.items,
        ra_system->alternate_cores.count,
        jw__launch_candidate_available, &candidates);
    free(cores_dir);

    const char *default_core = ra_system->default_core ? ra_system->default_core : "(none)";
    if (!selection.core_id) {
        memset(target, 0, sizeof(*target));
        snprintf(target->diagnostic, sizeof(target->diagnostic),
                 "no available core for %.64s (default %.64s)",
                 system_key, default_core);
        jw_log_warn("launch resolver: no available core system=%s rom=%s default=%s saved=%s%s",
                    system_key, rom_path ? rom_path : "(none)", default_core,
                    saved ? saved : "(none)",
                    retroarch_only ? " retroarch-only" : "");
        return JW__CATALOG_LAUNCH_UNAVAILABLE;
    }

    if (saved && selection.origin != JW_CORE_SELECTION_SAVED) {
        jw_log_warn("launch resolver: saved core %s is unavailable or not allowed for %s; saved choice kept",
                    saved, system_key);
    }
    if (selection.origin == JW_CORE_SELECTION_ALTERNATE) {
        snprintf(target->diagnostic, sizeof(target->diagnostic),
                 "default core %.64s unavailable; using alternate %.64s",
                 default_core, selection.core_id);
    }
    jw_log_info("launch resolver: system=%s rom=%s core=%s kind=%s origin=%s",
                system_key, rom_path ? rom_path : "(none)", selection.core_id,
                target->kind == JW_LAUNCH_TARGET_STANDALONE ? "standalone" : "retroarch",
                jw_core_selection_origin_name(selection.origin));
    return JW__CATALOG_LAUNCH_SELECTED;
}

/* ── Saturn BIOS selection ──────────────────────────────────────────────────
   Resolved per launch, applied to the forked child only, and only for the
   YabaSanshiro standalone. A selection saved while standalone was the core
   stays in the database when the user switches to the RetroArch core: it is
   neither applied nor validated for that launch, so a missing dormant BIOS
   cannot block another emulator.

   The stored value is logical (source id + BIOS-relative path). The absolute
   path is produced here, against the cards mounted right now, and never
   persisted -- the two MLP1 mounts swap across reboots. */

typedef struct {
    bool applies;                 /* this launch is the Saturn standalone */
    jw_bios_resolution resolution;
    jw_bios_file_status status;   /* file choices only */
    char abs_path[PATH_MAX];      /* set only when status is OK */
} jw__bios_launch;

static void jw__resolve_launch_bios(jw_daemon_state *state,
                                    const jw_launch_target *target,
                                    const char *system,
                                    const char *rom_path,
                                    jw__bios_launch *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->status = JW_BIOS_FILE_NO_CHOICE;
    if (!state || !jw__standalone_target_is_yabasanshiro(target)) {
        return;
    }
    out->applies = true;

    jw_game_entry game;
    memset(&game, 0, sizeof(game));
    bool have_game = jw__lookup_launch_game(state, rom_path, &game) == 0;
    const char *system_key = jw__launch_system_key(system, have_game ? &game : NULL);

    char game_value[JW_BIOS_VALUE_MAX];
    char system_value[JW_BIOS_VALUE_MAX];
    game_value[0] = '\0';
    system_value[0] = '\0';
    if (have_game && game.id > 0 && state->db_path) {
        (void)jw_db_get_game_setting(state->db_path, game.id,
                                     JW_CONTENT_SETTING_SATURN_BIOS,
                                     game_value, sizeof(game_value));
    }
    if (state->db_path && system_key && system_key[0]) {
        (void)jw_db_get_system_setting(state->db_path, system_key,
                                       JW_CONTENT_SETTING_SATURN_BIOS,
                                       system_value, sizeof(system_value));
    }

    jw_bios_resolve(game_value, system_value, &out->resolution);
    if (out->resolution.choice.kind != JW_BIOS_CHOICE_FILE) {
        return;
    }

    jw_storage_source_list sources;
    if (jw__storage_sources(state, &sources) != 0) {
        out->status = JW_BIOS_FILE_SOURCE_UNAVAILABLE;
        return;
    }
    out->status = jw_bios_resolve_file(&sources, &out->resolution.choice,
                                       out->abs_path, sizeof(out->abs_path));
}

/* NULL when the launch may proceed; otherwise a static, actionable reason.
   Never falls back to HLE or to another file: an unavailable explicit choice
   stops the launch so the user can fix or change it. */
static const char *jw__bios_launch_error(const jw__bios_launch *bios) {
    if (!bios || !bios->applies ||
        bios->resolution.choice.kind != JW_BIOS_CHOICE_FILE ||
        bios->status == JW_BIOS_FILE_OK) {
        return NULL;
    }
    return jw_bios_file_status_text(bios->status);
}

static int jw__resolve_launch_target(jw_daemon_state *state,
                                     const char *system,
                                     const char *rom_path,
                                     const char *requested_core_id,
                                     jw_launch_target *target) {
    if (!target) {
        return -1;
    }
    memset(target, 0, sizeof(*target));
    bool retroarch_only = false;

    if (requested_core_id && requested_core_id[0]) {
        jw_game_entry game;
        memset(&game, 0, sizeof(game));
        bool have_game = jw__lookup_launch_game(state, rom_path, &game) == 0;
        const char *system_key =
            jw__launch_system_key(system, have_game ? &game : NULL);

        char error[256];
        const jw_ra_catalog *catalog =
            jw_ra_catalog_get(state->sdcard_root, error, sizeof(error));
        if (!catalog) {
            if (error[0]) {
                jw_log_warn("requested launch core metadata unavailable: %s", error);
            }
            return -1;
        }

        const jw_ra_system *ra_system =
            jw__catalog_find_launch_system(catalog, system_key);
        if (!ra_system ||
            !jw__catalog_system_allows_core(ra_system, requested_core_id)) {
            jw_log_warn("requested launch core is not allowed: system=%s core=%s",
                        system_key ? system_key : "(none)", requested_core_id);
            if (system_key && strcmp(system_key, "PICO8") == 0 &&
                strcmp(requested_core_id, JW_PICO8_CORE) == 0) {
                retroarch_only = true;
                goto catalog_selection;
            }
            return -1;
        }

        const jw_ra_core *core =
            jw_ra_catalog_find_core(catalog, requested_core_id);
        if (!jw__try_path_core(state, catalog, core, rom_path, target)) {
            jw_log_warn("requested launch core is unavailable for content or is not an executable packaged path: core=%s rom=%s",
                        requested_core_id, rom_path ? rom_path : "(none)");
            if (system_key && strcmp(system_key, "PICO8") == 0 &&
                strcmp(requested_core_id, JW_PICO8_CORE) == 0) {
                retroarch_only = true;
                goto catalog_selection;
            }
            return -1;
        }
        return 0;
    }

catalog_selection:
    switch (jw__resolve_catalog_launch_target(state, system, rom_path,
                                              retroarch_only, target)) {
    case JW__CATALOG_LAUNCH_SELECTED:
        return 0;
    case JW__CATALOG_LAUNCH_UNAVAILABLE:
        return -1;
    case JW__CATALOG_LAUNCH_NO_METADATA:
        break;
    }

    /* Compatibility route for launches without usable catalog metadata only.
       A catalog system whose cores are all unavailable fails above: a path
       this resolver can construct is not proof that a catalog core exists. */
    char core_id[64];
    char core_config_folder[256];
    char diagnostic[256];
    char *core = jw__resolve_launch_core_path(state, system, rom_path,
                                              core_id, sizeof(core_id),
                                              core_config_folder,
                                              sizeof(core_config_folder),
                                              target->info_dir,
                                              sizeof(target->info_dir),
                                              diagnostic, sizeof(diagnostic));
    if (!core) {
        if (diagnostic[0]) {
            snprintf(target->diagnostic, sizeof(target->diagnostic), "%s", diagnostic);
        }
        return -1;
    }

    target->kind = JW_LAUNCH_TARGET_RETROARCH;
    snprintf(target->path, sizeof(target->path), "%s", core);
    snprintf(target->core_id, sizeof(target->core_id), "%s", core_id);
    snprintf(target->core_config_folder, sizeof(target->core_config_folder), "%s",
             core_config_folder);
    if (diagnostic[0]) {
        snprintf(target->diagnostic, sizeof(target->diagnostic), "%s", diagnostic);
    }
    free(core);
    return 0;
}

static void jw__pending_launch_forget_target(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    memset(&state->pending_launch_target, 0, sizeof(state->pending_launch_target));
    state->pending_launch_target_valid = false;
}

static void jw__pending_launch_keep_target(jw_daemon_state *state,
                                           const jw_launch_target *target) {
    state->pending_launch_target = *target;
    state->pending_launch_target_valid = true;
}

/* Whether the selected core is still where selection found it. Only that one
   file is checked; this never looks at any other candidate. */
static bool jw__launch_target_still_present(const jw_launch_target *target,
                                            const char **why) {
    struct stat st;
    if (stat(target->path, &st) != 0 || !S_ISREG(st.st_mode)) {
        *why = "is missing";
        return false;
    }
    if (target->kind == JW_LAUNCH_TARGET_STANDALONE &&
        access(target->path, X_OK) != 0) {
        *why = "is not executable";
        return false;
    }
    return true;
}

/* This launch's core. The selection stored when the request was accepted is
   revalidated, never replaced: if the selected core disappeared, the launch
   fails and no alternate is tried. A pending launch with no stored selection
   resolves one here and keeps it for the rest of the launch. */
static int jw__pending_launch_target(jw_daemon_state *state,
                                     jw_launch_target *out) {
    if (!state || !out) {
        return -1;
    }
    if (!state->pending_launch_target_valid) {
        if (jw__resolve_launch_target(state, state->pending_launch_system,
                                      state->pending_launch_rom_path,
                                      state->pending_launch_core_id, out) != 0) {
            return -1;
        }
        jw__pending_launch_keep_target(state, out);
        return 0;
    }

    const char *why = NULL;
    if (!jw__launch_target_still_present(&state->pending_launch_target, &why)) {
        jw_log_error("launch failed: selected core %s %s (%s); no other core is tried",
                     state->pending_launch_target.core_id[0]
                         ? state->pending_launch_target.core_id : "(unknown)",
                     why, state->pending_launch_target.path);
        jw__pending_launch_forget_target(state);
        return -1;
    }
    *out = state->pending_launch_target;
    return 0;
}

static void jw__rom_stem(const char *path, char *out, size_t out_size) {
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base && base[1] ? base + 1 : (path ? path : "");
    snprintf(out, out_size, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

static const jw_ra_system *jw__legacy_system(const jw_ra_catalog *catalog,
                                             const char *system_id) {
    const jw_ra_system *system = jw_ra_catalog_find_system(catalog, system_id);
    return system ? system : jw_ra_catalog_match_system_folder(catalog, system_id);
}

/* Flat files are keyed only by ROM stem. If this source contains another known
   game with the same stem but a different declared historical owner, assigning
   the file would be guesswork, so recovery fails closed. */
static bool jw__legacy_flat_ambiguous(jw_daemon_state *state,
                                      const jw_ra_catalog *catalog,
                                      const char *source_root,
                                      const char *rom_path,
                                      const char *legacy_owner) {
    if (!state || !state->db || !catalog || !source_root || !rom_path ||
        !legacy_owner || !legacy_owner[0]) {
        return false;
    }
    char wanted_stem[512];
    jw__rom_stem(rom_path, wanted_stem, sizeof(wanted_stem));
    if (!wanted_stem[0]) return false;

    jw_storage_source_list sources;
    if (jw__storage_sources(state, &sources) != 0) return false;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(state->db, "SELECT system, rom_path FROM games;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return false;
    }
    bool ambiguous = false;
    while (!ambiguous && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *candidate_system =
            (const char *)sqlite3_column_text(stmt, 0);
        const char *candidate_path =
            (const char *)sqlite3_column_text(stmt, 1);
        char candidate_stem[512];
        jw__rom_stem(candidate_path, candidate_stem, sizeof(candidate_stem));
        if (!candidate_system || !candidate_path ||
            strcasecmp(candidate_stem, wanted_stem) != 0) {
            continue;
        }

        char candidate_abs[PATH_MAX];
        if (jw__resolve_rom_path(state, candidate_path,
                                 candidate_abs, sizeof(candidate_abs)) != 0) {
            continue;
        }
        const jw_storage_source *candidate_source =
            jw_storage_sources_find_for_path(&sources, candidate_abs);
        if (!candidate_source || strcmp(candidate_source->root, source_root) != 0) {
            continue;
        }
        const jw_ra_system *system = jw__legacy_system(catalog, candidate_system);
        const char *candidate_owner = system ? system->legacy_flat_core : NULL;
        if (!candidate_owner || !candidate_owner[0] ||
            strcmp(candidate_owner, legacy_owner) != 0) {
            ambiguous = true;
        }
    }
    sqlite3_finalize(stmt);
    return ambiguous;
}

static void jw__recover_legacy_flat(jw_daemon_state *state,
                                    const char *system_id,
                                    const char *rom_path,
                                    const char *source_root,
                                    const char *core_id,
                                    const char *core_config_folder,
                                    char *warning, size_t warning_size) {
    if (warning && warning_size > 0) warning[0] = '\0';
    if (!core_config_folder || !core_config_folder[0]) {
        jw_log_warn("state features disabled: core=%s has no safe config_folder",
                    core_id && core_id[0] ? core_id : "(unknown)");
        if (warning && warning_size > 0) {
            snprintf(warning, warning_size,
                     "Save-state features disabled: core metadata is unavailable");
        }
        return;
    }

    char error[256];
    const jw_ra_catalog *catalog =
        jw_ra_catalog_get(state->sdcard_root, error, sizeof(error));
    if (!catalog) {
        /* The session folder is already authoritative. A transient catalog
           failure must not disable an otherwise valid running namespace. */
        jw_log_warn("legacy recovery metadata unavailable: %s",
                    error[0] ? error : "unknown error");
        return;
    }

    jw_game_entry game;
    memset(&game, 0, sizeof(game));
    bool have_game = jw__lookup_launch_game(state, rom_path, &game) == 0;
    const char *system_key = jw__launch_system_key(system_id,
                                                   have_game ? &game : NULL);
    const jw_ra_system *system = jw__legacy_system(catalog, system_key);
    const char *legacy_owner = system ? system->legacy_flat_core : NULL;
    if (!legacy_owner || !legacy_owner[0] || !core_id ||
        strcmp(core_id, legacy_owner) != 0 ||
        !jw_ra_legacy_flat_files_need_recovery(source_root, rom_path,
                                               core_config_folder)) {
        return;
    }
    bool ambiguous = jw__legacy_flat_ambiguous(state, catalog, source_root,
                                               rom_path, legacy_owner);
    jw_ra_legacy_migration_report report;
    jw_ra_legacy_migration_result result =
        jw_ra_migrate_legacy_flat_files(source_root, rom_path, core_id,
                                        core_config_folder, legacy_owner,
                                        ambiguous, &report);
    switch (result) {
        case JW_RA_LEGACY_MIGRATION_COPIED:
            jw_log_info("legacy save recovery: system=%s rom=%s core=%s folder=%s %s",
                        system_key ? system_key : "(unknown)", rom_path,
                        core_id, core_config_folder, report.detail);
            break;
        case JW_RA_LEGACY_MIGRATION_AMBIGUOUS:
            jw_log_warn("legacy save recovery skipped as ambiguous: system=%s rom=%s core=%s detail=%s",
                        system_key ? system_key : "(unknown)", rom_path,
                        core_id, report.detail);
            if (warning && warning_size > 0) {
                snprintf(warning, warning_size,
                         "Legacy save recovery skipped: ambiguous ROM name");
            }
            break;
        case JW_RA_LEGACY_MIGRATION_FAILED:
            jw_log_warn("legacy save recovery failed: system=%s rom=%s core=%s detail=%s",
                        system_key ? system_key : "(unknown)", rom_path,
                        core_id, report.detail);
            if (warning && warning_size > 0) {
                snprintf(warning, warning_size,
                         "Legacy save recovery failed; original files were retained");
            }
            break;
        default:
            break;
    }
}

static jw_platform_perf_profile jw__perf_requested_for_launch(
        jw_daemon_state *state, const char *system, const char *rom_path) {
    if (!state) {
        return JW_PLATFORM_PERF_PROFILE_AUTO;
    }
    if (state->perf_session_override) {
        return state->perf_session_profile;
    }

    jw_game_entry game;
    memset(&game, 0, sizeof(game));
    bool have_game = jw__lookup_launch_game(state, rom_path, &game) == 0;
    const char *system_key = jw__launch_system_key(system, have_game ? &game : NULL);

    char value[64];
    if (have_game && game.id > 0 && state->db_path &&
        jw_db_get_game_setting(state->db_path, game.id,
                               JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                               value, sizeof(value)) == 0 &&
        value[0]) {
        jw_platform_perf_profile profile;
        if (jw_platform_parse_perf_profile(value, &profile)) {
            return profile;
        }
        jw_log_warn("performance: ignoring invalid game override %s for rom=%s",
                    value, rom_path ? rom_path : "(none)");
    }

    if (state->db_path && system_key && system_key[0] &&
        jw_db_get_system_setting(state->db_path, system_key,
                                 JW_CONTENT_SETTING_PERFORMANCE_PROFILE,
                                 value, sizeof(value)) == 0 &&
        value[0]) {
        jw_platform_perf_profile profile;
        if (jw_platform_parse_perf_profile(value, &profile)) {
            return profile;
        }
        jw_log_warn("performance: ignoring invalid system override %s for system=%s",
                    value, system_key);
    }

    return state->perf_global_profile;
}

static int jw__perf_apply_launch_game(jw_daemon_state *state, const char *system,
                                      const char *rom_path, const char *reason) {
    return jw__perf_apply_profile(state,
                                  jw__perf_requested_for_launch(state, system, rom_path),
                                  system, reason);
}

/* On success, out_target (optional) receives the core this request selected.
   The caller stores it so the rest of the launch uses the same core. */
static int jw__validate_launch_request(jw_daemon_state *state, const char *system,
                                       const char *rom_path,
                                       const char *requested_core_id,
                                       jw_launch_target *out_target,
                                       const char **out_error) {
    if (!state || !system || !system[0] || !rom_path || !rom_path[0]) {
        if (out_error) *out_error = "missing launch payload";
        return -1;
    }
    if (jw_delete_job_busy(state->delete_job)) {
        if (out_error) *out_error = "ROM deletion is preparing or committing";
        return -1;
    }

    jw_game_entry reserved_game;
    if (jw__lookup_launch_game(state, rom_path, &reserved_game) == 0 &&
        jw_db_relocation_game_reserved(state->db, reserved_game.id)) {
        if (out_error) *out_error = "game is relocating";
        return -1;
    }

    jw_launch_target target;
    if (jw__resolve_launch_target(state, system, rom_path,
                                  requested_core_id, &target) != 0) {
        if (out_error) {
            *out_error = requested_core_id && requested_core_id[0]
                ? "requested core unavailable"
                : "unsupported system";
        }
        return -1;
    }

    if (target.kind == JW_LAUNCH_TARGET_RETROARCH) {
        /* The binary this launch will exec: the pak's frontend when the core
           names one, otherwise Leaf's own RetroArch. */
        char *retroarch = target.frontend[0] ? strdup(target.frontend)
                                             : jw_retroarch_bin_path();
        if (!retroarch || !jw__path_exists(retroarch)) {
            free(retroarch);
            if (out_error) *out_error = "RetroArch binary missing";
            return -1;
        }
        free(retroarch);
    }

    if (!jw__path_exists(target.path)) {
        if (out_error) {
            *out_error = target.kind == JW_LAUNCH_TARGET_STANDALONE
                ? "standalone emulator missing"
                : "libretro core missing";
        }
        return -1;
    }

    char exec_error[256];
    if (!jw_sdcard_exec_available_for_path(target.path, exec_error, sizeof(exec_error))) {
        jw_log_error("cannot launch game target from SD: %s", exec_error);
        if (out_error) *out_error = "SD-card mounted noexec; switcher remount failed or regressed";
        return -1;
    }

    char rom_abs[PATH_MAX];
    jw_game_entry game;
    jw_storage_source_list sources;
    const jw_storage_source *source = NULL;
    if (jw__lookup_launch_game(state, rom_path, &game) != 0 ||
        jw__resolve_library_game(state, game.id, &game, &sources, &source,
                                 rom_abs, sizeof(rom_abs)) != 0) {
        if (out_error) *out_error = "ROM path missing";
        return -1;
    }

    /* An explicitly selected Saturn BIOS that is gone, unreadable or the wrong
       size refuses the request here, while the launcher is still on screen and
       the user can correct the choice. */
    jw__bios_launch bios;
    jw__resolve_launch_bios(state, &target, system, rom_path, &bios);
    const char *bios_error = jw__bios_launch_error(&bios);
    if (bios_error) {
        jw_log_warn("launch blocked: %s (source=%s path=%s)", bios_error,
                    bios.resolution.choice.source_id,
                    bios.resolution.choice.rel_path);
        if (out_error) *out_error = bios_error;
        return -1;
    }
    if (out_target) {
        *out_target = target;
    }
    return 0;
}

static int jw__request_launch_game(jw_daemon_state *state, const char *system,
                                   const char *rom_path,
                                   const char *requested_core_id,
                                   bool switcher_resume,
                                   const char **out_error) {
    jw_launch_target selected;
    if (jw__validate_launch_request(state, system, rom_path,
                                    requested_core_id, &selected,
                                    out_error) != 0) {
        return -1;
    }
    /* Choosing another title supersedes a previously cancelled blocked
       request. An override is always one-shot and must be chosen again. */
    state->game_launch_blocked = false;
    state->game_launch_blocked_resume_switcher = false;
    state->pending_launch_override_unverified = false;
    state->pending_launch_skip_check = false;
    state->game_launch_blocked_requires_verified_stop = false;
    state->game_coordination_ready = false;
    state->game_check_decision = false;
    state->game_check_pending_items = 0;
    state->game_check_pending_bytes = 0;
    state->game_check_service_id[0] = '\0';
    state->game_launch_blocked_service_id[0] = '\0';
    state->game_launch_blocked_reason[0] = '\0';
    jw_game_entry game;
    if (jw__lookup_launch_game(state, rom_path, &game) != 0) {
        if (out_error) *out_error = "game no longer exists";
        return -1;
    }

    state->pending_launch_game_id = game.id;
    snprintf(state->pending_launch_system, sizeof(state->pending_launch_system), "%s", system);
    snprintf(state->pending_launch_rom_path, sizeof(state->pending_launch_rom_path), "%s", rom_path);
    snprintf(state->pending_launch_core_id, sizeof(state->pending_launch_core_id),
             "%s", requested_core_id ? requested_core_id : "");
    jw__pending_launch_keep_target(state, &selected);
    state->pending_launch_resume_switcher = switcher_resume;
    state->pending_launch = true;

    if (state->child_pid <= 0) {
        if (jw__spawn_pending_game(state) != 0) {
            state->pending_launch_resume_switcher = false;
            if (out_error) *out_error = "game spawn failed";
            return -1;
        }
    }

    return 0;
}

static bool jw__wait_for_tracked_child_exit(jw_daemon_state *state, pid_t pid,
                                            long long timeout_ms) {
    if (!state || pid <= 0) {
        return true;
    }

    long long deadline = jw__monotonic_ms() + timeout_ms;
    for (;;) {
        jw__handle_child_exit(state);
        if (state->child_pid != pid || state->child_kind != JW_CHILD_RETROARCH) {
            return true;
        }

        long long now = jw__monotonic_ms();
        if (now >= deadline) {
            return false;
        }
        jw__usleep(50000);
    }
}

static bool jw__force_retroarch_exit_if_needed(jw_daemon_state *state, pid_t pid,
                                               const char *reason) {
    if (jw__wait_for_tracked_child_exit(state, pid,
                                        JW_RETROARCH_QUIT_GRACE_MS)) {
        return true;
    }

    jw_log_warn("%s: RetroArch did not exit after QUIT; forcing pid=%d",
                reason ? reason : "retroarch", (int)pid);
    int kill_rc = state->child_pid == pid &&
                          state->child_kind == JW_CHILD_RETROARCH
                      ? jw__signal_tracked_game_group(state, SIGKILL)
                      : kill(pid, SIGKILL);
    if (kill_rc != 0 && errno != ESRCH) {
        jw_log_warn("%s: SIGKILL failed pid=%d: %s",
                    reason ? reason : "retroarch", (int)pid,
                    strerror(errno));
    }

    return jw__wait_for_tracked_child_exit(state, pid,
                                           JW_RETROARCH_KILL_GRACE_MS);
}

/* True when system + resolved ROM path match the running session — used so a
   switch-game request targeting the current game resumes instead of switching. */
static bool jw__is_current_session_game(jw_daemon_state *state,
                                        const char *system, const char *rom_path) {
    if (!jw__has_retroarch_session(state) || !rom_path) {
        return false;
    }
    const jw_retroarch_session *s = &state->retroarch_session;
    if (system && system[0] && s->system[0] && strcmp(system, s->system) != 0) {
        return false;
    }
    jw_game_entry game;
    return jw__lookup_launch_game(state, rom_path, &game) == 0 &&
           game.id == s->game_id;
}

/* RetroArch writes save states asynchronously — the SAVE_STATE command returns
   well before the file is on disk. Tearing the emulator down right after (quit,
   or a relaunch for a game switch) truncates the write, and the partial state
   then crashes the core on the next resume (seen on N64: a ~16MB mupen state cut
   to 100KB/5.7MB). After issuing a save to `slot`, wait until that slot's file
   has stopped growing before proceeding, then sync. A short warm-up avoids
   mistaking a *pre-existing* (already-stable) state file for the new write.
   Best-effort: bounded by a timeout so a user is never stranded. */
static void jw__wait_for_savestate_write(const jw_daemon_state *state, int slot) {
    const char *source_root = state->retroarch_session.source_root[0]
        ? state->retroarch_session.source_root
        : state->sdcard_root;
    char states_dir[PATH_MAX];
    if (!source_root || !source_root[0] ||
        snprintf(states_dir, sizeof(states_dir), "%s/States", source_root) >=
            (int)sizeof(states_dir)) {
        return;
    }
    const char *rom = state->retroarch_session.rom_path;
    if (!rom || !rom[0]) {
        return;
    }

    const int timeout_ms = 8000;
    const int poll_ms = 100;
    const int warmup_ms = 800;     /* let RA truncate + begin the new write */
    const int stable_polls = 3;    /* ~300ms of no growth = write finished */
    long long last_size = -1;
    int stable = 0;
    int elapsed = 0;
    char path[PATH_MAX];
    const char *core_folder = state->retroarch_session.core_config_folder;
    if (!core_folder[0]) {
        jw_log_warn("savestate settle skipped: active core has no safe config_folder");
        return;
    }
    while (elapsed < timeout_ms) {
        jw__usleep((unsigned long)poll_ms * 1000UL);
        elapsed += poll_ms;

        struct stat st;
        if (!jw_ra_find_slot_state_for_core(states_dir, core_folder, rom, slot,
                                             path, sizeof(path)) ||
            stat(path, &st) != 0) {
            continue;   /* not created yet */
        }
        long long sz = (long long)st.st_size;
        if (sz != last_size) {
            stable = 0;            /* still being written (or just truncated) */
        } else if (elapsed >= warmup_ms && sz > 0 && ++stable >= stable_polls) {
            sync();
            jw_log_info("savestate settled slot=%d bytes=%lld waited_ms=%d",
                        slot, sz, elapsed);
            return;
        }
        last_size = sz;
    }
    jw_log_warn("savestate did not settle slot=%d within %dms; proceeding anyway",
                slot, timeout_ms);
}

/* Commit a switch from the in-game switcher: save the current game into the
   reserved switcher slot, then prefer an in-process same-core/same-source
   content load. If RetroArch cannot do that, queue the selected game and quit;
   the child-exit handler records the old game's playtime and spawns the queued
   game directly — no launcher flash in between. */
static int jw__request_switch_game(jw_daemon_state *state, const char *system,
                                   const char *rom_path, const char **out_error) {
    if (!jw__has_retroarch_session(state)) {
        if (out_error) *out_error = "no active RetroArch session";
        return -1;
    }
    jw_launch_target selected;
    if (jw__validate_launch_request(state, system, rom_path, NULL, &selected,
                                    out_error) != 0) {
        return -1;
    }
    jw_game_entry target_game;
    if (jw__lookup_launch_game(state, rom_path, &target_game) != 0) {
        if (out_error) *out_error = "game no longer exists";
        return -1;
    }

    /* Selecting the running game is a resume, not a switch. */
    if (jw__is_current_session_game(state, system, rom_path)) {
        jw_ra_client ra = jw_ra_client_default();
        jw_ra_result resume = jw_ra_resume_direct(&ra);
        if (resume != JW_RA_OK) {
            jw_log_error("switch-game: current-game resume failed result=%s",
                         jw_ra_result_string(resume));
            if (out_error) *out_error = "resume failed";
            return -1;
        }
        state->retroarch_resume_on_menu_exit = false;
        state->menu_visible = false;
        if (state->menu_pid > 0) {
            kill(state->menu_pid, SIGUSR2);
        }
        return 0;
    }

    if (!state->retroarch_session.core_config_folder[0]) {
        jw_log_warn("switch-game disabled: active core has no safe config_folder");
        if (out_error) *out_error = "savestate namespace unavailable";
        return -1;
    }
    if (state->retroarch_session.resume_unconfirmed) {
        /* Switching saves slot 99, which still holds the position being resumed. */
        jw_log_warn("switch-game: the resumed game has not loaded yet");
        if (out_error) *out_error = "game is still resuming";
        return -1;
    }

    jw_ra_client ra = jw_ra_client_default();

    jw_ra_info info;
    memset(&info, 0, sizeof(info));
    jw_ra_result info_result = jw_ra_get_info(&ra, &info);
    if (info_result != JW_RA_OK) {
        jw_log_error("switch-game: state support probe failed result=%s",
                     jw_ra_result_string(info_result));
        if (out_error) *out_error = "savestate unavailable";
        return -1;
    }
    if (!info.savestate_supported) {
        jw_log_error("switch-game: core does not support savestates");
        if (out_error) *out_error = "savestates unsupported";
        return -1;
    }

    char reply[JW_RA_REPLY_MAX];
    jw_ra_result sv = jw_ra_save_state_slot(&ra, JW_RA_GAME_SWITCHER_STATE_SLOT,
                                            reply, sizeof(reply));
    if (sv != JW_RA_OK) {
        jw_log_error("switch-game: slot %d save-state failed result=%s",
                     JW_RA_GAME_SWITCHER_STATE_SLOT, jw_ra_result_string(sv));
        if (out_error) *out_error = "save-state failed";
        return -1;
    }
    jw_log_info("switch-game: saved current game state slot=%d",
                JW_RA_GAME_SWITCHER_STATE_SLOT);
    jw__wait_for_savestate_write(state, JW_RA_GAME_SWITCHER_STATE_SLOT);

    char target_rom_abs[PATH_MAX];
    target_rom_abs[0] = '\0';
    char target_source_root[PATH_MAX];
    target_source_root[0] = '\0';
    char target_core_id[64];
    target_core_id[0] = '\0';
    char target_core_folder[256];
    target_core_folder[0] = '\0';
    char target_warning[256];
    target_warning[0] = '\0';
    char *target_core = NULL;
    bool resident_eligible = false;
    int resident_switch_max = jw__resident_switch_max();

    {
        jw_storage_source_list sources;
        const jw_storage_source *target_source = NULL;
        if (jw__resolve_library_game(state, target_game.id, &target_game,
                                     &sources, &target_source, target_rom_abs,
                                     sizeof(target_rom_abs)) == 0) {
        if (target_source) {
            snprintf(target_source_root, sizeof(target_source_root), "%s",
                     target_source->root);
        } else {
            snprintf(target_source_root, sizeof(target_source_root), "%s",
                     state->sdcard_root ? state->sdcard_root : "");
        }

        /* The core this switch selected at validation; resident and cold
           switches both use it. */
        const jw_launch_target target = selected;
        bool target_is_retroarch = target.kind == JW_LAUNCH_TARGET_RETROARCH;
        if (target_is_retroarch) {
            snprintf(target_core_id, sizeof(target_core_id), "%s", target.core_id);
            snprintf(target_core_folder, sizeof(target_core_folder), "%s",
                     target.core_config_folder);
            target_core = strdup(target.path);
            if (!target_core_folder[0] &&
                strcmp(target.path, state->retroarch_session.core_path) == 0 &&
                strcmp(target.core_id, state->retroarch_session.core_id) == 0) {
                snprintf(target_core_folder, sizeof(target_core_folder), "%s",
                         state->retroarch_session.core_config_folder);
            }
        }
        resident_eligible =
            target_core && target_core[0] &&
            (resident_switch_max < 0 ||
             state->retroarch_session.resident_switches < resident_switch_max) &&
            state->retroarch_session.core_path[0] &&
            strcmp(target_core, state->retroarch_session.core_path) == 0 &&
            target_source_root[0] &&
            state->retroarch_session.source_root[0] &&
            strcmp(target_source_root, state->retroarch_session.source_root) == 0;
        }
    }

    /* A participating lifecycle service needs the old writer barrier and a
       fresh launch exchange. Without one, keep the original resident path: the
       process group and source binding are unchanged. */
    if (resident_eligible &&
        jw_svc_supervisor_game_has_participant(state->services)) {
        jw_log_info("resident switch disabled by active LIFE-1 participant; using cold handoff");
        resident_eligible = false;
    }

    if (resident_eligible) {
        jw__recover_legacy_flat(state, system, target_rom_abs,
                                target_source_root, target_core_id,
                                target_core_folder, target_warning,
                                sizeof(target_warning));
        (void)jw__perf_apply_launch_game(state, system, rom_path, "resident-switch");
        long long resident_start_ms = jw__monotonic_ms();
        char load_reply[JW_RA_REPLY_MAX];
        jw_ra_result load_content =
            jw_ra_load_content_current_core(&ra, target_rom_abs,
                                            load_reply, sizeof(load_reply));
        if (load_content == JW_RA_OK &&
            jw__wait_for_retroarch_content(&ra, target_rom_abs, 2500LL) == 0) {
            int slot = 0;
            char state_path[PATH_MAX];
            char states_dir[PATH_MAX];
            bool have_resume_state = false;
            state_path[0] = '\0';

            if (snprintf(states_dir, sizeof(states_dir), "%s/States",
                         target_source_root) < (int)sizeof(states_dir)) {
                have_resume_state = jw_ra_find_resume_state_for_core(
                    states_dir,
                    target_core_folder,
                    target_rom_abs, JW_RA_GAME_SWITCHER_STATE_SLOT,
                    &slot, state_path, sizeof(state_path));
            }

            jw__retroarch_session_retarget(state, target_game.id,
                                           system, target_rom_abs,
                                           rom_path, target_source_root,
                                           target_core,
                                           target_core_id,
                                           target_core_folder,
                                           target_warning);
            /* Same card and core, so only the ROM in the resume identity
               moves with the switch. */
            snprintf(state->retroarch_session.rom_relpath,
                     sizeof(state->retroarch_session.rom_relpath), "%s",
                     target_game.rom_relpath);

            if (have_resume_state) {
                char load_state_reply[JW_RA_REPLY_MAX];
                jw_ra_result load_state =
                    jw_ra_load_state_slot(&ra, slot,
                                          load_state_reply,
                                          sizeof(load_state_reply));
                if (load_state == JW_RA_OK) {
                    jw_log_info("resident switch: loaded slot=%d path=%s",
                                slot, state_path);
                } else {
                    jw_log_warn("resident switch: state load failed slot=%d path=%s result=%s",
                                slot, state_path,
                                jw_ra_result_string(load_state));
                }
            } else {
                jw_log_info("resident switch: no resume state found for %s",
                            target_rom_abs);
            }

            jw_ra_resume_direct(&ra);
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
            jw_log_info("resident switch timings: total_ms=%lld rom=%s",
                        jw__monotonic_ms() - resident_start_ms,
                        target_rom_abs);
            free(target_core);
            return 0;
        }

        jw_log_warn("resident switch unavailable result=%s reply=%s; falling back to cold switch",
                    jw_ra_result_string(load_content),
                    load_content == JW_RA_OK ? load_reply : "");
    } else {
        jw_log_info("resident switch skipped: eligible=%s resident_switches=%d resident_switch_max=%d target_source=%s target_core=%s current_source=%s current_core=%s",
                    resident_eligible ? "true" : "false",
                    state->retroarch_session.resident_switches,
                    resident_switch_max,
                    target_source_root[0] ? target_source_root : "(unknown)",
                    target_core ? target_core : "(unknown)",
                    state->retroarch_session.source_root[0]
                        ? state->retroarch_session.source_root
                        : "(unknown)",
                    state->retroarch_session.core_path[0]
                        ? state->retroarch_session.core_path
                        : "(unknown)");
    }
    free(target_core);

    snprintf(state->pending_launch_system, sizeof(state->pending_launch_system),
             "%s", system);
    state->pending_launch_game_id = target_game.id;
    snprintf(state->pending_launch_rom_path, sizeof(state->pending_launch_rom_path),
             "%s", rom_path);
    /* A switch never carries an explicit core; don't inherit the previous
       launch's request. */
    state->pending_launch_core_id[0] = '\0';
    jw__pending_launch_keep_target(state, &selected);
    state->pending_launch_resume_switcher = true;
    state->pending_launch = true;

    pid_t old_retroarch_pid = state->child_pid;
    jw_ra_result q = jw_ra_quit(&ra);
    if (q != JW_RA_OK) {
        jw_log_error("switch-game: quit failed result=%s", jw_ra_result_string(q));
        state->pending_launch = false;
        jw__pending_launch_forget_target(state);
        state->pending_launch_resume_switcher = false;
        /* The target's profile was applied before the switch attempt; the old
           game is still running under the old system, so restore its mode. */
        (void)jw__perf_apply_current_context(state, "switch-quit-failed");
        if (out_error) *out_error = "RetroArch quit failed";
        return -1;
    }
    if (!jw__force_retroarch_exit_if_needed(state, old_retroarch_pid,
                                            "switch-game")) {
        jw_log_error("switch-game: RetroArch did not exit after forced kill pid=%d",
                     (int)old_retroarch_pid);
        state->pending_launch = false;
        jw__pending_launch_forget_target(state);
        state->pending_launch_resume_switcher = false;
        (void)jw__perf_apply_current_context(state, "switch-exit-failed");
        if (out_error) *out_error = "RetroArch exit failed";
        return -1;
    }

    state->retroarch_resume_on_menu_exit = false;
    state->menu_visible = false; /* committed: do not resume the old game */
    return 0;
}

static int jw__request_launch_app(jw_daemon_state *state, const char *pak_dir,
                                  const char **out_error) {
    if (jw_delete_job_busy(state->delete_job)) {
        if (out_error) *out_error = "ROM deletion in progress";
        return -1;
    }
    if (state->services &&
        jw_svc_supervisor_package_active(state->services)) {
        if (out_error) *out_error = "package operation in progress";
        return -1;
    }
    if (state->services &&
        jw_svc_supervisor_mutation_target_blocked(state->services, pak_dir)) {
        if (out_error) *out_error = "package mutation in progress";
        return -1;
    }
    char pak_abs[PATH_MAX];
    char launch_abs[PATH_MAX];
    if (jw__resolve_app_launch_path(state, pak_dir, pak_abs, sizeof(pak_abs),
                                    launch_abs, sizeof(launch_abs), out_error) != 0) {
        return -1;
    }

    snprintf(state->pending_app_pak_dir, sizeof(state->pending_app_pak_dir), "%s", pak_dir);
    state->pending_app = true;

    if (state->child_pid <= 0) {
        if (jw__spawn_app(state) != 0) {
            if (out_error) *out_error = "app spawn failed";
            return -1;
        }
    }

    return 0;
}

static bool jw__env_is_disabled(const char *name) {
    const char *value = getenv(name);
    return value && strcmp(value, "0") == 0;
}

static bool jw__env_is_truthy(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 && strcmp(value, "no") != 0;
}

static void jw__osd_ready_reset(jw_daemon_state *state) {
    if (state->osd_ready_fd >= 0) {
        close(state->osd_ready_fd);
    }
    state->osd_ready_fd = -1;
    state->osd_ready = false;
}

/* Ready means the OSD has opened its fonts, initialised its backend and is
   listening: it writes one byte on the readiness pipe only once all three
   hold. A pid or a socket path alone proves none of that. Waits at most
   timeout_ms, which callers keep inside their own show budget. */
static bool jw__osd_wait_ready(jw_daemon_state *state, int timeout_ms) {
    if (state->osd_ready) {
        return true;
    }
    if (state->osd_pid <= 0 || state->osd_ready_fd < 0) {
        return false;
    }
    struct pollfd pfd = { .fd = state->osd_ready_fd, .events = POLLIN };
    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms > 0 ? timeout_ms : 0);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0) {
        return false;
    }
    char byte = 0;
    ssize_t n;
    do {
        n = read(state->osd_ready_fd, &byte, 1);
    } while (n < 0 && errno == EINTR);
    close(state->osd_ready_fd);
    state->osd_ready_fd = -1;
    if (n != 1) {
        jw_log_warn("jawaka-osd pid=%d exited before it was ready", (int)state->osd_pid);
        return false;
    }
    state->osd_ready = true;
    jw_log_info("jawaka-osd ready pid=%d spawn_to_ready_ms=%lld", (int)state->osd_pid,
                jw__monotonic_ms() - state->osd_spawned_ms);
    return true;
}

static void jw__stop_osd_child(jw_daemon_state *state) {
    if (!state || state->osd_pid <= 0) {
        return;
    }

    /* Whatever the OSD shows goes with it, including a PICO-8 exit prompt. */
    jw_osd_client_prompt_lost(&state->pico8_exit_confirm_until_ms);
    jw__osd_ready_reset(state);
    pid_t pid = state->osd_pid;
    kill(pid, SIGTERM);
    for (int attempt = 0; attempt < 10; attempt++) {
        int status = 0;
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || (waited < 0 && errno == ECHILD)) {
            state->osd_pid = -1;
            return;
        }
        jw__usleep(50000);
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    state->osd_pid = -1;
}

static int jw__spawn_osd(jw_daemon_state *state) {
    if (!state || state->osd_pid > 0 || jw__env_is_disabled("JAWAKA_OSD")) {
        return 0;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/jawaka-osd", state->bin_dir) >=
        (int)sizeof(path)) {
        jw_log_warn("osd binary path too long: %s/jawaka-osd", state->bin_dir);
        return -1;
    }
    if (!jw__path_exists(path)) {
        jw_log_warn("osd binary missing: %s", path);
        return -1;
    }

    /* Resolve appearance from the DB here in the parent — opening SQLite between
       fork() and execv() is not fork-safe on macOS (os_log landmine). The OSD
       draws no status bar, so skip the Bluetooth snapshot: it shells out to
       bluetoothctl on this loop, the one that forwards the D-pad. */
    jw_appearance_env appearance;
    jw_appearance_resolve_settings(state->db_path, &appearance);

    /* Close-on-exec on both ends so no other child inherits them; the OSD
       child clears it on the write end just before its own exec. */
    int ready_pipe[2] = { -1, -1 };
    char ready_env[16] = "";
    if (pipe(ready_pipe) == 0) {
        (void)fcntl(ready_pipe[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(ready_pipe[1], F_SETFD, FD_CLOEXEC);
        snprintf(ready_env, sizeof(ready_env), "%d", ready_pipe[1]);
    } else {
        jw_log_warn("osd readiness pipe failed: %s", strerror(errno));
    }

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_warn("osd fork failed: %s", strerror(errno));
        if (ready_pipe[0] >= 0) close(ready_pipe[0]);
        if (ready_pipe[1] >= 0) close(ready_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        jw_appearance_apply_env(&appearance);
        if (ready_pipe[1] >= 0 && fcntl(ready_pipe[1], F_SETFD, 0) == 0) {
            setenv("JAWAKA_OSD_READY_FD", ready_env, 1);
        } else {
            unsetenv("JAWAKA_OSD_READY_FD");
        }
        char *const argv[] = { (char *)path, NULL };
        execv(path, argv);
        perror("execv");
        _exit(127);
    }

    if (ready_pipe[1] >= 0) {
        close(ready_pipe[1]);
    }
    state->osd_pid = pid;
    state->osd_ready_fd = ready_pipe[0];
    /* Without a pipe there is no signal to wait for; fall back to the socket. */
    state->osd_ready = ready_pipe[0] < 0;
    state->osd_spawned_ms = jw__monotonic_ms();
    snprintf(state->osd_language, sizeof(state->osd_language), "%s", appearance.language);
    snprintf(state->osd_font_path, sizeof(state->osd_font_path), "%s",
             appearance.font_path ? appearance.font_path : "");
    snprintf(state->osd_font_bump, sizeof(state->osd_font_bump), "%s", appearance.font_bump);
    jw_log_info("spawned jawaka-osd pid=%d", (int)pid);
    return 0;
}

static void jw__handle_osd_exit(jw_daemon_state *state) {
    if (!state || state->osd_pid <= 0) {
        return;
    }
    /* Notice readiness without a request so spawn-to-ready is logged. */
    if (!state->osd_ready && state->osd_ready_fd >= 0) {
        (void)jw__osd_wait_ready(state, 0);
    }

    int status = 0;
    pid_t waited = waitpid(state->osd_pid, &status, WNOHANG);
    if (waited == 0 || waited < 0) {
        return;
    }

    state->osd_pid = -1;
    /* A restarted OSD shows nothing, so no old deadline may confirm an exit. */
    jw_osd_client_prompt_lost(&state->pico8_exit_confirm_until_ms);
    jw__osd_ready_reset(state);
    if (WIFEXITED(status)) {
        jw_log_info("jawaka-osd exited status=%d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("jawaka-osd terminated signal=%d", WTERMSIG(status));
    }

    if (!state->shutdown_requested && !g_shutdown_requested &&
        !state->direct_drm_active) {
        jw__spawn_osd(state);
    }
}

static void jw__spawn_ledd_for(jw_daemon_state *state, const jw_led_config *led);

static void jw__handle_ledd_exit(jw_daemon_state *state) {
    if (!state || state->ledd_pid <= 0) {
        return;
    }

    int status = 0;
    pid_t waited = waitpid(state->ledd_pid, &status, WNOHANG);
    if (waited == 0 || waited < 0) {
        return;
    }

    state->ledd_pid = -1;
    bool usage_exit = false;
    if (WIFEXITED(status)) {
        usage_exit = WEXITSTATUS(status) == JW_LEDD_EXIT_USAGE;
        jw_log_info("jawaka-ledd exited status=%d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("jawaka-ledd terminated signal=%d", WTERMSIG(status));
    }

    if (state->shutdown_requested || g_shutdown_requested ||
        !state->led_configured) {
        return;
    }
    /* Restart the helper alone so a mid-session crash recovers the effect.
       The platform baseline has not changed, so it is not written again: a
       full re-apply rewrites the stock LED config on /oem every time. */
    long long now = jw__monotonic_ms();
    if (!jw_ledd_should_restart(&state->ledd_restarts, usage_exit,
                                now - state->ledd_started_ms, now)) {
        jw_log_warn("jawaka-ledd %s; not restarting it until the lighting is set again",
                    usage_exit ? "rejected its arguments" : "keeps exiting at start");
        return;
    }
    jw__spawn_ledd_for(state, &state->cached_led);
}

static bool jw__osd_enabled(const jw_daemon_state *state) {
    return state && state->osd_socket_path && !jw__env_is_disabled("JAWAKA_OSD");
}

static int jw__osd_request(void *ctx, const char *json, int timeout_ms, bool show) {
    jw_daemon_state *state = (jw_daemon_state *)ctx;
    if (!jw__osd_enabled(state) || !json) {
        return -1;
    }
    if (state->osd_pid <= 0) {
        /* No OSD process shows nothing, so a hide has nothing to remove. */
        if (!show) {
            return 0;
        }
        /* Mid-shutdown there is nobody left to read a banner and the screen is
           about to go; starting an OSD process now would only leave a stray
           child behind. */
        if (state->shutdown_requested || g_shutdown_requested) {
            return -1;
        }
        jw__spawn_osd(state);
    }
    /* Readiness shares the request's budget rather than extending it. */
    long long started_ms = jw__monotonic_ms();
    if (!jw__osd_wait_ready(state, timeout_ms)) {
        jw_log_warn("osd: not ready within %dms", timeout_ms);
        return -1;
    }
    int remaining_ms = timeout_ms - (int)(jw__monotonic_ms() - started_ms);
    if (remaining_ms < 1) {
        return -1;
    }
    char *response = NULL;
    size_t response_len = 0;
    int rc = jw_ipc_request_timeout(state->osd_socket_path, json, strlen(json),
                                    &response, &response_len, remaining_ms);
    if (rc == 0) {
        cJSON *reply = response ? cJSON_Parse(response) : NULL;
        cJSON *type = reply ? cJSON_GetObjectItemCaseSensitive(reply, "type") : NULL;
        if (!cJSON_IsString(type) || strcmp(type->valuestring, "ok") != 0) rc = -1;
        cJSON_Delete(reply);
    }
    free(response);
    return rc;
}

static long long jw__osd_now_ms(void *ctx) {
    (void)ctx;
    return jw__monotonic_ms();
}

static void jw__osd_sleep_ms(void *ctx, int ms) {
    (void)ctx;
    jw__usleep((useconds_t)ms * 1000u);
}

/* A PICO-8 prompt may be on screen and even its hide went unanswered. The OSD
   is wedged or badly late; ending it takes its surface down, and the ordinary
   child-exit supervision starts a fresh one. */
static void jw__osd_discard(void *ctx) {
    jw_daemon_state *state = (jw_daemon_state *)ctx;
    if (!state || state->osd_pid <= 0) {
        return;
    }
    jw_log_warn("osd: prompt removal unconfirmed; ending jawaka-osd pid=%d",
                (int)state->osd_pid);
    jw_osd_client_prompt_lost(&state->pico8_exit_confirm_until_ms);
    if (kill(state->osd_pid, SIGKILL) != 0 && errno != ESRCH) {
        jw_log_warn("osd: could not end pid=%d: %s", (int)state->osd_pid, strerror(errno));
    }
}

static jw_osd_client jw__osd_client(jw_daemon_state *state) {
    return (jw_osd_client){
        .ctx = state,
        .request = jw__osd_request,
        .now_ms = jw__osd_now_ms,
        .sleep_ms = jw__osd_sleep_ms,
        .discard_osd = jw__osd_discard,
    };
}

/* An OSD process opens its font once, so a new language, font family or font
   size needs a new process. Restart it through the ordinary child-exit
   supervision, which respawns with the resolved appearance and never runs two
   OSD processes at once. Nothing to do when none is running: the next spawn
   resolves the appearance anyway. */
static void jw__osd_refresh_appearance(jw_daemon_state *state, const char *why) {
    if (!state || state->osd_pid <= 0 || !jw__osd_enabled(state) ||
        state->direct_drm_active || state->shutdown_requested || g_shutdown_requested) {
        return;
    }
    jw_appearance_env appearance;
    jw_appearance_resolve_settings(state->db_path, &appearance);
    const char *font_path = appearance.font_path ? appearance.font_path : "";
    if (strcmp(appearance.language, state->osd_language) == 0 &&
        strcmp(font_path, state->osd_font_path) == 0 &&
        strcmp(appearance.font_bump, state->osd_font_bump) == 0) {
        return;
    }
    jw_log_info("osd: appearance changed (%s); restarting jawaka-osd pid=%d",
                why ? why : "refresh", (int)state->osd_pid);
    jw_osd_client_prompt_lost(&state->pico8_exit_confirm_until_ms);
    if (kill(state->osd_pid, SIGTERM) != 0 && errno != ESRCH) {
        jw_log_warn("osd: could not restart pid=%d: %s", (int)state->osd_pid, strerror(errno));
    }
}

/* Every OSD request below replaces or removes whatever the OSD shows, so each
   one disarms the PICO-8 exit confirmation before it is sent -- including
   when the OSD is disabled or the request times out. */
static int jw__osd_show_brightness(jw_daemon_state *state, int percent) {
    if (!state) {
        return -1;
    }
    if (!jw__osd_enabled(state)) {
        jw_osd_client_prompt_lost(&state->pico8_exit_confirm_until_ms);
        return -1;
    }
    jw_osd_client client = jw__osd_client(state);
    if (jw_osd_client_show_level(&client, &state->pico8_exit_confirm_until_ms,
                                 "show-brightness", percent, 30000) != 0) {
        jw_log_warn("osd brightness request failed");
        return -1;
    }
    return 0;
}

static int jw__osd_game_launch(jw_daemon_state *state, const char *stage,
                               int pending_items) {
    if (!state || !stage) {
        return -1;
    }
    if (!jw__osd_enabled(state)) {
        jw_osd_client_prompt_lost(&state->pico8_exit_confirm_until_ms);
        return -1;
    }
    jw_log_info("life1: launch status stage=%s pending_items=%d",
                stage, pending_items < 0 ? 0 : pending_items);
    /* A visual hint must never extend a service's LIFE-1 acknowledgement
     * budget. The OSD is best-effort and already supervised separately. */
    jw_osd_client client = jw__osd_client(state);
    int rc = jw_osd_client_show_stage(&client, &state->pico8_exit_confirm_until_ms,
                                      stage, pending_items);
    if (rc != 0) {
        jw_log_warn("life1: launch OSD request failed stage=%s",
                    stage);
    }
    return rc;
}

/* The RetroArch app tile exited without its settings reaching the durable
   shared config. The user changed something and it is gone; say so on screen
   rather than only in the log, which is the silent-loss behavior Leaf#48
   reported. Best-effort and self-dismissing: nothing waits on this. */
static void jw__osd_settings_not_saved(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    if (!jw__osd_enabled(state)) {
        jw_osd_client_prompt_lost(&state->pico8_exit_confirm_until_ms);
        return;
    }
    jw_osd_client client = jw__osd_client(state);
    if (jw_osd_client_show_stage(&client, &state->pico8_exit_confirm_until_ms,
                                 "settings-not-saved", 0) != 0) {
        jw_log_warn("retroarch app: settings-not-saved OSD request failed");
    }
}

static void jw__osd_game_launch_hide(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    jw_osd_client client = jw__osd_client(state);
    (void)jw_osd_client_hide(&client, &state->pico8_exit_confirm_until_ms);
}

static void jw__persist_brightness(jw_daemon_state *state, int percent) {
    if (!state || !state->db_path) {
        return;
    }

    char value[16];
    snprintf(value, sizeof(value), "%d", percent);
    if (jw_db_set_setting(state->db_path, "platform.brightness_percent", value) != 0) {
        jw_log_warn("could not persist brightness setting");
    }
}

static int jw__set_brightness(jw_daemon_state *state, int percent,
                              bool persist, bool show_osd,
                              jw_platform_result *out) {
    if (!state) {
        if (out) {
            out->code = JW_PLATFORM_RESULT_INVALID;
            snprintf(out->message, sizeof(out->message), "%s", "daemon state missing");
            out->has_value = false;
            out->value = 0;
        }
        return -1;
    }

    int clamped = jw_platform_clamp_brightness_percent(percent);
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SET_BRIGHTNESS,
                               clamped, out);
    if (!out || out->code != JW_PLATFORM_RESULT_OK) {
        return -1;
    }

    int resolved = out->has_value ? out->value : clamped;
    state->cached_brightness_percent = jw_platform_clamp_brightness_percent(resolved);
    if (persist) {
        jw__persist_brightness(state, resolved);
    }
    if (show_osd) {
        jw__osd_show_brightness(state, resolved);
    }
    return 0;
}

static void jw__apply_persisted_brightness(jw_daemon_state *state) {
    char value[32];
    if (!state || !state->db_path ||
        jw_db_get_setting(state->db_path, "platform.brightness_percent",
                          value, sizeof(value)) != 0 ||
        !value[0]) {
        return;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        jw_log_warn("ignoring invalid persisted brightness: %s", value);
        return;
    }

    jw_platform_result result;
    if (jw__set_brightness(state, (int)parsed, false, false, &result) == 0) {
        jw_log_info("applied persisted brightness value=%d", result.value);
    } else {
        jw_log_warn("persisted brightness apply failed: %s", result.message);
    }
}

/* The color-temperature LUT lives in the CRTC, not in any config file, so unlike
   the refresh-rate weston.ini override it has to be replayed at startup (a cold
   power cycle clears it; see the color temperature section in
   device_mlp1.c), and swapped whenever HDMI hands the CRTC to or from the
   panel (jw__schedule_lut_sync). Key is "color_temp_k", written by the settings
   UI; absent means "never set" -> leave the panel at its native white point.

   With `external` set the external display is the active output, and the request
   is for neutral instead (an identity ramp, so the panel's correction does not
   tint it; the persisted value is left alone).

   Returns true when there is nothing more to do (applied, nothing persisted, an
   invalid value, or a platform without the feature) and false when the write
   failed, so a caller that can retry knows it may. */
static bool jw__apply_color_temp(jw_daemon_state *state, bool external) {
    char value[32];
    long parsed = JW_PLATFORM_COLOR_TEMP_NEUTRAL_K;
    if (!external) {
        if (!state || !state->db_path ||
            jw_db_get_setting(state->db_path, "color_temp_k",
                              value, sizeof(value)) != 0 ||
            !value[0]) {
            return true;
        }

        char *end = NULL;
        parsed = strtol(value, &end, 10);
        if (end == value || (end && *end != '\0')) {
            jw_log_warn("ignoring invalid persisted color temperature: %s", value);
            return true;
        }
    } else if (!state) {
        return true;
    }

    jw_platform_result result;
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SET_COLOR_TEMP,
                               (int)parsed, &result);
    if (result.code == JW_PLATFORM_RESULT_OK) {
        jw_log_info("applied color temperature value=%ld K (%s)", parsed,
                    external ? "external display" : "panel");
        return true;
    }
    if (result.code == JW_PLATFORM_RESULT_UNSUPPORTED) {
        return true;
    }
    jw_log_warn("color temperature apply failed: %s", result.message);
    return false;
}

/* The color-temperature LUT is swapped for the current output this long after the
   last HDMI event, and retried for JW_LUT_SYNC_WINDOW_MS if the display has no
   valid mode yet. */
#define JW_LUT_SETTLE_MS 2000
#define JW_LUT_SYNC_WINDOW_MS 15000

/* Swap the color-temperature LUT for the current output (external display or
   panel) from the HDMI poll, once no HDMI event has happened for
   JW_LUT_SETTLE_MS. It is not done inside the HDMI switch itself, so it does not
   race the mode change (see jw__mlp1_set_hdmi_output). Every new HDMI event
   restarts the wait. */
static void jw__schedule_lut_sync(jw_daemon_state *state, bool external) {
    if (!state) {
        return;
    }
    long long now = jw__monotonic_ms();
    state->lut_sync_external = external;
    state->lut_sync_not_before_ms = now + JW_LUT_SETTLE_MS;
    state->lut_sync_deadline_ms = state->lut_sync_not_before_ms + JW_LUT_SYNC_WINDOW_MS;
}

static void jw__persist_volume(jw_daemon_state *state, int percent) {
    if (!state || !state->db_path) {
        return;
    }

    char value[16];
    snprintf(value, sizeof(value), "%d", percent);
    if (jw_db_set_setting(state->db_path, "platform.volume_percent", value) != 0) {
        jw_log_warn("could not persist volume setting");
    }
}

static void jw__apply_persisted_volume(jw_daemon_state *state) {
    char value[32];
    if (!state || !state->db_path ||
        jw_db_get_setting(state->db_path, "platform.volume_percent",
                          value, sizeof(value)) != 0 ||
        !value[0]) {
        return;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        jw_log_warn("ignoring invalid persisted volume: %s", value);
        return;
    }

    int percent = (int)parsed;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    jw_platform_result result;
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SET_VOLUME,
                               percent, &result);
    if (result.code == JW_PLATFORM_RESULT_OK) {
        int resolved = result.has_value ? result.value : percent;
        if (resolved < 0) resolved = 0;
        if (resolved > 100) resolved = 100;
        state->cached_volume_percent = resolved;
        jw__notify_launcher_levels(state);
        jw_log_info("applied persisted volume value=%d", resolved);
    } else {
        jw_log_warn("persisted volume apply failed: %s", result.message);
    }
}

static void jw__persist_led(jw_daemon_state *state, const jw_led_config *led) {
    if (!state || !state->db_path[0] || !led) {
        return;
    }
    char vr[8], vg[8], vb[8], vbr[8], vsp[8];
    snprintf(vr,  sizeof(vr),  "%u", (unsigned)led->r);
    snprintf(vg,  sizeof(vg),  "%u", (unsigned)led->g);
    snprintf(vb,  sizeof(vb),  "%u", (unsigned)led->b);
    snprintf(vbr, sizeof(vbr), "%d", led->brightness);
    snprintf(vsp, sizeof(vsp), "%d", led->speed);
    const char *keys[] = {
        "platform.led_enabled", "platform.led_mode", "platform.led_r",
        "platform.led_g", "platform.led_b", "platform.led_brightness",
        "platform.led_speed",
    };
    const char *vals[] = {
        led->enabled ? "1" : "0", jw_led_mode_name(led->mode), vr, vg, vb, vbr, vsp,
    };
    if (jw_db_set_settings(state->db_path, keys, vals,
                           (int)(sizeof(keys) / sizeof(keys[0]))) != 0) {
        jw_log_warn("could not persist led settings");
    }
}

static void jw__stop_ledd(jw_daemon_state *state) {
    if (!state || state->ledd_pid <= 0) return;
    kill(state->ledd_pid, SIGTERM);   /* ledd's handler restores platform LED ownership */
    waitpid(state->ledd_pid, NULL, 0);
    state->ledd_pid = -1;
}

static int jw__spawn_ledd(jw_daemon_state *state, const char *effect,
                          int r, int g, int b, int brightness, int speed) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/jawaka-ledd", state->bin_dir) >= (int)sizeof(path)) {
        jw_log_warn("ledd binary path too long");
        return -1;
    }
    if (!jw__path_exists(path)) {
        jw_log_warn("ledd binary missing: %s", path);
        return -1;
    }
    char sr[8], sg[8], sb[8], sbr[8], ssp[8];
    snprintf(sr,  sizeof(sr),  "%d", r);
    snprintf(sg,  sizeof(sg),  "%d", g);
    snprintf(sb,  sizeof(sb),  "%d", b);
    snprintf(sbr, sizeof(sbr), "%d", brightness);
    snprintf(ssp, sizeof(ssp), "%d", speed);
    char *const argv[] = { path, (char *)effect, sr, sg, sb, sbr, ssp, NULL };
    pid_t pid = jw_ledd_spawn(path, argv);
    if (pid < 0) { jw_log_warn("ledd fork failed: %s", strerror(errno)); return -1; }
    state->ledd_pid = pid;
    state->ledd_started_ms = jw__monotonic_ms();
    jw_log_info("spawned jawaka-ledd %s pid=%d", effect, (int)pid);
    return 0;
}

static const char *jw__leaf_ledd_effect_name(const jw_led_config *led) {
    if (!led || !jw__env_is_truthy("UMRK_LEAF_MODE")) return NULL;
    if (!led->enabled) return "off";

    switch (led->mode) {
        case JW_LED_MODE_STATIC:  return "static";
        case JW_LED_MODE_BREATH:  return "breath";
        case JW_LED_MODE_RAINBOW: return "rainbow";
        default:
            return jw_led_mode_is_effect(led->mode) ? jw_led_mode_name(led->mode) : NULL;
    }
}

/* Start the effect engine when the config needs it: every mode in Leaf mode,
   only the custom effects otherwise. */
static void jw__spawn_ledd_for(jw_daemon_state *state, const jw_led_config *led) {
    const char *effect = jw__leaf_ledd_effect_name(led);
    if (!effect && led->enabled && jw_led_mode_is_effect(led->mode)) {
        effect = jw_led_mode_name(led->mode);
    }
    if (effect) {
        jw__spawn_ledd(state, effect,
                       led->r, led->g, led->b, led->brightness, led->speed);
    }
}

/* Apply an LED config: stop any running effect, write the platform baseline
   config, then spawn the custom effect engine when the selected mode needs it. */
static void jw__apply_led_config(jw_daemon_state *state, const jw_led_config *led) {
    if (!state || !led) return;
    jw__stop_ledd(state);

    jw_led_config base = *led;
    if (jw_led_mode_is_effect(base.mode)) base.mode = JW_LED_MODE_STATIC;
    jw_platform_result result;
    jw_platform_set_led(&state->platform, &base, &result);

    /* A new setting gets a fresh helper, so earlier quick exits stop counting. */
    jw_ledd_restarts_reset(&state->ledd_restarts);
    jw__spawn_ledd_for(state, led);
    state->cached_led = *led;
    state->led_configured = true;
}

static void jw__apply_persisted_led(jw_daemon_state *state) {
    if (!state || !state->db_path[0]) return;

    char value[32];
    jw_led_config led;
    memset(&led, 0, sizeof(led));

    if (jw_db_get_setting(state->db_path, "platform.led_mode", value, sizeof(value)) == 0 && value[0]) {
        /* Restore the user's saved LED config. */
        led.mode = JW_LED_MODE_STATIC;
        jw_led_mode_parse(value, &led.mode);
        led.brightness = 5;
        led.speed = 5;
        if (jw_db_get_setting(state->db_path, "platform.led_enabled", value, sizeof(value)) == 0)
            led.enabled = (strcmp(value, "0") != 0);
        if (jw_db_get_setting(state->db_path, "platform.led_r", value, sizeof(value)) == 0) led.r = (unsigned char)atoi(value);
        if (jw_db_get_setting(state->db_path, "platform.led_g", value, sizeof(value)) == 0) led.g = (unsigned char)atoi(value);
        if (jw_db_get_setting(state->db_path, "platform.led_b", value, sizeof(value)) == 0) led.b = (unsigned char)atoi(value);
        if (jw_db_get_setting(state->db_path, "platform.led_brightness", value, sizeof(value)) == 0) led.brightness = atoi(value);
        if (jw_db_get_setting(state->db_path, "platform.led_speed", value, sizeof(value)) == 0) led.speed = atoi(value);
    } else {
        /* Fresh install → a calm, subdued-green breath. Matches the boot logo's
           breathing finish, so the LED stays consistent from boot into the
           launcher. Persist it so it becomes the user's setting and shows in
           Lighting. */
        led.enabled = true;
        led.mode = JW_LED_MODE_BREATH;
        led.r = 0x0B; led.g = 0x28; led.b = 0x00;   /* #0B2800 — subdued green */
        led.brightness = 1;
        led.speed = 10;
        jw__persist_led(state, &led);
    }

    jw__apply_led_config(state, &led);
    jw_log_info("applied led mode=%s enabled=%d",
                jw_led_mode_name(led.mode), led.enabled);
}

static int jw__osd_show_volume(jw_daemon_state *state, int percent) {
    if (!state) {
        return -1;
    }
    if (!jw__osd_enabled(state)) {
        jw_osd_client_prompt_lost(&state->pico8_exit_confirm_until_ms);
        return -1;
    }
    jw_osd_client client = jw__osd_client(state);
    if (jw_osd_client_show_level(&client, &state->pico8_exit_confirm_until_ms,
                                 "show-volume", percent, 30000) != 0) {
        jw_log_warn("osd volume request failed");
        return -1;
    }
    return 0;
}

static void jw__input_volume_delta(void *userdata, int delta_percent) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;
    if (!state) {
        return;
    }

    if (state->cached_volume_percent < 0) {
        jw__refresh_platform_cache(state);
    }
    int current = state->cached_volume_percent >= 0 ? state->cached_volume_percent : 50;

    int target = current + delta_percent;
    if (target < 0) target = 0;
    if (target > 100) target = 100;

    jw_platform_result result;
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SET_VOLUME,
                               target, &result);
    if (result.code == JW_PLATFORM_RESULT_OK) {
        int resolved = result.has_value ? result.value : target;
        if (resolved < 0) resolved = 0;
        if (resolved > 100) resolved = 100;
        state->cached_volume_percent = resolved;
        jw__persist_volume(state, resolved);
        jw__notify_launcher_levels(state);
        /* No OSD over a kmsdrm standalone emulator: the Wayland overlay can
           only steal one stray frame from the emulator's page flips. The
           audible change is the feedback. */
        if (!jw__has_standalone_session(state)) {
            jw__osd_show_volume(state, resolved);
        }
        jw_log_info("volume hotkey delta=%d value=%d", delta_percent, resolved);
    } else {
        jw_log_warn("volume hotkey failed: %s", result.message);
    }
}

static void jw__input_brightness_delta(void *userdata, int delta_percent) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;
    if (!state) {
        return;
    }

    if (state->cached_brightness_percent < 0) {
        jw__refresh_platform_cache(state);
    }
    int current = state->cached_brightness_percent >= 0
                    ? state->cached_brightness_percent
                    : 50;

    jw_platform_result result;
    bool show_osd = !jw__has_standalone_session(state);
    if (jw__set_brightness(state, current + delta_percent, true, show_osd, &result) == 0) {
        jw_log_info("brightness hotkey delta=%d value=%d",
                    delta_percent, result.has_value ? result.value : current + delta_percent);
    } else {
        jw_log_warn("brightness hotkey failed: %s", result.message);
    }
}

static bool jw__standalone_quit(jw_daemon_state *state, bool escalate) {
    pid_t pid = state->retroarch_session.pid;
    long long now = jw__monotonic_ms();
    if (state->standalone_quit_request_ms == 0) {
        jw_log_info("menu tap: quitting standalone emulator pid=%d", (int)pid);
        int rc = state->child_pid == pid
                     ? jw__signal_tracked_game_group(state, SIGTERM)
                     : kill(pid, SIGTERM);
        if (rc != 0 && errno != ESRCH) {
            jw_log_warn("standalone quit: SIGTERM failed pid=%d: %s",
                        (int)pid, strerror(errno));
        }
        state->standalone_quit_request_ms = now;
    } else if (escalate && now - state->standalone_quit_request_ms >= 2000) {
        jw_log_warn("standalone emulator ignored quit; forcing pid=%d", (int)pid);
        int rc = state->child_pid == pid
                     ? jw__signal_tracked_game_group(state, SIGKILL)
                     : kill(pid, SIGKILL);
        if (rc != 0 && errno != ESRCH) {
            jw_log_warn("standalone quit: SIGKILL failed pid=%d: %s",
                        (int)pid, strerror(errno));
        }
    }
    return true;
}

/* A content pak's path core without a menu of its own ends on Menu, and ending
   it loses anything the player has not saved. Ask first, with the prompt native
   PICO-8 uses: the first tap shows "Return to Leaf?" and only a second tap
   inside its window quits. The prompt is armed only while it is on screen. If
   the OSD cannot show it, quit as before rather than turn Menu into a dead
   button. */
static bool jw__confirm_content_quit(jw_daemon_state *state, bool escalate) {
    pid_t pid = state->retroarch_session.pid;
    jw_osd_client client = jw__osd_client(state);
    jw_osd_pico8_menu confirm =
        jw_osd_client_pico8_menu(&client, &state->pico8_exit_confirm_until_ms);
    if (confirm == JW_OSD_PICO8_MENU_ARMED) {
        jw_log_info("menu tap: asking before quitting content-pak core id=%s pid=%d",
                    state->retroarch_session.core_id, (int)pid);
        return true;
    }
    if (confirm == JW_OSD_PICO8_MENU_CONFIRMED) {
        jw_log_info("menu tap: quit confirmed for content-pak core id=%s pid=%d",
                    state->retroarch_session.core_id, (int)pid);
    } else {
        jw_log_warn("menu tap: exit prompt unavailable; quitting content-pak core id=%s pid=%d",
                    state->retroarch_session.core_id, (int)pid);
    }
    return jw__standalone_quit(state, escalate);
}

static bool jw__input_menu_tap(void *userdata) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;

    /* During LIFE-1's opt-in waiting phase the launcher is already gone and
       the daemon owns the pad. The waiting OSD labels this exact Menu action;
       cancellation still leaves each subscriber its bounded ack window. */
    if (jw__game_coordination_start_now(state, "start-now-menu")) {
        return true;
    }

    if (state && state->child_pid > 0 &&
        (state->child_kind == JW_CHILD_PICO8_APP ||
         (jw__has_standalone_session(state) &&
          strcmp(state->retroarch_session.core_id, JW_PICO8_CORE) == 0))) {
        /* The confirmation is armed only while its prompt is shown; a prompt
           the OSD could not show leaves it disarmed. */
        jw_osd_client client = jw__osd_client(state);
        if (jw_osd_client_pico8_menu(&client, &state->pico8_exit_confirm_until_ms) ==
            JW_OSD_PICO8_MENU_CONFIRMED) {
            /* SDL turns SIGTERM into its normal quit event. Signal the native
               leader so it can save config and CARTDATA, then let the existing
               group barrier supervise any remaining downloader children. */
            int signal = jw_pico8_exit_signal(&state->pico8_quit_request_ms,
                                              jw__monotonic_ms());
            if (signal) {
                jw_log_info("PICO-8: confirmed MENU exit pid=%d signal=%d",
                            (int)state->child_pid, signal);
                int rc = signal == SIGKILL ? jw__signal_tracked_game_group(state, signal)
                                          : kill(state->child_pid, signal);
                if (rc != 0 && errno != ESRCH)
                    jw_log_warn("PICO-8 quit request failed: %s", strerror(errno));
            }
        }
        return true;
    }

    /* Standalone emulators own the display, so Jawaka's overlay menu cannot
       appear above them. PPSSPP has a patched SIGUSR2 pause-menu hook. DraStic,
       Fun DraStic, Flycast, and YabaSanshiro have native menu bindings, so let
       Menu reach the emulator.
       Standalone emulators without a menu hook keep Menu as the exit key. */
    if (jw__has_standalone_session(state)) {
        pid_t pid = state->retroarch_session.pid;
        switch (jw_standalone_policy_menu(&state->retroarch_session.standalone_policy,
                                          state->retroarch_session.supports_menu, false)) {
            case JW_STANDALONE_MENU_FORWARD: return false;
            case JW_STANDALONE_MENU_CONFIRM_QUIT: return jw__confirm_content_quit(state, false);
            default: break;
        }
        if (jw__standalone_session_is_ppsspp(state)) {
            state->standalone_quit_request_ms = 0;
            jw_log_info("menu tap: opening PPSSPP pause menu pid=%d", (int)pid);
            if (kill(pid, SIGUSR2) != 0 && errno != ESRCH) {
                jw_log_warn("PPSSPP menu: SIGUSR2 failed pid=%d: %s",
                            (int)pid, strerror(errno));
            }
            return true;
        }
        if (jw__standalone_session_is_drastic(state)) {
            state->standalone_quit_request_ms = 0;
            jw_log_info("menu tap: forwarding to DraStic native menu pid=%d", (int)pid);
            return false;
        }
        if (jw__standalone_session_is_fun_drastic(state)) {
            state->standalone_quit_request_ms = 0;
            jw_log_info("menu tap: forwarding to Fun DraStic native menu pid=%d", (int)pid);
            return false;
        }
        if (jw__standalone_session_is_mupen64plus(state)) {
            state->standalone_quit_request_ms = 0;
            jw_log_info("menu tap: forwarding to Mupen64Plus embedded menu pid=%d", (int)pid);
            return false;
        }
        if (jw__standalone_session_is_flycast(state)) {
            state->standalone_quit_request_ms = 0;
            jw_log_info("menu tap: forwarding to Flycast native menu pid=%d", (int)pid);
            return false;
        }
        if (jw__standalone_session_is_yabasanshiro(state)) {
            state->standalone_quit_request_ms = 0;
            jw_log_info("menu tap: forwarding to YabaSanshiro native menu pid=%d", (int)pid);
            return false;
        }

        return jw__standalone_quit(state, true);
    }

    if (!jw__has_retroarch_session(state)) {
        return false;
    }

    /* A frontend without Leaf's command set gets its own menu instead, through
       the upstream MENU_TOGGLE; a second tap closes it the same way. Leaf's
       config leaves RetroArch's menu button unbound, so the forwarded tap alone
       would do nothing. */
    if (state->retroarch_session.frontend && state->retroarch_session.frontend_probe <= 0) {
        jw_ra_client client = jw_ra_client_default();
        jw_ra_result result = jw_ra_menu_toggle(&client);
        jw_log_info("menu tap: RetroArch frontend without Leaf commands, MENU_TOGGLE result=%s",
                    jw_ra_result_string(result));
        return true;
    }

    if (state->advanced_shader_pending) {
        jw_ra_client client = jw_ra_client_default();
        jw_ra_status status;
        jw_ra_result result = jw_ra_get_status(&client, &status);
        if (result == JW_RA_OK && status.state == JW_RA_STATE_MENU) {
            result = jw_ra_menu_toggle(&client);
            jw_log_info("menu tap: closing RetroArch shader menu result=%s",
                        jw_ra_result_string(result));
        } else {
            jw_log_info("menu tap: waiting for Advanced shader handoff state=%s result=%s",
                        result == JW_RA_OK
                            ? jw_ra_play_state_string(status.state) : "unknown",
                        jw_ra_result_string(result));
        }
        return true;
    }

    /* RetroArch's bind-all listener must be able to see the dedicated Menu
       button. When its native menu is already foreground, pass the deferred
       tap through instead of stacking Leaf's menu over it. The direct native
       menu binding is nul by default, so an ordinary tap is otherwise inert;
       a binding prompt consumes it as button 5. Advanced shader handoff stays
       above this branch because Menu deliberately closes that flow. */
    jw_ra_client client = jw_ra_client_default();
    jw_ra_status status;
    if (jw_ra_get_status(&client, &status) == JW_RA_OK &&
        status.state == JW_RA_STATE_MENU) {
        jw_log_info("menu tap: forwarding to foreground RetroArch menu");
        return false;
    }

    /* Menu toggles: tap to open, tap again to close. */
    if (state->menu_visible) {
        jw__request_close_in_game_menu(state);
        return true;
    }

    if (jw__request_open_in_game_menu(state) != 0) {
        jw_log_warn("in-game menu: open request failed, forwarding Menu tap");
        return false;
    }

    return true;
}

/* External Guide is ungrabbed. "Handled" prevents virtual synthesis only;
   the child still receives the raw external press and release. */
static bool jw__external_menu_tap(void *userdata) {
    jw_daemon_state *state = userdata;
    if (jw__game_coordination_start_now(state, "start-now-menu")) return true;
    if (jw__has_standalone_session(state) &&
        strcmp(state->retroarch_session.core_id, JW_PICO8_CORE) != 0) {
        switch (jw_standalone_policy_menu(&state->retroarch_session.standalone_policy,
                                          state->retroarch_session.supports_menu, true)) {
            case JW_STANDALONE_MENU_EXTERNAL_HANDLED: return true;
            case JW_STANDALONE_MENU_CONFIRM_QUIT: return jw__confirm_content_quit(state, true);
            default: break;
        }
    }
    return jw__input_menu_tap(userdata);
}

static bool jw__content_menu_running(jw_daemon_state *state) {
    if (!jw__has_standalone_session(state) || state->shutdown_requested ||
        state->child_pid != state->retroarch_session.pid ||
        !state->session_menu_config.escape_enabled) return false;
    /* Do not signal surviving group members after the originating child exits,
       even when it exited between the loop's reap check and this input tick. */
    siginfo_t info = {0};
    return waitid(P_PID, state->child_pid, &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
           info.si_pid == 0;
}

static void jw__input_menu_escape(void *userdata, uint64_t hold, bool threshold) {
    jw_daemon_state *state = userdata;
    if (!threshold) {
        jw_menu_escape_end(&state->menu_escape, hold);
        return;
    }
    jw_menu_escape_cancel(&state->menu_escape);
    if (!jw__content_menu_running(state)) return;
    if (jw__signal_tracked_game_group(state, SIGTERM) == 0) {
        jw_menu_escape_sent(&state->menu_escape, state->menu_session_generation,
                             hold, (uint64_t)jw__monotonic_ms());
        jw_log_info("Menu escape: SIGTERM session=%llu hold=%llu pid=%d time_ms=%llu",
                    (unsigned long long)state->menu_session_generation,
                    (unsigned long long)hold, (int)state->child_pid,
                    (unsigned long long)state->menu_escape.term_ms);
    } else if (errno != ESRCH) {
        jw_log_warn("Menu escape: SIGTERM failed: %s", strerror(errno));
    }
}

static void jw__tick_menu_escape(jw_daemon_state *state) {
    if (!state->menu_escape.pending) return;
    uint64_t now = (uint64_t)jw__monotonic_ms();
    if (jw_menu_escape_kill(&state->menu_escape, state->menu_session_generation,
                            jw__content_menu_running(state),
                            now)) {
        jw_log_warn("Menu escape: SIGKILL session=%llu pid=%d time_ms=%llu",
                    (unsigned long long)state->menu_session_generation,
                    (int)state->child_pid, (unsigned long long)now);
        if (jw__signal_tracked_game_group(state, SIGKILL) != 0 && errno != ESRCH)
            jw_log_warn("Menu escape: SIGKILL failed: %s", strerror(errno));
    }
}

/* MLP1 spike: observed Menu+Volume intervals up to 1887 ms. Three seconds
   leaves over one second of margin; see the content-pak path-core plan. */
#define JW_CONTENT_MENU_ESCAPE_MS 3000u

static void jw__set_session_menu(jw_daemon_state *state, bool provider_bound) {
    if (!state) return;
    bool reset = provider_bound || state->session_menu_config.tap_only;
    jw_menu_escape_cancel(&state->menu_escape);
    state->menu_session_generation++;
    state->session_menu_config = (jw_input_menu_config){
        .tap_only = provider_bound,
        .escape_enabled = provider_bound,
        .escape_ms = provider_bound ? JW_CONTENT_MENU_ESCAPE_MS : 0,
    };
    if (reset)
        jw_input_proxy_configure_menu(&state->input_proxy, state->session_menu_config);
}

/* Menu + Select chord (input proxy): open the reversible in-game switcher
   overlay. Returns false when there is no game to switch in, so the proxy
   forwards the events normally. Opening only pauses + overlays — it never saves
   or quits. */
static bool jw__input_game_switcher(void *userdata) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;
    if (!jw__has_retroarch_session(state)) {
        return false;
    }

    /* A surface is already up: swallow the chord rather than stacking. */
    if (state->menu_visible) {
        return true;
    }

    if (jw__request_open_in_game_switcher(state) != 0) {
        jw_log_warn("in-game switcher: open request failed, forwarding chord");
        return false;
    }

    return true;
}

/* ─── Screenshot hotkey (Menu + L1) ─────────────────────────────────────────
   Captured on-device so it works with Wi-Fi off, in-game, and in 5-Game Mode.
   UI/focus (Weston up): kmsgrab the composited CRTC scanout and encode. In-game
   (Weston stopped, RetroArch is DRM master): phase 2, via the RA SCREENSHOT
   command. Capture runs on a detached worker so the input tick never blocks.
   See umrk-workspace/plans/Jawaka/on-device-screenshots.md. */
#define JW_SS_SCANOUT_W   720   /* panel native (portrait); UI runs landscape */
#define JW_SS_SCANOUT_H   960
#define JW_SS_THROTTLE_MS 1000
#define JW_RECORD_THROTTLE_MS 250 /* a toggle needs bounce protection, not a full second */
#define JW_SS_PRE_MAX     64      /* max pre-existing PNGs tracked for the in-game diff */

typedef struct {
    char  sdcard_root[PATH_MAX];
    char  db_path[PATH_MAX];
    char  shots_dir[PATH_MAX];   /* RA's tmpfs screenshot dir (in-game only) */
    char  rom_name[128];         /* sanitized running-game name (in-game only) */
    bool  in_game;     /* RA session running -> use the RA SCREENSHOT command */
} jw__screenshot_job;

/* Set by a UI-capture worker when its PNG is written; the main loop drains it and
   flashes the launcher (revalidating that the launcher is still foreground, so a
   stale/reused pid is never signalled). */
static atomic_int g_ss_flash_request;

static uint64_t jw__screenshot_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Ensure <sdcard>/Screenshots exists and write its path into out. */
static bool jw__screenshots_dir(const char *sdcard_root, char *out, size_t out_size) {
    if (!sdcard_root || !sdcard_root[0]) {
        return false;
    }
    if (snprintf(out, out_size, "%s/Screenshots", sdcard_root) >= (int)out_size) {
        return false;
    }
    if (mkdir(out, 0777) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

/* Grab the composited CRTC scanout via kmsgrab, convert XR24 (BGRX) -> RGB, and
   rotate 90 CW (the panel is mounted portrait). Returns a malloc'd RGB buffer of
   960x720 (caller frees) and sets ow + oh, or NULL on failure. */
static uint8_t *jw__kmsgrab_rgb(int *ow, int *oh) {
    int crtc = 85;   /* the usual MLP1 CRTC; rediscovered below if it differs */
    FILE *disc = popen("/usr/bin/kmsgrab 2>&1 >/dev/null", "r");
    if (disc) {
        char line[256];
        while (fgets(line, sizeof(line), disc)) {
            char *p = strstr(line, "Valid crtcs:");
            if (p) {
                int v;
                if (sscanf(p + 12, "%*[^0-9]%d", &v) == 1 || sscanf(p + 12, "%d", &v) == 1) {
                    crtc = v;
                }
                break;
            }
        }
        pclose(disc);
    }

    const size_t need = (size_t)JW_SS_SCANOUT_W * JW_SS_SCANOUT_H * 4u;
    uint8_t *raw = (uint8_t *)malloc(need);
    if (!raw) {
        return NULL;
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "/usr/bin/kmsgrab --crtc %d 2>/dev/null", crtc);
    FILE *cap = popen(cmd, "r");
    if (!cap) {
        free(raw);
        return NULL;
    }
    size_t got = 0, n;
    while (got < need && (n = fread(raw + got, 1, need - got, cap)) > 0) {
        got += n;
    }
    pclose(cap);
    if (got < need) {
        free(raw);
        return NULL;
    }

    const int W = JW_SS_SCANOUT_W, H = JW_SS_SCANOUT_H;
    const int OW = H;   /* rotated width = 960 */
    uint8_t *dst = (uint8_t *)malloc((size_t)W * H * 3u);
    if (!dst) {
        free(raw);
        return NULL;
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint8_t *px = raw + ((size_t)y * W + x) * 4u;   /* B,G,R,X */
            int nx = H - 1 - y, ny = x;                            /* 90 CW */
            uint8_t *o = dst + ((size_t)ny * OW + nx) * 3u;
            o[0] = px[2];   /* R */
            o[1] = px[1];   /* G */
            o[2] = px[0];   /* B */
        }
    }
    free(raw);
    *ow = OW;   /* 960 */
    *oh = W;    /* 720 */
    return dst;
}

static bool jw__screenshot_write_png(const char *path, const uint8_t *rgb, int w, int h) {
    size_t len = 0;
    void *png = tdefl_write_image_to_png_file_in_memory(rgb, w, h, 3, &len);
    if (!png) {
        return false;
    }
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(png, 1, len, f) == len;
    if (f && fclose(f) != 0) {
        ok = false;
    }
    mz_free(png);
    return ok;
}

/* basename(rom_path) minus its extension, with FAT-illegal characters replaced
   by '_', so it can be used in a screenshot filename. */
static void jw__screenshot_rom_name(const char *rom_path, char *out, size_t out_size) {
    if (out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!rom_path || !rom_path[0]) {
        return;
    }
    const char *base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    snprintf(out, out_size, "%.*s", (int)(out_size - 1), base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) {
        *dot = '\0';   /* drop the extension */
    }
    for (char *p = out; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) {
            *p = '_';
        }
    }
    if (!out[0]) {
        snprintf(out, out_size, "screenshot");
    }
}

static bool jw__screenshot_copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return false;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[65536];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) {
        ok = false;
    }
    fclose(in);
    if (fclose(out) != 0) {
        ok = false;
    }
    return ok;
}

static void *jw__screenshot_worker(void *arg) {
    jw__screenshot_job *job = (jw__screenshot_job *)arg;
    char dir[PATH_MAX];
    if (!jw__screenshots_dir(job->sdcard_root, dir, sizeof(dir))) {
        jw_log_warn("screenshot: cannot create %s/Screenshots", job->sdcard_root);
        free(job);
        return NULL;
    }

    if (job->in_game) {
        /* In-game: RetroArch owns the display (Weston is stopped), so ask RA to
           screenshot its own frame. It writes async into its tmpfs shots dir (the
           same one the pause-menu background uses) — poll for the new PNG and copy
           it out, named by ROM. RA shows its own "saved" toast. */
        if (!job->shots_dir[0]) {
            jw_log_warn("screenshot: in-game shots dir unknown");
            free(job);
            return NULL;
        }
        /* Snapshot the PNGs already in the dir so we only accept the file RA
           writes for THIS command — the pause-menu background writes here too, and
           1-second mtime granularity can't distinguish one from an existing frame. */
        char pre[JW_SS_PRE_MAX][256];   /* per-worker (concurrent captures allowed) */
        int pre_n = 0;
        {
            DIR *d = opendir(job->shots_dir);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)) != NULL && pre_n < JW_SS_PRE_MAX) {
                    const char *dot = strrchr(e->d_name, '.');
                    if (dot && strcasecmp(dot, ".png") == 0) {
                        snprintf(pre[pre_n++], sizeof(pre[0]), "%s", e->d_name);
                    }
                }
                closedir(d);
            }
        }

        jw_ra_client ra = jw_ra_client_default();
        if (jw_ra_screenshot(&ra) != JW_RA_OK) {
            jw_log_warn("screenshot: RA SCREENSHOT command failed");
            free(job);
            return NULL;
        }

        char newest[PATH_MAX] = "";
        off_t prev_size = -1;
        for (int tries = 0; tries < 30; tries++) {   /* up to ~3s */
            jw__usleep(100000);   /* 100ms */
            /* newest PNG that was NOT already present, with a non-zero size */
            time_t best_mt = 0;
            char  cand[PATH_MAX] = "";
            off_t cand_size = 0;
            DIR *d = opendir(job->shots_dir);
            if (!d) {
                continue;
            }
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                const char *dot = strrchr(e->d_name, '.');
                if (!dot || strcasecmp(dot, ".png") != 0) {
                    continue;
                }
                bool preexisting = false;
                for (int i = 0; i < pre_n; i++) {
                    if (strcmp(pre[i], e->d_name) == 0) {
                        preexisting = true;
                        break;
                    }
                }
                if (preexisting) {
                    continue;
                }
                char full[PATH_MAX];
                if (snprintf(full, sizeof(full), "%s/%s", job->shots_dir, e->d_name) >=
                        (int)sizeof(full)) {
                    continue;
                }
                struct stat st;
                if (stat(full, &st) != 0) {
                    continue;
                }
                if (st.st_size > 0 && st.st_mtime >= best_mt) {
                    best_mt = st.st_mtime;
                    snprintf(cand, sizeof(cand), "%s", full);
                    cand_size = st.st_size;
                }
            }
            closedir(d);
            if (!cand[0]) {
                prev_size = -1;
                continue;
            }
            /* RA writes async: only copy once the size stops growing (stable
               across two 100ms polls), so we never grab a half-written file. */
            if (strcmp(cand, newest) == 0 && cand_size == prev_size) {
                break;
            }
            snprintf(newest, sizeof(newest), "%s", cand);
            prev_size = cand_size;
        }
        if (!newest[0]) {
            jw_log_warn("screenshot: RA screenshot did not appear under %s", job->shots_dir);
            free(job);
            return NULL;
        }

        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        char ts[24];
        strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tmv);
        char dest[PATH_MAX];
        const char *nm = job->rom_name[0] ? job->rom_name : "screenshot";
        if (snprintf(dest, sizeof(dest), "%s/%s-%s.png", dir, nm, ts) >= (int)sizeof(dest)) {
            jw_log_warn("screenshot: dest path too long under %s", dir);
            free(job);
            return NULL;
        }
        if (jw__screenshot_copy_file(newest, dest)) {
            jw_log_info("screenshot: saved %s", dest);
        } else {
            jw_log_warn("screenshot: failed to copy %s -> %s", newest, dest);
        }
        free(job);
        return NULL;
    }

    int w = 0, h = 0;
    uint8_t *rgb = jw__kmsgrab_rgb(&w, &h);
    if (!rgb) {
        jw_log_warn("screenshot: kmsgrab capture failed");
        free(job);
        return NULL;
    }

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char ts[24];
    strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tmv);
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/screenshot-%s.png", dir, ts) >= (int)sizeof(path)) {
        jw_log_warn("screenshot: path too long under %s", dir);
        free(rgb);
        free(job);
        return NULL;
    }

    bool ok = jw__screenshot_write_png(path, rgb, w, h);
    free(rgb);
    if (ok) {
        jw_log_info("screenshot: saved %s", path);
        atomic_store(&g_ss_flash_request, 1);   /* main loop flashes the launcher */
    } else {
        jw_log_warn("screenshot: failed to write %s", path);
    }
    free(job);
    return NULL;
}

/* Menu + L1 (input proxy): take a screenshot. Returns true when consumed. When
   the feature is disabled it returns false so the chord forwards normally. */
/* Menu + R1: toggle a game recording. Only meaningful while RetroArch is the
   running session -- it owns the recorder -- so decline otherwise and let R1
   reach the app normally.

   RECORDING_TOGGLE goes over RetroArch's network command interface rather than
   through its own hotkey system: a RetroArch pad hotkey needs a modifier held,
   and RetroArch stops blocking that modifier from the core after
   input_hotkey_block_delay frames, so Select ends up pressing buttons in the
   game. Menu is never a game input and the chord is consumed here. */
static bool jw__on_record_hotkey(void *userdata) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;
    if (!state || !jw__has_retroarch_session(state)) {
        return false;
    }
    /* LEAF-RA-1 frontends are configured with recording off. */
    if (state->retroarch_session.frontend) {
        return false;
    }

    /* Opt-in gate, read from memory only -- same reason as the screenshot hook.
       Declining lets the button forward to the game, which is what a user with
       recording off expects. */
    if (!state->recording_enabled_cached) {
        return false;
    }

    /* Debounced, but on its own much shorter clock than screenshots. A screenshot
       repeated inside a second is almost always a bounce and costs only a junk
       file; a recording toggle inside a second can be a deliberate "stop, wrong
       moment", and swallowing that is invisible -- the chord is consumed, nothing
       on screen changes, and the recording the user believes they stopped keeps
       running. A physical shoulder button bounces in tens of milliseconds, so
       this only has to be long enough to stop a start/stop pair from producing a
       file with no frames in it. */
    uint64_t now = jw__screenshot_now_ms();
    if (state->last_record_toggle_ms != 0 &&
        now - state->last_record_toggle_ms < JW_RECORD_THROTTLE_MS) {
        return true;
    }
    state->last_record_toggle_ms = now;

    jw_ra_client client = jw_ra_client_default();
    jw_ra_result rc = jw_ra_send_raw(&client, "RECORDING_TOGGLE");
    if (rc != JW_RA_OK) {
        jw_log_warn("record hotkey: RECORDING_TOGGLE failed result=%s",
                    jw_ra_result_string(rc));
        return true;   /* consumed either way; R1 must not reach the game */
    }
    jw_log_info("record hotkey: RECORDING_TOGGLE sent");
    return true;
}

static bool jw__on_screenshot_hotkey(void *userdata) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;
    if (!state || !state->db_path) {
        return false;
    }
    /* Content-pak standalones own their display. The launcher scanout capture
       cannot promise their frame; let the ordinary button reach the child. */
    if (jw__has_standalone_session(state) &&
        state->retroarch_session.standalone_policy.provider_bound) {
        return false;
    }

    uint64_t now = jw__screenshot_now_ms();

    /* Opt-in gate, read from memory only. Loaded at startup and replaced by
       the Settings IPC push; this function runs from the input callback, where
       a database open would put SQLite's connect/schema/busy-timeout cost on
       the same thread that drains evdev. */
    if (!state->screenshots_enabled_cached) {
        return false;   /* decline so Menu+L1 forwards to the app */
    }

    if (state->last_screenshot_ms != 0 &&
        now - state->last_screenshot_ms < JW_SS_THROTTLE_MS) {
        return true;   /* consume, but debounce rapid repeats */
    }
    state->last_screenshot_ms = now;

    jw__screenshot_job *job = (jw__screenshot_job *)calloc(1, sizeof(*job));
    if (!job) {
        return true;
    }
    snprintf(job->sdcard_root, sizeof(job->sdcard_root), "%s",
             state->sdcard_root ? state->sdcard_root : "");
    snprintf(job->db_path, sizeof(job->db_path), "%s", state->db_path);
    job->in_game = jw__has_retroarch_session(state);
    if (job->in_game) {
        if (state->runtime_dir) {
            snprintf(job->shots_dir, sizeof(job->shots_dir), "%s/shots",
                     state->runtime_dir);
        }
        jw__screenshot_rom_name(state->retroarch_session.rom_path,
                                job->rom_name, sizeof(job->rom_name));
    }

    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, jw__screenshot_worker, job) != 0) {
        jw_log_warn("screenshot: worker thread spawn failed");
        free(job);
    }
    pthread_attr_destroy(&attr);
    return true;
}

/* Resolve a Menu chord against the daemon's shortcut snapshot and run the
   action the user bound to that button.
 *
 * Never touches SQLite. This runs on the same main loop that drains evdev, and
 * the settings helper opens a connection, reapplies schema, and can wait on
 * SQLite's busy timeout -- paying that per chord would turn a settings lookup
 * into dropped controller input. The snapshot is plain memory, replaced whole.
 *
 * Returns the handler's own answer, so a declining action (feature off, no
 * RetroArch session) still forwards the chord to the game. */
static bool jw__on_shortcut_chord(void *userdata,
                                  jw_input_shortcut_button button) {
    jw_daemon_state *state = (jw_daemon_state *)userdata;
    if (!state) {
        return false;
    }
    jw_input_shortcut_action action;
    if (!jw_input_shortcuts_action_for_button(&state->shortcuts, button,
                                              &action)) {
        return false;   /* nothing bound to it: forward as an ordinary chord */
    }
    switch (action) {
        case JW_INPUT_SHORTCUT_GAME_SWITCHER: return jw__input_game_switcher(state);
        case JW_INPUT_SHORTCUT_SCREENSHOT:    return jw__on_screenshot_hotkey(state);
        case JW_INPUT_SHORTCUT_RECORDING:     return jw__on_record_hotkey(state);
        default: return false;
    }
}

/* Load the shortcut bindings from the settings table into the daemon's
   snapshot. Called once, before the full proxy starts polling -- this is the
   only place the shortcut path touches SQLite, and it does so on a thread that
   is not yet draining evdev.
 *
 * A read failure keeps the compiled-in defaults rather than disabling
   everything: a card that cannot be read is not a user asking for no
   shortcuts. Malformed and duplicate values are resolved fail-closed by the
   shared model and logged individually, so a hand-edited database says which
   key was wrong rather than silently doing nothing. */
/* Everything the chord path needs, in one database open at startup: the three
   bindings and the two capture opt-ins that gate them.
   The opt-ins used to be refreshed lazily from the handlers, and then from a
   periodic tick -- both put SQLite on, or on the same loop as, the thread that
   drains evdev. They follow the bindings instead: read once here, replaced by
   the Settings IPC push, never polled. */
#define JW_SHORTCUT_SETTING_COUNT (JW_INPUT_SHORTCUT_ACTION_COUNT + 2)

static void jw__load_shortcuts(jw_daemon_state *state) {
    if (!state || !state->db_path[0]) {
        return;
    }
    char values[JW_SHORTCUT_SETTING_COUNT][32];
    jw_db_setting_query q[JW_SHORTCUT_SETTING_COUNT];
    for (int i = 0; i < JW_SHORTCUT_SETTING_COUNT; i++) {
        values[i][0] = '\0';
        q[i].out = values[i];
        q[i].out_size = sizeof(values[i]);
        q[i].found = 0;
    }
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        q[i].key = jw_input_shortcut_action_key((jw_input_shortcut_action)i);
    }
    q[JW_INPUT_SHORTCUT_ACTION_COUNT].key = "screenshots_enabled";
    q[JW_INPUT_SHORTCUT_ACTION_COUNT + 1].key = "recording_enabled";

    if (jw_db_get_settings(state->db_path, q, JW_SHORTCUT_SETTING_COUNT) != 0) {
        jw_log_warn("input shortcuts: settings read failed; keeping defaults");
        return;
    }

    /* Both opt-ins default off, so an absent key reads as off. */
    state->screenshots_enabled_cached =
        strcmp(values[JW_INPUT_SHORTCUT_ACTION_COUNT], "1") == 0;
    state->recording_enabled_cached =
        strcmp(values[JW_INPUT_SHORTCUT_ACTION_COUNT + 1], "1") == 0;

    const char *raw[JW_INPUT_SHORTCUT_ACTION_COUNT];
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        raw[i] = q[i].found ? values[i] : NULL;
    }
    bool invalid[JW_INPUT_SHORTCUT_ACTION_COUNT];
    int duplicate[JW_INPUT_SHORTCUT_ACTION_COUNT];
    jw_input_shortcuts loaded;
    int rejected = jw_input_shortcuts_resolve(raw, &loaded, invalid, duplicate);
    for (int i = 0; rejected && i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        jw_input_shortcut_action action = (jw_input_shortcut_action)i;
        if (invalid[i]) {
            jw_log_warn("input shortcuts: %s has an unreadable value \"%s\"; "
                        "%s disabled",
                        jw_input_shortcut_action_key(action), values[i],
                        jw_input_shortcut_action_label(action));
        } else if (duplicate[i] >= 0) {
            /* Name both sides: "disabled" alone leaves the user checking the
               other rows to find what took the button. */
            jw_input_shortcut_action winner =
                (jw_input_shortcut_action)duplicate[i];
            jw_log_warn("input shortcuts: %s and %s both want %s; "
                        "%s keeps it and %s is disabled",
                        jw_input_shortcut_action_label(action),
                        jw_input_shortcut_action_label(winner),
                        jw_input_shortcut_button_name(loaded.buttons[duplicate[i]]),
                        jw_input_shortcut_action_label(winner),
                        jw_input_shortcut_action_label(action));
        }
    }
    state->shortcuts = loaded;

    /* One line stating what the input path will actually act on. The warnings
       above only fire when something was rejected, so without this a support
       log cannot distinguish "loaded the defaults" from "never ran". */
    jw_log_info("input shortcuts: switcher=%s screenshot=%s recording=%s "
                "screenshots_enabled=%d recording_enabled=%d",
                jw_input_shortcut_button_name(
                    loaded.buttons[JW_INPUT_SHORTCUT_GAME_SWITCHER]),
                jw_input_shortcut_button_name(
                    loaded.buttons[JW_INPUT_SHORTCUT_SCREENSHOT]),
                jw_input_shortcut_button_name(
                    loaded.buttons[JW_INPUT_SHORTCUT_RECORDING]),
                state->screenshots_enabled_cached ? 1 : 0,
                state->recording_enabled_cached ? 1 : 0);
}

static void jw__start_input_proxy(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    if (state->input_proxy.enabled) {
        jw__publish_retroarch_input_env(state);
        return;
    }

    if (jw_input_proxy_init(&state->input_proxy, jw__input_brightness_delta,
                            jw__input_volume_delta, jw__input_menu_tap,
                            state) == 0) {
        state->input_proxy.menu_escape = jw__input_menu_escape;
        if (state->session_menu_config.tap_only)
            jw_input_proxy_configure_menu(&state->input_proxy, state->session_menu_config);
        state->input_proxy.shortcut = jw__on_shortcut_chord;
        state->input_proxy.rumble = jw__rumble_ff;
        /* Forward from a thread of its own, now that every callback is in
           place: this loop can block for hundreds of ms (IPC, spawns), and a
           release it held back turned taps into holds. The watch-only proxy
           forwards nothing, so it stays on this loop. */
        (void)jw_input_proxy_start(&state->input_proxy);
        jw__publish_retroarch_input_env(state);
    }
}

static void jw__suspend_input_proxy_for_app(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    if (state->input_proxy.enabled) {
        jw_log_info("input proxy: releasing grab for app launch");
        jw_input_proxy_shutdown(&state->input_proxy);
    }
    jw__publish_direct_input_env();
}

/* ── Input roster isolation (paired wireless controllers) ─────────────────
   Game/emulator children launch inside a frozen roster plus a private
   /dev/input mount snapshot, per plans/paired-wireless-controllers-mlp1.
   All failures block the launch with a stable error code; there is never a
   fallback to direct physical Loong input on roster-capable platforms. */

#define JW_INPUT_ISOLATION_SDL_MAX \
    (JW_INPUT_ROSTER_MAX_CONTROLLERS * (JW_INPUT_ROSTER_PATH_MAX + 1))

static int jw__pipe_cloexec(int fds[2]) {
    fds[0] = -1;
    fds[1] = -1;
    if (pipe(fds) != 0) {
        return -1;
    }
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

/* Build the frozen launch roster. Returns 1 when a roster is active, 0 when
   isolation is unsupported on this platform (desktop: legacy direct input),
   and -1 with a stable error code when the roster is required but could not
   be built -- the caller must abort the launch. */
static int jw__launch_prepare_input_roster(jw_daemon_state *state,
                                           jw_input_roster *roster,
                                           char *sdl_devices,
                                           size_t sdl_devices_size,
                                           int player_joypad_indices[4],
                                           const char *tag,
                                           char *error, size_t error_size) {
    if (!jw_input_roster_supported()) {
        return 0;
    }
    if (jw_input_roster_build(&state->input_proxy, roster,
                              error, error_size) != 0) {
        state->launch_input_failed = true;
        return -1;
    }
    jw_input_roster_log(roster, tag);
    jw_input_roster_sdl_devices(roster, sdl_devices, sdl_devices_size);
    for (int i = 0; i < 4; i++) {
        player_joypad_indices[i] = i < roster->count ? i : -1;
    }
    return 1;
}

/* Parent side, right after fork: stage the per-child /dev/input view and hand
   its dir to the child over the sync pipe. A failure is signaled as an empty
   string so the child exits rather than execing unisolated. */
static int jw__input_isolation_parent_prepare(const jw_input_roster *roster,
                                              pid_t child_pid,
                                              int sync_write_fd,
                                              char *dir_out,
                                              size_t dir_out_size) {
    char dir[PATH_MAX];
    dir[0] = '\0';
    if (jw_input_namespace_prepare(roster, child_pid, dir, sizeof(dir)) != 0) {
        (void)write(sync_write_fd, "", 1);
        return -1;
    }
    size_t len = strlen(dir) + 1;
    if (write(sync_write_fd, dir, len) != (ssize_t)len) {
        jw_input_namespace_cleanup_dir(dir);
        return -1;
    }
    if (dir_out && dir_out_size > 0) {
        snprintf(dir_out, dir_out_size, "%s", dir);
    }
    return 0;
}

/* Child side, between fork and exec. Blocks for the staged dir, enters the
   private namespace, publishes the frozen SDL roster, and returns only when
   the child may exec. Any failure is written to err_write_fd and the child
   exits 126 WITHOUT exec -- fail closed, no unisolated emulator, ever. */
static void jw__input_isolation_child_enter(const jw_input_roster *roster,
                                            const char *sdl_devices,
                                            int sync_read_fd,
                                            int err_write_fd) {
    char dir[PATH_MAX];
    ssize_t got;
    do {
        got = read(sync_read_fd, dir, sizeof(dir));
    } while (got < 0 && errno == EINTR);
    close(sync_read_fd);

    if (got <= 1 || got >= (ssize_t)sizeof(dir) || dir[0] == '\0' ||
        !memchr(dir, '\0', (size_t)got)) {
        static const char msg[] = JW_INPUT_ROSTER_ERR_PREPARE
            ": parent did not stage an input namespace";
        (void)write(err_write_fd, msg, sizeof(msg));
        _exit(126);
    }

    char err[256];
    if (jw_input_namespace_enter(dir, roster, err, sizeof(err)) != 0) {
        char msg[288];
        snprintf(msg, sizeof(msg), "%s", err);
        (void)write(err_write_fd, msg, strlen(msg) + 1);
        _exit(126);
    }

    setenv("SDL_JOYSTICK_DEVICE", sdl_devices, 1);
    setenv("SDL_JOYSTICK_DISABLE_UDEV", "1", 1);
    /* SDL's HIDAPI drivers reach Xbox and similar pads through /dev/hidraw*,
       which the mount namespace does not cover: such a pad would enter SDL's
       device list outside the roster order, and one excluded by the
       three-external limit would still be visible to the emulator. Force the
       evdev path so the private /dev/input view is the single source of truth
       for which controllers exist and in which order. */
    setenv("SDL_JOYSTICK_HIDAPI", "0", 1);
    char text[8];
    snprintf(text, sizeof(text), "%d", roster->count);
    setenv("JAWAKA_INPUT_ROSTER_COUNT", text, 1);
    snprintf(text, sizeof(text), "%d", jw_input_roster_virtual_index(roster));
    setenv("JAWAKA_RETROARCH_JOYPAD_INDEX", text, 1);
    close(err_write_fd); /* CLOEXEC closes it at exec too; explicit is fine */
}

/* Parent side: block until the child has exec'd (CLOEXEC closes the err fd)
   or reported a pre-exec isolation failure. Returns 0 once exec happened. */
static int jw__input_isolation_parent_wait_exec(int err_read_fd,
                                                char *error,
                                                size_t error_size) {
    ssize_t got;
    do {
        got = read(err_read_fd, error,
                   error_size > 0 ? error_size - 1 : 0);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        snprintf(error, error_size, "%s: child handshake read failed",
                 JW_INPUT_ROSTER_ERR_NAMESPACE);
        return -1;
    }
    if (got > 0) {
        error[got] = '\0';
        return -1;
    }
    if (error_size > 0) {
        error[0] = '\0';
    }
    return 0;
}

/* Parent side: run both halves of the isolation handshake for a roster
   launch. Returns 0 when the child has exec'd isolated; -1 with a stable
   error otherwise, after the child has been stopped and reaped. */
static int jw__input_isolation_parent_finish(jw_daemon_state *state,
                                             const jw_input_roster *roster,
                                             pid_t child_pid,
                                             int sync_write_fd,
                                             int err_read_fd,
                                             char *error, size_t error_size) {
    int rc = jw__input_isolation_parent_prepare(roster, child_pid,
                                                sync_write_fd,
                                                state->child_input_ns_dir,
                                                sizeof(state->child_input_ns_dir));
    if (rc != 0) {
        snprintf(error, error_size, "%s: parent staging failed",
                 JW_INPUT_ROSTER_ERR_PREPARE);
    }
    if (rc == 0) {
        rc = jw__input_isolation_parent_wait_exec(err_read_fd,
                                                  error, error_size);
    }
    if (rc != 0) {
        (void)kill(child_pid, SIGKILL);
        while (waitpid(child_pid, NULL, 0) < 0 && errno == EINTR) {
        }
        if (state->child_input_ns_dir[0]) {
            jw_input_namespace_cleanup_dir(state->child_input_ns_dir);
            state->child_input_ns_dir[0] = '\0';
        }
        state->launch_input_failed = true;
        jw_log_info("input namespace: setup failed; child pid=%d stopped",
                    (int)child_pid);
        return -1;
    }
    jw_log_info("input namespace: active %s",
                state->child_input_ns_dir);
    return 0;
}

static int jw__spawn_child(jw_daemon_state *state, jw_child_kind kind) {
    jw__set_session_menu(state, false);
    const char *name = jw__child_name(kind);
    if (!state || !name || kind == JW_CHILD_RETROARCH ||
        kind == JW_CHILD_EMULATOR || jw__child_kind_is_app(kind)) {
        return -1;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", state->bin_dir, name) >=
        (int)sizeof(path)) {
        jw_log_error("child binary path too long: %s/%s", state->bin_dir, name);
        return -1;
    }
    if (!jw__path_exists(path)) {
        jw_log_error("child binary missing: %s", path);
        return -1;
    }

    /* Resolve appearance from the DB here in the parent — opening SQLite between
       fork() and execv() is not fork-safe on macOS (os_log landmine). */
    jw_appearance_env appearance;
    jw_appearance_resolve(state->db_path, &appearance);

    /* Re-resolve 5-Game Mode from the DB + SD lock file on every launcher spawn
       (parent side, fork-safe) so an in-launcher unlock — which clears the flag
       and removes the lock file — is honored on the next spawn instead of a
       stale boot-time cache re-entering focus. */
    if (kind == JW_CHILD_LAUNCHER) {
        state->focus_boot = jw_focus_resolve_boot(state->db_path,
                                                  state->sdcard_root,
                                                  &state->focus_cfg);
    }

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        jw_appearance_apply_env(&appearance);
        if (kind == JW_CHILD_LAUNCHER && state->launch_input_failed)
            setenv("JAWAKA_LAUNCH_INPUT_FAILED", "1", 1);
        else
            unsetenv("JAWAKA_LAUNCH_INPUT_FAILED");
        /* Resume Game on Boot failed: the launcher posts its one notice. */
        if (kind == JW_CHILD_LAUNCHER && state->boot_resume_notice_pending)
            setenv("JAWAKA_BOOT_RESUME_FAILED", "1", 1);
        else
            unsetenv("JAWAKA_BOOT_RESUME_FAILED");
        if (kind == JW_CHILD_LAUNCHER && state->boot_resume_notice_pending &&
            state->boot_resume_notice_hardcore)
            setenv("JAWAKA_BOOT_RESUME_REASON", "hardcore", 1);
        else
            unsetenv("JAWAKA_BOOT_RESUME_REASON");
        /* 5-Game Mode: while active, every launcher spawn (incl. return-from-game)
           re-enters the focus screen. Pass the chosen set + style so the launcher
           renders focus mode instead of the normal tab UI. */
        if (kind == JW_CHILD_LAUNCHER &&
            state->focus_boot == JW_FOCUS_BOOT_ENTER) {
            char ids_csv[128];
            jw_focus_ids_to_csv(state->focus_cfg.ids, state->focus_cfg.id_count,
                                ids_csv, sizeof(ids_csv));
            setenv("JAWAKA_FOCUS_MODE", "1", 1);
            setenv("JAWAKA_FOCUS_IDS", ids_csv, 1);
            setenv("JAWAKA_FOCUS_STYLE",
                   jw_focus_style_name(state->focus_cfg.style), 1);
            setenv("JAWAKA_FOCUS_LOCK",
                   jw_focus_lock_name(state->focus_cfg.lock), 1);
        } else {
            unsetenv("JAWAKA_FOCUS_MODE");
        }
        char *const argv[] = { (char *)path, NULL };
        execv(path, argv);
        perror("execv");
        _exit(127);
    }

    if (kind == JW_CHILD_LAUNCHER) {
        state->launch_input_failed = false;
        state->boot_resume_notice_pending = false;
        state->boot_resume_notice_hardcore = false;
    }
    state->launcher_levels_signal = false;   /* until the new one says hello */
    state->child_pid = pid;
    state->child_kind = kind;
    jw_log_info("spawned %s pid=%d", name, (int)pid);
    return 0;
}

/* Spawn the in-game menu process. show_now=false parks it hidden as a warm
   standby (revealed later by a SIGUSR1 show signal); show_now=true makes it
   reveal itself as soon as it finishes cat_init (on-demand fallback path). */
static int jw__spawn_in_game_menu(jw_daemon_state *state, bool show_now) {
    if (!state || state->menu_pid > 0) {
        return state && state->menu_pid > 0 ? 0 : -1;
    }

    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/jawaka-menu", state->bin_dir) >=
        (int)sizeof(path)) {
        jw_log_error("in-game menu binary path too long: %s/jawaka-menu",
                     state->bin_dir);
        return -1;
    }
    if (!jw__path_exists(path)) {
        jw_log_error("in-game menu binary missing: %s", path);
        return -1;
    }

    /* Resolve appearance from the DB here in the parent — opening SQLite between
       fork() and execv() is not fork-safe on macOS (os_log landmine). */
    jw_appearance_env appearance;
    jw_appearance_resolve(state->db_path, &appearance);

    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("in-game menu fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        jw_appearance_apply_env(&appearance);
        if (jw__has_retroarch_session(state)) {
            setenv("JAWAKA_INGAME_ACTIVE", "1", 1);
            setenv("JAWAKA_INGAME_SYSTEM", state->retroarch_session.system, 1);
            setenv("JAWAKA_INGAME_ROM", state->retroarch_session.rom_path, 1);
            setenv("JAWAKA_INGAME_CORE", state->retroarch_session.core_path, 1);
            setenv("JAWAKA_INGAME_CORE_ID", state->retroarch_session.core_id, 1);
            setenv("JAWAKA_INGAME_CORE_FOLDER",
                   state->retroarch_session.core_config_folder, 1);
        }
        setenv("JAWAKA_INGAME_AUTOSHOW", show_now ? "1" : "0", 1);
        char dir_buf[PATH_MAX];
        if (state->runtime_dir &&
            snprintf(dir_buf, sizeof(dir_buf), "%s/shots", state->runtime_dir) <
                (int)sizeof(dir_buf)) {
            setenv("JAWAKA_INGAME_SHOTDIR", dir_buf, 1);
        }
        const char *state_root = state->retroarch_session.source_root[0]
            ? state->retroarch_session.source_root
            : state->sdcard_root;
        const char *states_dir = jw__env_value("STATES_PATH");
        if (state->retroarch_session.source_root[0] ||
            !states_dir || strcmp(states_dir, state->sdcard_root) == 0) {
            if (state_root &&
                snprintf(dir_buf, sizeof(dir_buf), "%s/States", state_root) <
                    (int)sizeof(dir_buf)) {
                setenv("JAWAKA_INGAME_STATEDIR", dir_buf, 1);
            }
        } else if (states_dir &&
            snprintf(dir_buf, sizeof(dir_buf), "%s", states_dir) <
                    (int)sizeof(dir_buf)) {
            setenv("JAWAKA_INGAME_STATEDIR", dir_buf, 1);
        }
        char *const argv[] = { (char *)path, "--in-game", NULL };
        execv(path, argv);
        perror("execv");
        _exit(127);
    }

    state->menu_pid = pid;
    state->menu_in_game = true;
    jw_log_info("spawned in-game menu pid=%d standby=%d", (int)pid, !show_now);
    return 0;
}

/* LOG-SAFE: this daemon's stdout and stderr are the supervisor's session log,
   which lives on a FAT card and can go unwritable (a bad cluster chain, a full
   card, or the FAT32 4 GiB per-file ceiling). A shell wrapper inheriting that
   fd dies on its first log write under set -e -- the Leaf 0.11 ports failure --
   so before forking a game the daemon proves its own descriptors take a byte
   and repoints any that do not: at internal storage on MLP1, /dev/null
   otherwise. Every child forked afterwards inherits a working descriptor, and
   the substitution is logged where it can be read. This runs in the parent:
   the child of a threaded daemon must not touch stdio between fork and exec. */
static void jw__heal_stdio_before_game(void) {
#ifdef PLATFORM_MLP1
    const char *fallback_dir = JW_STORAGE_MLP1_LOG_DIR;
#else
    const char *fallback_dir = NULL;
#endif
    switch (jw_log_heal_unwritable_stdio(fallback_dir, "jawakad")) {
    case 1:
        jw_log_warn("game launch: session log fd was unwritable; daemon and game output "
                    "continue in %s/jawakad.log", fallback_dir);
        break;
    case 2:
        jw_log_warn("game launch: session log fd was unwritable and no fallback log could "
                    "be opened; game output is discarded");
        break;
    default:
        break;
    }
}

static int jw__spawn_app(jw_daemon_state *state) {
    if (!state || !state->pending_app) {
        return -1;
    }
    if (jw_delete_job_busy(state->delete_job)) return -1;

    const char *error_message = NULL;
    char pak_abs[PATH_MAX];
    char launch_abs[PATH_MAX];
    if (jw__resolve_app_launch_path(state, state->pending_app_pak_dir,
                                    pak_abs, sizeof(pak_abs),
                                    launch_abs, sizeof(launch_abs),
                                    &error_message) != 0) {
        jw_log_error("could not resolve app launch path: %s",
                     error_message ? error_message : state->pending_app_pak_dir);
        state->pending_app = false;
        return -1;
    }

    jw_storage_source_list sources = {0};
    const jw_storage_source *app_source = NULL;
    if (jw__storage_sources(state, &sources) == 0) {
        app_source = jw_storage_sources_find_for_path(&sources, pak_abs);
    }

    bool app_is_retroarch = jw__pak_dir_is_retroarch(state->pending_app_pak_dir);
    bool app_is_pico8 = jw_pico8_app_matches(jw_storage_sources_primary(&sources), pak_abs);
    jw_pico8_paths pico8_paths;
    bool pico8_paths_ready = app_is_pico8 && jw_storage_source_paths_v2_valid() &&
                            jw_pico8_resolve_paths(&sources, NULL, &pico8_paths);

    jw_input_roster roster;
    char sdl_devices[JW_INPUT_ISOLATION_SDL_MAX];
    sdl_devices[0] = '\0';
    bool use_roster = false;
    if ((app_is_retroarch || app_is_pico8) && jw_input_roster_supported()) {
        /* RetroArch.pak menu launches consume the same protected roster as
           core launches (plans/paired-wireless-controllers-mlp1): the proxy
           stays in full grab-and-forward mode so calibrated built-in input and
           paired wireless controllers reach the RetroArch menu, and the child
           gets the private /dev/input view. The runner receives the roster
           positions from the environment; it must not count "Loong Gamepad"
           devices. */
        jw__start_input_proxy(state);
        int dummy_indices[4];
        char roster_error[256];
        if (jw__launch_prepare_input_roster(
                state, &roster, sdl_devices, sizeof(sdl_devices), dummy_indices,
                app_is_pico8 ? JW_PICO8_CORE : "retroarch-menu",
                roster_error, sizeof(roster_error)) < 0) {
            jw_log_error("%s app launch blocked: %s", app_is_pico8 ? "PICO-8" : "RetroArch", roster_error);
            state->pending_app = false;
            return -1;
        }
        use_roster = true;
    } else {
        jw__suspend_input_proxy_for_app(state);
        /* Watch the pad in watch-only mode so the hardware volume / brightness keys
           keep working while a generic app is foreground — the same as standalone
           emulators. The app reads the pad directly for its own input; jawakad only
           watches for the system hotkeys (volume/brightness; Menu and the switcher
           no-op without a game session). The app-exit handler tears this watch proxy
           down and restores the full grab. */
        if (jw_input_proxy_init_watch(&state->input_proxy, jw__input_brightness_delta,
                                      jw__input_volume_delta, jw__input_menu_tap,
                                      state) != 0) {
            jw_log_warn("input watch: init failed; volume/brightness keys unavailable this app session");
        }
    }
    jw__reconcile_audio(state, "app-launch", false);
    jw__publish_audio_env(state);
    if (app_is_pico8) (void)jw__perf_apply_game(state, "PICO8", "app-launch");
    else (void)jw__perf_apply_frontend(state, "app-launch");

    /* Resolve appearance from the DB here in the parent — opening SQLite between
       fork() and execv() is not fork-safe on macOS (os_log landmine). */
    jw_appearance_env appearance;
    jw_appearance_resolve(state->db_path, &appearance);

    /* The RetroArch.pak runner builds its own RA config (reading JAWAKA_CHEEVOS_*),
       so it legitimately needs the cheevos creds; every other app must not see
       them. Resolve in the parent (SQLite open is not fork-safe post-fork) and
       apply child-side only for the RetroArch app. */
    jw_cheevos_creds cheevos;
    jw__cheevos_resolve(state, &cheevos);

    /* Isolation handshake pipes (see the RetroArch game path). */
    int sync_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (use_roster &&
        (jw__pipe_cloexec(sync_pipe) != 0 || jw__pipe_cloexec(err_pipe) != 0)) {
        state->launch_input_failed = true;
        jw_log_error("%s app launch blocked: %s: control pipe creation failed",
                     app_is_pico8 ? "PICO-8" : "RetroArch", JW_INPUT_ROSTER_ERR_NAMESPACE);
        state->pending_app = false;
        return -1;
    }

    jw__heal_stdio_before_game();
    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        if (sync_pipe[0] >= 0) close(sync_pipe[0]);
        if (sync_pipe[1] >= 0) close(sync_pipe[1]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        if (err_pipe[1] >= 0) close(err_pipe[1]);
        jw_input_proxy_shutdown(&state->input_proxy);   /* drop the watch-only proxy */
        jw__start_input_proxy(state);
        state->pending_app = false;
        return -1;
    }

    if (pid == 0) {
        if (app_is_retroarch || app_is_pico8) {
            /* Both sides call setpgid so neither the fork/exec race nor a slow
               parent can leave RetroArch outside the group the daemon signals.
               RetroArch inherits this group from the runner, which lets a
               shutdown escalate to the whole app without touching jawakad or
               any other app. */
            (void)jw__game_child_set_own_group();
        }
        if (use_roster) {
            close(sync_pipe[1]);
            close(err_pipe[0]);
            /* Any failure here exits 126 without exec: fail closed. */
            jw__input_isolation_child_enter(&roster, sdl_devices,
                                            sync_pipe[0], err_pipe[1]);
        }
        jw_appearance_apply_env(&appearance);
        jw__publish_source_content_env(app_source);
        jw_pico8_export(pico8_paths_ready ? &pico8_paths : NULL,
                        app_is_pico8 && use_roster);
        /* Only the RetroArch runner gets cheevos creds (it writes its own RA
           config); every other app has them explicitly cleared so the plaintext
           password never reaches third-party app code. The standalone account
           snapshot (UMRK_RA_ACCOUNT_*) is never valid for any .pak app child:
           RetroArch is not a standalone consumer and non-RetroArch app pak
           launches are not authorized targets. */
        jw__ra_account_clear_env();
        jw__flycast_route_clear_env();
        if (app_is_retroarch) {
            jw__cheevos_apply_env(&cheevos);
        } else {
            jw__cheevos_clear_env();
        }
        if (chdir(pak_abs) != 0) {
            perror("chdir");
            _exit(127);
        }
        char *const argv[] = { (char *)launch_abs, NULL };
        execv(launch_abs, argv);
        perror("execv");
        _exit(127);
    }

    if (use_roster) {
        close(sync_pipe[0]);
        close(err_pipe[1]);
        char isolation_error[256];
        int isolation_rc = jw__input_isolation_parent_finish(
            state, &roster, pid, sync_pipe[1], err_pipe[0],
            isolation_error, sizeof(isolation_error));
        close(sync_pipe[1]);
        close(err_pipe[0]);
        if (isolation_rc != 0) {
            jw_log_error("%s app launch blocked: %s", app_is_pico8 ? "PICO-8" : "RetroArch", isolation_error);
            state->pending_app = false;
            return -1;
        }
    }

    state->child_pid = pid;
    state->child_pgid = -1;
    state->child_kind = JW_CHILD_APP;
    if (app_is_retroarch || app_is_pico8) {
        /* Only track a pgid we could actually confirm. A group signal aimed at
           an unverified id could reach jawakad itself or an unrelated app, so
           an unreservable group falls back to signalling the runner alone. */
        if (jw__reserve_game_process_group(pid)) {
            state->child_pgid = pid;
        } else {
            jw_log_warn("RetroArch app pgid could not be reserved for pid=%d; "
                        "stopping the runner alone", (int)pid);
        }
        if (app_is_retroarch) state->child_kind = JW_CHILD_RETROARCH_APP;
        else state->child_kind = JW_CHILD_PICO8_APP;
    }
    state->child_stop_signalled = false;
    state->child_stop_deadline_ms = 0;
    state->child_group_wait_started_ms = 0;
    state->child_group_kill_logged_ms = 0;
    state->pending_app = false;
    jw_log_info("spawned app pid=%d pgid=%d pak=%s", (int)pid,
                (int)state->child_pgid, pak_abs);
    return 0;
}

static int jw__spawn_standalone_emulator(jw_daemon_state *state,
                                         const jw_launch_target *target) {
    if (!state || !state->pending_launch || !target ||
        target->kind != JW_LAUNCH_TARGET_STANDALONE) {
        return -1;
    }

    char rom_abs[PATH_MAX];
    jw_game_entry launch_game;
    jw_storage_source_list sources;
    const jw_storage_source *rom_source = NULL;
    if (jw__resolve_library_game(state, state->pending_launch_game_id,
                                 &launch_game, &sources, &rom_source,
                                 rom_abs, sizeof(rom_abs)) != 0) {
        jw_log_error("could not resolve stable game id=%d",
                     state->pending_launch_game_id);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        return -1;
    }

    char source_root[PATH_MAX];
    if (rom_source) {
        snprintf(source_root, sizeof(source_root), "%s", rom_source->root);
    } else {
        snprintf(source_root, sizeof(source_root), "%s", state->sdcard_root);
    }

    jw_pico8_paths pico8_paths;
    if (target->native_pico8 &&
        !jw_pico8_resolve_paths(&sources, rom_source, &pico8_paths)) {
        jw_log_error("PICO-8 launch paths unavailable");
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        return -1;
    }
    bool direct_drm = jw__standalone_target_requests_direct_drm(state, target, rom_abs);

    bool switcher_resume = state->pending_launch_resume_switcher;
    bool have_standalone_resume = false;
    int standalone_resume_slot = 0;
    char standalone_resume_path[PATH_MAX];
    standalone_resume_path[0] = '\0';
    if (switcher_resume && jw__standalone_target_is_mupen64plus(target)) {
        char states_dir[PATH_MAX];
        if (snprintf(states_dir, sizeof(states_dir), "%s/States", source_root) <
                (int)sizeof(states_dir) &&
            jw_ra_find_resume_state(states_dir, rom_abs,
                                    JW_RA_GAME_SWITCHER_STATE_SLOT,
                                    &standalone_resume_slot,
                                    standalone_resume_path,
                                    sizeof(standalone_resume_path))) {
            have_standalone_resume = true;
            jw_log_info("standalone resume: state slot=%d path=%s",
                        standalone_resume_slot, standalone_resume_path);
        } else {
            jw_log_info("standalone resume: no prelaunch state found for %s", rom_abs);
        }
    }

    char exec_error[256];
    if (!jw_sdcard_exec_available_for_path(target->path, exec_error, sizeof(exec_error))) {
        jw_log_error("cannot launch standalone emulator from SD: %s", exec_error);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        return -1;
    }

    /* Re-resolve the Saturn BIOS choice here, not only at request time: a card
       can be pulled between the two. This is the last check before the display
       handoff, so a stale selection never reaches a black screen the user
       cannot back out of. */
    jw__bios_launch bios;
    jw__resolve_launch_bios(state, target, state->pending_launch_system,
                            state->pending_launch_rom_path, &bios);
    const char *bios_error = jw__bios_launch_error(&bios);
    if (bios_error) {
        jw_log_error("standalone launch blocked: %s (source=%s path=%s)",
                     bios_error, bios.resolution.choice.source_id,
                     bios.resolution.choice.rel_path);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        return -1;
    }
    if (bios.applies) {
        jw_log_info("saturn bios: mode=%s origin=%s%s%s",
                    bios.resolution.choice.kind == JW_BIOS_CHOICE_FILE
                        ? "external" : "hle",
                    bios.resolution.origin == JW_BIOS_ORIGIN_GAME ? "game"
                        : bios.resolution.origin == JW_BIOS_ORIGIN_SYSTEM
                            ? "system" : "default",
                    bios.abs_path[0] ? " file=" : "",
                    bios.abs_path[0] ? bios.abs_path : "");
    }

    jw_platform_result ready_result;
    jw_platform_frontend_ready(&state->platform, "launcher", &ready_result);
    jw_log_info("standalone emulator launch transition readiness code=%s",
                jw_platform_result_code_name(ready_result.code));

    jw_input_roster roster;
    char sdl_devices[JW_INPUT_ISOLATION_SDL_MAX];
    sdl_devices[0] = '\0';
    bool use_roster = false;
    if (jw__standalone_target_uses_calibrated_virtual_input(target)) {
        /* Provider-bound cores and the supported release standalones use
           the same calibrated virtual gamepad path as RetroArch. Keep the
           full grab-and-forward proxy active so Joe's calibration is applied
           before SDL sees the axes, and launch inside the frozen controller
           roster. Device builds fail closed when the roster cannot be built
           -- never a fallback to direct physical Loong input. */
        jw__start_input_proxy(state);
        if (jw_input_roster_supported()) {
            int dummy_indices[4];
            char roster_error[256];
            if (jw__launch_prepare_input_roster(
                    state, &roster, sdl_devices, sizeof(sdl_devices),
                    dummy_indices,
                    target->core_id[0] ? target->core_id : "standalone",
                    roster_error, sizeof(roster_error)) < 0) {
                jw_log_error("standalone launch blocked: %s", roster_error);
                state->pending_launch = false;
                state->pending_launch_resume_switcher = false;
                state->pending_launch_override_unverified = false;
                return -1;
            }
            use_roster = true;
        } else if (state->input_proxy.enabled && state->input_proxy.virtual_event_path[0]) {
            int joypad_index = jw_input_proxy_retroarch_joypad_index(&state->input_proxy);
            jw_log_info("standalone input proxy: physical=%s virtual=%s joypad_index=%d",
                        state->input_proxy.physical_event_path[0]
                            ? state->input_proxy.physical_event_path
                            : "(unknown)",
                        state->input_proxy.virtual_event_path,
                        joypad_index);
        } else {
            jw_log_warn("standalone input proxy: calibrated virtual gamepad unavailable; falling back to direct SDL input");
            jw__publish_direct_input_env();
        }
    } else {
        /* Other standalone emulators read the physical pad directly (suspend the
           grab, like app launches), but jawakad still needs the hotkeys: re-open
           the pad in watch-only mode so volume/brightness keep working and Menu
           can route to the emulator's native menu or generic exit path. */
        jw__suspend_input_proxy_for_app(state);
        if (jw_input_proxy_init_watch(&state->input_proxy, jw__input_brightness_delta,
                                      jw__input_volume_delta, jw__input_menu_tap,
                                      state) != 0) {
            jw_log_warn("input watch: init failed; Menu exit unavailable this session");
        }
    }
    jw__reconcile_audio(state, "standalone-launch", false);
    jw__publish_audio_env(state);
    /* Standalone emulators follow the UI language too. Publishing it here and
       not only on the RetroArch path is the difference between "works" and
       "works as long as you launched a RetroArch game first this boot" -- the
       daemon env persists, so a stale value from an earlier launch used to make
       this look correct by accident. */
    jw__publish_language_env(state);
    (void)jw__perf_apply_launch_game(state, state->pending_launch_system,
                                     state->pending_launch_rom_path,
                                     "standalone-emulator-launch");

    jw_appearance_env appearance;
    jw_appearance_resolve(state->db_path, &appearance);

    /* Resolve the RetroAchievements account and this target's authorization
       for the standalone-ra-account-v1 handoff in the parent: opening SQLite
       is not fork-safe post-fork, and the fresh snapshot applies child-side
       only to an authorized target. */
    jw_ra_account ra_account;
    jw__cheevos_resolve(state, &ra_account);
    char ra_platform_dir[PATH_MAX];
    bool ra_account_authorized =
        jw__platform_path(ra_platform_dir, sizeof(ra_platform_dir), state) == 0 &&
        jw_ra_account_target_authorized(target->path, target->core_id,
                                        &target->standalone_policy,
                                        target->provider, ra_platform_dir);
    /* Proxy plan P2: service intent for the bundled Flycast build that can
       route. Jawaka neither probes health nor decides per-game eligibility
       here; Flycast does both after loading its own per-game settings. */
    bool flycast_route_authorized =
        ra_account_authorized &&
        jw_flycast_ra_route_target_authorized(target->path, target->core_id,
                                              &target->standalone_policy,
                                              target->provider,
                                              ra_platform_dir);
    bool flycast_route_live =
        flycast_route_authorized && jw__raofflineproxy_service_live(state);
    if (flycast_route_authorized) {
        jw_log_info("RAOfflineProxy: Flycast route intent %s",
                    jw_flycast_ra_route_value(flycast_route_live));
    } else if (!target->standalone_policy.provider_bound &&
               target->standalone_policy.release ==
                   JW_STANDALONE_RELEASE_FLYCAST &&
               jw__raofflineproxy_service_live(state)) {
        jw_log_info("RAOfflineProxy: this Flycast build cannot route through "
                    "the offline service; achievements stay direct until "
                    "Flycast is updated");
    }

    if (direct_drm) {
        jw_log_info("direct DRM handoff requested for core=%s rom=%s",
                    target->core_id, rom_abs);
        jw__stop_osd_child(state);
        int stop_rc = jw_weston_initd_run(JW_WESTON_INITD("stop") " </dev/null >/dev/null 2>&1; "
                                          "for i in 1 2 3 4 5 6 7 8 9 10; do "
                                          "pidof weston >/dev/null 2>&1 || exit 0; sleep .1; "
                                          "done; exit 0");
        if (stop_rc == -1) {
            jw_log_warn("direct DRM handoff: could not invoke Weston stop");
        }
        state->direct_drm_active = true;
        state->direct_drm_weston_stopped = true;
    }

    /* Hand the motor over exactly as the RetroArch path does. The quiesce was
       previously inside the direct-DRM branch, so a standalone emulator that
       does not take the display could inherit a UI pulse still in flight. */
    jw__rumble_quiesce();
    jw_rumble_game_env rumble_env;
    jw__rumble_resolve_game_env(state, &rumble_env);
    /* Open the force-feedback route only for emulators that actually run on the
       calibrated virtual pad, because that pad is the only thing carrying force
       feedback. The others read the physical pad, where there is nothing to
       carry it -- opening the route for them would leave the daemon reporting a
       live rumble session that no effect can ever reach, and even the
       "route is closed" diagnostic would stay silent. */
    if (jw__standalone_target_uses_calibrated_virtual_input(target)) {
        jw__rumble_publish_ff(&rumble_env);
    } else {
        jw__rumble_publish_ff(NULL);
        jw_log_info("rumble: %s reads the physical pad, which has no force "
                    "feedback; no game rumble this session", target->core_id);
    }

    /* Isolation handshake pipes (see the RetroArch path). */
    int sync_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (use_roster &&
        (jw__pipe_cloexec(sync_pipe) != 0 || jw__pipe_cloexec(err_pipe) != 0)) {
        state->launch_input_failed = true;
        jw_log_error("standalone launch blocked: %s: control pipe creation failed",
                     JW_INPUT_ROSTER_ERR_NAMESPACE);
        jw__rumble_publish_ff(NULL);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        return -1;
    }

    jw__set_session_menu(state, target->standalone_policy.provider_bound &&
                                !target->native_pico8);
    jw__heal_stdio_before_game();
    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        jw__rumble_publish_ff(NULL);
        if (sync_pipe[0] >= 0) close(sync_pipe[0]);
        if (sync_pipe[1] >= 0) close(sync_pipe[1]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        if (err_pipe[1] >= 0) close(err_pipe[1]);
        if (direct_drm) {
            state->direct_drm_active = false;
            if (state->direct_drm_weston_stopped) {
                (void)jw_weston_initd_run(JW_WESTON_INITD("start") " </dev/null >/dev/null 2>&1");
                state->direct_drm_weston_stopped = false;
                jw__spawn_osd(state);
            }
        }
        jw_input_proxy_shutdown(&state->input_proxy);  /* drop the watch-only proxy */
        jw__start_input_proxy(state);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        jw__set_session_menu(state, false);
        return -1;
    }

    if (pid == 0) {
        if (use_roster) {
            close(sync_pipe[1]);
            close(err_pipe[0]);
            /* Any failure here exits 126 without exec: fail closed. */
            jw__input_isolation_child_enter(&roster, sdl_devices,
                                            sync_pipe[0], err_pipe[1]);
        }
        if (jw__game_child_set_own_group() != 0) {
            perror("setpgid");
            _exit(126);
        }
        jw_appearance_apply_env(&appearance);
        jw__publish_source_content_env(rom_source);
        jw_pico8_export(target->native_pico8 ? &pico8_paths : NULL,
                        target->native_pico8 && use_roster);
        /* Standalone children never inherit the RetroArch credential channel
           (JAWAKA_CHEEVOS_* is RetroArch's own per-launch config handoff) or
           a stale account snapshot; only an authorized target receives the
           fresh snapshot, applied over a cleared set. */
        jw_ra_account_prepare_standalone_env(&ra_account, ra_account_authorized);
        jw__flycast_route_clear_env();
        if (flycast_route_authorized) {
            setenv(JW_FLYCAST_RA_ROUTE_ENV,
                   jw_flycast_ra_route_value(flycast_route_live), 1);
        }
        setenv("JAWAKA_GAME_SYSTEM", state->pending_launch_system, 1);
        setenv("JAWAKA_GAME_ROM", state->pending_launch_rom_path, 1);
        setenv("JAWAKA_GAME_ROM_ABS", rom_abs, 1);
        setenv("JAWAKA_GAME_CORE_ID", target->core_id, 1);
        /* The Saturn BIOS contract with Yabasanshiro-standalone's launch.sh.
           Always written here, never in the daemon's own environment: an
           unrelated ambient override must not survive into a game, and a
           non-Saturn child must not inherit a resolved Saturn choice. */
        if (bios.applies && bios.resolution.choice.kind == JW_BIOS_CHOICE_FILE) {
            setenv("YABASANSHIRO_BIOS_MODE", "external", 1);
            setenv("YABASANSHIRO_BIOS_FILE", bios.abs_path, 1);
        } else if (bios.applies) {
            setenv("YABASANSHIRO_BIOS_MODE", "hle", 1);
            unsetenv("YABASANSHIRO_BIOS_FILE");
        } else {
            unsetenv("YABASANSHIRO_BIOS_MODE");
            unsetenv("YABASANSHIRO_BIOS_FILE");
        }
        if (direct_drm) {
            setenv("JAWAKA_DIRECT_DRM", "1", 1);
        }
        if (direct_drm && jw__standalone_target_is_ports(target)) {
            setenv("LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE", "1", 1);
            setenv("LEAF_PM_GOTHIC_MACHISMO_VULKAN_ROTATE_STOP_DISPLAY", "0", 1);
        }
        if (have_standalone_resume) {
            char slot_env[16];
            snprintf(slot_env, sizeof(slot_env), "%d", standalone_resume_slot);
            setenv("EMU_RESUME_SLOT", slot_env, 1);
        } else {
            unsetenv("EMU_RESUME_SLOT");
        }

        char *const argv[] = {
            (char *)target->path,
            rom_abs,
            NULL,
        };
        execv(target->path, argv);
        perror("execv");
        _exit(127);
    }

    if (use_roster) {
        close(sync_pipe[0]);
        close(err_pipe[1]);
        char isolation_error[256];
        int isolation_rc = jw__input_isolation_parent_finish(
            state, &roster, pid, sync_pipe[1], err_pipe[0],
            isolation_error, sizeof(isolation_error));
        close(sync_pipe[1]);
        close(err_pipe[0]);
        if (isolation_rc != 0) {
            jw_log_error("standalone launch blocked: %s", isolation_error);
            jw__rumble_publish_ff(NULL);
            /* The direct-DRM handoff already stopped Weston pre-fork; undo it
               so the user gets a desktop back on this blocked launch. */
            if (direct_drm) {
                state->direct_drm_active = false;
                if (state->direct_drm_weston_stopped) {
                    (void)jw_weston_initd_run(JW_WESTON_INITD("start") " </dev/null >/dev/null 2>&1");
                    state->direct_drm_weston_stopped = false;
                    jw__spawn_osd(state);
                }
            }
            state->pending_launch = false;
            state->pending_launch_resume_switcher = false;
            state->pending_launch_override_unverified = false;
            jw__set_session_menu(state, false);
            return -1;
        }
    }

    if (!jw__reserve_game_process_group(pid)) {
        jw_log_error("could not reserve standalone writer process group pid=%d: %s",
                     (int)pid, strerror(errno));
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        jw__rumble_publish_ff(NULL);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        jw__set_session_menu(state, false);
        return -1;
    }
    state->child_pid = pid;
    state->child_pgid = pid;
    state->child_kind = JW_CHILD_EMULATOR;
    state->pending_launch = false;
    state->pending_launch_resume_switcher = false;
    state->post_launch_resume_pending = false;
    state->post_launch_resume_attempts = 0;
    state->post_launch_resume_next_ms = 0;
    jw_log_info("spawned standalone emulator pid=%d launcher=%s core_id=%s",
                (int)pid, target->path,
                target->core_id[0] ? target->core_id : "(unknown)");
    jw__standalone_session_start(state, pid, state->pending_launch_game_id,
                                 state->pending_launch_system, rom_abs,
                                 state->pending_launch_rom_path, source_root,
                                 target);
    return 0;
}

static int jw__spawn_authorized_pending_game(jw_daemon_state *state) {
    if (!state || !state->pending_launch) {
        return -1;
    }

    jw_launch_target target;
    if (jw__pending_launch_target(state, &target) != 0) {
        jw_log_error("could not use launch target for system=%s rom=%s",
                     state->pending_launch_system,
                     state->pending_launch_rom_path);
        state->pending_launch = false;
        jw__pending_launch_forget_target(state);
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        return -1;
    }

    int rc = target.kind == JW_LAUNCH_TARGET_STANDALONE
                 ? jw__spawn_standalone_emulator(state, &target)
                 : jw__spawn_retroarch(state, &target);
    if (rc != 0) {
        /* Both spawns apply the game's governor profile before the child runs,
           so a handoff that never starts a writer would otherwise leave the
           device in the game's mode with the launcher (or a still-running
           previous game) on screen. Restore what the current context actually
           asks for: a live session keeps its own profile, everything else
           returns to the frontend profile. */
        (void)jw__perf_apply_current_context(state, "launch-failed");
    }
    /* Started or failed, this launch is over. */
    jw__pending_launch_forget_target(state);
    return rc;
}

/* Defined below with the rest of the LIFE-1 coordination machinery; the
   RAOfflineProxy gate arms the same fail-closed dialog channel. */
static void jw__game_coordination_block(jw_daemon_state *state,
                                        const char *service_id,
                                        const char *reason);

/* -- RAOfflineProxy transient launch bridge ---------------------------------
   The library game-launch path is the only site that waits on, injects for,
   or prompts about the offline-achievements proxy (the RetroArch pak runner
   launches --menu with no content and never touches this). One bounded
   routing decision per launch; no new generic service contract. */
typedef enum {
    JW__ROP_GATE_DIRECT = 0,
    JW__ROP_GATE_PROXIED,
    JW__ROP_GATE_BLOCKED,
} jw__rop_gate_result;

static jw__rop_gate_result jw__raofflineproxy_route(
    jw_daemon_state *state, jw_retroarch_launch_snapshot *snapshot) {
    jw_retroarch_launch_snapshot_init(snapshot);
    if (!state) {
        return JW__ROP_GATE_DIRECT;
    }
    /* The launcher's Play Anyway answer is honored exactly once: direct
       play, no further wait or prompt for this pending launch. */
    if (state->pending_launch_override_unverified) {
        return JW__ROP_GATE_DIRECT;
    }
    const jw_svc_supervised *entry =
        state->services
            ? jw_svc_supervisor_find(state->services, JW_ROP_SERVICE_ID)
            : NULL;
    /* Route on what the service IS DOING, not on either intent flag.
       desired_enabled ("Start with Leaf") and session_run ("Run") are
       independent, and neither alone identifies a usable proxy:

         - session_run is set by the Run op and by restart, but NOT by
           autostart (supervisor.c's autostart tick calls jw__start_generation
           and persists "autostart" without touching it), so gating on it
           ignores a service the user enabled and the daemon started at boot --
           the common case.
         - desired_enabled is a durable preference that says nothing about
           whether a process exists right now, so gating on it would wait on a
           service the user has explicitly stopped.

       A live pgid in RUNNING or STARTING is the only thing that means "there
       may be a proxy to talk to", and it is true however the service got
       there. Everything else is direct with zero wait, which is exactly what
       the plan asks for: absent, invalid, disabled, or session-stopped. */
    if (!jw__raofflineproxy_entry_live(entry)) {
        jw_log_info("RAOfflineProxy: direct launch (service %s)",
                    !entry ? "absent"
                    : !entry->pak_present || !entry->manifest_valid
                        ? "invalid"
                    : entry->desired_enabled || entry->session_run
                        ? "not running"
                        : "not enabled");
        return JW__ROP_GATE_DIRECT;
    }
    /* A durable Hardcore setting selects direct play and never routes
       through the proxy (upstream cannot award offline Hardcore). */
    if (jw_retroarch_shared_hardcore_enabled(state->sdcard_root)) {
        jw_log_info("RAOfflineProxy: direct launch (durable Hardcore)");
        return JW__ROP_GATE_DIRECT;
    }
    /* The user intends the service to run. Decide within one bounded window
       (500 ms total): a running, healthy service routes through the proxy;
       anything else earns the explicit bypass prompt. */
    long long deadline_ms = jw__monotonic_ms() + JW_ROP_ROUTING_BUDGET_MS;
    for (;;) {
        if (entry->state == JW_SVC_STATE_RUNNING && entry->pgid > 0) {
            int remaining_ms = (int)(deadline_ms - jw__monotonic_ms());
            if (remaining_ms <= 0) {
                break;
            }
            if (jw_raofflineproxy_health_ready(JW_ROP_HEALTH_HOST,
                                               JW_ROP_HEALTH_PORT,
                                               remaining_ms)) {
                /* The snapshot is what restores the user's durable cheevos
                 * lines on exit. If the shared config exists but cannot be
                 * read there is nothing to restore from, so proxying would
                 * mean overriding settings we could never put back. Absent is
                 * different and fine: there are no durable lines to lose. */
                jw_shared_config_status shared_status = JW_SHARED_CFG_UNREADABLE;
                char *shared_text = jw_retroarch_shared_config_read_status(
                    state->sdcard_root, &shared_status);
                if (shared_text) {
                    jw_retroarch_launch_snapshot_capture(snapshot, shared_text);
                    free(shared_text);
                } else if (shared_status == JW_SHARED_CFG_UNREADABLE) {
                    jw_log_warn("RAOfflineProxy: shared config unreadable; "
                                "direct launch (cannot restore on exit)");
                    return JW__ROP_GATE_DIRECT;
                }
                snapshot->proxied = true;
                jw_log_info("RAOfflineProxy: service healthy; proxied launch");
                return JW__ROP_GATE_PROXIED;
            }
        }
        if (jw__monotonic_ms() >= deadline_ms) {
            break;
        }
        jw__usleep(100 * 1000);
    }
    jw_log_info("RAOfflineProxy: service not ready after %dms; blocking launch",
                JW_ROP_ROUTING_BUDGET_MS);
    jw__game_coordination_block(state, JW_ROP_SERVICE_ID,
                                "raofflineproxy-not-ready");
    return JW__ROP_GATE_BLOCKED;
}

static int jw__spawn_retroarch(jw_daemon_state *state,
                               const jw_launch_target *target) {
    if (!state || !state->pending_launch || !target ||
        target->kind != JW_LAUNCH_TARGET_RETROARCH) {
        return -1;
    }
    long long launch_start_ms = jw__monotonic_ms();
    bool switcher_resume = state->pending_launch_resume_switcher;

    char rom_abs[PATH_MAX];
    jw_game_entry launch_game;
    jw_storage_source_list sources;
    const jw_storage_source *rom_source = NULL;
    if (jw__resolve_library_game(state, state->pending_launch_game_id,
                                 &launch_game, &sources, &rom_source,
                                 rom_abs, sizeof(rom_abs)) != 0) {
        jw_log_error("could not resolve stable game id=%d",
                     state->pending_launch_game_id);
        state->pending_launch = false;
        state->pending_launch_resume_switcher = false;
        return -1;
    }
    char source_root[PATH_MAX];
    if (rom_source) {
        snprintf(source_root, sizeof(source_root), "%s", rom_source->root);
    } else {
        snprintf(source_root, sizeof(source_root), "%s", state->sdcard_root);
    }

    /* A pak core with a LEAF-RA-1 frontend runs in the pak's own RetroArch;
       everything else in this launch is the same as for Leaf's. */
    bool frontend = target->frontend[0] != '\0';
    char *retroarch = frontend ? strdup(target->frontend) : jw_retroarch_bin_path();
    /* The resolved target is this launch's only core selection. No second
       resolver runs here, so a core file that disappeared since selection
       fails the launch below instead of being replaced by another core. */
    char core_id[64];
    char core_config_folder[256];
    char info_dir[PATH_MAX];
    char launch_warning[256];
    launch_warning[0] = '\0';
    snprintf(core_id, sizeof(core_id), "%s", target->core_id);
    snprintf(core_config_folder, sizeof(core_config_folder), "%s",
             target->core_config_folder);
    snprintf(info_dir, sizeof(info_dir), "%s", target->info_dir);
    char *core = strdup(target->path);
    char *runtime_config = NULL;
    char *ra_home = NULL;

    jw__publish_retroarch_input_env(state);
    jw__reconcile_audio(state, "retroarch-launch", false);
    jw__publish_audio_env(state);
    bool audio_bluetooth =
        strcmp(getenv("JAWAKA_AUDIO_OUTPUT") ? getenv("JAWAKA_AUDIO_OUTPUT") : "",
               "BLUETOOTH") == 0;
    jw__publish_display_env(state);

    /* Resolve cheevos creds here (SQLite open is not fork-safe between fork and
       execv). They are applied to the parent env only briefly around the config
       write below — jw_prepare_retroarch_config reads JAWAKA_CHEEVOS_* to bake
       cheevos_username/password into the per-launch config — then cleared, so the
       plaintext password reaches the RA config but never persists in the daemon
       env or leaks to later children. */
    jw_cheevos_creds cheevos;
    jw__cheevos_resolve(state, &cheevos);

    if (!retroarch || !jw__path_exists(retroarch)) {
        jw_log_error("RetroArch binary missing: %s", retroarch ? retroarch : "(null)");
        goto fail;
    }
    if (!core || !jw__path_exists(core)) {
        jw_log_error("libretro core missing for %s: %s",
                     state->pending_launch_system, core ? core : "(null)");
        goto fail;
    }

    char exec_error[256];
    if (!jw_sdcard_exec_available_for_path(core, exec_error, sizeof(exec_error))) {
        jw_log_error("cannot launch RetroArch core from SD: %s", exec_error);
        goto fail;
    }
    if (frontend) {
        if (!jw_sdcard_exec_available_for_path(retroarch, exec_error, sizeof(exec_error))) {
            jw_log_error("cannot launch RetroArch frontend from SD: %s", exec_error);
            goto fail;
        }
        jw_log_info("RetroArch frontend for core %s: %s", core_id, retroarch);
    }

    /* A boot resume starts only the game, core and card the record named and
       startup validated. Anything that moved since is a failed resume, never
       a substitute. */
    bool boot_resume = state->boot_resume.phase == JW_BOOT_RESUME_PHASE_LAUNCHING &&
                       state->boot_resume.game_id == state->pending_launch_game_id;
    if (boot_resume &&
        !jw__boot_resume_spawn_matches(state, rom_source, target, rom_abs)) {
        goto fail;
    }

    /* RAOfflineProxy routing ran earlier (jw__spawn_pending_game, where the
       other fail-closed blocks live); this launch carries its outcome. A
       BLOCKED result never reaches this function. */
    jw_retroarch_launch_snapshot rop_snapshot = state->pending_rop_snapshot;

    jw__recover_legacy_flat(state, state->pending_launch_system, rom_abs,
                            source_root, core_id, core_config_folder,
                            launch_warning, sizeof(launch_warning));

    jw_platform_result ready_result;
    jw_platform_frontend_ready(&state->platform, "launcher", &ready_result);
    jw_log_info("RetroArch launch transition readiness code=%s",
                jw_platform_result_code_name(ready_result.code));

    /* Frozen controller roster for this launch (paired wireless controllers).
       Device builds fail closed when the roster cannot be built; desktop
       launches keep the legacy single-player direct-input path. */
    jw_input_roster roster;
    char sdl_devices[JW_INPUT_ISOLATION_SDL_MAX];
    sdl_devices[0] = '\0';
    int player_indices[4] = { -1, -1, -1, -1 };
    const int *player_indices_arg = NULL;
    bool use_roster = false;
    {
        char roster_error[256];
        int roster_rc = jw__launch_prepare_input_roster(
            state, &roster, sdl_devices, sizeof(sdl_devices), player_indices,
            "retroarch", roster_error, sizeof(roster_error));
        if (roster_rc < 0) {
            jw_log_error("RetroArch launch blocked: %s", roster_error);
            goto fail;
        }
        if (roster_rc == 1) {
            use_roster = true;
            player_indices_arg = player_indices;
        }
    }
    if (!use_roster) {
        /* Legacy single-controller path (desktop builds without a roster). */
        int player1_joypad_index = jw_input_proxy_retroarch_joypad_index(&state->input_proxy);
        if (state->input_proxy.enabled && state->input_proxy.virtual_event_path[0]) {
            if (player1_joypad_index >= 0) {
                jw_log_info("RetroArch input proxy: physical=%s virtual=%s joypad_index=%d",
                            state->input_proxy.physical_event_path[0]
                                ? state->input_proxy.physical_event_path
                                : "(unknown)",
                            state->input_proxy.virtual_event_path,
                            player1_joypad_index);
            } else {
                jw_log_warn("RetroArch input proxy: could not resolve virtual joypad index for %s",
                            state->input_proxy.virtual_event_path);
            }
        }
        if (player1_joypad_index >= 0) {
            player_indices[0] = player1_joypad_index;
            player_indices_arg = player_indices;
        }
    }

    jw_saved_env storage_env[] = {
        { .name = NULL, .value = NULL, .had_value = false },
        { .name = NULL, .value = NULL, .had_value = false },
        { .name = NULL, .value = NULL, .had_value = false },
    };
    if (rom_source) {
        jw__save_env(&storage_env[0], "BIOS_PATH");
        jw__save_env(&storage_env[1], "SAVES_PATH");
        jw__save_env(&storage_env[2], "STATES_PATH");
        jw__publish_retroarch_source_dirs(rom_source,
                                          state->pending_launch_system);
    }

    char config_error[256];
    /* Persist RetroArch config changes on a clean exit regardless of how the game
       was launched. Previously resume launches (Recents/Switcher) used
       !switcher_resume = false, so a global RA setting changed while playing a
       resumed game was silently dropped on quit, while the same change from a cold
       Games-tab launch stuck. Per-game/core options already persist via RA's durable
       HOME; this makes global settings consistent too. (The rapid switcher-hop case
       is still excluded separately via switcher_transition at the write-back.) */
    bool persist_config = true;
    long long config_start_ms = jw__monotonic_ms();
    /* Bake cheevos creds into the config write, then clear them from the parent
       env immediately so the plaintext password never persists in the daemon. */
    jw__cheevos_apply_env(&cheevos);
    runtime_config = jw_prepare_retroarch_config_with_info(
        state->runtime_dir, source_root, core, info_dir,
        player_indices_arg, persist_config, rop_snapshot.proxied,
        frontend ? &jw_retroarch_frontend_profile_leaf_ra_1 : NULL,
        config_error, sizeof(config_error));
    jw__cheevos_clear_env();
    jw__restore_env(storage_env, 3);
    if (!runtime_config) {
        jw_log_error("could not prepare RetroArch config: %s",
                     config_error[0] ? config_error : "unknown error");
        goto fail;
    }
    /* The two transient keys are written by jw_prepare_retroarch_config
       itself, which also strips any merged-in copies so RetroArch's
       first-occurrence-wins parser cannot silently prefer a shared value.
       A failed write already aborted the launch above. */
    if (rop_snapshot.proxied) {
        jw_log_info("RAOfflineProxy: proxied launch (transient cheevos "
                    "host + forced casual override)");
    }
    long long config_done_ms = jw__monotonic_ms();

    bool entryslot_resume = false;
    int entryslot = JW_RA_GAME_SWITCHER_STATE_SLOT;
    char entryslot_arg[16];
    char entry_state_path[PATH_MAX];
    entryslot_arg[0] = '\0';
    entry_state_path[0] = '\0';
    long long state_resolve_start_ms = jw__monotonic_ms();
    if (switcher_resume) {
        char states_dir[PATH_MAX];
        int resolved_slot = 0;
        char resolved_path[PATH_MAX];
        resolved_path[0] = '\0';
        if (snprintf(states_dir, sizeof(states_dir), "%s/States", source_root) <
                (int)sizeof(states_dir) &&
            jw_ra_find_resume_state_for_core(
                states_dir, core_config_folder, rom_abs,
                JW_RA_GAME_SWITCHER_STATE_SLOT, &resolved_slot,
                resolved_path, sizeof(resolved_path))) {
            /* Resume via the post-launch network load-state command (below), NOT
               RetroArch's --entryslot. RA 1.22.2's --entryslot resolves a separate
               "<rom>.stateN.entry" file and does not reliably load our regular
               savestate when sort_savestates sorts states into per-core subfolders
               (verified on device: -e <slot> with a valid .state99 on disk still
               cold-started). The command path (jw_ra_load_state_slot) uses RA's
               normal slot loader, which honors the subfolder. Leave entryslot_resume
               false so post_launch_resume drives the load. */
            entryslot = resolved_slot;
            snprintf(entry_state_path, sizeof(entry_state_path), "%s", resolved_path);
            jw_log_info("switcher resume: state slot=%d path=%s (post-launch load)",
                        entryslot, entry_state_path);
        } else {
            jw_log_info("switcher resume: no prelaunch state found for %s", rom_abs);
        }
    }
    long long state_resolve_done_ms = jw__monotonic_ms();

    (void)jw__perf_apply_launch_game(state, state->pending_launch_system,
                                     state->pending_launch_rom_path,
                                     "retroarch-launch");

    /* RetroArch resolves its config dir / per-core option files (e.g.
       FCEUmm.opt) under $HOME. Point HOME at the SD state dir the runner uses so
       all RA config lives on the SD card jawaka launched from, not on device
       internal storage. Computed here (it mkdir's) and applied in the child only
       — setting it on the daemon parent would change HOME for every later child.
       Uses sdcard_root so all games share one canonical RA config location. */
    ra_home = jw_retroarch_state_dir(state->sdcard_root);
    if (!ra_home) {
        jw_log_warn("could not resolve RA HOME (SD state dir); RetroArch config "
                    "may fall back to device internal storage");
    }

    /* Pin the emulated pad for cores that need one the user's hardware expects
       (PS1 wants a DualShock, or nothing can rumble). Must happen before the
       fork: RetroArch reads the remap during startup. */
    jw_retroarch_pin_core_device(ra_home, core_id, core_config_folder, rom_abs);

    /* Seed the tuned option profile for cores that ship one, before RetroArch
       initializes the core. Failing here is deliberate: a launch without the
       installed profile would silently run the new core on stock defaults, so
       "the profile is not on disk" has to be a failure rather than a quiet
       downgrade. Already-seeded installs (and every other core) return without
       touching anything. */
    {
        char seed_error[256];
        if (jw_retroarch_seed_core_options(ra_home, state->sdcard_root, core_id,
                                           core_config_folder, seed_error,
                                           sizeof(seed_error)) != 0) {
            jw_log_error("could not install the core option profile for %s: %s",
                         core_id, seed_error[0] ? seed_error : "unknown error");
            goto fail;
        }
    }

    /* Stop any UI pulse before the handoff, then resolve the duty endpoints
       the game will drive the motor with (if game rumble is on). A bare
       force-off here was routinely undone: the launcher fires its commit tick
       (280 ms) immediately before this IPC, so the worker re-energised the
       motor a moment later. */
    jw__rumble_quiesce();
    jw_rumble_game_env rumble_env;
    jw__rumble_resolve_game_env(state, &rumble_env);
    /* RetroArch takes the same force-feedback route as everything else: its
       SDL2 joypad driver finds FF_RUMBLE on the virtual pad and rumbles through
       it. This is the only route now, so without it RetroArch is silent. */
    jw__rumble_publish_ff(&rumble_env);

    /* Isolation handshake pipes: parent -> child carries the staged namespace
       dir; child -> parent (CLOEXEC) reports pre-exec isolation failures and
       closes silently at exec. */
    int sync_pipe[2] = { -1, -1 };
    int err_pipe[2] = { -1, -1 };
    if (use_roster &&
        (jw__pipe_cloexec(sync_pipe) != 0 || jw__pipe_cloexec(err_pipe) != 0)) {
        state->launch_input_failed = true;
        jw_log_error("RetroArch launch blocked: %s: control pipe creation failed",
                     JW_INPUT_ROSTER_ERR_NAMESPACE);
        goto fail;
    }

    jw__heal_stdio_before_game();
    long long fork_start_ms = jw__monotonic_ms();
    pid_t pid = fork();
    if (pid < 0) {
        jw_log_error("fork failed: %s", strerror(errno));
        jw__rumble_publish_ff(NULL);
        if (sync_pipe[0] >= 0) close(sync_pipe[0]);
        if (sync_pipe[1] >= 0) close(sync_pipe[1]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        if (err_pipe[1] >= 0) close(err_pipe[1]);
        goto fail;
    }

    if (pid == 0) {
        if (use_roster) {
            close(sync_pipe[1]);
            close(err_pipe[0]);
            /* Any failure here exits 126 without exec: fail closed. */
            jw__input_isolation_child_enter(&roster, sdl_devices,
                                            sync_pipe[0], err_pipe[1]);
        }
        if (jw__game_child_set_own_group() != 0) {
            perror("setpgid");
            _exit(126);
        }
        if (ra_home && ra_home[0]) {
            setenv("HOME", ra_home, 1);
        }
        char *argv[9];
        int argc = 0;
        argv[argc++] = retroarch;
        argv[argc++] = (char *)"-L";
        argv[argc++] = core;
        argv[argc++] = (char *)"--config";
        argv[argc++] = runtime_config;
        if (entryslot_resume) {
            argv[argc++] = (char *)"-e";
            argv[argc++] = entryslot_arg;
        }
        argv[argc++] = rom_abs;
        argv[argc] = NULL;
        execv(retroarch, argv);
        perror("execv");
        _exit(127);
    }
    long long fork_done_ms = jw__monotonic_ms();

    if (use_roster) {
        close(sync_pipe[0]);
        close(err_pipe[1]);
        char isolation_error[256];
        int isolation_rc = jw__input_isolation_parent_finish(
            state, &roster, pid, sync_pipe[1], err_pipe[0],
            isolation_error, sizeof(isolation_error));
        close(sync_pipe[1]);
        close(err_pipe[0]);
        if (isolation_rc != 0) {
            jw_log_error("RetroArch launch blocked: %s", isolation_error);
            jw__rumble_publish_ff(NULL);
            goto fail;
        }
    }

    if (!jw__reserve_game_process_group(pid)) {
        jw_log_error("could not reserve RetroArch writer process group pid=%d: %s",
                     (int)pid, strerror(errno));
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        jw__rumble_publish_ff(NULL);
        goto fail;
    }
    state->child_pid = pid;
    state->child_pgid = pid;
    state->child_kind = JW_CHILD_RETROARCH;
    state->pending_launch = false;
    state->pending_launch_resume_switcher = false;
    jw_log_info("spawned RetroArch pid=%d retroarch=%s", (int)pid, retroarch);
    jw__retroarch_session_start(state, pid, state->pending_launch_game_id,
                                state->pending_launch_system, rom_abs,
                                state->pending_launch_rom_path, source_root,
                                core, core_id, core_config_folder,
                                runtime_config, persist_config,
                                audio_bluetooth && !frontend, launch_warning);
    state->retroarch_session.config_snapshot = rop_snapshot;
    if (frontend) {
        /* Until the probe hears GET_INFO, Leaf assumes nothing about the
           frontend's command set (see jw__tick_retroarch_frontend_probe). */
        state->retroarch_session.frontend = true;
        state->retroarch_session.frontend_probe_next_ms =
            jw__monotonic_ms() + JW_FRONTEND_PROBE_FIRST_MS;
    }
    jw__retroarch_session_pin_resume(state, rom_source, launch_game.rom_relpath,
                                     target->provider);
    if (boot_resume) {
        state->retroarch_session.resume_unconfirmed = true;
        state->boot_resume.phase = JW_BOOT_RESUME_PHASE_PROBING;
        state->boot_resume.attempts = 0;
        state->boot_resume.next_ms = jw__monotonic_ms();
        jw_log_info("boot resume: RetroArch pid=%d started; waiting for its command port",
                    (int)pid);
    }
    bool post_launch_resume = switcher_resume && !entryslot_resume &&
                              core_config_folder[0];
    if (post_launch_resume) {
        state->post_launch_resume_pending = true;
        state->post_launch_resume_attempts = 0;
        state->post_launch_resume_next_ms = jw__monotonic_ms();
    } else {
        state->post_launch_resume_pending = false;
        state->post_launch_resume_attempts = 0;
        state->post_launch_resume_next_ms = 0;
    }

    /* Let RetroArch own the first startup window before cold-starting the
       hidden standby menu's SDL/GL/input stack. A resume, either kind,
       schedules it once its state is in. */
    if (!post_launch_resume && !boot_resume) {
        jw__schedule_in_game_menu_prewarm(state, JW_INGAME_MENU_PREWARM_DELAY_MS);
    }
    jw_log_info("RetroArch launch timings: total_ms=%lld config_ms=%lld state_resolve_ms=%lld fork_ms=%lld entryslot=%s post_resume=%s",
                fork_done_ms - launch_start_ms,
                config_done_ms - config_start_ms,
                state_resolve_done_ms - state_resolve_start_ms,
                fork_done_ms - fork_start_ms,
                entryslot_resume ? entryslot_arg : "none",
                post_launch_resume ? "true" : "false");

    free(retroarch);
    free(core);
    free(runtime_config);
    free(ra_home);
    return 0;

fail:
    state->pending_launch = false;
    state->pending_launch_resume_switcher = false;
    state->post_launch_resume_pending = false;
    jw__cancel_in_game_menu_prewarm(state);
    jw__retroarch_session_clear(&state->retroarch_session);
    free(retroarch);
    free(core);
    free(runtime_config);
    free(ra_home);
    return -1;
}

/* ── Auto-sleep ───────────────────────────────────────────────────────────
   Idle (no button input, tracked globally by the input proxy so it covers games
   too) → blank the screen → suspend-to-RAM. Tiered: the screen-off stage wakes
   on any button; the suspend stage wakes on the power button. */
#define JW_AUTOSLEEP_DEFAULT_S        0       /* Off when the setting is unset.
                                                 Deep-suspend wake is not yet
                                                 reliable; auto-sleep is opt-in. */
#define JW_AUTOSLEEP_SUSPEND_GRACE_MS 30000   /* screen-off → suspend after this much more idle */
#define JW_AUTOSLEEP_SETTING_POLL_MS  2000    /* re-read the DB setting at most this often */
#define JW_CHARGING_STATUS_POLL_MS    1000    /* while in standby, notice unplug promptly */
#define JW_POWER_LONGPRESS_MS         2000    /* power held this long → clean power off (before
                                                 the PMIC hard-cut: 6 s stock, 10 s by default
                                                 once jw__power_hard_cut_sync_platform ran) */

static int jw__autosleep_read_timeout_s(jw_daemon_state *state) {
    if (!state->db_path) {
        return JW_AUTOSLEEP_DEFAULT_S;
    }
    char val[32];
    if (jw_db_get_setting(state->db_path, "auto_sleep_seconds", val, sizeof(val)) == 0 &&
        val[0]) {
        int seconds = atoi(val);
        return seconds > 0 ? seconds : 0;   /* 0 = explicitly off */
    }
    return JW_AUTOSLEEP_DEFAULT_S;
}

/* Force Off Hold from the DB; a missing or invalid value is the 10 s default
   (plan: power-button-hard-cut-setting.md), never "leave the register alone". */
static int jw__power_hard_cut_read_s(jw_daemon_state *state) {
    if (!state->db_path) {
        return JW_POWER_HARD_CUT_DEFAULT_S;
    }
    char val[32];
    if (jw_db_get_setting(state->db_path, JW_POWER_HARD_CUT_SETTING_KEY, val, sizeof(val)) != 0) {
        val[0] = '\0';
    }
    return jw_power_hard_cut_parse(val);
}

/* Apply the desired hold to the PMIC when it differs from what was last handed
   over, or on a forced pass (start-up and wake: U-Boot resets the register on
   every boot, and a re-apply that finds it unchanged is a read and no write).
   Called only from the settings poll in jw__tick_auto_sleep, never from the
   power-key edge handling: a hold in progress must not wait on I2C.
   The backend logs the change, or the one warning when the bus or read-back
   fails; a failure is not retried until the value changes or the next forced
   pass, so a missing bus costs one line per start, not one per poll. */
static void jw__power_hard_cut_sync_platform(jw_daemon_state *state, bool forced) {
    if (!state) return;
    int desired = state->power_hard_cut_desired_s;
    if (!forced && desired == state->power_hard_cut_applied_s) return;

    jw_platform_result result;
    jw_platform_set_power_hard_cut_s(&state->platform, desired, &result);
    state->power_hard_cut_applied_s = desired;
    switch (result.code) {
        case JW_PLATFORM_RESULT_OK:
            state->power_hard_cut_effective_s = result.has_value ? result.value : desired;
            break;
        case JW_PLATFORM_RESULT_UNSUPPORTED:
            state->power_hard_cut_effective_s = 0;   /* not an MLP1: nothing to apply */
            break;
        default:
            /* Read or read-back failed and the register was left alone: assume
               U-Boot's value, the shortest, so nothing counts on a longer hold. */
            state->power_hard_cut_effective_s = JW_POWER_HARD_CUT_STOCK_S;
            break;
    }
#ifndef PLATFORM_MLP1
    /* Host smoke only: the mock platform applies nothing, which the policy
       reads as the stock 6 s cut and declines at once. */
    if (state->test_power_hard_cut_s > 0) {
        state->power_hard_cut_effective_s = state->test_power_hard_cut_s;
    }
#endif
}

static void jw__screen_set(jw_daemon_state *state, bool on) {
    jw_platform_result result;
    jw_platform_perform_action(&state->platform,
                               on ? JW_PLATFORM_ACTION_SCREEN_ON
                                  : JW_PLATFORM_ACTION_SCREEN_OFF,
                               0, &result);
}

static bool jw__standby_active(const jw_daemon_state *state) {
    return state && state->standby_reason != JW_STANDBY_NONE;
}

static bool jw__charging_online(jw_daemon_state *state, bool force) {
    if (!state) {
        return false;
    }

    long long now = jw__monotonic_ms();
    if (!force && state->charging_cached >= 0 && now < state->charging_next_poll_ms) {
        return state->charging_cached == 1;
    }

    jw_platform_status status;
    jw_platform_get_status(&state->platform, &status);
    jw__cache_platform_status(state, &status);
    state->charging_cached = status.charging;
    state->charging_next_poll_ms = now + JW_CHARGING_STATUS_POLL_MS;
    return status.charging == 1;
}

static void jw__enter_standby_screen_off(jw_daemon_state *state,
                                         jw_standby_reason reason,
                                         bool reset_idle) {
    if (!state) {
        return;
    }

    /* A JW_SUSPEND_SCOPE_SCREEN lease defers the idle blank: a video player has
       no input for the length of a film, so input-idle is the wrong signal for
       it. Gated here rather than at each caller because jw__tick_auto_sleep
       blanks from three places and a fourth would silently miss the check.
       Only the idle path is deferred -- a power press or a charging standby is
       a deliberate act and still blanks. */
    if (reason == JW_STANDBY_AUTOSLEEP) {
        int screen_leases = jw_suspend_inhibitor_count_scope(
            &state->suspend_inhibitor, JW_SUSPEND_SCOPE_SCREEN);
        if (screen_leases > 0) {
            if (!state->autosleep_screen_inhibit_logged) {
                jw_log_info("auto-sleep: screen blank deferred by %d screen lease(s)",
                            screen_leases);
                state->autosleep_screen_inhibit_logged = true;
            }
            return;
        }
    }

    if (reset_idle) {
        jw_input_proxy_mark_activity(&state->input_proxy);
    }
    if (!jw__standby_active(state)) {
        jw__screen_set(state, false);
        /* Synthesize a release for anything still held on the virtual pad.
           Swallowing drops the physical release, and evdev is edge-based, so
           without this the launcher goes on auto-repeating a direction that is
           physically already up: the list scrolls invisibly behind a dark
           screen (buzzing, once haptics exist) and is still scrolling when you
           wake it. Same edge-based trap the in-game menu resume guards against.
           (Ordering vs set_swallow is irrelevant -- release_buttons writes
           straight to uinput and never consults the swallow flag.) */
        jw_input_proxy_release_buttons(&state->input_proxy);
        jw_input_proxy_set_swallow(&state->input_proxy, true);
        jw__rumble_set_gated(true);
        jw__rumble_quiesce();
    }
    state->standby_reason = reason;
    state->standby_entered_ms = jw__monotonic_ms();
    state->power_sleep_armed = false;
    state->autosleep_charging_logged = false;
}

static void jw__leave_standby_screen_off(jw_daemon_state *state) {
    if (!jw__standby_active(state)) {
        return;
    }

    jw__screen_set(state, true);
    jw_input_proxy_set_swallow(&state->input_proxy, false);
    jw__rumble_set_gated(false);
    state->standby_reason = JW_STANDBY_NONE;
    state->standby_entered_ms = 0;
    state->autosleep_charging_logged = false;
}

static bool jw__standby_has_wake_activity(jw_daemon_state *state) {
    if (!jw__standby_active(state) || state->standby_entered_ms <= 0) {
        return false;
    }

    long long now = jw__monotonic_ms();
    if (now <= state->standby_entered_ms) {
        return false;
    }
    uint64_t since_standby_ms = (uint64_t)(now - state->standby_entered_ms);
    uint64_t idle_ms = jw_input_proxy_idle_ms(&state->input_proxy);
    return idle_ms < since_standby_ms;
}

/* Keep STOCK loong_power's auto-sleep DISABLED. jawakad owns auto-sleep through the
   input proxy, which tracks the gamepad input our EVIOCGRAB hides from stock. If we
   mirrored our timeout into stock, stock would auto-suspend on its own timer that
   never sees button/d-pad presses, so the device would sleep regardless of input
   (the bug). We therefore always force stock to 0 and govern sleep ourselves. */
static void jw__autosleep_sync_platform(jw_daemon_state *state) {
    if (!state || state->autosleep_platform_synced_s == 0) {
        return;   /* stock auto-sleep already disabled */
    }

    jw_platform_result result;
    jw_platform_perform_action(&state->platform, JW_PLATFORM_ACTION_SET_AUTO_SLEEP,
                               0, &result);
    if (result.code == JW_PLATFORM_RESULT_OK) {
        jw_log_info("auto-sleep: stock power auto-sleep disabled (jawakad governs)");
    } else if (result.code != JW_PLATFORM_RESULT_UNSUPPORTED) {
        jw_log_warn("auto-sleep: disabling stock power auto-sleep failed: %s",
                    result.message[0] ? result.message
                                      : jw_platform_result_code_name(result.code));
        return;
    }
    state->autosleep_platform_synced_s = 0;
}

/* Real suspend-to-RAM and wake handling, shared by the auto-sleep timer and a power
   press. The proxy holds the power key exclusively, so loong_power can't re-suspend
   us on the wake press and the kernel wakes on a single press; we drop any input
   that queued while asleep (gamepad replay + the wake press itself) and reset the
   idle timer so the countdown restarts fresh after waking. */
static void jw__deep_suspend(jw_daemon_state *state) {
    int active_leases = jw_suspend_inhibitor_count(&state->suspend_inhibitor);
    if (active_leases > 0) {
        bool newly_pending = state->suspend_policy.pending == JW_SUSPEND_PENDING_NONE;
        if (newly_pending) state->suspend_policy.pending = JW_SUSPEND_PENDING_EXPLICIT;
        if (!jw__standby_active(state)) {
            jw__enter_standby_screen_off(state, JW_STANDBY_INHIBITED_POWER, true);
        }
        if (newly_pending) {
            jw_log_info("sleep: deferred by %d suspend inhibitor lease(s)", active_leases);
        }
        return;
    }
    state->suspend_policy.pending = JW_SUSPEND_PENDING_NONE;
    jw__schedule_retroarch_audio_reinit_if_bluetooth(state, "wake-bluetooth");
    jw__rumble_set_gated(true);
    jw__rumble_quiesce();   /* never carry a live pulse into the freeze */
    jw__screen_set(state, false);
    /* After the screen is off: stopping suspend-sensitive services is a
       synchronous, bounded wait (stop_grace_ms + 2000 ms per service), and
       holding a lit screen through it makes a sleeping device look wedged.
       The stop must still complete before the freeze, so it cannot move to
       tick the way the CTL-1 session ops did. */
    if (state->services) {
        char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
        int stopped = jw_svc_supervisor_suspend_begin(
            state->services, stuck, sizeof(stuck));
        if (stopped > 0) {
            jw_log_info("sleep: stopped %d suspend-sensitive service(s)",
                        stopped);
        }
        if (stuck[0]) {
            /* Contract table: suspend continues, but the survivor remains
             * visible and no replacement can start over its reservation. */
            jw_log_warn("sleep: service %s could not be verified stopped; "
                        "continuing suspend", stuck);
        }
    }
    /* Same reason as the standby path: a direction still held here is never
       released to the launcher, because the resume flush discards the physical
       release rather than forwarding it. This is the path a power tap takes
       (it skips standby entirely), so without this the wake resumes into a
       phantom held direction. */
    jw_input_proxy_release_buttons(&state->input_proxy);
    jw_input_proxy_set_swallow(&state->input_proxy, true);
    jw_platform_result result;
    jw__platform_sleep_with_performance(state, &result);   /* blocks until resume */
    /* MLP1 firmware can detach/recreate an SD mount while asleep. The platform
       backend first repairs executable mount options and refreshes cwd; only
       after that succeeds may the supervisor rediscover packages on the live
       mount and make suspend-sensitive services eligible for restart. */
    if (state->services && result.code == JW_PLATFORM_RESULT_OK) {
        int service_count = jw_svc_supervisor_scan(state->services);
        if (service_count < 0) {
            jw_log_warn("sleep: post-resume service rescan failed; "
                        "suspend-sensitive services remain stopped");
        } else {
            jw_log_info("sleep: post-resume service rescan found %d service(s)",
                        service_count);
        }
    } else if (state->services) {
        jw_log_warn("sleep: platform resume repair failed; "
                    "post-resume service rescan skipped");
    }
    jw_log_info("sleep: resumed");
    jw__rumble_off();   /* belt and braces if the quiesce above timed out */
    jw__screen_set(state, true);
    jw__reconcile_audio(state, "wake", true);
    jw_input_proxy_set_swallow(&state->input_proxy, false);
    jw_input_proxy_flush(&state->input_proxy);             /* drop queued gamepad + wake press */
    /* Clear the wake press's edges + disarm, so resuming doesn't read it as a new
       sleep request. */
    jw_power_edge edge;
    while (jw_input_proxy_take_power_edge(&state->input_proxy, &edge)) {
        /* discard */
    }
    state->power_sleep_armed = false;
    state->power_held = false;
    jw_input_proxy_mark_activity(&state->input_proxy);
    state->standby_reason = JW_STANDBY_NONE;
    /* This clears standby directly rather than via jw__leave_standby_screen_off,
       so the rumble gate has to be released here too -- otherwise suspending out
       of screen-off standby wakes with haptics silently dead for good. */
    jw__rumble_set_gated(false);
    state->standby_entered_ms = 0;
    state->autosleep_charging_logged = false;
    state->autosleep_setting_next_ms = 0;   /* re-read the setting promptly after wake */
}

static void jw__tick_suspend_inhibitors(jw_daemon_state *state) {
    int reaped = jw_suspend_inhibitor_reap(&state->suspend_inhibitor);
    if (reaped > 0) {
        jw_log_info("suspend-inhibit: reaped %d dead holder lease(s); active=%d",
                    reaped, jw_suspend_inhibitor_count(&state->suspend_inhibitor));
    }
    bool screen_held = jw_suspend_inhibitor_count_scope(&state->suspend_inhibitor,
                                                        JW_SUSPEND_SCOPE_SCREEN) > 0;
    if (screen_held) {
        state->autosleep_screen_inhibit_held = true;
    } else if (state->autosleep_screen_inhibit_held) {
        /* The last screen lease just went (released or reaped). Input-idle has
           been accumulating for the whole film, so without this the screen
           would blank the instant playback ends. Treat the release itself as
           activity and give the user a fresh idle window. */
        jw_input_proxy_mark_activity(&state->input_proxy);
        state->autosleep_screen_inhibit_held = false;
        state->autosleep_screen_inhibit_logged = false;
    }
    /* Let the auto-sleep tick below process and log wake activity before a
       last-release transition can perform a queued suspend. */
    if (state->suspend_policy.pending != JW_SUSPEND_PENDING_NONE &&
        jw__standby_has_wake_activity(state)) {
        return;
    }
    jw_suspend_pending pending = state->suspend_policy.pending;
    jw_suspend_decision decision = jw_suspend_policy_leases_changed(
        &state->suspend_policy,
        jw_suspend_inhibitor_count(&state->suspend_inhibitor));
    if (decision == JW_SUSPEND_DECISION_DEEP_SLEEP) {
        jw_log_info("sleep: performing pending %s suspend after last inhibitor release",
                    pending == JW_SUSPEND_PENDING_EXPLICIT ? "explicit" : "automatic");
        jw__deep_suspend(state);
    }
}

/* HDMI hotplug: on plug, apply the persisted HDMI output mode (4:3/stretch); on
   unplug, revert to the panel (single-head TV mode would otherwise leave the
   device black). Reads the cable status straight from the DRM connector and the
   chosen mode from the settings DB (shared with the launcher). ~1s poll. */
#define JW_HDMI_POLL_MS 1000
static int jw__hdmi_connected_now(void) {
    FILE *fp = fopen("/sys/class/drm/card0-HDMI-A-1/status", "r");
    if (!fp) {
        return 0;
    }
    char s[32] = { 0 };
    char *got = fgets(s, sizeof(s), fp);
    fclose(fp);
    if (!got) {
        return 0;
    }
    s[strcspn(s, "\r\n")] = '\0';
    return strcmp(s, "connected") == 0 ? 1 : 0;
}

/* Is the live HDMI scanout 1080p120? 120Hz only exists at 1080p on this chain, so
   a 1920x1080 mode at ~120Hz is unique to an HDMI 120Hz output. */
static int jw__hdmi_live_is_1080p120(jw_daemon_state *state) {
    /* Asked from the platform (the DRM core), never from the DRM debugfs summary:
       this runs every poll, and reading that file during a mode change crashed the
       kernel (see jw__mlp1_get_display_mode). */
    int width = 0, height = 0, hz = 0;
    if (!state ||
        jw_platform_get_display_mode(&state->platform, &width, &height, &hz) != 0) {
        return 0;
    }
    return width == 1920 && height == 1080 && hz >= 119;
}

static void jw__tick_hdmi(jw_daemon_state *state) {
    long long now = jw__monotonic_ms();
    if (now < state->hdmi_next_poll_ms) {
        return;
    }
    state->hdmi_next_poll_ms = now + JW_HDMI_POLL_MS;

    /* Swap the color-temperature LUT for the output HDMI settled on. The write only
       lands on an active CRTC, so while Weston is still switching it fails and is
       retried each poll until the window runs out. */
    if (state->lut_sync_not_before_ms != 0 && now >= state->lut_sync_not_before_ms) {
        if (jw__apply_color_temp(state, state->lut_sync_external)) {
            state->lut_sync_not_before_ms = 0;
        } else if (now > state->lut_sync_deadline_ms) {
            jw_log_warn("color temperature could not be swapped after the HDMI change; "
                        "it applies on the next HDMI change or restart");
            state->lut_sync_not_before_ms = 0;
        }
    }

    /* ── Auto-revert safety for 1080p120 ──
       A 1080p120 switch can black out a TV/cable that can't carry the 297MHz signal,
       stranding the user on a screen they can't see to navigate back. When 1080p120
       goes live by a deliberate change (skip the boot-apply window), arm a 15s
       deadline; the launcher's "keep" press clears it, otherwise revert to the safe,
       universal 720p60 and drop the saved rate to 60 so it sticks. */
    /* Only ask the display for its mode when a cable is in: 1080p120 exists only on
       HDMI, and the query opens the DRM device, so a unit with nothing plugged in
       should not do that every second. */
    int cur = jw__hdmi_connected_now();
    int is120 = cur == 1 ? jw__hdmi_live_is_1080p120(state) : 0;
    if (is120 && !state->hdmi_was_120 && now > 30000) {
        state->hdmi_revert_deadline_ms = now + 15000;
        jw_log_info("HDMI 1080p120 live -> auto-revert armed (15s)");
        jw__notify_launcher_levels(state);   /* it shows the keep-or-revert prompt */
    } else if (!is120) {
        state->hdmi_revert_deadline_ms = 0;
    }
    state->hdmi_was_120 = is120;
    if (state->hdmi_revert_deadline_ms != 0 && now > state->hdmi_revert_deadline_ms) {
        state->hdmi_revert_deadline_ms = 0;
        state->hdmi_was_120 = 0;
        jw_log_info("HDMI 1080p120 not kept -> reverting to 720p60");
        jw_platform_result rres;
        jw_platform_perform_action(&state->platform,
                                   JW_PLATFORM_ACTION_SET_REFRESH_RATE, 60, &rres);
        if (state->db_path) {
            jw_db_set_setting(state->db_path, "refresh_rate_hz", "60");
        }
    }

    int prev = state->hdmi_last_connected;
    if (cur == prev) {
        return;
    }
    state->hdmi_last_connected = cur;

    /* The persisted HDMI Output setting (0 off / 1 4:3 / 2 stretch). */
    int mode = 0;
    if (state->db_path) {
        char val[16];
        if (jw_db_get_setting(state->db_path, "hdmi_output_mode", val, sizeof(val)) == 0) {
            mode = atoi(val);
            if (mode < 0 || mode > 2) mode = 0;
        }
    }
    if (mode == 0) {
        return;   /* feature disabled -> ignore plug/unplug */
    }

    jw_platform_result res;
    if (cur == 1) {
        /* plugged in (incl. the first poll after boot) -> apply the chosen mode */
        jw_platform_perform_action(&state->platform,
                                   JW_PLATFORM_ACTION_SET_HDMI_OUTPUT, mode, &res);
        jw__schedule_lut_sync(state, true);
        jw_log_info("HDMI hotplug: connected -> applying mode %d", mode);
    } else if (prev == 1) {
        /* was connected, now unplugged -> back to the panel */
        jw_platform_perform_action(&state->platform,
                                   JW_PLATFORM_ACTION_SET_HDMI_OUTPUT, 0, &res);
        jw__schedule_lut_sync(state, false);
        jw_log_info("HDMI hotplug: disconnected -> reverting to panel");
    }
}

/* ── Save before power-button shutdown ─────────────────────────────────────── */

#define JW_POWER_HOLD_SAVE_PROBE_TIMEOUT_MS 300u
#define JW_PHS_MIB 1048576ull

/* One no-wait read: a missing row or anything but "1" is Off. A read the
   library cannot answer now leaves *on alone and returns false. */
static bool jw__power_hold_save_read_flag(const char *db_path, const char *key,
                                          bool *on) {
    char value[8];
    int rc = db_path ? jw_db_read_setting_nowait(db_path, key, value, sizeof(value)) : 1;
    if (rc < 0) {
        return false;
    }
    *on = rc == 0 && strcmp(value, "1") == 0;
    return true;
}

/* Settings poll, never on the hold path: a no-wait read. Resume Game on Boot
   is read with it: the save that arms a resume consults both cached values.
   A library that is busy (a scan or another writer holds it) keeps the cached
   values and returns false, so the caller reads again: a lock is not the
   player switching the save off. */
static bool jw__power_hold_save_refresh_setting(jw_daemon_state *state) {
    bool enabled = state->power_hold_save_enabled;
    bool resume = state->boot_resume_enabled;
    bool read = jw__power_hold_save_read_flag(state->db_path,
                                              "save_state_on_power_hold", &enabled);
    read = jw__power_hold_save_read_flag(state->db_path, JW_BOOT_RESUME_SETTING_KEY,
                                         &resume) && read;
    if (!read && !state->power_hold_settings_unread) {
        jw_log_info("power-hold save: library busy; keeping the cached settings");
    } else if (read && state->power_hold_settings_unread) {
        jw_log_info("power-hold save: library readable again; settings re-read");
    }
    state->power_hold_settings_unread = !read;
    if (enabled != state->power_hold_save_enabled) {
        jw_log_info("power-hold save: setting %s", enabled ? "on" : "off");
    }
    if (resume != state->boot_resume_enabled) {
        jw_log_info("boot resume: setting %s", resume ? "on" : "off");
    }
    state->power_hold_save_enabled = enabled;
    state->boot_resume_enabled = resume;
    return read;
}

/* One banner at a time; best-effort and bounded by the OSD client timeout. */
static bool jw__power_hold_save_notice(jw_daemon_state *state, const char *stage) {
    if (!jw__osd_enabled(state)) {
        return false;
    }
    jw_osd_client client = jw__osd_client(state);
    if (jw_osd_client_show_stage(&client, &state->pico8_exit_confirm_until_ms,
                                 stage, 0) != 0) {
        jw_log_warn("power-hold save: %s OSD request failed", stage);
        return false;
    }
    return true;
}

#define JW_POWER_HOLD_SAVE_RELEASE_NOTICE_TRIES 3
#define JW_POWER_HOLD_SAVE_RELEASE_NOTICE_RETRY_MS 100

/* The release prompt is the one notice the player must see in time, so it is
   retried on following ticks while the key is still held. Logs how long after
   the long press the OSD acknowledged it. */
static void jw__power_hold_save_release_notice(jw_daemon_state *state, long long now) {
    if (state->power_hold_save_release_notice_ok ||
        state->power_hold_save_release_notice_tries >= JW_POWER_HOLD_SAVE_RELEASE_NOTICE_TRIES ||
        now < state->power_hold_save_release_notice_next_ms) {
        return;
    }
    state->power_hold_save_release_notice_tries++;
    long long before = jw__monotonic_ms();
    bool ok = jw__power_hold_save_notice(state, "power-save-release");
    long long after = jw__monotonic_ms();
    if (ok) {
        state->power_hold_save_release_notice_ok = true;
        jw_log_info("power-hold save: release prompt acknowledged %lldms after the long press (attempt %d, osd_ms=%lld)",
                    after - state->power_hold_save_started_ms,
                    state->power_hold_save_release_notice_tries, after - before);
        return;
    }
    state->power_hold_save_release_notice_next_ms = after + JW_POWER_HOLD_SAVE_RELEASE_NOTICE_RETRY_MS;
    if (state->power_hold_save_release_notice_tries >= JW_POWER_HOLD_SAVE_RELEASE_NOTICE_TRIES) {
        jw_log_warn("power-hold save: release prompt not shown after %d attempts",
                    state->power_hold_save_release_notice_tries);
    }
}

static const char *jw__power_hold_save_how(const jw_daemon_state *state) {
    return state->power_hold_save.release_ms > 0 ? "released" : "held";
}

/* Log the latched outcome once. */
static void jw__power_hold_save_report(jw_daemon_state *state) {
    const jw_power_hold_save *s = &state->power_hold_save;
    if (state->power_hold_save_reported || s->phase != JW_POWER_HOLD_SAVE_DONE) {
        return;
    }
    state->power_hold_save_reported = true;
    long long elapsed = state->power_hold_save_started_ms > 0
        ? jw__monotonic_ms() - state->power_hold_save_started_ms : 0;
    const char *name = jw_power_hold_save_outcome_name(s->outcome);
    /* A signal is not the player's shutdown; nobody is waiting on a banner. */
    if (s->outcome != JW_POWER_HOLD_SAVE_OUTCOME_INTERRUPTED) {
        jw__power_hold_save_notice(state, s->outcome == JW_POWER_HOLD_SAVE_OUTCOME_SAVED
                                              ? "power-save-saved" : "power-save-failed");
    }
    if (s->outcome == JW_POWER_HOLD_SAVE_OUTCOME_SAVED) {
        jw_log_info("power-hold save: saved elapsed_ms=%lld key=%s bytes=%llu path=%s",
                    elapsed, jw__power_hold_save_how(state),
                    state->power_hold_save_bytes, state->power_hold_save_final);
        /* The only point that arms Resume Game on Boot. */
        jw__boot_resume_arm(state);
    } else {
        jw_log_warn("power-hold save: %s elapsed_ms=%lld key=%s", name, elapsed,
                    jw__power_hold_save_how(state));
    }
}

/* Everything the hold path may decide without touching the database. Probes
   RetroArch once, bounded. Pins the slot and thumbnail paths on success. */
static bool jw__power_hold_save_eligible(jw_daemon_state *state) {
    const jw_retroarch_session *session = &state->retroarch_session;
    /* Slot 99 still holds the shutdown position this boot is resuming; a cold
       core saved over it would replace the player's game. */
    if (jw__boot_resume_unconfirmed(state) || session->resume_unconfirmed) {
        jw_log_warn("power-hold save: the resumed game has not loaded; not saving over it");
        return false;
    }
    if (!session->rom_path[0] || !session->source_root[0] ||
        !session->core_config_folder[0] || !session->core_id[0]) {
        jw_log_warn("power-hold save: session has no pinned source/core namespace");
        return false;
    }
    char states_dir[PATH_MAX];
    if (snprintf(states_dir, sizeof(states_dir), "%s/States", session->source_root) >=
            (int)sizeof(states_dir) ||
        !jw_ra_slot_state_path_for_core(states_dir, session->core_config_folder,
                                        session->rom_path, JW_RA_GAME_SWITCHER_STATE_SLOT,
                                        false, state->power_hold_save_final,
                                        sizeof(state->power_hold_save_final)) ||
        !jw_ra_slot_state_path_for_core(states_dir, session->core_config_folder,
                                        session->rom_path, JW_RA_GAME_SWITCHER_STATE_SLOT,
                                        true, state->power_hold_save_thumb,
                                        sizeof(state->power_hold_save_thumb))) {
        jw_log_warn("power-hold save: could not pin the switcher slot path");
        return false;
    }
    char reason[JW_STORAGE_REASON_MAX] = "";
    if (!jw_storage_path_writable(states_dir, reason, sizeof(reason))) {
        jw_log_warn("power-hold save: states are not writable (%s)", reason);
        return false;
    }
    jw_ra_client ra = jw_ra_client_default();
    ra.timeout_ms = JW_POWER_HOLD_SAVE_PROBE_TIMEOUT_MS;
    jw_ra_state_save_info info;
    jw_ra_result probe = jw_ra_get_state_save_info(&ra, &info);
    if (probe != JW_RA_OK || !info.supported || info.compressed) {
        /* No fallback to the asynchronous save or the size-settle heuristic. */
        jw_log_info("power-hold save: RetroArch cannot sync-save (probe=%s supported=%d compressed=%d)",
                    jw_ra_result_string(probe), info.supported ? 1 : 0,
                    info.compressed ? 1 : 0);
        return false;
    }
    /* The estimate already rounds up to a MiB; allow the same growth. */
    state->power_hold_save_bytes = (info.bytes + JW_PHS_MIB - 1) / JW_PHS_MIB * JW_PHS_MIB;
    return true;
}

/* The one place a physical long press turns into shutdown. `press_ms` is the
   key-down event timestamp. Both the still-held and the released branch come
   through here; the released branch then feeds its release edge. */
static void jw__power_long_press(jw_daemon_state *state, long long press_ms,
                                 long long held_ms) {
    bool first = !state->shutdown_requested;
    bool handed_off = false;
    state->power_sleep_armed = false;
    if (first) {
        jw_suspend_policy_long_press(&state->suspend_policy);
        if (held_ms >= 0) {
            jw_log_info("power: long-press (%lldms) -> clean power off", held_ms);
        } else {
            jw_log_info("power: long-press -> clean power off");
        }
        /* On MLP1 this publishes the request to the supervisor right now; a
           refused handoff means no shutdown, so a save prompt would be a lie. */
        handed_off = jw__request_power_transition(state, JW_PLATFORM_ACTION_POWEROFF,
                                                  "power-button");
    }
    if (state->power_hold_save.phase != JW_POWER_HOLD_SAVE_IDLE) {
        return;  /* duplicate hold tick, or an attempt already decided */
    }
    /* Off, a refused handoff, other platforms, and anything but a RetroArch
       game get no save phase. */
    if (!first || !handed_off || !state->power_hold_save_enabled ||
        (strcmp(state->platform.platform_id, "mlp1") != 0 &&
         !state->test_power_hold_save_any_platform) ||
        !jw__has_retroarch_session(state) || g_shutdown_requested) {
        return;
    }
    /* The PMIC cut to plan against is the Force Off Hold the daemon applied
       (0 when unsupported or not applied -> the stock 6 s cut, which leaves no
       release wait at all). */
    int hold_s = state->power_hard_cut_effective_s;
    long long cut_ms = jw_power_hold_save_cut_ms_for_hold_s(hold_s);
    long long now = jw__monotonic_ms();
    state->power_hold_save_started_ms = now;
    bool eligible = jw__power_hold_save_eligible(state);
    if (jw_power_hold_save_long_press(&state->power_hold_save, press_ms, now, cut_ms,
                                      eligible, state->last_resume_ms) ==
        JW_POWER_HOLD_SAVE_WAIT) {
        jw_log_info("power-hold save: waiting for release (release by +%lldms; force-off hold %ds, cut %lldms)",
                    state->power_hold_save.release_offset_ms, hold_s, cut_ms);
        jw__power_hold_save_release_notice(state, now);
    } else {
        jw__power_hold_save_report(state);
    }
}

static jw_power_hold_save_outcome jw__power_hold_save_error_outcome(const char *code) {
    if (strcmp(code, "LATE") == 0 || strcmp(code, "TOO_LARGE") == 0) {
        return JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT;
    }
    if (strcmp(code, "UNSUPPORTED") == 0 || strcmp(code, "COMPRESSED") == 0 ||
        strcmp(code, "BUSY") == 0 || strcmp(code, "PATH") == 0 ||
        strcmp(code, "BAD_ARGS") == 0) {
        return JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED;
    }
    return JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR;
}

static void jw__power_hold_save_start(jw_daemon_state *state, long long now) {
    jw_power_hold_save *s = &state->power_hold_save;
    jw_power_hold_save_request req = {
        .state_bytes = state->power_hold_save_bytes,
    };
    long long deadline = 0;
    if (!jw_power_hold_save_admit(s, &req, now, &deadline)) {
        return;
    }
    /* RetroArch must begin writing while the flush and publish still fit. */
    long long estimate = jw_power_hold_save_estimate_ms(&req);
    long long start_by = deadline - (estimate - JW_POWER_HOLD_SAVE_SERIALIZE_MS);
    jw_ra_client ra = jw_ra_client_default();
    if (jw_ra_sync_save_send(&ra, &state->power_hold_save_request,
                             JW_RA_GAME_SWITCHER_STATE_SLOT,
                             state->power_hold_save_bytes, start_by) != JW_RA_OK ||
        !jw_power_hold_save_tmp_path(state->power_hold_save_final,
                                     state->power_hold_save_request.request_id,
                                     state->power_hold_save_tmp,
                                     sizeof(state->power_hold_save_tmp))) {
        jw_log_warn("power-hold save: could not send the save request");
        jw_ra_sync_save_close(&state->power_hold_save_request);
        jw_power_hold_save_finish(s, JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED);
        return;
    }
    state->power_hold_save_tmp_owned = true;
    jw__power_hold_save_notice(state, "power-save-saving");
    jw_log_info("power-hold save: saving bytes<=%llu estimate_ms=%lld window_ms=%lld",
                state->power_hold_save_bytes, estimate, deadline - now);
}

static void jw__power_hold_save_poll(jw_daemon_state *state) {
    jw_power_hold_save *s = &state->power_hold_save;
    jw_ra_sync_save_reply reply;
    jw_ra_result result = jw_ra_sync_save_poll(&state->power_hold_save_request, &reply);
    if (result == JW_RA_TIMEOUT) {
        return;
    }
    jw_ra_sync_save_close(&state->power_hold_save_request);
    if (result == JW_RA_SOCKET_ERROR) {
        jw_power_hold_save_finish(s, JW_POWER_HOLD_SAVE_OUTCOME_CHILD_EXITED);
        return;
    }
    if (result != JW_RA_OK) {
        jw_log_warn("power-hold save: malformed reply");
        jw_power_hold_save_finish(s, JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR);
        return;
    }
    if (!reply.ready) {
        jw_log_warn("power-hold save: RetroArch refused: %s", reply.error);
        state->power_hold_save_tmp_owned = false;  /* RetroArch removed or never made it */
        jw_power_hold_save_finish(s, jw__power_hold_save_error_outcome(reply.error));
        return;
    }
    char error[PATH_MAX + 128];
    jw_phs_publish_result published = jw_power_hold_save_publish(
        state->power_hold_save_tmp, reply.tmp_path, state->power_hold_save_final,
        state->power_hold_save_thumb, error, sizeof(error));
    switch (published) {
    case JW_PHS_PUBLISH_OK:
        state->power_hold_save_tmp_owned = false;
        state->power_hold_save_bytes = reply.bytes;
        jw_power_hold_save_finish(s, JW_POWER_HOLD_SAVE_OUTCOME_SAVED);
        return;
    case JW_PHS_PUBLISH_DIR_SYNC_ERROR:
        state->power_hold_save_tmp_owned = false;
        jw_log_warn("power-hold save: %s", error);
        jw_power_hold_save_finish(s, JW_POWER_HOLD_SAVE_OUTCOME_DURABILITY_UNCERTAIN);
        return;
    case JW_PHS_PUBLISH_REJECTED:
    case JW_PHS_PUBLISH_THUMB_ERROR:
    case JW_PHS_PUBLISH_RENAME_ERROR:
        jw_log_warn("power-hold save: not published: %s", error);
        jw_power_hold_save_finish(s, JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR);
        return;
    }
}

/* Runs every shutdown pass before child teardown. Returns true while the save
   phase must hold teardown back. Never blocks on RetroArch. */
static bool jw__tick_power_hold_save(jw_daemon_state *state) {
    jw_power_hold_save *s = &state->power_hold_save;
    if (jw_power_hold_save_active(s)) {
        if (g_shutdown_requested) {
            jw_power_hold_save_abort(s, JW_POWER_HOLD_SAVE_OUTCOME_INTERRUPTED);
        } else if (state->child_pid <= 0 || !jw__has_retroarch_session(state)) {
            jw_power_hold_save_abort(s, JW_POWER_HOLD_SAVE_OUTCOME_CHILD_EXITED);
        }
    }
    if (jw_power_hold_save_active(s)) {
        long long now = jw__monotonic_ms();
        switch (jw_power_hold_save_tick(s, now)) {
        case JW_POWER_HOLD_SAVE_ADMIT_NOW:
            jw__power_hold_save_start(state, now);
            break;
        case JW_POWER_HOLD_SAVE_WAIT:
            if (s->phase == JW_POWER_HOLD_SAVE_AWAIT_RELEASE) {
                jw__power_hold_save_release_notice(state, now);
            } else if (s->phase == JW_POWER_HOLD_SAVE_SAVING) {
                jw__power_hold_save_poll(state);
            }
            break;
        case JW_POWER_HOLD_SAVE_SHUT_DOWN:
            break;
        }
    }
    if (jw_power_hold_save_active(s)) {
        return true;
    }
    /* A late TMP_READY after a timeout is never read: the socket is gone. */
    jw_ra_sync_save_close(&state->power_hold_save_request);
    jw__power_hold_save_report(state);
    return false;
}

/* Shutdown branch, saved outcome only: RetroArch's own clean exit flushes
   SRAM, memory cards and the runtime config, which the 50 ms SIGTERM grace
   never proved. Send QUIT once, the same way the in-game menu quits, and hold
   the kill sequence back until RetroArch exits (reaped by the normal path,
   writer barrier and game.finish unchanged) or the grace expires. Returns true
   while the branch must wait. Never blocks on RetroArch. */
static bool jw__power_hold_save_quit_grace(jw_daemon_state *state) {
    if (state->power_hold_save.outcome != JW_POWER_HOLD_SAVE_OUTCOME_SAVED ||
        !jw__has_retroarch_session(state) || state->power_hold_save_quit_expired) {
        return false;
    }
    long long now = jw__monotonic_ms();
    if (state->power_hold_save_quit_sent_ms == 0) {
        jw_ra_client ra = jw_ra_client_default();
        jw_ra_result rc = jw_ra_quit(&ra);
        if (rc != JW_RA_OK) {
            state->power_hold_save_quit_expired = true;
            jw_log_warn("power-hold save: quit grace: QUIT failed (%s); killing",
                        jw_ra_result_string(rc));
            return false;
        }
        state->power_hold_save_quit_sent_ms = now;
        state->power_hold_save_quit_deadline_ms = now + JW_POWER_HOLD_SAVE_QUIT_GRACE_MS;
        jw_log_info("power-hold save: quit grace: sent QUIT to RetroArch pid=%d; allowing %d ms",
                    (int)state->child_pid, JW_POWER_HOLD_SAVE_QUIT_GRACE_MS);
        return true;
    }
    if (state->power_hold_save.key_down) {
        /* A re-press starts the PMIC timer again: stop waiting for RetroArch's
           own exit and use the measured teardown budget for the kill path. */
        state->power_hold_save_quit_expired = true;
        jw_log_warn("power-hold save: power key pressed during the quit grace (%lld ms in); killing",
                    now - state->power_hold_save_quit_sent_ms);
        return false;
    }
    if (now < state->power_hold_save_quit_deadline_ms) {
        return true;
    }
    state->power_hold_save_quit_expired = true;
    jw_log_warn("power-hold save: quit grace expired after %d ms; killing",
                JW_POWER_HOLD_SAVE_QUIT_GRACE_MS);
    return false;
}

/* After the writer group is gone: remove a temporary file nobody published. */
static void jw__power_hold_save_cleanup_tmp(jw_daemon_state *state) {
    if (!state->power_hold_save_tmp_owned) {
        return;
    }
    state->power_hold_save_tmp_owned = false;
    if (jw_power_hold_save_remove_tmp(state->power_hold_save_tmp) != 0) {
        jw_log_warn("power-hold save: could not remove %s: %s",
                    state->power_hold_save_tmp, strerror(errno));
    }
}

static void jw__tick_auto_sleep(jw_daemon_state *state) {
    long long now = jw__monotonic_ms();

    /* Settings polls touch the database (Auto Sleep with its 2 s busy wait), so
       they run only between presses and never once shutdown has begun: the
       hold path reads cached values. A skipped poll simply runs on release. */
    if (now >= state->autosleep_setting_next_ms && !state->power_held &&
        !state->shutdown_requested) {
        /* 0 means "re-read now" (start-up and wake); otherwise only a
           library.db write can have changed the setting. */
        bool forced = state->autosleep_setting_next_ms == 0;
        if (jw__db_file_changed(state->db_path, &state->autosleep_db_sig) || forced) {
            state->autosleep_timeout_s = jw__autosleep_read_timeout_s(state);
            if (!jw__power_hold_save_refresh_setting(state)) {
                state->autosleep_db_sig.valid = false;  /* read again next poll */
            }
            state->power_hard_cut_desired_s = jw__power_hard_cut_read_s(state);
        }
        state->autosleep_setting_next_ms = now + JW_AUTOSLEEP_SETTING_POLL_MS;
        jw__autosleep_sync_platform(state);
        jw__power_hard_cut_sync_platform(state, forced);
    }

    /* Power button — jawakad owns the key exclusively, so it must do everything
       loong_power used to: short tap = sleep, long hold = clean power off.
         - dark screen + press -> wake immediately (responsive).
         - lit screen  + press -> arm; sleep on RELEASE, so a long hold doesn't flash
                                  to sleep first.
         - held >= JW_POWER_LONGPRESS_MS -> clean power off (sync + reboot) BEFORE the
                                  PMIC hard-cuts (a hard cut can lose unsynced writes
                                  / corrupt the settings DB).
       Works regardless of the auto-sleep timeout setting. Hold durations come
       from the edges' own kernel timestamps (CLOCK_MONOTONIC, same domain as
       `now`), so a press and release that both queued behind a stalled tick
       still classify by their true spacing instead of reading as a 0ms tap. */
    {
        jw_power_edge edge;
        while (jw_input_proxy_take_power_edge(&state->input_proxy, &edge)) {
            jw_input_proxy_cancel_menu(&state->input_proxy);
            if (state->shutdown_requested) {
                /* Already shutting down: edges only feed the save policy. A tap
                   must not sleep and a new hold must not start anything. */
                jw_power_hold_save_key_edge(&state->power_hold_save, edge.down,
                                            (long long)edge.ms);
                continue;
            }
            if (edge.down) {
                state->power_held = true;
                state->power_down_ms = (long long)edge.ms;
                if (jw__standby_active(state)) {
                    jw_input_proxy_mark_activity(&state->input_proxy);  /* wake the dark screen */
                    state->power_sleep_armed = false;
                } else {
                    state->power_sleep_armed = true;                    /* sleep on release */
                }
                continue;
            }
            /* Release edge. */
            bool was_held = state->power_held;
            long long held_ms = was_held ? (long long)edge.ms - state->power_down_ms : 0;
            state->power_held = false;
            if (was_held && held_ms >= JW_POWER_LONGPRESS_MS) {
                /* Recognized late: the release is already queued. */
                jw__power_long_press(state, state->power_down_ms, held_ms);
                jw_power_hold_save_key_edge(&state->power_hold_save, false,
                                            (long long)edge.ms);
                return;
            }
            if (state->power_sleep_armed) {
                state->power_sleep_armed = false;
                int active_leases = jw_suspend_inhibitor_count(&state->suspend_inhibitor);
                if (active_leases > 0) {
                    jw_suspend_policy_power_tap(&state->suspend_policy, active_leases);
                    jw_log_info("power: sleep tap deferred by %d suspend inhibitor lease(s)",
                                active_leases);
                    jw__enter_standby_screen_off(state, JW_STANDBY_INHIBITED_POWER, true);
                } else if (jw__charging_online(state, true)) {
                    jw_log_info("power: standby while charging (tap)");
                    jw__enter_standby_screen_off(state, JW_STANDBY_POWER_CHARGING, true);
                } else {
                    jw_log_info("power: sleep (tap)");
                    jw__deep_suspend(state);
                }
                return;
            }
        }
        /* Still held with no release edge yet: power off the moment the hold
           crosses the threshold rather than waiting for the release. */
        if (state->power_held && now - state->power_down_ms >= JW_POWER_LONGPRESS_MS) {
            /* power_held only suppresses duplicates from here on; the save
               policy tracks the physical key from the edges. */
            state->power_held = false;
            jw__power_long_press(state, state->power_down_ms, -1);
            return;
        }
    }

    if (jw__standby_has_wake_activity(state)) {
        jw_suspend_pending cancelled =
            jw_suspend_policy_cancel_for_activity(&state->suspend_policy);
        if (cancelled == JW_SUSPEND_PENDING_AUTO) {
            jw_log_info("auto-sleep: pending suspend cancelled by user input");
        } else if (cancelled == JW_SUSPEND_PENDING_EXPLICIT) {
            jw_log_info("power: explicit pending sleep cancelled by user input");
        }
        jw_log_info("standby: wake on input");
        jw__leave_standby_screen_off(state);
        return;
    }

    /* Shutting down: ensure the screen is on and bail. */
    if (state->shutdown_requested) {
        jw__leave_standby_screen_off(state);
        return;
    }

    if (state->standby_reason == JW_STANDBY_POWER_CHARGING &&
        !jw__charging_online(state, false)) {
        long long standby_ms = state->standby_entered_ms > 0
            ? now - state->standby_entered_ms
            : JW_AUTOSLEEP_SUSPEND_GRACE_MS;
        if (standby_ms >= JW_AUTOSLEEP_SUSPEND_GRACE_MS) {
            jw_log_info("standby: charger unplugged -> sleep (standby %lldms)",
                        standby_ms);
            jw__deep_suspend(state);
        }
        return;
    }

    /* Auto-sleep disabled: undo only an auto-sleep blank. Power-button charging
       standby is handled above: it stays dark while plugged in, or suspends once
       unplugged after the usual grace window. */
    if (state->autosleep_timeout_s <= 0) {
        if (state->standby_reason == JW_STANDBY_AUTOSLEEP) {
            jw__leave_standby_screen_off(state);
        }
        return;
    }

    uint64_t idle_ms = jw_input_proxy_idle_ms(&state->input_proxy);
    uint64_t screen_off_at = (uint64_t)state->autosleep_timeout_s * 1000u;
    uint64_t suspend_at = screen_off_at + JW_AUTOSLEEP_SUSPEND_GRACE_MS;

    if (idle_ms < screen_off_at) {
        /* Active: undo an auto-sleep blank. Manual charging standby stays dark
           until the wake-activity check above sees a new button press. */
        if (state->standby_reason == JW_STANDBY_AUTOSLEEP) {
            jw__leave_standby_screen_off(state);
        }
        return;
    }

    if (idle_ms < suspend_at) {
        /* Stage 1 — blank the backlight. The proxy swallows input while blanked: a
           press resets the idle timer (waking the screen next tick) but is NOT
           forwarded, so the wake press only wakes the screen instead of also firing
           a navigation action. A power press here is routed above and wakes too. */
        if (!jw__standby_active(state)) {
            jw_log_info("auto-sleep: screen off (idle %llums)",
                        (unsigned long long)idle_ms);
            jw__enter_standby_screen_off(state, JW_STANDBY_AUTOSLEEP, false);
        }
        return;
    }

    if (jw__charging_online(state, false)) {
        if (!jw__standby_active(state)) {
            jw__enter_standby_screen_off(state, JW_STANDBY_AUTOSLEEP, false);
        }
        if (!state->autosleep_charging_logged) {
            jw_log_info("auto-sleep: standby while charging (idle %llums)",
                        (unsigned long long)idle_ms);
            state->autosleep_charging_logged = true;
        }
        return;
    }

    /* Stage 2 — real suspend-to-RAM. */
    bool newly_inhibited = state->suspend_policy.pending == JW_SUSPEND_PENDING_NONE;
    jw_suspend_decision sleep_decision = jw_suspend_policy_auto_stage2(
        &state->suspend_policy,
        jw_suspend_inhibitor_count(&state->suspend_inhibitor));
    if (sleep_decision == JW_SUSPEND_DECISION_SCREEN_OFF) {
        if (newly_inhibited) {
            jw_log_info("auto-sleep: deep suspend deferred by %d inhibitor lease(s)",
                        jw_suspend_inhibitor_count(&state->suspend_inhibitor));
        }
        if (!jw__standby_active(state)) {
            jw__enter_standby_screen_off(state, JW_STANDBY_AUTOSLEEP, false);
        }
        return;
    }
    jw_log_info("auto-sleep: suspending (idle %llums)", (unsigned long long)idle_ms);
    jw__deep_suspend(state);
}

static void jw__tick_post_launch_resume(jw_daemon_state *state) {
    if (!state || !state->post_launch_resume_pending) {
        return;
    }
    if (!state->retroarch_session.active || state->child_kind != JW_CHILD_RETROARCH) {
        state->post_launch_resume_pending = false;
        state->post_launch_resume_attempts = 0;
        state->post_launch_resume_next_ms = 0;
        return;
    }

    long long now = jw__monotonic_ms();
    if (state->post_launch_resume_next_ms > now) {
        return;
    }

    state->post_launch_resume_attempts++;
    jw_ra_client ra = jw_ra_client_default();
    ra.timeout_ms = 150u;

    jw_ra_info info;
    memset(&info, 0, sizeof(info));
    jw_ra_result info_result = jw_ra_get_info(&ra, &info);
    if (info_result != JW_RA_OK) {
        if (state->post_launch_resume_attempts >= JW_SWITCHER_RESUME_MAX_ATTEMPTS) {
            jw_log_warn("switcher resume: RetroArch command interface not ready result=%s",
                        jw_ra_result_string(info_result));
            state->post_launch_resume_pending = false;
            state->post_launch_resume_attempts = 0;
            state->post_launch_resume_next_ms = 0;
            jw__schedule_in_game_menu_prewarm(state,
                                              JW_INGAME_MENU_PREWARM_AFTER_RESUME_MS);
        } else {
            state->post_launch_resume_next_ms = now + JW_SWITCHER_RESUME_RETRY_MS;
        }
        return;
    }

    /* Don't commit (pending=false) until the load actually succeeds. The readiness
       probe (jw_ra_get_info) can answer before the core+content are ready to accept
       a state load, so the first load can time out; in that case keep retrying
       within the same attempt budget rather than cold-starting. Only the permanent
       failures below (no savestate support / no state on disk) give up immediately. */
    bool give_up = false;

    if (!info.savestate_supported) {
        jw_log_warn("switcher resume: core does not support savestates");
        give_up = true;
    }

    char states_dir[PATH_MAX];
    const char *source_root = state->retroarch_session.source_root[0]
        ? state->retroarch_session.source_root
        : state->sdcard_root;
    if (!give_up &&
        (!source_root ||
         snprintf(states_dir, sizeof(states_dir), "%s/States", source_root) >=
             (int)sizeof(states_dir))) {
        jw_log_warn("switcher resume: states path unavailable");
        give_up = true;
    }

    int slot = 0;
    char state_path[PATH_MAX];
    state_path[0] = '\0';
    if (!give_up &&
        !jw_ra_find_resume_state_for_core(
            states_dir,
            state->retroarch_session.core_config_folder,
            state->retroarch_session.rom_path,
            JW_RA_GAME_SWITCHER_STATE_SLOT,
            &slot, state_path, sizeof(state_path))) {
        jw_log_info("switcher resume: no state found for %s",
                    state->retroarch_session.rom_path);
        give_up = true;
    }

    if (!give_up) {
        char reply[JW_RA_REPLY_MAX];
        ra.timeout_ms = JW_SWITCHER_RESUME_LOAD_TIMEOUT_MS;
        jw_ra_result load = jw_ra_load_state_slot(&ra, slot, reply, sizeof(reply));
        if (load == JW_RA_OK) {
            jw_ra_resume_direct(&ra);
            jw_log_info("switcher resume: loaded slot=%d path=%s", slot, state_path);
        } else if (state->post_launch_resume_attempts < JW_SWITCHER_RESUME_MAX_ATTEMPTS) {
            /* RA accepted commands but isn't ready to load the state yet — retry
               next tick; pending stays set, attempts already counted at the top. */
            state->post_launch_resume_next_ms = now + JW_SWITCHER_RESUME_RETRY_MS;
            return;
        } else {
            jw_log_warn("switcher resume: load failed slot=%d path=%s result=%s",
                        slot, state_path, jw_ra_result_string(load));
        }
    }

    state->post_launch_resume_pending = false;
    state->post_launch_resume_attempts = 0;
    state->post_launch_resume_next_ms = 0;
    jw__schedule_in_game_menu_prewarm(state,
                                      JW_INGAME_MENU_PREWARM_AFTER_RESUME_MS);
}

/* ── Resume Game on Boot ──────────────────────────────────────────────────────
   One record per qualifying power-button shutdown, read and deleted once at
   the next start, then one launch and exactly one LOAD_STATE_SYNC. Either the
   saved state loads, or RetroArch quits and the launcher opens with a notice.
   Plan: umrk-workspace/plans/Jawaka/auto-resume-after-power-off.md. */

/* The state-info and capability probes, answered within a frame or two. */
#define JW_BOOT_RESUME_PROBE_TIMEOUT_MS 300u
/* A first frame can be slow, so one unanswered probe proves nothing; this
   many in a row while GET_INFO answers is a RetroArch without the commands. */
#define JW_BOOT_RESUME_SILENT_PROBES 3
/* The one load reads the whole state off the card and applies it inside
   RetroArch's command handler. A ~30 MB Dreamcast state on a slow card is the
   long end; this only decides when silence counts as failure. */
#define JW_BOOT_RESUME_LOAD_TIMEOUT_MS 10000LL

static bool jw__boot_resume_unconfirmed(const jw_daemon_state *state) {
    return state && state->boot_resume.phase != JW_BOOT_RESUME_PHASE_NONE;
}

static void jw__boot_resume_clear(jw_daemon_state *state) {
    jw_ra_sync_save_close(&state->boot_resume.load);
    memset(&state->boot_resume, 0, sizeof(state->boot_resume));
    jw_ra_sync_save_init(&state->boot_resume.load);
}

/* A card's persistent identity, or "" when it has none worth recording. */
static const char *jw__boot_resume_source_fingerprint(const jw_daemon_state *state,
                                                      const jw_storage_source *source) {
    if (!source) {
        return "";
    }
#ifndef PLATFORM_MLP1
    if (!source->filesystem_fingerprint[0] && source->primary &&
        state->test_source_fingerprint[0]) {
        return state->test_source_fingerprint;
    }
#else
    (void)state;
#endif
    const char *fp = source->filesystem_fingerprint;
    return strncmp(fp, "uuid:", 5) == 0 || strncmp(fp, "fat:", 4) == 0 ? fp : "";
}

static void jw__retroarch_session_pin_resume(jw_daemon_state *state,
                                             const jw_storage_source *source,
                                             const char *rom_relpath,
                                             const char *provider) {
    jw_retroarch_session *session = &state->retroarch_session;
    session->source_fingerprint[0] = '\0';
    session->states_path[0] = '\0';
    snprintf(session->rom_relpath, sizeof(session->rom_relpath), "%s",
             rom_relpath ? rom_relpath : "");
    snprintf(session->provider, sizeof(session->provider), "%s",
             provider ? provider : "");
    if (source) {
        snprintf(session->source_fingerprint, sizeof(session->source_fingerprint), "%s",
                 jw__boot_resume_source_fingerprint(state, source));
        snprintf(session->states_path, sizeof(session->states_path), "%s",
                 source->states_path);
    }
}

/* Saved outcome only, inside the released window and before child teardown:
   one small file and two fsyncs, from values already in memory. No database,
   no card scan, no read of the state. A failure costs only the automatic
   continuation; the save itself stands. */
static void jw__boot_resume_arm(jw_daemon_state *state) {
    if (!state->power_hold_save_enabled) {
        return;
    }
    if (!state->boot_resume_enabled) {
        /* Said out loud: on the device this is the first question when a
           game did not come back, and the session log of the shutdown is
           rotated away after two more boots. */
        jw_log_info("power-hold save: saved; Resume Game on Boot is off, nothing armed");
        return;
    }
    const jw_retroarch_session *session = &state->retroarch_session;
    long long started = jw__monotonic_ms();
    jw_boot_resume_record record;
    memset(&record, 0, sizeof(record));
    char reason[64] = "";
    const char *why = NULL;
    if (!state->boot_id[0]) {
        why = "boot id unknown";
    } else if (!session->source_fingerprint[0]) {
        why = "card has no persistent identity";
    } else if (!jw_boot_resume_relative_to(session->states_path,
                                           state->power_hold_save_final,
                                           record.state_path,
                                           sizeof(record.state_path))) {
        why = "state is outside the card's States folder";
    } else {
        snprintf(record.platform, sizeof(record.platform), "%s",
                 state->platform.platform_id);
        snprintf(record.boot_id, sizeof(record.boot_id), "%s", state->boot_id);
        snprintf(record.request_id, sizeof(record.request_id), "%s",
                 state->power_hold_save_request.request_id);
        snprintf(record.source_fingerprint, sizeof(record.source_fingerprint), "%s",
                 session->source_fingerprint);
        snprintf(record.system, sizeof(record.system), "%s", session->system);
        snprintf(record.rom_path, sizeof(record.rom_path), "%s", session->rom_relpath);
        snprintf(record.core_id, sizeof(record.core_id), "%s", session->core_id);
        snprintf(record.core_config_folder, sizeof(record.core_config_folder), "%s",
                 session->core_config_folder);
        snprintf(record.provider, sizeof(record.provider), "%s", session->provider);
        record.slot = JW_BOOT_RESUME_SLOT;
        record.state_bytes = state->power_hold_save_bytes;
        record.offline_proxy = session->config_snapshot.proxied;
        if (!jw_boot_resume_record_valid(&record, record.platform,
                                         reason, sizeof(reason))) {
            why = reason;
        } else if (!jw_boot_resume_write(state->state_dir, &record,
                                         reason, sizeof(reason))) {
            why = reason;
        }
    }
    long long write_ms = jw__monotonic_ms() - started;
    if (why) {
        jw_log_warn("power-hold save: saved; resume not armed (%s) write_ms=%lld",
                    why, write_ms);
        return;
    }
    jw_log_info("power-hold save: resume armed request=%s write_ms=%lld rom=%s route=%s",
                record.request_id, write_ms, record.rom_path,
                record.offline_proxy ? "offline-proxy" : "direct");
}

/* A repair or recovery owns this boot: a read-only card, a held repair, a
   repair result nobody has seen yet, or a launch LIFE-1 recovered. */
static bool jw__boot_resume_storage_recovery(jw_daemon_state *state) {
    if (state->storage_db_read_only || state->active_game.active) {
        return true;
    }
    jw_storage_probe_env env;
    jw_storage_probe_env_default(&env);
    for (int i = 0; i < state->storage_monitor.count; i++) {
        const jw_storage_health_slot *slot = &state->storage_monitor.slots[i];
        if (!slot->valid || !slot->health.mounted) {
            continue;
        }
        if (slot->health.access == JW_STORAGE_ACCESS_READ_ONLY ||
            slot->health.repair != JW_STORAGE_REPAIR_NONE) {
            return true;
        }
        jw_storage_repair_result result;
        if (jw_storage_repair_last_result(&env, slot->health.uuid, &result) &&
            !result.acknowledged) {
            return true;
        }
    }
    return false;
}

static void jw__boot_resume_notice(jw_daemon_state *state) {
    if (!jw__osd_enabled(state)) {
        return;
    }
    jw_osd_client client = jw__osd_client(state);
    if (jw_osd_client_show_stage(&client, &state->pico8_exit_confirm_until_ms,
                                 "boot-resume", 0) != 0) {
        jw_log_warn("boot resume: OSD notice request failed");
    }
}

/* A gate that would put a question in front of the player cancels instead:
   nobody chose to start this game, so nobody is there to answer. */
static void jw__boot_resume_cancel_prompts(jw_daemon_state *state) {
    if (state->game_check_decision) {
        (void)jw__game_check_cancel(state);
    }
    if (state->game_launch_blocked) {
        (void)jw__game_launch_blocked_dismiss(state);
    }
}

/* Anything but OK. RetroArch, when it is there, gets QUIT and the normal child
   and group cleanup runs; the launcher it returns to carries the notice. The
   state, its thumbnail and the saves are never touched. */
static void jw__boot_resume_fail(jw_daemon_state *state, const char *why, bool quit) {
    if (!jw__boot_resume_unconfirmed(state)) {
        return;
    }
    long long elapsed = state->boot_resume.started_ms > 0
        ? jw__monotonic_ms() - state->boot_resume.started_ms : 0;
    jw_log_warn("boot resume: failed (%s) elapsed_ms=%lld request=%s; the launcher opens with a notice",
                why ? why : "unknown", elapsed, state->boot_resume.request_id);
    jw__boot_resume_clear(state);
    state->boot_resume_notice_pending = true;
    if (!quit || !jw__has_retroarch_session(state)) {
        return;
    }
    jw_ra_client ra = jw_ra_client_default();
    jw_ra_result rc = jw_ra_quit(&ra);
    jw_log_info("boot resume: sent QUIT to RetroArch pid=%d result=%s",
                (int)state->child_pid, jw_ra_result_string(rc));
    if (rc != JW_RA_OK) {
        (void)jw__signal_tracked_game_group(state, SIGTERM);
    }
    /* A RetroArch that never leaves is forced closed by the stuck-quit tick. */
    state->retroarch_quit_deadline_ms = jw__monotonic_ms() + JW_RETROARCH_STUCK_QUIT_MS;
}

static void jw__boot_resume_succeed(jw_daemon_state *state, unsigned long long bytes) {
    long long elapsed = state->boot_resume.started_ms > 0
        ? jw__monotonic_ms() - state->boot_resume.started_ms : 0;
    /* As the switcher resume does, unless the player opened the in-game menu
       meanwhile: closing it resumes the game. */
    if (!state->menu_visible) {
        jw_ra_client ra = jw_ra_client_default();
        (void)jw_ra_resume_direct(&ra);
    }
    jw_log_info("boot resume: loaded request=%s bytes=%llu elapsed_ms=%lld path=%s",
                state->boot_resume.request_id, bytes, elapsed,
                state->boot_resume.state_path);
    /* From here the session is the player's game and records its play once,
       when it ends, like any other. */
    state->retroarch_session.resume_unconfirmed = false;
    jw__boot_resume_clear(state);
    jw__boot_resume_notice(state);
    jw__schedule_in_game_menu_prewarm(state, JW_INGAME_MENU_PREWARM_AFTER_RESUME_MS);
}

/* Last check before fork: the launch about to start is the one validated at
   start-up. Called from jw__spawn_retroarch for a boot resume only. */
static bool jw__boot_resume_spawn_matches(jw_daemon_state *state,
                                          const jw_storage_source *source,
                                          const jw_launch_target *target,
                                          const char *rom_abs) {
    const jw_boot_resume_launch *br = &state->boot_resume;
    char expected[PATH_MAX];
    struct stat st;
    const char *why = NULL;
    if (!source || !target || !rom_abs) {
        why = "launch has no source";
    } else if (strcmp(target->core_id, br->core_id) != 0 ||
               strcmp(target->core_config_folder, br->core_config_folder) != 0 ||
               strcmp(target->provider, br->provider) != 0) {
        why = "core changed";
    } else if (strcmp(jw__boot_resume_source_fingerprint(state, source),
                      br->source_fingerprint) != 0) {
        why = "card changed";
    } else if (!jw_ra_slot_state_path_for_core(source->states_path, br->core_config_folder,
                                               rom_abs, JW_BOOT_RESUME_SLOT, false,
                                               expected, sizeof(expected)) ||
               strcmp(expected, br->state_path) != 0) {
        why = "state path changed";
    } else if (lstat(expected, &st) != 0 || !S_ISREG(st.st_mode) ||
               (unsigned long long)st.st_size != br->state_bytes) {
        why = "state changed";
    } else if (br->offline_proxy && !state->pending_rop_snapshot.proxied) {
        why = "the launch would not go through the achievements proxy";
    }
    if (why) {
        jw_log_warn("boot resume: not starting RetroArch: %s", why);
        return false;
    }
    return true;
}

/* How long a resume saved through RAOfflineProxy waits for that service at
   start-up. It autostarts on the first loop pass; the bound is for a proxy
   that never becomes healthy, which ends in the launcher, not a Hardcore
   session that would refuse the state. */
#define JW_BOOT_RESUME_PROXY_WAIT_MS 10000LL

static const jw_svc_supervised *jw__boot_resume_proxy_entry(const jw_daemon_state *state) {
    return state->services
        ? jw_svc_supervisor_find(state->services, JW_ROP_SERVICE_ID) : NULL;
}

/* Installed, valid and set to start with Leaf: it will come up this boot. */
static bool jw__boot_resume_proxy_enabled(const jw_svc_supervised *entry) {
    return entry && entry->pak_present && entry->manifest_valid &&
           !entry->on_secondary_root &&
           (entry->desired_enabled || entry->session_run);
}

/* Ask for the launch the pinned resume describes. */
static bool jw__boot_resume_request(jw_daemon_state *state, char *why, size_t why_size) {
    jw_boot_resume_launch *br = &state->boot_resume;
    br->phase = JW_BOOT_RESUME_PHASE_LAUNCHING;
    jw_log_info("boot resume: launching system=%s rom=%s core=%s request=%s route=%s",
                br->system, br->rom_abs, br->core_id, br->request_id,
                br->offline_proxy ? "offline-proxy" : "direct");

    /* Every manual-launch gate applies: BIOS and the core check here, LIFE-1
       and RAOfflineProxy in the spawn path, and the pinned identity (and,
       for a proxied save, the proxied route) again right before fork.
       --entryslot is never used. */
    const char *error = NULL;
    if (jw__request_launch_game(state, br->system, br->db_rom_path,
                                NULL, false, &error) != 0) {
        jw__boot_resume_cancel_prompts(state);
        snprintf(why, why_size, "%s", error ? error : "launch refused");
        return false;
    }
    return true;
}

/* Resolve the record against this boot's cards, library and catalog, then ask
   for the launch. False with `why` when any of it fails. */
static bool jw__boot_resume_launch(jw_daemon_state *state,
                                   const jw_boot_resume_record *record,
                                   char *why, size_t why_size) {
    jw_storage_source_list sources;
    if (jw__storage_sources(state, &sources) != 0) {
        snprintf(why, why_size, "card list unavailable");
        return false;
    }
    /* Source ids are positional and mounts swap: only the fingerprint names
       the card. Two cards claiming it is as good as none. */
    const jw_storage_source *source = NULL;
    int matches = 0;
    for (int i = 0; i < sources.count; i++) {
        const jw_storage_source *candidate = &sources.sources[i];
        if (candidate->configured && candidate->available &&
            strcmp(jw__boot_resume_source_fingerprint(state, candidate),
                   record->source_fingerprint) == 0) {
            source = candidate;
            matches++;
        }
    }
    if (matches != 1) {
        snprintf(why, why_size, matches == 0 ? "card %s not found" : "card %s is ambiguous",
                 record->source_fingerprint);
        return false;
    }
    char rom_abs[PATH_MAX];
    if (!jw_storage_relative_path_valid(record->rom_path) ||
        jw_storage_resolve_rom(source, record->rom_path, true,
                               rom_abs, sizeof(rom_abs)) != 0) {
        snprintf(why, why_size, "ROM missing");
        return false;
    }
    /* The library row for exactly this card and path, by the keys that
       survive a mount swap: a second card's rom_path still holds the mount
       path of its last scan, and that scan has not run yet this boot. The
       launch then resolves the ROM through the row, on today's mount. */
    jw_game_entry game;
    if (!state->db_path ||
        jw_db_get_game_by_source_relpath(state->db_path, source->id,
                                         record->rom_path, &game) != 0 ||
        strcmp(game.system, record->system) != 0) {
        snprintf(why, why_size, "game not in the library");
        return false;
    }
    if (state->focus_boot == JW_FOCUS_BOOT_ENTER) {
        bool in_focus = false;
        for (int i = 0; i < state->focus_cfg.id_count; i++) {
            in_focus = in_focus || state->focus_cfg.ids[i] == game.id;
        }
        if (!in_focus) {
            snprintf(why, why_size, "5-game mode does not include this game");
            return false;
        }
    }
    /* The core this game launches with now, through the effective catalog,
       must be the one that saved: same id, state folder and provider. A
       changed selection cancels; nothing is substituted. (A requested core id
       only selects standalone emulators, so RetroArch is resolved this way.) */
    jw_launch_target target;
    if (jw__resolve_launch_target(state, record->system, game.rom_path,
                                  NULL, &target) != 0 ||
        target.kind != JW_LAUNCH_TARGET_RETROARCH) {
        snprintf(why, why_size, "core %s unavailable", record->core_id);
        return false;
    }
    if (strcmp(target.core_id, record->core_id) != 0 ||
        strcmp(target.core_config_folder, record->core_config_folder) != 0 ||
        strcmp(target.provider, record->provider) != 0) {
        snprintf(why, why_size, "core changed: %s/%s is now %s/%s",
                 record->core_id, record->core_config_folder,
                 target.core_id, target.core_config_folder);
        return false;
    }
    char state_abs[PATH_MAX];
    char state_reason[48];
    if (!jw_boot_resume_state_file_ok(record, source->states_path, rom_abs,
                                      state_abs, sizeof(state_abs),
                                      state_reason, sizeof(state_reason))) {
        snprintf(why, why_size, "%s", state_reason);
        return false;
    }

    jw_boot_resume_launch *br = &state->boot_resume;
    jw__boot_resume_clear(state);
    br->game_id = game.id;
    br->started_ms = jw__monotonic_ms();
    br->state_bytes = record->state_bytes;
    br->offline_proxy = record->offline_proxy;
    snprintf(br->core_id, sizeof(br->core_id), "%s", record->core_id);
    snprintf(br->core_config_folder, sizeof(br->core_config_folder), "%s",
             record->core_config_folder);
    snprintf(br->provider, sizeof(br->provider), "%s", record->provider);
    snprintf(br->source_fingerprint, sizeof(br->source_fingerprint), "%s",
             record->source_fingerprint);
    snprintf(br->state_path, sizeof(br->state_path), "%s", state_abs);
    snprintf(br->request_id, sizeof(br->request_id), "%s", record->request_id);
    snprintf(br->system, sizeof(br->system), "%s", record->system);
    snprintf(br->db_rom_path, sizeof(br->db_rom_path), "%s", game.rom_path);
    snprintf(br->rom_abs, sizeof(br->rom_abs), "%s", rom_abs);

    if (br->offline_proxy) {
        /* Services autostart on the first loop pass, after this. */
        if (!jw__boot_resume_proxy_enabled(jw__boot_resume_proxy_entry(state))) {
            jw__boot_resume_clear(state);
            snprintf(why, why_size, "the achievements proxy is off");
            return false;
        }
        br->phase = JW_BOOT_RESUME_PHASE_WAITING_PROXY;
        br->deadline_ms = br->started_ms + JW_BOOT_RESUME_PROXY_WAIT_MS;
        jw_log_info("boot resume: saved through the achievements proxy; waiting up to %lld ms for it",
                    JW_BOOT_RESUME_PROXY_WAIT_MS);
        return true;
    }
    return jw__boot_resume_request(state, why, why_size);
}

/* Between the request and RetroArch: a launch that needs a decision is
   cancelled, one that ended without RetroArch is a failure, and in both cases
   the launcher opens with the notice. */
static void jw__tick_boot_resume_launch(jw_daemon_state *state) {
    jw_boot_resume_launch *br = &state->boot_resume;
    if (br->phase == JW_BOOT_RESUME_PHASE_WAITING_PROXY) {
        const jw_svc_supervised *entry = jw__boot_resume_proxy_entry(state);
        long long now = jw__monotonic_ms();
        if (!jw__boot_resume_proxy_enabled(entry)) {
            jw__boot_resume_fail(state, "the achievements proxy is off", false);
        } else if (jw__raofflineproxy_entry_live(entry) &&
                   entry->state == JW_SVC_STATE_RUNNING &&
                   jw_raofflineproxy_health_ready(JW_ROP_HEALTH_HOST,
                                                  JW_ROP_HEALTH_PORT, 50)) {
            jw_log_info("boot resume: achievements proxy ready after %lld ms",
                        now - br->started_ms);
            char why[768];
            if (jw__boot_resume_request(state, why, sizeof(why))) {
                return;
            }
            jw__boot_resume_fail(state, why, false);
        } else if (now >= br->deadline_ms) {
            jw__boot_resume_fail(state, "the achievements proxy did not come up", false);
        } else {
            return;
        }
    } else if (br->phase == JW_BOOT_RESUME_PHASE_LAUNCHING) {
        if (state->game_check_decision || state->game_launch_blocked) {
            const char *why = state->game_launch_blocked
                ? "launch needs a decision" : "sync check needs a decision";
            jw__boot_resume_cancel_prompts(state);
            jw__boot_resume_fail(state, why, false);
        } else if (!state->pending_launch && !state->game_coordination_pending &&
                   state->child_pid <= 0) {
            jw__boot_resume_fail(state, "the launch did not start", false);
        } else {
            return;
        }
    } else {
        return;
    }
    if (!state->daemon_only && state->child_pid <= 0 && !state->shutdown_requested) {
        if (jw__spawn_child(state, JW_CHILD_LAUNCHER) != 0) {
            jw_log_error("boot resume: could not open the launcher");
        }
    }
}

/* RetroArch is up: wait for its command port and for a core that can take a
   state, prove the command exists, then send the one load and wait for its
   one reply. Never blocks longer than one bounded probe. */
static void jw__tick_boot_resume(jw_daemon_state *state) {
    jw_boot_resume_launch *br = &state->boot_resume;
    if (br->phase != JW_BOOT_RESUME_PHASE_PROBING &&
        br->phase != JW_BOOT_RESUME_PHASE_LOADING) {
        return;
    }
    if (state->shutdown_requested || g_shutdown_requested) {
        jw_log_warn("boot resume: abandoned for shutdown request=%s", br->request_id);
        jw__boot_resume_clear(state);
        return;
    }
    if (!jw__has_retroarch_session(state)) {
        jw__boot_resume_fail(state, "RetroArch exited before the state loaded", false);
        return;
    }
    long long now = jw__monotonic_ms();
    jw_ra_client ra = jw_ra_client_default();

    if (br->phase == JW_BOOT_RESUME_PHASE_PROBING) {
        if (br->next_ms > now) {
            return;
        }
        br->attempts++;
        ra.timeout_ms = 150u;
        jw_ra_info info;
        memset(&info, 0, sizeof(info));
        jw_ra_result result = jw_ra_get_info(&ra, &info);
        const char *step = "GET_INFO";
        bool ready = false;
        if (result == JW_RA_OK && info.savestate_supported) {
            /* GET_INFO can answer before the core will serialize: wait,
               within the same budget, for a size the load can be checked
               against, then for proof that LOAD_STATE_SYNC exists. */
            ra.timeout_ms = JW_BOOT_RESUME_PROBE_TIMEOUT_MS;
            jw_ra_state_save_info save_info;
            step = "state info";
            result = jw_ra_get_state_save_info(&ra, &save_info);
            if (result == JW_RA_OK && save_info.supported) {
                step = "probe";
                result = jw_ra_load_state_sync_probe(&ra, br->request_id);
                ready = result == JW_RA_OK;
            }
            if (result == JW_RA_TIMEOUT) {
                if (++br->silent_probes >= JW_BOOT_RESUME_SILENT_PROBES) {
                    char why[96];
                    snprintf(why, sizeof(why), "RetroArch cannot sync-load (%s=timeout)", step);
                    jw__boot_resume_fail(state, why, true);
                    return;
                }
            } else if (result != JW_RA_OK) {
                char why[96];
                snprintf(why, sizeof(why), "RetroArch cannot sync-load (%s=%s)", step,
                         jw_ra_result_string(result));
                jw__boot_resume_fail(state, why, true);
                return;
            } else {
                br->silent_probes = 0;
            }
        }
        if (!ready) {
            if (br->attempts >= JW_SWITCHER_RESUME_MAX_ATTEMPTS) {
                char why[96];
                snprintf(why, sizeof(why), result != JW_RA_OK
                             ? "RetroArch not ready (%s)" : "core cannot load states",
                         jw_ra_result_string(result));
                jw__boot_resume_fail(state, why, true);
            } else {
                br->next_ms = now + JW_SWITCHER_RESUME_RETRY_MS;
            }
            return;
        }
        /* Let RetroArch enforce active Hardcore here: the shared preference
           can be On while Achievements are Off, and the launch can override
           both. A HARDCORE reply takes the normal failed-resume path below. */
        if (jw_ra_load_state_sync(&ra, &br->load, br->request_id,
                                  JW_BOOT_RESUME_SLOT) != JW_RA_OK) {
            jw__boot_resume_fail(state, "could not send the load", true);
            return;
        }
        br->phase = JW_BOOT_RESUME_PHASE_LOADING;
        br->deadline_ms = jw__monotonic_ms() + JW_BOOT_RESUME_LOAD_TIMEOUT_MS;
        jw_log_info("boot resume: loading slot %d request=%s after %d probe(s)",
                    JW_BOOT_RESUME_SLOT, br->request_id, br->attempts);
        return;
    }

    jw_ra_sync_load_reply reply;
    jw_ra_result result = jw_ra_load_state_sync_poll(&br->load, &reply);
    if (result == JW_RA_TIMEOUT) {
        if (now >= br->deadline_ms) {
            jw__boot_resume_fail(state, "timeout", true);
        }
        return;
    }
    if (result != JW_RA_OK) {
        jw__boot_resume_fail(state, result == JW_RA_SOCKET_ERROR
                                        ? "RetroArch went away" : "malformed reply",
                             true);
        return;
    }
    if (!reply.loaded) {
        char why[64];
        snprintf(why, sizeof(why), "LOAD_STATE_SYNC %s", reply.error);
        jw__boot_resume_fail(state, why, true);
        if (state->boot_resume_notice_pending && strcmp(reply.error, "HARDCORE") == 0) {
            state->boot_resume_notice_hardcore = true;
            jw_log_info("boot resume: the notice names Hardcore");
        }
        return;
    }
    jw__boot_resume_succeed(state, reply.bytes);
}

/* Start-up, once per daemon: settings, cards, catalog and the input proxy are
   up, and no launcher or game has started. The record is deleted before
   anything is decided; one that cannot be deleted is left for a boot that
   can, and nothing else happens. */
static void jw__boot_resume_at_startup(jw_daemon_state *state) {
    jw_boot_resume_record record;
    char reason[64];
    jw_boot_resume_load_result loaded = jw_boot_resume_load(
        state->state_dir, state->platform.platform_id, &record, reason, sizeof(reason));
    if (loaded == JW_BOOT_RESUME_LOAD_ABSENT) {
        return;
    }
    char consume_reason[64];
    if (!jw_boot_resume_consume(state->state_dir, consume_reason, sizeof(consume_reason))) {
        jw_log_warn("boot resume: record could not be removed (%s); not resuming this boot",
                    consume_reason);
        return;
    }
    if (loaded != JW_BOOT_RESUME_LOAD_VALID) {
        jw_log_warn("boot resume: record rejected (%s) and removed", reason);
        return;
    }

    /* The settings poll has not run yet. A busy library gets a short grace:
       nothing waits on the daemon before the launcher starts. */
    bool settings_read = jw__power_hold_save_refresh_setting(state);
    for (int tries = 0; !settings_read && tries < 10; tries++) {
        jw__usleep(50000);
        settings_read = jw__power_hold_save_refresh_setting(state);
    }
    if (!settings_read) {
        jw_log_warn("boot resume: settings unreadable (library busy); "
                    "not resuming this boot request=%s", record.request_id);
        return;
    }
    /* Held B is the player saying no; it is swallowed either way so the
       launcher does not read it as Back. */
    bool bypass = jw_input_proxy_take_held_button(&state->input_proxy,
                                                  JW_INPUT_SHORTCUT_BUTTON_B);
    jw_boot_resume_boot_facts facts = {
        .current_boot_id = state->boot_id,
        .save_setting_on = state->power_hold_save_enabled,
        .resume_setting_on = state->boot_resume_enabled,
        .storage_recovery = jw__boot_resume_storage_recovery(state),
        .bypass_held = bypass,
    };
    jw_boot_resume_decision decision = jw_boot_resume_decide(&record, &facts);
    if (decision != JW_BOOT_RESUME_PROCEED) {
        jw_log_info("boot resume: record discarded (%s) request=%s",
                    jw_boot_resume_decision_name(decision), record.request_id);
        return;
    }

    /* Room for the longest reason: two core ids and two config folders. */
    char why[768] = "";
    if (!jw__boot_resume_launch(state, &record, why, sizeof(why))) {
        if (jw__boot_resume_unconfirmed(state)) {
            jw__boot_resume_fail(state, why, false);
        } else {
            /* Refused before a launch was asked for. */
            jw_log_warn("boot resume: failed (%s) request=%s; the launcher opens with a notice",
                        why, record.request_id);
            state->boot_resume_notice_pending = true;
        }
        return;
    }
    /* Started, blocked, or waiting on service coordination. */
    jw__tick_boot_resume_launch(state);
}

/* Leaf's RetroArch version, from the release build manifest, without its
   leading "v" (RetroArch's VERSION reply has none). Empty when unknown. */
static void jw__leaf_retroarch_version(char *out, size_t out_size) {
    out[0] = '\0';
    const char *platform = getenv("UMRK_PLATFORM_PATH");
    if (!platform || !platform[0]) {
        return;
    }
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/bin/retroarch.build-manifest.json", platform) >=
        (int)sizeof(path)) {
        return;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return;
    }
    static char text[64u * 1024u];
    size_t n = fread(text, 1, sizeof(text) - 1, fp);
    fclose(fp);
    text[n] = '\0';
    cJSON *root = cJSON_Parse(text);
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "retroarch_version");
    if (cJSON_IsString(version) && version->valuestring[0]) {
        const char *v = version->valuestring;
        snprintf(out, out_size, "%s", v[0] == 'v' ? v + 1 : v);
    }
    cJSON_Delete(root);
}

/* LEAF-RA-1 capability probe. A pak frontend is trusted with Leaf's command
   set (and so with the in-game menu, which pauses it first) only once it has
   answered GET_INFO. Short bounded attempts on the loop, like the launch
   warning below, so a slow or silent frontend never delays gameplay. */
static void jw__tick_retroarch_frontend_probe(jw_daemon_state *state) {
    if (!state) return;
    jw_retroarch_session *session = &state->retroarch_session;
    if (!session->active || !session->frontend || session->frontend_probe < 0 ||
        (session->frontend_probe > 0 && session->frontend_version_done) ||
        state->child_kind != JW_CHILD_RETROARCH) {
        return;
    }
    long long now = jw__monotonic_ms();
    if (session->frontend_probe_next_ms > now) return;

    jw_ra_client ra = jw_ra_client_default();
    if (session->frontend_probe > 0) {
        /* Second phase: does it share Leaf's RetroArch version, the only
           case its settings are promoted into the shared config. */
        session->frontend_version_attempts++;
        ra.timeout_ms = JW_FRONTEND_VERSION_TIMEOUT_MS;
        char reply[JW_RA_REPLY_MAX];
        reply[0] = '\0';
        jw_ra_result version_result =
            jw_ra_request_raw(&ra, "VERSION", reply, sizeof(reply));
        /* VERSION replies with the bare version, e.g. "1.22.2\n". */
        reply[strcspn(reply, "\r\n")] = '\0';
        if ((version_result != JW_RA_OK || !reply[0]) &&
            session->frontend_version_attempts < JW_FRONTEND_VERSION_MAX_ATTEMPTS) {
            session->frontend_probe_next_ms = now + JW_FRONTEND_PROBE_RETRY_MS;
            return;
        }
        char leaf_version[64];
        jw__leaf_retroarch_version(leaf_version, sizeof(leaf_version));
        session->frontend_version_done = true;
        session->frontend_version_ok = version_result == JW_RA_OK &&
                                       leaf_version[0] && reply[0] &&
                                       strcmp(reply, leaf_version) == 0;
        jw_log_info("RetroArch frontend version=%s leaf=%s config_promotion=%s",
                    reply[0] ? reply : "(no reply)",
                    leaf_version[0] ? leaf_version : "(unknown)",
                    session->frontend_version_ok ? "on" : "off");
        return;
    }

    session->frontend_probe_attempts++;
    ra.timeout_ms = 100u;
    jw_ra_info info;
    jw_ra_result result = jw_ra_get_info(&ra, &info);
    if (result == JW_RA_OK) {
        session->frontend_probe = 1;
        session->frontend_probe_next_ms = now + JW_FRONTEND_PROBE_RETRY_MS;
        jw_log_info("RetroArch frontend probe: Leaf commands available after %d attempt%s",
                    session->frontend_probe_attempts,
                    session->frontend_probe_attempts == 1 ? "" : "s");
        return;
    }
    if (session->frontend_probe_attempts >= JW_FRONTEND_PROBE_MAX_ATTEMPTS) {
        session->frontend_probe = -1;
        jw_log_warn("RetroArch frontend probe: no GET_INFO reply after %d attempts "
                    "result=%s; Menu opens the frontend's own menu",
                    session->frontend_probe_attempts, jw_ra_result_string(result));
        return;
    }
    session->frontend_probe_next_ms = now + JW_FRONTEND_PROBE_RETRY_MS;
}

/* Deliver launch/recovery warnings through RetroArch exactly once, after its
   command socket becomes ready. Failure is bounded and never delays gameplay. */
static void jw__tick_retroarch_warning(jw_daemon_state *state) {
    if (!state || !state->retroarch_session.warning_pending) return;
    jw_retroarch_session *session = &state->retroarch_session;
    if (!session->active || state->child_kind != JW_CHILD_RETROARCH) {
        session->warning_pending = false;
        return;
    }
    long long now = jw__monotonic_ms();
    if (session->warning_next_ms > now) return;

    session->warning_attempts++;
    jw_ra_client ra = jw_ra_client_default();
    ra.timeout_ms = 100u;
    jw_ra_result result = jw_ra_show_message(&ra, session->warning);
    if (result == JW_RA_OK) {
        jw_log_info("RetroArch warning shown: %s", session->warning);
        session->warning_pending = false;
        return;
    }
    if (session->warning_attempts >= JW_SWITCHER_RESUME_MAX_ATTEMPTS) {
        jw_log_warn("RetroArch warning could not be shown result=%s message=%s",
                    jw_ra_result_string(result), session->warning);
        session->warning_pending = false;
        return;
    }
    session->warning_next_ms = now + JW_SWITCHER_RESUME_RETRY_MS;
}

/* One-shot startup maintenance, armed before the launcher spawns and pulled
   forward by the frontend-ready handler (fallback deadline covers a launcher
   that never reports ready). Tiny maintenance still runs inline; the library
   scan is handed to a worker so the daemon IPC loop stays responsive. */
/* Restore the user's Bluetooth on/off choice at boot. The preference is kept in
   Leaf's own settings DB, not the stock BLUETOOTH_PARAM flag — stock re-enables
   BT and rewrites that flag on boot, so it can't hold a "keep it off". Absent key
   (user never toggled) → leave the boot default alone. */
static void jw__apply_persisted_bluetooth(jw_daemon_state *state) {
    char value[32];
    if (!state || !state->db_path[0] ||
        jw_db_get_setting(state->db_path, "platform.bluetooth_enabled",
                          value, sizeof(value)) != 0 ||
        !value[0]) {
        return;
    }
    bool on = (strcmp(value, "0") != 0);
    if (jw_bt_set_radio(on) == 0) {
        jw_log_info("applied persisted bluetooth enabled=%d", on ? 1 : 0);
    } else {
        jw_log_warn("persisted bluetooth apply failed (enabled=%d)", on ? 1 : 0);
    }
}

static void jw__tick_startup_maintenance(jw_daemon_state *state) {
    if (!state || !state->startup_maintenance_pending) {
        return;
    }
    if (state->shutdown_requested) {
        state->startup_maintenance_pending = false;
        return;
    }
    long long now = jw__monotonic_ms();
    if (state->startup_maintenance_next_ms > now) {
        return;
    }

    if (state->startup_maintenance_phase == 0) {
        long long wifi_start_ms = jw__monotonic_ms();
        int restored = jw_wifi_restore();
        int hardened = jw_wifi_harden();
        jw_log_info("wifi startup maintenance restore=%d harden=%d total_ms=%lld",
                    restored, hardened, jw__monotonic_ms() - wifi_start_ms);
        /* Restore the user's Bluetooth on/off preference (see above): stock boot
           leaves the radio powered regardless, so without this a device the user
           turned BT off on comes back up with it on. */
        jw__apply_persisted_bluetooth(state);
        /* 5-Game Mode keeps Wi-Fi off. The wifi-disabled marker survives a
           reboot, but jw_wifi_restore() above may have re-enabled the radio, so
           force it back off when booting into focus mode. */
        if (state->focus_boot == JW_FOCUS_BOOT_ENTER && jw_wifi_available()) {
            jw_wifi_set_radio(false);
            jw_log_info("5-game mode: Wi-Fi forced off (focus active)");
        }
        state->startup_maintenance_phase = 1;
        state->startup_maintenance_next_ms = jw__monotonic_ms();
        return;
    }

    state->startup_maintenance_pending = false;
    if (state->library_scanned_since_boot) {
        jw_log_info("startup library scan skipped; library already scanned this boot");
        return;
    }

    bool scan_active = false;
    if (state->scan_job.initialized) {
        pthread_mutex_lock(&state->scan_job.mu);
        scan_active = state->scan_job.running || state->scan_job.completed;
        pthread_mutex_unlock(&state->scan_job.mu);
    }
    if (scan_active) {
        jw_log_info("startup library scan already running");
        return;
    }

    if (jw__start_scan_job(state, "at startup (deferred)") < 0) {
        jw_log_warn("startup library scan could not start; launcher will use existing cache if available");
    }
}

static int jw__handle_scan(jw_daemon_state *state, jw_ipc_client *client,
                           cJSON *request) {
    jw_scan_title_list titles = {0};
    const char *parse_error = NULL;
    if (jw__parse_scan_title_groups(state, request, &titles, &parse_error) != 0) {
        return jw__reply_error(client, parse_error ? parse_error : "invalid title groups");
    }
    int accepted = jw__scan_title_list_path_count(&titles);
    int rc = jw__start_scan_job_with_titles(state, "requested", &titles);
    jw__scan_title_list_free(&titles);
    if (rc < 0) {
        return jw__reply_error(client, "scan-library could not start");
    }

    return jw__reply_scan_ok(client,
                             rc > 0 ? "scan-library queued" : "scan-library started",
                             accepted);
}

static int jw__reply_retroarch_session(jw_daemon_state *state, jw_ipc_client *client) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "retroarch-session");
    cJSON_AddBoolToObject(root, "active",
                          jw__has_retroarch_session(state));
    cJSON_AddNumberToObject(root, "resident_switch_max",
                            jw__resident_switch_max());

    if (!jw__has_retroarch_session(state)) {
        cJSON_AddBoolToObject(root, "command_ok", false);
        cJSON_AddStringToObject(root, "command_result", "inactive");
        return jw__reply_json(client, root);
    }

    cJSON_AddStringToObject(root, "system", state->retroarch_session.system);
    cJSON_AddStringToObject(root, "rom_path", state->retroarch_session.rom_path);
    cJSON_AddStringToObject(root, "core_path", state->retroarch_session.core_path);
    cJSON_AddStringToObject(root, "core_id", state->retroarch_session.core_id);
    cJSON_AddStringToObject(root, "core_config_folder",
                           state->retroarch_session.core_config_folder);
    cJSON_AddNumberToObject(root, "resident_switches",
                            state->retroarch_session.resident_switches);

    jw_ra_client ra = jw_ra_client_default();
    jw_ra_info info;
    jw_ra_result result = jw_ra_get_info(&ra, &info);
    cJSON_AddBoolToObject(root, "command_ok", result == JW_RA_OK);
    cJSON_AddStringToObject(root, "command_result", jw_ra_result_string(result));

    if (result == JW_RA_OK) {
        cJSON_AddNumberToObject(root, "disk_count", info.disk_count);
        if (info.disk_slot >= 0) {
            cJSON_AddNumberToObject(root, "disk_slot", info.disk_slot);
        } else {
            cJSON_AddNullToObject(root, "disk_slot");
        }
        cJSON_AddBoolToObject(root, "savestate_supported",
                              info.savestate_supported &&
                              state->retroarch_session.core_config_folder[0]);
        cJSON_AddNumberToObject(root, "state_slot", info.state_slot);
    }

    return jw__reply_json(client, root);
}

static int jw__reply_retroarch_result(jw_ipc_client *client, const char *action,
                                      jw_ra_result result) {
    bool ok = result == JW_RA_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", ok ? "ok" : "error");
    cJSON_AddStringToObject(root, "action", action ? action : "");
    cJSON_AddStringToObject(root, "result", jw_ra_result_string(result));
    cJSON_AddStringToObject(root, "message", jw_ra_result_string(result));
    return jw__reply_json(client, root);
}

static jw_ra_result jw__set_state_slot_delta(const jw_ra_client *ra, int delta) {
    int slot = -1;
    bool supported = false;
    jw_ra_result result = jw_ra_get_state_slot(ra, &slot, &supported);
    if (result != JW_RA_OK) {
        return result;
    }
    if (!supported) {
        return JW_RA_UNSUPPORTED;
    }

    slot += delta;
    if (slot < -1) {
        slot = -1;
    }
    return jw_ra_set_state_slot(ra, slot);
}

static jw_ra_result jw__set_disk_slot_delta(const jw_ra_client *ra, int delta) {
    int count = 0;
    int slot = -1;
    jw_ra_result result = jw_ra_get_disk_count(ra, &count);
    if (result != JW_RA_OK) {
        return result;
    }
    if (count <= 1) {
        return JW_RA_UNSUPPORTED;
    }

    result = jw_ra_get_disk_slot(ra, &slot);
    if (result != JW_RA_OK) {
        return result;
    }
    if (slot < 0 || slot >= count) {
        slot = 0;
    }

    int next = (slot + delta) % count;
    if (next < 0) {
        next += count;
    }
    return jw_ra_set_disk_slot(ra, next);
}

static void jw__osd_storage_read_only(jw_daemon_state *state);

static int jw__handle_retroarch_action(jw_daemon_state *state, jw_ipc_client *client,
                                       cJSON *root) {
    cJSON *action_json = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (!cJSON_IsString(action_json) || !action_json->valuestring ||
        !action_json->valuestring[0]) {
        return jw__reply_error(client, "missing RetroArch action");
    }
    if (!jw__has_retroarch_session(state)) {
        return jw__reply_error(client, "no active RetroArch session");
    }

    const char *action = action_json->valuestring;
    /* RetroArch writes savestates asynchronously and, on a read-only card,
       stalls at "Saving state 0%" with nothing to tell the player why. Refuse
       an explicit save up front; Save & Quit skips the save and still quits. */
    char states_reason[JW_STORAGE_REASON_MAX] = "";
    bool states_writable = !state->active_game.states_path[0] ||
                           jw_storage_path_writable(state->active_game.states_path,
                                                    states_reason, sizeof(states_reason));
    if (!states_writable && strcmp(action, "save-state") == 0) {
        jw_log_warn("retroarch-action save-state refused: %s (%s)", states_reason,
                    state->active_game.states_path);
        jw__osd_storage_read_only(state);
        return jw__reply_error(client, "SD card is read-only");
    }
    cJSON *value_json = cJSON_GetObjectItemCaseSensitive(root, "value");
    bool has_value = cJSON_IsNumber(value_json);
    int value = has_value ? value_json->valueint : 0;
    jw_ra_client ra = jw_ra_client_default();
    jw_ra_result result = JW_RA_UNSUPPORTED;

    if (strcmp(action, "continue") == 0) {
        /* Explicit UNPAUSE, symmetric with the explicit PAUSE on open. The
           status-polling jw_ra_resume() could leave the game stuck paused when
           GET_STATUS misreports (e.g. right after a state load). */
        result = jw__resume_game_after_menu(state, &ra);
        if (result == JW_RA_OK) {
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "settings") == 0) {
        result = jw_ra_open_menu(&ra);
        if (result == JW_RA_OK) {
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "shader-settings") == 0) {
        result = jw_ra_open_menu(&ra);
        if (result == JW_RA_OK) {
            long long now = jw__monotonic_ms();
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
            state->advanced_shader_pending = true;
            state->advanced_shader_menu_seen = false;
            state->advanced_shader_destination_sent = false;
            state->advanced_shader_started_ms = now;
            state->advanced_shader_next_poll_ms = now;
        }
    } else if (strcmp(action, "quit") == 0) {
        result = jw_ra_quit(&ra);
        if (result == JW_RA_OK) {
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "save-and-quit") == 0) {
        /* Snapshot into the reserved game-switcher slot so the game returns to
           the switcher/Recents resumable and with a screenshot, then quit. The
           save is best-effort: if savestates are unsupported or the save fails,
           quit anyway so the user is never stranded. Mirrors the save block in
           jw__request_switch_game. */
        jw_ra_info info;
        memset(&info, 0, sizeof(info));
        if (!states_writable) {
            jw_log_warn("save-and-quit: states are not writable (%s); quitting without save",
                        states_reason);
        } else if (state->retroarch_session.resume_unconfirmed) {
            /* Slot 99 is the power-off position still being resumed. */
            jw_log_warn("save-and-quit: the resumed game has not loaded; quitting without save");
        } else if (state->retroarch_session.core_config_folder[0] &&
            jw_ra_get_info(&ra, &info) == JW_RA_OK && info.savestate_supported) {
            char reply[JW_RA_REPLY_MAX];
            jw_ra_result sv = jw_ra_save_state_slot(
                &ra, JW_RA_GAME_SWITCHER_STATE_SLOT, reply, sizeof(reply));
            if (sv != JW_RA_OK) {
                jw_log_warn("save-and-quit: slot %d save failed result=%s; quitting anyway",
                            JW_RA_GAME_SWITCHER_STATE_SLOT, jw_ra_result_string(sv));
            } else {
                jw_log_info("save-and-quit: saved state slot=%d",
                            JW_RA_GAME_SWITCHER_STATE_SLOT);
                /* Let the async state write finish before we quit RetroArch,
                   otherwise the resume state is truncated and crashes on load. */
                jw__wait_for_savestate_write(state, JW_RA_GAME_SWITCHER_STATE_SLOT);
            }
        } else {
            jw_log_info("save-and-quit: savestates unavailable; quitting without save");
        }
        result = jw_ra_quit(&ra);
        if (result == JW_RA_OK) {
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "reset") == 0) {
        result = jw_ra_reset(&ra);
        if (result == JW_RA_OK) {
            /* Reset/Save/Load close the menu (the UI sets running=false), so
               resume the game now. The resident menu never exits, so the old
               resume-on-menu-exit path can't do it for us. */
            jw__resume_game_after_menu(state, &ra);
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "save-state") == 0) {
        if (!state->retroarch_session.core_config_folder[0]) {
            return jw__reply_error(client, "savestate namespace unavailable");
        }
        if (has_value) {
            /* value >= -1: the slot variant handles the auto slot (-1) too. */
            char reply[JW_RA_REPLY_MAX];
            result = jw_ra_save_state_slot(&ra, value, reply, sizeof(reply));
        } else {
            result = jw_ra_save_state(&ra);
        }
        if (result == JW_RA_OK) {
            jw__resume_game_after_menu(state, &ra);
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "load-state") == 0) {
        if (!state->retroarch_session.core_config_folder[0]) {
            return jw__reply_error(client, "savestate namespace unavailable");
        }
        if (has_value) {
            /* value >= -1: the slot variant handles the auto slot (-1) too. */
            char reply[JW_RA_REPLY_MAX];
            result = jw_ra_load_state_slot(&ra, value, reply, sizeof(reply));
        } else {
            result = jw_ra_load_state(&ra);
        }
        if (result == JW_RA_OK) {
            jw__resume_game_after_menu(state, &ra);
            state->retroarch_resume_on_menu_exit = false;
            state->menu_visible = false;
        }
    } else if (strcmp(action, "state-slot-prev") == 0) {
        if (!state->retroarch_session.core_config_folder[0]) {
            return jw__reply_error(client, "savestate namespace unavailable");
        }
        result = jw__set_state_slot_delta(&ra, -1);
    } else if (strcmp(action, "state-slot-next") == 0) {
        if (!state->retroarch_session.core_config_folder[0]) {
            return jw__reply_error(client, "savestate namespace unavailable");
        }
        result = jw__set_state_slot_delta(&ra, +1);
    } else if (strcmp(action, "disk-prev") == 0) {
        result = jw__set_disk_slot_delta(&ra, -1);
    } else if (strcmp(action, "disk-next") == 0) {
        result = jw__set_disk_slot_delta(&ra, +1);
    }

    if (result == JW_RA_OK &&
        (strcmp(action, "quit") == 0 || strcmp(action, "save-and-quit") == 0)) {
        state->retroarch_quit_deadline_ms = jw__monotonic_ms() + JW_RETROARCH_STUCK_QUIT_MS;
    }

    jw_log_info("retroarch-action requested action=%s result=%s",
                action, jw_ra_result_string(result));
    return jw__reply_retroarch_result(client, action, result);
}

/* The player chose to quit and RetroArch is still here long after. Never leave
   them stranded in a game that will not close. */
static void jw__tick_retroarch_stuck_quit(jw_daemon_state *state) {
    if (!state || state->retroarch_quit_deadline_ms <= 0) {
        return;
    }
    if (!jw__has_retroarch_session(state)) {
        state->retroarch_quit_deadline_ms = 0;
        return;
    }
    if (jw__monotonic_ms() < state->retroarch_quit_deadline_ms) {
        return;
    }
    state->retroarch_quit_deadline_ms = 0;
    jw_log_warn("retroarch: quit requested %llds ago but RetroArch is still running; forcing it closed",
                JW_RETROARCH_STUCK_QUIT_MS / 1000);
    (void)jw__signal_tracked_game_group(state, SIGKILL);
}

/* Shader operations for the in-game picker.
 *
 * Deliberately a closed request rather than a raw RetroArch command over the UI
 * IPC: operation, path and scope are the only inputs, each validated here. A
 * general passthrough would let anything that can reach the socket drive
 * RetroArch directly. */
static int jw__reply_shader_result(jw_ipc_client *client, const char *operation,
                                   jw_ra_result result,
                                   jw_ra_shader_outcome outcome,
                                   const char *path) {
    static const char *outcome_names[] = {
        "ok", "none", "absent", "missing", "unsupported", "apply", "error"
    };
    bool transport_ok = result == JW_RA_OK;
    cJSON *root = cJSON_CreateObject();
    /* An apply failure is a successful exchange: the picker needs to tell
       "RetroArch says this shader will not compile" from "RetroArch did not
       answer", because only the second is worth retrying. */
    cJSON_AddStringToObject(root, "type", transport_ok ? "ok" : "error");
    cJSON_AddStringToObject(root, "operation", operation ? operation : "");
    cJSON_AddStringToObject(root, "result", jw_ra_result_string(result));
    if (transport_ok) {
        size_t index = (size_t)outcome;
        cJSON_AddStringToObject(
            root, "outcome",
            index < sizeof(outcome_names) / sizeof(outcome_names[0])
                ? outcome_names[index] : "error");
    } else {
        cJSON_AddStringToObject(root, "message", jw_ra_result_string(result));
    }
    if (path && path[0]) {
        cJSON_AddStringToObject(root, "path", path);
    }
    return jw__reply_json(client, root);
}

static bool jw__shader_scope_from_string(const char *text,
                                         jw_ra_shader_scope *out) {
    if (!text) {
        return false;
    }
    if (strcmp(text, "game") == 0) {
        *out = JW_RA_SHADER_SCOPE_GAME;
    } else if (strcmp(text, "parent") == 0) {
        *out = JW_RA_SHADER_SCOPE_PARENT;
    } else if (strcmp(text, "core") == 0) {
        *out = JW_RA_SHADER_SCOPE_CORE;
    } else if (strcmp(text, "global") == 0) {
        *out = JW_RA_SHADER_SCOPE_GLOBAL;
    } else {
        return false;
    }
    return true;
}

static int jw__handle_retroarch_shader(jw_daemon_state *state,
                                       jw_ipc_client *client, cJSON *root) {
    cJSON *op_json = cJSON_GetObjectItemCaseSensitive(root, "operation");
    if (!cJSON_IsString(op_json) || !op_json->valuestring[0]) {
        return jw__reply_error(client, "missing shader operation");
    }
    if (!jw__has_retroarch_session(state)) {
        return jw__reply_error(client, "no active RetroArch session");
    }

    const char *operation = op_json->valuestring;
    jw_ra_client ra = jw_ra_client_default();
    ra.timeout_ms = JW_RA_SHADER_TIMEOUT_MS;
    jw_ra_shader_outcome outcome = JW_RA_SHADER_ERR;
    jw_ra_result result = JW_RA_UNSUPPORTED;
    char path[PATH_MAX];
    char relative[PATH_MAX];
    const char *scope_log = "-";

    path[0] = '\0';
    relative[0] = '\0';

    if (strcmp(operation, "get") == 0) {
        result = jw_ra_get_shader(&ra, &outcome, path, sizeof(path));
    } else if (strcmp(operation, "set") == 0 ||
               strcmp(operation, "restore") == 0) {
        cJSON *path_json = cJSON_GetObjectItemCaseSensitive(root, "path");
        char resolved[PATH_MAX];
        if (!cJSON_IsString(path_json) || !path_json->valuestring[0]) {
            return jw__reply_error(client, "missing shader path");
        }
        bool permitted = strcmp(operation, "set") == 0
            ? jw_retroarch_shader_path_is_recommended(
                  path_json->valuestring, resolved, sizeof(resolved), relative,
                  sizeof(relative))
            : jw_retroarch_shader_path_is_restorable(
                  path_json->valuestring,
                  state->retroarch_session.config_path,
                  resolved, sizeof(resolved), relative, sizeof(relative));
        if (!permitted) {
            jw_log_warn("retroarch-shader rejected %s path", operation);
            return jw__reply_error(client, "shader path not permitted");
        }
        snprintf(path, sizeof(path), "%s", resolved);
        result = jw_ra_set_shader(&ra, resolved, &outcome);
    } else if (strcmp(operation, "clear") == 0) {
        result = jw_ra_clear_shader(&ra, &outcome);
    } else if (strcmp(operation, "save") == 0 ||
               strcmp(operation, "remove") == 0) {
        cJSON *scope_json = cJSON_GetObjectItemCaseSensitive(root, "scope");
        jw_ra_shader_scope scope;
        /* Scopes only. A path here would let a caller write a preset anywhere
           RetroArch can reach. */
        if (!cJSON_IsString(scope_json) ||
            !jw__shader_scope_from_string(scope_json->valuestring, &scope)) {
            return jw__reply_error(client, "invalid shader scope");
        }
        scope_log = scope_json->valuestring;
        result = strcmp(operation, "save") == 0
                     ? jw_ra_save_shader_preset(&ra, scope, &outcome)
                     : jw_ra_remove_shader_preset(&ra, scope, &outcome);
    } else {
        return jw__reply_error(client, "unknown shader operation");
    }

    jw_log_info("retroarch-shader operation=%s scope=%s preset=%s result=%s",
                operation, scope_log, relative[0] ? relative : "-",
                jw_ra_result_string(result));
    return jw__reply_shader_result(client, operation, result, outcome,
                                   strcmp(operation, "get") == 0 ? path : NULL);
}

static pid_t jw__suspend_request_pid(jw_ipc_client *client, cJSON *request) {
    pid_t pid = 0;
    if (jw_ipc_client_peer_pid(client, &pid) == 0 && pid > 0) return pid;
#if defined(__APPLE__)
    /* Native/mock-only fallback for platforms without Unix peer credentials. */
    cJSON *test_pid = cJSON_GetObjectItemCaseSensitive(request, "test_pid");
    if (cJSON_IsNumber(test_pid) && test_pid->valueint > 0)
        return (pid_t)test_pid->valueint;
#else
    (void)request;
#endif
    return 0;
}

static int jw__reply_suspend_acquired(jw_ipc_client *client, const char *token,
                                      int active_count) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "suspend-inhibit-acquired");
    cJSON_AddStringToObject(reply, "token", token);
    cJSON_AddNumberToObject(reply, "active_count", active_count);
    return jw__reply_json(client, reply);
}

static int jw__reply_suspend_status(jw_daemon_state *state, jw_ipc_client *client) {
    jw_suspend_inhibitor_reap(&state->suspend_inhibitor);
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "suspend-inhibit-status");
    cJSON_AddNumberToObject(reply, "active_count",
                            jw_suspend_inhibitor_count(&state->suspend_inhibitor));
    cJSON *holders = cJSON_AddArrayToObject(reply, "holders");
    long long now = jw__monotonic_ms();
    for (int i = 0; i < JW_SUSPEND_INHIBIT_MAX_LEASES; i++) {
        const jw_suspend_lease *lease = &state->suspend_inhibitor.leases[i];
        if (!lease->active) continue;
        cJSON *holder = cJSON_CreateObject();
        cJSON_AddNumberToObject(holder, "pid", (double)lease->pid);
        cJSON_AddStringToObject(holder, "scope", lease->scope);
        cJSON_AddStringToObject(holder, "reason", lease->reason);
        cJSON_AddNumberToObject(holder, "age_ms",
                                now > lease->acquired_ms ? (double)(now - lease->acquired_ms) : 0);
        cJSON_AddItemToArray(holders, holder);
    }
    cJSON_AddStringToObject(reply, "pending_sleep",
        state->suspend_policy.pending == JW_SUSPEND_PENDING_EXPLICIT ? "explicit" :
        state->suspend_policy.pending == JW_SUSPEND_PENDING_AUTO ? "automatic" : "none");
    return jw__reply_json(client, reply);
}

/* ── SD card health ───────────────────────────────────────────────── */

#define JW_STORAGE_PROBE_INTERVAL_MS 2000
#define JW_STORAGE_TOOL_TIMEOUT_MS 30000

static const jw_storage_health_slot *jw__storage_slot(const jw_daemon_state *state,
                                                      const char *source_id) {
    if (!state || !source_id) {
        return NULL;
    }
    for (int i = 0; i < state->storage_monitor.count; i++) {
        const jw_storage_health_slot *slot = &state->storage_monitor.slots[i];
        if (slot->valid && strcmp(slot->health.source_id, source_id) == 0) {
            return slot;
        }
    }
    return NULL;
}

/* Acknowledgement belongs to a card and a boot, not to a mount path: the two
   MLP1 paths swap between boots and a replaced card must warn again. */
static void jw__storage_ack_key(const jw_storage_health *health, char *out, size_t out_size) {
    char boot_id[64] = "unknown";
    FILE *fp = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (fp) {
        if (fgets(boot_id, sizeof(boot_id), fp)) {
            boot_id[strcspn(boot_id, "\r\n")] = '\0';
        }
        fclose(fp);
    }
    if (health->uuid[0]) {
        snprintf(out, out_size, "%s uuid:%s", boot_id, health->uuid);
    } else {
        snprintf(out, out_size, "%s dev:%u:%u", boot_id, health->major, health->minor);
    }
}

static bool jw__storage_warning_acked(const jw_daemon_state *state,
                                      const jw_storage_health *health) {
    char path[PATH_MAX];
    char key[160];
    char line[192];
    snprintf(path, sizeof(path), "%s/storage-warning-acks", state->runtime_dir);
    jw__storage_ack_key(health, key, sizeof(key));
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return false;
    }
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, key) == 0) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

static int jw__storage_warning_ack_write(const jw_daemon_state *state,
                                         const jw_storage_health *health) {
    char path[PATH_MAX];
    char key[160];
    snprintf(path, sizeof(path), "%s/storage-warning-acks", state->runtime_dir);
    jw__storage_ack_key(health, key, sizeof(key));
    FILE *fp = fopen(path, "a");
    if (!fp) {
        return -1;
    }
    int rc = fprintf(fp, "%s\n", key) < 0 ? -1 : 0;
    if (fclose(fp) != 0) {
        rc = -1;
    }
    return rc;
}

static bool jw__storage_child_in_front(const jw_daemon_state *state) {
    return state->child_pid > 0 &&
           (state->child_kind == JW_CHILD_RETROARCH ||
            state->child_kind == JW_CHILD_EMULATOR ||
            state->child_kind == JW_CHILD_APP);
}

static void jw__storage_status_add_health(jw_daemon_state *state, cJSON *root,
                                          const char *source_id) {
    if (!state || !root) {
        return;
    }
    cJSON_AddNumberToObject(root, "health_generation",
                            (double)state->storage_monitor.generation);
    const jw_storage_health_slot *slot = jw__storage_slot(state, source_id);
    if (!slot) {
        cJSON_AddStringToObject(root, "access", "unknown");
        cJSON_AddStringToObject(root, "cause", "unknown");
        cJSON_AddStringToObject(root, "repair", "none");
        cJSON_AddBoolToObject(root, "repair_supported", false);
        cJSON_AddStringToObject(root, "repair_unavailable_reason", "unavailable");
        return;
    }
    const jw_storage_health *h = &slot->health;
    cJSON_AddStringToObject(root, "access", jw_storage_access_name(h->access));
    cJSON_AddStringToObject(root, "cause", jw_storage_cause_name(h->cause));
    cJSON_AddStringToObject(root, "repair", jw_storage_repair_name(h->repair));
    cJSON_AddStringToObject(root, "fs_type", h->fs_type);
    cJSON_AddStringToObject(root, "uuid", h->uuid);
    cJSON_AddStringToObject(root, "volume_label", h->label);
    if (h->kernel_message[0]) {
        cJSON_AddStringToObject(root, "kernel_message", h->kernel_message);
    }
    cJSON_AddBoolToObject(root, "dirty_at_boot", h->dirty_at_boot);
    cJSON_AddBoolToObject(root, "block_write_protected", h->block_write_protected);

    jw_platform_storage_repair_capability capability;
    jw_platform_get_storage_repair_capability(&state->platform, h->fs_type, h->uuid,
                                          h->block_write_protected, &capability);
    cJSON_AddBoolToObject(root, "repair_supported", capability.supported);
    cJSON_AddStringToObject(root, "repair_unavailable_reason",
                            capability.unavailable_reason);
    cJSON_AddStringToObject(root, "repair_mode", capability.mode);
    cJSON_AddBoolToObject(root, "warning_pending",
                          h->mounted && h->access == JW_STORAGE_ACCESS_READ_ONLY &&
                              !jw__storage_warning_acked(state, h));
    jw_platform_status power;
    jw_platform_get_status(&state->platform, &power);
    cJSON_AddBoolToObject(root, "external_power", power.charging == 1);
    jw__json_add_int_or_null(root, "battery_percent", power.battery_percent);

    jw_storage_probe_env env;
    jw_storage_probe_env_default(&env);
    char hold_trigger[32];
    if (h->repair != JW_STORAGE_REPAIR_NONE &&
        jw_storage_repair_hold_trigger(&env, h->uuid, hold_trigger, sizeof(hold_trigger))) {
        cJSON_AddStringToObject(root, "hold_trigger", hold_trigger);
    }
    jw_storage_repair_result result;
    if (jw_storage_repair_last_result(&env, h->uuid, &result)) {
        cJSON *last = cJSON_AddObjectToObject(root, "last_repair");
        if (last) {
            cJSON_AddStringToObject(last, "request_id", result.request_id);
            cJSON_AddStringToObject(last, "outcome", result.outcome);
            cJSON_AddStringToObject(last, "mount_state", result.mount_state);
            cJSON_AddStringToObject(last, "mode", result.mode);
            cJSON_AddStringToObject(last, "origin", result.origin);
            if (result.trigger[0]) {
                cJSON_AddStringToObject(last, "trigger", result.trigger);
            }
            cJSON_AddBoolToObject(last, "changes_complete", result.changes_complete);
            cJSON_AddNumberToObject(last, "reported_changes", result.reported_change_count);
            cJSON_AddBoolToObject(last, "acknowledged", result.acknowledged);
        }
    }
}

/* The card flipped read-only while a game or app is in front: one banner.
   Emulators are third-party; nothing here can promise they handle it. */
static void jw__osd_storage_read_only(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    if (!jw__osd_enabled(state) || state->shutdown_requested || g_shutdown_requested) {
        jw_osd_client_prompt_lost(&state->pico8_exit_confirm_until_ms);
        return;
    }
    jw_osd_client client = jw__osd_client(state);
    if (jw_osd_client_show_stage(&client, &state->pico8_exit_confirm_until_ms,
                                 "storage-read-only", 0) != 0) {
        jw_log_warn("storage: read-only OSD request failed");
    }
}

static void jw__storage_health_refresh(jw_daemon_state *state) {
    jw_platform_storage_root roots[JW_STORAGE_MAX_SOURCES];
    int count = jw_platform_storage_roots(&state->platform, roots, JW_STORAGE_MAX_SOURCES);
    jw_storage_probe_env env;
    jw_storage_probe_env_default(&env);
    char *kernel_log = NULL;
    bool kernel_read = false;
    bool changed = false;
    bool newly_read_only = false;

    for (int i = 0; i < count; i++) {
        jw_storage_health probe;
        bool newly = false;
        jw_storage_health_probe(&env, roots[i].source_id, roots[i].root, NULL, &probe);
        /* Hotplug refuses to mount a held second card. Keep its identity so
           Check SD card stays available after a repair on a computer. */
        if (!probe.mounted &&
            strcmp(roots[i].source_id, JW_PLATFORM_STORAGE_SECONDARY_ID) == 0) {
            (void)jw_storage_health_probe_unmounted_hold(&env, &probe);
        }
        changed |= jw_storage_health_monitor_update(&state->storage_monitor, i, &probe,
                                                    &newly);
        jw_storage_health_slot *slot = &state->storage_monitor.slots[i];
        /* Kernel evidence only at startup and on a new read-only observation;
           it explains the state, it never decides it. */
        if (probe.mounted && (newly || !slot->kernel_checked)) {
            if (!kernel_read) {
                kernel_log = jw_storage_kernel_log_read();
                kernel_read = true;
            }
            if (kernel_log) {
                bool ignored = false;
                jw_storage_health_probe(&env, roots[i].source_id, roots[i].root,
                                        kernel_log, &probe);
                changed |= jw_storage_health_monitor_update(&state->storage_monitor, i,
                                                            &probe, &ignored);
            }
            slot->kernel_checked = true;
        }
        newly_read_only |= newly;
    }
    free(kernel_log);

#ifdef PLATFORM_MLP1
    if (!state->storage_log_redirected &&
        jw_log_redirect_if_read_only(JW_STORAGE_MLP1_LOG_DIR, "jawakad") == 1) {
        state->storage_log_redirected = true;
        jw_log_warn("storage: session log card is read-only; daemon output continues in "
                    JW_STORAGE_MLP1_LOG_DIR "/jawakad.log (children started earlier "
                    "keep their old log)");
    }
#endif
    if (!changed) {
        return;
    }
    jw_scrape_storage_recheck();
    if (newly_read_only && jw__storage_child_in_front(state)) {
        jw__osd_storage_read_only(state);
    }
    if (state->storage_scan_deferred && state->db &&
        jw_storage_path_writable(state->db_path, NULL, 0)) {
        state->storage_scan_deferred = false;
        if (jw__start_scan_job(state, "after storage became writable") < 0) {
            jw_log_warn("storage: deferred library rescan could not start");
        }
    }
}

static void jw__tick_storage_health(jw_daemon_state *state) {
    long long now = jw__monotonic_ms();
    bool requested = jw_storage_take_refresh_request();
    if (!requested && now < state->storage_next_probe_ms) {
        return;
    }
    state->storage_next_probe_ms = now + JW_STORAGE_PROBE_INTERVAL_MS;
    jw__storage_health_refresh(state);

    if (!state->storage_repair_rescan_done) {
        /* A verified repair can change file names and sizes; rescan once. The
           scan itself defers while another card keeps the DB read-only. */
        state->storage_repair_rescan_done = true;
        jw_storage_probe_env env;
        jw_storage_probe_env_default(&env);
        jw_storage_repair_result result;
        for (int i = 0; i < state->storage_monitor.count; i++) {
            const jw_storage_health *health = &state->storage_monitor.slots[i].health;
            if (jw_storage_repair_last_result(&env, health->uuid, &result) &&
                !result.acknowledged && health->access == JW_STORAGE_ACCESS_READ_WRITE &&
                health->repair == JW_STORAGE_REPAIR_NONE &&
                (strcmp(result.outcome, "repaired") == 0 || strcmp(result.outcome, "clean") == 0) &&
                strcmp(result.mount_state, "read-write") == 0) {
                if (jw__start_scan_job(state, "after SD card check") < 0) {
                    jw_log_warn("storage: post-check library rescan could not start");
                }
                break;
            }
        }
    }
}

static int jw__handle_storage_warning_ack(jw_daemon_state *state, jw_ipc_client *client,
                                          cJSON *root) {
    cJSON *source_json = cJSON_GetObjectItemCaseSensitive(root, "source");
    const char *source = cJSON_IsString(source_json) && source_json->valuestring
        ? source_json->valuestring : JW_PLATFORM_STORAGE_LAUNCHER_ID;
    const jw_storage_health_slot *slot = jw__storage_slot(state, source);
    if (!slot || !slot->health.mounted) {
        return jw__reply_error(client, "storage source unavailable");
    }
    if (jw__storage_warning_ack_write(state, &slot->health) != 0) {
        return jw__reply_error(client, "could not record acknowledgement");
    }
    state->storage_monitor.generation++;
    return jw__reply_ok(client, "storage-warning-ack", NULL);
}

#ifdef PLATFORM_MLP1
/* Run the switcher's repair tool with a bounded wait. Output (first bytes of
   stdout+stderr) is returned for the refusal message. */
static int jw__storage_run_repair_tool(char *const argv[], char *out, size_t out_size) {
    int pipefd[2];
    if (out && out_size) {
        out[0] = '\0';
    }
    if (pipe(pipefd) != 0) {
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        if (chdir("/") != 0) {
            _exit(126);
        }
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execv(JW_STORAGE_MLP1_REPAIR_TOOL, argv);
        _exit(127);
    }
    close(pipefd[1]);
    size_t used = 0;
    long long deadline = jw__monotonic_ms() + JW_STORAGE_TOOL_TIMEOUT_MS;
    for (;;) {
        struct pollfd pfd = { .fd = pipefd[0], .events = POLLIN };
        long long left = deadline - jw__monotonic_ms();
        if (left <= 0) {
            break;
        }
        int ready = poll(&pfd, 1, (int)(left > 1000 ? 1000 : left));
        if (ready < 0 && errno != EINTR) {
            break;
        }
        if (ready <= 0) {
            continue;
        }
        char buf[256];
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (out && used + 1 < out_size) {
            size_t take = (size_t)n < out_size - 1 - used ? (size_t)n : out_size - 1 - used;
            memcpy(out + used, buf, take);
            used += take;
            out[used] = '\0';
        }
    }
    close(pipefd[0]);
    /* EOF can arrive before the tool has exited. Keep waiting within the same
       deadline: killing it here turned committed requests into failures. */
    int status = 0;
    for (;;) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (waited < 0 && errno != EINTR) {
            return -1;
        }
        if (jw__monotonic_ms() >= deadline) {
            break;
        }
        jw__usleep(10000);
    }
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* retry */
    }
    return -1;
}
#endif

static int jw__handle_storage_repair_request(jw_daemon_state *state, jw_ipc_client *client,
                                             cJSON *root) {
    if (jw_delete_job_busy(state->delete_job))
        return jw__reply_error(client, "Wait for ROM deletion before repairing storage");
    cJSON *source_json = cJSON_GetObjectItemCaseSensitive(root, "source");
    cJSON *mode_json = cJSON_GetObjectItemCaseSensitive(root, "mode");
    cJSON *battery_json = cJSON_GetObjectItemCaseSensitive(root, "allow_battery");
    bool allow_battery = cJSON_IsTrue(battery_json);
    const char *source = cJSON_IsString(source_json) && source_json->valuestring
        ? source_json->valuestring : JW_PLATFORM_STORAGE_LAUNCHER_ID;
    const char *mode = cJSON_IsString(mode_json) && mode_json->valuestring
        ? mode_json->valuestring : "repair";
    if (strcmp(mode, "repair") != 0 && strcmp(mode, "check") != 0) {
        return jw__reply_error(client, "invalid repair mode");
    }
    if ((battery_json && !cJSON_IsBool(battery_json)) ||
        (allow_battery && strcmp(mode, "repair") != 0)) {
        return jw__reply_error(client, "invalid battery override");
    }
    const jw_storage_health_slot *slot = jw__storage_slot(state, source);
    /* An unmounted card qualifies only as a held card with a known identity. */
    if (!slot || (!slot->health.mounted &&
                  (slot->health.repair == JW_STORAGE_REPAIR_NONE ||
                   !slot->health.uuid[0] || !slot->health.device[0]))) {
        return jw__reply_error(client, "not-found");
    }
    const jw_storage_health *h = &slot->health;
    jw_platform_storage_repair_capability capability;
    jw_platform_get_storage_repair_capability(&state->platform, h->fs_type, h->uuid,
                                          h->block_write_protected, &capability);
    if (!capability.supported) {
        return jw__reply_error(client, capability.unavailable_reason);
    }
    if (state->active_game.active || jw__storage_child_in_front(state)) {
        return jw__reply_error(client, "busy");
    }
    if (strcmp(mode, "repair") == 0) {
        jw_platform_status power;
        jw_platform_get_status(&state->platform, &power);
        if (power.charging != 1 &&
            (!allow_battery || power.battery_percent < JW_STORAGE_REPAIR_MIN_BATTERY_PERCENT)) {
            return jw__reply_error(client, "power-required");
        }
    }
#ifdef PLATFORM_MLP1
    char fs_type[JW_STORAGE_FS_TYPE_MAX];
    char uuid[JW_STORAGE_UUID_MAX];
    char device[JW_STORAGE_DEVICE_MAX];
    char source_copy[JW_STORAGE_SOURCE_ID_MAX];
    char mode_copy[16];
    snprintf(fs_type, sizeof(fs_type), "%s", h->fs_type);
    snprintf(uuid, sizeof(uuid), "%s", h->uuid);
    snprintf(device, sizeof(device), "%s", h->device);
    snprintf(source_copy, sizeof(source_copy), "%s", source);
    snprintf(mode_copy, sizeof(mode_copy), "%s", mode);
    char *argv[] = {
        (char *)"umrk-storage-repair", (char *)"request",
        (char *)"--uuid", uuid, (char *)"--source", source_copy,
        (char *)"--fs-type", fs_type, (char *)"--device", device,
        (char *)"--mode", mode_copy, allow_battery ? (char *)"--allow-battery" : NULL, NULL,
    };
    char output[256];
    int rc = jw__storage_run_repair_tool(argv, output, sizeof(output));
    output[strcspn(output, "\r\n")] = '\0';
    if (rc != 0) {
        jw_log_warn("storage: %s request for %s (%s) refused rc=%d: %s", mode, source, uuid,
                    rc, output[0] ? output : "no output");
        return jw__reply_error(client, output[0] ? output : "failed");
    }
    jw_log_warn("storage: %s request committed for %s uuid=%s; restarting", mode, source, uuid);
    state->storage_monitor.generation++;
    if (!jw__request_power_transition(state, JW_PLATFORM_ACTION_REBOOT, "storage-check")) {
        return jw__reply_error(client, "power-handoff-missing");
    }
    return jw__reply_ok(client, "storage-repair-request", NULL);
#else
    return jw__reply_error(client, "unsupported-platform");
#endif
}

static int jw__handle_storage_repair_result_ack(jw_daemon_state *state, jw_ipc_client *client,
                                                cJSON *root) {
    cJSON *id_json = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    const char *id = cJSON_IsString(id_json) && id_json->valuestring ? id_json->valuestring : "";
    size_t id_len = strlen(id);
    if (id_len == 0 || id_len >= JW_STORAGE_REQUEST_ID_MAX) {
        return jw__reply_error(client, "invalid request id");
    }
    for (const char *p = id; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') {
            return jw__reply_error(client, "invalid request id");
        }
    }
    jw_storage_probe_env env;
    jw_storage_probe_env_default(&env);
    if (!env.repair_dir) {
        return jw__reply_error(client, "unsupported-platform");
    }
    char dir[PATH_MAX];
    char path[PATH_MAX];
    if (snprintf(dir, sizeof(dir), "%s/acks", env.repair_dir) >= (int)sizeof(dir) ||
        snprintf(path, sizeof(path), "%s/%s", dir, id) >= (int)sizeof(path)) {
        return jw__reply_error(client, "invalid request id");
    }
    (void)mkdir(dir, 0755);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return jw__reply_error(client, "could not record acknowledgement");
    }
    int rc = fsync(fd);
    close(fd);
    int dir_fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (dir_fd >= 0) {
        if (fsync(dir_fd) != 0) {
            rc = -1;
        }
        close(dir_fd);
    }
    if (rc != 0) {
        return jw__reply_error(client, "could not record acknowledgement");
    }
    state->storage_monitor.generation++;
    return jw__reply_ok(client, "storage-repair-result-ack", NULL);
}

static int jw__handle_suspend_inhibit(jw_daemon_state *state,
                                      jw_ipc_client *client,
                                      cJSON *request,
                                      const char *type) {
    if (strcmp(type, "suspend-inhibit-status") == 0)
        return jw__reply_suspend_status(state, client);

    pid_t pid = jw__suspend_request_pid(client, request);
    if (pid <= 0) return jw__reply_error(client, "Unix peer pid unavailable");

    if (strcmp(type, "suspend-inhibit-acquire") == 0) {
        cJSON *scope = cJSON_GetObjectItemCaseSensitive(request, "scope");
        cJSON *reason = cJSON_GetObjectItemCaseSensitive(request, "reason");
        if (!cJSON_IsString(scope) || !scope->valuestring ||
            !cJSON_IsString(reason) || !reason->valuestring)
            return jw__reply_error(client, "missing suspend inhibitor scope or reason");
        char token[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
        jw_suspend_lease_result result = jw_suspend_inhibitor_acquire(
            &state->suspend_inhibitor, pid, scope->valuestring, reason->valuestring,
            jw__monotonic_ms(), token);
        if (result != JW_SUSPEND_LEASE_OK)
            return jw__reply_error(client, jw_suspend_lease_result_name(result));
        int count = jw_suspend_inhibitor_count(&state->suspend_inhibitor);
        jw_log_info("suspend-inhibit: acquired pid=%d scope=%s reason=%s active=%d",
                    (int)pid, scope->valuestring, reason->valuestring, count);
        return jw__reply_suspend_acquired(client, token, count);
    }

    cJSON *token = cJSON_GetObjectItemCaseSensitive(request, "token");
    if (!cJSON_IsString(token) || !token->valuestring)
        return jw__reply_error(client, "missing suspend inhibitor token");
    bool released = false;
    jw_suspend_lease_result result = jw_suspend_inhibitor_release(
        &state->suspend_inhibitor, pid, token->valuestring, &released);
    if (result != JW_SUSPEND_LEASE_OK)
        return jw__reply_error(client, jw_suspend_lease_result_name(result));
    int count = jw_suspend_inhibitor_count(&state->suspend_inhibitor);
    jw_log_info("suspend-inhibit: %s pid=%d active=%d",
                released ? "released" : "duplicate release", (int)pid, count);
    return jw__reply_ok(client, "suspend-inhibit-release", NULL);
}

static int jw__reply_relocation_error(jw_ipc_client *client, int code,
                                      const char *message) {
    const char *name =
        code == JW_RELOCATION_NOT_FOUND ? "not-found" :
        code == JW_RELOCATION_CONFLICT ? "conflict" :
        code == JW_RELOCATION_STALE ? "stale-generation" :
        code == JW_RELOCATION_BUSY ? "busy" :
        code == JW_RELOCATION_BAD_STATE ? "bad-state" : "invalid";
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "error");
    cJSON_AddStringToObject(reply, "code", name);
    cJSON_AddStringToObject(reply, "message", message && message[0] ? message : name);
    return jw__reply_json(client, reply);
}

static int jw__reply_relocation_status(jw_daemon_state *state,
                                       jw_ipc_client *client,
                                       const jw_relocation_status *status) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "library-relocate-status");
    cJSON_AddStringToObject(reply, "operation_id", status->operation_id);
    cJSON_AddStringToObject(reply, "state", status->state);
    cJSON_AddNumberToObject(reply, "expected_generation",
                           status->expected_generation);
    cJSON_AddNumberToObject(reply, "mapping_generation",
                           status->mapping_generation);
    cJSON_AddNumberToObject(reply, "scan_ticket_generation",
                           status->scan_ticket_generation);
    cJSON_AddNumberToObject(reply, "item_count", status->item_count);
    cJSON_AddNumberToObject(reply, "library_generation",
                           state->library_generation);
    cJSON *ids = cJSON_AddArrayToObject(reply, "game_ids");
    jw_relocation_item items[JW_RELOCATION_MAX_ITEMS];
    int count = 0;
    if (jw_db_relocation_load_items(state->db, status->operation_id, items,
                                    JW_RELOCATION_MAX_ITEMS, &count) == 0) {
        for (int i = 0; i < count; i++)
            cJSON_AddItemToArray(ids, cJSON_CreateNumber(items[i].game_id));
    }
    return jw__reply_json(client, reply);
}

static bool jw__relocation_scan_active(jw_daemon_state *state) {
    bool active = false;
    pthread_mutex_lock(&state->scan_job.mu);
    active = state->scan_job.running || state->scan_job.completed;
    pthread_mutex_unlock(&state->scan_job.mu);
    return active;
}

static int jw__relocation_parse_identity(const cJSON *object,
                                         jw_relocation_identity *out) {
    if (!cJSON_IsObject(object) || !out) return -1;
    memset(out, 0, sizeof(*out));
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(object, "source_id");
    const cJSON *rom = cJSON_GetObjectItemCaseSensitive(object, "rom_relpath");
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(object, "image_root_kind");
    const cJSON *image = cJSON_GetObjectItemCaseSensitive(object, "image_relpath");
    if (!cJSON_IsString(source) || !source->valuestring || !source->valuestring[0] ||
        strlen(source->valuestring) >= sizeof(out->source_id) ||
        !cJSON_IsString(rom) || !rom->valuestring ||
        strlen(rom->valuestring) >= sizeof(out->rom_relpath) ||
        !jw_storage_relative_path_valid(rom->valuestring)) return -1;
    snprintf(out->source_id, sizeof(out->source_id), "%s", source->valuestring);
    snprintf(out->rom_relpath, sizeof(out->rom_relpath), "%s", rom->valuestring);
    if (kind || image) {
        if (!cJSON_IsString(kind) || !kind->valuestring ||
            !cJSON_IsString(image) || !image->valuestring ||
            strlen(kind->valuestring) >= sizeof(out->image_root_kind) ||
            strlen(image->valuestring) >= sizeof(out->image_relpath) ||
            (strcmp(kind->valuestring, "images") != 0 &&
             strcmp(kind->valuestring, "roms") != 0 &&
             strcmp(kind->valuestring, "source") != 0) ||
            !jw_storage_relative_path_valid(image->valuestring)) return -1;
        snprintf(out->image_root_kind, sizeof(out->image_root_kind), "%s",
                 kind->valuestring);
        snprintf(out->image_relpath, sizeof(out->image_relpath), "%s",
                 image->valuestring);
    }
    return 0;
}

static int jw__relocation_snapshot(const jw_storage_source *source,
                                   char *out, size_t out_size) {
    if (!source || !source->configured || !source->available) return -1;
    int n = snprintf(out, out_size,
        "root=%s;root_dev=%llu;root_mount=%d;"
        "root_fp=%s;roms=%s;roms_dev=%llu;roms_mount=%d;roms_fp=%s;"
        "images=%s;images_dev=%llu;images_mount=%d;images_fp=%s",
        source->root_abs[0] ? source->root_abs : source->root,
        source->device_id, source->mount_id,
        source->filesystem_fingerprint,
        source->roms_path, source->roms_device_id, source->roms_mount_id,
        source->roms_filesystem_fingerprint,
        source->images_path, source->images_device_id, source->images_mount_id,
        source->images_filesystem_fingerprint);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__relocation_materialize(const jw_storage_source *source,
                                      const jw_relocation_identity *identity,
                                      char *rom_path, size_t rom_path_size,
                                      char *image_path, size_t image_path_size) {
    char rom_abs[PATH_MAX];
    if (!source || !identity ||
        snprintf(rom_abs, sizeof(rom_abs), "%s/%s", source->roms_path,
                 identity->rom_relpath) >= (int)sizeof(rom_abs)) return -1;
    char rom_relative[PATH_MAX];
    if (snprintf(rom_relative, sizeof(rom_relative), "Roms/%s",
                 identity->rom_relpath) >= (int)sizeof(rom_relative) ||
        jw_storage_db_path_for_source(source, rom_relative, rom_abs,
                                      rom_path, rom_path_size) != 0) return -1;
    image_path[0] = '\0';
    if (!identity->image_root_kind[0]) return 0;
    const char *base = strcmp(identity->image_root_kind, "images") == 0
        ? source->images_path
        : strcmp(identity->image_root_kind, "roms") == 0
            ? source->roms_path : source->root;
    const char *prefix = strcmp(identity->image_root_kind, "images") == 0
        ? "Images" : strcmp(identity->image_root_kind, "roms") == 0
            ? "Roms" : "";
    char image_abs[PATH_MAX];
    char image_relative[PATH_MAX];
    if (snprintf(image_abs, sizeof(image_abs), "%s/%s", base,
                 identity->image_relpath) >= (int)sizeof(image_abs) ||
        snprintf(image_relative, sizeof(image_relative), "%s%s%s",
                 prefix, prefix[0] ? "/" : "", identity->image_relpath) >=
            (int)sizeof(image_relative) ||
        jw_storage_db_path_for_source(source, image_relative, image_abs,
                                      image_path, image_path_size) != 0) return -1;
    return 0;
}

static int jw__relocation_game_id(sqlite3 *db,
                                  const jw_relocation_identity *identity) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id FROM games WHERE source_id=? AND rom_relpath=?;",
            -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, identity->source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, identity->rom_relpath, -1, SQLITE_TRANSIENT);
    int id = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return id;
}

static int jw__handle_relocation_prepare(jw_daemon_state *state,
                                         jw_ipc_client *client,
                                         cJSON *request) {
    cJSON *operation = cJSON_GetObjectItemCaseSensitive(request, "operation_id");
    cJSON *generation = cJSON_GetObjectItemCaseSensitive(request, "expected_generation");
    cJSON *array = cJSON_GetObjectItemCaseSensitive(request, "items");
    int count = cJSON_IsArray(array) ? cJSON_GetArraySize(array) : 0;
    if (!cJSON_IsString(operation) || !operation->valuestring ||
        !cJSON_IsNumber(generation) || count <= 0 ||
        count > JW_RELOCATION_MAX_ITEMS)
        return jw__reply_relocation_error(client, JW_RELOCATION_ERROR,
                                           "invalid relocation prepare request");
    if (jw__relocation_scan_active(state))
        return jw__reply_relocation_error(client, JW_RELOCATION_BUSY,
                                           "library scan is active");

    jw_storage_source_list sources;
    if (jw__storage_sources(state, &sources) != 0)
        return jw__reply_relocation_error(client, JW_RELOCATION_ERROR,
                                           "storage sources unavailable");
    jw_relocation_item items[JW_RELOCATION_MAX_ITEMS];
    memset(items, 0, sizeof(items));
    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsObject(entry) ||
            jw__relocation_parse_identity(
                cJSON_GetObjectItemCaseSensitive(entry, "old"),
                &items[i].old_identity) != 0 ||
            jw__relocation_parse_identity(
                cJSON_GetObjectItemCaseSensitive(entry, "new"),
                &items[i].new_identity) != 0) {
            return jw__reply_relocation_error(client, JW_RELOCATION_ERROR,
                                               "invalid relocation item");
        }
        const jw_storage_source *old_source =
            jw_storage_sources_find_by_id(&sources, items[i].old_identity.source_id);
        const jw_storage_source *new_source =
            jw_storage_sources_find_by_id(&sources, items[i].new_identity.source_id);
        if (!old_source || !new_source || !old_source->available || !new_source->available ||
            jw__relocation_snapshot(old_source, items[i].old_source_snapshot,
                                    sizeof(items[i].old_source_snapshot)) != 0 ||
            jw__relocation_snapshot(new_source, items[i].new_source_snapshot,
                                    sizeof(items[i].new_source_snapshot)) != 0 ||
            jw__relocation_materialize(new_source, &items[i].new_identity,
                                       items[i].new_rom_path,
                                       sizeof(items[i].new_rom_path),
                                       items[i].new_image_path,
                                       sizeof(items[i].new_image_path)) != 0) {
            return jw__reply_relocation_error(client, JW_RELOCATION_ERROR,
                                               "unknown or unavailable source");
        }
        int game_id = jw__relocation_game_id(state->db, &items[i].old_identity);
        if (game_id > 0 &&
            ((state->retroarch_session.active &&
              state->retroarch_session.game_id == game_id) ||
             (state->pending_launch &&
              state->pending_launch_game_id == game_id))) {
            return jw__reply_relocation_error(client, JW_RELOCATION_BUSY,
                                               "affected game is active");
        }
    }
    char *fingerprint = cJSON_PrintUnformatted(request);
    if (!fingerprint)
        return jw__reply_relocation_error(client, JW_RELOCATION_ERROR,
                                           "could not encode relocation request");
    jw_relocation_status status;
    char error[256] = "";
    int rc = jw_db_relocation_prepare(state->db, operation->valuestring,
                                      generation->valueint, fingerprint,
                                      items, count, &status,
                                      error, sizeof(error));
    cJSON_free(fingerprint);
    if (rc != 0) return jw__reply_relocation_error(client, rc, error);
    return jw__reply_relocation_status(state, client, &status);
}

static int jw__relocation_snapshots_match(jw_daemon_state *state,
                                          const char *operation_id,
                                          char *error, size_t error_size) {
    jw_relocation_item items[JW_RELOCATION_MAX_ITEMS];
    int count = 0;
    if (jw_db_relocation_load_items(state->db, operation_id, items,
                                    JW_RELOCATION_MAX_ITEMS, &count) != 0)
        return -1;
    jw_storage_source_list sources;
    if (jw__storage_sources(state, &sources) != 0) return -1;
    static const char *fingerprint_keys[] = {"root_fp=", "roms_fp=", "images_fp="};
    for (int i = 0; i < count; i++) {
        const jw_storage_source *old_source =
            jw_storage_sources_find_by_id(&sources, items[i].old_identity.source_id);
        const jw_storage_source *new_source =
            jw_storage_sources_find_by_id(&sources, items[i].new_identity.source_id);
        char old_snapshot[1024], new_snapshot[1024];
        if (jw__relocation_snapshot(old_source, old_snapshot, sizeof(old_snapshot)) != 0 ||
            jw__relocation_snapshot(new_source, new_snapshot, sizeof(new_snapshot)) != 0) {
            snprintf(error, error_size, "%s", "storage source identity changed");
            return -1;
        }
        const char *recorded[2] = {
            items[i].old_source_snapshot, items[i].new_source_snapshot
        };
        const char *current[2] = {old_snapshot, new_snapshot};
        for (int side = 0; side < 2; side++) {
            if (strcmp(recorded[side], current[side]) == 0) continue;
            for (size_t k = 0;
                 k < sizeof(fingerprint_keys) / sizeof(fingerprint_keys[0]); k++) {
                const char *a = strstr(recorded[side], fingerprint_keys[k]);
                const char *b = strstr(current[side], fingerprint_keys[k]);
                if (!a || !b) {
                    snprintf(error, error_size, "%s",
                             "storage fingerprint unavailable after rebind");
                    return -1;
                }
                a += strlen(fingerprint_keys[k]);
                b += strlen(fingerprint_keys[k]);
                size_t alen = strcspn(a, ";");
                size_t blen = strcspn(b, ";");
                if (alen == 0 || alen != blen || strncmp(a, b, alen) != 0) {
                    snprintf(error, error_size, "%s",
                             "storage filesystem fingerprint changed");
                    return -1;
                }
            }
        }
        if (jw__relocation_materialize(old_source, &items[i].old_identity,
                                       items[i].old_rom_path,
                                       sizeof(items[i].old_rom_path),
                                       items[i].old_image_path,
                                       sizeof(items[i].old_image_path)) != 0 ||
            jw__relocation_materialize(new_source, &items[i].new_identity,
                                       items[i].new_rom_path,
                                       sizeof(items[i].new_rom_path),
                                       items[i].new_image_path,
                                       sizeof(items[i].new_image_path)) != 0) {
            snprintf(error, error_size, "%s", "could not rematerialize locator");
            return -1;
        }
        snprintf(items[i].old_source_snapshot,
                 sizeof(items[i].old_source_snapshot), "%s", old_snapshot);
        snprintf(items[i].new_source_snapshot,
                 sizeof(items[i].new_source_snapshot), "%s", new_snapshot);
    }
    if (jw_db_relocation_refresh_items(state->db, operation_id, items, count) != 0) {
        snprintf(error, error_size, "%s", "could not refresh relocation snapshot");
        return -1;
    }
    return 0;
}

static int jw__handle_relocation(jw_daemon_state *state, jw_ipc_client *client,
                                  cJSON *request, const char *type) {
    if (jw_delete_job_busy(state->delete_job))
        return jw__reply_relocation_error(client, JW_RELOCATION_BUSY, "ROM deletion is preparing or committing");
    if (strcmp(type, "library-relocate-prepare") == 0)
        return jw__handle_relocation_prepare(state, client, request);
    cJSON *operation = cJSON_GetObjectItemCaseSensitive(request, "operation_id");
    if (!cJSON_IsString(operation) || !operation->valuestring ||
        !operation->valuestring[0])
        return jw__reply_relocation_error(client, JW_RELOCATION_ERROR,
                                           "missing operation id");
    jw_relocation_status status;
    char error[256] = "";
    int rc = 0;
    if (strcmp(type, "library-relocate-status") == 0) {
        rc = jw_db_relocation_status(state->db, operation->valuestring, &status);
    } else if (strcmp(type, "library-relocate-commit") == 0) {
        rc = jw_db_relocation_status(state->db, operation->valuestring, &status);
        if (rc == 0 && strcmp(status.state, "committed") == 0)
            return jw__reply_relocation_status(state, client, &status);
        if (rc != 0) return jw__reply_relocation_error(client, rc, error);
        if (jw__relocation_snapshots_match(state, operation->valuestring,
                                           error, sizeof(error)) != 0)
            return jw__reply_relocation_error(client, JW_RELOCATION_CONFLICT, error);
        rc = jw_db_relocation_commit(state->db, operation->valuestring,
                                     &status, error, sizeof(error));
        if (rc == 0) state->library_generation = status.mapping_generation;
    } else if (strcmp(type, "library-relocate-revert") == 0) {
        rc = jw_db_relocation_status(state->db, operation->valuestring, &status);
        if (rc == 0 && strcmp(status.state, "reverted") == 0)
            return jw__reply_relocation_status(state, client, &status);
        if (rc != 0) return jw__reply_relocation_error(client, rc, error);
        if (jw__relocation_snapshots_match(state, operation->valuestring,
                                           error, sizeof(error)) != 0)
            return jw__reply_relocation_error(client, JW_RELOCATION_CONFLICT, error);
        rc = jw_db_relocation_revert(state->db, operation->valuestring,
                                     &status, error, sizeof(error));
        if (rc == 0) state->library_generation = status.mapping_generation;
    } else if (strcmp(type, "library-relocate-abort") == 0) {
        rc = jw_db_relocation_abort(state->db, operation->valuestring, &status);
    } else if (strcmp(type, "library-relocate-finish") == 0) {
        rc = jw_db_relocation_finish(state->db, operation->valuestring, &status);
        if (rc == 0 && strcmp(status.state, "finished") != 0 &&
            jw__start_scan_job(state, "relocation reconciliation") < 0)
            return jw__reply_relocation_error(client, JW_RELOCATION_BUSY,
                                               "reconciliation scan could not start");
    } else {
        return jw__reply_relocation_error(client, JW_RELOCATION_ERROR,
                                           "unknown relocation operation");
    }
    if (rc != 0) return jw__reply_relocation_error(client, rc, error);
    return jw__reply_relocation_status(state, client, &status);
}

/* ------------------------------------------------------------------ */
/* CTL-1: service control and status IPC (app-services-v1)             */
/* ------------------------------------------------------------------ */

/* Serializes one supervised service into the closed CTL-1 status-response
 * shape frozen by control-ipc-v1.schema.json. Request correlation fields are
 * added by the caller. */
static bool jw__service_status_json(const jw_svc_supervised *e, cJSON *obj) {
    if (!e || !obj) return false;
    jw_svc_lifecycle_game game_policy =
        e->pgid > 0 ? e->active_lifecycle_game : e->manifest.lifecycle_game;
    if (!cJSON_AddStringToObject(obj, "service_id", e->service_id) ||
        !cJSON_AddBoolToObject(obj, "desired_enabled", e->desired_enabled) ||
        !cJSON_AddStringToObject(obj, "effective_state",
                                 jw_svc_effective_state_name(e->state)) ||
        !cJSON_AddStringToObject(obj, "coordination",
             game_policy == JW_SVC_LIFECYCLE_GAME_NOTIFY && e->pgid > 0
                 ? "unsubscribed" : "n/a")) {
        return false;
    }

    cJSON *ownership = cJSON_AddObjectToObject(obj, "ownership_identity");
    if (!ownership) return false;
    if (e->pgid > 0) {
        char instant[32];
        snprintf(instant, sizeof(instant), "%lld", e->launch_instant_us);
        if (!cJSON_AddNumberToObject(ownership, "pgid", (double)e->pgid) ||
            !cJSON_AddStringToObject(ownership, "launch_instant", instant)) {
            return false;
        }
    } else if (!cJSON_AddNullToObject(ownership, "pgid") ||
               !cJSON_AddNullToObject(ownership, "launch_instant")) {
        return false;
    }

    const char *lease = e->lease_fd >= 0 ? "held" :
        e->state == JW_SVC_STATE_STALE_GENERATION ? "stale" : "none";
    if (!cJSON_AddStringToObject(obj, "generation_lease_state", lease)) {
        return false;
    }

    cJSON *transition = cJSON_AddObjectToObject(obj, "last_transition");
    char transition_at[32];
    snprintf(transition_at, sizeof(transition_at), "%lld",
             e->control.last_transition_at_us);
    const char *transition_reason =
        e->state == JW_SVC_STATE_UNAVAILABLE && e->reject_reason[0]
            ? e->reject_reason : e->control.last_transition_reason;
    if (!transition ||
        !cJSON_AddStringToObject(transition, "at", transition_at) ||
        !cJSON_AddStringToObject(transition, "reason", transition_reason)) {
        return false;
    }

    cJSON *last_exit = cJSON_AddObjectToObject(obj, "last_exit");
    if (!last_exit) return false;
    if (e->control.has_last_exit) {
        char exit_at[32];
        snprintf(exit_at, sizeof(exit_at), "%lld",
                 e->control.last_exit_at_us);
        if (!cJSON_AddNumberToObject(last_exit, "status",
                                     (double)e->control.last_exit_code) ||
            !cJSON_AddStringToObject(last_exit, "at", exit_at)) {
            return false;
        }
    } else if (!cJSON_AddNullToObject(last_exit, "status") ||
               !cJSON_AddNullToObject(last_exit, "at")) {
        return false;
    }

    if (!cJSON_AddNumberToObject(obj, "restart_count",
                                 (double)e->control.restart_count)) {
        return false;
    }
    if (e->state == JW_SVC_STATE_BACKOFF) {
        if (!cJSON_AddStringToObject(obj, "backoff_or_breaker_reason",
                                     "on-failure")) return false;
    } else if (e->state == JW_SVC_STATE_FAILED) {
        if (!cJSON_AddStringToObject(obj, "backoff_or_breaker_reason",
                                     "circuit-breaker")) return false;
    } else if (!cJSON_AddNullToObject(obj, "backoff_or_breaker_reason")) {
        return false;
    }

    cJSON *installed = cJSON_AddObjectToObject(obj, "installed_package");
    const char *package_id = e->control.installed_package_id[0]
                                 ? e->control.installed_package_id
                                 : e->service_id;
    const char *package_version = e->installed_package_version[0]
                                      ? e->installed_package_version
                                      : e->control.installed_package_version;
    if (!installed ||
        !cJSON_AddStringToObject(installed, "id", package_id) ||
        !cJSON_AddStringToObject(installed, "version", package_version)) {
        return false;
    }

    /* contracts.md: report "whether a lifecycle policy stop is in force
     * (reason: game, storage, suspend, package)". All four implemented
     * triggers are reported distinctly. */
    const char *lifecycle_reason =
        jw_svc_stop_reason_lifecycle_slug(e->pending_stop_reason);
    cJSON *lifecycle =
        cJSON_AddObjectToObject(obj, "lifecycle_policy_stop");
    if (!lifecycle ||
        !cJSON_AddBoolToObject(lifecycle, "active",
                               lifecycle_reason != NULL)) {
        return false;
    }
    return lifecycle_reason
        ? cJSON_AddStringToObject(lifecycle, "reason", lifecycle_reason) != NULL
        : cJSON_AddNullToObject(lifecycle, "reason") != NULL;
}

/* Reads up to `tail` lines from the service's rotating log, redacting each
 * line per CTL-1's secret-redaction rule, and appends them as a JSON array
 * of strings to `arr`. Bounded: reads at most the newest current log file
 * and caps output so an IPC reply never carries an unbounded payload. */
static void jw__service_logs_json(jw_daemon_state *state,
                                  const jw_svc_supervised *entry,
                                  int tail, cJSON *arr) {
    const char *logs = getenv("LOGS_PATH");
    if (!logs || !logs[0] || !entry || !arr ||
        !jw_svc_supervisor_service_id_is_safe(entry->service_id)) {
        return;
    }
    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/services/%s/%s.log",
                           logs, entry->service_id, entry->service_id);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return;
    }
    /* Read the whole (bounded-by-rotation, <= 256 KiB) current file, then
     * keep only the last `tail` lines. */
    char *buf = malloc((size_t)JW_SVC_LOG_MAX_BYTES + 1u);
    if (!buf) {
        fclose(fp);
        return;
    }
    size_t n = fread(buf, 1, (size_t)JW_SVC_LOG_MAX_BYTES, fp);
    fclose(fp);
    buf[n] = '\0';

    tail = jw_svc_supervisor_bound_log_tail(tail);
    /* Split into lines, redact each, collect into a ring of the last N. */
    char **lines = calloc((size_t)tail, sizeof(char *));
    if (!lines) {
        free(buf);
        return;
    }
    int used = 0, start = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char redacted[2048];
        /* The boolean reports whether a secret was found; false still leaves
         * an unchanged, bounded copy in `redacted`. */
        (void)jw_svc_log_redact_line(line, redacted, sizeof(redacted));
        int slot = used < tail ? used : start;
        free(lines[slot]);
        lines[slot] = strdup(redacted);
        if (used < tail) {
            used++;
        } else {
            start = (start + 1) % tail;
        }
    }
    for (int i = 0; i < used; i++) {
        int idx = (start + i) % tail;
        if (lines[idx]) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(lines[idx]));
        }
        free(lines[idx]);
    }
    free(lines);
    free(buf);
    (void)state;
}

static int jw__reply_ctl1_error(jw_ipc_client *client, const char *id,
                                const char *code, const char *message) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "v", JW_CTL1_VERSION);
    cJSON_AddStringToObject(reply, "id", id ? id : "");
    cJSON *error = cJSON_AddObjectToObject(reply, "error");
    cJSON_AddStringToObject(error, "code", code ? code : "internal-error");
    cJSON_AddStringToObject(error, "message", message ? message : code);
    if (code && strcmp(code, "unsupported-version") == 0) {
        cJSON *versions = cJSON_AddArrayToObject(error, "supported_versions");
        cJSON_AddItemToArray(versions, cJSON_CreateNumber(JW_CTL1_VERSION));
    }
    return jw__reply_json(client, reply);
}

static int jw__reply_ctl1_json(jw_ipc_client *client, const char *id,
                               cJSON *reply) {
    char *json = cJSON_PrintUnformatted(reply);
    if (!json) {
        cJSON_Delete(reply);
        return -1;
    }
    size_t len = strlen(json);
    if (len > JW_CTL1_MAX_PAYLOAD) {
        cJSON_free(json);
        cJSON_Delete(reply);
        return jw__reply_ctl1_error(client, id, "response-too-large",
                                    "CTL-1 response exceeds 64 KiB");
    }
    int rc = jw_ipc_client_send(client, json, len);
    cJSON_free(json);
    cJSON_Delete(reply);
    return rc;
}

static int jw__handle_service_ctl(jw_daemon_state *state,
                                  jw_ipc_client *client,
                                  const jw_ctl1_request *request) {
    if (!state->services) {
        return jw__reply_ctl1_error(client, request->id,
                                    "services-unavailable",
                                    "service supervisor is unavailable");
    }

    if (request->operation == JW_CTL1_OP_CAPABILITIES) {
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddNumberToObject(reply, "v", JW_CTL1_VERSION);
        cJSON_AddStringToObject(reply, "id", request->id);
        cJSON *caps = cJSON_AddArrayToObject(reply, "capabilities");
        cJSON_AddItemToArray(caps, cJSON_CreateString("app-services-v1"));
        cJSON_AddItemToArray(caps, cJSON_CreateString("control-ipc-v1"));
        cJSON_AddItemToArray(caps, cJSON_CreateString("game-coordination-v1"));
#if defined(__linux__)
        cJSON_AddItemToArray(caps, cJSON_CreateString("pdeathsig"));
#endif
        return jw__reply_ctl1_json(client, request->id, reply);
    }

    if (request->operation == JW_CTL1_OP_LIST) {
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddNumberToObject(reply, "v", JW_CTL1_VERSION);
        cJSON_AddStringToObject(reply, "id", request->id);
        cJSON *arr = cJSON_AddArrayToObject(reply, "services");
        int n = jw_svc_supervisor_count(state->services);
        for (int i = 0; i < n; i++) {
            const jw_svc_supervised *e =
                jw_svc_supervisor_at(state->services, i);
            /* One shared predicate with the supervisor's own shedding rule:
             * canonical invalid discoveries remain visible as unavailable,
             * spent records are omitted, and a locked old-generation lease
             * stays discoverable even before a durable control-state write. */
            if (!jw_svc_supervisor_entry_is_listable(e)) {
                continue;
            }
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "service_id", e->service_id);
            cJSON_AddBoolToObject(obj, "desired_enabled", e->desired_enabled);
            cJSON_AddStringToObject(obj, "effective_state",
                                    jw_svc_effective_state_name(e->state));
            cJSON_AddItemToArray(arr, obj);
        }
        return jw__reply_ctl1_json(client, request->id, reply);
    }

    const char *service_id = request->service_id;
    char reason[JW_SVC_REASON_BUF] = {0};

    if (request->operation == JW_CTL1_OP_STATUS) {
        const jw_svc_supervised *e =
            jw_svc_supervisor_find(state->services, service_id);
        if (!e) {
            return jw__reply_ctl1_error(client, request->id,
                                        "unknown-service",
                                        "unknown service id");
        }
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddNumberToObject(reply, "v", JW_CTL1_VERSION);
        cJSON_AddStringToObject(reply, "id", request->id);
        if (!jw__service_status_json(e, reply)) {
            cJSON_Delete(reply);
            return jw__reply_ctl1_error(client, request->id,
                                        "internal-error",
                                        "could not serialize service status");
        }
        int subscriber = jw__usable_subscription(state, service_id);
        jw_svc_lifecycle_game policy = e->pgid > 0
            ? e->active_lifecycle_game : e->manifest.lifecycle_game;
        const char *coordination = subscriber >= 0 ? "subscribed"
            : e->pgid > 0 && policy != JW_SVC_LIFECYCLE_GAME_IGNORE
                ? "unsubscribed" : "n/a";
        cJSON_ReplaceItemInObjectCaseSensitive(
            reply, "coordination", cJSON_CreateString(coordination));
        return jw__reply_ctl1_json(client, request->id, reply);
    }

    if (request->operation == JW_CTL1_OP_LOGS ||
        request->operation == JW_CTL1_OP_EXPORT_LOGS) {
        const jw_svc_supervised *e =
            jw_svc_supervisor_find(state->services, service_id);
        if (!e) {
            return jw__reply_ctl1_error(client, request->id,
                                        "unknown-service",
                                        "unknown service id");
        }
        int tail = request->operation == JW_CTL1_OP_EXPORT_LOGS
                       ? JW_SVC_LOG_TAIL_MAX
                       : request->has_tail ? request->tail
                                           : JW_SVC_LOG_TAIL_DEFAULT;
        tail = jw_svc_supervisor_bound_log_tail(tail);
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddNumberToObject(reply, "v", JW_CTL1_VERSION);
        cJSON_AddStringToObject(reply, "id", request->id);
        cJSON *lines = cJSON_AddArrayToObject(reply, "lines");
        jw__service_logs_json(state, e, tail, lines);
        return jw__reply_ctl1_json(client, request->id, reply);
    }

    bool ok = false;
    if (request->operation == JW_CTL1_OP_ENABLE) {
        ok = jw_svc_supervisor_enable(state->services, service_id,
                                      reason, sizeof(reason));
    } else if (request->operation == JW_CTL1_OP_DISABLE) {
        ok = jw_svc_supervisor_disable(state->services, service_id,
                                       reason, sizeof(reason));
    } else if (request->operation == JW_CTL1_OP_RUN) {
        ok = jw_svc_supervisor_run(state->services, service_id,
                                   reason, sizeof(reason));
    } else if (request->operation == JW_CTL1_OP_STOP) {
        ok = jw_svc_supervisor_stop(state->services, service_id,
                                    reason, sizeof(reason));
    } else if (request->operation == JW_CTL1_OP_RESTART) {
        ok = jw_svc_supervisor_restart(state->services, service_id,
                                       reason, sizeof(reason));
    }

    if (!ok) {
        char message[128];
        snprintf(message, sizeof(message), "%s failed: %s",
                 jw_ctl1_operation_name(request->operation), reason);
        return jw__reply_ctl1_error(client, request->id,
                                    reason[0] ? reason : "operation-failed",
                                    message);
    }
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "v", JW_CTL1_VERSION);
    cJSON_AddStringToObject(reply, "id", request->id);
    cJSON_AddBoolToObject(reply, "ok", true);
    return jw__reply_ctl1_json(client, request->id, reply);
}

static int jw__handle_message(jw_daemon_state *state, jw_ipc_client *client,
                              const char *body, size_t body_len) {
    cJSON *root = cJSON_ParseWithLength(body, body_len);
    if (!root) {
        jw_log_error("invalid json message");
        return jw__reply_error(client, "invalid json");
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    cJSON *ctl_op = cJSON_GetObjectItemCaseSensitive(root, "op");
    cJSON *ctl_version = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (ctl_op || (ctl_version && !type)) {
        jw_ctl1_request request;
        char code[32];
        bool valid = jw_ctl1_parse_request(body, body_len, &request,
                                           code, sizeof(code));
        cJSON_Delete(root);
        if (!valid) {
            return jw__reply_ctl1_error(client, request.id, code,
                strcmp(code, "unsupported-version") == 0
                    ? "server supports CTL-1 v1 only"
                    : "invalid CTL-1 request");
        }
        return jw__handle_service_ctl(state, client, &request);
    }
    if (!cJSON_IsString(type) || !type->valuestring) {
        cJSON_Delete(root);
        return jw__reply_error(client, "missing type");
    }

    if (strcmp(type->valuestring, "suspend-inhibit-acquire") == 0 ||
        strcmp(type->valuestring, "suspend-inhibit-release") == 0 ||
        strcmp(type->valuestring, "suspend-inhibit-status") == 0) {
        int rc = jw__handle_suspend_inhibit(state, client, root, type->valuestring);
        cJSON_Delete(root);
        return rc;
    }

    if (strncmp(type->valuestring, "library-relocate-", 17) == 0) {
        int rc = jw__handle_relocation(state, client, root, type->valuestring);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "hello") == 0) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        if (cJSON_IsString(role) && role->valuestring) {
            if (strcmp(role->valuestring, "launcher") == 0) {
                state->launcher_levels_signal =
                    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "levels_signal"));
                jw_log_info("launcher hello");
            } else if (strcmp(role->valuestring, "menu") == 0) {
                jw_log_info("menu hello");
            } else {
                jw_log_info("client hello role=%s", role->valuestring);
            }
        }
        cJSON_Delete(root);
        return jw__reply_hello_ok(client);
    }

    if (strcmp(type->valuestring, "hdmi-revert-status") == 0) {
        int secs = jw__hdmi_revert_seconds(state);
        cJSON_Delete(root);
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "ok");
        cJSON_AddNumberToObject(reply, "seconds", secs);
        return jw__reply_json(client, reply);
    }
    if (strcmp(type->valuestring, "hdmi-revert-keep") == 0) {
        state->hdmi_revert_deadline_ms = 0;
        jw_log_info("HDMI 1080p120 kept by user");
        cJSON_Delete(root);
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddStringToObject(reply, "type", "ok");
        return jw__reply_json(client, reply);
    }

    if (strcmp(type->valuestring, "set-input-shortcuts") == 0) {
        /* Settings pushes the complete snapshot after persisting it, so a
           rebind takes effect without restarting the daemon.

           Deliberately does not read the database. This handler runs on the
           main loop; reopening SQLite here would put a settings query on the
           same thread that drains evdev, which is exactly what the cached
           snapshot exists to avoid. Everything needed is in the message.

           The snapshot is rebuilt from scratch and validated before it is
           installed, so a malformed or colliding message leaves the running
           bindings alone rather than half-applying. */
        cJSON *buttons = cJSON_GetObjectItemCaseSensitive(root, "buttons");
        jw_input_shortcuts next;
        bool ok = cJSON_IsArray(buttons) &&
                  cJSON_GetArraySize(buttons) == JW_INPUT_SHORTCUT_ACTION_COUNT;
        for (int i = 0; ok && i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
            cJSON *item = cJSON_GetArrayItem(buttons, i);
            ok = cJSON_IsString(item) && item->valuestring &&
                 jw_input_shortcut_button_parse(item->valuestring,
                                                &next.buttons[i]);
        }
        cJSON *shots = cJSON_GetObjectItemCaseSensitive(root, "screenshots_enabled");
        cJSON *rec = cJSON_GetObjectItemCaseSensitive(root, "recording_enabled");
        ok = ok && cJSON_IsBool(shots) && cJSON_IsBool(rec);
        if (!ok || !jw_input_shortcuts_valid(&next)) {
            cJSON_Delete(root);
            jw_log_warn("input shortcuts: rejected an invalid IPC snapshot; "
                        "keeping the running state");
            return jw__reply_error(client, "invalid input shortcuts");
        }
        state->shortcuts = next;   /* one assignment: never partly applied */
        state->screenshots_enabled_cached = cJSON_IsTrue(shots);
        state->recording_enabled_cached = cJSON_IsTrue(rec);
        cJSON_Delete(root);
        return jw__reply_ok(client, "set-input-shortcuts", NULL);
    }

    if (strcmp(type->valuestring, "rumble") == 0) {
        cJSON *ev = cJSON_GetObjectItemCaseSensitive(root, "event");
        const char *event = cJSON_IsString(ev) ? ev->valuestring : NULL;
        if (event && strcmp(event, "preview") == 0) {
            /* Live slider preview: one tick at the exact passed strength (not the
               TTL cache), so the user feels the level as they drag. */
            cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "strength");
            int strength = cJSON_IsNumber(s) ? (int)s->valuedouble : 65;
            jw__rumble_queue(1, jw__rumble_duty_for(strength), JW_RUMBLE_TICK_MS);
        } else if (event) {
            jw__rumble_event(state, event);
        }
        cJSON_Delete(root);
        return jw__reply_ok(client, "rumble", NULL);
    }

    if (strcmp(type->valuestring, "refresh-osd-appearance") == 0) {
        /* Settings persisted a font family or size. Internal: no arguments,
           and the reply carries nothing the caller waits for. */
        bool valid = cJSON_GetArraySize(root) == 1;
        cJSON_Delete(root);
        if (!valid) {
            return jw__reply_error(client, "invalid refresh-osd-appearance request");
        }
        jw__osd_refresh_appearance(state, "settings");
        return jw__reply_ok(client, "refresh-osd-appearance", NULL);
    }

    if (strcmp(type->valuestring, "set-language") == 0) {
        cJSON *lang_json = cJSON_GetObjectItemCaseSensitive(root, "language");
        if (!cJSON_IsString(lang_json) || !lang_json->valuestring ||
            !lang_json->valuestring[0]) {
            cJSON_Delete(root);
            return jw__reply_error(client, "missing language");
        }

        /* The code becomes a filename component (i18n/<lang>.jwi), so restrict it
           to what a language tag can legitimately contain. Without this, a "code"
           of ../../something would read an arbitrary file as a string table. */
        const char *lang = lang_json->valuestring;
        size_t lang_len = strlen(lang);
        if (lang_len >= JW_I18N_CODE_MAX) {   /* same limit the scanner and launcher use */
            cJSON_Delete(root);
            return jw__reply_error(client, "language code too long");
        }
        for (size_t i = 0; i < lang_len; i++) {
            char c = lang[i];
            bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!ok) {
                cJSON_Delete(root);
                return jw__reply_error(client, "invalid language code");
            }
        }

        char lang_buf[JW_I18N_CODE_MAX];
        snprintf(lang_buf, sizeof(lang_buf), "%s", lang);

        if (jw_db_set_setting(state->db_path, "language", lang_buf) != 0) {
            jw_log_warn("set-language: could not persist %s", lang_buf);
            cJSON_Delete(root);
            return jw__reply_error(client, "could not save language");
        }
        /* A launcher that can swap its own string table and font asks to stay
           running, so the user keeps their place in Settings. The daemon still
           owns persistence and the OSD refresh either way; only the SIGTERM is
           conditional. Every other caller keeps the restart. */
        const cJSON *keep = cJSON_GetObjectItemCaseSensitive(root, "keep_running");
        bool keep_running = cJSON_IsTrue(keep);
        jw_log_info("set-language %s; %s", lang_buf,
                    keep_running ? "launcher reloads in place" : "restarting launcher");
        jw__osd_refresh_appearance(state, "set-language");

        /* Everything the new language needs is resolved in the parent at spawn
           time -- jw__spawn_child re-runs jw_appearance_resolve on every spawn,
           so the replacement launcher comes up with the right font path and
           loads the matching string table itself. Nothing to push down here.

           SIGTERM rather than SIGKILL so the launcher can save its breadcrumb;
           the ordinary child-exit path respawns it. */
        if (!keep_running && state->child_pid > 0 &&
            state->child_kind == JW_CHILD_LAUNCHER) {
            kill(state->child_pid, SIGTERM);
        }

        cJSON_Delete(root);
        return jw__reply_ok(client, "set-language", NULL);
    }

    if (strcmp(type->valuestring, "scan-library") == 0) {
        int rc = jw__handle_scan(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-validate") == 0) {
        int rc = jw__handle_scrape_validate(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-start") == 0) {
        int rc = jw__handle_scrape_start(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-missing-counts") == 0) {
        int rc = jw__handle_scrape_missing_counts(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-status") == 0) {
        int rc = jw__reply_scrape_status(client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-queue") == 0) {
        int rc = jw__reply_scrape_queue(client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-cancel") == 0) {
        int rc = jw__handle_scrape_cancel(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "scrape-stop-all") == 0) {
        cJSON_Delete(root);
        return jw__handle_scrape_stop_all(state, client);
    }

    if (strcmp(type->valuestring, "scrape-clear-done") == 0) {
        cJSON_Delete(root);
        return jw__handle_scrape_clear_done(state, client);
    }

    if (strcmp(type->valuestring, "library-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_library_status(state, client);
    }

    if (strcmp(type->valuestring, "storage-status") == 0) {
        cJSON *source_json = cJSON_GetObjectItemCaseSensitive(root, "source");
        char source[32] = "";
        if (cJSON_IsString(source_json) && source_json->valuestring) {
            snprintf(source, sizeof(source), "%s", source_json->valuestring);
        }
        cJSON_Delete(root);
        return jw__reply_storage_status(state, client, source[0] ? source : NULL);
    }

    if (strcmp(type->valuestring, "storage-action") == 0) {
        int rc = jw__handle_storage_action(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "storage-warning-ack") == 0) {
        int rc = jw__handle_storage_warning_ack(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "storage-repair-request") == 0) {
        int rc = jw__handle_storage_repair_request(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "storage-repair-result-ack") == 0) {
        int rc = jw__handle_storage_repair_result_ack(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "open-menu") == 0) {
        bool in_game = jw__has_retroarch_session(state);
        if ((in_game ? jw__request_open_in_game_menu(state)
                     : jw__request_open_menu(state)) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, "open-menu failed");
        }
        jw_log_info("open-menu requested mode=%s", in_game ? "in-game" : "frontend");
        cJSON_Delete(root);
        return jw__reply_ok(client, "open-menu", NULL);
    }

    if (strcmp(type->valuestring, "open-switcher") == 0) {
        bool in_game = jw__has_retroarch_session(state);
        if (!in_game || jw__request_open_in_game_switcher(state) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client,
                in_game ? "open-switcher failed" : "no active RetroArch session");
        }
        jw_log_info("open-switcher requested");
        cJSON_Delete(root);
        return jw__reply_ok(client, "open-switcher", NULL);
    }

    if (strcmp(type->valuestring, "switch-game") == 0) {
        cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
        cJSON *rom_path = cJSON_GetObjectItemCaseSensitive(root, "rom_path");
        const char *error_message = NULL;
        if (!cJSON_IsString(system) || !system->valuestring ||
            !cJSON_IsString(rom_path) || !rom_path->valuestring ||
            jw__request_switch_game(state, system->valuestring,
                                    rom_path->valuestring, &error_message) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client,
                error_message ? error_message : "switch-game failed");
        }

        jw_log_info("switch-game requested system=%s rom=%s",
                    system->valuestring, rom_path->valuestring);
        cJSON_Delete(root);
        return jw__reply_ok(client, "switch-game", NULL);
    }

    if (strcmp(type->valuestring, "retroarch-session") == 0) {
        cJSON_Delete(root);
        return jw__reply_retroarch_session(state, client);
    }

    if (strcmp(type->valuestring, "retroarch-shader") == 0) {
        return jw__handle_retroarch_shader(state, client, root);
    }
    if (strcmp(type->valuestring, "retroarch-action") == 0) {
        int rc = jw__handle_retroarch_action(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "launch-game") == 0) {
        cJSON *system = cJSON_GetObjectItemCaseSensitive(root, "system");
        cJSON *rom_path = cJSON_GetObjectItemCaseSensitive(root, "rom_path");
        cJSON *core_id = cJSON_GetObjectItemCaseSensitive(root, "core_id");
        cJSON *resume_policy = cJSON_GetObjectItemCaseSensitive(root, "resume_policy");
        const char *requested_core_id = NULL;
        if (core_id) {
            if (!cJSON_IsString(core_id) || !core_id->valuestring ||
                !core_id->valuestring[0] ||
                strlen(core_id->valuestring) >=
                    sizeof(state->pending_launch_core_id)) {
                cJSON_Delete(root);
                return jw__reply_error(client, "invalid core id");
            }
            requested_core_id = core_id->valuestring;
        }
        bool switcher_resume = false;
        if (cJSON_IsString(resume_policy) && resume_policy->valuestring &&
            resume_policy->valuestring[0]) {
            if (strcmp(resume_policy->valuestring, "switcher-latest") != 0) {
                cJSON_Delete(root);
                return jw__reply_error(client, "unknown resume policy");
            }
            switcher_resume = true;
        }
        const char *error_message = NULL;
        if (!cJSON_IsString(system) || !system->valuestring ||
            !cJSON_IsString(rom_path) || !rom_path->valuestring ||
            jw__request_launch_game(state, system->valuestring, rom_path->valuestring,
                                    requested_core_id, switcher_resume,
                                    &error_message) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, error_message ? error_message : "launch-game failed");
        }

        jw_log_info("launch-game requested system=%s rom=%s core=%s resume=%s",
                    system->valuestring, rom_path->valuestring,
                    requested_core_id ? requested_core_id : "(default)",
                    switcher_resume ? "switcher-latest" : "none");
        cJSON_Delete(root);
        return jw__reply_ok(client, "launch-game", NULL);
    }

    /* Test/tool mirror of the device's Menu-button escape hatch. This is kept
       on the legacy one-shot IPC surface; LIFE-1's service wire remains exactly
       the frozen game.start/game.cancel/status protocol. */
    if (strcmp(type->valuestring, "game-start-now") == 0) {
        bool accepted = jw__game_coordination_start_now(
            state, "start-now-request");
        cJSON_Delete(root);
        return accepted
            ? jw__reply_ok(client, "game-start-now", NULL)
            : jw__reply_error(client, "no waiting game launch");
    }

    if (strcmp(type->valuestring, "game-launch-blocked-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_game_launch_blocked(state, client);
    }

    if (strcmp(type->valuestring, "game-check-wait") == 0) {
        bool accepted = jw__game_check_wait(state);
        cJSON_Delete(root);
        return accepted
            ? jw__reply_ok(client, "game-check-wait", NULL)
            : jw__reply_error(client, "no pending sync check");
    }

    if (strcmp(type->valuestring, "game-check-play-anyway") == 0) {
        bool accepted = jw__game_check_play_anyway(state);
        cJSON_Delete(root);
        return accepted
            ? jw__reply_ok(client, "game-check-play-anyway", NULL)
            : jw__reply_error(client, "sync check could not safely proceed");
    }

    if (strcmp(type->valuestring, "game-check-cancel") == 0) {
        bool accepted = jw__game_check_cancel(state);
        cJSON_Delete(root);
        return accepted
            ? jw__reply_ok(client, "game-check-cancel", NULL)
            : jw__reply_error(client, "no pending sync check");
    }

    if (strcmp(type->valuestring, "game-launch-blocked-dismiss") == 0) {
        bool dismissed = jw__game_launch_blocked_dismiss(state);
        cJSON_Delete(root);
        return dismissed
            ? jw__reply_ok(client, "game-launch-blocked-dismiss", NULL)
            : jw__reply_error(client, "no blocked game launch");
    }

    if (strcmp(type->valuestring, "game-launch-override") == 0) {
        if (!state->game_launch_blocked || state->active_game.active ||
            state->pending_launch_game_id <= 0) {
            cJSON_Delete(root);
            return jw__reply_error(client,
                                   "blocked game launch is not overrideable");
        }
        state->pending_launch = true;
        state->pending_launch_resume_switcher =
            state->game_launch_blocked_resume_switcher;
        state->pending_launch_skip_check =
            state->game_launch_blocked_requires_verified_stop;
        state->pending_launch_override_unverified =
            !state->game_launch_blocked_requires_verified_stop;
        state->game_launch_blocked = false;
        state->game_launch_blocked_resume_switcher = false;
        state->game_launch_blocked_requires_verified_stop = false;
        state->game_launch_blocked_service_id[0] = '\0';
        state->game_launch_blocked_reason[0] = '\0';
        jw_log_warn("life1: user chose Play Anyway for blocked launch; "
                    "skip_check=%s allow_unverified_stop=%s",
                    state->pending_launch_skip_check ? "true" : "false",
                    state->pending_launch_override_unverified
                        ? "true" : "false");
        bool launch_now = state->child_pid <= 0;
        cJSON_Delete(root);
        if (launch_now && jw__spawn_pending_game(state) != 0) {
            return jw__reply_error(client,
                                   "overridden game launch still failed");
        }
        return jw__reply_ok(client, "game-launch-override", NULL);
    }

    /* Retry answers the RAOfflineProxy prompt without bypassing anything: the
       same pending launch runs again with no override and no skipped check,
       so it takes the full coordination path and the same bounded (500 ms,
       fail-closed) routing decision as a fresh launch. Healthy now means a
       normal proxied launch; still not ready re-blocks with the same reason
       and the replacement launcher shows the prompt again. One attempt per
       request, never a loop. */
    if (strcmp(type->valuestring, "game-launch-retry") == 0) {
        if (!jw__game_launch_retry_allowed(state) ||
            state->active_game.active || state->pending_launch_game_id <= 0) {
            cJSON_Delete(root);
            return jw__reply_error(client,
                                   "blocked game launch is not retryable");
        }
        state->pending_launch = true;
        state->pending_launch_resume_switcher =
            state->game_launch_blocked_resume_switcher;
        state->pending_launch_skip_check = false;
        state->pending_launch_override_unverified = false;
        state->game_launch_blocked = false;
        state->game_launch_blocked_resume_switcher = false;
        state->game_launch_blocked_requires_verified_stop = false;
        state->game_launch_blocked_service_id[0] = '\0';
        state->game_launch_blocked_reason[0] = '\0';
        jw_log_info("life1: user chose Retry for blocked launch");
        bool launch_now = state->child_pid <= 0;
        cJSON_Delete(root);
        if (launch_now && jw__spawn_pending_game(state) != 0) {
            return jw__reply_error(client,
                                   state->game_launch_blocked
                                       ? "retried game launch still blocked"
                                       : "retried game launch failed");
        }
        return jw__reply_ok(client, "game-launch-retry", NULL);
    }

    if (strcmp(type->valuestring, "launch-app") == 0) {
        cJSON *pak_dir = cJSON_GetObjectItemCaseSensitive(root, "pak_dir");
        const char *error_message = NULL;
        if (!cJSON_IsString(pak_dir) || !pak_dir->valuestring ||
            jw__request_launch_app(state, pak_dir->valuestring, &error_message) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, error_message ? error_message : "launch-app failed");
        }

        jw_log_info("launch-app requested pak=%s", pak_dir->valuestring);
        cJSON_Delete(root);
        return jw__reply_ok(client, "launch-app", NULL);
    }

    if (strcmp(type->valuestring, "package-quiesce-begin") == 0) {
        int rc = jw__handle_package_quiesce_begin(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "package-quiesce-end") == 0) {
        int rc = jw__handle_package_quiesce_end(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "package-mutation-begin") == 0) {
        int rc = jw__handle_package_mutation_begin(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "package-mutation-end") == 0) {
        int rc = jw__handle_package_mutation_end(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "reset-retroarch-config") == 0) {
        char status[256];
        if (jw_reset_retroarch_shared_config(state->sdcard_root,
                                             status, sizeof(status)) != 0) {
            cJSON_Delete(root);
            return jw__reply_error(client, status[0] ? status : "reset failed");
        }

        jw_log_info("reset-retroarch-config requested");
        cJSON_Delete(root);
        return jw__reply_ok(client, status[0] ? status : "reset-retroarch-config", NULL);
    }

    if (strcmp(type->valuestring, "platform-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_platform_status(state, client);
    }

    if (strcmp(type->valuestring, "platform-audio-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_platform_audio_status(state, client);
    }

    if (strcmp(type->valuestring, "platform-levels") == 0) {
        cJSON_Delete(root);
        return jw__reply_platform_levels(state, client);
    }

    if (strcmp(type->valuestring, "performance-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_performance_status(state, client);
    }

    if (strcmp(type->valuestring, "performance-set-profile") == 0) {
        int rc = jw__handle_performance_set_profile(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "performance-set-custom") == 0) {
        int rc = jw__handle_performance_set_custom(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "performance-reset-session") == 0) {
        cJSON_Delete(root);
        return jw__handle_performance_reset_session(state, client);
    }

    if (strcmp(type->valuestring, "update-status") == 0) {
        cJSON_Delete(root);
        return jw__reply_update_status(state, client);
    }

    if (strcmp(type->valuestring, "update-check") == 0) {
        int rc = jw__handle_update_check(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "update-releases") == 0) {
        int rc = jw__handle_update_releases(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "update-download") == 0) {
        cJSON_Delete(root);
        return jw__handle_update_download(state, client);
    }

    if (strcmp(type->valuestring, "update-select") == 0) {
        int rc = jw__handle_update_select(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "update-cancel") == 0) {
        cJSON_Delete(root);
        return jw__handle_update_cancel(state, client);
    }

    if (strcmp(type->valuestring, "update-install-preflight") == 0) {
        int rc = jw__handle_update_install_preflight(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "update-install") == 0) {
        int rc = jw__handle_update_install(state, client, root);
        cJSON_Delete(root);
        return rc;
    }

    if (strcmp(type->valuestring, "platform-action") == 0) {
        cJSON *action_json = cJSON_GetObjectItemCaseSensitive(root, "action");
        if (!cJSON_IsString(action_json) || !action_json->valuestring) {
            cJSON_Delete(root);
            return jw__reply_error(client, "missing platform action");
        }

        jw_platform_action action;
        if (!jw_platform_parse_action(action_json->valuestring, &action)) {
            jw_platform_result result;
            char action_name[64];
            snprintf(action_name, sizeof(action_name), "%s", action_json->valuestring);
            result.code = JW_PLATFORM_RESULT_INVALID;
            snprintf(result.message, sizeof(result.message), "unknown platform action: %s",
                     action_name);
            cJSON_Delete(root);
            return jw__reply_platform_result(client, action_name, &result);
        }

        int value = 0;
        cJSON *value_json = cJSON_GetObjectItemCaseSensitive(root, "value");
        if (cJSON_IsNumber(value_json)) {
            value = value_json->valueint;
        }
        if (action == JW_PLATFORM_ACTION_SET_AUDIO_OUTPUT) {
            cJSON *output_json = cJSON_GetObjectItemCaseSensitive(root, "output");
            jw_platform_audio_output parsed_output;
            if (cJSON_IsString(output_json) && output_json->valuestring &&
                jw_platform_parse_audio_output(output_json->valuestring, &parsed_output)) {
                value = (int)parsed_output;
            }
        }

        jw_platform_result result;
        if (action == JW_PLATFORM_ACTION_SET_BRIGHTNESS) {
            jw__set_brightness(state, value, true, true, &result);
        } else if (action == JW_PLATFORM_ACTION_SET_VOLUME) {
            jw_platform_perform_action(&state->platform, action, value, &result);
            if (result.code == JW_PLATFORM_RESULT_OK) {
                int resolved = result.has_value ? result.value : value;
                if (resolved < 0) resolved = 0;
                if (resolved > 100) resolved = 100;
                state->cached_volume_percent = resolved;
                jw__persist_volume(state, resolved);
                jw__osd_show_volume(state, resolved);
                jw__notify_launcher_levels(state);
            }
        } else if (action == JW_PLATFORM_ACTION_SET_AUDIO_OUTPUT) {
            jw_platform_perform_action(&state->platform, action, value, &result);
            if (result.code == JW_PLATFORM_RESULT_OK) {
                jw__publish_audio_env(state);
                /* Each output restores its own stored level, so the cached
                   percent belongs to the output we just left. */
                state->cached_volume_percent = -1;
                jw__notify_launcher_levels(state);
            }
        } else if (action == JW_PLATFORM_ACTION_SET_HDMI_OUTPUT) {
            jw_platform_perform_action(&state->platform, action, value, &result);
            if (result.code == JW_PLATFORM_RESULT_OK) {
                jw__schedule_lut_sync(state, value == 1 || value == 2);
            }
        } else if (action == JW_PLATFORM_ACTION_SLEEP) {
            bool inhibited = jw_suspend_inhibitor_count(&state->suspend_inhibitor) > 0;
            jw__deep_suspend(state);
            memset(&result, 0, sizeof(result));
            result.code = JW_PLATFORM_RESULT_OK;
            snprintf(result.message, sizeof(result.message), "%s",
                     inhibited ? "sleep pending: suspend inhibited" : "sleep resumed");
        } else if (action == JW_PLATFORM_ACTION_POWEROFF ||
                   action == JW_PLATFORM_ACTION_REBOOT) {
            /* The loong_power handoff shim sends reason=low-battery. */
            cJSON *reason_json = cJSON_GetObjectItemCaseSensitive(root, "reason");
            const char *reason = cJSON_IsString(reason_json) &&
                                         jw_power_request_reason_valid(reason_json->valuestring)
                                     ? reason_json->valuestring
                                     : "menu";
            bool accepted = jw__request_power_transition(state, action, reason);
            memset(&result, 0, sizeof(result));
            result.code = accepted ? JW_PLATFORM_RESULT_OK : JW_PLATFORM_RESULT_FAILED;
            snprintf(result.message, sizeof(result.message), "%s",
                     !accepted ? "safe power handoff unavailable" :
                     action == JW_PLATFORM_ACTION_REBOOT ? "rebooting" : "powering off");
        } else if (action == JW_PLATFORM_ACTION_BLUETOOTH_ON ||
                   action == JW_PLATFORM_ACTION_BLUETOOTH_OFF) {
            jw_platform_perform_action(&state->platform, action, value, &result);
            if (result.code == JW_PLATFORM_RESULT_OK && state->db_path[0]) {
                /* Persist the choice in Leaf's DB so it survives a reboot; the
                   stock BLUETOOTH_PARAM flag gets clobbered back to on at boot. */
                jw_db_set_setting(state->db_path, "platform.bluetooth_enabled",
                                  action == JW_PLATFORM_ACTION_BLUETOOTH_ON ? "1" : "0");
            }
        } else {
            jw_platform_perform_action(&state->platform, action, value, &result);
        }
        jw_log_info("platform-action requested action=%s code=%s",
                    jw_platform_action_name(action),
                    jw_platform_result_code_name(result.code));
        cJSON_Delete(root);
        return jw__reply_platform_result(client, jw_platform_action_name(action), &result);
    }

    if (strcmp(type->valuestring, "set-led") == 0) {
        jw_led_config led;
        memset(&led, 0, sizeof(led));
        led.mode = JW_LED_MODE_STATIC;
        led.brightness = 5;
        led.speed = 5;
        cJSON *v;
        v = cJSON_GetObjectItemCaseSensitive(root, "enabled");
        led.enabled = v && (cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valueint));
        v = cJSON_GetObjectItemCaseSensitive(root, "mode");
        if (cJSON_IsString(v)) jw_led_mode_parse(v->valuestring, &led.mode);
        v = cJSON_GetObjectItemCaseSensitive(root, "r"); if (cJSON_IsNumber(v)) led.r = (unsigned char)v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(root, "g"); if (cJSON_IsNumber(v)) led.g = (unsigned char)v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(root, "b"); if (cJSON_IsNumber(v)) led.b = (unsigned char)v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(root, "brightness"); if (cJSON_IsNumber(v)) led.brightness = v->valueint;
        v = cJSON_GetObjectItemCaseSensitive(root, "speed"); if (cJSON_IsNumber(v)) led.speed = v->valueint;

        jw__apply_led_config(state, &led);
        jw__persist_led(state, &led);
        jw_log_info("set-led mode=%s enabled=%d", jw_led_mode_name(led.mode), led.enabled);
        jw_platform_result result;
        result.code = JW_PLATFORM_RESULT_OK;
        result.has_value = false;
        result.value = 0;
        snprintf(result.message, sizeof(result.message), "%s", "led applied");
        cJSON_Delete(root);
        return jw__reply_platform_result(client, "set-led", &result);
    }

    if (strcmp(type->valuestring, "frontend-ready") == 0) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        if (!cJSON_IsString(role) || !role->valuestring || !role->valuestring[0]) {
            cJSON_Delete(root);
            return jw__reply_error(client, "missing frontend role");
        }

        jw_platform_result result;
        jw_platform_frontend_ready(&state->platform, role->valuestring, &result);
        jw_log_info("frontend-ready role=%s code=%s",
                    role->valuestring, jw_platform_result_code_name(result.code));
        if (state->startup_maintenance_pending) {
            long long accel = jw__monotonic_ms() + JW_STARTUP_MAINT_GRACE_MS;
            if (accel < state->startup_maintenance_next_ms) {
                state->startup_maintenance_next_ms = accel;
            }
        }
        cJSON_Delete(root);
        return jw__reply_platform_result(client, "frontend-ready", &result);
    }

    if (strcmp(type->valuestring, "exit-stock") == 0) {
        state->shutdown_requested = true;
        char crash_state[PATH_MAX];
        if (jw__env_or_join(crash_state, sizeof(crash_state),
                            "UMRK_CRASH_STATE",
                            "UMRK_INTERNAL_DATA_PATH", "USERDATA_PATH",
                            NULL, "umrk-launcher-crash-state") == 0) {
            unlink(crash_state);
        }
        char exit_sentinel[PATH_MAX];
        if (jw__env_or_join(exit_sentinel, sizeof(exit_sentinel),
                            "UMRK_EXIT_TO_STOCK_SENTINEL",
                            "TMPDIR", NULL,
                            "/tmp", "umrk-exit-to-stock") == 0) {
            FILE *fp = fopen(exit_sentinel, "w");
            if (fp) fclose(fp);
        }
        jw_log_info("exit-stock requested - passing this boot to stock");
        cJSON_Delete(root);
        return jw__reply_ok(client, "exit-stock", NULL);
    }

    if (strcmp(type->valuestring, "shutdown") == 0) {
        state->shutdown_requested = true;
        jw_log_info("shutdown requested");
        cJSON_Delete(root);
        return jw__reply_ok(client, "shutdown", NULL);
    }

    cJSON_Delete(root);
    return jw__reply_error(client, "unknown type");
}

static void jw__ipc_connection_drop(jw_daemon_state *state, int index,
                                    const char *reason) {
    if (!state || index < 0 || index >= JW_DAEMON_IPC_CONNECTION_MAX) {
        return;
    }
    jw_daemon_ipc_connection *connection = &state->ipc_connections[index];
    if (!connection->stream) {
        return;
    }
    if (connection->exchange_phase != JW_GAME_EXCHANGE_NONE &&
        state->game_coordination_pending) {
        jw__game_coordination_connection_failed(state, index, reason);
        connection = &state->ipc_connections[index];
        if (!connection->stream) {
            return;
        }
    }
    if (connection->subscribed) {
        jw_log_info("life1: unsubscribed service=%s reason=%s",
                    connection->service_id,
                    reason && reason[0] ? reason : "connection-closed");
    }
    /* A started deletion runs to the end without its connection. */
    if (state->delete_job && state->delete_connection == index) state->delete_connection = -1;
    jw_ipc_stream_destroy(connection->stream);
    memset(connection, 0, sizeof(*connection));
}

static bool jw__ipc_connection_queue_json(jw_daemon_state *state, int index,
                                          char *json) {
    if (!json || !state || index < 0 ||
        index >= JW_DAEMON_IPC_CONNECTION_MAX) {
        cJSON_free(json);
        return false;
    }
    jw_daemon_ipc_connection *connection = &state->ipc_connections[index];
    size_t len = strlen(json);
    bool ok = connection->stream && len <= JW_LIFE1_MAX_PAYLOAD &&
              jw_ipc_stream_queue(connection->stream, json, len) == 0;
    cJSON_free(json);
    if (!ok) {
        /* LIFE-1 backpressure is fail-closed: a full 16-message queue makes
         * the service unsubscribed until it reconnects and reconciles. */
        jw__ipc_connection_drop(state, index, "outbound-queue-overflow");
    }
    return ok;
}

static void jw__tick_delete(jw_daemon_state *state) {
    /* Sample busy first: a commit finishing between the two reads must still
       publish before a disconnected job is destroyed below. */
    bool busy = jw_delete_job_busy(state->delete_job);
    if (jw_delete_job_changed(state->delete_job)) jw__bump_library_generation(state);
    if (busy) return;
    if (state->delete_job && state->delete_connection < 0) {
        jw_delete_job_destroy(state->delete_job);
        state->delete_job = NULL;
    }
    if (state->delete_pending_scan) {
        state->delete_pending_scan = false;
        jw_scan_title_list titles = {0};
        pthread_mutex_lock(&state->scan_job.mu);
        jw__scan_title_list_move(&titles, &state->scan_job.pending_titles);
        pthread_mutex_unlock(&state->scan_job.mu);
        (void)jw__start_scan_job_with_titles(state, "after ROM deletion", &titles);
        jw__scan_title_list_free(&titles);
    }
}

static const char *jw__delete_gate(jw_daemon_state *state, const char *source,
                                  char readonly_source[32]) {
    jw__storage_health_refresh(state);
    const char *cards[] = {JW_PLATFORM_STORAGE_LAUNCHER_ID,
        !strcmp(source, "primary") ? JW_PLATFORM_STORAGE_LAUNCHER_ID : source};
    for (size_t i = 0; i < 2; ++i) {
        const jw_storage_health_slot *slot = jw__storage_slot(state, cards[i]);
        if (slot && slot->health.access == JW_STORAGE_ACCESS_READ_ONLY) {
            snprintf(readonly_source, 32, "%s", cards[i]);
            return "Your ROM card or library card is read-only.";
        }
        if (slot && slot->health.repair != JW_STORAGE_REPAIR_NONE)
            return "An SD card check is pending. Finish it before deleting ROMs.";
    }
    if (jw__relocation_scan_active(state)) return "Your library is scanning. Request a preview when it finishes.";
    if (state->active_game.active || state->pending_launch || state->pending_app ||
        state->game_coordination_pending || state->game_coordination_ready || state->game_launch_blocked ||
        (state->child_pid > 0 && state->child_kind != JW_CHILD_LAUNCHER && state->child_kind != JW_CHILD_MENU))
        return "Return to the launcher before deleting ROMs.";
    return NULL;
}

static void jw__delete_queue(jw_daemon_state *state, int index, cJSON *reply) {
    char *json = reply ? cJSON_PrintUnformatted(reply) : NULL;
    cJSON_Delete(reply);
    bool ok = json && state->ipc_connections[index].stream &&
        jw_ipc_stream_queue(state->ipc_connections[index].stream, json, strlen(json)) == 0;
    cJSON_free(json);
    if (!ok) jw__ipc_connection_drop(state, index, "deletion-response-failed");
}

static void jw__delete_error(jw_daemon_state *state, int index, const char *message,
                             const char *readonly_source) {
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddStringToObject(reply, "type", "rom-delete-status");
    cJSON_AddNumberToObject(reply, "phase", JW_IPC_DELETE_ERROR);
    cJSON_AddStringToObject(reply, "error", message);
    cJSON_AddStringToObject(reply, "readonly_source", readonly_source ? readonly_source : "");
    jw__delete_queue(state, index, reply);
}

/* rom-delete-preview answers with the game's files and their digest and keeps
   nothing. rom-delete starts removal of the file set with that digest; its
   connection then polls rom-delete-status until the job finishes. */
static bool jw__delete_frame(jw_daemon_state *state, int index, const char *body, size_t len) {
    cJSON *request = cJSON_ParseWithLength(body, len);
    cJSON *type = cJSON_GetObjectItemCaseSensitive(request, "type");
    if (!cJSON_IsString(type) || strncmp(type->valuestring, "rom-delete", 10)) {
        cJSON_Delete(request); return false;
    }
    jw__tick_delete(state);
    const char *error = NULL;
    char readonly_source[32] = "";
    cJSON *reply = NULL;
    bool preview = !strcmp(type->valuestring, "rom-delete-preview");
    bool start = !strcmp(type->valuestring, "rom-delete");
    if (state->ipc_connections[index].subscribed) error = "Use a separate connection for ROM deletion.";
    else if (preview || start) {
        cJSON *source = cJSON_GetObjectItemCaseSensitive(request, "source_id");
        cJSON *path = cJSON_GetObjectItemCaseSensitive(request, "rom_relpath");
        cJSON *digest = cJSON_GetObjectItemCaseSensitive(request, "digest");
        if (!cJSON_IsString(source) || !source->valuestring[0] || strlen(source->valuestring) >= 32 ||
            !cJSON_IsString(path) || !jw_storage_relative_path_valid(path->valuestring) ||
            strlen(path->valuestring) >= 512) error = "Invalid game identity.";
        else if (start && (!cJSON_IsString(digest) || strlen(digest->valuestring) != 16))
            error = "Open Delete Game again.";
        else if (state->delete_job) error = "A game is being deleted. Try again when it finishes.";
        else if (!(error = jw__delete_gate(state, source->valuestring, readonly_source))) {
            if (preview) {
                reply = jw_delete_preview(state->db_path, state->sdcard_root,
                                          source->valuestring, path->valuestring);
                if (!reply) error = "Could not check your game's files.";
            } else {
                state->delete_job = jw_delete_job_start(state->db_path, state->sdcard_root,
                    source->valuestring, path->valuestring, digest->valuestring);
                if (!state->delete_job) error = "Could not start deleting your game.";
                else state->delete_connection = index;
            }
        }
    } else if (strcmp(type->valuestring, "rom-delete-status")) error = "Unknown ROM deletion request.";
    else if (!state->delete_job || state->delete_connection != index) error = "This connection has no deletion running.";
    if (!error && !reply) reply = jw_delete_job_status(state->delete_job);
    if (error) jw__delete_error(state, index, error, readonly_source);
    else jw__delete_queue(state, index, reply);
    cJSON_Delete(request);
    return true;
}

static void jw__connection_exchange_clear(jw_daemon_ipc_connection *connection) {
    if (!connection) {
        return;
    }
    connection->exchange_kind = JW_GAME_EXCHANGE_KIND_NONE;
    connection->exchange_phase = JW_GAME_EXCHANGE_NONE;
    connection->exchange_started_ms = 0;
    connection->exchange_wait_deadline_ms = 0;
    connection->exchange_ack_deadline_ms = 0;
    connection->exchange_total_deadline_ms = 0;
    connection->exchange_pending_seen = false;
    connection->exchange_pending_items = 0;
    connection->exchange_pending_bytes = 0;
}

static int jw__usable_subscription(jw_daemon_state *state,
                                   const char *service_id) {
    if (!state || !service_id || !service_id[0]) {
        return -1;
    }
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX; i++) {
        jw_daemon_ipc_connection *connection = &state->ipc_connections[i];
        if (!connection->stream || !connection->subscribed ||
            !connection->reconciled ||
            strcmp(connection->service_id, service_id) != 0) {
            continue;
        }
        if (!jw_svc_supervisor_revalidate_subscriber(
                state->services, connection->service_id,
                &connection->binding)) {
            jw__ipc_connection_drop(state, i,
                                    "generation-revalidation-failed");
            continue;
        }
        return i;
    }
    return -1;
}

static void jw__broadcast_game_event(jw_daemon_state *state,
                                     const char *launch_id,
                                     const char *event) {
    if (!state || !launch_id || !launch_id[0]) {
        return;
    }
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX; i++) {
        jw_daemon_ipc_connection *connection = &state->ipc_connections[i];
        if (!connection->stream || !connection->subscribed ||
            !connection->reconciled) {
            continue;
        }
        if (!jw_svc_supervisor_revalidate_subscriber(
                state->services, connection->service_id,
                &connection->binding)) {
            jw__ipc_connection_drop(state, i,
                                    "generation-revalidation-failed");
            continue;
        }
        char *json = NULL;
        if (event && strcmp(event, "game.finish") == 0) {
            json = jw_life1_build_game_finish(launch_id);
        } else if (event && strcmp(event, "game.abort") == 0) {
            json = jw_life1_build_game_abort(launch_id);
        } else {
            json = jw_life1_build_game_cancel(launch_id);
        }
        (void)jw__ipc_connection_queue_json(
            state, i, json);
    }
}

static bool jw__active_game_clear_durable(jw_daemon_state *state,
                                          const char *context) {
    char reason[64];
    if (jw_active_game_clear(state->runtime_dir, reason, sizeof(reason))) {
        return true;
    }
    state->active_game.active = true;
    state->active_game.uncertain = true;
    if (state->services) {
        jw_svc_supervisor_game_set_active(state->services, true);
    }
    jw_log_error("life1: could not durably clear active launch after %s (%s); retaining conservative gate",
                 context ? context : "transition", reason);
    return false;
}

static void jw__game_coordination_abort(jw_daemon_state *state,
                                        const char *reason) {
    if (!state) {
        return;
    }
    char launch_id[JW_ACTIVE_GAME_LAUNCH_ID_MAX + 1];
    snprintf(launch_id, sizeof(launch_id), "%s",
             state->active_game.launch_id);
    bool check_abort = state->game_check_decision;
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX && !check_abort; i++) {
        check_abort = state->ipc_connections[i].exchange_kind ==
                      JW_GAME_EXCHANGE_KIND_CHECK;
    }
    state->game_coordination_pending = false;
    state->game_coordination_ready = false;
    jw__osd_game_launch_hide(state);
    long long launch_elapsed_ms = state->game_launch_started_ms > 0
        ? jw__monotonic_ms() - state->game_launch_started_ms : 0;
    state->game_launch_started_ms = 0;
    state->game_check_decision = false;
    state->game_check_service_id[0] = '\0';
    state->game_coordination_exchanges = 0;
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX; i++) {
        jw__connection_exchange_clear(&state->ipc_connections[i]);
    }
    state->pending_launch = false;
    state->pending_launch_resume_switcher = false;
    state->pending_launch_override_unverified = false;
    state->pending_launch_skip_check = false;
    jw__pending_launch_forget_target(state);
    state->active_game_writer_started = false;
    if (launch_id[0]) {
        jw__broadcast_game_event(state, launch_id,
                                 check_abort ? "game.abort" : "game.cancel");
    }
    if (state->active_game.active &&
        jw__active_game_clear_durable(state, "aborted launch")) {
        memset(&state->active_game, 0, sizeof(state->active_game));
        if (state->services) {
            jw_svc_supervisor_game_finish(state->services);
        }
    }
    jw_log_warn("life1: game launch aborted before writer start reason=%s "
                "elapsed_ms=%lld",
                reason && reason[0] ? reason : "coordination-failed",
                launch_elapsed_ms);
}

static void jw__game_coordination_block(jw_daemon_state *state,
                                        const char *service_id,
                                        const char *reason) {
    if (!state) {
        return;
    }
    bool resume_switcher = state->pending_launch_resume_switcher;
    char blocked_service[JW_SVC_SUPERVISOR_ID_BUF];
    char blocked_reason[JW_SVC_REASON_BUF];
    snprintf(blocked_service, sizeof(blocked_service), "%s",
             service_id ? service_id : "");
    snprintf(blocked_reason, sizeof(blocked_reason), "%s",
             reason && reason[0] ? reason : "unverified-service-stop");
    bool requires_verified_stop =
        strcmp(blocked_reason, "check-before-stop-failed") == 0 ||
        strcmp(blocked_reason, "unsafe-card-binding") == 0 ||
        strcmp(blocked_reason, "sync-wait-expired") == 0 ||
        strcmp(blocked_reason, "check-stop-unverified") == 0;
    /* A blocked launch waits for Play Anyway, which continues this same
       launch, so its core selection survives the abort. */
    jw_launch_target blocked_target = state->pending_launch_target;
    bool blocked_target_valid = state->pending_launch_target_valid;
    jw__game_coordination_abort(state, blocked_reason);
    state->pending_launch_target = blocked_target;
    state->pending_launch_target_valid = blocked_target_valid;
    state->game_launch_blocked = true;
    state->game_launch_blocked_resume_switcher = resume_switcher;
    state->game_launch_blocked_requires_verified_stop =
        requires_verified_stop;
    snprintf(state->game_launch_blocked_service_id,
             sizeof(state->game_launch_blocked_service_id), "%s",
             blocked_service);
    snprintf(state->game_launch_blocked_reason,
             sizeof(state->game_launch_blocked_reason), "%s",
             blocked_reason);
    jw_log_warn("life1: game launch requires explicit override service=%s "
                "reason=%s",
                blocked_service[0] ? blocked_service : "unknown",
                blocked_reason);
}

static bool jw__pending_launch_source_matches_record(jw_daemon_state *state) {
    jw_game_entry game;
    jw_storage_source_list sources;
    const jw_storage_source *source = NULL;
    char rom_abs[PATH_MAX];
    if (!state || !state->active_game.active ||
        jw__resolve_library_game(state, state->pending_launch_game_id,
                                 &game, &sources, &source,
                                 rom_abs, sizeof(rom_abs)) != 0 || !source) {
        return false;
    }
    return strcmp(source->id, state->active_game.source_id) == 0 &&
           strcmp(source->saves_path, state->active_game.saves_path) == 0 &&
           strcmp(source->states_path, state->active_game.states_path) == 0;
}

/* Returns 1 when the writer was spawned, 0 while exchanges remain, and -1
 * after a fail-closed abort. */
static int jw__game_coordination_launch_if_ready(jw_daemon_state *state) {
    if (!state || !state->game_coordination_pending ||
        state->game_coordination_exchanges > 0) {
        return 0;
    }
    if (!jw__pending_launch_source_matches_record(state)) {
        jw__game_coordination_abort(state, "source-binding-changed");
        return -1;
    }
    state->game_coordination_pending = false;
    if (state->child_pid > 0 && state->child_kind == JW_CHILD_LAUNCHER) {
        state->game_coordination_ready = true;
        kill(state->child_pid, SIGTERM);
        return 0;
    }
    if (jw__spawn_authorized_pending_game(state) != 0) {
        jw__game_coordination_abort(state, "writer-spawn-failed");
        return -1;
    }
    state->pending_launch_override_unverified = false;
    state->active_game_writer_started = true;
    jw__osd_game_launch_hide(state);
    jw_log_info("life1: writer started launch_id=%s pgid=%d total_ms=%lld",
                state->active_game.launch_id, (int)state->child_pgid,
                state->game_launch_started_ms > 0
                    ? jw__monotonic_ms() - state->game_launch_started_ms : 0);
    state->game_launch_started_ms = 0;
    return 1;
}

static bool jw__game_stop_service(jw_daemon_state *state,
                                  const char *service_id,
                                  const jw_svc_subscriber_binding *coordinator,
                                  const char *why) {
    char stop_reason[JW_SVC_REASON_BUF];
    jw_svc_stop_result result = {0};
    (void)jw__osd_game_launch(state, "stopping", 0);
    if (jw_svc_supervisor_game_stop_service(
            state->services, service_id, coordinator, &result,
            stop_reason, sizeof(stop_reason))) {
        jw_log_info("life1: verified service stop service=%s reason=%s "
                    "stop_ms=%d coordinator_first=%s coordinator_ms=%d "
                    "group_fallback=%s group_ms=%d kill=%s kill_ms=%d "
                    "verified_absence_ms=%d",
                    service_id, why ? why : "game-policy",
                    result.total_wait_ms,
                    result.coordinator_first ? "true" : "false",
                    result.coordinator_wait_ms,
                    result.coordinator_first && result.group_term_sent
                        ? "true" : "false",
                    result.group_wait_ms,
                    result.escalated_to_kill ? "true" : "false",
                    result.kill_wait_ms, result.total_wait_ms);
        return true;
    }
    jw_log_warn("life1: service=%s stop could not be verified reason=%s trigger=%s",
                service_id, stop_reason, why ? why : "game-policy");
    return false;
}

static int jw__begin_game_exchange(jw_daemon_state *state, int index) {
    jw_daemon_ipc_connection *connection = &state->ipc_connections[index];
    bool check = connection->mode == JW_LIFE1_MODE_STOP &&
                 connection->check_before_stop;
    if (check) {
        (void)jw__osd_game_launch(state, "checking", 0);
    }
    long long now = jw__monotonic_ms();
    connection->exchange_kind = check ? JW_GAME_EXCHANGE_KIND_CHECK
                                      : JW_GAME_EXCHANGE_KIND_NOTIFY;
    connection->exchange_phase = JW_GAME_EXCHANGE_AWAITING;
    connection->exchange_started_ms = now;
    connection->exchange_wait_deadline_ms = now + connection->wait_ms;
    connection->exchange_ack_deadline_ms = now + connection->ack_ms;
    connection->exchange_total_deadline_ms =
        now + connection->wait_ms + connection->ack_ms;
    connection->exchange_pending_seen = false;
    connection->exchange_pending_items = 0;
    connection->exchange_pending_bytes = 0;
    state->game_coordination_exchanges++;

    if (!jw__ipc_connection_queue_json(
            state, index,
            check ? jw_life1_build_game_check(
                        state->active_game.launch_id,
                        state->active_game.source_id,
                        state->active_game.saves_path,
                        state->active_game.states_path,
                        connection->wait_ms)
                  : jw_life1_build_game_start(
                        state->active_game.launch_id,
                        state->active_game.source_id,
                        state->active_game.saves_path,
                        state->active_game.states_path,
                        connection->wait_ms))) {
        return state->game_coordination_pending ? 0 : -1;
    }
    if (!check && connection->wait_ms == 0) {
        connection->exchange_phase = JW_GAME_EXCHANGE_ACK;
        if (!jw__ipc_connection_queue_json(
                state, index,
                jw_life1_build_game_cancel(state->active_game.launch_id))) {
            return state->game_coordination_pending ? 0 : -1;
        }
    }
    jw_log_info("life1: game.%s service=%s launch_id=%s wait_ms=%d ack_ms=%d",
                check ? "check" : "start",
                connection->service_id, state->active_game.launch_id,
                connection->wait_ms, connection->ack_ms);
    return 0;
}

static int jw__spawn_pending_game(jw_daemon_state *state) {
    if (!state || !state->pending_launch) {
        return -1;
    }
    if (jw_delete_job_busy(state->delete_job)) return -1;
    if (state->active_game.active) {
        jw_log_warn("life1: refusing new game while launch state is %s",
                    state->active_game.uncertain ? "uncertain" : "active");
        state->pending_launch = false;
        jw__pending_launch_forget_target(state);
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        return -1;
    }

    jw_game_entry game;
    jw_storage_source_list sources;
    const jw_storage_source *source = NULL;
    char rom_abs[PATH_MAX];
    jw_launch_target target;
    if (jw__resolve_library_game(state, state->pending_launch_game_id,
                                 &game, &sources, &source,
                                 rom_abs, sizeof(rom_abs)) != 0 || !source ||
        jw__pending_launch_target(state, &target) != 0) {
        state->pending_launch = false;
        jw__pending_launch_forget_target(state);
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        return -1;
    }

    jw_active_game record;
    memset(&record, 0, sizeof(record));
    record.active = true;
    if (!jw_active_game_generate_id(record.launch_id,
                                    sizeof(record.launch_id)) ||
        snprintf(record.source_id, sizeof(record.source_id), "%s",
                 source->id) >= (int)sizeof(record.source_id) ||
        snprintf(record.saves_path, sizeof(record.saves_path), "%s",
                 source->saves_path) >= (int)sizeof(record.saves_path) ||
        snprintf(record.states_path, sizeof(record.states_path), "%s",
                 source->states_path) >= (int)sizeof(record.states_path)) {
        state->pending_launch = false;
        jw__pending_launch_forget_target(state);
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        return -1;
    }
    char persist_reason[64];
    if (!jw_active_game_persist(state->runtime_dir, &record,
                                persist_reason, sizeof(persist_reason))) {
        jw_log_error("life1: active launch commit failed (%s)", persist_reason);
        state->pending_launch = false;
        jw__pending_launch_forget_target(state);
        state->pending_launch_resume_switcher = false;
        state->pending_launch_override_unverified = false;
        return -1;
    }
    state->active_game = record;
    state->active_game_writer_started = false;
    state->game_coordination_pending = true;
    state->game_coordination_ready = false;
    state->game_coordination_exchanges = 0;
    state->game_launch_started_ms = jw__monotonic_ms();
    if (state->services) {
        jw_svc_supervisor_game_set_active(state->services, true);
    }
    jw_log_info("life1: active launch committed launch_id=%s source=%s kind=%s",
                record.launch_id, record.source_id,
                target.kind == JW_LAUNCH_TARGET_STANDALONE
                    ? "EMULATOR" : "RETROARCH");

    /* Preflight every stale generation before signalling any healthy service.
     * Only a trustworthy retained ignore policy is outside coordination. */
    if (state->services) {
        int service_count = jw_svc_supervisor_count(state->services);
        for (int i = 0; i < service_count; i++) {
            const jw_svc_supervised *entry =
                jw_svc_supervisor_at(state->services, i);
            if (entry && entry->state == JW_SVC_STATE_STALE_GENERATION &&
                entry->active_lifecycle_game !=
                    JW_SVC_LIFECYCLE_GAME_IGNORE) {
                if (state->pending_launch_override_unverified) {
                    jw_log_warn("life1: explicit override bypassing stale "
                                "service generation service=%s",
                                entry->service_id);
                    continue;
                }
                jw__game_coordination_block(
                    state, entry->service_id, "stale-service-generation");
                return -1;
            }
        }

        for (int i = 0; i < service_count; i++) {
            const jw_svc_supervised *entry =
                jw_svc_supervisor_at(state->services, i);
            if (!entry || entry->state == JW_SVC_STATE_STALE_GENERATION ||
                entry->pgid <= 0) {
                continue;
            }
            char service_id[JW_SVC_SUPERVISOR_ID_BUF];
            snprintf(service_id, sizeof(service_id), "%s", entry->service_id);
            int subscriber = jw__usable_subscription(state, service_id);
            jw_svc_lifecycle_game policy = entry->active_lifecycle_game;
            if (subscriber >= 0) {
                policy = state->ipc_connections[subscriber].mode ==
                                 JW_LIFE1_MODE_STOP
                             ? JW_SVC_LIFECYCLE_GAME_STOP
                             : JW_SVC_LIFECYCLE_GAME_NOTIFY;
            }
            if (policy == JW_SVC_LIFECYCLE_GAME_IGNORE) {
                continue;
            }
            bool check_before_stop =
                subscriber >= 0 && policy == JW_SVC_LIFECYCLE_GAME_STOP &&
                state->ipc_connections[subscriber].check_before_stop &&
                !state->pending_launch_override_unverified &&
                !state->pending_launch_skip_check;
            if (check_before_stop) {
                if (jw__begin_game_exchange(state, subscriber) != 0) {
                    return -1;
                }
                continue;
            }
            if (policy == JW_SVC_LIFECYCLE_GAME_STOP || subscriber < 0) {
                if (!jw__game_stop_service(
                        state, service_id, NULL,
                        policy == JW_SVC_LIFECYCLE_GAME_STOP
                            ? "mode-stop" : "notify-unsubscribed")) {
                    if (state->pending_launch_override_unverified) {
                        jw_log_warn("life1: explicit override bypassing "
                                    "unverified service stop service=%s",
                                    service_id);
                        continue;
                    }
                    jw__game_coordination_block(
                        state, service_id,
                        state->pending_launch_skip_check
                            ? "check-stop-unverified"
                            : "unverified-service-stop");
                    return -1;
                }
                continue;
            }
            if (jw__begin_game_exchange(state, subscriber) != 0) {
                return -1;
            }
        }
    }

    /* RAOfflineProxy routing decision: bounded wait and fail-closed prompt
       happen here, alongside the other blocks, never inside the writer spawn
       (whose failure would re-abort with "writer-spawn-failed" and scramble
       the blocked record the override flow depends on). RetroArch targets
       only; standalone launches and the pak runner never route. */
    if (target.kind == JW_LAUNCH_TARGET_RETROARCH &&
        jw__raofflineproxy_route(state, &state->pending_rop_snapshot) ==
            JW__ROP_GATE_BLOCKED) {
        return -1;
    }

    int launched = jw__game_coordination_launch_if_ready(state);
    return launched < 0 ? -1 : 0;
}

static void jw__game_coordination_connection_failed(jw_daemon_state *state,
                                                     int index,
                                                     const char *reason) {
    if (!state || index < 0 || index >= JW_DAEMON_IPC_CONNECTION_MAX) {
        return;
    }
    jw_daemon_ipc_connection *connection = &state->ipc_connections[index];
    if (connection->exchange_phase == JW_GAME_EXCHANGE_NONE) {
        return;
    }
    bool check = connection->exchange_kind == JW_GAME_EXCHANGE_KIND_CHECK;
    char service_id[JW_SVC_SUPERVISOR_ID_BUF];
    snprintf(service_id, sizeof(service_id), "%s", connection->service_id);
    jw__connection_exchange_clear(connection);
    if (state->game_coordination_exchanges > 0) {
        state->game_coordination_exchanges--;
    }
    if (check && !state->pending_launch_override_unverified) {
        const char *blocked_reason =
            reason && strcmp(reason, "unsafe-card-binding") == 0
                ? "unsafe-card-binding"
                : "check-before-stop-failed";
        state->game_check_decision = true;
        snprintf(state->game_check_service_id,
                 sizeof(state->game_check_service_id), "%s", service_id);
        jw_log_warn("life1: check-before-stop failed service=%s trigger=%s",
                    service_id, reason && reason[0] ? reason : "exchange-failure");
        jw__game_coordination_block(state, service_id, blocked_reason);
        return;
    }
    if (!state->services || !jw__game_stop_service(
            state, service_id, NULL,
            reason && reason[0] ? reason : "exchange-failure")) {
        if (state->pending_launch_override_unverified) {
            jw_log_warn("life1: explicit override bypassing unverified "
                        "exchange fallback service=%s",
                        service_id);
        } else {
            jw__game_coordination_block(
                state, service_id, "exchange-fallback-unverified");
        }
    }
}

static bool jw__game_exchange_send_cancel(jw_daemon_state *state, int index,
                                          long long now,
                                          const char *reason) {
    jw_daemon_ipc_connection *connection = &state->ipc_connections[index];
    if (!connection->stream ||
        connection->exchange_phase == JW_GAME_EXCHANGE_NONE) {
        return false;
    }
    connection->exchange_phase = JW_GAME_EXCHANGE_ACK;
    long long ack_deadline = now + connection->ack_ms;
    if (ack_deadline > connection->exchange_total_deadline_ms) {
        ack_deadline = connection->exchange_total_deadline_ms;
    }
    connection->exchange_ack_deadline_ms = ack_deadline;
    bool queued = jw__ipc_connection_queue_json(
        state, index,
        jw_life1_build_game_cancel(state->active_game.launch_id));
    if (queued) {
        jw_log_info("life1: game.cancel service=%s launch_id=%s reason=%s",
                    connection->service_id, state->active_game.launch_id,
                    reason ? reason : "wait-ended");
    }
    return queued;
}

static int jw__game_check_exchange(jw_daemon_state *state) {
    if (!state || !state->game_coordination_pending) {
        return -1;
    }
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX; i++) {
        const jw_daemon_ipc_connection *connection =
            &state->ipc_connections[i];
        if (connection->stream &&
            connection->exchange_kind == JW_GAME_EXCHANGE_KIND_CHECK &&
            connection->exchange_phase != JW_GAME_EXCHANGE_NONE) {
            return i;
        }
    }
    return -1;
}

static bool jw__game_check_wait(jw_daemon_state *state) {
    int index = jw__game_check_exchange(state);
    if (index < 0) {
        return false;
    }
    jw_daemon_ipc_connection *connection = &state->ipc_connections[index];
    if (connection->exchange_phase != JW_GAME_EXCHANGE_DECISION) {
        return false;
    }
    long long now = jw__monotonic_ms();
    connection->exchange_phase = JW_GAME_EXCHANGE_WAITING;
    connection->exchange_wait_deadline_ms = now + connection->wait_ms;
    connection->exchange_ack_deadline_ms = now + connection->ack_ms;
    connection->exchange_total_deadline_ms =
        now + connection->wait_ms + connection->ack_ms;
    state->game_check_decision = false;
    jw_log_info("life1: user chose Wait for sync service=%s launch_id=%s wait_ms=%d",
                connection->service_id, state->active_game.launch_id,
                connection->wait_ms);
    return true;
}

static bool jw__game_check_play_anyway(jw_daemon_state *state) {
    int index = jw__game_check_exchange(state);
    if (index < 0) {
        return false;
    }
    jw_daemon_ipc_connection *connection = &state->ipc_connections[index];
    char service_id[JW_SVC_SUPERVISOR_ID_BUF];
    snprintf(service_id, sizeof(service_id), "%s", connection->service_id);
    if (!jw__game_exchange_send_cancel(state, index, jw__monotonic_ms(),
                                       "play-anyway")) {
        return false;
    }
    (void)jw_ipc_stream_flush(connection->stream);
    jw__connection_exchange_clear(connection);
    if (state->game_coordination_exchanges > 0) {
        state->game_coordination_exchanges--;
    }
    state->game_check_decision = false;
    state->game_check_pending_items = 0;
    state->game_check_pending_bytes = 0;
    state->game_check_service_id[0] = '\0';
    if (!state->services ||
        !jw__game_stop_service(state, service_id, &connection->binding,
                               "play-anyway")) {
        jw__game_coordination_block(state, service_id,
                                    "check-stop-unverified");
        return false;
    }
    (void)jw__game_coordination_launch_if_ready(state);
    return true;
}

/* The launcher's Cancel on a blocked launch. True when one was pending. */
static bool jw__game_launch_blocked_dismiss(jw_daemon_state *state) {
    bool dismissed = state->game_launch_blocked;
    state->game_launch_blocked = false;
    state->game_launch_blocked_resume_switcher = false;
    state->pending_launch_override_unverified = false;
    state->pending_launch_skip_check = false;
    state->game_launch_blocked_requires_verified_stop = false;
    state->pending_launch = false;
    jw__pending_launch_forget_target(state);
    state->game_launch_blocked_service_id[0] = '\0';
    state->game_launch_blocked_reason[0] = '\0';
    return dismissed;
}

static bool jw__game_check_cancel(jw_daemon_state *state) {
    if (jw__game_check_exchange(state) < 0) {
        return false;
    }
    jw_log_info("life1: user cancelled check-before-stop launch_id=%s",
                state->active_game.launch_id);
    jw__game_coordination_abort(state, "user-cancelled-sync-check");
    state->game_launch_blocked = false;
    state->game_launch_blocked_service_id[0] = '\0';
    state->game_launch_blocked_reason[0] = '\0';
    return true;
}

static bool jw__game_coordination_start_now(jw_daemon_state *state,
                                            const char *reason) {
    if (!state || !state->game_coordination_pending) {
        return false;
    }
    bool waiting = false;
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX; i++) {
        const jw_daemon_ipc_connection *connection =
            &state->ipc_connections[i];
        if (connection->stream &&
            connection->exchange_phase == JW_GAME_EXCHANGE_WAITING) {
            waiting = true;
            break;
        }
    }
    if (!waiting) {
        return false;
    }

    long long now = jw__monotonic_ms();
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX &&
                    state->game_coordination_pending; i++) {
        jw_daemon_ipc_connection *connection = &state->ipc_connections[i];
        if (connection->stream &&
            connection->exchange_phase == JW_GAME_EXCHANGE_WAITING) {
            (void)jw__game_exchange_send_cancel(
                state, i, now, reason ? reason : "start-now");
        }
    }
    return true;
}

static void jw__game_coordination_status(jw_daemon_state *state, int index,
                                         const jw_life1_status *status) {
    jw_daemon_ipc_connection *connection = &state->ipc_connections[index];
    if (!connection->subscribed || !status || !status->launch_id) {
        return;
    }
    if (!state->active_game.active || state->active_game.uncertain ||
        strcmp(status->launch_id, state->active_game.launch_id) != 0) {
        (void)jw__ipc_connection_queue_json(
            state, index,
            jw_life1_build_error("", "stale-launch-id",
                                 "lifecycle reply does not name the authoritative launch"));
        return;
    }
    if (!jw_svc_supervisor_revalidate_subscriber(
            state->services, connection->service_id,
            &connection->binding)) {
        jw__ipc_connection_drop(state, index,
                                "generation-revalidation-failed");
        return;
    }
    if (connection->exchange_phase == JW_GAME_EXCHANGE_NONE) {
        /* ready/error duplicates for the current launch are no-ops once its
         * terminal reply was already consumed. */
        return;
    }
    if (status->kind == JW_LIFE1_STATUS_ERROR) {
        jw_log_warn("life1: service=%s returned error launch_id=%s reason=%s",
                    connection->service_id, status->launch_id,
                    status->reason ? status->reason : "unknown");
        jw__ipc_connection_drop(
            state, index,
            status->reason && strcmp(status->reason, "unsafe-card-binding") == 0
                ? "unsafe-card-binding"
                : "service-error");
        return;
    }
    if (connection->exchange_kind == JW_GAME_EXCHANGE_KIND_CHECK) {
        if (status->kind == JW_LIFE1_STATUS_READY) {
            jw__ipc_connection_drop(state, index,
                                    "ready-invalid-for-check");
            return;
        }
        if (status->kind == JW_LIFE1_STATUS_STOP) {
            char service_id[JW_SVC_SUPERVISOR_ID_BUF];
            snprintf(service_id, sizeof(service_id), "%s",
                     connection->service_id);
            jw_log_info("life1: check current service=%s launch_id=%s elapsed_ms=%lld",
                        service_id, status->launch_id,
                        jw__monotonic_ms() - connection->exchange_started_ms);
            jw__connection_exchange_clear(connection);
            if (state->game_coordination_exchanges > 0) {
                state->game_coordination_exchanges--;
            }
            state->game_check_decision = false;
            state->game_check_pending_items = 0;
            state->game_check_pending_bytes = 0;
            state->game_check_service_id[0] = '\0';
            if (!state->services || !jw__game_stop_service(
                    state, service_id, &connection->binding,
                    "check-before-stop-current")) {
                jw__game_coordination_block(
                    state, service_id, "check-stop-unverified");
                return;
            }
            (void)jw__game_coordination_launch_if_ready(state);
            return;
        }
        if (status->kind != JW_LIFE1_STATUS_WAITING ||
            !status->has_pending_bytes ||
            (status->pending_items == 0 && status->pending_bytes == 0) ||
            connection->exchange_phase == JW_GAME_EXCHANGE_ACK) {
            jw__ipc_connection_drop(state, index,
                                    "malformed-check-status");
            return;
        }

        connection->exchange_pending_seen = true;
        connection->exchange_pending_items = status->pending_items;
        connection->exchange_pending_bytes = status->pending_bytes;
        if (connection->exchange_phase == JW_GAME_EXCHANGE_AWAITING) {
            connection->exchange_phase = JW_GAME_EXCHANGE_DECISION;
        }
        state->game_check_decision =
            connection->exchange_phase == JW_GAME_EXCHANGE_DECISION;
        state->game_check_pending_items = status->pending_items;
        state->game_check_pending_bytes = status->pending_bytes;
        snprintf(state->game_check_service_id,
                 sizeof(state->game_check_service_id), "%s",
                 connection->service_id);
        (void)jw__osd_game_launch(state, "syncing", status->pending_items);
        jw_log_info("life1: check pending service=%s launch_id=%s pending_items=%d pending_bytes=%lld",
                    connection->service_id, status->launch_id,
                    status->pending_items, (long long)status->pending_bytes);
        return;
    }
    if (status->kind == JW_LIFE1_STATUS_READY) {
        jw_log_info("life1: service ready service=%s launch_id=%s elapsed_ms=%lld",
                    connection->service_id, status->launch_id,
                    jw__monotonic_ms() - connection->exchange_started_ms);
        jw__connection_exchange_clear(connection);
        if (state->game_coordination_exchanges > 0) {
            state->game_coordination_exchanges--;
        }
        return;
    }
    if (status->kind != JW_LIFE1_STATUS_WAITING ||
        status->has_pending_bytes) {
        jw__ipc_connection_drop(state, index, "malformed-exchange-status");
        return;
    }

    long long now = jw__monotonic_ms();
    if (connection->wait_ms <= 0 ||
        connection->exchange_phase == JW_GAME_EXCHANGE_ACK) {
        if (connection->exchange_phase != JW_GAME_EXCHANGE_ACK) {
            (void)jw__game_exchange_send_cancel(state, index, now,
                                                "waiting-not-enabled");
        }
        return;
    }
    bool progress = !connection->exchange_pending_seen ||
                    status->pending_items < connection->exchange_pending_items;
    if (!progress) {
        (void)jw__game_exchange_send_cancel(state, index, now,
                                            "waiting-stalled");
        return;
    }
    connection->exchange_pending_seen = true;
    connection->exchange_pending_items = status->pending_items;
    connection->exchange_phase = JW_GAME_EXCHANGE_WAITING;
    (void)jw__osd_game_launch(state, "syncing", status->pending_items);
    jw_log_info("life1: waiting service=%s launch_id=%s pending_items=%d",
                connection->service_id, status->launch_id,
                status->pending_items);
}

static void jw__game_coordination_tick(jw_daemon_state *state) {
    if (!state || !state->game_coordination_pending) {
        return;
    }
    long long now = jw__monotonic_ms();
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX &&
                    state->game_coordination_pending; i++) {
        jw_daemon_ipc_connection *connection = &state->ipc_connections[i];
        if (!connection->stream ||
            connection->exchange_phase == JW_GAME_EXCHANGE_NONE) {
            continue;
        }
        if (!jw_svc_supervisor_revalidate_subscriber(
                state->services, connection->service_id,
                &connection->binding)) {
            jw__ipc_connection_drop(state, i,
                                    "generation-revalidation-failed");
            continue;
        }
        if (connection->exchange_phase == JW_GAME_EXCHANGE_AWAITING &&
            now >= connection->exchange_ack_deadline_ms) {
            jw__ipc_connection_drop(state, i, "initial-ack-timeout");
            continue;
        }
        if (connection->exchange_phase == JW_GAME_EXCHANGE_WAITING &&
            now >= connection->exchange_wait_deadline_ms) {
            if (connection->exchange_kind == JW_GAME_EXCHANGE_KIND_CHECK) {
                char service_id[JW_SVC_SUPERVISOR_ID_BUF];
                snprintf(service_id, sizeof(service_id), "%s",
                         connection->service_id);
                jw__game_coordination_block(state, service_id,
                                            "sync-wait-expired");
                continue;
            }
            (void)jw__game_exchange_send_cancel(state, i, now,
                                                "wait-budget-expired");
            continue;
        }
        if (connection->exchange_phase == JW_GAME_EXCHANGE_ACK &&
            now >= connection->exchange_ack_deadline_ms) {
            jw__ipc_connection_drop(state, i, "ready-ack-timeout");
        }
    }
    if (state->game_coordination_pending) {
        (void)jw__game_coordination_launch_if_ready(state);
    }
}

static void jw__active_game_finish(jw_daemon_state *state) {
    if (!state || !state->active_game.active ||
        !state->active_game_writer_started) {
        return;
    }
    char launch_id[JW_ACTIVE_GAME_LAUNCH_ID_MAX + 1];
    snprintf(launch_id, sizeof(launch_id), "%s",
             state->active_game.launch_id);
    state->active_game_writer_started = false;
    if (!jw__active_game_clear_durable(state, "writer-exit barrier")) {
        return;
    }
    memset(&state->active_game, 0, sizeof(state->active_game));
    jw__broadcast_game_event(state, launch_id, "game.finish");
    if (state->services) {
        jw_svc_supervisor_game_finish(state->services);
    }
    jw_log_info("life1: game.finish launch_id=%s after writer barrier",
                launch_id);
}

static const char *jw__life1_auth_code(
    jw_svc_subscriber_auth_result result) {
    switch (result) {
    case JW_SVC_SUBSCRIBER_MISSING_CREDENTIAL:
        return "missing-peer-credential";
    case JW_SVC_SUBSCRIBER_UNKNOWN_SERVICE:
        return "unknown-service";
    case JW_SVC_SUBSCRIBER_STALE_GENERATION:
        return "stale-generation-peer";
    case JW_SVC_SUBSCRIBER_WRONG_GROUP:
        return "wrong-group-peer";
    case JW_SVC_SUBSCRIBER_FOREGROUND:
        return "foreground-app-peer";
    case JW_SVC_SUBSCRIBER_ACCEPTED:
        break;
    }
    return "internal-error";
}

static const char *jw__life1_auth_message(
    jw_svc_subscriber_auth_result result) {
    switch (result) {
    case JW_SVC_SUBSCRIBER_MISSING_CREDENTIAL:
        return "unable to obtain peer credentials for this connection";
    case JW_SVC_SUBSCRIBER_UNKNOWN_SERVICE:
        return "declared service id is not known to the supervisor";
    case JW_SVC_SUBSCRIBER_STALE_GENERATION:
        return "subscriber pid is not a member of the current generation's reserved process group";
    case JW_SVC_SUBSCRIBER_WRONG_GROUP:
        return "subscriber pid belongs to a different service's process group";
    case JW_SVC_SUBSCRIBER_FOREGROUND:
        return "subscriber pid is a foreground app, not a supervised service process";
    case JW_SVC_SUBSCRIBER_ACCEPTED:
        break;
    }
    return "subscriber authentication failed";
}

static void jw__ipc_replace_registration(jw_daemon_state *state, int keep,
                                         const char *service_id) {
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX; i++) {
        if (i == keep) {
            continue;
        }
        jw_daemon_ipc_connection *other = &state->ipc_connections[i];
        if (other->stream && other->subscribed &&
            strcmp(other->service_id, service_id) == 0) {
            jw__ipc_connection_drop(state, i, "replaced-by-resubscribe");
        }
    }
}

static void jw__ipc_handle_life1(jw_daemon_state *state, int index,
                                 jw_life1_request *request) {
    jw_daemon_ipc_connection *connection = &state->ipc_connections[index];
    if (request->kind == JW_LIFE1_REQUEST_SUBSCRIBE) {
        pid_t peer_pid = -1;
        bool credential_ok =
            jw_ipc_stream_peer_pid(connection->stream, &peer_pid) == 0;
        jw_svc_subscriber_binding binding;
        jw_svc_subscriber_auth_result auth = credential_ok
            ? jw_svc_supervisor_authenticate_subscriber(
                  state->services, request->service_id, peer_pid, &binding)
            : JW_SVC_SUBSCRIBER_MISSING_CREDENTIAL;
        if (auth != JW_SVC_SUBSCRIBER_ACCEPTED) {
            if (jw__ipc_connection_queue_json(
                    state, index,
                    jw_life1_build_error(request->id,
                                         jw__life1_auth_code(auth),
                                         jw__life1_auth_message(auth)))) {
                state->ipc_connections[index].close_after_flush = true;
            }
            return;
        }

        /* Queue the ack before replacing a healthy registration. If this
         * connection is already backpressured, the old subscriber remains. */
        if (!jw__ipc_connection_queue_json(
                state, index, jw_life1_build_ok(request->id))) {
            return;
        }
        jw__ipc_replace_registration(state, index, request->service_id);
        connection = &state->ipc_connections[index];
        connection->subscribed = true;
        connection->reconciled = false;
        snprintf(connection->service_id, sizeof(connection->service_id), "%s",
                 request->service_id);
        connection->mode = request->mode;
        connection->ack_ms = request->ack_ms;
        connection->wait_ms = request->wait_ms;
        connection->check_before_stop = request->check_before_stop;
        connection->binding = binding;
        jw_log_info("life1: subscribed service=%s pid=%d pgid=%d mode=%s "
                    "ack_ms=%d wait_ms=%d check_before_stop=%s; awaiting game.state",
                    connection->service_id, (int)binding.peer_pid,
                    (int)binding.pgid,
                    connection->mode == JW_LIFE1_MODE_STOP ? "stop" : "notify",
                    connection->ack_ms, connection->wait_ms,
                    connection->check_before_stop ? "true" : "false");
        return;
    }

    if (!connection->subscribed) {
        if (jw__ipc_connection_queue_json(
                state, index,
                jw_life1_build_error(request->id, "not-subscribed",
                                     "game.state requires an accepted subscription"))) {
            state->ipc_connections[index].close_after_flush = true;
        }
        return;
    }
    if (!jw_svc_supervisor_revalidate_subscriber(
            state->services, connection->service_id, &connection->binding)) {
        jw__ipc_connection_drop(state, index, "generation-revalidation-failed");
        return;
    }
    char *reply = NULL;
    if (state->active_game.active && state->active_game.uncertain) {
        reply = jw_life1_build_error(
            request->id, "active-state-uncertain",
            "an active-game record exists but its launch identity is not trustworthy");
    } else if (state->active_game.active) {
        reply = jw_life1_build_game_state_active(
            request->id, state->active_game.launch_id,
            state->active_game.source_id, state->active_game.saves_path,
            state->active_game.states_path);
    } else {
        reply = jw_life1_build_game_state_inactive(request->id);
    }
    if (jw__ipc_connection_queue_json(state, index, reply)) {
        state->ipc_connections[index].reconciled =
            !state->active_game.uncertain;
        state->ipc_connections[index].stop_after_flush =
            state->active_game.active && !state->active_game.uncertain &&
            state->ipc_connections[index].mode == JW_LIFE1_MODE_STOP;
        jw_log_info("life1: reconciled service=%s active=%s%s",
                    state->ipc_connections[index].service_id,
                    state->active_game.active ? "true" : "false",
                    state->active_game.uncertain ? " uncertain" : "");
    }
}

static void jw__ipc_handle_frame(jw_daemon_state *state, int index,
                                 const char *body, size_t len) {
    if (jw__delete_frame(state, index, body, len)) return;
    if (state->delete_job && state->delete_connection == index) {
        jw__ipc_connection_drop(state, index, "unexpected-deletion-message");
        return;
    }
    jw_daemon_ipc_connection *connection = &state->ipc_connections[index];
    jw_life1_request request;
    char error[32] = {0};
    jw_life1_parse_result parsed =
        jw_life1_parse_request(body, len, &request, error, sizeof(error));
    if (parsed == JW_LIFE1_PARSE_OK) {
        jw__ipc_handle_life1(state, index, &request);
        jw_life1_request_destroy(&request);
        return;
    }
    if (parsed == JW_LIFE1_PARSE_INVALID) {
        if (jw__ipc_connection_queue_json(
                state, index,
                jw_life1_build_error(
                    request.id, error[0] ? error : "invalid-payload",
                    strcmp(error, "unsupported-version") == 0
                        ? "server supports LIFE-1 v1 only"
                        : "invalid LIFE-1 request"))) {
            state->ipc_connections[index].close_after_flush = true;
        }
        jw_life1_request_destroy(&request);
        return;
    }
    jw_life1_request_destroy(&request);

    if (connection->subscribed) {
        jw_life1_status status;
        char status_error[32] = {0};
        jw_life1_parse_result status_parsed = jw_life1_parse_status(
            body, len, &status, status_error, sizeof(status_error));
        if (status_parsed == JW_LIFE1_PARSE_OK) {
            jw__game_coordination_status(state, index, &status);
            jw_life1_status_destroy(&status);
            return;
        }
        jw_life1_status_destroy(&status);
        if (connection->exchange_phase != JW_GAME_EXCHANGE_NONE &&
            state->game_coordination_pending) {
            /* Malformed lifecycle data is an exchange failure, which takes
             * the verified-stop fallback rather than merely rejecting JSON. */
            jw__ipc_connection_drop(state, index, "malformed-exchange-message");
            return;
        }
        if (jw__ipc_connection_queue_json(
                state, index,
                jw_life1_build_error("", "invalid-payload",
                                     "unexpected subscriber message"))) {
            state->ipc_connections[index].close_after_flush = true;
        }
        return;
    }

    /* Existing callers remain one-shot and use the unchanged blocking handler
     * only after their complete frame was accumulated without blocking the
     * daemon. */
    jw_ipc_client *client =
        jw_ipc_stream_detach_blocking(connection->stream);
    if (!client) {
        jw__ipc_connection_drop(state, index, "legacy-detach-failed");
        return;
    }
    memset(connection, 0, sizeof(*connection));
    if (jw__handle_message(state, client, body, len) != 0) {
        jw_log_warn("ipc handler failed to answer a request");
    }
    jw_ipc_client_close(client);
}

static int jw__ipc_find_free_connection(jw_daemon_state *state) {
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX; i++) {
        if (!state->ipc_connections[i].stream) {
            return i;
        }
    }
    return -1;
}

static void jw__ipc_accept_ready(jw_daemon_state *state) {
    for (;;) {
        jw_ipc_client *client = NULL;
        int rc = jw_ipc_server_accept(state->server, &client, 0);
        if (rc != 0) {
            return;
        }
        int slot = jw__ipc_find_free_connection(state);
        if (slot < 0) {
            jw_log_warn("ipc: connection limit reached");
            jw_ipc_client_close(client);
            continue;
        }
        jw_ipc_stream *stream = NULL;
        if (jw_ipc_stream_create(client, &stream) != 0) {
            jw_log_warn("ipc: could not make accepted client non-blocking");
            jw_ipc_client_close(client);
            continue;
        }
        state->ipc_connections[slot].stream = stream;
    }
}

/* A game or app is the foreground child: audio can start at any moment and
   most of the in-game machinery below is live. The launcher and the Settings
   menu are not content. */
static bool jw__content_child_active(const jw_daemon_state *state) {
    return state->child_pid > 0 &&
           state->child_kind != JW_CHILD_NONE &&
           state->child_kind != JW_CHILD_LAUNCHER &&
           state->child_kind != JW_CHILD_MENU;
}

/* The daemon sleeps to its next deadline, at most JW_LOOP_IDLE_MAX_MS. Work
   that is sub-second while it lasts keeps the old 50 ms cadence instead of
   declaring a deadline; this is the complete list, so anything not named here
   tolerates the idle heartbeat (the charger, HDMI, USB/Bluetooth audio and
   storage-health polls, the auto-sleep setting and screen-off time, package
   mutation recovery, the external-pad rescan, the IPC partial-frame timeout,
   suspend-inhibitor liveness). Events wake the loop on their own: IPC, the
   input proxy (power edges, callbacks, swallowed standby input), SIGCHLD, the
   jack and storage/netlink fds, the OSD readiness pipe and external pads. */
#define JW_LOOP_BUSY_MS 50
#define JW_LOOP_IDLE_MAX_MS 1000

static bool jw__loop_busy(const jw_daemon_state *state) {
    /* Shutting down: the shutdown branch waits for every child to exit, so
       each pass has to look for exits whether or not a SIGCHLD was seen. */
    if (state->shutdown_requested) return true;
    /* Game or app: speaker-gate SETUP window (~150 ms), rumble reclaim
       (250 ms), switcher resume and warning retries (100 ms), menu prewarm,
       shader poll (25 ms), menu escape, writer-group absence waits, standalone
       and PICO-8 quit confirmation and the import timeout. */
    if (jw__content_child_active(state)) return true;
    /* No foreground child yet: respawn and pending launch decisions. */
    if (!state->daemon_only && state->child_pid <= 0) return true;
    if (state->child_stop_deadline_ms > 0 || state->child_group_wait_started_ms > 0) return true;
    if (state->pending_menu || state->pending_launch || state->pending_app ||
        state->launch_status_pending) return true;
    if (jw__boot_resume_unconfirmed(state)) return true;
    if (state->post_launch_resume_pending || state->in_game_menu_prewarm_pending ||
        state->advanced_shader_pending || state->retroarch_session.warning_pending) return true;
    if (state->retroarch_audio_reinit_pending || state->rumble_reclaim_ms > 0 ||
        state->retroarch_quit_deadline_ms > 0) return true;
    if (state->menu_pid > 0 || state->menu_escape.pending) return true;
    if (state->game_coordination_pending) return true;
    /* Power held: the 2 s long-press threshold has no event of its own. */
    if (state->power_held) return true;
    /* OSD start-up: readiness is also noticed per pass. */
    if (state->osd_pid > 0 && !state->osd_ready) return true;
    /* Update progress polling, and the update-check and scan threads, which
       finish by setting a flag nothing wakes the loop for. */
    if (state->update_download_job.active || state->update_install_job.active ||
        state->update_check_job.active || state->update_package_quiesce_active) return true;
    if (state->scan_job.thread_started) return true;
    /* Start-up maintenance runs 500 ms after frontend-ready. */
    if (state->startup_maintenance_pending) return true;
    /* An inline input proxy reads the power key and runs its Menu-tap (80 ms)
       and escape timers only from its tick. */
    if (jw_input_proxy_needs_tick_cadence(&state->input_proxy)) return true;
    return false;
}

static int jw__loop_timeout_ms(jw_daemon_state *state) {
    if (jw__loop_busy(state)) {
        return JW_LOOP_BUSY_MS;
    }
    long long now = jw__monotonic_ms();
    long long due = now + JW_LOOP_IDLE_MAX_MS;
    long long at = jw_platform_next_deadline_ms(&state->platform, now);
    if (at >= 0 && at < due) {
        due = at;
    }
    if (state->services) {
        at = jw_svc_supervisor_next_deadline_ms(state->services, now);
        if (at >= 0 && at < due) {
            due = at;
        }
    }
    /* A deadline that is already due gets the old cadence rather than 0: if
       its tick cannot act on it yet, 0 would spin. */
    if (due <= now) {
        return JW_LOOP_BUSY_MS;
    }
    return (int)(due - now);
}

/* Drained at the top of every pass rather than after the poll, because the
   shutdown branch never reaches the poll. */
static void jw__drain_sigchld(jw_daemon_state *state) {
    if (g_sigchld_pipe[0] < 0) {
        return;
    }
    char drain[64];
    while (read(g_sigchld_pipe[0], drain, sizeof(drain)) > 0) {
        state->sigchld_seen = true;
    }
}

static void jw__ipc_tick(jw_daemon_state *state, int timeout_ms) {
    /* Listen socket, connections, then the wake-only fds below. */
    enum { JW_POLL_EXTRA = 2 + 4 + JW_EXT_INPUT_MAX_PADS + 1 };
    struct pollfd poll_fds[1 + JW_DAEMON_IPC_CONNECTION_MAX + JW_POLL_EXTRA];
    int slot_for_poll[1 + JW_DAEMON_IPC_CONNECTION_MAX + JW_POLL_EXTRA];
    const nfds_t poll_max = sizeof(poll_fds) / sizeof(poll_fds[0]);
    nfds_t count = 1;
    memset(poll_fds, 0, sizeof(poll_fds));
    for (nfds_t i = 0; i < poll_max; i++) {
        slot_for_poll[i] = -1;
    }
    poll_fds[0].fd = jw_ipc_server_fd(state->server);
    poll_fds[0].events = POLLIN;
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX; i++) {
        jw_daemon_ipc_connection *connection = &state->ipc_connections[i];
        if (!connection->stream) {
            continue;
        }
        poll_fds[count].fd = jw_ipc_stream_fd(connection->stream);
        poll_fds[count].events = POLLIN;
        if (jw_ipc_stream_wants_write(connection->stream)) {
            poll_fds[count].events |= POLLOUT;
        }
        slot_for_poll[count] = i;
        count++;
    }

    /* The input proxy used to be drained only once per daemon loop, while this
       poll waited up to 50ms on IPC alone. A short press could therefore be
       forwarded as adjacent down/up events and disappear between consumer
       frames. Let physical input wake the existing loop immediately. */
    int input_fd = jw_input_proxy_poll_fd(&state->input_proxy);
    if (input_fd >= 0) {
        poll_fds[count].fd = input_fd;
        poll_fds[count].events = POLLIN;
        count++;
    }

    /* Wake-only fds: the next pass drains them (the SIGCHLD pipe at its top,
       the rest in their ticks), so revents is not looked at here. */
    int wake_fds[JW_POLL_EXTRA];
    int wake_count = 0;
    if (g_sigchld_pipe[0] >= 0) {
        wake_fds[wake_count++] = g_sigchld_pipe[0];
    }
    wake_count += jw_platform_poll_fds(&state->platform, wake_fds + wake_count, 4);
    for (int i = 0; i < JW_EXT_INPUT_MAX_PADS; i++) {
        if (state->external_input.fds[i] >= 0) {
            wake_fds[wake_count++] = state->external_input.fds[i];
        }
    }
    if (!state->osd_ready && state->osd_ready_fd >= 0) {
        wake_fds[wake_count++] = state->osd_ready_fd;
    }
    for (int i = 0; i < wake_count && count < poll_max; i++) {
        poll_fds[count].fd = wake_fds[i];
        poll_fds[count].events = POLLIN;
        count++;
    }

    int ready = poll(poll_fds, count, timeout_ms);
    if (ready < 0 && errno != EINTR) {
        jw_log_warn("ipc poll failed: %s", strerror(errno));
        return;
    }

    if (ready > 0 && (poll_fds[0].revents & POLLIN)) {
        jw__ipc_accept_ready(state);
    }

    long long now = jw__monotonic_ms();
    for (nfds_t p = 1; p < count; p++) {
        int slot = slot_for_poll[p];
        if (slot < 0 || !state->ipc_connections[slot].stream) {
            continue;
        }
        short revents = poll_fds[p].revents;

        /* Calling receive on every tick, even without POLLIN, enforces the
         * fixed partial-frame deadline without giving idle subscribers a
         * deadline of their own. Reads remain non-blocking. */
        for (int frames = 0; frames < 8 &&
             state->ipc_connections[slot].stream; frames++) {
            char *body = NULL;
            size_t len = 0;
            int rc = jw_ipc_stream_receive(
                state->ipc_connections[slot].stream, now, &body, &len);
            if (rc == 1) {
                jw__ipc_handle_frame(state, slot, body, len);
                free(body);
                continue;
            }
            free(body);
            if (rc == 0) {
                break;
            }
            jw__ipc_connection_drop(
                state, slot,
                rc == -3 ? "partial-frame-timeout" :
                rc == -2 ? "invalid-frame" : "peer-closed");
            break;
        }
        if (!state->ipc_connections[slot].stream) {
            continue;
        }
        if (jw_ipc_stream_wants_write(
                state->ipc_connections[slot].stream) &&
            jw_ipc_stream_flush(state->ipc_connections[slot].stream) != 0) {
            jw__ipc_connection_drop(state, slot, "write-failed");
            continue;
        }
        if ((revents & (POLLERR | POLLNVAL)) != 0 ||
            ((revents & POLLHUP) != 0 &&
             !jw_ipc_stream_wants_write(
                 state->ipc_connections[slot].stream))) {
            jw__ipc_connection_drop(state, slot, "peer-hangup");
            continue;
        }
        if (state->ipc_connections[slot].close_after_flush &&
            !jw_ipc_stream_wants_write(
                state->ipc_connections[slot].stream)) {
            jw__ipc_connection_drop(state, slot, "protocol-rejected");
            continue;
        }
        if (state->ipc_connections[slot].stop_after_flush &&
            !jw_ipc_stream_wants_write(
                state->ipc_connections[slot].stream)) {
            char service_id[JW_SVC_SUPERVISOR_ID_BUF];
            snprintf(service_id, sizeof(service_id), "%s",
                     state->ipc_connections[slot].service_id);
            state->ipc_connections[slot].stop_after_flush = false;
            if (!jw__game_stop_service(state, service_id, NULL,
                                       "active-mode-stop-reconcile")) {
                jw_log_warn("life1: active mode-stop reconnect remains unverified service=%s",
                            service_id);
            }
            jw__ipc_connection_drop(state, slot,
                                    "active-mode-stop-reconcile");
        }
    }
}

static void jw__clear_menu_tracking(jw_daemon_state *state) {
    if (!state) {
        return;
    }

    state->menu_pid = -1;
    state->menu_in_game = false;
    state->menu_visible = false;
    state->retroarch_resume_on_menu_exit = false;
}

static void jw__terminate_menu_child(jw_daemon_state *state, bool force) {
    if (!state || state->menu_pid <= 0) {
        return;
    }

    pid_t pid = state->menu_pid;
    kill(pid, SIGTERM);

    int status = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            jw_log_info("in-game menu exited during cleanup status=%d", status);
            jw__clear_menu_tracking(state);
            return;
        }
        if (waited < 0) {
            if (errno != ECHILD) {
                jw_log_warn("in-game menu cleanup wait failed: %s",
                            strerror(errno));
            }
            jw__clear_menu_tracking(state);
            return;
        }
        jw__usleep(50000);
    }

    if (force) {
        jw_log_warn("in-game menu did not exit on SIGTERM; forcing pid=%d",
                    (int)pid);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        jw__clear_menu_tracking(state);
    }
}

/* Run only after the native Apps process-group barrier. The importer owns no
 * display or input and cannot overlap a game or package mutation. */
static bool jw__spawn_pico8_import(jw_daemon_state *state) {
    jw_storage_source_list sources;
    jw_pico8_paths paths;
    if (jw__storage_sources(state, &sources) != 0 ||
        !jw_pico8_resolve_paths(&sources, NULL, &paths)) return false;
    char favourites[PATH_MAX], journal[PATH_MAX];
    if (snprintf(favourites, sizeof(favourites), "%s/favourites.txt", paths.home) >= (int)sizeof(favourites) ||
        snprintf(journal, sizeof(journal), "%s/splore-imports.sqlite3", paths.home) >= (int)sizeof(journal)) return false;
    /* First-time setup failures have no favourites or import state to process. */
    if (access(favourites, F_OK) != 0 && access(journal, F_OK) != 0) return false;
    const jw_storage_source *primary = jw_storage_sources_primary(&sources);
    char helper[PATH_MAX];
    int n = snprintf(helper, sizeof(helper), "%s/%s/bin/pico8-import", primary->apps_path, JW_PICO8_PROVIDER);
    if (n < 0 || n >= (int)sizeof(helper) || access(helper, X_OK) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) { jw_log_warn("PICO-8 importer could not start"); return false; }
    if (pid == 0) {
        if (setpgid(0, 0) != 0) _exit(126);
        jw_pico8_export(&paths, false);
        execl(helper, helper, (char *)NULL);
        _exit(127);
    }
    (void)setpgid(pid, pid);
    state->child_pid = pid;
    state->child_pgid = getpgid(pid) == pid ? pid : -1;
    state->child_kind = JW_CHILD_PICO8_IMPORT;
    state->child_stop_deadline_ms = jw__monotonic_ms() + 30000;
    state->child_stop_signalled = false;
    jw_log_info("PICO-8 import started pid=%d", (int)pid);
    (void)jw__osd_game_launch(state, "pico8-import", 0);
    return true;
}

static void jw__handle_child_exit(jw_daemon_state *state) {
    if (!state || state->child_pid <= 0) {
        return;
    }

    int status = 0;
    pid_t waited = -1;
    /* The barrier is only meaningful with a pgid we actually reserved. Without
     * one, jw_svc_group_absent() would be asked about a group that never
     * existed and would answer "absent" -- a proof of nothing. Supervise such a
     * runner like a generic app instead of pretending the barrier holds. */
    bool group_barrier = jw__child_kind_has_group_barrier(state->child_kind) &&
                         (jw__child_kind_has_writer_barrier(state->child_kind) ||
                          state->child_pgid > 0);
    if (group_barrier) {
        /* Observe without reaping. The zombie leader pins the pgid while any
         * descendant still exists, preventing group-id reuse from invalidating
         * the absence proof. Only after the whole group is non-writer/zombie
         * may we reap the leader and run source-specific finalizers. */
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        int rc;
        do {
            rc = waitid(P_PID, (id_t)state->child_pid, &info,
                        WEXITED | WNOHANG | WNOWAIT);
        } while (rc != 0 && errno == EINTR);
        if (rc != 0 || info.si_pid != state->child_pid) {
            return;
        }
        pid_t pgid = state->child_pgid > 0 ? state->child_pgid
                                           : state->child_pid;
        if (!jw_svc_group_absent(pgid)) {
            if (jw__child_kind_has_writer_barrier(state->child_kind)) {
                return;
            }
            /* RetroArch app tile: the runner is gone but RetroArch is not.
             * There is no play session to keep fail-safe here, and refusing to
             * reap forever would leave the device on a dead screen with no
             * launcher, so bound the wait and then kill the group. */
            long long now = jw__monotonic_ms();
            if (state->child_group_wait_started_ms == 0) {
                state->child_group_wait_started_ms = now;
                jw_log_warn("RetroArch app runner pid=%d exited with pgid=%d "
                            "still present; waiting for it to clear",
                            (int)state->child_pid, (int)pgid);
                return;
            }
            if (now - state->child_group_wait_started_ms <
                JW_RA_APP_GROUP_WAIT_MS) {
                return;
            }
            if (state->child_group_kill_logged_ms == 0) {
                jw_log_error("RetroArch app pgid=%d still present after %d ms; "
                             "killing the group", (int)pgid,
                             JW_RA_APP_GROUP_WAIT_MS);
            }
            (void)jw__signal_tracked_game_group(state, SIGKILL);
            /* Prove the kill landed before forgetting the pgid. Having *sent*
             * SIGKILL is not an absence proof, and once the leader is reaped
             * the group id can be reused, so a survivor becomes untrackable. */
            bool absent = false;
            for (int i = 0; i < 25 && !absent; i++) {
                absent = jw_svc_group_absent(pgid);
                if (!absent) {
                    jw__usleep(20000);
                }
            }
            bool shutting_down =
                state->shutdown_requested || g_shutdown_requested;
            if (!jw_svc_group_tracking_may_end(absent, shutting_down)) {
                /* Same answer as the writer barrier, for the same reason.
                 * Reaping here and forgetting the pgid would leave a RetroArch
                 * that outlived SIGKILL still holding the display and writing
                 * to the card, with a fresh launcher spawned on top of it and
                 * nothing tracking either -- worse than a screen that stays
                 * blank. A process that survives SIGKILL is in uninterruptible
                 * sleep, which clears on its own, and each pass retries the
                 * kill, so the device recovers by itself once it does. */
                if (now - state->child_group_kill_logged_ms >
                        JW_RA_APP_GROUP_KILL_LOG_MS ||
                    state->child_group_kill_logged_ms == 0) {
                    state->child_group_kill_logged_ms = now;
                    jw_log_error("RetroArch app pgid=%d survived SIGKILL; "
                                 "holding the runner unreaped until the group "
                                 "is absent", (int)pgid);
                }
                return;
            }
            if (!absent) {
                /* Shutting down. Holding buys nothing here -- the tracking dies
                 * with this process either way -- and the main loop only exits
                 * once no child is left, so holding would hang the poweroff
                 * instead. Reap so the daemon can finish, and leave the
                 * survivor to the next boot's stale-launcher sweep. */
                jw_log_error("RetroArch app pgid=%d survived SIGKILL during "
                             "shutdown; leaving it for the next generation to "
                             "clear", (int)pgid);
            } else {
                jw_log_info("RetroArch app pgid=%d confirmed absent after "
                            "SIGKILL", (int)pgid);
            }
        }
        do {
            waited = waitpid(state->child_pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
    } else {
        waited = waitpid(state->child_pid, &status, WNOHANG);
    }
    if (waited == 0 || waited < 0) {
        return;
    }

    jw_child_kind exited_kind = state->child_kind;
    pid_t exited_pid = waited;
    if (state->pico8_exit_confirm_until_ms) {
        jw__osd_game_launch_hide(state);
        state->pico8_exit_confirm_until_ms = 0;
    }
    state->pico8_quit_request_ms = 0;
    jw__set_session_menu(state, false);
    state->child_pid = -1;
    state->child_pgid = -1;
    state->child_kind = JW_CHILD_NONE;
    state->child_stop_signalled = false;
    state->child_stop_deadline_ms = 0;
    state->child_group_wait_started_ms = 0;
    state->child_group_kill_logged_ms = 0;

    /* Remove the per-launch private input view once the child is reaped. */
    if (state->child_input_ns_dir[0]) {
        jw_input_namespace_cleanup_dir(state->child_input_ns_dir);
        jw_log_info("input namespace: removed %s", state->child_input_ns_dir);
        state->child_input_ns_dir[0] = '\0';
    }

    /* Reclaim the motor: a game that died mid-buzz can't have cleared it. Close
       the force-feedback route with it, so a stale effect id left behind by the
       dead session can't drive the motor for the next one. */
    jw__rumble_publish_ff(NULL);
    jw__rumble_quiesce();

    const char *name = jw__child_name(exited_kind);
    if (WIFEXITED(status)) {
        jw_log_info("%s exited status=%d", name ? name : "child", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("%s terminated signal=%d", name ? name : "child", WTERMSIG(status));
    } else {
        jw_log_warn("%s changed state status=%d", name ? name : "child", status);
    }

    if (exited_kind == JW_CHILD_RETROARCH_APP && WIFEXITED(status) &&
        WEXITSTATUS(status) == JW_RA_APP_EXIT_SAVE_FAILED) {
        jw_log_error("RetroArch app pid=%d exited without saving its settings "
                     "to the shared config", (int)exited_pid);
        jw__osd_settings_not_saved(state);
    }

    if (exited_kind == JW_CHILD_RETROARCH) {
        if (state->power_hold_save_quit_sent_ms > 0 &&
            !state->power_hold_save_quit_expired) {
            jw_log_info("power-hold save: quit grace: RetroArch exited after %lld ms",
                        jw__monotonic_ms() - state->power_hold_save_quit_sent_ms);
        }
        jw__retroarch_session_finish(state, exited_pid, status);
        if (state->menu_pid > 0) {
            jw__terminate_menu_child(state, true);
        }
    }
    if (exited_kind == JW_CHILD_EMULATOR) {
        /* Inspect the switcher-request marker while the session is still intact,
           so it can be validated against the just-exited game. */
        jw__consume_standalone_switcher_marker(state, exited_pid);
        jw__standalone_session_finish(state, exited_pid, status);
        if (state->direct_drm_active) {
            state->direct_drm_active = false;
            if (state->direct_drm_weston_stopped) {
                jw_log_info("direct DRM handoff ended; restarting Weston");
                /* From / and with the rootfs environment, not jawakad's
                   card working directory and launcher LD_LIBRARY_PATH:
                   Weston outlives this session, and a compositor sitting in
                   or mapping libraries from a launcher bundle that an update
                   has since replaced keeps the card from closing at
                   shutdown. See weston_initd.h. */
                (void)jw_weston_initd_run(JW_WESTON_INITD("start") " </dev/null >/dev/null 2>&1");
                jw__usleep(1000000UL);
                state->direct_drm_weston_stopped = false;
            }
            if (!state->shutdown_requested && !g_shutdown_requested) {
                jw__spawn_osd(state);
            }
        }
    }
    if (jw__child_kind_has_writer_barrier(exited_kind)) {
        /* Process-group absence was proved before this function reaped the
         * leader; the per-source session finalizer above has now completed as
         * well. This is the authoritative LIFE-1 writer-exit barrier. */
        jw__active_game_finish(state);
    }

    if (state->shutdown_requested || g_shutdown_requested) {
        return;
    }

    if (jw__child_kind_is_app(exited_kind) || exited_kind == JW_CHILD_EMULATOR) {
        /* A standalone session leaves the watch-only proxy active; tear it
           down so the full grab-and-forward proxy can come back. */
        jw_input_proxy_shutdown(&state->input_proxy);
        jw__start_input_proxy(state);
    }
    if (exited_kind == JW_CHILD_PICO8_APP) {
        (void)jw__perf_apply_frontend(state, "pico8-app-exit");
        if (group_barrier && jw__spawn_pico8_import(state)) return;
    }
    if (exited_kind == JW_CHILD_PICO8_IMPORT) {
        jw__osd_game_launch_hide(state);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            jw_log_warn("Some Splore favourites could not be imported; see PICO-8 import errors above. Next exit will retry.");
            (void)jw__osd_game_launch(state, "pico8-import-failed", 0);
        }
        if (jw__start_scan_job(state, "after Splore import") < 0)
            jw_log_warn("PICO-8 import scan could not start; next scan will retry");
    }

    /* Switch-game: the old RetroArch quit with a game already queued, so spawn
     * the selected game directly instead of returning to the launcher. The
     * session finish above already recorded the old game's playtime/recents. */
    if (exited_kind == JW_CHILD_RETROARCH && state->pending_launch) {
        if (jw__spawn_pending_game(state) != 0) {
            jw_log_warn("switch-game: next game spawn failed; returning to launcher");
            if (!state->daemon_only) {
                jw__spawn_child(state, JW_CHILD_LAUNCHER);
            }
        }
        return;
    }

    /* Spawn-on-exit model: the launcher sends a pending action, then exits
     * voluntarily. The daemon detects the exit here and owns the next process. */
    if (exited_kind == JW_CHILD_LAUNCHER && state->pending_launch) {
        if (state->game_coordination_ready) {
            state->game_coordination_ready = false;
            if (jw__spawn_authorized_pending_game(state) != 0) {
                jw__game_coordination_abort(state,
                                            "deferred-writer-spawn-failed");
                if (!state->daemon_only) {
                    jw__spawn_child(state, JW_CHILD_LAUNCHER);
                }
            } else {
                state->pending_launch_override_unverified = false;
                state->active_game_writer_started = true;
                jw__osd_game_launch_hide(state);
                jw_log_info("life1: writer started launch_id=%s pgid=%d "
                            "total_ms=%lld",
                            state->active_game.launch_id,
                            (int)state->child_pgid,
                            state->game_launch_started_ms > 0
                                ? jw__monotonic_ms() -
                                      state->game_launch_started_ms
                                : 0);
                state->game_launch_started_ms = 0;
            }
            return;
        }
        if (state->game_coordination_pending) {
            return;
        }
        if (jw__spawn_pending_game(state) != 0 && !state->daemon_only) {
            jw__spawn_child(state, JW_CHILD_LAUNCHER);
        }
        return;
    }

    if (exited_kind == JW_CHILD_LAUNCHER && state->pending_app) {
        if (jw__spawn_app(state) != 0 && !state->daemon_only) {
            jw__spawn_child(state, JW_CHILD_LAUNCHER);
        }
        return;
    }

    if (exited_kind == JW_CHILD_LAUNCHER && state->pending_menu) {
        state->pending_menu = false;
        jw__spawn_child(state, JW_CHILD_MENU);
        return;
    }

    if (state->daemon_only) {
        return;
    }

    if (exited_kind == JW_CHILD_MENU) {
        jw__spawn_child(state, JW_CHILD_LAUNCHER);
        return;
    }

    if (exited_kind == JW_CHILD_RETROARCH || exited_kind == JW_CHILD_EMULATOR) {
        /* A standalone launch that died before the emulator appeared must not
           come back as a silent black flash: hand the respawned launcher the
           name and status the same way JAWAKA_OPEN_SWITCHER is handed over
           (execv keeps environ, the launcher reads it on its first frame). */
        if (exited_kind == JW_CHILD_EMULATOR && state->launch_status_pending) {
            char code[16];
            snprintf(code, sizeof(code), "%d", state->launch_status_code);
            setenv("JAWAKA_LAUNCH_STATUS_NAME", state->launch_status_name, 1);
            setenv("JAWAKA_LAUNCH_STATUS_CODE", code, 1);
            state->launch_status_pending = false;
        }
        if (state->launcher_open_switcher) {
            /* A standalone Menu+Select asked us to reopen the launcher straight
               into the switcher, seeded on the just-exited game. The launcher
               inherits these env vars (execv keeps environ) and acts on them on
               its first frame; clear them again so later restarts start normally. */
            setenv("JAWAKA_OPEN_SWITCHER", "1", 1);
            setenv("JAWAKA_SWITCHER_SELECT_SYSTEM", state->launcher_switcher_system, 1);
            setenv("JAWAKA_SWITCHER_SELECT_ROM", state->launcher_switcher_rom, 1);
            state->launcher_open_switcher = false;
            jw__spawn_child(state, JW_CHILD_LAUNCHER);
            unsetenv("JAWAKA_OPEN_SWITCHER");
            unsetenv("JAWAKA_SWITCHER_SELECT_SYSTEM");
            unsetenv("JAWAKA_SWITCHER_SELECT_ROM");
        } else {
            jw__spawn_child(state, JW_CHILD_LAUNCHER);
        }
        unsetenv("JAWAKA_LAUNCH_STATUS_NAME");
        unsetenv("JAWAKA_LAUNCH_STATUS_CODE");
        return;
    }

    if (jw__child_kind_is_app(exited_kind)) {
        jw__spawn_child(state, JW_CHILD_LAUNCHER);
        return;
    }

    jw__spawn_child(state, JW_CHILD_LAUNCHER);
}

static void jw__handle_menu_exit(jw_daemon_state *state) {
    if (!state || state->menu_pid <= 0) {
        return;
    }

    int status = 0;
    pid_t waited = waitpid(state->menu_pid, &status, WNOHANG);
    if (waited == 0) {
        return;
    }
    if (waited < 0) {
        if (errno != ECHILD) {
            jw_log_warn("in-game menu wait failed: %s", strerror(errno));
        }
        jw__clear_menu_tracking(state);
        return;
    }

    /* Reaching here means the standby exited on its own — i.e. it crashed,
       because intentional teardown at game exit clears menu_pid in
       jw__handle_child_exit before this runs. */
    bool was_visible = state->menu_visible;
    bool session_active = state->retroarch_session.active;
    bool shutting = state->shutdown_requested || g_shutdown_requested;
    jw__clear_menu_tracking(state);

    if (WIFEXITED(status)) {
        jw_log_info("in-game menu exited status=%d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        jw_log_warn("in-game menu terminated signal=%d", WTERMSIG(status));
    } else {
        jw_log_warn("in-game menu changed state status=%d", status);
    }

    if (!session_active || shutting) {
        return;
    }

    /* The game is still running. If the menu died while shown, RetroArch is
       paused under it — resume so the player isn't stuck on a frozen frame. */
    if (was_visible) {
        jw_ra_client client = jw_ra_client_default();
        jw_ra_result result = jw_ra_resume_direct(&client);
        if (result != JW_RA_OK) {
            jw_log_warn("in-game menu crash: resume failed result=%s",
                        jw_ra_result_string(result));
        }
    }

    /* Re-arm a hidden standby for the next Menu tap, with a per-session cap so
       a menu that crashes on startup can't spin. Past the cap we leave menu_pid
       clear and let the next tap take the on-demand fallback path. */
    if (state->menu_standby_attempts < 3) {
        state->menu_standby_attempts++;
        if (jw__spawn_in_game_menu(state, false) != 0) {
            jw_log_warn("standby in-game menu respawn failed (attempt %d/3)",
                        state->menu_standby_attempts);
        } else {
            jw_log_info("standby in-game menu respawned (attempt %d/3)",
                        state->menu_standby_attempts);
        }
    } else {
        jw_log_warn("standby in-game menu respawn cap reached; on-demand fallback only");
    }
}

static void jw__cleanup(jw_daemon_state *state) {
    if (!state) {
        return;
    }
    jw_delete_job_destroy(state->delete_job);
    state->delete_job = NULL;

    jw__terminate_menu_child(state, true);

    if (state->child_pid > 0) {
        pid_t child_pid = state->child_pid;
        jw_child_kind child_kind = state->child_kind;
        if (jw__child_kind_has_stop_grace(child_kind)) {
            /* The RetroArch app runner answers SIGTERM by asking RetroArch to
             * quit over the command port, waiting for it to save, and copying
             * the config back. Killing it after 50 ms -- what the generic app
             * path below does -- is exactly how a session's settings were
             * being lost. Wait for the runner's own budget, then escalate to
             * the whole group so a wedged RetroArch cannot block a poweroff. */
            state->shutdown_requested = true;
            kill(child_pid, SIGTERM);
            long long deadline = jw__monotonic_ms() + JW_RA_APP_STOP_GRACE_MS;
            while (state->child_pid == child_pid &&
                   jw__monotonic_ms() < deadline) {
                jw__handle_child_exit(state);
                if (state->child_pid == child_pid) {
                    jw__usleep(20000);
                }
            }
            if (state->child_pid == child_pid) {
                jw_log_warn("cleanup: RetroArch app pid=%d did not stop within "
                            "%d ms; killing pgid=%d",
                            (int)child_pid, JW_RA_APP_STOP_GRACE_MS,
                            (int)state->child_pgid);
                (void)jw__signal_tracked_game_group(state, SIGKILL);
                deadline = jw__monotonic_ms() + 2000;
                while (state->child_pid == child_pid &&
                       jw__monotonic_ms() < deadline) {
                    jw__handle_child_exit(state);
                    if (state->child_pid == child_pid) {
                        jw__usleep(20000);
                    }
                }
            }
            if (state->child_pid == child_pid) {
                /* Do not reap the leader and fabricate absence, for the same
                 * reason the writer path does not: this daemon is exiting, and
                 * a RetroArch that outlived SIGKILL is still holding the
                 * display and the card. Leave it visible to the next
                 * generation's stale-launcher sweep rather than recording a
                 * clean stop that did not happen. */
                jw_log_error("cleanup: RetroArch app pgid=%d did not become "
                             "absent; leaving it for the next generation to "
                             "clear", (int)state->child_pgid);
            }
        } else if (jw__child_kind_has_writer_barrier(child_kind)) {
            state->shutdown_requested = true;
            (void)jw__signal_tracked_game_group(state, SIGTERM);
            long long deadline = jw__monotonic_ms() + 2000;
            while (state->child_pid == child_pid &&
                   jw__monotonic_ms() < deadline) {
                jw__handle_child_exit(state);
                if (state->child_pid == child_pid) {
                    jw__usleep(20000);
                }
            }
            if (state->child_pid == child_pid) {
                (void)jw__signal_tracked_game_group(state, SIGKILL);
                deadline = jw__monotonic_ms() + 2000;
                while (state->child_pid == child_pid &&
                       jw__monotonic_ms() < deadline) {
                    jw__handle_child_exit(state);
                    if (state->child_pid == child_pid) {
                        jw__usleep(20000);
                    }
                }
            }
            if (state->child_pid == child_pid) {
                /* Do not reap the leader and fabricate absence. The runtime
                 * active-game record intentionally survives this process so
                 * the next daemon generation stays fail-safe. */
                jw_log_error("cleanup: writer group pgid=%d did not become absent; preserving active launch record",
                             (int)state->child_pgid);
            }
        } else {
            int status = 0;
            kill(child_pid, SIGTERM);
            (void)waitpid(child_pid, &status, 0);
            state->child_pid = -1;
            state->child_pgid = -1;
            state->child_kind = JW_CHILD_NONE;
        }
    }

    if (state->osd_pid > 0) {
        kill(state->osd_pid, SIGTERM);
        waitpid(state->osd_pid, NULL, 0);
        state->osd_pid = -1;
    }

    jw__stop_ledd(state);

    jw_external_input_monitor_shutdown(&state->external_input);
    jw_input_proxy_shutdown(&state->input_proxy);
    if (state->child_input_ns_dir[0]) {
        jw_input_namespace_cleanup_dir(state->child_input_ns_dir);
        state->child_input_ns_dir[0] = '\0';
    }
    if (state->update_download_job.active) {
        jw_update_download_cancel(&state->update_status, &state->update_download_job);
    }
    jw_update_check_job_wait(&state->update_check_job);
    jw_scrape_worker_stop();
    jw__scan_job_shutdown(state);
    int inhibitor_count = jw_suspend_inhibitor_count(&state->suspend_inhibitor);
    if (inhibitor_count > 0) {
        jw_log_info("suspend-inhibit: releasing %d lease(s) on daemon shutdown",
                    inhibitor_count);
    }
    jw_suspend_inhibitor_clear(&state->suspend_inhibitor);
    jw_suspend_policy_init(&state->suspend_policy);
    for (int i = 0; i < JW_DAEMON_IPC_CONNECTION_MAX; i++) {
        jw__ipc_connection_drop(state, i, "daemon-shutdown");
    }
    if (state->services) {
        jw_svc_supervisor_close(state->services);
        state->services = NULL;
    }
    jw_pakrat_mutation_lock_release(&state->mutation_recovery_lock);
    jw_ipc_server_close(state->server);
    jw_db_close(state->db);
    /* MLP1 queued its rootfs handoff before shutdown began. */
#ifndef PLATFORM_MLP1
    if (state->power_transition_requested) {
        jw_platform_result result;
        jw_platform_perform_action(&state->platform,
                                   state->power_transition_action, 0, &result);
        if (result.code != JW_PLATFORM_RESULT_OK) {
            jw_log_error("deferred %s failed: %s",
                         jw_platform_action_name(state->power_transition_action),
                         result.message[0] ? result.message
                                           : jw_platform_result_code_name(result.code));
        }
    }
#endif
    jw_platform_shutdown(&state->platform);
    free(state->runtime_dir);
    free(state->sdcard_root);
    free(state->socket_path);
    free(state->osd_socket_path);
    free(state->db_path);
    free(state->state_dir);
}

#ifdef PLATFORM_MLP1
/* /proc/<pid>/stat field 22: start time in clock ticks. With the pid it
   identifies one process, so a recycled pid never reads as the game. */
static unsigned long long jw__proc_start_ticks(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    /* Fields after the command name start at field 3 (state). */
    int field = 2;
    for (p++; *p && field < 22; p++) {
        if (*p == ' ') field++;
    }
    return field == 22 ? strtoull(p, NULL, 10) : 0;
}

/* SVC-1: shutdown could not prove every writer gone. Tell the rootfs
   supervisor what to wait for instead of reporting completion. */
static int jw__publish_power_incomplete(int stuck_services, const char *stuck_record,
                                        pid_t game_pid) {
    char record[2600];
    int n = snprintf(record, sizeof(record), "reason=%s\n%s",
                     stuck_services > 0 ? "unverified-service" : "game-child",
                     stuck_record ? stuck_record : "");
    if (n < 0 || (size_t)n >= sizeof(record)) return -1;
    if (game_pid > 0) {
        int m = snprintf(record + n, sizeof(record) - (size_t)n,
                         "game pid=%d start=%llu\n", (int)game_pid,
                         jw__proc_start_ticks(game_pid));
        if (m < 0 || (size_t)m >= sizeof(record) - (size_t)n) return -1;
    }
    return jw_power_request_incomplete(record);
}
#endif

/* The boot this daemon runs in, read once: a power-hold save records it with
   a resume, and a record from this same boot is never acted on. "" when it
   cannot be read. Host builds take JAWAKA_TEST_BOOT_ID so a smoke can play a
   second boot without one. */
static void jw__read_boot_id(char *out, size_t out_size) {
    out[0] = '\0';
#ifndef PLATFORM_MLP1
    const char *test = getenv("JAWAKA_TEST_BOOT_ID");
    if (test && test[0]) {
        snprintf(out, out_size, "%s", test);
        jw_log_warn("boot resume: TEST override: boot id %s", out);
        return;
    }
#endif
    FILE *fp = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!fp) {
        return;
    }
    if (fgets(out, (int)out_size, fp)) {
        out[strcspn(out, "\r\n")] = '\0';
    } else {
        out[0] = '\0';
    }
    fclose(fp);
}

int main(int argc, char *argv[]) {
    signal(SIGINT, jw__handle_signal);
    signal(SIGTERM, jw__handle_signal);
    signal(SIGPIPE, SIG_IGN);

    jw_daemon_state state;
    memset(&state, 0, sizeof(state));
    state.reap_by_signal = jw__install_sigchld_pipe();
    if (!state.reap_by_signal) {
        jw_log_warn("SIGCHLD pipe unavailable (%s); checking children every pass",
                    strerror(errno));
    }
    /* A zeroed snapshot would read as "everything disabled"; the defaults are
       the fixed chords this replaces, so a daemon that never reaches the
       settings load still behaves as it always did. */
    jw_input_shortcuts_defaults(&state.shortcuts);
    state.mutation_recovery_lock.fd = -1;
    state.child_pid = -1;
    state.child_pgid = -1;
    state.child_stop_signalled = false;
    state.child_stop_deadline_ms = 0;
    state.child_group_wait_started_ms = 0;
    state.child_group_kill_logged_ms = 0;
    state.menu_pid = -1;
    state.osd_pid = -1;
    state.osd_ready_fd = -1;
    state.cached_brightness_percent = -1;
    state.cached_volume_percent = -1;
    state.ledd_pid = -1;
    state.led_configured = false;
    state.autosleep_platform_synced_s = -1;
    state.charging_cached = -1;
    state.perf_global_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    state.perf_active_profile = JW_PLATFORM_PERF_PROFILE_FRONTEND;
    state.perf_session_profile = JW_PLATFORM_PERF_PROFILE_AUTO;
    jw__perf_request_init(&state.perf_custom_request);
    jw_suspend_inhibitor_init(&state.suspend_inhibitor);
    jw_suspend_policy_init(&state.suspend_policy);
    jw_power_hold_save_init(&state.power_hold_save);
    jw_ra_sync_save_init(&state.power_hold_save_request);
    jw_ra_sync_save_init(&state.boot_resume.load);
#ifndef PLATFORM_MLP1
    /* Test hooks for scripts/power-hold-save-ipc-smoke.sh; host builds only. */
    {
        const char *cut = getenv("JAWAKA_TEST_POWER_HARD_CUT_S");
        if (cut && cut[0]) {
            state.test_power_hard_cut_s = atoi(cut);
            jw_log_warn("power-hold save: TEST override: Force Off Hold reads as %d s",
                        state.test_power_hard_cut_s);
        }
        const char *any = getenv("JAWAKA_TEST_POWER_HOLD_SAVE_ANY_PLATFORM");
        if (any && strcmp(any, "1") == 0) {
            state.test_power_hold_save_any_platform = true;
            jw_log_warn("power-hold save: TEST override: platform gate open on %s",
                        jw_platform_compiled_id());
        }
        /* scripts/boot-resume-ipc-smoke.sh plays two boots on one host and
           needs a card identity the mock platform cannot read. */
        const char *fingerprint = getenv("JAWAKA_TEST_SOURCE_FINGERPRINT");
        if (fingerprint && fingerprint[0]) {
            snprintf(state.test_source_fingerprint, sizeof(state.test_source_fingerprint),
                     "%s", fingerprint);
            jw_log_warn("boot resume: TEST override: primary card reads as %s",
                        state.test_source_fingerprint);
        }
    }
#endif
    jw__read_boot_id(state.boot_id, sizeof(state.boot_id));
    jw_update_download_job_init(&state.update_download_job);
    jw_update_install_job_init(&state.update_install_job);
    jw_update_check_job_init(&state.update_check_job);
    jw__scan_job_init(&state);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon-only") == 0) {
            state.daemon_only = true;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            jw__print_usage(stdout);
            return 0;
        }

        jw__print_usage(stderr);
        jw_log_error("unknown argument: %s", argv[i]);
        return 2;
    }

    if (jw__set_bin_dir(argv[0], state.bin_dir, sizeof(state.bin_dir)) != 0) {
        jw_log_error("could not resolve binary directory");
        return 1;
    }

    state.runtime_dir = jw_runtime_dir();
    state.sdcard_root = jw_sdcard_root();
    state.socket_path = jw_socket_path();
    state.osd_socket_path = jw_osd_socket_path();
    state.db_path = jw_db_path();
    state.state_dir = jw_state_dir();
    if (!state.runtime_dir || !state.sdcard_root || !state.socket_path ||
        !state.osd_socket_path || !state.db_path || !state.state_dir) {
        jw_log_error("could not resolve runtime paths");
        jw__cleanup(&state);
        return 1;
    }

    /* Paired-wireless-controllers contract: sweep stale per-launch input
       views left by an unclean shutdown, and never inherit an SDL device list
       from a wrapper -- the roster is rebuilt fresh for every launch. */
    jw_input_namespace_startup_sweep();
    unsetenv("SDL_JOYSTICK_DEVICE");
    unsetenv("SDL_JOYSTICK_DISABLE_UDEV");
    unsetenv("SDL_JOYSTICK_HIDAPI");
    unsetenv("JAWAKA_INPUT_ROSTER_COUNT");

    /* The RA account snapshot is per-launch child state, resolved fresh from
       the DB for each authorized standalone launch. Never inherit it: with
       the daemon's own environment cleared here, no app, helper or emulator
       can receive a stale snapshot through ordinary inheritance. The
       RetroArch channel is per-launch too: set only around the config write
       and cleared right after, so an inherited value is always stale, and
       supervised services inherit this environment as-is. */
    jw__ra_account_clear_env();
    jw__cheevos_clear_env();
    jw__flycast_route_clear_env();

    if (jw_platform_init(&state.platform, state.runtime_dir, state.sdcard_root) != 0) {
        jw_log_error("could not initialize platform service");
        jw__cleanup(&state);
        return 1;
    }
    jw_update_status_init(&state.update_status, state.platform.platform_id,
                          state.state_dir);

    if (!jw__path_exists(state.sdcard_root)) {
        jw_log_error("sdcard root missing: %s (run 'make mockgen')", state.sdcard_root);
        jw__cleanup(&state);
        return 2;
    }

    /* Storage health before the DB: a launcher card that is already read-only
       must still reach the warning and repair UI, without migrations, discovery
       writes or a restart loop. */
    jw__storage_health_refresh(&state);
    {
        char reason[JW_STORAGE_REASON_MAX];
        state.storage_db_read_only =
            !jw_storage_path_writable(state.db_path, reason, sizeof(reason));
        if (state.storage_db_read_only) {
            jw_log_warn("storage: library database is not writable (%s); starting read-only",
                        reason);
        }
    }
    bool db_opened = jw_db_open(state.db_path, &state.db) == 0;
    if (db_opened && jw_db_apply_schema(state.db) != 0) {
        if (!state.storage_db_read_only) {
            jw_log_error("could not open or initialize sqlite database: %s", state.db_path);
            jw__cleanup(&state);
            return 1;
        }
        jw_log_warn("storage: library schema could not be checked on a read-only card; "
                    "continuing without migrations");
    } else if (!db_opened) {
        if (!state.storage_db_read_only) {
            jw_log_error("could not open or initialize sqlite database: %s", state.db_path);
            jw__cleanup(&state);
            return 1;
        }
        if (state.db) {
            jw_db_close(state.db);
            state.db = NULL;
        }
        if (jw_db_open(":memory:", &state.db) != 0 || jw_db_apply_schema(state.db) != 0) {
            jw_log_error("could not open a fallback library database");
            jw__cleanup(&state);
            return 1;
        }
        jw_log_warn("storage: library database unreadable on a read-only card; "
                    "showing an empty library");
    }
    jw__load_library_generation(&state);
    jw__perf_load_global(&state);
    (void)jw__perf_apply_frontend(&state, "startup");
    jw__apply_persisted_brightness(&state);
    jw__apply_persisted_volume(&state);
    (void)jw__apply_color_temp(&state, false);
    jw__apply_persisted_led(&state);

    /* 5-Game Mode boot check: decide whether to enter the locked focus screen
       (active + SD recovery lock file present) or boot normally. A missing lock
       file on an active device is the documented "delete the file to unlock"
       recovery. jw_focus_resolve_boot performs that clear; we note the pre-state
       only to log which path was taken. Phase 2 consumes state.focus_boot to
       render the focus screen; for now this just resolves + logs the state. */
    {
        jw_focus_config pre;
        jw_focus_config_load(state.db_path, &pre);
        bool lock_present = jw_focus_lock_exists(state.sdcard_root);
        state.focus_boot = jw_focus_resolve_boot(state.db_path, state.sdcard_root,
                                                 &state.focus_cfg);
        if (state.focus_boot == JW_FOCUS_BOOT_ENTER) {
            jw_log_info("5-game mode: active (lock=%s, style=%s, %d game(s)) — "
                        "entering focus mode",
                        jw_focus_lock_name(state.focus_cfg.lock),
                        jw_focus_style_name(state.focus_cfg.style),
                        state.focus_cfg.id_count);
        } else if (pre.active && !lock_present) {
            jw_log_warn("5-game mode: active but recovery lock file absent — "
                        "unlocking (cleared five_game_active)");
            /* Entering focus mode turns the radio off and stashes the prior state,
               and the in-launcher exit restores it. This recovery path bypasses that
               exit, so without this the user would land in the normal launcher with
               Wi-Fi still off and no indication why. Restore it the same way. */
            if (pre.wifi_prev == 1 && jw_wifi_available()) {
                jw_log_info("5-game mode: restoring Wi-Fi after lock-file recovery");
                jw_wifi_set_radio(true);
            }
        }
    }

    /* HDMI boot-apply happens via the first hotplug poll; defer it a few seconds
       so the launcher + OSD are up before any TV-switch weston restart. */
    state.hdmi_last_connected = -1;
    state.hdmi_next_poll_ms = jw__monotonic_ms() + 4000;

    if (jw_ipc_server_listen(state.socket_path, &state.server) != 0) {
        jw_log_error("could not bind socket: %s", state.socket_path);
        jw__cleanup(&state);
        return 1;
    }

    /* Exported so child processes receive them via execv's inherited environment. */
    jw__publish_runtime_path_env(&state);
    jw__publish_audio_env(&state);

    /* Before anything reads a catalog. Runs after the runtime env is
       published because it needs $UMRK_INTERNAL_DATA_PATH. */
    jw__catalog_init(&state);

    /* LIFE-1 recovery precedes service scan/autostart. A process restart while
       a writer may still exist must never be interpreted as an empty launch;
       even a corrupt record leaves the conservative gate active. */
    {
        char active_reason[64];
        jw_active_game_load_result loaded = jw_active_game_load(
            state.runtime_dir, &state.active_game,
            active_reason, sizeof(active_reason));
        if (loaded == JW_ACTIVE_GAME_LOAD_VALID) {
            jw_log_warn("life1: recovered active launch id=%s source=%s; new games remain blocked",
                        state.active_game.launch_id,
                        state.active_game.source_id);
        } else if (loaded == JW_ACTIVE_GAME_LOAD_UNCERTAIN ||
                   loaded == JW_ACTIVE_GAME_LOAD_ERROR) {
            /* load() marks the state active/uncertain for both cases. */
            jw_log_error("life1: active launch recovery is uncertain (%s); game-sensitive services and new games remain blocked",
                         active_reason);
        }
    }

    /* SVC-1 service supervision: open the control-state store and scan for
       service-bearing paks. Non-fatal if it cannot come up. */
    jw__services_init(&state);

    /* TXN-1 pending uninstall is irreversible and therefore precedes P1's
       ordinary install reconciliation. With no supervisor, P1 still runs for
       unrelated targets but skips every durable pending-uninstall target. */
    state.pakrat_startup_recovery_needed = true;
    if (state.services) {
        jw__tick_package_mutation_recovery(&state);
    } else {
        int recovery_rc = jw__run_pakrat_p1_recovery(&state);
        if (recovery_rc == JW_PAKRAT_RECOVERY_OK) {
            state.pakrat_startup_recovery_needed = false;
        } else if (recovery_rc == JW_PAKRAT_RECOVERY_REPAIR_REQUIRED) {
            state.pakrat_startup_recovery_needed = false;
            jw_log_error("pakrat: install-transition recovery requires manual repair; automatic retries stopped");
        } else {
            jw_log_warn("pakrat: install-transition recovery failed; continuing startup");
        }
    }

    /* One-time: pre-create the per-system Roms/ folders so a fresh card is ready
       for drop-in ROMs without hunting the docs (needs ROMS_PATH above). */
    jw__seed_rom_folders(&state);

    /* Bring up the rumble motor (configure the PWM once, held off) and start the
       non-blocking pulse worker. Haptics no-op cleanly if the node is absent. */
    jw__rumble_init();
    /* Only run the worker when the channel AND its condvars came up. Starting it
       otherwise gives us a thread that can write duty values derived from an
       unknown polarity. */
    if (g_rumble_ready && g_rumble_cv_ready) {
        pthread_t rumble_thread;
        if (pthread_create(&rumble_thread, NULL, jw__rumble_worker, NULL) != 0) {
            jw_log_warn("rumble: worker thread failed to start; haptics disabled");
            g_rumble_ready = false;
            jw__rumble_off();
        } else {
            pthread_detach(rumble_thread);
        }
    }

    /* Export the user's time zone so launched apps (and the daemon's own
       localtime) use it. The launcher re-applies it live when changed. */
    {
        char tz[64] = "";
        if (state.db_path[0] &&
            jw_db_get_setting(state.db_path, "timezone", tz, sizeof(tz)) == 0 &&
            tz[0]) {
            setenv("TZ", tz, 1);
            tzset();
        }
    }

    setenv("JAWAKA_OSD_SOCKET", state.osd_socket_path, 1);
    setenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 0);
    {
        char runner_path[PATH_MAX];
        if (snprintf(runner_path, sizeof(runner_path), "%s/jawaka-retroarch-runner",
                     state.bin_dir) < (int)sizeof(runner_path)) {
            setenv("JAWAKA_RETROARCH_RUNNER", runner_path, 1);
        }
    }

    /* Before the proxy polls: the input path must never reach SQLite, so both
       the bindings and the capture opt-ins are in memory before the first
       chord can arrive. */
    jw__load_shortcuts(&state);
    jw__start_input_proxy(&state);
    if (jw_external_input_monitor_init(&state.external_input,
                                       jw__external_menu_tap, &state) != 0) {
        jw_log_warn("external input monitor: init failed; wireless controllers will be UI-only");
    }

    jw_log_info("jawakad starting");
    jw_log_info("runtime dir: %s", state.runtime_dir);
    jw_log_info("sdcard root: %s", state.sdcard_root);
    jw_log_info("socket path: %s", state.socket_path);
    jw_log_info("osd socket path: %s", state.osd_socket_path);
    jw_log_info("db path: %s", state.db_path);
    jw_log_info("platform: %s (%s)", state.platform.platform_id, state.platform.platform_name);
    jw_log_info("platform script dir: %s", state.platform.script_dir);
    if (state.daemon_only) {
        jw_log_info("daemon-only mode enabled");
    }

    /* Use the cached DB for the first frame whenever possible. If the cache is
       absent/empty, start a background scan instead of blocking launcher spawn;
       otherwise defer a freshness scan past frontend-ready. */
    {
        bool need_initial_scan = state.library_generation <= 0;
        /* Hidden games still populate the cache; browse counts can be zero. */
        jw_library_stats stats;
        memset(&stats, 0, sizeof(stats));
        if (!need_initial_scan) {
            need_initial_scan = jw_db_read_stats(state.db_path, &stats) != 0 ||
                                (stats.game_count <= 0 && stats.app_count <= 0);
        }
        if (need_initial_scan) {
            state.library_populated = false;
            if (jw__start_scan_job(&state, "at startup") < 0) {
                jw_log_warn("startup library scan could not start; launcher will show cache if available");
            }
        } else {
            state.library_populated = stats.game_count > 0 || stats.app_count > 0;
            jw_log_info("startup library scan deferred (generation=%d)",
                        state.library_generation);
        }
    }

    state.startup_maintenance_pending = true;
    state.startup_maintenance_phase = 0;
    state.startup_maintenance_next_ms = jw__monotonic_ms() +
        (state.daemon_only ? 0 : JW_STARTUP_MAINT_FALLBACK_MS);

    if (jw_scrape_worker_start(state.db_path, state.sdcard_root) != 0) {
        jw_log_warn("scrape worker failed to start; scraping disabled this run");
    }

    jw__spawn_osd(&state);

    /* Resume Game on Boot: before any launcher UI. A resume that started a
       game, or is still waiting on the achievements proxy or on service
       coordination, keeps the launcher away; every other outcome opens it
       (with the notice when one failed). */
    jw__boot_resume_at_startup(&state);

    if (!state.daemon_only && state.child_pid <= 0 &&
        state.boot_resume.phase == JW_BOOT_RESUME_PHASE_NONE) {
        if (jw__spawn_child(&state, JW_CHILD_LAUNCHER) != 0) {
            jw__cleanup(&state);
            return 1;
        }
    }

    while (1) {
        if (g_shutdown_requested) {
            state.shutdown_requested = true;
        }
        jw__drain_sigchld(&state);

        /* Detect a resume from ANY suspend — our auto-sleep OR loong_power's power
           button — by the gap between BOOTTIME (counts suspend time) and MONOTONIC
           (does not). On resume: drop gamepad presses made while asleep so they
           don't replay into the launcher, and treat the wake as fresh activity so
           the auto-sleep countdown restarts (otherwise it resumes mid-count and the
           screen blanks again moments after waking). Runs before the input tick so
           the flush lands before any buffered events are forwarded. */
        {
            static long long prev_mono = -1, prev_boot = -1;
            struct timespec mts, bts;
            clock_gettime(CLOCK_MONOTONIC, &mts);
            clock_gettime(CLOCK_BOOTTIME, &bts);
            long long mono = (long long)mts.tv_sec * 1000 + mts.tv_nsec / 1000000;
            long long boot = (long long)bts.tv_sec * 1000 + bts.tv_nsec / 1000000;
            if (prev_mono >= 0) {
                long long suspended = (boot - prev_boot) - (mono - prev_mono);
                if (suspended > 2000) {
                    jw_log_info("resume: ~%lldms suspended, flushing input + resetting idle",
                                suspended);
                    state.last_resume_ms = mono;
                    jw_input_proxy_flush(&state.input_proxy);
                    jw_input_proxy_mark_activity(&state.input_proxy);
                    jw__reconcile_audio(&state, "resume-detect", true);
                    jw__schedule_retroarch_audio_reinit(&state, "resume-detect-bluetooth");
                }
            }
            prev_mono = mono;
            prev_boot = boot;
        }

        jw_update_download_poll(&state.update_status, &state.update_download_job);
        jw__poll_update_install(&state);
        jw_update_check_poll(&state.update_status, &state.update_check_job);
        /* Child exits: only after a SIGCHLD, or on every pass while something
           is busy (a writer group can outlive its leader's one SIGCHLD). */
        bool check_exits = !state.reap_by_signal || state.sigchld_seen ||
                           jw__loop_busy(&state);
        if (state.sigchld_seen && state.services) {
            jw_svc_supervisor_note_child_exit(state.services);
        }
        state.sigchld_seen = false;
        if (check_exits) jw__handle_child_exit(&state);
        /* While RetroArch runs the synchronous save, nothing else talks to it:
           a blocked request would stall this loop and every retry would queue
           behind the save. */
        bool power_hold_saving = jw_power_hold_save_active(&state.power_hold_save);
        if (!power_hold_saving) {
            jw__tick_post_launch_resume(&state);
            jw__tick_boot_resume(&state);
            jw__tick_retroarch_warning(&state);
            jw__tick_retroarch_frontend_probe(&state);
            jw__tick_retroarch_stuck_quit(&state);
            jw__tick_in_game_menu_prewarm(&state);
        }
        if (check_exits) jw__handle_menu_exit(&state);
        if (!power_hold_saving) {
            jw__tick_advanced_shader(&state);
        }
        if (check_exits || (!state.osd_ready && state.osd_ready_fd >= 0)) {
            jw__handle_osd_exit(&state);
        }
        if (check_exits) jw__handle_ledd_exit(&state);
        jw_input_proxy_tick(&state.input_proxy);
        jw__tick_menu_escape(&state);
        jw_external_input_monitor_tick(&state.external_input,
                                       &state.input_proxy,
                                       (uint64_t)jw__monotonic_ms());
        /* A UI screenshot finished: flash the launcher, but only if it is still the
           foreground child (revalidated with the live pid, never a stale snapshot). */
        if (atomic_exchange(&g_ss_flash_request, 0) &&
            state.child_kind == JW_CHILD_LAUNCHER && state.child_pid > 0) {
            kill(state.child_pid, SIGUSR1);
        }
        unsigned audio_events = jw_platform_audio_tick(&state.platform);
        if (audio_events & JW_PLATFORM_AUDIO_EVENT_BLUETOOTH_CONNECTED) {
            jw__schedule_retroarch_audio_reinit(&state, "bluetooth-connected");
        }
        if (audio_events & JW_PLATFORM_AUDIO_EVENT_OUTPUT_CHANGED) {
            /* Each output restores its own stored level, so the cached percent
               now describes the output we just left. Drop it and let the next
               volume keypress re-read, or that press steps from the old value
               and jumps. */
            state.cached_volume_percent = -1;
            jw__notify_launcher_levels(&state);
        }
        if (!power_hold_saving) {
            jw__tick_retroarch_audio_reinit(&state);
        }
        jw__tick_rumble_reclaim(&state);
        jw__tick_suspend_inhibitors(&state);
        jw__tick_auto_sleep(&state);
        jw__tick_hdmi(&state);
        jw__tick_storage_health(&state);
        if (jw_platform_storage_tick(&state.platform)) {
            if (state.services) {
                /* Only a source going AWAY is a stop_on_storage_change event.
                   jw_platform_storage_tick() fires on any transition, and
                   stopping a running service because the user *inserted* a
                   card has no contract behind it -- SVC-1's storage policy
                   exists so a card is never pulled from under a live writer.
                   The backend reports no direction, so re-read the status. */
                jw_platform_storage_status storage_status;
                jw_platform_get_storage_status(&state.platform, NULL,
                                               &storage_status);
                bool source_departed = !storage_status.mounted;
                if (state.services_storage_stop_done) {
                    /* This daemon just performed a safe unmount and already
                       applied the policy; don't stop the same services twice. */
                    state.services_storage_stop_done = false;
                } else if (source_departed) {
                    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
                    int stopped = jw_svc_supervisor_storage_change_begin(
                        state.services, stuck, sizeof(stuck));
                    if (stopped > 0) {
                        jw_log_info("storage hotplug: stopped %d "
                                    "storage-sensitive service(s)", stopped);
                    }
                    if (stuck[0]) {
                        /* Already gone, so unlike safe-unmount this cannot be
                           refused. Warn and keep the survivor visible. */
                        jw_log_warn("storage hotplug: service %s could not be "
                                    "verified stopped", stuck);
                    }
                }
                if (jw_svc_supervisor_scan(state.services) < 0) {
                    jw_log_warn("storage hotplug: service rescan failed");
                }
                if (!source_departed) {
                    /* The mounted topology is now visible to the supervisor.
                     * Only now may services stopped for the departure resume. */
                    jw_svc_supervisor_storage_change_resume(state.services);
                }
            }
            if (jw__start_scan_job(&state, "after storage change") < 0) {
                jw_log_warn("storage hotplug: library rescan could not start");
            }
        }
        if (state.child_kind == JW_CHILD_PICO8_IMPORT && state.child_pid > 0 &&
            !state.child_stop_signalled && jw__monotonic_ms() >= state.child_stop_deadline_ms) {
            state.child_stop_signalled = true;
            jw_log_warn("PICO-8 import timed out; unfinished imports will retry next exit");
            (void)jw__signal_tracked_game_group(&state, SIGKILL);
        }
        jw__tick_scan_job(&state);
        jw__tick_delete(&state);
        jw__tick_startup_maintenance(&state);

        /* SVC-1 service supervision: poll child exit, run the stop sequence,
           retry stale-generation leases, and fire backoff/autostart. */
        if (state.services) {
            jw__tick_package_mutation_recovery(&state);
            jw_svc_supervisor_tick(state.services);
        }

        /* Save before power-button shutdown: hold teardown back while the
           release wait or the save runs. Top-of-loop input, child reaping, and
           service supervision above keep running. */
        if (state.shutdown_requested && jw_delete_job_busy(state.delete_job)) {
            jw__usleep(10000);
            continue;
        }
        if (state.shutdown_requested && jw__tick_power_hold_save(&state)) {
            jw__usleep(10000);
            continue;
        }

        if (state.shutdown_requested && state.child_pid <= 0 &&
            state.menu_pid <= 0) {
            break;
        }

        if (state.shutdown_requested && state.child_pid > 0) {
            if (jw__child_kind_has_stop_grace(state.child_kind)) {
                /* Signal once and remember the deadline: this branch runs on
                 * every loop pass, so re-sending SIGTERM and re-sleeping would
                 * restart the runner's save window on each iteration and then
                 * kill it anyway. Only the runner is signalled here -- it needs
                 * to stay alive long enough to promote the config; the group
                 * kill is the escalation. */
                if (!state.child_stop_signalled) {
                    state.child_stop_signalled = true;
                    state.child_stop_deadline_ms =
                        jw__monotonic_ms() + JW_RA_APP_STOP_GRACE_MS;
                    kill(state.child_pid, SIGTERM);
                    jw_log_info("shutdown: asked RetroArch app pid=%d to stop; "
                                "allowing %d ms to save",
                                (int)state.child_pid, JW_RA_APP_STOP_GRACE_MS);
                } else if (jw__monotonic_ms() >= state.child_stop_deadline_ms) {
                    jw_log_warn("shutdown: RetroArch app pid=%d did not stop in "
                                "time; killing pgid=%d",
                                (int)state.child_pid, (int)state.child_pgid);
                    (void)jw__signal_tracked_game_group(&state, SIGKILL);
                }
            } else if (jw__power_hold_save_quit_grace(&state)) {
                /* QUIT sent after a saved power-hold outcome: RetroArch exits
                   on its own and the normal path reaps it; the kill sequence
                   below runs only once the grace has expired. */
            } else {
                if (jw__child_kind_has_writer_barrier(state.child_kind)) {
                    (void)jw__signal_tracked_game_group(&state, SIGTERM);
                } else {
                    kill(state.child_pid, SIGTERM);
                }
                jw__usleep(50000);
                if (kill(state.child_pid, 0) == 0) {
                    if (jw__child_kind_has_writer_barrier(state.child_kind)) {
                        (void)jw__signal_tracked_game_group(&state, SIGKILL);
                    } else {
                        kill(state.child_pid, SIGKILL);
                    }
                }
            }
        }
        if (state.shutdown_requested && state.menu_pid > 0) {
            kill(state.menu_pid, SIGTERM);
            jw__usleep(50000);
            if (kill(state.menu_pid, 0) == 0) {
                kill(state.menu_pid, SIGKILL);
            }
        }

        if (state.shutdown_requested) {
            jw__usleep(50000);
            continue;
        }

        jw__ipc_tick(&state, jw__loop_timeout_ms(&state));
        jw__game_coordination_tick(&state);
        jw__tick_boot_resume_launch(&state);
        if (!state.daemon_only && state.child_pid <= 0 &&
            (state.game_check_decision || state.game_launch_blocked)) {
            if (jw__spawn_child(&state, JW_CHILD_LAUNCHER) != 0) {
                jw_log_error("life1: could not surface pending launch decision");
            }
        }
    }

    /* The game's writer group is gone: a temporary state that was never
       published can go now. */
    jw__power_hold_save_cleanup_tmp(&state);

    /* Quiesce, not a bare off: the worker is detached, so if a pattern is in
       flight when main returns the process dies mid-tick and the motor is
       stranded ON with nothing left to clear it. */
    jw__rumble_quiesce();

    bool shutdown_verified = true;
    int stuck_services = 0;
    char stuck_record[2048] = "";
    /* SVC-1: stop every running service and verify each group absent before
       exiting. Per the contract's unverified-stop table, an MLP1 power
       transition with a stuck service does not report completion: the rootfs
       supervisor pauses, and continues on proven absence, a user override, or
       its critical-battery rule. Exit-to-stock and other platforms continue
       with the warning. */
    if (state.services) {
        int stuck = jw_svc_supervisor_stop_all(state.services);
        if (stuck > 0) {
            shutdown_verified = false;
            stuck_services = stuck;
            (void)jw_svc_supervisor_describe_unverified(state.services, stuck_record,
                                                        sizeof(stuck_record));
            jw_log_warn("services: %d service(s) could not be verified stopped "
                        "during shutdown", stuck);
        }
    }

    jw_log_info("jawakad exiting");
    jw__cleanup(&state);
#ifdef PLATFORM_MLP1
    if (state.power_transition_requested) {
        if (shutdown_verified && state.child_pid <= 0) {
            if (jw_power_request_complete() != 0) {
                (void)jw_power_request_incomplete("reason=cleanup-failed\n");
                return 1;
            }
        } else if (jw__publish_power_incomplete(stuck_services, stuck_record,
                                                state.child_pid) != 0) {
            jw_log_error("power handoff: could not record the unverified stop; "
                         "the supervisor can only offer an override");
        }
    }
#else
    (void)shutdown_verified;
    (void)stuck_services;
    (void)stuck_record;
#endif

    /* Write clean-exit marker so the Leaf boot supervisor's crash-loop guard
       knows this was an intentional shutdown, not a crash. The marker lives in
       tmpfs by default so it clears on reboot. */
    {
        char clean_exit[PATH_MAX];
        if (jw__env_or_join(clean_exit, sizeof(clean_exit),
                            "UMRK_CLEAN_EXIT_SENTINEL",
                            "TMPDIR", NULL,
                            "/tmp", "umrk-clean-exit") == 0) {
            FILE *fp = fopen(clean_exit, "w");
            if (fp) fclose(fp);
        }
    }

    return 0;
}
