#!/bin/bash
# ----------------------------------------------------------------------- //
#
# checkcontract.sh -- THE GATE.
#
# Runs the host and diffs the resulting run record against the archived
# reference. Exits non-zero on any drift.
#
# This is the thing PORT_PLAN.md §4 and the background doc §3.1 are asking for,
# in the form NOLF 1 can currently support: a deterministic artefact of a
# known-good run, archived, diffed automatically, where drift is a failure until
# a human says otherwise.
#
# USAGE
#   checkcontract.sh              check against harness/reference/
#   checkcontract.sh --bless      accept the current run AS the new reference
#
# ⚠️ --bless IS THE DANGEROUS ONE. Blessing a record makes whatever the code
# does right now "correct" by definition. Do it only after reading the diff and
# deciding every line of it was intended. The NOLF 2 contract's value came
# entirely from people NOT doing this reflexively.
#
# ----------------------------------------------------------------------- //

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../nolfbin.sh"
REF="$HERE/reference/skeleton_client.record"
TMP="${TMPDIR:-/tmp}/nolf_contract_$$.record"

BLESS=0
[ "${1:-}" = "--bless" ] && BLESS=1

trap 'rm -f "$TMP"' EXIT

"$HERE/runrecord.sh" "$TMP" || exit 1

if [ "$BLESS" = "1" ]; then
    mkdir -p "$(dirname "$REF")"
    if [ -f "$REF" ] && ! diff -q "$REF" "$TMP" > /dev/null; then
        echo "changes being blessed:"
        diff -u "$REF" "$TMP" | sed 's/^/    /'
        echo
    fi
    cp "$TMP" "$REF"
    echo "blessed: $REF"

    mkdir -p "$HERE/reference/frames"
    for spec in "120:splash_f120.ppm" "700:menu3d_f700.ppm"; do
        n="${spec%%:*}"; f="${spec#*:}"
        # ⚠️ A THROWAWAY working directory, exactly as runrecord.sh uses one.
        # See the gate below for why this is not optional.
        fd=$(mktemp -d "${TMPDIR:-/tmp}/nolf_frame.XXXXXX")
        # ⚠️ LT_REALTIME=0 PINS THE FIXED TIMESTEP. The launcher now asks for
        # the wall clock, because a fixed step runs the GAME in slow motion
        # whenever the renderer misses 60 fps -- but a contract measured on the
        # wall clock is a different picture every run. The gate pins it off,
        # on BOTH runs.
        #
        # ⚠️ LT_NOSERVER here too, and it must MATCH the check run below
        # exactly. A reference blessed under different conditions than the gate
        # measures is a gate that fails forever for a reason nobody can see.
        ( cd "$fd" && LT_UPDATES="$n" \
            LT_DUMP_SCREEN="$HERE/reference/frames/$f" \
            LT_NOSERVER=1 LT_REALTIME=0 LT_SCREEN=640x480 LT_STUB_QUIET=1 "${NOLF_BIN:-$NOLF_BIN_DEFAULT}" > /dev/null 2>&1 )
        rm -rf "$fd"
        echo "blessed: $HERE/reference/frames/$f"
    done
    exit 0
fi

if [ ! -f "$REF" ]; then
    echo "checkcontract.sh: no reference at $REF"
    echo "  create one with: checkcontract.sh --bless"
    exit 2
fi

# ------------------------------------------------------------------ //
# THE PIXEL HALF.
#
# framediff.py was written and validated against synthetic images long before
# anything could render, and sat dormant. It has input now: a fixed-frame-count
# run in FIXED TIMESTEP mode produces the same pixels every time, so the
# archived frame is a real contract rather than a snapshot of a race.
#
# ⚠️ Separate run, deliberately. The record run is 3 frames and is about the
# CALL SEQUENCE; the frame run is 120 frames because that is where the intro
# fade peaks and there is something to look at. One run cannot serve both
# without making the record 40x longer for no gain.
# ------------------------------------------------------------------ //

