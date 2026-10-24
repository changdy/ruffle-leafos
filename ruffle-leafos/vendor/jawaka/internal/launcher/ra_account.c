#include "internal/launcher/ra_account.h"

#include "internal/platform/leaf_version.h"

#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *jw_ra_account_state_name(jw_ra_account_state state) {
    switch (state) {
    case JW_RA_ACCOUNT_CONFIGURED:       return "configured";
    case JW_RA_ACCOUNT_NEVER_CONFIGURED: return "never-configured";
    case JW_RA_ACCOUNT_SIGNED_OUT:       return "signed-out";
    case JW_RA_ACCOUNT_INVALID:          return "invalid";
    case JW_RA_ACCOUNT_UNREADABLE:       return "unreadable";
    }
    return NULL;
}

/* Read a small regular file byte-exactly. Returns the number of bytes read
   (an empty file is 0) with out NUL-terminated after them; -1 when the path
   is missing, not a regular file, unreadable, or holds more than
   out_size - 1 bytes. The count, not strlen(out), is the content length:
   the bytes may contain NUL. */
static long jw__ra_read_file(const char *path, char *out, size_t out_size) {
    out[0] = '\0';
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(out, 1, out_size - 1, f);
    /* One more byte means the file is larger than the buffer: refuse it
       rather than judge a prefix. */
    int too_large = (n == out_size - 1) && fgetc(f) != EOF;
    int read_failed = ferror(f);
    fclose(f);
    if (read_failed || too_large) {
        out[0] = '\0';
        return -1;
    }
    out[n] = '\0';
    return (long)n;
}

/* The capability record is exactly the contract id, optionally followed by
   one trailing newline: its full byte length is strlen(id) or strlen(id) + 1
   with a final '\n', and nothing else (no NUL, CR, spaces or trailing
   bytes). No versions, no flags: support is advertised by the payload
   carrying the record at all, inside a target directory whose ownership this
   function's caller already established. */
static bool jw__ra_capability_matches(const char *content, size_t len,
                                      const char *id) {
    size_t id_len = strlen(id);
    if (len == id_len + 1 && content[id_len] == '\n') len--;
    return len == id_len && memcmp(content, id, id_len) == 0;
}

/* <dir>/<name> is a regular file holding exactly id. */
static bool jw__ra_record_matches(const char *dir, const char *name,
                                  const char *id) {
    char path[1024];
    char content[64];
    if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path)) {
        return false;
    }
    long len = jw__ra_read_file(path, content, sizeof(content));
    return len >= 0 && jw__ra_capability_matches(content, (size_t)len, id);
}

/* <platform_dir>/emulators/flycast/<name> holds exactly id. */
static bool jw__ra_flycast_record_matches(const char *platform_dir,
                                          const char *name, const char *id) {
    char dir[1024];
    if (snprintf(dir, sizeof(dir), "%s/emulators/flycast", platform_dir) >=
        (int)sizeof(dir)) {
        return false;
    }
    return jw__ra_record_matches(dir, name, id);
}

/* DSperate's account adapter first shipped in pak 2.1.1. Older installed
   builds ignore the snapshot and must receive none, which is exactly the
   "unknown/older consumer gets no secrets" rule. */
static bool jw__ra_dsperate_version_capable(const char *text) {
    int version[3];
    if (jw_pak_version_parse(text, version) != 0) return false;
    return version[0] > 2 || (version[0] == 2 && version[1] > 1) ||
           (version[0] == 2 && version[1] == 1 && version[2] >= 1);
}

static bool jw__ra_dsperate_manifest_capable(const char *pak_json_path) {
    char *raw = malloc(65536);
    if (!raw) return false;
    bool capable = false;
    if (jw__ra_read_file(pak_json_path, raw, 65536) >= 0) {
        cJSON *root = cJSON_Parse(raw);
        if (root) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
            const cJSON *version =
                cJSON_GetObjectItemCaseSensitive(root, "pak_version");
            capable = cJSON_IsString(id) && id->valuestring &&
                      strcmp(id->valuestring, "org.umrk.dsperate") == 0 &&
                      cJSON_IsString(version) && version->valuestring &&
                      jw__ra_dsperate_version_capable(version->valuestring);
            cJSON_Delete(root);
        }
    }
    free(raw);
    return capable;
}

/* The launcher path must be exactly <...>/mlp1/DSperate.pak/scripts/run.sh;
   the pak root for the manifest check falls out of the same path. */
static bool jw__ra_dsperate_launcher_root(const char *launcher_path,
                                          char *out, size_t out_size) {
    static const char suffix[] = "/mlp1/DSperate.pak/scripts/run.sh";
    size_t path_len = strlen(launcher_path);
    size_t suffix_len = sizeof(suffix) - 1;
    if (path_len <= suffix_len ||
        strcmp(launcher_path + path_len - suffix_len, suffix) != 0) {
        return false;
    }
    size_t root_len = path_len - suffix_len + strlen("/mlp1/DSperate.pak");
    if (root_len + 1 > out_size) return false;
    memcpy(out, launcher_path, root_len);
    out[root_len] = '\0';
    return true;
}

