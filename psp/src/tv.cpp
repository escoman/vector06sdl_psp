#include <string>
#include <inttypes.h>
#include <cstring>
#include <cstdio>
#include "globaldefs.h"
#include "event.h"
#include "options.h"
#include "font.h"
#include "tv.h"
#include "vkbd.h"
#include "mainmenu.h"
#include "rombrowser.h"
#include "configwindow.h"
#include "statewindow.h"
#include "mapwindow.h"
#include "popup.h"

#include <pspgu.h>
#include <pspgum.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include "debuglog.h"

/*
 * PSP display:
 *
 * Physical screen: 480x272
 * Framebuffer stride: 512 pixels
 *
 * Vector-06C framebuffer:
 * Full framebuffer: 576x288
 * Picture area:     512x256 at (32,16)
 *
 * Options.show_border == false -> 512x256 picture area
 * Options.show_border == true  -> complete 576x288 framebuffer
 *
 * The picture area is fitted to 480x240 and centered vertically on
 * the 480x272 display. The border window takes the middle 272 of the
 * 288 lines and is stretched over the full 480x272 display through
 * the staging buffer (see tv.h).
 */

#define PSP_SCREEN_WIDTH  480
#define PSP_SCREEN_HEIGHT 272

/* PSP framebuffer is wider in memory than the visible display. */
#define PSP_FB_WIDTH  512
#define PSP_FB_HEIGHT 272
#define PSP_FB_STRIDE 512

static unsigned int __attribute__((aligned(16))) list[262144];

/* GE CLUT for the 8-bit indexed pipeline: entry i expands raw hardware
 * color i exactly like get_rgb2pixelformat() does. Static and 16-byte
 * aligned as sceGuClutLoad() requires; built once in TV::init(). */
static uint32_t __attribute__((aligned(16))) clut_table[256];
static unsigned int *fbp0 = 0;
static unsigned int *fbp1 = 0;
static bool gu_initialized = false;

/* PSP GU texture size must be a power of two. */
#define TEX_W 512
#define TEX_H 512

/* Complete Vector-06C framebuffer. */
#define SCREEN_W 576
#define SCREEN_H 288

/* Vector-06C picture area inside the complete framebuffer. */
#define VIDEO_X 32
#define VIDEO_Y 16
#define VIDEO_W 512
#define VIDEO_H 256


/* Destination image size on PSP. */
#define DST_W 480
#define DST_H 272
#define DST_X 0
#define DST_Y 0

#define GU_MODE GU_LINEAR

TV::TV() : ready_idx(-1), old_ready_idx(-1), displaying_idx(-1),
           fps_count(0), fps_last_us(0), fps_value(0),
           cpu_load(0), cpu_last_us(0),
           cpu_sync_wait_us(0), cpu_vbl_wait_us(0),
           cpu_worker_us(0), cpu_display_us(0),
           machine_fps(0),
           machine_cycles(0), exec_us(0), deadline_err_us(0),
           pending(false),
           pixelformat(TV_PIXELFORMAT)
{
    for (int i = 0; i < NBUF; ++i) {
        this->bmp[i] = 0;
        this->buf_state[i] = BUF_FREE;
    }
}

TV::~TV()
{
    for (int i = 0; i < NBUF; ++i) {
        delete[] this->bmp[i];
    }
}

int TV::probe()
{
    return 0;
}

void TV::init()
{
    dbglog("TV::init: screen %dx%d\n",
           Options.screen_width, Options.screen_height);

    /* Three framebuffers: while GE textures one and the worker draws
     * the next machine frame into another, the third carries the
     * handoff slack (see the BufState comments in tv.h). */
    for (int i = 0; i < NBUF; ++i) {
        this->bmp[i] = new uint8_t[Options.screen_width * Options.screen_height];
        memset(this->bmp[i], 0,
               Options.screen_width * Options.screen_height);
    }
    dbglog("TV::init: bmp[%d] allocated\n", NBUF);

    this->tex_width = Options.screen_width;
    this->tex_height = Options.screen_height;
    /* PSP LCD runs at 60 Hz while the Vector machine needs 50 fps.
     * The machine no longer derives its timing from this value: the
     * worker thread paces itself against the wall clock (20 ms per
     * machine frame), and the display thread shows the newest ready
     * frame every vblank, which reproduces the old 6:5 pullup without
     * ever skipping machine frames. */
    this->refresh_rate = 60;

    /* GE CLUT for the 8-bit indexed pipeline: entry i expands raw
     * hardware color i (r = bits 0-2, g = bits 3-5, b = bits 6-7)
     * exactly like get_rgb2pixelformat() does. The table never
     * changes at runtime; write it back once for the GE DMA. */
    for (int i = 0; i < 256; ++i) {
        const uint32_t r = i & 0x07;
        const uint32_t g = (i >> 3) & 0x07;
        const uint32_t b = (i >> 6) & 0x03;
        const uint32_t R = (r << 5) | (r << 2) | (r >> 1);
        const uint32_t G = (g << 5) | (g << 2) | (g >> 1);
        const uint32_t B = (b << 6) | (b << 4) | (b << 2) | b;
        clut_table[i] = 0xff000000u | (B << 16) | (G << 8) | R;
    }
    sceKernelDcacheWritebackInvalidateRange(clut_table, sizeof(clut_table));

    if (!gu_initialized) {
        dbglog("TV::init: initializing GU...\n");

        sceGuInit();
        sceGuStart(GU_DIRECT, list);

        fbp0 = (unsigned int*)guGetStaticVramBuffer(
            PSP_FB_WIDTH, PSP_FB_HEIGHT, GU_PSM_8888);

        fbp1 = (unsigned int*)guGetStaticVramBuffer(
            PSP_FB_WIDTH, PSP_FB_HEIGHT, GU_PSM_8888);

        sceGuDrawBuffer(GU_PSM_8888, fbp0, PSP_FB_STRIDE);
        sceGuDispBuffer(PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT,
                        fbp1, PSP_FB_STRIDE);

        sceGuOffset(2048 - (PSP_SCREEN_WIDTH / 2),
                    2048 - (PSP_SCREEN_HEIGHT / 2));

        sceGuViewport(2048, 2048,
                      PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);

        sceGuScissor(0, 0,
                     PSP_SCREEN_WIDTH, PSP_SCREEN_HEIGHT);
        sceGuEnable(GU_SCISSOR_TEST);

        sceGuDisable(GU_ALPHA_TEST);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuFrontFace(GU_CW);
        sceGuShadeModel(GU_FLAT);
        sceGuDisable(GU_LIGHTING);
        sceGuDisable(GU_CULL_FACE);
        sceGuEnable(GU_TEXTURE_2D);

        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexFilter(GU_MODE, GU_MODE);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);

        sceGuFinish();
        sceGuSync(0, 0);

        sceDisplayWaitVblankStart();
        sceGuDisplay(GU_TRUE);

        gu_initialized = true;
    }
}

