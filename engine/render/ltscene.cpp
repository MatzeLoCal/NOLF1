// ----------------------------------------------------------------------- //
//
// MODULE  : ltscene.cpp
//
// PURPOSE : Object list, camera, and RenderObjects. See ltscene.h.
//
// ----------------------------------------------------------------------- //

// ⚠️ Include order mirrors the game's own Stdafx.h -- HANDOVER.md §4.3.
#include "windows.h"
#include "lithtech.h"

#include "ltcomp.h"
#include "ltscene.h"
#include "ltspr.h"
#include "ltworldscene.h"
#include "ltsession.h"
#include "ltabc.h"
#include "ltdtx.h"
#include "ltmodeldraw.h"

// For LTWorldMirror() -- ONE decision about whether the scene is mirrored,
// shared by the world renderer and the menu. See ltworldrender.h.
#include "ltworldrender.h"
#include "ltworldcollide.h"
#include "ltserverphysics.h"
#include "ltserverbrush.h"
#include "ltclock.h"
#include "ltfilemgr.h"
#include "ltnullrender.h"
#include "ltstub.h"
#include "ltreplicate.h"

#include <set>

#include "iltclient.h"

#include <cmath>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ⚠️ Opt-IN, not opt-out: the animated path is known wrong for long
// animations (see CreateObject). Off by default so the menu looks right.
static bool AnimEnabled()
{
    static int s = -1;
    if (s < 0) s = getenv("LT_ANIM") ? 1 : 0;
    return s == 1;
}

// ⚠️ Duplicated in spirit from ltworldrender.cpp, and the duplication is the
// problem rather than the solution -- see the note at its use site. Kept
// identical: BOTH sides must be present at the SAME threshold the rasteriser
// will use, or the guard passes exactly what it exists to stop.
static bool DiscriminatesAt(const std::vector<unsigned char>& rgba, float fRef)
{
    if (rgba.size() < 4) return false;
    const unsigned char nRef = (unsigned char)(fRef * 255.0f + 0.5f);
    bool bBelow = false, bAbove = false;
    for (size_t i = 3; i < rgba.size(); i += 4)
    {
        if (rgba[i] < nRef) bBelow = true; else bAbove = true;
        if (bBelow && bAbove) return true;
    }
    return false;
}

static bool TraceOn()
{
    static int s = -1;
    if (s < 0) s = getenv("LT_TRACE_3D") ? 1 : 0;
    return s == 1;
}

// Game-spawned models drawn by RenderCamera. See LTScene_DumpModelCensus.
static unsigned int g_nDynamicDrawn = 0;

// Wall time spent inside RenderCamera -- the whole in-game 3D view, software.
static double       g_fRenderCameraMs    = 0.0;
static unsigned int g_nRenderCameraCalls = 0;
double       LTScene_RenderCameraMs()    { return g_fRenderCameraMs; }
unsigned int LTScene_RenderCameraCalls() { return g_nRenderCameraCalls; }

CLTScene::CLTScene()
    : m_pFileMgr(0), m_nRenderCalls(0), m_nModelsDrawn(0),
      m_nMissingModels(0), m_nMissingSkins(0)
{
    m_LightScale[0] = m_LightScale[1] = m_LightScale[2] = 1.0f;
}

CLTScene::~CLTScene()
{
    Term();
}

void CLTScene::Init(CLTFileMgr* pFileMgr)
{
    m_pFileMgr = pFileMgr;
}

void CLTScene::Term()
{
    for (size_t i = 0; i < m_Objects.size(); ++i) delete m_Objects[i];
    m_Objects.clear();

    // ⚠️ BOTH CACHES STORE A NULL ON FAILURE, ON PURPOSE -- it is what stops
    // a missing model or skin being re-loaded and re-reported once per frame
    // for the rest of the run (LoadModel:112, LoadSkin:139/151). The
    // consequence is that a null entry is ORDINARY here, not a corruption, and
    // teardown has to expect it.
    //
    // ⚠️ It did not, and `delete i->second->m_pDtx` on a null entry is a null
    // dereference during exit(). It hid for as long as every skin the menu
    // asked for existed; running a real LEVEL asks for hundreds more, and 13
    // of the 102 worlds reference at least one skin that does not resolve.
    // The symptom was a segfault AFTER the census had printed, which reads
    // like a teardown-order problem and is not one.
    for (std::map<std::string, CLTABCModel*>::iterator i = m_Models.begin();
         i != m_Models.end(); ++i) delete i->second;
    m_Models.clear();

    for (std::map<std::string, SkinEntry*>::iterator i = m_Skins.begin();
         i != m_Skins.end(); ++i)
    {
        if (!i->second) continue;               // a load that failed
        delete i->second->m_pDtx;
        delete i->second;
    }
    m_Skins.clear();

    m_Depth.clear();
}

void CLTScene::SetGlobalLightScale(const float* p)
{
    if (!p) return;
    for (int i = 0; i < 3; ++i) m_LightScale[i] = p[i];
}


// ----------------------------------------------------------------------- //
// caches
// ----------------------------------------------------------------------- //

const CLTABCModel* CLTScene::LoadModel(const std::string& sName)
{
    if (sName.empty()) return 0;

    std::map<std::string, CLTABCModel*>::iterator it = m_Models.find(sName);
    if (it != m_Models.end()) return it->second;

    CLTABCModel* pModel = new CLTABCModel;
    if (!pModel->LoadThroughFileMgr(m_pFileMgr, sName.c_str()))
    {
        // ⚠️ Reported, not swallowed. A model the menu wanted and did not get
        // is the difference between "the background is missing" and "the
        // background is wrong", and only one of those is a renderer bug.
        fprintf(stderr, "[LT3D] model FAILED  %s -- %s\n",
                sName.c_str(), pModel->GetError());
        delete pModel;
        ++m_nMissingModels;
        // A null entry is cached so a model that cannot load is not retried
        // every single frame.
        m_Models[sName] = 0;
        return 0;
    }

    // ⚠️ THE CHILD MODELS' ANIMATIONS, appended exactly as the world
    // renderer's cache does -- the SERVER resolves animation names through
    // this cache and the RENDERER draws through that one, so both must build
    // the same table or index 7 means two different animations.
    pModel->MergeChildAnimations(m_pFileMgr, sName.c_str());

    if (TraceOn())
        fprintf(stderr, "[LT3D] model  %s  v%u  %u pieces  %u tris\n",
                sName.c_str(), pModel->GetVersion(),
                (unsigned)pModel->GetPieces().size(), pModel->GetNumTris());

    m_Models[sName] = pModel;
    return pModel;
}

const LT3DTexture* CLTScene::SpriteTexture(const std::string& sSpr, float fSeconds,
                                           float fAlpha, int nBlendOp)
{
    // A .spr names DTX frames; a plain .dtx is its own single frame.
    std::string sDtx = sSpr;
    {
        static std::map<std::string, CLTSprite*> s_Spr;
        std::string lower = sSpr;
        for (size_t i = 0; i < lower.size(); ++i) lower[i] = (char)tolower((unsigned char)lower[i]);
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".dtx") != 0)
        {
            std::map<std::string, CLTSprite*>::iterator it = s_Spr.find(sSpr);
            CLTSprite* pS = (it != s_Spr.end()) ? it->second : 0;
            if (it == s_Spr.end())
            {
                pS = new CLTSprite;
                if (!pS->LoadThroughFileMgr(m_pFileMgr, sSpr.c_str()) || pS->GetNumFrames() == 0)
                {
                    fprintf(stderr, "[LT3D] sprite FAILED %s\n", sSpr.c_str());
                    delete pS; pS = 0;
                }
                s_Spr[sSpr] = pS;
            }
            if (!pS) return 0;
            sDtx = pS->GetFrame(pS->FrameAt(fSeconds));
        }
    }

    char aKey[64];
    snprintf(aKey, sizeof aKey, "|a%.4f|b%d", fAlpha, nBlendOp);
    const std::string sKey = sDtx + aKey;
    std::map<std::string, LT3DTexture*>::iterator it = m_SpriteTex.find(sKey);
    if (it != m_SpriteTex.end()) return it->second;

    const SkinEntry* pBase = LoadSkin(sDtx);
    LT3DTexture* pT = 0;
    if (pBase)
    {
        // Same rules as the game view (ltworldrender.cpp GetSpriteTexture):
        // additive ignores the channel; otherwise the channel is coverage if
        // it varies, else a constant folded into the object's alpha.
        pT = new LT3DTexture(pBase->m_Tex);
        pT->m_bAlphaTest  = false;
        pT->m_bBlend      = true;
        pT->m_fConstAlpha = fAlpha;
        pT->m_nBlendOp    = nBlendOp;
        pT->m_bTexAlpha   = false;
        if (nBlendOp != 1 && pT->m_pRGBA && pT->m_Width > 0 && pT->m_Height > 0)
        {
            const size_t nBytes = (size_t)pT->m_Width * pT->m_Height * 4;
            const unsigned char n0 = pT->m_pRGBA[3];
            bool bVaries = false;
            for (size_t q = 7; q < nBytes; q += 4)
                if (pT->m_pRGBA[q] != n0) { bVaries = true; break; }
            if (bVaries) pT->m_bTexAlpha = true;
            else         pT->m_fConstAlpha = fAlpha * (n0 / 255.0f);
        }
    }
    m_SpriteTex[sKey] = pT;
    return pT;
}

const CLTScene::SkinEntry* CLTScene::LoadSkin(const std::string& sName)
{
    if (sName.empty()) return 0;

    std::map<std::string, SkinEntry*>::iterator it = m_Skins.find(sName);
    if (it != m_Skins.end()) return it->second;

    CLTDtx* pDtx = new CLTDtx;
    if (!pDtx->LoadThroughFileMgr(m_pFileMgr, sName.c_str()))
    {
        fprintf(stderr, "[LT3D] skin  FAILED  %s -- %s\n",
                sName.c_str(), pDtx->GetError());
        delete pDtx;
        ++m_nMissingSkins;
        m_Skins[sName] = 0;
        return 0;
    }

    SkinEntry* pEntry = new SkinEntry;
    pEntry->m_pDtx = pDtx;
    if (!pDtx->DecodeMip(0, pEntry->m_RGBA) || pEntry->m_RGBA.empty())
    {
        fprintf(stderr, "[LT3D] skin  DECODE FAILED  %s\n", sName.c_str());
        delete pDtx;
        delete pEntry;
        ++m_nMissingSkins;
        m_Skins[sName] = 0;
        return 0;
    }

    pEntry->m_Tex.m_pRGBA = &pEntry->m_RGBA[0];
    pEntry->m_Tex.m_Width = (int)pDtx->GetMip(0)->m_nWidth;
    pEntry->m_Tex.m_Height = (int)pDtx->GetMip(0)->m_nHeight;

    // ⚠️ The threshold the TEXTURE states, not a constant. See
    // LT3DTexture::m_fAlphaRef -- 231 shipped textures carry an `alpharef` and
    // every one is below the 128 this used to assume. Set FIRST, because the
    // guard below is tested at it.
    {
        const int nRef = pDtx->GetAlphaRef();
        pEntry->m_Tex.m_fAlphaRef = (nRef >= 0) ? (nRef / 255.0f) : 0.5f;
    }

    // ⚠️ THE SAME GUARD THE WORLD PATH USES, and leaving it out of this one
    // cost the menu its flower.
    //
    // `AlphaIsMeaningful()` says the texture DECLARES alpha. It does not say
    // the alpha discriminates -- and punching a texture whose every texel is
    // below the threshold erases the thing entirely. The world path gained
    // `DiscriminatesAt` for exactly that (the invisible rocks); this path did
    // not, so the menu's sixties flower and one gradient panel simply
    // disappeared, at 9.298 mean absolute difference against the reference
    // frame.
    //
    // ⚠️ THE TWO PATHS MUST AGREE. This is the fourth time in this port that a
    // decision was changed in the world renderer and not in the menu renderer
    // (or the reverse) -- the scene mirror, the model mirror, the object-model
    // resolution, and now this. The frame contract is the only reason it was
    // caught.
    pEntry->m_Tex.m_bAlphaTest =
        pDtx->AlphaIsMeaningful()
        && DiscriminatesAt(pEntry->m_RGBA, pEntry->m_Tex.m_fAlphaRef);

    // ⚠️ The menu path has no world surfaces and so no SURF_TRANSPARENT, but
    // LT3DTexture is a plain struct -- an unset bool here is whatever the heap
    // held, and a future reader would get a coin flip rather than "no".
    pEntry->m_Tex.m_bFullyTransparent = false;

    if (TraceOn())
        fprintf(stderr, "[LT3D] skin   %s  %dx%d %s alpha=%s\n", sName.c_str(),
                pEntry->m_Tex.m_Width, pEntry->m_Tex.m_Height,
                pDtx->GetFormatName(), pEntry->m_Tex.m_bAlphaTest ? "tested" : "ignored");

    m_Skins[sName] = pEntry;
    return pEntry;
}


// ----------------------------------------------------------------------- //
// objects
// ----------------------------------------------------------------------- //

LTSceneObject* CLTScene::CreateObject(const LTSceneCreateInfo& info)
{
    // ⚠️ Only the types this scene can actually represent. Everything else
    // returns 0 so the game takes its own failure path rather than holding a
    // handle that never draws -- HANDOVER.md §2's "category 3" rule for the
    // null renderer, applied to objects.
    //
    // ⚠️ OT_NORMAL IS ON THIS LIST, and it belongs there: ltbasedefs.h:1207
    // calls it "Invisible object", which is a POSITION the game can ask about
    // and not a thing that draws. The scene represents that perfectly well --
    // the draw loop below already skips anything that is not a visible
    // OT_MODEL with a model loaded.
    //
    // ⚠️ Excluding it cost a whole evening. The local player's own object is
    // OT_NORMAL (NOLF is first person; drawing Cate's body would put it inside
    // the camera), CreateObject returned 0 for it, GetClientObject() therefore
    // answered null, and CInterfaceMgr::UpdateLoadingLevelState refused to
    // leave the loading screen -- with the world built, the server running and
    // the player spawned behind it. "Cannot represent" and "does not draw" are
    // not the same claim.
    // ⚠️ OT_SPRITE TOO (2026-10-04, HANDOVER 5bb): muzzle flashes, impacts and
    // smoke are client sprites, and refusing them meant none could exist --
    // 49 refusals in one M01S01 run. They draw in the GAME view through
    // CLTSceneViewObjects; the menu's RenderObjects still draws models only.
    // ⚠️ OT_CONTAINER TOO (2026-10-11, HANDOVER 5bn), as an invisible handle:
    // a volume's own effect -- SFX_VOLUMEBRUSH_ID, the water's tint, fog and
    // sound filter -- attaches to the client copy of the container, and with
    // no copy the effect was dropped, so CGameClientShell::UpdateContainerFX
    // never found the CVolumeBrushFX it reads the underwater look from. The
    // draw loop already skips anything that is not a model with a model.
    // ⚠️ OT_POLYGRID TOO (2026-10-14): the water surfaces. Its heightmap is
    // made by SetupPolyGrid, and it draws in the game view's translucent pass.
    if (info.m_nType != OT_MODEL && info.m_nType != OT_CAMERA &&
        info.m_nType != OT_NORMAL && info.m_nType != OT_SPRITE &&
        info.m_nType != OT_CONTAINER &&
        !(info.m_nType == OT_POLYGRID && LTPolyGridEnabled()))
        return 0;

    LTSceneObject* p = new LTSceneObject;
    p->m_nType = info.m_nType;
    for (int i = 0; i < 3; ++i) { p->m_Pos[i] = info.m_Pos[i]; p->m_Scale[i] = info.m_Scale[i]; }
    for (int i = 0; i < 4; ++i) p->m_Quat[i] = info.m_Quat[i];
    p->m_Flags = info.m_Flags;
    p->m_Flags2 = info.m_Flags2;
    p->m_UserFlags = 0;
    p->m_Color[0] = p->m_Color[1] = p->m_Color[2] = p->m_Color[3] = 1.0f;
    p->m_sFilename = info.m_sFilename;
    p->m_fFovX = p->m_fFovY = 1.5707963f;

    // ⚠️ Zeroed, for the same reason the server record's are: the game reads
    // velocity back and branches on it, and an object that was never given one
    // must answer exactly zero.
    for (int k = 0; k < 3; ++k)
    { p->m_Vel[k] = 0.0f; p->m_Accel[k] = 0.0f; p->m_Dims[k] = 0.0f; }
    p->m_bOnGround = false;
    p->m_bFullScreen = true;
    p->m_Rect[0] = p->m_Rect[1] = p->m_Rect[2] = p->m_Rect[3] = 0;
    p->m_pModel = 0;
    p->m_nAnim = -1;      // set to 0 below once a model is known
    p->m_bLooping = true;
    p->m_fAnimStart = 0.0f;
    p->m_ClientFlags = 0;
    p->m_fKeysUpToMs = -1.0f;
    p->m_bInGameView = false;

    if (info.m_nType == OT_MODEL)
    {
        p->m_pModel = LoadModel(info.m_sFilename);

        // ⚠️ ANIMATION PLAYBACK IS OFF BY DEFAULT, AND THIS IS A KNOWN GAP.
        //
        //   The skinning FORMULA is verified: for every single-weight vertex
        //   of CATE.ABC, `bindWorld[declaredNode] * offset == pos` to
        //   **0.0000**, so `m_Offset`, the node indices and the node matrices
        //   are all read correctly.
        //
        //   What is NOT yet right is the per-keyframe transform for
        //   multi-keyframe animations. The physical check is bone length --
        //   a bone cannot stretch between poses:
        //
        //       CATE.ABC            (13 keyframes)   max error    0.18%
        //       INTHERO_ACTION.ABC (151 keyframes)   max error 2382.27%
        //
        //   INTHERO_ACTION is the menu character, so playing it produced a
        //   visibly mis-proportioned figure -- short arms, hunched shoulders.
        //   ⚠️ It still looked like a plausible person, which is why this
        //   needed an outside eye rather than a crash.
        //
        //   Ruled out already: transforms are LOCAL, not absolute (CATE 0.18%
        //   vs 625%), and storage is NODE-MAJOR, not keyframe-major (0.18% vs
        //   480%). Both were tested by bone length, and note the size check
        //   CANNOT distinguish those orderings -- nNodes x nKeyframes x 28
        //   bytes either way.
        //
        //   Until the remaining difference is understood, the bind pose is
        //   drawn: it is exact, and it is what the model looks like when
        //   posed by hand. Set **LT_ANIM=1** to play animations anyway and
        //   continue the investigation.
        // ⚠️ THE COMMENT ABOVE IS KEPT AS A RECORD AND ITS CONCLUSION WAS
        // WRONG. "Short animations read correctly, long ones stretch bones by
        // 2382%" was measured with a BROKEN INSTRUMENT.
        //
        // `AnimationIsPlausible` derived each node's parent from the
        // depth-first child counts and read the parent BEFORE popping
        // completed subtrees, so the first node after any finished branch was
        // parented to the last node of that branch. On ARCH_BASE.ABC the right
        // shoulder was parented to the LEFT FOREARM. That is not a skeleton,
        // and of course its bones "stretched".
        //
        // ⚠️ A SWEEP OF ALL 695 MODELS / 2800 ANIMATIONS SETTLED IT:
        //   * failures start at TWO keyframes and a 232-keyframe animation
        //     passes, so keyframe count was never the variable;
        //   * node-major/pos-first is the best of all four byte layouts
        //     (keyframe-major 88, quat-first 88, current 186 at the time);
        //   * the ABC chunk end is enforced exactly, so the bytes were never
        //     in doubt -- only what they were composed into.
        // With the walk fixed, every limb of ARCH_BASE reads 0.0000 stretch.
        //
        // What remains is not a decode error either: the residual offenders
        // are CONTROL nodes that sit on top of their parent and carry root
        // motion, so the check now ignores bones shorter than 4% of the
        // model. See LTModelDraw::AnimationIsPlausible.
        //
        // LT_ANIM=1 still forces animation on regardless.
        if (p->m_pModel && !p->m_pModel->GetAnims().empty())
        {
            if (AnimEnabled() || LTModelDraw::AnimationIsPlausible(*p->m_pModel, 0))
                p->m_nAnim = 0;
            else if (TraceOn())
                fprintf(stderr, "[LT3D] anim REFUSED (bones stretch): %s\n",
                        info.m_sFilename.c_str());
        }
        p->m_SkinNames = info.m_Skins;
        for (size_t i = 0; i < info.m_Skins.size(); ++i)
        {
            const SkinEntry* pSkin = LoadSkin(info.m_Skins[i]);
            LT3DTexture t;
            if (pSkin) t = pSkin->m_Tex;
            else { t.m_pRGBA = 0; t.m_Width = t.m_Height = 0; t.m_bAlphaTest = false;
                   t.m_fAlphaRef = 0.5f; t.m_bFullyTransparent = false; }
            p->m_Skins.push_back(t);
        }
    }

    m_Objects.push_back(p);
    return p;
}

