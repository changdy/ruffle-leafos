#include "internal/scrape/scrape_identity.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static bool jw_scrape__descriptor(const char *path, char out[129]) {
    out[0] = '\0';
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) {
        return false;
    }
    unsigned char bytes[257];
    size_t count = fread(bytes, 1u, sizeof(bytes), file);
    bool read_error = ferror(file) != 0;
    fclose(file);
    if (read_error || count == 0u || count > 256u) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        unsigned char c = bytes[i];
        if (c == 0u || (c < 0x20u && c != '\t' && c != '\r' && c != '\n') ||
            c >= 0x7fu) {
            return false;
        }
    }

    size_t line_end = count;
    size_t trailing = count;
    for (size_t i = 0; i < count; i++) {
        if (bytes[i] == '\n') {
            line_end = i;
            trailing = i + 1u;
            break;
        }
        if (bytes[i] == '\r') {
            if (i + 1u >= count || bytes[i + 1u] != '\n') {
                return false;
            }
            line_end = i;
            trailing = i + 2u;
            break;
        }
    }
    for (size_t i = trailing; i < count; i++) {
        if (bytes[i] == ' ' || bytes[i] == '\t' || bytes[i] == '\n') {
            continue;
        }
        if (bytes[i] == '\r' && i + 1u < count && bytes[i + 1u] == '\n') {
            i++;
            continue;
        }
        return false;
    }

    size_t start = 0;
    while (start < line_end &&
           (bytes[start] == ' ' || bytes[start] == '\t')) {
        start++;
    }
    while (line_end > start &&
           (bytes[line_end - 1u] == ' ' || bytes[line_end - 1u] == '\t')) {
        line_end--;
    }
    size_t length = line_end - start;
    if (length < 1u || length > 128u) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = bytes[start + i];
        bool alphanumeric = (c >= 'A' && c <= 'Z') ||
                            (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9');
        if (!alphanumeric &&
            (i == 0u || (c != '_' && c != '.' && c != ':' && c != '-'))) {
            return false;
        }
    }
    memcpy(out, bytes + start, length);
    out[length] = '\0';
    return true;
}

static void jw_scrape__add(jw_scrape_identity_candidates *out,
                           const char *name) {
    if (!name || !name[0] || out->count >= JW_SCRAPE_IDENTITY_CANDIDATE_MAX) {
        return;
    }
    for (size_t i = 0; i < out->count; i++) {
        if (strcasecmp(out->names[i], name) == 0) {
            return;
        }
    }
    size_t length = strlen(name);
    if (length >= JW_SCRAPE_IDENTITY_NAME_MAX) {
        return;
    }
    memcpy(out->names[out->count++], name, length + 1u);
}

static void jw_scrape__add_with_extension(
    jw_scrape_identity_candidates *out,
    const char *name,
    const char *extension,
    bool keep_existing_extension) {
    if (!name || !name[0] || !extension || !extension[0]) {
        return;
    }
    size_t name_len = strlen(name);
    size_t extension_len = strlen(extension);
    if (keep_existing_extension && name_len > extension_len &&
        name[name_len - extension_len - 1u] == '.' &&
        strcasecmp(name + name_len - extension_len, extension) == 0) {
        jw_scrape__add(out, name);
        return;
    }
    char candidate[JW_SCRAPE_IDENTITY_NAME_MAX];
    int written = snprintf(candidate, sizeof(candidate), "%s.%s", name, extension);
    if (written > 0 && (size_t)written < sizeof(candidate)) {
        jw_scrape__add(out, candidate);
    }
}

void jw_scrape_identity_build(const char *descriptor_path,
                              const char *rom_name,
                              const char *effective_title,
                              const char *lookup_extension,
                              jw_scrape_identity_candidates *out) {
    memset(out, 0, sizeof(*out));
    char descriptor[129];
    if (jw_scrape__descriptor(descriptor_path, descriptor)) {
        jw_scrape__add_with_extension(out, descriptor, lookup_extension, false);
    }
    jw_scrape__add(out, rom_name);
    jw_scrape__add_with_extension(out, effective_title, lookup_extension, true);
}
