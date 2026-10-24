#include "internal/launcher/core_selection.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stub availability: a fixed set of available ids, plus a log of every id
   the selector asked about, in order. */
typedef struct {
    const char *const *available;
    size_t available_count;
    char calls[256];
} stub;

static bool stub_available(void *userdata, const char *core_id) {
    stub *s = userdata;
    size_t used = strlen(s->calls);
    snprintf(s->calls + used, sizeof(s->calls) - used, "%s%s",
             used ? "," : "", core_id);
    for (size_t i = 0; i < s->available_count; i++) {
        if (strcmp(s->available[i], core_id) == 0) {
            return true;
        }
    }
    return false;
}

static int failures = 0;

static void expect_str(const char *label, const char *actual,
                       const char *expected) {
    bool same = (!actual && !expected) ||
                (actual && expected && strcmp(actual, expected) == 0);
    if (!same) {
        fprintf(stderr, "%s: got %s, expected %s\n", label,
                actual ? actual : "(null)", expected ? expected : "(null)");
        failures++;
    }
}

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* One selection with its stub. `calls` is the exact probe order expected. */
static void check(const char *label,
                  const char *saved,
                  const char *default_core,
                  const char *const *alternates, size_t alternate_count,
                  const char *const *available, size_t available_count,
                  const char *expected_core,
                  jw_core_selection_origin expected_origin,
                  const char *expected_calls) {
    stub s = { available, available_count, "" };
    jw_core_selection selection =
        jw_core_select(saved, default_core, alternates, alternate_count,
                       stub_available, &s);
    char full[256];
    snprintf(full, sizeof(full), "%s: core", label);
    expect_str(full, selection.core_id, expected_core);
    snprintf(full, sizeof(full), "%s: origin", label);
    expect_str(full, jw_core_selection_origin_name(selection.origin),
               jw_core_selection_origin_name(expected_origin));
    snprintf(full, sizeof(full), "%s: probes", label);
    expect_str(full, s.calls, expected_calls);
}