bool CLTScene::SetObjectFilenames(LTSceneObject* p, const char* pFilename,
                                  const std::vector<std::string>& skins)
{
    if (!IsValid(p)) return false;
    if (pFilename && pFilename[0] && p->m_nType == OT_MODEL)
    {
        p->m_sFilename = pFilename;
        p->m_pModel = LoadModel(p->m_sFilename);
        // The game sets the animation right after (WeaponModel.cpp:1283);
        // start from the first one so the object is never left pointing past
        // the end of a model with fewer animations.
        p->m_nAnim = (p->m_pModel && !p->m_pModel->GetAnims().empty()) ? 0 : -1;
        p->m_fKeysUpToMs = -1.0f;
    }
    if (!skins.empty())
    {
        p->m_SkinNames = skins;
        p->m_Skins.clear();
        for (size_t i = 0; i < skins.size(); ++i)
        {
            const SkinEntry* pSkin = LoadSkin(skins[i]);
            LT3DTexture t;
            if (pSkin) t = pSkin->m_Tex;
            else { t.m_pRGBA = 0; t.m_Width = t.m_Height = 0; t.m_bAlphaTest = false;
                   t.m_fAlphaRef = 0.5f; t.m_bFullyTransparent = false; }
            p->m_Skins.push_back(t);
        }
    }
    if (getenv("LT_OBJTRACE"))
    {
        printf("[OBJ] client SETFILENAMES '%s' (%u skin(s))\n",
               p->m_sFilename.c_str(), (unsigned)p->m_SkinNames.size());
        fflush(stdout);
    }
    return true;
}

bool CLTScene::RemoveObject(LTSceneObject* pObj)
{
    // Paired with the create trace: an object that appears and vanishes is a
    // different problem from one that was never made.
    if (pObj && getenv("LT_OBJTRACE"))
    {
        printf("[OBJ] client REMOVE type %u '%s'\n", pObj->m_nType,
               pObj->m_sFilename.empty() ? "(none)" : pObj->m_sFilename.c_str());
        fflush(stdout);
    }

    for (size_t i = 0; i < m_Objects.size(); ++i)
    {
        if (m_Objects[i] == pObj)
        {
            delete pObj;
            m_Objects.erase(m_Objects.begin() + i);
            return true;
        }
    }
    return false;
}

bool CLTScene::IsValid(const LTSceneObject* pObj) const
{
    if (!pObj) return false;
    for (size_t i = 0; i < m_Objects.size(); ++i)
        if (m_Objects[i] == pObj) return true;
    return false;
}


// ----------------------------------------------------------------------- //
// animation
// ----------------------------------------------------------------------- //

namespace { unsigned int KeyframeAt(const LTABCAnim& a, float fElapsed, bool bLoop); }

// ⚠️ GIVE AN EXISTING OBJECT A MODEL -- because a model can arrive LATE.
//
// `CAI::InitialUpdate` runs after MID_PRECREATE and only then asks the bute
// manager which character this is, so an AI is replicated to the client BEFORE
// it has a model. Replication used to take the filename once, at creation, and
// never look again: the client held thirteen positioned, moving, correctly
// mirrored characters with nothing to draw, which reads from the outside as
// "there are no NPCs at all".
//
// Returns false when the name is empty, unchanged, or names no loadable model.
bool CLTScene::SetObjectModel(LTSceneObject* pObj, const char* pFilename)
{
    if (!pObj || !pFilename || !*pFilename) return false;
    if (pObj->m_sFilename == pFilename) return false;

    const CLTABCModel* pModel = LoadModel(pFilename);
    if (!pModel) return false;

    pObj->m_sFilename = pFilename;
    pObj->m_pModel    = pModel;

    // ⚠️ THE ANIMATION IS RESET, NOT KEPT. The index belonged to the previous
    // model and means something different in this one -- or nothing at all.
    // Carrying it over would pose the new character with the old one's frames.
    pObj->m_nAnim      = -1;
    pObj->m_fAnimStart = 0.0f;
    pObj->m_fKeysUpToMs = -1.0f;

    if (TraceOn())
        fprintf(stderr, "[LT3D] model set late: %s (%u anim(s))\n",
                pFilename, (unsigned)pModel->GetAnims().size());
    return true;
}


bool CLTScene::AnimDimsForFile(const char* pFilename, int nAnim, float* pOut3)
{
    if (!pOut3) return false;
    pOut3[0] = pOut3[1] = pOut3[2] = 0.0f;
    if (!pFilename || !*pFilename) return false;

    const CLTABCModel* pModel = LoadModel(pFilename);
    if (!pModel || pModel->GetAnims().empty()) return false;

    // ⚠️ An unknown animation falls back to the FIRST, which is the model's
    // own rest pose. Returning nothing would leave the caller with whatever
    // its stack held -- and that is exactly the bug this fixes.
    const size_t i = (nAnim >= 0 && (size_t)nAnim < pModel->GetAnims().size())
                   ? (size_t)nAnim : 0;
    const LTABCAnim& a = pModel->GetAnims()[i];
    for (int k = 0; k < 3; ++k) pOut3[k] = a.m_Dims[k];
    return true;
}


int CLTScene::AnimLengthForFile(const char* pFilename, int nAnim)
{
    if (!pFilename || !*pFilename || nAnim < 0) return -1;
    const CLTABCModel* pModel = LoadModel(pFilename);
    if (!pModel || (size_t)nAnim >= pModel->GetAnims().size()) return -1;
    const LTABCAnim& a = pModel->GetAnims()[(size_t)nAnim];
    return a.m_Keyframes.empty() ? 0 : (int)a.m_Keyframes.back().m_nTime;
}

int CLTScene::AnimInterpForFile(const char* pFilename, int nAnim)
{
    if (!pFilename || !*pFilename || nAnim < 0) return 0;
    const CLTABCModel* pModel = LoadModel(pFilename);
    if (!pModel || (size_t)nAnim >= pModel->GetAnims().size()) return 0;
    const LTABCAnim& a = pModel->GetAnims()[(size_t)nAnim];
    // Donor trk_ScanToKeyFrame: a one-key animation is not animated at all,
    // so it never interpolates either.
    if (a.m_Keyframes.size() <= 1) return 0;
    return (int)a.m_nInterpMS;
}

int CLTScene::WeightSetForFile(const char* pFilename, const char* pSetName)
{
    if (!pFilename || !*pFilename || !pSetName) return -1;
    const CLTABCModel* pModel = LoadModel(pFilename);
    if (!pModel) return -1;
    const std::vector<LTABCWeightSet>& w = pModel->GetWeightSets();
    for (size_t i = 0; i < w.size(); ++i)
        if (strcasecmp(w[i].m_sName.c_str(), pSetName) == 0) return (int)i;
    return -1;
}


int CLTScene::AnimIndexForFile(const char* pFilename, const char* pAnimName)
{
    if (!pFilename || !*pFilename || !pAnimName || !*pAnimName) return -1;

    const CLTABCModel* pModel = LoadModel(pFilename);
    if (!pModel) return -1;

    const std::vector<LTABCAnim>& a = pModel->GetAnims();
    for (size_t i = 0; i < a.size(); ++i)
        if (strcasecmp(a[i].m_sName.c_str(), pAnimName) == 0) return (int)i;
    return -1;
}


int CLTScene::GetAnimIndex(const LTSceneObject* pObj, const char* pName) const
{
    if (!pObj || !pObj->m_pModel || !pName) return -1;
    const std::vector<LTABCAnim>& a = pObj->m_pModel->GetAnims();
    if (TraceOn())
    {
        fprintf(stderr, "[LT3D] GetAnimIndex(%s) on %s -- model has:",
                pName, pObj->m_sFilename.c_str());
        for (size_t i = 0; i < a.size(); ++i)
            fprintf(stderr, " %s", a[i].m_sName.c_str());
        fprintf(stderr, "\n");
    }
    for (size_t i = 0; i < a.size(); ++i)
    {
        // ⚠️ Case-insensitive. The game looks animations up by names that come
        // from attribute files, and those do not match the exporter's casing.
        if (strcasecmp(a[i].m_sName.c_str(), pName) == 0) return (int)i;
    }
    return -1;
}

bool CLTScene::SetAnim(LTSceneObject* pObj, int nAnim, float fNow)
{
    if (!pObj) return false;

    // ⚠️ An invalid index LEAVES THE CURRENT ANIMATION ALONE rather than
    // clearing it. The game already guards this call, but a model that has
    // been asked for a nonexistent animation should keep playing, not freeze.
    if (nAnim < 0) return false;
    if (!pObj->m_pModel || (size_t)nAnim >= pObj->m_pModel->GetAnims().size())
        return false;

    if (pObj->m_nAnim != nAnim)
    {
        pObj->m_fAnimStart  = fNow;
        pObj->m_fKeysUpToMs = -1.0f;   // the new animation's keys are all ahead
    }
    pObj->m_nAnim = nAnim;
    return true;
}

bool CLTScene::SetLooping(LTSceneObject* pObj, bool bLoop)
{
    if (!pObj) return false;
    pObj->m_bLooping = bLoop;
    return true;
}

namespace
{
    // Which keyframe an animation is showing at `fElapsed` seconds.
    //
    // ⚠️ Keyframes carry their own times in MILLISECONDS and are NOT evenly
    // spaced -- an exporter drops frames where nothing moves. Dividing elapsed
    // time by a frame count would run the animation at the wrong speed and
    // stutter wherever the spacing changes, so the times are searched.
    //
    // ⚠️ Nearest keyframe, no interpolation between them. That is a real
    // limitation, not an oversight: interpolating requires slerping every
    // node's rotation, and doing it wrong looks like correct animation with
    // subtly broken joints. It is stated here so the next person knows it is
    // still to do.
    unsigned int KeyframeAt(const LTABCAnim& a, float fElapsed, bool bLoop)
    {
        unsigned int n = (unsigned int)a.m_Keyframes.size();
        if (n <= 1) return 0;

        unsigned int nLast = a.m_Keyframes[n - 1].m_nTime;
        if (nLast == 0) return 0;

        float fMs = fElapsed * 1000.0f;
        if (fMs < 0.0f) fMs = 0.0f;

        if (bLoop) fMs = (float)fmod(fMs, (float)nLast);
        else if (fMs >= (float)nLast) return n - 1;

        for (unsigned int i = n; i-- > 0; )
            if ((float)a.m_Keyframes[i].m_nTime <= fMs) return i;
        return 0;
    }
}

static void QuatFromOrthonormal(const float* m, float* q);

int CLTScene::FindSocket(const LTSceneObject* pObj, const char* pName) const
{
    if (!pObj || !pObj->m_pModel || !pName) return -1;
    const std::vector<LTABCSocket>& sockets = pObj->m_pModel->GetSockets();
    for (size_t i = 0; i < sockets.size(); ++i)
        if (strcasecmp(sockets[i].m_sName.c_str(), pName) == 0) return (int)i;
    return -1;
}

// Extracts a quaternion from a transform's ROTATION.
//
// ⚠️ THE BASIS MUST BE NORMALISED FIRST. The composed socket transform
// includes the parent object's scale (the interface character is drawn at
// 1.3), and every trace-based quaternion extraction assumes an ORTHONORMAL
// basis -- feed it a scaled matrix and it returns a rotation that is simply
// wrong. Symptom: Cate's sunglasses sat on her face at a visible tilt. The
// position was right, because that comes from the translation column and is
// unaffected, which is what makes this look like an art problem rather than a
// maths one.
static void QuatFromMatrix(const float* mIn, float* q)
{
    // Normalise the three basis COLUMNS (row-major storage, column vectors).
    float m[16];
    std::memcpy(m, mIn, sizeof(m));
    for (int c = 0; c < 3; ++c)
    {
        float len = sqrtf(m[0*4+c]*m[0*4+c] + m[1*4+c]*m[1*4+c] + m[2*4+c]*m[2*4+c]);
        if (len > 1e-8f)
            for (int r = 0; r < 3; ++r) m[r*4+c] /= len;
    }
    QuatFromOrthonormal(m, q);
}

static void QuatFromOrthonormal(const float* m, float* q)
{
    float tr = m[0] + m[5] + m[10];
    if (tr > 0.0f)
    {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q[3] = 0.25f * s;
        q[0] = (m[9] - m[6]) / s;
        q[1] = (m[2] - m[8]) / s;
        q[2] = (m[4] - m[1]) / s;
    }
    else if (m[0] > m[5] && m[0] > m[10])
    {
        float s = sqrtf(1.0f + m[0] - m[5] - m[10]) * 2.0f;
        q[3] = (m[9] - m[6]) / s; q[0] = 0.25f * s;
        q[1] = (m[1] + m[4]) / s; q[2] = (m[2] + m[8]) / s;
    }
    else if (m[5] > m[10])
    {
        float s = sqrtf(1.0f + m[5] - m[0] - m[10]) * 2.0f;
        q[3] = (m[2] - m[8]) / s; q[0] = (m[1] + m[4]) / s;
        q[1] = 0.25f * s;         q[2] = (m[6] + m[9]) / s;
    }
    else
    {
        float s = sqrtf(1.0f + m[10] - m[0] - m[5]) * 2.0f;
        q[3] = (m[4] - m[1]) / s; q[0] = (m[2] + m[8]) / s;
        q[1] = (m[6] + m[9]) / s; q[2] = 0.25f * s;
    }
}

bool CLTScene::GetSocketTransform(const LTSceneObject* pObj, int nSocket,
                                  bool bWorldSpace,
                                  float* pPos, float* pQuat) const
{
    if (!pObj || !pObj->m_pModel) return false;
    const std::vector<LTABCSocket>& sockets = pObj->m_pModel->GetSockets();
    if (nSocket < 0 || (size_t)nSocket >= sockets.size()) return false;

    const LTABCSocket& sk = sockets[nSocket];

    // The socket's own transform, relative to its node.
    float M[16];
    LT3D::FromQuatPos(sk.m_Rot, sk.m_Pos, M);

    // Times the node's transform for the pose being displayed THIS frame.
    LTModelPose pose;
    bool bHavePose = false;
    if (pObj->m_nAnim >= 0 && (size_t)pObj->m_nAnim < pObj->m_pModel->GetAnims().size())
    {
        const LTABCAnim& a = pObj->m_pModel->GetAnims()[pObj->m_nAnim];
        float fElapsed = LTGetClock()->GetTime() - pObj->m_fAnimStart;
        unsigned int kf = KeyframeAt(a, fElapsed, pObj->m_bLooping);
        bHavePose = LTModelDraw::BuildPose(*pObj->m_pModel, (unsigned)pObj->m_nAnim, kf, pose);
    }
    if (!bHavePose) bHavePose = LTModelDraw::BuildBindPose(*pObj->m_pModel, pose);

    if (bHavePose && sk.m_nNode < pose.Count())
    {
        float T[16];
        LT3D::Multiply(pose.Get(sk.m_nNode), M, T);
        std::memcpy(M, T, sizeof(M));
    }

    // ⚠️ MODEL SPACE IS MIRRORED IN X, SO THIS TRANSFORM MUST BE TOO.
    //
    // Everything above is expressed in the model's own space, and LTModelDraw
    // mirrors that space (see ltmodeldraw.cpp). Leaving the socket alone puts
    // the attachment where the UNmirrored head was: Cate's sunglasses came off
    // her face entirely and floated beside her head.
    //
    // ⚠️ It is a CONJUGATION, `Mx * M * Mx`, not just a negated position. That
    // negates the x translation AND the row-0/column-0 off-diagonal terms of
    // the rotation, which is what changing the handedness of the basis a
    // rotation is expressed in actually does. Negating only the position
    // leaves the glasses correctly placed and wrongly ANGLED, which reads as
    // an art bug rather than a maths one -- the same trap this file already
    // hit once by extracting a quaternion from a scaled basis.
    //
    // ⚠️ And the result is still a PROPER rotation: det(Mx*R*Mx) = det(Mx)^2 *
    // det(R) = det(R). That matters because the quaternion extraction below
    // cannot represent a reflection and would return silent garbage for one.
    //
    // ⚠️ BUT ONLY FOR A MODEL THE MENU DRAWS. The game camera draws its models
    // UNMIRRORED and shifted by the animation's origin offset
    // (ltworldrender.cpp PrepareDynamic: Place.t -= R*S*ofs), so a socket on
    // one of those must be read the same way. Mirroring it put the view
    // rifle's scope beside the rifle instead of on it (user report, 5bd).
    // LT_OLDSOCKETS=1 restores the menu rule for everything (says so).
    static const bool s_bOldSockets = []() {
        const bool b = (getenv("LT_OLDSOCKETS") != 0);
        if (b) { printf("[SOCKET] LT_OLDSOCKETS=1 -- game-view sockets read MIRRORED, no origin offset\n"); fflush(stdout); }
        return b; }();
    const bool bGame = pObj->m_bInGameView && !s_bOldSockets;
    const bool bMirror = bGame ? (getenv("LT_DYNMIRROR") != 0) : LTModelDraw::MirrorX();
    if (bMirror)
    {
        M[1] = -M[1];  M[2]  = -M[2];   M[3] = -M[3];   // row 0
        M[4] = -M[4];  M[8]  = -M[8];                   // column 0
    }
    if (getenv("LT_SOCKTRACE"))
    {
        static int s_n = 0;
        if ((s_n++ % 30) == 0)
        {
            float o2[3] = {0,0,0};
            pObj->m_pModel->GetOriginOffset(pObj->m_nAnim >= 0 ? (unsigned)pObj->m_nAnim : 0u, o2);
            printf("[SOCK] %s '%s' anim %d node %u local(%.2f %.2f %.2f) posed(%.2f %.2f %.2f) ofs(%.2f %.2f %.2f) game %d\n",
                   pObj->m_sFilename.c_str(), sk.m_sName.c_str(), pObj->m_nAnim, sk.m_nNode,
                   sk.m_Pos[0], sk.m_Pos[1], sk.m_Pos[2], M[3], M[7], M[11], o2[0], o2[1], o2[2], (int)bGame);
            fflush(stdout);
        }
    }
    if (bGame)
    {
        float ofs[3] = { 0.0f, 0.0f, 0.0f };
        pObj->m_pModel->GetOriginOffset(pObj->m_nAnim >= 0 ? (unsigned)pObj->m_nAnim : 0u, ofs);
        if (bMirror) ofs[0] = -ofs[0];
        M[3] -= ofs[0]; M[7] -= ofs[1]; M[11] -= ofs[2];
    }

    if (bWorldSpace)
    {
        // ⚠️ THE OBJECT'S SCALE BELONGS IN HERE. The interface character is
        // drawn at scale 1.3, so a socket composed without it lands at 1/1.3
        // of its proper offset -- Cate's sunglasses sat on her collarbone
        // instead of her eyes. The attachment MODEL is scaled separately by
        // the game (from the attachment's own fScale), which is why leaving
        // scale out here looks like a plausible near-miss rather than an
        // obvious error.
        float O[16], S[16], OS[16], T[16];
        LT3D::FromQuatPos(pObj->m_Quat, pObj->m_Pos, O);
        LT3D::Identity(S);
        S[0] = pObj->m_Scale[0]; S[5] = pObj->m_Scale[1]; S[10] = pObj->m_Scale[2];
        LT3D::Multiply(O, S, OS);
        LT3D::Multiply(OS, M, T);
        std::memcpy(M, T, sizeof(M));
    }

    if (pPos) { pPos[0] = M[3]; pPos[1] = M[7]; pPos[2] = M[11]; }
    if (pQuat) QuatFromMatrix(M, pQuat);
    return true;
}


// ----------------------------------------------------------------------- //
// RenderObjects
// ----------------------------------------------------------------------- //

