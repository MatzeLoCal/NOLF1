// ----------------------------------------------------------------------- //
//
// MODULE  : ltgui_controls.cpp
//
// PURPOSE : The derived LTGUIMgr controls, declared in LT2/lithshared/ltguimgr/.
//
//   CLTGUICtrl
//     +-- CLTGUIFadeColorCtrl        selection-fade colour
//           +-- CLTGUITextItemCtrl   a list of strings, one selected
//           |     +-- CLTGUICycleCtrl      header + cycling value
//           |           +-- CLTGUIOnOffCtrl      two-state cycle
//           +-- CLTGUIColumnTextCtrl  columns of text
//           +-- CLTGUIEditCtrl        text entry
//
// See ltgui_base.cpp for why this exists and what is real here. In short: the
// STATE is real, the DRAWING is not, because every draw path ends at the engine
// surface API which is still the announcing skeleton.
//
// ⚠️ HSTRINGs are ENGINE-OWNED. The controls store handles the engine hands
// them; with the skeleton in place those are null. Every path here therefore
// tolerates a null handle rather than assuming a live string -- otherwise the
// first menu construction dereferences null.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iltclient.h"
#include "ltguicommandhandler.h"
#include "ltguifont.h"
#include "ltguictrl.h"
#include "ltguifadecolorctrl.h"
#include "ltguitextitemctrl.h"
#include "ltguicyclectrl.h"
#include "ltguionoffctrl.h"
#include "ltguicolumntextctrl.h"
#include "ltguieditctrl.h"

static void ltgui_NeedsRenderer2(const char* what)
{
    static bool s_reported = false;
    if (s_reported) return;
    s_reported = true;
    fprintf(stderr, "\n[LTGUI] %s needs the engine surface API (still skeleton);"
                    " control state is live, nothing is drawn.\n\n", what);
    fflush(stderr);
}

// ----------------------------------------------------------------------- //
// CLTGUIFont -- the declared virtual destructor (its vtable needs a definition)
// ----------------------------------------------------------------------- //

CLTGUIFont::~CLTGUIFont()
{
    Term();
}

// ----------------------------------------------------------------------- //
// CLTGUIFadeColorCtrl
//
// Holds the three state colours and the fade timer. GetCurrentColor is the
// only piece with real logic, and it is pure arithmetic -- no engine needed.
// ----------------------------------------------------------------------- //

CLTGUIFadeColorCtrl::CLTGUIFadeColorCtrl()
{
    m_dwFadeTime    = 0;
    m_selColor      = SETRGB(255, 255, 255);
    m_nonSelColor   = SETRGB(255, 255, 255);
    m_disabledColor = SETRGB(128, 128, 128);
    m_fStartTime    = 0.0f;
}

CLTGUIFadeColorCtrl::~CLTGUIFadeColorCtrl()
{
}

void CLTGUIFadeColorCtrl::SetColor(HLTCOLOR selColor, HLTCOLOR nonSelColor,
                                   HLTCOLOR disabledColor)
{
    m_selColor      = selColor;
    m_nonSelColor   = nonSelColor;
    m_disabledColor = disabledColor;
}

void CLTGUIFadeColorCtrl::StartFade()
{
    m_fStartTime = 0.0f;    // a real clock arrives with the engine
}

void CLTGUIFadeColorCtrl::StopFade()
{
    m_fStartTime = 0.0f;
}

HLTCOLOR CLTGUIFadeColorCtrl::GetCurrentColor()
{
    if (!IsEnabled())
        return m_disabledColor;
    return IsSelected() ? m_selColor : m_nonSelColor;
}

// ----------------------------------------------------------------------- //
// CLTGUITextItemCtrl -- a list of strings with one selected.
// ----------------------------------------------------------------------- //

CLTGUITextItemCtrl::CLTGUITextItemCtrl()
{
    m_pClientDE   = 0;
    m_pFont       = 0;
    m_pnValue     = 0;
    m_dwWidth     = 0;
    m_dwHeight    = 0;
    m_bFixedWidth = LTFALSE;
    m_nSelIndex   = 0;
}

CLTGUITextItemCtrl::~CLTGUITextItemCtrl()
{
    Destroy();
}

LTBOOL CLTGUITextItemCtrl::Create(ILTClient* pClientDE, uint32 dwCommandID, HSTRING hText,
                                  CLTGUIFont* pFont, CLTGUICommandHandler* pCommandHandler,
                                  int* pnValue, uint32 dwParam1, uint32 dwParam2)
{
    if (!pClientDE || !pFont)
        return LTFALSE;

    m_pClientDE      = pClientDE;
    m_pFont          = pFont;
    m_pnValue        = pnValue;
    m_dwCommandID    = dwCommandID;
    m_dwParam1       = dwParam1;
    m_dwParam2       = dwParam2;
    m_pCommandHandler = pCommandHandler;

    if (hText)
        AddString(hText);

    m_bCreated = LTTRUE;
    CalculateSize();
    return LTTRUE;
}

