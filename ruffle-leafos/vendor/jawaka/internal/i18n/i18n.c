#include "internal/i18n/i18n.h"

#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal/core/log.h"

/* Compiled table layout, written by tools/i18n-compile.py. One read, one
 * allocation, no per-lookup allocation and no filesystem access after load --
 * this sits on the render path, and the Display & Sound lag was a lesson in
 * what per-frame work costs there.
 *
 *   magic   "JWI18N\0\0"           8 bytes
 *   u32     version                1
 *   u32     count                  entries
 *   u32     pool_size              bytes of string pool
 *   u32     reserved               0
 *   entry[count]                   { u32 hash; u32 key_off; u32 val_off; }
 *                                  sorted by hash, then by key
 *   pool[pool_size]                NUL-terminated UTF-8
 *
 * All integers little-endian. Both the build host and every supported device
 * are little-endian; the loader checks the magic and every offset rather than
 * trusting the file, so a mismatched or truncated table is rejected, not
 * misread.
 */

#define JW_I18N_MAGIC   "JWI18N\0\0"
#define JW_I18N_VERSION 1u
#define JW_I18N_HEADER  24u

typedef struct {
    uint32_t hash;
    uint32_t key_off;
    uint32_t val_off;
} jw__i18n_entry;

static struct {
    unsigned char  *blob;        /* whole file, owns the pool */
    size_t          blob_size;
    const jw__i18n_entry *entries;
    uint32_t        count;
    const char     *pool;
    uint32_t        pool_size;
    char            lang[JW_I18N_CODE_MAX];
} g_i18n = { NULL, 0, NULL, 0, NULL, 0, "en" };

/* FNV-1a. Chosen for being three lines and dependency-free; the table is a few
 * thousand entries, so distribution matters more than speed and collisions are
 * resolved by strcmp anyway. */
static uint32_t jw__i18n_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}

static uint32_t jw__i18n_rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A key may carry a "context|" prefix so identical English can translate two
 * ways. Display strips it, so an untranslated T("verb|Open") still shows
 * "Open" rather than leaking the disambiguator to the user. Only a prefix
 * without spaces counts, so a literal pipe in real UI text is left alone. */
static const char *jw__i18n_strip_context(const char *key) {
    const char *bar = strchr(key, '|');
    if (!bar || bar == key) return key;
    for (const char *p = key; p < bar; p++) {
        if (*p == ' ' || *p == '\t') return key;
    }
    return bar + 1;
}

static void jw__i18n_reset(void) {
    free(g_i18n.blob);
    g_i18n.blob      = NULL;
    g_i18n.blob_size = 0;
    g_i18n.entries   = NULL;
    g_i18n.count     = 0;
    g_i18n.pool      = NULL;
    g_i18n.pool_size = 0;
    snprintf(g_i18n.lang, sizeof(g_i18n.lang), "%s", "en");
}

static bool jw__i18n_path(char *out, size_t out_size, const char *env,
                          const char *fallback, const char *lang,
                          const char *ext) {
    const char *root = getenv(env);
    if ((!root || !root[0]) && fallback) root = fallback;
    if (!root || !root[0]) return false;
    return snprintf(out, out_size, "%s/i18n/%s.%s", root, lang, ext) < (int)out_size;
}

/* printf-specifier fingerprint of a string, for validating translations of
 * format strings. A translation that turns %d into %s would send snprintf
 * reading an integer as a pointer, so a value whose conversions do not match
 * its key's -- same count, same order, same letters -- is refused and the
 * English format used instead. Length modifiers are kept ("%zu" != "%d");
 * "%%" is skipped. Returns the number of conversions, -1 on overflow. */
static int jw__i18n_fmt_sig(const char *s, char *out, size_t out_size) {
    int n = 0;
    size_t o = 0;
    for (const char *p = s; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%' ) continue;
        while (*p && strchr("-+ #0123456789.*'", *p)) p++;   /* flags/width/prec */
        while (*p && strchr("hlLqjzt", *p)) {                /* length modifiers */
            if (o + 1 >= out_size) return -1;
            out[o++] = *p++;
        }
        if (!*p) break;
        if (o + 1 >= out_size) return -1;
        out[o++] = *p;
        n++;
    }
    out[o] = '\0';
    return n;
}

