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
 * 512-pixel GE texture, so it is presented as two side-by-side quads
 * sampled from the same source rows (columns 0..512 and 512..576);
 * the GE scales it to the display, no CPU copy is involved.
 *
 * The Vector-06C frame is 288 lines tall, 16 more than the 272-line
 * PSP display. The middle 272 lines are shown (8 dropped above,
 * 8 below), stretched over the full 480x272 display.
 */
#define BORDER_DST_W 480
#define BORDER_DST_H 272
#define BORDER_DST_Y 0
#define BORDER_SRC_Y 8
#define BORDER_SRC_H 272   /* 288 frame lines minus 8 above and 8 below */

class VirtualKeyboard;
class MainMenu;
class RomBrowser;
class ConfigWindow;
class StateWindow;
class Popup;

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
    int fps_count;      /* presented frames since the last FPS update */
    unsigned fps_last_us;
    int fps_value;      /* FPS currently shown by the overlay */
    /* Total PSP CPU load (percent), refreshed once a second from the
     * telemetry this port already collects (see update_cpu_load in
     * tv.cpp): worker frame execution time plus display-thread busy
     * time. The kernel thread-run-time API is unusable (PPSSPP never
     * accumulates runClocks), and an idle sentinel thread cannot be
     * calibrated reliably, because the emulation worker already runs
     * while it would calibrate. */
    int cpu_load;
    unsigned cpu_last_us;             /* wall clock of the last sample */
    /* Blocking waits accumulated by the display thread since the
     * last CPU load sample, µs; everything else it does is work. */
    unsigned cpu_sync_wait_us;
    unsigned cpu_vbl_wait_us;
    /* Last measured busy times for the overlay's second line, µs. */
    unsigned cpu_worker_us;
    unsigned cpu_display_us;
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
    /* Recompute cpu_load from the worker/display telemetry.
     * Display thread only, called once a second. */
    void update_cpu_load();
    /* VKBD overlay quad, appended to the same GE list as the machine
     * picture. Display thread only. */
    void draw_vkbd_quad(VirtualKeyboard & vkbd);
    /* Popup UI layer (MAIN MENU, ROM Browser), appended to the same
     * GE list above the machine picture: a translucent dim quad over
     * the whole display plus one centered popup panel quad (own
     * indexed texture from the Popup base class, 480x272 UI
     * coordinate space, never scaled with the Vector picture). The
     * two popups are never open at once. Display thread only. */
    void draw_dim_overlay();
    void draw_popup_quad(Popup & popup);
    /* ROM Browser preview (Stage 4): a second quad over the right
     * pane of the browser panel, sampled from the browser's own
     * RGBA texture (aspect-preserving fit, bilinear, alpha blend so
     * transparent TGA pixels show the panel through). Skipped when
     * the selected ROM has no preview. Display thread only. */
    void draw_preview_quad(RomBrowser & browser);
    /* State Browser slot thumbnails (Stage 5): one quad per occupied
     * slot above the window panel, sampled from the window's shared
     * RGBA atlas (UV sub-rectangle per slot). Display thread only. */
    void draw_state_thumbs(StateWindow & state);
    /* One textured quad of the machine picture: bind the framebuffer
     * window as an indexed texture of the declared power-of-two width
     * tex_w and scale the u0..u1 x v source column/row window onto the
     * x/y/w/h screen rectangle. src must keep the framebuffer row
     * alignment (the GE masks low texture-address bits), so a quad that
     * starts mid-row shifts the window with u0 instead of the pointer.
     * Display thread only. */
    void draw_tex_quad(uint8_t * src, int tex_w, float u0, float u1, float v,
                       float x, float y, float w, float h);

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

    /* Save-state screenshot (Stage 5): copy the picture currently on
     * screen — the displayed frame, else the newest ready one — as
     * 0xAABBGGRR pixels into dst (Options.screen_width x
     * screen_height entries, CLUT resolved). All UI layers (dim
     * backdrop, popups, VKBD) exist only inside the GE list, never
     * in these buffers, so the result is the pure Vector picture. */
    void copy_latest_rgb(uint32_t * dst);

    int get_machine_fps() const { return this->machine_fps.load(); }
    void set_machine_fps(int v) { this->machine_fps = v; }
    int get_machine_cycles() const { return this->machine_cycles.load(); }
    void set_machine_cycles(int v) { this->machine_cycles = v; }
    int get_exec_us() const { return this->exec_us.load(); }
    void set_exec_us(int v) { this->exec_us = v; }
    int get_deadline_err_us() const { return this->deadline_err_us.load(); }
    void set_deadline_err_us(int v) { this->deadline_err_us = v; }

    void draw_fps_overlay(uint8_t * buf, int stride, int ox, int oy);
    std::function<uint32_t(uint8_t,uint8_t,uint8_t)> get_rgb2pixelformat() const;
    /* Present the newest ready frame (or the current one again when
     * the worker has not published anything new since the last
     * vblank). Display thread only: contains every sceGu* call.
     * Layer order in the GE list:
     *   1. current Vector frame
     *   2. translucent dim backdrop  (a popup is open)
     *   3. one popup window: State Browser, Config, ROM Browser or
     *      MAIN MENU (they are mutually exclusive); the ROM Browser
     *      gets one more quad on top: the preview of the selected
     *      ROM, the State Browser one quad per occupied slot
     *      (its screenshot thumbnails)
     *   4. VKBD, when visible (always hidden while any popup is
     *      open). */
    void render(VirtualKeyboard * vkbd = nullptr, MainMenu * menu = nullptr,
                RomBrowser * browser = nullptr,
                ConfigWindow * config = nullptr,
                StateWindow * state = nullptr);
#ifdef AUTOSELECT_ROM
    /* render sub-stage breakdown (test builds only), µs per log window */
    unsigned perf_sync_us = 0;
    unsigned perf_flush_us = 0;
    unsigned perf_vbl_us = 0;
#endif
    int get_refresh_rate() const;
    void handle_window_event(SDL_Event & event);
};
