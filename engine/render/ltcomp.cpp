// ----------------------------------------------------------------------- //
//
// MODULE  : ltcomp.cpp
//
// See ltcomp.h. Frame state and the surface -> output mapping; the GPU half
// is installed by the host.
//
// ----------------------------------------------------------------------- //

#include "ltcomp.h"
#include "ltwindow.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    LTCompOutSizeFn  g_fnSize     = 0;
    LTCompPresentFn  g_fnPresent  = 0;
    LTCompFrameEndFn g_fnFrameEnd = 0;

    // Host frames, and the last one a RenderCamera drew in.
    unsigned int g_nFrame       = 1;
    unsigned int g_nCamFrame    = 0;
    bool         g_bCamThisFrame = false;
    unsigned int g_nObjFrame    = 0;        // the last frame RenderObjects drew in (5ce)
    bool         g_bObjThisFrame = false;

    // The mapping last presented -- what the mouse is inverted through.
    bool      g_bHaveMap = false;
    LTCompMap g_LastMap;

    // Fits a source rectangle into the output, centred.
    // ⚠️ A SHAPE THAT IS ALMOST RIGHT IS STRETCHED, not barred. The surface's
    // width is the output's aspect times a whole number of lines, rounded --
    // so "the same aspect" is never exact, and fitting it strictly left a one
    // or two pixel black seam down each side of a fullscreen game.
    void Fit(float sx, float sy, float sw, float sh, int nOutW, int nOutH, LTCompMap& m)
    {
        m.m_SrcX = sx; m.m_SrcY = sy; m.m_SrcW = sw; m.m_SrcH = sh;
        const float fOut = (float)nOutW / (float)(nOutH > 0 ? nOutH : 1);
        const float fSrc = sw / (sh > 0.0f ? sh : 1.0f);
        if (fabsf(fSrc / fOut - 1.0f) < 0.01f)
        {
            m.m_DstX = 0; m.m_DstY = 0; m.m_DstW = nOutW; m.m_DstH = nOutH;
            return;
        }
        const float s = fminf((float)nOutW / sw, (float)nOutH / sh);
        m.m_DstW = (int)(sw * s + 0.5f);
        m.m_DstH = (int)(sh * s + 0.5f);
        if (m.m_DstW > nOutW) m.m_DstW = nOutW;
        if (m.m_DstH > nOutH) m.m_DstH = nOutH;
        m.m_DstX = (nOutW - m.m_DstW) / 2;
        m.m_DstY = (nOutH - m.m_DstH) / 2;
    }

    bool EnvOn(const char* p)  { return p && p[0] && p[0] != '0'; }

    // ⚠️ THE BARS BESIDE A ZOOMED MENU ARE THE GAME'S OWN (HANDOVER 5dt).
    // CBaseFolder::Render fills everything outside its 640x480 box with the
    // layout's ShadeColor -- <185, 205, 95> in Attributes\Layout.txt, the
    // olive the Windows game shows beside its menus -- so the surface around
    // the box is not empty, and the mapping is widened at the SAME scale to
    // show it, as far as the surface reaches; past that the bars stay black.
    // A screen that draws nothing there still shows black. Widening keeps the
    // map's affine transform, so the 3D (LTComp_ObjectsRect) and the mouse
    // (LTComp_OutputToSurface) are unchanged. LT_MENUBLACKBARS=1 = old.
    void Widen(LTCompMap& m, int nSurfW, int nSurfH, int nOutW, int nOutH)
    {
        static const bool s_bBlack = []() {
            const bool b = getenv("LT_MENUBLACKBARS") != 0;
            if (b) { printf("[COMP] LT_MENUBLACKBARS=1 -- black bars beside the menus (old)\n"); fflush(stdout); }
            return b; }();
        if (s_bBlack || m.m_DstW <= 0 || m.m_DstH <= 0 || m.m_SrcW <= 0.0f || m.m_SrcH <= 0.0f) return;
        const float kx = (float)m.m_DstW / m.m_SrcW, ky = (float)m.m_DstH / m.m_SrcH;
        // The output's edges in surface pixels, clipped to the surface, then
        // back to whole output pixels and the source recomputed from those.
        float sx0 = m.m_SrcX - (float)m.m_DstX / kx;
        float sx1 = m.m_SrcX + (float)(nOutW - m.m_DstX) / kx;
        float sy0 = m.m_SrcY - (float)m.m_DstY / ky;
        float sy1 = m.m_SrcY + (float)(nOutH - m.m_DstY) / ky;
        if (sx0 < 0.0f) sx0 = 0.0f;
        if (sy0 < 0.0f) sy0 = 0.0f;
        if (sx1 > (float)nSurfW) sx1 = (float)nSurfW;
        if (sy1 > (float)nSurfH) sy1 = (float)nSurfH;
        int dx0 = m.m_DstX + (int)ceilf((sx0 - m.m_SrcX) * kx - 0.001f);
        int dy0 = m.m_DstY + (int)ceilf((sy0 - m.m_SrcY) * ky - 0.001f);
        int dx1 = m.m_DstX + (int)floorf((sx1 - m.m_SrcX) * kx + 0.001f);
        int dy1 = m.m_DstY + (int)floorf((sy1 - m.m_SrcY) * ky + 0.001f);
        if (dx0 < 0) dx0 = 0;
        if (dy0 < 0) dy0 = 0;
        if (dx1 > nOutW) dx1 = nOutW;
        if (dy1 > nOutH) dy1 = nOutH;
        if (dx1 - dx0 < m.m_DstW || dy1 - dy0 < m.m_DstH) return;
        const float fSrcX = m.m_SrcX + (float)(dx0 - m.m_DstX) / kx;
        const float fSrcY = m.m_SrcY + (float)(dy0 - m.m_DstY) / ky;
        m.m_SrcX = fSrcX; m.m_SrcY = fSrcY;
        m.m_SrcW = (float)(dx1 - dx0) / kx; m.m_SrcH = (float)(dy1 - dy0) / ky;
        m.m_DstX = dx0; m.m_DstY = dy0; m.m_DstW = dx1 - dx0; m.m_DstH = dy1 - dy0;
    }
}


