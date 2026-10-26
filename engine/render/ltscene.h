// ----------------------------------------------------------------------- //
//
// MODULE  : ltscene.h
//
// PURPOSE : The object list, the camera, and RenderObjects.
//
//           This is where the game's own 3D calls finally reach the
//           rasteriser. Up to now `lt3d` and `ltmodeldraw` were only driven by
//           a harness tool; this is the engine path.
//
// ⚠️ THE MAIN MENU NEEDS EXACTLY ONE DRAW ENTRY POINT, AND THE CENSUS FOUND IT
//
//   `CInterfaceMgr::UpdateInterfaceSFX` (InterfaceMgr.cpp:5045) ends in
//
//       g_pLTClient->RenderObjects(hCamera, objs, next);
//
//   -- an explicit ARRAY of objects rendered through a camera. There is no
//   world, no BSP, no visibility query and no RenderCamera in this path. That
//   is what makes the menu background reachable long before a world loader
//   exists, and it is why this file is small.
//
//   The objects themselves arrive through `ILTClient::CreateObject`, which
//   carries the model filename AND its skins in one `ObjectCreateStruct`
//   (`m_Filename`, `m_SkinNames[MAX_MODEL_TEXTURES]`) -- so a model and its
//   textures are bound at creation, not looked up later.
//
// ⚠️ LTRotation IS A QUATERNION, NOT A MATRIX
//
//   `ltrotation.h:42` -- `Init(a,b,c,d)` defaults to `(0,0,0,1)`, "facing down
//   positive Z". So it maps straight onto LT3D::FromQuatPos, and ABC's
//   keyframes use the same representation. No conversion, and no chance of a
//   transposed matrix quietly mirroring the scene.
//
// CACHING
//
//   Models and decoded skins are cached BY NAME. The menu builds a character,
//   two attachments and eleven sprite/model FX every time the folder is
//   rebuilt; reloading and re-decoding a 512x512 DXT1 skin each time would be
//   pure waste. The cache owns its entries and outlives individual objects.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_SCENE_H__
#define __LT_SCENE_H__

#include "lt2d.h"
#include "lt3d.h"

#include <map>
#include <memory>
#include <string.h>
#include <string>
#include <vector>

class CLTABCModel;
class CLTDtx;
struct LTModelPose;

// ⚠️ THE MENU'S OBJECTS AS DRAW ITEMS (HANDOVER 5ce). CLTScene::RenderObjects
// decides everything -- camera, mirror, sprite quads, poses -- and in collect
// mode emits these instead of rasterising, so a GPU backend draws exactly
// what the software path would, at the output's native resolution.
struct LTSceneGpuItem
{
    int                 m_nKind;        // 0 model, 1 sprite
    const CLTABCModel*  m_pModel;
    const LT3DTexture*  m_pSkins;
    int                 m_nSkins;
    int                 m_nPose;        // index into the call's pose list, -1 = bind pose
    float               m_MVP[16];      // the software pass's matrix (GL clip)
    float               m_Normal[16];
    const LT3DTexture*  m_pTex;         // sprite: its texture
    float               m_Corner[4][3]; // sprite: world-space corners
    float               m_RGB[3];       // sprite: colour
};
class CLTFileMgr;
class CLTSprite;

// What CreateObject was asked for. A plain struct so this file does not have
// to agree with the SDK's ObjectCreateStruct layout -- the stub translates.
struct LTSceneCreateInfo
{
    unsigned int    m_nType;
    std::string     m_sFilename;
    std::vector<std::string> m_Skins;
    float           m_Pos[3];
    float           m_Quat[4];      // x, y, z, w
    float           m_Scale[3];
    unsigned int    m_Flags;
    unsigned int    m_Flags2;
};

struct LTScenePolyGrid;     // below

