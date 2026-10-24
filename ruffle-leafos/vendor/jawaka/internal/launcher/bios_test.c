/* Saturn BIOS selection: value grammar, precedence, validation and the bounded
   folder listing the picker pages through. No SDL and no database — everything
   the picker and the launch resolver rely on is exercised against a temporary
   BIOS tree. */

#include "internal/launcher/bios.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[JW_STORAGE_PATH_MAX];

static void path_of(char *out, size_t out_size, const char *fmt, ...) {
    char tail[JW_STORAGE_PATH_MAX];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tail, sizeof(tail), fmt, args);
    va_end(args);
    snprintf(out, out_size, "%s/%s", g_root, tail);
}

static void make_dir(const char *relative) {
    char path[JW_STORAGE_PATH_MAX];
    path_of(path, sizeof(path), "%s", relative);
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);
}

/* Sparse: only st_size matters here, and the paging case wants 600 of these. */
static void make_file(const char *relative, off_t size) {
    char path[JW_STORAGE_PATH_MAX];
    path_of(path, sizeof(path), "%s", relative);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(ftruncate(fd, size) == 0);
    close(fd);
}

static void remove_tree(const char *path) {
    char command[JW_STORAGE_PATH_MAX + 32];
    snprintf(command, sizeof(command), "rm -rf '%s'", path);
    assert(system(command) == 0);
}

/* Two configured cards, both available, each with its own BIOS root. */
static void build_sources(jw_storage_source_list *list, bool secondary_available) {
    memset(list, 0, sizeof(*list));
    list->count = 2;

    snprintf(list->sources[0].id, sizeof(list->sources[0].id), "primary");
    path_of(list->sources[0].root, sizeof(list->sources[0].root), "card1");
    path_of(list->sources[0].bios_path, sizeof(list->sources[0].bios_path),
            "card1/BIOS");
    list->sources[0].primary = true;
    list->sources[0].configured = true;
    list->sources[0].available = true;

    snprintf(list->sources[1].id, sizeof(list->sources[1].id), "secondary_sd");
    path_of(list->sources[1].root, sizeof(list->sources[1].root), "card2");
    path_of(list->sources[1].bios_path, sizeof(list->sources[1].bios_path),
            "card2/BIOS");
    list->sources[1].configured = true;
    list->sources[1].available = secondary_available;
}

static jw_bios_choice file_choice(const char *source_id, const char *rel_path) {
    jw_bios_choice choice;
    memset(&choice, 0, sizeof(choice));
    choice.kind = JW_BIOS_CHOICE_FILE;
    snprintf(choice.source_id, sizeof(choice.source_id), "%s", source_id);
    snprintf(choice.rel_path, sizeof(choice.rel_path), "%s", rel_path);
    return choice;
}

