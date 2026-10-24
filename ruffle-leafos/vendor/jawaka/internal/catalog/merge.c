#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/catalog/merge.h"

#include "cJSON.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char *kind;
    char *value;
} claim;

typedef struct {
    claim *items;
    size_t count;
    size_t capacity;
} claims;

typedef struct {
    const char *kind;
    const char *provider;
    const cJSON *entry;
    claims owned;
    bool refused;
} candidate;

typedef struct {
    candidate *items;
    size_t count;
    size_t capacity;
} candidates;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} strings;

static const cJSON *item(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

static const char *text(const cJSON *object, const char *key) {
    const cJSON *value = item(object, key);
    return cJSON_IsString(value) && value->valuestring ? value->valuestring : "";
}

static char *fold(const char *value) {
    size_t length = strlen(value);
    char *copy = malloc(length + 1u);
    if (!copy) return NULL;
    for (size_t i = 0; i < length; i++) {
        copy[i] = (char)tolower((unsigned char)value[i]);
    }
    copy[length] = '\0';
    return copy;
}

static void claims_free(claims *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].kind);
        free(list->items[i].value);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static bool claim_equal(const claim *a, const claim *b) {
    return strcmp(a->kind, b->kind) == 0 && strcmp(a->value, b->value) == 0;
}

static int claim_compare(const void *left, const void *right) {
    const claim *a = left;
    const claim *b = right;
    int by_kind = strcmp(a->kind, b->kind);
    return by_kind ? by_kind : strcmp(a->value, b->value);
}

static int claims_add(claims *list, const char *kind, const char *value) {
    claim next = {.kind = strdup(kind), .value = fold(value)};
    if (!next.kind || !next.value) {
        free(next.kind);
        free(next.value);
        return -1;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (claim_equal(&list->items[i], &next)) {
            free(next.kind);
            free(next.value);
            return 0;
        }
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2u : 8u;
        claim *grown = realloc(list->items, capacity * sizeof(*grown));
        if (!grown) {
            free(next.kind);
            free(next.value);
            return -1;
        }
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count++] = next;
    return 0;
}

static int system_claims(const cJSON *system, claims *out) {
    if (claims_add(out, "system_id", text(system, "id")) != 0 ||
        claims_add(out, "rom_root", text(system, "rom_root")) != 0 ||
        claims_add(out, "image_root", text(system, "image_root")) != 0) {
        return -1;
    }
    const cJSON *patterns = item(system, "patterns");
    const cJSON *pattern = NULL;
    cJSON_ArrayForEach(pattern, patterns) {
        if (cJSON_IsString(pattern) &&
            claims_add(out, "pattern", pattern->valuestring) != 0) return -1;
    }
    qsort(out->items, out->count, sizeof(*out->items), claim_compare);
    return 0;
}

static int core_claims(const cJSON *core, claims *out) {
    if (claims_add(out, "core_id", text(core, "id")) != 0) return -1;
    const char *folder = text(core, "config_folder");
    if (folder[0] && claims_add(out, "config_folder", folder) != 0) return -1;
    const char *info = text(core, "info_name");
    if (info[0]) {
        const char *slash = strrchr(info, '/');
        if (claims_add(out, "info_name", slash ? slash + 1 : info) != 0) return -1;
    }
    qsort(out->items, out->count, sizeof(*out->items), claim_compare);
    return 0;
}

static int base_claims(const cJSON *base, claims *out) {
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, item(base, "systems")) {
        claims one = {0};
        if (system_claims(row, &one) != 0) {
            claims_free(&one);
            return -1;
        }
        for (size_t i = 0; i < one.count; i++) {
            if (claims_add(out, one.items[i].kind, one.items[i].value) != 0) {
                claims_free(&one);
                return -1;
            }
        }
        claims_free(&one);
    }
    cJSON_ArrayForEach(row, item(base, "cores")) {
        claims one = {0};
        if (core_claims(row, &one) != 0) {
            claims_free(&one);
            return -1;
        }
        for (size_t i = 0; i < one.count; i++) {
            if (claims_add(out, one.items[i].kind, one.items[i].value) != 0) {
                claims_free(&one);
                return -1;
            }
        }
        claims_free(&one);
    }
    qsort(out->items, out->count, sizeof(*out->items), claim_compare);
    return 0;
}

