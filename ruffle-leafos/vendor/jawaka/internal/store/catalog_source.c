#include "internal/store/catalog_source.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define JW_PAKRAT_PRODUCTION_BASE_URL "https://leaf.game/pakrat/v1/"

static int jw__copy_trimmed_url(const char *value, char *out, size_t out_size) {
    if (!value || !out || out_size == 0) {
        return -1;
    }

    const char *start = value;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }

    size_t len = (size_t)(end - start);
    if (len == 0) {
        out[0] = '\0';
        return 1;
    }
    int needs_slash = start[len - 1] != '/';
    if (len + (needs_slash ? 1u : 0u) + 1u > out_size) {
        return -1;
    }

    memcpy(out, start, len);
    if (needs_slash) {
        out[len++] = '/';
    }
    out[len] = '\0';
    return 0;
}

static int jw__read_dev_catalog_file(const char *internal_data_path,
                                     char *out, size_t out_size) {
    if (!internal_data_path || !internal_data_path[0] || !out || out_size == 0) {
        return 1;
    }

    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", internal_data_path,
                     JW_PAKRAT_DEV_CATALOG_FILE);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return 1;
    }

    char buffer[1024];
    size_t used = fread(buffer, 1, sizeof(buffer) - 1, fp);
    int failed = ferror(fp);
    fclose(fp);
    if (failed) {
        return -1;
    }
    buffer[used] = '\0';
    return jw__copy_trimmed_url(buffer, out, out_size);
}

int jw_pakrat_catalog_url_allowed(const char *url, int is_dev_override) {
    if (!url || !url[0]) {
        return 1;
    }
    if (strncasecmp(url, "https://", 8) == 0) {
        return 1;
    }
    if (strncasecmp(url, "http://", 7) == 0) {
        return is_dev_override ? 1 : 0;
    }
    return 0;
}

int jw_pakrat_catalog_base_url(const char *internal_data_path,
                               char *out, size_t out_size,
                               int *out_is_dev_override) {
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    if (out_is_dev_override) {
        *out_is_dev_override = 0;
    }

    const char *env = getenv("PAKRAT_CATALOG_BASE_URL");
    if (env && env[0]) {
        int rc = jw__copy_trimmed_url(env, out, out_size);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0) {
            if (out_is_dev_override) {
                *out_is_dev_override = 1;
            }
            return jw_pakrat_catalog_url_allowed(out, 1) ? 0 : -1;
        }
    }

    const char *state_dir = internal_data_path;
    if (!state_dir || !state_dir[0]) {
        state_dir = getenv("UMRK_INTERNAL_DATA_PATH");
    }
    int rc = jw__read_dev_catalog_file(state_dir, out, out_size);
    if (rc < 0) {
        return -1;
    }
    if (rc == 0) {
        if (out_is_dev_override) {
            *out_is_dev_override = 1;
        }
        return jw_pakrat_catalog_url_allowed(out, 1) ? 0 : -1;
    }

    return jw__copy_trimmed_url(JW_PAKRAT_PRODUCTION_BASE_URL, out, out_size);
}
