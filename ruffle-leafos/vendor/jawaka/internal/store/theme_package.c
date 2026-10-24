#include "internal/store/theme_package.h"

#include "internal/launcher/image_header.h"
#include "miniz.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define JW_TP_MAX_UNCOMPRESSED (25UL * 1024UL * 1024UL)
#define JW_TP_MAX_ENTRIES      512
#define JW_TP_MAX_RATIO        100ULL
#define JW_TP_MAX_MANIFEST     (64UL * 1024UL)
#define JW_TP_ART_MAX_PX       1024u
#define JW_TP_ICON_TARGET_PX   512u
#define JW_TP_WALLPAPER_MAX_PX 2048u
#define JW_TP_PREVIEW_W        960u
#define JW_TP_PREVIEW_H        720u
#define JW_TP_JSON_MAX_DEPTH   512

static const char *const kReasonSlugs[JW_THEME_REASON_COUNT] = {
    "theme-archive-too-large",      "theme-malformed-archive",
    "theme-too-many-entries",       "theme-unsupported-compression",
    "theme-uncompressed-too-large", "theme-compression-ratio",
    "theme-entry-name-encoding",    "theme-absolute-path",
    "theme-backslash-path",         "theme-path-traversal",
    "theme-hidden-file",            "theme-symlink",
    "theme-special-file",           "theme-duplicate-entry",
    "theme-not-single-folder",      "theme-unknown-file",
    "theme-system-id-invalid",      "theme-reserved-system-id",
    "theme-multiple-wallpapers",    "theme-missing-manifest",
    "theme-manifest-too-large",     "theme-malformed-manifest",
    "theme-missing-preview",        "theme-id-mismatch",
    "theme-reserved-name",          "theme-unsupported-image",
    "theme-image-dimensions",       "theme-unknown-schema",
    "theme-unknown-field",          "theme-id-invalid",
    "theme-name-invalid",           "theme-author-invalid",
    "theme-version-invalid",        "theme-min-leaf-version",
    "theme-unknown-license",        "theme-description-invalid",
    "theme-grid-invalid",           "theme-color-invalid",
    "theme-color-level-invalid",    "theme-status-style-invalid",
};

static const char *const kWarningSlugs[JW_THEME_WARNING_COUNT] = {
    "theme-icon-off-size",
    "theme-no-art",
};

/* Folder names a Leaf release ships in Themes/ (bundled-themes.txt), in the
   contract's "Reserved install names" table. The installer also refuses what
   the running release's own bundled-themes.txt lists. */
static const char *const kReservedInstallNames[] = { "Sample" };

static const char *const kLicenses[] = {
    "CC-BY-4.0", "CC-BY-SA-4.0", "CC-BY-NC-SA-2.0", "CC-BY-NC-SA-4.0", "CC0-1.0",
    "redistribution-permitted",
};

const char *jw_theme_package_reason_slug(jw_theme_reason reason) {
    return (unsigned)reason < JW_THEME_REASON_COUNT ? kReasonSlugs[reason] : "";
}

const char *jw_theme_package_warning_slug(jw_theme_warning warning) {
    return (unsigned)warning < JW_THEME_WARNING_COUNT ? kWarningSlugs[warning] : "";
}

static char jw__tp_ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool jw_theme_package_reserved_name(const char *id) {
    if (!id) return false;
    for (size_t r = 0; r < sizeof(kReservedInstallNames) / sizeof(kReservedInstallNames[0]); r++) {
        const char *reserved = kReservedInstallNames[r];
        size_t i = 0;
        while (id[i] && jw__tp_ascii_lower(id[i]) == jw__tp_ascii_lower(reserved[i])) i++;
        if (!id[i] && !reserved[i]) return true;
    }
    return false;
}

int jw_theme_package_first_reason(const jw_theme_package_result *result) {
    if (!result) return -1;
    for (int i = 0; i < JW_THEME_REASON_COUNT; i++)
        if (result->reasons & (1ULL << i)) return i;
    return -1;
}

static void jw__tp_add(jw_theme_package_result *out, jw_theme_reason reason) {
    out->reasons |= 1ULL << reason;
}

static bool jw__tp_has(const jw_theme_package_result *out, jw_theme_reason reason) {
    return (out->reasons & (1ULL << reason)) != 0;
}

/* ── Text ────────────────────────────────────────────────────────────────── */

/* Strict UTF-8, as Python's bytes.decode("utf-8"): no overlong forms, no
   encoded surrogates, nothing past U+10FFFF. */
static bool jw__tp_utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned c = s[i];
        size_t need;
        unsigned lo = 0x80, hi = 0xBF;
        if (c < 0x80) { i++; continue; }
        else if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c == 0xE0) { need = 2; lo = 0xA0; }
        else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) need = 2;
        else if (c == 0xED) { need = 2; hi = 0x9F; }
        else if (c == 0xF0) { need = 3; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) need = 3;
        else if (c == 0xF4) { need = 3; hi = 0x8F; }
        else return false;
        if (n - i <= need) return false;
        if (s[i + 1] < lo || s[i + 1] > hi) return false;
        for (size_t k = 2; k <= need; k++)
            if (s[i + k] < 0x80 || s[i + k] > 0xBF) return false;
        i += need + 1;
    }
    return true;
}

/* Decode one code point from generalized UTF-8 (lone surrogates allowed, as
   the manifest parser stores them). Input is already known to be well formed. */
static unsigned jw__tp_next_cp(const unsigned char *s, size_t n, size_t *i) {
    unsigned c = s[*i];
    if (c < 0x80 || *i + 1 >= n) { (*i)++; return c; }
    if (c < 0xE0) {
        unsigned cp = ((c & 0x1Fu) << 6) | (s[*i + 1] & 0x3Fu);
        *i += 2;
        return cp;
    }
    if (c < 0xF0 && *i + 2 < n) {
        unsigned cp = ((c & 0x0Fu) << 12) | ((s[*i + 1] & 0x3Fu) << 6) | (s[*i + 2] & 0x3Fu);
        *i += 3;
        return cp;
    }
    if (*i + 3 < n) {
        unsigned cp = ((c & 0x07u) << 18) | ((s[*i + 1] & 0x3Fu) << 12) |
                      ((s[*i + 2] & 0x3Fu) << 6) | (s[*i + 3] & 0x3Fu);
        *i += 4;
        return cp;
    }
    (*i)++;
    return c;
}

