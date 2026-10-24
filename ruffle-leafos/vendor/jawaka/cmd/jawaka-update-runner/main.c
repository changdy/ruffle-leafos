#include "cJSON.h"
#include "internal/update/sha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char request_path[PATH_MAX];
    char result_path[PATH_MAX];
    char state_dir[PATH_MAX];
    char sdcard_root[PATH_MAX];
    char runtime_path[PATH_MAX];
    char logs_path[PATH_MAX];
    char marker_path[PATH_MAX];
    char platform[64];
    char release_id[128];
    char version[128];
    char artifact_kind[64];
    char artifact_name[256];
    char artifact_path[PATH_MAX];
    char artifact_sha256[65];
    long long artifact_size;
    char handoff_type[64];
    char handoff_completion[64];
    char trigger_file[256];
} jw_update_request;

static const char *const kStockSdRoots[] = {
    "/mnt/sdcard",
    "/media/sdcard1",
};

static void jw__copy_string(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", value ? value : "");
}

static bool jw__join_path(char *out, size_t out_size,
                          const char *dir, const char *name) {
    if (!out || out_size == 0 || !dir || !dir[0] || !name || !name[0]) {
        return false;
    }
    int needed = snprintf(out, out_size, "%s/%s", dir, name);
    return needed >= 0 && (size_t)needed < out_size;
}

static bool jw__join3(char *out, size_t out_size,
                      const char *a, const char *b, const char *c) {
    char tmp[PATH_MAX];
    return jw__join_path(tmp, sizeof(tmp), a, b) &&
           jw__join_path(out, out_size, tmp, c);
}

static int jw__mkdir_if_needed(const char *path, mode_t mode) {
    if (mkdir(path, mode) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int jw__mkdir_p(const char *path, mode_t mode) {
    if (!path || !path[0]) {
        return -1;
    }

    char buf[PATH_MAX];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int)sizeof(buf)) {
        return -1;
    }

    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/') {
        buf[--len] = '\0';
    }

    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (buf[0] && jw__mkdir_if_needed(buf, mode) != 0) {
            return -1;
        }
        *p = '/';
    }

    return jw__mkdir_if_needed(buf, mode);
}

static bool jw__path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static bool jw__is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool jw__is_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* True only if `path` is the mount target of a real mounted filesystem (listed
   in /proc/mounts), not merely an existing directory. The MLP1 has two SD slots;
   the second slot's mountpoint (/media/sdcard1) exists as an empty directory on
   the read-only rootfs even when no card is inserted. Arming a stock trigger
   there is impossible (rootfs is ro) and must not be attempted — so candidate SD
   roots are gated on being actually mounted, not just present. */
static bool jw__path_is_mounted(const char *path) {
    if (!path || !path[0]) return false;
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        char *mp = strchr(line, ' ');          /* skip the device field */
        if (!mp) continue;
        mp++;
        char *end = strchr(mp, ' ');           /* mountpoint ends at next space */
        if (!end) continue;
        *end = '\0';
        if (strcmp(mp, path) == 0) {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

static const char *jw__json_string(const cJSON *obj, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static long long jw__json_ll(const cJSON *obj, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsNumber(item) ? (long long)item->valuedouble : 0;
}

static bool jw__simple_name_safe(const char *name) {
    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }
    if (strstr(name, "..")) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (*p < 0x20 || *p == '/' || *p == '\\') {
            return false;
        }
    }
    return true;
}

static bool jw__managed_app_safe(const char *path) {
    if (!path || !path[0] || path[0] == '/' || strstr(path, "..") ||
        strchr(path, '\\')) {
        return false;
    }

    const char *slash = strchr(path, '/');
    if (!slash || slash == path || slash[1] == '\0' || strchr(slash + 1, '/')) {
        return false;
    }

    char platform[64];
    char app[256];
    size_t platform_len = (size_t)(slash - path);
    if (platform_len >= sizeof(platform) || strlen(slash + 1) >= sizeof(app)) {
        return false;
    }
    memcpy(platform, path, platform_len);
    platform[platform_len] = '\0';
    snprintf(app, sizeof(app), "%s", slash + 1);
    return jw__simple_name_safe(platform) && jw__simple_name_safe(app);
}

static char *jw__read_text_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    char *buf = (char *)malloc((size_t)len + 1u);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, fp);
    int read_error = ferror(fp);
    fclose(fp);
    if (read_error || got != (size_t)len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

static void jw__log_msg(const jw_update_request *req, const char *fmt, ...) {
    if (!req || !req->logs_path[0]) {
        return;
    }
    if (jw__mkdir_p(req->logs_path, 0755) != 0) {
        return;
    }

    char path[PATH_MAX];
    if (!jw__join_path(path, sizeof(path), req->logs_path,
                       "jawaka-update-runner.log")) {
        return;
    }

    FILE *fp = fopen(path, "a");
    if (!fp) {
        return;
    }

    char stamp[64];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = localtime_r(&now, &tm_buf);
    if (tm) {
        strftime(stamp, sizeof(stamp), "%F %T", tm);
    } else {
        snprintf(stamp, sizeof(stamp), "%s", "unknown");
    }

    fprintf(fp, "[%s] ", stamp);
    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt ? fmt : "", args);
    va_end(args);
    fputc('\n', fp);
    fclose(fp);
}

