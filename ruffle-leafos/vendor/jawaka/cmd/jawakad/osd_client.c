#include "cmd/jawakad/osd_client.h"

#include "internal/launcher/pico8.h"

#include <stdio.h>
#include <string.h>

void jw_osd_client_prompt_lost(long long *exit_deadline_ms) {
    if (exit_deadline_ms) *exit_deadline_ms = 0;
}

int jw_osd_client_show_level(const jw_osd_client *client, long long *exit_deadline_ms,
                             const char *type, int percent, int timeout_ms) {
    jw_osd_client_prompt_lost(exit_deadline_ms);
    if (!client || !client->request || !type ||
        (strcmp(type, "show-volume") != 0 && strcmp(type, "show-brightness") != 0)) {
        return -1;
    }
    char request[96];
    snprintf(request, sizeof(request), "{\"type\":\"%s\",\"percent\":%d}", type, percent);
    for (int attempt = 0; attempt < 2; attempt++) {
        if (client->request(client->ctx, request, timeout_ms, true) == 0) return 0;
        if (attempt == 0 && client->sleep_ms) client->sleep_ms(client->ctx, 100);
    }
    return -1;
}

int jw_osd_client_show_stage(const jw_osd_client *client, long long *exit_deadline_ms,
                             const char *stage, int pending_items) {
    jw_osd_client_prompt_lost(exit_deadline_ms);
    if (!client || !client->request || !stage || !stage[0] ||
        strcmp(stage, "pico8-exit") == 0 || strchr(stage, '"') || strchr(stage, '\\')) {
        return -1;
    }
    char request[128];
    if (strcmp(stage, "syncing") == 0) {
        snprintf(request, sizeof(request),
                 "{\"type\":\"show-game-launch\",\"stage\":\"syncing\","
                 "\"pending_items\":%d}", pending_items < 0 ? 0 : pending_items);
    } else {
        snprintf(request, sizeof(request),
                 "{\"type\":\"show-game-launch\",\"stage\":\"%s\"}", stage);
    }
    return client->request(client->ctx, request, JW_OSD_CLIENT_BANNER_TIMEOUT_MS, true);
}

int jw_osd_client_hide(const jw_osd_client *client, long long *exit_deadline_ms) {
    jw_osd_client_prompt_lost(exit_deadline_ms);
    if (!client || !client->request) return -1;
    return client->request(client->ctx, "{\"type\":\"hide-game-launch\"}",
                           JW_OSD_CLIENT_BANNER_TIMEOUT_MS, false);
}

jw_osd_pico8_menu jw_osd_client_pico8_menu(const jw_osd_client *client,
                                           long long *exit_deadline_ms) {
    if (!client || !client->request || !client->now_ms || !exit_deadline_ms) {
        jw_osd_client_prompt_lost(exit_deadline_ms);
        return JW_OSD_PICO8_MENU_UNAVAILABLE;
    }
    if (jw_pico8_exit_confirmed(exit_deadline_ms, client->now_ms(client->ctx))) {
        (void)jw_osd_client_hide(client, exit_deadline_ms);
        return JW_OSD_PICO8_MENU_CONFIRMED;
    }
    /* The OSD refuses to submit the prompt once the daemon has stopped
       waiting for it, so a request delayed before submission never appears. */
    char request[128];
    snprintf(request, sizeof(request),
             "{\"type\":\"show-game-launch\",\"stage\":\"pico8-exit\","
             "\"expires_ms\":%lld}",
             client->now_ms(client->ctx) + JW_OSD_CLIENT_BANNER_TIMEOUT_MS);
    if (client->request(client->ctx, request, JW_OSD_CLIENT_BANNER_TIMEOUT_MS,
                        true) != 0) {
        /* Never keep a hidden confirmation armed if the OSD cannot show it. */
        jw_osd_client_prompt_lost(exit_deadline_ms);
        /* A late or missing reply does not mean the prompt stayed off screen:
           it may have been submitted just after the timeout. Take it down, and
           when even that cannot be confirmed, end the OSD process. */
        if (client->request(client->ctx, "{\"type\":\"hide-game-launch\"}",
                            JW_OSD_CLIENT_BANNER_TIMEOUT_MS, false) != 0 &&
            client->discard_osd) {
            client->discard_osd(client->ctx);
        }
        return JW_OSD_PICO8_MENU_UNAVAILABLE;
    }
    return JW_OSD_PICO8_MENU_ARMED;
}