/* Python's str.isspace, which is what str.strip() removes. */
static bool jw__tp_py_space(unsigned cp) {
    return (cp >= 0x09 && cp <= 0x0D) || (cp >= 0x1C && cp <= 0x20) ||
           cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
           (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
           cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

/* ── theme.json: a strict JSON reader ────────────────────────────────────── */

/* cJSON cannot say whether `3` was written `3.0`, keeps duplicate keys
   silently, and rejects a lone \ud800 escape that Python keeps. THEME-1 turns
   on all three, so theme.json gets a small reader of its own that follows
   Python's json module exactly. Strings are stored as generalized UTF-8: a
   lone surrogate escape is kept as its three-byte form and flagged. */

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } jw__jv_kind;

typedef struct {
    jw__jv_kind kind;
    bool is_int;           /* JV_NUM written without a fraction or exponent */
    bool lone_surrogate;   /* JV_STR holds an unpaired \uD800-\uDFFF escape */
    double num;
    size_t str;            /* JV_STR: offset into the string arena */
    size_t str_len;
    size_t cps;            /* JV_STR: code points */
    size_t key;            /* member of a JV_OBJ: key offset in the arena */
    size_t key_len;
    int first_child;       /* JV_ARR/JV_OBJ: first member, -1 when empty */
    int next;              /* next sibling, -1 at the end */
} jw__jv;

typedef struct {
    const unsigned char *src;
    size_t len, pos;
    jw__jv *nodes;
    int count, cap;
    char *arena;
    size_t arena_len, arena_cap;
    bool failed;
} jw__jp;

static int jw__jp_node(jw__jp *p, jw__jv_kind kind) {
    if (p->count == p->cap) {
        int next = p->cap ? p->cap * 2 : 64;
        jw__jv *grown = realloc(p->nodes, (size_t)next * sizeof(*grown));
        if (!grown) { p->failed = true; return -1; }
        p->nodes = grown;
        p->cap = next;
    }
    jw__jv *v = &p->nodes[p->count];
    memset(v, 0, sizeof(*v));
    v->kind = kind;
    v->first_child = -1;
    v->next = -1;
    return p->count++;
}

static void jw__jp_ws(jw__jp *p) {
    while (p->pos < p->len) {
        unsigned char c = p->src[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        p->pos++;
    }
}

static void jw__jp_put(jw__jp *p, const unsigned char *bytes, size_t n) {
    /* The arena is sized from the input, which no decoding can outgrow. */
    memcpy(p->arena + p->arena_len, bytes, n);
    p->arena_len += n;
}

static int jw__jp_hex4(jw__jp *p, size_t at) {
    if (at + 4 > p->len) return -1;
    int v = 0;
    for (size_t k = 0; k < 4; k++) {
        unsigned char c = p->src[at + k];
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = (v << 4) | d;
    }
    return v;
}

static void jw__jp_put_cp(jw__jp *p, unsigned cp) {
    unsigned char b[4];
    size_t n;
    if (cp < 0x80)        { b[0] = (unsigned char)cp; n = 1; }
    else if (cp < 0x800)  { b[0] = (unsigned char)(0xC0 | (cp >> 6)); b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        b[0] = (unsigned char)(0xF0 | (cp >> 18));
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    jw__jp_put(p, b, n);
}

/* Parses a string starting at the opening quote. */
static bool jw__jp_string(jw__jp *p, size_t *off, size_t *len, size_t *cps, bool *lone) {
    p->pos++;
    *off = p->arena_len;
    *cps = 0;
    *lone = false;
    while (p->pos < p->len) {
        unsigned char c = p->src[p->pos];
        if (c == '"') {
            p->pos++;
            *len = p->arena_len - *off;
            p->arena[p->arena_len++] = '\0';
            return true;
        }
        if (c < 0x20) return false;
        if (c != '\\') {
            if ((c & 0xC0) != 0x80) (*cps)++;
            jw__jp_put(p, &c, 1);
            p->pos++;
            continue;
        }
        if (p->pos + 1 >= p->len) return false;
        unsigned char e = p->src[p->pos + 1];
        unsigned char simple = 0;
        switch (e) {
            case '"':  simple = '"';  break;
            case '\\': simple = '\\'; break;
            case '/':  simple = '/';  break;
            case 'b':  simple = '\b'; break;
            case 'f':  simple = '\f'; break;
            case 'n':  simple = '\n'; break;
            case 'r':  simple = '\r'; break;
            case 't':  simple = '\t'; break;
            case 'u':  break;
            default:   return false;
        }
        if (simple) {
            jw__jp_put(p, &simple, 1);
            (*cps)++;
            p->pos += 2;
            continue;
        }
        int u = jw__jp_hex4(p, p->pos + 2);
        if (u < 0) return false;
        p->pos += 6;
        unsigned cp = (unsigned)u;
        if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 1 < p->len &&
            p->src[p->pos] == '\\' && p->src[p->pos + 1] == 'u') {
            int low = jw__jp_hex4(p, p->pos + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned)low - 0xDC00);
                p->pos += 6;
            }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) *lone = true;
        jw__jp_put_cp(p, cp);
        (*cps)++;
    }
    return false;
}

static int jw__jp_value(jw__jp *p, int depth);

static int jw__jp_number(jw__jp *p) {
    size_t start = p->pos;
    bool is_int = true;
    if (p->src[p->pos] == '-') p->pos++;
    if (p->pos >= p->len) return -1;
    if (p->src[p->pos] == '0') {
        p->pos++;
    } else if (p->src[p->pos] >= '1' && p->src[p->pos] <= '9') {
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') p->pos++;
    } else {
        return -1;
    }
    /* A '.' or exponent that is not followed by digits is left unconsumed, as
       Python's scanner does; whatever follows then fails as extra data. */
    if (p->pos + 1 < p->len && p->src[p->pos] == '.' &&
        p->src[p->pos + 1] >= '0' && p->src[p->pos + 1] <= '9') {
        is_int = false;
        p->pos++;
        while (p->pos < p->len && p->src[p->pos] >= '0' && p->src[p->pos] <= '9') p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        size_t e = p->pos + 1;
        if (e < p->len && (p->src[e] == '+' || p->src[e] == '-')) e++;
        if (e < p->len && p->src[e] >= '0' && p->src[e] <= '9') {
            is_int = false;
            while (e < p->len && p->src[e] >= '0' && p->src[e] <= '9') e++;
            p->pos = e;
        }
    }
    /* No Leaf binary calls setlocale, so strtod reads '.' as JSON does. */
    size_t n = p->pos - start;
    char *lexeme = malloc(n + 1);
    if (!lexeme) {
        p->failed = true;
        return -1;
    }
    memcpy(lexeme, p->src + start, n);
    lexeme[n] = '\0';
    double value = strtod(lexeme, NULL);
    free(lexeme);
    int idx = jw__jp_node(p, JV_NUM);
    if (idx < 0) return -1;
    p->nodes[idx].num = value;
    p->nodes[idx].is_int = is_int;
    return idx;
}

