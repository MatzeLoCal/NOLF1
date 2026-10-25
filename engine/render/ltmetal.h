// ----------------------------------------------------------------------- //
//
// MODULE  : ltmetal.h
//
// PURPOSE : A GPU backend for the world renderer, on Metal.
//
// ⚠️ A SECOND BACKEND, NOT A SECOND RENDERER. `ltworldrender` still decides
// WHAT to draw -- which surfaces are sky, which are invisible, how a texture
// coordinate is built from O/P/Q, how a vertex colour is scaled. This module
// only decides HOW the triangles reach the screen. The moment it starts making
// its own decisions about the world it becomes a second renderer, and the
// surface-flag rules get fixed twice and forgotten once, which is exactly what
// `ltworldrender.h` exists to prevent.
//
// ⚠️ AND THE SOFTWARE RASTERISER STAYS. It is not a fallback, it is the
// REFERENCE: it is deterministic, the frame contract is built on it, and it is
// the only thing that can say whether this backend draws the right picture.
// Metal is opt-in (`LT_METAL=1`) so the contract keeps holding untouched.
//
// ⚠️ SHADERS ARE COMPILED AT RUNTIME from a source string rather than shipped
// as a .metallib. `engine/build_skeleton.sh` has no shader step and the Xcode
// project is still outstanding (HANDOVER.md §6); a runtime library keeps the
// whole backend inside the existing build with no toolchain change.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_METAL_H__
#define __LT_METAL_H__

// Was the port built with a Metal backend, and did the device come up?
bool LTMetal_Available();

// Attaches a CAMetalLayer to the window's content view. Safe to call twice.
// Returns false when Metal is unavailable, which is not an error -- the caller
// keeps using the software path.
bool LTMetal_Attach(int nWidth, int nHeight);

void LTMetal_Shutdown();

// ⚠️ BRINGS THE PIPELINE UP WITHOUT A WINDOW, so the parts most likely to be
// wrong -- the shader source, the vertex layout matching the shader's
// attributes, the pipeline's pixel formats -- can be checked headlessly and in
// the test suite. "It compiled" is not evidence that a renderer draws; this is
// the smallest amount of evidence that is not free.
//
// Writes a reason into pErr on failure. Does not disturb an attached layer.
bool LTMetal_SelfTest(char* pErr, int nErrBytes);

// ---- per frame ------------------------------------------------------- //

// Starts a frame and clears colour + depth. Returns false when the drawable
// could not be acquired (a minimised window), and the caller must skip drawing
// rather than treat it as fatal.
bool LTMetal_BeginFrame(float fR, float fG, float fB);

// ⚠️ Clears DEPTH ONLY, mid-frame. The sky is drawn first from its own centre
// and must then lose every depth test against the world -- the software path
// does exactly this between its passes (ltworldrender.cpp), and without it the
// sky occludes the level.
void LTMetal_ClearDepth();

// ---- mirrors (HANDOVER 5de) ------------------------------------------ //
//
// A mirror's REFLECTION is a whole scene drawn from the mirrored camera into a
// target of its own, before the main pass draws anything. Call inside a frame
// (after LTMetal_BeginScene / BeginOffscreen / BeginFrame), once per mirror:
// BeginReflection, the scene's draws, EndReflection. Then
// ResumeAfterReflections puts the main pass back (its colour kept, depth
// cleared). ReflectionTexture(slot) is a texture handle drawn by SCREEN
// position: a triangle using it writes the reflection's pixel at its own.
bool LTMetal_BeginReflection(int nSlot, int nWidth, int nHeight);
void LTMetal_EndReflection();
void LTMetal_ResumeAfterReflections();
unsigned int LTMetal_ReflectionTexture(int nSlot);

// ---- offscreen ------------------------------------------------------- //
//
// ⚠️ RENDERING WITHOUT A WINDOW, and it is what makes the GPU path TESTABLE.
// A backend that can only draw into a live CAMetalLayer can only be checked by
// a human looking at it, which is not a check anyone else can repeat. With an
// offscreen target the same view can be rendered through both backends in one
// headless process and compared.
//
// Use instead of LTMetal_BeginFrame; pair with LTMetal_ReadPixels then
// LTMetal_EndFrame.
bool LTMetal_BeginOffscreen(int nWidth, int nHeight, float fR, float fG, float fB);

// Reads the offscreen target back as RGBA8, top-down, w*h*4. Must be called
// after LTMetal_EndFrame so the work has completed.
bool LTMetal_ReadPixels(unsigned char* pRGBA, int nWidth, int nHeight);

void LTMetal_EndFrame();

// ---- resources ------------------------------------------------------- //