static bool jw__i18n_fmt_compatible(const char *key, const char *val) {
    char a[32], b[32];
    if (jw__i18n_fmt_sig(key, a, sizeof(a)) < 0) return false;
    if (jw__i18n_fmt_sig(val, b, sizeof(b)) < 0) return false;
    return strcmp(a, b) == 0;
}

/* ── Compiled table ──────────────────────────────────────────────────────── */

static bool jw__i18n_load_compiled(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long size = ftell(fp);
    if (size < (long)JW_I18N_HEADER || size > 64L * 1024 * 1024) {
        fclose(fp);
        return false;
    }
    rewind(fp);

    unsigned char *blob = (unsigned char *)malloc((size_t)size);
    if (!blob) { fclose(fp); return false; }
    size_t got = fread(blob, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) { free(blob); return false; }

    if (memcmp(blob, JW_I18N_MAGIC, 8) != 0 ||
        jw__i18n_rd32(blob + 8) != JW_I18N_VERSION) {
        jw_log_warn("i18n: %s is not a v%u table; ignoring", path, JW_I18N_VERSION);
        free(blob);
        return false;
    }

    uint32_t count     = jw__i18n_rd32(blob + 12);
    uint32_t pool_size = jw__i18n_rd32(blob + 16);

    /* Validate the geometry before trusting a single offset. A truncated table
       must be rejected here, not discovered by reading past the buffer on some
       later lookup. */
    uint64_t need = (uint64_t)JW_I18N_HEADER +
                    (uint64_t)count * sizeof(jw__i18n_entry) + pool_size;
    if (need != (uint64_t)size) {
        jw_log_warn("i18n: %s has inconsistent size; ignoring", path);
        free(blob);
        return false;
    }
    const char *pool = (const char *)(blob + JW_I18N_HEADER +
                                      (size_t)count * sizeof(jw__i18n_entry));
    if (pool_size == 0 || pool[pool_size - 1] != '\0') {
        jw_log_warn("i18n: %s pool is not NUL-terminated; ignoring", path);
        free(blob);
        return false;
    }

    const unsigned char *raw = blob + JW_I18N_HEADER;
    jw__i18n_entry *entries  = (jw__i18n_entry *)raw;
    for (uint32_t i = 0; i < count; i++) {
        const unsigned char *e = raw + (size_t)i * sizeof(jw__i18n_entry);
        uint32_t h = jw__i18n_rd32(e);
        uint32_t k = jw__i18n_rd32(e + 4);
        uint32_t v = jw__i18n_rd32(e + 8);
        if (k >= pool_size || v >= pool_size) {
            jw_log_warn("i18n: %s entry %u points outside the pool; ignoring", path, i);
            free(blob);
            return false;
        }
        entries[i].hash = h;
        entries[i].key_off = k;
        entries[i].val_off = v;
    }

    g_i18n.blob      = blob;
    g_i18n.blob_size = (size_t)size;
    g_i18n.entries   = entries;
    g_i18n.count     = count;
    g_i18n.pool      = pool;
    g_i18n.pool_size = pool_size;
    return true;
}

/* ── Live TSV override ───────────────────────────────────────────────────── */

/* Builds the same in-memory shape as the compiled table so lookup has one code
 * path. Format is "english<TAB>translation", # comments, blank lines skipped --
 * what a translator exports from a spreadsheet. Deliberately forgiving: a bad
 * line is dropped rather than failing the file, because this is a hand-edited
 * file on an SD card and losing the whole translation to one stray tab would be
 * a miserable way to find out. */
