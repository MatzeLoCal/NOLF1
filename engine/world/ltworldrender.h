// ----------------------------------------------------------------------- //
//
// MODULE  : ltworldrender.h
//
// PURPOSE : Draws a loaded NOLF world -- architecture, sky and props -- into
//           an RGBA target, from an arbitrary camera.
//
// ⚠️ THIS EXISTS SO THERE IS ONE RENDERER, NOT TWO. It began as code inside
// `harness/worldview`, and the moment the host wanted to draw the same world
// interactively there was a choice between copying it and lifting it. Copies
// drift: the surface-flag rules, the sky pass and the skin-list splitting are
// all things that were got wrong once already, and having them in two places
// means fixing them twice and forgetting the second.
//
// WHAT IT IS NOT
//
// ⚠️ Not the game's renderer. It has no PVS, no portals-as-clipping, no
// lightmaps, no dynamic lights and no draw ordering beyond the depth buffer.
// It draws everything the world declares, every frame. That is fine for a
// still and adequate for walking around a level; it is not what shipping the
// game would use.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_WORLD_RENDER_H__
#define __LT_WORLD_RENDER_H__

#include "ltmodeldraw.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <functional>

#include "ltworld.h"
#include "lt3d.h"
#include "ltabc.h"
#include "ltproptypes.h"

class CLTFileMgr;

// ⚠️ IS THE SCENE MIRRORED? Default TRUE (LT_NOMIRROR=1 disables) -- NOLF's
// world is left-handed and this renderer's basis is not, so the image is
// flipped to match the real game. Settled against Windows screenshots of
// M01S01's 28/29 door corner; see ltworldrender.cpp.
//
// ⚠️ EXPOSED BECAUSE BOTH BACKENDS MUST AGREE. It was file-static, the GPU
// path built its own projection without it, and the `metal` suite failed the
// moment the default changed -- software mirrored, GPU not, agreement 99.7%
// -> 81.5%. That is the "one decision path, two backends" rule earning its
// keep for the second time in one day; a mirror decided in two places is a
// mirror that will drift.
bool LTWorldMirror();

// ⚠️ AN EXPERIMENT KNOB FOR THE PLAYER, not a fix. A wall prop's orientation is
// something only a human looking at the running game can judge, and three
// rounds of me comparing a render against a general idea of "how a door handle
// looks" produced three wrong answers. `LT_PROPYAW=<degrees>` adds a constant
// yaw to every world OBJECT (never to world geometry), so the question stops
// being "does this look right to me" and becomes "which of these four looks
// right to you".
//
// Default 0. Radians, converted from the degrees the env var gives.
float LTPropYawOffset();

// World-file props: no model mirror and a 180-degree turn about the model's own
// Y (see the definitions, HANDOVER 5ay). LT_PROPMIRROR=1 -> the old placement.
bool  LTPropOldPlacement();
int   LTPropMirrorModel();              // the nMirrorModel to pass to Draw
void  LTApplyPropTurn(float* pM16);     // M = M * turnY180, only under LT_PROPTURN180 (5bt)
bool  LTPropTurn180();                  // LT_PROPTURN180=1: 5ay's turn instead of the rest pose
float LTOriginOffsetSign();             // +1: the donor ADDS the origin offset (5bt)

// Is the translucent-model pass on? LT_NOMODELBLEND=1 turns it off, so a
// before/after comparison does not need two binaries.
bool LTModelBlendEnabled();

// ⚠️ ONE PLACE DECIDES WHETHER A MODEL IS COMPOSITED, and it is inline here
// because THREE call sites need the same answer -- the software object pass,
// the Metal one, and the model cache that sets the skin flags. The last time a
// rule like this lived in two of the three, the two renderers drifted (§5e).
//
// ⚠️ MULTIPLY IS DELIBERATELY NOT IN IT. Only 13 objects in the shipped data
// set it: three `Water` brushes with no model at all, and four multiplayer
// `PlayerVehicle` motorcycle spawns. Rendering those four through `dst * src`
// turns the motorcycle almost completely BLACK in its dim garage -- measured,
// 25.7 mean absolute difference -- which is plainly not what the level wants,
// and there is no reference picture to argue from. The mode is implemented in
// both rasterisers; nothing selects it yet, on purpose.
inline bool LTIsTranslucentDraw(float fAlpha, int nBlendOp)
{
    return fAlpha < 1.0f || nBlendOp == 1;
}

// Are sprite objects drawn? LT_NOSPRITES=1 turns them off, the same A/B
// instrument as LT_NOMODELBLEND.
bool LTSpritesEnabled();

// Is the sky pass drawn? LT_NOSKY=1 skips it, so a frame shows the level
// alone -- the only way to ask whether a suspect area is a surface or the sky.
bool LTSkyEnabled();

// ⚠️ FOG BY RANGE, LT2's rule (HANDOVER 5ch): the straight-line distance from
// the eye, as d3d.ren's vertex fog computes it. LT_FOGDEPTH=1 = view depth,
// the donor's table fog (old, says so). Both renderers ask this.
bool LTFogRange();

// Are prop objects drawn? LT_NOOBJECTS=1 skips them, for bisecting a
// difference into "the level" or "the things standing in it".
bool LTObjectsEnabled();

// ⚠️ MIRRORS (HANDOVER 5de): reflections are drawn when the game's "Mirrors"
// option is on -- console DrawPortals, which the host pushes in each frame
// (absent = on). LT_NOMIRRORS=1 = never (says so). Both renderers ask this.
void LTWorldSetMirrors(bool bOn);
bool LTMirrorsEnabled();

// ⚠️ TRUE WHILE A MIRROR'S REFLECTION IS BEING DRAWN (HANDOVER 5df). The
// object lists ask it, because LT2 draws a different set in a "portal":
// FLAG_VISIBLE *or* FLAG_PORTALVISIBLE, and never FLAG2_PORTALINVISIBLE. The
// first-person player is exactly that case -- the game hides her
// (ShowPlayer(FALSE)) and CPlayerObj carries FLAG_PORTALVISIBLE, so Cate is
// seen in a mirror and nowhere else; the view weapon is PORTALINVISIBLE.
bool LTWorldPortalPass();
void LTWorldSetPortalPass(bool b);

