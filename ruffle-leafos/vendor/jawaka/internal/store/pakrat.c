#include "internal/store/pakrat.h"

#include "internal/db/db.h"
#include "internal/discovery/discovery.h"
#include "internal/ipc/ipc.h"
#include "internal/launcher/user_themes.h"
#include "internal/platform/leaf_version.h"
#include "internal/store/catalog_source.h"
#include "internal/store/managed_apps.h"
#include "internal/store/pakrat_recovery.h"
#include "internal/store/pakrat_themes.h"
#include "internal/store/pakrat_txn.h"
#include "internal/store/theme_package.h"
#include "internal/storage/sources.h"
#include "internal/update/sha256.h"
#include "internal/store/pakrat_state.h"
#include "cJSON.h"
#include "miniz.h"

#include <curl/curl.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#ifdef JW_ENABLE_FAULT_INJECTION
#include <signal.h>
#endif
#include <stdbool.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static int jw__curl_ready = 0;

#define JW_PAKRAT_CONNECT_TIMEOUT_S 10L
#define JW_PAKRAT_LOW_SPEED_LIMIT_BPS 1024L
#define JW_PAKRAT_LOW_SPEED_TIME_S 30L
#define JW_PAKRAT_MAX_REDIRS 5L
/* Hard ceiling on a downloaded artifact regardless of the catalog's advertised
   size, so a lying catalog cannot fill the (writable) SD and flip it read-only. */
#define JW_PAKRAT_DOWNLOAD_ABS_MAX_BYTES (384LL * 1024LL * 1024LL)
/* Slack added over the catalog's advertised size before aborting the transfer. */
#define JW_PAKRAT_DOWNLOAD_SLACK_BYTES (64LL * 1024LL)
#define JW_PAKRAT_MUTATION_IPC_TIMEOUT_MS 30000

typedef struct {
    FILE *fp;
    curl_off_t written;
    curl_off_t ceiling;
} jw_download_sink;

static size_t jw__download_write(void *ptr, size_t size, size_t nmemb,
                                 void *userdata) {
    jw_download_sink *sink = (jw_download_sink *)userdata;
    size_t bytes = size * nmemb;
    if (size != 0 && bytes / size != nmemb) {
        return 0;
    }
    if ((curl_off_t)bytes > sink->ceiling - sink->written) {
        /* Returning short of nmemb aborts the transfer (CURLE_WRITE_ERROR). */
        return 0;
    }
    size_t got = fwrite(ptr, 1, bytes, sink->fp);
    sink->written += (curl_off_t)got;
    return got;
}

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

static void jw__configure_curl_ca(CURL *curl) {
    static const char *ca_files[] = {
        "/etc/ssl/certs/ca-certificates.crt",
        "/etc/ssl/cert.pem",
        "/etc/pki/tls/certs/ca-bundle.crt",
        "/run/libreelec/cacert.pem",
        NULL,
    };
    for (int i = 0; ca_files[i]; i++) {
        if (jw__pakrat_path_exists(ca_files[i])) {
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca_files[i]);
            return;
        }
    }
    if (jw__pakrat_path_exists("/etc/ssl/certs")) {
        curl_easy_setopt(curl, CURLOPT_CAPATH, "/etc/ssl/certs");
    }
}

static bool jw__has_suffix(const char *value, const char *suffix) {
    if (!value || !suffix) {
        return false;
    }
    size_t value_len = strlen(value);
    size_t suffix_len = strlen(suffix);
    return value_len >= suffix_len &&
           strcmp(value + value_len - suffix_len, suffix) == 0;
}

static void jw__pakrat_error(const jw_pakrat_context *ctx,
                             const char *fmt, ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    fprintf(stderr, "%s\n", message);
    if (ctx && ctx->error_message && ctx->error_message_size > 0) {
        snprintf(ctx->error_message, ctx->error_message_size, "%s", message);
    }
}

static int jw__mutation_request(const jw_pakrat_context *ctx,
                                const char *type,
                                const char *operation_id,
                                const char *target_path,
                                const char *package_id) {
    if (!ctx || !type || !operation_id || !operation_id[0] ||
        !ctx->socket_path[0] || !jw__pakrat_path_exists(ctx->socket_path)) {
        jw__pakrat_error(ctx, "Jawaka service supervisor is unavailable");
        return -1;
    }
    cJSON *request = cJSON_CreateObject();
    if (!request || !cJSON_AddStringToObject(request, "type", type) ||
        !cJSON_AddStringToObject(request, "operation_id", operation_id) ||
        (target_path &&
         !cJSON_AddStringToObject(request, "target_path", target_path)) ||
        (package_id &&
         !cJSON_AddStringToObject(request, "package_id", package_id))) {
        cJSON_Delete(request);
        jw__pakrat_error(ctx, "Could not build package mutation request");
        return -1;
    }
    char *json = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!json) {
        jw__pakrat_error(ctx, "Could not build package mutation request");
        return -1;
    }
    char *response = NULL;
    size_t response_len = 0;
    int ipc_rc = jw_ipc_request_timeout(
        ctx->socket_path, json, strlen(json), &response, &response_len,
        JW_PAKRAT_MUTATION_IPC_TIMEOUT_MS);
    cJSON_free(json);
    if (ipc_rc != 0 || !response) {
        free(response);
        jw__pakrat_error(ctx, "Jawaka did not complete package quiescence");
        return -1;
    }
    cJSON *reply = cJSON_ParseWithLength(response, response_len);
    free(response);
    cJSON *reply_type = reply
        ? cJSON_GetObjectItemCaseSensitive(reply, "type") : NULL;
    bool ok = cJSON_IsString(reply_type) && reply_type->valuestring &&
              strcmp(reply_type->valuestring, "ok") == 0;
    if (!ok) {
        cJSON *message = reply
            ? cJSON_GetObjectItemCaseSensitive(reply, "message") : NULL;
        jw__pakrat_error(
            ctx, "%s",
            cJSON_IsString(message) && message->valuestring
                ? message->valuestring
                : "Jawaka refused package mutation");
    }
    cJSON_Delete(reply);
    return ok ? 0 : -1;
}

static int jw__mutation_begin(const jw_pakrat_context *ctx,
                              const char *operation_id,
                              const char *target_path,
                              const char *package_id,
                              jw_pakrat_mutation_lock *lock) {
    char reason[JW_SVC_REASON_BUF] = {0};
    if (!ctx->runtime_dir[0] ||
        jw_pakrat_mutation_lock_acquire(
            ctx->runtime_dir, operation_id, package_id, target_path, lock,
            reason, sizeof(reason)) != 0) {
        jw__pakrat_error(ctx, "Could not lock package mutation: %s",
                         reason[0] ? reason : "runtime unavailable");
        return -1;
    }
    if (jw__mutation_request(ctx, "package-mutation-begin", operation_id,
                             target_path, package_id) != 0) {
        jw_pakrat_mutation_lock_release(lock);
        return -1;
    }
    return 0;
}

static int jw__mutation_end(const jw_pakrat_context *ctx,
                            const char *operation_id) {
    return jw__mutation_request(ctx, "package-mutation-end", operation_id,
                                NULL, NULL);
}

static int jw__cache_metadata(const char *db_path,
                              const jw_pakrat_txn_metadata *metadata) {
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }
    int rc = jw_pakrat_txn_metadata_upsert_db(db, metadata);
    jw_db_close(db);
    return rc;
}

static int jw__validate_context(const jw_pakrat_context *ctx) {
    if (!ctx || !ctx->platform[0] || !ctx->sdcard_root[0] || !ctx->state_dir[0] ||
        !ctx->db_path[0] || !ctx->platform_root[0]) {
        return -1;
    }
    /* Never create state/Apps directories on an unmounted rootfs stub. The
       storage model uses exact decoded mountinfo membership on MLP1. */
    jw_storage_source_list sources;
    const jw_storage_source *primary = NULL;
    struct stat root_st;
    char root_abs[PATH_MAX];
    if (jw_storage_sources_resolve(ctx->sdcard_root, &sources) != 0 ||
        !(primary = jw_storage_sources_primary(&sources)) ||
        !primary->available ||
        !realpath(ctx->sdcard_root, root_abs) ||
        strcmp(root_abs, primary->root_abs) != 0 ||
        stat(ctx->sdcard_root, &root_st) != 0 || !S_ISDIR(root_st.st_mode) ||
        (unsigned long long)root_st.st_dev != primary->device_id) {
        fprintf(stderr, "Pak Rat source is not mounted: %s\n", ctx->sdcard_root);
        return -1;
    }
    if (jw__pakrat_mkdir_p(ctx->state_dir, 0755) != 0) {
        fprintf(stderr, "could not create state dir: %s\n", ctx->state_dir);
        return -1;
    }
    if (!jw__pakrat_is_dir(ctx->platform_root)) {
        fprintf(stderr, "platform root missing: %s\n", ctx->platform_root);
        return -1;
    }
    return 0;
}

