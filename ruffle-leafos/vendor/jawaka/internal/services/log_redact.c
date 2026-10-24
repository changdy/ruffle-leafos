#include "internal/services/log_redact.h"

#include <ctype.h>
#include <string.h>

#define JW__REDACT_PLACEHOLDER "[REDACTED]"
#define JW__REDACT_LINE_PLACEHOLDER "[REDACTED LINE]"
#define JW__REDACT_KEY_MAX 64
#define JW__REDACT_NORMALIZED_KEY_MAX ((JW__REDACT_KEY_MAX * 2) - 1)

typedef enum {
    JW__REDACT_KEY_NONE = 0,
    JW__REDACT_KEY_SENSITIVE,
    JW__REDACT_KEY_AUTH,
    JW__REDACT_KEY_AUTHORIZATION,
} jw__redact_key_kind;

/* These normalized suffixes must start at a key-component boundary. That
 * catches namespaced fields such as `oauth_access_token` and
 * `five_game_pin_hash`, but not ordinary fields such as `cookie_count` or
 * `password_policy`. */
static const char *const JW__REDACT_SENSITIVE_SUFFIXES[] = {
    "api_key",  "token",         "secret",        "password",
    "password_hash", "passwd",   "passwd_hash",   "pwd",
    "pwd_hash", "pin",           "pin_hash",      "cookie",
    "set_cookie", "private_key", "priv_key",
};
#define JW__REDACT_SENSITIVE_SUFFIX_COUNT                                  \
    (sizeof(JW__REDACT_SENSITIVE_SUFFIXES) /                               \
     sizeof(JW__REDACT_SENSITIVE_SUFFIXES[0]))

/* Acronyms and all-lowercase compound spellings have no camelCase boundary
 * to preserve during normalization. Keep their common compact forms exact:
 * unlike the suffixes above, these do not match arbitrary word endings. */
static const char *const JW__REDACT_SENSITIVE_COMPACT_KEYS[] = {
    "apikey",       "xapikey",       "accesstoken", "refreshtoken",
    "authtoken",    "xauthtoken",    "bearertoken", "idtoken",
    "clientsecret", "passwordhash",  "passwdhash",  "pwdhash",
    "pinhash",      "setcookie",     "privatekey",   "privkey",
};
#define JW__REDACT_SENSITIVE_COMPACT_KEY_COUNT                             \
    (sizeof(JW__REDACT_SENSITIVE_COMPACT_KEYS) /                           \
     sizeof(JW__REDACT_SENSITIVE_COMPACT_KEYS[0]))

static const char *const JW__REDACT_HASH_PREFIXES[] = {
    "$1$",       "$2$",       "$2a$",      "$2b$",      "$2x$",
    "$2y$",      "$5$",       "$6$",       "$7$",       "$y$",
    "$gy$",      "$argon2d$", "$argon2i$", "$argon2id$",
};
#define JW__REDACT_HASH_PREFIX_COUNT \
    (sizeof(JW__REDACT_HASH_PREFIXES) / sizeof(JW__REDACT_HASH_PREFIXES[0]))

static bool jw__redact_is_key_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '-';
}

static bool jw__redact_is_key_separator(char c) {
    return c == '_' || c == '-' || isspace((unsigned char)c);
}

static bool jw__redact_has_component_suffix(const char *normalized,
                                             size_t normalized_len,
                                             const char *suffix) {
    size_t suffix_len = strlen(suffix);
    if (normalized_len < suffix_len) {
        return false;
    }

    size_t suffix_start = normalized_len - suffix_len;
    return (suffix_start == 0 || normalized[suffix_start - 1] == '_') &&
           strcmp(normalized + suffix_start, suffix) == 0;
}

