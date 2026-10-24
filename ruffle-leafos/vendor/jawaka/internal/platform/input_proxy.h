#ifndef JW_PLATFORM_INPUT_PROXY_H
#define JW_PLATFORM_INPUT_PROXY_H

#include "internal/platform/input_shortcuts.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JW_INPUT_PROXY_MAX_PATH 256
#define JW_INPUT_PROXY_MAX_NAME 128

typedef void (*jw_input_brightness_delta_cb)(void *userdata, int delta_percent);
typedef void (*jw_input_volume_delta_cb)(void *userdata, int delta_percent);
typedef bool (*jw_input_menu_tap_cb)(void *userdata);

/* Per-session policy, retained by flush/screen-off. No gesture state lives here. */
typedef struct {
    bool tap_only;
    bool escape_enabled;
    uint32_t escape_ms;
} jw_input_menu_config;

/* A threshold is reported only after all queued input has drained. false ends
   that hold on release, cancellation, flush or shutdown. hold_id identifies
   this press within the proxy's lifetime; the daemon also binds it to a session. */
typedef void (*jw_input_menu_escape_cb)(void *userdata, uint64_t hold_id,
                                       bool threshold);

/* A Menu chord whose second button is one the shortcut model can name.
 *
 * The proxy does not know which action a button means -- that mapping is the
 * user's, lives in the settings database, and is resolved by jawakad against
 * its cached snapshot. The proxy's job is only to recognize the chord, hand
 * over the symbolic button, and act on the answer.
 *
 * Return true when the chord was consumed: the proxy then swallows the press,
 * every repeat, and the matching release, and suppresses the Menu tap, so
 * neither half of the chord reaches the running game. Return false to decline
 * -- the feature is off, or the foreground cannot perform the action. In
 * tap-only mode the proxy forwards only the other button and suppresses the
 * eventual tap; otherwise it forwards the ordinary Menu chord to RetroArch.
 *
 * These are Leaf-level chords rather than RetroArch hotkeys because RetroArch
 * only withholds its modifier from the core after input_hotkey_block_delay
 * frames; Menu is never a game input and jawakad consumes the whole chord.
 *
 * Optional; leave NULL to forward every chord. Assign directly on the proxy
 * struct after init -- it is read on the next tick. */
typedef bool (*jw_input_shortcut_dispatch_cb)(void *userdata,
                                              jw_input_shortcut_button button);
/* Force feedback played on the virtual gamepad. The pad advertises FF_RUMBLE, so
   any emulator that rumbles through SDL (or evdev directly) reaches the motor
   here instead of needing its own sysfs sink. `magnitude` is the effect's two
   channels collapsed to one 0..0xFFFF value -- the MLP1 has a single motor, so
   there is no strong/weak to drive separately -- and 0 means stop. `duration_ms`
   is the effect's replay length; 0 means "until stopped". Optional; assign
   directly on the proxy struct after init, like the screenshot hook. */
typedef void (*jw_input_rumble_cb)(void *userdata, uint16_t magnitude,
                                   uint32_t duration_ms);

typedef struct {
    bool enabled;
    char physical_event_path[JW_INPUT_PROXY_MAX_PATH];
    char virtual_event_path[JW_INPUT_PROXY_MAX_PATH];
    char device_name[JW_INPUT_PROXY_MAX_NAME];
    void *backend_data;
    jw_input_brightness_delta_cb brightness_delta;
    jw_input_volume_delta_cb volume_delta;
    jw_input_menu_tap_cb menu_tap;
    jw_input_menu_config menu_config;
    jw_input_menu_escape_cb menu_escape;
    jw_input_shortcut_dispatch_cb shortcut;
    jw_input_rumble_cb rumble;
    void *userdata;
} jw_input_proxy;

/* Assign `shortcut` on the returned struct to receive Menu chords. */
int  jw_input_proxy_init(jw_input_proxy *proxy,
                         jw_input_brightness_delta_cb brightness_delta,
                         jw_input_volume_delta_cb volume_delta,
                         jw_input_menu_tap_cb menu_tap,
                         void *userdata);
/* Watch-only: observe the physical pad for hotkeys (volume/brightness/Menu)
   without grabbing it or creating a virtual device, so a standalone emulator
   reads the pad directly while jawakad keeps the hotkeys.
 *
 * Menu chords are NOT dispatched here, and `shortcut` is ignored: with nothing
 * grabbed there is nothing to consume, so a claimed chord would reach the
 * emulator anyway. The old build attached a fixed game-switcher hook to this
 * path, but it could only ever decline -- watch-only means no RetroArch session
 * -- so removing it changes no observable behavior. */
int  jw_input_proxy_init_watch(jw_input_proxy *proxy,
                               jw_input_brightness_delta_cb brightness_delta,
                               jw_input_volume_delta_cb volume_delta,
                               jw_input_menu_tap_cb menu_tap,
                               void *userdata);
int  jw_input_proxy_retroarch_joypad_index(const jw_input_proxy *proxy);
/* Read and forward the pad on a thread of its own from now on, so a daemon
 * loop busy with something else cannot hold a button release back (a late
 * release reads as a hold, and the UI repeats). Call once the callbacks are
 * assigned. The callbacks still run on the thread that calls
 * jw_input_proxy_tick(), which must keep calling it. Without this, or if the
 * thread cannot start (-1), tick reads and forwards inline as before. */
