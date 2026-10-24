#include "internal/retroarch/command.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

const char *jw_ra_result_string(jw_ra_result result) {
    switch (result) {
        case JW_RA_OK: return "ok";
        case JW_RA_TIMEOUT: return "timeout";
        case JW_RA_UNSUPPORTED: return "unsupported";
        case JW_RA_PARSE_ERROR: return "parse_error";
        case JW_RA_SOCKET_ERROR: return "socket_error";
        default: return "unknown";
    }
}

const char *jw_ra_play_state_string(jw_ra_play_state state) {
    switch (state) {
        case JW_RA_STATE_CONTENTLESS: return "contentless";
        case JW_RA_STATE_PLAYING: return "playing";
        case JW_RA_STATE_PAUSED: return "paused";
        case JW_RA_STATE_MENU: return "menu";
        case JW_RA_STATE_UNKNOWN:
        default: return "unknown";
    }
}

jw_ra_client jw_ra_client_default(void) {
    jw_ra_client client;
    client.host = JW_RA_DEFAULT_HOST;
    client.port = JW_RA_DEFAULT_PORT;
    client.timeout_ms = JW_RA_DEFAULT_TIMEOUT_MS;
    return client;
}

static bool jw_ra__starts_with_word(const char *command, const char *prefix) {
    size_t len;
    if (!command || !prefix) {
        return false;
    }

    len = strlen(prefix);
    return strncmp(command, prefix, len) == 0 &&
           (command[len] == '\0' || isspace((unsigned char)command[len]));
}

static bool jw_ra__exact_supported(const char *command) {
    static const char *const supported[] = {
        "CLOSE_CONTENT",
        "DISK_EJECT_TOGGLE",
        "DISK_NEXT",
        "DISK_PREV",
        "FPS_TOGGLE",
        "FULLSCREEN_TOGGLE",
        "GET_DISK_COUNT",
        "GET_DISK_SLOT",
        "GET_INFO",
        "GET_PERF_INFO",
        "GET_STATE_SAVE_INFO",
        "GET_STATE_SLOT",
        "GET_STATUS",
        "LOAD_STATE",
        "MENU_TOGGLE",
        "MUTE",
        "OPEN_MENU",
        "PAUSE",
        "PAUSE_TOGGLE",
        "QUIT",
        "AUDIO_REINIT",
        "RECORDING_TOGGLE",
        "RESET",
        "SAVE_STATE",
        "SCREENSHOT",
        "STATE_SLOT_MINUS",
        "STATE_SLOT_PLUS",
        "UNPAUSE",
        "VERSION",
        "VOLUME_DOWN",
        "VOLUME_UP",
    };
    size_t i;

    for (i = 0; i < sizeof(supported) / sizeof(supported[0]); i++) {
        if (strcmp(command, supported[i]) == 0) {
            return true;
        }
    }

    return false;
}

bool jw_ra_raw_command_supported(const char *command) {
    const char *p = command;

    if (!command || !command[0]) {
        return false;
    }

    while (*p) {
        if ((unsigned char)*p < 0x20 && *p != '\t') {
            return false;
        }
        p++;
    }

    if (jw_ra__exact_supported(command)) {
        return true;
    }

    return jw_ra__starts_with_word(command, "GET_CONFIG_PARAM") ||
           jw_ra__starts_with_word(command, "GET_PATH") ||
           jw_ra__starts_with_word(command, "JAWAKA_CLEAR_SHADER") ||
           jw_ra__starts_with_word(command, "JAWAKA_GET_SHADER") ||
           jw_ra__starts_with_word(command, "JAWAKA_LOAD_CONTENT") ||
           jw_ra__starts_with_word(command, "JAWAKA_REMOVE_SHADER_PRESET") ||
           jw_ra__starts_with_word(command, "JAWAKA_SAVE_SHADER_PRESET") ||
           jw_ra__starts_with_word(command, "JAWAKA_SET_SHADER") ||
           jw_ra__starts_with_word(command, "LOAD_CORE") ||
           jw_ra__starts_with_word(command, "LOAD_STATE_SLOT") ||
           jw_ra__starts_with_word(command, "OPEN_MENU") ||
           jw_ra__starts_with_word(command, "PLAY_REPLAY_SLOT") ||
           jw_ra__starts_with_word(command, "READ_CORE_MEMORY") ||
           jw_ra__starts_with_word(command, "SEEK_REPLAY") ||
           jw_ra__starts_with_word(command, "SAVE_STATE_SLOT") ||
           jw_ra__starts_with_word(command, "SET_DISK_SLOT") ||
           jw_ra__starts_with_word(command, "SET_SHADER") ||
           jw_ra__starts_with_word(command, "SET_STATE_SLOT") ||
           jw_ra__starts_with_word(command, "SHOW_MSG") ||
           jw_ra__starts_with_word(command, "WRITE_CORE_MEMORY");
}

static jw_ra_result jw_ra__resolve(const jw_ra_client *client,
                                   struct sockaddr_storage *out_addr,
                                   socklen_t *out_addr_len) {
    char port[16];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *it = NULL;
    int rc;

    if (!client || !client->host || !out_addr || !out_addr_len) {
        return JW_RA_SOCKET_ERROR;
    }

    snprintf(port, sizeof(port), "%u", client->port ? client->port : JW_RA_DEFAULT_PORT);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    rc = getaddrinfo(client->host, port, &hints, &res);
    if (rc != 0 || !res) {
        return JW_RA_SOCKET_ERROR;
    }

    for (it = res; it; it = it->ai_next) {
        if (it->ai_addrlen <= sizeof(*out_addr)) {
            memset(out_addr, 0, sizeof(*out_addr));
            memcpy(out_addr, it->ai_addr, it->ai_addrlen);
            *out_addr_len = (socklen_t)it->ai_addrlen;
            freeaddrinfo(res);
            return JW_RA_OK;
        }
    }

    freeaddrinfo(res);
    return JW_RA_SOCKET_ERROR;
}