static void test_value_grammar(void) {
    jw_bios_choice choice;
    char value[JW_BIOS_VALUE_MAX];

    jw_bios_choice_parse(NULL, &choice);
    assert(choice.kind == JW_BIOS_CHOICE_DEFAULT);
    jw_bios_choice_parse("", &choice);
    assert(choice.kind == JW_BIOS_CHOICE_DEFAULT);
    /* A value a newer build wrote degrades to Default, never to another file. */
    jw_bios_choice_parse("auto", &choice);
    assert(choice.kind == JW_BIOS_CHOICE_DEFAULT);
    jw_bios_choice_parse("file:primary", &choice);
    assert(choice.kind == JW_BIOS_CHOICE_DEFAULT);
    jw_bios_choice_parse("file::SATURN/saturn.bin", &choice);
    assert(choice.kind == JW_BIOS_CHOICE_DEFAULT);
    jw_bios_choice_parse("file:primary:/etc/passwd", &choice);
    assert(choice.kind == JW_BIOS_CHOICE_DEFAULT);
    jw_bios_choice_parse("file:primary:../../etc/passwd", &choice);
    assert(choice.kind == JW_BIOS_CHOICE_DEFAULT);

    jw_bios_choice_parse("hle", &choice);
    assert(choice.kind == JW_BIOS_CHOICE_HLE);
    assert(jw_bios_choice_format(&choice, value, sizeof(value)));
    assert(strcmp(value, "hle") == 0);

    /* Source id and relative path stay together, so a half-applied update
       cannot point a saved selection at the other card's file. */
    /* The picker stores every selection below the dedicated Saturn folder. */
    jw_bios_choice_parse("file:secondary_sd:SATURN/Sega Saturn BIOS (JP).bin",
                         &choice);
    assert(choice.kind == JW_BIOS_CHOICE_FILE);
    assert(strcmp(choice.source_id, "secondary_sd") == 0);
    assert(strcmp(choice.rel_path, "SATURN/Sega Saturn BIOS (JP).bin") == 0);
    assert(jw_bios_choice_format(&choice, value, sizeof(value)));
    assert(strcmp(value, "file:secondary_sd:SATURN/Sega Saturn BIOS (JP).bin") == 0);

    /* Old root-level selections are not allowed to bypass BIOS/SATURN. */
    jw_bios_choice_parse("file:primary:saturn_bios.bin", &choice);
    assert(choice.kind == JW_BIOS_CHOICE_DEFAULT);

    /* Only the first colon separates: a filename may contain one. */
    jw_bios_choice_parse("file:primary:SATURN/odd:name.bin", &choice);
    assert(choice.kind == JW_BIOS_CHOICE_FILE);
    assert(strcmp(choice.source_id, "primary") == 0);
    assert(strcmp(choice.rel_path, "SATURN/odd:name.bin") == 0);

    /* Shell metacharacters are data. Nothing here evaluates them, and the
       round trip must not lose or rewrite a byte of the name. */
    jw_bios_choice_parse("file:primary:SATURN/weird/$(reboot) `x` ;rm -rf.bin", &choice);
    assert(choice.kind == JW_BIOS_CHOICE_FILE);
    assert(strcmp(choice.rel_path, "SATURN/weird/$(reboot) `x` ;rm -rf.bin") == 0);
    assert(jw_bios_choice_format(&choice, value, sizeof(value)));
    assert(strcmp(value, "file:primary:SATURN/weird/$(reboot) `x` ;rm -rf.bin") == 0);

    /* Default has no stored representation: the caller deletes the setting. */
    memset(&choice, 0, sizeof(choice));
    assert(!jw_bios_choice_format(&choice, value, sizeof(value)));

    jw_bios_choice a = file_choice("primary", "a.bin");
    jw_bios_choice b = file_choice("secondary_sd", "a.bin");
    jw_bios_choice c = file_choice("primary", "a.bin");
    assert(!jw_bios_choice_equal(&a, &b));   /* same name, different card */
    assert(jw_bios_choice_equal(&a, &c));
}

static void test_precedence(void) {
    jw_bios_resolution resolution;

    /* No override anywhere: today's behavior, HLE, marked as the default. */
    jw_bios_resolve(NULL, NULL, &resolution);
    assert(resolution.choice.kind == JW_BIOS_CHOICE_HLE);
    assert(resolution.origin == JW_BIOS_ORIGIN_DEFAULT);

    /* A game-level Default inherits the system file. */
    jw_bios_resolve("", "file:primary:SATURN/saturn.bin", &resolution);
    assert(resolution.choice.kind == JW_BIOS_CHOICE_FILE);
    assert(strcmp(resolution.choice.rel_path, "SATURN/saturn.bin") == 0);
    assert(resolution.origin == JW_BIOS_ORIGIN_SYSTEM);

    /* Explicit HLE at game level opts one title out of a system-wide file. */
    jw_bios_resolve("hle", "file:primary:SATURN/saturn.bin", &resolution);
    assert(resolution.choice.kind == JW_BIOS_CHOICE_HLE);
    assert(resolution.origin == JW_BIOS_ORIGIN_GAME);

    jw_bios_resolve("file:secondary_sd:SATURN/jp.bin",
                    "file:primary:SATURN/saturn.bin",
                    &resolution);
    assert(resolution.origin == JW_BIOS_ORIGIN_GAME);
    assert(strcmp(resolution.choice.source_id, "secondary_sd") == 0);

    /* A system-level Default restores HLE even with a game value present that
       is itself unparsable. */
    jw_bios_resolve("nonsense", "", &resolution);
    assert(resolution.choice.kind == JW_BIOS_CHOICE_HLE);
    assert(resolution.origin == JW_BIOS_ORIGIN_DEFAULT);
}

