#include "internal/scrape/scrape_systems.h"
#include "internal/retroarch/catalog.h"

#include <stdio.h>
#include <stdlib.h>

static void expect_int(const char *label, int actual, int expected) {
    if (actual == expected)
        return;
    fprintf(stderr, "%s: got %d, expected %d\n", label, actual, expected);
    exit(1);
}

int main(void) {
    int ids[JW_SCRAPE_MAX_PLATFORM_IDS] = {0};

    expect_int("PC-98 count", (int)jw_scrape_platform_ids("pc98", ids, 4), 1);
    expect_int("PC-98 id", ids[0], 208);
    expect_int("Atomiswave id", jw_scrape_platform_id("atomiswave"), 53);

    expect_int("Amiga count", (int)jw_scrape_platform_ids("AMIGA", ids, 4), 4);
    expect_int("Amiga", ids[0], 64);
    expect_int("Amiga AGA", ids[1], 111);
    expect_int("Amiga CDTV", ids[2], 129);
    expect_int("Amiga CD32", ids[3], 130);
    expect_int("PUAE compatibility alias", jw_scrape_platform_id("PUAE"), 64);

    expect_int("Naomi count", (int)jw_scrape_platform_ids("NAOMI", ids, 4), 3);
    expect_int("Naomi", ids[0], 56);
    expect_int("Naomi GD-ROM", ids[1], 227);
    expect_int("Naomi 2", ids[2], 230);

    ids[0] = ids[1] = 0;
    expect_int("Naomi truncated count",
               (int)jw_scrape_platform_ids("naomi", ids, 2), 3);
    expect_int("Naomi truncated first", ids[0], 56);
    expect_int("Naomi truncated second", ids[1], 227);
    expect_int("unknown", (int)jw_scrape_platform_ids("PORTS", NULL, 0), 0);

    jw_ra_system systems[1] = {0};
    systems[0].id = "SCUMMVM";
    systems[0].screenscraper_platform_ids[0] = 999;
    systems[0].screenscraper_platform_ids[1] = 123;
    systems[0].screenscraper_platform_id_count = 2;
    jw_ra_catalog catalog = {.systems = systems, .system_count = 1};
    expect_int("catalog count",
               (int)jw_scrape_platform_ids_for_catalog(&catalog, "SCUMMVM", ids, 8), 2);
    expect_int("catalog wins", ids[0], 999);
    expect_int("catalog order", ids[1], 123);
    expect_int("catalog fallback",
               jw_scrape_platform_ids_for_catalog(&catalog, "GBA", ids, 8), 1);
    expect_int("fallback id", ids[0], 12);

    puts("ScreenScraper platform mapping checks passed");
    return 0;
}
