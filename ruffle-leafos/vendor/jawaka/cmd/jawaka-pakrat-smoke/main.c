#include "internal/store/pakrat.h"
#include "internal/store/pakrat_recovery.h"
#include "internal/store/pakrat_state.h"
#include "internal/store/theme_package.h"

#include <stdbool.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *action;
    const char *store_id;
    const char *version;
    jw_pakrat_context ctx;
} pakrat_smoke_opts;

static void jw__usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [options] install <store-id>\n"
        "       %s [options] install-target <store-id> <version>\n"
        "       %s [options] adopt <store-id>\n"
        "       %s [options] repair <store-id> <version>\n"
        "       %s [options] uninstall <store-id>\n"
        "       %s [options] rescan\n"
        "       %s [options] recover\n"
        "       %s [options] list\n"
        "       %s [options] preview <store-id>\n"
        "\n"
        "  install replaces only paks Pak Rat owns; adopt also takes over a pak\n"
        "  already present on disk from a manual install.\n"
        "  recover runs only install-transition recovery (no library rescan),\n"
        "  exactly as jawakad does at startup before the first scan.\n"
        "  preview fetches a theme's store preview into the cache the launcher uses.\n"
        "\n"
        "options:\n"
        "  --platform <id>        target platform namespace (default: PLATFORM or mac)\n"
        "  --sdcard-root <path>   SD root (default: SDCARD_PATH, JAWAKA_SDCARD_ROOT, ./mock-sdcard)\n"
        "  --state-dir <path>     UMRK internal data dir (default: <root>/.umrk/<platform>)\n"
        "  --db <path>            library DB (default: <state-dir>/library.db)\n"
        "  --platform-root <path> active platform manifest root (default: <root>/.system/leaf/platforms/<platform>)\n"
        "  --runtime-dir <path>   runtime lock root (default: JAWAKA_RUNTIME_DIR or socket parent)\n"
        "  --socket <path>        optional jawakad socket to notify after install/uninstall\n",
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0);
}

static const char *jw__env_or_null(const char *name) {
    const char *value = getenv(name);
    return (value && value[0]) ? value : NULL;
}

static int jw__copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0 || !value) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s", value);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__join2(char *out, size_t out_size, const char *a, const char *b) {
    int n = snprintf(out, out_size, "%s/%s", a, b);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__join3(char *out, size_t out_size, const char *a, const char *b,
                     const char *c) {
    int n = snprintf(out, out_size, "%s/%s/%s", a, b, c);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__join5(char *out, size_t out_size, const char *a, const char *b,
                     const char *c, const char *d, const char *e) {
    int n = snprintf(out, out_size, "%s/%s/%s/%s/%s", a, b, c, d, e);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

static int jw__parse_args(int argc, char **argv, pakrat_smoke_opts *opts) {
    memset(opts, 0, sizeof(*opts));
    const char *platform = jw__env_or_null("PLATFORM");
    jw__copy(opts->ctx.platform, sizeof(opts->ctx.platform),
             platform ? platform : "mac");

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.platform, sizeof(opts->ctx.platform), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--sdcard-root") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.sdcard_root, sizeof(opts->ctx.sdcard_root), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--state-dir") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.state_dir, sizeof(opts->ctx.state_dir), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.db_path, sizeof(opts->ctx.db_path), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--platform-root") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.platform_root, sizeof(opts->ctx.platform_root), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--runtime-dir") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.runtime_dir,
                         sizeof(opts->ctx.runtime_dir), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            if (jw__copy(opts->ctx.socket_path, sizeof(opts->ctx.socket_path), argv[i + 1]) != 0) {
                return -1;
            }
            i += 2;
        } else {
            break;
        }
    }
    if (i >= argc) {
        return -1;
    }
    opts->action = argv[i++];
    if (strcmp(opts->action, "install") == 0 ||
        strcmp(opts->action, "install-target") == 0 ||
        strcmp(opts->action, "adopt") == 0 ||
        strcmp(opts->action, "repair") == 0 ||
        strcmp(opts->action, "uninstall") == 0 ||
        strcmp(opts->action, "preview") == 0) {
        if (i >= argc || !argv[i][0]) {
            return -1;
        }
        opts->store_id = argv[i++];
    }
    if (strcmp(opts->action, "repair") == 0 ||
        strcmp(opts->action, "install-target") == 0) {
        if (i >= argc || !argv[i][0]) {
            return -1;
        }
        opts->version = argv[i++];
    }
    if (i != argc) {
        return -1;
    }

    if (!opts->ctx.sdcard_root[0]) {
        const char *root = jw__env_or_null("SDCARD_PATH");
        if (!root) {
            root = jw__env_or_null("JAWAKA_SDCARD_ROOT");
        }
        jw__copy(opts->ctx.sdcard_root, sizeof(opts->ctx.sdcard_root),
                 root ? root : "./mock-sdcard");
    }
    if (!opts->ctx.state_dir[0]) {
        const char *state = jw__env_or_null("UMRK_INTERNAL_DATA_PATH");
        if (state) {
            jw__copy(opts->ctx.state_dir, sizeof(opts->ctx.state_dir), state);
        } else if (jw__join3(opts->ctx.state_dir, sizeof(opts->ctx.state_dir),
                             opts->ctx.sdcard_root, ".umrk",
                             opts->ctx.platform) != 0) {
            return -1;
        }
    }
    if (!opts->ctx.db_path[0] &&
        jw__join2(opts->ctx.db_path, sizeof(opts->ctx.db_path),
                  opts->ctx.state_dir, "library.db") != 0) {
        return -1;
    }
    if (!opts->ctx.platform_root[0]) {
        const char *platform_root = jw__env_or_null("UMRK_PLATFORM_PATH");
        if (!platform_root) {
            platform_root = jw__env_or_null("SYSTEM_PATH");
        }
        if (platform_root) {
            jw__copy(opts->ctx.platform_root, sizeof(opts->ctx.platform_root),
                     platform_root);
        } else if (jw__join5(opts->ctx.platform_root,
                             sizeof(opts->ctx.platform_root),
                             opts->ctx.sdcard_root, ".system", "leaf",
                             "platforms", opts->ctx.platform) != 0) {
            return -1;
        }
    }
    if (!opts->ctx.socket_path[0]) {
        const char *socket = jw__env_or_null("JAWAKA_SOCKET_PATH");
        jw__copy(opts->ctx.socket_path, sizeof(opts->ctx.socket_path),
                 socket ? socket : "/tmp/jawaka-runtime/jawakad.sock");
    }
    if (!opts->ctx.runtime_dir[0]) {
        const char *runtime = jw__env_or_null("JAWAKA_RUNTIME_DIR");
        if (runtime) {
            jw__copy(opts->ctx.runtime_dir, sizeof(opts->ctx.runtime_dir),
                     runtime);
        } else if (jw__copy(opts->ctx.runtime_dir,
                            sizeof(opts->ctx.runtime_dir),
                            opts->ctx.socket_path) == 0) {
            char *slash = strrchr(opts->ctx.runtime_dir, '/');
            if (!slash) {
                return -1;
            }
            if (slash == opts->ctx.runtime_dir) {
                slash[1] = '\0';
            } else {
                *slash = '\0';
            }
        } else {
            return -1;
        }
    }

    return opts->ctx.platform[0] && opts->ctx.sdcard_root[0] &&
           opts->ctx.state_dir[0] && opts->ctx.db_path[0] &&
           opts->ctx.platform_root[0] ? 0 : -1;
}

