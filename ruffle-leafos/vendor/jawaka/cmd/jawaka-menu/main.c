#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "internal/core/autodemo.h"
#include "internal/i18n/i18n.h"
#include "internal/core/log.h"
#include "internal/core/title.h"
#include "internal/db/db.h"
#include "internal/ipc/ipc_client.h"
#include "internal/launcher/game_switcher.h"
#include "internal/platform/cat_services.h"
#include "internal/platform/paths.h"
#include "internal/retroarch/shader_catalog.h"
#include "internal/retroarch/shader_picker.h"
#include "internal/retroarch/states.h"
#include "internal/settings/settings.h"
#include "internal/settings/theme_resolve.h"

#include <SDL2/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static inline const char *jw_hint(const char *desktop_key) {
    return CAT_PLATFORM_IS_DEVICE ? NULL : desktop_key;
}
static inline const char *jw_hint_device(const char *desktop_key, const char *device_key) {
    return CAT_PLATFORM_IS_DEVICE ? device_key : desktop_key;
}
#define JW_HINT(dk)            jw_hint(dk)
#define JW_HINT_DEVICE(dk, vk) jw_hint_device(dk, vk)

static long long jw__monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

/* Self-pipe used to wake the resident in-game menu when the daemon asks it to
   show (SIGUSR1). Writing one byte from the async-signal-safe handler lets the
   parked poll() in jw__menu_wait_for_show() return instantly. */
static int g_show_pipe[2] = { -1, -1 };
static int g_hide_pipe[2] = { -1, -1 };

static void jw__menu_show_signal(int signo) {
    (void)signo;
    if (g_show_pipe[1] >= 0) {
        const char b = 1;
        ssize_t n = write(g_show_pipe[1], &b, 1);
        (void)n;
    }
}

/* Set when the daemon asks us to hide (Menu toggle closed, SIGUSR2). Checked by
   the visible render loop so the menu drops back to standby. */
static volatile sig_atomic_t g_hide_requested = 0;

static bool jw__menu_hide_requested(void) {
    return g_hide_requested != 0;
}

static void jw__menu_hide_signal(int signo) {
    (void)signo;
    int saved_errno = errno;
    g_hide_requested = 1;
    const char b = 1;
    ssize_t n = write(g_hide_pipe[1], &b, 1);
    (void)n;
    errno = saved_errno;
}

static void jw__menu_clear_hide(void) {
    char buf[64];
    while (read(g_hide_pipe[0], buf, sizeof(buf)) > 0) { }
    g_hide_requested = 0;
}

/* Install the SIGUSR1 show handler and self-pipe. Call before cat_init so a
   show request that races startup is buffered, not lost. When the daemon
   spawned us for immediate display (JAWAKA_INGAME_AUTOSHOW=1, the on-demand
   fallback) we pre-arm the pipe so the first wait returns at once. */
static int jw__menu_init_show_signal(void) {
    if (pipe(g_show_pipe) != 0) {
        jw_log_error("in-game menu: show pipe failed: %s", strerror(errno));
        return -1;
    }
    fcntl(g_show_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(g_show_pipe[1], F_SETFL, O_NONBLOCK);
    if (pipe(g_hide_pipe) != 0) {
        jw_log_error("in-game menu: hide pipe failed: %s", strerror(errno));
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        if (fcntl(g_hide_pipe[i], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(g_hide_pipe[i], F_SETFD, FD_CLOEXEC) < 0) {
            jw_log_error("in-game menu: hide pipe setup failed: %s", strerror(errno));
            return -1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = jw__menu_show_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* keep SDL syscalls during cat_init from seeing EINTR */
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        jw_log_error("in-game menu: sigaction failed: %s", strerror(errno));
        return -1;
    }

    /* The pipe stays readable even if SIGUSR2 arrives before cat_present()
       starts waiting or is delivered to a graphics helper thread. */
    struct sigaction sh;
    memset(&sh, 0, sizeof(sh));
    sh.sa_handler = jw__menu_hide_signal;
    sigemptyset(&sh.sa_mask);
    sh.sa_flags = 0;
    if (sigaction(SIGUSR2, &sh, NULL) != 0) {
        jw_log_error("in-game menu: sigaction(USR2) failed: %s", strerror(errno));
        return -1;
    }

    const char *autoshow = getenv("JAWAKA_INGAME_AUTOSHOW");
    if (autoshow && strcmp(autoshow, "1") == 0) {
        jw__menu_show_signal(SIGUSR1); /* pre-arm: reveal as soon as we are ready */
    }
    return 0;
}

/* Block at ~zero CPU until the daemon requests a show (SIGUSR1) or the autoshow
   pre-arm fires, then drain the pipe and return. poll() is not restarted across
   signals, so EINTR just re-enters the loop and finds the buffered byte. */
static void jw__menu_wait_for_show(void) {
    struct pollfd pfd = { .fd = g_show_pipe[0], .events = POLLIN, .revents = 0 };
    for (;;) {
        int rc = poll(&pfd, 1, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            jw_log_warn("in-game menu: show wait poll failed: %s", strerror(errno));
            return; /* fail open: reveal rather than hang */
        }
        if (rc > 0 && (pfd.revents & POLLIN)) {
            char buf[64];
            while (read(g_show_pipe[0], buf, sizeof(buf)) > 0) { }
            return;
        }
    }
}

/* System menu items, ordered benign -> destructive: system info/maintenance up
   top, session in the middle, the destructive power trio anchored at the bottom
   so it can't be fat-fingered. About and System Update are hosted here (they no
   longer live in Settings) via the shared jw_settings_ui render API. */
static const char *kMenuItems[] = {
    JW_UI("Search"),
    JW_UI("System Update"),
    JW_UI("About"),
    JW_UI("Rescan Library"),
    JW_UI("Return to Launcher"),
    JW_UI("Sleep"),
    JW_UI("Exit to Stock"),
    JW_UI("Reboot"),
    JW_UI("Power Off"),
};
#define JW_MENU_COUNT       9
#define JW_MENU_SEARCH      0
#define JW_MENU_UPDATE      1
#define JW_MENU_ABOUT       2
#define JW_MENU_RESCAN      3
#define JW_MENU_RETURN      4
#define JW_MENU_SLEEP       5
#define JW_MENU_EXIT_STOCK  6
#define JW_MENU_REBOOT      7
#define JW_MENU_POWEROFF    8

/* Picking Search drops a /tmp marker and returns to the launcher, which opens its
   search overlay on respawn (the launcher exits while this menu is up). */
#define JW_OPEN_SEARCH_MARKER "/tmp/jawaka-open-search"

static const char *kInGameItems[] = {
    "Continue",
    "Save State",
    "Load State",
    "Reset",
    "Performance",
    "Shader",
    "RetroArch Settings",
    "Quit",
};
#define JW_INGAME_COUNT     8
#define JW_INGAME_CONTINUE  0
#define JW_INGAME_SAVE      1
#define JW_INGAME_LOAD      2
#define JW_INGAME_RESET     3
#define JW_INGAME_PERF      4
#define JW_INGAME_SHADER    5
#define JW_INGAME_SETTINGS  6
#define JW_INGAME_QUIT      7

_Static_assert(sizeof(kInGameItems) / sizeof(kInGameItems[0]) == JW_INGAME_COUNT,
               "in-game row count must match its table");
_Static_assert(JW_INGAME_SHADER == JW_INGAME_PERF + 1 &&
               JW_INGAME_SETTINGS == JW_INGAME_SHADER + 1,
               "Shader must stay between Performance and RetroArch Settings");

/* A frame that stalls RetroArch for over a second, such as a slow disc read or
   a first start, can time out the session query the menu sends as it opens.
   The menu would then show no save slots and "Command: timeout" until it was
   reopened, so it asks again a few times while it stays open. */
#define JW_INGAME_REFRESH_RETRIES  3
#define JW_INGAME_REFRESH_RETRY_MS 400

#define JW_INGAME_PERF_ROWS 5
#define JW_INGAME_PERF_PROFILE 0
#define JW_INGAME_PERF_CPU     1
#define JW_INGAME_PERF_GPU     2
#define JW_INGAME_PERF_DMC     3
#define JW_INGAME_PERF_RESET   4

typedef struct {
    const char *label;
    const char *governor;
    int frequency;
} jw_perf_option;

static const jw_platform_perf_profile kInGamePerfProfiles[] = {
    JW_PLATFORM_PERF_PROFILE_BALANCED,
    JW_PLATFORM_PERF_PROFILE_PERFORMANCE,
    JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER,
    JW_PLATFORM_PERF_PROFILE_CUSTOM,
};
#define JW_INGAME_PERF_PROFILE_COUNT \
    ((int)(sizeof(kInGamePerfProfiles) / sizeof(kInGamePerfProfiles[0])))

static const jw_perf_option kCpuPerfOptions[] = {
    { "Balanced", "schedutil", -1 },
    { "Performance", "performance", -1 },
    { "600 MHz", "userspace", 600000 },
    { "816 MHz", "userspace", 816000 },
    { "1.10 GHz", "userspace", 1104000 },
    { "1.42 GHz", "userspace", 1416000 },
    { "1.61 GHz", "userspace", 1608000 },
    { "1.80 GHz", "userspace", 1800000 },
    { "1.99 GHz", "userspace", 1992000 },
};

static const jw_perf_option kGpuPerfOptions[] = {
    { "Balanced", "simple_ondemand", -1 },
    { "Performance", "performance", -1 },
    { "200 MHz", "userspace", 200000000 },
    { "300 MHz", "userspace", 300000000 },
    { "400 MHz", "userspace", 400000000 },
    { "600 MHz", "userspace", 600000000 },
    { "700 MHz", "userspace", 700000000 },
    { "800 MHz", "userspace", 800000000 },
};

static const jw_perf_option kDmcPerfOptions[] = {
    { "Balanced", "dmc_ondemand", -1 },
    { "Performance", "performance", -1 },
    { "324 MHz", "userspace", 324000000 },
    { "528 MHz", "userspace", 528000000 },
    { "780 MHz", "userspace", 780000000 },
    { "1.06 GHz", "userspace", 1056000000 },
};

#define JW_CPU_PERF_OPTION_COUNT ((int)(sizeof(kCpuPerfOptions) / sizeof(kCpuPerfOptions[0])))
#define JW_GPU_PERF_OPTION_COUNT ((int)(sizeof(kGpuPerfOptions) / sizeof(kGpuPerfOptions[0])))
#define JW_DMC_PERF_OPTION_COUNT ((int)(sizeof(kDmcPerfOptions) / sizeof(kDmcPerfOptions[0])))

typedef struct {
    cat_list_state      list;
    char                status[256];
    cat_status_bar_opts status_bar;
    bool                show_hints;
    const char         *db_path;          /* for hosting About / System Update */
    int                 pending_settings; /* JW_SETTINGS_* to host next, or -1 */
    jw_library_summary  summary;          /* live counts for the Rescan detail */
    bool                summary_ready;
} jw_menu_state;

typedef struct {
    const char                     *db_path;         /* library DB, for game-title lookup */
    cat_list_state                  list;
    char                            status[256];
    char                            game_title[256]; /* header: resolved game name */
    char                            console_title[96];/* subtitle: console display name */
    cat_status_bar_opts             status_bar;
    bool                            show_hints;
    bool                            session_details_ready;
    bool                            perf_ready;
    jw_ipc_performance_status_info  perf;
    int                             perf_profile_index;
    int                             perf_cpu_index;
    int                             perf_gpu_index;
    int                             perf_dmc_index;
    char                            shader_label[96];
    jw_ipc_retroarch_session_info   session;
    SDL_Texture                    *still_tex;       /* paused-game still behind the menu */
    SDL_Texture                    *thumb_tex;       /* selected-slot savestate thumbnail */
    int                             thumb_slot;      /* slot thumb_tex is for (INT_MIN = none) */
    long long                       thumb_move_ms;   /* accepted move into Save/Load */
    long long                       thumb_present_ms;/* first frame still owed for that move */
    bool                            quit_save;       /* Quit row armed to Save & Quit (default) */
    /* Decoupled Save/Load slot selection (Item 3): the menu drives explicit
       slots rather than RetroArch's single shared slot. Save is a numeric target
       (Auto + 0–9, slot 99 hidden); Load browses existing saves newest-first,
       with the switcher quicksave surfaced as a "Latest" entry when it's newest. */
    int                             save_slot;       /* -1 = auto, 0..9 (skips 99) */
    int                             load_index;      /* selection in the Load list */
    int                             load_count;      /* entries in load_entries */
    struct {
        int  slot;                                   /* -1 auto, 0..9, or switcher slot */
        long mtime;
        bool is_latest;                              /* switcher quicksave ("Latest") */
    }                               load_entries[32];
} jw_ingame_state;

/* Footer labels are UI text; the button_text pills ("A", "L1/R1") are glyphs and
   stay as they are. Funnelled here so the eight footer arrays below stay
   untouched -- they are stack locals, so rewriting the pointer is safe. */
static void jw__menu_footer(cat_footer_item *items, int count) {
    for (int i = 0; i < count; i++) {
        if (items[i].label) items[i].label = T(items[i].label);
    }
    cat_draw_footer(items, count);
}

static void jw__render_menu(const jw_menu_state *state) {
    ap_theme *theme     = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_MEDIUM);

    cat_status_bar_opts sb = state->status_bar;

    cat_clear_screen();
    cat_draw_screen_title(T("System"), &sb);

    /* Box-model composition (same shape as the Settings / browse pages): carve a
       0-height tab bar and a 0-height sub-header off the content box, leaving a
       full-width list with no cover/icon pane. */
    SDL_Rect cr = cat_get_content_rect(true, true, false);
    cat_box page = { cr.x, cr.y, cr.w, cr.h, 0, 0, 0, 0 };
    (void)cat_box_carve_top(&page, 0);   /* tab bar: none      */
    (void)cat_box_carve_top(&page, 0);   /* sub-header: none   */
    SDL_Rect content = cat_box_content(&page);

    int x      = content.x + CAT_S(24);
    int item_w = content.w * 55 / 100;
    int body_h = TTF_FontHeight(body_font);
    int item_h = body_h + CAT_S(12);
    int pill_h = body_h + CAT_S(6);
    int pill_w = item_w;

    /* The right column (formerly the browse layout's cover pane) is reserved for
       the Rescan Library notification — the live library counts, or "Scanning…"
       while a scan runs. Every other row leaves it empty. */
    int detail_x = x + item_w + CAT_S(14);
    int detail_w = (content.x + content.w - CAT_S(24)) - detail_x;

    for (int i = 0; i < JW_MENU_COUNT; i++) {
        int iy     = content.y + CAT_S(16) + i * item_h;
        int pill_y = iy + (item_h - pill_h) / 2;
        bool sel   = (i == state->list.cursor);

        if (sel)
            cat_draw_pill(x - CAT_S(10), pill_y, pill_w, pill_h, theme->highlight);

        ap_color col = sel ? theme->highlighted_text : theme->text;
        int text_y   = pill_y + (pill_h - body_h) / 2;
        cat_draw_text(body_font, T(kMenuItems[i]), x, text_y, col);
    }

    /* Right pane: the Rescan Library notification, sized to fill the pane (not a
       single line). "Scanning…" while a scan runs; otherwise the live library
       counts, stacked and centered over the pane's full height. */
    if (detail_w > CAT_S(40)) {
        int pane_y = content.y + CAT_S(16);
        int pane_h = JW_MENU_COUNT * item_h;
        int line_h = TTF_FontHeight(body_font);
        if (state->status[0]) {
            int ty = pane_y + (pane_h - line_h) / 2;
            cat_draw_text_wrapped(body_font, state->status, detail_x, ty,
                                  detail_w, theme->hint, CAT_ALIGN_CENTER);
        } else if (state->summary_ready) {
            char l1[32], l2[32];
            snprintf(l1, sizeof(l1), "%d games", state->summary.game_count);
            snprintf(l2, sizeof(l2), "%d apps",  state->summary.app_count);
            int gap     = CAT_S(6);
            int block_h = line_h * 2 + gap;
            int ty      = pane_y + (pane_h - block_h) / 2;
            cat_draw_text_wrapped(body_font, l1, detail_x, ty, detail_w,
                                  theme->text, CAT_ALIGN_CENTER);
            cat_draw_text_wrapped(body_font, l2, detail_x, ty + line_h + gap,
                                  detail_w, theme->hint, CAT_ALIGN_CENTER);
        }
    }

    if (state->show_hints) {
        cat_footer_item footer[] = {
            { CAT_BTN_B,  "Back",     true,  JW_HINT("B") },
            { CAT_BTN_A,  "Select",   true,  JW_HINT("A") },
        };
        jw__menu_footer(footer, 2);
    }
    cat_present();
}

/* Fire a semantic UI haptic (see internal/ipc jw_ipc_rumble). Fire-and-forget:
   the daemon owns the vocabulary (single/double/triple tick) and the enable/nav
   gating, so call sites only name the event.

   Safe to use from the in-game menu even though a game normally owns the motor:
   jawakad pauses RetroArch before showing this menu, so the core is not driving
   rumble and the daemon is the only writer while the menu is up. */
static inline void jw__haptic(const char *socket_path, const char *event) {
    if (socket_path && socket_path[0]) jw_ipc_rumble(socket_path, event);
}

/* Catastrophe's widgets report what happened to them; this turns that into what
   it feels like. Same mapping the launcher uses -- movement and committed values
   both ride the opt-in navigation tick. */
static void jw__ui_feedback(cat_ui_feedback what, void *user) {
    const char *socket_path = (const char *)user;
    switch (what) {
        case CAT_UI_MOVED:
        case CAT_UI_ENTERED:
            jw__haptic(socket_path, "nav");
            break;
        case CAT_UI_SURFACE:
            jw__haptic(socket_path, "select");
            break;
        case CAT_UI_EDGE:
            jw__haptic(socket_path, "blocked");
            break;
    }
}

static int jw__activate(const char *socket_path, jw_menu_state *state, bool *running) {
    switch (state->list.cursor) {
        case JW_MENU_SEARCH: {
            /* Hand off to the launcher: drop a marker it consumes on respawn. */
            FILE *mf = fopen(JW_OPEN_SEARCH_MARKER, "w");
            if (mf) fclose(mf);
            *running = false;
            return 0;
        }
        case JW_MENU_UPDATE:
            /* Hosted in this popup via the shared settings UI; main() picks it up
               after input so the modal sub-loop runs outside the event drain. */
            state->pending_settings = JW_SETTINGS_UPDATE;
            return 0;
        case JW_MENU_ABOUT:
            state->pending_settings = JW_SETTINGS_ABOUT;
            return 0;
        case JW_MENU_RESCAN: {
            snprintf(state->status, sizeof(state->status), "%s", T("Scanning…"));
            cat_request_frame();
            jw__render_menu(state);
            int rc = jw_ipc_scan_library(socket_path, state->status,
                                         sizeof(state->status));
            if (rc == 0 && strcmp(state->status, "scan started") == 0) {
                snprintf(state->status, sizeof(state->status), "%s", T("Scanning…"));
            } else if (rc != 0 && !state->status[0]) {
                snprintf(state->status, sizeof(state->status), "%s", T("Scan failed"));
            }
            /* Refresh cached counts opportunistically; the daemon now runs the
               scan asynchronously, so these may remain the pre-scan counts. */
            if (jw_db_read_summary(state->db_path, &state->summary) == 0)
                state->summary_ready = true;
            return rc;
        }
        case JW_MENU_RETURN:
            *running = false;
            return 0;
        case JW_MENU_SLEEP:
            /* The platform "sleep" write to /sys/power/state blocks until the
               system resumes, so this call returns after wake. Keep the menu
               open so the user lands back where they were. */
            snprintf(state->status, sizeof(state->status), "%s", T("Sleeping…"));
            cat_request_frame();
            jw__render_menu(state);
            if (jw_ipc_platform_action(socket_path, "sleep", 0) != 0) {
                /* Daemon unreachable: keep the feedback up instead of flashing.
                   Return the failure too, or the caller's haptic contradicts
                   the status line the user is looking at. */
                snprintf(state->status, sizeof(state->status), "%s", T("Sleep failed"));
                return -1;
            }
            state->status[0] = '\0';
            return 0;
        /* Close only once the daemon has accepted these. Discarding the result
           and closing regardless means that with jawakad unreachable the menu
           disappears and the caller's haptic says "committed" for something
           that never happened -- the same contract the Sleep case above already
           gets right. */
        case JW_MENU_EXIT_STOCK:
            if (jw_ipc_exit_stock(socket_path) != 0) {
                snprintf(state->status, sizeof(state->status), "%s",
                         "Exit to stock failed");
                return -1;
            }
            *running = false;
            return 0;
        case JW_MENU_REBOOT:
            if (jw_ipc_platform_action(socket_path, "reboot", 0) != 0) {
                snprintf(state->status, sizeof(state->status), "%s", T("Reboot failed"));
                return -1;
            }
            *running = false;
            return 0;
        case JW_MENU_POWEROFF:
            if (jw_ipc_platform_action(socket_path, "poweroff", 0) != 0) {
                snprintf(state->status, sizeof(state->status), "%s",
                         "Power off failed");
                return -1;
            }
            *running = false;
            return 0;
        default:
            return 0;
    }
}

static void jw__handle_input(const char *socket_path, jw_menu_state *state,
                              cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
            cat_list_state_move(&state->list, -1, JW_MENU_COUNT);
            state->status[0] = '\0';   /* a moved cursor dismisses stale feedback */
            break;
        case CAT_BTN_DOWN:
            cat_list_state_move(&state->list, +1, JW_MENU_COUNT);
            state->status[0] = '\0';
            break;
        case CAT_BTN_A:
        case CAT_BTN_START: {
            /* Rows that only open another surface get the overlay-open tick the
               launcher uses; the rest report what actually happened, since
               Rescan and Sleep can come back having failed. */
            int cur = state->list.cursor;
            bool opens_surface = (cur == JW_MENU_SEARCH ||
                                  cur == JW_MENU_UPDATE ||
                                  cur == JW_MENU_ABOUT);
            int rc = jw__activate(socket_path, state, running);
            jw__haptic(socket_path,
                       opens_surface ? "select" : (rc == 0 ? "commit" : "blocked"));
            break;
        }
        case CAT_BTN_B:
            jw__haptic(socket_path, "select");
            *running = false;
            break;
        default:
            break;
    }
}

static const char *jw__basename_const(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash && slash[1] ? slash + 1 : (path ? path : "");
}

static void jw__title_from_filename(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    const char *base = jw__basename_const(path);
    size_t n = strlen(base);
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy(out, base, n);
    out[n] = '\0';
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

static int jw__db_game_name(sqlite3 *db, const char *rom_path,
                            char *out, size_t out_size) {
    if (!db || !rom_path || !rom_path[0]) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
                           "SELECT COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), g.name) "
                           "FROM games g "
                           "LEFT JOIN game_settings gs "
                           "ON gs.game_id = g.id AND gs.key = 'display_name' "
                           "LEFT JOIN game_settings ig "
                           "ON ig.game_id = g.id AND ig.key = 'imported_display_name' "
                           "WHERE g.rom_path = ? LIMIT 1;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, rom_path, -1, SQLITE_TRANSIENT);
    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 0);
        if (name && name[0]) {
            snprintf(out, out_size, "%s", name);
            rc = 0;
        }
    }
    sqlite3_finalize(stmt);
    return rc;
}

