#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psptypes.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspaudiolib.h>
#include <pspaudio.h>
#include <psppower.h>
#include <pspthreadman.h>

#ifdef PROFILE
#include <pspprof.h>
#endif

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <ctime>

#include "memory.h"
#include "vio.h"
#include "tv.h"
#include "board.h"
#include "emulator.h"
#include "options.h"
#include "config.h"
#include "keyboard.h"
#include "vkbd.h"
#include "8253.h"
#include "sound.h"
#include "ay.h"
#include "wav.h"
#include "util.h"
#include "debuglog.h"

#include "../filebrowser.h"

#ifdef AUTOSELECT_ROM
#include "i8080.h"
#endif

PSP_MODULE_INFO("VECTOR06C", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(16 * 1024);

static int exitRequest = 0;
static std::string statusMessage;

/* Machine РУС/ЛАТ mode latch (IO::PC bit 3): the ROM toggles it on
 * every РУС/ЛАТ press; lit = Russian input mode. Written by the
 * worker thread via IO::onruslat, read by the main thread for the
 * VKBD LED. */
static bool vector_ruslat = false;

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

static uint16_t get_rom_org(const std::string& path)
{
    if (path.size() < 2)
        return 0x0100;

    char c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(path[path.size() - 2]))
    );

    if (c == 'o')
        return 0x0100;

    if (c >= '0' && c <= '9')
        return static_cast<uint16_t>((c - '0') * 0x0100);

    return 0x0100;
}

/* Load a ROM file into memory (Old verion) */
//void load_rom_file(Memory & memory, Board & board, const std::string & path)
//{
//    std::vector<uint8_t> data = util::load_binfile(path);
//    if (data.size() > 0) {
//        /* Load ROM at 0xC000 (typical for Vector-06C programs) */
//        memory.init_from_vector(data, 0xC000);
//        /* Set PC to the load address so the program starts executing */
//        Options.pc = 0xC000;
//        board.reset(Board::ResetMode::LOADROM);
//        dbglog("ROM loaded: %s size=%lu bytes entry=0x%04x (0xC000)\n",
//          path.c_str(), data.size(), Options.pc);
//        printf("Loaded ROM: %s (%lu bytes) at 0xC000\n", path.c_str(), data.size());
//    } else {
//        dbglog("Failed to load ROM: %s\n", path.c_str());
//        printf("Failed to load ROM: %s\n", path.c_str());
//    }
//}

/* Load a ROM file into memory */
void load_rom_file(Memory& memory, Board& board, const std::string& path)
{
    std::vector<uint8_t> data = util::load_binfile(path);

    if (data.empty()) {
        dbglog("Failed to load ROM: %s\n", path.c_str());
        printf("Failed to load ROM: %s\n", path.c_str());
        return;
    }

    uint16_t org = get_rom_org(path);

    // Загружаем ROM туда же, куда Android.
    memory.init_from_vector(data, org);

    Options.pc = org;

    dbglog("ROM loaded: %s size=%lu org=%04X pc=%04X\n",
           path.c_str(),
           static_cast<unsigned long>(data.size()),
           org,
           Options.pc);

    printf("ROM loaded: %s\n", path.c_str());
    printf("  size = %lu bytes\n",
           static_cast<unsigned long>(data.size()));
    printf("  org  = %04X\n", org);
    printf("  pc   = %04X\n", Options.pc);

    board.reset(Board::ResetMode::LOADROM);
}

/* PSP buttons -> normalized VKBD pad state (which buttons are held) */
static unsigned vkbd_padmask(uint32_t buttons)
{
    unsigned pad = 0;
    if (buttons & PSP_CTRL_LEFT)   pad |= VKBD_PAD_LEFT;
    if (buttons & PSP_CTRL_RIGHT)  pad |= VKBD_PAD_RIGHT;
    if (buttons & PSP_CTRL_UP)     pad |= VKBD_PAD_UP;
    if (buttons & PSP_CTRL_DOWN)   pad |= VKBD_PAD_DOWN;
    if (buttons & PSP_CTRL_CROSS)  pad |= VKBD_PAD_PRESS;
    return pad;
}

/* Map PSP buttons to Vector-06C keycodes. Runs in the worker thread
 * (Emulator::on_frame_input) once per machine frame, so rendering
 * stalls in the display thread cannot delay button handling. */
