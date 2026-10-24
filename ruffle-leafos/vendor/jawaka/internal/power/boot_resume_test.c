/* Resume Game on Boot record: write/parse/consume and the boot-time rows of
   the acceptance table that do not need a running daemon. */
#include "internal/power/boot_resume.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[] = "/tmp/jawaka-boot-resume.XXXXXX";
static char reason[64];

static void join(char *out, size_t size, const char *name) {
    assert(snprintf(out, size, "%s/%s", root, name) < (int)size);
}

static void write_file(const char *path, const char *text, size_t len) {
    FILE *fp = fopen(path, "wb");
    assert(fp);
    assert(fwrite(text, 1, len, fp) == len);
    assert(fclose(fp) == 0);
}

static void write_record_text(const char *text) {
    char path[512];
    join(path, sizeof(path), JW_BOOT_RESUME_FILENAME);
    unlink(path);
    write_file(path, text, strlen(text));
}

static bool exists(const char *name) {
    char path[512];
    join(path, sizeof(path), name);
    struct stat st;
    return lstat(path, &st) == 0;
}

static jw_boot_resume_record sample(void) {
    jw_boot_resume_record r;
    memset(&r, 0, sizeof(r));
    snprintf(r.platform, sizeof(r.platform), "mlp1");
    snprintf(r.boot_id, sizeof(r.boot_id), "5ad9e8d6-2b3f-4a1e-9c2a-0c3d4e5f6a7b");
    snprintf(r.request_id, sizeof(r.request_id), "a1b2c3d4e5f6");
    snprintf(r.source_fingerprint, sizeof(r.source_fingerprint), "uuid:1234-ABCD");
    snprintf(r.system, sizeof(r.system), "PS");
    snprintf(r.rom_path, sizeof(r.rom_path), "PS/Spyro (USA).chd");
    snprintf(r.core_id, sizeof(r.core_id), "pcsx_rearmed");
    snprintf(r.core_config_folder, sizeof(r.core_config_folder), "PCSX-ReARMed");
    r.provider[0] = '\0';
    r.slot = 99;
    snprintf(r.state_path, sizeof(r.state_path), "PCSX-ReARMed/Spyro (USA).state99");
    r.state_bytes = 4456472;
    return r;
}

/* Every field round-trips through the file exactly. */
static void roundtrip(void) {
    jw_boot_resume_record in = sample();
    snprintf(in.provider, sizeof(in.provider), "shared/ScummVM.pak");
    in.offline_proxy = true;
    assert(jw_boot_resume_write(root, &in, reason, sizeof(reason)));
    assert(strcmp(reason, "ok") == 0);
    assert(exists(JW_BOOT_RESUME_FILENAME));
    char temp[96];
    snprintf(temp, sizeof(temp), ".%s.tmp.%ld", JW_BOOT_RESUME_FILENAME, (long)getpid());
    assert(!exists(temp));

    jw_boot_resume_record out;
    assert(jw_boot_resume_load(root, "mlp1", &out, reason, sizeof(reason)) ==
           JW_BOOT_RESUME_LOAD_VALID);
    assert(memcmp(&in, &out, sizeof(in)) == 0);

    /* A second qualifying power-off replaces the record whole. */
    jw_boot_resume_record next = sample();
    snprintf(next.request_id, sizeof(next.request_id), "ffff00001111");
    assert(jw_boot_resume_write(root, &next, reason, sizeof(reason)));
    assert(jw_boot_resume_load(root, "mlp1", &out, reason, sizeof(reason)) ==
           JW_BOOT_RESUME_LOAD_VALID);
    assert(strcmp(out.request_id, "ffff00001111") == 0 && out.provider[0] == '\0' &&
           !out.offline_proxy);

    /* The record belongs to the platform that wrote it. */
    assert(jw_boot_resume_load(root, "tg5040", &out, reason, sizeof(reason)) ==
           JW_BOOT_RESUME_LOAD_INVALID);
    assert(strcmp(reason, "platform") == 0);
}