static jw_ra_result jw_ra__exchange(const jw_ra_client *client,
                                    const char *command,
                                    bool expect_reply,
                                    char *reply,
                                    size_t reply_size) {
    jw_ra_result result;
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    int fd = -1;
    ssize_t sent;
    unsigned timeout_ms;

    if (!jw_ra_raw_command_supported(command)) {
        return JW_RA_UNSUPPORTED;
    }

    if (expect_reply && (!reply || reply_size == 0)) {
        return JW_RA_PARSE_ERROR;
    }

    if (reply && reply_size > 0) {
        reply[0] = '\0';
    }

    result = jw_ra__resolve(client, &addr, &addr_len);
    if (result != JW_RA_OK) {
        return result;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return JW_RA_SOCKET_ERROR;
    }

    sent = sendto(fd, command, strlen(command), 0, (struct sockaddr *)&addr, addr_len);
    if (sent < 0 || (size_t)sent != strlen(command)) {
        close(fd);
        return JW_RA_SOCKET_ERROR;
    }

    if (!expect_reply) {
        close(fd);
        return JW_RA_OK;
    }

    timeout_ms = client && client->timeout_ms ? client->timeout_ms : JW_RA_DEFAULT_TIMEOUT_MS;
    for (;;) {
        fd_set read_fds;
        struct timeval tv;
        int ready;

        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        tv.tv_sec = (time_t)(timeout_ms / 1000u);
        tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);

        ready = select(fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready == 0) {
            close(fd);
            return JW_RA_TIMEOUT;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return JW_RA_SOCKET_ERROR;
        }
        break;
    }

    {
        ssize_t nread = recvfrom(fd, reply, reply_size - 1u, 0, NULL, NULL);
        if (nread < 0) {
            close(fd);
            return JW_RA_SOCKET_ERROR;
        }
        reply[nread] = '\0';
    }

    close(fd);
    return JW_RA_OK;
}

static void jw_ra__trim_line(char *s);

/* ---------------------------------------------------------- shader commands */

const char *jw_ra_shader_scope_token(jw_ra_shader_scope scope) {
    switch (scope) {
        case JW_RA_SHADER_SCOPE_GAME:   return "GAME";
        case JW_RA_SHADER_SCOPE_PARENT: return "PARENT";
        case JW_RA_SHADER_SCOPE_CORE:   return "CORE";
        case JW_RA_SHADER_SCOPE_GLOBAL: return "GLOBAL";
    }
    return NULL;
}

/* Lowercase hex, bounded. The ID is echoed back into a reply we then compare,
 * so it must not contain anything that could be read as another field. */
static void jw_ra__make_request_id(char *out, size_t out_size) {
    static const char digits[] = "0123456789abcdef";
    static unsigned counter;
    unsigned long long seed;
    struct timespec ts;
    size_t i;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    seed = (unsigned long long)ts.tv_sec * 1000000000ull +
           (unsigned long long)ts.tv_nsec;
    seed ^= (unsigned long long)getpid() << 32;
    seed += ++counter;

    for (i = 0; i + 1 < out_size && i < 12; i++) {
        out[i] = digits[seed & 0xfu];
        seed >>= 4;
    }
    out[i] = '\0';
}

static long jw_ra__now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

/* Parse exactly the documented reply forms. Anything else is a parse error:
 * a partially-recognised reply is worse than none, because it would be acted
 * on. `operation` is the verb this exchange asked for; a reply describing a
 * different one belongs to another request. */
jw_ra_result jw_ra_parse_shader_reply(const char *reply,
                                      const char *request_id,
                                      const char *operation,
                                      jw_ra_shader_outcome *outcome,
                                      char *path, size_t path_size) {
    const char *p = reply;
    size_t id_len, op_len;

    if (!reply || !request_id || !operation || !outcome) {
        return JW_RA_PARSE_ERROR;
    }
    if (strncmp(p, "JAWAKA_SHADER ", 14) != 0) {
        return JW_RA_PARSE_ERROR;
    }
    p += 14;

    id_len = strlen(request_id);
    if (strncmp(p, request_id, id_len) != 0 || p[id_len] == '\0') {
        /* Either a different request ID, or ours with nothing after it. The
         * first is somebody else's reply and must be ignored; the second is a
         * truncated reply to ours and is malformed. */
        if (strncmp(p, request_id, id_len) == 0) {
            return JW_RA_PARSE_ERROR;
        }
        return JW_RA_TIMEOUT;
    }
    if (p[id_len] != ' ') {
        /* Our ID is a prefix of a longer one: still somebody else's request. */
        return JW_RA_TIMEOUT;
    }
    p += id_len + 1;

    op_len = strlen(operation);
    if (strncmp(p, operation, op_len) != 0 || p[op_len] != ' ') {
        return JW_RA_PARSE_ERROR;
    }
    p += op_len + 1;

    /* SAVE and REMOVE echo the scope before the outcome; skip it. */
    if (strcmp(operation, "SAVE") == 0 || strcmp(operation, "REMOVE") == 0) {
        const char *space = strchr(p, ' ');
        if (!space) {
            return JW_RA_PARSE_ERROR;
        }
        p = space + 1;
    }

    if (strcmp(p, "OK") == 0) {
        *outcome = JW_RA_SHADER_OK;
        return JW_RA_OK;
    }
    if (strcmp(p, "NONE") == 0) {
        *outcome = JW_RA_SHADER_NONE;
        return JW_RA_OK;
    }
    if (strcmp(p, "ABSENT") == 0) {
        *outcome = JW_RA_SHADER_ABSENT;
        return JW_RA_OK;
    }
    if (strcmp(p, "ERROR") == 0) {
        *outcome = JW_RA_SHADER_ERR;
        return JW_RA_OK;
    }
    if (strncmp(p, "ERROR ", 6) == 0) {
        const char *reason = p + 6;
        if (strcmp(reason, "missing") == 0) {
            *outcome = JW_RA_SHADER_ERR_MISSING;
        } else if (strcmp(reason, "unsupported") == 0) {
            *outcome = JW_RA_SHADER_ERR_UNSUPPORTED;
        } else if (strcmp(reason, "apply") == 0) {
            *outcome = JW_RA_SHADER_ERR_APPLY;
        } else {
            return JW_RA_PARSE_ERROR;
        }
        return JW_RA_OK;
    }
    if (strncmp(p, "OK ", 3) == 0) {
        /* GET OK <path>. The path is the rest of the line verbatim: it may
         * contain spaces. */
        if (!path || path_size == 0) {
            return JW_RA_PARSE_ERROR;
        }
        if (strlen(p + 3) >= path_size) {
            return JW_RA_PARSE_ERROR;
        }
        snprintf(path, path_size, "%s", p + 3);
        *outcome = JW_RA_SHADER_OK;
        return JW_RA_OK;
    }
    return JW_RA_PARSE_ERROR;
}

