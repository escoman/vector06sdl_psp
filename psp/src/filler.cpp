#include <cstring>
#include "globaldefs.h"
#include "filler.h"

// TODO: fix neon shimmering
#undef __ARM_NEON

PixelFiller::PixelFiller(Memory & _mem, IO & _io, TV & _tv): 
    memory(_mem), io(_io), tv(_tv)
{
    this->mode512 = false;
    this->mem32 = (uint32_t *) this->memory.buffer();
    this->pixel32 = 0;  // 4 bytes of bit planes
    this->border_index = 0;
    this->raster_pixel = 0;
    this->fast = false;  // Options.fast_framebuffer, picked up in init()

    this->reset();

    this->io.onborderchange = [this](int border) {
        //printf("onborderchange: %x\n", border);
        this->border_index = border;
    };

    this->io.onmodechange = [this](bool mode) {
        this->mode512 = mode;
    };
}

void PixelFiller::init()
{
    this->first_visible_line = 312 - Options.screen_height;
    this->center_offset = Options.center_offset;
    this->screen_width = Options.screen_width;
    this->fast = Options.fast_framebuffer;
}

void PixelFiller::reset()
{
    // It is tempting to reset the pixel count but the beam is reset in 
    // advanceLine(), don't do that here.
    //this->raster_pixel = 0;   // horizontal pixel counter

    this->raster_line = 0;    // raster line counter
    this->fb_column = 0;      // frame buffer column
    this->fb_row = 0;         // frame buffer row
    this->vborder = true;     // vertical border flag
    this->visible = false;    // visible area flag
    this->bmpofs = 0;         // bitmap offset for current pixel
    this->brk = false;
    this->irq = false;
    /* The framebuffer comes from the worker via set_framebuffer();
     * it is already set when the worker starts a machine frame. */
}

#if USE_BIT_PERMUTE
static uint32_t bit_permute_step(uint32_t x, uint32_t m, uint32_t shift) {
    uint32_t t;
    t = ((x >> shift) ^ x) & m;
    x = (x ^ t) ^ (t << shift);
    return x;
}
#endif

void PixelFiller::fetchPixels() 
{
    size_t addr = ((this->fb_column & 0xff) << 8) | (this->fb_row & 0xff);
    this->pixel32 = this->mem32[0x2000 + addr];

#if USE_BIT_PERMUTE
    // h/t Code generator for bit permutations
    // http://programming.sirrida.de/calcperm.php
    // Input:
    // 31 23 15 7 30 22 14 6 29 21 13 5 28 20 12 4 27 19 11 3 26 18 10 2 25 17 9 1  24 16 8 0
    // LSB, indices refer to source, Beneš/BPC
    uint32_t x = this->pixel32;
    x = bit_permute_step(x, 0x00550055, 9);  // Bit index swap+complement 0,3
    x = bit_permute_step(x, 0x00003333, 18);  // Bit index swap+complement 1,4
    x = bit_permute_step(x, 0x000f000f, 12);  // Bit index swap+complement 2,3
    x = bit_permute_step(x, 0x000000ff, 24);  // Bit index swap+complement 3,4

    this->pixel32_grouped = x;
#endif
}

int PixelFiller::shiftOutPixels()
{
//        uint32_t p = this->pixel32;
//        // msb of every byte in p stands for bit plane
//        uint32_t modeless = (p >> 4 & 8) | (p >> 13 & 4) | (p >> 22 & 2) | (p >> 31 & 1);
//        // shift left
//        this->pixel32 = (p << 1);// & 0xfefefefe; -- unnecessary
//        return modeless;
#if USE_BIT_PERMUTE
    uint32_t modeless = this->pixel32_grouped >> 28;
    this->pixel32_grouped <<= 4;
#else
    uint32_t p = this->pixel32;
    // msb of every byte in p stands for bit plane
    uint32_t modeless = (p >> 4 & 8) | (p >> 13 & 4) | (p >> 22 & 2) | (p >> 31 & 1);
    // shift left
    this->pixel32 = (p << 1);// & 0xfefefefe; -- unnecessary
#endif
    return modeless;
}

