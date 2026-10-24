#ifndef JW_STORE_MANAGED_APPS_H
#define JW_STORE_MANAGED_APPS_H

#define JW_PAKRAT_MANAGED_APPS_MAX 128

typedef struct {
    char paths[JW_PAKRAT_MANAGED_APPS_MAX][512];
    int  count;
} jw_pakrat_managed_apps;

/* Loads managed app paths from <platform_root>/manifest.json. Paths use the
   Apps namespace, e.g. "mlp1/SSHServer.pak" or "shared/RetroArch.pak". */
int jw_pakrat_load_managed_apps(const char *platform_root,
                                jw_pakrat_managed_apps *out);

/* Returns 1 when install_path is managed, 0 when it is store-manageable. Accepts
   either Apps-namespace paths or scanned paths prefixed with "Apps/". */
int jw_pakrat_managed_app_path_blocked(const jw_pakrat_managed_apps *managed,
                                       const char *install_path);

int jw_pakrat_managed_app_path_blocked_from_platform(const char *platform_root,
                                                     const char *install_path);

#endif
