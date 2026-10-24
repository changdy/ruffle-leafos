#include "internal/platform/leaf_version.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void assert_release_version(const char *value,
                                   int major, int minor, int patch) {
    int parsed[3] = {-1, -1, -1};
    assert(jw_leaf_release_version_parse(value, parsed) == 0);
    assert(parsed[0] == major);
    assert(parsed[1] == minor);
    assert(parsed[2] == patch);
}

static void assert_pak_version(const char *value,
                               int major, int minor, int patch) {
    int parsed[3] = {-1, -1, -1};
    assert(jw_pak_version_parse(value, parsed) == 0);
    assert(parsed[0] == major);
    assert(parsed[1] == minor);
    assert(parsed[2] == patch);
}

static void write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    assert(fp);
    size_t length = strlen(text);
    assert(fwrite(text, 1, length, fp) == length);
    assert(fclose(fp) == 0);
}

int main(void) {
    assert_release_version("v0.6.1", 0, 6, 1);
    assert_release_version("V0.7.0", 0, 7, 0);
    assert_release_version("v0.7.0-rc.1", 0, 7, 0);
    assert_release_version("v0.5.3-save-isolation-ota1", 0, 5, 3);
    assert_release_version("0.7.0+build.4", 0, 7, 0);

    int parsed[3];
    assert(jw_leaf_release_version_parse("2026-07-15-g6eb96aa", parsed) != 0);
    assert(jw_leaf_release_version_parse("0.7", parsed) != 0);
    assert(jw_leaf_release_version_parse("", parsed) != 0);
    assert(jw_leaf_release_version_parse(NULL, parsed) != 0);
    assert(jw_leaf_release_version_parse("0.7.0.1", parsed) != 0);
    assert(jw_leaf_release_version_parse("10000.0.0", parsed) != 0);

    assert_pak_version("0.2.0", 0, 2, 0);
    assert(jw_pak_version_parse("v0.2.0", parsed) != 0);
    assert(jw_pak_version_parse("0.2", parsed) != 0);
    assert(jw_pak_version_parse("0.2.0-rc.1", parsed) != 0);
    assert(jw_pak_version_parse("0.2.0garbage", parsed) != 0);
    assert(jw_pak_version_parse("0.2.0.1", parsed) != 0);
    assert(jw_pak_version_parse("0.10000.0", parsed) != 0);

    const int older[3] = {0, 6, 9};
    const int equal[3] = {0, 7, 0};
    const int newer[3] = {1, 0, 0};
    assert(jw_version_cmp(older, equal) < 0);
    assert(jw_version_cmp(equal, equal) == 0);
    assert(jw_version_cmp(newer, equal) > 0);

    char temp[] = "/tmp/jawaka-leaf-version.XXXXXX";
    assert(mkdtemp(temp));
    char path[512];
    assert(snprintf(path, sizeof(path), "%s/release.json", temp) <
           (int)sizeof(path));

    jw_installed_release release;
    assert(jw_installed_release_read(temp, &release) == 1);
    assert(release.schema == 0);
    assert(release.version[0] == '\0');
    assert(release.release_id[0] == '\0');

    write_text(path,
               "{\n"
               "  \"schema\": 1,\n"
               "  \"version\": \"v0.7.0-save-isolation-ota1\",\n"
               "  \"release_id\": \"2026-07-20-gabc1234\"\n"
               "}\n");
    assert(jw_installed_release_read(temp, &release) == 0);
    assert(release.schema == 1);
    assert(strcmp(release.version, "v0.7.0-save-isolation-ota1") == 0);
    assert(strcmp(release.release_id, "2026-07-20-gabc1234") == 0);

    write_text(path, "{\"schema\":1,\"release_id\":\"dev\"}\n");
    assert(jw_installed_release_read(temp, &release) == 0);
    assert(release.version[0] == '\0');
    assert(strcmp(release.release_id, "dev") == 0);

    write_text(path, "{\"version\":");
    assert(jw_installed_release_read(temp, &release) == -1);
    assert(release.version[0] == '\0');

    assert(unlink(path) == 0);
    assert(rmdir(temp) == 0);
    puts("PASS leaf-version-test");
    return 0;
}
