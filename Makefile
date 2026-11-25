# EPICYCLE — MIT loop engine for Akai MPC Sample
# Cross-build: aarch64-linux-gnu-gcc (Debian/Ubuntu) or set CROSS=

APP        := epicycle
VERSION    := 0.2.8-midi-rewrite
BUILD      := build
DIST       := dist

CROSS      ?= aarch64-linux-gnu-
CC_CROSS   := $(CROSS)gcc
CC_HOST    := gcc

INCLUDES      := -Isrc/include -Isrc/display -Ithird_party/stb
CFLAGS_COMMON := -std=c11 -Wall -Wextra -Wpedantic -O2 $(INCLUDES) -pthread
CFLAGS_CROSS  := $(CFLAGS_COMMON) -static -DEPICYCLE_ON_DEVICE
CFLAGS_HOST   := $(CFLAGS_COMMON)
# stb_image is third-party; drop -Wpedantic for that TU only.
CFLAGS_PNG    := -std=c11 -Wall -Wextra -O2 $(INCLUDES) -pthread
LDFLAGS_COMMON := -pthread -lm

SRCS := \
	src/main.c \
	src/platform/paths.c \
	src/platform/alsa_card.c \
	src/display/display.c \
	src/audio/audio.c \
	src/surface/surface.c \
	src/engine/engine.c \
	src/sample/wav.c \
	src/ui/ui.c

PNG_SRC := src/image/png.c

.PHONY: all host package clean test-host help assets

all: $(BUILD)/$(APP) assets

host: $(BUILD)/$(APP)-host assets

help:
	@echo "make / make all   — aarch64 static binary → $(BUILD)/$(APP)"
	@echo "make host         — native smoke binary"
	@echo "make package      — SD zip via tools/make_autorun.py → $(DIST)/"
	@echo "make test-host    — demo bank + offline preview WAV"
	@echo "make clean"

assets: $(BUILD)/assets/boot.png

$(BUILD)/assets/boot.png: assets/boot.png | $(BUILD)
	mkdir -p $(BUILD)/assets
	cp assets/boot.png $@

$(BUILD)/png.o: $(PNG_SRC) | $(BUILD)
	$(CC_CROSS) $(CFLAGS_PNG) -static -DEPICYCLE_ON_DEVICE -c $(PNG_SRC) -o $@

$(BUILD)/png-host.o: $(PNG_SRC) | $(BUILD)
	$(CC_HOST) $(CFLAGS_PNG) -c $(PNG_SRC) -o $@

$(BUILD)/$(APP): $(SRCS) $(BUILD)/png.o | $(BUILD)
	$(CC_CROSS) $(CFLAGS_CROSS) -o $@ $(SRCS) $(BUILD)/png.o $(LDFLAGS_COMMON)
	@echo "built $@"

$(BUILD)/$(APP)-host: $(SRCS) $(BUILD)/png-host.o | $(BUILD)
	$(CC_HOST) $(CFLAGS_HOST) -o $@ $(SRCS) $(BUILD)/png-host.o $(LDFLAGS_COMMON)
	@echo "built $@"

$(BUILD):
	mkdir -p $(BUILD)

package: $(BUILD)/$(APP)
	python3 tools/make_autorun.py \
		--name $(APP) \
		--version $(VERSION) \
		--program $(BUILD)/$(APP) \
		--outdir $(DIST) \
		--top-autorun install/top-autorun.sh \
		--assets assets \
		--extra install/recovery-hint.txt \
		--extra install/samples-hint.txt

test-host: host
	MPC_APP_DIR=$(CURDIR)/$(BUILD) $(BUILD)/$(APP)-host --version
	MPC_APP_DIR=$(CURDIR)/$(BUILD) $(BUILD)/$(APP)-host --midi-selftest
	MPC_APP_DIR=$(CURDIR)/$(BUILD) $(BUILD)/$(APP)-host --preview $(BUILD)/preview.wav
	@python3 tools/check_preview.py $(BUILD)/preview.wav
	MPC_APP_DIR=$(CURDIR)/$(BUILD) $(BUILD)/$(APP)-host --demo

clean:
	rm -rf $(BUILD) $(DIST)
