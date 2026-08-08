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

/* PSP GU rendering: 576x288 Vector-06C framebuffer scaled to 480x272 */

#define PSP_FB_WIDTH 512
#define PSP_FB_HEIGHT 272
#define PSP_FB_STRIDE 512

static unsigned int __attribute__((aligned(16))) list[262144];
static unsigned int __attribute__((aligned(16))) fbp0[PSP_FB_WIDTH * PSP_FB_HEIGHT];
static unsigned int __attribute__((aligned(16))) fbp1[PSP_FB_WIDTH * PSP_FB_HEIGHT];
static bool gu_initialized = false;

TV::TV() : pixelformat(TV_PIXELFORMAT)
{
}

TV::~TV()
{
    if (this->bmp) {
        delete[] bmp;
    }
}

int TV::probe()
{
    return 0;
}

void TV::init()
{
    /* Allocate pixel buffer for Vector-06C framebuffer */
    this->bmp = new uint32_t[Options.screen_width * Options.screen_height];
    memset(this->bmp, 0, Options.screen_width * Options.screen_height * 4);

    this->tex_width = Options.screen_width;
    this->tex_height = Options.screen_height;
    this->refresh_rate = 50; /* PAL */

    /* Initialize PSP GU */
    if (!gu_initialized) {
        sceGuInit();
        sceGuStart(GU_DIRECT, list);

        sceGuDrawBuffer(GU_PSM_8888, fbp0, PSP_FB_STRIDE);
        sceGuDispBuffer(PSP_FB_WIDTH, PSP_FB_HEIGHT, fbp1, PSP_FB_STRIDE);
        sceGuDepthBuffer((void*)0x110000, PSP_FB_STRIDE);

        sceGuOffset(2048 - (PSP_FB_WIDTH / 2), 2048 - (PSP_FB_HEIGHT / 2));
        sceGuViewport(2048, 2048, PSP_FB_WIDTH, PSP_FB_HEIGHT);
        sceGuDepthRange(0xc350, 0x2710);

        sceGuScissor(0, 0, PSP_FB_WIDTH, PSP_FB_HEIGHT);
        sceGuEnable(GU_SCISSOR_TEST);
        sceGuAlphaFunc(GU_GREATER, 0, 0xff);
        sceGuEnable(GU_ALPHA_TEST);
        sceGuDepthFunc(GU_GEQUAL);
        sceGuEnable(GU_DEPTH_TEST);
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
        /* Upload framebuffer as texture and draw scaled quad */
        sceGuStart(GU_DIRECT, list);

        /* Upload the Vector-06C framebuffer as a texture */
        sceGuTexMode(GU_PSM_8888, 0, 0, 0);
        sceGuTexImage(0, this->tex_width, this->tex_height, this->tex_width,
                      this->bmp);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        sceGuTexScale(1.0f, 1.0f);
        sceGuTexOffset(0.0f, 0.0f);

        /* Draw a fullscreen quad with the texture */
        struct Vertex {
            float u, v;
            float x, y, z;
        } vertices[4] = {
            { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
            { 1.0f, 0.0f, PSP_FB_WIDTH, 0.0f, 0.0f },
            { 1.0f, 1.0f, PSP_FB_WIDTH, PSP_FB_HEIGHT, 0.0f },
            { 0.0f, 1.0f, 0.0f, PSP_FB_HEIGHT, 0.0f },
        };

        sceGuDrawArray(GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       4, 0, vertices);

        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
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
