#!/bin/bash
# ----------------------------------------------------------------------- //
#
# runrecord.sh -- the headless run recipe.
#
# Runs the skeleton host under fixed conditions and writes one run record.
# "Fixed conditions" is the whole job: a contract is only a contract if the two
# sides were produced the same way, so everything that could vary is pinned here
# rather than left to whatever the caller's shell happens to have set.
#
# USAGE
#   runrecord.sh <output-record-path>
#
# ENVIRONMENT (all optional)
#   NOLF_BIN        the host binary            (default /tmp/nolf_skeleton)
#   NOLF_GAME_DIR   colon-separated data dirs  (default: the host's own default)
#
# ----------------------------------------------------------------------- //

set -u

OUT="${1:-}"
if [ -z "$OUT" ]; then
    echo "usage: runrecord.sh <output-record-path>" >&2
    exit 2
fi

. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/nolfbin.sh"
BIN="${NOLF_BIN:-$NOLF_BIN_DEFAULT}"

if [ ! -x "$BIN" ]; then
    echo "runrecord.sh: no host binary at $BIN" >&2
    echo "  build it with: xcodebuild -project NOLF1.xcodeproj -scheme nolf1 (NOLF1_Xcode/NOLF1), then deploy to NOLF1/build" >&2
    exit 2
fi

# ⚠️ PIN EVERY GATE, INCLUDING THE ONES WE WANT OFF.
#
# Unsetting is not the same as leaving alone. If the caller has LT_TRACE_FILE
# exported, the run still records the same events -- but LT_STUB_ABORT would
# terminate the run early and produce a SHORT record that diffs as a real
# regression. A harness that inherits the operator's shell is a harness that
# reports different answers to different people.
unset LT_STUB_ABORT
unset LT_TRACE_FILE
export LT_STUB_QUIET=1          # the record is the output; stderr is noise here

# ⚠️ THE RECORD IS A NO-SERVER RUN, AND IT SAYS SO HERE.
#
# The reference was blessed from a headless run with no server in it. That used
# to be the engine's DEFAULT, so this file said nothing about it -- which meant
# the record silently depended on whether the operator had LT_SERVER exported
# in their own shell, exactly the failure the block above exists to prevent.
#
# The server is on by default now (a player pressing Start in the menus needs
# one), so the recording run has to ask for the old behaviour explicitly. Pin
# it rather than inherit it.
export LT_NOSERVER=1
unset LT_SERVER

# ⚠️ AND THE FIXED TIMESTEP, for the same reason: a recording made on the wall
# clock cannot be compared with anything, including itself.
export LT_REALTIME=0

# ⚠️ AND THE SIZE OF THE PICTURE. The engine's default is 1024x768 now; the
# blessed frames are 640x480 and a recording at another size compares with
# nothing.
export LT_SCREEN=640x480

# LT_RECORD must be absolute: the host is run from a scratch directory below.
case "$OUT" in
    /*) ;;
    *)  OUT="$PWD/$OUT" ;;
esac
export LT_RECORD="$OUT"
rm -f "$OUT"

# ⚠️ THE HOST WRITES TO ITS WORKING DIRECTORY.
#
# CGameClientShell::OnEngineInitialized creates a `Save` directory if one is
# absent (GameClientShell.cpp:1015). That is real game behaviour, not a harness
# artefact -- but it means the run has a side effect on wherever it is started
# from, and the first version of this script littered `harness/` with an empty
# `Save/`. Started from inside FULLGAME/ it would write into a real install.
#
# So the run happens in a throwaway directory. That also makes the run
# reproducible in a way it otherwise would not be: `DirExist("Save")` answers
# differently on the second run in the same place.
SCRATCH="$(mktemp -d "${TMPDIR:-/tmp}/nolf_run.XXXXXX")"
trap 'rm -rf "$SCRATCH"' EXIT

# stdout and stderr are deliberately discarded: they are the human-readable
# instruments, and mixing them into the machine-readable one is how the two
# start disagreeing.
( cd "$SCRATCH" && "$BIN" > /dev/null 2>&1 )
rc=$?

if [ ! -f "$OUT" ]; then
    echo "runrecord.sh: no record was written (host exit $rc)" >&2
    exit 1
fi

# ⚠️ An unterminated record means the run died partway. Without this check a
# crash halfway through produces a truncated record, and a diff against a
# reference would report it as "these lines are missing" -- true, but it buries
# the actual news, which is that the host stopped.
last="$(tail -1 "$OUT")"

case "$last" in
    "# end")
        ;;                                    # clean run
    "# end (crashed)")
        # The host died, but its crash handler terminated the record -- so the
        # crash is a LINE IN THE CONTRACT rather than a missing file. That is
        # deliberate: "the run reaches here and dies" is a fact worth gating on,
        # and it turns into a diff the moment the run gets further.
        echo "runrecord.sh: ⚠️  the host CRASHED (rc=$rc); the record captured it." >&2
        ;;
    *)
        echo "runrecord.sh: ⚠️  record is not terminated at all -- the host died" >&2
        echo "  before its crash handler ran, or was killed. (rc=$rc)" >&2
        exit 1
        ;;
esac

if [ "$rc" != "0" ] && [ "$last" = "# end" ]; then
    echo "runrecord.sh: ⚠️  host exited $rc (record is complete, but check why)" >&2
fi

exit 0