static int jw__i18n_entry_cmp(const void *a, const void *b) {
    const jw__i18n_entry *x = (const jw__i18n_entry *)a;
    const jw__i18n_entry *y = (const jw__i18n_entry *)b;
    if (x->hash < y->hash) return -1;
    if (x->hash > y->hash) return 1;
    return 0;
}

static bool jw__i18n_load_tsv(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long size = ftell(fp);
    if (size <= 0 || size > 16L * 1024 * 1024) { fclose(fp); return false; }
    rewind(fp);

    /* One allocation holds the entry array and the pool: the pool is the file
       text itself, rewritten in place with NULs at the tabs and newlines. */
    size_t cap = 256;
    jw__i18n_entry *entries = (jw__i18n_entry *)malloc(cap * sizeof(*entries));
    char *text = (char *)malloc((size_t)size + 1);
    if (!entries || !text) { free(entries); free(text); fclose(fp); return false; }

    size_t got = fread(text, 1, (size_t)size, fp);
    fclose(fp);
    text[got] = '\0';

    uint32_t count = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = '\0';
        if (!len || line[0] == '#') continue;

        char *tab = strchr(line, '\t');
        if (!tab || tab == line) continue;        /* no key or no separator */
        *tab = '\0';
        char *val = tab + 1;
        if (!val[0]) continue;                    /* untranslated, leave English */

        if (count == cap) {
            size_t ncap = cap * 2;
            jw__i18n_entry *grown =
                (jw__i18n_entry *)realloc(entries, ncap * sizeof(*entries));
            if (!grown) { free(entries); free(text); return false; }
            entries = grown;
            cap = ncap;
        }
        entries[count].hash    = jw__i18n_hash(line);
        entries[count].key_off = (uint32_t)(line - text);
        entries[count].val_off = (uint32_t)(val - text);
        count++;
    }

    if (count == 0) { free(entries); free(text); return false; }
    qsort(entries, count, sizeof(*entries), jw__i18n_entry_cmp);

    g_i18n.blob      = (unsigned char *)text;
    g_i18n.blob_size = got + 1;
    g_i18n.entries   = entries;
    g_i18n.count     = count;
    g_i18n.pool      = text;
    g_i18n.pool_size = (uint32_t)(got + 1);
    return true;
}

/* The TSV path allocates its entry array separately from the pool; the
 * compiled path carves both out of one blob. Tracking which is which keeps
 * shutdown from freeing an interior pointer. */
static bool g_i18n_entries_owned = false;

/* ── Public API ──────────────────────────────────────────────────────────── */

static void jw__i18n_miss_init(const char *lang);
static void jw__i18n_miss_free(void);

bool jw_i18n_load(const char *lang) {
    jw_i18n_shutdown();

    if (!lang || !lang[0] || strcmp(lang, "en") == 0) return false;

    /* Only with a table loaded. English has nothing to miss against, so
       recording there would report every string in the product as untranslated. */
    jw__i18n_miss_init(lang);

    char path[PATH_MAX];

    if (jw__i18n_path(path, sizeof(path), "UMRK_INTERNAL_DATA_PATH", NULL,
                      lang, "tsv") &&
        jw__i18n_load_tsv(path)) {
        g_i18n_entries_owned = true;
        snprintf(g_i18n.lang, sizeof(g_i18n.lang), "%s", lang);
        jw_log_info("i18n: %s from live override %s (%u entries)",
                    lang, path, g_i18n.count);
        return true;
    }

    if (jw__i18n_path(path, sizeof(path), "UMRK_PLATFORM_PATH", NULL,
                      lang, "jwi") &&
        jw__i18n_load_compiled(path)) {
        g_i18n_entries_owned = false;
        snprintf(g_i18n.lang, sizeof(g_i18n.lang), "%s", lang);
        jw_log_info("i18n: %s (%u entries)", lang, g_i18n.count);
        return true;
    }

    jw_log_warn("i18n: no table for %s; falling back to English", lang);
    return false;
}