static void test_validation(void) {
    jw_storage_source_list sources;
    build_sources(&sources, true);

    char abs[JW_STORAGE_PATH_MAX];
    jw_bios_choice choice;

    /* A custom, non-standard filename with no known checksum is accepted on
       size alone; nothing calls it verified. */
    choice = file_choice("primary", "SATURN/my saturn dump.bin");
    assert(jw_bios_resolve_file(&sources, &choice, abs, sizeof(abs)) ==
           JW_BIOS_FILE_OK);
    char expected[JW_STORAGE_PATH_MAX];
    path_of(expected, sizeof(expected), "card1/BIOS/SATURN/my saturn dump.bin");
    assert(strcmp(abs, expected) == 0);

    /* Duplicate filenames on different cards stay distinct selections. */
    choice = file_choice("secondary_sd", "SATURN/my saturn dump.bin");
    assert(jw_bios_resolve_file(&sources, &choice, abs, sizeof(abs)) ==
           JW_BIOS_FILE_OK);
    path_of(expected, sizeof(expected), "card2/BIOS/SATURN/my saturn dump.bin");
    assert(strcmp(abs, expected) == 0);

    choice = file_choice("primary", "SATURN/toosmall.bin");
    assert(jw_bios_resolve_file(&sources, &choice, abs, sizeof(abs)) ==
           JW_BIOS_FILE_WRONG_SIZE);
    assert(abs[0] == '\0');

    choice = file_choice("primary", "SATURN/gone.bin");
    assert(jw_bios_resolve_file(&sources, &choice, abs, sizeof(abs)) ==
           JW_BIOS_FILE_MISSING);

    choice = file_choice("primary", "SATURN/nested");
    assert(jw_bios_resolve_file(&sources, &choice, abs, sizeof(abs)) ==
           JW_BIOS_FILE_NOT_REGULAR);

    /* A symlink out of the BIOS root is never resolved, even when its target
       is a perfectly valid image. */
    choice = file_choice("primary", "SATURN/escape.bin");
    assert(jw_bios_resolve_file(&sources, &choice, abs, sizeof(abs)) ==
           JW_BIOS_FILE_OUTSIDE_ROOT);

    choice = file_choice("primary", "../outside/saturn.bin");
    assert(jw_bios_resolve_file(&sources, &choice, abs, sizeof(abs)) ==
           JW_BIOS_FILE_INVALID_PATH);

    choice = file_choice("primary", "root-level.bin");
    assert(jw_bios_resolve_file(&sources, &choice, abs, sizeof(abs)) ==
           JW_BIOS_FILE_INVALID_PATH);

    choice = file_choice("no_such_card", "SATURN/my saturn dump.bin");
    assert(jw_bios_resolve_file(&sources, &choice, abs, sizeof(abs)) ==
           JW_BIOS_FILE_SOURCE_UNAVAILABLE);

    /* The card is configured but not mounted: the selection stays identifiable
       and is reported as unavailable rather than replaced. */
    build_sources(&sources, false);
    choice = file_choice("secondary_sd", "SATURN/my saturn dump.bin");
    assert(jw_bios_resolve_file(&sources, &choice, abs, sizeof(abs)) ==
           JW_BIOS_FILE_SOURCE_UNAVAILABLE);

    jw_bios_choice hle;
    memset(&hle, 0, sizeof(hle));
    hle.kind = JW_BIOS_CHOICE_HLE;
    assert(jw_bios_resolve_file(&sources, &hle, abs, sizeof(abs)) ==
           JW_BIOS_FILE_NO_CHOICE);
}

