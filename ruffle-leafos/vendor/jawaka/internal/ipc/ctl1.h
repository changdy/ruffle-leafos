#ifndef JW_CTL1_H
#define JW_CTL1_H

#include <stdbool.h>
#include <stddef.h>

#define JW_CTL1_VERSION 1
#define JW_CTL1_MAX_PAYLOAD (64u * 1024u)
#define JW_CTL1_ID_MAX 128
#define JW_CTL1_SERVICE_ID_MAX 128

typedef enum {
    JW_CTL1_OP_INVALID = 0,
    JW_CTL1_OP_LIST,
    JW_CTL1_OP_CAPABILITIES,
    JW_CTL1_OP_ENABLE,
    JW_CTL1_OP_DISABLE,
    JW_CTL1_OP_RUN,
    JW_CTL1_OP_STOP,
    JW_CTL1_OP_RESTART,
    JW_CTL1_OP_STATUS,
    JW_CTL1_OP_LOGS,
    JW_CTL1_OP_EXPORT_LOGS,
} jw_ctl1_operation;

typedef struct {
    jw_ctl1_operation operation;
    char id[JW_CTL1_ID_MAX + 1];
    char service_id[JW_CTL1_SERVICE_ID_MAX + 1];
    int tail;
    bool has_tail;
} jw_ctl1_request;

/* Parse and validate one complete CTL-1 JSON payload against
 * control-ipc-v1.schema.json. Every request shape is closed: unknown fields,
 * wrong types, unsupported versions, and trailing bytes fail closed.
 * `error_code` receives a stable slug; `request.id` is retained when it was a
 * valid correlation id even if a later field was invalid. */
bool jw_ctl1_parse_request(const char *payload, size_t payload_len,
                           jw_ctl1_request *request,
                           char *error_code, size_t error_code_size);

const char *jw_ctl1_operation_name(jw_ctl1_operation operation);

#endif