// ⚠️ A PARTICLE (HANDOVER 5dh). It BEGINS WITH iltclient.h's LTParticle,
// field for field -- m_Velocity, m_Color (0-255), m_Alpha, m_Size, m_pNext --
// because the game walks the list and writes those fields itself
// (BaseParticleSystemFX.cpp: m_Size and m_Alpha over a particle's life). The
// engine's own state follows. ltscene.cpp static_asserts the layout against
// the SDK's struct. Positions are RELATIVE to the system's position and
// rotation (iltclient.h).
struct LTSceneParticle
{
    float               m_Velocity[3];
    float               m_Color[3];
    float               m_Alpha;
    float               m_Size;
    LTSceneParticle*    m_pNext;
    // ---- the engine's ----
    float               m_Pos[3];
    float               m_fLifetime, m_fTotalLifetime;
    LTSceneParticle*    m_pPrev;
};

// ⚠️ AN OT_PARTICLESYSTEM (ILTClient SetupParticleSystem ... iltclient.h:864):
// smoke, steam, sparks, trails, the particle half of explosions. A circular
// list around a SENTINEL: GetParticles hands out (first, sentinel) and the game
// walks `while (p != tail)`. Defaults are iltclient.h's (gravity -500, radius
// 300); the update is the donor's ps_UpdateParticles.
struct LTSceneParticleSystem
{
    std::string         m_sTexture;
    float               m_fGravity = -500.0f;
    unsigned int        m_nFlags = 0;           // PS_BOUNCE, PS_NEVERDIE, PS_DUMB
    float               m_fRadius = 300.0f;
    LTSceneParticle     m_Head;                 // the sentinel
    unsigned int        m_nCount = 0;
    // A placed ParticleSystem's rate (DEVIATIONS E-009, 5dj): read from its
    // effect message, so AddParticles can add back what the game's
    // floor(rate x dt) dropped. 0 = not a placed ParticleSystem.
    float               m_fRate = 0.0f;
    float               m_fRateCarry = 0.0f;
    double              m_fLastAdd = -1.0;
    bool                m_bBounceSaid = false;  // LT_PARTTRACE: first bounce reported (5dk)
    // An ANIMATED .spr texture (5dm): the sprite, and the system's own clock
    // in ms -- the donor's SpriteTracker, one per system, started at setup.
    const CLTSprite*    m_pSprite = nullptr;
    float               m_fSprMs = 0.0f;
    LTSceneParticleSystem() { memset(&m_Head, 0, sizeof(m_Head)); m_Head.m_pNext = m_Head.m_pPrev = &m_Head; }
    ~LTSceneParticleSystem()
    {
        LTSceneParticle* p = m_Head.m_pNext;
        while (p != &m_Head) { LTSceneParticle* n = p->m_pNext; delete p; p = n; }
    }
    LTSceneParticleSystem(const LTSceneParticleSystem&) = delete;
    LTSceneParticleSystem& operator=(const LTSceneParticleSystem&) = delete;
};

struct LTSceneObject
{
    unsigned int    m_nType;
    float           m_Pos[3];
    float           m_Quat[4];
    float           m_Scale[3];
    unsigned int    m_Flags;
    unsigned int    m_Flags2;

    // ⚠️ THE GAME'S OWN FLAGS, which the engine never interprets and must
    // still carry. NOLF marks its live cinematic camera with USRFLG_CAMERA_LIVE
    // and the CLIENT reads it back off the object to decide whether to take the
    // view (GameClientShell.cpp:2443). A mirror that dropped them would be a
    // camera the client can see and can never be told is on.
    unsigned int    m_UserFlags;

    float           m_Color[4];         // r, g, b, a -- SetObjectColor

    std::string     m_sFilename;

    // Camera state. Meaningful only when m_nType == OT_CAMERA.
    float           m_fFovX, m_fFovY;

    // ⚠️ PHYSICS STATE ON THE CLIENT, and it is not a duplicate of the
    // server's for no reason: NOLF drives the PLAYER from the client.
    // `CPlayerObj` clears FLAG_GRAVITY with the comment "This is controlled by
    // the client", and CMoveMgr does every SetVelocity / GetAcceleration /
    // GetStandingOn on `g_pPhysicsLT` -- the CLIENT's. A port that implemented
    // only the server's physics moves the AI and not the player.
    float           m_Vel[3];
    float           m_Accel[3];
    float           m_Dims[3];
    bool            m_bOnGround;
    bool            m_bFullScreen;
    int             m_Rect[4];          // left, top, right, bottom

