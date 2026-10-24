#define _POSIX_C_SOURCE 200809L

#include "internal/platform/paths.h"
#include "third_party/retroarch/config_file_parser.inc"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int fail(const char *message) {
    fprintf(stderr, "retroarch-config-test: %s\n", message);
    return 1;
}

static int mkdir_one(const char *path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t len = strlen(text);
    return fwrite(text, 1, len, fp) == len && fclose(fp) == 0 ? 0 : -1;
}

static int path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

static char *read_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp || fseek(fp, 0, SEEK_END) != 0) return NULL;
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *text = calloc((size_t)size + 1u, 1u);
    if (!text || fread(text, 1, (size_t)size, fp) != (size_t)size) {
        free(text);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return text;
}

/* Stand in for RetroArch's save-on-exit, which rewrites the whole --config
   file with a single line per key. Changing a value in place is what that
   looks like; prepending a duplicate is not, and would be resolved by the
   backup's dedupe rather than by the user's edit. */
static char *replace_line(const char *text, const char *old_line,
                          const char *new_line) {
    const char *at = text ? strstr(text, old_line) : NULL;
    if (!at) return NULL;
    size_t head = (size_t)(at - text);
    size_t old_len = strlen(old_line);
    char *out = malloc(strlen(text) - old_len + strlen(new_line) + 1u);
    if (!out) return NULL;
    memcpy(out, text, head);
    strcpy(out + head, new_line);
    strcat(out + head, at + old_len);
    return out;
}

/* How many times a key appears at all, regardless of value. A protected key
   must be written exactly once: twice would mean a persisted copy survived
   alongside ours, and RetroArch takes the last one. */
static int occurrences(const char *text, const char *key) {
    int count = 0;
    size_t key_len = strlen(key);
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > key_len && strncmp(line, key, key_len) == 0 &&
            (line[key_len] == ' ' || line[key_len] == '=')) {
            count++;
        }
        line = end ? end + 1 : NULL;
    }
    return count;
}

static int jw__mkdir_p_test(const char *path) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(buf, 0755); *p = '/'; }
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int key_count(const char *text, const char *key, const char *value) {
    int count = 0;
    size_t key_len = strlen(key);
    for (const char *line = text; line && *line;) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        /* The value search must stop at the end of THIS line. `line` points
           into the whole text, so an unbounded strstr() runs on into later
           keys: searching for "false" from a line holding "true" would happily
           match some other key's value further down the file. */
        if (len >= key_len && strncmp(line, key, key_len) == 0) {
            if (!value) {
                count++;
            } else {
                char buf[1024];
                size_t copy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
                memcpy(buf, line, copy);
                buf[copy] = '\0';
                if (strstr(buf, value)) {
                    count++;
                }
            }
        }
        line = next ? next + 1 : NULL;
    }
    return count;
}

/* Look `key` up in config text the way RetroArch v1.22.2 does, returning a
   malloc'd copy of its value or NULL. Lines split on '\n' as
   filestream_getline does; the per-line steps restate the non-directive path
   of config_file_parse_line and call the vendored config_file_strip_comment
   and config_file_extract_value for the parts that decide a value. The first
   occurrence wins, as in config_file_load_internal. */
static char *ra_parse_value(const char *text, const char *key) {
    char *copy = strdup(text ? text : "");
    char *found = NULL;
    if (!copy) return NULL;
    for (char *line = copy; line && !found;) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        if (*line && !config_file_strip_comment(line)) {
            char *p = line;
            while (*p == ' ' || *p == '\t' || *p == '\r') p++;
            char *k = p;
            while (isgraph((unsigned char)*p)) p++;
            size_t key_len = (size_t)(p - k);
            while (*p == ' ' || *p == '\t' || *p == '\r') p++;
            if (*p == '=' && key_len == strlen(key) &&
                strncmp(k, key, key_len) == 0) {
                found = config_file_extract_value(p + 1);
            }
        }
        line = next;
    }
    free(copy);
    return found;
}

static int verify_runtime(const char *path, const char *shader_dir) {
    char *text = read_text(path);
    if (!text) return -1;
    int ok = key_count(text, "sort_savefiles_enable", NULL) == 1 &&
             key_count(text, "sort_savefiles_enable", "= \"true\"") == 1 &&
             key_count(text, "sort_savestates_enable", NULL) == 1 &&
             key_count(text, "sort_savestates_enable", "= \"true\"") == 1 &&
             (!shader_dir ||
              key_count(text, "video_shader_dir", shader_dir) == 1);
    free(text);
    return ok ? 0 : -1;
}

#ifdef PLATFORM_MLP1
static int verify_recordings(const char *path, const char *recordings_dir) {
    char *text = read_text(path);
    if (!text) return -1;
    int ok = key_count(text, "recording_output_directory", recordings_dir) == 1;
    free(text);
    return ok ? 0 : -1;
}

static const char *const hotkey_keys[5] = {
    "input_enable_hotkey_btn",
    "input_hotkey_block_delay",
    "input_exit_emulator_btn",
    "input_menu_toggle_btn",
    "input_menu_toggle_gamepad_combo",
};

/* Assert both the value and the occurrence count. A duplicated key makes a
   plain text search look right while RetroArch uses the first, wrong value.
   A NULL expectation means the key must be absent entirely, which is what the
   durable config must look like for the protected exit bind. */
static int verify_hotkeys(const char *path, const char *label,
                          const char *modifier, const char *delay,
                          const char *exit_btn, const char *menu_toggle,
                          const char *combo) {
    const char *want[5] = {modifier, delay, exit_btn, menu_toggle, combo};
    char *text = read_text(path);
    if (!text) return -1;
    int ok = 1;
    for (size_t i = 0; i < 5; i++) {
        int total = key_count(text, hotkey_keys[i], NULL);
        int matching = 0;
        if (want[i]) {
            char needle[64];
            snprintf(needle, sizeof(needle), "= \"%s\"", want[i]);
            matching = key_count(text, hotkey_keys[i], needle);
        }
        int key_ok = want[i] ? (total == 1 && matching == 1) : (total == 0);
        if (!key_ok) {
            fprintf(stderr,
                    "retroarch-config-test: %s: %s: want %s, got %d line(s)\n",
                    label, hotkey_keys[i],
                    want[i] ? want[i] : "no line", total);
            ok = 0;
        }
    }
    free(text);
    return ok ? 0 : -1;
}
#endif

/* One named assertion: prints which one failed and yields 0/1 for &=. */
static int expect_ok(int condition, const char *label) {
    if (!condition) {
        fprintf(stderr, "retroarch-config-test: FAILED: %s\n", label);
    }
    return condition ? 1 : 0;
}

/* The runtime config text for one launch with the given frontend profile
   (NULL: Leaf's own RetroArch). The file is removed again. */
static char *prepare_text(const char *runtime, const char *root,
                          const char *core,
                          const jw_retroarch_frontend_profile *profile,
                          char *error, size_t error_size) {
    char *cfg = jw_prepare_retroarch_config_with_info(
        runtime, root, core, NULL, NULL, true, false, profile,
        error, error_size);
    if (!cfg) return NULL;
    char *text = read_text(cfg);
    unlink(cfg);
    free(cfg);
    return text;
}

/* `text` with every line of the listed keys dropped. */
static char *without_keys(const char *text, const char *const *keys,
                          size_t key_count_in) {
    char *out = calloc(strlen(text) + 1u, 1u);
    if (!out) return NULL;
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) + 1u : strlen(line);
        int drop = 0;
        for (size_t i = 0; i < key_count_in; i++) {
            size_t klen = strlen(keys[i]);
            if (len > klen && strncmp(line, keys[i], klen) == 0 &&
                (line[klen] == ' ' || line[klen] == '=')) {
                drop = 1;
            }
        }
        if (!drop) strncat(out, line, len);
        line = end ? end + 1 : NULL;
    }
    return out;
}

/* The keys a LEAF-RA-1 frontend profile may change: recording and audio. */
static const char *const profile_keys[] = {
    "record_driver", "video_record_config", "video_record_quality",
    "recording_output_directory", "audio_driver", "audio_device",
};