void handle_input(Emulator & lator, Keyboard & keyboard,
                  VirtualKeyboard & vkbd)
{
    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);

    static uint32_t oldButtons = 0;
    uint32_t buttons = pad.Buttons;
    uint32_t pressed = buttons & ~oldButtons;
    uint32_t released = oldButtons & ~buttons;

    dbglog("buttons=%08X pressed=%08X\n", buttons, pressed);

    /* SELECT: toggle the on-screen keyboard (used to be reset). The
     * keyboard keeps its top/bottom position across hide/show. */
    if (pressed & PSP_CTRL_SELECT) {
        if (vkbd.is_visible()) {
            /* releases every active virtual key, incl. a held X */
            vkbd.hide(vkbd_padmask(buttons));
        } else {
            /* While the VKBD is open no PSP button reaches the
             * Vector; release whatever is held right now or that
             * key would stay pressed forever. */
            if (buttons & PSP_CTRL_UP)        lator.keyup(SDL_SCANCODE_UP);
            if (buttons & PSP_CTRL_DOWN)      lator.keyup(SDL_SCANCODE_DOWN);
            if (buttons & PSP_CTRL_LEFT)      lator.keyup(SDL_SCANCODE_LEFT);
            if (buttons & PSP_CTRL_RIGHT)     lator.keyup(SDL_SCANCODE_RIGHT);
            if (buttons & PSP_CTRL_CROSS)     lator.keyup(SDL_SCANCODE_RETURN);
            if (buttons & PSP_CTRL_CIRCLE)    lator.keyup(SDL_SCANCODE_BACKSPACE);
            if (buttons & PSP_CTRL_TRIANGLE)  lator.keyup(SDL_SCANCODE_SPACE);
            if (buttons & PSP_CTRL_SQUARE)    lator.keyup(SDL_SCANCODE_TAB);
            if (buttons & PSP_CTRL_LTRIGGER)  lator.keyup(SDL_SCANCODE_F6);
            if (buttons & PSP_CTRL_RTRIGGER)  lator.keyup(SDL_SCANCODE_LSHIFT);

            vkbd.show(vkbd_padmask(buttons));
        }
    }

    if (vkbd.is_visible()) {
        /* Nothing reaches the emulator while the VKBD is open.
         * The D-pad navigates the keyboard (with autorepeat) and X
         * presses the selected key. */
        vkbd.update(vkbd_padmask(buttons));

        /* O: move the keyboard top <-> bottom. */
        if (pressed & PSP_CTRL_CIRCLE) {
            vkbd.move();
        }
    } else {
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
    }

    /* Start → Exit (never intercepted by the VKBD) */
    if (pressed & PSP_CTRL_START) {
        exitRequest = 1;

        #ifdef PROFILE
            gprof_stop("gmon.out", true);
        #endif

        sceKernelExitGame();
    }

    /* Numeric keys 0-9 via D-Pad + buttons combos */
    /* (simplified: number row is not directly mapped) */

    oldButtons = buttons;
}