    // ---- animation ---------------------------------------------------- //
    //
    // ⚠️ The keyframe is derived from the CLOCK, not counted per frame. A
    // per-frame counter drifts with frame rate and would make the animation
    // play at a different speed than the game intends -- and, worse, would put
    // the frame contract at the mercy of how many Update() calls happened.
    int                         m_nAnim;        // -1 = none, draw the bind pose
    bool                        m_bLooping;
    float                       m_fAnimStart;   // engine seconds

    // ⚠️ CLIENT FLAGS, AND CF_NOTIFYMODELKEYS IS THE ONE THAT MATTERS. The
    // game asks for model-key notification per object (WeaponModel.cpp:1269,
    // CharacterFX.cpp:254, BodyFX.cpp:112) and the request was being thrown
    // away -- Get/SetObjectClientFlags were function-pointer stubs.
    unsigned int                m_ClientFlags;

    // ⚠️ HOW FAR THROUGH THE ANIMATION ITS KEYS HAVE BEEN DELIVERED, in
    // milliseconds; -1 means "none yet", so a key at time 0 still fires.
    // Reset whenever the animation changes or restarts, or a weapon would fire
    // once and never again.
    float                       m_fKeysUpToMs;

    // ⚠️ DRAWN BY THE GAME CAMERA (RenderCamera's client-model list), as
    // opposed to the menu's RenderObjects. The two draw a model differently --
    // the game view unmirrored with the origin offset applied (HANDOVER 5as,
    // 5ay), the menu mirrored without it -- and a SOCKET must be read the way
    // its model is drawn, or whatever is attached to it floats off (the view
    // rifle's scope, 5bd).
    bool                        m_bInGameView;

    // Resolved resources. Owned by the scene's caches, never by the object.
    const CLTABCModel*          m_pModel;
    std::vector<LT3DTexture>    m_Skins;
    std::vector<std::string>    m_SkinNames;    // as created -- the world renderer's
                                                // model cache is keyed by NAME

    // OT_POLYGRID only (SetupPolyGrid made it); null for everything else.
    std::unique_ptr<LTScenePolyGrid> m_pPolyGrid;

    // OT_PARTICLESYSTEM only, made with the object (5dh); null otherwise.
    std::unique_ptr<LTSceneParticleSystem> m_pPartSys;

    // ⚠️ HAS THIS CLIENT OBJECT HAD ITS ATTACHED EFFECT? LT2 delivers an
    // object's special-effect message once, when the object appears on the
    // client; a later StartSpecialEffectMessage only replaces the stored one.
    // Reset for every object when the client leaves a world (the game removes
    // all its effects there). See skeleton_host.cpp kLTMsg_SFX.
    bool                        m_bSFXDelivered = false;
};

// ⚠️ A POLYGRID -- water surfaces, the plasma in a tank (ILTClient
// SetupPolyGrid ... FitPolyGrid, iltclient.h:935). A W x H heightmap of signed
// bytes the GAME writes every frame through the pointer GetPolyGridInfo hands
// out, and a 256-entry colour table indexed by height + 128. Both pointers stay
// valid for the object's life, so neither may be reallocated after setup.
//
// The layout, from the donor's drawpolygrid.cpp: cell (i, j) sits at local
// x = (i - (W-1)/2), z = (j - (H-1)/2), y = height, then the OBJECT SCALE
// (which FitPolyGrid computes), its rotation and its position.
struct LTScenePolyGrid
{
    unsigned int        m_W = 0, m_H = 0;
    bool                m_bHalfTris = false;
    std::vector<char>   m_Data;
    float               m_Table[256][4];        // PGColor: r g b 0..255, a 0..255
    std::string         m_sSprite, m_sEnvMap;
    float               m_xPan = 0.0f, m_yPan = 0.0f, m_xScale = 1.0f, m_yScale = 1.0f;
};