/* ── Coverage log ────────────────────────────────────────────────────────────
 *
 * A missing translation is silent: T() returns its key and the screen renders
 * English, so the only thing that has ever detected one is a native speaker
 * reading the device. That does not scale to a second language, and it spends a
 * volunteer's time on a task a machine should do.
 *
 * Recording the keys that miss turns coverage into something measurable: walk
 * the UI, dump the set, translate what comes out. It also catches the class no
 * static extractor can reach, because a key assembled with snprintf at runtime
 * has no literal to find.
 *
 * ⛔ A miss is the HOT path, not an exception. Values flow through T() as well
 * as labels, so every game name, system name and scraper status misses on every
 * redraw. This is why the whole thing hangs off a flag that is off in normal
 * use, and why the set never grows without bound. jawakad logging one line per
 * haptic tick once accounted for 16% of the log; the same mistake here would sit
 * on the render path.
 *
 * ── Why this appends as it goes rather than dumping at exit ──
 *
 * The first version wrote the set once, on the way out. A walkthrough could be
 * lost three different ways and each one cost real testing time:
 *
 *   1. A hot-reload deploy kills the launcher with SIGKILL, which cannot be
 *      caught, so nothing was ever written.
 *   2. Changing language calls jw_i18n_load, which shuts the table down and
 *      freed the set -- and switching TO English stopped recording entirely,
 *      silently, because "en" returns before this is initialised.
 *   3. jawaka-menu does not exit at all in the in-game case. It hides and stays
 *      resident, so its exit hook never ran.
 *
 * Appending each newly-seen key the moment it is first seen fixes all three:
 * the file on disk is always current, and losing the process loses nothing.
 *
 * The write cost is bounded by DISTINCT keys, not by lookups -- a key is written
 * once, ever, and the in-memory set makes every repeat free. The worst case is
 * scrolling a large game library for the first time, which appends one short
 * line per title and then never again.
 *
 * Duplicates across runs are possible and deliberate: the set is per-process, so
 * a relaunch re-appends what it re-sees rather than reading the file back. The
 * consumer sorts and dedups, which is cheaper and less fragile than parsing our
 * own output at startup.
 */

#ifdef JW_I18N_COVERAGE

#define JW_I18N_MISS_CAP 4096u          /* distinct keys; ~200KB worst case */

typedef struct {
    char    **keys;                      /* open-addressed, NULL = free slot */
    uint32_t *hashes;
    uint32_t  cap;                       /* power of two */
    uint32_t  count;
    bool      enabled;
    bool      overflowed;
    FILE     *sink;                      /* append-mode, flushed per new key */
} jw__i18n_misses;

static jw__i18n_misses g_misses;

/* Read once at load rather than per lookup: getenv on the render path is the
 * kind of thing this file's header comment exists to prevent.
 *
 * Two triggers, because the env var alone is not usable on the device. The
 * launcher and menu are spawned by jawakad, which init starts three levels up
 * from any shell a tester has, so there is nowhere convenient to export a
 * variable. A marker file on the card is settable over ADB in one line, sticks
 * across reboots until removed, and matches how everything else here is gated
 * (loong_upgrade, the .umrk-migrations stamps). The env var stays for desktop
 * runs and the test binary. */
