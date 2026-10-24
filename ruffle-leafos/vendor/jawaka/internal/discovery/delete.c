#include "internal/discovery/delete.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int jw__error(char *error, size_t size, const char *format, ...) {
    if (error && size) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, size, format, args);
        va_end(args);
    }
    return -1;
}

bool jw_delete_supported(const jw_ra_system *system) {
    /* Deleting an arcade parent ROM breaks its clones, which the preview's
       warning doesn't describe. PortMaster manages ports; deleting the script
       would leave the port's folder behind. */
    static const char *const denied[] = {
        "ARCADE", "MAME", "MAME2003", "MAME2010", "NEOGEO", "NAOMI", "ATOMISWAVE", "PORTS",
    };
    /* A content pak owns its systems' content, and a filename-based launch
       contract means the game is a folder; deletion is never recursive. */
    if (!system || !system->id || (system->provider && *system->provider) || system->file_names.count)
        return false;
    for (size_t i = 0; i < sizeof(denied) / sizeof(denied[0]); i++)
        if (!strcasecmp(system->id, denied[i])) return false;
    return true;
}

static bool jw__writable_image(const jw_ra_system *system) {
    static const char *const systems[] = {"AMIGA", "DOS", "PC98"};
    for (size_t i = 0; i < sizeof(systems) / sizeof(systems[0]); i++)
        if (!strcasecmp(system->id, systems[i])) return true;
    return false;
}

/* Post-order from the launch file: every file a descriptor names is removed
   before that descriptor. Iterative so a long descriptor chain cannot exhaust
   the worker's stack. order receives each reachable file once. */
static int jw__removal_order(const jw_content *content, size_t *order, size_t *count) {
    size_t nodes = content->file_count, edges = content->reference_count;
    size_t *start = calloc(nodes + 1, sizeof(*start));
    size_t *children = calloc(edges ? edges : 1, sizeof(*children));
    size_t *fill = calloc(nodes, sizeof(*fill));
    size_t *stack = calloc(nodes, sizeof(*stack));
    size_t *cursor = calloc(nodes, sizeof(*cursor));
    bool *visited = calloc(nodes, sizeof(*visited));
    int rc = -1;
    if (!start || !children || !fill || !stack || !cursor || !visited) goto done;
    for (size_t i = 0; i < edges; i++) start[content->references[i].parent + 1]++;
    for (size_t i = 0; i < nodes; i++) start[i + 1] += start[i];
    memcpy(fill, start, nodes * sizeof(*fill));
    for (size_t i = 0; i < edges; i++)
        children[fill[content->references[i].parent]++] = content->references[i].child;
    size_t top = 1;
    *count = 0;
    stack[0] = content->launch_file;
    cursor[0] = start[content->launch_file];
    visited[content->launch_file] = true;
    while (top) {
        size_t node = stack[top - 1];
        if (cursor[top - 1] < start[node + 1]) {
            size_t child = children[cursor[top - 1]++];
            if (visited[child]) continue;
            visited[child] = true;
            stack[top] = child;
            cursor[top] = start[child];
            top++;
        } else {
            order[(*count)++] = node;
            top--;
        }
    }
    rc = 0;
done:
    free(start); free(children); free(fill); free(stack); free(cursor); free(visited);
    return rc;
}

static uint64_t jw__hash(uint64_t hash, const char *text, size_t length) {
    for (size_t i = 0; i < length; i++) hash = (hash ^ (unsigned char)text[i]) * 1099511628211ULL;
    return hash;
}

