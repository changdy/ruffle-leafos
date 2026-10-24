#ifndef JW_AUTODEMO_H
#define JW_AUTODEMO_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL2/SDL.h>
#include "internal/core/env.h"

typedef struct {
    bool     enabled;
    bool     fired;
    uint32_t delay_ms;
    uint32_t started_ms;
} jw_autodemo;

static inline void jw_autodemo_init(jw_autodemo *d) {
    d->enabled    = jw_env_flag("JAWAKA_AUTODEMO");
    d->fired      = false;
    d->delay_ms   = jw_env_u32("JAWAKA_AUTODEMO_DELAY_MS", 1200);
    d->started_ms = SDL_GetTicks();
}

/* Returns true exactly once, when the delay has elapsed and the demo action
 * should be fired. Call cat_request_frame_in() when false but still pending. */
static inline bool jw_autodemo_should_fire(jw_autodemo *d) {
    if (!d->enabled || d->fired) return false;
    if (SDL_GetTicks() - d->started_ms >= d->delay_ms) {
        d->fired = true;
        return true;
    }
    return false;
}

/* Returns remaining ms until the demo fires, or 0 if already elapsed/fired. */
static inline uint32_t jw_autodemo_remaining_ms(const jw_autodemo *d) {
    if (!d->enabled || d->fired) return 0;
    uint32_t elapsed = SDL_GetTicks() - d->started_ms;
    return elapsed < d->delay_ms ? d->delay_ms - elapsed : 0;
}

#endif /* JW_AUTODEMO_H */
