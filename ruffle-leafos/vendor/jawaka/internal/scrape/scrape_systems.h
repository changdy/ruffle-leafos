#ifndef JW_SCRAPE_SYSTEMS_H
#define JW_SCRAPE_SYSTEMS_H

#include <stddef.h>
#include "internal/retroarch/catalog.h"

#define JW_SCRAPE_MAX_PLATFORM_IDS 8

/* Return the number of ordered ScreenScraper platform ids for a Jawaka
   system folder tag. Copies up to capacity ids when out is non-NULL. */
size_t jw_scrape_platform_ids(const char *system_tag,
                              int *out, size_t capacity);

/* Prefer ids declared by the effective catalog system. The compatibility
   table remains the fallback for release rows that do not declare them. */
size_t jw_scrape_platform_ids_for_catalog(const jw_ra_catalog *catalog,
                                          const char *system_tag,
                                          int *out, size_t capacity);

/* Map a Jawaka system folder tag (Roms/<TAG>/) to a screenscraper.fr
   platform id. Returns the first ordered id, or -1 for an unmapped system. */
int jw_scrape_platform_id(const char *system_tag);

#endif /* JW_SCRAPE_SYSTEMS_H */
