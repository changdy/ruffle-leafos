/* standalone-ra-account-v1 launch authorization tests: the exact target and
   capability checks that decide whether a standalone child receives the
   account snapshot. Covers the bundled-Flycast launcher/marker pair, the
   DSperate provider/core/path/manifest/record matrix, the separate Flycast
   proxy route record, the spoof cases that must every time fall to refusal,
   and the child environment the producer builds from a stored account
   (never CONFIGURED without a revision). */

#include "internal/launcher/ra_account.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void expect(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "ra-account-launch-test: %s\n", what);
        failures++;
    }
}

static char root[PATH_MAX];

static void write_file(const char *rel, const char *content) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    char *slash = strrchr(path, '/');
    *slash = '\0';
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    if (system(cmd) != 0) exit(1);
    *slash = '/';
    FILE *f = fopen(path, "wb");
    if (!f) exit(1);
    fputs(content, f);
    fclose(f);
}

/* Byte-exact write: record fixtures may contain NUL. */
static void write_bytes(const char *rel, const char *data, size_t len) {
    write_file(rel, "");
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(data, 1, len, f) != len || fclose(f) != 0) exit(1);
}

/* A capability record must be a regular file whose full byte length is the
   id, or the id plus one trailing '\n'. Replays every shape against one
   record path and restores the valid record afterwards. */
typedef bool (*authorized_fn)(void);

static void check_record_shapes(const char *rel, const char *id,
                                authorized_fn authorized, const char *label) {
    char big[200];
    size_t id_len = strlen(id);
    memset(big, 'x', sizeof(big));
    memcpy(big, id, id_len);
    big[id_len] = '\n';
    char nul_junk[64], nul_only[64], nl_nul[64], junk[64], nl_junk[64];
    snprintf(junk, sizeof(junk), "%sjunk", id);
    snprintf(nl_junk, sizeof(nl_junk), "%s\njunk", id);
    memcpy(nul_junk, id, id_len); memcpy(nul_junk + id_len, "\0junk", 5);
    memcpy(nul_only, id, id_len); nul_only[id_len] = '\0';
    memcpy(nl_nul, id, id_len); memcpy(nl_nul + id_len, "\n\0", 2);
    const struct { const char *data; size_t len; bool ok; const char *what; } cases[] = {
        { id, id_len, true, "exact id" },
        { big, id_len + 1, true, "id + newline" },
        { nul_junk, id_len + 5, false, "id + NUL + junk" },
        { nul_only, id_len + 1, false, "id + NUL" },
        { nl_nul, id_len + 2, false, "id + newline + NUL" },
        { junk, strlen(junk), false, "id + trailing garbage" },
        { nl_junk, strlen(nl_junk), false, "id + newline + garbage" },
        { "", 0, false, "empty" },
        { big, sizeof(big), false, "id + newline + 175 bytes" },
        { id, id_len - 1, false, "truncated id" },
    };
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char what[160];
        write_bytes(rel, cases[i].data, cases[i].len);
        snprintf(what, sizeof(what), "%s record '%s' %s", label, cases[i].what,
                 cases[i].ok ? "authorized" : "denied");
        expect(authorized() == cases[i].ok, what);
    }
    /* A directory with the record's name is not a record. */
    unlink(path);
    mkdir(path, 0755);
    char what[160];
    snprintf(what, sizeof(what), "%s record as a directory denied", label);
    expect(!authorized(), what);
    rmdir(path);
    write_bytes(rel, big, id_len + 1);
}

/* mlp1/DSperate.pak lives under an apps root; run.sh must be executable. */
static void make_dsperate_pak(const char *pak_json) {
    write_file("apps/mlp1/DSperate.pak/pak.json", pak_json);
    char run_sh[PATH_MAX];
    snprintf(run_sh, sizeof(run_sh), "%s/%s", root,
             "apps/mlp1/DSperate.pak/scripts/run.sh");
    write_file("apps/mlp1/DSperate.pak/scripts/run.sh", "#!/bin/sh\n");
    chmod(run_sh, 0755);
    /* DSperate-pak ships pak/ra-account-v1 at the installed pak root. */
    write_file("apps/mlp1/DSperate.pak/ra-account-v1",
               "standalone-ra-account-v1\n");
}

