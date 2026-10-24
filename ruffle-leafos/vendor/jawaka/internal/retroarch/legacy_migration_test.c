#define _POSIX_C_SOURCE 200809L

#include "internal/retroarch/legacy_migration.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int make_dir(const char *path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int write_file(const char *path, const char *text, time_t mtime) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t len = strlen(text);
    int rc = fwrite(text, 1, len, fp) == len && fclose(fp) == 0 ? 0 : -1;
    struct timespec times[2] = { { mtime, 0 }, { mtime, 0 } };
    if (rc == 0 && utimensat(AT_FDCWD, path, times, 0) != 0) rc = -1;
    return rc;
}

static int read_equals(const char *path, const char *expected) {
    char buf[64] = {0};
    FILE *fp = fopen(path, "rb");
    if (!fp) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1u, fp);
    fclose(fp);
    return n == strlen(expected) && strcmp(buf, expected) == 0;
}

static int fail(const char *message) {
    fprintf(stderr, "legacy-migration-test: %s\n", message);
    return 1;
}

int main(void) {
    char root[] = "/tmp/jw-legacy-migration-XXXXXX";
    int root_fd = mkstemp(root);
    if (root_fd < 0) return fail("mkstemp failed");
    close(root_fd);
    if (unlink(root) != 0 || mkdir(root, 0700) != 0) {
        return fail("fixture root failed");
    }

    char saves[PATH_MAX], states[PATH_MAX];
    snprintf(saves, sizeof(saves), "%s/Saves", root);
    snprintf(states, sizeof(states), "%s/States", root);
    if (make_dir(saves) != 0 || make_dir(states) != 0) return fail("mkdir failed");

    const time_t old_mtime = 946684800;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Purple.srm", saves);
    if (write_file(path, "battery", old_mtime) != 0) return fail("write srm failed");
    snprintf(path, sizeof(path), "%s/Purple.rtc", saves);
    if (write_file(path, "rtc", old_mtime) != 0) return fail("write rtc failed");
    const char *extra_save_suffixes[] = { ".sav", ".eep", ".fla", ".sra", ".mcd" };
    for (size_t i = 0; i < sizeof(extra_save_suffixes) / sizeof(extra_save_suffixes[0]); i++) {
        snprintf(path, sizeof(path), "%s/Purple%s", saves, extra_save_suffixes[i]);
        if (write_file(path, extra_save_suffixes[i], old_mtime) != 0) {
            return fail("write save companion failed");
        }
    }
    snprintf(path, sizeof(path), "%s/Purple.state", states);
    if (write_file(path, "slot0", old_mtime) != 0) return fail("write state failed");
    snprintf(path, sizeof(path), "%s/Purple.state1", states);
    if (write_file(path, "slot1", old_mtime) != 0) return fail("write numbered state failed");
    snprintf(path, sizeof(path), "%s/Purple.state.auto", states);
    if (write_file(path, "auto", old_mtime) != 0) return fail("write auto state failed");
    snprintf(path, sizeof(path), "%s/Purple.state.png", states);
    if (write_file(path, "thumb0", old_mtime) != 0) return fail("write slot0 thumb failed");
    snprintf(path, sizeof(path), "%s/Purple.state99.png", states);
    if (write_file(path, "thumb", old_mtime) != 0) return fail("write thumb failed");
    snprintf(path, sizeof(path), "%s/Purple.state.auto.png", states);
    if (write_file(path, "autothumb", old_mtime) != 0) return fail("write auto thumb failed");
    snprintf(path, sizeof(path), "%s/Purple.state.bad", states);
    if (write_file(path, "ignore", old_mtime) != 0) return fail("write ignored failed");

    if (!jw_ra_legacy_flat_files_need_recovery(root,
                                               "/Roms/GBA/Purple.gba",
                                               "mGBA")) {
        return fail("preflight missed pending legacy files");
    }

    jw_ra_legacy_migration_report report;
    if (jw_ra_migrate_legacy_flat_files(root, "/Roms/GBA/Purple.gba",
                                        "gpsp", "gpSP", "mgba", false,
                                        &report) !=
        JW_RA_LEGACY_MIGRATION_NOT_APPLICABLE) {
        return fail("alternate core accepted legacy files");
    }

    char mgba_states[PATH_MAX], mgba_saves[PATH_MAX];
    snprintf(mgba_states, sizeof(mgba_states), "%s/mGBA", states);
    snprintf(mgba_saves, sizeof(mgba_saves), "%s/mGBA", saves);
    if (make_dir(mgba_saves) != 0 || make_dir(mgba_states) != 0) {
        return fail("destination mkdir failed");
    }
    snprintf(path, sizeof(path), "%s/Purple.state", mgba_states);
    if (write_file(path, "destination", old_mtime + 1) != 0) {
        return fail("write destination failed");
    }

    if (jw_ra_migrate_legacy_flat_files(root, "/Roms/GBA/Purple.gba",
                                        "mgba", "mGBA", "mgba", false,
                                        &report) != JW_RA_LEGACY_MIGRATION_COPIED ||
        report.copied != 12 || report.existing != 1 || report.failed != 0) {
        return fail("unexpected migration counts");
    }
    snprintf(path, sizeof(path), "%s/Purple.srm", mgba_saves);
    struct stat copied;
    if (!read_equals(path, "battery") || stat(path, &copied) != 0 ||
        copied.st_mtime != old_mtime) {
        return fail("save bytes or mtime not preserved");
    }
    snprintf(path, sizeof(path), "%s/Purple.state", mgba_states);
    if (!read_equals(path, "destination")) return fail("destination overwritten");
    snprintf(path, sizeof(path), "%s/Purple.srm", saves);
    if (!read_equals(path, "battery")) return fail("flat source removed");

    if (jw_ra_migrate_legacy_flat_files(root, "/Roms/GBA/Purple.gba",
                                        "mgba", "mGBA", "mgba", false,
                                        &report) != JW_RA_LEGACY_MIGRATION_NO_FILES ||
        report.copied != 0 || report.existing != 0) {
        return fail("migration is not idempotent");
    }
    if (jw_ra_legacy_flat_files_need_recovery(root,
                                              "/Roms/GBA/Purple.gba",
                                              "mGBA")) {
        return fail("preflight did not short-circuit existing destinations");
    }

    snprintf(path, sizeof(path), "%s/Ambiguous.srm", saves);
    if (write_file(path, "ambiguous", old_mtime) != 0) {
        return fail("write ambiguity fixture failed");
    }
    if (jw_ra_migrate_legacy_flat_files(root, "/Roms/GBA/Ambiguous.gba",
                                        "mgba", "mGBA", "mgba", true,
                                        &report) != JW_RA_LEGACY_MIGRATION_AMBIGUOUS) {
        return fail("ambiguity did not fail closed");
    }
    snprintf(path, sizeof(path), "%s/Ambiguous.srm", mgba_saves);
    if (access(path, F_OK) == 0) return fail("ambiguous save was copied");
    if (jw_ra_migrate_legacy_flat_files(root, "/Roms/GBA/Purple.gba",
                                        "mgba", "../mGBA", "mgba", false,
                                        &report) !=
        JW_RA_LEGACY_MIGRATION_NOT_APPLICABLE) {
        return fail("unsafe folder accepted");
    }

    char secondary[PATH_MAX], secondary_saves[PATH_MAX], secondary_states[PATH_MAX];
    snprintf(secondary, sizeof(secondary), "%s/secondary", root);
    snprintf(secondary_saves, sizeof(secondary_saves), "%s/Saves", secondary);
    snprintf(secondary_states, sizeof(secondary_states), "%s/States", secondary);
    if (make_dir(secondary) != 0 || make_dir(secondary_saves) != 0 ||
        make_dir(secondary_states) != 0) {
        return fail("secondary fixture mkdir failed");
    }
    snprintf(path, sizeof(path), "%s/Locality.srm", saves);
    if (write_file(path, "primary", old_mtime) != 0) {
        return fail("primary locality fixture failed");
    }
    if (jw_ra_migrate_legacy_flat_files(secondary,
                                        "/Roms/GBA/Locality.gba",
                                        "mgba", "mGBA", "mgba", false,
                                        &report) != JW_RA_LEGACY_MIGRATION_NO_FILES) {
        return fail("secondary recovery read primary source");
    }
    snprintf(path, sizeof(path), "%s/mGBA/Locality.srm", secondary_saves);
    if (access(path, F_OK) == 0) return fail("primary save leaked to secondary");
    snprintf(path, sizeof(path), "%s/Locality.srm", secondary_saves);
    if (write_file(path, "secondary", old_mtime) != 0) {
        return fail("secondary locality fixture failed");
    }
    if (jw_ra_migrate_legacy_flat_files(secondary,
                                        "/Roms/GBA/Locality.gba",
                                        "mgba", "mGBA", "mgba", false,
                                        &report) != JW_RA_LEGACY_MIGRATION_COPIED) {
        return fail("secondary save was not recovered");
    }
    snprintf(path, sizeof(path), "%s/mGBA/Locality.srm", secondary_saves);
    if (!read_equals(path, "secondary")) return fail("secondary destination wrong");
    snprintf(path, sizeof(path), "%s/mGBA/Locality.srm", saves);
    if (access(path, F_OK) == 0) return fail("secondary save leaked to primary");

    snprintf(path, sizeof(path), "%s/Failure.srm", saves);
    if (mkdir(path, 0755) != 0) return fail("failure fixture mkdir failed");
    if (jw_ra_migrate_legacy_flat_files(root, "/Roms/GBA/Failure.gba",
                                        "mgba", "mGBA", "mgba", false,
                                        &report) != JW_RA_LEGACY_MIGRATION_FAILED ||
        !strstr(report.detail, "Failure.srm") ||
        !strstr(report.detail, "not a regular file")) {
        return fail("copy failure lacks actionable detail");
    }

    printf("legacy-migration-test: ok\n");
    return 0;
}
