# 3DS specific build steps: the vertex shader and the .3dsx/.smdh/.cia packaging.

ifeq ($(TARGET),3ds)

TOOLS_BIN   := $(DEVKITPRO)/tools/bin
# The order is the DVLE order gfx_c3d.c expects: 0 pass-through, 1 transform, 2-3 sprite GS, 4 sprite table,
# 5 slim track (P65 5b), 6 waves (P65 rank 3).
SHADER_SRC  := 3ds/shader.v.pica 3ds/transform.v.pica 3ds/sprite.v.pica 3ds/sprite.g.pica 3ds/sprite_table.v.pica \
               3ds/track.v.pica 3ds/waves.v.pica
SHADER_BIN  := $(BUILD_DIR)/shader/shader.shbin
SHADER_ASM  := $(BUILD_DIR)/shader/shader_shbin.s
SHADER_HDR  := $(BUILD_DIR)/shader/shader_shbin.h
SHADER_OBJS := $(BUILD_DIR)/shader/shader_shbin.o

APP_TITLE       := Diddy Kong Racing
# The .cia's title. The profile-guided builds (PGO=use, PGO=gen) install as titles of their own next to the usual
# one, so the same test runs can be compared on one console; they share its data folder (ROM, saves, settings).
CIA_UNIQUE_ID   := 0xD4B52
CIA_PRODUCT     := CTR-H-DKRP
ifeq ($(PGO),use)
  APP_TITLE     := DKR PGO
  CIA_UNIQUE_ID := 0xD4B53
  CIA_PRODUCT   := CTR-H-DKRG
else ifeq ($(PGO),gen)
  APP_TITLE     := DKR PGO training
  CIA_UNIQUE_ID := 0xD4B54
  CIA_PRODUCT   := CTR-H-DKRT
endif
APP_DESCRIPTION := Native port of the DKR decompilation, version $(PORT_VERSION)
APP_AUTHOR      := DKR decomp contributors
APP_ICON        := $(firstword $(wildcard 3ds/icon.png) $(CTRULIB)/default_icon.png)

# The installable .cia (roadmap O7): `make -C port TARGET=3ds cia`. makerom and bannertool are not part of
# devkitPro; CI downloads them, or point these at your own copies.
MAKEROM         ?= makerom
BANNERTOOL      ?= bannertool
CIA_DIR         := 3ds/cia

SYS_CFLAGS += -I$(BUILD_DIR)/shader

$(SHADER_BIN): $(SHADER_SRC)
	@mkdir -p $(dir $@)
	@echo "PICASSO $^"
	@$(TOOLS_BIN)/picasso -o $@ $^

# One rule makes both files: as "asm hdr: bin" a parallel make ran bin2s twice at once, and the assembler could read a
# half-written .s and leave an empty object (an undefined shader_shbin at link).
$(SHADER_ASM): $(SHADER_BIN)
	@cd $(dir $<) && $(TOOLS_BIN)/bin2s -a 4 -H shader_shbin.h shader.shbin > shader_shbin.s.tmp && \
		mv shader_shbin.s.tmp shader_shbin.s

$(SHADER_HDR): $(SHADER_ASM)

$(SHADER_OBJS): $(SHADER_ASM)
	@$(CC) $(ARCH) -x assembler-with-cpp -c $< -o $@

# The GS hang ladder's own blob (P65 rank 4, DKR_GS_LADDER, gfx_c3d.c gs_ladder): kept apart from shader.shbin so
# that the game's shader code (and its upload) stays the same; only the ladder binds it.
GSL_SRC := 3ds/gsl_n.v.pica 3ds/gsl_p.v.pica 3ds/gsl_p.g.pica 3ds/gsl_t.v.pica 3ds/gsl_t.g.pica \
           3ds/sprite.v.pica 3ds/sprite.g.pica
GSL_BIN := $(BUILD_DIR)/shader/gsl.shbin
GSL_ASM := $(BUILD_DIR)/shader/gsl_shbin.s
GSL_HDR := $(BUILD_DIR)/shader/gsl_shbin.h
GSL_OBJ := $(BUILD_DIR)/shader/gsl_shbin.o

$(GSL_BIN): $(GSL_SRC)
	@mkdir -p $(dir $@)
	@echo "PICASSO $^"
	@$(TOOLS_BIN)/picasso -o $@ $^

$(GSL_ASM): $(GSL_BIN)
	@cd $(dir $<) && $(TOOLS_BIN)/bin2s -a 4 -H gsl_shbin.h gsl.shbin > gsl_shbin.s.tmp && \
		mv gsl_shbin.s.tmp gsl_shbin.s

$(GSL_HDR): $(GSL_ASM)

$(GSL_OBJ): $(GSL_ASM)
	@$(CC) $(ARCH) -x assembler-with-cpp -c $< -o $@

# The renderer includes the generated headers.
$(call obj,3ds/gfx_c3d.c): $(SHADER_HDR) $(GSL_HDR)

$(EXE): $(SHADER_OBJS) $(GSL_OBJ)

# Rewritten only when the version changes, so the files that show it are rebuilt then (an incremental build would
# otherwise keep the old one).
VERSION_STAMP := $(BUILD_DIR)/version.txt
$(shell mkdir -p $(BUILD_DIR) && echo '$(PORT_VERSION) $(PORT_BUILD_VERSION)' | cmp -s - $(VERSION_STAMP) || \
        echo '$(PORT_VERSION) $(PORT_BUILD_VERSION)' > $(VERSION_STAMP))
$(foreach f,src/plat/plat_common.c 3ds/bugreport_3ds.c 3ds/update_3ds.c,$(call obj,$(f))): $(VERSION_STAMP)

$(BUILD_DIR)/dkr.smdh: $(APP_ICON) $(VERSION_STAMP)
	@$(TOOLS_BIN)/smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $@

$(BUILD_DIR)/dkr.3dsx: $(EXE) $(BUILD_DIR)/dkr.smdh
	@echo "3DSX $@"
	@$(TOOLS_BIN)/3dsxtool $(EXE) $@ --smdh=$(BUILD_DIR)/dkr.smdh

# The HOME menu banner: a flat 256x128 picture and a short sound, both original art (port/3ds/cia/README.md).
$(BUILD_DIR)/dkr.bnr: $(CIA_DIR)/banner.png $(CIA_DIR)/banner.wav
	@$(BANNERTOOL) makebanner -i $(CIA_DIR)/banner.png -a $(CIA_DIR)/banner.wav -o $@

$(BUILD_DIR)/dkr.cia: $(EXE) $(BUILD_DIR)/dkr.smdh $(BUILD_DIR)/dkr.bnr $(CIA_DIR)/dkr.rsf
	@echo "CIA $@"
	@$(MAKEROM) -f cia -o $@ -elf $(EXE) -rsf $(CIA_DIR)/dkr.rsf -icon $(BUILD_DIR)/dkr.smdh \
		-banner $(BUILD_DIR)/dkr.bnr -exefslogo -target t \
		-DAPP_TITLE="$(APP_TITLE)" -DAPP_PRODUCT_CODE=$(CIA_PRODUCT) -DAPP_UNIQUE_ID=$(CIA_UNIQUE_ID) \
		-major $(PORT_VERSION_MAJOR) -minor $(PORT_VERSION_MINOR) -micro $(PORT_VERSION_MICRO)

.PHONY: cia
cia: $(BUILD_DIR)/dkr.cia

endif