static const jw_standalone_policy FLYCAST_RELEASE = {
    .provider_bound = false, .release = JW_STANDALONE_RELEASE_FLYCAST,
};
static const jw_standalone_policy PROVIDER_BOUND = {
    .provider_bound = true, .release = JW_STANDALONE_RELEASE_NONE,
};
static const jw_standalone_policy DRASTIC_RELEASE = {
    .provider_bound = false, .release = JW_STANDALONE_RELEASE_DRASTIC,
};

/* Helpers naming the resolved pieces the way the daemon resolves them. */
static void platform_dir(char *out, size_t size) {
    snprintf(out, size, "%s/platform", root);
}
static void flycast_launcher(char *out, size_t size) {
    snprintf(out, size, "%s/platform/emulators/flycast/launch.sh", root);
}
static void dsperate_launcher(char *out, size_t size) {
    snprintf(out, size, "%s/apps/mlp1/DSperate.pak/scripts/run.sh", root);
}

static bool flycast_authorized(void) {
    char platform[PATH_MAX], launcher[PATH_MAX];
    platform_dir(platform, sizeof(platform));
    flycast_launcher(launcher, sizeof(launcher));
    return jw_ra_account_target_authorized(launcher, "flycast_standalone",
                                           &FLYCAST_RELEASE, NULL, platform);
}

static bool dsperate_authorized(void) {
    char platform[PATH_MAX], launcher[PATH_MAX];
    platform_dir(platform, sizeof(platform));
    dsperate_launcher(launcher, sizeof(launcher));
    return jw_ra_account_target_authorized(launcher, "dsperate",
                                           &PROVIDER_BOUND,
                                           "mlp1/DSperate.pak", platform);
}

static void test_flycast(void) {
    char platform[PATH_MAX], launcher[PATH_MAX];
    platform_dir(platform, sizeof(platform));
    flycast_launcher(launcher, sizeof(launcher));

    write_file("platform/emulators/flycast/launch.sh", "#!/bin/sh\n");
    write_file("platform/emulators/flycast/ra-account-v1",
               "standalone-ra-account-v1\n");

    expect(jw_ra_account_target_authorized(launcher, "flycast_standalone",
                                           &FLYCAST_RELEASE, NULL, platform),
           "bundled flycast: exact launcher + capability record authorized");

    /* The capability record is required: a filename match alone grants
       nothing. */
    expect(!jw_ra_account_target_authorized(launcher, "flycast_standalone",
                                            &FLYCAST_RELEASE, NULL,
                                            "/nonexistent-platform"),
           "bundled flycast: wrong platform dir denied");

    /* Spoofed provider naming itself after the release: provider-bound cores
       never take the release branch, whatever the files say. */
    expect(!jw_ra_account_target_authorized(launcher, "flycast",
                                            &PROVIDER_BOUND,
                                            "mlp1/not-a-release.pak", platform),
           "provider-bound flycast-named core denied");

    /* A same-named launcher outside the release payload. */
    char other_launcher[PATH_MAX];
    snprintf(other_launcher, sizeof(other_launcher),
             "%s/apps/mlp1/Flycast.pak/launch.sh", root);
    write_file("apps/mlp1/Flycast.pak/launch.sh", "#!/bin/sh\n");
    write_file("apps/mlp1/Flycast.pak/ra-account-v1",
               "standalone-ra-account-v1\n");
    expect(!jw_ra_account_target_authorized(other_launcher, "flycast",
                                            &FLYCAST_RELEASE, NULL, platform),
           "flycast identity outside the release-owned launcher denied");

    /* Wrong marker content. */
    write_file("platform/emulators/flycast/ra-account-v1",
               "standalone-ra-account-v0\n");
    expect(!jw_ra_account_target_authorized(launcher, "flycast_standalone",
                                            &FLYCAST_RELEASE, NULL, platform),
           "stale capability record denied");
    write_file("platform/emulators/flycast/ra-account-v1",
               "standalone-ra-account-v1\n");
    check_record_shapes("platform/emulators/flycast/ra-account-v1",
                        "standalone-ra-account-v1", flycast_authorized,
                        "flycast ra-account-v1");

    /* Other release standalones never qualify for the Flycast branch. */
    expect(!jw_ra_account_target_authorized(launcher, "drastic",
                                            &DRASTIC_RELEASE, NULL, platform),
           "drastic identity on the flycast path denied");
}