int main(int argc, char *argv[])
{
    dbglog_open();
    dbglog("=== VECTOR06C PSP start ===\n");

    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0x00000000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenClearLineDisable();

    /* Highest clock first; step down until the firmware accepts one.
     * The return value matters: when every attempt is rejected the
     * PSP silently stays at its default 222 MHz and the emulator
     * runs ~1.5x slower (PPSSPP accepts anything, so it looks fine
     * there). The readback below shows what actually took effect. */
    {
        static const struct { int cpu; int bus; } clocks[] = {
            { 333, 166 }, { 300, 150 }, { 266, 133 }, { 222, 111 },
        };
        int rc = -1;
        for (size_t i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i) {
            rc = scePowerSetClockFrequency(
                clocks[i].cpu, clocks[i].cpu, clocks[i].bus);
            if (rc == 0) {
                break;
            }
            dbglog("clock %d/%d MHz rejected (rc=%d)\n",
                   clocks[i].cpu, clocks[i].bus, rc);
        }
        const int cpu_mhz = scePowerGetCpuClockFrequencyInt();
        const int bus_mhz = scePowerGetBusClockFrequencyInt();
        printf("CPU clock: %d MHz, bus %d MHz (set rc=%d)\n",
               cpu_mhz, bus_mhz, rc);
        dbglog("CPU clock: %d MHz, bus %d MHz (set rc=%d)\n",
               cpu_mhz, bus_mhz, rc);
    }

    dbglog("Vector-06c PSP starting...\n");

    /* config.ini next to the EBOOT (border / fps options) */
    config_load(argv[0]);

    /* Display (main) thread priority from config.ini. Changing it
     * before anything spawns also covers the ROM browser phase. */
    {
        const int rc = sceKernelChangeThreadPriority(
            0, Options.main_priority);
        dbglog("main thread priority set to 0x%02x (rc=%d)\n",
               Options.main_priority, rc);
    }

    setupCallbacks();
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

    /* ROM selection phase */
    std::vector<std::string> files;
    FileBrowser::listRoms(ROM_DIR, files);

    int selected = 0;
    int scrollOffset = 0;
    int oldButtons = 0;

    char selected_file[128] = "";

#ifdef AUTOSELECT_ROM
    /* Test build skips the browser: autoselect.txt next to the EBOOT
     * holds the file name of the ROM to boot from ROMS/. */
    FILE * as = std::fopen("autoselect.txt", "r");
    if (as != nullptr) {
        if (std::fgets(selected_file, sizeof(selected_file), as)
                != nullptr) {
            /* fgets keeps the line ending; it would end up inside
             * the ROM path. */
            size_t n = std::strlen(selected_file);
            while (n > 0 &&
                   (selected_file[n - 1] == '\n' ||
                    selected_file[n - 1] == '\r' ||
                    selected_file[n - 1] == ' '  ||
                    selected_file[n - 1] == '\t')) {
                selected_file[--n] = '\0';
            }
        }
        std::fclose(as);
    }
#else
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
                snprintf(selected_file, sizeof(selected_file), "%s",
                         files[selected].c_str());
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
#endif

#ifdef PROFILE
    gprof_start();
#endif

    /* --- Emulator objects initialization (after PSP env is ready) --- */
    /* NOTE: Large objects (Memory ~640KB, Debug ~9.4MB) must be allocated
     * on the heap, NOT on the stack (PSP stack is only ~256KB). */

    dbglog("Старт main()...\n");
    dbglog("Инициализирую Memory... ");
    Memory* memory = new Memory();
    dbglog("OK\n");

    dbglog("Инициализирую Debug... ");
    Debug* debug = new Debug(memory);
    dbglog("OK\n");

    dbglog("Инициализирую FD1793... ");
    FD1793* fdc = new FD1793();
    dbglog("OK\n");

    dbglog("Инициализирую Wav... ");
    Wav* wav = new Wav();
    dbglog("OK\n");

    dbglog("Инициализирую WavPlayer... ");
    WavPlayer* tape_player = new WavPlayer(*wav);
    dbglog("OK\n");

    dbglog("Инициализирую Keyboard... ");
    Keyboard* keyboard = new Keyboard();
    dbglog("OK\n");

    dbglog("Инициализирую I8253... ");
    I8253* timer = new I8253();
    dbglog("OK\n");

    dbglog("Инициализирую TimerWrapper... ");
    TimerWrapper* tw = new TimerWrapper(*timer);
    dbglog("OK\n");

    dbglog("Инициализирую AY... ");
    AY* ay = new AY();
    dbglog("OK\n");

    dbglog("Инициализирую AYWrapper... ");
    AYWrapper* aw = new AYWrapper(*ay);
    dbglog("OK\n");

    dbglog("Инициализирую Soundnik... ");
    Soundnik* soundnik = new Soundnik(*tw, *aw);
    dbglog("OK\n");

    dbglog("Инициализирую IO... ");
    IO* io = new IO(*memory, *keyboard, *timer, *fdc, *ay, *tape_player);
    dbglog("OK\n");

    /* Sound chip writes are queued as timestamped events and rendered in
     * batch by Soundnik::process_frame() at the end of each frame */
    io->sound_event = [soundnik](SoundEventType type, uint8_t addr,
        uint8_t value) {
        soundnik->push_event(type, addr, value);
    };

    /* The ROM flips this latch together with the input mode when the
     * РУС/ЛАТ key is pressed; the VKBD LED shows the latch. */
    io->onruslat = [](bool rus) { vector_ruslat = rus; };

    dbglog("Инициализирую TV... ");
    TV* tv = new TV();
    dbglog("OK\n");

    dbglog("Инициализирую PixelFiller... ");
    PixelFiller* filler = new PixelFiller(*memory, *io, *tv);
    dbglog("OK\n");

    dbglog("Инициализирую Board... ");
    Board* board = new Board(*memory, *io, *filler, *soundnik, *tv, *tape_player, *debug);
    dbglog("OK\n");

    dbglog("Инициализирую Emulator... ");
    Emulator* lator = new Emulator(*board);
    dbglog("OK\n");

    dbglog("Инициализирую экранную клавиатуру... ");
    /* On-screen Vector keyboard (UI layer of the main thread); its
     * virtual presses go through the same keydown/keyup queue as the
     * physical PSP buttons. */
    VirtualKeyboard* vkbd = new VirtualKeyboard();
    vkbd->prepare();
    /* The VKBD LED shows the machine РУС/ЛАТ mode latch (lit =
     * Russian input), not the keyboard's key level. */
    vkbd->set_ruslat_source(&vector_ruslat);
    vkbd->on_keydown = [lator](int scancode) { lator->keydown(scancode); };
    vkbd->on_keyup = [lator](int scancode) { lator->keyup(scancode); };
    dbglog("OK\n");

    /* PSP pad handling lives in the worker thread: one poll per
     * machine frame (50 Hz), independent of how fast the display
     * thread presents pictures. */
    lator->on_frame_input = [lator, keyboard, vkbd]() {
        handle_input(*lator, *keyboard, *vkbd);
    };

    dbglog("Инициализирую эмулятор (options)...\n");

    /* Init options for PSP */
    options(0, NULL);

    /* Init components (like android_main.cpp) */
    filler->init();

    /* Diagnostic sound recording (config.ini: sound_record = true):
     * psp_internal.wav gets the samples right after Soundnik generates
     * them, psp_callback.wav gets what the PSP audio callback actually
     * feeds to the hardware. Files live in the working directory (the
     * game folder on the memory stick / under PPSSPP). */
    WavRecorder* rec_internal = nullptr;
    WavRecorder* rec_callback = nullptr;
    if (Options.sound_record && !Options.nosound) {
        rec_internal = new WavRecorder();
        rec_internal->init("psp_internal.wav");
        rec_callback = new WavRecorder();
        rec_callback->init("psp_callback.wav");
        printf("Sound recording enabled\n");
        printf("Internal: psp_internal.wav\n");
        printf("Callback: psp_callback.wav\n");
        dbglog("sound_record: internal=psp_internal.wav callback=psp_callback.wav\n");
    }

    soundnik->init(rec_internal, rec_callback);
    tv->init();
    board->init();
    fdc->init();
    io->yellowblue();

    keyboard->onreset = [board](bool blkvvod) {
        board->reset(blkvvod ?
                Board::ResetMode::BLKVVOD : Board::ResetMode::BLKSBR);
    };

    board->reset(Board::ResetMode::BLKVVOD);

    /* Load the selected ROM */
    if (selected_file[0] != '\0') {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", ROM_DIR, selected_file);
        load_rom_file(*memory, *board, path);
    }

    /* ROM is loaded, Board is ready: start the emulation worker.
     * From here on the main thread only polls input, renders with GU
     * and waits for vblank; the Board itself belongs to the worker. */
    lator->start_emulator_thread();

    /* --- Main emulation loop --- */
    dbglog("Running...\n");
    dbglog("entering main loop\n");
    int dbg_frame = 0;

    /* Frame counter */
    unsigned int fps_frames = 0;
    unsigned int fps_last_time = sceKernelGetSystemTimeLow();
#ifdef AUTOSELECT_ROM
    unsigned int auto_start_us = 0;
#endif

    while (!exitRequest) {
        /* PSP pad handling runs in the worker thread now (see
         * lator->on_frame_input); the display thread only paints. */

        /* Re-rasterize the VKBD overlay texture only when its visual
         * state changed (selection, pressed keys, РУС/LAT). Hidden
         * keyboard: zero cost. */
        if (vkbd->is_visible() && vkbd->needs_repaint()) {
            vkbd->paint();
        }

        /* Present the newest ready frame via PSP GU; this call also
         * paces the loop at the LCD vblank. The machine frames
         * themselves run in the worker thread, independently. */
        dbglog("frame %d: tv->render...\n", dbg_frame);
#ifdef AUTOSELECT_ROM
        unsigned perf_tr0 = sceKernelGetSystemTimeLow();
#endif
        tv->render(vkbd);
#ifdef AUTOSELECT_ROM
        board->perf_render_us += sceKernelGetSystemTimeLow() - perf_tr0;
#endif
        dbglog("frame %d: tv->render done\n", dbg_frame);

        /*if (dbg_frame == 99)
            tv->save_frame( files[selected] + ".bmp" );
        if (dbg_frame == 100)
            memory->save_dump( files[selected] + ".dump");*/

#ifdef AUTOSELECT_ROM
        if (dbg_frame == 600) {
            tv->save_frame("frame_t600.bmp");
            memory->save_dump("dump_t600.bin");
        }
        if (dbg_frame % 50 == 0) {
            dbglog("TRACE pc=%04x sp=%04x\n", i8080cpu::i8080_pc(), i8080cpu::i8080_regs_sp());
        }
#endif
        ++dbg_frame;

#ifdef AUTOSELECT_ROM
        /* Auto-stop: run the machine for exactly 60 s so the host can
         * collect a reproducible gmon.out without touching the pad. */
        if (auto_start_us == 0)
            auto_start_us = sceKernelGetSystemTimeLow();
        if ((unsigned int)(sceKernelGetSystemTimeLow() - auto_start_us)
                >= 60000000) {
#ifdef PROFILE
            gprof_stop("gmon.out", 1);
#endif
            break;
        }
#endif

        ++fps_frames;
        unsigned int now = sceKernelGetSystemTimeLow();
        if ((unsigned int)(now - fps_last_time) >= 1000000) {
            dbglog("FPS: %u\n", fps_frames);

#ifdef AUTOSELECT_ROM
            {
                FILE* pf = fopen("perf.log", "a");
                if (pf) {
                    fprintf(pf,
                        "PERF loop=%u mach=%u exec=%u.%03u snd=%u.%03u "
                        "render=%u.%03u cpu=%u.%03u fill=%u.%03u "
                        "fastfb=%u.%03u "
                        "sync=%u.%03u vbl=%u.%03u flush=%u.%03u ms\n",
                        fps_frames, board->perf_frames,
                        board->perf_exec_us / 1000, board->perf_exec_us % 1000,
                        board->perf_snd_us / 1000, board->perf_snd_us % 1000,
                        board->perf_render_us / 1000,
                        board->perf_render_us % 1000,
                        board->perf_cpu_us / 1000, board->perf_cpu_us % 1000,
                        board->perf_fill_us / 1000, board->perf_fill_us % 1000,
                        board->perf_fastfb_us / 1000,
                        board->perf_fastfb_us % 1000,
                        tv->perf_sync_us / 1000, tv->perf_sync_us % 1000,
                        tv->perf_vbl_us / 1000, tv->perf_vbl_us % 1000,
                        tv->perf_flush_us / 1000, tv->perf_flush_us % 1000);
                    fprintf(pf,
                        "SND  ev=%u.%03u tmr=%u.%03u ay=%u.%03u mix=%u.%03u "
                        "samples=%u aysteps=%u\n",
                        board->snd_perf().perf_ev_us / 1000, board->snd_perf().perf_ev_us % 1000,
                        board->snd_perf().perf_tmr_us / 1000, board->snd_perf().perf_tmr_us % 1000,
                        board->snd_perf().perf_ay_us / 1000, board->snd_perf().perf_ay_us % 1000,
                        board->snd_perf().perf_mix_us / 1000, board->snd_perf().perf_mix_us % 1000,
                        board->snd_perf().perf_nsamples, board->snd_perf().perf_naysteps);
                    board->snd_perf().perf_ev_us = board->snd_perf().perf_tmr_us = 0;
                    board->snd_perf().perf_ay_us = board->snd_perf().perf_mix_us = 0;
                    board->snd_perf().perf_nsamples = board->snd_perf().perf_naysteps = 0;
                    tv->perf_sync_us = tv->perf_vbl_us = tv->perf_flush_us = 0;
                    fclose(pf);
                }
                board->perf_exec_us = board->perf_snd_us = 0;
                board->perf_render_us = board->perf_cpu_us = 0;
                board->perf_fill_us = 0;
                board->perf_fastfb_us = 0;
                board->perf_frames = 0;
            }
#endif

            fps_frames = 0;
            fps_last_time = now;
        }
    }

    /* Stop the worker before tearing the machine objects down. */
    lator->stop_emulator_thread();

    /* Finish the diagnostic sound recording: finalize the WAV headers
     * and report how much each side of the pipeline produced. */
    if (rec_internal != nullptr || rec_callback != nullptr) {
        uint32_t internal_frames = 0, callback_frames = 0;
        if (rec_internal != nullptr) {
            internal_frames = rec_internal->frames_written();
            rec_internal->close();
        }
        if (rec_callback != nullptr) {
            callback_frames = rec_callback->frames_written();
            rec_callback->close();
        }
        printf("Internal samples: %lu\n",
               (unsigned long)internal_frames);
        printf("Callback samples: %lu\n",
               (unsigned long)callback_frames);
        dbglog("sound_record done: internal=%lu callback=%lu frames\n",
               (unsigned long)internal_frames,
               (unsigned long)callback_frames);
    }

    dbglog_close();
    sceKernelExitGame();
    return 0;
}
