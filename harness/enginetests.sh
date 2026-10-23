#!/bin/bash
# ----------------------------------------------------------------------- //
#
# enginetests.sh -- unit tests for engine subsystems, against the real data.
#
# HOW THIS DIFFERS FROM THE OTHER TWO SCRIPTS
#
#   selftest.sh       tests the HARNESS -- is the instrument sound?
#   checkcontract.sh  tests the RUN     -- did behaviour drift?
#   enginetests.sh    tests a SUBSYSTEM -- is this module right?
#
#   The contract catches drift but cannot say what "correct" is; it would hold
#   just as firmly around a string table that returned the wrong string every
#   time, as long as it did so consistently. These tests assert correctness
#   against known values, with a failing counterpart for each one.
#
# USAGE
#   enginetests.sh                run every test
#
# ENVIRONMENT
#   NOLF_GAME_DIR   colon-separated data dirs (default: the GOTY discs)
#
# ----------------------------------------------------------------------- //

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "$HERE/.." && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/nolf_enginetest.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

DEFAULT_DIRS="/Users/matthiasschonder/Development/NOLF1/FULLGAME/GOTY/GOTY-disc2/Game:/Users/matthiasschonder/Development/NOLF1/FULLGAME/GOTY/GOTY-disc1/Data:/Users/matthiasschonder/Development/NOLF1/FULLGAME/GOTY/GOTY-disc2/Data"
IFS=':' read -r -a DIRS <<< "${NOLF_GAME_DIR:-$DEFAULT_DIRS}"

# The same flags the engine is built with. ⚠️ Not a reduced set: a test
# compiled with different defines is testing a different program (HANDOVER.md
# §4.3 -- the include-order/ODR trap is exactly this shape).
SYS="-I$R/engine/compat -I$R/engine/compat/include-shims -I$R/LT2/sdk/inc \
 -I$R/LT2/lithshared/incs -I$R/LT2/lithshared/mfcstub \
 -I$R/engine/kernel/io -I$R/engine/kernel/record -I$R/engine/kernel/res \
 -I$R/engine/kernel/tex -I$R/engine/render -I$R/engine/model -I$R/engine/world -I$R/engine/sound"
DEF="-DNO_PRAGMA_LIBS -D_NOLFBUILD -D_REZFILE -D_FINAL -DNDEBUG \
 -D_LITHTECH_ -D_LITHTECH2 -DSTRICT -D_CLIENTBUILD"
FLAGS="-std=c++17 -fdeclspec -Wno-everything -g -O1"

fail=0

run_test()
{
    local name="$1"; shift
    printf '\n--- %s ---\n' "$name"

    if ! clang++ $FLAGS $DEF $SYS -o "$WORK/$name" "$@" 2> "$WORK/$name.log"; then
        echo "  FAIL  did not compile"
        head -25 "$WORK/$name.log" | sed 's/^/        /'
        fail=1
        return
    fi

    if ! "$WORK/$name" "${DIRS[@]}"; then
        fail=1
    fi
}

run_test strings \
    "$HERE/enginetest_strings.cpp" \
    "$R/engine/kernel/res/ltperes.cpp" \
    "$R/engine/kernel/res/ltstringmgr.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp"

run_test pcx \
    "$HERE/enginetest_pcx.cpp" \
    "$R/engine/kernel/tex/ltpcx.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp"

# ⚠️ No game data and no engine headers -- lt2d is deliberately standalone, so
# every case is a handful of pixels with an exactly known answer.
run_test 2d \
    "$HERE/enginetest_2d.cpp" \
    "$R/engine/render/lt2d.cpp"

# ⚠️ Standalone like lt2d, and for the same reason -- every answer here is
# computable by hand, so a failure names the broken RULE, not a wrong picture.
run_test 3d \
    "$HERE/enginetest_3d.cpp" \
    "$R/engine/render/lt3d.cpp"

run_test font \
    "$HERE/enginetest_font.cpp" \
    "$R/engine/render/ltfont.cpp" \
    "$R/engine/render/lt2d.cpp" \
    "$R/engine/kernel/tex/ltpcx.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp"

run_test dtx \
    "$HERE/enginetest_dtx.cpp" \
    "$R/engine/kernel/tex/ltdtx.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp"

run_test world \
    "$HERE/enginetest_world.cpp" \
    "$R/engine/world/ltworld.cpp" \
    "$R/engine/world/ltworldvis.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp"

