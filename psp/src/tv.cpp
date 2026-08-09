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

/* PSP GU rendering: 576x288 Vector-06C framebuffer, visible 512x256
 * picture is cropped and scaled to 480x272 screen. */

#define PSP_FB_WIDTH 512
#define PSP_FB_HEIGHT 272
#define PSP_FB_STRIDE 512

static unsigned int __attribute__((aligned(16))) list[262144];
static unsigned int *fbp0 = 0;
static unsigned int *fbp1 = 0;
static bool gu_initialized = false;

/* PSP GU max texture size for 32-bit (GU_PSM_8888) is 512x512 */
#define TEX_W 512
#define TEX_H 512

/* Visible Vector-06C picture region within the 576x288 framebuffer:
 * The picture 512x256 is written starting at bmp[0] (top-left).
 * The remaining columns (64) and rows (32) are the right/bottom borders. */
#define PIC_X 0
#define PIC_Y 0
#define PIC_W 512
#define PIC_H 256

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

    /* Allocate pixel buffer for Vector-06C framebuffer */
    this->bmp = new uint32_t[Options.screen_width * Options.screen_height];
    memset(this->bmp, 0, Options.screen_width * Options.screen_height * 4);
    dbglog("TV::init: bmp allocated\n");

    /* Allocate power-of-two texture buffer (required by PSP GU) */
    this->texbuf = new uint32_t[TEX_W * TEX_H];
    memset(this->texbuf, 0, TEX_W * TEX_H * 4);
    dbglog("TV::init: texbuf allocated\n");

    this->tex_width = Options.screen_width;
    this->tex_height = Options.screen_height;
    this->refresh_rate = 50; /* PAL */

    /* Initialize PSP GU */
    if (!gu_initialized) {
        dbglog("TV::init: initializing GU...\n");
        sceGuInit();
        sceGuStart(GU_DIRECT, list);

        /* Allocate framebuffers from VRAM using the proper PSPSDK helper.
         * sceGuGetMemory() allocates from the display list, NOT VRAM,
         * and is invalidated on the next sceGuStart(). */
        fbp0 = (unsigned int*)guGetStaticVramBuffer(
            PSP_FB_WIDTH, PSP_FB_HEIGHT, GU_PSM_8888);
        fbp1 = (unsigned int*)guGetStaticVramBuffer(
            PSP_FB_WIDTH, PSP_FB_HEIGHT, GU_PSM_8888);

        sceGuDrawBuffer(GU_PSM_8888, fbp0, PSP_FB_STRIDE);
        sceGuDispBuffer(PSP_FB_WIDTH, PSP_FB_HEIGHT, fbp1, PSP_FB_STRIDE);

        sceGuOffset(2048 - (PSP_FB_WIDTH / 2), 2048 - (PSP_FB_HEIGHT / 2));
        sceGuViewport(2048, 2048, PSP_FB_WIDTH, PSP_FB_HEIGHT);

        sceGuScissor(0, 0, PSP_FB_WIDTH, PSP_FB_HEIGHT);
        sceGuEnable(GU_SCISSOR_TEST);
        /* Simple 2D pipeline: no alpha, no depth test needed */
        sceGuDisable(GU_ALPHA_TEST);
        sceGuDisable(GU_DEPTH_TEST);
        sceGuFrontFace(GU_CW);
        sceGuShadeModel(GU_FLAT);
        sceGuDisable(GU_LIGHTING);
        sceGuDisable(GU_CULL_FACE);
        sceGuEnable(GU_TEXTURE_2D);
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
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
    /* Not supported on PSP */
}

uint32_t* TV::pixels() const {
    return this->bmp;
}

void TV::render(int executed)
{
    if (!Options.novideo) {
        dbglog("TV::render: start\n");
        /* Upload framebuffer as texture and draw scaled quad */
        sceGuStart(GU_DIRECT, list);
        dbglog("TV::render: sceGuStart OK\n");

        /* Crop the visible 512x256 picture region from the 576x288
         * framebuffer into the power-of-two 512x512 texture.
         * The framebuffer row stride is tex_width (576). */
        uint32_t * src = this->bmp + PIC_Y * this->tex_width + PIC_X;
        uint32_t * dst = this->texbuf;
        for (int y = 0; y < PIC_H; ++y) {
            memcpy(dst, src, PIC_W * 4);
            src += this->tex_width;
            dst += TEX_W;
        }
        dbglog("TV::render: texture copy done\n");

        /* Flush CPU writeback cache so the GPU sees the updated texture */
        sceKernelDcacheWritebackInvalidateAll();

        /* Upload the texture (power-of-two 512x512) */
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, TEX_W, TEX_H, TEX_W, this->texbuf);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);
        dbglog("TV::render: texture upload done\n");

        /* UV coordinates: only the top-left 512x256 region of the
         * 512x512 texture is used. */
        float u = (float)PIC_W / TEX_W;
        float v = (float)PIC_H / TEX_H;

        /* Draw a fullscreen quad with the texture */
        struct Vertex {
            float u, v;
            float x, y, z;
        } vertices[4] = {
            { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
            { u,    0.0f, PSP_FB_WIDTH, 0.0f, 0.0f },
            { u,    v,    PSP_FB_WIDTH, PSP_FB_HEIGHT, 0.0f },
            { 0.0f, v,    0.0f, PSP_FB_HEIGHT, 0.0f },
        };

        sceGuDrawArray(GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
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
    /* PSP uses ABGR8888 */
    return [](uint8_t r, uint8_t g, uint8_t b) {
        uint32_t result =
            0xff000000 |
            (b << 16) |
            (g << 8) |
            (r << 0);
         return result;
    };
}

void TV::handle_window_event(SDL_Event & event)
{
    /* Not used on PSP */
}
