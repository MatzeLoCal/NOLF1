#!/bin/bash
# ----------------------------------------------------------------------- //
#
# selftest.sh -- VALIDATE THE INSTRUMENTS, not the port.
#
# WHY THIS IS THE MOST IMPORTANT FILE IN THIS DIRECTORY
# (background doc §3.5)
#
#   The NOLF 2 port produced confident wrong answers three separate times from
#   bad instruments: a console printer that was never connected, so 49
#   diagnostics were invisible; a file trace heavy enough to make a run look
#   like it had no audio; and an envelope cross-correlation that scored a
#   NEGATIVE CONTROL highest. Each cost real time, and each would have been
#   caught in minutes by a test like this one.
#
#   A harness is not exempt from needing tests. It is the thing most in need of
#   them, because when it is wrong every answer it gives afterwards is wrong and
#   confident.
#
# WHAT IT CHECKS
#
#   framediff.py
#     1. identical images report 0.00 and pass
#     2. ⚠️ NEGATIVE CONTROL: a differ that always returns 0 passes test 1.
#        So a ONE-PIXEL, ONE-UNIT change must FAIL at the default threshold.
#     3. a known analytic value -- half black / half white is exactly 127.5
#     4. a size mismatch is an ERROR, not a large number
#     5. the amplified diff image is actually written
#
#   the run record
#     6. two runs of the same binary produce byte-identical records
#     7. ⚠️ NEGATIVE CONTROL: a deliberately perturbed run must BREAK the
#        contract. Without this, an empty record diffed against an empty
#        reference passes forever and the gate is decorative.
#     8. the reference is not trivially small
#
#   the stub census
#     9. every LTSTUB marker sits on a genuinely empty function -- a marker
#        left over working code files finished work as missing, and the census
#        is the to-do list (HANDOVER.md 5w)
#    10. ⚠️ NEGATIVE CONTROL: a marker over a working function must be caught,
#        and a genuine hand-written stub beside it must NOT be.
#
# USAGE
#   selftest.sh          run everything
#
# ----------------------------------------------------------------------- //

set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/nolf_selftest.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

ok()   { echo "  ok    $*"; pass=$((pass+1)); }
bad()  { echo "  FAIL  $*"; fail=$((fail+1)); }

echo "=== instrument self-test ==="
echo

# --------------------------------------------------------------------- //
echo "framediff.py"

python3 - "$WORK" <<'PY'
import sys
from PIL import Image
w = sys.argv[1]

# identical pair
a = Image.new("RGB", (64, 32), (10, 20, 30))
a.save(w + "/same_a.png")
a.save(w + "/same_b.png")

# one pixel differing by exactly 1 in one channel -- mean is 1/(64*32*3)
# = 0.000163, which rounds to 0.000 in any sane report. It must still FAIL.
b = a.copy()
b.putpixel((17, 5), (11, 20, 30))
b.save(w + "/onepixel.png")

# half black / half white: mean abs must be exactly 127.5
left  = Image.new("RGB", (64, 32), (0, 0, 0))
right = Image.new("RGB", (64, 32), (255, 255, 255))
half  = Image.new("RGB", (64, 32), (0, 0, 0))
half.paste(right.crop((0, 0, 32, 32)), (0, 0))
left.save(w + "/black.png")
half.save(w + "/half.png")

# different size
Image.new("RGB", (65, 32), (10, 20, 30)).save(w + "/wrongsize.png")
PY

FD="$HERE/framediff.py"

out=$("$FD" "$WORK/same_a.png" "$WORK/same_b.png" 2>&1); rc=$?
if [ $rc -eq 0 ] && echo "$out" | grep -q "contract holds"; then
    ok "identical images pass"
else
    bad "identical images should pass (rc=$rc)"; echo "$out" | sed 's/^/        /'
fi