/* Resolve the header game title and console subtitle for the active session.
   Game title prefers the user display name/library name keyed by rom_path,
   falling back to a cleaned ROM basename. The console uses the launch-time
   system id so an active IGM never depends on replaceable metadata. */
static void jw__ingame_resolve_titles(jw_ingame_state *state) {
    if (!state->session.active) {
        snprintf(state->game_title, sizeof(state->game_title), "%s",
                 "No active session");
        state->console_title[0] = '\0';
        return;
    }

    jw__title_from_filename(state->session.rom_path, state->game_title,
                            sizeof(state->game_title));

    sqlite3 *db = NULL;
    if (state->db_path && jw_db_open(state->db_path, &db) == 0) {
        int name_rc = -1;
        char *sd_root = jw_sdcard_root();
        if (sd_root && sd_root[0]) {
            size_t root_len = strlen(sd_root);
            if (strncmp(state->session.rom_path, sd_root, root_len) == 0 &&
                state->session.rom_path[root_len] == '/') {
                name_rc = jw__db_game_name(db, state->session.rom_path + root_len + 1,
                                           state->game_title, sizeof(state->game_title));
            }
        }
        if (sd_root) {
            free(sd_root);
        }
        /* The DB keys games by SD-relative path, so this absolute lookup only
           helps if the relative one missed (or didn't apply). */
        if (name_rc != 0) {
            jw__db_game_name(db, state->session.rom_path,
                             state->game_title, sizeof(state->game_title));
        }
        jw_db_close(db);
    }

    jw_clean_rom_title(state->game_title);   /* drop (U)/[!]/(En,Fr) filename tags */

    snprintf(state->console_title, sizeof(state->console_title), "%s",
             state->session.system[0] ? state->session.system : "");
}

/* ── Decoupled Save/Load slot selection (Item 3) ──────────────────────────── */

/* Resolve the active core's in-game state namespace from the folder captured
   by the daemon at launch. This keeps a live session independent of later
   metadata removal/replacement and prevents cross-core state exposure. */
static bool jw__ingame_states_dir(const jw_ingame_state *state,
                                  char *out, size_t out_size) {
    if (!state || !state->session.core_config_folder[0] ||
        !out || out_size == 0) {
        return false;
    }

    char root[PATH_MAX];
    root[0] = '\0';
    const char *states = getenv("JAWAKA_INGAME_STATEDIR");
    if (states && states[0]) {
        snprintf(root, sizeof(root), "%s", states);
    } else {
        const char *sp = getenv("STATES_PATH");
        if (sp && sp[0]) {
            snprintf(root, sizeof(root), "%s", sp);
        }
    }

    char *sd = jw_sdcard_root();
    if (!root[0] && sd && sd[0]) {
        snprintf(root, sizeof(root), "%s/States", sd);
    }
    if (!root[0]) {
        free(sd);
        out[0] = '\0';
        return false;
    }
    bool ok = jw_ra_core_states_dir(root, state->session.core_config_folder,
                                    out, out_size);
    free(sd);
    if (!ok) out[0] = '\0';
    return ok;
}

static void jw__rel_time(long mtime, char *out, size_t out_size) {
    if (mtime <= 0) {
        out[0] = '\0';
        return;
    }
    long d = (long)time(NULL) - mtime;
    if (d < 0) d = 0;
    if (d < 60) {
        snprintf(out, out_size, "%s", T("just now"));
    } else if (d < 3600) {
        snprintf(out, out_size, T("%ldm ago"), d / 60);
    } else if (d < 86400) {
        snprintf(out, out_size, T("%ldh ago"), d / 3600);
    } else {
        snprintf(out, out_size, T("%ldd ago"), d / 86400);
    }
}

static void jw__slot_name(int slot, char *out, size_t out_size) {
    if (slot < 0) {
        snprintf(out, out_size, "%s", T("Slot Auto"));
    } else {
        snprintf(out, out_size, T("Slot %d"), slot);
    }
}

/* Slot the selected Save/Load row acts on, for the thumbnail preview.
   INT_MIN = nothing to preview. */
static int jw__ingame_selected_slot(const jw_ingame_state *state) {
    if (state->list.cursor == JW_INGAME_SAVE) {
        return state->save_slot;
    }
    if (state->list.cursor == JW_INGAME_LOAD &&
        state->load_count > 0 && state->load_index >= 0 &&
        state->load_index < state->load_count) {
        return state->load_entries[state->load_index].slot;
    }
    return INT_MIN;
}

static void jw__save_caption(const jw_ingame_state *state, char *out, size_t out_size) {
    bool exists = false;
    for (int i = 0; i < state->load_count; i++) {
        if (!state->load_entries[i].is_latest &&
            state->load_entries[i].slot == state->save_slot) {
            exists = true;
            break;
        }
    }
    char name[16];
    jw__slot_name(state->save_slot, name, sizeof(name));
    snprintf(out, out_size, "%s · %s", name, exists ? T("overwrites") : T("empty"));
}

/* Load row caption: a main label plus a recency sub-line (newest/oldest marker,
   relative time, and position) so "sorted by last created" reads clearly. */
static void jw__load_caption(const jw_ingame_state *state,
                             char *main, size_t main_size,
                             char *sub, size_t sub_size) {
    main[0] = '\0';
    sub[0] = '\0';
    if (state->load_count <= 0) {
        snprintf(main, main_size, "%s", T("No saves yet"));
        return;
    }
    int idx = state->load_index;
    if (idx < 0) idx = 0;
    if (idx >= state->load_count) idx = state->load_count - 1;
    char rel[24];
    jw__rel_time(state->load_entries[idx].mtime, rel, sizeof(rel));
    const char *order = (idx == 0) ? T("newest")
                      : (idx == state->load_count - 1 ? T("oldest") : "");
    if (state->load_entries[idx].is_latest) {
        snprintf(main, main_size, "%s", T("Latest"));
        if (rel[0]) {
            snprintf(sub, sub_size, T("unsaved · %s · %d of %d"),
                     rel, idx + 1, state->load_count);
        } else {
            snprintf(sub, sub_size, T("unsaved · %d of %d"),
                     idx + 1, state->load_count);
        }
        return;
    }
    jw__slot_name(state->load_entries[idx].slot, main, main_size);
    if (order[0] && rel[0]) {
        snprintf(sub, sub_size, T("%s · %s · %d of %d"),
                 order, rel, idx + 1, state->load_count);
    } else if (rel[0]) {
        snprintf(sub, sub_size, T("%s · %d of %d"), rel, idx + 1, state->load_count);
    } else {
        snprintf(sub, sub_size, T("%d of %d"), idx + 1, state->load_count);
    }
}

/* Rebuild the Load list for the active ROM: existing saves newest-first, the
   switcher slot (99) hidden as a number but surfaced as a "Latest" entry when it
   is newer than every numbered save. Called on open and after a Keep. */
static void jw__ingame_rebuild_slots(jw_ingame_state *state) {
    state->load_count = 0;
    state->load_index = 0;
    state->thumb_slot = INT_MIN; /* force preview refresh */
    if (!state->session.rom_path[0]) {
        return;
    }
    char dir[PATH_MAX];
    if (!jw__ingame_states_dir(state, dir, sizeof(dir))) {
        return;
    }

    jw_ra_slot_info raw[64];
    int n = 0;
    jw_ra_list_slots(dir, state->session.rom_path, raw, 64, &n);

    long switcher_mtime = 0;
    bool has_switcher = false;
    jw_ra_slot_info nums[64];
    int nc = 0;
    for (int i = 0; i < n; i++) {
        if (raw[i].slot == JW_RA_GAME_SWITCHER_STATE_SLOT) {
            has_switcher = true;
            switcher_mtime = raw[i].mtime;
        } else {
            nums[nc++] = raw[i];
        }
    }
    for (int i = 1; i < nc; i++) { /* insertion sort, newest-first */
        jw_ra_slot_info key = nums[i];
        int j = i - 1;
        while (j >= 0 && nums[j].mtime < key.mtime) {
            nums[j + 1] = nums[j];
            j--;
        }
        nums[j + 1] = key;
    }
    bool latest = has_switcher && (nc == 0 || switcher_mtime > nums[0].mtime);

    int cap = (int)(sizeof(state->load_entries) / sizeof(state->load_entries[0]));
    int idx = 0;
    if (latest && idx < cap) {
        state->load_entries[idx].slot = JW_RA_GAME_SWITCHER_STATE_SLOT;
        state->load_entries[idx].mtime = switcher_mtime;
        state->load_entries[idx].is_latest = true;
        idx++;
    }
    for (int i = 0; i < nc && idx < cap; i++) {
        state->load_entries[idx].slot = nums[i].slot;
        state->load_entries[idx].mtime = nums[i].mtime;
        state->load_entries[idx].is_latest = false;
        idx++;
    }
    state->load_count = idx;
    state->load_index = 0;
}

