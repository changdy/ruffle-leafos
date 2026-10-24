#include "cmd/jawakad/osd_client.h"
#include "internal/launcher/pico8.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    long long now;
    int results[16];     /* per request, in order; 0 once exhausted */
    int result_count;
    int calls;
    char last[160];
    int last_timeout;
    bool last_show;
    int shows_of_prompt;
    int hides;
    int discards;
} fake_osd;

static int fake_request(void *ctx, const char *json, int timeout_ms, bool show) {
    fake_osd *osd = ctx;
    snprintf(osd->last, sizeof(osd->last), "%s", json);
    osd->last_timeout = timeout_ms;
    osd->last_show = show;
    if (strstr(json, "\"pico8-exit\"")) osd->shows_of_prompt++;
    if (strstr(json, "\"hide-game-launch\"")) osd->hides++;
    int index = osd->calls++;
    return index < osd->result_count ? osd->results[index] : 0;
}

static long long fake_now(void *ctx) { return ((fake_osd *)ctx)->now; }
static void fake_discard(void *ctx) { ((fake_osd *)ctx)->discards++; }

static jw_osd_client client_for(fake_osd *osd) {
    return (jw_osd_client){ .ctx = osd, .request = fake_request, .now_ms = fake_now,
                            .discard_osd = fake_discard };
}

static void arm(const jw_osd_client *client, fake_osd *osd, long long *deadline) {
    assert(jw_osd_client_pico8_menu(client, deadline) == JW_OSD_PICO8_MENU_ARMED);
    assert(*deadline == osd->now + JW_PICO8_EXIT_CONFIRM_MS);
    /* The OSD must not submit the prompt after the daemon stops waiting. */
    char want[128];
    snprintf(want, sizeof(want),
             "{\"type\":\"show-game-launch\",\"stage\":\"pico8-exit\",\"expires_ms\":%lld}",
             osd->now + JW_OSD_CLIENT_BANNER_TIMEOUT_MS);
    assert(strcmp(osd->last, want) == 0);
    assert(osd->last_timeout == JW_OSD_CLIENT_BANNER_TIMEOUT_MS && osd->last_show);
}

static void confirm_inside_window(void) {
    fake_osd osd = { .now = 1000 };
    jw_osd_client client = client_for(&osd);
    long long deadline = 0;
    arm(&client, &osd, &deadline);
    osd.now = 1000 + JW_PICO8_EXIT_CONFIRM_MS - 1;
    assert(jw_osd_client_pico8_menu(&client, &deadline) == JW_OSD_PICO8_MENU_CONFIRMED);
    assert(deadline == 0 && osd.hides == 1 && !osd.last_show);
}

/* At the boundary the prompt is gone, so the press shows it again. */
static void expiry_boundary_rearms(void) {
    fake_osd osd = { .now = 1000 };
    jw_osd_client client = client_for(&osd);
    long long deadline = 0;
    arm(&client, &osd, &deadline);
    osd.now = 1000 + JW_PICO8_EXIT_CONFIRM_MS;
    arm(&client, &osd, &deadline);
    assert(osd.shows_of_prompt == 2 && osd.hides == 0);
}

/* The backend could not submit the prompt: no hidden confirmation. */
static void show_failure_disarms(void) {
    fake_osd osd = { .now = 50, .results = { -1 }, .result_count = 1 };
    jw_osd_client client = client_for(&osd);
    long long deadline = 0;
    assert(jw_osd_client_pico8_menu(&client, &deadline) == JW_OSD_PICO8_MENU_UNAVAILABLE);
    assert(deadline == 0);
    /* An error reply still gets a hide, which succeeds: nothing to discard. */
    assert(osd.hides == 1 && osd.discards == 0);
    osd.now = 60;
    arm(&client, &osd, &deadline);   /* the next press shows, never confirms */
}

/* The reply was late: the prompt may have been submitted anyway. */
static void late_prompt_is_removed(void) {
    fake_osd osd = { .now = 10, .results = { -1, 0 }, .result_count = 2 };
    jw_osd_client client = client_for(&osd);
    long long deadline = 0;
    assert(jw_osd_client_pico8_menu(&client, &deadline) == JW_OSD_PICO8_MENU_UNAVAILABLE);
    assert(deadline == 0 && osd.hides == 1 && osd.discards == 0);
    assert(!osd.last_show && osd.last_timeout == JW_OSD_CLIENT_BANNER_TIMEOUT_MS);

    /* The hide cannot be confirmed either: end the OSD so the prompt goes. */
    osd = (fake_osd){ .now = 10, .results = { -1, -1 }, .result_count = 2 };
    assert(jw_osd_client_pico8_menu(&client, &deadline) == JW_OSD_PICO8_MENU_UNAVAILABLE);
    assert(deadline == 0 && osd.hides == 1 && osd.discards == 1);

    /* Without a discard hook it still stays disarmed. */
    client.discard_osd = NULL;
    osd = (fake_osd){ .now = 10, .results = { -1, -1 }, .result_count = 2 };
    assert(jw_osd_client_pico8_menu(&client, &deadline) == JW_OSD_PICO8_MENU_UNAVAILABLE);
    assert(deadline == 0 && osd.discards == 0);
    osd.now = 20;
    assert(jw_osd_client_pico8_menu(&client, &deadline) == JW_OSD_PICO8_MENU_ARMED);
}

