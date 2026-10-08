# Picashot build. Needs a C compiler, SDL3 and glslc (from shaderc). Nothing here is tied to one CPU
# architecture: the same Makefile builds on x86_64 and aarch64.
#   make            build ./picashot
#   make DEV=1 shaders   rebuild only the shaders (a running `make dev` reloads them)
#   make dev        build the developer binary (shader hot reload, scripted input) and run it
#   make install    install under PREFIX (default /usr/local); DESTDIR is honoured
CC      ?= cc
GLSLC   ?= glslc
PREFIX  ?= /usr/local
VERSION := $(shell cat VERSION)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c17 -Wall -Wextra -Wshadow -Wno-unused-parameter -Wno-missing-field-initializers
CPPFLAGS += -Ithird_party/vulkan -DPICASHOT_VERSION='"$(VERSION)"' $(shell pkg-config --cflags sdl3)
LDLIBS  += $(shell pkg-config --libs sdl3) -lm

# libjpeg-turbo is optional but strongly recommended: with it Picashot decodes the camera's MJPEG itself,
# straight to YUV. Without it, cameras that only offer MJPEG at the wanted size go through SDL's slower path.
ifeq ($(shell pkg-config --exists libturbojpeg && echo yes),yes)
CPPFLAGS += -DHAVE_TURBOJPEG $(shell pkg-config --cflags libturbojpeg)
LDLIBS  += $(shell pkg-config --libs libturbojpeg)
endif

# A developer build (make DEV=1, or make dev) goes in its own directory and gets its own name, so it
# never mixes with release objects. Only it has shader hot reload and scripted input.
ifdef DEV
BUILD   := build/dev
BIN     := picashot-dev
CPPFLAGS += -DPICASHOT_DEV
else
BUILD   := build
BIN     := picashot
endif
CPPFLAGS += -I$(BUILD)/spv

SRC     := src/main.c src/gpu.c src/capture.c src/filters.c src/profile.c src/camera.c src/camera_v4l2.c src/camera_sdl.c
OBJ     := $(SRC:src/%.c=$(BUILD)/%.o)
# Shaders that are part of the renderer and not filters.
INTERNAL := present yuv2rgb video_y video_uv
FILTERS := $(filter-out $(INTERNAL),$(basename $(notdir $(wildcard shaders/*.frag))))
SHADERS := $(notdir $(wildcard shaders/*.vert shaders/*.frag))
SPV     := $(SHADERS:%=$(BUILD)/spv/%.spv)
INC     := $(SHADERS:%=$(BUILD)/spv/%.inc)

.PHONY: all shaders dev install uninstall clean
all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) VERSION | $(BUILD)/spv
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/filters.o: $(INC) $(BUILD)/spv/filters_spv.inc

# Every shader is compiled twice: a .spv file for hot reload and a C array that is built into the binary.
$(BUILD)/spv/%.spv: shaders/% shaders/common.glsl | $(BUILD)/spv
	$(GLSLC) -O -Ishaders $< -o $@
$(BUILD)/spv/%.inc: shaders/% shaders/common.glsl | $(BUILD)/spv
	$(GLSLC) -O -Ishaders -mfmt=c $< -o $@

$(BUILD)/spv/filters_spv.inc: $(wildcard shaders/*.frag) | $(BUILD)/spv
	@for f in $(FILTERS); do printf 'static const uint32_t spv_%s[] =\n#include "%s.frag.inc"\n;\n' $$f $$f; done > $@

$(BUILD)/spv:
	@mkdir -p $@

shaders: $(SPV)

dev:
	$(MAKE) DEV=1 picashot-dev shaders
	PICASHOT_SHADER_DIR=build/dev/spv PICASHOT_DEBUG=1 ./picashot-dev

install: picashot
	install -Dm755 picashot $(DESTDIR)$(PREFIX)/bin/picashot
	install -Dm644 data/picashot.desktop $(DESTDIR)$(PREFIX)/share/applications/picashot.desktop
	install -Dm644 data/picashot.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/picashot.svg
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/licenses/picashot/LICENSE

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/picashot $(DESTDIR)$(PREFIX)/share/applications/picashot.desktop \
	      $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/picashot.svg
	rm -rf $(DESTDIR)$(PREFIX)/share/licenses/picashot

clean:
	rm -rf build picashot picashot-dev
