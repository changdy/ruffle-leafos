#include "internal/catalog/json.h"
#include "internal/catalog/merge.h"
#include "internal/update/sha256.h"

#include "cJSON.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef JW_MERGE_FIXTURE
#define JW_MERGE_FIXTURE "../leaf-contracts/contracts/leaf-content/merge/fixtures.json"
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
    assert(text);
    assert(fread(text, 1u, (size_t)size, file) == (size_t)size);
    fclose(file);
    text[size] = '\0';
    return text;
}

int main(void) {
    char *fixture_text = read_file(JW_MERGE_FIXTURE);
    if (!fixture_text) {
        fprintf(stderr, "fixture missing: %s\n", JW_MERGE_FIXTURE);
        return 1;
    }
    cJSON *fixture = cJSON_Parse(fixture_text);
    free(fixture_text);
    assert(fixture);
    const cJSON *base = cJSON_GetObjectItemCaseSensitive(fixture, "base");
    const cJSON *cases = cJSON_GetObjectItemCaseSensitive(fixture, "cases");
    int count = 0;
    const cJSON *test = NULL;
    cJSON_ArrayForEach(test, cases) {
        const cJSON *name_item = cJSON_GetObjectItemCaseSensitive(test, "name");
        const char *name = name_item->valuestring;
        cJSON *merged = NULL, *diagnostics = NULL;
        assert(jw_catalog_merge(base,
                                cJSON_GetObjectItemCaseSensitive(test, "contributors"),
                                &merged, &diagnostics) == 0);

        size_t actual_size = 0, expected_size = 0;
        char *actual = jw_catalog_json_canonical(merged, &actual_size);
        char *expected = jw_catalog_json_canonical(
            cJSON_GetObjectItemCaseSensitive(test, "expected"), &expected_size);
        if (!actual || !expected || actual_size != expected_size ||
            memcmp(actual, expected, actual_size) != 0) {
            fprintf(stderr, "FAIL merge/%s: merged bytes differ\nactual: %s\nexpected: %s\n",
                    name, actual ? actual : "(null)", expected ? expected : "(null)");
            assert(false);
        }
        char digest[65];
        jw_sha256_buf_hex(actual, actual_size, digest);
        const cJSON *expected_sha = cJSON_GetObjectItemCaseSensitive(test, "expected_sha256");
        if (strcmp(digest, expected_sha->valuestring) != 0) {
            fprintf(stderr, "FAIL merge/%s: sha %s != %s\n",
                    name, digest, expected_sha->valuestring);
            assert(false);
        }

        size_t diag_size = 0, expected_diag_size = 0;
        char *diag_bytes = jw_catalog_json_canonical(diagnostics, &diag_size);
        char *expected_diag = jw_catalog_json_canonical(
            cJSON_GetObjectItemCaseSensitive(test, "diagnostics"), &expected_diag_size);
        if (!diag_bytes || !expected_diag || diag_size != expected_diag_size ||
            memcmp(diag_bytes, expected_diag, diag_size) != 0) {
            fprintf(stderr, "FAIL merge/%s: diagnostics differ\nactual: %s\nexpected: %s\n",
                    name, diag_bytes ? diag_bytes : "(null)",
                    expected_diag ? expected_diag : "(null)");
            assert(false);
        }
        printf("ok: merge/%s -> %s\n", name, digest);
        free(expected_diag);
        free(diag_bytes);
        free(expected);
        free(actual);
        cJSON_Delete(diagnostics);
        cJSON_Delete(merged);
        count++;
    }
    cJSON_Delete(fixture);
    assert(count == 17);
    puts("PASS catalog-merge-test");
    return 0;
}
