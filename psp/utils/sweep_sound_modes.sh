#!/bin/bash

# sweep_sound_modes.sh - прогон sound_mode = none/cubic/gaussian/sinc
# в PPSSPP (AUTOSELECT-сборка, 60 с на режим).
#
# Для каждого режима переписывает config.ini в каталоге игры,
# запускает run_60sec_kill и складывает результаты в
# sound_mode_results/<режим>/:
#   perf.log          - FPS/CPU/тайминги по секундам (release AUTOSELECT)
#   debug.log         - snd_cb fill/step/underrun + статистика
#                       (только при RECORD=1, debug-сборка)
#   psp_internal.wav  - звук на выходе генератора (только RECORD=1)
#   psp_callback.wav  - звук, отданный PSP audio (только RECORD=1)
#
# Использование:
#   ./utils/sweep_sound_modes.sh          # perf-прогон (release)
#   RECORD=1 ./utils/sweep_sound_modes.sh # прогон с записью WAV (debug)

set -u

HERE="$(cd "$(dirname "$0")/.." && pwd)"
GD="$HOME/snap/ppsspp-emu/common/.config/ppsspp/PSP/GAME/VECTOR06C"
OUT="$HERE/sound_mode_results"
RECORD="${RECORD:-0}"

mkdir -p "$OUT"

for M in none cubic gaussian sinc; do
    echo "=== sound_mode=$M ==="
    cat > "$GD/config.ini" <<EOF
border = true
fps = true
fast_framebuffer = true
worker_priority = 0x30
main_priority = 0x18
sound_mode = $M
EOF
    if [ "$RECORD" = "1" ]; then
        echo "sound_record = true" >> "$GD/config.ini"
        # старые записи не должны смешиваться с новыми (ТЗ п.12)
        rm -f "$GD/psp_internal.wav" "$GD/psp_callback.wav"
    fi
    rm -f "$GD/debug.log"

    "$HERE/utils/run_60sec_kill" "$GD/ppsspp_$M.log"

    D="$OUT/$M"
    mkdir -p "$D"
    cp -f "$GD/perf.log" "$D/" 2>/dev/null
    cp -f "$GD/debug.log" "$D/" 2>/dev/null
    if [ "$RECORD" = "1" ]; then
        cp -f "$GD/psp_internal.wav" "$D/" 2>/dev/null
        cp -f "$GD/psp_callback.wav" "$D/" 2>/dev/null
    fi
done

echo "results in $OUT"