void TV::toggle_fullscreen()
{
    /* Not supported on PSP */
}

void TV::save_frame(std::string path)
{
    const int width = Options.screen_width;
    const int height = Options.screen_height;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        dbglog("TV::save_frame: failed to open %s\n", path.c_str());
        return;
    }

    const uint32_t pixel_offset = 54;
    const uint32_t row_size = width * 4;
    const uint32_t image_size = row_size * height;
    const uint32_t file_size = pixel_offset + image_size;

    uint8_t header[54];
    std::memset(header, 0, sizeof(header));

    header[0] = 'B';
    header[1] = 'M';

    std::memcpy(&header[2], &file_size, 4);
    std::memcpy(&header[10], &pixel_offset, 4);

    const uint32_t info_size = 40;
    const int32_t bmp_width = width;
    const int32_t bmp_height = height;

    std::memcpy(&header[14], &info_size, 4);
    std::memcpy(&header[18], &bmp_width, 4);
    std::memcpy(&header[22], &bmp_height, 4);

    const uint16_t planes = 1;
    const uint16_t bpp = 32;

    std::memcpy(&header[26], &planes, 2);
    std::memcpy(&header[28], &bpp, 2);

    std::memcpy(&header[34], &image_size, 4);

    std::fwrite(header, 1, sizeof(header), f);

    // BMP is bottom-to-top.
    // TV::bmp holds CLUT indices; clut[] is ABGR8888 as 0xAABBGGRR.
    // Best available picture: the displayed frame, else the newest
    // ready one, else buffer 0 (test builds only).
    const uint8_t * srcbuf = nullptr;
    if (this->displaying_idx >= 0) {
        srcbuf = this->bmp[this->displaying_idx];
    } else {
        int idx = this->ready_idx.load();
        srcbuf = idx >= 0 ? this->bmp[idx] : this->bmp[0];
    }
    const uint8_t * const srcbuf_ = srcbuf;
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            uint32_t p = clut_table[srcbuf_[y * width + x]];

            uint8_t r = (p >> 0) & 0xff;
            uint8_t g = (p >> 8) & 0xff;
            uint8_t b = (p >> 16) & 0xff;

            uint8_t pixel[4] = { b, g, r, 0 };
            std::fwrite(pixel, 1, 4, f);
        }
    }

    std::fclose(f);

    dbglog("TV::save_frame: saved %s (%dx%d)\n",
           path.c_str(), width, height);
}

/*
 * Save-state screenshot source (Stage 5). Same "best available
 * picture" choice as save_frame: the displayed frame, else the
 * newest ready one, else buffer 0. The buffers hold the machine
 * picture only (every UI layer is composed later in the GE list),
 * so this is exactly "State screenshot = Vector framebuffer".
 * Reading a DISPLAYING buffer races harmlessly with the GE's DMA
 * read (both are pure reads); saving only ever happens while the
 * machine is paused, so the picture cannot change mid-copy.
 */
void TV::copy_latest_rgb(uint32_t * dst)
{
    const int width = Options.screen_width;
    const int height = Options.screen_height;

    const uint8_t * srcbuf;
    if (this->displaying_idx >= 0) {
        srcbuf = this->bmp[this->displaying_idx];
    } else {
        const int idx = this->ready_idx.load();
        srcbuf = (idx >= 0) ? this->bmp[idx] : this->bmp[0];
    }

    for (int y = 0; y < height; ++y) {
        const uint8_t * row = srcbuf + (size_t)y * width;
        uint32_t * out = dst + (size_t)y * width;
        for (int x = 0; x < width; ++x) {
            out[x] = clut_table[row[x]];
        }
    }
}

/*
 * Framebuffer ownership handoff between the emulation worker thread
 * and the display thread. The only shared state is the per-buffer
 * state word plus the two ready slots, all atomic; the buffer memory
 * itself is never touched by both threads at once (see tv.h).
 */
