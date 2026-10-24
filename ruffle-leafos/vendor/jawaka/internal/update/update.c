#include "internal/update/update.h"
#include "internal/update/sha256.h"
#include "internal/platform/leaf_version.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef JW_UPDATE_USE_LIBCURL
#include <curl/curl.h>
#endif

#define JW_UPDATE_INSTALLED_SCHEMA 1
#define JW_UPDATE_MANIFEST_MAX_BYTES (1024 * 1024)
/* Stable and Beta live in separate repos; the channel just selects the source.
   Beta builds are published as regular (non-prerelease) releases in Leaf-beta,
   so the prerelease-skipping selection logic below needs no channel awareness. */
#define JW_UPDATE_RELEASES_URL_STABLE "https://api.github.com/repos/Utility-Muffin-Research-Kitchen/Leaf/releases?per_page=20"
#define JW_UPDATE_RELEASES_URL_BETA   "https://api.github.com/repos/Utility-Muffin-Research-Kitchen/Leaf-beta/releases?per_page=20"
#define JW_UPDATE_USER_AGENT "Leaf-Jawaka-Updater/1"
#define JW_UPDATE_MIN_DOWNLOAD_FREE_BYTES (64LL * 1024LL * 1024LL)
#define JW_UPDATE_INSTALL_HEADROOM_BYTES (64LL * 1024LL * 1024LL)
#define JW_UPDATE_MIN_INSTALL_BATTERY_PERCENT 29
#define JW_UPDATE_MLP1_STOCK_UPDATE_ROOT "/mnt/sdcard"

static void jw__copy_string(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0) {
        return;
    }
    if (!value) {
        value = "";
    }
    size_t n = strlen(value);
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy(out, value, n);
    out[n] = '\0';
}

static void jw__set_message(jw_update_status *status, const char *fmt, ...) {
    if (!status) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(status->message, sizeof(status->message), fmt ? fmt : "", args);
    va_end(args);
}

static void jw__set_install_message(jw_update_status *status,
                                    const char *reason,
                                    const char *fmt,
                                    ...) {
    if (!status) {
        return;
    }
    jw__copy_string(status->install_reason, sizeof(status->install_reason),
                    reason ? reason : "");

    va_list args;
    va_start(args, fmt);
    vsnprintf(status->install_message, sizeof(status->install_message),
              fmt ? fmt : "", args);
    va_end(args);
}

static void jw__clear_install_preflight(jw_update_status *status) {
    if (!status) {
        return;
    }

    status->install_ready = false;
    status->install_blocked = false;
    status->install_needs_confirmation = false;
    status->install_idle = false;
    status->install_battery_percent = -1;
    status->install_charging = -1;
    status->install_required_free = 0;
    status->install_available_free = 0;
    status->install_reason[0] = '\0';
    status->install_message[0] = '\0';
}

static void jw__clear_candidate(jw_update_status *status) {
    if (!status) {
        return;
    }

    status->compatible = false;
    status->has_update = false;
    status->release_id[0] = '\0';
    status->version[0] = '\0';
    status->published_at[0] = '\0';
    status->notes_url[0] = '\0';
    status->source_manifest[0] = '\0';
    status->manifest_url[0] = '\0';
    status->artifact_kind[0] = '\0';
    status->artifact_name[0] = '\0';
    status->artifact_url[0] = '\0';
    status->artifact_sha256[0] = '\0';
    status->artifact_size = 0;
    status->installed_size = 0;
    status->downloaded = false;
    status->download_active = false;
    status->download_received = 0;
    status->download_total = 0;
    status->download_percent = -1;
    status->download_path[0] = '\0';
    status->recovery_name[0] = '\0';
    status->recovery_url[0] = '\0';
    status->handoff_type[0] = '\0';
    status->handoff_completion[0] = '\0';
    status->handoff_trigger_file[0] = '\0';
    status->managed_apps_count = 0;
    status->migrations_count = 0;
    status->install_active = false;
    status->install_armed = false;
    jw__clear_install_preflight(status);
}

static void jw__clear_options(jw_update_status *status) {
    if (!status) {
        return;
    }
    status->options_complete = false;
    status->option_count = 0;
    status->selected_option = -1;
    memset(status->options, 0, sizeof(status->options));
}

static void jw__copy_status_to_option(jw_update_status *status,
                                      jw_update_option *option,
                                      int index);

static bool jw__join_path(char *out, size_t out_size,
                          const char *dir, const char *name) {
    if (!out || out_size == 0 || !dir || !dir[0] || !name || !name[0]) {
        return false;
    }

    int needed = snprintf(out, out_size, "%s/%s", dir, name);
    return needed >= 0 && (size_t)needed < out_size;
}

static bool jw__file_exists(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0;
}

static int jw__mkdir_if_needed(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int jw__mkdir_p(const char *path, mode_t mode) {
    if (!path || !path[0]) {
        return -1;
    }

    char buf[PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) {
        return -1;
    }

    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (buf[0] && jw__mkdir_if_needed(buf, mode) != 0) {
            return -1;
        }
        *p = '/';
    }

    return jw__mkdir_if_needed(buf, mode);
}

static bool jw__update_dir(char *out, size_t out_size, const char *state_dir) {
    if (!jw__join_path(out, out_size, state_dir, "update")) {
        return false;
    }
    return jw__mkdir_p(out, 0755) == 0;
}

static bool jw__filename_safe(const char *name) {
    if (!name || !name[0] || strlen(name) >= JW_UPDATE_NAME_MAX) {
        return false;
    }
    if (strstr(name, "..")) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == '/' || *p == '\\') {
            return false;
        }
    }
    return true;
}

static bool jw__https_url_safe(const char *url) {
    return url && strncmp(url, "https://", 8) == 0 && strlen(url) < JW_UPDATE_URL_MAX;
}

static bool jw__ascii_equal_ci(const char *a, const char *b) {
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return false;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* A transfer that averages under JW_UPDATE_STALL_BYTES_PER_SEC for the stall
   window is dead. The connect timeout only covers the handshake, so without this
   a Wi-Fi drop mid-response blocks the release check (or a download) forever,
   and the daemon refuses every later check while that job is still "running". */
#define JW_UPDATE_STALL_BYTES_PER_SEC 1L
#define JW_UPDATE_STALL_SECONDS 30L
/* Release metadata is small, so its fetches also get a hard cap: a connection
   that trickles bytes can't hold the check open either. Artifact downloads have
   no total cap; they can be cancelled. */
#define JW_UPDATE_METADATA_TIMEOUT_SECONDS 60L
#define JW_UPDATE_FETCH_MAX_CONNECTIONS 6L

/* JAWAKA_UPDATE_STALL_SECONDS shortens the stall window for tests. */
static long jw__stall_seconds(void) {
    const char *value = getenv("JAWAKA_UPDATE_STALL_SECONDS");
    if (value && value[0]) {
        char *end = NULL;
        long seconds = strtol(value, &end, 10);
        if (end && *end == '\0' && seconds > 0) {
            return seconds;
        }
    }
    return JW_UPDATE_STALL_SECONDS;
}

#ifdef JW_UPDATE_USE_LIBCURL
static bool jw__env_truthy(const char *name) {
    const char *value = getenv(name);
    return value && value[0] &&
           !jw__ascii_equal_ci(value, "0") &&
           !jw__ascii_equal_ci(value, "false") &&
           !jw__ascii_equal_ci(value, "no") &&
           !jw__ascii_equal_ci(value, "off");
}

static struct curl_slist *jw__curl_accept_header(const char *accept) {
    char accept_header[160];
    snprintf(accept_header, sizeof(accept_header), "Accept: %s",
             accept && accept[0] ? accept : "application/octet-stream");
    return curl_slist_append(NULL, accept_header);
}

static CURLcode jw__curl_apply_options(CURL *easy,
                                       const char *url,
                                       struct curl_slist *headers,
                                       char *error_buffer,
                                       long total_timeout) {
    CURLcode rc = CURLE_OK;
#define JW_CURL_TRY(option, value) \
    do { \
        rc = curl_easy_setopt(easy, option, value); \
        if (rc != CURLE_OK) { \
            return rc; \
        } \
    } while (0)

    JW_CURL_TRY(CURLOPT_URL, url);
    JW_CURL_TRY(CURLOPT_ERRORBUFFER, error_buffer);
    JW_CURL_TRY(CURLOPT_HTTPHEADER, headers);
    JW_CURL_TRY(CURLOPT_USERAGENT, JW_UPDATE_USER_AGENT);
    JW_CURL_TRY(CURLOPT_FAILONERROR, 1L);
    JW_CURL_TRY(CURLOPT_FOLLOWLOCATION, 1L);
    JW_CURL_TRY(CURLOPT_CONNECTTIMEOUT, 15L);
    JW_CURL_TRY(CURLOPT_LOW_SPEED_LIMIT, JW_UPDATE_STALL_BYTES_PER_SEC);
    JW_CURL_TRY(CURLOPT_LOW_SPEED_TIME, jw__stall_seconds());
    if (total_timeout > 0) {
        JW_CURL_TRY(CURLOPT_TIMEOUT, total_timeout);
    }
    JW_CURL_TRY(CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    JW_CURL_TRY(CURLOPT_PROTOCOLS_STR, "https");
    JW_CURL_TRY(CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
    JW_CURL_TRY(CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    JW_CURL_TRY(CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif

    /* Verify TLS against the launcher's bundled Mozilla CA roots when present.
       Relying on libcurl's compiled-in default is not safe here: some stock
       LoongOS libcurl builds default to a CA path that does not exist on the
       device (/run/libreelec/cacert.pem), which fails every HTTPS request. The
       bundled file makes verification independent of the firmware's CA store. */
    {
        const char *bundle = getenv("UMRK_LAUNCHER_PATH");
        char ca_path[PATH_MAX];
        if (bundle && bundle[0] &&
            jw__join_path(ca_path, sizeof(ca_path), bundle, "res/certs/cacert.pem") &&
            jw__file_exists(ca_path)) {
            JW_CURL_TRY(CURLOPT_CAINFO, ca_path);
        }
    }

    /* Developer escape hatch only; normal builds verify TLS. */
    if (jw__env_truthy("JAWAKA_UPDATE_INSECURE_TLS")) {
        JW_CURL_TRY(CURLOPT_SSL_VERIFYPEER, 0L);
        JW_CURL_TRY(CURLOPT_SSL_VERIFYHOST, 0L);
    }

#undef JW_CURL_TRY
    return CURLE_OK;
}

static void jw__curl_failure(char *error, size_t error_size,
                             CURLcode rc, const char *curl_error) {
    if (!error || error_size == 0) {
        return;
    }
    const char *msg = curl_easy_strerror(rc);
    snprintf(error, error_size, "libcurl download failed: %s",
             curl_error && curl_error[0] ? curl_error :
             (msg && msg[0] ? msg : "unknown error"));
}
#endif

static int jw__download_with_libcurl(const char *url,
                                     const char *out_path,
                                     const char *accept,
                                     long total_timeout,
                                     char *error,
                                     size_t error_size) {
    if (error && error_size > 0) {
        error[0] = '\0';
    }

#ifndef JW_UPDATE_USE_LIBCURL
    (void)url;
    (void)out_path;
    (void)accept;
    (void)total_timeout;
    if (error && error_size > 0) {
        snprintf(error, error_size, "%s", "libcurl support is not built in");
    }
    return -2;
#else
    int result = -1;
    bool global_started = false;
    CURL *easy = NULL;
    struct curl_slist *headers = NULL;
    FILE *fp = NULL;
    char curl_error[CURL_ERROR_SIZE];
    curl_error[0] = '\0';

    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "libcurl global init failed: %s",
                     curl_easy_strerror(rc));
        }
        goto cleanup;
    }
    global_started = true;

    easy = curl_easy_init();
    if (!easy) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", "libcurl easy init failed");
        }
        goto cleanup;
    }

    fp = fopen(out_path, "wb");
    if (!fp) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "cannot open download output: %s",
                     strerror(errno));
        }
        goto cleanup;
    }

    headers = jw__curl_accept_header(accept);
    if (!headers) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", "cannot allocate libcurl header list");
        }
        goto cleanup;
    }

    rc = jw__curl_apply_options(easy, url, headers, curl_error, total_timeout);
    if (rc == CURLE_OK) {
        rc = curl_easy_setopt(easy, CURLOPT_WRITEDATA, fp);
    }
    if (rc != CURLE_OK) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "libcurl option failed: %s",
                     curl_easy_strerror(rc));
        }
        goto cleanup;
    }

    rc = curl_easy_perform(easy);
    if (rc != CURLE_OK) {
        jw__curl_failure(error, error_size, rc, curl_error);
        goto cleanup;
    }

    if (fflush(fp) != 0 || fclose(fp) != 0) {
        fp = NULL;
        if (error && error_size > 0) {
            snprintf(error, error_size, "cannot finish download output: %s",
                     strerror(errno));
        }
        goto cleanup;
    }
    fp = NULL;
    result = 0;