/* One exchange, one request ID, one absolute deadline.
 *
 * The socket is connect()ed so the kernel drops datagrams from anywhere other
 * than RetroArch: that is the source validation, and it costs nothing. Replies
 * carrying another request ID are ignored, but they never extend the deadline
 * -- otherwise a chatty peer could hold the UI open indefinitely. */
static jw_ra_result jw_ra__shader_exchange(const jw_ra_client *client,
                                           const char *command,
                                           const char *request_id,
                                           const char *operation,
                                           jw_ra_shader_outcome *outcome,
                                           char *path, size_t path_size) {
    jw_ra_result result;
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    int fd = -1;
    size_t command_len;
    unsigned timeout_ms;
    long deadline;

    if (!command || !request_id || !operation || !outcome) {
        return JW_RA_PARSE_ERROR;
    }
    if (!jw_ra_raw_command_supported(command)) {
        return JW_RA_UNSUPPORTED;
    }

    result = jw_ra__resolve(client, &addr, &addr_len);
    if (result != JW_RA_OK) {
        return result;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return JW_RA_SOCKET_ERROR;
    }
    if (connect(fd, (struct sockaddr *)&addr, addr_len) != 0) {
        close(fd);
        return JW_RA_SOCKET_ERROR;
    }

    command_len = strlen(command);
    if (send(fd, command, command_len, 0) != (ssize_t)command_len) {
        close(fd);
        return JW_RA_SOCKET_ERROR;
    }

    timeout_ms = client && client->timeout_ms ? client->timeout_ms
                                             : JW_RA_SHADER_TIMEOUT_MS;
    deadline = jw_ra__now_ms() + (long)timeout_ms;

    for (;;) {
        char reply[JW_RA_REPLY_MAX];
        fd_set read_fds;
        struct timeval tv;
        long remaining = deadline - jw_ra__now_ms();
        ssize_t nread;
        int ready;

        if (remaining <= 0) {
            close(fd);
            return JW_RA_TIMEOUT;
        }

        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        tv.tv_sec = (time_t)(remaining / 1000L);
        tv.tv_usec = (suseconds_t)((remaining % 1000L) * 1000L);

        ready = select(fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready == 0) {
            close(fd);
            return JW_RA_TIMEOUT;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return JW_RA_SOCKET_ERROR;
        }

        nread = recv(fd, reply, sizeof(reply) - 1u, 0);
        if (nread < 0) {
            close(fd);
            return JW_RA_SOCKET_ERROR;
        }
        reply[nread] = '\0';
        jw_ra__trim_line(reply);

        result = jw_ra_parse_shader_reply(reply, request_id, operation,
                                          outcome, path, path_size);
        if (result == JW_RA_TIMEOUT) {
            /* Somebody else's reply. Keep waiting against the same deadline. */
            continue;
        }
        close(fd);
        return result;
    }
}

jw_ra_result jw_ra_get_shader(const jw_ra_client *client,
                              jw_ra_shader_outcome *outcome,
                              char *path, size_t path_size) {
    char id[JW_RA_REQUEST_ID_MAX];
    char command[64];

    if (path && path_size > 0) {
        path[0] = '\0';
    }
    jw_ra__make_request_id(id, sizeof(id));
    snprintf(command, sizeof(command), "JAWAKA_GET_SHADER %s", id);
    return jw_ra__shader_exchange(client, command, id, "GET", outcome,
                                  path, path_size);
}

jw_ra_result jw_ra_set_shader(const jw_ra_client *client,
                              const char *preset_path,
                              jw_ra_shader_outcome *outcome) {
    char id[JW_RA_REQUEST_ID_MAX];
    char command[JW_RA_REPLY_MAX];
    int written;

    if (!preset_path || !preset_path[0]) {
        return JW_RA_PARSE_ERROR;
    }
    jw_ra__make_request_id(id, sizeof(id));
    written = snprintf(command, sizeof(command), "JAWAKA_SET_SHADER %s %s",
                       id, preset_path);
    if (written < 0 || (size_t)written >= sizeof(command)) {
        return JW_RA_PARSE_ERROR;
    }
    return jw_ra__shader_exchange(client, command, id, "SET", outcome, NULL, 0);
}

jw_ra_result jw_ra_clear_shader(const jw_ra_client *client,
                                jw_ra_shader_outcome *outcome) {
    char id[JW_RA_REQUEST_ID_MAX];
    char command[64];

    jw_ra__make_request_id(id, sizeof(id));
    snprintf(command, sizeof(command), "JAWAKA_CLEAR_SHADER %s", id);
    return jw_ra__shader_exchange(client, command, id, "CLEAR", outcome,
                                  NULL, 0);
}

static jw_ra_result jw_ra__shader_preset_op(const jw_ra_client *client,
                                            const char *verb,
                                            const char *operation,
                                            jw_ra_shader_scope scope,
                                            jw_ra_shader_outcome *outcome) {
    char id[JW_RA_REQUEST_ID_MAX];
    char command[128];
    const char *token = jw_ra_shader_scope_token(scope);

    if (!token) {
        return JW_RA_PARSE_ERROR;
    }
    jw_ra__make_request_id(id, sizeof(id));
    snprintf(command, sizeof(command), "%s %s %s", verb, id, token);
    return jw_ra__shader_exchange(client, command, id, operation, outcome,
                                  NULL, 0);
}

jw_ra_result jw_ra_save_shader_preset(const jw_ra_client *client,
                                      jw_ra_shader_scope scope,
                                      jw_ra_shader_outcome *outcome) {
    return jw_ra__shader_preset_op(client, "JAWAKA_SAVE_SHADER_PRESET",
                                   "SAVE", scope, outcome);
}