// One animation key that has just been crossed. `m_sCommand` points into the
// loaded model and stays valid as long as the model is loaded, which is for
// the life of the scene's cache -- the host copies it into argv immediately.
struct LTModelKey
{
    void*       m_hObj;
    const char* m_pCommand;
};

class CLTScene
{
public:

    // Collects every model key crossed since the last call. See the .cpp.
    unsigned        TakeModelKeys(LTModelKey* pOut, unsigned nMax);


    CLTScene();
    ~CLTScene();

    void            Init(CLTFileMgr* pFileMgr);
    void            Term();

    // A .spr, loaded once and kept for the scene's life; 0 if it cannot be
    // read or has no frames (says so once on stderr).
    const CLTSprite* LoadSprite(const std::string& sSpr);

    // Returns 0 if the type is one this scene cannot represent yet. ⚠️ It
    // returns 0 rather than a live handle for an unsupported type, so the game
    // takes its own "could not create" path instead of holding a handle that
    // silently never draws.
    LTSceneObject*  CreateObject(const LTSceneCreateInfo& info);
    const std::vector<LTSceneObject*>& Objects() const { return m_Objects; }

    // ⚠️ ILTCommon::SetObjectFilenames ON THE CLIENT -- how the game swaps the
    // player-view weapon when Cate changes weapon (WeaponModel.cpp:1259): one
    // object for the whole level, its model and skins replaced. An empty
    // filename leaves the model alone; no skins leaves the skins alone.
    bool            SetObjectFilenames(LTSceneObject* pObj, const char* pFilename,
                                       const std::vector<std::string>& skins);
    bool            RemoveObject(LTSceneObject* pObj);
    bool            IsValid(const LTSceneObject* pObj) const;

    // Renders an explicit list through a camera, into dest. Returns the number
    // of pixels written -- "0 drawn" and "worked" must never look alike.
    int             RenderObjects(LTSceneObject* pCamera,
                                  LTSceneObject* const* ppObjects, int nObjects,
                                  const LT2DSurf& dest,
                                  std::vector<LTSceneGpuItem>* pCollect = 0,
                                  std::vector<LTModelPose>* pPoses = 0);

    void            SetGlobalLightScale(const float* pScale3);
    const float*    GetGlobalLightScale() const { return m_LightScale; }

    // Animation. Index -1 restores the bind pose.
    int             GetAnimIndex(const LTSceneObject* pObj, const char* pName) const;

    // ⚠️ By MODEL FILE rather than by object -- the server has no scene object,
    // only the model's name. Resolves through the same model cache the client
    // draws from, so the two halves cannot disagree about what an animation
    // index means. See LTScene_GetAnimIndexForFile.
    int             AnimIndexForFile(const char* pFilename, const char* pAnimName);

    // The shared model cache, for callers that need the model itself -- node
    // enumeration (GetNextModelNode) must walk the SAME file the renderer
    // poses, or HMODELNODE would mean different nodes on the two sides.
    const CLTABCModel*  ModelForFile(const std::string& sName) { return LoadModel(sName); }
    int             AnimLengthForFile(const char* pFilename, int nAnim);
    int             AnimInterpForFile(const char* pFilename, int nAnim);
    bool            AnimDimsForFile(const char* pFilename, int nAnim, float* pOut3);
    int             WeightSetForFile(const char* pFilename, const char* pSetName);

    // ⚠️ Attach a model to an object that already exists -- see the definition
    // for why a model can arrive after the object it belongs to.
    bool            SetObjectModel(LTSceneObject* pObj, const char* pFilename);
    bool            SetAnim(LTSceneObject* pObj, int nAnim, float fNow);
    bool            SetLooping(LTSceneObject* pObj, bool bLoop);

    // Index of a named socket, or -1. Case-insensitive, as iltmodel.h says.
    int             FindSocket(const LTSceneObject* pObj, const char* pName) const;

