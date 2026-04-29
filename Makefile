#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

GAME_TITLE     := maxtracker
GAME_SUBTITLE1 := nds tracker
GAME_SUBTITLE2 := maxmod native

export TARGET	:=	$(shell basename $(GAME_TITLE))
export TOPDIR	:=	$(CURDIR)

# NitroFS data directory — ds_rules generates _ADDFILES from this
NITRO_FILES	:= ./data

include $(DEVKITARM)/ds_rules

MAXMOD  := lib
MAXMOD_SRC := $(MAXMOD)/maxmod

# LFE (Lightweight Fixed-point Engine) — offline sample synth & FX engine.
# Set LFE_ENABLED=0 to build without the waveform editor; the *-nosynth
# targets below set this automatically.
LFE_ENABLED ?= 1
LFE_DIR     := lib/lfe

.PHONY: maxmod_ds7 maxmod_ds9 build_arm7 build_arm9 \
        lfe_ds9 lfe-test \
        emulator emulator-nosynth native native-nosynth \
        clean ensure_release ensure_maxmod help

# Default: show help
all: help

help:
	@echo ""
	@echo "  maxtracker build targets"
	@echo "  ------------------------"
	@echo "  make emulator          - build .nds with data/ embedded via NitroFS (with waveform editor)"
	@echo "  make emulator-nosynth  - same as emulator, but without the waveform editor"
	@echo "  make native            - build .nds without embedded data (with waveform editor)"
	@echo "  make native-nosynth    - same as native, but without the waveform editor"
	@echo "  make lfe-test          - compile and run lfe library tests natively on host"
	@echo "  make clean             - remove all build artifacts"
	@echo ""

#---------------------------------------------------------------------------------
ensure_release:
	@mkdir -p release

#---------------------------------------------------------------------------------
ensure_maxmod:
	@if [ ! -d $(MAXMOD_SRC)/source ]; then \
		echo "maxmod not found — please git clone maxmod into lib/maxmod"; \
		exit 1; \
	fi

#---------------------------------------------------------------------------------
maxmod_ds7: ensure_maxmod
	@$(MAKE) -C $(MAXMOD) -f maxmod.mak SYSTEM=DS7

maxmod_ds9: ensure_maxmod
	@$(MAKE) -C $(MAXMOD) -f maxmod.mak SYSTEM=DS9

#---------------------------------------------------------------------------------
# LFE — built only when LFE_ENABLED=1
lfe_ds9:
ifeq ($(LFE_ENABLED),1)
	@$(MAKE) -C $(LFE_DIR) -f Makefile_arm
endif

#---------------------------------------------------------------------------------
build_arm7: maxmod_ds7
	@$(MAKE) -C arm7

build_arm9: maxmod_ds9 lfe_ds9
	@$(MAKE) -C arm9 LFE_ENABLED=$(LFE_ENABLED)

#---------------------------------------------------------------------------------
# Emulator build: embed data/ in NitroFS inside the .nds
# Use for no$gba, DeSmuME, melonDS, etc.
#---------------------------------------------------------------------------------
emulator: ensure_release build_arm7 build_arm9
	@mkdir -p data
	ndstool	-c release/$(TARGET).nds \
		-7 arm7/$(TARGET).elf \
		-9 arm9/$(TARGET).elf \
		-b $(GAME_ICON) "$(GAME_TITLE);$(GAME_SUBTITLE1);$(GAME_SUBTITLE2)" \
		$(_ADDFILES)
	@echo "Built (emulator): release/$(TARGET).nds"

#---------------------------------------------------------------------------------
# Native build: no embedded data. Put songs on SD at /maxtracker/
# Use for R4, DSTT, Acekard, etc.
#---------------------------------------------------------------------------------
native: ensure_release build_arm7 build_arm9
	ndstool	-c release/$(TARGET).nds \
		-7 arm7/$(TARGET).elf \
		-9 arm9/$(TARGET).elf \
		-b $(GAME_ICON) "$(GAME_TITLE);$(GAME_SUBTITLE1);$(GAME_SUBTITLE2)"
	@echo "Built (native): release/$(TARGET).nds"

#---------------------------------------------------------------------------------
# *-nosynth variants: LFE excluded. Smaller binary, no waveform editor.
#---------------------------------------------------------------------------------
emulator-nosynth:
	@$(MAKE) emulator LFE_ENABLED=0

native-nosynth:
	@$(MAKE) native LFE_ENABLED=0

#---------------------------------------------------------------------------------
# LFE host tests — independent of maxtracker.
#---------------------------------------------------------------------------------
lfe-test:
	@$(MAKE) -C $(LFE_DIR) test

#---------------------------------------------------------------------------------
clean:
	$(MAKE) -C $(MAXMOD) -f maxmod.mak SYSTEM=DS7 clean
	$(MAKE) -C $(MAXMOD) -f maxmod.mak SYSTEM=DS9 clean
	$(MAKE) -C arm9 clean
	$(MAKE) -C arm7 clean
	$(MAKE) -C $(LFE_DIR) clean
	@if [ -f $(LFE_DIR)/Makefile_arm ]; then $(MAKE) -C $(LFE_DIR) -f Makefile_arm clean; fi
	rm -rf $(MAXMOD)/build $(MAXMOD)/maxmod_build_ds7 $(MAXMOD)/maxmod_build_ds9
	rm -f release/$(TARGET).nds $(TARGET).arm7 $(TARGET).arm9