int PixelFiller::getColorIndex(int rpixel, bool border) {
    if (border) {
        this->fb_column = 0;
        return this->border_index;
    } else {
        if ((rpixel & 0x0f) == 0) {
            this->fetchPixels();
            ++this->fb_column;
        }
        return this->shiftOutPixels();
    }
}

#define TESTTABLE 0

int PixelFiller::fill(int clocks, int commit_time, 
        int commit_time_pal, bool updateScreen) 
{
    /* commit_time/commit_time_pal are -1 when no OUT is pending: a
     * plain boolean test treats -1 as true and the fast fill2/3/4
     * paths never run (profiler: fill1 did all 65k pixels per frame) */
    if (TESTTABLE || commit_time != -1 || commit_time_pal != -1 || 
            this->raster_line == 22 + 18 || 
            this->raster_line == 0 ||
            this->raster_line == 311 ||
            this->raster_pixel <= (768-512)/2 + clocks ||
            this->raster_pixel + clocks >= 768-(768-512)/2)
    {
        fill1_count += clocks;
        if (this->fast) {
            /* No i/o commit pending and not on an event line (irq at
             * line 0 pixel 174, scroll load at line 40 pixel 150,
             * brk at line 311): fill1_nodraw would only step the
             * raster counters two units at a time, so jump straight
             * to the end. clocks never spans more than one line
             * (longest instruction is 18 T-states = 72 units), so at
             * most one advanceLine is needed and afterbrk stays 0. */
            if (commit_time == -1 && commit_time_pal == -1 &&
                    this->raster_line != 0 &&
                    this->raster_line != 22 + 18 &&
                    this->raster_line != 311) {
                this->raster_pixel += clocks;
                if (this->raster_pixel >= 768) {
                    this->raster_pixel -= 768;
                    this->advanceLine(updateScreen);
                }
                return 0;
            }
            return fill1_nodraw(clocks, commit_time, commit_time_pal, updateScreen);
        }
        return fill1(clocks, commit_time, commit_time_pal, updateScreen);
    } else {
        fill2_count += clocks;
        if (this->fast) {
            /* No OUT underway, away from the line edges and the
             * scroll/irq moments: the raster fast paths (fill2/3/4)
             * would only paint pixels, which render_full_frame()
             * rebuilds in one pass after the machine frame. */
            this->raster_pixel += clocks;
            return 0;
        }
        if (!this->visible) {
            this->raster_pixel += clocks;
            return 0;
        }
        else if (this->vborder) {
            return fill4(clocks);
        }
        else if (this->mode512) {
            return fill3(clocks);
        } 
        else {
            return fill2(clocks);
        }
    }
}

