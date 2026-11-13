// ----------------------------------------------------------------------- //
//
// MODULE  : ltworldscene.cpp
//
// See ltworldscene.h for why the world is owned here rather than by whoever
// happened to load it first.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "ltworldscene.h"
#include "ltworldrender.h"
#include "ltrecord.h"
#include "iltlightanim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>

namespace
{
    // ⚠️ FUNCTION-LOCAL STATICS, so they are CONSTRUCTED ON FIRST USE. Other
    // translation units bind a reference to this scene during their own static
    // initialisation (ltworldwalk.cpp does), and a namespace-scope object here
    // would be racing them -- static initialisation order across translation
    // units is not defined, and the failure would be a scene that works or
    // does not depending on link order.
    CLTWorldScene& Scene() { static CLTWorldScene s; return s; }
    std::string&   Name()  { static std::string s;   return s; }
}


CLTWorldScene* LTGetWorldScene() { return &Scene(); }

bool LTWorldScene_IsLoaded()  { return Scene().IsLoaded(); }
const char* LTWorldScene_Name() { return Name().c_str(); }


bool LTWorldScene_Load(CLTFileMgr* pFileMgr, const char* pWorldName)
{
    if (!pFileMgr || !pWorldName || !*pWorldName) return false;

    // ⚠️ Asking for the world that is already loaded is a no-op, not a
    // reload. The game's client and the server both name the level, and
    // reloading a 2.3-million-triangle world because two callers agreed about
    // it would be a very expensive way to change nothing.
    if (Scene().IsLoaded() && Name() == pWorldName) return true;

    if (!Scene().Load(pFileMgr, pWorldName))
    {
        // ⚠️ Loud, never silent. A world that will not load must not leave the
        // engine quietly drawing an empty scene as though nothing had been
        // asked for.
        printf("[WORLD] %s FAILED: %s\n", pWorldName, Scene().GetError());
        fflush(stdout);
        LTRecord_Event("world", "load failed");
        Name().clear();
        return false;
    }

    Name() = pWorldName;

    int nTex = 0, nMdl = 0;
    Scene().PreloadAll(&nTex, &nMdl);
    printf("[WORLD] %s loaded -- preloaded %d texture references and %d prop models\n",
           pWorldName, nTex, nMdl);
    fflush(stdout);

    LTRecord_Event("world", "loaded");
    return true;
}


// ---- ILTLightAnim (HANDOVER 5fe) -- see ltworldscene.h ------------------- //
namespace
{
    bool LATraceOn()
    {
        static const bool b = (getenv("LT_LATRACE") != 0);
        return b;
    }