static void jw__i18n_miss_init(const char *lang) {
    const char *env = getenv("JAWAKA_I18N_COVERAGE");
    g_misses.enabled = env && env[0] && strcmp(env, "0") != 0;

    if (!g_misses.enabled) {
        const char *root = getenv("UMRK_INTERNAL_DATA_PATH");
        if (root && root[0]) {
            char marker[PATH_MAX];
            if (snprintf(marker, sizeof(marker), "%s/i18n/coverage.on", root) <
                    (int)sizeof(marker) &&
                access(marker, F_OK) == 0) {
                g_misses.enabled = true;
            }
        }
    }
    if (!g_misses.enabled) return;

    g_misses.cap = 1024u;
    g_misses.keys = calloc(g_misses.cap, sizeof(*g_misses.keys));
    g_misses.hashes = calloc(g_misses.cap, sizeof(*g_misses.hashes));
    if (!g_misses.keys || !g_misses.hashes) {
        free(g_misses.keys);
        free(g_misses.hashes);
        g_misses.keys = NULL;
        g_misses.hashes = NULL;
        g_misses.enabled = false;
        return;
    }
    g_misses.count = 0;
    g_misses.overflowed = false;

    /* One file per binary, because each UI binary loads its own table and has
       its own gaps -- jawaka-menu once rendered English purely because only the
       launcher had loaded one. The name comes from /proc/self/comm so no call
       site has to pass it and no signature changes; the env var covers desktop
       runs and the test binary, where /proc does not exist. */
    char tag[32] = "";
    const char *tag_env = getenv("JAWAKA_I18N_COVERAGE_TAG");
    if (tag_env && tag_env[0]) {
        snprintf(tag, sizeof(tag), "%s", tag_env);
    } else {
        FILE *cf = fopen("/proc/self/comm", "r");
        if (cf) {
            if (fgets(tag, sizeof(tag), cf)) tag[strcspn(tag, "\n")] = '\0';
            fclose(cf);
        }
    }
    if (!tag[0]) snprintf(tag, sizeof(tag), "%s", "unknown");
    for (char *p = tag; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == ' ') *p = '-';   /* never a path */
    }

    const char *logs = getenv("LOGS_PATH");
    char path[PATH_MAX];
    if (logs && logs[0] &&
        snprintf(path, sizeof(path), "%s/i18n-coverage-%s.txt", logs, tag) <
            (int)sizeof(path)) {
        g_misses.sink = fopen(path, "a");
    }
    if (!g_misses.sink) {
        /* Recording with nowhere to put it would silently waste a walkthrough,
           which is the exact failure this rewrite exists to remove. */
        jw_log_warn("i18n coverage: no writable log; recording disabled");
        jw__i18n_miss_free();
        return;
    }
    fprintf(g_misses.sink, "# session start lang=%s binary=%s\n",
            lang ? lang : "?", tag);
    fflush(g_misses.sink);
    jw_log_info("i18n coverage: appending untranslated keys to %s", path);
}

static void jw__i18n_miss_free(void) {
    if (g_misses.sink) fclose(g_misses.sink);        /* content is already on disk */
    for (uint32_t i = 0; i < g_misses.cap; i++) {
        if (g_misses.keys && g_misses.keys[i]) free(g_misses.keys[i]);
    }
    free(g_misses.keys);
    free(g_misses.hashes);
    memset(&g_misses, 0, sizeof(g_misses));
}

static bool jw__i18n_miss_grow(void);

/* Cheap on a repeat, which is the common case: the same handful of keys miss on
 * every redraw, so after the first frame this is a hash probe and a strcmp
 * against a slot that is already there. */
static void jw__i18n_miss_record(const char *english, uint32_t hash) {
    if (!g_misses.enabled) return;
    if (g_misses.count * 4 >= g_misses.cap * 3 && !jw__i18n_miss_grow()) return;

    uint32_t mask = g_misses.cap - 1;
    uint32_t i = hash & mask;
    while (g_misses.keys[i]) {
        if (g_misses.hashes[i] == hash &&
            strcmp(g_misses.keys[i], english) == 0) {
            return;                                  /* already recorded */
        }
        i = (i + 1) & mask;
    }

    char *copy = strdup(english);
    if (!copy) return;                               /* diagnostics never fail a launch */
    g_misses.keys[i] = copy;
    g_misses.hashes[i] = hash;
    g_misses.count++;

    /* Written the moment it is first seen, so nothing depends on this process
       living long enough to be asked. Bounded by distinct keys: every repeat
       returned above without reaching here. Newlines would corrupt the
       one-key-per-line format, so they are escaped rather than dropped -- a key
       containing one is rare but real (multi-line dialog text). */
    if (g_misses.sink) {
        for (const char *p = copy; *p; p++) {
            if (*p == '\n')      fputs("\\n", g_misses.sink);
            else if (*p == '\r') fputs("\\r", g_misses.sink);
            else                 fputc(*p, g_misses.sink);
        }
        fputc('\n', g_misses.sink);
        fflush(g_misses.sink);
    }
}

