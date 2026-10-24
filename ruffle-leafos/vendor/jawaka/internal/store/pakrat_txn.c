#include "internal/store/pakrat_txn.h"

#include "internal/storage/sources.h"
#include "internal/store/pakrat_kind.h"
#include "internal/store/pakrat_recovery.h"
#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#ifdef JW_ENABLE_FAULT_INJECTION
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define JW_PAKRAT_MANIFEST_MAX (1024u * 1024u)



/* Test-only crash/pause points for the irreversible uninstall path. Dedicated
   smoke/test binaries define JW_ENABLE_FAULT_INJECTION; production jawakad and
   launcher builds compile this to an inert call. */
#ifdef JW_ENABLE_FAULT_INJECTION
static void jw__txn_fault_crash(const char *point) {
    const char *pause_at = getenv("JW_PAKRAT_PAUSE_AT");
    if (pause_at && pause_at[0] && strcmp(pause_at, point) == 0) {
        fprintf(stderr, "pakrat uninstall fault injection: paused at %s pid=%ld\n",
                point, (long)getpid());
        fflush(stderr);
        raise(SIGSTOP);
    }
    const char *fault_at = getenv("JW_PAKRAT_FAULT_AT");
    if (fault_at && fault_at[0] && strcmp(fault_at, point) == 0) {
        fprintf(stderr, "pakrat uninstall fault injection: crash at %s\n",
                point);
        _exit(42);
    }
}
#else
static void jw__txn_fault_crash(const char *point) {
    (void)point;
}
#endif

static void jw__txn_reason(char *out, size_t out_size, const char *value) {
    if (out && out_size > 0) {
        snprintf(out, out_size, "%s", value ? value : "unknown");
    }
}

static char *jw__txn_strdup(const char *value) {
    if (!value) {
        return NULL;
    }
    size_t size = strlen(value) + 1u;
    char *copy = malloc(size);
    if (copy) {
        memcpy(copy, value, size);
    }
    return copy;
}

static int jw__txn_copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0 || !value) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s", value);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

void jw_pakrat_txn_metadata_destroy(jw_pakrat_txn_metadata *metadata) {
    if (!metadata) {
        return;
    }
    free(metadata->state_root);
    for (int i = 0; i < metadata->revoke_count; i++) {
        free(metadata->revoke_on_uninstall[i]);
    }
    for (int i = 0; i < metadata->retained_count; i++) {
        free(metadata->retained_roots[i]);
    }
    memset(metadata, 0, sizeof(*metadata));
}

void jw_pakrat_pending_uninstall_destroy(jw_pakrat_pending_uninstall *pending) {
    if (!pending) {
        return;
    }
    jw_pakrat_txn_metadata_destroy(&pending->metadata);
    memset(pending, 0, sizeof(*pending));
}

void jw_pakrat_uninstall_info_destroy(jw_pakrat_uninstall_info *info) {
    if (!info) {
        return;
    }
    jw_pakrat_txn_metadata_destroy(&info->metadata);
    free(info->items);
    memset(info, 0, sizeof(*info));
}

static int jw__txn_metadata_copy(jw_pakrat_txn_metadata *out,
                                 const jw_pakrat_txn_metadata *in) {
    if (!out || !in) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    *out = *in;
    out->state_root = NULL;
    memset(out->revoke_on_uninstall, 0, sizeof(out->revoke_on_uninstall));
    memset(out->retained_roots, 0, sizeof(out->retained_roots));
    out->state_root = jw__txn_strdup(in->state_root ? in->state_root : "");
    if (!out->state_root) {
        goto failed;
    }
    for (int i = 0; i < in->revoke_count; i++) {
        out->revoke_on_uninstall[i] =
            jw__txn_strdup(in->revoke_on_uninstall[i]);
        if (!out->revoke_on_uninstall[i]) {
            goto failed;
        }
    }
    for (int i = 0; i < in->retained_count; i++) {
        out->retained_roots[i] = jw__txn_strdup(in->retained_roots[i]);
        if (!out->retained_roots[i]) {
            goto failed;
        }
    }
    return 0;
failed:
    jw_pakrat_txn_metadata_destroy(out);
    return -1;
}

static char *jw__txn_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    char *data = NULL;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long size = ftell(fp);
        if (size > 0 && (unsigned long)size <= JW_PAKRAT_MANIFEST_MAX &&
            fseek(fp, 0, SEEK_SET) == 0) {
            data = malloc((size_t)size + 1u);
            if (data) {
                size_t got = fread(data, 1, (size_t)size, fp);
                if (got == (size_t)size) {
                    data[got] = '\0';
                } else {
                    free(data);
                    data = NULL;
                }
            }
        }
    }
    fclose(fp);
    return data;
}

/* "<platform>/<Name>.pak" for an app, "Themes/<id>" for a theme: exactly one
   separator either way, and never the legacy "Apps/" spelling. */
bool jw_pakrat_txn_target_path_valid(const char *path) {
    if (!path || !path[0] || strncmp(path, "Apps/", 5) == 0 ||
        strlen(path) > JW_PAKRAT_TXN_TARGET_MAX ||
        !jw__pakrat_safe_rel_path(path)) {
        return false;
    }
    const char *slash = strchr(path, '/');
    return slash && slash != path && slash[1] &&
           strchr(slash + 1, '/') == NULL &&
           jw__pakrat_safe_name(slash + 1);
}

