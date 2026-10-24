#include "internal/scrape/ss_client.h"
#include "internal/scrape/scrape_md5.h"
#include "internal/core/scraped_text.h"

#include "cJSON.h"

#include <curl/curl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#include "miniz.h"

/* ── Credentials ──────────────────────────────────────────────────────── */

bool jw_ss_available(void) {
    return SCREENSCRAPER_DEV_ID[0] != '\0' &&
           SCREENSCRAPER_DEV_PASSWORD[0] != '\0';
}

bool jw_ss_is_debug(void) {
    return SCREENSCRAPER_DEBUG_PASSWORD[0] != '\0';
}

/* ── Error helpers (thread-local, like the rest of the daemon workers) ── */

static _Thread_local int jw__ss_errno;
static _Thread_local char jw__ss_error[256];

static void jw__ss_clear_error(void) {
    jw__ss_error[0] = '\0';
}

static void jw__ss_clear_errno(void) {
    jw__ss_errno = 0;
}

int jw_ss_last_errno(void) {
    return jw__ss_errno;
}

static void jw__ss_set_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(jw__ss_error, sizeof(jw__ss_error), fmt, args);
    va_end(args);
}

const char *jw_ss_last_error(void) {
    return jw__ss_error[0] ? jw__ss_error : NULL;
}

#ifdef JW_SS_TESTING
static jw_ss_test_search_request_fn jw__test_search_request;
static void *jw__test_search_request_userdata;

void jw_ss_test_set_search_request(jw_ss_test_search_request_fn request,
                                   void *userdata) {
    jw__test_search_request = request;
    jw__test_search_request_userdata = userdata;
}
#endif

static void jw__ss_progress(const jw_ss_client *client, jw_ss_phase phase) {
    if (client && client->progress) {
        client->progress(client->progress_userdata, phase);
    }
}

/* ── cURL plumbing ────────────────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t size;
} jw__curl_buffer;

/* Hard ceiling on any single HTTP response we buffer in RAM. Box art is well
   under a few MB; this guards a 1GB device against an unbounded body (up to 4
   workers download concurrently). */
#define JW__SS_MAX_BODY_BYTES (16 * 1024 * 1024)

static pthread_once_t jw__curl_once = PTHREAD_ONCE_INIT;
static CURLcode jw__curl_global_result = CURLE_OK;

static void jw__curl_global_init_once(void) {
    jw__curl_global_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}

static bool jw__cancelled(atomic_int *interrupt) {
    return interrupt && atomic_load(interrupt) != 0;
}

static int jw__sleep_with_cancel(int backoff_ms, atomic_int *interrupt) {
    int remaining = backoff_ms;
    while (remaining > 0) {
        int chunk_ms = remaining > 50 ? 50 : remaining;
        if (jw__cancelled(interrupt))
            return -1;
        usleep((useconds_t)chunk_ms * 1000U);
        remaining -= chunk_ms;
    }
    return 0;
}

/* Same CA strategy as internal/update: prefer the launcher's bundled
   Mozilla roots (stock-firmware libcurl may default to a CA path that does
   not exist on device), then SSL_CERT_FILE, then libcurl's default. */
static const char *jw__ss_ca_path(char *buf, size_t buf_size) {
    const char *bundle = getenv("UMRK_LAUNCHER_PATH");
    if (bundle && bundle[0]) {
        int n = snprintf(buf, buf_size, "%s/res/certs/cacert.pem", bundle);
        if (n > 0 && n < (int)buf_size && access(buf, R_OK) == 0)
            return buf;
    }
    const char *ca = getenv("SSL_CERT_FILE");
    return (ca && ca[0]) ? ca : NULL;
}

static int jw__curl_xferinfo_cb(void *clientp, curl_off_t dltotal,
                                curl_off_t dlnow, curl_off_t ultotal,
                                curl_off_t ulnow) {
    (void)dltotal; (void)dlnow; (void)ultotal; (void)ulnow;
    return jw__cancelled((atomic_int *)clientp) ? 1 : 0;
}