int main(void) {
    char root[] = "/tmp/jw-retroarch-config-XXXXXX";
    int fd = mkstemp(root);
    if (fd < 0) return fail("mkstemp failed");
    close(fd);
    if (unlink(root) != 0 || mkdir_one(root) != 0) return fail("root mkdir failed");

    char platform[PATH_MAX], defaults[PATH_MAX], internal[PATH_MAX];
    char runtime[PATH_MAX], cores[PATH_MAX], shaders[PATH_MAX];
    char user_shaders[PATH_MAX], custom_shaders[PATH_MAX];
    char default_cfg[PATH_MAX], record_cfg[PATH_MAX], recordings_dir[PATH_MAX];
    char retroarch_dir[PATH_MAX], shared_cfg[PATH_MAX];
    snprintf(platform, sizeof(platform), "%s/platform", root);
    snprintf(defaults, sizeof(defaults), "%s/defaults", platform);
    snprintf(internal, sizeof(internal), "%s/internal", root);
    snprintf(runtime, sizeof(runtime), "%s/runtime", root);
    snprintf(cores, sizeof(cores), "%s/cores", platform);
    snprintf(shaders, sizeof(shaders), "%s/shaders", platform);
    snprintf(user_shaders, sizeof(user_shaders),
             "%s/retroarch/.config/retroarch/shaders", internal);
    snprintf(custom_shaders, sizeof(custom_shaders), "%s/custom-shaders", root);
    snprintf(retroarch_dir, sizeof(retroarch_dir), "%s/retroarch", internal);
    snprintf(shared_cfg, sizeof(shared_cfg), "%s/retroarch.cfg", retroarch_dir);
    if (mkdir_one(platform) || mkdir_one(defaults) || mkdir_one(internal) ||
        mkdir_one(runtime) || mkdir_one(cores) || mkdir_one(shaders) ||
        mkdir_one(custom_shaders) || mkdir_one(retroarch_dir)) {
        return fail("fixture mkdir failed");
    }
    snprintf(default_cfg, sizeof(default_cfg), "%s/retroarch.cfg", defaults);
    snprintf(record_cfg, sizeof(record_cfg), "%s/retroarch-record.cfg", defaults);
    /* Mirrors the five hotkey values device/mlp1/defaults/retroarch.cfg ships,
       so the migration cases below see the real packaged baseline rather than
       an empty one. */
    static const char packaged_defaults[] =
        "video_vsync = \"true\"\n"
        "sort_savefiles_enable = \"false\"\n"
        "sort_savestates_enable = \"false\"\n"
        "sort_savefiles_enable = \"false\"\n"
        "input_menu_toggle_btn = \"nul\"\n"
        "input_menu_toggle_gamepad_combo = \"0\"\n"
        "input_enable_hotkey_btn = \"5\"\n"
        "input_hotkey_block_delay = \"0\"\n"
        "input_exit_emulator_btn = \"nul\"\n";
    if (write_text(default_cfg, packaged_defaults) != 0) {
        return fail("defaults write failed");
    }
    if (write_text(record_cfg, "# recording preset fixture\n") != 0) {
        return fail("recording preset write failed");
    }
    if (write_text(shared_cfg,
                   "menu_driver = \"rgui\"\n"
                   "sort_savefiles_enable = \"false\"\n"
                   "sort_savestates_enable = \"false\"\n"
                   "sort_savestates_enable = \"false\"\n") != 0) {
        return fail("persisted config write failed");
    }

    setenv("SDCARD_PATH", root, 1);
    setenv("UMRK_PLATFORM_PATH", platform, 1);
    setenv("UMRK_INTERNAL_DATA_PATH", internal, 1);
    setenv("UMRK_RETROARCH_SHADERS_DIR", shaders, 1);
    setenv("UMRK_RETROARCH_USER_SHADERS_DIR", user_shaders, 1);
    {
        char expected[PATH_MAX];
        snprintf(expected, sizeof(expected), "%s/manifest.json", shaders);
        char *manifest = jw_retroarch_shader_manifest_path();
        if (!manifest || strcmp(manifest, expected) != 0) {
            free(manifest);
            return fail("release shader manifest path does not follow the runtime contract");
        }
        free(manifest);
    }
    if (!jw_primary_recordings_path(recordings_dir, sizeof(recordings_dir), root)) {
        return fail("could not resolve primary recordings path");
    }

    char core[PATH_MAX];
    snprintf(core, sizeof(core), "%s/mgba_libretro.so", cores);

    char error[256];
    char *runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                    true, false, error, sizeof(error));
    if (!runtime_cfg || verify_runtime(runtime_cfg, user_shaders) != 0) {
        return fail(error[0] ? error : "protected keys not normalized");
    }
#ifdef PLATFORM_MLP1
    if (verify_recordings(runtime_cfg, recordings_dir) != 0) {
        return fail("RetroArch recording directory disagrees with the primary path");
    }
#endif
    struct stat user_shader_stat;
    if (stat(user_shaders, &user_shader_stat) != 0 ||
        !S_ISDIR(user_shader_stat.st_mode)) {
        return fail("durable user shader directory was not created");
    }
    if (jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                   error, sizeof(error)) != 0) {
        return fail(error[0] ? error : "backup failed");
    }

    char *shared = read_text(shared_cfg);
    if (!shared || key_count(shared, "sort_savefiles_enable", NULL) != 0 ||
        key_count(shared, "sort_savestates_enable", NULL) != 0) {
        free(shared);
        return fail("protected keys persisted to shared config");
    }
    free(shared);
    unlink(runtime_cfg);
    free(runtime_cfg);

    /* S-2: the launch owns an immutable catalog snapshot. Its merged info
       directory must beat the core-adjacent/default INFO_PATH guess. */
    {
        char launch_info[PATH_MAX];
        snprintf(launch_info, sizeof(launch_info), "%s/gen-new-info", root);
        if (mkdir_one(launch_info) != 0) {
            return fail("per-launch info fixture mkdir failed");
        }
        runtime_cfg = jw_prepare_retroarch_config_with_info(
            runtime, root, core, launch_info, NULL, true, false, NULL,
            error, sizeof(error));
        char *text = runtime_cfg ? read_text(runtime_cfg) : NULL;
        char resolved_info[PATH_MAX];
        if (!realpath(launch_info, resolved_info)) {
            return fail("per-launch info fixture realpath failed");
        }
        char expected[PATH_MAX + 64];
        snprintf(expected, sizeof(expected),
                 "libretro_info_path = \"%s\"", resolved_info);
        if (!runtime_cfg || !text || strstr(text, expected) == NULL) {
            free(text);
            return fail(error[0] ? error : "per-launch info path was not used");
        }
        free(text);
        unlink(runtime_cfg);
        free(runtime_cfg);
    }

#ifdef PLATFORM_MLP1
    /* Hotkey ownership and migration matrix. The modifier is user-owned: only
       the two exactly-known old durable states are normalized, and every other
       surviving value passes through. Exit stays daemon-owned and unbound in
       the runtime config while never reaching the durable one. */
    {
        /* A fresh card has no durable config at all. Prepare seeds it from the
           packaged defaults, so the new values arrive without any migration. */
        if (unlink(shared_cfg) != 0) {
            return fail("could not clear the durable config for the fresh case");
        }
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "fresh hotkey config generation failed");
        }
        if (verify_hotkeys(runtime_cfg, "fresh install",
                           "5", "0", "nul", "nul", "0") != 0) {
            unlink(runtime_cfg);
            free(runtime_cfg);
            return fail("fresh install did not get the packaged hotkey defaults");
        }
        unlink(runtime_cfg);
        free(runtime_cfg);

        /* Each case: the durable state before launch, then the runtime values
           it must produce. A NULL runtime expectation means the key must not
           appear at all. */
        static const struct {
            const char *label;
            const char *durable;
            const char *modifier;
            const char *delay;
            const char *menu_toggle;
            const char *combo;
        } cases[] = {
            {
                /* The original MLP1 pair, recognized only together. */
                "legacy Select+Start",
                "menu_driver = \"rgui\"\n"
                "input_enable_hotkey_btn = \"4\"\n"
                "input_hotkey_block_delay = \"5\"\n"
                "input_exit_emulator_btn = \"6\"\n"
                "input_menu_toggle_btn = \"5\"\n"
                "input_menu_toggle_gamepad_combo = \"0\"\n",
                "5", "0", "nul", "0",
            },
            {
                /* What the current beta leaves behind: the protected-key
                   filter stripped the modifier on every promotion. */
                "beta-stripped modifier",
                "menu_driver = \"rgui\"\n"
                "input_hotkey_block_delay = \"5\"\n"
                "input_menu_toggle_btn = \"5\"\n"
                "input_menu_toggle_gamepad_combo = \"0\"\n",
                "5", "0", "nul", "0",
            },
            {
                /* A custom modifier is a deliberate choice, not damage; the
                   custom native-menu bind next to it is equally the user's. */
                "custom modifier and menu bind",
                "menu_driver = \"rgui\"\n"
                "input_enable_hotkey_btn = \"7\"\n"
                "input_hotkey_block_delay = \"5\"\n"
                "input_menu_toggle_btn = \"2\"\n"
                "input_menu_toggle_gamepad_combo = \"0\"\n",
                "7", "5", "2", "0",
            },
            {
                /* An explicit clear survives. RetroArch writes "nul" for a
                   cleared bind, so this is distinguishable from the absent
                   key above and needs no migration marker. */
                "explicitly cleared modifier",
                "menu_driver = \"rgui\"\n"
                "input_enable_hotkey_btn = \"nul\"\n"
                "input_hotkey_block_delay = \"0\"\n"
                "input_menu_toggle_btn = \"nul\"\n"
                "input_menu_toggle_gamepad_combo = \"0\"\n",
                "nul", "0", "nul", "0",
            },
            {
                /* Back on Menu by the user's own hand. Identical to the
                   migrated value, and must not re-trigger the migration:
                   their custom delay and combo stay untouched. */
                "user chose Menu again",
                "menu_driver = \"rgui\"\n"
                "input_enable_hotkey_btn = \"5\"\n"
                "input_hotkey_block_delay = \"3\"\n"
                "input_menu_toggle_btn = \"nul\"\n"
                "input_menu_toggle_gamepad_combo = \"2\"\n",
                "5", "3", "nul", "2",
            },
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            if (write_text(shared_cfg, cases[i].durable) != 0) {
                return fail("hotkey durable fixture write failed");
            }
            runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                      true, false,
                                                      error, sizeof(error));
            if (!runtime_cfg) {
                return fail(error[0] ? error : "hotkey config generation failed");
            }
            int runtime_ok =
                verify_hotkeys(runtime_cfg, cases[i].label, cases[i].modifier,
                               cases[i].delay, "nul", cases[i].menu_toggle,
                               cases[i].combo) == 0;
            if (!runtime_ok) {
                unlink(runtime_cfg);
                free(runtime_cfg);
                return fail("runtime hotkey values are wrong for this case");
            }
            /* Save-on-exit promotion: the user-owned values persist and only
               the protected exit bind is stripped. */
            if (jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                           error, sizeof(error)) != 0) {
                unlink(runtime_cfg);
                free(runtime_cfg);
                return fail(error[0] ? error : "hotkey backup failed");
            }
            unlink(runtime_cfg);
            free(runtime_cfg);
            if (verify_hotkeys(shared_cfg, cases[i].label, cases[i].modifier,
                               cases[i].delay, NULL, cases[i].menu_toggle,
                               cases[i].combo) != 0) {
                return fail("durable hotkey values are wrong after promotion");
            }
            /* The next launch reads back what was just promoted and must not
               migrate it a second time. */
            runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                      true, false,
                                                      error, sizeof(error));
            if (!runtime_cfg) {
                return fail(error[0] ? error : "hotkey relaunch config failed");
            }
            int stable = verify_hotkeys(runtime_cfg, cases[i].label,
                                        cases[i].modifier, cases[i].delay,
                                        "nul", cases[i].menu_toggle,
                                        cases[i].combo) == 0;
            unlink(runtime_cfg);
            free(runtime_cfg);
            if (!stable) {
                return fail("hotkey values changed again on the next launch");
            }
        }

        /* A modifier the user changes inside RetroArch survives the round
           trip. This is the beta report: it used to be stripped on promotion
           and regenerated as "nul" on the next launch. */
        if (write_text(shared_cfg,
                       "menu_driver = \"rgui\"\n"
                       "input_enable_hotkey_btn = \"5\"\n"
                       "input_hotkey_block_delay = \"0\"\n"
                       "input_menu_toggle_btn = \"nul\"\n"
                       "input_menu_toggle_gamepad_combo = \"0\"\n") != 0) {
            return fail("hotkey round-trip fixture write failed");
        }
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "hotkey round-trip config failed");
        }
        char *session = read_text(runtime_cfg);
        char *changed = replace_line(session, "input_enable_hotkey_btn = \"5\"",
                                     "input_enable_hotkey_btn = \"7\"");
        free(session);
        if (!changed || write_text(runtime_cfg, changed) != 0) {
            free(changed);
            unlink(runtime_cfg);
            free(runtime_cfg);
            return fail("could not simulate a RetroArch modifier change");
        }
        free(changed);
        if (jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                       error, sizeof(error)) != 0) {
            unlink(runtime_cfg);
            free(runtime_cfg);
            return fail(error[0] ? error : "hotkey round-trip backup failed");
        }
        unlink(runtime_cfg);
        free(runtime_cfg);
        if (verify_hotkeys(shared_cfg, "user-changed modifier",
                           "7", "0", NULL, "nul", "0") != 0) {
            return fail("a modifier chosen in RetroArch did not persist");
        }
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "hotkey round-trip relaunch failed");
        }
        int kept = verify_hotkeys(runtime_cfg, "user-changed modifier",
                                  "7", "0", "nul", "nul", "0") == 0;
        unlink(runtime_cfg);
        free(runtime_cfg);
        if (!kept) {
            return fail("a persisted modifier was rewritten on the next launch");
        }
    }
