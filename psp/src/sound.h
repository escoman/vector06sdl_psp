#pragma once

#include <atomic>
#include "globaldefs.h"
#include "8253.h"
#include "ay.h"
#include "resampler.h"
#include "wav.h"
#include "sound_events.h"

/* Sound chip clock: 12 MHz pixel clock / 8 = 1497600 Hz.
 * Board advances sound_clock in these units via advance_clock(). */
#define SOUND_CLOCK_RATE (50*768*312/8)

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

    /* Integer timebase: the current sample ends at next_sample_clock
     * sound clocks. Sample length is SOUND_CLOCK_RATE / sampleRate with
     * the fractional part accumulated Bresenham-style (33 or 34 clocks
     * per sample). Everything is integer because the PSP FPU only does
     * single precision; 64-bit doubles are emulated in software. */
    uint64_t next_sample_clock;
    int cps_whole;
    int cps_frac_num;
    int cps_frac_acc;
    /* dt is always cps_whole or cps_whole+1 (Bresenham), so the hot
     * loop multiplies by a cached reciprocal instead of dividing. */
    float inv_dt_lo, inv_dt_hi;
    /* (covox_level - 255) / 256, updated only on Covox events */
    float covox_norm;

    /* Chip mirrors. IO keeps writing into the real chips so CPU reads
     * stay correct; the mirrors are replayed from the event queue at
     * render time with correct timing. */
    AY mirror_ay;
    int ay_accu;
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
        int phase;
        int remain;
    };
    TimerChannel timer_ch[3];

    int tapeout_level;
    int tapein_level;
    int covox_level;

    void apply_event(const SoundEvent & e);
    void apply_timer_write(int addr, uint8_t w8);
    /* Number of clocks (out of dt) the channel output spends high */
    int integrate_timer(int ch, int dt);
    float step_ay(int dt, int ena0, int ena1, int ena2);
    int next_sample_dt();
    void reset_mirrors();

public:
    Soundnik(TimerWrapper & tw, AYWrapper & aw) : timerwrapper(tw),
        aywrapper(aw), wrptr(0), wrbuf(0), rdbuf(0), rdpos(0),
        last_value(0.0f), sampleRate(0), sound_accu_top(0), rec(0),
        sound_clock(0), next_sample_clock(0),
        cps_whole(SOUND_CLOCK_RATE / 44100),
        cps_frac_num(SOUND_CLOCK_RATE % 44100), cps_frac_acc(0),
        inv_dt_lo(1.0f / (SOUND_CLOCK_RATE / 44100)),
        inv_dt_hi(1.0f / (SOUND_CLOCK_RATE / 44100 + 1)),
        covox_norm(0.0f),
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

    /* Advance the sound clock (replacement for the old soundSteps).
     * Called per instruction from Board::single_step(), so it must be
     * cheap: keep it inline here. */
    void advance_clock(int nclk1m5)
    {
        this->sound_clock += (uint64_t)nclk1m5;
    }

    /* Render output samples for all clocks executed so far. Called once
     * per frame from Board::execute_frame(). */
    void process_frame();

#ifdef AUTOSELECT_ROM
    /* process_frame sub-stage breakdown (test builds only) */
    unsigned perf_ev_us = 0, perf_tmr_us = 0, perf_ay_us = 0, perf_mix_us = 0;
    unsigned perf_nsamples = 0, perf_naysteps = 0;
#endif

    void reset();
};
