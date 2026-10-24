#ifndef JW_PLATFORM_RAOFFLINEPROXY_H
#define JW_PLATFORM_RAOFFLINEPROXY_H

#include <stdbool.h>
#include <stdint.h>

#include "internal/services/supervisor.h"

/* RAOfflineProxy transient launch bridge (umrk-workspace/plans/RAOfflineProxy).
 * One bounded loopback readiness check against the supervised service's fixed
 * /leaf/health response. Only the fixed service id / protocol / ready body is
 * ever accepted; a host or port is never taken from the response. */

#define JW_ROP_SERVICE_ID "org.umrk.raofflineproxy"
#define JW_ROP_HEALTH_HOST "127.0.0.1"
#define JW_ROP_HEALTH_PORT 8080
#define JW_ROP_HEALTH_PATH "/leaf/health"
/* Total routing budget when the service is intended to run: at most this
 * long is ever added to a game launch. */
#define JW_ROP_ROUTING_BUDGET_MS 500

/* True only when host:port answers /leaf/health with the fixed
 * service/protocol/ready body before timeout_ms elapses. False on connect
 * failure, timeout, or any unexpected body. */
bool jw_raofflineproxy_health_ready(const char *host, uint16_t port,
                                    int timeout_ms);

/* The service is live when the supervisor holds a positive PGID in RUNNING
 * or STARTING for a present pak with a valid manifest (proxy plan P2).
 * STARTING counts: it is a launched generation that has not yet survived
 * the supervisor's settle window, and a launch in that window must not
 * lose the route. desired_enabled ("Start with Leaf") and session_run
 * ("Run") are intent flags and neither alone means a process exists. The
 * RetroArch gate and the bundled-Flycast route intent both decide with
 * this one predicate. NULL (no entry) is not live. */
bool jw_raofflineproxy_entry_live(const jw_svc_supervised *entry);

#endif