int PixelFiller::fill1(int clocks, int commit_time, int commit_time_pal, bool updateScreen) {
    uint8_t * bmp = this->pixels;
    int clk;
    int afterbrk = 0;
    int index = 0;

    for (clk = 0; clk < clocks; clk += 2, afterbrk += this->brk ? 2 : 0) {
        // offset for matching border/palette writes and the raster -- test:bord2
        const int rpixel = this->raster_pixel - 24;
        bool border = this->vborder || 
            /* hborder */ (rpixel < (768-512)/2) || (rpixel >= (768 - (768-512)/2));

        index = this->getColorIndex(rpixel, border);
        if (clk == commit_time) {
            this->io.commit(); // regular i/o writes (border index); test: bord2
        }
        if (clk == commit_time_pal) {
            this->io.commit_palette(index); // palette writes; test: bord2
        }
        if (this->visible) {
            const int bmp_x = this->raster_pixel - this->center_offset;
            if (bmp_x >= 0 && bmp_x < this->screen_width) {
                if (this->mode512) {// && !border -- border A/B alternation, see Cherezov page 7
                    bmp[this->bmpofs++] = this->io.PaletteRaw(index & 0x03);
                    bmp[this->bmpofs++] = this->io.PaletteRaw(index & 0x0c);
                } else {
                    uint8_t p = this->io.PaletteRaw(index);
                    bmp[this->bmpofs++] = p;
                    bmp[this->bmpofs++] = p;
                }
            }
        }
        // 22 vsync + 18 border + 256 picture + 16 border = 312 lines
        this->raster_pixel += 2;
        if (this->raster_pixel == 768) {
            this->advanceLine(updateScreen);
        }
        // load scroll register at this precise moment -- test:scrltst2
        if (this->raster_line == 22 + 18 && this->raster_pixel == 150) {
            this->fb_row = this->io.ScrollStart();
        }
        // irq time -- test:bord2, vst (MovR=1d37, MovM=1d36)
        else if (this->raster_line == 0 && this->raster_pixel == 174) {
            this->irq = true;
            this->irq_clk = clk;
        }
    } 

    if (clk == commit_time) {
        this->io.commit(); // regular i/o writes (border index); test: bord2
    }
    if (clk == commit_time_pal) {
        this->io.commit_palette(index); // palette writes; test: bord2
    }

    if (afterbrk) {
        afterbrk -= 2;
    }
    return afterbrk;
}

/* fast_framebuffer variant of fill1: drives the raster counters and the
 * machine-visible side effects (i/o commits, palette commits, scroll
 * load, irq, line advance, brk) exactly like fill1, but paints nothing:
 * render_full_frame() builds the whole frame at the end of the machine
 * frame from the final VRAM and palette state. */
int PixelFiller::fill1_nodraw(int clocks, int commit_time,
        int commit_time_pal, bool updateScreen) {
    int clk;
    int afterbrk = 0;
    int index = 0;
    const int rp0 = this->raster_pixel;

    for (clk = 0; clk < clocks; clk += 2, afterbrk += this->brk ? 2 : 0) {
        if (clk == commit_time) {
            this->io.commit(); // regular i/o writes (border index); test: bord2
        }
        if (clk == commit_time_pal) {
            /* commit_palette(index) is the only consumer of the pixel
             * index; reconstruct it on demand instead of shifting the
             * whole raster out */
            index = this->pixelIndexAt(rp0 + clk - 24);
            this->io.commit_palette(index); // palette writes; test: bord2
        }
        // 22 vsync + 18 border + 256 picture + 16 border = 312 lines
        this->raster_pixel += 2;
        if (this->raster_pixel == 768) {
            this->advanceLine(updateScreen);
        }
        // load scroll register at this precise moment -- test:scrltst2
        if (this->raster_line == 22 + 18 && this->raster_pixel == 150) {
            this->fb_row = this->io.ScrollStart();
        }
        // irq time -- test:bord2, vst (MovR=1d37, MovM=1d36)
        else if (this->raster_line == 0 && this->raster_pixel == 174) {
            this->irq = true;
            this->irq_clk = clk;
        }
    }

    if (clk == commit_time) {
        this->io.commit(); // regular i/o writes (border index); test: bord2
    }
    if (clk == commit_time_pal) {
        /* fill1 uses the index of the last pixel shifted out */
        index = this->pixelIndexAt(rp0 + clk - 2 - 24);
        this->io.commit_palette(index); // palette writes; test: bord2
    }

    if (afterbrk) {
        afterbrk -= 2;
    }
    return afterbrk;
}

/* Pixel index displayed at rpixel, reconstructed from VRAM. fill1 would
 * have shifted it out of the group fetched at (rpixel & 0x0f) == 0. */