int CLTScene::RenderObjects(LTSceneObject* pCamera,
                            LTSceneObject* const* ppObjects, int nObjects,
                            const LT2DSurf& dest,
                            std::vector<LTSceneGpuItem>* pCollect,
                            std::vector<LTModelPose>* pPoses)
{
    ++m_nRenderCalls;

    if (!pCamera || pCamera->m_nType != OT_CAMERA) return 0;
    if (!dest.m_pRGBA || dest.m_Width <= 0 || dest.m_Height <= 0) return 0;

    // The camera's viewport. A full-screen camera covers the surface; a
    // rect-limited one covers what SetCameraRect was given.
    int vx = 0, vy = 0, vw = dest.m_Width, vh = dest.m_Height;
    if (!pCamera->m_bFullScreen)
    {
        vx = pCamera->m_Rect[0];
        vy = pCamera->m_Rect[1];
        vw = pCamera->m_Rect[2] - pCamera->m_Rect[0];
        vh = pCamera->m_Rect[3] - pCamera->m_Rect[1];
    }
    if (vw <= 0 || vh <= 0) return 0;

    // ⚠️ The depth buffer is sized to the whole surface and CLEARED per call,
    // not per frame. RenderObjects is the game's unit of "draw this scene", and
    // the interface calls it once per frame with the whole list -- so clearing
    // here is right, and carrying depth across calls would make a second call
    // silently occlude against the first.
    m_Depth.assign((size_t)dest.m_Width * dest.m_Height, 0.0f);

    LT3DTarget target;
    target.m_pRGBA = dest.m_pRGBA;
    target.m_pDepth = &m_Depth[0];
    target.m_Width = dest.m_Width;
    target.m_Height = dest.m_Height;

    // View: the camera's own position and orientation.
    //
    // ⚠️ LTRotation's identity is "facing down POSITIVE Z" (ltrotation.h:42),
    // while the projection built by LT3D::Perspective looks down NEGATIVE Z
    // like every right-handed convention. So the camera's forward vector is
    // +Z rotated by the quaternion, and LookAt is aimed along it. Getting this
    // backwards points the camera away from the scene and renders nothing --
    // which is indistinguishable from a broken object list.
    float camM[16];
    LT3D::FromQuatPos(pCamera->m_Quat, pCamera->m_Pos, camM);

    float fwd[3] = { camM[2], camM[6], camM[10] };      // +Z column
    float up[3]  = { camM[1], camM[5], camM[9]  };      // +Y column
    float at[3]  = { pCamera->m_Pos[0] + fwd[0],
                     pCamera->m_Pos[1] + fwd[1],
                     pCamera->m_Pos[2] + fwd[2] };

    float V[16], P[16], VP[16];
    LT3D::LookAt(pCamera->m_Pos, at, up, V);

    // NOLF gives the camera a horizontal AND a vertical FOV; the projection
    // takes the vertical one plus an aspect ratio, so the aspect is derived
    // from the pair rather than from the viewport. That keeps the game's own
    // widescreen handling meaningful instead of overriding it.
    float fAspect = (pCamera->m_fFovY > 0.0f)
                  ? (float)tan(pCamera->m_fFovX * 0.5) / (float)tan(pCamera->m_fFovY * 0.5)
                  : (float)vw / (float)vh;
    LT3D::Perspective(pCamera->m_fFovY, fAspect, 1.0f, 10000.0f, P);

    // ⚠️ THE SAME SCENE MIRROR THE WORLD USES, from the same decision. NOLF's
    // data is left-handed against this renderer's basis, so the image is
    // flipped; the world renderer has done this since 2026-09-09 and the MENU
    // was left behind, which is why Cate stood on the wrong side of the screen.
    // The real game puts her on the RIGHT with the menu panel on the LEFT --
    // established from a photograph of the running Windows game, not argued.
    if (LTWorldMirror()) for (int k = 0; k < 4; ++k) P[k] = -P[k];

    LT3D::Multiply(P, V, VP);

    if (TraceOn())
    {
        fprintf(stderr, "[LT3D] camera pos (%.2f %.2f %.2f) quat (%.3f %.3f %.3f %.3f) "
                        "fov (%.3f %.3f) fullscreen=%d rect %d,%d..%d,%d\n",
                pCamera->m_Pos[0], pCamera->m_Pos[1], pCamera->m_Pos[2],
                pCamera->m_Quat[0], pCamera->m_Quat[1], pCamera->m_Quat[2], pCamera->m_Quat[3],
                pCamera->m_fFovX, pCamera->m_fFovY, pCamera->m_bFullScreen ? 1 : 0,
                pCamera->m_Rect[0], pCamera->m_Rect[1], pCamera->m_Rect[2], pCamera->m_Rect[3]);
        fprintf(stderr, "[LT3D] camera fwd (%.3f %.3f %.3f) up (%.3f %.3f %.3f)\n",
                fwd[0], fwd[1], fwd[2], up[0], up[1], up[2]);
    }

    int nPixels = 0;
    int nDrawn = 0;

    for (int i = 0; i < nObjects; ++i)
    {
        LTSceneObject* pObj = ppObjects[i];
        if (!pObj) continue;

        // ⚠️ A SPRITE: a camera-facing quad, the game view's geometry term
        // for term (ltworldrender.cpp DrawSpriteQuad, after d3d_DrawSprite) --
        // half-extents of texture size x scale along the camera's right and
        // up, +up at V=0. Positions are taken as the models' are in this pass.
        // LT_NOMENUSPRITES=1 restores models-only (says so).
        if (pObj->m_nType == OT_SPRITE)
        {
            static const bool s_bOff = []() {
                const bool b = (getenv("LT_NOMENUSPRITES") != 0);
                if (b) { printf("[LT3D] LT_NOMENUSPRITES=1 -- menu sprites not drawn\n"); fflush(stdout); }
                return b; }();
            if (s_bOff || pObj->m_sFilename.empty() || !(pObj->m_Flags & FLAG_VISIBLE)) continue;
            const int nOp = (pObj->m_Flags2 & FLAG2_ADDITIVE) ? 1 : ((pObj->m_Flags2 & FLAG2_MULTIPLY) ? 2 : 0);
            const LT3DTexture* pTex = SpriteTexture(pObj->m_sFilename,
                LTGetClock()->GetTime() - pObj->m_fAnimStart, pObj->m_Color[3], nOp);
            if (!pTex || pTex->m_Width <= 0 || pTex->m_Height <= 0) continue;
            const float sx = (float)pTex->m_Width  * (pObj->m_Scale[0] > 0.0f ? pObj->m_Scale[0] : 1.0f);
            const float sy = (float)pTex->m_Height * (pObj->m_Scale[1] > 0.0f ? pObj->m_Scale[1] : 1.0f);
            const float cr[3] = { camM[0], camM[4], camM[8] }, cu[3] = { camM[1], camM[5], camM[9] };
            float W[4][3];
            for (int k = 0; k < 3; ++k)
            {
                const float c = pObj->m_Pos[k], R = cr[k] * sx, U = cu[k] * sy;
                W[0][k] = c + U - R; W[1][k] = c + U + R; W[2][k] = c - U + R; W[3][k] = c - U - R;
            }
            static const float kU[4] = { 0.0f, 1.0f, 1.0f, 0.0f }, kV[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            if (pCollect)
            {
                LTSceneGpuItem it;
                memset(&it, 0, sizeof(it));
                it.m_nKind = 1;
                it.m_nPose = -1;
                for (int k = 0; k < 16; ++k) it.m_MVP[k] = VP[k];
                it.m_pTex = pTex;
                for (int q = 0; q < 4; ++q) for (int k = 0; k < 3; ++k) it.m_Corner[q][k] = W[q][k];
                for (int k = 0; k < 3; ++k) it.m_RGB[k] = pObj->m_Color[k];
                pCollect->push_back(it);
                ++nDrawn;
                continue;
            }
            LT3DVert v[4];
            for (int q = 0; q < 4; ++q)
            {
                LT3D::TransformPoint(VP, W[q], v[q].m_Clip);
                v[q].m_U = kU[q]; v[q].m_V = kV[q];
                v[q].m_R = pObj->m_Color[0]; v[q].m_G = pObj->m_Color[1]; v[q].m_B = pObj->m_Color[2];
            }
            int n = LT3D::DrawTriangle(target, pTex, v[0], v[1], v[2], false);
            n += LT3D::DrawTriangle(target, pTex, v[0], v[2], v[3], false);
            nPixels += n;
            ++nDrawn;
            if (TraceOn())
                fprintf(stderr, "[LT3D] sprite %-36s %dx%d scale %.2f  px %d\n", pObj->m_sFilename.c_str(),
                        pTex->m_Width, pTex->m_Height, pObj->m_Scale[0], n);
            continue;
        }

        if (pObj->m_nType != OT_MODEL) continue;
        if (!pObj->m_pModel) continue;
        if (!(pObj->m_Flags & FLAG_VISIBLE)) continue;

        // Model matrix: rotate/translate, then scale in the object's own space.
        float M[16], S[16], MV[16], MVP[16];
        LT3D::FromQuatPos(pObj->m_Quat, pObj->m_Pos, M);

        LT3D::Identity(S);
        S[0] = pObj->m_Scale[0];
        S[5] = pObj->m_Scale[1];
        S[10] = pObj->m_Scale[2];
        LT3D::Multiply(M, S, MV);
        LT3D::Multiply(VP, MV, MVP);

        // The normal matrix is the model rotation composed with the view, so
        // lighting follows the camera the way the interface expects.
        float NM[16];
        LT3D::Multiply(V, M, NM);

        // Pose for this frame, if the object is playing an animation.
        LTModelPose pose;
        const LTModelPose* pPose = 0;
        if (pObj->m_nAnim >= 0
            && (size_t)pObj->m_nAnim < pObj->m_pModel->GetAnims().size())
        {
            const LTABCAnim& a = pObj->m_pModel->GetAnims()[pObj->m_nAnim];
            float fElapsed = LTGetClock()->GetTime() - pObj->m_fAnimStart;
            unsigned int kf = KeyframeAt(a, fElapsed, pObj->m_bLooping);
            if (LTModelDraw::BuildPose(*pObj->m_pModel, (unsigned)pObj->m_nAnim, kf, pose))
                pPose = &pose;
        }

        if (pCollect)
        {
            LTSceneGpuItem it;
            memset(&it, 0, sizeof(it));
            it.m_nKind = 0;
            it.m_pModel = pObj->m_pModel;
            it.m_pSkins = pObj->m_Skins.empty() ? 0 : &pObj->m_Skins[0];
            it.m_nSkins = (int)pObj->m_Skins.size();
            it.m_nPose = -1;
            if (pPose && pPoses) { pPoses->push_back(*pPose); it.m_nPose = (int)pPoses->size() - 1; }
            for (int k = 0; k < 16; ++k) { it.m_MVP[k] = MVP[k]; it.m_Normal[k] = NM[k]; }
            pCollect->push_back(it);
            ++nDrawn;
            ++m_nModelsDrawn;
            continue;
        }

        // ⚠️ `bMirrorU` IS FALSE HERE, AND THE WORLD PASSES TRUE. That
        // asymmetry is deliberate, measured, and NOT understood -- which is
        // why it is written down rather than tidied into a shared constant.
        //
        //   WORLD: the "CLASSIFIED" stamp on LooseFiles_01 in M01S01 reads
        //          BACKWARDS without the flip and correctly with it.
        //   MENU:  with the flip, Cate's FACE is destroyed and the suit's
        //          placket lands on the wrong side; without it she matches the
        //          reference frame exactly.
        //
        // Both are A/B comparisons against a known-good picture, so both are
        // believed; what is missing is a reason the same ABC needs opposite
        // treatment in the two passes. The likely place to look is that the
        // menu composes its matrix from a QUATERNION and the world from EULER
        // angles, so one of the two may carry a reflection the other does not.
        // ⚠️ Do not "unify" these without re-checking BOTH pictures.
        LTModelDrawStats st = LTModelDraw::Draw(
            target, *pObj->m_pModel, MVP, NM,
            pObj->m_Skins.empty() ? 0 : &pObj->m_Skins[0],
            (int)pObj->m_Skins.size(), 0, false, pPose, 0, false);

        nPixels += st.m_nPixels;
        ++nDrawn;
        ++m_nModelsDrawn;

        if (TraceOn())
        {
            // ⚠️ Report the actual numbers when nothing lands. "0 pixels" with
            // no context is exactly the black-screen situation HANDOVER.md §5
            // warns about; the clip coordinate of one real vertex says whether
            // the model is behind the camera, off-screen, or degenerate.
            float mn[3], mx[3];
            LTModelDraw::GetBounds(*pObj->m_pModel, mn, mx);
            float c[3] = { (mn[0]+mx[0])*0.5f, (mn[1]+mx[1])*0.5f, (mn[2]+mx[2])*0.5f };
            float clip[4];
            LT3D::TransformPoint(MVP, c, clip);
            fprintf(stderr, "[LT3D]   obj pos (%.1f %.1f %.1f) scale (%.2f %.2f %.2f) "
                            "flags %#x\n",
                    pObj->m_Pos[0], pObj->m_Pos[1], pObj->m_Pos[2],
                    pObj->m_Scale[0], pObj->m_Scale[1], pObj->m_Scale[2], pObj->m_Flags);
            fprintf(stderr, "[LT3D]   model centre (%.1f %.1f %.1f) -> clip "
                            "(%.2f %.2f %.2f w=%.2f)%s\n",
                    c[0], c[1], c[2], clip[0], clip[1], clip[2], clip[3],
                    clip[3] <= 0.0f ? "   BEHIND THE CAMERA" : "");
        }

        if (TraceOn())
            fprintf(stderr, "[LT3D] draw  %-38s tris %5d  px %7d\n",
                    pObj->m_sFilename.c_str(), st.m_nTriangles, st.m_nPixels);
    }

    if (TraceOn())
        fprintf(stderr, "[LT3D] RenderObjects: %d object(s) in, %d model(s) drawn, "
                        "%d pixel(s)\n", nObjects, nDrawn, nPixels);

    return nPixels;
}


// ----------------------------------------------------------------------- //
// The SDK entry points.
//
// ⚠️ These are FUNCTION-POINTER members, not virtuals -- HANDOVER.md §4.5.
// ILTClient carries 208 of them, and a missed one is a jump to address 0 with
// no [LTSTUB] line in front of it. Each is announced exactly as the generated
// stubs are, so the census keeps working.
// ----------------------------------------------------------------------- //

static CLTScene g_Scene;
CLTScene* LTGetScene() { return &g_Scene; }

static inline LTSceneObject* Obj(HLOCALOBJ h)
{
    LTSceneObject* p = (LTSceneObject*)h;
    return g_Scene.IsValid(p) ? p : 0;
}

// ⚠️ THE GAME'S OWN FLAGS, READ BACK OFF A MIRROR. NOLF's client decides
// whether to take the view from a cinematic camera by asking the camera object
// for USRFLG_CAMERA_LIVE (GameClientShell.cpp:2443), so a stub answering
// LT_ERROR here is a camera that can never be switched on. Replication keeps
// the value in step with the server's; this only reads it.
static LTRESULT SC_GetObjectUserFlags(HLOCALOBJ hObj, uint32* pFlags)
{
    if (!pFlags) return LT_ERROR;
    LTSceneObject* p = Obj(hObj);
    if (!p) { *pFlags = 0; return LT_ERROR; }
    *pFlags = (uint32)p->m_UserFlags;

    // ⚠️ THE OTHER END OF LT_CAMTRACE. The server owns USRFLG_CAMERA_LIVE
    // (1<<5, Camera::TurnOn/TurnOff) and the CLIENT decides what to do about
    // it: CGameClientShell::UpdateAlternativeCamera (GameClientShell.cpp:2445)
    // reads it here, every frame, and a live camera means
    // TurnOnAlternativeCamera -> m_weaponModel.Disable(LTTRUE). Tracing both
    // ends is what separates "the server never cleared it" from "the client
    // never saw it cleared".
    // ⚠️ PER OBJECT, not one global flag. The first version of this trace kept
    // a single "is a camera live" bool and toggled it on every OTHER object
    // the game asked about -- a stream of on/off lines that described the scan
    // order rather than the cameras.
    if (getenv("LT_CAMTRACE"))
    {
        static std::map<const void*, int> live;
        const int nNow = (*pFlags & (1u << 5)) ? 1 : 0;
        int& nWas = live[(const void*)p];
        if (nNow != nWas)
        {
            nWas = nNow;
            // ⚠️ BIT 5 IS ONLY "CAMERA LIVE" ON A CAMERA. Other objects carry
            // it meaning something else (0x102a turns up on characters), and
            // the game only tests it while walking its camera FX list -- so
            // this line reports the bit, and does not claim what it implies.
            printf("[CAM] the client reads object %p with bit 5 %s "
                   "(userflags 0x%08x)%s\n",
                   (const void*)p, nNow ? "SET" : "clear", *pFlags,
                   (nNow && *pFlags == 0x20u)
                       ? "  <- a bare camera: the weapon is disabled while this lasts" : "");
            fflush(stdout);
        }
    }
    return LT_OK;
}


static HLOCALOBJ SC_CreateObject(ObjectCreateStruct* pStruct)
{
    LTIMPL("ILTClient::CreateObject");
    if (!pStruct) return (HLOCALOBJ)0;

    LTSceneCreateInfo info;
    info.m_nType = pStruct->m_ObjectType;
    info.m_sFilename = pStruct->m_Filename;
    for (int i = 0; i < MAX_MODEL_TEXTURES; ++i)
    {
        if (pStruct->m_SkinNames[i][0])
            info.m_Skins.push_back(pStruct->m_SkinNames[i]);
    }
    info.m_Pos[0] = pStruct->m_Pos.x;
    info.m_Pos[1] = pStruct->m_Pos.y;
    info.m_Pos[2] = pStruct->m_Pos.z;
    info.m_Scale[0] = pStruct->m_Scale.x;
    info.m_Scale[1] = pStruct->m_Scale.y;
    info.m_Scale[2] = pStruct->m_Scale.z;

    // ⚠️ LTRotation is a quaternion; operator[] gives its four floats in
    // (x, y, z, w) order, which is exactly LT3D::FromQuatPos's order.
    LTRotation r = pStruct->m_Rotation;
    for (int i = 0; i < 4; ++i) info.m_Quat[i] = r[i];

    info.m_Flags = pStruct->m_Flags;
    info.m_Flags2 = pStruct->m_Flags2;

    void* h = g_Scene.CreateObject(info);

    // ⚠️ LT_OBJTRACE=1 -- what the CLIENT was asked to make, and whether it
    // said yes. The scene answers null for a type it cannot represent
    // (deliberate -- see LTScene::CreateObject), so "the game asked and got
    // nothing" and "the game never asked" are different problems that look
    // identical from outside.
    if (getenv("LT_OBJTRACE"))
    {
        printf("[OBJ] client create type %u '%s' -> %s\n",
               info.m_nType,
               info.m_sFilename.empty() ? "(none)" : info.m_sFilename.c_str(),
               h ? "ok" : "REFUSED");
        fflush(stdout);
    }

    return (HLOCALOBJ)h;
}

void* LTScene_CreateMirror(unsigned int nType,
                           const float* pPos3, const float* pQuat4,
                           unsigned int nFlags, const char* pFilename)
{
    LTSceneCreateInfo info;
    info.m_nType = nType;
    info.m_sFilename = pFilename ? pFilename : "";

    for (int k = 0; k < 3; ++k) info.m_Pos[k] = pPos3 ? pPos3[k] : 0.0f;
    for (int k = 0; k < 4; ++k)
        info.m_Quat[k] = pQuat4 ? pQuat4[k] : (k == 3 ? 1.0f : 0.0f);

    // ⚠️ UNIT SCALE, not the server's. The server record does not carry one --
    // scale is a render property the game sets separately -- and a zero here
    // would collapse every mirror to a point, which looks like "replication
    // does not work" and is a struct field nobody filled in.
    info.m_Scale[0] = info.m_Scale[1] = info.m_Scale[2] = 1.0f;

    info.m_Flags  = nFlags;
    info.m_Flags2 = 0;

    return (void*)g_Scene.CreateObject(info);
}


// ---- the virtual half (see ltscene.h) ---------------------------------- //

int LTScene_RemoveObject(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p) return 0;
    return g_Scene.RemoveObject(p) ? 1 : 0;
}

// The last object moved with collision (the player's mover) -- for
// LT_LIFTTRACE, which reports anything else that sets its position.
static void* g_pLastMover = 0;

int LTScene_SetObjectPos(void* hObj, float x, float y, float z)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p) return 0;
    static const bool s_bTrace = (getenv("LT_LIFTTRACE") != 0);
    if (s_bTrace && hObj == g_pLastMover)
    { printf("[LIFT] SetObjectPos on the mover: (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)\n",
             p->m_Pos[0], p->m_Pos[1], p->m_Pos[2], x, y, z); fflush(stdout); }
    p->m_Pos[0] = x; p->m_Pos[1] = y; p->m_Pos[2] = z;
    return 1;
}

// ---- client physics ---------------------------------------------------- //

namespace
{
    // ⚠️ COUNTED, because "the player moves now" is otherwise a claim nobody
    // can check from a headless run -- which is the only kind this port
    // measures. Distance is the number that matters: a mover that is CALLED
    // and always blocked looks identical to one that is never called.
    unsigned int g_nCliMove = 0, g_nCliBlocked = 0, g_nCliTeleport = 0;
    unsigned int g_nAccelSetNZ = 0, g_nAccelSetZero = 0;
    double       g_fCliDist = 0.0;
}

// ⚠️ THE WORLD AS AN OBJECT, and the game cannot land without it.
//
// `CMoveMgr::UpdateOnGround` decides the player is standing on something with
// one test -- `if (Info.m_hObject)` (CMoveMgr.cpp:672). Standing on level
// geometry in LithTech means that handle is THE WORLD OBJECT, which the real
// engine creates and this port did not have. With it null the player is
//永远 falling: CMoveMgr keeps applying gravity, velocity grows without bound,
// and the first run of the client mover travelled 8.5e17 world units in 900
// frames before this existed.
//
// ⚠️ OT_NORMAL and invisible on purpose -- it is a handle to compare against,
// not something to draw. Created once, lazily, and never destroyed.
void* LTScene_WorldObject()
{
    static void* s_hWorld = 0;
    if (!s_hWorld)
    {
        const float pos[3] = { 0.0f, 0.0f, 0.0f };
        const float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        s_hWorld = LTScene_CreateMirror(OT_NORMAL, pos, quat, 0, 0);
    }
    return s_hWorld;
}

