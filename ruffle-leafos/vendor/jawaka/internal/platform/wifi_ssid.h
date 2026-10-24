#ifndef JW_PLATFORM_WIFI_SSID_H
#define JW_PLATFORM_WIFI_SSID_H

#include <stddef.h>

#define JW_WIFI_SSID_MAX_BYTES 32
#define JW_WIFI_SSID_HEX_SIZE (JW_WIFI_SSID_MAX_BYTES * 2 + 1)
#define JW_WIFI_SSID_PRINTABLE_SIZE (JW_WIFI_SSID_MAX_BYTES * 4 + 1)

/* Convert wpa_cli's printf-escaped SSID output to its original bytes. */
int jw_wifi_ssid_decode(const char *encoded, char *out, size_t out_size);

/* Convert a canonical, NUL-terminated SSID to wpa_supplicant's hex form. */
int jw_wifi_ssid_hex(const char *ssid, char *out, size_t out_size);

/* Parse a wpa_supplicant ssid= value: quoted, P"...", or unquoted hex. */
int jw_wifi_ssid_parse_config(const char *value, char *out, size_t out_size);

#endif /* JW_PLATFORM_WIFI_SSID_H */
