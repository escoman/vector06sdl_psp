#include <string>
#include <inttypes.h>
#include <cstring>
#include <cstdio>
#include "globaldefs.h"
#include "event.h"
#include "options.h"
#include "font.h"
#include "tv.h"

#include <pspgu.h>
#include <pspgum.h>
#include <pspdisplay.h>
#include <pspkernel.h>
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
           texbuf(nullptr),
           fps_count(0), fps_last_us(0), fps_value(0), machine_fps(0),
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
    delete[] this->texbuf;
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

    if (Options.show_border) {
        this->texbuf = new uint8_t[TEX_W * TEX_H];
        memset(this->texbuf, 0, TEX_W * TEX_H);
        dbglog("TV::init: texbuf allocated\n");
    }

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
 * Copy a window of the source framebuffer into the texture staging
 * buffer using nearest-neighbour sampling. The 576-pixel wide border
 * window does not fit into the 512-pixel GU texture, so it is fitted
 * into 480x272 here.
 */
void TV::copy_bmt_to_texbuf(const uint8_t * src_buf,
                            int src_x, int src_y, int src_w, int src_h)
{
    const uint8_t * const src_base = src_buf
        + (size_t)src_y * this->tex_width + src_x;

    /* Source column for every output column, computed once. */
    int sx[BORDER_DST_W];
    for (int x = 0; x < BORDER_DST_W; x++) {
        sx[x] = (x * src_w) / BORDER_DST_W;
    }

    for (int y = 0; y < BORDER_DST_H; y++) {
        const int sy = (y * src_h) / BORDER_DST_H;
        const uint8_t * const src_row = src_base
            + (size_t)sy * this->tex_width;
        uint8_t * const dst_row = this->texbuf + (size_t)y * TEX_W;
        for (int x = 0; x < BORDER_DST_W; x++) {
            dst_row[x] = src_row[sx[x]];
        }
    }
}

/*
 * FPS/FRAMES overlay. The counters are drawn into the picture the GE
 * will display (the border staging buffer or the framebuffer window),
 * not into the PSP display buffer directly: the display buffer
 * contents written by the CPU are not what reaches the screen in the
 * pipelined GU mode. Glyphs come from font.h (8x8, bit 7 = leftmost).
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

void TV::draw_fps_overlay(uint8_t * buf, int stride, int ox, int oy)
{
    char text[24];
    snprintf(text, sizeof(text), "FPS: %d", this->fps_value);
    this->draw_overlay_line(buf, stride, ox, oy, text);

    snprintf(text, sizeof(text), "FRAMES: %d", this->get_machine_fps());
    this->draw_overlay_line(buf, stride, ox, oy + OVERLAY_FONT_H, text);
}

void TV::render()
{
    if (!Options.novideo) {
        dbglog("TV::render: start\n");

        /* Finish presenting the previously submitted list; only now is
         * its buffer safe to hand back to the worker. */
        if (this->pending) {
#ifdef AUTOSELECT_ROM
            unsigned perf_a = sceKernelGetSystemTimeLow();
#endif
            sceGuSync(0, 0);
            dbglog("TV::render: sceGuSync done\n");
#ifdef AUTOSELECT_ROM
            unsigned perf_b = sceKernelGetSystemTimeLow();
#endif

            sceDisplayWaitVblankStart();
            dbglog("TV::render: vblank done\n");
#ifdef AUTOSELECT_ROM
            unsigned perf_c = sceKernelGetSystemTimeLow();
            this->perf_sync_us += perf_b - perf_a;
            this->perf_vbl_us += perf_c - perf_b;
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
             * vblank rhythm, submit nothing. */
            sceDisplayWaitVblankStart();
            return;
        }

        sceGuStart(GU_DIRECT, list);
        dbglog("TV::render: sceGuStart OK\n");

#ifdef AUTOSELECT_ROM
        unsigned perf_f0 = sceKernelGetSystemTimeLow();