static int jw__write_result(const jw_update_request *req,
                            const char *state,
                            const char *message) {
    if (!req || !req->result_path[0]) {
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "product", "leaf");
    cJSON_AddStringToObject(root, "operation", "install");
    cJSON_AddStringToObject(root, "state", state ? state : "error");
    cJSON_AddStringToObject(root, "message", message ? message : "");
    cJSON_AddStringToObject(root, "platform", req->platform);
    cJSON_AddStringToObject(root, "release_id", req->release_id);
    cJSON_AddNumberToObject(root, "finished_at", (double)time(NULL));

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return -1;
    }

    char tmp[PATH_MAX];
    int needed = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld",
                          req->result_path, (long)getpid());
    if (needed < 0 || needed >= (int)sizeof(tmp)) {
        cJSON_free(text);
        return -1;
    }

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        cJSON_free(text);
        return -1;
    }
    int ok = fputs(text, fp) >= 0 && fputc('\n', fp) != EOF;
    if (fclose(fp) != 0) {
        ok = 0;
    }
    cJSON_free(text);
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, req->result_path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int jw__remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        if (!jw__join_path(child, sizeof(child), path, entry->d_name)) {
            closedir(dir);
            return -1;
        }
        if (jw__remove_tree(child) != 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return rmdir(path);
}

static int jw__copy_file(const char *src, const char *dst, mode_t mode) {
    int in_fd = open(src, O_RDONLY);
    if (in_fd < 0) {
        return -1;
    }

    char tmp[PATH_MAX];
    int needed = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", dst, (long)getpid());
    if (needed < 0 || needed >= (int)sizeof(tmp)) {
        close(in_fd);
        return -1;
    }

    int out_fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
    if (out_fd < 0) {
        close(in_fd);
        return -1;
    }

    char buf[64 * 1024];
    int ok = 1;
    for (;;) {
        ssize_t got = read(in_fd, buf, sizeof(buf));
        if (got == 0) {
            break;
        }
        if (got < 0) {
            ok = 0;
            break;
        }
        char *p = buf;
        ssize_t remaining = got;
        while (remaining > 0) {
            ssize_t wrote = write(out_fd, p, (size_t)remaining);
            if (wrote < 0) {
                ok = 0;
                break;
            }
            p += wrote;
            remaining -= wrote;
        }
        if (!ok) {
            break;
        }
    }

    if (fsync(out_fd) != 0) {
        ok = 0;
    }
    if (close(out_fd) != 0) {
        ok = 0;
    }
    close(in_fd);

    if (!ok) {
        unlink(tmp);
        return -1;
    }
    if (chmod(tmp, mode ? mode : 0644) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, dst) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int jw__copy_tree_merge(const char *src, const char *dst,
                               const char *skip_root_name,
                               bool is_root);

static int jw__copy_entry(const char *src, const char *dst,
                          const char *skip_root_name,
                          bool is_root) {
    struct stat st;
    if (lstat(src, &st) != 0) {
        return -1;
    }
    if (S_ISLNK(st.st_mode)) {
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        return jw__copy_tree_merge(src, dst, skip_root_name, false);
    }
    if (!S_ISREG(st.st_mode)) {
        return -1;
    }

    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", dst);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        if (jw__mkdir_p(parent, 0755) != 0) {
            return -1;
        }
    }

    (void)is_root;
    return jw__copy_file(src, dst, st.st_mode & 0777);
}

