#include "internal/discovery/art_path.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Set ART_PATH_TEST_DIR to a case-sensitive volume (an ext4 card, or a
   case-sensitive APFS image) to run the distinct-case assertions. On a
   case-insensitive volume those are skipped and the case-folding checks run
   instead. */

static char g_root[PATH_MAX];
static bool g_case_sensitive;
static int g_skipped;

static void touch(const char *dir, const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs(name, f);
    fclose(f);
}

static void make_fresh_dir(const char *name, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", g_root, name);
    assert(mkdir(out, 0755) == 0);
}

static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                remove_tree(child);
            } else {
                unlink(child);
            }
        }
        closedir(d);
    }
    rmdir(path);
}

static bool probe_case_sensitive(void) {
    char dir[PATH_MAX];
    make_fresh_dir("case-probe", dir, sizeof(dir));
    touch(dir, "Probe");
    char other[PATH_MAX];
    snprintf(other, sizeof(other), "%s/probe", dir);
    struct stat st;
    bool sensitive = stat(other, &st) != 0;
    remove_tree(dir);
    return sensitive;
}

/* Every lookup is checked both through a fresh index and without one. */
static void expect_find(const char *dir, const char *stem, const char *expected) {
    for (int pass = 0; pass < 2; pass++) {
        jw_art_index *index = pass ? jw_art_index_new() : NULL;
        char name[PATH_MAX];
        int rc = jw_art_find(index, dir, stem, name, sizeof(name));
        if (expected ? (rc != 0 || strcmp(name, expected) != 0) : rc != 1) {
            fprintf(stderr, "%s: expected %s for %s in %s, got rc=%d name=%s\n",
                    pass ? "index" : "one-off", expected ? expected : "(none)",
                    stem, dir, rc, rc == 0 ? name : "");
        }
        if (expected) {
            assert(rc == 0);
            assert(strcmp(name, expected) == 0);
        } else {
            assert(rc == 1);
        }
        jw_art_index_free(index);
    }
}

/* The spellings of `ext`, built without the implementation's help. */
static int spellings(const char *ext, char out[][8]) {
    size_t len = strlen(ext);
    int n = 1 << len;
    for (int mask = 0; mask < n; mask++) {
        for (size_t i = 0; i < len; i++) {
            out[mask][i] = (mask >> i) & 1 ? (char)toupper((unsigned char)ext[i])
                                           : (char)tolower((unsigned char)ext[i]);
        }
        out[mask][len] = '\0';
    }
    return n;
}

static void test_every_extension_spelling_is_found(void) {
    static const char *exts[] = { "png", "jpg", "jpeg" };
    int total = 0;
    for (size_t e = 0; e < 3; e++) {
        char spelled[16][8];
        int n = spellings(exts[e], spelled);
        for (int i = 0; i < n; i++) {
            char dir[PATH_MAX];
            char dirname[64];
            snprintf(dirname, sizeof(dirname), "spell-%s-%d", exts[e], i);
            make_fresh_dir(dirname, dir, sizeof(dir));
            char file[64];
            snprintf(file, sizeof(file), "Game.%s", spelled[i]);
            touch(dir, file);
            /* The stored name is always the spelling on disk. */
            expect_find(dir, "Game", file);
            total++;
        }
    }
    assert(total == 32);
}