static size_t jw__curl_write_cb(void *ptr, size_t size, size_t nmemb,
                                void *userdata) {
    size_t total = size * nmemb;
    jw__curl_buffer *buf = (jw__curl_buffer *)userdata;
    if (buf->size + total > JW__SS_MAX_BODY_BYTES)
        return 0; /* abort the transfer: response exceeds the in-RAM ceiling */
    char *tmp = realloc(buf->data, buf->size + total + 1);
    if (!tmp)
        return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/* GET with retries and exponential backoff. Returns the HTTP status code,
   -1 on error, -2 when cancelled. */
static int jw__http_get(const jw_ss_client *client, const char *url,
                        jw__curl_buffer *out_buf, int max_retries,
                        char *content_type_out, size_t content_type_len) {
    atomic_int *interrupt = client ? client->interrupt : NULL;
    char ca_buf[PATH_MAX];
    const char *ca = jw__ss_ca_path(ca_buf, sizeof(ca_buf));

    out_buf->data = NULL;
    out_buf->size = 0;
    if (content_type_out && content_type_len > 0)
        content_type_out[0] = '\0';

    pthread_once(&jw__curl_once, jw__curl_global_init_once);
    if (jw__curl_global_result != CURLE_OK) {
        jw__ss_set_error("curl global init failed: %s",
                         curl_easy_strerror(jw__curl_global_result));
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        jw__ss_set_error("curl init failed");
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, JW_SS_USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, jw__curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 100L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     (curl_off_t)JW__SS_MAX_BODY_BYTES);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, jw__curl_xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, interrupt);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    if (ca)
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca);

    long http_code = 0;
    int backoff_ms = 1000;

    for (int attempt = 0; attempt <= max_retries; attempt++) {
        if (attempt > 0) {
            free(out_buf->data);
            out_buf->data = NULL;
            out_buf->size = 0;
            if (content_type_out && content_type_len > 0)
                content_type_out[0] = '\0';
            if (jw__sleep_with_cancel(backoff_ms, interrupt) != 0) {
                curl_easy_cleanup(curl);
                return -2;
            }
            backoff_ms *= 2;
        }

        if (jw__cancelled(interrupt)) {
            curl_easy_cleanup(curl);
            return -2;
        }

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_ABORTED_BY_CALLBACK || jw__cancelled(interrupt)) {
            curl_easy_cleanup(curl);
            return -2;
        }
        if (res != CURLE_OK) {
            if (attempt < max_retries)
                continue;
            jw__ss_set_error("request failed: %s", curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            return -1;
        }

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (content_type_out && content_type_len > 0) {
            char *content_type = NULL;
            if (curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE,
                                  &content_type) == CURLE_OK &&
                content_type != NULL) {
                snprintf(content_type_out, content_type_len, "%s", content_type);
            }
        }

        if (http_code >= 500 && attempt < max_retries)
            continue;
        break;
    }

    curl_easy_cleanup(curl);
    return (int)http_code;
}

/* ── URL building ─────────────────────────────────────────────────────── */

/* Append "&key=urlencode(value)" to a malloc'd URL under construction.
   Returns false on encode failure or if the append would truncate; truncation
   is fatal because advancing *len past url_cap underflows the next size_t
   capacity calc and writes out of bounds. */
static bool jw__url_append_kv(CURL *curl, char *url, size_t url_cap,
                              int *len, const char *key, const char *value) {
    char *enc = curl_easy_escape(curl, value, 0);
    if (!enc)
        return false;
    int r = snprintf(url + *len, url_cap - (size_t)*len, "&%s=%s", key, enc);
    curl_free(enc);
    if (r < 0 || (size_t)r >= url_cap - (size_t)*len)
        return false;
    *len += r;
    return true;
}