#endif

    if (write_text(
            shared_cfg,
            "menu_driver = \"rgui\"\n"
            "video_shader_dir = "
            "\"/mnt/sdcard/.system/leaf/platforms/mlp1/shaders\"\n") != 0) {
        return fail("stale release shader config write failed");
    }
    runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                              true, false, error, sizeof(error));
    if (!runtime_cfg || verify_runtime(runtime_cfg, user_shaders) != 0) {
        return fail("stale release shader directory did not migrate to user state");
    }
    char *migrated_runtime = read_text(runtime_cfg);
    if (!migrated_runtime ||
        key_count(migrated_runtime, "video_shader_dir",
                  "/mnt/sdcard/.system/leaf/platforms/mlp1/shaders") != 0) {
        free(migrated_runtime);
        return fail("stale release shader directory survived migration");
    }
    free(migrated_runtime);
    unlink(runtime_cfg);
    free(runtime_cfg);

    char custom_cfg[PATH_MAX + 64];
    snprintf(custom_cfg, sizeof(custom_cfg),
             "menu_driver = \"rgui\"\nvideo_shader_dir = \"%s\"\n",
             custom_shaders);
    if (write_text(shared_cfg, custom_cfg) != 0) {
        return fail("custom shader config write failed");
    }
    runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                              true, false, error, sizeof(error));
    if (!runtime_cfg || verify_runtime(runtime_cfg, custom_shaders) != 0) {
        return fail("custom shader directory did not win over bundle fallback");
    }
    char *custom_runtime = read_text(runtime_cfg);
    if (!custom_runtime ||
        key_count(custom_runtime, "video_shader_dir", user_shaders) != 0) {
        free(custom_runtime);
        return fail("durable shader fallback overrode the custom shader directory");
    }
    free(custom_runtime);
    unlink(runtime_cfg);
    free(runtime_cfg);

    if (rmdir(shaders) != 0 ||
        write_text(shared_cfg, "menu_driver = \"rgui\"\n") != 0) {
        return fail("missing shader bundle fixture setup failed");
    }
    runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                              true, false, error, sizeof(error));
    if (!runtime_cfg || verify_runtime(runtime_cfg, user_shaders) != 0) {
        return fail("missing shader bundle prevented config generation");
    }
    char *missing_runtime = read_text(runtime_cfg);
    if (!missing_runtime ||
        key_count(missing_runtime, "video_shader_dir", user_shaders) != 1) {
        free(missing_runtime);
        return fail("missing bundle did not retain the durable shader root");
    }
    free(missing_runtime);
    unlink(runtime_cfg);
    free(runtime_cfg);

    /* Automatic shader preset discovery is pinned; the browser root is not.
       A user who redirects rgui_config_directory or switches either enable
       flag off would break every saved preset silently: RetroArch would look
       somewhere Fugazi and the in-game picker never write, or return before
       automatic discovery, and nothing would say so. */
    {
        if (write_text(shared_cfg,
                       "menu_driver = \"rgui\"\n"
                       "rgui_config_directory = \"/tmp/attacker-owned\"\n"
                       "auto_shaders_enable = \"false\"\n"
                       "video_shader_enable = \"false\"\n"
                       "video_shader_preset_save_reference_enable = \"false\"\n") != 0) {
            return fail("shader discovery override write failed");
        }
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "shader discovery config generation failed");
        }
        char *disc = read_text(runtime_cfg);
        if (!disc) {
            return fail("shader discovery runtime config unreadable");
        }
        int ok =
            /* The persisted values must not survive at all. */
            key_count(disc, "rgui_config_directory", "/tmp/attacker-owned") == 0 &&
            key_count(disc, "auto_shaders_enable", "false") == 0 &&
            key_count(disc, "video_shader_enable", "false") == 0 &&
            /* Shader loading and automatic discovery stay on, exactly once. */
            key_count(disc, "auto_shaders_enable", "true") == 1 &&
            key_count(disc, "video_shader_enable", "true") == 1 &&
            /* And the config directory is written exactly once, by us. */
            occurrences(disc, "rgui_config_directory") == 1 &&
            /* This one stays the user's: both save styles are supported. */
            key_count(disc, "video_shader_preset_save_reference_enable",
                      "false") == 1;
        if (!ok) {
            fprintf(stderr,
                    "  rgui_config_directory=/tmp/attacker-owned : %d\n"
                    "  auto_shaders_enable=false                 : %d\n"
                    "  video_shader_enable=false                : %d\n"
                    "  auto_shaders_enable=true                  : %d\n"
                    "  video_shader_enable=true                 : %d\n"
                    "  rgui_config_directory occurrences         : %d\n"
                    "  save_reference_enable=false (user)        : %d\n",
                    key_count(disc, "rgui_config_directory", "/tmp/attacker-owned"),
                    key_count(disc, "auto_shaders_enable", "false"),
                    key_count(disc, "video_shader_enable", "false"),
                    key_count(disc, "auto_shaders_enable", "true"),
                    key_count(disc, "video_shader_enable", "true"),
                    occurrences(disc, "rgui_config_directory"),
                    key_count(disc, "video_shader_preset_save_reference_enable", "false"));
        }
        free(disc);
        unlink(runtime_cfg);
        free(runtime_cfg);
        if (!ok) {
            return fail("persisted values redirected or disabled shader discovery");
        }
    }

    /* Preview paths are constrained to leaf-recommended. Restore separately
       accepts a daemon-reported original below the durable shader, automatic
       preset, or effective browser roots, so a failed preview can recover a
       custom/global preset without turning preview into an arbitrary loader. */
    {
        char *recdir = jw_retroarch_recommended_shaders_dir();
        char resolved[PATH_MAX];
        char relative[PATH_MAX];
        int ok = 1;

        if (!recdir) {
            return fail("recommended shader directory unavailable");
        }
        if (jw__mkdir_p_test(recdir) != 0) {
            free(recdir);
            return fail("recommended shader directory could not be created");
        }

        char good[PATH_MAX];
        char wrong_ext[PATH_MAX];
        char outside[PATH_MAX];
        char escape[PATH_MAX];
        char custom[PATH_MAX];
        char foreign[PATH_MAX];
        char unconfigured_dir[PATH_MAX];
        char unconfigured[PATH_MAX];
        char shader_runtime_cfg[PATH_MAX];
        char automatic_dir[PATH_MAX];
        char automatic[PATH_MAX];
        snprintf(good, sizeof(good), "%s/crt-sharp.glslp", recdir);
        snprintf(wrong_ext, sizeof(wrong_ext), "%s/notes.txt", recdir);
        snprintf(outside, sizeof(outside), "%s/../escaped.glslp", recdir);
        snprintf(escape, sizeof(escape), "%s/../../../../etc/passwd", recdir);
        snprintf(custom, sizeof(custom), "%s/custom/user.glslp", user_shaders);
        snprintf(foreign, sizeof(foreign), "%s/outside.glslp", custom_shaders);
        snprintf(unconfigured_dir, sizeof(unconfigured_dir), "%s/unconfigured", root);
        snprintf(unconfigured, sizeof(unconfigured), "%s/outside.glslp",
                 unconfigured_dir);
        snprintf(shader_runtime_cfg, sizeof(shader_runtime_cfg),
                 "%s/shader-runtime.cfg", runtime);
        snprintf(automatic_dir, sizeof(automatic_dir),
                 "%s/retroarch/.config/retroarch/config/FCEUmm", internal);
        snprintf(automatic, sizeof(automatic), "%s/Game.glslp", automatic_dir);
        if (jw__mkdir_p_test(user_shaders) != 0 ||
            jw__mkdir_p_test(unconfigured_dir) != 0 ||
            jw__mkdir_p_test(automatic_dir) != 0) {
            free(recdir);
            return fail("shader restore directories could not be created");
        }
        char custom_dir[PATH_MAX];
        snprintf(custom_dir, sizeof(custom_dir), "%s/custom", user_shaders);
        if (jw__mkdir_p_test(custom_dir) != 0) {
            free(recdir);
            return fail("custom shader directory could not be created");
        }
        if (write_text(good, "#reference \"x\"\n") != 0 ||
            write_text(wrong_ext, "notes\n") != 0 ||
            write_text(custom, "shaders = 0\n") != 0 ||
            write_text(foreign, "shaders = 0\n") != 0 ||
            write_text(unconfigured, "shaders = 0\n") != 0 ||
            write_text(shader_runtime_cfg, custom_cfg) != 0 ||
            write_text(automatic, "#reference \"x\"\n") != 0) {
            free(recdir);
            return fail("shader path fixture write failed");
        }
        write_text(outside, "#reference \"x\"\n");

        /* A real preset inside the directory is accepted, and the logged form
           is relative to the root. */
        if (!jw_retroarch_shader_path_is_recommended(good, resolved, sizeof(resolved),
                                                     relative, sizeof(relative)) ||
            strcmp(relative, "crt-sharp.glslp") != 0) {
            ok = 0;
        }
        /* Everything else is refused. */
        if (jw_retroarch_shader_path_is_recommended(wrong_ext, resolved, sizeof(resolved),
                                                    relative, sizeof(relative)) ||
            jw_retroarch_shader_path_is_recommended(outside, resolved, sizeof(resolved),
                                                    relative, sizeof(relative)) ||
            jw_retroarch_shader_path_is_recommended(escape, resolved, sizeof(resolved),
                                                    relative, sizeof(relative)) ||
            jw_retroarch_shader_path_is_recommended("/etc/passwd", resolved, sizeof(resolved),
                                                    relative, sizeof(relative)) ||
            jw_retroarch_shader_path_is_recommended("", resolved, sizeof(resolved),
                                                    relative, sizeof(relative)) ||
            jw_retroarch_shader_path_is_recommended(NULL, resolved, sizeof(resolved),
                                                    relative, sizeof(relative))) {
            ok = 0;
        }
        if (!jw_retroarch_shader_path_is_restorable(custom, shader_runtime_cfg,
                                                     resolved,
                                                     sizeof(resolved), relative,
                                                     sizeof(relative)) ||
            strcmp(relative, "shaders/custom/user.glslp") != 0 ||
            !jw_retroarch_shader_path_is_restorable(automatic, shader_runtime_cfg,
                                                     resolved,
                                                     sizeof(resolved), relative,
                                                     sizeof(relative)) ||
            strcmp(relative, "config/FCEUmm/Game.glslp") != 0 ||
            !jw_retroarch_shader_path_is_restorable(foreign, shader_runtime_cfg,
                                                     resolved, sizeof(resolved),
                                                     relative, sizeof(relative)) ||
            strcmp(relative, "video_shader_dir/outside.glslp") != 0 ||
            jw_retroarch_shader_path_is_restorable(foreign, NULL, resolved,
                                                    sizeof(resolved), relative,
                                                    sizeof(relative)) ||
            jw_retroarch_shader_path_is_restorable(unconfigured,
                                                    shader_runtime_cfg, resolved,
                                                    sizeof(resolved), relative,
                                                    sizeof(relative)) ||
            jw_retroarch_shader_path_is_restorable("/etc/passwd",
                                                    shader_runtime_cfg, resolved,
                                                    sizeof(resolved), relative,
                                                    sizeof(relative))) {
            ok = 0;
        }
        unlink(good);
        unlink(wrong_ext);
        unlink(outside);
        unlink(custom);
        unlink(foreign);
        unlink(unconfigured);
        unlink(shader_runtime_cfg);
        unlink(automatic);
        rmdir(unconfigured_dir);
        free(recdir);
        if (!ok) {
            return fail("picker shader path constraint is not enforced");
        }
    }

    /* Launch roster: every generated index lands in the protected block,
       unused players are omitted entirely, and a persisted override cannot
       win. */
    {
        int indices[4] = {0, 1, 3, -1};
        if (write_text(shared_cfg,
                       "menu_driver = \"rgui\"\n"
                       "input_max_users = \"1\"\n"
                       "input_player1_joypad_index = \"2\"\n"
                       "input_player2_joypad_index = \"9\"\n") != 0) {
            return fail("override config write failed");
        }
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, indices,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "roster config generation failed");
        }
        char *roster_runtime = read_text(runtime_cfg);
        int ok = roster_runtime &&
