#include <algorithm>
#include <cstring>
#include "8253.h"
#include "ay.h"
#include "wav.h"
#include "options.h"
#include "sound.h"
#include "resampler.h"

#include <pspaudiolib.h>
#include <pspaudio.h>

/* PSP Audio: 44100 Hz stereo, 16-bit */
#define PSP_AUDIO_SAMPLE_RATE 44100
#define PSP_AUDIO_CHANNELS 2

/* Sound chip clock: 12 MHz pixel clock / 8 = 1497600 Hz.
 * Board advances sound_clock in these units via advance_clock(). */
#define SOUND_CLOCK_RATE (50*768*312/8)

static bool audio_initialized = false;

void Soundnik::init(WavRecorder * _rec)
{
    this->rec = _rec;

    if (Options.nosound) {
        return;
    }

    this->sampleRate = PSP_AUDIO_SAMPLE_RATE;
    this->sound_frame_size = this->sampleRate / 50;

    this->clocks_per_sample = (double)SOUND_CLOCK_RATE / this->sampleRate;
    if (this->next_sample_clock <= 0.0) {
        this->next_sample_clock = this->clocks_per_sample;
    }

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
        /* PSP audio doesn't have a simple pause; clear buffers */
    }
    this->wrptr = 0;
    this->rdbuf = 0;
    this->wrbuf = 0;
}

/* Called by PSP audio thread.
 * PSP Audio expects 16-bit signed stereo samples.
 * We keep float samples in our ring buffer and convert to short here. */
void Soundnik::callback(void * buf, unsigned int reqn, void * pdata)
{
    Soundnik * that = (Soundnik *)pdata;
    short * sstream = (short *)buf;
    int sample_count = reqn; /* number of stereo frames requested */

    /* Fill the requested number of frames */
    for (int i = 0; i < sample_count; ++i) {
        float samp;
        if (that->rdbuf == that->wrbuf && that->wrptr == 0) {
            /* Buffer empty - output last value */
            samp = that->last_value;
        } else {
            samp = that->buffer[that->rdbuf][that->rdpos++];
            if (that->rdpos >= that->sound_frame_size * PSP_AUDIO_CHANNELS) {
                that->rdpos = 0;
                if (++that->rdbuf == Soundnik::NBUFFERS) {
                    that->rdbuf = 0;
                }
            }
        }
        /* Convert float (-1..1) to 16-bit signed */
        int v = (int)(samp * 32767.0f);
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        sstream[i * 2] = (short)v;
        sstream[i * 2 + 1] = (short)v;
    }

    that->rec &&
        that->rec->record_buffer((float *)sstream, sample_count * PSP_AUDIO_CHANNELS);
}

void Soundnik::sample(float samp)
{
    if (!Options.nosound) {
        this->last_value = samp;
        this->buffer[this->wrbuf][this->wrptr++] = samp;
        this->buffer[this->wrbuf][this->wrptr++] = samp;
        if (this->wrptr >= this->sound_frame_size * PSP_AUDIO_CHANNELS) {
            this->wrptr = 0;
            if (++this->wrbuf == Soundnik::NBUFFERS) {
                this->wrbuf = 0;
            }
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

void Soundnik::advance_clock(int nclk1m5)
{
    this->sound_clock += (uint64_t)nclk1m5;
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

/* Average output level of one timer channel over dt sound clocks,
 * advancing the channel state. */
double Soundnik::integrate_timer(int n, double dt)
{
    TimerChannel & ch = this->timer_ch[n];

    if (!ch.enabled) {
        return ch.out;
    }

    switch (ch.mode) {
        case 3: {
            /* square wave: output toggles every loadvalue/2 clocks */
            int period = ch.loadvalue ? ch.loadvalue : 65536;
            double half = period / 2.0;
            if (half < 1.0) half = 1.0;

            double high = 0;
            double remaining = dt;
            while (remaining > 1e-9) {
                double to_toggle = half - ch.phase;
                if (to_toggle > remaining) {
                    if (ch.out) high += remaining;
                    ch.phase += remaining;
                    remaining = 0;
                } else {
                    if (ch.out) high += to_toggle;
                    remaining -= to_toggle;
                    ch.out ^= 1;
                    ch.phase = 0;
                }
            }
            return high / dt;
        }
        case 2:
            /* rate generator: one-clock low pulse per period. The pulse
             * is inaudible; keep the DC level like the old model. */
            return 1.0;
        case 0: {
            /* interrupt on terminal count: output rises once after the
             * countdown and stays high */
            if (ch.out) {
                return 1.0;
            }
            if (ch.remain > dt) {
                ch.remain -= dt;
                return 0.0;
            }
            double t = dt - ch.remain;
            ch.remain = 0;
            ch.out = 1;
            return t / dt;
        }
        default:
            /* modes 1, 4, 5: approximate with the constant level */
            return ch.out;
    }
}

/* Step the AY mirror for dt sound clocks and return the averaged output.
 * Tick rate matches the legacy AYWrapper::step2(): 14 accumulator units
 * per 1.5 MHz clock, one chip step per 96 units (~218.75 kHz). */
float Soundnik::step_ay(double dt, int ena0, int ena1, int ena2)
{
    this->ay_accu += dt * 14.0;
    int steps = (int)(this->ay_accu / 96.0);
    this->ay_accu -= steps * 96.0;

    if (steps == 0) {
        return this->ay_last;
    }

    float acc = 0;
    for (int i = 0; i < steps; ++i) {
        acc += this->mirror_ay.step(ena0, ena1, ena2);
    }
    this->ay_last = acc / steps;
    return this->ay_last;
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

    int budget = 8 * this->sound_frame_size; /* max samples per call */

    while (this->next_sample_clock <= (double)this->sound_clock
            && budget-- > 0) {
        const double t1 = this->next_sample_clock;

        /* apply everything the CPU committed up to this sample boundary */
        while (!this->events.empty()
                && (double)this->events.peek().clock <= t1) {
            this->apply_event(this->events.peek());
            this->events.pop();
        }

        const double dt = this->clocks_per_sample;

        float s = (float)(
                this->integrate_timer(0, dt) * ech0 +
                this->integrate_timer(1, dt) * ech1 +
                this->integrate_timer(2, dt) * ech2) * Options.volume.timer
            + this->step_ay(dt, aych0, aych1, aych2) * Options.volume.ay
            + (this->tapeout_level + this->tapein_level) * Options.volume.beeper
            + Options.volume.covox * ((this->covox_level - 255) / 256.0f);

        s *= Options.volume.global;
        if (s < -1.0f) s = -1.0f;
        if (s > 1.0f) s = 1.0f;
        this->sample(s);

        this->next_sample_clock += this->clocks_per_sample;
    }

    if (this->next_sample_clock <= (double)this->sound_clock) {
        /* fell too far behind: skip ahead, replaying queued events so the
         * chip mirrors stay in sync with the CPU */
        while (!this->events.empty()
                && (double)this->events.peek().clock
                    <= (double)this->sound_clock) {
            this->apply_event(this->events.peek());
            this->events.pop();
        }
        this->next_sample_clock = (double)this->sound_clock;
    }
}

void Soundnik::reset()
{
    this->timerwrapper.reset();
    this->aywrapper.reset();

    this->events.clear();
    this->sound_clock = 0;
    this->next_sample_clock = this->clocks_per_sample;
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
}
