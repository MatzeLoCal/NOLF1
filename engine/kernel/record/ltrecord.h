// ----------------------------------------------------------------------- //
//
// MODULE  : ltrecord.h
//
// PURPOSE : The run record -- an ordered, deterministic, diffable log of
//           everything the game asked the engine for during one run.
//
//           This is the frame contract, in the only form NOLF 1 can currently
//           support.
//
// WHY THIS EXISTS BEFORE THERE ARE ANY FRAMES
//
//   PORT_PLAN.md §4 and the background doc §3.1 both say the same thing, in
//   capitals: build the harness FIRST, not after the renderer works. The NOLF 2
//   port's contract is four archived PNGs at 0.00 mean absolute difference, and
//   it is the reason a GL→Metal conversion was survivable at all.
//
//   NOLF 1 has nothing renderable, so there is nothing to photograph. But the
//   *shape* of a frame contract is not "pixels" -- it is:
//
//       a deterministic artefact of a known-good run, archived, and diffed
//       automatically on every change, where any drift is a failure until
//       someone says otherwise.
//
//   The deterministic artefact available today is the ORDERED SEQUENCE OF
//   ENGINE CALLS AND FILE ACCESSES. That is a real contract with real teeth:
//   it is exactly what regressed silently while OpenFile was being implemented
//   (four DebugOut calls became four census entries), and exactly the kind of
//   drift no one would notice by reading output.
//
//   When the null renderer lands, draw calls join the same stream and the
//   contract tightens without changing shape. When real frames land,
//   framediff.py takes over for pixels and this keeps covering everything that
//   is not pixels.
//
// ⚠️ DETERMINISM IS THE WHOLE VALUE, AND IT IS EASY TO LOSE
//
//   A record containing a timestamp, a pointer, a PID, an absolute path or a
//   hash-table iteration order is not a contract -- it is a random number
//   generator that fails every run and gets switched off within a week. Nothing
//   written through here may contain any of those. In particular:
//
//     * paths are recorded as BASENAMES, never as the caller's absolute path;
//     * no address, handle value or allocation size is ever recorded;
//     * counts are recorded, times are not.
//
//   harness/selftest.sh verifies this by running the host repeatedly and
//   diffing the records against each other. An instrument that is not checked
//   is an instrument that lies (background doc §3.5).
//
// USAGE
//   LT_RECORD=<path>   write the run record to <path>. Unset = disabled, and
//                      the cost is then one comparison per event.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_RECORD_H__
#define __LT_RECORD_H__

// Reads LT_RECORD and opens the file if it is set. Safe to call more than once;
// only the first call does anything.
void LTRecord_Init();

// True if a record is being written. Check this before doing work that only
// exists to produce a record.
int  LTRecord_Enabled();

// Appends one event. pKind is a short tag ("stub", "file", "call", ...) and
// becomes the line's first column, so the record stays greppable and the
// harness can diff one kind at a time.
//
// ⚠️ Nothing passed here may vary between two runs of the same build.
void LTRecord_Event(const char* pKind, const char* pFmt, ...);

// Flushes and closes. Called at the end of a run; harmless if never opened.
void LTRecord_Close();

// Appends a fatal-signal marker and a terminator, using only async-signal-safe
// calls. ⚠️ Intended for a SIGSEGV/SIGBUS/SIGABRT handler.
//
// A crashed run that leaves NO record is the worst outcome for the contract
// harness: the gate cannot distinguish "died" from "never ran", and the record
// is silent about the one event that mattered. With this, a crash is a LINE IN
// THE CONTRACT, and moving it is a diff.
void LTRecord_NoteFatalAndClose(const char* pWhat);

// Strips a directory prefix, so a path can be recorded without embedding the
// machine it was run on. Returns a pointer into pPath.
const char* LTRecord_BaseName(const char* pPath);

#endif // __LT_RECORD_H__
