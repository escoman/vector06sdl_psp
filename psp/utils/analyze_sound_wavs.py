#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""analyze_sound_wavs.py - сравнение WAV по режимам sound_mode.

Для каждого каталога sound_mode_results/<режим>/ берёт
psp_internal.wav (выход генератора) и psp_callback.wav (выход
audio callback) и считает:
  - DC (постоянная составляющая), RMS, пик;
  - распределение энергии по полосам спектра (FFT):
    низкая 0-2 кГц, средняя 2-8 кГц, ВЧ 8-16 кГц,
    околонайквистовская 16-22.05 кГц. Доля ВЧ - объективная
    метрика "скрипа".

Только стандартная библиотека. Запуск: python3 utils/analyze_sound_wavs.py
"""

import cmath
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(HERE, "..", "sound_mode_results")
MODES = ["none", "cubic", "gaussian", "sinc"]
FFT_N = 65536


def fft(x):
    """Итеративный FFT Кули-Тьюки, длина x - степень двойки."""
    n = len(x)
    # bit reversal
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            x[i], x[j] = x[j], x[i]
    length = 2
    while length <= n:
        ang = -2j * cmath.pi / length
        wlen = cmath.exp(ang)
        half = length // 2
        for start in range(0, n, length):
            w = 1 + 0j
            for k in range(start, start + half):
                u = x[k]
                v = x[k + half] * w
                x[k] = u + v
                x[k + half] = u - v
                w *= wlen
        length *= 2
    return x


def load_mono(path, max_samples):
    with wave.open(path, "rb") as w:
        n = w.getnframes()
        sr = w.getframerate()
        ch = w.getnchannels()
        raw = w.readframes(n)
    samples = struct.unpack("<%dh" % (len(raw) // 2), raw)
    mono = samples[::ch]
    if len(mono) > max_samples:
        # середина записи: без старта (boot) и хвоста
        mid = len(mono) // 2
        mono = mono[mid - max_samples // 2: mid + max_samples // 2]
    return [s / 32768.0 for s in mono], sr, n


def bands(spectrum, sr):
    """Доли энергии по полосам."""
    n = len(spectrum)
    bins = n // 2
    e = [0.0] * 4
    limits = [2000, 8000, 16000, 22050]
    for k in range(1, bins):
        f = k * sr / (2 * bins)
        p = (spectrum[k].real ** 2 + spectrum[k].imag ** 2)
        for b in range(4):
            if f <= limits[b]:
                e[b] += p
                break
    total = sum(e)
    if total <= 0:
        return [0.0] * 4
    return [x / total for x in e]


def analyze(path):
    if not os.path.exists(path):
        return None
    sig, sr, total = load_mono(path, FFT_N * 4)
    n = len(sig)
    dc = sum(sig) / n
    rms = (sum((s - dc) ** 2 for s in sig) / n) ** 0.5
    peak = max(abs(s) for s in sig)
    # спектр с середины, окно Хэнна
    mid = n // 2
    win = sig[mid - FFT_N // 2: mid + FFT_N // 2]
    for i in range(FFT_N):
        w = 0.5 - 0.5 * cmath.cos(2 * cmath.pi * i / (FFT_N - 1)).real
        win[i] *= w
    sp = fft(win)
    b = bands(sp, sr)
    return {
        "frames": total,
        "sec": total / float(sr),
        "dc": dc,
        "rms": rms,
        "peak": peak,
        "b_lo": b[0],
        "b_mid": b[1],
        "b_hf": b[2],
        "b_nyq": b[3],
    }


def main():
    if len(sys.argv) > 1:
        # явные пути: python3 analyze_sound_wavs.py file1.wav ...
        for path in sys.argv[1:]:
            r = analyze(path)
            if r is None:
                print("%-40s missing" % path)
                continue
            print("%-40s %8d %7.1f %+9.5f %8.4f %8.4f   "
                  "%5.1f%% %5.1f%% %5.1f%% %5.1f%%" %
                  (path, r["frames"], r["sec"], r["dc"], r["rms"],
                   r["peak"],
                   100 * r["b_lo"], 100 * r["b_mid"],
                   100 * r["b_hf"], 100 * r["b_nyq"]))
        return 0
    print("%-8s %-9s %8s %7s %9s %8s %8s   "
          "%6s %6s %6s %6s" %
          ("mode", "file", "frames", "sec", "dc", "rms", "peak",
           "0-2k", "2-8k", "8-16k", "16-22k"))
    for m in MODES:
        for name in ("psp_internal.wav", "psp_callback.wav"):
            path = os.path.join(RESULTS, m, name)
            r = analyze(path)
            if r is None:
                print("%-8s %-9s missing" % (m, name))
                continue
            tag = "internal" if "internal" in name else "callback"
            print("%-8s %-9s %8d %7.1f %+9.5f %8.4f %8.4f   "
                  "%5.1f%% %5.1f%% %5.1f%% %5.1f%%" %
                  (m, tag, r["frames"], r["sec"], r["dc"], r["rms"],
                   r["peak"],
                   100 * r["b_lo"], 100 * r["b_mid"],
                   100 * r["b_hf"], 100 * r["b_nyq"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
