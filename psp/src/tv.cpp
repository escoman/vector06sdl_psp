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

/*
 * 0 - picture only
 * 1 - complete Vector-06C framebuffer including border
 */
#define SHOW_BORDER 0

/* Destination image size on PSP. */
#define DST_W 480
#define DST_H 272
#define DST_X 0
#define DST_Y 0

#define GU_MODE GU_LINEAR

TV::TV() : bmp(0), texbuf(0), pixelformat(TV_PIXELFORMAT)
{
}

TV::~TV()
{
    if (this->bmp) {
        delete[] bmp;
    }
    if (this->texbuf) {
        delete[] texbuf;
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

    this->bmp = new uint32_t[Options.screen_width * Options.screen_height];
    memset(this->bmp, 0,
           Options.screen_width * Options.screen_height * sizeof(uint32_t));
    dbglog("TV::init: bmp allocated\n");

    this->texbuf = new uint32_t[TEX_W * TEX_H];
    memset(this->texbuf, 0, TEX_W * TEX_H * sizeof(uint32_t));
    dbglog("TV::init: texbuf allocated\n");

    this->tex_width = Options.screen_width;
    this->tex_height = Options.screen_height;
    this->refresh_rate = 50; /* PAL */

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
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            uint32_t p = this->bmp[y * width + x];

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
    return this->bmp;
}

void TV::render(int executed)
{
    if (!Options.novideo) {
        dbglog("TV::render: start\n");

        sceGuStart(GU_DIRECT, list);
        dbglog("TV::render: sceGuStart OK\n");

#if SHOW_BORDER
        const int src_x = 0;
        const int src_y = 8;
        const int src_w = SCREEN_W;
        const int src_h = SCREEN_H-16;
#else
        const int src_x = VIDEO_X;
        const int src_y = VIDEO_Y;
        const int src_w = VIDEO_W;
        const int src_h = VIDEO_H;
#endif

        /*
         * Resample the selected Vector-06C area to 480x240.
         *
         * 512x256 -> 480x240
         * 576x288 -> 480x240
         *
         * Both preserve the original 2:1 aspect ratio.
         */
        uint32_t *src =
            this->bmp + src_y * this->tex_width + src_x;

        uint32_t *dst = this->texbuf;

        for (int y = 0; y < DST_H; ++y) {
            const int sy = (y * src_h) / DST_H;
            const uint32_t *src_row =
                src + sy * this->tex_width;

            for (int x = 0; x < DST_W; ++x) {
                const int sx = (x * src_w) / DST_W;
                dst[y * TEX_W + x] = src_row[sx];
            }
        }

        /*
         * Clear the unused texture area. Only the 480x240 top-left
         * rectangle is sampled by the GU.
         */
        for (int y = 0; y < TEX_H; ++y) {
            if (y < DST_H) {
                for (int x = DST_W; x < TEX_W; ++x) {
                    texbuf[y * TEX_W + x] = 0xff000000;
                }
            } else {
                memset(&texbuf[y * TEX_W],
                       0,
                       TEX_W * sizeof(uint32_t));
            }
        }

        dbglog("TV::render: texture copy/resample done\n");

        sceKernelDcacheWritebackInvalidateAll();

        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, TEX_W, TEX_H, TEX_W, this->texbuf);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexFilter(GU_MODE, GU_MODE);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);

        dbglog("TV::render: texture upload done\n");

        /*
         * PSP GU texture coordinates are in texels, not normalized 0..1.
         */
        const float u = (float)DST_W;
        const float v = (float)DST_H;

        struct Vertex {
            float u, v;
            float x, y, z;
        };

        Vertex vertices[4] = {
            { 0.0f, 0.0f,
              (float)DST_X, (float)DST_Y, 0.0f },

            { u, 0.0f,
              (float)(DST_X + DST_W), (float)DST_Y, 0.0f },

            { u, v,
              (float)(DST_X + DST_W),
              (float)(DST_Y + DST_H), 0.0f },

            { 0.0f, v,
              (float)DST_X,
              (float)(DST_Y + DST_H), 0.0f },
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

        sceGuSync(0, 0);
        dbglog("TV::render: sceGuSync done\n");

        sceDisplayWaitVblankStart();
        dbglog("TV::render: vblank done\n");

        sceGuSwapBuffers();
        dbglog("TV::render: swap done\n");
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