cleanup:
    if (fp) {
        fclose(fp);
    }
    if (headers) {
        curl_slist_free_all(headers);
    }
    if (easy) {
        curl_easy_cleanup(easy);
    }
    if (global_started) {
        curl_global_cleanup();
    }
    return result;
#endif
}

static pid_t jw__spawn_https_to_file(const char *url,
                                     const char *out_path,
                                     const char *accept,
                                     long total_timeout,
                                     char *error,
                                     size_t error_size) {
    if (error && error_size > 0) {
        error[0] = '\0';
    }
    if (!jw__https_url_safe(url) || !out_path || !out_path[0]) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", "invalid HTTPS download request");
        }
        return -1;
    }

    char accept_header[160];
    snprintf(accept_header, sizeof(accept_header), "Accept: %s",
             accept && accept[0] ? accept : "application/octet-stream");
    char user_agent_header[160];
    snprintf(user_agent_header, sizeof(user_agent_header), "User-Agent: %s",
             JW_UPDATE_USER_AGENT);
    char speed_limit[24];
    snprintf(speed_limit, sizeof(speed_limit), "%ld", JW_UPDATE_STALL_BYTES_PER_SEC);
    char speed_time[24];
    snprintf(speed_time, sizeof(speed_time), "%ld", jw__stall_seconds());
    char max_time[24];
    snprintf(max_time, sizeof(max_time), "%ld", total_timeout);

    const char *argv[32];
    int argc = 0;
    argv[argc++] = "curl";
    argv[argc++] = "--fail";
    argv[argc++] = "--location";
    argv[argc++] = "--silent";
    argv[argc++] = "--show-error";
    argv[argc++] = "--proto";
    argv[argc++] = "=https";
    argv[argc++] = "--tlsv1.2";
    argv[argc++] = "--retry";
    argv[argc++] = "2";
    argv[argc++] = "--connect-timeout";
    argv[argc++] = "15";
    argv[argc++] = "--speed-limit";
    argv[argc++] = speed_limit;
    argv[argc++] = "--speed-time";
    argv[argc++] = speed_time;
    if (total_timeout > 0) {
        argv[argc++] = "--max-time";
        argv[argc++] = max_time;
    }
    argv[argc++] = "-H";
    argv[argc++] = accept_header;
    argv[argc++] = "-H";
    argv[argc++] = user_agent_header;
    argv[argc++] = "-o";
    argv[argc++] = out_path;
    argv[argc++] = url;
    argv[argc] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "cannot start HTTPS downloader: %s",
                     strerror(errno));
        }
        return -1;
    }
    if (pid == 0) {
        char child_error[256];
        int libcurl_rc = jw__download_with_libcurl(url, out_path, accept,
                                                   total_timeout,
                                                   child_error,
                                                   sizeof(child_error));
        if (libcurl_rc == 0) {
            _exit(0);
        }
        if (libcurl_rc != -2 && child_error[0]) {
            fprintf(stderr, "jawaka-update: %s\n", child_error);
        }
        execvp("curl", (char *const *)argv);
        _exit(libcurl_rc == -2 ? 127 : 126);
    }

    return pid;
}

static int jw__fetch_https_to_file(const char *url,
                                   const char *out_path,
                                   const char *accept,
                                   long total_timeout,
                                   char *error,
                                   size_t error_size) {
    if (error && error_size > 0) {
        error[0] = '\0';
    }
    if (!jw__https_url_safe(url) || !out_path || !out_path[0]) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", "invalid HTTPS download request");
        }
        return -1;
    }

    char tmp_path[PATH_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%ld",
                 out_path, (long)getpid()) >= (int)sizeof(tmp_path)) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", "download path is too long");
        }
        return -1;
    }

    int direct_rc = jw__download_with_libcurl(url, tmp_path, accept, total_timeout,
                                              error, error_size);
    if (direct_rc == 0) {
        goto move_download;
    }
    unlink(tmp_path);
    if (direct_rc != -2) {
        return -1;
    }

    pid_t pid = jw__spawn_https_to_file(url, tmp_path, accept, total_timeout,
                                        error, error_size);
    if (pid < 0) {
        return -1;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            unlink(tmp_path);
            if (error && error_size > 0) {
                snprintf(error, error_size, "HTTPS downloader wait failed: %s",
                         strerror(errno));
            }
            return -1;
        }
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(tmp_path);
        if (error && error_size > 0) {
            snprintf(error, error_size, "HTTPS download failed for %s", url);
        }
        return -1;
    }

move_download:
    if (rename(tmp_path, out_path) != 0) {
        unlink(tmp_path);
        if (error && error_size > 0) {
            snprintf(error, error_size, "cannot move download into place: %s",
                     strerror(errno));
        }
        return -1;
    }
    return 0;
}

static char *jw__read_text_file(const char *path, char *error, size_t error_size) {
    if (error && error_size > 0) {
        error[0] = '\0';
    }
    if (!path || !path[0]) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", "missing file path");
        }
        return NULL;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "cannot stat %s: %s", path, strerror(errno));
        }
        return NULL;
    }
    if (!S_ISREG(st.st_mode)) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "not a regular file: %s", path);
        }
        return NULL;
    }
    if (st.st_size < 0 || st.st_size > JW_UPDATE_MANIFEST_MAX_BYTES) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "file too large: %s", path);
        }
        return NULL;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "cannot open %s: %s", path, strerror(errno));
        }
        return NULL;
    }

    size_t len = (size_t)st.st_size;
    char *buf = (char *)malloc(len + 1u);
    if (!buf) {
        fclose(fp);
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", "out of memory");
        }
        return NULL;
    }

    size_t got = fread(buf, 1, len, fp);
    int read_error = ferror(fp);
    fclose(fp);
    if (got != len || read_error) {
        free(buf);
        if (error && error_size > 0) {
            snprintf(error, error_size, "cannot read %s", path);
        }
        return NULL;
    }

    buf[len] = '\0';
    return buf;
}

/* One small HTTPS resource fetched into memory: a release list or a manifest. */
typedef struct {
    const char *url;
    char *data;   /* NUL-terminated body when ok; release with jw__fetch_item_free */
    size_t size;
    bool ok;
    char error[256];
} jw__fetch_item;

/* Fetches release metadata. With libcurl, one multi handle lives for the whole
   check: the transfers in a batch run concurrently, and its connection cache
   keeps the GitHub and asset-CDN connections open between batches, so every
   manifest after the first skips the TCP and TLS handshakes. Without libcurl,
   each item goes through the curl CLI in turn via a scratch file. */
typedef struct {
#ifdef JW_UPDATE_USE_LIBCURL
    CURLM *multi;
    bool global_started;
#endif
    char scratch_path[PATH_MAX];
} jw__fetcher;

static void jw__fetch_item_free(jw__fetch_item *item) {
    if (!item) {
        return;
    }
    free(item->data);
    item->data = NULL;
    item->size = 0;
    item->ok = false;
}

static void jw__fetcher_close(jw__fetcher *fetcher) {
    if (!fetcher) {
        return;
    }
#ifdef JW_UPDATE_USE_LIBCURL
    if (fetcher->multi) {
        curl_multi_cleanup(fetcher->multi);
        fetcher->multi = NULL;
    }
    if (fetcher->global_started) {
        curl_global_cleanup();
        fetcher->global_started = false;
    }
#endif
}

static int jw__fetcher_open(jw__fetcher *fetcher,
                            const char *update_dir,
                            char *error,
                            size_t error_size) {
    if (error && error_size > 0) {
        error[0] = '\0';
    }
    memset(fetcher, 0, sizeof(*fetcher));
    int needed = snprintf(fetcher->scratch_path, sizeof(fetcher->scratch_path),
                          "%s/.metadata.%ld", update_dir, (long)getpid());
    if (needed < 0 || needed >= (int)sizeof(fetcher->scratch_path)) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", "Update metadata path is too long");
        }
        return -1;
    }

#ifdef JW_UPDATE_USE_LIBCURL
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "libcurl global init failed: %s",
                     curl_easy_strerror(rc));
        }
        return -1;
    }
    fetcher->global_started = true;

    fetcher->multi = curl_multi_init();
    if (!fetcher->multi) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "%s", "libcurl multi init failed");
        }
        jw__fetcher_close(fetcher);
        return -1;
    }
    curl_multi_setopt(fetcher->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                      JW_UPDATE_FETCH_MAX_CONNECTIONS);
#endif
    return 0;
}

#ifdef JW_UPDATE_USE_LIBCURL
static size_t jw__fetch_item_write(char *ptr, size_t size, size_t nmemb, void *user) {
    jw__fetch_item *item = (jw__fetch_item *)user;
    size_t n = size * nmemb;
    if (n == 0) {
        return 0;
    }
    if (item->size + n > JW_UPDATE_MANIFEST_MAX_BYTES) {
        snprintf(item->error, sizeof(item->error), "response too large: %s", item->url);
        return 0;
    }
    char *grown = (char *)realloc(item->data, item->size + n + 1u);
    if (!grown) {
        snprintf(item->error, sizeof(item->error), "%s", "out of memory");
        return 0;
    }
    memcpy(grown + item->size, ptr, n);
    item->size += n;
    grown[item->size] = '\0';
    item->data = grown;
    return n;
}
#endif

/* Fetch every item, concurrently where libcurl allows. Each item comes back with
   ok and data set, or with error explaining why not. */
