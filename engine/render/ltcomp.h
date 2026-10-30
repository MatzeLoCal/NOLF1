// ----------------------------------------------------------------------- //
//
// MODULE  : ltcomp.h
//
// PURPOSE : THE COMPOSITOR -- the game's 3D at the OUTPUT's native resolution,
//           its 2D interface at the game's own resolution, combined on the GPU.
//           HANDOVER 5bq.
//
// ⚠️ WHY IT EXISTS. NOLF lays its interface out in SCREEN PIXELS: a HUD bitmap
// is so many pixels wide whatever the resolution. Raising the game's resolution
// to a Retina panel's 2560x1664 therefore shrinks the HUD to a postage stamp,
// and keeping it at 1024x768 left the 3D at 1024x768 too -- read back to the
// CPU and stretched nearest-neighbour by CoreGraphics. The two had to come
// apart: the 3D renders straight into a native-size GPU target and is never
// read back; the 2D stays on the game's surface and is scaled up at present.
//
// ⚠️ THE 2D IS DRAWN TWICE, and that is what makes every blend mode exact.
// The game's 2D blends ONTO the 3D -- alpha panels, additive glyphs, MASK
// shadows, MULTIPLY -- and the 3D is no longer in the surface to blend with.
// Every lt2d operation is affine per channel in the destination
// (dest' = dest*m + a), so the screen surface keeps TWO layers: one that
// starts black (B) and one that starts mid grey (G, 128) wherever the 3D
// shows. After all the 2D: B = A and G = 0.5*M + A, so the GPU recovers
//     out = scene * M + A,   M = (G - B) * 255/128,   A = B
// per pixel, exactly, for alpha, solid, multiply, multiply2, mask and
// mask-add. Only ADD brighter than half saturates the grey layer.
//
// ⚠️ THIS FILE LINKS NO METAL. The GPU half is installed as function pointers
// by the host (skeleton_host.cpp), so ltnullrender and ltscene -- which call
// in here -- still build anywhere.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_COMP_H__
#define __LT_COMP_H__

// Where the 2D layer's rectangle lands in the output, in output pixels.
struct LTCompMap
{
    int     m_DstX, m_DstY, m_DstW, m_DstH;     // output pixels
    float   m_SrcX, m_SrcY, m_SrcW, m_SrcH;     // 2D-layer pixels
    bool    m_bScene;                           // the 3D target underlies dst
    // ⚠️ THE MENU BOX OVER THE 3D TARGET (HANDOVER 5ce): RenderObjects drew
    // the menu's 3D natively; inside the box the composite reads the scene
    // where the layers are open, outside it the bars stay black.
    bool    m_bMenuScene = false;
};

// ---- the GPU half, installed by the host -------------------------------- //

typedef bool (*LTCompOutSizeFn)(int* pW, int* pH);
typedef bool (*LTCompPresentFn)(const unsigned char* pB, const unsigned char* pG,
                                int nLayerW, int nLayerH, const LTCompMap& map,
                                int nOutW, int nOutH, bool bPresent,
                                unsigned char* pReadRGBA);
typedef void (*LTCompFrameEndFn)();

void LTComp_Install(LTCompOutSizeFn fnSize, LTCompPresentFn fnPresent,
                    LTCompFrameEndFn fnFrameEnd);

// ---- state --------------------------------------------------------------- //

// Will this run use the compositor? Decided from the environment alone, so it
// can be asked before the game sets its mode (the surface's shape depends on
// it): a window with Metal and the game's server, or a headless LT_METAL=1
// run with LT_COMPOSITOR=1. LT_COMPOSITOR=0 is the old path (readback +
// CoreGraphics), and says so.
bool LTComp_Wanted();

// Is it running -- wanted AND the GPU half installed.
bool LTComp_Active();

// The output: the window's drawable in pixels, or LT_NATIVE=WxH headless
// (default 2560x1664).
bool LTComp_OutputSize(int* pW, int* pH);

// The output rectangle (x, y, w, h) the in-game mapping puts a surface
// rectangle on -- where a RenderCamera's view is drawn.
void LTComp_SceneRect(int nSurfW, int nSurfH, const int* pSurfRect4, int* pOutRect4);

// A RenderCamera drew this frame. The first one clears the 3D target.
void LTComp_NoteCamera();
bool LTComp_FirstCameraThisFrame();

// The menu's RenderObjects drew into the 3D target this frame (HANDOVER
// 5ce); the first scene draw of a frame -- camera or objects -- clears it.
void LTComp_NoteObjects();
bool LTComp_FirstSceneThisFrame();

// The game scaled a picture over the WHOLE screen surface this frame -- the
// splash and demo screens (HANDOVER 5dy) -- so this frame shows the whole
// surface, not the menu box.
void LTComp_NoteFullScreen2D();

// The output rectangle (x, y, w, h) a RenderObjects camera's surface
// rectangle lands on, under whichever mapping this frame will present with.
void LTComp_ObjectsRect(int nSurfW, int nSurfH, const int* pSurfRect4, int* pOutRect4);

// The mapping this frame presents with: in-game (the whole surface, filling
// the output) while the world is being drawn, or THE MENU BOX -- NOLF draws
// every menu and loading screen as a 640x480 box in the middle of whatever
// surface it has -- zoomed to fit, with bars only where the aspect differs.
LTCompMap LTComp_CurrentMap(int nSurfW, int nSurfH);

// Composites and presents (bPresent) or reads the composite back (pRead,
// nOutW*nOutH*4 RGBA). Returns the output size used.
bool LTComp_Present(const unsigned char* pB, const unsigned char* pG,
                    int nLayerW, int nLayerH, bool bPresent,
                    unsigned char* pReadRGBA, int* pOutW, int* pOutH);

// End of a host frame: commits GPU work that no present took.
void LTComp_FrameEnd();

// Output pixel -> 2D-layer pixel, through the last mapping presented (the
// mouse). Clamped into the mapped rectangle.
bool LTComp_OutputToSurface(float fX, float fY, int* pX, int* pY);

#endif // __LT_COMP_H__