// ----------------------------------------------------------------------- //
//
// ⚠️ SEPARATE WORLD MODELS ARE PULLED A HAIR TOWARD THE EYE, AND THAT IS NOT A
// HACK -- IT IS WHAT MAKES THE PICTURE DETERMINISTIC.
//
// A door, a window, a railing, a safety net is a BRUSH laid FLUSH on the
// brushwork behind it. Measured over the shipped levels, almost every separate
// world model shares at least one plane with the main BSP -- 138 of 138 in
// M03S01, 299 of 303 in M10S04. So "which of these two exactly coincident
// surfaces do you see?" is not an edge case, it is the normal case, and the
// answer was being decided by floating-point noise in the depth interpolation:
// the software rasteriser picked the wall on every pixel of T05S01's safety
// net and Metal picked the net, for a 57.97 mean absolute difference between
// two renderers that were each behaving correctly.
//
// ⚠️ A DEPTH-TEST TIE-BREAK CANNOT FIX THIS. The depths are not equal, they are
// merely equal in intent; two different interpolation paths across two
// differently shaped triangles land a few ulps apart, and which way is
// arbitrary per pixel. Reversing the comparison just moves the coin flip.
//
// ⚠️ NOR CAN A GPU DEPTH BIAS, not if the two backends must agree: Metal's
// depth bias is in clip-z units and the software rasteriser's depth is 1/w, so
// the same "small nudge" is a different nudge in each.
//
// So the GEOMETRY moves instead, by scaling the model about the EYE:
//
//     p' = eye + (p - eye) * (1 - 1/8192)
//
// Because p' lies on the ray from the eye through p, the projected position is
// UNCHANGED -- only the depth shrinks, by a constant 0.012% of the distance.
// That is roughly a hundred times the interpolation noise and a small fraction
// of any real gap in the level, and being a plain matrix it is identical in
// both backends by construction.
//
// Writes a 4x4 into pOut16, to be multiplied on the RIGHT of the view-projection
// (it operates in world space, so it goes inside any per-model transform).
// LT_NOMODELBIAS=1 turns it off.
//
// ----------------------------------------------------------------------- //
void LTBuildModelDepthBias(const float* pEye3, float* pOut16);
bool LTModelDepthBiasEnabled();

// ⚠️ Is this model the level's MAIN brushwork -- the one thing the nudge above
// is measured AGAINST? One function, because both backends ask and this port
// has been bitten seven times by a rule that lived in one of them.
//
// Measured over all 102 shipped worlds: exactly one model per world carries
// this flag, always named `PhysicsBSP`, and nothing else does.
bool LTIsMainWorldBSP(const LTWorldModelHeader& m);

// ----------------------------------------------------------------------- //
//
// ⚠️ A SPRITE OBJECT IS NOT A MODEL AND NOT A BRUSH -- it is a camera-facing
// QUAD with no geometry in the world file at all. 5,191 of them are placed
// across 85 of the 102 shipped worlds and this port drew none of them: every
// candle flame, lamp glow and window flare in NOLF is one of these.
//
// ⚠️ `ScaleSprite` declares `Additive` DEFAULT TRUE, and 4,624 of the 5,191
// say it explicitly. Their textures are dark with bright centres and an alpha
// channel that is 100% ZERO -- the same shape as the light beams in §5g, and
// for the same reason: the picture is meant to be ADDED to what is behind it.
//
// ----------------------------------------------------------------------- //
struct LTSpriteDraw
{
    std::string m_sSprite;      // the `.spr` (or a `.dtx`) the object names
    float       m_fScaleX;      // ScaleX / ScaleY, default 0.5 as the class does
    float       m_fScaleY;
    float       m_Colour[3];    // 0..1, the object's Color / 255
    float       m_fAlpha;       // 0..1
    int         m_nBlendOp;     // 0 source-alpha over, 1 additive

    // ⚠️ FLAG_GLOWSPRITE: the quad grows with distance, 0.1x at 10 units to
    // 2.0x at 500. It is what keeps a distant lamp glow visible instead of
    // shrinking into one pixel, and 4,584 of the 5,191 ask for it.
    bool        m_bGlow;

    // ⚠️ FLAG_SPRITEBIAS: pulled 20 units toward the camera and shrunk to
    // match, so it keeps its apparent size and stops fighting the wall it is
    // mounted on for the depth test. 5,019 ask for it.
    bool        m_bZBias;

    LTSpriteDraw() : m_fScaleX(0.5f), m_fScaleY(0.5f), m_fAlpha(1.0f),
                     m_nBlendOp(0), m_bGlow(false), m_bZBias(false)
    { m_Colour[0] = m_Colour[1] = m_Colour[2] = 1.0f; }
};

// ⚠️ How a moving brush -- a door -- tells the renderer where it is now. The
// renderer must not know what a door IS; it only needs to ask each model
// whether it has a transform this frame.
// ⚠️ OBJECTS THE WORLD FILE DOES NOT KNOW ABOUT -- every character in the game.
//
// Pass 3 of Render draws objects by reading the WORLD FILE: an object carries
// `Filename` and `Skin` properties and that is what gets drawn. Characters are
// not like that. An AI's model comes from the bute manager at runtime
// (CAI::InitialUpdate -> SetObjectFilenames), so the world file names nothing
// and pass 3 correctly draws nothing. Its own comment said so: "What is NOT
// here: characters."
//
// ⚠️ AND NOTHING ELSE DREW THEM. `ILTClient::RenderCamera` -- the whole in-game
// view -- calls Render and nothing more, so every game-spawned model in every
// level was never submitted to a rasteriser. The client scene held thirteen
// loaded, visible characters and the frame contained none of them. That is
// "there are no NPCs at all".
//
// This is the source for those objects. Whoever owns the live object state
// implements it; the renderer only reads.
// ⚠️ ABI-IDENTICAL TO THE SDK'S NodeControlFn -- (HOBJECT, HMODELNODE,
// LTMatrix*, void*) is (pointer, uint32, pointer, pointer). Declared without
// the SDK types so this header stays free of lithtech.h; the server fills it
// from the game's own callback. See LTModelDraw::NodeHook.
typedef void (*LTDynNodeFn)(void* hObj, unsigned int nNode, float* pGlobal16, void* pUser);

// ⚠️ AN EXTRA TRACKER (ILTModel::AddTracker) -- the blink, the twitch, the
// pitch. Blended onto the main animation through its weight set by
// LTModelDraw::BuildPose; listed in the order the game added them.
struct LTDynamicLayer
{
    int     m_nAnim;
    float   m_fAnimTime;        // seconds since that animation started
    bool    m_bLooping;
    int     m_nWeightSet;
};

struct LTDynamicModel
{
    // ⚠️ A CONSTRUCTOR -- the callers declare one of these on the stack and
    // hand it to Get(), and a garbage m_pfnNode would be called.
    LTDynamicModel()
        : m_nAnim(-1), m_fAnimTime(0.0f), m_bLooping(false),
          m_pfnNode(0), m_hNodeObj(0), m_pNodeUser(0), m_bReallyClose(false),
          m_fAlpha(1.0f), m_nBlendOp(0), m_bNoLight(false)
    {
        for (int k = 0; k < 3; ++k) { m_Pos[k] = 0.0f; m_Scale[k] = 1.0f; m_Colour[k] = 1.0f; }
        m_Quat[0] = m_Quat[1] = m_Quat[2] = 0.0f; m_Quat[3] = 1.0f;
    }

    std::string sModel;         // .abc
    std::string sSkins;         // ';'-joined, the same form a world object uses
    float       m_Pos[3];
    float       m_Quat[4];      // x y z w -- identical convention to the world
                                // pass's Euler matrix, verified to 1e-15
    float       m_Scale[3];
    int         m_nAnim;        // -1 = bind pose
    float       m_fAnimTime;    // seconds since that animation started
    bool        m_bLooping;