static char *jw__build_api_url(const jw_ss_client *client, const char *endpoint,
                               const char *rom_name, const char *md5_hash,
                               long file_size, int system_id) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        jw__ss_set_error("curl init failed while building request");
        return NULL;
    }

    /* Two 255-char credentials plus an escaped ROM name (escaping ~triples a
       worst-case value) legitimately exceed 2048. */
    size_t url_cap = 4096;
    char *url = malloc(url_cap);
    if (!url) {
        jw__ss_set_error("out of memory while building request");
        curl_easy_cleanup(curl);
        return NULL;
    }

    char *enc_devid = curl_easy_escape(curl, SCREENSCRAPER_DEV_ID, 0);
    char *enc_devpwd = curl_easy_escape(curl, SCREENSCRAPER_DEV_PASSWORD, 0);
    if (!enc_devid || !enc_devpwd) {
        jw__ss_set_error("failed to encode ScreenScraper request");
        goto fail;
    }
    int len = snprintf(url, url_cap,
        "%s/%s?devid=%s&devpassword=%s&softname=%s&output=json",
        JW_SS_API_BASE, endpoint, enc_devid, enc_devpwd, JW_SS_SOFTNAME);
    curl_free(enc_devid);
    curl_free(enc_devpwd);
    enc_devid = enc_devpwd = NULL;
    if (len < 0 || (size_t)len >= url_cap) {
        jw__ss_set_error("ScreenScraper request URL too long");
        goto fail;
    }

    if (client->username[0]) {
        if (!jw__url_append_kv(curl, url, url_cap, &len, "ssid",
                               client->username) ||
            !jw__url_append_kv(curl, url, url_cap, &len, "sspassword",
                               client->password)) {
            jw__ss_set_error("failed to encode ScreenScraper credentials");
            goto fail;
        }
    }

    if (rom_name) {
        if (!jw__url_append_kv(curl, url, url_cap, &len, "romnom", rom_name)) {
            jw__ss_set_error("failed to encode ScreenScraper request");
            goto fail;
        }
        int r = snprintf(url + len, url_cap - (size_t)len,
                         "&systemeid=%d", system_id);
        if (r < 0 || (size_t)r >= url_cap - (size_t)len) {
            jw__ss_set_error("ScreenScraper request URL too long");
            goto fail;
        }
        len += r;
        if (md5_hash && md5_hash[0]) {
            if (!jw__url_append_kv(curl, url, url_cap, &len, "md5", md5_hash)) {
                jw__ss_set_error("failed to encode ScreenScraper request");
                goto fail;
            }
            r = snprintf(url + len, url_cap - (size_t)len,
                         "&romtaille=%ld", file_size);
            if (r < 0 || (size_t)r >= url_cap - (size_t)len) {
                jw__ss_set_error("ScreenScraper request URL too long");
                goto fail;
            }
            len += r;
        }
    }

    if (jw_ss_is_debug()) {
        if (!jw__url_append_kv(curl, url, url_cap, &len, "devdebugpassword",
                               SCREENSCRAPER_DEBUG_PASSWORD)) {
            jw__ss_set_error("failed to encode ScreenScraper debug credentials");
            goto fail;
        }
    }

    curl_easy_cleanup(curl);
    return url;

fail:
    if (enc_devid) curl_free(enc_devid);
    if (enc_devpwd) curl_free(enc_devpwd);
    free(url);
    curl_easy_cleanup(curl);
    return NULL;
}

/* ── Response parsing helpers ─────────────────────────────────────────── */

/* ScreenScraper serialises numbers inconsistently (string or number). */
static int jw__json_int(const cJSON *obj, const char *key, int fallback) {
    const cJSON *v = cJSON_GetObjectItem((cJSON *)obj, key);
    if (!v)
        return fallback;
    if (cJSON_IsNumber(v))
        return (int)v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0])
        return atoi(v->valuestring);
    return fallback;
}

static const char *jw__http_status_error(int code) {
    switch (code) {
    case 429: return "Thread limit reached (HTTP 429)";
    case 430: return "Daily quota exceeded (HTTP 430)";
    case 431: return "Too many unrecognized ROMs (HTTP 431)";
    case 423: return "ScreenScraper API temporarily closed (HTTP 423)";
    case 426: return "Software has been blacklisted (HTTP 426)";
    default:  return NULL;
    }
}

static void jw__resolve_game_name(cJSON *names, const char *const *region_prio,
                                  int region_count, char *name_out, size_t name_len);

/* ScreenScraper wraps most single facts as { "text": ... }; a few are plain
   strings. Both shapes appear in the wild, so accept either. */
static void jw__ss_field(cJSON *jeu, const char *key, char *out, size_t out_len) {
    out[0] = '\0';
    cJSON *node = cJSON_GetObjectItem(jeu, key);
    if (!node) return;
    if (cJSON_IsString(node)) {
        snprintf(out, out_len, "%s", node->valuestring);
        jw_clean_scraped_text(out);
        return;
    }
    cJSON *text = cJSON_GetObjectItem(node, "text");
    if (cJSON_IsString(text)) snprintf(out, out_len, "%s", text->valuestring);
    jw_clean_scraped_text(out);
}

/* Pick a localised string out of an array of { "langue": ..., "text": ... },
   preferring English and otherwise taking whatever the first entry offers, so a
   game documented only in French still says something. */
