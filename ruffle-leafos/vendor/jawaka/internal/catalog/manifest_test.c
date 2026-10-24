#include "internal/catalog/manifest.h"

#include "cJSON.h"

#include <assert.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef JW_CONTENT_FIXTURES_ROOT
#define JW_CONTENT_FIXTURES_ROOT "../leaf-contracts/contracts/leaf-content/manifests"
#endif

#ifndef JW_CONTENT_SCRAPE_FIXTURE_PATH
#define JW_CONTENT_SCRAPE_FIXTURE_PATH \
    "../leaf-contracts/contracts/leaf-content/scrape/fixtures.json"
#endif

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *text = malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(text, 1u, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(text);
        return NULL;
    }
    text[got] = '\0';
    return text;
}

static const char *json_string(const cJSON *root, const char *key,
                               const char *fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
}

static cJSON *load_optional_json(const char *path) {
    char *text = read_file(path);
    if (!text) {
        return NULL;
    }
    cJSON *json = cJSON_Parse(text);
    free(text);
    return json;
}

static int walk(const char *kind, bool expect_valid) {
    char root[1024];
    snprintf(root, sizeof(root), "%s/%s", JW_CONTENT_FIXTURES_ROOT, kind);
    DIR *dir = opendir(root);
    assert(dir);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char fixture[1200], pak_path[1300], expect_path[1300], context_path[1300];
        snprintf(fixture, sizeof(fixture), "%s/%s", root, entry->d_name);
        snprintf(pak_path, sizeof(pak_path), "%s/pak.json", fixture);
        snprintf(expect_path, sizeof(expect_path), "%s/expect.json", fixture);
        snprintf(context_path, sizeof(context_path), "%s/context.json", fixture);
        char *pak = read_file(pak_path);
        cJSON *expect = load_optional_json(expect_path);
        cJSON *context = load_optional_json(context_path);
        assert(pak && expect);

        const char *lane_text = json_string(context, "install_lane", "platform");
        jw_content_install_lane lane = strcmp(lane_text, "shared") == 0
                                           ? JW_CONTENT_SHARED_LANE
                                           : JW_CONTENT_PLATFORM_LANE;
        const char *source_id = json_string(context, "source_id", "primary");
        jw_content_manifest manifest;
        char reason[JW_CONTENT_REASON_MAX] = {0};
        bool valid = jw_content_manifest_validate(pak, fixture, lane, source_id,
                                                  &manifest, reason, sizeof(reason));
        if (valid != expect_valid) {
            fprintf(stderr, "FAIL %s/%s: expected valid=%d, got valid=%d reason=%s\n",
                    kind, entry->d_name, expect_valid, valid, reason);
            assert(false);
        }
        if (!expect_valid) {
            const char *wanted = json_string(expect, "reason", "");
            if (strcmp(reason, wanted) != 0) {
                fprintf(stderr, "FAIL %s/%s: expected %s, got %s\n",
                        kind, entry->d_name, wanted, reason);
                assert(false);
            }
        } else {
            const cJSON *warnings = cJSON_GetObjectItemCaseSensitive(expect, "warnings");
            bool wants_warning = cJSON_IsArray(warnings) && cJSON_GetArraySize(warnings) > 0;
            if (manifest.redundant_case_variant != wants_warning) {
                fprintf(stderr, "FAIL %s/%s: warning mismatch\n", kind, entry->d_name);
                assert(false);
            }
            jw_content_manifest_destroy(&manifest);
        }
        printf("ok: manifests/%s/%s%s%s\n", kind, entry->d_name,
               expect_valid ? "" : " -> ", expect_valid ? "" : reason);
        count++;
        cJSON_Delete(context);
        cJSON_Delete(expect);
        free(pak);
    }
    closedir(dir);
    return count;
}

static int check_scrape_fixtures(void) {
    cJSON *fixtures = load_optional_json(JW_CONTENT_SCRAPE_FIXTURE_PATH);
    assert(fixtures);
    const cJSON *base_provides = cJSON_GetObjectItemCaseSensitive(
        fixtures, "base_provides");
    const cJSON *cases = cJSON_GetObjectItemCaseSensitive(fixtures, "cases");
    assert(cJSON_IsObject(base_provides) && cJSON_IsArray(cases));

    int count = 0;
    const cJSON *test_case = NULL;
    cJSON_ArrayForEach(test_case, cases) {
        const char *name = json_string(test_case, "name", "unnamed");
        const cJSON *provided = cJSON_GetObjectItemCaseSensitive(test_case,
                                                                 "provides");
        const cJSON *content_scrape = cJSON_GetObjectItemCaseSensitive(
            test_case, "content_scrape");
        const cJSON *valid_item = cJSON_GetObjectItemCaseSensitive(test_case,
                                                                   "valid");
        bool expect_valid = cJSON_IsTrue(valid_item);

        cJSON *document = cJSON_CreateObject();
        assert(document);
        cJSON_AddItemToObject(document, "provides", cJSON_Duplicate(
            provided ? provided : base_provides, true));
        if (content_scrape) {
            cJSON_AddItemToObject(document, "content_scrape",
                                  cJSON_Duplicate(content_scrape, true));
        }

        const cJSON *validated = NULL;
        char reason[JW_CONTENT_REASON_MAX] = {0};
        int status = jw_content_scrape_validate(document, &validated,
                                                reason, sizeof(reason));
        if (expect_valid) {
            int wanted = content_scrape ? 1 : 0;
            const cJSON *expected_block = cJSON_GetObjectItemCaseSensitive(
                document, "content_scrape");
            if (status != wanted || (status > 0 && validated != expected_block)) {
                fprintf(stderr,
                        "FAIL scrape/%s: expected status=%d, got status=%d reason=%s\n",
                        name, wanted, status, reason);
                assert(false);
            }
        } else {
            const char *wanted = json_string(test_case, "reason", "");
            if (status != -1 || strcmp(reason, wanted) != 0) {
                fprintf(stderr,
                        "FAIL scrape/%s: expected %s, got status=%d reason=%s\n",
                        name, wanted, status, reason);
                assert(false);
            }
        }
        printf("ok: scrape/%s%s%s\n", name, expect_valid ? "" : " -> ",
               expect_valid ? "" : reason);
        cJSON_Delete(document);
        count++;
    }
    cJSON_Delete(fixtures);
    return count;
}

int main(void) {
    struct stat st;
    if (stat(JW_CONTENT_FIXTURES_ROOT, &st) != 0) {
        fprintf(stderr, "fixture root missing: %s\n", JW_CONTENT_FIXTURES_ROOT);
        return 1;
    }
    int valid = walk("valid", true);
    int invalid = walk("invalid", false);
    int scrape = check_scrape_fixtures();
    printf("content-manifest-test: %d valid + %d invalid fixtures checked\n",
           valid, invalid);
    assert(valid == 9);
    assert(invalid == 53);
    assert(scrape == 14);
    puts("PASS content-manifest-test");
    return 0;
}
