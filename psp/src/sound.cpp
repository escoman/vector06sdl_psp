#include <algorithm>
#include <cstring>
#include "8253.h"
#include "ay.h"
#include "wav.h"
#include "options.h"
#include "sound.h"
#include "resampler.h"
#include "debuglog.h"

#include <pspaudiolib.h>
#include <pspaudio.h>
#include <pspkernel.h>

/* PSP Audio: 44100 Hz stereo, 16-bit */
#define PSP_AUDIO_SAMPLE_RATE 44100
#define PSP_AUDIO_CHANNELS 2

static bool audio_initialized = false;

void Soundnik::init(WavRecorder * _rec_internal, WavRecorder * _rec_callback)
{
    this->rec_internal = _rec_internal;
    this->rec_callback = _rec_callback;

    if (Options.nosound) {
        return;
    }

    this->sampleRate = PSP_AUDIO_SAMPLE_RATE;
    this->sound_frame_size = this->sampleRate / 50;

    this->cps_whole = SOUND_CLOCK_RATE / this->sampleRate;
    this->cps_frac_num = SOUND_CLOCK_RATE % this->sampleRate;
    this->cps_frac_acc = 0;
    this->inv_dt_lo = 1.0f / (float)this->cps_whole;
    this->inv_dt_hi = 1.0f / (float)(this->cps_whole + 1);

    /* Initialize PSP Audio */
    if (!audio_initialized) {
        pspAudioInit();
        pspAudioSetChannelCallback(0, Soundnik::callback, (void *)this);
        audio_initialized = true;
    }
}

void Soundnik::pause(int pause)
{
    if (!Options.nosound) {
        /* PSP audio doesn't have a simple pause; drain the ring */
    }
    this->rd_frame = this->wr_total.load(std::memory_order_relaxed);
    this->rd_frac = 0;
    this->step_frac = Soundnik::STEP_ONE;
    this->rate_int = 0;
    if (this->sound_frame_size > 0) {
        this->rdbuf = (int)(this->rd_frame
            % (uint64_t)(NBUFFERS * this->sound_frame_size))
            / this->sound_frame_size;
        this->rdpos = (int)(this->rd_frame
            % (uint32_t)this->sound_frame_size);
    }
}

/* Called by the PSP audio thread.
 * PSP Audio expects 16-bit signed stereo samples.
 *
 * The ring holds float stereo frames produced by process_frame(); the
 * hardware always pulls reqn frames per call at a fixed 44100 Hz. The
 * generator rate follows the machine, which under load runs slightly
 * slower than wall clock, so the callback resamples the ring with an
 * adaptive fractional step around 1.0 (dynamic rate control): when the
 * fill level drops below the target the step slows down, when it grows
 * the step speeds up. The output is continuous at exactly the hardware
 * rate; nothing is ever repeated or skipped in chunks. */