int jw_pakrat_txn_inspect_manifest(const char *pak_dir,
                                   const char *manifest_rel,
                                   const char *userdata_root,
                                   const char *expected_package_id,
                                   const char *install_path,
                                   jw_pakrat_txn_metadata *out,
                                   char *reason, size_t reason_size) {
    if (!pak_dir || !pak_dir[0] || !manifest_rel ||
        !jw__pakrat_safe_rel_path(manifest_rel) ||
        !expected_package_id || !expected_package_id[0] ||
        !jw__pakrat_safe_name(expected_package_id) ||
        strlen(expected_package_id) > JW_SVC_ID_MAX ||
        !jw_pakrat_txn_target_path_valid(install_path) || !out) {
        jw__txn_reason(reason, reason_size, "invalid-arguments");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char path[PATH_MAX];
    if (jw__pakrat_join2(path, sizeof(path), pak_dir, manifest_rel) != 0) {
        jw__txn_reason(reason, reason_size, "path-too-long");
        return -1;
    }
    char *text = jw__txn_read_file(path);
    if (!text) {
        jw__txn_reason(reason, reason_size, "manifest-unreadable");
        return -1;
    }
    cJSON *root = cJSON_ParseWithOpts(text, NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        free(text);
        jw__txn_reason(reason, reason_size, "invalid-json-shape");
        return -1;
    }
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *service = cJSON_GetObjectItemCaseSensitive(root, "service");
    if ((id && (!cJSON_IsString(id) || !id->valuestring ||
                strcmp(id->valuestring, expected_package_id) != 0)) ||
        (name && (!cJSON_IsString(name) || !name->valuestring))) {
        cJSON_Delete(root);
        free(text);
        jw__txn_reason(reason, reason_size, "package-id-mismatch");
        return -1;
    }
    if (jw__txn_copy(out->store_id, sizeof(out->store_id),
                     expected_package_id) != 0 ||
        jw__txn_copy(out->package_id, sizeof(out->package_id),
                     expected_package_id) != 0 ||
        jw__txn_copy(out->install_path, sizeof(out->install_path),
                     install_path) != 0 ||
        jw__txn_copy(out->display_name, sizeof(out->display_name),
                     name && name->valuestring ? name->valuestring
                                                : expected_package_id) != 0) {
        cJSON_Delete(root);
        free(text);
        jw_pakrat_txn_metadata_destroy(out);
        jw__txn_reason(reason, reason_size, "metadata-too-long");
        return -1;
    }
    out->state_root = jw__txn_strdup("");
    if (!out->state_root) {
        cJSON_Delete(root);
        free(text);
        jw_pakrat_txn_metadata_destroy(out);
        jw__txn_reason(reason, reason_size, "out-of-memory");
        return -1;
    }
    if (service) {
        jw_service_manifest manifest;
        char manifest_reason[JW_SVC_REASON_BUF] = {0};
        if (!jw_service_manifest_validate(text, pak_dir, userdata_root,
                                          &manifest, manifest_reason,
                                          sizeof(manifest_reason))) {
            cJSON_Delete(root);
            free(text);
            jw_pakrat_txn_metadata_destroy(out);
            jw__txn_reason(reason, reason_size,
                           manifest_reason[0] ? manifest_reason
                                              : "invalid-service-manifest");
            return -1;
        }
        out->has_service = true;
        snprintf(out->service_id, sizeof(out->service_id), "%s", manifest.id);
        free(out->state_root);
        out->state_root = jw__txn_strdup(
            manifest.state_root ? manifest.state_root : "");
        out->revoke_count = manifest.state_revoke_count;
        out->retained_count = manifest.state_retained_count;
        bool copied = out->state_root != NULL;
        for (int i = 0; copied && i < out->revoke_count; i++) {
            out->revoke_on_uninstall[i] =
                jw__txn_strdup(manifest.state_revoke_on_uninstall[i]);
            copied = out->revoke_on_uninstall[i] != NULL;
        }
        for (int i = 0; copied && i < out->retained_count; i++) {
            out->retained_roots[i] =
                jw__txn_strdup(manifest.state_retained_roots[i]);
            copied = out->retained_roots[i] != NULL;
        }
        jw_service_manifest_destroy(&manifest);
        if (!copied) {
            cJSON_Delete(root);
            free(text);
            jw_pakrat_txn_metadata_destroy(out);
            jw__txn_reason(reason, reason_size, "out-of-memory");
            return -1;
        }
    }
    cJSON_Delete(root);
    free(text);
    return 0;
}

int jw_pakrat_txn_theme_metadata(const char *store_id,
                                 const char *install_path,
                                 const char *display_name,
                                 jw_pakrat_txn_metadata *out) {
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (!store_id || !jw__pakrat_safe_name(store_id) ||
        strlen(store_id) > JW_SVC_ID_MAX ||
        jw_pakrat_install_path_kind(install_path) != JW_PAKRAT_KIND_THEME ||
        !jw_pakrat_txn_target_path_valid(install_path) ||
        jw__txn_copy(out->store_id, sizeof(out->store_id), store_id) != 0 ||
        jw__txn_copy(out->package_id, sizeof(out->package_id), store_id) != 0 ||
        jw__txn_copy(out->install_path, sizeof(out->install_path),
                     install_path) != 0 ||
        jw__txn_copy(out->display_name, sizeof(out->display_name),
                     display_name && display_name[0] ? display_name
                                                     : store_id) != 0 ||
        !(out->state_root = jw__txn_strdup(""))) {
        jw_pakrat_txn_metadata_destroy(out);
        return -1;
    }
    return 0;
}

static char *jw__txn_paths_json(char *const *paths, int count) {
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateString(paths[i]);
        if (!item || !cJSON_AddItemToArray(array, item)) {
            cJSON_Delete(item);
            cJSON_Delete(array);
            return NULL;
        }
    }
    char *json = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    return json;
}

