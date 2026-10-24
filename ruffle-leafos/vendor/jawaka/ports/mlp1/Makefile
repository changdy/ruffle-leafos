# ports/mlp1/Makefile
# Cross-compile dispatcher for the Miniloong Pocket 1 target.

SHELL := /bin/bash

JAWAKA_DIR ?= /workspace/Jawaka
BUILD ?= build/mlp1
CATASTROPHE_DIR ?= /workspace/Catastrophe
MLP1_BUILD_PROFILE ?= release
I18N_COVERAGE ?= 0

.PHONY: all pakrat-smoke inhibit-smoke chord-test timezone-probe clean

all:
	$(MAKE) -C "$(JAWAKA_DIR)" \
		BUILD="$(BUILD)" \
		CSTD="-std=gnu11" \
		PLATFORM=mlp1 \
		MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		I18N_COVERAGE="$(I18N_COVERAGE)" \
		CATASTROPHE_DIR="$(CATASTROPHE_DIR)" \
		all

pakrat-smoke:
	$(MAKE) -C "$(JAWAKA_DIR)" \
		BUILD="$(BUILD)" \
		CSTD="-std=gnu11" \
		PLATFORM=mlp1 \
		MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		I18N_COVERAGE="$(I18N_COVERAGE)" \
		CATASTROPHE_DIR="$(CATASTROPHE_DIR)" \
		jawaka-pakrat-smoke

inhibit-smoke:
	$(MAKE) -C "$(JAWAKA_DIR)" \
		BUILD="$(BUILD)" \
		CSTD="-std=gnu11" \
		PLATFORM=mlp1 \
		MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		I18N_COVERAGE="$(I18N_COVERAGE)" \
		CATASTROPHE_DIR="$(CATASTROPHE_DIR)" \
		jawaka-inhibitctl jawaka-platformctl

chord-test:
	$(MAKE) -C "$(JAWAKA_DIR)" \
		BUILD="$(BUILD)" \
		CSTD="-std=gnu11" \
		PLATFORM=mlp1 \
		MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		I18N_COVERAGE="$(I18N_COVERAGE)" \
		CATASTROPHE_DIR="$(CATASTROPHE_DIR)" \
		jawaka-input-proxy-chord-test

timezone-probe:
	$(MAKE) -C "$(JAWAKA_DIR)" \
		BUILD="$(BUILD)" \
		CSTD="-std=gnu11" \
		PLATFORM=mlp1 \
		MLP1_BUILD_PROFILE="$(MLP1_BUILD_PROFILE)" \
		I18N_COVERAGE="$(I18N_COVERAGE)" \
		CATASTROPHE_DIR="$(CATASTROPHE_DIR)" \
		jawaka-timezone-probe

clean:
	$(MAKE) -C "$(JAWAKA_DIR)" BUILD="$(BUILD)" clean
