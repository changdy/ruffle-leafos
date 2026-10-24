#include "internal/launcher/launch_notice.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(const char *label, bool actual, bool expected) {
    if (actual == expected) {
        return;
    }
    fprintf(stderr, "%s: got %s, expected %s\n", label,
            actual ? "true" : "false", expected ? "true" : "false");
    exit(1);
}

static void expect_remaining(const char *label, uint32_t actual, uint32_t expected) {
    if (actual == expected) {
        return;
    }
    fprintf(stderr, "%s: got %u, expected %u\n", label, actual, expected);
    exit(1);
}

/* 10 px per character, 20 px per CJK character. */
static int fake_width(void *context, const char *text) {
    (void)context;
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if ((*p & 0xC0) == 0x80) continue;
        w += *p >= 0xE0 ? 20 : 10;
    }
    return w;
}

static void expect_split(const char *label, const char *text, int max_w,
                         int count, const char *first, const char *second) {
    jw_launch_notice_lines lines;
    memset(&lines, 'z', sizeof(lines));
    int got = jw_launch_notice_split(text, max_w, fake_width, NULL, &lines);
    if (got == count && lines.count == count && strcmp(lines.line[0], first) == 0 &&
        strcmp(lines.line[1], second) == 0)
        return;
    fprintf(stderr, "%s: got %d [%s] [%s], expected %d [%s] [%s]\n", label, got,
            lines.line[0], lines.line[1], count, first, second);
    exit(1);
}

int main(void) {
    jw_system_notice notice;
    memset(&notice, 0, sizeof(notice));

    /* An untouched notice draws nothing. */
    expect("initially empty", jw_launch_notice_text(&notice, 0)[0] == '\0', true);
    expect_remaining("initially no remaining",
                     jw_launch_notice_remaining(&notice, 0), 0);

    /* Set starts the lifetime. */
    const char *text = "ports launcher exited (status 3)";
    jw_launch_notice_show(&notice, text, 1000);
    expect("shown text", strcmp(jw_launch_notice_text(&notice, 1000), text) == 0, true);
    expect_remaining("full lifetime",
                     jw_launch_notice_remaining(&notice, 1000), JW_SYSTEM_NOTICE_MS);
    expect_remaining("one tick left",
                     jw_launch_notice_remaining(&notice, 1000 + JW_SYSTEM_NOTICE_MS - 1), 1);

    /* Reads alone never clear or shorten the notice. */
    (void)jw_launch_notice_text(&notice, 1000 + JW_SYSTEM_NOTICE_MS - 1);
    (void)jw_launch_notice_remaining(&notice, 1000 + JW_SYSTEM_NOTICE_MS - 1);
    expect("reads do not clear",
           jw_launch_notice_remaining(&notice, 1000 + JW_SYSTEM_NOTICE_MS - 1) == 1, true);

    /* At the deadline it reads as gone but still exists until the tick. */
    expect("expired text empty",
           jw_launch_notice_text(&notice, 1000 + JW_SYSTEM_NOTICE_MS)[0] == '\0', true);
    expect("expiry reports the clear",
           jw_launch_notice_expire(&notice, 1000 + JW_SYSTEM_NOTICE_MS), true);
    expect("storage cleared",
           notice.text[0] == '\0' && notice.started_ms == 0, true);
    expect("expiry is not repeated",
           jw_launch_notice_expire(&notice, 1000 + JW_SYSTEM_NOTICE_MS + 60000), false);

    /* A new launch attempt clears immediately... */
    jw_launch_notice_show(&notice, "relocating files", 5000);
    expect("live before retry",
           jw_launch_notice_remaining(&notice, 5000) == JW_SYSTEM_NOTICE_MS, true);
    jw_launch_notice_begin(&notice);
    expect("new attempt clears", notice.text[0] == '\0', true);
    expect_remaining("new attempt leaves no lifetime",
                     jw_launch_notice_remaining(&notice, 5000), 0);

    /* ...so a retry restarts the lifetime rather than inheriting the old one. */
    jw_launch_notice_show(&notice, "unsupported system", 5001);
    expect_remaining("retry restarts lifetime",
                     jw_launch_notice_remaining(&notice, 5001), JW_SYSTEM_NOTICE_MS);
    expect("retry outlives the first deadline",
           jw_launch_notice_remaining(&notice, 5000 + JW_SYSTEM_NOTICE_MS) > 0, true);

    /* A long translated message is kept, truncated to the buffer. */
    char long_text[512];
    memset(long_text, 'x', sizeof(long_text));
    long_text[sizeof(long_text) - 1] = '\0';
    jw_launch_notice_show(&notice, long_text, 9000);
    expect("long message terminated",
           notice.text[sizeof(notice.text) - 1] == '\0', true);
    expect("long message kept", strlen(notice.text) == sizeof(notice.text) - 1, true);

    expect_split("fits on one line", "Launch canceled", 1000, 1, "Launch canceled", "");
    expect_split("empty notice", "", 1000, 0, "", "");
    /* 600 px: the break nearest the middle, so neither line is much wider. */
    expect_split("balanced two lines",
                 "Couldn't resume: Hardcore mode doesn't allow loading a save.", 400, 2,
                 "Couldn't resume: Hardcore mode", "doesn't allow loading a save.");
    /* Too long for two lines: line one fills, line two keeps the rest for the
       caller to ellipsize. */
    expect_split("overflow fills line one",
                 "Couldn't resume: Hardcore mode doesn't allow loading a save.", 250, 2,
                 "Couldn't resume: Hardcore", "mode doesn't allow loading a save.");
    expect_split("space run is not kept", "Launch   canceled", 100, 2, "Launch", "canceled");
    expect_split("long word cut at a character", "Supercalifragilisticexpialidocious", 200, 2,
                 "Supercalifragilistic", "expialidocious");
    /* Chinese has no spaces: break between characters, never inside one. */
    expect_split("CJK breaks between characters",
                 "无法继续游戏。请打开游戏并尝试读取存档。", 250, 2,
                 "无法继续游戏。请打开", "游戏并尝试读取存档。");
    /* Breaking before 。 ties on width but would start line two with it. */
    expect_split("closing punctuation stays on line one", "一二三四。五六七八", 100, 2,
                 "一二三四。", "五六七八");
    /* Breaking after （ would be narrower, but would end line one with it. */
    expect_split("opening punctuation stays on line two", "一二三（四五六七", 100, 2,
                 "一二三", "（四五六七");
    expect_split("CJK keeps a spaced word whole", "请重启 Leaf 后重试。", 120, 2,
                 "请重启 Leaf", "后重试。");

    printf("launch-notice-test: ok\n");
    return 0;
}