void LTComp_Install(LTCompOutSizeFn fnSize, LTCompPresentFn fnPresent,
                    LTCompFrameEndFn fnFrameEnd)
{
    g_fnSize = fnSize; g_fnPresent = fnPresent; g_fnFrameEnd = fnFrameEnd;
}


bool LTComp_Wanted()
{
    static int s = -1;
    if (s >= 0) return s != 0;

    const char* pComp = getenv("LT_COMPOSITOR");
    bool b = false;
    if (pComp && pComp[0] == '0')
    {
        printf("[COMP] LT_COMPOSITOR=0 -- old path: 3D read back, CoreGraphics present\n");
        fflush(stdout);
    }
    else if (!getenv("LT_SOFTWARE"))
    {
        const char* pMetal = getenv("LT_METAL");
        const bool bWindow = LTWindow_Wanted();
        const bool bMetal  = pMetal ? EnvOn(pMetal) : bWindow;
        // A window: the game (the server host is on -- walk mode has neither
        // and keeps its own Metal path). Headless: asked for by name only, so
        // no test script changes what it measures.
        if (bWindow) b = bMetal && getenv("LT_SERVER") != 0;
        else         b = bMetal && EnvOn(pComp);
    }
    s = b ? 1 : 0;
    return b;
}


bool LTComp_Active()
{
    // The size hook is optional: headless, LT_NATIVE gives the output.
    return LTComp_Wanted() && g_fnPresent;
}


bool LTComp_OutputSize(int* pW, int* pH)
{
    if (g_fnSize && g_fnSize(pW, pH) && *pW > 0 && *pH > 0) return true;

    // Headless: LT_NATIVE=WxH, default a Retina MacBook panel.
    static int w = 0, h = 0;
    if (!w)
    {
        w = 2560; h = 1664;
        if (const char* p = getenv("LT_NATIVE"))
        {
            int a = 0, b = 0;
            if (sscanf(p, "%dx%d", &a, &b) == 2 && a >= 64 && b >= 64 && a <= 8192 && b <= 8192)
            { w = a; h = b; }
        }
    }
    *pW = w; *pH = h;
    return true;
}