jw_ra_result jw_ra_remove_shader_preset(const jw_ra_client *client,
                                        jw_ra_shader_scope scope,
                                        jw_ra_shader_outcome *outcome) {
    return jw_ra__shader_preset_op(client, "JAWAKA_REMOVE_SHADER_PRESET",
                                   "REMOVE", scope, outcome);
}

jw_ra_result jw_ra_send_raw(const jw_ra_client *client, const char *command) {
    return jw_ra__exchange(client, command, false, NULL, 0);
}

jw_ra_result jw_ra_request_raw(const jw_ra_client *client, const char *command,
                               char *reply, size_t reply_size) {
    return jw_ra__exchange(client, command, true, reply, reply_size);
}

static void jw_ra__trim_line(char *s) {
    size_t len;
    if (!s) {
        return;
    }

    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[len - 1] = '\0';
        len--;
    }
}

static void jw_ra__copy_token(char *dst, size_t dst_size, const char *start, size_t len) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!start) {
        dst[0] = '\0';
        return;
    }
    if (len >= dst_size) {
        len = dst_size - 1u;
    }
    memcpy(dst, start, len);
    dst[len] = '\0';
}

jw_ra_result jw_ra_parse_status_reply(const char *reply, jw_ra_status *status) {
    const char *p;
    const char *token_end;
    const char *comma;
    const char *next_comma;

    if (!reply || !status) {
        return JW_RA_PARSE_ERROR;
    }

    memset(status, 0, sizeof(*status));
    status->state = JW_RA_STATE_UNKNOWN;
    snprintf(status->raw, sizeof(status->raw), "%s", reply);
    jw_ra__trim_line(status->raw);

    if (strcmp(status->raw, "GET_STATUS CONTENTLESS") == 0) {
        status->state = JW_RA_STATE_CONTENTLESS;
        return JW_RA_OK;
    }

    p = status->raw;
    if (strncmp(p, "GET_STATUS ", 11) != 0) {
        return JW_RA_PARSE_ERROR;
    }
    p += 11;

    token_end = strchr(p, ' ');
    if (!token_end) {
        return JW_RA_PARSE_ERROR;
    }

    if ((size_t)(token_end - p) == strlen("PLAYING") &&
        strncmp(p, "PLAYING", (size_t)(token_end - p)) == 0) {
        status->state = JW_RA_STATE_PLAYING;
    } else if ((size_t)(token_end - p) == strlen("PAUSED") &&
               strncmp(p, "PAUSED", (size_t)(token_end - p)) == 0) {
        status->state = JW_RA_STATE_PAUSED;
    } else if ((size_t)(token_end - p) == strlen("MENU") &&
               strncmp(p, "MENU", (size_t)(token_end - p)) == 0) {
        status->state = JW_RA_STATE_MENU;
    } else {
        return JW_RA_PARSE_ERROR;
    }

    p = token_end + 1;
    comma = strchr(p, ',');
    if (!comma) {
        jw_ra__copy_token(status->system, sizeof(status->system), p, strlen(p));
        return JW_RA_OK;
    }

    jw_ra__copy_token(status->system, sizeof(status->system), p, (size_t)(comma - p));

    p = comma + 1;
    next_comma = strchr(p, ',');
    if (next_comma) {
        jw_ra__copy_token(status->content, sizeof(status->content), p, (size_t)(next_comma - p));
    } else {
        jw_ra__copy_token(status->content, sizeof(status->content), p, strlen(p));
    }

    return JW_RA_OK;
}

static jw_ra_result jw_ra__parse_prefixed_int(const char *reply,
                                              const char *prefix,
                                              int *out) {
    char *end = NULL;
    long value;
    size_t prefix_len;

    if (!reply || !prefix || !out) {
        return JW_RA_PARSE_ERROR;
    }

    prefix_len = strlen(prefix);
    if (strncmp(reply, prefix, prefix_len) != 0) {
        return JW_RA_PARSE_ERROR;
    }

    errno = 0;
    value = strtol(reply + prefix_len, &end, 10);
    if (errno != 0 || end == reply + prefix_len ||
        (end && *end != '\0' && *end != '\n' && *end != '\r')) {
        return JW_RA_PARSE_ERROR;
    }
    if (value < -1 || value > 999999L) {
        return JW_RA_PARSE_ERROR;
    }

    *out = (int)value;
    return JW_RA_OK;
}

static jw_ra_result jw_ra__parse_info(const char *reply, jw_ra_info *info) {
    char raw[JW_RA_REPLY_MAX];
    char state_token[32];
    int nread;

    if (!reply || !info) {
        return JW_RA_PARSE_ERROR;
    }

    memset(info, 0, sizeof(*info));
    info->disk_count = 0;
    info->disk_slot = 0;
    info->savestate_supported = false;
    info->state_slot = 0;
    snprintf(info->raw, sizeof(info->raw), "%s", reply);
    jw_ra__trim_line(info->raw);
    snprintf(raw, sizeof(raw), "%s", info->raw);

    nread = sscanf(raw, "GET_INFO %d %d %31s",
                   &info->disk_count, &info->disk_slot, state_token);
    if (nread != 3 || info->disk_count < 0 || info->disk_slot < 0) {
        return JW_RA_PARSE_ERROR;
    }

    if (strcmp(state_token, "NO") == 0) {
        info->savestate_supported = false;
        info->state_slot = 0;
        return JW_RA_OK;
    }

    {
        char *end = NULL;
        long value;
        errno = 0;
        value = strtol(state_token, &end, 10);
        if (errno != 0 || end == state_token || (end && *end != '\0') ||
            value < -1 || value > 999999L) {
            return JW_RA_PARSE_ERROR;
        }
        info->savestate_supported = true;
        info->state_slot = (int)value;
    }

    return JW_RA_OK;
}

