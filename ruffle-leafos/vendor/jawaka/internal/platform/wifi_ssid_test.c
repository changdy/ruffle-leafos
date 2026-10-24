#include "internal/platform/wifi_ssid.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect_decode(const char *encoded, const char *expected) {
    char out[64];
    assert(jw_wifi_ssid_decode(encoded, out, sizeof(out)) == (int)strlen(expected));
    assert(strcmp(out, expected) == 0);
}

static void expect_config(const char *value, const char *expected) {
    char out[64];
    assert(jw_wifi_ssid_parse_config(value, out, sizeof(out)) == (int)strlen(expected));
    assert(strcmp(out, expected) == 0);
}

int main(void) {
    const char *iphone = "Gareth\xe2\x80\x99s iPhone";
    expect_decode("Gareth\\xe2\\x80\\x99s iPhone", iphone);
    expect_decode("caf\\xc3\\xa9", "caf\xc3\xa9");
    expect_decode("quote\\\"slash\\\\", "quote\"slash\\");
    expect_decode("controls\\n\\r\\t\\e", "controls\n\r\t\x1b");
    expect_decode("plain ASCII", "plain ASCII");

    char hex[JW_WIFI_SSID_HEX_SIZE];
    assert(jw_wifi_ssid_hex(iphone, hex, sizeof(hex)) == 34);
    assert(strcmp(hex, "476172657468e2809973206950686f6e65") == 0);

    expect_config("\"plain ASCII\"\n", "plain ASCII");
    expect_config("\"quote\"slash\\\"", "quote\"slash\\");
    expect_config("P\"Gareth\\xe2\\x80\\x99s iPhone\"", iphone);
    expect_config("476172657468e2809973206950686f6e65", iphone);

    char scan[64], saved[64], broken[64];
    assert(jw_wifi_ssid_decode("Gareth\\xe2\\x80\\x99s iPhone",
                               scan, sizeof(scan)) >= 0);
    assert(jw_wifi_ssid_decode("Gareth\\xe2\\x80\\x99s iPhone",
                               saved, sizeof(saved)) >= 0);
    assert(jw_wifi_ssid_decode("Gareth\\\\xe2\\\\x80\\\\x99s iPhone",
                               broken, sizeof(broken)) >= 0);
    assert(strcmp(scan, saved) == 0);
    assert(strcmp(scan, broken) != 0);

    char max_ssid[JW_WIFI_SSID_MAX_BYTES + 1];
    memset(max_ssid, 'a', JW_WIFI_SSID_MAX_BYTES);
    max_ssid[JW_WIFI_SSID_MAX_BYTES] = '\0';
    assert(jw_wifi_ssid_decode(max_ssid, saved, sizeof(saved)) == JW_WIFI_SSID_MAX_BYTES);
    assert(jw_wifi_ssid_hex(max_ssid, hex, sizeof(hex)) == JW_WIFI_SSID_MAX_BYTES * 2);

    char too_long[JW_WIFI_SSID_MAX_BYTES + 2];
    memset(too_long, 'b', JW_WIFI_SSID_MAX_BYTES + 1);
    too_long[JW_WIFI_SSID_MAX_BYTES + 1] = '\0';
    assert(jw_wifi_ssid_decode(too_long, saved, sizeof(saved)) < 0);
    assert(jw_wifi_ssid_hex(too_long, hex, sizeof(hex)) < 0);

    assert(jw_wifi_ssid_decode("bad\\x", saved, sizeof(saved)) < 0);
    assert(jw_wifi_ssid_decode("bad\\xgg", saved, sizeof(saved)) < 0);
    assert(jw_wifi_ssid_decode("bad\\q", saved, sizeof(saved)) < 0);
    assert(jw_wifi_ssid_decode("bad\\x00ssid", saved, sizeof(saved)) < 0);
    assert(jw_wifi_ssid_parse_config("610062", saved, sizeof(saved)) < 0);
    assert(jw_wifi_ssid_parse_config("abc", saved, sizeof(saved)) < 0);

    puts("wifi ssid tests passed");
    return 0;
}