static void test_dsperate(void) {
    char platform[PATH_MAX], launcher[PATH_MAX];
    platform_dir(platform, sizeof(platform));
    dsperate_launcher(launcher, sizeof(launcher));

    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"name\": \"DSperate\","
        "  \"platform\": \"mlp1\", \"pak_version\": \"2.1.1\","
        "  \"min_leaf_version\": \"0.12.0\" }\n");

    expect(jw_ra_account_target_authorized(launcher, "dsperate",
                                           &PROVIDER_BOUND,
                                           "mlp1/DSperate.pak", platform),
           "validated dsperate pak authorized");

    /* The published 2.0.0 build cannot consume the handoff. */
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.0.0\" }\n");
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "dsperate 2.0.0 denied");
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1\" }\n");

    /* A malformed version is not a capability. */
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1+leaf.1\" }\n");
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "unparseable dsperate version denied");
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1\" }\n");

    /* A pak claiming DSperate's identity from the wrong provider path, or the
       right pak with the wrong store id, is denied. */
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/Impostor.pak", platform),
           "wrong provider denied");
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.impostor\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1\" }\n");
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "wrong manifest id denied");
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1\" }\n");

    /* Right provider, wrong core id or wrong pak-relative launcher. */
    expect(!jw_ra_account_target_authorized(launcher, "scummvm",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "wrong core id denied");
    char wrong_launcher[PATH_MAX];
    snprintf(wrong_launcher, sizeof(wrong_launcher),
             "%s/apps/mlp1/DSperate.pak/launch.sh", root);
    expect(!jw_ra_account_target_authorized(wrong_launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "non-run.sh launcher denied");

    /* A release-recognized path-core (never provider-bound in the real
       catalog) cannot reach the provider branch. */
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &FLYCAST_RELEASE,
                                            "mlp1/DSperate.pak", platform),
           "release policy cannot take the provider branch");

    /* The shipped capability record is required alongside the version. */
    char record[PATH_MAX];
    snprintf(record, sizeof(record), "%s/apps/mlp1/DSperate.pak/ra-account-v1",
             root);
    unlink(record);
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "dsperate 2.1.1 without the ra-account-v1 record denied");
    static const struct { const char *content; bool ok; } records[] = {
        { "standalone-ra-account-v1", true },
        { "standalone-ra-account-v1\n", true },
        { "standalone-ra-account-v1\n\n", false },
        { "standalone-ra-account-v1\r\n", false },
        { " standalone-ra-account-v1\n", false },
        { "standalone-ra-account-v2\n", false },
        { "", false },
    };
    for (size_t i = 0; i < sizeof(records) / sizeof(records[0]); i++) {
        char what[128];
        write_file("apps/mlp1/DSperate.pak/ra-account-v1", records[i].content);
        snprintf(what, sizeof(what), "dsperate record case %zu %s", i,
                 records[i].ok ? "authorized" : "denied");
        expect(jw_ra_account_target_authorized(launcher, "dsperate",
                                               &PROVIDER_BOUND,
                                               "mlp1/DSperate.pak", platform) ==
                   records[i].ok,
               what);
    }
    write_file("apps/mlp1/DSperate.pak/ra-account-v1",
               "standalone-ra-account-v1\n");
    check_record_shapes("apps/mlp1/DSperate.pak/ra-account-v1",
                        "standalone-ra-account-v1", dsperate_authorized,
                        "dsperate ra-account-v1");
    /* The record does not stand in for the version: 2.0.0 with it is still
       refused. */
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.0.0\" }\n");
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "dsperate 2.0.0 with the record denied");
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1\" }\n");

    /* Missing manifest. */
    char manifest[PATH_MAX];
    snprintf(manifest, sizeof(manifest), "%s/apps/mlp1/DSperate.pak/pak.json",
             root);
    char saved[PATH_MAX];
    snprintf(saved, sizeof(saved), "%s.pakjson-saved", manifest);
    rename(manifest, saved);
    expect(!jw_ra_account_target_authorized(launcher, "dsperate",
                                            &PROVIDER_BOUND,
                                            "mlp1/DSperate.pak", platform),
           "missing manifest denied");
    rename(saved, manifest);
}