void Soundnik::callback(void * buf, unsigned int reqn, void * pdata)
{
    Soundnik * that = (Soundnik *)pdata;
    short * sstream = (short *)buf;
    const int sample_count = (int)reqn; /* stereo frames requested */

    const uint64_t wr = that->wr_total.load(std::memory_order_acquire);
    const int frame_size = that->sound_frame_size;

    /* PI controller on the fill level. A sustained machine-rate
     * deficit (the worker below 50 fps) can only be absorbed by the
     * integrator: the proportional part damps, the integral part
     * settles the step at whatever ratio keeps the ring at the target.
     * Gains give a critically damped loop with ~10 rad/s bandwidth;
     * all math is fixed-point integer. */
    const int err = (int)(wr - that->rd_frame)
        - (int)Soundnik::TARGET_FILL;
    that->rate_int += err;
    /* Clamp the integrator to the step range (anti-windup) */
    if (that->rate_int < -(int64_t)5040000) that->rate_int = -(int64_t)5040000;
    if (that->rate_int >  (int64_t)1580000) that->rate_int =  (int64_t)1580000;
    int step = (int)Soundnik::STEP_ONE
        + ((err * 30) >> 10)                       /* P: ~3% of range */
        + (int)((that->rate_int * 149) >> 16);     /* I */
    if (step < (int)Soundnik::STEP_MIN) step = (int)Soundnik::STEP_MIN;
    if (step > (int)Soundnik::STEP_MAX) step = (int)Soundnik::STEP_MAX;
    that->step_frac = (uint32_t)step;

    for (int i = 0; i < sample_count; ++i) {
        float samp;
        if (that->rd_frame >= wr) {
            /* The machine has not generated this sample yet: hold the
             * level and re-anchor right behind the writer, keeping the
             * step at its catch-up maximum so the ring refills. */
            samp = that->last_value;
            ++that->underrun_frames;
            that->rd_frame = wr > 0 ? wr - 1 : 0;
            that->rd_frac = 0;
            that->rdbuf = (int)(that->rd_frame
                % (uint64_t)(NBUFFERS * frame_size)) / frame_size;
            that->rdpos = (int)(that->rd_frame % (uint32_t)frame_size);
        } else {
            /* Linear interpolation between consecutive ring frames,
             * read strictly below the writer position. */
            const float s0 = that->buffer[that->rdbuf][that->rdpos * 2];
            uint64_t next = that->rd_frame + 1;
            float s1;
            if (next < wr) {
                int nb = that->rdbuf, np = that->rdpos + 1;
                if (np >= frame_size) {
                    np = 0;
                    if (++nb == NBUFFERS) nb = 0;
                }
                s1 = that->buffer[nb][np * 2];
            } else {
                s1 = s0; /* do not peek past the writer */
            }
            const uint32_t frac = that->rd_frac;
            samp = s0 + (s1 - s0) * ((float)frac * (1.0f / 65536.0f));
            that->last_value = samp;

            that->rd_frac += that->step_frac;
            uint32_t adv = that->rd_frac >> 16;
            that->rd_frac &= 0xffffu;
            that->rd_frame += adv;
            that->rdpos += (int)adv;
            while (that->rdpos >= frame_size) {
                that->rdpos -= frame_size;
                if (++that->rdbuf == NBUFFERS) that->rdbuf = 0;
            }
        }

        /* Convert float (-1..1) to 16-bit signed */
        int v = (int)(samp * 32767.0f);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        sstream[i * 2] = (short)v;
        sstream[i * 2 + 1] = (short)v;
    }

    /* Diagnostic: capture exactly the interleaved 16-bit stereo frames
     * the callback hands to the PSP audio hardware. */
    if (that->rec_callback != 0) {
        that->rec_callback->record_shorts(
            sstream, (size_t)sample_count * PSP_AUDIO_CHANNELS);

        /* Once-a-second health line (only while recording): fill level,
         * playback step in %, and underrun padding since the last line */
        static uint32_t last_report_us = 0;
        const uint32_t now = sceKernelGetSystemTimeLow();
        if ((uint32_t)(now - last_report_us) >= 1000000) {
            last_report_us = now;
            dbglog("snd_cb: fill=%d step=%u.%02u%% underrun=%u\n",
                   (int)(that->wr_total.load(std::memory_order_relaxed)
                         - that->rd_frame),
                   (unsigned)(that->step_frac * 100 / Soundnik::STEP_ONE),
                   (unsigned)((that->step_frac * 10000 / Soundnik::STEP_ONE) % 100),
                   (unsigned)that->underrun_frames);
            that->underrun_frames = 0;
        }
    }
}

void Soundnik::sample(float samp)
{
    if (!Options.nosound) {
        this->last_value = samp;
        const int frame_size = this->sound_frame_size;
        const uint64_t pos =
            this->wr_total.load(std::memory_order_relaxed);
        const int wb = (int)(pos
            % (uint64_t)(NBUFFERS * frame_size)) / frame_size;
        const int wp = (int)(pos % (uint32_t)frame_size);
        this->buffer[wb][wp * 2] = samp;
        this->buffer[wb][wp * 2 + 1] = samp;
        this->wr_total.fetch_add(1, std::memory_order_release);

        /* Diagnostic: capture the sample at the Soundnik -> ring buffer
         * boundary, converted with the exact same float->short math the
         * audio callback uses, so the two recordings are comparable. */
        if (this->rec_internal != 0) {
            int v = (int)(samp * 32767.0f);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            int16_t pair[2] = { (int16_t)v, (int16_t)v };
            this->rec_internal->record_shorts(pair, 2);
        }
    }
}

