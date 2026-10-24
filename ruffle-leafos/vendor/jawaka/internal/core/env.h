#ifndef JW_ENV_H
#define JW_ENV_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static inline int jw_env_flag(const char *name) {
    const char *v = getenv(name);
    return v && strcmp(v, "0") != 0;
}

static inline uint32_t jw_env_u32(const char *name, uint32_t fallback) {
    const char *v = getenv(name);
    if (!v || !v[0]) return fallback;
    long parsed = strtol(v, NULL, 10);
    return parsed > 0 ? (uint32_t)parsed : fallback;
}

#endif /* JW_ENV_H */
