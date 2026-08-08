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

static bool audio_initialized = false;

void Soundnik::soundSteps(int nclk1m5, int tapeout, int covox, int tapein)
{
    covox = covox - 255;
    int ech0 = Options.enable.timer_ch0,
        ech1 = Options.enable.timer_ch1, 
        ech2 = Options.enable.timer_ch2,
        aych0 = Options.enable.ay_ch0,
        aych1 = Options.enable.ay_ch1,
        aych2 = Options.enable.ay_ch2;

    for (int clk = 0; clk < nclk1m5; ++clk) {
        float ay = this->aywrapper.step2(2, aych0, aych1, aych2);

        /* timerwrapper does the stepping of 8253, it must always be called */
        float soundf = (this->timerwrapper.singlestep(ech0, ech1, ech2)) * Options.volume.timer
            + (tapeout + tapein) * Options.volume.beeper
            + Options.volume.covox * (covox/256.0f)
            + Options.volume.ay * ay;

        if (!Options.nosound) {
            soundf = this->resampler.sample(soundf);

            if (resampler.egg) {
                resampler.egg = false;
                float v = soundf * Options.volume.global;
                if (v < -1.0f) v = -1.0f;
                if (v > 1.0f) v = 1.0f;
                this->sample(v);
            }
        }
    }
}

void Soundnik::init(WavRecorder * _rec)
{
    this->rec = _rec;

    if (Options.nosound) {
        return;
    }

    this->sampleRate = PSP_AUDIO_SAMPLE_RATE;
    this->sound_frame_size = this->sampleRate / 50;

    /* One second = 50 frames
     * raster time in 12 MHz pixelclocks = 768 columns by 312 lines
     * timer clocks = pixel clock / 8 */
    int timer_cycles_per_second = 50*768*312/8; // 1497600
    this->sound_accu_top = (int)(0.5 + 100.0 * timer_cycles_per_second / this->sampleRate);

    /* Filters */
    if (Options.nofilter) {
        resampler.set_passthrough(true);
    }

    /* Initialize PSP Audio */
    if (!audio_initialized && !Options.nosound) {
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

void Soundnik::reset()
{
    this->timerwrapper.reset();
    this->aywrapper.reset();
}
