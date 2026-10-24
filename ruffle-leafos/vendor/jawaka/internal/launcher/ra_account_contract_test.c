/* standalone-ra-account-v1 producer replay.

   Replays the pinned leaf-contracts fixtures through Jawaka's PRODUCER, not
   through a copy of the consumer classifier: each fixture is mapped to the
   input the producer actually has (stored account rows, or an inherited
   environment), run through the same code jawakad uses for a standalone
   child (jw_db_resolve_ra_account_handoff, then
   jw_ra_account_prepare_standalone_env), and the environment that comes out
   is compared with what the contract says must come out.

   Mappings, by fixture:

   - valid handoff, state configured / signed-out: the stored rows that
     state is made of (the pair and the revision; a sign-out stores empty
     credentials and keeps the revision). The output must be the fixture's
     snapshot byte for byte.
   - valid handoff, never-configured / invalid / unreadable: an empty store,
     an incomplete stored pair, a store that is not a database. Output must
     equal the fixture.
   - invalid handoff whose reason is a property of a stored value (missing
     half, empty, oversized, control character, invalid UTF-8, malformed or
     overflowing revision): those stored rows. The producer must answer with
     the "invalid" verdict and no credentials, never the values. The one
     exception is revision-missing on an otherwise valid pair: that is a
     pre-contract store, and the contract gives it revision 1 in a checked
     write, so the output is that configured snapshot at revision 1.
   - invalid handoff whose reason is a SHAPE a producer could only emit by
     mistake (no or wrong version, missing or unknown state, credentials or a
     revision on the wrong state, RetroArch credentials in a standalone
     child): there are no stored rows that produce it. The fixture's
     environment is planted as the inherited environment instead, and the
     producer must replace it: an authorized launch with an empty store
     yields exactly never-configured, an unauthorized one yields total
     absence. For the RetroArch leak the store holds the fixture's account
     and the output is that snapshot without JAWAKA_CHEEVOS_*.
   - unmanaged: an unauthorized target over a planted stale snapshot must
     come out with total absence.

   Every output is also written to a JSON file that
   scripts/ra-account-contract-replay.py classifies with the pinned
   reference classifier (classify_ra_account_env); every output must be a
   valid handoff or unmanaged, never a malformed one. A fixture whose reason
   this file does not know fails the run, so a contract change forces a
   reviewed mapping update here.

   Usage: ra-account-contract-test <fixtures.json> <outputs.json> <workdir>
   The fixtures are synthetic; nothing here prints a credential. */

#include "internal/db/db.h"
#include "internal/launcher/ra_account.h"

#include "cJSON.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int failures;

static void failf(const char *name, const char *what) {
    fprintf(stderr, "ra-account-contract-test: %s: %s\n", name, what);
    failures++;
}

/* Every variable the contract classifier looks at. */
static const char *const WATCHED[] = {
    "UMRK_RA_ACCOUNT_VERSION",  "UMRK_RA_ACCOUNT_STATE",
    "UMRK_RA_ACCOUNT_USERNAME", "UMRK_RA_ACCOUNT_PASSWORD",
    "UMRK_RA_ACCOUNT_REVISION", "JAWAKA_CHEEVOS_USERNAME",
    "JAWAKA_CHEEVOS_PASSWORD",
};
#define WATCHED_COUNT (sizeof(WATCHED) / sizeof(WATCHED[0]))

typedef struct {
    bool present[WATCHED_COUNT];
    unsigned char *value[WATCHED_COUNT];
    size_t len[WATCHED_COUNT];
} env_set;

static void env_free(env_set *e) {
    for (size_t i = 0; i < WATCHED_COUNT; i++) free(e->value[i]);
    memset(e, 0, sizeof(*e));
}

static int watched_index(const char *name) {
    for (size_t i = 0; i < WATCHED_COUNT; i++)
        if (strcmp(WATCHED[i], name) == 0) return (int)i;
    return -1;
}

static void env_put(env_set *e, const char *name, const unsigned char *bytes,
                    size_t len) {
    int i = watched_index(name);
    if (i < 0) return;  /* UMRK_LANGUAGE and friends are not contract fields */
    free(e->value[i]);
    e->value[i] = malloc(len + 1);
    if (!e->value[i]) exit(2);
    memcpy(e->value[i], bytes, len);
    e->value[i][len] = '\0';
    e->len[i] = len;
    e->present[i] = true;
}

static const unsigned char *env_get(const env_set *e, const char *name,
                                    size_t *len) {
    int i = watched_index(name);
    if (i < 0 || !e->present[i]) return NULL;
    if (len) *len = e->len[i];
    return e->value[i];
}

