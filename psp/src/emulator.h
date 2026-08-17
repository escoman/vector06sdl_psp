#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <pspkernel.h>
#include "event.h"
#include "board.h"

class Emulator {
    static const int N_SCANCODES = 4;
    static const int N_COMMANDS = 4;

public:
    /* Main -> Worker commands */
    enum Command : int {
        CMD_NONE = 0,
        CMD_RESET_BLKVVOD,
        CMD_RESET_BLKSBR
    };

private:
    Board & board;

    /* Input handoff slots, filled by keydown()/keyup() and emptied
     * by the worker before every machine frame; no other locking. */
    std::atomic<int> keydowns[N_SCANCODES];
    std::atomic<int> keyups[N_SCANCODES];

    /* Main -> Worker command slots (reset etc.) */
    std::atomic<int> commands[N_COMMANDS];

    /* Worker thread */
    int worker_thid;
    std::atomic<bool> worker_running;
    std::atomic<bool> worker_stop_req;

    /* UI pause flag (MAIN MENU open): the worker thread keeps
     * running, but executes no machine frames while it is set. The
     * worker never stops/suspends; it polls the UI input and waits.
     * Written by pause()/resume(), checked by the worker loop before
     * every machine frame. */
    std::atomic<bool> paused;

    /* Worker only: first machine frame published since start. */
    bool first_frame_published;

    /* Worker only: the MAIN MENU auto-open fired once. */
    bool menu_auto_opened;
    unsigned worker_start_us;

    /* Base name of the currently loaded ROM without its extension
     * ("RISEOUT"); the save-state directory is tied to it
     * (SAVES/<rom_base>/). Stays "boot" until the ROM Browser loads
     * something. */
    std::string rom_base;

    /* Full path of the currently loaded ROM file, exactly as passed
     * to load_rom(); empty while the default boot loader runs (no
     * ROM file behind it). The Save Preview menu item writes the
     * preview TGA next to this file. */
    std::string rom_path;

    /* Worker wall-clock pacing: one machine frame every 20 ms. */
    static const unsigned FRAME_PERIOD_US = 20000;
    /* Auto-open MAIN MENU delay after worker start, µs: the boot ROM
     * runs this long before the menu appears. */
    static const unsigned AUTO_MENU_OPEN_DELAY_US = 1*1000000;
    unsigned frame_deadline_us;
    /* Machine rate measurement window: fixed 1 s grid anchored once at
     * worker start (machine_us_last += 1000000, never = now). */
    unsigned machine_us_last;
    int machine_count;
    unsigned cycles_window_last;  /* Board total cycles at window start */
    unsigned exec_us_window;      /* execute_frame time this window, µs */
    int last_deadline_err_us;     /* late (+) / early (-) vs deadline */

    static int worker_entry(SceSize args, void * argp);
    void worker_loop();

public:
    /* One machine frame: input/commands first, then the board. Worker
     * thread only; the main thread must not call this once the worker
     * runs (the board is owned by the worker). */
    int execute_frame();
    void keydown(int scancode);
    void keyup(int scancode);
    void set_joysticks(int joy_0e, int joy_0f);
    void set_volumes(float timer, float beeper, float ay, float covox, float master);
    void enable_timer_channels(bool ech0, bool ech1, bool ech2);
    void enable_ay_channels(bool ech0, bool ech1, bool ech2);
    void export_audio_frame(float * dst, size_t count);
    size_t pixel_bytes_size();

    /* Queue a machine reset to be executed by the worker. */
    void request_reset(bool blkvvod);

    /* Install a ROM through the existing loader: read the file,
     * memory.init_from_vector at the org derived from the file name,
     * Board reset with ResetMode::LOADROM. The file is read and
     * size-checked first, so the old ROM's working state stays intact
     * when the read fails. Runs wherever the Board is currently
     * owned: the main thread before start_emulator_thread(), the
     * worker thread afterwards (the ROM Browser calls it from
     * on_frame_input while paused). Returns true once the new ROM is
     * installed. */
    bool load_rom(const std::string & path);

    /* PSP pad handling: called by the worker thread right before
     * every machine frame (50 Hz), independently of how fast the
     * display thread presents pictures, so button presses never
     * stall with the rendering. Also called at the same rate while
     * paused, so the MAIN MENU stays operable with the machine
     * frozen. */
    std::function<void()> on_frame_input;

    /* Called once by the worker when the auto-open delay expires
     * (AUTO_MENU_OPEN_DELAY_US after worker start, the first frame
     * already published): the UI opens the MAIN MENU over the boot
     * ROM picture and pauses the machine, as if START was pressed.
     * Left unwired in AUTOSELECT_ROM test builds. */
    std::function<void()> on_auto_open_menu;

    /* Freeze/thaw the machine without touching the worker thread:
     * no Board/CPU/Memory/IO/PixelFiller/Soundnik work and no new
     * frames are published while paused. */
    void pause();
    void resume();
    bool is_paused() const;

    /* Save-state directory key of the running ROM (see rom_base). */
    const std::string & get_rom_base() const { return this->rom_base; }

    /* Full path of the running ROM file; empty while the boot
     * loader runs (see rom_path). */
    const std::string & get_rom_path() const { return this->rom_path; }

public:
    Emulator(Board & borat);
    virtual ~Emulator();
    void run_event_loop();
    void start_emulator_thread();
    void stop_emulator_thread();

    void export_memory_bytes(uint8_t * dst, int addr, int size);
        
    void save_state(vector<uint8_t> & to);
    bool restore_state(vector<uint8_t> & to);
    void set_bootrom(const vector<uint8_t>& bootbytes);
};
