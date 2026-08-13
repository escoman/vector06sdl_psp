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
 * SHOW_BORDER=0 -> 512x256 picture area
 * SHOW_BORDER=1 -> complete 576x288 framebuffer
 *
 * Both source areas have 2:1 aspect ratio and are fitted to 480x240,
 * preserving the original geometry. The image is centered vertically
 * on the 480x272 PSP display (16 pixels above and below).
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

TV::TV() : wr(0), last(0), pending(false), pixelformat(TV_PIXELFORMAT)
{
    this->bmp[0] = this->bmp[1] = 0;
#if SHOW_BORDER
    this->texbuf = nullptr;
#endif
}

TV::~TV()
{
    delete[] this->bmp[0];
    delete[] this->bmp[1];
#if SHOW_BORDER
    delete[] this->texbuf;
#endif
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

#if SHOW_BORDER
    this->texbuf = new uint32_t[TEX_W * TEX_H];
    memset(this->texbuf, 0, TEX_W * TEX_H * sizeof(uint32_t));
    dbglog("TV::init: texbuf allocated\n");
#endif

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
        }

        sceGuStart(GU_DIRECT, list);
        dbglog("TV::render: sceGuStart OK\n");

#ifdef AUTOSELECT_ROM
        unsigned perf_f0 = sceKernelGetSystemTimeLow();
#endif
#if SHOW_BORDER
        /* Full frame with border: the 576x272 window does not fit
         * into a 512-pixel wide GU texture, so it is downscaled into
         * the 480x240 staging buffer first. On a cadence skip frame
         * the picture is unchanged and the staging buffer is reused. */
        if (executed) {
            this->copy_bmt_to_texbuf(src_buf, 0, 8, SCREEN_W, SCREEN_H - 16);
            sceKernelDcacheWritebackInvalidateRange(
                (void *)this->texbuf,
                (unsigned)(BORDER_DST_H * TEX_W * sizeof(uint32_t)));
        }

        const float u = (float)BORDER_DST_W;
        const float v = (float)BORDER_DST_H;
        const float quad_x = 0.0f;
        const float quad_y = (float)BORDER_DST_Y;
        const float quad_w = (float)BORDER_DST_W;
        const float quad_h = (float)BORDER_DST_H;
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, TEX_W, TEX_H, TEX_W, this->texbuf);
#else
        /* The GE reads the framebuffer directly: the 576-pixel row
         * stride is a multiple of 16 bytes, which is all sceGuTexImage
         * needs. Only the shown window is writeback-invalidated instead
         * of the whole data cache. */
        uint32_t * const texsrc = src_buf
            + (size_t)VIDEO_Y * this->tex_width + VIDEO_X;
        sceKernelDcacheWritebackInvalidateRange(
            (void *)texsrc,
            (unsigned)(VIDEO_H * this->tex_width * sizeof(uint32_t)));

        const float u = (float)VIDEO_W;
        const float v = (float)VIDEO_H;
        const float quad_x = (float)DST_X;
        const float quad_y = (float)DST_Y;
        const float quad_w = (float)DST_W;
        const float quad_h = (float)DST_H;
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, TEX_W, TEX_H, this->tex_width, texsrc);
#endif
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

        Vertex vertices[4] = {
            { 0.0f, 0.0f,
              quad_x, quad_y, 0.0f },

            { u, 0.0f,
              quad_x + quad_w, quad_y, 0.0f },

            { u, v,
              quad_x + quad_w, quad_y + quad_h, 0.0f },

            { 0.0f, v,
              quad_x, quad_y + quad_h, 0.0f },
        };

        sceGuDrawArray(
            GU_TRIANGLE_FAN,
            GU_TEXTURE_32BITF |
            GU_VERTEX_32BITF |
            GU_TRANSFORM_2D,
            4, 0, vertices);

        dbglog("TV::render: draw array done\n");

        sceGuFinish();
        dbglog("TV::render: sceGuFinish done\n");

        /* No sync here: the GE keeps reading src_buf while the CPU
         * emulates the next machine frame into the other buffer. The
         * sync + vblank + swap happen at the top of the next call. */
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
