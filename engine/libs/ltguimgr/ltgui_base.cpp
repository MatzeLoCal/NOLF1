// ----------------------------------------------------------------------- //
//
// MODULE  : ltgui_base.cpp
//
// PURPOSE : CLTGUICtrl, CLTGUICommandHandler and CLTGUIFont -- the foundation
//           of NOLF 1's UI widget library, declared in LT2/lithshared/ltguimgr/.
//
// WHY THIS EXISTS
//
//   lithshared ships headers only; the original build linked a prebuilt
//   ltguimgr_LT2.lib. Written against the SDK's own declarations; the headers
//   are not modified.
//
// ⚠️ WHAT IS REAL HERE AND WHAT IS NOT
//
//   The control STATE machine is real: selection, enable, position, command
//   dispatch, key routing. That logic is self-contained and correct.
//
//   The DRAWING is not, and cannot be yet. Every Render/Draw path ends at
//   HSURFACE and ILTClient -- the engine surface API, which is currently the
//   announcing skeleton. So these compile and link and their state behaves,
//   but nothing appears on screen until a real renderer exists.
//
//   That distinction is deliberate and is why the drawing paths report rather
//   than silently doing nothing: a UI that quietly draws nothing looks
//   identical to a UI that is broken.
//
// ⚠️ CLTGUIFont IS THE BOUNDARY WITH lithfontmgr
//
//   CLTGUIFont declares a `LithFont* m_pLithFont`, and lithfontmgr is another
//   headers-only library (~68 more undefined methods). Those are NOT referenced
//   today, because only CLTGUIFont's bodies would dereference that pointer.
//
//   This implementation therefore does NOT delegate to LithFont. Doing so would
//   drag in a bitmap-font engine that must be written against a surface API
//   that does not exist. Text metrics are computed from the font height instead,
//   which is enough for layout to be exercised, and the real font engine can be
//   dropped in behind this boundary later.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include <stdio.h>
#include <string.h>

#include "ltfont.h"
#include "ltpcx.h"
#include "ltfilemgr.h"
#include "ltnullrender.h"

// ⚠️ ltguicommandhandler.h MUST precede ltguictrl.h: CLTGUICtrl derives from
// CLTGUICommandHandler but ltguictrl.h does not include it, relying on the
// includer instead. Same pattern as mfcstub/windows.h elsewhere in this port.
#include "iltclient.h"          // ILTClient, needed by ltguifont.h
#include "ltguicommandhandler.h"
#include "ltguifont.h"
#include "ltguictrl.h"

// ----------------------------------------------------------------------- //
// One-shot reporting for paths that need a renderer.
// ----------------------------------------------------------------------- //

static void ltgui_NeedsRenderer(const char* what)
{
    static bool s_reported = false;
    if (s_reported)
        return;
    s_reported = true;
    fprintf(stderr,
        "\n[LTGUI] %s was called, but drawing requires the engine surface API\n"
        "[LTGUI] (HSURFACE / ILTClient), which is currently the announcing\n"
        "[LTGUI] skeleton. Control STATE is live; nothing is rendered yet.\n\n",
        what ? what : "a draw path");
    fflush(stderr);
}

// ----------------------------------------------------------------------- //
// CLTGUICommandHandler
// ----------------------------------------------------------------------- //

CLTGUICommandHandler::CLTGUICommandHandler()
{
}

CLTGUICommandHandler::~CLTGUICommandHandler()
{
}

// ----------------------------------------------------------------------- //
// CLTGUICtrl -- the base control.
//
// The event handlers return LTFALSE meaning "not handled", which is what lets
// a container move focus on to the next control. Returning LTTRUE from the base
// would silently swallow every arrow key in the menus.
// ----------------------------------------------------------------------- //

CLTGUICtrl::CLTGUICtrl()
{
    m_dwCommandID  = 0;
    m_dwParam1     = 0;
    m_dwParam2     = 0;
    m_bCreated     = LTFALSE;
    m_pos.x        = 0;
    m_pos.y        = 0;
    m_bSelected    = LTFALSE;
    m_bEnabled     = LTTRUE;
    m_xBoxDrawAdj  = 0;
    m_dwHelpID     = 0;
    m_hTransColor  = 0;
}