static void jw__ss_lang_text(cJSON *arr, char *out, size_t out_len) {
    out[0] = '\0';
    if (!cJSON_IsArray(arr)) return;
    int count = cJSON_GetArraySize(arr);
    for (int i = 0; i < count; i++) {
        cJSON *e = cJSON_GetArrayItem(arr, i);
        cJSON *lang = cJSON_GetObjectItem(e, "langue");
        cJSON *text = cJSON_GetObjectItem(e, "text");
        if (cJSON_IsString(lang) && cJSON_IsString(text) &&
            strcmp(lang->valuestring, "en") == 0) {
            snprintf(out, out_len, "%s", text->valuestring);
            jw_clean_scraped_text(out);
            return;
        }
    }
    for (int i = 0; i < count; i++) {
        cJSON *text = cJSON_GetObjectItem(cJSON_GetArrayItem(arr, i), "text");
        if (cJSON_IsString(text)) {
            snprintf(out, out_len, "%s", text->valuestring);
            jw_clean_scraped_text(out);
            return;
        }
    }
}

/* Genres arrive as a list, each with its own localised names. One is enough for
   a chip, so take the first. */
static void jw__ss_genre(cJSON *jeu, char *out, size_t out_len) {
    out[0] = '\0';
    cJSON *genres = cJSON_GetObjectItem(jeu, "genres");
    if (!cJSON_IsArray(genres) || cJSON_GetArraySize(genres) == 0) return;
    jw__ss_lang_text(cJSON_GetObjectItem(cJSON_GetArrayItem(genres, 0), "noms"),
                     out, out_len);
}

/* Release dates are per region and written "YYYY-MM-DD" or just "YYYY"; only the
   year is worth a chip. */
static void jw__ss_year(cJSON *jeu, const char *const *region_prio, int region_count,
                        char *out, size_t out_len) {
    out[0] = '\0';
    char date[32];
    jw__resolve_game_name(cJSON_GetObjectItem(jeu, "dates"), region_prio, region_count,
                          date, sizeof(date));
    if (strlen(date) < 4 || out_len < 5) return;
    memcpy(out, date, 4);
    out[4] = '\0';
}

static int jw__resolve_media(cJSON *medias,
                             const char *const *media_types, int type_count,
                             const char *const *region_prio, int region_count,
                             char *url_out, size_t url_len,
                             char *format_out, size_t format_len) {
    if (!cJSON_IsArray(medias))
        return -1;

    for (int t = 0; t < type_count; t++) {
        int media_count = cJSON_GetArraySize(medias);
        cJSON *first_match = NULL;

        for (int r = 0; r < region_count; r++) {
            for (int m = 0; m < media_count; m++) {
                cJSON *media = cJSON_GetArrayItem(medias, m);
                cJSON *type = cJSON_GetObjectItem(media, "type");
                cJSON *region = cJSON_GetObjectItem(media, "region");
                if (!cJSON_IsString(type) ||
                    strcmp(type->valuestring, media_types[t]) != 0)
                    continue;
                if (!first_match)
                    first_match = media;
                if (cJSON_IsString(region) &&
                    strcmp(region->valuestring, region_prio[r]) == 0) {
                    cJSON *murl = cJSON_GetObjectItem(media, "url");
                    cJSON *mfmt = cJSON_GetObjectItem(media, "format");
                    if (cJSON_IsString(murl)) {
                        snprintf(url_out, url_len, "%s", murl->valuestring);
                        snprintf(format_out, format_len, "%s",
                                 cJSON_IsString(mfmt) ? mfmt->valuestring : "png");
                        return 0;
                    }
                }
            }
        }

        /* No region match: take the first media of this type. */
        if (first_match) {
            cJSON *murl = cJSON_GetObjectItem(first_match, "url");
            cJSON *mfmt = cJSON_GetObjectItem(first_match, "format");
            if (cJSON_IsString(murl)) {
                snprintf(url_out, url_len, "%s", murl->valuestring);
                snprintf(format_out, format_len, "%s",
                         cJSON_IsString(mfmt) ? mfmt->valuestring : "png");
                return 0;
            }
        }
    }

    return -1;
}

static void jw__resolve_game_name(cJSON *names,
                                  const char *const *region_prio,
                                  int region_count,
                                  char *name_out, size_t name_len) {
    name_out[0] = '\0';
    if (!cJSON_IsArray(names) || cJSON_GetArraySize(names) == 0)
        return;

    for (int r = 0; r < region_count; r++) {
        int count = cJSON_GetArraySize(names);
        for (int i = 0; i < count; i++) {
            cJSON *entry = cJSON_GetArrayItem(names, i);
            cJSON *region = cJSON_GetObjectItem(entry, "region");
            cJSON *text = cJSON_GetObjectItem(entry, "text");
            if (cJSON_IsString(region) && cJSON_IsString(text) &&
                strcmp(region->valuestring, region_prio[r]) == 0) {
                snprintf(name_out, name_len, "%s", text->valuestring);
                return;
            }
        }
    }

    cJSON *first = cJSON_GetArrayItem(names, 0);
    cJSON *text = cJSON_GetObjectItem(first, "text");
    if (cJSON_IsString(text))
        snprintf(name_out, name_len, "%s", text->valuestring);
}

