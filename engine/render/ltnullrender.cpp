// ----------------------------------------------------------------------- //
//
// MODULE  : ltnullrender.cpp
//
// PURPOSE : The null renderer. See ltnullrender.h for the three-category rule
//           this file is built on (state is truthful, drawing is a declared
//           no-op, everything else stays LT_ERROR).
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltnullrender.h"
#include "ltbindings.h"
#include "ltrecord.h"
#include "ltpcx.h"
#include "lt2d.h"
#include "ltwindow.h"
#include "ltcomp.h"
#include "ltfilemgr.h"
#include "ltstub.h"

#include "iltclient.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// ----------------------------------------------------------------------- //
// A surface.
//
// ⚠️ HSURFACE is `struct {int __nothing;}*` (ltbasedefs.h:88), an opaque
// pointer type -- so this object's own address IS the handle, and no lookup
// table is needed. Find() still walks the list to VALIDATE a handle before
// dereferencing it, because a stale handle from the game would otherwise be a
// wild pointer read with no diagnostic.
// ----------------------------------------------------------------------- //

struct CLTNullRender::Surface
{
    uint32                      m_Width;
    uint32                      m_Height;
    void*                       m_pUserData;
    float                       m_Alpha;
    bool                        m_bScreen;
    char                        m_Origin[64];   // what created it, for the census
    std::vector<unsigned char>  m_RGBA;         // always allocated, w*h*4
    // ⚠️ THE COMPOSITOR'S GREY LAYER (ltcomp.h), screen only and only while
    // the compositor runs: every 2D write lands in both, and where the 3D
    // shows this starts at 128 instead of 0.
    std::vector<unsigned char>  m_RGBA2;
    bool                        m_bOptimized;
    uint32                      m_TransColor;
    Surface*                    m_pNext;
};


// The mode list offered to the game. NOLF's UI is authored at 640x480 and the
// options screen enumerates whatever it is given, so a short honest list beats
// a long invented one.
static const struct { uint32 w, h, d; } kOfferedModes[] =
{
    { 640,  480,  32 },
    { 800,  600,  32 },
    { 1024, 768,  32 },
    { 1280, 960,  32 },
};
static const int kNumOfferedModes = (int)(sizeof(kOfferedModes) / sizeof(kOfferedModes[0]));

#define NULLRENDER_NAME "nullrender"


CLTNullRender::CLTNullRender()
    : m_bModeSet(false)
    , m_pSurfaces(0)
    , m_pScreen(0)
    , m_nSurfacesMade(0)
    , m_nSurfacesLive(0)
    , m_nBitmapsLoaded(0)
    , m_nBitmapsMissing(0)
    , m_nDrawOps(0)
    , m_nDrawOpsOverflow(0)
{
    memset(&m_Mode, 0, sizeof(m_Mode));
    memset(m_DrawOps, 0, sizeof(m_DrawOps));
}

CLTNullRender::~CLTNullRender()
{
    while (m_pSurfaces)
    {
        Surface* pNext = m_pSurfaces->m_pNext;
        delete m_pSurfaces;
        m_pSurfaces = pNext;
    }
}


// ----------------------------------------------------------------------- //
// Render mode -- STATE, so truthful.
// ----------------------------------------------------------------------- //

// ⚠️ THE RESOLUTION, DECIDED IN ONE PLACE -- and it has to be HERE, not at
// boot, because the GAME sets the mode and the game wins.
//
// The shipped autoexec.cfg says screenwidth 640 / screenheight 480, and
// CGameClientShell asks for exactly that during startup. Setting a different
// boot mode in the host achieved nothing: the window reported `640x480 open`
// because the game had already replaced the boot mode by the time the window
// was created. Overriding the request is the only place that works, short of
// writing to the install -- which is forbidden by rule.
//
// Default 1024x768 for development; LT_SCREEN=WxH to choose, and the contract
// gates pin LT_SCREEN=640x480 because the blessed frames are 640x480 pictures.
//
// ⚠️ It says so out loud. Silently handing the game a different surface than it
// asked for is the kind of kindness that costs a day later.
static bool LTScreen_Wanted(uint32* pW, uint32* pH)
{
    int w = 1024, h = 768;

    // ⚠️ UNDER THE COMPOSITOR THE SURFACE TAKES THE SCREEN'S SHAPE (HANDOVER
    // 5bq): 768 lines -- the interface size this port has always shown -- and
    // as many columns as the panel's aspect needs, so the game lays its HUD
    // out to the real edges and the 3D fills the screen. The resolution of
    // the 3D is NOT this; it renders at the output's native pixels.
    // LT_SCREEN still wins, exactly.
    if (!getenv("LT_SCREEN") && LTComp_Wanted())
    {
        // ⚠️ THE WINDOW'S SHAPE ONCE THERE IS ONE, the screen's before. A mode
        // the game sets later -- Options > Display > Apply, which then calls
        // its own ScreenDimsChanged -- so re-lays the HUD out to whatever the
        // window has become. At startup the window does not exist yet and
        // opens at the screen's shape.
        float a = 0.0f;
        int ow = 0, oh = 0;
        if (LTComp_Active() && LTComp_OutputSize(&ow, &oh) && oh > 0) a = (float)ow / (float)oh;
        else if (LTWindow_Wanted()) a = LTWindow_ScreenAspect();
        else if (LTComp_OutputSize(&ow, &oh) && oh > 0) a = (float)ow / (float)oh;
        if (a > 0.5f && a < 4.0f) w = (int)(h * a / 2.0f + 0.5f) * 2;
    }

    if (const char* p = getenv("LT_SCREEN"))
    {
        int a = 0, b = 0;
        if (sscanf(p, "%dx%d", &a, &b) == 2 &&
            a >= 320 && b >= 240 && a <= 4096 && b <= 4096)
        { w = a; h = b; }
        else
        {
            static bool bSaid = false;
            if (!bSaid) { bSaid = true; fprintf(stderr, "[LTRENDER] ⚠️  ignoring LT_SCREEN=%s\n", p); }
        }
    }
    *pW = (uint32)w; *pH = (uint32)h;
    return true;
}

bool CLTNullRender::SetMode(const RMode* pMode)
{
    if (!pMode) return false;

    RMode want = *pMode;
    {
        uint32 w = 0, h = 0;
        LTScreen_Wanted(&w, &h);
        if (w != want.m_Width || h != want.m_Height)
        {
            // Every change, not only the first: a mode the game sets after a
            // window resize is the re-layout, and it should be visible.
            static uint32 s_LastW = 0, s_LastH = 0;
            if (w != s_LastW || h != s_LastH)
            {
                s_LastW = w; s_LastH = h;
                printf("[LTRENDER] the game asked for %ux%u; using %ux%u (%s)\n",
                       want.m_Width, want.m_Height, w, h,
                       getenv("LT_SCREEN") ? "LT_SCREEN"
                       : LTComp_Wanted() ? "the compositor: 768 lines, the window's shape"
                       : "default");
                fflush(stdout);
            }
            want.m_Width = w; want.m_Height = h;
        }
    }
    pMode = &want;

    // ⚠️ REFUSE A DEGENERATE MODE. Accepting anything at all would look more
    // permissive and would be worse: the game divides by width and height when
    // it lays out the interface, and CGameClientShell has a documented
    // three-step fallback (GameClientShell.cpp:1037) that only stays reachable
    // if a refusal is possible at all.
    if (pMode->m_Width == 0 || pMode->m_Height == 0)
        return false;

    m_Mode = *pMode;
    m_Mode.m_pNext = 0;             // never hand back a borrowed list pointer
    m_bModeSet = true;

    // A mode change resizes the screen surface. Doing this here rather than
    // lazily means GetScreenSurface can never hand back dimensions from the
    // previous mode.
    if (m_pScreen)
    {
        m_pScreen->m_Width  = m_Mode.m_Width;
        m_pScreen->m_Height = m_Mode.m_Height;
        // ⚠️ The BUFFER has to follow the dimensions. Resizing one without the
        // other is a heap overflow on the next blit, and it would only fire
        // when someone changed resolution.
        m_pScreen->m_RGBA.assign((size_t)m_Mode.m_Width * m_Mode.m_Height * 4, 0);
        if (!m_pScreen->m_RGBA2.empty()) m_pScreen->m_RGBA2.assign(m_pScreen->m_RGBA.size(), 0);
    }

    LTRecord_Event("mode", "set %ux%ux%u hw=%d %s",
                   m_Mode.m_Width, m_Mode.m_Height, m_Mode.m_BitDepth,
                   (int)m_Mode.m_bHardware,
                   m_Mode.m_RenderDLL[0] ? m_Mode.m_RenderDLL : "(none)");
    return true;
}