static jw__redact_key_kind jw__redact_classify_key(const char *key,
                                                    size_t key_len) {
    if (key_len == 0) {
        return JW__REDACT_KEY_NONE;
    }
    if (key_len > JW__REDACT_KEY_MAX) {
        key += key_len - JW__REDACT_KEY_MAX;
        key_len = JW__REDACT_KEY_MAX;
    }

    /* CamelCase normalization can insert at most one separator between each
     * pair of input characters. */
    char normalized[JW__REDACT_NORMALIZED_KEY_MAX + 1];
    size_t normalized_len = 0;
    for (size_t i = 0; i < key_len; i++) {
        unsigned char c = (unsigned char)key[i];
        if (jw__redact_is_key_separator((char)c)) {
            if (normalized_len > 0 && normalized[normalized_len - 1] != '_') {
                normalized[normalized_len++] = '_';
            }
            continue;
        }

        if (isupper(c) && i > 0 &&
            (islower((unsigned char)key[i - 1]) ||
             isdigit((unsigned char)key[i - 1])) &&
            normalized_len > 0 && normalized[normalized_len - 1] != '_') {
            normalized[normalized_len++] = '_';
        }
        normalized[normalized_len++] = (char)tolower(c);
    }
    if (normalized_len > 0 && normalized[normalized_len - 1] == '_') {
        normalized_len--;
    }
    normalized[normalized_len] = '\0';

    /* Jawaka's controller mapper uses "controller pin" to mean a persistent
     * device assignment, not an authentication PIN. */
    if (strcmp(normalized, "controller_pin") == 0) {
        return JW__REDACT_KEY_NONE;
    }
    if (strcmp(normalized, "auth") == 0) {
        return JW__REDACT_KEY_AUTH;
    }
    if (jw__redact_has_component_suffix(normalized, normalized_len,
                                        "authorization")) {
        return JW__REDACT_KEY_AUTHORIZATION;
    }

    for (size_t i = 0; i < JW__REDACT_SENSITIVE_SUFFIX_COUNT; i++) {
        const char *suffix = JW__REDACT_SENSITIVE_SUFFIXES[i];
        if (jw__redact_has_component_suffix(normalized, normalized_len,
                                            suffix)) {
            return JW__REDACT_KEY_SENSITIVE;
        }
    }
    for (size_t i = 0; i < JW__REDACT_SENSITIVE_COMPACT_KEY_COUNT; i++) {
        if (strcmp(normalized, JW__REDACT_SENSITIVE_COMPACT_KEYS[i]) == 0) {
            return JW__REDACT_KEY_SENSITIVE;
        }
    }
    return JW__REDACT_KEY_NONE;
}

