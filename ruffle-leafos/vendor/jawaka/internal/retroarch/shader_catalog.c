#include "internal/retroarch/shader_catalog.h"

#include "cJSON.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void jw_sc__diag(jw_shader_catalog *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->diagnostic, sizeof(out->diagnostic), fmt, ap);
    va_end(ap);
}

bool jw_shader_catalog_path_ok(const char *path) {
    static const char prefix[] = "leaf-recommended/";
    size_t len;

    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || strchr(path, '\\')) {
        return false;
    }
    if (strncmp(path, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    /* Any ".." component, not just a leading one. */
    for (const char *p = path; (p = strstr(p, "..")); p += 2) {
        bool at_start = (p == path) || p[-1] == '/';
        bool at_end = (p[2] == '\0') || p[2] == '/';
        if (at_start && at_end) {
            return false;
        }
    }
    len = strlen(path);
    if (len <= 6 || strcmp(path + len - 6, ".glslp") != 0) {
        return false;
    }
    /* Something has to remain below the prefix. */
    return len > sizeof(prefix) - 1 + 6;
}

bool jw_shader_catalog_reference_target(const char *preset_path,
                                        char *out, size_t out_size) {
    char line[4096];
    char candidate[PATH_MAX];
    char resolved[PATH_MAX];
    FILE *fp;

    if (!preset_path || !preset_path[0] || !out || out_size == 0) return false;
    out[0] = '\0';
    fp = fopen(preset_path, "r");
    if (!fp) return false;

    bool found = false;
    size_t inspected = 0;
    while (inspected < 8192u && fgets(line, sizeof(line), fp)) {
        inspected += strlen(line);
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (strncmp(p, "#reference", 10) != 0 ||
            (!isspace((unsigned char)p[10]) && p[10] != '"' && p[10] != '\''))
            continue;
        p += 10;
        while (isspace((unsigned char)*p)) p++;
        char quote = 0;
        if (*p == '"' || *p == '\'') quote = *p++;
        char *end = p;
        while (*end && (quote ? *end != quote
                               : !isspace((unsigned char)*end)))
            end++;
        if (end == p) break;
        *end = '\0';

        if (p[0] == '/') {
            if (snprintf(candidate, sizeof(candidate), "%s", p) >=
                (int)sizeof(candidate))
                break;
        } else {
            const char *slash = strrchr(preset_path, '/');
            size_t dir_len = slash ? (size_t)(slash - preset_path) : 1u;
            const char *dir = slash ? preset_path : ".";
            if (dir_len + 1u + strlen(p) + 1u > sizeof(candidate)) break;
            memcpy(candidate, dir, dir_len);
            candidate[dir_len] = '/';
            strcpy(candidate + dir_len + 1u, p);
        }
        found = realpath(candidate, resolved) != NULL;
        break;
    }
    fclose(fp);
    if (!found) return false;
    size_t len = strlen(resolved);
    if (len < 6u || strcmp(resolved + len - 6u, ".glslp") != 0 ||
        len >= out_size)
        return false;
    memcpy(out, resolved, len + 1u);
    return true;
}

static char *jw_sc__dup(const char *s) {
    size_t n;
    char *out;
    if (!s) {
        return NULL;
    }
    n = strlen(s);
    out = malloc(n + 1);
    if (out) {
        memcpy(out, s, n + 1);
    }
    return out;
}

static char *jw_sc__read_file(const char *path, jw_shader_catalog *out) {
    struct stat st;
    FILE *fp;
    char *buf;
    size_t got;

    if (stat(path, &st) != 0) {
        jw_sc__diag(out, "shader manifest is missing");
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        jw_sc__diag(out, "shader manifest is not a regular file");
        return NULL;
    }
    /* Capped before the read, so an implausible manifest costs a stat rather
     * than the memory it claims to need. */
    if ((unsigned long long)st.st_size > JW_SHADER_CATALOG_MAX_BYTES) {
        jw_sc__diag(out, "shader manifest is too large");
        return NULL;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        jw_sc__diag(out, "shader manifest could not be opened");
        return NULL;
    }
    buf = malloc((size_t)st.st_size + 1u);
    if (!buf) {
        fclose(fp);
        jw_sc__diag(out, "out of memory reading the shader manifest");
        return NULL;
    }
    got = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

static bool jw_sc__system_matches(const cJSON *systems, const char *system_id) {
    const cJSON *item;
    if (!cJSON_IsArray(systems) || !system_id || !system_id[0]) {
        return false;
    }
    cJSON_ArrayForEach(item, systems) {
        if (cJSON_IsString(item) && item->valuestring &&
            strcmp(item->valuestring, system_id) == 0) {
            return true;
        }
    }
    return false;
}

static int jw_sc__compare_rows(const void *a, const void *b) {
    const jw_shader_catalog_row *ra = a;
    const jw_shader_catalog_row *rb = b;
    int cmp = strcmp(ra->display_name ? ra->display_name : "",
                     rb->display_name ? rb->display_name : "");
    if (cmp != 0) {
        return cmp;
    }
    /* Display names are authored and could collide; path is unique, so this
     * keeps the order total and therefore stable across runs. */
    return strcmp(ra->path ? ra->path : "", rb->path ? rb->path : "");
}

void jw_shader_catalog_free(jw_shader_catalog *catalog) {
    if (!catalog) {
        return;
    }
    for (size_t i = 0; i < catalog->count; i++) {
        jw_shader_catalog_row *row = &catalog->rows[i];
        free(row->path);
        free(row->display_name);
        free(row->description);
        for (size_t c = 0; c < row->constraint_count; c++) {
            free(row->constraints[c]);
        }
    }
    free(catalog->rows);
    catalog->rows = NULL;
    catalog->count = 0;
}

bool jw_shader_catalog_load(const char *manifest_path, const char *system_id,
                            jw_shader_catalog *out) {
    char *text;
    cJSON *root;
    const cJSON *schema;
    const cJSON *presets;
    const cJSON *entry;
    size_t capacity = 0;
    bool ok = false;

    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!manifest_path || !manifest_path[0]) {
        jw_sc__diag(out, "no shader manifest path");
        return false;
    }

    text = jw_sc__read_file(manifest_path, out);
    if (!text) {
        return false;
    }

    root = cJSON_Parse(text);
    free(text);
    if (!root) {
        jw_sc__diag(out, "shader manifest is not valid JSON");
        return false;
    }

    schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(schema) || schema->valueint != JW_SHADER_CATALOG_SCHEMA) {
        /* A newer schema may redefine a field this code reads, so refuse it
         * rather than interpret it optimistically. */
        jw_sc__diag(out, "unsupported shader manifest schema");
        goto done;
    }

    presets = cJSON_GetObjectItemCaseSensitive(root, "presets");
    if (!cJSON_IsArray(presets)) {
        jw_sc__diag(out, "shader manifest has no presets");
        goto done;
    }

    cJSON_ArrayForEach(entry, presets) {
        const cJSON *group, *qual, *path, *name, *desc, *systems, *constraints;
        jw_shader_catalog_row row;
        bool duplicate = false;

        if (out->count >= JW_SHADER_CATALOG_MAX_ROWS) {
            jw_sc__diag(out, "shader manifest has too many recommendations");
            break;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        group = cJSON_GetObjectItemCaseSensitive(entry, "group");
        qual = cJSON_GetObjectItemCaseSensitive(entry, "qualification");
        if (!cJSON_IsString(group) || !cJSON_IsString(qual) ||
            strcmp(group->valuestring, "leaf-recommended") != 0 ||
            strcmp(qual->valuestring, "recommended") != 0) {
            continue;
        }
        systems = cJSON_GetObjectItemCaseSensitive(entry, "intended_systems");
        if (!jw_sc__system_matches(systems, system_id)) {
            continue;
        }
        path = cJSON_GetObjectItemCaseSensitive(entry, "path");
        name = cJSON_GetObjectItemCaseSensitive(entry, "display_name");
        if (!cJSON_IsString(path) || !cJSON_IsString(name) ||
            !name->valuestring[0] ||
            !jw_shader_catalog_path_ok(path->valuestring)) {
            continue;
        }
        for (size_t i = 0; i < out->count; i++) {
            if (strcmp(out->rows[i].path, path->valuestring) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        memset(&row, 0, sizeof(row));
        row.path = jw_sc__dup(path->valuestring);
        row.display_name = jw_sc__dup(name->valuestring);
        desc = cJSON_GetObjectItemCaseSensitive(entry, "description");
        if (cJSON_IsString(desc)) {
            row.description = jw_sc__dup(desc->valuestring);
        }
        constraints = cJSON_GetObjectItemCaseSensitive(entry, "constraints");
        if (cJSON_IsArray(constraints)) {
            const cJSON *c;
            cJSON_ArrayForEach(c, constraints) {
                if (row.constraint_count >= JW_SHADER_CATALOG_MAX_CONSTRAINTS) {
                    break;
                }
                if (cJSON_IsString(c) && c->valuestring[0]) {
                    row.constraints[row.constraint_count++] =
                        jw_sc__dup(c->valuestring);
                }
            }
        }
        if (!row.path || !row.display_name) {
            free(row.path);
            free(row.display_name);
            free(row.description);
            for (size_t c = 0; c < row.constraint_count; c++) {
                free(row.constraints[c]);
            }
            continue;
        }

        if (out->count == capacity) {
            size_t next = capacity ? capacity * 2u : 8u;
            jw_shader_catalog_row *grown =
                realloc(out->rows, next * sizeof(*grown));
            if (!grown) {
                jw_sc__diag(out, "out of memory building the shader catalog");
                goto done;
            }
            out->rows = grown;
            capacity = next;
        }
        out->rows[out->count++] = row;
    }

    /* Manifest order is generated by path, which puts crt- before lcd- for
     * reasons that mean nothing to a user. Sort by display name so the list
     * reads sensibly and is stable run to run. */
    if (out->count > 1) {
        qsort(out->rows, out->count, sizeof(*out->rows), jw_sc__compare_rows);
    }
    if (out->count == 0 && !out->diagnostic[0]) {
        jw_sc__diag(out, "no recommendations for this system");
    }
    ok = true;

done:
    cJSON_Delete(root);
    if (!ok) {
        jw_shader_catalog_free(out);
    }
    return ok;
}