static int jw__print_list(const jw_pakrat_context *ctx) {
    enum { JW_SMOKE_MAX_STATES = 128 };
    jw_pakrat_app_state *states = calloc(JW_SMOKE_MAX_STATES, sizeof(*states));
    if (!states) {
        return -1;
    }
    int count = 0;
    int rc = jw_pakrat_list_app_states(ctx, states, JW_SMOKE_MAX_STATES,
                                       &count);
    if (rc > 0) {
        printf("Pak Rat catalog URL is not configured\n");
        free(states);
        return 0;
    }
    if (rc == JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF) {
        fprintf(stderr, "Pak Rat catalog requires a newer Leaf\n");
        free(states);
        return -1;
    }
    if (rc < 0) {
        fprintf(stderr, "failed to load Pak Rat app states\n");
        free(states);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        const jw_pakrat_app_state *state = &states[i];
        printf("%s\t%s\t%s\tinstalled=%s\tmanaged=%d\tpath=%s%s"
               "\taction=%d\ttarget=%s\thistory=%d\tmissing_history=%d"
               "\tgated=%s\tmin_leaf=%s\tkind=%s\twithdrawn=%d"
               "\tslots_full=%d\n",
               jw_pakrat_app_status_name(state->status),
               state->package.id,
               state->package.version,
               state->installed_version[0] ? state->installed_version : "-",
               state->managed,
               jw_pakrat_install_path_display_prefix(state->package.install_path),
               state->package.install_path,
               state->primary_action_allowed,
               state->action_version[0] ? state->action_version : "-",
               state->action_uses_history,
               state->installed_version_missing_from_history,
               state->gated_version[0] ? state->gated_version : "-",
               state->gated_min_leaf_version[0]
                   ? state->gated_min_leaf_version
                   : "-",
               jw_pakrat_kind_name(state->package.kind),
               state->package.withdrawn, state->theme_slots_full);
    }
    free(states);
    return 0;
}

/* The launcher's preview fetch, driven from the catalog listing it reads. */
static int jw__print_preview(const jw_pakrat_context *ctx, const char *store_id) {
    enum { JW_SMOKE_MAX_STATES = 128 };
    jw_pakrat_app_state *states = calloc(JW_SMOKE_MAX_STATES, sizeof(*states));
    int count = 0;
    if (!states ||
        jw_pakrat_list_app_states(ctx, states, JW_SMOKE_MAX_STATES, &count) != 0) {
        fprintf(stderr, "failed to load Pak Rat app states\n");
        free(states);
        return -1;
    }
    int rc = -1;
    for (int i = 0; i < count; i++) {
        const jw_pakrat_catalog_package *pkg = &states[i].package;
        if (strcmp(pkg->id, store_id) != 0) {
            continue;
        }
        char path[PATH_MAX];
        rc = jw_pakrat_fetch_preview(ctx->state_dir, pkg->preview_url,
                                     pkg->preview_sha256, pkg->preview_size,
                                     path, sizeof(path));
        if (rc == 0) {
            printf("preview: %s\n", path);
        } else {
            printf("preview: refused\n");
        }
        break;
    }
    free(states);
    return rc;
}

