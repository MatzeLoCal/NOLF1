#!/bin/bash
# NOLF1 Linux port -- play. Usage: ./play_linux.sh [WIDTHxHEIGHT]
#
# Hungarian text and subtitles are ON by default (launcher/hu.txt).
#   NOLF_LANG=en ./play_linux.sh   -> original English
#   NOLF_LANG_FILE=<file>          -> any other language file (id TAB text, UTF-8)
#
# Resolution and renderer: what was last chosen in Options > Display (the game
# saves it to ~/.local/share/NOLF1/autoexec.cfg). A WxH argument, LT_SCREEN or
# LT_SOFTWARE given by hand win over it.
#
# Every run is logged to ~/.local/share/NOLF1/utolso_futas.log (the one before
# it: utolso_futas.log.1) -- that is the file to look at after a crash.
R="$(cd "$(dirname "$0")" && pwd)"
CFG="$HOME/.local/share/NOLF1/autoexec.cfg"
cfgval() { [ -f "$CFG" ] && sed -n "s/^\"$1\" \"\([^\"]*\)\".*/\1/Ip" "$CFG" | tail -1; }

export NOLF_GAME_DIR="${NOLF_GAME_DIR:-$HOME/workspace/No One Lives Forever}"
if [ "${NOLF_LANG:-hu}" = "en" ]; then
    unset NOLF_LANG_FILE
else
    export NOLF_LANG_FILE="${NOLF_LANG_FILE:-$R/launcher/hu.txt}"
fi

# Resolution: argument > LT_SCREEN > saved in the game > 1024x768
if [ -n "${1:-}" ]; then
    export LT_SCREEN="$1"
elif [ -z "${LT_SCREEN:-}" ]; then
    W="$(cfgval screenwidth)"; H="$(cfgval screenheight)"
    if [ -n "$W" ] && [ -n "$H" ] && [ "$W" -ge 640 ] 2>/dev/null && [ "$H" -ge 480 ] 2>/dev/null; then
        export LT_SCREEN="${W}x${H}"
    else
        export LT_SCREEN="1024x768"
    fi
fi

# Renderer: OpenGL (default) or Software, as chosen in Options > Display.
if [ -z "${LT_SOFTWARE:-}" ] && [ "$(cfgval RenderDll)" = "Software" ]; then
    export LT_SOFTWARE=1
fi

export LT_WINDOW=1 LT_REALTIME=1 LT_STUB_QUIET=1

LOG="$HOME/.local/share/NOLF1/utolso_futas.log"
mkdir -p "$(dirname "$LOG")"
[ -f "$LOG" ] && mv -f "$LOG" "$LOG.1"
echo "NOLF1 $(date '+%F %T')  LT_SCREEN=$LT_SCREEN  renderer=$([ -n "${LT_SOFTWARE:-}" ] && echo Software || echo OpenGL)" > "$LOG"

cd "$R/build-linux" || exit 1
set -o pipefail
./nolf1 2>&1 | tee -a "$LOG"
RC=$?
[ $RC -ne 0 ] && echo "[play_linux.sh] the game exited with code $RC" | tee -a "$LOG"
exit $RC
