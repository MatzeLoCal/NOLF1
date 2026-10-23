// ----------------------------------------------------------------------- //
//
// MODULE  : ltnullcursor.h
//
// PURPOSE : ILTCursor for the null renderer.
//
// WHY A CURSOR BELONGS IN A RENDERER THAT DRAWS NOTHING
//
//   ILTCursor is not a drawing interface. Everything the game asks it is a
//   question about STATE -- which cursor is loaded, which is current, is this
//   handle still valid, what mode are we in. None of that needs pixels, so all
//   of it can be answered truthfully, and by ltnullrender.h's rule it therefore
//   MUST be. Only the act of putting the cursor on screen is the no-op.
//
//   This is the first thing the interface manager does
//   (CInterfaceMgr::InitCursor, InterfaceMgr.cpp:466), and it gives up
//   immediately if the cursor will not load:
//
//       ERROR in CInterfaceMgr::Init():  Could not initialize Cursor!
//
// ⚠️ THE ARGUMENT IS NOT ALWAYS A STRING
//
//   LoadCursorBitmapResource takes `const char *pName`, and the game calls it
//   as `MAKEINTRESOURCE(IDC_POINTER)` -- which compat/windows.h:603 expands to
//   `(const char*)(uintptr_t)(WORD)104`. That is the integer 104 wearing a
//   pointer's clothes, and strlen() on it is a wild read of address 0x68.
//
//   Win32 resolves this with IS_INTRESOURCE: a "pointer" below 0x10000 is an
//   ordinal, not an address. Anything taking a resource name has to make the
//   same distinction, and getting it wrong is a crash whose stack trace points
//   at string handling rather than at the caller.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_NULLCURSOR_H__
#define __LT_NULLCURSOR_H__

#include "iltcursor.h"

class CLTNullCursor : public ILTCursor
{
public:

    CLTNullCursor();

    // ⚠️ NOT `override`: ILTCursor has no virtual destructor, so there is
    // nothing to override and marking it is an error. This object is a static
    // singleton and is never deleted through an ILTCursor*, which is the only
    // situation the missing virtual destructor would make unsafe.
    ~CLTNullCursor();

    LTRESULT    SetCursorMode(CursorMode cMode) override;
    LTRESULT    GetCursorMode(CursorMode& cMode) override;
    LTRESULT    IsCursorModeAvailable(CursorMode cMode) override;

    LTRESULT    LoadCursorBitmapResource(const char* pName, HLTCURSOR& hCursor) override;
    LTRESULT    FreeCursor(const HLTCURSOR hCursor) override;

    LTRESULT    SetCursor(HLTCURSOR hCursor) override;
    LTRESULT    IsValidCursor(HLTCURSOR hCursor) override;
    LTRESULT    RefreshCursor() override;

    void        DumpCensus() const;
    void        RecordTotals() const;

private:

    class Inst;

    bool        Owns(HLTCURSOR h) const;

    Inst*       m_pCursors;         // singly-linked, newest first
    Inst*       m_pCurrent;
    CursorMode  m_Mode;
    unsigned    m_nLoaded;
    unsigned    m_nRefreshes;
};

CLTNullCursor* LTGetNullCursor();

#endif // __LT_NULLCURSOR_H__