static int b64_value(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *b64_decode(const char *text, size_t *out_len) {
    size_t n = strlen(text);
    unsigned char *out = malloc(n + 1);
    if (!out) exit(2);
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n && text[i] != '='; i++) {
        int v = b64_value((unsigned char)text[i]);
        if (v < 0) exit(2);
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    *out_len = o;
    return out;
}

static void b64_encode(FILE *f, const unsigned char *bytes, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)bytes[i] << 16;
        if (i + 1 < len) v |= (unsigned)bytes[i + 1] << 8;
        if (i + 2 < len) v |= bytes[i + 2];
        fputc(tbl[(v >> 18) & 63], f);
        fputc(tbl[(v >> 12) & 63], f);
        fputc(i + 1 < len ? tbl[(v >> 6) & 63] : '=', f);
        fputc(i + 2 < len ? tbl[v & 63] : '=', f);
    }
}

static void fixture_env(const cJSON *c, env_set *e) {
    memset(e, 0, sizeof(*e));
    const cJSON *env = cJSON_GetObjectItemCaseSensitive(c, "env");
    const cJSON *item;
    cJSON_ArrayForEach(item, env) {
        if (cJSON_IsString(item))
            env_put(e, item->string, (const unsigned char *)item->valuestring,
                    strlen(item->valuestring));
    }
    const cJSON *env_b64 = cJSON_GetObjectItemCaseSensitive(c, "env_b64");
    cJSON_ArrayForEach(item, env_b64) {
        if (!cJSON_IsString(item)) continue;
        size_t len = 0;
        unsigned char *bytes = b64_decode(item->valuestring, &len);
        env_put(e, item->string, bytes, len);
        free(bytes);
    }
}

/* ---- the producer's inputs ------------------------------------------- */

static char workdir[PATH_MAX];
static char db_path[PATH_MAX];

static void store_reset(void) {
    unlink(db_path);
    /* Create the real schema through the product's own writer. */
    if (jw_db_set_setting(db_path, "theme_name", "replay") != 0) exit(2);
}

