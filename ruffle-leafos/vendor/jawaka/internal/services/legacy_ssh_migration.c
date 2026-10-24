#define _GNU_SOURCE
#include "internal/services/legacy_ssh_migration.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define JW__LEGACY_SSH_CONFIG_MAX (64u * 1024u)
#define JW__LEGACY_SSH_LINE_MAX 4095u

typedef enum {
    JW__CONFIG_READ_ERROR = -1,
    JW__CONFIG_ABSENT = 0,
    JW__CONFIG_PRESENT = 1,
} jw__config_read_result;

static void jw__set_reason(char *reason, size_t reason_size,
                           const char *value) {
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", value);
    }
}

static bool jw__valid_username(const char *value) {
    size_t length = value ? strlen(value) : 0;
    if (length == 0 || length >= 64u ||
        (!isalpha((unsigned char)value[0]) && value[0] != '_')) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!isalnum(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

static bool jw__valid_port(const char *value) {
    if (!value || !value[0]) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    return errno == 0 && end && *end == '\0' && parsed >= 1 && parsed <= 65535;
}

static bool jw__valid_endpoint(const char *value) {
    if (!value || !value[0] || strlen(value) >= 128u ||
        strchr(value, ' ') || strchr(value, '\t')) {
        return false;
    }
    const char *colon = strrchr(value, ':');
    return colon && colon != value && jw__valid_port(colon + 1);
}

static bool jw__valid_start_dir(const char *value) {
    size_t length = value ? strlen(value) : 0;
    return length > 0 && length < (size_t)PATH_MAX && value[0] == '/' &&
           !strchr(value, '\n') && !strchr(value, '\r');
}

static bool jw__valid_legacy_password(const char *value) {
    size_t length = value ? strlen(value) : 0;
    return length > 0 && length < 128u && !strchr(value, ':') &&
           !strchr(value, '\n') && !strchr(value, '\r');
}

static bool jw__valid_password_hash(const char *value) {
    size_t length = value ? strlen(value) : 0;
    return length >= 20u && length < 512u && value[0] == '$' &&
           !strchr(value, ':') && !strchr(value, '\n') &&
           !strchr(value, '\r');
}

static bool jw__parse_bool(const char *value, bool *out) {
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static void jw__unescape(char *value) {
    size_t source = 0;
    size_t target = 0;
    while (value[source]) {
        if (value[source] == '\\' && value[source + 1]) {
            source++;
            switch (value[source]) {
                case 'n': value[target++] = '\n'; break;
                case 'r': value[target++] = '\r'; break;
                case 't': value[target++] = '\t'; break;
                case '=': value[target++] = '='; break;
                case '\\': value[target++] = '\\'; break;
                default: value[target++] = value[source]; break;
            }
            source++;
            continue;
        }
        value[target++] = value[source++];
    }
    value[target] = '\0';
}

static bool jw__config_is_valid(char *text, size_t length) {
    bool username_seen = false;
    bool username_valid = false;
    bool endpoint_seen = false;
    bool endpoint_valid = false;
    bool start_seen = false;
    bool start_valid = false;
    bool legacy_password_seen = false;
    bool legacy_password_valid = false;
    bool hash_seen = false;
    bool hash_valid = false;
    bool configured_seen = false;
    bool configured_valid = false;
    bool configured = false;
    bool auth_seen = false;
    bool auth_valid = false;
    bool auth_enabled = true;

    char *cursor = text;
    char *limit = text + length;
    while (cursor < limit) {
        char *newline = memchr(cursor, '\n', (size_t)(limit - cursor));
        char *line_end = newline ? newline : limit;
        size_t line_length = (size_t)(line_end - cursor);
        if (line_length > JW__LEGACY_SSH_LINE_MAX) {
            return false;
        }
        *line_end = '\0';
        if (line_length > 0 && cursor[line_length - 1u] == '\r') {
            cursor[line_length - 1u] = '\0';
        }

        char *key = cursor;
        while (*key && isspace((unsigned char)*key)) {
            key++;
        }
        if (*key && *key != '#') {
            char *separator = strchr(key, '=');
            if (!separator) {
                return false;
            }
            *separator = '\0';
            char *value = separator + 1;
            jw__unescape(value);

            if (strcmp(key, "username") == 0) {
                username_seen = true;
                username_valid = jw__valid_username(value);
            } else if (strcmp(key, "bind_address") == 0) {
                endpoint_seen = true;
                endpoint_valid = jw__valid_endpoint(value);
            } else if (strcmp(key, "port") == 0) {
                endpoint_seen = true;
                endpoint_valid = jw__valid_port(value);
            } else if (strcmp(key, "start_dir") == 0) {
                start_seen = true;
                start_valid = jw__valid_start_dir(value);
            } else if (strcmp(key, "password") == 0) {
                legacy_password_seen = true;
                legacy_password_valid = jw__valid_legacy_password(value);
            } else if (strcmp(key, "password_hash") == 0) {
                hash_seen = true;
                hash_valid = jw__valid_password_hash(value);
            } else if (strcmp(key, "password_configured") == 0) {
                configured_seen = true;
                configured_valid = jw__parse_bool(value, &configured);
            } else if (strcmp(key, "password_auth_enabled") == 0) {
                auth_seen = true;
                auth_valid = jw__parse_bool(value, &auth_enabled);
            }
        }
        cursor = newline ? newline + 1 : limit;
    }

    bool legacy_credential =
        legacy_password_seen && legacy_password_valid;
    bool hash_credential = configured_seen && configured_valid && configured &&
                           hash_seen && hash_valid;
    bool key_only = auth_seen && auth_valid && !auth_enabled;
    return username_seen && username_valid && endpoint_seen && endpoint_valid &&
           start_seen && start_valid &&
           (!configured_seen || configured_valid) &&
           (!auth_seen || auth_valid) &&
           (legacy_credential || hash_credential || key_only);
}

static jw__config_read_result jw__read_config(const char *path, char **out,
                                               size_t *out_length,
                                               bool *out_structurally_valid) {
    *out = NULL;
    *out_length = 0;
    *out_structurally_valid = false;

    struct stat before;
    if (lstat(path, &before) != 0) {
        return errno == ENOENT ? JW__CONFIG_ABSENT : JW__CONFIG_READ_ERROR;
    }
    if (!S_ISREG(before.st_mode) || before.st_size <= 0 ||
        (unsigned long long)before.st_size > JW__LEGACY_SSH_CONFIG_MAX) {
        return JW__CONFIG_PRESENT;
    }

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path, flags);
    if (fd < 0) {
        return JW__CONFIG_READ_ERROR;
    }
    struct stat opened;
    if (fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) ||
        opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
        opened.st_size != before.st_size) {
        close(fd);
        return JW__CONFIG_READ_ERROR;
    }

    size_t wanted = (size_t)opened.st_size;
    char *buffer = malloc(wanted + 1u);
    if (!buffer) {
        close(fd);
        return JW__CONFIG_READ_ERROR;
    }
    size_t used = 0;
    while (used < wanted) {
        ssize_t amount = read(fd, buffer + used, wanted - used);
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            free(buffer);
            close(fd);
            return JW__CONFIG_READ_ERROR;
        }
        used += (size_t)amount;
    }
    char extra;
    ssize_t extra_amount;
    do {
        extra_amount = read(fd, &extra, 1u);
    } while (extra_amount < 0 && errno == EINTR);
    if (close(fd) != 0 || extra_amount != 0) {
        free(buffer);
        return JW__CONFIG_READ_ERROR;
    }
    buffer[used] = '\0';
    if (memchr(buffer, '\0', used)) {
        free(buffer);
        return JW__CONFIG_PRESENT;
    }
    *out = buffer;
    *out_length = used;
    *out_structurally_valid = true;
    return JW__CONFIG_PRESENT;
}