/* ── ROM search ───────────────────────────────────────────────────────── */

static jw_ss_search_status jw__search_request(
    const jw_ss_client *client, const char *rom_name,
    const char *md5_hash, long file_size, int system_id,
    const char *const *artwork_types, int artwork_count,
    const char *const *region_prio, int region_count,
    jw_ss_result *result) {
#ifdef JW_SS_TESTING
    if (jw__test_search_request) {
        return jw__test_search_request(
            rom_name, md5_hash, file_size, system_id, result,
            jw__test_search_request_userdata);
    }
#endif
    char *url = jw__build_api_url(client, "jeuInfos.php", rom_name,
                                  md5_hash, file_size, system_id);
    if (!url)
        return JW_SS_SEARCH_ERROR;

    jw__curl_buffer buf;
    int http_code = jw__http_get(client, url, &buf, 2, NULL, 0);
    free(url);

    if (http_code == -2) {
        free(buf.data);
        return JW_SS_SEARCH_CANCELLED;
    }
    if (http_code < 0) {
        if (!jw_ss_last_error())
            jw__ss_set_error("ScreenScraper request failed");
        free(buf.data);
        return JW_SS_SEARCH_ERROR;
    }
    if (http_code == 404) {
        free(buf.data);
        return JW_SS_SEARCH_NOT_FOUND;
    }
    if (http_code >= 400) {
        const char *msg = jw__http_status_error(http_code);
        if (msg)
            jw__ss_set_error("%s", msg);
        else
            jw__ss_set_error("ScreenScraper returned HTTP %d", http_code);
        free(buf.data);
        return JW_SS_SEARCH_ERROR;
    }
    if (!buf.data || buf.size == 0) {
        jw__ss_set_error("ScreenScraper returned an empty response");
        free(buf.data);
        return JW_SS_SEARCH_ERROR;
    }

    cJSON *json = cJSON_Parse(buf.data);
    free(buf.data);
    if (!json) {
        jw__ss_set_error("Failed to parse ScreenScraper response");
        return JW_SS_SEARCH_ERROR;
    }

    cJSON *response = cJSON_GetObjectItem(json, "response");
    if (!response) {
        cJSON_Delete(json);
        jw__ss_set_error("ScreenScraper response is missing 'response'");
        return JW_SS_SEARCH_ERROR;
    }

    cJSON *jeu = cJSON_GetObjectItem(response, "jeu");
    if (!jeu) {
        cJSON_Delete(json);
        return JW_SS_SEARCH_NOT_FOUND;
    }
    cJSON *game_id = cJSON_GetObjectItem(jeu, "id");
    if (!game_id || !cJSON_IsString(game_id) || game_id->valuestring[0] == '\0') {
        cJSON_Delete(json);
        return JW_SS_SEARCH_NOT_FOUND;
    }

    jw__resolve_game_name(cJSON_GetObjectItem(jeu, "noms"),
                          region_prio, region_count,
                          result->game_name, sizeof(result->game_name));

    if (jw__resolve_media(cJSON_GetObjectItem(jeu, "medias"),
                          artwork_types, artwork_count,
                          region_prio, region_count,
                          result->media_url, sizeof(result->media_url),
                          result->media_format, sizeof(result->media_format)) != 0) {
        cJSON_Delete(json);
        return JW_SS_SEARCH_NO_MEDIA;
    }

    /* The rest of the response, kept rather than discarded. Every one of these
       is optional and an absent field simply stays empty. */
    jw__ss_genre(jeu, result->genre, sizeof(result->genre));
    jw__ss_field(jeu, "developpeur", result->developer, sizeof(result->developer));
    jw__ss_field(jeu, "editeur",     result->publisher, sizeof(result->publisher));
    jw__ss_field(jeu, "joueurs",     result->players,   sizeof(result->players));
    jw__ss_field(jeu, "note",        result->rating,    sizeof(result->rating));
    jw__ss_year(jeu, region_prio, region_count, result->year, sizeof(result->year));
    jw__ss_lang_text(cJSON_GetObjectItem(jeu, "synopsis"),
                     result->synopsis, sizeof(result->synopsis));

    cJSON *ssuser = cJSON_GetObjectItem(response, "ssuser");
    if (ssuser) {
        result->requests_today = jw__json_int(ssuser, "requeststoday", 0);
        result->max_requests = jw__json_int(ssuser, "maxrequestsperday", 0);
        result->max_threads = jw__json_int(ssuser, "maxthreads", 1);
    }

    cJSON_Delete(json);
    return JW_SS_SEARCH_FOUND;
}

