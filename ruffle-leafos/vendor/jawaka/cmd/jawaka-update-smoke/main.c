#include "internal/update/update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(FILE *stream)
{
    fprintf(stream,
            "Usage: jawaka-update-smoke --state-dir PATH --platform ID --manifest PATH\n"
            "       jawaka-update-smoke --state-dir PATH --platform ID --releases-url URL [--all]\n"
            "       jawaka-update-smoke --state-dir PATH --platform ID --job-scenario\n"
            "\n"
            "--job-scenario drives the async check job the way jawakad does, against\n"
            "the feed in JAWAKA_UPDATE_RELEASES_URL, and prints one JSON object.\n");
}

static void wait_for_job(jw_update_status *status, jw_update_check_job *job)
{
    for (int i = 0; i < 400 && job->active; i++) {
        struct timespec pause = { 0, 50 * 1000 * 1000 };
        nanosleep(&pause, NULL);
        jw_update_check_poll(status, job);
    }
}

/* A release-list load asked for during a routine check queues behind it, and a
   later reload keeps the chosen release and its finished download. */
static int run_job_scenario(const char *state_dir, const char *platform)
{
    jw_update_status status;
    jw_update_check_job job;
    jw_update_status_init(&status, platform, state_dir);
    jw_update_check_job_init(&job);

    cJSON *out = cJSON_CreateObject();
    jw_update_check_start(&status, &job, state_dir, JW_UPDATE_CHANNEL_BETA,
                          JW_UPDATE_SCOPE_LATEST);
    cJSON_AddStringToObject(out, "state_while_checking",
                            jw_update_status_name(status.status));
    jw_update_check_start(&status, &job, state_dir, JW_UPDATE_CHANNEL_BETA,
                          JW_UPDATE_SCOPE_ALL);
    cJSON_AddBoolToObject(out, "loading_after_queue", status.options_loading);
    wait_for_job(&status, &job);
    cJSON_AddItemToObject(out, "after_queue", jw_update_status_to_json(&status));

    int pick = status.option_count - 1;
    jw_update_select_option(&status, pick);
    status.downloaded = true;   /* as if the chosen release had finished downloading */
    jw_update_check_start(&status, &job, state_dir, JW_UPDATE_CHANNEL_BETA,
                          JW_UPDATE_SCOPE_ALL);
    cJSON_AddStringToObject(out, "state_while_loading",
                            jw_update_status_name(status.status));
    wait_for_job(&status, &job);
    cJSON_AddNumberToObject(out, "picked", pick);
    cJSON_AddItemToObject(out, "after_reload", jw_update_status_to_json(&status));

    /* Reopening the page runs a routine check. Asking for the list during it
       must queue a fresh load, not trust the complete list from before. */
    jw_update_check_start(&status, &job, state_dir, JW_UPDATE_CHANNEL_BETA,
                          JW_UPDATE_SCOPE_LATEST);
    cJSON_AddBoolToObject(out, "complete_while_rechecking", status.options_complete);
    jw_update_check_start(&status, &job, state_dir, JW_UPDATE_CHANNEL_BETA,
                          JW_UPDATE_SCOPE_ALL);
    cJSON_AddBoolToObject(out, "loading_after_second_queue", status.options_loading);
    wait_for_job(&status, &job);
    cJSON_AddItemToObject(out, "after_recheck", jw_update_status_to_json(&status));
    jw_update_check_job_wait(&job);

    char *printed = cJSON_Print(out);
    cJSON_Delete(out);
    if (!printed) {
        return 1;
    }
    puts(printed);
    free(printed);
    return 0;
}

int main(int argc, char **argv)
{
    const char *state_dir = NULL;
    const char *platform = NULL;
    const char *manifest = NULL;
    const char *releases_url = NULL;
    jw_update_check_scope scope = JW_UPDATE_SCOPE_LATEST;
    bool job_scenario = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) {
            state_dir = argv[++i];
        } else if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            platform = argv[++i];
        } else if (strcmp(argv[i], "--manifest") == 0 && i + 1 < argc) {
            manifest = argv[++i];
        } else if (strcmp(argv[i], "--releases-url") == 0 && i + 1 < argc) {
            releases_url = argv[++i];
        } else if (strcmp(argv[i], "--all") == 0) {
            scope = JW_UPDATE_SCOPE_ALL;
        } else if (strcmp(argv[i], "--job-scenario") == 0) {
            job_scenario = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else {
            usage(stderr);
            return 2;
        }
    }

    if (state_dir && platform && job_scenario && !manifest && !releases_url) {
        return run_job_scenario(state_dir, platform);
    }
    if (!state_dir || !platform || job_scenario || (!manifest == !releases_url)) {
        usage(stderr);
        return 2;
    }

    jw_update_status status;
    jw_update_status_init(&status, platform, state_dir);
    int rc = manifest
        ? jw_update_check_local_manifest(&status, state_dir, platform, manifest)
        : jw_update_check_releases(&status, state_dir, platform, releases_url, scope);
    cJSON *json = jw_update_status_to_json(&status);
    char *printed = cJSON_Print(json);
    cJSON_Delete(json);
    if (!printed) {
        return 1;
    }
    puts(printed);
    free(printed);
    return rc == 0 ? 0 : 1;
}