    // ⚠️ THE GAME'S NODE CONTROL for this object, if it installed one
    // (ILTClient::ModelNodeControl on the CLIENT mirror). Null when none.
    LTDynNodeFn m_pfnNode;
    void*       m_hNodeObj;     // the handle the game registered it on
    void*       m_pNodeUser;

    std::vector<LTDynamicLayer> m_Layers;

    // ⚠️ FLAG_REALLYCLOSE -- the player-view weapon and what rides on it.
    // m_Pos/m_Quat are already in WORLD space (the provider composed them with
    // the camera); the renderer draws these LAST, depth cleared, with a near
    // plane close enough that a gun a few units from the eye is not clipped
    // and no wall can cut through it -- LithTech's contract for the flag
    // (ltbasedefs.h:1106).
    bool        m_bReallyClose;

    // ⚠️ HOW IT IS COMPOSITED -- the object's SetObjectColor alpha and its
    // FLAG2_ADDITIVE / FLAG2_MULTIPLY, the same pair a world prop carries
    // (LT3DTexture::m_nBlendOp). Defaults are "opaque, normal", which is what
    // every game-spawned model got before these existed. A model for which
    // LTIsTranslucentDraw says yes is drawn after the opaque ones, back to
    // front: the intel popup's 0.9-alpha MULTIPLY backdrop (FX.txt
    // `PopupBackground`) covered the whole view, opaque, until it was.
    float       m_fAlpha;
    int         m_nBlendOp;     // 0 over, 1 additive, 2 multiply

    // ⚠️ FLAG_NOLIGHT -- "just use the object's color and global light
    // scale" (ltbasedefs.h:1123): lit by m_Colour alone, not by the level.
    // Every scale FX is made with it (FXButeMgr.cpp:3320), so the intel
    // popup's letter was lit by the dim room instead of shown white.
    bool        m_bNoLight;
    float       m_Colour[3];    // SetObjectColor r, g, b
};

// Is FLAG_NOLIGHT honoured for game-spawned models? LT_LITNOLIGHT=1 lights
// them by the level as before (announces itself).
bool LTNoLightEnabled();

// Do game-spawned models carry their alpha and blend mode? LT_NODYNBLEND=1
// draws every one of them opaque, as before (announces itself).
bool LTDynamicBlendEnabled();

// The near plane of the FLAG_REALLYCLOSE pass: the donor's `reallyclose_near`
// console variable, 0.1 (rendererconsolevars.h:38). LT_RCNEAR25=1 restores
// the port's old 0.25, which clipped the intel popups placed 0.215 from the
// eye -- the letter, ticket, manifest, roster, log and security pass
// (announces itself).
float LTReallyCloseNear();

// ⚠️ A SPRITE THE GAME MADE AT RUN TIME -- muzzle flashes, impact puffs,
// smoke. Drawn by the world renderer's own sprite rule (DrawSpriteQuad) in the
// translucent pass; m_bReallyClose ones (the flash on the view weapon) in pass
// 6 with it. Position already in WORLD space.
struct LTDynamicSprite
{
    LTSpriteDraw    m_Draw;
    float           m_Pos[3];
    bool            m_bReallyClose;
    bool            m_bNoZ;         // FLAG_SPRITE_NOZ -- drawn without the depth test
    LTDynamicSprite() : m_bReallyClose(false), m_bNoZ(false) { m_Pos[0] = m_Pos[1] = m_Pos[2] = 0.0f; }
};

class ILTWorldDynamicObjects
{
public:
    virtual ~ILTWorldDynamicObjects() {}
    virtual unsigned int Count() const = 0;

    // Fills `out` for object i and returns true if it should be drawn.
    virtual bool         Get(unsigned int i, LTDynamicModel& out) const = 0;

    // Sprites, if the source has any. Default: none.
    virtual unsigned int SpriteCount() const { return 0; }
    virtual bool         GetSprite(unsigned int /*i*/, LTDynamicSprite& /*out*/) const { return false; }

    // PolyGrids (water surfaces), if the source has any. Default: none.
    virtual unsigned int PolyGridCount() const { return 0; }
    virtual bool         GetPolyGrid(unsigned int /*i*/, struct LTDynamicPolyGrid& /*out*/) const { return false; }
};

// ⚠️ ONE POLYGRID AS THE RENDERER SEES IT -- see LTScenePolyGrid
// (ltscene.h) for the layout. The pointers belong to the game object and are
// valid for this frame only.
struct LTDynamicPolyGrid
{
    float               m_Pos[3];
    float               m_Quat[4];          // x y z w
    float               m_Scale[3];         // what FitPolyGrid computed
    unsigned int        m_W = 0, m_H = 0;
    const signed char*  m_pData = 0;        // W*H heights, -127..127
    const float       (*m_pTable)[4] = 0;   // 256 colours, index height + 128
    std::string         m_sSprite;          // a .spr; its frame 0 is the texture
    float               m_xPan = 0.0f, m_yPan = 0.0f, m_xScale = 1.0f, m_yScale = 1.0f;
    float               m_Colour[3];        // SetObjectColor r g b, 0..1
    float               m_fAlpha = 1.0f;
    int                 m_nBlendOp = 0;     // 0 over, 1 additive, 2 multiply
};

// Are PolyGrids made and drawn? LT_NOPOLYGRID=1 refuses OT_POLYGRID and keeps
// SFX_POLYGRID_ID gated, as before (announces itself).
bool LTPolyGridEnabled();

// The TRANSLUCENT game-spawned models (LTIsTranslucentDraw on their
// m_fAlpha / m_nBlendOp) of one kind -- really-close or not -- as indices for
// Get(), FAR TO NEAR from pEye3. One list for both renderers, so the order
// that decides the colour cannot differ between them.
std::vector<unsigned int> LTTranslucentDynamics(const ILTWorldDynamicObjects* pDyn,
                                                bool bReallyClose, const float* pEye3);

// The PolyGrids, as indices for GetPolyGrid(), FAR TO NEAR -- they are drawn
// with the translucent models, after them. Empty when LTPolyGridEnabled() is off.
std::vector<unsigned int> LTPolyGridsBackToFront(const ILTWorldDynamicObjects* pDyn,
                                                 const float* pEye3);


class ILTWorldModelXform
{
public:
    virtual ~ILTWorldModelXform() {}
    virtual bool GetModelTransform(const std::string& sName, float* pOut16) const = 0;

    // ⚠️ IS THIS BRUSH'S OBJECT INVISIBLE? A world model that belongs to an
    // object is drawn only while that object has FLAG_VISIBLE -- the engine's
    // rule. Volume brushes (Filter, Ladder, Water, Weather ...) are authored
    // with real textures (`tex\sound.dtx` reads "SOUND FILTER") and never set
    // it; drawing them by default put sound-filter walls across M12S03.
    // Default: nothing is hidden (walk mode has no server to ask).
    virtual bool IsModelHidden(const std::string& /*sName*/) const { return false; }

