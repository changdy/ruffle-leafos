#include "internal/ipc/delete_client.h"
#include "internal/ipc/ipc.h"
#include "cJSON.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *ready_json =
    "{\"type\":\"rom-delete-status\",\"phase\":1,\"digest\":\"0123456789abcdef\","
    "\"name\":\"Synthetic Game\",\"source_id\":\"primary\",\"bytes\":10,"
    "\"removed_count\":0,\"absent_count\":0,\"writable_progress\":false,"
    "\"files\":[{\"source_id\":\"primary\",\"rom_relpath\":\"GBA/Game.gba\",\"size\":10,"
    "\"missing\":false,\"removed\":false}]}";

static void reply(jw_ipc_client *peer, const cJSON *object) {
    char *json = cJSON_PrintUnformatted(object);
    assert(json && jw_ipc_client_send(peer, json, strlen(json)) == 0);
    cJSON_free(json);
}

static cJSON *receive(jw_ipc_client *peer, const char *type) {
    char *json = NULL;
    size_t length;
    assert(jw_ipc_client_recv(peer, &json, &length) == 0);
    cJSON *object = cJSON_ParseWithLength(json, length);
    free(json);
    const cJSON *actual = cJSON_GetObjectItemCaseSensitive(object, "type");
    assert(cJSON_IsString(actual) && !strcmp(actual->valuestring, type));
    return object;
}

/* Only the selected identity, and for a delete the reviewed digest, crosses IPC. */
static void receive_identity(jw_ipc_client *peer, const char *type, const char *digest) {
    cJSON *request = receive(peer, type);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "source_id")->valuestring, "primary"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "rom_relpath")->valuestring, "GBA/Game.gba"));
    if (digest)
        assert(!strcmp(cJSON_GetObjectItemCaseSensitive(request, "digest")->valuestring, digest));
    assert(cJSON_GetArraySize(request) == (digest ? 4 : 3));
    cJSON_Delete(request);
}

static void expect_closed(jw_ipc_client *peer) {
    char *unexpected = NULL;
    size_t length;
    assert(jw_ipc_client_recv(peer, &unexpected, &length) != 0);
    free(unexpected);
}