int PixelFiller::pixelIndexAt(int rpixel) {
    if (this->vborder ||
            rpixel < (768-512)/2 || rpixel >= (768 - (768-512)/2)) {
        this->fb_column = 0;
        return this->border_index;
    }
    const int col = (rpixel - (768-512)/2) >> 4;
    this->fb_column = col;
    this->fetchPixels();
    this->fb_column = col + 1;
#if USE_BIT_PERMUTE
    /* drop the pixels already displayed within the group */
    this->pixel32_grouped <<= (rpixel & 0x0f) << 1;
#endif
    return this->shiftOutPixels();
}

void PixelFiller::advanceLine(bool updateScreen) {
    this->raster_pixel = 0;
    this->raster_line += 1;
    this->fb_row -= 1;
    if (!this->vborder && this->fb_row < 0) {
        this->fb_row = 0xff;
    }
    // update vertical border only when line changes
    this->vborder = (this->raster_line < 40) || (this->raster_line >= (40 + 256));
    // turn on pixel copying after blanking area
    this->visible = this->visible || 
        (updateScreen && this->raster_line == this->first_visible_line);
    if (this->raster_line == 312) {
        this->raster_line = 0;
        this->visible = false; // blanking starts
        this->brk = true;
    }
}

/* simple fill, no out instructions underway, mode 256 */
int PixelFiller::fill2(int clocks)
{
    uint8_t * const bmp = this->pixels;
    int clk;

    int ofs = this->bmpofs;

    // clocks=16/32/48/64/80/96..

    int rpixel = this->raster_pixel - 24;
    this->raster_pixel += clocks;

    /* Head: pixels before the next fetch boundary ((rpixel & 0x0f) == 0).
     * No fetch can happen here, so just shift. */
    for (clk = 0; clk < clocks && (rpixel & 0x0f) != 0; clk += 2) {
        uint8_t p = this->io.PaletteRaw(this->shiftOutPixels());
        bmp[ofs++] = p; bmp[ofs++] = p;
        rpixel += 2;
    }

    /* Aligned groups: 16 clocks = 8 pixels = exactly one fetch. The
     * fetch test disappears from the inner loop, and clocks is always
     * a multiple of 16 here, so entry and exit alignment match. */
    for (; clk + 16 <= clocks; clk += 16, rpixel += 16) {
        this->fetchPixels();
        ++this->fb_column;

        uint32_t p0 = this->shiftOutPixels();
        uint32_t p1 = this->shiftOutPixels();
        uint32_t p2 = this->shiftOutPixels();
        uint32_t p3 = this->shiftOutPixels();
        uint32_t p4 = this->shiftOutPixels();
        uint32_t p5 = this->shiftOutPixels();
        uint32_t p6 = this->shiftOutPixels();
        uint32_t p7 = this->shiftOutPixels();

#if __ARM_NEON
        uint32x4_t d0,d1,d2,d3;

        p0 = this->io.PaletteRaw(p0);
        p1 = this->io.PaletteRaw(p1);
        d0 = vsetq_lane_u32(p0, d0, 0);
        d0 = vsetq_lane_u32(p0, d0, 1);
        d0 = vsetq_lane_u32(p1, d0, 2);
        d0 = vsetq_lane_u32(p1, d0, 3);

        p2 = this->io.PaletteRaw(p2);
        p3 = this->io.PaletteRaw(p3);
        d1 = vsetq_lane_u32(p2, d1, 0);
        d1 = vsetq_lane_u32(p2, d1, 1);
        d1 = vsetq_lane_u32(p3, d1, 2);
        d1 = vsetq_lane_u32(p3, d1, 3);

        p4 = this->io.PaletteRaw(p4);
        p5 = this->io.PaletteRaw(p5);
        d2 = vsetq_lane_u32(p4, d2, 0);
        d2 = vsetq_lane_u32(p4, d2, 1);
        d2 = vsetq_lane_u32(p5, d2, 2);
        d2 = vsetq_lane_u32(p5, d2, 3);

        p6 = this->io.PaletteRaw(p6);
        p7 = this->io.PaletteRaw(p7);
        d3 = vsetq_lane_u32(p6, d3, 0);
        d3 = vsetq_lane_u32(p6, d3, 1);
        d3 = vsetq_lane_u32(p7, d3, 2);
        d3 = vsetq_lane_u32(p7, d3, 3);


        vst1q_u32(&bmp[ofs], d0); ofs+= 4;
        vst1q_u32(&bmp[ofs], d1); ofs+= 4;
        vst1q_u32(&bmp[ofs], d2); ofs+= 4;
        vst1q_u32(&bmp[ofs], d3); ofs+= 4;
#else
        p0 = this->io.PaletteRaw(p0);
        p1 = this->io.PaletteRaw(p1);
        p2 = this->io.PaletteRaw(p2);
        p3 = this->io.PaletteRaw(p3);
        p4 = this->io.PaletteRaw(p4);
        p5 = this->io.PaletteRaw(p5);
        p6 = this->io.PaletteRaw(p6);
        p7 = this->io.PaletteRaw(p7);

        bmp[ofs++] = p0; bmp[ofs++] = p0;
        bmp[ofs++] = p1; bmp[ofs++] = p1;
        bmp[ofs++] = p2; bmp[ofs++] = p2;
        bmp[ofs++] = p3; bmp[ofs++] = p3;
        bmp[ofs++] = p4; bmp[ofs++] = p4;
        bmp[ofs++] = p5; bmp[ofs++] = p5;
        bmp[ofs++] = p6; bmp[ofs++] = p6;
        bmp[ofs++] = p7; bmp[ofs++] = p7;
#endif

    }

    /* Tail: runs only if clocks is not a multiple of 16 */
    for (; clk < clocks; clk += 2) {
        if ((rpixel & 0x0f) == 0) {
            this->fetchPixels();
            ++this->fb_column;
        }
        uint8_t p = this->io.PaletteRaw(this->shiftOutPixels());
        bmp[ofs++] = p; bmp[ofs++] = p;
        rpixel += 2;
    }

    this->bmpofs = ofs;
    return 0;
}

