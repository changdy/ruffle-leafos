#include "internal/platform/leaf_version.h"

#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JW_RELEASE_JSON_MAX_BYTES (64u * 1024u)
#define JW_VERSION_COMPONENT_MAX 9999

static int jw__parse_component(const char **cursor, int *out) {
    const unsigned char *p = (const unsigned char *)*cursor;
    unsigned int value = 0;
    int digits = 0;

    while (*p >= '0' && *p <= '9') {
        value = value * 10u + (unsigned int)(*p - '0');
        if (value > JW_VERSION_COMPONENT_MAX) {
            return -1;
        }
        digits++;
        p++;
    }
    if (digits == 0) {
        return -1;
    }
    *cursor = (const char *)p;
    *out = (int)value;
    return 0;
}

static int jw__version_parse(const char *value, int out[3],
                             int allow_v_prefix, int allow_suffix) {
    if (!value || !value[0] || !out) {
        return -1;
    }

    const char *cursor = value;
    int parsed[3] = {0, 0, 0};
    if (allow_v_prefix && (*cursor == 'v' || *cursor == 'V')) {
        cursor++;
    }
    if (jw__parse_component(&cursor, &parsed[0]) != 0 || *cursor++ != '.' ||
        jw__parse_component(&cursor, &parsed[1]) != 0 || *cursor++ != '.' ||
        jw__parse_component(&cursor, &parsed[2]) != 0) {
        return -1;
    }

    if (*cursor != '\0') {
        if (!allow_suffix || *cursor == '.' ||
            (*cursor >= '0' && *cursor <= '9')) {
            return -1;
        }
    }

    memcpy(out, parsed, sizeof(parsed));
    return 0;
}

int jw_leaf_release_version_parse(const char *value, int out[3]) {
    return jw__version_parse(value, out, 1, 1);
}

int jw_pak_version_parse(const char *value, int out[3]) {
    return jw__version_parse(value, out, 0, 0);
}

int jw_version_cmp(const int left[3], const int right[3]) {
    if (!left || !right) {
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        if (left[i] < right[i]) {
            return -1;
        }
        if (left[i] > right[i]) {
            return 1;
        }
    }
    return 0;
}

static int jw__copy_json_string(char *out, size_t out_size,
                                const cJSON *root, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return 0;
    }
    int needed = snprintf(out, out_size, "%s", item->valuestring);
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}

int jw_installed_release_read(const char *state_dir,
                              jw_installed_release *out) {
    if (!state_dir || !state_dir[0] || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char path[PATH_MAX];
    int needed = snprintf(path, sizeof(path), "%s/release.json", state_dir);
    if (needed < 0 || (size_t)needed >= sizeof(path)) {
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return errno == ENOENT ? 1 : -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long length = ftell(fp);
    if (length < 0 || (unsigned long)length > JW_RELEASE_JSON_MAX_BYTES ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    char *text = (char *)malloc((size_t)length + 1u);
    if (!text) {
        fclose(fp);
        return -1;
    }
    size_t got = fread(text, 1, (size_t)length, fp);
    int read_error = ferror(fp);
    fclose(fp);
    if (read_error || got != (size_t)length) {
        free(text);
        return -1;
    }
    text[got] = '\0';

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (cJSON_IsNumber(schema)) {
        out->schema = schema->valueint;
    }
    if (jw__copy_json_string(out->version, sizeof(out->version),
                             root, "version") != 0 ||
        jw__copy_json_string(out->release_id, sizeof(out->release_id),
                             root, "release_id") != 0) {
        cJSON_Delete(root);
        memset(out, 0, sizeof(*out));
        return -1;
    }

    cJSON_Delete(root);
    return 0;
}
