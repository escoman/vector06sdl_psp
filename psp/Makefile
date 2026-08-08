# PSP build for vector06sdl - ROM Browser stage
# Builds EBOOT.PBP, copies result into boot/

TARGET = v06xpsp
OBJS = main.o filebrowser.o

PSPSDK = $(shell psp-config --pspsdk-path)
CFLAGS = -O2 -G0 -Wall -Wno-unused-variable
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
LIBS = -lpspdebug -lpspctrl -lpspdisplay -lpsprtc -lpsppower -lstdc++

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = Vector-06c PSP
PSP_EBOOT_ICON = assets/ICON0.PNG
PSP_EBOOT_PIC1 = assets/PIC1.PNG

EXTRA_CLEAN = boot/EBOOT.PBP

include $(PSPSDK)/lib/build.mak

# Правильный Linux-синтаксис для создания папки и копирования
all: $(TARGET).elf $(EXTRA_TARGETS)
	@mkdir -p boot
	@cp -f EBOOT.PBP boot/EBOOT.PBP
	@echo "EBOOT.PBP successfully copied to boot/"
