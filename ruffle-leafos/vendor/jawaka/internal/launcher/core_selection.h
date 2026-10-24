#ifndef JW_CORE_SELECTION_H
#define JW_CORE_SELECTION_H

#include <stdbool.h>
#include <stddef.h>

/* Whether one exact catalog core can launch this content right now. The
   callback checks only the candidate it is given; it must never substitute a
   default, an alternate or any other core. */
typedef bool (*jw_core_available_fn)(void *userdata, const char *core_id);

typedef enum {
    JW_CORE_SELECTION_NONE = 0,
    JW_CORE_SELECTION_SAVED,
    JW_CORE_SELECTION_DEFAULT,
    JW_CORE_SELECTION_ALTERNATE,
} jw_core_selection_origin;

typedef struct {
    /* Points into the caller's inputs; NULL when no candidate is available. */
    const char *core_id;
    jw_core_selection_origin origin;
} jw_core_selection;

/* The saved choice that applies: a nonempty per-game choice, otherwise the
   per-system choice. A nonempty game choice masks the system choice even when
   it later proves unavailable or disallowed. Returns NULL when neither is set. */
const char *jw_core_selection_saved_choice(const char *game_choice,
                                           const char *system_choice);

/* Pick the core for a launch:
   1. the saved choice, when the system allows it and it is available;
   2. otherwise the default core, when available;
   3. only if the default is unavailable, the first available alternate in
      declared order, whatever its type.
   An unavailable or disallowed saved choice falls through without being
   erased; that is the caller's storage and is never touched here. The callback
   is not called again once a candidate is selected, so the last successful
   call always describes the selection. */
jw_core_selection jw_core_select(const char *saved_choice,
                                 const char *default_core,
                                 const char *const *alternates,
                                 size_t alternate_count,
                                 jw_core_available_fn available,
                                 void *userdata);

const char *jw_core_selection_origin_name(jw_core_selection_origin origin);

#endif
