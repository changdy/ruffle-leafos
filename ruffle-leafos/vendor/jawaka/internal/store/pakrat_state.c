#include "internal/store/pakrat_state.h"

#include "internal/db/db.h"
#include "internal/platform/leaf_version.h"
#include "internal/store/catalog_source.h"
#include "internal/store/managed_apps.h"
#include "internal/store/pakrat_recovery.h"
#include "internal/store/pakrat_state_logic.h"
#include "internal/store/pakrat_themes.h"
#include "internal/storage/sources.h"

#include "cJSON.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *data;
    size_t size;
} jw_mem;

#define JW_PAKRAT_CATALOG_MAX_BYTES (8u * 1024u * 1024u)
#define JW_PAKRAT_CONNECT_TIMEOUT_S 10L
#define JW_PAKRAT_TRANSFER_TIMEOUT_S 30L
#define JW_PAKRAT_MAX_REDIRS 5L

static int jw__curl_ready = 0;

static int jw__ensure_curl(void) {
    if (jw__curl_ready) {
        return 0;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return -1;
    }
    jw__curl_ready = 1;
    return 0;
}

static int jw__copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0 || !value) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s", value);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

/* Resolve a catalog install_path to the on-disk path under its kind's root, so
   a manually-present (unmanaged) pak or theme folder can be told apart from a
   merely-available one. The same resolution the installer uses. */
static int jw__install_target_path(const jw_pakrat_context *ctx,
                                   const char *install_path,
                                   char *out, size_t out_size) {
    return jw__pakrat_target_path(ctx->sdcard_root, install_path, out, out_size);
}

/* Room for the storefront's selections without putting them on the stack:
   this runs on a launcher worker thread, and a package record is kilobytes. */
#define JW_PAKRAT_MAX_SELECTIONS 128

static void jw__configure_curl_ca(CURL *curl) {
    static const char *ca_files[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/ssl/cert.pem",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/run/libreelec/cacert.pem",
        NULL,
    };
    for (int i = 0; ca_files[i]; i++) {
        if (jw__path_exists(ca_files[i])) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca_files[i]);
            return;
        }
    }
    if (jw__path_exists("/etc/ssl/certs")) {
        curl_easy_setopt(curl, CURLOPT_CAPATH, "/etc/ssl/certs");
    }
}

static size_t jw__curl_mem_write(void *ptr, size_t size, size_t nmemb,
                                 void *userdata) {
    jw_mem *mem = (jw_mem *)userdata;
    size_t bytes = size * nmemb;
    if (size != 0 && bytes / size != nmemb) {
        return 0;
    }
    if (mem->size > JW_PAKRAT_CATALOG_MAX_BYTES ||
        bytes > JW_PAKRAT_CATALOG_MAX_BYTES - mem->size) {
        return 0;
    }
    char *next = (char *)realloc(mem->data, mem->size + bytes + 1u);
    if (!next) {
        return 0;
    }
    mem->data = next;
    memcpy(mem->data + mem->size, ptr, bytes);
    mem->size += bytes;
    mem->data[mem->size] = '\0';
    return bytes;
}

