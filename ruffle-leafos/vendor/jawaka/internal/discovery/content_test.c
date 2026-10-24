#include "internal/discovery/content.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static jw_storage_source_list sources;
static char fixture[JW_STORAGE_PATH_MAX];

static void join(char *out, size_t size, const char *root, const char *name) {
    int n = snprintf(out, size, "%s/%s", root, name);
    assert(n > 0 && (size_t)n < size);
}

static void directory(const char *name) {
    char path[JW_STORAGE_PATH_MAX];
    join(path, sizeof(path), fixture, name);
    assert(mkdir(path, 0700) == 0);
}

static void write_file(const char *name, const char *contents) {
    char path[JW_STORAGE_PATH_MAX];
    join(path, sizeof(path), fixture, name);
    FILE *fp = fopen(path, "wb");
    assert(fp);
    assert(fwrite(contents, 1, strlen(contents), fp) == strlen(contents));
    assert(fclose(fp) == 0);
}

static jw_content inspect(const char *path, const jw_ra_system *system) {
    jw_content content;
    char error[512];
    if (jw_content_inspect(&sources, "primary", path, system, &content, error, sizeof(error))) {
        fprintf(stderr, "%s: %s\n", path, error);
        assert(0);
    }
    return content;
}

static void fails(const char *path, const jw_ra_system *system, const char *message) {
    jw_content content;
    char error[512];
    if (jw_content_inspect(&sources, "primary", path, system, &content, error, sizeof(error)) != -1) {
        fprintf(stderr, "%s: expected inspection failure containing '%s'\n", path, message);
        jw_content_free(&content);
        assert(0);
    }
    if (!strstr(error, message)) {
        fprintf(stderr, "%s: expected '%s', got '%s'\n", path, message, error);
        assert(0);
    }
    assert(content.files == NULL && content.references == NULL && content.file_count == 0);
}

static jw_content_file *file_named(jw_content *content, const char *path) {
    for (size_t i = 0; i < content->file_count; i++)
        if (!strcmp(content->files[i].rom_relpath, path)) return &content->files[i];
    assert(0);
    return NULL;
}

static void test_formats(void) {
    directory("cardA/Roms/PS");
    directory("cardA/Roms/PS/discs");
    write_file("cardA/Roms/PS/discs/Track 1.bin", "first");
    write_file("cardA/Roms/PS/discs/track2.bin", "second");
    const char *cue = "FILE \"Track 1.bin\" BINARY\r\n TRACK 01 MODE2/2352\r\n INDEX 01 00:00:00\r\n"
                      "FILE track2.bin BINARY\r\n TRACK 02 AUDIO\r\n";
    write_file("cardA/Roms/PS/discs/Disc 1.cue", cue);
    write_file("cardA/Roms/PS/discs/Disc 2.toc", "CD_ROM\nDATAFILE \"Track 1.bin\"\nFILE track2.bin 0\n");
    write_file("cardA/Roms/PS/discs/Disc 3.gdi", "2\n1 0 4 2352 \"Track 1.bin\" 0\n2 150 0 2352 track2.bin 0\n");
    write_file("cardA/Roms/PS/archive.zip", "archive");
    const char *playlist = "\xef\xbb\xbf#EXTM3U\r\n#EXTINF:0,First disc\r\n\"discs\\Disc 1.cue\"\r\n"
                           "discs/Disc 2.toc|Second disc\r\n./discs/Disc 3.gdi\r\n"
                           "archive.zip#Folder/DiskA.adf|Archive disc\r\n"
                           "archive.zip#Folder/diska.adf|Different member\r\n"
                           "discs/../discs/Disc 1.cue|Repeated disc\r\n#SAVEDISK:Save Disk\r\n";
    write_file("cardA/Roms/PS/Game.m3u", playlist);
    jw_content content = inspect("PS/Game.m3u", NULL);
    /* Repeated discs and both archive members resolve to one file each. */
    assert(content.file_count == 7);
    assert(!strcmp(content.files[content.launch_file].rom_relpath, "PS/Game.m3u"));
    const char *names[] = {"PS/discs/Disc 1.cue", "PS/discs/Disc 2.toc", "PS/discs/Disc 3.gdi",
                           "PS/discs/Track 1.bin", "PS/discs/track2.bin", "PS/archive.zip"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) assert(!file_named(&content, names[i])->missing);
    assert(file_named(&content, "PS/discs/Track 1.bin")->size == 5);
    assert(content.reference_count == 12);
    jw_content_free(&content);
    assert(!content.file_count && !content.reference_count);

    write_file("cardA/Roms/PS/one.m3u8", "discs/Disc 1.cue\n");
    content = inspect("PS/one.m3u8", NULL);
    assert(content.file_count == 4);
    jw_content_free(&content);
    content = inspect("PS/discs/Disc 2.toc", NULL);
    assert(content.file_count == 3);
    jw_content_free(&content);
}

