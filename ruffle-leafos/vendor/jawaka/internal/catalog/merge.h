#ifndef JW_CATALOG_MERGE_H
#define JW_CATALOG_MERGE_H

typedef struct cJSON cJSON;

/* Pure CONTENT-1 merge. Inputs are not modified. `contributors` is an array
   of {provider, provides}; outputs are newly allocated cJSON values. */
int jw_catalog_merge(const cJSON *base,
                     const cJSON *contributors,
                     cJSON **out_catalog,
                     cJSON **out_diagnostics);

/* Apply validated CONTENT-ART-1/2 blocks after the ordinary merge. Each image
   slot (wordmark, grid_icon, wordmark_color) is claimed independently. */
int jw_catalog_apply_content_art(cJSON *systems, const cJSON *cores,
                                  const cJSON *contributors, cJSON *diagnostics);

#endif