int  jw_input_proxy_start(jw_input_proxy *proxy);
/* Poll this fd and call jw_input_proxy_tick() when it is readable: the
 * physical pad, or once started, the forwarding thread's wake-up. -1 when no
 * input backend is active. */
int  jw_input_proxy_poll_fd(const jw_input_proxy *proxy);
/* True while the proxy runs inline (never started, or the thread could not
 * start): it then reads the power key and runs its Menu-tap and escape timers
 * only from jw_input_proxy_tick(), so the daemon has to keep calling it every
 * 50 ms or so. False once started, or with no input backend. */
bool jw_input_proxy_needs_tick_cadence(const jw_input_proxy *proxy);
/* Unstarted: read, route and forward what is queued, and run the timers.
 * Started: run the callback the forwarding thread is waiting on, if any. */
void jw_input_proxy_tick(jw_input_proxy *proxy);
void jw_input_proxy_shutdown(jw_input_proxy *proxy);
/* Resets in-flight input before installing new session policy. */
void jw_input_proxy_configure_menu(jw_input_proxy *proxy,
                                  jw_input_menu_config config);
/* Power and daemon transitions cancel a hold without changing session policy. */
void jw_input_proxy_cancel_menu(jw_input_proxy *proxy);

/* Milliseconds since the last physical button input (for auto-sleep idle
 * tracking). Counts EV_KEY only — stick drift (EV_ABS) doesn't reset it. */
uint64_t jw_input_proxy_idle_ms(const jw_input_proxy *proxy);
/* Reset the idle timer to "now" (e.g. after resuming from suspend, whose wake
 * key arrives outside the proxied input device). */
void     jw_input_proxy_mark_activity(jw_input_proxy *proxy);
/* Discard any pending input without forwarding it (e.g. on resume from suspend,
 * to drop button/d-pad presses that queued while asleep so they don't replay into
 * the launcher on wake). Safe to call when disabled. */
void     jw_input_proxy_flush(jw_input_proxy *proxy);
/* While "swallow" is on (the auto-sleep screen-off stage), gamepad input still
 * resets the idle timer (so a press wakes the screen) but is NOT forwarded to the
 * launcher — so the wake press only wakes the screen instead of also firing a
 * navigation action. Turn it off once the screen is back on. */
void     jw_input_proxy_set_swallow(jw_input_proxy *proxy, bool swallow);
/* Emit a release on the virtual pad for every button currently held-forwarded,
 * so a consumer reading it sees a neutral state. Call right before unpausing
 * RetroArch when closing the in-game menu: the button that triggered the menu
 * action (e.g. A on Save State) is often still physically down, and without this
 * the core reads it as a fresh in-game press on resume. evdev is edge-based, so
 * a still-held physical button will not re-fire until released and pressed again.
 * Safe to call when disabled. */
void     jw_input_proxy_release_buttons(jw_input_proxy *proxy);
/* Synthesise a Menu tap on the virtual pad, exactly as the handheld Menu button
 * produces when nothing inside the daemon claims it (no game running, so the
 * press belongs to the launcher UI). External controllers are watched without
 * being grabbed, so their Guide press cannot be forwarded the way the built-in
 * pad's is -- without this the UI never sees Guide at all. The matching release
 * is emitted by the proxy tick, so this is safe to call from an event callback.
 * No-op when the proxy is watch-only or a tap is already in flight. */
void     jw_input_proxy_emit_menu_tap(jw_input_proxy *proxy);
/* Resume Game on Boot bypass. True when the built-in pad's `button` is down
 * according to the proxy's own physical state: the EVIOCGKEY sample taken when
 * it opened the pad, kept current by every event since, so a button held from
 * before the proxy started counts without a new press. When it is held, the
 * press is taken away from the foreground: if it was already forwarded, the
 * virtual pad releases it now, and every later event for that button is dropped
 * until its release, so the launcher never turns it into Back. False when the
 * state could not be read. */
bool     jw_input_proxy_take_held_button(jw_input_proxy *proxy,
                                         jw_input_shortcut_button button);
/* One power-key press or release edge, stamped with when the key actually moved
 * (kernel event time, CLOCK_MONOTONIC ms) — not when the daemon got around to
 * consuming it. The distinction matters for long-press detection: if a tick
 * stalls long enough for a press AND its release to queue up, consume-time
 * stamps would make any hold look like a short tap. */
typedef struct {
    bool     down;   /* true = press edge, false = release edge */
    uint64_t ms;     /* CLOCK_MONOTONIC ms of the edge */
} jw_power_edge;

/* The proxy grabs the power key exclusively at init (jawakad owns sleep/wake).
 * This pops the oldest unconsumed power-key edge into *edge and returns true,
 * or returns false when none are pending. The daemon routes them in order:
 * wake on press when the screen is off, sleep on release when it's on, clean
 * power-off when press→release spans the long-press threshold. Drain fully
 * once per tick. */
bool     jw_input_proxy_take_power_edge(jw_input_proxy *proxy, jw_power_edge *edge);

#endif /* JW_PLATFORM_INPUT_PROXY_H */