jw_ss_search_status jw_ss_search_rom_platforms(
    const jw_ss_client *client, const char *rom_name,
    const char *rom_abs_path, const int *system_ids, size_t system_count,
    const char *const *artwork_types, int artwork_count,
    const char *const *region_prio, int region_count,
    jw_ss_result *result) {
    jw__ss_clear_error();
    memset(result, 0, sizeof(*result));

    if (!jw_ss_available()
#ifdef JW_SS_TESTING
        && !jw__test_search_request
#endif
    ) {
        jw__ss_set_error("Scraping is unavailable in this build (no API credentials)");
        return JW_SS_SEARCH_ERROR;
    }
    if (!system_ids || system_count == 0) {
        jw__ss_set_error("No ScreenScraper platform ids provided");
        return JW_SS_SEARCH_ERROR;
    }

    char md5_hash[33] = {0};
    long file_size = 0;
    if (rom_abs_path) {
        jw__ss_progress(client, JW_SS_PHASE_HASHING);
    }
    if (rom_abs_path &&
        jw_scrape_md5(rom_abs_path, md5_hash, &file_size) != 0) {
        md5_hash[0] = '\0';
        file_size = 0;
    }

    bool saw_no_media = false;
    for (size_t i = 0; i < system_count; i++) {
        memset(result, 0, sizeof(*result));
        jw__ss_progress(client, JW_SS_PHASE_SEARCHING);
        jw_ss_search_status ret = jw__search_request(
            client, rom_name, md5_hash, file_size, system_ids[i],
            artwork_types, artwork_count, region_prio, region_count, result);
        if (ret == JW_SS_SEARCH_NO_MEDIA)
            saw_no_media = true;

        /* A hash may identify a sparse clone entry while the same name finds
           a media-bearing parent. Preserve that recovery for both not-found
           and no-media before advancing to another declared platform. */
        if ((ret == JW_SS_SEARCH_NOT_FOUND ||
             ret == JW_SS_SEARCH_NO_MEDIA) && md5_hash[0] != '\0') {
            memset(result, 0, sizeof(*result));
            jw__ss_progress(client, JW_SS_PHASE_SEARCHING);
            ret = jw__search_request(client, rom_name, "", 0, system_ids[i],
                                     artwork_types, artwork_count,
                                     region_prio, region_count, result);
            if (ret == JW_SS_SEARCH_NO_MEDIA)
                saw_no_media = true;
        }
        if (ret == JW_SS_SEARCH_FOUND || ret < 0)
            return ret;
    }

    return saw_no_media ? JW_SS_SEARCH_NO_MEDIA : JW_SS_SEARCH_NOT_FOUND;
}

jw_ss_search_status jw_ss_search_rom(
    const jw_ss_client *client,
    const char *rom_name, const char *rom_abs_path,
    int system_id,
    const char *const *artwork_types, int artwork_count,
    const char *const *region_prio, int region_count,
    jw_ss_result *result) {
    return jw_ss_search_rom_platforms(
        client, rom_name, rom_abs_path, &system_id, 1,
        artwork_types, artwork_count, region_prio, region_count, result);
}

/* ── User validation (ssuserInfos.php) ────────────────────────────────── */

