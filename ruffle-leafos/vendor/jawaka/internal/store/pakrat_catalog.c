#include "internal/store/pakrat_catalog.h"

#include "cJSON.h"
#include "internal/platform/leaf_version.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JW_PAKRAT_MAX_VERSIONS_PER_PACKAGE 16

typedef struct {
    jw_pakrat_catalog_package versions[JW_PAKRAT_MAX_VERSIONS_PER_PACKAGE];
    int count;
} jw__package_history;

static const char *jw__json_string(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static long long jw__json_positive_integer(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item) || item->valuedouble <= 0 ||
        item->valuedouble > (double)LLONG_MAX ||
        item->valuedouble != (double)(long long)item->valuedouble) {
        return 0;
    }
    return (long long)item->valuedouble;
}

static int jw__copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0 || !value) {
        return -1;
    }
    int needed = snprintf(out, out_size, "%s", value);
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}

static int jw__package_valid(const jw_pakrat_catalog_package *pkg) {
    int parsed[3];
    /* An app on a platform called "Themes" would resolve into the themes root:
       install_path carries the root (pakrat_kind.h). */
    return pkg && pkg->id[0] && pkg->name[0] && pkg->version[0] &&
           (pkg->kind == JW_PAKRAT_KIND_THEME ||
            strcmp(pkg->platform, JW_PAKRAT_THEMES_DIR) != 0) &&
           jw_pak_version_parse(pkg->version, parsed) == 0 &&
           (!pkg->min_leaf_version[0] ||
            jw_pak_version_parse(pkg->min_leaf_version, parsed) == 0) &&
           pkg->platform[0] && pkg->install_name[0] &&
           pkg->install_path[0] && pkg->runtime_manifest_path[0] &&
           pkg->artifact_url[0] && pkg->artifact_name[0] &&
           strcmp(pkg->artifact_archive, "zip") == 0 &&
           pkg->artifact_sha256[0] && pkg->artifact_size > 0 &&
           pkg->artifact_installed_size > 0;
}

/* THEME-1's id grammar: ^[a-z0-9][a-z0-9-]{1,39}$. The id names the theme's
   folder on the card, so the catalog holds it to the same rule the package
   does. */
