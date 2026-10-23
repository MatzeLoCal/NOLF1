// ----------------------------------------------------------------------- //
//
// MODULE  : ltstub.h
//
// PURPOSE : The reporting mechanism every engine-skeleton stub uses.
//
// HOW THIS IS MEANT TO BE USED
//
//   The skeleton exists to answer one question: what does NOLF 1 actually ask
//   the engine for, and in what order? Every stub records its first call, so a
//   run produces a census of the real engine surface the game needs -- rather
//   than the 275 methods the headers declare, most of which may never be
//   touched on a given path.
//
//   That census is the implementation order. It is measured from the game's own
//   behaviour instead of guessed from the header list.
//
// ⚠️ FIRST CALL ONLY, BY DESIGN
//
//   These sit on per-frame paths. Printing every call would bury the signal and
//   distort timing badly enough to change behaviour -- the NOLF 2 port lost time
//   to exactly that: a heavy trace made a run look like it had no audio at all.
//   So each site reports once, and a running tally is available on demand.
//
// CONTROLS
//   LT_STUB_QUIET=1   suppress the per-call lines (the tally still accrues)
//   LT_STUB_ABORT=<n> abort on the n'th distinct stub, to get a stack trace
//
// ----------------------------------------------------------------------- //

#ifndef __LT_STUB_H__
#define __LT_STUB_H__

#include "lithtech.h"

// ⚠️ A STUB IS NOT A CALL SITE, AND CONFLATING THEM COST THIS PORT A MONTH.
//
//   LTSTUB() only REPORTS -- it does not return, and the code after it runs.
//   Left inside a function that was later finished, the marker keeps filing the
//   function as missing. 56 of them did exactly that, including
//   `ILTClient::RenderCamera`, which is fully implemented, wired, and draws
//   every level this port renders. The census listed it as a stub for months
//   and HANDOVER §6 called it "the single call between here and seeing a
//   level" -- while it was already drawing them.
//
//   ⚠️ THE CENSUS IS THE TO-DO LIST. A to-do list that keeps finished work on
//   it is worse than no list, because the next person works from the top of it.
//
//   So there are two markers and they mean different things:
//
//     LTSTUB(name)  -- NOT IMPLEMENTED. The body does nothing useful and the
//                      caller gets a default. This belongs on the to-do list.
//     LTIMPL(name)  -- IMPLEMENTED, and the game reached it. Recorded because
//                      "which entry points does the game actually exercise?"
//                      is still worth answering; it is NOT work outstanding.
//
//   When you finish a stub, change its marker. That is the whole discipline.

// Records one stub call site. Returns the site's ordinal (1-based).
int  LTStub_Report(const char* what);

// Prints every stub that was reached, in the order first reached.
void LTStub_DumpCensus();

// How many distinct stubs have been reached.
int  LTStub_Count();

// Records one IMPLEMENTED entry point the game reached. Separate list,
// separate census section, never counted as a stub.
int  LTStub_ReportImpl(const char* what);
int  LTStub_ImplCount();

#define LTSTUB(what)                                    \
    do {                                                \
        static int _lt_seen = 0;                        \
        if (!_lt_seen) { _lt_seen = 1;                  \
            LTStub_Report(what); }                      \
    } while (0)

#define LTIMPL(what)                                    \
    do {                                                \
        static int _lt_impl_seen = 0;                   \
        if (!_lt_impl_seen) { _lt_impl_seen = 1;        \
            LTStub_ReportImpl(what); }                  \
    } while (0)

#endif // __LT_STUB_H__