CLTGUICtrl::~CLTGUICtrl()
{
}

void CLTGUICtrl::Destroy()
{
    m_bCreated = LTFALSE;
}

void CLTGUICtrl::ResetAnimation()
{
    // Base controls have no animation state; derived ones override.
}

void CLTGUICtrl::Select(LTBOOL bSelected)
{
    m_bSelected = bSelected;
}

void CLTGUICtrl::UpdateData(LTBOOL bSaveAndValidate)
{
    // The base holds no bound data. Derived controls move values between their
    // display and the variable they were created with.
    (void)bSaveAndValidate;
}

void CLTGUICtrl::OnCommand(uint32 dwCommandID)
{
    // The base does not act on commands; a container routes them to a handler.
    (void)dwCommandID;
}

// Key routing. The arrow keys and enter are turned into the semantic handlers
// so a derived control only has to override the ones it cares about.
LTBOOL CLTGUICtrl::HandleKeyDown(int key, int rep)
{
    (void)rep;

    if (!m_bEnabled)
        return LTFALSE;

    switch (key)
    {
        case VK_LEFT:   return OnLeft();
        case VK_RIGHT:  return OnRight();
        case VK_UP:     return OnUp();
        case VK_DOWN:   return OnDown();
        case VK_RETURN: return OnEnter();
        default:        break;
    }
    return LTFALSE;
}

LTBOOL CLTGUICtrl::HandleChar(char c)
{
    (void)c;
    return LTFALSE;     // only text-entry controls consume characters
}

LTBOOL CLTGUICtrl::OnLeft()  { return LTFALSE; }
LTBOOL CLTGUICtrl::OnRight() { return LTFALSE; }
LTBOOL CLTGUICtrl::OnUp()    { return LTFALSE; }
LTBOOL CLTGUICtrl::OnDown()  { return LTFALSE; }

LTBOOL CLTGUICtrl::OnEnter()
{
    // Enter on an enabled control raises its command; that is the base action.
    if (!m_bEnabled)
        return LTFALSE;
    if (m_dwCommandID == 0)
        return LTFALSE;
    OnCommand(m_dwCommandID);
    return LTTRUE;
}

LTBOOL CLTGUICtrl::OnLButtonDown(int x, int y)     { (void)x; (void)y; return LTFALSE; }
LTBOOL CLTGUICtrl::OnLButtonUp(int x, int y)       { (void)x; (void)y; return LTFALSE; }
LTBOOL CLTGUICtrl::OnLButtonDblClick(int x, int y) { (void)x; (void)y; return LTFALSE; }
LTBOOL CLTGUICtrl::OnRButtonDown(int x, int y)     { (void)x; (void)y; return LTFALSE; }
LTBOOL CLTGUICtrl::OnRButtonUp(int x, int y)       { (void)x; (void)y; return LTFALSE; }
LTBOOL CLTGUICtrl::OnRButtonDblClick(int x, int y) { (void)x; (void)y; return LTFALSE; }
LTBOOL CLTGUICtrl::OnMouseMove(int x, int y)       { (void)x; (void)y; return LTFALSE; }

// ----------------------------------------------------------------------- //
// CLTGUIFont
//
// Metrics are real enough for layout to be exercised; drawing is not.
// ----------------------------------------------------------------------- //

LTBOOL CLTGUIFont::s_bWrapUseSpaces = LTTRUE;

CLTGUIFont::CLTGUIFont()
{
    m_pClientDE  = 0;
    m_nFontHeight = 0;
    m_pLithFont  = 0;       // see the boundary note at the top of this file
}