static int g_cancel_after = -1;
static int g_cancel_calls;

static bool cancel_probe(void *ctx) {
    (void)ctx;
    g_cancel_calls++;
    return g_cancel_after >= 0 && g_cancel_calls >= g_cancel_after;
}

static void test_listing(void) {
    char dir[JW_STORAGE_PATH_MAX];
    jw_bios_entry rows[JW_BIOS_PAGE_ROWS];
    jw_bios_list_result result;
    int count = 0;

    path_of(dir, sizeof(dir), "card1/BIOS/SATURN");
    assert(jw_bios_list_dir(dir, NULL, rows, JW_BIOS_PAGE_ROWS, &count,
                            NULL, NULL, &result) == 0);
    assert(!result.failed && !result.cancelled && !result.has_more);
    /* Subfolders first, then eligible files, each byte-ascending. The wrong
       sized file, the symlink and the unreadable-size decoys never appear. */
    assert(count == 3);
    assert(rows[0].is_dir && strcmp(rows[0].name, "many") == 0);
    assert(rows[1].is_dir && strcmp(rows[1].name, "nested") == 0);
    assert(!rows[2].is_dir && strcmp(rows[2].name, "my saturn dump.bin") == 0);

    /* Enumerating a folder does not descend into it. */
    path_of(dir, sizeof(dir), "card1/BIOS/SATURN/nested");
    assert(jw_bios_list_dir(dir, NULL, rows, JW_BIOS_PAGE_ROWS, &count,
                            NULL, NULL, &result) == 0);
    assert(count == 1);
    assert(rows[0].is_dir && strcmp(rows[0].name, "deeper") == 0);

    path_of(dir, sizeof(dir), "card1/BIOS/SATURN/nested/deeper");
    assert(jw_bios_list_dir(dir, NULL, rows, JW_BIOS_PAGE_ROWS, &count,
                            NULL, NULL, &result) == 0);
    assert(count == 1);
    assert(!rows[0].is_dir && strcmp(rows[0].name, "buried.bin") == 0);

    assert(jw_bios_list_dir("/no/such/folder", NULL, rows, JW_BIOS_PAGE_ROWS,
                            &count, NULL, NULL, &result) != 0);
    assert(result.failed && count == 0);
}

/* 600 eligible files in one folder, mixed with 400 ineligible ones: every
   eligible file stays reachable by paging, with no duplicate and no gap. */
static void test_paging(void) {
    char dir[JW_STORAGE_PATH_MAX];
    path_of(dir, sizeof(dir), "card1/BIOS/SATURN/many");

    jw_bios_entry rows[JW_BIOS_PAGE_ROWS];
    jw_bios_entry cursor;
    jw_bios_list_result result;
    int total = 0;
    int pages = 0;
    bool have_cursor = false;
    char previous[JW_BIOS_NAME_MAX] = "";

    for (;;) {
        int count = 0;
        assert(jw_bios_list_dir(dir, have_cursor ? &cursor : NULL, rows,
                                JW_BIOS_PAGE_ROWS, &count, NULL, NULL,
                                &result) == 0);
        assert(!result.failed);
        if (count == 0) {
            break;
        }
        for (int i = 0; i < count; i++) {
            assert(!rows[i].is_dir);
            /* Strictly increasing across the whole walk: no repeats, and the
               page boundary does not skip a name. */
            assert(previous[0] == '\0' || strcmp(rows[i].name, previous) > 0);
            snprintf(previous, sizeof(previous), "%s", rows[i].name);
        }
        total += count;
        pages++;
        cursor = rows[count - 1];
        have_cursor = true;
        if (!result.has_more) {
            break;
        }
        assert(count == JW_BIOS_PAGE_ROWS);
        assert(pages < 16);
    }
    assert(total == 600);
    assert(pages == 3);   /* 256 + 256 + 88, never silently truncated */

    /* Cancellation stops between batches and reports partial rows rather than
       pretending the folder is small. */
    g_cancel_after = 1;
    g_cancel_calls = 0;
    int count = 0;
    assert(jw_bios_list_dir(dir, NULL, rows, JW_BIOS_PAGE_ROWS, &count,
                            cancel_probe, NULL, &result) == 0);
    assert(result.cancelled);
    assert(result.examined <= JW_BIOS_SCAN_BATCH + 1);
    g_cancel_after = -1;
}

