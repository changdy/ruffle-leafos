SHELL := /bin/bash

CC ?= cc
CSTD := -std=c11
CWARN := -Wall -Wextra -Wpedantic -Wno-unused-parameter
BUILD ?= build
CFLAGS_PLATFORM ?=
LDFLAGS_PLATFORM ?=
PLATFORM ?= mac
MLP1_TOOLCHAIN_IMAGE ?= ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local
MLP1_BUILD_PROFILE ?= release
WORKSPACE_ROOT ?= $(abspath ..)

ifeq ($(PLATFORM),mlp1)
MLP1_FLAGS_MK ?= $(firstword $(wildcard /opt/mlp1-toolchain/umrk/mlp1-build-flags.mk $(WORKSPACE_ROOT)/mlp1-toolchain/flags/mlp1-build-flags.mk ../mlp1-toolchain/flags/mlp1-build-flags.mk))
ifneq ($(MLP1_FLAGS_MK),)
include $(MLP1_FLAGS_MK)
else
UMRK_MLP1_PROFILE_CFLAGS ?= -O2 -mcpu=cortex-a55 -mtune=cortex-a55 -ffunction-sections -fdata-sections -DNDEBUG
UMRK_MLP1_PROFILE_LDFLAGS ?= -Wl,--gc-sections
endif
CDEBUG ?= $(UMRK_MLP1_PROFILE_CFLAGS)
LDFLAGS_PLATFORM += $(UMRK_MLP1_PROFILE_LDFLAGS)
else
CDEBUG ?= -g -O0
endif

ifeq ($(PLATFORM),mlp1)
CFLAGS_PLATFORM += -DPLATFORM_MLP1
endif

DEFAULT_CATASTROPHE_DIR := $(if $(wildcard ../Catastrophe/include/catastrophe.h),$(abspath ../Catastrophe),third_party/catastrophe)
CATASTROPHE_DIR ?= $(DEFAULT_CATASTROPHE_DIR)
CATASTROPHE_INCLUDE := $(CATASTROPHE_DIR)/include
CATASTROPHE_HEADER := $(CATASTROPHE_INCLUDE)/catastrophe.h
CATASTROPHE_WIDGETS_HEADER := $(CATASTROPHE_INCLUDE)/catastrophe_widgets.h
CATASTROPHE_RES := $(CATASTROPHE_DIR)/res

SDL_CFLAGS := $(shell pkg-config --cflags sdl2 SDL2_ttf SDL2_image)
SDL_LDFLAGS := $(shell pkg-config --libs sdl2 SDL2_ttf SDL2_image)
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client 2>/dev/null)
WAYLAND_LDFLAGS := $(shell pkg-config --libs wayland-client 2>/dev/null)
WAYLAND_PROTOCOLS_DIR := $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)
WAYLAND_SCANNER ?= wayland-scanner
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LDFLAGS := $(shell pkg-config --libs libcurl 2>/dev/null)
# The Mac lane links the system libcurl when pkg-config has no entry; the
# mlp1 branch below still demands a pkg-config hit from the toolchain.
ifneq ($(PLATFORM),mlp1)
ifeq ($(strip $(CURL_LDFLAGS)),)
CURL_LDFLAGS := -lcurl
endif
endif

