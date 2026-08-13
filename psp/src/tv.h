#pragma once

#include <string>
#include <functional>
#include <inttypes.h>
#include "globaldefs.h"
#include "event.h"
#include "options.h"

class TV
{
private:
    /* Double-buffered framebuffer for the GE pipeline: while GE reads
     * one buffer as a texture, the filler writes the next machine frame
     * into the other one. */
    uint32_t * bmp[2];
    int wr;             /* index of the buffer the filler writes */
    uint32_t * last;    /* last buffer submitted to GE */
    bool pending;       /* a GE list was submitted and not synced yet */
    int tex_width;
    int tex_height;
    int refresh_rate;

    uint32_t pixelformat;

public:
    TV();
    ~TV();
    int probe();
    void init();
    void toggle_fullscreen();
    void save_frame(std::string path);
    uint32_t* pixels() const;
    std::function<uint32_t(uint8_t,uint8_t,uint8_t)> get_rgb2pixelformat() const;
    /* executed: 1 if the frame was real, 0 if the frame is a skip frame */
    void render(int executed);
#ifdef AUTOSELECT_ROM
    /* render sub-stage breakdown (test builds only), µs per log window */
    unsigned perf_sync_us = 0;
    unsigned perf_flush_us = 0;
    unsigned perf_vbl_us = 0;
#endif
    int get_refresh_rate() const;
    void handle_window_event(SDL_Event & event);
};
