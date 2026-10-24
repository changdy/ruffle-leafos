#include "internal/ipc/life1.h"

#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void parse_subscribe(const char *json, int ack_ms, int wait_ms,
                            jw_life1_mode mode, bool check_before_stop) {
    jw_life1_request request;
    char error[32] = "not-cleared";
    assert(jw_life1_parse_request(json, strlen(json), &request,
                                  error, sizeof(error)) == JW_LIFE1_PARSE_OK);
    assert(request.kind == JW_LIFE1_REQUEST_SUBSCRIBE);
    assert(strcmp(request.id, "1") == 0);
    assert(strcmp(request.service_id, "org.umrk.syncthing") == 0);
    assert(request.mode == mode);
    assert(request.ack_ms == ack_ms);
    assert(request.wait_ms == wait_ms);
    assert(request.check_before_stop == check_before_stop);
    assert(error[0] == '\0');
    jw_life1_request_destroy(&request);
}

static void parse_invalid(const char *json, const char *expected) {
    jw_life1_request request;
    char error[32] = {0};
    assert(jw_life1_parse_request(json, strlen(json), &request,
                                  error, sizeof(error)) ==
           JW_LIFE1_PARSE_INVALID);
    assert(strcmp(error, expected) == 0);
    jw_life1_request_destroy(&request);
}

static void parse_status(const char *json, jw_life1_status_kind kind,
                         int pending_items, int64_t pending_bytes,
                         bool has_pending_bytes, const char *reason) {
    jw_life1_status status;
    char error[32] = {0};
    assert(jw_life1_parse_status(json, strlen(json), &status,
                                 error, sizeof(error)) == JW_LIFE1_PARSE_OK);
    assert(status.kind == kind);
    assert(strcmp(status.launch_id, "launch-1") == 0);
    assert(status.pending_items == pending_items);
    assert(status.pending_bytes == pending_bytes);
    assert(status.has_pending_bytes == has_pending_bytes);
    if (reason) {
        assert(status.reason && strcmp(status.reason, reason) == 0);
    } else {
        assert(status.reason == NULL);
    }
    jw_life1_status_destroy(&status);
}