static void candidates_free(candidates *list) {
    for (size_t i = 0; i < list->count; i++) claims_free(&list->items[i].owned);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int candidates_add(candidates *list, const char *kind,
                          const char *provider, const cJSON *entry) {
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2u : 16u;
        candidate *grown = realloc(list->items, capacity * sizeof(*grown));
        if (!grown) return -1;
        list->items = grown;
        list->capacity = capacity;
    }
    candidate *next = &list->items[list->count];
    memset(next, 0, sizeof(*next));
    next->kind = kind;
    next->provider = provider;
    next->entry = entry;
    int rc = strcmp(kind, "system") == 0
                 ? system_claims(entry, &next->owned)
                 : core_claims(entry, &next->owned);
    if (rc != 0) {
        claims_free(&next->owned);
        return -1;
    }
    list->count++;
    return 0;
}

static int strings_add(strings *list, const char *value) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], value) == 0) return 0;
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2u : 8u;
        char **grown = realloc(list->items, capacity * sizeof(*grown));
        if (!grown) return -1;
        list->items = grown;
        list->capacity = capacity;
    }
    list->items[list->count] = strdup(value);
    if (!list->items[list->count]) return -1;
    list->count++;
    return 0;
}

static bool strings_contains_casefold(const strings *list, const char *value) {
    for (size_t i = 0; i < list->count; i++) {
        if (strcasecmp(list->items[i], value) == 0) return true;
    }
    return false;
}