# ⚠️ Walk mode cannot be tested by running the host: its scripted-key path
# pushes DOWN and UP in the same tick by design, so no scripted run can hold a
# key. The camera is driven through its own API instead.
# ⚠️ walk now links the surface table (footsteps) -- see ltsurfaces.h
run_test walk \
    "$HERE/enginetest_walk.cpp" \
    "$R/engine/world/ltworldrender.cpp" \
    "$R/engine/world/ltproptypes.cpp" \
    "$R/engine/kernel/tex/ltspr.cpp" \
    "$R/engine/world/ltworldwalk.cpp" \
    "$R/engine/world/ltworldscene.cpp" \
    "$R/engine/world/ltworldcollide.cpp" \
    "$R/engine/world/ltworlddoors.cpp" \
    "$R/engine/sound/ltsurfaces.cpp" \
    "$R/engine/world/ltworld.cpp" \
    "$R/engine/model/ltabc.cpp" \
    "$R/engine/model/ltmodeldraw.cpp" \
    "$R/engine/render/lt3d.cpp" \
    "$R/engine/kernel/tex/ltdtx.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp"

# ⚠️ THE ONLY SUITE THAT LINKS A GPU STACK, and it is its own suite for exactly
# that reason: every other suite stays headless C++ with no Cocoa, Metal or
# QuartzCore. It SKIPS rather than fails when no Metal device is present.
if [ -f "$R/engine/render/ltmetal.mm" ]; then
run_test metal \
    "$HERE/enginetest_metal.cpp" \
    "$R/engine/world/ltworldrender.cpp" \
    "$R/engine/world/ltproptypes.cpp" \
    "$R/engine/kernel/tex/ltspr.cpp" \
    "$R/engine/world/ltworldmetal.cpp" \
    "$R/engine/world/ltworldwalk.cpp" \
    "$R/engine/world/ltworldscene.cpp" \
    "$R/engine/world/ltworldcollide.cpp" \
    "$R/engine/world/ltworlddoors.cpp" \
    "$R/engine/sound/ltsurfaces.cpp" \
    "$R/engine/world/ltworld.cpp" \
    "$R/engine/model/ltabc.cpp" \
    "$R/engine/model/ltmodeldraw.cpp" \
    "$R/engine/render/lt3d.cpp" \
    "$R/engine/render/ltmetal.mm" \
    "$R/engine/platform/ltwindow.mm" \
    "$R/engine/kernel/tex/ltdtx.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp" \
    -I"$R/engine/platform" -ObjC++ -fobjc-arc \
    -framework Cocoa -framework Metal -framework QuartzCore
fi

run_test surfaces \
    "$HERE/enginetest_surfaces.cpp" \
    "$R/engine/sound/ltsurfaces.cpp" \
    "$R/engine/kernel/tex/ltdtx.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp" \
    -I"$R/engine/sound"

run_test mixer \
    "$HERE/enginetest_mixer.cpp" \
    "$R/engine/sound/ltmixer.cpp" \
    -I"$R/engine/sound"

# ⚠️ The only sound suite that links AudioToolbox -- the mixer above stays pure
# C++ with no frameworks, exactly as `metal` is the only suite needing a GPU.
if [ -f "$R/engine/sound/ltaudio.mm" ]; then
run_test audio \
    "$HERE/enginetest_audio.cpp" \
    "$R/engine/sound/ltwav.cpp" \
    "$R/engine/sound/ltaudio.mm" \
    "$R/engine/sound/ltmixer.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp" \
    -I"$R/engine/sound" -ObjC++ -fobjc-arc \
    -framework AudioToolbox -framework CoreFoundation
fi

if [ -f "$R/engine/sound/ltaudio.mm" ]; then
run_test soundmgr \
    "$HERE/enginetest_soundmgr.cpp" \
    "$R/engine/sound/ltsoundmgr.cpp" \
    "$R/engine/sound/ltwav.cpp" \
    "$R/engine/sound/ltmixer.cpp" \
    "$R/engine/sound/ltaudio.mm" \
    "$R/engine/kernel/math/ltmathimpl.cpp" \
    "$R/LT2/sdk/inc/ltquatbase.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp" \
    -I"$R/engine/sound" -I"$R/engine/kernel/math" -ObjC++ -fobjc-arc \
    -framework AudioToolbox -framework CoreFoundation
fi

run_test wav \
    "$HERE/enginetest_wav.cpp" \
    "$R/engine/sound/ltwav.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp" \
    -I"$R/engine/sound"

# ⚠️ ltmodeldraw + lt3d are linked here for the NORMAL ORACLE -- the reader
# tests alone cannot say whether the renderer USES what it read correctly, and
# for a day it did not (HANDOVER.md 5aq).
run_test abc \
    "$HERE/enginetest_abc.cpp" \
    "$R/engine/model/ltabc.cpp" \
    "$R/engine/model/ltmodeldraw.cpp" \
    "$R/engine/render/lt3d.cpp" \
    "$R/engine/kernel/io/ltrezfile.cpp" \
    "$R/engine/kernel/io/ltfilemgr.cpp" \
    "$R/engine/kernel/io/ltfilestream.cpp" \
    "$R/engine/kernel/record/ltrecord.cpp"

echo
if [ "$fail" != "0" ]; then
    echo "=== ENGINE TESTS FAILED ==="
    exit 1
fi
echo "=== engine tests passed ==="