static void test_rel_helpers(void) {
    char out[JW_BIOS_REL_PATH_MAX];

    assert(jw_bios_rel_join("", "SATURN", out, sizeof(out)));
    assert(strcmp(out, "SATURN") == 0);
    assert(jw_bios_rel_join("SATURN", "jp.bin", out, sizeof(out)));
    assert(strcmp(out, "SATURN/jp.bin") == 0);
    assert(!jw_bios_rel_join("SATURN", "..", out, sizeof(out)));
    assert(!jw_bios_rel_join("SATURN", "a/b", out, sizeof(out)));
    assert(!jw_bios_rel_join("SATURN", "", out, sizeof(out)));

    jw_bios_rel_parent("SATURN/deep/jp.bin", out, sizeof(out));
    assert(strcmp(out, "SATURN/deep") == 0);
    jw_bios_rel_parent("SATURN", out, sizeof(out));
    assert(out[0] == '\0');
    jw_bios_rel_parent("", out, sizeof(out));
    assert(out[0] == '\0');
}

static void build_tree(void) {
    make_dir("card1");
    make_dir("card1/BIOS");
    make_dir("card1/BIOS/SATURN");
    make_dir("card1/BIOS/SATURN/nested");
    make_dir("card1/BIOS/SATURN/nested/deeper");
    make_dir("card1/BIOS/SATURN/many");
    make_dir("card2");
    make_dir("card2/BIOS");
    make_dir("card2/BIOS/SATURN");
    make_dir("outside");

    make_file("card1/BIOS/SATURN/my saturn dump.bin", JW_BIOS_SATURN_IMAGE_BYTES);
    make_file("card1/BIOS/SATURN/toosmall.bin", 1024);
    make_file("card1/BIOS/root-level.bin", JW_BIOS_SATURN_IMAGE_BYTES);
    make_file("card1/BIOS/notes.txt", 12);
    make_file("card1/BIOS/SATURN/nested/deeper/buried.bin", JW_BIOS_SATURN_IMAGE_BYTES);
    make_file("card2/BIOS/SATURN/my saturn dump.bin", JW_BIOS_SATURN_IMAGE_BYTES);
    make_file("outside/saturn.bin", JW_BIOS_SATURN_IMAGE_BYTES);

    char target[JW_STORAGE_PATH_MAX];
    char link[JW_STORAGE_PATH_MAX];
    path_of(target, sizeof(target), "outside/saturn.bin");
    path_of(link, sizeof(link), "card1/BIOS/SATURN/escape.bin");
    assert(symlink(target, link) == 0);

    for (int i = 0; i < 600; i++) {
        char name[64];
        snprintf(name, sizeof(name), "card1/BIOS/SATURN/many/bios-%04d.bin", i);
        make_file(name, JW_BIOS_SATURN_IMAGE_BYTES);
    }
    for (int i = 0; i < 400; i++) {
        char name[64];
        snprintf(name, sizeof(name), "card1/BIOS/SATURN/many/junk-%04d.dat", i);
        make_file(name, 64);
    }
}

int main(void) {
    char template_path[] = "/tmp/jw-bios-test-XXXXXX";
    assert(mkdtemp(template_path));
    /* realpath: /tmp is a symlink on macOS, and the resolver compares resolved
       paths, so the expected absolute paths have to be resolved too. */
    assert(realpath(template_path, g_root));

    build_tree();

    test_value_grammar();
    test_precedence();
    test_validation();
    test_listing();
    test_paging();
    test_rel_helpers();

    remove_tree(g_root);
    printf("bios-test: ok\n");
    return 0;
}