/* Default the Save target to the lowest free numbered slot (avoid clobbering an
   existing save); fall back to 0 when 0–9 are all taken. */
static void jw__ingame_init_save_slot(jw_ingame_state *state) {
    int slot = 0;
    for (int s = 0; s <= 9; s++) {
        bool used = false;
        for (int i = 0; i < state->load_count; i++) {
            if (!state->load_entries[i].is_latest &&
                state->load_entries[i].slot == s) {
                used = true;
                break;
            }
        }
        if (!used) {
            slot = s;
            break;
        }
    }
    state->save_slot = slot;
}

static int jw__ingame_perf_profile_index(const char *name) {
    jw_platform_perf_profile profile;
    if (!jw_platform_parse_perf_profile(name, &profile)) {
        return 0;
    }
    for (int i = 0; i < JW_INGAME_PERF_PROFILE_COUNT; i++) {
        if (kInGamePerfProfiles[i] == profile) {
            return i;
        }
    }
    return 0;
}

static int jw__ingame_perf_match_option(
        const jw_ipc_performance_domain_status *domain,
        const jw_perf_option *options,
        int count) {
    if (!domain || !options || count <= 0) {
        return 0;
    }
    int freq = domain->set_freq >= 0 ? domain->set_freq : domain->current_freq;
    for (int i = 0; i < count; i++) {
        if (strcmp(domain->governor, options[i].governor) != 0) {
            continue;
        }
        if (options[i].frequency < 0 ||
            (strcmp(options[i].governor, "userspace") == 0 &&
             options[i].frequency == freq)) {
            return i;
        }
    }
    return 0;
}

static void jw__ingame_perf_sync_indices(jw_ingame_state *state) {
    if (!state || !state->perf_ready) {
        return;
    }
    const char *shown = state->perf.session_override
        ? state->perf.session_profile : state->perf.active_profile;
    jw_platform_perf_profile parsed;
    if (jw_platform_parse_perf_profile(shown, &parsed) &&
        parsed == JW_PLATFORM_PERF_PROFILE_AUTO &&
        state->perf.active_profile[0]) {
        shown = state->perf.active_profile;
    }
    state->perf_profile_index = jw__ingame_perf_profile_index(shown);
    state->perf_cpu_index = jw__ingame_perf_match_option(
        &state->perf.domains[JW_PLATFORM_PERF_DOMAIN_CPU],
        kCpuPerfOptions, JW_CPU_PERF_OPTION_COUNT);
    state->perf_gpu_index = jw__ingame_perf_match_option(
        &state->perf.domains[JW_PLATFORM_PERF_DOMAIN_GPU],
        kGpuPerfOptions, JW_GPU_PERF_OPTION_COUNT);
    state->perf_dmc_index = jw__ingame_perf_match_option(
        &state->perf.domains[JW_PLATFORM_PERF_DOMAIN_DMC],
        kDmcPerfOptions, JW_DMC_PERF_OPTION_COUNT);
}

static void jw__ingame_perf_refresh(const char *socket_path,
                                    jw_ingame_state *state) {
    if (!state) {
        return;
    }
    state->perf_ready = false;
    if (jw_ipc_get_performance_status(socket_path, &state->perf,
                                      NULL, 0) == 0) {
        state->perf_ready = true;
        jw__ingame_perf_sync_indices(state);
    }
}

typedef struct {
    const char *socket_path;
} jw_ingame_shader_transport;

static int jw__ingame_shader_send(
    void *ctx, jw_shader_picker_operation operation, const char *path,
    jw_ra_shader_scope scope, jw_ipc_retroarch_shader_reply *reply) {
    jw_ingame_shader_transport *transport = ctx;
    if (!transport || !transport->socket_path) return -1;
    switch (operation) {
        case JW_SHADER_PICKER_GET:
            return jw_ipc_retroarch_shader_get(transport->socket_path, reply,
                                                NULL, 0);
        case JW_SHADER_PICKER_SET:
            return jw_ipc_retroarch_shader_set(transport->socket_path, path,
                                                reply, NULL, 0);
        case JW_SHADER_PICKER_RESTORE:
            return jw_ipc_retroarch_shader_restore(transport->socket_path, path,
                                                    reply, NULL, 0);
        case JW_SHADER_PICKER_CLEAR:
            return jw_ipc_retroarch_shader_clear(transport->socket_path, reply,
                                                  NULL, 0);
        case JW_SHADER_PICKER_SAVE:
            return jw_ipc_retroarch_shader_save(transport->socket_path, scope,
                                                 reply, NULL, 0);
        case JW_SHADER_PICKER_REMOVE:
            return jw_ipc_retroarch_shader_remove(transport->socket_path, scope,
                                                   reply, NULL, 0);
    }
    return -1;
}

static jw_shader_picker_transport jw__ingame_shader_transport(
    jw_ingame_shader_transport *ctx, const char *socket_path) {
    ctx->socket_path = socket_path;
    jw_shader_picker_transport transport = {
        .send = jw__ingame_shader_send,
        .ctx = ctx,
    };
    return transport;
}

static bool jw__ingame_shader_row_path(const char *recommended_root,
                                       const jw_shader_catalog_row *row,
                                       char *out, size_t out_size) {
    static const char prefix[] = "leaf-recommended/";
    if (!recommended_root || !row || !row->path ||
        strncmp(row->path, prefix, sizeof(prefix) - 1) != 0)
        return false;
    int n = snprintf(out, out_size, "%s/%s", recommended_root,
                     row->path + sizeof(prefix) - 1);
    return n > 0 && (size_t)n < out_size;
}

static void jw__ingame_shader_refresh(const char *socket_path,
                                      jw_ingame_state *state) {
    snprintf(state->shader_label, sizeof(state->shader_label), "%s",
             T("Unavailable"));
    jw_ingame_shader_transport ctx;
    jw_shader_picker_transport transport =
        jw__ingame_shader_transport(&ctx, socket_path);
    jw_shader_picker_state picker;
    jw_shader_picker_init(&picker);
    if (jw_shader_picker_probe(&picker, &transport) != JW_SHADER_PICKER_OK)
        return;
    if (!picker.current_path[0]) {
        snprintf(state->shader_label, sizeof(state->shader_label), "%s", T("Off"));
        return;
    }

    char *manifest = jw_retroarch_shader_manifest_path();
    char *root = jw_retroarch_recommended_shaders_dir();
    jw_shader_catalog catalog;
    bool found = false;
    bool loaded = manifest && root &&
                  jw_shader_catalog_load(manifest, state->session.system, &catalog);
    if (loaded) {
        char referenced[PATH_MAX] = "";
        (void)jw_shader_catalog_reference_target(picker.current_path,
                                                  referenced,
                                                  sizeof(referenced));
        for (size_t i = 0; i < catalog.count; i++) {
            char path[PATH_MAX];
            if (jw__ingame_shader_row_path(root, &catalog.rows[i], path,
                                           sizeof(path)) &&
                (strcmp(path, picker.current_path) == 0 ||
                 (referenced[0] && strcmp(path, referenced) == 0))) {
                snprintf(state->shader_label, sizeof(state->shader_label), "%s",
                         T(catalog.rows[i].display_name));
                found = true;
                break;
            }
        }
        jw_shader_catalog_free(&catalog);
    }
    if (!found)
        snprintf(state->shader_label, sizeof(state->shader_label), "%s", T("Custom"));
    free(manifest);
    free(root);
}

static const char *jw__ingame_perf_active_label(const jw_ingame_state *state) {
    if (!state || !state->perf_ready || !state->perf.supported) {
        return "Unavailable";
    }
    jw_platform_perf_profile profile;
    if (jw_platform_parse_perf_profile(state->perf.active_profile, &profile)) {
        return jw_platform_perf_profile_label(profile);
    }
    return "Unknown";
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

static void jw__perf_request_set_option(jw_platform_perf_request *request,
                                        jw_platform_perf_domain domain,
                                        const jw_perf_option *option) {
    if (!request || !option || domain < 0 || domain >= JW_PLATFORM_PERF_DOMAIN_COUNT) {
        return;
    }
    jw_platform_perf_domain_request *d = &request->domains[domain];
    snprintf(d->governor, sizeof(d->governor), "%s", option->governor);
    d->frequency = option->frequency;
}

static void jw__ingame_detail(const jw_ingame_state *state, int item,
                              char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';

    if (!state->session.active || !state->session_details_ready) {
        return;
    }

    if (item == JW_INGAME_CONTINUE && state->session.disk_count > 1) {
        int slot = state->session.disk_slot >= 0 ? state->session.disk_slot : 0;
        snprintf(out, out_size, T("Disk %d/%d"), slot + 1,
                 state->session.disk_count);
        return;
    }

    if (item == JW_INGAME_SAVE) {
        if (!state->session.savestate_supported) {
            snprintf(out, out_size, "%s", T("No states"));
        } else {
            jw__slot_name(state->save_slot, out, out_size);
        }
    } else if (item == JW_INGAME_LOAD) {
        if (!state->session.savestate_supported) {
            snprintf(out, out_size, "%s", T("No states"));
        } else if (state->load_count <= 0) {
            snprintf(out, out_size, "%s", T("No saves"));
        } else {
            int idx = state->load_index;
            if (idx < 0) idx = 0;
            if (idx >= state->load_count) idx = state->load_count - 1;
            if (state->load_entries[idx].is_latest) {
                snprintf(out, out_size, "%s", T("Latest"));
            } else {
                jw__slot_name(state->load_entries[idx].slot, out, out_size);
            }
        }
    } else if (item == JW_INGAME_PERF) {
        if (state->perf_ready && state->perf.supported && state->perf.soc_temp_c >= 0) {
            snprintf(out, out_size, "%s, %d C",
                     jw__ingame_perf_active_label(state),
                     state->perf.soc_temp_c);
        } else {
            snprintf(out, out_size, "%s", jw__ingame_perf_active_label(state));
        }
    } else if (item == JW_INGAME_SHADER) {
        snprintf(out, out_size, "%s", state->shader_label[0]
                                      ? state->shader_label : T("Unavailable"));
    }
}

/* ── Paused-frame capture ─────────────────────────────────────────────────
   The IGM background is the live paused RetroArch frame, grabbed straight from
   the DRM scanout with the on-device `kmsgrab` tool — no RetroArch screenshot,
   no PNG, no filesystem polling. It is captured synchronously *before* the menu
   window is mapped, so the very first visible frame already shows the game
   behind the menu (no late "pop-in"). The scanout is the panel-native portrait
   buffer (720x960, bytes B,G,R,X); we rotate it 90° CW into the menu's landscape
   space and force opaque alpha while uploading the texture. */
#define JW_KMS_PANEL_W 720
#define JW_KMS_PANEL_H 960

static int g_kms_crtc = -2; /* -2 = not probed, -1 = unavailable, else crtc id */

/* `kmsgrab` with no --crtc prints "Valid crtcs: <id> ...". That probe is slow
   (~2s), so it runs once at warm-up and is cached; falls back to the common MLP1
   crtc id if parsing fails. */
static int jw__kms_probe_crtc(void) {
    int crtc = -1;
    FILE *fp = popen("kmsgrab 2>&1", "r");
    if (fp) {
        char line[256];
        while (fgets(line, sizeof(line), fp)) {
            char *p = strstr(line, "Valid crtcs:");
            if (!p) {
                continue;
            }
            p += strlen("Valid crtcs:");
            while (*p && (*p < '0' || *p > '9')) {
                p++;
            }
            if (*p) {
                crtc = (int)strtol(p, NULL, 10);
            }
            break;
        }
        pclose(fp);
    }
    return crtc >= 0 ? crtc : 85; /* MLP1 DSI-1 default */
}

/* Grab one raw scanout frame (JW_KMS_PANEL_W*H*4 bytes) into buf. */
static bool jw__kms_read_raw(int crtc, uint8_t *buf, size_t buf_size) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "kmsgrab --crtc %d 2>/dev/null", crtc);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return false;
    }
    size_t got = 0;
    while (got < buf_size) {
        size_t n = fread(buf + got, 1, buf_size - got, fp);
        if (n == 0) {
            break;
        }
        got += n;
    }
    pclose(fp);
    return got >= buf_size;
}

static SDL_Texture *jw__load_blend_texture(const char *path) {
    SDL_Texture *tex = cat_load_image(path);
    if (tex) {
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    }
    return tex;
}

/* Capture the paused frame and build state->still_tex (960x720, opaque ARGB).
   Synchronous; call before showing the window so the still is ready in the first
   frame. On any failure still_tex stays NULL and the menu falls back to the flat
   dark background. */
