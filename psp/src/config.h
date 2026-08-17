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
 *   sound_mode       = none|cubic|gaussian|sinc
 *                                   waveform reconstruction kernel of
 *                                   the audio callback resampler
 *                                   (default none, case-insensitive)
 *   worker_priority  = 0x08..0x77   emulation thread priority (hex)
 *   main_priority    = 0x08..0x77   display thread priority (hex)
 *
 * Missing file: defaults are applied and the file is created.
 * Unknown or malformed entries are ignored.
 */
std::string config_load(const char * argv0);

/* Write one "key = value" back into the existing config file,
 * keeping every comment and all other lines untouched (only the
 * matching key line is replaced; a missing key is appended). The
 * file is never recreated from scratch and key names never change.
 * Returns true when the file was written. */
bool config_set_value(const std::string & path, const std::string & key,
                      const std::string & value);
