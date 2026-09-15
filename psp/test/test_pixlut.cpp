/*
 * Palette LUT correctness test — ТЗ №1 (LUT для преобразования палитры).
 *
 * Verifies that the new precomputed lookup table used by
 * IO::commit_palette() is bit-for-bit identical to the old std::function
 * conversion it replaced, for every possible Vector-06C hardware color.
 *
 * Standalone native test. Does NOT modify any production code.
 *
 * Build & run:
 *   g++ -std=c++17 -O2 -o test_pixlut test_pixlut.cpp && ./test_pixlut
 *
 * Three independent implementations are cross-checked:
 *   (A) old lambda   — faithful copy of TV::get_rgb2pixelformat() lambda
 *                      (psp/src/tv.cpp:819-831) + IO::commit_palette()
 *                      bit extraction (psp/src/vio.h, pre-change).
 *   (B) new LUT      — faithful copy of IO::build_pix_lut() fill and the
 *                      new IO::commit_palette() lookup (psp/src/vio.h).
 *   (C) GE clut      — faithful copy of the clut_table[] built in
 *                      TV::init() (psp/src/tv.cpp:156-164), which uses the
 *                      identical formula and is the value the GE expands.
 */

#include <cstdio>
#include <cstdint>
#include <cstdlib>

/* ------------------------------------------------------------------
 * (A) OLD path: rgb2pixelformat lambda invoked through std::function.
 * Verbatim body of TV::get_rgb2pixelformat() (tv.cpp:821-830).
 * ----------------------------------------------------------------*/
static uint32_t old_rgb2pixelformat(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t R = (r << 5) | (r << 2) | (r >> 1);
    uint8_t G = (g << 5) | (g << 2) | (g >> 1);
    uint8_t B = (b << 6) | (b << 4) | (b << 2) | b;

    return 0xff000000u |
           (uint32_t(B) << 16) |
           (uint32_t(G) << 8) |
           R;
}

/* Verbatim bit extraction of the pre-change IO::commit_palette(). */
static uint32_t old_commit_conv(int w8)
{
    int b = (w8 & 0xc0) >> 6;
    int g = (w8 & 0x38) >> 3;
    int r = (w8 & 0x07);
    return old_rgb2pixelformat(r, g, b);
}

/* ------------------------------------------------------------------
 * (B) NEW path: IO::build_pix_lut() + new IO::commit_palette() lookup.
 * Verbatim copies of the new vio.h code.
 * ----------------------------------------------------------------*/
static uint32_t pix_lut[512];

static void build_pix_lut()
{
    for (int i = 0; i < 512; ++i) {
        const int r = (i & 0x07);
        const int g = (i & 0x38) >> 3;
        const int b = (i & 0xc0) >> 6;
        pix_lut[i] = old_rgb2pixelformat(r, g, b);
    }
}

static uint32_t new_commit_conv(int w8)
{
    return pix_lut[w8 & 0x1ff];
}

/* ------------------------------------------------------------------
 * (C) GE CLUT: verbatim copy of the clut_table[] fill in TV::init()
 * (tv.cpp:156-164). Independent third implementation.
 * ----------------------------------------------------------------*/
static uint32_t clut_table[256];

static void build_clut_table()
{
    for (int i = 0; i < 256; ++i) {
        const uint32_t r = i & 0x07;
        const uint32_t g = (i >> 3) & 0x07;
        const uint32_t b = (i >> 6) & 0x03;
        const uint32_t R = (r << 5) | (r << 2) | (r >> 1);
        const uint32_t G = (g << 5) | (g << 2) | (g >> 1);
        const uint32_t B = (b << 6) | (b << 4) | (b << 2) | b;
        clut_table[i] = 0xff000000u | (B << 16) | (G << 8) | R;
    }
}

int main()
{
    build_pix_lut();
    build_clut_table();

    int failures = 0;
    int checked = 0;

    /* Required range: the full 0..511 span demanded by the ТЗ. */
    for (int v = 0; v <= 511; ++v) {
        const uint32_t a = old_commit_conv(v);   /* std::function path */
        const uint32_t b = new_commit_conv(v);   /* LUT path           */
        const uint32_t c = clut_table[v & 0xff]; /* GE CLUT            */
        ++checked;
        if (a != b || a != c) {
            ++failures;
            if (failures <= 16) {
                printf("MISMATCH v=%3d (0x%02x): old=%08x lut=%08x clut=%08x\n",
                       v, v & 0xff, a, b, c);
            }
        }
    }

    /* Extra safety: the conversion depends only on bits 0-7, so the LUT
     * lookup must stay bit-exact even for values above the hardware byte
     * range (the 0x1ff mask preserves bits 0-7). Scan a wide span. */
    for (int v = 0; v <= 0xffff; ++v) {
        const uint32_t a = old_commit_conv(v);
        const uint32_t b = new_commit_conv(v);
        ++checked;
        if (a != b) {
            ++failures;
            if (failures <= 16) {
                printf("MISMATCH(wide) v=%d: old=%08x lut=%08x\n", v, a, b);
            }
        }
    }

    printf("test_pixlut: checked=%d failures=%d\n", checked, failures);
    if (failures == 0) {
        printf("PASS: pix_lut[w8 & 0x1ff] == old std::function conversion "
               "== GE clut for all tested values\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