bool CLTNullRender::GetMode(RMode* pMode) const
{
    if (!pMode || !m_bModeSet) return false;
    *pMode = m_Mode;
    pMode->m_pNext = 0;
    return true;
}

RMode* CLTNullRender::AllocModeList()
{
    RMode* pHead = 0;
    RMode* pTail = 0;

    for (int i = 0; i < kNumOfferedModes; ++i)
    {
        RMode* p = (RMode*)calloc(1, sizeof(RMode));
        if (!p) break;

        p->m_bHardware = LTTRUE;
        p->m_Width     = kOfferedModes[i].w;
        p->m_Height    = kOfferedModes[i].h;
        p->m_BitDepth  = kOfferedModes[i].d;
        strcpy(p->m_RenderDLL,    NULLRENDER_NAME);
        strcpy(p->m_InternalName, NULLRENDER_NAME);
        snprintf(p->m_Description, sizeof(p->m_Description),
                 "Null renderer %ux%u", p->m_Width, p->m_Height);
        p->m_pNext = 0;

        if (pTail) pTail->m_pNext = p; else pHead = p;
        pTail = p;
    }

    return pHead;
}

void CLTNullRender::FreeModeList(RMode* pModes)
{
    while (pModes)
    {
        RMode* pNext = pModes->m_pNext;
        free(pModes);
        pModes = pNext;
    }
}

void CLTNullRender::ShutdownRender(uint32 flags)
{
    LTRecord_Event("mode", "shutdown flags=%u", flags);
    m_bModeSet = false;
}


// ----------------------------------------------------------------------- //
// Surfaces -- STATE, so truthful. No pixels; every queryable property real.
// ----------------------------------------------------------------------- //

CLTNullRender::Surface* CLTNullRender::Find(HSURFACE h) const
{
    if (!h) return 0;
    for (Surface* p = m_pSurfaces; p; p = p->m_pNext)
        if ((HSURFACE)p == h) return p;
    return 0;      // stale or foreign handle -- refuse rather than dereference
}

HSURFACE CLTNullRender::CreateSurface(uint32 w, uint32 h, const char* pOrigin)
{
    // A zero-sized surface is almost always a bug upstream, and returning one
    // turns it into a division by zero somewhere else.
    if (w == 0 || h == 0)
    {
        LTRecord_Event("surf", "refused 0-sized from %s", pOrigin ? pOrigin : "?");
        return 0;
    }

    Surface* p = new Surface;
    p->m_Width      = w;
    p->m_Height     = h;
    p->m_pUserData  = 0;
    p->m_Alpha      = 1.0f;
    p->m_bScreen    = false;
    p->m_bOptimized = false;
    p->m_TransColor = 0;
    p->m_pNext      = m_pSurfaces;

    // ⚠️ ZEROED, and every surface gets one. A surface without pixels used to
    // be fine because nothing drew; now that the compositor is real, a missing
    // buffer would be a null deref on the first blit into it.
    p->m_RGBA.assign((size_t)w * h * 4, 0);
    strncpy(p->m_Origin, pOrigin ? pOrigin : "?", sizeof(p->m_Origin) - 1);
    p->m_Origin[sizeof(p->m_Origin) - 1] = 0;

    m_pSurfaces = p;
    ++m_nSurfacesMade;
    ++m_nSurfacesLive;

    return (HSURFACE)p;
}

HSURFACE CLTNullRender::CreateSurfaceFromBitmap(const char* pName)
{
    if (!pName || !pName[0]) return 0;

    CLTPcx pcx;
    if (!pcx.LoadThroughFileMgr(LTGetFileMgr(), pName))
    {
        // ⚠️ 0, not a placeholder surface. Handing back a 1x1 or a guessed size
        // would put an invented number into the game's layout arithmetic, which
        // is category 1 answered with a guess -- the thing this module exists
        // not to do. The caller's own fallback stays reachable instead.
        ++m_nBitmapsMissing;
        LTRecord_Event("bmp", "MISSING %s", pName);
        return 0;
    }

    Surface* p = (Surface*)CreateSurface(pcx.GetWidth(), pcx.GetHeight(), "bitmap");
    if (!p) return 0;

    p->m_RGBA.assign(pcx.GetRGBA(), pcx.GetRGBA() + pcx.GetRGBASize());
    strncpy(p->m_Origin, pName, sizeof(p->m_Origin) - 1);
    p->m_Origin[sizeof(p->m_Origin) - 1] = 0;

    ++m_nBitmapsLoaded;
    LTRecord_Event("bmp", "%s %ux%u planes=%u", pName,
                   pcx.GetWidth(), pcx.GetHeight(), pcx.GetPlanes());

    return (HSURFACE)p;
}


const unsigned char* CLTNullRender::GetPixels(HSURFACE h) const
{
    Surface* p = Find(h);
    if (!p || p->m_RGBA.empty()) return 0;
    return &p->m_RGBA[0];
}


LT2DSurf CLTNullRender::GetView(HSURFACE h) const
{
    LT2DSurf v = { 0, 0, 0 };

    Surface* p = Find(h);
    if (!p || p->m_RGBA.empty()) return v;      // unknown handle -> inert view

    v.m_pRGBA  = const_cast<unsigned char*>(&p->m_RGBA[0]);
    v.m_Width  = (int)p->m_Width;
    v.m_Height = (int)p->m_Height;
    return v;
}


// ⚠️ THE SECOND LAYER, for the screen only, and only under the compositor. It
// starts as a copy of the first -- whatever is already there is opaque, and
// an opaque pixel is the same in both layers.
LT2DSurf CLTNullRender::GetTwinView(HSURFACE h) const
{
    LT2DSurf v = { 0, 0, 0 };
    Surface* p = Find(h);
    if (!p || !p->m_bScreen || p->m_RGBA.empty() || !LTComp_Active()) return v;
    if (p->m_RGBA2.size() != p->m_RGBA.size()) p->m_RGBA2 = p->m_RGBA;
    v.m_pRGBA  = &p->m_RGBA2[0];
    v.m_Width  = (int)p->m_Width;
    v.m_Height = (int)p->m_Height;
    return v;
}