    // A socket's transform for the object's CURRENT animated pose.
    //
    // ⚠️ A socket is attached to a NODE, so its transform moves with the
    // animation -- socketWorld = objectTransform * nodePose * socketLocal.
    // Returning the stored socket transform alone would pin every attachment
    // to the bind pose while the character moved underneath it, which looks
    // like a physics bug rather than a missing multiply.
    bool            GetSocketTransform(const LTSceneObject* pObj, int nSocket,
                                       bool bWorldSpace,
                                       float* pPos3, float* pQuat4) const;

    // Census, for the run record and for answering "did anything happen".
    unsigned int    GetNumObjects() const { return (unsigned int)m_Objects.size(); }

    // ⚠️ Model objects by STATE, not just a count -- see the definition.
    void            DumpModelCensus() const;
    unsigned int    GetNumModelsLoaded() const { return (unsigned int)m_Models.size(); }
    unsigned int    GetNumSkinsLoaded() const { return (unsigned int)m_Skins.size(); }
    unsigned int    GetNumRenderCalls() const { return m_nRenderCalls; }
    unsigned int    GetNumModelsDrawn() const { return m_nModelsDrawn; }
    unsigned int    GetNumMissingModels() const { return m_nMissingModels; }
    unsigned int    GetNumMissingSkins() const { return m_nMissingSkins; }

private:

    struct SkinEntry
    {
        CLTDtx*                     m_pDtx;
        std::vector<unsigned char>  m_RGBA;     // decoded mip 0
        LT3DTexture                 m_Tex;
    };

    const CLTABCModel*  LoadModel(const std::string& sName);
    const SkinEntry*    LoadSkin(const std::string& sName);

    // ⚠️ MENU SPRITES (2026-10-06). The interface builds its backgrounds from
    // ScaleFX sprites -- the briefing, mission-status and summary screens all
    // sit on `menu\Spr\briefingbackground.spr` -- and RenderObjects drew
    // models only, so those screens were black and their BLACK text vanished.
    // The blended texture for a sprite's current frame, cached per
    // (sprite, frame, alpha, blend) exactly as CLTWorldScene::GetSpriteTexture.
    const LT3DTexture*  SpriteTexture(const std::string& sSpr, float fSeconds,
                                      float fAlpha, int nBlendOp);
    std::map<std::string, LT3DTexture*> m_SpriteTex;
    std::map<std::string, CLTSprite*>   m_Sprites;      // LoadSprite's cache

    CLTFileMgr*                             m_pFileMgr;
    std::vector<LTSceneObject*>             m_Objects;
    std::map<std::string, CLTABCModel*>     m_Models;
    std::map<std::string, SkinEntry*>       m_Skins;

    std::vector<float>                      m_Depth;    // grown to fit dest
    float                                   m_LightScale[3];

    unsigned int    m_nRenderCalls;
    unsigned int    m_nModelsDrawn;
    unsigned int    m_nMissingModels;
    unsigned int    m_nMissingSkins;
};

// The engine has one, for the same reason CLTFileMgr does: the game reaches it
// through interface pointers it is handed at runtime.
CLTScene* LTGetScene();

// Assigns the object/camera/render entry points, AFTER the generated defaults
// and alongside LTNullRender_Wire.
class ILTClient;
void LTScene_Wire(ILTClient* p);

// ⚠️ THESE FIVE ARE VIRTUALS ON THE INTERFACE, NOT FUNCTION POINTERS, so they
// cannot be assigned by LTScene_Wire -- the host class must override them and
// call these. HANDOVER.md §4.5: ILTClient is a HYBRID of virtuals, 208
// function-pointer members and inline-accessor data members, and mixing the
// two up fails with no diagnostic at all.
//
//   ILTCSBase::RemoveObject      iltcsbase.h:205
//   ILTClient::SetObjectPos      iltclient.h:800
//   ILTClient::GetObjectFlags    iltclient.h:815
//   ILTClient::SetObjectFlags    iltclient.h:816
//   ILTCommon::GetObjectType     iltcommon.h:94
int             LTScene_RemoveObject(void* hObj);
// ILTClient::AddParticle is a VIRTUAL too (iltclient.h:883); the host's
// override calls this. Returns the particle as the game's LTParticle*.
void*           LTScene_AddParticle(void* hObj, const float* pPos, const float* pVel,
                                    const float* pColor, float fLifetime);
