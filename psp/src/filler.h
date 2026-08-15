#pragma once

#include "globaldefs.h"

#include "memory.h"
#include "vio.h"
#include "tv.h"

/* Group the 4 bit planes once per fetched byte column (fetchPixels,
 * a fixed 4-step bit permutation) so shiftOutPixels emits a pixel in
 * one shift+extract instead of four shifts per pixel. Measured on PSP:
 * the pixel loop is the single biggest CPU consumer. */
#define USE_BIT_PERMUTE 1


#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

class PixelFiller
{
private:
    bool mode512;
    int raster_pixel;   // horizontal pixel counter
    int raster_line;    // raster line counter
    int fb_column;      // frame buffer column
    int fb_row;         // frame buffer row
    bool vborder;       // vertical border flag
    bool visible;       // visible area flag
    int bmpofs;         // bitmap offset for current pixel
    int border_index;
    int first_visible_line;
    int center_offset;
    int screen_width;

    uint32_t pixel32;
#if USE_BIT_PERMUTE
    uint32_t pixel32_grouped;
#endif
    uint32_t * mem32;
    uint8_t * pixels;

    Memory & memory;
    IO & io;
    TV & tv;

    int fill1_count, fill2_count;

    /* fast_framebuffer mode (Options.fast_framebuffer, set in init()):
     * the raster fast paths paint nothing; render_full_frame() builds
     * the whole bmp in one pass after the machine frame completes. */
    bool fast;

    int fill1_nodraw(int clocks, int commit_time, int commit_time_pal, bool updateScreen);
    int pixelIndexAt(int rpixel) __attribute__((always_inline));
    void render_full_frame_256();
    void render_full_frame_512();

public:
    bool brk;
    bool irq;
    int irq_clk;
    auto get_raster_pixel() const -> const int;
    auto get_raster_line() const -> const int;

public:
    PixelFiller(Memory & _mem, IO & _io, TV & _tv);
    void init();
    void reset();
    /* The emulation worker hands the filler the framebuffer it
     * acquired for the current machine frame (the filler must never
     * pick one on its own: buffer ownership is shared with the
     * display thread). */
    void set_framebuffer(uint8_t * buf) { this->pixels = buf; }
    /* Per-pixel helpers: must be inlined into fill1/2/3/4. GCC at -O2
     * keeps them out of line otherwise, and the call overhead is half
     * of the pixel loop cost (profiler on PSP). */
    void fetchPixels() __attribute__((always_inline));
    int shiftOutPixels() __attribute__((always_inline));
    int getColorIndex(int rpixel, bool border) __attribute__((always_inline));

    int fill(int clocks, int commit_time, int commit_time_pal, bool updateScreen);
    int fill1(int clocks, int commit_time, int commit_time_pal, bool updateScreen);
    int fill2(int clocks);
    int fill3(int clocks);
    int fill4(int clocks);
    void advanceLine(bool updateScreen);

    /* fast_framebuffer: build the whole bmp from the final VRAM,
     * palette, scroll and mode state after the machine frame. */
    void render_full_frame();
};