bool LTScene_IsWorldObject(void* hObj)
{
    return hObj != 0 && hObj == LTScene_WorldObject();
}

void LTScene_DumpPhysCensus()
{
    if (!g_nCliMove && !g_nCliTeleport) return;
    printf("\n[PHYS] client movement\n");
    printf("       %u move(s): %u went somewhere, %u blocked; %u teleport(s)\n",
           g_nCliMove + g_nCliBlocked, g_nCliMove, g_nCliBlocked, g_nCliTeleport);
    printf("       total distance travelled: %.1f world units\n", g_fCliDist);
    printf("       the game set acceleration %u time(s) non-zero, %u zero\n",
           g_nAccelSetNZ, g_nAccelSetZero);
    if (g_nCliMove && g_fCliDist < 1.0)
        printf("       ⚠️  CALLED BUT GOING NOWHERE -- blocked, or zero deltas.\n");
    fflush(stdout);
}

bool LTScene_GetVelocity(void* hObj, float* pV3)
{
    if (!pV3) return false;
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    // Written on every path -- the caller uses the value regardless.
    for (int k = 0; k < 3; ++k) pV3[k] = p ? p->m_Vel[k] : 0.0f;
    return p != 0;
}

bool LTScene_SetVelocity(void* hObj, const float* pV3)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || !pV3) return false;
    for (int k = 0; k < 3; ++k) p->m_Vel[k] = pV3[k];
    return true;
}

bool LTScene_GetAcceleration(void* hObj, float* pA3)
{
    if (!pA3) return false;
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    for (int k = 0; k < 3; ++k) pA3[k] = p ? p->m_Accel[k] : 0.0f;
    return p != 0;
}

bool LTScene_SetAcceleration(void* hObj, const float* pA3)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || !pA3) return false;
    for (int k = 0; k < 3; ++k) p->m_Accel[k] = pA3[k];

    // ⚠️ THE NON-ZERO ONES ONLY. CMoveMgr zeroes the acceleration at the top
    // of every UpdateNormalMotion (CMoveMgr.cpp:1045), so a trace of every
    // call is 99% zeros and hides the one value worth seeing.
    if (pA3[0] || pA3[1] || pA3[2]) ++g_nAccelSetNZ;
    else                            ++g_nAccelSetZero;

    return true;
}

// ⚠️ THE MODEL'S OWN DIMENSIONS, per animation. `.abc` carries a box for every
// animation (LTABCAnim::m_Dims) because a character's bounds change when it
// crouches or lies down -- which is exactly what the game asks this for.
//
// ⚠️ WITHOUT IT THE PLAYER IS A ONE-UNIT SPHERE. CMoveMgr sizes itself from
// `GetModelAnimUserDims` (CMoveMgr.cpp:1674) and, with a stub, kept its
// default (1,1,1). A collision radius of one unit turns every small object
// into an obstacle and the carpet into a wall.
bool LTScene_GetAnimDims(void* hObj, int nAnim, float* pD3)
{
    if (!pD3) return false;
    pD3[0] = pD3[1] = pD3[2] = 0.0f;

    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || !p->m_pModel) return false;

    const std::vector<LTABCAnim>& anims = p->m_pModel->GetAnims();
    if (anims.empty()) return false;

    // ⚠️ A negative or out-of-range index means "whatever it is now" rather
    // than a failure -- the game passes the CURRENT animation and sometimes
    // has not chosen one yet.
    int n = (nAnim >= 0 && (size_t)nAnim < anims.size()) ? nAnim : p->m_nAnim;
    if (n < 0 || (size_t)n >= anims.size()) n = 0;

    for (int k = 0; k < 3; ++k) pD3[k] = anims[(size_t)n].m_Dims[k];
    return true;
}

// The sphere this port collides with: the smaller horizontal half-extent, so
// a character fits the doorways it is meant to. Named once -- the mover and
// the dims change have to agree or a resize means something different to each.
static float Radius(const float* pD3)
{
    float r = (pD3[0] < pD3[2]) ? pD3[0] : pD3[2];
    return (r < 1.0f) ? 1.0f : r;
}

bool LTScene_GetPhysDims(void* hObj, float* pD3)
{
    if (!pD3) return false;
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    for (int k = 0; k < 3; ++k) pD3[k] = p ? p->m_Dims[k] : 0.0f;
    return p != 0;
}

// ⚠️ A DIMS CHANGE CAN LEAVE THE OBJECT INSIDE THE FLOOR, AND THIS LIFTS IT
// OUT. That is not tidiness -- without it the player sinks into the UNITY
// lobby floor the moment it lands.
//
// The game resizes the player's collider whenever the animation changes, and
// CMoveMgr::ResetDims (CMoveMgr.cpp:1723) does it in three steps: shrink to a
// 0.5 cube, MOVE DOWN by the height the box is about to lose, then set the
// real dims back. The move keeps a BOX's feet on the ground -- a box's centre
// has to drop by exactly the half-height it loses.
//
// This port sweeps a SPHERE of radius min(dims.x, dims.z). Its radius is 24
// whether the box is 53 tall or 24, so the game's compensating drop is a drop
// into the floor and nothing brings it back up. Measured on T01S02: the player
// landed correctly at y=24, the game asked for animation 1 (dims 24x24x24
// against the standing 24x53x24), ResetDims moved the centre down 29, and the
// player stayed at y=1 with its middle below the floor -- which is what
// "falling out of the lobby" looked like once the real fall was fixed.
//
// ⚠️ ONLY WHEN THE NEW SIZE OVERLAPS AND THE OLD ONE DID NOT. An object the
// game has deliberately put inside geometry stays where it was put, and an
// object merely resting on a surface is not overlapping it, so a standing
// player is never nudged.
//
// ⚠️ AND IT GIVES UP RATHER THAN GUESS. If no lift within the radius frees the
// sphere, the position is left exactly as it was: a wrong position the game
// chose is debuggable, one this function invented is not.
//
// The real fix is a box collider. This is the same approximation the stair
// lift makes, kept in one place and marked as one.
// How far below the box centre the mover's sphere sits -- see
// LTScene_MoveWithCollision. One definition, so the lift below tests the
// sphere that actually collides.
static bool CentreSphere()
{
    static const bool s_b = []() {
        const bool b = (getenv("LT_SPHEREPLAYER") != 0);
        if (b) { printf("[PHYS] LT_SPHEREPLAYER=1 -- the client sphere is swept at the box centre\n"); fflush(stdout); }
        return b; }();
    return s_b;
}
static float SphereFeet(const float* pD3, float r)
{
    return CentreSphere() ? 0.0f : ((pD3[1] > r) ? (pD3[1] - r) : 0.0f);
}

bool LTScene_SetPhysDims(void* hObj, const float* pD3)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || !pD3) return false;

    // ⚠️ THE SPHERE THAT COLLIDES, not the centre. Since 5bb the mover sweeps
    // its sphere at the BOX BOTTOM (centre - (dims.y - r)); this lift tested a
    // sphere at the CENTRE. M13S01 puts its GameStartPoint 19 units above the
    // floor: the game teleports the player there as a 0.5 cube and restores
    // 24x53x24 (OnServerForcePos, SETDIMS_PUSHOBJECTS). The old test lifted
    // the centre sphere 6 units clear and left the BOTTOM one 24 inside the
    // floor -- Cate dropped through it at spawn, fell 337 units and started the
    // level on 3.5 health (HANDOVER 5bi). The donor grows the box against the
    // world (moveobject.cpp GrowDim), which rests its bottom on the floor.
    // LT_OLDDIMSLIFT=1 = the centre-sphere test (announces itself).
    static const bool s_bOld = []() {
        const bool b = (getenv("LT_OLDDIMSLIFT") != 0);
        if (b) { printf("[PHYS] LT_OLDDIMSLIFT=1 -- dims lift tests the centre sphere\n"); fflush(stdout); }
        return b; }();

    const float fOld = Radius(p->m_Dims);
    const float fOldFeet = s_bOld ? 0.0f : SphereFeet(p->m_Dims, fOld);
    for (int k = 0; k < 3; ++k) p->m_Dims[k] = pD3[k];
    const float fNew = Radius(p->m_Dims);
    const float fNewFeet = s_bOld ? 0.0f : SphereFeet(p->m_Dims, fNew);

    if (!LTWorldCollide_Ready()) return true;
    if (s_bOld && fNew <= fOld) return true;

    const float pos[3]  = { p->m_Pos[0], p->m_Pos[1], p->m_Pos[2] };
    const float sOld[3] = { pos[0], pos[1] - fOldFeet, pos[2] };
    const float sNew[3] = { pos[0], pos[1] - fNewFeet, pos[2] };
    if (!LTGetWorldCollide()->Overlaps(sNew, fNew)) return true;
    if (LTGetWorldCollide()->Overlaps(sOld, fOld))  return true;   // already inside

    const float fMax = (s_bOld ? fNew * 2.0f : (p->m_Dims[1] + fNew) * 2.0f) + 2.0f;
    for (float fLift = 2.0f; fLift <= fMax; fLift += 2.0f)
    {
        float up[3] = { sNew[0], sNew[1] + fLift, sNew[2] };
        if (LTGetWorldCollide()->Overlaps(up, fNew)) continue;

        if (getenv("LT_PHYSTRACE"))
            printf("[PHYS] dims %.0f -> %.0f left the object inside the world; "
                   "lifted %.0f to (%.1f %.1f %.1f)\n",
                   fOld, fNew, fLift, pos[0], pos[1] + fLift, pos[2]);

        p->m_Pos[1] = pos[1] + fLift;
        return true;
    }
    return true;
}

bool LTScene_GetOnGround(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    return p ? p->m_bOnGround : false;
}

// The surface under the object's feet: a world ray from just above the box
// bottom straight down. Normal, point and polygon id of the first hit; false
// when nothing is within reach (the sphere may rest on an edge the ray misses).
bool LTScene_GetGroundContact(void* hObj, float* pNormal3, float* pPoint3, unsigned int* pPoly)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || !LTWorldCollide_Ready()) return false;
    const float fBottom = p->m_Pos[1] - p->m_Dims[1];
    const float from[3] = { p->m_Pos[0], fBottom + 4.0f, p->m_Pos[2] };
    const float to[3]   = { p->m_Pos[0], fBottom - 24.0f, p->m_Pos[2] };
    float pt[3], n[3], t;
    unsigned int nPoly = 0;
    if (!LTGetWorldCollide()->RaySegment(from, to, pt, n, &t, &nPoly)) return false;
    for (int k = 0; k < 3; ++k) { if (pNormal3) pNormal3[k] = n[k]; if (pPoint3) pPoint3[k] = pt[k]; }
    if (pPoly) *pPoly = nPoly;
    return true;
}

void LTScene_TeleportTo(void* hObj, const float* pPos3)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || !pPos3) return;
    static const bool s_bTrace = (getenv("LT_LIFTTRACE") != 0);
    if (s_bTrace && hObj == g_pLastMover)
    { printf("[LIFT] TELEPORT of the mover: (%.1f %.1f %.1f) -> (%.1f %.1f %.1f)\n",
             p->m_Pos[0], p->m_Pos[1], p->m_Pos[2], pPos3[0], pPos3[1], pPos3[2]); fflush(stdout); }
    for (int k = 0; k < 3; ++k) p->m_Pos[k] = pPos3[k];
    p->m_bOnGround = false;
    ++g_nCliTeleport;
    if (getenv("LT_PHYSTRACE"))
        printf("[PHYS] cli TELEPORT to (%.1f %.1f %.1f)\n",
               pPos3[0], pPos3[1], pPos3[2]);
}

// ⚠️ RIDING LIFTS AND PLATFORMS (HANDOVER 5bl). In the donor a solid world
// model that moves on the client is handed to the game (IClientShell::
// OnObjectMove), and CMoveMgr answers with ILTClientPhysics::MovePushObjects
// for the player: the model moves and carries what stands on it
// (MO_MOVESTANDINGONS). This port has no client world-model objects to report,
// so the carry is done here, once per frame after the server has placed the
// brushes: a player whose foot sphere rests on a brush that moved is moved by
// that brush's motion, with collision. Without it a player standing still on
// the M02S01 elevator was left behind as the cab rose (and, before the ground
// probe knew about brushes, "rode" it only by falling onto it every frame).
// LT_NOCARRY=1 = no carrying (says so).
bool LTScene_CarryOnMovers(void* hObj)
{
    static const bool s_bOff = []() {
        const bool b = (getenv("LT_NOCARRY") != 0);
        if (b) { printf("[PHYS] LT_NOCARRY=1 -- moving world models carry nothing\n"); fflush(stdout); }
        return b; }();
    if (s_bOff) return false;
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || !LTWorldCollide_Ready() || !LTServerBrush_MovedCount()) return false;

    const float r = Radius(p->m_Dims);
    const float fFeet = SphereFeet(p->m_Dims, r);
    const float c[3] = { p->m_Pos[0], p->m_Pos[1] - fFeet, p->m_Pos[2] };
    float d[3];
    if (!LTServerBrush_CarryPoint(c, r, d)) return false;

    const float want[3] = { p->m_Pos[0] + d[0], p->m_Pos[1] + d[1], p->m_Pos[2] + d[2] };
    LTScene_MoveWithCollision(hObj, want, 0);
    static const bool s_bTrace = (getenv("LT_CARRYTRACE") != 0);
    if (s_bTrace)
    {
        printf("[CARRY] obj %p by (%.2f %.2f %.2f) -> (%.1f %.1f %.1f) ground=%d\n", hObj,
               d[0], d[1], d[2], p->m_Pos[0], p->m_Pos[1], p->m_Pos[2], (int)p->m_bOnGround);
        fflush(stdout);
    }
    return true;
}

// ⚠️ THE SAME GRID THE SERVER USES (ltworldcollide.h). One world, one answer
// to "is this solid" -- the player and the AI must not disagree about a wall.
bool LTScene_MoveWithCollision(void* hObj, const float* pWant3, bool* pbGround)
{
    if (pbGround) *pbGround = false;

    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || !pWant3) return false;
    g_pLastMover = hObj;

    if (!LTWorldCollide_Ready())
    {
        // ⚠️ Say so ONCE rather than silently teleporting forever: a client
        // that never built collision looks exactly like a collision bug.
        static bool bSaid = false;
        if (!bSaid)
        {
            bSaid = true;
            fprintf(stderr, "[PHYS] client move with no collision built -- "
                            "moves are teleports until a world is loaded.\n");
        }
        for (int k = 0; k < 3; ++k) p->m_Pos[k] = pWant3[k];
        return true;
    }

    // The box's smaller horizontal half-extent, so a character fits the
    // doorways it is meant to. See ltserverphysics.cpp for the same choice.
    const float r = Radius(p->m_Dims);

    if (getenv("LT_PHYSTRACE"))
    {
        static bool bSaid = false;
        if (!bSaid)
        {
            bSaid = true;
            printf("[PHYS] player dims (%.1f %.1f %.1f) -> sphere r=%.1f, "
                   "stair=%.1f\n",
                   p->m_Dims[0], p->m_Dims[1], p->m_Dims[2], r,
                   LTServerPhys_GetStairHeight());
            fflush(stdout);
        }
    }

    // ⚠️ THE SPHERE IS SWEPT AT THE BOTTOM OF THE BOX, not at its centre.
    // The game's box is 24 x 53 x 24 (half extents); a radius-24 sphere at the
    // centre rests the centre 24 above the floor, so the player stood 29 units
    // too low and the eye (centre + 41, PlayerShared.h) sat ~65 above the
    // floor instead of ~94 -- "the player feels a metre tall" (HANDOVER 5bb).
    // The sphere is moved by the feet offset into the box's bottom and back,
    // and a second sphere at the top of the box keeps the head out of ceilings
    // the 106-unit box would not fit under. LT_SPHEREPLAYER=1 = the old mover.
    const float fFeet = SphereFeet(p->m_Dims, r);

    const float start[3] = { p->m_Pos[0], p->m_Pos[1] - fFeet, p->m_Pos[2] };
    const float delta[3] = { pWant3[0] - p->m_Pos[0],
                             pWant3[1] - p->m_Pos[1],
                             pWant3[2] - p->m_Pos[2] };

    float pos[3] = { start[0], start[1], start[2] };
    bool  bGround = false;
    LTGetWorldCollide()->MoveSphere(pos, delta, r, &bGround);

    // ⚠️ THE STAIR LIFT, AND ITS ABSENCE IS WHY A CARPET STOPPED THE PLAYER.
    //
    // The server mover has had this since it was written
    // (ltserverphysics.cpp); the CLIENT mover -- the one that actually moves
    // the player -- was a plain sweep. With no lift, anything at all standing
    // proud of the floor is a wall: a rug, a threshold, a kerb. The symptom is
    // absurd enough to look like a physics bug rather than a missing step-up.
    //
    // If the flat part of the move was mostly blocked, retry from stair height
    // and drop back down.
    const float fFlatWanted = fabsf(delta[0]) + fabsf(delta[2]);
    const float fFlatGot    = fabsf(pos[0] - start[0]) + fabsf(pos[2] - start[2]);
    // ⚠️ THE LIFT IS CAPPED BELOW THE SPHERE RADIUS, and that cap is the
    // difference between stepping over a rug and climbing onto a table.
    //
    // The game's stair height (32 here) is written for its own BOX collider,
    // whose bottom is flat and whose sides are vertical. This port sweeps a
    // SPHERE, so a lift of 32 with a radius of 24 puts the sphere's underside
    // above a table top and the player walks onto the furniture.
    //
    // The walk viewer -- 102 worlds, years of use -- pairs radius 24 with a
    // 22 step, deliberately just under the radius. Same ratio here.
    //
    // ⚠️ THIS IS AN APPROXIMATION AND IS MARKED AS ONE. The real fix is a box
    // collider; until then the cap keeps the two failure modes (blocked by a
    // carpet / climbing the scenery) both out of reach.
    float fStair = LTServerPhys_GetStairHeight();
    const float fStairCap = r * 0.9f;
    if (fStair > fStairCap) fStair = fStairCap;

    // ⚠️ TRY THE LIFT WHENEVER THE MOVE CAME UP SHORT, not only when it was
    // half blocked (HANDOVER 5cx). A riser lower than the sphere's radius does
    // not block it, it RAMPS it: the sphere rides up the step edge, keeps
    // ~half its speed and leaves the ground for a frame at every step --
    // measured on M04S02's stairs, 2.9 units/frame against 5.0 on the flat,
    // "on ground" flickering, which the game plays as being in the air ("slow
    // and stuckish", user report). The lifted path is kept only when it gets
    // further, so a wall slide or a slope that the lift does not help keeps the
    // plain move. LT_OLDSTAIRLIFT=1 = lift only when half blocked (old, says so).
    static const bool s_bOldLift = []() {
        const bool b = (getenv("LT_OLDSTAIRLIFT") != 0);
        if (b) { printf("[PHYS] LT_OLDSTAIRLIFT=1 -- the client stair lift only when a move is half blocked (old)\n"); fflush(stdout); }
        return b; }();
    // Only from the ground and not moving up: the lift replaces the move's
    // vertical part (lift, flat, drop), which would swallow a jump -- including
    // its first frame, still on the ground -- or a fall that grazes a wall.
    // Otherwise the old rule stands.
    const bool bTryLift = (fFlatGot < fFlatWanted * 0.5f) ||
                          (!s_bOldLift && p->m_bOnGround && delta[1] <= 0.5f &&
                           fFlatGot < fFlatWanted * 0.95f);

    if (fStair > 0.0f && fFlatWanted > 0.01f && bTryLift)
    {
        float lifted[3] = { start[0], start[1] + fStair, start[2] };
        // LT_LIFTTRACE=1 -- each lift attempt and why it was or was not taken.
        static const bool s_bLiftTrace = (getenv("LT_LIFTTRACE") != 0);
        const bool bOverlap = LTGetWorldCollide()->Overlaps(lifted, r);
        if (s_bLiftTrace && bOverlap)
        { printf("[LIFT] at (%.1f %.1f %.1f) +%.1f: lifted sphere OVERLAPS -- not taken (flat got %.2f of %.2f)\n",
                 start[0], start[1], start[2], fStair, fFlatGot, fFlatWanted); fflush(stdout); }
        if (!bOverlap)
        {
            float flat[3] = { delta[0], 0.0f, delta[2] };
            bool  bG2 = false;
            LTGetWorldCollide()->MoveSphere(lifted, flat, r, &bG2);

            const float fGot2 = fabsf(lifted[0] - start[0]) + fabsf(lifted[2] - start[2]);
            if (s_bLiftTrace)
            { printf("[LIFT] at (%.1f %.1f %.1f) +%.1f: lifted got %.2f, flat got %.2f of %.2f -- %s\n",
                     start[0], start[1], start[2], fStair, fGot2, fFlatGot, fFlatWanted,
                     fGot2 > fFlatGot ? "taken" : "not taken"); fflush(stdout); }
            if (fGot2 > fFlatGot)
            {
                const float fMax = fStair + 1.0f;
                if (s_bOldLift)
                {
                    float drop[3] = { 0.0f, -fMax, 0.0f };
                    LTGetWorldCollide()->MoveSphere(lifted, drop, r, &bG2);
                }
                else
                {
                    // ⚠️ STRAIGHT DOWN, NO SLIDE (HANDOVER 5cx). A swept drop
                    // that lands on a step's EDGE slides along the slanted
                    // contact -- back toward the lower step -- and gave away
                    // most of what the lift had won: every lift was taken and
                    // the player still climbed at 58%. The resting height at
                    // this x/z is the lowest the sphere can sit without
                    // overlapping, found by bisection.
                    float probe[3] = { lifted[0], lifted[1] - fMax, lifted[2] };
                    if (!LTGetWorldCollide()->Overlaps(probe, r))
                    {
                        lifted[1] -= fMax;              // nothing below within reach
                        bG2 = false;
                    }
                    else
                    {
                        float lo = 0.0f, hi = fMax;     // lo free, hi overlapping
                        for (int it = 0; it < 12; ++it)
                        {
                            const float mid = 0.5f * (lo + hi);
                            probe[1] = lifted[1] - mid;
                            if (LTGetWorldCollide()->Overlaps(probe, r)) hi = mid; else lo = mid;
                        }
                        lifted[1] -= lo;
                        bG2 = true;
                    }
                }
                for (int k = 0; k < 3; ++k) pos[k] = lifted[k];
                bGround = bG2;
            }
        }
    }

    // The head sphere follows the achieved move; if it is stopped sooner (a
    // low lintel), the whole box stops where the head did.
    if (fFeet > 0.0f)
    {
        float head[3] = { start[0], start[1] + 2.0f * fFeet, start[2] };
        const float got[3] = { pos[0] - start[0], pos[1] - start[1], pos[2] - start[2] };
        bool bHG = false;
        if (!LTGetWorldCollide()->Overlaps(head, r))
        {
            LTGetWorldCollide()->MoveSphere(head, got, r, &bHG);
            const float hx = head[0] - (start[0] + got[0]), hz = head[2] - (start[2] + got[2]);
            static const bool s_bHeadTrace = (getenv("LT_LIFTTRACE") != 0);
            if (s_bHeadTrace && (hx*hx + hz*hz > 0.25f || (got[1] > 0.0f && head[1] < start[1] + 2.0f * fFeet + got[1] - 0.5f)))
            { printf("[LIFT]   head sphere at (%.1f %.1f %.1f) moving (%.1f %.1f %.1f) stopped short by (%.1f, %.1f) horizontally, %.1f vertically\n",
                     start[0], start[1] + 2.0f * fFeet, start[2], got[0], got[1], got[2], hx, hz,
                     (start[1] + 2.0f * fFeet + got[1]) - head[1]); fflush(stdout); }
            if (hx*hx + hz*hz > 0.25f)
            { pos[0] = head[0]; pos[2] = head[2]; }
            if (got[1] > 0.0f && head[1] < start[1] + 2.0f * fFeet + got[1] - 0.5f)
                pos[1] = head[1] - 2.0f * fFeet;          // bumped the ceiling
        }
    }

    for (int k = 0; k < 3; ++k) p->m_Pos[k] = pos[k];
    p->m_Pos[1] += fFeet;                               // back to the box centre
    p->m_bOnGround = bGround;
    if (pbGround) *pbGround = bGround;

    // LT_PHYSTRACE=N traces the first N moves (default 12), so a slow fall
    // can be watched all the way down instead of only at the top.
    static const unsigned s_nTraceMax = (getenv("LT_PHYSTRACE") && atoi(getenv("LT_PHYSTRACE")) > 1)
                                        ? (unsigned)atoi(getenv("LT_PHYSTRACE")) : 12u;
    if (getenv("LT_PHYSTRACE") && g_nCliMove + g_nCliBlocked < s_nTraceMax)
        printf("[PHYS] cli move #%u  from (%.1f %.1f %.1f) want (%.1f %.1f %.1f) "
               "-> (%.1f %.1f %.1f)  r=%.1f ground=%d\n",
               g_nCliMove + g_nCliBlocked,
               start[0], start[1], start[2],
               pWant3[0], pWant3[1], pWant3[2],
               pos[0], pos[1], pos[2], r, (int)bGround);

    const double dx = pos[0]-start[0], dy = pos[1]-start[1], dz = pos[2]-start[2];
    const bool bMoved = (dx != 0.0 || dy != 0.0 || dz != 0.0);
    if (bMoved) { ++g_nCliMove; g_fCliDist += sqrt(dx*dx + dy*dy + dz*dz); }
    else        { ++g_nCliBlocked; }

    return bMoved;
}

