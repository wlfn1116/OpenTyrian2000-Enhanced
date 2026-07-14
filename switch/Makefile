#---------------------------------------------------------------------------------
# OpenTyrian 2000 (widescreen fork) -- Nintendo Switch homebrew build
#
# Produces opentyrian2000.nro for the Homebrew Launcher (Atmosphere/CFW) or an
# emulator. Based on the standard devkitPro libnx application template.
#
# Requires devkitPro with: switch-dev  switch-sdl2   (dkp-pacman -S ...)
# Build from this directory:  make
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# Metadata baked into the .nro (shown in the Homebrew Launcher)
#---------------------------------------------------------------------------------
APP_TITLE   := OpenTyrian 2000
APP_AUTHOR  := OpenTyrian Team (widescreen fork)
APP_VERSION := 2.1

#---------------------------------------------------------------------------------
# TARGET   is the output filename (opentyrian2000.nro / .elf)
# BUILD    is the intermediate object directory
# SOURCES  are the directories scanned for .c/.cpp/.s
# INCLUDES are the directories scanned for .h
# ROMFS    is packed read-only into the .nro and mounted at romfs:/
#---------------------------------------------------------------------------------
TARGET   := opentyrian2000
BUILD    := build
SOURCES  := ../src
INCLUDES := ../src
ROMFS    := romfs

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# SDL2 build flags come from the switch-portlibs pkg-config wrapper. Resolve it by
# explicit path so it works whether or not portlibs/bin is on PATH. Switch links
# statically, so --static is required to pull in EGL/GLESv2/glapi/drm_nouveau/nx.
# (If your devkitPro lacks this wrapper, see switch/README.md for the literal flags.)
PKGCONF := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config

# __SWITCH__ is defined automatically by devkitA64; -fsigned-char keeps the
# behaviour the game assumes on x86 (ARM chars are unsigned by default).
#
# -DNDEBUG: this is a RELEASE build (it's what players run), so compile out assert()
# exactly like the desktop MSVC Release config does. Without it, asserts are LIVE, and
# several are latent tripwires the engine relies on NDEBUG to elide -- e.g. blit_sprite()
# asserts on an out-of-range sprite index that the endless shop legitimately produces
# (draw_ship_illustration with a ship whose big graphic isn't in the illustrated set),
# and JE_loadCompShapesB asserts data==NULL. On desktop Release those are silent no-ops
# (the code then returns/skips gracefully); on Switch a live assert -> abort() -> _exit()
# closes the game with NO Atmosphere crash report (looks like a silent crash). Keep -g so
# the ELF still symbolises real faults for crash-report decoding (see switch/README / memory).
CFLAGS := -g -Wall -O2 -DNDEBUG -ffunction-sections -fsigned-char $(ARCH)

# TYRIAN_DIR is a compile-time fallback data path the engine expects to be defined
# (desktop builds pass it too). On Switch the sdmc:/ and romfs: paths prepended in
# file.c take priority, so this is only a last-resort placeholder.
CFLAGS += $(INCLUDE) -D__SWITCH__ -DTYRIAN_DIR='"."' $(shell $(PKGCONF) sdl2 --cflags)

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# SDL2 and its full (static) dependency chain, then libnx/libm as a safe backstop.
LIBS := $(shell $(PKGCONF) sdl2 --libs --static) -lnx -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib
#---------------------------------------------------------------------------------
LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)

export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
	export LD := $(CC)
else
	export LD := $(CXX)
endif

export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES     := $(OFILES_SRC)
export HFILES     := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.h))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(CONFIG_JSON)),)
	jsons := $(wildcard *.json)
	ifneq (,$(findstring $(TARGET).json,$(jsons)))
		export APP_JSON := $(TOPDIR)/$(TARGET).json
	else
		ifneq (,$(findstring config.json,$(jsons)))
			export APP_JSON := $(TOPDIR)/config.json
		endif
	endif
else
	export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)
endif

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.jpg)
	ifneq (,$(findstring $(TARGET).jpg,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).jpg
	else
		ifneq (,$(findstring icon.jpg,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.jpg
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
	export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
	export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

ifneq ($(ROMFS),)
	export NROFLAGS += --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all: $(OUTPUT).nro

# switch_rules provides the recipes: %.nro (runs elf2nro ... $(NROFLAGS)) and %.nacp.
# NROFLAGS are command-line args to elf2nro, NOT prerequisites -- the .nro just needs
# the linked .elf plus the generated .nacp metadata.
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