/* simple fill, no out instructions underway, mode 512 */
int PixelFiller::fill3(int clocks)
{
    uint8_t * const bmp = this->pixels;
    int clk;

    int ofs = this->bmpofs;

    int rpixel = this->raster_pixel - 24;
    this->raster_pixel += clocks;

    int index;

    /* Head: pixels before the next fetch boundary; shift only */
    for (clk = 0; clk < clocks && (rpixel & 0x0f) != 0; clk += 2) {
        index = this->shiftOutPixels();
        bmp[ofs++] = this->io.PaletteRaw(index & 0x03);
        bmp[ofs++] = this->io.PaletteRaw(index & 0x0c);
        rpixel += 2;
    }

    /* Aligned groups: 16 clocks = 8 pixels = exactly one fetch */
    for (; clk + 16 <= clocks; clk += 16, rpixel += 16) {
        this->fetchPixels();
        ++this->fb_column;

        index = this->shiftOutPixels();
        bmp[ofs++] = this->io.PaletteRaw(index & 0x03);
        bmp[ofs++] = this->io.PaletteRaw(index & 0x0c);
        index = this->shiftOutPixels();
        bmp[ofs++] = this->io.PaletteRaw(index & 0x03);
        bmp[ofs++] = this->io.PaletteRaw(index & 0x0c);
        index = this->shiftOutPixels();
        bmp[ofs++] = this->io.PaletteRaw(index & 0x03);
        bmp[ofs++] = this->io.PaletteRaw(index & 0x0c);
        index = this->shiftOutPixels();
        bmp[ofs++] = this->io.PaletteRaw(index & 0x03);
        bmp[ofs++] = this->io.PaletteRaw(index & 0x0c);
        index = this->shiftOutPixels();
        bmp[ofs++] = this->io.PaletteRaw(index & 0x03);
        bmp[ofs++] = this->io.PaletteRaw(index & 0x0c);
        index = this->shiftOutPixels();
        bmp[ofs++] = this->io.PaletteRaw(index & 0x03);
        bmp[ofs++] = this->io.PaletteRaw(index & 0x0c);
        index = this->shiftOutPixels();
        bmp[ofs++] = this->io.PaletteRaw(index & 0x03);
        bmp[ofs++] = this->io.PaletteRaw(index & 0x0c);
        index = this->shiftOutPixels();
        bmp[ofs++] = this->io.PaletteRaw(index & 0x03);
        bmp[ofs++] = this->io.PaletteRaw(index & 0x0c);
    }

    /* Tail: runs only if clocks is not a multiple of 16 */
    for (; clk < clocks; clk += 2) {
        if ((rpixel & 0x0f) == 0) {
            this->fetchPixels();
            ++this->fb_column;
        }
        index = this->shiftOutPixels();
        bmp[ofs++] = this->io.PaletteRaw(index & 0x03);
        bmp[ofs++] = this->io.PaletteRaw(index & 0x0c);
        rpixel += 2;
    }

    this->bmpofs = ofs;
    return 0;
}