int TV::buffer_index(uint8_t * buf) const
{
    for (int i = 0; i < NBUF; ++i) {
        if (this->bmp[i] == buf) {
            return i;
        }
    }
    return -1;
}

uint8_t * TV::acquire_write_buffer()
{
    for (int i = 0; i < NBUF; ++i) {
        int expected = BUF_FREE;
        if (this->buf_state[i].compare_exchange_strong(
                expected, BUF_WRITING)) {
            return this->bmp[i];
        }
    }

    /* No free buffer: the display thread holds everything. Recycle a
     * stale READY frame (dropping it) rather than waiting; a buffer
     * the GE may be reading (DISPLAYING) is never touched. Prefer the
     * older slot so the newest unshown frame survives. */
    int idx = this->old_ready_idx.exchange(-1, std::memory_order_acq_rel);
    if (idx < 0) {
        idx = this->ready_idx.exchange(-1, std::memory_order_acq_rel);
    }
    if (idx >= 0) {
        this->buf_state[idx] = BUF_WRITING;
        return this->bmp[idx];
    }
    return nullptr;
}

void TV::publish_frame(uint8_t * buf)
{
    const int idx = this->buffer_index(buf);
    if (idx < 0) {
        return;
    }

    /* The GE samples the buffer by DMA from main memory, and the
     * display thread never copies it: write the whole frame back at
     * the ownership boundary (once per frame, never per line). */
    sceKernelDcacheWritebackRange(buf,
        (unsigned)(Options.screen_width * Options.screen_height));

    this->buf_state[idx] = BUF_READY;

    /* Publish as the newest ready frame; whatever was ready before
     * moves one slot down, and whatever falls out of that slot is
     * stale and goes back to the free pool. */
    int prev = this->ready_idx.exchange(idx, std::memory_order_acq_rel);
    int dropped = this->old_ready_idx.exchange(prev, std::memory_order_acq_rel);
    if (dropped >= 0 && dropped != idx) {
        this->buf_state[dropped] = BUF_FREE;
    }
}

uint8_t * TV::acquire_ready_frame()
{
    const int idx = this->ready_idx.exchange(-1, std::memory_order_acquire);
    if (idx < 0) {
        return nullptr;
    }

    /* Any older unshown frame is obsolete: free it right away. */
    const int older = this->old_ready_idx.exchange(-1, std::memory_order_acquire);
    if (older >= 0 && older != idx) {
        this->buf_state[older] = BUF_FREE;
    }

    this->buf_state[idx] = BUF_DISPLAYING;
    return this->bmp[idx];
}

void TV::release_displayed_frame(uint8_t * buf)
{
    const int idx = this->buffer_index(buf);
    if (idx < 0) {
        return;
    }
    if (this->displaying_idx == idx) {
        this->displaying_idx = -1;
    }
    this->buf_state[idx] = BUF_FREE;
}

/*
 * FPS/FRAMES overlay. The counters are drawn into the picture the GE
 * will display (the framebuffer window), not into the PSP display
 * buffer directly: the display buffer contents written by the CPU are
 * not what reaches the screen in the pipelined GU mode. Glyphs come
 * from font.h (8x8, bit 7 = leftmost).
 *
 * Two lines: FPS is the display rate of this thread, FRAMES is the
 * machine frame rate published by the emulation worker, so a gap
 * between them shows which side is behind.
 */
void TV::draw_overlay_line(uint8_t * buf, int stride, int ox, int oy,
                           const char * text)
{
    for (int i = 0; text[i] != '\0'; ++i) {
        const uint8_t * g = overlay_font_glyph(text[i]);
        if (g == nullptr) {
            continue;
        }
        for (int y = 0; y < OVERLAY_FONT_H; ++y) {
            uint8_t * dst = buf + (size_t)(oy + y) * stride + ox + i * OVERLAY_FONT_W;
            uint8_t row = g[y];
            for (int x = 0; x < OVERLAY_FONT_W; ++x) {
                dst[x] = (row & (0x80u >> x))
                    ? 0xFFu   /* glyph: white = CLUT entry 255 */
                    : 0x00u;  /* cell background: black = CLUT entry 0 */
            }
        }
    }
}

/*
 * Total PSP CPU load. Neither kernel facility works for this port:
 * sceKernelReferThreadRunStatus() reports runClocks, but PPSSPP
 * never accumulates them (always 0), and an idle sentinel thread
 * cannot be calibrated because the emulation worker is already
 * running during any calibration window. So the load is composed
 * from telemetry this port collects anyway:
 *
 *   worker  - average execute_frame time x machine frames executed,
 *             published by the worker every second;
 *   display - everything the display thread does outside its two
 *             blocking waits (sceGuSync, sceDisplayWaitVblankStart).
 *
 * The PSP audio callback thread is not counted (a few percent at
 * most); the value therefore reads slightly low, on both hardware
 * and PPSSPP alike.
 */
