#pragma once

#include <string>

/*
 * Flat "key = value" config file (config.ini) lying next to EBOOT.PBP.
 * Lines starting with '#' or ';' are comments.
 *
 * Currently known keys:
 *   border           = true|false   show the Vector-06C screen border
 *   fps              = true|false   show the FPS counter in the top-left corner
 *   fast_framebuffer = true|false   one-shot frame render after the machine
 *                                   frame instead of the raster filler
 *   sound_record     = true|false   diagnostic WAV recording of the sound
 *                                   pipeline (psp_internal.wav /
 *                                   psp_callback.wav)
 *   sound_buffer_ms  = 1..150       target ring fill for the playback
 *                                   controller in ms (default 40)
 *
 * Missing file: defaults are applied and the file is created.
 * Unknown or malformed entries are ignored.
 */
std::string config_load(const char * argv0);