static void test_cmd(void) {
    directory("cardA/Roms/PC98");
    directory("cardA/Roms/PC98/disks");
    write_file("cardA/Roms/PC98/disks/Disk 1.hdi", "disk");
    write_file("cardA/Roms/PC98/Game.cmd", "np2kai \"disks\\Disk 1.hdi\" missing.fdi --speed 2 --label=ordinary.hdi\n");
    char *extensions[] = {"cmd", "hdi", "fdi"};
    jw_ra_system system = {.extensions = {.items = extensions, .count = 3}};
    jw_content content = inspect("PC98/Game.cmd", &system);
    assert(content.file_count == 3);
    assert(!file_named(&content, "PC98/disks/Disk 1.hdi")->missing);
    assert(file_named(&content, "PC98/missing.fdi")->missing);
    jw_content_free(&content);
    fails("PC98/Game.cmd", NULL, "system's file formats");
    write_file("cardA/Roms/PC98/bad.cmd", "np2kai \"unterminated.hdi\n");
    fails("PC98/bad.cmd", &system, "unterminated quote");
}

static void test_missing_and_errors(void) {
    write_file("cardA/Roms/PS/missing.m3u", "Missing.cue\nabsent/folder/disc.bin|Missing image\n");
    jw_content content = inspect("PS/missing.m3u", NULL);
    assert(content.file_count == 3);
    assert(file_named(&content, "PS/Missing.cue")->missing);
    assert(file_named(&content, "PS/absent/folder/disc.bin")->missing);
    jw_content_free(&content);
    content = inspect("PS/gone.m3u", NULL);
    assert(content.file_count == 1 && content.files[0].missing);
    jw_content_free(&content);

    write_file("cardA/Roms/PS/bad.cue", "FILE \"unterminated.bin BINARY\n");
    fails("PS/bad.cue", NULL, "FILE filename");
    write_file("cardA/Roms/PS/no-tracks.cue", "REM missing FILE records\n");
    fails("PS/no-tracks.cue", NULL, "track records");
    write_file("cardA/Roms/PS/bad.gdi", "2\n1 0 4 2352 disc.bin 0\n");
    fails("PS/bad.gdi", NULL, "track records");
    write_file("cardA/Roms/PS/quotes.m3u", "\"unterminated.chd\n");
    fails("PS/quotes.m3u", NULL, "unterminated quote");
    write_file("cardA/Roms/PS/cycle.m3u", "cycle.m3u\n");
    fails("PS/cycle.m3u", NULL, "cycle");
    write_file("cardA/Roms/PS/outer.m3u", "inner.m3u\n");
    write_file("cardA/Roms/PS/inner.m3u", "outer.m3u\n");
    fails("PS/outer.m3u", NULL, "cycle");
    write_file("cardA/Roms/PS/directory.m3u", "discs\n");
    fails("PS/directory.m3u", NULL, "not a regular file");
    write_file("cardA/Roms/PS/not-directory.m3u", "archive.zip/disc.cue\n");
    fails("PS/not-directory.m3u", NULL, "Not a directory");
    write_file("cardA/Roms/PS/outside.m3u", "../../../outside.bin\n");
    fails("PS/outside.m3u", NULL, "outside the mounted ROM roots");
    write_file("cardA/Roms/PS/missing-parent.m3u", "absent/../archive.zip\n");
    fails("PS/missing-parent.m3u", NULL, "Cannot traverse missing directory");

    char path[JW_STORAGE_PATH_MAX];
    join(path, sizeof(path), fixture, "cardA/Roms/PS/unreadable.cue");
    write_file("cardA/Roms/PS/unreadable.cue", "FILE disc.bin BINARY\n");
    assert(chmod(path, 0000) == 0);
    if (access(path, R_OK)) fails("PS/unreadable.cue", NULL, "Cannot read descriptor");
    else puts("content: unreadable-file check skipped for privileged user");
    assert(chmod(path, 0600) == 0);

    directory("cardA/Roms/PS/unreadable-dir");
    write_file("cardA/Roms/PS/unreadable-dir/disc.chd", "disc");
    join(path, sizeof(path), fixture, "cardA/Roms/PS/unreadable-dir");
    assert(chmod(path, 0000) == 0);
    if (access(path, R_OK | X_OK)) fails("PS/unreadable-dir/disc.chd", NULL, "Cannot resolve");
    assert(chmod(path, 0700) == 0);

    join(path, sizeof(path), fixture, "cardA/Roms/PS/pipe");
    assert(mkfifo(path, 0600) == 0);
    write_file("cardA/Roms/PS/fifo.m3u", "pipe\n");
    fails("PS/fifo.m3u", NULL, "not a regular file");
}

