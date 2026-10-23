// ----------------------------------------------------------------------- //
//
// MODULE  : ltnullcursor.cpp
//
// PURPOSE : ILTCursor for the null renderer. See ltnullcursor.h -- in
//           particular the note about MAKEINTRESOURCE, which is why this file
//           never calls strlen on its argument without checking first.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltnullcursor.h"
#include "ltrecord.h"
#include "ltwindow.h"
#include "ltstub.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>


// ⚠️ Win32's IS_INTRESOURCE, reproduced because the compat header does not
// define it and this is the only place that needs it. Anything below 0x10000 is
// an ordinal, not an address -- see the header.
static bool IsIntResource(const char* p)
{
    return ((uintptr_t)p) < 0x10000;
}

// Renders a resource argument as text, WITHOUT dereferencing an ordinal.
static void DescribeResource(const char* pName, char* pOut, size_t nOut)
{
    if (!pName)
        snprintf(pOut, nOut, "(null)");
    else if (IsIntResource(pName))
        snprintf(pOut, nOut, "#%u", (unsigned)(uintptr_t)pName);
    else
        snprintf(pOut, nOut, "%s", pName);
}


// ----------------------------------------------------------------------- //
// One cursor.
// ----------------------------------------------------------------------- //

class CLTNullCursor::Inst : public ILTCursorInst
{
public:

    Inst() : m_pData(0), m_pNext(0) { m_Desc[0] = 0; }

    LTRESULT IsValid() override         { return LT_YES; }
    void     SetData(const void* p) override { m_pData = (void*)p; }
    void*    GetData() override         { return m_pData; }

    char     m_Desc[64];
    void*    m_pData;
    Inst*    m_pNext;
};


// ----------------------------------------------------------------------- //

CLTNullCursor::CLTNullCursor()
    : m_pCursors(0)
    , m_pCurrent(0)
    , m_Mode(CM_None)
    , m_nLoaded(0)
    , m_nRefreshes(0)
{
}

CLTNullCursor::~CLTNullCursor()
{
    while (m_pCursors)
    {
        Inst* pNext = m_pCursors->m_pNext;
        delete m_pCursors;
        m_pCursors = pNext;
    }
}


bool CLTNullCursor::Owns(HLTCURSOR h) const
{
    if (!h) return false;
    for (Inst* p = m_pCursors; p; p = p->m_pNext)
        if ((HLTCURSOR)p == h) return true;
    return false;
}


LTRESULT CLTNullCursor::SetCursorMode(CursorMode cMode)
{
    m_Mode = cMode;
    LTRecord_Event("cursor", "mode %d", (int)cMode);

    // ⚠️ THE GAME IS TELLING US WHETHER IT NEEDS A POINTER, and that is exactly
    // the question mouse look has to answer.
    //
    // CM_None means "I am flying the camera" -- capture the pointer and feed
    // the deltas to look. CM_Hardware means a menu, a folder, or the DIALOGUE
    // WINDOW is up and the player has to be able to CLICK it
    // (CInterfaceMgr::OnLButtonDown routes a click to m_MainWnd, which is how
    // a subtitle window is dismissed and how a cutscene advances).
    //
    // ⚠️ CAPTURING UNCONDITIONALLY IN GAME MODE -- which is what this port did
    // for one build -- hides the pointer over the dialogue window and makes the
    // cutscene impossible to advance. Driving capture from the game's own
    // request means the two can never disagree.
    LTWindow_SetMouseLook(cMode == CM_None);
    return LT_OK;
}

LTRESULT CLTNullCursor::GetCursorMode(CursorMode& cMode)
{
    // "Always returns LT_OK and always fills in cMode" -- iltcursor.h:35.
    cMode = m_Mode;
    return LT_OK;
}

LTRESULT CLTNullCursor::IsCursorModeAvailable(CursorMode cMode)
{
    // ⚠️ LT_NO for CM_Hardware, and that is a deliberate answer rather than a
    // copy of the SDK comment's "just return LT_YES for now". There is no
    // hardware cursor here: claiming one would be state answered with a guess.
    // The game reads this through UseHardwareCursor and has a software path.
    return (cMode == CM_None) ? LT_YES : LT_NO;
}


LTRESULT CLTNullCursor::LoadCursorBitmapResource(const char* pName, HLTCURSOR& hCursor)
{
    LTIMPL("ILTCursor::LoadCursorBitmapResource");

    hCursor = 0;

    Inst* p = new Inst;
    DescribeResource(pName, p->m_Desc, sizeof(p->m_Desc));
    p->m_pNext = m_pCursors;
    m_pCursors = p;
    ++m_nLoaded;

    hCursor = (HLTCURSOR)p;

    // ⚠️ NO PIXELS ARE LOADED, and the record says so rather than implying a
    // bitmap arrived. The resource itself lives in ClientRes (FORMATS.md §6),
    // which is not readable yet; what this returns is a valid, distinct,
    // trackable handle, which is all the game does with it.
    LTRecord_Event("cursor", "load %s (no bitmap)", p->m_Desc);
    return LT_OK;
}

LTRESULT CLTNullCursor::FreeCursor(const HLTCURSOR hCursor)
{
    if (!Owns(hCursor)) return LT_ERROR;

    Inst* pFound = (Inst*)hCursor;
    if (m_pCurrent == pFound) m_pCurrent = 0;

    Inst** pp = &m_pCursors;
    while (*pp && *pp != pFound) pp = &(*pp)->m_pNext;
    if (!*pp) return LT_ERROR;

    *pp = pFound->m_pNext;
    delete pFound;
    return LT_OK;
}

LTRESULT CLTNullCursor::SetCursor(HLTCURSOR hCursor)
{
    // A NULL cursor means "no cursor", which is legal.
    if (hCursor && !Owns(hCursor)) return LT_ERROR;

    m_pCurrent = (Inst*)hCursor;
    LTRecord_Event("cursor", "set %s",
                   m_pCurrent ? m_pCurrent->m_Desc : "(none)");
    return LT_OK;
}

LTRESULT CLTNullCursor::IsValidCursor(HLTCURSOR hCursor)
{
    return Owns(hCursor) ? LT_YES : LT_NO;
}

LTRESULT CLTNullCursor::RefreshCursor()
{
    // The one genuinely display-side call: a declared no-op, counted.
    ++m_nRefreshes;
    return LT_OK;
}


void CLTNullCursor::DumpCensus() const
{
    fprintf(stderr, "  cursors   : %u loaded, current=%s, mode=%d, %u refresh(es)\n",
            m_nLoaded,
            m_pCurrent ? m_pCurrent->m_Desc : "(none)",
            (int)m_Mode, m_nRefreshes);
}

void CLTNullCursor::RecordTotals() const
{
    LTRecord_Event("count", "cursors-loaded %u", m_nLoaded);
    LTRecord_Event("count", "cursor-refreshes %u", m_nRefreshes);
}


static CLTNullCursor g_NullCursor;

CLTNullCursor* LTGetNullCursor()
{
    return &g_NullCursor;
}
