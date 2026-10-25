// ----------------------------------------------------------------------- //
//
// MODULE  : ltworldrender.cpp
//
// See ltworldrender.h. Every rule enforced here was learned by drawing the
// thing and looking at it; the comments say which picture proved which.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltworldrender.h"
#include "ltworldcollide.h"
#include "ltmodeldraw.h"
#include "ltdtx.h"
#include "ltspr.h"
#include "ltfilemgr.h"

#include <math.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <algorithm>
#include <set>

namespace
{
    // WIF_ bits, from the donor's de_world.h:24.
    //
    // ⚠️ THE MODEL NAMES IN THE FILE ARE MISLEADING -- READ THE FLAGS. The
    // model called "PhysicsBSP" carries WIF_PHYSICSBSP | WIF_MAINWORLD and IS
    // the visible world; the one called "VisBSP" is a near-duplicate kept for
    // visibility work. Drawing both z-fights them, so the visibility one goes.
    const unsigned int kWIF_VISBSP    = (1u << 5);

    // ⚠️ THE MAIN WORLD, and it is the one model that must NOT be nudged
    // forward -- everything else is nudged relative to it. Measured on all 102
    // shipped worlds: each has EXACTLY ONE model carrying this bit, always
    // named PhysicsBSP, and no other model carries it. (19,371 ordinary
    // brushes are 0x02, 608 terrain meshes 0x08, 102 VisBSP 0x20.)
    const unsigned int kWIF_PHYSICSBSP = (1u << 2);

    // SURF_ bits, from de_world.h:59.
    const unsigned int kSURF_INVISIBLE = (1u << 2);
    // ⚠️ The LEVEL's own statement that a surface is see-through -- the world
    // equivalent of a prop's Chromakey, and ignored here until 2026-09-12.
    const unsigned int kSURF_TRANSPARENT = (1u << 3);
    const unsigned int kSURF_SKY       = (1u << 4);
    const unsigned int kSURF_BRIGHT    = (1u << 5);

    std::string LowerOf(const std::string& s)
    {
        std::string o(s);
        for (size_t i = 0; i < o.size(); ++i) o[i] = (char)tolower(o[i]);
        return o;
    }

    bool EndsWith(const std::string& s, const char* pSuffix)
    {
        size_t n = strlen(pSuffix);
        if (s.size() < n) return false;
        return LowerOf(s).compare(s.size() - n, n, pSuffix) == 0;
    }

    // Row-major with translation in column 3 -- the layout LT3D::Multiply and
    // TransformPoint already use.
    //
    // ⚠️ Yaw is Euler[1]. Not a guess: the same index on GameStartPoint's
    // rotation aims the camera down a corridor rather than into a wall.
    // ⚠️ pScale3 may be null, which means "no scaling" and produces exactly the
    // matrix this built before scaling existed.
    void WorldMatrix(const float* pos, const float* eul, float* out,
                     const float* pScale3 = 0)
    {
        float cy = cosf(eul[1]), sy = sinf(eul[1]);
        float cx = cosf(eul[0]), sx = sinf(eul[0]);
        float cz = cosf(eul[2]), sz = sinf(eul[2]);

        float m00 =  cy*cz + sy*sx*sz, m01 = -cy*sz + sy*sx*cz, m02 = sy*cx;
        float m10 =  cx*sz,            m11 =  cx*cz,            m12 = -sx;
        float m20 = -sy*cz + cy*sx*sz, m21 =  sy*sz + cy*sx*cz, m22 = cy*cx;

        LT3D::Identity(out);
        out[0] = m00; out[1] = m01; out[2]  = m02; out[3]  = pos[0];
        out[4] = m10; out[5] = m11; out[6]  = m12; out[7]  = pos[1];
        out[8] = m20; out[9] = m21; out[10] = m22; out[11] = pos[2];

        // ⚠️ SCALE GOES ON THE COLUMNS, i.e. it is applied to the model BEFORE
        // the rotation (out = T * R * S). Scaling the rows instead would scale
        // along world axes and shear anything that is rotated.
        if (pScale3)
            for (int c = 0; c < 3; ++c)
                for (int r = 0; r < 3; ++r) out[r*4 + c] *= pScale3[c];
    }
}


CLTWorldScene::CLTWorldScene()
    : m_pFileMgr(0), m_pSky(0), m_bLoaded(false), m_fFarZ(0.0f)
{
    // ⚠️ Front-to-back is ON. It is not opt-in like the sampler flags, and the
    // reason is that it is provably not a visual change: sorted and unsorted
    // output is BYTE-IDENTICAL across every test view, because reordering
    // opaque geometry under a depth buffer cannot change the result. What it
    // changes is how many pixels get shaded on the way there.
    m_bSort = (getenv("LT_NOSORT") == 0);
}


CLTWorldScene::~CLTWorldScene()
{
    for (std::map<std::string, CachedTex*>::iterator it = m_Textures.begin();
         it != m_Textures.end(); ++it) delete it->second;
    for (std::map<std::string, CachedModel*>::iterator it = m_Models.begin();
         it != m_Models.end(); ++it) delete it->second;
}


// ----------------------------------------------------------------------- //
//
// ⚠️ The RGBA must outlive every LT3DTexture that points into it, so the bytes
// live in the cache and never in a local. The store is a map of POINTERS
// rather than a map of values for the same reason: a container that
// reallocates would dangle every texture already handed out.
//
// ----------------------------------------------------------------------- //

namespace
{
    // ⚠️ DEFAULT ON since 2026-09-09, and the switch is now LT_NOMIRROR.
    //
    // Settled by the player's screenshots of the real Windows game, not by
    // argument. The decisive one is M01S01's 28/29 door corner: Windows puts
    // door 28 on the LEFT wall and 29 on the RIGHT, and un-mirrored we drew the
    // opposite. The menu agrees -- Windows has Cate on the RIGHT of the screen
    // with the menu panel on the LEFT.
    //
    // ⚠️ THE DOOR PLAQUES ARE READABLE EITHER WAY AND PROVE NOTHING, because
    // world U is toggled with this flag (see the U negation below). That is
    // why this took so long to settle: the most obvious evidence in the level
    // was the one piece that could not discriminate. Only the LAYOUT could.
    //
    // ⚠️ And it does NOT replace the model-space mirror in ltmodeldraw.cpp.
    // They fix different things and BOTH are on: this one flips the whole
    // IMAGE (which side of the screen a thing is on); that one fixes each
    // model's own handedness in WORLD space (which way a door lever points).
    // Rendering the 28/29 corner in all four combinations is what showed they
    // do not cancel.
    bool MirrorWorld() { return LTWorldMirror(); }

    // ⚠️ DOES THIS TEXTURE ACTUALLY CUT ANYTHING OUT?
    //
    // CLTDtx::AlphaIsMeaningful() answers "is alpha worth believing", and it
    // says yes for every COMPRESSED texture -- because DXT1 *can* encode
    // punch-through and DXT3/5 exist to carry alpha. That is the right rule for
    // deciding whether to read the channel at all, but it is a statement about
    // the FORMAT, not about this texture's contents, and NOLF's architecture is
    // almost entirely DXT1. So "alpha is meaningful" was true for nearly every
    // wall in the game while nearly none of them cut anything out.
    //
    // That distinction did not matter until filtering arrived, because a
    // texture with no transparent texel never fails the alpha test anyway --
    // the flag was true and inert. It matters now: bilinear is suppressed on
    // alpha-tested textures, and the broad flag suppressed it on the whole
    // level. Measured: 1.2% of pixels changed when filtering was switched on.
    //
    // So the flag is narrowed to what it is being asked, by looking. This
    // CANNOT change what the alpha test does -- a texture with no texel below
    // the threshold discards no pixel either way -- it only stops the test
    // claiming a cut-out that is not there.
    // ⚠️ BOTH SIDES MUST BE PRESENT, and asking only about one erases surfaces.
    //
    // This used to be HasCutout(): "is there ANY transparent texel". A texture
    // that is transparent EVERYWHERE passes that and is then punched away
    // entirely -- the prop becomes invisible. `Rock_01`, `_01b` and `_01c`
    // (12 placements) are exactly that: alpha 0 in every texel, so a rock
    // vanished.
    //
    // ⚠️ That guard existed before and I removed it. CLTDtx::AlphaDiscriminates
    // asks the right question -- "are there both transparent and opaque
    // texels" -- and its whole purpose was this case. Replacing the alpha RULE
    // was correct; dropping the guard that came with it was not.
    //
    // ⚠️ Tested at the SAME threshold the rasteriser will use, not a fixed
    // half. A texture whose alpha straddles 128 but sits entirely below an
    // authored `alpharef 96` still erases itself, and a guard that disagreed
    // with the test it is guarding would let exactly that through.
    // Is EVERY texel below the threshold? See LT3DTexture::m_bFullyTransparent.
    bool FullyTransparentAt(const std::vector<unsigned char>& rgba, float fRef)
    {
        if (rgba.size() < 4) return false;
        const unsigned char nRef = (unsigned char)(fRef * 255.0f + 0.5f);
        for (size_t i = 3; i < rgba.size(); i += 4)
            if (rgba[i] >= nRef) return false;
        return true;
    }

    // ⚠️ DOES THIS ALPHA CHANNEL SAY ANYTHING AT ALL? Not "is it transparent
    // somewhere" -- whether it VARIES. A channel that is one value everywhere
    // carries no per-texel information, and there are two of those in the
    // shipped data, both of which matter:
    //
    //   all 0     `SFX\Beams\Skins\SpotlightBeam.dtx`, 98 placements. Blending
    //             by the texel would draw an invisible light beam.
    //   all 255   `Props\Skins\Tree_03t.dtx`, a tree trunk. Reading it is
    //             harmless but saying so is what keeps the two cases together.
    //
    // In both, the object's own `Alpha` is the only statement there is.
    // Does sName contain any of the ','-separated substrings in pList?
    bool NameMatchesList(const std::string& sName, const char* pList)
    {
        std::string sList(pList);
        size_t a = 0;
        while (a <= sList.size())
        {
            size_t b = sList.find(',', a);
            if (b == std::string::npos) b = sList.size();
            const std::string one = sList.substr(a, b - a);
            a = b + 1;
            if (!one.empty() && sName.find(one) != std::string::npos) return true;
        }
        return false;
    }

    bool AlphaVaries(const std::vector<unsigned char>& rgba)
    {
        if (rgba.size() < 8) return false;
        const unsigned char n0 = rgba[3];
        for (size_t i = 7; i < rgba.size(); i += 4)
            if (rgba[i] != n0) return true;
        return false;
    }

    bool DiscriminatesAt(const std::vector<unsigned char>& rgba, float fRef)
    {
        const unsigned char nRef = (unsigned char)(fRef * 255.0f + 0.5f);
        bool bAnyBelow = false, bAnyAbove = false;
        for (size_t i = 3; i < rgba.size(); i += 4)
        {
            if (rgba[i] < nRef) bAnyBelow = true;
            else                bAnyAbove = true;
            if (bAnyBelow && bAnyAbove) return true;
        }
        return false;
    }
}


// ----------------------------------------------------------------------- //
//
// ResolveObjectModel
//
// See the header. For a PropType the attribute file wins outright, exactly as
// PropType::ReadProp does; for everything else the world's own properties are
// the answer.
//
// ----------------------------------------------------------------------- //

void CLTWorldScene::ObjectScale(size_t i, float* pScale3) const
{
    pScale3[0] = pScale3[1] = pScale3[2] = 1.0f;
    const std::vector<LTWorldObject>& obj = m_World.GetObjects();
    if (i >= obj.size()) return;
    if (const LTWorldProperty* pSc = obj[i].Find("Scale"))
        for (int k = 0; k < 3; ++k)
            if (pSc->m_Vec[k] > 0.0f) pScale3[k] = pSc->m_Vec[k];
    // PropType.cpp:171 -- "Only use the prop type scale if the scale wasn't
    // set in DEdit".
    if (obj[i].m_sType == "PropType" && pScale3[0] == 1.0f && pScale3[1] == 1.0f && pScale3[2] == 1.0f)
        if (const LTWorldProperty* pT = obj[i].Find("Type"))
            if (const CLTPropTypes::Entry* pE = m_PropTypes.Find(pT->m_sString))
                for (int k = 0; k < 3; ++k)
                    if (pE->m_Scale[k] > 0.0f) pScale3[k] = pE->m_Scale[k];
}

bool CLTWorldScene::ObjectMoveToFloor(size_t i) const
{
    const std::vector<LTWorldObject>& obj = m_World.GetObjects();
    if (i >= obj.size()) return false;
    // ⚠️ A PROPTYPE'S IS ITS TYPE'S, unconditionally (PropType.cpp:167) -- the
    // world file's MoveToFloor is a hidden default. Reading the world file
    // left every PropType where the editor dropped it: M02S01's trees with
    // their trunks 183 units above the street, M02S02's flour bag in mid-air.
    if (obj[i].m_sType == "PropType")
    {
        if (const LTWorldProperty* pT = obj[i].Find("Type"))
            if (const CLTPropTypes::Entry* pE = m_PropTypes.Find(pT->m_sString))
                return pE->m_bMoveToFloor;
        return false;
    }
    const LTWorldProperty* pM = obj[i].Find("MoveToFloor");
    return pM && pM->m_bBool;
}

const LTModelPose* CLTWorldScene::PropRestPose(CachedModel* pM) const
{
    if (!pM || LTPropTurn180() || LTPropOldPlacement()) return 0;
    if (pM->m_nRestPose < 0)
    {
        pM->m_nRestPose = 0;
        const std::vector<LTABCAnim>& an = pM->m_Model.GetAnims();
        unsigned int nAnim = 0;
        for (size_t a = 0; a < an.size(); ++a)
            if (strcasecmp(an[a].m_sName.c_str(), "World") == 0) { nAnim = (unsigned)a; break; }
        pM->m_nRestAnim = (int)nAnim;
        if (!an.empty() && LTModelDraw::BuildPose(pM->m_Model, nAnim, 0, pM->m_RestPose))
            pM->m_nRestPose = 1;
    }
    return pM->m_nRestPose == 1 ? &pM->m_RestPose : 0;
}

static LTWorldLiveAnimFn s_pfnLiveAnim = 0;
void LTWorldScene_SetLiveAnimQuery(LTWorldLiveAnimFn fn) { s_pfnLiveAnim = fn; }

const LTModelPose* CLTWorldScene::PropPose(size_t i, CachedModel* pM) const
{
    const LTModelPose* pRest = PropRestPose(pM);   // also settles m_nRestAnim
    if (!pRest || !s_pfnLiveAnim) return pRest;
    static const bool s_bOff = []() {
        const bool b = (getenv("LT_NOPROPANIM") != 0);
        if (b) { printf("[LT3D] LT_NOPROPANIM=1 -- level props drawn in their rest pose (old)\n"); fflush(stdout); }
        return b; }();
    if (s_bOff) return pRest;

    int nAnim = -1; float fSec = 0.0f; bool bLoop = true;
    if (!s_pfnLiveAnim((unsigned int)i, &nAnim, &fSec, &bLoop)) return pRest;

    // ⚠️ "NONE CHOSEN" IS THE TRACKER'S DEFAULT, not a still: the donor's
    // trk_Init starts every model on an animation, playing and looping. The
    // rest pose's choice (`World`, else 0) is Prop.cpp's and that default.
    const std::vector<LTABCAnim>& an = pM->m_Model.GetAnims();
    if (nAnim < 0) nAnim = pM->m_nRestAnim;
    if ((size_t)nAnim >= an.size() || an[(size_t)nAnim].m_Keyframes.empty()) return pRest;

    unsigned int k0 = 0, k1 = 0; float f = 0.0f;
    if (an[(size_t)nAnim].m_Keyframes.size() >= 2)
        LTModelDraw::KeyframeSpan(pM->m_Model, (unsigned int)nAnim, fSec, bLoop, &k0, &k1, &f);
    if (nAnim == pM->m_nRestAnim && k0 == 0 && f == 0.0f) return pRest;   // still: as before

    if (pM->m_nAnimPoseAnim != nAnim || pM->m_nAnimPoseK0 != k0
        || pM->m_nAnimPoseK1 != k1 || pM->m_fAnimPoseFrac != f)
    {
        if (!LTModelDraw::BuildPoseBetween(pM->m_Model, (unsigned int)nAnim, k0, k1, f, pM->m_AnimPose))
            return pRest;
        pM->m_nAnimPoseAnim = nAnim;
        pM->m_nAnimPoseK0 = k0; pM->m_nAnimPoseK1 = k1; pM->m_fAnimPoseFrac = f;
    }
    return &pM->m_AnimPose;
}

bool CLTWorldScene::ResolveObjectModel(size_t i, std::string& sModel,
                                       std::string& sSkin,
                                       bool* pbChromakey,
                                       float* pfAlpha,
                                       int* pnBlendOp) const
{
    const std::vector<LTWorldObject>& obj = m_World.GetObjects();
    if (i >= obj.size()) return false;

    sModel.clear();
    sSkin.clear();
    if (pbChromakey) *pbChromakey = false;
    if (pfAlpha)     *pfAlpha     = 1.0f;
    if (pnBlendOp)   *pnBlendOp   = 0;

    if (obj[i].m_sType == "PropType")
    {
        const LTWorldProperty* pT = obj[i].Find("Type");
        if (pT && !pT->m_sString.empty())
        {
            if (const CLTPropTypes::Entry* pE = m_PropTypes.Find(pT->m_sString))
            {
                if (!pE->m_sFilename.empty())
                {
                    sModel = pE->m_sFilename;
                    sSkin  = pE->m_sSkin;

                    // ⚠️ THE ATTRIBUTE FILE WINS, exactly as it does for the
                    // Filename and Skin above. The world object carries its
                    // own `Chromakey` and it is 0 on the cattail, whose prop
                    // TYPE says 1 -- so reading the object's copy would make
                    // every cut-out prop solid.
                    // ⚠️ Chromakey OR a translucent Alpha -- see
                    // CLTPropTypes::Entry::UsesTextureAlpha. The long-grass
                    // props say it the second way.
                    if (pbChromakey) *pbChromakey = pE->UsesTextureAlpha();

                    // ⚠️ THE ATTRIBUTE FILE WINS HERE TOO, and the game's own
                    // source says so in one line: PropType::ObjectCreated does
                    // `m_fAlpha = pPropType->fAlpha` unconditionally, and the
                    // object's own `Alpha` is declared PF_HIDDEN beside the
                    // Filename and Skin that are already known to be editor
                    // bookkeeping. 200 PropType placements carry an Alpha
                    // below 1.0 in their world files and every one of them is
                    // ignored by the game.
                    if (LTModelBlendEnabled())
                    {
                        if (pfAlpha)   *pfAlpha   = pE->m_fAlpha;
                        if (pnBlendOp) *pnBlendOp = pE->BlendOp();
                    }
                    return EndsWith(sModel, ".abc");
                }
            }
        }
    }

    const LTWorldProperty* pF = obj[i].Find("Filename");
    if (!pF || !EndsWith(pF->m_sString, ".abc")) return false;
    sModel = pF->m_sString;

    const LTWorldProperty* pS = obj[i].Find("Skin");
    if (pS) sSkin = pS->m_sString;

    // A non-PropType prop has no attribute-file entry, so its own property is
    // the only statement there is.
    if (pbChromakey)
    {
        const LTWorldProperty* pC = obj[i].Find("Chromakey");
        *pbChromakey = pC ? pC->m_bBool : false;
    }

    // ⚠️ And its own `Alpha` IS the statement for a plain Prop -- Prop.cpp
    // reads the property directly. 178 of them are below 1.0 across the 102
    // shipped worlds, 98 of those the spotlight beams at 0.25 and 0.50.
    if (pfAlpha)
    {
        const LTWorldProperty* pA = obj[i].Find("Alpha");
        if (pA && pA->m_fReal >= 0.0f && pA->m_fReal <= 1.0f
            && LTModelBlendEnabled()) *pfAlpha = pA->m_fReal;
    }

    // ⚠️ Prop.cpp reads these as ordinary bool properties and turns them into
    // FLAG2_ADDITIVE / FLAG2_MULTIPLY. 102 light-beam props say Additive and
    // 9 motorcycles say Multiply.
    if (pnBlendOp && LTModelBlendEnabled())
    {
        const LTWorldProperty* pAd = obj[i].Find("Additive");
        const LTWorldProperty* pMu = obj[i].Find("Multiply");
        if (pAd && pAd->m_bBool)      *pnBlendOp = 1;
        else if (pMu && pMu->m_bBool) *pnBlendOp = 2;
    }
    return true;
}


bool LTWorldMirror()
{
    static const bool b = (getenv("LT_NOMIRROR") == 0);
    return b;
}


// ⚠️ AN A/B SWITCH FOR THE MODEL BLEND, and it exists because the alternative
// is comparing against a binary that no longer exists. LT_NOMODELBLEND=1 draws
// every translucent prop the way this port drew it before pass 5: solid, with
// the punch rule alone. Read once, so a process is internally consistent and
// the model cache cannot be poisoned by a mid-run change.
bool LTModelBlendEnabled()
{
    static const bool b = (getenv("LT_NOMODELBLEND") == 0);
    return b;
}


bool LTDynamicBlendEnabled()
{
    static const bool b = []() {
        const bool bOff = (getenv("LT_NODYNBLEND") != 0);
        if (bOff) { printf("[LT3D] LT_NODYNBLEND=1 -- game-spawned models drawn opaque, alpha and blend ignored\n"); fflush(stdout); }
        return !bOff; }();
    return b;
}


std::vector<unsigned int> LTTranslucentDynamics(const ILTWorldDynamicObjects* pDyn,
                                                bool bReallyClose, const float* pEye3)
{
    std::vector<std::pair<float, unsigned int> > v;
    const unsigned int n = pDyn ? pDyn->Count() : 0u;
    for (unsigned int i = 0; i < n; ++i)
    {
        LTDynamicModel d;
        if (!pDyn->Get(i, d) || d.m_bReallyClose != bReallyClose) continue;
        if (!LTIsTranslucentDraw(d.m_fAlpha, d.m_nBlendOp)) continue;
        const float dx = d.m_Pos[0] - pEye3[0], dy = d.m_Pos[1] - pEye3[1], dz = d.m_Pos[2] - pEye3[2];
        v.push_back(std::make_pair(-(dx*dx + dy*dy + dz*dz), i));
    }
    std::stable_sort(v.begin(), v.end(),
                     [](const std::pair<float, unsigned int>& a,
                        const std::pair<float, unsigned int>& b) { return a.first < b.first; });
    std::vector<unsigned int> out;
    out.reserve(v.size());
    for (size_t k = 0; k < v.size(); ++k) out.push_back(v[k].second);
    return out;
}