static bool jw__jp_literal(jw__jp *p, const char *word) {
    size_t n = strlen(word);
    if (p->len - p->pos < n || memcmp(p->src + p->pos, word, n) != 0) return false;
    p->pos += n;
    return true;
}

static int jw__jp_container(jw__jp *p, int depth, bool object) {
    int idx = jw__jp_node(p, object ? JV_OBJ : JV_ARR);
    if (idx < 0) return -1;
    p->pos++;
    jw__jp_ws(p);
    if (p->pos < p->len && p->src[p->pos] == (object ? '}' : ']')) {
        p->pos++;
        return idx;
    }
    int last = -1;
    for (;;) {
        size_t key = 0, key_len = 0;
        if (object) {
            size_t cps;
            bool lone;
            if (p->pos >= p->len || p->src[p->pos] != '"' ||
                !jw__jp_string(p, &key, &key_len, &cps, &lone))
                return -1;
            jw__jp_ws(p);
            if (p->pos >= p->len || p->src[p->pos] != ':') return -1;
            p->pos++;
            jw__jp_ws(p);
        }
        int child = jw__jp_value(p, depth + 1);
        if (child < 0) return -1;
        if (object) {
            /* Duplicate keys at any depth: the device's cJSON keeps the first
               and most parsers keep the last, so they would read different
               themes. */
            for (int m = p->nodes[idx].first_child; m >= 0; m = p->nodes[m].next) {
                if (p->nodes[m].key_len == key_len &&
                    memcmp(p->arena + p->nodes[m].key, p->arena + key, key_len) == 0)
                    return -1;
            }
            p->nodes[child].key = key;
            p->nodes[child].key_len = key_len;
        }
        if (last < 0) p->nodes[idx].first_child = child;
        else          p->nodes[last].next = child;
        last = child;
        jw__jp_ws(p);
        if (p->pos >= p->len) return -1;
        if (p->src[p->pos] == ',') {
            p->pos++;
            jw__jp_ws(p);
            continue;
        }
        if (p->src[p->pos] == (object ? '}' : ']')) {
            p->pos++;
            return idx;
        }
        return -1;
    }
}

static int jw__jp_value(jw__jp *p, int depth) {
    if (depth > JW_TP_JSON_MAX_DEPTH || p->pos >= p->len) return -1;
    unsigned char c = p->src[p->pos];
    if (c == '{') return jw__jp_container(p, depth, true);
    if (c == '[') return jw__jp_container(p, depth, false);
    if (c == '"') {
        int idx = jw__jp_node(p, JV_STR);
        if (idx < 0) return -1;
        size_t off, len, cps;
        bool lone;
        if (!jw__jp_string(p, &off, &len, &cps, &lone)) return -1;
        p->nodes[idx].str = off;
        p->nodes[idx].str_len = len;
        p->nodes[idx].cps = cps;
        p->nodes[idx].lone_surrogate = lone;
        return idx;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return jw__jp_number(p);
    if (jw__jp_literal(p, "true") || jw__jp_literal(p, "false")) {
        int idx = jw__jp_node(p, JV_BOOL);
        return idx;
    }
    if (jw__jp_literal(p, "null")) return jw__jp_node(p, JV_NULL);
    return -1;   /* NaN, Infinity and anything else */
}

/* ── theme.json: field rules ─────────────────────────────────────────────── */

static int jw__jv_member(const jw__jp *p, int obj, const char *key) {
    size_t n = strlen(key);
    for (int m = p->nodes[obj].first_child; m >= 0; m = p->nodes[m].next)
        if (p->nodes[m].key_len == n && memcmp(p->arena + p->nodes[m].key, key, n) == 0)
            return m;
    return -1;
}

static const char *jw__jv_str(const jw__jp *p, int v) {
    return p->arena + p->nodes[v].str;
}

static bool jw__jv_key_in(const jw__jp *p, int m, const char *const *keys, size_t count) {
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(keys[i]);
        if (p->nodes[m].key_len == n && memcmp(p->arena + p->nodes[m].key, keys[i], n) == 0)
            return true;
    }
    return false;
}

static bool jw__jv_is_int(const jw__jp *p, int v, double lo, double hi) {
    return v >= 0 && p->nodes[v].kind == JV_NUM && p->nodes[v].is_int &&
           p->nodes[v].num >= lo && p->nodes[v].num <= hi;
}

/* _text_ok: 1..max_chars characters, at most max_bytes of UTF-8, no control
   characters (except line feed where allowed), not only whitespace. */
static bool jw__jv_text_ok(const jw__jp *p, int v, size_t max_chars, size_t max_bytes,
                           bool allow_newline, bool allow_empty) {
    if (v < 0 || p->nodes[v].kind != JV_STR || p->nodes[v].lone_surrogate) return false;
    const unsigned char *s = (const unsigned char *)jw__jv_str(p, v);
    size_t n = p->nodes[v].str_len;
    if (p->nodes[v].cps > max_chars || (!allow_empty && p->nodes[v].cps == 0) ||
        (max_bytes && n > max_bytes))
        return false;
    bool all_space = true;
    for (size_t i = 0; i < n;) {
        unsigned cp = jw__tp_next_cp(s, n, &i);
        if ((cp < 0x20 && !(allow_newline && cp == '\n')) || cp == 0x7F) return false;
        if (!jw__tp_py_space(cp)) all_space = false;
    }
    return allow_empty || !all_space;
}

static bool jw__tp_ascii_match(const char *s, size_t n, bool (*first)(char),
                               bool (*rest)(char), size_t min, size_t max) {
    if (n < min || n > max || !first(s[0])) return false;
    for (size_t i = 1; i < n; i++)
        if (!rest(s[i])) return false;
    return true;
}