jw_ra_result jw_ra_get_status(const jw_ra_client *client, jw_ra_status *status) {
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result = jw_ra_request_raw(client, "GET_STATUS", reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra_parse_status_reply(reply, status);
}

jw_ra_result jw_ra_get_info(const jw_ra_client *client, jw_ra_info *info) {
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result = jw_ra_request_raw(client, "GET_INFO", reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_info(reply, info);
}

jw_ra_result jw_ra_pause(const jw_ra_client *client) {
    jw_ra_status status;
    jw_ra_result result = jw_ra_get_status(client, &status);
    if (result != JW_RA_OK) {
        return result;
    }
    if (status.state == JW_RA_STATE_PAUSED) {
        return JW_RA_OK;
    }
    if (status.state != JW_RA_STATE_PLAYING) {
        return JW_RA_UNSUPPORTED;
    }
    return jw_ra_send_raw(client, "PAUSE_TOGGLE");
}

jw_ra_result jw_ra_resume(const jw_ra_client *client) {
    jw_ra_status status;
    jw_ra_result result = jw_ra_get_status(client, &status);
    if (result != JW_RA_OK) {
        return result;
    }
    if (status.state == JW_RA_STATE_PLAYING) {
        return JW_RA_OK;
    }
    if (status.state != JW_RA_STATE_PAUSED) {
        return JW_RA_UNSUPPORTED;
    }
    return jw_ra_send_raw(client, "PAUSE_TOGGLE");
}

jw_ra_result jw_ra_pause_direct(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "PAUSE");
}

jw_ra_result jw_ra_resume_direct(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "UNPAUSE");
}

jw_ra_result jw_ra_menu_toggle(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "MENU_TOGGLE");
}

jw_ra_result jw_ra_open_menu(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "OPEN_MENU");
}

jw_ra_result jw_ra_open_shader_menu(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "OPEN_MENU SHADERS");
}

jw_ra_result jw_ra_quit(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "QUIT");
}

jw_ra_result jw_ra_reset(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "RESET");
}

jw_ra_result jw_ra_audio_reinit(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "AUDIO_REINIT");
}

jw_ra_result jw_ra_screenshot(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "SCREENSHOT");
}

jw_ra_result jw_ra_save_state(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "SAVE_STATE");
}

jw_ra_result jw_ra_load_state(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "LOAD_STATE");
}

jw_ra_result jw_ra_load_state_slot(const jw_ra_client *client, int slot,
                                   char *reply, size_t reply_size) {
    char command[64];

    if (slot < -1) {
        return JW_RA_UNSUPPORTED;
    }
    if (slot == -1) {
        jw_ra_result result = jw_ra_set_state_slot(client, -1);
        if (result != JW_RA_OK) {
            return result;
        }
        (void)reply;
        (void)reply_size;
        return jw_ra_load_state(client);
    }

    snprintf(command, sizeof(command), "LOAD_STATE_SLOT %d", slot);
    if (reply && reply_size > 0) {
        return jw_ra_request_raw(client, command, reply, reply_size);
    }
    return jw_ra_send_raw(client, command);
}

jw_ra_result jw_ra_set_state_slot(const jw_ra_client *client, int slot) {
    char command[64];
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result;

    if (slot < -1) {
        return JW_RA_UNSUPPORTED;
    }

    snprintf(command, sizeof(command), "SET_STATE_SLOT %d", slot);
    result = jw_ra_request_raw(client, command, reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_prefixed_int(reply, "SET_STATE_SLOT ", &slot);
}

jw_ra_result jw_ra_get_state_slot(const jw_ra_client *client, int *out_slot,
                                  bool *out_supported) {
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result;

    if (out_slot) {
        *out_slot = 0;
    }
    if (out_supported) {
        *out_supported = false;
    }

    result = jw_ra_request_raw(client, "GET_STATE_SLOT", reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }

    jw_ra__trim_line(reply);
    if (strcmp(reply, "GET_STATE_SLOT NO") == 0) {
        return JW_RA_OK;
    }

    result = jw_ra__parse_prefixed_int(reply, "GET_STATE_SLOT ", out_slot);
    if (result == JW_RA_OK && out_supported) {
        *out_supported = true;
    }
    return result;
}

jw_ra_result jw_ra_save_state_slot(const jw_ra_client *client, int slot,
                                   char *reply, size_t reply_size) {
    char command[64];

    if (slot < -1) {
        return JW_RA_UNSUPPORTED;
    }
    if (slot == -1) {
        /* The auto slot has no "SAVE_STATE_SLOT -1" form; select it, then save.
           Mirrors jw_ra_load_state_slot's handling of the auto slot. */
        jw_ra_result result = jw_ra_set_state_slot(client, -1);
        if (result != JW_RA_OK) {
            return result;
        }
        (void)reply;
        (void)reply_size;
        return jw_ra_save_state(client);
    }

    snprintf(command, sizeof(command), "SAVE_STATE_SLOT %d", slot);
    if (reply && reply_size > 0) {
        return jw_ra_request_raw(client, command, reply, reply_size);
    }
    return jw_ra_send_raw(client, command);
}

jw_ra_result jw_ra_state_slot_plus(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "STATE_SLOT_PLUS");
}

jw_ra_result jw_ra_state_slot_minus(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "STATE_SLOT_MINUS");
}

jw_ra_result jw_ra_get_disk_count(const jw_ra_client *client, int *out_count) {
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result = jw_ra_request_raw(client, "GET_DISK_COUNT", reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_prefixed_int(reply, "GET_DISK_COUNT ", out_count);
}

jw_ra_result jw_ra_get_disk_slot(const jw_ra_client *client, int *out_slot) {
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result = jw_ra_request_raw(client, "GET_DISK_SLOT", reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_prefixed_int(reply, "GET_DISK_SLOT ", out_slot);
}

jw_ra_result jw_ra_set_disk_slot(const jw_ra_client *client, int slot) {
    char command[64];
    char reply[JW_RA_REPLY_MAX];
    jw_ra_result result;

    if (slot < 0) {
        return JW_RA_UNSUPPORTED;
    }

    snprintf(command, sizeof(command), "SET_DISK_SLOT %d", slot);
    result = jw_ra_request_raw(client, command, reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }
    return jw_ra__parse_prefixed_int(reply, "SET_DISK_SLOT ", &slot);
}

jw_ra_result jw_ra_disk_eject_toggle(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "DISK_EJECT_TOGGLE");
}

jw_ra_result jw_ra_disk_next(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "DISK_NEXT");
}

