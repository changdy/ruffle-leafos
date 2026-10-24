#ifndef JW_PLATFORM_LEAF_VERSION_H
#define JW_PLATFORM_LEAF_VERSION_H

#include <stddef.h>

#define JW_LEAF_VERSION_TEXT_MAX 64
#define JW_LEAF_RELEASE_ID_MAX 128

typedef struct {
    int schema;
    char version[JW_LEAF_VERSION_TEXT_MAX];
    char release_id[JW_LEAF_RELEASE_ID_MAX];
} jw_installed_release;

/* Parse a Leaf release version. One leading v/V is optional, exactly three
   numeric components are required, and a non-numeric suffix is ignored. */
int jw_leaf_release_version_parse(const char *value, int out[3]);

/* Parse a Pak Rat package/minimum version. Only an exact X.Y.Z triple is
   accepted; prefixes and suffixes are rejected. */
int jw_pak_version_parse(const char *value, int out[3]);

int jw_version_cmp(const int left[3], const int right[3]);

/* Read <state_dir>/release.json. Returns 0 for a parsed JSON object, 1 when
   the file is absent, and -1 for invalid input, I/O, size, or JSON errors.
   Missing/non-string version fields remain empty so callers can apply their
   own unknown-version policy. */
int jw_installed_release_read(const char *state_dir,
                              jw_installed_release *out);

#endif
