/* standalone-ra-account-v1 producer tests: the checked account/save/clear
   transactions, the coherent resolver snapshot, legacy revision
   initialization, malformed and oversized stored values, and the
   credential-input validator. Runs entirely against temporary databases. */

/* Only this test calls the real SQLite entrypoints; db.c is compiled with
   wrappers so a second writer can commit at deterministic read boundaries. */
#undef sqlite3_close
#undef sqlite3_step
#include "internal/db/db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void expect(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "ra-account-test: %s\n", what);
        failures++;
    }
}

static const char *race_db;
static int race_close_count;
static int race_step;
static int race_sign_out;
static int race_writes;

static void race_write(void) {
    long long revision = 0;
    race_writes++;
    int rc = race_sign_out
        ? jw_db_clear_ra_account(race_db, &revision)
        : jw_db_save_ra_account(race_db, "player-two", "two", &revision);
    expect(rc == 0, "interleaved writer commits");
}

int jw_test_sqlite3_close(sqlite3 *db) {
    int rc = sqlite3_close(db);
    if (rc == SQLITE_OK && race_close_count > 0 && --race_close_count == 0)
        race_write();
    return rc;
}

int jw_test_sqlite3_step(sqlite3_stmt *stmt) {
    int rc = sqlite3_step(stmt);
    if (race_step && rc == SQLITE_ROW &&
        strcmp(sqlite3_sql(stmt), "SELECT value FROM settings WHERE key = ?;") == 0) {
        race_step = 0;
        race_write();
    }
    return rc;
}

/* A fresh database file per case, so no case sees another's keys. */
static void fresh_db(char *path, size_t size) {
    snprintf(path, size, "/tmp/jawaka-ra-account.XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        exit(1);
    }
    close(fd);
    unlink(path);
}

static void set(const char *db, const char *key, const char *value) {
    if (jw_db_set_setting(db, key, value) != 0) {
        fprintf(stderr, "ra-account-test: could not set %s\n", key);
        exit(1);
    }
}

static void expect_state(const char *db, jw_ra_account_state want,
                         const char *name) {
    jw_ra_account acc;
    char what[160];
    snprintf(what, sizeof(what), "%s: resolve failed", name);
    expect(jw_db_resolve_ra_account(db, &acc) == 0, what);
    snprintf(what, sizeof(what), "%s: state %d, want %d", name, acc.state, want);
    expect(acc.state == want, what);
}

/* ------------------------------------------------------------------ */
/* Save / clear / revision discipline                                 */
/* ------------------------------------------------------------------ */

static void test_save_clear_cycle(void) {
    char db[64];
    fresh_db(db, sizeof(db));

    jw_ra_account acc;
    expect_state(db, JW_RA_ACCOUNT_NEVER_CONFIGURED, "fresh");

    long long rev = 0;
    expect(jw_db_save_ra_account(db, "player-one", "correct horse", &rev) == 0,
           "first save succeeds");
    expect(rev == 1, "first save assigns revision 1");
    expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
           acc.state == JW_RA_ACCOUNT_CONFIGURED && acc.revision == 1 &&
           strcmp(acc.user, "player-one") == 0 &&
           strcmp(acc.pass, "correct horse") == 0,
           "first save resolves configured revision 1");

    /* A changed password for the same username still bumps the revision. */
    expect(jw_db_save_ra_account(db, "player-one", "new horse", &rev) == 0,
           "password change save succeeds");
    expect(rev == 2, "password change bumps revision to 2");
    expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
           acc.state == JW_RA_ACCOUNT_CONFIGURED && acc.revision == 2 &&
           strcmp(acc.pass, "new horse") == 0,
           "password change persists revision 2 and the new password");

    /* Quotes, spaces and shell metacharacters are valid data. */
    expect(jw_db_save_ra_account(db, "o'hara \"jr\"", "p@$$ |;,>&", &rev) == 0,
           "punctuation save succeeds");
    expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
           acc.state == JW_RA_ACCOUNT_CONFIGURED &&
           strcmp(acc.user, "o'hara \"jr\"") == 0 &&
           strcmp(acc.pass, "p@$$ |;,>&") == 0,
           "punctuation survives the round trip");

    /* Sign-out clears the credentials but bumps and retains the revision. */
    expect(jw_db_clear_ra_account(db, &rev) == 0, "clear succeeds");
    expect(rev == 4, "clear bumps the retained revision");
    expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
           acc.state == JW_RA_ACCOUNT_SIGNED_OUT && acc.revision == 4 &&
           acc.user[0] == '\0' && acc.pass[0] == '\0',
           "clear resolves signed-out with the retained revision");

    /* A new account after sign-out continues the same sequence. */
    expect(jw_db_save_ra_account(db, "player-two", "another", &rev) == 0,
           "save after sign-out succeeds");
    expect(rev == 5, "save after sign-out continues the revision sequence");
}