static bool jw__redact_is_shadow_username(const char *text, size_t text_len) {
    if (text_len == 0) {
        return false;
    }
    for (size_t i = 0; i < text_len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

static bool jw__redact_is_decimal_or_empty(const char *text, size_t text_len) {
    for (size_t i = 0; i < text_len; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

/* Recognize a complete /etc/shadow record, including locked (`!`/`*`) and
 * empty password fields that do not carry a self-identifying hash marker.
 * The six aging fields must be decimal-or-empty and the reserved field must
 * be empty, which keeps ordinary colon-heavy log messages out. */
static bool jw__redact_is_shadow_entry(const char *line, size_t len) {
    const char *field_start = line;
    const char *line_end = line + len;

    for (size_t field = 0; field < 8; field++) {
        const char *delimiter = memchr(field_start, ':',
                                      (size_t)(line_end - field_start));
        if (!delimiter) {
            return false;
        }

        size_t field_len = (size_t)(delimiter - field_start);
        if (field == 0 && !jw__redact_is_shadow_username(field_start, field_len)) {
            return false;
        }
        if (field >= 2 &&
            !jw__redact_is_decimal_or_empty(field_start, field_len)) {
            return false;
        }
        field_start = delimiter + 1;
    }

    return field_start == line_end;
}

static bool jw__redact_has_marker(const char *line, size_t len) {
    for (size_t i = 0; i < JW__REDACT_HASH_PREFIX_COUNT; i++) {
        if (strstr(line, JW__REDACT_HASH_PREFIXES[i]) != NULL) {
            return true;
        }
    }
    return strstr(line, "PRIVATE KEY") != NULL ||
           jw__redact_is_shadow_entry(line, len);
}

static void jw__redact_append(char *out, size_t out_size, size_t *out_pos,
                               const char *text, size_t text_len) {
    if (*out_pos >= out_size - 1) {
        return;
    }
    size_t space = (out_size - 1) - *out_pos;
    size_t n = text_len < space ? text_len : space;
    memcpy(out + *out_pos, text, n);
    *out_pos += n;
}

static void jw__redact_append_str(char *out, size_t out_size, size_t *out_pos,
                                   const char *text) {
    jw__redact_append(out, out_size, out_pos, text, strlen(text));
}

static bool jw__redact_word_equals(const char *word, size_t word_len,
                                   const char *expected) {
    size_t expected_len = strlen(expected);
    if (word_len != expected_len) {
        return false;
    }
    for (size_t i = 0; i < word_len; i++) {
        if (tolower((unsigned char)word[i]) !=
            tolower((unsigned char)expected[i])) {
            return false;
        }
    }
    return true;
}

/* Finds a possibly quoted, delimiter-adjacent key. Unquoted keys use the
 * documented [A-Za-z0-9_-] alphabet, with one preceding word included for
 * the common `API key`, `private key`, and `password hash` shapes. Quoted
 * structured-data keys may use arbitrary whitespace as a word separator. */
static bool jw__redact_key_before_delimiter(const char *line, size_t delimiter,
                                            size_t *key_start_out,
                                            size_t *key_len_out) {
    size_t key_end = delimiter;
    while (key_end > 0 && isspace((unsigned char)line[key_end - 1])) {
        key_end--;
    }
    if (key_end == 0) {
        return false;
    }

    size_t key_start;
    if (line[key_end - 1] == '"' || line[key_end - 1] == '\'') {
        char quote = line[key_end - 1];
        key_end--;
        key_start = key_end;
        while (key_start > 0 &&
               (jw__redact_is_key_char(line[key_start - 1]) ||
                isspace((unsigned char)line[key_start - 1]))) {
            key_start--;
        }
        if (key_start == 0 || line[key_start - 1] != quote) {
            return false;
        }
        while (key_start < key_end &&
               isspace((unsigned char)line[key_start])) {
            key_start++;
        }
        while (key_end > key_start &&
               isspace((unsigned char)line[key_end - 1])) {
            key_end--;
        }
    } else {
        key_start = key_end;
        while (key_start > 0 && jw__redact_is_key_char(line[key_start - 1])) {
            key_start--;
        }

        size_t last_word_len = key_end - key_start;
        if (jw__redact_word_equals(line + key_start, last_word_len, "key") ||
            jw__redact_word_equals(line + key_start, last_word_len, "hash") ||
            jw__redact_word_equals(line + key_start, last_word_len, "pin")) {
            size_t previous_end = key_start;
            while (previous_end > 0 &&
                   isspace((unsigned char)line[previous_end - 1])) {
                previous_end--;
            }
            size_t previous_start = previous_end;
            while (previous_start > 0 &&
                   jw__redact_is_key_char(line[previous_start - 1])) {
                previous_start--;
            }
            if (previous_start < previous_end) {
                key_start = previous_start;
            }
        }
    }

    *key_start_out = key_start;
    *key_len_out = key_end - key_start;
    return true;
}

/* Finds the end of the value starting at `value_start` for an '='
 * delimiter: a quoted span up to (and including) its closing quote, or
 * up to end of line if unterminated; otherwise up to the next
 * whitespace character or end of line. */
static size_t jw__redact_equals_value_end(const char *line, size_t len, size_t value_start) {
    if (value_start < len && (line[value_start] == '"' || line[value_start] == '\'')) {
        char quote = line[value_start];
        for (size_t i = value_start + 1; i < len; i++) {
            if (line[i] == '\\' && i + 1 < len) {
                i++;
                continue;
            }
            if (line[i] == quote) {
                return i + 1;
            }
        }
        return len;
    }
    size_t i = value_start;
    while (i < len && !isspace((unsigned char)line[i])) {
        i++;
    }
    return i;
}

static bool jw__redact_text_equals(const char *text, size_t text_len,
                                   const char *expected) {
    size_t expected_len = strlen(expected);
    if (text_len != expected_len) {
        return false;
    }
    for (size_t i = 0; i < text_len; i++) {
        if (tolower((unsigned char)text[i]) !=
            tolower((unsigned char)expected[i])) {
            return false;
        }
    }
    return true;
}

/* A bare `auth` field commonly reports configuration state rather than a
 * credential. Keep a small exact allowlist; every other non-empty value is
 * treated as sensitive. */
static bool jw__redact_auth_is_status(const char *line, size_t len,
                                      size_t value_start) {
    static const char *const statuses[] = {
        "disabled", "enabled",  "false",    "none", "off", "on",
        "optional", "required", "true",     "no",   "yes",
    };
    const size_t status_count = sizeof(statuses) / sizeof(statuses[0]);

    size_t value_end = jw__redact_equals_value_end(line, len, value_start);
    if (value_start == value_end) {
        return true;
    }

    size_t text_start = value_start;
    size_t text_end = value_end;
    if (line[text_start] == '"' || line[text_start] == '\'') {
        char quote = line[text_start];
        text_start++;
        if (text_end <= text_start || line[text_end - 1] != quote) {
            return false;
        }
        text_end--;
    }

    for (size_t i = 0; i < status_count; i++) {
        if (jw__redact_text_equals(line + text_start, text_end - text_start,
                                   statuses[i])) {
            return true;
        }
    }
    return false;
}

static size_t jw__redact_authorization_value_end(const char *line, size_t len,
                                                  size_t value_start) {
    size_t value_end = jw__redact_equals_value_end(line, len, value_start);
    if (value_start >= len || line[value_start] == '"' ||
        line[value_start] == '\'') {
        return value_end;
    }

    bool has_scheme =
        jw__redact_text_equals(line + value_start, value_end - value_start,
                               "bearer") ||
        jw__redact_text_equals(line + value_start, value_end - value_start,
                               "basic");
    if (!has_scheme || value_end >= len ||
        !isspace((unsigned char)line[value_end])) {
        return value_end;
    }

    size_t credential_start = value_end;
    while (credential_start < len &&
           isspace((unsigned char)line[credential_start])) {
        credential_start++;
    }
    return jw__redact_equals_value_end(line, len, credential_start);
}

bool jw_svc_log_redact_line(const char *line, char *out, size_t out_size) {
    if (!line || !out || out_size == 0) {
        return false;
    }

    size_t len = strlen(line);
    if (jw__redact_has_marker(line, len)) {
        size_t out_pos = 0;
        jw__redact_append_str(out, out_size, &out_pos, JW__REDACT_LINE_PLACEHOLDER);
        out[out_pos] = '\0';
        return true;
    }

    bool redacted = false;
    size_t out_pos = 0;
    size_t cursor = 0;
    size_t scan = 0;

    while (scan < len) {
        if (line[scan] != ':' && line[scan] != '=') {
            scan++;
            continue;
        }

        size_t delim_pos = scan;
        size_t key_start = 0;
        size_t key_len = 0;

        if (!jw__redact_key_before_delimiter(line, delim_pos, &key_start,
                                             &key_len)) {
            scan = delim_pos + 1;
            continue;
        }

        jw__redact_key_kind key_kind =
            jw__redact_classify_key(line + key_start, key_len);
        if (key_kind == JW__REDACT_KEY_NONE) {
            scan = delim_pos + 1;
            continue;
        }

        size_t value_start = delim_pos + 1;
        while (value_start < len &&
               isspace((unsigned char)line[value_start])) {
            value_start++;
        }
        if (key_kind == JW__REDACT_KEY_AUTH &&
            jw__redact_auth_is_status(line, len, value_start)) {
            scan = delim_pos + 1;
            continue;
        }

        /* Preserve the source's delimiter-adjacent whitespace. Jawaka's
         * logger does not impose a message convention, and both compact
         * logfmt and spaced header/structured-data forms occur in practice. */
        jw__redact_append(out, out_size, &out_pos, line + cursor,
                          value_start - cursor);
        jw__redact_append_str(out, out_size, &out_pos,
                              JW__REDACT_PLACEHOLDER);
        redacted = true;

        if (line[delim_pos] == ':') {
            /* Header-style convention: one value per line. Nothing more
             * on this line is worth scanning. */
            cursor = len;
            break;
        }

        size_t value_end =
            (key_kind == JW__REDACT_KEY_AUTH ||
             key_kind == JW__REDACT_KEY_AUTHORIZATION)
                ? jw__redact_authorization_value_end(line, len, value_start)
                : jw__redact_equals_value_end(line, len, value_start);
        cursor = value_end;
        scan = value_end;
    }

    jw__redact_append(out, out_size, &out_pos, line + cursor, len - cursor);
    out[out_pos] = '\0';
    return redacted;
}