# ScreenScraper developer credentials. Prefer the current checkout's ignored
# .env.local, then the primary checkout's copy when building from a linked Git
# worktree. Release-candidate builds use temporary worktrees, and silently
# dropping the credentials there produces a launcher whose artwork picker can
# enumerate local games but can never enqueue a scrape.
JAWAKA_GIT_COMMON_DIR := $(shell git -C "$(CURDIR)" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
SCREENSCRAPER_ENV_FILE ?= $(firstword $(wildcard \
	$(CURDIR)/.env.local \
	$(JAWAKA_GIT_COMMON_DIR)/../.env.local))
ifneq ($(strip $(SCREENSCRAPER_ENV_FILE)),)
-include $(SCREENSCRAPER_ENV_FILE)
endif

# Export names into the MLP1 Docker build with `docker -e NAME`; values never
# appear in the echoed command line. Direct builds without credentials remain
# supported unless their caller explicitly requires the feature.
export SCREENSCRAPER_DEV_ID
export SCREENSCRAPER_DEV_PASSWORD
export SCREENSCRAPER_DEBUG_PASSWORD

SCREENSCRAPER_AVAILABLE := 0
ifneq ($(strip $(SCREENSCRAPER_DEV_ID)),)
ifneq ($(strip $(SCREENSCRAPER_DEV_PASSWORD)),)
SCREENSCRAPER_AVAILABLE := 1
endif
endif

SCREENSCRAPER_REQUIRED ?= 0
ifneq ($(filter 1 yes true,$(SCREENSCRAPER_REQUIRED)),)
ifeq ($(SCREENSCRAPER_AVAILABLE),0)
$(error ScreenScraper credentials are required; create .env.local or set SCREENSCRAPER_DEV_ID and SCREENSCRAPER_DEV_PASSWORD)
endif
endif

.PHONY: screenscraper-status
screenscraper-status:
	@printf '%s\n' '$(if $(filter 1,$(SCREENSCRAPER_AVAILABLE)),true,false)'

SCRAPE_CREDENTIALS_HEADER := $(BUILD)/generated/screenscraper_credentials.h

# Translation-coverage recorder: a DEV-BUILD TOOL, compiled out of every
# release, beta included. Build with I18N_COVERAGE=1 to run a coverage pass.
#
# It is off by default rather than gated at runtime because the only people who
# ever run a pass are the developers, on a device they can already deploy to --
# nobody was going to enable it on a shipped build, so shipping it only bought
# risk. On a release it would sit dormant behind a marker file that a restored
# backup or a copied card could create, and then append to a log on the user's
# SD card. This device already treats a dirty FAT as a hazard.

CFLAGS_COMMON := $(CSTD) $(CWARN) $(CDEBUG) $(CFLAGS_PLATFORM) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -I. -Iinternal -Ithird_party/cjson
ifeq ($(I18N_COVERAGE),1)
CFLAGS_COMMON += -DJW_I18N_COVERAGE
endif
CFLAGS_DAEMON := $(CFLAGS_COMMON)
CFLAGS_UI := $(CFLAGS_COMMON) -I$(CATASTROPHE_INCLUDE) -Ithird_party/miniz $(SDL_CFLAGS) $(CURL_CFLAGS)
LDLIBS_COMMON := $(LDFLAGS_PLATFORM) -lsqlite3
LDLIBS_DAEMON := $(LDLIBS_COMMON)
LDLIBS_UI := $(LDLIBS_COMMON) $(SDL_LDFLAGS) $(CURL_LDFLAGS) -lm -lpthread
ifeq ($(shell uname -s),Darwin)
LDLIBS_UI += -lobjc
endif
ifeq ($(PLATFORM),mlp1)
ifeq ($(strip $(CURL_LDFLAGS)),)
$(error PLATFORM=mlp1 requires libcurl in the toolchain; rebuild mlp1-toolchain)
endif
CFLAGS_DAEMON += -DJW_UPDATE_USE_LIBCURL=1 $(CURL_CFLAGS)
LDLIBS_DAEMON += $(CURL_LDFLAGS)
# MLP1 dismisses the stock boot transition from jawakad's platform backend by
# dlopen-ing libloong_sdk.so at runtime, which needs libdl.
LDLIBS_DAEMON += -ldl
PLATFORM_BACKEND_SRC := internal/platform/device_mlp1.c
PLATFORM_ID_SRC := internal/platform/platform_id_mlp1.c
INPUT_PROXY_SRC := internal/platform/input_proxy_mlp1.c
INPUT_ROSTER_SRC := internal/platform/input_roster_mlp1.c
EXTERNAL_INPUT_SRC := internal/platform/external_input_monitor_mlp1.c
BLUETOOTH_SRC := internal/platform/bluetooth_mlp1.c
WIFI_SRC := internal/platform/wifi_mlp1.c internal/platform/wifi_ssid.c \
	internal/platform/wifi_attempt.c
OSD_BACKEND_SRC := cmd/jawaka-osd/osd_wayland.c $(BUILD)/generated/xdg-shell-protocol.c
OSD_DEPS := $(BUILD)/generated/xdg-shell-client-protocol.h
# Banner text renders with SDL_ttf into the Wayland shm buffer; Wayland keeps
# display ownership. Only sdl2 and SDL2_ttf: the OSD has no use for SDL2_image.
OSD_CFLAGS := $(CFLAGS_COMMON) $(WAYLAND_CFLAGS) $(shell pkg-config --cflags sdl2 SDL2_ttf) -I$(BUILD)/generated -Ithird_party/stb
OSD_LDLIBS := $(LDLIBS_COMMON) $(WAYLAND_LDFLAGS) $(shell pkg-config --libs sdl2 SDL2_ttf) -lm
else
PLATFORM_BACKEND_SRC := internal/platform/device_mock.c
PLATFORM_ID_SRC := internal/platform/platform_id_mock.c
INPUT_PROXY_SRC := internal/platform/input_proxy_mock.c
INPUT_ROSTER_SRC := internal/platform/input_roster_mock.c
EXTERNAL_INPUT_SRC := internal/platform/external_input_monitor_mock.c
BLUETOOTH_SRC := internal/platform/bluetooth_unsupported.c
WIFI_SRC := internal/platform/wifi_unsupported.c internal/platform/wifi_attempt.c
OSD_BACKEND_SRC := cmd/jawaka-osd/osd_sdl.c
OSD_DEPS :=
OSD_CFLAGS := $(CFLAGS_UI)
OSD_LDLIBS := $(LDLIBS_UI)
endif
PLATFORM_COMMON_SRC := internal/platform/platform_common.c

# CAT-1 effective catalog. internal/retroarch/catalog.c resolves through it,
# so every build that links catalog.c links these too. Use lists are wrapped
# in $(sort ...) where a build already carried sha256.c or leaf_version.c --
# duplicates on a cc(1) command line are duplicate symbols, not a no-op.
EFFECTIVE_CATALOG_SRCS := \
	internal/catalog/effective.c \
	internal/catalog/json.c \
	internal/catalog/merge.c \
	internal/update/sha256.c \
	internal/platform/leaf_version.c
LEAF_VERSION_SRC := internal/platform/leaf_version.c
PAKRAT_CATALOG_SRC := internal/store/pakrat_catalog.c
PAKRAT_STATE_LOGIC_SRC := internal/store/pakrat_state_logic.c
# THEME-1 install-time validation and Pak Rat theme policy (miniz inflates).
THEME_PACKAGE_SRCS := \
	internal/launcher/image_header.c \
	internal/store/pakrat_themes.c \
	internal/store/theme_package.c
MINIZ_SRCS := \
	third_party/miniz/miniz.c \
	third_party/miniz/miniz_tdef.c \
	third_party/miniz/miniz_tinfl.c \
	third_party/miniz/miniz_zip.c
LEAF_THEMES_CONTRACT ?= $(WORKSPACE_ROOT)/leaf-contracts/contracts/leaf-themes
CONTENT_MANIFEST_SRC := internal/catalog/manifest.c

# ScreenScraper scrape engine (daemon-side; curl + vendored stb/miniz/md5).
SCRAPE_SRCS := \
	internal/scrape/ss_client.c \
	internal/scrape/scrape_catalog.c \
	internal/scrape/scrape_identity.c \
	internal/scrape/scrape_md5.c \
	internal/scrape/scrape_systems.c \
	internal/scrape/scrape_worker.c \
	internal/storage/health.c \
	third_party/md5/md5.c \
	third_party/miniz/miniz.c \
	third_party/miniz/miniz_tdef.c \
	third_party/miniz/miniz_tinfl.c \
	third_party/miniz/miniz_zip.c
SCRAPE_CFLAGS := -include $(SCRAPE_CREDENTIALS_HEADER) $(CURL_CFLAGS) \
	-Ithird_party/stb -Ithird_party/miniz -Ithird_party/md5

CFLAGS_DAEMON += $(SCRAPE_CFLAGS)
LDLIBS_DAEMON += $(CURL_LDFLAGS) -lpthread -lm

DAEMON_SRCS := \
	internal/discovery/content.c \
	internal/discovery/delete.c \
	internal/discovery/delete_job.c \
	cmd/jawakad/main.c \
	cmd/jawakad/osd_client.c \
	$(CONTENT_MANIFEST_SRC) \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/ipc/ipc_stream.c \
	internal/ipc/ipc_client.c \
	internal/ipc/ctl1.c \
	internal/ipc/life1.c \
	internal/launcher/active_game.c \
	internal/launcher/bios.c \
	internal/launcher/standalone_policy.c \
	internal/launcher/ra_account.c \
	internal/launcher/core_selection.c \
	internal/launcher/pico8.c \
	$(PLATFORM_COMMON_SRC) \
	internal/platform/device.c \
	$(BLUETOOTH_SRC) \
	$(WIFI_SRC) \
	$(PLATFORM_BACKEND_SRC) \
	$(PLATFORM_ID_SRC) \
	$(INPUT_PROXY_SRC) \
	$(INPUT_ROSTER_SRC) \
	$(EXTERNAL_INPUT_SRC) \
	internal/platform/calibration.c \
	internal/platform/input_shortcuts.c \
	$(LEAF_VERSION_SRC) \
	internal/platform/paths.c \
	internal/platform/perf_policy.c \
	internal/platform/raofflineproxy.c \
	internal/power/boot_resume.c \
	internal/power/power_hold_save.c \
	internal/power/power_hold_save_io.c \
	internal/power/power_hard_cut.c \
	internal/power/suspend_inhibit.c \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	internal/retroarch/command.c \
	internal/retroarch/legacy_migration.c \
	internal/retroarch/states.c \
	internal/storage/sources.c \
	internal/store/catalog_source.c \
	internal/store/managed_apps.c \
	internal/store/pakrat_recovery.c \
	internal/store/pakrat_txn.c \
	internal/update/update.c \
	internal/update/sha256.c \
	internal/db/db.c \
	internal/focus/focus.c \
	internal/db/relocation.c \
	internal/settings/appearance.c \
	internal/i18n/i18n.c \
	internal/settings/theme_resolve.c \
internal/discovery/art_path.c \
internal/discovery/discovery.c \
	$(SCRAPE_SRCS) \
	internal/services/manifest.c \
	internal/services/ownership.c \
	internal/services/lease.c \
	internal/services/stop.c \
	internal/services/reservation.c \
	internal/services/backoff.c \
	internal/services/dup_ids.c \
	internal/services/unverified_stop.c \
	internal/services/control_state.c \
	internal/services/legacy_ssh_migration.c \
	internal/services/log_redact.c \
	internal/services/launch.c \
	internal/services/supervisor.c \
	third_party/cjson/cJSON.c

RETROARCH_CTL_SRCS := \
	cmd/jawaka-retroarchctl/main.c \
	internal/retroarch/command.c

RETROARCH_RUNNER_SRCS := \
	cmd/jawaka-retroarch-runner/main.c \
	internal/core/log.c \
	$(PLATFORM_ID_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	internal/retroarch/command.c \
	third_party/cjson/cJSON.c

UPDATE_RUNNER_SRCS := \
	cmd/jawaka-update-runner/main.c \
	internal/update/sha256.c \
	third_party/cjson/cJSON.c

PLATFORM_CTL_SRCS := \
	cmd/jawaka-platformctl/main.c \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/ipc/ipc_client.c \
	internal/platform/input_shortcuts.c \
	internal/platform/platform_common.c \
	internal/i18n/i18n.c \
	$(PLATFORM_ID_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	third_party/cjson/cJSON.c

OSD_SRCS := \
	cmd/jawaka-osd/main.c \
	cmd/jawaka-osd/game_launch.c \
	cmd/jawaka-osd/osd_view.c \
	cmd/jawaka-osd/osd_layout.c \
	cmd/jawaka-osd/osd_text.c \
	$(OSD_BACKEND_SRC) \
	internal/core/log.c \
	internal/i18n/i18n.c \
	internal/ipc/ipc.c \
	$(PLATFORM_ID_SRC) \
	internal/platform/paths.c \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	third_party/cjson/cJSON.c

SCAN_SMOKE_SRCS := \
	cmd/jawaka-scan-smoke/main.c \
	$(PLATFORM_ID_SRC) \
	internal/db/db.c \
	internal/db/relocation.c \
	internal/discovery/art_path.c \
	internal/discovery/discovery.c \
	$(CONTENT_MANIFEST_SRC) \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	internal/storage/sources.c \
	third_party/cjson/cJSON.c

SCRAPE_SMOKE_SRCS := \
	cmd/jawaka-scrape-smoke/main.c \
	internal/discovery/art_path.c \
	$(SCRAPE_SRCS) \
	internal/core/log.c \
	internal/db/db.c \
	internal/db/relocation.c \
	$(PLATFORM_ID_SRC) \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	internal/storage/sources.c \
	third_party/cjson/cJSON.c

PAKRAT_SMOKE_SRCS := \
	cmd/jawaka-pakrat-smoke/main.c \
	$(LEAF_VERSION_SRC) \
	$(PAKRAT_CATALOG_SRC) \
	$(PAKRAT_STATE_LOGIC_SRC) \
	$(THEME_PACKAGE_SRCS) \
	internal/core/log.c \
	internal/launcher/user_themes.c \
	internal/store/pakrat.c \
	internal/store/pakrat_recovery.c \
	internal/store/pakrat_state.c \
	internal/store/pakrat_txn.c \
	internal/ipc/ipc.c \
	$(PLATFORM_ID_SRC) \
	internal/db/db.c \
	internal/db/relocation.c \
	internal/discovery/art_path.c \
	internal/discovery/discovery.c \
	$(CONTENT_MANIFEST_SRC) \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	internal/storage/sources.c \
	internal/store/catalog_source.c \
	internal/store/managed_apps.c \
	internal/services/manifest.c \
	internal/update/sha256.c \
	third_party/cjson/cJSON.c \
	third_party/miniz/miniz.c \
	third_party/miniz/miniz_tdef.c \
	third_party/miniz/miniz_tinfl.c \
	third_party/miniz/miniz_zip.c

CATALOG_SMOKE_SRCS := \
	cmd/jawaka-catalog-smoke/main.c \
	$(PLATFORM_ID_SRC) \
	internal/launcher/core_selection.c \
	internal/launcher/standalone_policy.c \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	third_party/cjson/cJSON.c

CONTENT_RUNTIME_SMOKE_SRCS := \
	cmd/jawaka-content-runtime-smoke/main.c \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	internal/platform/platform_id_mock.c \
	third_party/cjson/cJSON.c

I18N_TEST_SRCS := \
	cmd/jawaka-i18n-test/main.c \
	internal/i18n/i18n.c \
	internal/core/log.c

CORE_OVERRIDE_SMOKE_SRCS := \
	cmd/jawaka-core-override-smoke/main.c \
	$(PLATFORM_ID_SRC) \
	internal/db/db.c \
	internal/db/relocation.c \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	internal/storage/sources.c \
	third_party/cjson/cJSON.c

UPDATE_SMOKE_SRCS := \
	cmd/jawaka-update-smoke/main.c \
	$(LEAF_VERSION_SRC) \
	internal/update/update.c \
	internal/update/sha256.c \
	third_party/cjson/cJSON.c

INHIBIT_CTL_SRCS := \
	cmd/jawaka-inhibitctl/main.c \
	internal/ipc/ipc.c \
	third_party/cjson/cJSON.c

UI_SRCS := \
	internal/discovery/delete.c \
	internal/ipc/delete_client.c \
	internal/discovery/content.c \
	internal/core/log.c \
	internal/ipc/ipc.c \
	internal/ipc/ipc_client.c \
	$(PLATFORM_COMMON_SRC) \
	$(PLATFORM_ID_SRC) \
	internal/platform/cat_services.c \
	internal/platform/input_shortcuts.c \
	$(LEAF_VERSION_SRC) \
	internal/platform/paths.c \
	$(BLUETOOTH_SRC) \
	$(WIFI_SRC) \
	internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	internal/retroarch/shader_catalog.c \
	internal/retroarch/shader_picker.c \
	internal/retroarch/shader_strings.c \
	internal/retroarch/states.c \
	internal/storage/health.c \
	internal/storage/sources.c \
	internal/store/catalog_source.c \
	internal/store/managed_apps.c \
	$(PAKRAT_CATALOG_SRC) \
	$(PAKRAT_STATE_LOGIC_SRC) \
	$(THEME_PACKAGE_SRCS) \
	internal/store/pakrat.c \
	internal/store/pakrat_recovery.c \
	internal/store/pakrat_state.c \
	internal/store/pakrat_txn.c \
	internal/update/sha256.c \
	internal/discovery/art_path.c \
	internal/discovery/discovery.c \
	$(CONTENT_MANIFEST_SRC) \
	internal/db/db.c \
	internal/focus/focus.c \
	internal/db/relocation.c \
	internal/launcher/bios.c \
	internal/launcher/console_colors.c \
	internal/launcher/coverflow.c \
	internal/launcher/cover_loader.c \
	internal/launcher/grid.c \
	internal/launcher/grid_games.c \
	internal/launcher/user_themes.c \
	internal/launcher/focus_screen.c \
	internal/launcher/game_switcher.c \
	internal/launcher/standalone_policy.c \
	internal/launcher/system_names.c \
	internal/scrape/scrape_catalog.c \
	internal/settings/appearance.c \
	internal/i18n/i18n.c \
	internal/power/power_hard_cut.c \
	internal/settings/settings.c internal/settings/storage_ui.c \
	internal/storage/repair_advice.c \
	internal/settings/theme_resolve.c \
	internal/settings/timezones.c \
	internal/services/manifest.c \
	third_party/cjson/cJSON.c \
	third_party/miniz/miniz.c \
	third_party/miniz/miniz_tdef.c \
	third_party/miniz/miniz_tinfl.c \
	third_party/miniz/miniz_zip.c

ALL_BINS := \
	$(BUILD)/bin/jawakad \
	$(BUILD)/bin/jawaka-launcher \
	$(BUILD)/bin/jawaka-menu \
	$(BUILD)/bin/jawaka-osd \
	$(BUILD)/bin/jawaka-retroarchctl \
	$(BUILD)/bin/jawaka-retroarch-runner \
	$(BUILD)/bin/jawaka-update-runner \
	$(BUILD)/bin/jawaka-platformctl \
	$(BUILD)/bin/jawaka-inhibitctl

ifeq ($(PLATFORM),mlp1)
ALL_BINS += $(BUILD)/bin/jawaka-ledd
endif
ifeq ($(PLATFORM),mlp1)
ALL_OUTPUTS := $(ALL_BINS) $(BUILD)/build-manifest.json
else
ALL_OUTPUTS := $(ALL_BINS)
endif

.PHONY: all jawakad jawaka-launcher jawaka-menu jawaka-osd jawaka-retroarchctl jawaka-retroarch-runner jawaka-update-runner jawaka-platformctl jawaka-ledd jawaka-scan-smoke jawaka-scrape-smoke jawaka-pakrat-smoke jawaka-catalog-smoke jawaka-content-runtime-smoke jawaka-core-override-smoke jawaka-i18n-test i18n-test i18n-pot i18n-check jawaka-update-smoke jawaka-inhibitctl leaf-version-test wifi-ssid-test wifi-attempt-test input-shortcuts-test stick-calibration-test shortcut-dispatch-check shortcut-ipc-smoke jawaka-input-proxy-chord-test mlp1-adb-chord-test pakrat-catalog-test pakrat-state-logic-test pakrat-txn-test theme-package-test user-themes-test storage-sources-test storage-health-test storage-repair-advice-test source-paths-v2-smoke service-manifest-test content-manifest-test catalog-merge-test ownership-test lease-test stop-test reservation-test backoff-test dup-ids-test unverified-stop-test control-state-test legacy-ssh-migration-test log-redact-test log-heal-test launch-test supervisor-test service-fixtures service-fixture-test ctl1-test life1-test ipc-stream-test wire-fixture-test osd-game-launch-test osd-view-test osd-client-test osd-layout-test osd-banner-ui-test life1-subscriber-ipc-smoke life1-game-ipc-smoke life1-game-wait-ipc-smoke life1-game-check-ipc-smoke launch-core-pin-ipc-smoke life1-game-fallback-ipc-smoke life1-game-unmanaged-ipc-smoke life1-game-override-ipc-smoke life1-app-noevent-ipc-smoke active-game-recovery-ipc-smoke active-game-test writer-group-test service-client-test focus-test schema-v6-test rumble-settings-test relocation-test relocation-ipc-smoke package-quiesce-ipc-smoke power-transition-ipc-smoke power-hold-save-ipc-smoke boot-resume-ipc-smoke retroarch-frontend-ipc-smoke imported-title-test pinyin-search-test imported-title-ipc-smoke ra-account-test ra-account-launch-test ra-account-retroarch-test ra-account-contract-test ra-account-env-ipc-smoke flycast-ra-route-ipc-smoke settings-account-test settings-status-test states-core-test appearance-env-test jawaka-timezone-probe timezone-test mlp1-device-timezone-test legacy-migration-test shader-catalog-test shader-picker-test shader-menu-contract-test retroarch-command-test retroarch-config-test retroarch-recording-path-test retroarch-runner-stop-smoke retroarch-app-shutdown-ipc-smoke catalog-effective-test catalog-generation-smoke content-catalog-smoke catalog-folder-test catalog-frontend-test standalone-policy-test core-selection-test launch-notice-test bios-test bios-launch-contract-check scrape-systems-test ss-client-test suspend-inhibit-test power-hold-save-test boot-resume-test suspend-inhibit-ipc-smoke update-local-manifest-smoke update-github-check-smoke pakrat-state-smoke pakrat-history-smoke pakrat-recovery-smoke pakrat-theme-smoke pakrat-service-mutation-smoke mockgen run-daemon run-daemon-interactive run-daemon-only run-launcher run-menu run-interactive clean help tg5040 tg5050 my355 mlp1 mlp1-pakrat-smoke mlp1-inhibit-smoke mlp1-adb-smoke mlp1-adb-service-fixture-smoke mlp1-adb-pakrat-recovery-smoke mlp1-adb-service-mutation-smoke mlp1-adb-life1-smoke mlp1-adb-input-capture mlp1-adb-ra-command-smoke phase3-fixture-scan-smoke phase3-core-choice-smoke check-catastrophe check-sdl power-hard-cut-test FORCE

all: $(ALL_OUTPUTS)

jawakad: $(BUILD)/bin/jawakad
jawaka-launcher: $(BUILD)/bin/jawaka-launcher
jawaka-menu: $(BUILD)/bin/jawaka-menu
jawaka-osd: $(BUILD)/bin/jawaka-osd
jawaka-retroarchctl: $(BUILD)/bin/jawaka-retroarchctl
jawaka-retroarch-runner: $(BUILD)/bin/jawaka-retroarch-runner
jawaka-update-runner: $(BUILD)/bin/jawaka-update-runner
jawaka-platformctl: $(BUILD)/bin/jawaka-platformctl
ifeq ($(PLATFORM),mlp1)
jawaka-ledd: $(BUILD)/bin/jawaka-ledd
else
jawaka-ledd:
	@echo "jawaka-ledd is MLP1-only; build with PLATFORM=mlp1." >&2
	@exit 1
endif
jawaka-scan-smoke: $(BUILD)/bin/jawaka-scan-smoke
jawaka-scrape-smoke: $(BUILD)/bin/jawaka-scrape-smoke
jawaka-pakrat-smoke: $(BUILD)/bin/jawaka-pakrat-smoke
jawaka-catalog-smoke: $(BUILD)/bin/jawaka-catalog-smoke
jawaka-content-runtime-smoke: $(BUILD)/bin/jawaka-content-runtime-smoke
jawaka-core-override-smoke: $(BUILD)/bin/jawaka-core-override-smoke
jawaka-i18n-test: $(BUILD)/bin/jawaka-i18n-test

# Build and RUN the i18n engine test against its committed fixture. The binary
# needs a fixture directory holding a compiled zh_CN table; that fixture was never
# committed, so the test could not be run from the repo and nothing ran it.
i18n-test: $(BUILD)/bin/jawaka-i18n-test
	@fx=$$(mktemp -d); mkdir -p $$fx/platform/i18n $$fx/userdata/i18n; \
	python3 tools/i18n-compile.py cmd/jawaka-i18n-test/fixture.po \
		-o $$fx/platform/i18n/zh_CN.jwi >/dev/null && \
	$(BUILD)/bin/jawaka-i18n-test $$fx; rc=$$?; rm -rf $$fx; exit $$rc

# Regenerate the canonical key list from the sources. Commit the result --
# CI diffs it, so a UI-string change without a regenerated .pot fails there.
i18n-pot:
	python3 tools/gen-shader-strings.py
	python3 tools/i18n-extract.py --po $(wildcard i18n/*.po)

# What CI runs: the committed .pot must match the code, and any committed
# translation must parse, carry no orphan keys, and keep its printf
# conversions compatible (i18n-compile.py enforces that last one).
i18n-check:
	python3 tools/gen-shader-strings.py --check
	python3 tools/i18n-extract.py --check --po $(wildcard i18n/*.po)
	@for po in $(wildcard i18n/*.po); do \
		python3 tools/i18n-compile.py $$po -o /tmp/i18n-check.jwi || exit 1; \
	done
jawaka-update-smoke: $(BUILD)/bin/jawaka-update-smoke
jawaka-inhibitctl: $(BUILD)/bin/jawaka-inhibitctl

leaf-version-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/leaf-version-test \
		internal/platform/leaf_version_test.c $(LEAF_VERSION_SRC) \
		third_party/cjson/cJSON.c
	$(BUILD)/bin/leaf-version-test

wifi-ssid-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/wifi-ssid-test \
		internal/platform/wifi_ssid_test.c internal/platform/wifi_ssid.c
	$(BUILD)/bin/wifi-ssid-test

wifi-attempt-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/wifi-attempt-test \
		internal/platform/wifi_attempt_test.c internal/platform/wifi_attempt.c
	$(BUILD)/bin/wifi-attempt-test

# Links input_shortcuts.c alone: it uses JW_UI(), which is a macro, so the
# i18n.h include costs nothing at link time.
# Shallow guard, not a unit test: no direct jw_db_*/sqlite3_* call in any of
# the shortcut-dispatch function bodies. It does not follow the call graph --
# the script header says so and says what that misses.
shortcut-dispatch-check:
	scripts/check-shortcut-dispatch-no-sqlite.sh

# Startup load + live IPC update, against a local daemon on a temp SD tree.
# Both live in jawakad, so no unit test over the shared model can reach them.
shortcut-ipc-smoke:
	scripts/shortcut-ipc-smoke.sh

input-shortcuts-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/input-shortcuts-test \
		internal/platform/input_shortcuts_test.c \
		internal/platform/input_shortcuts.c
	$(BUILD)/bin/input-shortcuts-test

stick-calibration-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/stick-calibration-test \
		internal/platform/calibration_test.c \
		internal/platform/calibration.c internal/core/log.c \
		third_party/cjson/cJSON.c -lm -lpthread
	$(BUILD)/bin/stick-calibration-test

pakrat-catalog-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/pakrat-catalog-test \
		internal/store/pakrat_catalog_test.c $(PAKRAT_CATALOG_SRC) \
		$(LEAF_VERSION_SRC) third_party/cjson/cJSON.c
	$(BUILD)/bin/pakrat-catalog-test

pakrat-state-logic-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/pakrat-state-logic-test \
		internal/store/pakrat_state_logic_test.c \
		$(PAKRAT_STATE_LOGIC_SRC) $(LEAF_VERSION_SRC) \
		third_party/cjson/cJSON.c
	$(BUILD)/bin/pakrat-state-logic-test

pakrat-txn-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DJW_ENABLE_FAULT_INJECTION=1 -Ithird_party/miniz \
		-o $(BUILD)/bin/pakrat-txn-test \
		internal/store/pakrat_txn_test.c internal/store/pakrat_txn.c \
		internal/store/pakrat_recovery.c internal/services/manifest.c \
		$(THEME_PACKAGE_SRCS) $(LEAF_VERSION_SRC) $(MINIZ_SRCS) \
		internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		third_party/cjson/cJSON.c $(LDLIBS_COMMON)
	$(BUILD)/bin/pakrat-txn-test

user-themes-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/user-themes-test \
		internal/launcher/user_themes_test.c internal/launcher/user_themes.c \
		internal/launcher/image_header.c internal/core/log.c third_party/cjson/cJSON.c
	$(BUILD)/bin/user-themes-test

# THEME-1 parity with leaf-contracts: every fixture against its hand-written
# expectation, and every in-memory variant the contract's own checks build
# against the reference validator's answer.
theme-package-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -Ithird_party/miniz -o $(BUILD)/bin/theme-package-test \
		internal/store/theme_package_test.c internal/store/theme_package.c \
		internal/launcher/image_header.c $(MINIZ_SRCS) third_party/cjson/cJSON.c
	python3 scripts/theme-package-corpus.py "$(LEAF_THEMES_CONTRACT)" "$(BUILD)/theme-package-corpus"
	$(BUILD)/bin/theme-package-test "$(LEAF_THEMES_CONTRACT)" "$(BUILD)/theme-package-corpus"

storage-sources-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/storage-sources-test \
		internal/storage/sources_test.c internal/storage/sources.c
	$(BUILD)/bin/storage-sources-test

# Weston outlives the Leaf generation that restarts it; see weston_initd.h.
# Every S49weston call in C must go through that header and its helpers.
.PHONY: weston-initd-test
weston-initd-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/weston-initd-test internal/platform/weston_initd_test.c
	$(BUILD)/bin/weston-initd-test
	@if grep -rn --include='*.c' '/etc/init.d/S49weston' cmd internal; then \
		echo "weston-initd-test: start S49weston through JW_WESTON_INITD (weston_initd.h)"; exit 1; fi
	@if grep -rnE -A2 --include='*.c' '(system|popen|exec[lv]p?e?) *\(' cmd internal | grep 'JW_WESTON'; then \
		echo "weston-initd-test: run JW_WESTON_INITD commands through jw_weston_initd_run/_spawn_detached"; exit 1; fi

.PHONY: power-request-test
power-request-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/power-request-test internal/platform/power_request_test.c
	$(BUILD)/bin/power-request-test

# jawaka-ledd ships for the MLP1 only, but it is plain libc: a host build is
# enough to prove it stops, and hands the LED ring back, when jawakad dies.
.PHONY: ledd-spawn-test
ledd-spawn-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/ledd-spawn-test-ledd cmd/jawaka-ledd/main.c
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/ledd-spawn-test internal/platform/ledd_spawn_test.c
	$(BUILD)/bin/ledd-spawn-test $(BUILD)/bin/ledd-spawn-test-ledd

# Battery Level's reader, palette, and flash (battery.h) and the write decision
# static, off, and battery share (steady.h), with temporary capacity files and
# no LED writes.
.PHONY: led-battery-test
led-battery-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/led-battery-test \
		cmd/jawaka-ledd/battery_test.c internal/platform/platform_common.c \
		internal/i18n/i18n.c internal/core/log.c
	$(BUILD)/bin/led-battery-test

storage-health-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/storage-health-test \
		internal/storage/health_test.c internal/storage/health.c internal/core/log.c
	$(BUILD)/bin/storage-health-test

storage-repair-advice-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/storage-repair-advice-test \
		internal/storage/repair_advice_test.c internal/storage/repair_advice.c
	$(BUILD)/bin/storage-repair-advice-test

source-paths-v2-smoke:
	BUILD="$(BUILD)" scripts/source-paths-v2-smoke.sh

schema-v6-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/schema-v6-test \
		internal/db/schema_v6_test.c internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(LDLIBS_COMMON)
	$(BUILD)/bin/schema-v6-test

.PHONY: visibility-test visibility-ui-test game-switcher-test content-test

.PHONY: delete-test deletion-db-test delete-ui-test delete-client-test rom-delete-ipc-smoke

delete-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -pthread -DJW_ENABLE_FAULT_INJECTION=1 -o $(BUILD)/bin/delete-test \
		internal/discovery/delete_test.c internal/discovery/delete.c internal/discovery/content.c
	$(BUILD)/bin/delete-test

deletion-db-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/deletion-db-test \
		internal/db/deletion_test.c internal/db/db.c internal/db/relocation.c \
		internal/storage/sources.c $(LDLIBS_COMMON)
	$(BUILD)/bin/deletion-db-test

delete-ui-test: | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $(BUILD)/bin/delete-ui-test \
		internal/launcher/delete_ui_test.c $(sort $(UI_SRCS)) $(LDLIBS_UI)
	CAT_FONTS_DIR="$(CATASTROPHE_DIR)/res" $(BUILD)/bin/delete-ui-test

delete-client-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/delete-client-test \
		internal/ipc/delete_client_test.c internal/ipc/delete_client.c \
		internal/ipc/ipc.c internal/core/log.c third_party/cjson/cJSON.c
	$(BUILD)/bin/delete-client-test

content-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/content-test \
		internal/discovery/content_test.c internal/discovery/content.c
	$(BUILD)/bin/content-test

visibility-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/visibility-test \
		internal/db/visibility_test.c internal/db/db.c internal/db/relocation.c \
		internal/storage/sources.c $(LDLIBS_COMMON)
	$(BUILD)/bin/visibility-test

visibility-ui-test: | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $(BUILD)/bin/visibility-ui-test \
		internal/launcher/visibility_ui_test.c $(sort $(UI_SRCS)) $(LDLIBS_UI)
	CAT_FONTS_DIR="$(CATASTROPHE_DIR)/res" $(BUILD)/bin/visibility-ui-test

game-switcher-test: | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $(BUILD)/bin/game-switcher-test \
		internal/launcher/game_switcher_test.c $(sort $(UI_SRCS)) $(LDLIBS_UI)
	CAT_FONTS_DIR="$(CATASTROPHE_DIR)/res" $(BUILD)/bin/game-switcher-test

rumble-settings-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/rumble-settings-test \
		internal/db/rumble_settings_test.c internal/db/db.c internal/db/relocation.c \
		internal/storage/sources.c $(LDLIBS_COMMON)
	$(BUILD)/bin/rumble-settings-test

ra-account-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -Dsqlite3_close=jw_test_sqlite3_close \
		-Dsqlite3_step=jw_test_sqlite3_step -o $(BUILD)/bin/ra-account-test \
		internal/db/ra_account_test.c internal/db/db.c internal/db/relocation.c \
		internal/storage/sources.c $(LDLIBS_COMMON)
	$(BUILD)/bin/ra-account-test

relocation-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/relocation-test \
		internal/db/relocation_test.c internal/db/db.c internal/db/relocation.c \
		internal/storage/sources.c $(LDLIBS_COMMON)
	$(BUILD)/bin/relocation-test

relocation-ipc-smoke:
	scripts/relocation-ipc-smoke.sh

package-quiesce-ipc-smoke:
	scripts/package-quiesce-ipc-smoke.sh

power-transition-ipc-smoke:
	scripts/power-transition-ipc-smoke.sh

power-hold-save-ipc-smoke: jawakad jawaka-platformctl
	scripts/power-hold-save-ipc-smoke.sh

boot-resume-ipc-smoke: jawakad jawaka-platformctl
	scripts/boot-resume-ipc-smoke.sh

# LEAF-RA-1: a content pak's own RetroArch launched in place of Leaf's, probed
# and version-gated (Linux container; takes about a minute).
retroarch-frontend-ipc-smoke: jawakad jawaka-platformctl
	scripts/retroarch-frontend-ipc-smoke.sh

# jawakad exits on SIGTERM while supervising a launcher child.
.PHONY: daemon-shutdown-smoke
daemon-shutdown-smoke:
	scripts/daemon-shutdown-smoke.sh

service-manifest-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/service-manifest-test \
		internal/services/manifest_test.c internal/services/manifest.c \
		third_party/cjson/cJSON.c
	$(BUILD)/bin/service-manifest-test

content-manifest-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) \
		-DJW_CONTENT_FIXTURES_ROOT=\"$(WORKSPACE_ROOT)/leaf-contracts/contracts/leaf-content/manifests\" \
		-DJW_CONTENT_SCRAPE_FIXTURE_PATH=\"$(WORKSPACE_ROOT)/leaf-contracts/contracts/leaf-content/scrape/fixtures.json\" \
		-o $(BUILD)/bin/content-manifest-test \
		internal/catalog/manifest_test.c internal/catalog/manifest.c \
		internal/retroarch/catalog.c $(EFFECTIVE_CATALOG_SRCS) \
		internal/platform/platform_id_mock.c third_party/cjson/cJSON.c
	$(BUILD)/bin/content-manifest-test

.PHONY: content-art-test
content-art-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DJW_ART_FIXTURE=\"$(WORKSPACE_ROOT)/leaf-contracts/contracts/leaf-content/art/fixtures.json\" \
		-o $(BUILD)/bin/content-art-test internal/catalog/art_test.c \
		internal/catalog/manifest.c internal/retroarch/catalog.c $(EFFECTIVE_CATALOG_SRCS) \
		internal/platform/platform_id_mock.c third_party/cjson/cJSON.c
	$(BUILD)/bin/content-art-test
	$(BUILD)/bin/content-art-test "$(WORKSPACE_ROOT)/leaf-contracts/contracts/leaf-content/art/grid-fixtures.json"

catalog-merge-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DJW_MERGE_FIXTURE=\"$(WORKSPACE_ROOT)/leaf-contracts/contracts/leaf-content/merge/fixtures.json\" -o $(BUILD)/bin/catalog-merge-test \
		internal/catalog/merge_test.c internal/catalog/merge.c \
		internal/catalog/json.c internal/update/sha256.c third_party/cjson/cJSON.c
	$(BUILD)/bin/catalog-merge-test

$(BUILD)/bin/ownership-test: internal/services/ownership_test.c \
		internal/services/ownership.c internal/services/ownership.h | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DJW_SVC_OWNERSHIP_TESTING -o $(BUILD)/bin/ownership-test \
		internal/services/ownership_test.c internal/services/ownership.c -lpthread

ownership-test: $(BUILD)/bin/ownership-test
	$(BUILD)/bin/ownership-test

lease-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/lease-test \
		internal/services/lease_test.c internal/services/lease.c
	$(BUILD)/bin/lease-test

stop-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/stop-test \
		internal/services/stop_test.c internal/services/stop.c
	$(BUILD)/bin/stop-test

osd-game-launch-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/osd-game-launch-test \
		cmd/jawaka-osd/game_launch_test.c cmd/jawaka-osd/game_launch.c \
		internal/i18n/i18n.c internal/core/log.c third_party/cjson/cJSON.c
	$(BUILD)/bin/osd-game-launch-test

osd-layout-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/osd-layout-test \
		cmd/jawaka-osd/osd_layout_test.c cmd/jawaka-osd/osd_layout.c
	$(BUILD)/bin/osd-layout-test

# Renders every banner with the real font code under SDL's dummy driver. Set
# JAWAKA_OSD_BANNER_DUMP=<dir> to write BMPs for inspection.
osd-banner-ui-test: | $(BUILD)/bin check-sdl
	$(CC) $(CFLAGS_UI) -o $(BUILD)/bin/osd-banner-ui-test \
		cmd/jawaka-osd/osd_banner_ui_test.c \
		$(sort $(filter-out cmd/jawaka-osd/main.c $(OSD_BACKEND_SRC),$(OSD_SRCS))) \
		$(LDLIBS_UI)
	CAT_FONTS_DIR="$(CATASTROPHE_DIR)/res" $(BUILD)/bin/osd-banner-ui-test

osd-view-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/osd-view-test \
		cmd/jawaka-osd/osd_view_test.c cmd/jawaka-osd/osd_view.c
	$(BUILD)/bin/osd-view-test

osd-client-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/osd-client-test \
		cmd/jawakad/osd_client_test.c cmd/jawakad/osd_client.c \
		internal/launcher/pico8.c internal/storage/sources.c internal/db/db.c \
		internal/db/relocation.c $(LDLIBS_COMMON)
	$(BUILD)/bin/osd-client-test

reservation-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/reservation-test \
		internal/services/reservation_test.c internal/services/reservation.c \
		third_party/cjson/cJSON.c
	$(BUILD)/bin/reservation-test

backoff-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/backoff-test \
		internal/services/backoff_test.c internal/services/backoff.c
	$(BUILD)/bin/backoff-test

dup-ids-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/dup-ids-test \
		internal/services/dup_ids_test.c internal/services/dup_ids.c
	$(BUILD)/bin/dup-ids-test

unverified-stop-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/unverified-stop-test \
		internal/services/unverified_stop_test.c internal/services/unverified_stop.c
	$(BUILD)/bin/unverified-stop-test

control-state-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/control-state-test \
		internal/services/control_state_test.c internal/services/control_state.c \
		-lsqlite3
	$(BUILD)/bin/control-state-test

legacy-ssh-migration-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/legacy-ssh-migration-test \
		internal/services/legacy_ssh_migration_test.c \
		internal/services/legacy_ssh_migration.c \
		internal/services/control_state.c -lsqlite3
	$(BUILD)/bin/legacy-ssh-migration-test

log-heal-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/log-heal-test \
		internal/core/log_heal_test.c internal/core/log.c
	$(BUILD)/bin/log-heal-test

log-redact-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/log-redact-test \
		internal/services/log_redact_test.c internal/services/log_redact.c
	$(BUILD)/bin/log-redact-test

launch-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/launch-test \
		internal/services/launch_test.c internal/services/launch.c \
		third_party/cjson/cJSON.c -lpthread
	$(BUILD)/bin/launch-test

supervisor-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/supervisor-test \
		internal/services/supervisor_test.c internal/services/supervisor.c \
		internal/services/manifest.c internal/services/lease.c \
		internal/services/launch.c internal/services/ownership.c \
		internal/services/stop.c internal/services/reservation.c \
		internal/services/backoff.c internal/services/dup_ids.c \
		internal/services/control_state.c \
		internal/services/legacy_ssh_migration.c \
		internal/core/log.c \
		third_party/cjson/cJSON.c -lsqlite3 -lpthread
	$(BUILD)/bin/supervisor-test

SERVICE_FIXTURE_ROOT := $(BUILD)/service-fixtures
SERVICE_FIXTURE_INVALID := $(WORKSPACE_ROOT)/leaf-contracts/contracts/leaf-services/manifests/invalid
SERVICE_FIXTURE_TEST_ROOT ?= $(abspath $(SERVICE_FIXTURE_ROOT))

$(BUILD)/bin/service-fixture: internal/services/fixtures/fixture_service.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $<

service-fixtures: $(BUILD)/bin/service-fixture
	python3 internal/services/fixtures/materialize.py \
		--templates internal/services/fixtures/paks \
		--binary $(BUILD)/bin/service-fixture \
		--canonical-invalid $(SERVICE_FIXTURE_INVALID) \
		--output $(SERVICE_FIXTURE_ROOT)

$(BUILD)/bin/service-fixture-test: service-fixtures \
		internal/services/fixtures/fixture_paks_test.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) \
		-DJW_TEST_SERVICE_FIXTURES_ROOT=\"$(SERVICE_FIXTURE_TEST_ROOT)\" \
		-o $(BUILD)/bin/service-fixture-test \
		internal/services/fixtures/fixture_paks_test.c \
		internal/services/supervisor.c internal/services/manifest.c \
		internal/services/lease.c internal/services/launch.c \
		internal/services/ownership.c internal/services/stop.c \
		internal/services/reservation.c internal/services/backoff.c \
		internal/services/dup_ids.c internal/services/control_state.c \
		internal/services/legacy_ssh_migration.c \
		internal/core/log.c \
		third_party/cjson/cJSON.c -lsqlite3 -lpthread

service-fixture-test: $(BUILD)/bin/service-fixture-test
	$(BUILD)/bin/service-fixture-test

ctl1-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/ctl1-test \
		internal/ipc/ctl1_test.c internal/ipc/ctl1.c \
		third_party/cjson/cJSON.c -lm
	$(BUILD)/bin/ctl1-test

life1-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/life1-test \
		internal/ipc/life1_test.c internal/ipc/life1.c \
		third_party/cjson/cJSON.c -lm
	$(BUILD)/bin/life1-test

wire-fixture-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) \
		-DJW_TEST_WIRE_FIXTURES_ROOT=\"$(WORKSPACE_ROOT)/leaf-contracts/contracts/leaf-services/wire-fixtures\" \
		-o $(BUILD)/bin/wire-fixture-test \
		internal/ipc/wire_fixture_test.c third_party/cjson/cJSON.c -lm
	$(BUILD)/bin/wire-fixture-test

active-game-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/active-game-test \
		internal/launcher/active_game_test.c \
		internal/launcher/active_game.c third_party/cjson/cJSON.c
	$(BUILD)/bin/active-game-test

writer-group-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/writer-group-test \
		internal/launcher/writer_group_test.c internal/services/ownership.c
	$(BUILD)/bin/writer-group-test

ipc-stream-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/ipc-stream-test \
		internal/ipc/ipc_stream_test.c internal/ipc/ipc_stream.c \
		internal/ipc/ipc.c
	$(BUILD)/bin/ipc-stream-test

$(BUILD)/bin/life1-fixture-service: internal/services/fixtures/life1_fixture_service.c \
		internal/ipc/ipc.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $^

$(BUILD)/bin/game-writer-fixture: internal/services/fixtures/game_writer_fixture.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $<

life1-subscriber-ipc-smoke: $(BUILD)/bin/life1-fixture-service
	scripts/life1-subscriber-ipc-smoke.sh

life1-game-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	scripts/life1-game-ipc-smoke.sh

life1-game-fallback-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	UMRK_LIFE1_SMOKE_SCENARIO=game-malformed scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-drop scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-timeout scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-stalled scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-never-subscribe scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-late-subscribe scripts/life1-game-ipc-smoke.sh

life1-game-wait-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	UMRK_LIFE1_SMOKE_SCENARIO=game-waiting scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-wait-expiry scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-start-now scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-slow-ready scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-reconnect scripts/life1-game-ipc-smoke.sh

life1-game-check-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-current scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-wait scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-play scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-cancel scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-expiry scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-timeout scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-malformed scripts/life1-game-check-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-check-unsafe-card scripts/life1-game-check-ipc-smoke.sh

launch-core-pin-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	UMRK_CORE_PIN_TARGET=path UMRK_CORE_PIN_PHASE=vanish scripts/launch-core-pin-ipc-smoke.sh
	UMRK_CORE_PIN_TARGET=retroarch UMRK_CORE_PIN_PHASE=vanish scripts/launch-core-pin-ipc-smoke.sh
	UMRK_CORE_PIN_TARGET=path UMRK_CORE_PIN_PHASE=cancel-fresh scripts/launch-core-pin-ipc-smoke.sh
	UMRK_CORE_PIN_TARGET=retroarch UMRK_CORE_PIN_PHASE=cancel-fresh scripts/launch-core-pin-ipc-smoke.sh

life1-game-unmanaged-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	UMRK_LIFE1_SMOKE_SCENARIO=game-disabled scripts/life1-game-ipc-smoke.sh
	UMRK_LIFE1_SMOKE_SCENARIO=game-no-pak scripts/life1-game-ipc-smoke.sh

life1-game-override-ipc-smoke: $(BUILD)/bin/life1-fixture-service $(BUILD)/bin/game-writer-fixture
	scripts/life1-game-override-ipc-smoke.sh

raofflineproxy-bridge-ipc-smoke: jawakad jawaka-platformctl
	scripts/raofflineproxy-bridge-ipc-smoke.sh

ra-account-env-ipc-smoke: jawakad jawaka-platformctl
	scripts/ra-account-env-ipc-smoke.sh

flycast-ra-route-ipc-smoke: jawakad jawaka-platformctl
	scripts/flycast-ra-route-ipc-smoke.sh

.PHONY: raofflineproxy-bridge-ipc-smoke raofflineproxy-bridge-test ra-account-env-ipc-smoke flycast-ra-route-ipc-smoke

life1-app-noevent-ipc-smoke: $(BUILD)/bin/life1-fixture-service
	scripts/life1-app-noevent-ipc-smoke.sh

active-game-recovery-ipc-smoke:
	scripts/active-game-recovery-ipc-smoke.sh

service-client-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/service-client-test \
		internal/ipc/service_client_test.c internal/ipc/ipc.c \
		internal/ipc/ipc_client.c internal/ipc/ctl1.c \
		internal/platform/input_shortcuts.c \
		internal/platform/platform_common.c internal/i18n/i18n.c \
		internal/core/log.c third_party/cjson/cJSON.c -lm
	$(BUILD)/bin/service-client-test

imported-title-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/imported-title-test \
		internal/db/imported_title_test.c internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(LDLIBS_COMMON)
	$(BUILD)/bin/imported-title-test

pinyin-search-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/pinyin-search-test \
		internal/db/pinyin_search_test.c internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(LDLIBS_COMMON)
	$(BUILD)/bin/pinyin-search-test

.PHONY: wordmark-test
wordmark-test: | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $(BUILD)/bin/wordmark-test \
		internal/launcher/wordmark_test.c $(sort $(UI_SRCS)) $(LDLIBS_UI)
	CAT_FONTS_DIR="$(CATASTROPHE_DIR)/res" $(BUILD)/bin/wordmark-test

# The RetroAchievements Accounts row reads the account through the handoff's
# validator. No renderer, so it runs wherever the UI sources build.
settings-account-test: | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $(BUILD)/bin/settings-account-test \
		internal/settings/settings_account_test.c $(sort $(UI_SRCS)) $(LDLIBS_UI)
	$(BUILD)/bin/settings-account-test

settings-status-test: | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $(BUILD)/bin/settings-status-test \
		internal/settings/settings_status_test.c $(sort $(UI_SRCS)) $(LDLIBS_UI)
	mkdir -p $(BUILD)/settings-status/i18n
	python3 tools/i18n-compile.py i18n/ru_RU.po -o $(BUILD)/settings-status/i18n/ru_RU.jwi
	CAT_FONTS_DIR="$(CATASTROPHE_DIR)/res" $(BUILD)/bin/settings-status-test $(BUILD)/settings-status

appearance-env-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -D_GNU_SOURCE -o $(BUILD)/bin/appearance-env-test \
		internal/settings/appearance_env_test.c internal/settings/appearance.c \
		internal/settings/theme_resolve.c internal/settings/timezones.c \
		internal/i18n/i18n.c internal/core/log.c \
		internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(BLUETOOTH_SRC) $(LDLIBS_COMMON)
	$(BUILD)/bin/appearance-env-test

# The time-zone table against the C library that will convert it. Build only,
# so the same rule serves the cross-build that `make mlp1-device-timezone-test`
# ships to a device.
jawaka-timezone-probe: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -D_GNU_SOURCE -o $(BUILD)/bin/timezone-probe \
		internal/settings/timezone_probe.c internal/settings/timezones.c \
		$(LDLIBS_COMMON)

# Host run. The device run is the one that counts: the host's zoneinfo is not
# the device's, and this table is only as good as the data under it.
timezone-test: jawaka-timezone-probe
	$(BUILD)/bin/timezone-probe

# Cross-build the probe and run it on an MLP1. It converts fixed UTC instants
# and never touches the system clock, so it is safe with the launcher up.
mlp1-device-timezone-test:
	docker run --rm \
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-v "$(CURDIR)":/workspace/Jawaka \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile timezone-probe
	scripts/mlp1-timezone-probe.sh

imported-title-ipc-smoke:
	scripts/imported-title-ipc-smoke.sh

focus-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/focus-test \
		internal/focus/focus_test.c internal/focus/focus.c \
		internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		internal/update/sha256.c $(LDLIBS_COMMON)
	$(BUILD)/bin/focus-test

states-core-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/states-core-test \
		internal/retroarch/states_core_test.c internal/retroarch/states.c
	$(BUILD)/bin/states-core-test

legacy-migration-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/legacy-migration-test \
		internal/retroarch/legacy_migration_test.c \
		internal/retroarch/legacy_migration.c internal/retroarch/catalog.c \
		$(EFFECTIVE_CATALOG_SRCS) \
		internal/platform/platform_id_mock.c third_party/cjson/cJSON.c
	$(BUILD)/bin/legacy-migration-test

shader-catalog-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/shader-catalog-test \
		internal/retroarch/shader_catalog_test.c \
		internal/retroarch/shader_catalog.c third_party/cjson/cJSON.c
	$(BUILD)/bin/shader-catalog-test

shader-picker-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/shader-picker-test \
		internal/retroarch/shader_picker_test.c \
		internal/retroarch/shader_picker.c
	$(BUILD)/bin/shader-picker-test

shader-menu-contract-test:
	bash scripts/check-shader-menu-contract.sh

retroarch-command-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/retroarch-command-test \
		internal/retroarch/command_test.c internal/retroarch/command.c
	$(BUILD)/bin/retroarch-command-test

# Chord state machine, built for the device only: it includes
# input_proxy_mlp1.c to reach the static handler, and that needs
# <linux/input.h> and uinput. `make mlp1-adb-chord-test` cross-builds this and
# runs it over adb.
jawaka-input-proxy-chord-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/input-proxy-chord-test \
		internal/platform/input_proxy_chord_test.c \
		internal/platform/input_shortcuts.c \
		internal/platform/calibration.c internal/core/log.c \
		third_party/cjson/cJSON.c -lm -lpthread

RETROARCH_CONFIG_TEST_SRCS := \
	internal/platform/paths_config_test.c internal/platform/paths.c \
	internal/platform/platform_id_mock.c internal/retroarch/catalog.c \
	$(EFFECTIVE_CATALOG_SRCS) \
	internal/core/log.c third_party/cjson/cJSON.c

# Both platform shapes, because the hotkey ownership/migration matrix and the
# rest of the MLP1 protected-key policy only compile under -DPLATFORM_MLP1,
# while this Mac lane's default build is the one that covers the generic path.
# The credential round trip parses with a vendored copy of RetroArch's config
# parser; the drift check compares it with a fetched RetroArch tree when one
# exists (RETROARCH_SRC, default ../retroarch-builds/workdir/src/RetroArch).
retroarch-config-test: | $(BUILD)/bin
	sh scripts/check-retroarch-config-parser.sh
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/retroarch-config-test \
		$(RETROARCH_CONFIG_TEST_SRCS)
	$(BUILD)/bin/retroarch-config-test
	$(CC) $(CFLAGS_COMMON) -DPLATFORM_MLP1 \
		-o $(BUILD)/bin/retroarch-config-mlp1-test \
		$(RETROARCH_CONFIG_TEST_SRCS)
	$(BUILD)/bin/retroarch-config-mlp1-test

# standalone-ra-account-v1 producer replay against the pinned contract. The
# pin (commit + fixtures sha256) lives here and in
# .github/workflows/ra-account.yml; bump both together in a reviewed change.
LEAF_CONTRACTS_DIR ?= $(WORKSPACE_ROOT)/leaf-contracts
RA_ACCOUNT_FIXTURES_SHA256 := 981a98efb4f55598918e410d6a443fb2ddf0330e2ee46138175b9cb53d18d534
ra-account-contract-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/ra-account-contract-test \
		internal/launcher/ra_account_contract_test.c \
		internal/launcher/ra_account.c internal/launcher/standalone_policy.c \
		internal/platform/leaf_version.c third_party/cjson/cJSON.c \
		internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(LDLIBS_COMMON)
	python3 scripts/ra-account-contract-replay.py \
		--contracts "$(LEAF_CONTRACTS_DIR)" \
		--sha256 $(RA_ACCOUNT_FIXTURES_SHA256) \
		--producer $(BUILD)/bin/ra-account-contract-test

# The RetroArch half of the account handoff: stored rows -> the launch
# resolve -> JAWAKA_CHEEVOS_* -> cheevos_username/password in the config.
ra-account-retroarch-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/ra-account-retroarch-test \
		internal/launcher/ra_account_retroarch_test.c \
		internal/launcher/ra_account.c internal/launcher/standalone_policy.c \
		internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(filter-out internal/platform/paths_config_test.c,$(RETROARCH_CONFIG_TEST_SRCS)) \
		$(LDLIBS_COMMON)
	$(BUILD)/bin/ra-account-retroarch-test

raofflineproxy-bridge-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/raofflineproxy-bridge-test \
		internal/platform/raofflineproxy_bridge_test.c internal/platform/paths.c \
		internal/platform/raofflineproxy.c \
		internal/platform/platform_id_mock.c internal/retroarch/catalog.c \
		internal/launcher/ra_account.c internal/launcher/standalone_policy.c \
		internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(EFFECTIVE_CATALOG_SRCS) \
		internal/core/log.c third_party/cjson/cJSON.c -lpthread $(LDLIBS_COMMON)
	$(BUILD)/bin/raofflineproxy-bridge-test

retroarch-runner-stop-smoke: jawaka-retroarch-runner
	scripts/retroarch-runner-stop-smoke.sh

retroarch-app-shutdown-ipc-smoke: jawakad jawaka-platformctl
	scripts/retroarch-app-shutdown-ipc-smoke.sh

retroarch-recording-path-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DPLATFORM_MLP1 -o $(BUILD)/bin/retroarch-recording-path-test \
		internal/platform/paths_config_test.c internal/platform/paths.c \
		internal/platform/platform_id_mock.c internal/retroarch/catalog.c \
		$(EFFECTIVE_CATALOG_SRCS) \
		internal/core/log.c third_party/cjson/cJSON.c
	$(BUILD)/bin/retroarch-recording-path-test

# CAT-1. The expected digests come from the contract's own reference
# implementation (leaf-contracts/contracts/leaf-content/scripts/canonical.py), so this also
# proves the C canonical-bytes emitter agrees with it byte for byte.
catalog-effective-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/catalog-effective-test \
		internal/catalog/effective_test.c $(EFFECTIVE_CATALOG_SRCS) \
		internal/platform/platform_id_mock.c third_party/cjson/cJSON.c
	$(BUILD)/bin/catalog-effective-test

# End-to-end over a real release payload: the unit test uses a synthetic
# three-file catalog and cannot answer "is it the SAME catalog".
catalog-generation-smoke:
	scripts/catalog-generation-smoke.sh

content-catalog-smoke:
	scripts/content-catalog-smoke.sh

.PHONY: system-names-test
system-names-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/system-names-test \
		internal/launcher/system_names_test.c internal/launcher/system_names.c \
		internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		internal/i18n/i18n.c internal/core/log.c internal/retroarch/catalog.c \
		$(EFFECTIVE_CATALOG_SRCS) \
		internal/platform/platform_id_mock.c third_party/cjson/cJSON.c $(LDLIBS_COMMON)
	$(BUILD)/bin/system-names-test

catalog-folder-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/catalog-folder-test \
		internal/retroarch/catalog_folder_test.c internal/retroarch/catalog.c \
		$(EFFECTIVE_CATALOG_SRCS) \
		internal/platform/platform_id_mock.c third_party/cjson/cJSON.c
	$(BUILD)/bin/catalog-folder-test

# LEAF-RA-1: what the loader keeps of a core's "frontend" object and what
# jw_ra_catalog_resolve_frontend_path() makes of it. Fixtures live under /tmp:
# on a noowners bind mount access(X_OK) is true for every file.
catalog-frontend-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/catalog-frontend-test \
		internal/retroarch/catalog_frontend_test.c internal/retroarch/catalog.c \
		$(EFFECTIVE_CATALOG_SRCS) \
		internal/platform/platform_id_mock.c third_party/cjson/cJSON.c
	$(BUILD)/bin/catalog-frontend-test

bios-launch-contract-check:
	scripts/check-bios-launch-contract.sh

bios-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/bios-test \
		internal/launcher/bios_test.c internal/launcher/bios.c \
		internal/storage/sources.c
	$(BUILD)/bin/bios-test

standalone-policy-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/standalone-policy-test \
		internal/launcher/standalone_policy_test.c internal/launcher/standalone_policy.c
	$(BUILD)/bin/standalone-policy-test

ra-account-launch-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/ra-account-launch-test \
		internal/launcher/ra_account_test.c internal/launcher/ra_account.c \
		internal/launcher/standalone_policy.c \
		internal/platform/leaf_version.c third_party/cjson/cJSON.c \
		internal/db/db.c internal/db/relocation.c internal/storage/sources.c \
		$(LDLIBS_COMMON)
	$(BUILD)/bin/ra-account-launch-test

.PHONY: menu-escape-test
menu-escape-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/menu-escape-test \
		internal/launcher/menu_escape_test.c internal/launcher/standalone_policy.c
	$(BUILD)/bin/menu-escape-test

# Qualification tool only; intentionally absent from all/package targets.
.PHONY: content-menu-fixture
content-menu-fixture: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) $(SDL_CFLAGS) -o $(BUILD)/bin/content-menu-fixture \
		scripts/content-menu-fixture.c $(SDL_LDFLAGS)

