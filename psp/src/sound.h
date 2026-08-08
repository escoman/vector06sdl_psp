#pragma once

#include <atomic>
#include "globaldefs.h"
#include "8253.h"
#include "ay.h"
#include "resampler.h"
#include "wav.h"


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

    Resampler resampler;
    WavRecorder * rec;

public:
    Soundnik(TimerWrapper & tw, AYWrapper & aw) : timerwrapper(tw),
        aywrapper(aw), sampleRate(0), sound_accu_top(0), wrptr(0),
        wrbuf(0), rdbuf(0), rdpos(0), last_value(0.0f), rec(0)
    {}

    void init(WavRecorder * _rec = 0);
    void pause(int pause);
    static void callback(void * buf, unsigned int reqn, void * pdata);
    void sample(float samp);
    void soundSteps(int nclk1m5, int tapeout, int covox, int tapein);

    void reset();
};
