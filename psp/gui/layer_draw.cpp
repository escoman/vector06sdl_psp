#include "layer_draw.h"
#include "layer.h"

#include <pspgu.h>
#include <pspkernel.h>

/* ---- UILayer default draw() ---- */

UILayer::~UILayer() {}

void UILayer::draw()
{
    layer_draw_centered_quad(
        tex_data(), tex_width(), tex_height(),
        clut_data(),
        (float)display_width(), (float)display_height(),
        consume_tex_upload());
}

/*
 * Shared GE drawing helpers — see layer_draw.h.
 *
 * The per-frame vertex pool is a small static buffer; each render
 * frame resets it at the top and all layers allocate their vertices
 * from it.  The sceGuSync at the start of TV::render() finishes any
 * previous GE work before the pool is reused.
 */

#define FRAME_VERTEX_POOL_BYTES 2048
static uint8_t frame_vertex_pool[FRAME_VERTEX_POOL_BYTES]
    __attribute__((aligned(16)));
static unsigned frame_vertex_used = 0;

void layer_draw_vertex_reset()
{
    frame_vertex_used = 0;
}

void * layer_draw_alloc_vertices(unsigned bytes)
{
    unsigned off = (frame_vertex_used + 15u) & ~15u;
    frame_vertex_used = off + bytes;
    return frame_vertex_pool + off;
}

/* ---- Internal: common CLUT/texture setup for indexed quads ---- */

static void setup_indexed_tex(const uint8_t * tex, int tex_w, int tex_h,
                               const uint32_t * clut, bool bilinear)
{
    sceGuClutMode(GU_PSM_8888, 0, 0xff, 0);
    sceGuClutLoad(32, clut);
    sceGuTexMode(GU_PSM_T8, 0, 0, 0);
    sceGuTexImage(0, tex_w, tex_h, tex_w, tex);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(bilinear ? GU_LINEAR : GU_NEAREST,
                   bilinear ? GU_LINEAR : GU_NEAREST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
}

/* ---- Internal: draw a quad from pre-built vertices ---- */

struct TexVertex {
    float u, v;
    float x, y, z;
};

static void draw_quad_vertices(TexVertex * v)
{
    sceKernelDcacheWritebackInvalidateRange(v, sizeof(TexVertex) * 4);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(
        GU_TRIANGLE_FAN,
        GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
        4, 0, v);
    sceGuDisable(GU_BLEND);
}

/* ---- Public API ---- */

void layer_draw_centered_quad(
    const uint8_t * tex, int tex_w, int tex_h,
    const uint32_t * clut,
    float disp_w, float disp_h,
    bool upload_flag)
{
    if (upload_flag) {
        sceKernelDcacheWritebackInvalidateRange(
            (void *)tex, (unsigned)(tex_w * tex_h));
    }

    setup_indexed_tex(tex, tex_w, tex_h, clut, false);

    const float x = ((float)LAYER_SCREEN_W - disp_w) / 2.0f;
    const float y = ((float)LAYER_SCREEN_H - disp_h) / 2.0f;

    TexVertex * v = (TexVertex *)layer_draw_alloc_vertices(
        sizeof(TexVertex) * 4);
    v[0] = { 0.0f,       0.0f,       x,           y,           0.0f };
    v[1] = { disp_w,     0.0f,       x + disp_w,  y,           0.0f };
    v[2] = { disp_w,     disp_h,     x + disp_w,  y + disp_h,  0.0f };
    v[3] = { 0.0f,       disp_h,     x,           y + disp_h,  0.0f };

    draw_quad_vertices(v);
}

void layer_draw_quad(
    const uint8_t * tex, int tex_w, int tex_h,
    const uint32_t * clut,
    float screen_x, float screen_y,
    float disp_w, float disp_h,
    float u0, float v0, float u1, float v1,
    bool bilinear,
    bool upload_flag)
{
    if (upload_flag) {
        sceKernelDcacheWritebackInvalidateRange(
            (void *)tex, (unsigned)(tex_w * tex_h));
    }

    setup_indexed_tex(tex, tex_w, tex_h, clut, bilinear);

    TexVertex * v = (TexVertex *)layer_draw_alloc_vertices(
        sizeof(TexVertex) * 4);
    v[0] = { u0,  v0,  screen_x,            screen_y,            0.0f };
    v[1] = { u1,  v0,  screen_x + disp_w,   screen_y,            0.0f };
    v[2] = { u1,  v1,  screen_x + disp_w,   screen_y + disp_h,   0.0f };
    v[3] = { u0,  v1,  screen_x,            screen_y + disp_h,   0.0f };

    draw_quad_vertices(v);
}

void layer_draw_rgba_quad(
    const uint32_t * tex, int tex_w, int tex_h,
    float screen_x, float screen_y,
    float disp_w, float disp_h,
    float u0, float v0, float u1, float v1,
    bool bilinear,
    bool upload_flag)
{
    if (upload_flag) {
        sceKernelDcacheWritebackInvalidateRange(
            (void *)tex, (unsigned)(tex_w * tex_h * sizeof(uint32_t)));
    }

    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, tex_w, tex_h, tex_w, tex);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(bilinear ? GU_LINEAR : GU_NEAREST,
                   bilinear ? GU_LINEAR : GU_NEAREST);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);

    TexVertex * v = (TexVertex *)layer_draw_alloc_vertices(
        sizeof(TexVertex) * 4);
    v[0] = { u0,  v0,  screen_x,            screen_y,            0.0f };
    v[1] = { u1,  v0,  screen_x + disp_w,   screen_y,            0.0f };
    v[2] = { u1,  v1,  screen_x + disp_w,   screen_y + disp_h,   0.0f };
    v[3] = { u0,  v1,  screen_x,            screen_y + disp_h,   0.0f };

    draw_quad_vertices(v);
}