static int jw__fetch_url(const char *url, int is_dev, jw_mem *out) {
    if (!url || !out || jw__ensure_curl() != 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    CURL *curl = curl_easy_init();
    if (!curl) {
        return -1;
    }
    char error[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
    jw__configure_curl_ca(curl);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, JW_PAKRAT_MAX_REDIRS);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, is_dev ? "http,https" : "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
                     is_dev ? "http,https" : "https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                     is_dev ? (CURLPROTO_HTTPS | CURLPROTO_HTTP) : CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                     is_dev ? (CURLPROTO_HTTPS | CURLPROTO_HTTP) : CURLPROTO_HTTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, JW_PAKRAT_CONNECT_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, JW_PAKRAT_TRANSFER_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, jw__curl_mem_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "jawaka-pakrat/1");
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || (http != 0 && http >= 400)) {
        fprintf(stderr, "Pak Rat fetch failed: url=%s curl=%d http=%ld%s%s\n",
                url, (int)rc, http, error[0] ? " error=" : "",
                error[0] ? error : "");
        free(out->data);
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

static int jw__fetch_storefront(const jw_pakrat_context *ctx, jw_mem *out,
                                int *out_is_dev_override) {
    if (!ctx || !ctx->state_dir[0] || !out) {
        return -1;
    }

    char base_url[1024];
    int is_dev = 0;
    if (jw_pakrat_catalog_base_url(ctx->state_dir, base_url, sizeof(base_url),
                                   &is_dev) != 0) {
        return -1;
    }
    if (!base_url[0]) {
        return 1;
    }
    if (out_is_dev_override) {
        *out_is_dev_override = is_dev;
    }

    char storefront_url[1200];
    if (snprintf(storefront_url, sizeof(storefront_url), "%sstorefront.json",
                 base_url) >= (int)sizeof(storefront_url)) {
        return -1;
    }
    if (jw__fetch_url(storefront_url, is_dev, out) != 0) {
        fprintf(stderr, "failed to fetch %s\n", storefront_url);
        return -1;
    }
    return 0;
}

static const char *jw__installed_leaf_version(const jw_pakrat_context *ctx,
                                               jw_installed_release *release) {
    memset(release, 0, sizeof(*release));
    return jw_installed_release_read(ctx->state_dir, release) == 0
               ? release->version
               : "";
}

static int jw__version_cmp_text(const char *left, const char *right,
                                int *out_cmp) {
    int left_version[3];
    int right_version[3];
    if (!left || !right || !out_cmp ||
        jw_pak_version_parse(left, left_version) != 0 ||
        jw_pak_version_parse(right, right_version) != 0) {
        return -1;
    }
    *out_cmp = jw_version_cmp(left_version, right_version);
    return 0;
}

static void jw__hide_obsolete_gate(jw_pakrat_app_state *state) {
    if (!state->gated_version[0] || !state->installed_owned) {
        return;
    }
    int cmp = 0;
    if (jw__version_cmp_text(state->gated_version,
                             state->installed_version, &cmp) != 0 ||
        cmp <= 0) {
        state->gated_version[0] = '\0';
        state->gated_min_leaf_version[0] = '\0';
    }
}

const char *jw_pakrat_app_status_name(jw_pakrat_app_status status) {
    switch (status) {
    case JW_PAKRAT_APP_AVAILABLE:
        return "available";
    case JW_PAKRAT_APP_INSTALLED:
        return "installed";
    case JW_PAKRAT_APP_UPDATE_AVAILABLE:
        return "update_available";
    case JW_PAKRAT_APP_STALE:
        return "stale";
    case JW_PAKRAT_APP_UNMANAGED:
        return "unmanaged";
    }
    return "unknown";
}

int jw_pakrat_find_catalog_package(const jw_pakrat_context *ctx,
                                   const char *store_id,
                                   jw_pakrat_catalog_package *out,
                                   int *out_is_dev_override) {
    if (!ctx || !ctx->platform[0] || !store_id || !store_id[0] || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    jw_mem storefront;
    int is_dev = 0;
    int fetch_rc = jw__fetch_storefront(ctx, &storefront, &is_dev);
    if (fetch_rc != 0) {
        return fetch_rc;
    }

    jw_installed_release release;
    jw_pakrat_catalog_selection *selections =
        calloc(JW_PAKRAT_MAX_SELECTIONS, sizeof(*selections));
    if (!selections) {
        free(storefront.data);
        return -1;
    }
    int count = 0;
    int parse_rc = jw_pakrat_catalog_parse_and_select(
        storefront.data, ctx->platform,
        jw__installed_leaf_version(ctx, &release), is_dev,
        selections, JW_PAKRAT_MAX_SELECTIONS, &count);
    free(storefront.data);
    int rc = parse_rc != 0 ? parse_rc : 1;
    for (int i = 0; parse_rc == 0 && i < count; i++) {
        if (strcmp(selections[i].package.id, store_id) == 0) {
            *out = selections[i].package;
            if (out_is_dev_override) {
                *out_is_dev_override = is_dev;
            }
            rc = 0;
            break;
        }
    }
    free(selections);
    return rc;
}

int jw_pakrat_find_catalog_package_version(
    const jw_pakrat_context *ctx,
    const char *store_id,
    const char *version,
    jw_pakrat_catalog_package *out,
    int *out_is_dev_override) {
    if (!ctx || !ctx->platform[0] || !store_id || !store_id[0] ||
        !version || !version[0] || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    jw_mem storefront;
    int is_dev = 0;
    int fetch_rc = jw__fetch_storefront(ctx, &storefront, &is_dev);
    if (fetch_rc != 0) {
        return fetch_rc;
    }
    int parse_rc = jw_pakrat_catalog_find_exact(
        storefront.data, ctx->platform, store_id, version, out);
    free(storefront.data);
    if (parse_rc == 0 && out_is_dev_override) {
        *out_is_dev_override = is_dev;
    }
    return parse_rc;
}

#define JW_PAKRAT_SMALL_FILE_MAX (256u * 1024u)

static char *jw__read_small_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (unsigned long long)st.st_size > JW_PAKRAT_SMALL_FILE_MAX) {
        return NULL;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    char *buf = (char *)malloc(len + 1u);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1u, len, fp);
    fclose(fp);
    if (got != len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

/* A one-line answer to "what does this pak add?", for the store detail page.
   Systems are what a user recognizes -- they become console tiles -- so they
   lead; cores are counted rather than named because a core the user cannot
   see by name is not information they can act on. */
static void jw__summarize_provides(const cJSON *provides, char *out,
                                   size_t out_size) {
    const cJSON *systems = cJSON_GetObjectItemCaseSensitive(provides, "systems");
    const cJSON *cores = cJSON_GetObjectItemCaseSensitive(provides, "cores");
    const cJSON *extensions =
        cJSON_GetObjectItemCaseSensitive(provides, "system_extensions");
    char ids[96] = {0};
    size_t used = 0;
    int system_count = 0;

    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, systems) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "id");
        if (!cJSON_IsString(id) || !id->valuestring) {
            continue;
        }
        system_count++;
        int written = snprintf(ids + used, sizeof(ids) - used, "%s%s",
                               used ? ", " : "", id->valuestring);
        if (written < 0 || (size_t)written >= sizeof(ids) - used) {
            ids[used] = '\0';
            break;
        }
        used += (size_t)written;
    }

    int core_count = cJSON_IsArray(cores) ? cJSON_GetArraySize(cores) : 0;
    int extension_count =
        cJSON_IsArray(extensions) ? cJSON_GetArraySize(extensions) : 0;

    if (system_count > 0 && core_count > 0) {
        snprintf(out, out_size, "%s and %d core%s", ids, core_count,
                 core_count == 1 ? "" : "s");
    } else if (system_count > 0) {
        snprintf(out, out_size, "%s", ids);
    } else if (core_count > 0 && extension_count > 0) {
        snprintf(out, out_size, "%d core%s for %d existing system%s",
                 core_count, core_count == 1 ? "" : "s",
                 extension_count, extension_count == 1 ? "" : "s");
    } else if (core_count > 0) {
        snprintf(out, out_size, "%d core%s", core_count,
                 core_count == 1 ? "" : "s");
    } else if (extension_count > 0) {
        snprintf(out, out_size, "%d system extension%s", extension_count,
                 extension_count == 1 ? "" : "s");
    }
}

/* Surface the newest diagnostics.json entry for this provider. Without this a
   refused contribution is silent: the pak installs, contributes nothing, and
   the only explanation lives in a file the user has no way to open. */
static void jw__read_content_diagnostic(const jw_pakrat_context *ctx,
                                        const char *install_path,
                                        char *out, size_t out_size) {
    char path[PATH_MAX];
    if (!ctx->state_dir[0] ||
        snprintf(path, sizeof(path), "%s/catalog/diagnostics.json",
                 ctx->state_dir) >= (int)sizeof(path)) {
        return;
    }
    char *text = jw__read_small_file(path);
    if (!text) {
        return;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        return;
    }
    const cJSON *entries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, entries) {
        const cJSON *provider =
            cJSON_GetObjectItemCaseSensitive(entry, "provider");
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(entry, "reason");
        const cJSON *detail = cJSON_GetObjectItemCaseSensitive(entry, "detail");
        if (!cJSON_IsString(provider) || !provider->valuestring ||
            strcmp(provider->valuestring, install_path) != 0 ||
            !cJSON_IsString(reason) || !reason->valuestring) {
            continue;
        }
        snprintf(out, out_size, "%s: %s", reason->valuestring,
                 (cJSON_IsString(detail) && detail->valuestring)
                     ? detail->valuestring
                     : "");
    }
    cJSON_Delete(root);
}