void CLTGUITextItemCtrl::Destroy()
{
    RemoveAll();
    m_pFont     = 0;
    m_pClientDE = 0;
    m_bCreated  = LTFALSE;
}

// ⚠️ STORING AN HSTRING MEANS TAKING A REFERENCE. THIS WAS A USE-AFTER-FREE.
//
// The game hands a string over and then drops its OWN reference:
//
//     HSTRING hStr = g_pLTClient->FormatString(stringID);
//     pCtrl = CreateTextItem(hStr, ...);   // control stores it
//     g_pLTClient->FreeString(hStr);       // BaseFolder.cpp:1402
//
// which is correct Win32-LithTech usage -- iltcsbase.h:161 says CopyString
// increments the count -- and only works if the storer takes a reference.
//
// Storing the raw handle dropped the count to zero, the string object was
// deleted, and the next `new` reused the same heap block. Every stale handle
// then resolved to a DIFFERENT, LIVE string, so all four difficulty items drew
// the same text -- whichever string happened to land at that address.
//
// ⚠️ The handle validation could not catch this: the address really was live
// again. Ownership is the fix; a liveness check is not.
void CLTGUITextItemCtrl::AddString(HSTRING hString)
{
    if (hString && m_pClientDE)
        hString = m_pClientDE->CopyString(hString);

    m_stringArray.Add(hString);
    CalculateSize();
}

void CLTGUITextItemCtrl::AddString(int messageCode)
{
    HSTRING h = m_pClientDE ? m_pClientDE->FormatString(messageCode) : 0;
    AddString(h);
}

HSTRING CLTGUITextItemCtrl::GetString(int nIndex)
{
    if (nIndex < 0 || (uint32)nIndex >= m_stringArray.GetSize())
        return 0;
    return m_stringArray[nIndex];
}

void CLTGUITextItemCtrl::RemoveAll()
{
    if (m_pClientDE)
    {
        for (uint32 i = 0; i < m_stringArray.GetSize(); ++i)
            if (m_stringArray[i])
                m_pClientDE->FreeString(m_stringArray[i]);
    }
    m_stringArray.SetSize(0);
    m_nSelIndex = 0;
}

void CLTGUITextItemCtrl::SetSelIndex(int nIndex)
{
    int n = (int)m_stringArray.GetSize();
    if (n <= 0) { m_nSelIndex = 0; return; }

    // Wrap, which is what makes left/right cycle through the options.
    if (nIndex < 0)   nIndex = n - 1;
    if (nIndex >= n)  nIndex = 0;

    m_nSelIndex = nIndex;
    OnSelChange();
}

LTBOOL CLTGUITextItemCtrl::OnLeft()
{
    if (m_stringArray.GetSize() < 2) return LTFALSE;
    SetSelIndex(m_nSelIndex - 1);
    return LTTRUE;
}

LTBOOL CLTGUITextItemCtrl::OnRight()
{
    if (m_stringArray.GetSize() < 2) return LTFALSE;
    SetSelIndex(m_nSelIndex + 1);
    return LTTRUE;
}

void CLTGUITextItemCtrl::OnSelChange()
{
    // Push the new selection out to the bound variable immediately, so a caller
    // reading it without an explicit UpdateData still sees the truth.
    if (m_pnValue)
        *m_pnValue = m_nSelIndex;
}

void CLTGUITextItemCtrl::UpdateData(LTBOOL bSaveAndValidate)
{
    if (!m_pnValue)
        return;

    if (bSaveAndValidate) *m_pnValue = m_nSelIndex;
    else                  SetSelIndex(*m_pnValue);
}

void CLTGUITextItemCtrl::CalculateSize()
{
    m_dwHeight = m_pFont ? (uint32)m_pFont->GetHeight() : 0;

    if (m_bFixedWidth)
        return;

    uint32 dwMax = 0;
    for (uint32 i = 0; i < m_stringArray.GetSize(); ++i)
    {
        if (!m_pFont || !m_stringArray[i])
            continue;
        LTIntPt ext = m_pFont->GetTextExtents(m_stringArray[i]);
        if ((uint32)ext.x > dwMax) dwMax = (uint32)ext.x;
    }
    m_dwWidth = dwMax;
}

// ⚠️ THE CONTROLS DRAW THROUGH CLTGUIFont, NOT THROUGH THE COMPOSITOR.
//
// Every Render below is a position, a colour and a string handed to the font.
// Keeping the pixel work on the other side of that boundary is what let the
// blitter and the font each be tested on their own; a control that reached for
// LT2D directly would be untestable without a whole GUI tree.
//
// The colour comes from GetCurrentColor(), which is the fade state machine that
// was already correct and already exercised -- selected items are brighter, and
// that now actually shows.