const unsigned char* CLTNullRender::GetTwinPixels() const
{
    if (!m_pScreen || m_pScreen->m_RGBA2.size() != m_pScreen->m_RGBA.size()) return 0;
    return m_pScreen->m_RGBA2.empty() ? 0 : &m_pScreen->m_RGBA2[0];
}

// ⚠️ "THE 3D IS HERE": black 0 in the first layer, grey 128 in the second --
// M = 1, A = 0 (ltcomp.h). What the old path did by overwriting the surface
// with the read-back 3D, as a rule the GPU can undo at any resolution.
void CLTNullRender::OpenScreenForScene()
{
    if (!m_pScreen || m_pScreen->m_RGBA.empty()) return;
    LT2DSurf t = GetTwinView((HSURFACE)m_pScreen);
    if (!t.m_pRGBA) return;
    const size_t n = m_pScreen->m_RGBA.size() / 4;
    unsigned int* pB = (unsigned int*)&m_pScreen->m_RGBA[0];
    unsigned int* pG = (unsigned int*)t.m_pRGBA;
    // RGBA in memory: r, g, b, a -> little-endian word a<<24 | b<<16 | g<<8 | r.
    const unsigned int kBlack = 0xFF000000u, kGrey = 0xFF808080u;
    for (size_t i = 0; i < n; ++i) { pB[i] = kBlack; pG[i] = kGrey; }
}


void CLTNullRender::OpenScreenRect(int x, int y, int w, int h)
{
    if (!m_pScreen || m_pScreen->m_RGBA.empty()) return;
    LT2DSurf t = GetTwinView((HSURFACE)m_pScreen);
    if (!t.m_pRGBA) return;
    const int W = (int)m_pScreen->m_Width, H = (int)m_pScreen->m_Height;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > W ? W : x + w, y1 = y + h > H ? H : y + h;
    unsigned int* pB = (unsigned int*)&m_pScreen->m_RGBA[0];
    unsigned int* pG = (unsigned int*)t.m_pRGBA;
    const unsigned int kBlack = 0xFF000000u, kGrey = 0xFF808080u;
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) { pB[(size_t)yy * W + xx] = kBlack; pG[(size_t)yy * W + xx] = kGrey; }
}


bool CLTNullRender::Optimize(HSURFACE h, uint32 transColor)
{
    Surface* p = Find(h);
    if (!p) return false;
    p->m_bOptimized = true;
    p->m_TransColor = transColor;
    return true;
}

bool CLTNullRender::Unoptimize(HSURFACE h)
{
    Surface* p = Find(h);
    if (!p) return false;
    p->m_bOptimized = false;
    return true;
}

bool CLTNullRender::IsOptimized(HSURFACE h) const
{
    Surface* p = Find(h);
    return p && p->m_bOptimized;
}

uint32 CLTNullRender::GetTransColor(HSURFACE h) const
{
    Surface* p = Find(h);
    return p ? p->m_TransColor : 0;
}


// ⚠️ STANDS IN FOR THE 3D SCENE, and says so.
//
// The game clears the screen only 3 times in 600 frames, because in the real
// engine the 3D scene repaints the entire viewport every frame -- the menu
// background is a camera looking at sprites and models (HANDOVER.md §2).
// With a null 3D renderer nothing overwrites the previous frame, so anything
// that MOVES smears: the mouse cursor leaves a trail of itself.
//
// Clearing here is the engine's frame setup doing what the engine does, not a
// workaround -- but it is only correct while the 3D side draws nothing. Once
// the scene renders, this becomes redundant and should go.
void CLTNullRender::ClearScreenForFrame()
{
    if (!m_pScreen || m_pScreen->m_RGBA.empty()) return;
    memset(&m_pScreen->m_RGBA[0], 0, m_pScreen->m_RGBA.size());
    if (m_pScreen->m_RGBA2.size() == m_pScreen->m_RGBA.size())
        memset(&m_pScreen->m_RGBA2[0], 0, m_pScreen->m_RGBA2.size());
}


void CLTNullRender::PresentToWindow() const
{
    if (!m_pScreen || m_pScreen->m_RGBA.empty()) return;
    // ⚠️ THE COMPOSITOR PRESENTS ON THE GPU (HANDOVER 5bq); the CoreGraphics
    // blit below is the old path, kept for LT_COMPOSITOR=0 and the software
    // renderer.
    if (LTComp_Active())
    {
        LT2DSurf t = GetTwinView((HSURFACE)m_pScreen);
        if (t.m_pRGBA &&
            LTComp_Present(&m_pScreen->m_RGBA[0], t.m_pRGBA,
                           (int)m_pScreen->m_Width, (int)m_pScreen->m_Height,
                           true, 0, 0, 0))
            return;
    }
    LTWindow_Present(&m_pScreen->m_RGBA[0],
                     (int)m_pScreen->m_Width, (int)m_pScreen->m_Height);
}


bool CLTNullRender::DumpScreenPPM(const char* pPath) const
{
    if (!m_pScreen || m_pScreen->m_RGBA.empty() || !pPath) return false;

    // ⚠️ UNDER THE COMPOSITOR THE PICTURE IS NOT IN THE SURFACE -- the 3D lives
    // on the GPU -- so a dump is the composite, read back at OUTPUT size. That
    // is what the player sees; the surface alone would be a frame of holes.
    if (LTComp_Active())
    {
        LT2DSurf t = GetTwinView((HSURFACE)m_pScreen);
        int ow = 0, oh = 0;
        LTComp_OutputSize(&ow, &oh);
        if (t.m_pRGBA && ow > 0 && oh > 0)
        {
            std::vector<unsigned char> rgba((size_t)ow * oh * 4);
            if (LTComp_Present(&m_pScreen->m_RGBA[0], t.m_pRGBA,
                               (int)m_pScreen->m_Width, (int)m_pScreen->m_Height,
                               false, &rgba[0], &ow, &oh))
            {
                FILE* f = fopen(pPath, "wb");
                if (!f) return false;
                fprintf(f, "P6\n%d %d\n255\n", ow, oh);
                for (size_t i = 0; i < (size_t)ow * oh; ++i) fwrite(&rgba[i * 4], 1, 3, f);
                fclose(f);
                return true;
            }
        }
    }

    FILE* f = fopen(pPath, "wb");
    if (!f) return false;

    // Binary P6. PIL reads it, so harness/framediff.py takes it directly --
    // no image library needed on this side.
    fprintf(f, "P6\n%u %u\n255\n", m_pScreen->m_Width, m_pScreen->m_Height);

    const unsigned char* p = &m_pScreen->m_RGBA[0];
    for (uint32 i = 0; i < m_pScreen->m_Width * m_pScreen->m_Height; ++i)
        fwrite(p + (size_t)i * 4, 1, 3, f);     // RGBA -> RGB

    fclose(f);
    return true;
}


HSURFACE CLTNullRender::GetScreenSurface()
{
    if (!m_pScreen)
    {
        // ⚠️ The screen surface only exists once a mode is set. Handing one out
        // before that would be inventing a resolution, and the game's very
        // first engine call on the client path IS GetScreenSurface (census
        // entry 2) -- so this refusal is on a hot path and has to be correct.
        if (!m_bModeSet) return 0;

        m_pScreen = (Surface*)CreateSurface(m_Mode.m_Width, m_Mode.m_Height, "screen");
        if (m_pScreen) m_pScreen->m_bScreen = true;
    }
    return (HSURFACE)m_pScreen;
}