unsigned int LTScene_GetObjectUserFlags(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    return p ? p->m_UserFlags : 0u;
}


void LTScene_SetObjectUserFlags(void* hObj, unsigned int nFlags)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (p) p->m_UserFlags = nFlags;
}


unsigned int LTScene_GetObjectFlags(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    return p ? p->m_Flags : 0;
}

void LTScene_SetObjectFlags(void* hObj, unsigned int nFlags)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (p) p->m_Flags = nFlags;
}

bool LTScene_SFXDelivered(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    return p && p->m_bSFXDelivered;
}

void LTScene_SetSFXDelivered(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (p) p->m_bSFXDelivered = true;
}

void LTScene_ResetSFXDelivered()
{
    const std::vector<LTSceneObject*>& v = g_Scene.Objects();
    for (size_t i = 0; i < v.size(); ++i) if (v[i]) v[i]->m_bSFXDelivered = false;
}

unsigned int LTScene_GetObjectFlags2(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    return p ? p->m_Flags2 : 0;
}

void LTScene_SetObjectFlags2(void* hObj, unsigned int nFlags2)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (p) p->m_Flags2 = nFlags2;
}

// ⚠️ ANIMATION LOOKUP BY FILENAME, FOR THE SERVER.
//
// The SERVER has no scene: it knows an object's model as a STRING
// ("Chars\\Models\\Arch_base.abc") and nothing else. But the game asks the
// server for animation indices -- every cinematic's
// `msg victim (animate anim=walkstop)` reaches ILTCSBase::GetAnimIndex on the
// SERVER's interface -- and a stub there means no scripted animation in the
// game ever selects anything.
//
// ⚠️ ONE MODEL CACHE, SHARED. This resolves through the same cache the client
// draws from, so the server cannot disagree with the renderer about which
// animation index a name means -- which would be a silent mismatch, the worst
// kind, since both sides would look internally consistent.
//
// Returns -1 when the model or the animation is unknown; the caller must then
// leave its output alone rather than treat that as animation 0.
int LTScene_GetAnimIndexForFile(const char* pFilename, const char* pAnimName)
{
    return g_Scene.AnimIndexForFile(pFilename, pAnimName);
}


// ---- NODE CONTROL -- how NOLF animates a FACE ----------------------------- //
//
// ⚠️ ALL THREE WERE STUBS, and the first one reached was enough to switch the
// whole mechanism off. CNodeController::Init walks the model's nodes with
// GetNextModelNode, maps them by name to its skeleton, and from then on drives
// them through ModelNodeControl: the JAW from the amplitude of the voice that
// is playing (lip sync), the HEAD toward a speaker. With GetNextModelNode
// failing, the map stayed empty and no character's mouth ever moved -- the
// Windows build shows Cate mid-word in the frame the port shows her closed.
//
// ⚠️ HMODELNODE IS THE FILE'S NODE INDEX. The renderer calls the callback with
// the same index while walking the same file's hierarchy (LTModelDraw::
// NodeHook), so the two sides agree by construction.
namespace
{
    std::map<void*, std::pair<LTSceneNodeFn, void*> > g_NodeControl;

    const CLTABCModel* ModelOf(HLOCALOBJ hObj)
    {
        LTSceneObject* p = Obj(hObj);
        if (!p || p->m_sFilename.empty()) return 0;
        return g_Scene.ModelForFile(p->m_sFilename);
    }

    // ⚠️ LT_NODETRACE=1 -- who initialises node control, who installs it,
    // and whether the character's update gate (USRFLG_GAMEBASE_ACTIVE, 1<<23)
    // would even let CNodeController::Update run. Each line once per object.
    bool NodeTraceOn()
    {
        static const bool b = (getenv("LT_NODETRACE") != 0);
        return b;
    }

    LTRESULT SC_GetNextModelNode(HLOCALOBJ hObj, HMODELNODE hNode, HMODELNODE* pNext)
    {
        LTIMPL("ILTClient::GetNextModelNode");
        if (NodeTraceOn() && hNode == INVALID_MODEL_NODE)
        {
            static std::set<const void*> seen;
            if (seen.insert(hObj).second)
            {
                LTSceneObject* po = Obj(hObj);
                printf("[NODE] enumerate %p %s  at (%.0f %.0f %.0f) flags 0x%08x userflags 0x%08x%s\n", (void*)hObj,
                       po ? po->m_sFilename.c_str() : "(no object)",
                       po ? po->m_Pos[0] : 0.0f, po ? po->m_Pos[1] : 0.0f, po ? po->m_Pos[2] : 0.0f,
                       po ? po->m_Flags : 0u,
                       po ? po->m_UserFlags : 0u,
                       (po && (po->m_UserFlags & (1u << 23))) ? "" : "   ⚠️  NOT GAMEBASE_ACTIVE -- CharacterFX::Update returns early");
                fflush(stdout);
            }
        }
        if (!pNext) return LT_ERROR;
        const CLTABCModel* pM = ModelOf(hObj);
        if (!pM) return LT_ERROR;
        const HMODELNODE n = (hNode == INVALID_MODEL_NODE) ? 0 : hNode + 1;
        if (n >= (HMODELNODE)pM->GetNodes().size()) return LT_FINISHED;
        *pNext = n;
        return LT_OK;
    }

    LTRESULT SC_GetModelNodeName(HLOCALOBJ hObj, HMODELNODE hNode, char* pName, uint32 maxLen)
    {
        LTIMPL("ILTClient::GetModelNodeName");
        if (!pName || !maxLen) return LT_ERROR;
        const CLTABCModel* pM = ModelOf(hObj);
        if (!pM || hNode >= (HMODELNODE)pM->GetNodes().size()) return LT_ERROR;
        strncpy(pName, pM->GetNodes()[hNode].m_sName.c_str(), maxLen - 1);
        pName[maxLen - 1] = 0;
        return LT_OK;
    }
}

const CLTABCModel* LTScene_ModelForFile(const char* pFilename)
{
    if (!pFilename || !*pFilename) return 0;
    return g_Scene.ModelForFile(pFilename);
}

void LTScene_SetNodeControl(void* hObj, LTSceneNodeFn fn, void* pUser)
{
    if (NodeTraceOn())
    {
        static std::map<const void*, bool> state;
        std::map<const void*, bool>::iterator it = state.find(hObj);
        if (it == state.end() || it->second != (fn != 0))
        {
            state[hObj] = (fn != 0);
            LTSceneObject* po = Obj((HLOCALOBJ)hObj);
            printf("[NODE] control %p %s -> %s\n", hObj,
                   po ? po->m_sFilename.c_str() : "(no object)", fn ? "INSTALLED" : "none");
            fflush(stdout);
        }
    }
    if (!hObj) return;
    if (fn) g_NodeControl[hObj] = std::make_pair(fn, pUser);
    else    g_NodeControl.erase(hObj);
}

bool LTScene_GetNodeControl(void* hObj, LTSceneNodeFn* pFn, void** ppUser)
{
    std::map<void*, std::pair<LTSceneNodeFn, void*> >::const_iterator it = g_NodeControl.find(hObj);
    if (it == g_NodeControl.end()) return false;
    if (pFn) *pFn = it->second.first;
    if (ppUser) *ppUser = it->second.second;
    return true;
}

// ⚠️ LENGTH IN MILLISECONDS, i.e. the last key's time -- the unit the game's
// GetCurAnimLength and MS_PLAYDONE are defined in. -1 when unknown, so a caller
// cannot mistake "no such animation" for "an animation of length zero", which
// would report every one-shot as instantly finished.
bool LTScene_GetAnimDimsForFile(const char* pFilename, int nAnim, float* pOut3)
{
    return g_Scene.AnimDimsForFile(pFilename, nAnim, pOut3);
}

int LTScene_GetAnimLengthForFile(const char* pFilename, int nAnim)
{
    return g_Scene.AnimLengthForFile(pFilename, nAnim);
}

int LTScene_GetAnimInterpForFile(const char* pFilename, int nAnim)
{
    return g_Scene.AnimInterpForFile(pFilename, nAnim);
}

int LTScene_GetWeightSetForFile(const char* pFilename, const char* pSetName)
{
    return g_Scene.WeightSetForFile(pFilename, pSetName);
}

// ⚠️ WHAT THE CLIENT ACTUALLY HOLDS, AND WHETHER IT CAN DRAW IT.
//
// "There are no NPCs" has at least four causes that look identical from a
// chair: the server never made them, the server made them but named no model,
// they were replicated but the model file failed to load, or they loaded and
// are invisible. Every one of those was guessed at during this investigation
// before being measured.
//
// CF_NOTIFYMODELKEYS is (1<<1) in ltcodes.h; spelled out rather than included
// so this file keeps depending on nothing but the model and the clock.
static const unsigned int kCF_NOTIFYMODELKEYS = (1u << 1);

// Counts model objects by state and names the ones that cannot be drawn.
void CLTScene::DumpModelCensus() const
{
    unsigned nModels = 0, nNoFile = 0, nNotLoaded = 0, nInvisible = 0, nDrawable = 0;
    std::map<std::string, unsigned> badFiles;

    for (size_t i = 0; i < m_Objects.size(); ++i)
    {
        const LTSceneObject* p = m_Objects[i];
        if (!p || p->m_nType != OT_MODEL) continue;
        ++nModels;

        if (p->m_sFilename.empty())          { ++nNoFile; continue; }
        if (!p->m_pModel)                    { ++nNotLoaded; ++badFiles[p->m_sFilename]; continue; }
        if (!(p->m_Flags & FLAG_VISIBLE))    { ++nInvisible; continue; }
        ++nDrawable;
    }

    printf("\n===== client models =====\n");
    printf("  model objects : %u\n", nModels);
    printf("  drawable      : %u\n", nDrawable);
    if (nNoFile)    printf("  ⚠️  %u with NO FILENAME -- the server never named a model\n", nNoFile);
    if (nNotLoaded) printf("  ⚠️  %u whose model FAILED TO LOAD\n", nNotLoaded);
    if (nInvisible) printf("  %u not FLAG_VISIBLE (hidden on purpose, or never shown)\n", nInvisible);

    for (std::map<std::string, unsigned>::const_iterator i = badFiles.begin();
         i != badFiles.end(); ++i)
        printf("        x%-3u %s\n", i->second, i->first.c_str());
    // ⚠️ AND WHETHER ANY OF THEM REACHED THE RASTERISER. "13 drawable" is a
    // statement about the scene; "0 drawn" is a statement about the renderer,
    // and only the pair distinguishes "the characters are missing" from "the
    // characters are there and something is culling them".
    printf("  render calls  : %u,  model draws: %u,  missing models: %u\n",
           m_nRenderCalls, m_nModelsDrawn, m_nMissingModels);

    // ⚠️ LT_MODELDUMP=1 -- WHICH models, not how many. "24 model objects" and
    // "the player has no weapon in hand" are perfectly consistent with each
    // other, and only the list tells them apart: the player-view weapon is a
    // client-side object like any other, and if CWeaponModel could not create
    // it then m_hObject is null and UpdateWeaponModel returns W_IDLE before it
    // looks at the trigger at all (WeaponModel.cpp:583).
    if (getenv("LT_MODELDUMP"))
    {
        std::map<std::string, unsigned> seen;
        for (size_t i = 0; i < m_Objects.size(); ++i)
        {
            const LTSceneObject* p = m_Objects[i];
            if (!p || p->m_nType != OT_MODEL || p->m_sFilename.empty()) continue;
            ++seen[p->m_sFilename];
        }
        printf("  --- client model files ---\n");
        for (std::map<std::string, unsigned>::const_iterator i = seen.begin();
             i != seen.end(); ++i)
            printf("        x%-3u %s\n", i->second, i->first.c_str());

        // ⚠️ AND WHETHER A MODEL KEY COULD EVER REACH THE GAME. Three things
        // have to be true -- the object asked (CF_NOTIFYMODELKEYS), it is
        // playing an animation, and that animation carries command strings --
        // and all three fail in the same way from outside: nothing happens.
        printf("  --- model keys ---\n");
        for (size_t i = 0; i < m_Objects.size(); ++i)
        {
            const LTSceneObject* p = m_Objects[i];
            if (!p || p->m_nType != OT_MODEL || !p->m_pModel) continue;

            unsigned nWithCmd = 0, nAnimsWithCmd = 0;
            const std::vector<LTABCAnim>& aa = p->m_pModel->GetAnims();
            for (size_t ia = 0; ia < aa.size(); ++ia)
            {
                unsigned n = 0;
                for (size_t k = 0; k < aa[ia].m_Keyframes.size(); ++k)
                    if (!aa[ia].m_Keyframes[k].m_sCommand.empty()) ++n;
                if (n) { ++nAnimsWithCmd; nWithCmd += n; }
            }
            printf("        %-44s cflags 0x%-4x anim %-4d  %u key(s) in %u anim(s)%s\n",
                   p->m_sFilename.c_str(), p->m_ClientFlags, p->m_nAnim,
                   nWithCmd, nAnimsWithCmd,
                   (p->m_ClientFlags & kCF_NOTIFYMODELKEYS) ? "  NOTIFY" : "");
        }
    }
    printf("=========================\n");
    fflush(stdout);
}

void LTScene_DumpModelCensus()
{
    g_Scene.DumpModelCensus();
    printf("  in-game view  : %u game-spawned model draw(s) via RenderCamera%s\n",
           g_nDynamicDrawn,
           g_nDynamicDrawn ? "" : "   ⚠️  NONE -- no character reached the level view");
    fflush(stdout);
}


bool LTScene_SetObjectModel(void* hObj, const char* pFilename)
{
    return g_Scene.SetObjectModel(Obj((HLOCALOBJ)hObj), pFilename);
}

int LTScene_GetAnimIndex(void* hObj, const char* pName)
{
    return g_Scene.GetAnimIndex(Obj((HLOCALOBJ)hObj), pName);
}

void LTScene_SetAnim(void* hObj, int nAnim)
{
    g_Scene.SetAnim(Obj((HLOCALOBJ)hObj), nAnim, LTGetClock()->GetTime());
}

void LTScene_SetLooping(void* hObj, bool bLoop)
{
    g_Scene.SetLooping(Obj((HLOCALOBJ)hObj), bLoop);
}

// ⚠️ THE READ HALF OF THE ANIMATION API, AND WITHOUT IT NOTHING CAN FIRE.
//
// The client could SET an animation and SET looping since the model pass was
// written, but every READ was a stub -- and NOLF's weapon is a state machine
// built entirely out of the reads (WeaponModel.cpp:2114, 2158, 2188, 2332,
// 2384):
//
//     uint32 dwState = g_pLTClient->GetModelPlaybackState(m_hObject);
//     if (!IsFireAni(g_pLTClient->GetModelAnimation(m_hObject)) ||
//         (dwState & MS_PLAYDONE)) ...
//
// With GetModelAnimation stuck at 0 and the playback state stuck at "not
// done", the machine cannot leave the state it is in: the weapon never
// finishes selecting, never fires, never reloads. Pressing the trigger did
// exactly nothing and produced no sound, which is what was reported.
//
// ⚠️ SAME RULE AS THE SERVER'S (ltserverhost.cpp GetPlaybackState): a looping
// animation is NEVER done -- an idle that reported done would end every state
// machine instantly -- and an animation whose length we cannot determine
// reports NOT done, because "done" is the answer that makes the game act.
int LTScene_GetAnim(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    return p ? p->m_nAnim : -1;
}

bool LTScene_GetLooping(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    return p ? p->m_bLooping : false;
}

void LTScene_RestartAnim(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (p) { p->m_fAnimStart = LTGetClock()->GetTime(); p->m_fKeysUpToMs = -1.0f; }
}

// Milliseconds since the current animation started, and its length; -1 length
// means "unknown", which the caller must treat as not finished.
bool LTScene_GetAnimProgress(void* hObj, float* pElapsedMs, int* pLengthMs)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p) return false;

    if (pElapsedMs)
        *pElapsedMs = (LTGetClock()->GetTime() - p->m_fAnimStart) * 1000.0f;

    if (pLengthMs)
    {
        *pLengthMs = -1;
        if (p->m_pModel && p->m_nAnim >= 0)
        {
            const std::vector<LTABCAnim>& anims = p->m_pModel->GetAnims();
            if ((size_t)p->m_nAnim < anims.size() &&
                !anims[(size_t)p->m_nAnim].m_Keyframes.empty())
                *pLengthMs = (int)anims[(size_t)p->m_nAnim].m_Keyframes.back().m_nTime;
        }
    }
    return true;
}

