#include "internal/store/managed_apps.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *jw__read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || size > 1024 * 1024) {
        fclose(fp);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }

    char *data = (char *)malloc((size_t)size + 1u);
    if (!data) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(data, 1, (size_t)size, fp);
    int failed = ferror(fp);
    fclose(fp);
    if (failed || got != (size_t)size) {
        free(data);
        return NULL;
    }
    data[got] = '\0';
    return data;
}

static int jw__managed_path_safe(const char *path) {
    if (!path || !path[0] || path[0] == '/' || strstr(path, "\\") ||
        strstr(path, "//") || strstr(path, "../") || strstr(path, "/..")) {
        return 0;
    }
    const char *slash = strchr(path, '/');
    if (!slash || slash == path || slash[1] == '\0') {
        return 0;
    }
    if (strncmp(path, "Apps/", 5) == 0) {
        return 0;
    }
    return 1;
}

static const char *jw__apps_namespace_path(const char *path) {
    if (!path) {
        return "";
    }
    if (strncmp(path, "Apps/", 5) == 0) {
        return path + 5;
    }
    return path;
}

int jw_pakrat_load_managed_apps(const char *platform_root,
                                jw_pakrat_managed_apps *out) {
    if (!platform_root || !platform_root[0] || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char manifest_path[1024];
    int n = snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json",
                     platform_root);
    if (n < 0 || (size_t)n >= sizeof(manifest_path)) {
        return -1;
    }

    char *text = jw__read_file(manifest_path);
    if (!text) {
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        return -1;
    }

    const cJSON *managed_apps = cJSON_GetObjectItemCaseSensitive(root, "managed_apps");
    if (!cJSON_IsArray(managed_apps)) {
        cJSON_Delete(root);
        return -1;
    }

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, managed_apps) {
        if (out->count >= JW_PAKRAT_MANAGED_APPS_MAX) {
            cJSON_Delete(root);
            return -1;
        }
        if (!cJSON_IsString(item) || !item->valuestring ||
            !jw__managed_path_safe(item->valuestring)) {
            cJSON_Delete(root);
            return -1;
        }
        snprintf(out->paths[out->count], sizeof(out->paths[out->count]), "%s",
                 item->valuestring);
        out->count++;
    }

    cJSON_Delete(root);
    return 0;
}

int jw_pakrat_managed_app_path_blocked(const jw_pakrat_managed_apps *managed,
                                       const char *install_path) {
    if (!managed || !install_path || !install_path[0]) {
        return 0;
    }
    const char *path = jw__apps_namespace_path(install_path);
    for (int i = 0; i < managed->count; i++) {
        if (strcmp(path, managed->paths[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

int jw_pakrat_managed_app_path_blocked_from_platform(const char *platform_root,
                                                     const char *install_path) {
    jw_pakrat_managed_apps managed;
    if (jw_pakrat_load_managed_apps(platform_root, &managed) != 0) {
        return -1;
    }
    return jw_pakrat_managed_app_path_blocked(&managed, install_path);
}