static void test_failed_save_preserves_prior(void) {
    char db[64];
    fresh_db(db, sizeof(db));

    long long rev = 0;
    expect(jw_db_save_ra_account(db, "player-one", "correct horse", &rev) == 0,
           "initial save succeeds");

    /* Oversized input is refused before any write: nothing changes. */
    char big_user[JW_RA_USERNAME_MAX + 2];
    memset(big_user, 'a', sizeof(big_user) - 1);
    big_user[sizeof(big_user) - 1] = '\0';
    expect(jw_db_save_ra_account(db, big_user, "ok", &rev) != 0,
           "oversized username save is refused");
    expect(jw_db_save_ra_account(db, "", "ok", &rev) != 0,
           "empty username save is refused");

    jw_ra_account acc;
    expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
           acc.state == JW_RA_ACCOUNT_CONFIGURED && acc.revision == 1 &&
           strcmp(acc.user, "player-one") == 0 &&
           strcmp(acc.pass, "correct horse") == 0,
           "prior account intact after refused saves");

    /* A database that vanished on disk reports failure, never success. */
    expect(jw_db_save_ra_account("/nonexistent-dir-xyz/library.db",
                                 "player-two", "pw", &rev) != 0,
           "unwritable database save fails");
}

/* ------------------------------------------------------------------ */
/* Legacy pairs and malformed stored values                           */
/* ------------------------------------------------------------------ */

static void test_legacy_pair_initialized_once(void) {
    char db[64];
    fresh_db(db, sizeof(db));

    /* A pair saved before revisions existed (pre-contract launcher). */
    set(db, "retroachievements_user", "player-one");
    set(db, "retroachievements_pass", "correct horse");

    jw_ra_account acc;
    expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
           acc.state == JW_RA_ACCOUNT_CONFIGURED && acc.revision == 0,
           "legacy pair resolves configured with revision 0");

    long long rev = 0;
    expect(jw_db_ensure_ra_account_revision(db, &rev) == 0 && rev == 1,
           "legacy pair initialized at revision 1");
    expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
           acc.state == JW_RA_ACCOUNT_CONFIGURED && acc.revision == 1,
           "initialized legacy pair resolves revision 1");

    /* The initialization happens exactly once: it never bumps afterwards. */
    expect(jw_db_ensure_ra_account_revision(db, &rev) == 0 && rev == 1,
           "second ensure is a no-op");
    expect(jw_db_ensure_ra_account_revision(db, &rev) == 0 && rev == 1,
           "third ensure is a no-op");

    /* Nothing to initialize in a fresh database. */
    char db2[64];
    fresh_db(db2, sizeof(db2));
    expect(jw_db_ensure_ra_account_revision(db2, &rev) == 1,
           "fresh database has no legacy pair");
}