// ⚠️ m_pLithFont IS REPURPOSED, and that is deliberate.
//
// CLTGUIFont declares `LithFont* m_pLithFont` -- lithfontmgr's type, which this
// port does not implement. Rather than modify the SDK header (rule 3), the
// pointer carries a CLTBitmapFont instead. Nothing outside this file ever
// dereferences it, so the substitution is invisible to the game, and the SDK
// header stays byte-identical to the original.
static CLTBitmapFont* GuiFont(LithFont* p) { return (CLTBitmapFont*)p; }

LTBOOL CLTGUIFont::Init(ILTClient* pClientDE, LITHFONTCREATESTRUCT* lfCS)
{
    if (!pClientDE)
        return LTFALSE;

    m_pClientDE = pClientDE;

    // ---- the BITMAP path, which is what NOLF's English build uses ----
    //
    // CInterfaceResMgr::SetupFont fills in szFontBitmap and a transparent
    // colour: kBlack for blended fonts, SETRGB(255,0,255) for chroma-keyed
    // ones (InterfaceResMgr.cpp:765-771). See ltfont.h.
    if (lfCS && lfCS->szFontBitmap && lfCS->szFontBitmap[0])
    {
        CLTPcx pcx;
        if (pcx.LoadThroughFileMgr(LTGetFileMgr(), lfCS->szFontBitmap))
        {
            CLTBitmapFont* pFont = new CLTBitmapFont;

            unsigned int flags = lfCS->nGroupFlags ? lfCS->nGroupFlags : LTFONT_ALL;

            if (pFont->Build(pcx.GetRGBA(), (int)pcx.GetWidth(), (int)pcx.GetHeight(),
                             (unsigned int)lfCS->hTransColor, flags))
            {
                Term();
                pFont->m_bChromaKey = lfCS->bChromaKey ? true : false;
                m_pClientDE   = pClientDE;
                m_pLithFont   = (LithFont*)pFont;
                m_nFontHeight = pFont->GetHeight();
                return LTTRUE;
            }

            delete pFont;
        }

        // ⚠️ Reported, not silent. A font that fails to load makes every later
        // string measure zero, and zero-width text is invisible rather than
        // obviously broken -- the symptom would look like a layout bug.
        fprintf(stderr, "[LTGUI] ⚠️  bitmap font failed: %s\n", lfCS->szFontBitmap);
        return LTFALSE;
    }

    // ---- the SYSTEM-FONT path, which English never takes ----
    //
    // InitEngineFont supplies a typeface NAME and no bitmap. Implementing it
    // means a platform text engine; until then keep the cell height so layout
    // stays proportional, and say so once.
    m_nFontHeight = (lfCS && lfCS->nHeight > 0) ? lfCS->nHeight : 12;

    static int s_bSaid = 0;
    if (!s_bSaid)
    {
        s_bSaid = 1;
        fprintf(stderr, "[LTGUI] ⚠️  system fonts are not implemented (non-English"
                        " builds only); metrics are approximate\n");
    }
    return LTTRUE;
}

void CLTGUIFont::Term()
{
    delete GuiFont(m_pLithFont);
    m_pClientDE   = 0;
    m_nFontHeight = 0;
    m_pLithFont   = 0;
}

int CLTGUIFont::GetHeight()
{
    return m_nFontHeight;
}

LTBOOL CLTGUIFont::IsChromaKeyed()
{
    // ⚠️ THE FONT'S OWN bChromaKey, as LithBitmapFont answers it
    // (lithbitmapfont.h:43) -- NOT "is it a bitmap font" (HANDOVER 5dt).
    // CInterfaceResMgr::SetupFont makes every menu font a BLENDED one (no
    // key, black background); only the air and weapon-chooser fonts are
    // keyed. CStaticTextCtrl asks this to choose how it draws: keyed, a green
    // transparent blit of WHITE glyphs; blended, MASK for black text, ADD for
    // white, MASK then ADD x colour otherwise. Answering "keyed" for every
    // font drew the difficulty reminder, the briefing and every other static
    // text in white where the game asks for black. LT_GUIALLKEYED=1 = old.
    static const bool s_bOld = []() {
        const bool b = (getenv("LT_GUIALLKEYED") != 0);
        if (b) { printf("[LTGUI] LT_GUIALLKEYED=1 -- every bitmap font reports chroma-keyed (old)\n"); fflush(stdout); }
        return b; }();
    if (!m_pLithFont) return LTFALSE;
    if (s_bOld) return LTTRUE;
    return GuiFont(m_pLithFont)->m_bChromaKey ? LTTRUE : LTFALSE;
}