void TV::update_cpu_load()
{
    const unsigned now = sceKernelGetSystemTimeLow();
    unsigned long long worker_us = 0;
    unsigned long long display_us = 0;
    if (this->cpu_last_us != 0) {
        const unsigned window_us = now - this->cpu_last_us;
        if (window_us >= 1000000) {
            /* Worker busy time over the window, µs. */
            worker_us =
                (unsigned long long)(unsigned)this->get_exec_us()
                * (unsigned)this->get_machine_fps();

            /* Display thread busy = window minus its blocking
             * waits; everything else it does is real work. */
            const unsigned long long wait_us =
                (unsigned long long)this->cpu_sync_wait_us
                + this->cpu_vbl_wait_us;
            display_us = window_us;
            if (wait_us < display_us) {
                display_us -= wait_us;
            } else {
                display_us = 0;
            }

            unsigned long long total_us = worker_us + display_us;
            if (total_us > window_us) {
                total_us = window_us;
            }
            this->cpu_load = (int)((total_us * 100) / window_us);

            /* Breakdown for the overlay's second line. */
            this->cpu_worker_us = (unsigned)worker_us;
            this->cpu_display_us = (unsigned)display_us;
            dbglog("CPU: %d%% worker=%lu display=%lu us\n",
                   this->cpu_load,
                   (unsigned long)worker_us,
                   (unsigned long)display_us);
        }
    }
    this->cpu_sync_wait_us = 0;
    this->cpu_vbl_wait_us = 0;
    this->cpu_last_us = now;
}

void TV::draw_fps_overlay(uint8_t * buf, int stride, int ox, int oy)
{
    char text[120];
    snprintf(text, sizeof(text), "FPS: %0d  FRAMES: %0d  CYCLES: %d  CPU: %d%%",
        this->fps_value, this->get_machine_fps(), this->get_machine_cycles(),
        this->cpu_load);
    this->draw_overlay_line(buf, stride, ox, oy, text);

    /* Second line: where the single core goes, per second. */
    snprintf(text, sizeof(text), "WORKER: %u.%03uMS  DISPLAY: %u.%03uMS",
        this->cpu_worker_us / 1000, this->cpu_worker_us % 1000,
        this->cpu_display_us / 1000, this->cpu_display_us % 1000);
    this->draw_overlay_line(buf, stride, ox, oy + OVERLAY_FONT_H, text);
}

/*
 * Vertex storage of the overlay quads. Two real-hardware constraints
 * shape it:
 *
 * 1. The GE executes the GU_DIRECT list while the CPU is still
 *    filling it, and sceGuDrawArray() never copies the vertices: the
 *    GE fetches them by DMA when it reaches the command. On the stack
 *    the next quad's helper reuses the same slot right after the call
 *    returns, so a GE lagging behind the CPU fetches overwritten or
 *    partially written vertices — garbage triangles and flicker. The
 *    data therefore must stay untouched until sceGuSync() finishes
 *    the list.
 *
 * 2. The GE's vertex DMA reads main memory, not the data cache.
 *    Parking the vertices inside the display list itself
 *    (sceGuGetMemory) keeps them alive, but nothing writes the CPU
 *    stores back, so the GE sampled stale list bytes and the popup
 *    windows blinked every other frame. Every vertex block must be
 *    explicitly written back right before its draw call.
 *
 * A small per-frame pool of static buffers satisfies both: blocks are
 * never overwritten within a frame, the sceGuSync at the top of
 * TV::render() finishes the GE before the pool is reused, and each
 * block is cache-writebacked just before its draw call.
 */
#define FRAME_VERTEX_POOL_BYTES 2048
static uint8_t frame_vertex_pool[FRAME_VERTEX_POOL_BYTES]
    __attribute__((aligned(16)));
static unsigned frame_vertex_used = 0;

static void frame_vertex_reset()
{
    frame_vertex_used = 0;
}

static void * alloc_frame_vertices(unsigned bytes)
{
    unsigned off = (frame_vertex_used + 15u) & ~15u;
    frame_vertex_used = off + bytes;
    return frame_vertex_pool + off;
}