static void jw__fetcher_run(jw__fetcher *fetcher,
                            jw__fetch_item *items,
                            int count,
                            const char *accept) {
    if (!fetcher || !items || count <= 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        items[i].data = NULL;
        items[i].size = 0;
        items[i].ok = false;
        items[i].error[0] = '\0';
        if (!jw__https_url_safe(items[i].url)) {
            snprintf(items[i].error, sizeof(items[i].error), "%s",
                     "invalid HTTPS download request");
        }
    }

#ifdef JW_UPDATE_USE_LIBCURL
    CURL **easies = (CURL **)calloc((size_t)count, sizeof(*easies));
    char (*curl_errors)[CURL_ERROR_SIZE] =
        calloc((size_t)count, sizeof(*curl_errors));
    struct curl_slist *headers = jw__curl_accept_header(accept);
    if (!fetcher->multi || !easies || !curl_errors || !headers) {
        for (int i = 0; i < count; i++) {
            if (!items[i].error[0]) {
                snprintf(items[i].error, sizeof(items[i].error), "%s",
                         "cannot prepare libcurl transfers");
            }
        }
        goto done;
    }

    for (int i = 0; i < count; i++) {
        if (items[i].error[0]) {
            continue;
        }
        CURL *easy = curl_easy_init();
        if (!easy) {
            snprintf(items[i].error, sizeof(items[i].error), "%s",
                     "libcurl easy init failed");
            continue;
        }
        CURLcode rc = jw__curl_apply_options(easy, items[i].url, headers,
                                             curl_errors[i],
                                             JW_UPDATE_METADATA_TIMEOUT_SECONDS);
        if (rc == CURLE_OK) {
            rc = curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, jw__fetch_item_write);
        }
        if (rc == CURLE_OK) {
            rc = curl_easy_setopt(easy, CURLOPT_WRITEDATA, &items[i]);
        }
        if (rc != CURLE_OK) {
            snprintf(items[i].error, sizeof(items[i].error),
                     "libcurl option failed: %s", curl_easy_strerror(rc));
            curl_easy_cleanup(easy);
            continue;
        }
        if (curl_multi_add_handle(fetcher->multi, easy) != CURLM_OK) {
            snprintf(items[i].error, sizeof(items[i].error), "%s",
                     "cannot queue libcurl transfer");
            curl_easy_cleanup(easy);
            continue;
        }
        easies[i] = easy;
    }

    for (;;) {
        int running = 0;
        CURLMcode mc = curl_multi_perform(fetcher->multi, &running);
        CURLMsg *msg = NULL;
        int left = 0;
        while ((msg = curl_multi_info_read(fetcher->multi, &left)) != NULL) {
            if (msg->msg != CURLMSG_DONE) {
                continue;
            }
            for (int i = 0; i < count; i++) {
                if (easies[i] != msg->easy_handle) {
                    continue;
                }
                if (msg->data.result == CURLE_OK) {
                    items[i].ok = true;
                } else if (!items[i].error[0]) {
                    jw__curl_failure(items[i].error, sizeof(items[i].error),
                                     msg->data.result, curl_errors[i]);
                }
                break;
            }
        }
        if (mc != CURLM_OK || running == 0) {
            break;
        }
        if (curl_multi_wait(fetcher->multi, NULL, 0, 1000, NULL) != CURLM_OK) {
            break;
        }
    }

    for (int i = 0; i < count; i++) {
        if (!easies[i]) {
            continue;
        }
        if (!items[i].ok && !items[i].error[0]) {
            snprintf(items[i].error, sizeof(items[i].error), "%s",
                     "libcurl transfer did not finish");
        }
        curl_multi_remove_handle(fetcher->multi, easies[i]);
        curl_easy_cleanup(easies[i]);
    }

done:
    free(easies);
    free(curl_errors);
    if (headers) {
        curl_slist_free_all(headers);
    }
    for (int i = 0; i < count; i++) {
        if (items[i].ok && !items[i].data) {
            items[i].data = (char *)calloc(1, 1);   /* empty body */
            if (!items[i].data) {
                items[i].ok = false;
                snprintf(items[i].error, sizeof(items[i].error), "%s", "out of memory");
            }
        }
    }
#else
    for (int i = 0; i < count; i++) {
        if (items[i].error[0]) {
            continue;
        }
        if (jw__fetch_https_to_file(items[i].url, fetcher->scratch_path, accept,
                                    JW_UPDATE_METADATA_TIMEOUT_SECONDS,
                                    items[i].error, sizeof(items[i].error)) != 0) {
            continue;
        }
        char *text = jw__read_text_file(fetcher->scratch_path, items[i].error,
                                        sizeof(items[i].error));
        unlink(fetcher->scratch_path);
        if (!text) {
            continue;
        }
        items[i].data = text;
        items[i].size = strlen(text);
        items[i].ok = true;
    }
#endif

    for (int i = 0; i < count; i++) {
        if (!items[i].ok) {
            jw__fetch_item_free(&items[i]);
        }
    }
}

static const char *jw__json_string(const cJSON *obj, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static long long jw__json_ll(const cJSON *obj, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(item)) {
        return 0;
    }
    return (long long)item->valuedouble;
}

static bool jw__sha256_looks_valid(const char *sha) {
    if (!sha || strlen(sha) != 64u) {
        return false;
    }
    for (size_t i = 0; i < 64u; i++) {
        if (!isxdigit((unsigned char)sha[i])) {
            return false;
        }
    }
    return true;
}

static void jw__json_add_string_or_null(cJSON *root,
                                        const char *name,
                                        const char *value) {
    if (value && value[0]) {
        cJSON_AddStringToObject(root, name, value);
    } else {
        cJSON_AddNullToObject(root, name);
    }
}

const char *jw_update_status_name(jw_update_status_code status) {
    switch (status) {
    case JW_UPDATE_STATUS_IDLE:
        return "idle";
    case JW_UPDATE_STATUS_AVAILABLE:
        return "available";
    case JW_UPDATE_STATUS_CHECKING:
        return "checking";
    case JW_UPDATE_STATUS_UP_TO_DATE:
        return "up-to-date";
    case JW_UPDATE_STATUS_INCOMPATIBLE:
        return "incompatible";
    case JW_UPDATE_STATUS_DOWNLOADING:
        return "downloading";
    case JW_UPDATE_STATUS_DOWNLOADED:
        return "downloaded";
    case JW_UPDATE_STATUS_CANCELLED:
        return "cancelled";
    case JW_UPDATE_STATUS_INSTALLING:
        return "installing";
    case JW_UPDATE_STATUS_ARMED:
        return "armed";
    case JW_UPDATE_STATUS_ERROR:
        return "error";
    }
    return "unknown";
}

void jw_update_refresh_installed(jw_update_status *status,
                                 const char *state_dir) {
    if (!status) {
        return;
    }

    status->current_unknown = true;
    status->installed_schema = 0;
    status->current_release_id[0] = '\0';
    status->current_version[0] = '\0';

    jw_installed_release installed;
    if (jw_installed_release_read(state_dir, &installed) != 0) {
        return;
    }

    status->installed_schema = installed.schema;
    const char *release_id = installed.release_id[0]
                                 ? installed.release_id
                                 : installed.version;
    const char *version = installed.version[0]
                              ? installed.version
                              : release_id;

    if (release_id && release_id[0]) {
        jw__copy_string(status->current_release_id,
                        sizeof(status->current_release_id), release_id);
        jw__copy_string(status->current_version,
                        sizeof(status->current_version), version);
        status->current_unknown = false;
    }
}

void jw_update_refresh_install_result(jw_update_status *status,
                                      const char *state_dir,
                                      const char *sdcard_root) {
    if (!status || !state_dir || !state_dir[0]) {
        return;
    }

    char update_dir[PATH_MAX];
    char result_path[PATH_MAX];
    if (!jw__update_dir(update_dir, sizeof(update_dir), state_dir) ||
        !jw__join_path(result_path, sizeof(result_path),
                       update_dir, "install-result.json")) {
        return;
    }

    char error[128];
    char *text = jw__read_text_file(result_path, error, sizeof(error));
    if (!text) {
        return;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return;
    }

    const char *state = jw__json_string(root, "state");
    const char *message = jw__json_string(root, "message");
    const char *release_id = jw__json_string(root, "release_id");
    if (!state || !state[0]) {
        state = "unknown";
    }
    if (!message) {
        message = "";
    }
    if (!release_id) {
        release_id = "";
    }

    bool armed_now_installed =
        strcmp(state, "armed") == 0 &&
        release_id[0] &&
        !status->current_unknown &&
        strcmp(status->current_release_id, release_id) == 0;
    bool stock_trigger_present = false;
    bool primary_stock_trigger_present = false;
    if (strcmp(state, "armed") == 0) {
        const char *trigger_file = status->handoff_trigger_file[0]
            ? status->handoff_trigger_file
            : "loong_upgrade";
        const char *roots[] = {
            sdcard_root,
            JW_UPDATE_MLP1_STOCK_UPDATE_ROOT,
            "/media/sdcard1",
        };
        for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
            const char *root_path = roots[i];
            if (!root_path || !root_path[0]) {
                continue;
            }
            bool seen = false;
            for (size_t j = 0; j < i; j++) {
                if (roots[j] && strcmp(roots[j], root_path) == 0) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            char trigger_path[PATH_MAX];
            if (jw__join_path(trigger_path, sizeof(trigger_path),
                              root_path, trigger_file) &&
                jw__file_exists(trigger_path)) {
                stock_trigger_present = true;
                if (strcmp(root_path, JW_UPDATE_MLP1_STOCK_UPDATE_ROOT) == 0) {
                    primary_stock_trigger_present = true;
                }
            }
        }
    }
    if (stock_trigger_present) {
        armed_now_installed = false;
    }

    jw__copy_string(status->install_result_state,
                    sizeof(status->install_result_state),
                    armed_now_installed ? "installed" : state);
    jw__copy_string(status->install_result_release_id,
                    sizeof(status->install_result_release_id),
                    release_id);
    jw__copy_string(status->install_result_path,
                    sizeof(status->install_result_path),
                    result_path);

    if (armed_now_installed) {
        snprintf(status->install_result_message,
                 sizeof(status->install_result_message),
                 "Leaf %s installed", release_id);
        status->install_armed = false;
        status->install_blocked = false;
    } else if (stock_trigger_present) {
        status->status = JW_UPDATE_STATUS_ARMED;
        status->install_armed = primary_stock_trigger_present;
        status->install_blocked = !primary_stock_trigger_present;
        status->install_ready = false;
        status->install_needs_confirmation = false;
        if (primary_stock_trigger_present) {
            snprintf(status->install_result_message,
                     sizeof(status->install_result_message),
                     "%s", "Restart to finish installing");
        } else {
            snprintf(status->install_result_message,
                     sizeof(status->install_result_message),
                     "%s", "Update was prepared on the other SD slot; install again");
        }
        jw__set_install_message(status,
                                primary_stock_trigger_present ? "restart_needed" : "install_again",
                                "%s", status->install_result_message);
        jw__set_message(status, "%s", status->install_result_message);
    } else {
        jw__copy_string(status->install_result_message,
                        sizeof(status->install_result_message),
                        message[0] ? message : state);
    }

    cJSON_Delete(root);
}

void jw_update_status_init(jw_update_status *status,
                           const char *platform_id,
                           const char *state_dir) {
    if (!status) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->status = JW_UPDATE_STATUS_IDLE;
    jw__clear_install_preflight(status);
    jw__clear_options(status);
    jw__copy_string(status->platform_id, sizeof(status->platform_id), platform_id);
    jw_update_refresh_installed(status, state_dir);
    jw_update_refresh_install_result(status, state_dir, NULL);
    if (status->install_result_message[0]) {
        jw__set_message(status, "%s", status->install_result_message);
    } else {
        jw__set_message(status, "%s", "No update check has been run");
    }
}

static void jw__begin_manifest_check(jw_update_status *status,
                                     const char *state_dir,
                                     const char *platform_id) {
    jw_update_refresh_installed(status, state_dir);
    jw__clear_candidate(status);
    jw__copy_string(status->platform_id, sizeof(status->platform_id), platform_id);
    status->checked_at = time(NULL);
}

/* Evaluate one artifact of a parsed manifest into the status candidate. Call
   jw__begin_manifest_check first. */