int main(void) {
    const char *canonical =
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\"],\"service_id\":\"org.umrk.syncthing\","
        "\"mode\":\"notify\",\"ack_ms\":250,\"wait_ms\":0}";
    parse_subscribe(canonical, 250, 0, JW_LIFE1_MODE_NOTIFY, false);
    parse_subscribe(
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\"],\"service_id\":\"org.umrk.syncthing\","
        "\"mode\":\"stop\",\"ack_ms\":5000,\"wait_ms\":20000}",
        1000, 15000, JW_LIFE1_MODE_STOP, false);
    parse_subscribe(
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\"],\"service_id\":\"org.umrk.syncthing\","
        "\"mode\":\"stop\",\"ack_ms\":250,\"wait_ms\":15000,"
        "\"check_before_stop\":true}",
        250, 15000, JW_LIFE1_MODE_STOP, true);

    jw_life1_request request;
    char error[32] = {0};
    const char *state = "{\"v\":1,\"op\":\"game.state\",\"id\":\"7\"}";
    assert(jw_life1_parse_request(state, strlen(state), &request,
                                  error, sizeof(error)) == JW_LIFE1_PARSE_OK);
    assert(request.kind == JW_LIFE1_REQUEST_GAME_STATE);
    assert(strcmp(request.id, "7") == 0);
    jw_life1_request_destroy(&request);

    assert(jw_life1_parse_request(
               "{\"v\":1,\"op\":\"list\",\"id\":\"1\"}", 28,
               &request, error, sizeof(error)) == JW_LIFE1_PARSE_NOT_LIFE1);
    jw_life1_request_destroy(&request);

    parse_invalid(
        "{\"v\":2,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\"],\"service_id\":\"org.umrk.syncthing\","
        "\"mode\":\"notify\",\"ack_ms\":250,\"wait_ms\":0}",
        "unsupported-version");
    parse_invalid(
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\",\"storage\"],"
        "\"service_id\":\"org.umrk.syncthing\",\"mode\":\"notify\","
        "\"ack_ms\":250,\"wait_ms\":0}",
        "invalid-payload");
    parse_invalid(
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\"],\"service_id\":\"org.umrk.syncthing\","
        "\"mode\":\"notify\",\"ack_ms\":-1,\"wait_ms\":0}",
        "invalid-payload");
    parse_invalid(
        "{\"v\":1,\"op\":\"game.state\",\"id\":\"7\",\"extra\":true}",
        "invalid-payload");
    parse_invalid(
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\"],\"service_id\":\"org.umrk.syncthing\","
        "\"mode\":\"notify\",\"ack_ms\":250,\"wait_ms\":0,"
        "\"check_before_stop\":true}",
        "invalid-payload");
    parse_invalid(
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\"],\"service_id\":\"org.umrk.syncthing\","
        "\"mode\":\"notify\",\"ack_ms\":250,\"wait_ms\":0,"
        "\"check_before_stop\":false}",
        "invalid-payload");
    parse_invalid(
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\"],\"service_id\":\"org.umrk.syncthing\","
        "\"mode\":\"stop\",\"ack_ms\":250,\"wait_ms\":15000,"
        "\"check_before_stop\":\"yes\"}",
        "invalid-payload");

    parse_status(
        "{\"v\":1,\"status\":\"waiting\",\"launch_id\":\"launch-1\","
        "\"pending_items\":3}", JW_LIFE1_STATUS_WAITING, 3, 0, false, NULL);
    parse_status(
        "{\"v\":1,\"status\":\"waiting\",\"launch_id\":\"launch-1\","
        "\"pending_items\":3,\"pending_bytes\":49152}",
        JW_LIFE1_STATUS_WAITING, 3, 49152, true, NULL);
    parse_status(
        "{\"v\":1,\"status\":\"ready\",\"launch_id\":\"launch-1\"}",
        JW_LIFE1_STATUS_READY, 0, 0, false, NULL);
    parse_status(
        "{\"v\":1,\"status\":\"stop\",\"launch_id\":\"launch-1\"}",
        JW_LIFE1_STATUS_STOP, 0, 0, false, NULL);
    parse_status(
        "{\"v\":1,\"status\":\"error\",\"launch_id\":\"launch-1\","
        "\"reason\":\"inbound failed\"}", JW_LIFE1_STATUS_ERROR, 0, 0, false,
        "inbound failed");
    jw_life1_status parsed_status;
    const char *invalid_status =
        "{\"v\":1,\"status\":\"ready\",\"launch_id\":\"launch-1\","
        "\"extra\":true}";
    assert(jw_life1_parse_status(
        invalid_status, strlen(invalid_status), &parsed_status,
        error, sizeof(error)) ==
        JW_LIFE1_PARSE_INVALID);
    jw_life1_status_destroy(&parsed_status);

    char *json = jw_life1_build_ok("1");
    assert(json && strcmp(json, "{\"v\":1,\"id\":\"1\",\"ok\":true}") == 0);
    cJSON_free(json);
    json = jw_life1_build_game_state_inactive("7");
    assert(json && strcmp(json, "{\"v\":1,\"id\":\"7\",\"active\":false}") == 0);
    cJSON_free(json);
    json = jw_life1_build_game_state_active(
        "7", "launch-1", "secondary_sd", "/card/Saves", "/card/States");
    assert(json && strcmp(json,
        "{\"v\":1,\"id\":\"7\",\"active\":true,\"launch_id\":\"launch-1\","
        "\"source_id\":\"secondary_sd\",\"saves_path\":\"/card/Saves\","
        "\"states_path\":\"/card/States\"}") == 0);
    cJSON_free(json);
    json = jw_life1_build_game_start(
        "launch-1", "secondary_sd", "/card/Saves", "/card/States", 15000);
    assert(json && strcmp(json,
        "{\"v\":1,\"event\":\"game.start\",\"launch_id\":\"launch-1\","
        "\"source_id\":\"secondary_sd\",\"saves_path\":\"/card/Saves\","
        "\"states_path\":\"/card/States\",\"wait_budget_ms\":15000}") == 0);
    cJSON_free(json);
    json = jw_life1_build_game_check(
        "launch-1", "secondary_sd", "/card/Saves", "/card/States", 15000);
    assert(json && strcmp(json,
        "{\"v\":1,\"event\":\"game.check\",\"launch_id\":\"launch-1\","
        "\"source_id\":\"secondary_sd\",\"saves_path\":\"/card/Saves\","
        "\"states_path\":\"/card/States\",\"wait_budget_ms\":15000}") == 0);
    cJSON_free(json);
    json = jw_life1_build_game_cancel("launch-1");
    assert(json && strcmp(json,
        "{\"v\":1,\"event\":\"game.cancel\",\"launch_id\":\"launch-1\"}") == 0);
    cJSON_free(json);
    json = jw_life1_build_game_abort("launch-1");
    assert(json && strcmp(json,
        "{\"v\":1,\"event\":\"game.abort\",\"launch_id\":\"launch-1\"}") == 0);
    cJSON_free(json);
    json = jw_life1_build_game_finish("launch-1");
    assert(json && strcmp(json,
        "{\"v\":1,\"event\":\"game.finish\",\"launch_id\":\"launch-1\"}") == 0);
    cJSON_free(json);
    json = jw_life1_build_error(
        "2", "stale-generation-peer",
        "subscriber pid is not a member of the current generation's reserved process group");
    assert(json && strcmp(json,
        "{\"v\":1,\"id\":\"2\",\"error\":{\"code\":\"stale-generation-peer\","
        "\"message\":\"subscriber pid is not a member of the current generation's reserved process group\"}}") == 0);
    cJSON_free(json);

    puts("PASS life1-test");
    return 0;
}
