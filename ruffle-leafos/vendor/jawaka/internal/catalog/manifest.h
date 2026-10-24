#ifndef JW_CONTENT_MANIFEST_H
#define JW_CONTENT_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>

typedef struct cJSON cJSON;

#define JW_CONTENT_REASON_MAX 64

typedef enum {
    JW_CONTENT_PLATFORM_LANE = 0,
    JW_CONTENT_SHARED_LANE,
} jw_content_install_lane;

/* A validated CONTENT-1 manifest. The parsed document is retained so the
   compiler can consume the exact provides object that was validated. */
typedef struct {
    cJSON *document;
    cJSON *provides;
    bool redundant_case_variant;
} jw_content_manifest;

/* Validate one already-read pak.json plus its install context. On success,
   `out` owns the parsed JSON until jw_content_manifest_destroy(). */
bool jw_content_manifest_validate(const char *pak_json_text,
                                  const char *pak_abs_path,
                                  jw_content_install_lane lane,
                                  const char *source_id,
                                  jw_content_manifest *out,
                                  char *reason,
                                  size_t reason_size);

/* Validate optional CONTENT-SCRAPE-1 metadata on an already parsed pak.json.
   Returns 1 and points `out` at the block when valid, 0 when absent, and -1
   with a reason when invalid. Invalid companion metadata never invalidates
   the separately validated CONTENT-1 contribution. */
int jw_content_scrape_validate(const cJSON *document,
                               const cJSON **out,
                               char *reason,
                               size_t reason_size);

/* CONTENT-ART-1/2 have the same fail-soft return convention; paths use pak_abs_path. */
int jw_content_art_validate(const cJSON *document, const char *pak_abs_path,
                            const cJSON **out, char *reason, size_t reason_size);

void jw_content_manifest_destroy(jw_content_manifest *manifest);

#endif