int jw_ss_validate_user(const jw_ss_client *client, jw_ss_user *out) {
    jw__ss_clear_error();
    memset(out, 0, sizeof(*out));

    if (!jw_ss_available()) {
        jw__ss_set_error("Scraping is unavailable in this build (no API credentials)");
        return -1;
    }
    if (!client->username[0]) {
        jw__ss_set_error("No ScreenScraper username configured");
        return 1;
    }

    char *url = jw__build_api_url(client, "ssuserInfos.php", NULL, NULL, 0, 0);
    if (!url)
        return -1;

    jw__curl_buffer buf;
    int http_code = jw__http_get(client, url, &buf, 1, NULL, 0);
    free(url);

    if (http_code == -2) {
        free(buf.data);
        return -2;
    }
    if (http_code < 0) {
        free(buf.data);
        return -1;
    }
    if (http_code == 400 || http_code == 401 || http_code == 403) {
        jw__ss_set_error("ScreenScraper rejected the credentials");
        free(buf.data);
        return 1;
    }
    if (http_code >= 400) {
        const char *msg = jw__http_status_error(http_code);
        jw__ss_set_error("%s", msg ? msg : "ScreenScraper validation failed");
        free(buf.data);
        return -1;
    }

    cJSON *json = buf.data ? cJSON_Parse(buf.data) : NULL;
    free(buf.data);
    if (!json) {
        jw__ss_set_error("Failed to parse ScreenScraper response");
        return -1;
    }

    cJSON *response = cJSON_GetObjectItem(json, "response");
    cJSON *ssuser = response ? cJSON_GetObjectItem(response, "ssuser") : NULL;
    cJSON *id = ssuser ? cJSON_GetObjectItem(ssuser, "id") : NULL;
    if (!ssuser || !cJSON_IsString(id) || id->valuestring[0] == '\0') {
        cJSON_Delete(json);
        jw__ss_set_error("ScreenScraper rejected the credentials");
        return 1;
    }

    out->requests_today = jw__json_int(ssuser, "requeststoday", 0);
    out->max_requests = jw__json_int(ssuser, "maxrequestsperday", 0);
    out->max_threads = jw__json_int(ssuser, "maxthreads", 1);
    out->user_level = jw__json_int(ssuser, "niveau", 0);

    cJSON_Delete(json);
    return 0;
}

/* ── Media download ───────────────────────────────────────────────────── */

static void jw__ensure_parent_dir(const char *path) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0755);
            *p = '/';
        }
    }
    mkdir(dir, 0755);
}

static bool jw__url_has_jpeg_suffix(const char *media_url) {
    const char *last_dot = strrchr(media_url, '.');
    if (!last_dot)
        return false;
    return strcasecmp(last_dot, ".jpg") == 0 || strcasecmp(last_dot, ".jpeg") == 0;
}

/* Area-average downscale, RGBA8. Box filter is plenty for box art going to
   <= max_dim; avoids growing the daemon a resampling dependency. */
static unsigned char *jw__downscale_rgba(const unsigned char *src,
                                         int w, int h, int nw, int nh) {
    unsigned char *dst = malloc((size_t)nw * (size_t)nh * 4);
    if (!dst)
        return NULL;

    for (int dy = 0; dy < nh; dy++) {
        int sy0 = (int)((long long)dy * h / nh);
        int sy1 = (int)(((long long)dy + 1) * h / nh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > h) sy1 = h;
        for (int dx = 0; dx < nw; dx++) {
            int sx0 = (int)((long long)dx * w / nw);
            int sx1 = (int)(((long long)dx + 1) * w / nw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > w) sx1 = w;

            unsigned long long acc[4] = {0, 0, 0, 0};
            int count = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const unsigned char *row = src + ((size_t)sy * w + sx0) * 4;
                for (int sx = sx0; sx < sx1; sx++) {
                    acc[0] += row[0];
                    acc[1] += row[1];
                    acc[2] += row[2];
                    acc[3] += row[3];
                    row += 4;
                    count++;
                }
            }
            unsigned char *out = dst + ((size_t)dy * nw + dx) * 4;
            out[0] = (unsigned char)(acc[0] / (unsigned long long)count);
            out[1] = (unsigned char)(acc[1] / (unsigned long long)count);
            out[2] = (unsigned char)(acc[2] / (unsigned long long)count);
            out[3] = (unsigned char)(acc[3] / (unsigned long long)count);
        }
    }
    return dst;
}

/* Capture errno at the first failing step, before cleanup can overwrite it:
   the caller tells a read-only card apart from a full one or a bad path. */
static int jw__write_file_synced(const char *path, const void *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        jw__ss_errno = errno ? errno : EIO;
        return -1;
    }
    int first_errno = 0;
    size_t written = fwrite(data, 1, size, f);
    int write_ok = (written == size);
    if (!write_ok) first_errno = errno ? errno : EIO;
    int flush_ok = (fflush(f) == 0);
    if (!flush_ok && !first_errno) first_errno = errno ? errno : EIO;
    int sync_ok = flush_ok && (fsync(fileno(f)) == 0);
    if (flush_ok && !sync_ok && !first_errno) first_errno = errno ? errno : EIO;
    int close_ok = (fclose(f) == 0);
    if (!close_ok && !first_errno) first_errno = errno ? errno : EIO;
    if (write_ok && flush_ok && sync_ok && close_ok) {
        return 0;
    }
    jw__ss_errno = first_errno;
    return -1;
}

