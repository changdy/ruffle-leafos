#include "internal/platform/cat_services.h"

#include "catastrophe.h"
#include "internal/ipc/ipc_client.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static char s_socket_path[1024];
/* Atomics: the setters are documented as callable from a background poller
   while the render thread reads via the hooks below — plain ints there would
   be a C data race even when the values are word-sized. */
static atomic_int s_battery_percent = -1;
static atomic_int s_charging_state = -1;
static atomic_int s_wifi_strength = -1;

static int jw__cat_socket_ready(void) {
    return s_socket_path[0] != '\0';
}

/* The status bar polls these hooks on the render thread, so they must never
   spawn or block — they just return the last value the background poller pushed
   via jw_cat_services_set_*(). */
static int jw__cat_wifi_strength(void *userdata) {
    (void)userdata;
    return s_wifi_strength;
}

static int jw__cat_battery_percent(void *userdata) {
    (void)userdata;
    return s_battery_percent;
}

static int jw__cat_charging_state(void *userdata) {
    (void)userdata;
    return s_charging_state;
}

void jw_cat_services_set_wifi_strength(int strength) {
    s_wifi_strength = strength;
}

void jw_cat_services_set_power(int battery_percent, int charging) {
    s_battery_percent = battery_percent;
    s_charging_state = charging;
}

static int jw__cat_power_suspend(void *userdata) {
    (void)userdata;
    return jw__cat_socket_ready() &&
           jw_ipc_platform_action(s_socket_path, "sleep", 0) == 0
        ? CAT_OK
        : CAT_ERROR;
}

static int jw__cat_power_shutdown(void *userdata) {
    (void)userdata;
    return jw__cat_socket_ready() &&
           jw_ipc_platform_action(s_socket_path, "poweroff", 0) == 0
        ? CAT_OK
        : CAT_ERROR;
}

void jw_cat_services_install(const char *socket_path) {
    snprintf(s_socket_path, sizeof(s_socket_path), "%s", socket_path ? socket_path : "");
    s_battery_percent = -1;
    s_charging_state = -1;
    s_wifi_strength = -1;

    cat_platform_services services;
    memset(&services, 0, sizeof(services));
    services.wifi_strength = jw__cat_wifi_strength;
    services.battery_percent = jw__cat_battery_percent;
    services.charging_state = jw__cat_charging_state;
    services.power_suspend = jw__cat_power_suspend;
    services.power_shutdown = jw__cat_power_shutdown;
    cat_set_platform_services(&services);
}

void jw_cat_services_clear(void) {
    s_socket_path[0] = '\0';
    s_battery_percent = -1;
    s_charging_state = -1;
    s_wifi_strength = -1;
    cat_set_platform_services(NULL);
}