// ⚠️ A DISABLED CONTROL IS STILL DRAWN -- in its disabled colour. Disabled is
// how the menus mark INFORMATION rather than a choice: the mission status and
// summary screens build every line (mission name, "Objectives", each
// objective, the stats) with Enable(LTFALSE) (FolderMission.cpp:43-52).
// Returning early here left those screens empty but for the checkboxes
// (2026-10-06). The original renders unless INVISIBLE (NOLF2 ltguitextitemctrl
// .cpp:168; LT2's controls have no visible flag) and lets GetCurrentColor pick
// m_disabledColor. Same for the cycle, column and edit controls below.
void CLTGUITextItemCtrl::Render(HSURFACE hDestSurf)
{
    if (!m_pFont) return;
    if (m_nSelIndex < 0 || (uint32)m_nSelIndex >= m_stringArray.GetSize()) return;

    HSTRING hStr = m_stringArray[m_nSelIndex];
    if (!hStr) return;

    LTIntPt pos = GetPos();

    if (m_bFixedWidth && m_dwWidth > 0)
        m_pFont->DrawFormat(hStr, hDestSurf, pos.x, pos.y, (int)m_dwWidth, GetCurrentColor());
    else
        m_pFont->Draw(hStr, hDestSurf, pos.x, pos.y, 0, GetCurrentColor());
}

// ----------------------------------------------------------------------- //
// CLTGUICycleCtrl -- a header plus a cycling value.
// ----------------------------------------------------------------------- //

// ⚠️ A STRING HANDED TO A CONTROL IS COPIED, because the game frees its own
// the moment the call returns: CBaseFolder::CreateCycleItem / CreateToggle /
// CreateEditCtrl (BaseFolder.cpp:1537, 1581, 1493), FolderSave's columns
// (FolderSave.cpp:247-250), FolderCustomControls' SetString (:210, :506).
// Kept as given, the control drew whatever string the manager put in that
// handle next -- Display Options labelled every row "32-bit" or "On" -- and
// its destructor freed someone else's string (user report 2026-10-17).
// AddString already copied. LT_GUINOCOPY=1 = keep the caller's handle (old).
static HSTRING LTGUI_Own(ILTClient* pClientDE, HSTRING h)
{
    static const bool s_bOld = []() {
        const bool b = (getenv("LT_GUINOCOPY") != 0);
        if (b) { printf("[GUI] LT_GUINOCOPY=1 -- controls keep the caller's string handles (old)\n"); fflush(stdout); }
        return b; }();
    if (s_bOld || !pClientDE || !h) return h;
    return pClientDE->CopyString(h);
}

CLTGUICycleCtrl::CLTGUICycleCtrl()
{
    m_hHeaderText  = 0;
    m_nHeaderWidth = 0;
    m_nSpacerWidth = 0;
    m_nAlign       = 0;
    m_bInUpdate    = LTFALSE;
}

CLTGUICycleCtrl::~CLTGUICycleCtrl()
{
    if (m_pClientDE && m_hHeaderText)
        m_pClientDE->FreeString(m_hHeaderText);
    m_hHeaderText = 0;
}

LTBOOL CLTGUICycleCtrl::Create(ILTClient* pClientDE, HSTRING hText, CLTGUIFont* pFont,
                               int nHeaderWidth, int nSpacerWidth, int* pnValue, int alignment)
{
    if (!pClientDE || !pFont)
        return LTFALSE;

    m_pClientDE    = pClientDE;
    m_pFont        = pFont;
    m_pnValue      = pnValue;
    if (m_hHeaderText) m_pClientDE->FreeString(m_hHeaderText);
    m_hHeaderText  = LTGUI_Own(pClientDE, hText);
    m_nHeaderWidth = nHeaderWidth;
    m_nSpacerWidth = nSpacerWidth;
    m_nAlign       = alignment;

    m_bCreated = LTTRUE;
    CalculateSize();
    return LTTRUE;
}

void CLTGUICycleCtrl::NotifyOnChange(uint32 dwCommandID, CLTGUICommandHandler* pCommandHandler,
                                     uint32 dwParam1, uint32 dwParam2)
{
    m_dwCommandID     = dwCommandID;
    m_pCommandHandler = pCommandHandler;
    m_dwParam1        = dwParam1;
    m_dwParam2        = dwParam2;
}

void CLTGUICycleCtrl::SetSelIndex(int nIndex)
{
    CLTGUITextItemCtrl::SetSelIndex(nIndex);

    // ⚠️ m_bInUpdate guards against re-entry: the change notification can run
    // code that calls UpdateData, which would set the index again.
    if (m_bInUpdate || !m_pCommandHandler || m_dwCommandID == 0)
        return;

    m_bInUpdate = LTTRUE;
    // SendCommand() is the SDK's public wrapper for the protected OnCommand();
    // it is how the game's own controls dispatch (see BitmapCtrl.cpp).
    m_pCommandHandler->SendCommand(m_dwCommandID, m_dwParam1, m_dwParam2);
    m_bInUpdate = LTFALSE;
}

void CLTGUICycleCtrl::UpdateData(LTBOOL bSaveAndValidate)
{
    if (m_bInUpdate)
        return;
    CLTGUITextItemCtrl::UpdateData(bSaveAndValidate);
}

