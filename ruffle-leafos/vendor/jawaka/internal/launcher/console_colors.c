#include "console_colors.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "internal/core/log.h"

void jw_console_colors_clear(jw_console_color_table *t) {
    if (!t) return;
    t->count = 0;
    memset(t->entries, 0, sizeof(t->entries));
}

/* Parse "#RRGGBB" or "RRGGBB" into a packed 0xRRGGBB integer. Returns 0 on
 * failure. */
static uint32_t jw__parse_hex_rgb(const char *s) {
    if (!s) return 0;
    if (s[0] == '#') s++;
    if (strlen(s) < 6) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 6; i++) {
        char c = s[i];
        uint32_t d;
        if      (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    /* Reject 0x000000 (ambiguous with parse failure). If a theme really wants
     * pure black it can use #010101 — visually identical. */
    return v ? v : 0;
}

static char *jw__read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long len = ftell(fp);
    if (len < 0 || len > (1 << 20)) { fclose(fp); return NULL; }
    rewind(fp);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[n] = '\0';
    return buf;
}

int jw_console_colors_load(jw_console_color_table *out,
                           const char *theme_dir,
                           const char *theme_name) {
    if (!out) return -1;
    jw_console_colors_clear(out);
    if (!theme_dir || !theme_dir[0] || !theme_name || !theme_name[0]) {
        return 0;  /* nothing to load is not an error */
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/stylesheet.json", theme_dir, theme_name);
    char *json = jw__read_file(path);
    if (!json) return -1;

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        jw_log_warn("console_colors: failed to parse %s", path);
        return -1;
    }

    cJSON *launcher = cJSON_GetObjectItem(root, "launcher");
    cJSON *map = launcher ? cJSON_GetObjectItem(launcher, "console_colors") : NULL;
    if (!map || !cJSON_IsObject(map)) {
        cJSON_Delete(root);
        return 0;  /* no map defined; not an error */
    }

    cJSON *child = NULL;
    cJSON_ArrayForEach(child, map) {
        if (!child->string || !cJSON_IsString(child)) continue;
        if (out->count >= JW_CONSOLE_COLOR_MAX) {
            jw_log_warn("console_colors: table full (%d entries), ignoring rest",
                        JW_CONSOLE_COLOR_MAX);
            break;
        }
        uint32_t rgb = jw__parse_hex_rgb(child->valuestring);
        if (!rgb) {
            jw_log_warn("console_colors: invalid color for %s: %s",
                        child->string, child->valuestring);
            continue;
        }
        jw_console_color *e = &out->entries[out->count++];
        strncpy(e->code, child->string, sizeof(e->code) - 1);
        e->code[sizeof(e->code) - 1] = '\0';
        e->rgb = rgb;
    }

    cJSON_Delete(root);
    return 0;
}

bool jw_console_colors_lookup(const jw_console_color_table *t,
                              const char *code,
                              SDL_Color *out_color) {
    if (!t || !code || !out_color) return false;
    for (int i = 0; i < t->count; i++) {
        if (strcmp(t->entries[i].code, code) == 0) {
            uint32_t v = t->entries[i].rgb;
            out_color->r = (uint8_t)((v >> 16) & 0xFF);
            out_color->g = (uint8_t)((v >>  8) & 0xFF);
            out_color->b = (uint8_t)( v        & 0xFF);
            out_color->a = 255;
            return true;
        }
    }
    return false;
}