#ifdef PLATFORM_MLP1
                 /* three roster members -> three users, not a fixed 4 */
                 key_count(roster_runtime, "input_max_users", "= \"3\"") == 1 &&
#endif
                 key_count(roster_runtime, "input_player1_joypad_index",
                           "= \"0\"") == 1 &&
                 key_count(roster_runtime, "input_player2_joypad_index",
                           "= \"1\"") == 1 &&
                 key_count(roster_runtime, "input_player3_joypad_index",
                           "= \"3\"") == 1 &&
                 /* An unused player gets no key at all. A "-1" sentinel would
                    be read back into RetroArch's unsigned joypad-index array
                    and fault on the first poll. */
                 key_count(roster_runtime, "input_player4_joypad_index",
                           NULL) == 0 &&
                 key_count(roster_runtime, "input_player1_joypad_index",
                           NULL) == 1 &&
                 key_count(roster_runtime, "input_player2_joypad_index",
                           NULL) == 1 &&
                 /* exactly one max_users line (the generated one); the
                    persisted override never reaches the runtime config */
                 key_count(roster_runtime, "input_max_users", NULL)
#ifdef PLATFORM_MLP1
                     == 1
#else
                     >= 0
#endif
            ;
        free(roster_runtime);
        unlink(runtime_cfg);
        free(runtime_cfg);
        if (!ok) {
            return fail("roster player indices not protected from overrides");
        }
    }

    /* LEAF-RA-1 frontend profile (a content pak's own RetroArch). It may take
       away only what such a RetroArch can lack: FFmpeg recording and the
       BlueALSA audio plugin. Everything else in the protected block, the
       command port above all, is Leaf's, byte for byte. */
    {
        unsetenv("JAWAKA_AUDIO_OUTPUT");
        unsetenv("UMRK_AUDIO_OUTPUT");
        unsetenv("JAWAKA_AUDIO_DEVICE");
        unsetenv("UMRK_AUDIO_DEVICE");
        /* Persisted recording and audio keys must not leak into either. */
        if (write_text(shared_cfg,
                       "menu_driver = \"rgui\"\n"
                       "record_driver = \"ffmpeg\"\n"
                       "video_record_config = \"/persisted/evil.cfg\"\n"
                       "audio_driver = \"alsa\"\n"
                       "audio_device = \"bluealsa\"\n") != 0) {
            return fail("frontend profile shared config write failed");
        }
        char *leaf = prepare_text(runtime, root, core, NULL, error, sizeof(error));
        char *pak = prepare_text(runtime, root, core,
                                 &jw_retroarch_frontend_profile_leaf_ra_1,
                                 error, sizeof(error));
        if (!leaf || !pak) {
            free(leaf);
            free(pak);
            return fail(error[0] ? error : "frontend profile config generation failed");
        }
        int ok = 1;
        /* The profile object itself says what it does. */
        ok &= expect_ok(jw_retroarch_frontend_profile_leaf_ra_1.no_recording &&
                            jw_retroarch_frontend_profile_leaf_ra_1.pulse_audio,
                        "profile flags: no_recording and pulse_audio");
        /* The command interface Leaf drives stays on, once, on the same port. */
        ok &= expect_ok(occurrences(pak, "network_cmd_enable") == 1 &&
                            key_count(pak, "network_cmd_enable", "= \"true\"") == 1,
                        "frontend: network_cmd_enable true exactly once");
        ok &= expect_ok(occurrences(pak, "network_cmd_port") == 1 &&
                            key_count(pak, "network_cmd_port", "= \"55355\"") == 1,
                        "frontend: network_cmd_port 55355 exactly once");
        ok &= expect_ok(key_count(pak, "pause_nonactive", "= \"false\"") == 1 &&
                            key_count(pak, "config_save_on_exit", "= \"true\"") == 1 &&
                            key_count(pak, "sort_savefiles_enable", "= \"true\"") == 1 &&
                            key_count(pak, "sort_savestates_enable", "= \"true\"") == 1 &&
                            key_count(pak, "video_shader_enable", "= \"true\"") == 1 &&
                            occurrences(pak, "rgui_config_directory") == 1,
                        "frontend: the rest of the protected block is present");
        /* Same lines as Leaf's own RetroArch once recording and audio are set
           aside. */
        char *leaf_rest = without_keys(leaf, profile_keys,
                                       sizeof(profile_keys) / sizeof(profile_keys[0]));
        char *pak_rest = without_keys(pak, profile_keys,
                                      sizeof(profile_keys) / sizeof(profile_keys[0]));
        ok &= expect_ok(leaf_rest && pak_rest && strcmp(leaf_rest, pak_rest) == 0,
                        "the profile changes only recording and audio keys");
        free(leaf_rest);
        free(pak_rest);
#ifdef PLATFORM_MLP1
        /* Persisted recording and audio values are stripped from the merge on
           the MLP1 (and only there), so none of them comes back. */
        ok &= expect_ok(key_count(pak, "video_record_config", "/persisted/evil.cfg") == 0 &&
                            key_count(leaf, "video_record_config", "/persisted/evil.cfg") == 0,
                        "a persisted video_record_config never reaches the session");
        ok &= expect_ok(key_count(pak, "audio_device", "bluealsa") == 0 &&
                            key_count(leaf, "audio_device", "bluealsa") == 0,
                        "a persisted audio_device never reaches the session");
        char *leaf_record = ra_parse_value(leaf, "record_driver");
        char *pak_record = ra_parse_value(pak, "record_driver");
        /* Leaf's RetroArch records through the preset that exists in
           defaults/; the frontend gets the do-nothing driver although the same
           preset is right there, and none of the ffmpeg keys. */
        ok &= expect_ok(path_exists(record_cfg), "recording preset fixture present");
        ok &= expect_ok(leaf_record && strcmp(leaf_record, "ffmpeg") == 0 &&
                            occurrences(leaf, "record_driver") == 1 &&
                            key_count(leaf, "video_record_config", record_cfg) == 1 &&
                            key_count(leaf, "recording_output_directory",
                                      recordings_dir) == 1,
                        "no profile: ffmpeg recording through the preset");
        ok &= expect_ok(pak_record && strcmp(pak_record, "null") == 0 &&
                            occurrences(pak, "record_driver") == 1 &&
                            occurrences(pak, "video_record_config") == 0 &&
                            occurrences(pak, "video_record_quality") == 0 &&
                            occurrences(pak, "recording_output_directory") == 0,
                        "frontend profile: record_driver null, no ffmpeg keys, "
                        "although the preset exists");
        free(leaf_record);
        free(pak_record);
#else
        /* Neither the recording nor the audio block exists off the MLP1, so
           the profile has no effect at all. */
        ok &= expect_ok(strcmp(leaf, pak) == 0,
                        "off the MLP1 the profile changes nothing");
#endif
        free(leaf);
        free(pak);
        if (!ok) {
            return fail("frontend profile contract not met");
        }
    }