bool jw_svc_migrate_legacy_ssh_intent(
    jw_svc_control_store *store, const char *config_path,
    jw_svc_legacy_ssh_migration_report *out,
    char *reason, size_t reason_size) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!store || !config_path || !config_path[0] || !out) {
        jw__set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    bool already_complete = false;
    if (!jw_svc_control_store_has_migration(
            store, JW_SVC_LEGACY_SSH_MIGRATION_ID, &already_complete,
            reason, reason_size)) {
        return false;
    }
    if (already_complete) {
        return true;
    }

    char *config = NULL;
    size_t config_length = 0;
    bool structurally_valid = false;
    jw__config_read_result read_result =
        jw__read_config(config_path, &config, &config_length,
                        &structurally_valid);
    if (read_result == JW__CONFIG_READ_ERROR) {
        jw__set_reason(reason, reason_size, "config-read-failed");
        return false;
    }

    out->config_present = read_result == JW__CONFIG_PRESENT;
    out->config_valid = structurally_valid &&
                        jw__config_is_valid(config, config_length);
    free(config);
    out->enabled = out->config_valid;

    char store_reason[32];
    if (!jw_svc_control_store_apply_intent_migration(
            store, JW_SVC_LEGACY_SSH_MIGRATION_ID,
            JW_SVC_LEGACY_SSH_SERVICE_ID, out->enabled, &out->applied,
            store_reason, sizeof(store_reason))) {
        jw__set_reason(reason, reason_size, store_reason);
        return false;
    }
    return true;
}