std::vector<unsigned int> LTPolyGridsBackToFront(const ILTWorldDynamicObjects* pDyn,
                                                 const float* pEye3)
{
    std::vector<std::pair<float, unsigned int> > v;
    const unsigned int n = (pDyn && LTPolyGridEnabled()) ? pDyn->PolyGridCount() : 0u;
    for (unsigned int i = 0; i < n; ++i)
    {
        LTDynamicPolyGrid g;
        if (!pDyn->GetPolyGrid(i, g)) continue;
        const float dx = g.m_Pos[0] - pEye3[0], dy = g.m_Pos[1] - pEye3[1], dz = g.m_Pos[2] - pEye3[2];
        v.push_back(std::make_pair(-(dx*dx + dy*dy + dz*dz), i));
    }
    std::stable_sort(v.begin(), v.end(),
                     [](const std::pair<float, unsigned int>& a,
                        const std::pair<float, unsigned int>& b) { return a.first < b.first; });
    std::vector<unsigned int> out;
    for (size_t k = 0; k < v.size(); ++k) out.push_back(v[k].second);
    return out;
}


bool LTPolyGridEnabled()
{
    static const bool b = []() {
        const bool bOff = (getenv("LT_NOPOLYGRID") != 0);
        if (bOff) { printf("[LT3D] LT_NOPOLYGRID=1 -- no PolyGrids: OT_POLYGRID refused, SFX_POLYGRID_ID gated (old)\n"); fflush(stdout); }
        return !bOff; }();
    return b;
}


bool LTNoLightEnabled()
{
    static const bool b = []() {
        const bool bOff = (getenv("LT_LITNOLIGHT") != 0);
        if (bOff) { printf("[LT3D] LT_LITNOLIGHT=1 -- FLAG_NOLIGHT models lit by the level (old)\n"); fflush(stdout); }
        return !bOff; }();
    return b;
}


float LTReallyCloseNear()
{
    static const float f = []() {
        const bool bOld = (getenv("LT_RCNEAR25") != 0);
        if (bOld) { printf("[LT3D] LT_RCNEAR25=1 -- really-close near plane 0.25 (old), not the donor's 0.1\n"); fflush(stdout); }
        return bOld ? 0.25f : 0.1f; }();
    return f;
}


bool LTSpritesEnabled()
{
    static const bool b = (getenv("LT_NOSPRITES") == 0);
    return b;
}


// ⚠️ LT_NOSKY=1 skips the sky PASS while leaving the sky model excluded from
// the world pass, so the frame is the level and nothing else. It is a
// diagnostic, not a feature: when the two backends disagree over a large flat
// area, "is that surface, or is it the sky showing through it?" is the first
// question and there was no way to ask it.
bool LTFogRange()
{
    static const bool b = []() {
        const bool bDepth = (getenv("LT_FOGDEPTH") != 0);
        if (bDepth) { printf("[LT3D] LT_FOGDEPTH=1 -- fog by view depth (old), not by range\n"); fflush(stdout); }
        return !bDepth; }();
    return b;
}

bool LTSkyEnabled()
{
    static const bool b = (getenv("LT_NOSKY") == 0);
    return b;
}


// ⚠️ LT_NOOBJECTS=1 draws the level and no props. "World or objects?" is the
// first bisection for any difference that covers a large area, and until this
// existed there was no way to ask it without a rebuild.
bool LTObjectsEnabled()
{
    static const bool b = (getenv("LT_NOOBJECTS") == 0);
    return b;
}


bool LTModelDepthBiasEnabled()
{
    static const bool b = (getenv("LT_NOMODELBIAS") == 0);
    return b;
}


bool LTIsMainWorldBSP(const LTWorldModelHeader& m)
{
    return (m.m_nInfoFlags & kWIF_PHYSICSBSP) != 0;
}


void LTBuildModelDepthBias(const float* pEye3, float* pOut16)
{
    LT3D::Identity(pOut16);
    if (!LTModelDepthBiasEnabled()) return;

    // ⚠️ A POWER OF TWO, so the factor is exact in binary and the two backends
    // multiply by the identical number rather than by two roundings of 0.9999.
    const float s = 1.0f - 1.0f / 8192.0f;

    // p' = s*p + (1-s)*eye, which is the scale-about-the-eye written out.
    pOut16[0]  = s;
    pOut16[5]  = s;
    pOut16[10] = s;
    pOut16[3]  = (1.0f - s) * pEye3[0];
    pOut16[7]  = (1.0f - s) * pEye3[1];
    pOut16[11] = (1.0f - s) * pEye3[2];
}


// ----------------------------------------------------------------------- //
//
// ResolveObjectSprite
//
// ⚠️ EVERY RULE HERE IS `ScaleSprite::ReadProp` AND `PostPropRead`, read from
// the game's own source rather than guessed. The defaults matter as much as
// the values: `ScaleX`/`ScaleY` default to 0.5 and `Additive` defaults to
// TRUE, so an object that omits them is a HALF-SIZE ADDITIVE sprite, not a
// full-size ordinary one.
//
// ----------------------------------------------------------------------- //

static LTWorldLiveFn s_pfnLive = 0;
void LTWorldScene_SetLiveQuery(LTWorldLiveFn fn) { s_pfnLive = fn; }

static LTWorldLivePoseFn s_pfnLivePose = 0;
void LTWorldScene_SetLivePoseQuery(LTWorldLivePoseFn fn) { s_pfnLivePose = fn; }
bool LTWorldScene_LivePose(size_t i, float* pPos3, float* pQuat4)
{
    static const bool s_bOff = []() {
        const bool b = (getenv("LT_FILEPLACEMENT") != 0);
        if (b) { printf("[LT3D] LT_FILEPLACEMENT=1 -- level props placed from the world file, not the game\n"); fflush(stdout); }
        return b; }();
    if (s_bOff || !s_pfnLivePose) return false;
    if (!s_pfnLivePose((unsigned int)i, pPos3, pQuat4)) return false;
    // A degenerate rotation is not a placement.
    const float l = pQuat4[0]*pQuat4[0] + pQuat4[1]*pQuat4[1] + pQuat4[2]*pQuat4[2] + pQuat4[3]*pQuat4[3];
    if (!(l > 0.5f && l < 2.0f)) return false;
    const float n = 1.0f / sqrtf(l);
    for (int k = 0; k < 4; ++k) pQuat4[k] *= n;
    return true;
}

// -1 = use the world file; 0 = do not draw; 1 = draw. LT_FILEVISIBILITY=1
// ignores the live state (says so). LT_LIVETRACE=1 lists, once each, the
// world objects the live state hides.
static int LiveState(size_t i, const LTWorldObject& o)
{
    static const bool s_bOff = []() {
        const bool b = (getenv("LT_FILEVISIBILITY") != 0);
        if (b) { printf("[LT3D] LT_FILEVISIBILITY=1 -- world-file objects drawn from the file, not their live state\n"); fflush(stdout); }
        return b; }();
    if (s_bOff || !s_pfnLive) return -1;
    const int n = s_pfnLive((unsigned int)i);
    if (n == 0 && getenv("LT_LIVETRACE"))
    {
        static std::set<size_t> s_Said;
        if (s_Said.insert(i).second)
        {
            printf("[LIVE] hidden at run time: %-24s %s\n", o.m_sType.c_str(), o.GetName());
            fflush(stdout);
        }
    }
    return n;
}

bool CLTWorldScene::HiddenNow(size_t i) const
{
    const std::vector<LTWorldObject>& obj = m_World.GetObjects();
    // 2 = the live pass draws this object with the model the game chose
    // (CLTServerObjects::DrawnLive): the prop pass must not draw the file's.
    if (i >= obj.size()) return false;
    const int n = LiveState(i, obj[i]);
    return n == 0 || n == 2;
}

bool CLTWorldScene::ResolveObjectSprite(size_t i, LTSpriteDraw& out) const
{
    if (!LTSpritesEnabled()) return false;

    const std::vector<LTWorldObject>& obj = m_World.GetObjects();
    if (i >= obj.size()) return false;

    const LTWorldProperty* pF = obj[i].Find("Filename");
    if (!pF || !EndsWith(pF->m_sString, ".spr")) return false;

    // ⚠️ TWO SEPARATE WAYS OF BEING OFF, and the sprite class uses the second.
    // `Visible` is the engine flag every object has; `StartOn` is ScaleSprite's
    // own, and its Update() is what sets FLAG_VISIBLE when it is true. 82
    // placements are StartOn false and belong to triggers.
    // ⚠️ AND THE LIVE STATE WINS when there is one -- ScaleSprite's on/off
    // messages set FLAG_VISIBLE on the server object (LiveState above).
    const int nLive = LiveState(i, obj[i]);
    if (nLive == 0) return false;
    if (nLive < 0)
    {
        const LTWorldProperty* pV = obj[i].Find("Visible");
        if (pV && !pV->m_bBool) return false;
        const LTWorldProperty* pOn = obj[i].Find("StartOn");
        if (pOn && !pOn->m_bBool) return false;
    }

    out = LTSpriteDraw();
    out.m_sSprite = pF->m_sString;

    if (const LTWorldProperty* p = obj[i].Find("ScaleX")) out.m_fScaleX = p->m_fReal;
    if (const LTWorldProperty* p = obj[i].Find("ScaleY")) out.m_fScaleY = p->m_fReal;

    // ⚠️ `Color` is 0..255 and the class divides by 255 before handing it to
    // SetObjectColor. Using it raw would multiply every sprite by 255.
    if (const LTWorldProperty* p = obj[i].Find("Color"))
        for (int k = 0; k < 3; ++k) out.m_Colour[k] = p->m_Vec[k] / 255.0f;

    if (const LTWorldProperty* p = obj[i].Find("Alpha")) out.m_fAlpha = p->m_fReal;

    // ⚠️ DEFAULT TRUE. `ADD_BOOLPROP_FLAG(Additive, LTTRUE, PF_GROUP2)` -- an
    // object with no stored value is additive. Every shipped placement does
    // store one, but the default is what a hand-built test world would get.
    out.m_nBlendOp = 1;
    if (const LTWorldProperty* p = obj[i].Find("Additive"))
        out.m_nBlendOp = p->m_bBool ? 1 : 0;
    if (const LTWorldProperty* p = obj[i].Find("Multiply"))
        if (p->m_bBool && out.m_nBlendOp != 1) out.m_nBlendOp = 2;

    out.m_bGlow  = true;
    out.m_bZBias = true;
    if (const LTWorldProperty* p = obj[i].Find("Glow"))  out.m_bGlow  = p->m_bBool;
    if (const LTWorldProperty* p = obj[i].Find("ZBias")) out.m_bZBias = p->m_bBool;

    if (!(out.m_fScaleX > 0.0f)) out.m_fScaleX = 0.5f;
    if (!(out.m_fScaleY > 0.0f)) out.m_fScaleY = 0.5f;
    if (!(out.m_fAlpha >= 0.0f && out.m_fAlpha <= 1.0f)) out.m_fAlpha = 1.0f;
    return true;
}


// ⚠️ WORLD-FILE PROPS: NO MODEL MIRROR, AND A 180-DEGREE TURN ABOUT THEIR OWN
// Y (default since 2026-10-02, HANDOVER 5ay). Measured against the Windows
// build by the user, two props pin it down:
//
//     placement                    phone (front/back)   door lever (left/right)
//     old: R * mirrorX             WRONG                right
//     yaw+180, mirrorX = R*mirrorZ right                WRONG
//     R * turnY180, no mirror      right                right
//
// The phone needed only its model Z flipped relative to the old placement and
// the lever needed its model X kept: the one transform that does both is a
// proper rotation -- no reflection at all, as for characters. So the mirror
// that props kept in 5as was compensating for this missing turn.
//
// The turn is in MODEL space (R * turnY180), not added to the yaw: for the 7%
// of placements that also carry pitch or roll (security cameras, spotlight
// beams) the two differ, and the model's own axis is what is turned around.
//
// LT_PROPMIRROR=1 restores the old placement (mirror, no turn).
// LT_PROPYAW=<degrees> is still an extra yaw knob, 0 by default.
bool LTPropOldPlacement()
{
    static const bool b = []() -> bool {
        const bool bOld = (getenv("LT_PROPMIRROR") != 0);
        if (bOld) { printf("[WORLD] LT_PROPMIRROR=1 -- props mirrored in X, not turned (old placement)\n"); fflush(stdout); }
        return bOld;
    }();
    return b;
}

int LTPropMirrorModel()
{
    return LTPropOldPlacement() ? (LTModelDraw::MirrorX() ? 1 : 0) : 0;
}

// ⚠️ THE TURN IS RETIRED (HANDOVER 5bt) -- it was a model-specific fact
// applied to every prop. LithTech draws each model through its animation's
// pose, and the census over the 219 PropTypes.txt models says what that pose
// does: 165 come out exactly as stored, 48 (every phone and door knob) come out
// turned 180 degrees about Y, 8 animated ones differently again. 5ay's two
// references were both from the 48, so a global turn fixed them and turned the
// other 165 -- chairs faced away from their tables -- backwards. Props are now
// drawn through PropRestPose and this applies only under LT_PROPTURN180=1,
// which restores 5ay's placement entirely (says so).
bool LTPropTurn180()
{
    static const bool b = []() -> bool {
        const bool bOn = (getenv("LT_PROPTURN180") != 0);
        if (bOn) { printf("[WORLD] LT_PROPTURN180=1 -- props turned 180 about Y, no pose, origin offset subtracted (5ay)\n"); fflush(stdout); }
        return bOn;
    }();
    return b;
}

// ⚠️ THE ORIGIN OFFSET IS ADDED, the donor's rule (transformmaker.cpp:183:
// `outVec += translation` on the root node): the whole mesh moves by the
// animation binding's translation in MODEL space. This port subtracted it,
// which the 5ay turn happened to undo for the door knobs (turn * -ofs == +ofs
// in the turned frame) and which put the STASI guards' caps 6-12 units above
// their heads. +1, or -1 under LT_PROPTURN180 / LT_OFFSETSUBTRACT=1 (says so).
float LTOriginOffsetSign()
{
    static const float f = []() -> float {
        if (LTPropTurn180()) return -1.0f;
        const bool b = (getenv("LT_OFFSETSUBTRACT") != 0);
        if (b) { printf("[WORLD] LT_OFFSETSUBTRACT=1 -- model origin offsets subtracted (old sign)\n"); fflush(stdout); }
        return b ? -1.0f : 1.0f;
    }();
    return f;
}

void LTApplyPropTurn(float* pM16)
{
    if (LTPropOldPlacement() || !LTPropTurn180()) return;
    // M * diag(-1, 1, -1): negate the model X and Z columns.
    for (int r = 0; r < 3; ++r) { pM16[r*4+0] = -pM16[r*4+0]; pM16[r*4+2] = -pM16[r*4+2]; }
}

float LTPropYawOffset()
{
    static const float f = []() -> float {
        const char* p = getenv("LT_PROPYAW");
        if (!p || !*p) return 0.0f;
        printf("[WORLD] LT_PROPYAW=%s -- extra yaw on world props\n", p);
        fflush(stdout);
        return (float)atof(p) * 3.14159265358979f / 180.0f;
    }();
    return f;
}


void CLTWorldScene::PreloadAll(int* pnTextures, int* pnModels)
{
    int nTex = 0, nMdl = 0;

    // Architecture: every texture every parsed world model names.
    const std::vector<LTWorldModelHeader>& mdl = m_World.GetModels();
    for (size_t i = 0; i < mdl.size(); ++i)
        for (size_t k = 0; k < mdl[i].m_Textures.size(); ++k)
            if (!mdl[i].m_Textures[k].empty())
            { GetTexture(mdl[i].m_Textures[k]); ++nTex; }

    // Props: every model and skin list, resolved the same way the object pass
    // resolves them -- including the PropType attribute-file override, or this
    // would warm the wrong cache entries.
    const std::vector<LTWorldObject>& obj = m_World.GetObjects();
    for (size_t i = 0; i < obj.size(); ++i)
    {
        std::string sModel, sSkin;
        bool bCK = false;
        if (!ResolveObjectModel(i, sModel, sSkin, &bCK)) continue;

        const LTWorldProperty* pV = obj[i].Find("Visible");
        if (pV && !pV->m_bBool) continue;        // never drawn, never needed

        if (GetModel(sModel, sSkin, bCK)) ++nMdl;
    }

    if (pnTextures) *pnTextures = nTex;
    if (pnModels)   *pnModels   = nMdl;
}


const LT3DTexture* CLTWorldScene::GetTexture(const std::string& sName, bool bBlend)
{
    // ⚠️ The treatment is part of the KEY -- see the header. The name still
    // drives every load below; only the cache slot differs.
    const std::string sKey = bBlend ? (sName + "|blend") : sName;

    std::map<std::string, CachedTex*>::iterator it = m_Textures.find(sKey);
    if (it != m_Textures.end())
        return it->second->m_bValid ? &it->second->m_Tex : 0;

    // ⚠️ A SURFACE MAY NAME A SPRITE INSTEAD OF A TEXTURE, and 63 of them do.
    // A `.spr` is not an image -- it is a list of DTX frames (see ltspr.h) --
    // so the name is resolved to a frame and the rest of this function never
    // knows the difference. Without this those surfaces got no texture at all
    // and drew flat shaded, which reads as a lighting fault.
    //
    // ⚠️ Frame 0, deliberately, and it is a STILL for now. The sprite carries
    // a real frame rate (1..30) so animating is a clock away, but the clock is
    // the one thing that would make world rendering non-reproducible, and the
    // frame contract is built on it being reproducible. Animate when there is
    // somewhere to put the time that the harness can hold still.
    if (EndsWith(sName, ".spr"))
    {
        CLTSprite spr;
        if (spr.LoadThroughFileMgr(m_pFileMgr, sName.c_str()) && spr.GetNumFrames() > 0)
        {
            const std::string sFrame = spr.GetFrame(0);
            if (!sFrame.empty() && sFrame != sName)
            {
                const LT3DTexture* pTex = GetTexture(sFrame, bBlend);

                // Cache the SPRITE name too, so the next surface using it does
                // not re-read and re-decode the sprite's first frame.
                CachedTex* pAlias = new CachedTex();
                pAlias->m_bValid = (pTex != 0);
                pAlias->m_nSurfaceType = -1;
                if (pTex)
                {
                    pAlias->m_Tex = *pTex;
                    std::map<std::string, CachedTex*>::iterator f = m_Textures.find(sFrame);
                    if (f != m_Textures.end()) pAlias->m_nSurfaceType = f->second->m_nSurfaceType;
                }
                pAlias->m_Tex.m_bBlend = bBlend;
                if (bBlend) pAlias->m_Tex.m_bAlphaTest = false;
                m_Textures[sKey] = pAlias;
                return pAlias->m_bValid ? &pAlias->m_Tex : 0;
            }
        }
    }

    CachedTex* pC = new CachedTex();
    pC->m_bValid = false;
    pC->m_nSurfaceType = -1;

    CLTDtx dtx;
    if (dtx.LoadThroughFileMgr(m_pFileMgr, sName.c_str())
        && dtx.DecodeMip(0, pC->m_RGBA) && !pC->m_RGBA.empty())
    {
        pC->m_Tex.m_pRGBA  = &pC->m_RGBA[0];
        pC->m_Tex.m_Width  = (int)dtx.GetMip(0)->m_nWidth;
        pC->m_Tex.m_Height = (int)dtx.GetMip(0)->m_nHeight;
        {
            // ⚠️ The authored threshold -- see LT3DTexture::m_fAlphaRef -- and
            // the guard is tested AT it.
            const int nRef = dtx.GetAlphaRef();
            pC->m_Tex.m_fAlphaRef = (nRef >= 0) ? (nRef / 255.0f) : 0.5f;
            pC->m_Tex.m_bAlphaTest = dtx.AlphaIsMeaningful()
                                     && DiscriminatesAt(pC->m_RGBA, pC->m_Tex.m_fAlphaRef);

            // ⚠️ ONLY AN AUTHORED THRESHOLD PUNCHES A WORLD SURFACE (2026-10-06,
            // HANDOVER 5be). The donor tests a world section only when its
            // texture carries an alpharef (render_a d3d_rendershader_gouraud
            // .cpp:235, `m_AlphaRef != ALPHAREF_NONE`; lightmapped sections
            // never). The alpha of an un-referenced world texture is something
            // else -- an EnvMap or detail mask, a light panel's glow -- and
            // testing it at the invented 0.5 punched holes: T01S03's elevator
            // ceiling (`Mt0519.dtx`, 32-bit, 75% of its alpha below 128) showed
            // the shaft through a blotch pattern (user report). Swept over 52
            // worlds: 38 such textures on opaque surfaces, ~6,000 light-panel
            // polys among them, and NO cut-out -- every Tex\Trans\ grate and
            // fence authors its AlphaRef. Model skins already follow this rule
            // (CachedModel, "the punch survives only if the texture authored a
            // threshold"). LT_OLDWORLDALPHA=1 restores the guess (says so).
            static const bool s_bOld = []() {
                const bool b = (getenv("LT_OLDWORLDALPHA") != 0);
                if (b) { printf("[LT3D] LT_OLDWORLDALPHA=1 -- world textures alpha-tested without an authored alpharef\n"); fflush(stdout); }
                return b; }();
            if (nRef < 0 && !s_bOld) pC->m_Tex.m_bAlphaTest = false;
            pC->m_Tex.m_bFullyTransparent =
                FullyTransparentAt(pC->m_RGBA, pC->m_Tex.m_fAlphaRef);
        }
        pC->m_nSurfaceType     = dtx.GetUserFlags();
        pC->m_bValid = pC->m_Tex.m_Width > 0 && pC->m_Tex.m_Height > 0;

        // ⚠️ The editor size the level's texture coordinates were authored
        // against -- see LT3DTexture::m_nUVShift. Clamped so neither axis
        // shrinks below one texel. LT_OLDUVSCALE=1 divides by the pixel size
        // again, as before (says so).
        {
            static const bool s_bOldUV = []() {
                const bool b = (getenv("LT_OLDUVSCALE") != 0);
                if (b) { printf("[LT3D] LT_OLDUVSCALE=1 -- world UVs ignore the DTX UI mipmap offset\n"); fflush(stdout); }
                return b; }();
            int nShift = s_bOldUV ? 0 : (int)dtx.GetUIMipmapOffset();
            while (nShift > 0 && ((pC->m_Tex.m_Width >> nShift) < 1 || (pC->m_Tex.m_Height >> nShift) < 1)) --nShift;
            pC->m_Tex.m_nUVShift = nShift;
        }

        // ---- the mip chain, which the DTX already carries ---------------- //
        //
        // ⚠️ Not generated -- SHIPPED, built by the game's own tools, so there
        // is no filter here to disagree with what the game displayed.
        //
        // ⚠️ Stops at the first level that is not a power of two: the sampler
        // wraps by masking, so a 3-wide level would fetch the wrong texel
        // rather than fail.
        unsigned int nMips = dtx.GetNumMipmaps();
        if (nMips > (unsigned int)LT3DTexture::kMaxMips)
            nMips = (unsigned int)LT3DTexture::kMaxMips;

        unsigned int nGood = 0;
        for (unsigned int k = 0; k < nMips; ++k)
        {
            const CLTDtx::Mip* pM = dtx.GetMip(k);
            if (!pM || pM->m_nWidth == 0 || pM->m_nHeight == 0) break;
            if ((pM->m_nWidth  & (pM->m_nWidth  - 1)) != 0) break;
            if ((pM->m_nHeight & (pM->m_nHeight - 1)) != 0) break;
            if (!dtx.DecodeMip(k, pC->m_MipRGBA[k]) || pC->m_MipRGBA[k].empty()) break;

            pC->m_Tex.m_pMipRGBA[k] = &pC->m_MipRGBA[k][0];
            pC->m_Tex.m_MipW[k] = (int)pM->m_nWidth;
            pC->m_Tex.m_MipH[k] = (int)pM->m_nHeight;
            ++nGood;
        }
        pC->m_Tex.m_nMips = (int)nGood;
    }

    // ⚠️ A BLENDED TEXTURE IS NOT ALSO ALPHA-TESTED. The blend already carries
    // every alpha value continuously; punching first would cut a hard hole in
    // the middle of a gradient and defeat the point.
    pC->m_Tex.m_bBlend = bBlend;
    if (bBlend) pC->m_Tex.m_bAlphaTest = false;

    m_Textures[sKey] = pC;
    return pC->m_bValid ? &pC->m_Tex : 0;
}