#ifdef PLATFORM_MLP1
    /* The audio half. Without a profile the Bluetooth route is ALSA through
       BlueALSA, as before; a frontend without that plugin stays on PulseAudio
       and never names "bluealsa" as its device. */
    {
        static const struct {
            const char *label;
            const char *output_env;     /* NULL: no output variable */
            const char *output;
            const char *device_env;     /* NULL: no device variable */
            const char *device;
            /* Leaf's RetroArch (NULL profile) */
            const char *leaf_driver;
            const char *leaf_device;
            /* a LEAF-RA-1 frontend */
            const char *pak_driver;
            const char *pak_device;
        } cases[] = {
            { "bluetooth, no device", "JAWAKA_AUDIO_OUTPUT", "BLUETOOTH", NULL, NULL,
              "alsa", "bluealsa", "pulse", "" },
            { "bluetooth, device bluealsa", "JAWAKA_AUDIO_OUTPUT", "BLUETOOTH",
              "JAWAKA_AUDIO_DEVICE", "bluealsa",
              "alsa", "bluealsa", "pulse", "" },
            { "bluetooth through the UMRK_ names", "UMRK_AUDIO_OUTPUT", "BLUETOOTH",
              "UMRK_AUDIO_DEVICE", "bluealsa",
              "alsa", "bluealsa", "pulse", "" },
            { "bluetooth, device default", "JAWAKA_AUDIO_OUTPUT", "BLUETOOTH",
              "JAWAKA_AUDIO_DEVICE", "default",
              "alsa", "bluealsa", "pulse", "" },
            { "speaker, no device", "JAWAKA_AUDIO_OUTPUT", "SPEAKER", NULL, NULL,
              "pulse", "", "pulse", "" },
            { "speaker, named sink", "JAWAKA_AUDIO_OUTPUT", "SPEAKER",
              "JAWAKA_AUDIO_DEVICE", "alsa_output.platform-sound.stereo",
              "pulse", "alsa_output.platform-sound.stereo",
              "pulse", "alsa_output.platform-sound.stereo" },
            { "nothing set", NULL, NULL, NULL, NULL, "pulse", "", "pulse", "" },
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            unsetenv("JAWAKA_AUDIO_OUTPUT");
            unsetenv("UMRK_AUDIO_OUTPUT");
            unsetenv("JAWAKA_AUDIO_DEVICE");
            unsetenv("UMRK_AUDIO_DEVICE");
            if (cases[i].output_env) setenv(cases[i].output_env, cases[i].output, 1);
            if (cases[i].device_env) setenv(cases[i].device_env, cases[i].device, 1);
            char *leaf = prepare_text(runtime, root, core, NULL, error, sizeof(error));
            char *pak = prepare_text(runtime, root, core,
                                     &jw_retroarch_frontend_profile_leaf_ra_1,
                                     error, sizeof(error));
            unsetenv("JAWAKA_AUDIO_OUTPUT");
            unsetenv("UMRK_AUDIO_OUTPUT");
            unsetenv("JAWAKA_AUDIO_DEVICE");
            unsetenv("UMRK_AUDIO_DEVICE");
            if (!leaf || !pak) {
                free(leaf);
                free(pak);
                return fail(error[0] ? error : "audio profile config generation failed");
            }
            char *leaf_driver = ra_parse_value(leaf, "audio_driver");
            char *leaf_device = ra_parse_value(leaf, "audio_device");
            char *pak_driver = ra_parse_value(pak, "audio_driver");
            char *pak_device = ra_parse_value(pak, "audio_device");
            int ok = leaf_driver && leaf_device && pak_driver && pak_device &&
                     strcmp(leaf_driver, cases[i].leaf_driver) == 0 &&
                     strcmp(leaf_device, cases[i].leaf_device) == 0 &&
                     strcmp(pak_driver, cases[i].pak_driver) == 0 &&
                     strcmp(pak_device, cases[i].pak_device) == 0 &&
                     /* each exactly once: the persisted alsa/bluealsa lines
                        from the shared config never survive */
                     occurrences(leaf, "audio_driver") == 1 &&
                     occurrences(leaf, "audio_device") == 1 &&
                     occurrences(pak, "audio_driver") == 1 &&
                     occurrences(pak, "audio_device") == 1 &&
                     /* a frontend never names the BlueALSA pcm anywhere */
                     strstr(pak, "bluealsa") == NULL &&
                     /* the rest of the audio block is shared */
                     key_count(pak, "audio_latency", "= \"128\"") == 1 &&
                     key_count(pak, "audio_block_frames", "= \"256\"") == 1;
            if (!ok) {
                fprintf(stderr,
                        "retroarch-config-test: audio case \"%s\": leaf %s/%s (want %s/%s), "
                        "frontend %s/%s (want %s/%s)\n",
                        cases[i].label,
                        leaf_driver ? leaf_driver : "(none)",
                        leaf_device ? leaf_device : "(none)",
                        cases[i].leaf_driver, cases[i].leaf_device,
                        pak_driver ? pak_driver : "(none)",
                        pak_device ? pak_device : "(none)",
                        cases[i].pak_driver, cases[i].pak_device);
            }
            free(leaf_driver);
            free(leaf_device);
            free(pak_driver);
            free(pak_device);
            free(leaf);
            free(pak);
            if (!ok) {
                return fail("audio routing differs from the frontend profile contract");
            }
        }
    }

    /* The recording gate is the profile, not the preset: with no preset on the
       card Leaf's own RetroArch lands on the same do-nothing driver. */
    {
        char preset_stash[PATH_MAX + 8];
        snprintf(preset_stash, sizeof(preset_stash), "%s.stash", record_cfg);
        if (rename(record_cfg, preset_stash) != 0) {
            return fail("could not stash the recording preset");
        }
        char *leaf = prepare_text(runtime, root, core, NULL, error, sizeof(error));
        char *pak = prepare_text(runtime, root, core,
                                 &jw_retroarch_frontend_profile_leaf_ra_1,
                                 error, sizeof(error));
        int restored = rename(preset_stash, record_cfg) == 0;
        char *leaf_record = leaf ? ra_parse_value(leaf, "record_driver") : NULL;
        char *pak_record = pak ? ra_parse_value(pak, "record_driver") : NULL;
        int ok = restored && leaf_record && pak_record &&
                 strcmp(leaf_record, "null") == 0 &&
                 strcmp(pak_record, "null") == 0 &&
                 occurrences(leaf, "video_record_config") == 0;
        free(leaf_record);
        free(pak_record);
        free(leaf);
        free(pak);
        if (!ok) {
            return fail("no recording preset did not select the null record driver");
        }
    }