void LTComp_SceneRect(int nSurfW, int nSurfH, const int* pSurfRect4, int* pOutRect4)
{
    int ow = 0, oh = 0;
    LTComp_OutputSize(&ow, &oh);

    // ⚠️ A FULL-SURFACE CAMERA FILLS THE WHOLE OUTPUT, whatever its shape -- the
    // 3D is never barred. The 2D surface is fitted inside it at its own
    // proportions (LTComp_CurrentMap), and the camera's projection widens or
    // heightens so the game's framing stays fully inside (ltscene.cpp).
    // LT_NOFILL3D=1 keeps the 3D inside the 2D's rectangle instead (says so).
    static const bool s_bNoFill = []() {
        const bool b = getenv("LT_NOFILL3D") != 0;
        if (b) { printf("[COMP] LT_NOFILL3D=1 -- the 3D is fitted with the 2D, not filled\n"); fflush(stdout); }
        return b; }();
    if (!s_bNoFill && pSurfRect4[0] <= 0 && pSurfRect4[1] <= 0 &&
        pSurfRect4[2] >= nSurfW && pSurfRect4[3] >= nSurfH)
    {
        pOutRect4[0] = 0; pOutRect4[1] = 0; pOutRect4[2] = ow; pOutRect4[3] = oh;
        return;
    }
    LTCompMap m;
    Fit(0.0f, 0.0f, (float)nSurfW, (float)nSurfH, ow, oh, m);
    const float kx = (float)m.m_DstW / (float)nSurfW;
    const float ky = (float)m.m_DstH / (float)nSurfH;
    const int x0 = m.m_DstX + (int)floorf(pSurfRect4[0] * kx + 0.5f);
    const int y0 = m.m_DstY + (int)floorf(pSurfRect4[1] * ky + 0.5f);
    const int x1 = m.m_DstX + (int)floorf(pSurfRect4[2] * kx + 0.5f);
    const int y1 = m.m_DstY + (int)floorf(pSurfRect4[3] * ky + 0.5f);
    pOutRect4[0] = x0; pOutRect4[1] = y0;
    pOutRect4[2] = x1 - x0; pOutRect4[3] = y1 - y0;
}


void LTComp_NoteCamera()
{
    g_bCamThisFrame = true;
    g_nCamFrame = g_nFrame;
}

bool LTComp_FirstCameraThisFrame() { return !g_bCamThisFrame; }

void LTComp_NoteObjects()
{
    g_bObjThisFrame = true;
    g_nObjFrame = g_nFrame;
}

bool LTComp_FirstSceneThisFrame() { return !g_bCamThisFrame && !g_bObjThisFrame; }

void LTComp_ObjectsRect(int nSurfW, int nSurfH, const int* pSurfRect4, int* pOutRect4)
{
    // The mapping CurrentMap will present with: in-game, the camera's; in a
    // menu, the zoomed box. Same arithmetic as LTComp_SceneRect's fitted case.
    const LTCompMap m = LTComp_CurrentMap(nSurfW, nSurfH);
    if (m.m_bScene) { LTComp_SceneRect(nSurfW, nSurfH, pSurfRect4, pOutRect4); return; }
    const float kx = (float)m.m_DstW / (m.m_SrcW > 0.0f ? m.m_SrcW : 1.0f);
    const float ky = (float)m.m_DstH / (m.m_SrcH > 0.0f ? m.m_SrcH : 1.0f);
    const int x0 = m.m_DstX + (int)floorf(((float)pSurfRect4[0] - m.m_SrcX) * kx + 0.5f);
    const int y0 = m.m_DstY + (int)floorf(((float)pSurfRect4[1] - m.m_SrcY) * ky + 0.5f);
    const int x1 = m.m_DstX + (int)floorf(((float)pSurfRect4[2] - m.m_SrcX) * kx + 0.5f);
    const int y1 = m.m_DstY + (int)floorf(((float)pSurfRect4[3] - m.m_SrcY) * ky + 0.5f);
    pOutRect4[0] = x0; pOutRect4[1] = y0;
    pOutRect4[2] = x1 - x0; pOutRect4[3] = y1 - y0;
}