static int jw__update_check_manifest_json(jw_update_status *status,
                                          const char *platform_id,
                                          const cJSON *root,
                                          int artifact_index,
                                          bool allow_explicit_urls) {
    if (!status) {
        return -1;
    }
    if (!root) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update manifest is not valid JSON");
        return -1;
    }

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const char *product = jw__json_string(root, "product");
    const char *release_id = jw__json_string(root, "release_id");
    const char *version = jw__json_string(root, "version");
    const cJSON *platforms = cJSON_GetObjectItemCaseSensitive(root, "platforms");
    if (!cJSON_IsNumber(schema) || schema->valueint < 1) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update manifest is missing schema");
        return -1;
    }
    if (!product || strcmp(product, "leaf") != 0) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update manifest product is not leaf");
        return -1;
    }
    if (!release_id || !release_id[0]) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update manifest is missing release_id");
        return -1;
    }
    if (!version || !version[0]) {
        version = release_id;
    }
    if (!cJSON_IsObject(platforms)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update manifest is missing platforms");
        return -1;
    }

    const cJSON *platform = cJSON_GetObjectItemCaseSensitive(platforms, platform_id);
    if (!cJSON_IsObject(platform)) {
        status->status = JW_UPDATE_STATUS_INCOMPATIBLE;
        jw__copy_string(status->release_id, sizeof(status->release_id), release_id);
        jw__copy_string(status->version, sizeof(status->version), version);
        jw__set_message(status, "Release %s does not support %s",
                        release_id, platform_id && platform_id[0] ? platform_id : "this platform");
        return 0;
    }

    long long min_schema = jw__json_ll(platform, "min_installed_schema");
    if (min_schema > JW_UPDATE_INSTALLED_SCHEMA) {
        status->status = JW_UPDATE_STATUS_INCOMPATIBLE;
        jw__copy_string(status->release_id, sizeof(status->release_id), release_id);
        jw__copy_string(status->version, sizeof(status->version), version);
        jw__set_message(status, "Release %s requires a newer updater", release_id);
        return 0;
    }

    const cJSON *artifacts = cJSON_GetObjectItemCaseSensitive(platform, "artifacts");
    const cJSON *artifact = NULL;
    if (cJSON_IsArray(artifacts)) {
        artifact = cJSON_GetArrayItem(artifacts, artifact_index < 0 ? 0 : artifact_index);
    } else if (artifact_index <= 0) {
        artifact = cJSON_GetObjectItemCaseSensitive(platform, "artifact");
    }
    const char *artifact_kind = jw__json_string(artifact, "kind");
    const char *artifact_name = jw__json_string(artifact, "name");
    const char *artifact_sha256 = jw__json_string(artifact, "sha256");
    const char *artifact_url = allow_explicit_urls
        ? jw__json_string(artifact, "url")
        : NULL;
    if (!cJSON_IsObject(artifact) ||
        !artifact_kind || !artifact_kind[0] ||
        !jw__filename_safe(artifact_name) ||
        !jw__sha256_looks_valid(artifact_sha256)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Platform artifact metadata is incomplete");
        return -1;
    }
    if (artifact_url && !jw__https_url_safe(artifact_url)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Local manifest artifact URL must use HTTPS");
        return -1;
    }

    jw__copy_string(status->release_id, sizeof(status->release_id), release_id);
    jw__copy_string(status->version, sizeof(status->version), version);
    jw__copy_string(status->published_at, sizeof(status->published_at),
                    jw__json_string(root, "published_at"));
    const cJSON *notes = cJSON_GetObjectItemCaseSensitive(root, "notes");
    jw__copy_string(status->notes_url, sizeof(status->notes_url),
                    jw__json_string(notes, "url"));

    jw__copy_string(status->artifact_kind, sizeof(status->artifact_kind),
                    artifact_kind);
    jw__copy_string(status->artifact_name, sizeof(status->artifact_name),
                    artifact_name);
    jw__copy_string(status->artifact_sha256, sizeof(status->artifact_sha256),
                    artifact_sha256);
    jw__copy_string(status->artifact_url, sizeof(status->artifact_url),
                    artifact_url);
    status->artifact_size = jw__json_ll(artifact, "size");
    status->installed_size = jw__json_ll(artifact, "installed_size");

    const cJSON *recovery = cJSON_GetObjectItemCaseSensitive(platform, "recovery_zip");
    if (!cJSON_IsObject(recovery)) {
        recovery = cJSON_GetObjectItemCaseSensitive(platform, "recovery");
    }
    if (cJSON_IsObject(recovery)) {
        jw__copy_string(status->recovery_name, sizeof(status->recovery_name),
                        jw__json_string(recovery, "name"));
    }

    const cJSON *handoff = cJSON_GetObjectItemCaseSensitive(platform, "handoff");
    if (cJSON_IsObject(handoff)) {
        jw__copy_string(status->handoff_type, sizeof(status->handoff_type),
                        jw__json_string(handoff, "type"));
        jw__copy_string(status->handoff_completion, sizeof(status->handoff_completion),
                        jw__json_string(handoff, "completion"));
        jw__copy_string(status->handoff_trigger_file,
                        sizeof(status->handoff_trigger_file),
                        jw__json_string(handoff, "trigger_file"));
    }

    const cJSON *managed_apps = cJSON_GetObjectItemCaseSensitive(platform, "managed_apps");
    if (cJSON_IsArray(managed_apps)) {
        status->managed_apps_count = cJSON_GetArraySize(managed_apps);
    }
    const cJSON *migrations = cJSON_GetObjectItemCaseSensitive(platform, "migrations");
    if (cJSON_IsArray(migrations)) {
        status->migrations_count = cJSON_GetArraySize(migrations);
    }

    status->compatible = true;
    if (!status->current_unknown &&
        strcmp(status->current_release_id, status->release_id) == 0) {
        status->status = JW_UPDATE_STATUS_UP_TO_DATE;
        status->has_update = false;
        jw__set_message(status, "Leaf %s is already installed", status->release_id);
    } else {
        status->status = JW_UPDATE_STATUS_AVAILABLE;
        status->has_update = true;
        if (status->current_unknown) {
            jw__set_message(status, "Leaf %s is available; installed release is unknown",
                            status->release_id);
        } else {
            jw__set_message(status, "Leaf %s is available", status->release_id);
        }
    }

    return 0;
}

static int jw__update_check_local_manifest_artifact(jw_update_status *status,
                                                    const char *state_dir,
                                                    const char *platform_id,
                                                    const char *manifest_path) {
    if (!status) {
        return -1;
    }

    jw__begin_manifest_check(status, state_dir, platform_id);

    if (!manifest_path || !manifest_path[0]) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "No update manifest path was provided");
        return -1;
    }
    jw__copy_string(status->source_manifest, sizeof(status->source_manifest),
                    manifest_path);

    char error[192];
    char *text = jw__read_text_file(manifest_path, error, sizeof(error));
    if (!text) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", error[0] ? error : "Cannot read update manifest");
        return -1;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    int rc = jw__update_check_manifest_json(status, platform_id, root, 0, true);
    cJSON_Delete(root);
    return rc;
}

int jw_update_check_local_manifest(jw_update_status *status,
                                   const char *state_dir,
                                   const char *platform_id,
                                   const char *manifest_path) {
    if (status) {
        jw__clear_options(status);
        /* A local manifest is the whole list; there are no older releases to load. */
        status->options_complete = true;
    }
    int rc = jw__update_check_local_manifest_artifact(status, state_dir, platform_id,
                                                     manifest_path);
    if (rc == 0 && status && status->compatible) {
        jw__copy_status_to_option(status, &status->options[0], 0);
        status->option_count = 1;
        status->selected_option = 0;
    }
    return rc;
}

static int jw__manifest_artifact_count(const cJSON *root, const char *platform_id) {
    if (!cJSON_IsObject(root) || !platform_id || !platform_id[0]) {
        return 0;
    }

    int count = 0;
    const cJSON *platforms = cJSON_GetObjectItemCaseSensitive(root, "platforms");
    const cJSON *platform = cJSON_GetObjectItemCaseSensitive(platforms, platform_id);
    if (cJSON_IsObject(platform)) {
        const cJSON *artifacts = cJSON_GetObjectItemCaseSensitive(platform, "artifacts");
        if (cJSON_IsArray(artifacts)) {
            count = cJSON_GetArraySize(artifacts);
        } else if (cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(platform, "artifact"))) {
            count = 1;
        }
    }
    return count;
}

static void jw__copy_status_to_option(jw_update_status *status,
                                      jw_update_option *option,
                                      int index) {
    if (!status || !option) {
        return;
    }

    memset(option, 0, sizeof(*option));
    option->index = index;
    option->installed = !status->current_unknown &&
                        status->release_id[0] &&
                        strcmp(status->current_release_id, status->release_id) == 0;
    jw__copy_string(option->release_id, sizeof(option->release_id),
                    status->release_id);
    jw__copy_string(option->version, sizeof(option->version), status->version);
    jw__copy_string(option->published_at, sizeof(option->published_at),
                    status->published_at);
    jw__copy_string(option->notes_url, sizeof(option->notes_url),
                    status->notes_url);
    jw__copy_string(option->manifest_url, sizeof(option->manifest_url),
                    status->manifest_url);
    jw__copy_string(option->artifact_kind, sizeof(option->artifact_kind),
                    status->artifact_kind);
    jw__copy_string(option->artifact_name, sizeof(option->artifact_name),
                    status->artifact_name);
    jw__copy_string(option->artifact_url, sizeof(option->artifact_url),
                    status->artifact_url);
    jw__copy_string(option->artifact_sha256, sizeof(option->artifact_sha256),
                    status->artifact_sha256);
    option->artifact_size = status->artifact_size;
    option->installed_size = status->installed_size;
    jw__copy_string(option->recovery_name, sizeof(option->recovery_name),
                    status->recovery_name);
    jw__copy_string(option->recovery_url, sizeof(option->recovery_url),
                    status->recovery_url);
    jw__copy_string(option->handoff_type, sizeof(option->handoff_type),
                    status->handoff_type);
    jw__copy_string(option->handoff_completion, sizeof(option->handoff_completion),
                    status->handoff_completion);
    jw__copy_string(option->handoff_trigger_file,
                    sizeof(option->handoff_trigger_file),
                    status->handoff_trigger_file);
    option->managed_apps_count = status->managed_apps_count;
    option->migrations_count = status->migrations_count;
}

int jw_update_select_option(jw_update_status *status, int option_index) {
    if (!status || option_index < 0 || option_index >= status->option_count ||
        option_index >= JW_UPDATE_MAX_OPTIONS) {
        if (status) {
            status->status = JW_UPDATE_STATUS_ERROR;
            jw__set_message(status, "%s", "Selected update is no longer available");
        }
        return -1;
    }

    jw_update_option option = status->options[option_index];
    jw__clear_candidate(status);

    status->selected_option = option_index;
    status->compatible = true;
    jw__copy_string(status->release_id, sizeof(status->release_id),
                    option.release_id);
    jw__copy_string(status->version, sizeof(status->version), option.version);
    jw__copy_string(status->published_at, sizeof(status->published_at),
                    option.published_at);
    jw__copy_string(status->notes_url, sizeof(status->notes_url),
                    option.notes_url);
    jw__copy_string(status->manifest_url, sizeof(status->manifest_url),
                    option.manifest_url);
    jw__copy_string(status->artifact_kind, sizeof(status->artifact_kind),
                    option.artifact_kind);
    jw__copy_string(status->artifact_name, sizeof(status->artifact_name),
                    option.artifact_name);
    jw__copy_string(status->artifact_url, sizeof(status->artifact_url),
                    option.artifact_url);
    jw__copy_string(status->artifact_sha256, sizeof(status->artifact_sha256),
                    option.artifact_sha256);
    status->artifact_size = option.artifact_size;
    status->installed_size = option.installed_size;
    jw__copy_string(status->recovery_name, sizeof(status->recovery_name),
                    option.recovery_name);
    jw__copy_string(status->recovery_url, sizeof(status->recovery_url),
                    option.recovery_url);
    jw__copy_string(status->handoff_type, sizeof(status->handoff_type),
                    option.handoff_type);
    jw__copy_string(status->handoff_completion, sizeof(status->handoff_completion),
                    option.handoff_completion);
    jw__copy_string(status->handoff_trigger_file,
                    sizeof(status->handoff_trigger_file),
                    option.handoff_trigger_file);
    status->managed_apps_count = option.managed_apps_count;
    status->migrations_count = option.migrations_count;

    if (option.installed) {
        status->status = JW_UPDATE_STATUS_UP_TO_DATE;
        status->has_update = false;
        jw__set_message(status, "Leaf %s is already installed",
                        status->release_id);
    } else {
        status->status = JW_UPDATE_STATUS_AVAILABLE;
        status->has_update = true;
        jw__set_message(status, "Leaf %s selected", status->release_id);
    }
    return 0;
}

