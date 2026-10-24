#include "internal/scrape/ss_client.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    jw_ss_search_status outcomes[8];
    int outcome_count;
    int call_count;
    int systems[8];
    char hashes[8][33];
    long sizes[8];
} request_trace;

static jw_ss_search_status fake_search(
    const char *rom_name, const char *md5_hash,
    long file_size, int system_id, jw_ss_result *result, void *userdata) {
    request_trace *trace = userdata;
    int call = trace->call_count++;
    assert(strcmp(rom_name, "game.zip") == 0);
    assert(call < trace->outcome_count);
    assert(result->game_name[0] == '\0');
    trace->systems[call] = system_id;
    snprintf(trace->hashes[call], sizeof(trace->hashes[call]), "%s", md5_hash);
    trace->sizes[call] = file_size;
    if (trace->outcomes[call] == JW_SS_SEARCH_FOUND)
        snprintf(result->game_name, sizeof(result->game_name), "Found");
    else
        snprintf(result->game_name, sizeof(result->game_name), "Stale");
    return trace->outcomes[call];
}

static void test_ordered_hash_and_name_search(void) {
    char path[] = "/tmp/jawaka-ss-client-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, "abc", 3) == 3);
    close(fd);

    request_trace trace = {
        .outcomes = {
            JW_SS_SEARCH_NOT_FOUND, JW_SS_SEARCH_NOT_FOUND,
            JW_SS_SEARCH_NOT_FOUND, JW_SS_SEARCH_FOUND,
        },
        .outcome_count = 4,
    };
    int systems[] = {56, 227, 230};
    jw_ss_result result;
    jw_ss_test_set_search_request(fake_search, &trace);

    assert(jw_ss_search_rom_platforms(
               NULL, "game.zip", path, systems, 3,
               NULL, 0, NULL, 0, &result) == JW_SS_SEARCH_FOUND);
    assert(trace.call_count == 4);
    assert(trace.systems[0] == 56 && trace.systems[1] == 56);
    assert(trace.systems[2] == 227 && trace.systems[3] == 227);
    assert(strcmp(trace.hashes[0], "900150983cd24fb0d6963f7d28e17f72") == 0);
    assert(strcmp(trace.hashes[0], trace.hashes[2]) == 0);
    assert(trace.hashes[1][0] == '\0' && trace.hashes[3][0] == '\0');
    assert(trace.sizes[0] == 3 && trace.sizes[2] == 3);
    assert(strcmp(result.game_name, "Found") == 0);

    unlink(path);
}

static void test_unhashable_path_and_terminal_results(void) {
    int systems[] = {56, 227, 230};
    jw_ss_result result;
    request_trace not_found_then_success = {
        .outcomes = {JW_SS_SEARCH_NOT_FOUND, JW_SS_SEARCH_FOUND},
        .outcome_count = 2,
    };
    jw_ss_test_set_search_request(fake_search, &not_found_then_success);
    assert(jw_ss_search_rom_platforms(
               NULL, "game.zip", NULL, systems, 3,
               NULL, 0, NULL, 0, &result) == JW_SS_SEARCH_FOUND);
    assert(not_found_then_success.call_count == 2);
    assert(not_found_then_success.systems[0] == 56);
    assert(not_found_then_success.systems[1] == 227);
    assert(not_found_then_success.hashes[0][0] == '\0');
    assert(not_found_then_success.hashes[1][0] == '\0');

    for (int terminal = JW_SS_SEARCH_CANCELLED;
         terminal <= JW_SS_SEARCH_ERROR; terminal++) {
        request_trace trace = {
            .outcomes = {terminal},
            .outcome_count = 1,
        };
        jw_ss_test_set_search_request(fake_search, &trace);
        assert(jw_ss_search_rom_platforms(
                   NULL, "game.zip", NULL, systems, 3,
                   NULL, 0, NULL, 0, &result) ==
               (jw_ss_search_status)terminal);
        assert(trace.call_count == 1);
    }
}

static void test_invalid_platform_list(void) {
    request_trace trace = {0};
    jw_ss_result result;
    jw_ss_test_set_search_request(fake_search, &trace);
    assert(jw_ss_search_rom_platforms(
               NULL, "game.zip", NULL, NULL, 0,
               NULL, 0, NULL, 0, &result) == JW_SS_SEARCH_ERROR);
    assert(trace.call_count == 0);
}

static void test_no_media_keeps_same_identity_recovery(void) {
    char path[] = "/tmp/jawaka-ss-client-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, "abc", 3) == 3);
    close(fd);

    request_trace trace = {
        .outcomes = {JW_SS_SEARCH_NO_MEDIA, JW_SS_SEARCH_FOUND},
        .outcome_count = 2,
    };
    int system = 56;
    jw_ss_result result;
    jw_ss_test_set_search_request(fake_search, &trace);

    assert(jw_ss_search_rom_platforms(
               NULL, "game.zip", path, &system, 1,
               NULL, 0, NULL, 0, &result) == JW_SS_SEARCH_FOUND);
    assert(trace.call_count == 2);
    assert(trace.systems[0] == 56 && trace.systems[1] == 56);
    assert(trace.hashes[0][0] != '\0' && trace.hashes[1][0] == '\0');
    assert(strcmp(result.game_name, "Found") == 0);

    unlink(path);
}

static void test_no_media_aggregates_across_platforms(void) {
    request_trace trace = {
        .outcomes = {
            JW_SS_SEARCH_NO_MEDIA,
            JW_SS_SEARCH_NOT_FOUND,
            JW_SS_SEARCH_NO_MEDIA,
        },
        .outcome_count = 3,
    };
    int systems[] = {56, 227, 230};
    jw_ss_result result;
    jw_ss_test_set_search_request(fake_search, &trace);

    assert(jw_ss_search_rom_platforms(
               NULL, "game.zip", NULL, systems, 3,
               NULL, 0, NULL, 0, &result) == JW_SS_SEARCH_NO_MEDIA);
    assert(trace.call_count == 3);
    assert(trace.systems[0] == 56);
    assert(trace.systems[1] == 227);
    assert(trace.systems[2] == 230);
}

int main(void) {
    test_ordered_hash_and_name_search();
    test_unhashable_path_and_terminal_results();
    test_invalid_platform_list();
    test_no_media_keeps_same_identity_recovery();
    test_no_media_aggregates_across_platforms();
    jw_ss_test_set_search_request(NULL, NULL);
    puts("ss_client_test: ok");
    return 0;
}
