# The backend replay (roadmap P56 package D2): port/3ds/gfx_c3d.c and the real citro3d, built for the PC (32-bit, like
# the host build) to play record files of the host build's backend calls. port/tools/backend_replay.py fetches the
# sources and runs this; by hand:
#
#   make -C port/host/c3d_replay DEPS=/path/to/deps OUT=/tmp/replay-head [PORT=/other/checkout/port]
#
# DEPS holds checkouts of libctru, citro3d and picasso at the versions the 3DS build's devkitARM image ships
# (backend_replay.py's DEPS table) and a picasso built from its source (DEPS/picasso/picasso). PORT is the port
# directory whose backend (3ds/gfx_c3d.c, gpu_cuts.inc, the shaders and src/ headers) is replayed: another commit's
# checkout to compare two commits. The harness itself (this directory) always comes from this checkout.

DEPS ?= $(error set DEPS to the directory with libctru, citro3d and picasso)
OUT  ?= build
HERE := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
PORT ?= $(abspath $(HERE)/../..)

CTRU    := $(DEPS)/libctru/libctru
C3D     := $(DEPS)/citro3d
PICASSO := $(DEPS)/picasso/picasso

CC := gcc
# The host build's float settings, and the port's: unsigned char as on ARM, no strict aliasing, wrapping integers.
ARCH   := -m32 -msse2 -mfpmath=sse
CFLAGS := $(ARCH) -std=gnu11 -O2 -g -fno-math-errno -funsigned-char -fno-strict-aliasing -fwrapv -fno-common \
          -D__3DS__ -DARM11 -I$(HERE)shim -I$(CTRU)/include -I$(C3D)/include -w
BACKEND_CFLAGS := $(CFLAGS) -DPORT_NATIVE_OPS=1 -I$(PORT)/include -I$(PORT)/src -I$(OUT)/shader \
                  -DPORT_VERSION=\"replay\" -DPORT_BUILD_VERSION=\"replay\" -DPORT_BUILD_COMMIT=\"replay\"
HARNESS_CFLAGS := $(CFLAGS) -I$(PORT)/src -I$(HERE)

C3D_SRC  := $(wildcard $(C3D)/source/*.c) $(wildcard $(C3D)/source/maths/*.c)
CTRU_SRC := $(CTRU)/source/gpu/gpu.c $(CTRU)/source/gpu/shaderProgram.c $(CTRU)/source/gpu/shbin.c

C3D_OBJS  := $(patsubst $(C3D)/source/%.c,$(OUT)/citro3d/%.o,$(C3D_SRC))
CTRU_OBJS := $(patsubst $(CTRU)/source/gpu/%.c,$(OUT)/ctru/%.o,$(CTRU_SRC))
HARNESS_OBJS := $(OUT)/harness/replay.o $(OUT)/harness/stubs_ctru.o $(OUT)/harness/stubs_plat.o
BACKEND_OBJS := $(OUT)/backend/gfx_c3d.o $(OUT)/shader/shader_shbin.o

# As the 3DS build links (port/Makefile): the backend's own wrap of citro3d's settings writer (gpu_cuts.inc) and of
# libctru's screen calls; the GPU queue's run is a stub here.
WRAPS := C3Di_UpdateContext gxCmdQueueRun gfxGetFramebuffer gfxScreenSwapBuffers
LDFLAGS := $(ARCH) $(foreach f,$(WRAPS),-Wl,--wrap=$(f))

.PHONY: all
all: $(OUT)/c3d_replay

$(OUT)/c3d_replay: $(HARNESS_OBJS) $(BACKEND_OBJS) $(C3D_OBJS) $(CTRU_OBJS)
	$(CC) $(LDFLAGS) $^ -lm -o $@

$(OUT)/citro3d/%.o: $(C3D)/source/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/ctru/%.o: $(CTRU)/source/gpu/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/harness/%.o: $(HERE)%.c $(HERE)replay.h $(HERE)record_format.h $(PORT)/src/gfx/gfx_backend.h
	@mkdir -p $(dir $@)
	$(CC) $(HARNESS_CFLAGS) -c $< -o $@

$(OUT)/backend/gfx_c3d.o: $(PORT)/3ds/gfx_c3d.c $(wildcard $(PORT)/3ds/*.inc) $(OUT)/shader/shader_shbin.h
	@mkdir -p $(dir $@)
	$(CC) $(BACKEND_CFLAGS) -c $< -o $@

# The vertex shaders, assembled as the 3DS build does (port/3ds/rules.mk), into a C array instead of bin2s's assembly.
$(OUT)/shader/shader.shbin: $(PORT)/3ds/shader.v.pica $(PORT)/3ds/transform.v.pica $(PORT)/3ds/sprite.v.pica \
                           $(PORT)/3ds/sprite.g.pica
	@mkdir -p $(dir $@)
	$(PICASSO) -o $@ $^

$(OUT)/shader/shader_shbin.c $(OUT)/shader/shader_shbin.h: $(OUT)/shader/shader.shbin
	python3 $(HERE)bin2c.py $< $(OUT)/shader/shader_shbin

$(OUT)/shader/shader_shbin.o: $(OUT)/shader/shader_shbin.c
	$(CC) $(ARCH) -c $< -o $@
