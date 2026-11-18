#!/bin/bash
# NOLF1 Linux port -- play. Usage: ./play_linux.sh [WIDTHxHEIGHT]
#
# Hungarian text and subtitles are ON by default (launcher/hu.txt).
# NOLF_LANG=en ./play_linux.sh   -> original English
# NOLF_LANG_FILE=<file>          -> any other language file (id TAB text, UTF-8)
R="$(cd "$(dirname "$0")" && pwd)"
export NOLF_GAME_DIR="${NOLF_GAME_DIR:-$HOME/workspace/No One Lives Forever}"
if [ "${NOLF_LANG:-hu}" = "en" ]; then
    unset NOLF_LANG_FILE
else
    export NOLF_LANG_FILE="${NOLF_LANG_FILE:-$R/launcher/hu.txt}"
fi
export LT_WINDOW=1 LT_REALTIME=1 LT_STUB_QUIET=1   # OpenGL; LT_SOFTWARE=1 for the software renderer
export LT_SCREEN="${1:-1024x768}"
cd "$R/build-linux" && exec ./nolf1 "$@"
