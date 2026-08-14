#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <inttypes.h>
#include "globaldefs.h"
#include "event.h"
#include "options.h"

/*
 * Options.show_border == true: the complete Vector-06C frame including
 * border is shown. The border window (576x272) is wider than the
 * 512-pixel GU texture, so it is downscaled through a staging buffer
 * (TV::copy_bmt_to_texbuf).
 *
 * The Vector-06C frame is 288 lines tall, 16 more than the 272-line
 * PSP display. The copy takes the middle 272 lines (8 dropped above,
 * 8 below), so every copied line reaches the screen, and the result
 * is stretched over the full 480x272 display.
 */
#define BORDER_DST_W 480
#define BORDER_DST_H 272
#define BORDER_DST_Y 0

class TV
{
public:
    /* Framebuffer ownership states for the Worker <-> Main handoff.
     * The worker draws a machine frame into a WRITING buffer and
     * publishes it as READY; the display thread takes the newest
     * READY buffer for GE (DISPLAYING) and hands it back (FREE) once
     * the GE list that sampled it has been synced. The worker never
     * touches a DISPLAYING buffer, the display thread never reads a
     * WRITING one. */
    enum BufState : int {
        BUF_FREE = 0,
        BUF_WRITING,
        BUF_READY,
        BUF_DISPLAYING
    };

    static const int NBUF = 3;

private:
    uint8_t * bmp[NBUF];
    std::atomic<int> buf_state[NBUF];
    /* Newest READY buffer and the READY one published before it, if
     * the display thread has not picked it up yet (-1 when none). */
    std::atomic<int> ready_idx;
    std::atomic<int> old_ready_idx;
    int displaying_idx; /* display thread only */
    uint8_t * texbuf;   /* downscale staging buffer for the border window */
    int fps_count;      /* presented frames since the last FPS update */
    unsigned fps_last_us;
    int fps_value;      /* FPS currently shown by the overlay */
    /* Machine frames executed per second, published by the worker
     * thread and shown next to fps_value by the overlay. */
    std::atomic<int> machine_fps;
    /* CPU cycles executed per measurement window (target 2995200),
     * published by the worker alongside machine_fps. */
    std::atomic<int> machine_cycles;
    /* Timing diagnostics published by the worker: average execute_frame
     * time and last deadline error for the previous window, µs. */
    std::atomic<int> exec_us;
    std::atomic<int> deadline_err_us;
    int tex_width;
    int tex_height;
    int refresh_rate;

    bool pending;       /* a GE list was submitted and not synced yet */

    uint32_t pixelformat;

    int buffer_index(uint8_t * buf) const;
    void draw_overlay_line(uint8_t * buf, int stride, int ox, int oy,
                           const char * text);

public:
    TV();
    ~TV();
    int probe();
    void init();
    void toggle_fullscreen();
    void save_frame(std::string path);

    /* Worker thread: take a buffer to draw the next machine frame
     * into. Never returns a buffer the GE may still be reading; if
     * nothing else is available a stale READY frame is dropped and
     * recycled. Returns nullptr only when even that is impossible. */
    uint8_t * acquire_write_buffer();
    /* Worker thread: publish a completed frame (cache writeback at
     * this ownership boundary, then READY). */
    void publish_frame(uint8_t * buf);
    /* Display thread: take the newest READY frame for presentation,
     * dropping any older unshown ones; nullptr when nothing is new. */
    uint8_t * acquire_ready_frame();
    /* Display thread: return a fully synced buffer to the free pool. */
    void release_displayed_frame(uint8_t * buf);

    int get_machine_fps() const { return this->machine_fps.load(); }
    void set_machine_fps(int v) { this->machine_fps = v; }
    int get_machine_cycles() const { return this->machine_cycles.load(); }
    void set_machine_cycles(int v) { this->machine_cycles = v; }
    int get_exec_us() const { return this->exec_us.load(); }
    void set_exec_us(int v) { this->exec_us = v; }
    int get_deadline_err_us() const { return this->deadline_err_us.load(); }
    void set_deadline_err_us(int v) { this->deadline_err_us = v; }

    void copy_bmt_to_texbuf(const uint8_t * src_buf,
                            int src_x, int src_y, int src_w, int src_h);
    void draw_fps_overlay(uint8_t * buf, int stride, int ox, int oy);
    std::function<uint32_t(uint8_t,uint8_t,uint8_t)> get_rgb2pixelformat() const;
    /* Present the newest ready frame (or the current one again when
     * the worker has not published anything new since the last
     * vblank). Display thread only: contains every sceGu* call. */
    void render();
#ifdef AUTOSELECT_ROM
    /* render sub-stage breakdown (test builds only), µs per log window */
    unsigned perf_sync_us = 0;
    unsigned perf_flush_us = 0;
    unsigned perf_vbl_us = 0;
#endif
    int get_refresh_rate() const;
    void handle_window_event(SDL_Event & event);
};
