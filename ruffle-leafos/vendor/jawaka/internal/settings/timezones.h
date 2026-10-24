#ifndef JAWAKA_SETTINGS_TIMEZONES_H
#define JAWAKA_SETTINGS_TIMEZONES_H

/* The Settings > System > Time Zone catalog.
 *
 * It lives in its own translation unit, apart from the settings UI, so the
 * focused tests and the on-device libc probe can check the table users
 * actually get instead of a copy that drifts from it. Nothing here touches
 * SDL, Catastrophe or the settings database. */

typedef struct {
    const char *label;  /* picker label; translated through T() at draw time */
    const char *tz;     /* IANA id, persisted and exported as TZ. Never translated */
    const char *off;    /* displayed standard-time offset; DST shifts it at runtime */
} jw_timezone_entry;

extern const jw_timezone_entry kJawakaTimeZones[];
extern const int kJawakaTimeZoneCount;

/* Friendly label for a persisted id. "" is the system default; an id that is
   not in the table comes back verbatim, so an upgrade that drops a row still
   shows the user what their setting says. */
const char *jw_timezone_label(const char *tz);

/* Row for a persisted id, falling back to UTC for "" and for unknown ids.
   The fallback is the picker's opening cursor, so it must not be row 0: that
   row is UTC-12, and parking the cursor there would read as a default. */
int jw_timezone_index_of(const char *tz);

/* Row holding the plain "UTC" entry. */
int jw_timezone_utc_index(void);

#endif /* JAWAKA_SETTINGS_TIMEZONES_H */