void TV::render(VirtualKeyboard * vkbd, MainMenu * menu, RomBrowser * browser,
                ConfigWindow * config, StateWindow * state, MapWindow * mapk)
{
    if (!Options.novideo) {
        dbglog("TV::render: start\n");

        /* Finish presenting the previously submitted list; only now is
         * its buffer safe to hand back to the worker. */
        if (this->pending) {
            /* The two blocking waits are timed unconditionally:
             * they feed the CPU load meter (update_cpu_load). */
            const unsigned t_sync0 = sceKernelGetSystemTimeLow();
            sceGuSync(0, 0);
            dbglog("TV::render: sceGuSync done\n");
            const unsigned t_sync1 = sceKernelGetSystemTimeLow();

            sceDisplayWaitVblankStart();
            dbglog("TV::render: vblank done\n");
            const unsigned t_vbl1 = sceKernelGetSystemTimeLow();

            this->cpu_sync_wait_us += t_sync1 - t_sync0;
            this->cpu_vbl_wait_us += t_vbl1 - t_sync1;
#ifdef AUTOSELECT_ROM
            this->perf_sync_us += t_sync1 - t_sync0;
            this->perf_vbl_us += t_vbl1 - t_sync1;
#endif

            sceGuSwapBuffers();
            dbglog("TV::render: swap done\n");

            /* FPS counter: count presented frames and refresh the
             * value once a second. The text itself is drawn into the
             * picture by TV::draw_fps_overlay(), because a CPU write
             * into the displayed buffer is not visible when the GE
             * pipeline composes the screen. */
            ++this->fps_count;
            unsigned now = sceKernelGetSystemTimeLow();
            if ((unsigned)(now - this->fps_last_us) >= 1000000) {
                this->fps_value = this->fps_count;
                this->fps_count = 0;
                this->fps_last_us = now;
                this->update_cpu_load();
            }
        }

        /* Present the newest frame the worker published since the
         * last vblank; when there is none (the machine runs 50 fps,
         * the LCD 60 Hz, or the worker fell behind) the current
         * picture is shown again. */
        uint8_t * src_buf = this->acquire_ready_frame();
        const bool fresh = src_buf != nullptr;
        if (fresh && this->displaying_idx < 0 && !this->pending) {
            dbglog("TV::render: first fresh frame acquired\n");
            printf("MAIN: first fresh frame acquired\n");
        }
        if (fresh) {
            if (this->pending && this->displaying_idx >= 0) {
                /* The GE list synced above is done sampling the old
                 * displayed buffer. */
                this->release_displayed_frame(this->bmp[this->displaying_idx]);
            }
            this->displaying_idx = this->buffer_index(src_buf);
        } else if (this->displaying_idx >= 0) {
            src_buf = this->bmp[this->displaying_idx];
        }

        if (!src_buf) {
            /* The worker has not published a frame yet: keep the
             * vblank rhythm, submit nothing. The wait is idle time
             * for the CPU load meter. */
            const unsigned t_vbl0 = sceKernelGetSystemTimeLow();
            sceDisplayWaitVblankStart();
            this->cpu_vbl_wait_us += sceKernelGetSystemTimeLow() - t_vbl0;
            return;
        }

        /* The pool survives until the sceGuSync above finishes the
         * previous list; only now may the next frame reuse it. */
        frame_vertex_reset();

        sceGuStart(GU_DIRECT, list);
        dbglog("TV::render: sceGuStart OK\n");

#ifdef AUTOSELECT_ROM
        unsigned perf_f0 = sceKernelGetSystemTimeLow();
#endif
        /* Texture source window for the GE. Neither mode copies the
         * picture on the CPU: the GE samples the framebuffer by DMA
         * and scales it onto the display itself. Without border the
         * source is the 512x256 picture window; with border it is the
         * full 576x272 frame, presented as two side-by-side quads
         * because a GE texture is at most 512 pixels wide. */
        uint8_t * texsrc;
        unsigned wb_bytes;
        if (Options.show_border) {
            texsrc = src_buf + (size_t)BORDER_SRC_Y * this->tex_width;
            wb_bytes = (unsigned)(BORDER_SRC_H * this->tex_width);
        } else {
            texsrc = src_buf
                + (size_t)VIDEO_Y * this->tex_width + VIDEO_X;
            wb_bytes = (unsigned)(VIDEO_H * this->tex_width);
        }
        if (Options.show_fps) {
            this->draw_fps_overlay(src_buf, this->tex_width,
                Options.show_border ? 0 : VIDEO_X,
                Options.show_border ? BORDER_SRC_Y : VIDEO_Y);
        }
        sceKernelDcacheWritebackInvalidateRange((void *)texsrc, wb_bytes);

        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(32, clut_table);
        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
#ifdef AUTOSELECT_ROM
        this->perf_flush_us += sceKernelGetSystemTimeLow() - perf_f0;
#endif

        dbglog("TV::render: texture flush done\n");

        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexFilter(GU_MODE, GU_MODE);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);

        if (Options.show_border) {
            /* Two quads side by side. The left one samples source
             * columns 0..512; the right one starts its window at
             * column 511 so its bilinear filtering at the seam sees
             * the genuine left neighbor. It is drawn second with a
             * full pixel of destination overlap, so it wins
             * everywhere the left quad's u approaches its 512 texel
             * edge and would wrap. The right quad's window stops at
             * source column 575: the last texel is the real border
             * edge, not the next framebuffer row.
             *
             * The GE masks the low bits of the texture address, so
             * the right quad keeps the row-aligned base (column 496,
             * a multiple of 16) and reaches column 511 through u0 =
             * 15; an unaligned base pointer made the whole right part
             * jump sideways on real hardware. */
            const float x_left_end = (float)DST_W * (float)VIDEO_W
                / (float)SCREEN_W;                       /* 426.67 */
            const float x_right_start = x_left_end - 1.0f;
            this->draw_tex_quad(texsrc, TEX_W,
                0.0f, (float)VIDEO_W, (float)BORDER_SRC_H,
                0.0f, (float)BORDER_DST_Y,
                x_left_end, (float)BORDER_DST_H);
            this->draw_tex_quad(texsrc + 496, 128,
                15.0f, 79.0f, (float)BORDER_SRC_H,
                x_right_start, (float)BORDER_DST_Y,
                (float)DST_W - x_right_start, (float)BORDER_DST_H);
        } else {
            this->draw_tex_quad(texsrc, TEX_W,
                0.0f, (float)VIDEO_W, (float)VIDEO_H,
                (float)DST_X, (float)DST_Y,
                (float)DST_W, (float)DST_H);
        }

        dbglog("TV::render: draw array done\n");

        /* UI layer: a translucent backdrop dims the game picture
         * (which stays visible underneath and is never copied or
         * touched), then one popup window on top — the Config
         * window, the ROM Browser and the MAIN MENU are mutually
         * exclusive. The layer lives in the 480x272 display
         * coordinate space, independent of the Vector picture
         * size. */
        if (state != nullptr && state->is_open()) {
            this->draw_dim_overlay();
            /* Slot screenshots first (one quad per occupied slot,
             * stretched over its whole cell, no-op without any);
             * the panel quad on top keeps transparent windows
             * (C_HOLE) where the pictures must stay visible, while
             * the slot numbers and dates rasterized into the panel
             * land above the pictures. */
            this->draw_state_thumbs(*state);
            this->draw_popup_quad(*state);
            dbglog("TV::render: state browser done\n");
        } else if (config != nullptr && config->is_open()) {
            this->draw_dim_overlay();
            this->draw_popup_quad(*config);
            dbglog("TV::render: config window done\n");
        } else if (browser != nullptr && browser->is_open()) {
            this->draw_dim_overlay();
            this->draw_popup_quad(*browser);
            /* Preview of the selected ROM above the right pane
             * (no-op without an image). */
            this->draw_preview_quad(*browser);
            dbglog("TV::render: rom browser done\n");
        } else if (menu != nullptr && menu->is_open()) {
            this->draw_dim_overlay();
            this->draw_popup_quad(*menu);
            dbglog("TV::render: main menu done\n");
        } else if (mapk != nullptr && mapk->is_open()) {
            this->draw_dim_overlay();
            this->draw_popup_quad(*mapk);
            dbglog("TV::render: map keys done\n");
        }

        /* VKBD overlay: a second textured quad in the same GE list,
         * drawn on top of the full-size machine picture and sampled
         * from the keyboard's own indexed texture. Hidden whenever a
         * popup window is open, except the Map Keys window, which
         * keeps the VKBD on screen as its key picker. */
        if (vkbd != nullptr && vkbd->is_visible()) {
            this->draw_vkbd_quad(*vkbd);
            dbglog("TV::render: vkbd quad done\n");
        }

        sceGuFinish();
        dbglog("TV::render: sceGuFinish done\n");

        /* No sync here: the GE keeps reading the texture buffer while
         * the CPU emulates the next machine frame. The sync + vblank +
         * swap happen at the top of the next call. */
        this->pending = true;
    }
}