static bool jw__tp_id_first(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
static bool jw__tp_id_rest(char c)  { return jw__tp_id_first(c) || c == '-'; }
static bool jw__tp_sys_char(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static bool jw__tp_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* MAJOR.MINOR.PATCH, each 0-9999 with no leading zeros. */
static bool jw__tp_parse_version(const char *s, size_t n, int out[3]) {
    size_t i = 0;
    for (int part = 0; part < 3; part++) {
        size_t start = i;
        int value = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            value = value * 10 + (s[i] - '0');
            i++;
        }
        size_t digits = i - start;
        if (digits == 0 || digits > 4 || (digits > 1 && s[start] == '0')) return false;
        out[part] = value;
        if (part < 2) {
            if (i >= n || s[i] != '.') return false;
            i++;
        }
    }
    return i == n;
}

static void jw__tp_copy_field(char *dst, size_t dst_size, const jw__jp *p, int v) {
    if (v < 0 || p->nodes[v].kind != JV_STR || p->nodes[v].str_len >= dst_size) return;
    memcpy(dst, jw__jv_str(p, v), p->nodes[v].str_len + 1);
}

static void jw__tp_manifest_fields(const jw__jp *p, int root, jw_theme_package_result *out) {
    const jw__jv *obj = &p->nodes[root];
    int schema = jw__jv_member(p, root, "schema");
    /* A newer schema is refused, never guessed at: nothing else is judged. */
    if (schema < 0 || p->nodes[schema].kind != JV_NUM || p->nodes[schema].num != 1.0) {
        jw__tp_add(out, JW_THEME_UNKNOWN_SCHEMA);
        return;
    }
    static const char *const manifest_keys[] = {
        "schema", "id", "name", "author", "version", "min_leaf_version",
        "license", "description", "grid", "colors", "status_style",
    };
    for (int m = obj->first_child; m >= 0; m = p->nodes[m].next)
        if (!jw__jv_key_in(p, m, manifest_keys, sizeof(manifest_keys) / sizeof(manifest_keys[0])))
            jw__tp_add(out, JW_THEME_UNKNOWN_FIELD);

    int id = jw__jv_member(p, root, "id");
    if (id < 0 || p->nodes[id].kind != JV_STR ||
        !jw__tp_ascii_match(jw__jv_str(p, id), p->nodes[id].str_len, jw__tp_id_first,
                            jw__tp_id_rest, 2, JW_THEME_ID_MAX))
        jw__tp_add(out, JW_THEME_ID_INVALID);
    else
        jw__tp_copy_field(out->id, sizeof(out->id), p, id);

    int name = jw__jv_member(p, root, "name");
    if (!jw__jv_text_ok(p, name, 40, 95, false, false)) jw__tp_add(out, JW_THEME_NAME_INVALID);
    else jw__tp_copy_field(out->name, sizeof(out->name), p, name);

    int author = jw__jv_member(p, root, "author");
    if (!jw__jv_text_ok(p, author, 60, 63, false, false)) jw__tp_add(out, JW_THEME_AUTHOR_INVALID);
    else jw__tp_copy_field(out->author, sizeof(out->author), p, author);

    int parsed[3];
    int version = jw__jv_member(p, root, "version");
    if (version < 0 || p->nodes[version].kind != JV_STR ||
        !jw__tp_parse_version(jw__jv_str(p, version), p->nodes[version].str_len, parsed))
        jw__tp_add(out, JW_THEME_VERSION_INVALID);
    else
        jw__tp_copy_field(out->version, sizeof(out->version), p, version);

    int minimum = jw__jv_member(p, root, "min_leaf_version");
    if (minimum < 0 || p->nodes[minimum].kind != JV_STR ||
        !jw__tp_parse_version(jw__jv_str(p, minimum), p->nodes[minimum].str_len, parsed) ||
        (parsed[0] == 0 && parsed[1] < 12))
        jw__tp_add(out, JW_THEME_MIN_LEAF_VERSION);
    else
        jw__tp_copy_field(out->min_leaf_version, sizeof(out->min_leaf_version), p, minimum);

    int license = jw__jv_member(p, root, "license");
    bool license_ok = false;
    if (license >= 0 && p->nodes[license].kind == JV_STR) {
        for (size_t i = 0; i < sizeof(kLicenses) / sizeof(kLicenses[0]); i++)
            if (p->nodes[license].str_len == strlen(kLicenses[i]) &&
                strcmp(jw__jv_str(p, license), kLicenses[i]) == 0)
                license_ok = true;
    }
    if (!license_ok) jw__tp_add(out, JW_THEME_UNKNOWN_LICENSE);
    else jw__tp_copy_field(out->license, sizeof(out->license), p, license);

    int description = jw__jv_member(p, root, "description");
    if (description >= 0 && !jw__jv_text_ok(p, description, 300, 0, true, true))
        jw__tp_add(out, JW_THEME_DESCRIPTION_INVALID);

    int grid = jw__jv_member(p, root, "grid");
    if (grid >= 0) {
        if (p->nodes[grid].kind != JV_OBJ) {
            jw__tp_add(out, JW_THEME_GRID_INVALID);
        } else {
            static const char *const grid_keys[] = { "cols", "rows" };
            for (int m = p->nodes[grid].first_child; m >= 0; m = p->nodes[m].next)
                if (!jw__jv_key_in(p, m, grid_keys, 2)) jw__tp_add(out, JW_THEME_UNKNOWN_FIELD);
            if (!jw__jv_is_int(p, jw__jv_member(p, grid, "cols"), 1, 8) ||
                !jw__jv_is_int(p, jw__jv_member(p, grid, "rows"), 1, 6))
                jw__tp_add(out, JW_THEME_GRID_INVALID);
        }
    }

    int colors = jw__jv_member(p, root, "colors");
    if (colors >= 0) {
        if (p->nodes[colors].kind != JV_OBJ) {
            jw__tp_add(out, JW_THEME_COLOR_INVALID);
        } else {
            static const char *const color_keys[] = {
                "text", "highlight", "highlight_text", "underlay", "tile_border", "focus_ring",
            };
            static const char *const level_keys[] = { "underlay_opacity", "shadow" };
            for (int m = p->nodes[colors].first_child; m >= 0; m = p->nodes[m].next) {
                if (jw__jv_key_in(p, m, color_keys, 6)) {
                    const char *s = p->nodes[m].kind == JV_STR ? jw__jv_str(p, m) : NULL;
                    size_t n = s ? p->nodes[m].str_len : 0;
                    bool ok = s && (n == 7 || n == 9) && s[0] == '#';
                    for (size_t i = 1; ok && i < n; i++) ok = jw__tp_hex(s[i]);
                    if (!ok) jw__tp_add(out, JW_THEME_COLOR_INVALID);
                } else if (jw__jv_key_in(p, m, level_keys, 2)) {
                    if (!jw__jv_is_int(p, m, 0, 255)) jw__tp_add(out, JW_THEME_COLOR_LEVEL_INVALID);
                } else {
                    jw__tp_add(out, JW_THEME_UNKNOWN_FIELD);
                }
            }
        }
    }

    int style = jw__jv_member(p, root, "status_style");
    if (style >= 0) {
        const char *s = p->nodes[style].kind == JV_STR ? jw__jv_str(p, style) : NULL;
        size_t n = s ? p->nodes[style].str_len : 0;
        if (!s || !((n == 4 && strcmp(s, "auto") == 0) || (n == 5 && strcmp(s, "light") == 0) ||
                    (n == 4 && strcmp(s, "dark") == 0)))
            jw__tp_add(out, JW_THEME_STATUS_STYLE_INVALID);
    }
}

void jw_theme_package_check_manifest(const unsigned char *data, size_t len,
                                     jw_theme_package_result *out) {
    if (!out) return;
    if (len > JW_TP_MAX_MANIFEST) {
        jw__tp_add(out, JW_THEME_MANIFEST_TOO_LARGE);
        return;
    }
    if (!data || (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) ||
        !jw__tp_utf8_valid(data, len)) {
        jw__tp_add(out, JW_THEME_MALFORMED_MANIFEST);
        return;
    }
    jw__jp p;
    memset(&p, 0, sizeof(p));
    p.src = data;
    p.len = len;
    p.arena_cap = len + 1;
    p.arena = malloc(p.arena_cap);
    if (!p.arena) {
        jw__tp_add(out, JW_THEME_MALFORMED_MANIFEST);
        return;
    }
    jw__jp_ws(&p);
    int root = jw__jp_value(&p, 0);
    jw__jp_ws(&p);
    if (root < 0 || p.failed || p.pos != p.len || p.nodes[root].kind != JV_OBJ)
        jw__tp_add(out, JW_THEME_MALFORMED_MANIFEST);
    else
        jw__tp_manifest_fields(&p, root, out);
    free(p.nodes);
    free(p.arena);
}

/* ── The zip container ───────────────────────────────────────────────────── */

typedef enum {
    JW_TP_SLOT_NONE = 0,   /* set aside or refused: never extracted */
    JW_TP_SLOT_DIR,
    JW_TP_SLOT_MANIFEST,
    JW_TP_SLOT_PREVIEW,
    JW_TP_SLOT_LICENSE,
    JW_TP_SLOT_WALLPAPER,
    JW_TP_SLOT_ART,
} jw__tp_slot;

typedef struct {
    const unsigned char *name;
    size_t name_len;
    unsigned flags, method, made_by;
    unsigned long crc, csize, usize, external, offset;
    size_t data_start;
    int order;              /* central directory position */
    bool is_dir;
    bool clean;             /* passed the per-entry name rules and duplicates */
    jw__tp_slot slot;
    bool icon;              /* an ART slot in an icons folder */
} jw__tp_entry;

static unsigned jw__le16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned long jw__le32(const unsigned char *p) {
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static int jw__tp_by_offset(const void *a, const void *b) {
    const jw__tp_entry *const *ea = a, *const *eb = b;
    if ((*ea)->offset != (*eb)->offset) return (*ea)->offset < (*eb)->offset ? -1 : 1;
    return (*ea)->order - (*eb)->order;
}

/* Stage 1. Returns the entry count, or a negative reason (-1 - reason). */
static int jw__tp_read_directory(const unsigned char *blob, size_t size,
                                 jw__tp_entry **out_entries) {
    *out_entries = NULL;
    long eocd = -1;
    if (size >= 22) {
        size_t lowest = size - 22 > 65535 ? size - 22 - 65535 : 0;
        for (size_t pos = size - 22 + 1; pos-- > lowest;) {
            if (memcmp(blob + pos, "PK\x05\x06", 4) == 0 &&
                pos + 22 + jw__le16(blob + pos + 20) == size) {
                eocd = (long)pos;
                break;
            }
        }
    }
    if (eocd < 0) return -1 - JW_THEME_MALFORMED_ARCHIVE;
    const unsigned char *e = blob + eocd;
    unsigned disk = jw__le16(e + 4), cd_disk = jw__le16(e + 6);
    unsigned on_disk = jw__le16(e + 8), total = jw__le16(e + 10);
    unsigned long cd_size = jw__le32(e + 12), cd_offset = jw__le32(e + 16);
    /* ZIP64 and multi-volume archives are never needed under these limits. */
    if (disk || cd_disk || on_disk != total || total == 0xFFFF ||
        cd_offset == 0xFFFFFFFFUL || (unsigned long long)cd_offset + cd_size != (unsigned long long)eocd)
        return -1 - JW_THEME_MALFORMED_ARCHIVE;
    if (total > JW_TP_MAX_ENTRIES) return -1 - JW_THEME_TOO_MANY_ENTRIES;

    jw__tp_entry *entries = calloc(total ? total : 1, sizeof(*entries));
    jw__tp_entry **ordered = calloc(total ? total : 1, sizeof(*ordered));
    if (!entries || !ordered) {
        free(entries);
        free(ordered);
        return -1 - JW_THEME_REASON_COUNT;   /* out of memory: no verdict */
    }
    int rc = -1 - JW_THEME_MALFORMED_ARCHIVE;
    size_t pos = cd_offset;
    for (unsigned i = 0; i < total; i++) {
        if (pos + 46 > (size_t)eocd || memcmp(blob + pos, "PK\x01\x02", 4) != 0) goto done;
        const unsigned char *c = blob + pos;
        jw__tp_entry *entry = &entries[i];
        entry->made_by = jw__le16(c + 4);
        entry->flags = jw__le16(c + 8);
        entry->method = jw__le16(c + 10);
        entry->crc = jw__le32(c + 16);
        entry->csize = jw__le32(c + 20);
        entry->usize = jw__le32(c + 24);
        size_t name_len = jw__le16(c + 28), extra_len = jw__le16(c + 30), comment_len = jw__le16(c + 32);
        unsigned disk_start = jw__le16(c + 34);
        entry->external = jw__le32(c + 38);
        entry->offset = jw__le32(c + 42);
        size_t end = pos + 46 + name_len + extra_len + comment_len;
        if (end > (size_t)eocd || disk_start || entry->csize == 0xFFFFFFFFUL ||
            entry->usize == 0xFFFFFFFFUL || entry->offset == 0xFFFFFFFFUL)
            goto done;
        entry->name = c + 46;
        entry->name_len = name_len;
        entry->order = (int)i;
        entry->is_dir = name_len > 0 && entry->name[name_len - 1] == '/';
        if (entry->is_dir && entry->usize) goto done;
        ordered[i] = entry;
        pos = end;
    }
    if (pos != (size_t)eocd) goto done;

    /* Local headers repeat each central name and method and tile the file from
       offset 0 without overlapping. Overlap is how one stored payload is
       counted many times by a zip bomb. */
    qsort(ordered, total, sizeof(*ordered), jw__tp_by_offset);
    if (total && ordered[0]->offset != 0) goto done;
    unsigned long long expected = 0;
    for (unsigned i = 0; i < total; i++) {
        jw__tp_entry *entry = ordered[i];
        unsigned long long at = entry->offset;
        if (at < expected || at + 30 > cd_offset || memcmp(blob + at, "PK\x03\x04", 4) != 0)
            goto done;
        unsigned method = jw__le16(blob + at + 8);
        size_t name_len = jw__le16(blob + at + 26), extra_len = jw__le16(blob + at + 28);
        if (method != entry->method || name_len != entry->name_len ||
            at + 30 + name_len > size || memcmp(blob + at + 30, entry->name, name_len) != 0)
            goto done;
        entry->data_start = (size_t)(at + 30 + name_len + extra_len);
        expected = (unsigned long long)entry->data_start + entry->csize;
        if (expected > cd_offset) goto done;
    }
    rc = (int)total;
done:
    free(ordered);
    if (rc < 0) free(entries);
    else *out_entries = entries;
    return rc;
}

/* The entry's bytes, never more than it declared. NULL when the data does not
   decompress to exactly its declared size and CRC-32. */
static unsigned char *jw__tp_inflate(const unsigned char *blob, const jw__tp_entry *entry) {
    const unsigned char *payload = blob + entry->data_start;
    unsigned char *data = malloc(entry->usize + 1);
    if (!data) return NULL;
    size_t got;
    if (entry->method == 0) {
        if (entry->csize != entry->usize) { free(data); return NULL; }
        memcpy(data, payload, entry->usize);
        got = entry->usize;
    } else {
        got = tinfl_decompress_mem_to_mem(data, entry->usize + 1, payload, entry->csize, 0);
        if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) { free(data); return NULL; }
    }
    if (got != entry->usize ||
        mz_crc32(MZ_CRC32_INIT, data, got) != entry->crc) {
        free(data);
        return NULL;
    }
    return data;
}

/* ── Entry names and the allowlist ───────────────────────────────────────── */

#define JW_TP_S_IFMT  0170000UL
#define JW_TP_S_IFREG 0100000UL
#define JW_TP_S_IFDIR 0040000UL
#define JW_TP_S_IFLNK 0120000UL

/* The first per-entry rule this name breaks, in contract order, or -1. */
static int jw__tp_name_reason(const jw__tp_entry *entry) {
    const unsigned char *s = entry->name;
    size_t n = entry->name_len;
    if (n == 0 || !jw__tp_utf8_valid(s, n)) return JW_THEME_ENTRY_NAME_ENCODING;
    for (size_t i = 0; i < n; i++)
        if (s[i] < 0x20 || s[i] == 0x7F) return JW_THEME_ENTRY_NAME_ENCODING;
    if (s[0] == '/' ||
        (n >= 2 && ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':'))
        return JW_THEME_ABSOLUTE_PATH;
    if (memchr(s, '\\', n)) return JW_THEME_BACKSLASH_PATH;
    size_t trimmed = n;
    while (trimmed > 0 && s[trimmed - 1] == '/') trimmed--;
    bool traversal = false, hidden = false;
    for (size_t start = 0; start <= trimmed;) {
        const unsigned char *slash = memchr(s + start, '/', trimmed - start);
        size_t end = slash ? (size_t)(slash - s) : trimmed;
        size_t len = end - start;
        if ((len == 1 && s[start] == '.') || (len == 2 && s[start] == '.' && s[start + 1] == '.'))
            traversal = true;
        if ((len > 0 && s[start] == '.') || (len == 8 && memcmp(s + start, "__MACOSX", 8) == 0))
            hidden = true;
        start = end + 1;
    }
    if (traversal) return JW_THEME_PATH_TRAVERSAL;
    if (hidden) return JW_THEME_HIDDEN_FILE;
    if ((entry->made_by >> 8) == 3) {   /* Unix: the high 16 bits are st_mode */
        unsigned long kind = (entry->external >> 16) & JW_TP_S_IFMT;
        if (kind == JW_TP_S_IFLNK) return JW_THEME_SYMLINK;
        if ((kind != 0 && kind != JW_TP_S_IFREG && kind != JW_TP_S_IFDIR) ||
            (kind == JW_TP_S_IFDIR && !entry->is_dir) || (kind == JW_TP_S_IFREG && entry->is_dir))
            return JW_THEME_SPECIAL_FILE;
    }
    return -1;
}

static bool jw__tp_eq(const char *s, size_t n, const char *lit) {
    return strlen(lit) == n && memcmp(s, lit, n) == 0;
}

static bool jw__tp_is_wallpaper_name(const char *s, size_t n) {
    return jw__tp_eq(s, n, "wallpaper.png") || jw__tp_eq(s, n, "wallpaper.jpg") ||
           jw__tp_eq(s, n, "wallpaper.jpeg");
}

/* Classify a root-relative path. Returns -1 with the slot filled, or a reason. */
static int jw__tp_classify(const char *rel, size_t n, bool is_dir, jw__tp_entry *entry,
                           int *wallpaper_view) {
    *wallpaper_view = -1;
    if (is_dir) {
        while (n > 0 && rel[n - 1] == '/') n--;
        static const char *const dirs[] = {
            "", "icons", "grid", "coverflow", "grid/icons", "grid/labels",
            "grid/wordmarks", "coverflow/icons",
        };
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
            if (jw__tp_eq(rel, n, dirs[i])) { entry->slot = JW_TP_SLOT_DIR; return -1; }
        return JW_THEME_UNKNOWN_FILE;
    }
    if (jw__tp_eq(rel, n, "theme.json"))  { entry->slot = JW_TP_SLOT_MANIFEST; return -1; }
    if (jw__tp_eq(rel, n, "preview.png")) { entry->slot = JW_TP_SLOT_PREVIEW;  return -1; }
    if (jw__tp_eq(rel, n, "LICENSE.txt")) { entry->slot = JW_TP_SLOT_LICENSE;  return -1; }

    const char *parts[3] = { rel, NULL, NULL };
    size_t lens[3] = { 0, 0, 0 };
    int count = 1;
    size_t start = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i == n || rel[i] == '/') {
            if (count > 3) return JW_THEME_UNKNOWN_FILE;
            lens[count - 1] = i - start;
            if (i < n) {
                if (count == 3) return JW_THEME_UNKNOWN_FILE;
                parts[count++] = rel + i + 1;
                start = i + 1;
            }
        }
    }
    if (count == 1 && jw__tp_is_wallpaper_name(parts[0], lens[0])) {
        entry->slot = JW_TP_SLOT_WALLPAPER;
        *wallpaper_view = 0;
        return -1;
    }
    if (count == 2 && jw__tp_eq(parts[0], lens[0], "grid") &&
        jw__tp_is_wallpaper_name(parts[1], lens[1])) {
        entry->slot = JW_TP_SLOT_WALLPAPER;
        *wallpaper_view = 1;
        return -1;
    }
    /* icons/<ID>.png at the root is the set both views share. */
    bool shared = count == 2 && jw__tp_eq(parts[0], lens[0], "icons");
    if (!shared && count != 3) return JW_THEME_UNKNOWN_FILE;
    bool grid = !shared && jw__tp_eq(parts[0], lens[0], "grid");
    bool coverflow = !shared && jw__tp_eq(parts[0], lens[0], "coverflow");
    bool icons = shared || jw__tp_eq(parts[1], lens[1], "icons");
    bool wordmarks = !shared && jw__tp_eq(parts[1], lens[1], "wordmarks");
    bool labels = !shared && jw__tp_eq(parts[1], lens[1], "labels");
    if (!(shared || (grid && (icons || wordmarks || labels)) || (coverflow && icons)))
        return JW_THEME_UNKNOWN_FILE;

    const char *stem = shared ? parts[1] : parts[2];
    size_t stem_len = shared ? lens[1] : lens[2];
    if (wordmarks && stem_len >= 10 && memcmp(stem + stem_len - 10, ".color.png", 10) == 0)
        stem_len -= 10;
    else if (stem_len >= 4 && memcmp(stem + stem_len - 4, ".png", 4) == 0)
        stem_len -= 4;
    else
        return JW_THEME_UNKNOWN_FILE;

    bool is_default = stem_len == 8;
    for (size_t i = 0; is_default && i < 8; i++)
        is_default = jw__tp_ascii_lower(stem[i]) == "_default"[i];
    if (is_default) return JW_THEME_RESERVED_SYSTEM_ID;
    /* The Apps tile: an icon in both views and a label in Grid, never a wordmark. */
    bool apps_tile = (icons || labels) && jw__tp_eq(stem, stem_len, "_apps");
    if (!apps_tile && (stem_len < 2 || stem_len > 32 ||
                       !jw__tp_ascii_match(stem, stem_len, jw__tp_sys_char, jw__tp_sys_char, 2, 32)))
        return JW_THEME_SYSTEM_ID_INVALID;
    entry->slot = JW_TP_SLOT_ART;
    entry->icon = icons;
    return -1;
}