jw_ra_result jw_ra_disk_prev(const jw_ra_client *client) {
    return jw_ra_send_raw(client, "DISK_PREV");
}

jw_ra_result jw_ra_get_path(const jw_ra_client *client, const char *kind,
                            char *out, size_t out_size) {
    char command[64];
    char reply[JW_RA_REPLY_MAX];
    char prefix[96];
    size_t prefix_len;
    jw_ra_result result;
    const char *value;

    if (!kind || !kind[0] || !out || out_size == 0) {
        return JW_RA_PARSE_ERROR;
    }

    out[0] = '\0';
    if (snprintf(command, sizeof(command), "GET_PATH %s", kind) >= (int)sizeof(command) ||
        snprintf(prefix, sizeof(prefix), "GET_PATH %s ", kind) >= (int)sizeof(prefix)) {
        return JW_RA_PARSE_ERROR;
    }

    result = jw_ra_request_raw(client, command, reply, sizeof(reply));
    if (result != JW_RA_OK) {
        return result;
    }

    jw_ra__trim_line(reply);
    prefix_len = strlen(prefix);
    if (strncmp(reply, prefix, prefix_len) != 0) {
        return JW_RA_PARSE_ERROR;
    }

    value = reply + prefix_len;
    snprintf(out, out_size, "%s", value);
    return out[0] ? JW_RA_OK : JW_RA_UNSUPPORTED;
}

jw_ra_result jw_ra_get_savestate_path(const jw_ra_client *client,
                                      char *out, size_t out_size) {
    return jw_ra_get_path(client, "savestate", out, out_size);
}

jw_ra_result jw_ra_show_message(const jw_ra_client *client, const char *message) {
    char command[JW_RA_REPLY_MAX];
    if (!message || !message[0]) {
        return JW_RA_PARSE_ERROR;
    }
    if (snprintf(command, sizeof(command), "SHOW_MSG %s", message) >= (int)sizeof(command)) {
        return JW_RA_PARSE_ERROR;
    }
    return jw_ra_send_raw(client, command);
}

jw_ra_result jw_ra_load_content_current_core(const jw_ra_client *client,
                                             const char *content_path,
                                             char *reply, size_t reply_size) {
    char command[PATH_MAX + 64];
    char local_reply[JW_RA_REPLY_MAX];
    const char *ok_prefix = "JAWAKA_LOAD_CONTENT OK";
    char *reply_buf = reply;
    size_t reply_buf_size = reply_size;
    jw_ra_result result;

    if (!content_path || !content_path[0]) {
        return JW_RA_PARSE_ERROR;
    }
    if (snprintf(command, sizeof(command), "JAWAKA_LOAD_CONTENT %s",
                 content_path) >= (int)sizeof(command)) {
        return JW_RA_PARSE_ERROR;
    }

    if (!reply_buf || reply_buf_size == 0) {
        reply_buf = local_reply;
        reply_buf_size = sizeof(local_reply);
    }

    result = jw_ra_request_raw(client, command, reply_buf, reply_buf_size);
    if (result != JW_RA_OK) {
        return result;
    }

    jw_ra__trim_line(reply_buf);
    if (strncmp(reply_buf, ok_prefix, strlen(ok_prefix)) == 0 &&
        (reply_buf[strlen(ok_prefix)] == '\0' ||
         isspace((unsigned char)reply_buf[strlen(ok_prefix)]))) {
        return JW_RA_OK;
    }
    return JW_RA_UNSUPPORTED;
}

/* ------------------------------------------------ power-hold synchronous save */