static void jw__ingame_capture_still(jw_ingame_state *state) {
    if (g_kms_crtc == -2) {
        g_kms_crtc = jw__kms_probe_crtc();
    }
    if (g_kms_crtc < 0) {
        return;
    }

    const size_t raw_size = (size_t)JW_KMS_PANEL_W * JW_KMS_PANEL_H * 4;
    uint8_t *raw = malloc(raw_size);
    if (!raw) {
        return;
    }
    if (!jw__kms_read_raw(g_kms_crtc, raw, raw_size)) {
        free(raw);
        return;
    }

    /* Rotate 90° CW into landscape: out(H-1-y, x) = in(x, y). Keep B,G,R and
       force A=255 — ARGB8888 is B,G,R,A in little-endian memory, matching the
       scanout's B,G,R,X byte order. */
    const int iw = JW_KMS_PANEL_W, ih = JW_KMS_PANEL_H;
    const int ow = ih, oh = iw; /* 960x720 */
    uint8_t *out = malloc((size_t)ow * oh * 4);
    if (!out) {
        free(raw);
        return;
    }
    for (int y = 0; y < ih; y++) {
        const uint8_t *irow = raw + (size_t)y * iw * 4;
        for (int x = 0; x < iw; x++) {
            const uint8_t *ip = irow + (size_t)x * 4;
            uint8_t *op = out + ((size_t)x * ow + (ih - 1 - y)) * 4;
            op[0] = ip[0];
            op[1] = ip[1];
            op[2] = ip[2];
            op[3] = 255;
        }
    }
    free(raw);

    SDL_Texture *tex = SDL_CreateTexture(cat_get_renderer(),
                                         SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, ow, oh);
    if (tex) {
        SDL_UpdateTexture(tex, NULL, out, ow * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        if (state->still_tex) {
            SDL_DestroyTexture(state->still_tex);
        }
        state->still_tex = tex;
    }
    free(out);
}

/* Locate the savestate thumbnail PNG inside the active core namespace. */
static bool jw__slot_thumb_path(const jw_ingame_state *state, int slot,
                                char *out, size_t out_size) {
    char dir[PATH_MAX];
    if (!jw__ingame_states_dir(state, dir, sizeof(dir))) {
        return false;
    }
    return jw_ra_find_slot_thumb(dir, state->session.rom_path, slot,
                                 out, out_size);
}

/* Keep thumb_tex in sync with the selected slot while the cursor is on
   Save/Load. Attempts each slot once (thumb_tex stays NULL = placeholder). */
static void jw__ingame_update_thumb(jw_ingame_state *state) {
    bool on_slot = (state->list.cursor == JW_INGAME_SAVE ||
                    state->list.cursor == JW_INGAME_LOAD) &&
                   state->session.savestate_supported;
    int slot = on_slot ? jw__ingame_selected_slot(state) : INT_MIN;
    if (slot == INT_MIN) {
        if (state->thumb_tex) {
            SDL_DestroyTexture(state->thumb_tex);
            state->thumb_tex = NULL;
        }
        state->thumb_slot = INT_MIN;
        return;
    }
    if (state->thumb_slot == slot) {
        return; /* already attempted this slot */
    }
    if (state->thumb_tex) {
        SDL_DestroyTexture(state->thumb_tex);
        state->thumb_tex = NULL;
    }
    if (state->list.anim_active) {
        /* File decode is the only cold work in the main menu render loop. Keep
           the old slot cleared while focus moves, then load only the row where
           the 80 ms list animation actually settles. */
        state->thumb_slot = INT_MIN;
        cat_request_frame(); /* decode once more after the final animation frame */
        return;
    }
    state->thumb_slot = slot;
    char path[PATH_MAX];
    long long decode_start_ms = jw__monotonic_ms();
    bool found = jw__slot_thumb_path(state, slot, path, sizeof(path));
    if (found) {
        state->thumb_tex = jw__load_blend_texture(path); /* NULL -> placeholder */
    }
    long long decode_done_ms = jw__monotonic_ms();
    if (state->thumb_move_ms > 0) {
        jw_log_info(
            "in-game thumbnail timings: move_to_decode_ms=%lld decode_ms=%lld outcome=%s",
            decode_start_ms - state->thumb_move_ms,
            decode_done_ms - decode_start_ms,
            state->thumb_tex ? "texture" : "placeholder");
        state->thumb_move_ms = 0;
    }
}

static void jw__ingame_free_imagery(jw_ingame_state *state) {
    if (state->still_tex) {
        SDL_DestroyTexture(state->still_tex);
        state->still_tex = NULL;
    }
    if (state->thumb_tex) {
        SDL_DestroyTexture(state->thumb_tex);
        state->thumb_tex = NULL;
    }
    state->thumb_slot = INT_MIN;
}

/* Readability underlay for the in-game menu: a panel in the theme's own
   background colour behind the list, so theme->text keeps the launcher's
   guaranteed contrast over the paused game frame for every colour scheme —
   including light ones whose text is dark and would otherwise wash out against
   the dimmed still. Slightly transparent so a hint of the game still reads. */
#define JW_INGAME_UNDERLAY_ALPHA 200   /* slightly transparent; game still reads through */
static void jw__draw_ingame_underlay(SDL_Rect content, int top, int bottom) {
    if (bottom <= top) return;
    ap_color panel = cat_get_theme()->background;
    panel.a = JW_INGAME_UNDERLAY_ALPHA;
    cat_draw_rounded_rect(content.x + CAT_S(8), top,
                          content.w - CAT_S(16), bottom - top, CAT_S(14), panel);
}

/* The status bar for a page whose title sits on the header underlay (drawn
   from CAT_S(4) to content.y - CAT_S(4)). Centred in that band: at
   Catastrophe's default y the pill hangs past the underlay's bottom edge. */
static void jw__draw_ingame_header_status(cat_status_bar_opts *sb, SDL_Rect content) {
    int top    = CAT_S(4);
    int bottom = content.y - CAT_S(4);
    sb->use_y      = true;
    sb->y_position = top + (bottom - top - CAT_DS(CAT__PILL_SIZE)) / 2;
    cat_draw_status_bar(sb);
}

static void jw__draw_ingame_list_focus(int x, int y, int w, int h, void *user) {
    (void)user;
    int pill_h = h - CAT_S(4);
    cat_draw_pill(x, y + (h - pill_h) / 2, w, pill_h,
                  cat_get_theme()->highlight);
}

typedef struct {
    const jw_ingame_state *state;
    TTF_Font *body;
    TTF_Font *small;
    int label_x;
    int list_w;
    int item_w;
    int detail_x;
    int detail_w;
    bool preview_active;
} jw__ingame_menu_rows;

static void jw__draw_ingame_menu_item(int i, int ix, int iy, int iw, int ih,
                                      float focus, void *user) {
    (void)ix; (void)iw;
    jw__ingame_menu_rows *rows = user;
    ap_theme *theme = cat_get_theme();
    int body_h = TTF_FontHeight(rows->body);
    int pill_h = ih - CAT_S(4);
    if (pill_h < body_h + CAT_S(2)) pill_h = body_h + CAT_S(2);
    int text_y = iy + (ih - pill_h) / 2 + (pill_h - body_h) / 2;
    ap_color text = cat_draw_color_lerp(theme->text,
                                         theme->highlighted_text, focus);
    ap_color hint = cat_draw_color_lerp(theme->hint,
                                         theme->highlighted_text, focus);

    char detail[96];
    jw__ingame_detail(rows->state, i, detail, sizeof(detail));
    bool show_detail = !rows->preview_active && detail[0] &&
                       rows->detail_w > CAT_S(40);
    int label_w = show_detail ? rows->detail_x - rows->label_x - CAT_S(10)
                : (rows->preview_active ? rows->item_w - CAT_S(10)
                                        : rows->list_w - CAT_S(8));
    const char *label = (i == JW_INGAME_QUIT)
                      ? (rows->state->quit_save ? T("Save & Quit") : T("Quit"))
                      : T(kInGameItems[i]);
    cat_draw_text_ellipsized(rows->body, label, rows->label_x, text_y,
                             text, label_w);
    if (show_detail) {
        cat_draw_text_ellipsized(rows->small, detail, rows->detail_x,
                                 text_y + CAT_S(2), hint, rows->detail_w);
    }
}

static void jw__render_ingame_menu(const jw_ingame_state *state) {
    ap_theme *theme     = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small     = cat_get_font(CAT_FONT_SMALL);

    cat_status_bar_opts sb = state->status_bar;

    cat_clear_screen();
    /* Allium-style backdrop: the paused frame captured at open, dimmed by a
       scrim. The still is ready before the window is shown, so it is drawn at
       full opacity from the first frame (no fade, no pop-in). Falls back to the
       flat dark background when the capture failed. */
    if (state->still_tex) {
        int sw = cat_get_screen_width();
        int sh = cat_get_screen_height();
        cat_draw_image(state->still_tex, 0, 0, sw, sh);
        ap_color scrim = { 0, 0, 0, 150 };
        cat_draw_rect(0, 0, sw, sh, scrim);
    }
    SDL_Rect content = cat_get_content_rect(true, true, false);
    jw__draw_ingame_underlay(content, CAT_S(4), content.y - CAT_S(4));   /* behind the title */
    cat_draw_screen_title(state->game_title[0] ? state->game_title : T("Game"), &sb);
    jw__draw_ingame_header_status(&sb, content);

    int pad       = CAT_S(24);
    int x         = content.x + pad;
    int right     = content.x + content.w - pad;
    int list_w    = right - x;
    int body_h    = TTF_FontHeight(body_font);
    int small_h   = TTF_FontHeight(small);
    int line_gap  = CAT_S(5);
    int meta_y    = content.y + CAT_S(12);
    bool show_command = state->session.active && !state->session.command_ok;
    int top_y     = meta_y + small_h + CAT_S(12);
    int status_h  = (state->show_hints && state->status[0])
                  ? small_h + CAT_S(12) : 0;
    int bottom_y  = content.y + content.h - CAT_S(10) - status_h;
    int item_h    = body_h + CAT_S(12);
    int min_item_h = body_h + CAT_S(4);
    int item_w    = list_w * 62 / 100;
    int detail_x  = x + item_w + CAT_S(14);
    int detail_w  = right - detail_x;
    /* On Save/Load the right column shows a slot thumbnail preview instead of
       the inline slot text. */
    bool preview_active = (state->list.cursor == JW_INGAME_SAVE ||
                           state->list.cursor == JW_INGAME_LOAD) &&
                          state->session.savestate_supported;
    if (show_command) {
        top_y += small_h + line_gap;
    }
    if (item_h * JW_INGAME_COUNT > bottom_y - top_y && bottom_y > top_y) {
        item_h = (bottom_y - top_y) / JW_INGAME_COUNT;
        if (item_h < min_item_h) {
            item_h = min_item_h;
        }
    }
    int pill_w = list_w;

    jw__draw_ingame_underlay(content, content.y + CAT_S(4), bottom_y);

    const char *session_line = state->console_title[0]
                             ? state->console_title
                             : (state->session.active ? "" : "No active RetroArch session");
    if (session_line[0]) {
        cat_draw_text_ellipsized(small, session_line, x, meta_y,
                                 theme->hint, list_w);
    }

    if (show_command) {
        char command_line[128];
        snprintf(command_line, sizeof(command_line), T("Command: %s"),
                 state->session.command_result);
        cat_draw_text_ellipsized(small, command_line, x,
                                 meta_y + small_h + line_gap,
                                 theme->hint, list_w);
    }

    jw__ingame_menu_rows rows = {
        .state = state,
        .body = body_font,
        .small = small,
        .label_x = x,
        .list_w = list_w,
        .item_w = item_w,
        .detail_x = detail_x,
        .detail_w = detail_w,
        .preview_active = preview_active,
    };
    cat_draw_list_pane_layered(x - CAT_S(10), top_y, pill_w,
                               JW_INGAME_COUNT * item_h,
                               JW_INGAME_COUNT, &state->list, item_h,
                               jw__draw_ingame_list_focus,
                               jw__draw_ingame_menu_item, &rows);

    /* Slot thumbnail preview (Save/Load): the saved image in the selected slot,
       or a placeholder when the slot is empty. */
    if (preview_active) {
        int pv_x   = detail_x;
        int pv_w   = right - pv_x;
        int avail  = bottom_y - top_y;
        int cap_h  = small_h * 2 + CAT_S(10); /* caption + recency sub-line */
        int pv_h   = pv_w * 3 / 4;
        if (pv_h > avail - cap_h) {
            pv_h = avail - cap_h;
        }
        /* Align with the top of the menu list rather than centering, so the
           preview sits high next to the first rows. */
        int pv_y = top_y;
        if (pv_w > CAT_S(60) && pv_h > CAT_S(40)) {
            ap_color frame = { 0, 0, 0, 180 };
            cat_draw_rect(pv_x, pv_y, pv_w, pv_h, frame);
            if (state->thumb_tex) {
                cat_draw_image(state->thumb_tex, pv_x, pv_y, pv_w, pv_h);
            } else {
                cat_draw_text_ellipsized(small, T("No save in this slot"),
                                         pv_x + CAT_S(8),
                                         pv_y + pv_h / 2 - small_h / 2,
                                         theme->hint, pv_w - CAT_S(16));
            }
            char cap[64];
            char cap_sub[64];
            cap[0] = cap_sub[0] = '\0';
            if (state->list.cursor == JW_INGAME_SAVE) {
                jw__save_caption(state, cap, sizeof(cap));
            } else {
                jw__load_caption(state, cap, sizeof(cap), cap_sub, sizeof(cap_sub));
            }
            int cap_y = pv_y + pv_h + CAT_S(4);
            cat_draw_text_ellipsized(small, cap, pv_x, cap_y, theme->text, pv_w);
            if (cap_sub[0]) {
                cat_draw_text_ellipsized(small, cap_sub, pv_x,
                                         cap_y + small_h + CAT_S(2),
                                         theme->hint, pv_w);
            }
        }
    }

    /* On the "Latest" Load entry, explain what it is and how to act on it. */
    const char *status_text = state->status;
    char latest_hint[112];
    bool on_latest = (state->list.cursor == JW_INGAME_LOAD &&
                      state->session.savestate_supported &&
                      state->load_count > 0 &&
                      state->load_entries[state->load_index].is_latest);
    if (!status_text[0] && on_latest) {
        snprintf(latest_hint, sizeof(latest_hint),
                 "Save & Quit quicksave \xe2\x80\x94 A resumes, Y keeps it as a slot");
        status_text = latest_hint;
    }
    if (state->show_hints && status_text[0]) {
        int status_y = content.y + content.h - small_h - CAT_S(10);
        cat_draw_text_ellipsized(small, status_text, x, status_y,
                                 theme->hint, list_w);
    }

    if (state->show_hints) {
        cat_set_footer_bg_opacity(JW_INGAME_UNDERLAY_ALPHA);   /* match the underlay panels */
        if (on_latest) {
            cat_footer_item footer[] = {
                { CAT_BTN_LEFT, "Browse", false, JW_HINT_DEVICE("\xe2\x86\x90\xe2\x86\x92", "\xe2\x86\x90\xe2\x86\x92") },
                { CAT_BTN_Y,  "Keep",   false, JW_HINT("Y") },
                { CAT_BTN_B,  "Resume", true,  JW_HINT("B") },
                { CAT_BTN_A,  "Load",   true,  JW_HINT("A") },
            };
            jw__menu_footer(footer, 4);
        } else {
            cat_footer_item footer[] = {
                { CAT_BTN_UP, "Move", false, JW_HINT_DEVICE("\xe2\x86\x91\xe2\x86\x93", "\xe2\x86\x91\xe2\x86\x93") },
                { CAT_BTN_LEFT, "Adjust", false, JW_HINT_DEVICE("\xe2\x86\x90\xe2\x86\x92", "\xe2\x86\x90\xe2\x86\x92") },
                { CAT_BTN_B,  "Resume", true,  JW_HINT("B") },
                { CAT_BTN_A,  "OK",     true,  JW_HINT("A") },
            };
            jw__menu_footer(footer, 4);
        }
    }
    cat_present();
}

static const char *jw__ingame_perf_profile_label_for_index(int index) {
    if (index < 0 || index >= JW_INGAME_PERF_PROFILE_COUNT) {
        index = 0;
    }
    return jw_platform_perf_profile_label(kInGamePerfProfiles[index]);
}

static void jw__ingame_perf_apply_profile(const char *socket_path,
                                          jw_ingame_state *state) {
    if (!state) {
        return;
    }
    if (!state->perf_ready || !state->perf.supported) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "Performance unavailable");
        return;
    }
    jw_platform_perf_profile profile = kInGamePerfProfiles[state->perf_profile_index];
    if (profile == JW_PLATFORM_PERF_PROFILE_CUSTOM) {
        jw_platform_perf_request request;
        jw__perf_request_init(&request);
        jw__perf_request_set_option(&request, JW_PLATFORM_PERF_DOMAIN_CPU,
                                    &kCpuPerfOptions[state->perf_cpu_index]);
        jw__perf_request_set_option(&request, JW_PLATFORM_PERF_DOMAIN_GPU,
                                    &kGpuPerfOptions[state->perf_gpu_index]);
        jw__perf_request_set_option(&request, JW_PLATFORM_PERF_DOMAIN_DMC,
                                    &kDmcPerfOptions[state->perf_dmc_index]);
        if (jw_ipc_set_performance_custom(socket_path, &request,
                                          state->status,
                                          sizeof(state->status)) == 0) {
            jw__ingame_perf_refresh(socket_path, state);
        }
        return;
    }
    if (jw_ipc_set_performance_profile(socket_path, "session",
            jw_platform_perf_profile_name(profile),
            state->status, sizeof(state->status)) == 0) {
        jw__ingame_perf_refresh(socket_path, state);
    }
}

static void jw__ingame_perf_apply_custom(const char *socket_path,
                                         jw_ingame_state *state) {
    if (!state) {
        return;
    }
    state->perf_profile_index =
        jw__ingame_perf_profile_index(jw_platform_perf_profile_name(
            JW_PLATFORM_PERF_PROFILE_CUSTOM));
    jw__ingame_perf_apply_profile(socket_path, state);
}

static void jw__ingame_perf_reset(const char *socket_path,
                                  jw_ingame_state *state) {
    if (!state) {
        return;
    }
    if (jw_ipc_reset_performance_session(socket_path, state->status,
                                         sizeof(state->status)) == 0) {
        jw__ingame_perf_refresh(socket_path, state);
    }
}

/* Returns true when the press changed a value, so the caller can avoid ticking
   "nav" on the Reset row or on a device with no performance control. */
