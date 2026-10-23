// ----------------------------------------------------------------------- //
//
// MODULE  : ltstub.cpp
//
// PURPOSE : Implementation of the engine-skeleton stub reporter. See ltstub.h
//           for why this exists and how it is meant to be read.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "ltstub.h"
#include "ltrecord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    const int kMaxSites = 512;

    const char* g_sites[kMaxSites];
    int         g_count   = 0;

    const char* g_impl[kMaxSites];
    int         g_implCount = 0;
    int         g_quiet   = -1;      // -1 = not yet read from the environment
    int         g_abortAt = -1;

    void ReadEnvOnce()
    {
        if (g_quiet >= 0)
            return;

        const char* q = getenv("LT_STUB_QUIET");
        g_quiet = (q && *q && *q != '0') ? 1 : 0;

        const char* a = getenv("LT_STUB_ABORT");
        g_abortAt = (a && *a) ? atoi(a) : 0;
    }
}

int LTStub_Report(const char* what)
{
    ReadEnvOnce();

    if (g_count < kMaxSites)
        g_sites[g_count] = what;
    ++g_count;

    // ⚠️ The ORDINAL is deliberately not recorded, only the name. Recording
    // "5 ILTCSBase::FormatString" would make every insertion earlier in the
    // sequence renumber everything after it, so one new call would show up as
    // a hundred changed lines and the diff would be unreadable exactly when it
    // matters most. Position in the file already carries the order.
    LTRecord_Event("stub", "%s", what ? what : "?");

    if (!g_quiet)
    {
        fprintf(stderr, "[LTSTUB %3d] %s\n", g_count, what ? what : "?");
        fflush(stderr);
    }

    // Deliberate hard stop, so a debugger can catch the caller of a specific
    // stub. Far more useful than reading it out of a log after the fact.
    if (g_abortAt > 0 && g_count == g_abortAt)
    {
        fprintf(stderr, "[LTSTUB] LT_STUB_ABORT=%d reached at '%s' -- aborting.\n",
                g_abortAt, what ? what : "?");
        fflush(stderr);
        abort();
    }

    return g_count;
}

int LTStub_Count()
{
    return g_count;
}

// ⚠️ SEPARATE TAG IN THE RECORD ("impl", not "stub"), because the frame
// contract is diffed by a person reading it. A finished entry point that still
// recorded itself as a stub would keep the lie in the one artefact this port
// treats as ground truth.
int LTStub_ReportImpl(const char* what)
{
    ReadEnvOnce();

    if (g_implCount < kMaxSites)
        g_impl[g_implCount] = what;
    ++g_implCount;

    LTRecord_Event("impl", "%s", what ? what : "?");

    // ⚠️ NOT PRINTED PER CALL even when not quiet. A stub line is news; an
    // implemented call being reached is the normal case, and 56 of them would
    // bury the handful of lines that matter.
    return g_implCount;
}

int LTStub_ImplCount()
{
    return g_implCount;
}

void LTStub_DumpCensus()
{
    fprintf(stderr, "\n===== engine stubs reached: %d =====\n", g_count);
    int n = (g_count < kMaxSites) ? g_count : kMaxSites;
    for (int i = 0; i < n; ++i)
        fprintf(stderr, "  %3d  %s\n", i + 1, g_sites[i] ? g_sites[i] : "?");
    if (g_count > kMaxSites)
        fprintf(stderr, "  ... and %d more (raise kMaxSites)\n", g_count - kMaxSites);
    fprintf(stderr, "====================================\n\n");

    // ⚠️ Printed AFTER the stub list and clearly separated, because the two
    // are read for opposite reasons: the list above is work outstanding, this
    // one is surface already covered.
    if (g_implCount)
    {
        int m = (g_implCount < kMaxSites) ? g_implCount : kMaxSites;
        fprintf(stderr, "===== implemented entry points reached: %d =====\n",
                g_implCount);
        for (int i = 0; i < m; ++i)
            fprintf(stderr, "  %3d  %s\n", i + 1, g_impl[i] ? g_impl[i] : "?");
        fprintf(stderr, "  (these are DONE -- not work outstanding)\n");
        fprintf(stderr, "===============================================\n\n");
    }
    fflush(stderr);
}
