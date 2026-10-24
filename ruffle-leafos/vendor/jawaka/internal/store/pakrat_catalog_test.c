#include "internal/store/pakrat_catalog.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define SHA_FLOOR \
    "1111111111111111111111111111111111111111111111111111111111111111"
#define SHA_NEW \
    "2222222222222222222222222222222222222222222222222222222222222222"

static const char *catalog_versions =
    "{"
    "\"schema\":1,"
    "\"product\":\"pak-rat\","
    "\"apps\":[{"
      "\"id\":\"org.umrk.portmaster\","
      "\"name\":\"PortMaster\","
      "\"summary\":\"Ports\","
      "\"version\":\"0.1.2\","
      "\"packages\":[{"
        "\"platform\":\"mlp1\","
        "\"runtime\":\"leaf\","
        "\"version\":\"0.1.2\","
        "\"install_name\":\"PortMaster.pak\","
        "\"runtime_manifest_path\":\"pak.json\","
        "\"artifact\":{"
          "\"url\":\"https://example.invalid/v0.1.2/PortMaster.zip\","
          "\"name\":\"PortMaster.zip\","
          "\"archive\":\"zip\","
          "\"size\":100,"
          "\"installed_size\":200,"
          "\"sha256\":\"" SHA_FLOOR "\""
        "},"
        "\"versions\":["
          "{"
            "\"version\":\"0.2.0\","
            "\"min_leaf_version\":\"0.7.0\","
            "\"artifact\":{"
              "\"url\":\"https://example.invalid/v0.2.0/PortMaster.zip\","
              "\"name\":\"PortMaster.zip\","
              "\"archive\":\"zip\","
              "\"size\":101,"
              "\"installed_size\":201,"
              "\"sha256\":\"" SHA_NEW "\""
            "}"
          "},"
          "{"
            "\"version\":\"0.1.2\","
            "\"artifact\":{"
              "\"url\":\"https://example.invalid/v0.1.2/PortMaster.zip\","
              "\"name\":\"PortMaster.zip\","
              "\"archive\":\"zip\","
              "\"size\":100,"
              "\"installed_size\":200,"
              "\"sha256\":\"" SHA_FLOOR "\""
            "}"
          "}"
        "]"
      "}]"
    "}]"
    "}";

static const char *catalog_legacy =
    "{"
    "\"schema\":1,"
    "\"product\":\"pak-rat\","
    "\"apps\":[{"
      "\"id\":\"org.example.legacy\","
      "\"name\":\"Legacy\","
      "\"summary\":\"Legacy package\","
      "\"version\":\"1.2.3\","
      "\"packages\":[{"
        "\"platform\":\"mlp1\","
        "\"runtime\":\"leaf\","
        "\"version\":\"1.2.3\","
        "\"install_name\":\"Legacy.pak\","
        "\"runtime_manifest_path\":\"pak.json\","
        "\"artifact\":{"
          "\"url\":\"https://example.invalid/Legacy.zip\","
          "\"name\":\"Legacy.zip\","
          "\"archive\":\"zip\","
          "\"size\":10,"
          "\"installed_size\":20,"
          "\"sha256\":\"" SHA_FLOOR "\""
        "}"
      "}]"
    "}]"
    "}";


/* ---- STORE-CONTENT-1: the content[] lane ------------------------------- */

#define CONTENT_ARTIFACT(url, sha) \
    "\"artifact\":{" \
      "\"url\":\"" url "\"," \
      "\"name\":\"ScummVM.zip\"," \
      "\"archive\":\"zip\"," \
      "\"size\":100," \
      "\"installed_size\":200," \
      "\"sha256\":\"" sha "\"" \
    "}"

/* Every version gated, and no ungated safe floor anywhere -- the exact shape
   the apps[] lane forbids and the reason content[] had to be a separate key
   rather than a flag on an existing one. */