core-selection-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/core-selection-test \
		internal/launcher/core_selection_test.c \
		internal/launcher/core_selection.c
	$(BUILD)/bin/core-selection-test

# The AUTO governor-profile rule: which systems enter performance without a
# user override. Dreamcast-family systems (DC, NAOMI, ATOMISWAVE) are the ones
# every Flycast choice depends on, standalone and libretro alike. Above 60 Hz
# AUTO boosts every game, since each frame gets one refresh of time.
.PHONY: perf-policy-test
perf-policy-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/perf-policy-test \
		internal/platform/perf_policy_test.c \
		internal/platform/perf_policy.c
	$(BUILD)/bin/perf-policy-test

# End-to-end governor policy against a real jawakad on the mock platform: every
# Dreamcast-family choice enters performance before the child runs and restores
# the frontend profile on exit, including a launch that fails after the profile
# was applied.
.PHONY: performance-dreamcast-ipc-smoke
performance-dreamcast-ipc-smoke:
	scripts/performance-dreamcast-ipc-smoke.sh

# Pure header over the shared system-notice primitive: set/expiry/only-a-new-
# launch-attempt-clears, and the two-line split. Link-free, so no product
# sources are dragged in.
launch-notice-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/launch-notice-test \
		internal/launcher/launch_notice_test.c
	$(BUILD)/bin/launch-notice-test