    class CLightAnimLT : public ILTLightAnim
    {
    public:
        LTRESULT FindLightAnim(const char* pName, HLIGHTANIM& hLightAnim) override
        {
            hLightAnim = INVALID_LIGHT_ANIM;
            // ⚠️ LT_NOLMANIM=1 (the renderer announces it) finds none, as the
            // stub did: a LightFX with an animation gives its dynamic light
            // FLAG_ONLYLIGHTOBJECTS, so finding them without drawing them
            // would leave those rooms with neither.
            static const bool s_bOff = (getenv("LT_NOLMANIM") != 0);
            if (s_bOff) return LT_NOTFOUND;
            const int i = Scene().GetWorld().FindLightAnim(pName);
            if (LATraceOn())
            {
                printf("[LATRACE] FindLightAnim(\"%s\") -> %d\n", pName ? pName : "(null)", i);
                fflush(stdout);
            }
            if (i < 0) return LT_NOTFOUND;
            hLightAnim = (HLIGHTANIM)i;
            return LT_OK;
        }
        LTRESULT GetNumFrames(HLIGHTANIM h, uint32& nFrames) override
        {
            const std::vector<LTWorldLightAnim>& A = Scene().GetLightAnims();
            if (h >= A.size()) return LT_ERROR;
            nFrames = A[h].m_nFrames;
            return LT_OK;
        }
        LTRESULT GetLightAnimInfo(HLIGHTANIM h, LAInfo& info) override
        {
            const std::vector<LTWorldLightAnim>& A = Scene().GetLightAnims();
            if (h >= A.size()) return LT_ERROR;
            const LTWorldLightAnim& la = A[h];
            info.m_bShadowMap = la.m_bShadowMap;
            info.m_iFrames[0] = la.m_iFrames[0];
            info.m_iFrames[1] = la.m_iFrames[1];
            info.m_fPercentBetween = la.m_fPercentBetween;
            info.m_fBlendPercent = la.m_fBlendPercent;
            info.m_vLightPos.Init(la.m_vLightPos[0], la.m_vLightPos[1], la.m_vLightPos[2]);
            info.m_vLightColor.Init(la.m_vLightColor[0], la.m_vLightColor[1], la.m_vLightColor[2]);
            info.m_fLightRadius = la.m_fLightRadius;
            return LT_OK;
        }
        // lithtech.exe (0x404af0 client, 0x46d4d0 server): compare, copy, and
        // only a CHANGE marks the animation's polygons -- a LightFX sets its
        // info every frame, mostly unchanged. m_bShadowMap is not taken: "This
        // can't be changed at runtime".
        LTRESULT SetLightAnimInfo(HLIGHTANIM h, LAInfo& info) override
        {
            std::vector<LTWorldLightAnim>& A = Scene().GetLightAnims();
            if (h >= A.size()) return LT_ERROR;
            LTWorldLightAnim& la = A[h];
            const bool bSame = la.m_iFrames[0] == info.m_iFrames[0] && la.m_iFrames[1] == info.m_iFrames[1]
                && la.m_fPercentBetween == info.m_fPercentBetween && la.m_fBlendPercent == info.m_fBlendPercent
                && la.m_vLightPos[0] == info.m_vLightPos.x && la.m_vLightPos[1] == info.m_vLightPos.y
                && la.m_vLightPos[2] == info.m_vLightPos.z
                && la.m_vLightColor[0] == info.m_vLightColor.x && la.m_vLightColor[1] == info.m_vLightColor.y
                && la.m_vLightColor[2] == info.m_vLightColor.z && la.m_fLightRadius == info.m_fLightRadius;
            if (bSame) return LT_OK;
            la.m_iFrames[0] = info.m_iFrames[0];
            la.m_iFrames[1] = info.m_iFrames[1];
            la.m_fPercentBetween = info.m_fPercentBetween;
            la.m_fBlendPercent = info.m_fBlendPercent;
            la.m_vLightPos[0] = info.m_vLightPos.x; la.m_vLightPos[1] = info.m_vLightPos.y; la.m_vLightPos[2] = info.m_vLightPos.z;
            la.m_vLightColor[0] = info.m_vLightColor.x; la.m_vLightColor[1] = info.m_vLightColor.y; la.m_vLightColor[2] = info.m_vLightColor.z;
            la.m_fLightRadius = info.m_fLightRadius;
            ++la.m_nChange;
            if (LATraceOn())
            {
                static unsigned int s_nSaid = 0;
                if (s_nSaid < 400)
                {
                    ++s_nSaid;
                    printf("[LATRACE] %s: frames %d %d (%.2f) blend %.2f pos (%.0f %.0f %.0f) colour (%.0f %.0f %.0f) radius %.0f\n",
                           la.m_sName.c_str(), (int)la.m_iFrames[0], (int)la.m_iFrames[1], la.m_fPercentBetween,
                           la.m_fBlendPercent, la.m_vLightPos[0], la.m_vLightPos[1], la.m_vLightPos[2],
                           la.m_vLightColor[0], la.m_vLightColor[1], la.m_vLightColor[2], la.m_fLightRadius);
                    fflush(stdout);
                }
            }
            return LT_OK;
        }
    };
}

ILTLightAnim* LTWorldScene_LightAnimLT()
{
    static CLightAnimLT s;
    return &s;
}

void LTWorldScene_ResetLightAnims()
{
    std::vector<LTWorldLightAnim>& A = Scene().GetLightAnims();
    for (size_t i = 0; i < A.size(); ++i)
    {
        LTWorldLightAnim& la = A[i];
        const unsigned int nFrame = (i == 0) ? 0u : 0xFFFFFFFFu;
        if (la.m_iFrames[0] == nFrame && la.m_iFrames[1] == nFrame && la.m_fPercentBetween == 0.0f
            && la.m_fBlendPercent == 1.0f) continue;
        la.m_iFrames[0] = la.m_iFrames[1] = nFrame;
        la.m_fPercentBetween = 0.0f;
        la.m_fBlendPercent = 1.0f;
        ++la.m_nChange;
    }
}