bool LTScene_IsAnimDone(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || p->m_nAnim < 0) return false;

    float fMs = 0.0f; int nLen = -1;
    const bool bGot = LTScene_GetAnimProgress(hObj, &fMs, &nLen);
    const bool bDone = bGot && !p->m_bLooping && nLen >= 0 && fMs >= (float)nLen;

    if (getenv("LT_KEYTRACE"))
    {
        static std::set<std::string> seen;
        char szK[320];
        snprintf(szK, sizeof(szK), "%s#%d#%d", p->m_sFilename.c_str(),
                 p->m_nAnim, (int)bDone);
        if (seen.insert(szK).second)
            printf("[MKEY] playback %-36s anim %-4d loop %d  %.0f of %d ms -> %s\n",
                   p->m_sFilename.c_str(), p->m_nAnim, (int)p->m_bLooping,
                   fMs, nLen, bDone ? "DONE" : "playing");
    }
    return bDone;
}

// See the header. CF_NOTIFYMODELKEYS is (1<<1) in ltcodes.h; it is spelled
// out here rather than included so this file keeps depending on nothing but
// the model and the clock.

unsigned CLTScene::TakeModelKeys(LTModelKey* pOut, unsigned nMax)
{
    if (!pOut || !nMax) return 0;

    // ⚠️ LT_NOMODELKEYS=1 restores the old behaviour -- no keys delivered --
    // so the claim "this is what makes the weapon fire" can be measured rather
    // than asserted. It PRINTS when it engages: an A/B whose switch is silent
    // is an A/B that can pass while doing nothing (HANDOVER 5ai).
    static const bool bOff = (getenv("LT_NOMODELKEYS") != 0);
    if (bOff)
    {
        static bool bSaid = false;
        if (!bSaid) { bSaid = true; printf("[MKEY] disabled (LT_NOMODELKEYS)\n"); fflush(stdout); }
        return 0;
    }

    const float fNow = LTGetClock()->GetTime();
    unsigned n = 0;

    for (size_t i = 0; i < m_Objects.size() && n < nMax; ++i)
    {
        LTSceneObject* p = m_Objects[i];
        if (!p || p->m_nType != OT_MODEL) continue;
        if (!(p->m_ClientFlags & kCF_NOTIFYMODELKEYS)) continue;
        if (!p->m_pModel || p->m_nAnim < 0) continue;

        const std::vector<LTABCAnim>& anims = p->m_pModel->GetAnims();
        if ((size_t)p->m_nAnim >= anims.size()) continue;
        const LTABCAnim& a = anims[(size_t)p->m_nAnim];

        // ⚠️ REPORTED WHILE IT IS PLAYING, not at exit. The weapon object is
        // destroyed during OnEngineTerm, so a census printed afterwards cannot
        // see the one object the question is about.
        if (getenv("LT_KEYTRACE"))
        {
            static std::set<std::string> seen;
            char szK[320];
            snprintf(szK, sizeof(szK), "%s#%d", p->m_sFilename.c_str(), p->m_nAnim);
            if (seen.insert(szK).second)
            {
                unsigned nCmd = 0;
                for (size_t k = 0; k < a.m_Keyframes.size(); ++k)
                    if (!a.m_Keyframes[k].m_sCommand.empty()) ++nCmd;
                printf("[MKEY] playing %-40s anim %-4d '%s'  %u of %u keyframe(s) "
                       "carry a command\n",
                       p->m_sFilename.c_str(), p->m_nAnim, a.m_sName.c_str(),
                       nCmd, (unsigned)a.m_Keyframes.size());
                fflush(stdout);
            }
        }

        if (a.m_Keyframes.empty()) continue;

        const float fLen = (float)a.m_Keyframes.back().m_nTime;
        float fMs = (fNow - p->m_fAnimStart) * 1000.0f;
        if (fMs < 0.0f) fMs = 0.0f;

        float fFrom = p->m_fKeysUpToMs;          // -1 the first time round

        if (p->m_bLooping && fLen > 0.0f)
        {
            // ⚠️ THE WRAP IS TWO RANGES, NOT ONE. A looping animation whose
            // time has passed the end delivers the tail of the loop and then
            // the head; collapsing that into one comparison silently drops
            // every key in between, which for a footstep loop is most of them.
            const float fWrapped = fmodf(fMs, fLen);
            if (fWrapped < fFrom)
            {
                for (size_t k = 0; k < a.m_Keyframes.size() && n < nMax; ++k)
                {
                    const LTABCKeyframe& kf = a.m_Keyframes[k];
                    if (kf.m_sCommand.empty()) continue;
                    if ((float)kf.m_nTime <= fFrom) continue;
                    pOut[n].m_hObj = (void*)p;
                    pOut[n].m_pCommand = kf.m_sCommand.c_str();
                    ++n;
                }
                fFrom = -1.0f;                   // now take the head of the loop
            }
            fMs = fWrapped;
        }
        else if (fLen > 0.0f && fMs > fLen)
        {
            fMs = fLen;                          // a one-shot holds its last frame
        }

        for (size_t k = 0; k < a.m_Keyframes.size() && n < nMax; ++k)
        {
            const LTABCKeyframe& kf = a.m_Keyframes[k];
            if (kf.m_sCommand.empty()) continue;
            const float t = (float)kf.m_nTime;
            if (t <= fFrom || t > fMs) continue;
            pOut[n].m_hObj = (void*)p;
            pOut[n].m_pCommand = kf.m_sCommand.c_str();
            ++n;
        }

        p->m_fKeysUpToMs = fMs;
    }

    return n;
}

unsigned int LTScene_GetClientFlags(void* hObj)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    return p ? p->m_ClientFlags : 0u;
}

void LTScene_SetClientFlags(void* hObj, unsigned int nFlags)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (p) p->m_ClientFlags = nFlags;
}

// ⚠️ MODEL KEYS -- THE ANIMATION TELLING THE GAME TO DO SOMETHING, and until
// now the engine never delivered one.
//
// An ABC animation carries a command string on individual keyframes ("FIRE",
// "SOUND ...", "FX ..."), and the engine is expected to call
// IClientShell::OnModelKey as playback crosses them. NOLF's weapon depends on
// it completely: m_bFire is set in exactly two places, both of them inside
// OnModelKey (WeaponModel.cpp:1645, :1707), and Fire() runs only when m_bFire
// is set. So with no keys delivered the trigger was live, the command was set,
// the fire animation played -- and the weapon never fired. Footstep sounds and
// character FX come through the same door.
//
// ⚠️ Only objects that ASKED (CF_NOTIFYMODELKEYS) get keys, which is why the
// client flags had to stop being a stub first.
//
// ⚠️ Keys are emitted by TIME CROSSED, not by frame: everything in
// (m_fKeysUpToMs, now] fires, so a key is never missed because a frame was
// long and never repeats because a frame was short. A looping animation that
// wrapped delivers the tail of the loop and then the head.
unsigned LTScene_TakeModelKeys(LTModelKey* pOut, unsigned nMax)
{
    if (!pOut || !nMax) return 0;
    return g_Scene.TakeModelKeys(pOut, nMax);
}

int LTScene_FindSocket(void* hObj, const char* pName)
{
    return g_Scene.FindSocket(Obj((HLOCALOBJ)hObj), pName);
}

int LTScene_GetSocketTransform(void* hObj, int nSocket, int bWorldSpace,
                               float* pPos, float* pQuat)
{
    return g_Scene.GetSocketTransform(Obj((HLOCALOBJ)hObj), nSocket,
                                      bWorldSpace != 0, pPos, pQuat) ? 1 : 0;
}

int LTScene_GetObjectType(void* hObj, unsigned int* pType)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || !pType) return 0;
    *pType = p->m_nType;
    return 1;
}

static void SC_GetObjectPos(HLOCALOBJ hObj, LTVector* pPos)
{
    LTIMPL("ILTClient::GetObjectPos");
    LTSceneObject* p = Obj(hObj);

    // ⚠️ On a miss the caller's LTVector is LEFT ALONE, which sounds wrong and
    // is deliberate -- the same rule GetSurfaceDims follows. But note that
    // CBaseFolder::CreateScaleFX passes an UNINITIALISED vector here and then
    // stores the result back, so a miss propagates garbage rather than a zero.
    // That is the game's shape, not ours; the defence is that every object it
    // asks about really is in the scene.
    if (!p || !pPos) return;
    pPos->Init(p->m_Pos[0], p->m_Pos[1], p->m_Pos[2]);
}

static void SC_GetObjectRotation(HLOCALOBJ hObj, LTRotation* pRot)
{
    LTIMPL("ILTClient::GetObjectRotation");
    LTSceneObject* p = Obj(hObj);
    if (!p || !pRot) return;
    pRot->Init(p->m_Quat[0], p->m_Quat[1], p->m_Quat[2], p->m_Quat[3]);
}

static void SC_SetObjectRotation(HLOCALOBJ hObj, LTRotation* pRot)
{
    // ⚠️ THE PROBE FOR "IS THE WEAPON BEING UPDATED AT ALL". CWeaponModel
    // rotates its own object every frame it runs (WeaponModel.cpp:654), and
    // that line sits AFTER UpdateModelState -- so seeing this is proof the
    // state machine ran, and not seeing it is proof the game returned earlier.
    if (getenv("LT_KEYTRACE"))
    {
        LTSceneObject* p = Obj(hObj);
        // ⚠️ Models_PV, not "Guns\\" -- the first version of this probe matched
        // Guns\\Models_HH\\Sunglasses_hh.abc (a hand-held attachment) and
        // reported the weapon as updating when it was not. A probe that can
        // match the wrong object answers a different question than the one
        // asked.
        if (p && p->m_sFilename.find("Models_PV") != std::string::npos)
        {
            static bool bSaid = false;
            if (!bSaid)
            {
                bSaid = true;
                printf("[MKEY] the game is updating the weapon model (%s)\n",
                       p->m_sFilename.c_str());
                fflush(stdout);
            }
        }
    }

    LTIMPL("ILTClient::SetObjectRotation");
    LTSceneObject* p = Obj(hObj);
    if (!p || !pRot) return;
    for (int i = 0; i < 4; ++i) p->m_Quat[i] = (*pRot)[i];
}

static void SC_SetObjectPosAndRotation(HLOCALOBJ hObj, LTVector* pPos, LTRotation* pRot)
{
    LTIMPL("ILTClient::SetObjectPosAndRotation");
    LTSceneObject* p = Obj(hObj);
    if (!p) return;
    if (pPos) { p->m_Pos[0] = pPos->x; p->m_Pos[1] = pPos->y; p->m_Pos[2] = pPos->z; }
    if (pRot) { for (int i = 0; i < 4; ++i) p->m_Quat[i] = (*pRot)[i]; }
}

static LTRESULT SC_SetObjectScale(HLOCALOBJ hObj, LTVector* pScale)
{
    LTIMPL("ILTClient::SetObjectScale");
    LTSceneObject* p = Obj(hObj);
    if (!p || !pScale) return LT_ERROR;
    p->m_Scale[0] = pScale->x; p->m_Scale[1] = pScale->y; p->m_Scale[2] = pScale->z;
    return LT_OK;
}

static void SC_SetObjectColor(HLOCALOBJ hObj, float r, float g, float b, float a)
{
    LTIMPL("ILTClient::SetObjectColor");
    LTSceneObject* p = Obj(hObj);
    if (!p) return;
    p->m_Color[0] = r; p->m_Color[1] = g; p->m_Color[2] = b; p->m_Color[3] = a;
}

// ⚠️ WAS A STUB THAT WROTE NOTHING, and 17 ClientShellDLL files read it --
// the PolyGrid FX does `GetObjectColor(&r,&g,&b,&a); SetObjectColor(r,g,b,
// alpha)` (PolyGridFX.cpp:151), so its colour was whatever the stack held.
// Every output is written, also for an unknown object (white, opaque).
// LT_STUBGETCOLOR=1 = the old stub (announces itself).
static void SC_GetObjectColor(HLOCALOBJ hObj, float* r, float* g, float* b, float* a)
{
    LTIMPL("ILTClient::GetObjectColor");
    static const bool s_bOld = []() {
        const bool bOld = (getenv("LT_STUBGETCOLOR") != 0);
        if (bOld) { printf("[LT3D] LT_STUBGETCOLOR=1 -- client GetObjectColor writes nothing (old stub)\n"); fflush(stdout); }
        return bOld; }();
    if (s_bOld) return;
    const LTSceneObject* p = Obj(hObj);
    const float c[4] = { p ? p->m_Color[0] : 1.0f, p ? p->m_Color[1] : 1.0f,
                         p ? p->m_Color[2] : 1.0f, p ? p->m_Color[3] : 1.0f };
    if (r) *r = c[0];
    if (g) *g = c[1];
    if (b) *b = c[2];
    if (a) *a = c[3];
}

// ---- PolyGrids (iltclient.h:935) ----------------------------------------- //
//
// ⚠️ All seven were stubs, which is why SFX_POLYGRID_ID had to stay gated:
// CPolyGridFX::CreateObject writes its palette through the table pointer
// GetPolyGridInfo returns, so a stub that returned nothing was a write to
// wherever an uninitialised local pointed (AddressSanitizer: PolyGridFX.cpp:174).
// The semantics are the donor's (clientde_impl.cpp ci_*PolyGrid*, pg_Init).

static LTScenePolyGrid* SC_PolyGrid(HLOCALOBJ hObj)
{
    LTSceneObject* p = Obj(hObj);
    return (p && p->m_nType == OT_POLYGRID) ? p->m_pPolyGrid.get() : 0;
}

static LTBOOL SC_SetupPolyGrid(HLOCALOBJ hObj, uint32 width, uint32 height, LTBOOL bHalfTris)
{
    LTIMPL("ILTClient::SetupPolyGrid");
    LTSceneObject* p = Obj(hObj);
    if (!p || p->m_nType != OT_POLYGRID) return LTFALSE;
    // pg_Init's limits: at least 2 x 2, and the index list is 16-bit.
    if (width < 2 || height < 2 || width * height > 65000) return LTFALSE;
    std::unique_ptr<LTScenePolyGrid> pg(new LTScenePolyGrid);
    pg->m_W = width;
    pg->m_H = height;
    pg->m_bHalfTris = (bHalfTris != 0);
    pg->m_Data.assign((size_t)width * height, 0);
    memset(pg->m_Table, 0, sizeof(pg->m_Table));
    if (p->m_pPolyGrid)       // a re-setup keeps the texture state
    {
        pg->m_sSprite = p->m_pPolyGrid->m_sSprite;  pg->m_sEnvMap = p->m_pPolyGrid->m_sEnvMap;
        pg->m_xPan = p->m_pPolyGrid->m_xPan;        pg->m_yPan = p->m_pPolyGrid->m_yPan;
        pg->m_xScale = p->m_pPolyGrid->m_xScale;    pg->m_yScale = p->m_pPolyGrid->m_yScale;
    }
    p->m_pPolyGrid = std::move(pg);
    return LTTRUE;
}

static LTRESULT SC_SetPolyGridTexture(HLOCALOBJ hObj, char* pFilename)
{
    LTIMPL("ILTClient::SetPolyGridTexture");
    LTScenePolyGrid* pg = SC_PolyGrid(hObj);
    if (!pg) return LT_ERROR;
    pg->m_sSprite = pFilename ? pFilename : "";
    return LT_OK;
}

// ⚠️ STORED, NOT DRAWN: no shipped PolyGrid FX path sets one (the env map is
// the donor's reflective-water extra). Kept so a later renderer can use it.
static LTRESULT SC_SetPolyGridEnvMap(HLOCALOBJ hObj, char* pFilename)
{
    LTIMPL("ILTClient::SetPolyGridEnvMap");
    LTScenePolyGrid* pg = SC_PolyGrid(hObj);
    if (!pg) return LT_ERROR;
    pg->m_sEnvMap = pFilename ? pFilename : "";
    return LT_OK;
}

static LTRESULT SC_GetPolyGridTextureInfo(HLOCALOBJ hObj, float* xPan, float* yPan,
                                          float* xScale, float* yScale)
{
    LTIMPL("ILTClient::GetPolyGridTextureInfo");
    const LTScenePolyGrid* pg = SC_PolyGrid(hObj);
    // Every output written: the defaults (iltclient.h:951) when there is no grid.
    if (xPan)   *xPan   = pg ? pg->m_xPan   : 0.0f;
    if (yPan)   *yPan   = pg ? pg->m_yPan   : 0.0f;
    if (xScale) *xScale = pg ? pg->m_xScale : 1.0f;
    if (yScale) *yScale = pg ? pg->m_yScale : 1.0f;
    return pg ? LT_OK : LT_ERROR;
}

static LTRESULT SC_SetPolyGridTextureInfo(HLOCALOBJ hObj, float xPan, float yPan,
                                          float xScale, float yScale)
{
    LTIMPL("ILTClient::SetPolyGridTextureInfo");
    LTScenePolyGrid* pg = SC_PolyGrid(hObj);
    if (!pg) return LT_ERROR;
    pg->m_xPan = xPan; pg->m_yPan = yPan; pg->m_xScale = xScale; pg->m_yScale = yScale;
    return LT_OK;
}

static LTRESULT SC_GetPolyGridInfo(HLOCALOBJ hObj, char** pBytes, uint32* pWidth,
                                   uint32* pHeight, PGColor** pColorTable)
{
    LTIMPL("ILTClient::GetPolyGridInfo");
    LTScenePolyGrid* pg = SC_PolyGrid(hObj);

    // ⚠️ ON FAILURE A SIZE OF ZERO AND A REAL TABLE, not garbage: the FX
    // fills all 256 palette entries without checking the result
    // (PolyGridFX.cpp SetPolyGridPalette), so the table pointer must point
    // somewhere writable even when there is no grid.
    static float s_Scratch[256][4];
    static_assert(sizeof(PGColor) == sizeof(s_Scratch[0]), "PGColor is four floats");
    if (pBytes)      *pBytes      = pg ? &pg->m_Data[0] : 0;
    if (pWidth)      *pWidth      = pg ? pg->m_W : 0;
    if (pHeight)     *pHeight     = pg ? pg->m_H : 0;
    if (pColorTable) *pColorTable = (PGColor*)(pg ? pg->m_Table : s_Scratch);
    return pg ? LT_OK : LT_ERROR;
}

// ⚠️ NOT IN THE DONOR -- Jupiter dropped it for object dims. LT2's contract
// (iltclient.h:958): give the box, get back the position and the scale that
// make the grid fill it. With the layout above that is the box centre, the
// cell spacing in X and Z, and in Y the box height over the 254 steps of a
// -127..127 height -- the donor's own vertical rule (dims.y / 127).
static LTRESULT SC_FitPolyGrid(HLOCALOBJ hObj, LTVector* pMin, LTVector* pMax,
                               LTVector* pPos, LTVector* pScale)
{
    LTIMPL("ILTClient::FitPolyGrid");
    const LTScenePolyGrid* pg = SC_PolyGrid(hObj);
    if (!pMin || !pMax)
    {
        if (pPos)   pPos->Init(0.0f, 0.0f, 0.0f);
        if (pScale) pScale->Init(1.0f, 1.0f, 1.0f);
        return LT_ERROR;
    }
    if (pPos) *pPos = (*pMin + *pMax) * 0.5f;
    if (pScale)
    {
        if (!pg) { pScale->Init(1.0f, 1.0f, 1.0f); return LT_ERROR; }
        pScale->Init((pMax->x - pMin->x) / (float)(pg->m_W - 1),
                     (pMax->y - pMin->y) / 254.0f,
                     (pMax->z - pMin->z) / (float)(pg->m_H - 1));
    }
    // LT_PGTRACE=1 -- every grid fitted: its box and size.
    if (getenv("LT_PGTRACE") && pg)
    {
        printf("[PG] %ux%u grid fitted to (%.0f %.0f %.0f)..(%.0f %.0f %.0f)\n", pg->m_W, pg->m_H,
               pMin->x, pMin->y, pMin->z, pMax->x, pMax->y, pMax->z);
        fflush(stdout);
    }
    return pg ? LT_OK : LT_ERROR;
}

static void SC_SetCameraRect(HLOCALOBJ hObj, LTBOOL bFullscreen,
                             int left, int top, int right, int bottom)
{
    LTIMPL("ILTClient::SetCameraRect");
    LTSceneObject* p = Obj(hObj);
    if (!p) return;
    p->m_bFullScreen = (bFullscreen != 0);
    p->m_Rect[0] = left; p->m_Rect[1] = top;
    p->m_Rect[2] = right; p->m_Rect[3] = bottom;
}

static void SC_SetCameraFOV(HLOCALOBJ hObj, float fovX, float fovY)
{
    LTIMPL("ILTClient::SetCameraFOV");
    LTSceneObject* p = Obj(hObj);
    if (!p) return;
    p->m_fFovX = fovX; p->m_fFovY = fovY;
}

static void SC_GetCameraFOV(HLOCALOBJ hObj, float* pX, float* pY)
{
    LTIMPL("ILTClient::GetCameraFOV");
    LTSceneObject* p = Obj(hObj);
    if (!p) return;
    if (pX) *pX = p->m_fFovX;
    if (pY) *pY = p->m_fFovY;
}