void CLTGUICycleCtrl::CalculateSize()
{
    CLTGUITextItemCtrl::CalculateSize();
    m_dwWidth = (uint32)(m_nHeaderWidth + m_nSpacerWidth) + m_dwWidth;
}

void CLTGUICycleCtrl::Render(HSURFACE hDestSurf)
{
    // ⚠️ CLTGUICycleCtrl DERIVES from CLTGUITextItemCtrl, so the value text is
    // the base class's current string. Only the header is its own.
    if (!m_pFont) return;

    LTIntPt pos = GetPos();
    HLTCOLOR col = GetCurrentColor();

    if (m_hHeaderText)
        m_pFont->Draw(m_hHeaderText, hDestSurf, pos.x, pos.y, m_nAlign, col);

    // ⚠️ THE VALUE STARTS AFTER THE HEADER COLUMN -- nHeaderWidth is "the
    // width to use for the header string" and nSpacerWidth "the space between
    // the header and text" (ltguicyclectrl.h; the later LTGUI draws the value
    // at m_pos.x + m_nHeaderWidth, ltguicyclectrl.cpp:126). Drawn at pos.x,
    // label and value overlapped on every cycle and on/off row: "Resolution"
    // under "640x480x32" in Display Options (user report 2026-10-17).
    // CalculateSize already counted header + spacer. LT_GUINOVALUEOFFSET=1 =
    // the overlapping drawing (says so).
    static const bool s_bNoOffset = []() {
        const bool b = (getenv("LT_GUINOVALUEOFFSET") != 0);
        if (b) { printf("[GUI] LT_GUINOVALUEOFFSET=1 -- cycle values drawn over their labels (old)\n"); fflush(stdout); }
        return b; }();
    if (s_bNoOffset) { CLTGUITextItemCtrl::Render(hDestSurf); return; }

    if (m_nSelIndex < 0 || (uint32)m_nSelIndex >= m_stringArray.GetSize()) return;
    HSTRING hStr = m_stringArray[m_nSelIndex];
    if (!hStr) return;
    const int nOffset = m_nHeaderWidth + m_nSpacerWidth;
    if (m_bFixedWidth && (int)m_dwWidth > nOffset)
        m_pFont->DrawFormat(hStr, hDestSurf, pos.x + nOffset, pos.y, (int)m_dwWidth - nOffset, col);
    else
        m_pFont->Draw(hStr, hDestSurf, pos.x + nOffset, pos.y, 0, col);
}

// ----------------------------------------------------------------------- //
// CLTGUIOnOffCtrl -- a two-entry cycle bound to an LTBOOL.
// ----------------------------------------------------------------------- //

CLTGUIOnOffCtrl::CLTGUIOnOffCtrl()
{
    m_bOn      = LTFALSE;
    m_hStrOn   = 0;
    m_hStrOff  = 0;
    m_pbValue  = 0;
}

CLTGUIOnOffCtrl::~CLTGUIOnOffCtrl()
{
    if (m_pClientDE)
    {
        if (m_hStrOn)  m_pClientDE->FreeString(m_hStrOn);
        if (m_hStrOff) m_pClientDE->FreeString(m_hStrOff);
    }
    m_hStrOn = m_hStrOff = 0;
}

LTBOOL CLTGUIOnOffCtrl::Create(ILTClient* pClientDE, HSTRING hString, CLTGUIFont* pFont,
                               int nRightColumnOffset, LTBOOL* pbValue, int alignment)
{
    if (!CLTGUICycleCtrl::Create(pClientDE, hString, pFont, nRightColumnOffset, 0, 0, alignment))
        return LTFALSE;

    m_pbValue = pbValue;
    return LTTRUE;
}

// ⚠️ THE ON/OFF TEXT IS THE CYCLE'S OWN VALUE. LT2's on/off control declares
// no Render (ltguionoffctrl.h): it is drawn by CLTGUICycleCtrl::Render from
// the string list, so the two strings have to BE that list -- entry 0 "off",
// entry 1 "on" -- and SetOn has to select one. They were kept aside where
// nothing drew them; the "32-bit" / "On" a toggle seemed to show were stale
// header handles (see LTGUI_Own). LT_GUINOCOPY=1 brings that back.
static void LTGUI_OnOffRebuild(CLTGUITextItemCtrl* pCtrl, HSTRING hOff, HSTRING hOn)
{
    pCtrl->RemoveAll();
    pCtrl->AddString(hOff);     // copies
    pCtrl->AddString(hOn);
}

void CLTGUIOnOffCtrl::SetOnString(HSTRING hStrOn)
{
    if (getenv("LT_GUINOCOPY")) { m_hStrOn = hStrOn; return; }
    if (m_pClientDE && m_hStrOn) m_pClientDE->FreeString(m_hStrOn);
    m_hStrOn = LTGUI_Own(m_pClientDE, hStrOn);
    LTGUI_OnOffRebuild(this, m_hStrOff, m_hStrOn);
    // ⚠️ SILENTLY: the base class's, not the cycle's notifying SetSelIndex.
    // CFolderControls::Build sets NotifyOnChange BEFORE SetOnString
    // (FolderControls.cpp:80); notifying here ran the folder's OnCommand on a
    // half-built folder -- SIGSEGV entering Options > Controls.
    CLTGUITextItemCtrl::SetSelIndex(m_bOn ? 1 : 0);
}