static bool jw__ingame_perf_adjust(const char *socket_path,
                                   jw_ingame_state *state,
                                   cat_list_state *list,
                                   int delta) {
    if (!state || !list) {
        return false;
    }
    if (!state->perf_ready || !state->perf.supported) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "Performance unavailable");
        return false;
    }
    switch (list->cursor) {
        case JW_INGAME_PERF_PROFILE:
            state->perf_profile_index =
                (state->perf_profile_index + delta + JW_INGAME_PERF_PROFILE_COUNT) %
                JW_INGAME_PERF_PROFILE_COUNT;
            jw__ingame_perf_apply_profile(socket_path, state);
            return true;
        case JW_INGAME_PERF_CPU:
            state->perf_cpu_index =
                (state->perf_cpu_index + delta + JW_CPU_PERF_OPTION_COUNT) %
                JW_CPU_PERF_OPTION_COUNT;
            jw__ingame_perf_apply_custom(socket_path, state);
            return true;
        case JW_INGAME_PERF_GPU:
            state->perf_gpu_index =
                (state->perf_gpu_index + delta + JW_GPU_PERF_OPTION_COUNT) %
                JW_GPU_PERF_OPTION_COUNT;
            jw__ingame_perf_apply_custom(socket_path, state);
            return true;
        case JW_INGAME_PERF_DMC:
            state->perf_dmc_index =
                (state->perf_dmc_index + delta + JW_DMC_PERF_OPTION_COUNT) %
                JW_DMC_PERF_OPTION_COUNT;
            jw__ingame_perf_apply_custom(socket_path, state);
            return true;
        default:
            return false;   /* Reset row adjusts nothing */
    }
}

typedef struct {
    TTF_Font *body;
    TTF_Font *small;
    const char **labels;
    const char **values;
    int label_x;
    int detail_x;
    int detail_w;
} jw__ingame_perf_rows;

static void jw__draw_ingame_perf_item(int i, int ix, int iy, int iw, int ih,
                                      float focus, void *user) {
    (void)ix; (void)iw;
    jw__ingame_perf_rows *rows = user;
    ap_theme *theme = cat_get_theme();
    int body_h = TTF_FontHeight(rows->body);
    int pill_h = ih - CAT_S(4);
    int text_y = iy + (ih - pill_h) / 2 + (pill_h - body_h) / 2;
    ap_color text = cat_draw_color_lerp(theme->text,
                                         theme->highlighted_text, focus);
    ap_color hint = cat_draw_color_lerp(theme->hint,
                                         theme->highlighted_text, focus);
    cat_draw_text_ellipsized(rows->body, rows->labels[i], rows->label_x,
                             text_y, text,
                             rows->detail_x - rows->label_x - CAT_S(10));
    cat_draw_text_ellipsized(rows->small, rows->values[i], rows->detail_x,
                             text_y + CAT_S(2), hint, rows->detail_w);
}

static void jw__render_ingame_performance(const jw_ingame_state *state,
                                          const cat_list_state *list) {
    ap_theme *theme     = cat_get_theme();
    TTF_Font *body_font = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small     = cat_get_font(CAT_FONT_SMALL);
    cat_status_bar_opts sb = state->status_bar;

    cat_clear_screen();
    if (state->still_tex) {
        int sw = cat_get_screen_width();
        int sh = cat_get_screen_height();
        cat_draw_image(state->still_tex, 0, 0, sw, sh);
        ap_color scrim = { 0, 0, 0, 165 };
        cat_draw_rect(0, 0, sw, sh, scrim);
    }
    SDL_Rect content = cat_get_content_rect(true, true, false);
    jw__draw_ingame_underlay(content, CAT_S(4), content.y - CAT_S(4));   /* behind the title */
    cat_draw_screen_title(T("Performance"), &sb);
    jw__draw_ingame_header_status(&sb, content);

    int pad = CAT_S(24);
    int x = content.x + pad;
    int right = content.x + content.w - pad;
    int list_w = right - x;
    int body_h = TTF_FontHeight(body_font);
    int small_h = TTF_FontHeight(small);
    int item_h = body_h + CAT_S(12);
    int top_y = content.y + CAT_S(18);
    int detail_x = x + list_w * 48 / 100;
    int detail_w = right - detail_x;

    jw__draw_ingame_underlay(content, content.y + CAT_S(4),
                             content.y + CAT_S(18) + small_h + CAT_S(12)
                                 + JW_INGAME_PERF_ROWS * item_h + CAT_S(8));

    const char *labels[JW_INGAME_PERF_ROWS] = {
        T("Profile"), T("CPU"), T("GPU"), T("DMC"), T("Reset Override"),
    };
    const char *values[JW_INGAME_PERF_ROWS] = {
        jw__ingame_perf_profile_label_for_index(state->perf_profile_index),
        T(kCpuPerfOptions[state->perf_cpu_index].label),
        T(kGpuPerfOptions[state->perf_gpu_index].label),
        T(kDmcPerfOptions[state->perf_dmc_index].label),
        state->perf.session_override ? T("Active") : T("None"),
    };

    if (state->perf_ready && state->perf.supported) {
        char line[128];
        if (state->perf.soc_temp_c >= 0) {
            snprintf(line, sizeof(line), T("Active: %s  Temp: %d C"),
                     jw__ingame_perf_active_label(state),
                     state->perf.soc_temp_c);
        } else {
            snprintf(line, sizeof(line), T("Active: %s"),
                     jw__ingame_perf_active_label(state));
        }
        cat_draw_text_ellipsized(small, line, x, top_y,
                                 theme->hint, list_w);
        top_y += small_h + CAT_S(12);
    } else {
        cat_draw_text_ellipsized(small, T("Performance unavailable"), x, top_y,
                                 theme->hint, list_w);
        top_y += small_h + CAT_S(12);
    }

    jw__ingame_perf_rows rows = {
        .body = body_font,
        .small = small,
        .labels = labels,
        .values = values,
        .label_x = x,
        .detail_x = detail_x,
        .detail_w = detail_w,
    };
    cat_draw_list_pane_layered(x - CAT_S(10), top_y, list_w,
                               JW_INGAME_PERF_ROWS * item_h,
                               JW_INGAME_PERF_ROWS, list, item_h,
                               jw__draw_ingame_list_focus,
                               jw__draw_ingame_perf_item, &rows);

    if (state->show_hints && state->status[0]) {
        int y = content.y + content.h - small_h - CAT_S(10);
        cat_draw_text_ellipsized(small, state->status, x, y, theme->hint, list_w);
    }
    if (state->show_hints) {
        cat_set_footer_bg_opacity(JW_INGAME_UNDERLAY_ALPHA);   /* match the underlay panels */
        cat_footer_item footer[] = {
            { CAT_BTN_LEFT, "Adjust", false, JW_HINT_DEVICE("\xe2\x86\x90\xe2\x86\x92", "\xe2\x86\x90\xe2\x86\x92") },
            { CAT_BTN_B,  "Back", true, JW_HINT("B") },
            { CAT_BTN_A,  "Apply", true, JW_HINT("A") },
        };
        jw__menu_footer(footer, 3);
    }
    cat_present();
}

static void jw__ingame_show_performance(const char *socket_path,
                                        jw_ingame_state *state) {
    if (!state) {
        return;
    }
    jw__ingame_perf_refresh(socket_path, state);
    cat_list_state list;
    cat_list_state_init(&list, JW_INGAME_PERF_ROWS);
    state->status[0] = '\0';

    bool running = true;
    while (running && !g_hide_requested) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            switch (ev.button) {
                case CAT_BTN_UP:
                    cat_list_state_move(&list, -1, JW_INGAME_PERF_ROWS);
                    break;
                case CAT_BTN_DOWN:
                    cat_list_state_move(&list, +1, JW_INGAME_PERF_ROWS);
                    break;
                case CAT_BTN_LEFT:
                    jw__haptic(socket_path,
                        jw__ingame_perf_adjust(socket_path, state, &list, -1)
                            ? "nav" : "blocked");
                    break;
                case CAT_BTN_RIGHT:
                    jw__haptic(socket_path,
                        jw__ingame_perf_adjust(socket_path, state, &list, +1)
                            ? "nav" : "blocked");
                    break;
                case CAT_BTN_A:
                case CAT_BTN_START:
                    jw__haptic(socket_path, "commit");
                    if (list.cursor == JW_INGAME_PERF_RESET) {
                        jw__ingame_perf_reset(socket_path, state);
                    } else if (list.cursor == JW_INGAME_PERF_PROFILE) {
                        jw__ingame_perf_apply_profile(socket_path, state);
                    } else {
                        jw__ingame_perf_apply_custom(socket_path, state);
                    }
                    break;
                case CAT_BTN_B:
                    jw__haptic(socket_path, "select");
                    running = false;
                    break;
                default:
                    break;
            }
        }
        jw__render_ingame_performance(state, &list);
    }
}

static void jw__ingame_continue(const char *socket_path,
                                jw_ingame_state *state, bool *running);
static void jw__ingame_refresh(const char *socket_path,
                               jw_ingame_state *state);

/* Global shader writes need the ownership-safe Fugazi resolver in the assembled
   Leaf build, so that a global save Fugazi did not make stays recoverable.
   Fugazi 0.2.0 carries it. Return this to false if Leaf ever assembles an older
   Fugazi, because the picker would then create a conflict nothing can resolve. */
#define JW_FUGAZI_RESOLVER_ASSEMBLED true

typedef struct {
    jw_shader_catalog catalog;
    jw_shader_picker_state picker;
    cat_list_state list;
    char *recommended_root;
    bool catalog_valid;
    bool custom_row;
    int applied_cursor;
    int scope_index;
    long long present_ms;
    char status[256];
} jw_ingame_shader_view;

static jw_ra_shader_scope jw__shader_scope_from_index(int index);
static const char *jw__shader_scope_label(int index);

static int jw__shader_item_count(const jw_ingame_shader_view *view) {
    return 1 + (int)view->catalog.count + (view->custom_row ? 1 : 0) + 1;
}

static int jw__shader_custom_index(const jw_ingame_shader_view *view) {
    return view->custom_row ? 1 + (int)view->catalog.count : -1;
}

static int jw__shader_advanced_index(const jw_ingame_shader_view *view) {
    return jw__shader_item_count(view) - 1;
}

static const jw_shader_catalog_row *jw__shader_item_row(
    const jw_ingame_shader_view *view, int index) {
    if (index <= 0 || index > (int)view->catalog.count) return NULL;
    return &view->catalog.rows[index - 1];
}

static const char *jw__shader_item_label(const jw_ingame_shader_view *view,
                                         int index) {
    if (index == 0) return T("Off");
    const jw_shader_catalog_row *row = jw__shader_item_row(view, index);
    /* Catalog text is data from shaders/manifest.json, not a literal in this
       repo, so it is translated here at draw the way a row label would be. The
       keys reach the .pot through internal/retroarch/shader_strings.c. */
    if (row) return T(row->display_name);
    if (index == jw__shader_custom_index(view)) return T("Save current shader…");
    return T("Advanced RetroArch menu");
}

static bool jw__shader_item_path(const jw_ingame_shader_view *view, int index,
                                 char *out, size_t out_size) {
    if (index == 0) {
        if (out_size) out[0] = '\0';
        return true;
    }
    const jw_shader_catalog_row *row = jw__shader_item_row(view, index);
    if (row)
        return jw__ingame_shader_row_path(view->recommended_root, row,
                                          out, out_size);
    if (index == jw__shader_custom_index(view)) {
        int n = snprintf(out, out_size, "%s", view->picker.original_path);
        return n >= 0 && (size_t)n < out_size;
    }
    return false;
}

static void jw__shader_result_status(jw_shader_picker_result result,
                                     const char *scope,
                                     char *out, size_t out_size) {
    const char *message = NULL;
    switch (result) {
        case JW_SHADER_PICKER_UNAVAILABLE:
            message = T("RetroArch is not available. Shader unchanged.");
            break;
        case JW_SHADER_PICKER_UNSUPPORTED_BUILD:
            message = T("This RetroArch build does not support Leaf's shader picker.");
            break;
        case JW_SHADER_PICKER_MISSING:
            message = T("Leaf's shader files are incomplete. Update or reinstall Leaf.");
            break;
        case JW_SHADER_PICKER_UNSUPPORTED_TYPE:
            message = T("Leaf's in-game picker supports GLSL presets (.glslp).");
            break;
        case JW_SHADER_PICKER_APPLY_FAILED:
            message = T("This shader could not compile or link on this video driver. Previous shader restored.");
            break;
        case JW_SHADER_PICKER_UNKNOWN_STATE:
            message = T("RetroArch did not confirm the shader. Reopen Shader or restart the game.");
            break;
        case JW_SHADER_PICKER_RESTORE_FAILED:
            message = T("Shader failed and the previous shader could not be restored. Reopen the game.");
            break;
        case JW_SHADER_PICKER_SAVE_FAILED:
            snprintf(out, out_size,
                     T("Shader is active for this session, but could not be saved for %s."),
                     scope ? scope : T("that scope"));
            return;
        case JW_SHADER_PICKER_REMOVE_ABSENT:
            message = T("No saved shader exists at that scope.");
            break;
        case JW_SHADER_PICKER_REMOVE_FAILED:
            message = T("The saved shader could not be removed.");
            break;
        case JW_SHADER_PICKER_OK:
            message = "";
            break;
    }
    snprintf(out, out_size, "%s", message ? message : T("Shader operation failed."));
}

static bool jw__shader_confirmation(const char *message,
                                    const char *confirm_label) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = T("Cancel"), .is_confirm = false },
        { .button = CAT_BTN_A, .label = confirm_label, .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
        .cancel_requested = jw__menu_hide_requested,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

static void jw__shader_notice(const char *message) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = T("OK"), .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 1,
        .cancel_requested = jw__menu_hide_requested,
    };
    cat_confirm_result result;
    (void)cat_confirmation(&opts, &result);
}

static bool jw__shader_offer_settings(const char *message) {
    return jw__shader_confirmation(message, T("RetroArch Settings"));
}

static jw_shader_picker_result jw__shader_apply_selection(
    jw_ingame_shader_view *view,
    const jw_shader_picker_transport *transport,
    int cursor) {
    char path[PATH_MAX];
    jw_shader_picker_result result =
        jw__shader_item_path(view, cursor, path, sizeof(path))
        ? jw_shader_picker_apply(&view->picker, transport,
                                 path[0] ? path : NULL)
        : JW_SHADER_PICKER_MISSING;
    if (result == JW_SHADER_PICKER_OK) {
        view->applied_cursor = cursor;
        view->status[0] = '\0';
    } else {
        jw__shader_result_status(result, NULL, view->status,
                                 sizeof(view->status));
        if (result != JW_SHADER_PICKER_UNAVAILABLE) {
            cat_list_state_jump(&view->list, view->applied_cursor,
                                jw__shader_item_count(view));
        }
    }
    return result;
}

typedef struct {
    const jw_ingame_shader_view *view;
    TTF_Font *body;
} jw__ingame_shader_rows;

static void jw__draw_ingame_shader_item(int i, int ix, int iy, int iw, int ih,
                                        float focus, void *user) {
    jw__ingame_shader_rows *rows = user;
    ap_theme *theme = cat_get_theme();
    ap_color text = cat_draw_color_lerp(theme->text,
                                         theme->highlighted_text, focus);
    cat_draw_text_ellipsized(rows->body,
                             jw__shader_item_label(rows->view, i),
                             ix + CAT_S(10), iy + CAT_S(3), text,
                             iw - CAT_S(20));
    (void)ih;
}