static const char *catalog_content_all_gated =
    "{"
    "\"schema\":1,"
    "\"product\":\"pak-rat\","
    "\"apps\":[],"
    "\"content\":[{"
      "\"id\":\"org.umrk.scummvm\","
      "\"name\":\"ScummVM\","
      "\"summary\":\"ScummVM\","
      "\"version\":\"1.0.0\","
      "\"packages\":[{"
        "\"platform\":\"mlp1\","
        "\"runtime\":\"leaf\","
        "\"version\":\"1.0.0\","
        "\"min_leaf_version\":\"0.11.0\","
        "\"install_name\":\"ScummVM.pak\","
        "\"runtime_manifest_path\":\"pak.json\","
        CONTENT_ARTIFACT("https://example.invalid/1.0.0/ScummVM.zip", SHA_FLOOR) ","
        "\"versions\":[{"
          "\"version\":\"1.0.0\","
          "\"min_leaf_version\":\"0.11.0\","
          CONTENT_ARTIFACT("https://example.invalid/1.0.0/ScummVM.zip", SHA_FLOOR)
        "}]"
      "}]"
    "}]"
    "}";

/* S-3: the same id in both lanes. Both resolve to one install_path. */
static const char *catalog_id_in_both_lanes =
    "{"
    "\"schema\":1,"
    "\"product\":\"pak-rat\","
    "\"apps\":[{"
      "\"id\":\"org.umrk.scummvm\","
      "\"name\":\"ScummVM\","
      "\"summary\":\"ScummVM\","
      "\"version\":\"1.0.0\","
      "\"packages\":[{"
        "\"platform\":\"mlp1\","
        "\"runtime\":\"leaf\","
        "\"version\":\"1.0.0\","
        "\"install_name\":\"ScummVM.pak\","
        "\"runtime_manifest_path\":\"pak.json\","
        CONTENT_ARTIFACT("https://example.invalid/1.0.0/ScummVM.zip", SHA_FLOOR)
      "}]"
    "}],"
    "\"content\":[{"
      "\"id\":\"org.umrk.scummvm\","
      "\"name\":\"ScummVM\","
      "\"summary\":\"ScummVM\","
      "\"version\":\"1.0.0\","
      "\"packages\":[{"
        "\"platform\":\"mlp1\","
        "\"runtime\":\"leaf\","
        "\"version\":\"1.0.0\","
        "\"install_name\":\"ScummVM.pak\","
        "\"runtime_manifest_path\":\"pak.json\","
        CONTENT_ARTIFACT("https://example.invalid/1.0.0/ScummVM.zip", SHA_FLOOR)
      "}]"
    "}]"
    "}";

/* D16: a content package must name a concrete platform. */
static const char *catalog_content_shared_platform =
    "{"
    "\"schema\":1,"
    "\"product\":\"pak-rat\","
    "\"apps\":[],"
    "\"content\":[{"
      "\"id\":\"org.umrk.scummvm\","
      "\"name\":\"ScummVM\","
      "\"summary\":\"ScummVM\","
      "\"version\":\"1.0.0\","
      "\"packages\":[{"
        "\"platform\":\"shared\","
        "\"runtime\":\"leaf\","
        "\"version\":\"1.0.0\","
        "\"install_name\":\"ScummVM.pak\","
        "\"runtime_manifest_path\":\"pak.json\","
        CONTENT_ARTIFACT("https://example.invalid/1.0.0/ScummVM.zip", SHA_FLOOR)
      "}]"
    "}]"
    "}";

/* ---- The themes[] lane --------------------------------------------------- */

#define THEME_ARTIFACT(version, sha) \
    "\"artifact\":{" \
      "\"url\":\"https://example.invalid/neon-nights-" version ".zip\"," \
      "\"name\":\"neon-nights-" version ".zip\"," \
      "\"archive\":\"zip\"," \
      "\"size\":100," \
      "\"installed_size\":200," \
      "\"sha256\":\"" sha "\"" \
    "}"