LTBOOL CLTGUIFont::IsSystemFont()
{
    return m_pLithFont ? LTFALSE : LTTRUE;
}

// ⚠️ Approximate metrics. Without a rasterised font there are no per-glyph
// widths, so this uses a fixed ratio of the cell height. Layout that merely
// needs to be consistent will behave; anything measuring exact pixel extents
// will be wrong, and that is the first thing to revisit when a real font
// engine lands behind this boundary.
static const float kLTGuiAvgCharWidthRatio = 0.5f;

static int ltgui_TextWidth(const char* s, int nHeight)
{
    if (!s || !*s)
        return 0;
    return (int)((float)strlen(s) * (float)nHeight * kLTGuiAvgCharWidthRatio);
}

// Real per-glyph metrics when a bitmap font is loaded; the ratio above only
// when it is not (the system-font path).
static int ltgui_MeasureWidth(LithFont* pLith, const char* s, int nHeight)
{
    if (pLith) return GuiFont(pLith)->MeasureWidth(s);
    return ltgui_TextWidth(s, nHeight);
}

LTIntPt CLTGUIFont::GetTextExtents(HSTRING hString)
{
    LTIntPt pt;
    pt.x = 0;
    pt.y = m_nFontHeight;

    if (m_pClientDE && hString)
    {
        const char* s = m_pClientDE->GetStringData(hString);
        pt.x = ltgui_MeasureWidth(m_pLithFont, s, m_nFontHeight);
    }
    return pt;
}

LTIntPt CLTGUIFont::GetTextExtentsFormat(HSTRING hString, int nWidth)
{
    LTIntPt pt = GetTextExtents(hString);

    // Wrapped text: as many lines as the single-line width needs.
    if (nWidth > 0 && pt.x > nWidth)
    {
        int lines = (pt.x + nWidth - 1) / nWidth;
        pt.x = nWidth;
        pt.y = m_nFontHeight * lines;
    }
    return pt;
}

// --- drawing: REAL, via the bitmap font and the 2D compositor ------------
//
// ⚠️ The transparent colour is the one the font was BUILT with, because that is
// what separates glyphs in the strip. Passing anything else keys out nothing
// and each glyph arrives inside a rectangle of its own separator colour.
//
// hTransColor is not stored on CLTGUIFont (the SDK header has no field for it),
// so it is recovered from the sheet: the separator is by definition the colour
// of the strip's first column, which is always blank.

static unsigned int ltgui_FontKey(LithFont* pLith)
{
    CLTBitmapFont* pFont = GuiFont(pLith);
    if (!pFont || !pFont->IsBuilt()) return 0;
    return LT2D::GetPixel(pFont->GetSheet(), 0, 0);
}

// LT_TRACE_TEXT=1: every string drawn, with its position, justification and
// colour. ⚠️ "text is drawn" and "the RIGHT text is drawn in the right place"
// are different claims, and only this can tell them apart.
static int ltgui_TraceText()
{
    static int s = -1;
    if (s < 0) s = getenv("LT_TRACE_TEXT") ? 1 : 0;
    return s;
}