static void jw__render_ingame_shader(const jw_ingame_state *state,
                                     const jw_ingame_shader_view *view) {
    ap_theme *theme = cat_get_theme();
    TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    cat_status_bar_opts sb = state->status_bar;

    cat_clear_screen();
    if (state->still_tex) {
        int sw = cat_get_screen_width(), sh = cat_get_screen_height();
        cat_draw_image(state->still_tex, 0, 0, sw, sh);
        ap_color scrim = { 0, 0, 0, 165 };
        cat_draw_rect(0, 0, sw, sh, scrim);
    }
    SDL_Rect content = cat_get_content_rect(true, true, false);
    jw__draw_ingame_underlay(content, CAT_S(4), content.y - CAT_S(4));
    cat_draw_screen_title(T("Shader"), &sb);
    jw__draw_ingame_header_status(&sb, content);
    jw__draw_ingame_underlay(content, content.y + CAT_S(4),
                             content.y + content.h - CAT_S(8));

    int pad = CAT_S(24);
    int x = content.x + pad;
    int right = content.x + content.w - pad;
    int list_w = (right - x) * 48 / 100;
    int detail_x = x + list_w + CAT_S(18);
    int detail_w = right - detail_x;
    int body_h = TTF_FontHeight(body);
    int small_h = TTF_FontHeight(small);
    int item_h = body_h + CAT_S(10);
    int top = content.y + CAT_S(16);
    int bottom = content.y + content.h - CAT_S(18) - small_h;
    int count = jw__shader_item_count(view);

    jw__ingame_shader_rows rows = { .view = view, .body = body };
    cat_draw_list_pane_layered(x - CAT_S(10), top, list_w, bottom - top,
                               count, &view->list, item_h,
                               jw__draw_ingame_list_focus,
                               jw__draw_ingame_shader_item, &rows);

    const jw_shader_catalog_row *selected =
        jw__shader_item_row(view, view->list.cursor);
    const char *description = NULL;
    const char *constraint = NULL;
    if (selected) {
        description = T(selected->description);
        if (selected->constraint_count > 0) {
            constraint = T(selected->constraints[0]);
            /* The active-system filter already enforces simple "GBA only"
               constraints. Prefer the safety/performance caveat the device
               matrix asks the user to see when one is present. */
            for (size_t i = 0; i < selected->constraint_count; i++) {
                if (strstr(selected->constraints[i], "BFI")) {
                    constraint = T(selected->constraints[i]);
                    break;
                }
            }
        }
    } else if (view->list.cursor == 0) {
        description = T("Remove the saved preset at the selected scope and turn shaders off.");
        constraint = T("A broader preset may apply after you reload the game.");
    } else if (view->list.cursor == jw__shader_custom_index(view)) {
        description = T("Choose where RetroArch should load this shader automatically.");
        constraint = T("Use Advanced to edit it in RetroArch.");
    } else {
        description = T("Edit the current shader in RetroArch.");
        constraint = T("Save your edits in RetroArch before you leave.");
    }
    bool show_scope = view->list.cursor != jw__shader_advanced_index(view);
    char scope_hint[128] = "";
    if (show_scope)
        snprintf(scope_hint, sizeof(scope_hint), "%s: %s",
                 T("Shader scope"), jw__shader_scope_label(view->scope_index));
    int dy = top;
    if (description && description[0]) {
        dy += cat_draw_text_wrapped(body, description, detail_x, dy, detail_w,
                                    theme->text, CAT_ALIGN_LEFT);
        dy += CAT_S(8);
    }
    /* Keep the selected scope as part of the detail copy: it must use the
       same readable body face/color and follow the description naturally. */
    if (scope_hint[0]) {
        dy += CAT_S(8);
        dy += cat_draw_text_wrapped(body, scope_hint, detail_x, dy, detail_w,
                                    theme->text, CAT_ALIGN_LEFT);
    }
    if (constraint && constraint[0]) {
        dy += CAT_S(8);
        cat_draw_text_wrapped(small, constraint, detail_x, dy, detail_w,
                              theme->hint, CAT_ALIGN_LEFT);
    }

    const char *status = view->status;
    if (!status[0] && !view->catalog_valid)
        status = T("Recommended shaders are unavailable. Update or reinstall Leaf.");
    if (status[0])
        cat_draw_text_wrapped(small, status, x,
                              bottom - small_h * 2 - CAT_S(4), right - x,
                              theme->hint, CAT_ALIGN_LEFT);

    if (state->show_hints) {
        cat_set_footer_bg_opacity(JW_INGAME_UNDERLAY_ALPHA);
        cat_footer_item footer[] = {
            { CAT_BTN_UP, "Move", false, JW_HINT_DEVICE("\xe2\x86\x91\xe2\x86\x93", "\xe2\x86\x91\xe2\x86\x93") },
            { CAT_BTN_B, "Back", true, JW_HINT("B") },
            { CAT_BTN_A, "Select", true, JW_HINT("A") },
            { CAT_BTN_LEFT, "Scope", false, JW_HINT_DEVICE("\xe2\x86\x90\xe2\x86\x92", "\xe2\x86\x90\xe2\x86\x92") },
        };
        jw__menu_footer(footer, show_scope ? 4 : 3);
    }
    cat_present();
}

static jw_ra_shader_scope jw__shader_scope_from_index(int index) {
    static const jw_ra_shader_scope scopes[] = {
        JW_RA_SHADER_SCOPE_GAME,
        JW_RA_SHADER_SCOPE_PARENT,
        JW_RA_SHADER_SCOPE_GLOBAL,
    };
    return index >= 0 && index < 3 ? scopes[index] : JW_RA_SHADER_SCOPE_GAME;
}

static const char *jw__shader_scope_label(int index) {
    static const char *labels[] = {
        JW_UI("This game"), JW_UI("This folder"), JW_UI("All RetroArch"),
    };
    return index >= 0 && index < 3 ? T(labels[index]) : T("that scope");
}

static int jw__shader_scope_cycle(int index, int direction) {
    int next = index;
    for (int attempts = 0; attempts < 3; attempts++) {
        next = (next + direction + 3) % 3;
        if (jw_shader_picker_scope_enabled(
                jw__shader_scope_from_index(next),
                JW_FUGAZI_RESOLVER_ASSEMBLED))
            return next;
    }
    return index;
}

static void jw__shader_resume(const char *socket_path, jw_ingame_state *state,
                              bool *menu_running) {
    jw__ingame_continue(socket_path, state, menu_running);
}

static void jw__shader_save_choice(
    const char *socket_path, jw_ingame_state *state,
    jw_ingame_shader_view *view, const jw_shader_picker_transport *transport,
    bool *menu_running) {
    jw_ra_shader_scope scope = jw__shader_scope_from_index(view->scope_index);
    if (scope == JW_RA_SHADER_SCOPE_GLOBAL &&
        !jw__shader_confirmation(
            T("This replaces the shader for every RetroArch game and can replace Fugazi. Continue?"),
            T("Save globally"))) {
        return;
    }
    jw_shader_picker_result result = jw_shader_picker_save(transport, scope);
    if (result != JW_SHADER_PICKER_OK) {
        jw__shader_result_status(result, jw__shader_scope_label(view->scope_index),
                                 view->status, sizeof(view->status));
        jw__shader_notice(view->status);
    }
    jw__shader_resume(socket_path, state, menu_running);
}

static void jw__shader_off_choice(
    const char *socket_path, jw_ingame_state *state,
    jw_ingame_shader_view *view, const jw_shader_picker_transport *transport,
    bool *menu_running) {
    /* The scope is selected in the shader list with Left/Right. Turning the
       shader off therefore removes the preset at that already-visible scope;
       do not open another scope picker window. */
    jw_ra_shader_scope scope = jw__shader_scope_from_index(view->scope_index);
    if (!jw_shader_picker_scope_enabled(scope, JW_FUGAZI_RESOLVER_ASSEMBLED)) {
        jw__shader_notice(T("All RetroArch requires a Leaf build with Fugazi's conflict resolver."));
        return;
    }
    jw_shader_picker_result result = jw_shader_picker_remove(transport, scope);
    if (result != JW_SHADER_PICKER_OK) {
        jw__shader_result_status(result, NULL, view->status, sizeof(view->status));
        jw__shader_notice(view->status);
        return;
    }
    /* Removing an automatic preset does not synthesize an Off override;
       broader presets are reevaluated on the next content load by RetroArch. */
    jw__shader_resume(socket_path, state, menu_running);
}

static void jw__ingame_show_shader(const char *socket_path,
                                   jw_ingame_state *state,
                                   bool *menu_running) {
    jw_ingame_shader_view view;
    memset(&view, 0, sizeof(view));
    jw_ingame_shader_transport ctx;
    jw_shader_picker_transport transport =
        jw__ingame_shader_transport(&ctx, socket_path);
    jw_shader_picker_init(&view.picker);

    jw_shader_picker_result probe = jw_shader_picker_probe(&view.picker, &transport);
    if (probe != JW_SHADER_PICKER_OK) {
        char message[256];
        jw__shader_result_status(probe, NULL, message, sizeof(message));
        if (jw__shader_confirmation(message, T("RetroArch Settings")) &&
            jw_ipc_retroarch_action(socket_path, "settings", 0,
                                    state->status, sizeof(state->status)) == 0)
            *menu_running = false;
        return;
    }

    char *manifest = jw_retroarch_shader_manifest_path();
    view.recommended_root = jw_retroarch_recommended_shaders_dir();
    view.catalog_valid = manifest && view.recommended_root &&
        jw_shader_catalog_load(manifest, state->session.system, &view.catalog);
    free(manifest);

    int initial = 0;
    if (view.picker.original_path[0]) {
        char referenced[PATH_MAX] = "";
        (void)jw_shader_catalog_reference_target(view.picker.original_path,
                                                  referenced,
                                                  sizeof(referenced));
        initial = -1;
        for (size_t i = 0; i < view.catalog.count; i++) {
            char path[PATH_MAX];
            if (jw__ingame_shader_row_path(view.recommended_root,
                                           &view.catalog.rows[i], path,
                                           sizeof(path)) &&
                (strcmp(path, view.picker.original_path) == 0 ||
                 (referenced[0] && strcmp(path, referenced) == 0))) {
                initial = 1 + (int)i;
                break;
            }
        }
        if (initial < 0) {
            view.custom_row = true;
            initial = jw__shader_custom_index(&view);
        }
    }
    cat_list_state_init(&view.list, 8);
    cat_list_state_jump(&view.list, initial, jw__shader_item_count(&view));
    view.applied_cursor = initial;

    bool running = true;
    while (running && *menu_running && !g_hide_requested) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            if (ev.button == CAT_BTN_UP || ev.button == CAT_BTN_DOWN) {
                int before = view.list.cursor;
                cat_list_state_move(&view.list,
                                    ev.button == CAT_BTN_UP ? -1 : +1,
                                    jw__shader_item_count(&view));
                if (view.list.cursor != before) {
                    /* The backdrop is a frozen still, so browsing must not
                       block input applying an invisible shader preview. */
                    view.present_ms = jw__monotonic_ms();
                    view.status[0] = '\0';
                }
            } else if ((ev.button == CAT_BTN_LEFT || ev.button == CAT_BTN_RIGHT) &&
                       view.list.cursor != jw__shader_advanced_index(&view)) {
                int before = view.scope_index;
                view.scope_index = jw__shader_scope_cycle(
                    view.scope_index, ev.button == CAT_BTN_LEFT ? -1 : +1);
                if (view.scope_index != before) {
                    view.present_ms = jw__monotonic_ms();
                    view.status[0] = '\0';
                }
            } else if (ev.button == CAT_BTN_B) {
                jw_shader_picker_result result =
                    jw_shader_picker_cancel(&view.picker, &transport);
                if (result == JW_SHADER_PICKER_OK) {
                    running = false;
                } else {
                    jw__shader_result_status(result, NULL, view.status,
                                             sizeof(view.status));
                }
            } else if (ev.button == CAT_BTN_A || ev.button == CAT_BTN_START) {
                int cursor = view.list.cursor;
                if (cursor == jw__shader_advanced_index(&view)) {
                    jw_shader_picker_result result =
                        jw_shader_picker_cancel(&view.picker, &transport);
                    if (result != JW_SHADER_PICKER_OK) {
                        jw__shader_result_status(result, NULL, view.status,
                                                 sizeof(view.status));
                    } else if (jw_ipc_retroarch_action(
                                   socket_path, "shader-settings", 0,
                                   state->status, sizeof(state->status)) == 0) {
                        *menu_running = false;
                    }
                } else {
                    jw_shader_picker_result result = jw__shader_apply_selection(
                        &view, &transport, cursor);
                    if (result == JW_SHADER_PICKER_OK) {
                        if (cursor == 0) {
                            jw__shader_off_choice(socket_path, state, &view,
                                                  &transport, menu_running);
                        } else {
                            jw__shader_save_choice(socket_path, state, &view,
                                                   &transport, menu_running);
                        }
                    }
                }
            }

            if (view.status[0] &&
                !view.picker.current_known &&
                jw__shader_offer_settings(view.status) &&
                jw_ipc_retroarch_action(socket_path, "settings", 0,
                                        state->status,
                                        sizeof(state->status)) == 0) {
                *menu_running = false;
            }
        }
        if (running && *menu_running) {
            jw__render_ingame_shader(state, &view);
            if (view.present_ms > 0) {
                jw_log_info(
                    "shader picker navigation timings: input_to_present_ms=%lld cursor=%d",
                    jw__monotonic_ms() - view.present_ms, view.list.cursor);
                view.present_ms = 0;
            }
        }
    }
    jw_shader_catalog_free(&view.catalog);
    free(view.recommended_root);
    jw__ingame_shader_refresh(socket_path, state);
}

static void jw__ingame_refresh(const char *socket_path, jw_ingame_state *state) {
    state->session_details_ready = false;
    if (jw_ipc_get_retroarch_session(socket_path, &state->session,
                                     state->status,
                                     sizeof(state->status)) != 0) {
        memset(&state->session, 0, sizeof(state->session));
        if (!state->status[0]) {
            snprintf(state->status, sizeof(state->status), "%s",
                     "RetroArch session unavailable");
        }
    } else if (state->session.active && state->session.command_ok) {
        state->session_details_ready = true;
        state->status[0] = '\0';
    }
    jw__ingame_resolve_titles(state);
    jw__ingame_perf_refresh(socket_path, state);
    jw__ingame_shader_refresh(socket_path, state);
}

/* Only a timeout is worth retrying: the session is there, RetroArch was just
   late. Any other failure would answer the same way again. */
static bool jw__ingame_refresh_timed_out(const jw_ingame_state *state) {
    return state->session.active && !state->session_details_ready &&
           strcmp(state->session.command_result, "timeout") == 0;
}

static void jw__copy_env_string(const char *name, char *out, size_t out_size) {
    const char *value = getenv(name);
    if (!out || out_size == 0) {
        return;
    }
    if (!value) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "%s", value);
}

static bool jw__prime_ingame_session_from_env(jw_ingame_state *state) {
    const char *active = getenv("JAWAKA_INGAME_ACTIVE");
    if (!state || !active || strcmp(active, "1") != 0) {
        return false;
    }

    memset(&state->session, 0, sizeof(state->session));
    state->session.active = true;
    state->session.command_ok = true;
    state->session.disk_count = 0;
    state->session.disk_slot = -1;
    state->session.state_slot = -1;
    jw__copy_env_string("JAWAKA_INGAME_SYSTEM",
                        state->session.system,
                        sizeof(state->session.system));
    jw__copy_env_string("JAWAKA_INGAME_ROM",
                        state->session.rom_path,
                        sizeof(state->session.rom_path));
    jw__copy_env_string("JAWAKA_INGAME_CORE",
                        state->session.core_path,
                        sizeof(state->session.core_path));
    jw__copy_env_string("JAWAKA_INGAME_CORE_ID",
                        state->session.core_id,
                        sizeof(state->session.core_id));
    jw__copy_env_string("JAWAKA_INGAME_CORE_FOLDER",
                        state->session.core_config_folder,
                        sizeof(state->session.core_config_folder));
    snprintf(state->session.command_result,
             sizeof(state->session.command_result), "%s", "pending");
    state->status[0] = '\0';
    jw__ingame_resolve_titles(state);
    return true;
}

static void jw__ingame_continue(const char *socket_path, jw_ingame_state *state,
                                bool *running) {
    if (!state->session.active) {
        *running = false;
        return;
    }

    if (jw_ipc_retroarch_action(socket_path, "continue", 0,
                                state->status, sizeof(state->status)) == 0) {
        *running = false;
    }
}

