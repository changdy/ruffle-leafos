#ifndef JW_DISCOVERY_DELETE_H
#define JW_DISCOVERY_DELETE_H

#include "internal/discovery/content.h"

typedef struct {
    char source_id[JW_STORAGE_SOURCE_ID_MAX];
    char rom_relpath[512];
    char path[JW_STORAGE_PATH_MAX];
    uint64_t size;
    bool missing;
} jw_delete_file;

/* The files behind one game entry, in removal order: payloads before the
   descriptors that name them, the launch file last. */
typedef struct {
    jw_delete_file *files;
    size_t file_count;
    uint64_t bytes;
    /* Progress can live inside a writable disk image that is deleted. */
    bool writable_image;
    /* Identifies the reviewed file set; a commit refuses a different one. */
    char digest[17];
} jw_delete_plan;

/* Invoked before every removal, including an already absent file. Returning -1
   stops immediately; the daemon can recheck storage and tests can inject I/O errors. */
typedef int (*jw_delete_guard)(void *context, const jw_delete_file *file,
                              char *error, size_t error_size);

typedef enum {
    JW_DELETE_PENDING = 0,
    JW_DELETE_REMOVED,
    /* Already gone at removal time; as safe to reconcile as a removed file. */
    JW_DELETE_ABSENT,
} jw_delete_outcome;

typedef struct {
    /* One per plan file. */
    jw_delete_outcome *outcomes;
    size_t removed_count;
    size_t absent_count;
    uint64_t bytes;
    size_t failed_index;
} jw_delete_result;

/* Delete is offered for any indexed launch file except where removing it breaks
   other games (arcade parent ROMs) or another app owns the content (ports,
   content paks, folder games launched by filename). */
bool jw_delete_supported(const jw_ra_system *system);
int jw_delete_plan_build(const jw_storage_source_list *sources, const jw_ra_system *system,
                         const char *source_id, const char *rom_relpath,
                         jw_delete_plan *out, char *error, size_t error_size);
int jw_delete_execute(const jw_delete_plan *plan, jw_delete_guard guard, void *context,
                      jw_delete_result *result, char *error, size_t error_size);
void jw_delete_plan_free(jw_delete_plan *plan);
void jw_delete_result_free(jw_delete_result *result);

#endif
