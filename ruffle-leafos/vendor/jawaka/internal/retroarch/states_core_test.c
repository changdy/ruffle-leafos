#include "internal/retroarch/states.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "states-core-test: %s\n", message);
    exit(1);
}

static void make_file(const char *path) {
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fail("could not create fixture file");
    }
    fputs("state", fp);
    fclose(fp);
}

static void expect_path(const char *label, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "states-core-test: %s got=%s want=%s\n",
                label, got, want);
        exit(1);
    }
}

static bool has_slot(const jw_ra_slot_info *slots, int count, int want) {
    for (int i = 0; i < count; i++) {
        if (slots[i].slot == want) return true;
    }
    return false;
}

static void test_slot_path_builder(void) {
    char out[PATH_MAX];
    if (!jw_ra_slot_state_path_for_core("/cards/b/States", "PCSX-ReARMed",
                                        "/cards/b/Roms/PS/Spyro the Dragon (USA).chd",
                                        99, false, out, sizeof(out))) {
        fail("slot path builder rejected a valid slot 99 request");
    }
    expect_path("slot99", out,
                "/cards/b/States/PCSX-ReARMed/Spyro the Dragon (USA).state99");
    if (!jw_ra_slot_state_path_for_core("/cards/b/States", "PCSX-ReARMed",
                                        "/cards/b/Roms/PS/Spyro the Dragon (USA).chd",
                                        99, true, out, sizeof(out))) {
        fail("slot path builder rejected a thumbnail request");
    }
    expect_path("slot99 thumb", out,
                "/cards/b/States/PCSX-ReARMed/Spyro the Dragon (USA).state99.png");
    /* Only the last extension goes, as RetroArch strips it. */
    if (!jw_ra_slot_state_path_for_core("/s", "Gambatte", "/r/Game.v1.zip", 0,
                                        false, out, sizeof(out))) {
        fail("slot path builder rejected slot 0");
    }
    expect_path("slot0", out, "/s/Gambatte/Game.v1.state");
    if (jw_ra_slot_state_path_for_core("/s", "../Other", "/r/Game.zip", 99,
                                       false, out, sizeof(out)) ||
        jw_ra_slot_state_path_for_core("/s", "", "/r/Game.zip", 99,
                                       false, out, sizeof(out)) ||
        jw_ra_slot_state_path_for_core("/s", "Gambatte", "", 99,
                                       false, out, sizeof(out))) {
        fail("slot path builder accepted an unsafe namespace or empty ROM");
    }
    char tiny[16];
    if (jw_ra_slot_state_path_for_core("/s", "Gambatte", "/r/Game.zip", 99,
                                       false, tiny, sizeof(tiny))) {
        fail("slot path builder accepted a truncated path");
    }
}

int main(void) {
    test_slot_path_builder();
    char root[] = "/tmp/jawaka-states-core.XXXXXX";
    int fd = mkstemp(root);
    if (fd < 0) {
        fail("mkstemp failed");
    }
    close(fd);
    if (unlink(root) != 0 || mkdir(root, 0700) != 0) {
        fail("could not create fixture root");
    }

    char mgba_dir[PATH_MAX];
    char gpsp_dir[PATH_MAX];
    char flat_state[PATH_MAX];
    char mgba_state[PATH_MAX];
    char gpsp_state[PATH_MAX];
    char gpsp_other[PATH_MAX];
    snprintf(mgba_dir, sizeof(mgba_dir), "%s/mGBA", root);
    snprintf(gpsp_dir, sizeof(gpsp_dir), "%s/gpSP", root);
    snprintf(flat_state, sizeof(flat_state), "%s/smoke.state99", root);
    snprintf(mgba_state, sizeof(mgba_state), "%s/smoke.state99", mgba_dir);
    snprintf(gpsp_state, sizeof(gpsp_state), "%s/smoke.state99", gpsp_dir);
    snprintf(gpsp_other, sizeof(gpsp_other), "%s/smoke.state1", gpsp_dir);

    if (mkdir(mgba_dir, 0700) != 0 || mkdir(gpsp_dir, 0700) != 0) {
        fail("could not create core fixture directories");
    }
    make_file(flat_state);
    make_file(mgba_state);
    make_file(gpsp_other);

    char core_states[PATH_MAX];
    if (!jw_ra_core_states_dir(root, "gpSP", core_states, sizeof(core_states))) {
        fail("gpSP state namespace was not resolved");
    }
    expect_path("gpSP namespace", core_states, gpsp_dir);
    if (jw_ra_core_states_dir(root, "../mGBA", core_states, sizeof(core_states)) ||
        jw_ra_core_states_dir(root, "nested/gpSP", core_states,
                              sizeof(core_states))) {
        fail("state namespace accepted traversal or a nested path");
    }

    /* The IGM passes this resolved directory to the generic slot enumerator.
       It must see gpSP slot 1 without surfacing mGBA/flat slot 99. */
    jw_ra_slot_info slots[8];
    int count = 0;
    if (!jw_ra_list_slots(gpsp_dir, "smoke.gba", slots, 8, &count) ||
        count != 1 || !has_slot(slots, count, 1) ||
        has_slot(slots, count, JW_RA_GAME_SWITCHER_STATE_SLOT)) {
        fail("core-scoped slot enumeration mixed namespaces");
    }

    int slot = 0;
    char resolved[PATH_MAX];
    if (!jw_ra_find_resume_state_for_core(root, "gpSP", "Roms/GBA/smoke.gba",
                                          JW_RA_GAME_SWITCHER_STATE_SLOT,
                                          &slot, resolved, sizeof(resolved))) {
        fail("gpSP fallback state was not found");
    }
    if (slot != 1) {
        fail("gpSP lookup crossed into a preferred slot from another namespace");
    }
    expect_path("gpSP fallback", resolved, gpsp_other);

    make_file(gpsp_state);
    if (!jw_ra_find_resume_state_for_core(root, "gpSP", "smoke.gba",
                                          JW_RA_GAME_SWITCHER_STATE_SLOT,
                                          &slot, resolved, sizeof(resolved)) ||
        slot != JW_RA_GAME_SWITCHER_STATE_SLOT) {
        fail("gpSP preferred state was not found");
    }
    expect_path("gpSP preferred", resolved, gpsp_state);

    if (!jw_ra_find_resume_state_for_core(root, "mGBA", "smoke.gba",
                                          JW_RA_GAME_SWITCHER_STATE_SLOT,
                                          &slot, resolved, sizeof(resolved))) {
        fail("mGBA state was not found after switching back");
    }
    expect_path("mGBA preferred", resolved, mgba_state);

    if (jw_ra_find_resume_state_for_core(root, "missing", "smoke.gba",
                                         JW_RA_GAME_SWITCHER_STATE_SLOT,
                                         &slot, resolved, sizeof(resolved)) ||
        jw_ra_find_resume_state_for_core(root, "../mGBA", "smoke.gba",
                                         JW_RA_GAME_SWITCHER_STATE_SLOT,
                                         &slot, resolved, sizeof(resolved)) ||
        jw_ra_find_resume_state_for_core(root, NULL, "smoke.gba",
                                         JW_RA_GAME_SWITCHER_STATE_SLOT,
                                         &slot, resolved, sizeof(resolved))) {
        fail("core-isolated lookup accepted an unsafe namespace");
    }

    unlink(gpsp_other);
    unlink(gpsp_state);
    unlink(mgba_state);
    unlink(flat_state);
    rmdir(gpsp_dir);
    rmdir(mgba_dir);
    rmdir(root);
    puts("PASS states-core-test");
    return 0;
}