static void test_format_precedence(void) {
    char dir[PATH_MAX];
    make_fresh_dir("png-jpg", dir, sizeof(dir));
    touch(dir, "Game.jpg");
    touch(dir, "Game.png");
    expect_find(dir, "Game", "Game.png");

    make_fresh_dir("jpg-jpeg", dir, sizeof(dir));
    touch(dir, "Game.jpeg");
    touch(dir, "Game.jpg");
    expect_find(dir, "Game", "Game.jpg");

    make_fresh_dir("jpeg-only", dir, sizeof(dir));
    touch(dir, "Game.jpeg");
    expect_find(dir, "Game", "Game.jpeg");

    /* A mixed-case PNG still beats a lower-case JPG. */
    make_fresh_dir("mixed-png-vs-jpg", dir, sizeof(dir));
    touch(dir, "Game.jpg");
    touch(dir, "Game.pNg");
    expect_find(dir, "Game", "Game.pNg");

    make_fresh_dir("upper-jpeg-vs-lower-jpg", dir, sizeof(dir));
    touch(dir, "Game.JPEG");
    touch(dir, "Game.jpg");
    expect_find(dir, "Game", "Game.jpg");

    make_fresh_dir("not-art", dir, sizeof(dir));
    touch(dir, "Game.gif");
    touch(dir, "Game.webp");
    touch(dir, "Game.png.txt");
    expect_find(dir, "Game", NULL);
}

static void test_same_format_case_order(void) {
    if (!g_case_sensitive) {
        printf("SKIP same-format case order (case-insensitive volume)\n");
        g_skipped++;
        return;
    }
    char dir[PATH_MAX];
    make_fresh_dir("case-order", dir, sizeof(dir));
    touch(dir, "Game.Png");
    touch(dir, "Game.PNG");
    touch(dir, "Game.png");
    expect_find(dir, "Game", "Game.png");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Game.png", dir);
    unlink(path);
    expect_find(dir, "Game", "Game.PNG");
    snprintf(path, sizeof(path), "%s/Game.PNG", dir);
    unlink(path);
    expect_find(dir, "Game", "Game.Png");

    /* Mixed spellings in bytewise order: "PnG" < "pNG" < "pnG". */
    make_fresh_dir("mixed-order", dir, sizeof(dir));
    touch(dir, "Game.pnG");
    touch(dir, "Game.pNG");
    touch(dir, "Game.PnG");
    expect_find(dir, "Game", "Game.PnG");
    snprintf(path, sizeof(path), "%s/Game.PnG", dir);
    unlink(path);
    expect_find(dir, "Game", "Game.pNG");

    make_fresh_dir("jpeg-mixed-order", dir, sizeof(dir));
    touch(dir, "Game.Jpeg");
    touch(dir, "Game.JpEg");
    expect_find(dir, "Game", "Game.JpEg");
}

static void test_stem_case(void) {
    char dir[PATH_MAX];
    if (g_case_sensitive) {
        make_fresh_dir("stem-case", dir, sizeof(dir));
        touch(dir, "Game.png");
        touch(dir, "game.jpg");
        expect_find(dir, "Game", "Game.png");
        expect_find(dir, "game", "game.jpg");

        make_fresh_dir("stem-case-alone", dir, sizeof(dir));
        touch(dir, "game.jpg");
        expect_find(dir, "Game", NULL);
        expect_find(dir, "GAME", NULL);
    } else {
        /* Where the filesystem folds case, opening "Game.jpg" would find
           "game.jpg", so the index matches the stem without case too. */
        make_fresh_dir("stem-fold", dir, sizeof(dir));
        touch(dir, "game.jpg");
        expect_find(dir, "Game", "game.jpg");
        expect_find(dir, "GAME", "game.jpg");
        expect_find(dir, "Gam", NULL);
    }
}

static void test_dotted_and_spaced_stems(void) {
    char dir[PATH_MAX];
    make_fresh_dir("dots", dir, sizeof(dir));
    touch(dir, "Super Mario Bros. 3.jpg");
    touch(dir, "Super Mario Bros.png");
    expect_find(dir, "Super Mario Bros. 3", "Super Mario Bros. 3.jpg");
    expect_find(dir, "Super Mario Bros", "Super Mario Bros.png");
    expect_find(dir, "Super Mario", NULL);
}