/* ── Extraction ──────────────────────────────────────────────────────────── */

/* Create the missing parents of path below its first `keep` bytes, which name
   the caller's existing extraction directory. Nothing above it is touched. */
static int jw__tp_mkdirs_for(char *path, size_t keep) {
    for (char *p = path + keep + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        int rc = mkdir(path, 0755);
        struct stat st;
        bool ok = rc == 0 || (errno == EEXIST && lstat(path, &st) == 0 && S_ISDIR(st.st_mode));
        *p = '/';
        if (!ok) return -1;
    }
    return 0;
}

static int jw__tp_write_file(const char *path, const unsigned char *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return -1; }
        off += (size_t)n;
    }
    return close(fd) == 0 ? 0 : -1;
}

/* ── Stage 5 on the extracted tree ───────────────────────────────────────── */

static unsigned char *jw__tp_read_file(const char *path, size_t max, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    unsigned char *data = NULL;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long len = ftell(fp);
        if (len >= 0 && (size_t)len <= max && fseek(fp, 0, SEEK_SET) == 0 &&
            (data = malloc((size_t)len + 1)) != NULL) {
            if (fread(data, 1, (size_t)len, fp) != (size_t)len) {
                free(data);
                data = NULL;
            } else {
                *out_len = (size_t)len;
            }
        }
    }
    fclose(fp);
    return data;
}