/* UMRK_FLYCAST_RA_ROUTE authorization (proxy plan P2): the bundled Flycast
   target only, and only when its payload carries both the account record and
   the separate route record. */
static bool flycast_route_authorized(void) {
    char platform[PATH_MAX], launcher[PATH_MAX];
    platform_dir(platform, sizeof(platform));
    flycast_launcher(launcher, sizeof(launcher));
    return jw_flycast_ra_route_target_authorized(launcher, "flycast_standalone",
                                                 &FLYCAST_RELEASE, NULL,
                                                 platform);
}

static void test_flycast_route(void) {
    char platform[PATH_MAX], launcher[PATH_MAX], other[PATH_MAX];
    platform_dir(platform, sizeof(platform));
    flycast_launcher(launcher, sizeof(launcher));
    snprintf(other, sizeof(other), "%s/apps/mlp1/Flycast.pak/launch.sh", root);
    char route_record[PATH_MAX], account_record[PATH_MAX], saved[PATH_MAX];
    snprintf(route_record, sizeof(route_record),
             "%s/platform/emulators/flycast/ra-route-v1", root);
    snprintf(account_record, sizeof(account_record),
             "%s/platform/emulators/flycast/ra-account-v1", root);

    write_file("platform/emulators/flycast/launch.sh", "#!/bin/sh\n");
    write_file("platform/emulators/flycast/ra-account-v1",
               "standalone-ra-account-v1\n");

    /* An account-capable build without the route record keeps its native
       path: account import alone is not routing support. */
    expect(!jw_flycast_ra_route_target_authorized(launcher,
                                                  "flycast_standalone",
                                                  &FLYCAST_RELEASE, NULL,
                                                  platform),
           "route: account-only flycast build denied");

    write_file("platform/emulators/flycast/ra-route-v1",
               "umrk-flycast-ra-route-v1\n");
    expect(jw_flycast_ra_route_target_authorized(launcher,
                                                 "flycast_standalone",
                                                 &FLYCAST_RELEASE, NULL,
                                                 platform),
           "route: bundled flycast with both records authorized");
    write_file("platform/emulators/flycast/ra-route-v1",
               "umrk-flycast-ra-route-v1");
    expect(jw_flycast_ra_route_target_authorized(launcher,
                                                 "flycast_standalone",
                                                 &FLYCAST_RELEASE, NULL,
                                                 platform),
           "route: record without trailing newline authorized");

    /* The route record never stands in for the account record. */
    snprintf(saved, sizeof(saved), "%s.saved", account_record);
    rename(account_record, saved);
    expect(!jw_flycast_ra_route_target_authorized(launcher,
                                                  "flycast_standalone",
                                                  &FLYCAST_RELEASE, NULL,
                                                  platform),
           "route: missing account record denied");
    rename(saved, account_record);

    /* Stale, padded or foreign record content. */
    static const char *const bad[] = {
        "umrk-flycast-ra-route-v0\n", "umrk-flycast-ra-route-v1\n\n",
        " umrk-flycast-ra-route-v1\n", "standalone-ra-account-v1\n", "",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        write_file("platform/emulators/flycast/ra-route-v1", bad[i]);
        expect(!jw_flycast_ra_route_target_authorized(launcher,
                                                      "flycast_standalone",
                                                      &FLYCAST_RELEASE, NULL,
                                                      platform),
               "route: malformed route record denied");
    }
    write_file("platform/emulators/flycast/ra-route-v1",
               "umrk-flycast-ra-route-v1\n");
    check_record_shapes("platform/emulators/flycast/ra-route-v1",
                        JW_FLYCAST_RA_ROUTE_CAPABILITY_ID, flycast_route_authorized,
                        "flycast ra-route-v1");

    /* A sideloaded launcher named like Flycast, carrying both records, and a
       provider-bound core claiming the release identity. */
    write_file("apps/mlp1/Flycast.pak/launch.sh", "#!/bin/sh\n");
    write_file("apps/mlp1/Flycast.pak/ra-account-v1",
               "standalone-ra-account-v1\n");
    write_file("apps/mlp1/Flycast.pak/ra-route-v1",
               "umrk-flycast-ra-route-v1\n");
    expect(!jw_flycast_ra_route_target_authorized(other, "flycast",
                                                  &FLYCAST_RELEASE, NULL,
                                                  platform),
           "route: flycast launcher outside the release payload denied");
    expect(!jw_flycast_ra_route_target_authorized(launcher, "flycast",
                                                  &PROVIDER_BOUND,
                                                  "mlp1/Flycast.pak",
                                                  platform),
           "route: provider-bound flycast-named core denied");
    expect(!jw_flycast_ra_route_target_authorized(launcher, "drastic",
                                                  &DRASTIC_RELEASE, NULL,
                                                  platform),
           "route: other release standalone denied");

    /* DSperate is account-authorized and must still never get the route. */
    char dsperate[PATH_MAX];
    dsperate_launcher(dsperate, sizeof(dsperate));
    make_dsperate_pak(
        "{ \"id\": \"org.umrk.dsperate\", \"platform\": \"mlp1\","
        "  \"pak_version\": \"2.1.1\" }\n");
    write_file("apps/mlp1/DSperate.pak/ra-route-v1",
               "umrk-flycast-ra-route-v1\n");
    expect(jw_ra_account_target_authorized(dsperate, "dsperate",
                                           &PROVIDER_BOUND,
                                           "mlp1/DSperate.pak", platform),
           "route: dsperate fixture is account-authorized");
    expect(!jw_flycast_ra_route_target_authorized(dsperate, "dsperate",
                                                  &PROVIDER_BOUND,
                                                  "mlp1/DSperate.pak",
                                                  platform),
           "route: account-authorized dsperate denied the flycast route");

    expect(!jw_flycast_ra_route_target_authorized(NULL, "flycast_standalone",
                                                  &FLYCAST_RELEASE, NULL,
                                                  platform) &&
               !jw_flycast_ra_route_target_authorized(launcher,
                                                      "flycast_standalone",
                                                      NULL, NULL, platform),
           "route: missing inputs denied");

    expect(strcmp(jw_flycast_ra_route_value(true), "service-live") == 0,
           "route value: live service");
    expect(strcmp(jw_flycast_ra_route_value(false), "native") == 0,
           "route value: no live service");
}