static bool jw__i18n_miss_grow(void) {
    if (g_misses.cap >= JW_I18N_MISS_CAP) {
        if (!g_misses.overflowed) {
            g_misses.overflowed = true;
            jw_log_warn("i18n coverage: hit the %u key cap; later misses dropped",
                        JW_I18N_MISS_CAP);
        }
        return false;
    }
    uint32_t ncap = g_misses.cap * 2;
    char **nk = calloc(ncap, sizeof(*nk));
    uint32_t *nh = calloc(ncap, sizeof(*nh));
    if (!nk || !nh) { free(nk); free(nh); return false; }

    uint32_t nmask = ncap - 1;
    for (uint32_t i = 0; i < g_misses.cap; i++) {
        if (!g_misses.keys[i]) continue;
        uint32_t j = g_misses.hashes[i] & nmask;
        while (nk[j]) j = (j + 1) & nmask;
        nk[j] = g_misses.keys[i];
        nh[j] = g_misses.hashes[i];
    }
    free(g_misses.keys);
    free(g_misses.hashes);
    g_misses.keys = nk;
    g_misses.hashes = nh;
    g_misses.cap = ncap;
    return true;
}

/* The file is written as keys are discovered, so there is nothing to flush and
   no "dump" step to forget. This reports what THIS process has seen, which is
   what a test can assert on; the file may hold more, from earlier runs. */
size_t jw_i18n_coverage_count(void) {
    return g_misses.enabled ? g_misses.count : 0;
}

bool jw_i18n_coverage_enabled(void) { return g_misses.enabled; }

#else  /* !JW_I18N_COVERAGE -- compiled out for stable releases */

static void jw__i18n_miss_init(const char *lang) { (void)lang; }
static void jw__i18n_miss_free(void) { }
static void jw__i18n_miss_record(const char *english, uint32_t hash) {
    (void)english; (void)hash;
}
size_t jw_i18n_coverage_count(void) { return 0; }
bool jw_i18n_coverage_enabled(void) { return false; }

#endif /* JW_I18N_COVERAGE */

void jw_i18n_shutdown(void) {
    if (g_i18n_entries_owned) free((void *)g_i18n.entries);
    g_i18n_entries_owned = false;
    jw__i18n_miss_free();
    jw__i18n_reset();
}

const char *jw_i18n(const char *english) {
    if (!english) return NULL;
    if (g_i18n.count == 0) return jw__i18n_strip_context(english);

    uint32_t h = jw__i18n_hash(english);

    /* Binary search to the first entry with this hash, then walk equals and
       strcmp. Collisions are rare but must not silently return the wrong
       string, which is exactly the sort of bug nobody who reads the language
       would ever report. */
    uint32_t lo = 0, hi = g_i18n.count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_i18n.entries[mid].hash < h) lo = mid + 1;
        else hi = mid;
    }
    for (uint32_t i = lo; i < g_i18n.count && g_i18n.entries[i].hash == h; i++) {
        const char *key = g_i18n.pool + g_i18n.entries[i].key_off;
        if (strcmp(key, english) == 0) {
            const char *val = g_i18n.pool + g_i18n.entries[i].val_off;
            /* Format strings are looked up and then handed to snprintf, so a
               translation with mismatched conversions must lose here, not
               crash there. Cheap for plain strings: no '%' exits instantly. */
            if (strchr(english, '%') && !jw__i18n_fmt_compatible(english, val)) {
                /* Recorded like any other miss: the user sees English either
                   way. It stays distinguishable downstream without extra
                   plumbing, because this key IS in the .po -- so a dumped key
                   that already has a translation is a format mismatch, not a
                   coverage gap. */
                jw__i18n_miss_record(english, h);
                return jw__i18n_strip_context(english);
            }
            return val;
        }
    }
    jw__i18n_miss_record(english, h);
    return jw__i18n_strip_context(english);
}