/* Store one settings row byte-exactly (invalid UTF-8 and CR/LF included). */
static void store_raw(const char *key, const unsigned char *bytes, size_t len) {
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
                           "INSERT OR REPLACE INTO settings (key, value) "
                           "VALUES (?, ?);",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(st, 2, (const char *)bytes, (int)len,
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_DONE) {
        fprintf(stderr, "ra-account-contract-test: fixture store failed\n");
        exit(2);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

/* The rows a fixture's snapshot is made of. */
static void store_from_snapshot(const env_set *e) {
    store_reset();
    size_t len = 0;
    const unsigned char *state = env_get(e, "UMRK_RA_ACCOUNT_STATE", NULL);
    bool signed_out = state && strcmp((const char *)state, "signed-out") == 0;
    const unsigned char *v;
    if (signed_out) {
        store_raw("retroachievements_user", (const unsigned char *)"", 0);
        store_raw("retroachievements_pass", (const unsigned char *)"", 0);
    } else {
        if ((v = env_get(e, "UMRK_RA_ACCOUNT_USERNAME", &len)))
            store_raw("retroachievements_user", v, len);
        if ((v = env_get(e, "UMRK_RA_ACCOUNT_PASSWORD", &len)))
            store_raw("retroachievements_pass", v, len);
    }
    if ((v = env_get(e, "UMRK_RA_ACCOUNT_REVISION", &len)))
        store_raw("retroachievements_revision", v, len);
}

static void store_not_a_database(void) {
    unlink(db_path);
    FILE *f = fopen(db_path, "wb");
    if (!f) exit(2);
    fputs("not an SQLite database\n", f);
    fclose(f);
}

static void plant_env(const env_set *e) {
    for (size_t i = 0; i < WATCHED_COUNT; i++) {
        if (e->present[i]) setenv(WATCHED[i], (const char *)e->value[i], 1);
        else unsetenv(WATCHED[i]);
    }
}

static void capture_env(env_set *out) {
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < WATCHED_COUNT; i++) {
        const char *v = getenv(WATCHED[i]);
        if (v) env_put(out, WATCHED[i], (const unsigned char *)v, strlen(v));
    }
}

/* What jawakad's standalone child ends up with for this store. */
static void run_producer(bool authorized, env_set *out) {
    jw_ra_account account;
    jw_db_resolve_ra_account_handoff(db_path, &account);
    jw_ra_account_prepare_standalone_env(&account, authorized);
    capture_env(out);
    memset(&account, 0, sizeof(account));
}

static bool env_equal(const env_set *a, const env_set *b) {
    for (size_t i = 0; i < WATCHED_COUNT; i++) {
        if (a->present[i] != b->present[i]) return false;
        if (a->present[i] && (a->len[i] != b->len[i] ||
                              memcmp(a->value[i], b->value[i], a->len[i]) != 0))
            return false;
    }
    return true;
}

static void env_set_str(env_set *e, const char *name, const char *value) {
    env_put(e, name, (const unsigned char *)value, strlen(value));
}

static void verdict(env_set *e, const char *state) {
    memset(e, 0, sizeof(*e));
    env_set_str(e, "UMRK_RA_ACCOUNT_VERSION", "1");
    env_set_str(e, "UMRK_RA_ACCOUNT_STATE", state);
}

/* Only the five contract fields of a fixture (drops JAWAKA_CHEEVOS_*). */
static void snapshot_only(const env_set *in, env_set *out) {
    memset(out, 0, sizeof(*out));
    for (size_t i = 0; i < 5; i++)
        if (in->present[i]) env_put(out, WATCHED[i], in->value[i], in->len[i]);
}

/* ---- output for the reference classifier ---------------------------- */

static FILE *outputs;
static bool first_output = true;

static void emit(const char *name, const char *mapping, const char *expect_kind,
                 const env_set *e) {
    fprintf(outputs, "%s\n  {\"case\": \"%s\", \"mapping\": \"%s\", "
                     "\"expect_kind\": \"%s\", \"env_b64\": {",
            first_output ? "" : ",", name, mapping, expect_kind);
    first_output = false;
    bool first = true;
    for (size_t i = 0; i < WATCHED_COUNT; i++) {
        if (!e->present[i]) continue;
        fprintf(outputs, "%s\"%s\": \"", first ? "" : ", ", WATCHED[i]);
        b64_encode(outputs, e->value[i], e->len[i]);
        fputc('"', outputs);
        first = false;
    }
    fputs("}}", outputs);
}

static void check(const char *name, const char *mapping, const char *expect_kind,
                  const env_set *got, const env_set *want) {
    emit(name, mapping, expect_kind, got);
    if (!env_equal(got, want)) {
        char what[160];
        snprintf(what, sizeof(what), "%s: producer output differs from the contract",
                 mapping);
        failf(name, what);
    }
}

/* ---- the mapping ---------------------------------------------------- */

static const char *const STORED_VALUE_REASONS[] = {
    "username-missing", "password-missing", "credential-empty",
    "username-oversized", "password-oversized", "credential-control-character",
    "credential-invalid-utf8", "revision-missing", "revision-invalid",
    "revision-overflow",
};
static const char *const SHAPE_REASONS[] = {
    "unsupported-version", "missing-account-state", "unknown-account-state",
    "credentials-unexpected", "revision-unexpected", "stale-retroarch-credentials",
};

static bool in_list(const char *s, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(s, list[i]) == 0) return true;
    return false;
}