#endif
        float u, v, quad_x, quad_y, quad_w, quad_h;

        if (Options.show_border) {
        /* Full frame with border: the 576x272 window does not fit
         * into a 512-pixel wide GU texture, so it is downscaled into
         * the 480x272 staging buffer first. Without a new frame the
         * picture is unchanged and the staging buffer is reused. */
        if (fresh) {
            this->copy_bmt_to_texbuf(src_buf, 0, 8, SCREEN_W, SCREEN_H - 16);
        }
        if (Options.show_fps) {
            this->draw_fps_overlay(this->texbuf, TEX_W, 0, 0);
        }
        /* The GE reads the staging buffer by DMA: write it back. */
        sceKernelDcacheWritebackInvalidateRange(
            (void *)this->texbuf,
            (unsigned)(BORDER_DST_H * TEX_W));

        u = (float)BORDER_DST_W;
        v = (float)BORDER_DST_H;
        quad_x = 0.0f;
        quad_y = (float)BORDER_DST_Y;
        quad_w = (float)BORDER_DST_W;
        quad_h = (float)BORDER_DST_H;
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(32, clut_table);
        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
        sceGuTexImage(0, TEX_W, TEX_H, TEX_W, this->texbuf);
        /* texbuf is rewritten in place every executed frame; drop the
         * GE texture cache so the list never samples stale texels. */
        sceGuTexFlush();
        } else {
        /* The GE reads the framebuffer directly: the 576-pixel row
         * stride is a multiple of 16 bytes, which is all sceGuTexImage
         * needs. Only the shown window is writeback-invalidated instead
         * of the whole data cache. */
        uint8_t * const texsrc = src_buf
            + (size_t)VIDEO_Y * this->tex_width + VIDEO_X;
        if (Options.show_fps) {
            this->draw_fps_overlay(src_buf, this->tex_width,
                                   VIDEO_X, VIDEO_Y);
        }
        sceKernelDcacheWritebackInvalidateRange(
            (void *)texsrc,
            (unsigned)(VIDEO_H * this->tex_width));

        u = (float)VIDEO_W;
        v = (float)VIDEO_H;
        quad_x = (float)DST_X;
        quad_y = (float)DST_Y;
        quad_w = (float)DST_W;
        quad_h = (float)DST_H;
        sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
        sceGuClutLoad(32, clut_table);
        sceGuTexMode(GU_PSM_T8, 0, 0, 0);
        sceGuTexImage(0, TEX_W, TEX_H, this->tex_width, texsrc);
        }
#ifdef AUTOSELECT_ROM
        this->perf_flush_us += sceKernelGetSystemTimeLow() - perf_f0;
#endif

        dbglog("TV::render: texture flush done\n");

        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexFilter(GU_MODE, GU_MODE);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);

        dbglog("TV::render: texture upload done\n");

        struct Vertex {
            float u, v;
            float x, y, z;
        };

        Vertex __attribute__((aligned(16))) vertices[4] = {
            { 0.0f, 0.0f,
              quad_x, quad_y, 0.0f },

            { u, 0.0f,
              quad_x + quad_w, quad_y, 0.0f },

            { u, v,
              quad_x + quad_w, quad_y + quad_h, 0.0f },

            { 0.0f, v,
              quad_x, quad_y + quad_h, 0.0f },
        };

        /* The GE fetches the vertices by DMA straight from main memory;
         * the CPU has just written them through the data cache, so the
         * range must be written back or the GE sees stale bytes. On
         * PPSSPP this does not matter (no cache emulation), on real
         * hardware it made the picture appear only on some frames. */
        sceKernelDcacheWritebackInvalidateRange(vertices, sizeof(vertices));

        sceGuDrawArray(
            GU_TRIANGLE_FAN,
            GU_TEXTURE_32BITF |
            GU_VERTEX_32BITF |
            GU_TRANSFORM_2D,
            4, 0, vertices);

        dbglog("TV::render: draw array done\n");

        sceGuFinish();
        dbglog("TV::render: sceGuFinish done\n");

        /* No sync here: the GE keeps reading the texture buffer while
         * the CPU emulates the next machine frame. The sync + vblank +
         * swap happen at the top of the next call. */
        this->pending = true;
    }
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