static const char *jw__refusal_name(jw_pakrat_refusal refusal) {
    switch (refusal) {
    case JW_PAKRAT_REFUSED_NONE: return "none";
    case JW_PAKRAT_REFUSED_NEEDS_ADOPTION: return "needs-adoption";
    case JW_PAKRAT_REFUSED_WITHDRAWN: return "withdrawn";
    case JW_PAKRAT_REFUSED_RESERVED_NAME: return "reserved-name";
    case JW_PAKRAT_REFUSED_THEME_LIMIT: return "theme-limit";
    case JW_PAKRAT_REFUSED_INVALID_THEME: return "invalid-theme";
    case JW_PAKRAT_REFUSED_THEME_NOT_LISTED: return "theme-not-listed";
    case JW_PAKRAT_REFUSED_CHECKSUM: return "checksum";
    }
    return "unknown";
}

/* One line a smoke script can assert on: what the launcher would act on. */
static void jw__print_outcome(const jw_pakrat_outcome *outcome,
                              const char *error) {
    printf("outcome: kind=%s refusal=%s themes_changed=%d theme_updated=%d "
           "selection_cleared=%d theme_dir=%s reasons=",
           jw_pakrat_kind_name(outcome->kind),
           jw__refusal_name(outcome->refusal), outcome->themes_changed,
           outcome->theme_updated, outcome->theme_selection_cleared,
           outcome->theme_dir[0] ? outcome->theme_dir : "-");
    bool any = false;
    for (int i = 0; i < JW_THEME_REASON_COUNT; i++) {
        if (outcome->theme_reasons & (1ULL << i)) {
            printf("%s%s", any ? "," : "",
                   jw_theme_package_reason_slug((jw_theme_reason)i));
            any = true;
        }
    }
    printf("%s error=%s\n", any ? "" : "-", error[0] ? error : "-");
}

int main(int argc, char **argv) {
    pakrat_smoke_opts opts;
    if (jw__parse_args(argc, argv, &opts) != 0) {
        jw__usage(argv[0]);
        return 2;
    }

    jw_pakrat_outcome outcome;
    char error[512] = "";
    memset(&outcome, 0, sizeof(outcome));
    opts.ctx.outcome = &outcome;
    opts.ctx.error_message = error;
    opts.ctx.error_message_size = sizeof(error);
    int rc = -1;
    if (strcmp(opts.action, "install") == 0) {
        rc = jw_pakrat_install_app(&opts.ctx, opts.store_id, 0);
    } else if (strcmp(opts.action, "install-target") == 0) {
        rc = jw_pakrat_install_app_target(
            &opts.ctx, opts.store_id, opts.version, 0);
    } else if (strcmp(opts.action, "adopt") == 0) {
        rc = jw_pakrat_install_app(&opts.ctx, opts.store_id, 1);
    } else if (strcmp(opts.action, "repair") == 0) {
        rc = jw_pakrat_repair_app_version(
            &opts.ctx, opts.store_id, opts.version);
    } else if (strcmp(opts.action, "uninstall") == 0) {
        rc = jw_pakrat_uninstall_app(&opts.ctx, opts.store_id);
    } else if (strcmp(opts.action, "rescan") == 0) {
        rc = jw_pakrat_rescan(&opts.ctx);
    } else if (strcmp(opts.action, "recover") == 0) {
        jw_pakrat_recovery_context recovery;
        memset(&recovery, 0, sizeof(recovery));
        snprintf(recovery.platform, sizeof(recovery.platform), "%s",
                 opts.ctx.platform);
        snprintf(recovery.sdcard_root, sizeof(recovery.sdcard_root), "%s",
                 opts.ctx.sdcard_root);
        snprintf(recovery.state_dir, sizeof(recovery.state_dir), "%s",
                 opts.ctx.state_dir);
        snprintf(recovery.db_path, sizeof(recovery.db_path), "%s",
                 opts.ctx.db_path);
        rc = jw_pakrat_recover_installs(&recovery);
    } else if (strcmp(opts.action, "list") == 0) {
        rc = jw__print_list(&opts.ctx);
    } else if (strcmp(opts.action, "preview") == 0) {
        rc = jw__print_preview(&opts.ctx, opts.store_id);
    } else {
        jw__usage(argv[0]);
        rc = -1;
    }
    if (strcmp(opts.action, "install") == 0 ||
        strcmp(opts.action, "install-target") == 0 ||
        strcmp(opts.action, "adopt") == 0 ||
        strcmp(opts.action, "repair") == 0 ||
        strcmp(opts.action, "uninstall") == 0) {
        jw__print_outcome(&outcome, error);
    }
    return rc == 0 ? 0 : 1;
}
