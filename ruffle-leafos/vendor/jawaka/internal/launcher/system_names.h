#ifndef JW_LAUNCHER_SYSTEM_NAMES_H
#define JW_LAUNCHER_SYSTEM_NAMES_H

#include <stddef.h>
#include "internal/retroarch/catalog.h"

#define JW_CONTENT_SETTING_DISPLAY_NAME "display_name"

/* Manual rename > content-pak name > curated release label > catalog name > id.
   A NULL catalog retains manual renames and built-in labels. */
void jw_system_display_name(const char *db_path,
                            const jw_ra_catalog *catalog,
                            const char *system_id,
                            char *out,
                            size_t out_size);

#endif /* JW_LAUNCHER_SYSTEM_NAMES_H */