static void SC_SetGlobalLightScale(LTVector* pScale)
{
    LTIMPL("ILTClient::SetGlobalLightScale");
    if (!pScale) return;
    float s[3] = { pScale->x, pScale->y, pScale->z };
    g_Scene.SetGlobalLightScale(s);
}

static void SC_GetGlobalLightScale(LTVector* pScale)
{
    LTIMPL("ILTClient::GetGlobalLightScale");
    if (!pScale) return;
    const float* s = g_Scene.GetGlobalLightScale();
    pScale->Init(s[0], s[1], s[2]);
}

// ⚠️ A CAMERA'S LIGHT ADD -- the game's screen flashes (hit, flash grenade,
// the water's LightAdd). Kept per camera; applied with the global light scale
// at the end of RenderCamera. Both halves were stubs (the getter answered
// LTFALSE and wrote nothing).
static std::map<const void*, LTVector> s_CamLightAdd;
static LTBOOL SC_SetCameraLightAdd(HLOCALOBJ hCamera, LTVector* pAdd)
{
    LTIMPL("ILTClient::SetCameraLightAdd");
    if (!Obj(hCamera) || !pAdd) return LTFALSE;
    s_CamLightAdd[(const void*)hCamera] = *pAdd;
    return LTTRUE;
}
static LTBOOL SC_GetCameraLightAdd(HLOCALOBJ hCamera, LTVector* pAdd)
{
    LTIMPL("ILTClient::GetCameraLightAdd");
    if (!pAdd) return LTFALSE;
    pAdd->Init(0.0f, 0.0f, 0.0f);
    if (!Obj(hCamera)) return LTFALSE;
    std::map<const void*, LTVector>::const_iterator it = s_CamLightAdd.find((const void*)hCamera);
    if (it != s_CamLightAdd.end()) *pAdd = it->second;
    return LTTRUE;
}

// ⚠️ THE GLOBAL LIGHT SCALE WAS STORED AND NEVER APPLIED. It is how the game
// tints the 3D view: underwater blue-green (CGameClientShell::UpdateContainer
// FX, TintColor / 255), the endless-fall fade to black, the light-scale
// manager's effects. Applied to the 3D frame only -- RenderCamera's output,
// both renderers write it into this surface -- so the HUD drawn afterwards is
// untouched, as in LithTech. Light add: the camera's additive colour (0..1).
// LT_NOLIGHTSCALE=1 = the old untinted frame (says so).
// The global light scale and this camera's light add -- (1,1,1) and (0,0,0)
// under LT_NOLIGHTSCALE=1 (says so).
static void LTScene_LightScaleFor(HLOCALOBJ hCamera, float* sc, float* add)
{
    static const bool s_bOff = []() {
        const bool b = (getenv("LT_NOLIGHTSCALE") != 0);
        if (b) { printf("[LT3D] LT_NOLIGHTSCALE=1 -- global light scale / camera light add not applied\n"); fflush(stdout); }
        return b; }();
    for (int k = 0; k < 3; ++k) { sc[k] = 1.0f; add[k] = 0.0f; }
    if (s_bOff) return;
    const float* g = g_Scene.GetGlobalLightScale();
    for (int k = 0; k < 3; ++k) sc[k] = g[k];
    std::map<const void*, LTVector>::const_iterator it = s_CamLightAdd.find((const void*)hCamera);
    if (it != s_CamLightAdd.end()) { add[0] = it->second.x; add[1] = it->second.y; add[2] = it->second.z; }
}

static void LTScene_ApplyLightScale(const LT2DSurf& dest, HLOCALOBJ hCamera)
{
    if (!dest.m_pRGBA) return;
    float sc[3], add[3];
    LTScene_LightScaleFor(hCamera, sc, add);
    const bool bScale = fabsf(sc[0]-1.0f) > 1e-3f || fabsf(sc[1]-1.0f) > 1e-3f || fabsf(sc[2]-1.0f) > 1e-3f;
    const bool bAdd   = add[0] > 1e-3f || add[1] > 1e-3f || add[2] > 1e-3f;
    if (!bScale && !bAdd) return;
    int lut[3][256];
    for (int c = 0; c < 3; ++c)
        for (int v = 0; v < 256; ++v)
        {
            int o = (int)(v * sc[c] + add[c] * 255.0f + 0.5f);
            lut[c][v] = o < 0 ? 0 : (o > 255 ? 255 : o);
        }
    unsigned char* px = dest.m_pRGBA;
    const size_t n = (size_t)dest.m_Width * (size_t)dest.m_Height;
    for (size_t i = 0; i < n; ++i, px += 4)
    {
        px[0] = (unsigned char)lut[0][px[0]];
        px[1] = (unsigned char)lut[1][px[1]];
        px[2] = (unsigned char)lut[2][px[2]];
    }
}

static LTSceneGpuObjectsFn s_pGpuObjects = 0;
void LTScene_SetGpuObjects(LTSceneGpuObjectsFn fn) { s_pGpuObjects = fn; }

static LTRESULT SC_RenderObjects(HLOCALOBJ hCamera, HLOCALOBJ* pObjects, int nObjects)
{
    LTIMPL("ILTClient::RenderObjects");

    LTSceneObject* pCam = Obj(hCamera);
    if (!pCam) return LT_ERROR;

    std::vector<LTSceneObject*> objs;
    objs.reserve(nObjects > 0 ? nObjects : 0);
    for (int i = 0; i < nObjects; ++i) objs.push_back(Obj(pObjects[i]));

    // ⚠️ Renders into the SCREEN SURFACE, which the 2D compositor then draws
    // the menu text on top of. Order matters and it is the game's, not ours:
    // InterfaceMgr does Start3D -> UpdateInterfaceSFX (this) ->
    // StartOptimized2D -> text -> EndOptimized2D -> End3D.
    CLTNullRender* pR = LTGetNullRender();
    LT2DSurf dest = pR->GetView(pR->GetScreenSurface());

    // ⚠️ THE MENU'S 3D AT THE OUTPUT'S RESOLUTION (HANDOVER 5ce). The menu's
    // background sprite and models used to be rasterised into the 640x480
    // menu box and zoomed with it; under the compositor they now go to the
    // native 3D target, at the camera's place in the zoomed box, and the 2D
    // layers are opened over the camera's area -- RenderCamera's rule. The
    // menu's text, drawn after this, blends onto them exactly as before.
    // Falls back to the software path below on any failure. LT_MENUGPU=0 =
    // always the software path (says so).
    static const bool s_bMenuGpu = []() {
        const char* e = getenv("LT_MENUGPU");
        const bool b = !(e && e[0] == '0');
        if (!b) { printf("[COMP] LT_MENUGPU=0 -- menu 3D drawn into the 2D surface (old)\n"); fflush(stdout); }
        return b; }();
    if (s_bMenuGpu && s_pGpuObjects && LTComp_Active() && pCam->m_nType == OT_CAMERA
        && dest.m_pRGBA && dest.m_Width > 0 && dest.m_Height > 0)
    {
        int surf[4] = { 0, 0, dest.m_Width, dest.m_Height };
        if (!pCam->m_bFullScreen)
        {
            surf[0] = pCam->m_Rect[0]; surf[1] = pCam->m_Rect[1];
            surf[2] = pCam->m_Rect[2]; surf[3] = pCam->m_Rect[3];
        }
        if (surf[2] > surf[0] && surf[3] > surf[1])
        {
            std::vector<LTSceneGpuItem> items;
            std::vector<LTModelPose> poses;
            g_Scene.RenderObjects(pCam, objs.empty() ? 0 : &objs[0], (int)objs.size(), dest, &items, &poses);
            int nOW = 0, nOH = 0, rect[4];
            LTComp_OutputSize(&nOW, &nOH);
            LTComp_ObjectsRect(dest.m_Width, dest.m_Height, surf, rect);
            if (s_pGpuObjects(nOW, nOH, rect, LTComp_FirstSceneThisFrame(),
                              items.empty() ? 0 : &items[0], (int)items.size(),
                              poses.empty() ? 0 : &poses[0]))
            {
                pR->OpenScreenRect(surf[0], surf[1], surf[2] - surf[0], surf[3] - surf[1]);
                LTComp_NoteObjects();
                return LT_OK;
            }
            static bool s_bSaid = false;
            if (!s_bSaid) { s_bSaid = true; fprintf(stderr, "[COMP] menu 3D on the GPU failed -- software path\n"); }
        }
    }

    // ⚠️ UNDER THE COMPOSITOR the models are drawn once, into the black layer,
    // and every pixel they changed is copied into the grey one -- an opaque
    // write lands identically in both (ltcomp.h). Drawing twice instead would
    // be simpler and wrong: the call advances nothing today, but it is the
    // game's object list, and a second pass is a second frame of it.
    LT2DSurf twin = pR->GetTwinView(pR->GetScreenSurface());
    static std::vector<unsigned char> s_Before;
    const size_t nBytes = (size_t)dest.m_Width * (size_t)dest.m_Height * 4;
    if (twin.m_pRGBA && dest.m_pRGBA) s_Before.assign(dest.m_pRGBA, dest.m_pRGBA + nBytes);

    g_Scene.RenderObjects(pCam, objs.empty() ? 0 : &objs[0], (int)objs.size(), dest);

    if (twin.m_pRGBA && dest.m_pRGBA && s_Before.size() == nBytes)
    {
        const unsigned int* pNow = (const unsigned int*)dest.m_pRGBA;
        const unsigned int* pOld = (const unsigned int*)&s_Before[0];
        unsigned int* pTwin = (unsigned int*)twin.m_pRGBA;
        const size_t n = nBytes / 4;
        for (size_t i = 0; i < n; ++i)
            if (pNow[i] != pOld[i]) pTwin[i] = pNow[i];
    }
    return LT_OK;
}

// ----------------------------------------------------------------------- //
//
// ⚠️ THE IN-GAME RENDER PATH, AND IT IS A DIFFERENT CALL FROM THE MENU'S.
//
//   the menu   CInterfaceMgr::UpdateInterfaceSFX -> RenderObjects(cam, objs, n)
//   the game   CGameClientShell::RenderCamera    -> RenderCamera(m_hCamera)
//                                                   -- GameClientShell.cpp:6902
//
// RenderObjects draws an explicit ARRAY of objects and no world; RenderCamera
// draws THE WORLD, from a camera, and the engine decides what is visible. That
// is why the menu could be finished long before a world loader existed, and
// why this one call is the whole difference between a menu and a level.
//
// ⚠️ THE WORLD IS NOT LOADED BY THIS CALL, and it must not be: the game asks
// for a level through the server, and by the time the client renders, the world
// is already up. With no world loaded this draws nothing and says so ONCE --
// silence here would look exactly like a camera pointing the wrong way.
//
// ----------------------------------------------------------------------- //

// ⚠️ INSTALLED, NOT CALLED. The render layer cannot know the server exists --
// a headless render test links this file without one -- so whoever owns the
// live objects hands them in. Null draws the world alone, as before.
static const ILTWorldDynamicObjects* s_pDynamic = 0;
void LTScene_SetDynamicSource(const ILTWorldDynamicObjects* p) { s_pDynamic = p; }

// ⚠️ THE CLIENT'S OWN MODELS IN THE GAME VIEW -- and the player-view weapon
// is one of them. RenderCamera drew the world and the SERVER's objects only,
// so everything the client makes by itself -- the gun in Cate's hands, and
// later shell casings, debris, gibs -- existed, animated, fired its model
// keys, and was never drawn. Mirrors of server objects are skipped (the
// server list already draws them); the local player's own body is never a
// visible client MODEL (it is OT_NORMAL), so nothing here sits inside the
// camera except what the game meant to: FLAG_REALLYCLOSE objects, whose
// position is in CAMERA space (WeaponModel.cpp:641 "camera-relative") and is
// composed with the camera here, so the renderer receives world space.
//
// LT_NOCLIENTMODELS=1 draws server objects only, as before (says so).
class CLTSceneViewObjects : public ILTWorldDynamicObjects
{
public:
    const ILTWorldDynamicObjects*   m_pServer = 0;
    std::vector<LTSceneObject*>     m_Client;       // snapshot for one frame
    float                           m_Cam[16];      // the camera's world matrix

    void Snapshot(const LTSceneObject* pCam)
    {
        m_Client.clear();
        m_Sprites.clear();
        m_PolyGrids.clear();
        LT3D::FromQuatPos(pCam->m_Quat, pCam->m_Pos, m_Cam);
        static const bool s_bOff = []() {
            const bool b = (getenv("LT_NOCLIENTMODELS") != 0);
            if (b) { printf("[LT3D] LT_NOCLIENTMODELS=1 -- client models are not drawn in the game view\n"); fflush(stdout); }
            return b; }();
        if (s_bOff) return;
        const std::vector<LTSceneObject*>& v = g_Scene.Objects();
        for (size_t i = 0; i < v.size(); ++i)
        {
            LTSceneObject* p = v[i];
            // ⚠️ FLAG_WASDRAWN -- "the engine sets this if a particle system
            // or PolyGrid was drawn" (ltbasedefs.h:1094), and CPolyGridFX
            // stops animating its water without it. Set for a visible grid,
            // cleared for a hidden one; no view test, so it errs on "drawn".
            if (p->m_nType == OT_POLYGRID)
            {
                const bool bDraw = (p->m_Flags & FLAG_VISIBLE) && p->m_pPolyGrid;
                if (bDraw) { p->m_Flags |= FLAG_WASDRAWN; m_PolyGrids.push_back(p); }
                else         p->m_Flags &= ~FLAG_WASDRAWN;
                continue;
            }
            // ⚠️ AND FLAG_PORTALVISIBLE (5df): hidden here, drawn in a mirror
            // -- the player's first-person-hidden attachments. Get() decides
            // per pass; only what the MAIN view shows is "in the game view".
            if (!(p->m_Flags & (FLAG_VISIBLE | FLAG_PORTALVISIBLE))) continue;
            if (LTReplicate_IsMirror(p)) continue;
            if (p->m_nType == OT_SPRITE && !p->m_sFilename.empty())
            { if (p->m_Flags & FLAG_VISIBLE) m_Sprites.push_back(p); continue; }
            if (p->m_nType != OT_MODEL || !p->m_pModel) continue;
            if (p->m_Flags & FLAG_VISIBLE) p->m_bInGameView = true;
            m_Client.push_back(p);
        }
    }

    std::vector<LTSceneObject*>     m_Sprites;
    std::vector<LTSceneObject*>     m_PolyGrids;

    unsigned int PolyGridCount() const override { return (unsigned)m_PolyGrids.size(); }

    bool GetPolyGrid(unsigned int i, LTDynamicPolyGrid& out) const override
    {
        if (i >= m_PolyGrids.size()) return false;
        const LTSceneObject* p = m_PolyGrids[i];
        const LTScenePolyGrid* pg = p->m_pPolyGrid.get();
        if (!pg || pg->m_W < 2 || pg->m_H < 2) return false;
        for (int k = 0; k < 3; ++k) { out.m_Pos[k] = p->m_Pos[k]; out.m_Scale[k] = p->m_Scale[k]; }
        for (int k = 0; k < 4; ++k) out.m_Quat[k] = p->m_Quat[k];
        out.m_W = pg->m_W;
        out.m_H = pg->m_H;
        out.m_pData  = (const signed char*)&pg->m_Data[0];
        out.m_pTable = pg->m_Table;
        out.m_sSprite = pg->m_sSprite;
        out.m_xPan = pg->m_xPan; out.m_yPan = pg->m_yPan;
        out.m_xScale = pg->m_xScale; out.m_yScale = pg->m_yScale;
        for (int k = 0; k < 3; ++k) out.m_Colour[k] = p->m_Color[k];
        out.m_fAlpha   = p->m_Color[3];
        out.m_nBlendOp = (p->m_Flags2 & FLAG2_ADDITIVE) ? 1 : ((p->m_Flags2 & FLAG2_MULTIPLY) ? 2 : 0);
        return true;
    }

    unsigned int SpriteCount() const override { return (unsigned)m_Sprites.size(); }

    bool GetSprite(unsigned int i, LTDynamicSprite& out) const override
    {
        if (i >= m_Sprites.size()) return false;
        const LTSceneObject* p = m_Sprites[i];
        // In a mirror: never FLAG2_PORTALINVISIBLE (the player-view FX, 5df).
        if (LTWorldPortalPass() && (p->m_Flags2 & FLAG2_PORTALINVISIBLE)) return false;
        out.m_Draw.m_sSprite  = p->m_sFilename;
        out.m_Draw.m_fScaleX  = p->m_Scale[0] > 0.0f ? p->m_Scale[0] : 1.0f;
        out.m_Draw.m_fScaleY  = p->m_Scale[1] > 0.0f ? p->m_Scale[1] : 1.0f;
        for (int k = 0; k < 3; ++k) out.m_Draw.m_Colour[k] = p->m_Color[k];
        out.m_Draw.m_fAlpha   = p->m_Color[3];
        out.m_Draw.m_nBlendOp = (p->m_Flags2 & FLAG2_ADDITIVE) ? 1 : ((p->m_Flags2 & FLAG2_MULTIPLY) ? 2 : 0);
        out.m_Draw.m_bGlow    = (p->m_Flags & FLAG_GLOWSPRITE) != 0;
        out.m_Draw.m_bZBias   = (p->m_Flags & FLAG_SPRITEBIAS) != 0;
        out.m_bNoZ            = (p->m_Flags & FLAG_SPRITE_NOZ) != 0;
        out.m_bReallyClose    = (p->m_Flags & FLAG_REALLYCLOSE) != 0;
        if (!out.m_bReallyClose)
        {
            for (int k = 0; k < 3; ++k) out.m_Pos[k] = p->m_Pos[k];
            return true;
        }
        // Camera space -> world, as for the view-weapon model.
        const float* c = p->m_Pos;
        for (int r = 0; r < 3; ++r)
            out.m_Pos[r] = m_Cam[r*4+0]*c[0] + m_Cam[r*4+1]*c[1] + m_Cam[r*4+2]*c[2] + m_Cam[r*4+3];
        return true;
    }

    unsigned int Count() const override
    {
        return (m_pServer ? m_pServer->Count() : 0u) + (unsigned)m_Client.size();
    }

    bool Get(unsigned int i, LTDynamicModel& out) const override
    {
        const unsigned int nS = m_pServer ? m_pServer->Count() : 0u;
        if (i < nS) return m_pServer->Get(i, out);
        i -= nS;
        if (i >= m_Client.size()) return false;
        const LTSceneObject* p = m_Client[i];
        // ⚠️ LT2'S PORTAL RULE (5df): the main view draws FLAG_VISIBLE; a
        // mirror draws FLAG_VISIBLE or FLAG_PORTALVISIBLE, never
        // FLAG2_PORTALINVISIBLE (the view weapon, the zipcord, PV FX).
        if (LTWorldPortalPass())
        {
            if (p->m_Flags2 & FLAG2_PORTALINVISIBLE) return false;
        }
        else if (!(p->m_Flags & FLAG_VISIBLE)) return false;

        out.sModel = p->m_sFilename;
        out.sSkins.clear();
        for (size_t k = 0; k < p->m_SkinNames.size(); ++k)
        { if (k) out.sSkins += ";"; out.sSkins += p->m_SkinNames[k]; }
        for (int k = 0; k < 3; ++k) out.m_Scale[k] = p->m_Scale[k] > 0.0f ? p->m_Scale[k] : 1.0f;
        out.m_nAnim     = p->m_nAnim;
        out.m_fAnimTime = LTGetClock()->GetTime() - p->m_fAnimStart;
        out.m_bLooping  = p->m_bLooping;
        out.m_bReallyClose = (p->m_Flags & FLAG_REALLYCLOSE) != 0;

        // ⚠️ ITS ALPHA AND BLEND MODE, as for a sprite (GetSprite below) --
        // SetObjectColor's alpha, FLAG2_ADDITIVE / FLAG2_MULTIPLY. Without
        // them the intel popup's backdrop (0.9 alpha, Multiply) was an
        // opaque grey sheet over the whole view, and the additive view-weapon
        // FX (lighter flame, welder torch) drew as solid shapes.
        if (LTDynamicBlendEnabled())
        {
            out.m_fAlpha   = p->m_Color[3];
            out.m_nBlendOp = (p->m_Flags2 & FLAG2_ADDITIVE) ? 1 : ((p->m_Flags2 & FLAG2_MULTIPLY) ? 2 : 0);
        }
        out.m_bNoLight = (p->m_Flags & FLAG_NOLIGHT) != 0;
        for (int k = 0; k < 3; ++k) out.m_Colour[k] = p->m_Color[k];

        if (!out.m_bReallyClose)
        {
            for (int k = 0; k < 3; ++k) out.m_Pos[k] = p->m_Pos[k];
            for (int k = 0; k < 4; ++k) out.m_Quat[k] = p->m_Quat[k];
            return true;
        }

        // Camera space -> world: W = Cam * Local.
        float L[16], W[16];
        LT3D::FromQuatPos(p->m_Quat, p->m_Pos, L);
        LT3D::Multiply(m_Cam, L, W);
        if (getenv("LT_PVTRACE"))
        {
            static int s_n = 0;
            if ((s_n++ % 120) == 0)
            {
                printf("[PV] %s local pos (%.2f %.2f %.2f) q (%.3f %.3f %.3f %.3f) scale (%.2f %.2f %.2f) anim %d\n",
                       p->m_sFilename.c_str(), p->m_Pos[0], p->m_Pos[1], p->m_Pos[2],
                       p->m_Quat[0], p->m_Quat[1], p->m_Quat[2], p->m_Quat[3],
                       p->m_Scale[0], p->m_Scale[1], p->m_Scale[2], p->m_nAnim);
                printf("[PV]   cam R (%.2f %.2f %.2f) U (%.2f %.2f %.2f) F (%.2f %.2f %.2f)  model Z in world (%.2f %.2f %.2f)\n",
                       m_Cam[0], m_Cam[4], m_Cam[8], m_Cam[1], m_Cam[5], m_Cam[9],
                       m_Cam[2], m_Cam[6], m_Cam[10], W[2], W[6], W[10]);
                fflush(stdout);
            }
        }
        out.m_Pos[0] = W[3]; out.m_Pos[1] = W[7]; out.m_Pos[2] = W[11];
        QuatFromMatrix(W, out.m_Quat);
        return true;
    }

