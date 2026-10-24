#define _POSIX_C_SOURCE 200809L

#include "internal/launcher/system_names.h"
#include "internal/db/db.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void expect_name(const char *db_path, const jw_ra_catalog *catalog,
                         const char *id, const char *want) {
    char name[64];
    jw_system_display_name(db_path, catalog, id, name, sizeof(name));
    if (strcmp(name, want) != 0) {
        fprintf(stderr, "system-names-test: %s: got '%s', want '%s'\n",
                id ? id : "(null)", name, want);
        exit(1);
    }
}

int main(void) {
    char *patterns[] = {"O2", "O2EM", "ODYSSEY2"};
    jw_ra_system systems[] = {
        {.id = "O2", .name = "Odyssey 2", .provider = "mac/O2EM.pak",
         .patterns = {.items = patterns, .count = 3}},
        {.id = "NES", .name = "FC"},
        {.id = "SCUMMVM", .name = "ScummVM", .provider = "mac/ScummVM.pak"},
        {.id = "SFC_JP", .name = "Custom Super Famicom", .provider = "mac/Custom.pak"},
        {.id = "OTHER", .name = "Another Console"},
        {.id = "EMPTY", .name = ""},
        {.id = "UNNAMED"},
    };
    jw_ra_catalog catalog = {
        .systems = systems, .system_count = sizeof(systems) / sizeof(systems[0])
    };

    expect_name(NULL, &catalog, "O2", "Odyssey 2");
    expect_name(NULL, &catalog, "o2em", "Odyssey 2");
    expect_name(NULL, &catalog, "SCUMMVM", "ScummVM");
    expect_name(NULL, &catalog, "NES", "Nintendo Entertainment System");
    expect_name(NULL, &catalog, "SFC_JP", "Custom Super Famicom");
    expect_name(NULL, &catalog, "OTHER", "Another Console");
    expect_name(NULL, &catalog, "EMPTY", "EMPTY");
    expect_name(NULL, &catalog, "UNNAMED", "UNNAMED");
    expect_name(NULL, &catalog, "UNKNOWN", "UNKNOWN");
    expect_name(NULL, NULL, "O2", "O2");
    expect_name(NULL, NULL, "nes", "Nintendo Entertainment System");
    expect_name(NULL, &catalog, NULL, "");
    expect_name(NULL, &catalog, "", "");

    char db_path[] = "/tmp/jw-system-names-XXXXXX";
    int fd = mkstemp(db_path);
    assert(fd >= 0);
    close(fd);
    assert(jw_db_set_system_setting(db_path, "O2",
                                    JW_CONTENT_SETTING_DISPLAY_NAME, "My Odyssey") == 0);
    expect_name(db_path, &catalog, "O2", "My Odyssey");
    expect_name(db_path, NULL, "O2", "My Odyssey");
    assert(jw_db_set_system_setting(db_path, "NES",
                                    JW_CONTENT_SETTING_DISPLAY_NAME, "My NES") == 0);
    expect_name(db_path, &catalog, "NES", "My NES");
    assert(jw_db_delete_system_setting(db_path, "O2",
                                       JW_CONTENT_SETTING_DISPLAY_NAME) == 0);
    expect_name(db_path, &catalog, "O2", "Odyssey 2");
    char small[4];
    jw_system_display_name(NULL, &catalog, "O2", small, sizeof(small));
    assert(strcmp(small, "Ody") == 0);
    jw_system_display_name(NULL, &catalog, "O2", NULL, 0);
    unlink(db_path);
    puts("system-names-test: ok");
    return 0;
}