bool CLTNullRender::DeleteSurface(HSURFACE h)
{
    Surface* pFound = Find(h);
    if (!pFound) return false;

    // Deleting the screen surface would leave m_pScreen dangling.
    if (pFound->m_bScreen)
    {
        LTRecord_Event("surf", "refused delete of screen surface");
        return false;
    }

    Surface** pp = &m_pSurfaces;
    while (*pp && *pp != pFound) pp = &(*pp)->m_pNext;
    if (!*pp) return false;

    *pp = pFound->m_pNext;
    delete pFound;
    --m_nSurfacesLive;
    return true;
}

bool CLTNullRender::GetDims(HSURFACE h, uint32* pW, uint32* pH) const
{
    Surface* p = Find(h);
    if (!p)
    {
        // ⚠️ THE CALLER'S VALUES ARE LEFT ALONE ON FAILURE, and the reason is
        // in the game's own code:
        //
        //     uint32 dwWidth = 640;
        //     uint32 dwHeight = 480;
        //     g_pLTClient->GetSurfaceDims(g_pLTClient->GetScreenSurface(),
        //                                 &dwWidth, &dwHeight);
        //     -- GameClientShell.cpp:1131
        //
        // It seeds a sensible default IMMEDIATELY BEFORE the call, because
        // GetSurfaceDims returns void and there is no other way to survive a
        // failure. Zeroing the outputs (which this did first) overwrites that
        // fallback with 0 and turns a recoverable miss into a division by zero
        // somewhere downstream.
        //
        // Leaving them untouched is not "failing silently": the miss is
        // recorded, counted, and shown in the census. Silence would be doing
        // neither.
        LTRecord_Event("surf", "dims of unknown surface");
        return false;
    }
    if (pW) *pW = p->m_Width;
    if (pH) *pH = p->m_Height;
    return true;
}

void* CLTNullRender::GetUserData(HSURFACE h) const
{
    Surface* p = Find(h);
    return p ? p->m_pUserData : 0;
}

void CLTNullRender::SetUserData(HSURFACE h, void* pData)
{
    Surface* p = Find(h);
    if (p) p->m_pUserData = pData;
}

bool CLTNullRender::GetAlpha(HSURFACE h, float& a) const
{
    Surface* p = Find(h);
    if (!p) { a = 0.0f; return false; }
    a = p->m_Alpha;
    return true;
}

bool CLTNullRender::SetAlpha(HSURFACE h, float a)
{
    Surface* p = Find(h);
    if (!p) return false;
    p->m_Alpha = a;
    return true;
}


// ----------------------------------------------------------------------- //
// Drawing -- a DECLARED no-op, counted.
// ----------------------------------------------------------------------- //

void CLTNullRender::Draw(const char* pOp)
{
    for (int i = 0; i < m_nDrawOps; ++i)
    {
        if (m_DrawOps[i].m_pName == pOp)   // literal pointers, so == is enough
        {
            ++m_DrawOps[i].m_nCalls;
            return;
        }
    }

    if (m_nDrawOps >= kMaxDrawOps) { ++m_nDrawOpsOverflow; return; }

    m_DrawOps[m_nDrawOps].m_pName  = pOp;
    m_DrawOps[m_nDrawOps].m_nCalls = 1;
    ++m_nDrawOps;

    // First occurrence only, same discipline as the stub census: these sit on
    // per-frame paths and one line each is the difference between a readable
    // record and a flood.
    LTRecord_Event("draw", "%s", pOp);
}


static int LT2D_TraceEnabled()
{
    static int s = -1;
    if (s < 0) s = getenv("LT_TRACE_2D") ? 1 : 0;
    return s;
}

const char* CLTNullRender::DescribeSurface(HSURFACE h) const
{
    if (!h) return "(null)";
    Surface* p = Find(h);
    if (!p) return "(unknown)";
    if (p->m_bScreen) return "SCREEN";
    return p->m_Origin;
}

void CLTNullRender::TraceDraw(const char* pOp, HSURFACE hDest, HSURFACE hSrc)
{
    TraceDrawKeyed(pOp, hDest, hSrc, 0, false);
}

void CLTNullRender::TraceDrawKeyed(const char* pOp, HSURFACE hDest, HSURFACE hSrc,
                                   uint32 key, bool bHasKey)
{
    Draw(pOp);
    if (!LT2D_TraceEnabled()) return;

    Surface* pd = Find(hDest);

    // How much of the SCREEN is non-black right now. "the op ran" and "the op
    // put something on screen" are different claims; this measures the second.
    unsigned int nLit = 0;
    if (m_pScreen && !m_pScreen->m_RGBA.empty())
    {
        const unsigned char* q = &m_pScreen->m_RGBA[0];
        for (uint32 i = 0; i < m_pScreen->m_Width * m_pScreen->m_Height; ++i)
            if (q[i*4] || q[i*4+1] || q[i*4+2]) ++nLit;
    }

    Surface* ps = Find(hSrc);
    char keybuf[32];
    if (bHasKey) snprintf(keybuf, sizeof(keybuf), "key=%08X", key);
    else         snprintf(keybuf, sizeof(keybuf), "%s", "-");

    // What colour does the source actually START with? If a "transparent" blit
    // is painting black over everything, the answer is either that the key does
    // not match the fill or that the source was never filled.
    unsigned int srcTopLeft = 0;
    if (ps && !ps->m_RGBA.empty())
        srcTopLeft = ((unsigned int)ps->m_RGBA[0] << 16)
                   | ((unsigned int)ps->m_RGBA[1] << 8) | ps->m_RGBA[2];

    fprintf(stderr, "[LT2D] %-34s dest=%-10s src=%-26s %ux%u px0=%06X %-16s screenlit=%u\n",
            pOp, DescribeSurface(hDest),
            hSrc ? DescribeSurface(hSrc) : "-",
            ps ? ps->m_Width : 0, ps ? ps->m_Height : 0,
            srcTopLeft, keybuf, nLit);
}


void CLTNullRender::DumpCensus() const
{
    fprintf(stderr, "\n===== null renderer =====\n");

    if (m_bModeSet)
        fprintf(stderr, "  mode      : %ux%ux%u  hardware=%d  dll=%s\n",
                m_Mode.m_Width, m_Mode.m_Height, m_Mode.m_BitDepth,
                (int)m_Mode.m_bHardware,
                m_Mode.m_RenderDLL[0] ? m_Mode.m_RenderDLL : "(none)");
    else
        fprintf(stderr, "  mode      : NONE SET\n");

    fprintf(stderr, "  surfaces  : %u created, %u still live\n",
            m_nSurfacesMade, m_nSurfacesLive);
    fprintf(stderr, "  bitmaps   : %u loaded, %u missing\n",
            m_nBitmapsLoaded, m_nBitmapsMissing);

    fprintf(stderr, "  draw calls: %d distinct op(s)\n", m_nDrawOps);
    for (int i = 0; i < m_nDrawOps; ++i)
        fprintf(stderr, "    %-36s %u\n",
                m_DrawOps[i].m_pName, m_DrawOps[i].m_nCalls);
    if (m_nDrawOpsOverflow)
        fprintf(stderr, "    (+%u calls to ops past the census cap)\n",
                m_nDrawOpsOverflow);

    // ⚠️ ZERO IS NOT A MEASUREMENT UNTIL SOMETHING HAS MEASURED IT.
    //
    // "draw calls: 0 distinct op(s)" reads exactly like the finding "this game
    // does no drawing" while actually meaning "the game never got far enough to
    // draw". Those are opposite conclusions from identical output, and the
    // background doc §3.5 is a list of times this project believed the first
    // one. So the census says which it is instead of leaving it to be inferred.
    if (m_nDrawOps == 0)
    {
        fprintf(stderr,
            "\n  ⚠️  NOT EXERCISED. Zero here does NOT mean the game draws\n"
            "      nothing -- it means the run ended before any drawing. The\n"
            "      null renderer's draw census is only a measurement once the\n"
            "      game reaches a frame.\n");
    }

    fprintf(stderr, "=========================\n");
    fflush(stderr);
}