static int jw__random_commit_token(char out[JW_PAKRAT_COMMIT_TOKEN_BUF]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        ssize_t got = read(fd, bytes + offset, sizeof(bytes) - offset);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            close(fd);
            return -1;
        }
        offset += (size_t)got;
    }
    if (close(fd) != 0) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(bytes); i++) {
        out[i * 2u] = hex[bytes[i] >> 4];
        out[i * 2u + 1u] = hex[bytes[i] & 0x0fu];
    }
    out[JW_PAKRAT_COMMIT_TOKEN_HEX_LEN] = '\0';
    return 0;
}

static int jw__download_file(const char *url, const char *path,
                             long long expected_size, int is_dev) {
    if (jw__ensure_curl() != 0) {
        return -1;
    }
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.partial", path) >= (int)sizeof(tmp) ||
        jw__pakrat_mkdir_parent(path) != 0) {
        return -1;
    }
    /* Bound the transfer to the catalog's advertised size (plus a little slack),
       clamped to an absolute ceiling. If the size is unknown (<= 0), fall back to
       the absolute ceiling so the download is still bounded. */
    curl_off_t ceiling = JW_PAKRAT_DOWNLOAD_ABS_MAX_BYTES;
    if (expected_size > 0 &&
        expected_size <=
            JW_PAKRAT_DOWNLOAD_ABS_MAX_BYTES - JW_PAKRAT_DOWNLOAD_SLACK_BYTES) {
        ceiling = (curl_off_t)(expected_size + JW_PAKRAT_DOWNLOAD_SLACK_BYTES);
    }
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        return -1;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        unlink(tmp);
        return -1;
    }
    jw_download_sink sink = {fp, 0, ceiling};
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
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, ceiling);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, JW_PAKRAT_CONNECT_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, JW_PAKRAT_LOW_SPEED_LIMIT_BPS);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, JW_PAKRAT_LOW_SPEED_TIME_S);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, jw__download_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "jawaka-pakrat/1");
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    int close_rc = fclose(fp);
    if (rc != CURLE_OK || close_rc != 0 || (http != 0 && http >= 400)) {
        fprintf(stderr, "Pak Rat download failed: url=%s curl=%d http=%ld%s%s\n",
                url, (int)rc, http, error[0] ? " error=" : "",
                error[0] ? error : "");
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int jw__extract_zip_single_pak(const char *zip_path, const char *stage_dir,
                                      const char *install_name,
                                      char *out_pak_dir, size_t out_size) {
    if (jw__pakrat_remove_tree(stage_dir) != 0 ||
        jw__pakrat_mkdir_p(stage_dir, 0755) != 0) {
        return -1;
    }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        return -1;
    }

    char top[256] = "";
    int files = 0;
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || !st.m_is_supported ||
            !jw__pakrat_safe_rel_path(st.m_filename)) {
            mz_zip_reader_end(&zip);
            return -1;
        }

        char entry_top[256];
        const char *slash = strchr(st.m_filename, '/');
        size_t top_len = slash ? (size_t)(slash - st.m_filename) : strlen(st.m_filename);
        if (top_len == 0 || top_len >= sizeof(entry_top)) {
            mz_zip_reader_end(&zip);
            return -1;
        }
        memcpy(entry_top, st.m_filename, top_len);
        entry_top[top_len] = '\0';
        if (!jw__has_suffix(entry_top, ".pak")) {
            mz_zip_reader_end(&zip);
            return -1;
        }
        if (!top[0]) {
            jw__pakrat_copy(top, sizeof(top), entry_top);
        } else if (strcmp(top, entry_top) != 0) {
            mz_zip_reader_end(&zip);
            return -1;
        }

        char dest[PATH_MAX];
        if (jw__pakrat_join2(dest, sizeof(dest), stage_dir, st.m_filename) != 0) {
            mz_zip_reader_end(&zip);
            return -1;
        }
        if (st.m_is_directory) {
            if (jw__pakrat_mkdir_p(dest, 0755) != 0) {
                mz_zip_reader_end(&zip);
                return -1;
            }
            continue;
        }
        if (jw__pakrat_mkdir_parent(dest) != 0 ||
            !mz_zip_reader_extract_to_file(&zip, i, dest, 0)) {
            mz_zip_reader_end(&zip);
            return -1;
        }
        mode_t mode = (mode_t)((st.m_external_attr >> 16) & 0777u);
        chmod(dest, mode ? mode : 0644);
        files++;
    }

    mz_zip_reader_end(&zip);
    if (!top[0] || files <= 0 || strcmp(top, install_name) != 0) {
        return -1;
    }
    return jw__pakrat_join2(out_pak_dir, out_size, stage_dir, top);
}

static int jw__validate_runtime_manifest(const jw_pakrat_catalog_package *pkg,
                                         const char *pak_dir) {
    jw__pakrat_manifest manifest;
    char entry_point[PATH_MAX];
    if (jw__pakrat_read_manifest(pak_dir, pkg->runtime_manifest_path,
                                 &manifest) != 0 ||
        jw__pakrat_join2(entry_point, sizeof(entry_point), pak_dir,
                         "launch.sh") != 0) {
        return -1;
    }
    /* CONTENT-1's four-way table: a pak must be launchable, or contribute
       content, or both. Only "neither" is broken. Requiring launch.sh
       unconditionally would have made every pure content pak refuse to
       install. */
    if (!jw__pakrat_is_regular_file(entry_point) && !manifest.has_provides) {
        return -1;
    }
    return strcmp(manifest.platform, pkg->platform) == 0 &&
                   strcmp(manifest.pak_version, pkg->version) == 0 &&
                   strcmp(manifest.min_leaf_version, pkg->min_leaf_version) == 0
               ? 0
               : -1;
}

/* The promoted theme is the one the catalog selected: THEME-1 already held the
   archive to the catalog's id and version, so this re-reads only identity at
   the live path. */
static int jw__validate_theme_identity(const jw_pakrat_catalog_package *pkg,
                                       const char *theme_dir) {
    char id[128];
    char version[64];
    return jw__pakrat_read_theme_identity(theme_dir, id, sizeof(id), version,
                                          sizeof(version)) == 0 &&
                   strcmp(id, pkg->id) == 0 && strcmp(version, pkg->version) == 0
               ? 0
               : -1;
}

/* The folder a theme action changed, for the launcher. A name that does not
   fit is left empty rather than cut: a truncated folder name is no folder. */
static void jw__set_theme_dir(const jw_pakrat_context *ctx, const char *folder) {
    if (!ctx || !ctx->outcome || !folder) {
        return;
    }
    size_t len = strlen(folder);
    if (len < sizeof(ctx->outcome->theme_dir)) {
        memcpy(ctx->outcome->theme_dir, folder, len + 1);
    }
}

static void jw__set_refusal(const jw_pakrat_context *ctx,
                            jw_pakrat_refusal refusal) {
    if (ctx && ctx->outcome) {
        ctx->outcome->refusal = refusal;
    }
}

static int jw__notify_daemon_scan(const jw_pakrat_context *ctx) {
    if (!ctx || !ctx->socket_path[0] || !jw__pakrat_path_exists(ctx->socket_path)) {
        return 1;
    }

    const char *request = "{\"type\":\"scan-library\"}";
    char *response = NULL;
    size_t response_len = 0;
    if (jw_ipc_request(ctx->socket_path, request, strlen(request),
                       &response, &response_len) != 0) {
        free(response);
        return -1;
    }
    printf("daemon-scan: ");
    fwrite(response, 1, response_len, stdout);
    fputc('\n', stdout);
    free(response);
    return 0;
}

/* Recovery runs from the same observable state in the daemon (startup) and in
   the install/rescan paths; the full context only adds the fields recovery
   does not need (managed-app policy root, daemon socket). */
