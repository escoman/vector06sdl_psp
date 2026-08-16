#include "sound_filters.h"

#include <cctype>
#include <cmath>

namespace sound_filters
{

/* --- configuration parsing ----------------------------------------- */

bool parse_mode(const std::string & s, SoundMode fallback, SoundMode & out)
{
    std::string t = s;
    for (size_t i = 0; i < t.size(); ++i) {
        t[i] = (char)tolower((unsigned char)t[i]);
    }
    if (t == "none") {
        out = SoundMode::None;
        return true;
    }
    if (t == "cubic") {
        out = SoundMode::Cubic;
        return true;
    }
    if (t == "gaussian") {
        out = SoundMode::Gaussian;
        return true;
    }
    if (t == "sinc") {
        out = SoundMode::Sinc;
        return true;
    }
    out = fallback;
    return false;
}

const char * mode_name(SoundMode m)
{
    switch (m) {
        case SoundMode::Cubic:    return "cubic";
        case SoundMode::Gaussian: return "gaussian";
        case SoundMode::Sinc:     return "sinc";
        case SoundMode::None:
        default:                  return "none";
    }
}

/* --- coefficient tables --------------------------------------------
 * Generated once by init_tables(); const from the audio thread's
 * point of view. 4*256 + 8*256 = 3072 floats = 12 KiB. */
static float gauss_tab[GAUSS_PHASES][GAUSS_TAPS];
static float sinc_tab[SINC_PHASES][SINC_TAPS];
static bool tables_ready = false;

/* 4-tap Gaussian. The interpolated position sits between p1 and p2 at
 * offset f in [0, 1); the taps are centered around it at relative
 * distances f+1.5, f+0.5, 0.5-f, 1.5-f, so the kernel is symmetric at
 * f = 0.5. sigma trades HF damping against dullness: 0.45 keeps the
 * sound bright while still rounding off the square-wave staircase.
 * The coefficients are normalized per phase, so sum = 1 exactly and a
 * DC input passes unchanged. */
static void build_gauss()
{
    const double sigma = 0.45;
    for (int ph = 0; ph < GAUSS_PHASES; ++ph) {
        const double f = (double)ph / (double)GAUSS_PHASES;
        const double rel[GAUSS_TAPS] = {
            f + 1.5, f + 0.5, 0.5 - f, 1.5 - f };
        double sum = 0.0;
        double c[GAUSS_TAPS];
        for (int k = 0; k < GAUSS_TAPS; ++k) {
            const double d = rel[k] / sigma;
            c[k] = exp(-0.5 * d * d);
            sum += c[k];
        }
        for (int k = 0; k < GAUSS_TAPS; ++k) {
            gauss_tab[ph][k] = (float)(c[k] / sum);
        }
    }
}

/* 8-tap windowed sinc (Hann window). The interpolated position lies
 * between taps 3 and 4 at offset f in [0, 1), so the distance from
 * the position to tap k is x = (k - 3) - f; phase 0 puts x = 0
 * exactly on tap 3 (near-identity kernel). The Hann window is
 * centered on the interpolated position itself (half-width 4 taps),
 * so the central taps never land in the window trough. cutoff
 * 0.95*Fnyq slightly rounds the sharpest edges (less "скрип") without
 * dulling the square-wave body; the exact per-phase sum normalization
 * keeps the DC gain at 1.0. */
static void build_sinc()
{
    const double pi = 3.14159265358979323846;
    const double fc = 0.475;            /* cutoff in cycles/sample,
                                         * 0.95 of Nyquist (0.5) */
    for (int ph = 0; ph < SINC_PHASES; ++ph) {
        const double f = (double)ph / (double)SINC_PHASES;
        double sum = 0.0;
        double c[SINC_TAPS];
        for (int k = 0; k < SINC_TAPS; ++k) {
            const double x = (double)(k - 3) - f;
            /* Hann window centered at the interpolated position;
             * x stays within [-4, 4], window reaches 0 at +-4 */
            const double w = 0.5 + 0.5 * cos(pi * x * 0.25);
            double h;
            if (x == 0.0) {
                h = 2.0 * fc;
            } else {
                h = sin(2.0 * pi * fc * x) / (pi * x);
            }
            c[k] = w * h;
            sum += c[k];
        }
        for (int k = 0; k < SINC_TAPS; ++k) {
            sinc_tab[ph][k] = (float)(c[k] / sum);
        }
    }
}

void init_tables()
{
    if (tables_ready) {
        return;
    }
    build_gauss();
    build_sinc();
    tables_ready = true;
}

/* --- hot-path kernels ----------------------------------------------
 * frac in [0, 1); the phase index is taken straight from the 16-bit
 * fractional phase of the resampler (rd_frac >> 8 -> 256 phases). */

float gaussian(float p0, float p1, float p2, float p3, float frac)
{
    const int ph = (int)(frac * (float)GAUSS_PHASES) & (GAUSS_PHASES - 1);
    const float * c = gauss_tab[ph];
    return p0 * c[0] + p1 * c[1] + p2 * c[2] + p3 * c[3];
}

float sinc8(float s0, float s1, float s2, float s3,
            float s4, float s5, float s6, float s7, float frac)
{
    const int ph = (int)(frac * (float)SINC_PHASES) & (SINC_PHASES - 1);
    const float * c = sinc_tab[ph];
    return s0 * c[0] + s1 * c[1] + s2 * c[2] + s3 * c[3]
         + s4 * c[4] + s5 * c[5] + s6 * c[6] + s7 * c[7];
}

} /* namespace sound_filters */