int jw_ss_download_media(const jw_ss_client *client, const char *media_url,
                         const char *dest_path, int max_dim) {
    jw__ss_clear_error();
    jw__ss_clear_errno();

    jw__curl_buffer buf;
    char content_type[128];
    jw__ss_progress(client, JW_SS_PHASE_DOWNLOADING);
    int http_code = jw__http_get(client, media_url, &buf, 2,
                                 content_type, sizeof(content_type));
    if (http_code == -2) {
        free(buf.data);
        return -2;
    }
    if (http_code != 200) {
        free(buf.data);
        if (http_code < 0 && jw_ss_last_error())
            return -1;
        jw__ss_set_error("Media download returned HTTP %d", http_code);
        return -1;
    }
    if (!buf.data || buf.size == 0) {
        free(buf.data);
        jw__ss_set_error("Media download returned an empty body");
        return -1;
    }

    jw__ss_progress(client, JW_SS_PHASE_SAVING);
    jw__ensure_parent_dir(dest_path);

    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", dest_path);

    bool is_jpeg = strstr(content_type, "jpeg") != NULL ||
                   jw__url_has_jpeg_suffix(media_url);

    int w = 0, h = 0, comp = 0;
    bool have_info = stbi_info_from_memory((const unsigned char *)buf.data,
                                           (int)buf.size, &w, &h, &comp) != 0;
    bool oversized = have_info && max_dim > 0 && (w > max_dim || h > max_dim);
    bool recode = is_jpeg || oversized;

    int rc;
    if (!recode) {
        /* PNG within bounds (or unparseable but nominally PNG): keep the
           original bytes. */
        rc = jw__write_file_synced(tmp_path, buf.data, buf.size);
        free(buf.data);
        if (rc != 0) {
            unlink(tmp_path);
            jw__ss_set_error("Failed to write media file");
            return -1;
        }
    } else {
        unsigned char *pixels = stbi_load_from_memory(
            (const unsigned char *)buf.data, (int)buf.size, &w, &h, &comp, 4);
        if (!pixels && !is_jpeg) {
            /* Claimed PNG we cannot decode: store as-is rather than fail. */
            rc = jw__write_file_synced(tmp_path, buf.data, buf.size);
            free(buf.data);
            if (rc != 0) {
                unlink(tmp_path);
                jw__ss_set_error("Failed to write media file");
                return -1;
            }
        } else if (!pixels) {
            free(buf.data);
            jw__ss_set_error("Image decode failed (stb_image)");
            return -1;
        } else {
            free(buf.data);

            unsigned char *out_pixels = pixels;
            int out_w = w, out_h = h;
            if (max_dim > 0 && (w > max_dim || h > max_dim)) {
                if (w >= h) {
                    out_w = max_dim;
                    out_h = (int)((long long)h * max_dim / w);
                } else {
                    out_h = max_dim;
                    out_w = (int)((long long)w * max_dim / h);
                }
                if (out_w < 1) out_w = 1;
                if (out_h < 1) out_h = 1;
                out_pixels = jw__downscale_rgba(pixels, w, h, out_w, out_h);
                if (!out_pixels) {
                    stbi_image_free(pixels);
                    jw__ss_set_error("Out of memory while downscaling image");
                    return -1;
                }
            }

            size_t png_size = 0;
            void *png_data = tdefl_write_image_to_png_file_in_memory(
                out_pixels, out_w, out_h, 4, &png_size);
            if (out_pixels != pixels)
                free(out_pixels);
            stbi_image_free(pixels);
            if (!png_data) {
                jw__ss_set_error("PNG encoding failed (miniz)");
                return -1;
            }

            rc = jw__write_file_synced(tmp_path, png_data, png_size);
            mz_free(png_data);
            if (rc != 0) {
                unlink(tmp_path);
                jw__ss_set_error("Failed to write media file");
                return -1;
            }
        }
    }

    if (rename(tmp_path, dest_path) != 0) {
        jw__ss_errno = errno ? errno : EIO;
        unlink(tmp_path);
        jw__ss_set_error("Failed to finalize media file");
        return -1;
    }

    return 0;
}