static int jw__txn_bind_metadata(sqlite3_stmt *stmt, int offset,
                                 const jw_pakrat_txn_metadata *metadata,
                                 const char *revoke_json,
                                 const char *retained_json) {
    return sqlite3_bind_text(stmt, offset + 0, metadata->store_id, -1,
                             SQLITE_TRANSIENT) == SQLITE_OK &&
                   sqlite3_bind_text(stmt, offset + 1, metadata->install_path,
                                     -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                   sqlite3_bind_text(stmt, offset + 2, metadata->package_id,
                                     -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                   sqlite3_bind_text(stmt, offset + 3, metadata->display_name,
                                     -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                   sqlite3_bind_int(stmt, offset + 4,
                                    metadata->has_service ? 1 : 0) == SQLITE_OK &&
                   sqlite3_bind_text(stmt, offset + 5, metadata->service_id,
                                     -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                   sqlite3_bind_text(stmt, offset + 6,
                                     metadata->state_root
                                         ? metadata->state_root : "",
                                     -1, SQLITE_TRANSIENT) == SQLITE_OK &&
                   sqlite3_bind_text(stmt, offset + 7, revoke_json, -1,
                                     SQLITE_TRANSIENT) == SQLITE_OK &&
                   sqlite3_bind_text(stmt, offset + 8, retained_json, -1,
                                     SQLITE_TRANSIENT) == SQLITE_OK
               ? 0 : -1;
}

int jw_pakrat_txn_metadata_upsert_db(sqlite3 *db,
                                     const jw_pakrat_txn_metadata *metadata) {
    if (!db || !metadata || !metadata->store_id[0] ||
        !metadata->package_id[0] || !metadata->install_path[0]) {
        return -1;
    }
    char *revoke = jw__txn_paths_json(metadata->revoke_on_uninstall,
                                      metadata->revoke_count);
    char *retained = jw__txn_paths_json(metadata->retained_roots,
                                        metadata->retained_count);
    if (!revoke || !retained) {
        free(revoke);
        free(retained);
        return -1;
    }
    static const char *sql =
        "INSERT INTO pakrat_service_metadata "
        "(store_id,install_path,package_id,display_name,has_service,service_id,"
        "state_root,revoke_json,retained_json,validated_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,"
        "strftime('%Y-%m-%dT%H:%M:%SZ','now')) "
        "ON CONFLICT(store_id) DO UPDATE SET "
        "install_path=excluded.install_path,package_id=excluded.package_id,"
        "display_name=excluded.display_name,has_service=excluded.has_service,"
        "service_id=excluded.service_id,state_root=excluded.state_root,"
        "revoke_json=excluded.revoke_json,retained_json=excluded.retained_json,"
        "validated_at=excluded.validated_at;";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
                     jw__txn_bind_metadata(stmt, 1, metadata, revoke,
                                           retained) == 0 &&
                     sqlite3_step(stmt) == SQLITE_DONE
                 ? 0 : -1;
    sqlite3_finalize(stmt);
    free(revoke);
    free(retained);
    return rc;
}

static int jw__txn_parse_paths(const char *json, char **out, int *out_count) {
    if (!json || !out || !out_count) {
        return -1;
    }
    *out_count = 0;
    cJSON *array = cJSON_ParseWithOpts(json, NULL, true);
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(array);
        return -1;
    }
    int count = cJSON_GetArraySize(array);
    if (count < 0 || count > JW_SVC_MAX_STATE_LIST) {
        cJSON_Delete(array);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsString(item) || !item->valuestring ||
            !jw__pakrat_safe_rel_path(item->valuestring)) {
            cJSON_Delete(array);
            for (int j = 0; j < i; j++) {
                free(out[j]);
                out[j] = NULL;
            }
            return -1;
        }
        out[i] = jw__txn_strdup(item->valuestring);
        if (!out[i]) {
            cJSON_Delete(array);
            for (int j = 0; j < i; j++) {
                free(out[j]);
                out[j] = NULL;
            }
            return -1;
        }
    }
    cJSON_Delete(array);
    *out_count = count;
    return 0;
}

static int jw__txn_metadata_from_stmt(sqlite3_stmt *stmt, int offset,
                                      jw_pakrat_txn_metadata *out) {
    memset(out, 0, sizeof(*out));
    const char *store_id = (const char *)sqlite3_column_text(stmt, offset + 0);
    const char *install_path = (const char *)sqlite3_column_text(stmt, offset + 1);
    const char *package_id = (const char *)sqlite3_column_text(stmt, offset + 2);
    const char *display_name = (const char *)sqlite3_column_text(stmt, offset + 3);
    int has_service = sqlite3_column_int(stmt, offset + 4);
    const char *service_id = (const char *)sqlite3_column_text(stmt, offset + 5);
    const char *state_root = (const char *)sqlite3_column_text(stmt, offset + 6);
    const char *revoke = (const char *)sqlite3_column_text(stmt, offset + 7);
    const char *retained = (const char *)sqlite3_column_text(stmt, offset + 8);
    if (!store_id || !install_path || !package_id || !display_name ||
        (has_service != 0 && has_service != 1) || !service_id || !state_root ||
        !revoke || !retained ||
        strcmp(store_id, package_id) != 0 ||
        !jw__pakrat_safe_name(package_id) ||
        strlen(package_id) > JW_SVC_ID_MAX ||
        (has_service && !jw_service_id_is_reverse_dns(package_id)) ||
        !jw_pakrat_txn_target_path_valid(install_path) ||
        (has_service && strcmp(service_id, package_id) != 0) ||
        (!has_service && service_id[0]) ||
        (state_root[0] && !jw__pakrat_safe_name(state_root)) ||
        jw__txn_copy(out->store_id, sizeof(out->store_id), store_id) != 0 ||
        jw__txn_copy(out->install_path, sizeof(out->install_path),
                     install_path) != 0 ||
        jw__txn_copy(out->package_id, sizeof(out->package_id), package_id) != 0 ||
        jw__txn_copy(out->display_name, sizeof(out->display_name),
                     display_name) != 0 ||
        jw__txn_copy(out->service_id, sizeof(out->service_id), service_id) != 0) {
        jw_pakrat_txn_metadata_destroy(out);
        return -1;
    }
    out->has_service = has_service != 0;
    out->state_root = jw__txn_strdup(state_root);
    if (!out->state_root ||
        jw__txn_parse_paths(revoke, out->revoke_on_uninstall,
                            &out->revoke_count) != 0 ||
        jw__txn_parse_paths(retained, out->retained_roots,
                            &out->retained_count) != 0) {
        jw_pakrat_txn_metadata_destroy(out);
        return -1;
    }
    return 0;
}

int jw_pakrat_txn_metadata_get(const char *db_path, const char *store_id,
                               jw_pakrat_txn_metadata *out) {
    if (!db_path || !store_id || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }
    static const char *sql =
        "SELECT store_id,install_path,package_id,display_name,has_service,"
        "service_id,state_root,revoke_json,retained_json "
        "FROM pakrat_service_metadata WHERE store_id=?1;";
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 1, store_id, -1, SQLITE_TRANSIENT) == SQLITE_OK) {
        int step = sqlite3_step(stmt);
        rc = step == SQLITE_ROW ? jw__txn_metadata_from_stmt(stmt, 0, out)
                                : (step == SQLITE_DONE ? 1 : -1);
    }
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_pakrat_txn_pending_persist(const char *db_path,
                                  const char *source_id,
                                  const jw_pakrat_txn_metadata *metadata) {
    if (!db_path || !source_id || !source_id[0] ||
        strlen(source_id) > JW_PAKRAT_TXN_SOURCE_ID_MAX || !metadata) {
        return -1;
    }
    jw__txn_fault_crash("uninstall-before-intent");
    char *revoke = jw__txn_paths_json(metadata->revoke_on_uninstall,
                                      metadata->revoke_count);
    char *retained = jw__txn_paths_json(metadata->retained_roots,
                                        metadata->retained_count);
    sqlite3 *db = NULL;
    if (!revoke || !retained || jw_db_open(db_path, &db) != 0 ||
        jw_db_apply_schema(db) != 0 ||
        sqlite3_exec(db, "PRAGMA synchronous=FULL;BEGIN IMMEDIATE;", NULL,
                     NULL, NULL) != SQLITE_OK) {
        free(revoke);
        free(retained);
        jw_db_close(db);
        return -1;
    }
    static const char *sql =
        "INSERT INTO pakrat_pending_uninstalls "
        "(store_id,source_id,install_path,package_id,display_name,has_service,"
        "service_id,state_root,revoke_json,retained_json,created_at) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,"
        "strftime('%Y-%m-%dT%H:%M:%SZ','now')) "
        "ON CONFLICT(store_id) DO UPDATE SET store_id=excluded.store_id "
        "WHERE source_id=excluded.source_id "
        "AND install_path=excluded.install_path "
        "AND package_id=excluded.package_id "
        "AND display_name=excluded.display_name "
        "AND has_service=excluded.has_service "
        "AND service_id=excluded.service_id "
        "AND state_root=excluded.state_root "
        "AND revoke_json=excluded.revoke_json "
        "AND retained_json=excluded.retained_json;";
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 1, metadata->store_id, -1,
                          SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 2, source_id, -1,
                          SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 3, metadata->install_path, -1,
                          SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 4, metadata->package_id, -1,
                          SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 5, metadata->display_name, -1,
                          SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_int(stmt, 6, metadata->has_service ? 1 : 0) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 7, metadata->service_id, -1,
                          SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 8,
                          metadata->state_root ? metadata->state_root : "",
                          -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 9, revoke, -1, SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 10, retained, -1,
                          SQLITE_TRANSIENT) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) == 1) {
        jw__txn_fault_crash("uninstall-during-intent");
        if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) == SQLITE_OK) {
            rc = 0;
            jw__txn_fault_crash("uninstall-after-intent");
        }
    }
    if (rc != 0) {
        fprintf(stderr, "pakrat uninstall: step 'final-db' failed: %s\n",
                sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    }
    sqlite3_finalize(stmt);
    free(revoke);
    free(retained);
    jw_db_close(db);
    return rc;
}

