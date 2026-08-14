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

/* Map PSP buttons to Vector-06C keycodes */
void handle_input(Emulator & lator, Keyboard & keyboard)
{
    SceCtrlData pad;
    sceCtrlReadBufferPositive(&pad, 1);

    static uint32_t oldButtons = 0;
    uint32_t buttons = pad.Buttons;
    uint32_t pressed = buttons & ~oldButtons;
    uint32_t released = oldButtons & ~buttons;

    dbglog("buttons=%08X pressed=%08X\n", buttons, pressed);

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

    /* Select → Reset (BLKVVOD): executed by the worker thread, the
     * main thread must not touch the Board while the worker runs. */
    if (pressed & PSP_CTRL_SELECT) {
        lator.request_reset(true);
    }

    /* Start → Exit */
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

    scePowerSetClockFrequency(333, 333, 166);

    pspDebugScreenInit();
    pspDebugScreenSetBackColor(0x00000000);
    pspDebugScreenSetTextColor(0xFFFFFFFF);
    pspDebugScreenClearLineDisable();

    dbglog("Vector-06c PSP starting...\n");

    /* config.ini next to the EBOOT (border / fps options) */
    config_load(argv[0]);

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
#ifdef AUTOSELECT_ROM
    /* Test hook: if autoselect.txt exists next to the EBOOT, skip the
     * browser and boot that ROM index directly (-1 = stay in the boot
     * ROM). Without the file the browser works as usual. */
    {
        std::vector<uint8_t> d = util::load_binfile("autoselect.txt");
        if (!d.empty()) {
            d.push_back(0);
            int idx = atoi((const char *)d.data());
            if (idx == -1) {
                files.clear();
                romSelected = true;
            } else if (idx < (int)files.size()) {
                selected = idx;
                romSelected = true;
            }
        }
    }
#endif
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

    dbglog("Инициализирую эмулятор (options)...\n");

    /* Init options for PSP */
    options(0, NULL);

    /* Init components (like android_main.cpp) */
    filler->init();
    soundnik->init();
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
    if (!files.empty()) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", ROM_DIR, files[selected].c_str());
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
        /* Poll input and queue it for the worker thread */
        dbglog("frame %d: handle_input...\n", dbg_frame);
        handle_input(*lator, *keyboard);
        dbglog("frame %d: handle_input done\n", dbg_frame);

        /* Present the newest ready frame via PSP GU; this call also
         * paces the loop at the LCD vblank. The machine frames
         * themselves run in the worker thread, independently. */
        dbglog("frame %d: tv->render...\n", dbg_frame);
#ifdef AUTOSELECT_ROM
        unsigned perf_tr0 = sceKernelGetSystemTimeLow();
#endif
        tv->render();
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
                        "sync=%u.%03u vbl=%u.%03u flush=%u.%03u ms\n",
                        fps_frames, board->perf_frames,
                        board->perf_exec_us / 1000, board->perf_exec_us % 1000,
                        board->perf_snd_us / 1000, board->perf_snd_us % 1000,
                        board->perf_render_us / 1000,
                        board->perf_render_us % 1000,
                        board->perf_cpu_us / 1000, board->perf_cpu_us % 1000,
                        board->perf_fill_us / 1000, board->perf_fill_us % 1000,
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
                board->perf_frames = 0;
            }
#endif

            fps_frames = 0;
            fps_last_time = now;
        }
    }

    /* Stop the worker before tearing the machine objects down. */
    lator->stop_emulator_thread();

    dbglog_close();
    sceKernelExitGame();
    return 0;
}