/* Every language Leaf can plausibly ship needs the CJK face, so this is a
 * prefix test rather than a table: zh (Chinese), ja (Japanese) and ko (Korean)
 * all draw from the same Source Han family. Kept as a function so adding a
 * Latin language later is a one-line change here rather than a hunt through
 * call sites. */
bool jw_i18n_language_is_cjk(const char *lang) {
    if (!lang || !lang[0]) return false;
    return strncmp(lang, "zh", 2) == 0 ||
           strncmp(lang, "ja", 2) == 0 ||
           strncmp(lang, "ko", 2) == 0;
}

/* Scans both roots for <code>.jwi and <code>.tsv. A language offered by either
 * is offered to the user, so a translator's dropped .tsv makes the Settings row
 * appear without a build. */
static void jw__i18n_scan_dir(const char *env, const char *ext,
                              char store[][JW_I18N_CODE_MAX], size_t *count, size_t max) {
    char dir[PATH_MAX];
    const char *root = getenv(env);
    if (!root || !root[0]) return;
    if (snprintf(dir, sizeof(dir), "%s/i18n", root) >= (int)sizeof(dir)) return;

    DIR *dp = opendir(dir);
    if (!dp) return;
    size_t ext_len = strlen(ext);
    struct dirent *de;
    /* Read every entry, not just until full: a language past the limit has to be
       seen to be reported. Duplicates are checked first, so a language offered
       both as a .tsv and as a compiled table is never mistaken for overflow. */
    while ((de = readdir(dp)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n <= ext_len + 1 || strcmp(de->d_name + n - ext_len, ext) != 0) continue;
        size_t base = n - ext_len - 1;                 /* drop ".<ext>" */
        if (base == 0) continue;
        if (base >= JW_I18N_CODE_MAX) {
            jw_log_warn("i18n: %s/%s: language code longer than %d characters; ignoring",
                        dir, de->d_name, JW_I18N_CODE_MAX - 1);
            continue;
        }
        char code[JW_I18N_CODE_MAX];
        memcpy(code, de->d_name, base);
        code[base] = '\0';
        if (strcmp(code, "en") == 0) continue;         /* en is the built-in default */
        bool seen = false;
        for (size_t i = 0; i < *count; i++)
            if (strcmp(store[i], code) == 0) { seen = true; break; }
        if (seen) continue;
        if (*count >= max) {
            /* `max` is the effective limit for this call, which is below the
               constant when a caller asks for fewer. Naming the constant only
               when it is the reason keeps the message true either way. */
            if (max >= JW_I18N_MAX_LANGUAGES)
                jw_log_warn("i18n: %s not offered: Leaf lists at most %d languages "
                            "(raise JW_I18N_MAX_LANGUAGES)", code, JW_I18N_MAX_LANGUAGES);
            else
                jw_log_warn("i18n: %s not offered: the caller asked for %zu", code, max);
            continue;
        }
        snprintf(store[*count], JW_I18N_CODE_MAX, "%s", code);
        (*count)++;
    }
    closedir(dp);
}

size_t jw_i18n_available(const char **out, size_t max) {
    static char store[JW_I18N_MAX_LANGUAGES][JW_I18N_CODE_MAX];
    size_t count = 0;
    if (!out || max == 0) return 0;
    size_t cap = max < JW_I18N_MAX_LANGUAGES ? max : JW_I18N_MAX_LANGUAGES;
    jw__i18n_scan_dir("UMRK_INTERNAL_DATA_PATH", "tsv", store, &count, cap);
    jw__i18n_scan_dir("UMRK_PLATFORM_PATH", "jwi", store, &count, cap);
    for (size_t i = 0; i < count; i++) out[i] = store[i];
    return count;
}

const char *jw_i18n_language(void) {
    return g_i18n.lang;
}

size_t jw_i18n_count(void) {
    return (size_t)g_i18n.count;
}