#define THEME_ENTRY(extra) \
    "{" \
      "\"id\":\"neon-nights\"," \
      "\"name\":\"Neon Nights\"," \
      "\"author\":\"Example\"," \
      "\"owner_github_id\":1234567," \
      "\"summary\":\"Pink and cyan\"," \
      "\"description\":\"Pink and cyan on black.\"," \
      "\"license\":\"CC-BY-4.0\"," \
      "\"preview\":{\"url\":\"https://example.invalid/neon.preview.png\"," \
        "\"sha256\":\"" SHA_FLOOR "\",\"size\":123}," \
      "\"version\":\"1.2.0\"," \
      "\"min_leaf_version\":\"0.12.0\"," \
      "\"install_name\":\"neon-nights\"," \
      THEME_ARTIFACT("1.2.0", SHA_NEW) "," \
      "\"versions\":[" \
        "{\"version\":\"1.2.0\",\"min_leaf_version\":\"0.12.0\"," \
          THEME_ARTIFACT("1.2.0", SHA_NEW) "}," \
        "{\"version\":\"1.1.0\",\"min_leaf_version\":\"0.12.0\"," \
          THEME_ARTIFACT("1.1.0", SHA_FLOOR) "}" \
      "]" \
      extra \
    "}"

static const char *catalog_theme =
    "{\"schema\":1,\"product\":\"pak-rat\",\"apps\":[],"
    "\"themes\":[" THEME_ENTRY("") "]}";

static const char *catalog_theme_withdrawn =
    "{\"schema\":1,\"product\":\"pak-rat\",\"apps\":[],"
    "\"themes\":[" THEME_ENTRY(",\"withdrawn\":true") "]}";

static int parse_theme(const char *json, const char *platform, const char *leaf,
                       jw_pakrat_catalog_selection *out, int *count) {
    return jw_pakrat_catalog_parse_and_select(json, platform, leaf, 0, out, 4,
                                              count);
}

/* A theme document with one substitution applied, for the refusal cases. */
static int parse_theme_edit(const char *from, const char *to) {
    char edited[8192];
    const char *at = strstr(catalog_theme, from);
    assert(at);
    int n = snprintf(edited, sizeof(edited), "%.*s%s%s", (int)(at - catalog_theme),
                     catalog_theme, to, at + strlen(from));
    assert(n > 0 && n < (int)sizeof(edited));
    jw_pakrat_catalog_selection out[4];
    int count = 0;
    return parse_theme(edited, "mlp1", "0.12.0", out, &count);
}

