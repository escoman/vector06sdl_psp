#include <cstring>
#include <cctype>
#include "emulator.h"
#include "filler.h"
#include "tv.h"
#include "options.h"
#include "util.h"
#include "serialize.h"
#include "debuglog.h"

/*
 * Two-thread architecture on the PSP:
 *
 *   Worker thread  - the whole Vector-06C core (Board: CPU, Memory,
 *                    IO, PixelFiller, Soundnik) runs strictly
 *                    sequentially here, paced against the wall clock
 *                    (one machine frame every 20 ms, i.e. 50 fps).
 *                    The PSP pad input (on_frame_input) also runs
 *                    here, so button presses are sampled even when
 *                    the picture presentation stalls.
 *   Main thread    - PSP GU and VSync only. It presents whatever the
 *                    newest ready frame is and never touches the
 *                    Board directly; resets go to the worker through
 *                    the command slots below.
 *
 * The framebuffer handoff (ownership states, publish/acquire) lives
 * in TV. The audio boundary is Soundnik's sample ring, exactly as
 * before: the callback only reads finished samples.
 */

/* Worker priority comes from config.ini (worker_priority, default
 * 0x18): higher than the display thread (main_priority, default
 * 0x20) but below the PSP system/audio threads; tune on hardware if
 * the worker starves the display thread or vice versa. */
#define WORKER_STACKSIZE (64 * 1024)

Emulator::Emulator(Board & borat) : board(borat),
    worker_thid(-1), worker_running(false), worker_stop_req(false),
    paused(false), first_frame_published(false),
    menu_auto_opened(false), worker_start_us(0),
    rom_base("boot"),
    frame_deadline_us(0), machine_us_last(0), machine_count(0)
{
    for (int i = 0; i < N_SCANCODES; ++i) {
        this->keydowns[i] = 0;
        this->keyups[i] = 0;
    }
    for (int i = 0; i < N_COMMANDS; ++i) {
        this->commands[i] = CMD_NONE;
    }
}

Emulator::~Emulator()
{
    this->stop_emulator_thread();
}

/* Worker thread only: drain the input and command slots, then run
 * exactly one machine frame. The cadence pattern is not used: it only
 * existed to skip machine frames while this loop was vblank-locked at
 * 60 Hz; the worker now paces itself and never skips frames. */
int Emulator::execute_frame()
{
    /* PSP pad first: its events go into the slots drained right
     * below, so a press lands in this very machine frame. */
    if (this->on_frame_input) {
        this->on_frame_input();
    }

    for (int i = 0; i < N_SCANCODES; ++i) {
        const int kd = this->keydowns[i].exchange(0);
        const int ku = this->keyups[i].exchange(0);
        if (kd) {
            SDL_KeyboardEvent ev;
            ev.keysym.scancode = kd;
            this->board.handle_keydown(ev);
        }
        if (ku) {
            SDL_KeyboardEvent ev;
            ev.keysym.scancode = ku;
            this->board.handle_keyup(ev);
        }
    }

    for (int i = 0; i < N_COMMANDS; ++i) {
        const int cmd = this->commands[i].exchange(CMD_NONE);
        if (cmd == CMD_RESET_BLKVVOD) {
            this->board.reset(Board::ResetMode::BLKVVOD);
        } else if (cmd == CMD_RESET_BLKSBR) {
            this->board.reset(Board::ResetMode::BLKSBR);
        }
    }

    const int executed = this->board.execute_frame_with_cadence(true, false);
    return executed ? 1 : 0;
}