void CLTGUIOnOffCtrl::SetOffString(HSTRING hStrOff)
{
    if (getenv("LT_GUINOCOPY")) { m_hStrOff = hStrOff; return; }
    if (m_pClientDE && m_hStrOff) m_pClientDE->FreeString(m_hStrOff);
    m_hStrOff = LTGUI_Own(m_pClientDE, hStrOff);
    LTGUI_OnOffRebuild(this, m_hStrOff, m_hStrOn);
    // ⚠️ SILENTLY: the base class's, not the cycle's notifying SetSelIndex.
    // CFolderControls::Build sets NotifyOnChange BEFORE SetOnString
    // (FolderControls.cpp:80); notifying here ran the folder's OnCommand on a
    // half-built folder -- SIGSEGV entering Options > Controls.
    CLTGUITextItemCtrl::SetSelIndex(m_bOn ? 1 : 0);
}

void CLTGUIOnOffCtrl::SetOnString(int nStrOnID)
{
    HSTRING h = m_pClientDE ? m_pClientDE->FormatString(nStrOnID) : 0;
    SetOnString(h);
    if (h && !getenv("LT_GUINOCOPY")) m_pClientDE->FreeString(h);   // kept a copy
}

void CLTGUIOnOffCtrl::SetOffString(int nStrOffID)
{
    HSTRING h = m_pClientDE ? m_pClientDE->FormatString(nStrOffID) : 0;
    SetOffString(h);
    if (h && !getenv("LT_GUINOCOPY")) m_pClientDE->FreeString(h);   // kept a copy
}

void CLTGUIOnOffCtrl::SetOn(LTBOOL bOn)
{
    m_bOn = bOn;
    if (m_pbValue)
        *m_pbValue = bOn;
    if (m_stringArray.GetSize() == 2)
        SetSelIndex(bOn ? 1 : 0);
}

void CLTGUIOnOffCtrl::UpdateData(LTBOOL bSaveAndValidate)
{
    if (!m_pbValue)
        return;

    // Loading the value into the control is not the player changing it: no
    // notification (the cycle's m_bInUpdate guard), only a toggle does.
    if (bSaveAndValidate) *m_pbValue = m_bOn;
    else
    {
        const LTBOOL bWas = m_bInUpdate;
        m_bInUpdate = LTTRUE;
        SetOn(*m_pbValue);
        m_bInUpdate = bWas;
    }
}

// ----------------------------------------------------------------------- //
// CLTGUIColumnTextCtrl -- columns of text.
// ----------------------------------------------------------------------- //

CLTGUIColumnTextCtrl::CLTGUIColumnTextCtrl()
{
    m_pClientDE = 0;
    m_pFont     = 0;
    m_dwWidth   = 0;
    m_dwHeight  = 0;
}

CLTGUIColumnTextCtrl::~CLTGUIColumnTextCtrl()
{
    Destroy();
}

void CLTGUIColumnTextCtrl::Destroy()
{
    RemoveAllColumns();
    m_pFont     = 0;
    m_pClientDE = 0;
    m_bCreated  = LTFALSE;
}

void CLTGUIColumnTextCtrl::RemoveAllColumns()
{
    for (uint32 i = 0; i < m_columnArray.GetSize(); ++i)
    {
        LTGUIColumn* pCol = m_columnArray[i];
        if (!pCol) continue;
        if (m_pClientDE && pCol->m_hString)
            m_pClientDE->FreeString(pCol->m_hString);
        delete pCol;
    }
    m_columnArray.SetSize(0);
    m_dwWidth = 0;
}

LTBOOL CLTGUIColumnTextCtrl::AddColumn(HSTRING hString, int nWidth, short nJustify)
{
    LTGUIColumn* pCol = new LTGUIColumn;
    if (!pCol)
        return LTFALSE;

    pCol->m_hString = LTGUI_Own(m_pClientDE, hString);
    pCol->m_nWidth  = nWidth;
    pCol->m_nJustify = nJustify;

    m_columnArray.Add(pCol);
    m_dwWidth += (uint32)nWidth;
    if (m_pFont)
        m_dwHeight = (uint32)m_pFont->GetHeight();
    return LTTRUE;
}

LTBOOL CLTGUIColumnTextCtrl::AddColumn(int messageCode, int nWidth, short nJustify)
{
    HSTRING h = m_pClientDE ? m_pClientDE->FormatString(messageCode) : 0;
    const LTBOOL b = AddColumn(h, nWidth, nJustify);
    if (h && getenv("LT_GUINOCOPY") == 0) m_pClientDE->FreeString(h);   // AddColumn kept a copy
    return b;
}

