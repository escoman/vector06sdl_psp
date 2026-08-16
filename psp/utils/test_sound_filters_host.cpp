/* Host-side verification of the PSP sound_filters kernels (ТЗ stage 2,
 * section 14): DC preservation, gain, pitch, overshoot and ringing on
 * synthetic signals. Build from psp/: g++ -O2 -Isrc -o /tmp/sftest
 * utils/test_sound_filters_host.cpp src/sound_filters.cpp -lm
 */
#include <cstdio>
#include <cmath>
#include <vector>
#include "sound_filters.h"

using namespace sound_filters;

static const int NSIG = 44100;        /* 1 s of input, 44100 Hz */
static const double PI = 3.14159265358979323846;

typedef float (*Kernel4)(float, float, float, float, float);
typedef float (*Kernel8)(float, float, float, float,
                         float, float, float, float, float);

/* Generic runner kernel: 8 taps, interpolated position between s[3]
 * and s[4], fractional phase t. */
typedef float (*K8)(float, float, float, float,
                    float, float, float, float, float);

/* Resample sig at fractional step ~1.0 with a given constant phase
 * drift, exactly like the callback does (phase sweeps 0..1). */
template <typename F>
static std::vector<float> run(const std::vector<float> & sig, double step,
                              F kern, int taps_left, int taps_right)
{
    std::vector<float> out;
    out.reserve(NSIG + 64);
    double pos = 0.0;
    const int n = (int)sig.size();
    while (pos < n - taps_right - 1) {
        int i0 = (int)pos;
        float frac = (float)(pos - i0);
        /* clamp at the edges like the callback does */
        auto at = [&](int k) -> float {
            if (k < 0) k = 0;
            if (k >= n) k = n - 1;
            return sig[k];
        };
        float s[8];
        for (int k = 0; k < 8; ++k) {
            s[k] = at(i0 - taps_left + k);
        }
        out.push_back(kern(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
                           frac));
        pos += step;
    }
    return out;
}

static void stats(const char * name, const std::vector<float> & v,
                  double expect_dc)
{
    double mn = 1e9, mx = -1e9, sum = 0.0;
    for (float x : v) {
        if (x < mn) mn = x;
        if (x > mx) mx = x;
        sum += x;
    }
    double dc = sum / v.size();
    /* RMS AC energy */
    double ac = 0.0;
    for (float x : v) ac += (x - dc) * (x - dc);
    ac = sqrt(ac / v.size());
    printf("%-8s n=%6zu min=%+.4f max=%+.4f dc=%+.6f (want %+.4f) "
           "ac_rms=%.5f\n",
           name, v.size(), mn, mx, dc, expect_dc, ac);
}

/* Dominant frequency via zero crossings (after DC removal) */
static double zc_freq(const std::vector<float> & v, double sr)
{
    double dc = 0;
    for (float x : v) dc += x;
    dc /= v.size();
    int zc = 0;
    for (size_t i = 1; i < v.size(); ++i) {
        if ((v[i] - dc >= 0) != (v[i - 1] - dc >= 0)) ++zc;
    }
    return zc * 0.5 * sr / v.size();
}

static float wrap4_cubic(float, float, float, float,
                         float, float, float, float, float)
{
    return 0.0f; /* unused */
}