void CLTNullRender::RecordTotals() const
{
    // ⚠️ Draw COUNTS, not just which ops were reached. Two runs that call the
    // same ops a different number of times are different runs, and the "draw"
    // event lines alone cannot tell them apart -- exactly the reason the file
    // system records counts as well as names (harness/README.md).
    for (int i = 0; i < m_nDrawOps; ++i)
        LTRecord_Event("drawn", "%s %u", m_DrawOps[i].m_pName, m_DrawOps[i].m_nCalls);

    LTRecord_Event("count", "bitmaps-loaded %u",  m_nBitmapsLoaded);
    LTRecord_Event("count", "bitmaps-missing %u", m_nBitmapsMissing);
    LTRecord_Event("count", "surfaces-made %u", m_nSurfacesMade);
    LTRecord_Event("count", "surfaces-live %u", m_nSurfacesLive);
}


// ----------------------------------------------------------------------- //

static CLTNullRender g_NullRender;

CLTNullRender* LTGetNullRender()
{
    return &g_NullRender;
}


// ----------------------------------------------------------------------- //
// ILTClient wiring.
//
// ⚠️ THESE ARE FUNCTION-POINTER MEMBERS, NOT VIRTUALS (HANDOVER.md §4.5).
// A method named GetScreenSurface on a subclass of ILTClient compiles, is never
// called, and produces no diagnostic at all. They have to be ASSIGNED, and
// assigned AFTER LTSkel_WireFnPtrs_ILTClient has installed the LT_ERROR
// defaults, or the defaults win.
//
// The one safety net worth noting: because these are assignments and not
// overrides, a signature that does not match exactly is a COMPILE error rather
// than a silent non-override. That is the opposite of the virtual case, and it
// is why writing them out by hand here is acceptable.
// ----------------------------------------------------------------------- //

static LTRESULT NR_SetRenderMode(RMode* pMode)
{
    LTIMPL("ILTClient::SetRenderMode");
    return LTGetNullRender()->SetMode(pMode) ? LT_OK : LT_KEPTSAMEMODE;
}

static LTRESULT NR_GetRenderMode(RMode* pMode)
{
    LTIMPL("ILTClient::GetRenderMode");
    return LTGetNullRender()->GetMode(pMode) ? LT_OK : LT_ERROR;
}

static RMode* NR_GetRenderModes()
{
    LTIMPL("ILTClient::GetRenderModes");
    return LTGetNullRender()->AllocModeList();
}

static void NR_RelinquishRenderModes(RMode* pModes)
{
    LTIMPL("ILTClient::RelinquishRenderModes");
    LTGetNullRender()->FreeModeList(pModes);
}

// ----------------------------------------------------------------------- //
// ⚠️ WriteConfigFile IS REFUSED, DELIBERATELY AND LOUDLY.
//
// The game calls g_pLTClient->WriteConfigFile("autoexec.cfg") from ELEVEN menu
// screens, with a RELATIVE filename. The file manager's lowest-priority trees
// are the game data directories, so an implementation that resolved that name
// the same way reads do would write straight into GOTY-disc2/Game/autoexec.cfg
// -- one of the two REAL INSTALLS this project is forbidden to modify
// (PORT_PLAN.md §5).
//
// The generic stub already returned LT_ERROR, which was safe but silent about
// why. This one says so, and records it, so the next person to implement config
// writing meets the hazard instead of discovering it afterwards.
//
// When it IS implemented: write to a per-user directory, and refuse any path
// that resolves inside a mounted tree. Never make this relative-path-relative.
// ----------------------------------------------------------------------- //

static LTRESULT NR_WriteConfigFile(char* pFilename)
{
    LTIMPL("ILTClient::WriteConfigFile");
    // ⚠️ NOW IMPLEMENTED -- in the PLAYER'S directory, never the install
    // (~/Library/Application Support/NOLF1, user decision 2026-10-17; only
    // the file name is kept). See LTConfig_Write. Still refused where the
    // player's config is off (headless runs without LT_CONFIGDIR).
    if (LTConfig_Enabled())
        return LTConfig_Write(pFilename) ? LT_OK : LT_ERROR;
    LTRecord_Event("cfg", "REFUSED write of %s", pFilename ? pFilename : "(null)");

    static int s_bSaid = 0;
    if (!s_bSaid)
    {
        s_bSaid = 1;
        fprintf(stderr, "[LTCFG] ⚠️  refusing to write \"%s\" -- a relative config\n"
                        "        write would land in the real game install.\n",
                pFilename ? pFilename : "(null)");
    }
    return LT_ERROR;
}

static LTRESULT NR_ShutdownRender(uint32 flags)
{
    LTIMPL("ILTClient::ShutdownRender");
    LTGetNullRender()->ShutdownRender(flags);
    return LT_OK;
}

// --- frame markers: state, and cheap to keep honest ------------------- //

namespace { bool g_bHostPresents = false; }
void LTNullRender_SetHostPresents(bool b) { g_bHostPresents = b; }

static LTRESULT NR_FlipScreen(uint32 /*flags*/)
{
    LTGetNullRender()->Draw("FlipScreen");

    // ⚠️ THIS IS WHERE "presents to nowhere" STOPS BEING TRUE -- but only when
    // a window exists. With LT_WINDOW unset the call costs one comparison and
    // the run stays headless and deterministic, which the frame contract needs.
    // ⚠️ Not inside the host's PostUpdate: the host presents that frame
    // itself, right after (see LTNullRender_SetHostPresents).
    if (LTWindow_Wanted() && !g_bHostPresents)
        LTGetNullRender()->PresentToWindow();

    return LT_OK;
}

static LTRESULT NR_ClearScreen(LTRect* pRect, uint32 /*flags*/)
{
    LTGetNullRender()->Draw("ClearScreen");

    // ⚠️ This used to be a recorded no-op, which was wrong: ClearScreen is not
    // drawing, it is the frame's starting state, and "returned LT_OK and did
    // nothing" is exactly the lie this module exists to avoid.
    LT2DSurf d = LTGetNullRender()->GetView(LTGetNullRender()->GetScreenSurface());
    if (!d.m_pRGBA) return LT_ERROR;

    LT2DRect r;
    if (pRect) { r.left = pRect->left; r.top = pRect->top;
                 r.right = pRect->right; r.bottom = pRect->bottom; }

    LT2D::FillRect(d, pRect ? &r : 0, 0x000000);
    LT2DSurf d2 = LTGetNullRender()->GetTwinView(LTGetNullRender()->GetScreenSurface());
    if (d2.m_pRGBA) LT2D::FillRect(d2, pRect ? &r : 0, 0x000000);
    return LT_OK;
}