.PHONY: launch-notice-ui-test
launch-notice-ui-test: | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $(BUILD)/bin/launch-notice-ui-test \
		internal/launcher/launch_notice_ui_test.c $(sort $(UI_SRCS)) $(LDLIBS_UI)
	CAT_FONTS_DIR="$(CATASTROPHE_DIR)/res" $(BUILD)/bin/launch-notice-ui-test

.PHONY: scrape-art-test
scrape-art-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -Ithird_party/stb -Ithird_party/miniz -Ithird_party/md5 \
		-o $(BUILD)/bin/scrape-art-test \
		internal/scrape/scrape_art_test.c internal/scrape/scrape_worker.c \
		internal/scrape/scrape_catalog.c internal/scrape/scrape_identity.c \
		internal/scrape/scrape_md5.c internal/scrape/scrape_systems.c \
		internal/discovery/art_path.c internal/discovery/discovery.c \
		$(CONTENT_MANIFEST_SRC) internal/core/log.c internal/db/db.c \
		internal/db/relocation.c internal/storage/sources.c internal/storage/health.c \
		internal/retroarch/catalog.c $(EFFECTIVE_CATALOG_SRCS) \
		internal/platform/platform_id_mlp1.c \
		third_party/md5/md5.c third_party/miniz/miniz.c \
		third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c \
		third_party/miniz/miniz_zip.c third_party/cjson/cJSON.c \
		$(LDLIBS_COMMON) -lpthread -lm
	JW_TEST_DEFAULTS_DIR=../miniloong-launcher-switcher/device/mlp1/defaults \
		$(BUILD)/bin/scrape-art-test

