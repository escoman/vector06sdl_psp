#pragma once

#include <inttypes.h>

/*
 * Minimal TGA (Targa) loader for the ROM Browser preview (Stage 4).
 * Native-simple format: an 18-byte header plus raw or RLE pixels,
 * no external libraries. Preview files live next to the ROM with the
 * same base name ("riseout.rom" -> "riseout.tga").
 *
 * Supported subset (what every mainstream converter emits):
 *   - type 2 (uncompressed) and type 10 (RLE) truecolor;
 *   - 24-bit (BGR) and 32-bit (BGRA) pixels;
 *   - both vertical origins (bottom-up / top-down).
 * Color-mapped and grayscale TGAs are refused: the caller just shows
 * an empty preview area, never an error.
 *
 * The pixels are converted straight into the PSP GE layout (memory
 * bytes A B G R, i.e. uint32 0xAABBGGRR of GU_PSM_8888) and written
 * into a caller-owned power-of-two texture buffer. Images larger
 * than the destination are shrunk with a box filter, aspect ratio
 * always preserved; smaller images are copied 1:1 (the GE does any
 * upscaling when the quad is drawn).
 */

/* Load path into dst (dst_w x dst_h pixels, PSP 8888 layout).
 * Returns true when a preview was decoded: *out_w and *out_h hold the
 * stored image size (<= dst_w/dst_h, aspect preserved). Returns
 * false on any missing/undecodable/oversized file — the dst content
 * is undefined then and the caller must treat it as "no preview". */
bool tga_load(const char * path,
              uint32_t * dst, int dst_w, int dst_h,
              int * out_w, int * out_h);

/* Save pixels (w x h, PSP 8888 layout 0xAABBGGRR) as an uncompressed
 * 32-bit top-down TGA. Used for the save-state screenshots (Stage 5):
 * the same format tga_load reads back for the slot thumbnails.
 * Returns true when the file was written. */
bool tga_save(const char * path, const uint32_t * pixels, int w, int h);