// Uploads RGBA8 with its mip chain and returns a handle, or 0. Levels are
// tightly packed and must halve each time, as the DTX chain does.
//
// ⚠️ bCutout (an alpha-tested texture) FILLS THE COLOUR OF EVERY TEXEL BELOW
// fAlphaRef from its opaque neighbours, on every level, in the uploaded copy
// only. What an artist leaves under a cut-out is arbitrary -- black, magenta,
// the next leaf -- and a filtered sample at the edge mixes it in as a fringe.
// Nearest sampling plus the alpha test never shows those texels, so the old
// path is unchanged by it; the compositor's filtered cut-outs depend on it.
// LT_CUTOUTS=0 skips it (HANDOVER 5bs).
unsigned int LTMetal_CreateTexture(const unsigned char* const* ppLevels,
                                   const int* pW, const int* pH, int nLevels,
                                   bool bCutout = false, float fAlphaRef = 0.5f);

// ---- drawing --------------------------------------------------------- //

// One vertex, matching the layout the shader declares.
struct LTMetalVert
{
    float m_Pos[3];
    float m_UV[2];
    float m_RGB[3];
};

// Queues triangles that share one texture. `pMVP` is row-major, as everything
// else in this engine is.
//
// ⚠️ bAlphaTest discards texels below fAlphaRef, which is the texture's OWN
// authored threshold (CLTDtx::GetAlphaRef, carried on LT3DTexture::m_fAlphaRef)
// and NOT a constant. 231 shipped textures state one and every one is below the
// 0.5 this used to assume. Anything outside (0,1] falls back to 0.5, matching
// the software rasteriser -- the two backends must agree, because the software
// one is the reference the frame contract is built on.
//
// ⚠️ bBlend composites source-alpha over the target and WRITES NO DEPTH, which
// is lt3d.cpp's blended path in fixed function. It is the caller's job -- the
// same caller, ltworldrender's pass 4 -- to submit these runs AFTER all opaque
// geometry and back-to-front among themselves; nothing here can recover an
// order that arrives wrong.
// ⚠️ fConstAlpha and bTexAlpha are the two halves of the blend weight and are
// meaningless unless bBlend: the product `(bTexAlpha ? texel.a : 1) *
// fConstAlpha`, exactly as lt3d.cpp computes it. A model's translucency comes
// from the OBJECT (`Alpha` in Attributes\PropTypes.txt), and for the shipped
// light-beam props the skin's alpha channel is all zero -- so bTexAlpha false
// is not an optimisation, it is the difference between a beam and nothing.
// ⚠️ nBlendOp picks WHICH compositing rule: 0 source-alpha over, 1 additive,
// 2 multiply. Each is a separate pipeline object here because blend factors are
// fixed-function state, and each matches lt3d.cpp's arithmetic exactly.
void LTMetal_DrawTris(const float* pMVP, unsigned int nTexture,
                      const LTMetalVert* pVerts, int nVerts,
                      bool bAlphaTest, float fAlphaRef, bool bBlend = false,
                      float fConstAlpha = 1.0f, bool bTexAlpha = true,
                      int nBlendOp = 0);

// ⚠️ VERTICES THAT LIVE ON THE GPU (HANDOVER 5bu): the static world, recorded
// once per level. A buffer is made once and drawn from every frame; the draw
// is LTMetal_DrawTris in every other respect, through the same encode. Returns
// 0 when there is no device yet -- create after a Begin call, as textures are.
unsigned int LTMetal_CreateBuffer(const LTMetalVert* pVerts, int nVerts);
void         LTMetal_FreeBuffer(unsigned int nBuffer);
void LTMetal_DrawTrisFrom(const float* pMVP, unsigned int nTexture,
                          unsigned int nBuffer, int nFirst, int nVerts,
                          bool bAlphaTest, float fAlphaRef, bool bBlend = false,
                          float fConstAlpha = 1.0f, bool bTexAlpha = true,
                          int nBlendOp = 0);

// Diagnostics, for the harness rather than the game.
// Linear depth fog for the draws that follow (until set again): view depth
// near..far blends toward the colour (0..255). Off by default. HANDOVER 5bo.
void LTMetal_SetFog(bool bOn, float fNear, float fFar, const float* pRGB255);
// RANGE fog (HANDOVER 5ch): 1/|P00|, 1/|P11| of the projection the following
// draws use; 0, 0 = depth fog (the default).
void LTMetal_SetFogRange(float fKX, float fKY);

