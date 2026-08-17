#include "tgaload.h"

#include <cstdio>
#include <cstring>
#include <vector>

/*
 * Minimal TGA loader, see tgaload.h. Runs in the worker thread while
 * the machine is paused (ROM Browser open), so a blocking read and a
 * few milliseconds of decoding are fine.
 *
 * Pipeline: whole file into memory -> decode the pixels into a
 * scratch buffer in top-down 0xAABBGGRR order -> copy or box-shrink
 * into the caller's GE texture buffer.
 */

/* Decoding cap: the scratch buffer is w*h uint32s; a preview bigger
 * than this is refused (empty area) instead of eating heap. */
static const int MAX_SRC_DIM = 1024;

struct TgaHeader
{
    uint8_t  id_len;
    uint8_t  cmap_type;
    uint8_t  img_type;
    uint16_t cmap_first;
    uint16_t cmap_len;
    uint8_t  cmap_bpp;
    uint16_t x_org;
    uint16_t y_org;
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint8_t  descriptor;
};

static uint16_t rd16(const uint8_t * p)
{
    return (uint16_t)(p[0] | (p[1] << 8));   /* TGA is little-endian */
}

/* One source pixel (BGR[A] byte order in the file) into the PSP GE
 * layout: memory bytes A B G R = 0xAABBGGRR. 24-bit carries no alpha
 * and becomes fully opaque. */
static uint32_t tga_pixel(const uint8_t * p, int bytes_pp)
{
    const uint8_t b = p[0];
    const uint8_t g = p[1];
    const uint8_t r = p[2];
    const uint8_t a = (bytes_pp == 4) ? p[3] : 0xff;
    return ((uint32_t)a << 24) | ((uint32_t)b << 16)
         | ((uint32_t)g << 8)  |  (uint32_t)r;
}

/* Decode type 2 (raw) / type 10 (RLE) truecolor into scratch,
 * top-down row order regardless of the file's origin bit. */
static bool tga_decode(const uint8_t * data, size_t size, size_t off,
                       const TgaHeader & h, std::vector<uint32_t> & out)
{
    const int w = h.width;
    const int hh = h.height;
    const int bytes_pp = h.bpp / 8;
    const bool top_down = (h.descriptor & 0x20) != 0;
    const size_t total = (size_t)w * (size_t)hh;

    out.resize(total);

    if (h.img_type == 2) {
        /* Uncompressed: exactly w*h pixels after the header. */
        if (size - off < total * (size_t)bytes_pp)
            return false;
        for (size_t i = 0; i < total; ++i) {
            /* Map the linear file order onto top-down rows. */
            const size_t row = i / (size_t)w;
            const size_t col = i % (size_t)w;
            const size_t dst_i = top_down
                ? i
                : (size_t)(hh - 1 - row) * (size_t)w + col;
            out[dst_i] = tga_pixel(data + off + i * (size_t)bytes_pp,
                                   bytes_pp);
        }
        return true;
    }

    /* Type 10, RLE: packets may cross row boundaries, so decode the
     * stream linearly first, then un-flip. */
    size_t src = off;
    size_t done = 0;
    std::vector<uint32_t> linear;
    linear.reserve(total);

    while (done < total) {
        if (src >= size)
            return false;
        const uint8_t hdr = data[src++];
        const size_t run = (size_t)(hdr & 0x7f) + 1;
        if (done + run > total)
            return false;                 /* malformed packet */

        if (hdr & 0x80) {
            /* RLE: one pixel repeated run times. */
            if (size - src < (size_t)bytes_pp)
                return false;
            const uint32_t px = tga_pixel(data + src, bytes_pp);
            src += (size_t)bytes_pp;
            for (size_t i = 0; i < run; ++i)
                linear.push_back(px);
        } else {
            /* Raw: run literal pixels. */
            if (size - src < run * (size_t)bytes_pp)
                return false;
            for (size_t i = 0; i < run; ++i) {
                linear.push_back(tga_pixel(data + src, bytes_pp));
                src += (size_t)bytes_pp;
            }
        }
        done += run;
    }

    for (int row = 0; row < hh; ++row) {
        const int src_row = top_down ? row : (hh - 1 - row);
        memcpy(out.data() + (size_t)row * (size_t)w,
               linear.data() + (size_t)src_row * (size_t)w,
               (size_t)w * sizeof(uint32_t));
    }
    return true;
}

/* Aspect-preserving box-filter shrink of src (sw x sh) into dst
 * (tw x th); the source rows are contiguous. Integer per-channel
 * sums, no float. */
static void tga_box_shrink(const std::vector<uint32_t> & src, int sw, int sh,
                           uint32_t * dst, int tw, int th)
{
    for (int dy = 0; dy < th; ++dy) {
        const int sy0 = (int)((size_t)dy * sh / th);
        const int sy1 = (int)((size_t)(dy + 1) * sh / th);
        for (int dx = 0; dx < tw; ++dx) {
            const int sx0 = (int)((size_t)dx * sw / tw);
            const int sx1 = (int)((size_t)(dx + 1) * sw / tw);

            unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint32_t * row =
                    src.data() + (size_t)sy * (size_t)sw;
                for (int sx = sx0; sx < sx1; ++sx) {
                    const uint32_t p = row[sx];
                    r += p & 0xff;
                    g += (p >> 8) & 0xff;
                    b += (p >> 16) & 0xff;
                    a += (p >> 24) & 0xff;
                    ++n;
                }
            }
            if (n == 0)
                n = 1;
            dst[dy * tw + dx] =
                ((a / n) << 24) | ((b / n) << 16)
              | ((g / n) << 8)  |  (r / n);
        }
    }
}

