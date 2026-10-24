#include "internal/catalog/json.h"

#include "cJSON.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} jw_json_buf;

static void append(jw_json_buf *buf, const char *data, size_t length) {
    if (buf->failed) return;
    if (buf->length + length + 1u > buf->capacity) {
        size_t capacity = buf->capacity ? buf->capacity : 512u;
        while (capacity < buf->length + length + 1u) capacity *= 2u;
        char *grown = realloc(buf->data, capacity);
        if (!grown) {
            buf->failed = 1;
            return;
        }
        buf->data = grown;
        buf->capacity = capacity;
    }
    memcpy(buf->data + buf->length, data, length);
    buf->length += length;
    buf->data[buf->length] = '\0';
}

static void puts_buf(jw_json_buf *buf, const char *text) {
    append(buf, text, strlen(text));
}

static void string_buf(jw_json_buf *buf, const char *value) {
    puts_buf(buf, "\"");
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; p++) {
        switch (*p) {
        case '"': puts_buf(buf, "\\\""); break;
        case '\\': puts_buf(buf, "\\\\"); break;
        case '\b': puts_buf(buf, "\\b"); break;
        case '\f': puts_buf(buf, "\\f"); break;
        case '\n': puts_buf(buf, "\\n"); break;
        case '\r': puts_buf(buf, "\\r"); break;
        case '\t': puts_buf(buf, "\\t"); break;
        default:
            if (*p < 0x20u) {
                char escaped[8];
                snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)*p);
                puts_buf(buf, escaped);
            } else {
                append(buf, (const char *)p, 1u);
            }
        }
    }
    puts_buf(buf, "\"");
}

static int key_cmp(const void *left, const void *right) {
    const cJSON *const *a = left;
    const cJSON *const *b = right;
    return strcmp((*a)->string, (*b)->string);
}

static void emit(jw_json_buf *buf, const cJSON *value) {
    if (cJSON_IsNull(value)) {
        puts_buf(buf, "null");
    } else if (cJSON_IsTrue(value)) {
        puts_buf(buf, "true");
    } else if (cJSON_IsFalse(value)) {
        puts_buf(buf, "false");
    } else if (cJSON_IsString(value)) {
        string_buf(buf, value->valuestring);
    } else if (cJSON_IsNumber(value)) {
        char *printed = cJSON_PrintUnformatted(value);
        if (!printed) buf->failed = 1;
        else {
            puts_buf(buf, printed);
            free(printed);
        }
    } else if (cJSON_IsArray(value)) {
        puts_buf(buf, "[");
        bool first = true;
        for (const cJSON *item = value->child; item; item = item->next) {
            if (!first) puts_buf(buf, ",");
            emit(buf, item);
            first = false;
        }
        puts_buf(buf, "]");
    } else if (cJSON_IsObject(value)) {
        size_t count = 0;
        for (const cJSON *item = value->child; item; item = item->next) count++;
        cJSON **items = count ? malloc(count * sizeof(*items)) : NULL;
        if (count && !items) {
            buf->failed = 1;
            return;
        }
        size_t i = 0;
        for (cJSON *item = value->child; item; item = item->next) items[i++] = item;
        qsort(items, count, sizeof(*items), key_cmp);
        puts_buf(buf, "{");
        for (i = 0; i < count; i++) {
            if (i) puts_buf(buf, ",");
            string_buf(buf, items[i]->string);
            puts_buf(buf, ":");
            emit(buf, items[i]);
        }
        puts_buf(buf, "}");
        free(items);
    } else {
        buf->failed = 1;
    }
}

char *jw_catalog_json_canonical(const cJSON *value, size_t *out_size) {
    jw_json_buf buf = {0};
    emit(&buf, value);
    puts_buf(&buf, "\n");
    if (buf.failed) {
        free(buf.data);
        return NULL;
    }
    if (out_size) *out_size = buf.length;
    return buf.data;
}
