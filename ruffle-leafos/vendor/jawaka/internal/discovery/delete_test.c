#include "internal/discovery/delete.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char fixture[JW_STORAGE_PATH_MAX];
static jw_storage_source_list sources;
static char *ps_extensions[] = {"cue", "chd", "toc"};
static char *playlists[] = {"m3u"};
static char *gba_extensions[] = {"gba"};
static char *archives[] = {"zip", "7z"};
static char *pc98_extensions[] = {"cmd", "hdi"};
static jw_ra_system ps = {.id = "PS", .extensions = {ps_extensions, 3}, .playlist_extensions = {playlists, 1}};
static jw_ra_system gba = {.id = "GBA", .extensions = {gba_extensions, 1}, .archive_extensions = {archives, 2}};
static jw_ra_system pc98 = {.id = "PC98", .extensions = {pc98_extensions, 2}};

static void path(char *out, const char *relative) {
    int n = snprintf(out, JW_STORAGE_PATH_MAX, "%s/%s", fixture, relative);
    assert(n > 0 && n < JW_STORAGE_PATH_MAX);
}
static void directory(const char *relative) {
    char absolute[JW_STORAGE_PATH_MAX]; path(absolute, relative);
    assert(mkdir(absolute, 0700) == 0);
}
static void put(const char *relative, const char *data) {
    char absolute[JW_STORAGE_PATH_MAX]; path(absolute, relative);
    FILE *fp = fopen(absolute, "wb"); assert(fp);
    assert(fwrite(data, 1, strlen(data), fp) == strlen(data)); assert(!fclose(fp));
}
static bool exists(const char *relative) {
    char absolute[JW_STORAGE_PATH_MAX]; path(absolute, relative);
    return access(absolute, F_OK) == 0;
}
static void remove_file(const char *relative) {
    char absolute[JW_STORAGE_PATH_MAX]; path(absolute, relative); assert(!unlink(absolute));
}
static jw_delete_plan preview(const char *relative, jw_ra_system *system) {
    jw_delete_plan plan;
    char error[512];
    int rc = jw_delete_plan_build(&sources, system, "primary", relative, &plan, error, sizeof(error));
    if (rc) fprintf(stderr, "preview %s: %s\n", relative, error);
    assert(rc == 0 && strlen(plan.digest) == 16);
    return plan;
}
static void refused(const char *relative, jw_ra_system *system, const char *message) {
    jw_delete_plan plan;
    char error[512];
    assert(jw_delete_plan_build(&sources, system, "primary", relative, &plan, error, sizeof(error)) == -1);
    if (!strstr(error, message)) fprintf(stderr, "expected %s, got %s\n", message, error);
    assert(strstr(error, message)); assert(!plan.files && !plan.file_count);
}
static size_t slot(const jw_delete_plan *plan, const char *relative) {
    for (size_t i = 0; i < plan->file_count; i++) if (!strcmp(plan->files[i].rom_relpath, relative)) return i;
    assert(0); return 0;
}
static jw_delete_result execute(jw_delete_plan *plan) {
    jw_delete_result result; char error[512];
    int rc = jw_delete_execute(plan, NULL, NULL, &result, error, sizeof(error));
    if (rc) fprintf(stderr, "execute: %s\n", error);
    assert(!rc); return result;
}

static void test_eligibility(void) {
    const char *allowed[] = {"PS", "GBA", "PC98", "DOS", "AMIGA", "DC", "PCE", "SATURN", "SEGACD"};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        jw_ra_system s = {.id = (char *)allowed[i]}; assert(jw_delete_supported(&s));
    }
    const char *denied[] = {"ARCADE", "MAME", "MAME2003", "MAME2010", "NEOGEO", "NAOMI", "ATOMISWAVE", "PORTS"};
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++) {
        jw_ra_system s = {.id = (char *)denied[i]}; assert(!jw_delete_supported(&s));
    }
    assert(!jw_delete_supported(NULL));
    jw_ra_system s = ps; s.provider = "shared/Content.pak"; assert(!jw_delete_supported(&s));
    char *names[] = {"RPG_RT.ldb"};
    s = (jw_ra_system){.id = "EASYRPG", .file_names = {names, 1}}; assert(!jw_delete_supported(&s));
    put("A/Roms/GBA/Arcade.zip", "romset");
    jw_ra_system arcade = {.id = "ARCADE"};
    refused("GBA/Arcade.zip", &arcade, "isn't available");
    assert(exists("A/Roms/GBA/Arcade.zip")); remove_file("A/Roms/GBA/Arcade.zip");
}