static void ltgui_DrawText(LithFont* pLith, HSURFACE hDest, const char* s,
                           int x, int y, int justify, HLTCOLOR color,
                           bool bTint = true)
{
    if (!pLith || !s || !*s) return;

    if (ltgui_TraceText())
        fprintf(stderr, "[LTTEXT] (%4d,%4d) just=%d col=%08X \"%.60s\"\n",
                x, y, justify, (unsigned)color, s);

    CLTBitmapFont* pFont = GuiFont(pLith);
    LT2DSurf dst = LTGetNullRender()->GetView(hDest);
    if (!dst.m_pRGBA) return;

    // LTF_JUSTIFY_LEFT 0, RIGHT 1, CENTER 2 -- lithfontdefs.h:117.
    int w = pFont->MeasureWidth(s);
    if (justify == 1) x -= w;
    else if (justify == 2) x -= w / 2;

    const unsigned int key = ltgui_FontKey(pLith);

    // ⚠️ THE COLOUR IS A COLOUR, AND 0 IS BLACK.
    //
    // A first version treated 0 as "untinted, use the font's own pixels",
    // reasoning that CLTGUIFont's declarations default the argument to LTNULL.
    // That was wrong, and the trace proved it -- NOLF's main menu draws:
    //
    //     (54,148) col=00FFFFFF "Continue game"    <- selected
    //     (54,212) col=00000000 "Options"          <- not selected
    //
    // The game sets those through CLTGUIFadeColorCtrl::SetColor, so black is a
    // deliberate choice: NOLF's menu is DARK TEXT on the bright 3D scene that
    // forms the background. Rendering it untinted made the menu readable here
    // only because our background is black -- an accident, not a fix.
    //
    // ⚠️ So the honest result is that unselected items are INVISIBLE until the
    // 3D menu background exists (HANDOVER.md §2). That is the missing renderer
    // showing through, not a text bug.
    //
    // LT_UI_FORCE_VISIBLE=1 draws untinted anyway, so the menu can be USED in
    // the meantime. It is a debug aid and it is named like one.
    static int s_bForce = -1;
    if (s_bForce < 0) s_bForce = getenv("LT_UI_FORCE_VISIBLE") ? 1 : 0;

    // ⚠️ bTint false means DRAW THE FONT'S OWN PIXELS. That is not the same as
    // "colour 0", which is a deliberate solid black -- see the menu note above.
    // The LITHFONTDRAWDATA overloads need the distinction: they carry a colour
    // AND a mode, and conflating "no mode flag" with "black" painted every line
    // of mission text black on a black surface.
    const bool bSolid = bTint && !s_bForce;

    pFont->Draw(dst, x, y, s, bSolid, (unsigned int)color, key);

    // ⚠️ AND INTO THE COMPOSITOR'S GREY LAYER (ltcomp.h), like every 2D op in
    // ltnullrender. The glyphs went into the black layer only, so where the
    // 3D shows through (G 128) a glyph of value c came back as
    // scene * (128 - c) * 2/255 + c: black text vanished into the 3D -- the
    // main menu's unselected "Multiplayer / Options / Quit" over its native
    // 3D background (HANDOVER 5ce) -- and any HUD text with a channel below
    // 128 had the scene bleeding through that channel. The same write in
    // both layers keeps the composite exact. LT_GUINOTWIN=1 = black layer
    // only (old, says so).
    static const bool s_bNoTwin = []() {
        const bool b = (getenv("LT_GUINOTWIN") != 0);
        if (b) { printf("[GUI] LT_GUINOTWIN=1 -- menu/HUD text into the compositor's black layer only (old)\n"); fflush(stdout); }
        return b; }();
    if (!s_bNoTwin)
    {
        LT2DSurf twin = LTGetNullRender()->GetTwinView(hDest);
        if (twin.m_pRGBA) pFont->Draw(twin, x, y, s, bSolid, (unsigned int)color, key);
    }
}

// ⚠️ Word wrapping, and it must break on SPACES rather than mid-word --
// s_bWrapUseSpaces exists precisely because Japanese does not (ltguifont.h:65).
//
// ⚠️ bTint false draws the font's own pixels; see ltgui_DrawText. The wrapped
// path needs the same distinction as the unwrapped one, or mission text -- the
// one caller that is BOTH formatted and untinted -- goes black again.
static int ltgui_DrawWrappedTinted(LithFont* pLith, HSURFACE hDest, const char* s,
                                   int x, int y, int nWidth, HLTCOLOR color,
                                   bool bTint, int nLineHeight,
                                   bool bMeasureOnly = false);