HSTRING CLTGUIColumnTextCtrl::GetString(int nColumnIndex)
{
    if (nColumnIndex < 0 || (uint32)nColumnIndex >= m_columnArray.GetSize())
        return 0;
    LTGUIColumn* pCol = m_columnArray[nColumnIndex];
    return pCol ? pCol->m_hString : 0;
}

void CLTGUIColumnTextCtrl::SetString(int nColumnIndex, HSTRING hString)
{
    if (nColumnIndex < 0 || (uint32)nColumnIndex >= m_columnArray.GetSize())
        return;
    LTGUIColumn* pCol = m_columnArray[nColumnIndex];
    if (!pCol)
        return;

    // Release the old handle first; these are engine-owned and leaking them
    // would accumulate across every menu refresh.
    if (m_pClientDE && pCol->m_hString && pCol->m_hString != hString)
        m_pClientDE->FreeString(pCol->m_hString);

    pCol->m_hString = (pCol->m_hString == hString) ? hString : LTGUI_Own(m_pClientDE, hString);
}

void CLTGUIColumnTextCtrl::SetString(int nColumnIndex, int messageCode)
{
    HSTRING h = m_pClientDE ? m_pClientDE->FormatString(messageCode) : 0;
    SetString(nColumnIndex, h);
    if (h && getenv("LT_GUINOCOPY") == 0) m_pClientDE->FreeString(h);   // SetString kept a copy
}

void CLTGUIColumnTextCtrl::Render(HSURFACE hDestSurf)
{
    // Columns: each string at its own x offset, all sharing the control colour.
    if (!m_pFont) return;

    LTIntPt pos = GetPos();
    HLTCOLOR col = GetCurrentColor();

    int x = pos.x;
    for (uint32 i = 0; i < m_columnArray.GetSize(); ++i)
    {
        LTGUIColumn* pCol = m_columnArray[i];
        if (!pCol) continue;

        if (pCol->m_hString)
            m_pFont->Draw(pCol->m_hString, hDestSurf, x, pos.y, pCol->m_nJustify, col);

        x += pCol->m_nWidth;
    }
}

// ----------------------------------------------------------------------- //
// CLTGUIEditCtrl -- text entry.
// ----------------------------------------------------------------------- //

CLTGUIEditCtrl::CLTGUIEditCtrl()
{
    m_pClientDE      = 0;
    m_pFont          = 0;
    m_lpszString     = 0;
    m_hDescription   = 0;
    m_nAlignment     = 0;
    m_nTextOffset    = 0;
    m_nBufferSize    = 0;
    m_nCursorPos     = 0;
    m_pStringValue   = 0;
    m_bCursorEnabled = LTFALSE;
    m_hCursorColor   = SETRGB(255, 255, 255);
    m_fCursorTime    = 0.0f;
    m_pCommandHandler = 0;
}

CLTGUIEditCtrl::~CLTGUIEditCtrl()
{
    Destroy();
}

void CLTGUIEditCtrl::Destroy()
{
    delete[] m_lpszString;
    m_lpszString = 0;

    if (m_pClientDE && m_hDescription)
        m_pClientDE->FreeString(m_hDescription);
    m_hDescription = 0;

    m_pFont     = 0;
    m_pClientDE = 0;
    m_bCreated  = LTFALSE;
}

void CLTGUIEditCtrl::SetText(char* lpszString)
{
    if (!lpszString)
        return;

    // Respect the buffer size the control was created with; a longer string is
    // truncated rather than overrunning.
    int nLen = (int)strlen(lpszString);
    if (m_nBufferSize > 0 && nLen > m_nBufferSize - 1)
        nLen = m_nBufferSize - 1;

    // ⚠️ THE BUFFER STAYS THE SIZE THE CONTROL WAS CREATED WITH (HANDOVER
    // 5da). Reallocating it to the text's length made it too short for what
    // comes next: UpdateData's strncpy pads to the full size and HandleChar
    // inserts up to it -- FolderSave::NameSaveGame does SetText then
    // UpdateData(FALSE), a heap overrun every time the save-name field opened.
    const int nAlloc = (m_nBufferSize > 0) ? m_nBufferSize : nLen + 1;
    char* pNew = new char[nAlloc];
    if (!pNew)
        return;
    memcpy(pNew, lpszString, (size_t)nLen);
    pNew[nLen] = '\0';
    delete[] m_lpszString;
    m_lpszString = pNew;
    m_nCursorPos = nLen;
}

void CLTGUIEditCtrl::EnableCursor(float fToggleTime, HLTCOLOR hColor)
{
    m_bCursorEnabled = LTTRUE;
    m_fCursorTime    = fToggleTime;
    m_hCursorColor   = hColor;
}

