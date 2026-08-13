#pragma once

#include <string>
#include <functional>
#include <inttypes.h>
#include "globaldefs.h"
#include "event.h"
#include "options.h"

/*
 * 0 - picture area only (512x256)
 * 1 - complete Vector-06C frame including border
 *
 * The border window (576x272) is wider than the 512-pixel GU texture,
 * so it is downscaled through a staging buffer (TV::copy_bmt_to_texbuf).
 */
#define SHOW_BORDER 1

#if SHOW_BORDER
/* The Vector-06C frame is 288 lines tall, 16 more than the 272-line
 * PSP display. The copy takes the middle 272 lines (8 dropped above,
 * 8 below), so every copied line reaches the screen, and the result
 * is stretched over the full 480x272 display. */
#define BORDER_DST_W 480
#define BORDER_DST_H 272
#define BORDER_DST_Y 0
#endif

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
#if SHOW_BORDER
    uint32_t * texbuf;  /* downscale staging buffer for the border window */
#endif
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
    void copy_bmt_to_texbuf(const uint32_t * src_buf,
                            int src_x, int src_y, int src_w, int src_h);
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