static const cJSON *jw__find_release_asset(const cJSON *release,
                                           const char *asset_name) {
    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(release, "assets");
    if (!cJSON_IsArray(assets) || !asset_name || !asset_name[0]) {
        return NULL;
    }

    const cJSON *asset = NULL;
    cJSON_ArrayForEach(asset, assets) {
        const char *name = jw__json_string(asset, "name");
        if (name && strcmp(name, asset_name) == 0) {
            return asset;
        }
    }
    return NULL;
}

static const char *jw__release_asset_download_url(const cJSON *asset) {
    const char *url = jw__json_string(asset, "browser_download_url");
    if (jw__https_url_safe(url)) {
        return url;
    }
    return NULL;
}

/* JAWAKA_UPDATE_RELEASES_URL points both channels at another GitHub-shaped
   release feed, for staging feeds and tests. It must be HTTPS. */
static const char *jw__releases_url(jw_update_channel channel) {
    const char *override = getenv("JAWAKA_UPDATE_RELEASES_URL");
    if (jw__https_url_safe(override)) {
        return override;
    }
    return channel == JW_UPDATE_CHANNEL_BETA ? JW_UPDATE_RELEASES_URL_BETA
                                             : JW_UPDATE_RELEASES_URL_STABLE;
}

/* Add every compatible artifact a release's manifest offers this platform to
   status->options. */
static void jw__add_release_options(jw_update_status *status,
                                    const char *state_dir,
                                    const char *platform_id,
                                    const cJSON *release,
                                    const char *manifest_url,
                                    const char *manifest_text,
                                    char *last_message,
                                    size_t last_message_size) {
    cJSON *root = cJSON_Parse(manifest_text ? manifest_text : "");
    int artifact_count = jw__manifest_artifact_count(root, platform_id);
    if (artifact_count < 1) {
        artifact_count = 1;
    }

    for (int artifact_index = 0;
         artifact_index < artifact_count &&
         status->option_count < JW_UPDATE_MAX_OPTIONS;
         artifact_index++) {
        jw__begin_manifest_check(status, state_dir, platform_id);
        if (jw__update_check_manifest_json(status, platform_id, root,
                                           artifact_index, false) != 0) {
            snprintf(last_message, last_message_size, "%s", status->message);
            continue;
        }

        if (status->status == JW_UPDATE_STATUS_INCOMPATIBLE) {
            snprintf(last_message, last_message_size, "%s", status->message);
            continue;
        }

        const cJSON *artifact_asset =
            jw__find_release_asset(release, status->artifact_name);
        const char *artifact_url = jw__release_asset_download_url(artifact_asset);
        if (!artifact_url) {
            snprintf(last_message, last_message_size, "%s",
                     "Release is missing an update asset");
            continue;
        }

        jw__copy_string(status->manifest_url, sizeof(status->manifest_url),
                        manifest_url);
        jw__copy_string(status->artifact_url, sizeof(status->artifact_url),
                        artifact_url);

        if (!status->published_at[0]) {
            jw__copy_string(status->published_at, sizeof(status->published_at),
                            jw__json_string(release, "published_at"));
        }
        if (!status->notes_url[0]) {
            jw__copy_string(status->notes_url, sizeof(status->notes_url),
                            jw__json_string(release, "html_url"));
        }

        if (status->recovery_name[0]) {
            const cJSON *recovery_asset =
                jw__find_release_asset(release, status->recovery_name);
            const char *recovery_url = jw__release_asset_download_url(recovery_asset);
            if (recovery_url) {
                jw__copy_string(status->recovery_url, sizeof(status->recovery_url),
                                recovery_url);
            }
        }

        jw__copy_status_to_option(status,
                                  &status->options[status->option_count],
                                  status->option_count);
        status->option_count++;
    }

    cJSON_Delete(root);
}

int jw_update_check_releases(jw_update_status *status,
                             const char *state_dir,
                             const char *platform_id,
                             const char *releases_url,
                             jw_update_check_scope scope) {
    if (!status) {
        return -1;
    }

    jw_update_refresh_installed(status, state_dir);
    jw__clear_candidate(status);
    jw__clear_options(status);
    jw__copy_string(status->platform_id, sizeof(status->platform_id), platform_id);
    status->checked_at = time(NULL);

    char update_dir[PATH_MAX];
    if (!jw__update_dir(update_dir, sizeof(update_dir), state_dir)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Cannot prepare update state directory");
        return -1;
    }

    char error[256];
    jw__fetcher fetcher;
    if (jw__fetcher_open(&fetcher, update_dir, error, sizeof(error)) != 0) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", error[0] ? error : "Cannot fetch Leaf releases");
        return -1;
    }

    jw__fetch_item list;
    memset(&list, 0, sizeof(list));
    list.url = releases_url;
    jw__fetcher_run(&fetcher, &list, 1, "application/vnd.github+json");
    if (!list.ok) {
        jw__fetcher_close(&fetcher);
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s",
                        list.error[0] ? list.error : "Cannot fetch Leaf releases");
        return -1;
    }

    cJSON *releases = cJSON_Parse(list.data);
    jw__fetch_item_free(&list);
    if (!cJSON_IsArray(releases)) {
        if (releases) {
            cJSON_Delete(releases);
        }
        jw__fetcher_close(&fetcher);
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Leaf releases response was not a JSON array");
        return -1;
    }

    /* Published releases that carry update metadata, newest first. */
    const cJSON *candidates[JW_UPDATE_MAX_OPTIONS];
    jw__fetch_item manifests[JW_UPDATE_MAX_OPTIONS];
    memset(manifests, 0, sizeof(manifests));
    int candidate_count = 0;
    const cJSON *release = NULL;
    cJSON_ArrayForEach(release, releases) {
        if (candidate_count >= JW_UPDATE_MAX_OPTIONS) {
            break;
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "draft")) ||
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "prerelease"))) {
            continue;
        }

        const cJSON *manifest_asset = jw__find_release_asset(release, "leaf-update.json");
        const char *manifest_url = jw__release_asset_download_url(manifest_asset);
        if (!manifest_url) {
            continue;
        }
        candidates[candidate_count] = release;
        manifests[candidate_count].url = manifest_url;
        candidate_count++;
    }

    char last_message[JW_UPDATE_MESSAGE_MAX];
    snprintf(last_message, sizeof(last_message), "%s",
             "No compatible Leaf update metadata found");

    /* The routine check only needs the newest compatible release, so it takes
       manifests one at a time and stops at the first hit; the fetcher keeps the
       connections warm for the next one if it has to keep looking. Loading the
       full list for the release picker fetches every manifest at once. */
    bool all_fetched = true;
    int next = 0;
    while (next < candidate_count && status->option_count < JW_UPDATE_MAX_OPTIONS) {
        int batch = scope == JW_UPDATE_SCOPE_ALL ? candidate_count - next : 1;
        jw__fetcher_run(&fetcher, &manifests[next], batch, "application/octet-stream");
        for (int i = next; i < next + batch; i++) {
            if (!manifests[i].ok) {
                all_fetched = false;
                snprintf(last_message, sizeof(last_message), "%s",
                         manifests[i].error[0] ? manifests[i].error
                                               : "Cannot fetch release manifest");
                continue;
            }
            jw__add_release_options(status, state_dir, platform_id, candidates[i],
                                    manifests[i].url, manifests[i].data,
                                    last_message, sizeof(last_message));
        }
        next += batch;
        if (scope == JW_UPDATE_SCOPE_LATEST && status->option_count > 0) {
            break;
        }
    }
    status->options_complete = all_fetched &&
        (next >= candidate_count || status->option_count >= JW_UPDATE_MAX_OPTIONS);

    for (int i = 0; i < candidate_count; i++) {
        jw__fetch_item_free(&manifests[i]);
    }
    jw__fetcher_close(&fetcher);
    cJSON_Delete(releases);

    if (status->option_count > 0) {
        return jw_update_select_option(status, 0);
    }
    status->status = JW_UPDATE_STATUS_ERROR;
    jw__set_message(status, "%s", last_message);
    return -1;
}

int jw_update_check_github(jw_update_status *status,
                           const char *state_dir,
                           const char *platform_id,
                           jw_update_channel channel,
                           jw_update_check_scope scope) {
    return jw_update_check_releases(status, state_dir, platform_id,
                                    jw__releases_url(channel), scope);
}

int jw_update_download_candidate(jw_update_status *status,
                                 const char *state_dir) {
    if (!status) {
        return -1;
    }
    status->downloaded = false;
    status->download_active = false;
    status->download_received = 0;
    status->download_total = status->artifact_size;
    status->download_percent = status->artifact_size > 0 ? 0 : -1;
    status->download_path[0] = '\0';

    if (!jw__https_url_safe(status->artifact_url) ||
        !jw__filename_safe(status->artifact_name) ||
        !jw__sha256_looks_valid(status->artifact_sha256)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "No downloadable update artifact is selected");
        return -1;
    }

    char update_dir[PATH_MAX];
    if (!jw__update_dir(update_dir, sizeof(update_dir), state_dir)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Cannot prepare update state directory");
        return -1;
    }

    char downloads_dir[PATH_MAX];
    if (!jw__join_path(downloads_dir, sizeof(downloads_dir),
                       update_dir, "downloads") ||
        jw__mkdir_p(downloads_dir, 0755) != 0) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Cannot prepare update download directory");
        return -1;
    }

    if (status->artifact_size > 0) {
        struct statvfs fs;
        if (statvfs(downloads_dir, &fs) == 0) {
            long long available = (long long)fs.f_bavail * (long long)fs.f_frsize;
            long long required = status->artifact_size + JW_UPDATE_MIN_DOWNLOAD_FREE_BYTES;
            if (available < required) {
                status->status = JW_UPDATE_STATUS_ERROR;
                jw__set_message(status, "%s", "Not enough free space for update download");
                return -1;
            }
        }
    }

    char out_path[PATH_MAX];
    if (!jw__join_path(out_path, sizeof(out_path),
                       downloads_dir, status->artifact_name)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update download path is too long");
        return -1;
    }

    char error[256];
    if (jw__fetch_https_to_file(status->artifact_url, out_path,
                                "application/octet-stream", 0,
                                error, sizeof(error)) != 0) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", error[0] ? error : "Cannot download update artifact");
        return -1;
    }

    if (status->artifact_size > 0) {
        struct stat st;
        if (stat(out_path, &st) != 0 || st.st_size != status->artifact_size) {
            unlink(out_path);
            status->status = JW_UPDATE_STATUS_ERROR;
            jw__set_message(status, "%s", "Downloaded update size does not match metadata");
            return -1;
        }
        status->download_received = st.st_size;
    }

    char actual_sha[JW_UPDATE_SHA256_MAX];
    if (jw_sha256_file_hex(out_path, actual_sha, error, sizeof(error)) != 0) {
        unlink(out_path);
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", error[0] ? error : "Cannot verify update artifact");
        return -1;
    }
    if (!jw__ascii_equal_ci(actual_sha, status->artifact_sha256)) {
        unlink(out_path);
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Downloaded update SHA-256 does not match metadata");
        return -1;
    }

    status->downloaded = true;
    status->download_active = false;
    if (status->artifact_size > 0) {
        status->download_percent = 100;
    }
    status->status = JW_UPDATE_STATUS_DOWNLOADED;
    jw__copy_string(status->download_path, sizeof(status->download_path), out_path);
    jw__set_message(status, "Leaf %s downloaded and verified",
                    status->release_id[0] ? status->release_id : "update");
    return 0;
}

void jw_update_download_job_init(jw_update_download_job *job) {
    if (!job) {
        return;
    }
    memset(job, 0, sizeof(*job));
    job->pid = -1;
}