// Moves and ages every particle system by fDt seconds -- once per client
// frame, after the shell's Update, where the donor's UpdateObjects runs (5dh).
void            LTScene_UpdateParticles(float fDt);
// The next OT_PARTICLESYSTEM created gets this rate (particles/second) -- set
// around the delivery of a ParticleSystem effect, 0 clears (E-009, 5dj).
void            LTScene_SetPendingParticleRate(float fRate);
int             LTScene_SetObjectPos(void* hObj, float x, float y, float z);

// ---- client physics state ---------------------------------------------- //
//
// ⚠️ The CLIENT's, and that is where the player's movement lives. See the
// members on LTSceneObject for why there are two sets of these in the engine.
bool  LTScene_GetVelocity(void* hObj, float* pV3);
bool  LTScene_SetVelocity(void* hObj, const float* pV3);
bool  LTScene_GetAcceleration(void* hObj, float* pA3);
bool  LTScene_SetAcceleration(void* hObj, const float* pA3);
// The model's per-animation bounding box. See the implementation for why a
// stub here makes the player a one-unit sphere.
bool  LTScene_GetAnimDims(void* hObj, int nAnim, float* pD3);

bool  LTScene_GetPhysDims(void* hObj, float* pD3);
bool  LTScene_SetPhysDims(void* hObj, const float* pD3);
bool  LTScene_GetOnGround(void* hObj);
bool  LTScene_GetGroundContact(void* hObj, float* pNormal3, float* pPoint3, unsigned int* pPoly);

// Swept move against the shared world collision; writes back where it ENDED.
// Returns true when the object finished somewhere other than it started.
bool  LTScene_MoveWithCollision(void* hObj, const float* pWant3, bool* pbGround);
// Carries the object by the motion of a moving brush it stands on (lifts).
bool  LTScene_CarryOnMovers(void* hObj);
void  LTScene_TeleportTo(void* hObj, const float* pPos3);
// ⚠️ The handle that means "the level itself". GetStandingOn must report this
// when an object rests on world geometry -- see the implementation.
void* LTScene_WorldObject();
bool  LTScene_IsWorldObject(void* hObj);

void  LTScene_DumpPhysCensus();

// ⚠️ Model objects by state -- named, loaded, visible. Separates the four
// different bugs that all present as "the characters are missing".
void  LTScene_DumpModelCensus();

// ⚠️ The live, game-spawned models RenderCamera must add to the world view --
// every character. See ILTWorldDynamicObjects. Installed by the host.
class ILTWorldDynamicObjects;
void  LTScene_SetDynamicSource(const ILTWorldDynamicObjects* p);

// ⚠️ Where the game has moved the world's own brushes -- doors above all.
class ILTWorldModelXform;
void  LTScene_SetModelXform(const ILTWorldModelXform* p);

// See CLTScene::SetObjectFilenames. False for a handle that is not ours.
bool  LTScene_SetObjectFilenames(void* hObj, const char* pFilename,
                                 const std::vector<std::string>& skins);

// ⚠️ THE GAME'S 3D VIEW ON THE GPU. When set, ILTClient::RenderCamera draws
// through this instead of the software rasteriser, into the same RGBA surface
// (w*h*4), and returns false to fall back. Installed by the host; null = software.
class CLTWorldScene;
class ILTWorldDynamicObjects;
typedef bool (*LTSceneGpuCameraFn)(CLTWorldScene& scene, unsigned char* pRGBA,
                                   int nWidth, int nHeight,
                                   const float* pEye3, const float* pAt3, float fFovY,
                                   const ILTWorldModelXform* pXform,
                                   const ILTWorldDynamicObjects* pDyn,
                                   unsigned int* pnDynamicDrawn);