void Emulator::worker_loop()
{
    TV & tv = this->board.get_tv();
    dbglog("worker: loop entered\n");
    printf("WORKER: loop entered\n");

    this->frame_deadline_us = sceKernelGetSystemTimeLow();
    this->worker_start_us = this->frame_deadline_us;
    this->machine_us_last = this->frame_deadline_us;
    this->machine_count = 0;
    this->cycles_window_last = (unsigned)this->board.get_total_cycles();
    this->exec_us_window = 0;
    this->last_deadline_err_us = 0;

    bool was_paused = false;

    while (!this->worker_stop_req.load(std::memory_order_relaxed)) {
        if (this->paused.load(std::memory_order_acquire)) {
            /* Machine frozen (MAIN MENU open). No machine frame, no
             * framebuffer, no CPU/Memory/IO/PixelFiller/Soundnik
             * work: the loop only keeps the UI input alive so the
             * menu can be navigated and closed, then sleeps shortly
             * to stay well below 100% CPU. The worker thread itself
             * is never terminated or suspended. */
            if (this->on_frame_input) {
                this->on_frame_input();
            }
            sceKernelDelayThread(FRAME_PERIOD_US);
            was_paused = true;
            continue;
        }
        if (was_paused) {
            /* Resume: re-anchor the pacing deadline and the rate
             * measurement window; the pause would otherwise look
             * like one huge deadline slip / a collapsed frame
             * rate. */
            this->frame_deadline_us = sceKernelGetSystemTimeLow();
            this->machine_us_last = this->frame_deadline_us;
            this->machine_count = 0;
            this->cycles_window_last =
                (unsigned)this->board.get_total_cycles();
            this->exec_us_window = 0;
            this->last_deadline_err_us = 0;
            was_paused = false;
        }
        /* Framebuffer for this machine frame. With three buffers this
         * succeeds immediately in the normal pipeline; the retry path
         * only exists as a safety net and is not a wait on the display
         * thread per se. */
        uint8_t * fb = tv.acquire_write_buffer();
        if (!fb) {
            dbglog("worker: no write buffer, retry\n");
            sceKernelDelayThread(1000);
            continue;
        }

        this->board.get_filler().set_framebuffer(fb);
        const unsigned exec_t0 = sceKernelGetSystemTimeLow();
        this->execute_frame();
        this->exec_us_window += sceKernelGetSystemTimeLow() - exec_t0;
        tv.publish_frame(fb);
        if (!this->first_frame_published) {
            this->first_frame_published = true;
            dbglog("worker: first frame published\n");
            printf("WORKER: first frame published\n");
        }
        /* AUTO_MENU_OPEN_DELAY_US after the worker start the MAIN
         * MENU opens over the boot ROM picture and the machine
         * freezes, as if START was pressed; until then the boot ROM
         * keeps running. Fires once, and only when the UI wired the
         * hook (the AUTOSELECT_ROM test builds leave it unwired). */
        if (!this->menu_auto_opened && this->on_auto_open_menu &&
                this->first_frame_published &&
                (unsigned)(sceKernelGetSystemTimeLow()
                    - this->worker_start_us) >= AUTO_MENU_OPEN_DELAY_US) {
            this->menu_auto_opened = true;
            this->on_auto_open_menu();
        }
        if ((this->machine_count % 50) == 0) {
            printf("WORKER: %d frames published\n", this->machine_count);
        }

        /* Machine frames per second, shown by the FRAMES overlay.
         * The window lives on its own fixed 1 s grid: the boundary
         * advances by exactly 1000000 µs, so the leftover fraction of
         * every window carries into the next one. Re-anchoring the
         * boundary to "now" here was making the displayed rate wobble
         * between 50 and 51 even with a perfectly paced worker. */
        ++this->machine_count;
        unsigned now = sceKernelGetSystemTimeLow();
        if ((unsigned)(now - this->machine_us_last) >= 1000000) {
            const unsigned cycles_now =
                (unsigned)this->board.get_total_cycles();
            tv.set_machine_fps(this->machine_count);
            tv.set_machine_cycles(
                (int)(cycles_now - this->cycles_window_last));
            if (this->machine_count > 0) {
                tv.set_exec_us(
                    (int)(this->exec_us_window / (unsigned)this->machine_count));
            }
            tv.set_deadline_err_us(this->last_deadline_err_us);
            printf("WORKER: mfr=%d cyc=%lu exec=%u.%03ums dline=%+dus\n",
                   this->machine_count,
                   (unsigned long)(cycles_now - this->cycles_window_last),
                   this->exec_us_window / 1000,
                   this->exec_us_window % 1000,
                   this->last_deadline_err_us);
            this->machine_count = 0;
            this->cycles_window_last = cycles_now;
            this->exec_us_window = 0;

            this->machine_us_last += 1000000;
            if ((unsigned)(now - this->machine_us_last) >= 1000000) {
                /* The worker stalled for over a second (suspend,
                 * debugger): the counters above already cover it, so
                 * re-anchor instead of reporting a bogus rate. */
                this->machine_us_last = now;
            }
        }

        /* Wall-clock pacing: never run faster than the real machine.
         * Falling behind costs nothing extra: the deadline just slips
         * until the gap exceeds four frames, when it is re-anchored
         * (no burst catch-up). */
        this->frame_deadline_us += FRAME_PERIOD_US;
        now = sceKernelGetSystemTimeLow();
        if ((int)(this->frame_deadline_us - now) > 0) {
            sceKernelDelayThread(this->frame_deadline_us - now);
            now = sceKernelGetSystemTimeLow();
        }
        /* How far real time drifted past (+) the deadline; a growing
         * value would point at sceKernelDelayThread inaccuracy, a
         * large one at a frame that took longer than 20 ms. */
        this->last_deadline_err_us = (int)(now - this->frame_deadline_us);
        if (this->last_deadline_err_us > 4 * (int)FRAME_PERIOD_US) {
            this->frame_deadline_us = now;
        }
    }

    this->worker_running = false;
}