static void jw__update_download_progress(jw_update_status *status,
                                         const jw_update_download_job *job) {
    if (!status || !job || !job->active) {
        return;
    }

    struct stat st;
    if (stat(job->tmp_path, &st) == 0 && st.st_size >= 0) {
        status->download_received = st.st_size;
    }
    status->download_total = job->expected_size;
    if (status->download_total > 0) {
        long long percent = (status->download_received * 100LL) /
                            status->download_total;
        if (percent < 0) percent = 0;
        if (percent > 99) percent = 99;
        status->download_percent = (int)percent;
    } else {
        status->download_percent = -1;
    }
}

static void jw__finish_download_job(jw_update_status *status,
                                    jw_update_download_job *job,
                                    int child_status) {
    if (!status || !job) {
        return;
    }

    jw__update_download_progress(status, job);
    job->active = false;
    status->download_active = false;

    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
        unlink(job->tmp_path);
        status->downloaded = false;
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update download failed");
        return;
    }

    if (job->expected_size > 0) {
        struct stat st;
        if (stat(job->tmp_path, &st) != 0 || st.st_size != job->expected_size) {
            unlink(job->tmp_path);
            status->downloaded = false;
            status->status = JW_UPDATE_STATUS_ERROR;
            jw__set_message(status, "%s", "Downloaded update size does not match metadata");
            return;
        }
        status->download_received = st.st_size;
    }

    if (rename(job->tmp_path, job->out_path) != 0) {
        unlink(job->tmp_path);
        status->downloaded = false;
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "Cannot move update download into place: %s",
                        strerror(errno));
        return;
    }

    char error[256];
    char actual_sha[JW_UPDATE_SHA256_MAX];
    if (jw_sha256_file_hex(job->out_path, actual_sha, error, sizeof(error)) != 0) {
        unlink(job->out_path);
        status->downloaded = false;
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", error[0] ? error : "Cannot verify update artifact");
        return;
    }
    if (!jw__ascii_equal_ci(actual_sha, job->expected_sha256)) {
        unlink(job->out_path);
        status->downloaded = false;
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Downloaded update SHA-256 does not match metadata");
        return;
    }

    status->downloaded = true;
    status->download_active = false;
    status->download_total = job->expected_size;
    if (job->expected_size > 0) {
        status->download_received = job->expected_size;
        status->download_percent = 100;
    } else {
        status->download_percent = -1;
    }
    status->status = JW_UPDATE_STATUS_DOWNLOADED;
    jw__copy_string(status->download_path, sizeof(status->download_path),
                    job->out_path);
    jw__set_message(status, "Leaf %s downloaded and verified",
                    status->release_id[0] ? status->release_id : "update");
}

int jw_update_download_start(jw_update_status *status,
                             jw_update_download_job *job,
                             const char *state_dir) {
    if (!status || !job) {
        return -1;
    }
    if (job->active) {
        jw__update_download_progress(status, job);
        jw__set_message(status, "%s", "Update download already in progress");
        return 0;
    }

    status->downloaded = false;
    status->download_active = false;
    status->download_received = 0;
    status->download_total = status->artifact_size;
    status->download_percent = status->artifact_size > 0 ? 0 : -1;
    status->download_path[0] = '\0';

    if (!jw__https_url_safe(status->artifact_url) ||
        !jw__filename_safe(status->artifact_name) ||
        !jw__sha256_looks_valid(status->artifact_sha256)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "No downloadable update artifact is selected");
        return -1;
    }

    char update_dir[PATH_MAX];
    if (!jw__update_dir(update_dir, sizeof(update_dir), state_dir)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Cannot prepare update state directory");
        return -1;
    }

    char downloads_dir[PATH_MAX];
    if (!jw__join_path(downloads_dir, sizeof(downloads_dir),
                       update_dir, "downloads") ||
        jw__mkdir_p(downloads_dir, 0755) != 0) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Cannot prepare update download directory");
        return -1;
    }

    if (status->artifact_size > 0) {
        struct statvfs fs;
        if (statvfs(downloads_dir, &fs) == 0) {
            long long available = (long long)fs.f_bavail * (long long)fs.f_frsize;
            long long required = status->artifact_size + JW_UPDATE_MIN_DOWNLOAD_FREE_BYTES;
            if (available < required) {
                status->status = JW_UPDATE_STATUS_ERROR;
                jw__set_message(status, "%s", "Not enough free space for update download");
                return -1;
            }
        }
    }

    char out_path[JW_UPDATE_PATH_MAX];
    if (!jw__join_path(out_path, sizeof(out_path),
                       downloads_dir, status->artifact_name)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update download path is too long");
        return -1;
    }

    char tmp_path[JW_UPDATE_PATH_MAX];
    int needed = snprintf(tmp_path, sizeof(tmp_path), "%s/.%s.part.%ld",
                          downloads_dir, status->artifact_name, (long)getpid());
    if (needed < 0 || needed >= (int)sizeof(tmp_path)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update temporary path is too long");
        return -1;
    }
    unlink(tmp_path);

    char error[256];
    pid_t pid = jw__spawn_https_to_file(status->artifact_url, tmp_path,
                                        "application/octet-stream", 0,
                                        error, sizeof(error));
    if (pid < 0) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", error[0] ? error : "Cannot start update download");
        return -1;
    }

    job->active = true;
    job->pid = pid;
    jw__copy_string(job->tmp_path, sizeof(job->tmp_path), tmp_path);
    jw__copy_string(job->out_path, sizeof(job->out_path), out_path);
    jw__copy_string(job->artifact_name, sizeof(job->artifact_name),
                    status->artifact_name);
    jw__copy_string(job->expected_sha256, sizeof(job->expected_sha256),
                    status->artifact_sha256);
    job->expected_size = status->artifact_size;

    status->status = JW_UPDATE_STATUS_DOWNLOADING;
    status->download_active = true;
    status->download_total = status->artifact_size;
    status->download_received = 0;
    status->download_percent = status->artifact_size > 0 ? 0 : -1;
    jw__set_message(status, "Downloading %s", status->artifact_name);
    return 0;
}

void jw_update_download_poll(jw_update_status *status,
                             jw_update_download_job *job) {
    if (!status || !job || !job->active || job->pid <= 0) {
        return;
    }

    jw__update_download_progress(status, job);

    int child_status = 0;
    pid_t waited = waitpid(job->pid, &child_status, WNOHANG);
    if (waited == 0) {
        return;
    }
    if (waited < 0) {
        if (errno == ECHILD) {
            job->active = false;
            status->download_active = false;
            status->downloaded = false;
            status->status = JW_UPDATE_STATUS_ERROR;
            jw__set_message(status, "%s", "Update download process disappeared");
        }
        return;
    }

    jw__finish_download_job(status, job, child_status);
}

static void *jw__update_check_worker(void *arg) {
    jw_update_check_job *job = (jw_update_check_job *)arg;
    job->result = jw_update_check_github(&job->scratch, job->state_dir,
                                         job->platform_id, job->channel,
                                         job->scope);
    atomic_store_explicit(&job->done, true, memory_order_release);
    return NULL;
}

/* Hand a finished job's result to the live status. A routine check replaces the
   whole status. A release-list load only swaps in the options, so the selected
   candidate and any download stay as they were. */
static void jw__finish_check_job(jw_update_status *status,
                                 const jw_update_check_job *job) {
    if (job->scope == JW_UPDATE_SCOPE_LATEST) {
        *status = job->scratch;
        status->options_loading = false;
        return;
    }

    status->options_loading = false;
    if (job->result != 0) {
        status->options_complete = false;
        jw__set_message(status, "%s", job->scratch.message);
        return;
    }

    memcpy(status->options, job->scratch.options, sizeof(status->options));
    status->option_count = job->scratch.option_count;
    status->options_complete = job->scratch.options_complete;
    if (!status->options_complete) {
        jw__set_message(status, "%s", "Some releases could not be loaded");
    }
    status->selected_option = -1;
    for (int i = 0; i < status->option_count; i++) {
        const jw_update_option *option = &status->options[i];
        if (status->release_id[0] &&
            strcmp(option->release_id, status->release_id) == 0 &&
            strcmp(option->artifact_name, status->artifact_name) == 0) {
            status->selected_option = i;
            break;
        }
    }
}

void jw_update_check_job_init(jw_update_check_job *job) {
    if (!job) {
        return;
    }
    memset(job, 0, sizeof(*job));
    atomic_init(&job->done, false);
}

int jw_update_check_start(jw_update_status *status,
                          jw_update_check_job *job,
                          const char *state_dir,
                          jw_update_channel channel,
                          jw_update_check_scope scope) {
    if (!status || !job) {
        return -1;
    }
    if (job->active) {
        /* One job at a time. A release-list load asked for during a routine check
           runs once that check lands, against the same channel, so the picker is
           not left waiting on a request that was dropped. */
        if (scope == JW_UPDATE_SCOPE_ALL && job->scope == JW_UPDATE_SCOPE_LATEST) {
            job->queued_all = true;
            status->options_loading = true;
        }
        return 0;
    }

    /* Seed the worker's scratch from the live status so fields the check does not
       touch are preserved when we copy it back. */
    job->scratch = *status;
    atomic_store_explicit(&job->done, false, memory_order_relaxed);
    job->result = 0;
    job->channel = channel;
    job->scope = scope;
    job->queued_all = false;
    jw__copy_string(job->state_dir, sizeof(job->state_dir),
                    state_dir ? state_dir : "");
    jw__copy_string(job->platform_id, sizeof(job->platform_id),
                    status->platform_id);

    if (pthread_create(&job->thread, NULL, jw__update_check_worker, job) != 0) {
        /* Threading failed: fall back to a synchronous check so the feature still
           works. This blocks (the old behaviour) but is not a regression. */
        job->result = jw_update_check_github(&job->scratch, job->state_dir,
                                             job->platform_id, channel, scope);
        jw__finish_check_job(status, job);
        return 0;
    }

    job->active = true;
    if (scope == JW_UPDATE_SCOPE_ALL) {
        status->options_loading = true;
    } else {
        status->status = JW_UPDATE_STATUS_CHECKING;
        /* The check replaces the options with the newest release alone. Until
           it lands, don't let the picker take the previous list as complete:
           asking for the full list now has to queue a load behind this check. */
        status->options_complete = false;
        jw__set_message(status, "%s", "Checking for updates");
    }
    return 0;
}

void jw_update_check_poll(jw_update_status *status,
                          jw_update_check_job *job) {
    if (!status || !job || !job->active ||
        !atomic_load_explicit(&job->done, memory_order_acquire)) {
        return;
    }
    pthread_join(job->thread, NULL);  /* makes the worker's writes visible */
    jw__finish_check_job(status, job);
    job->active = false;
    atomic_store_explicit(&job->done, false, memory_order_relaxed);

    if (job->queued_all) {
        job->queued_all = false;
        if (!status->options_complete) {
            char state_dir[JW_UPDATE_PATH_MAX];
            jw__copy_string(state_dir, sizeof(state_dir), job->state_dir);
            jw_update_check_start(status, job, state_dir, job->channel,
                                  JW_UPDATE_SCOPE_ALL);
        }
    }
}

void jw_update_check_job_wait(jw_update_check_job *job) {
    if (!job || !job->active) {
        return;
    }
    pthread_join(job->thread, NULL);
    job->active = false;
    job->queued_all = false;
    atomic_store_explicit(&job->done, false, memory_order_relaxed);
}

int jw_update_download_cancel(jw_update_status *status,
                              jw_update_download_job *job) {
    if (!status || !job || !job->active || job->pid <= 0) {
        if (status) {
            jw__set_message(status, "%s", "No update download is active");
        }
        return -1;
    }

    kill(job->pid, SIGTERM);
    int child_status = 0;
    for (int attempt = 0; attempt < 5; attempt++) {
        pid_t waited = waitpid(job->pid, &child_status, WNOHANG);
        if (waited == job->pid || (waited < 0 && errno == ECHILD)) {
            break;
        }
        usleep(50000);
    }
    if (kill(job->pid, 0) == 0) {
        kill(job->pid, SIGKILL);
        waitpid(job->pid, &child_status, 0);
    }

    unlink(job->tmp_path);
    job->active = false;
    status->download_active = false;
    status->downloaded = false;
    status->status = JW_UPDATE_STATUS_CANCELLED;
    jw__set_message(status, "%s", "Update download cancelled");
    return 0;
}