int PixelFiller::fill4(int clocks)
{
    uint8_t * const bmp = this->pixels;
    int clk;

    int ofs = this->bmpofs;
    this->raster_pixel += clocks;

    uint8_t p = this->io.PaletteRaw(this->getColorIndex(0, true));
    uint16_t p16 = (uint16_t)p | (uint16_t)((uint16_t)p << 8);
    for (clk = 0; clk < clocks; clk += 16) {
        *(uint16_t*)&bmp[ofs] = p16; ofs += 2;
        *(uint16_t*)&bmp[ofs] = p16; ofs += 2;
        *(uint16_t*)&bmp[ofs] = p16; ofs += 2;
        *(uint16_t*)&bmp[ofs] = p16; ofs += 2;
        *(uint16_t*)&bmp[ofs] = p16; ofs += 2;
        *(uint16_t*)&bmp[ofs] = p16; ofs += 2;
        *(uint16_t*)&bmp[ofs] = p16; ofs += 2;
        *(uint16_t*)&bmp[ofs] = p16; ofs += 2;
    } 

    this->bmpofs = ofs;
    return 0;
}


auto PixelFiller::get_raster_pixel() const -> const int
{
    return raster_pixel;
}
auto PixelFiller::get_raster_line() const -> const int
{
    return raster_line;
}

/* ---- fast_framebuffer: one-shot VRAM -> bmp after the machine frame ----
 *
 * The raster filler paints the bmp as the beam sweeps; these functions
 * instead rebuild the whole bmp from the final VRAM, palette, scroll
 * and mode state. Geometry mirrors fill1:
 *   rpixel = raster_pixel - 24, picture at rpixel [128, 640)
 *   bmp_x  = raster_pixel - center_offset
 *   picture lines 40..295, fb_row = (scroll - (line - 40)) & 0xff
 * center_offset is even (120), so raster-unit pairs sit at even bmp_x.
 */

void PixelFiller::render_full_frame()
{
    if (this->mode512) {
        this->render_full_frame_512();
    } else {
        this->render_full_frame_256();
    }
}