/* --- event-based block generation --- */

void Soundnik::push_event(SoundEventType type, uint8_t addr, uint8_t value)
{
    SoundEvent e;
    e.clock = this->sound_clock;
    e.type = type;
    e.addr = addr;
    e.value = value;
    this->events.push(e);
}

void Soundnik::apply_event(const SoundEvent & e)
{
    switch (e.type) {
        case SoundEventType::TimerReg:
            this->apply_timer_write(e.addr, e.value);
            break;
        case SoundEventType::AyReg:
            this->mirror_ay.write(e.addr, e.value);
            break;
        case SoundEventType::TapeOut:
            this->tapeout_level = e.value ? 1 : 0;
            break;
        case SoundEventType::TapeIn:
            this->tapein_level = e.value ? 1 : 0;
            break;
        case SoundEventType::Covox:
            this->covox_level = e.value;
            this->covox_norm = ((int)e.value - 255) * (1.0f / 256.0f);
            break;
    }
}

/* Replay an 8253 write on the timer mirror. Tracks only what is needed
 * for sound generation (mode, load value, output phase); the fine
 * read/latch delays of CounterUnit are skipped because they are
 * inaudible. */
void Soundnik::apply_timer_write(int addr, uint8_t w8)
{
    if (addr == 3) {
        /* control word */
        int counter = (w8 >> 6) & 3;
        if (counter >= 3) {
            return; /* read-back command: not modeled */
        }
        int latch = (w8 >> 4) & 3;
        if (latch == 0) {
            return; /* counter latch command: only affects reads */
        }

        TimerChannel & ch = this->timer_ch[counter];
        int m = (w8 >> 1) & 3;
        ch.mode = (m == 2) ? 2 : (m == 3) ? 3 : m;
        ch.bcd = (w8 & 1) != 0;
        ch.latch_mode = latch;
        ch.write_state = 0;
        ch.enabled = false;
        ch.out = (ch.mode == 0) ? 0 : 1;
        ch.phase = 0;
        ch.remain = 0;
        return;
    }

    if (addr < 0 || addr >= 3) {
        return;
    }
    TimerChannel & ch = this->timer_ch[addr];
    if (ch.latch_mode == 0) {
        return;
    }

    int loadvalue = -1;
    if (ch.latch_mode == 3) {
        if (ch.write_state == 0) {
            ch.write_lsb = w8;
            ch.write_state = 1;
            return;
        }
        ch.write_state = 0;
        loadvalue = (w8 << 8) | ch.write_lsb;
    } else if (ch.latch_mode == 1) {
        loadvalue = w8;
    } else {
        loadvalue = w8 << 8;  /* latch_mode 2: msb only */
    }

    if (ch.bcd) {
        loadvalue = CounterUnit::frombcd((uint16_t)loadvalue);
    }
    ch.loadvalue = loadvalue;
    ch.enabled = true;
    ch.phase = 0;
    if (ch.mode == 0) {
        ch.out = 0;
        ch.remain = loadvalue;
    }
}

/* Number of clocks (out of dt) the timer channel output spends high.
 * Advances the channel state. All-integer math: the PSP FPU emulates
 * 64-bit doubles in software, so doubles here were 5 us per call. */