static int jw__theme_id_valid(const char *id) {
    size_t len = id ? strlen(id) : 0;
    if (len < 2 || len > 40) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 (i > 0 && c == '-');
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* A present key of the wrong type is malformed; an absent one is not. */
static int jw__fill_theme_fields(const cJSON *theme,
                                 jw_pakrat_catalog_package *out) {
    const cJSON *preview = cJSON_GetObjectItemCaseSensitive(theme, "preview");
    const cJSON *owner =
        cJSON_GetObjectItemCaseSensitive(theme, "owner_github_id");
    const cJSON *withdrawn =
        cJSON_GetObjectItemCaseSensitive(theme, "withdrawn");
    const cJSON *description =
        cJSON_GetObjectItemCaseSensitive(theme, "description");
    if (!cJSON_IsObject(preview) ||
        (description && !cJSON_IsString(description)) ||
        (withdrawn && !cJSON_IsBool(withdrawn)) ||
        jw__copy(out->author, sizeof(out->author),
                 jw__json_string(theme, "author")) != 0 ||
        jw__copy(out->description, sizeof(out->description),
                 jw__json_string(theme, "description")) != 0 ||
        jw__copy(out->license, sizeof(out->license),
                 jw__json_string(theme, "license")) != 0 ||
        jw__copy(out->preview_url, sizeof(out->preview_url),
                 jw__json_string(preview, "url")) != 0 ||
        jw__copy(out->preview_sha256, sizeof(out->preview_sha256),
                 jw__json_string(preview, "sha256")) != 0) {
        return -1;
    }
    out->preview_size = jw__json_positive_integer(preview, "size");
    out->owner_github_id = jw__json_positive_integer(theme, "owner_github_id");
    out->withdrawn = cJSON_IsTrue(withdrawn) ? 1 : 0;
    return jw__theme_id_valid(out->id) &&
                   strcmp(out->install_name, out->id) == 0 &&
                   out->author[0] && out->license[0] &&
                   out->preview_url[0] && out->preview_sha256[0] &&
                   out->preview_size > 0 &&
                   (!owner || out->owner_github_id > 0)
               ? 0
               : -1;
}

static int jw__fill_package(const cJSON *app, const cJSON *package,
                            const cJSON *version,
                            jw_pakrat_catalog_lane lane,
                            jw_pakrat_catalog_package *out) {
    const cJSON *artifact =
        cJSON_GetObjectItemCaseSensitive(version, "artifact");
    if (!cJSON_IsObject(artifact)) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    int theme = lane == JW_PAKRAT_LANE_THEMES;
    out->kind = theme ? JW_PAKRAT_KIND_THEME : JW_PAKRAT_KIND_APP;
    if (jw__copy(out->id, sizeof(out->id),
                 jw__json_string(app, "id")) != 0 ||
        jw__copy(out->name, sizeof(out->name),
                 jw__json_string(app, "name")) != 0 ||
        jw__copy(out->summary, sizeof(out->summary),
                 jw__json_string(app, "summary")) != 0 ||
        jw__copy(out->platform, sizeof(out->platform),
                 theme ? JW_PAKRAT_THEME_PLATFORM
                       : jw__json_string(package, "platform")) != 0 ||
        jw__copy(out->version, sizeof(out->version),
                 jw__json_string(version, "version")) != 0 ||
        jw__copy(out->min_leaf_version, sizeof(out->min_leaf_version),
                 jw__json_string(version, "min_leaf_version")) != 0 ||
        jw__copy(out->install_name, sizeof(out->install_name),
                 jw__json_string(package, "install_name")) != 0 ||
        jw__copy(out->runtime_manifest_path,
                 sizeof(out->runtime_manifest_path),
                 jw__json_string(package, "runtime_manifest_path")) != 0 ||
        jw__copy(out->artifact_url, sizeof(out->artifact_url),
                 jw__json_string(artifact, "url")) != 0 ||
        jw__copy(out->artifact_name, sizeof(out->artifact_name),
                 jw__json_string(artifact, "name")) != 0 ||
        jw__copy(out->artifact_archive, sizeof(out->artifact_archive),
                 jw__json_string(artifact, "archive")) != 0 ||
        jw__copy(out->artifact_sha256, sizeof(out->artifact_sha256),
                 jw__json_string(artifact, "sha256")) != 0) {
        return -1;
    }
    if (theme) {
        /* A theme is judged by THEME-1, not by a manifest the catalog names. */
        if (jw__copy(out->runtime_manifest_path,
                     sizeof(out->runtime_manifest_path),
                     JW_PAKRAT_THEME_MANIFEST) != 0) {
            return -1;
        }
    } else if (!out->runtime_manifest_path[0] &&
               jw__copy(out->runtime_manifest_path,
                        sizeof(out->runtime_manifest_path), "pak.json") != 0) {
        return -1;
    }
    if (snprintf(out->install_path, sizeof(out->install_path), "%s/%s",
                 theme ? JW_PAKRAT_THEMES_DIR : out->platform,
                 out->install_name) >=
        (int)sizeof(out->install_path)) {
        return -1;
    }
    out->artifact_size = jw__json_positive_integer(artifact, "size");
    out->artifact_installed_size =
        jw__json_positive_integer(artifact, "installed_size");
    if (theme && jw__fill_theme_fields(app, out) != 0) {
        return -1;
    }
    return jw__package_valid(out) ? 0 : -1;
}

static int jw__artifact_equal(const jw_pakrat_catalog_package *left,
                              const jw_pakrat_catalog_package *right) {
    return left && right &&
           strcmp(left->artifact_url, right->artifact_url) == 0 &&
           strcmp(left->artifact_name, right->artifact_name) == 0 &&
           strcmp(left->artifact_archive, right->artifact_archive) == 0 &&
           strcmp(left->artifact_sha256, right->artifact_sha256) == 0 &&
           left->artifact_size == right->artifact_size &&
           left->artifact_installed_size == right->artifact_installed_size;
}

static int jw__version_cmp_text(const char *left, const char *right) {
    int left_version[3];
    int right_version[3];
    if (jw_pak_version_parse(left, left_version) != 0 ||
        jw_pak_version_parse(right, right_version) != 0) {
        return 0;
    }
    return jw_version_cmp(left_version, right_version);
}

static int jw__gate_passes(const jw_pakrat_catalog_package *version,
                           const int device_version[3],
                           int device_known,
                           int is_dev_override) {
    if (!version->min_leaf_version[0]) {
        return 1;
    }
    if (!device_known) {
        return is_dev_override != 0;
    }
    int minimum[3];
    return jw_pak_version_parse(version->min_leaf_version, minimum) == 0 &&
           jw_version_cmp(device_version, minimum) >= 0;
}

static int jw__parse_package_history(const cJSON *app, const cJSON *package,
                                     jw_pakrat_catalog_lane lane,
                                     jw__package_history *out) {
    jw_pakrat_catalog_package legacy;
    if (!out ||
        jw__fill_package(app, package, package, lane, &legacy) != 0 ||
        strcmp(jw__json_string(app, "version"), legacy.version) != 0) {
        return -1;
    }
    /* In apps[] the legacy fields ARE the ungated safe floor, so carrying a
       gate there would hand a gate-unaware client something it cannot run.
       In content[] and themes[] no gate-unaware client is looking, and the
       legacy fields mirror the newest entry -- gate included, because that
       entry has one. */
    int gated_lane = lane != JW_PAKRAT_LANE_APPS;
    if ((gated_lane && !legacy.min_leaf_version[0]) ||
        (!gated_lane && legacy.min_leaf_version[0])) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    const cJSON *versions =
        cJSON_GetObjectItemCaseSensitive(package, "versions");
    if (!versions) {
        out->versions[0] = legacy;
        out->count = 1;
        return 0;
    }
    if (!cJSON_IsArray(versions)) {
        return -1;
    }

    int version_count = cJSON_GetArraySize(versions);
    if (version_count <= 0 ||
        version_count > JW_PAKRAT_MAX_VERSIONS_PER_PACKAGE) {
        return -1;
    }

    int legacy_index = -1;
    int safe_floor_index = -1;
    for (int i = 0; i < version_count; i++) {
        const cJSON *entry = cJSON_GetArrayItem(versions, i);
        if (!cJSON_IsObject(entry) ||
            jw__fill_package(app, package, entry, lane,
                             &out->versions[i]) != 0) {
            return -1;
        }
        if (gated_lane && !out->versions[i].min_leaf_version[0]) {
            return -1;
        }
        for (int j = 0; j < i; j++) {
            if (strcmp(out->versions[i].version,
                       out->versions[j].version) == 0) {
                return -1;
            }
        }
        if (!out->versions[i].min_leaf_version[0] &&
            (safe_floor_index < 0 ||
             jw__version_cmp_text(out->versions[i].version,
                                  out->versions[safe_floor_index].version) > 0)) {
            safe_floor_index = i;
        }
        if (strcmp(out->versions[i].version, legacy.version) == 0) {
            legacy_index = i;
        }
    }

    if (gated_lane) {
        /* No ungated safe floor is required here, and none is expected: every
           content package and theme gates on the contract that defines it. The legacy
           fields still have to mirror SOMETHING real, so they mirror the
           newest entry rather than the newest ungated one. */
        int newest_index = 0;
        for (int i = 1; i < version_count; i++) {
            if (jw__version_cmp_text(out->versions[i].version,
                                     out->versions[newest_index].version) > 0) {
                newest_index = i;
            }
        }
        if (legacy_index < 0 || legacy_index != newest_index ||
            !jw__artifact_equal(&legacy, &out->versions[legacy_index]) ||
            strcmp(legacy.min_leaf_version,
                   out->versions[legacy_index].min_leaf_version) != 0) {
            return -1;
        }
    } else if (safe_floor_index < 0 || legacy_index < 0 ||
               legacy_index != safe_floor_index ||
               out->versions[legacy_index].min_leaf_version[0] ||
               !jw__artifact_equal(&legacy, &out->versions[legacy_index])) {
        /* apps[] is the gate-unaware lane: its legacy fields are what an old
           client installs, so they must be the newest UNGATED entry. */
        return -1;
    }
    out->count = version_count;
    return 0;
}

/* Returns 0 when a version was selected, 1 when the package exists but has
   nothing this device can install, and -1 when the catalog is malformed. */
static int jw__select_package(const cJSON *app, const cJSON *package,
                              jw_pakrat_catalog_lane lane,
                              const int device_version[3],
                              int device_known, int is_dev_override,
                              jw_pakrat_catalog_selection *out) {
    /* Sixteen full package records: kept off the stack, since this also runs
       on a launcher worker thread. */
    jw__package_history *history = malloc(sizeof(*history));
    if (!history || jw__parse_package_history(app, package, lane, history) != 0) {
        free(history);
        return -1;
    }
    int rc = 0;
    int selected_index = -1;
    int gated_index = -1;
    for (int i = 0; i < history->count; i++) {
        if (jw__gate_passes(&history->versions[i], device_version, device_known,
                            is_dev_override)) {
            if (selected_index < 0 ||
                jw__version_cmp_text(history->versions[i].version,
                                     history->versions[selected_index].version) > 0) {
                selected_index = i;
            }
        } else if (gated_index < 0 ||
                   jw__version_cmp_text(history->versions[i].version,
                                        history->versions[gated_index].version) > 0) {
            gated_index = i;
        }
    }
    if (selected_index < 0) {
        /* An apps[] package always has an ungated floor, so nothing passing
           the gate means the catalog contradicts itself. A content package
           or theme has no floor by design: on a device below its minimum
           there is simply nothing to offer, which is not an error. */
        free(history);
        return lane != JW_PAKRAT_LANE_APPS ? 1 : -1;
    }

    memset(out, 0, sizeof(*out));
    out->package = history->versions[selected_index];
    if (gated_index >= 0 &&
        jw__version_cmp_text(history->versions[gated_index].version,
                             history->versions[selected_index].version) > 0) {
        if (jw__copy(out->gated_version, sizeof(out->gated_version),
                     history->versions[gated_index].version) != 0 ||
            jw__copy(out->gated_min_leaf_version,
                     sizeof(out->gated_min_leaf_version),
                     history->versions[gated_index].min_leaf_version) != 0) {
            rc = -1;
        }
    }
    free(history);
    return rc;
}

/* Exact version lookup in one package's history: 0 found, 1 absent, -1 when
   the history is malformed. */
static int jw__find_in_history(const cJSON *app, const cJSON *package,
                               jw_pakrat_catalog_lane lane,
                               const char *requested_version,
                               jw_pakrat_catalog_package *out) {
    jw__package_history *history = malloc(sizeof(*history));
    if (!history || jw__parse_package_history(app, package, lane, history) != 0) {
        free(history);
        return -1;
    }
    int rc = 1;
    for (int i = 0; i < history->count; i++) {
        if (strcmp(history->versions[i].version, requested_version) == 0) {
            *out = history->versions[i];
            rc = 0;
            break;
        }
    }
    free(history);
    return rc;
}

/* S-3: an id may appear in exactly one lane. Both lanes resolve to the same
   install_path, so two catalog entries pointing at one install path is a
   duplicate-identity bug rather than a way to serve two audiences. The
   generator and the CI validator reject this at publication time; the client
   refuses it too, because a catalog that disagrees with itself is exactly the
   "internally inconsistent" case this parser already fails closed on. */
static int jw__lane_contains_id(const cJSON *lane, const char *id) {
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, lane) {
        if (cJSON_IsObject(entry) &&
            strcmp(jw__json_string(entry, "id"), id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int jw__lane_count_id(const cJSON *lane, const char *id) {
    int count = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, lane) {
        if (cJSON_IsObject(entry) &&
            strcmp(jw__json_string(entry, "id"), id) == 0) {
            count++;
        }
    }
    return count;
}

/* An absent lane is not an error: `content` and `themes` are optional, and
   `apps` may be empty. A present-but-not-an-array lane is malformed. */
static int jw__lane_array(const cJSON *root, const char *key,
                          const cJSON **out) {
    const cJSON *lane = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!lane) {
        *out = NULL;
        return 0;
    }
    if (!cJSON_IsArray(lane)) {
        return -1;
    }
    *out = lane;
    return 0;
}

static int jw__catalog_root_valid(const cJSON *root) {
    if (!cJSON_IsObject(root)) {
        return -1;
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (schema) {
        if (!cJSON_IsNumber(schema) ||
            schema->valuedouble != (double)schema->valueint ||
            schema->valueint < 1) {
            return -1;
        }
        if (schema->valueint > JW_PAKRAT_CATALOG_SCHEMA_MAX) {
            return JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF;
        }
    }
    return strcmp(jw__json_string(root, "product"), "pak-rat") == 0
               ? 0
               : -1;
}

int jw_pakrat_catalog_parse_and_select(
    const char *json,
    const char *platform,
    const char *device_leaf_version,
    int is_dev_override,
    jw_pakrat_catalog_selection *out,
    int max_count,
    int *out_count) {
    if (!json || !platform || !platform[0] || !out || max_count <= 0 ||
        !out_count) {
        return -1;
    }
    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    cJSON *root = cJSON_Parse(json);
    int root_rc = jw__catalog_root_valid(root);
    if (root_rc != 0) {
        cJSON_Delete(root);
        return root_rc;
    }

    int device_version[3] = {0, 0, 0};
    int device_known =
        jw_leaf_release_version_parse(device_leaf_version,
                                      device_version) == 0;
    const cJSON *apps = NULL;
    const cJSON *content = NULL;
    const cJSON *themes = NULL;
    if (jw__lane_array(root, "apps", &apps) != 0 ||
        jw__lane_array(root, "content", &content) != 0 ||
        jw__lane_array(root, "themes", &themes) != 0 || !apps) {
        cJSON_Delete(root);
        return -1;
    }

    const struct {
        const cJSON *lane;
        jw_pakrat_catalog_lane kind;
    } lanes[] = {{apps, JW_PAKRAT_LANE_APPS},
                 {content, JW_PAKRAT_LANE_CONTENT},
                 {themes, JW_PAKRAT_LANE_THEMES}};

    for (size_t l = 0; l < sizeof(lanes) / sizeof(lanes[0]); l++) {
        const cJSON *app = NULL;
        cJSON_ArrayForEach(app, lanes[l].lane) {
            if (!cJSON_IsObject(app)) {
                cJSON_Delete(root);
                return -1;
            }
            const char *app_id = jw__json_string(app, "id");
            if ((lanes[l].kind == JW_PAKRAT_LANE_CONTENT &&
                 jw__lane_contains_id(apps, app_id)) ||
                (lanes[l].kind == JW_PAKRAT_LANE_THEMES &&
                 (jw__lane_contains_id(apps, app_id) ||
                  jw__lane_contains_id(content, app_id) ||
                  jw__lane_count_id(themes, app_id) > 1))) {
                cJSON_Delete(root);
                return -1;
            }
            if (lanes[l].kind == JW_PAKRAT_LANE_THEMES) {
                /* One entry is one package on every platform. */
                if (*out_count >= max_count) {
                    cJSON_Delete(root);
                    return -1;
                }
                int selected = jw__select_package(
                    app, app, JW_PAKRAT_LANE_THEMES, device_version,
                    device_known, is_dev_override, &out[*out_count]);
                if (selected < 0) {
                    cJSON_Delete(root);
                    return -1;
                }
                if (selected == 0) {
                    out[*out_count].lane = JW_PAKRAT_LANE_THEMES;
                    (*out_count)++;
                }
                continue;
            }
            const cJSON *packages =
                cJSON_GetObjectItemCaseSensitive(app, "packages");
            if (!cJSON_IsArray(packages)) {
                cJSON_Delete(root);
                return -1;
            }
            const cJSON *package = NULL;
            cJSON_ArrayForEach(package, packages) {
                if (!cJSON_IsObject(package)) {
                    continue;
                }
                const char *pkg_platform =
                    jw__json_string(package, "platform");
                /* D16: cores and standalone emulator binaries are
                   platform-specific, so a content package must name a
                   concrete platform. "shared" is refused rather than
                   skipped -- skipping would hide a malformed catalog. */
                if (lanes[l].kind == JW_PAKRAT_LANE_CONTENT &&
                    strcmp(pkg_platform, "shared") == 0) {
                    cJSON_Delete(root);
                    return -1;
                }
                if (strcmp(pkg_platform, platform) != 0) {
                    continue;
                }
                if (*out_count >= max_count) {
                    cJSON_Delete(root);
                    return -1;
                }
                int selected = jw__select_package(
                    app, package, lanes[l].kind, device_version, device_known,
                    is_dev_override, &out[*out_count]);
                if (selected < 0) {
                    cJSON_Delete(root);
                    return -1;
                }
                if (selected > 0) {
                    continue; /* nothing installable on this device */
                }
                out[*out_count].lane = lanes[l].kind;
                (*out_count)++;
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}

int jw_pakrat_catalog_find_exact(
    const char *json,
    const char *platform,
    const char *store_id,
    const char *requested_version,
    jw_pakrat_catalog_package *out) {
    int parsed_version[3];
    if (!json || !platform || !platform[0] || !store_id || !store_id[0] ||
        !requested_version || jw_pak_version_parse(requested_version,
                                                   parsed_version) != 0 ||
        !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    int root_rc = jw__catalog_root_valid(root);
    if (root_rc != 0) {
        cJSON_Delete(root);
        return root_rc;
    }
    const cJSON *apps = NULL;
    const cJSON *content = NULL;
    const cJSON *themes = NULL;
    if (jw__lane_array(root, "apps", &apps) != 0 ||
        jw__lane_array(root, "content", &content) != 0 ||
        jw__lane_array(root, "themes", &themes) != 0 || !apps) {
        cJSON_Delete(root);
        return -1;
    }

    /* Repair of an owned install must keep working after a package moves
       lanes -- PortMaster does exactly that in Phase 9 -- so exact-version
       lookup searches every lane and does not care which one answers. */
    const cJSON *lane_list[] = {apps, content, themes};
    for (size_t l = 0; l < sizeof(lane_list) / sizeof(lane_list[0]); l++) {
    const cJSON *app = NULL;
    cJSON_ArrayForEach(app, lane_list[l]) {
        if (!cJSON_IsObject(app)) {
            cJSON_Delete(root);
            return -1;
        }
        if (strcmp(jw__json_string(app, "id"), store_id) != 0) {
            continue;
        }
        if (lane_list[l] == themes) {
            int found = jw__find_in_history(app, app, JW_PAKRAT_LANE_THEMES,
                                            requested_version, out);
            if (found <= 0) {
                cJSON_Delete(root);
                return found;
            }
            continue;
        }
        const cJSON *packages =
            cJSON_GetObjectItemCaseSensitive(app, "packages");
        if (!cJSON_IsArray(packages)) {
            cJSON_Delete(root);
            return -1;
        }
        const cJSON *package = NULL;
        cJSON_ArrayForEach(package, packages) {
            if (!cJSON_IsObject(package) ||
                strcmp(jw__json_string(package, "platform"), platform) != 0) {
                continue;
            }
            int found = jw__find_in_history(
                app, package,
                lane_list[l] == apps ? JW_PAKRAT_LANE_APPS
                                     : JW_PAKRAT_LANE_CONTENT,
                requested_version, out);
            if (found <= 0) {
                cJSON_Delete(root);
                return found;
            }
        }
    }
    }

    cJSON_Delete(root);
    return 1;
}