static void test_sources_and_symlinks(void) {
    directory("cardB/Roms/PS");
    write_file("cardB/Roms/PS/Other.chd", "other-card");
    char reference[JW_STORAGE_PATH_MAX], path[JW_STORAGE_PATH_MAX];
    join(reference, sizeof(reference), fixture, "cardB/Roms/PS/Other.chd");
    write_file("cardA/Roms/PS/cross.m3u", reference);
    jw_content content = inspect("PS/cross.m3u", NULL);
    assert(content.file_count == 2 && !strcmp(file_named(&content, "PS/Other.chd")->source_id, "secondary_sd"));
    jw_content_free(&content);
    sources.sources[1].available = false;
    fails("PS/cross.m3u", NULL, "outside the mounted ROM roots");
    sources.sources[1].available = true;

    join(path, sizeof(path), fixture, "cardA/Roms/PS/escape.chd");
    assert(symlink(reference, path) == 0);
    write_file("cardA/Roms/PS/escape.m3u", "escape.chd\n");
    fails("PS/escape.m3u", NULL, "escapes its ROM source");
    char alias[JW_STORAGE_PATH_MAX];
    join(alias, sizeof(alias), fixture, "card-alias");
    assert(symlink(sources.sources[0].root, alias) == 0);
    join(reference, sizeof(reference), alias, "Roms/PS/escape.chd");
    write_file("cardA/Roms/PS/alias-escape.m3u", reference);
    fails("PS/alias-escape.m3u", NULL, "escapes its ROM source");
    join(path, sizeof(path), fixture, "cardA/Roms/PS/inside.cue");
    assert(symlink("discs/Disc 1.cue", path) == 0);
    write_file("cardA/Roms/PS/inside.m3u", "inside.cue\ndiscs/Disc 1.cue\n");
    content = inspect("PS/inside.m3u", NULL);
    /* The symlink resolves to the disc it names, so both entries are one file. */
    assert(content.file_count == 4 && !file_named(&content, "PS/discs/Disc 1.cue")->missing);
    jw_content_free(&content);
}

static void test_case_rules(void) {
    write_file("cardA/Roms/PS/MixedCase.chd", "first");
    char path[JW_STORAGE_PATH_MAX];
    join(path, sizeof(path), fixture, "cardA/Roms/PS/mixedcase.chd");
    struct stat st;
    bool insensitive = stat(path, &st) == 0;
    write_file("cardA/Roms/PS/case.m3u", "MixedCase.chd\nmixedcase.chd\n");
    jw_content content = inspect("PS/case.m3u", NULL);
    if (insensitive) {
        assert(content.file_count == 2);
        assert(!file_named(&content, "PS/MixedCase.chd")->missing);
        puts("content: case-insensitive filesystem spelling and dedup passed");
    } else {
        assert(content.file_count == 3);
        assert(file_named(&content, "PS/mixedcase.chd")->missing);
        jw_content_free(&content);
        write_file("cardA/Roms/PS/mixedcase.chd", "second");
        content = inspect("PS/case.m3u", NULL);
        assert(content.file_count == 3);
        assert(!file_named(&content, "PS/mixedcase.chd")->missing);
        assert(!file_named(&content, "PS/MixedCase.chd")->missing);
        puts("content: case-sensitive filesystem distinct names passed");
    }
    jw_content_free(&content);
}

static void test_hash_names(void) {
    /* RetroArch only treats '#' after .zip, .7z or .apk as an archive selector. */
    write_file("cardA/Roms/PS/Hash #1 (Disc 1).chd", "one");
    write_file("cardA/Roms/PS/set.7z", "seven");
    write_file("cardA/Roms/PS/hash.m3u",
               "#LABEL:Directive label\nHash #1 (Disc 1).chd|\n"
               "ARCHIVE.ZIP#Disc #2.cue|Upper case\nset.7z#Disc 3.cue\n"
               "#EXTINF:0,Kept\n#EXTINF:0,\nnot.zip.bak#x.chd\n");
    jw_content content = inspect("PS/hash.m3u", NULL);
    assert(content.file_count == 5);
    assert(!file_named(&content, "PS/Hash #1 (Disc 1).chd")->missing);
    assert(!file_named(&content, "PS/set.7z")->missing);
    assert(file_named(&content, "PS/not.zip.bak#x.chd")->missing);
    jw_content_free(&content);
}