/* CONTENT-1 facts, read from the pak that is actually on disk.

   Deliberately not derived from the storefront lane: the same answer has to
   hold for a hybrid, for a pure content pak, and for a sideloaded pak the
   storefront has never heard of. "Open" and "is this install damaged?" both
   hang off this, so a lane-derived answer would be wrong in exactly the cases
   that matter. */
static void jw__fill_content_facts(const jw_pakrat_context *ctx,
                                   const jw_pakrat_install *install,
                                   jw_pakrat_app_state *state) {
    jw_storage_source_list sources;
    const jw_storage_source *source;
    char pak_dir[PATH_MAX];
    char manifest_path[PATH_MAX];
    char launch_path[PATH_MAX];

    /* A theme contributes no content and has nothing to open. */
    if (!install->install_path[0] ||
        jw_pakrat_install_path_kind(install->install_path) == JW_PAKRAT_KIND_THEME ||
        jw_storage_sources_resolve(ctx->sdcard_root, &sources) != 0) {
        return;
    }
    source = jw_storage_sources_find_by_id(
        &sources, install->source_id[0] ? install->source_id : "primary");
    if (!source || !source->available ||
        jw__pakrat_target_path(source->root, install->install_path, pak_dir,
                               sizeof(pak_dir)) != 0 ||
        snprintf(manifest_path, sizeof(manifest_path), "%s/pak.json",
                 pak_dir) >= (int)sizeof(manifest_path)) {
        return;
    }

    char *text = jw__read_small_file(manifest_path);
    if (!text) {
        return;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        return;
    }
    const cJSON *provides = cJSON_GetObjectItemCaseSensitive(root, "provides");
    if (cJSON_IsObject(provides)) {
        state->content_provides = 1;
        jw__summarize_provides(provides, state->provides_summary,
                               sizeof(state->provides_summary));
    }
    cJSON_Delete(root);

    struct stat st;
    int launchable =
        snprintf(launch_path, sizeof(launch_path), "%s/launch.sh", pak_dir) <
            (int)sizeof(launch_path) &&
        stat(launch_path, &st) == 0 && S_ISREG(st.st_mode) &&
        access(launch_path, X_OK) == 0;
    state->open_allowed = launchable ? 1 : 0;
    if (!state->content_provides) {
        return;
    }
    state->content_only = state->open_allowed ? 0 : 1;

    jw__read_content_diagnostic(ctx, install->install_path,
                                state->content_diagnostic,
                                sizeof(state->content_diagnostic));
}