bool tga_load(const char * path,
              uint32_t * dst, int dst_w, int dst_h,
              int * out_w, int * out_h)
{
    /* Whole file into memory; previews are small screenshots. */
    std::vector<uint8_t> data;
    {
        FILE * f = std::fopen(path, "rb");
        if (f == nullptr)
            return false;
        if (std::fseek(f, 0, SEEK_END) != 0) {
            std::fclose(f);
            return false;
        }
        const long fsz = std::ftell(f);
        if (fsz < 18) {                   /* smaller than the header */
            std::fclose(f);
            return false;
        }
        std::fseek(f, 0, SEEK_SET);
        data.resize((size_t)fsz);
        const size_t got = std::fread(data.data(), 1, data.size(), f);
        std::fclose(f);
        if (got != data.size())
            return false;
    }

    TgaHeader h;
    h.id_len     = data[0];
    h.cmap_type  = data[1];
    h.img_type   = data[2];
    h.cmap_first = rd16(&data[3]);
    h.cmap_len   = rd16(&data[5]);
    h.cmap_bpp   = data[7];
    h.x_org      = rd16(&data[8]);
    h.y_org      = rd16(&data[10]);
    h.width      = rd16(&data[12]);
    h.height     = rd16(&data[14]);
    h.bpp        = data[16];
    h.descriptor = data[17];

    /* Only the plain truecolor subset; color-mapped / grayscale stay
     * "no preview" rather than an error. */
    if ((h.img_type != 2 && h.img_type != 10)
            || (h.bpp != 24 && h.bpp != 32)
            || h.width == 0 || h.height == 0
            || h.width > MAX_SRC_DIM || h.height > MAX_SRC_DIM) {
        return false;
    }

    std::vector<uint32_t> pixels;
    const size_t off = 18 + h.id_len
        + (size_t)h.cmap_len * ((h.cmap_bpp + 7) / 8);
    if (!tga_decode(data.data(), data.size(), off, h, pixels))
        return false;

    const int sw = h.width;
    const int sh = h.height;

    if (sw <= dst_w && sh <= dst_h) {
        /* Fits as is: straight copy, the GE upscales the quad. */
        memcpy(dst, pixels.data(),
               (size_t)sw * (size_t)sh * sizeof(uint32_t));
        *out_w = sw;
        *out_h = sh;
        return true;
    }

    /* Too big for the texture: shrink preserving the aspect ratio
     * (scale = min(dst_w/sw, dst_h/sh)). */
    int tw, th;
    if ((size_t)dst_w * sh <= (size_t)dst_h * sw) {
        tw = dst_w;
        th = (int)((size_t)sh * dst_w / sw);
    } else {
        th = dst_h;
        tw = (int)((size_t)sw * dst_h / sh);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;

    tga_box_shrink(pixels, sw, sh, dst, tw, th);
    *out_w = tw;
    *out_h = th;
    return true;
}

/*
 * Save side of the format (Stage 5 state screenshots): one 18-byte
 * header, then raw 32-bit BGRA rows, top-down (descriptor bit 5).
 * tga_load reads exactly this subset back for the slot thumbnails.
 */
bool tga_save(const char * path, const uint32_t * pixels, int w, int h)
{
    if (pixels == nullptr || w <= 0 || h <= 0 || w > 0x7fff || h > 0x7fff)
        return false;

    FILE * f = std::fopen(path, "wb");
    if (f == nullptr)
        return false;

    uint8_t hdr[18];
    memset(hdr, 0, sizeof(hdr));
    hdr[2]  = 2;                        /* uncompressed truecolor */
    hdr[12] = (uint8_t)(w & 0xff);
    hdr[13] = (uint8_t)((w >> 8) & 0xff);
    hdr[14] = (uint8_t)(h & 0xff);
    hdr[15] = (uint8_t)((h >> 8) & 0xff);
    hdr[16] = 32;                       /* bits per pixel */
    hdr[17] = 0x20 | 0x08;              /* top-down, 8 alpha bits */

    if (std::fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        std::fclose(f);
        return false;
    }

    /* File order is B G R A per pixel; the source is the PSP GE
     * layout 0xAABBGGRR. */
    bool ok = true;
    for (int y = 0; y < h && ok; ++y) {
        const uint32_t * row = pixels + (size_t)y * (size_t)w;
        for (int x = 0; x < w; ++x) {
            const uint32_t p = row[x];
            const uint8_t px[4] = {
                (uint8_t)((p >> 16) & 0xff),    /* B */
                (uint8_t)((p >> 8)  & 0xff),    /* G */
                (uint8_t)( p        & 0xff),    /* R */
                (uint8_t)((p >> 24) & 0xff),    /* A */
            };
            if (std::fwrite(px, 1, 4, f) != 4) {
                ok = false;
                break;
            }
        }
    }

    if (std::fclose(f) != 0)
        ok = false;
    if (!ok)
        std::remove(path);      /* never leave a truncated screenshot */
    return ok;
}
