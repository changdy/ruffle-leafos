#include "cJSON.h"

#include <arpa/inet.h>
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef JW_TEST_WIRE_FIXTURES_ROOT
#define JW_TEST_WIRE_FIXTURES_ROOT \
    "../leaf-contracts/contracts/leaf-services/wire-fixtures"
#endif

#define JW_TEST_SEMANTIC_MAX (64u * 1024u)

static unsigned char *read_file(const char *path, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long end = ftell(file);
    assert(end >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    unsigned char *bytes = malloc((size_t)end + 1u);
    assert(bytes);
    assert(fread(bytes, 1, (size_t)end, file) == (size_t)end);
    assert(fclose(file) == 0);
    bytes[end] = '\0';
    *out_len = (size_t)end;
    return bytes;
}

int main(void) {
    char manifest_path[4096];
    snprintf(manifest_path, sizeof(manifest_path), "%s/MANIFEST.json",
             JW_TEST_WIRE_FIXTURES_ROOT);
    size_t manifest_len = 0;
    unsigned char *manifest_bytes = read_file(manifest_path, &manifest_len);
    cJSON *manifest = cJSON_ParseWithLength((char *)manifest_bytes,
                                            manifest_len);
    free(manifest_bytes);
    assert(manifest);
    cJSON *fixtures = cJSON_GetObjectItemCaseSensitive(manifest, "fixtures");
    assert(cJSON_IsArray(fixtures));

    int checked = 0;
    cJSON *fixture = NULL;
    cJSON_ArrayForEach(fixture, fixtures) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(fixture, "name");
        cJSON *violation = cJSON_GetObjectItemCaseSensitive(
            fixture, "expect_semantic_ceiling_violation");
        assert(cJSON_IsString(name) && name->valuestring);
        assert(cJSON_IsBool(violation));

        char frame_path[4096];
        snprintf(frame_path, sizeof(frame_path), "%s/%s.bin",
                 JW_TEST_WIRE_FIXTURES_ROOT, name->valuestring);
        size_t frame_len = 0;
        unsigned char *frame = read_file(frame_path, &frame_len);
        assert(frame_len >= sizeof(uint32_t));
        uint32_t network_len = 0;
        memcpy(&network_len, frame, sizeof(network_len));
        size_t payload_len = (size_t)ntohl(network_len);
        assert(payload_len == frame_len - sizeof(network_len));

        if (cJSON_IsTrue(violation)) {
            assert(payload_len > JW_TEST_SEMANTIC_MAX);
            free(frame);
            checked++;
            continue;
        }
        assert(payload_len <= JW_TEST_SEMANTIC_MAX);
        cJSON *message = cJSON_ParseWithLength(
            (char *)frame + sizeof(network_len), payload_len);
        assert(message);
        char *encoded = cJSON_PrintUnformatted(message);
        assert(encoded);
        size_t encoded_len = strlen(encoded);
        if (encoded_len != payload_len ||
            memcmp(encoded, frame + sizeof(network_len),
                   encoded_len < payload_len ? encoded_len : payload_len) != 0) {
            fprintf(stderr,
                    "fixture %s did not round-trip canonically "
                    "(encoded=%zu payload=%zu)\n",
                    name->valuestring, encoded_len, payload_len);
        }
        assert(encoded_len == payload_len);
        assert(memcmp(encoded, frame + sizeof(network_len), payload_len) == 0);
        uint32_t encoded_prefix = htonl((uint32_t)encoded_len);
        assert(memcmp(&encoded_prefix, frame, sizeof(encoded_prefix)) == 0);
        cJSON_free(encoded);
        cJSON_Delete(message);
        free(frame);
        checked++;
    }
    assert(checked > 0);
    cJSON_Delete(manifest);
    printf("PASS wire-fixture-test (%d fixtures)\n", checked);
    return 0;
}