static void test_env_contract_names(void) {
    expect(strcmp(jw_ra_account_state_name(JW_RA_ACCOUNT_CONFIGURED),
                  "configured") == 0, "state configured");
    expect(strcmp(jw_ra_account_state_name(JW_RA_ACCOUNT_NEVER_CONFIGURED),
                  "never-configured") == 0, "state never-configured");
    expect(strcmp(jw_ra_account_state_name(JW_RA_ACCOUNT_SIGNED_OUT),
                  "signed-out") == 0, "state signed-out");
    expect(strcmp(jw_ra_account_state_name(JW_RA_ACCOUNT_INVALID),
                  "invalid") == 0, "state invalid");
    expect(strcmp(jw_ra_account_state_name(JW_RA_ACCOUNT_UNREADABLE),
                  "unreadable") == 0, "state unreadable");
    expect(jw_ra_account_state_name((jw_ra_account_state)999) == NULL,
           "out-of-range state has no name");
}

/* ------------------------------------------------------------------ */
/* The child environment the producer builds                          */
/* ------------------------------------------------------------------ */

static const char *const ACCOUNT_VARS[] = {
    JW_RA_ACCOUNT_ENV_VERSION, JW_RA_ACCOUNT_ENV_STATE,
    JW_RA_ACCOUNT_ENV_USERNAME, JW_RA_ACCOUNT_ENV_PASSWORD,
    JW_RA_ACCOUNT_ENV_REVISION,
};

/* Exactly the verdict: VERSION=1, STATE=state and no other account field. */
static int env_is_verdict(const char *state) {
    const char *v = getenv(JW_RA_ACCOUNT_ENV_VERSION);
    const char *st = getenv(JW_RA_ACCOUNT_ENV_STATE);
    return v && strcmp(v, "1") == 0 && st && strcmp(st, state) == 0 &&
           !getenv(JW_RA_ACCOUNT_ENV_USERNAME) &&
           !getenv(JW_RA_ACCOUNT_ENV_PASSWORD) &&
           !getenv(JW_RA_ACCOUNT_ENV_REVISION);
}