static int jw__ingame_activate(const char *socket_path, jw_ingame_state *state,
                               bool *running) {
    if (!state->session.active) {
        snprintf(state->status, sizeof(state->status), "%s",
                 "No active RetroArch session");
        *running = false;
        return -1;
    }

    const char *action = NULL;
    int value = 0;

    switch (state->list.cursor) {
        case JW_INGAME_CONTINUE:
            jw__ingame_continue(socket_path, state, running);
            return 0;
        case JW_INGAME_SAVE:
            if (!state->session.savestate_supported) {
                snprintf(state->status, sizeof(state->status), "%s",
                         "Savestates are not available");
                return -1;
            }
            action = "save-state";
            value = state->save_slot; /* -1 = auto; daemon writes the explicit slot */
            break;
        case JW_INGAME_LOAD:
            if (!state->session.savestate_supported) {
                snprintf(state->status, sizeof(state->status), "%s",
                         "Savestates are not available");
                return -1;
            }
            if (state->load_count <= 0) {
                snprintf(state->status, sizeof(state->status), "%s",
                         T("No saves to load"));
                return -1;
            }
            action = "load-state";
            value = state->load_entries[state->load_index].slot; /* 99 for Latest */
            break;
        case JW_INGAME_RESET:
            action = "reset";
            break;
        case JW_INGAME_PERF:
            jw__ingame_show_performance(socket_path, state);
            return 0;
        case JW_INGAME_SHADER:
            jw__ingame_show_shader(socket_path, state, running);
            return 0;
        case JW_INGAME_SETTINGS:
            action = "settings";
            break;
        case JW_INGAME_QUIT:
            action = state->quit_save ? "save-and-quit" : "quit";
            break;
        default:
            return 0;
    }

    if (jw_ipc_retroarch_action(socket_path, action, value,
                                state->status, sizeof(state->status)) != 0) {
        /* Report the refusal. Returning 0 here made a failed save/load/reset
           feel identical to a successful one, while the status line said the
           opposite. */
        return -1;
    }
    *running = false;
    return 0;
}

/* Returns true when the press actually changed something, so the caller can
   tick "nav" rather than claiming movement on a row that adjusts nothing (the
   default Continue row on a single-disc game, Reset, Settings, and the
   savestate rows when the core has no savestate support). */
static bool jw__ingame_adjust(const char *socket_path, jw_ingame_state *state,
                              int delta) {
    const char *action = NULL;
    if (state->list.cursor == JW_INGAME_QUIT) {
        /* Left/Right toggles the quit mode between Save & Quit (default) and a
           plain discard Quit; no IPC until A confirms. */
        state->quit_save = !state->quit_save;
        return true;
    }
    if (state->list.cursor == JW_INGAME_CONTINUE &&
        state->session.disk_count > 1) {
        action = delta < 0 ? "disk-prev" : "disk-next";
    } else if (state->list.cursor == JW_INGAME_SAVE &&
               state->session.savestate_supported) {
        /* Cycle the Save target Auto(-1) → 0 → … → 9, wrapping; slot 99 is never
           in this set (hidden from regular use). Local only — no IPC. */
        int s = state->save_slot + delta;
        if (s < -1) s = 9;
        if (s > 9) s = -1;
        state->save_slot = s;
        return true;
    } else if (state->list.cursor == JW_INGAME_LOAD &&
               state->session.savestate_supported) {
        /* Browse existing saves newest-first (the "Latest" switcher entry, when
           present, sits at index 0). Local only — no IPC. */
        if (state->load_count <= 0) {
            return false;
        }
        {
            int i = state->load_index + delta;
            if (i < 0) i = state->load_count - 1;
            if (i >= state->load_count) i = 0;
            state->load_index = i;
        }
        return true;
    } else if (state->list.cursor == JW_INGAME_PERF) {
        if (!state->perf_ready) {
            jw__ingame_perf_refresh(socket_path, state);
        }
        if (!state->perf_ready || !state->perf.supported) {
            snprintf(state->status, sizeof(state->status), "%s",
                     "Performance unavailable");
            return false;
        }
        int quick_count = JW_INGAME_PERF_PROFILE_COUNT - 1; /* keep Custom in the tuner */
        if (state->perf_profile_index >= quick_count) {
            /* On CUSTOM (a custom session override is live): the quick-cycle
               can't represent it, so enter the cycle at a defined endpoint
               rather than mod-wrapping an out-of-range index. */
            state->perf_profile_index = 0;
        } else {
            state->perf_profile_index =
                (state->perf_profile_index + delta + quick_count) % quick_count;
        }
        jw__ingame_perf_apply_profile(socket_path, state);
        return true;
    }

    if (!action) {
        return false;
    }

    if (jw_ipc_retroarch_action(socket_path, action, 0,
                                state->status, sizeof(state->status)) != 0) {
        return false;
    }
    jw__ingame_refresh(socket_path, state);
    return true;
}

static void jw__handle_ingame_input(const char *socket_path,
                                    jw_ingame_state *state,
                                    cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_UP:
        case CAT_BTN_DOWN: {
            int before = state->list.cursor;
            cat_list_state_move(&state->list,
                                button == CAT_BTN_UP ? -1 : +1,
                                JW_INGAME_COUNT);
            if (state->list.cursor != before) {
                bool on_thumb = state->session.savestate_supported &&
                    (state->list.cursor == JW_INGAME_SAVE ||
                     state->list.cursor == JW_INGAME_LOAD);
                state->thumb_move_ms = on_thumb ? jw__monotonic_ms() : 0;
                state->thumb_present_ms = state->thumb_move_ms;
            }
            break;
        }
        case CAT_BTN_LEFT:
            jw__haptic(socket_path,
                       jw__ingame_adjust(socket_path, state, -1) ? "nav" : "blocked");
            break;
        case CAT_BTN_RIGHT:
            jw__haptic(socket_path,
                       jw__ingame_adjust(socket_path, state, +1) ? "nav" : "blocked");
            break;
        case CAT_BTN_A:
        case CAT_BTN_START:
            /* Tick on the outcome: activate refuses when there is no session or
               savestates are unavailable, and Performance only opens a submenu
               (the launcher signals an overlay open with "select"). */
            if (state->list.cursor == JW_INGAME_PERF ||
                state->list.cursor == JW_INGAME_SHADER) {
                jw__haptic(socket_path, "select");
                jw__ingame_activate(socket_path, state, running);
            } else {
                jw__haptic(socket_path,
                           jw__ingame_activate(socket_path, state, running) == 0
                               ? "commit" : "blocked");
            }
            break;
        case CAT_BTN_B:
            jw__haptic(socket_path, "commit");   /* double tick: back into the game */
            jw__ingame_continue(socket_path, state, running);
            break;
        case CAT_BTN_Y:
            /* Keep: promote the switcher quicksave (the "Latest" Load entry) into
               a permanent numbered slot. File copy via the states helper — does
               not disturb the running game. */
            if (state->list.cursor == JW_INGAME_LOAD &&
                state->session.savestate_supported &&
                state->load_count > 0 &&
                state->load_entries[state->load_index].is_latest) {
                char dir[PATH_MAX];
                int kept = -1;
                if (jw__ingame_states_dir(state, dir, sizeof(dir)) &&
                    jw_ra_promote_switcher_slot(dir, state->session.rom_path,
                                                &kept)) {
                    snprintf(state->status, sizeof(state->status),
                             "Kept as Slot %d", kept);
                    jw__ingame_rebuild_slots(state);
                } else {
                    snprintf(state->status, sizeof(state->status), "%s",
                             "Keep failed");
                }
            }
            break;
        default:
            break;
    }
}

/* ── In-game game switcher overlay ────────────────────────────────────────
   The resident in-game process serves two surfaces depending on a runtime
   mode file the daemon writes before each reveal: the regular in-game menu, or
   this recents/resume carousel. Both share the paused-frame backdrop and the
   warm-standby show/hide path. */

typedef enum {
    JW_INGAME_UI_MENU = 0,
    JW_INGAME_UI_SWITCHER,
} jw_ingame_ui_mode;

static jw_ingame_ui_mode jw__ingame_ui_mode_read(void) {
    char *path = jw_ingame_ui_mode_path();
    if (!path) {
        return JW_INGAME_UI_MENU;
    }
    jw_ingame_ui_mode mode = JW_INGAME_UI_MENU;
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[32] = {0};
        if (fgets(buf, sizeof(buf), f)) {
            buf[strcspn(buf, "\r\n")] = '\0';
            if (strcmp(buf, "switcher") == 0)
                mode = JW_INGAME_UI_SWITCHER;
        }
        fclose(f);
    }
    free(path);
    return mode;
}

static void jw__render_ingame_switcher(const jw_ingame_state *state,
                                       jw_game_switcher *switcher) {
    cat_status_bar_opts sb = state->status_bar;

    cat_clear_screen();
    /* Same paused-frame backdrop + scrim as the in-game menu. */
    if (state->still_tex) {
        int sw = cat_get_screen_width();
        int sh = cat_get_screen_height();
        cat_draw_image(state->still_tex, 0, 0, sw, sh);
        ap_color scrim = { 0, 0, 0, 150 };
        cat_draw_rect(0, 0, sw, sh, scrim);
    }
    cat_draw_screen_title(T("Switcher"), &sb);

    SDL_Rect content = cat_get_content_rect(true, state->show_hints, false);
    int margin = cat_scale(12);
    jw_game_switcher_render(switcher, content.x + margin, content.y,
                            content.w - margin * 2, content.h);

    if (state->show_hints) {
        cat_footer_item footer[3];
        int count = 0;
        bool has_selection = jw_game_switcher_selected(switcher) != NULL;
        if (has_selection && !jw_game_switcher_selected_is_current(switcher))
            footer[count++] = (cat_footer_item){ CAT_BTN_Y, "Remove", false, JW_HINT("Y") };
        footer[count++] = (cat_footer_item){ CAT_BTN_B, "Resume", true, JW_HINT("B") };
        if (has_selection)
            footer[count++] = (cat_footer_item){ CAT_BTN_A, "Switch", true, JW_HINT("A") };
        jw__menu_footer(footer, count);
    }
    cat_present();
}

/* Resume the running game and drop the overlay back to standby. */
static void jw__ingame_switcher_resume(const char *socket_path,
                                       jw_ingame_state *state, bool *running) {
    if (jw_ipc_retroarch_action(socket_path, "continue", 0,
                                state->status, sizeof(state->status)) == 0) {
        *running = false;
    }
}

static void jw__handle_ingame_switcher_input(const char *socket_path,
                                             const char *db_path,
                                             jw_ingame_state *state,
                                             jw_game_switcher *switcher,
                                             cat_button button, bool *running) {
    switch (button) {
        case CAT_BTN_LEFT:
        case CAT_BTN_UP:
        case CAT_BTN_RIGHT:
        case CAT_BTN_DOWN: {
            /* The strip clamps rather than wraps below JW_SWITCHER_LOOP_MIN
               entries, so compare -- at the ends of a short Recents list an
               unconditional tick would claim a move that did not happen. */
            int before = switcher->cursor;
            jw_game_switcher_move(switcher,
                (button == CAT_BTN_LEFT || button == CAT_BTN_UP) ? -1 : +1);
            jw__haptic(socket_path,
                       switcher->cursor != before ? "nav" : "blocked");
            break;
        }
        case CAT_BTN_A: {
            const jw_game_entry *sel = jw_game_switcher_selected(switcher);
            if (!sel) {
                jw__haptic(socket_path, "blocked");
                break;
            }
            jw__haptic(socket_path, "commit");   /* double tick: leaving into a game */
            if (jw_game_switcher_selected_is_current(switcher)) {
                jw__ingame_switcher_resume(socket_path, state, running);
            } else if (jw_ipc_switch_game(socket_path, sel->system, sel->rom_path,
                                          state->status, sizeof(state->status)) == 0) {
                /* The daemon either switches in-process or falls back to
                   save+quit+spawn. Stop the visible loop so we hide cleanly in
                   the meantime. */
                *running = false;
            }
            break;
        }
        case CAT_BTN_B:
            jw__haptic(socket_path, "commit");   /* double tick: back into the game */
            jw__ingame_switcher_resume(socket_path, state, running);
            break;
        case CAT_BTN_Y: {
            if (jw_game_switcher_selected_is_current(switcher)) {
                jw__haptic(socket_path, "blocked");
                break; /* never remove the running game from the overlay */
            }
            const jw_game_entry *sel = jw_game_switcher_selected(switcher);
            if (sel && sel->id >= 0 &&
                jw_db_remove_recent(db_path, "game", sel->id) == 0) {
                jw_game_switcher_remove_selected(switcher);
                jw__haptic(socket_path, "commit");
            } else {
                jw__haptic(socket_path, "blocked");
            }
            break;
        }
        default:
            break;
    }
}

/* Status-bar prefs plus the values the menu cannot read live: volume (the
   daemon's last persisted level), Bluetooth and Wi-Fi. The process stays
   resident for the whole game, so each reveal reloads them or a mid-game volume
   change would show the level from game start. Battery needs none of this:
   Catastrophe reads it on every draw. */
static void jw__ingame_load_status_bar(const char *db_path, jw_ingame_state *state) {
    jw_settings_load_status_prefs(db_path, &state->status_bar, &state->show_hints);
    /* Supply the wifi strength ourselves so Catastrophe doesn't shell out for
       it — same source as the launcher. */
    if (state->status_bar.show_wifi) {
        state->status_bar.wifi_supplied = true;
        state->status_bar.wifi_strength = jw_wifi_strength_now();
    }
}

static void jw__ingame_show_switcher(const char *socket_path, const char *db_path,
                                     jw_ingame_state *state) {
    state->status[0] = '\0';
    jw__ingame_refresh(socket_path, state);

    char *sd_root = jw_sdcard_root();
    /* States/ root for savestate thumbnails: the daemon hands us a source-aware
       JAWAKA_INGAME_STATEDIR; fall back to STATES_PATH, then <sdcard>/States. */
    const char *states = getenv("JAWAKA_INGAME_STATEDIR");
    char states_buf[PATH_MAX];
    if (!states || !states[0]) {
        const char *sp = getenv("STATES_PATH");
        if (sp && sp[0]) {
            states = sp;
        } else if (sd_root && sd_root[0]) {
            snprintf(states_buf, sizeof(states_buf), "%s/States", sd_root);
            states = states_buf;
        } else {
            states = NULL;
        }
    }

    jw_game_switcher switcher;
    jw_game_switcher_reset(&switcher, true, sd_root ? sd_root : "", states);
    /* The resident menu reloads here on every reveal, including visibility. */
    jw_game_switcher_load(&switcher, db_path);
    if (state->session.active && state->session.rom_path[0]) {
        jw_game_switcher_set_current(&switcher, db_path, state->session.system,
                                     state->session.rom_path,
                                     state->game_title, NULL);
    }
    jw_game_switcher_resolve_thumbnails(&switcher);
    free(sd_root);

    /* Grab the paused frame before mapping so the game shows behind the
       carousel from the first visible frame (mirrors the in-game menu). */
    jw__ingame_capture_still(state);

    /* Reuse that same still as the running game's carousel tile so the current
       tile shows a real screenshot instantly, even before any save exists.
       Borrowed — state->still_tex stays owned/freed by jw__ingame_free_imagery. */
    jw_game_switcher_set_current_texture(&switcher, state->still_tex);

    cat_show_window();
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

    bool running = true;
    while (running && !g_hide_requested) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            jw__handle_ingame_switcher_input(socket_path, db_path, state,
                                             &switcher, ev.button, &running);
        }
        jw__render_ingame_switcher(state, &switcher);
    }
}