    // ⚠️ IS THIS BRUSH'S OBJECT TRANSLUCENT, AND HOW MUCH? (HANDOVER 5cf) An
    // object's alpha (SetObjectColor; a TranslucentWorldModel's `Alpha`)
    // scales its whole world model -- LT2 draws a world model whose m_ColorA is
    // below 255 in the translucent pass, every surface modulated by it
    // (render_a drawworldmodel.cpp). True, with *pAlpha in [0, 1), only for a
    // model that must be drawn that way; then every pass handles it with
    // LTWorldModelAlpha below. Default: none (walk mode has no server).
    virtual bool GetModelAlpha(const std::string& /*sName*/, float* /*pAlpha*/) const { return false; }
};

// ⚠️ WHERE TRIANGLES GO. The software rasteriser is not the only possible
// destination, but the DECISIONS about which triangles exist, what their
// texture coordinates are and how they are lit must stay in one place -- so a
// backend receives finished triangles and nothing else.
//
// `pWorld3x3` is the three vertices in WORLD space (what a GPU wants, since it
// applies the matrix itself); `pVerts3` is the same three already transformed
// and shaded, which is what the software path wants.
class ILTWorldGeomSink
{
public:
    virtual ~ILTWorldGeomSink() {}
    virtual void Tri(const LT3DTexture* pTex,
                     const float pWorld3x3[3][3],
                     const LT3DVert* pVerts3) = 0;
};

// ⚠️ LINEAR DEPTH FOG, as the level asks for it (HANDOVER 5bo). Filled by
// the host from the client console every frame (FogEnable/FogNearZ/FogFarZ/
// FogR,G,B and SkyFogEnable/SkyFogNearZ/SkyFogFarZ -- the game copies them
// there from WorldProperties, CGameClientShell::ResetGlobalFog, and a water
// volume overrides them while the camera is in it). Off until set.
struct LTFogState
{
    bool    m_bOn;
    float   m_fNear, m_fFar;
    float   m_RGB[3];       // 0..255
    LTFogState() : m_bOn(false), m_fNear(0.0f), m_fFar(1.0f) { m_RGB[0] = m_RGB[1] = m_RGB[2] = 0.0f; }
};

// Everything one world needs to draw, loaded once.
class CLTWorldScene
{
public:

    // The fog the next frames draw with: the world's and the sky's.
    void                SetFog(const LTFogState& world, const LTFogState& sky)
    { m_FogWorld = world; m_FogSky = sky; }
    const LTFogState&   FogWorld() const { return m_FogWorld; }
    const LTFogState&   FogSky() const   { return m_FogSky; }

    // The far clip plane for the world pass: the client's FarZ console var
    // (HANDOVER 5bp). <= 0 = none.
    void                SetFarZ(float f) { m_fFarZ = f; }
    float               FarZ() const     { return m_fFarZ; }
    // What both renderers pass as the world projection's far value: FarZ when
    // set, else the old fixed 40000.
    float               WorldFar() const { return m_fFarZ > 0.0f ? m_fFarZ : 40000.0f; }

    CLTWorldScene();
    ~CLTWorldScene();

    // Loads the world and resolves its textures, sky and object models.
    // Returns false and leaves the scene empty on anything it cannot do;
    // GetError() says why.
    bool                Load(CLTFileMgr* pFileMgr, const char* pWorldName);

    bool                IsLoaded() const { return m_bLoaded; }
    const char*         GetError() const { return m_sError.c_str(); }

    // ⚠️ The world's own spawn, not an invented one: GameStartPoint's Pos and
    // its yaw. Returns false when the world has none.
    bool                GetSpawn(float* pPos3, float* pYaw) const;

    // ⚠️ The eye height the camera is placed at above the spawn point. NOLF's
    // player model stands about this tall; named rather than sprinkled as a
    // literal so the two callers cannot disagree.
    static float        EyeHeight() { return 48.0f; }

    struct Stats
    {
        int m_nWorldTris;
        int m_nObjTris;
        int m_nPixels;
        int m_nSkyPixels;       // pixels the world never covered
        int m_nObjects;
        int m_nDynamic;         // of those, game-spawned (pass 3b)
    };

    // Draws into a caller-owned RGBA8 target with a matching float depth
    // buffer. Both must be w*h; depth may not be null -- the sky pass depends
    // on it.
    Stats               Render(unsigned char* pRGBA, float* pDepth,
                               int nWidth, int nHeight,
                               const float* pEye3, const float* pAt3,
                               float fFovY,
                               const ILTWorldModelXform* pXform = 0,
                               const ILTWorldDynamicObjects* pDyn = 0);

    // ⚠️ Draws this scene through the Metal backend instead of the software
    // rasteriser. Returns false when Metal is unavailable, and the caller must
    // fall back rather than treat that as fatal -- the software path is the
    // reference and is never removed.
    // Same as RenderMetal, but into an offscreen target that can be read back
    // -- the only way to compare the two backends without a human looking at a
    // window. Fills pRGBA (w*h*4) on success.
    bool                RenderMetalOffscreen(unsigned char* pRGBA,
                                             int nWidth, int nHeight,
                                             const float* pEye3, const float* pAt3,
                                             float fFovY,
                                             const ILTWorldModelXform* pXform = 0,
                                             const ILTWorldDynamicObjects* pDyn = 0);

    // Game-spawned models the last Metal frame drew (the census counts them).
    unsigned int        LastMetalDynamic() const { return m_nLastMetalDynamic; }

    // ⚠️ THE COMPOSITOR'S CAMERA (HANDOVER 5bq): the same passes, into the
    // native-size 3D target LTMetal_BeginScene keeps, restricted to the
    // camera's output rectangle, NOT read back. The projection's aspect is the
    // rectangle's, so a wider window shows more to the sides (the game's
    // vertical field of view is kept). Light scale and the camera's light add
    // are applied to the 3D only, before any 2D.
    struct MetalSceneTarget
    {
        int     m_OutW, m_OutH;             // the 3D target (the output)
        int     m_X, m_Y, m_W, m_H;         // this camera's rectangle in it
        bool    m_bClear;                   // the frame's first camera
        float   m_Scale[3], m_Add[3];
    };
    bool                RenderMetalScene(const MetalSceneTarget& t,
                                         const float* pEye3, const float* pAt3,
                                         float fFovY,
                                         const ILTWorldModelXform* pXform,
                                         const ILTWorldDynamicObjects* pDyn);

    bool                RenderMetal(int nWidth, int nHeight,
                                    const float* pEye3, const float* pAt3,
                                    float fFovY,
                                    const ILTWorldModelXform* pXform = 0);

    const CLTWorld&     GetWorld() const { return m_World; }