/* Returns 0, or -1 when the file cannot be read back (no verdict). */
static int jw__tp_check_image(const char *path, const jw__tp_entry *entry, size_t rel_len,
                              const char *rel, jw_theme_package_result *out) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    unsigned w = 0, h = 0;
    bool ok;
    if (rel_len >= 4 && memcmp(rel + rel_len - 4, ".png", 4) == 0) {
        unsigned char header[33];
        size_t got = fread(header, 1, sizeof(header), fp);
        ok = jw_image_png_dims(header, got, true, &w, &h);
    } else {
        ok = jw_image_jpeg_dims(fp, true, &w, &h);
    }
    fclose(fp);
    if (!ok) {
        jw__tp_add(out, JW_THEME_UNSUPPORTED_IMAGE);
        return 0;
    }
    bool in_range;
    if (entry->slot == JW_TP_SLOT_PREVIEW)
        in_range = w == JW_TP_PREVIEW_W && h == JW_TP_PREVIEW_H;
    else if (entry->slot == JW_TP_SLOT_WALLPAPER)
        in_range = w >= 1 && w <= JW_TP_WALLPAPER_MAX_PX && h >= 1 && h <= JW_TP_WALLPAPER_MAX_PX;
    else
        in_range = w >= 1 && w <= JW_TP_ART_MAX_PX && h >= 1 && h <= JW_TP_ART_MAX_PX;
    if (!in_range)
        jw__tp_add(out, JW_THEME_IMAGE_DIMENSIONS);
    else if (entry->icon && (w != JW_TP_ICON_TARGET_PX || h != JW_TP_ICON_TARGET_PX))
        out->warnings |= 1u << JW_THEME_WARN_ICON_OFF_SIZE;
    return 0;
}