int Emulator::worker_entry(SceSize args, void * argp)
{
    (void)args;
    Emulator * self = nullptr;
    std::memcpy(&self, argp, sizeof(self));
    self->worker_loop();
    sceKernelExitThread(0);
    return 0;
}

void Emulator::start_emulator_thread()
{
    if (this->worker_thid >= 0) {
        return;
    }

    this->worker_stop_req = false;
    this->worker_running = true;

    this->worker_thid = sceKernelCreateThread("v06x_worker",
        &Emulator::worker_entry, Options.worker_priority, WORKER_STACKSIZE,
        THREAD_ATTR_USER, 0);
    if (this->worker_thid >= 0) {
        Emulator * self = this;
        const int rc = sceKernelStartThread(
            this->worker_thid, sizeof(self), &self);
        printf("MAIN: StartThread rc=%d thid=%d\n",
               rc, this->worker_thid);
        dbglog("Emulator: worker thread started (prio 0x%x, rc=%d)\n",
               Options.worker_priority, rc);
    } else {
        dbglog("Emulator: failed to create worker thread\n");
        this->worker_running = false;
    }
}

void Emulator::stop_emulator_thread()
{
    if (this->worker_thid < 0) {
        return;
    }

    this->worker_stop_req = true;
    /* The loop checks the flag once per machine frame (~20 ms). */
    for (int i = 0; i < 300; ++i) {
        if (!this->worker_running.load()) {
            break;
        }
        sceKernelDelayThread(10000);
    }

    sceKernelDeleteThread(this->worker_thid);
    this->worker_thid = -1;
}

void Emulator::pause()
{
    this->paused.store(true, std::memory_order_release);
}

void Emulator::resume()
{
    this->paused.store(false, std::memory_order_release);
}

bool Emulator::is_paused() const
{
    return this->paused.load(std::memory_order_acquire);
}

void Emulator::keydown(int scancode)
{
    for (int i = 0; i < N_SCANCODES; ++i) {
        int expected = 0;
        if (this->keydowns[i].compare_exchange_strong(expected, scancode)) {
            break;
        }
        if (expected == scancode) {
            break; /* already queued */
        }
    }
}

void Emulator::keyup(int scancode)
{
    for (int i = 0; i < N_SCANCODES; ++i) {
        int expected = 0;
        if (this->keyups[i].compare_exchange_strong(expected, scancode)) {
            break;
        }
        if (expected == scancode) {
            break; /* already queued */
        }
    }
}

