#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psptypes.h>

#include <cstdio>
#include <vector>
#include <string>

#include "filebrowser.h"

PSP_MODULE_INFO("VECTOR06C", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(8 * 1024);

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

static void drawList(const std::vector<std::string> &files, int selected,
                     int scrollOffset)
{
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

    if (files.empty())
    {
        pspDebugScreenSetTextColor(0xFFFF0000);
        pspDebugScreenPrintf("\nNo .rom/.bin files found.\n");
        pspDebugScreenPrintf("Place ROMs in %s/\n", ROM_DIR);
    }

    if (!statusMessage.empty())
    {
        pspDebugScreenSetTextColor(0xFFFFFF00);
        pspDebugScreenPrintf("\n%s\n", statusMessage.c_str());
    }
}

int main(int argc, char *argv[])
{
    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0x00000000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);

    /* Disable clear-line to reduce flicker on redraws */
    pspDebugScreenClearLineDisable();

    pspDebugScreenPrintf("Vector-06c PSP starting...\n");

    setupCallbacks();

    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    std::vector<std::string> files;
    FileBrowser::listRoms(ROM_DIR, files);

    int selected = 0;
    int scrollOffset = 0;
    int oldButtons = 0;

    drawList(files, selected, scrollOffset);
    bool redraw = true;

    while (!exitRequest)
    {
        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);

        int buttons = pad.Buttons;
        int pressed = buttons & ~oldButtons;

        if (pressed & PSP_CTRL_DOWN)
        {
            if (selected < (int)files.size() - 1)
            {
                ++selected;
                if (selected - scrollOffset >= (272 - 56) / 16)
                {
                    ++scrollOffset;
                }
                redraw = true;
            }
        }
        if (pressed & PSP_CTRL_UP)
        {
            if (selected > 0)
            {
                --selected;
                if (selected < scrollOffset)
                {
                    --scrollOffset;
                }
                redraw = true;
            }
        }
        if (pressed & PSP_CTRL_CROSS)
        {
            if (!files.empty())
            {
                char path[512];
                snprintf(path, sizeof(path), "ms0:/PSP/GAME/VECTOR06C/ROMS/%s",
                         files[selected].c_str());
                statusMessage = std::string("[Selected] ") + path +
                                "\n(ROM loading coming in next stage)";
                redraw = true;
            }
        }
        if (pressed & PSP_CTRL_CIRCLE)
        {
            exitRequest = 1;
            sceKernelExitGame();
        }

        oldButtons = buttons;

        if (redraw)
        {
            drawList(files, selected, scrollOffset);
            redraw = false;
        }

        sceDisplayWaitVblankStart();
        sceKernelDelayThread(10000);
    }

    sceKernelExitGame();
    return 0;
}