static void wait_child(pid_t child) {
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void check_preview(const char *socket, const cJSON *message, bool accepted) {
    jw_ipc_server *server = NULL;
    assert(jw_ipc_server_listen(socket, &server) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        jw_ipc_client *peer = NULL;
        assert(jw_ipc_server_accept(server, &peer, 1000) == 0);
        receive_identity(peer, "rom-delete-preview", NULL);
        reply(peer, message);
        expect_closed(peer); /* the preview keeps no connection */
        jw_ipc_client_close(peer);
        _exit(0);
    }
    jw_ipc_delete_status status = {0};
    int rc = jw_ipc_delete_preview(socket, "primary", "GBA/Game.gba", &status);
    assert((rc == 0) == accepted);
    if (!accepted) assert(status.phase == JW_IPC_DELETE_ERROR && status.error[0] && !status.digest[0] && !status.files);
    jw_ipc_delete_status_free(&status);
    wait_child(child);
    jw_ipc_server_close(server);
}

/* outcome: 0 finishes, 1 loses the result after removal starts, 2 is refused. */
static void lifecycle(const char *socket, int outcome) {
    jw_ipc_server *server = NULL;
    assert(jw_ipc_server_listen(socket, &server) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        jw_ipc_client *peer = NULL;
        assert(jw_ipc_server_accept(server, &peer, 1000) == 0);
        receive_identity(peer, "rom-delete", "0123456789abcdef");
        cJSON *message = cJSON_Parse(outcome == 2
            ? "{\"type\":\"rom-delete-status\",\"phase\":4,\"source_id\":\"primary\","
              "\"error\":\"Your game's files changed. Open Delete Game again.\"}"
            : "{\"type\":\"rom-delete-status\",\"phase\":2,\"source_id\":\"primary\"}");
        reply(peer, message);
        cJSON_Delete(message);
        if (outcome == 0) {
            cJSON_Delete(receive(peer, "rom-delete-status"));
            message = cJSON_Parse(ready_json);
            cJSON_ReplaceItemInObjectCaseSensitive(message, "phase", cJSON_CreateNumber(3));
            cJSON_DeleteItemFromObjectCaseSensitive(message, "digest");
            cJSON_ReplaceItemInObjectCaseSensitive(message, "removed_count", cJSON_CreateNumber(1));
            cJSON *file = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(message, "files"), 0);
            cJSON_ReplaceItemInObjectCaseSensitive(file, "removed", cJSON_CreateBool(true));
            reply(peer, message);
            cJSON_Delete(message);
        }
        jw_ipc_client_close(peer);
        _exit(0);
    }
    jw_ipc_delete_session *session = NULL;
    jw_ipc_delete_status status = {0};
    assert(jw_ipc_delete_begin(socket, "primary", "GBA/Game.gba", "0123456789abcdef", &session, &status) == 0);
    if (outcome == 2) {
        assert(status.phase == JW_IPC_DELETE_ERROR && strstr(status.error, "changed"));
    } else {
        assert(status.phase == JW_IPC_DELETE_COMMITTING && !status.files);
        int rc = jw_ipc_delete_poll(session, &status);
        if (outcome == 1) {
            assert(rc == -1 && status.phase == JW_IPC_DELETE_ERROR && strstr(status.error, "Couldn't confirm"));
            assert(jw_ipc_delete_poll(session, &status) == -1); /* a failed session sends nothing more */
        } else {
            assert(rc == 0 && status.phase == JW_IPC_DELETE_DONE);
            assert(status.removed_count == 1 && status.files_count == 1 && status.files[0].removed);
        }
    }
    jw_ipc_delete_status_free(&status);
    jw_ipc_delete_close(session);
    wait_child(child);
    jw_ipc_server_close(server);
}

int main(void) {
    /* The launcher inherits jawakad's ignored SIGPIPE; a poll after the
       daemon closed must fail with EPIPE, not kill the process. */
    signal(SIGPIPE, SIG_IGN);
    char root[] = "/tmp/jw-delete-client-XXXXXX";
    assert(mkdtemp(root));
    char socket[128];
    snprintf(socket, sizeof(socket), "%s/ipc", root);
    cJSON *message = cJSON_Parse(ready_json);
    assert(message);
    check_preview(socket, message, true);
    cJSON_Delete(message);
    const struct { const char *field; const char *value; bool file; } invalid[] = {
        {"bytes", "-1", false}, {"bytes", "1.5", false},
        {"bytes", "9007199254740992", false}, {"digest", "\"\"", false},
        {"removed_count", "1", false}, {"writable_progress", "\"false\"", false},
        {"files", "[false]", false}, {"files", "[]", false}, {"phase", "2", false},
        {"rom_relpath", "\"\"", true}, {"missing", "0", true}, {"removed", "true", true},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        message = cJSON_Parse(ready_json);
        cJSON *object = invalid[i].file
            ? cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(message, "files"), 0) : message;
        cJSON_ReplaceItemInObjectCaseSensitive(object, invalid[i].field, cJSON_Parse(invalid[i].value));
        check_preview(socket, message, false);
        cJSON_Delete(message);
    }
    message = cJSON_Parse(ready_json);
    cJSON_DeleteItemFromObjectCaseSensitive(message, "files");
    check_preview(socket, message, false);
    cJSON_Delete(message);
    message = cJSON_Parse("{\"type\":\"rom-delete-status\",\"phase\":4,\"error\":\"Card is read-only.\",\"readonly_source\":\"secondary_sd\"}");
    check_preview(socket, message, true);
    cJSON_Delete(message);
    lifecycle(socket, 0);
    lifecycle(socket, 1);
    lifecycle(socket, 2);
    rmdir(root);
    puts("delete-client-test: one-shot preview, digest-carrying delete, lost-result and malformed reply handling passed");
    return 0;
}
