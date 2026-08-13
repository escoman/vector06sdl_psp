#include <string>
#include <inttypes.h>
#include <cstring>
#include <cstdio>
#include "globaldefs.h"
#include "event.h"
#include "options.h"
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

TV::TV() : wr(0), last(0), pending(false), texbuf(nullptr),
           fps_count(0), fps_last_us(0), fps_value(0),
           pixelformat(TV_PIXELFORMAT)
{
    this->bmp[0] = this->bmp[1] = 0;
}

TV::~TV()
{
    delete[] this->bmp[0];
    delete[] this->bmp[1];
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

    /* Two framebuffers: GE textures from one while the filler draws
     * the next machine frame into the other (see TV::render). */
    for (int i = 0; i < 2; ++i) {
        this->bmp[i] = new uint32_t[Options.screen_width * Options.screen_height];
        memset(this->bmp[i], 0,
               Options.screen_width * Options.screen_height * sizeof(uint32_t));
    }
    dbglog("TV::init: bmp[2] allocated\n");

    if (Options.show_border) {
        this->texbuf = new uint32_t[TEX_W * TEX_H];
        memset(this->texbuf, 0, TEX_W * TEX_H * sizeof(uint32_t));
        dbglog("TV::init: texbuf allocated\n");
    }

    this->tex_width = Options.screen_width;
    this->tex_height = Options.screen_height;
    /* PSP LCD runs at 60 Hz while the Vector machine needs 50 fps.
     * Board::init() passes this value to cadence, which builds the 6:5
     * pullup pattern: exactly 50 machine frames per 60 vblank-locked
     * loop iterations. With the old 50 (1:1 cadence) the machine ran
     * 20% too fast and audio generation outpaced the 44.1 kHz callback,
     * overrunning the ring buffer. */
    this->refresh_rate = 60;

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
    // TV::bmp contains ABGR8888 as 0xAABBGGRR.
    const uint32_t * const srcbuf =
        this->last ? this->last : this->bmp[this->wr];
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            uint32_t p = srcbuf[y * width + x];

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

uint32_t* TV::pixels() const
{
    return this->bmp[this->wr];
}

/*
 * Copy a window of the source framebuffer into the texture staging
 * buffer using nearest-neighbour sampling. The 576-pixel wide border
 * window does not fit into the 512-pixel GU texture, so it is fitted
 * into 480x272 here.
 */
void TV::copy_bmt_to_texbuf(const uint32_t * src_buf,
                            int src_x, int src_y, int src_w, int src_h)
{
    const uint32_t * const src_base = src_buf
        + (size_t)src_y * this->tex_width + src_x;

    /* Source column for every output column, computed once. */
    int sx[BORDER_DST_W];
    for (int x = 0; x < BORDER_DST_W; x++) {
        sx[x] = (x * src_w) / BORDER_DST_W;
    }

    for (int y = 0; y < BORDER_DST_H; y++) {
        const int sy = (y * src_h) / BORDER_DST_H;
        const uint32_t * const src_row = src_base
            + (size_t)sy * this->tex_width;
        uint32_t * const dst_row = this->texbuf + (size_t)y * TEX_W;
        for (int x = 0; x < BORDER_DST_W; x++) {
            dst_row[x] = src_row[sx[x]];
        }
    }
}

/*
 * FPS overlay. The counter is drawn into the picture the GE will
 * display (the border staging buffer or the framebuffer window), not
 * into the PSP display buffer directly: the display buffer contents
 * written by the CPU are not what reaches the screen in the pipelined
 * GU mode. 8x8 font glyphs taken from the pspDebugScreen font
 * (pspsdk libdebug); bit 7 of a row byte is the leftmost pixel.
 */
static const uint8_t fps_overlay_font[][8] = {
    { 0xf8, 0x80, 0x80, 0xf0, 0x80, 0x80, 0x80, 0x00 }, /* F */
    { 0xf0, 0x88, 0x88, 0xf0, 0x80, 0x80, 0x80, 0x00 }, /* P */
    { 0x70, 0x88, 0x80, 0x70, 0x08, 0x88, 0x70, 0x00 }, /* S */
    { 0x00, 0x00, 0x20, 0x00, 0x00, 0x20, 0x00, 0x00 }, /* : */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, /*   */
    { 0x70, 0x88, 0x98, 0xa8, 0xc8, 0x88, 0x70, 0x00 }, /* 0 */
    { 0x20, 0x60, 0xa0, 0x20, 0x20, 0x20, 0xf8, 0x00 }, /* 1 */
    { 0x70, 0x88, 0x08, 0x10, 0x60, 0x80, 0xf8, 0x00 }, /* 2 */
    { 0x70, 0x88, 0x08, 0x30, 0x08, 0x88, 0x70, 0x00 }, /* 3 */
    { 0x10, 0x30, 0x50, 0x90, 0xf8, 0x10, 0x10, 0x00 }, /* 4 */
    { 0xf8, 0x80, 0xe0, 0x10, 0x08, 0x10, 0xe0, 0x00 }, /* 5 */
    { 0x30, 0x40, 0x80, 0xf0, 0x88, 0x88, 0x70, 0x00 }, /* 6 */
    { 0xf8, 0x88, 0x10, 0x20, 0x20, 0x20, 0x20, 0x00 }, /* 7 */
    { 0x70, 0x88, 0x88, 0x70, 0x88, 0x88, 0x70, 0x00 }, /* 8 */
    { 0x70, 0x88, 0x88, 0x78, 0x08, 0x10, 0x60, 0x00 }, /* 9 */
};

static const char FPS_OVERLAY_CHARS[] = "FPS: 0123456789";

void TV::draw_fps_overlay(uint32_t * buf, int stride, int ox, int oy)
{
    char text[16];
    snprintf(text, sizeof(text), "FPS: %d", this->fps_value);

    for (int i = 0; text[i] != '\0'; ++i) {
        const char * f = strchr(FPS_OVERLAY_CHARS, text[i]);
        if (f == nullptr) {
            continue;
        }
        const uint8_t * g = fps_overlay_font[f - FPS_OVERLAY_CHARS];
        for (int y = 0; y < 8; ++y) {
            uint32_t * dst = buf + (size_t)(oy + y) * stride + ox + i * 8;
            uint8_t row = g[y];
            for (int x = 0; x < 8; ++x) {
                dst[x] = (row & (0x80u >> x))
                    ? 0xFFFFFFFFu   /* glyph: white */
                    : 0xFF000000u;  /* cell background: black */
            }
        }
    }
}

void TV::render(int executed)
{
    if (!Options.novideo) {
        dbglog("TV::render: start\n");

        /* Buffer to present: the one the machine just drew, or — on a
         * cadence skip frame — the previous one shown again. */
        uint32_t * const src_buf = executed
            ? this->bmp[this->wr]
            : (this->last ? this->last : this->bmp[this->wr]);
        if (executed) {
            this->last = src_buf;
            /* the filler picks up the other buffer in Filler::reset() */
            this->wr ^= 1;
        }

        /* Finish presenting the previously submitted list; only now is
         * its buffer safe to overwrite again. */
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

        sceGuStart(GU_DIRECT, list);
        dbglog("TV::render: sceGuStart OK\n");

#ifdef AUTOSELECT_ROM
        unsigned perf_f0 = sceKernelGetSystemTimeLow();
#endif
        float u, v, quad_x, quad_y, quad_w, quad_h;

        if (Options.show_border) {
        /* Full frame with border: the 576x272 window does not fit
         * into a 512-pixel wide GU texture, so it is downscaled into
         * the 480x272 staging buffer first. On a cadence skip frame
         * the picture is unchanged and the staging buffer is reused. */
        if (executed) {
            this->copy_bmt_to_texbuf(src_buf, 0, 8, SCREEN_W, SCREEN_H - 16);
        }
        if (Options.show_fps) {
            this->draw_fps_overlay(this->texbuf, TEX_W, 0, 0);
        }
        /* The GE reads the staging buffer by DMA: write it back. */
        sceKernelDcacheWritebackInvalidateRange(
            (void *)this->texbuf,
            (unsigned)(BORDER_DST_H * TEX_W * sizeof(uint32_t)));

        u = (float)BORDER_DST_W;
        v = (float)BORDER_DST_H;
        quad_x = 0.0f;
        quad_y = (float)BORDER_DST_Y;
        quad_w = (float)BORDER_DST_W;
        quad_h = (float)BORDER_DST_H;
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, TEX_W, TEX_H, TEX_W, this->texbuf);
        } else {
        /* The GE reads the framebuffer directly: the 576-pixel row
         * stride is a multiple of 16 bytes, which is all sceGuTexImage
         * needs. Only the shown window is writeback-invalidated instead
         * of the whole data cache. */
        uint32_t * const texsrc = src_buf
            + (size_t)VIDEO_Y * this->tex_width + VIDEO_X;
        if (Options.show_fps) {
            this->draw_fps_overlay(src_buf, this->tex_width,
                                   VIDEO_X, VIDEO_Y);
        }
        sceKernelDcacheWritebackInvalidateRange(
            (void *)texsrc,
            (unsigned)(VIDEO_H * this->tex_width * sizeof(uint32_t)));

        u = (float)VIDEO_W;
        v = (float)VIDEO_H;
        quad_x = (float)DST_X;
        quad_y = (float)DST_Y;
        quad_w = (float)DST_W;
        quad_h = (float)DST_H;
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
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