# ⚠️ TWO frames now, and the second one is the point.
#
#   splash_f120   the 2D intro fade -- the compositor, fonts and PCX path.
#   menu3d_f700   the MAIN MENU, which is a 3D SCENE: Cate's model, six FX
#                 models, real DTX skins, through the software rasteriser.
#
# The second frame is what makes the pixel contract cover the renderer, and it
# is only gateable because the rasteriser is SOFTWARE and therefore bit-exact
# (lt3d.h). A GPU frame could not be diffed at 0.000 against anything but
# itself on one machine -- which is the whole reason lt3d exists.
frames_ok=1

for spec in "120:splash_f120.ppm" "700:menu3d_f700.ppm"; do
    n="${spec%%:*}"; f="${spec#*:}"
    REFFRAME="$HERE/reference/frames/$f"
    [ -f "$REFFRAME" ] || continue

    GOT="${TMPDIR:-/tmp}/nolf_frame_$$_$n.ppm"

    # ⚠️ A THROWAWAY WORKING DIRECTORY, AND IT IS LOAD-BEARING.
    #
    #   The host creates a `Save` directory if one is absent
    #   (GameClientShell.cpp:1015) and writes Save\intel.sav into it. That is
    #   real game behaviour, not a port artefact -- but it means the SECOND run
    #   in a given directory behaves differently from the first, because
    #   DirExist("Save") now answers yes and the main menu gains a "Continue
    #   game" item.
    #
    #   Measured: two runs in the same directory differ by **35.91% of pixels**
    #   (mean abs 6.255). The run itself is perfectly deterministic -- three
    #   runs in a fresh directory are byte-identical -- so this would have read
    #   as "the renderer is non-deterministic" and sent someone hunting through
    #   the rasteriser. HANDOVER.md §4.9 flagged the hazard; the frame gate is
    #   simply the first thing sensitive enough to trip it.
    fd=$(mktemp -d "${TMPDIR:-/tmp}/nolf_frame.XXXXXX")
    # ⚠️ LT_NOSERVER, for the same reason runrecord.sh pins it: the frame
    # contract is a recording of a run with NO SERVER in it. The server is on
    # by default now (a player pressing Start in the menus needs one), so the
    # gate has to ask for the old behaviour explicitly or it is diffing a
    # different program against the blessed reference.
    #
    # ⚠️ This was not hypothetical for one build: with a server, this run
    # dlopen'd the server module and aborted before drawing, and the gate
    # reported "no frame was dumped" -- a true statement that named neither
    # the server nor the module.
    ( cd "$fd" && LT_UPDATES="$n" LT_DUMP_SCREEN="$GOT" \
        LT_NOSERVER=1 LT_REALTIME=0 LT_SCREEN=640x480 LT_STUB_QUIET=1 "${NOLF_BIN:-$NOLF_BIN_DEFAULT}" > /dev/null 2>&1 )
    rm -rf "$fd"

    if [ -f "$GOT" ]; then
        if ! "$HERE/framediff.py" -q "$REFFRAME" "$GOT"; then
            echo "   (frame that differs: $f)"
            frames_ok=0
        fi
        rm -f "$GOT"
    else
        echo "⚠️  no frame was dumped for $f"
        frames_ok=0
    fi
done

if diff -q "$REF" "$TMP" > /dev/null && [ "$frames_ok" = "1" ]; then
    lines=$(grep -vc '^#' "$REF")
    echo "contract holds  ($lines events match $(basename "$REF"), 2 frames match)"
    exit 0
fi

if diff -q "$REF" "$TMP" > /dev/null; then
    echo "⚠️  the run record matches but the FRAME does not."
    exit 1
fi

echo "⚠️  CONTRACT BROKEN -- the run no longer matches the reference."
echo
diff -u "$REF" "$TMP" | sed 's/^/    /'
echo
echo "reference: $REF"
echo
echo "If every one of those changes is intended, re-bless with:"
echo "    $0 --bless"
exit 1
