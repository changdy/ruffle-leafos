#ifndef JW_POWER_BOOT_RESUME_H
#define JW_POWER_BOOT_RESUME_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

/* Resume Game on Boot: one pending continuation of one power-button shutdown.

   A power-hold save that ends `saved` writes boot-resume.json into the
   launcher control state ($UMRK_INTERNAL_DATA_PATH). The next daemon start
   reads it once and deletes it before deciding anything, so a boot gets at most
   one attempt. The record names the game, core and state file; it does not hash
   anything. Whether the state still fits is settled by RetroArch's
   LOAD_STATE_SYNC reply, never here.

   Everything in this module takes paths, the platform and the current boot id
   as arguments so host tests need no device. Plan:
   umrk-workspace/plans/Jawaka/auto-resume-after-power-off.md. */

#ifndef PATH_MAX
#define JW_BOOT_RESUME_PATH_MAX 4096
#else
#define JW_BOOT_RESUME_PATH_MAX PATH_MAX
#endif

#define JW_BOOT_RESUME_FILENAME "boot-resume.json"
#define JW_BOOT_RESUME_SCHEMA 1
#define JW_BOOT_RESUME_SLOT 99            /* JW_RA_GAME_SWITCHER_STATE_SLOT */
#define JW_BOOT_RESUME_SETTING_KEY "resume_game_on_boot"

#define JW_BOOT_RESUME_PLATFORM_MAX 32
#define JW_BOOT_RESUME_BOOT_ID_MAX 64
/* The id RetroArch echoes: 1-16 of [A-Za-z0-9_-], within jawakad's own 12. */
#define JW_BOOT_RESUME_REQUEST_ID_MAX 17
#define JW_BOOT_RESUME_FINGERPRINT_MAX 128
#define JW_BOOT_RESUME_SYSTEM_MAX 64
#define JW_BOOT_RESUME_ROM_PATH_MAX 512
#define JW_BOOT_RESUME_CORE_ID_MAX 64
#define JW_BOOT_RESUME_CORE_FOLDER_MAX 256
#define JW_BOOT_RESUME_PROVIDER_MAX 128

typedef struct {
    char platform[JW_BOOT_RESUME_PLATFORM_MAX];
    char boot_id[JW_BOOT_RESUME_BOOT_ID_MAX];       /* the boot that armed it */
    char request_id[JW_BOOT_RESUME_REQUEST_ID_MAX]; /* the save that published it */
    /* Persistent "uuid:" or "fat:" identity of the card: source ids are
       positional and mount paths swap between boots. */
    char source_fingerprint[JW_BOOT_RESUME_FINGERPRINT_MAX];
    char system[JW_BOOT_RESUME_SYSTEM_MAX];
    char rom_path[JW_BOOT_RESUME_ROM_PATH_MAX];     /* relative to the source's ROM root */
    char core_id[JW_BOOT_RESUME_CORE_ID_MAX];
    char core_config_folder[JW_BOOT_RESUME_CORE_FOLDER_MAX];
    char provider[JW_BOOT_RESUME_PROVIDER_MAX];     /* content pak; "" for release cores */
    int slot;
    char state_path[JW_BOOT_RESUME_PATH_MAX];       /* relative to the source's States root */
    unsigned long long state_bytes;
    /* The save ran through the RAOfflineProxy, which forces casual play. The
       state then only loads in a casual session, so the boot launch has to
       go through the proxy too: Hardcore refuses every state load. */
    bool offline_proxy;
} jw_boot_resume_record;

typedef enum {
    JW_BOOT_RESUME_LOAD_ABSENT = 0,
    JW_BOOT_RESUME_LOAD_VALID,
    JW_BOOT_RESUME_LOAD_INVALID,  /* present but rejected; reason says why */
    JW_BOOT_RESUME_LOAD_ERROR,    /* present but could not be read */
} jw_boot_resume_load_result;

/* Why a well-formed record is not launched. PROCEED is the only launch. */
typedef enum {
    JW_BOOT_RESUME_PROCEED = 0,
    JW_BOOT_RESUME_DISCARD_STORAGE_RECOVERY,
    JW_BOOT_RESUME_DISCARD_SETTING_OFF,
    JW_BOOT_RESUME_DISCARD_BOOT_UNKNOWN,
    JW_BOOT_RESUME_DISCARD_SAME_BOOT,
    JW_BOOT_RESUME_DISCARD_BYPASS,
} jw_boot_resume_decision;

typedef struct {
    const char *current_boot_id;  /* NULL or "" when it could not be read */
    bool save_setting_on;         /* Save Before Power Off */
    bool resume_setting_on;       /* Resume Game on Boot */
    bool storage_recovery;        /* a repair/recovery owns this boot */
    bool bypass_held;             /* B held while Leaf started */
} jw_boot_resume_boot_facts;

/* Field rules shared by write and parse: everything present and bounded,
   platform as given, slot 99, relative paths that stay inside their root,
   state_path inside the core's folder. `reason` names the first failing
   field. */
bool jw_boot_resume_record_valid(const jw_boot_resume_record *record,
                                 const char *platform,
                                 char *reason, size_t reason_size);

/* <dir>/boot-resume.json: same-directory temporary, complete write and file
   fsync, rename, directory fsync. A failed write leaves no temporary behind. */
bool jw_boot_resume_write(const char *dir, const jw_boot_resume_record *record,
                          char *reason, size_t reason_size);

/* Strict: a regular file of bounded size, exactly the schema-1 keys with the
   right types, then jw_boot_resume_record_valid(). Never deletes anything. */
jw_boot_resume_load_result jw_boot_resume_load(const char *dir, const char *platform,
                                               jw_boot_resume_record *out,
                                               char *reason, size_t reason_size);

/* Unlink the record (absent is fine) and fsync the directory. True only when
   removal is confirmed: the caller must not act on the record otherwise. */
bool jw_boot_resume_consume(const char *dir, char *reason, size_t reason_size);

/* The boot-time discards, in plan order. Pure. */
jw_boot_resume_decision jw_boot_resume_decide(const jw_boot_resume_record *record,
                                              const jw_boot_resume_boot_facts *facts);
const char *jw_boot_resume_decision_name(jw_boot_resume_decision decision);

/* `abs` relative to `root` ("<root>/<rest>", rest a valid relative path).
   False when abs is not strictly inside root. */
bool jw_boot_resume_relative_to(const char *root, const char *abs,
                                char *out, size_t out_size);

/* The exact slot file this record may load: the state path the launcher
   builds for (states_root, core folder, rom) must be the recorded one, and it
   must be a regular file (not a link) of the recorded size. Writes the
   absolute path to out. `reason` is a short log token. */
bool jw_boot_resume_state_file_ok(const jw_boot_resume_record *record,
                                  const char *states_root, const char *rom_abs,
                                  char *out, size_t out_size,
                                  char *reason, size_t reason_size);

#endif