static void test_themes_lane(void) {
    jw_pakrat_catalog_selection out[4];
    int count = 0;

    /* One entry, every platform: no packages[], no platform filter. */
    for (int i = 0; i < 2; i++) {
        const char *platform = i == 0 ? "mlp1" : "tg5040";
        assert(parse_theme(catalog_theme, platform, "0.12.0", out, &count) == 0);
        assert(count == 1);
        const jw_pakrat_catalog_package *pkg = &out[0].package;
        assert(out[0].lane == JW_PAKRAT_LANE_THEMES);
        assert(pkg->kind == JW_PAKRAT_KIND_THEME);
        assert(strcmp(pkg->version, "1.2.0") == 0);
        assert(strcmp(pkg->install_name, "neon-nights") == 0);
        assert(strcmp(pkg->install_path, "Themes/neon-nights") == 0);
        assert(jw_pakrat_install_path_kind(pkg->install_path) == JW_PAKRAT_KIND_THEME);
        assert(strcmp(pkg->platform, JW_PAKRAT_THEME_PLATFORM) == 0);
        assert(strcmp(pkg->runtime_manifest_path, "theme.json") == 0);
        assert(strcmp(pkg->author, "Example") == 0);
        assert(strcmp(pkg->license, "CC-BY-4.0") == 0);
        assert(strcmp(pkg->description, "Pink and cyan on black.") == 0);
        assert(strcmp(pkg->preview_url, "https://example.invalid/neon.preview.png") == 0);
        assert(strcmp(pkg->preview_sha256, SHA_FLOOR) == 0);
        assert(pkg->preview_size == 123);
        assert(pkg->owner_github_id == 1234567);
        assert(!pkg->withdrawn);
    }

    /* Every theme version is gated; below the gate there is nothing to offer. */
    assert(parse_theme(catalog_theme, "mlp1", "0.11.0", out, &count) == 0);
    assert(count == 0);

    /* Withdrawn is carried, not filtered: an installed copy must still show. */
    assert(parse_theme(catalog_theme_withdrawn, "mlp1", "0.12.0", out, &count) == 0);
    assert(count == 1 && out[0].package.withdrawn);

    jw_pakrat_catalog_package exact;
    assert(jw_pakrat_catalog_find_exact(catalog_theme, "mlp1", "neon-nights",
                                        "1.1.0", &exact) == 0);
    assert(exact.kind == JW_PAKRAT_KIND_THEME);
    assert(strcmp(exact.artifact_sha256, SHA_FLOOR) == 0);
    assert(jw_pakrat_catalog_find_exact(catalog_theme, "mlp1", "neon-nights",
                                        "1.3.0", &exact) == 1);

    /* Refusals: the catalog fails closed like every other lane. */
    assert(parse_theme_edit("\"install_name\":\"neon-nights\"",
                            "\"install_name\":\"neon-nights.pak\"") == -1);
    assert(parse_theme_edit("\"id\":\"neon-nights\"", "\"id\":\"Neon-Nights\"") == -1);
    assert(parse_theme_edit("\"author\":\"Example\",", "") == -1);
    assert(parse_theme_edit("\"license\":\"CC-BY-4.0\",", "") == -1);
    assert(parse_theme_edit("\"size\":123}", "\"size\":0}") == -1);
    assert(parse_theme_edit("\"owner_github_id\":1234567", "\"owner_github_id\":\"x\"") == -1);
    assert(parse_theme_edit("\"min_leaf_version\":\"0.12.0\",\"install_name\"",
                            "\"install_name\"") == -1);
    {
        const char *not_array =
            "{\"schema\":1,\"product\":\"pak-rat\",\"apps\":[],\"themes\":{}}";
        assert(parse_theme(not_array, "mlp1", "0.12.0", out, &count) == -1);
        const char *absent = "{\"schema\":1,\"product\":\"pak-rat\",\"apps\":[]}";
        assert(parse_theme(absent, "mlp1", "0.12.0", out, &count) == 0 && count == 0);
    }

    /* Ids are unique across apps[], content[] and themes[]. */
    {
        char doc[16384];
        snprintf(doc, sizeof(doc),
                 "{\"schema\":1,\"product\":\"pak-rat\",\"apps\":[],"
                 "\"themes\":[" THEME_ENTRY("") "," THEME_ENTRY("") "]}");
        assert(parse_theme(doc, "mlp1", "0.12.0", out, &count) == -1);

        snprintf(doc, sizeof(doc),
                 "{\"schema\":1,\"product\":\"pak-rat\","
                 "\"apps\":[{\"id\":\"neon-nights\",\"name\":\"N\",\"summary\":\"N\","
                 "\"version\":\"1.0.0\",\"packages\":[{\"platform\":\"mlp1\","
                 "\"version\":\"1.0.0\",\"install_name\":\"N.pak\","
                 CONTENT_ARTIFACT("https://example.invalid/N.zip", SHA_FLOOR) "}]}],"
                 "\"themes\":[" THEME_ENTRY("") "]}");
        assert(parse_theme(doc, "mlp1", "0.12.0", out, &count) == -1);

        snprintf(doc, sizeof(doc),
                 "{\"schema\":1,\"product\":\"pak-rat\",\"apps\":[],"
                 "\"content\":[{\"id\":\"neon-nights\",\"name\":\"N\",\"summary\":\"N\","
                 "\"version\":\"1.0.0\",\"packages\":[{\"platform\":\"mlp1\","
                 "\"version\":\"1.0.0\",\"min_leaf_version\":\"0.11.0\","
                 "\"install_name\":\"N.pak\","
                 CONTENT_ARTIFACT("https://example.invalid/N.zip", SHA_FLOOR) "}]}],"
                 "\"themes\":[" THEME_ENTRY("") "]}");
        assert(parse_theme(doc, "mlp1", "0.12.0", out, &count) == -1);
    }

    /* An app cannot sit on a platform called Themes: its install_path would
       resolve into the themes root. */
    {
        char doc[8192];
        assert(snprintf(doc, sizeof(doc), "%s", catalog_legacy) < (int)sizeof(doc));
        char *platform = strstr(doc, "\"platform\":\"mlp1\"");
        assert(platform);
        char rebuilt[8192];
        snprintf(rebuilt, sizeof(rebuilt), "%.*s\"platform\":\"Themes\"%s",
                 (int)(platform - doc), doc, platform + strlen("\"platform\":\"mlp1\""));
        int apps = 0;
        assert(jw_pakrat_catalog_parse_and_select(rebuilt, "Themes", "", 0, out, 4,
                                                  &apps) == -1);
    }
}