static int jw__run_ingame_menu(const char *socket_path, const char *db_path,
                               long long process_start_ms) {
    (void)process_start_ms;
    jw_ingame_state state;
    memset(&state, 0, sizeof(state));
    state.db_path = db_path;
    jw__ingame_load_status_bar(db_path, &state);
    jw__ingame_free_imagery(&state); /* init texture/slot bookkeeping */
    g_kms_crtc = jw__kms_probe_crtc(); /* warm-up: pay the slow DRM probe once */

    /* Resident loop: warm up once, then park hidden and reveal on demand. Each
       Menu tap signals SIGUSR1 -> we show; Continue/Quit hides us back to the
       wait. The process exits only when the daemon terminates it at game exit
       (default SIGTERM disposition), so there is no normal loop exit. */
    bool first_show = true;
    for (;;) {
        jw__menu_wait_for_show();

        /* A close (SIGUSR2) can race the show (SIGUSR1) on a fast double-tap.
           If one already arrived, the daemon has resumed the game and cleared
           visibility, so skip showing entirely and go back to standby. */
        if (g_hide_requested) {
            jw__menu_clear_hide();
            continue;
        }

        /* The daemon picks the surface for this reveal via the UI-mode file. */
        jw_ingame_ui_mode mode = jw__ingame_ui_mode_read();
        if (mode == JW_INGAME_UI_SWITCHER) {
            jw__ingame_show_switcher(socket_path, db_path, &state);
        } else {
        long long show_start_ms = jw__monotonic_ms();
        cat_list_state_init(&state.list, JW_INGAME_COUNT);
        state.status[0] = '\0';
        state.quit_save = true; /* Save & Quit is the default Quit action */
        bool primed = jw__prime_ingame_session_from_env(&state);

        /* Grab the paused frame BEFORE mapping the window, so the first visible
           frame already shows the game behind the menu (no late pop-in). */
        long long cap_start_ms = jw__monotonic_ms();
        jw__ingame_capture_still(&state);
        jw_log_info("in-game menu capture timings: capture_ms=%lld",
                    jw__monotonic_ms() - cap_start_ms);

        cat_show_window();
        /* Drop controller input buffered while hidden so gameplay presses don't
           fire as menu navigation on entry (input gating, defense in depth). */
        SDL_PumpEvents();
        SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);

        if (primed) {
            cat_request_frame();
            jw__render_ingame_menu(&state);
            jw_log_info("in-game menu show timings: show_ms=%lld primed=1 first=%d",
                        jw__monotonic_ms() - show_start_ms, first_show);
        }

        long long refresh_start_ms = jw__monotonic_ms();
        jw__ingame_refresh(socket_path, &state);
        long long refresh_done_ms = jw__monotonic_ms();

        /* Build the decoupled Save/Load slot lists for this session (existing
           saves newest-first; switcher slot surfaced as "Latest"). */
        jw__ingame_rebuild_slots(&state);
        jw__ingame_init_save_slot(&state);
        if (!primed) {
            cat_request_frame();
            jw__render_ingame_menu(&state);
            jw_log_info("in-game menu show timings: show_ms=%lld primed=0 first=%d",
                        refresh_done_ms - show_start_ms, first_show);
        }
        jw_log_info("in-game menu refresh timings: refresh_ms=%lld command=%s",
                    refresh_done_ms - refresh_start_ms,
                    state.session.command_result);
        /* After the first frame so menu open time is unchanged; the loop's
           first render repaints with the fresh values. */
        jw__ingame_load_status_bar(db_path, &state);

        int refresh_retries =
            jw__ingame_refresh_timed_out(&state) ? JW_INGAME_REFRESH_RETRIES : 0;
        long long refresh_retry_ms = jw__monotonic_ms() + JW_INGAME_REFRESH_RETRY_MS;
        if (refresh_retries > 0) {
            /* An idle menu sleeps until input or the clock's next minute. */
            cat_request_frame_in(JW_INGAME_REFRESH_RETRY_MS);
        }

        bool running = true;
        while (running && !g_hide_requested) {
            cat_input_event ev;
            while (cat_poll_input(&ev)) {
                if (!ev.pressed) continue;
                jw__handle_ingame_input(socket_path, &state, ev.button, &running);
            }
            if (running && refresh_retries > 0 &&
                jw__monotonic_ms() >= refresh_retry_ms) {
                refresh_retries--;
                long long retry_start_ms = jw__monotonic_ms();
                jw__ingame_refresh(socket_path, &state);
                jw__ingame_rebuild_slots(&state);
                jw__ingame_init_save_slot(&state);
                jw_log_info("in-game menu refresh retry: refresh_ms=%lld command=%s "
                            "retries_left=%d",
                            jw__monotonic_ms() - retry_start_ms,
                            state.session.command_result, refresh_retries);
                if (!jw__ingame_refresh_timed_out(&state)) {
                    refresh_retries = 0;
                } else if (refresh_retries > 0) {
                    cat_request_frame_in(JW_INGAME_REFRESH_RETRY_MS);
                }
                refresh_retry_ms = jw__monotonic_ms() + JW_INGAME_REFRESH_RETRY_MS;
                cat_request_frame();
            }
            jw__ingame_update_thumb(&state);
            jw__render_ingame_menu(&state);
            if (state.thumb_present_ms > 0) {
                jw_log_info(
                    "in-game thumbnail timings: input_to_present_ms=%lld outcome=%s",
                    jw__monotonic_ms() - state.thumb_present_ms,
                    state.thumb_tex ? "texture" : "placeholder");
                state.thumb_present_ms = 0;
            }
        }
        }

        /* Leave reason: running=false (Continue/Save/Load/Reset/Quit, daemon
           already resumed) or g_hide_requested (Menu toggle closed, daemon
           already resumed). Either way just hide back to standby. */
        jw__menu_clear_hide();
        cat_hide_window();
        jw__ingame_free_imagery(&state); /* don't hold textures while parked */
        first_show = false;
    }

    return 0;
}

/* ── Hosted settings screens (About / System Update) ──────────────────────────
   These two used to be Settings categories; they now live in the System menu and
   render here through the shared jw_settings_ui API (no duplicated layout). The
   page draws its own header, so we add no separate title bar (the menu popup has
   no status bar) and supply the per-screen footer the launcher would normally
   provide. */
static void jw__render_hosted(const jw_menu_state *menu, const jw_settings_ui *ui) {
    cat_clear_screen();
    SDL_Rect cr = cat_get_content_rect(false, menu->show_hints, false);
    int margin = CAT_S(12);
    jw_settings_ui_render(ui, cr.x + margin, cr.y, cr.w - margin * 2, cr.h);

    if (menu->show_hints) {
        switch (jw_settings_ui_screen(ui)) {
            case JW_SETTINGS_ABOUT: {
                cat_footer_item f[] = {
                    { CAT_BTN_B, "Back", true, JW_HINT("B") },
                };
                cat_draw_footer(f, 1);
                break;
            }
            case JW_SETTINGS_UPDATE: {
                cat_footer_item f[] = {
                    { CAT_BTN_X, "Releases", false, JW_HINT("X") },
                    { CAT_BTN_B, "Back",     true,  JW_HINT("B") },
                    { CAT_BTN_A, "Select",   true,  JW_HINT("A") },
                };
                cat_draw_footer(f, 3);
                break;
            }
            case JW_SETTINGS_UPDATE_PICKER: {
                cat_footer_item f[] = {
                    { CAT_BTN_X, "Refresh", false, JW_HINT("X") },
                    { CAT_BTN_B, "Back",    true,  JW_HINT("B") },
                    { CAT_BTN_A, "Pick",    true,  JW_HINT("A") },
                };
                cat_draw_footer(f, 3);
                break;
            }
            default:
                break;
        }
    }
    cat_present();
}

/* Modal sub-loop: host one settings screen until the user backs out of it (the
   screen returns to HOME). The jw_settings_ui is built once on first use and
   reused for later visits; it lives until the menu process exits. */
static void jw__menu_host_setting(const char *socket_path, jw_menu_state *menu,
                                  jw_settings_screen screen) {
    static jw_settings_ui *ui = NULL;
    if (!ui) {
        ui = malloc(sizeof(*ui));
        if (!ui) return;
        char theme_name[256];
        jw_resolve_theme_name(menu->db_path, theme_name, sizeof(theme_name));
        jw_settings_ui_init(ui, menu->db_path, theme_name, socket_path);
    }
    jw_settings_ui_open(ui, screen);

    char status[256] = { 0 };
    bool running = true;
    cat_request_frame();
    while (running) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            bool theme_changed = false;
            jw_settings_ui_handle_button(ui, ev.button, status, sizeof(status),
                                         &theme_changed);
            /* B at the hosted screen returns it to HOME; that is our cue to drop
               back to the System list. The Update <-> Releases picker navigation
               stays inside the loop (neither is HOME). */
            if (!jw_settings_ui_is_open(ui) ||
                jw_settings_ui_screen(ui) == JW_SETTINGS_HOME) {
                running = false;
                break;
            }
        }
        if (!running) break;
        if (jw_settings_ui_screen(ui) == JW_SETTINGS_UPDATE ||
            jw_settings_ui_screen(ui) == JW_SETTINGS_UPDATE_PICKER)
            jw_settings_ui_refresh_update(ui);
        jw__render_hosted(menu, ui);
    }
    jw_settings_ui_close(ui);
    cat_request_frame();   /* repaint the System list on return */
}

int main(int argc, char **argv) {
    long long process_start_ms = jw__monotonic_ms();
    bool in_game = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--in-game") == 0) {
            in_game = true;
            continue;
        }
        fprintf(stderr, "Usage: jawaka-menu [--in-game]\n");
        return 2;
    }

    char *socket_path = jw_socket_path();
    char *db_path     = jw_db_path();
    if (!socket_path) {
        jw_log_error("could not resolve socket path");
        free(db_path);
        return 1;
    }

    /* Load the UI language before anything draws -- this binary renders the
       in-game menu and hosts settings pages, and without its own load every
       T() in it silently returns English. (Found on device: the About page
       translated but this menu's footers did not.) */
    {
        char lang[JW_I18N_CODE_MAX];
        if (!db_path ||
            jw_db_get_setting(db_path, "language", lang, sizeof(lang)) != 0 ||
            !lang[0])
            snprintf(lang, sizeof(lang), "%s", "en");
        jw_i18n_load(lang);
    }

    /* From here on every list and keyboard reports its own movement. */
    cat_ui_feedback_set(jw__ui_feedback, socket_path);

    long long hello_start_ms = jw__monotonic_ms();
    if (jw_ipc_hello(socket_path, in_game ? "ingame-menu" : "menu") != 0) {
        jw_log_error("could not connect to jawakad at %s; is the daemon running?",
                     socket_path);
        free(socket_path);
        free(db_path);
        return 1;
    }
    long long hello_done_ms = jw__monotonic_ms();

    /* In-game menu runs as a resident warm standby: install the show signal
       before cat_init so an early reveal request is buffered, and create the
       window hidden so we never flash over RetroArch while warming up. */
    if (in_game && jw__menu_init_show_signal() != 0) {
        free(socket_path);
        free(db_path);
        return 1;
    }

    cat_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.window_title       = "Jawaka Menu";
    cfg.disable_background = true;
    cfg.start_hidden       = in_game;

    long long cat_start_ms = jw__monotonic_ms();
    if (cat_init(&cfg) != CAT_OK) {
        jw_log_error("catastrophe init failed: %s", cat_get_error());
        free(socket_path);
        free(db_path);
        return 1;
    }
    jw_cat_services_install(socket_path);
    if (in_game) cat_set_idle_wake_fd(g_hide_pipe[0]);
    long long cat_done_ms = jw__monotonic_ms();

    /* Resolve theme: env > DB > default Jawaka-Tabs.
       Matches launcher precedence so menu inherits whatever was last picked. */
    long long theme_start_ms = jw__monotonic_ms();
    {
        char theme_name[256];
        jw_resolve_theme_name(db_path, theme_name, sizeof(theme_name));
        cat_stylesheet ss;
        if (cat_stylesheet_load_theme(&ss, theme_name) == CAT_OK)
            cat_stylesheet_apply(&ss);

        /* Apply the user's persisted color/layout overrides on top of the
           theme so the menu matches the launcher's customized appearance. */
        jw_settings_apply_persisted_overrides(db_path);
    }
    long long theme_done_ms = jw__monotonic_ms();

    /* Frontend menu raises itself now; the in-game standby stays hidden until
       cat_show_window() reveals (and raises) it on the first Menu tap. */
    if (!in_game) {
        cat_activate_window();
    }
    long long activated_ms = jw__monotonic_ms();
    if (in_game) {
        jw_log_info("menu startup timings: mode=in-game hello_ms=%lld cat_ms=%lld theme_ms=%lld activate_ms=%lld total_ms=%lld",
                    hello_done_ms - hello_start_ms,
                    cat_done_ms - cat_start_ms,
                    theme_done_ms - theme_start_ms,
                    activated_ms - theme_done_ms,
                    activated_ms - process_start_ms);
    }

    if (in_game) {
        int rc = jw__run_ingame_menu(socket_path, db_path, process_start_ms);
        cat_quit();
        free(socket_path);
        free(db_path);
        return rc;
    }

    jw_menu_state state;
    memset(&state, 0, sizeof(state));
    cat_list_state_init(&state.list, JW_MENU_COUNT);
    state.db_path = db_path;          /* for hosting About / System Update */
    state.pending_settings = -1;
    /* Prime the library counts for the Rescan detail (cheap local DB read). */
    if (jw_db_read_summary(db_path, &state.summary) == 0)
        state.summary_ready = true;
    /* status stays empty (memset above); the line only shows real feedback. */

    /* Inherit the launcher's status-bar and button-hint preferences. */
    long long status_start_ms = jw__monotonic_ms();
    jw_settings_load_status_prefs(db_path, &state.status_bar, &state.show_hints);
    /* Do not block menu open on a live radio read. Supplying "unknown" keeps
       Catastrophe from doing its own synchronous status-bar Wi-Fi probe. */
    if (state.status_bar.show_wifi) {
        state.status_bar.wifi_supplied = true;
        state.status_bar.wifi_strength = -1;
    }
    long long status_done_ms = jw__monotonic_ms();

    jw_autodemo demo;
    jw_autodemo_init(&demo);
    bool running = true;

    long long frame_start_ms = jw__monotonic_ms();
    cat_request_frame();
    jw__render_menu(&state);
    long long frame_done_ms = jw__monotonic_ms();
    jw_log_info("menu startup timings: mode=frontend hello_ms=%lld cat_ms=%lld theme_ms=%lld activate_ms=%lld status_ms=%lld first_frame_ms=%lld total_ms=%lld",
                hello_done_ms - hello_start_ms,
                cat_done_ms - cat_start_ms,
                theme_done_ms - theme_start_ms,
                activated_ms - theme_done_ms,
                status_done_ms - status_start_ms,
                frame_done_ms - frame_start_ms,
                frame_done_ms - process_start_ms);

    while (running) {
        cat_input_event ev;
        while (cat_poll_input(&ev)) {
            if (!ev.pressed) continue;
            jw__handle_input(socket_path, &state, ev.button, &running);
        }

        /* About / System Update run as a modal sub-loop outside the event drain,
           then we return to the System list. */
        if (state.pending_settings >= 0) {
            jw__menu_host_setting(socket_path, &state,
                                  (jw_settings_screen)state.pending_settings);
            state.pending_settings = -1;
            jw_autodemo_init(&demo);   /* the user was active; reset the idle timer */
        }

        if (demo.enabled && !demo.fired) {
            uint32_t rem = jw_autodemo_remaining_ms(&demo);
            if (jw_autodemo_should_fire(&demo)) {
                jw_ipc_shutdown(socket_path);
                running = false;
            } else {
                cat_request_frame_in(rem);
            }
        }

        jw__render_menu(&state);
    }

        /* Hand-off exit (back to the launcher): skip cat_quit()'s SDL/TTF teardown — the
       OS reclaims it and the Wayland surface on exit, and it's pure dead time before
       the launcher reappears. Logs are line-flushed; no exit-time writes are pending. */
    cat_hide_window();
    _Exit(0);
}
