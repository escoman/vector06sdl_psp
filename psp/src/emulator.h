#pragma once

#include "event.h"
#include "board.h"

class Emulator {
    static const int N_SCANCODES = 4;

private:
    enum event_type {
        EXECUTE_FRAME,
        KEYDOWN,
        KEYUP,
        JOY,
        QUIT,
        RENDER,
        VACANT
    };

    struct threadevent {
        event_type type;
        int data;
        int frame_no;
        SDL_KeyboardEvent key;
        threadevent() {}
        threadevent(event_type t, int d) : type(t), data(d) {}
        threadevent(event_type t, int d, int frameno) : type(t), data(d),
            frame_no(frameno) {}
        threadevent(event_type t, SDL_KeyboardEvent k) : 
            type(t), data(0), key(k) {}

        bool operator <(const threadevent& other) const
        {
            return false;
        }
    };

    Board & board;

    threadevent ui_to_engine_event;
    threadevent engine_to_ui_event;
    int keydowns[N_SCANCODES];
    int keyups[N_SCANCODES];

public:
    /* returns 1 if a machine frame was executed, 0 on cadence skip */
    int execute_frame();
    void keydown(int scancode);
    void keyup(int scancode);
    void set_joysticks(int joy_0e, int joy_0f);
    void set_volumes(float timer, float beeper, float ay, float covox, float master);
    void enable_timer_channels(bool ech0, bool ech1, bool ech2);
    void enable_ay_channels(bool ech0, bool ech1, bool ech2);
    void export_pixel_bytes(uint8_t * dst);
    void export_audio_frame(float * dst, size_t count);
    size_t pixel_bytes_size();

public:
    Emulator(Board & borat);
    virtual ~Emulator();
    void run_event_loop();
    void start_emulator_thread();

    void export_memory_bytes(uint8_t * dst, int addr, int size);
        
    void save_state(vector<uint8_t> & to);
    bool restore_state(vector<uint8_t> & to);
    void set_bootrom(const vector<uint8_t>& bootbytes);
};