/*
 * VKBD overlay. The keyboard lives in its own indexed texture owned
 * by VirtualKeyboard (main thread memory, never the Vector
 * framebuffer); it is re-rasterized only on visual state changes, so
 * here the only recurring work is the texture setup and one quad.
 */
void TV::draw_vkbd_quad(VirtualKeyboard & vkbd)
{
    /* Newly rasterized pixels must reach main memory before the GE
     * samples them by DMA; done once per repaint, not per frame. */
    if (vkbd.consume_tex_upload()) {
        sceKernelDcacheWritebackInvalidateRange(
            (void *)vkbd.tex_data(),
            (unsigned)(VirtualKeyboard::VKBD_TEX_W * VirtualKeyboard::VKBD_TEX_H));
    }

    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, vkbd.clut_data());
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, VirtualKeyboard::VKBD_TEX_W, VirtualKeyboard::VKBD_TEX_H,
                  VirtualKeyboard::VKBD_TEX_W, vkbd.tex_data());
    /* The keyboard is rasterized at display resolution: no filtering,
     * keeps the 8x8 legends crisp. The machine picture restores its
     * own filter/CLUT every frame. */
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

    struct Vertex {
        float u, v;
        float x, y, z;
    };

    const float w = (float)vkbd.get_width();
    const float h = (float)vkbd.get_height();
    const float x = ((float)PSP_SCREEN_WIDTH - w) / 2.0f;
    const float y = vkbd.is_top() ? 0.0f
                                   : (float)PSP_SCREEN_HEIGHT - h;

    Vertex * vertices = (Vertex *)alloc_frame_vertices(sizeof(Vertex) * 4);
    vertices[0] = { 0.0f, 0.0f, x,     y,     0.0f };
    vertices[1] = { w,    0.0f, x + w, y,     0.0f };
    vertices[2] = { w,    h,    x + w, y + h, 0.0f };
    vertices[3] = { 0.0f, h,    x,     y + h, 0.0f };

    /* The GE fetches the vertices by DMA from main memory. */
    sceKernelDcacheWritebackInvalidateRange(vertices, sizeof(Vertex) * 4);

    sceGuDrawArray(
        GU_TRIANGLE_FAN,
        GU_TEXTURE_32BITF |
        GU_VERTEX_32BITF |
        GU_TRANSFORM_2D,
        4, 0, vertices);
}

/*
 * Translucent backdrop under the popup windows (MAIN MENU, ROM
 * Browser): one untextured blended quad over the whole display.
 * Alpha ~50%: the game picture stays well visible underneath. PSP
 * colors are 0xAABBGGRR.
 */
#define MENU_DIM_ALPHA 0x80

void TV::draw_dim_overlay()
{
    sceGuDisable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuColor((unsigned)MENU_DIM_ALPHA << 24); /* black, alpha = MENU_DIM_ALPHA */

    struct Vertex {
        float x, y, z;
    };

    Vertex * vertices =
        (Vertex *)alloc_frame_vertices(sizeof(Vertex) * 4);
    vertices[0] = { 0.0f,                    0.0f,                     0.0f };
    vertices[1] = { (float)PSP_SCREEN_WIDTH, 0.0f,                     0.0f };
    vertices[2] = { (float)PSP_SCREEN_WIDTH, (float)PSP_SCREEN_HEIGHT, 0.0f };
    vertices[3] = { 0.0f,                    (float)PSP_SCREEN_HEIGHT, 0.0f };

    /* The GE fetches the vertices by DMA from main memory. */
    sceKernelDcacheWritebackInvalidateRange(vertices, sizeof(Vertex) * 4);

    sceGuDrawArray(
        GU_TRIANGLE_FAN,
        GU_VERTEX_32BITF |
        GU_TRANSFORM_2D,
        4, 0, vertices);

    /* Restore the state the textured quads expect. */
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
}

