/* resample_wav.cpp - хост-инструмент сравнения ядер sound_mode.
 *
 * Прогоняет один и тот же входной WAV (запись psp_internal.wav из
 * режима none = точный выход генератора) через тот же дробный
 * ресемплер, что и audio callback PSP, с каждым из ядер
 * none/cubic/gaussian/sinc. Выходы сравниваются спектром при
 * идентичном входном потоке (ТЗ п.5, п.15).
 *
 * Использование:
 *   resample_wav <in.wav> <out.wav> <none|cubic|gaussian|sinc> [step%]
 *   step% - шаг воспроизведения в процентах (по умолчанию 95.9,
 *   как в среднем у ПИ-регулятора при debug-прогоне).
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include "sound_filters.h"

static bool read_wav_mono(const char * path, std::vector<float> & out,
                          uint32_t & sr)
{
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) != 44) { fclose(f); return false; }
    uint16_t ch = (uint16_t)(hdr[22] | (hdr[23] << 8));
    sr = (uint32_t)(hdr[24] | (hdr[25] << 8) | (hdr[26] << 16)
                    | (hdr[27] << 24));
    /* поиск чанка data */
    fseek(f, 12, SEEK_SET);
    uint32_t datasize = 0;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) { fclose(f); return false; }
        uint32_t sz = (uint32_t)(ck[4] | (ck[5] << 8) | (ck[6] << 16)
                                 | (ck[7] << 24));
        if (memcmp(ck, "data", 4) == 0) { datasize = sz; break; }
        fseek(f, sz, SEEK_CUR);
    }
    std::vector<int16_t> raw(datasize / 2);
    if (fread(raw.data(), 2, raw.size(), f) != raw.size()) {
        fclose(f); return false;
    }
    fclose(f);
    for (size_t i = 0; i < raw.size(); i += ch) {
        out.push_back(raw[i] / 32768.0f);
    }
    return true;
}

static bool write_wav(const char * path, const std::vector<float> & in,
                      uint32_t sr)
{
    FILE * f = fopen(path, "wb");
    if (!f) return false;
    uint32_t datasize = (uint32_t)(in.size() * 4);
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    uint32_t riff = datasize + 36;
    memcpy(hdr + 4, &riff, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t fmtsz = 16; memcpy(hdr + 16, &fmtsz, 4);
    uint16_t fmt = 1, ch = 2, bits = 16, align = 4;
    memcpy(hdr + 20, &fmt, 2); memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &sr, 4);
    uint32_t brate = sr * 4; memcpy(hdr + 28, &brate, 4);
    memcpy(hdr + 32, &align, 2); memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &datasize, 4);
    fwrite(hdr, 1, 44, f);
    for (float v : in) {
        int x = (int)(v * 32767.0f);
        if (x > 32767) x = 32767;
        if (x < -32768) x = -32768;
        int16_t s = (int16_t)x;
        fwrite(&s, 2, 1, f);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    return true;
}

int main(int argc, char ** argv)
{
    if (argc < 5) {
        printf("usage: resample_wav in.wav out.wav "
               "none|cubic|gaussian|sinc [step%%]\n");
        return 1;
    }
    SoundMode mode;
    if (!sound_filters::parse_mode(argv[3], SoundMode::None, mode)) {
        printf("unknown mode %s\n", argv[3]);
        return 1;
    }
    double step_pct = (argc > 5) ? atof(argv[5]) : 95.9;
    const uint32_t step_frac = (uint32_t)(step_pct * 655.36);

    sound_filters::init_tables();

    std::vector<float> in;
    uint32_t sr;
    if (!read_wav_mono(argv[1], in, sr)) {
        printf("cannot read %s\n", argv[1]);
        return 1;
    }

    /* Тот же алгоритм, что Soundnik::callback(): позиция идёт по
     * входу с дробным шагом, ядро реконструирует значение в
     * дробной фазе; края повторяют граничный сэмпл. */
    const int n = (int)in.size();
    std::vector<float> out;
    out.reserve(n + 64);
    uint64_t pos_int = 0;
    uint32_t frac = 0;
    auto at = [&](int k) -> float {
        if (k < 0) return in[0];
        if (k >= n) return in[n - 1];
        return in[k];
    };
    while ((int)pos_int < n) {
        const int i0 = (int)pos_int;
        const float t = (float)frac * (1.0f / 65536.0f);
        float samp;
        if (mode == SoundMode::None) {
            samp = at(i0) + (at(i0 + 1) - at(i0)) * t;
        } else if (mode == SoundMode::Cubic) {
            samp = sound_filters::cubic(at(i0 - 1), at(i0),
                                        at(i0 + 1), at(i0 + 2), t);
        } else if (mode == SoundMode::Gaussian) {
            samp = sound_filters::gaussian(at(i0 - 1), at(i0),
                                           at(i0 + 1), at(i0 + 2), t);
        } else {
            samp = sound_filters::sinc8(at(i0 - 3), at(i0 - 2), at(i0 - 1),
                                        at(i0), at(i0 + 1), at(i0 + 2),
                                        at(i0 + 3), at(i0 + 4), t);
        }
        out.push_back(samp);
        frac += step_frac;
        pos_int += frac >> 16;
        frac &= 0xffffu;
    }

    if (!write_wav(argv[2], out, sr)) {
        printf("cannot write %s\n", argv[2]);
        return 1;
    }
    printf("%s: %d -> %d frames (step %.2f%%)\n",
           argv[3], n, (int)out.size(), step_pct);
    return 0;
}
