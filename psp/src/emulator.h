#pragma once

#include <atomic>
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

    /* Main -> Worker input handoff. One slot per concurrently tracked
     * key, claimed with CAS by the main thread and emptied by the
     * worker before every machine frame; no other locking. */
    std::atomic<int> keydowns[N_SCANCODES];
    std::atomic<int> keyups[N_SCANCODES];

    /* Main -> Worker command slots (reset etc.) */
    std::atomic<int> commands[N_COMMANDS];

    /* Worker thread */
    int worker_thid;
    std::atomic<bool> worker_running;
    std::atomic<bool> worker_stop_req;

    /* Worker wall-clock pacing: one machine frame every 20 ms. */
    static const unsigned FRAME_PERIOD_US = 20000;
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
