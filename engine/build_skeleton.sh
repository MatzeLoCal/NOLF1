#!/bin/bash
# ----------------------------------------------------------------------- //
#
# build_skeleton.sh -- build the NOLF 1 skeleton host from scratch.
#
# WHY THIS EXISTS
#
#   Until now the port was built by ad-hoc clang++ command lines recorded only
#   in HANDOVER.md §3. That is fine for one session and lossy across sessions:
#   the object directory lives in /tmp and disappears on reboot, and every flag
#   in it is load-bearing (see below). This script is the executable version of
#   that section.
#
#   It is NOT a substitute for the Xcode project that rule 4 calls for -- it is
#   what keeps the tree buildable until that project exists.
#
# WHAT IT BUILDS
#
#   ObjectDLL      \  compiled from the .dsp file lists, NOT from the directory
#   ClientShellDLL /  listing -- see HANDOVER.md 4.2.
#   Shared            compiled TWICE, once per flavour -- see HANDOVER.md 4.1.
#   engine/libs       the support libraries.
#   engine/skeleton   the generated engine stand-in and the host.
#
# USAGE
#   ./build_skeleton.sh            incremental (skips up-to-date objects)
#   ./build_skeleton.sh clean      wipe the object tree first
#   ./build_skeleton.sh -j 12      override the job count
#
# ----------------------------------------------------------------------- //

set -u

R="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
O="${NOLF_OBJ:-/tmp/nolf_o}"

# ⚠️ ONE definition of where the binary goes, shared with the harness.
# See nolfbin.sh for what went wrong when there were two.
. "$R/nolfbin.sh"
OUT="${NOLF_BIN:-$NOLF_BIN_DEFAULT}"
SERVER_OUT="${NOLF_SERVER:-${OUT%/*}/libnolfobject.dylib}"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 8)

while [ $# -gt 0 ]; do
    case "$1" in
        clean) rm -rf "$O"; echo "cleaned $O"; shift ;;
        -j)    JOBS="$2"; shift 2 ;;
        *)     echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# --- flags ------------------------------------------------------------- //
#
# EVERY ONE OF THESE IS LOAD-BEARING. See HANDOVER.md 3 for the full reasons;
# the short version:
#
#   -iquote (not -I) for the game dirs   NOLF/Shared/unistd.h is a ZERO-BYTE
#                                        file. On -I it shadows the system
#                                        <unistd.h> and silently deletes every
#                                        POSIX declaration.
#   -fdeclspec                           ltserverobj.h's DEFINE_CLASSES() needs
#                                        __declspec.
#   -DSTRICT                             decides WNDPROC vs FARPROC for
#                                        g_pfnMainWndProc; without it
#                                        CallWindowProc does not match.
#   WIN32 / _WINDOWS deliberately UNSET  they come from the .dsp files, which is
#                                        also where every other -D here is from
#                                        (the "Final" configuration).

SYS="-I$R/engine/compat -I$R/engine/compat/include-shims -I$R/LT2/sdk/inc \
 -I$R/LT2/lithshared/incs -I$R/LT2/lithshared/butemgr -I$R/LT2/lithshared/stdlith \
 -I$R/LT2/lithshared/ltguimgr -I$R/LT2/lithshared/lithfontmgr -I$R/LT2/lithshared/lith \
 -I$R/LT2/lithshared/cryptmgr -I$R/LT2/lithshared/mfcstub \
 -I$R/LT2/lithshared/gamespy/gamespyclientmgr -I$R/engine/server -I$R/engine/shared -I$R/engine/skeleton -I$R/engine/generated \
 -I$R/engine/kernel/io -I$R/engine/kernel/record -I$R/engine/kernel/res \
 -I$R/engine/kernel/tex -I$R/engine/kernel/time -I$R/engine/render \
 -I$R/engine/platform -I$R/engine/sound -I$R/engine/model -I$R/engine/kernel/console -I$R/engine/kernel/math -I$R/engine/world"

Q="-iquote$R/NOLF/Shared -iquote$R/NOLF/ObjectDLL -iquote$R/NOLF/ClientShellDLL"

DEF="-DNO_PRAGMA_LIBS -D_NOLFBUILD -D_REZFILE -D_FINAL -DNDEBUG \
 -D_LITHTECH_ -D_LITHTECH2 -DSTRICT"

FLAGS="-std=c++17 -fdeclspec -Wno-everything -g -O1"