static const char *const JW__PENDING_SELECT =
    "SELECT source_id,store_id,install_path,package_id,display_name,has_service,"
    "service_id,state_root,revoke_json,retained_json "
    "FROM pakrat_pending_uninstalls ";

static int jw__txn_pending_from_stmt(sqlite3_stmt *stmt,
                                     jw_pakrat_pending_uninstall *out) {
    memset(out, 0, sizeof(*out));
    const char *source_id = (const char *)sqlite3_column_text(stmt, 0);
    if (!source_id || !source_id[0] ||
        jw__txn_copy(out->source_id, sizeof(out->source_id), source_id) != 0 ||
        jw__txn_metadata_from_stmt(stmt, 1, &out->metadata) != 0) {
        jw_pakrat_pending_uninstall_destroy(out);
        return -1;
    }
    return 0;
}

int jw_pakrat_txn_pending_get(const char *db_path, const char *store_id,
                              jw_pakrat_pending_uninstall *out) {
    if (!db_path || !store_id || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }
    char sql[512];
    snprintf(sql, sizeof(sql), "%s WHERE store_id=?1;", JW__PENDING_SELECT);
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 1, store_id, -1, SQLITE_TRANSIENT) == SQLITE_OK) {
        int step = sqlite3_step(stmt);
        rc = step == SQLITE_ROW ? jw__txn_pending_from_stmt(stmt, out)
                                : (step == SQLITE_DONE ? 1 : -1);
    }
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_pakrat_txn_pending_list(const char *db_path,
                               jw_pakrat_pending_uninstall **out,
                               int *out_count) {
    if (!db_path || !out || !out_count) {
        return -1;
    }
    *out = NULL;
    *out_count = 0;
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }
    char sql[512];
    snprintf(sql, sizeof(sql), "%s ORDER BY store_id;", JW__PENDING_SELECT);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }
    int cap = 0;
    int step = SQLITE_ROW;
    int rc = 0;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (*out_count == cap) {
            int next = cap ? cap * 2 : 4;
            jw_pakrat_pending_uninstall *grown =
                realloc(*out, (size_t)next * sizeof(**out));
            if (!grown) {
                rc = -1;
                break;
            }
            *out = grown;
            cap = next;
        }
        if (jw__txn_pending_from_stmt(stmt, &(*out)[*out_count]) != 0) {
            rc = -1;
            break;
        }
        (*out_count)++;
    }
    if (step != SQLITE_DONE) {
        rc = -1;
    }
    sqlite3_finalize(stmt);
    jw_db_close(db);
    if (rc != 0) {
        for (int i = 0; i < *out_count; i++) {
            jw_pakrat_pending_uninstall_destroy(&(*out)[i]);
        }
        free(*out);
        *out = NULL;
        *out_count = 0;
    }
    return rc;
}