static void strings_free(strings *list) {
    for (size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int string_ptr_compare(const void *left, const void *right) {
    return strcmp(*(char *const *)left, *(char *const *)right);
}

static char *format(const char *pattern, ...) {
    va_list args;
    va_start(args, pattern);
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, pattern, copy);
    va_end(copy);
    if (length < 0) {
        va_end(args);
        return NULL;
    }
    char *result = malloc((size_t)length + 1u);
    if (result) vsnprintf(result, (size_t)length + 1u, pattern, args);
    va_end(args);
    return result;
}

static int record(cJSON *diagnostics, const char *provider,
                  const char *reason, char *detail) {
    if (!detail) return -1;
    cJSON *row = cJSON_CreateObject();
    if (!row || !cJSON_AddStringToObject(row, "provider", provider) ||
        !cJSON_AddStringToObject(row, "reason", reason) ||
        !cJSON_AddStringToObject(row, "detail", detail) ||
        !cJSON_AddItemToArray(diagnostics, row)) {
        cJSON_Delete(row);
        free(detail);
        return -1;
    }
    free(detail);
    return 0;
}

static bool claim_in(const claims *list, const claim *wanted) {
    for (size_t i = 0; i < list->count; i++) {
        if (claim_equal(&list->items[i], wanted)) return true;
    }
    return false;
}

static char *base_collision_detail(const candidate *candidate,
                                   const claims *base) {
    size_t capacity = 128u;
    char *hits = malloc(capacity);
    if (!hits) return NULL;
    hits[0] = '\0';
    size_t length = 0;
    for (size_t i = 0; i < candidate->owned.count; i++) {
        const claim *owned = &candidate->owned.items[i];
        if (!claim_in(base, owned)) continue;
        int needed = snprintf(NULL, 0, "%s%s=%s", length ? ", " : "",
                              owned->kind, owned->value);
        if (needed < 0) {
            free(hits);
            return NULL;
        }
        if (length + (size_t)needed + 1u > capacity) {
            while (length + (size_t)needed + 1u > capacity) capacity *= 2u;
            char *grown = realloc(hits, capacity);
            if (!grown) {
                free(hits);
                return NULL;
            }
            hits = grown;
        }
        snprintf(hits + length, capacity - length, "%s%s=%s", length ? ", " : "",
                 owned->kind, owned->value);
        length += (size_t)needed;
    }
    char *detail = format("%s %s claims %s already owned by the release catalog",
                          candidate->kind, text(candidate->entry, "id"), hits);
    free(hits);
    return detail;
}

static const claim *first_contributor_collision(const candidates *all, size_t index) {
    const candidate *candidate = &all->items[index];
    const claim *best = NULL;
    for (size_t c = 0; c < candidate->owned.count; c++) {
        const claim *owned = &candidate->owned.items[c];
        for (size_t j = 0; j < all->count; j++) {
            if (j == index || all->items[j].refused ||
                strcmp(candidate->provider, all->items[j].provider) == 0) continue;
            if (claim_in(&all->items[j].owned, owned) &&
                (!best || claim_compare(owned, best) < 0)) best = owned;
        }
    }
    return best;
}

static int json_id_compare(const void *left, const void *right) {
    const cJSON *const *a = left;
    const cJSON *const *b = right;
    return strcmp(text(*a, "id"), text(*b, "id"));
}

static int sort_array_by_id(cJSON *array) {
    int count = cJSON_GetArraySize(array);
    cJSON **rows = count ? malloc((size_t)count * sizeof(*rows)) : NULL;
    if (count && !rows) return -1;
    for (int i = 0; i < count; i++) rows[i] = cJSON_GetArrayItem(array, i);
    qsort(rows, (size_t)count, sizeof(*rows), json_id_compare);
    cJSON *sorted = cJSON_CreateArray();
    if (!sorted) {
        free(rows);
        return -1;
    }
    for (int i = 0; i < count; i++) {
        cJSON *detached = cJSON_DetachItemViaPointer(array, rows[i]);
        cJSON_AddItemToArray(sorted, detached);
    }
    while (sorted->child) cJSON_AddItemToArray(array, cJSON_DetachItemFromArray(sorted, 0));
    cJSON_Delete(sorted);
    free(rows);
    return 0;
}

static void clear_forbidden(cJSON *node) {
    if (cJSON_IsObject(node)) {
        cJSON *child = node->child;
        while (child) {
            cJSON *next = child->next;
            if (child->string &&
                (strcmp(child->string, "requires_direct_drm") == 0 ||
                 strcmp(child->string, "legacy_flat_core") == 0 ||
                 strcmp(child->string, "name_map") == 0 ||
                 strcmp(child->string, "status") == 0)) {
                cJSON_DeleteItemFromObjectCaseSensitive(node, child->string);
            } else {
                clear_forbidden(child);
            }
            child = next;
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) clear_forbidden(child);
    }
}

static cJSON *find_id(const cJSON *array, const char *id) {
    cJSON *row = NULL;
    cJSON_ArrayForEach(row, array) {
        if (strcasecmp(text(row, "id"), id) == 0) return row;
    }
    return NULL;
}

static bool core_exists(const strings *ids, const char *id) {
    return strings_contains_casefold(ids, id);
}

static int diagnostic_compare(const void *left, const void *right) {
    const cJSON *const *a = left;
    const cJSON *const *b = right;
    int value = strcmp(text(*a, "provider"), text(*b, "provider"));
    if (!value) value = strcmp(text(*a, "reason"), text(*b, "reason"));
    if (!value) value = strcmp(text(*a, "detail"), text(*b, "detail"));
    return value;
}

static int sort_diagnostics(cJSON *array) {
    int count = cJSON_GetArraySize(array);
    cJSON **rows = count ? malloc((size_t)count * sizeof(*rows)) : NULL;
    if (count && !rows) return -1;
    for (int i = 0; i < count; i++) rows[i] = cJSON_GetArrayItem(array, i);
    qsort(rows, (size_t)count, sizeof(*rows), diagnostic_compare);
    cJSON *sorted = cJSON_CreateArray();
    if (!sorted) {
        free(rows);
        return -1;
    }
    for (int i = 0; i < count; i++)
        cJSON_AddItemToArray(sorted, cJSON_DetachItemViaPointer(array, rows[i]));
    while (sorted->child) cJSON_AddItemToArray(array, cJSON_DetachItemFromArray(sorted, 0));
    cJSON_Delete(sorted);
    free(rows);
    return 0;
}

int jw_catalog_merge(const cJSON *base, const cJSON *contributors,
                     cJSON **out_catalog, cJSON **out_diagnostics) {
    if (!cJSON_IsObject(base) || !cJSON_IsArray(contributors) ||
        !out_catalog || !out_diagnostics) return -1;
    *out_catalog = NULL;
    *out_diagnostics = NULL;
    claims release = {0};
    candidates all = {0};
    strings refused_cores = {0};
    strings core_ids = {0};
    cJSON *diagnostics = cJSON_CreateArray();
    cJSON *merged = NULL;
    int rc = -1;
    if (!diagnostics || base_claims(base, &release) != 0) goto done;

    const cJSON *contributor = NULL;
    cJSON_ArrayForEach(contributor, contributors) {
        const char *provider = text(contributor, "provider");
        const cJSON *provides = item(contributor, "provides");
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, item(provides, "systems")) {
            if (candidates_add(&all, "system", provider, entry) != 0) goto done;
        }
        cJSON_ArrayForEach(entry, item(provides, "cores")) {
            if (candidates_add(&all, "core", provider, entry) != 0) goto done;
        }
    }

    /* Stage 2: release namespace collisions. */
    for (size_t i = 0; i < all.count; i++) {
        candidate *candidate = &all.items[i];
        bool hit = false;
        for (size_t c = 0; c < candidate->owned.count; c++)
            if (claim_in(&release, &candidate->owned.items[c])) hit = true;
        if (!hit) continue;
        candidate->refused = true;
        if (strcmp(candidate->kind, "core") == 0 &&
            strings_add(&refused_cores, text(candidate->entry, "id")) != 0) goto done;
        if (record(diagnostics, candidate->provider, "base-collision",
                   base_collision_detail(candidate, &release)) != 0) goto done;
    }

    /* Stage 3: cross-provider collisions refuse every claimant. Determine
       each candidate's first sorted colliding claim before marking any of
       them, so the decision cannot depend on iteration order. */
    const claim **collisions = all.count ? calloc(all.count, sizeof(*collisions)) : NULL;
    if (all.count && !collisions) goto done;
    for (size_t i = 0; i < all.count; i++) {
        if (!all.items[i].refused) collisions[i] = first_contributor_collision(&all, i);
    }
    for (size_t i = 0; i < all.count; i++) {
        if (!collisions[i]) continue;
        candidate *candidate = &all.items[i];
        candidate->refused = true;
        if (strcmp(candidate->kind, "core") == 0 &&
            strings_add(&refused_cores, text(candidate->entry, "id")) != 0) {
            free(collisions);
            goto done;
        }
        strings providers = {0};
        for (size_t j = 0; j < all.count; j++) {
            if (i != j && strcmp(candidate->provider, all.items[j].provider) != 0 &&
                claim_in(&all.items[j].owned, collisions[i]) &&
                strings_add(&providers, all.items[j].provider) != 0) {
                strings_free(&providers);
                free(collisions);
                goto done;
            }
        }
        qsort(providers.items, providers.count, sizeof(*providers.items), string_ptr_compare);
        size_t length = 0;
        for (size_t p = 0; p < providers.count; p++)
            length += strlen(providers.items[p]) + (p ? 2u : 0u);
        char *joined = malloc(length + 1u);
        if (!joined) {
            strings_free(&providers);
            free(collisions);
            goto done;
        }
        joined[0] = '\0';
        for (size_t p = 0; p < providers.count; p++) {
            if (p) strcat(joined, ", ");
            strcat(joined, providers.items[p]);
        }
        char *detail = format("%s %s claims %s=%s, also claimed by %s; both refused",
                              candidate->kind, text(candidate->entry, "id"),
                              collisions[i]->kind, collisions[i]->value, joined);
        free(joined);
        strings_free(&providers);
        if (record(diagnostics, candidate->provider, "contributor-collision", detail) != 0) {
            free(collisions);
            goto done;
        }
    }
    free(collisions);

    cJSON *systems = cJSON_Duplicate(item(base, "systems"), true);
    cJSON *cores = cJSON_Duplicate(item(base, "cores"), true);
    if (!systems || !cores) {
        cJSON_Delete(systems);
        cJSON_Delete(cores);
        goto done;
    }
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, cores) {
        if (strings_add(&core_ids, text(row, "id")) != 0) {
            cJSON_Delete(systems);
            cJSON_Delete(cores);
            goto done;
        }
    }
    for (size_t i = 0; i < all.count; i++) {
        candidate *candidate = &all.items[i];
        if (!candidate->refused && strcmp(candidate->kind, "core") == 0 &&
            strings_add(&core_ids, text(candidate->entry, "id")) != 0) {
            cJSON_Delete(systems);
            cJSON_Delete(cores);
            goto done;
        }
    }

    /* Stage 4: dependency cleanup, retained as a fixed-point loop. */
    bool dropped;
    do {
        dropped = false;
        for (size_t i = 0; i < all.count; i++) {
            candidate *candidate = &all.items[i];
            if (candidate->refused || strcmp(candidate->kind, "system") != 0) continue;
            const char *needed = text(candidate->entry, "default_core");
            if (core_exists(&core_ids, needed)) continue;
            candidate->refused = true;
            dropped = true;
            const char *reason = strings_contains_casefold(&refused_cores, needed)
                                     ? "default-core-unavailable"
                                     : "unknown-default-core";
            if (record(diagnostics, candidate->provider, reason,
                       format("system %s needs core %s, which is not available after merge; "
                              "the system is refused too rather than producing a tile that cannot launch",
                              text(candidate->entry, "id"), needed)) != 0) {
                cJSON_Delete(systems);
                cJSON_Delete(cores);
                goto done;
            }
        }
    } while (dropped);

    for (size_t i = 0; i < all.count; i++) {
        candidate *candidate = &all.items[i];
        if (candidate->refused) continue;
        cJSON *copy = cJSON_Duplicate(candidate->entry, true);
        if (!copy) {
            cJSON_Delete(systems);
            cJSON_Delete(cores);
            goto done;
        }
        clear_forbidden(copy);
        cJSON_AddStringToObject(copy, "provider", candidate->provider);
        if (strcmp(candidate->kind, "core") == 0) {
            cJSON_AddStringToObject(copy, "status", "packaged");
            cJSON_AddFalseToObject(copy, "requires_direct_drm");
            cJSON_AddItemToArray(cores, copy);
        } else {
            cJSON_AddItemToArray(systems, copy);
        }
    }

    /* Stage 5: extensions. */
    cJSON_ArrayForEach(contributor, contributors) {
        const char *provider = text(contributor, "provider");
        const cJSON *extension = NULL;
        cJSON_ArrayForEach(extension, item(item(contributor, "provides"), "system_extensions")) {
            const char *system_id = text(extension, "system_id");
            cJSON *target = find_id(systems, system_id);
            if (!target) {
                if (record(diagnostics, provider, "dangling-extension-system",
                           format("system_extensions names %s, which no system provides after merge; extension dropped",
                                  system_id)) != 0) {
                    cJSON_Delete(systems);
                    cJSON_Delete(cores);
                    goto done;
                }
                continue;
            }
            cJSON *alternates = cJSON_GetObjectItemCaseSensitive(target, "alternate_cores");
            if (!cJSON_IsArray(alternates)) {
                alternates = cJSON_CreateArray();
                cJSON_AddItemToObject(target, "alternate_cores", alternates);
            }
            const cJSON *core = NULL;
            cJSON_ArrayForEach(core, item(extension, "add_alternate_cores")) {
                const char *core_id = core->valuestring;
                if (!core_exists(&core_ids, core_id)) {
                    if (record(diagnostics, provider, "dangling-alternate-core",
                               format("system_extensions for %s names core %s, absent after merge; "
                                      "that name dropped, extension accepted", system_id, core_id)) != 0) {
                        cJSON_Delete(systems);
                        cJSON_Delete(cores);
                        goto done;
                    }
                    continue;
                }
                bool exists = false;
                const cJSON *existing = NULL;
                cJSON_ArrayForEach(existing, alternates) {
                    if (cJSON_IsString(existing) && strcasecmp(existing->valuestring, core_id) == 0)
                        exists = true;
                }
                if (!exists) cJSON_AddItemToArray(alternates, cJSON_CreateString(core_id));
            }
        }
    }

    if (sort_array_by_id(systems) != 0 || sort_array_by_id(cores) != 0 ||
        sort_diagnostics(diagnostics) != 0) {
        cJSON_Delete(systems);
        cJSON_Delete(cores);
        goto done;
    }
    merged = cJSON_CreateObject();
    if (!merged || !cJSON_AddStringToObject(merged, "platform", text(base, "platform"))) {
        cJSON_Delete(merged);
        cJSON_Delete(systems);
        cJSON_Delete(cores);
        goto done;
    }
    cJSON_AddItemToObject(merged, "systems", systems);
    cJSON_AddItemToObject(merged, "cores", cores);
    *out_catalog = merged;
    *out_diagnostics = diagnostics;
    merged = NULL;
    diagnostics = NULL;
    rc = 0;