int main(void) {
    /* Saturn today: RetroArch default, installed standalone alternate. */
    {
        const char *const alternates[] = { "yabasanshiro_standalone" };
        const char *const available[] = { "yabasanshiro",
                                          "yabasanshiro_standalone" };
        check("RetroArch default wins over installed path alternate", NULL,
              "yabasanshiro", alternates, COUNT(alternates), available,
              COUNT(available), "yabasanshiro", JW_CORE_SELECTION_DEFAULT,
              "yabasanshiro");
    }
    {
        const char *const alternates[] = { "fun_drastic" };
        const char *const available[] = { "drastic", "fun_drastic" };
        check("path default, no saved choice", NULL, "drastic", alternates,
              COUNT(alternates), available, COUNT(available), "drastic",
              JW_CORE_SELECTION_DEFAULT, "drastic");
    }
    {
        const char *const alternates[] = { "path_alt", "ra_alt" };
        const char *const available[] = { "path_alt", "ra_alt" };
        check("missing default, [path, RetroArch]", NULL, "missing_ra",
              alternates, COUNT(alternates), available, COUNT(available),
              "path_alt", JW_CORE_SELECTION_ALTERNATE, "missing_ra,path_alt");
    }
    {
        const char *const alternates[] = { "ra_alt", "path_alt" };
        const char *const available[] = { "path_alt", "ra_alt" };
        check("missing default, [RetroArch, path]", NULL, "missing_ra",
              alternates, COUNT(alternates), available, COUNT(available),
              "ra_alt", JW_CORE_SELECTION_ALTERNATE, "missing_ra,ra_alt");
    }
    {
        const char *const alternates[] = { "first_missing", "second" };
        const char *const available[] = { "second" };
        check("alternates in declared order", NULL, "missing", alternates,
              COUNT(alternates), available, COUNT(available), "second",
              JW_CORE_SELECTION_ALTERNATE, "missing,first_missing,second");
    }
    {
        const char *const alternates[] = { "a", "b" };
        check("all candidates unavailable", NULL, "d", alternates,
              COUNT(alternates), NULL, 0, NULL, JW_CORE_SELECTION_NONE,
              "d,a,b");
    }
    {
        const char *const alternates[] = { "gpsp" };
        const char *const available[] = { "mgba", "gpsp" };
        check("saved choice for an available allowed core", "gpsp", "mgba",
              alternates, COUNT(alternates), available, COUNT(available),
              "gpsp", JW_CORE_SELECTION_SAVED, "gpsp");
    }
    {
        const char *const alternates[] = { "gpsp" };
        const char *const available[] = { "mgba", "gpsp", "flycast" };
        check("disallowed saved choice is never probed", "flycast", "mgba",
              alternates, COUNT(alternates), available, COUNT(available),
              "mgba", JW_CORE_SELECTION_DEFAULT, "mgba");
    }
    {
        const char *const alternates[] = { "missing_ra" };
        const char *const available[] = { "mupen64plus_standalone" };
        check("unavailable saved RetroArch choice, available path default",
              "missing_ra", "mupen64plus_standalone", alternates,
              COUNT(alternates), available, COUNT(available),
              "mupen64plus_standalone", JW_CORE_SELECTION_DEFAULT,
              "missing_ra,mupen64plus_standalone");
    }
    {
        const char *const alternates[] = { "b", "c" };
        const char *const available[] = { "c" };
        check("unavailable saved default is probed once", "d", "d",
              alternates, COUNT(alternates), available, COUNT(available), "c",
              JW_CORE_SELECTION_ALTERNATE, "d,b,c");
    }
    {
        const char *const alternates[] = { "b", "c" };
        const char *const available[] = { "c" };
        check("unavailable saved alternate is probed once", "b", "missing",
              alternates, COUNT(alternates), available, COUNT(available), "c",
              JW_CORE_SELECTION_ALTERNATE, "b,missing,c");
    }
    {
        const char *const alternates[] = { "a", "", "a", "b" };
        const char *const available[] = { "b" };
        check("empty and repeated alternates are skipped", NULL, "", alternates,
              COUNT(alternates), available, COUNT(available), "b",
              JW_CORE_SELECTION_ALTERNATE, "a,b");
    }
    {
        const char *const available[] = { "d" };
        check("no alternates", NULL, "d", NULL, 0, available,
              COUNT(available), "d", JW_CORE_SELECTION_DEFAULT, "d");
    }
    {
        /* PICO-8 pak installed: FAKE-08 default stays, no hard-coded skip. */
        const char *const alternates[] = { "pico8_native" };
        const char *const available[] = { "fake08", "pico8_native" };
        check("PICO-8 pak alternate does not replace FAKE-08", NULL, "fake08",
              alternates, COUNT(alternates), available, COUNT(available),
              "fake08", JW_CORE_SELECTION_DEFAULT, "fake08");
    }

    /* Game choice precedence. */
    expect_str("game choice masks system choice",
               jw_core_selection_saved_choice("bogus", "gpsp"), "bogus");
    expect_str("empty game choice uses system choice",
               jw_core_selection_saved_choice("", "gpsp"), "gpsp");
    expect_str("NULL game choice uses system choice",
               jw_core_selection_saved_choice(NULL, "gpsp"), "gpsp");
    expect_str("no saved choice", jw_core_selection_saved_choice("", NULL),
               NULL);
    {
        const char *const alternates[] = { "gpsp" };
        const char *const available[] = { "mgba", "gpsp" };
        check("invalid game choice masks available system choice",
              jw_core_selection_saved_choice("bogus", "gpsp"), "mgba",
              alternates, COUNT(alternates), available, COUNT(available),
              "mgba", JW_CORE_SELECTION_DEFAULT, "mgba");
    }

    {
        jw_core_selection none =
            jw_core_select("a", "a", NULL, 0, NULL, NULL);
        expect_str("NULL callback selects nothing", none.core_id, NULL);
    }

    if (failures) {
        fprintf(stderr, "%d core selection check(s) failed\n", failures);
        return 1;
    }
    puts("Core selection checks passed");
    return 0;
}