static int env_is_absent(void) {
    for (size_t i = 0; i < sizeof(ACCOUNT_VARS) / sizeof(ACCOUNT_VARS[0]); i++)
        if (getenv(ACCOUNT_VARS[i])) return 0;
    return !getenv("JAWAKA_CHEEVOS_USERNAME") &&
           !getenv("JAWAKA_CHEEVOS_PASSWORD");
}

static void seed_stale_env(void) {
    setenv(JW_RA_ACCOUNT_ENV_VERSION, "1", 1);
    setenv(JW_RA_ACCOUNT_ENV_STATE, "configured", 1);
    setenv(JW_RA_ACCOUNT_ENV_USERNAME, "stale-inherited", 1);
    setenv(JW_RA_ACCOUNT_ENV_PASSWORD, "stale-inherited", 1);
    setenv(JW_RA_ACCOUNT_ENV_REVISION, "99", 1);
    setenv("JAWAKA_CHEEVOS_USERNAME", "stale-inherited", 1);
    setenv("JAWAKA_CHEEVOS_PASSWORD", "stale-inherited", 1);
}

static jw_ra_account account(jw_ra_account_state state, const char *user,
                             const char *pass, long long revision) {
    jw_ra_account a;
    memset(&a, 0, sizeof(a));
    a.state = state;
    snprintf(a.user, sizeof(a.user), "%s", user);
    snprintf(a.pass, sizeof(a.pass), "%s", pass);
    a.revision = revision;
    return a;
}

static void test_env_guard(void) {
    jw_ra_account a;

    a = account(JW_RA_ACCOUNT_CONFIGURED, "player-one", "correct horse", 3);
    seed_stale_env();
    jw_ra_account_prepare_standalone_env(&a, true);
    expect(getenv(JW_RA_ACCOUNT_ENV_STATE) &&
           strcmp(getenv(JW_RA_ACCOUNT_ENV_STATE), "configured") == 0 &&
           strcmp(getenv(JW_RA_ACCOUNT_ENV_USERNAME), "player-one") == 0 &&
           strcmp(getenv(JW_RA_ACCOUNT_ENV_PASSWORD), "correct horse") == 0 &&
           strcmp(getenv(JW_RA_ACCOUNT_ENV_REVISION), "3") == 0 &&
           !getenv("JAWAKA_CHEEVOS_USERNAME") &&
           !getenv("JAWAKA_CHEEVOS_PASSWORD"),
           "configured snapshot replaces inherited values exactly");

    /* CONFIGURED is never emitted without an established counter. */
    static const long long bad_revisions[] = {
        0, -1, JW_RA_REVISION_MAX + 1,
    };
    for (size_t i = 0; i < sizeof(bad_revisions) / sizeof(bad_revisions[0]); i++) {
        char what[96];
        a = account(JW_RA_ACCOUNT_CONFIGURED, "player-one", "correct horse",
                    bad_revisions[i]);
        seed_stale_env();
        jw_ra_account_prepare_standalone_env(&a, true);
        snprintf(what, sizeof(what),
                 "configured with revision %lld exports unreadable",
                 bad_revisions[i]);
        expect(env_is_verdict("unreadable"), what);
    }

    /* Producer-side validation: credentials that fail the contract rules
       export the invalid verdict, never the values. */
    a = account(JW_RA_ACCOUNT_CONFIGURED, "player\r\none", "correct horse", 3);
    jw_ra_account_prepare_standalone_env(&a, true);
    expect(env_is_verdict("invalid"), "configured with CR/LF exports invalid");
    a = account(JW_RA_ACCOUNT_CONFIGURED, "", "correct horse", 3);
    jw_ra_account_prepare_standalone_env(&a, true);
    expect(env_is_verdict("invalid"), "configured with empty user exports invalid");

    /* Sign-out needs its retained revision too. */
    a = account(JW_RA_ACCOUNT_SIGNED_OUT, "", "", 0);
    jw_ra_account_prepare_standalone_env(&a, true);
    expect(env_is_verdict("unreadable"), "signed-out with revision 0 exports unreadable");
    a = account(JW_RA_ACCOUNT_SIGNED_OUT, "", "", 6);
    jw_ra_account_prepare_standalone_env(&a, true);
    expect(getenv(JW_RA_ACCOUNT_ENV_REVISION) &&
           strcmp(getenv(JW_RA_ACCOUNT_ENV_REVISION), "6") == 0 &&
           !getenv(JW_RA_ACCOUNT_ENV_USERNAME),
           "signed-out keeps its retained revision");

    /* An unauthorized target gets total absence. */
    a = account(JW_RA_ACCOUNT_CONFIGURED, "player-one", "correct horse", 3);
    seed_stale_env();
    jw_ra_account_prepare_standalone_env(&a, false);
    expect(env_is_absent(), "unauthorized target receives nothing");

    a = account((jw_ra_account_state)999, "", "", 0);
    seed_stale_env();
    jw_ra_account_prepare_standalone_env(&a, true);
    expect(env_is_absent(), "out-of-range state exports nothing");
}