.PHONY: cover-failure-test
cover-failure-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/cover-failure-test \
		internal/launcher/cover_loader_test.c internal/launcher/cover_loader.c -lpthread
	$(BUILD)/bin/cover-failure-test

.PHONY: art-path-test
art-path-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/art-path-test \
		internal/discovery/art_path_test.c internal/discovery/art_path.c \
		internal/retroarch/catalog.c $(EFFECTIVE_CATALOG_SRCS) \
		internal/platform/platform_id_mock.c third_party/cjson/cJSON.c
	$(BUILD)/bin/art-path-test

scrape-systems-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/scrape-systems-test \
		internal/scrape/scrape_systems_test.c \
		internal/scrape/scrape_systems.c internal/retroarch/catalog.c \
		$(EFFECTIVE_CATALOG_SRCS) internal/platform/platform_id_mock.c \
		third_party/cjson/cJSON.c
	$(BUILD)/bin/scrape-systems-test

.PHONY: scrape-identity-test
scrape-identity-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/scrape-identity-test \
		internal/scrape/scrape_identity_test.c \
		internal/scrape/scrape_identity.c
	$(BUILD)/bin/scrape-identity-test

ss-client-test: $(SCRAPE_CREDENTIALS_HEADER) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) $(SCRAPE_CFLAGS) -DJW_SS_TESTING \
		-o $(BUILD)/bin/ss-client-test \
		internal/scrape/ss_client_test.c \
		internal/scrape/ss_client.c internal/scrape/scrape_md5.c \
		third_party/md5/md5.c third_party/miniz/miniz.c \
		third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c \
		third_party/miniz/miniz_zip.c third_party/cjson/cJSON.c \
		$(LDLIBS_COMMON) $(CURL_LDFLAGS) -lpthread -lm
	$(BUILD)/bin/ss-client-test