#endif

    /* RAOfflineProxy transient launch bridge. The shared config deliberately
       already carries both cheevos keys with foreign values, which is the
       normal state once RetroArch has saved once. */
    {
        static const char shared_with_cheevos[] =
            "menu_driver = \"rgui\"\n"
            "cheevos_custom_host = \"example.invalid:9999\"\n"
            "cheevos_hardcore_mode_enable = \"true\"\n";
        if (write_text(shared_cfg, shared_with_cheevos) != 0) {
            return fail("cheevos shared config write failed");
        }

        /* Direct launch: neither key is touched, both persist unchanged. */
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false,
                                                  error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "direct cheevos config failed");
        }
        char *direct_runtime = read_text(runtime_cfg);
        int direct_ok =
            direct_runtime &&
            key_count(direct_runtime, "cheevos_custom_host",
                      "example.invalid:9999") == 1 &&
            key_count(direct_runtime, "cheevos_hardcore_mode_enable",
                      "= \"true\"") == 1;
        free(direct_runtime);
        if (!direct_ok) {
            unlink(runtime_cfg);
            free(runtime_cfg);
            return fail("direct launch did not pass through shared cheevos values");
        }
        unlink(runtime_cfg);
        free(runtime_cfg);

        /* Proxied launch: each key appears EXACTLY ONCE and carries the proxy
           value. RetroArch keeps the first occurrence of a key and drops later
           duplicates, so a surviving merged-in copy would silently win and the
           session would bypass the proxy. */
        jw_retroarch_launch_snapshot snapshot;
        jw_retroarch_launch_snapshot_init(&snapshot);
        char *shared_text = jw_retroarch_shared_config_read(root);
        if (!shared_text) {
            return fail("shared config read for snapshot failed");
        }
        jw_retroarch_launch_snapshot_capture(&snapshot, shared_text);
        free(shared_text);
        snapshot.proxied = true;
        if (!snapshot.custom_host_present || !snapshot.hardcore_present) {
            return fail("snapshot did not capture both shared cheevos lines");
        }

        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, true,
                                                  error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "proxied cheevos config failed");
        }
        char *proxied_runtime = read_text(runtime_cfg);
        int proxied_ok =
            proxied_runtime &&
            key_count(proxied_runtime, "cheevos_custom_host", NULL) == 1 &&
            key_count(proxied_runtime, "cheevos_custom_host",
                      "127.0.0.1:8080") == 1 &&
            key_count(proxied_runtime, "cheevos_hardcore_mode_enable",
                      NULL) == 1 &&
            key_count(proxied_runtime, "cheevos_hardcore_mode_enable",
                      "= \"false\"") == 1;
        free(proxied_runtime);
        if (!proxied_ok) {
            unlink(runtime_cfg);
            free(runtime_cfg);
            return fail("proxied launch did not emit each cheevos key exactly once");
        }

        /* Backup restores the prior shared lines byte-identically. */
        if (jw_backup_retroarch_config(runtime_cfg, root, &snapshot,
                                       error, sizeof(error)) != 0) {
            unlink(runtime_cfg);
            free(runtime_cfg);
            return fail(error[0] ? error : "proxied backup failed");
        }
        unlink(runtime_cfg);
        free(runtime_cfg);

        char *restored = read_text(shared_cfg);
        int restored_ok =
            restored &&
            key_count(restored, "cheevos_custom_host",
                      "example.invalid:9999") == 1 &&
            key_count(restored, "cheevos_custom_host", "127.0.0.1:8080") == 0 &&
            key_count(restored, "cheevos_hardcore_mode_enable",
                      "= \"true\"") == 1 &&
            key_count(restored, "cheevos_hardcore_mode_enable",
                      "= \"false\"") == 0 &&
            /* Generated by RetroArch from the injected password; never a
               user-owned setting, so it must not reach the shared card. */
            key_count(restored, "cheevos_token", NULL) == 0;
        free(restored);
        if (!restored_ok) {
            return fail("proxied backup did not restore the shared cheevos lines");
        }

        /* Absence is a value too: a shared config with neither key must come
           back with neither key after a proxied session. */
        if (write_text(shared_cfg, "menu_driver = \"rgui\"\n") != 0) {
            return fail("empty cheevos shared config write failed");
        }
        jw_retroarch_launch_snapshot_init(&snapshot);
        shared_text = jw_retroarch_shared_config_read(root);
        if (!shared_text) {
            return fail("shared config re-read for snapshot failed");
        }
        jw_retroarch_launch_snapshot_capture(&snapshot, shared_text);
        free(shared_text);
        snapshot.proxied = true;
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, true,
                                                  error, sizeof(error));
        if (!runtime_cfg ||
            jw_backup_retroarch_config(runtime_cfg, root, &snapshot,
                                       error, sizeof(error)) != 0) {
            free(runtime_cfg);
            return fail("absent-key proxied session failed");
        }
        unlink(runtime_cfg);
        free(runtime_cfg);
        char *absent = read_text(shared_cfg);
        int absent_ok = absent &&
                        key_count(absent, "cheevos_custom_host", NULL) == 0 &&
                        key_count(absent, "cheevos_hardcore_mode_enable",
                                  NULL) == 0;
        free(absent);
        if (!absent_ok) {
            return fail("proxied backup invented cheevos keys that were absent");
        }
    }

    /* RetroAchievements credentials reach RetroArch exactly. The writer's
       output goes through RetroArch's own parser (vendored, see
       third_party/retroarch/config_file_parser.inc), so a password with a '"'
       or '\\' either reads back byte for byte or is left out entirely --
       never truncated or doubled. Synthetic passwords only; a failure names
       the case number, never the value. */
    {
        static const struct {
            const char *pass;
            jw_ra_cfg_form form;
        } cases[] = {
            { "plainpass", JW_RA_CFG_QUOTED },
            { "with space", JW_RA_CFG_QUOTED },
            { "semi;colon", JW_RA_CFG_QUOTED },
            { "hash#tag", JW_RA_CFG_QUOTED },
            { "#leading-hash", JW_RA_CFG_QUOTED },
            { " padded ", JW_RA_CFG_QUOTED },
            { "back\\slash", JW_RA_CFG_QUOTED },
            { "trail\\", JW_RA_CFG_QUOTED },
            { "\\\\two", JW_RA_CFG_QUOTED },
            { "tab\there", JW_RA_CFG_QUOTED },
            { "caf\xc3\xa9 ok", JW_RA_CFG_QUOTED },
            { "=equals=", JW_RA_CFG_QUOTED },
            { "quote\"inside", JW_RA_CFG_BARE },
            { "trail\"", JW_RA_CFG_BARE },
            { "a\"b\\c", JW_RA_CFG_BARE },
            { "q\"=;!~", JW_RA_CFG_BARE },
            { "\"leading", JW_RA_CFG_UNWRITABLE },
            { "quote\" and space", JW_RA_CFG_UNWRITABLE },
            { "quote\"#hash", JW_RA_CFG_UNWRITABLE },
            { "quote\"caf\xc3\xa9", JW_RA_CFG_UNWRITABLE },
            { "quote\"\ttab", JW_RA_CFG_UNWRITABLE },
            { "line\nbreak", JW_RA_CFG_UNWRITABLE },
            { "carriage\rreturn", JW_RA_CFG_UNWRITABLE },
        };
        static const char user[] = "synthetic_user";
        if (write_text(shared_cfg, "menu_driver = \"rgui\"\n") != 0) {
            return fail("cheevos credential shared config write failed");
        }
        setenv("JAWAKA_CHEEVOS_USERNAME", user, 1);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            char label[96];
            if (jw_retroarch_cfg_value_form(cases[i].pass) != cases[i].form) {
                snprintf(label, sizeof(label),
                         "credential case %zu classified wrongly", i);
                return fail(label);
            }
            setenv("JAWAKA_CHEEVOS_PASSWORD", cases[i].pass, 1);
            runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                      true, false,
                                                      error, sizeof(error));
            if (!runtime_cfg) {
                unsetenv("JAWAKA_CHEEVOS_USERNAME");
                unsetenv("JAWAKA_CHEEVOS_PASSWORD");
                return fail(error[0] ? error : "credential config failed");
            }
            char *text = read_text(runtime_cfg);
            char *got_pass = ra_parse_value(text, "cheevos_password");
            char *got_user = ra_parse_value(text, "cheevos_username");
            char *got_enable = ra_parse_value(text, "cheevos_enable");
            /* An unrelated generated key still parses the same way. */
            char *got_menu = ra_parse_value(text, "network_cmd_enable");
            int ok = text && got_menu && strcmp(got_menu, "true") == 0;
            if (cases[i].form == JW_RA_CFG_UNWRITABLE) {
                ok = ok && !got_pass && !got_user && !got_enable &&
                     occurrences(text, "cheevos_password") == 0;
            } else {
                ok = ok && got_pass && strcmp(got_pass, cases[i].pass) == 0 &&
                     got_user && strcmp(got_user, user) == 0 &&
                     got_enable && strcmp(got_enable, "true") == 0 &&
                     occurrences(text, "cheevos_password") == 1;
            }
            free(got_pass);
            free(got_user);
            free(got_enable);
            free(got_menu);
            free(text);
            unlink(runtime_cfg);
            free(runtime_cfg);
            if (!ok) {
                unsetenv("JAWAKA_CHEEVOS_USERNAME");
                unsetenv("JAWAKA_CHEEVOS_PASSWORD");
                snprintf(label, sizeof(label),
                         "credential case %zu did not round-trip through "
                         "RetroArch's parser", i);
                return fail(label);
            }
        }
        unsetenv("JAWAKA_CHEEVOS_USERNAME");
        unsetenv("JAWAKA_CHEEVOS_PASSWORD");
    }

    /* An ordinary, non-Leaf-owned key survives the full round trip the app
       tile and every game launch use: prepare -> RetroArch rewrites the
       working file -> backup -> prepare. This is the path Leaf#48 says loses
       settings, so it is asserted end to end rather than one hop at a time. */
    {
        if (write_text(shared_cfg, "menu_driver = \"rgui\"\n"
                                   "rewind_enable = \"false\"\n") != 0) {
            return fail("round-trip shared config write failed");
        }
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "round-trip config generation failed");
        }
        /* Stand in for RetroArch's save-on-exit: the user flipped the setting
           and RetroArch rewrote its --config file. */
        char *session = read_text(runtime_cfg);
        if (!session) {
            return fail("round-trip runtime config read failed");
        }
        char *changed = replace_line(session, "rewind_enable = \"false\"",
                                     "rewind_enable = \"true\"");
        free(session);
        if (!changed) {
            return fail("round-trip session edit failed");
        }
        int wrote = write_text(runtime_cfg, changed);
        free(changed);
        if (wrote != 0) {
            return fail("round-trip session write failed");
        }
        if (jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                       error, sizeof(error)) != 0) {
            return fail(error[0] ? error : "round-trip backup failed");
        }
        unlink(runtime_cfg);
        free(runtime_cfg);

        char *durable = read_text(shared_cfg);
        int durable_ok = durable &&
                         key_count(durable, "rewind_enable", "= \"true\"") == 1 &&
                         key_count(durable, "rewind_enable", "= \"false\"") == 0;
        free(durable);
        if (!durable_ok) {
            return fail("ordinary key did not reach the durable shared config");
        }

        /* And the next launch hands it back to RetroArch: RetroArch keeps the
           FIRST occurrence, so a stale duplicate would silently win. */
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "round-trip re-prepare failed");
        }
        char *relaunch = read_text(runtime_cfg);
        int relaunch_ok = relaunch &&
                          key_count(relaunch, "rewind_enable", NULL) == 1 &&
                          key_count(relaunch, "rewind_enable", "= \"true\"") == 1;
        free(relaunch);
        unlink(runtime_cfg);
        free(runtime_cfg);
        if (!relaunch_ok) {
            return fail("ordinary key did not come back on the next launch");
        }
    }

