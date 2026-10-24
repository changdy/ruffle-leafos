#include "internal/ipc/ctl1.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void parse_ok(const char *json, jw_ctl1_operation operation,
                     const char *id, const char *service_id) {
    jw_ctl1_request request;
    char error[32] = "not-cleared";
    assert(jw_ctl1_parse_request(json, strlen(json), &request,
                                 error, sizeof(error)));
    assert(request.operation == operation);
    assert(strcmp(request.id, id) == 0);
    assert(strcmp(request.service_id, service_id ? service_id : "") == 0);
    assert(error[0] == '\0');
}

static void parse_error(const char *json, const char *expected) {
    jw_ctl1_request request;
    char error[32] = {0};
    assert(!jw_ctl1_parse_request(json, strlen(json), &request,
                                  error, sizeof(error)));
    assert(strcmp(error, expected) == 0);
}

int main(void) {
    /* Canonical A0 fixture payloads. */
    parse_ok("{\"v\":1,\"op\":\"list\",\"id\":\"1\"}",
             JW_CTL1_OP_LIST, "1", NULL);
    parse_ok("{\"v\":1,\"op\":\"capabilities\",\"id\":\"5\"}",
             JW_CTL1_OP_CAPABILITIES, "5", NULL);
    parse_ok("{\"v\":1,\"op\":\"run\",\"id\":\"3\","
             "\"service_id\":\"org.umrk.syncthing\"}",
             JW_CTL1_OP_RUN, "3", "org.umrk.syncthing");

    jw_ctl1_request request;
    char error[32] = {0};
    const char *logs = "{\"v\":1,\"op\":\"logs\",\"id\":\"6\","
                       "\"service_id\":\"org.umrk.syncthing\",\"tail\":50}";
    assert(jw_ctl1_parse_request(logs, strlen(logs), &request,
                                 error, sizeof(error)));
    assert(request.operation == JW_CTL1_OP_LOGS);
    assert(request.has_tail && request.tail == 50);

    parse_error("{\"v\":2,\"op\":\"list\",\"id\":\"4\"}",
                "unsupported-version");
    parse_error("{\"v\":1,\"op\":\"bogus\",\"id\":\"1\"}",
                "unknown-op");
    parse_error("{\"v\":1,\"op\":\"list\",\"id\":\"1\",\"extra\":true}",
                "invalid-payload");
    parse_error("{\"v\":1,\"op\":\"run\",\"id\":\"1\"}",
                "invalid-payload");
    parse_error("{\"v\":1,\"op\":\"export-logs\",\"id\":\"1\","
                "\"service_id\":\"org.umrk.x\",\"tail\":1}",
                "invalid-payload");
    parse_error("{\"v\":1,\"op\":\"logs\",\"id\":\"1\","
                "\"service_id\":\"org.umrk.x\",\"tail\":1.5}",
                "invalid-payload");
    parse_error("{\"v\":1,\"op\":\"list\",\"id\":\"visible\\u0000hidden\"}",
                "invalid-payload");
    parse_error("{\"v\":1,\"op\":\"run\",\"id\":\"1\","
                "\"service_id\":\"org.umrk.good\\u0000.bad\"}",
                "invalid-payload");
    parse_ok("{\"v\":1,\"op\":\"list\",\"id\":\"literal\\\\u0000\"}",
             JW_CTL1_OP_LIST, "literal\\u0000", NULL);

    const char embedded[] = "{\"v\":1}\0garbage";
    assert(!jw_ctl1_parse_request(embedded, sizeof(embedded) - 1u,
                                  &request, error, sizeof(error)));

    char oversized[JW_CTL1_MAX_PAYLOAD + 1u];
    memset(oversized, ' ', sizeof(oversized));
    assert(!jw_ctl1_parse_request(oversized, sizeof(oversized),
                                  &request, error, sizeof(error)));

    puts("PASS ctl1-test");
    return 0;
}