// ⚠️ GPU SKINNING (HANDOVER 5cd). A model's mesh goes up once -- vertices with
// their file position, normal and weight range; the weights; one CORNER per
// face corner (UVs live on faces) -- and a frame sends only the node palette
// and the uniforms. The vertex shader does LTModelDraw::Draw's per-vertex
// work; the fragment stage is the world's.
struct LTMetalSkinVert   { float m_Pos[3]; unsigned int m_nWFirst; float m_Nrm[3]; unsigned int m_nWCount; };
struct LTMetalSkinWeight { float m_Offset[3]; float m_fBias; unsigned int m_nNode; unsigned int m_Pad[3]; };
struct LTMetalSkinCorner { unsigned int m_nVert; float m_U; float m_V; unsigned int m_Pad; };

unsigned int LTMetal_CreateSkinMesh(const LTMetalSkinVert* pVerts, int nVerts,
                                    const LTMetalSkinWeight* pWeights, int nWeights,
                                    const LTMetalSkinCorner* pCorners, int nCorners);
void         LTMetal_FreeSkinMesh(unsigned int nMesh);

// The palette: 24 floats per node -- three rows of the posed node matrix
// (x, y, z, translation), then three rows of R_node * R_bind^T (w unused).
// Returns its offset in this frame's arena, or -1.
int          LTMetal_PushPalette(const float* pPalette, int nNodes);

struct LTMetalSkinDraw
{
    float m_Normal[9];          // object normal matrix, row-major 3x3
    int   m_nPaletteOfs;        // LTMetal_PushPalette's answer, -1 = bind pose
    int   m_nNodes;
    bool  m_bMirror, m_bMirrorU, m_bLit, m_bSkinNormal, m_bBiasNorm;
    int   m_nLights;
    float m_Ambient[3];
    float m_Dir[8][3];
    float m_Color[8][3];
};
void LTMetal_DrawSkinned(const float* pMVP, unsigned int nTexture, unsigned int nMesh,
                         int nFirstCorner, int nCorners, const LTMetalSkinDraw& sd,
                         bool bAlphaTest, float fAlphaRef, bool bBlend = false,
                         float fConstAlpha = 1.0f, bool bTexAlpha = true, int nBlendOp = 0);
bool LTMetal_SkinningAvailable();

// ---- the compositor (ltcomp.h, HANDOVER 5bq) ------------------------------ //
//
// ⚠️ ONE COMMAND BUFFER PER HOST FRAME, AND NOTHING WAITS FOR IT. The scene
// passes (every RenderCamera) and the composite go into it; it is committed
// by the composite, or by LTMetal_CompFrameEnd when nothing presented. Three
// frames may be in flight, so everything the CPU rewrites each frame -- the
// vertex arena, the two 2D layer textures -- exists three times.

// A CAMetalLayer on the window's content view at its backing scale, for the
// compositor (walk mode keeps LTMetal_Attach). Returns false when unavailable.
bool LTMetal_CompAttach();

// The window's drawable in PIXELS (points x backing scale). False headless.
bool LTMetal_CompOutputSize(int* pW, int* pH);

// Opens the 3D target -- the output's size, kept across frames -- and a pass
// on it restricted to (vx, vy, vw, vh). bClear for a frame's first camera.
// Everything LTMetal_DrawTris / LTMetal_ClearDepth do lands there until
// LTMetal_EndScene.
bool LTMetal_BeginScene(int nOutW, int nOutH, int vx, int vy, int vw, int vh, bool bClear);

// The global light scale and the camera's light add, on the 3D only:
// c = c * scale + add over the pass's viewport (ltscene's LUT, on the GPU).
void LTMetal_SceneLightScale(const float* pScale3, const float* pAdd3);

void LTMetal_EndScene();

// out = scene * M + A over the mapped rectangle, black elsewhere (see
// ltcomp.h for M and A). pB/pG are the two 2D layers, RGBA, nLayerW*nLayerH.
// bPresent -> the window's drawable; otherwise an offscreen target, read
// back into pRead (nOutW*nOutH*4 RGBA) when it is given.
struct LTCompMap;
bool LTMetal_Composite(const unsigned char* pB, const unsigned char* pG,
                       int nLayerW, int nLayerH, const LTCompMap& map,
                       int nOutW, int nOutH, bool bPresent, unsigned char* pRead);

// Commits the frame's command buffer if no composite did.
void LTMetal_CompFrameEnd();

// The GPU's own time over the compositor frames completed since the last call
// (summed, ms), how many, and command buffers failed so far. For LT_PASSTIME.
bool LTMetal_TakeGpuTime(double* pMs, int* pnFrames, int* pnErrors);

// Waits for every frame in flight, then releases the compositor's layer and
// targets. Before the window closes; safe to call when it never started.
void LTMetal_CompShutdown();

unsigned int LTMetal_FrameTriangles();
const char*  LTMetal_LastError();

#endif // __LT_METAL_H__
