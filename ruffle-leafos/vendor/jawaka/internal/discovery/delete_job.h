#ifndef JW_DISCOVERY_DELETE_JOB_H
#define JW_DISCOVERY_DELETE_JOB_H

#include <stdbool.h>
#include <stddef.h>
#include "internal/ipc/delete_client.h"

typedef struct jw_delete_job jw_delete_job;
typedef struct cJSON cJSON;

/* The preview reply for one game: its files and their digest, or an error.
   Runs on the calling thread; it reads only that game's descriptors. */
cJSON *jw_delete_preview(const char *db_path, const char *sdcard_root,
                         const char *source_id, const char *rom_relpath);
/* Starts removal on a worker. It rebuilds the file list, refuses one whose
   digest differs from the preview, then removes the files and reconciles the
   library. Once started it runs to the end, even if its connection closes. */
jw_delete_job *jw_delete_job_start(const char *db_path, const char *sdcard_root,
                                  const char *source_id, const char *rom_relpath,
                                  const char *digest);
/* These functions run on the daemon main thread. */
bool jw_delete_job_busy(jw_delete_job *job);
bool jw_delete_job_changed(jw_delete_job *job); /* consumes the refresh flag */
cJSON *jw_delete_job_status(jw_delete_job *job);
void jw_delete_job_destroy(jw_delete_job *job);

#endif