    // ⚠️ THE SURFACE TYPE OF A POLYGON -- what ILTServer/ILTClient::
    // GetPolyTextureFlags answer. It is the DTX's user-flags byte of the
    // polygon's texture (the same value SurfaceUnder reads), loaded on demand.
    // False for an id this port did not hand out.
    bool                PolySurfaceType(unsigned int nPolyId, unsigned int* pType);

    // ⚠️ THE CONSOLE'S `ModelAdd`, 0..255 per channel -- the constant every
    // model's light gets on top of the world's. The shipped autoexec.cfg says
    // "50 50 50", which is the default here; the host sets it from the console
    // so a user's own value wins. See BuildModelLights.
    static void         SetModelAdd(const float* pRGB255);

    // The PropTypes attribute file this scene resolved its props against.
    const CLTPropTypes& GetPropTypes() const { return m_PropTypes; }

    // ⚠️ DECODE EVERY TEXTURE AND MODEL THIS WORLD USES, NOW. Textures are
    // otherwise decoded the first time a surface carrying them becomes
    // visible, which is invisible in a still and awful to PLAY: measured on
    // M01S01, the first frame costs 86 ms against 20-36 ms for every frame
    // after it, and the same spike happens again on walking into any area
    // whose textures have not been seen yet.
    //
    // ⚠️ NOT called from Load(), deliberately. The test suites load worlds
    // repeatedly and only ever render a handful of views; making them decode
    // every texture in the level would cost minutes for no coverage. The HOST
    // calls this because the host is the thing a human plays.
    //
    // Returns how many textures and models it resolved.
    void                PreloadAll(int* pnTextures, int* pnModels);

    // The MoveToFloor adjustment applied to object i (0 when none). Exposed so
    // the placement can be asserted without re-deriving it.
    float               GetFloorDrop(size_t i) const
                        { return i < m_ObjectDrop.size() ? m_ObjectDrop[i] : 0.0f; }

    // The highest drawn surface at or below fFromY under (x, z). Public because
    // asserting that MoveToFloor did not SINK anything needs to ask the same
    // question independently of the answer it produced.
    bool                FloorUnder(float fX, float fZ, float fFromY,
                                   float fMaxDrop, float* pOutY) const;

    // ⚠️ The surface type of whatever is under (x, z) -- what the player is
    // standing on. Returns -1 when nothing is found; 0 is "Unknown", a real
    // surface, and must not be confused with "no answer".
    //
    // ⚠️ NOT const, and that is deliberate: the surface type lives on the
    // texture, so this may have to LOAD one. A const version silently returned
    // "nothing" for any texture the renderer had not drawn yet -- correct after
    // the first frame and wrong before it, which is the worst kind of nearly.
    int                 SurfaceUnder(float fX, float fZ, float fFromY,
                                     float fMaxDrop);

    // ⚠️ Front-to-back polygon ordering. On by default (LT_NOSORT disables it);
    // exposed so a test can render the SAME scene both ways, because the
    // property worth asserting is that it changes the pixel COUNT and not the
    // pixels. A pure optimisation has to be provable as one.
    void                SetFrontToBack(bool b) { m_bSort = b; }
    bool                GetFrontToBack() const { return m_bSort; }

    // Returns false when the object is not a drawable model at all.
    //
    // ⚠️ PUBLIC, and for the same reason it exists at all. The rule that a
    // PropType object's model comes from Attributes\PropTypes.txt and not from
    // its own `Filename` property is ONE decision, and a tool that inspects
    // props has to make the same one the renderer does. `harness/propview`
    // matched on the raw `Filename` for a while and therefore could not see a
    // single PropType-driven prop -- it reported "0 match" for a model with 84
    // placements. An inspection tool that disagrees with the renderer about
    // what is on screen is worse than no tool.
    // ⚠️ pbChromakey, when given, receives the AUTHORED "use this texture's
    // transparency" flag -- from Attributes\PropTypes.txt for a PropType, and
    // from the object's own property otherwise. It is the only thing that
    // separates a cut-out prop from a solid one; see CLTPropTypes::Entry.
    // ⚠️ pfAlpha, when given, receives the object's TRANSLUCENCY -- `Alpha` in
    // Attributes\PropTypes.txt for a PropType and the object's own `Alpha`
    // property otherwise. Anything below 1.0 means the model is blended rather
    // than drawn solid; `Prop::PostPropRead` hands exactly this number to
    // SetObjectColor, which is what makes a spotlight beam a beam.
    // ⚠️ pnBlendOp receives 0 for ordinary source-alpha, 1 ADDITIVE, 2
    // MULTIPLY -- `Additive` and `Multiply` beside the `Alpha` above. An
    // object with either is composited whatever its Alpha says.
    // ⚠️ THE GAME'S PLACEMENT RULES FOR OBJECT i (HANDOVER 5bt), one answer
    // for the floor drop and both object passes. A PropType takes MoveToFloor
    // from its type, and the type's Scale unless the level set one.
    void                ObjectScale(size_t i, float* pScale3) const;
    bool                ObjectMoveToFloor(size_t i) const;

    bool                ResolveObjectModel(size_t i, std::string& sModel,
                                           std::string& sSkin,
                                           bool* pbChromakey = 0,
                                           float* pfAlpha = 0,
                                           int* pnBlendOp = 0) const;

    // ⚠️ The sprite half of the same question, and PUBLIC for the same reason:
    // a tool that inspects what is on screen has to ask the renderer, not
    // re-derive it. Returns false when object i is not a drawable sprite --
    // including when it asks not to be (`StartOn` false on 82 of them).
    bool                ResolveObjectSprite(size_t i, LTSpriteDraw& out) const;
    // True when the game has hidden or removed world-file object i since the
    // level started (see LTWorldScene_SetLiveQuery). Both object passes ask.
    bool                HiddenNow(size_t i) const;

private:

    LTFogState          m_FogWorld, m_FogSky;
    float               m_fFarZ;

    struct CachedTex
    {
        // ⚠️ The DTX's m_UserFlags: NOLF's SURFACE TYPE. See ltsurfaces.h for
        // how that was established. Read at load because the alternative is
        // re-opening the .dtx every footstep.
        int                         m_nSurfaceType;

        std::vector<unsigned char>  m_RGBA;
        std::vector<unsigned char>  m_MipRGBA[LT3DTexture::kMaxMips];
        LT3DTexture                 m_Tex;
        bool                        m_bValid;

        // Read only for GetObjectAlphaTexture's entries, which set it: the
        // texture's alpha channel as a constant 0..1, or -1 when it varies.
        float                       m_fChanAlpha;
    };

    // ⚠️ A model can carry SEVERAL skins. The object's `Skin` property is one
    // string holding a ";"-separated list, one entry per texture index its
    // pieces use, and they are indexed BY POSITION -- so a skin that fails to
    // load still occupies its slot or every later piece takes the wrong one.
    enum { kMaxSkins = 8 };

    struct CachedModel
    {
        CLTABCModel                 m_Model;
        std::vector<unsigned char>  m_SkinRGBA[kMaxSkins];

