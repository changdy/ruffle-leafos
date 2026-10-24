#include "internal/launcher/core_selection.h"

#include <string.h>

static bool jw__nonempty(const char *value) {
    return value && value[0];
}

static bool jw__same(const char *a, const char *b) {
    return jw__nonempty(a) && jw__nonempty(b) && strcmp(a, b) == 0;
}

static bool jw__system_allows(const char *core_id,
                              const char *default_core,
                              const char *const *alternates,
                              size_t alternate_count) {
    if (jw__same(core_id, default_core)) {
        return true;
    }
    for (size_t i = 0; alternates && i < alternate_count; i++) {
        if (jw__same(core_id, alternates[i])) {
            return true;
        }
    }
    return false;
}

const char *jw_core_selection_saved_choice(const char *game_choice,
                                           const char *system_choice) {
    if (jw__nonempty(game_choice)) {
        return game_choice;
    }
    return jw__nonempty(system_choice) ? system_choice : NULL;
}

jw_core_selection jw_core_select(const char *saved_choice,
                                 const char *default_core,
                                 const char *const *alternates,
                                 size_t alternate_count,
                                 jw_core_available_fn available,
                                 void *userdata) {
    jw_core_selection selection = { NULL, JW_CORE_SELECTION_NONE };
    if (!available) {
        return selection;
    }

    /* Cores already found unavailable are not asked about twice. */
    const char *probed_saved = NULL;
    if (jw__nonempty(saved_choice) &&
        jw__system_allows(saved_choice, default_core, alternates,
                          alternate_count)) {
        if (available(userdata, saved_choice)) {
            selection.core_id = saved_choice;
            selection.origin = JW_CORE_SELECTION_SAVED;
            return selection;
        }
        probed_saved = saved_choice;
    }

    if (jw__nonempty(default_core) && !jw__same(default_core, probed_saved)) {
        if (available(userdata, default_core)) {
            selection.core_id = default_core;
            selection.origin = JW_CORE_SELECTION_DEFAULT;
            return selection;
        }
    }

    for (size_t i = 0; alternates && i < alternate_count; i++) {
        const char *alternate = alternates[i];
        if (!jw__nonempty(alternate) || jw__same(alternate, default_core) ||
            jw__same(alternate, probed_saved)) {
            continue;
        }
        bool seen = false;
        for (size_t j = 0; j < i && !seen; j++) {
            seen = jw__same(alternate, alternates[j]);
        }
        if (!seen && available(userdata, alternate)) {
            selection.core_id = alternate;
            selection.origin = JW_CORE_SELECTION_ALTERNATE;
            return selection;
        }
    }
    return selection;
}

const char *jw_core_selection_origin_name(jw_core_selection_origin origin) {
    switch (origin) {
    case JW_CORE_SELECTION_SAVED:
        return "saved";
    case JW_CORE_SELECTION_DEFAULT:
        return "default";
    case JW_CORE_SELECTION_ALTERNATE:
        return "alternate";
    case JW_CORE_SELECTION_NONE:
        break;
    }
    return "none";
}