done:
    cJSON_Delete(merged);
    cJSON_Delete(diagnostics);
    strings_free(&core_ids);
    strings_free(&refused_cores);
    candidates_free(&all);
    claims_free(&release);
    return rc;
}

/* A release-system claim needs this provider's surviving alternate core. */
static bool art_eligible(const cJSON *system, const cJSON *cores,
                         const cJSON *contributor) {
    if (!system) return false;
    const char *provider = text(contributor, "provider");
    if (text(system, "provider")[0])
        return strcmp(text(system, "provider"), provider) == 0;
    const cJSON *extension = NULL;
    cJSON_ArrayForEach(extension, item(item(contributor, "provides"), "system_extensions")) {
        if (strcmp(text(extension, "system_id"), text(system, "id"))) continue;
        const cJSON *core_id = NULL;
        cJSON_ArrayForEach(core_id, item(extension, "add_alternate_cores")) {
            if (!cJSON_IsString(core_id)) continue;
            const cJSON *core = find_id(cores, core_id->valuestring);
            if (!core || strcmp(text(core, "provider"), provider)) continue;
            const cJSON *alternate = NULL;
            cJSON_ArrayForEach(alternate, item(system, "alternate_cores")) {
                if (cJSON_IsString(alternate) && !strcmp(alternate->valuestring, core_id->valuestring))
                    return true;
            }
        }
    }
    return false;
}

