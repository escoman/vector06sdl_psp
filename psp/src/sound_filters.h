#pragma once

#include <cstdint>
#include <string>

/* Sound reconstruction filters (config.ini: sound_mode).
 *
 * The audio callback replays the ring buffer with an adaptive
 * fractional step around 1.0 (PI controller on the fill level). At
 * every output frame the waveform has to be reconstructed at a
 * fractional phase t = rd_frac / 65536 between two consecutive ring
 * frames. This module provides the reconstruction kernels for that
 * step, selected once at startup:
 *
 *   none     reference mode: the linear two-point reconstruction the
 *            pipeline has always used (kept verbatim for A/B testing)
 *   cubic    Catmull-Rom spline over 4 points
 *   gaussian table-driven 4-tap Gaussian kernel (symmetric, DC-preserving)
 *   sinc     table-driven 8-tap Hann-windowed sinc
 *
 * The modes are mutually exclusive algorithms (no chaining), they do
 * not change the number of generated or consumed samples, and the
 * gaussian/sinc tables are generated once - the hot loop never calls
 * sin()/cos()/exp() or allocates. Extensible: a new kernel is one
 * enum value, one table and one case in Soundnik::callback(). */
enum class SoundMode
{
    None,
    Cubic,
    Gaussian,
    Sinc
};

namespace sound_filters
{
    /* config.ini string -> enum. Case-insensitive; an unknown value
     * falls back to `fallback` (and the caller logs a diagnostic). */
    bool parse_mode(const std::string & s, SoundMode fallback,
                    SoundMode & out);
    const char * mode_name(SoundMode m);

    /* --- kernel parameters ---------------------------------------- */

    static const int GAUSS_PHASES = 256;
    static const int GAUSS_TAPS = 4;      /* p0..p3, interpolated between p1..p2 */
    static const int SINC_PHASES = 256;
    static const int SINC_TAPS = 8;       /* windowed sinc, Hann window */

    /* Build the coefficient tables once at sound init. Idempotent.
     * All values are generated with double precision math on the host
     * side of startup, then rounded to float. */
    void init_tables();

    /* --- reconstruction kernels ------------------------------------
     * frac is the fractional phase in [0, 1). The sample arguments
     * follow the Catmull-Rom convention: the interpolated position
     * lies between p1 and p2. The caller clamps them at the buffer
     * edges (repeat of the boundary sample), so no kernel ever reads
     * outside the ring. */

    /* Catmull-Rom cubic over p0..p3 */
    inline float cubic(float p0, float p1, float p2, float p3, float t)
    {
        const float t2 = t * t;
        const float t3 = t2 * t;
        return 0.5f * (2.0f * p1
            + (-p0 + p2) * t
            + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
            + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
    }

    /* 4-tap Gaussian, coefficients from gauss_tab[phase][tap].
     * The kernel is symmetric and normalized to sum = 1 per phase,
     * so a constant input reproduces itself (no DC shift, no gain). */
    float gaussian(float p0, float p1, float p2, float p3, float frac);

    /* 8-tap Hann-windowed sinc, coefficients from sinc_tab[phase][tap].
     * s0 is the sample 3 frames before the interpolated position;
     * the taps run s0..s7 and the position lies between s3 and s4.
     * Normalized to sum = 1 per phase (DC-preserving). */
    float sinc8(float s0, float s1, float s2, float s3,
                float s4, float s5, float s6, float s7, float frac);
}