void Emulator::request_reset(bool blkvvod)
{
    const int cmd = blkvvod ? CMD_RESET_BLKVVOD : CMD_RESET_BLKSBR;
    for (int i = 0; i < N_COMMANDS; ++i) {
        int expected = CMD_NONE;
        if (this->commands[i].compare_exchange_strong(expected, cmd)) {
            break;
        }
    }
}

/* Load address comes from the file name, same rule as the desktop
 * port: NAME0.ROM loads at 0x0000, NAME1.ROM at 0x0100, ... NAME9.ROM
 * at 0x0900; anything else (plain .ROM/.BIN) defaults to 0x0100. */
static uint16_t get_rom_org(const std::string & path)
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

bool Emulator::load_rom(const std::string & path)
{
    /* Read the file first: the old ROM's working state must survive
     * until the new one passes this basic check. */
    std::vector<uint8_t> data = util::load_binfile(path);
    if (data.empty()) {
        dbglog("Failed to load ROM: %s\n", path.c_str());
        return false;
    }

    const uint16_t org = get_rom_org(path);

    /* Install the ROM exactly where the Android port does, then
     * reset the board into the new program. Paging goes back to the
     * power-on state FIRST: init_from_vector() places the bytes
     * through the current mapping, and the desktop port loads its
     * single ROM with the power-on mapping too. */
    this->board.get_memory().reset_paging();
    this->board.get_memory().init_from_vector(data, org);
    Options.pc = org;
    this->board.reset(Board::ResetMode::LOADROM);

    /* The save-state directory is tied to the ROM name: the file
     * name without its extension, exactly as it sits in ROMS/ (no
     * path part, case untouched). */
    {
        const size_t slash = path.find_last_of('/');
        std::string base = (slash == std::string::npos)
            ? path : path.substr(slash + 1);
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos && dot > 0)
            base = base.substr(0, dot);
        if (!base.empty())
            this->rom_base = base;
    }

    /* The full file path backs the Save Preview menu item; the boot
     * loader never reaches load_rom, so an empty rom_path keeps
     * meaning "no ROM file". */
    this->rom_path = path;

    dbglog("ROM loaded: %s size=%lu org=%04X pc=%04X\n",
           path.c_str(), (unsigned long)data.size(),
           org, Options.pc);
    return true;
}

void Emulator::export_audio_frame(float * dst, size_t framesize)
{
    /* PSP version: audio is handled by the PSP audio callback thread.
     * This function is not used in the PSP main loop. */
    (void)dst;
    (void)framesize;
}

size_t Emulator::pixel_bytes_size() {
    return (size_t) (Options.screen_width * Options.screen_height * 4);
}

void Emulator::save_state(vector <uint8_t> &to) {
    this->board.serialize(to);
}

/* The state file carries only memory, CPU and the visible IO part;
 * the sound chips (8253, AY) and the Soundnik mirrors/event queue
 * stay out of it. Reset the whole machine through the same path a
 * ROM load uses before applying the snapshot, so no pre-load
 * register contents survive — without this, a note playing at the
 * load moment drones on forever after the restore. */
static bool state_chunks_valid(std::vector<uint8_t> & data)
{
    auto it = data.begin();
    while (it != data.end()) {
        if ((size_t)std::distance(it, data.end()) < 8)
            return false;
        SerializeChunk::id signature;
        uint32_t size;
        auto begin = SerializeChunk::take_chunk(it, signature, size);
        switch (signature) {
            case SerializeChunk::MEMORY:
            case SerializeChunk::IO:
            case SerializeChunk::CPU:
            case SerializeChunk::BOARD:
            case SerializeChunk::DEBUG:
                break;
            default:
                return false;
        }
        if ((size_t)std::distance(begin, data.end()) < size)
            return false;
        it = begin + size;
    }
    return true;
}

bool Emulator::restore_state(vector <uint8_t> &from) {
    /* §23: a refused state leaves the running machine untouched, so
     * validate the stream before the destructive reset. */
    if (!state_chunks_valid(from))
        return false;
    this->board.reset(Board::ResetMode::LOADROM);
    return this->board.deserialize(from);
}