static void test_malformed_stored_values(void) {
    /* Oversized legacy username: invalid, never a truncated login. */
    {
        char db[64];
        fresh_db(db, sizeof(db));
        char big_user[JW_RA_USERNAME_MAX + 2];
        memset(big_user, 'a', sizeof(big_user) - 1);
        big_user[sizeof(big_user) - 1] = '\0';
        set(db, "retroachievements_user", big_user);
        set(db, "retroachievements_pass", "ok");
        set(db, "retroachievements_revision", "3");
        jw_ra_account acc;
        expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
               acc.state == JW_RA_ACCOUNT_INVALID,
               "oversized username is invalid");
    }
    /* Oversized legacy password. */
    {
        char db[64];
        fresh_db(db, sizeof(db));
        char big_pass[JW_RA_PASSWORD_MAX + 2];
        memset(big_pass, 'a', sizeof(big_pass) - 1);
        big_pass[sizeof(big_pass) - 1] = '\0';
        set(db, "retroachievements_user", "player-one");
        set(db, "retroachievements_pass", big_pass);
        set(db, "retroachievements_revision", "3");
        jw_ra_account acc;
        expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
               acc.state == JW_RA_ACCOUNT_INVALID,
               "oversized password is invalid");
    }
    /* Embedded CR/LF. */
    {
        char db[64];
        fresh_db(db, sizeof(db));
        set(db, "retroachievements_user", "player\r\none");
        set(db, "retroachievements_pass", "ok");
        set(db, "retroachievements_revision", "3");
        expect_state(db, JW_RA_ACCOUNT_INVALID, "embedded CR/LF");
    }
    /* Incomplete pair, either direction. */
    {
        char db[64];
        fresh_db(db, sizeof(db));
        set(db, "retroachievements_user", "player-one");
        expect_state(db, JW_RA_ACCOUNT_INVALID, "user without pass");
    }
    {
        char db[64];
        fresh_db(db, sizeof(db));
        set(db, "retroachievements_pass", "correct horse");
        expect_state(db, JW_RA_ACCOUNT_INVALID, "pass without user");
    }
    /* Malformed revisions: zero, negative, text, overflow. */
    const char *bad_revs[] = {"0", "-3", "abc", "4611686018427387905", ""};
    for (size_t i = 0; i < sizeof(bad_revs) / sizeof(bad_revs[0]); i++) {
        char db[64];
        fresh_db(db, sizeof(db));
        set(db, "retroachievements_user", "player-one");
        set(db, "retroachievements_pass", "correct horse");
        set(db, "retroachievements_revision", bad_revs[i]);
        char name[96];
        snprintf(name, sizeof(name), "malformed revision '%s'", bad_revs[i]);
        expect_state(db, JW_RA_ACCOUNT_INVALID, name);
        /* ensure must not guess a repair for a malformed revision. */
        long long rev = 0;
        expect(jw_db_ensure_ra_account_revision(db, &rev) == 1,
               "malformed revision is not repaired by ensure");
    }
    /* A malformed revision with empty credentials is also invalid: it cannot
       prove either sign-out or never-configured. */
    {
        char db[64];
        fresh_db(db, sizeof(db));
        set(db, "retroachievements_revision", "xyz");
        expect_state(db, JW_RA_ACCOUNT_INVALID, "bad revision, no credentials");
    }
    /* A present revision with empty credentials is a retained sign-out. */
    {
        char db[64];
        fresh_db(db, sizeof(db));
        set(db, "retroachievements_revision", "7");
        jw_ra_account acc;
        expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
               acc.state == JW_RA_ACCOUNT_SIGNED_OUT && acc.revision == 7,
               "revision alone resolves signed-out");
    }
}

