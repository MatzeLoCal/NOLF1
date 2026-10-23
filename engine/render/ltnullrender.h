// ----------------------------------------------------------------------- //
//
// MODULE  : ltnullrender.h
//
// PURPOSE : The null renderer -- PORT_PLAN.md §4, Phase 4.
//
//           "Logs draw calls; census what the game actually asks for before
//            drawing anything."
//
// WHY THIS IS NOT "MAKE SetRenderMode RETURN LT_OK"
//
//   The skeleton's generated stubs all return LT_ERROR and announce themselves,
//   for the reason gen_skeleton.py's header gives: "succeeded" when nothing
//   happened is the silent failure that costs weeks. SetRenderMode is currently
//   one of those, and the game correctly gives up:
//
//       NOLF Error: Couldn't set to D3D Emulation mode. Shutting down NOLF...
//
//   Flipping that one return value would get past the message and produce a
//   game running blind on a screen surface that does not exist, with
//   GetSurfaceDims writing into uninitialised memory. Every later symptom would
//   appear miles from the cause.
//
// ⚠️ THE RULE THIS MODULE IS BUILT ON
//
//   A null renderer is allowed to not draw. It is NOT allowed to lie about
//   state. So every entry point falls into exactly one of three categories, and
//   which one it is is a decision, not an accident:
//
//   1. STATE -- must be TRUTHFUL. GetRenderMode returns the mode actually set;
//      GetSurfaceDims returns the dimensions of a surface that was actually
//      allocated. These answers feed layout arithmetic, and a wrong number here
//      propagates into every coordinate the game computes.
//
//   2. DRAWING -- a DECLARED no-op. Returns LT_OK, draws nothing, and is
//      counted. This is not a lie because it is the module's stated contract,
//      it is recorded in the run record, and the count is checked. "I did not
//      draw, and here is exactly how many times I did not draw" is data.
//
//   3. EVERYTHING ELSE -- still LT_ERROR, still announced, exactly as before.
//      The census is how the next piece of work gets chosen, so anything not
//      deliberately implemented must keep showing up in it.
//
//   Category 3 is the important one. It is why this module implements the mode
//   and surface state and STOPS, instead of pre-emptively filling in thirty
//   drawing entry points the game may never call. The census decides what comes
//   next -- that is Phase 4's whole purpose.
//
// ⚠️ AS OF THE 2D COMPOSITOR, "NULL" IS ONLY HALF TRUE
//
//   The 2D path now REALLY DRAWS, into the screen surface's pixel buffer via
//   lt2d.h. Those entry points have moved out of category 2 and are no longer
//   no-ops. What remains null is the 3D side -- Start3D/End3D/RenderCamera and
//   everything world- or model-shaped -- and the presentation step: FlipScreen
//   still presents to nowhere, because there is no window yet.
//
//   The name is kept because renaming the file would churn every include for a
//   distinction this comment makes better.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_NULLRENDER_H__
#define __LT_NULLRENDER_H__

#include <vector>

#include "ltbasedefs.h"
#include "lt2d.h"

class ILTClient;

class CLTNullRender
{
public:

    CLTNullRender();
    ~CLTNullRender();

    // --- render mode (STATE: truthful) -------------------------------- //

    // Accepts any mode with a plausible width/height/depth and remembers it.
    // ⚠️ Rejects a zero-sized mode rather than accepting everything: a renderer
    // that accepts 0x0 makes every later division by width a crash a long way
    // from here, and the game has a documented fallback path for a refused
    // mode that we want to stay reachable.
    bool                SetMode(const RMode* pMode);
    bool                GetMode(RMode* pMode) const;
    bool                HasMode() const { return m_bModeSet; }

    // The offered mode list. Deliberately short and fixed -- these are the
    // resolutions NOLF's UI is authored against.
    RMode*              AllocModeList();
    void                FreeModeList(RMode* pModes);

    void                ShutdownRender(uint32 flags);

    // --- surfaces (STATE: truthful) ----------------------------------- //
    //
    // Surfaces are real allocations with real dimensions and real user-data
    // slots, and one made from a bitmap holds the decoded image. Everything the
    // game can ASK about a surface is answered correctly; nothing is drawn.

    HSURFACE            GetScreenSurface();
    HSURFACE            CreateSurface(uint32 w, uint32 h, const char* pOrigin);
    bool                DeleteSurface(HSURFACE h);

    // Loads a PCX through the file manager and makes a surface of the real
    // size holding the real pixels. ⚠️ Holding pixels is not drawing them --
    // this stays a null renderer. It is what makes the loader verifiable
    // end-to-end today rather than at the same moment as a first blitter.
    HSURFACE            CreateSurfaceFromBitmap(const char* pName);

    // RGBA8888, m_Width * m_Height * 4. Every surface has one now -- the 2D
    // compositor draws into them.
    const unsigned char* GetPixels(HSURFACE h) const;

