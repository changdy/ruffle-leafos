#include "internal/services/dup_ids.h"

#include <string.h>

bool jw_svc_find_duplicate_ids(const char *const *service_ids, size_t count, bool *is_duplicate) {
    if (count == 0) {
        return false;
    }
    if (!service_ids || !is_duplicate) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        is_duplicate[i] = false;
    }

    bool any_duplicate = false;
    for (size_t i = 0; i < count; i++) {
        if (!service_ids[i]) {
            continue;
        }
        for (size_t j = i + 1; j < count; j++) {
            if (!service_ids[j]) {
                continue;
            }
            if (strcmp(service_ids[i], service_ids[j]) == 0) {
                is_duplicate[i] = true;
                is_duplicate[j] = true;
                any_duplicate = true;
            }
        }
    }

    return any_duplicate;
}