void  LTScene_SetGpuCamera(LTSceneGpuCameraFn fn);
bool  LTScene_GpuCameraActive();

// ⚠️ THE GAME'S 3D VIEW THROUGH THE COMPOSITOR (ltcomp.h, HANDOVER 5bq): into
// the native-size 3D target at pRect4 (x, y, w, h output pixels), never read
// back. When installed and the compositor is active, RenderCamera uses this
// and leaves the screen surface's 2D layers "open" over the camera's area.
typedef bool (*LTSceneGpuSceneFn)(CLTWorldScene& scene, int nOutW, int nOutH,
                                  const int* pRect4, bool bClear,
                                  const float* pEye3, const float* pAt3, float fFovY,
                                  const ILTWorldModelXform* pXform,
                                  const ILTWorldDynamicObjects* pDyn,
                                  const float* pScale3, const float* pAdd3,
                                  unsigned int* pnDynamicDrawn);
void  LTScene_SetGpuScene(LTSceneGpuSceneFn fn);

// ⚠️ THE MENU'S 3D THROUGH THE COMPOSITOR (HANDOVER 5ce): RenderObjects'
// items drawn into the native-size 3D target at pRect4 (output pixels; the
// camera's place in the zoomed menu box). When installed and the compositor
// is active, RenderObjects uses this and opens the 2D layers over the
// camera's area, as RenderCamera does. LT_MENUGPU=0 = the software path.
typedef bool (*LTSceneGpuObjectsFn)(int nOutW, int nOutH, const int* pRect4, bool bClear,
                                    const LTSceneGpuItem* pItems, int nItems,
                                    const LTModelPose* pPoses);
void  LTScene_SetGpuObjects(LTSceneGpuObjectsFn fn);

// Position and rotation of the camera RenderCamera last drew. False before any.
bool  LTScene_GetLastCamera(float* pPos3, float* pQuat4);

// Cumulative wall time and call count of RenderCamera, for the frame census.
double       LTScene_RenderCameraMs();
unsigned int LTScene_RenderCameraCalls();
int             LTScene_SetObjectRotation(void* hObj, const float* pQuat4);

// ⚠️ THE LOCAL PLAYER'S CLIENT-SIDE OBJECT, made by the ENGINE rather than by
// the game. In the original the server tells the client which object is its
// own and the engine mints it; this port has no such protocol yet, so the host
// creates exactly one and mirrors the server player's transform onto it.
//
// ⚠️ It has NO MODEL, on purpose. NOLF is first person: the local player's own
// body is not drawn, and giving it one would put Cate's model inside the
// camera. The object exists to be a POSITION the game can ask about --
// InterfaceMgr will not leave its loading screen without one, and the camera
// follows it.
void*           LTScene_CreatePlayerObject();

// ⚠️ A CLIENT OBJECT MADE FROM PLAIN DATA, for server->client replication.
// SC_CreateObject takes an ObjectCreateStruct because that is what the GAME
// hands the engine; the replicator has a server record instead, and building a
// fake create struct to satisfy a signature would be ceremony. Returns null
// for a type the scene cannot represent, exactly as CreateObject does.
void*           LTScene_CreateMirror(unsigned int nType,
                                     const float* pPos3, const float* pQuat4,
                                     unsigned int nFlags, const char* pFilename);
unsigned int    LTScene_GetObjectFlags(void* hObj);
unsigned int    LTScene_GetObjectUserFlags(void* hObj);
void            LTScene_SetObjectUserFlags(void* hObj, unsigned int nFlags);
void            LTScene_SetObjectFlags(void* hObj, unsigned int nFlags);
// FLAG2_* (ILTCommon's OFT_Flags2): blend mode, chromakey, portal-invisible.
unsigned int    LTScene_GetObjectFlags2(void* hObj);
bool            LTScene_SFXDelivered(void* hObj);
void            LTScene_SetSFXDelivered(void* hObj);
void            LTScene_ResetSFXDelivered();     // every object
void            LTScene_SetObjectFlags2(void* hObj, unsigned int nFlags2);
int             LTScene_GetObjectType(void* hObj, unsigned int* pType);
int             LTScene_GetAnimIndex(void* hObj, const char* pName);
void            LTScene_SetAnim(void* hObj, int nAnim);