    // Rotation part of a rigid row-major matrix -> (x y z w).
    static void QuatFromMatrix(const float* m, float* q)
    {
        const float tr = m[0] + m[5] + m[10];
        if (tr > 0.0f)
        {
            const float s = sqrtf(tr + 1.0f) * 2.0f;
            q[3] = 0.25f * s; q[0] = (m[9] - m[6]) / s; q[1] = (m[2] - m[8]) / s; q[2] = (m[4] - m[1]) / s;
        }
        else if (m[0] > m[5] && m[0] > m[10])
        {
            const float s = sqrtf(1.0f + m[0] - m[5] - m[10]) * 2.0f;
            q[3] = (m[9] - m[6]) / s; q[0] = 0.25f * s; q[1] = (m[1] + m[4]) / s; q[2] = (m[2] + m[8]) / s;
        }
        else if (m[5] > m[10])
        {
            const float s = sqrtf(1.0f + m[5] - m[0] - m[10]) * 2.0f;
            q[3] = (m[2] - m[8]) / s; q[0] = (m[1] + m[4]) / s; q[1] = 0.25f * s; q[2] = (m[6] + m[9]) / s;
        }
        else
        {
            const float s = sqrtf(1.0f + m[10] - m[0] - m[5]) * 2.0f;
            q[3] = (m[4] - m[1]) / s; q[0] = (m[2] + m[8]) / s; q[1] = (m[6] + m[9]) / s; q[2] = 0.25f * s;
        }
    }
};
static CLTSceneViewObjects s_ViewObjects;

// ⚠️ MOVING BRUSH GEOMETRY -- doors. Same reason as the dynamic objects above:
// the render layer cannot know the server exists, so the answer is installed.
// Null draws every brush where the level put it, which is what RenderCamera
// did before and why an opening door never moved on screen.
static const ILTWorldModelXform* s_pXform = 0;
void LTScene_SetModelXform(const ILTWorldModelXform* p) { s_pXform = p; }

// ⚠️ THE GPU CAMERA -- installed by the host when the game runs with Metal
// (the default with a window). A function pointer, so this module never links
// Metal: the engine test suites build it without the GPU frameworks.
static LTSceneGpuCameraFn s_pGpuCamera = 0;

// The camera RenderCamera last drew -- what the player sees (LT_AIMAT).
static bool  s_bHaveCam = false;
static float s_CamPos[3], s_CamQuat[4];
bool LTScene_GetLastCamera(float* pPos3, float* pQuat4)
{
    if (!s_bHaveCam) return false;
    for (int k = 0; k < 3; ++k) pPos3[k] = s_CamPos[k];
    for (int k = 0; k < 4; ++k) pQuat4[k] = s_CamQuat[k];
    return true;
}
void LTScene_SetGpuCamera(LTSceneGpuCameraFn fn) { s_pGpuCamera = fn; }
static LTSceneGpuSceneFn s_pGpuScene = 0;
void LTScene_SetGpuScene(LTSceneGpuSceneFn fn) { s_pGpuScene = fn; }
bool LTScene_GpuCameraActive() { return s_pGpuCamera != 0; }

static LTRESULT SC_RenderCamera(HLOCALOBJ hCamera)
{
    LTIMPL("ILTClient::RenderCamera");

    LTSceneObject* pCam = Obj(hCamera);
    if (!pCam) return LT_ERROR;

    CLTWorldScene* pWorld = LTGetWorldScene();
    if (!pWorld || !pWorld->IsLoaded())
    {
        static bool bSaid = false;
        if (!bSaid)
        {
            bSaid = true;
            fprintf(stderr, "[LT3D] RenderCamera: no world is loaded -- "
                            "nothing to draw. (LT_WORLD names one.)\n");
        }
        return LT_ERROR;
    }

    CLTNullRender* pR = LTGetNullRender();
    LT2DSurf dest = pR->GetView(pR->GetScreenSurface());
    if (!dest.m_pRGBA || dest.m_Width <= 0 || dest.m_Height <= 0) return LT_ERROR;

    // ⚠️ THE SAME BASIS AS RenderObjects, and it has to be. LTRotation's
    // identity faces down POSITIVE Z (ltrotation.h:42) while the projection
    // looks down negative Z, so forward is +Z rotated by the camera's
    // quaternion. Deriving it differently here is how the menu and the game
    // would end up disagreeing about which way the same camera points.
    float camM[16];
    LT3D::FromQuatPos(pCam->m_Quat, pCam->m_Pos, camM);

    const float fwd[3] = { camM[2], camM[6], camM[10] };
    float eye[3] = { pCam->m_Pos[0], pCam->m_Pos[1], pCam->m_Pos[2] };
    float at[3]  = { pCam->m_Pos[0] + fwd[0],
                     pCam->m_Pos[1] + fwd[1],
                     pCam->m_Pos[2] + fwd[2] };

    // ⚠️ LT_PEEK="ex,ey,ez,ax,ay,az" -- AN OBSERVER CAMERA, and labelled as
    // one. Overrides only what this call DRAWS, never the game's camera
    // object, so it changes no game state. It exists because a headless run
    // cannot turn its head: "is the character there?" needs a view of where
    // the character is, and the player spawns facing a wall.
    // ⚠️ LT_PEEK_CHARS=<n> -- VISIT EVERY CHARACTER, n frames each. Frames
    // the i-th drawable game-spawned model from in front of its own face, so
    // one headless run with LT_DUMP_EVERY=<n> yields a contact sheet of every
    // character in the level. "Is the woman at the counter drawn, textured and
    // upright?" is otherwise a question nobody can answer without a mouse.
    if (const char* pEvery = getenv("LT_PEEK_CHARS"))
    {
        static unsigned int s_nCall = 0;
        const unsigned int n = (unsigned)atoi(pEvery) > 0 ? (unsigned)atoi(pEvery) : 60;
        if (s_pDynamic)
        {
            std::vector<LTDynamicModel> v;
            for (unsigned int i = 0; i < s_pDynamic->Count(); ++i)
            { LTDynamicModel d; if (s_pDynamic->Get(i, d)) v.push_back(d); }
            if (!v.empty())
            {
                const LTDynamicModel& d = v[(s_nCall / n) % v.size()];
                float M[16];
                LT3D::FromQuatPos(d.m_Quat, d.m_Pos, M);
                const float f[3] = { M[2], M[6], M[10] };   // the model's forward
                for (int k = 0; k < 3; ++k)
                {
                    // LT_PEEK_DIST -- how far in front of the character to
                    // stand. Close is a DIFFERENT TEST: the near-plane clip
                    // only engages when geometry crosses it, so a camera that
                    // always stands 110 units away can never reproduce a
                    // close-up artefact.
                    static const float s_fDist = getenv("LT_PEEK_DIST")
                        ? (float)atof(getenv("LT_PEEK_DIST")) : 110.0f;
                    eye[k] = d.m_Pos[k] + f[k] * s_fDist;
                    at[k]  = d.m_Pos[k];
                }
                // ⚠️ HEAD HEIGHT, NOT BODY CENTRE. A character model's origin
                // is the middle of its collision box, so a camera aimed there
                // frames the torso and cuts the head off -- which made this
                // camera useless for the one question it kept being pointed
                // at ("does the FACE look right?"). A NOLF character is ~106
                // units tall, so the head is ~45 above the origin.
                // LT_PEEK_EYE overrides the aim height.
                static const float s_fEye = getenv("LT_PEEK_EYE")
                    ? (float)atof(getenv("LT_PEEK_EYE")) : 45.0f;
                eye[1] += s_fEye; at[1] += s_fEye;
                if (s_nCall % n == 0)
                    fprintf(stderr, "[PEEK] %u/%u %s at (%.0f %.0f %.0f)\n",
                            (unsigned)((s_nCall / n) % v.size()) + 1, (unsigned)v.size(),
                            d.sModel.c_str(), d.m_Pos[0], d.m_Pos[1], d.m_Pos[2]);
            }
        }
        ++s_nCall;
    }

    if (const char* pPeek = getenv("LT_PEEK"))
    {
        float v[6];
        if (sscanf(pPeek, "%f,%f,%f,%f,%f,%f", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) == 6)
        { for (int k = 0; k < 3; ++k) { eye[k] = v[k]; at[k] = v[3+k]; } }
    }

    // ⚠️ The world renderer takes a VERTICAL fov in radians. The game sets
    // both axes (SetCameraFOV), and 0 is what a camera reports before the game
    // has configured it -- rendering with a zero fov produces a degenerate
    // projection and a black frame, which reads as "the renderer is broken".
    const float fFovY = (pCam->m_fFovY > 0.0f) ? pCam->m_fFovY : 1.4f;

    // ⚠️ Depth is required -- CLTWorldScene::Render's sky pass reads it to
    // find the pixels the world never covered. Sized to the target and kept
    // across frames, because a per-frame allocation of a 640x480 float buffer
    // is 1.2 MB of churn for nothing.
    static std::vector<float> s_Depth;
    const size_t nPix = (size_t)dest.m_Width * (size_t)dest.m_Height;
    if (s_Depth.size() != nPix) s_Depth.assign(nPix, 0.0f);

    s_bHaveCam = true;
    for (int k = 0; k < 3; ++k) s_CamPos[k] = pCam->m_Pos[k];
    for (int k = 0; k < 4; ++k) s_CamQuat[k] = pCam->m_Quat[k];

    // The server's objects AND the client's own -- see CLTSceneViewObjects.
    s_ViewObjects.m_pServer = s_pDynamic;
    s_ViewObjects.Snapshot(pCam);
    const ILTWorldDynamicObjects* pViewObjs = &s_ViewObjects;

    const auto tR0 = std::chrono::steady_clock::now();

    // ⚠️ THE COMPOSITOR FIRST (HANDOVER 5bq): the 3D goes into the native
    // target at the camera's place in the output, and the 2D layers are left
    // OPEN over it (black 0, grey 128), exactly as the old path overwrote the
    // surface with the 3D. Everything the game draws afterwards blends onto
    // that, and the GPU recovers the blend at present. Falls through on failure.
    if (s_pGpuScene && LTComp_Active())
    {
        int nOW = 0, nOH = 0;
        LTComp_OutputSize(&nOW, &nOH);
        const int surf[4] = { 0, 0, dest.m_Width, dest.m_Height };
        int rect[4];
        LTComp_SceneRect(dest.m_Width, dest.m_Height, surf, rect);
        // ⚠️ THE GAME'S FRAMING STAYS INSIDE. The 3D fills an output of any
        // shape with the game's vertical field of view -- wider shows more at
        // the sides. NARROWER would cut the sides off, so there the horizontal
        // field is kept and the vertical one grows instead.
        float fFovScene = fFovY;
        {
            const float fSurf = (float)dest.m_Width / (float)(dest.m_Height > 0 ? dest.m_Height : 1);
            const float fOut  = (float)rect[2] / (float)(rect[3] > 0 ? rect[3] : 1);
            if (fOut > 0.0f && fOut < fSurf * 0.999f)
                fFovScene = 2.0f * atanf(tanf(fFovY * 0.5f) * fSurf / fOut);
        }
        float sc[3], add[3];
        LTScene_LightScaleFor(hCamera, sc, add);
        unsigned int nDyn = 0;
        if (s_pGpuScene(*pWorld, nOW, nOH, rect, LTComp_FirstCameraThisFrame(),
                        eye, at, fFovScene, s_pXform, pViewObjs, sc, add, &nDyn))
        {
            pR->OpenScreenForScene();
            LTComp_NoteCamera();
            g_fRenderCameraMs += std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - tR0).count();
            ++g_nRenderCameraCalls;
            g_nDynamicDrawn += nDyn;
            return LT_OK;
        }
        static bool s_bSaid = false;
        if (!s_bSaid)
        {
            s_bSaid = true;
            fprintf(stderr, "[COMP] scene frame failed -- old path for this frame\n");
        }
    }

    // ⚠️ METAL FIRST when installed: the GPU draws the same passes offscreen
    // and the pixels are read back INTO THIS SAME SURFACE, so everything the
    // game draws afterwards -- HUD, subtitles, menus -- composites on top
    // exactly as before. A failed GPU frame falls through to software.
    if (s_pGpuCamera)
    {
        unsigned int nDyn = 0;
        if (s_pGpuCamera(*pWorld, dest.m_pRGBA, dest.m_Width, dest.m_Height,
                         eye, at, fFovY, s_pXform, pViewObjs, &nDyn))
        {
            LTScene_ApplyLightScale(dest, hCamera);
            g_fRenderCameraMs += std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - tR0).count();
            ++g_nRenderCameraCalls;
            g_nDynamicDrawn += nDyn;
            return LT_OK;
        }
        static bool s_bSaid = false;
        if (!s_bSaid)
        {
            s_bSaid = true;
            fprintf(stderr, "[LT3D] Metal camera frame failed -- software renderer for this frame\n");
        }
    }

    CLTWorldScene::Stats st =
        pWorld->Render(dest.m_pRGBA, &s_Depth[0], dest.m_Width, dest.m_Height,
                       eye, at, fFovY, s_pXform, pViewObjs);
    LTScene_ApplyLightScale(dest, hCamera);
    g_fRenderCameraMs += std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - tR0).count();
    ++g_nRenderCameraCalls;

    // ⚠️ COUNTED, because "the characters are missing" was answered wrongly
    // once already by a census that measured a different renderer.
    g_nDynamicDrawn += (unsigned)st.m_nDynamic;

    return LT_OK;
}


// ----------------------------------------------------------------------- //
// The session calls -- see ltsession.h for why they matter more than they look.
// ----------------------------------------------------------------------- //

// ⚠️ StartGame DOES NOT NAME A WORLD, and that surprised me. In single player
// the request carries no `m_WorldName` (GameClientShell.cpp:6097 fills in only
// the type and two empty structs); which level to load is decided a few lines
// later, by a MID_LOAD_GAME message the client sends the SERVER. So this call
// starts a SESSION, not a level, and reading it as "load this world" leads to
// an engine that ignores the game's own choice of mission.
static LTRESULT SC_StartGame(StartGameRequest* pRequest)
{
    LTIMPL("ILTClient::StartGame");
    if (!pRequest) return LT_ERROR;

    LTSession_SetGameMode(pRequest->m_Type);
    LTSession_SetConnected(true);
    return LT_OK;
}

static LTRESULT SC_GetGameMode(int* pMode)
{
    // ⚠️ Written on every path. The game does `int nGameMode = 0;
    // GetGameMode(&nGameMode);` and then compares it -- an untouched output
    // reads as STARTGAME_HOST, which is a real mode and the wrong one.
    if (!pMode) return LT_ERROR;
    *pMode = LTSession_GetGameMode();
    return LT_OK;
}

static LTBOOL SC_IsConnected()
{
    return LTSession_IsConnected() ? LTTRUE : LTFALSE;
}

static uint32 SC_GetObjectClientFlags(HLOCALOBJ hObj)
{
    LTIMPL("ILTClient::GetObjectClientFlags");
    return (uint32)LTScene_GetClientFlags((void*)hObj);
}

static void SC_SetObjectClientFlags(HLOCALOBJ hObj, uint32 flags)
{
    LTIMPL("ILTClient::SetObjectClientFlags");
    LTScene_SetClientFlags((void*)hObj, (unsigned int)flags);
}

static HLOCALOBJ SC_GetClientObject()
{
    void* h = LTSession_GetClientObject();

    // ⚠️ CGameClientShell::UpdateWeaponModel returns immediately when this is
    // null (GameClientShell.cpp:3111), so a null here is silently the end of
    // the weapon -- no firing, no player-view model update, no diagnosis.
    if (getenv("LT_KEYTRACE"))
    {
        static int nLast = -1;
        const int  nNow  = h ? 1 : 0;
        if (nNow != nLast)
        {
            nLast = nNow;
            printf("[MKEY] the game asked for the client object -> %s\n",
                   h ? "ok" : "NULL");
            fflush(stdout);
        }
    }
    return (HLOCALOBJ)h;
}


int LTScene_SetObjectRotation(void* hObj, const float* pQuat4)
{
    LTSceneObject* p = Obj((HLOCALOBJ)hObj);
    if (!p || !pQuat4) return 0;
    for (int i = 0; i < 4; ++i) p->m_Quat[i] = pQuat4[i];
    return 1;
}

void* LTScene_CreatePlayerObject()
{
    LTSceneCreateInfo info;
    info.m_nType = OT_NORMAL;           // ⚠️ no model -- see ltscene.h
    info.m_Pos[0] = info.m_Pos[1] = info.m_Pos[2] = 0.0f;
    info.m_Quat[0] = info.m_Quat[1] = info.m_Quat[2] = 0.0f;
    info.m_Quat[3] = 1.0f;
    info.m_Scale[0] = info.m_Scale[1] = info.m_Scale[2] = 1.0f;
    info.m_Flags = info.m_Flags2 = 0;

    return (void*)g_Scene.CreateObject(info);
}


void LTScene_Wire(ILTClient* p)
{
    if (!p) return;

    p->CreateObject            = SC_CreateObject;
    p->GetObjectUserFlags      = SC_GetObjectUserFlags;
    p->GetObjectPos            = SC_GetObjectPos;
    p->GetObjectRotation       = SC_GetObjectRotation;
    p->SetObjectRotation       = SC_SetObjectRotation;
    p->SetObjectPosAndRotation = SC_SetObjectPosAndRotation;
    p->SetObjectScale          = SC_SetObjectScale;
    p->SetObjectColor          = SC_SetObjectColor;
    p->GetObjectColor          = SC_GetObjectColor;

    p->SetupPolyGrid           = SC_SetupPolyGrid;
    p->SetPolyGridTexture      = SC_SetPolyGridTexture;
    p->SetPolyGridEnvMap       = SC_SetPolyGridEnvMap;
    p->GetPolyGridTextureInfo  = SC_GetPolyGridTextureInfo;
    p->SetPolyGridTextureInfo  = SC_SetPolyGridTextureInfo;
    p->GetPolyGridInfo         = SC_GetPolyGridInfo;
    p->FitPolyGrid             = SC_FitPolyGrid;

    p->SetCameraRect           = SC_SetCameraRect;
    p->SetCameraFOV            = SC_SetCameraFOV;
    p->GetCameraFOV            = SC_GetCameraFOV;
    p->SetGlobalLightScale     = SC_SetGlobalLightScale;
    p->GetGlobalLightScale     = SC_GetGlobalLightScale;
    p->SetCameraLightAdd       = SC_SetCameraLightAdd;
    p->GetCameraLightAdd       = SC_GetCameraLightAdd;

    p->RenderObjects           = SC_RenderObjects;
    p->RenderCamera            = SC_RenderCamera;

    p->StartGame               = SC_StartGame;
    p->GetGameMode             = SC_GetGameMode;
    p->IsConnected             = SC_IsConnected;
    p->GetClientObject         = SC_GetClientObject;

    // ⚠️ FUNCTION POINTERS, not virtuals -- assigned, not overridden
    // (HANDOVER 4.5). Both were stubs, so every CF_NOTIFYMODELKEYS request the
    // game made was discarded and no animation key could ever be delivered.
    p->GetObjectClientFlags    = SC_GetObjectClientFlags;
    p->SetObjectClientFlags    = SC_SetObjectClientFlags;

    // Node control -- see LTScene_SetNodeControl.
    p->GetNextModelNode        = SC_GetNextModelNode;
    p->GetModelNodeName        = SC_GetModelNodeName;
}


bool LTScene_SetObjectFilenames(void* hObj, const char* pFilename,
                                const std::vector<std::string>& skins)
{
    return g_Scene.SetObjectFilenames((LTSceneObject*)hObj, pFilename, skins);
}

