#ifndef JW_DB_RELOCATION_H
#define JW_DB_RELOCATION_H

#include <stddef.h>
#include <sqlite3.h>

#define JW_RELOCATION_OPERATION_ID_MAX 96
#define JW_RELOCATION_STATE_MAX 16
#define JW_RELOCATION_MAX_ITEMS 256

typedef struct {
    char source_id[32];
    char rom_relpath[512];
    char image_root_kind[16];
    char image_relpath[512];
} jw_relocation_identity;

typedef struct {
    int game_id;
    jw_relocation_identity old_identity;
    jw_relocation_identity new_identity;
    char old_rom_path[512];
    char old_image_path[512];
    char new_rom_path[512];
    char new_image_path[512];
    char old_source_snapshot[1024];
    char new_source_snapshot[1024];
} jw_relocation_item;

typedef struct {
    char operation_id[JW_RELOCATION_OPERATION_ID_MAX];
    char state[JW_RELOCATION_STATE_MAX];
    int expected_generation;
    int mapping_generation;
    int scan_ticket_generation;
    int item_count;
} jw_relocation_status;

enum {
    JW_RELOCATION_OK = 0,
    JW_RELOCATION_ERROR = -1,
    JW_RELOCATION_NOT_FOUND = -2,
    JW_RELOCATION_CONFLICT = -3,
    JW_RELOCATION_STALE = -4,
    JW_RELOCATION_BUSY = -5,
    JW_RELOCATION_BAD_STATE = -6
};

int jw_db_relocation_prepare(sqlite3 *db, const char *operation_id,
                             int expected_generation,
                             const char *request_fingerprint,
                             jw_relocation_item *items, int item_count,
                             jw_relocation_status *out,
                             char *error, size_t error_size);
int jw_db_relocation_status(sqlite3 *db, const char *operation_id,
                            jw_relocation_status *out);
int jw_db_relocation_load_items(sqlite3 *db, const char *operation_id,
                                jw_relocation_item *out, int max_items,
                                int *out_count);
int jw_db_relocation_refresh_items(sqlite3 *db, const char *operation_id,
                                   const jw_relocation_item *items,
                                   int item_count);
int jw_db_relocation_commit(sqlite3 *db, const char *operation_id,
                            jw_relocation_status *out,
                            char *error, size_t error_size);
int jw_db_relocation_revert(sqlite3 *db, const char *operation_id,
                            jw_relocation_status *out,
                            char *error, size_t error_size);
int jw_db_relocation_abort(sqlite3 *db, const char *operation_id,
                           jw_relocation_status *out);
int jw_db_relocation_finish(sqlite3 *db, const char *operation_id,
                            jw_relocation_status *out);
int jw_db_relocation_note_scan(sqlite3 *db, int generation);

int jw_db_relocation_game_reserved(sqlite3 *db, int game_id);
int jw_db_relocation_key_reserved(sqlite3 *db, const char *source_id,
                                  const char *rom_relpath);

#endif