void PixelFiller::render_full_frame_256()
{
    uint8_t * const bmp = this->pixels;
    const int w = this->screen_width;
    const uint8_t * const pal = this->io.palette_raw_data();
    const int scroll = this->io.ScrollStart();
    const int pic_left = 152 - this->center_offset;   /* rpixel 128 */
    const int pic_right = pic_left + 512;             /* rpixel 640 */

    /* every index fills both bytes of a raster unit; the byte LUT maps
     * one byte of pixel32_grouped (two nibble pixels) straight to the
     * packed 32-bit pair, so the picture loop does one lookup per two
     * pixels instead of one per pixel */
    uint16_t pal2[16];
    uint32_t lut[256];
    for (int i = 0; i < 16; ++i) {
        const uint16_t v = pal[i];
        pal2[i] = (uint16_t)(v | (v << 8));
    }
    for (int b = 0; b < 256; ++b) {
        lut[b] = (uint32_t)pal2[b >> 4] | ((uint32_t)pal2[b & 15] << 16);
    }
    const uint8_t border_byte = pal[this->border_index];

    for (int y = 0; y < 312 - this->first_visible_line; ++y) {
        uint8_t * const row = bmp + (size_t)y * w;
        const int line = y + this->first_visible_line;

        if (line < 40 || line >= 40 + 256) {
            /* vertical border row */
            memset(row, border_byte, w);
            continue;
        }

        /* horizontal border strips */
        memset(row, border_byte, pic_left);
        memset(row + pic_right, border_byte, w - pic_right);

        /* picture: 32 columns of 8 pixels, same VRAM addressing and
         * bit permutation as fetchPixels() */
        this->fb_row = (scroll - (line - 40)) & 0xff;
        uint32_t * p = (uint32_t *)(row + pic_left);
        for (int col = 0; col < 32; ++col) {
            this->fb_column = col;
            this->fetchPixels();
            const uint32_t x = this->pixel32_grouped;
            p[0] = lut[(x >> 24) & 0xff];
            p[1] = lut[(x >> 16) & 0xff];
            p[2] = lut[(x >> 8) & 0xff];
            p[3] = lut[x & 0xff];
            p += 4;
        }
    }
}

void PixelFiller::render_full_frame_512()
{
    uint8_t * const bmp = this->pixels;
    const int w = this->screen_width;
    const uint8_t * const pal = this->io.palette_raw_data();
    const int scroll = this->io.ScrollStart();
    const int pic_left = 152 - this->center_offset;
    const int pic_right = pic_left + 512;

    /* every index produces two colors (Cherezov page 7): the low and
     * the high plane pair; the byte LUT packs two adjacent pixels
     * into one 32-bit word */
    uint16_t pair[16];
    uint32_t lut[256];
    for (int i = 0; i < 16; ++i) {
        pair[i] = (uint16_t)(pal[i & 0x03] | (pal[i & 0x0c] << 8));
    }
    for (int b = 0; b < 256; ++b) {
        lut[b] = (uint32_t)pair[b >> 4] | ((uint32_t)pair[b & 15] << 16);
    }
    const uint32_t b4 = (uint32_t)pair[this->border_index] |
        ((uint32_t)pair[this->border_index] << 16);

    for (int y = 0; y < 312 - this->first_visible_line; ++y) {
        uint8_t * const row = bmp + (size_t)y * w;
        const int line = y + this->first_visible_line;

        if (line < 40 || line >= 40 + 256) {
            /* vertical border row: mode512 shows the pair (fill1 /
             * Cherezov page 7). The raster filler only paints the pair
             * where fill1 runs (the line edges); the fill4 middle uses
             * the unmasked color as a shortcut. We render the genuine
             * pair everywhere. */
            uint32_t * d = (uint32_t *)row;
            for (int x = 0; x < w; x += 4) {
                *d++ = b4;
            }
            continue;
        }

        /* horizontal border strips alternate the pair (fill1) */
        uint32_t * d = (uint32_t *)row;
        for (int x = 0; x < pic_left; x += 4) {
            *d++ = b4;
        }
        d = (uint32_t *)(row + pic_right);
        for (int x = pic_right; x < w; x += 4) {
            *d++ = b4;
        }

        /* picture */
        this->fb_row = (scroll - (line - 40)) & 0xff;
        uint32_t * p = (uint32_t *)(row + pic_left);
        for (int col = 0; col < 32; ++col) {
            this->fb_column = col;
            this->fetchPixels();
            const uint32_t x = this->pixel32_grouped;
            p[0] = lut[(x >> 24) & 0xff];
            p[1] = lut[(x >> 16) & 0xff];
            p[2] = lut[(x >> 8) & 0xff];
            p[3] = lut[x & 0xff];
            p += 4;
        }
    }
}