static int jw_ra__connected_udp(const jw_ra_client *client) {
    struct sockaddr_storage addr;
    socklen_t addr_len = 0;
    if (jw_ra__resolve(client, &addr, &addr_len) != JW_RA_OK) {
        return -1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }
    /* connect() makes the kernel drop datagrams from any other source. */
    if (connect(fd, (struct sockaddr *)&addr, addr_len) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

jw_ra_result jw_ra_parse_state_save_info_reply(const char *reply,
                                               jw_ra_state_save_info *info) {
    static const char prefix[] = "GET_STATE_SAVE_INFO ";
    char line[128];
    int version = 0;
    unsigned long long bytes = 0;
    int compressed = -1;
    char tail = 0;

    if (!reply || !info) {
        return JW_RA_PARSE_ERROR;
    }
    memset(info, 0, sizeof(*info));
    snprintf(line, sizeof(line), "%s", reply);
    jw_ra__trim_line(line);
    if (strncmp(line, prefix, sizeof(prefix) - 1u) != 0) {
        return JW_RA_PARSE_ERROR;
    }
    const char *p = line + sizeof(prefix) - 1u;
    if (sscanf(p, "%d", &version) != 1 || version != JW_RA_SYNC_SAVE_PROTOCOL) {
        return JW_RA_UNSUPPORTED;
    }
    if (strcmp(p, "1 NO") == 0) {
        return JW_RA_OK;
    }
    if (p[0] == '-' || strchr(p, '-') ||
        sscanf(p, "%d %llu %d %c", &version, &bytes, &compressed, &tail) != 3 ||
        bytes == 0 || (compressed != 0 && compressed != 1)) {
        return JW_RA_PARSE_ERROR;
    }
    info->supported = true;
    info->compressed = compressed == 1;
    info->bytes = bytes;
    return JW_RA_OK;
}

jw_ra_result jw_ra_get_state_save_info(const jw_ra_client *client,
                                       jw_ra_state_save_info *info) {
    static const char command[] = "GET_STATE_SAVE_INFO";
    if (!info) {
        return JW_RA_PARSE_ERROR;
    }
    memset(info, 0, sizeof(*info));
    int fd = jw_ra__connected_udp(client);
    if (fd < 0) {
        return JW_RA_SOCKET_ERROR;
    }
    if (send(fd, command, sizeof(command) - 1u, 0) != (ssize_t)(sizeof(command) - 1u)) {
        close(fd);
        return JW_RA_SOCKET_ERROR;
    }
    unsigned timeout_ms = client && client->timeout_ms ? client->timeout_ms
                                                       : JW_RA_DEFAULT_TIMEOUT_MS;
    long deadline = jw_ra__now_ms() + (long)timeout_ms;
    for (;;) {
        char reply[JW_RA_REPLY_MAX];
        long remaining = deadline - jw_ra__now_ms();
        if (remaining <= 0) {
            close(fd);
            return JW_RA_TIMEOUT;
        }
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        struct timeval tv = { (time_t)(remaining / 1000L),
                              (suseconds_t)((remaining % 1000L) * 1000L) };
        int ready = select(fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            close(fd);
            return ready == 0 ? JW_RA_TIMEOUT : JW_RA_SOCKET_ERROR;
        }
        ssize_t nread = recv(fd, reply, sizeof(reply) - 1u, 0);
        if (nread < 0) {
            close(fd);
            return JW_RA_SOCKET_ERROR;
        }
        reply[nread] = '\0';
        jw_ra_result result = jw_ra_parse_state_save_info_reply(reply, info);
        if (result == JW_RA_PARSE_ERROR &&
            strncmp(reply, "GET_STATE_SAVE_INFO", 19) != 0) {
            continue;  /* an unrelated late reply; same deadline */
        }
        close(fd);
        return result;
    }
}

/* "<command> <request id> " starts a reply to this request. Returns what
   follows it, or NULL for anyone else's reply, including the id-less
   "<command> - ERROR BAD_ARGS". */
static const char *jw_ra__sync_reply_body(const char *reply, const char *command,
                                          const char *request_id) {
    size_t command_len = strlen(command);
    if (strncmp(reply, command, command_len) != 0 || reply[command_len] != ' ') {
        return NULL;
    }
    const char *p = reply + command_len + 1u;
    size_t id_len = strlen(request_id);
    if (strncmp(p, request_id, id_len) != 0 || p[id_len] != ' ') {
        return NULL;
    }
    return p + id_len + 1u;
}

/* "ERROR <CODE>" with CODE of [A-Z_]; anything else after ERROR is malformed. */
static jw_ra_result jw_ra__sync_reply_error(const char *body, char *out, size_t out_size) {
    const char *code = body + 6;
    size_t n = strlen(code);
    while (n > 0 && (code[n - 1] == '\n' || code[n - 1] == '\r')) {
        n--;
    }
    if (n == 0 || n >= out_size) {
        return JW_RA_PARSE_ERROR;
    }
    for (size_t i = 0; i < n; i++) {
        if (!(isupper((unsigned char)code[i]) || code[i] == '_')) {
            return JW_RA_PARSE_ERROR;
        }
    }
    memcpy(out, code, n);
    out[n] = '\0';
    return JW_RA_OK;
}

jw_ra_result jw_ra_parse_sync_save_reply(const char *reply, const char *request_id,
                                         jw_ra_sync_save_reply *out) {
    if (!reply || !request_id || !request_id[0] || !out) {
        return JW_RA_PARSE_ERROR;
    }
    memset(out, 0, sizeof(*out));
    const char *p = jw_ra__sync_reply_body(reply, "SAVE_STATE_SYNC", request_id);
    if (!p) {
        return JW_RA_TIMEOUT;  /* another request, or the id-less BAD_ARGS */
    }

    if (strncmp(p, "ERROR ", 6) == 0) {
        return jw_ra__sync_reply_error(p, out->error, sizeof(out->error));
    }

    if (strncmp(p, "TMP_READY ", 10) != 0) {
        return JW_RA_PARSE_ERROR;
    }
    p += 10;
    char *end = NULL;
    if (!isdigit((unsigned char)*p)) {
        return JW_RA_PARSE_ERROR;
    }
    errno = 0;
    unsigned long long bytes = strtoull(p, &end, 10);
    if (errno != 0 || !end || *end != ' ' || bytes == 0) {
        return JW_RA_PARSE_ERROR;
    }
    const char *path = end + 1;
    size_t n = strlen(path);
    while (n > 0 && (path[n - 1] == '\n' || path[n - 1] == '\r')) {
        n--;
    }
    if (n == 0 || path[0] != '/' || n >= sizeof(out->tmp_path)) {
        return JW_RA_PARSE_ERROR;
    }
    memcpy(out->tmp_path, path, n);
    out->tmp_path[n] = '\0';
    out->bytes = bytes;
    out->ready = true;
    return JW_RA_OK;
}

void jw_ra_sync_save_init(jw_ra_sync_save *save) {
    if (save) {
        save->fd = -1;
        save->request_id[0] = '\0';
    }
}

jw_ra_result jw_ra_sync_save_send(const jw_ra_client *client, jw_ra_sync_save *save,
                                  int slot, unsigned long long max_bytes,
                                  long long start_by_ms) {
    if (!save || slot < 0 || max_bytes == 0 || start_by_ms <= 0) {
        return JW_RA_PARSE_ERROR;
    }
    jw_ra_sync_save_close(save);
    save->fd = jw_ra__connected_udp(client);
    if (save->fd < 0) {
        return JW_RA_SOCKET_ERROR;
    }
    jw_ra__make_request_id(save->request_id, sizeof(save->request_id));
    char command[160];
    int len = snprintf(command, sizeof(command), "SAVE_STATE_SYNC %s %d %llu %lld",
                       save->request_id, slot, max_bytes, start_by_ms);
    if (len <= 0 || (size_t)len >= sizeof(command) ||
        send(save->fd, command, (size_t)len, 0) != (ssize_t)len) {
        jw_ra_sync_save_close(save);
        return JW_RA_SOCKET_ERROR;
    }
    return JW_RA_OK;
}

jw_ra_result jw_ra_sync_save_poll(jw_ra_sync_save *save, jw_ra_sync_save_reply *reply) {
    if (!save || save->fd < 0 || !reply) {
        return JW_RA_SOCKET_ERROR;
    }
    /* Drain what is queued; never wait. */
    for (;;) {
        char buf[JW_RA_SYNC_SAVE_PATH_MAX + 128u];
        ssize_t nread = recv(save->fd, buf, sizeof(buf) - 1u, MSG_DONTWAIT);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return JW_RA_TIMEOUT;
            }
            /* ECONNREFUSED: RetroArch is gone. */
            return JW_RA_SOCKET_ERROR;
        }
        buf[nread] = '\0';
        jw_ra_result result = jw_ra_parse_sync_save_reply(buf, save->request_id, reply);
        if (result == JW_RA_TIMEOUT) {
            continue;
        }
        return result;
    }
}