static int ltgui_DrawWrappedTinted(LithFont* pLith, HSURFACE hDest, const char* s,
                                   int x, int y, int nWidth, HLTCOLOR color,
                                   bool bTint, int nLineHeight,
                                   bool bMeasureOnly)
{
    int nLines = 0;
    if (!pLith || !s || !*s || nWidth <= 0) return 0;

    CLTBitmapFont* pFont = GuiFont(pLith);

    char line[512];
    int  nLine = 0;
    int  lastSpace = -1;
    int  penY = y;

    for (const char* p = s; ; ++p)
    {
        if (*p && *p != '\n' && nLine < (int)sizeof(line) - 1)
        {
            if (*p == ' ') lastSpace = nLine;
            line[nLine++] = *p;
            line[nLine] = 0;

            if (pFont->MeasureWidth(line) <= nWidth) continue;

            // Too wide: back up to the last space if there was one.
            int nBreak = (lastSpace > 0) ? lastSpace : nLine - 1;
            char save = line[nBreak];
            line[nBreak] = 0;

            if (!bMeasureOnly)
                ltgui_DrawText(pLith, hDest, line, x, penY, 0, color, bTint);
            penY += nLineHeight; ++nLines;

            // Carry the remainder, skipping the space we broke on.
            int nStart = (lastSpace > 0) ? nBreak + 1 : nBreak;
            line[nBreak] = save;
            memmove(line, line + nStart, (size_t)(nLine - nStart) + 1);
            nLine -= nStart;
            lastSpace = -1;
            continue;
        }

        if (nLine > 0)
        {
            if (!bMeasureOnly)
                ltgui_DrawText(pLith, hDest, line, x, penY, 0, color, bTint);
            penY += nLineHeight; ++nLines;
            nLine = 0; line[0] = 0; lastSpace = -1;
        }

        if (!*p) break;
    }

    return nLines;
}

void CLTGUIFont::Draw(HSTRING hString, HSURFACE hDest, int x, int y, int justify, HLTCOLOR color)
{
    if (!m_pClientDE || !hString) return;
    ltgui_DrawText(m_pLithFont, hDest, m_pClientDE->GetStringData(hString), x, y, justify, color);
}

void CLTGUIFont::Draw(char* szString, HSURFACE hDest, int x, int y, int justify, HLTCOLOR color)
{
    ltgui_DrawText(m_pLithFont, hDest, szString, x, y, justify, color);
}