static void db_exec(const char *db_path, const char *statement) {
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK ||
        sqlite3_exec(db, statement, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "ra-account-launch-test: fixture SQL failed\n");
        exit(1);
    }
    sqlite3_close(db);
}

/* The daemon path end to end short of fork(): stored rows -> the one launch
   resolve -> the child environment an authorized target receives. */
static void test_env_from_store(void) {
    char db[PATH_MAX];
    jw_ra_account a;

    /* A legacy pair whose revision cannot be written. */
    snprintf(db, sizeof(db), "%s/ensure-fails.db", root);
    if (jw_db_set_setting(db, "retroachievements_user", "player-one") != 0 ||
        jw_db_set_setting(db, "retroachievements_pass", "correct horse") != 0) {
        fprintf(stderr, "ra-account-launch-test: could not seed the store\n");
        exit(1);
    }
    db_exec(db,
            "CREATE TRIGGER ra_revision_write_fails BEFORE INSERT ON settings "
            "WHEN NEW.key = 'retroachievements_revision' "
            "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END;");
    jw_db_resolve_ra_account_handoff(db, &a);
    seed_stale_env();
    jw_ra_account_prepare_standalone_env(&a, true);
    expect(env_is_verdict("unreadable"),
           "store: failed legacy revision write hands off unreadable, no credentials");
    jw_ra_account_apply_retroarch_env(&a);
    expect(!getenv("JAWAKA_CHEEVOS_USERNAME") && !getenv("JAWAKA_CHEEVOS_PASSWORD"),
           "store: failed legacy revision write gives RetroArch nothing");

    /* The same pair once the write can happen: revision 1. */
    db_exec(db, "DROP TRIGGER ra_revision_write_fails;");
    jw_db_resolve_ra_account_handoff(db, &a);
    jw_ra_account_prepare_standalone_env(&a, true);
    expect(getenv(JW_RA_ACCOUNT_ENV_REVISION) &&
           strcmp(getenv(JW_RA_ACCOUNT_ENV_REVISION), "1") == 0 &&
           strcmp(getenv(JW_RA_ACCOUNT_ENV_STATE), "configured") == 0,
           "store: legacy pair hands off revision 1 once written");

    /* A malformed existing revision row. */
    if (jw_db_set_setting(db, "retroachievements_revision", "+7") != 0) exit(1);
    jw_db_resolve_ra_account_handoff(db, &a);
    jw_ra_account_prepare_standalone_env(&a, true);
    expect(env_is_verdict("invalid"),
           "store: malformed revision row hands off invalid, no credentials");
    jw_ra_account_clear_env();
}

int main(void) {
    snprintf(root, sizeof(root), "/tmp/jawaka-ra-launch.XXXXXX");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }

    test_flycast();
    test_dsperate();
    test_flycast_route();
    test_env_contract_names();
    test_env_guard();
    test_env_from_store();

    if (failures) {
        fprintf(stderr, "ra-account-launch-test: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("ra-account-launch-test: ok\n");
    return 0;
}
