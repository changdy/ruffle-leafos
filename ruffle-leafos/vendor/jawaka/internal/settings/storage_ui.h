#ifndef JW_SETTINGS_STORAGE_UI_H
#define JW_SETTINGS_STORAGE_UI_H

#include <stdbool.h>
#include <stddef.h>

#include "internal/ipc/ipc_client.h"

/* SD card warning, repair confirmation and result screens, shared by the
   launcher (which shows them when a card changes) and Settings (which lets
   the user act on a card later). All screens are blocking Catastrophe
   dialogs; the daemon owns every decision about whether repair is possible. */

#define JW_STORAGE_UI_SOURCE_COUNT 2
extern const char *const jw_storage_ui_sources[JW_STORAGE_UI_SOURCE_COUNT];

bool jw_storage_ui_is_read_only(const jw_ipc_storage_status_info *card);
bool jw_storage_ui_needs_repair(const jw_ipc_storage_status_info *card);
/* "launcher SD card (MLPPRDLEAF)" */
void jw_storage_ui_card_name(const jw_ipc_storage_status_info *card,
                             char *out, size_t out_size);
/* Short translated state for a Settings row. */
const char *jw_storage_ui_card_state_key(const jw_ipc_storage_status_info *card);

const char *jw_storage_ui_card_state(const jw_ipc_storage_status_info *card);

/* The one-per-boot warning for a newly read-only card. Acknowledges it, and
   when repair is available lets the user go straight to the confirmation.
   Returns true when a repair was committed and the device is restarting. */
bool jw_storage_ui_show_warning(const char *socket_path,
                                const jw_ipc_storage_status_info *card);

/* Re-show the library card's warning after a failed DB write, even when the
   startup warning was acknowledged. Returns true when restarting for repair. */
bool jw_storage_ui_show_library_read_only(const char *socket_path);

/* Confirmation, then the request. mode is "repair" or "check". Returns true
   when the device is restarting. */
bool jw_storage_ui_request_repair(const char *socket_path,
                                  const jw_ipc_storage_status_info *card,
                                  const char *mode);

/* Shows the last repair result once and acknowledges it. library_writable is
   false when another card still keeps the library read-only. */
void jw_storage_ui_show_repair_result(
    const char *socket_path, const jw_ipc_storage_status_info *card,
    bool library_writable);

/* Settings > System > SD Cards: choose a card, then an action. Sets
   *unmount_secondary when the user chose to unmount the second card, so the
   caller runs its existing safe-unmount flow. */
void jw_storage_ui_manage_cards(const char *socket_path, char *status,
                                size_t status_size, bool *unmount_secondary);

#endif /* JW_SETTINGS_STORAGE_UI_H */