int jw_catalog_apply_content_art(cJSON *systems, const cJSON *cores,
                                  const cJSON *contributors, cJSON *diagnostics) {
    /* At most 64 contributors with 32 entries each. Count eligible claims before
       applying any, so conflict handling cannot depend on enumeration order. */
    const cJSON *contributor = NULL;
    cJSON_ArrayForEach(contributor, contributors) {
        const cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, item(item(contributor, "content_art"), "systems")) {
            const char *id = text(entry, "id");
            const char *provider = text(contributor, "provider");
            cJSON *system = find_id(systems, id);
            if (!art_eligible(system, cores, contributor)) {
                if (record(diagnostics, provider, "ineligible-content-art-system", strdup(id))) return -1;
                continue;
            }
            const char *slots[] = {"wordmark", "grid_icon", "wordmark_color"};
            const char *providers[] = {"wordmark_provider", "grid_icon_provider",
                                       "wordmark_color_provider"};
            for (int slot = 0; slot < 3; slot++) {
                if (!text(entry, slots[slot])[0]) continue;
                int claims = 0;
                const cJSON *other = NULL;
                cJSON_ArrayForEach(other, contributors) {
                    if (!art_eligible(system, cores, other)) continue;
                    const cJSON *claim = NULL;
                    cJSON_ArrayForEach(claim, item(item(other, "content_art"), "systems"))
                        if (!strcmp(text(claim, "id"), id) && text(claim, slots[slot])[0]) claims++;
                }
                if (claims > 1) {
                    char detail[64];
                    snprintf(detail, sizeof(detail), "%s%s%s", id, slot ? ":" : "",
                             slot ? slots[slot] : "");
                    if (record(diagnostics, provider, "conflicting-content-art-system", strdup(detail))) return -1;
                    continue;
                }
                cJSON_DeleteItemFromObjectCaseSensitive(system, slots[slot]);
                cJSON_DeleteItemFromObjectCaseSensitive(system, providers[slot]);
                if (!cJSON_AddStringToObject(system, slots[slot], text(entry, slots[slot])) ||
                    !cJSON_AddStringToObject(system, providers[slot], provider)) return -1;
            }
        }
    }
    return sort_diagnostics(diagnostics);
}