int Soundnik::integrate_timer(int n, int dt)
{
    TimerChannel & ch = this->timer_ch[n];

    if (!ch.enabled) {
        return ch.out ? dt : 0;
    }

    switch (ch.mode) {
        case 3: {
            /* square wave: output toggles every loadvalue/2 clocks */
            int period = ch.loadvalue ? ch.loadvalue : 65536;
            int half = period / 2;
            if (half < 1) half = 1;

            int high = 0;
            int rem = dt;
            while (rem > 0) {
                int to_toggle = half - ch.phase;
                if (to_toggle > rem) {
                    if (ch.out) high += rem;
                    ch.phase += rem;
                    rem = 0;
                } else {
                    if (ch.out) high += to_toggle;
                    rem -= to_toggle;
                    ch.out ^= 1;
                    ch.phase = 0;
                }
            }
            return high;
        }
        case 2:
            /* rate generator: one-clock low pulse per period. The pulse
             * is inaudible; keep the DC level like the old model. */
            return dt;
        case 0: {
            /* interrupt on terminal count: output rises once after the
             * countdown and stays high */
            if (ch.out) {
                return dt;
            }
            if (ch.remain > dt) {
                ch.remain -= dt;
                return 0;
            }
            int t = dt - ch.remain;
            ch.remain = 0;
            ch.out = 1;
            return t;
        }
        default:
            /* modes 1, 4, 5: approximate with the constant level */
            return ch.out ? dt : 0;
    }
}

/* Step the AY mirror for dt sound clocks and return the averaged output.
 * Tick rate matches the legacy AYWrapper::step2(): 14 accumulator units
 * per 1.5 MHz clock, one chip step per 96 units (~218.75 kHz). The chip
 * runs its integer state machine (step_int) and only the averaged
 * result is converted to float: ~150k float-heavy steps per second
 * were 95 ms/s on the PSP. */
float Soundnik::step_ay(int dt, int ena0, int ena1, int ena2)
{
    this->ay_accu += dt * 14;
    int steps = this->ay_accu / 96;
    this->ay_accu -= steps * 96;

    if (steps == 0) {
        return this->ay_last;
    }

#ifdef AUTOSELECT_ROM
    unsigned perf_a0 = sceKernelGetSystemTimeLow();
#endif
    int acc = 0;
    for (int i = 0; i < steps; ++i) {
        acc += this->mirror_ay.step_int(ena0, ena1, ena2);
    }
#ifdef AUTOSELECT_ROM
    this->perf_ay_us += sceKernelGetSystemTimeLow() - perf_a0;
    this->perf_naysteps += steps;
#endif
    /* steps is 1..5 for dt = 33..34 clocks; avoid the float division.
     * The 1/4096 undoes the integer amplitude scaling of amp_int. */
    static const float rcp_steps[9] = {
        0.0f, 1.0f/4096, 1.0f/(2*4096), 1.0f/(3*4096), 1.0f/(4*4096),
        1.0f/(5*4096), 1.0f/(6*4096), 1.0f/(7*4096), 1.0f/(8*4096) };
    this->ay_last = (steps <= 8)
        ? (float)acc * rcp_steps[steps]
        : (float)acc / (float)(steps * 4096);
    return this->ay_last;
}

/* Length of the next output sample in sound clocks. Accumulates the
 * fractional part of SOUND_CLOCK_RATE / sampleRate Bresenham-style, so
 * samples are 33 or 34 clocks and average out exactly. */
int Soundnik::next_sample_dt()
{
    int dt = this->cps_whole;
    this->cps_frac_acc += this->cps_frac_num;
    if (this->cps_frac_acc >= this->sampleRate) {
        this->cps_frac_acc -= this->sampleRate;
        dt += 1;
    }
    return dt;
}

/* Render output samples for all clocks executed since the previous call.
 * Typically once per frame: ~29952 clocks -> 882 samples at 44100 Hz. */