typedef enum { REPLACE_VOLUME, REPLACE_BRIGHTNESS, REPLACE_STAGE, REPLACE_WARNING,
               REPLACE_HIDE, REPLACE_OSD_EXIT } replacement;

static int replace(const jw_osd_client *client, long long *deadline, replacement how) {
    switch (how) {
        case REPLACE_VOLUME:
            return jw_osd_client_show_level(client, deadline, "show-volume", 30, 100);
        case REPLACE_BRIGHTNESS:
            return jw_osd_client_show_level(client, deadline, "show-brightness", 30, 100);
        case REPLACE_STAGE:
            return jw_osd_client_show_stage(client, deadline, "syncing", 2);
        case REPLACE_WARNING:
            return jw_osd_client_show_stage(client, deadline, "storage-read-only", 0);
        case REPLACE_HIDE:
            return jw_osd_client_hide(client, deadline);
        case REPLACE_OSD_EXIT:
            jw_osd_client_prompt_lost(deadline);
            return 0;
    }
    return -1;
}

/* Every replacement disarms, whether or not its own request succeeds: a
   timeout leaves the display outcome uncertain, and uncertain means disarmed. */
static void replacement_disarms(replacement how, bool replacement_fails) {
    fake_osd osd = { .now = 1000 };
    jw_osd_client client = client_for(&osd);
    long long deadline = 0;
    arm(&client, &osd, &deadline);
    if (replacement_fails) {
        for (int i = 0; i < 16; i++) osd.results[i] = -1;
        osd.result_count = 16;
    }
    int rc = replace(&client, &deadline, how);
    assert(replacement_fails && how != REPLACE_OSD_EXIT ? rc != 0 : rc == 0);
    assert(deadline == 0);
    osd.result_count = 0;
    osd.now = 1500;   /* well inside the old window */
    arm(&client, &osd, &deadline);
}

static void request_shapes(void) {
    fake_osd osd = { .now = 0 };
    jw_osd_client client = client_for(&osd);
    long long deadline = 0;
    assert(jw_osd_client_show_stage(&client, &deadline, "syncing", -4) == 0);
    assert(strcmp(osd.last, "{\"type\":\"show-game-launch\",\"stage\":\"syncing\",\"pending_items\":0}") == 0);
    assert(osd.last_timeout == JW_OSD_CLIENT_BANNER_TIMEOUT_MS && osd.last_show);
    assert(jw_osd_client_show_stage(&client, &deadline, "checking", 9) == 0);
    assert(strcmp(osd.last, "{\"type\":\"show-game-launch\",\"stage\":\"checking\"}") == 0);
    int calls = osd.calls;
    /* The prompt has its own path; nothing may show it without arming. */
    assert(jw_osd_client_show_stage(&client, &deadline, "pico8-exit", 0) != 0);
    assert(jw_osd_client_show_stage(&client, &deadline, "x\"y", 0) != 0);
    assert(jw_osd_client_show_level(&client, &deadline, "show-game-launch", 1, 1) != 0);
    assert(osd.calls == calls);

    /* Levels keep their single retry. */
    osd = (fake_osd){ .results = { -1, 0 }, .result_count = 2 };
    assert(jw_osd_client_show_level(&client, &deadline, "show-volume", 55, 30000) == 0);
    assert(osd.calls == 2 && osd.last_timeout == 30000);
    assert(strcmp(osd.last, "{\"type\":\"show-volume\",\"percent\":55}") == 0);
    osd = (fake_osd){ .results = { -1, -1, 0 }, .result_count = 3 };
    assert(jw_osd_client_show_level(&client, &deadline, "show-brightness", 5, 100) != 0);
    assert(osd.calls == 2);
}

int main(void) {
    confirm_inside_window();
    expiry_boundary_rearms();
    show_failure_disarms();
    late_prompt_is_removed();
    for (int how = REPLACE_VOLUME; how <= REPLACE_OSD_EXIT; how++) {
        replacement_disarms((replacement)how, false);
        replacement_disarms((replacement)how, true);
    }
    request_shapes();
    puts("PASS osd-client-test");
    return 0;
}