static void jw__recovery_context(const jw_pakrat_context *ctx,
                                 jw_pakrat_recovery_context *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->platform, sizeof(out->platform), "%s", ctx->platform);
    snprintf(out->sdcard_root, sizeof(out->sdcard_root), "%s", ctx->sdcard_root);
    snprintf(out->state_dir, sizeof(out->state_dir), "%s", ctx->state_dir);
    snprintf(out->db_path, sizeof(out->db_path), "%s", ctx->db_path);
}

static int jw__recover_pending_install_transitions(const jw_pakrat_context *ctx) {
    jw_pakrat_recovery_context recovery;
    jw__recovery_context(ctx, &recovery);
    return jw_pakrat_recover_installs(&recovery);
}

/* Test-only pause/crash/failure injection at promote-transaction boundaries.
   This code is compiled only into dedicated smoke binaries; production UI
   binaries contain inert stubs and cannot observe either environment variable.
   JW_PAKRAT_PAUSE_AT raises SIGSTOP before JW_PAKRAT_FAULT_AT is evaluated, so
   a device harness can prove the exact filesystem/record state and perform a
   physical card pull at a deterministic boundary. Crash points kill the
   process without cleanup, simulating power loss mid-transaction;
   "during-record" instead fails the install-record update to exercise the
   in-process failure path. */
#ifdef JW_ENABLE_FAULT_INJECTION
static int jw__fault_requested(const char *point) {
    const char *at = getenv("JW_PAKRAT_FAULT_AT");
    return at && at[0] && strcmp(at, point) == 0;
}

static void jw__fault_crash(const char *point) {
    const char *pause_at = getenv("JW_PAKRAT_PAUSE_AT");
    if (pause_at && pause_at[0] && strcmp(pause_at, point) == 0) {
        fprintf(stderr, "pakrat fault injection: paused at %s pid=%ld\n",
                point, (long)getpid());
        fflush(stderr);
        raise(SIGSTOP);
    }
    if (jw__fault_requested(point)) {
        fprintf(stderr, "pakrat fault injection: crash at %s\n", point);
        _exit(42);
    }
}
#else
static int jw__fault_requested(const char *point) {
    (void)point;
    return 0;
}

static void jw__fault_crash(const char *point) {
    (void)point;
}
#endif

static int jw__pakrat_rescan_impl(const jw_pakrat_context *ctx,
                                  int recover_pending_installs) {
    if (jw__validate_context(ctx) != 0 ||
        jw__pakrat_mkdir_parent(ctx->db_path) != 0) {
        return -1;
    }
    if (recover_pending_installs &&
        jw__recover_pending_install_transitions(ctx) != 0) {
        return -1;
    }
    /* jw_scan_library reads getenv("PLATFORM"); install/uninstall run this on a
       worker thread while the render thread is live, and POSIX setenv is not
       thread-safe against a concurrent getenv. ctx->platform is the compiled
       platform id (process-wide constant), so only write when the value is
       actually missing or wrong -- the steady-state rescan then performs no env
       mutation and races nothing. */
    const char *cur_platform = getenv("PLATFORM");
    if (!cur_platform || strcmp(cur_platform, ctx->platform) != 0) {
        setenv("PLATFORM", ctx->platform, 1);
    }
    sqlite3 *db = NULL;
    if (jw_db_open(ctx->db_path, &db) != 0 || !db) {
        return -1;
    }
    jw_scan_result result;
    int rc = jw_db_apply_schema(db) == 0 &&
             jw_scan_library(db, ctx->sdcard_root, &result) == 0 ? 0 : -1;
    jw_db_close(db);
    if (rc == 0) {
        printf("rescan: games=%d systems=%d apps=%d\n",
               result.game_count, result.system_count, result.app_count);
    }
    return rc;
}

int jw_pakrat_rescan(const jw_pakrat_context *ctx) {
    return jw__pakrat_rescan_impl(ctx, 1);
}

static int jw__begin_install_commit(const jw_pakrat_context *ctx,
                                    const char *clear_service_id,
                                    sqlite3 **out_db,
                                    jw_scan_result *out_scan) {
    if (!ctx || !out_db || !out_scan) {
        return -1;
    }
    *out_db = NULL;
    const char *cur_platform = getenv("PLATFORM");
    if (!cur_platform || strcmp(cur_platform, ctx->platform) != 0) {
        setenv("PLATFORM", ctx->platform, 1);
    }
    sqlite3 *db = NULL;
    if (jw_db_open(ctx->db_path, &db) != 0 || !db ||
        jw_db_apply_schema(db) != 0 ||
        (clear_service_id && clear_service_id[0] &&
         jw_pakrat_txn_attach_control_db(db, ctx->state_dir) != 0)) {
        jw_db_close(db);
        return -1;
    }
    /* Same reasoning as the uninstall commit: a library scan holds the write
       lock for 6-9 seconds, and jw_db_open's 2s default is sized for
       interactive writes. An install that loses that race fails outright
       having already downloaded and staged the package. */
    sqlite3_busy_timeout(db, JW_PAKRAT_COMMIT_BUSY_TIMEOUT_MS);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK ||
        jw_scan_library(db, ctx->sdcard_root, out_scan) != 0) {
        if (db) {
            (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            jw_db_close(db);
        }
        return -1;
    }
    *out_db = db;
    return 0;
}

/* A theme's commit step. The library scan an app needs says nothing about
   themes; what matters is that the launcher's own reader, with its folder cap
   and its theme.json parser, lists the promoted folder. Scanned before the
   write lock is taken: it only reads a few small files. */
static int jw__begin_theme_commit(const jw_pakrat_context *ctx,
                                  const char *card_root,
                                  const char *folder,
                                  sqlite3 **out_db) {
    *out_db = NULL;
    jw_user_theme_catalog *themes = calloc(1, sizeof(*themes));
    if (!themes) {
        return -1;
    }
    jw_user_themes_scan(themes, card_root);
    int listed = jw_user_themes_find(themes, folder) >= 0;
    free(themes);
    if (!listed) {
        jw__set_refusal(ctx, JW_PAKRAT_REFUSED_THEME_NOT_LISTED);
        jw__pakrat_error(ctx, "The launcher does not list the installed theme %s",
                         folder);
        return -1;
    }
    sqlite3 *db = NULL;
    if (jw_db_open(ctx->db_path, &db) != 0 || !db ||
        jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, JW_PAKRAT_COMMIT_BUSY_TIMEOUT_MS);
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }
    *out_db = db;
    return 0;
}

static void jw__rollback_install_commit(sqlite3 **db) {
    if (!db || !*db) {
        return;
    }
    (void)sqlite3_exec(*db, "ROLLBACK", NULL, NULL, NULL);
    jw_db_close(*db);
    *db = NULL;
}