void Soundnik::process_frame()
{
    if (Options.nosound) {
        /* keep the queue drained so it cannot overflow */
        this->events.clear();
        return;
    }

    int ech0 = Options.enable.timer_ch0,
        ech1 = Options.enable.timer_ch1,
        ech2 = Options.enable.timer_ch2,
        aych0 = Options.enable.ay_ch0,
        aych1 = Options.enable.ay_ch1,
        aych2 = Options.enable.ay_ch2;

    /* Snapshot the volumes once: they do not change inside a frame,
     * and folding global in here removes a multiply per sample. */
    const float vol_timer = Options.volume.timer * Options.volume.global;
    const float vol_ay = Options.volume.ay * Options.volume.global;
    const float vol_beeper = Options.volume.beeper * Options.volume.global;
    const float vol_covox = Options.volume.covox * Options.volume.global;

    int budget = 8 * this->sound_frame_size; /* max samples per call */

    while (this->next_sample_clock <= this->sound_clock
            && budget-- > 0) {
        const uint64_t t1 = this->next_sample_clock;

        /* apply everything the CPU committed up to this sample boundary */
#ifdef AUTOSELECT_ROM
        unsigned perf_e0 = sceKernelGetSystemTimeLow();
#endif
        while (!this->events.empty()
                && this->events.peek().clock <= t1) {
            this->apply_event(this->events.peek());
            this->events.pop();
        }
#ifdef AUTOSELECT_ROM
        unsigned perf_e1 = sceKernelGetSystemTimeLow();
        this->perf_ev_us += perf_e1 - perf_e0;
#endif

        const int dt = this->next_sample_dt();

#ifdef AUTOSELECT_ROM
        unsigned perf_t0 = sceKernelGetSystemTimeLow();
#endif
        const int high =
            this->integrate_timer(0, dt) * ech0 +
            this->integrate_timer(1, dt) * ech1 +
            this->integrate_timer(2, dt) * ech2;
#ifdef AUTOSELECT_ROM
        unsigned perf_t1 = sceKernelGetSystemTimeLow();
        this->perf_tmr_us += perf_t1 - perf_t0;
#endif

        const float inv_dt = (dt == this->cps_whole)
            ? this->inv_dt_lo : this->inv_dt_hi;

#ifdef AUTOSELECT_ROM
        unsigned perf_m0 = sceKernelGetSystemTimeLow();
#endif
        float s = (float)high * inv_dt * vol_timer
            + this->step_ay(dt, aych0, aych1, aych2) * vol_ay
            + (this->tapeout_level + this->tapein_level) * vol_beeper
            + vol_covox * this->covox_norm;

        if (s < -1.0f) s = -1.0f;
        if (s > 1.0f) s = 1.0f;
        this->sample(s);
#ifdef AUTOSELECT_ROM
        this->perf_mix_us += sceKernelGetSystemTimeLow() - perf_m0;
        ++this->perf_nsamples;
#endif

        this->next_sample_clock += dt;
    }

    if (this->next_sample_clock <= this->sound_clock) {
        /* fell too far behind: skip ahead, replaying queued events so the
         * chip mirrors stay in sync with the CPU */
        while (!this->events.empty()
                && this->events.peek().clock <= this->sound_clock) {
            this->apply_event(this->events.peek());
            this->events.pop();
        }
        this->next_sample_clock = this->sound_clock;
    }
}

void Soundnik::reset()
{
    this->timerwrapper.reset();
    this->aywrapper.reset();

    this->events.clear();
    this->sound_clock = 0;
    this->cps_frac_acc = 0;
    this->next_sample_clock = 0;
    this->reset_mirrors();
}

void Soundnik::reset_mirrors()
{
    this->mirror_ay.init();
    this->ay_accu = 0;
    this->ay_last = 0;

    for (int i = 0; i < 3; ++i) {
        TimerChannel & ch = this->timer_ch[i];
        ch.mode = 0;
        ch.latch_mode = 0;
        ch.bcd = false;
        ch.write_state = 0;
        ch.write_lsb = 0;
        ch.loadvalue = 0;
        ch.enabled = false;
        ch.out = 0;
        ch.phase = 0;
        ch.remain = 0;
    }

    /* levels at PIA/PPI power-up defaults (PC = 0xff, PA2 = 0xff) */
    this->tapeout_level = 1;
    this->tapein_level = 0;
    this->covox_level = 0xff;
    this->covox_norm = 0.0f;
}
