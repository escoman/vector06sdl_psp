#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psptypes.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspaudiolib.h>
#include <pspaudio.h>

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

#include "memory.h"
#include "vio.h"
#include "tv.h"
#include "board.h"
#include "emulator.h"
#include "options.h"
#include "keyboard.h"
#include "8253.h"
#include "sound.h"
#include "ay.h"
#include "wav.h"
#include "util.h"

#include "../filebrowser.h"

PSP_MODULE_INFO("VECTOR06C", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(16 * 1024);

static int exitRequest = 0;
static std::string statusMessage;

static const char ROM_DIR[] = "ms0:/PSP/GAME/VECTOR06C/ROMS";

int exitCallback(int arg1, int arg2, void *common)
{
    exitRequest = 1;
    sceKernelExitGame();
    return 0;
}

int callbackThread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("Exit Callback", exitCallback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

int setupCallbacks(void)
{
    int thid = sceKernelCreateThread("update_thread", callbackThread,
                                     0x11, 0xFA0, 0, 0);
    if (thid >= 0)
    {
        sceKernelStartThread(thid, 0, 0);
    }
    return thid;
}

/* Global emulator objects (like android_main.cpp) */
Memory memory;
Debug debug(&memory);
FD1793 fdc;
Wav wav;
WavPlayer tape_player(wav);
Keyboard keyboard;
I8253 timer;
TimerWrapper tw(timer);
AY ay;
AYWrapper aw(ay);
Soundnik soundnik(tw, aw);
IO io(memory, keyboard, timer, fdc, ay, tape_player);
TV tv;
PixelFiller filler(memory, io, tv);
Board board(memory, io, filler, soundnik, tv, tape_player, debug);
Emulator lator(board);

/* Load a ROM file into memory */
void load_rom_file(const std::string & path)
{
    std::vector<uint8_t> data = util::load_binfile(path);
    if (data.size() > 0) {
        /* Load ROM at 0xC000 (typical for Vector-06C programs) */
        memory.init_from_vector(data, 0xC000);
        /* Set PC to the load address so the program starts executing */
        Options.pc = 0xC000;
        board.reset(Board::ResetMode::LOADROM);
        printf("Loaded ROM: %s (%lu bytes) at 0xC000\n", path.c_str(), data.size());
    } else {
        printf("Failed to load ROM: %s\n", path.c_str());
    }
}

/* Map PSP buttons to Vector-06C keycodes */
void handle_input()
{
    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);

    static uint32_t oldButtons = 0;
    uint32_t buttons = pad.Buttons;
    uint32_t pressed = buttons & ~oldButtons;
    uint32_t released = oldButtons & ~buttons;

    /* D-Pad → arrow keys */
    if (pressed & PSP_CTRL_UP) lator.keydown(SDL_SCANCODE_UP);
    if (released & PSP_CTRL_UP) lator.keyup(SDL_SCANCODE_UP);
    if (pressed & PSP_CTRL_DOWN) lator.keydown(SDL_SCANCODE_DOWN);
    if (released & PSP_CTRL_DOWN) lator.keyup(SDL_SCANCODE_DOWN);
    if (pressed & PSP_CTRL_LEFT) lator.keydown(SDL_SCANCODE_LEFT);
    if (released & PSP_CTRL_LEFT) lator.keyup(SDL_SCANCODE_LEFT);
    if (pressed & PSP_CTRL_RIGHT) lator.keydown(SDL_SCANCODE_RIGHT);
    if (released & PSP_CTRL_RIGHT) lator.keyup(SDL_SCANCODE_RIGHT);

    /* Cross → Enter (ВК) */
    if (pressed & PSP_CTRL_CROSS) lator.keydown(SDL_SCANCODE_RETURN);
    if (released & PSP_CTRL_CROSS) lator.keyup(SDL_SCANCODE_RETURN);

    /* Circle → Backspace (ЗАБ) */
    if (pressed & PSP_CTRL_CIRCLE) lator.keydown(SDL_SCANCODE_BACKSPACE);
    if (released & PSP_CTRL_CIRCLE) lator.keyup(SDL_SCANCODE_BACKSPACE);

    /* Triangle → Space */
    if (pressed & PSP_CTRL_TRIANGLE) lator.keydown(SDL_SCANCODE_SPACE);
    if (released & PSP_CTRL_TRIANGLE) lator.keyup(SDL_SCANCODE_SPACE);

    /* Square → Tab */
    if (pressed & PSP_CTRL_SQUARE) lator.keydown(SDL_SCANCODE_TAB);
    if (released & PSP_CTRL_SQUARE) lator.keyup(SDL_SCANCODE_TAB);

    /* L → RUS/LAT toggle */
    if (pressed & PSP_CTRL_LTRIGGER) lator.keydown(SDL_SCANCODE_F6);
    if (released & PSP_CTRL_LTRIGGER) lator.keyup(SDL_SCANCODE_F6);

    /* R → Shift (SS) */
    if (pressed & PSP_CTRL_RTRIGGER) lator.keydown(SDL_SCANCODE_LSHIFT);
    if (released & PSP_CTRL_RTRIGGER) lator.keyup(SDL_SCANCODE_LSHIFT);

    /* Select → Reset (BLKVVOD) */
    if (pressed & PSP_CTRL_SELECT) {
        keyboard.onreset(true);
    }

    /* Start → Exit */
    if (pressed & PSP_CTRL_START) {
        exitRequest = 1;
        sceKernelExitGame();
    }

    /* Numeric keys 0-9 via D-Pad + buttons combos */
    /* (simplified: number row is not directly mapped) */

    oldButtons = buttons;
}

/* Draw a simple status screen while emulator runs */
static void drawStatus(const std::string & msg)
{
    pspDebugScreenSetXY(0, 0);
    pspDebugScreenClear();
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenPrintf("Vector-06c PSP\n");
    pspDebugScreenSetTextColor(0xFFAAAAAA);
    pspDebugScreenPrintf("%s\n", msg.c_str());
    pspDebugScreenSetTextColor(0xFFFFFF00);
    pspDebugScreenPrintf("\nRunning emulator...\n");
    pspDebugScreenPrintf("Press START to exit\n");
}

int main(int argc, char *argv[])
{
    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0x00000000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenClearLineDisable();

    pspDebugScreenPrintf("Vector-06c PSP starting...\n");

    setupCallbacks();
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    /* ROM selection phase */
    std::vector<std::string> files;
    FileBrowser::listRoms(ROM_DIR, files);

    int selected = 0;
    int scrollOffset = 0;
    int oldButtons = 0;

    /* Simple ROM browser (like existing main.cpp) */
    bool romSelected = false;
    while (!exitRequest && !romSelected)
    {
        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        int buttons = pad.Buttons;
        int pressed = buttons & ~oldButtons;

        if (pressed & PSP_CTRL_DOWN) {
            if (selected < (int)files.size() - 1) {
                ++selected;
                if (selected - scrollOffset >= (272 - 56) / 16) {
                    ++scrollOffset;
                }
            }
        }
        if (pressed & PSP_CTRL_UP) {
            if (selected > 0) {
                --selected;
                if (selected < scrollOffset) {
                    --scrollOffset;
                }
            }
        }
        if (pressed & PSP_CTRL_CROSS) {
            if (!files.empty()) {
                romSelected = true;
            }
        }
        if (pressed & PSP_CTRL_CIRCLE) {
            exitRequest = 1;
            sceKernelExitGame();
        }

        oldButtons = buttons;

        /* Draw ROM list */
        pspDebugScreenSetXY(0, 0);
        pspDebugScreenClear();
        pspDebugScreenSetTextColor(0xFFFFFFFF);
        pspDebugScreenPrintf("Vector-06c PSP - ROM Browser\n");
        pspDebugScreenSetTextColor(0xFFAAAAAA);
        pspDebugScreenPrintf("Directory: %s/\n", ROM_DIR);
        pspDebugScreenPrintf("Press X to select, O to exit\n\n");

        int visible = (272 - 56) / 16;
        for (int i = scrollOffset;
             i < (int)files.size() && i < scrollOffset + visible; ++i)
        {
            unsigned int color = (i == selected) ? 0xFFFF00FF : 0xFF00FF00;
            pspDebugScreenSetTextColor(color);
            pspDebugScreenPrintf("  %s\n", files[i].c_str());
        }

        if (files.empty()) {
            pspDebugScreenSetTextColor(0xFFFF0000);
            pspDebugScreenPrintf("\nNo .rom/.bin files found.\n");
            pspDebugScreenPrintf("Place ROMs in %s/\n", ROM_DIR);
        }

        sceDisplayWaitVblankStart();
        sceKernelDelayThread(10000);
    }

    if (exitRequest) {
        sceKernelExitGame();
        return 0;
    }

    /* --- Emulator initialization phase --- */
    pspDebugScreenPrintf("\nInitializing emulator...\n");

    /* Init options for PSP */
    options(0, NULL);

    /* Init components (like android_main.cpp) */
    filler.init();
    soundnik.init();
    tv.init();
    board.init();
    fdc.init();
    io.yellowblue();

    keyboard.onreset = [](bool blkvvod) {
        board.reset(blkvvod ?
                Board::ResetMode::BLKVVOD : Board::ResetMode::BLKSBR);
    };

    board.reset(Board::ResetMode::BLKVVOD);

    /* Load the selected ROM */
    if (!files.empty()) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", ROM_DIR, files[selected].c_str());
        load_rom_file(path);
    }

    /* --- Main emulation loop --- */
    pspDebugScreenPrintf("Running...\n");

    while (!exitRequest) {
        /* Poll input and map to keyboard */
        handle_input();

        /* Execute one frame */
        lator.execute_frame();

        /* Render frame via PSP GU */
        tv.render(1);

        /* Audio is handled by PSP audio callback */
    }

    sceKernelExitGame();
    return 0;
}