// ----------------------------------------------------------------------- //
//
// GetSpriteTexture
//
// The blended variant of a texture, carrying ONE object's alpha and blend op.
//
// ⚠️ AN ALIAS, NOT A SECOND DECODE. The pixels come from the ordinary cache
// entry and this only copies the descriptor -- the same trick the `.spr`
// alias above uses, and it is safe for the same reason: both entries live in
// m_Textures for the scene's whole life, so the pointers cannot dangle.
//
// ----------------------------------------------------------------------- //

const LT3DTexture* CLTWorldScene::GetSpriteTexture(const std::string& sName,
                                                   float fAlpha, int nBlendOp)
{
    char aKey[64];
    snprintf(aKey, sizeof aKey, "|sprite|a%.4f|b%d", fAlpha, nBlendOp);
    const std::string sKey = sName + aKey;

    std::map<std::string, CachedTex*>::iterator it = m_Textures.find(sKey);
    if (it != m_Textures.end())
        return it->second->m_bValid ? &it->second->m_Tex : 0;

    const LT3DTexture* pBase = GetTexture(sName, true);

    CachedTex* pC = new CachedTex();
    pC->m_nSurfaceType = -1;
    pC->m_bValid = (pBase != 0);
    if (pBase)
    {
        pC->m_Tex = *pBase;
        pC->m_Tex.m_bAlphaTest  = false;
        pC->m_Tex.m_bBlend      = true;
        pC->m_Tex.m_fConstAlpha = fAlpha;
        pC->m_Tex.m_nBlendOp    = nBlendOp;

        // ⚠️ THE TWO MODES READ THE ALPHA CHANNEL DIFFERENTLY, and collapsing
        // them into one rule produced the most visible wrong picture of this
        // whole session -- black rectangles painted over M05S01's lit windows.
        //
        //   ADDITIVE: the channel takes NO part. An additive blend's source
        //   factor is not the source alpha, so nothing in the texture gates
        //   the contribution; the object's `Alpha` is a BRIGHTNESS dial and
        //   nothing else. NOLF's own LensFlareFX proves it -- it fades a flare
        //   in and out by writing object alpha alone (LensFlareFX.cpp), and
        //   the shipped levels spread 4,624 additive sprites over alphas from
        //   0.01 to 1.0, which would all look identical otherwise.
        //
        //   ORDINARY: the channel IS the coverage, multiplied by the object's
        //   alpha. A channel that does not vary is a CONSTANT coverage -- 255
        //   opaque, 0 invisible -- and folding it into the constant handles
        //   both ends with no special case. `sfx\flares\warmsoft1.spr` is the
        //   one that matters: all-zero alpha, near-black picture, and 77
        //   placements that are NOT additive. Treated as "alpha means nothing,
        //   so use 1.0" it becomes a 254-unit black square in the night sky.
        pC->m_Tex.m_bTexAlpha = false;
        if (nBlendOp != 1 && pBase->m_pRGBA && pBase->m_Width > 0 && pBase->m_Height > 0)
        {
            const size_t nBytes = (size_t)pBase->m_Width * pBase->m_Height * 4;
            const unsigned char n0 = pBase->m_pRGBA[3];
            bool bVaries = false;
            for (size_t q = 7; q < nBytes; q += 4)
                if (pBase->m_pRGBA[q] != n0) { bVaries = true; break; }

            if (bVaries) pC->m_Tex.m_bTexAlpha = true;
            else         pC->m_Tex.m_fConstAlpha = fAlpha * (n0 / 255.0f);
        }
    }

    m_Textures[sKey] = pC;
    return pC->m_bValid ? &pC->m_Tex : 0;
}


// ----------------------------------------------------------------------- //
//
// GetObjectAlphaTexture / ModelObjectAlpha (HANDOVER 5cf)
//
// ⚠️ D3D's MODULATE, as LT2 sets it for a translucent object
// (d3d_SetTranslucentObjectStates): coverage = texel alpha x object alpha,
// source-over, no depth write. The channel analysis is GetSpriteTexture's
// ORDINARY rule and for its reason -- a constant channel is folded into the
// constant, so `m_bTexAlpha` stays false for the opaque textures nearly every
// brush carries and the blend is the object's alpha alone.
//
// ----------------------------------------------------------------------- //

const LT3DTexture* CLTWorldScene::GetObjectAlphaTexture(const std::string& sName, float fAlpha)
{
    const std::string sKey = sName + "|objalpha";

    CachedTex* pC = 0;
    std::map<std::string, CachedTex*>::iterator it = m_Textures.find(sKey);
    if (it != m_Textures.end()) pC = it->second;
    else
    {
        // ⚠️ AN ALIAS of the blended entry -- same pixels, no second decode,
        // and it lives as long as the scene, as GetSpriteTexture's do.
        const LT3DTexture* pBase = GetTexture(sName, true);
        pC = new CachedTex();
        pC->m_nSurfaceType = -1;
        pC->m_bValid = (pBase != 0);
        pC->m_fChanAlpha = 1.0f;
        if (pBase)
        {
            pC->m_Tex = *pBase;
            pC->m_Tex.m_bAlphaTest = false;
            pC->m_Tex.m_bBlend     = true;
            pC->m_Tex.m_nBlendOp   = 0;
            if (pBase->m_pRGBA && pBase->m_Width > 0 && pBase->m_Height > 0)
            {
                const size_t nBytes = (size_t)pBase->m_Width * pBase->m_Height * 4;
                const unsigned char n0 = pBase->m_pRGBA[3];
                bool bVaries = false;
                for (size_t q = 7; q < nBytes; q += 4)
                    if (pBase->m_pRGBA[q] != n0) { bVaries = true; break; }
                pC->m_fChanAlpha = bVaries ? -1.0f : (n0 / 255.0f);
            }
        }
        m_Textures[sKey] = pC;
    }
    if (!pC->m_bValid) return 0;

    // Written on EVERY call: the object's alpha of the moment.
    pC->m_Tex.m_bTexAlpha   = (pC->m_fChanAlpha < 0.0f);
    pC->m_Tex.m_fConstAlpha = pC->m_Tex.m_bTexAlpha ? fAlpha : fAlpha * pC->m_fChanAlpha;
    return &pC->m_Tex;
}


bool CLTWorldScene::ModelObjectAlpha(const LTWorldModelHeader& M,
                                     const ILTWorldModelXform* pXform, float* pAlpha)
{
    float a = 1.0f;
    if (!pXform || !pXform->GetModelAlpha(M.m_sName, &a)) return false;
    if (!(a > 0.0f)) a = 0.0f;
    if (a > 1.0f)    a = 1.0f;
    *pAlpha = a;
    return true;
}


// ----------------------------------------------------------------------- //
//
// DrawSpriteQuad
//
// ⚠️ THE GEOMETRY IS `d3d_DrawSprite`'s, term for term: half-extents of
// (texture width x ScaleX) and (texture height x ScaleY) along the CAMERA's
// right and up, so the full quad is twice that. Getting this wrong by a factor
// of two is invisible on a radially symmetric flare and wrong everywhere else,
// which is why it is written down rather than tuned.
//
// ----------------------------------------------------------------------- //