int jw_pakrat_list_app_states(const jw_pakrat_context *ctx,
                              jw_pakrat_app_state *out,
                              int max_count,
                              int *out_count) {
    if (!ctx || !ctx->platform[0] || !ctx->state_dir[0] || !ctx->db_path[0] ||
        !ctx->platform_root[0] || !out || max_count <= 0 || !out_count) {
        return -1;
    }
    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    jw_mem storefront;
    int is_dev = 0;
    int fetch_rc = jw__fetch_storefront(ctx, &storefront, &is_dev);
    if (fetch_rc != 0) {
        return fetch_rc;
    }

    jw_installed_release release;
    jw_pakrat_catalog_selection *selections =
        calloc(JW_PAKRAT_MAX_SELECTIONS, sizeof(*selections));
    if (!selections) {
        free(storefront.data);
        return -1;
    }
    int package_count = 0;
    int parse_rc = jw_pakrat_catalog_parse_and_select(
        storefront.data, ctx->platform,
        jw__installed_leaf_version(ctx, &release), is_dev,
        selections, JW_PAKRAT_MAX_SELECTIONS, &package_count);
    if (parse_rc != 0) {
        free(selections);
        free(storefront.data);
        return parse_rc;
    }

    jw_pakrat_managed_apps managed;
    if (jw_pakrat_load_managed_apps(ctx->platform_root, &managed) != 0) {
        free(selections);
        free(storefront.data);
        return -1;
    }
    int db_available = jw__path_exists(ctx->db_path);
    /* Read once per listing: one opendir, not one per theme. */
    int theme_folders = -1;
    int rc = 0;

    for (int i = 0; i < package_count && *out_count < max_count; i++) {
        jw_pakrat_app_state *state = &out[*out_count];
        memset(state, 0, sizeof(*state));
        state->package = selections[i].package;
        state->lane = selections[i].lane;
        state->status = JW_PAKRAT_APP_AVAILABLE;
        state->primary_action_allowed = 1;
        jw__copy(state->action_version, sizeof(state->action_version),
                 state->package.version);
        jw__copy(state->gated_version, sizeof(state->gated_version),
                 selections[i].gated_version);
        jw__copy(state->gated_min_leaf_version,
                 sizeof(state->gated_min_leaf_version),
                 selections[i].gated_min_leaf_version);
        state->managed =
            jw_pakrat_managed_app_path_blocked(&managed,
                                               state->package.install_path) > 0;
        bool theme = state->package.kind == JW_PAKRAT_KIND_THEME;
        if (theme) {
            /* A folder a Leaf release replaces on every install is release
               managed in exactly the sense an app is. */
            int reserved = jw_pakrat_theme_name_reserved(
                ctx->platform_root, ctx->state_dir,
                state->package.install_name);
            state->theme_name_reserved = reserved != 0;
            if (reserved != 0) {
                state->managed = 1;
            }
        }

        jw_pakrat_install install;
        int install_rc = db_available ?
            jw_db_pakrat_get_install(ctx->db_path, state->package.id,
                                     &install) :
            1;
        if (install_rc < 0) {
            rc = -1;
            break;
        }
        /* A withdrawn theme is hidden from browsing. One already installed
           stays listed so the store can say it is no longer available. */
        if (theme && state->package.withdrawn && install_rc != 0) {
            continue;
        }
        if (install_rc == 0 && theme) {
            state->installed_owned = 1;
            jw__copy(state->installed_version, sizeof(state->installed_version),
                     install.version);
            jw__copy(state->installed_at, sizeof(state->installed_at),
                     install.installed_at);
            char target[PATH_MAX];
            state->app_present =
                jw__install_target_path(ctx, install.install_path, target,
                                        sizeof(target)) == 0 &&
                jw__path_exists(target);
            state->status = jw_pakrat_resolve_owned_theme_state(
                state->package.version, install.version,
                &state->primary_action_allowed);
            if (state->status == JW_PAKRAT_APP_INSTALLED) {
                /* Reinstall is the same version the catalog selects. */
                state->primary_action_allowed =
                    strcmp(state->package.version, install.version) == 0;
                if (!state->primary_action_allowed) {
                    state->action_version[0] = '\0';
                }
            }
            if (state->package.withdrawn) {
                state->primary_action_allowed = 0;
                state->action_version[0] = '\0';
            }
            jw__hide_obsolete_gate(state);
        } else if (install_rc == 0) {
            state->installed_owned = 1;
            state->app_present = install.app_present;
            jw__copy(state->installed_version, sizeof(state->installed_version),
                     install.version);
            jw__copy(state->installed_at, sizeof(state->installed_at),
                     install.installed_at);
            jw__copy(state->app_name, sizeof(state->app_name), install.app_name);
            jw__copy(state->app_pak_dir, sizeof(state->app_pak_dir),
                     install.app_pak_dir);

            jw__fill_content_facts(ctx, &install, state);
            state->status = jw_pakrat_resolve_owned_state(
                state->package.version, install.version, install.app_present,
                state->content_only, &state->primary_action_allowed);

            int needs_exact =
                state->status == JW_PAKRAT_APP_STALE ||
                state->status == JW_PAKRAT_APP_INSTALLED;
            if (needs_exact &&
                strcmp(state->package.version, install.version) == 0) {
                state->primary_action_allowed = 1;
                jw__copy(state->action_version,
                         sizeof(state->action_version), install.version);
                state->action_uses_history =
                    state->status == JW_PAKRAT_APP_STALE;
            } else if (needs_exact) {
                int installed_parsed[3];
                jw_pakrat_catalog_package exact;
                int exact_rc =
                    jw_pak_version_parse(install.version,
                                         installed_parsed) == 0
                        ? jw_pakrat_catalog_find_exact(
                              storefront.data, ctx->platform,
                              state->package.id, install.version, &exact)
                        : 1;
                if (exact_rc < 0) {
                    rc = exact_rc;
                    break;
                }
                if (exact_rc == 0) {
                    state->primary_action_allowed = 1;
                    state->action_uses_history = 1;
                    jw__copy(state->action_version,
                             sizeof(state->action_version), install.version);
                } else {
                    state->primary_action_allowed = 0;
                    state->action_version[0] = '\0';
                    state->installed_version_missing_from_history = 1;
                }
            }
            jw__hide_obsolete_gate(state);
        } else {
            /* No ownership row: if the pak is already on disk it was installed
               manually (or by an older tool). Surface it as unmanaged so the UI
               can offer to adopt it rather than wrongly calling it "available". */
            char target[PATH_MAX];
            if (jw__install_target_path(ctx, state->package.install_path,
                                        target, sizeof(target)) == 0 &&
                jw__path_exists(target)) {
                state->status = JW_PAKRAT_APP_UNMANAGED;
                state->app_present = 1;
            } else if (theme) {
                if (theme_folders < 0) {
                    theme_folders = jw_pakrat_theme_folder_count(ctx->sdcard_root);
                }
                state->theme_slots_full =
                    theme_folders >= 0 &&
                    jw_pakrat_theme_slots_full(theme_folders, false);
            }
        }

        (*out_count)++;
    }

    free(selections);
    free(storefront.data);
    if (rc != 0) {
        return rc;
    }
    return package_count <= max_count ? 0 : -1;
}