static void test_revision_overflow_fails(void) {
    char db[64];
    fresh_db(db, sizeof(db));
    set(db, "retroachievements_user", "player-one");
    set(db, "retroachievements_pass", "correct horse");
    char ceiling[24];
    snprintf(ceiling, sizeof(ceiling), "%lld", JW_RA_REVISION_MAX);
    set(db, "retroachievements_revision", ceiling);

    long long rev = 0;
    expect(jw_db_save_ra_account(db, "player-one", "new", &rev) != 0,
           "save at the revision ceiling fails instead of wrapping");
    jw_ra_account acc;
    expect(jw_db_resolve_ra_account(db, &acc) == 0 &&
           acc.state == JW_RA_ACCOUNT_CONFIGURED &&
           acc.revision == JW_RA_REVISION_MAX &&
           strcmp(acc.pass, "correct horse") == 0,
           "prior account intact after overflow refusal");

    /* One below the ceiling still saves. */
    char below[24];
    snprintf(below, sizeof(below), "%lld", JW_RA_REVISION_MAX - 1);
    set(db, "retroachievements_revision", below);
    expect(jw_db_save_ra_account(db, "player-one", "new", &rev) == 0 &&
           rev == JW_RA_REVISION_MAX,
           "save below the ceiling reaches it normally");
}

/* ------------------------------------------------------------------ */
/* Coherent snapshot reads                                            */
/* ------------------------------------------------------------------ */

/* A second connection saves after the first account key has been read.
   WAL lets that writer commit while the reader still holds its snapshot. */
static void test_coherent_snapshot(void) {
    char db[64];
    fresh_db(db, sizeof(db));

    long long rev = 0;
    expect(jw_db_save_ra_account(db, "player-one", "one", &rev) == 0 &&
           rev == 1, "baseline save");

    sqlite3 *connection = NULL;
    expect(sqlite3_open(db, &connection) == SQLITE_OK, "open WAL fixture");
    expect(sqlite3_exec(connection, "PRAGMA journal_mode=WAL", NULL, NULL, NULL)
           == SQLITE_OK, "enable concurrent writer fixture");
    sqlite3_close(connection);
    race_db = db;
    race_step = 1;
    race_writes = 0;
    jw_ra_account account;
    jw_db_resolve_ra_account_handoff(db, &account);
    expect(race_writes == 1, "writer ran between account key reads");
    expect(account.state == JW_RA_ACCOUNT_CONFIGURED && account.revision == 1 &&
           strcmp(account.user, "player-one") == 0 && strcmp(account.pass, "one") == 0,
           "in-flight reader keeps the complete old snapshot");
    jw_db_resolve_ra_account_handoff(db, &account);
    expect(account.revision == 2 && strcmp(account.user, "player-two") == 0 &&
           strcmp(account.pass, "two") == 0, "next reader gets the complete new snapshot");
    unlink(db);
}

/* Save or sign out after either the initial legacy read or its revision
   initialization. Neither gap may pair old credentials with a new revision. */
static void test_legacy_handoff_race(void) {
    for (int boundary = 1; boundary <= 2; boundary++) {
        for (int sign_out = 0; sign_out <= 1; sign_out++) {
            char db[64];
            fresh_db(db, sizeof(db));
            set(db, "retroachievements_user", "player-one");
            set(db, "retroachievements_pass", "one");
            race_db = db;
            race_close_count = boundary;
            race_sign_out = sign_out;
            race_writes = 0;
            jw_ra_account handed, stored;
            jw_db_resolve_ra_account_handoff(db, &handed);
            expect(race_writes == 1, "writer ran at the legacy transaction boundary");
            jw_db_resolve_ra_account(db, &stored);
            expect(handed.state == stored.state && handed.revision == stored.revision &&
                   strcmp(handed.user, stored.user) == 0 &&
                   strcmp(handed.pass, stored.pass) == 0,
                   "legacy handoff re-reads the whole committed account");
            unlink(db);
        }
    }
    race_sign_out = 0;
}

/* ------------------------------------------------------------------ */
/* Credential input validation                                        */
/* ------------------------------------------------------------------ */

