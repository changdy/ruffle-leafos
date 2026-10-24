#include "internal/platform/wifi_ssid.h"

#include <string.h>

static int jw__hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int jw__ssid_finish(char *out, size_t out_size, size_t len) {
    if (!out || len == 0 || len > JW_WIFI_SSID_MAX_BYTES || len >= out_size) {
        if (out && out_size) out[0] = '\0';
        return -1;
    }
    out[len] = '\0';
    return (int)len;
}

int jw_wifi_ssid_decode(const char *encoded, char *out, size_t out_size) {
    if (!encoded || !out || out_size == 0) return -1;

    size_t len = 0;
    for (size_t i = 0; encoded[i]; i++) {
        unsigned char value = (unsigned char)encoded[i];
        if (value == '\\') {
            char escape = encoded[++i];
            if (!escape) return jw__ssid_finish(out, out_size, 0);
            switch (escape) {
                case '\\': value = '\\'; break;
                case '"':  value = '"'; break;
                case 'n':  value = '\n'; break;
                case 'r':  value = '\r'; break;
                case 't':  value = '\t'; break;
                case 'e':  value = 0x1b; break;
                case 'x': {
                    if (!encoded[i + 1] || !encoded[i + 2]) {
                        out[0] = '\0';
                        return -1;
                    }
                    int high = jw__hex_digit(encoded[i + 1]);
                    int low = jw__hex_digit(encoded[i + 2]);
                    if (high < 0 || low < 0) {
                        out[0] = '\0';
                        return -1;
                    }
                    value = (unsigned char)((high << 4) | low);
                    i += 2;
                    break;
                }
                default:
                    out[0] = '\0';
                    return -1;
            }
        }
        if (value == 0 || len >= JW_WIFI_SSID_MAX_BYTES || len + 1 >= out_size) {
            out[0] = '\0';
            return -1;
        }
        out[len++] = (char)value;
    }
    return jw__ssid_finish(out, out_size, len);
}

int jw_wifi_ssid_hex(const char *ssid, char *out, size_t out_size) {
    static const char digits[] = "0123456789abcdef";
    if (!ssid || !out || out_size == 0) return -1;

    size_t len = strlen(ssid);
    if (len == 0 || len > JW_WIFI_SSID_MAX_BYTES || out_size < len * 2 + 1) {
        out[0] = '\0';
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char byte = (unsigned char)ssid[i];
        out[i * 2] = digits[byte >> 4];
        out[i * 2 + 1] = digits[byte & 0x0f];
    }
    out[len * 2] = '\0';
    return (int)(len * 2);
}

int jw_wifi_ssid_parse_config(const char *value, char *out, size_t out_size) {
    if (!value || !out || out_size == 0) return -1;
    out[0] = '\0';

    while (*value == ' ' || *value == '\t') value++;
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r' ||
                       value[len - 1] == ' ' || value[len - 1] == '\t')) {
        len--;
    }

    if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
        size_t raw_len = len - 2;
        if (raw_len == 0 || raw_len > JW_WIFI_SSID_MAX_BYTES || raw_len >= out_size) {
            return -1;
        }
        memcpy(out, value + 1, raw_len);
        return jw__ssid_finish(out, out_size, raw_len);
    }

    if (len >= 3 && value[0] == 'P' && value[1] == '"' && value[len - 1] == '"') {
        size_t encoded_len = len - 3;
        if (encoded_len == 0 || encoded_len >= JW_WIFI_SSID_PRINTABLE_SIZE) return -1;
        char encoded[JW_WIFI_SSID_PRINTABLE_SIZE];
        memcpy(encoded, value + 2, encoded_len);
        encoded[encoded_len] = '\0';
        return jw_wifi_ssid_decode(encoded, out, out_size);
    }

    if (len == 0 || len > JW_WIFI_SSID_MAX_BYTES * 2 || (len & 1)) return -1;
    size_t raw_len = len / 2;
    if (raw_len >= out_size) return -1;
    for (size_t i = 0; i < raw_len; i++) {
        int high = jw__hex_digit(value[i * 2]);
        int low = jw__hex_digit(value[i * 2 + 1]);
        if (high < 0 || low < 0 || (high == 0 && low == 0)) {
            out[0] = '\0';
            return -1;
        }
        out[i] = (char)((high << 4) | low);
    }
    return jw__ssid_finish(out, out_size, raw_len);
}