static int jw__copy_tree_merge(const char *src, const char *dst,
                               const char *skip_root_name,
                               bool is_root) {
    struct stat st;
    if (lstat(src, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) {
        return -1;
    }
    if (jw__mkdir_p(dst, st.st_mode & 0777 ? st.st_mode & 0777 : 0755) != 0) {
        return -1;
    }

    DIR *dir = opendir(src);
    if (!dir) {
        return -1;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (is_root && skip_root_name && strcmp(entry->d_name, skip_root_name) == 0) {
            continue;
        }

        char child_src[PATH_MAX];
        char child_dst[PATH_MAX];
        if (!jw__join_path(child_src, sizeof(child_src), src, entry->d_name) ||
            !jw__join_path(child_dst, sizeof(child_dst), dst, entry->d_name)) {
            closedir(dir);
            return -1;
        }
        if (jw__copy_entry(child_src, child_dst, skip_root_name, false) != 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return 0;
}

static int jw__replace_tree(const char *src, const char *dst) {
    if (!jw__is_dir(src)) {
        return -1;
    }
    if (jw__remove_tree(dst) != 0) {
        return -1;
    }
    return jw__copy_tree_merge(src, dst, NULL, true);
}

static int jw__replace_file(const char *src, const char *dst) {
    struct stat st;
    if (lstat(src, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) {
        return -1;
    }
    if (unlink(dst) != 0 && errno != ENOENT) {
        return -1;
    }
    return jw__copy_file(src, dst, st.st_mode & 0777);
}

static bool jw__same_path(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static bool jw__stock_root_seen(const char *const *roots, size_t count,
                                const char *root) {
    for (size_t i = 0; i < count; i++) {
        if (jw__same_path(roots[i], root)) {
            return true;
        }
    }
    return false;
}

static int jw__copy_stock_helper(const char *stage_dir,
                                 const char *root,
                                 const char *name,
                                 mode_t fallback_mode) {
    char src[PATH_MAX];
    char dst[PATH_MAX];
    if (!jw__join_path(src, sizeof(src), stage_dir, name) ||
        !jw__join_path(dst, sizeof(dst), root, name) ||
        !jw__is_file(src)) {
        return -1;
    }
    if (jw__replace_file(src, dst) != 0) {
        return -1;
    }
    chmod(dst, fallback_mode);
    return 0;
}

static int jw__spawn_wait(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

static int jw__run_unzip(const char *zip_path, const char *stage_dir) {
    char *const unzip_argv[] = {
        "unzip",
        "-oq",
        (char *)zip_path,
        "-d",
        (char *)stage_dir,
        NULL,
    };
    if (jw__spawn_wait(unzip_argv) == 0) {
        return 0;
    }

    char *const busybox_argv[] = {
        "busybox",
        "unzip",
        "-oq",
        (char *)zip_path,
        "-d",
        (char *)stage_dir,
        NULL,
    };
    return jw__spawn_wait(busybox_argv);
}

static void jw__run_sync(void) {
    sync();
}

static int jw__parse_request(const char *path, jw_update_request *req) {
    memset(req, 0, sizeof(*req));
    jw__copy_string(req->request_path, sizeof(req->request_path), path);
    jw__copy_string(req->trigger_file, sizeof(req->trigger_file), "loong_upgrade");

    char *text = jw__read_text_file(path);
    if (!text) {
        return -1;
    }
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return -1;
    }

    const cJSON *artifact = cJSON_GetObjectItemCaseSensitive(root, "artifact");
    const cJSON *handoff = cJSON_GetObjectItemCaseSensitive(root, "handoff");
    jw__copy_string(req->result_path, sizeof(req->result_path),
                    jw__json_string(root, "result_path"));
    jw__copy_string(req->state_dir, sizeof(req->state_dir),
                    jw__json_string(root, "state_dir"));
    jw__copy_string(req->sdcard_root, sizeof(req->sdcard_root),
                    jw__json_string(root, "sdcard_root"));
    jw__copy_string(req->runtime_path, sizeof(req->runtime_path),
                    jw__json_string(root, "runtime_path"));
    jw__copy_string(req->logs_path, sizeof(req->logs_path),
                    jw__json_string(root, "logs_path"));
    jw__copy_string(req->marker_path, sizeof(req->marker_path),
                    jw__json_string(root, "marker_path"));
    jw__copy_string(req->platform, sizeof(req->platform),
                    jw__json_string(root, "platform"));
    jw__copy_string(req->release_id, sizeof(req->release_id),
                    jw__json_string(root, "release_id"));
    jw__copy_string(req->version, sizeof(req->version),
                    jw__json_string(root, "version"));
    jw__copy_string(req->artifact_kind, sizeof(req->artifact_kind),
                    jw__json_string(artifact, "kind"));
    jw__copy_string(req->artifact_name, sizeof(req->artifact_name),
                    jw__json_string(artifact, "name"));
    jw__copy_string(req->artifact_path, sizeof(req->artifact_path),
                    jw__json_string(artifact, "path"));
    jw__copy_string(req->artifact_sha256, sizeof(req->artifact_sha256),
                    jw__json_string(artifact, "sha256"));
    req->artifact_size = jw__json_ll(artifact, "size");
    jw__copy_string(req->handoff_type, sizeof(req->handoff_type),
                    jw__json_string(handoff, "type"));
    jw__copy_string(req->handoff_completion, sizeof(req->handoff_completion),
                    jw__json_string(handoff, "completion"));
    if (jw__json_string(handoff, "trigger_file")) {
        jw__copy_string(req->trigger_file, sizeof(req->trigger_file),
                        jw__json_string(handoff, "trigger_file"));
    }

    cJSON_Delete(root);

    return req->result_path[0] &&
           req->state_dir[0] &&
           req->sdcard_root[0] &&
           req->platform[0] &&
           jw__simple_name_safe(req->release_id) &&
           strcmp(req->artifact_kind, "sd_root_zip") == 0 &&
           req->artifact_path[0] &&
           req->artifact_sha256[0] &&
           jw__simple_name_safe(req->trigger_file) ? 0 : -1;
}

static int jw__maybe_reexec_tmpfs(const jw_update_request *req,
                                  const char *argv0) {
    if (getenv("JAWAKA_UPDATE_RUNNER_REEXECED")) {
        return 0;
    }
    if (!req || !req->runtime_path[0] || !argv0 || !argv0[0]) {
        return 0;
    }
    if (jw__mkdir_p(req->runtime_path, 0700) != 0) {
        return -1;
    }

    char tmp_path[PATH_MAX];
    int needed = snprintf(tmp_path, sizeof(tmp_path),
                          "%s/jawaka-update-runner.%ld",
                          req->runtime_path, (long)getpid());
    if (needed < 0 || needed >= (int)sizeof(tmp_path)) {
        return -1;
    }
    if (jw__copy_file(argv0, tmp_path, 0755) != 0) {
        return -1;
    }

    setenv("JAWAKA_UPDATE_RUNNER_REEXECED", "1", 1);
    execl(tmp_path, tmp_path, "--request", req->request_path, (char *)NULL);
    return -1;
}

static int jw__verify_artifact(const jw_update_request *req) {
    jw__write_result(req, "installing", "Verifying downloaded update");
    struct stat st;
    if (stat(req->artifact_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return -1;
    }
    if (req->artifact_size > 0 && st.st_size != req->artifact_size) {
        return -1;
    }

    char actual[65];
    char error[160];
    if (jw_sha256_file_hex(req->artifact_path, actual, error, sizeof(error)) != 0) {
        return -1;
    }
    return strcasecmp(actual, req->artifact_sha256) == 0 ? 0 : -1;
}

static int jw__extract_artifact(const jw_update_request *req, char *stage_dir,
                                size_t stage_dir_size) {
    jw__write_result(req, "installing", "Extracting update files");
    char update_dir[PATH_MAX];
    if (!jw__join_path(update_dir, sizeof(update_dir), req->state_dir, "update") ||
        !jw__join3(stage_dir, stage_dir_size, update_dir, "stage", req->release_id)) {
        return -1;
    }

    if (jw__remove_tree(stage_dir) != 0 ||
        jw__mkdir_p(stage_dir, 0755) != 0) {
        return -1;
    }

    return jw__run_unzip(req->artifact_path, stage_dir);
}

static int jw__release_paths(const jw_update_request *req,
                             const char *root,
                             char *release_root,
                             size_t release_root_size,
                             char *release_platform,
                             size_t release_platform_size) {
    char releases_root[PATH_MAX];
    if (!jw__join3(releases_root, sizeof(releases_root),
                   root, ".system/leaf/releases", req->release_id) ||
        !jw__join3(release_platform, release_platform_size,
                   releases_root, "platforms", req->platform)) {
        return -1;
    }
    jw__copy_string(release_root, release_root_size, releases_root);
    return 0;
}

static int jw__stock_loong_prepare(const jw_update_request *req,
                                   const char *stage_dir) {
    char installer[PATH_MAX];
    char probe[PATH_MAX];
    char release_root[PATH_MAX];
    char release_platform[PATH_MAX];

    if (!jw__join_path(installer, sizeof(installer),
                       stage_dir, "umrk-launcher-install.sh") ||
        !jw__join_path(probe, sizeof(probe),
                       stage_dir, "launcher_probe.bin") ||
        jw__release_paths(req, stage_dir, release_root, sizeof(release_root),
                          release_platform, sizeof(release_platform)) != 0) {
        return -1;
    }

    if (!jw__is_file(probe) ||
        !jw__is_file(installer) ||
        !jw__is_dir(release_platform)) {
        return -1;
    }

    jw__log_msg(req, "copying staged release %s to SD root", req->release_id);
    jw__write_result(req, "installing", "Copying update to SD card");
    if (jw__copy_tree_merge(stage_dir, req->sdcard_root,
                            req->trigger_file, true) != 0) {
        return -1;
    }

    char root_installer[PATH_MAX];
    if (jw__join_path(root_installer, sizeof(root_installer),
                      req->sdcard_root, "umrk-launcher-install.sh")) {
        chmod(root_installer, 0755);
    }
    char root_recovery[PATH_MAX];
    if (jw__join_path(root_recovery, sizeof(root_recovery),
                      req->sdcard_root, "umrk-launcher-recovery.sh")) {
        chmod(root_recovery, 0755);
    }

    const char *roots[3];
    size_t root_count = 0;
    if (req->sdcard_root[0]) {
        roots[root_count++] = req->sdcard_root;
    }
    for (size_t i = 0; i < sizeof(kStockSdRoots) / sizeof(kStockSdRoots[0]); i++) {
        if (!jw__stock_root_seen(roots, root_count, kStockSdRoots[i]) &&
            jw__path_is_mounted(kStockSdRoots[i])) {
            roots[root_count++] = kStockSdRoots[i];
        }
    }

    jw__log_msg(req, "arming stock trigger %s on %zu SD root(s)",
                req->trigger_file, root_count);
    jw__write_result(req, "installing", "Preparing restart step");
    for (size_t i = 0; i < root_count; i++) {
        const char *root = roots[i];
        /* The primary root (where the payload was just copied) is mandatory; a
           genuine second SD is best-effort so an unwritable/odd secondary can
           never abort an otherwise-good install. */
        bool primary = jw__same_path(root, req->sdcard_root);
        if (!primary) {
            if (jw__copy_stock_helper(stage_dir, root,
                                      "umrk-launcher-install.sh", 0755) != 0 ||
                jw__copy_stock_helper(stage_dir, root,
                                      "launcher_probe.bin", 0644) != 0 ||
                jw__copy_stock_helper(stage_dir, root,
                                      req->trigger_file, 0755) != 0) {
                jw__log_msg(req, "secondary SD root %s not armable; skipping", root);
            }
            continue;
        }
        if (jw__copy_stock_helper(stage_dir, root, req->trigger_file, 0755) != 0) {
            return -1;
        }
    }

    jw__write_result(req, "installing", "Syncing update files");
    jw__run_sync();
    unlink(req->artifact_path);
    return 0;
}

static int jw__write_release_json(const jw_update_request *req) {
    char tmp[PATH_MAX];
    char out[PATH_MAX];
    if (!jw__join_path(out, sizeof(out), req->state_dir, "release.json")) {
        return -1;
    }
    int needed = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", out, (long)getpid());
    if (needed < 0 || needed >= (int)sizeof(tmp)) {
        return -1;
    }
    if (jw__mkdir_p(req->state_dir, 0755) != 0) {
        return -1;
    }

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        return -1;
    }

    char stamp[64];
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *tm = gmtime_r(&now, &tm_buf);
    if (tm) {
        strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", tm);
    } else {
        snprintf(stamp, sizeof(stamp), "%s", "unknown");
    }

    int ok = fprintf(fp,
        "{\n"
        "  \"schema\": 1,\n"
        "  \"product\": \"leaf\",\n"
        "  \"platform\": \"%s\",\n"
        "  \"version\": \"%s\",\n"
        "  \"release_id\": \"%s\",\n"
        "  \"installed_at\": \"%s\",\n"
        "  \"source\": \"jawaka-update-runner\"\n"
        "}\n",
        req->platform,
        req->version[0] ? req->version : req->release_id,
        req->release_id,
        stamp) > 0;
    if (fclose(fp) != 0) {
        ok = 0;
    }
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    return rename(tmp, out);
}

static int jw__generic_promote(const jw_update_request *req,
                               const char *stage_dir) {
    char release_root[PATH_MAX];
    char release_platform[PATH_MAX];
    char active_platform[PATH_MAX];
    char release_launcher[PATH_MAX];
    char active_launcher[PATH_MAX];

    if (jw__release_paths(req, stage_dir, release_root, sizeof(release_root),
                          release_platform, sizeof(release_platform)) != 0 ||
        !jw__join3(active_platform, sizeof(active_platform),
                   req->sdcard_root, ".system/leaf/platforms", req->platform) ||
        !jw__join_path(release_launcher, sizeof(release_launcher),
                       release_platform, "launcher") ||
        !jw__join_path(active_launcher, sizeof(active_launcher),
                       active_platform, "launcher")) {
        return -1;
    }
    if (!jw__is_dir(release_platform) || !jw__is_dir(release_launcher)) {
        return -1;
    }

    if (req->marker_path[0]) {
        unlink(req->marker_path);
    }

    jw__log_msg(req, "promoting launcher payload release=%s", req->release_id);
    jw__write_result(req, "installing", "Promoting launcher update");
    if (jw__replace_tree(release_launcher, active_launcher) != 0) {
        return -1;
    }

    const char *managed_dirs[] = {
        "bin", "cores", "info", "defaults", "platform.d",
        "autoconfig", "boot-animation",
    };
    for (size_t i = 0; i < sizeof(managed_dirs) / sizeof(managed_dirs[0]); i++) {
        char src[PATH_MAX];
        char dst[PATH_MAX];
        if (!jw__join_path(src, sizeof(src), release_platform, managed_dirs[i]) ||
            !jw__join_path(dst, sizeof(dst), active_platform, managed_dirs[i])) {
            return -1;
        }
        if (jw__path_exists(src) && jw__replace_tree(src, dst) != 0) {
            return -1;
        }
    }

    const char *managed_files[] = { "manifest.json" };
    for (size_t i = 0; i < sizeof(managed_files) / sizeof(managed_files[0]); i++) {
        char src[PATH_MAX];
        char dst[PATH_MAX];
        if (!jw__join_path(src, sizeof(src), release_platform, managed_files[i]) ||
            !jw__join_path(dst, sizeof(dst), active_platform, managed_files[i])) {
            return -1;
        }
        if (jw__path_exists(src) && jw__replace_file(src, dst) != 0) {
            return -1;
        }
    }

    char managed_apps_path[PATH_MAX];
    if (jw__join_path(managed_apps_path, sizeof(managed_apps_path),
                      release_root, "managed-apps.txt") &&
        jw__is_file(managed_apps_path)) {
        FILE *fp = fopen(managed_apps_path, "r");
        if (!fp) {
            return -1;
        }
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!line[0] || line[0] == '#') {
                continue;
            }
            if (!jw__managed_app_safe(line)) {
                fclose(fp);
                return -1;
            }
            char src[PATH_MAX];
            char dst[PATH_MAX];
            if (!jw__join3(src, sizeof(src), release_root, "Apps", line) ||
                !jw__join3(dst, sizeof(dst), req->sdcard_root, "Apps", line)) {
                fclose(fp);
                return -1;
            }
            jw__log_msg(req, "promoting managed app %s", line);
            jw__write_result(req, "installing", "Promoting managed apps");
            if (jw__replace_tree(src, dst) != 0) {
                fclose(fp);
                return -1;
            }
        }
        fclose(fp);
    }

    jw__write_result(req, "installing", "Writing installed release marker");
    if (jw__write_release_json(req) != 0) {
        return -1;
    }
    if (req->marker_path[0]) {
        char parent[PATH_MAX];
        snprintf(parent, sizeof(parent), "%s", req->marker_path);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            if (jw__mkdir_p(parent, 0755) != 0) {
                return -1;
            }
        }
        FILE *fp = fopen(req->marker_path, "w");
        if (!fp) {
            return -1;
        }
        fclose(fp);
    }

    jw__write_result(req, "installing", "Syncing update files");
    jw__run_sync();
    unlink(req->artifact_path);
    return 0;
}

static int jw__run_install(jw_update_request *req) {
    if (jw__verify_artifact(req) != 0) {
        jw__log_msg(req, "artifact verification failed: %s", req->artifact_path);
        jw__write_result(req, "error", "Downloaded artifact failed final verification");
        return 1;
    }

    char stage_dir[PATH_MAX];
    jw__log_msg(req, "extracting %s", req->artifact_path);
    if (jw__extract_artifact(req, stage_dir, sizeof(stage_dir)) != 0) {
        jw__write_result(req, "error", "Cannot extract update artifact");
        return 1;
    }

    int rc = 1;
    if (strcmp(req->handoff_type, "stock_loong_upgrade") == 0) {
        if (jw__stock_loong_prepare(req, stage_dir) == 0) {
            jw__write_result(req, "armed",
                             "Restart to finish installing");
            rc = 0;
        } else {
            jw__write_result(req, "error", "Cannot prepare update for restart");
        }
    } else if (strcmp(req->handoff_type, "jawaka_c_runner") == 0 ||
               strcmp(req->handoff_type, "generic_runner") == 0 ||
               strcmp(req->handoff_type, "direct_runner") == 0) {
        if (jw__generic_promote(req, stage_dir) == 0) {
            jw__write_result(req, "installed", "Update installed");
            rc = 0;
        } else {
            jw__write_result(req, "error", "Cannot promote update payload");
        }
    } else {
        jw__write_result(req, "error", "Unsupported update install method");
    }

    if (rc == 0) {
        jw__remove_tree(stage_dir);
    }
    return rc;
}

static void jw__usage(FILE *fp) {
    fprintf(fp, "Usage: jawaka-update-runner --request <install-request.json>\n");
}

int main(int argc, char **argv) {
    const char *request_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--request") == 0 && i + 1 < argc) {
            request_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            jw__usage(stdout);
            return 0;
        } else {
            jw__usage(stderr);
            return 2;
        }
    }

    if (!request_path || !request_path[0]) {
        jw__usage(stderr);
        return 2;
    }

    jw_update_request req;
    if (jw__parse_request(request_path, &req) != 0) {
        fprintf(stderr, "invalid update install request: %s\n", request_path);
        return 1;
    }

    if (jw__maybe_reexec_tmpfs(&req, argv[0]) != 0) {
        jw__write_result(&req, "error", "Cannot copy update runner to runtime storage");
        return 1;
    }

    jw__log_msg(&req, "update runner starting handoff=%s release=%s",
                req.handoff_type, req.release_id);
    int rc = jw__run_install(&req);
    jw__log_msg(&req, "update runner finished rc=%d", rc);
    return rc;
}
