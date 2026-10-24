#include "internal/scrape/scrape_catalog.h"

/* Catalog order is the order the settings editors list entries; defaults
   follow ScrapeGoat's. */

const jw_ss_option jw_ss_media_types[] = {
    {"Screenshot (title)",   "sstitle"},
    {"Screenshot (in-game)", "ss"},
    {"Fan art",              "fanart"},
    {"Wheel",                "wheel"},
    {"Wheel (carbon)",       "wheel-carbon"},
    {"Wheel (steel)",        "wheel-steel"},
    {"Box art (2D)",         "box-2D"},
    {"Box art (3D)",         "box-3D"},
    {"Mix Recalbox V1",      "mixrbv1"},
    {"Mix Recalbox V2",      "mixrbv2"},
};
const int jw_ss_media_types_count =
    (int)(sizeof(jw_ss_media_types) / sizeof(jw_ss_media_types[0]));

const jw_ss_option jw_ss_regions[] = {
    {"World",         "wor"},
    {"USA",           "us"},
    {"Europe",        "eu"},
    {"Japan",         "jp"},
    {"France",        "fr"},
    {"Germany",       "de"},
    {"Spain",         "es"},
    {"Italy",         "it"},
    {"Portugal",      "pt"},
    {"ScreenScraper", "ss"},
};
const int jw_ss_regions_count =
    (int)(sizeof(jw_ss_regions) / sizeof(jw_ss_regions[0]));

const char *const jw_ss_default_artwork_priority[] = {
    "box-2D", "box-3D", "mixrbv1", "ss",
};
const int jw_ss_default_artwork_priority_count =
    (int)(sizeof(jw_ss_default_artwork_priority) /
          sizeof(jw_ss_default_artwork_priority[0]));

const char *const jw_ss_default_region_priority[] = {
    "us", "eu", "jp", "wor",
};
const int jw_ss_default_region_priority_count =
    (int)(sizeof(jw_ss_default_region_priority) /
          sizeof(jw_ss_default_region_priority[0]));