void CLTGUIEditCtrl::Render(HSURFACE hDestSurf)
{
    if (!m_pFont) return;

    LTIntPt pos = GetPos();
    HLTCOLOR col = GetCurrentColor();

    if (m_hDescription)
        m_pFont->Draw(m_hDescription, hDestSurf, pos.x, pos.y, m_nAlignment, col);

    // The edit text is a plain char buffer, not an HSTRING.
    if (m_lpszString)
        m_pFont->Draw(m_lpszString, hDestSurf, pos.x + m_nTextOffset, pos.y, 0, col);

    // The caret EnableCursor asked for, blinking at its toggle time while the
    // field is selected (the folders Select it when they capture it). It was
    // never drawn (HANDOVER 5da).
    if (m_bCursorEnabled && IsSelected() && m_lpszString && m_pClientDE)
    {
        const float fPeriod = (m_fCursorTime > 0.01f) ? m_fCursorTime : 0.333f;
        const bool bOn = ((int)(m_pClientDE->GetTime() / fPeriod) % 2) == 0;
        if (bOn)
        {
            char szLeft[256];
            int n = m_nCursorPos;
            if (n < 0) n = 0;
            if (n > (int)sizeof(szLeft) - 1) n = (int)sizeof(szLeft) - 1;
            if (n > (int)strlen(m_lpszString)) n = (int)strlen(m_lpszString);
            memcpy(szLeft, m_lpszString, (size_t)n);
            szLeft[n] = '\0';
            int x = pos.x + m_nTextOffset;
            if (n > 0)
            {
                HSTRING hLeft = m_pClientDE->CreateString(szLeft);
                if (hLeft)
                {
                    x += m_pFont->GetTextExtents(hLeft).x;
                    m_pClientDE->FreeString(hLeft);
                }
            }
            m_pFont->Draw((char*)"_", hDestSurf, x, pos.y, 0, m_hCursorColor);
        }
    }
}

// ----------------------------------------------------------------------- //
// Remaining CLTGUIColumnTextCtrl / CLTGUITextItemCtrl / CLTGUIEditCtrl members
// ----------------------------------------------------------------------- //

LTBOOL CLTGUIColumnTextCtrl::Create(ILTClient* pClientDE, uint32 dwCommandID,
                                    CLTGUIFont* pFont, CLTGUICommandHandler* pCommandHandler,
                                    uint32 dwParam1, uint32 dwParam2)
{
    if (!pClientDE || !pFont)
        return LTFALSE;

    m_pClientDE       = pClientDE;
    m_pFont           = pFont;
    m_dwCommandID     = dwCommandID;
    m_pCommandHandler = pCommandHandler;
    m_dwParam1        = dwParam1;
    m_dwParam2        = dwParam2;
    m_dwHeight        = (uint32)pFont->GetHeight();

    m_bCreated = LTTRUE;
    return LTTRUE;
}

LTBOOL CLTGUIColumnTextCtrl::OnEnter()
{
    if (!IsEnabled() || !m_pCommandHandler || m_dwCommandID == 0)
        return LTFALSE;
    m_pCommandHandler->SendCommand(m_dwCommandID, m_dwParam1, m_dwParam2);
    return LTTRUE;
}

void CLTGUIColumnTextCtrl::OnSelChange()
{
    // Selection only changes which colour GetCurrentColor reports; there is no
    // bound value on a column control.
}

LTBOOL CLTGUITextItemCtrl::OnEnter()
{
    if (!IsEnabled() || !m_pCommandHandler || m_dwCommandID == 0)
        return LTFALSE;
    m_pCommandHandler->SendCommand(m_dwCommandID, m_dwParam1, m_dwParam2);
    return LTTRUE;
}

// --- CLTGUIEditCtrl ------------------------------------------------------

LTBOOL CLTGUIEditCtrl::Create(ILTClient* pClientDE, uint32 dwCommandID, HSTRING hDescription,
                              CLTGUIFont* pFont, int nTextOffset, int nBufferSize,
                              CLTGUICommandHandler* pCommandHandler, char* pStringValue,
                              uint32 dwParam1, uint32 dwParam2)
{
    if (!pClientDE || !pFont || nBufferSize <= 0)
        return LTFALSE;

    m_pClientDE       = pClientDE;
    m_pFont           = pFont;
    if (m_hDescription) m_pClientDE->FreeString(m_hDescription);
    m_hDescription    = LTGUI_Own(pClientDE, hDescription);
    m_nTextOffset     = nTextOffset;
    m_nBufferSize     = nBufferSize;
    m_pCommandHandler = pCommandHandler;
    m_pStringValue    = pStringValue;
    m_dwCommandID     = dwCommandID;
    m_dwParam1        = dwParam1;
    m_dwParam2        = dwParam2;

    // Own an empty buffer up front so every edit path has somewhere to write.
    delete[] m_lpszString;
    m_lpszString = new char[nBufferSize];
    if (!m_lpszString)
        return LTFALSE;
    m_lpszString[0] = '\0';
    m_nCursorPos    = 0;

    m_bCreated = LTTRUE;
    return LTTRUE;
}

int CLTGUIEditCtrl::GetWidth()
{
    if (!m_pFont)
        return 0;
    int nText = m_lpszString ? (int)((float)strlen(m_lpszString) * (float)m_pFont->GetHeight() * 0.5f) : 0;
    return m_nTextOffset + nText;
}

