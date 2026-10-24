/* Exercises the i18n table loader against files it will actually meet: a
 * compiled table, a hand-edited TSV override, and several kinds of damaged
 * input. The damaged cases matter most -- these files live on a FAT32 SD card
 * that gets yanked mid-write, and a translation table must never be able to
 * take the launcher down with it. */

#include "internal/i18n/i18n.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

static void expect_str(const char *label, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "i18n-test: %s got=%s want=%s\n",
                label, got ? got : "(null)", want);
        failures++;
    }
}

static void expect_true(const char *label, bool cond) {
    if (!cond) {
        fprintf(stderr, "i18n-test: %s was false\n", label);
        failures++;
    }
}

static void write_file(const char *path, const void *data, size_t len) {
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "i18n-test: cannot write %s\n", path); exit(1); }
    fwrite(data, 1, len, fp);
    fclose(fp);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: jawaka-i18n-test <fixture-dir>\n");
        return 2;
    }
    const char *dir = argv[1];
    char platform[PATH_MAX], userdata[PATH_MAX], path[PATH_MAX];
    snprintf(platform, sizeof(platform), "%s/platform", dir);
    snprintf(userdata, sizeof(userdata), "%s/userdata", dir);
    setenv("UMRK_PLATFORM_PATH", platform, 1);
    setenv("UMRK_INTERNAL_DATA_PATH", userdata, 1);

    /* ── English is the no-op default ─────────────────────────────────── */
    expect_true("en loads nothing", !jw_i18n_load("en"));
    expect_str("en passthrough", T("Settings"), "Settings");
    expect_str("en language", jw_i18n_language(), "en");
    expect_true("en count", jw_i18n_count() == 0);

    /* NULL and empty behave like English rather than crashing. */
    expect_true("null lang", !jw_i18n_load(NULL));
    expect_true("empty lang", !jw_i18n_load(""));

    /* ── Compiled table ───────────────────────────────────────────────── */
    expect_true("compiled loads", jw_i18n_load("zh_CN"));
    expect_str("translated", T("Settings"), "设置");
    expect_str("translated with ampersand", T("Display & Sound"), "显示与声音");
    expect_str("context key translated", T("verb|Open"), "打开");
    expect_str("language reported", jw_i18n_language(), "zh_CN");

    /* The two cases that make a partial translation shippable. */
    expect_str("fuzzy falls back", T("Refresh Rate"), "Refresh Rate");
    expect_str("empty msgstr falls back", T("Untranslated Thing"), "Untranslated Thing");
    expect_str("unknown key falls back", T("Never Seen Before"), "Never Seen Before");

    /* An untranslated context key must not leak its disambiguator to the UI. */
    expect_str("context stripped on miss", T("noun|Close"), "Close");
    /* ...but a pipe inside real text is not a context prefix. */
    expect_str("pipe in text kept", T("A | B"), "A | B");

    expect_str("null in", jw_i18n(NULL) == NULL ? "ok" : "bad", "ok");

    /* ── Live TSV override wins over the compiled table ───────────────── */
    snprintf(path, sizeof(path), "%s/i18n", userdata);
    mkdir(userdata, 0755);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/i18n/zh_CN.tsv", userdata);
    const char *tsv =
        "# translator's export\n"
        "Settings\t设置OVERRIDE\n"
        "\n"
        "Refresh Rate\t刷新率\n"
        "Malformed line with no tab\n"
        "Empty value\t\n";
    write_file(path, tsv, strlen(tsv));

    expect_true("tsv loads", jw_i18n_load("zh_CN"));
    expect_str("override wins", T("Settings"), "设置OVERRIDE");
    expect_str("override adds", T("Refresh Rate"), "刷新率");
    /* The override replaces the table rather than merging, so a key only the
       compiled table had is English again. Worth asserting: a translator
       testing a partial export should see exactly their file. */
    expect_str("override replaces", T("Display & Sound"), "Display & Sound");
    expect_str("malformed line skipped", T("Malformed line with no tab"),
               "Malformed line with no tab");
    expect_str("empty value skipped", T("Empty value"), "Empty value");
    expect_true("tsv count", jw_i18n_count() == 2);

    unlink(path);

    /* ── Damaged input must degrade, never crash ─────────────────────── */
    snprintf(path, sizeof(path), "%s/i18n/zh_CN.jwi", platform);

    /* Sized from the file, not a fixed buffer. This read into a 64 KB stack
       array, which would silently truncate a larger table and then write the
       truncated copy back as the "restored" one -- Mexican Spanish is already
       49 KB. Reading the real size keeps the test independent of how many
       strings a language has. */
    size_t good_len = 0;
    char *good = NULL;
    FILE *fp = fopen(path, "rb");
    if (fp && fseek(fp, 0, SEEK_END) == 0) {
        long sz = ftell(fp);
        rewind(fp);
        if (sz > 0 && (good = malloc((size_t)sz)) != NULL)
            good_len = fread(good, 1, (size_t)sz, fp);
    }
    if (fp) fclose(fp);
    expect_true("fixture readable", good && good_len > 24);
    if (!good) return 1;

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s/i18n/zh_CN.jwi.bak", platform);
    write_file(tmp, good, good_len);

    write_file(path, "not a table at all", 18);
    expect_true("bad magic rejected", !jw_i18n_load("zh_CN"));
    expect_str("bad magic falls back", T("Settings"), "Settings");

    write_file(path, good, good_len / 2);          /* truncated mid-pool */
    expect_true("truncated rejected", !jw_i18n_load("zh_CN"));
    expect_str("truncated falls back", T("Settings"), "Settings");

    write_file(path, good, 8);                     /* header only, cut short */
    expect_true("stub rejected", !jw_i18n_load("zh_CN"));

    write_file(path, "", 0);
    expect_true("empty rejected", !jw_i18n_load("zh_CN"));

    /* A corrupt offset must be caught at load, not by reading out of bounds on
       some later lookup. Point the first entry's key at the far end of nowhere. */
    /* Its own buffer. This used to reuse `tmp` -- a PATH_MAX path buffer, 1 KB on
       macOS -- as scratch space for a copy of the whole table. It fit the tiny
       fixture the test was written against; a real compiled table overran it by
       34 KB and fortified libc aborted the run, so none of the checks below had
       been running. */
    char *corrupt = malloc(good_len);
    if (!corrupt) return 1;
    memcpy(corrupt, good, good_len);
    corrupt[24 + 4] = (char)0xFF; corrupt[24 + 5] = (char)0xFF;
    corrupt[24 + 6] = (char)0xFF; corrupt[24 + 7] = (char)0x7F;
    write_file(path, corrupt, good_len);
    free(corrupt);
    expect_true("out-of-range offset rejected", !jw_i18n_load("zh_CN"));

    /* Restore, confirm we are back to a working table, and shut down clean. */
    snprintf(tmp, sizeof(tmp), "%s/i18n/zh_CN.jwi.bak", platform);
    /* The backup holds exactly `good`, so restore from memory rather than
       re-reading it into a buffer that might again be the wrong size. */
    write_file(path, good, good_len);
    unlink(tmp);
    free(good);
    expect_true("restored table loads", jw_i18n_load("zh_CN"));
    expect_str("restored lookup", T("Settings"), "设置");

    jw_i18n_shutdown();
    expect_str("after shutdown", T("Settings"), "Settings");
    expect_str("language after shutdown", jw_i18n_language(), "en");

    /* Reload after shutdown must not double-free or leak the previous table. */
    expect_true("reload after shutdown", jw_i18n_load("zh_CN"));
    jw_i18n_shutdown();
    jw_i18n_shutdown();                            /* idempotent */

    /* ── Format-string safety ─────────────────────────────────────────── */
    /* A translation whose printf conversions differ from its key's must lose
       the lookup, not crash snprintf later. The TSV is hand-edited on an SD
       card, so this is a when, not an if. */
    snprintf(path, sizeof(path), "%s/i18n/zh_CN.tsv", userdata);
    {
        const char *fmt_tsv =
            "%d games\t%d 个游戏\n"                /* compatible: kept    */
            "%d apps\t%s 个应用\n"                 /* %d vs %s: refused   */
            "Found %d in %d ms\t在 %d 毫秒内找到 %d\n" /* same, reordered ok */
            "Save %% done\t完成 %%\n";             /* %% is not a conversion */
        write_file(path, fmt_tsv, strlen(fmt_tsv));
    }
    expect_true("fmt tsv loads", jw_i18n_load("zh_CN"));
    expect_str("compatible fmt kept", T("%d games"), "%d 个游戏");
    expect_str("mismatched fmt refused", T("%d apps"), "%d apps");
    expect_str("same-order multi ok", T("Found %d in %d ms"), "在 %d 毫秒内找到 %d");
    expect_str("percent-escape ok", T("Save %% done"), "完成 %%");
    unlink(path);

    /* ── Language classification and discovery ───────────────────────── */
    expect_true("zh is cjk", jw_i18n_language_is_cjk("zh_CN"));
    expect_true("ja is cjk", jw_i18n_language_is_cjk("ja"));
    expect_true("ko is cjk", jw_i18n_language_is_cjk("ko_KR"));
    expect_true("en is not cjk", !jw_i18n_language_is_cjk("en"));
    expect_true("null is not cjk", !jw_i18n_language_is_cjk(NULL));
    expect_true("empty is not cjk", !jw_i18n_language_is_cjk(""));

    const char *langs[JW_I18N_MAX_LANGUAGES];
    size_t n = jw_i18n_available(langs, JW_I18N_MAX_LANGUAGES);
    expect_true("finds the compiled table", n == 1 && strcmp(langs[0], "zh_CN") == 0);

    /* A dropped .tsv must not double-count a language the compiled table
       already offers -- the Settings row would show zh_CN twice. */
    snprintf(path, sizeof(path), "%s/i18n/zh_CN.tsv", userdata);
    write_file(path, "Settings\tX\n", 11);
    n = jw_i18n_available(langs, JW_I18N_MAX_LANGUAGES);
    expect_true("tsv does not duplicate", n == 1);
    unlink(path);

    /* A language present only as a .tsv still gets offered, which is how a
       translator makes the row appear without a build. */
    snprintf(path, sizeof(path), "%s/i18n/ja.tsv", userdata);
    write_file(path, "Settings\t\xe8\xa8\xad\xe5\xae\x9a\n", 16);
    n = jw_i18n_available(langs, JW_I18N_MAX_LANGUAGES);
    expect_true("tsv-only language offered", n == 2);
    unlink(path);

    /* ── Capacity ─────────────────────────────────────────────────────── */
    /* Exactly JW_I18N_MAX_LANGUAGES are offered and one more is refused, not
       stored: the limit is the constant, not a number written out somewhere
       else. The overflow is logged by the scanner. */
    {
        char cap_path[PATH_MAX];
        for (int i = 0; i <= JW_I18N_MAX_LANGUAGES; i++) {   /* one past the limit */
            snprintf(cap_path, sizeof(cap_path), "%s/i18n/x%02d.tsv", userdata, i);
            write_file(cap_path, "Settings\tX\n", 11);
        }
        const char *many[JW_I18N_MAX_LANGUAGES];
        size_t got = jw_i18n_available(many, JW_I18N_MAX_LANGUAGES);
        expect_true("capacity is the constant", got == JW_I18N_MAX_LANGUAGES);
        /* A caller asking for fewer than the limit is still honored. */
        expect_true("smaller max honored", jw_i18n_available(many, 3) == 3);
        for (int i = 0; i <= JW_I18N_MAX_LANGUAGES; i++) {
            snprintf(cap_path, sizeof(cap_path), "%s/i18n/x%02d.tsv", userdata, i);
            unlink(cap_path);
        }
    }

    /* ── Coverage recording ─────────────────────────────────────────── */

    /* Off unless asked for. A tester enables it; nobody else pays for it. */
    unsetenv("JAWAKA_I18N_COVERAGE");
    setenv("JAWAKA_I18N_COVERAGE_TAG", "i18n-test", 1);
    setenv("LOGS_PATH", userdata, 1);
    expect_true("coverage off by default", jw_i18n_load("zh_CN") &&
                                           !jw_i18n_coverage_enabled());
    (void)T("Definitely Not Translated");
    expect_true("nothing counted when off", jw_i18n_coverage_count() == 0);

    setenv("JAWAKA_I18N_COVERAGE", "1", 1);
    expect_true("coverage on with env", jw_i18n_load("zh_CN") &&
                                        jw_i18n_coverage_enabled());

    /* A hit must not be recorded, and the same miss repeated must count once --
       this sits on the render path, where the same keys miss every redraw. */
    (void)T("Settings");
    (void)T("Definitely Not Translated");
    (void)T("Definitely Not Translated");
    (void)T("Also Missing");
    expect_true("records misses, deduped, hits excluded",
                jw_i18n_coverage_count() == 2);

    /* The file is written as keys are found, not at exit -- that is the whole
       point of the rewrite, so assert it is on disk BEFORE any teardown. */
    snprintf(path, sizeof(path), "%s/i18n-coverage-i18n-test.txt", userdata);
    {
        FILE *fp = fopen(path, "r");
        expect_true("file exists mid-run", fp != NULL);
        int found = 0;
        if (fp) {
            char line[512];
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\n")] = '\0';
                if (strcmp(line, "Definitely Not Translated") == 0 ||
                    strcmp(line, "Also Missing") == 0) found++;
            }
            fclose(fp);
        }
        expect_true("both misses already on disk", found == 2);
    }

    /* A language change must not lose what is already recorded: reloading frees
       the in-memory set, and the file has to survive that. */
    expect_true("reload", jw_i18n_load("zh_CN"));
    (void)T("Only One Now");
    expect_true("set is per-load", jw_i18n_coverage_count() == 1);
    {
        FILE *fp = fopen(path, "r");
        int lines = 0;
        if (fp) {
            char line[512];
            while (fgets(line, sizeof(line), fp)) if (line[0] != '#') lines++;
            fclose(fp);
        }
        expect_true("earlier keys survived the reload", lines == 3);
    }
    unsetenv("JAWAKA_I18N_COVERAGE");
    unsetenv("JAWAKA_I18N_COVERAGE_TAG");
    jw_i18n_shutdown();

    if (failures) {
        fprintf(stderr, "i18n-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("i18n-test\tok\n");
    return 0;
}
