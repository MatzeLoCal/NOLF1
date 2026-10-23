// ----------------------------------------------------------------------- //
//
// MODULE  : ltrecord.cpp
//
// PURPOSE : The run record. See ltrecord.h for what it is and why it exists
//           before there are any frames to diff.
//
// ----------------------------------------------------------------------- //

#include "ltrecord.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

// The record's own format version. Bumped when the LINE FORMAT changes, which
// invalidates archived references. Adding a new event KIND does not require a
// bump -- the harness diffs whole lines, so a new kind simply shows up as an
// added line and the reference is re-blessed deliberately.
#define LT_RECORD_VERSION 1

namespace
{
    FILE* g_pFile    = 0;
    int   g_bInited  = 0;
}


void LTRecord_Init()
{
    if (g_bInited) return;
    g_bInited = 1;

    const char* pPath = getenv("LT_RECORD");
    if (!pPath || !pPath[0]) return;

    g_pFile = fopen(pPath, "wb");
    if (!g_pFile)
    {
        // Loud, because a silently-absent record makes the contract check pass
        // vacuously -- the worst possible failure mode for a gate.
        fprintf(stderr, "[LTRECORD] ⚠️  cannot write %s -- NO RECORD WILL BE PRODUCED\n",
                pPath);
        return;
    }

    // ⚠️ UNBUFFERED, DELIBERATELY.
    //
    // A crash is exactly when the record matters most, and it is exactly when
    // a buffered tail is lost -- so the file would go empty at the one moment
    // it had something to say. The cost is a write per event on a path that
    // only runs when LT_RECORD is set.
    setvbuf(g_pFile, 0, _IONBF, 0);

    fprintf(g_pFile, "# nolf1 run record v%d\n", LT_RECORD_VERSION);
}


int LTRecord_Enabled()
{
    return g_pFile != 0;
}


void LTRecord_Event(const char* pKind, const char* pFmt, ...)
{
    if (!g_pFile) return;

    char buf[1024];
    va_list args;
    va_start(args, pFmt);
    vsnprintf(buf, sizeof(buf), pFmt ? pFmt : "", args);
    va_end(args);

    // ⚠️ A newline inside an event would split one record into two lines and
    // desynchronise every later diff. Replaced rather than rejected: losing the
    // shape of one message is better than losing the whole contract.
    for (char* p = buf; *p; ++p)
        if (*p == '\n' || *p == '\r') *p = ' ';

    fprintf(g_pFile, "%-6s %s\n", pKind ? pKind : "?", buf);
}


void LTRecord_Close()
{
    if (!g_pFile) return;
    fprintf(g_pFile, "# end\n");
    fclose(g_pFile);
    g_pFile = 0;
}


// ⚠️ Called from a SIGNAL HANDLER, so write(2) and nothing else. fprintf is not
// async-signal-safe: taking stdio's lock in a handler that interrupted stdio
// deadlocks, which would turn a crash into a hang and lose the diagnostic
// twice over.
void LTRecord_NoteFatalAndClose(const char* pWhat)
{
    if (!g_pFile) return;

    int fd = fileno(g_pFile);
    if (fd < 0) return;

    // ⚠️ strlen, NEVER a hand-counted literal. The first version of this wrote
    // 18 bytes of "\n# end (crashed)\n" -- which is 17 characters plus the
    // terminating NUL. That one stray NUL made the whole record a BINARY file
    // as far as grep and diff were concerned, so the harness silently stopped
    // counting events and the contract self-test reported "0 lines differ"
    // while cheerfully passing. A gate that reads its own input as binary is
    // a gate that answers yes to everything.
    const char* pre  = "host   FATAL ";
    const char* post = "\n# end (crashed)\n";

    ssize_t ignored;
    ignored = write(fd, pre,   strlen(pre));
    ignored = write(fd, pWhat, strlen(pWhat));
    ignored = write(fd, post,  strlen(post));
    (void)ignored;
}


const char* LTRecord_BaseName(const char* pPath)
{
    if (!pPath) return "";

    const char* pSlash = strrchr(pPath, '/');
    if (pSlash) return pSlash + 1;

    // Also handle DOS separators, since game-supplied names use them.
    pSlash = strrchr(pPath, '\\');
    return pSlash ? pSlash + 1 : pPath;
}