int CLTGUIEditCtrl::GetHeight()
{
    return m_pFont ? m_pFont->GetHeight() : 0;
}

// ⚠️ A DISABLED EDIT FIELD STILL TAKES INPUT (HANDOVER 5da). NOLF's folders
// create their name fields `EnableCursor(); Enable(LTFALSE);` (FolderSave,
// FolderHost, FolderPlayer) -- disabled so menu navigation skips them -- and
// use them only through SetCapture. Refusing input when disabled (the port's
// old rule) meant the save name could not be typed and Enter did nothing: no
// save. The donor's edit control (NOLF2 LTGUIMgr) checks nothing.
// LT_GUIOLDEDIT=1 = refuse when disabled (old, says so).
static bool LTGUI_EditRefuses(const CLTGUICtrl* p)
{
    static const bool s_bOld = []() {
        const bool b = (getenv("LT_GUIOLDEDIT") != 0);
        if (b) { printf("[GUI] LT_GUIOLDEDIT=1 -- a disabled edit field refuses typing and Enter (old)\n"); fflush(stdout); }
        return b; }();
    return s_bOld && !const_cast<CLTGUICtrl*>(p)->IsEnabled();
}

// Text entry: backspace and the arrow keys move/edit; everything else is left
// for the container so navigation still works out of an edit box.
LTBOOL CLTGUIEditCtrl::HandleKeyDown(int key, int rep)
{
    (void)rep;
    if (LTGUI_EditRefuses(this) || !m_lpszString)
        return LTFALSE;

    switch (key)
    {
        case VK_BACK:
            if (m_nCursorPos > 0)
            {
                int len = (int)strlen(m_lpszString);
                memmove(m_lpszString + m_nCursorPos - 1,
                        m_lpszString + m_nCursorPos,
                        (size_t)(len - m_nCursorPos) + 1);
                --m_nCursorPos;
            }
            return LTTRUE;

        case VK_DELETE:
        {
            int len = (int)strlen(m_lpszString);
            if (m_nCursorPos < len)
                memmove(m_lpszString + m_nCursorPos,
                        m_lpszString + m_nCursorPos + 1,
                        (size_t)(len - m_nCursorPos));
            return LTTRUE;
        }

        case VK_LEFT:
            if (m_nCursorPos > 0) { --m_nCursorPos; return LTTRUE; }
            return LTFALSE;

        case VK_RIGHT:
            if (m_nCursorPos < (int)strlen(m_lpszString)) { ++m_nCursorPos; return LTTRUE; }
            return LTFALSE;

        case VK_HOME:  m_nCursorPos = 0; return LTTRUE;
        case VK_END:   m_nCursorPos = (int)strlen(m_lpszString); return LTTRUE;
        case VK_RETURN: return OnEnter();
        default: break;
    }
    return LTFALSE;
}

LTBOOL CLTGUIEditCtrl::HandleChar(char c)
{
    if (LTGUI_EditRefuses(this) || !m_lpszString || m_nBufferSize <= 0)
        return LTFALSE;

    // Control characters are not text; backspace etc. arrive via HandleKeyDown.
    if ((unsigned char)c < 32)
        return LTFALSE;

    int len = (int)strlen(m_lpszString);
    if (len >= m_nBufferSize - 1)
        return LTTRUE;      // full: swallow rather than overrun

    memmove(m_lpszString + m_nCursorPos + 1,
            m_lpszString + m_nCursorPos,
            (size_t)(len - m_nCursorPos) + 1);
    m_lpszString[m_nCursorPos] = c;
    ++m_nCursorPos;
    return LTTRUE;
}

LTBOOL CLTGUIEditCtrl::OnEnter()
{
    if (LTGUI_EditRefuses(this))
        return LTFALSE;

    UpdateData(LTTRUE);     // commit before notifying

    if (!m_pCommandHandler || m_dwCommandID == 0)
        return LTFALSE;
    m_pCommandHandler->SendCommand(m_dwCommandID, m_dwParam1, m_dwParam2);
    return LTTRUE;
}

void CLTGUIEditCtrl::OnSelChange()
{
    // Entering the field puts the caret at the end, which is what the menus do.
    if (m_lpszString)
        m_nCursorPos = (int)strlen(m_lpszString);
}

void CLTGUIEditCtrl::UpdateData(LTBOOL bSaveAndValidate)
{
    if (!m_pStringValue || !m_lpszString || m_nBufferSize <= 0)
        return;

    if (bSaveAndValidate)
    {
        strncpy(m_pStringValue, m_lpszString, (size_t)m_nBufferSize - 1);
        m_pStringValue[m_nBufferSize - 1] = '\0';
    }
    else
    {
        strncpy(m_lpszString, m_pStringValue, (size_t)m_nBufferSize - 1);
        m_lpszString[m_nBufferSize - 1] = '\0';
        m_nCursorPos = (int)strlen(m_lpszString);
    }
}