static LTRESULT NR_Start3D()            { LTGetNullRender()->Draw("Start3D");            return LT_OK; }
static LTRESULT NR_End3D()              { LTGetNullRender()->Draw("End3D");              return LT_OK; }
// ⚠️ THE OPTIMIZED-2D STATE. Two settings the game changes between blits and
// expects the engine to honour -- see LT2D::BlitBlend for what ignoring them
// did to every line of on-screen text.
//
// ⚠️ RESET BY StartOptimized2D, not carried across blocks. The game sets a
// blend, draws, and sets it back; a block that inherited the previous block's
// state would be correct only for as long as every drawing site remembered to
// restore it, and one that forgot would tint the rest of the frame.
namespace
{
    // ⚠️ THE LOCK BETWEEN THE SDK'S ENUM AND THE COMPOSITOR'S MIRROR OF IT.
    // lt2d.h cannot include ltbasedefs.h without giving up being unit-testable
    // in isolation, so it keeps its own copy -- and this file, which includes
    // both, is the one place that can prove they still agree. Renumbering
    // either side is now a compile error instead of a wrong blend mode.
    static_assert((int)LTSURFACEBLEND_ALPHA     == (int)LT2D::kBlend_Alpha,     "blend enum drift");
    static_assert((int)LTSURFACEBLEND_SOLID     == (int)LT2D::kBlend_Solid,     "blend enum drift");
    static_assert((int)LTSURFACEBLEND_ADD       == (int)LT2D::kBlend_Add,       "blend enum drift");
    static_assert((int)LTSURFACEBLEND_MULTIPLY  == (int)LT2D::kBlend_Multiply,  "blend enum drift");
    static_assert((int)LTSURFACEBLEND_MULTIPLY2 == (int)LT2D::kBlend_Multiply2, "blend enum drift");
    static_assert((int)LTSURFACEBLEND_MASK      == (int)LT2D::kBlend_Mask,      "blend enum drift");
    static_assert((int)LTSURFACEBLEND_MASKADD   == (int)LT2D::kBlend_MaskAdd,   "blend enum drift");

    int          g_n2DBlend    = LTSURFACEBLEND_ALPHA;
    unsigned int g_n2DColor    = 0x00FFFFFFu;
}

// Are the optimized-2D blend modes honoured? See NR_DrawSurfaceToSurface.
static bool LT2DBlendEnabled()
{
    static const bool b = []() {
        const bool bOff = (getenv("LT_NO2DBLEND") != 0);
        if (bOff) { printf("[LT2D] LT_NO2DBLEND=1 -- 2D blend modes ignored, every blit an opaque copy (old)\n"); fflush(stdout); }
        return !bOff; }();
    return b;
}

static LTRESULT NR_StartOptimized2D()
{
    LTGetNullRender()->Draw("StartOptimized2D");
    g_n2DBlend = LTSURFACEBLEND_ALPHA;
    g_n2DColor = 0x00FFFFFFu;
    return LT_OK;
}

static LTRESULT NR_EndOptimized2D()     { LTGetNullRender()->Draw("EndOptimized2D");     return LT_OK; }

static LTRESULT NR_SetOptimized2DBlend(LTSurfaceBlend blend)
{
    LTIMPL("ILTClient::SetOptimized2DBlend");
    g_n2DBlend = (int)blend;
    return LT_OK;
}

static LTRESULT NR_GetOptimized2DBlend(LTSurfaceBlend &blend)
{
    LTIMPL("ILTClient::GetOptimized2DBlend");
    blend = (LTSurfaceBlend)g_n2DBlend;
    return LT_OK;
}

static LTRESULT NR_SetOptimized2DColor(HLTCOLOR hColor)
{
    LTIMPL("ILTClient::SetOptimized2DColor");
    g_n2DColor = LT2D::ColorRGB((unsigned int)(uintptr_t)hColor);
    return LT_OK;
}

static LTRESULT NR_GetOptimized2DColor(HLTCOLOR &hColor)
{
    LTIMPL("ILTClient::GetOptimized2DColor");
    hColor = (HLTCOLOR)(uintptr_t)g_n2DColor;
    return LT_OK;
}

// --- surfaces --------------------------------------------------------- //

static HSURFACE NR_GetScreenSurface()
{
    LTIMPL("ILTClient::GetScreenSurface");
    return LTGetNullRender()->GetScreenSurface();
}

static HSURFACE NR_CreateSurface(uint32 w, uint32 h)
{
    return LTGetNullRender()->CreateSurface(w, h, "CreateSurface");
}

static LTRESULT NR_DeleteSurface(HSURFACE hSurface)
{
    return LTGetNullRender()->DeleteSurface(hSurface) ? LT_OK : LT_ERROR;
}

static void NR_GetSurfaceDims(HSURFACE hSurf, uint32* pWidth, uint32* pHeight)
{
    LTGetNullRender()->GetDims(hSurf, pWidth, pHeight);
}

static void* NR_GetSurfaceUserData(HSURFACE hSurface)
{
    return LTGetNullRender()->GetUserData(hSurface);
}

static void NR_SetSurfaceUserData(HSURFACE hSurface, void* pUserData)
{
    LTGetNullRender()->SetUserData(hSurface, pUserData);
}

static LTRESULT NR_GetSurfaceAlpha(HSURFACE hSurface, float& alpha)
{
    return LTGetNullRender()->GetAlpha(hSurface, alpha) ? LT_OK : LT_ERROR;
}

static LTRESULT NR_SetSurfaceAlpha(HSURFACE hSurface, float alpha)
{
    return LTGetNullRender()->SetAlpha(hSurface, alpha) ? LT_OK : LT_ERROR;
}

// ----------------------------------------------------------------------- //
// THE 2D COMPOSITOR, wired.
//
// ⚠️ These have MOVED OUT OF CATEGORY 2. They are no longer declared no-ops --
// they draw. Everything below converts LT2's types to lt2d's and defers; no
// pixel logic lives here, so the blitter stays independently testable.
//
// The alpha argument is the SOURCE surface's, because the SDK says so:
// "alpha is only used on optimized surfaces, and only when they are blitted to
// the screen" (iltclient.h:616).
// ----------------------------------------------------------------------- //

static LT2DRect ToRect(const LTRect* p)
{
    LT2DRect r = { p->left, p->top, p->right, p->bottom };
    return r;
}

static float SrcAlpha(HSURFACE hSrc)
{
    float a = 1.0f;
    LTGetNullRender()->GetAlpha(hSrc, a);
    return a;
}

static LTRESULT NR_FillRect(HSURFACE hDest, LTRect* pRect, HLTCOLOR hColor)
{
    LTGetNullRender()->TraceDraw("FillRect", hDest, 0);

    LT2DSurf d = LTGetNullRender()->GetView(hDest);
    if (!d.m_pRGBA) return LT_ERROR;

    LT2DRect r;
    if (pRect) r = ToRect(pRect);
    LT2D::FillRect(d, pRect ? &r : 0, hColor);
    LT2DSurf d2 = LTGetNullRender()->GetTwinView(hDest);
    if (d2.m_pRGBA) LT2D::FillRect(d2, pRect ? &r : 0, hColor);
    return LT_OK;
}