int jw_delete_plan_build(const jw_storage_source_list *sources, const jw_ra_system *system,
                         const char *source_id, const char *rom_relpath,
                         jw_delete_plan *out, char *error, size_t error_size) {
    if (error && error_size) *error = '\0';
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!jw_delete_supported(system)) return jw__error(error, error_size, "Delete isn't available for this game.");
    if (!sources || !source_id || !rom_relpath) return jw__error(error, error_size, "Invalid game identity.");
    /* Inspection resolves symlinks, so a symlink launch file would delete its target. */
    const jw_storage_source *source = NULL;
    for (int i = 0; i < sources->count; i++)
        if (!strcmp(sources->sources[i].id, source_id)) source = &sources->sources[i];
    if (source) {
        char path[JW_STORAGE_PATH_MAX];
        struct stat st;
        int n = snprintf(path, sizeof(path), "%s/%s", source->roms_path, rom_relpath);
        if (n > 0 && (size_t)n < sizeof(path) && !lstat(path, &st) && S_ISLNK(st.st_mode))
            return jw__error(error, error_size, "Delete can't remove a symlinked game file.");
    }
    jw_content content;
    if (jw_content_inspect(sources, source_id, rom_relpath, system, &content, error, error_size)) return -1;
    size_t *order = calloc(content.file_count, sizeof(*order));
    size_t count = 0;
    int rc = -1;
    if (!order || jw__removal_order(&content, order, &count)) {
        jw__error(error, error_size, "Out of memory preparing deletion");
        goto done;
    }
    out->files = calloc(count, sizeof(*out->files));
    if (!out->files) { jw__error(error, error_size, "Out of memory preparing deletion"); goto done; }
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < count; i++) {
        const jw_content_file *file = &content.files[order[i]];
        jw_delete_file *target = &out->files[i];
        snprintf(target->source_id, sizeof(target->source_id), "%s", file->source_id);
        snprintf(target->rom_relpath, sizeof(target->rom_relpath), "%s", file->rom_relpath);
        snprintf(target->path, sizeof(target->path), "%s", file->path);
        target->size = file->size;
        target->missing = file->missing;
        if (!file->missing) out->bytes += file->size;
        char line[64];
        int n = snprintf(line, sizeof(line), "%llu %d\n", (unsigned long long)file->size, file->missing);
        hash = jw__hash(hash, file->source_id, strlen(file->source_id) + 1);
        hash = jw__hash(hash, file->rom_relpath, strlen(file->rom_relpath) + 1);
        hash = jw__hash(hash, line, (size_t)n);
    }
    out->file_count = count;
    out->writable_image = jw__writable_image(system);
    snprintf(out->digest, sizeof(out->digest), "%016llx", (unsigned long long)hash);
    rc = 0;
done:
    free(order);
    jw_content_free(&content);
    if (rc) jw_delete_plan_free(out);
    return rc;
}

void jw_delete_plan_free(jw_delete_plan *plan) {
    if (!plan) return;
    free(plan->files);
    memset(plan, 0, sizeof(*plan));
}

void jw_delete_result_free(jw_delete_result *result) {
    if (!result) return;
    free(result->outcomes);
    memset(result, 0, sizeof(*result));
}

static int jw__sync_parent(const jw_delete_file *file, char *error, size_t error_size) {
    char parent[JW_STORAGE_PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", file->path);
    char *slash = strrchr(parent, '/');
    if (!slash) return jw__error(error, error_size, "Invalid path for %.160s", file->rom_relpath);
    if (slash == parent) slash[1] = '\0'; else *slash = '\0';
    int directory = open(parent, O_RDONLY | O_DIRECTORY);
    int rc = directory < 0 ? -1 : fsync(directory);
    int saved = errno;
    if (directory >= 0 && close(directory) && !rc) { rc = -1; saved = errno; }
    return rc ? jw__error(error, error_size, "Couldn't save the removal of %.160s: %s",
                          file->rom_relpath, strerror(saved)) : 0;
}

int jw_delete_execute(const jw_delete_plan *plan, jw_delete_guard guard, void *context,
                      jw_delete_result *result, char *error, size_t error_size) {
    if (error && error_size) *error = '\0';
    if (!result) return -1;
    memset(result, 0, sizeof(*result));
    result->failed_index = (size_t)-1;
    if (!plan || !plan->files) return jw__error(error, error_size, "No deletion plan");
    result->outcomes = calloc(plan->file_count, sizeof(*result->outcomes));
    if (!result->outcomes) return jw__error(error, error_size, "Out of memory recording deletion");
    for (size_t i = 0; i < plan->file_count; i++) {
        const jw_delete_file *file = &plan->files[i];
        result->failed_index = i;
        if (guard && guard(context, file, error, error_size)) return -1;
        struct stat st;
        if (lstat(file->path, &st)) {
            if (errno != ENOENT)
                return jw__error(error, error_size, "Couldn't check %.160s: %s", file->rom_relpath, strerror(errno));
            result->outcomes[i] = JW_DELETE_ABSENT;
            result->absent_count++;
            continue;
        }
        if (!S_ISREG(st.st_mode))
            return jw__error(error, error_size, "Couldn't delete %.160s: it's no longer a regular file", file->rom_relpath);
        if (unlink(file->path))
            return jw__error(error, error_size, "Couldn't delete %.160s: %s", file->rom_relpath, strerror(errno));
        result->outcomes[i] = JW_DELETE_REMOVED;
        result->removed_count++;
        result->bytes += (uint64_t)st.st_size;
        /* Record each removal before the descriptor that names it goes, so an
           interruption still leaves the launch file to retry from. */
        if (jw__sync_parent(file, error, error_size)) return -1;
    }
    result->failed_index = (size_t)-1;
    return 0;
}
