#include "cmd/jawaka-osd/osd_layout.h"
#include "cmd/jawaka-osd/osd_utf8.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Every byte is 10 px wide: enough to exercise the shortening loop exactly. */
static int measure_bytes(void *ctx, const char *text) {
    (void)ctx;
    return (int)strlen(text) * 10;
}

/* Counts characters, not bytes, so multibyte text is not penalised. */
static int measure_chars(void *ctx, const char *text) {
    (void)ctx;
    int chars = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if ((*p & 0xC0) != 0x80) chars++;
    }
    return chars * 20;
}

static bool valid_utf8(const char *s) {
    size_t len = strlen(s);
    return jw_osd_utf8_boundary(s, len) == len;
}

static void metrics_match_plan(void) {
    jw_osd_banner_metrics m;
    jw_osd_banner_metrics_for(960, 2, &m);
    assert(m.max_w == 864);
    assert(m.inset == 11 && m.pad_y == 3);
    assert(m.radius == 7 && m.inner_radius == 6);
    assert(m.border == 1 && m.font_px == 28);
    assert(jw_osd_banner_text_max_w(&m) == 864 - 22);

    jw_osd_banner_metrics_for(1024, 0, &m);
    assert(m.inset == 12 && m.pad_y == 4 && m.radius == 8 && m.font_px == 24);
    jw_osd_banner_metrics_for(2048, 5, &m);   /* damped above the reference */
    assert(m.inset == 21 && m.pad_y == 7 && m.font_px == 34);
    assert(m.max_w == 1843);
    jw_osd_banner_metrics_for(640, 99, &m);
    assert(m.font_px == 28 && m.inset == 7);

    assert(jw_osd_font_bump(NULL) == 2 && jw_osd_font_bump("") == 2);
    assert(jw_osd_font_bump("0") == 0 && jw_osd_font_bump("5") == 5);
    assert(jw_osd_font_bump("6") == 2 && jw_osd_font_bump("-1") == 2);
    assert(jw_osd_font_bump("3x") == 2);
}

static void layout_stays_inside_output(void) {
    jw_osd_banner_metrics m;
    jw_osd_banner_metrics_for(960, 2, &m);
    jw_osd_banner_layout l;

    jw_osd_banner_layout_for(&m, 960, 720, 300, 200, true, 38, &l);
    assert(l.box.w == 300 + 22 && l.box.h == 38 * 2 + 3 * 3);
    assert(l.box.x == (960 - l.box.w) / 2);
    assert(l.box.y + l.box.h == 720 - 11);
    assert(l.title_x == l.box.x + 11 && l.title_y == l.box.y + 3);
    assert(l.action_x == l.box.x + (l.box.w - 200) / 2);
    assert(l.action_y == l.title_y + 38 + 3);
    assert(l.action_y + 38 + 3 == l.box.y + l.box.h);

    /* One row: the action position is unused. */
    jw_osd_banner_layout_for(&m, 960, 720, 120, 999, false, 38, &l);
    assert(l.box.h == 38 + 6 && l.box.w == 142 && l.action_x == 0);

    /* Measured text beyond the limit clamps to 90% of the full output. */
    jw_osd_banner_layout_for(&m, 960, 720, 5000, 0, false, 38, &l);
    assert(l.box.w == 864 && l.box.x == 48);
    assert(l.box.x >= 0 && l.box.x + l.box.w <= 960);

    /* A tiny output never places the box off screen. */
    jw_osd_banner_layout_for(&m, 960, 40, 100, 100, true, 38, &l);
    assert(l.box.y == 0);
}

static void fit_shortens_at_character_boundaries(void) {
    char out[64];
    assert(jw_osd_fit_text("short", 100, measure_bytes, NULL, out, sizeof(out)) == 50);
    assert(strcmp(out, "short") == 0);

    assert(jw_osd_fit_text("Splore import incomplete", 100, measure_bytes, NULL,
                           out, sizeof(out)) == 90);
    assert(strcmp(out, "Splore...") == 0);   /* trailing space dropped */

    /* Chinese text keeps whole characters. */
    const char *zh = "正在添加 Splore 收藏";
    assert(jw_osd_fit_text(zh, 120, measure_chars, NULL, out, sizeof(out)) <= 120);
    assert(valid_utf8(out) && strstr(out, "..."));
    assert(strcmp(out, "正在添...") == 0);

    /* A buffer too small for the text cuts on a boundary before measuring. */
    char tiny[8];
    assert(jw_osd_fit_text("收藏收藏", 1000, measure_chars, NULL, tiny, sizeof(tiny)) > 0);
    assert(valid_utf8(tiny) && strcmp(tiny, "收...") == 0);
    char three[3];
    assert(jw_osd_fit_text("abcdef", 1000, measure_bytes, NULL, three, sizeof(three)) == 0);
    assert(three[0] == '\0');

    /* Not even the ellipsis fits. */
    assert(jw_osd_fit_text("abcdef", 10, measure_bytes, NULL, out, sizeof(out)) == 0);
    assert(out[0] == '\0');
    assert(jw_osd_fit_text(NULL, 10, measure_bytes, NULL, out, sizeof(out)) == 0);
}

static void utf8_boundaries(void) {
    const char *s = "a\xE6\x94\xB6";   /* "a收" */
    assert(jw_osd_utf8_boundary(s, 4) == 4);
    assert(jw_osd_utf8_boundary(s, 3) == 1);
    assert(jw_osd_utf8_boundary(s, 2) == 1);
    assert(jw_osd_utf8_boundary(s, 1) == 1);
    assert(jw_osd_utf8_prev(s, 4) == 1 && jw_osd_utf8_prev(s, 1) == 0);
    assert(jw_osd_utf8_boundary("\xF0\x9F\x98", 3) == 0);
}

static void damage_covers_old_pixels(void) {
    jw_osd_rect a = { 10, 20, 300, 40 };
    jw_osd_rect same = a;
    jw_osd_rect narrower = { 40, 20, 240, 40 };
    jw_osd_rect shorter = { 10, 40, 300, 20 };
    jw_osd_rect d = jw_osd_damage_rect(true, a, same, 960, 720);
    assert(jw_osd_rect_equal(d, a));
    d = jw_osd_damage_rect(true, a, narrower, 960, 720);   /* width shrinks only */
    assert(d.x == 0 && d.y == 0 && d.w == 960 && d.h == 720);
    d = jw_osd_damage_rect(true, a, shorter, 960, 720);
    assert(d.w == 960 && d.h == 720);
    d = jw_osd_damage_rect(false, a, same, 960, 720);       /* nothing shown yet */
    assert(d.w == 960 && d.h == 720);
}

int main(void) {
    metrics_match_plan();
    layout_stays_inside_output();
    fit_shortens_at_character_boundaries();
    utf8_boundaries();
    damage_covers_old_pixels();
    puts("PASS osd-layout-test");
    return 0;
}