int main()
{
    init_tables();

    /* --- coefficient sanity: DC passes every kernel unchanged ----
     * (equivalent to checking sum(coefficients) = 1 per phase) */
    {
        std::vector<float> ones(64, 1.0f);
        for (int ph = 0; ph < 64; ++ph) {
            float t = (float)ph / 64.0f;
            float g = gaussian(1, 1, 1, 1, t);
            float s = sinc8(1, 1, 1, 1, 1, 1, 1, 1, t);
            float c = cubic(1, 1, 1, 1, t);
            if (fabs(g - 1.0f) > 1e-5f || fabs(s - 1.0f) > 1e-5f
                    || fabs(c - 1.0f) > 1e-5f) {
                printf("FAIL DC at phase %d: g=%f s=%f c=%f\n",
                       ph, g, s, c);
                return 1;
            }
        }
        printf("coefficient sums: ok (DC gain 1.0 at all phases)\n");
    }

    const double step = 1.0 + 0.003;   /* typical PI-controller offset */

    /* wrappers: the generic runner feeds 8 taps with the interpolated
     * position between s[3] and s[4]; cubic/gaussian use the middle 4 */
    K8 k_cubic = [](float, float, float c, float d,
                    float e, float f, float, float, float t) {
        return cubic(c, d, e, f, t);
    };
    K8 k_gauss = [](float, float, float c, float d,
                    float e, float f, float, float, float t) {
        return gaussian(c, d, e, f, t);
    };
    K8 k_sinc = [](float a, float b, float c, float d,
                   float e, float f, float g, float h, float t) {
        return sinc8(a, b, c, d, e, f, g, h, t);
    };
    K8 k_linear = [](float, float, float d, float,
                     float e, float, float, float, float t) {
        return d + (e - d) * t;
    };

    struct Mode { const char * name; K8 fn; } modes[] = {
        { "linear", k_linear },
        { "cubic",  k_cubic },
        { "gauss",  k_gauss },
        { "sinc",   k_sinc },
    };

    /* --- 1. constant level (DC) ---------------------------------- */
    printf("\n== DC 0.5 ==\n");
    std::vector<float> dc(NSIG, 0.5f);
    for (auto & m : modes) {
        auto out = run(dc, step, m.fn, 3, 4);
        stats(m.name, out, 0.5);
    }

    /* --- 2. sine 1 kHz -------------------------------------------- */
    printf("\n== sine 1 kHz ==\n");
    std::vector<float> sine(NSIG);
    for (int i = 0; i < NSIG; ++i)
        sine[i] = (float)sin(2 * PI * 1000.0 * i / 44100.0);
    for (auto & m : modes) {
        auto out = run(sine, step, m.fn, 3, 4);
        stats(m.name, out, 0.0);
        printf("         freq=%.1f Hz (want 1000), gain vs input: %.4f\n",
               zc_freq(out, 44100.0 * step),
               [&] {
                   double a = 0;
                   for (size_t i = 4410; i < out.size() - 4410; ++i)
                       if (fabs(out[i]) > a) a = fabs(out[i]);
                   return a;   /* input amplitude is 1.0 */
               }());
    }

    /* --- 3. square 2 kHz ------------------------------------------ */
    printf("\n== square 2 kHz ==\n");
    std::vector<float> sq(NSIG);
    for (int i = 0; i < NSIG; ++i)
        sq[i] = ((i / 11) % 2) ? 0.5f : -0.5f;   /* ~2004 Hz */
    for (auto & m : modes) {
        auto out = run(sq, step, m.fn, 3, 4);
        double overshoot = 0;
        for (float x : out) {
            double d = fabs(x) - 0.5;
            if (d > overshoot) overshoot = d;
        }
        printf("%-8s overshoot=%+.4f  ", m.name, overshoot);
        /* HF noise energy: difference from an ideal moving average */
        double hf = 0;
        for (size_t i = 1; i < out.size() - 1; ++i) {
            float d = out[i] - 0.5f * (out[i - 1] + out[i + 1]);
            hf += d * d;
        }
        printf("hf_roughness=%.6f\n", sqrt(hf / out.size()));
    }

    /* --- 4. impulse ----------------------------------------------- */
    printf("\n== impulse (ringing) ==\n");
    std::vector<float> imp(NSIG, 0.0f);
    imp[NSIG / 2] = 1.0f;
    for (auto & m : modes) {
        auto out = run(imp, 1.0, m.fn, 3, 4);
        /* impulse response: look around the center */
        size_t c = out.size() / 2;
        printf("%-8s response: ", m.name);
        for (int k = -6; k <= 6; ++k) {
            printf("%+.4f ", out[c + k]);
        }
        double peak = 0;
        for (size_t i = c + 2; i < c + 40 && i < out.size(); ++i)
            if (fabs(out[i]) > peak) peak = fabs(out[i]);
        for (size_t i = c > 40 ? c - 40 : 0; i < c - 1; ++i)
            if (fabs(out[i]) > peak) peak = fabs(out[i]);
        printf(" ringing_peak=%.4f\n", peak);
    }

    /* --- phase continuity: output count --------------------------- */
    printf("\n== output counts (must match across modes) ==\n");
    for (auto & m : modes) {
        auto out = run(sine, step, m.fn, 3, 4);
        printf("%-8s %zu\n", m.name, out.size());
    }

    return 0;
}