static void replay_case(const cJSON *c) {
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "name"));
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "kind"));
    const char *reason = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(c, "reason"));
    if (!name || !kind) {
        failf("?", "fixture without name or kind");
        return;
    }
    env_set fixture, got, want;
    fixture_env(c, &fixture);
    memset(&got, 0, sizeof(got));
    memset(&want, 0, sizeof(want));
    const unsigned char *state_b = env_get(&fixture, "UMRK_RA_ACCOUNT_STATE", NULL);
    const char *state = state_b ? (const char *)state_b : "";

    if (strcmp(kind, "unmanaged") == 0) {
        /* An unauthorized target over a planted stale snapshot. */
        env_set stale;
        memset(&stale, 0, sizeof(stale));
        env_set_str(&stale, "UMRK_RA_ACCOUNT_VERSION", "1");
        env_set_str(&stale, "UMRK_RA_ACCOUNT_STATE", "configured");
        env_set_str(&stale, "UMRK_RA_ACCOUNT_USERNAME", "stale-inherited");
        env_set_str(&stale, "UMRK_RA_ACCOUNT_PASSWORD", "stale-inherited");
        env_set_str(&stale, "UMRK_RA_ACCOUNT_REVISION", "99");
        env_set_str(&stale, "JAWAKA_CHEEVOS_USERNAME", "stale-inherited");
        plant_env(&stale);
        store_reset();
        store_raw("retroachievements_user", (const unsigned char *)"player-one", 10);
        store_raw("retroachievements_pass", (const unsigned char *)"correct horse", 13);
        store_raw("retroachievements_revision", (const unsigned char *)"1", 1);
        run_producer(false, &got);
        check(name, "unauthorized-target", "unmanaged", &got, &want);
        env_free(&stale);
    } else if (strcmp(kind, "valid-handoff") == 0) {
        if (strcmp(state, "configured") == 0 || strcmp(state, "signed-out") == 0) {
            store_from_snapshot(&fixture);
        } else if (strcmp(state, "never-configured") == 0) {
            store_reset();
        } else if (strcmp(state, "invalid") == 0) {
            store_reset();  /* an incomplete legacy pair */
            store_raw("retroachievements_user", (const unsigned char *)"player-one", 10);
        } else if (strcmp(state, "unreadable") == 0) {
            store_not_a_database();
        } else {
            failf(name, "valid fixture with a state this replay does not map");
            goto done;
        }
        plant_env(&(env_set){0});
        run_producer(true, &got);
        check(name, "stored-rows", "valid-handoff", &got, &fixture);
    } else if (strcmp(kind, "invalid-handoff") == 0 && reason &&
               in_list(reason, STORED_VALUE_REASONS,
                       sizeof(STORED_VALUE_REASONS) / sizeof(STORED_VALUE_REASONS[0]))) {
        store_from_snapshot(&fixture);
        plant_env(&(env_set){0});
        run_producer(true, &got);
        if (strcmp(reason, "revision-missing") == 0) {
            /* A pre-contract pair: initialized to revision 1, then handed out. */
            snapshot_only(&fixture, &want);
            env_set_str(&want, "UMRK_RA_ACCOUNT_REVISION", "1");
            check(name, "stored-rows-legacy-pair", "valid-handoff", &got, &want);
        } else {
            verdict(&want, "invalid");
            check(name, "stored-rows-rejected", "valid-handoff", &got, &want);
        }
    } else if (strcmp(kind, "invalid-handoff") == 0 && reason &&
               in_list(reason, SHAPE_REASONS,
                       sizeof(SHAPE_REASONS) / sizeof(SHAPE_REASONS[0]))) {
        /* Planted as the inherited environment: the producer must replace it
           for an authorized target and remove it for any other. */
        if (strcmp(reason, "stale-retroarch-credentials") == 0) {
            store_from_snapshot(&fixture);
            snapshot_only(&fixture, &want);
        } else {
            store_reset();
            verdict(&want, "never-configured");
        }
        plant_env(&fixture);
        run_producer(true, &got);
        check(name, "inherited-authorized", "valid-handoff", &got, &want);
        env_free(&got);
        env_free(&want);
        plant_env(&fixture);
        run_producer(false, &got);
        check(name, "inherited-unauthorized", "unmanaged", &got, &want);
    } else {
        failf(name, "no producer mapping for this fixture's kind/reason; "
                    "update ra_account_contract_test.c in a reviewed change");
    }
done:
    env_free(&fixture);
    env_free(&got);
    env_free(&want);
    jw_ra_account_clear_env();
    jw_ra_account_clear_retroarch_env();
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = calloc((size_t)size + 1u, 1u);
    if (text && fread(text, 1, (size_t)size, f) != (size_t)size) {
        free(text);
        text = NULL;
    }
    fclose(f);
    return text;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr,
                "usage: ra-account-contract-test <fixtures.json> <outputs.json> <workdir>\n");
        return 2;
    }
    char *raw = read_file(argv[1]);
    cJSON *root = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    const cJSON *cases = cJSON_GetObjectItemCaseSensitive(root, "cases");
    if (!cJSON_IsArray(cases) || cJSON_GetArraySize(cases) == 0) {
        fprintf(stderr, "ra-account-contract-test: no cases in %s\n", argv[1]);
        return 2;
    }
    snprintf(workdir, sizeof(workdir), "%s", argv[3]);
    snprintf(db_path, sizeof(db_path), "%s/library.db", workdir);
    outputs = fopen(argv[2], "wb");
    if (!outputs) return 2;
    fputs("[", outputs);

    int count = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, cases) {
        replay_case(c);
        count++;
    }
    fputs("\n]\n", outputs);
    fclose(outputs);
    cJSON_Delete(root);
    unlink(db_path);

    if (failures) {
        fprintf(stderr, "ra-account-contract-test: %d FAILURE(S) over %d fixtures\n",
                failures, count);
        return 1;
    }
    printf("ra-account-contract-test: %d fixtures replayed through the producer\n",
           count);
    return 0;
}