suspend-inhibit-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/suspend-inhibit-test \
		internal/power/suspend_inhibit_test.c internal/power/suspend_inhibit.c
	$(BUILD)/bin/suspend-inhibit-test

power-hold-save-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/power-hold-save-test \
		internal/power/power_hold_save_test.c internal/power/power_hold_save.c
	$(BUILD)/bin/power-hold-save-test
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/power-hold-save-io-test \
		internal/power/power_hold_save_io_test.c internal/power/power_hold_save_io.c
	$(BUILD)/bin/power-hold-save-io-test
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/setting-nowait-test \
		internal/db/setting_nowait_test.c internal/db/db.c internal/db/relocation.c \
		internal/storage/sources.c $(LDLIBS_COMMON)
	$(BUILD)/bin/setting-nowait-test

# Resume Game on Boot record: write/parse/consume, the boot-time discards and
# the exact-slot check, on temp dirs.
boot-resume-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/boot-resume-test \
		internal/power/boot_resume_test.c internal/power/boot_resume.c \
		internal/retroarch/states.c internal/storage/sources.c \
		third_party/cjson/cJSON.c -lm
	$(BUILD)/bin/boot-resume-test

# Force Off Hold: seconds -> RK817 0xf7 bits, parse of the stored value with
# the 10 s default on garbage, and the read-modify-write on a register byte.
power-hard-cut-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/power-hard-cut-test \
		internal/power/power_hard_cut_test.c internal/power/power_hard_cut.c
	$(BUILD)/bin/power-hard-cut-test

