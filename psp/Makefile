# PSP build for vector06sdl - Vector-06C emulator
# Builds EBOOT.PBP, copies result into boot/

TARGET = v06xpsp
OBJS = src/main.o src/debuglog.o src/config.o src/filelist.o src/imgload.o \
	src/stb_impl.o \
	src/i8080.o src/memory.o src/board.o src/filler.o \
	src/emulator.o src/options.o src/tv.o src/sound.o src/sound_filters.o \
	src/debug.o src/fsimage.o src/resampler.o src/cadence.o \
	src/util.o src/utils_string.o src/hal.o src/vkbd.o src/statefile.o \
	src/keymap.o \
	src/popups/mainmenu.o src/popups/popup.o src/popups/rombrowser.o \
	src/popups/configwindow.o src/popups/statewindow.o \
	src/popups/mapwindow.o \
	boots.o

DEBUG_ENABLED ?= 1

PSPSDK = $(shell psp-config --pspsdk-path)

OPTLEVEL = -O3
ifeq ($(PROFILE),1)
# -pg proved unstable on this toolchain: combined with -O3 the EBOOT
# crashed right after startup; even at -O2 a profile build misbehaved
# (machine stuck in the boot ROM) once the pixel helpers got inlined.
# Profiles therefore stay at -O2 and must not be used to judge behavior.
OPTLEVEL = -O2
else ifeq ($(DEBUG_ENABLED),1)
OPTLEVEL = -O2
endif

CFLAGS = $(OPTLEVEL) -G0 -Wall -Wno-unused-variable -DDEBUG_ENABLED=$(DEBUG_ENABLED) -DPSP_PORT $(EXTRA_DEFS)
CXXFLAGS = $(CFLAGS) -std=gnu++17 -I./src -I./src/popups -I.. -I../fast-filters/sources -I../coreutil/sources -I../boost

# Add profiler
ifeq ($(PROFILE),1)
CFLAGS += -DPROFILE
CFLAGS += -pg -g
CXXFLAGS += -pg -g
LDFLAGS += -pg -g
endif

LIBS = -lpspdebug -lpspctrl -lpspdisplay -lpsprtc -lpsppower \
	-lstdc++ -lpspgu -lpspgum -lpspaudiolib -lpspaudio \
	-latomic # 64-bit atomics in Soundnik's ring (wr_total)
ifeq ($(PROFILE),1)
LIBS += -lpspprof
endif

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Vector-06c PSP
PSP_EBOOT_ICON = assets/ICON1.PNG
PSP_EBOOT_PIC1 = assets/PIC1.PNG
PSP_EBOOT_SND0 = assets/LOGO.AT3

EXTRA_CLEAN = boot/EBOOT.PBP gmon.out profile.txt

include $(PSPSDK)/lib/build.mak

# Generate boots_bin from boot/boots.bin using xxd -i
boots.c: ../boot/boots.bin
	xxd -i $< > $@
	sed -i 's/___boot_boots_bin_len/boots_bin_len/g; s/___boot_boots_bin/boots_bin/g' $@

boots.o: boots.c
	$(CC) $(CFLAGS) -c $< -o $@

# Правильный Linux-синтаксис для создания папки и копирования
all: $(TARGET).elf $(EXTRA_TARGETS)
	@mkdir -p boot
	@cp -f EBOOT.PBP boot/EBOOT.PBP
	@echo "EBOOT.PBP successfully copied to boot/"

.PHONY: deploy deploy-debug deploy-release deploy-autoselect deploy-profile
.PHONY: _do_deploy

## deploy: release build + deploy (default)
deploy: deploy-release

## deploy-debug: debug build (DEBUG_ENABLED=1, -O2) + deploy
deploy-debug:
	@$(MAKE) DEBUG_ENABLED=1
	@$(MAKE) DEPLOY_TYPE=deploy-debug _do_deploy

## deploy-release: release build (DEBUG_ENABLED=0, -O3) + deploy
deploy-release:
	@$(MAKE) DEBUG_ENABLED=0
	@$(MAKE) DEPLOY_TYPE=deploy-release _do_deploy

## deploy-autoselect: release + AUTOSELECT_ROM test hook + deploy
deploy-autoselect:
	@$(MAKE) DEBUG_ENABLED=0 EXTRA_DEFS=-DAUTOSELECT_ROM
	@$(MAKE) DEPLOY_TYPE=deploy-autoselect _do_deploy

## deploy-profile: profiler build (PROFILE=1, DEBUG_ENABLED=0, -O2, -pg) + deploy
deploy-profile:
	@$(MAKE) PROFILE=1 DEBUG_ENABLED=0
	@$(MAKE) DEPLOY_TYPE=deploy-profile _do_deploy


# PPSSPP deployment: copy EBOOT.PBP to the emulator's game
# directory.  Skipped silently when PPSSPP_DIR does not exist on
# this machine or boot/EBOOT.PBP was not built.
PPSSPP_DIR = $(HOME)/snap/ppsspp-emu/common/.config/ppsspp/PSP/GAME/VECTOR06C

## _do_deploy: conditional copy to PPSSPP_DIR (shared by all deploy-* targets)
_do_deploy:
	@if [ -d "$(PPSSPP_DIR)" ] && [ -f boot/EBOOT.PBP ]; then \
		cp -f boot/EBOOT.PBP $(PPSSPP_DIR)/EBOOT.PBP && \
		echo "Deployed $(DEPLOY_TYPE) to $(PPSSPP_DIR)"; \
	fi
