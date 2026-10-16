# Run from the repository root: make -f tests/sameboy/Makefile test ...
SAMEBOY_REV := 26a56bd6a122a6318a456abd80558408742d5adf
SAMEBOY_DIR ?= build/tests/SameBoy-$(SAMEBOY_REV)
CORE := $(SAMEBOY_DIR)/build/lib/libsameboy.a
HARNESS := build/tests/color_camera
CC := gcc
CORE_OPTIONS := DISABLE_REWIND=1 DISABLE_DEBUGGER=1 DISABLE_CHEATS=1 DISABLE_CHEAT_SEARCH=1
CORE_DEFINES := -DGB_DISABLE_REWIND -DGB_DISABLE_DEBUGGER -DGB_DISABLE_CHEATS -DGB_DISABLE_CHEAT_SEARCH
CGB_ROM ?= build/gbc/photo.gbc
CGB_SYMBOLS ?= build/gbc/photo.noi
SAVE ?= assets/photo.sav
CGB_BOOT ?=
LEGACY_ROM ?=
LEGACY_SYMBOLS ?=
SCENARIOS := font-boundary transitions ui fresh fresh-zero fresh-color fresh-sensor normal color filters bad-version cancel-red cancel cancel-blue no-space shuffled incomplete duplicate neutral stress

.PHONY: all test
all: $(HARNESS)

$(SAMEBOY_DIR)/.source:
	mkdir -p build/tests $(SAMEBOY_DIR)
	curl --fail --location https://codeload.github.com/LIJI32/SameBoy/tar.gz/$(SAMEBOY_REV) -o build/tests/sameboy.tar.gz
	tar -xzf build/tests/sameboy.tar.gz --strip-components=1 -C $(SAMEBOY_DIR)
	touch $@

$(CORE): $(SAMEBOY_DIR)/.source
	$(MAKE) -C $(SAMEBOY_DIR) CC=$(CC) CONF=release $(CORE_OPTIONS) build/lib/libsameboy.a

$(HARNESS): tests/sameboy/color_camera.c $(CORE)
	mkdir -p build/tests
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror $(CORE_DEFINES) -I$(SAMEBOY_DIR) $< $(CORE) -lm -ldl -o $@

test: $(HARNESS)
	@test -f "$(CGB_BOOT)" -a -f "$(LEGACY_ROM)" -a -f "$(LEGACY_SYMBOLS)" || \
	    { echo 'Set CGB_BOOT, LEGACY_ROM, and LEGACY_SYMBOLS to existing files.'; exit 1; }
	@set -e; for scenario in $(SCENARIOS); do \
	    $(HARNESS) "$(CGB_ROM)" "$(CGB_SYMBOLS)" "$(SAVE)" "$(CGB_BOOT)" $$scenario > build/tests/$$scenario.log 2>&1 || \
	        { cat build/tests/$$scenario.log; exit 1; }; \
	    sed -n '/^PASS:/p' build/tests/$$scenario.log; \
	    case $$scenario in color|filters|bad-version|shuffled|neutral|stress) cp build/tests/hicolor.png build/tests/$$scenario.png ;; esac; \
	done
	$(HARNESS) "$(CGB_ROM)" "$(CGB_SYMBOLS)" "$(SAVE)" "$(CGB_BOOT)" color agb
	cp build/tests/hicolor.png build/tests/agb.png
	$(HARNESS) "$(LEGACY_ROM)" "$(LEGACY_SYMBOLS)" build/tests/color.sav "$(CGB_BOOT)" compat
	$(HARNESS) "$(CGB_ROM)" "$(CGB_SYMBOLS)" "$(SAVE)" "$(CGB_BOOT)" fresh agb
	$(HARNESS) "$(CGB_ROM)" "$(CGB_SYMBOLS)" "$(SAVE)" "$(CGB_BOOT)" ui agb
	$(HARNESS) "$(CGB_ROM)" "$(CGB_SYMBOLS)" "$(SAVE)" "$(CGB_BOOT)" transitions agb