// ⚠️ For the SERVER, which knows a model only by filename. -1 when unknown.
// ⚠️ NODE CONTROL (ILTClient::ModelNodeControl), keyed by the CLIENT handle
// the game registered it on. A null fn removes it. The callback is stored as
// the SDK's NodeControlFn signature, spelled out so this header does not need
// lithtech.h: (HOBJECT, HMODELNODE, LTMatrix*, void*). See ltscene.cpp.
typedef void (*LTSceneNodeFn)(void* hObj, unsigned int nNode, void* pGlobalMat, void* pUser);
// The shared model cache by filename -- the server places attachments on
// sockets of the SAME model the renderer poses. Null if it cannot load.
class CLTABCModel;
const CLTABCModel* LTScene_ModelForFile(const char* pFilename);

void            LTScene_SetNodeControl(void* hObj, LTSceneNodeFn fn, void* pUser);
bool            LTScene_GetNodeControl(void* hObj, LTSceneNodeFn* pFn, void** ppUser);

int             LTScene_GetAnimIndexForFile(const char* pFilename,
                                            const char* pAnimName);

// ⚠️ The dims an ANIMATION declares -- what an object's collision box should
// be. Writes zeros and returns false when unknown; never leaves the output
// alone, because the game reads it whatever the return says.
bool            LTScene_GetAnimDimsForFile(const char* pFilename, int nAnim, float* pOut3);

// Milliseconds (last key time), or -1 when the model/animation is unknown.
int             LTScene_GetAnimLengthForFile(const char* pFilename, int nAnim);
// The animation's BLEND-IN time in ms (ModelAnim::m_InterpolationMS): how long
// the original engine cross-fades into it before its own clock starts. 0 when
// unknown -- an unknown animation must not stall anything.
int             LTScene_GetAnimInterpForFile(const char* pFilename, int nAnim);
// Weight-set index by name, or -1.
int             LTScene_GetWeightSetForFile(const char* pFilename, const char* pSetName);

// ⚠️ Set an object's model AFTER creation. Replication calls this when the
// server names a model the client did not have at mirror time.
bool            LTScene_SetObjectModel(void* hObj, const char* pFilename);
void            LTScene_SetLooping(void* hObj, bool bLoop);
// ---- the READ half of the animation API -------------------------------- //
//
// ⚠️ The weapon is a state machine made of these. See the .cpp: setting an
// animation without being able to read one back is why nothing could fire.
int             LTScene_GetAnim(void* hObj);              // -1 = none
bool            LTScene_GetLooping(void* hObj);
void            LTScene_RestartAnim(void* hObj);          // play it again from the top
bool            LTScene_GetAnimProgress(void* hObj, float* pElapsedMs, int* pLengthMs);
bool            LTScene_IsAnimDone(void* hObj);           // false when looping or unknown

// ---- client flags and model keys --------------------------------------- //
//
// ⚠️ CF_NOTIFYMODELKEYS is how an object asks to be told when its animation
// crosses a key. Nothing could fire while these were stubs -- see the .cpp.
unsigned int    LTScene_GetClientFlags(void* hObj);
void            LTScene_SetClientFlags(void* hObj, unsigned int nFlags);
unsigned        LTScene_TakeModelKeys(LTModelKey* pOut, unsigned nMax);

int             LTScene_FindSocket(void* hObj, const char* pName);
int             LTScene_GetSocketTransform(void* hObj, int nSocket, int bWorldSpace,
                                           float* pPos3, float* pQuat4);

#endif // __LT_SCENE_H__
