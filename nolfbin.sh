# ----------------------------------------------------------------------- //
#
# nolfbin.sh -- WHERE THE BINARY IS. Sourced, never run.
#
# ⚠️ THIS FILE EXISTS BECAUSE THERE WERE TWO ANSWERS AND THEY DISAGREED.
#
#   build_skeleton.sh wrote   /tmp/nolf_skeleton
#   build/play.sh ran         NOLF1/build/nolf1
#   the harness measured      /tmp/nolf_skeleton
#
# So a day of fixes was built, measured, and confirmed green -- against a
# binary the person at the keyboard was not running. Every "please try this"
# pointed at an executable a day old, and the reports that came back ("no
# control at all", "W is not recognised") were true of that binary and of
# nothing I had changed. Hours went into diagnosing an input system that was
# never in the build being tested.
#
# ⚠️ The failure mode is silent and self-confirming: the build succeeds, the
# gates pass, the measurements are real, and the game never changes. Nothing
# in any log says "you are running something else".
#
# ONE definition, shared by the build and by every harness script. If you add
# a script that runs the engine, source this -- do not re-derive the path.
#
# Sets NOLF_BIN_DEFAULT. Callers still honour an explicit NOLF_BIN override,
# which is how a scratch build in /tmp stays possible on purpose.
#
# ----------------------------------------------------------------------- //

# The port's code root, whichever directory sourced us.
_nolf_code_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# NOLF1/build -- the directory play.sh lives in and runs from, and where the
# Xcode products are DEPLOYED to (HANDOVER 5cm/5cn). Since 2026-10-18 the code
# lives in NOLF1_Xcode/NOLF1, beside NOLF1/, so it is ../../NOLF1/build from
# here -- the Xcode build itself lands in DerivedData, which is NOT what the
# person plays: build, gate the product (NOLF_BIN=...), deploy, gate again.
_nolf_play_dir="$(cd "$_nolf_code_root/../../NOLF1" 2>/dev/null && pwd)/build"

if [ -d "$_nolf_play_dir" ]; then
    NOLF_BIN_DEFAULT="$_nolf_play_dir/nolf1"
else
    # No install tree (a bare checkout, CI): a scratch binary is still useful.
    NOLF_BIN_DEFAULT="/tmp/nolf_skeleton"
fi

export NOLF_BIN_DEFAULT