LTCompMap LTComp_CurrentMap(int nSurfW, int nSurfH)
{
    int ow = 0, oh = 0;
    LTComp_OutputSize(&ow, &oh);

    LTCompMap m;
    // ⚠️ IN-GAME FOR ONE FRAME AFTER THE LAST CAMERA, not only the frame it
    // drew in: a single frame the game skips its RenderCamera must not jump
    // the picture into the zoomed menu box and back.
    const bool bInGame = g_nCamFrame != 0 && (g_nFrame - g_nCamFrame) <= 1;
    if (bInGame)
    {
        Fit(0.0f, 0.0f, (float)nSurfW, (float)nSurfH, ow, oh, m);
        m.m_bScene = true;
        m.m_bMenuScene = false;
        return m;
    }

    // ⚠️ THE MENU BOX, at the game's own offset: CInterfaceResMgr::
    // ScreenDimsChanged puts the 640x480 interface at ((w-640)/2, (h-480)/2),
    // integer division, for every menu, folder and loading screen. Nothing is
    // drawn outside it, so zooming it is the "fill the screen" for menus.
    // LT_NOMENUZOOM=1 shows the whole surface instead (says so).
    static const bool s_bNoZoom = []() {
        const bool b = getenv("LT_NOMENUZOOM") != 0;
        if (b) { printf("[COMP] LT_NOMENUZOOM=1 -- menus shown unzoomed\n"); fflush(stdout); }
        return b; }();
    if (!s_bNoZoom && nSurfW > 640 && nSurfH >= 480)
    {
        Fit((float)((nSurfW - 640) / 2), (float)((nSurfH - 480) / 2), 640.0f, 480.0f, ow, oh, m);
        Widen(m, nSurfW, nSurfH, ow, oh);
    }
    else
        Fit(0.0f, 0.0f, (float)nSurfW, (float)nSurfH, ow, oh, m);
    m.m_bScene = false;
    // ⚠️ ONE FRAME OF GRACE, as for the camera: a menu frame whose
    // RenderObjects is skipped must not flash the 2D's open (black) area.
    m.m_bMenuScene = g_nObjFrame != 0 && (g_nFrame - g_nObjFrame) <= 1;
    return m;
}


bool LTComp_Present(const unsigned char* pB, const unsigned char* pG,
                    int nLayerW, int nLayerH, bool bPresent,
                    unsigned char* pReadRGBA, int* pOutW, int* pOutH)
{
    if (!LTComp_Active() || !pB || !pG) return false;
    int ow = 0, oh = 0;
    LTComp_OutputSize(&ow, &oh);
    const LTCompMap m = LTComp_CurrentMap(nLayerW, nLayerH);
    if (bPresent) { g_LastMap = m; g_bHaveMap = true; }
    if (pOutW) *pOutW = ow;
    if (pOutH) *pOutH = oh;
    return g_fnPresent(pB, pG, nLayerW, nLayerH, m, ow, oh, bPresent, pReadRGBA);
}


void LTComp_FrameEnd()
{
    if (g_fnFrameEnd) g_fnFrameEnd();
    ++g_nFrame;
    g_bCamThisFrame = false;
    g_bObjThisFrame = false;
}


bool LTComp_OutputToSurface(float fX, float fY, int* pX, int* pY)
{
    if (!g_bHaveMap || g_LastMap.m_DstW <= 0 || g_LastMap.m_DstH <= 0) return false;
    const LTCompMap& m = g_LastMap;
    float x = m.m_SrcX + (fX - (float)m.m_DstX) * m.m_SrcW / (float)m.m_DstW;
    float y = m.m_SrcY + (fY - (float)m.m_DstY) * m.m_SrcH / (float)m.m_DstH;
    // Clamped into the mapped rectangle: a click on a bar lands on the edge
    // of the picture, never outside the surface the game knows about.
    if (x < m.m_SrcX) x = m.m_SrcX;
    if (y < m.m_SrcY) y = m.m_SrcY;
    if (x > m.m_SrcX + m.m_SrcW - 1.0f) x = m.m_SrcX + m.m_SrcW - 1.0f;
    if (y > m.m_SrcY + m.m_SrcH - 1.0f) y = m.m_SrcY + m.m_SrcH - 1.0f;
    *pX = (int)x;
    *pY = (int)y;
    return true;
}