static LTRESULT NR_DrawSurfaceToSurface(HSURFACE hDest, HSURFACE hSrc,
                                        LTRect* pSrcRect, int destX, int destY)
{
    LTGetNullRender()->TraceDraw("DrawSurfaceToSurface", hDest, hSrc);

    LT2DSurf d = LTGetNullRender()->GetView(hDest);
    LT2DSurf s = LTGetNullRender()->GetView(hSrc);
    if (!d.m_pRGBA || !s.m_pRGBA) return LT_ERROR;

    LT2DRect r;
    if (pSrcRect) r = ToRect(pSrcRect);
    // ⚠️ THE BLEND MODES ARE HONOURED, and every in-game text box depended on
    // it. NOLF draws its text white on BLACK into a surface and composites it
    // in two passes -- MASK for the shadow, ADD for the tinted glyphs
    // (PopupText.cpp, MissionText.cpp:128, Subtitle.cpp) -- so an opaque copy
    // paints the black background too: the box behind every intel text,
    // subtitle and pickup message.
    //
    // ⚠️ This was OFF until 2026-10-14 because `CBaseFolder::Draw` blits the
    // menu help text under MASK *alone* (BaseFolder.cpp:387) and that text
    // "vanished" -- measured when the menu's help bar was not drawn, so the
    // black glyphs landed on black. Since the menu sprites (re-blessed
    // 2026-10-06) the bar is there and MASK gives dark text on it, as the
    // SDK defines MASK: (1 - src) * dest (ltbasedefs.h:359).
    //
    // LT_NO2DBLEND=1 = the old opaque copy (announces itself).
    const bool bBlend = LT2DBlendEnabled();
    LT2DSurf d2 = LTGetNullRender()->GetTwinView(hDest);
    for (int k = 0; k < (d2.m_pRGBA ? 2 : 1); ++k)
    {
        const LT2DSurf& dd = k ? d2 : d;
        if (bBlend)
            LT2D::BlitBlend(dd, s, pSrcRect ? &r : 0, destX, destY, false, 0,
                            SrcAlpha(hSrc), g_n2DBlend, g_n2DColor);
        else
            LT2D::Blit(dd, s, pSrcRect ? &r : 0, destX, destY, false, 0, SrcAlpha(hSrc));
    }
    return LT_OK;
}

static LTRESULT NR_DrawSurfaceToSurfaceTransparent(HSURFACE hDest, HSURFACE hSrc,
                                                   LTRect* pSrcRect, int destX, int destY,
                                                   HLTCOLOR hColor)
{
    LTGetNullRender()->TraceDrawKeyed("DrawSurfaceToSurfaceTransparent", hDest, hSrc, hColor, true);

    LT2DSurf d = LTGetNullRender()->GetView(hDest);
    LT2DSurf s = LTGetNullRender()->GetView(hSrc);
    if (!d.m_pRGBA || !s.m_pRGBA) return LT_ERROR;

    LT2DRect r;
    if (pSrcRect) r = ToRect(pSrcRect);
    // The blend state is honoured -- see NR_DrawSurfaceToSurface.
    const bool bBlend = LT2DBlendEnabled();
    LT2DSurf d2 = LTGetNullRender()->GetTwinView(hDest);
    for (int k = 0; k < (d2.m_pRGBA ? 2 : 1); ++k)
    {
        const LT2DSurf& dd = k ? d2 : d;
        if (bBlend)
            LT2D::BlitBlend(dd, s, pSrcRect ? &r : 0, destX, destY, true,
                            (unsigned int)(uintptr_t)hColor, SrcAlpha(hSrc),
                            g_n2DBlend, g_n2DColor);
        else
            LT2D::Blit(dd, s, pSrcRect ? &r : 0, destX, destY, true, hColor, SrcAlpha(hSrc));
    }
    return LT_OK;
}

static LTRESULT NR_DrawSurfaceSolidColor(HSURFACE hDest, HSURFACE hSrc,
                                         LTRect* pSrcRect, int destX, int destY,
                                         HLTCOLOR hTransColor, HLTCOLOR hFillColor)
{
    LTGetNullRender()->TraceDraw("DrawSurfaceSolidColor", hDest, hSrc);

    LT2DSurf d = LTGetNullRender()->GetView(hDest);
    LT2DSurf s = LTGetNullRender()->GetView(hSrc);
    if (!d.m_pRGBA || !s.m_pRGBA) return LT_ERROR;

    LT2DRect r;
    if (pSrcRect) r = ToRect(pSrcRect);
    LT2D::BlitSolidColor(d, s, pSrcRect ? &r : 0, destX, destY,
                         hTransColor, hFillColor, SrcAlpha(hSrc));
    LT2DSurf d2 = LTGetNullRender()->GetTwinView(hDest);
    if (d2.m_pRGBA)
        LT2D::BlitSolidColor(d2, s, pSrcRect ? &r : 0, destX, destY,
                             hTransColor, hFillColor, SrcAlpha(hSrc));
    return LT_OK;
}

static LTRESULT NR_ScaleSurfaceToSurface(HSURFACE hDest, HSURFACE hSrc,
                                         LTRect* pDestRect, LTRect* pSrcRect)
{
    LTGetNullRender()->TraceDraw("ScaleSurfaceToSurface", hDest, hSrc);

    LT2DSurf d = LTGetNullRender()->GetView(hDest);
    LT2DSurf s = LTGetNullRender()->GetView(hSrc);
    if (!d.m_pRGBA || !s.m_pRGBA) return LT_ERROR;

    LT2DRect dr, sr;
    if (pDestRect) dr = ToRect(pDestRect);
    if (pSrcRect)  sr = ToRect(pSrcRect);
    LT2D::Scale(d, pDestRect ? &dr : 0, s, pSrcRect ? &sr : 0,
                false, 0, SrcAlpha(hSrc));
    LT2DSurf d2 = LTGetNullRender()->GetTwinView(hDest);
    if (d2.m_pRGBA)
        LT2D::Scale(d2, pDestRect ? &dr : 0, s, pSrcRect ? &sr : 0,
                    false, 0, SrcAlpha(hSrc));
    return LT_OK;
}

static LTRESULT NR_ScaleSurfaceToSurfaceTransparent(HSURFACE hDest, HSURFACE hSrc,
                                                    LTRect* pDestRect, LTRect* pSrcRect,
                                                    HLTCOLOR hColor)
{
    LTGetNullRender()->TraceDrawKeyed("ScaleSurfaceToSurfaceTransparent", hDest, hSrc, hColor, true);

    LT2DSurf d = LTGetNullRender()->GetView(hDest);
    LT2DSurf s = LTGetNullRender()->GetView(hSrc);
    if (!d.m_pRGBA || !s.m_pRGBA) return LT_ERROR;

    LT2DRect dr, sr;
    if (pDestRect) dr = ToRect(pDestRect);
    if (pSrcRect)  sr = ToRect(pSrcRect);
    LT2D::Scale(d, pDestRect ? &dr : 0, s, pSrcRect ? &sr : 0,
                true, hColor, SrcAlpha(hSrc));
    LT2DSurf d2 = LTGetNullRender()->GetTwinView(hDest);
    if (d2.m_pRGBA)
        LT2D::Scale(d2, pDestRect ? &dr : 0, s, pSrcRect ? &sr : 0,
                    true, hColor, SrcAlpha(hSrc));
    return LT_OK;
}

