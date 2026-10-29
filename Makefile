#---------------------------------------------------------------------------------
# cr_agent.3dsx: the 3DS side of tools/hw_loop.py (see source/main.c). Build:
#   docker run --rm -v "<repo>:/proj" -w /proj/tools/hw/agent mkw3ds-build:2026-10-09 make
#---------------------------------------------------------------------------------
.SUFFIXES:
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif
TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET		:=	cr_agent
BUILD		:=	build
SOURCES		:=	source
INCLUDES	:=

APP_TITLE	:=	cr_agent
APP_DESCRIPTION	:=	custom-robo-recomp hardware run agent
APP_AUTHOR	:=	custom-robo-recomp

ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS	:=	-g -Wall -O2 -Wno-format-truncation -mword-relocations -ffunction-sections $(ARCH) $(INCLUDE) -D__3DS__
ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)
LIBS	:= -lctru -lm
LIBDIRS	:= $(CTRULIB)

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export LD	:=	$(CC)
export OFILES	:=	$(CFILES:.c=.o)
export INCLUDE	:=	$(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
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
-include $(DEPSDIR)/*.d
endif
