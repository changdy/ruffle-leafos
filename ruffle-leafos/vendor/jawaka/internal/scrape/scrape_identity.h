#ifndef JW_SCRAPE_IDENTITY_H
#define JW_SCRAPE_IDENTITY_H

#include <stddef.h>

#define JW_SCRAPE_IDENTITY_CANDIDATE_MAX 3
#define JW_SCRAPE_IDENTITY_NAME_MAX 256

typedef struct {
    char names[JW_SCRAPE_IDENTITY_CANDIDATE_MAX]
              [JW_SCRAPE_IDENTITY_NAME_MAX];
    size_t count;
} jw_scrape_identity_candidates;

/* Build the descriptor-mode lookup ladder. An invalid descriptor simply
   omits the strongest candidate; the actual filename and effective title
   remain available as bounded fallbacks. */
void jw_scrape_identity_build(const char *descriptor_path,
                              const char *rom_name,
                              const char *effective_title,
                              const char *lookup_extension,
                              jw_scrape_identity_candidates *out);

#endif