static int jw__pakrat_install_app(const jw_pakrat_context *ctx,
                                  const char *store_id,
                                  const char *expected_version,
                                  int repair_exact,
                                  int allow_adopt) {
    if (ctx && ctx->error_message && ctx->error_message_size > 0) {
        ctx->error_message[0] = '\0';
    }
    if (ctx && ctx->outcome) {
        memset(ctx->outcome, 0, sizeof(*ctx->outcome));
    }
    int expected_parsed[3];
    if (jw__validate_context(ctx) != 0 || !store_id || !store_id[0] ||
        (expected_version && expected_version[0] &&
         jw_pak_version_parse(expected_version, expected_parsed) != 0) ||
        (repair_exact && (!expected_version || !expected_version[0]))) {
        return -1;
    }

    int rc = -1;
    char artifact_path[PATH_MAX] = "";
    char app_stage_dir[PATH_MAX] = "";
    char target_stage[PATH_MAX] = "";
    char target_rollback[PATH_MAX] = "";
    char catalog_url[1200] = "";
    char catalog_base[1024] = "";
    int catalog_is_dev = 0;
    int moved_live = 0;
    int promoted = 0;
    int committed = 0;
    int mutation_started = 0;
    int mutation_ended = 0;
    int installed_metadata_loaded = 0;
    int candidate_metadata_loaded = 0;
    int selected_downgrade = 0;
    sqlite3 *commit_db = NULL;
    char commit_token[JW_PAKRAT_COMMIT_TOKEN_BUF] = "";
    char mutation_operation[JW_PAKRAT_TXN_OPERATION_MAX + 1] = "";
    jw_pakrat_mutation_lock mutation_lock;
    memset(&mutation_lock, 0, sizeof(mutation_lock));
    mutation_lock.fd = -1;
    jw_pakrat_txn_metadata installed_metadata;
    jw_pakrat_txn_metadata candidate_metadata;
    memset(&installed_metadata, 0, sizeof(installed_metadata));
    memset(&candidate_metadata, 0, sizeof(candidate_metadata));
    jw_storage_source_list sources;
    memset(&sources, 0, sizeof(sources));
    const jw_storage_source *primary_source = NULL;
    const jw_storage_source *target_source = NULL;
    if (jw_pakrat_catalog_base_url(ctx->state_dir, catalog_base,
                                   sizeof(catalog_base),
                                   &catalog_is_dev) == 0 &&
        catalog_base[0]) {
        snprintf(catalog_url, sizeof(catalog_url), "%sstorefront.json",
                 catalog_base);
    }
    jw__pakrat_log(
        ctx->state_dir,
        "install-start store_id=%s platform=%s catalog_mode=%s catalog_url=%s target_version=%s repair=%d",
        store_id, ctx->platform, catalog_is_dev ? "dev" : "official",
        catalog_url[0] ? catalog_url : "-",
        expected_version && expected_version[0] ? expected_version : "auto",
        repair_exact);

    jw_pakrat_catalog_package pkg;
    int is_dev = 0;
    int parse_rc =
        repair_exact
            ? jw_pakrat_find_catalog_package_version(
                  ctx, store_id, expected_version, &pkg, &is_dev)
            : jw_pakrat_find_catalog_package(ctx, store_id, &pkg, &is_dev);
    if (parse_rc != 0) {
        if (parse_rc == JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF) {
            fprintf(stderr, "Pak Rat catalog requires a newer Leaf\n");
        } else if (repair_exact && parse_rc > 0) {
            fprintf(stderr,
                    "installed version %s is missing from catalog history\n",
                    expected_version);
        } else {
            fprintf(stderr, parse_rc > 0 ?
                    "Pak Rat catalog URL is not configured or store id/platform not found\n" :
                    "invalid storefront\n");
        }
        goto cleanup;
    }
    if (!repair_exact && expected_version && expected_version[0] &&
        strcmp(pkg.version, expected_version) != 0) {
        fprintf(stderr,
                "requested version %s is no longer compatible; catalog now selects %s\n",
                expected_version, pkg.version);
        goto cleanup;
    }

    bool theme = pkg.kind == JW_PAKRAT_KIND_THEME;
    if (ctx->outcome) {
        ctx->outcome->kind = pkg.kind;
    }
    if (!jw__pakrat_safe_name(pkg.install_name) ||
        (theme ? jw__has_suffix(pkg.install_name, ".pak") ||
                     strcmp(pkg.install_name, store_id) != 0 ||
                     strcmp(pkg.runtime_manifest_path,
                            JW_PAKRAT_THEME_MANIFEST) != 0 ||
                     jw_pakrat_install_path_kind(pkg.install_path) !=
                         JW_PAKRAT_KIND_THEME
               : !jw__has_suffix(pkg.install_name, ".pak") ||
                     strcmp(pkg.runtime_manifest_path, "pak.json") != 0 ||
                     jw_pakrat_install_path_kind(pkg.install_path) !=
                         JW_PAKRAT_KIND_APP) ||
        !jw__pakrat_safe_name(pkg.artifact_name) ||
        !jw_pakrat_catalog_url_allowed(pkg.artifact_url, is_dev)) {
        fprintf(stderr, "catalog package failed safety checks\n");
        goto cleanup;
    }
    if (theme) {
        jw__set_theme_dir(ctx, pkg.install_name);
        /* Withdrawn: installed copies keep working, nothing new is installed. */
        if (pkg.withdrawn) {
            jw__set_refusal(ctx, JW_PAKRAT_REFUSED_WITHDRAWN);
            jw__pakrat_error(ctx, "%s is no longer available in Pak Rat",
                             pkg.name);
            goto cleanup;
        }
        int reserved = jw_pakrat_theme_name_reserved(
            ctx->platform_root, ctx->state_dir, pkg.install_name);
        if (reserved != 0) {
            if (reserved > 0) {
                jw__set_refusal(ctx, JW_PAKRAT_REFUSED_RESERVED_NAME);
            }
            jw__pakrat_error(ctx, reserved > 0
                                      ? "Theme folder %s belongs to a theme Leaf ships"
                                      : "Could not read the bundled theme list for %s",
                             pkg.install_name);
            goto cleanup;
        }
    }

    int blocked = jw_pakrat_managed_app_path_blocked_from_platform(
        ctx->platform_root, pkg.install_path);
    if (blocked != 0) {
        fprintf(stderr, blocked > 0 ? "target path is release-managed\n" :
                "could not read managed app policy\n");
        goto cleanup;
    }

    jw_pakrat_install existing;
    int install_row = jw_db_pakrat_get_install(ctx->db_path, store_id, &existing);
    if (install_row < 0) {
        goto cleanup;
    }
    if (repair_exact &&
        (install_row != 0 ||
         strcmp(existing.version, expected_version) != 0)) {
        fprintf(stderr,
                "historical repair target does not match the owned installed version\n");
        goto cleanup;
    }
    if (!repair_exact && install_row == 0) {
        int selected_version[3];
        int installed_version[3];
        if (jw_pak_version_parse(pkg.version, selected_version) != 0 ||
            jw_pak_version_parse(existing.version, installed_version) != 0) {
            goto cleanup;
        }
        selected_downgrade =
            jw_version_cmp(selected_version, installed_version) < 0;
    }

    if (jw_storage_sources_resolve(ctx->sdcard_root, &sources) != 0 ||
        !(primary_source = jw_storage_sources_primary(&sources)) ||
        !primary_source->available) {
        jw__pakrat_error(ctx, "Pak Rat Primary source is unavailable");
        goto cleanup;
    }
    target_source = install_row == 0
        ? jw_storage_sources_find_by_id(
              &sources, existing.source_id[0] ? existing.source_id : "primary")
        : primary_source;
    if (!target_source || !target_source->available) {
        jw__pakrat_error(ctx, "Installed package source is unavailable");
        goto cleanup;
    }
    /* The launcher discovers user themes on the primary card only. */
    if (theme && target_source != primary_source) {
        jw__pakrat_error(ctx, "Installed theme is not on the primary card");
        goto cleanup;
    }

    char target[PATH_MAX];
    if (jw__pakrat_target_path(target_source->root, pkg.install_path,
                               target, sizeof(target)) != 0 ||
        jw__pakrat_target_sibling_path(target, store_id, "stage",
                                       target_stage, sizeof(target_stage)) != 0 ||
        jw__pakrat_target_sibling_path(target, store_id, "rollback",
                                       target_rollback, sizeof(target_rollback)) != 0) {
        goto cleanup;
    }
    bool target_existed = jw__pakrat_path_exists(target);
    if (target_existed && install_row != 0 && !allow_adopt) {
        jw__set_refusal(ctx, JW_PAKRAT_REFUSED_NEEDS_ADOPTION);
        fprintf(stderr, "target exists without Pak Rat ownership; adoption requires consent\n");
        goto cleanup;
    }
    if (install_row == 0 && strcmp(existing.install_path, pkg.install_path) != 0) {
        fprintf(stderr, "installed record uses another path; move is deferred\n");
        goto cleanup;
    }
    if (theme) {
        int folders = jw_pakrat_theme_folder_count(target_source->root);
        if (folders < 0) {
            jw__pakrat_error(ctx, "Could not read the Themes folder");
            goto cleanup;
        }
        if (jw_pakrat_theme_slots_full(folders, target_existed)) {
            jw__set_refusal(ctx, JW_PAKRAT_REFUSED_THEME_LIMIT);
            jw__pakrat_error(ctx,
                             "The Themes folder already holds %d themes, the most Leaf lists",
                             JW_USER_THEME_MAX);
            goto cleanup;
        }
    }

    char downloads_dir[PATH_MAX];
    char staging_dir[PATH_MAX];
    if (jw__pakrat_join3(downloads_dir, sizeof(downloads_dir), ctx->state_dir,
                         "store", "downloads") != 0 ||
        jw__pakrat_join3(staging_dir, sizeof(staging_dir), ctx->state_dir,
                         "store", "staging") != 0 ||
        jw__pakrat_mkdir_p(downloads_dir, 0755) != 0 ||
        jw__pakrat_join2(artifact_path, sizeof(artifact_path), downloads_dir,
                         pkg.artifact_name) != 0) {
        goto cleanup;
    }

    jw__pakrat_log(ctx->state_dir,
                   "download-start store_id=%s version=%s target=%s%s artifact_url=%s artifact_sha256=%s artifact_size=%lld",
                   store_id, pkg.version,
                   jw_pakrat_install_path_display_prefix(pkg.install_path),
                   pkg.install_path,
                   pkg.artifact_url, pkg.artifact_sha256, pkg.artifact_size);
    printf("download: %s\n", pkg.artifact_url);
    if (jw__download_file(pkg.artifact_url, artifact_path, pkg.artifact_size,
                          is_dev) != 0) {
        fprintf(stderr, "download failed\n");
        goto cleanup;
    }
    struct stat artifact_st;
    if (stat(artifact_path, &artifact_st) != 0 ||
        (long long)artifact_st.st_size != pkg.artifact_size) {
        jw__set_refusal(ctx, JW_PAKRAT_REFUSED_CHECKSUM);
        fprintf(stderr, "artifact size mismatch\n");
        goto cleanup;
    }
    char sha[65] = "";
    char sha_err[256];
    if (jw_sha256_file_hex(artifact_path, sha, sha_err, sizeof(sha_err)) != 0) {
        fprintf(stderr, "artifact SHA-256 unreadable: %s\n", sha_err);
        goto cleanup;
    }
    if (strcmp(sha, pkg.artifact_sha256) != 0) {
        jw__set_refusal(ctx, JW_PAKRAT_REFUSED_CHECKSUM);
        fprintf(stderr, "artifact SHA-256 mismatch\n");
        goto cleanup;
    }

    char extracted_pak[PATH_MAX];
    char metadata_reason[JW_SVC_REASON_BUF] = {0};
    if (jw__pakrat_join2(app_stage_dir, sizeof(app_stage_dir), staging_dir,
                         store_id) != 0) {
        goto cleanup;
    }
    if (theme) {
        /* THEME-1 on the downloaded archive, before anything is staged next to
           the live folder. Only allowlisted files are ever extracted. */
        jw_theme_package_result package;
        if (jw__pakrat_remove_tree(app_stage_dir) != 0 ||
            jw__pakrat_mkdir_p(app_stage_dir, 0755) != 0 ||
            jw_theme_package_validate_zip(artifact_path, app_stage_dir,
                                          &package) != 0) {
            fprintf(stderr, "artifact extraction/validation failed\n");
            goto cleanup;
        }
        int first = jw_theme_package_first_reason(&package);
        if (first >= 0) {
            jw__set_refusal(ctx, JW_PAKRAT_REFUSED_INVALID_THEME);
            if (ctx->outcome) {
                ctx->outcome->theme_reasons = package.reasons;
            }
            jw__pakrat_error(ctx, "Theme package is not valid: %s",
                             jw_theme_package_reason_slug((jw_theme_reason)first));
            goto cleanup;
        }
        if (strcmp(package.id, store_id) != 0 ||
            strcmp(package.root, pkg.install_name) != 0 ||
            strcmp(package.version, pkg.version) != 0 ||
            strcmp(package.min_leaf_version, pkg.min_leaf_version) != 0) {
            jw__pakrat_error(ctx, "Theme package does not match the catalog entry");
            goto cleanup;
        }
        if (jw__pakrat_join2(extracted_pak, sizeof(extracted_pak),
                             app_stage_dir, package.root) != 0 ||
            jw_pakrat_txn_theme_metadata(store_id, pkg.install_path,
                                         package.name,
                                         &candidate_metadata) != 0) {
            goto cleanup;
        }
    } else {
        if (jw__extract_zip_single_pak(artifact_path, app_stage_dir,
                                       pkg.install_name, extracted_pak,
                                       sizeof(extracted_pak)) != 0 ||
            jw__validate_runtime_manifest(&pkg, extracted_pak) != 0) {
            fprintf(stderr, "artifact extraction/validation failed\n");
            goto cleanup;
        }
        if (jw_pakrat_txn_inspect_manifest(
                extracted_pak, pkg.runtime_manifest_path,
                primary_source->userdata_path, store_id, pkg.install_path,
                &candidate_metadata, metadata_reason,
                sizeof(metadata_reason)) != 0) {
            jw__pakrat_error(ctx, "Package manifest is not transaction-safe: %s",
                             metadata_reason[0] ? metadata_reason : "invalid");
            goto cleanup;
        }
    }
    candidate_metadata_loaded = 1;

    if (install_row == 0) {
        int metadata_rc = jw_pakrat_txn_metadata_get(
            ctx->db_path, store_id, &installed_metadata);
        if (metadata_rc == 1) {
            metadata_reason[0] = '\0';
            if ((theme ? jw_pakrat_txn_theme_metadata(
                             store_id, existing.install_path,
                             candidate_metadata.display_name,
                             &installed_metadata)
                       : jw_pakrat_txn_inspect_manifest(
                             target, pkg.runtime_manifest_path,
                             primary_source->userdata_path, store_id,
                             existing.install_path, &installed_metadata,
                             metadata_reason, sizeof(metadata_reason))) != 0 ||
                jw__cache_metadata(ctx->db_path, &installed_metadata) != 0) {
                jw__pakrat_error(
                    ctx, "Installed package metadata cannot be validated: %s",
                    metadata_reason[0] ? metadata_reason : "cache failed");
                goto cleanup;
            }
        } else if (metadata_rc != 0) {
            jw__pakrat_error(ctx,
                             "Installed package metadata is corrupt or unavailable");
            goto cleanup;
        }
        installed_metadata_loaded = 1;
        if (strcmp(installed_metadata.store_id, store_id) != 0 ||
            strcmp(installed_metadata.package_id, store_id) != 0 ||
            strcmp(installed_metadata.install_path,
                   existing.install_path) != 0) {
            jw__pakrat_error(ctx, "Installed package metadata does not match ownership");
            goto cleanup;
        }
    }

    /* Preserve the ordinary no-downgrade rule, but allow the one transition
       required by CAT-F: when a rolled-back Leaf can no longer select the real
       service version, it may replace that owned service with the validated
       inert floor selected by the catalog. Manifest inspection has already
       proved the stable package id and the absence of a service declaration. */
    if (selected_downgrade &&
        !(installed_metadata_loaded && installed_metadata.has_service &&
          !candidate_metadata.has_service)) {
        fprintf(stderr,
                "catalog selection would downgrade the installed package\n");
        goto cleanup;
    }

    if (candidate_metadata.has_service && !target_source->primary) {
        jw__pakrat_error(
            ctx,
            "%s is installed on Secondary. Uninstall it there first, then install the service pak on Primary.",
            candidate_metadata.display_name[0]
                ? candidate_metadata.display_name : store_id);
        goto cleanup;
    }
    jw_pakrat_commit_marker commit_marker;
    memset(&commit_marker, 0, sizeof(commit_marker));
    snprintf(commit_marker.store_id, sizeof(commit_marker.store_id), "%s",
             store_id);
    snprintf(commit_marker.version, sizeof(commit_marker.version), "%s",
             pkg.version);
    snprintf(commit_marker.artifact_sha256,
             sizeof(commit_marker.artifact_sha256), "%s", sha);
    if (jw__random_commit_token(commit_token) != 0) {
        fprintf(stderr, "could not generate Pak Rat commit token\n");
        goto cleanup;
    }
    snprintf(commit_marker.token, sizeof(commit_marker.token), "%s",
             commit_token);
    /* O_EXCL in the writer is also the reserved-name check: an archive that
       supplied .pakrat-commit is rejected rather than overwritten. */
    if (jw__pakrat_write_commit_marker(extracted_pak, &commit_marker) != 0) {
        fprintf(stderr, "artifact contains or cannot write reserved commit marker\n");
        goto cleanup;
    }

    bool mutation_required = candidate_metadata.has_service ||
                             (installed_metadata_loaded &&
                              installed_metadata.has_service);
    if (mutation_required) {
        int operation_size = snprintf(
            mutation_operation, sizeof(mutation_operation), "pakrat-%s",
            commit_token);
        if (operation_size <= 0 ||
            operation_size >= (int)sizeof(mutation_operation) ||
            jw__mutation_begin(ctx, mutation_operation, pkg.install_path,
                               store_id, &mutation_lock) != 0) {
            goto cleanup;
        }
        mutation_started = 1;
    }

    jw_pakrat_recovery_context recovery;
    jw__recovery_context(ctx, &recovery);
    snprintf(recovery.sdcard_root, sizeof(recovery.sdcard_root), "%s",
             target_source->root);
    jw__fault_crash("before-stage");
    if (jw__pakrat_mkdir_parent(target) != 0 ||
        jw__pakrat_reconcile_transition(
            &recovery, store_id, pkg.install_path,
            install_row == 0 ? &existing : NULL) != 0 ||
        jw__pakrat_remove_tree(target_stage) != 0 ||
        rename(extracted_pak, target_stage) != 0) {
        fprintf(stderr, "install stage promotion failed\n");
        goto cleanup;
    }
    jw__fault_crash("after-stage");
    {
        char target_parent[PATH_MAX];
        struct stat root_st;
        struct stat stage_st;
        struct stat parent_st;
        if (jw__pakrat_copy(target_parent, sizeof(target_parent), target) != 0) {
            goto cleanup;
        }
        char *leaf = strrchr(target_parent, '/');
        if (!leaf || leaf == target_parent) {
            goto cleanup;
        }
        *leaf = '\0';
        if (stat(target_source->root, &root_st) != 0 ||
            stat(target_stage, &stage_st) != 0 ||
            stat(target_parent, &parent_st) != 0 ||
            stage_st.st_dev != root_st.st_dev ||
            parent_st.st_dev != root_st.st_dev) {
            fprintf(stderr, "install staging and target are not on one mounted source\n");
            goto cleanup;
        }
    }
    if (jw__pakrat_path_exists(target)) {
        if (jw__pakrat_remove_tree(target_rollback) != 0) {
            fprintf(stderr, "install rollback move failed\n");
            goto cleanup;
        }
        /* Write the recovery mapping before moving the live tree. An adopted
           install has no install row yet, so a crash after move-aside but
           before this marker existed would otherwise leave recovery unable to
           map the only surviving copy back to its target. */
        if (jw__pakrat_write_origin_marker(target, store_id,
                                           pkg.install_path) != 0) {
            fprintf(stderr, "install rollback origin marker failed\n");
            goto cleanup;
        }
        jw__fault_crash("after-origin-marker");
        if (rename(target, target_rollback) != 0) {
            jw__pakrat_clear_origin_marker(target, store_id);
            fprintf(stderr, "install rollback move failed\n");
            goto cleanup;
        }
        moved_live = 1;
    }
    jw__fault_crash("before-promote");
    if (rename(target_stage, target) != 0) {
        fprintf(stderr, "install target replacement failed\n");
        goto cleanup;
    }
    promoted = 1;
    jw__fault_crash("after-promote");

    /* Validate the promoted tree again at its live path, build discovery state
       under one uncommitted SQLite transaction, then durably flush package
       bytes before the token-bearing install row becomes the commit point. */
    jw_pakrat_commit_marker promoted_marker;
    jw_scan_result scan_result;
    const char *clear_service_id =
        installed_metadata_loaded && installed_metadata.has_service &&
                !candidate_metadata.has_service
            ? installed_metadata.service_id
            : NULL;
    memset(&scan_result, 0, sizeof(scan_result));
    if ((theme ? jw__validate_theme_identity(&pkg, target)
               : jw__validate_runtime_manifest(&pkg, target)) != 0 ||
        jw__pakrat_read_commit_marker(target, &promoted_marker) != 0 ||
        strcmp(promoted_marker.store_id, store_id) != 0 ||
        strcmp(promoted_marker.version, pkg.version) != 0 ||
        strcmp(promoted_marker.artifact_sha256, sha) != 0 ||
        strcmp(promoted_marker.token, commit_token) != 0 ||
        (theme ? jw__begin_theme_commit(ctx, target_source->root,
                                        pkg.install_name, &commit_db)
               : jw__begin_install_commit(ctx, clear_service_id, &commit_db,
                                          &scan_result)) != 0) {
        goto cleanup;
    }
    jw__fault_crash("before-syncfs");
    if (jw__fault_requested("during-syncfs") ||
        jw__pakrat_sync_filesystem(target) != 0) {
        fprintf(stderr, "Pak Rat install filesystem sync failed\n");
        goto cleanup;
    }
    jw__fault_crash("after-syncfs");
    jw__fault_crash("before-record");
    if (jw_db_pakrat_upsert_install_db(
            commit_db, store_id, pkg.version, pkg.platform, target_source->id,
            pkg.install_path,
            sha, NULL, commit_token) != 0 ||
        jw_pakrat_txn_metadata_upsert_db(
            commit_db, &candidate_metadata) != 0 ||
        (clear_service_id &&
         jw_pakrat_txn_clear_service_control_db(
             commit_db, clear_service_id) != 0)) {
        goto cleanup;
    }
    if (jw__fault_requested("during-record")) {
        fprintf(stderr, "pakrat fault injection: failing record update\n");
        goto cleanup;
    }
    if (sqlite3_exec(commit_db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        goto cleanup;
    }
    jw_db_close(commit_db);
    commit_db = NULL;
    committed = 1;
    rc = 0;
    if (theme && ctx->outcome) {
        ctx->outcome->themes_changed = 1;
        ctx->outcome->theme_updated = target_existed ? 1 : 0;
    }
    if (!theme) {
        printf("rescan: games=%d systems=%d apps=%d\n",
               scan_result.game_count, scan_result.system_count,
               scan_result.app_count);
    }
    jw__fault_crash("after-record");
    jw__fault_crash("before-cleanup");
    if (moved_live) {
        if (jw__pakrat_remove_tree(target_rollback) != 0) {
            /* Post-commit cleanup is retryable recovery work. Never roll back
               a transaction whose token-bearing record already committed. */
            fprintf(stderr, "install rollback cleanup deferred\n");
            jw__pakrat_log(ctx->state_dir,
                           "install-cleanup-deferred store_id=%s", store_id);
        } else {
            jw__pakrat_clear_origin_marker(target, store_id);
        }
    }
    if (mutation_started) {
        if (jw__mutation_end(ctx, mutation_operation) != 0) {
            /* The committed package remains authoritative. Releasing the
               process lock below lets jawakad adopt the still-active gate and
               finish recovery from its own database state. */
            rc = -1;
            goto cleanup;
        }
        mutation_ended = 1;
        jw_pakrat_mutation_lock_release(&mutation_lock);
    }
    /* Rescan AFTER promotion, the way uninstall already does.
     *
     * The only other scan in this function runs inside
     * jw__begin_install_commit, before the package exists at its target path.
     * That scan also enumerates content contributors and publishes an
     * effective catalog -- one that necessarily EXCLUDES the pak being
     * installed. Leaving it as the last word moves `current` backwards at the
     * moment of install: a content pak's system would vanish from the catalog
     * until some later scan happened to run, which on a device looked like
     * "installing a content pak does nothing until you reboot".
     *
     * A failure here does not fail the install. The package is committed and
     * authoritative by this point; the library is merely out of date, and the
     * daemon notification below plus the next scan will settle it. */
    if (!theme) {
        /* A theme adds nothing to the library, so neither refresh applies;
           the launcher rescans its themes from ctx->outcome. */
        if (jw_pakrat_rescan(ctx) != 0) {
            fprintf(stderr,
                    "install: package committed, but the post-install library "
                    "refresh failed; the catalog will settle on the next scan\n");
            jw__pakrat_log(ctx->state_dir,
                           "install-refresh-deferred store_id=%s", store_id);
        }
        (void)jw__notify_daemon_scan(ctx);
    }

    printf("installed: %s %s -> %s%s\n", store_id, pkg.version,
           jw_pakrat_install_path_display_prefix(pkg.install_path),
           pkg.install_path);
    jw__pakrat_log(ctx->state_dir,
                   "install-complete store_id=%s version=%s target=%s%s",
                   store_id, pkg.version,
                   jw_pakrat_install_path_display_prefix(pkg.install_path),
                   pkg.install_path);

cleanup:
    if (commit_db) {
        jw__rollback_install_commit(&commit_db);
    }
    if (rc != 0 && !committed) {
        int live_restored = 0;
        if (promoted) {
            if (moved_live) {
                if (jw__pakrat_remove_tree(target) != 0 ||
                    rename(target_rollback, target) != 0) {
                    jw__pakrat_log(ctx->state_dir, "install-rollback-restore-failed store_id=%s", store_id);
                } else {
                    live_restored = 1;
                    jw__pakrat_log(ctx->state_dir, "install-rollback-restored store_id=%s", store_id);
                }
            } else {
                (void)jw__pakrat_remove_tree(target);
            }
        } else if (moved_live) {
            if (rename(target_rollback, target) != 0) {
                jw__pakrat_log(ctx->state_dir, "install-rollback-restore-failed store_id=%s", store_id);
            } else {
                live_restored = 1;
                jw__pakrat_log(ctx->state_dir, "install-rollback-restored store_id=%s", store_id);
            }
        }
        if (live_restored) {
            jw__pakrat_clear_origin_marker(target, store_id);
        }
        (void)jw__pakrat_remove_tree(target_stage);
        if (mutation_started && !mutation_ended) {
            /* Reconcile any failed in-process rollback before asking the
               supervisor to rescan and restore the previously installed
               service state. */
            if (jw_pakrat_recover_installs(&recovery) == 0 &&
                jw__mutation_end(ctx, mutation_operation) == 0) {
                mutation_ended = 1;
            }
        }
    }
    if (rc != 0) {
        jw__pakrat_log(ctx->state_dir, "install-failed store_id=%s", store_id);
    }
    if (artifact_path[0]) {
        (void)jw__pakrat_remove_tree(artifact_path);
    }
    if (app_stage_dir[0]) {
        (void)jw__pakrat_remove_tree(app_stage_dir);
    }
    jw_pakrat_mutation_lock_release(&mutation_lock);
    if (installed_metadata_loaded) {
        jw_pakrat_txn_metadata_destroy(&installed_metadata);
    }
    if (candidate_metadata_loaded) {
        jw_pakrat_txn_metadata_destroy(&candidate_metadata);
    }
    return rc;
}

int jw_pakrat_install_app(const jw_pakrat_context *ctx, const char *store_id,
                          int allow_adopt) {
    return jw__pakrat_install_app(ctx, store_id, NULL, 0, allow_adopt);
}

int jw_pakrat_install_app_target(const jw_pakrat_context *ctx,
                                 const char *store_id,
                                 const char *expected_version,
                                 int allow_adopt) {
    return jw__pakrat_install_app(ctx, store_id, expected_version, 0,
                                  allow_adopt);
}

int jw_pakrat_repair_app_version(const jw_pakrat_context *ctx,
                                 const char *store_id,
                                 const char *version) {
    return jw__pakrat_install_app(ctx, store_id, version, 1, 0);
}

static int jw__load_owned_metadata(const jw_pakrat_context *ctx,
                                   const jw_pakrat_install *install,
                                   jw_pakrat_txn_metadata *out) {
    if (!ctx || !install || !out) {
        return -1;
    }
    int rc = jw_pakrat_txn_metadata_get(ctx->db_path, install->store_id, out);
    if (rc == 0) {
        return strcmp(out->store_id, install->store_id) == 0 &&
                       strcmp(out->package_id, install->store_id) == 0 &&
                       strcmp(out->install_path, install->install_path) == 0
                   ? 0 : -1;
    }
    if (rc < 0) {
        return -1;
    }

    /* One-time adoption of a pre-B4a install into Jawaka's validated cache.
       After this succeeds, uninstall no longer depends on the package being
       runnable, compatible, or able to answer a socket. */
    jw_storage_source_list sources;
    if (jw_storage_sources_resolve(ctx->sdcard_root, &sources) != 0) {
        return -1;
    }
    const jw_storage_source *primary = jw_storage_sources_primary(&sources);
    const jw_storage_source *source = jw_storage_sources_find_by_id(
        &sources, install->source_id[0] ? install->source_id : "primary");
    char target[PATH_MAX];
    char reason[JW_SVC_REASON_BUF] = {0};
    bool theme = jw_pakrat_install_path_kind(install->install_path) ==
                 JW_PAKRAT_KIND_THEME;
    if (!primary || !primary->available || !source || !source->available ||
        jw__pakrat_target_path(source->root, install->install_path,
                               target, sizeof(target)) != 0 ||
        (theme ? jw_pakrat_txn_theme_metadata(install->store_id,
                                              install->install_path,
                                              install->store_id, out)
               : jw_pakrat_txn_inspect_manifest(
                     target, "pak.json", primary->userdata_path,
                     install->store_id, install->install_path, out, reason,
                     sizeof(reason))) != 0 ||
        jw__cache_metadata(ctx->db_path, out) != 0) {
        jw__pakrat_error(ctx, "Installed package metadata cannot be cached: %s",
                         reason[0] ? reason : "validation failed");
        jw_pakrat_txn_metadata_destroy(out);
        return -1;
    }
    return 0;
}

int jw_pakrat_uninstall_app(const jw_pakrat_context *ctx, const char *store_id) {
    if (ctx && ctx->error_message && ctx->error_message_size > 0) {
        ctx->error_message[0] = '\0';
    }
    if (ctx && ctx->outcome) {
        memset(ctx->outcome, 0, sizeof(*ctx->outcome));
    }
    if (jw__validate_context(ctx) != 0 || !store_id || !store_id[0]) {
        return -1;
    }

    jw_pakrat_install row;
    int rc = jw_db_pakrat_get_install(ctx->db_path, store_id, &row);
    if (rc > 0) {
        printf("not installed: %s\n", store_id);
        return 0;
    }
    if (rc < 0) {
        return -1;
    }
    jw_pakrat_kind kind = jw_pakrat_install_path_kind(row.install_path);
    if (strcmp(row.kind, jw_pakrat_kind_name(kind)) != 0) {
        jw__pakrat_error(ctx, "Pak Rat ownership record is inconsistent: %s",
                         store_id);
        return -1;
    }
    if (ctx->outcome) {
        ctx->outcome->kind = kind;
    }

    int blocked = jw_pakrat_managed_app_path_blocked_from_platform(
        ctx->platform_root, row.install_path);
    if (blocked != 0) {
        fprintf(stderr, blocked > 0 ? "target path is release-managed\n" :
                "could not read managed app policy\n");
        return -1;
    }

    jw_pakrat_txn_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    if (jw__load_owned_metadata(ctx, &row, &metadata) != 0) {
        return -1;
    }

    int result = -1;
    bool mutation_started = false;
    bool pending_written = false;
    char operation_id[JW_PAKRAT_TXN_OPERATION_MAX + 1] = {0};
    jw_pakrat_mutation_lock lock;
    memset(&lock, 0, sizeof(lock));
    lock.fd = -1;
    if (metadata.has_service) {
        char token[JW_PAKRAT_COMMIT_TOKEN_BUF];
        int operation_size =
            jw__random_commit_token(token) == 0
                ? snprintf(operation_id, sizeof(operation_id),
                           "uninstall-%s", token)
                : -1;
        if (operation_size <= 0 ||
            operation_size >= (int)sizeof(operation_id) ||
            jw__mutation_begin(ctx, operation_id, metadata.install_path,
                               metadata.package_id, &lock) != 0) {
            goto uninstall_done;
        }
        mutation_started = true;
    }

    if (jw_pakrat_txn_pending_persist(ctx->db_path, row.source_id,
                                      &metadata) != 0) {
        jw__pakrat_error(ctx, "Could not record confirmed uninstall intent");
        goto uninstall_done;
    }
    pending_written = true;
    jw_pakrat_pending_uninstall pending;
    int pending_rc = jw_pakrat_txn_pending_get(ctx->db_path, store_id,
                                               &pending);
    if (pending_rc != 0) {
        jw__pakrat_error(ctx, "Could not reload confirmed uninstall intent");
        goto uninstall_done;
    }
    int complete_rc = jw_pakrat_txn_complete_uninstall(ctx, &pending);
    jw_pakrat_pending_uninstall_destroy(&pending);
    if (complete_rc != 0) {
        jw__pakrat_error(
            ctx, "%s",
            complete_rc > 0 ? "Installed package source is absent; uninstall will resume when it returns"
                            : "Confirmed uninstall could not be completed");
        goto uninstall_done;
    }
    pending_written = false;
    if (mutation_started && jw__mutation_end(ctx, operation_id) != 0) {
        goto uninstall_done;
    }
    if (mutation_started) {
        jw_pakrat_mutation_lock_release(&lock);
        mutation_started = false;
    }
    if (kind == JW_PAKRAT_KIND_THEME) {
        if (ctx->outcome) {
            ctx->outcome->themes_changed = 1;
        }
        jw__set_theme_dir(ctx, row.install_path +
                                   sizeof(JW_PAKRAT_THEME_PATH_PREFIX) - 1);
        printf("uninstalled theme: %s\n", store_id);
        result = 0;
        goto uninstall_done;
    }
    if (jw_pakrat_rescan(ctx) != 0) {
        jw__pakrat_error(ctx, "Package was removed, but library refresh failed");
        goto uninstall_done;
    }
    (void)jw__notify_daemon_scan(ctx);

    printf("uninstalled: %s (userdata preserved)\n", store_id);
    result = 0;

uninstall_done:
    if (mutation_started && !pending_written) {
        /* Intent was not made durable (or removal already committed), so an
           immediate release is safe to attempt. On an ambiguous IPC failure,
           dropping the flock delegates the still-active gate to jawakad. */
        (void)jw__mutation_end(ctx, operation_id);
    }
    jw_pakrat_mutation_lock_release(&lock);
    jw_pakrat_txn_metadata_destroy(&metadata);
    return result;
}

int jw_pakrat_get_uninstall_info(const jw_pakrat_context *ctx,
                                 const char *store_id,
                                 jw_pakrat_uninstall_info *out) {
    if (!ctx || !store_id || !store_id[0] || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    jw_pakrat_txn_metadata metadata;
    int rc = jw_pakrat_txn_metadata_get(ctx->db_path, store_id, &metadata);
    if (rc == 1) {
        jw_pakrat_install install;
        int install_rc = jw_db_pakrat_get_install(ctx->db_path, store_id,
                                                  &install);
        if (install_rc != 0) {
            return install_rc;
        }
        rc = jw__load_owned_metadata(ctx, &install, &metadata);
    }
    if (rc != 0) {
        return rc;
    }
    rc = jw_pakrat_txn_inventory_retained(ctx, &metadata, out);
    jw_pakrat_txn_metadata_destroy(&metadata);
    return rc;
}

void jw_pakrat_free_uninstall_info(jw_pakrat_uninstall_info *info) {
    jw_pakrat_uninstall_info_destroy(info);
}

int jw_pakrat_remove_retained_data(const jw_pakrat_context *ctx,
                                   const char *store_id) {
    if (!ctx || !store_id || !store_id[0]) {
        return -1;
    }
    jw_pakrat_txn_metadata metadata;
    int rc = jw_pakrat_txn_metadata_get(ctx->db_path, store_id, &metadata);
    if (rc != 0) {
        return rc;
    }
    rc = jw_pakrat_txn_remove_retained(ctx, &metadata);
    jw_pakrat_txn_metadata_destroy(&metadata);
    return rc;
}

static bool jw__preview_sha_valid(const char *sha256) {
    if (!sha256 || strlen(sha256) != 64) {
        return false;
    }
    for (const char *p = sha256; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) {
            return false;
        }
    }
    return true;
}

/* A cached preview is named <sha256>.png or <sha256>.jpg; anything else in the
   folder is a download that never finished. */
static bool jw__preview_cache_name(const char *name) {
    size_t len = strlen(name);
    if (len != 68 || (strcmp(name + 64, ".png") != 0 &&
                      strcmp(name + 64, ".jpg") != 0)) {
        return false;
    }
    char sha[65];
    memcpy(sha, name, 64);
    sha[64] = '\0';
    return jw__preview_sha_valid(sha);
}

/* A cached copy is trusted only while it still hashes to its name. */
static bool jw__preview_cached_ok(const char *path, const char *sha256,
                                  long long size) {
    struct stat st;
    char sha[65] = "";
    char err[256];
    int w = 0, h = 0;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           (long long)st.st_size == size &&
           jw_sha256_file_hex(path, sha, err, sizeof(err)) == 0 &&
           strcmp(sha, sha256) == 0 &&
           jw_user_theme_image_dims(path, &w, &h);
}

/* Keep the newest JW_PAKRAT_PREVIEW_CACHE_MAX previews, and drop unfinished
   downloads a launcher exit left behind. Best effort: a failure only means the
   folder stays larger than it should. */
typedef struct {
    char name[72];
    time_t mtime;
} jw__preview_entry;

static int jw__preview_newest_first(const void *a, const void *b) {
    time_t ta = ((const jw__preview_entry *)a)->mtime;
    time_t tb = ((const jw__preview_entry *)b)->mtime;
    return ta < tb ? 1 : ta > tb ? -1 : 0;
}

static void jw__preview_prune(const char *dir, const char *keep) {
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    jw__preview_entry *entries = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        char path[PATH_MAX];
        struct stat st;
        if (entry->d_name[0] == '.' || strcmp(entry->d_name, keep) == 0 ||
            jw__pakrat_join2(path, sizeof(path), dir, entry->d_name) != 0 ||
            stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (!jw__preview_cache_name(entry->d_name)) {
            (void)unlink(path);
            continue;
        }
        if (count == capacity) {
            size_t grown = capacity ? capacity * 2 : JW_PAKRAT_PREVIEW_CACHE_MAX;
            jw__preview_entry *next = realloc(entries, grown * sizeof(*entries));
            if (!next) {
                break;
            }
            entries = next;
            capacity = grown;
        }
        snprintf(entries[count].name, sizeof(entries[count].name), "%s",
                 entry->d_name);
        entries[count].mtime = st.st_mtime;
        count++;
    }
    closedir(d);
    /* `keep` is the preview just stored, and counts against the cap. */
    if (count > (size_t)JW_PAKRAT_PREVIEW_CACHE_MAX - 1) {
        qsort(entries, count, sizeof(*entries), jw__preview_newest_first);
        for (size_t i = JW_PAKRAT_PREVIEW_CACHE_MAX - 1; i < count; i++) {
            char path[PATH_MAX];
            if (jw__pakrat_join2(path, sizeof(path), dir, entries[i].name) == 0) {
                (void)unlink(path);
            }
        }
    }
    free(entries);
}

int jw_pakrat_fetch_preview(const char *state_dir, const char *url,
                            const char *sha256, long long size,
                            char *out_path, size_t out_size) {
    if (out_path && out_size > 0) {
        out_path[0] = '\0';
    }
    char base[1024] = "";
    int is_dev = 0;
    if (!state_dir || !state_dir[0] || !url || !url[0] || !out_path ||
        out_size == 0 || !jw__preview_sha_valid(sha256) || size <= 0 ||
        size > JW_PAKRAT_PREVIEW_MAX_BYTES ||
        jw_pakrat_catalog_base_url(state_dir, base, sizeof(base), &is_dev) != 0 ||
        !jw_pakrat_catalog_url_allowed(url, is_dev)) {
        return -1;
    }
    char dir[PATH_MAX];
    if (jw__pakrat_join3(dir, sizeof(dir), state_dir, "store", "previews") != 0 ||
        jw__pakrat_mkdir_p(dir, 0755) != 0) {
        return -1;
    }

    static const char *const exts[] = { ".png", ".jpg" };
    char path[PATH_MAX];
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        if (snprintf(path, sizeof(path), "%s/%s%s", dir, sha256, exts[i]) >=
            (int)sizeof(path)) {
            return -1;
        }
        if (!jw__pakrat_path_exists(path)) {
            continue;
        }
        if (jw__preview_cached_ok(path, sha256, size)) {
            (void)utimes(path, NULL);   /* most recently used */
            return jw__pakrat_copy(out_path, out_size, path);
        }
        (void)unlink(path);
    }

    char download[PATH_MAX];
    if (snprintf(download, sizeof(download), "%s/%s.download", dir, sha256) >=
            (int)sizeof(download) ||
        jw__download_file(url, download, size, is_dev) != 0) {
        return -1;
    }
    struct stat st;
    char sha[65] = "";
    char err[256];
    int w = 0, h = 0;
    bool ok = stat(download, &st) == 0 && (long long)st.st_size == size &&
              jw_sha256_file_hex(download, sha, err, sizeof(err)) == 0 &&
              strcmp(sha, sha256) == 0 &&
              jw_user_theme_image_dims(download, &w, &h) && w > 0 && h > 0 &&
              w <= JW_PAKRAT_PREVIEW_MAX_PX && h <= JW_PAKRAT_PREVIEW_MAX_PX;
    /* image_dims told PNG from JPEG by their bytes; the name records which. */
    int png_w = 0, png_h = 0;
    const char *ext = jw_user_theme_png_dims(download, &png_w, &png_h)
                          ? ".png" : ".jpg";
    if (!ok ||
        snprintf(path, sizeof(path), "%s/%s%s", dir, sha256, ext) >=
            (int)sizeof(path) ||
        rename(download, path) != 0) {
        fprintf(stderr, "Pak Rat preview refused: url=%s (%s)\n", url,
                ok ? "could not store it" : "size, checksum or image header");
        (void)unlink(download);
        return -1;
    }
    jw__preview_prune(dir, strrchr(path, '/') + 1);
    return jw__pakrat_copy(out_path, out_size, path);
}
