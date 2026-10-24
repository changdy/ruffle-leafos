#ifndef JW_SCRAPE_SS_CLIENT_H
#define JW_SCRAPE_SS_CLIENT_H

#include "internal/scrape/scrape_catalog.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

/* ScreenScraper.fr API client, ported from Helaas's nextui-scrapegoat-pak
   (MIT) with the SDL conversion path removed: this client is daemon-side
   only and re-encodes through stb_image + miniz. */

#define JW_SS_API_BASE   "https://api.screenscraper.fr/api2"
#define JW_SS_SOFTNAME   "jawaka"
#define JW_SS_USER_AGENT "jawaka/0.0.1"

/* Build-time developer credentials, injected by the Makefile from the
   git-ignored .env.local. Empty means scraping is unavailable in this
   build; everything still compiles and runs. */
#ifndef SCREENSCRAPER_DEV_ID
#define SCREENSCRAPER_DEV_ID ""
#endif
#ifndef SCREENSCRAPER_DEV_PASSWORD
#define SCREENSCRAPER_DEV_PASSWORD ""
#endif
#ifndef SCREENSCRAPER_DEBUG_PASSWORD
#define SCREENSCRAPER_DEBUG_PASSWORD ""
#endif

bool jw_ss_available(void);   /* dev credentials compiled in */
bool jw_ss_is_debug(void);

typedef enum {
    JW_SS_PHASE_HASHING = 0,
    JW_SS_PHASE_SEARCHING,
    JW_SS_PHASE_DOWNLOADING,
    JW_SS_PHASE_SAVING,
} jw_ss_phase;

typedef enum {
    JW_SS_SEARCH_CANCELLED = -2,
    JW_SS_SEARCH_ERROR = -1,
    JW_SS_SEARCH_FOUND = 0,
    JW_SS_SEARCH_NOT_FOUND = 1,
    JW_SS_SEARCH_NO_MEDIA = 2,
} jw_ss_search_status;

typedef void (*jw_ss_progress_fn)(void *userdata, jw_ss_phase phase);

typedef struct {
    char username[256];       /* ScreenScraper user account ("" = anonymous) */
    char password[256];
    atomic_int *interrupt;    /* optional: set non-zero to cancel mid-request */
    jw_ss_progress_fn progress;
    void *progress_userdata;
} jw_ss_client;

typedef struct {
    char game_name[512];
    char media_url[1024];
    char media_format[16];    /* "png" or "jpg" */
    /* Facts from the same jeuInfos response the artwork comes from, so filling
       these costs no extra request and no extra quota. Any field the response
       omits stays empty; the caller stores only what it got. */
    char genre[96];
    char developer[96];
    char publisher[96];
    char players[16];         /* "1", "1-2", "4" ... as ScreenScraper words it */
    char rating[8];           /* ScreenScraper's note, 0-20 */
    char year[8];             /* first release year */
    char synopsis[1200];
    int  requests_today;
    int  max_requests;
    int  max_threads;
} jw_ss_result;

typedef struct {
    int requests_today;
    int max_requests;
    int max_threads;
    int user_level;
} jw_ss_user;

/* Look up a ROM (jeuInfos.php). rom_name is the on-disk filename including
   extension; rom_abs_path enables md5+size disambiguation (NULL or
   unhashable files fall back to name+system only, as does a no-match
   retry when an md5 was sent). artwork_types/region_prio are
   priority-ordered ScreenScraper value strings.
   Returns a named JW_SS_SEARCH_* outcome. */
jw_ss_search_status jw_ss_search_rom(
    const jw_ss_client *client,
    const char *rom_name, const char *rom_abs_path,
    int system_id,
    const char *const *artwork_types, int artwork_count,
    const char *const *region_prio, int region_count,
    jw_ss_result *result);

/* Search the same ROM across an ordered list of ScreenScraper platforms.
   The ROM is hashed once; each platform gets hash then filename lookup.
   Same-identity variants continue after not-found or no-media. If none finds
   media, no-media wins over not-found in the aggregate result. */
jw_ss_search_status jw_ss_search_rom_platforms(
    const jw_ss_client *client,
    const char *rom_name,
    const char *rom_abs_path,
    const int *system_ids, size_t system_count,
    const char *const *artwork_types,
    int artwork_count,
    const char *const *region_prio,
    int region_count,
    jw_ss_result *result);

#ifdef JW_SS_TESTING
typedef jw_ss_search_status (*jw_ss_test_search_request_fn)(
    const char *rom_name, const char *md5_hash, long file_size,
    int system_id, jw_ss_result *result, void *userdata);
void jw_ss_test_set_search_request(jw_ss_test_search_request_fn request,
                                   void *userdata);
#endif

/* Download media to dest_path, always stored as PNG (JPEG sources are
   re-encoded). When max_dim > 0 images larger than max_dim on their longest
   side are downscaled to fit. Writes tmp + fsync + rename.
   Returns 0 on success, -1 error, -2 cancelled. */
int jw_ss_download_media(const jw_ss_client *client, const char *media_url,
                         const char *dest_path, int max_dim);

/* Validate user credentials with ssuserInfos.php.
   Returns 0 valid (out filled), 1 rejected credentials, -1 error,
   -2 cancelled. */
int jw_ss_validate_user(const jw_ss_client *client, jw_ss_user *out);

/* Last error message for the calling thread, or NULL. Never contains
   passwords or the dev secret. */
const char *jw_ss_last_error(void);
/* errno captured at the failing open/write/flush/fsync/close/rename of the
   last jw_ss_download_media on this thread, or 0. */
int jw_ss_last_errno(void);

#endif /* JW_SCRAPE_SS_CLIENT_H */