# ASan: NOLF_ASAN=1 ./build_skeleton.sh   (HANDOVER.md 4.6 -- reach for this
# early; a degenerate backtrace means stop inferring and let ASan name the PC.)
if [ "${NOLF_ASAN:-0}" = "1" ]; then
    FLAGS="$FLAGS -fsanitize=address -fno-omit-frame-pointer"
fi

CXX="${CXX:-clang++}"

# --- helpers ----------------------------------------------------------- //

fail=0

# ⚠️ Compiles run in the background, so their exit status cannot reach `fail`
# through a variable -- a subshell's assignment is invisible to the parent.
# Each failed compile touches this file instead. Without it the first symptom
# of a compile error is an undefined symbol at link time, which reads as an
# unrelated problem.
ERRFLAG="$O/.build_failed"
mkdir -p "$O"
rm -f "$ERRFLAG"

# Wait until fewer than $JOBS background compiles are running.
throttle() {
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do wait -n 2>/dev/null || break; done
}

# compile <src> <objdir> <extra-defines...>
compile() {
    local src="$1" objdir="$2"; shift 2
    local obj="$objdir/$(basename "${src%.*}").o"
    mkdir -p "$objdir"
    # Incremental: skip if the object is newer than the source. Deliberately
    # NOT a real dependency scan -- after any header change, run `clean`.
    if [ -f "$obj" ] && [ "$obj" -nt "$src" ]; then return 0; fi
    throttle
    ( if ! $CXX $FLAGS $DEF $SYS $Q "$@" -c "$src" -o "$obj" 2>"$obj.log"; then
          echo "COMPILE FAILED: $src"
          head -40 "$obj.log"
          rm -f "$obj"                 # never leave a stale object behind
          echo "$src" >> "$ERRFLAG"
      fi
      rm -f "$obj.log" ) &
}

# dsp_sources <dsp-path> -- the .dsp file list IS the source of truth.
# Six ObjectDLL .cpp files on disk are not in the build (HANDOVER.md 4.2);
# parsing the .dsp excludes them automatically rather than by a hand list.
dsp_sources() {
    grep "^SOURCE=" "$1" | sed 's/^SOURCE=//' | tr -d '\r' \
        | grep -i '\.cpp$' | tr '\\' '/'
}

section() { printf '\n--- %s ---\n' "$1"; }

# --- 1. engine support libraries --------------------------------------- //