suspend-inhibit-ipc-smoke:
	scripts/suspend-inhibit-ipc-smoke.sh

update-local-manifest-smoke:
	@scripts/update-local-manifest-smoke.sh

update-github-check-smoke:
	@scripts/update-github-check-smoke.sh

$(BUILD)/bin:
	@mkdir -p $(BUILD)/bin

check-catastrophe:
	@test -f "$(CATASTROPHE_HEADER)" || \
		( echo "Catastrophe headers not found. Set CATASTROPHE_DIR=/path/to/Catastrophe and retry." && exit 1 )

check-sdl:
	@pkg-config --exists sdl2 SDL2_ttf SDL2_image 2>/dev/null || \
		( echo "SDL2 libraries not found. Install with: brew install sdl2 sdl2_ttf sdl2_image" && exit 1 )

$(BUILD)/bin/jawakad: $(sort $(DAEMON_SRCS)) $(SCRAPE_CREDENTIALS_HEADER) | $(BUILD)/bin
	@echo "  CC      $@"
	@$(CC) $(CFLAGS_DAEMON) -o $@ $(sort $(DAEMON_SRCS)) $(LDLIBS_DAEMON)

$(BUILD)/bin/jawakad-delete-test: $(sort $(DAEMON_SRCS)) $(SCRAPE_CREDENTIALS_HEADER) | $(BUILD)/bin
	@$(CC) $(CFLAGS_DAEMON) -DJW_ENABLE_FAULT_INJECTION=1 -o $@ $(sort $(DAEMON_SRCS)) $(LDLIBS_DAEMON)

rom-delete-ipc-smoke: $(BUILD)/bin/jawakad-delete-test
	python3 scripts/rom-delete-ipc-smoke.py $(BUILD)/bin/jawakad-delete-test

$(BUILD)/bin/jawaka-launcher: cmd/jawaka-launcher/main.c internal/launcher/system_activity.h internal/launcher/launch_notice.h $(sort $(UI_SRCS)) $(CATASTROPHE_HEADER) $(CATASTROPHE_WIDGETS_HEADER) | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $@ cmd/jawaka-launcher/main.c $(sort $(UI_SRCS)) $(LDLIBS_UI)

$(BUILD)/bin/jawaka-menu: cmd/jawaka-menu/main.c $(sort $(UI_SRCS)) $(CATASTROPHE_HEADER) $(CATASTROPHE_WIDGETS_HEADER) | $(BUILD)/bin check-catastrophe check-sdl
	$(CC) $(CFLAGS_UI) -o $@ cmd/jawaka-menu/main.c $(sort $(UI_SRCS)) $(LDLIBS_UI)

$(BUILD)/generated:
	@mkdir -p $(BUILD)/generated

# Keep credential changes in make's dependency graph without exposing values in
# compiler command lines. FORCE reruns this small recipe; preserving the header
# mtime when its content is unchanged keeps incremental daemon builds fast.
$(SCRAPE_CREDENTIALS_HEADER): FORCE | $(BUILD)/generated
	@umask 077; tmp="$@.tmp"; \
		{ \
			printf '#define SCREENSCRAPER_DEV_ID "%s"\n' "$$SCREENSCRAPER_DEV_ID"; \
			printf '#define SCREENSCRAPER_DEV_PASSWORD "%s"\n' "$$SCREENSCRAPER_DEV_PASSWORD"; \
			printf '#define SCREENSCRAPER_DEBUG_PASSWORD "%s"\n' "$$SCREENSCRAPER_DEBUG_PASSWORD"; \
		} > "$$tmp"; \
		if test -f "$@" && cmp -s "$$tmp" "$@"; then \
			rm -f "$$tmp"; \
		else \
			mv -f "$$tmp" "$@"; \
		fi

$(BUILD)/generated/xdg-shell-client-protocol.h: | $(BUILD)/generated
	@test -n "$(WAYLAND_PROTOCOLS_DIR)" || { echo "wayland-protocols pkg-config data dir missing" >&2; exit 1; }
	$(WAYLAND_SCANNER) client-header "$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml" $@

$(BUILD)/generated/xdg-shell-protocol.c: $(BUILD)/generated/xdg-shell-client-protocol.h
	@test -n "$(WAYLAND_PROTOCOLS_DIR)" || { echo "wayland-protocols pkg-config data dir missing" >&2; exit 1; }
	$(WAYLAND_SCANNER) private-code "$(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml" $@

$(BUILD)/bin/jawaka-osd: $(sort $(OSD_SRCS)) $(OSD_DEPS) $(CATASTROPHE_HEADER) | $(BUILD)/bin
	$(CC) $(OSD_CFLAGS) -o $@ $(sort $(OSD_SRCS)) $(OSD_LDLIBS)

$(BUILD)/bin/jawaka-retroarchctl: $(sort $(RETROARCH_CTL_SRCS)) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(sort $(RETROARCH_CTL_SRCS))

$(BUILD)/bin/jawaka-retroarch-runner: $(sort $(RETROARCH_RUNNER_SRCS)) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(sort $(RETROARCH_RUNNER_SRCS))

$(BUILD)/bin/jawaka-update-runner: $(sort $(UPDATE_RUNNER_SRCS)) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(sort $(UPDATE_RUNNER_SRCS))

$(BUILD)/bin/jawaka-platformctl: $(sort $(PLATFORM_CTL_SRCS)) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(sort $(PLATFORM_CTL_SRCS)) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-scan-smoke: $(sort $(SCAN_SMOKE_SRCS)) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(sort $(SCAN_SMOKE_SRCS)) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-scrape-smoke: $(sort $(SCRAPE_SMOKE_SRCS)) $(SCRAPE_CREDENTIALS_HEADER) | $(BUILD)/bin
	@echo "  CC      $@"
	@$(CC) $(CFLAGS_COMMON) $(SCRAPE_CFLAGS) -o $@ $(sort $(SCRAPE_SMOKE_SRCS)) $(LDLIBS_COMMON) $(CURL_LDFLAGS) -lpthread -lm

$(BUILD)/bin/jawaka-pakrat-smoke: $(sort $(PAKRAT_SMOKE_SRCS)) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DJW_ENABLE_FAULT_INJECTION=1 $(CURL_CFLAGS) -Ithird_party/miniz -o $@ $(sort $(PAKRAT_SMOKE_SRCS)) $(LDLIBS_COMMON) $(CURL_LDFLAGS) -lm

$(BUILD)/bin/jawaka-catalog-smoke: $(sort $(CATALOG_SMOKE_SRCS)) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(sort $(CATALOG_SMOKE_SRCS))

$(BUILD)/bin/jawaka-content-runtime-smoke: $(sort $(CONTENT_RUNTIME_SMOKE_SRCS)) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(sort $(CONTENT_RUNTIME_SMOKE_SRCS))

