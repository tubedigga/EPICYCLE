# EPICYCLE — MIT loop engine for Akai MPC Sample
# Cross-build: aarch64-linux-gnu-gcc (Debian/Ubuntu) or set CROSS=

APP        := epicycle
VERSION    := 0.2.0-loop
BUILD      := build
DIST       := dist

CROSS      ?= aarch64-linux-gnu-
CC_CROSS   := $(CROSS)gcc
CC_HOST    := gcc

CFLAGS_COMMON := -std=c11 -Wall -Wextra -Wpedantic -O2 -Isrc/include -pthread
CFLAGS_CROSS  := $(CFLAGS_COMMON) -static -DEPICYCLE_ON_DEVICE
CFLAGS_HOST   := $(CFLAGS_COMMON)
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

.PHONY: all host package clean test-host help

all: $(BUILD)/$(APP)

host: $(BUILD)/$(APP)-host

help:
	@echo "make / make all   — aarch64 static binary → $(BUILD)/$(APP)"
	@echo "make host         — native smoke binary"
	@echo "make package      — SD zip via tools/make_autorun.py → $(DIST)/"
	@echo "make test-host    — demo bank + offline preview WAV"
	@echo "make clean"

$(BUILD)/$(APP): $(SRCS) | $(BUILD)
	$(CC_CROSS) $(CFLAGS_CROSS) -o $@ $(SRCS) $(LDFLAGS_COMMON)
	@echo "built $@"

$(BUILD)/$(APP)-host: $(SRCS) | $(BUILD)
	$(CC_HOST) $(CFLAGS_HOST) -o $@ $(SRCS) $(LDFLAGS_COMMON)
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
		--extra install/recovery-hint.txt \
		--extra install/samples-hint.txt

test-host: host
	MPC_APP_DIR=$(CURDIR)/$(BUILD) $(BUILD)/$(APP)-host --version
	MPC_APP_DIR=$(CURDIR)/$(BUILD) $(BUILD)/$(APP)-host --preview $(BUILD)/preview.wav
	@python3 tools/check_preview.py $(BUILD)/preview.wav
	MPC_APP_DIR=$(CURDIR)/$(BUILD) $(BUILD)/$(APP)-host --demo

clean:
	rm -rf $(BUILD) $(DIST)