void jw_update_install_job_init(jw_update_install_job *job) {
    if (!job) {
        return;
    }
    memset(job, 0, sizeof(*job));
    job->pid = -1;
}

static int jw__write_json_file(const char *path, const cJSON *root) {
    if (!path || !path[0] || !root) {
        return -1;
    }

    char *text = cJSON_PrintUnformatted((cJSON *)root);
    if (!text) {
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        cJSON_free(text);
        return -1;
    }

    int ok = fputs(text, fp) >= 0 && fputc('\n', fp) != EOF;
    if (fclose(fp) != 0) {
        ok = 0;
    }
    cJSON_free(text);
    return ok ? 0 : -1;
}

static int jw__write_install_result_file(const char *path,
                                         const char *state,
                                         const char *message,
                                         const char *release_id) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "product", "leaf");
    cJSON_AddStringToObject(root, "operation", "install");
    cJSON_AddStringToObject(root, "state", state ? state : "error");
    cJSON_AddStringToObject(root, "message", message ? message : "");
    if (release_id && release_id[0]) {
        cJSON_AddStringToObject(root, "release_id", release_id);
    }
    cJSON_AddNumberToObject(root, "finished_at", (double)time(NULL));

    int rc = jw__write_json_file(path, root);
    cJSON_Delete(root);
    return rc;
}

static const char *jw__env_or_empty(const char *name) {
    const char *value = getenv(name);
    return value ? value : "";
}

static int jw__write_install_request(jw_update_status *status,
                                     const char *state_dir,
                                     const char *sdcard_root,
                                     const char *request_path,
                                     const char *result_path) {
    if (!status || !state_dir || !state_dir[0] ||
        !sdcard_root || !sdcard_root[0] ||
        !request_path || !request_path[0] ||
        !result_path || !result_path[0]) {
        return -1;
    }

    const char *trigger_file = status->handoff_trigger_file[0]
        ? status->handoff_trigger_file
        : "loong_upgrade";

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }

    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "product", "leaf");
    cJSON_AddStringToObject(root, "operation", "install");
    cJSON_AddNumberToObject(root, "created_at", (double)time(NULL));
    cJSON_AddStringToObject(root, "platform", status->platform_id);
    cJSON_AddStringToObject(root, "release_id", status->release_id);
    cJSON_AddStringToObject(root, "version",
                            status->version[0] ? status->version : status->release_id);
    cJSON_AddStringToObject(root, "state_dir", state_dir);
    cJSON_AddStringToObject(root, "sdcard_root", sdcard_root);
    cJSON_AddStringToObject(root, "runtime_path", jw__env_or_empty("UMRK_RUNTIME_PATH"));
    cJSON_AddStringToObject(root, "logs_path", jw__env_or_empty("LOGS_PATH"));
    cJSON_AddStringToObject(root, "marker_path", jw__env_or_empty("UMRK_MARKER_PATH"));
    cJSON_AddStringToObject(root, "request_path", request_path);
    cJSON_AddStringToObject(root, "result_path", result_path);
    cJSON_AddNumberToObject(root, "managed_apps_count", status->managed_apps_count);
    cJSON_AddNumberToObject(root, "migrations_count", status->migrations_count);

    cJSON *artifact = cJSON_CreateObject();
    cJSON_AddStringToObject(artifact, "kind", status->artifact_kind);
    cJSON_AddStringToObject(artifact, "name", status->artifact_name);
    cJSON_AddStringToObject(artifact, "path", status->download_path);
    cJSON_AddStringToObject(artifact, "sha256", status->artifact_sha256);
    cJSON_AddNumberToObject(artifact, "size", (double)status->artifact_size);
    cJSON_AddNumberToObject(artifact, "installed_size", (double)status->installed_size);
    cJSON_AddItemToObject(root, "artifact", artifact);

    cJSON *handoff = cJSON_CreateObject();
    cJSON_AddStringToObject(handoff, "type", status->handoff_type);
    cJSON_AddStringToObject(handoff, "completion", status->handoff_completion);
    cJSON_AddStringToObject(handoff, "trigger_file", trigger_file);
    cJSON_AddItemToObject(root, "handoff", handoff);

    int rc = jw__write_json_file(request_path, root);
    cJSON_Delete(root);
    return rc;
}

static void jw__read_install_result(jw_update_status *status,
                                    const char *result_path,
                                    int runner_status) {
    if (!status) {
        return;
    }

    char error[128];
    char *text = jw__read_text_file(result_path, error, sizeof(error));
    if (!text) {
        status->status = JW_UPDATE_STATUS_ERROR;
        status->install_armed = false;
        jw__copy_string(status->install_result_state,
                        sizeof(status->install_result_state), "error");
        status->install_result_release_id[0] = '\0';
        jw__copy_string(status->install_result_message,
                        sizeof(status->install_result_message),
                        "Update runner did not write a result file");
        jw__set_install_message(status, "result_missing",
                                "%s", "Update runner did not write a result file");
        jw__set_message(status, "%s", status->install_message);
        return;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        status->status = JW_UPDATE_STATUS_ERROR;
        status->install_armed = false;
        jw__copy_string(status->install_result_state,
                        sizeof(status->install_result_state), "error");
        status->install_result_release_id[0] = '\0';
        jw__copy_string(status->install_result_message,
                        sizeof(status->install_result_message),
                        "Update runner result was not valid JSON");
        jw__set_install_message(status, "result_invalid",
                                "%s", "Update runner result was not valid JSON");
        jw__set_message(status, "%s", status->install_message);
        return;
    }

    const char *state = jw__json_string(root, "state");
    const char *message = jw__json_string(root, "message");
    const char *release_id = jw__json_string(root, "release_id");
    if (!state || !state[0]) {
        state = "error";
    }
    if (!message) {
        message = "";
    }
    if (!release_id) {
        release_id = status->release_id;
    }
    jw__copy_string(status->install_result_state,
                    sizeof(status->install_result_state), state);
    jw__copy_string(status->install_result_release_id,
                    sizeof(status->install_result_release_id), release_id);
    jw__copy_string(status->install_result_message,
                    sizeof(status->install_result_message),
                    message[0] ? message : state);

    if ((strcmp(state, "armed") == 0 || strcmp(state, "installed") == 0) &&
        WIFEXITED(runner_status) && WEXITSTATUS(runner_status) == 0) {
        status->status = JW_UPDATE_STATUS_ARMED;
        status->install_armed = true;
        jw__set_install_message(status, state, "%s",
                                message[0] ? message : "Restart to finish installing");
        jw__set_message(status, "%s", status->install_message);
    } else {
        status->status = JW_UPDATE_STATUS_ERROR;
        status->install_armed = false;
        jw__set_install_message(status, state, "%s",
                                message[0] ? message : "Update install could not be prepared");
        jw__set_message(status, "%s", status->install_message);
    }

    cJSON_Delete(root);
}

static void jw__read_install_progress(jw_update_status *status,
                                      const char *result_path) {
    if (!status || !result_path || !result_path[0]) {
        return;
    }

    char error[128];
    char *text = jw__read_text_file(result_path, error, sizeof(error));
    if (!text) {
        return;
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return;
    }

    const char *state = jw__json_string(root, "state");
    const char *message = jw__json_string(root, "message");
    const char *release_id = jw__json_string(root, "release_id");
    if (!state || !state[0]) {
        state = "installing";
    }
    if (!message || !message[0]) {
        message = state;
    }
    if (!release_id) {
        release_id = status->release_id;
    }

    jw__copy_string(status->install_result_state,
                    sizeof(status->install_result_state), state);
    jw__copy_string(status->install_result_release_id,
                    sizeof(status->install_result_release_id), release_id);
    jw__copy_string(status->install_result_message,
                    sizeof(status->install_result_message), message);
    jw__copy_string(status->install_result_path,
                    sizeof(status->install_result_path), result_path);
    status->status = JW_UPDATE_STATUS_INSTALLING;
    status->install_active = true;
    status->install_armed = false;
    jw__set_install_message(status, "installing", "%s", message);
    jw__set_message(status, "%s", status->install_message);

    cJSON_Delete(root);
}

int jw_update_install_start(jw_update_status *status,
                            jw_update_install_job *job,
                            const char *state_dir,
                            const char *sdcard_root,
                            const char *runner_path) {
    if (!status || !job) {
        return -1;
    }
    if (job->active) {
        jw__set_message(status, "%s", "Update install is already in progress");
        return 0;
    }
    if (!status->install_ready) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update is not ready to install");
        return -1;
    }
    if (!runner_path || !runner_path[0] || access(runner_path, X_OK) != 0) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update runner is not available");
        return -1;
    }

    char update_dir[PATH_MAX];
    if (!jw__update_dir(update_dir, sizeof(update_dir), state_dir)) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Cannot prepare update state directory");
        return -1;
    }

    char request_path[JW_UPDATE_PATH_MAX];
    char result_path[JW_UPDATE_PATH_MAX];
    if (!jw__join_path(request_path, sizeof(request_path),
                       update_dir, "install-request.json") ||
        !jw__join_path(result_path, sizeof(result_path),
                       update_dir, "install-result.json")) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Update install state path is too long");
        return -1;
    }

    unlink(result_path);
    if (jw__write_install_request(status, state_dir, sdcard_root,
                                  request_path, result_path) != 0) {
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "%s", "Cannot write update install request");
        return -1;
    }
    jw__write_install_result_file(result_path, "starting",
                                  "Preparing update install",
                                  status->release_id);

    pid_t pid = fork();
    if (pid < 0) {
        jw__write_install_result_file(result_path, "error",
                                      "Cannot start update runner",
                                      status->release_id);
        status->status = JW_UPDATE_STATUS_ERROR;
        jw__set_message(status, "Cannot start update runner: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execl(runner_path, runner_path, "--request", request_path, (char *)NULL);
        _exit(127);
    }

    job->active = true;
    job->pid = pid;
    jw__copy_string(job->request_path, sizeof(job->request_path), request_path);
    jw__copy_string(job->result_path, sizeof(job->result_path), result_path);
    jw__copy_string(status->install_request_path,
                    sizeof(status->install_request_path), request_path);
    jw__copy_string(status->install_result_path,
                    sizeof(status->install_result_path), result_path);
    jw__copy_string(status->install_result_state,
                    sizeof(status->install_result_state), "starting");
    jw__copy_string(status->install_result_release_id,
                    sizeof(status->install_result_release_id),
                    status->release_id);
    jw__copy_string(status->install_result_message,
                    sizeof(status->install_result_message),
                    "Preparing update install");
    status->install_active = true;
    status->install_armed = false;
    status->status = JW_UPDATE_STATUS_INSTALLING;
    jw__set_install_message(status, "installing",
                            "%s", "Preparing update install");
    jw__set_message(status, "%s", status->install_message);
    return 0;
}

void jw_update_install_poll(jw_update_status *status,
                            jw_update_install_job *job) {
    if (!status || !job || !job->active || job->pid <= 0) {
        return;
    }

    int child_status = 0;
    pid_t waited = waitpid(job->pid, &child_status, WNOHANG);
    if (waited == 0) {
        status->install_active = true;
        if (status->status != JW_UPDATE_STATUS_INSTALLING) {
            status->status = JW_UPDATE_STATUS_INSTALLING;
        }
        jw__read_install_progress(status, job->result_path);
        return;
    }
    if (waited < 0) {
        if (errno == ECHILD) {
            job->active = false;
            status->install_active = false;
            status->status = JW_UPDATE_STATUS_ERROR;
            jw__copy_string(status->install_result_state,
                            sizeof(status->install_result_state), "error");
            status->install_result_release_id[0] = '\0';
            jw__copy_string(status->install_result_message,
                            sizeof(status->install_result_message),
                            "Update runner process disappeared");
            jw__set_install_message(status, "runner_missing",
                                    "%s", "Update runner process disappeared");
            jw__set_message(status, "%s", status->install_message);
        }
        return;
    }

    job->active = false;
    status->install_active = false;
    jw__read_install_result(status, job->result_path, child_status);
}

