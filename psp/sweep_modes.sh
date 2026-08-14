#!/bin/bash
# Test helper: sweep one ROM (autoselect index) in raster and fast
# framebuffer modes under PPSSPP, archive perf.log/frame/dump, and
# compare machine state (dump) and pixels (frame) between the modes.
#
# The AUTOSELECT build stops the ROM after 60 s, but PPSSPP itself only
# returns to its menu then; this script kills PPSSPP once the run is
# over (>=55 PERF lines in perf.log, ~1 line per run second).
#
# usage: ./sweep_modes.sh <autoselect-index> <name>
set -e
IDX=$1
NAME=$2
GAMEDIR=~/snap/ppsspp-emu/common/.config/ppsspp/PSP/GAME/VECTOR06C
EBOOT=$(dirname "$0")/boot/EBOOT.PBP
OUT=/tmp/vec_base
mkdir -p $OUT

echo "$IDX" > $GAMEDIR/autoselect.txt

run_ppsspp() {
    rm -f $GAMEDIR/perf.log $GAMEDIR/frame_t600.bmp $GAMEDIR/dump_t600.bin
    # start PPSSPP in its own process group so it can be killed
    # together with all its children (snap wrapper, bwrap, ppsspp)
    setsid snap run ppsspp-emu.ppsspp-sdl $GAMEDIR/EBOOT.PBP \
        > /tmp/sweep_$NAME.log 2>&1 &
    local pid=$!
    # the ROM runs 60 s (~60 PERF lines); wait for it to finish
    local n=0
    for i in $(seq 1 150); do
        sleep 1
        n=$(grep -c '^PERF' $GAMEDIR/perf.log 2>/dev/null || true)
        [ -n "$n" ] && [ "$n" -ge 55 ] && break
    done
    sleep 3        # let the emulator flush its last files
    kill -- -$pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
}

for mode in raster fast; do
    if [ $mode = raster ]; then
        grep -v fast_framebuffer $GAMEDIR/config.ini > $GAMEDIR/config.ini.tmp
        mv $GAMEDIR/config.ini.tmp $GAMEDIR/config.ini
    else
        grep -q fast_framebuffer $GAMEDIR/config.ini || \
            echo "fast_framebuffer = true" >> $GAMEDIR/config.ini
    fi
    cp $EBOOT $GAMEDIR/EBOOT.PBP
    run_ppsspp
    cp $GAMEDIR/perf.log        $OUT/perf_${mode}_${NAME}.log   2>/dev/null || echo "WARN: no perf.log $mode $NAME"
    cp $GAMEDIR/frame_t600.bmp  $OUT/frame_${mode}_${NAME}.bmp  2>/dev/null || echo "WARN: no frame $mode $NAME"
    cp $GAMEDIR/dump_t600.bin   $OUT/dump_${mode}_${NAME}.bin   2>/dev/null || echo "WARN: no dump $mode $NAME"
done

if cmp -s $OUT/dump_raster_${NAME}.bin $OUT/dump_fast_${NAME}.bin; then
    echo "RESULT $NAME: dump IDENTICAL"
else
    echo "RESULT $NAME: dump DIFFERS"
fi
if cmp -s $OUT/frame_raster_${NAME}.bmp $OUT/frame_fast_${NAME}.bmp; then
    echo "RESULT $NAME: frame IDENTICAL"
else
    echo "RESULT $NAME: frame DIFFERS"
fi