        // ⚠️ AND THE MIP CHAIN, which prop skins did NOT have. World textures
        // have carried theirs since the corridor-banding work; model skins were
        // loaded at level 0 alone, so a tree seen from forty metres sampled one
        // texel in ten thousand. On an ALPHA-TESTED skin that is not merely
        // aliasing, it decides whether the pixel exists at all -- a sub-texel
        // difference in where the two rasterisers land picks a different texel
        // and the alpha test then answers differently. GUERILLA_NIGHT_DM's
        // conifers were a fine speckle of disagreement across every branch.
        std::vector<unsigned char>  m_SkinMipRGBA[kMaxSkins][LT3DTexture::kMaxMips];

        LT3DTexture                 m_Skins[kMaxSkins];
        int                         m_nSkins;
        bool                        m_bValid;

        // ⚠️ THE REST POSE a world prop is drawn in (HANDOVER 5bt): its "World"
        // animation if it has one, else animation 0, at keyframe 0 -- what the
        // game sets (Prop.cpp) and what LithTech always draws a model through.
        // -1 not built yet, 0 none (draw the stored vertices), 1 valid.
        LTModelPose                 m_RestPose;
        int                         m_nRestPose = -1;
        int                         m_nRestAnim = 0;    // the animation it was built from

        // ⚠️ THE ANIMATED PROP POSE (5cg), rebuilt only when the keys or the
        // fraction change -- a still prop never touches it. See PropPose.
        LTModelPose                 m_AnimPose;
        int                         m_nAnimPoseAnim = -1;
        unsigned int                m_nAnimPoseK0 = 0, m_nAnimPoseK1 = 0;
        float                       m_fAnimPoseFrac = -1.0f;

        // ⚠️ THE MODEL'S MESH ON THE GPU (HANDOVER 5cd), uploaded on first
        // draw by the Metal pass and freed with this record: 0 not yet, ~0u
        // could not be built (that model keeps the CPU path). One corner
        // range per piece, LOD 0, in piece order.
        unsigned int                m_nGpuMesh = 0;
        std::vector<int>            m_GpuFirst, m_GpuCount;
        ~CachedModel();
    };

    // The Metal pass registers how its GPU meshes are freed; ltworldrender
    // must not depend on the Metal backend (the software tools link it).
    static void SetGpuMeshFree(void (*pfn)(unsigned int));

    // The rest pose to draw a prop's model through, or null (HANDOVER 5bt).
    const LTModelPose*  PropRestPose(CachedModel* pM) const;

    // ⚠️ THE POSE A LEVEL PROP IS DRAWN IN (HANDOVER 5cg): its live animation
    // from the game (Prop.cpp plays `World`, a level script `ANIM`/`ANIMLOOP`),
    // between keys, when that animation has more than one key; otherwise --
    // walk mode, no server, a one-key animation -- exactly PropRestPose.
    // LT_NOPROPANIM=1 = the rest pose always (says so).
    const LTModelPose*  PropPose(size_t i, CachedModel* pM) const;

    // ⚠️ WHICH MODEL AND SKINS DOES OBJECT i ACTUALLY GET? One answer, used by
    // BOTH backends. The software and GPU object passes each had their own
    // copy of this decision, and the PropType attribute-file override is
    // exactly the kind of rule that would have gone into one and not the
    // other -- which is how the scene mirror and the model mirror each drifted
    // once already today.
    //


    // ⚠️ bBlend IS PART OF THE CACHE KEY, not just a field to set. Alpha state
    // rides on the LT3DTexture, the Metal sink batches its draw runs BY
    // TEXTURE POINTER, and one texture can appear on both an ordinary surface
    // and a SURF_TRANSPARENT one. Sharing an entry would let whichever surface
    // was loaded first decide how the other is drawn -- and would merge the
    // opaque and blended runs into a single batch, which cannot be ordered.
    const LT3DTexture*  GetTexture(const std::string& sName, bool bBlend = false);

    // ⚠️ ONE CACHE ENTRY PER (texture, alpha, blend op), because a sprite's
    // translucency is the OBJECT's and one flare texture is shared by hundreds
    // of objects at a dozen different alphas. Same argument as the model
    // cache's alpha key; 34 distinct colour+alpha combinations exist over all
    // 5,191 placements, so the cache stays small.
    const LT3DTexture*  GetSpriteTexture(const std::string& sName,
                                         float fAlpha, int nBlendOp);

    // A plain WHITE texture with one object's alpha and blend op -- for
    // geometry the game gave colours but no texture (an untextured PolyGrid:
    // M05S01's `gloop`), so it still goes through the blended rasteriser.
    const LT3DTexture*  GetWhiteTexture(float fAlpha, int nBlendOp);

    // ⚠️ A WORLD-MODEL TEXTURE CARRYING ITS OBJECT'S ALPHA (HANDOVER 5cf).
    // ONE entry per texture, not per alpha: the alpha is written into it on
    // every call, so a slide fading in over a second does not mint (and the
    // Metal path upload) a texture per frame. Safe because each translucent
    // model is drawn and flushed before the next one asks -- see
    // ModelObjectAlpha's callers. The texel's alpha joins exactly as for an
    // ordinary sprite (GetSpriteTexture): a varying channel multiplies, a
    // constant one folds into the constant.
    const LT3DTexture*  GetObjectAlphaTexture(const std::string& sName, float fAlpha);

    // The object alpha a world model is drawn with, from the server's xform
    // (ILTWorldModelXform::GetModelAlpha); false = drawn as before.
    static bool         ModelObjectAlpha(const LTWorldModelHeader& M,
                                         const ILTWorldModelXform* pXform, float* pAlpha);

    // Draws one sprite as two triangles, through whichever backend is asking.
    void                DrawSpriteQuad(const LT3DTarget& t, const float* pMVP,
                                       const LTSpriteDraw& sd, const float* pPos3,
                                       const float* pRight3, const float* pUp3,
                                       const float* pFwd3, const float* pEye3,
                                       Stats& st, ILTWorldGeomSink* pSink);

    // Draws one PolyGrid (water surface) as its (W-1)(H-1)*2 triangles, the
    // same way: software raster, or pSink for Metal.
    void                DrawPolyGrid(const LT3DTarget& t, const float* pMVP,
                                     const LTDynamicPolyGrid& pg,
                                     Stats& st, ILTWorldGeomSink* pSink);
    // ⚠️ bChromakey is part of the CACHE KEY, not just an argument. One skin in
    // the shipped set (`props\skins\lamp_04.dtx`) is used by prop types with
    // OPPOSITE Chromakey, so a cache keyed on model+skin alone would hand the
    // second prop whichever decision the first one happened to make.
    // ⚠️ fAlpha IS ALSO PART OF THE KEY, for the same reason bChromakey is:
    // `WineBottle_01.abc` is placed both as a PropType at 0.999 and as a plain
    // Prop, and the skins carry the treatment.
    CachedModel*        GetModel(const std::string& sModel, const std::string& sSkin,
                                 bool bChromakey, float fAlpha = 1.0f,
                                 int nBlendOp = 0);

