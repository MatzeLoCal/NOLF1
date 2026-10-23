#!/usr/bin/env python3
"""framediff.py -- the pixel half of the frame contract.

WHY THIS EXISTS BEFORE THERE IS A RENDERER
------------------------------------------
NOLF 1 cannot draw anything yet, so there is nothing to diff. That is precisely
why this is written now: PORT_PLAN.md §4 says the harness comes first, and the
background doc §3.5 says the instrument itself has to be validated. Debugging
the differ at the same moment as debugging the first frame means neither can be
trusted -- when the numbers look wrong you will not know which half is lying.

So this ships validated (see selftest.sh) and simply waits.

WHAT IT MEASURES
----------------
Mean absolute difference over every channel of every pixel, 0..255 -- the same
metric the NOLF 2 port's contract uses, so its "four c01s01 frames at 0.00 mean
abs" number is directly comparable.

Also reported, because one number is not enough to discriminate:

  * percentage of pixels differing at all -- a tiny mean hides a small bright
    defect; a large mean can be a uniform gamma shift that is visually fine.
    The two together say WHICH kind of drift happened.
  * the worst single channel difference, and where it is. A mean of 0.01 with a
    max of 255 is a handful of catastrophically wrong pixels, not a rounding
    change, and it is the case a mean-only report reads as a pass.

⚠️ THIS EXITS NON-ZERO WHEN THE CONTRACT IS BROKEN.
A differ that only prints is a differ that gets ignored. The default threshold
is 0.00 -- exact equality -- and loosening it is a deliberate act with a number
attached to it, not a default.
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageChops
except ImportError:
    sys.exit("framediff.py needs Pillow:  python3 -m pip install pillow")


def load(path):
    return Image.open(path).convert("RGB")


def compare(a_path, b_path):
    """Returns (mean, pct_changed, max_diff, max_pos, error-or-None)."""
    if not os.path.exists(a_path):
        return None, None, None, None, "missing: %s" % a_path
    if not os.path.exists(b_path):
        return None, None, None, None, "missing: %s" % b_path

    a, b = load(a_path), load(b_path)
    if a.size != b.size:
        # Not a small difference: a size change means the comparison is
        # meaningless, so it is an error rather than a large number.
        return None, None, None, None, "size mismatch %s vs %s" % (a.size, b.size)

    diff = ImageChops.difference(a, b)

    # Mean over all channels, straight from the histogram: exact and fast.
    hist = diff.histogram()
    total = 0
    count = 0
    for ch in range(3):
        for value in range(256):
            n = hist[ch * 256 + value]
            total += value * n
            count += n
    mean = total / count if count else 0.0

    # Pixels differing in ANY channel.
    bands = diff.split()
    worst_band = bands[0]
    for band in bands[1:]:
        worst_band = ImageChops.lighter(worst_band, band)

    changed = sum(n for v, n in enumerate(worst_band.histogram()) if v)
    pct = 100.0 * changed / (a.size[0] * a.size[1])

    max_diff = worst_band.getextrema()[1]
    max_pos = None
    if max_diff:
        # getextrema gives the value, not the place; the place is what makes a
        # report actionable.
        #
        # ⚠️ tobytes(), not getdata(). getdata() returns an ImagingCore, which
        # is sequence-like enough to look right and has no .index() -- so this
        # line raised AttributeError only on the path where something actually
        # differed, i.e. never during a passing run. selftest.sh caught it on
        # its first execution; nothing else would have, until the day a real
        # frame regressed and the differ crashed instead of reporting.
        w = a.size[0]
        idx = worst_band.tobytes().index(max_diff)     # 'L' band: 1 byte/pixel
        max_pos = (idx % w, idx // w)

    return mean, pct, max_diff, max_pos, None


def write_diff_image(a_path, b_path, out_path, amplify):
    """Amplified difference image. A 1-unit drift is invisible at 1x, and
    'I looked at the diff and it was black' is how a real defect gets waved
    through."""
    a, b = load(a_path), load(b_path)
    if a.size != b.size:
        return False
    diff = ImageChops.difference(a, b)
    if amplify != 1:
        diff = diff.point(lambda v: min(255, v * amplify))
    diff.save(out_path)
    return True


def main():
    ap = argparse.ArgumentParser(description="Frame-contract pixel differ.")
    ap.add_argument("pairs", nargs="+", metavar="A B",
                    help="pairs of images to compare")
    ap.add_argument("--max-mean", type=float, default=0.0,
                    help="fail if mean abs difference exceeds this (default 0.0 "
                         "-- exact equality)")
    ap.add_argument("--max-pixels-pct", type=float, default=100.0,
                    help="fail if more than this %% of pixels differ")
    ap.add_argument("--out-dir", default=None,
                    help="write an amplified difference image per pair here")
    ap.add_argument("--amplify", type=int, default=8,
                    help="multiplier for difference images (default 8)")
    ap.add_argument("-q", "--quiet", action="store_true",
                    help="print only failures")
    args = ap.parse_args()

    if len(args.pairs) % 2 != 0:
        sys.exit("framediff.py: images must come in pairs")

    if args.out_dir:
        os.makedirs(args.out_dir, exist_ok=True)

    worst = 0.0
    failures = 0
    n = 0

    for i in range(0, len(args.pairs), 2):
        a_path, b_path = args.pairs[i], args.pairs[i + 1]
        label = os.path.basename(a_path)
        n += 1

        mean, pct, max_diff, max_pos, err = compare(a_path, b_path)
        if err:
            print("FAIL %-28s  %s" % (label, err))
            failures += 1
            continue

        bad = (mean > args.max_mean) or (pct > args.max_pixels_pct)
        if bad:
            failures += 1
        worst = max(worst, mean)

        if bad or not args.quiet:
            where = " at (%d,%d)" % max_pos if max_pos else ""
            print("%s %-28s  mean abs %6.3f   differing %6.2f%%   max %3d%s"
                  % ("FAIL" if bad else "ok  ", label, mean, pct, max_diff, where))

        if args.out_dir and (max_diff or 0) > 0:
            out = os.path.join(args.out_dir, label + ".diff.png")
            if write_diff_image(a_path, b_path, out, args.amplify):
                print("      diff image (%dx amplified): %s" % (args.amplify, out))

    print("\n%d pair(s), worst mean abs %.3f, threshold %.3f -- %s"
          % (n, worst, args.max_mean,
             "CONTRACT BROKEN" if failures else "contract holds"))

    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