/*
 * Popup window quad (MAIN MENU panel or ROM Browser window): every
 * popup is rasterized into its own indexed texture inherited from
 * the Popup base class (main thread memory, never the Vector
 * framebuffer), so the recurring per-frame work is the texture
 * setup and one quad. The panel is centered on the 480x272 display.
 */
void TV::draw_popup_quad(Popup & popup)
{
    /* Newly rasterized pixels must reach main memory before the GE
     * samples them by DMA; done once per repaint, not per frame. */
    if (popup.consume_tex_upload()) {
        sceKernelDcacheWritebackInvalidateRange(
            (void *)popup.tex_data(),
            (unsigned)(Popup::POPUP_TEX_W * Popup::POPUP_TEX_H));
    }

    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, popup.clut_data());
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, Popup::POPUP_TEX_W, Popup::POPUP_TEX_H,
                  Popup::POPUP_TEX_W, popup.tex_data());
    /* Rasterized at display resolution: no filtering, keeps the 2x
     * font glyphs crisp. The machine picture restores its own
     * filter/CLUT every frame. */
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

    struct Vertex {
        float u, v;
        float x, y, z;
    };

    const float w = (float)popup.get_width();
    const float h = (float)popup.get_height();
    const float x = ((float)PSP_SCREEN_WIDTH - w) / 2.0f;
    const float y = ((float)PSP_SCREEN_HEIGHT - h) / 2.0f;

    Vertex * vertices = (Vertex *)alloc_frame_vertices(sizeof(Vertex) * 4);
    vertices[0] = { 0.0f, 0.0f, x,     y,     0.0f };
    vertices[1] = { w,    0.0f, x + w, y,     0.0f };
    vertices[2] = { w,    h,    x + w, y + h, 0.0f };
    vertices[3] = { 0.0f, h,    x,     y + h, 0.0f };

    /* The GE fetches the vertices by DMA from main memory. */
    sceKernelDcacheWritebackInvalidateRange(vertices, sizeof(Vertex) * 4);

    /* Alpha blend on: every palette entry is opaque except C_HOLE,
     * so normal panels render exactly as before, while the State
     * Browser's transparent cell windows let the underlaid slot
     * thumbnails show through. */
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

    sceGuDrawArray(
        GU_TRIANGLE_FAN,
        GU_TEXTURE_32BITF |
        GU_VERTEX_32BITF |
        GU_TRANSFORM_2D,
        4, 0, vertices);

    sceGuDisable(GU_BLEND);
}

/*
 * ROM Browser preview quad (Stage 4): the selected ROM's picture,
 * decoded once by the worker into the browser's RGBA texture and
 * stretched over the whole right pane (full pane height). Bilinear
 * filtering (the image is scaled), alpha blend so transparent 32-bit
 * TGA pixels show the panel background through. One quad per frame
 * while the browser is open and a preview is loaded.
 */
void TV::draw_preview_quad(RomBrowser & browser)
{
    if (!browser.has_preview())
        return;

    /* Newly decoded pixels must reach main memory before the GE
     * samples them by DMA; done once per image, not per frame. */
    if (browser.consume_preview_upload()) {
        sceKernelDcacheWritebackInvalidateRange(
            (void *)browser.preview_tex_data(),
            (unsigned)(RomBrowser::PREVIEW_TEX_W
                       * RomBrowser::PREVIEW_TEX_H * sizeof(uint32_t)));
    }

    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, RomBrowser::PREVIEW_TEX_W, RomBrowser::PREVIEW_TEX_H,
                  RomBrowser::PREVIEW_TEX_W, browser.preview_tex_data());
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    /* Scaled pictures look better filtered; the popup panel below
     * keeps its own NEAREST setting, restored on the next frame. */
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

    struct Vertex {
        float u, v;
        float x, y, z;
    };

    /* Panel-local fit rectangle moved onto the display: the panel is
     * centered the same way draw_popup_quad() centers it. */
    int fx, fy, fw, fh;
    browser.get_preview_rect(&fx, &fy, &fw, &fh);
    const float x0 = ((float)PSP_SCREEN_WIDTH - (float)browser.get_width())
        / 2.0f + (float)fx;
    const float y0 = ((float)PSP_SCREEN_HEIGHT - (float)browser.get_height())
        / 2.0f + (float)fy;
    const float uw = (float)browser.get_preview_w();
    const float vh = (float)browser.get_preview_h();

    Vertex * vertices = (Vertex *)alloc_frame_vertices(sizeof(Vertex) * 4);
    vertices[0] = { 0.0f, 0.0f, x0,              y0,              0.0f };
    vertices[1] = { uw,   0.0f, x0 + (float)fw,  y0,              0.0f };
    vertices[2] = { uw,   vh,   x0 + (float)fw,  y0 + (float)fh,  0.0f };
    vertices[3] = { 0.0f, vh,   x0,              y0 + (float)fh,  0.0f };

    /* The GE fetches the vertices by DMA from main memory. */
    sceKernelDcacheWritebackInvalidateRange(vertices, sizeof(Vertex) * 4);

    sceGuDrawArray(
        GU_TRIANGLE_FAN,
        GU_TEXTURE_32BITF |
        GU_VERTEX_32BITF |
        GU_TRANSFORM_2D,
        4, 0, vertices);

    /* Restore the state the following quads expect. */
    sceGuDisable(GU_BLEND);
}