/* Record write fails: nothing is published and no temporary is left. */
static void write_failures(void) {
    char missing[512];
    join(missing, sizeof(missing), "no-such-dir");
    jw_boot_resume_record r = sample();
    assert(!jw_boot_resume_write(missing, &r, reason, sizeof(reason)));
    assert(strcmp(reason, "temp-open-failed") == 0);

    /* Invalid records never reach the disk. */
    r.slot = 98;
    char path[512];
    join(path, sizeof(path), JW_BOOT_RESUME_FILENAME);
    unlink(path);
    assert(!jw_boot_resume_write(root, &r, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-record") == 0);
    assert(!exists(JW_BOOT_RESUME_FILENAME));
}

static void expect_invalid(const char *text, const char *why) {
    write_record_text(text);
    jw_boot_resume_record out;
    jw_boot_resume_load_result result =
        jw_boot_resume_load(root, "mlp1", &out, reason, sizeof(reason));
    if (result != JW_BOOT_RESUME_LOAD_INVALID || strcmp(reason, why) != 0) {
        fprintf(stderr, "boot-resume-test: %s -> %d %s, want invalid %s\n",
                text, (int)result, reason, why);
        abort();
    }
    /* Rejected means rejected: nothing of it survives in the output. */
    assert(out.rom_path[0] == '\0' && out.state_path[0] == '\0');
}

#define GOOD_PREFIX \
    "{\"schema\":1,\"platform\":\"mlp1\"," \
    "\"boot_id\":\"5ad9e8d6-2b3f-4a1e-9c2a-0c3d4e5f6a7b\"," \
    "\"request_id\":\"a1b2c3d4e5f6\",\"source_fingerprint\":\"uuid:1234-ABCD\"," \
    "\"system\":\"PS\","

static char *record_with(const char *rom, const char *folder, const char *provider,
                         const char *slot, const char *state, const char *bytes) {
    static char buf[2048];
    snprintf(buf, sizeof(buf),
             GOOD_PREFIX "\"rom_path\":%s,\"core_id\":\"pcsx_rearmed\","
             "\"core_config_folder\":%s,\"provider\":%s,\"slot\":%s,"
             "\"state_path\":%s,\"state_bytes\":%s,\"offline_proxy\":false}",
             rom, folder, provider, slot, state, bytes);
    return buf;
}

/* Malformed, truncated, future-schema, absolute or traversal path record:
   rejected, nothing usable comes back. */
static void rejects(void) {
    const char *rom = "\"PS/Spyro (USA).chd\"";
    const char *folder = "\"PCSX-ReARMed\"";
    const char *state = "\"PCSX-ReARMed/Spyro (USA).state99\"";

    /* The baseline really is valid, so each row below fails on its own field. */
    write_record_text(record_with(rom, folder, "\"\"", "99", state, "4456472"));
    jw_boot_resume_record out;
    assert(jw_boot_resume_load(root, "mlp1", &out, reason, sizeof(reason)) ==
           JW_BOOT_RESUME_LOAD_VALID);

    expect_invalid("{", "malformed");
    expect_invalid("not json", "malformed");
    expect_invalid("[]", "malformed");
    char truncated[2048];
    const char *good = record_with(rom, folder, "\"\"", "99", state, "4456472");
    snprintf(truncated, sizeof(truncated), "%.*s", (int)(strlen(good) / 2), good);
    expect_invalid(truncated, "malformed");
    /* Trailing bytes after the object are not a record either. */
    char trailing[2100];
    snprintf(trailing, sizeof(trailing), "%s{}", good);
    expect_invalid(trailing, "malformed");

    expect_invalid("{\"schema\":2}", "schema");
    expect_invalid("{\"schema\":\"1\"}", "schema");
    expect_invalid("{\"schema\":1.5}", "schema");
    expect_invalid("{\"platform\":\"mlp1\"}", "schema");

    /* Shape: a missing key, an extra key, a wrong type. */
    expect_invalid(GOOD_PREFIX "\"rom_path\":\"PS/a.chd\"}", "fields");
    char extra[2100];
    snprintf(extra, sizeof(extra), "%.*s,\"sha256\":\"00\"}", (int)strlen(good) - 1, good);
    expect_invalid(extra, "fields");
    expect_invalid(record_with("7", folder, "\"\"", "99", state, "4456472"), "fields");
    expect_invalid(record_with(rom, folder, "null", "99", state, "4456472"), "fields");
    expect_invalid(record_with(rom, folder, "\"\"", "99", state, "-1"), "fields");
    expect_invalid(record_with(rom, folder, "\"\"", "99", state, "12.5"), "fields");
    expect_invalid(record_with(rom, folder, "\"\"", "\"99\"", state, "4456472"), "fields");
    expect_invalid(record_with(rom, folder, "\"\"", "1000", state, "4456472"), "fields");
    /* offline_proxy is a JSON boolean, never a string or a number. */
    char proxy_text[2100];
    snprintf(proxy_text, sizeof(proxy_text), "%s", good);
    char *flag = strstr(proxy_text, "\"offline_proxy\":false");
    assert(flag);
    memcpy(flag, "\"offline_proxy\":0    ", 21);
    expect_invalid(proxy_text, "fields");

    /* Absolute and traversal paths. */
    expect_invalid(record_with("\"/mnt/sdcard/Roms/PS/Spyro (USA).chd\"", folder, "\"\"",
                               "99", state, "4456472"), "rom_path");
    expect_invalid(record_with("\"PS/../../etc/passwd\"", folder, "\"\"",
                               "99", state, "4456472"), "rom_path");
    expect_invalid(record_with("\"\"", folder, "\"\"", "99", state, "4456472"), "fields");
    expect_invalid(record_with(rom, folder, "\"\"", "99",
                               "\"/mnt/sdcard/States/PCSX-ReARMed/Spyro (USA).state99\"",
                               "4456472"), "state_path");
    expect_invalid(record_with(rom, folder, "\"\"", "99",
                               "\"PCSX-ReARMed/../Other/Spyro (USA).state99\"",
                               "4456472"), "state_path");
    /* The state must be in the recorded core's own folder. */
    expect_invalid(record_with(rom, folder, "\"\"", "99",
                               "\"Other Core/Spyro (USA).state99\"", "4456472"), "state_path");
    expect_invalid(record_with(rom, folder, "\"\"", "99",
                               "\"PCSX-ReARMed/deeper/Spyro (USA).state99\"", "4456472"),
                   "state_path");
    expect_invalid(record_with(rom, "\"../PCSX\"", "\"\"", "99",
                               "\"../PCSX/Spyro (USA).state99\"", "4456472"),
                   "core_config_folder");
    expect_invalid(record_with(rom, folder, "\"/Apps/x.pak\"", "99", state, "4456472"),
                   "provider");
    expect_invalid(record_with(rom, folder, "\"\"", "98", state, "4456472"), "slot");
    expect_invalid(record_with(rom, folder, "\"\"", "99", state, "0"), "state_bytes");

    /* A fingerprint that names a mount, not a card. */
    char by_dev[2048];
    snprintf(by_dev, sizeof(by_dev), "%s", good);
    char *fp = strstr(by_dev, "uuid:1234-ABCD");
    assert(fp);
    memcpy(fp, "dev:179:65    ", 14);
    expect_invalid(by_dev, "source_fingerprint");

    /* Not a regular file, or not a sane size. */
    char path[512];
    join(path, sizeof(path), JW_BOOT_RESUME_FILENAME);
    unlink(path);
    write_file(path, "", 0);
    assert(jw_boot_resume_load(root, "mlp1", &out, reason, sizeof(reason)) ==
           JW_BOOT_RESUME_LOAD_INVALID && strcmp(reason, "size") == 0);
    unlink(path);
    char target[512];
    join(target, sizeof(target), "elsewhere.json");
    write_file(target, good, strlen(good));
    assert(symlink(target, path) == 0);
    assert(jw_boot_resume_load(root, "mlp1", &out, reason, sizeof(reason)) ==
           JW_BOOT_RESUME_LOAD_INVALID && strcmp(reason, "not-a-file") == 0);
    unlink(path);
    unlink(target);
    char *big = malloc(20000);
    assert(big);
    memset(big, ' ', 20000);
    memcpy(big, good, strlen(good));
    write_file(path, big, 20000);
    free(big);
    assert(jw_boot_resume_load(root, "mlp1", &out, reason, sizeof(reason)) ==
           JW_BOOT_RESUME_LOAD_INVALID && strcmp(reason, "size") == 0);
    unlink(path);
    write_file(path, "{\"schema\":1}\0junk", 17);
    assert(jw_boot_resume_load(root, "mlp1", &out, reason, sizeof(reason)) ==
           JW_BOOT_RESUME_LOAD_INVALID && strcmp(reason, "malformed") == 0);
    unlink(path);

    assert(jw_boot_resume_load(root, "mlp1", &out, reason, sizeof(reason)) ==
           JW_BOOT_RESUME_LOAD_ABSENT);
}

/* Delete first: consume removes the record and reports whether that is
   certain. A record it cannot remove (read-only card) must not be acted on. */
static void consume(void) {
    jw_boot_resume_record r = sample();
    assert(jw_boot_resume_write(root, &r, reason, sizeof(reason)));
    assert(jw_boot_resume_consume(root, reason, sizeof(reason)));
    assert(!exists(JW_BOOT_RESUME_FILENAME));
    /* Nothing there is still a confirmed absence. */
    assert(jw_boot_resume_consume(root, reason, sizeof(reason)));

    /* Removal that cannot happen is reported, even to root: a directory with
       an entry in it stands in for a card that refuses the unlink. */
    char path[512];
    char inner[600];
    join(path, sizeof(path), JW_BOOT_RESUME_FILENAME);
    assert(mkdir(path, 0755) == 0);
    snprintf(inner, sizeof(inner), "%s/keep", path);
    write_file(inner, "x", 1);
    assert(!jw_boot_resume_consume(root, reason, sizeof(reason)));
    assert(strcmp(reason, "unlink-failed") == 0);
    assert(exists(JW_BOOT_RESUME_FILENAME));
    unlink(inner);
    rmdir(path);

    char missing[512];
    join(missing, sizeof(missing), "no-such-dir");
    assert(!jw_boot_resume_consume(missing, reason, sizeof(reason)));
}

static void decide(void) {
    jw_boot_resume_record r = sample();
    jw_boot_resume_boot_facts f = {
        .current_boot_id = "11111111-2222-3333-4444-555555555555",
        .save_setting_on = true, .resume_setting_on = true,
        .storage_recovery = false, .bypass_held = false,
    };
    assert(jw_boot_resume_decide(&r, &f) == JW_BOOT_RESUME_PROCEED);

    jw_boot_resume_boot_facts g = f;
    g.save_setting_on = false;
    assert(jw_boot_resume_decide(&r, &g) == JW_BOOT_RESUME_DISCARD_SETTING_OFF);
    g = f;
    g.resume_setting_on = false;
    assert(jw_boot_resume_decide(&r, &g) == JW_BOOT_RESUME_DISCARD_SETTING_OFF);

    /* Same-boot record: a daemon restart or a direct jawakad. */
    g = f;
    g.current_boot_id = r.boot_id;
    assert(jw_boot_resume_decide(&r, &g) == JW_BOOT_RESUME_DISCARD_SAME_BOOT);
    g.current_boot_id = "";
    assert(jw_boot_resume_decide(&r, &g) == JW_BOOT_RESUME_DISCARD_BOOT_UNKNOWN);
    g.current_boot_id = NULL;
    assert(jw_boot_resume_decide(&r, &g) == JW_BOOT_RESUME_DISCARD_BOOT_UNKNOWN);

    g = f;
    g.bypass_held = true;
    assert(jw_boot_resume_decide(&r, &g) == JW_BOOT_RESUME_DISCARD_BYPASS);

    /* Storage repair or recovery wins over everything. */
    g = f;
    g.storage_recovery = true;
    g.bypass_held = true;
    g.resume_setting_on = false;
    assert(jw_boot_resume_decide(&r, &g) == JW_BOOT_RESUME_DISCARD_STORAGE_RECOVERY);

    assert(strcmp(jw_boot_resume_decision_name(JW_BOOT_RESUME_DISCARD_BYPASS), "bypass") == 0);
}

static void relative(void) {
    char out[512];
    assert(jw_boot_resume_relative_to("/mnt/sdcard/States",
                                      "/mnt/sdcard/States/PCSX-ReARMed/a.state99",
                                      out, sizeof(out)));
    assert(strcmp(out, "PCSX-ReARMed/a.state99") == 0);
    assert(jw_boot_resume_relative_to("/mnt/sdcard/States/",
                                      "/mnt/sdcard/States/PCSX-ReARMed/a.state99",
                                      out, sizeof(out)));
    assert(strcmp(out, "PCSX-ReARMed/a.state99") == 0);
    assert(!jw_boot_resume_relative_to("/mnt/sdcard/States",
                                       "/mnt/sdcard/States2/PCSX/a.state99",
                                       out, sizeof(out)));
    assert(!jw_boot_resume_relative_to("/mnt/sdcard/States", "/mnt/sdcard/States",
                                       out, sizeof(out)));
    assert(!jw_boot_resume_relative_to("/mnt/sdcard/States",
                                       "/mnt/sdcard/States/../Saves/a.srm",
                                       out, sizeof(out)));
    assert(!jw_boot_resume_relative_to("/mnt/sdcard/States", "/media/sdcard1/States/a",
                                       out, sizeof(out)));
}

/* Missing or zero-length state, size mismatch, a different slot: never
   another file. */
static void state_file(void) {
    char states[512], core_dir[600], rom[512], state[700], out[700];
    join(states, sizeof(states), "States");
    assert(mkdir(states, 0755) == 0);
    snprintf(core_dir, sizeof(core_dir), "%s/PCSX-ReARMed", states);
    assert(mkdir(core_dir, 0755) == 0);
    join(rom, sizeof(rom), "Spyro (USA).chd");
    snprintf(state, sizeof(state), "%s/Spyro (USA).state99", core_dir);

    jw_boot_resume_record r = sample();
    r.state_bytes = 6;
    assert(!jw_boot_resume_state_file_ok(&r, states, rom, out, sizeof(out),
                                         reason, sizeof(reason)));
    assert(strcmp(reason, "state-missing") == 0 && out[0] == '\0');

    /* Another slot of the same game is not a substitute. */
    char other[700];
    snprintf(other, sizeof(other), "%s/Spyro (USA).state98", core_dir);
    write_file(other, "STATE!", 6);
    assert(!jw_boot_resume_state_file_ok(&r, states, rom, out, sizeof(out),
                                         reason, sizeof(reason)));
    assert(strcmp(reason, "state-missing") == 0);

    write_file(state, "", 0);
    assert(!jw_boot_resume_state_file_ok(&r, states, rom, out, sizeof(out),
                                         reason, sizeof(reason)));
    assert(strcmp(reason, "state-size") == 0);
    write_file(state, "STATE", 5);
    assert(!jw_boot_resume_state_file_ok(&r, states, rom, out, sizeof(out),
                                         reason, sizeof(reason)));
    assert(strcmp(reason, "state-size") == 0);
    write_file(state, "STATE!", 6);
    assert(jw_boot_resume_state_file_ok(&r, states, rom, out, sizeof(out),
                                        reason, sizeof(reason)));
    assert(strcmp(out, state) == 0);

    /* The recorded name must be the one the launcher would build for this ROM. */
    jw_boot_resume_record renamed = r;
    snprintf(renamed.state_path, sizeof(renamed.state_path),
             "PCSX-ReARMed/Crash (USA).state99");
    assert(!jw_boot_resume_state_file_ok(&renamed, states, rom, out, sizeof(out),
                                         reason, sizeof(reason)));
    assert(strcmp(reason, "state-path-mismatch") == 0);

    /* A link in the slot's place is not the published file. */
    unlink(state);
    assert(symlink(other, state) == 0);
    assert(!jw_boot_resume_state_file_ok(&r, states, rom, out, sizeof(out),
                                         reason, sizeof(reason)));
    assert(strcmp(reason, "state-not-a-file") == 0);
    unlink(state);
    assert(mkdir(state, 0755) == 0);
    assert(!jw_boot_resume_state_file_ok(&r, states, rom, out, sizeof(out),
                                         reason, sizeof(reason)));
    assert(strcmp(reason, "state-not-a-file") == 0);
    rmdir(state);
    unlink(other);
    rmdir(core_dir);
    rmdir(states);
}

int main(void) {
    assert(mkdtemp(root));
    roundtrip();
    write_failures();
    rejects();
    consume();
    decide();
    relative();
    state_file();
    char path[512];
    join(path, sizeof(path), JW_BOOT_RESUME_FILENAME);
    unlink(path);
    assert(rmdir(root) == 0);
    puts("PASS boot-resume-test");
    return 0;
}