int jw_theme_package_validate_zip(const char *zip_path, const char *extract_dir,
                                  jw_theme_package_result *out) {
    if (!zip_path || !extract_dir || !out) return -1;
    memset(out, 0, sizeof(*out));

    int fd = open(zip_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        return -1;
    }
    if (st.st_size > JW_THEME_MAX_ARCHIVE_BYTES) {
        close(fd);
        jw__tp_add(out, JW_THEME_ARCHIVE_TOO_LARGE);
        return 0;
    }
    size_t size = (size_t)st.st_size;
    unsigned char *blob = malloc(size ? size : 1);
    size_t got = 0;
    while (blob && got < size) {
        ssize_t n = read(fd, blob + got, size - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (!blob || got != size) {
        free(blob);
        return -1;
    }

    int rc = -1;
    jw__tp_entry *entries = NULL;
    char *path = NULL;

    /* Stage 1: container structure and entry count. */
    int count = jw__tp_read_directory(blob, size, &entries);
    if (count < 0) {
        int reason = -1 - count;
        if (reason < JW_THEME_REASON_COUNT) {
            jw__tp_add(out, (jw_theme_reason)reason);
            rc = 0;
        }
        goto done;
    }

    /* Stage 2: declared sizes and encodings, before anything is decompressed. */
    unsigned long long total = 0;
    for (int i = 0; i < count; i++) {
        if ((entries[i].method != 0 && entries[i].method != 8) || (entries[i].flags & 1u))
            jw__tp_add(out, JW_THEME_UNSUPPORTED_COMPRESSION);
        if ((unsigned long long)entries[i].usize > JW_TP_MAX_RATIO * entries[i].csize)
            jw__tp_add(out, JW_THEME_COMPRESSION_RATIO);
        total += entries[i].usize;
    }
    if (total > JW_TP_MAX_UNCOMPRESSED) jw__tp_add(out, JW_THEME_UNCOMPRESSED_TOO_LARGE);
    if (out->reasons) {
        rc = 0;
        goto done;
    }

    /* Stage 3: integrity. Every byte is bounded by stage 2 now. */
    for (int i = 0; i < count; i++) {
        unsigned char *data = jw__tp_inflate(blob, &entries[i]);
        if (!data) {
            jw__tp_add(out, JW_THEME_MALFORMED_ARCHIVE);
            rc = 0;
            goto done;
        }
        free(data);
    }

    /* Stage 4: names, types, duplicates, layout. */
    for (int i = 0; i < count; i++) {
        jw__tp_entry *entry = &entries[i];
        int reason = jw__tp_name_reason(entry);
        if (reason >= 0) {
            jw__tp_add(out, (jw_theme_reason)reason);
            continue;
        }
        size_t key_len = entry->name_len;
        while (key_len > 0 && entry->name[key_len - 1] == '/') key_len--;
        bool duplicate = false;
        /* FAT32 folds case, so two names that differ only in case are one file.
           ASCII folding decides every name the allowlist can accept. */
        for (int j = 0; j < i && !duplicate; j++) {
            const jw__tp_entry *prior = &entries[j];
            if (!prior->clean) continue;
            size_t prior_len = prior->name_len;
            while (prior_len > 0 && prior->name[prior_len - 1] == '/') prior_len--;
            if (prior_len != key_len) continue;
            duplicate = true;
            for (size_t k = 0; k < key_len && duplicate; k++)
                duplicate = jw__tp_ascii_lower((char)prior->name[k]) ==
                            jw__tp_ascii_lower((char)entry->name[k]);
        }
        if (duplicate) {
            jw__tp_add(out, JW_THEME_DUPLICATE_ENTRY);
            continue;
        }
        entry->clean = true;
    }

    size_t root_len = 0;
    const unsigned char *root = NULL;
    bool single = false;
    for (int i = 0; i < count; i++) {
        const jw__tp_entry *entry = &entries[i];
        if (!entry->clean) continue;
        const unsigned char *slash = memchr(entry->name, '/', entry->name_len);
        if (!slash) { single = false; root = NULL; break; }
        size_t len = (size_t)(slash - entry->name);
        if (!root) {
            root = entry->name;
            root_len = len;
            single = true;
        } else if (len != root_len || memcmp(root, entry->name, len) != 0) {
            single = false;
            break;
        }
    }
    /* A folder name too long for any card is reported as the layout failing
       rather than carried further: it could never be extracted. */
    if (!single || root_len >= sizeof(out->root)) {
        jw__tp_add(out, JW_THEME_NOT_SINGLE_FOLDER);
        rc = 0;
        goto done;
    }
    memcpy(out->root, root, root_len);
    out->root[root_len] = '\0';

    int wallpapers[2] = { 0, 0 };
    int manifest_index = -1, preview_index = -1;
    for (int i = 0; i < count; i++) {
        jw__tp_entry *entry = &entries[i];
        if (!entry->clean) continue;
        const char *rel = (const char *)entry->name + root_len + 1;
        size_t rel_len = entry->name_len - root_len - 1;
        int view = -1;
        int reason = jw__tp_classify(rel, rel_len, entry->is_dir, entry, &view);
        if (reason >= 0) {
            entry->slot = JW_TP_SLOT_NONE;
            jw__tp_add(out, (jw_theme_reason)reason);
            continue;
        }
        if (view >= 0) wallpapers[view]++;
        if (entry->slot == JW_TP_SLOT_MANIFEST) manifest_index = i;
        if (entry->slot == JW_TP_SLOT_PREVIEW) preview_index = i;
    }
    if (wallpapers[0] > 1 || wallpapers[1] > 1) jw__tp_add(out, JW_THEME_MULTIPLE_WALLPAPERS);

    /* Extract what the allowlist accepted, and only that. */
    size_t dir_len = strlen(extract_dir);
    size_t path_cap = dir_len + 2 + 0x10000 + sizeof("/theme.json");
    path = malloc(path_cap);
    if (!path) goto done;
    for (int i = 0; i < count; i++) {
        const jw__tp_entry *entry = &entries[i];
        if (!entry->clean || entry->slot == JW_TP_SLOT_NONE || entry->slot == JW_TP_SLOT_DIR)
            continue;
        memcpy(path, extract_dir, dir_len);
        path[dir_len] = '/';
        memcpy(path + dir_len + 1, entry->name, entry->name_len);
        path[dir_len + 1 + entry->name_len] = '\0';
        unsigned char *data = jw__tp_inflate(blob, entry);
        int written = data && jw__tp_mkdirs_for(path, dir_len) == 0 &&
                      jw__tp_write_file(path, data, entry->usize) == 0;
        free(data);
        if (!written) goto done;
    }

    /* Stage 5: contents, read back from the tree just written. */
    if (manifest_index < 0) {
        jw__tp_add(out, JW_THEME_MISSING_MANIFEST);
    } else {
        const jw__tp_entry *entry = &entries[manifest_index];
        if (entry->usize > JW_TP_MAX_MANIFEST) {
            jw__tp_add(out, JW_THEME_MANIFEST_TOO_LARGE);
        } else {
            snprintf(path, path_cap, "%s/%s/theme.json", extract_dir, out->root);
            size_t len = 0;
            unsigned char *data = jw__tp_read_file(path, JW_TP_MAX_MANIFEST, &len);
            if (!data) goto done;
            jw_theme_package_check_manifest(data, len, out);
            free(data);
            if (!jw__tp_has(out, JW_THEME_UNKNOWN_SCHEMA) && !jw__tp_has(out, JW_THEME_ID_INVALID) &&
                !jw__tp_has(out, JW_THEME_MALFORMED_MANIFEST) && out->id[0]) {
                if (strcmp(out->id, out->root) != 0) jw__tp_add(out, JW_THEME_ID_MISMATCH);
                if (jw_theme_package_reserved_name(out->id)) jw__tp_add(out, JW_THEME_RESERVED_NAME);
            }
        }
    }
    if (preview_index < 0) jw__tp_add(out, JW_THEME_MISSING_PREVIEW);

    int art_files = 0;
    for (int i = 0; i < count; i++) {
        const jw__tp_entry *entry = &entries[i];
        if (!entry->clean || (entry->slot != JW_TP_SLOT_PREVIEW &&
                              entry->slot != JW_TP_SLOT_WALLPAPER && entry->slot != JW_TP_SLOT_ART))
            continue;
        if (entry->slot != JW_TP_SLOT_PREVIEW) art_files++;
        snprintf(path, path_cap, "%s/%.*s", extract_dir,
                 (int)entry->name_len, (const char *)entry->name);
        const char *rel = (const char *)entry->name + root_len + 1;
        if (jw__tp_check_image(path, entry, entry->name_len - root_len - 1, rel, out) != 0)
            goto done;
    }
    if (art_files == 0) out->warnings |= 1u << JW_THEME_WARN_NO_ART;
    rc = 0;

done:
    free(path);
    free(entries);
    free(blob);
    return rc;
}