    // ⚠️ WHICH HALF OF THE GEOMETRY TO DRAW. A blended surface writes no depth,
    // so it MUST come after every opaque one -- which means the world is
    // traversed twice rather than sorted once. Passing this rather than
    // filtering at the call site keeps the "is this surface blended" rule in
    // ONE place; this port has been bitten four times by that decision living
    // in two.
    // kPassAll: every surface, transparent or not -- a world model drawn by
    // its OBJECT's alpha (5cf) is translucent as a whole.
    enum EGeomPass { kPassOpaque, kPassBlended, kPassAll };

    void                DrawModel(const LT3DTarget& t, const float* pMVP,
                                  const LTWorldModelHeader& m,
                                  const std::vector<const LT3DTexture*>& tex,
                                  Stats& st,
                                  const std::vector<int>* pOrder = 0,
                                  ILTWorldGeomSink* pSink = 0,
                                  EGeomPass ePass = kPassOpaque);

    // Front-to-back draw order for one model; null when it is not worth it.
    const std::vector<int>* BuildOrder(const LTWorldModelHeader& m,
                                       const float* pEye3);

    // ⚠️ BACK to front, for the blended pass -- and it ALWAYS sorts, however
    // few polygons there are. BuildOrder may decline on a small model because
    // front-to-back is only an optimisation there; here the order decides what
    // the pixel ends up being, so declining would be a wrong picture rather
    // than a slower one.
    const std::vector<int>* BuildBackToFrontOrder(const LTWorldModelHeader& m,
                                                  const float* pEye3);

    // Does this model carry any SURF_TRANSPARENT surface at all? Almost none
    // do, and pass 4 skips the whole model when the answer is no.
    bool                HasBlendedSurface(const LTWorldModelHeader& m) const;

    bool                Drawable(const LTWorldModelHeader& m) const;
    // ...and, per frame, whether the brush's OBJECT is visible right now
    // (ILTWorldModelXform::IsModelHidden). Every draw pass uses this one.
    bool                Drawable(const LTWorldModelHeader& m,
                                 const ILTWorldModelXform* pXform) const;

    bool                RenderMetalInto(bool bOffscreen, unsigned char* pRGBA,
                                        int nWidth, int nHeight,
                                        const float* pEye3, const float* pAt3,
                                        float fFovY,
                                        const ILTWorldModelXform* pXform,
                                        const ILTWorldDynamicObjects* pDyn);
    unsigned int        m_nLastMetalDynamic = 0;
    const MetalSceneTarget* m_pMetalScene = 0;  // set only inside RenderMetalScene
    void                ComputeFloorDrops();

    // ---- the level's MODEL LIGHTS -- see BuildModelLights ------------- //
    struct ModelLight
    {
        float   m_Pos[3];
        float   m_Color[3];     // 0..255, BrightScale applied
        float   m_fRadius;
        bool    m_bSpot;        // a DirLight: cone around m_Dir
        float   m_Dir[3];       // spot axis, unit
        float   m_fCosHalf;     // cos(FOV/2)
        std::string m_sName;    // for the trace
    };
    // ---- one game-spawned model, prepared for EITHER backend ----------- //
    struct DynamicDraw
    {
        CachedModel*                    m_pModel;
        float                           m_ObjMVP[16];
        float                           m_World[16];    // also the normal matrix
        bool                            m_bPose;
        LTModelPose                     m_Pose;
        bool                            m_bLit;
        LTModelDraw::LTModelLighting    m_Light;
        bool                            m_bMirror;
    };
    bool                PrepareDynamic(const LTDynamicModel& d, const float* MVP,
                                       DynamicDraw& out);

    void                BuildModelLights();
    void                ComputeModelLighting(const float* pPos3,
                                             LTModelDraw::LTModelLighting& out,
                                             bool bTrace) const;
    std::vector<ModelLight>                 m_ModelLights;
    float                                   m_WorldAmbient[3];  // 0..255

    CLTFileMgr*                             m_pFileMgr;
    CLTWorld                                m_World;
    CLTPropTypes                            m_PropTypes;
    const LTWorldModelHeader*               m_pSky;
    std::map<std::string, CachedTex*>       m_Textures;
    std::map<std::string, CachedModel*>     m_Models;
    // Scratch for BuildOrder, kept across frames so the sort does not allocate.
    std::vector<std::pair<float, int> >     m_SortKeys;
    std::vector<int>                        m_SortOrder;
    // Scratch for DrawPolyGrid, the same reason.
    std::vector<LT3DVert>                   m_PGVerts;
    std::vector<float>                      m_PGWorld;
    bool                                    m_bSort;

    // Per-object Y adjustment from MoveToFloor; 0 for objects without it.
    std::vector<float>                      m_ObjectDrop;

    // CPU texture -> Metal handle, filled on first use of the GPU backend.
    std::map<const LT3DTexture*, unsigned int>  m_MetalTex;

    // ⚠️ THE STATIC WORLD ON THE GPU (HANDOVER 5bu). Each world model's OPAQUE
    // triangles, recorded once per level by DrawModel itself into one GPU
    // buffer, a run per texture in the batcher's own order. What stays per
    // frame is what changes per frame: which models are drawn and with which
    // matrix. Built by the Metal backend (ltworldmetal.cpp) after a Begin;
    // Load only marks it stale, because this file links no Metal.
    struct StaticRun   { const LT3DTexture* m_pTex; unsigned int m_nFirst, m_nCount; };
    struct StaticModel { std::vector<StaticRun> m_Runs; unsigned int m_nTris = 0; };
    struct StaticWorld
    {
        bool                        m_bValid  = false;
        unsigned int                m_nBuffer = 0;      // LTMetal_CreateBuffer handle
        std::vector<StaticModel>    m_Models;           // indexed like GetModels()
    };
    StaticWorld                             m_Static;