out=$("$FD" "$WORK/same_a.png" "$WORK/onepixel.png" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "max   1"; then
    ok "NEGATIVE CONTROL: a 1-pixel 1-unit change fails, and max is reported"
else
    bad "a 1-pixel change must fail at threshold 0 (rc=$rc)"; echo "$out" | sed 's/^/        /'
fi

out=$("$FD" --max-mean 200 "$WORK/black.png" "$WORK/half.png" 2>&1)
if echo "$out" | grep -q "mean abs 127.500"; then
    ok "known analytic value: half black/white is 127.500"
else
    bad "half black/white must be exactly 127.500"; echo "$out" | sed 's/^/        /'
fi

out=$("$FD" "$WORK/same_a.png" "$WORK/wrongsize.png" 2>&1); rc=$?
if [ $rc -ne 0 ] && echo "$out" | grep -q "size mismatch"; then
    ok "size mismatch is an error, not a number"
else
    bad "size mismatch must be an error (rc=$rc)"; echo "$out" | sed 's/^/        /'
fi

"$FD" --out-dir "$WORK/diffs" "$WORK/black.png" "$WORK/half.png" > /dev/null 2>&1
if [ -f "$WORK/diffs/black.png.diff.png" ]; then
    ok "difference image is written"
else
    bad "difference image was not written"
fi

# --------------------------------------------------------------------- //
echo
echo "run record"

. "$HERE/../nolfbin.sh"
BIN="${NOLF_BIN:-$NOLF_BIN_DEFAULT}"
if [ ! -x "$BIN" ]; then
    echo "  SKIP  no host binary at $BIN -- build it first"
    echo
    echo "=== $pass passed, $fail failed (record tests skipped) ==="
    [ $fail -eq 0 ] || exit 1
    exit 0
fi

"$HERE/runrecord.sh" "$WORK/r1.record" > /dev/null 2>&1
"$HERE/runrecord.sh" "$WORK/r2.record" > /dev/null 2>&1

# ⚠️ A MINIMUM EVENT COUNT, not just "non-empty".
#
# `-s` passes on a one-byte file, and an almost-empty record compared against
# an almost-empty record is byte-identical -- so this test would report perfect
# determinism about nothing. That is not hypothetical: a stray NUL byte written
# by the crash handler made both records BINARY to grep, the event count read
# as 0, and every record test still said ok.
n1=$(grep -vc '^#' "$WORK/r1.record" 2>/dev/null || echo 0)

if [ "$n1" -lt 20 ]; then
    bad "record has only $n1 events -- too thin to prove anything"
    head -5 "$WORK/r1.record" | sed 's/^/        /'
elif ! LC_ALL=C grep -q . "$WORK/r1.record" 2>/dev/null; then
    bad "record is not readable as text (binary content?)"
elif diff -q "$WORK/r1.record" "$WORK/r2.record" > /dev/null; then
    ok "two runs produce byte-identical records ($n1 events)"
else
    bad "runs are not deterministic"
    diff -u "$WORK/r1.record" "$WORK/r2.record" 2>&1 | head -20 | sed 's/^/        /'
fi

# ⚠️ THE TEST THAT KEEPS THE GATE HONEST.
#
# Run against ONE data directory instead of three. That is a real change to what
# the engine can see -- fewer trees, and files resolving from different archives
# -- so the record MUST differ. If it does not, the record is not capturing the
# thing it claims to capture, and every green result afterwards is meaningless.
# The first directory the record itself names, so this does not hardcode a
# machine layout. Falls back to NOLF_SELFTEST_DIR if the record shape changes.
PERTURBED="${NOLF_SELFTEST_DIR:-$(awk '$1=="tree" && $2=="dir" {print $3; exit}' "$WORK/r1.record")}"
[ -n "$PERTURBED" ] && PERTURBED="$(find /Users/matthiasschonder/Development/NOLF1/FULLGAME -type d -path "*/$PERTURBED" 2>/dev/null | head -1)"

if [ -n "$PERTURBED" ] && [ -d "$PERTURBED" ]; then
    NOLF_GAME_DIR="$PERTURBED" "$HERE/runrecord.sh" "$WORK/r3.record" > /dev/null 2>&1
    n=$(diff "$WORK/r1.record" "$WORK/r3.record" 2>/dev/null | grep -c '^[<>]')
    # ⚠️ A THRESHOLD, not "differs at all". `diff -q` also reports a difference
    # when one side is missing or binary, so "they differ" was passing for
    # reasons that had nothing to do with the perturbation. Dropping three of
    # the seven trees changes far more than four lines.
    if [ "$n" -ge 5 ]; then
        ok "NEGATIVE CONTROL: a perturbed run breaks the contract ($n lines differ)"
    else
        bad "perturbed run differs by only $n lines -- the gate is not measuring what it claims"
    fi
else
    echo "  SKIP  perturbation test: $PERTURBED not present"
fi

REF="$HERE/reference/skeleton_client.record"
if [ -f "$REF" ]; then
    n=$(grep -vc '^#' "$REF")
    if [ "$n" -ge 20 ]; then
        ok "reference has $n events"
    else
        bad "reference has only $n events -- too thin to be a contract"
    fi
else
    echo "  SKIP  no reference yet -- create with checkcontract.sh --bless"
fi

# --------------------------------------------------------------------- //
#
# ⚠️ THE STUB CENSUS IS AN INSTRUMENT, AND IT WAS WRONG FOR MONTHS.
#
# LTSTUB() only REPORTS -- it does not return, and the code after it runs. Left
# at the top of a function that was later finished, it files that function as
# missing forever. 56 of them did, including ILTClient::RenderCamera, which
# draws every level this port renders. The census is the TO-DO LIST, picked
# from in first-reached order, so finished work sat on it sending the next
# person to re-do something that already works. (HANDOVER.md 5w)
#
echo
echo "stub census markers"

if [ -x "$HERE/checkstubmarkers.py" ] || [ -f "$HERE/checkstubmarkers.py" ]; then

    if out=$(python3 "$HERE/checkstubmarkers.py" "$HERE/../engine" 2>&1); then
        ok "every LTSTUB marker sits on a genuinely empty function"
    else
        bad "LTSTUB marker over working code -- use LTIMPL (HANDOVER.md 5w)"
        echo "$out" | sed 's/^/        /'
    fi

    # ⚠️ NEGATIVE CONTROL. A checker that never fails is decorative -- this file
    # exists because three instruments in the NOLF 2 port passed while broken.
    # A marker over a function that plainly does work MUST be caught, and a
    # genuine stub next to it MUST NOT be.
    NEG="$WORK/stubmarkers"
    mkdir -p "$NEG"
    cat > "$NEG/fake.cpp" <<'FAKE'
static LTRESULT SC_Genuine(int a)
{
    LTSTUB("ILTFake::Genuine");
    return LT_ERROR;
}

static LTRESULT SC_Finished(int a)
{
    LTSTUB("ILTFake::Finished");
    Thing* p = Lookup(a);
    if (!p) return LT_ERROR;
    p->Do(a);
    return LT_OK;
}
FAKE

    if neg=$(python3 "$HERE/checkstubmarkers.py" "$NEG" 2>&1); then
        bad "NEGATIVE CONTROL: a marker over working code was NOT caught"
    elif echo "$neg" | grep -q 'ILTFake::Genuine'; then
        bad "NEGATIVE CONTROL: a GENUINE stub was wrongly flagged"
    elif echo "$neg" | grep -q 'ILTFake::Finished'; then
        ok "NEGATIVE CONTROL: implemented caught, genuine stub left alone"
    else
        bad "NEGATIVE CONTROL: checker failed for the wrong reason"
        echo "$neg" | sed 's/^/        /'
    fi
else
    echo "  SKIP  checkstubmarkers.py not present"
fi

echo
echo "=== $pass passed, $fail failed ==="
[ $fail -eq 0 ] || exit 1
exit 0