void CLTWorldScene::DrawSpriteQuad(const LT3DTarget& t, const float* pMVP,
                                   const LTSpriteDraw& sd, const float* pPos3,
                                   const float* pRight3, const float* pUp3,
                                   const float* pFwd3, const float* pEye3,
                                   Stats& st, ILTWorldGeomSink* pSink)
{
    const LT3DTexture* pTex = GetSpriteTexture(sd.m_sSprite, sd.m_fAlpha, sd.m_nBlendOp);
    if (!pTex || pTex->m_Width <= 0 || pTex->m_Height <= 0) return;

    // Distance along the view direction, which is what both the glow factor
    // and the depth bias are measured in.
    float fZ = 0.0f;
    for (int k = 0; k < 3; ++k) fZ += (pPos3[k] - pEye3[k]) * pFwd3[k];

    // ⚠️ Behind the camera. The bias below divides by fZ, and a sprite behind
    // the eye has nothing to contribute anyway.
    const float kNearZ = 4.0f;               // the world projection's near plane
    if (fZ < kNearZ) return;

    float fSizeX = (float)pTex->m_Width  * sd.m_fScaleX;
    float fSizeY = (float)pTex->m_Height * sd.m_fScaleY;

    // ⚠️ 10..500 units maps to 0.1x..2.0x, from the sprite renderer's own
    // constants. A glow sprite is DELIBERATELY not perspective-correct in
    // size: it shrinks as you walk into it and grows as you walk away, which
    // is how a point light reads as a point light.
    if (sd.m_bGlow)
    {
        float f = (fZ - 10.0f) / (500.0f - 10.0f);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        const float fFactor = 0.1f + (2.0f - 0.1f) * f;
        fSizeX *= fFactor;
        fSizeY *= fFactor;
    }

    float c[3] = { pPos3[0], pPos3[1], pPos3[2] };
    float fScaleBias = 1.0f;

    // ⚠️ PULLED TOWARD THE CAMERA AND SHRUNK TO MATCH. Moving it without the
    // shrink would make every flare balloon as it approached the eye; the
    // pair together leave the picture identical and only change which side of
    // the depth test the sprite lands on.
    if (sd.m_bZBias)
    {
        float fBias = -20.0f;
        if (fZ + fBias < kNearZ) fBias = kNearZ - fZ;
        fScaleBias = 1.0f + fBias / fZ;
        for (int k = 0; k < 3; ++k) c[k] += pFwd3[k] * fBias;
        fSizeX *= fScaleBias;
        fSizeY *= fScaleBias;
    }

    float R[3], U[3];
    for (int k = 0; k < 3; ++k) { R[k] = pRight3[k] * fSizeX; U[k] = pUp3[k] * fSizeY; }

    // ⚠️ +up gets V = 0. The projection carries a Y flip (LT3D::Perspective)
    // precisely so that world-up ends at the TOP of a top-down frame buffer,
    // and a DTX decodes row 0 first -- so the texture's top belongs on the
    // sprite's top edge, exactly as it does for every brush in the level.
    const float kU[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    const float kV[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    float W[4][3];
    for (int k = 0; k < 3; ++k)
    {
        W[0][k] = c[k] + U[k] - R[k];
        W[1][k] = c[k] + U[k] + R[k];
        W[2][k] = c[k] - U[k] + R[k];
        W[3][k] = c[k] - U[k] - R[k];
    }

    LT3DVert v[4];
    for (int q = 0; q < 4; ++q)
    {
        LT3D::TransformPoint(pMVP, W[q], v[q].m_Clip);
        v[q].m_U = kU[q];
        v[q].m_V = kV[q];
        v[q].m_R = sd.m_Colour[0];
        v[q].m_G = sd.m_Colour[1];
        v[q].m_B = sd.m_Colour[2];
    }

    // ⚠️ LT_SPRDEBUG=1 prints every sprite this frame draws. Sprites have no
    // geometry in the world file, so when one looks wrong there is nothing to
    // inspect with worldview or propview -- this is the only way to ask which
    // file, at what size, with which blend it was.
    if (getenv("LT_SPRDEBUG"))
        fprintf(stderr, "[SPR] %-34s a=%.3f op=%d glow=%d zb=%d  %dx%d  size %.1fx%.1f  z=%.0f  texa=%d\n",
                sd.m_sSprite.c_str(), sd.m_fAlpha, sd.m_nBlendOp, (int)sd.m_bGlow,
                (int)sd.m_bZBias, pTex->m_Width, pTex->m_Height, fSizeX, fSizeY, fZ,
                (int)pTex->m_bTexAlpha);

    static const int kIdx[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    for (int f = 0; f < 2; ++f)
    {
        LT3DVert tri[3];
        float    mpos[3][3];
        for (int k = 0; k < 3; ++k)
        {
            tri[k] = v[kIdx[f][k]];
            for (int q = 0; q < 3; ++q) mpos[k][q] = W[kIdx[f][k]][q];
        }

        if (pSink) { pSink->Tri(pTex, mpos, tri); ++st.m_nObjTris; continue; }

        // ⚠️ NO BACKFACE CULL. The quad faces the camera, but the scene mirror
        // negates row 0 of the projection and reverses its winding -- culling
        // would drop every sprite in exactly one of the two mirror modes.
        ++st.m_nObjTris;
        st.m_nPixels += LT3D::DrawTriangle(t, pTex, tri[0], tri[1], tri[2], false);
    }
}


// ----------------------------------------------------------------------- //
//
// DrawPolyGrid
//
// ⚠️ THE DONOR'S GRID, term for term (drawpolygrid.cpp, the plain path):
//   - vertex (i, j) at local ((i - (W-1)/2) * sx, height * sy, (j - (H-1)/2) * sz),
//     then the object's rotation and position;
//   - colour = colour table[height + 128] x the object's colour, both 0..255
//     there and 0..1 here;
//   - U from fmod(xPan, 1) across xScale repeats over the whole grid, V the
//     same with yPan / yScale -- the game ANIMATES the scale (PolyGridFX.cpp
//     UpdateSurface), the pan is a fixed offset;
//   - two triangles per cell in pg_Init's index order.
// The texture is the sprite's frame 0, blended by the object's alpha and
// FLAG2_ADDITIVE / _MULTIPLY exactly as a sprite is (GetSpriteTexture).
//
// ⚠️ An UNTEXTURED grid (no SpriteSurfaceName -- M05S01's green `gloop`)
// draws its colours on a white texture. Not drawn: the env map (no shipped FX
// sets one).
//
// ----------------------------------------------------------------------- //

const LT3DTexture* CLTWorldScene::GetWhiteTexture(float fAlpha, int nBlendOp)
{
    char aKey[64];
    snprintf(aKey, sizeof aKey, "|white|a%.4f|b%d", fAlpha, nBlendOp);
    std::map<std::string, CachedTex*>::iterator it = m_Textures.find(aKey);
    if (it != m_Textures.end()) return &it->second->m_Tex;

    CachedTex* pC = new CachedTex();
    pC->m_nSurfaceType = -1;
    pC->m_RGBA.assign(4 * 4 * 4, 255);          // 4x4: wrapping masks need a power of two
    pC->m_Tex.m_pRGBA       = &pC->m_RGBA[0];
    pC->m_Tex.m_Width       = 4;
    pC->m_Tex.m_Height      = 4;
    pC->m_Tex.m_bBlend      = true;
    pC->m_Tex.m_fConstAlpha = fAlpha;
    pC->m_Tex.m_nBlendOp    = nBlendOp;
    pC->m_Tex.m_bTexAlpha   = false;
    pC->m_bValid = true;
    m_Textures[aKey] = pC;
    return &pC->m_Tex;
}

void CLTWorldScene::DrawPolyGrid(const LT3DTarget& t, const float* pMVP,
                                 const LTDynamicPolyGrid& pg,
                                 Stats& st, ILTWorldGeomSink* pSink)
{
    if (!pg.m_pData || !pg.m_pTable || pg.m_W < 2 || pg.m_H < 2) return;
    const LT3DTexture* pTex = pg.m_sSprite.empty()
                              ? GetWhiteTexture(pg.m_fAlpha, pg.m_nBlendOp)
                              : GetSpriteTexture(pg.m_sSprite, pg.m_fAlpha, pg.m_nBlendOp);
    // LT_PGTRACE=1 -- the first draw of each grid's sprite: what it is drawn with.
    if (getenv("LT_PGTRACE"))
    {
        static std::set<std::string> s_Said;
        if (s_Said.insert(pg.m_sSprite).second)
        {
            const float* c0 = pg.m_pTable[128];
            printf("[PG] draw %ux%u '%s' tex %dx%d alpha %.2f op %d at (%.0f %.0f %.0f) scale (%.2f %.3f %.2f)"
                   " colour (%.2f %.2f %.2f) table[128] (%.0f %.0f %.0f) uv %.2f,%.2f x %.2f,%.2f%s\n",
                   pg.m_W, pg.m_H, pg.m_sSprite.c_str(), pTex ? pTex->m_Width : 0, pTex ? pTex->m_Height : 0,
                   pg.m_fAlpha, pg.m_nBlendOp, pg.m_Pos[0], pg.m_Pos[1], pg.m_Pos[2],
                   pg.m_Scale[0], pg.m_Scale[1], pg.m_Scale[2], pg.m_Colour[0], pg.m_Colour[1], pg.m_Colour[2],
                   c0[0], c0[1], c0[2], pg.m_xPan, pg.m_yPan, pg.m_xScale, pg.m_yScale, pSink ? " (Metal)" : "");
            fflush(stdout);
        }
    }
    if (!pTex || pTex->m_Width <= 0 || pTex->m_Height <= 0) return;

    const unsigned int W = pg.m_W, H = pg.m_H;
    float M[16];
    LT3D::FromQuatPos(pg.m_Quat, pg.m_Pos, M);      // row-major, translation in column 3

    std::vector<LT3DVert>& v  = m_PGVerts;      // members, reused frame to frame
    std::vector<float>&    wp = m_PGWorld;
    v.resize((size_t)W * H);
    wp.resize((size_t)W * H * 3);

    const float hw = 0.5f * (float)(W - 1), hh = 0.5f * (float)(H - 1);
    const float u0 = fmodf(pg.m_xPan, 1.0f), v0 = fmodf(pg.m_yPan, 1.0f);
    const float du = pg.m_xScale / (float)(W - 1), dv = pg.m_yScale / (float)(H - 1);
    const float k255 = 1.0f / 255.0f;

    for (unsigned int j = 0; j < H; ++j)
        for (unsigned int i = 0; i < W; ++i)
        {
            const size_t n = (size_t)j * W + i;
            const int    h = pg.m_pData[n];
            const float  l[3] = { ((float)i - hw) * pg.m_Scale[0],
                                  (float)h        * pg.m_Scale[1],
                                  ((float)j - hh) * pg.m_Scale[2] };
            float* w = &wp[n * 3];
            for (int r = 0; r < 3; ++r)
                w[r] = M[r*4+0]*l[0] + M[r*4+1]*l[1] + M[r*4+2]*l[2] + M[r*4+3];
            LT3D::TransformPoint(pMVP, w, v[n].m_Clip);
            v[n].m_U = u0 + (float)i * du;
            v[n].m_V = v0 + (float)j * dv;
            const float* c = pg.m_pTable[(h + 128) & 255];
            v[n].m_R = c[0] * k255 * pg.m_Colour[0];
            v[n].m_G = c[1] * k255 * pg.m_Colour[1];
            v[n].m_B = c[2] * k255 * pg.m_Colour[2];
        }

    for (unsigned int y = 0; y + 1 < H; ++y)
        for (unsigned int x = 0; x + 1 < W; ++x)
        {
            const unsigned int ul = y * W + x;
            const unsigned int idx[2][3] = { { ul + W + 1, ul + 1, ul },
                                             { ul + W,     ul + W + 1, ul } };
            for (int f = 0; f < 2; ++f)
            {
                LT3DVert tri[3];
                float    mpos[3][3];
                for (int k = 0; k < 3; ++k)
                {
                    tri[k] = v[idx[f][k]];
                    for (int q = 0; q < 3; ++q) mpos[k][q] = wp[idx[f][k] * 3 + q];
                }
                ++st.m_nObjTris;
                // ⚠️ NOT CULLED, unlike the donor (CCW unless PG_NOBACKFACECULL):
                // the scene mirror reverses winding in one of its two modes (see
                // DrawSpriteQuad). So the surface also shows from underwater.
                if (pSink) { pSink->Tri(pTex, mpos, tri); continue; }
                st.m_nPixels += LT3D::DrawTriangle(t, pTex, tri[0], tri[1], tri[2], false);
            }
        }
}


CLTWorldScene::CachedModel* CLTWorldScene::GetModel(const std::string& sModel,
                                                    const std::string& sSkin,
                                                    bool bChromakey,
                                                    float fAlpha,
                                                    int nBlendOp)
{
    if (!(fAlpha >= 0.0f && fAlpha <= 1.0f)) fAlpha = 1.0f;
    if (nBlendOp < 0 || nBlendOp > 2) nBlendOp = 0;
    if (!LTModelBlendEnabled()) { fAlpha = 1.0f; nBlendOp = 0; }

    // ⚠️ AN ADDITIVE MODEL IS COMPOSITED WHATEVER ITS ALPHA SAYS -- and the
    // SAME predicate the object passes use, not a second copy of it.
    const bool bTranslucent = LTIsTranslucentDraw(fAlpha, nBlendOp);

    char aKey[32];
    snprintf(aKey, sizeof aKey, "|a%.4f|b%d", fAlpha, nBlendOp);
    std::string sKey = sModel + "|" + sSkin + (bChromakey ? "|ck" : "|--") + aKey;
    std::map<std::string, CachedModel*>::iterator it = m_Models.find(sKey);
    if (it != m_Models.end()) return it->second->m_bValid ? it->second : 0;

    CachedModel* pC = new CachedModel();
    pC->m_nSkins = 0;
    pC->m_bValid = pC->m_Model.LoadThroughFileMgr(m_pFileMgr, sModel.c_str());

    // ⚠️ Same merge as CLTScene::LoadModel -- see there for why both caches
    // must build the same animation table.
    if (pC->m_bValid) pC->m_Model.MergeChildAnimations(m_pFileMgr, sModel.c_str());

    if (pC->m_bValid && !sSkin.empty())
    {
        // ⚠️ Split on ';' and trim -- the shipped strings carry spaces around
        // it. Loading only the first entry draws every piece that indexes a
        // later one UNTEXTURED, which comes out pure white: M01S01's trees
        // rendered as big white stars until this was split.
        size_t a = 0;
        while (a <= sSkin.size() && pC->m_nSkins < kMaxSkins)
        {
            size_t b = sSkin.find(';', a);
            if (b == std::string::npos) b = sSkin.size();
            std::string one = sSkin.substr(a, b - a);
            a = b + 1;

            size_t f = one.find_first_not_of(" \t");
            size_t l = one.find_last_not_of(" \t");
            if (f == std::string::npos) continue;
            one = one.substr(f, l - f + 1);
            if (one.empty()) continue;

            int k = pC->m_nSkins;
            CLTDtx dtx;
            if (dtx.LoadThroughFileMgr(m_pFileMgr, one.c_str())
                && dtx.DecodeMip(0, pC->m_SkinRGBA[k]) && !pC->m_SkinRGBA[k].empty())
            {
                pC->m_Skins[k].m_pRGBA  = &pC->m_SkinRGBA[k][0];
                pC->m_Skins[k].m_Width  = (int)dtx.GetMip(0)->m_nWidth;
                pC->m_Skins[k].m_Height = (int)dtx.GetMip(0)->m_nHeight;
                // ⚠️ THE PROP SAYS WHETHER ITS TEXTURE IS A CUT-OUT, not the
                // texture. `Chromakey` in Attributes\PropTypes.txt is the
                // authored flag and it predicts every case this port verified
                // by eye -- cattail, both wall/ceiling lanterns and the
                // foliage are 1; the hanging lamp, the dinner tray and the
                // flour sack are 0. Nothing in the DTX separates the first
                // group from the second: they share formats, flags, and in one
                // case a skin.
                //
                // ⚠️ THE THRESHOLD IS SET FIRST, because the guard below is
                // tested AT it. Written the other way round -- which it was --
                // DiscriminatesAt reads an uninitialised float, so whether a
                // prop is alpha-tested depends on stack garbage.
                const int nAuthoredRef = dtx.GetAlphaRef();
                pC->m_Skins[k].m_fAlphaRef =
                    (nAuthoredRef >= 0) ? (nAuthoredRef / 255.0f) : 0.5f;

                // ⚠️ The guard still applies on top of the authored flag. A
                // prop may say Chromakey and carry a texture that is
                // transparent EVERYWHERE -- `Rock_01` is -- and punching that
                // erases the prop rather than cutting it out.
                pC->m_Skins[k].m_bAlphaTest = bChromakey
                                              && DiscriminatesAt(pC->m_SkinRGBA[k],
                                                                 pC->m_Skins[k].m_fAlphaRef);
                pC->m_Skins[k].m_bFullyTransparent = false;   // props: no SURF flag

                // ⚠️ THE SHIPPED MIP CHAIN, on the same terms as a world
                // texture: not generated, stopping at the first level that is
                // not a power of two, because the sampler wraps by masking.
                // See CachedModel::m_SkinMipRGBA for why a prop needs it.
                {
                    unsigned int nMips = dtx.GetNumMipmaps();
                    if (nMips > (unsigned int)LT3DTexture::kMaxMips)
                        nMips = (unsigned int)LT3DTexture::kMaxMips;

                    unsigned int nGood = 0;
                    for (unsigned int q = 0; q < nMips; ++q)
                    {
                        const CLTDtx::Mip* pM = dtx.GetMip(q);
                        if (!pM || pM->m_nWidth == 0 || pM->m_nHeight == 0) break;
                        if ((pM->m_nWidth  & (pM->m_nWidth  - 1)) != 0) break;
                        if ((pM->m_nHeight & (pM->m_nHeight - 1)) != 0) break;
                        if (!dtx.DecodeMip(q, pC->m_SkinMipRGBA[k][q])
                            || pC->m_SkinMipRGBA[k][q].empty()) break;

                        pC->m_Skins[k].m_pMipRGBA[q] = &pC->m_SkinMipRGBA[k][q][0];
                        pC->m_Skins[k].m_MipW[q] = (int)pM->m_nWidth;
                        pC->m_Skins[k].m_MipH[q] = (int)pM->m_nHeight;
                        ++nGood;
                    }
                    pC->m_Skins[k].m_nMips = (int)nGood;
                }

                // ⚠️ TRANSLUCENCY IS THE OBJECT'S, NOT THE TEXTURE'S. `Alpha`
                // below 1.0 in Attributes\PropTypes.txt (or on a plain Prop)
                // is what Prop.cpp hands to SetObjectColor, so the whole model
                // is blended and writes no depth. 18 prop types and 178 loose
                // props say it.
                if (bTranslucent)
                {
                    pC->m_Skins[k].m_bBlend      = true;
                    pC->m_Skins[k].m_fConstAlpha = fAlpha;
                    pC->m_Skins[k].m_nBlendOp    = nBlendOp;

                    // ⚠️ The texel's alpha joins the blend only when it varies.
                    // The two light-beam skins are 100% ZERO, and multiplying
                    // by that draws nothing where the level wants a beam.
                    pC->m_Skins[k].m_bTexAlpha = AlphaVaries(pC->m_SkinRGBA[k]);

                    // ⚠️ AND THE PUNCH SURVIVES ONLY IF THE TEXTURE AUTHORED A
                    // THRESHOLD. This is §5d's rule applied to a new question:
                    // an `alpharef` in the command string is the artist saying
                    // "test me", and the foliage and steering-wheel skins all
                    // say 96 -- they stay cut-outs, and blending at 0.999 on
                    // top changes nothing anyone can see. The glass skins
                    // author NOTHING, so there is no threshold to honour, and
                    // punching them at the invented 0.5 would put holes in a
                    // wine glass. Blending replaces the test there rather than
                    // joining it.
                    if (nAuthoredRef < 0) pC->m_Skins[k].m_bAlphaTest = false;
                }
            }
            ++pC->m_nSkins;     // counted either way -- indexed by position
        }
    }

    // ⚠️ LT_SKINTRACE=1 -- WHICH PIECE GETS WHICH SKIN, once per model.
    //
    // A piece whose texture index is >= the number of skins loaded is drawn
    // UNTEXTURED (ltmodeldraw.cpp:320 leaves pTex null), which comes out as a
    // flat pale wedge -- and on a face that reads as a modelling error rather
    // than a missing texture. This prints the model's own demand (the highest
    // texture index its pieces use) against what was actually loaded, so the
    // two are never inferred from a screenshot.
    if (pC->m_bValid && getenv("LT_SKINTRACE"))
    {
        const std::vector<LTABCPiece>& pcs = pC->m_Model.GetPieces();
        int nMax = -1;
        for (size_t i = 0; i < pcs.size(); ++i)
            if ((int)pcs[i].m_nTexture > nMax) nMax = (int)pcs[i].m_nTexture;

        printf("[SKIN] %-40s %u skin(s) loaded, pieces want up to index %d%s\n",
               sModel.c_str(), (unsigned)pC->m_nSkins, nMax,
               (nMax >= (int)pC->m_nSkins) ? "   ⚠️  SHORT -- those pieces draw untextured" : "");
        printf("       skins: %s\n", sSkin.empty() ? "(none)" : sSkin.c_str());
        for (size_t i = 0; i < pcs.size(); ++i)
            printf("       piece %-2u %-22s -> skin %u%s\n",
                   (unsigned)i, pcs[i].m_sName.c_str(), pcs[i].m_nTexture,
                   ((int)pcs[i].m_nTexture >= (int)pC->m_nSkins) ? "  ⚠️  MISSING" : "");
        fflush(stdout);
    }

    m_Models[sKey] = pC;
    return pC->m_bValid ? pC : 0;
}


// ⚠️ Which world models the engine would draw. One of these rules is a flag
// and the other is a heuristic, said plainly rather than blurred:
//
//   * VisBSP is excluded BY FLAG -- it duplicates the main world.
//
//   * AIVolume brushes are excluded BY NAME, because there is no flag. They
//     are ordinary world models -- flags 2, identical to a door -- textured
//     with DEdit's placeholder `Tex\AI.dtx`, whose bitmap literally reads
//     "AI". The engine never draws them because the server-side AIVolume
//     object that owns each one has no visible client representation, and
//     that relationship is not in the world file at all. M01S01 has 78 of
//     them and they fill the corridors.
bool CLTWorldScene::Drawable(const LTWorldModelHeader& m) const
{
    if (!m.m_bBodyParsed) return false;

    // ⚠️ LT_SKIPMODEL=<substring> drops every world model whose NAME contains
    // it, and LT_ONLYMODEL=<substring> keeps only those. Bisecting "which
    // brush is that?" used to mean a rebuild per guess; a level has tens of
    // models and this turns the search into a handful of runs. ⚠️ It is a
    // DIAGNOSTIC -- it sits in Drawable() so both backends honour it from one
    // place, which is the whole point after seven cross-renderer drifts.
    {
        static const char* pSkip = getenv("LT_SKIPMODEL");
        static const char* pOnly = getenv("LT_ONLYMODEL");
        // Both accept a ','-separated list, because the useful questions are
        // "these two together" and "everything except these two".
        if (pSkip && *pSkip && NameMatchesList(m.m_sName, pSkip)) return false;
        if (pOnly && *pOnly && !NameMatchesList(m.m_sName, pOnly)) return false;
    }

    if (m.m_nInfoFlags & kWIF_VISBSP) return false;
    if (m.m_sName.compare(0, 8, "AIVolume") == 0) return false;
    return true;
}


bool CLTWorldScene::Drawable(const LTWorldModelHeader& m,
                             const ILTWorldModelXform* pXform) const
{
    if (!Drawable(m)) return false;
    return !(pXform && pXform->IsModelHidden(m.m_sName));
}


bool CLTWorldScene::Load(CLTFileMgr* pFileMgr, const char* pWorldName)
{
    m_pFileMgr = pFileMgr;
    m_bLoaded = false;
    m_pSky = 0;
    m_Static.m_bValid = false;      // rebuilt by the Metal pass; it frees the old buffer
    m_sError.clear();

    if (!m_World.LoadThroughFileMgr(pFileMgr, pWorldName))
    {
        m_sError = m_World.GetError();
        return false;
    }

    // ⚠️ THE ATTRIBUTE FILE IS THE AUTHORITY FOR PropType OBJECTS. Loaded per
    // scene rather than once globally so a scene stays self-contained, and it
    // is CHEAP -- 301 records, parsed once. Failure is not fatal: without it
    // the object pass falls back to the world's own properties, which is what
    // this port did before and is wrong only for the props listed in
    // ltproptypes.h.
    if (!m_PropTypes.Load(pFileMgr))
        fprintf(stderr, "[WORLD] Attributes\\PropTypes.txt not found -- "
                        "PropType objects will use the world's own (unreliable) "
                        "Filename/Skin\n");

    // ---- which model is the sky ----------------------------------------- //
    //
    // ⚠️ Read from the world, not assumed from a name. A `SkyPointer` object
    // carries a `SkyObjectName` string naming the object that is the sky, and
    // it differs between levels -- "SkyBox" in M01S01, "Clouds1" in M02S01.
    // Matching on "SkyBox" would work in one and silently fail in the other.
    {
        const std::vector<LTWorldObject>& obj = m_World.GetObjects();
        std::string sSkyName;
        for (size_t i = 0; i < obj.size() && sSkyName.empty(); ++i)
        {
            if (obj[i].m_sType != "SkyPointer") continue;
            const LTWorldProperty* p = obj[i].Find("SkyObjectName");
            if (p && !p->m_sString.empty()) sSkyName = p->m_sString;
        }
        if (!sSkyName.empty())
        {
            const std::vector<LTWorldModelHeader>& mdl = m_World.GetModels();
            for (size_t i = 0; i < mdl.size(); ++i)
                if (mdl[i].m_sName == sSkyName) { m_pSky = &mdl[i]; break; }
        }
    }

    m_bLoaded = true;

    // ⚠️ AFTER m_bLoaded and after the sky is resolved: this walks the world's
    // own geometry and loads prop models, so everything it reads must already
    // be in place.
    ComputeFloorDrops();

    BuildModelLights();

    return true;
}


// ---- MODEL LIGHTING -- the engine's rule, from the level's own lights ----- //
//
// ⚠️ WHY THIS EXISTS. Until 2026-09-30 every model in a level was lit by a
// light this port INVENTED: 0.35 ambient plus one fixed direction. The level
// says otherwise, in detail. M01S01's cinematic room has an `ObjectLight`
// (219,181,100, radius 200) standing on Cate's mark -- a warm key light that
// exists for no reason except to light her -- and the Windows build shows her
// face at ~1.0x its texture, warm. The invented light gave ~0.6x, grey.
//
// The rule is the lineage's (donor setupmodel.cpp + renderlight.cpp):
//
//   ambient   = the world's `AmbientLight` + the console's `ModelAdd`
//   each light that lights models is SAMPLED ONCE AT THE OBJECT'S POSITION
//             (distance falloff, and the cone for a DirLight) and applied as a
//             DIRECTIONAL light from the light toward the object
//   vertex    = clamp( ambient + SUM colour_i * max(0, N . -dir_i) )
//
// Which lights light models, read from the level:
//   Light        when `LightObjects` is set           (M01S01: 145, all set)
//   ObjectLight  always -- it lights nothing else     (M01S01: 29)
//   DirLight     when `LightObjects` is set; a spot   (M01S01: 6)
//
// ⚠️ NOT YET: `StaticSunLight` (needs a sky-visibility ray, as the donor casts)
// and the world's LIGHT TABLE -- the per-cell ambient `LightTableRes` refers
// to, which v66 stores somewhere this port does not read. The world's single
// `AmbientLight` stands in for it. Both are named here so their absence is a
// known gap and not a surprise.
namespace
{
    float g_ModelAdd[3] = { 50.0f, 50.0f, 50.0f };      // shipped autoexec.cfg

    // ⚠️ FITTED, NOT DERIVED -- and said so. The donor gives every light a
    // convert-to-ambient fraction; NOLF 1's lights state none and there is no
    // LT2 renderer source to read it from. So it was fitted to the Windows
    // build's own frame of M01S01's cinematic -- at the SAME MOMENT (frame
    // 990, Cate saying "What do you make of all that talk of a traitor?",
    // same pose once characters stopped being mirrored), face skin only:
    //
    //     reference         239.6 190.6 160.6
    //     0.50              235.5 186.0 137.9    blue short
    //     0.75              235.7 189.3 152.4
    //     0.90              235.7 189.3 161.1    <- within 1.6% on every channel
    //     1.00              235.7 189.3 166.9    blue over
    //
    // Red sits at the texture's own value (light saturated) in every arm, which
    // is the 1.6%. An earlier fit at 0.75 compared DIFFERENT poses and is
    // superseded. LT_MODELLIGHT_AMBIENT re-asks the question.
    const float kDefaultConvertToAmbient = 0.9f;

    // ⚠️ LT_DYNMIRROR=1 -- mirror GAME-SPAWNED models in X as props are, which
    // is what this port did until 2026-10-01. Announces itself.
    bool DynamicMirror()
    {
        static const bool b = (getenv("LT_DYNMIRROR") != 0);
        static const bool bSaid = (b ? (printf("[MIRROR] LT_DYNMIRROR=1 -- characters MIRRORED "
                                               "in model space, like props\n"), true) : true);
        (void)bSaid;
        return b;
    }

    // LT_OLDMODELLIGHT=1 -- the invented light, for an A/B. Announces itself.
    bool OldModelLight()
    {
        static const bool b = (getenv("LT_OLDMODELLIGHT") != 0);
        static const bool bSaid = (b ? (printf("[MODELLIGHT] LT_OLDMODELLIGHT=1 -- models lit by "
                                               "the port's old FIXED light, not the level's\n"),
                                        true) : true);
        (void)bSaid;
        return b;
    }

    // LT_MODELLIGHT_AMBIENT=<0..1> -- the fraction of each light's sampled
    // colour that goes to AMBIENT instead of being applied with N.L. The
    // donor inserts every light with such a fraction (InsertLight(...,
    // m_fConvertToAmbient)); NOLF 1's lights do not state one, so it is fitted
    // to the Windows reference -- see HANDOVER.md 5ar. Announces itself.
    float ConvertToAmbient()
    {
        static const char* p = getenv("LT_MODELLIGHT_AMBIENT");
        static const float f = p ? (float)atof(p) : kDefaultConvertToAmbient;
        static const bool bSaid = (p ? (printf("[MODELLIGHT] convert-to-ambient = %.2f\n", f), true)
                                     : true);
        (void)bSaid;
        return (f < 0.0f) ? 0.0f : (f > 1.0f ? 1.0f : f);
    }

    // LT_MODELLIGHT_ATT=linear|quartic -- the distance falloff. The donor
    // carries both; NOLF 1's lights state neither, so the reference picture
    // decides, and this switch is how it is asked. Announces itself.
    bool QuarticFalloff()
    {
        // Quartic by default -- the donor's default for NOLF 2's lights and
        // the falloff the reference fit above was measured with.
        static const char* p = getenv("LT_MODELLIGHT_ATT");
        static const bool b = !(p && (p[0] == 'l' || p[0] == 'L'));
        static const bool bSaid = (p ? (printf("[MODELLIGHT] falloff = %s\n", b ? "QUARTIC" : "LINEAR"),
                                        true) : true);
        (void)bSaid;
        return b;
    }
}

void CLTWorldScene::SetModelAdd(const float* pRGB255)
{
    if (!pRGB255) return;
    for (int k = 0; k < 3; ++k)
    {
        float v = pRGB255[k];
        if (v < 0.0f) v = 0.0f;
        if (v > 250.0f) v = 250.0f;          // the donor clamps ModelAdd to 250
        g_ModelAdd[k] = v;
    }
}

void CLTWorldScene::BuildModelLights()
{
    m_ModelLights.clear();
    m_WorldAmbient[0] = m_WorldAmbient[1] = m_WorldAmbient[2] = 0.0f;

    // "AmbientLight 48 48 48;LightTableRes 1024;..."
    {
        const char* pInfo = m_World.GetInfoString();
        const char* pA = pInfo ? strstr(pInfo, "AmbientLight") : 0;
        if (pA)
            sscanf(pA + 12, "%f %f %f", &m_WorldAmbient[0], &m_WorldAmbient[1], &m_WorldAmbient[2]);
    }

    const std::vector<LTWorldObject>& obj = m_World.GetObjects();
    unsigned nLight = 0, nObjLight = 0, nDir = 0;
    for (size_t i = 0; i < obj.size(); ++i)
    {
        const std::string& t = obj[i].m_sType;
        const bool bLight = (t == "Light"), bObj = (t == "ObjectLight"), bDir = (t == "DirLight");
        if (!bLight && !bObj && !bDir) continue;

        if (bLight || bDir)
        {
            const LTWorldProperty* pLO = obj[i].Find("LightObjects");
            if (!pLO || !pLO->m_bBool) continue;
        }

        ModelLight L;
        if (!obj[i].GetPos(L.m_Pos)) continue;
        const LTWorldProperty* pR = obj[i].Find("LightRadius");
        L.m_fRadius = pR ? pR->m_fReal : 0.0f;
        if (!(L.m_fRadius > 0.0f)) continue;

        const LTWorldProperty* pC = obj[i].Find(bDir ? "InnerColor" : "LightColor");
        const LTWorldProperty* pB = obj[i].Find("BrightScale");
        const float fB = pB ? pB->m_fReal : 1.0f;
        for (int k = 0; k < 3; ++k) L.m_Color[k] = (pC ? pC->m_Vec[k] : 255.0f) * fB;

        L.m_bSpot = bDir;
        L.m_Dir[0] = L.m_Dir[1] = 0.0f; L.m_Dir[2] = 1.0f;
        L.m_fCosHalf = -1.0f;
        if (bDir)
        {
            float eul[3] = { 0, 0, 0 }, M[16];
            obj[i].GetEuler(eul);
            WorldMatrix(L.m_Pos, eul, M);
            L.m_Dir[0] = M[2]; L.m_Dir[1] = M[6]; L.m_Dir[2] = M[10];   // +Z, forward
            const LTWorldProperty* pF = obj[i].Find("FOV");
            const float fFov = pF ? pF->m_fReal : 90.0f;
            L.m_fCosHalf = cosf(fFov * 0.5f * 3.14159265f / 180.0f);
        }
        L.m_sName = obj[i].GetName() ? obj[i].GetName() : "";
        m_ModelLights.push_back(L);
        if (bLight) ++nLight; else if (bObj) ++nObjLight; else ++nDir;
    }

    printf("[MODELLIGHT] %u Light + %u ObjectLight + %u DirLight light models; "
           "AmbientLight %.0f %.0f %.0f, ModelAdd %.0f %.0f %.0f\n",
           nLight, nObjLight, nDir, m_WorldAmbient[0], m_WorldAmbient[1], m_WorldAmbient[2],
           g_ModelAdd[0], g_ModelAdd[1], g_ModelAdd[2]);
}

void CLTWorldScene::ComputeModelLighting(const float* p, LTModelDraw::LTModelLighting& out,
                                         bool bTrace) const
{
    for (int k = 0; k < 3; ++k)
        out.m_Ambient[k] = (m_WorldAmbient[k] + g_ModelAdd[k]) / 255.0f;
    out.m_nLights = 0;

    struct Cand { float lum; float dir[3]; float col[3]; const ModelLight* pL; float att; };
    std::vector<Cand> c;
    const bool bQuartic = QuarticFalloff();

    for (size_t i = 0; i < m_ModelLights.size(); ++i)
    {
        const ModelLight& L = m_ModelLights[i];
        float v[3] = { p[0] - L.m_Pos[0], p[1] - L.m_Pos[1], p[2] - L.m_Pos[2] };
        const float d2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
        if (d2 >= L.m_fRadius * L.m_fRadius) continue;
        const float d = sqrtf(d2);

        float att;
        if (bQuartic) { att = 1.0f - d2 / (L.m_fRadius * L.m_fRadius); att *= att; }
        else            att = 1.0f - d / L.m_fRadius;

        if (L.m_bSpot && d > 1e-4f)
        {
            const float cd = (v[0]*L.m_Dir[0] + v[1]*L.m_Dir[1] + v[2]*L.m_Dir[2]) / d;
            if (cd <= L.m_fCosHalf) continue;
            att *= (cd - L.m_fCosHalf) / (1.0f - L.m_fCosHalf);
        }
        if (att <= 0.0f) continue;

        Cand k2;
        k2.pL = &L; k2.att = att;
        for (int k = 0; k < 3; ++k) k2.col[k] = L.m_Color[k] * att / 255.0f;
        if (d > 1e-4f) { for (int k = 0; k < 3; ++k) k2.dir[k] = v[k] / d; }
        else           { k2.dir[0] = 0.0f; k2.dir[1] = -1.0f; k2.dir[2] = 0.0f; }
        k2.lum = k2.col[0] + k2.col[1] + k2.col[2];
        c.push_back(k2);
    }

    // The strongest first, up to the limit -- the donor's light list does the
    // same with its own maximum.
    for (size_t i = 0; i + 1 < c.size(); ++i)
        for (size_t j = i + 1; j < c.size(); ++j)
            if (c[j].lum > c[i].lum) { Cand t2 = c[i]; c[i] = c[j]; c[j] = t2; }

    const float fAmb = ConvertToAmbient();
    for (size_t i = 0; i < c.size() && out.m_nLights < LTModelDraw::LTModelLighting::kMaxLights; ++i)
    {
        for (int k = 0; k < 3; ++k)
        {
            out.m_Ambient[k] += c[i].col[k] * fAmb;
            out.m_Dir[out.m_nLights][k]   = c[i].dir[k];
            out.m_Color[out.m_nLights][k] = c[i].col[k] * (1.0f - fAmb);
        }
        ++out.m_nLights;
    }

    if (bTrace)
    {
        printf("[MODELLIGHT] at (%.0f %.0f %.0f): ambient %.3f %.3f %.3f, %d light(s)\n",
               p[0], p[1], p[2], out.m_Ambient[0], out.m_Ambient[1], out.m_Ambient[2], out.m_nLights);
        for (size_t i = 0; i < c.size() && i < (size_t)out.m_nLights; ++i)
            printf("[MODELLIGHT]    %-18s att %.3f  -> %.3f %.3f %.3f\n", c[i].pL->m_sName.c_str(),
                   c[i].att, c[i].col[0], c[i].col[1], c[i].col[2]);
        fflush(stdout);
    }
}


bool CLTWorldScene::GetSpawn(float* pPos3, float* pYaw) const
{
    const std::vector<LTWorldObject>& obj = m_World.GetObjects();
    for (size_t i = 0; i < obj.size(); ++i)
    {
        if (obj[i].m_sType != "GameStartPoint") continue;
        if (!obj[i].GetPos(pPos3)) continue;

        float eul[3] = { 0, 0, 0 };
        obj[i].GetEuler(eul);

        // ⚠️ Yaw only. The start point's pitch and roll are the PLAYER's, and
        // a player spawns upright; applying all three would tilt the horizon.
        if (pYaw) *pYaw = eul[1];
        return true;
    }
    return false;
}


// Builds a front-to-back draw order for one model, or returns null when the
// model is too small for it to be worth the sort.
//
// ⚠️ The scratch vectors are MEMBERS, reused every frame. Allocating 5,000
// pairs per model per frame would hand back a slice of what the ordering wins.
bool CLTWorldScene::HasBlendedSurface(const LTWorldModelHeader& M) const
{
    for (size_t i = 0; i < M.m_Surfaces.size(); ++i)
        if (M.m_Surfaces[i].m_nFlags & kSURF_TRANSPARENT) return true;
    return false;
}


const std::vector<int>* CLTWorldScene::BuildBackToFrontOrder(
        const LTWorldModelHeader& M, const float* pEye3)
{
    m_SortKeys.clear();
    m_SortKeys.reserve(M.m_Polys.size());

    for (size_t q = 0; q < M.m_Polys.size(); ++q)
    {
        const LTWorldPoly& po = M.m_Polys[q];

        // ⚠️ THE FARTHEST VERTEX, the mirror of BuildOrder's nearest one, and
        // for the mirror of its reason. A pane seen edge-on spans a range of
        // depths; compositing has to start with the part that is behind
        // everything else, so the polygon's key is how far away its most
        // distant corner is.
        //
        // ⚠️ NOT the stored m_Centre -- FORMATS.md records that it is not the
        // centroid and sits a mean of 112 units from it.
        float fFar = -1.0f;
        for (size_t k = 0; k < po.m_Verts.size(); ++k)
        {
            if (po.m_Verts[k].m_nPoint >= M.m_Points.size()) continue;
            const float* p = M.m_Points[po.m_Verts[k].m_nPoint].m_Pos;
            const float dx = p[0] - pEye3[0];
            const float dy = p[1] - pEye3[1];
            const float dz = p[2] - pEye3[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 > fFar) fFar = d2;
        }

        // Negated so the ordinary ascending sort puts the FARTHEST first.
        m_SortKeys.push_back(std::make_pair(-fFar, (int)q));
    }

    std::sort(m_SortKeys.begin(), m_SortKeys.end());

    m_SortOrder.clear();
    m_SortOrder.reserve(m_SortKeys.size());
    for (size_t k = 0; k < m_SortKeys.size(); ++k)
        m_SortOrder.push_back(m_SortKeys[k].second);

    return &m_SortOrder;
}


const std::vector<int>* CLTWorldScene::BuildOrder(const LTWorldModelHeader& M,
                                                  const float* pEye3)
{
    // Below this the sort costs more than the overdraw it saves; M01S01's
    // architecture is one model of 5,049 polygons and everything else is tens.
    if (!m_bSort || M.m_Polys.size() < 256) return 0;

    m_SortKeys.clear();
    m_SortKeys.reserve(M.m_Polys.size());
    for (size_t q = 0; q < M.m_Polys.size(); ++q)
    {
        const LTWorldPoly& po = M.m_Polys[q];

        // ⚠️ THE NEAREST VERTEX, NOT THE CENTRE, and the difference is the
        // whole optimisation. A corridor floor is ONE polygon running from
        // under your feet to the far wall: its centre is far away, so a
        // centre-distance sort draws it late, after everything it should have
        // occluded. Sorting on the nearest point puts it first, which is what
        // "front to back" has to mean for large polygons.
        //
        // ⚠️ Sorting by the stored m_Centre also does not work for a second and
        // independent reason -- see FORMATS.md: that field is NOT the polygon's
        // centroid. It sits a mean of 112 units away from it (worst 1,938) and
        // matches neither the vertex mean nor the bounding-box centre.
        float fNear = 3.4e38f;
        for (size_t k = 0; k < po.m_Verts.size(); ++k)
        {
            if (po.m_Verts[k].m_nPoint >= M.m_Points.size()) continue;
            const float* p = M.m_Points[po.m_Verts[k].m_nPoint].m_Pos;
            const float dx = p[0] - pEye3[0];
            const float dy = p[1] - pEye3[1];
            const float dz = p[2] - pEye3[2];
            const float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < fNear) fNear = d2;
        }
        m_SortKeys.push_back(std::make_pair(fNear, (int)q));
    }

    std::sort(m_SortKeys.begin(), m_SortKeys.end());

    m_SortOrder.clear();
    m_SortOrder.reserve(m_SortKeys.size());
    for (size_t k = 0; k < m_SortKeys.size(); ++k)
        m_SortOrder.push_back(m_SortKeys[k].second);

    return &m_SortOrder;
}


void CLTWorldScene::DrawModel(const LT3DTarget& t, const float* pMVP,
                              const LTWorldModelHeader& M,
                              const std::vector<const LT3DTexture*>& tex,
                              Stats& st,
                              const std::vector<int>* pOrder,
                              ILTWorldGeomSink* pSink,
                              EGeomPass ePass)
{
    // ⚠️ pOrder is a DRAW ORDER, not a subset -- every polygon is still drawn.
    // Null means file order, which is what this always did.
    const size_t nDraw = pOrder ? pOrder->size() : M.m_Polys.size();

    for (size_t nAt = 0; nAt < nDraw; ++nAt)
    {
        const size_t q = pOrder ? (size_t)(*pOrder)[nAt] : nAt;
        const LTWorldPoly& po = M.m_Polys[q];
        if (po.m_Verts.size() < 3) continue;

        if (po.m_nSurface >= M.m_Surfaces.size()) continue;

        const LTWorldSurface& sf = M.m_Surfaces[po.m_nSurface];

        // ⚠️ A SKY PORTAL IS NOT A TEXTURED SURFACE. The engine draws the sky
        // through it; the texture on disk is DEdit's placeholder and it says
        // so -- the bitmap literally reads "SKY". Drawing it puts a cyan slab
        // where the sky belongs.
        if (sf.m_nFlags & kSURF_SKY) continue;
        if (sf.m_nFlags & kSURF_INVISIBLE) continue;

        const LT3DTexture* pTex = 0;
        if (sf.m_nTexture < tex.size()) pTex = tex[sf.m_nTexture];
        if (!pTex) continue;

        // ⚠️ THE LEVEL SAYS THIS SURFACE IS SEE-THROUGH AND ITS TEXTURE SHOWS
        // NOTHING, so draw nothing. The alpha-test guard deliberately refuses
        // to punch a wholly transparent texture -- that erases props whose
        // alpha is meaningless garbage -- but a surface the LEVEL flagged
        // SURF_TRANSPARENT is the case where the emptiness is intended.
        // `tex\invisible.dtx` is on 88 surfaces and was drawing as a solid
        // panel.
        const bool bTrans = (sf.m_nFlags & kSURF_TRANSPARENT) != 0;
        if (bTrans && pTex->m_bFullyTransparent) continue;

        // ⚠️ EACH SURFACE BELONGS TO EXACTLY ONE PASS. Blended geometry writes
        // no depth, so it has to be drawn after everything opaque -- which
        // means the world is walked twice and each walk skips the other half.
        // A surface drawn in BOTH would blend over itself.
        if (ePass == kPassOpaque  &&  bTrans) continue;
        if (ePass == kPassBlended && !bTrans) continue;

        // ⚠️ SURF_BRIGHT means FULLBRIGHT -- ignore lighting. Every skybox
        // face carries it, and an unlit brush stores vertex RGB 255, which the
        // 1/128 scale below turns into 2.0 and saturates the texture to flat
        // white. That is exactly what the first sky render produced.
        const bool bBright = (sf.m_nFlags & kSURF_BRIGHT) != 0;

        for (size_t k = 1; k + 1 < po.m_Verts.size(); ++k)
        {
            const size_t vi[3] = { 0, k, k + 1 };
            LT3DVert tri[3];
            float    wpos[3][3];        // world space, for the GPU backend
            bool bOK = true;

            for (int v = 0; v < 3; ++v)
            {
                const LTWorldPolyVert& pv = po.m_Verts[vi[v]];
                if (pv.m_nPoint >= M.m_Points.size()) { bOK = false; break; }
                const LTWorldPoint& pt = M.m_Points[pv.m_nPoint];
                LT3D::TransformPoint(pMVP, pt.m_Pos, tri[v].m_Clip);
                for (int q2 = 0; q2 < 3; ++q2) wpos[v][q2] = pt.m_Pos[q2];

                // ⚠️ P and Q carry the TEXEL SCALE in their length -- measured
                // across 80,913 surfaces they are 1, 2, 4, 8, 0.5, 0.25 and so
                // on, never normalised. So the dot product is already in texels
                // and only the texture size divides it.
                //
                // ⚠️ AND U IS NEGATED. Every wall, floor and ceiling in the
                // game looks correct either way, because wood grain, plaster
                // and carpet are symmetric enough to hide a mirrored axis.
                // TEXT IS NOT: the door plate in M01S01's first corridor read
                // "82" until this sign was flipped, and now reads "28".
                //
                // ⚠️ So a whole level of plausible-looking texturing proved
                // nothing about handedness, and one two-digit number settled
                // it. When a bug can only be seen on one kind of content, go
                // and find that content rather than trusting the rest.
                float d[3] = { pt.m_Pos[0] - sf.m_O[0],
                               pt.m_Pos[1] - sf.m_O[1],
                               pt.m_Pos[2] - sf.m_O[2] };
                float u  = d[0]*sf.m_P[0] + d[1]*sf.m_P[1] + d[2]*sf.m_P[2];
                float vv = d[0]*sf.m_Q[0] + d[1]*sf.m_Q[1] + d[2]*sf.m_Q[2];
                // ⚠️ The U sign moves WITH the mirror: flipping the axis
                // without flipping U would mirror the text too, which tests
                // nothing -- the whole point is a readable, oppositely-handed
                // world to compare against the original.
                //
                // ⚠️ Divided by the EDITOR size, which is the pixel size
                // shifted by the DTX's UI mipmap offset (LT3DTexture::
                // m_nUVShift) -- a 256x256 painting authored as 128x128.
                tri[v].m_U = (MirrorWorld() ? u : -u) / (float)(pTex->m_Width  >> pTex->m_nUVShift);
                tri[v].m_V = vv / (float)(pTex->m_Height >> pTex->m_nUVShift);

                // Lighting comes from the FILE: each polygon vertex carries an
                // RGB triple with 128 as neutral. These are baked vertex
                // lights, which is why unlit brushes are exactly 255,255,255.
                if (bBright)
                {
                    tri[v].m_R = tri[v].m_G = tri[v].m_B = 1.0f;
                }
                else
                {
                    tri[v].m_R = pv.m_RGB[0] * (1.0f / 128.0f);
                    tri[v].m_G = pv.m_RGB[1] * (1.0f / 128.0f);
                    tri[v].m_B = pv.m_RGB[2] * (1.0f / 128.0f);
                }
            }
            if (!bOK) continue;

            // ⚠️ ONE DECISION PATH, TWO BACKENDS. Everything above -- the
            // surface flags, the negated U, the texel scale in P/Q, the 1/128
            // vertex-light scale -- has been got wrong at least once already
            // and is settled here. A GPU backend that rebuilt any of it would
            // be a second renderer, and this file exists to prevent that.
            if (pSink)
            {
                pSink->Tri(pTex, wpos, tri);
                ++st.m_nWorldTris;
                continue;
            }

            st.m_nPixels += LT3D::DrawTriangle(t, pTex, tri[0], tri[1], tri[2], false);
            ++st.m_nWorldTris;
        }
    }
}



// ----------------------------------------------------------------------- //
//
// MoveToFloor
//
// ⚠️ WHY THIS IS HERE AT ALL. `MoveToFloor` is a property the level author sets
// on about half of every level's props, and it is the GAME that acts on it: the
// ObjectDLL drops the object onto whatever is under it when the level spawns.
// This port has no ObjectDLL, so nothing did, and props hang in the air --
// measured on M01S01, 33 objects float, by a mean of 26.8 units and a worst of
// 92 (a tree).
//
// ⚠️ AND WHY THE BOX HAS TO COME FROM THE DRAWN GEOMETRY. Dropping an object by
// its ORIGIN sinks it up to half its height into the floor, because a model's
// origin is not its base. The offset that matters is from the origin to the
// LOWEST DRAWN POINT, after the object's own rotation and scale -- a rotated
// chair does not present the same lowest point as an upright one.
//
// ----------------------------------------------------------------------- //

namespace
{
}


// The highest surface at or below fFromY under (x, z), or false if there is
// none within fMaxDrop.
//
// ⚠️ A brute-force ray against every drawn triangle, deliberately. It runs ONCE
// per MoveToFloor object at load -- 38 objects against 9,189 triangles on
// M01S01 -- and using the collision module instead would make every consumer of
// this renderer link the collision module too. That dependency is exactly the
// mistake that left the walk suite silently unbuildable for a session.
bool CLTWorldScene::FloorUnder(float fX, float fZ, float fFromY,
                               float fMaxDrop, float* pOutY) const
{
    const std::vector<LTWorldModelHeader>& mdl = m_World.GetModels();

    float fBest = -3.4e38f;
    bool  bHit  = false;

    for (size_t i = 0; i < mdl.size(); ++i)
    {
        const LTWorldModelHeader& M = mdl[i];
        if (&M == m_pSky || !M.m_bBodyParsed) continue;
        if (!Drawable(M)) continue;

        for (size_t q = 0; q < M.m_Polys.size(); ++q)
        {
            const LTWorldPoly& po = M.m_Polys[q];
            if (po.m_Verts.size() < 3) continue;
            if (po.m_nSurface >= M.m_Surfaces.size()) continue;
            const unsigned int nF = M.m_Surfaces[po.m_nSurface].m_nFlags;
            if (nF & kSURF_SKY) continue;
            if (nF & kSURF_INVISIBLE) continue;

            for (size_t k = 1; k + 1 < po.m_Verts.size(); ++k)
            {
                const unsigned short vi[3] = { po.m_Verts[0].m_nPoint,
                                               po.m_Verts[k].m_nPoint,
                                               po.m_Verts[k+1].m_nPoint };
                if (vi[0] >= M.m_Points.size() ||
                    vi[1] >= M.m_Points.size() ||
                    vi[2] >= M.m_Points.size()) continue;

                const float* a = M.m_Points[vi[0]].m_Pos;
                const float* b = M.m_Points[vi[1]].m_Pos;
                const float* c = M.m_Points[vi[2]].m_Pos;

                // Barycentric test in the XZ plane -- a vertical wall projects
                // to zero area and drops out here on its own.
                const float d = (b[2]-c[2])*(a[0]-c[0]) + (c[0]-b[0])*(a[2]-c[2]);
                if (d > -1e-6f && d < 1e-6f) continue;

                const float w0 = ((b[2]-c[2])*(fX-c[0]) + (c[0]-b[0])*(fZ-c[2])) / d;
                if (w0 < 0.0f || w0 > 1.0f) continue;
                const float w1 = ((c[2]-a[2])*(fX-c[0]) + (a[0]-c[0])*(fZ-c[2])) / d;
                if (w1 < 0.0f || w1 > 1.0f) continue;
                const float w2 = 1.0f - w0 - w1;
                if (w2 < 0.0f || w2 > 1.0f) continue;

                const float y = w0*a[1] + w1*b[1] + w2*c[1];

                // At or below the start, and not further than the limit.
                if (y > fFromY + 1.0f) continue;
                if (y < fFromY - fMaxDrop) continue;
                if (y > fBest) { fBest = y; bHit = true; }
            }
        }
    }

    if (bHit && pOutY) *pOutY = fBest;
    return bHit;
}


int CLTWorldScene::SurfaceUnder(float fX, float fZ, float fFromY, float fMaxDrop)
{
    const std::vector<LTWorldModelHeader>& mdl = m_World.GetModels();

    float fBest = -3.4e38f;
    int   nBest = -1;

    for (size_t i = 0; i < mdl.size(); ++i)
    {
        const LTWorldModelHeader& M = mdl[i];
        if (&M == m_pSky || !M.m_bBodyParsed) continue;
        if (!Drawable(M)) continue;

        for (size_t q = 0; q < M.m_Polys.size(); ++q)
        {
            const LTWorldPoly& po = M.m_Polys[q];
            if (po.m_Verts.size() < 3) continue;
            if (po.m_nSurface >= M.m_Surfaces.size()) continue;
            const LTWorldSurface& sf = M.m_Surfaces[po.m_nSurface];
            if (sf.m_nFlags & kSURF_SKY) continue;
            if (sf.m_nFlags & kSURF_INVISIBLE) continue;

            for (size_t k = 1; k + 1 < po.m_Verts.size(); ++k)
            {
                const unsigned short vi[3] = { po.m_Verts[0].m_nPoint,
                                               po.m_Verts[k].m_nPoint,
                                               po.m_Verts[k+1].m_nPoint };
                if (vi[0] >= M.m_Points.size() ||
                    vi[1] >= M.m_Points.size() ||
                    vi[2] >= M.m_Points.size()) continue;

                const float* a = M.m_Points[vi[0]].m_Pos;
                const float* b = M.m_Points[vi[1]].m_Pos;
                const float* c = M.m_Points[vi[2]].m_Pos;

                const float d = (b[2]-c[2])*(a[0]-c[0]) + (c[0]-b[0])*(a[2]-c[2]);
                if (d > -1e-6f && d < 1e-6f) continue;

                const float w0 = ((b[2]-c[2])*(fX-c[0]) + (c[0]-b[0])*(fZ-c[2])) / d;
                if (w0 < 0.0f || w0 > 1.0f) continue;
                const float w1 = ((c[2]-a[2])*(fX-c[0]) + (a[0]-c[0])*(fZ-c[2])) / d;
                if (w1 < 0.0f || w1 > 1.0f) continue;
                const float w2 = 1.0f - w0 - w1;
                if (w2 < 0.0f || w2 > 1.0f) continue;

                const float y = w0*a[1] + w1*b[1] + w2*c[1];
                if (y > fFromY + 1.0f) continue;
                if (y < fFromY - fMaxDrop) continue;
                if (y <= fBest) continue;

                // ⚠️ The surface type is on the TEXTURE, so this LOADS one if
                // the renderer has not already. Looking only in the cache made
                // the answer depend on what had been drawn.
                if (sf.m_nTexture >= M.m_Textures.size()) continue;
                const std::string& sTex = M.m_Textures[sf.m_nTexture];
                GetTexture(sTex);                       // caches on demand
                std::map<std::string, CachedTex*>::const_iterator it =
                    m_Textures.find(sTex);
                if (it == m_Textures.end() || !it->second) continue;

                fBest = y;
                nBest = it->second->m_nSurfaceType;
            }
        }
    }
    return nBest;
}


bool CLTWorldScene::PolySurfaceType(unsigned int nPolyId, unsigned int* pType)
{
    if (pType) *pType = 0;
    unsigned int nModel = 0, nPoly = 0;
    if (!LTWorldPolyDecode(nPolyId, &nModel, &nPoly)) return false;
    const std::vector<LTWorldModelHeader>& mdl = m_World.GetModels();
    if (nModel >= mdl.size()) return false;
    const LTWorldModelHeader& M = mdl[nModel];
    if (nPoly >= M.m_Polys.size()) return false;
    const unsigned short nSurf = M.m_Polys[nPoly].m_nSurface;
    if (nSurf >= M.m_Surfaces.size()) return false;
    const unsigned short nTex = M.m_Surfaces[nSurf].m_nTexture;
    if (nTex >= M.m_Textures.size()) return false;
    const std::string& sTex = M.m_Textures[nTex];
    GetTexture(sTex);                                   // caches on demand
    std::map<std::string, CachedTex*>::const_iterator it = m_Textures.find(sTex);
    if (it == m_Textures.end() || !it->second || it->second->m_nSurfaceType < 0) return false;
    if (pType) *pType = (unsigned int)it->second->m_nSurfaceType;
    return true;
}


void CLTWorldScene::ComputeFloorDrops()
{
    const std::vector<LTWorldObject>& obj = m_World.GetObjects();
    m_ObjectDrop.assign(obj.size(), 0.0f);

    // ⚠️ THIS FOLLOWS THE GAME'S OWN MoveObjectToFloor -- see
    // NOLF/ObjectDLL/ServerUtilities.cpp:470 -- rather than a reimplementation
    // of what "drop to the floor" ought to mean. Three details come straight
    // from there and every one of them changes the answer:
    //
    //   * `INTERSECT_OBJECTS` -- the ray hits OTHER OBJECTS, not just world
    //     brushes. A first attempt here tested world geometry only, and
    //     cigarette butts and drinks fell 105-125 units THROUGH the tables
    //     they were standing on. That is a regression dressed as a fix.
    //   * the height is the model's ANIMATION DIMS times the object's scale
    //     (`GetModelAnimUserDims`, Prop.cpp:398), not a bounding box computed
    //     from the vertices. Dims are half-extents about the object's origin.
    //   * it only ever moves DOWN, and only when the gap already exceeds the
    //     object's own half-height (`if (fDist > vDims.y)`).
    //
    // ⚠️ And it is ordered: the game runs this per object on its first update,
    // so an object resting on another sees that one already placed. One pass in
    // object order reproduces it.

    struct Placed { float m_Pos[3]; float m_Dims[3]; bool m_bSolid; bool m_bReal; };
    std::vector<Placed> pl(obj.size());
    for (size_t i = 0; i < obj.size(); ++i)
    {
        pl[i].m_bReal = false;
        pl[i].m_bSolid = false;
        for (int k = 0; k < 3; ++k) { pl[i].m_Pos[k] = 0.0f; pl[i].m_Dims[k] = 0.0f; }

        // ⚠️ THE MODEL THE GAME USES -- ResolveObjectModel, so a PropType gets
        // its type's (it carries no Filename at all, and this loop used to
        // skip every one of them: 106 objects in M02S01 alone).
        std::string sModel, sSkin;
        bool bCK = false;
        if (!ResolveObjectModel(i, sModel, sSkin, &bCK)) continue;
        const LTWorldProperty* pV = obj[i].Find("Visible");
        if (pV && !pV->m_bBool) continue;
        if (!obj[i].GetPos(pl[i].m_Pos)) continue;

        CachedModel* pMod = GetModel(sModel, sSkin, bCK);
        if (!pMod) continue;

        const std::vector<LTABCAnim>& an = pMod->m_Model.GetAnims();
        if (an.empty()) continue;                   // no dims: leave it alone

        float scale[3];
        ObjectScale(i, scale);

        for (int k = 0; k < 3; ++k) pl[i].m_Dims[k] = an[0].m_Dims[k] * scale[k];

        const LTWorldProperty* pSol = obj[i].Find("Solid");
        pl[i].m_bSolid = (pSol && pSol->m_bBool);
        pl[i].m_bReal = true;
    }

    for (size_t i = 0; i < obj.size(); ++i)
    {
        if (!pl[i].m_bReal) continue;
        if (!ObjectMoveToFloor(i)) continue;

        const float fX = pl[i].m_Pos[0], fY = pl[i].m_Pos[1], fZ = pl[i].m_Pos[2];

        float fHit = -3.4e38f;
        bool  bHit = false;

        float fWorld = 0.0f;
        if (FloorUnder(fX, fZ, fY, 10000.0f, &fWorld)) { fHit = fWorld; bHit = true; }

        // ...and the tops of other SOLID objects, at their current placement.
        for (size_t j = 0; j < obj.size(); ++j)
        {
            if (j == i || !pl[j].m_bReal || !pl[j].m_bSolid) continue;
            if (pl[j].m_Dims[0] <= 0.0f && pl[j].m_Dims[2] <= 0.0f) continue;

            const float ox = pl[j].m_Pos[0], oz = pl[j].m_Pos[2];
            if (fX < ox - pl[j].m_Dims[0] || fX > ox + pl[j].m_Dims[0]) continue;
            if (fZ < oz - pl[j].m_Dims[2] || fZ > oz + pl[j].m_Dims[2]) continue;

            const float fTop = pl[j].m_Pos[1] + m_ObjectDrop[j] + pl[j].m_Dims[1];
            if (fTop > fY) continue;                // above us: not a floor
            if (fTop > fHit) { fHit = fTop; bHit = true; }
        }

        if (!bHit) continue;

        const float fDist = fY - fHit;
        if (fDist > pl[i].m_Dims[1])
            m_ObjectDrop[i] = -(fDist - (pl[i].m_Dims[1] + 0.1f));

        // LT_DROPTRACE=<name part> -- the renderer's own MoveToFloor result, to
        // set beside the server's (LT_PROPTRACE). Says so.
        static const char* s_pDrop = []() -> const char* {
            const char* p = getenv("LT_DROPTRACE");
            if (p && p[0]) { printf("[DROP] LT_DROPTRACE=%s -- the renderer's floor drops\n", p); fflush(stdout); }
            return (p && p[0]) ? p : 0; }();
        if (s_pDrop && obj[i].GetName() && strcasestr(obj[i].GetName(), s_pDrop))
            printf("[DROP] %-12s y %.1f -> %.1f  (floor %.1f, dims.y %.1f)\n", obj[i].GetName(),
                   fY, fY + m_ObjectDrop[i], fHit, pl[i].m_Dims[1]);
    }
}

// ---- LT_UVPROBE in the GAME -- the same instrument modelview has --------- //
//
// ⚠️ WHY IT IS HERE AS WELL. modelview draws LOD 0 with mip 0 only, nearest
// sampling and a view-space light; the game draws the same model through a
// target with bilinear, trilinear, 4x anisotropy, the FULL mip chain and a
// world-space light. A fix proven in modelview therefore proves nothing about
// the frame a player sees -- which is exactly what happened with the cheek
// slab (HANDOVER.md 5ar). So the per-pixel probe is wired into the game's own
// frame too.
//
//   LT_UVPROBE=1            attach the probe buffers to the in-game target
//   LT_UVPROBE_MODEL=<sub>  stamp only models whose name contains <sub>
//                           (default "HERO" -- Cate); everything else stamps 0,
//                           so a pixel another object wins is recorded as such
//
// The buffers are written beside each LT_DUMP_EVERY frame by the host, via
// LTWorldScene_ProbeDump below, so every probe file pairs with a screenshot.
namespace
{
    std::vector<unsigned>      s_ProbeID;
    std::vector<unsigned char> s_ProbeTex;
    std::vector<float>         s_ProbeUV;
    std::vector<unsigned char> s_ProbeMip;
    std::vector<unsigned char> s_ProbeFinal;     // the 3D view as rendered
    int                        s_nProbeW = 0, s_nProbeH = 0;

    bool ProbeOn()
    {
        static const bool b = (getenv("LT_UVPROBE") != 0);
        return b;
    }
    const char* ProbeModel()
    {
        static const char* p = getenv("LT_UVPROBE_MODEL") ? getenv("LT_UVPROBE_MODEL") : "HERO";
        return p;
    }
    bool ProbeWants(const std::string& sModel)
    {
        std::string a = sModel, b = ProbeModel();
        for (size_t i = 0; i < a.size(); ++i) a[i] = (char)toupper((unsigned char)a[i]);
        for (size_t i = 0; i < b.size(); ++i) b[i] = (char)toupper((unsigned char)b[i]);
        return a.find(b) != std::string::npos;
    }
}

namespace
{
    // The pose builder's hook -> the game's node-control callback.
    struct NodeTramp { LTDynNodeFn fn; void* hObj; void* user; };
    void NodeTrampoline(unsigned int nNode, float* pGlobal16, void* pUser)
    {
        const NodeTramp* t = (const NodeTramp*)pUser;
        t->fn(t->hObj, nNode, pGlobal16, t->user);
    }
}

bool LTWorldScene_ProbeDump(const char* pBase)
{
    if (!ProbeOn() || s_nProbeW <= 0 || s_nProbeH <= 0 || !pBase) return false;
    const size_t n = (size_t)s_nProbeW * s_nProbeH;
    char szPath[600];

    // The two images as PPMs, the rest raw, all in the 3D view's own frame.
    const unsigned char* imgs[2] = { &s_ProbeFinal[0], &s_ProbeTex[0] };
    const char* names[2] = { "final", "texel" };
    for (int k = 0; k < 2; ++k)
    {
        snprintf(szPath, sizeof(szPath), "%s.probe.%s.ppm", pBase, names[k]);
        FILE* f = fopen(szPath, "wb");
        if (!f) continue;
        fprintf(f, "P6\n%d %d\n255\n", s_nProbeW, s_nProbeH);
        for (size_t i = 0; i < n; ++i) fwrite(imgs[k] + i * 4, 1, 3, f);
        fclose(f);
    }
    snprintf(szPath, sizeof(szPath), "%s.probe.triid.bin", pBase);
    if (FILE* f = fopen(szPath, "wb")) { fwrite(&s_ProbeID[0], sizeof(unsigned), n, f); fclose(f); }
    snprintf(szPath, sizeof(szPath), "%s.probe.uv.bin", pBase);
    if (FILE* f = fopen(szPath, "wb")) { fwrite(&s_ProbeUV[0], sizeof(float), n * 2, f); fclose(f); }
    snprintf(szPath, sizeof(szPath), "%s.probe.mip.bin", pBase);
    if (FILE* f = fopen(szPath, "wb")) { fwrite(&s_ProbeMip[0], 1, n, f); fclose(f); }
    snprintf(szPath, sizeof(szPath), "%s.probe.txt", pBase);
    if (FILE* f = fopen(szPath, "w"))
    {
        fprintf(f, "%d %d %s\n", s_nProbeW, s_nProbeH, ProbeModel());
        fclose(f);
    }
    return true;
}

// ⚠️ ONE GAME-SPAWNED MODEL, READY TO DRAW -- shared by BOTH backends.
//
// Everything pass 3b decides about a character -- which cached model, where it
// stands (origin offset, mirror), its pose (animation, the extra trackers'
// layers, the game's node control for lip sync), and its light -- happens here,
// and the software and Metal passes differ ONLY in where the triangles go. The
// Metal backend drew no characters at all until this existed; copying these
// rules into it would have been the seventh cross-renderer drift.
bool CLTWorldScene::PrepareDynamic(const LTDynamicModel& d, const float* MVP,
                                   DynamicDraw& out)
{
    CachedModel* pM = GetModel(d.sModel, d.sSkins, false, d.m_fAlpha, d.m_nBlendOp);
    if (!pM) return false;

    // ⚠️ GAME-SPAWNED MODELS ARE NOT MIRRORED -- props are. Measured
    // against the Windows build's frame of M01S01's cinematic: with
    // the mirror, Cate preens with her glove behind the WRONG side of
    // her head and her face turned the wrong way; without it she
    // matches the reference exactly. The same frame's door lever
    // points the way the MIRRORED prop does, so props keep it. See
    // DynamicMirror() and HANDOVER.md 5as.
    const bool bDynMirror = DynamicMirror();
    float ofs[3] = { 0.0f, 0.0f, 0.0f };
    pM->m_Model.GetOriginOffset(d.m_nAnim >= 0 ? (unsigned)d.m_nAnim : 0u, ofs);
    if (bDynMirror) ofs[0] = -ofs[0];

    // LT_YTRACE=1 -- where a character is drawn against where it
    // stands. "Levitating" and "sunk" are the same arithmetic with a
    // different sign, and only the numbers tell them apart.
    if (getenv("LT_YTRACE"))
    {
        static std::set<std::string> seen;
        if (seen.insert(d.sModel).second)
        {
            float dims[3] = { 0, 0, 0 };
            const std::vector<LTABCAnim>& aa = pM->m_Model.GetAnims();
            const size_t ia = (d.m_nAnim >= 0 && (size_t)d.m_nAnim < aa.size())
                            ? (size_t)d.m_nAnim : 0u;
            if (ia < aa.size())
                for (int k = 0; k < 3; ++k) dims[k] = aa[ia].m_Dims[k];
            // The mesh's own Y extent, bind pose: where the model
            // puts its feet relative to its origin. This is the number
            // that says whether the origin is at the feet (min ~0) or
            // at the box centre (min ~ -dims.y).
            float yLo = 3.4e38f, yHi = -3.4e38f;
            const std::vector<LTABCPiece>& pcs = pM->m_Model.GetPieces();
            for (size_t ip = 0; ip < pcs.size(); ++ip)
                if (!pcs[ip].m_Lods.empty())
                {
                    const std::vector<LTABCVertex>& vv = pcs[ip].m_Lods[0].m_Verts;
                    for (size_t iv = 0; iv < vv.size(); ++iv)
                    {
                        const float y = vv[iv].m_Pos[1];
                        if (y < yLo) yLo = y;
                        if (y > yHi) yHi = y;
                    }
                }

            printf("[YTRACE] %-40s pos.y %8.1f  origin ofs (%.1f %.1f %.1f)"
                   "  anim %d dims (%.1f %.1f %.1f)  mesh y %.1f..%.1f\n",
                   d.sModel.c_str(), d.m_Pos[1], ofs[0], ofs[1], ofs[2],
                   d.m_nAnim, dims[0], dims[1], dims[2], yLo, yHi);
            fflush(stdout);
        }
    }

    // ⚠️ NORMALISED BEFORE USE. A quaternion that is not unit length
    // does not just turn a model, it scales and SHEARS it -- which is
    // exactly how every NPC came out as a flat disc while the server's
    // SetupEuler was a stub. That cause is fixed; this makes the next
    // one visible as a wrong FACING instead of a mangled body, and a
    // degenerate one (zero, NaN) falls back to identity.
    // ⚠️ IS THE TRANSFORM SANE? A character drawn enormous, sheared or
    // sideways is not a model bug and not a skinning bug -- it is this
    // matrix. Scale comes from the server record and a quaternion that
    // is not unit length scales and SHEARS as well as turning, so both
    // are checked and anything out of range is named once. Silence
    // here rules the whole class out; a line names the object.
    if (getenv("LT_XFORMCHECK"))
    {
        const float l = sqrtf(d.m_Quat[0]*d.m_Quat[0] + d.m_Quat[1]*d.m_Quat[1]
                            + d.m_Quat[2]*d.m_Quat[2] + d.m_Quat[3]*d.m_Quat[3]);
        const bool bBadQ = !(l > 0.99f && l < 1.01f);
        bool bBadS = false;
        for (int k = 0; k < 3; ++k)
            if (!(d.m_Scale[k] > 0.2f && d.m_Scale[k] < 5.0f)) bBadS = true;

        if (bBadQ || bBadS)
        {
            static std::set<std::string> seenX;
            char szX[400];
            snprintf(szX, sizeof(szX), "%s|%d%d", d.sModel.c_str(),
                     (int)bBadQ, (int)bBadS);
            if (seenX.insert(szX).second)
                printf("[XFORM] %-40s at (%.0f %.0f %.0f)  scale (%.3f %.3f %.3f)%s"
                       "  |q| %.4f%s\n", d.sModel.c_str(),
                       d.m_Pos[0], d.m_Pos[1], d.m_Pos[2],
                       d.m_Scale[0], d.m_Scale[1], d.m_Scale[2],
                       bBadS ? "  ⚠️  SCALE" : "", l,
                       bBadQ ? "  ⚠️  NOT UNIT -- this shears the model" : "");
        }
    }

    float q[4] = { d.m_Quat[0], d.m_Quat[1], d.m_Quat[2], d.m_Quat[3] };
    {
        const float l = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        if (!(l > 1e-6f) || l != l) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; }
        else for (int k = 0; k < 4; ++k) q[k] /= l;
    }

    float World[16], Place[16], ObjMVP[16];
    LT3D::FromQuatPos(q, d.m_Pos, World);
    memcpy(Place, World, sizeof(Place));
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            Place[r*4 + c] *= (d.m_Scale[c] > 0.0f ? d.m_Scale[c] : 1.0f);

    if (ofs[0] != 0.0f || ofs[1] != 0.0f || ofs[2] != 0.0f)
    {
        float dd[3];
        for (int r = 0; r < 3; ++r)
            dd[r] = Place[r*4+0]*ofs[0] + Place[r*4+1]*ofs[1] + Place[r*4+2]*ofs[2];
        const float sg = LTOriginOffsetSign();      // the donor adds it (5bt)
        for (int r = 0; r < 3; ++r) { Place[r*4+3] += sg*dd[r]; World[r*4+3] += sg*dd[r]; }
    }

    LT3D::Multiply(MVP, Place, ObjMVP);

    // ⚠️ THE POSE, by time. Keyframe times are milliseconds from the
    // start of the animation; a looping animation wraps on the last
    // key's time, a one-shot holds its last frame -- which is what a
    // character that has finished `walkstop` should look like.
    LTModelPose pose;
    const LTModelPose* pPose = 0;
    const std::vector<LTABCAnim>& anims = pM->m_Model.GetAnims();
    if (d.m_nAnim >= 0 && (size_t)d.m_nAnim < anims.size()
        && !anims[d.m_nAnim].m_Keyframes.empty())
    {
        const LTABCAnim& a = anims[d.m_nAnim];
        const unsigned int nLen = a.m_Keyframes.back().m_nTime;
        unsigned int t = (unsigned int)(d.m_fAnimTime * 1000.0f);
        if (nLen > 0) t = d.m_bLooping ? (t % nLen) : (t > nLen ? nLen : t);

        unsigned int kf = 0;
        for (size_t k = 0; k < a.m_Keyframes.size(); ++k)
            if (a.m_Keyframes[k].m_nTime <= t) kf = (unsigned int)k;

        // ⚠️ WITH THE GAME'S NODE CONTROL when it installed one --
        // the jaw of a speaking character, a head following a speaker.
        // LT_NONODECONTROL=1 ignores it, for an A/B.
        static const bool s_bNoNC = (getenv("LT_NONODECONTROL") != 0);
        NodeTramp tr; tr.fn = d.m_pfnNode; tr.hObj = d.m_hNodeObj; tr.user = d.m_pNodeUser;
        const bool bNC = (d.m_pfnNode != 0) && !s_bNoNC;

        // The extra trackers -- the blink -- each at its own time.
        std::vector<LTModelDraw::Layer> layers;
        for (size_t l = 0; l < d.m_Layers.size(); ++l)
        {
            const LTDynamicLayer& L = d.m_Layers[l];
            if (L.m_nAnim < 0 || (size_t)L.m_nAnim >= anims.size()) continue;
            LTModelDraw::Layer y;
            y.m_nAnim      = (unsigned)L.m_nAnim;
            y.m_nKeyframe  = LTModelDraw::KeyframeAt(pM->m_Model, (unsigned)L.m_nAnim,
                                                     L.m_fAnimTime, L.m_bLooping);
            y.m_nWeightSet = L.m_nWeightSet;
            layers.push_back(y);
        }

        if (LTModelDraw::BuildPose(pM->m_Model, (unsigned)d.m_nAnim, kf, pose,
                                   bNC ? NodeTrampoline : 0, bNC ? &tr : 0,
                                   layers.empty() ? 0 : &layers[0],
                                   (unsigned)layers.size()))
            pPose = &pose;
    }

    // ⚠️ THE SAME QUESTION AGAIN, BUT FOR THE POSED MESH. The bind pose
    // is centred on the origin (mesh y -53..53 against dims.y 53), so
    // a character standing at floor+53 has its feet on the floor. If a
    // POSED character sits higher than that, the pose is what lifts it
    // -- which is what "levitating about a metre" would be.
    if (pPose && getenv("LT_YTRACE"))
    {
        static std::set<std::string> seenPosed;
        char szKey[512];
        snprintf(szKey, sizeof(szKey), "%s#%d", d.sModel.c_str(), d.m_nAnim);
        if (seenPosed.insert(szKey).second)
        {
            float yLo = 3.4e38f, yHi = -3.4e38f;
            const std::vector<LTABCPiece>& pcs = pM->m_Model.GetPieces();
            for (size_t ip = 0; ip < pcs.size(); ++ip)
                if (!pcs[ip].m_Lods.empty())
                {
                    const std::vector<LTABCVertex>& vv = pcs[ip].m_Lods[0].m_Verts;
                    for (size_t iv = 0; iv < vv.size(); ++iv)
                    {
                        // The renderer's own rule, so this trace can
                        // never disagree with what is drawn.
                        float p3[3];
                        if (!LTModelDraw::SkinPosition(pM->m_Model, vv[iv], *pPose, p3))
                            continue;
                        const float y = p3[1];
                        if (y < yLo) yLo = y;
                        if (y > yHi) yHi = y;
                    }
                }
            printf("[YTRACE] POSED %-34s anim %3d  posed mesh y %.1f..%.1f"
                   "  (feet land at %.1f, floor should be %.1f)\n",
                   d.sModel.c_str(), d.m_nAnim, yLo, yHi,
                   d.m_Pos[1] + yLo, d.m_Pos[1] - 53.0f);
            fflush(stdout);
        }
    }


    // ⚠️ THE LEVEL'S LIGHT, sampled at this object -- see
    // BuildModelLights. LT_LIGHTTRACE=<substr> prints what one model
    // received, once per distinct position bucket.
    LTModelDraw::LTModelLighting ML;
    bool bML = !OldModelLight();
    if (bML)
    {
        bool bTrace = false;
        if (const char* pLT = getenv("LT_LIGHTTRACE"))
        {
            std::string a = d.sModel, b = pLT;
            for (size_t q = 0; q < a.size(); ++q) a[q] = (char)toupper((unsigned char)a[q]);
            for (size_t q = 0; q < b.size(); ++q) b[q] = (char)toupper((unsigned char)b[q]);
            if (a.find(b) != std::string::npos)
            {
                static std::set<std::string> seenL;
                char szK[256];
                snprintf(szK, sizeof(szK), "%s|%d|%d|%d", d.sModel.c_str(),
                         (int)(d.m_Pos[0] / 16), (int)(d.m_Pos[1] / 16), (int)(d.m_Pos[2] / 16));
                bTrace = seenL.insert(szK).second;
            }
        }
        if (d.m_bNoLight && LTNoLightEnabled())
        {
            // FLAG_NOLIGHT: the object's own colour, nothing from the level.
            for (int k = 0; k < 3; ++k) ML.m_Ambient[k] = d.m_Colour[k];
            ML.m_nLights = 0;
        }
        else
            ComputeModelLighting(d.m_Pos, ML, bTrace);
    }

    // ⚠️ LT_ANIMTRACE=<substr> -- WHICH ANIMATION AND KEYFRAME a model is
    // drawn in, every frame. A pose that is wrong in game and right in
    // modelview is almost always a different animation; this is what
    // makes the game's pose reproducible offline.
    if (const char* pAT = getenv("LT_ANIMTRACE"))
    {
        std::string a = d.sModel, b = pAT;
        for (size_t q = 0; q < a.size(); ++q) a[q] = (char)toupper((unsigned char)a[q]);
        for (size_t q = 0; q < b.size(); ++q) b[q] = (char)toupper((unsigned char)b[q]);
        if (a.find(b) != std::string::npos)
        {
            int kfShown = -1; unsigned tShown = 0;
            if (d.m_nAnim >= 0 && (size_t)d.m_nAnim < anims.size()
                && !anims[d.m_nAnim].m_Keyframes.empty())
            {
                const LTABCAnim& a2 = anims[d.m_nAnim];
                const unsigned nLen = a2.m_Keyframes.back().m_nTime;
                unsigned tt = (unsigned)(d.m_fAnimTime * 1000.0f);
                if (nLen > 0) tt = d.m_bLooping ? (tt % nLen) : (tt > nLen ? nLen : tt);
                tShown = tt;
                for (size_t k = 0; k < a2.m_Keyframes.size(); ++k)
                    if (a2.m_Keyframes[k].m_nTime <= tt) kfShown = (int)k;
            }
            printf("[ANIM] %-34s anim %3d %-20s t=%6u ms  kf %3d of %3u  %s\n",
                   d.sModel.c_str(), d.m_nAnim,
                   (d.m_nAnim >= 0 && (size_t)d.m_nAnim < anims.size())
                       ? anims[d.m_nAnim].m_sName.c_str() : "?",
                   tShown, kfShown,
                   (d.m_nAnim >= 0 && (size_t)d.m_nAnim < anims.size())
                       ? (unsigned)anims[d.m_nAnim].m_Keyframes.size() : 0u,
                   pPose ? "posed" : "BIND (no pose)");
            // ...and every extra tracker blended onto it.
            const std::vector<LTABCWeightSet>& ws = pM->m_Model.GetWeightSets();
            for (size_t l = 0; l < d.m_Layers.size(); ++l)
            {
                const LTDynamicLayer& L = d.m_Layers[l];
                const bool bA = L.m_nAnim >= 0 && (size_t)L.m_nAnim < anims.size();
                const bool bW = L.m_nWeightSet >= 0 && (size_t)L.m_nWeightSet < ws.size();
                printf("[ANIM]   + layer %u anim %3d %-12s t=%7.0f ms kf %3u  weightset %d %s\n",
                       (unsigned)l, L.m_nAnim, bA ? anims[L.m_nAnim].m_sName.c_str() : "?",
                       L.m_fAnimTime * 1000.0f,
                       bA ? LTModelDraw::KeyframeAt(pM->m_Model, (unsigned)L.m_nAnim,
                                                    L.m_fAnimTime, L.m_bLooping) : 0u,
                       L.m_nWeightSet, bW ? ws[L.m_nWeightSet].m_sName.c_str() : "?");
            }
        }
    }

    out.m_pModel  = pM;
    out.m_bMirror = bDynMirror;
    memcpy(out.m_ObjMVP, ObjMVP, sizeof(out.m_ObjMVP));
    memcpy(out.m_World, World, sizeof(out.m_World));
    out.m_bPose = (pPose != 0);
    if (pPose) out.m_Pose = pose;
    out.m_bLit = bML;
    if (bML) out.m_Light = ML;
    return true;
}


CLTWorldScene::Stats CLTWorldScene::Render(unsigned char* pRGBA, float* pDepth,
                                           int nWidth, int nHeight,
                                           const float* pEye3, const float* pAt3,
                                           float fFovY,
                                           const ILTWorldModelXform* pXform,
                                           const ILTWorldDynamicObjects* pDyn)
{
    Stats st;
    st.m_nWorldTris = st.m_nObjTris = st.m_nPixels = st.m_nSkyPixels = st.m_nObjects = 0;
    st.m_nDynamic = 0;
    if (!m_bLoaded || !pRGBA || !pDepth) return st;

    LT3DTarget t;
    t.m_pRGBA = pRGBA; t.m_pDepth = pDepth;
    t.m_Width = nWidth; t.m_Height = nHeight;

    // ⚠️ LT_UVPROBE -- see LTWorldScene_ProbeDump. Held at id 0 by default, so
    // world geometry and every other model stamp "not the one I asked for".
    if (ProbeOn())
    {
        const size_t n = (size_t)nWidth * nHeight;
        s_nProbeW = nWidth; s_nProbeH = nHeight;
        s_ProbeID.assign(n, 0u);
        s_ProbeTex.assign(n * 4, 0);
        s_ProbeUV.assign(n * 2, 0.0f);
        s_ProbeMip.assign(n, 255);
        t.m_pProbeID  = &s_ProbeID[0];
        t.m_pProbeTex = &s_ProbeTex[0];
        t.m_pProbeUV  = &s_ProbeUV[0];
        t.m_pProbeMip = &s_ProbeMip[0];
        LT3D::g_nProbeID   = 0;
        LT3D::g_bProbeHold = true;
    }

    // ⚠️ WORLD GEOMETRY SHADED AFFINELY UNTIL 2026-09-14, AND NOW DOES NOT.
    //
    // The affine choice was made to kill a corridor-floor stipple, measured at
    // the time as 13.63 high-frequency energy against 20.11 perspective-
    // correct -- a 47% penalty for doing it the ordinary way. The mip chain,
    // trilinear filtering and anisotropy arrived afterwards and are what that
    // stipple was really about: re-measured on the same view it is now 12.21
    // against 13.32, a 9% penalty.
    //
    // ⚠️ AND THE COST WAS NOT 9%. Affine interpolation does not survive the
    // NEAR-PLANE CUT identically on a GPU, so the two backends disagreed by up
    // to 24.77 mean absolute -- M03S01's ceiling came out saturated ORANGE
    // under Metal and beige in software, on the same geometry with the same
    // texture. Perspective-correct colour drops that to 0.35 and is equal or
    // better in 8 of the 9 worlds measured. See HANDOVER §5i.
    //
    // LT_AFFINECOLOUR=1 restores the old behaviour for a side-by-side.
    t.m_bAffineColour = (getenv("LT_AFFINECOLOUR") != 0);

    // ⚠️ And it is filtered. See LT3DTarget::m_bBilinear -- level textures are
    // magnified whenever a wall is close, and nearest sampling speckles them.
    // LT_NOFILTER turns it off for a side-by-side without a rebuild.
    t.m_bBilinear = (getenv("LT_NOFILTER") == 0);

    // ⚠️ And it is anisotropic. See LT3DTarget::m_nMaxAniso -- a corridor is
    // mostly walls seen at a grazing angle, which is the exact case isotropic
    // mip selection blurs away. LT_ANISO overrides; 1 disables.
    // ⚠️ And it blends between mip levels. See LT3DTarget::m_bTrilinear -- the
    // nearest-level seams are bands across a receding floor that SWIM as you
    // walk. LT_NOTRILINEAR turns it off for a side-by-side.
    t.m_bTrilinear = (getenv("LT_NOTRILINEAR") == 0);

    t.m_nMaxAniso = getenv("LT_ANISO") ? atoi(getenv("LT_ANISO")) : 4;
    if (t.m_nMaxAniso < 1)  t.m_nMaxAniso = 1;
    if (t.m_nMaxAniso > 16) t.m_nMaxAniso = 16;

    const float up[3] = { 0, 1, 0 };
    const float fAspect = (float)nWidth / (float)(nHeight ? nHeight : 1);

    LT3D::ClearColor(t, 0x0E1116);
    LT3D::ClearDepth(t);

    const std::vector<LTWorldModelHeader>& mdl = m_World.GetModels();

    // ⚠️ RANGE FOG (5ch): the view ray's tangent per pixel, from the
    // projection every pass below shares (the sky's differs only in its
    // near/far, which do not enter P00/P11).
    {
        float Pf[16];
        LT3D::Perspective(fFovY, fAspect, 4.0f, 100.0f, Pf);
        const bool bRange = LTFogRange() && t.m_Width > 0 && t.m_Height > 0
                         && Pf[0] != 0.0f && Pf[5] != 0.0f;
        t.m_fFogKX = bRange ? 2.0f / ((float)t.m_Width  * fabsf(Pf[0])) : 0.0f;
        t.m_fFogKY = bRange ? 2.0f / ((float)t.m_Height * fabsf(Pf[5])) : 0.0f;
    }

    // ---- pass 1: the sky ------------------------------------------------ //
    //
    // ⚠️ THE SKY DOES NOT TRANSLATE WITH THE CAMERA. It is drawn from the sky
    // object's own centre using the camera's ORIENTATION only, which is what
    // puts it at infinity: walk down a corridor and the sky must not slide
    // past the doorway. Then the depth buffer is CLEARED, so world geometry
    // always wins and the sky survives only where nothing was drawn -- exactly
    // the holes the sky-portal surfaces leave behind.
    if (m_pSky && m_pSky->m_bBodyParsed && LTSkyEnabled())
    {
        float smn[3] = { 1e30f, 1e30f, 1e30f }, smx[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t p = 0; p < m_pSky->m_Points.size(); ++p)
            for (int k = 0; k < 3; ++k)
            {
                float v = m_pSky->m_Points[p].m_Pos[k];
                if (v < smn[k]) smn[k] = v;
                if (v > smx[k]) smx[k] = v;
            }

        float sc[3], sr = 0.0f;
        for (int k = 0; k < 3; ++k)
        { sc[k] = (smn[k] + smx[k]) * 0.5f; float d = (smx[k] - smn[k]) * 0.5f; sr += d * d; }
        sr = sqrtf(sr);

        float dir[3] = { pAt3[0] - pEye3[0], pAt3[1] - pEye3[1], pAt3[2] - pEye3[2] };
        float sat[3] = { sc[0] + dir[0], sc[1] + dir[1], sc[2] + dir[2] };

        float SP[16], SV[16], SMVP[16];
        LT3D::Perspective(fFovY, fAspect, sr * 0.001f + 0.1f, sr * 4.0f, SP);
        if (MirrorWorld()) for (int k = 0; k < 4; ++k) SP[k] = -SP[k];
        LT3D::LookAt(sc, sat, up, SV);
        LT3D::Multiply(SP, SV, SMVP);

        std::vector<const LT3DTexture*> stex(m_pSky->m_Textures.size(), (const LT3DTexture*)0);
        for (size_t k = 0; k < m_pSky->m_Textures.size(); ++k)
            stex[k] = GetTexture(m_pSky->m_Textures[k]);

        // The sky fogs with ITS OWN distances (donor d3d_drawsky.cpp sets
        // D3DRS_FOGSTART/END from SkyFogNearZ/FarZ around the sky draw),
        // measured in the sky box's own units.
        t.m_bFog = m_FogSky.m_bOn;
        t.m_fFogNear = m_FogSky.m_fNear; t.m_fFogFar = m_FogSky.m_fFar;
        for (int k = 0; k < 3; ++k) t.m_FogRGB[k] = m_FogSky.m_RGB[k];
        DrawModel(t, SMVP, *m_pSky, stex, st);
        t.m_bFog = false;
        LT3D::ClearDepth(t);
    }

    // Everything after the sky -- world, objects, sprites -- fogs with the
    // world's distances.
    t.m_bFog = m_FogWorld.m_bOn;
    t.m_fFogNear = m_FogWorld.m_fNear; t.m_fFogFar = m_FogWorld.m_fFar;
    for (int k = 0; k < 3; ++k) t.m_FogRGB[k] = m_FogWorld.m_RGB[k];
    // ...and clip at FarZ, as the donor's frustum does (lt3d.h m_fFarW).
    t.m_fFarW = m_fFarZ;

    // ---- the camera ----------------------------------------------------- //
    //
    // ⚠️ A near plane scaled to the whole level would clip the wall you are
    // standing next to. Standing inside, it has to be small.
    float P[16], V[16], MVP[16];
    LT3D::Perspective(fFovY, fAspect, 4.0f, WorldFar(), P);
    if (MirrorWorld()) for (int k = 0; k < 4; ++k) P[k] = -P[k];
    LT3D::LookAt(pEye3, pAt3, up, V);
    LT3D::Multiply(P, V, MVP);

    // ⚠️ THE CAMERA BASIS, BUILT THE SAME WAY LT3D::LookAt BUILDS IT -- forward
    // normalised, right = forward x up, up' = right x forward. A sprite is a
    // quad in WORLD space that happens to face the camera, so it needs these
    // three vectors and not the view matrix.
    // ⚠️ The separate-world-model depth nudge. See LTBuildModelDepthBias.
    float Bias[16], MVPBias[16];
    LTBuildModelDepthBias(pEye3, Bias);
    LT3D::Multiply(MVP, Bias, MVPBias);

    float camF[3], camR[3], camU[3];
    {
        for (int k = 0; k < 3; ++k) camF[k] = pAt3[k] - pEye3[k];
        float l = sqrtf(camF[0]*camF[0] + camF[1]*camF[1] + camF[2]*camF[2]);
        if (l > 0.0f) for (int k = 0; k < 3; ++k) camF[k] /= l;

        camR[0] = camF[1]*up[2] - camF[2]*up[1];
        camR[1] = camF[2]*up[0] - camF[0]*up[2];
        camR[2] = camF[0]*up[1] - camF[1]*up[0];
        l = sqrtf(camR[0]*camR[0] + camR[1]*camR[1] + camR[2]*camR[2]);
        if (l > 0.0f) for (int k = 0; k < 3; ++k) camR[k] /= l;

        camU[0] = camR[1]*camF[2] - camR[2]*camF[1];
        camU[1] = camR[2]*camF[0] - camR[0]*camF[2];
        camU[2] = camR[0]*camF[1] - camR[1]*camF[0];
    }

    // ---- pass 2: the world ---------------------------------------------- //
    for (size_t i = 0; i < mdl.size(); ++i)
    {
        if (&mdl[i] == m_pSky) continue;        // drawn already, at infinity
        if (!Drawable(mdl[i], pXform)) continue;
        // ⚠️ Translucent by its OBJECT's alpha (5cf): pass 4 draws all of it.
        { float a; if (ModelObjectAlpha(mdl[i], pXform, &a)) continue; }

        const LTWorldModelHeader& M = mdl[i];
        std::vector<const LT3DTexture*> tex(M.m_Textures.size(), (const LT3DTexture*)0);
        for (size_t k = 0; k < M.m_Textures.size(); ++k)
            tex[k] = GetTexture(M.m_Textures[k]);

        // ⚠️ Everything EXCEPT the main BSP is pulled a hair toward the eye, so
        // a door or a window flush against the wall it sits in wins the tie
        // instead of flickering. See LTBuildModelDepthBias.
        const float* pVP = LTIsMainWorldBSP(M) ? MVP : MVPBias;

        // ⚠️ A moving brush gets its own matrix folded in. Static geometry --
        // almost everything -- takes the same path it always did, so nothing
        // pays for doors existing.
        float Local[16], ModelMVP[16];
        if (pXform && pXform->GetModelTransform(M.m_sName, Local))
        {
            LT3D::Multiply(pVP, Local, ModelMVP);
            DrawModel(t, ModelMVP, M, tex, st);
        }
        else
        {
            // ⚠️ FRONT TO BACK, AND THIS IS THE OPTIMISATION THAT THE FRUSTUM
            // CULLING SHOULD HAVE BEEN. Measured on M01S01 at 640x480: triangles
            // cover 4.76x the screen, of which 2.41x is actually SHADED -- so
            // 59% of all shading is spent on pixels that a later polygon
            // overwrites. Drawing nearest-first makes those pixels fail the
            // depth test instead, and the depth test sits BEFORE the divide,
            // the UV recovery and the texture taps.
            //
            // ⚠️ Why frustum culling bought nothing, for the record: the frame
            // is 5.6 ms of fixed cost plus ~86 ms per megapixel, so at 640x480
            // it is 82% per-PIXEL work. Culling attacks the other 18%, and
            // worse, off-screen triangles were ALREADY nearly free -- their
            // screen bounding box is clamped to the viewport and comes back
            // empty. It was optimising something that did not cost anything.
            //
            // Sorting is by the polygon's own stored centre, so there is no
            // geometry to recompute -- only a distance and a sort.
            DrawModel(t, pVP, M, tex, st, BuildOrder(M, pEye3));
        }
    }

    // ---- pass 3: the objects -------------------------------------------- //
    //
    // ⚠️ The world file names both halves: an object carries `Filename` (an
    // .abc) and `Skin` (a .dtx) as ordinary string properties. Nothing is
    // inferred from a class name.
    //
    // ⚠️ `Filename` is not always a model -- objects reuse the property for
    // sounds and textures -- so the extension decides.
    //
    // ⚠️ What is NOT here: characters. Cate and the AI are spawned by GAME
    // CODE, not by a world property, so no amount of reading the .dat places
    // them. An empty-looking level is the correct result for this reader.
    //
    // ⚠️ RUN TWICE -- ONCE HERE AND ONCE AFTER PASS 4. A model whose object
    // `Alpha` is below 1.0 is translucent and must be composited last, for the
    // same reason a SURF_TRANSPARENT brush must: it writes no depth. This is a
    // LAMBDA rather than a copied loop because every placement rule in it --
    // Visible, Scale, MoveToFloor, the origin offset, being carried by a door
    // -- has already cost this port a bug apiece, and a second copy is a
    // second place to forget one.
    auto drawObjectPass = [&](bool bTranslucentPass)
    {
        if (!LTObjectsEnabled()) return;
        const std::vector<LTWorldObject>& obj = m_World.GetObjects();

        // ⚠️ BACK TO FRONT when compositing, natural order otherwise. Same
        // argument as BuildBackToFrontOrder: for opaque geometry the order is
        // an optimisation, here it decides the colour.
        std::vector<std::pair<float, int> > sorted;
        std::vector<int> order;
        order.reserve(obj.size());
        if (bTranslucentPass)
        {
            for (size_t q = 0; q < obj.size(); ++q)
            {
                float p3[3];
                if (!obj[q].GetPos(p3)) { continue; }
                const float dx = p3[0] - pEye3[0];
                const float dy = p3[1] - pEye3[1];
                const float dz = p3[2] - pEye3[2];
                sorted.push_back(std::make_pair(-(dx*dx + dy*dy + dz*dz), (int)q));
            }
            std::sort(sorted.begin(), sorted.end());
            for (size_t q = 0; q < sorted.size(); ++q) order.push_back(sorted[q].second);
        }
        else
        {
            for (size_t q = 0; q < obj.size(); ++q) order.push_back((int)q);
        }

        for (size_t oi = 0; oi < order.size(); ++oi)
        {
            const size_t i = (size_t)order[oi];

            // ⚠️ SPRITES FIRST, because they are not models and never reach
            // ResolveObjectModel -- it demands an `.abc`. They are ALWAYS
            // composited, so they belong to this pass and to no other.
            {
                LTSpriteDraw sd;
                if (ResolveObjectSprite(i, sd))
                {
                    if (!bTranslucentPass) continue;
                    float spos[3];
                    if (!obj[i].GetPos(spos)) continue;
                    DrawSpriteQuad(t, MVP, sd, spos, camR, camU, camF, pEye3, st, 0);
                    ++st.m_nObjects;
                    continue;
                }
            }

            std::string sModelName, sSkinName;
            bool  bCK      = false;
            float fAlpha   = 1.0f;
            int   nBlendOp = 0;
            if (!ResolveObjectModel(i, sModelName, sSkinName, &bCK, &fAlpha, &nBlendOp))
                continue;
            if (LTIsTranslucentDraw(fAlpha, nBlendOp) != bTranslucentPass) continue;
            if (HiddenNow(i)) continue;     // hidden or removed since the level began

            // ⚠️ AN OBJECT CAN ASK NOT TO BE DRAWN, and two of M01S01's do --
            // both candle holders, which is why one turned up embedded in a
            // wall. The game hides them; nothing here was reading the flag.
            const LTWorldProperty* pV = obj[i].Find("Visible");
            if (pV && !pV->m_bBool) continue;

            float pos[3], eul[3] = { 0, 0, 0 };
            if (!obj[i].GetPos(pos)) continue;
            obj[i].GetEuler(eul);
            eul[1] += LTPropYawOffset();      // a knob, 0 by default -- see there

            // ⚠️ WHERE THE GAME HAS IT, when the server knows the object
            // (HANDOVER 5bt) -- its own MoveToFloor, anything it moved since.
            // Otherwise MoveToFloor resolved here at load.
            float livePos[3], liveQuat[4];
            const bool bLive = LTWorldScene_LivePose(i, livePos, liveQuat);
            // LT_LIVEPOSETRACE=1 -- once per object, where the live placement
            // is not the file's plus a pure drop (says so on first use).
            if (bLive && getenv("LT_LIVEPOSETRACE"))
            {
                static std::set<size_t> s_Said;
                const float dx = livePos[0] - pos[0], dz = livePos[2] - pos[2], dy = livePos[1] - pos[1];
                if ((fabsf(dx) > 0.5f || fabsf(dz) > 0.5f || dy > 0.5f) && s_Said.insert(i).second)
                    printf("[LIVEPOSE] %-24s %-14s file (%.1f %.1f %.1f) live (%.1f %.1f %.1f)\n",
                           obj[i].m_sType.c_str(), obj[i].GetName(), pos[0], pos[1], pos[2],
                           livePos[0], livePos[1], livePos[2]);
            }
            if (bLive) { for (int k = 0; k < 3; ++k) pos[k] = livePos[k]; }
            else if (i < m_ObjectDrop.size()) pos[1] += m_ObjectDrop[i];

            // ⚠️ AND IT CAN ASK TO BE A DIFFERENT SIZE. 42 of 288 drawable
            // objects across three worlds carry a non-unit Scale -- trees at
            // 1.5, pillows at 2 -- and drawing them all at 1.0 puts props
            // through the walls they are meant to stand against.
            float scale[3];
            ObjectScale(i, scale);
            const bool bScaled = (scale[0] != 1.0f || scale[1] != 1.0f || scale[2] != 1.0f);

            CachedModel* pM = GetModel(sModelName, sSkinName, bCK, fAlpha, nBlendOp);
            if (!pM) continue;

            // ⚠️ TWO MATRICES, and the difference is the scale. `World` is the
            // rotation alone and feeds the NORMAL matrix -- scaling normals
            // changes how a scaled prop is lit for no reason. `Place` carries
            // the scale and positions the geometry.
            // ⚠️ AND THE MODEL CAN DECLARE ITS OWN ORIGIN OFFSET, which is
            // NOT the same thing as its Pos. The AnimBinding beside the dims
            // carries the vector from the mesh origin to the centre of the box
            // the model declares for itself, and `Pos` addresses that centre --
            // so the mesh has to be shifted by MINUS the offset.
            //
            // ⚠️ It is applied in MODEL space, before the rotation, which is
            // why it goes in through the position rather than after: two knobs
            // authored at the same point on opposite faces of one door carry
            // opposite yaws, so the same model-space offset must land in
            // opposite WORLD directions. Adding it to `pos` afterwards would
            // move both the same way and fix neither.
            //
            // Measured on M01S01: the two faces of HingedDoor0 put their
            // plates 9.9 units apart without this and 0.15 apart with it.
            // Doorknob_01L/R declare (-5, 0, 0); Lamp_02 and the Walther in
            // hand declare (0, 0, 0) and are unmoved -- see
            // CLTABCModel::GetOriginOffset for why those controls matter.
            float ofs[3] = { 0.0f, 0.0f, 0.0f };
            pM->m_Model.GetOriginOffset(0, ofs);

            // ⚠️ MODEL SPACE CANNOT BE HALF MIRRORED. When LTModelDraw is
            // mirroring the mesh in X, this offset -- which is a vector in
            // that same space -- has to be mirrored with it. Leaving it alone
            // fixed the lever direction and simultaneously BROKE the alignment
            // of the two plates on one door (0.1 units apart, then 19.6),
            // which is exactly how the omission was found.
            if (LTPropMirrorModel()) ofs[0] = -ofs[0];

            float World[16], Place[16], ObjMVP[16];
            if (bLive)
            {
                // The live rotation -- proven the same matrix as the Euler one
                // for an object the game never turned (5as) -- and the scale
                // on the columns, as WorldMatrix applies it.
                LT3D::FromQuatPos(liveQuat, pos, World);
                memcpy(Place, World, sizeof(Place));
                if (bScaled)
                    for (int c = 0; c < 3; ++c)
                        for (int r = 0; r < 3; ++r) Place[r*4 + c] *= scale[c];
            }
            else
            {
                WorldMatrix(pos, eul, World);
                if (bScaled) WorldMatrix(pos, eul, Place, scale);
                else         memcpy(Place, World, sizeof(Place));
            }

            // The 180-degree turn about the model's own Y -- LTApplyPropTurn.
            // Before the origin offset, which is a model-space vector too.
            LTApplyPropTurn(World);
            LTApplyPropTurn(Place);

            // Shift along the object's OWN axes: Place already holds the
            // rotation (and the scale) in its 3x3, so multiplying the offset
            // through it gives the world-space displacement.
            if (ofs[0] != 0.0f || ofs[1] != 0.0f || ofs[2] != 0.0f)
            {
                float d[3];
                for (int r = 0; r < 3; ++r)
                    d[r] = Place[r*4+0]*ofs[0] + Place[r*4+1]*ofs[1] + Place[r*4+2]*ofs[2];
                const float sg = LTOriginOffsetSign();  // the donor adds it (5bt)
                for (int r = 0; r < 3; ++r) { Place[r*4+3] += sg*d[r]; World[r*4+3] += sg*d[r]; }
            }

            // ⚠️ AN OBJECT CAN BE CARRIED BY MOVING GEOMETRY. A door handle is
            // its own object with its own fixed Pos, so without this it stays
            // hanging in the air when the door it belongs to swings away. The
            // renderer asks the same question it asks of world models -- "has
            // anything moved you?" -- and still knows nothing about doors.
            // (Not for a live-placed object: the server already moved it with
            // its door, and carrying it again would move it twice.)
            if (pXform && !bLive)
            {
                const char* pName = obj[i].GetName();
                float Carry[16];
                if (pName && *pName && pXform->GetModelTransform(pName, Carry))
                {
                    float Tmp[16];
                    LT3D::Multiply(Carry, Place, Tmp);
                    memcpy(Place, Tmp, sizeof(Place));
                    LT3D::Multiply(Carry, World, Tmp);
                    memcpy(World, Tmp, sizeof(World));
                }
            }

            LT3D::Multiply(MVP, Place, ObjMVP);

            // ⚠️ The NORMAL matrix is the object's world rotation, not the
            // full chain: lighting must not follow the camera.
            // ⚠️ bMirrorU IS FALSE, AND IT USED TO BE `LTWorldMirror()`.
            // That was wrong twice over, and the wall lantern is the proof:
            // with the flip on it renders as disconnected fragments, with it
            // off it is a lantern with a cage, a hood and a scrollwork bracket
            // that matches a screenshot of the Windows build.
            //
            // ⚠️ 1. NEGATING U MIRRORS THE WHOLE TEXTURE SHEET, which is not
            // the same thing as mirroring a polygon's own image. With the
            // sampler's wrap-by-masking, u in [0,1] negates to the mirror
            // position on the SAME sheet -- so every polygon now reads
            // whatever was packed at its mirror-image location.
            //
            // A prop skin is an ATLAS: Lamp_02's single 128x128 carries the
            // lantern's cage, its hood, its bracket and its flame as separate
            // regions. Mirroring the sheet hands each polygon its neighbour's
            // region, so the geometry stays perfect and the surfaces show the
            // wrong thing -- which is why it read as a broken MODEL.
            //
            // ⚠️ Measured, because the first explanation written here was
            // wrong: only 23 of 288 prop models have UVs outside [0,1], and
            // Lamp_02 is not one of them. The damage does not need wrapping
            // past the edge -- an atlas inside [0,1] is destroyed just as
            // thoroughly. A model only survives the flip if its texture is
            // symmetric, which is why some props looked fine for weeks.
            //
            // ⚠️ 2. IT WAS DRIVEN BY THE WRONG MIRROR. LTWorldMirror() is the
            // SCENE mirror -- a fact about the CAMERA. Texture coordinates
            // live in MODEL space. HANDOVER §5a is explicit that the two
            // mirrors are independent, and this line silently coupled them.
            //
            // ⚠️ This also settles the "bMirrorU true in the world, false in
            // the menu" asymmetry that has been recorded as unexplained: the
            // MENU was right. Both passes now agree, and the menu's reference
            // frame was blessed against a real Windows screenshot.
            //
            // LT_NOMIRRORU=1 still forces it off everywhere, as an instrument.
            // ⚠️ THROUGH THE MODEL'S REST POSE, as LithTech draws every model
            // (HANDOVER 5bt) -- it is what turns a phone and a door knob 180
            // degrees and leaves a chair as stored. See LTApplyPropTurn.
            LTModelDrawStats ms = LTModelDraw::Draw(
                t, pM->m_Model, ObjMVP, World,
                pM->m_nSkins ? pM->m_Skins : 0, pM->m_nSkins, 0, false,
                PropPose(i, pM),
                0, false, 0, LTPropMirrorModel());

            ++st.m_nObjects;
            st.m_nObjTris += ms.m_nTriangles;
            st.m_nPixels  += ms.m_nPixels;
        }
    };

    drawObjectPass(false);

    // ---- pass 3b: GAME-SPAWNED objects ---------------------------------- //
    //
    // ⚠️ See ILTWorldDynamicObjects. Before this pass existed the in-game view
    // contained no character at all -- not hidden, not culled, never
    // submitted.
    //
    // ⚠️ OPAQUE ONLY, and after the world objects so both share one depth
    // buffer: a character standing behind a desk is cut by the desk.
    //
    // ⚠️ THE PLACEMENT RULES ARE THE WORLD PASS'S, not new ones. Same
    // GetModel cache, same origin offset (mirrored with the mesh), same
    // normal matrix, same bMirrorU. The quaternion is proven identical to the
    // Euler matrix pass 3 builds, so a character is placed exactly as a prop
    // with the same numbers would be.
    if (pDyn)
    {
        const unsigned int nDyn = pDyn->Count();
        for (unsigned int i = 0; i < nDyn; ++i)
        {
            LTDynamicModel d;
            if (!pDyn->Get(i, d)) continue;

            if (d.m_bReallyClose) continue;         // pass 6, after everything
            if (LTIsTranslucentDraw(d.m_fAlpha, d.m_nBlendOp)) continue;   // pass 5
            DynamicDraw dd;
            if (!PrepareDynamic(d, MVP, dd)) continue;
            CachedModel* pM = dd.m_pModel;

            if (ProbeOn()) LT3D::g_bProbeHold = !ProbeWants(d.sModel);

            LTModelDrawStats ms = LTModelDraw::Draw(
                t, pM->m_Model, dd.m_ObjMVP, dd.m_World,
                pM->m_nSkins ? pM->m_Skins : 0, pM->m_nSkins, 0, false,
                dd.m_bPose ? &dd.m_Pose : 0,
                0, false, dd.m_bLit ? &dd.m_Light : 0, dd.m_bMirror ? 1 : 0);

            if (ProbeOn()) { LT3D::g_bProbeHold = true; LT3D::g_nProbeID = 0; }

            ++st.m_nObjects;
            ++st.m_nDynamic;
            st.m_nObjTris += ms.m_nTriangles;
            st.m_nPixels  += ms.m_nPixels;
        }
    }

    // ---- pass 4: BLENDED geometry --------------------------------------- //
    //
    // ⚠️ LAST, AND THAT IS THE WHOLE REASON IT IS A SEPARATE PASS. A blended
    // fragment writes no depth, so it cannot occlude anything -- which means
    // everything it should appear IN FRONT OF has to be on the screen already.
    // Drawn in pass 2 it would blend against whatever happened to be there at
    // the time, and the room behind a window would then paint over it.
    //
    // ⚠️ BACK TO FRONT, the opposite of pass 2. Two panes of glass in a line
    // give different colours depending on which is composited first, so the
    // far one must go down before the near one. Pass 2 sorts the other way
    // because for OPAQUE geometry the depth test makes nearest-first a pure
    // saving; here the order is a correctness requirement, not an optimisation.
    //
    // ⚠️ Sorting is PER MODEL and per polygon within it, not globally. Two
    // separate world models whose glass interpenetrates can still composite in
    // the wrong order -- a known limitation, cheap to state and expensive to
    // fix, and NOLF's windows are flat panes set into walls rather than
    // intersecting volumes.
    for (size_t i = 0; i < mdl.size(); ++i)
    {
        if (&mdl[i] == m_pSky) continue;
        if (!Drawable(mdl[i], pXform)) continue;

        const LTWorldModelHeader& M = mdl[i];

        // ⚠️ A MODEL TRANSLUCENT BY ITS OBJECT'S ALPHA (5cf) is drawn here
        // WHOLE -- every surface, by GetObjectAlphaTexture -- and at alpha 0
        // not at all: T01S02's projector beam (`ProjectorLight`, Alpha 0) is
        // an opaque near-white texture on SURF_TRANSPARENT faces and drew as a
        // solid wedge. LT_NOBRUSHALPHA=1 = drawn by its surfaces, as before.
        float fObjAlpha = 1.0f;
        const bool bObjAlpha = ModelObjectAlpha(M, pXform, &fObjAlpha);
        if (bObjAlpha && fObjAlpha <= 0.0f) continue;
        if (!bObjAlpha && !HasBlendedSurface(M)) continue;  // the overwhelming majority

        // ⚠️ The BLENDED variant of each texture -- a different cache entry
        // from the one pass 2 used. See GetTexture.
        std::vector<const LT3DTexture*> btex(M.m_Textures.size(), (const LT3DTexture*)0);
        for (size_t k = 0; k < M.m_Textures.size(); ++k)
            btex[k] = bObjAlpha ? GetObjectAlphaTexture(M.m_Textures[k], fObjAlpha)
                                : GetTexture(M.m_Textures[k], true);

        float Local[16], ModelMVP[16];
        const float* pUse = LTIsMainWorldBSP(M) ? MVP : MVPBias;
        if (pXform && pXform->GetModelTransform(M.m_sName, Local))
        {
            LT3D::Multiply(pUse, Local, ModelMVP);
            pUse = ModelMVP;
        }

        DrawModel(t, pUse, M, btex, st, BuildBackToFrontOrder(M, pEye3),
                  0, bObjAlpha ? kPassAll : kPassBlended);
    }

    // ---- pass 5: TRANSLUCENT MODELS ------------------------------------- //
    //
    // ⚠️ AFTER the blended brushes, not before. A wine glass on a table behind
    // a window has to composite onto the window, and neither writes depth, so
    // the only thing deciding which wins is the order they are submitted in.
    //
    // ⚠️ KNOWN LIMITATION, same shape as pass 4's: objects are sorted against
    // each other by distance, and blended BRUSHES are not sorted against
    // OBJECTS at all -- the brushes simply go first. NOLF puts its glass in
    // walls and its glassware on tables, so the two sets rarely overlap.
    drawObjectPass(true);

    // ...and the translucent GAME-SPAWNED models, far to near, after the
    // level's own: pass 3b's placement (PrepareDynamic), drawn with no depth.
    if (pDyn)
    {
        const std::vector<unsigned int> order = LTTranslucentDynamics(pDyn, false, pEye3);
        for (size_t q = 0; q < order.size(); ++q)
        {
            LTDynamicModel d;
            if (!pDyn->Get(order[q], d)) continue;
            DynamicDraw dd;
            if (!PrepareDynamic(d, MVP, dd)) continue;
            CachedModel* pM = dd.m_pModel;
            LTModelDrawStats ms = LTModelDraw::Draw(
                t, pM->m_Model, dd.m_ObjMVP, dd.m_World,
                pM->m_nSkins ? pM->m_Skins : 0, pM->m_nSkins, 0, false,
                dd.m_bPose ? &dd.m_Pose : 0,
                0, false, dd.m_bLit ? &dd.m_Light : 0, dd.m_bMirror ? 1 : 0);
            ++st.m_nObjects;
            ++st.m_nDynamic;
            st.m_nObjTris += ms.m_nTriangles;
            st.m_nPixels  += ms.m_nPixels;
        }

        // ...and the PolyGrids -- water surfaces -- far to near, last of the
        // translucent geometry: they are big flat sheets the rest sits in.
        const std::vector<unsigned int> pgs = LTPolyGridsBackToFront(pDyn, pEye3);
        for (size_t q = 0; q < pgs.size(); ++q)
        {
            LTDynamicPolyGrid g;
            if (!pDyn->GetPolyGrid(pgs[q], g)) continue;
            DrawPolyGrid(t, MVP, g, st, 0);
            ++st.m_nObjects;
        }
    }

    // ---- pass 5b: the game's run-time SPRITES (muzzle flash, impacts) ---- //
    // Back to front, after everything opaque and translucent, by the world
    // sprites' own rule. REALLYCLOSE ones wait for pass 6.
    if (pDyn && pDyn->SpriteCount())
    {
        std::vector<std::pair<float, LTDynamicSprite> > v;
        for (unsigned int i = 0; i < pDyn->SpriteCount(); ++i)
        {
            LTDynamicSprite sp;
            if (!pDyn->GetSprite(i, sp) || sp.m_bReallyClose) continue;
            const float dx = sp.m_Pos[0]-pEye3[0], dy = sp.m_Pos[1]-pEye3[1], dz = sp.m_Pos[2]-pEye3[2];
            v.push_back(std::make_pair(-(dx*dx + dy*dy + dz*dz), sp));
        }
        std::sort(v.begin(), v.end(),
                  [](const std::pair<float, LTDynamicSprite>& a,
                     const std::pair<float, LTDynamicSprite>& b) { return a.first < b.first; });
        for (size_t i = 0; i < v.size(); ++i)
        {
            DrawSpriteQuad(t, MVP, v[i].second.m_Draw, v[i].second.m_Pos, camR, camU, camF, pEye3, st, 0);
            ++st.m_nObjects;
        }
    }

    // Depth was cleared between the sky and the world, so anything still at
    // zero is a pixel the world never covered -- which is where the sky shows.
    for (int i = 0; i < nWidth * nHeight; ++i)
        if (pDepth[i] == 0.0f) ++st.m_nSkyPixels;

    // ---- pass 6: FLAG_REALLYCLOSE -- the player-view weapon -------------- //
    //
    // ⚠️ LAST, OVER EVERYTHING, with the depth cleared: LithTech's contract
    // for the flag (ltbasedefs.h:1106) -- the gun is never cut by the wall
    // the player is pressed against. And with its OWN near plane: the world's
    // is 4 units, and a view weapon sits closer than that to the eye.
    // Same x/y projection as the world, so it lines up with what it fires at.
    t.m_fFarW = 0.0f;       // the view weapon is never far-clipped
    if (pDyn)
    {
        float PC[16], MVPC[16];
        LT3D::Perspective(fFovY, fAspect, LTReallyCloseNear(), 40000.0f, PC);
        if (MirrorWorld()) for (int k = 0; k < 4; ++k) PC[k] = -PC[k];
        LT3D::Multiply(PC, V, MVPC);
        bool bCleared = false;
        auto drawClose = [&](const LTDynamicModel& d)
        {
            DynamicDraw dd;
            if (!PrepareDynamic(d, MVPC, dd)) return;
            if (!bCleared) { LT3D::ClearDepth(t); bCleared = true; }
            CachedModel* pM = dd.m_pModel;
            LTModelDrawStats ms = LTModelDraw::Draw(
                t, pM->m_Model, dd.m_ObjMVP, dd.m_World,
                pM->m_nSkins ? pM->m_Skins : 0, pM->m_nSkins, 0, false,
                dd.m_bPose ? &dd.m_Pose : 0,
                0, false, dd.m_bLit ? &dd.m_Light : 0, dd.m_bMirror ? 1 : 0);
            ++st.m_nObjects;
            ++st.m_nDynamic;
            st.m_nObjTris += ms.m_nTriangles;
            st.m_nPixels  += ms.m_nPixels;
        };
        // ⚠️ OPAQUE FIRST, then the translucent ones far to near -- the rule
        // of passes 3b and 5, inside the cleared depth. The intel popup is
        // the case that needs it: its multiply backdrop (5 units out) must
        // darken the scene BEHIND the letter (0.215 out), not the letter.
        const unsigned int nDyn = pDyn->Count();
        for (unsigned int i = 0; i < nDyn; ++i)
        {
            LTDynamicModel d;
            if (!pDyn->Get(i, d) || !d.m_bReallyClose) continue;
            if (LTIsTranslucentDraw(d.m_fAlpha, d.m_nBlendOp)) continue;
            drawClose(d);
        }
        const std::vector<unsigned int> order = LTTranslucentDynamics(pDyn, true, pEye3);
        for (size_t q = 0; q < order.size(); ++q)
        {
            LTDynamicModel d;
            if (pDyn->Get(order[q], d)) drawClose(d);
        }
        // ...and the REALLYCLOSE sprites -- the muzzle flash rides on the gun.
        for (unsigned int i = 0; i < pDyn->SpriteCount(); ++i)
        {
            LTDynamicSprite sp;
            if (!pDyn->GetSprite(i, sp) || !sp.m_bReallyClose) continue;
            if (!bCleared) { LT3D::ClearDepth(t); bCleared = true; }
            DrawSpriteQuad(t, MVPC, sp.m_Draw, sp.m_Pos, camR, camU, camF, pEye3, st, 0);
            ++st.m_nObjects;
        }
    }

    if (ProbeOn())
    {
        s_ProbeFinal.assign(pRGBA, pRGBA + (size_t)nWidth * nHeight * 4);
        LT3D::g_bProbeHold = false;         // leave modelview-style callers alone
        LT3D::g_nProbeID   = 0;
    }

    return st;
}



// ---- GPU mesh lifetime (HANDOVER 5cd) ---------------------------------------
namespace { void (*s_pfnFreeGpuMesh)(unsigned int) = 0; }

void CLTWorldScene::SetGpuMeshFree(void (*pfn)(unsigned int)) { s_pfnFreeGpuMesh = pfn; }

CLTWorldScene::CachedModel::~CachedModel()
{
    if (m_nGpuMesh != 0 && m_nGpuMesh != ~0u && s_pfnFreeGpuMesh) s_pfnFreeGpuMesh(m_nGpuMesh);
}
