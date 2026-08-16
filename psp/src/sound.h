#pragma once

#include <atomic>
#include <cstdint>
#include "globaldefs.h"
#include "8253.h"
#include "ay.h"
#include "resampler.h"
#include "sound_filters.h"
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

    /* Total stereo frames written into the ring (worker thread).
     * The callback derives every ring position from this counter, so
     * it can never read ahead of the writer or mistake a stale buffer
     * for fresh data. */
    std::atomic<uint64_t> wr_total;

    /* Adaptive consumption (audio callback thread only). The hardware
     * always pulls 44100 frames per wall-clock second while the
     * generator follows the machine, which can run slightly slower
     * than real time. The callback therefore reads the ring with a
     * fractional step around 1.0, steered by the fill level (dynamic
     * rate control): the output stays continuous instead of padding
     * underruns with repeats. All fixed-point integer math. */
    static const uint32_t STEP_ONE = 65536;
    static const uint32_t STEP_MIN = 54067;   /* ~0.825x: heavy slowdown */
    static const uint32_t STEP_MAX = 69134;   /* ~1.055x: catch up     */
    static const uint32_t STEP_RANGE = STEP_MAX - STEP_MIN;
    static const uint32_t TARGET_FILL = 1764; /* 40 ms of sound, default */
    uint32_t target_fill; /* runtime value, from sound_buffer_ms */
    /* Waveform reconstruction kernel of the callback resampler
     * (config.ini: sound_mode). Selected once in init(), before the
     * audio callback is registered; the hot loop only sees the enum.
     * None = the historical linear two-point reconstruction. */
    SoundMode sound_mode;
    uint64_t rd_frame;   /* next ring frame to read (resampled pos) */
    uint32_t rd_frac;    /* fractional phase of the resampler */
    uint32_t step_frac;  /* playback step per output frame */
    int64_t  rate_int;   /* integrator of the fill error, Q16 */
    int rdbuf, rdpos;    /* derived ring position, kept incremental */
    float last_value;

    /* Diagnostic counters, reported once a second while recording */
    uint32_t underrun_frames;

    /* Latency diagnostics (ring sojourn time). The writer stamps each
     * sound_frame_size-sized block when it starts writing it; the
     * callback compares the stamp of the block being read with the
     * current time. The ring holds NBUFFERS blocks, so while fill
     * stays below the capacity the stamp of the read block is valid. */
    uint32_t wr_block_ts[NBUFFERS];

    /* Run-wide statistics, reported by report_stats() at shutdown.
     * All counters are written by a single thread each (fill/step/
     * rate_int/latency/underrun by the audio callback, pf_* by the
     * worker), so plain integers are enough. */
    uint64_t stat_fill_min, stat_fill_max, stat_fill_sum, stat_fill_n;
    uint32_t stat_step_min, stat_step_max;
    uint64_t stat_step_sum;
    uint32_t stat_step_n;
    uint32_t stat_step_at_min, stat_step_at_max, stat_step_not_one;
    int64_t stat_rint_min, stat_rint_max;
    uint32_t stat_underrun_total;
    uint32_t stat_underrun_run, stat_underrun_max_run;
    uint64_t stat_lat_min, stat_lat_max, stat_lat_sum, stat_lat_n;
    uint32_t pf_last_us;                       /* previous process_frame */
    uint32_t stat_pf_min, stat_pf_max;
    uint64_t stat_pf_sum;
    uint32_t stat_pf_n;

    int sampleRate;

    int sound_accu_top;

    /* Kept for interface compatibility; the block generator does not
     * use the FIR resampler. */
    Resampler resampler;
    /* Diagnostic recorders (sound_record = true in config.ini):
     * rec_internal receives the samples at the Soundnik -> ring buffer
     * boundary (worker thread), rec_callback receives exactly what the
     * PSP audio callback hands to the hardware (audio thread). Both are
     * null in the normal run; never touched when null. */
    WavRecorder * rec_internal;
    WavRecorder * rec_callback;

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
        aywrapper(aw), wr_total(0),
        rd_frame(0), rd_frac(0), step_frac(STEP_ONE), rate_int(0),
        rdbuf(0), rdpos(0),
        last_value(0.0f), underrun_frames(0),
        target_fill(TARGET_FILL),
        sound_mode(SoundMode::None),
        stat_fill_min(UINT64_MAX), stat_fill_max(0),
        stat_fill_sum(0), stat_fill_n(0),
        stat_step_min(UINT32_MAX), stat_step_max(0),
        stat_step_sum(0), stat_step_n(0),
        stat_step_at_min(0), stat_step_at_max(0), stat_step_not_one(0),
        stat_rint_min(INT64_MAX), stat_rint_max(INT64_MIN),
        stat_underrun_total(0), stat_underrun_run(0),
        stat_underrun_max_run(0),
        stat_lat_min(UINT64_MAX), stat_lat_max(0),
        stat_lat_sum(0), stat_lat_n(0),
        pf_last_us(0), stat_pf_min(UINT32_MAX), stat_pf_max(0),
        stat_pf_sum(0), stat_pf_n(0),
        sampleRate(0), sound_accu_top(0),
        rec_internal(0), rec_callback(0),
        sound_clock(0), next_sample_clock(0),
        cps_whole(SOUND_CLOCK_RATE / 44100),
        cps_frac_num(SOUND_CLOCK_RATE % 44100), cps_frac_acc(0),
        inv_dt_lo(1.0f / (SOUND_CLOCK_RATE / 44100)),
        inv_dt_hi(1.0f / (SOUND_CLOCK_RATE / 44100 + 1)),
        covox_norm(0.0f),
        ay_accu(0), ay_last(0),
        tapeout_level(1), tapein_level(0), covox_level(0xff)
    {
        for (int i = 0; i < NBUFFERS; ++i) {
            this->wr_block_ts[i] = 0;
        }
        this->reset_mirrors();
    }

    void init(WavRecorder * _rec_internal = 0,
              WavRecorder * _rec_callback = 0);
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

    /* Dump the run-wide sound statistics into debug.log (shutdown). */
    void report_stats();

#ifdef AUTOSELECT_ROM
    /* process_frame sub-stage breakdown (test builds only) */
    unsigned perf_ev_us = 0, perf_tmr_us = 0, perf_ay_us = 0, perf_mix_us = 0;
    unsigned perf_nsamples = 0, perf_naysteps = 0;
#endif

    void reset();
};