static void test_non_files(void) {
    char dir[PATH_MAX];
    make_fresh_dir("dir-match", dir, sizeof(dir));
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/Game.jpg", dir);
    assert(mkdir(sub, 0755) == 0);
    expect_find(dir, "Game", NULL);
    touch(dir, "Game.jpeg");
    expect_find(dir, "Game", "Game.jpeg");

    /* A symlink to a file is art (it opens as one); one to a folder is not. */
    make_fresh_dir("links", dir, sizeof(dir));
    touch(dir, "target.bin");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Linked.png", dir);
    assert(symlink("target.bin", path) == 0);
    snprintf(sub, sizeof(sub), "%s/folder", dir);
    assert(mkdir(sub, 0755) == 0);
    snprintf(path, sizeof(path), "%s/Folder.png", dir);
    assert(symlink("folder", path) == 0);
    expect_find(dir, "Linked", "Linked.png");
    expect_find(dir, "Folder", NULL);

    char missing[PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/does-not-exist", g_root);
    expect_find(missing, "Game", NULL);
}

static void test_index_keeps_its_snapshot(void) {
    char dir[PATH_MAX];
    make_fresh_dir("snapshot", dir, sizeof(dir));
    touch(dir, "One.jpg");

    jw_art_index *index = jw_art_index_new();
    assert(index);
    char name[PATH_MAX];
    assert(jw_art_find(index, dir, "One", name, sizeof(name)) == 0);
    assert(jw_art_find(index, dir, "Two", name, sizeof(name)) == 1);

    /* Files added after the folder was read belong to the next pass. */
    touch(dir, "Two.png");
    touch(dir, "One.png");
    assert(jw_art_find(index, dir, "Two", name, sizeof(name)) == 1);
    assert(jw_art_find(index, dir, "One", name, sizeof(name)) == 0);
    assert(strcmp(name, "One.jpg") == 0);
    jw_art_index_free(index);

    expect_find(dir, "Two", "Two.png");
    expect_find(dir, "One", "One.png");

    /* Many folders and many names in one index. */
    index = jw_art_index_new();
    for (int d = 0; d < 100; d++) {
        char sub[64];
        snprintf(sub, sizeof(sub), "many-%d", d);
        make_fresh_dir(sub, dir, sizeof(dir));
        for (int g = 0; g < 20; g++) {
            char file[64];
            snprintf(file, sizeof(file), "Game %d.%s", g, g % 2 ? "jpg" : "png");
            touch(dir, file);
        }
    }
    for (int d = 0; d < 100; d++) {
        snprintf(dir, sizeof(dir), "%s/many-%d", g_root, d);
        for (int g = 0; g < 20; g++) {
            char stem[64], want[64];
            snprintf(stem, sizeof(stem), "Game %d", g);
            snprintf(want, sizeof(want), "Game %d.%s", g, g % 2 ? "jpg" : "png");
            assert(jw_art_find(index, dir, stem, name, sizeof(name)) == 0);
            assert(strcmp(name, want) == 0);
        }
        assert(jw_art_find(index, dir, "Game 20", name, sizeof(name)) == 1);
    }
    jw_art_index_free(index);
}

static void test_locations_order(void) {
    jw_storage_source source;
    memset(&source, 0, sizeof(source));
    char base[PATH_MAX];
    make_fresh_dir("sd", base, sizeof(base));
    snprintf(source.root, sizeof(source.root), "%s", base);
    snprintf(source.images_path, sizeof(source.images_path), "%s/Images", base);
    snprintf(source.roms_path, sizeof(source.roms_path), "%s/Roms", base);

    char path[PATH_MAX];
    const char *dirs[] = { "Images", "Images/NES", "Images/FC", "Roms", "Roms/FC",
                           "Roms/FC/Imgs" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", base, dirs[i]);
        assert(mkdir(path, 0755) == 0);
    }

    char abs[PATH_MAX], rel[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Roms/FC/Imgs", base);
    touch(path, "Mario.png");
    assert(jw_art_find_for_rom(NULL, &source, "Images/NES", "FC", "Mario",
                               abs, sizeof(abs), rel, sizeof(rel)) == 0);
    assert(strcmp(rel, "Roms/FC/Imgs/Mario.png") == 0);

    /* A JPEG in Images/<folder> beats a PNG in Roms/<folder>/Imgs. */
    snprintf(path, sizeof(path), "%s/Images/FC", base);
    touch(path, "Mario.JPG");
    assert(jw_art_find_for_rom(NULL, &source, "Images/NES", "FC", "Mario",
                               abs, sizeof(abs), rel, sizeof(rel)) == 0);
    assert(strcmp(rel, "Images/FC/Mario.JPG") == 0);
    snprintf(path, sizeof(path), "%s/%s", base, rel);
    assert(strcmp(abs, path) == 0);

    /* Canonical image_root beats the physical folder regardless of format. */
    snprintf(path, sizeof(path), "%s/Images/NES", base);
    touch(path, "Mario.jpeg");
    jw_art_index *index = jw_art_index_new();
    assert(jw_art_find_for_rom(index, &source, "Images/NES", "FC", "Mario",
                               abs, sizeof(abs), rel, sizeof(rel)) == 0);
    assert(strcmp(rel, "Images/NES/Mario.jpeg") == 0);

    /* No canonical root (compat scan): physical folder first. */
    assert(jw_art_find_for_rom(index, &source, NULL, "FC", "Mario",
                               abs, sizeof(abs), rel, sizeof(rel)) == 0);
    assert(strcmp(rel, "Images/FC/Mario.JPG") == 0);

    assert(jw_art_find_for_rom(index, &source, "Images/NES", "FC", "Zelda",
                               abs, sizeof(abs), rel, sizeof(rel)) == 1);
    assert(jw_art_find_for_rom(index, &source, NULL, NULL, "Mario",
                               abs, sizeof(abs), rel, sizeof(rel)) == 1);
    jw_art_index_free(index);
}

static void test_stem_for_rom(void) {
    char *exts[] = { "md", "bin" };
    char *archives[] = { "zip" };
    jw_ra_system system;
    memset(&system, 0, sizeof(system));
    system.extensions.items = exts;
    system.extensions.count = 2;
    system.archive_extensions.items = archives;
    system.archive_extensions.count = 1;

    char out[PATH_MAX];
    jw_art_stem_for_rom(&system, "Sonic.md", out, sizeof(out));
    assert(strcmp(out, "Sonic") == 0);
    jw_art_stem_for_rom(&system, "Sonic.md.zip", out, sizeof(out));
    assert(strcmp(out, "Sonic") == 0);
    jw_art_stem_for_rom(&system, "Super Mario Bros. 3.ZIP", out, sizeof(out));
    assert(strcmp(out, "Super Mario Bros. 3") == 0);
    jw_art_stem_for_rom(NULL, "Sonic.md.zip", out, sizeof(out));
    assert(strcmp(out, "Sonic.md") == 0);
}

int main(void) {
    const char *base = getenv("ART_PATH_TEST_DIR");
    if (!base || !base[0]) base = getenv("TMPDIR");
    if (!base || !base[0]) base = "/tmp";
    snprintf(g_root, sizeof(g_root), "%s/jawaka-art-path.XXXXXX", base);
    assert(mkdtemp(g_root) != NULL);

    g_case_sensitive = probe_case_sensitive();
    printf("art-path-test: %s volume at %s\n",
           g_case_sensitive ? "case-sensitive" : "case-insensitive", g_root);

    test_every_extension_spelling_is_found();
    test_format_precedence();
    test_same_format_case_order();
    test_stem_case();
    test_dotted_and_spaced_stems();
    test_non_files();
    test_index_keeps_its_snapshot();
    test_locations_order();
    test_stem_for_rom();

    remove_tree(g_root);
    printf("art-path-test: ok (%d skipped)\n", g_skipped);
    return 0;
}
