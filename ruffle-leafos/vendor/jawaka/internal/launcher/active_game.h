#ifndef JW_LAUNCHER_ACTIVE_GAME_H
#define JW_LAUNCHER_ACTIVE_GAME_H

#include <stdbool.h>
#include <stddef.h>
#include <limits.h>

/* PATH_MAX has to be settled here, not left to each includer. jw_active_game is
   passed across translation units (the daemon builds the record, active_game.c
   validates and serializes it), and a struct whose array sizes depend on
   whether <limits.h> happened to be included first has two layouts in one
   process: on macOS, where PATH_MAX is 1024, the validator then reads the wrong
   bytes and rejects every launch with "invalid-arguments". */
#include <limits.h>

#ifndef PATH_MAX
#define JW_ACTIVE_GAME_PATH_MAX 4096
#else
#define JW_ACTIVE_GAME_PATH_MAX PATH_MAX
#endif

#define JW_ACTIVE_GAME_LAUNCH_ID_MAX 95
#define JW_ACTIVE_GAME_SOURCE_ID_MAX 31
#define JW_ACTIVE_GAME_FILENAME "active-game.json"

typedef struct {
    bool active;
    bool recovered;
    bool uncertain;
    char launch_id[JW_ACTIVE_GAME_LAUNCH_ID_MAX + 1];
    char source_id[JW_ACTIVE_GAME_SOURCE_ID_MAX + 1];
    char saves_path[JW_ACTIVE_GAME_PATH_MAX];
    char states_path[JW_ACTIVE_GAME_PATH_MAX];
} jw_active_game;

typedef enum {
    JW_ACTIVE_GAME_LOAD_ERROR = -1,
    JW_ACTIVE_GAME_LOAD_ABSENT = 0,
    JW_ACTIVE_GAME_LOAD_VALID = 1,
    /* A record exists but cannot be trusted. Callers must fail safe: suppress
     * game-sensitive service starts and refuse to overwrite it with a new
     * launch until an authoritative recovery path clears it. */
    JW_ACTIVE_GAME_LOAD_UNCERTAIN = 2,
} jw_active_game_load_result;

/* Writes <runtime_dir>/active-game.json with the LIFE-1 atomic commit:
 * same-directory temporary, complete write + file fsync, rename, then parent
 * directory fsync. The caller must provide a fully resolved source and paths. */
bool jw_active_game_persist(const char *runtime_dir,
                            const jw_active_game *record,
                            char *reason, size_t reason_size);

/* Loads the runtime record. A malformed/truncated/non-regular record returns
 * UNCERTAIN with out->active/recovered/uncertain all true. I/O failures return
 * ERROR and are likewise not evidence that no writer exists. */
jw_active_game_load_result jw_active_game_load(
    const char *runtime_dir, jw_active_game *out,
    char *reason, size_t reason_size);

/* Durably clears the record (unlink + parent-directory fsync). ENOENT is a
 * successful idempotent clear. */
bool jw_active_game_clear(const char *runtime_dir,
                          char *reason, size_t reason_size);

/* Generates an opaque process-unique id suitable for one active launch. */
bool jw_active_game_generate_id(char *out, size_t out_size);

#endif