static void test_error_location(void) {
    /* A malformed descriptor anywhere must be findable from the error alone. */
    write_file("cardA/Roms/PS/locate-outer.m3u", "locate-inner.cue\n");
    write_file("cardA/Roms/PS/locate-inner.cue", "REM no FILE records\n");
    jw_content content;
    char error[512];
    assert(jw_content_inspect(&sources, "primary", "PS/locate-outer.m3u", NULL,
                              &content, error, sizeof(error)) == -1);
    assert(strstr(error, "track records (in ") && strstr(error, "PS/locate-inner.cue)"));
    assert(!strstr(error, "locate-outer"));
    write_file("cardA/Roms/PS/locate-ref.m3u", "../../../outside.bin\n");
    assert(jw_content_inspect(&sources, "primary", "PS/locate-ref.m3u", NULL,
                              &content, error, sizeof(error)) == -1);
    assert(strstr(error, "outside the mounted ROM roots") && strstr(error, "PS/locate-ref.m3u)"));
}

static void test_limits(void) {
    char path[JW_STORAGE_PATH_MAX], text[1200];
    memset(text, 'a', 600);
    text[600] = '\0';
    strcat(text, ".chd\n");
    write_file("cardA/Roms/PS/long.m3u", text);
    /* The filesystem may reject an oversized component before the key check. */
    jw_content content;
    char error[512];
    assert(jw_content_inspect(&sources, "primary", "PS/long.m3u", NULL,
                              &content, error, sizeof(error)) == -1);
    assert(error[0]);
    for (int i = 0; i < 35; i++) {
        snprintf(path, sizeof(path), "cardA/Roms/PS/depth%d.m3u", i);
        snprintf(text, sizeof(text), "depth%d.m3u\n", i + 1);
        write_file(path, text);
    }
    fails("PS/depth0.m3u", NULL, "nested too deeply");

    join(path, sizeof(path), fixture, "cardA/Roms/PS/oversized.m3u");
    FILE *fp = fopen(path, "wb");
    assert(fp && fseek(fp, 4 * 1024 * 1024, SEEK_SET) == 0);
    assert(fputc('\n', fp) != EOF && fclose(fp) == 0);
    fails("PS/oversized.m3u", NULL, "size limit");
}

static void test_wide_directory(void) {
    /* A wide directory is name-indexed; a miss must still recover the on-disk spelling. */
    char name[128];
    directory("cardA/Roms/Many");
    directory("cardA/Roms/Many/wide");
    for (int i = 0; i < 64; i++) {
        snprintf(name, sizeof(name), "cardA/Roms/Many/wide/disc%02d.bin", i);
        write_file(name, "x");
    }
    write_file("cardA/Roms/Many/wide.m3u", "wide/disc07.bin\nwide/DISC08.BIN\nwide/disc99.bin\n");
    char path[JW_STORAGE_PATH_MAX];
    join(path, sizeof(path), fixture, "cardA/Roms/Many/wide/DISC08.BIN");
    struct stat st;
    bool insensitive = stat(path, &st) == 0;
    jw_content content = inspect("Many/wide.m3u", NULL);
    assert(content.file_count == 4);
    assert(!file_named(&content, "Many/wide/disc07.bin")->missing);
    assert(file_named(&content, insensitive ? "Many/wide/disc08.bin" : "Many/wide/DISC08.BIN")->missing == !insensitive);
    assert(file_named(&content, "Many/wide/disc99.bin")->missing);
    jw_content_free(&content);
}

int main(void) {
    char temporary[] = "/tmp/jw-content-XXXXXX";
    assert(mkdtemp(temporary));
    assert(realpath(temporary, fixture));
    directory("cardA"); directory("cardB");
    directory("cardA/Roms"); directory("cardB/Roms");
    sources.count = 2;
    strcpy(sources.sources[0].id, "primary");
    strcpy(sources.sources[1].id, "secondary_sd");
    for (int i = 0; i < 2; i++) {
        join(sources.sources[i].root, sizeof(sources.sources[i].root), fixture, i ? "cardB" : "cardA");
        join(sources.sources[i].roms_path, sizeof(sources.sources[i].roms_path),
             sources.sources[i].root, "Roms");
        sources.sources[i].available = true;
    }
    test_formats();
    test_cmd();
    test_missing_and_errors();
    test_sources_and_symlinks();
    test_case_rules();
    test_hash_names();
    test_error_location();
    test_limits();
    test_wide_directory();
    char command[JW_STORAGE_PATH_MAX + 16];
    snprintf(command, sizeof(command), "rm -rf '%s'", fixture);
    assert(system(command) == 0);
    puts("content tests passed");
    return 0;
}