    // A mutable view for the compositor. Returns a zeroed view for an unknown
    // handle, so callers cannot accidentally draw through a stale pointer.
    LT2DSurf            GetView(HSURFACE h) const;

    // ⚠️ THE COMPOSITOR'S SECOND 2D LAYER (ltcomp.h, HANDOVER 5bq): a view of
    // the screen's grey layer while the compositor runs, else a zeroed view.
    // Every write to the screen is applied to both.
    LT2DSurf            GetTwinView(HSURFACE h) const;
    const unsigned char* GetTwinPixels() const;

    // Marks the whole screen as "the 3D shows here" in both layers.
    void                OpenScreenForScene();
    // The same, inside one rectangle of the screen (x, y, w, h), clipped --
    // the menu's 3D (HANDOVER 5ce).
    void                OpenScreenRect(int x, int y, int w, int h);

    // OptimizeSurface remembers the transparent colour the game chose for a
    // surface. ⚠️ Real state, not a no-op: the UI calls OptimizeSurface once
    // and then blits without repeating the colour.
    bool                Optimize(HSURFACE h, uint32 transColor);
    bool                Unoptimize(HSURFACE h);
    bool                IsOptimized(HSURFACE h) const;
    uint32              GetTransColor(HSURFACE h) const;

    // Clears the screen surface. ⚠️ Stands in for the 3D scene that would
    // otherwise repaint the viewport every frame -- see the .cpp.
    void                ClearScreenForFrame();

    // Blits the screen surface to the window, if one is open.
    void                PresentToWindow() const;

    // Writes the screen surface as a binary PPM. Env-gated by the host.
    // ⚠️ This is what gives harness/framediff.py its first real input.
    bool                DumpScreenPPM(const char* pPath) const;

    bool                GetDims(HSURFACE h, uint32* pW, uint32* pH) const;
    void*               GetUserData(HSURFACE h) const;
    void                SetUserData(HSURFACE h, void* p);
    bool                GetAlpha(HSURFACE h, float& a) const;
    bool                SetAlpha(HSURFACE h, float a);

    // --- drawing (DECLARED no-op, counted) ---------------------------- //

    // Records one draw call by name. First occurrence goes to the run record;
    // every occurrence is counted.
    void                Draw(const char* pOp);

    // LT_TRACE_2D=1: name every draw and WHICH SURFACES it touched.
    // ⚠️ "the compositor ran" and "the compositor drew on the screen" are
    // different claims, and the draw census cannot tell them apart.
    void                TraceDraw(const char* pOp, HSURFACE hDest, HSURFACE hSrc);
    void                TraceDrawKeyed(const char* pOp, HSURFACE hDest, HSURFACE hSrc,
                                       uint32 key, bool bHasKey);
    const char*         DescribeSurface(HSURFACE h) const;

    // --- census -------------------------------------------------------- //

    void                DumpCensus() const;
    void                RecordTotals() const;

    uint32              GetNumSurfacesLive() const  { return m_nSurfacesLive; }
    uint32              GetNumSurfacesMade() const  { return m_nSurfacesMade; }
    uint32              GetNumBitmapsLoaded() const { return m_nBitmapsLoaded; }
    uint32              GetNumBitmapsMissing() const { return m_nBitmapsMissing; }

private:

    struct Surface;

    Surface*            Find(HSURFACE h) const;

    RMode               m_Mode;
    bool                m_bModeSet;

    Surface*            m_pSurfaces;        // singly-linked, newest first
    Surface*            m_pScreen;
    uint32              m_nSurfacesMade;
    uint32              m_nSurfacesLive;
    uint32              m_nBitmapsLoaded;
    uint32              m_nBitmapsMissing;

    // Draw-call census. Fixed-size on purpose: an unbounded table on a
    // per-frame path is a memory leak that only shows up in a long run.
    enum { kMaxDrawOps = 64 };
    struct DrawOp { const char* m_pName; uint32 m_nCalls; };
    DrawOp              m_DrawOps[kMaxDrawOps];
    int                 m_nDrawOps;
    uint32              m_nDrawOpsOverflow;
};

CLTNullRender* LTGetNullRender();

// Points ILTClient's renderer and surface function-pointer members at this
// module. ⚠️ Must be called AFTER the generated wiring, which fills in the
// LT_ERROR defaults and would otherwise overwrite these.
void LTNullRender_Wire(ILTClient* pClient);

// ⚠️ THE HOST PRESENTS EVERY FRAME ITSELF (skeleton_host.cpp, after the
// shell's Update and PostUpdate). The game's PostUpdate calls FlipScreen every
// frame too (CInterfaceMgr::PostUpdate), so while the host runs it FlipScreen
// is recorded but does not present a second time. Outside it -- the loading
// screen, a display-mode change -- FlipScreen presents as before.
void LTNullRender_SetHostPresents(bool bHostPresents);

#endif // __LT_NULLRENDER_H__