static void test_single_and_digest(void) {
    put("A/Roms/GBA/Game.zip", "container");
    jw_delete_plan first = preview("GBA/Game.zip", &gba);
    assert(first.file_count == 1 && first.bytes == 9 && !first.writable_image);
    jw_delete_plan again = preview("GBA/Game.zip", &gba);
    assert(!strcmp(first.digest, again.digest));
    jw_delete_plan_free(&again);
    put("A/Roms/GBA/Game.zip", "a larger container");
    again = preview("GBA/Game.zip", &gba);
    assert(strcmp(first.digest, again.digest));
    jw_delete_plan_free(&first);
    jw_delete_result result = execute(&again);
    assert(result.removed_count == 1 && result.bytes == 18 && !exists("A/Roms/GBA/Game.zip"));
    assert(result.outcomes[0] == JW_DELETE_REMOVED);
    jw_delete_result_free(&result); jw_delete_plan_free(&again);
}

/* A multi-disc game is its playlist, the discs it lists and their tracks. Leaving
   any "(Disc N)" CUE behind would let the next scan regenerate the playlist. */
static void test_multi_disc_order(void) {
    put("A/Roms/PS/Game (Disc 1).cue", "FILE \"Game (Disc 1).bin\" BINARY\r\n");
    put("A/Roms/PS/Game (Disc 1).bin", "first");
    put("A/Roms/PS/sub/Game (Disc 2).cue", "FILE \"Game (Disc 2).bin\" BINARY\n");
    put("A/Roms/PS/sub/Game (Disc 2).bin", "second");
    put("A/Roms/PS/Game.m3u", "#EXTM3U\nGame (Disc 1).cue|One\nsub/Game (Disc 2).cue\n#SAVEDISK:Save Disk\n");
    jw_delete_plan plan = preview("PS/Game.m3u", &ps);
    assert(plan.file_count == 5 && !strcmp(plan.files[4].rom_relpath, "PS/Game.m3u"));
    /* Tracks go before the CUE that names them, and every CUE before the playlist. */
    assert(slot(&plan, "PS/Game (Disc 1).bin") < slot(&plan, "PS/Game (Disc 1).cue"));
    assert(slot(&plan, "PS/sub/Game (Disc 2).bin") < slot(&plan, "PS/sub/Game (Disc 2).cue"));
    jw_delete_result result = execute(&plan);
    assert(result.removed_count == 5 && !result.absent_count);
    const char *gone[] = {"A/Roms/PS/Game.m3u", "A/Roms/PS/Game (Disc 1).cue", "A/Roms/PS/Game (Disc 1).bin",
                          "A/Roms/PS/sub/Game (Disc 2).cue", "A/Roms/PS/sub/Game (Disc 2).bin"};
    for (size_t i = 0; i < sizeof(gone) / sizeof(gone[0]); i++) assert(!exists(gone[i]));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
}

typedef struct { int calls; int fail_at; char last[512]; } fault;
static int fail_guard(void *context, const jw_delete_file *file, char *error, size_t size) {
    fault *f = context; f->calls++; snprintf(f->last, sizeof(f->last), "%s", file->rom_relpath);
    if (f->calls == f->fail_at) { snprintf(error, size, "Injected I/O error"); return -1; }
    return 0;
}

