#ifndef JW_IPC_DELETE_CLIENT_H
#define JW_IPC_DELETE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct jw_ipc_delete_session jw_ipc_delete_session;

typedef enum {
    /* The launcher's own state until the preview reply; jawakad never sends it. */
    JW_IPC_DELETE_PREPARING,
    JW_IPC_DELETE_READY,
    JW_IPC_DELETE_COMMITTING,
    JW_IPC_DELETE_DONE,
    JW_IPC_DELETE_ERROR
} jw_ipc_delete_phase;

typedef struct {
    char source_id[32];
    char rom_relpath[512];
    uint64_t size;
    bool missing;
    bool removed;
} jw_ipc_delete_file;

typedef struct {
    jw_ipc_delete_phase phase;
    char name[512];
    char source_id[32];
    /* READY only: identifies the previewed file set for the delete request. */
    char digest[17];
    char error[512];
    char readonly_source[32];
    bool writable_progress;
    uint64_t bytes;
    size_t removed_count;
    size_t absent_count;
    jw_ipc_delete_file *files;
    size_t files_count;
} jw_ipc_delete_status;

/* One request on its own connection, answered with the game's files and a
   digest of them. jawakad keeps nothing between the preview and the delete.
   A daemon refusal is phase ERROR with return 0; -1 is a transport or
   protocol failure, with status.error populated. Initialize status to zero;
   each call replaces it. */
int jw_ipc_delete_preview(const char *socket_path, const char *source_id,
                          const char *rom_relpath, jw_ipc_delete_status *status);
/* Asks jawakad to delete the previewed files on a retained connection. It
   rebuilds the file list and refuses one whose digest differs. COMMITTING
   means removal started; poll for DONE or ERROR. Nothing is retried: after
   -1 the result is unknown. */
int jw_ipc_delete_begin(const char *socket_path, const char *source_id,
                        const char *rom_relpath, const char *digest,
                        jw_ipc_delete_session **session, jw_ipc_delete_status *status);
int jw_ipc_delete_poll(jw_ipc_delete_session *session, jw_ipc_delete_status *status);
void jw_ipc_delete_close(jw_ipc_delete_session *session);
void jw_ipc_delete_status_free(jw_ipc_delete_status *status);

#endif
