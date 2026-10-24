#include "internal/scrape/scrape_systems.h"
#include "internal/retroarch/catalog.h"

#include <string.h>
#include <strings.h>

/* ScreenScraper platform ids, from nextui-scrapegoat-pak systems.c with
   Jawaka folder-tag aliases added (NES/SNES/MS/ARCADE/NEOGEO/TG16...).
   Aliases share ids when they are the same platform under another core or
   folder spelling. */

typedef struct { const char *tag; int id; } jw__platform_entry;

static const jw__platform_entry jw__platforms[] = {
    {"32X",        19},
    {"3DO",        29},
    {"A2600",      26},
    {"A5200",      40},
    {"A7800",      41},
    {"A800",       43},
    {"AMIGA",      64},
    {"AMIGA",      111},
    {"AMIGA",      129},
    {"AMIGA",      130},
    {"ARCADE",     75},
    {"ATOMISWAVE", 53},
    {"C128",       87},
    {"C64",        66},
    {"COLECO",     60},
    {"CPC",        65},
    {"DC",         23},
    {"EASYRPG",    231},
    {"FBN",        75},
    {"FC",         3},
    {"FDS",        106},
    {"GB",         9},
    {"GBA",        12},
    {"GBC",        10},
    {"GG",         21},
    {"INTV",       115},
    {"JAGUAR",     27},
    {"LYNX",       28},
    {"MAME",       75},
    {"MD",         1},
    {"MGBA",       12},
    {"MS",         2},
    {"MSX",        62},
    {"N64",        14},
    {"NAOMI",      56},
    {"NAOMI",      227},
    {"NAOMI",      230},
    {"NDS",        15},
    {"NEOGEO",     142},
    {"NES",        3},
    {"NGP",        25},
    {"NGPC",       82},
    {"P8",         234},
    {"PCE",        31},
    {"PC98",       208},
    {"PICO",       234},
    {"PKM",        211},
    {"PS",         57},
    {"PSP",        61},
    {"PUAE",       64},
    {"SCUMMVM",    123},
    {"SEGACD",     20},
    {"SFC",        4},
    {"SG1000",     109},
    {"SGB",        9},
    {"SMS",        2},
    {"SNES",       4},
    {"SS",         22},
    {"SUPA",       4},
    {"SUPERGRAFX", 105},
    {"TG16",       31},
    {"TIC",        222},
    {"VB",         11},
    {"VIC",        73},

    /* Jawaka folder-ids (systems.json canonical ids) that differ from the tags
       above. Each aliases the SAME ScreenScraper platform as an entry above, so
       "Scrape Artwork" works for these folders too. Without these, the daemon's
       jw_scrape_platform_id() returns -1 and the scrape is silently refused. */
    {"GENESIS",            1},    /* == MD   */
    {"GEN",                1},
    {"MD32X",              19},   /* == 32X  */
    {"ATARI2600",          26},   /* == A2600 */
    {"SEVENTYEIGHTHUNDRED", 41},  /* == A7800 */
    {"SATURN",             22},   /* == SS   */
    {"PSX",                57},   /* == PS   */
    {"SFC_JP",             4},    /* == SFC  */
    {"PICO8",              234},  /* == PICO / P8 */
    {"MAME2003",           75},   /* == MAME / ARCADE */
    {"MAME2010",           75},

    /* Systems with no prior table entry; ids verified against ScreenScraper's
       systemesListe (PC Engine CD-Rom 114, Bandai WonderSwan 45 / Color 46,
       GCE Vectrex 102, Game & Watch 52, MS-Dos 135). */
    {"PCECD",              114},
    {"WS",                 45},
    {"WSC",                46},
    {"VECTREX",            102},
    {"GW",                 52},
    {"DOS",                135},
};

size_t jw_scrape_platform_ids(const char *system_tag,
                              int *out, size_t capacity) {
    if (!system_tag || !system_tag[0])
        return 0;
    size_t count = 0;
    for (size_t i = 0; i < sizeof(jw__platforms) / sizeof(jw__platforms[0]); i++) {
        if (strcasecmp(jw__platforms[i].tag, system_tag) != 0)
            continue;
        if (out && count < capacity)
            out[count] = jw__platforms[i].id;
        count++;
    }
    return count;
}

size_t jw_scrape_platform_ids_for_catalog(const jw_ra_catalog *catalog,
                                          const char *system_tag,
                                          int *out, size_t capacity) {
    const jw_ra_system *system =
        jw_ra_catalog_match_system_folder(catalog, system_tag);
    if (system && system->screenscraper_platform_id_count > 0) {
        size_t count = system->screenscraper_platform_id_count;
        size_t copied = count < capacity ? count : capacity;
        if (out && copied > 0) {
            memcpy(out, system->screenscraper_platform_ids,
                   copied * sizeof(system->screenscraper_platform_ids[0]));
        }
        return count;
    }
    return jw_scrape_platform_ids(system_tag, out, capacity);
}

int jw_scrape_platform_id(const char *system_tag) {
    int first = -1;
    return jw_scrape_platform_ids(system_tag, &first, 1) > 0 ? first : -1;
}