static int parse(const char *json, const char *leaf, int dev,
                 jw_pakrat_catalog_selection *selection) {
    int count = 0;
    int rc = jw_pakrat_catalog_parse_and_select(
        json, "mlp1", leaf, dev, selection, 1, &count);
    if (rc == 0) {
        assert(count == 1);
    }
    return rc;
}

int main(void) {
    jw_pakrat_catalog_selection selection;
    jw_pakrat_catalog_package exact;

    assert(parse(catalog_legacy, "", 0, &selection) == 0);
    assert(strcmp(selection.package.version, "1.2.3") == 0);
    assert(selection.gated_version[0] == '\0');

    assert(parse(catalog_versions, "v0.6.1", 0, &selection) == 0);
    assert(strcmp(selection.package.version, "0.1.2") == 0);
    assert(strcmp(selection.gated_version, "0.2.0") == 0);
    assert(strcmp(selection.gated_min_leaf_version, "0.7.0") == 0);

    assert(parse(catalog_versions, "v0.7.0-rc.1", 0, &selection) == 0);
    assert(strcmp(selection.package.version, "0.2.0") == 0);
    assert(strcmp(selection.package.min_leaf_version, "0.7.0") == 0);
    assert(selection.gated_version[0] == '\0');

    assert(parse(catalog_versions, "", 0, &selection) == 0);
    assert(strcmp(selection.package.version, "0.1.2") == 0);
    assert(strcmp(selection.gated_version, "0.2.0") == 0);

    assert(parse(catalog_versions, "", 1, &selection) == 0);
    assert(strcmp(selection.package.version, "0.2.0") == 0);

    assert(jw_pakrat_catalog_find_exact(
               catalog_versions, "mlp1", "org.umrk.portmaster", "0.2.0",
               &exact) == 0);
    assert(strcmp(exact.version, "0.2.0") == 0);
    assert(strcmp(exact.min_leaf_version, "0.7.0") == 0);
    assert(strcmp(exact.artifact_sha256, SHA_NEW) == 0);
    assert(jw_pakrat_catalog_find_exact(
               catalog_versions, "mlp1", "org.umrk.portmaster", "0.3.0",
               &exact) == 1);
    assert(jw_pakrat_catalog_find_exact(
               catalog_versions, "mlp1", "org.umrk.portmaster", "v0.2.0",
               &exact) == -1);

    const char *schema_newer =
        "{\"schema\":2,\"product\":\"pak-rat\",\"apps\":[]}";
    assert(parse(schema_newer, "0.7.0", 0, &selection) ==
           JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF);
    assert(jw_pakrat_catalog_find_exact(
               schema_newer, "mlp1", "org.umrk.portmaster", "0.2.0",
               &exact) == JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF);

    char mismatch[8192];
    assert(snprintf(mismatch, sizeof(mismatch), "%s", catalog_versions) <
           (int)sizeof(mismatch));
    char *hash = strstr(mismatch, SHA_FLOOR);
    assert(hash);
    hash[0] = '9';
    assert(parse(mismatch, "0.7.0", 0, &selection) == -1);

    char app_mismatch[8192];
    assert(snprintf(app_mismatch, sizeof(app_mismatch), "%s",
                    catalog_versions) < (int)sizeof(app_mismatch));
    char *app_version = strstr(app_mismatch, "\"version\":\"0.1.2\"");
    assert(app_version);
    memcpy(app_version, "\"version\":\"0.1.1\"",
           strlen("\"version\":\"0.1.1\""));
    assert(parse(app_mismatch, "0.7.0", 0, &selection) == -1);

    char package_suffix[8192];
    assert(snprintf(package_suffix, sizeof(package_suffix), "%s",
                    catalog_versions) < (int)sizeof(package_suffix));
    char *new_version = strstr(package_suffix, "\"version\":\"0.2.0\"");
    assert(new_version);
    memcpy(new_version, "\"version\":\"0.2.x\"",
           strlen("\"version\":\"0.2.x\""));
    assert(parse(package_suffix, "0.7.0", 0, &selection) == -1);

    const char *malformed_versions =
        "{"
        "\"schema\":1,\"product\":\"pak-rat\",\"apps\":[{"
        "\"id\":\"x\",\"name\":\"X\",\"summary\":\"X\",\"version\":\"1.0.0\","
        "\"packages\":[{"
        "\"platform\":\"mlp1\",\"version\":\"1.0.0\","
        "\"install_name\":\"X.pak\","
        "\"artifact\":{\"url\":\"https://example.invalid/X.zip\","
        "\"name\":\"X.zip\",\"archive\":\"zip\",\"size\":1,"
        "\"installed_size\":1,\"sha256\":\"" SHA_FLOOR "\"},"
        "\"versions\":{}"
        "}]}]}";
    assert(parse(malformed_versions, "0.7.0", 0, &selection) == -1);

    /* ---- STORE-CONTENT-1 ---------------------------------------------- */

    /* An all-gated content package is valid: there is no safe floor to
       protect, because no gate-unaware client can see this lane at all. */
    assert(parse(catalog_content_all_gated, "0.11.0", 0, &selection) == 0);
    assert(strcmp(selection.package.id, "org.umrk.scummvm") == 0);
    assert(strcmp(selection.package.version, "1.0.0") == 0);
    assert(selection.lane == JW_PAKRAT_LANE_CONTENT);

    /* Same document, a device too old for the gate: nothing is offered.
       The apps[] lane would have had a floor to fall back to; this one has
       none by design, so the package is simply unavailable. */
    {
        jw_pakrat_catalog_selection none[1];
        int count = -1;
        assert(jw_pakrat_catalog_parse_and_select(
                   catalog_content_all_gated, "mlp1", "0.10.0", 0, none, 1,
                   &count) == 0);
        assert(count == 0);
    }

    /* A package in apps[] keeps reporting the apps lane. */
    assert(parse(catalog_versions, "v0.6.1", 0, &selection) == 0);
    assert(selection.lane == JW_PAKRAT_LANE_APPS);

    /* Exact-version repair searches both lanes, so it keeps working across
       the PortMaster lane migration. */
    assert(jw_pakrat_catalog_find_exact(
               catalog_content_all_gated, "mlp1", "org.umrk.scummvm", "1.0.0",
               &exact) == 0);
    assert(strcmp(exact.version, "1.0.0") == 0);

    assert(parse(catalog_id_in_both_lanes, "0.11.0", 0, &selection) == -1);
    assert(parse(catalog_content_shared_platform, "0.11.0", 0, &selection) == -1);

    /* Every content version is gated by construction, including the legacy
       mirror. An ungated entry would expose a contract-dependent pak to a
       client that cannot honor CONTENT-1. */
    {
        char ungated[8192];
        assert(snprintf(ungated, sizeof(ungated), "%s",
                        catalog_content_all_gated) < (int)sizeof(ungated));
        char *gate = strstr(ungated, "\"min_leaf_version\":\"0.11.0\",");
        assert(gate);
        memmove(gate, gate + strlen("\"min_leaf_version\":\"0.11.0\","),
                strlen(gate + strlen("\"min_leaf_version\":\"0.11.0\",")) + 1);
        assert(parse(ungated, "0.11.0", 0, &selection) == -1);
    }

    /* A present-but-malformed content key is refused rather than ignored. */
    {
        const char *content_not_array =
            "{\"schema\":1,\"product\":\"pak-rat\","
            "\"apps\":[],\"content\":{}}";
        jw_pakrat_catalog_selection none[1];
        int count = 0;
        assert(jw_pakrat_catalog_parse_and_select(
                   content_not_array, "mlp1", "0.11.0", 0, none, 1,
                   &count) == -1);
    }

    /* An ABSENT content key is not an error -- that is every storefront
       published before this contract. */
    {
        jw_pakrat_catalog_selection one[1];
        int count = 0;
        assert(jw_pakrat_catalog_parse_and_select(
                   catalog_legacy, "mlp1", "", 0, one, 1, &count) == 0);
        assert(count == 1);
        assert(one[0].lane == JW_PAKRAT_LANE_APPS);
    }

    test_themes_lane();

    puts("PASS pakrat-catalog-test");
    return 0;
}