# Always built WITH the coverage recorder: this binary exists to test it, and a
# test that silently compiles out what it asserts is worse than no test.
$(BUILD)/bin/jawaka-i18n-test: $(I18N_TEST_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DJW_I18N_COVERAGE -o $@ $(I18N_TEST_SRCS) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-core-override-smoke: $(sort $(CORE_OVERRIDE_SMOKE_SRCS)) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(sort $(CORE_OVERRIDE_SMOKE_SRCS)) $(LDLIBS_COMMON)

$(BUILD)/bin/jawaka-update-smoke: $(sort $(UPDATE_SMOKE_SRCS)) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -DJW_UPDATE_USE_LIBCURL=1 $(CURL_CFLAGS) -o $@ $(sort $(UPDATE_SMOKE_SRCS)) $(LDLIBS_COMMON) $(CURL_LDFLAGS) -lpthread

$(BUILD)/bin/jawaka-inhibitctl: $(INHIBIT_CTL_SRCS) | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ $(INHIBIT_CTL_SRCS)

ifeq ($(PLATFORM),mlp1)
$(BUILD)/bin/jawaka-ledd: cmd/jawaka-ledd/main.c cmd/jawaka-ledd/battery.h cmd/jawaka-ledd/steady.h | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ cmd/jawaka-ledd/main.c

$(BUILD)/build-manifest.json: $(ALL_BINS) FORCE
	@mkdir -p "$(BUILD)"
	@{ \
		printf '{\n'; \
		printf '  "platform": "mlp1",\n'; \
		printf '  "target_soc": "%s",\n' "$(UMRK_MLP1_TARGET_SOC)"; \
		printf '  "target_cpu": "%s",\n' "$(UMRK_MLP1_TARGET_CPU)"; \
		printf '  "build_profile": "%s",\n' "$(MLP1_BUILD_PROFILE)"; \
		printf '  "cflags": "%s",\n' "$(CDEBUG)"; \
		printf '  "ldflags": "%s",\n' "$(LDFLAGS_PLATFORM)"; \
		printf '  "features": {"screenscraper": %s, "i18n_coverage": %s},\n' "$(if $(filter 1,$(SCREENSCRAPER_AVAILABLE)),true,false)" "$(if $(filter 1,$(I18N_COVERAGE)),true,false)"; \
		printf '  "binaries": ["jawakad", "jawaka-launcher", "jawaka-menu", "jawaka-osd", "jawaka-retroarchctl", "jawaka-retroarch-runner", "jawaka-update-runner", "jawaka-platformctl", "jawaka-inhibitctl", "jawaka-ledd"],\n'; \
		printf '  "exceptions": []\n'; \
		printf '}\n'; \
	} > "$@"

endif

FORCE:

phase3-fixture-scan-smoke:
	scripts/phase3-fixture-scan-smoke.sh

phase3-core-choice-smoke:
	scripts/phase3-core-choice-smoke.sh

pakrat-state-smoke:
	scripts/pakrat-state-smoke.sh

pakrat-history-smoke:
	scripts/pakrat-history-smoke.sh

pakrat-recovery-smoke:
	scripts/pakrat-recovery-smoke.sh

pakrat-theme-smoke:
	BUILD="$(BUILD)" bash scripts/pakrat-theme-smoke.sh

pakrat-service-mutation-smoke:
	BUILD="$(BUILD)" bash scripts/pakrat-service-mutation-smoke.sh

mockgen:
	bash scripts/mockgen.sh

run-daemon: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_AUTODEMO="$${JAWAKA_AUTODEMO:-1}" \
	JAWAKA_AUTODEMO_DELAY_MS="$${JAWAKA_AUTODEMO_DELAY_MS:-1200}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawakad

run-daemon-interactive: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_AUTODEMO=0 \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawakad

run-daemon-only: $(BUILD)/bin/jawakad mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawakad --daemon-only

run-launcher: $(BUILD)/bin/jawaka-launcher mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawaka-launcher

run-interactive: run-daemon-interactive

run-menu: $(BUILD)/bin/jawaka-menu mockgen
	CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
	CAT_FONTS_DIR="$(CATASTROPHE_RES)" \
	CAT_THEMES_DIR="$(CATASTROPHE_RES)/themes" \
	JAWAKA_SDCARD_ROOT="$${JAWAKA_SDCARD_ROOT:-./mock-sdcard}" \
	JAWAKA_THEME="$${JAWAKA_THEME:-Jawaka-Tabs}" \
	$(BUILD)/bin/jawaka-menu

clean:
	rm -rf $(BUILD)

tg5040 tg5050 my355:
	$(MAKE) -C ports/$@ all

mlp1:
	docker run --rm \
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-e SCREENSCRAPER_REQUIRED="$(SCREENSCRAPER_REQUIRED)" \
		-e I18N_COVERAGE="$(I18N_COVERAGE)" \
		-e SCREENSCRAPER_ENV_FILE \
		-e SCREENSCRAPER_DEV_ID \
		-e SCREENSCRAPER_DEV_PASSWORD \
		-e SCREENSCRAPER_DEBUG_PASSWORD \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-v "$(CURDIR)":/workspace/Jawaka \
		-v "$(abspath $(CATASTROPHE_DIR))":/workspace/Catastrophe \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile all

mlp1-pakrat-smoke:
	docker run --rm \
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile pakrat-smoke

mlp1-inhibit-smoke:
	docker run --rm \
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile inhibit-smoke

mlp1-adb-smoke:
	scripts/adb-mlp1-smoke.sh

mlp1-adb-pakrat-recovery-smoke:
	scripts/adb-mlp1-pakrat-recovery-smoke.sh

mlp1-adb-service-mutation-smoke:
	scripts/adb-mlp1-service-mutation-smoke.sh

mlp1-adb-service-fixture-smoke:
	scripts/adb-mlp1-service-fixture-smoke.sh

mlp1-adb-life1-smoke:
	scripts/adb-mlp1-life1-smoke.sh

mlp1-adb-inhibit-smoke:
	scripts/adb-mlp1-suspend-inhibit-smoke.sh

mlp1-adb-input-capture:
	scripts/adb-mlp1-input-capture.sh

# Cross-build the chord state-machine test and run it on the device. Touches no
# real input device -- the proxy's fds are a temp file and -1 -- so it is safe
# to run while the launcher is up.
mlp1-adb-chord-test:
	docker run --rm \
		-e MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		-v "$(WORKSPACE_ROOT)":/workspace \
		-w /workspace/Jawaka \
		"$(MLP1_TOOLCHAIN_IMAGE)" \
		make -f ports/mlp1/Makefile chord-test
	scripts/adb-mlp1-chord-test.sh

mlp1-adb-ra-command-smoke:
	scripts/adb-mlp1-retroarch-command-smoke.sh $(if $(CONTENT),"$(CONTENT)")

help:
	@echo ""
	@echo "Jawaka build targets"
	@echo "===================="
	@echo "  make               Build jawakad, jawaka-launcher, jawaka-menu"
	@echo "  make mockgen       Create/update the mock SD-card tree"
	@echo "  make run-daemon              Run the daemon-driven phase-0/1 demo (auto-transitions)"
	@echo "  make run-daemon-interactive  Run daemon without auto-demo (stays open for testing)"
	@echo "  make run-daemon-only         Run jawakad without spawning launcher/menu"
	@echo "  make run-launcher            Run jawaka-launcher directly (requires daemon)"
	@echo "  make run-interactive         Alias for run-daemon-interactive"
	@echo "  make run-menu                Run jawaka-menu directly"
	@echo "  make jawaka-osd              Build the daemon-owned brightness OSD"
	@echo "  make jawaka-platformctl      Build platform status/control helper"
	@echo "  make jawaka-inhibitctl       Build suspend-inhibitor diagnostic helper"
	@echo "  make suspend-inhibit-test suspend-inhibit-ipc-smoke  Run native lease/power tests"
	@echo "  make power-hold-save-test  Run the power-hold save policy test"
	@echo "  make boot-resume-test     Run the Resume Game on Boot record test"
	@echo "  make power-hold-save-ipc-smoke  Drive jawakad through scripted power-key edges and a fake RetroArch (Linux container)"
	@echo "  make boot-resume-ipc-smoke  Arm a resume with a power-hold save, then boot again through each acceptance row (Linux container)"
	@echo "  make retroarch-frontend-ipc-smoke  Launch a content pak's own RetroArch (LEAF-RA-1) under jawakad: probe, menu, config version gate (Linux container)"
	@echo "  make catalog-frontend-test  Check the loader and resolver for a pak core's frontend"
	@echo "  make power-hard-cut-test  Force Off Hold value mapping and register rewrite"
	@echo "  make wifi-ssid-test          Validate wpa_supplicant SSID conversions"
	@echo "  make wifi-attempt-test       Validate when a Wi-Fi join attempt succeeds or fails"
	@echo "  make jawaka-retroarch-runner Build RetroArch app/config runner"
	@echo "  make jawaka-update-runner    Build OTA install handoff runner"
	@echo "  make jawaka-pakrat-smoke     Build local Pak Rat install/uninstall smoke helper"
	@echo "  make jawaka-catalog-smoke    Build metadata/core-choice smoke helper"
	@echo "  make standalone-policy-test  Validate standalone DRM/input classification"
	@echo "  make core-selection-test  Validate saved/default/alternate launch core order"
	@echo "  make perf-policy-test     Validate the AUTO performance-profile rule per system and refresh rate"
	@echo "  make performance-dreamcast-ipc-smoke  Verify DC-family launches enter and restore performance"
	@echo "  make launch-core-pin-ipc-smoke  Verify a pending launch never changes its selected core"
	@echo "  make launch-notice-test      Validate launch-notice set/expiry policy"
	@echo "  make launch-notice-ui-test   Validate notice rendering and expiry wakes"
	@echo "  make art-path-test         Validate box-art lookup order and extension casing"
	@echo "  make scrape-art-test       Validate missing-art checks against JPEG and fallback art"
	@echo "  make cover-failure-test    Validate launcher handling of undecodable cover art"
	@echo "  make update-local-manifest-smoke  Validate developer artifact.url handling"
	@echo "  make pakrat-state-smoke      Exercise Pak Rat stale + managed-state safeguards"
	@echo "  make pakrat-txn-test         Exercise service-pak mutation metadata and uninstall"
	@echo "  make theme-package-test      Check THEME-1 validation against leaf-contracts fixtures"
	@echo "  make pakrat-recovery-smoke   Exercise Pak Rat promote-transaction crash recovery"
	@echo "  make pakrat-theme-smoke      Exercise theme install, update, uninstall and refusals"
	@echo "  make pakrat-service-mutation-smoke  Exercise TXN-1 through jawakad and Pak Rat"
	@echo "  make clean         Remove build artifacts"
	@echo "  make tg5040        Placeholder cross-compile target"
	@echo "  make tg5050        Placeholder cross-compile target"
	@echo "  make my355         Placeholder cross-compile target"
	@echo "  make mlp1          Cross-compile for Miniloong Pocket 1"
	@echo "  make mlp1-pakrat-smoke  Cross-compile local Pak Rat smoke helper for MLP1"
	@echo "  make mlp1-adb-inhibit-smoke  Run the RTC-woken MLP1 suspend/reap smoke"
	@echo "  make mlp1-adb-smoke  Build, push to /tmp, and run an ADB UI smoke"
	@echo "  make mlp1-adb-pakrat-recovery-smoke  Run P1 recovery matrix on a real FAT card"
	@echo "  make mlp1-adb-service-mutation-smoke Run B4a service mutation matrix on both FAT cards"
	@echo "  make mlp1-adb-service-fixture-smoke  Run A2 service fixtures on an attached MLP1"
	@echo "  make mlp1-adb-life1-smoke  Run A3b LIFE-1 coordination on an attached MLP1"
	@echo "  make mlp1-adb-input-capture  Record Loong Gamepad evtest labels over ADB"
	@echo "  make mlp1-adb-ra-command-smoke  Run RetroArch command feature smoke over ADB"
	@echo "  make phase3-fixture-scan-smoke  Run metadata-aware scan fixture checks"
	@echo "  make phase3-core-choice-smoke  Run core picker path-core fixture checks"
	@echo ""
	@echo "Environment variables"
	@echo "====================="
	@echo "  JAWAKA_SDCARD_ROOT            Path to SD-card root (default: ./mock-sdcard)"
	@echo "  JAWAKA_AUTODEMO=1             Auto-transition launcher→menu→shutdown (default in run-daemon)"
	@echo "  JAWAKA_AUTODEMO_DELAY_MS=N    Delay before auto-demo action fires, ms (default: 1200)"
	@echo "  JAWAKA_THEME=Jawaka-Tabs      Launcher theme: Jawaka-Tabs, Jawaka-Vertical, Jawaka-Horizontal"
	@echo ""
	@echo "Catastrophe include root: $(CATASTROPHE_INCLUDE)"
	@echo ""

.PHONY: pico8-test
pico8-test: | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $(BUILD)/bin/pico8-test \
		internal/launcher/pico8_test.c internal/launcher/pico8.c \
		internal/storage/sources.c internal/db/db.c internal/db/relocation.c \
		$(LDLIBS_COMMON)
	$(BUILD)/bin/pico8-test

.PHONY: jawaka-input-roster-test
jawaka-input-roster-test: $(BUILD)/bin/jawaka-input-roster-test
$(BUILD)/bin/jawaka-input-roster-test: internal/platform/input_roster_test.c internal/platform/input_roster_mlp1.c internal/core/log.c | $(BUILD)/bin
	$(CC) $(CFLAGS_COMMON) -o $@ internal/platform/input_roster_test.c internal/core/log.c