int jw_pakrat_txn_attach_control_db(sqlite3 *db, const char *state_dir) {
    if (!db || !state_dir || !state_dir[0]) {
        return -1;
    }
    char path[PATH_MAX];
    if (jw__pakrat_join2(path, sizeof(path), state_dir,
                         "services-control.db") != 0) {
        return -1;
    }
    char *sql = sqlite3_mprintf("ATTACH DATABASE %Q AS service_control;", path);
    if (!sql) {
        return -1;
    }
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
    sqlite3_free(sql);
    return rc;
}

int jw_pakrat_txn_clear_service_control_db(sqlite3 *db,
                                           const char *service_id) {
    if (!db || !service_id) {
        return -1;
    }
    if (!service_id[0]) {
        return 0;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(
            db,
            "DELETE FROM service_control.service_control_state "
            "WHERE service_id=?1;",
            -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, service_id, -1, SQLITE_TRANSIENT) !=
            SQLITE_OK) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

static int jw__txn_source_list(const jw_pakrat_context *ctx,
                               jw_storage_source_list *sources) {
    return ctx && sources &&
                   jw_storage_sources_resolve(ctx->sdcard_root, sources) == 0
               ? 0 : -1;
}

static const jw_storage_source *jw__txn_source(
    const jw_storage_source_list *sources, const char *source_id) {
    const jw_storage_source *source =
        jw_storage_sources_find_by_id(sources, source_id);
    return source && source->available ? source : NULL;
}

static int jw__txn_no_symlink_components(const char *root,
                                         const char *relative) {
    if (!root || !relative || !jw__pakrat_safe_rel_path(relative)) {
        return -1;
    }
    size_t root_len = strlen(root);
    size_t rel_len = strlen(relative);
    if (root_len > SIZE_MAX - rel_len - 2u) {
        return -1;
    }
    char *current = malloc(root_len + rel_len + 2u);
    if (!current) {
        return -1;
    }
    memcpy(current, root, root_len + 1u);
    size_t used = root_len;
    const char *cursor = relative;
    int rc = 0;
    while (*cursor) {
        const char *slash = strchr(cursor, '/');
        size_t len = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (used == 0 || current[used - 1] != '/') {
            current[used++] = '/';
        }
        memcpy(current + used, cursor, len);
        used += len;
        current[used] = '\0';
        struct stat st;
        if (lstat(current, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                rc = -1;
                break;
            }
        } else if (errno == ENOENT || errno == ENOTDIR) {
            break;
        } else {
            rc = -1;
            break;
        }
        if (!slash) {
            break;
        }
        cursor = slash + 1;
    }
    free(current);
    return rc;
}

static int jw__txn_state_path(char **out, const char *userdata_root,
                              const char *state_root, const char *relative) {
    if (!out || !userdata_root || !relative ||
        !jw__pakrat_safe_rel_path(relative) ||
        (state_root && state_root[0] && !jw__pakrat_safe_name(state_root))) {
        return -1;
    }
    const char *root = state_root ? state_root : "";
    size_t size = strlen(userdata_root) + strlen(root) + strlen(relative) + 3u;
    char *path = malloc(size);
    if (!path) {
        return -1;
    }
    int n = root[0] ? snprintf(path, size, "%s/%s/%s", userdata_root, root,
                               relative)
                    : snprintf(path, size, "%s/%s", userdata_root, relative);
    if (n < 0 || (size_t)n >= size) {
        free(path);
        return -1;
    }
    *out = path;
    return 0;
}

static int jw__txn_revoke(const jw_storage_source *primary,
                          const jw_pakrat_txn_metadata *metadata) {
    if (!primary || !metadata) {
        return -1;
    }
    for (int i = 0; i < metadata->revoke_count; i++) {
        char rel[PATH_MAX];
        const char *state_root = metadata->state_root ? metadata->state_root : "";
        int n = state_root[0]
                    ? snprintf(rel, sizeof(rel), "%s/%s", state_root,
                               metadata->revoke_on_uninstall[i])
                    : snprintf(rel, sizeof(rel), "%s",
                               metadata->revoke_on_uninstall[i]);
        char *path = NULL;
        if (n < 0 || n >= (int)sizeof(rel) ||
            jw__txn_no_symlink_components(primary->userdata_path, rel) != 0 ||
            jw__txn_state_path(&path, primary->userdata_path, state_root,
                               metadata->revoke_on_uninstall[i]) != 0 ||
            jw__pakrat_remove_tree(path) != 0) {
            free(path);
            return -1;
        }
        struct stat st;
        if (lstat(path, &st) == 0 || errno != ENOENT) {
            free(path);
            return -1;
        }
        free(path);
    }
    return 0;
}

static int jw__txn_package_paths(const jw_storage_source *source,
                                 const jw_pakrat_pending_uninstall *pending,
                                 char target[PATH_MAX], char stage[PATH_MAX],
                                 char rollback[PATH_MAX]) {
    return source && pending &&
                   jw__pakrat_target_path(source->root,
                                          pending->metadata.install_path,
                                          target, PATH_MAX) == 0 &&
                   jw__pakrat_target_sibling_path(
                       target, pending->metadata.store_id, "stage", stage,
                       PATH_MAX) == 0 &&
                   jw__pakrat_target_sibling_path(
                       target, pending->metadata.store_id, "rollback",
                       rollback, PATH_MAX) == 0
               ? 0 : -1;
}

/* Name the step that failed. "Confirmed uninstall could not be completed" on
   its own sends you reading the whole transaction; this makes the failure
   self-locating in a device log. */
static int jw__uninstall_step_failed(const char *step) {
    fprintf(stderr, "pakrat uninstall: step '%s' failed (errno=%d %s)\n",
            step, errno, errno ? strerror(errno) : "-");
    return -1;
}

int jw_pakrat_txn_complete_uninstall(const jw_pakrat_context *ctx,
                                     const jw_pakrat_pending_uninstall *pending) {
    if (!ctx || !pending || !pending->metadata.store_id[0]) {
        fprintf(stderr,
                "pakrat uninstall: step 'preconditions' failed "
                "(ctx=%d pending=%d store_id=%d)\n",
                ctx ? 1 : 0, pending ? 1 : 0,
                (pending && pending->metadata.store_id[0]) ? 1 : 0);
        return -1;
    }
    jw_storage_source_list sources;
    errno = 0;
    if (jw__txn_source_list(ctx, &sources) != 0) {
        return jw__uninstall_step_failed("source-list");
    }
    const jw_storage_source *primary = jw_storage_sources_primary(&sources);
    const jw_storage_source *apps_source =
        jw__txn_source(&sources, pending->source_id);
    if (!primary || !primary->available || !apps_source) {
        return 1; /* owning source absent: defer without mutation */
    }
    jw__txn_fault_crash("uninstall-before-revoke");
    errno = 0;
    if (jw__txn_revoke(primary, &pending->metadata) != 0) {
        return jw__uninstall_step_failed("revoke");
    }
    jw__txn_fault_crash("uninstall-after-revoke");
    char target[PATH_MAX], stage[PATH_MAX], rollback[PATH_MAX];
    errno = 0;
    if (jw__txn_package_paths(apps_source, pending, target, stage, rollback) != 0) {
        return jw__uninstall_step_failed("package-paths");
    }
    jw__txn_fault_crash("uninstall-before-package-remove");
    errno = 0;
    if (jw__pakrat_remove_tree(target) != 0) {
        return jw__uninstall_step_failed("remove-target");
    }
    errno = 0;
    if (jw__pakrat_remove_tree(stage) != 0) {
        return jw__uninstall_step_failed("remove-stage");
    }
    errno = 0;
    if (jw__pakrat_remove_tree(rollback) != 0) {
        return jw__uninstall_step_failed("remove-rollback");
    }
    jw__pakrat_clear_origin_marker(target, pending->metadata.store_id);
    jw__txn_fault_crash("uninstall-after-package-remove");
    errno = 0;
    if (jw__pakrat_path_exists(target) || jw__pakrat_path_exists(stage) ||
        jw__pakrat_path_exists(rollback)) {
        return jw__uninstall_step_failed("post-remove-verify");
    }
    jw__txn_fault_crash("uninstall-before-syncfs");
    errno = 0;
    if (jw__pakrat_sync_filesystem(primary->root) != 0) {
        return jw__uninstall_step_failed("syncfs-primary");
    }
    errno = 0;
    if (apps_source != primary &&
        jw__pakrat_sync_filesystem(apps_source->root) != 0) {
        return jw__uninstall_step_failed("syncfs-apps-source");
    }
    jw__txn_fault_crash("uninstall-after-syncfs");

    jw__txn_fault_crash("uninstall-before-final-db");
    sqlite3 *db = NULL;
    errno = 0;
    if (jw_db_open(ctx->db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        (pending->metadata.service_id[0] &&
         jw_pakrat_txn_attach_control_db(db, ctx->state_dir) != 0)) {
        fprintf(stderr, "pakrat uninstall: step 'final-db-open' failed: %s\n",
                db ? sqlite3_errmsg(db) : "open failed");
        jw_db_close(db);
        return -1;
    }
    /* This is the commit point of an irreversible operation: the package is
       already gone from disk, and only this transaction records that fact.
       Failing it because a library scan happens to hold the write lock leaves
       the install row behind and tells the user the uninstall failed when the
       package is unrecoverably removed.

       jw_db_open's 2s default is tuned for interactive writes (a favourite, a
       recent) where waiting longer would feel like a hang. A scan holds the
       lock for 6-9 seconds on a full library, so 2s loses that race whenever
       the two overlap -- which a large package makes likely, because removing
       it and syncing the filesystem takes long enough for a scan to start.
       Wait it out instead. */
    sqlite3_busy_timeout(db, JW_PAKRAT_COMMIT_BUSY_TIMEOUT_MS);
    if (sqlite3_exec(db, "PRAGMA synchronous=FULL;BEGIN IMMEDIATE;", NULL,
                     NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "pakrat uninstall: step 'final-db-begin' failed: %s\n",
                sqlite3_errmsg(db));
        jw_db_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = 0;
    if (pending->metadata.service_id[0] &&
        jw_pakrat_txn_clear_service_control_db(
            db, pending->metadata.service_id) != 0) {
        rc = -1;
    }
    static const char *delete_install =
        "DELETE FROM pakrat_installs WHERE store_id=?1;";
    if (rc == 0 &&
        (sqlite3_prepare_v2(db, delete_install, -1, &stmt, NULL) != SQLITE_OK ||
         sqlite3_bind_text(stmt, 1, pending->metadata.store_id, -1,
                           SQLITE_TRANSIENT) != SQLITE_OK ||
         sqlite3_step(stmt) != SQLITE_DONE)) {
        rc = -1;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    static const char *delete_pending =
        "DELETE FROM pakrat_pending_uninstalls WHERE store_id=?1;";
    /* Deliberately NOT "exactly one row was deleted".

       jawakad runs TXN-1 recovery for pending uninstalls, so the daemon and a
       CLI uninstall can be discharging the same intent concurrently. Removing
       a large package takes seconds, which is ample time for the daemon to get
       there first -- and then this DELETE legitimately affects zero rows.

       Treating that as failure told the user "Confirmed uninstall could not be
       completed" about an uninstall that had fully succeeded, just not by this
       process. What matters is the END STATE, which is verified below, not
       which process got there. Only a real SQL error is a failure. */
    if (rc == 0 &&
        (sqlite3_prepare_v2(db, delete_pending, -1, &stmt, NULL) != SQLITE_OK ||
         sqlite3_bind_text(stmt, 1, pending->metadata.store_id, -1,
                           SQLITE_TRANSIENT) != SQLITE_OK ||
         sqlite3_step(stmt) != SQLITE_DONE)) {
        rc = -1;
    }
    if (rc != 0) {
        fprintf(stderr, "pakrat uninstall: step 'final-db-deletes' failed: %s\n",
                sqlite3_errmsg(db));
    }
    sqlite3_finalize(stmt);
    stmt = NULL;

    /* A selection naming a folder that no longer exists would come back to
       life the day a folder of that name reappears. Clear it in the same
       commit that forgets the theme. */
    int selection_cleared = 0;
    if (rc == 0 && jw_pakrat_install_path_kind(pending->metadata.install_path) ==
                       JW_PAKRAT_KIND_THEME) {
        static const char *clear_selection =
            "UPDATE settings SET value='' WHERE key='user_theme' AND value=?1;";
        const char *folder = pending->metadata.install_path +
                             sizeof(JW_PAKRAT_THEME_PATH_PREFIX) - 1;
        if (sqlite3_prepare_v2(db, clear_selection, -1, &stmt, NULL) != SQLITE_OK ||
            sqlite3_bind_text(stmt, 1, folder, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(stmt) != SQLITE_DONE) {
            fprintf(stderr, "pakrat uninstall: step 'final-db-selection' failed: %s\n",
                    sqlite3_errmsg(db));
            rc = -1;
        } else {
            selection_cleared = sqlite3_changes(db) > 0;
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    /* The uninstall is complete when neither record survives, whoever removed
       them. Verifying the state is what makes concurrent completion safe
       instead of merely tolerated. */
    if (rc == 0) {
        static const char *verify =
            "SELECT (SELECT COUNT(*) FROM pakrat_installs WHERE store_id=?1) + "
            "(SELECT COUNT(*) FROM pakrat_pending_uninstalls WHERE store_id=?1);";
        long long remaining = -1;
        if (sqlite3_prepare_v2(db, verify, -1, &stmt, NULL) != SQLITE_OK ||
            sqlite3_bind_text(stmt, 1, pending->metadata.store_id, -1,
                              SQLITE_TRANSIENT) != SQLITE_OK ||
            sqlite3_step(stmt) != SQLITE_ROW) {
            rc = -1;
        } else {
            remaining = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
        if (rc == 0 && remaining != 0) {
            fprintf(stderr,
                    "pakrat uninstall: step 'final-db-verify' failed: %lld "
                    "record(s) still reference %s\n",
                    remaining, pending->metadata.store_id);
            rc = -1;
        }
    }
    jw__txn_fault_crash("uninstall-during-final-db");
    if (rc == 0 && sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "pakrat uninstall: step 'final-db-commit' failed: %s\n",
                sqlite3_errmsg(db));
        rc = -1;
    }
    if (rc != 0) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    }
    if (rc == 0) {
        jw__txn_fault_crash("uninstall-after-final-db");
        if (ctx->outcome && selection_cleared) {
            ctx->outcome->theme_selection_cleared = 1;
        }
    }
    jw_db_close(db);
    return rc;
}

static int jw__txn_tree_size(const char *path, unsigned long long *out) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) {
            *out = 0;
            return 0;
        }
        return -1;
    }
    if (S_ISLNK(st.st_mode)) {
        return -1;
    }
    unsigned long long total = (unsigned long long)st.st_size;
    if (!S_ISDIR(st.st_mode)) {
        *out = total;
        return 0;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    int rc = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        unsigned long long child_size = 0;
        if (jw__pakrat_join2(child, sizeof(child), path, entry->d_name) != 0 ||
            jw__txn_tree_size(child, &child_size) != 0 ||
            total > UINT64_MAX - child_size) {
            rc = -1;
            break;
        }
        total += child_size;
    }
    closedir(dir);
    if (rc == 0) {
        *out = total;
    }
    return rc;
}

int jw_pakrat_txn_inventory_retained(const jw_pakrat_context *ctx,
                                     const jw_pakrat_txn_metadata *metadata,
                                     jw_pakrat_uninstall_info *out) {
    if (!ctx || !metadata || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (jw__txn_metadata_copy(&out->metadata, metadata) != 0) {
        return -1;
    }
    jw_storage_source_list sources;
    if (jw__txn_source_list(ctx, &sources) != 0 ||
        (metadata->retained_count > 0 &&
         sources.count > INT32_MAX / metadata->retained_count)) {
        jw_pakrat_uninstall_info_destroy(out);
        return -1;
    }
    int count = sources.count * metadata->retained_count;
    if (count > 0) {
        out->items = calloc((size_t)count, sizeof(*out->items));
        if (!out->items) {
            jw_pakrat_uninstall_info_destroy(out);
            return -1;
        }
    }
    for (int s = 0; s < sources.count; s++) {
        const jw_storage_source *source = &sources.sources[s];
        for (int r = 0; r < metadata->retained_count; r++) {
            jw_pakrat_retained_item *item = &out->items[out->item_count++];
            snprintf(item->source_id, sizeof(item->source_id), "%s", source->id);
            item->source_present = source->available;
            if (snprintf(item->root, sizeof(item->root), "%s/%s",
                         source->userdata_path,
                         metadata->retained_roots[r]) >=
                (int)sizeof(item->root)) {
                item->size_known = false;
                continue;
            }
            if (!source->available) {
                continue;
            }
            item->size_known =
                jw__txn_no_symlink_components(
                    source->userdata_path, metadata->retained_roots[r]) == 0 &&
                jw__txn_tree_size(item->root, &item->size_bytes) == 0;
        }
    }
    return 0;
}

int jw_pakrat_txn_remove_retained(const jw_pakrat_context *ctx,
                                  const jw_pakrat_txn_metadata *metadata) {
    if (!ctx || !metadata) {
        return -1;
    }
    jw_storage_source_list sources;
    if (jw__txn_source_list(ctx, &sources) != 0) {
        return -1;
    }
    for (int s = 0; s < sources.count; s++) {
        const jw_storage_source *source = &sources.sources[s];
        if (!source->available) {
            continue;
        }
        for (int r = 0; r < metadata->retained_count; r++) {
            char path[PATH_MAX];
            if (snprintf(path, sizeof(path), "%s/%s", source->userdata_path,
                         metadata->retained_roots[r]) >= (int)sizeof(path) ||
                jw__txn_no_symlink_components(
                    source->userdata_path, metadata->retained_roots[r]) != 0 ||
                jw__pakrat_remove_tree(path) != 0) {
                return -1;
            }
        }
        if (metadata->retained_count > 0 &&
            jw__pakrat_sync_filesystem(source->root) != 0) {
            return -1;
        }
    }
    return 0;
}

static int jw__txn_lock_path(char out[PATH_MAX], const char *runtime_dir,
                             const char *package_id, bool create_dir) {
    if (!runtime_dir || !runtime_dir[0] ||
        !jw_service_id_is_reverse_dns(package_id)) {
        return -1;
    }
    char dir[PATH_MAX];
    if (jw__pakrat_join2(dir, sizeof(dir), runtime_dir,
                         "pakrat-mutations") != 0 ||
        (create_dir && jw__pakrat_mkdir_p(dir, 0700) != 0)) {
        return -1;
    }
    int n = snprintf(out, PATH_MAX, "%s/%s.lock", dir, package_id);
    return n >= 0 && n < PATH_MAX ? 0 : -1;
}

static int jw__txn_lock_record(char *out, size_t out_size,
                               const char *operation_id,
                               const char *package_id,
                               const char *target_path) {
    if (!operation_id || !operation_id[0] || !package_id || !target_path ||
        strlen(operation_id) > JW_PAKRAT_TXN_OPERATION_MAX ||
        strchr(operation_id, '\n') || strchr(package_id, '\n') ||
        strchr(target_path, '\n')) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s\n%s\n%s\n", operation_id,
                     package_id, target_path);
    return n >= 0 && (size_t)n < out_size ? n : -1;
}

int jw_pakrat_mutation_lock_acquire(const char *runtime_dir,
                                    const char *operation_id,
                                    const char *package_id,
                                    const char *target_path,
                                    jw_pakrat_mutation_lock *out,
                                    char *reason, size_t reason_size) {
    if (!out || !jw_service_id_is_reverse_dns(package_id) ||
        !jw_pakrat_txn_target_path_valid(target_path)) {
        jw__txn_reason(reason, reason_size, "invalid-arguments");
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    char path[PATH_MAX];
    char record[1024];
    int record_size = jw__txn_lock_record(record, sizeof(record), operation_id,
                                          package_id, target_path);
    if (record_size < 0 ||
        jw__txn_lock_path(path, runtime_dir, package_id, true) != 0) {
        jw__txn_reason(reason, reason_size, "path-failed");
        return -1;
    }
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        jw__txn_reason(reason, reason_size, "lock-open-failed");
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        jw__txn_reason(reason, reason_size, "mutation-in-progress");
        return -1;
    }
    ssize_t wrote = 0;
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0 ||
        (wrote = write(fd, record, (size_t)record_size)) != record_size ||
        fsync(fd) != 0) {
        flock(fd, LOCK_UN);
        close(fd);
        jw__txn_reason(reason, reason_size, "lock-write-failed");
        return -1;
    }
    out->fd = fd;
    snprintf(out->operation_id, sizeof(out->operation_id), "%s", operation_id);
    snprintf(out->package_id, sizeof(out->package_id), "%s", package_id);
    snprintf(out->target_path, sizeof(out->target_path), "%s", target_path);
    return 0;
}

void jw_pakrat_mutation_lock_release(jw_pakrat_mutation_lock *lock) {
    if (!lock) {
        return;
    }
    if (lock->fd >= 0) {
        (void)flock(lock->fd, LOCK_UN);
        close(lock->fd);
    }
    memset(lock, 0, sizeof(*lock));
    lock->fd = -1;
}

bool jw_pakrat_mutation_lock_is_held(const char *runtime_dir,
                                     const char *operation_id,
                                     const char *package_id,
                                     const char *target_path) {
    char path[PATH_MAX];
    char expected[1024];
    int expected_size = jw__txn_lock_record(
        expected, sizeof(expected), operation_id, package_id, target_path);
    if (expected_size < 0 ||
        jw__txn_lock_path(path, runtime_dir, package_id, false) != 0) {
        return false;
    }
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char actual[1024];
    ssize_t got = pread(fd, actual, sizeof(actual), 0);
    bool matches = got == expected_size &&
                   memcmp(actual, expected, (size_t)expected_size) == 0;
    bool held = false;
    if (matches) {
        if (flock(fd, LOCK_EX | LOCK_NB) != 0 &&
            (errno == EWOULDBLOCK || errno == EAGAIN)) {
            held = true;
        } else {
            (void)flock(fd, LOCK_UN);
        }
    }
    close(fd);
    return held;
}
