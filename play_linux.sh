#!/bin/bash
# NOLF1 Linux port -- play. Usage: ./play_linux.sh [WIDTHxHEIGHT]
R="$(cd "$(dirname "$0")" && pwd)"
export NOLF_GAME_DIR="${NOLF_GAME_DIR:-$HOME/workspace/No One Lives Forever}"
export LT_WINDOW=1 LT_REALTIME=1 LT_STUB_QUIET=1   # OpenGL; LT_SOFTWARE=1 for the software renderer
export LT_SCREEN="${1:-1024x768}"
cd "$R/build-linux" && exec ./nolf1 "$@"
