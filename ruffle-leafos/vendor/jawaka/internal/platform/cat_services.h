#ifndef JW_PLATFORM_CAT_SERVICES_H
#define JW_PLATFORM_CAT_SERVICES_H

void jw_cat_services_install(const char *socket_path);
void jw_cat_services_clear(void);

/* Push cached status values from a background thread so the Catastrophe status
   bar (drawn on the render thread) never spawns wpa_cli or blocks on a daemon
   IPC. The status-bar hooks just return whatever was last set here. */
void jw_cat_services_set_wifi_strength(int strength);
void jw_cat_services_set_power(int battery_percent, int charging);

#endif /* JW_PLATFORM_CAT_SERVICES_H */
