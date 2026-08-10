# PSP build for vector06sdl - Vector-06C emulator
# Builds EBOOT.PBP, copies result into boot/

TARGET = v06xpsp
OBJS = src/main.o src/debuglog.o filebrowser.o \
	src/i8080.o src/memory.o src/board.o src/filler.o \
	src/emulator.o src/options.o src/tv.o src/sound.o \
	src/debug.o src/fsimage.o src/resampler.o src/cadence.o \
	src/util.o src/utils_string.o src/hal.o \
	boots.o

DEBUG_ENABLED ?= 1

PSPSDK = $(shell psp-config --pspsdk-path)
CFLAGS = -O2 -G0 -Wall -Wno-unused-variable -DDEBUG_ENABLED=$(DEBUG_ENABLED)
CXXFLAGS = $(CFLAGS) -std=gnu++17 -I./src -I.. -I../fast-filters/sources -I../coreutil/sources -I../boost

LIBS = -lpspdebug -lpspctrl -lpspdisplay -lpsprtc -lpsppower \
	-lstdc++ -lpspgu -lpspgum -lpspaudiolib -lpspaudio

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Vector-06c PSP
PSP_EBOOT_ICON = assets/ICON0.PNG
PSP_EBOOT_PIC1 = assets/PIC1.PNG

EXTRA_CLEAN = boot/EBOOT.PBP

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
