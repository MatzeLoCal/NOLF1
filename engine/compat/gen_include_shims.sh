#!/bin/bash
# gen_include_shims.sh
#
# The original NOLF 1 / LT2 sources use Windows-style backslash separators in
# some quoted #includes, e.g.:
#
#     #include "..\ClientShellDLL\BaseScaleFX.h"
#
# clang treats that whole string as a single filename, so it cannot resolve.
#
# Rather than edit the original sources, this generates a flat directory of
# symlinks whose FILENAMES are the literal backslash strings, pointing at the
# real headers. Put the directory on the include path with -I and the original
# sources compile untouched.
#
# ⚠️ DO NOT RE-RUN AS IT STANDS (2026-10-18, HANDOVER 5cn). It begins with
# `rm -rf` of the shim directory, and the checked-in set is NOT its output:
# re-run on the current tree it leaves 20 includes unresolved (lithtmpl.h's
# ..\lith\*, WonShared.h, dibmgr.h, zlib.h, rezmgr.h, GameSpyClientMgr.h's
# ..\cengine\goaceng.h) that the shipped set has links for. The 53 entries in
# include-shims/ are the source of truth; treat this script as a record of how
# most of them were made, and snapshot the directory before touching it.

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SHIMDIR="$ROOT/engine/compat/include-shims"

rm -rf "$SHIMDIR"
mkdir -p "$SHIMDIR"

made=0; collided=0; missing=0

# Find every (including-file, backslash-include-path) pair.
while IFS= read -r line; do
    src="${line%%:*}"
    inc="${line#*:}"
    [ -n "$inc" ] || continue

    # Resolve the include relative to the including file's directory,
    # translating backslashes to slashes to find the real target.
    posix="${inc//\\//}"
    target="$(cd "$(dirname "$src")" 2>/dev/null && cd "$(dirname "$posix")" 2>/dev/null && pwd)/$(basename "$posix")"

    if [ ! -f "$target" ]; then
        # Some backslash includes name a SYSTEM header, e.g. "sys\stat.h".
        # Those resolve to nothing in the project tree, so emit a forwarding
        # stub instead of a symlink.
        case "$posix" in
            sys/*|time.h|io.h|direct.h)
                printf '#include <%s>\n' "$posix" > "$SHIMDIR/$inc"
                made=$((made+1))
                continue
                ;;
        esac

        # Fallback: some includes are written as though the including file sat
        # one directory shallower than it does -- e.g. GameSpyServer.h, which
        # lives in lithshared/gamespy/gamespyclientmgr/, includes
        # "..\gamespy\cengine\goaceng.h". Resolving that literally walks into a
        # doubled `gamespy/gamespy/` that does not exist, yet the file is
        # plainly present. Retry against each ancestor directory and accept the
        # result only if EXACTLY ONE ancestor yields a match, so an ambiguous
        # name is reported rather than guessed.
        probe="$(dirname "$src")"
        found=""; nfound=0
        while [ "$probe" != "/" ] && [ "$probe" != "." ]; do
            cand="$probe/$posix"
            if [ -f "$cand" ]; then
                real="$(cd "$(dirname "$cand")" && pwd)/$(basename "$cand")"
                if [ "$real" != "$found" ]; then
                    found="$real"; nfound=$((nfound+1))
                fi
            fi
            probe="$(dirname "$probe")"
        done

        if [ "$nfound" -eq 1 ]; then
            ln -s "$found" "$SHIMDIR/$inc" 2>/dev/null && made=$((made+1))
            continue
        elif [ "$nfound" -gt 1 ]; then
            echo "  AMBIGUOUS: $inc  ($nfound candidates -- not guessing)"
        fi

        echo "  MISSING: $inc  (from $src)"
        missing=$((missing+1))
        continue
    fi

    link="$SHIMDIR/$inc"
    if [ -L "$link" ]; then
        prev="$(readlink "$link")"
        if [ "$prev" != "$target" ]; then
            echo "  COLLISION: '$inc' -> $prev  AND  $target"
            collided=$((collided+1))
        fi
        continue
    fi
    ln -s "$target" "$link" && made=$((made+1))
done < <(
    # NB: skip lines where the #include is commented out. The original sources
    # contain e.g.  //#include "..\ltguimgr\coolfont.h"  for headers that were
    # cut from the public SDK -- counting those reports phantom failures.
    grep -rn '#include[[:space:]]*"[^"]*\\' "$ROOT/NOLF" "$ROOT/LT2" 2>/dev/null \
    | grep -v ':[[:space:]]*//' \
    | sed -E 's/^([^:]+):[0-9]+:.*#include[[:space:]]*"([^"]*)".*/\1:\2/'
)

# ⚠️ RELATIVE LINKS, NOT ABSOLUTE (HANDOVER 5cm). The links above are made
# from absolute paths, and a copied or moved tree then still includes the OLD
# tree's headers: `#pragma once` sees two files and CGameSpyServer is defined
# twice. Rewritten here so the tree stands on its own wherever it is.
for link in "$SHIMDIR"/*; do
    [ -L "$link" ] || continue
    t="$(readlink "$link")"
    case "$t" in
        /*) rel="$(/usr/bin/python3 -I -c 'import os,sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' "$t" "$SHIMDIR")"
            ln -sfn "$rel" "$link" ;;
    esac
done

echo "include shims: $made created, $collided collisions, $missing unresolved"
echo "shim dir: $SHIMDIR"
[ "$collided" -eq 0 ] && [ "$missing" -eq 0 ]