bool jw_ra_account_target_authorized(const char *launcher_path,
                                     const char *core_id,
                                     const jw_standalone_policy *policy,
                                     const char *provider,
                                     const char *platform_dir) {
    if (!launcher_path || !launcher_path[0] || !core_id || !policy ||
        !platform_dir || !platform_dir[0]) {
        return false;
    }

    if (!policy->provider_bound &&
        policy->release == JW_STANDALONE_RELEASE_FLYCAST) {
        char expected[1024];
        if (snprintf(expected, sizeof(expected),
                     "%s/emulators/flycast/launch.sh", platform_dir) >=
                (int)sizeof(expected) ||
            strcmp(launcher_path, expected) != 0) {
            return false;
        }
        return jw__ra_flycast_record_matches(platform_dir, "ra-account-v1",
                                             JW_RA_ACCOUNT_CONTRACT_ID);
    }

    if (policy->provider_bound && provider &&
        strcmp(provider, "mlp1/DSperate.pak") == 0 &&
        strcmp(core_id, "dsperate") == 0) {
        char pak_root[1024];
        char manifest[1024 + 12];
        return jw__ra_dsperate_launcher_root(launcher_path, pak_root,
                                             sizeof(pak_root)) &&
               snprintf(manifest, sizeof(manifest), "%s/pak.json", pak_root) <
                   (int)sizeof(manifest) &&
               jw__ra_dsperate_manifest_capable(manifest) &&
               /* The version says the build could consume the contract; the
                  shipped record says this installed payload does. */
               jw__ra_record_matches(pak_root, "ra-account-v1",
                                     JW_RA_ACCOUNT_CONTRACT_ID);
    }

    return false;
}

bool jw_flycast_ra_route_target_authorized(const char *launcher_path,
                                           const char *core_id,
                                           const jw_standalone_policy *policy,
                                           const char *provider,
                                           const char *platform_dir) {
    /* The account authorization already pins the exact release-owned
       launcher; repeating the release check here keeps DSperate, which is
       also account-authorized, out of the route. */
    return policy && !policy->provider_bound &&
           policy->release == JW_STANDALONE_RELEASE_FLYCAST &&
           jw_ra_account_target_authorized(launcher_path, core_id, policy,
                                           provider, platform_dir) &&
           jw__ra_flycast_record_matches(platform_dir, "ra-route-v1",
                                         JW_FLYCAST_RA_ROUTE_CAPABILITY_ID);
}

const char *jw_flycast_ra_route_value(bool service_live) {
    return service_live ? JW_FLYCAST_RA_ROUTE_SERVICE_LIVE
                        : JW_FLYCAST_RA_ROUTE_NATIVE;
}

void jw_ra_account_clear_env(void) {
    unsetenv(JW_RA_ACCOUNT_ENV_VERSION);
    unsetenv(JW_RA_ACCOUNT_ENV_STATE);
    unsetenv(JW_RA_ACCOUNT_ENV_USERNAME);
    unsetenv(JW_RA_ACCOUNT_ENV_PASSWORD);
    unsetenv(JW_RA_ACCOUNT_ENV_REVISION);
}

static bool jw__ra_revision_in_range(long long revision) {
    return revision >= 1 && revision <= JW_RA_REVISION_MAX;
}

void jw_ra_account_apply_env(const jw_ra_account *account) {
    jw_ra_account_clear_env();
    if (!account || !jw_ra_account_state_name(account->state)) {
        return;
    }
    jw_ra_account_state state = account->state;
    if (state == JW_RA_ACCOUNT_CONFIGURED) {
        if (jw_ra_credentials_check_values(account->user, account->pass) !=
            JW_RA_CREDENTIALS_OK) {
            state = JW_RA_ACCOUNT_INVALID;
        } else if (!jw__ra_revision_in_range(account->revision)) {
            /* Never CONFIGURED without an established counter. */
            state = JW_RA_ACCOUNT_UNREADABLE;
        }
    } else if (state == JW_RA_ACCOUNT_SIGNED_OUT &&
               !jw__ra_revision_in_range(account->revision)) {
        state = JW_RA_ACCOUNT_UNREADABLE;
    }

    setenv(JW_RA_ACCOUNT_ENV_VERSION, "1", 1);
    setenv(JW_RA_ACCOUNT_ENV_STATE, jw_ra_account_state_name(state), 1);
    if (state == JW_RA_ACCOUNT_CONFIGURED || state == JW_RA_ACCOUNT_SIGNED_OUT) {
        char revision[32];
        snprintf(revision, sizeof(revision), "%lld", account->revision);
        if (state == JW_RA_ACCOUNT_CONFIGURED) {
            setenv(JW_RA_ACCOUNT_ENV_USERNAME, account->user, 1);
            setenv(JW_RA_ACCOUNT_ENV_PASSWORD, account->pass, 1);
        }
        setenv(JW_RA_ACCOUNT_ENV_REVISION, revision, 1);
    }
}

void jw_ra_account_clear_retroarch_env(void) {
    unsetenv("JAWAKA_CHEEVOS_USERNAME");
    unsetenv("JAWAKA_CHEEVOS_PASSWORD");
}

void jw_ra_account_apply_retroarch_env(const jw_ra_account *account) {
    if (account && account->state == JW_RA_ACCOUNT_CONFIGURED &&
        account->user[0] && account->pass[0]) {
        setenv("JAWAKA_CHEEVOS_USERNAME", account->user, 1);
        setenv("JAWAKA_CHEEVOS_PASSWORD", account->pass, 1);
    } else {
        jw_ra_account_clear_retroarch_env();
    }
}

void jw_ra_account_prepare_standalone_env(const jw_ra_account *account,
                                          bool authorized) {
    jw_ra_account_clear_retroarch_env();
    jw_ra_account_clear_env();
    if (authorized) {
        jw_ra_account_apply_env(account);
    }
}
