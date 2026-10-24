#ifndef JW_SCRAPE_CATALOG_H
#define JW_SCRAPE_CATALOG_H

/* ScreenScraper media-type and region catalogs + built-in priority defaults.
   Data only (no curl/network) so the settings UI can link it for the
   priority editors without pulling in the API client. */

typedef struct {
    const char *display;
    const char *value;
} jw_ss_option;

extern const jw_ss_option jw_ss_media_types[];
extern const int          jw_ss_media_types_count;
extern const jw_ss_option jw_ss_regions[];
extern const int          jw_ss_regions_count;

extern const char *const jw_ss_default_artwork_priority[];
extern const int         jw_ss_default_artwork_priority_count;
extern const char *const jw_ss_default_region_priority[];
extern const int         jw_ss_default_region_priority_count;

#endif /* JW_SCRAPE_CATALOG_H */
