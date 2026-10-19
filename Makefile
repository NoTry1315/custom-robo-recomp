#---------------------------------------------------------------------------------
# custom-robo-recomp: runtime/ (hand-written SDK layer) + gen/ (translated main.dol)
# gen/ is produced by: PYTHONUTF8=1 python tools/recomp/recomp.py
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET		:=	custom-robo-recomp
BUILD		:=	build
SOURCES		:=	runtime gen
DATA		:=
INCLUDES	:=	runtime gen
ROMFS		:=	romfs

APP_TITLE	:=	Custom Robo Recomp
APP_DESCRIPTION	:=	Static recompilation of Custom Robo
APP_AUTHOR	:=	custom-robo-recomp

ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
# gen/ is goto-heavy and has unused labels/values by construction
CFLAGS	:=	-g -Wall -Wno-unused-label -Wno-unused-but-set-variable -O2 -mword-relocations \
			-ffunction-sections -fno-strict-aliasing $(ARCH)
CFLAGS	+=	$(INCLUDE) -D__3DS__
CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11
ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--wrap=svcBreak
LIBS	:= -lcitro3d -lctru -lm
LIBDIRS	:= $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD	:=	$(CC)
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
export OFILES_SRC	:=	$(CFILES:.c=.o)
export OFILES	:=	$(PICAFILES:.v.pica=.shbin.o) $(OFILES_SRC)
export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export _3DSXDEPS	:=	$(OUTPUT).smdh
export APP_ICON := $(DEVKITPRO)/libctru/default_icon.png
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh

.PHONY: all clean
all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
$(BUILD):
	@mkdir -p $@
clean:
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf
else
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)
$(OUTPUT).elf	:	$(OFILES)
$(OFILES_SRC)	:	$(HFILES)
# vertex shaders: picasso, then embedded with bin2s (symbols <name>_shbin, _shbin_size)
%.shbin.o %_shbin.h : %.v.pica
	@echo $(notdir $<)
	@picasso -o $*.shbin $<
	@bin2s -a 4 -H $*_shbin.h $*.shbin | $(AS) -o $*.shbin.o
-include $(DEPSDIR)/*.d
endif
