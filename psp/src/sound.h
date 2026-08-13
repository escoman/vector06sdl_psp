#pragma once

#include <atomic>
#include "globaldefs.h"
#include "8253.h"
#include "ay.h"
#include "resampler.h"
#include "wav.h"
#include "sound_events.h"


class Soundnik
{
private:
    TimerWrapper & timerwrapper;
    AYWrapper & aywrapper;
    static const int buffer_size = 2048 * 2; // 96000/50=1920, enough
    int sound_frame_size = 2048;

    static const int NBUFFERS = 8;
    float buffer[NBUFFERS][buffer_size];
    static const int mask = buffer_size - 1;
    std::atomic_int wrptr;
    int wrbuf;
    int rdbuf;
    int rdpos;
    float last_value;

    int sampleRate;

    int sound_accu_top;

    /* Kept for interface compatibility; the block generator does not
     * use the FIR resampler. */
    Resampler resampler;
    WavRecorder * rec;

    /* --- event-based block sound generation --- */

    /* Timestamped chip writes collected while the CPU runs the frame. */
    SoundEventQueue events;

    /* 1.5 MHz sound clock counter, advanced by Board::single_step(). */
    uint64_t sound_clock;

    /* Sound clock value where the next output sample ends. */
    double next_sample_clock;
    double clocks_per_sample;

    /* Chip mirrors. IO keeps writing into the real chips so CPU reads
     * stay correct; the mirrors are replayed from the event queue at
     * render time with correct timing. */
    AY mirror_ay;
    double ay_accu;
    float ay_last;

    struct TimerChannel {
        int mode;
        int latch_mode;
        bool bcd;
        int write_state;
        uint8_t write_lsb;
        int loadvalue;
        bool enabled;
        int out;
        double phase;
        double remain;
    };
    TimerChannel timer_ch[3];

    int tapeout_level;
    int tapein_level;
    int covox_level;

    void apply_event(const SoundEvent & e);
    void apply_timer_write(int addr, uint8_t w8);
    double integrate_timer(int ch, double dt);
    float step_ay(double dt, int ena0, int ena1, int ena2);
    void reset_mirrors();

public:
    Soundnik(TimerWrapper & tw, AYWrapper & aw) : timerwrapper(tw),
        aywrapper(aw), wrptr(0), wrbuf(0), rdbuf(0), rdpos(0),
        last_value(0.0f), sampleRate(0), sound_accu_top(0), rec(0),
        sound_clock(0), next_sample_clock(0),
        clocks_per_sample(1497600.0 / 44100.0),
        ay_accu(0), ay_last(0),
        tapeout_level(1), tapein_level(0), covox_level(0xff)
    {
        this->reset_mirrors();
    }

    void init(WavRecorder * _rec = 0);
    void pause(int pause);
    static void callback(void * buf, unsigned int reqn, void * pdata);
    void sample(float samp);

    /* Queue a chip write at the current sound clock (called from IO). */
    void push_event(SoundEventType type, uint8_t addr, uint8_t value);

    /* Advance the sound clock (replacement for the old soundSteps). */
    void advance_clock(int nclk1m5);

    /* Render output samples for all clocks executed so far. Called once
     * per frame from Board::execute_frame(). */
    void process_frame();

    void reset();
};
