#ifndef JW_PLATFORM_WIFI_H
#define JW_PLATFORM_WIFI_H

#include <stdbool.h>

/* Platform capability guard. When false, other jw_wifi_* calls are safe but
 * report unavailable/no-op behavior. */
bool jw_wifi_available(void);

/* Wi-Fi status from the active platform implementation. */
typedef struct {
    bool valid;        /* false if status could not be read at all */
    bool connected;    /* wpa_state == COMPLETED */
    char state[24];    /* raw wpa_state, e.g. "COMPLETED", "SCANNING", "DISCONNECTED" */
    char ssid[64];     /* current SSID, "" if none */
    char ip[40];       /* assigned IPv4, "" if none */
    int  rssi;         /* signal in dBm; 0 = unknown/disconnected */
    int  strength;     /* 0=none, 1=weak, 2=good, 3=strong — same RSSI thresholds
                          as the status-bar wifi icon, so the two agree */
} jw_wifi_status_t;

/* Populate *out from the platform Wi-Fi service. Returns 0 on success
 * (out->valid = true), -1 if the platform cannot provide status (out is still
 * zeroed with valid = false). */
int jw_wifi_status(jw_wifi_status_t *out);

/* Live signal strength 0..3 (none/weak/good/strong) from the same RSSI source as
 * jw_wifi_status, for feeding the status-bar wifi icon. 0 when disconnected/off.
 * Some platform implementations may be expensive; call on a throttle. */
int jw_wifi_strength_now(void);

/* ── Scanning (Phase 2) ──────────────────────────────────────────────────── */

#define JW_WIFI_MAX_NETWORKS 32

typedef struct {
    char ssid[64];
    int  rssi;       /* dBm */
    int  strength;   /* 0..3, same thresholds as the status icon */
    bool secured;    /* WPA/WPA2/WPA3/WEP present in the flags */
    bool current;    /* matches the currently-connected SSID */
    bool saved;      /* a saved platform profile exists for this SSID */
} jw_wifi_network_t;

/* Kick off a background scan. Returns 0 if the command was accepted, -1
 * otherwise. */
int jw_wifi_scan_start(void);

/* Read the latest scan results into out[]: blank SSIDs skipped, deduped by
 * SSID keeping the strongest, sorted by signal descending. current_ssid (may
 * be NULL/"") flags the in-use network. Returns the count written (0..max), or
 * -1 on failure. */
int jw_wifi_scan_results(const char *current_ssid, jw_wifi_network_t *out, int max);

/* ── Connect (Phase 3) ───────────────────────────────────────────────────── */

typedef enum {
    JW_WIFI_CONNECT_OK = 0,         /* association requested (watch status to confirm) */
    JW_WIFI_CONNECT_NEED_PASSWORD,  /* secured network with no saved profile (Phase 4) */
    JW_WIFI_CONNECT_FAILED,         /* platform error */
} jw_wifi_connect_result;

/* Connect by SSID. If a usable saved profile exists, re-selects it. Else if the
 * network is open, creates an open profile and connects. A secured network with
 * no saved key returns NEED_PASSWORD without changing anything (Phase 4 supplies
 * the key). On OK, a DHCP client is kicked for the interface. */
jw_wifi_connect_result jw_wifi_connect(const char *ssid, bool secured);

/* Connect to a secured network with the given pre-shared key. A wrong key can
 * still return OK here when the platform accepted the association request; the
 * caller should monitor status/events to determine the final result. */
jw_wifi_connect_result jw_wifi_connect_psk(const char *ssid, const char *psk);

/* ── Join monitoring via the platform event source ───────────────────────── */

/* Attach to the platform event source. Returns an fd to pass to the poll/close
 * calls, or -1 on failure. Open it right when a connect attempt starts. */
int  jw_wifi_monitor_open(void);

/* Result of draining the event socket during a join attempt. This hardware does
 * NOT emit reason=WRONG_KEY (esp. on SAE/WPA3, where a bad key fails at the auth
 * phase), so we distinguish a definitive wrong key from a generic auth failure. */
typedef enum {
    JW_WIFI_EVT_NONE = 0,    /* nothing conclusive yet */
    JW_WIFI_EVT_WRONG_KEY,   /* explicit WRONG_KEY (plain WPA2-PSK 4-way failure) */
    JW_WIFI_EVT_AUTH_FAIL,   /* auth timed out / assoc-reject from one access point:
                                maybe a bad key, maybe one AP of several refusing */
} jw_wifi_evt;

/* Drain buffered events (non-blocking) and report the strongest failure seen.
 * Success is detected separately via status COMPLETED on the target SSID. */
jw_wifi_evt jw_wifi_monitor_poll(int fd);

/* Detach + close the monitor fd (and clean up its local socket). */
void jw_wifi_monitor_close(int fd);

/* Recover connectivity after a failed attempt. */
void jw_wifi_recover(void);

/* Where a join attempt stands after one poll. */
typedef enum {
    JW_WIFI_ATTEMPT_PENDING = 0,
    JW_WIFI_ATTEMPT_CONNECTED,
    JW_WIFI_ATTEMPT_WRONG_KEY,   /* definitive; the profile can be forgotten */
    JW_WIFI_ATTEMPT_FAILED,      /* give up and recover the previous network */
} jw_wifi_attempt_result;

#define JW_WIFI_ATTEMPT_TIMEOUT_MS 12000u
#define JW_WIFI_ATTEMPT_AUTH_FAILS 2   /* AUTH_FAIL events that end an attempt */

/* Resolve a join attempt from one poll: whether status shows the target SSID
 * connected, the event drained since the last poll, and the time since the
 * attempt began. *auth_fails counts AUTH_FAIL events across the attempt and
 * starts at 0. Platform-independent (wifi_attempt.c). */
jw_wifi_attempt_result jw_wifi_attempt_resolve(bool connected, jw_wifi_evt evt,
                                               int *auth_fails, unsigned elapsed_ms);

/* ── Radio on/off ───────────────────────────────────────────────────────── */

/* True if the radio is enabled according to the platform implementation. */
bool jw_wifi_radio_is_on(void);

/* Toggle the radio. Returns 0 on success. */
int  jw_wifi_set_radio(bool on);

/* ── Manage (Phase 5) ────────────────────────────────────────────────────── */

/* Remove the saved profile for ssid. Returns 0 on success, -1 if no saved
 * profile / error. */
int jw_wifi_forget(const char *ssid);

/* Disconnect from the current network. It will not auto-reconnect until a
 * network is selected again. Returns 0/-1. */
int jw_wifi_disconnect(void);

/* Re-add any saved networks from the durable SD store. Adds missing profiles,
 * saves, re-exports, and reconnects. Returns the number added (0 if none/no
 * store), -1 on error. Idempotent. */
int jw_wifi_restore(void);

/* Harden any saved credentials where the platform supports migration away from
 * reusable plaintext secrets. Returns the number migrated (0 if none), -1 on
 * error. Idempotent. */
int jw_wifi_harden(void);

#endif /* JW_PLATFORM_WIFI_H */