#ifdef PLATFORM_MLP1
    /* Leaf#48 Branch B: RetroArch's Configuration File menu is the supported
       way to point RARCH_PATH_CONFIG somewhere Leaf never reads, which loses
       the session. The key is daemon-owned, so it must appear exactly once as
       "false" in the working file even when the persisted config says
       otherwise, and never reach the durable config. */
    {
        if (write_text(shared_cfg, "menu_driver = \"rgui\"\n"
                                   "menu_show_configurations = \"true\"\n") != 0) {
            return fail("configurations-menu shared config write failed");
        }
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "configurations-menu config failed");
        }
        char *menu_runtime = read_text(runtime_cfg);
        int menu_ok = menu_runtime &&
                      key_count(menu_runtime, "menu_show_configurations", NULL) == 1 &&
                      key_count(menu_runtime, "menu_show_configurations",
                                "= \"false\"") == 1;
        free(menu_runtime);
        if (!menu_ok) {
            unlink(runtime_cfg);
            free(runtime_cfg);
            return fail("configurations menu was not hidden for the session");
        }
        if (jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                       error, sizeof(error)) != 0) {
            unlink(runtime_cfg);
            free(runtime_cfg);
            return fail(error[0] ? error : "configurations-menu backup failed");
        }
        unlink(runtime_cfg);
        free(runtime_cfg);
        char *menu_shared = read_text(shared_cfg);
        int menu_shared_ok = menu_shared &&
                             key_count(menu_shared, "menu_show_configurations",
                                       NULL) == 0;
        free(menu_shared);
        if (!menu_shared_ok) {
            return fail("menu_show_configurations persisted to the shared config");
        }
    }

    /* 120 Hz frame pacing. The persisted values are what the BFI-repetition
       trial left on a real card: a manual interval 1 would keep Auto from
       ever choosing 2, and 0 dark frames would make the next genuine BFI
       request plain repetition. The session must read Auto and one dark
       frame, and neither key may be promoted back. */
    {
        if (write_text(shared_cfg, "menu_driver = \"rgui\"\n"
                                   "video_swap_interval = \"1\"\n"
                                   "video_bfi_dark_frames = \"0\"\n") != 0) {
            return fail("frame-pacing shared config write failed");
        }
        setenv("JAWAKA_BFI", "1", 1);
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        unsetenv("JAWAKA_BFI");
        if (!runtime_cfg) {
            return fail(error[0] ? error : "frame-pacing config failed");
        }
        char *pacing = read_text(runtime_cfg);
        char *interval = ra_parse_value(pacing, "video_swap_interval");
        char *dark = ra_parse_value(pacing, "video_bfi_dark_frames");
        char *bfi = ra_parse_value(pacing, "video_black_frame_insertion");
        int pacing_ok = pacing &&
                        key_count(pacing, "video_swap_interval", NULL) == 1 &&
                        key_count(pacing, "video_bfi_dark_frames", NULL) == 1 &&
                        interval && strcmp(interval, "0") == 0 &&
                        dark && strcmp(dark, "1") == 0 &&
                        bfi && strcmp(bfi, "1") == 0;
        free(interval);
        free(dark);
        free(bfi);
        free(pacing);
        if (!pacing_ok) {
            unlink(runtime_cfg);
            free(runtime_cfg);
            return fail("frame-pacing keys were not pinned for the session");
        }
        if (jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                       error, sizeof(error)) != 0) {
            unlink(runtime_cfg);
            free(runtime_cfg);
            return fail(error[0] ? error : "frame-pacing backup failed");
        }
        unlink(runtime_cfg);
        free(runtime_cfg);
        char *pacing_shared = read_text(shared_cfg);
        int pacing_shared_ok = pacing_shared &&
                               key_count(pacing_shared, "video_swap_interval", NULL) == 0 &&
                               key_count(pacing_shared, "video_bfi_dark_frames", NULL) == 0;
        free(pacing_shared);
        if (!pacing_shared_ok) {
            return fail("frame-pacing keys persisted to the shared config");
        }
    }