static void test_credential_check(void) {
    expect(jw_ra_credentials_check_values("player-one", "correct horse") ==
           JW_RA_CREDENTIALS_OK, "ordinary credentials ok");
    expect(jw_ra_credentials_check_values("user; 'x'", "p\"q\\r`$") ==
           JW_RA_CREDENTIALS_OK, "punctuation ok");
    expect(jw_ra_credentials_check_values(NULL, "x") ==
           JW_RA_CREDENTIALS_INCOMPLETE, "NULL user incomplete");
    expect(jw_ra_credentials_check_values("u", "") ==
           JW_RA_CREDENTIALS_INCOMPLETE, "empty pass incomplete");
    expect(jw_ra_credentials_check_values("u\r", "x") ==
           JW_RA_CREDENTIALS_BAD_CHARS, "CR rejected");
    expect(jw_ra_credentials_check_values("u", "x\n") ==
           JW_RA_CREDENTIALS_BAD_CHARS, "LF rejected");
    expect(jw_ra_credentials_check_values("\xc3\x28", "x") ==
           JW_RA_CREDENTIALS_BAD_CHARS, "invalid UTF-8 rejected");
    expect(jw_ra_credentials_check_values("\xed\xa0\x80", "x") ==
           JW_RA_CREDENTIALS_BAD_CHARS, "UTF-16 surrogate rejected");
    expect(jw_ra_credentials_check_values("caf\xc3\xa9", "x") ==
           JW_RA_CREDENTIALS_OK, "valid multibyte ok");

    /* Byte limits, not character counts: 32 e-acute chars is 64 bytes. */
    char over_by_char[65 * 3];
    memset(over_by_char, 0, sizeof(over_by_char));
    for (int i = 0; i < 32; i++) strcat(over_by_char, "\xc3\xa9");
    expect(strlen(over_by_char) == 64, "fixture length");
    expect(jw_ra_credentials_check_values(over_by_char, "x") ==
           JW_RA_CREDENTIALS_TOO_LONG, "UTF-8 byte limit enforced");

    char exact_user[JW_RA_USERNAME_MAX + 1];
    memset(exact_user, 'a', JW_RA_USERNAME_MAX);
    exact_user[JW_RA_USERNAME_MAX] = '\0';
    expect(jw_ra_credentials_check_values(exact_user, "x") ==
           JW_RA_CREDENTIALS_OK, "username at limit ok");
    char exact_pass[JW_RA_PASSWORD_MAX + 1];
    memset(exact_pass, 'a', JW_RA_PASSWORD_MAX);
    exact_pass[JW_RA_PASSWORD_MAX] = '\0';
    expect(jw_ra_credentials_check_values("x", exact_pass) ==
           JW_RA_CREDENTIALS_OK, "password at limit ok");
}

/* ------------------------------------------------------------------ */
/* The launch resolve never fabricates a revision                     */
/* ------------------------------------------------------------------ */

static void sql(const char *db_path, const char *statement) {
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK ||
        sqlite3_exec(db, statement, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "ra-account-test: fixture SQL failed: %s\n",
                db ? sqlite3_errmsg(db) : "open");
        exit(1);
    }
    sqlite3_close(db);
}

static int credentials_wiped(const jw_ra_account *acc) {
    for (size_t i = 0; i < sizeof(acc->user); i++)
        if (acc->user[i]) return 0;
    for (size_t i = 0; i < sizeof(acc->pass); i++)
        if (acc->pass[i]) return 0;
    return acc->revision == 0;
}