void jw_ra_sync_save_close(jw_ra_sync_save *save) {
    if (save && save->fd >= 0) {
        close(save->fd);
        save->fd = -1;
    }
}

jw_ra_result jw_ra_parse_load_state_sync_reply(const char *reply, const char *request_id,
                                               jw_ra_sync_load_reply *out) {
    if (!reply || !request_id || !request_id[0] || !out) {
        return JW_RA_PARSE_ERROR;
    }
    memset(out, 0, sizeof(*out));
    const char *p = jw_ra__sync_reply_body(reply, "LOAD_STATE_SYNC", request_id);
    if (!p) {
        return JW_RA_TIMEOUT;
    }
    if (strncmp(p, "ERROR ", 6) == 0) {
        return jw_ra__sync_reply_error(p, out->error, sizeof(out->error));
    }
    if (strncmp(p, "OK ", 3) != 0 || !isdigit((unsigned char)p[3])) {
        return JW_RA_PARSE_ERROR;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long bytes = strtoull(p + 3, &end, 10);
    while (end && (*end == '\n' || *end == '\r')) {
        end++;
    }
    if (errno != 0 || !end || *end != '\0' || bytes == 0) {
        return JW_RA_PARSE_ERROR;
    }
    out->bytes = bytes;
    out->loaded = true;
    return JW_RA_OK;
}

static bool jw_ra__sync_request_id_ok(const char *request_id) {
    size_t n = request_id ? strlen(request_id) : 0;
    if (n == 0 || n >= JW_RA_REQUEST_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)request_id[i];
        if (!isalnum(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

jw_ra_result jw_ra_load_state_sync(const jw_ra_client *client, jw_ra_sync_save *exchange,
                                   const char *request_id, int slot) {
    if (!exchange || slot < 0 || slot > 999 || !jw_ra__sync_request_id_ok(request_id)) {
        return JW_RA_PARSE_ERROR;
    }
    jw_ra_sync_save_close(exchange);
    exchange->fd = jw_ra__connected_udp(client);
    if (exchange->fd < 0) {
        return JW_RA_SOCKET_ERROR;
    }
    snprintf(exchange->request_id, sizeof(exchange->request_id), "%s", request_id);
    char command[64];
    int len = snprintf(command, sizeof(command), "LOAD_STATE_SYNC %s %d",
                       exchange->request_id, slot);
    if (len <= 0 || (size_t)len >= sizeof(command) ||
        send(exchange->fd, command, (size_t)len, 0) != (ssize_t)len) {
        jw_ra_sync_save_close(exchange);
        return JW_RA_SOCKET_ERROR;
    }
    return JW_RA_OK;
}

jw_ra_result jw_ra_load_state_sync_poll(jw_ra_sync_save *exchange,
                                        jw_ra_sync_load_reply *reply) {
    if (!exchange || exchange->fd < 0 || !reply) {
        return JW_RA_SOCKET_ERROR;
    }
    for (;;) {
        char buf[JW_RA_REPLY_MAX];
        ssize_t nread = recv(exchange->fd, buf, sizeof(buf) - 1u, MSG_DONTWAIT);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return JW_RA_TIMEOUT;
            }
            return JW_RA_SOCKET_ERROR;  /* ECONNREFUSED: RetroArch is gone */
        }
        buf[nread] = '\0';
        jw_ra_result result =
            jw_ra_parse_load_state_sync_reply(buf, exchange->request_id, reply);
        if (result == JW_RA_TIMEOUT) {
            continue;
        }
        return result;
    }
}

jw_ra_result jw_ra_load_state_sync_probe(const jw_ra_client *client,
                                         const char *request_id) {
    if (!jw_ra__sync_request_id_ok(request_id)) {
        return JW_RA_PARSE_ERROR;
    }
    jw_ra_sync_save exchange;
    jw_ra_sync_save_init(&exchange);
    exchange.fd = jw_ra__connected_udp(client);
    if (exchange.fd < 0) {
        return JW_RA_SOCKET_ERROR;
    }
    snprintf(exchange.request_id, sizeof(exchange.request_id), "%s", request_id);
    char command[64];
    int len = snprintf(command, sizeof(command), "LOAD_STATE_SYNC %s -1", request_id);
    if (len <= 0 || (size_t)len >= sizeof(command) ||
        send(exchange.fd, command, (size_t)len, 0) != (ssize_t)len) {
        jw_ra_sync_save_close(&exchange);
        return JW_RA_SOCKET_ERROR;
    }
    unsigned timeout_ms = client && client->timeout_ms ? client->timeout_ms
                                                       : JW_RA_DEFAULT_TIMEOUT_MS;
    long deadline = jw_ra__now_ms() + (long)timeout_ms;
    jw_ra_result result = JW_RA_TIMEOUT;
    for (;;) {
        long remaining = deadline - jw_ra__now_ms();
        if (remaining <= 0) {
            break;
        }
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(exchange.fd, &read_fds);
        struct timeval tv = { (time_t)(remaining / 1000L),
                              (suseconds_t)((remaining % 1000L) * 1000L) };
        int ready = select(exchange.fd + 1, &read_fds, NULL, NULL, &tv);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            result = ready == 0 ? JW_RA_TIMEOUT : JW_RA_SOCKET_ERROR;
            break;
        }
        jw_ra_sync_load_reply reply;
        result = jw_ra_load_state_sync_poll(&exchange, &reply);
        if (result == JW_RA_TIMEOUT) {
            continue;  /* someone else's reply; same deadline */
        }
        /* Any answer for this id, even the expected BAD_ARGS, proves the
           command is there; a load OK to slot -1 never happens. */
        if (result == JW_RA_OK && reply.loaded) {
            result = JW_RA_PARSE_ERROR;
        }
        break;
    }
    jw_ra_sync_save_close(&exchange);
    return result;
}