#endif

    /* Leaf#48 shared hardening: a backup that cannot be written must never
       destroy the durable config it was replacing. The old implementation
       opened it "wb" and truncated it before it knew the write could
       succeed. */
    {
        static const char durable_text[] =
            "menu_driver = \"rgui\"\n"
            "rewind_enable = \"true\"\n";
        if (write_text(shared_cfg, durable_text) != 0) {
            return fail("atomic-backup durable write failed");
        }
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "atomic-backup config generation failed");
        }
        /* Something the filtered result would definitely change, so a
           successful commit could not be mistaken for the skip-if-unchanged
           path. */
        char *session = read_text(runtime_cfg);
        char *changed = replace_line(
            session, "rewind_enable = \"true\"",
            "rewind_enable = \"false\"\ncheevos_password = \"hunter2\"");
        free(session);
        if (!changed) {
            return fail("atomic-backup session edit failed");
        }
        int wrote = write_text(runtime_cfg, changed);
        free(changed);
        if (wrote != 0) {
            return fail("atomic-backup session write failed");
        }

        /* Root ignores directory permissions, so only assert the read-only
           parent case where it actually applies. */
        if (geteuid() != 0) {
            if (chmod(retroarch_dir, 0500) != 0) {
                return fail("atomic-backup chmod failed");
            }
            int rc = jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                                error, sizeof(error));
            if (chmod(retroarch_dir, 0755) != 0) {
                return fail("atomic-backup chmod restore failed");
            }
            if (rc == 0) {
                return fail("unwritable backup destination reported success");
            }
            char *survivor = read_text(shared_cfg);
            int survived = survivor && strcmp(survivor, durable_text) == 0;
            free(survivor);
            if (!survived) {
                return fail("failed backup damaged the durable shared config");
            }
            if (!read_text(runtime_cfg)) {
                return fail("failed backup destroyed the working config");
            }
        }

        /* Commit failure with the rendered bytes already complete: they are
           kept as a recovery candidate rather than discarded. A directory
           where the durable file belongs makes rename() fail for root and
           non-root alike. */
        char recovered[PATH_MAX + 16];
        snprintf(recovered, sizeof(recovered), "%s.recovered", shared_cfg);
        unlink(recovered);
        if (unlink(shared_cfg) != 0 || mkdir(shared_cfg, 0755) != 0) {
            return fail("atomic-backup commit-failure fixture setup failed");
        }
        if (jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                       error, sizeof(error)) == 0) {
            return fail("uncommittable backup reported success");
        }
        struct stat dest_st;
        if (stat(shared_cfg, &dest_st) != 0 || !S_ISDIR(dest_st.st_mode)) {
            return fail("failed commit replaced the durable destination");
        }
        char *candidate = read_text(recovered);
        int candidate_ok =
            candidate &&
            key_count(candidate, "rewind_enable", "= \"false\"") == 1 &&
            /* Filtered, so it is safe to leave on the card: the raw working
               file's plaintext achievement password is not in it. */
            key_count(candidate, "cheevos_password", NULL) == 0 &&
            key_count(candidate, "cheevos_token", NULL) == 0;
        free(candidate);
        if (!candidate_ok) {
            return fail("commit failure left no secret-free recovery candidate");
        }
        unlink(runtime_cfg);
        free(runtime_cfg);
        if (rmdir(shared_cfg) != 0) {
            return fail("atomic-backup fixture teardown failed");
        }

        /* The candidate is the ONLY copy of that failed session. A later
           session saving successfully does not recover it -- that save came
           from a durable config which never held those settings -- so it must
           survive until it is provably redundant. */
        if (write_text(shared_cfg, durable_text) != 0) {
            return fail("recovery-retention durable write failed");
        }
        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "recovery-retention config failed");
        }
        char *later = read_text(runtime_cfg);
        char *later_changed = replace_line(later, "rewind_enable = \"true\"",
                                           "rewind_enable = \"maybe\"");
        free(later);
        if (!later_changed || write_text(runtime_cfg, later_changed) != 0) {
            free(later_changed);
            return fail("recovery-retention session write failed");
        }
        free(later_changed);
        if (jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                       error, sizeof(error)) != 0) {
            return fail(error[0] ? error : "recovery-retention backup failed");
        }
        char *kept = read_text(recovered);
        int kept_ok = kept && key_count(kept, "rewind_enable", "= \"false\"") == 1;
        free(kept);
        if (!kept_ok) {
            return fail("a later successful save destroyed the recovery candidate");
        }

        /* Once the durable config holds exactly those bytes, the candidate is
           redundant and should not linger. */
        char *redundant = read_text(shared_cfg);
        if (!redundant || write_text(recovered, redundant) != 0) {
            free(redundant);
            return fail("recovery-redundancy fixture write failed");
        }
        free(redundant);
        if (jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                       error, sizeof(error)) != 0) {
            return fail(error[0] ? error : "recovery-redundancy backup failed");
        }
        unlink(runtime_cfg);
        free(runtime_cfg);
        if (read_text(recovered)) {
            return fail("a redundant recovery candidate was left behind");
        }
    }

    /* Duplicate resolution must match RetroArch's: it keeps the FIRST
       occurrence of a key (libretro-common/file/config_file.c only adds a key
       that is not already in the map). Keeping the last, as an earlier
       implementation did, persisted a value the running session never used.

       Also a complexity guard: the rescan-per-line this replaced was O(n^2),
       and a config well inside the 4 MiB ceiling took long enough to blow
       through the shutdown budget that now bounds the backup. The bound below
       is deliberately loose -- it is here to catch a return to quadratic
       behaviour, not to measure the machine. */
    {
        size_t big_lines = 40000;
        size_t cap = big_lines * 64u + 256u;
        char *big = malloc(cap);
        if (!big) {
            return fail("duplicate/scale fixture allocation failed");
        }
        size_t off = 0;
        off += (size_t)sprintf(big + off, "dup_key = \"first\"\n");
        for (size_t i = 0; i < big_lines; i++) {
            off += (size_t)sprintf(big + off,
                                   "user_key_%06zu = \"value_%06zu\"\n", i, i);
        }
        off += (size_t)sprintf(big + off, "dup_key = \"last\"\n");
        int wrote = write_text(shared_cfg, big);
        free(big);
        if (wrote != 0) {
            return fail("duplicate/scale fixture write failed");
        }

        runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                  true, false, error, sizeof(error));
        if (!runtime_cfg) {
            return fail(error[0] ? error : "duplicate/scale config generation failed");
        }
        clock_t started = clock();
        int rc = jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                            error, sizeof(error));
        double elapsed_ms =
            (double)(clock() - started) * 1000.0 / (double)CLOCKS_PER_SEC;
        unlink(runtime_cfg);
        free(runtime_cfg);
        if (rc != 0) {
            return fail(error[0] ? error : "duplicate/scale backup failed");
        }

        char *big_shared = read_text(shared_cfg);
        int dup_ok = big_shared &&
                     key_count(big_shared, "dup_key", NULL) == 1 &&
                     key_count(big_shared, "dup_key", "= \"first\"") == 1;
        free(big_shared);
        if (!dup_ok) {
            return fail("backup did not keep RetroArch's first-wins duplicate");
        }
        if (elapsed_ms > 3000.0) {
            fprintf(stderr, "retroarch-config-test: backup of %zu lines took "
                            "%.0f ms; the per-line rescan is back\n",
                    big_lines, elapsed_ms);
            return 1;
        }
    }

    /* FlyCast Fast UMRK first-launch option seed. The profile is installed once,
       only while the core's .opt is absent; after the versioned stamp exists,
       the file belongs to the user and nothing here may touch it again. */
    {
        char seed_root[PATH_MAX], seed_home[PATH_MAX], seed_config[PATH_MAX];
        char seed_template_dir[PATH_MAX], seed_template[PATH_MAX];
        char seed_folder_dir[PATH_MAX], seed_dest[PATH_MAX], seed_stamp[PATH_MAX];
        snprintf(seed_root, sizeof(seed_root), "%s/flycast-seed", root);
        snprintf(seed_home, sizeof(seed_home), "%s/home", seed_root);
        snprintf(seed_config, sizeof(seed_config), "%s/config", seed_root);
        snprintf(seed_template_dir, sizeof(seed_template_dir),
                 "%s/defaults/retroarch/core-options/FlyCast Fast UMRK", platform);
        snprintf(seed_template, sizeof(seed_template),
                 "%s/FlyCast Fast UMRK.opt", seed_template_dir);
        snprintf(seed_folder_dir, sizeof(seed_folder_dir),
                 "%s/FlyCast Fast UMRK", seed_config);
        snprintf(seed_dest, sizeof(seed_dest),
                 "%s/FlyCast Fast UMRK.opt", seed_folder_dir);
        snprintf(seed_stamp, sizeof(seed_stamp),
                 "%s/.leaf-core-options-seed-flycast_fast_umrk-v1", seed_home);
        if (mkdir_one(seed_root) || mkdir_one(seed_home) || mkdir_one(seed_config) ||
            jw__mkdir_p_test(seed_template_dir)) {
            return fail("core option seed fixture mkdir failed");
        }

        static const char profile[] =
            "reicast_internal_resolution = \"640x480\"\n"
            "reicast_threaded_rendering = \"enabled\"\n"
            "reicast_texupscale = \"1\"\n";
        if (write_text(seed_template, profile) != 0) {
            return fail("core option template fixture failed");
        }
        setenv("UMRK_RETROARCH_CONFIG_DIR", seed_config, 1);

        char seed_error[256];
        /* Every other core is out of scope: no destination, no stamp, no error. */
        if (jw_retroarch_seed_core_options(seed_home, root, "flycast", "Flycast",
                                           seed_error, sizeof(seed_error)) != 0 ||
            path_exists(seed_folder_dir) || path_exists(seed_stamp)) {
            return fail("seed ran for a core it does not own");
        }

        /* First seed: the exact template bytes, and completion only afterwards. */
        if (jw_retroarch_seed_core_options(seed_home, root, "flycast_fast_umrk",
                                           "FlyCast Fast UMRK", seed_error,
                                           sizeof(seed_error)) != 0) {
            return fail(seed_error[0] ? seed_error : "first seed failed");
        }
        char *seeded = read_text(seed_dest);
        if (!seeded || strcmp(seeded, profile) != 0) {
            free(seeded);
            return fail("first seed did not copy the template byte for byte");
        }
        free(seeded);
        if (!path_exists(seed_stamp)) {
            return fail("first seed did not record completion");
        }

        /* Preservation: an existing file -- user-edited here, but any content --
           is never rewritten, and still counts as a completed seed. */
        static const char user_edited[] =
            "reicast_internal_resolution = \"1280x960\"\n";
        if (write_text(seed_dest, user_edited) != 0) {
            return fail("user profile fixture failed");
        }
        unlink(seed_stamp);   /* the pre-stamp upgrade case */
        if (jw_retroarch_seed_core_options(seed_home, root, "flycast_fast_umrk",
                                           "FlyCast Fast UMRK", seed_error,
                                           sizeof(seed_error)) != 0) {
            return fail(seed_error[0] ? seed_error : "preservation failed");
        }
        char *kept = read_text(seed_dest);
        if (!kept || strcmp(kept, user_edited) != 0) {
            free(kept);
            return fail("an existing core option file was rewritten");
        }
        free(kept);
        if (!path_exists(seed_stamp)) {
            return fail("preservation did not record completion");
        }

        /* Completed: a deliberate deletion stays deleted. Re-seeding it would
           make the user's own choice impossible to keep. */
        unlink(seed_dest);
        if (jw_retroarch_seed_core_options(seed_home, root, "flycast_fast_umrk",
                                           "FlyCast Fast UMRK", seed_error,
                                           sizeof(seed_error)) != 0) {
            return fail("a completed seed reported failure");
        }
        char *resurrected = read_text(seed_dest);
        if (resurrected) {
            free(resurrected);
            return fail("a completed seed resurrected a deleted profile");
        }

        /* A non-file completion marker is not completion. Refuse it instead of
           silently treating a corrupt or hostile path as a successful seed. */
        unlink(seed_stamp);
        if (mkdir(seed_stamp, 0755) != 0) {
            return fail("could not build the invalid-stamp fixture");
        }
        int rc = jw_retroarch_seed_core_options(seed_home, root,
                                                "flycast_fast_umrk",
                                                "FlyCast Fast UMRK", seed_error,
                                                sizeof(seed_error));
        if (rmdir(seed_stamp) != 0 || rc == 0 || !seed_error[0] ||
            path_exists(seed_dest)) {
            return fail("an invalid completion stamp was accepted");
        }

        /* Failed copy: reported, and no stamp, so the next launch retries. A
           regular file where the destination directory belongs is the
           privilege-independent way to make the copy fail. */
        if (rmdir(seed_folder_dir) != 0 ||
            write_text(seed_folder_dir, "not a directory\n") != 0) {
            return fail("could not build the failed-copy fixture");
        }
        rc = jw_retroarch_seed_core_options(seed_home, root, "flycast_fast_umrk",
                                            "FlyCast Fast UMRK", seed_error,
                                            sizeof(seed_error));
        unlink(seed_folder_dir);
        if (rc == 0 || !seed_error[0] || path_exists(seed_stamp) ||
            path_exists(seed_dest)) {
            return fail("a failed option copy was not surfaced cleanly");
        }

        /* Missing template: surfaced against the path the payload owns, again
           with no completion stamp. */
        unlink(seed_template);
        rc = jw_retroarch_seed_core_options(seed_home, root, "flycast_fast_umrk",
                                            "FlyCast Fast UMRK", seed_error,
                                            sizeof(seed_error));
        if (rc == 0 || strstr(seed_error, "template") == NULL ||
            path_exists(seed_stamp)) {
            return fail("a missing option template was not surfaced");
        }
        if (write_text(seed_template, profile) != 0) {
            return fail("core option template restore failed");
        }

        /* Config-folder validation: a folder that would climb out of the
           RetroArch config dir is refused, and so is a safe folder belonging to
           a different core -- the Fast profile must never land in the standard
           Flycast settings. */
        const char *bad_folders[] = { "../Flycast", "Flycast", "" };
        for (size_t i = 0; i < sizeof(bad_folders) / sizeof(bad_folders[0]); i++) {
            if (jw_retroarch_seed_core_options(seed_home, root, "flycast_fast_umrk",
                                               bad_folders[i], seed_error,
                                               sizeof(seed_error)) == 0 ||
                !seed_error[0] || path_exists(seed_stamp)) {
                return fail("a foreign config folder was accepted");
            }
        }

        unsetenv("UMRK_RETROARCH_CONFIG_DIR");
    }

    printf("retroarch-config-test: ok\n");
    return 0;
}