static LTRESULT NR_ScaleSurfaceToSurfaceSolidColor(HSURFACE hDest, HSURFACE hSrc,
                                                   LTRect* pDestRect, LTRect* pSrcRect,
                                                   HLTCOLOR hTransColor, HLTCOLOR hFillColor)
{
    LTGetNullRender()->TraceDraw("ScaleSurfaceToSurfaceSolidColor", hDest, hSrc);

    LT2DSurf d = LTGetNullRender()->GetView(hDest);
    LT2DSurf s = LTGetNullRender()->GetView(hSrc);
    if (!d.m_pRGBA || !s.m_pRGBA) return LT_ERROR;

    LT2DRect dr, sr;
    if (pDestRect) dr = ToRect(pDestRect);
    if (pSrcRect)  sr = ToRect(pSrcRect);
    LT2D::ScaleSolidColor(d, pDestRect ? &dr : 0, s, pSrcRect ? &sr : 0,
                          hTransColor, hFillColor, SrcAlpha(hSrc));
    LT2DSurf d2 = LTGetNullRender()->GetTwinView(hDest);
    if (d2.m_pRGBA)
        LT2D::ScaleSolidColor(d2, pDestRect ? &dr : 0, s, pSrcRect ? &sr : 0,
                              hTransColor, hFillColor, SrcAlpha(hSrc));
    return LT_OK;
}

static LTRESULT NR_GetPixel(HSURFACE hSurface, uint32 x, uint32 y, HLTCOLOR* pColor)
{
    if (!pColor) return LT_ERROR;
    LT2DSurf s = LTGetNullRender()->GetView(hSurface);
    if (!s.m_pRGBA) { *pColor = 0; return LT_ERROR; }
    *pColor = LT2D::GetPixel(s, (int)x, (int)y);
    return LT_OK;
}

static LTRESULT NR_SetPixel(HSURFACE hSurface, uint32 x, uint32 y, HLTCOLOR color)
{
    LT2DSurf s = LTGetNullRender()->GetView(hSurface);
    if (!s.m_pRGBA) return LT_ERROR;
    LT2D::SetPixel(s, (int)x, (int)y, color);
    LT2DSurf s2 = LTGetNullRender()->GetTwinView(hSurface);
    if (s2.m_pRGBA) LT2D::SetPixel(s2, (int)x, (int)y, color);
    return LT_OK;
}

static LTRESULT NR_GetBorderSize(HSURFACE hSurface, HLTCOLOR hColor, LTRect* pRect)
{
    if (!pRect) return LT_ERROR;
    LT2DSurf s = LTGetNullRender()->GetView(hSurface);
    if (!s.m_pRGBA) return LT_ERROR;

    LT2DRect r;
    if (!LT2D::BorderSize(s, hColor, r)) return LT_ERROR;

    pRect->left = r.left; pRect->top = r.top;
    pRect->right = r.right; pRect->bottom = r.bottom;
    return LT_OK;
}

static LTRESULT NR_OptimizeSurface(HSURFACE hSurface, HLTCOLOR hTransparentColor)
{
    return LTGetNullRender()->Optimize(hSurface, hTransparentColor) ? LT_OK : LT_ERROR;
}

static LTRESULT NR_UnoptimizeSurface(HSURFACE hSurface)
{
    return LTGetNullRender()->Unoptimize(hSurface) ? LT_OK : LT_ERROR;
}

// HLTCOLOR is just a packed uint32 (ltbasedefs.h:105), so "creating" one is
// arithmetic and "deleting" one is nothing. SetupColor1/2 are documented as
// always-available conveniences, and are the same packing.
static HLTCOLOR NR_MakeColor(float r, float g, float b, LTBOOL bTransparent)
{
    HLTCOLOR c = SETRGB_F(r, g, b);
    if (bTransparent) c |= COLOR_TRANSPARENCY_MASK;
    return c;
}
static HLTCOLOR NR_CreateColor(float r, float g, float b, LTBOOL bT) { return NR_MakeColor(r,g,b,bT); }
static HLTCOLOR NR_SetupColor1(float r, float g, float b, LTBOOL bT) { LTIMPL("ILTClient::SetupColor1"); return NR_MakeColor(r,g,b,bT); }
static HLTCOLOR NR_SetupColor2(float r, float g, float b, LTBOOL bT) { return NR_MakeColor(r,g,b,bT); }
static void     NR_DeleteColor(HLTCOLOR) {}


static HSURFACE NR_CreateSurfaceFromBitmap(char* pBitmapName)
{
    LTIMPL("ILTClient::CreateSurfaceFromBitmap");
    return LTGetNullRender()->CreateSurfaceFromBitmap(pBitmapName);
}

// ⚠️ CreateSurfaceFromString is still a DELIBERATE LT_ERROR STUB, and so is
// every DrawXxx. A string surface needs a font, and inventing its size would be
// category 1 answered with a guess -- the thing this file exists not to do.
// The census is what decides when the font work is worth starting.


void LTNullRender_Wire(ILTClient* p)
{
    if (!p) return;

    p->SetRenderMode         = NR_SetRenderMode;
    p->GetRenderMode         = NR_GetRenderMode;
    p->GetRenderModes        = NR_GetRenderModes;
    p->RelinquishRenderModes = NR_RelinquishRenderModes;
    p->ShutdownRender        = NR_ShutdownRender;
    p->WriteConfigFile       = NR_WriteConfigFile;

    p->FlipScreen            = NR_FlipScreen;
    p->ClearScreen           = NR_ClearScreen;
    p->Start3D               = NR_Start3D;
    p->End3D                 = NR_End3D;
    p->StartOptimized2D      = NR_StartOptimized2D;
    p->SetOptimized2DBlend   = NR_SetOptimized2DBlend;
    p->GetOptimized2DBlend   = NR_GetOptimized2DBlend;
    p->SetOptimized2DColor   = NR_SetOptimized2DColor;
    p->GetOptimized2DColor   = NR_GetOptimized2DColor;
    p->EndOptimized2D        = NR_EndOptimized2D;

    p->GetScreenSurface      = NR_GetScreenSurface;
    p->CreateSurfaceFromBitmap = NR_CreateSurfaceFromBitmap;

    // The 2D compositor -- these really draw now.
    p->FillRect                          = NR_FillRect;
    p->DrawSurfaceToSurface              = NR_DrawSurfaceToSurface;
    p->DrawSurfaceToSurfaceTransparent   = NR_DrawSurfaceToSurfaceTransparent;
    p->DrawSurfaceSolidColor             = NR_DrawSurfaceSolidColor;
    p->ScaleSurfaceToSurface             = NR_ScaleSurfaceToSurface;
    p->ScaleSurfaceToSurfaceTransparent  = NR_ScaleSurfaceToSurfaceTransparent;
    p->ScaleSurfaceToSurfaceSolidColor   = NR_ScaleSurfaceToSurfaceSolidColor;
    p->GetPixel                          = NR_GetPixel;
    p->SetPixel                          = NR_SetPixel;
    p->GetBorderSize                     = NR_GetBorderSize;
    p->OptimizeSurface                   = NR_OptimizeSurface;
    p->UnoptimizeSurface                 = NR_UnoptimizeSurface;
    p->CreateColor                       = NR_CreateColor;
    p->DeleteColor                       = NR_DeleteColor;
    p->SetupColor1                       = NR_SetupColor1;
    p->SetupColor2                       = NR_SetupColor2;
    p->CreateSurface         = NR_CreateSurface;
    p->DeleteSurface         = NR_DeleteSurface;
    p->GetSurfaceDims        = NR_GetSurfaceDims;
    p->GetSurfaceUserData    = NR_GetSurfaceUserData;
    p->SetSurfaceUserData    = NR_SetSurfaceUserData;
    p->GetSurfaceAlpha       = NR_GetSurfaceAlpha;
    p->SetSurfaceAlpha       = NR_SetSurfaceAlpha;
}