int jw_update_install_preflight(jw_update_status *status,
                                const char *state_dir,
                                const char *install_root,
                                bool device_idle,
                                int battery_percent,
                                int charging,
                                bool confirm_unknown_battery) {
    (void)state_dir;
    if (!status) {
        return -1;
    }

    jw__clear_install_preflight(status);
    status->install_idle = device_idle;
    status->install_battery_percent = battery_percent;
    status->install_charging = charging;

    long long required = 0;
    if (status->artifact_size > 0) {
        if (status->installed_size > 0) {
            required = status->artifact_size + status->installed_size +
                       JW_UPDATE_INSTALL_HEADROOM_BYTES;
        } else {
            required = (2LL * status->artifact_size) +
                       JW_UPDATE_INSTALL_HEADROOM_BYTES;
        }
    }
    status->install_required_free = required;

    struct statvfs fs;
    if (install_root && install_root[0] && statvfs(install_root, &fs) == 0) {
        status->install_available_free =
            (long long)fs.f_bavail * (long long)fs.f_frsize;
    } else {
        status->install_available_free = -1;
    }

    if (status->install_active) {
        status->install_blocked = true;
        jw__set_install_message(status, "install_active",
                                "%s", "Update install is already in progress");
        return 0;
    }

    if (status->install_armed) {
        status->install_blocked = true;
        jw__set_install_message(status, "restart_needed",
                                "%s", "Restart to finish installing");
        return 0;
    }

    if (status->download_active) {
        status->install_blocked = true;
        jw__set_install_message(status, "download_active",
                                "%s", "Update download is still in progress");
        return 0;
    }

    if (!status->downloaded || !status->download_path[0]) {
        status->install_blocked = true;
        jw__set_install_message(status, "not_downloaded",
                                "%s", "Update artifact has not been downloaded");
        return 0;
    }

    struct stat downloaded_st;
    if (stat(status->download_path, &downloaded_st) != 0 ||
        !S_ISREG(downloaded_st.st_mode)) {
        status->install_blocked = true;
        jw__set_install_message(status, "download_missing",
                                "%s", "Downloaded update artifact is missing");
        return 0;
    }

    if (strcmp(status->artifact_kind, "sd_root_zip") != 0) {
        status->install_blocked = true;
        jw__set_install_message(status, "unsupported_artifact",
                                "Unsupported update artifact kind: %s",
                                status->artifact_kind[0] ? status->artifact_kind : "unknown");
        return 0;
    }

    if (strcmp(status->handoff_type, "stock_loong_upgrade") == 0) {
        if (strcmp(status->platform_id, "mlp1") != 0 ||
            (status->handoff_completion[0] &&
             strcmp(status->handoff_completion, "reboot") != 0)) {
            status->install_blocked = true;
            jw__set_install_message(status, "unsupported_handoff",
                                    "Unsupported stock update method for %s",
                                    status->platform_id[0] ? status->platform_id : "unknown");
            return 0;
        }
        if (status->handoff_trigger_file[0] &&
            !jw__filename_safe(status->handoff_trigger_file)) {
            status->install_blocked = true;
            jw__set_install_message(status, "unsupported_handoff",
                                    "Unsafe stock update trigger: %s",
                                    status->handoff_trigger_file);
            return 0;
        }
    } else if (strcmp(status->handoff_type, "direct_runner") != 0 &&
               strcmp(status->handoff_type, "generic_runner") != 0 &&
               strcmp(status->handoff_type, "jawaka_c_runner") != 0) {
        status->install_blocked = true;
        jw__set_install_message(status, "unsupported_handoff",
                                "Unsupported update install method: %s",
                                status->handoff_type[0] ? status->handoff_type : "unknown");
        return 0;
    }

    if (!device_idle) {
        status->install_blocked = true;
        jw__set_install_message(status, "not_idle",
                                "%s", "Close running games or apps before installing");
        return 0;
    }

    if (battery_percent < 0) {
        if (!confirm_unknown_battery) {
            status->install_needs_confirmation = true;
            jw__set_install_message(status, "battery_unknown",
                                    "%s", "Battery status is unknown; confirmation required");
            return 0;
        }
    } else if (battery_percent < JW_UPDATE_MIN_INSTALL_BATTERY_PERCENT &&
               charging != 1) {
        status->install_blocked = true;
        jw__set_install_message(status, "battery_low",
                                "Battery must be at least %d%% or charging",
                                JW_UPDATE_MIN_INSTALL_BATTERY_PERCENT);
        return 0;
    }

    if (required > 0 &&
        status->install_available_free >= 0 &&
        status->install_available_free < required) {
        status->install_blocked = true;
        jw__set_install_message(status, "space_low",
                                "%s", "Not enough free space to install update");
        return 0;
    }

    if (status->install_available_free < 0) {
        status->install_needs_confirmation = true;
        jw__set_install_message(status, "space_unknown",
                                "%s", "Free space could not be checked; confirmation required");
        return 0;
    }

    status->install_ready = true;
    jw__set_install_message(status, "ready", "%s", "Update is ready to install");
    return 0;
}

cJSON *jw_update_status_to_json(const jw_update_status *status) {
    cJSON *root = cJSON_CreateObject();
    if (!status) {
        cJSON_AddStringToObject(root, "type", "update-status");
        cJSON_AddStringToObject(root, "state", "error");
        cJSON_AddStringToObject(root, "message", "update status unavailable");
        return root;
    }

    cJSON_AddStringToObject(root, "type", "update-status");
    cJSON_AddStringToObject(root, "state", jw_update_status_name(status->status));
    cJSON_AddStringToObject(root, "platform_id", status->platform_id);
    cJSON_AddBoolToObject(root, "compatible", status->compatible);
    cJSON_AddBoolToObject(root, "has_update", status->has_update);
    cJSON_AddStringToObject(root, "message", status->message);
    cJSON_AddNumberToObject(root, "selected_option", status->selected_option);
    if (status->checked_at > 0) {
        cJSON_AddNumberToObject(root, "checked_at", (double)status->checked_at);
    } else {
        cJSON_AddNullToObject(root, "checked_at");
    }
    jw__json_add_string_or_null(root, "source_manifest", status->source_manifest);

    cJSON *current = cJSON_CreateObject();
    cJSON_AddBoolToObject(current, "unknown", status->current_unknown);
    cJSON_AddNumberToObject(current, "schema", status->installed_schema);
    jw__json_add_string_or_null(current, "release_id", status->current_release_id);
    jw__json_add_string_or_null(current, "version", status->current_version);
    cJSON_AddItemToObject(root, "current", current);

    cJSON *candidate = cJSON_CreateObject();
    jw__json_add_string_or_null(candidate, "release_id", status->release_id);
    jw__json_add_string_or_null(candidate, "version", status->version);
    jw__json_add_string_or_null(candidate, "published_at", status->published_at);
    jw__json_add_string_or_null(candidate, "notes_url", status->notes_url);
    cJSON_AddItemToObject(root, "candidate", candidate);

    cJSON *artifact = cJSON_CreateObject();
    jw__json_add_string_or_null(artifact, "kind", status->artifact_kind);
    jw__json_add_string_or_null(artifact, "name", status->artifact_name);
    jw__json_add_string_or_null(artifact, "sha256", status->artifact_sha256);
    cJSON_AddNumberToObject(artifact, "size", (double)status->artifact_size);
    cJSON_AddNumberToObject(artifact, "installed_size", (double)status->installed_size);
    cJSON_AddItemToObject(root, "artifact", artifact);

    cJSON *handoff = cJSON_CreateObject();
    jw__json_add_string_or_null(handoff, "type", status->handoff_type);
    jw__json_add_string_or_null(handoff, "completion", status->handoff_completion);
    jw__json_add_string_or_null(handoff, "trigger_file",
                                status->handoff_trigger_file);
    cJSON_AddItemToObject(root, "handoff", handoff);

    cJSON_AddNumberToObject(root, "managed_apps_count", status->managed_apps_count);
    cJSON_AddNumberToObject(root, "migrations_count", status->migrations_count);
    cJSON *options = cJSON_CreateArray();
    for (int i = 0; i < status->option_count && i < JW_UPDATE_MAX_OPTIONS; i++) {
        const jw_update_option *option = &status->options[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", option->index);
        cJSON_AddBoolToObject(item, "selected", i == status->selected_option);
        cJSON_AddBoolToObject(item, "installed", option->installed);
        jw__json_add_string_or_null(item, "release_id", option->release_id);
        jw__json_add_string_or_null(item, "version", option->version);
        jw__json_add_string_or_null(item, "published_at", option->published_at);
        jw__json_add_string_or_null(item, "notes_url", option->notes_url);
        jw__json_add_string_or_null(item, "artifact_kind", option->artifact_kind);
        jw__json_add_string_or_null(item, "artifact_name", option->artifact_name);
        cJSON_AddNumberToObject(item, "artifact_size",
                                (double)option->artifact_size);
        cJSON_AddNumberToObject(item, "installed_size",
                                (double)option->installed_size);
        cJSON_AddItemToArray(options, item);
    }
    cJSON_AddItemToObject(root, "options", options);
    cJSON_AddBoolToObject(root, "options_complete", status->options_complete);
    cJSON_AddBoolToObject(root, "options_loading", status->options_loading);
    jw__json_add_string_or_null(root, "recovery_name", status->recovery_name);
    jw__json_add_string_or_null(root, "manifest_url", status->manifest_url);
    jw__json_add_string_or_null(root, "artifact_url", status->artifact_url);
    jw__json_add_string_or_null(root, "recovery_url", status->recovery_url);
    cJSON_AddBoolToObject(root, "downloaded", status->downloaded);
    cJSON_AddBoolToObject(root, "download_active", status->download_active);
    cJSON_AddNumberToObject(root, "download_received",
                            (double)status->download_received);
    cJSON_AddNumberToObject(root, "download_total",
                            (double)status->download_total);
    if (status->download_percent >= 0) {
        cJSON_AddNumberToObject(root, "download_percent",
                                status->download_percent);
    } else {
        cJSON_AddNullToObject(root, "download_percent");
    }
    jw__json_add_string_or_null(root, "download_path", status->download_path);

    cJSON *install = cJSON_CreateObject();
    cJSON_AddBoolToObject(install, "ready", status->install_ready);
    cJSON_AddBoolToObject(install, "blocked", status->install_blocked);
    cJSON_AddBoolToObject(install, "needs_confirmation",
                          status->install_needs_confirmation);
    cJSON_AddBoolToObject(install, "active", status->install_active);
    cJSON_AddBoolToObject(install, "armed", status->install_armed);
    cJSON_AddBoolToObject(install, "idle", status->install_idle);
    cJSON_AddNumberToObject(install, "battery_percent",
                            status->install_battery_percent);
    cJSON_AddNumberToObject(install, "charging", status->install_charging);
    cJSON_AddNumberToObject(install, "required_free",
                            (double)status->install_required_free);
    cJSON_AddNumberToObject(install, "available_free",
                            (double)status->install_available_free);
    jw__json_add_string_or_null(install, "result_state",
                                status->install_result_state);
    jw__json_add_string_or_null(install, "result_release_id",
                                status->install_result_release_id);
    jw__json_add_string_or_null(install, "result_message",
                                status->install_result_message);
    jw__json_add_string_or_null(install, "request_path",
                                status->install_request_path);
    jw__json_add_string_or_null(install, "result_path",
                                status->install_result_path);
    jw__json_add_string_or_null(install, "reason", status->install_reason);
    jw__json_add_string_or_null(install, "message", status->install_message);
    cJSON_AddItemToObject(root, "install", install);

    return root;
}
