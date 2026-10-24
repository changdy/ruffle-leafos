/* Hand-made theme limits the launcher enforces itself: names cut on a UTF-8
 * boundary, _default refused in any case, and the header read that caps a
 * wallpaper before it is ever decoded. */
#include "internal/launcher/user_themes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                            \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

static void write_bytes(const char *path, const unsigned char *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp || fwrite(data, 1, len, fp) != len || fclose(fp) != 0) {
        fprintf(stderr, "write failed: %s\n", path);
        exit(2);
    }
}

static void be32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/* Signature + IHDR is all a header read looks at. */
static void write_png_header(const char *path, unsigned w, unsigned h) {
    unsigned char png[33] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n',
                              0, 0, 0, 13, 'I', 'H', 'D', 'R' };
    be32(png + 16, w);
    be32(png + 20, h);
    png[24] = 8;
    write_bytes(path, png, sizeof(png));
}

/* SOI, one APP0 segment to walk past, then a baseline frame header. */
static void write_jpeg_header(const char *path, unsigned w, unsigned h) {
    unsigned char jpg[] = {
        0xFF, 0xD8,
        0xFF, 0xE0, 0x00, 0x06, 'J', 'F', 'I', 'F',
        0xFF, 0xC0, 0x00, 0x0B, 0x08, 0, 0, 0, 0, 0x01, 0x01, 0x11, 0x00,
        0xFF, 0xD9,
    };
    jpg[15] = (unsigned char)(h >> 8);
    jpg[16] = (unsigned char)h;
    jpg[17] = (unsigned char)(w >> 8);
    jpg[18] = (unsigned char)w;
    write_bytes(path, jpg, sizeof(jpg));
}

int main(void) {
    char root[] = "/tmp/jw-user-themes.XXXXXX";
    if (!mkdtemp(root)) return 2;
    char path[1024];

    /* 31 three-byte characters plus "ab" fill 95 bytes, which fits the name
       buffer; one more character does not, and the cut must not split it. */
    snprintf(path, sizeof(path), "%s/Themes", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/Themes/long", root);
    mkdir(path, 0755);
    char json[512];
    size_t used = (size_t)snprintf(json, sizeof(json), "{\"name\":\"");
    for (int i = 0; i < 31; i++) used += (size_t)snprintf(json + used, sizeof(json) - used, "\xE3\x81\x82");
    snprintf(json + used, sizeof(json) - used, "a\xE3\x81\x82\"}");
    snprintf(path, sizeof(path), "%s/Themes/long/theme.json", root);
    write_bytes(path, (const unsigned char *)json, strlen(json));

    jw_user_theme_catalog *cat = calloc(1, sizeof(*cat));
    CHECK(cat && jw_user_themes_scan(cat, root) == 1);
    const char *name = cat->items[0].name;
    CHECK(strlen(name) == 94);   /* 31 x 3 + "a"; the last character dropped whole */
    CHECK((unsigned char)name[strlen(name) - 1] == 'a');

    char asset[1024];
    CHECK(jw_user_theme_icon_path(cat, 0, "grid", "FC", asset, sizeof(asset)));
    CHECK(!jw_user_theme_icon_path(cat, 0, "grid", "_default", asset, sizeof(asset)));
    CHECK(!jw_user_theme_label_path(cat, 0, "grid", "_DEFAULT", asset, sizeof(asset)));
    CHECK(!jw_user_theme_wordmark_path(cat, 0, "grid", "_Default", asset, sizeof(asset)));

    /* icons/ at the root: one set both views read, found by the rescan and
       counted by validation alongside grid/icons, which Grid also draws. */
    CHECK(!cat->items[0].has_shared_icons);
    CHECK(jw_user_theme_shared_icon_path(cat, 0, "FC", asset, sizeof(asset)));
    snprintf(path, sizeof(path), "%s/Themes/long/icons/FC.png", root);
    CHECK(strcmp(asset, path) == 0);
    CHECK(jw_user_theme_shared_icon_path(cat, 0, "_apps", asset, sizeof(asset)));
    CHECK(!jw_user_theme_shared_icon_path(cat, 0, "_DEFAULT", asset, sizeof(asset)));
    CHECK(!jw_user_theme_shared_icon_path(cat, 0, "", asset, sizeof(asset)));
    CHECK(!jw_user_theme_shared_icon_path(cat, 1, "FC", asset, sizeof(asset)));
    snprintf(path, sizeof(path), "%s/Themes/long/icons", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/Themes/long/grid", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/Themes/long/grid/icons", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/Themes/long/icons/FC.png", root);
    write_png_header(path, 512, 512);
    snprintf(path, sizeof(path), "%s/Themes/long/icons/MD.png", root);
    write_png_header(path, 256, 256);            /* off-size: flagged */
    snprintf(path, sizeof(path), "%s/Themes/long/icons/GBA.png", root);
    write_png_header(path, 2048, 2048);          /* over the cap: rejected */
    snprintf(path, sizeof(path), "%s/Themes/long/grid/icons/FC.png", root);
    write_png_header(path, 512, 512);
    CHECK(jw_user_themes_scan(cat, root) == 1);
    CHECK(cat->items[0].has_shared_icons && cat->items[0].has_grid_icons);
    CHECK(!cat->items[0].has_coverflow_icons);
    int present = 0, flagged = 0;
    CHECK(jw_user_theme_validate(cat, 0, &present, &flagged) == 1);
    CHECK(present == 4 && flagged == 1);

    /* Wallpapers: PNG or JPEG by their bytes, at most 2048 px per edge. */
    snprintf(path, sizeof(path), "%s/ok.png", root);
    write_png_header(path, 2048, 2048);
    CHECK(jw_user_theme_wallpaper_ok(path));
    snprintf(path, sizeof(path), "%s/wide.png", root);
    write_png_header(path, 2049, 720);
    CHECK(!jw_user_theme_wallpaper_ok(path));
    snprintf(path, sizeof(path), "%s/ok.jpg", root);
    write_jpeg_header(path, 960, 2048);
    int w = 0, h = 0;
    CHECK(jw_user_theme_jpeg_dims(path, &w, &h) && w == 960 && h == 2048);
    CHECK(jw_user_theme_wallpaper_ok(path));
    snprintf(path, sizeof(path), "%s/tall.jpeg", root);
    write_jpeg_header(path, 960, 2049);
    CHECK(!jw_user_theme_wallpaper_ok(path));
    /* A JPEG named .png is still a JPEG; the decoder sniffs bytes too. */
    snprintf(path, sizeof(path), "%s/named-wrong.png", root);
    write_jpeg_header(path, 1280, 720);
    CHECK(jw_user_theme_wallpaper_ok(path));
    snprintf(path, sizeof(path), "%s/not-an-image.png", root);
    write_bytes(path, (const unsigned char *)"GIF89a", 6);
    CHECK(!jw_user_theme_wallpaper_ok(path));

    free(cat);
    char cmd[1100];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) fprintf(stderr, "cleanup failed: %s\n", root);
    if (g_failures) {
        fprintf(stderr, "FAIL user-themes-test (%d failures)\n", g_failures);
        return 1;
    }
    puts("PASS user-themes-test");
    return 0;
}
