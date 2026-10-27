#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# GRAPHICS is a list of directories containing graphics files
# GFXBUILD is the directory where converted graphics files will be placed
#   If set to $(BUILD), it will statically link in the converted
#   files as if they were data files.
#
# NO_SMDH: if set to anything, no SMDH file is generated.
# ROMFS is the directory which contains the RomFS, relative to the Makefile (Optional)
# APP_TITLE is the name of the app stored in the SMDH file (Optional)
# APP_DESCRIPTION is the description of the app stored in the SMDH file (Optional)
# APP_AUTHOR is the author of the app stored in the SMDH file (Optional)
# ICON is the filename of the icon (.png), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.png
#     - icon.png
#     - <libctru folder>/default_icon.png
#---------------------------------------------------------------------------------
TARGET		:=	psnap-3ds
BUILD		:=	build
SOURCES		:=	source source/port source/render source/ui \
			runtime/ultramodern runtime/librecomp runtime/thirdparty/o1heap/o1heap \
			recomp/RecompiledFuncs recomp/rsp
DATA		:=	data
INCLUDES	:=	include source/port source/render source/ui runtime/include runtime/include/librecomp \
			runtime/thirdparty runtime/thirdparty/concurrentqueue runtime/thirdparty/o1heap \
			recomp/RecompiledFuncs
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)
ROMFS		:=	romfs
APP_TITLE	:=	Pokemon Snap: Double Take!!
# VERSION (a build number, from the VERSION file) shows in the HOME menu / Azahar entry and the
# dist file name, so builds can be told apart.
VERSION		?=	$(strip $(shell cat $(CURDIR)/VERSION 2>/dev/null))
export VERSION
APP_DESCRIPTION	:=	Pokemon Snap 3DS port v$(VERSION)
APP_AUTHOR	:=	PSNAP3DS
#GFXBUILD	:=	$(ROMFS)/gfx

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# No -g/-Wall: 58 MB of generated C makes debug info huge and warnings useless.
# -funwind-tables: ultramodern ends N64 threads by throwing a C++ exception through recompiled C frames.
# -fno-strict-aliasing: recompiled code type-puns RDRAM freely.
CFLAGS	:=	-O2 -mword-relocations \
			-ffunction-sections -funwind-tables -fno-strict-aliasing \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__ -DSNAP_VERSION=\"$(VERSION)\"

CXXFLAGS	:= $(CFLAGS) -std=gnu++20

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# curl + mbedTLS (devkitPro portlibs 3ds-curl, 3ds-mbedtls): the updater's HTTPS (updater.cpp).
LIBS	:= -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lcitro2d -lcitro3d -lctru -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(CTRULIB)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
ifeq ($(GFXBUILD),$(BUILD))
#---------------------------------------------------------------------------------
export T3XFILES :=  $(GFXFILES:.t3s=.t3x)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean deploy run cia cia-personal makecia release

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@mkdir -p $@

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(GFXBUILD)

#---------------------------------------------------------------------------------
# Local emulators (portable: <emu>/user is the data folder, so the SD card is <emu>/user/sdmc).
# deploy copies the app and ROM to both; run launches Azahar (falls back to Citra).
#---------------------------------------------------------------------------------
EMUS		:=	azahar

deploy: all
	@for emu in $(EMUS); do \
		dir="$(CURDIR)/$$emu/user/sdmc/3ds/psnap"; \
		[ -d "$(CURDIR)/$$emu" ] || continue; \
		mkdir -p "$$dir"; \
		cp $(TARGET).3dsx "$$dir/"; \
		[ -f "$$dir/pokemonsnap.us.z64" ] || cp "$(CURDIR)/rom/pokemonsnap.us.z64" "$$dir/pokemonsnap.us.z64"; \
		echo "deployed to $$emu/user/sdmc/3ds/psnap"; \
	done

#---------------------------------------------------------------------------------
# CIA for the HOME menu: banner from assets/banner (tools/make_banner.py), icon from the SMDH.
# Needs bannertool and makerom in tools/cia/.
#---------------------------------------------------------------------------------
CIATOOLS	:=	$(CURDIR)/tools/cia