/*
 * State Browser slot thumbnails (Stage 5): the occupied slots' Vector
 * screenshots, box-shrunk into one shared RGBA atlas by the worker.
 * The atlas is bound once, every VISIBLE occupied slot draws one
 * quad sampling its own tile at its scroll-window position. Opaque
 * pictures, no blending; bilinear like the ROM preview. Runs every
 * frame while the window is open.
 */
void TV::draw_state_thumbs(StateWindow & state)
{
    /* Rebuilt atlas must reach main memory before the GE samples it
     * by DMA; done once per rebuild, not per frame. */
    if (state.consume_thumb_upload()) {
        sceKernelDcacheWritebackInvalidateRange(
            (void *)state.thumb_tex_data(),
            (unsigned)(StateWindow::ATLAS_W
                       * StateWindow::ATLAS_H * sizeof(uint32_t)));
    }

    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, StateWindow::ATLAS_W, StateWindow::ATLAS_H,
                  StateWindow::ATLAS_W, state.thumb_tex_data());
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

    /* Panel-local rectangles moved onto the display: the panel is
     * centered the same way draw_popup_quad() centers it. */
    const float px = ((float)PSP_SCREEN_WIDTH
                      - (float)state.get_width()) / 2.0f;
    const float py = ((float)PSP_SCREEN_HEIGHT
                      - (float)state.get_height()) / 2.0f;

    struct Vertex {
        float u, v;
        float x, y, z;
    };

    const int first = state.first_visible();
    const int last = first + STATE_GRID_ROWS * STATE_GRID_COLS;
    for (int i = first; i < last; ++i) {
        if (!state.slot_has_thumb(i))
            continue;

        int tu, tv_, tw, th;
        state.thumb_tile(i, &tu, &tv_, &tw, &th);
        int rx, ry, rw, rh;
        StateWindow::thumb_rect(i, first, &rx, &ry, &rw, &rh);

        Vertex * vertices =
            (Vertex *)alloc_frame_vertices(sizeof(Vertex) * 4);
        vertices[0] = { (float)tu,        (float)tv_,
          px + (float)rx,           py + (float)ry,           0.0f };
        vertices[1] = { (float)(tu + tw), (float)tv_,
          px + (float)(rx + rw),    py + (float)ry,           0.0f };
        vertices[2] = { (float)(tu + tw), (float)(tv_ + th),
          px + (float)(rx + rw),    py + (float)(ry + rh),    0.0f };
        vertices[3] = { (float)tu,        (float)(tv_ + th),
          px + (float)rx,           py + (float)(ry + rh),    0.0f };

        /* The GE fetches the vertices by DMA from main memory. */
        sceKernelDcacheWritebackInvalidateRange(
            vertices, sizeof(Vertex) * 4);

        sceGuDrawArray(
            GU_TRIANGLE_FAN,
            GU_TEXTURE_32BITF |
            GU_VERTEX_32BITF |
            GU_TRANSFORM_2D,
            4, 0, vertices);
    }
}

/*
 * One textured quad of the machine picture: binds the framebuffer
 * window as an indexed texture (declared power-of-two width tex_w,
 * real row pitch tex_width) and scales the u0..u1 x v source window
 * onto the x/y/w/h screen rectangle.
 */
void TV::draw_tex_quad(uint8_t * src, int tex_w, float u0, float u1, float v,
                       float x, float y, float w, float h)
{
    sceGuTexImage(0, tex_w, TEX_H, this->tex_width, src);

    struct Vertex {
        float u, v;
        float x, y, z;
    };

    Vertex * vertices = (Vertex *)alloc_frame_vertices(sizeof(Vertex) * 4);
    vertices[0] = { u0, 0.0f, x,     y,     0.0f };
    vertices[1] = { u1, 0.0f, x + w, y,     0.0f };
    vertices[2] = { u1, v,    x + w, y + h, 0.0f };
    vertices[3] = { u0, v,    x,     y + h, 0.0f };

    /* The GE fetches the vertices by DMA from main memory. The pool
     * hands each call its own block, so the two border quads never
     * share one. */
    sceKernelDcacheWritebackInvalidateRange(vertices, sizeof(Vertex) * 4);

    sceGuDrawArray(
        GU_TRIANGLE_FAN,
        GU_TEXTURE_32BITF |
        GU_VERTEX_32BITF |
        GU_TRANSFORM_2D,
        4, 0, vertices);
}

int TV::get_refresh_rate() const
{
    return this->refresh_rate;
}

std::function<uint32_t(uint8_t,uint8_t,uint8_t)> TV::get_rgb2pixelformat() const
{
    return [](uint8_t r, uint8_t g, uint8_t b) {
        uint8_t R = (r << 5) | (r << 2) | (r >> 1);
        uint8_t G = (g << 5) | (g << 2) | (g >> 1);
        uint8_t B = (b << 6) | (b << 4) | (b << 2) | b;

        return 0xff000000u |
               (uint32_t(B) << 16) |
               (uint32_t(G) << 8) |
               R;
    };
}

void TV::handle_window_event(SDL_Event & event)
{
    /* Not used on PSP */
}