    // ---- MIRRORS (HANDOVER 5de) ----------------------------------------- //
    //
    // ⚠️ A MIRROR IS A SURFACE EFFECT, not a flag: `Mirror Overlay` in the
    // surface's effect list (64 surfaces in 10 levels; no surface carries the
    // donor's SURF_MIRROR bit 23, and the donor never draws one). LT2 renders
    // the scene reflected through it when DrawPortals is on, then lays the
    // surface's own texture over it. Here: the mirror polygons are grouped by
    // plane at load; each frame up to kMirrorSlots planes that face the camera
    // and cover the most screen get a REFLECTION PASS -- the whole scene from
    // the mirrored camera, near plane on the mirror -- into a target the size
    // of the view. The mirror's polygons then write that target's pixel at
    // their own pixel (opaque pass) and their texture blends over it (blended
    // pass). A plane with no reflection this frame draws as before.
    enum { kMirrorSlots = 2 };
    struct MirrorPlane
    {
        size_t              m_nModel;       // index into GetModels()
        float               m_Plane[4];     // rest pose: n.x + d = 0, n unit
        float               m_Point[3];     // a point on it (rest pose)
        float               m_Min[3], m_Max[3];
        std::vector<int>    m_PolyIdx;      // its polygons, indices into the model
        int                 m_nSlot;        // this frame's reflection, -1 none
        float               m_Live[4];      // this frame's plane (a door moves it)
    };
    std::vector<MirrorPlane>                m_Mirrors;
    std::map<const LTWorldPoly*, int>       m_MirrorOfPoly;     // polygon -> plane
    std::set<const LTWorldModelHeader*>     m_MirrorModels;
    // Stand-in textures, one per (plane, real texture); stable pointers.
    std::map<std::pair<int, const LT3DTexture*>, LT3DTexture*> m_MirrorPseudo;
    int                                     m_ActiveMirror[kMirrorSlots];
    int                                     m_nActiveMirrors = 0;
    std::vector<unsigned char>              m_ReflRGBA[kMirrorSlots];
    std::vector<float>                      m_ReflDepth[kMirrorSlots];

    // Inside a reflection pass: the plane (kept side positive), the camera's
    // reflected up, and which target it renders into.
    bool                                    m_bReflecting = false;
    float                                   m_ReflPlane[4] = { 0, 1, 0, 0 };
    float                                   m_CamUp[3] = { 0, 1, 0 };
    int                                     m_nReflSlot = 0;
    // ...and WHICH plane: a reflection never draws its own mirror (5dg).
    int                                     m_nReflPlaneIdx = -1;
    // While the GPU's static world is recorded, mirror polygons always go
    // under their stand-in, so they become runs of their own.
    bool                                    m_bRecordingStatic = false;
    // Set only while pass 4 draws the mirrors' own textures over their
    // reflections -- the ordinary blended walk never draws a mirror.
    bool                                    m_bMirrorOverlay = false;
    // The reflection target's size in the Metal backend (the view's pixels).
    int                                     m_nReflW = 0, m_nReflH = 0;

    void                BuildMirrors();
    // The mirrors' own walks, the same for both backends: each reflected
    // plane's model, the matrix it is drawn with, its textures and its
    // polygons. bGlass: pass 4's -- BLENDED textures (the object-alpha ones
    // for a model translucent as a whole); the caller draws kPassBlended with
    // m_bMirrorOverlay set. !bGlass: pass 2's FILL, only for a model pass 2
    // skips (translucent by its object's alpha, 5cf); kPassOpaque.
    void                ForEachActiveMirror(bool bGlass, const float* MVP, const float* MVPBias,
                                            const ILTWorldModelXform* pXform,
                                            const std::function<void(const LTWorldModelHeader&,
                                                                     const float*,
                                                                     const std::vector<const LT3DTexture*>&,
                                                                     const std::vector<int>&)>& fn);
    LT3DTexture*        MirrorPseudo(int nPlane, const LT3DTexture* pReal);
    // Points a stand-in at its plane's reflection this frame: the slot, and
    // (software) the picture. ⚠️ AT EVERY USE, not once per frame -- a stand-in
    // is made the first time its polygon is drawn, after any per-frame pass.
    void                SyncMirrorPseudo(LT3DTexture* p) const;
    // The software reflections' size this frame (0: none drawn in software).
    int                                     m_nReflScreenW = 0, m_nReflScreenH = 0;
    bool                MirrorActive(int nPlane) const
                        { return nPlane >= 0 && nPlane < (int)m_Mirrors.size() && m_Mirrors[nPlane].m_nSlot >= 0; }
    // Chooses this frame's reflections (sets m_nSlot, m_Live); returns how many.
    int                 SelectMirrors(const float* pEye3, const float* pAt3, float fFovY,
                                      float fAspect, const ILTWorldModelXform* pXform);
    // The mirrored camera for plane nPlane: eye, look-at and up.
    void                ReflectCamera(int nPlane, const float* pEye3, const float* pAt3,
                                      float* pEyeOut, float* pAtOut, float* pUpOut) const;
    // Inside a reflection pass: P's x row negated once more (a reflection
    // flips handedness) and its near plane moved onto the mirror (oblique).
    void                ReflectProjection(float* P, const float* V) const;
    // m_ReflPlane / m_nReflPlaneIdx for a reflection of plane nPlane (5dg).
    void                SetReflectPlane(int nPlane);
    static bool         NoReflSelfSkip();

    bool                                    m_bLoaded;
    std::string                             m_sError;
};

// ⚠️ LT_UVPROBE in the GAME: writes the last in-game frame's per-pixel probe
// (triangle id, sampled texel, uv, mip level, and the finished 3D view) as
// <base>.probe.*. The host calls it beside every LT_DUMP_EVERY frame. Returns
// false, and writes nothing, when the probe is off. See ltworldrender.cpp.
bool LTWorldScene_ProbeDump(const char* pBase);

// ⚠️ THE LIVE STATE OF A WORLD-FILE OBJECT (2026-10-07, HANDOVER 5bg). Pass
// 3 draws level props and sprites from the world file, which knows only how a
// level STARTS: a glow the script switches on (M01S02 `charge1glow on`) never
// appeared, and a prop the game removed stayed drawn. The host installs this;
// it answers -1 (unknown -- fall back to the file), 0 (hidden or removed) or
// 1 (visible). Null (walk mode) means "use the file".
typedef int (*LTWorldLiveFn)(unsigned int nWorldIndex);
void LTWorldScene_SetLiveQuery(LTWorldLiveFn fn);

// ⚠️ AND WHERE THE GAME HAS IT (HANDOVER 5bt). The prop passes re-derived a
// level object's placement from the world file -- its own MoveToFloor ray,
// its own idea of what a floor is -- and disagreed with the server on 9 of 14
// trees in M02S01, which hit brushes the game's IGNORE_NONSOLID ray passes
// through. With this installed a prop is drawn at its live record's position
// and rotation: the drop, anything the game moved since, and a door carrying
// its knob (whose own carry is then not applied again). LT_FILEPLACEMENT=1 =
// the world file's placement (says so).
typedef bool (*LTWorldLivePoseFn)(unsigned int nWorldIndex, float* pPos3, float* pQuat4);
void LTWorldScene_SetLivePoseQuery(LTWorldLivePoseFn fn);
bool LTWorldScene_LivePose(size_t nWorldIndex, float* pPos3, float* pQuat4);

// ⚠️ AND ITS ANIMATION (5cg): the animation the game selected (-1 = none
// chosen: the tracker's default), seconds since it started, and whether it
// loops. False when there is no live object (walk mode).
typedef bool (*LTWorldLiveAnimFn)(unsigned int nWorldIndex, int* pnAnim, float* pfSeconds, bool* pbLoop);
void LTWorldScene_SetLiveAnimQuery(LTWorldLiveAnimFn fn);

#endif // __LT_WORLD_RENDER_H__