# Shared builds (cia) carry no ROM: players put their own dump on the SD card (rom_check.c).
# cia-personal packs rom/pokemonsnap.us.z64 into the app's RomFS as well, for your own console
# only: dist/psnap-3ds-v<VERSION>-personal.cia. Never share that one.
cia: all
	@$(MAKE) --no-print-directory makecia ROMFS_ROOT=romfs CIA_NAME=$(TARGET)-v$(VERSION)

cia-personal: all
	@[ -f rom/pokemonsnap.us.z64 ] || { echo "rom/pokemonsnap.us.z64 is missing"; exit 1; }
	@rm -rf $(BUILD)/romfs_personal && mkdir -p $(BUILD)/romfs_personal/rom
	@cp -r romfs/. $(BUILD)/romfs_personal/
	@cp rom/pokemonsnap.us.z64 $(BUILD)/romfs_personal/rom/pokemonsnap.z64
	@$(MAKE) --no-print-directory makecia ROMFS_ROOT=$(BUILD)/romfs_personal CIA_NAME=$(TARGET)-v$(VERSION)-personal

makecia:
	@$(CIATOOLS)/bannertool.exe makebanner -i assets/banner/banner.png -a assets/banner/banner.wav -o $(BUILD)/banner.bnr
	@$(CIATOOLS)/makerom.exe -f cia -o $(TARGET).cia -elf $(TARGET).elf -rsf assets/cia.rsf -DROMFS_ROOT=$(ROMFS_ROOT) 		-icon $(TARGET).smdh -banner $(BUILD)/banner.bnr -exefslogo -target t -major 0 -minor $(VERSION) -micro 0
	@mkdir -p dist
	@cp $(TARGET).cia dist/$(CIA_NAME).cia
	@echo built dist/$(CIA_NAME).cia

# A numbered copy for testers: dist/psnap-3ds-v<VERSION>.3dsx (the SMDH is rebuilt so the HOME
# menu / Azahar entry shows the same number).
release:
	@rm -f $(TARGET).smdh $(BUILD)/main.o $(BUILD)/crash_log.o $(BUILD)/bottom_ui.o $(BUILD)/updater.o # the files with SNAP_VERSION in them
	@$(MAKE) --no-print-directory deploy
	@mkdir -p dist
	@cp $(TARGET).3dsx dist/$(TARGET)-v$(VERSION).3dsx
	@echo released dist/$(TARGET)-v$(VERSION).3dsx

# Publishes this version on GitHub (xXDankeyKongXx/psnap-3ds), where the in-game updater looks
# (updater.cpp): tag v<VERSION> on the pushed commit, with the shareable CIA and .3dsx as
# psnap-3ds.cia / psnap-3ds.3dsx and NOTES (a file) as the description. Never the -personal CIA.
# gh only finds its login from a Windows shell (Git Bash, PowerShell), not the devkitPro one.
github-release:
	@[ -f dist/$(TARGET)-v$(VERSION).cia ] && [ -f dist/$(TARGET)-v$(VERSION).3dsx ] || { echo "make release cia first"; exit 1; }
	@[ -f "$(NOTES)" ] || { echo "NOTES=<file> needed"; exit 1; }
	@rm -rf $(BUILD)/github && mkdir -p $(BUILD)/github
	@cp dist/$(TARGET)-v$(VERSION).cia $(BUILD)/github/$(TARGET).cia
	@cp dist/$(TARGET)-v$(VERSION).3dsx $(BUILD)/github/$(TARGET).3dsx
	@gh release create v$(VERSION) $(BUILD)/github/$(TARGET).cia $(BUILD)/github/$(TARGET).3dsx 		--repo xXDankeyKongXx/psnap-3ds --title "v$(VERSION)" --notes-file "$(NOTES)" --target main

run: deploy
	@powershell -NoProfile -ExecutionPolicy Bypass -File "$$(cygpath -w "$(CURDIR)/tools/run_emu.ps1")"

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PRECIOUS	:	%.t3x %.shbin
#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
%.shbin.o %_shbin.h : %.shbin
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
