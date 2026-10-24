#ifndef JW_PLATFORM_DEVICE_BACKEND_H
#define JW_PLATFORM_DEVICE_BACKEND_H

#include "internal/platform/device.h"

typedef struct {
    const char *platform_id;
    const char *platform_name;
    jw_platform_capabilities capabilities;

    int  (*init)(jw_platform_context *ctx);
    void (*shutdown)(jw_platform_context *ctx);
    void (*get_status)(jw_platform_context *ctx, jw_platform_status *out);
    void (*get_audio_status)(jw_platform_context *ctx, jw_platform_status *out);
    unsigned (*audio_tick)(jw_platform_context *ctx);
    /* Optional: see jw_platform_poll_fds / jw_platform_next_deadline_ms. */
    int  (*poll_fds)(jw_platform_context *ctx, int *fds, int max);
    long long (*next_deadline_ms)(jw_platform_context *ctx, long long now_ms);
    /* Optional: see jw_platform_sleep_audio / jw_platform_wake_audio. */
    void (*sleep_audio)(jw_platform_context *ctx);
    void (*wake_audio)(jw_platform_context *ctx);
    void (*audio_reconcile)(jw_platform_context *ctx, const char *reason);
    void (*frontend_ready)(jw_platform_context *ctx, const char *role,
                           jw_platform_result *out);
    void (*perform_action)(jw_platform_context *ctx, jw_platform_action action,
                           int value, jw_platform_result *out);
    void (*get_performance_status)(jw_platform_context *ctx,
                                   jw_platform_perf_status *out);
    void (*apply_performance)(jw_platform_context *ctx,
                              const jw_platform_perf_request *request,
                              jw_platform_result *out);
    bool (*storage_tick)(jw_platform_context *ctx);
    void (*get_storage_status)(jw_platform_context *ctx, const char *source_id,
                               jw_platform_storage_status *out);
    void (*safe_unmount_storage)(jw_platform_context *ctx, const char *source_id,
                                 jw_platform_result *out);
    int  (*storage_roots)(jw_platform_context *ctx, jw_platform_storage_root *out,
                          int max);
    void (*storage_repair_capability)(jw_platform_context *ctx, const char *fs_type,
                                      const char *uuid, bool block_write_protected,
                                      jw_platform_storage_repair_capability *out);
    void (*set_led)(jw_platform_context *ctx, const jw_led_config *cfg,
                    jw_platform_result *out);
    /* Optional: the current mode of the active output. 0 on success, -1 unknown. */
    int  (*get_display_mode)(jw_platform_context *ctx, int *width, int *height, int *hz);
    /* Optional: see jw_platform_set_power_hard_cut_s. NULL = unsupported. */
    void (*set_power_hard_cut)(jw_platform_context *ctx, int seconds,
                               jw_platform_result *out);
} jw_platform_backend;

const jw_platform_backend *jw_platform_get_backend(void);

void jw_platform_result_set(jw_platform_result *out,
                            jw_platform_result_code code,
                            const char *message);
void jw_platform_result_set_value(jw_platform_result *out,
                                  jw_platform_result_code code,
                                  const char *message,
                                  int value);
void jw_platform_result_unsupported(jw_platform_action action,
                                    const char *platform_id,
                                    jw_platform_result *out);

#endif /* JW_PLATFORM_DEVICE_BACKEND_H */
