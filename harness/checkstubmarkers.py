#!/usr/bin/env python3
# ----------------------------------------------------------------------- //
#
# checkstubmarkers.py -- KEEP THE STUB CENSUS HONEST.
#
# WHAT WENT WRONG, AND WHY THIS EXISTS (HANDOVER.md 5w)
#
#   LTSTUB(name) only REPORTS. It does not return, and the code after it runs.
#   Left at the top of a function that was later finished, it keeps filing that
#   function as missing -- forever, with nothing to indicate otherwise.
#
#   56 of them were doing exactly that: every hand-written LTSTUB site in the
#   engine sat inside implemented code. The worst was ILTClient::RenderCamera,
#   52 lines that draw every level this port renders, which the census listed
#   as a stub for months while HANDOVER 6 called it "the single call between
#   here and seeing a level".
#
#   The census IS the to-do list -- this port picks its next task from it, in
#   first-reached order, because that order is a dependency order. A list that
#   keeps finished work on it sends the next person to re-do something that
#   already works.
#
# THE INVARIANT
#
#   An LTSTUB() site's function body must be TRIVIAL: nothing after the marker,
#   or a single bare return. Anything that does real work is implemented and
#   must say LTIMPL() instead.
#
#   It is deliberately NOT "no LTSTUB in hand-written code". Writing a real
#   hand-rolled stub is legitimate and should stay legible as one; what is not
#   legitimate is a stub marker over working code.
#
# USAGE
#   checkstubmarkers.py <file-or-dir> [...]      exit 1 on any violation
#
# ----------------------------------------------------------------------- //

import os, re, sys

# skeleton_generated.* is machine-written and is stubs BY CONSTRUCTION;
# ltstub.* defines the markers and mentions them in its own comments.
SKIP_FILES = {'skeleton_generated.h', 'skeleton_generated.cpp',
              'ltstub.h', 'ltstub.cpp'}
EXTS = ('.cpp', '.mm', '.c')


def body_after(src, marker_end):
    """The enclosing function's body after the marker, brace-matched."""
    ob = src.rfind('{', 0, marker_end)
    if ob < 0:
        return None
    depth, i = 0, ob
    while i < len(src):
        if src[i] == '{':
            depth += 1
        elif src[i] == '}':
            depth -= 1
            if depth == 0:
                break
        i += 1
    return src[marker_end:i]


def strip(text):
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'\s+', ' ', text).strip()
    return text.lstrip(';').strip()


# A trivial body: empty, or one bare return (optionally of a constant/ident).
TRIVIAL = re.compile(r'(return\s*[A-Za-z0-9_:\-\.\(\)]*\s*;?)?\s*$')


def check(path):
    try:
        src = open(path, errors='ignore').read()
    except OSError:
        return []
    out = []
    for m in re.finditer(r'\bLTSTUB\s*\(\s*"([^"]+)"\s*\)\s*;?', src):
        b = body_after(src, m.end())
        if b is None:
            continue
        b = strip(b)
        if not TRIVIAL.fullmatch(b):
            line = src[:m.start()].count('\n') + 1
            out.append((path, line, m.group(1), b[:90]))
    return out


def walk(target):
    if os.path.isfile(target):
        return check(target)
    found = []
    for root, _dirs, files in os.walk(target):
        for f in files:
            if f.endswith(EXTS) and f not in SKIP_FILES:
                found += check(os.path.join(root, f))
    return found


def main(argv):
    if len(argv) < 2:
        print("usage: checkstubmarkers.py <file-or-dir> [...]", file=sys.stderr)
        return 2

    bad = []
    for t in argv[1:]:
        bad += walk(t)

    if not bad:
        return 0

    print("LTSTUB marker over working code -- these are IMPLEMENTED and must "
          "use LTIMPL():", file=sys.stderr)
    for path, line, name, snippet in sorted(bad):
        print("  %s:%d  %s" % (path, line, name), file=sys.stderr)
        print("      body: %s" % snippet, file=sys.stderr)
    print("\n%d marker(s) misfiled. The stub census is the to-do list; these "
          "would sit on it forever. (HANDOVER.md 5w)" % len(bad), file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
