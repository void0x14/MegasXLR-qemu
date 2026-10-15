# SPDX-License-Identifier: GPL-2.0-or-later
# The small controller builds independently of QEMU's Meson tree.
CC ?= cc
PYTHON ?= python3
BUILD ?= build
QEMU ?=
QEMU_IMG ?=
QEMU_DATA ?=
QEMU_MODULES ?=
TESSERACT ?=
TESSERACT_DATA ?=
CPPFLAGS += -Isrc -Ivendor
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra
COMMON = src/main.c src/engine.c src/platform.c src/rfb.c src/agent.c src/guest.c src/seed.c src/live.c
HEADERS = src/megas.h src/rfb.h src/platform.h src/guest.h src/seed.h src/live.h
WINDOWS := $(if $(filter Windows_NT,$(OS)),1,$(if $(findstring mingw,$(CC)),1,0))
ifeq ($(WINDOWS),1)
EXE = .exe
CORE_LIBS = -lws2_32 -lpsapi
APP_LDFLAGS = -municode
GUI_LIBS = -lgdi32 -luser32 -lcomdlg32 -lmsimg32 -lm
else
EXE =
CORE_LIBS =
GUI_LIBS = -lX11 -lm
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
endif

.PHONY: all headless clean test test-rfb test-platform test-seed test-engine test-guest test-smoke test-live test-agent test-bundle test-task-budget bundle
.PHONY: test-stream test-android test-isolation test-android-matrix-contract
all: $(BUILD)/megas$(EXE)
headless: $(BUILD)/megas-headless$(EXE)
$(BUILD):
	mkdir -p "$@"
$(BUILD)/megas$(EXE): $(COMMON) src/gui.c $(wildcard $(HEADERS)) $(wildcard vendor/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(APP_LDFLAGS) -o "$@" $(COMMON) src/gui.c $(CORE_LIBS) $(GUI_LIBS) $(LDLIBS)
$(BUILD)/megas-headless$(EXE): $(COMMON) $(wildcard $(HEADERS)) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DMEGAS_HEADLESS $(LDFLAGS) $(APP_LDFLAGS) -o "$@" $(COMMON) $(CORE_LIBS) -lm $(LDLIBS)
$(BUILD)/test-rfb$(EXE): tests/test_rfb.c src/rfb.c $(wildcard src/rfb.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/test_rfb.c $(CORE_LIBS) $(LDLIBS)
test-rfb: $(BUILD)/test-rfb$(EXE)
	"./$(BUILD)/test-rfb$(EXE)"
$(BUILD)/test-platform$(EXE): tests/test_platform.c src/platform.c $(wildcard src/platform.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/test_platform.c src/platform.c $(CORE_LIBS) $(LDLIBS)
test-platform: $(BUILD)/test-platform$(EXE)
	"./$(BUILD)/test-platform$(EXE)"
$(BUILD)/test-seed$(EXE): tests/test_seed.c src/seed.c src/platform.c $(wildcard src/seed.h src/platform.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/test_seed.c src/seed.c src/platform.c $(CORE_LIBS) $(LDLIBS)
test-seed: $(BUILD)/test-seed$(EXE)
	"./$(BUILD)/test-seed$(EXE)"
$(BUILD)/live-fixture$(EXE): tests/live_fixture.c src/live.c src/platform.c src/live.h src/platform.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/live_fixture.c src/live.c src/platform.c $(CORE_LIBS) $(LDLIBS)
test-stream: $(BUILD)/live-fixture$(EXE)
	$(PYTHON) tests/test_stream.py "$(BUILD)/live-fixture$(EXE)"
ifeq ($(WINDOWS),0)
$(BUILD)/test-isolation: tests/test_isolation.c src/platform.c src/platform.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -o "$@" tests/test_isolation.c src/platform.c $(LDLIBS)
test-isolation: $(BUILD)/test-isolation
	"./$(BUILD)/test-isolation"
$(BUILD)/test-android: tests/test_android.c $(filter-out src/main.c,$(COMMON)) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/test_android.c $(filter-out src/main.c src/engine.c,$(COMMON)) -lm $(LDLIBS)
test-android: $(BUILD)/test-android
	"./$(BUILD)/test-android"
$(BUILD)/test-engine: tests/test_engine.c $(filter-out src/main.c,$(COMMON)) $(wildcard $(HEADERS)) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" tests/test_engine.c $(filter-out src/main.c,$(COMMON)) -lm $(LDLIBS)
test-engine: $(BUILD)/test-engine
	"./$(BUILD)/test-engine"
$(BUILD)/test-guest: tests/test_guest.c src/guest.c src/platform.c $(wildcard src/guest.h src/platform.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -pthread -o "$@" tests/test_guest.c src/guest.c src/platform.c $(LDLIBS)
test-guest: $(BUILD)/test-guest
	"./$(BUILD)/test-guest"
test: test-rfb test-platform test-seed test-engine test-guest test-bundle test-live test-agent test-task-budget test-stream test-android test-isolation test-android-matrix-contract
else
test-engine test-guest test-android test-isolation:
	@echo "SKIP: $@ uses POSIX-only test fixtures and is not run on Windows."
test: test-rfb test-platform test-seed test-bundle test-live test-agent test-task-budget test-stream test-android-matrix-contract
endif
test-bundle:
	$(PYTHON) tests/test_bundle.py
test-android-matrix-contract:
	$(PYTHON) -m unittest discover -s tests -p 'test_android_matrix_contract.py'
test-task-budget:
	$(PYTHON) -m unittest discover -s tests -p 'test_task_budget.py'
test-smoke test-live: headless
	$(PYTHON) tests/test_live.py "$(BUILD)/megas-headless$(EXE)" --report "$(BUILD)/live-report.json"
test-agent: headless
	$(PYTHON) tests/test_agent.py "$(BUILD)/megas-headless$(EXE)"
bundle: all headless
	$(PYTHON) packaging/bundle.py --binary "$(BUILD)/megas$(EXE)" --headless "$(BUILD)/megas-headless$(EXE)" --output "$(BUILD)/bundle" $(if $(QEMU),--qemu "$(QEMU)") $(if $(QEMU_IMG),--qemu-img "$(QEMU_IMG)") $(if $(QEMU_DATA),--qemu-data "$(QEMU_DATA)") $(if $(QEMU_MODULES),--qemu-modules "$(QEMU_MODULES)") $(if $(TESSERACT),--tesseract "$(TESSERACT)") $(if $(TESSERACT_DATA),--tesseract-data "$(TESSERACT_DATA)")
clean:
	rm -rf "$(BUILD)"