static void test_handoff_never_fabricates_revision(void) {
    jw_ra_account acc;

    /* A legacy pair gets its revision in a checked write first. */
    {
        char db[64];
        fresh_db(db, sizeof(db));
        set(db, "retroachievements_user", "player-one");
        set(db, "retroachievements_pass", "correct horse");
        jw_db_resolve_ra_account_handoff(db, &acc);
        expect(acc.state == JW_RA_ACCOUNT_CONFIGURED && acc.revision == 1 &&
               strcmp(acc.user, "player-one") == 0,
               "handoff: legacy pair is initialized to revision 1");
        unlink(db);
    }

    /* The initialization write fails (the store refuses the revision row,
       as a full or failing card does): unreadable, never configured with
       revision 0. */
    {
        char db[64];
        fresh_db(db, sizeof(db));
        set(db, "retroachievements_user", "player-one");
        set(db, "retroachievements_pass", "correct horse");
        sql(db,
            "CREATE TRIGGER ra_revision_write_fails BEFORE INSERT ON settings "
            "WHEN NEW.key = 'retroachievements_revision' "
            "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END;");
        jw_db_resolve_ra_account_handoff(db, &acc);
        expect(acc.state == JW_RA_ACCOUNT_UNREADABLE,
               "handoff: failed revision write is unreadable");
        expect(credentials_wiped(&acc),
               "handoff: failed revision write carries no credentials");
        /* Nothing was committed: the pair is still legacy for the next try. */
        jw_ra_account raw;
        expect(jw_db_resolve_ra_account(db, &raw) == 0 &&
               raw.state == JW_RA_ACCOUNT_CONFIGURED && raw.revision == 0,
               "handoff: failed initialization committed nothing");
        unlink(db);
    }

    /* A read-only store (card flipped read-only) cannot take the write
       either. Root ignores file modes, so this case needs a normal user. */
    if (geteuid() != 0) {
        char dir[] = "/tmp/jawaka-ra-ro.XXXXXX";
        if (!mkdtemp(dir)) {
            perror("mkdtemp");
            exit(1);
        }
        char db[128];
        snprintf(db, sizeof(db), "%s/library.db", dir);
        set(db, "retroachievements_user", "player-one");
        set(db, "retroachievements_pass", "correct horse");
        chmod(db, 0444);
        chmod(dir, 0555);
        jw_db_resolve_ra_account_handoff(db, &acc);
        expect(acc.state == JW_RA_ACCOUNT_UNREADABLE && credentials_wiped(&acc),
               "handoff: read-only store with a legacy pair is unreadable");
        chmod(dir, 0755);
        chmod(db, 0644);
        unlink(db);
        rmdir(dir);
    }

    /* A malformed stored revision is invalid, whatever the pair says. */
    static const char *const malformed[] = { "+7", "0", "-3", " 7", "7.0", "abc" };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        char db[64];
        char what[96];
        fresh_db(db, sizeof(db));
        set(db, "retroachievements_user", "player-one");
        set(db, "retroachievements_pass", "correct horse");
        set(db, "retroachievements_revision", malformed[i]);
        jw_db_resolve_ra_account_handoff(db, &acc);
        snprintf(what, sizeof(what),
                 "handoff: malformed revision '%s' is invalid", malformed[i]);
        expect(acc.state == JW_RA_ACCOUNT_INVALID && credentials_wiped(&acc),
               what);
        unlink(db);
    }

    /* An unreadable store: not a database at all. */
    {
        char db[64];
        fresh_db(db, sizeof(db));
        FILE *f = fopen(db, "wb");
        if (!f) exit(1);
        fputs("this is not an SQLite database, it is a text file\n", f);
        fclose(f);
        jw_db_resolve_ra_account_handoff(db, &acc);
        expect(acc.state == JW_RA_ACCOUNT_UNREADABLE && credentials_wiped(&acc),
               "handoff: corrupt store is unreadable");
        unlink(db);
    }
    jw_db_resolve_ra_account_handoff(NULL, &acc);
    expect(acc.state == JW_RA_ACCOUNT_UNREADABLE && credentials_wiped(&acc),
           "handoff: no store path is unreadable");
}

int main(void) {
    test_save_clear_cycle();
    test_failed_save_preserves_prior();
    test_legacy_pair_initialized_once();
    test_malformed_stored_values();
    test_revision_overflow_fails();
    test_coherent_snapshot();
    test_legacy_handoff_race();
    test_credential_check();
    test_handoff_never_fabricates_revision();

    if (failures) {
        fprintf(stderr, "ra-account-test: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("ra-account-test: ok\n");
    return 0;
}
