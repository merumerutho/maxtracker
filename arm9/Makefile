#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

# Capture this Makefile's own location BEFORE any `include` directives
# pollute MAKEFILE_LIST.
LFE_THIS_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

include $(DEVKITARM)/ds_rules

MAXMOD      := $(CURDIR)/../lib

BUILD		:=	build
SOURCES		:=	source/core source/ui source/ui/lfe source/ui/lfe/gen source/ui/lfe/fx source/io source/test
INCLUDES	:=	source/core source/ui source/ui/lfe source/ui/lfe/gen source/ui/lfe/fx source/io source/test ../include
DATA		:=

# ----- LFE integration -----
# LFE_ENABLED is passed in by the top-level Makefile. When set to 1 we
# add the lib/lfe include path, define MAXTRACKER_LFE, and link liblfe.a.
LFE_ENABLED ?= 0
LFE_DIR     := $(abspath $(dir $(LFE_THIS_MAKEFILE))../lib/lfe)

ifeq ($(LFE_ENABLED),1)
  LFE_CFLAGS  := -DMAXTRACKER_LFE -I$(LFE_DIR)/include
  LFE_LDFLAGS := -L$(LFE_DIR)/build_nds -llfe
else
  LFE_CFLAGS  :=
  LFE_LDFLAGS :=
endif

ARCH	:=	-mthumb -mthumb-interwork

CFLAGS	:=	-g -Wall -O2\
 			-march=armv5te -mtune=arm946e-s -fomit-frame-pointer\
			-ffast-math \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -DARM9 $(EXTRA_CFLAGS) $(LFE_CFLAGS)
CXXFLAGS	:=	$(CFLAGS) -fno-rtti -fno-exceptions

ASFLAGS	:=	-g $(ARCH) -march=armv5te -mtune=arm946e-s

LDFLAGS	=	-specs=ds_arm9.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:=	$(LFE_LDFLAGS) -lfilesystem -lfat -lnds9 -lmm9 -lm

LIBDIRS	:=	$(MAXMOD) $(LIBNDS)
MAXMOD_INC	:=	$(MAXMOD)/maxmod/include

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export ARM9ELF	:=	$(CURDIR)/$(TARGET).elf
export DEPSDIR := $(CURDIR)/$(BUILD)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
	export LD	:=	$(CC)
else
	export LD	:=	$(CXX)
endif

export OFILES	:=	$(addsuffix .o,$(BINFILES)) \
					$(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			-I$(MAXMOD_INC) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	-L$(MAXMOD)/build $(foreach dir,$(filter-out $(MAXMOD),$(LIBDIRS)),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean arm9 ...
	@rm -fr $(BUILD) *.elf *.nds* *.bin

#---------------------------------------------------------------------------------
else

$(ARM9ELF)	:	$(OFILES)
	@echo linking $(notdir $@)
	@$(LD)  $(LDFLAGS) $(OFILES) $(LIBPATHS) $(LIBS) -o $@

%.bin.o	:	%.bin
	@echo $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

endif