section "engine libs"
# ⚠️ SUPPORT libraries go to their OWN directory, separate from the engine.
# ObjectDLL needs them (ButeMgr, RegMgr, CryptMgr, GameSpy, stdlith, mfcstub)
# and must NOT get the engine or the skeleton, which belong to the host. Before
# this split the only way to give the server its libraries was to give it
# everything, which pulled in CreateClientShell and the whole renderer.
for f in "$R"/engine/libs/*/*.cpp; do compile "$f" "$O/support"; done
compile "$R/LT2/sdk/inc/ltquatbase.cpp" "$O/libs"

# The flex/bison AI-animation parser. AnimationLex.cpp and AnimationParse.cpp
# are listed in Object.dsp but do not exist on disk -- they are generated.
# They go to $O/obj, not $O/libs: they are ObjectDLL's, and the client link
# below must not see them (AnimationLex.o calls into CAnimationParser).
for f in "$R"/engine/generated/*.cpp; do compile "$f" "$O/obj"; done

# The real (non-stub) engine subsystems, as they appear. Globbed rather than
# listed so a new subsystem directory needs no build change -- but note the
# depths differ: kernel/ groups by subsystem, render/ and friends do not.
for pat in "$R/engine/kernel/*/*.cpp" "$R/engine/render/*.cpp" \
           "$R/engine/world/*.cpp" "$R/engine/model/*.cpp" \
           "$R/engine/sound/*.cpp" "$R/engine/shared/*.cpp" \
           "$R/engine/server/*.cpp" "$R/engine/platform/*.cpp"; do
    if compgen -G "$pat" > /dev/null; then
        for f in $pat; do compile "$f" "$O/libs"; done
    fi
done

# ⚠️ Objective-C++, and the ONLY such file in the port. It needs -ObjC++ and
# links Cocoa; nothing else does. Compiled unconditionally so a build never
# silently loses the window, but the window itself is off unless LT_WINDOW is
# set (see engine/platform/ltwindow.h).
LINK_EXTRA=""
if [ -f "$R/engine/platform/ltwindow.mm" ]; then
    obj="$O/libs/ltwindow.o"
    mkdir -p "$O/libs"
    if [ ! -f "$obj" ] || [ "$R/engine/platform/ltwindow.mm" -nt "$obj" ]; then
        if ! $CXX $FLAGS -ObjC++ -fobjc-arc $SYS -c "$R/engine/platform/ltwindow.mm" \
             -o "$obj" 2>"$obj.log"; then
            echo "COMPILE FAILED: ltwindow.mm"; head -30 "$obj.log"
            rm -f "$obj"; echo "ltwindow.mm" >> "$ERRFLAG"
        fi
        rm -f "$obj.log"
    fi
    LINK_EXTRA="-framework Cocoa"
fi

# ⚠️ The Metal backend, compiled the same way and for the same reason: always
# built so a build cannot silently lose it, but inert unless LT_METAL is set.
# Its shaders are compiled at RUNTIME from a source string, so there is no
# .metal step here and no toolchain dependency beyond the frameworks.
if [ -f "$R/engine/render/ltmetal.mm" ]; then
    obj="$O/libs/ltmetal.o"
    mkdir -p "$O/libs"
    if [ ! -f "$obj" ] || [ "$R/engine/render/ltmetal.mm" -nt "$obj" ]; then
        if ! $CXX $FLAGS -ObjC++ -fobjc-arc $SYS -c "$R/engine/render/ltmetal.mm" \
             -o "$obj" 2>"$obj.log"; then
            echo "COMPILE FAILED: ltmetal.mm"; head -30 "$obj.log"
            rm -f "$obj"; echo "ltmetal.mm" >> "$ERRFLAG"
        fi
        rm -f "$obj.log"
    fi
    LINK_EXTRA="$LINK_EXTRA -framework Metal -framework QuartzCore"
fi

# ⚠️ The CoreAudio half of the sound system. Compiled the same way as the window
# and Metal backends, and for the same reason: ltsoundmgr.cpp calls into it for
# MP3, so a build that skipped it would link-fail rather than quietly lose sound.
if [ -f "$R/engine/sound/ltaudio.mm" ]; then
    obj="$O/libs/ltaudio.o"
    mkdir -p "$O/libs"
    if [ ! -f "$obj" ] || [ "$R/engine/sound/ltaudio.mm" -nt "$obj" ]; then
        if ! $CXX $FLAGS -ObjC++ -fobjc-arc $SYS -c "$R/engine/sound/ltaudio.mm" \
             -o "$obj" 2>"$obj.log"; then
            echo "COMPILE FAILED: ltaudio.mm"; head -30 "$obj.log"
            rm -f "$obj"; echo "ltaudio.mm" >> "$ERRFLAG"
        fi
        rm -f "$obj.log"
    fi
    LINK_EXTRA="$LINK_EXTRA -framework AudioToolbox -framework CoreFoundation"
fi

compile "$R/engine/skeleton/ltstub.cpp"            "$O/libs"
compile "$R/engine/skeleton/skeleton_generated.cpp" "$O/libs"
compile "$R/engine/skeleton/skeleton_host.cpp"      "$O/libs" -D_CLIENTBUILD

wait
$CXX $FLAGS -c "$R/engine/compat/windows_compat.c" -o "$O/compat/windows_compat.o" \
    -I"$R/engine/compat" 2>/dev/null || {
    mkdir -p "$O/compat"
    clang -std=c11 -Wno-everything -g -O1 -I"$R/engine/compat" \
        -c "$R/engine/compat/windows_compat.c" -o "$O/compat/windows_compat.o" || fail=1
}

# --- 2. ObjectDLL (server flavour) ------------------------------------- //
#
# NOTE the flavour split. Shared/ is compiled TWICE with different defines
# (HANDOVER.md 4.1): CDebrisMgr::CreateDebris and ~43 other symbols exist only
# in the _CLIENTBUILD flavour. Building one set and linking it into both looks
# like several unrelated link failures.

section "ObjectDLL + Shared (server flavour)"
while read -r f; do
    case "$f" in
        ./AnimationLex.cpp|./AnimationParse.cpp) continue ;;   # generated, above
        ../../lt2/*) continue ;;                               # ltquatbase, above
        ../Shared/*) compile "$R/NOLF/Shared/$(basename "$f")" "$O/shared" ;;
        *)           compile "$R/NOLF/ObjectDLL/$(basename "$f")" "$O/obj" ;;
    esac
done < <(dsp_sources "$R/NOLF/ObjectDLL/Object.dsp")
wait

# --- 3. ClientShellDLL (client flavour) -------------------------------- //

section "ClientShellDLL + Shared (client flavour)"
while read -r f; do
    case "$f" in
        ../Shared/*) compile "$R/NOLF/Shared/$(basename "$f")" "$O/shared_csh" -D_CLIENTBUILD ;;
        *)           compile "$R/NOLF/ClientShellDLL/$(basename "$f")" "$O/csh" -D_CLIENTBUILD ;;
    esac
done < <(dsp_sources "$R/NOLF/ClientShellDLL/ClientShellDLL.dsp")

# SharedMovement.cpp is in Object.dsp's Shared list but NOT in
# ClientShellDLL.dsp's -- yet the client flavour needs it to link. Kept
# explicitly so the asymmetry is visible rather than an accident of globbing.
compile "$R/NOLF/Shared/SharedMovement.cpp" "$O/shared_csh" -D_CLIENTBUILD
wait

# --- 4. link ----------------------------------------------------------- //
#
# ⚠️ THE CLIENT AND SERVER SIDES CANNOT SHARE ONE EXECUTABLE.
#
# Shared/ is compiled twice (see 2 above), so $O/shared and $O/shared_csh define
# the SAME symbols with different bodies. In the original game those are two
# DLLs; in one Mach-O they are duplicate-symbol errors. The skeleton host is
# therefore a CLIENT link: libs + compat + csh + shared_csh.
#
# ObjectDLL is still compiled above -- keeping "141/141 compile" a live test --
# it is simply not linked here. That is also why the two flex/bison parser
# objects, which are ObjectDLL's, never reach this link.

if [ -f "$ERRFLAG" ]; then
    echo
    echo "BUILD FAILED -- $(wc -l < "$ERRFLAG" | tr -d ' ') file(s) did not compile:"
    sed 's/^/    /' "$ERRFLAG"
    exit 1
fi

section "link (client side)"
$CXX $FLAGS -o "$OUT" \
    "$O"/libs/*.o "$O"/support/*.o "$O"/compat/*.o \
    "$O"/csh/*.o "$O"/shared_csh/*.o $LINK_EXTRA || fail=1

if [ "$fail" != "0" ]; then
    echo "BUILD FAILED"
    exit 1
fi

# --- 5. the SERVER shell, as a dynamic library -------------------------- //
#
# ⚠️ THIS IS HOW THE ORIGINAL WAS BUILT, AND IT IS WHY IT WORKS. NOLF ships an
# ObjectDLL and a ClientShellDLL: two modules, each with its own copy of
# Shared/. Trying to put both in one Mach-O is what made the server side
# unlinkable (§4.8) -- the duplicate symbols are not an accident to be worked
# around, they are the architecture.
#
# So the server side becomes a real dylib, exactly as it was a real DLL. It
# takes ObjectDLL + the SERVER flavour of Shared + the support libraries, and
# NOT the engine: engine symbols are resolved at load time from the host, which
# is what `-undefined dynamic_lookup` says.
#
# ⚠️ It exports GetServerShellFunctions / GetServerShellVersion (SETUP_SERVERSHELL
# in GameServerShell.cpp), which is the pair the host looks up.

section "link (server shell dylib)"
$CXX $FLAGS -dynamiclib -o "$SERVER_OUT" \
    "$O"/obj/*.o "$O"/shared/*.o "$O"/support/*.o \
    -undefined dynamic_lookup || fail=1

if [ "$fail" != "0" ]; then
    echo "BUILD FAILED (server dylib)"
    exit 1
fi

printf '\nbuilt %s\n' "$SERVER_OUT"
printf '\nbuilt %s\n' "$OUT"
printf 'objects: obj=%s csh=%s shared=%s shared_csh=%s libs=%s\n' \
    "$(ls "$O/obj" | wc -l | tr -d ' ')" \
    "$(ls "$O/csh" | wc -l | tr -d ' ')" \
    "$(ls "$O/shared" | wc -l | tr -d ' ')" \
    "$(ls "$O/shared_csh" | wc -l | tr -d ' ')" \
    "$(ls "$O/libs" | wc -l | tr -d ' ')"