// ⚠️ THE DRAWDATA OVERLOADS -- AND WHY MISSION TEXT WAS INVISIBLE.
//
// These used to read the colour ONLY when LTF_DRAW_SOLID was set and pass 0
// otherwise. 0 is a real colour here -- black, deliberately, because NOLF's
// menu is dark text on a bright 3D background (see ltgui_DrawText). So every
// caller that did not set that one flag got BLACK GLYPHS.
//
// `CMissionText::Init` is exactly such a caller (MissionText.cpp:43):
//
//     m_lfDrawData.dwFlags = LTF_DRAW_TIMED | LTF_DRAW_FORMATTED
//                          | LTF_TIMED_ALL | LTF_EXTRA_LOCKLAST;
//     m_lfDrawData.hColor  = kWhite;
//
// White, asked for explicitly, and no LTF_DRAW_SOLID. It drew black text onto
// a black surface and blitted it over a letterboxed cutscene: the subtitles
// were rendered, twice per frame, and could not be seen. LTF_DRAW_SOLID is a
// rendering MODE in the original -- flat fill versus the font's own pixels --
// not permission to use the colour at all.
//
// ⚠️ LTF_DRAW_FORMATTED IS HONOURED TOO. The caller sets dwFormatWidth and
// sizes its surface from it; drawing one unwrapped line instead ran the text
// off the edge and clipped it, which would have looked like the same bug again
// as soon as the colour was fixed.
namespace
{
    void ltgui_DrawData(LithFont* pLith, HSURFACE hDest, const char* s,
                        LITHFONTDRAWDATA* lfDD, int x, int y, int nLineHeight)
    {
        if (!s || !*s) return;

        const int      justify = lfDD ? lfDD->byJustify : 0;
        const HLTCOLOR color   = lfDD ? lfDD->hColor : 0;

        // A colour of 0 from this path means "nothing was asked for" rather
        // than "black" -- these callers all set hColor when they care.
        const bool bTint = (color != 0);

        if (lfDD && (lfDD->dwFlags & LTF_DRAW_FORMATTED) && lfDD->dwFormatWidth > 0)
        {
            // ⚠️ THE CURSOR IS THE BOTTOM LINE, NOT THE TOP ONE.
            //
            // A TIMED caller draws at a CURSOR position and the font scrolls
            // finished lines UP past it -- `CMissionText::Init` sets
            // m_nCursorPos = height - lineHeight, i.e. the last line of its own
            // surface. Drawing a whole wrapped block downward from there puts
            // line 1 on the bottom edge and every line after it off the surface
            // entirely. Measured: text at y=35 and y=54 on a 54-pixel surface.
            //
            // We do not implement the reveal, so we render its FINISHED state:
            // shift up so the LAST line lands on the cursor, which is where the
            // scroll would have left it. Without the flag the caller means a
            // plain top-down block and y is already correct.
            const int nLines = ltgui_DrawWrappedTinted(
                pLith, hDest, s, x, y, (int)lfDD->dwFormatWidth,
                color, bTint, nLineHeight, true);

            int yTop = y;
            if ((lfDD->dwFlags & LTF_DRAW_TIMED) && nLines > 1)
                yTop = y - (nLines - 1) * nLineHeight;

            ltgui_DrawWrappedTinted(pLith, hDest, s, x, yTop,
                                    (int)lfDD->dwFormatWidth, color, bTint,
                                    nLineHeight);
            return;
        }

        ltgui_DrawText(pLith, hDest, s, x, y, justify, color, bTint);
    }
}

void CLTGUIFont::Draw(HSTRING hString, HSURFACE hDest, LITHFONTDRAWDATA* lfDD, int x, int y, LITHFONTSAVEDATA* lfSD)
{
    // ⚠️ lfSD is the TIMED reveal's state, and it is still ignored: the whole
    // string appears at once instead of typing itself out. The caller reads
    // lfSD->byLastState back to drive its typing sound, so that sound never
    // plays. Cosmetic, and recorded rather than hidden.
    (void)lfSD;
    if (!m_pClientDE || !hString) return;

    ltgui_DrawData(m_pLithFont, hDest, m_pClientDE->GetStringData(hString),
                   lfDD, x, y, GetHeight());
}

void CLTGUIFont::Draw(char* szString, HSURFACE hDest, LITHFONTDRAWDATA* lfDD, int x, int y, LITHFONTSAVEDATA* lfSD)
{
    (void)lfSD;
    ltgui_DrawData(m_pLithFont, hDest, szString, lfDD, x, y, GetHeight());
}

static void ltgui_DrawWrapped(LithFont* pLith, HSURFACE hDest, const char* s,
                              int x, int y, int nWidth, HLTCOLOR color, int nLineHeight)
{
    // DrawFormat's callers pass an explicit colour and mean it, black included.
    (void)ltgui_DrawWrappedTinted(pLith, hDest, s, x, y, nWidth, color, true, nLineHeight);
}

void CLTGUIFont::DrawFormat(HSTRING hString, HSURFACE hDest, int x, int y, int nWidth, HLTCOLOR color)
{
    if (!m_pClientDE || !hString) return;
    ltgui_DrawWrapped(m_pLithFont, hDest, m_pClientDE->GetStringData(hString),
                      x, y, nWidth, color, m_nFontHeight);
}

void CLTGUIFont::DrawFormat(char* szString, HSURFACE hDest, int x, int y, int nWidth, HLTCOLOR color)
{
    ltgui_DrawWrapped(m_pLithFont, hDest, szString, x, y, nWidth, color, m_nFontHeight);
}