static void test_partial_failure_and_retry(void) {
    put("A/Roms/PS/Track.bin", "payload"); put("A/Roms/PS/Disc.cue", "FILE Track.bin BINARY\r\n");
    put("A/Roms/PS/Game.m3u", "Disc.cue\r\nAbsent.cue\r\n");
    jw_delete_plan plan = preview("PS/Game.m3u", &ps);
    assert(plan.file_count == 4 && plan.files[slot(&plan, "PS/Absent.cue")].missing);
    assert(plan.bytes == strlen("payload") + strlen("FILE Track.bin BINARY\r\n") + strlen("Disc.cue\r\nAbsent.cue\r\n"));
    fault injection = {.fail_at = 2}; jw_delete_result result; char error[512];
    assert(jw_delete_execute(&plan, fail_guard, &injection, &result, error, sizeof(error)) == -1);
    assert(result.removed_count == 1 && result.failed_index == 1 && strstr(error, "Injected"));
    assert(!exists("A/Roms/PS/Track.bin") && exists("A/Roms/PS/Disc.cue") && exists("A/Roms/PS/Game.m3u"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    /* The launch file survived, so a fresh preview lists what is left. */
    plan = preview("PS/Game.m3u", &ps);
    assert(plan.files[slot(&plan, "PS/Track.bin")].missing);
    result = execute(&plan);
    assert(result.removed_count == 2 && result.absent_count == 2 && !exists("A/Roms/PS/Game.m3u"));
    assert(result.outcomes[slot(&plan, "PS/Track.bin")] == JW_DELETE_ABSENT);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
}

static void test_vanished_and_missing(void) {
    /* A file gone between preview and removal counts as absent, not removed. */
    put("A/Roms/PS/Solo.chd", "solo"); put("A/Roms/PS/Solo.m3u", "Solo.chd\n");
    jw_delete_plan plan = preview("PS/Solo.m3u", &ps);
    remove_file("A/Roms/PS/Solo.chd");
    jw_delete_result result = execute(&plan);
    assert(result.removed_count == 1 && result.absent_count == 1);
    assert(result.outcomes[slot(&plan, "PS/Solo.chd")] == JW_DELETE_ABSENT);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    /* A launch file that is already gone still lets the library row be reconciled. */
    plan = preview("PS/Gone.cue", &ps);
    assert(plan.file_count == 1 && plan.files[0].missing && !plan.bytes);
    result = execute(&plan); assert(result.absent_count == 1 && !result.removed_count);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
}

static void test_symlinks(void) {
    put("A/Roms/GBA/Game.gba", "game");
    char target[JW_STORAGE_PATH_MAX], link[JW_STORAGE_PATH_MAX];
    path(target, "A/Roms/GBA/Game.gba"); path(link, "A/Roms/GBA/Alias.gba");
    assert(!symlink(target, link));
    refused("GBA/Alias.gba", &gba, "symlinked");
    assert(exists("A/Roms/GBA/Game.gba"));
    remove_file("A/Roms/GBA/Alias.gba"); remove_file("A/Roms/GBA/Game.gba");
}

static void test_writable_image_and_cross_card(void) {
    put("A/Roms/PC98/Game.cmd", "np2kai disk.hdi --speed 2\n"); put("A/Roms/PC98/disk.hdi", "progress inside image");
    jw_delete_plan plan = preview("PC98/Game.cmd", &pc98);
    assert(plan.writable_image && plan.file_count == 2);
    jw_delete_result result = execute(&plan); assert(result.removed_count == 2);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    /* An absolute reference to the other mounted card is part of the game, and
       the plan names the card each file is on. */
    put("B/Roms/Other/Far.chd", "far");
    char reference[JW_STORAGE_PATH_MAX]; path(reference, "B/Roms/Other/Far.chd");
    put("A/Roms/PS/Far.m3u", reference);
    plan = preview("PS/Far.m3u", &ps);
    assert(plan.file_count == 2 && !strcmp(plan.files[slot(&plan, "Other/Far.chd")].source_id, "secondary_sd"));
    result = execute(&plan); assert(result.removed_count == 2 && !exists("B/Roms/Other/Far.chd"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    put("A/Roms/PS/Bad.cue", "FILE\n");
    refused("PS/Bad.cue", &ps, "FILE filename");
    remove_file("A/Roms/PS/Bad.cue");
}

static void test_hash_filenames(void) {
    /* RetroArch only splits archive.zip#member; this '#' is part of the filename. */
    put("A/Roms/PS/Game #1 (Disc 1).cue", "FILE \"Game #1 (Disc 1).bin\" BINARY\n");
    put("A/Roms/PS/Game #1 (Disc 1).bin", "payload");
    put("A/Roms/PS/Game #1.m3u", "Game #1 (Disc 1).cue\n");
    jw_delete_plan plan = preview("PS/Game #1.m3u", &ps);
    assert(plan.file_count == 3 && plan.bytes == strlen("payload") +
           strlen("FILE \"Game #1 (Disc 1).bin\" BINARY\n") + strlen("Game #1 (Disc 1).cue\n"));
    jw_delete_result result = execute(&plan);
    assert(result.removed_count == 3 && !exists("A/Roms/PS/Game #1 (Disc 1).bin"));
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
}

static void *test_deep_chain_worker(void *unused) {
    (void)unused;
    /* Match jawakad's worker stack at the reader's nesting bound. */
    char name[128], descriptor[512];
    for (int level = 0; level < 32; level++) {
        snprintf(name, sizeof(name), "A/Roms/PC98/deep%02d.cmd", level);
        if (level < 31) snprintf(descriptor, sizeof(descriptor), "np2kai deep%02d.cmd\n", level + 1);
        else {
            strcpy(descriptor, "np2kai ");
            for (int missing = 0; missing < 30; missing++) strcat(descriptor, "absent/");
            strcat(descriptor, "disk.hdi\n");
        }
        put(name, descriptor);
    }
    jw_delete_plan plan = preview("PC98/deep00.cmd", &pc98);
    assert(plan.file_count == 33 && !strcmp(plan.files[32].rom_relpath, "PC98/deep00.cmd"));
    jw_delete_result result = execute(&plan);
    assert(result.removed_count == 32 && result.absent_count == 1);
    jw_delete_result_free(&result); jw_delete_plan_free(&plan);
    return NULL;
}

static void test_deep_chain(void) {
    pthread_attr_t attributes;
    pthread_t worker;
    assert(!pthread_attr_init(&attributes));
    assert(!pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024));
    assert(!pthread_create(&worker, &attributes, test_deep_chain_worker, NULL));
    assert(!pthread_attr_destroy(&attributes));
    assert(!pthread_join(worker, NULL));
}

int main(void) {
    char temporary[] = "/tmp/jw-delete-XXXXXX"; assert(mkdtemp(temporary)); assert(realpath(temporary, fixture));
    directory("A"); directory("B"); directory("A/Roms"); directory("B/Roms");
    directory("A/Roms/PS"); directory("A/Roms/PS/sub");
    directory("A/Roms/GBA"); directory("A/Roms/PC98"); directory("B/Roms/Other");
    sources.count = 2;
    for (int i = 0; i < 2; i++) {
        jw_storage_source *s = &sources.sources[i]; strcpy(s->id, i ? "secondary_sd" : "primary");
        path(s->root, i ? "B" : "A"); strcpy(s->root_abs, s->root);
        path(s->roms_path, i ? "B/Roms" : "A/Roms"); s->available = true;
    }
    test_eligibility(); test_single_and_digest(); test_multi_disc_order();
    test_partial_failure_and_retry(); test_vanished_and_missing(); test_symlinks();
    test_writable_image_and_cross_card(); test_hash_filenames(); test_deep_chain();
    char command[JW_STORAGE_PATH_MAX + 32]; snprintf(command, sizeof(command), "rm -rf '%s'", fixture);
    assert(system(command) == 0); puts("delete engine tests passed"); return 0;
}
