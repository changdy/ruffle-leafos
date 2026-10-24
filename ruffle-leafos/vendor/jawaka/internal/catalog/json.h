#ifndef JW_CATALOG_JSON_H
#define JW_CATALOG_JSON_H

#include <stddef.h>

typedef struct cJSON cJSON;

/* CONTENT-1/CAT-1 canonical JSON: object keys in UTF-8 byte order, compact
   separators, UTF-8 strings, and exactly one trailing newline. */
char *jw_catalog_json_canonical(const cJSON *value, size_t *out_size);

#endif
