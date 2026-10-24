/* THEME-1 parity: the install-time validator against leaf-contracts.
 *
 *   theme-package-test <leaf-themes contract dir> <corpus dir>
 *
 * Every committed fixture must produce exactly the reasons and warnings its
 * hand-written expect.json entry names. The corpus (scripts/
 * theme-package-corpus.py) adds the fixture built in memory and every boundary
 * variant the contract's own checks build, each with the reference validator's
 * answer, and the C validator must match those exactly too. */
#include "internal/store/theme_package.h"

#include "cJSON.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

static char *read_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)len, fp) == (size_t)len) {
        buf[len] = '\0';
    } else {
        free(buf);
        buf = NULL;
    }
    fclose(fp);
    return buf;
}

static void remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[4096];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            remove_tree(child);
        }
        if (d) closedir(d);
        rmdir(path);
    } else {
        unlink(path);
    }
}

/* Render a result as the reference validator prints it: sorted slugs. */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void render_mask(uint64_t mask, bool warnings, char *out, size_t out_size) {
    const char *slugs[64];
    int n = 0;
    int count = warnings ? JW_THEME_WARNING_COUNT : JW_THEME_REASON_COUNT;
    for (int i = 0; i < count; i++)
        if (mask & (1ULL << i))
            slugs[n++] = warnings ? jw_theme_package_warning_slug((jw_theme_warning)i)
                                  : jw_theme_package_reason_slug((jw_theme_reason)i);
    qsort(slugs, (size_t)n, sizeof(slugs[0]), cmp_str);
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++)
        used += (size_t)snprintf(out + used, out_size - used, "%s%s", i ? "," : "", slugs[i]);
}

static void render_array(const cJSON *array, char *out, size_t out_size) {
    const char *slugs[64];
    int n = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, array)
        if (cJSON_IsString(item) && n < 64) slugs[n++] = item->valuestring;
    qsort(slugs, (size_t)n, sizeof(slugs[0]), cmp_str);
    size_t used = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++)
        used += (size_t)snprintf(out + used, out_size - used, "%s%s", i ? "," : "", slugs[i]);
}

/* 1 when the C result matches, 0 on a mismatch. */
static int check_case(const char *zip, const char *label, const cJSON *reasons,
                      const cJSON *warnings) {
    char extract[] = "/tmp/jw-theme-package.XXXXXX";
    if (!mkdtemp(extract)) {
        fprintf(stderr, "mkdtemp failed\n");
        exit(2);
    }
    jw_theme_package_result result;
    int rc = jw_theme_package_validate_zip(zip, extract, &result);
    remove_tree(extract);

    char want_r[2048], want_w[256], got_r[2048], got_w[256];
    render_array(reasons, want_r, sizeof(want_r));
    render_array(warnings, want_w, sizeof(want_w));
    render_mask(result.reasons, false, got_r, sizeof(got_r));
    render_mask(result.warnings, true, got_w, sizeof(got_w));
    if (rc != 0 || strcmp(want_r, got_r) != 0 || strcmp(want_w, got_w) != 0) {
        fprintf(stderr, "FAIL %s (%s)\n  expected reasons [%s] warnings [%s]\n"
                        "  got      reasons [%s] warnings [%s] rc=%d\n",
                label, zip, want_r, want_w, got_r, got_w, rc);
        g_failures++;
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <leaf-themes contract dir> <corpus dir>\n", argv[0]);
        return 2;
    }
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/expect.json", argv[1]);
    char *text = read_text(path);
    cJSON *expect = text ? cJSON_Parse(text) : NULL;
    free(text);
    snprintf(path, sizeof(path), "%s/index.json", argv[2]);
    text = read_text(path);
    cJSON *index = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!expect || !index) {
        fprintf(stderr, "could not read expect.json or the corpus index\n");
        return 2;
    }

    /* Hand-written expectations for every fixture, committed or in memory. */
    int fixtures = 0, fixtures_ok = 0;
    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(expect, "fixtures")) {
        const char *file = cJSON_GetObjectItemCaseSensitive(item, "file")->valuestring;
        bool in_memory = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "in_memory"));
        if (in_memory)
            snprintf(path, sizeof(path), "%s/fixtures/%s", argv[2], file);
        else
            snprintf(path, sizeof(path), "%s/fixtures/%s", argv[1], file);
        fixtures++;
        fixtures_ok += check_case(path, file,
                                  cJSON_GetObjectItemCaseSensitive(item, "reasons"),
                                  cJSON_GetObjectItemCaseSensitive(item, "warnings"));
    }

    /* The reference validator's own answers for everything built in memory. */
    int variants = 0, variants_ok = 0;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(index, "cases")) {
        const char *file = cJSON_GetObjectItemCaseSensitive(item, "file")->valuestring;
        snprintf(path, sizeof(path), "%s/%s", argv[2], file);
        variants++;
        variants_ok += check_case(path,
                                  cJSON_GetObjectItemCaseSensitive(item, "label")->valuestring,
                                  cJSON_GetObjectItemCaseSensitive(item, "reasons"),
                                  cJSON_GetObjectItemCaseSensitive(item, "warnings"));
    }

    /* Outside the corpus: a theme.json that is valid JSON but not an object.
       The reference validator has no answer for it inside an archive (it
       raises), so the manifest rule is asserted directly. */
    {
        jw_theme_package_result result;
        memset(&result, 0, sizeof(result));
        static const char array_manifest[] = "[\"not\", \"an\", \"object\"]";
        jw_theme_package_check_manifest((const unsigned char *)array_manifest,
                                        sizeof(array_manifest) - 1, &result);
        if (result.reasons != (1ULL << JW_THEME_MALFORMED_MANIFEST)) {
            fprintf(stderr, "FAIL a theme.json array is not theme-malformed-manifest\n");
            g_failures++;
        }
    }

    printf("theme-package-test: fixtures %d/%d match expect.json, "
           "reference-validator variants %d/%d match\n",
           fixtures_ok, fixtures, variants_ok, variants);
    cJSON_Delete(expect);
    cJSON_Delete(index);
    if (g_failures) {
        fprintf(stderr, "FAIL theme-package-test (%d mismatches)\n", g_failures);
        return 1;
    }
    puts("PASS theme-package-test");
    return 0;
}
