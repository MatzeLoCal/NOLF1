// ----------------------------------------------------------------------- //
//
// MODULE  : ltworldmetal.cpp
//
// PURPOSE : CLTWorldScene's Metal backend, in its OWN translation unit.
//
// ⚠️ THIS FILE IS SEPARATE FOR A LINKER REASON, NOT A TIDINESS ONE. Putting
// RenderMetal in ltworldrender.cpp made every consumer of the world renderer
// pull in Metal, QuartzCore and Cocoa -- including the headless engine test
// suites, which promptly stopped linking. That is the third time this shape of
// mistake has appeared in this port (ltworlddoors.cpp missing from a suite's
// source list; the walk camera calling into the window layer), so this time it
// is structural: the optional backend lives in an optional object, and nothing
// that does not want a GPU has to know it exists.
//
// ⚠️ It is plain C++, not Objective-C++ -- ltmetal.h is a pure C++ interface --
// so it costs nothing to compile and only the LINK needs the frameworks.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltworldrender.h"
#include "ltworldwalk.h"
#include "ltmodeldraw.h"
#include "ltmetal.h"
#include "ltabc.h"

#include <algorithm>
#include <chrono>
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

// ----------------------------------------------------------------------- //
//
// The Metal backend's entry point.
//
// ⚠️ THIS FUNCTION DECIDES NOTHING ABOUT THE WORLD. It runs the same passes in
// the same order as Render() and hands every triangle to the same DrawModel;
// what it adds is batching by texture and a GPU submit. If a surface rule ever
// needs changing, it is changed in DrawModel and both backends follow.
//
// ----------------------------------------------------------------------- //

namespace
{
    // ⚠️ TWO CONVENTIONS DIFFER BETWEEN THIS ENGINE AND METAL, and both are
    // corrected HERE -- at the one place a GPU sees the matrix -- rather than
    // in LT3D::Perspective, which the software path shares and must not change.
    //
    //   z' = (z + w) / 2   OpenGL clip z (-w..w) -> Metal clip z (0..w).
    //                      Without it everything nearer than the middle of the
    //                      depth range has z < 0 and is clipped away.
    //
    //   y' = -y            LT3D::Perspective carries a deliberate Y FLIP,
    //                      because lt2d's surfaces are top-down and the
    //                      software rasteriser scans them that way. Metal's
    //                      NDC already has +Y up, so that flip applies twice
    //                      and the level renders UPSIDE DOWN -- carpet on the
    //                      ceiling, cornice on the floor. It is a completely
    //                      convincing picture of the right corridor, which is
    //                      why it needed a pixel comparison to catch and not
    //                      a glance.
    void ToMetalClip(const float* pIn, float* pOut)
    {
        float Z[16];
        LT3D::Identity(Z);
        Z[1*4 + 1] = -1.0f;
        Z[2*4 + 2] = 0.5f;
        Z[2*4 + 3] = 0.5f;
        LT3D::Multiply(Z, pIn, pOut);
    }

    bool EndsWithABC(const std::string& s)
    {
        if (s.size() < 4) return false;
        std::string e = s.substr(s.size() - 4);
        for (size_t i = 0; i < e.size(); ++i) e[i] = (char)tolower((unsigned char)e[i]);
        return e == ".abc";
    }

    // ⚠️ The same construction as ltworldrender.cpp's WorldMatrix, and it is a
    // DUPLICATE ON PURPOSE ONLY BECAUSE THAT ONE IS FILE-STATIC. If either ever
    // changes, both must: it is asserted against LTMathImpl::SetupEuler by the
    // 3d suite, which is what stops them drifting silently.
    void WorldMatrixForMetal(const float* pos, const float* eul, float* out,
                             const float* pScale3)
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
        if (pScale3)
            for (int c = 0; c < 3; ++c)
                for (int r = 0; r < 3; ++r) out[r*4 + c] *= pScale3[c];
    }

    // One corner as the GPU takes it -- shared by the batcher and the static
    // recorder, so the two cannot convert differently.
    inline void ToMetalVert(const float* pW, const LT3DVert& v, LTMetalVert& o)
    {
        o.m_Pos[0] = pW[0]; o.m_Pos[1] = pW[1]; o.m_Pos[2] = pW[2];
        o.m_UV[0]  = v.m_U; o.m_UV[1] = v.m_V;
        o.m_RGB[0] = v.m_R; o.m_RGB[1] = v.m_G; o.m_RGB[2] = v.m_B;
    }

    // ⚠️ RECORDS ONE WORLD MODEL'S TRIANGLES, per texture, in the order the
    // batcher would have drawn them: its runs are a map by texture POINTER
    // and each run keeps submission order. The static world (HANDOVER 5bu)
    // is this, made once per level.
    class CStaticRecorder : public ILTWorldGeomSink
    {
    public:
        virtual void Tri(const LT3DTexture* pTex, const float pW[3][3], const LT3DVert* pV)
        {
            if (!pTex) return;
            std::vector<LTMetalVert>& r = m_Runs[pTex];
            for (int i = 0; i < 3; ++i) { LTMetalVert v; ToMetalVert(pW[i], pV[i], v); r.push_back(v); }
            ++m_nTris;
        }
        std::map<const LT3DTexture*, std::vector<LTMetalVert> > m_Runs;
        unsigned int m_nTris = 0;
    };

    // Collects triangles into one run per texture, so the GPU sees a handful of
    // draws instead of thousands.
    //
    // ⚠️ Batching is by TEXTURE POINTER, not by name: two names can resolve to
    // the same cached texture, and splitting those into separate draws would
    // cost draw calls for nothing.
    class CMetalBatcher : public ILTWorldGeomSink, public LTModelDraw::ISink
    {
    public:
        CMetalBatcher() : m_pMVP(0), m_bOrdered(false), m_pCur(0), m_pHandles(0) {}

        void SetMVP(const float* p) { m_pMVP = p; }

        // ⚠️ ORDERED MODE EXISTS FOR THE BLENDED PASS, and it gives up some of
        // the batching on purpose. Grouping by texture pointer reorders the
        // triangles, which is free when the depth test decides the result and
        // WRONG when a compositor does: pass 4 hands its polygons over already
        // sorted back to front, and two panes of different glass would come
        // back in map order, i.e. in address order, i.e. arbitrarily.
        //
        // In this mode a run is still accumulated, but only while the texture
        // does not change; a change draws what is pending first. Consecutive
        // polygons sharing a texture -- which is most of a window -- still
        // batch.
        void SetOrdered(bool b) { m_bOrdered = b; }

        virtual void Tri(const LT3DTexture* pTex,
                         const float pW[3][3],
                         const LT3DVert* pV)
        {
            if (!pTex) return;

            std::vector<LTMetalVert>* pOut;
            if (m_bOrdered)
            {
                if (pTex != m_pCur) { EmitOrdered(); m_pCur = pTex; }
                pOut = &m_Cur;
            }
            else
            {
                pOut = &m_Runs[pTex].m_Verts;
            }

            for (int i = 0; i < 3; ++i)
            {
                LTMetalVert v;
                ToMetalVert(pW[i], pV[i], v);
                pOut->push_back(v);
            }
        }

        // ⚠️ Held rather than passed, because ordered mode has to draw from
        // inside Tri(), which has no room for an argument.
        void SetHandles(const std::map<const LT3DTexture*, unsigned int>* p)
        {
            m_pHandles = p;
        }

        // Draws and empties every run.
        void Flush(const std::map<const LT3DTexture*, unsigned int>& handles)
        {
            m_pHandles = &handles;
            if (!m_pMVP) return;

            EmitOrdered();

            for (std::map<const LT3DTexture*, Run>::iterator it = m_Runs.begin();
                 it != m_Runs.end(); ++it)
            {
                if (it->second.m_Verts.empty()) continue;
                Emit(it->first, it->second.m_Verts);
                it->second.m_Verts.clear();
            }
        }

    private:
        void Emit(const LT3DTexture* pTex, std::vector<LTMetalVert>& verts)
        {
            if (verts.empty() || !pTex || !m_pHandles || !m_pMVP) return;
            // ⚠️ A MIRROR'S STAND-IN (HANDOVER 5de) names its reflection by
            // the slot DrawModel gave it -- it has no texture of its own.
            if (pTex->m_pMirrorFallback)
            {
                const unsigned int hR = LTMetal_ReflectionTexture(pTex->m_nMirrorSlot);
                if (hR) LTMetal_DrawTris(m_pMVP, hR, &verts[0], (int)verts.size(),
                                         false, 0.5f, false, 1.0f, true, 0);
                return;
            }
            std::map<const LT3DTexture*, unsigned int>::const_iterator h =
                m_pHandles->find(pTex);
            if (h == m_pHandles->end() || h->second == 0) return;

            LTMetal_DrawTris(m_pMVP, h->second, &verts[0], (int)verts.size(),
                             pTex->m_bAlphaTest, pTex->m_fAlphaRef, pTex->m_bBlend,
                             pTex->m_fConstAlpha, pTex->m_bTexAlpha, pTex->m_nBlendOp);
        }

        void EmitOrdered()
        {
            if (!m_Cur.empty()) Emit(m_pCur, m_Cur);
            m_Cur.clear();
        }

        struct Run { std::vector<LTMetalVert> m_Verts; };
        std::map<const LT3DTexture*, Run> m_Runs;
        const float* m_pMVP;

        bool                     m_bOrdered;
        const LT3DTexture*       m_pCur;
        std::vector<LTMetalVert> m_Cur;
        const std::map<const LT3DTexture*, unsigned int>* m_pHandles;
    };

    // LT_PASSTIME=1 -- CPU time of each part of a Metal frame, averaged and
    // printed every 300 frames (says so on first use). The GPU's own time is
    // not in it: nothing here waits for the GPU except Begin's frame slot.
    class CPassTimer
    {
    public:
        enum { kBegin, kPre, kUpload, kSky, kWorld, kObjects, kDynamic,
               kBlended, kTranslucent, kSprites, kClose, kEnd, kCount };
        // Inside pass 3b, ALSO counted in kDynamic: PrepareDynamic (pose,
        // light), LTModelDraw::Draw (CPU skinning into the batcher), Flush.
        enum { kDynPrep, kDynDraw, kDynFlush, kSubCount };

        CPassTimer() : m_bOn(On()), m_nAt(0) { if (m_bOn) m_T0 = Now(); }
        ~CPassTimer()
        {
            if (!m_bOn) return;
            Mark(kEnd);
            Totals& t = Get();
            if (++t.m_nFrames < 300) return;
            static const char* kName[kCount] = { "begin", "pre", "upload", "sky", "world",
                "objects", "dynamic", "blended", "transl", "sprites", "close", "end" };
            double all = 0.0;
            for (int k = 0; k < kCount; ++k) all += t.m_Ms[k];
            printf("[PASSTIME] %d frames, ms/frame: total %.2f |", t.m_nFrames, all / t.m_nFrames);
            for (int k = 0; k < kCount; ++k) printf(" %s %.2f", kName[k], t.m_Ms[k] / t.m_nFrames);
            printf(" | world tris %.0f (%.1f models moved) | 3b: prep %.2f draw %.2f flush %.2f, %.1f models"
                   " (%.1f in view: %.2f ms; outside: %.2f ms), skinned corners %.0f for %.0f vertices\n",
                   t.m_fTris / t.m_nFrames, t.m_fMoved / t.m_nFrames, t.m_Sub[kDynPrep] / t.m_nFrames,
                   t.m_Sub[kDynDraw] / t.m_nFrames, t.m_Sub[kDynFlush] / t.m_nFrames,
                   t.m_fDyn / t.m_nFrames, t.m_fVis / t.m_nFrames,
                   t.m_fVisMs / t.m_nFrames, t.m_fOutMs / t.m_nFrames,
                   t.m_fCorners / t.m_nFrames, t.m_fVerts / t.m_nFrames);
            double gms = 0.0; int gn = 0, ge = 0;
            if (LTMetal_TakeGpuTime(&gms, &gn, &ge))
                printf("[PASSTIME] GPU: %.2f ms per frame over %d frame(s), %d failed so far\n", gms / gn, gn, ge);
            fflush(stdout);
            t = Totals();
        }
        // The section that ENDS now.
        void Mark(int k)
        {
            if (!m_bOn) return;
            const double t1 = Now();
            Get().m_Ms[k] += t1 - m_T0;
            m_T0 = t1;
        }
        void Tris(unsigned int n) { if (m_bOn) Get().m_fTris += n; }
        void Moved(unsigned int n) { if (m_bOn) Get().m_fMoved += n; }
        void Dyn(unsigned int n)  { if (m_bOn) Get().m_fDyn += n; }
        // Diagnostic only: would a box test have skipped this model, and what
        // did drawing it cost.
        void DynVis(bool bIn, double fMs) { if (m_bOn) { Totals& t = Get(); (bIn ? t.m_fVisMs : t.m_fOutMs) += fMs; if (bIn) t.m_fVis += 1; } }
        bool On1() const { return m_bOn; }
        void Corners(double nCorners, double nVerts) { if (m_bOn) { Get().m_fCorners += nCorners; Get().m_fVerts += nVerts; } }
        static double NowMs() { return Now(); }
        // A sub-section: Begin, then End adds the time since Begin.
        void SubBegin()      { if (m_bOn) m_S0 = Now(); }
        void SubEnd(int k)   { if (m_bOn) { const double t1 = Now(); Get().m_Sub[k] += t1 - m_S0; m_S0 = t1; } }

    private:
        struct Totals { double m_Ms[kCount]; double m_Sub[kSubCount]; int m_nFrames; double m_fTris, m_fDyn;
                        double m_fVis = 0, m_fVisMs = 0, m_fOutMs = 0, m_fCorners = 0, m_fVerts = 0, m_fMoved = 0;
                        Totals() : m_nFrames(0), m_fTris(0), m_fDyn(0)
                        { for (int k = 0; k < kCount; ++k) m_Ms[k] = 0; for (int k = 0; k < kSubCount; ++k) m_Sub[k] = 0; } };
        static Totals& Get() { static Totals t; return t; }
        static bool On()
        {
            static const bool b = []() {
                const bool v = getenv("LT_PASSTIME") != 0;
                if (v) { printf("[PASSTIME] LT_PASSTIME=1 -- CPU ms per Metal frame section, every 300 frames\n"); fflush(stdout); }
                return v; }();
            return b;
        }
        static double Now()
        {
            using namespace std::chrono;
            return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
        }
        bool   m_bOn;
        int    m_nAt;
        double m_T0, m_S0 = 0.0;
    };
}


// ⚠️ ONE body for both targets. If the offscreen path ran different passes
// from the on-screen one, comparing them would prove nothing about what the
// player sees -- which is the entire purpose of having it.
bool CLTWorldScene::RenderMetalInto(bool bOffscreen, unsigned char* pRGBA,
                                    int nWidth, int nHeight,
                                    const float* pEye3, const float* pAt3,
                                    float fFovY,
                                    const ILTWorldModelXform* pXform,
                                    const ILTWorldDynamicObjects* pDyn)
{
    if (!m_bLoaded) return false;
    CPassTimer pt;                  // LT_PASSTIME=1

    // The camera's up -- reflected inside a mirror's pass (HANDOVER 5de).
    const float up[3] = { m_CamUp[0], m_CamUp[1], m_CamUp[2] };
    const float fAspect = (float)nWidth / (float)(nHeight ? nHeight : 1);

    // ⚠️ THREE TARGETS, ONE BODY: offscreen + readback (the tests, and the
    // old game path), the window's drawable (walk mode), or the compositor's
    // native 3D target (m_pMetalScene, HANDOVER 5bq).
    const MetalSceneTarget* pScene = m_pMetalScene;
    // ...and a fourth: a mirror's reflection (HANDOVER 5de), encoded inside
    // the frame the outer call began.
    const bool bBegun = m_bReflecting
        ? LTMetal_BeginReflection(m_nReflSlot, m_nReflW, m_nReflH)
        : pScene
        ? LTMetal_BeginScene(pScene->m_OutW, pScene->m_OutH, pScene->m_X, pScene->m_Y,
                             pScene->m_W, pScene->m_H, pScene->m_bClear)
        : bOffscreen
        ? LTMetal_BeginOffscreen(nWidth, nHeight, 0.055f, 0.067f, 0.086f)
        : LTMetal_BeginFrame(0.055f, 0.067f, 0.086f);
    if (!bBegun) return (bOffscreen || pScene || m_bReflecting) ? false : true;   // on-screen: window minimised
    pt.Mark(CPassTimer::kBegin);

    // ⚠️ AFTER the frame has begun, and that ORDER IS THE WHOLE POINT. Uploading
    // here rather than above is not tidiness: LTMetal_CreateTexture refuses to
    // do anything before the device exists, and the device is created by the
    // Begin call. Doing it first returned handle 0 for every texture, the
    // batcher skipped every run that had no handle, and the result was a
    // perfectly clean, perfectly blank frame -- which looks exactly like a
    // camera or matrix bug and is neither.
    // ⚠️ Textures are uploaded ON DEMAND and cached by their CPU-side pointer.
    // Doing it here rather than at Load keeps the GPU path from costing
    // anything when it is not used, which matters because it is opt-in.
    //
    // ⚠️ THE BLENDED VARIANTS ARE CREATED FIRST, BEFORE the upload loop, and
    // the order is the whole point. A blended texture is its OWN cache entry
    // (GetTexture's "|blend" key); asking for it inside pass 4, i.e. after the
    // loop below has run, means it has no GPU handle on the frame it is first
    // seen and every blended run is silently dropped. On screen that self-
    // corrects on frame 2 and is invisible; OFFSCREEN there is no frame 2, so
    // the `metal` suite would compare glass against no glass.
    {
        const std::vector<LTWorldModelHeader>& pre = m_World.GetModels();
        for (size_t i = 0; i < pre.size(); ++i)
        {
            if (&pre[i] == m_pSky) continue;
            if (!Drawable(pre[i])) continue;
            // ...and a model translucent by its OBJECT's alpha (5cf) uses the
            // object-alpha entries instead, which need their handle as much.
            float fObjAlpha;
            if (ModelObjectAlpha(pre[i], pXform, &fObjAlpha))
            {
                for (size_t k = 0; k < pre[i].m_Textures.size(); ++k)
                    GetObjectAlphaTexture(pre[i].m_Textures[k], fObjAlpha);
                continue;
            }
            if (!HasBlendedSurface(pre[i])) continue;
            for (size_t k = 0; k < pre[i].m_Textures.size(); ++k)
                GetTexture(pre[i].m_Textures[k], true);
        }

        // ⚠️ AND THE SPRITE VARIANTS, for exactly the same reason. A sprite's
        // texture entry carries ONE object's alpha, so it cannot exist until
        // the object is looked at -- and pass 5 looks at it long after the
        // upload loop below has run. Offscreen there is no second frame to
        // fix it up, so every flare would be missing from the comparison.
        const std::vector<LTWorldObject>& preObj = m_World.GetObjects();
        for (size_t i = 0; i < preObj.size(); ++i)
        {
            LTSpriteDraw sd;
            if (ResolveObjectSprite(i, sd))
                GetSpriteTexture(sd.m_sSprite, sd.m_fAlpha, sd.m_nBlendOp);
        }
        // ...and the game's run-time sprites, for the same reason.
        if (pDyn)
            for (unsigned int i = 0; i < pDyn->SpriteCount(); ++i)
            {
                LTDynamicSprite sp;
                if (pDyn->GetSprite(i, sp))
                    GetSpriteTexture(sp.m_Draw.m_sSprite, sp.m_Draw.m_fAlpha, sp.m_Draw.m_nBlendOp);
            }
    }
    pt.Mark(CPassTimer::kPre);

    // ⚠️ THE STATIC WORLD, BUILT ONCE PER LEVEL (HANDOVER 5bu), and BEFORE the
    // upload loop for the reason the blended variants are: a texture this
    // loads for the first time must get its GPU handle this frame. DrawModel
    // itself does the recording -- every surface rule (sky, invisible, the
    // negated U, P/Q texel scale, 1/128 light) is its, not re-derived. The
    // matrix it is given only feeds the software clip, which a sink ignores.
    // LT_WORLDSTATIC=0 = every world triangle rebuilt per frame (says so).
    static const bool s_bStatic = []() {
        const char* p = getenv("LT_WORLDSTATIC");
        const bool b = !(p && p[0] == '0');
        if (!b) { printf("[LT3D] LT_WORLDSTATIC=0 -- world triangles rebuilt on the CPU every frame\n"); fflush(stdout); }
        return b; }();
    if (s_bStatic && !m_Static.m_bValid)
    {
        if (m_Static.m_nBuffer) LTMetal_FreeBuffer(m_Static.m_nBuffer);
        m_Static = StaticWorld();
        const std::vector<LTWorldModelHeader>& sm = m_World.GetModels();
        m_Static.m_Models.resize(sm.size());
        std::vector<LTMetalVert> all;
        float I[16]; LT3D::Identity(I);
        LT3DTarget rt; rt.m_Width = nWidth; rt.m_Height = nHeight;
        Stats rst;
        // ⚠️ Mirror polygons go under their stand-ins (5de), so each mirror
        // is a run of its own the frame can point at its reflection.
        m_bRecordingStatic = true;
        for (size_t i = 0; i < sm.size(); ++i)
        {
            if (&sm[i] == m_pSky) continue;
            std::vector<const LT3DTexture*> tex(sm[i].m_Textures.size(), (const LT3DTexture*)0);
            for (size_t k = 0; k < sm[i].m_Textures.size(); ++k)
                tex[k] = GetTexture(sm[i].m_Textures[k]);
            CStaticRecorder rec;
            DrawModel(rt, I, sm[i], tex, rst, 0, &rec);
            StaticModel& out = m_Static.m_Models[i];
            out.m_nTris = rec.m_nTris;
            for (std::map<const LT3DTexture*, std::vector<LTMetalVert> >::iterator it = rec.m_Runs.begin();
                 it != rec.m_Runs.end(); ++it)
            {
                StaticRun r;
                r.m_pTex = it->first;
                r.m_nFirst = (unsigned int)all.size();
                r.m_nCount = (unsigned int)it->second.size();
                all.insert(all.end(), it->second.begin(), it->second.end());
                out.m_Runs.push_back(r);
            }
        }
        m_bRecordingStatic = false;
        m_Static.m_nBuffer = all.empty() ? 0 : LTMetal_CreateBuffer(&all[0], (int)all.size());
        m_Static.m_bValid = (m_Static.m_nBuffer != 0) || all.empty();
        printf("[LT3D] static world on the GPU: %u vertices in %zu models, %.1f MB\n",
               (unsigned)all.size(), sm.size(), all.size() * sizeof(LTMetalVert) / 1048576.0);
        fflush(stdout);
    }

    for (std::map<std::string, CachedTex*>::iterator it = m_Textures.begin();
         it != m_Textures.end(); ++it)
    {
        CachedTex* pC = it->second;
        if (!pC->m_bValid) continue;
        if (m_MetalTex.find(&pC->m_Tex) != m_MetalTex.end()) continue;

        const unsigned char* pLv[LT3DTexture::kMaxMips];
        int w[LT3DTexture::kMaxMips], h[LT3DTexture::kMaxMips];
        int n = 0;
        if (pC->m_Tex.m_nMips > 1)
        {
            for (int i = 0; i < pC->m_Tex.m_nMips; ++i)
            {
                if (!pC->m_Tex.m_pMipRGBA[i]) break;
                pLv[n] = pC->m_Tex.m_pMipRGBA[i];
                w[n] = pC->m_Tex.m_MipW[i];
                h[n] = pC->m_Tex.m_MipH[i];
                ++n;
            }
        }
        if (n == 0)
        {
            pLv[0] = pC->m_Tex.m_pRGBA;
            w[0] = pC->m_Tex.m_Width;
            h[0] = pC->m_Tex.m_Height;
            n = 1;
        }
        m_MetalTex[&pC->m_Tex] = LTMetal_CreateTexture(pLv, w, h, n,
                                                       pC->m_Tex.m_bAlphaTest, pC->m_Tex.m_fAlphaRef);
    }
    pt.Mark(CPassTimer::kUpload);

    // ---- pass 0: MIRRORS (HANDOVER 5de) -------------------------------- //
    //
    // The software path's rule (ltworldrender.cpp pass 0): each chosen mirror
    // is this whole function again, from the mirrored camera, into a
    // reflection target the size of the view; then the main pass resumes and
    // the mirrors' stand-ins are pointed at those targets.
    if (!m_bReflecting)
    {
        const int nMir = SelectMirrors(pEye3, pAt3, fFovY, fAspect, pXform);
        if (nMir > 0)
        {
            m_nReflW = pScene ? pScene->m_W : nWidth;
            m_nReflH = pScene ? pScene->m_H : nHeight;
            for (int s = 0; s < nMir; ++s)
            {
                float e2[3], a2[3], u2[3];
                ReflectCamera(m_ActiveMirror[s], pEye3, pAt3, e2, a2, u2);
                SetReflectPlane(m_ActiveMirror[s]);
                memcpy(m_CamUp, u2, sizeof(m_CamUp));
                m_bReflecting = true;
                m_nReflSlot = s;
                LTWorldSetPortalPass(true);
                if (!RenderMetalInto(bOffscreen, 0, nWidth, nHeight, e2, a2, fFovY, pXform, pDyn))
                    m_Mirrors[m_ActiveMirror[s]].m_nSlot = -1;     // no picture: draw it plain
                LTWorldSetPortalPass(false);
                m_bReflecting = false;
                m_CamUp[0] = 0.0f; m_CamUp[1] = 1.0f; m_CamUp[2] = 0.0f;
            }
            LTMetal_ResumeAfterReflections();
        }
        // The stand-ins' slots this frame (the static world's runs are not
        // drawn through DrawModel, which syncs at use); no software picture.
        m_nReflScreenW = m_nReflScreenH = 0;
        for (std::map<std::pair<int, const LT3DTexture*>, LT3DTexture*>::iterator it = m_MirrorPseudo.begin();
             it != m_MirrorPseudo.end(); ++it)
            SyncMirrorPseudo(it->second);
    }

    Stats st;
    st.m_nWorldTris = st.m_nObjTris = st.m_nPixels = st.m_nSkyPixels = st.m_nObjects = 0;
    st.m_nDynamic = 0;

    LT3DTarget dummy;               // DrawModel needs one; the sink bypasses it
    dummy.m_Width = nWidth; dummy.m_Height = nHeight;

    CMetalBatcher batch;
    const std::vector<LTWorldModelHeader>& mdl = m_World.GetModels();

    // ⚠️ RANGE FOG (5ch) -- 1/|P00|, 1/|P11| of the projection every pass
    // below shares. See ltworldrender.cpp.
    {
        float Pf[16];
        LT3D::Perspective(fFovY, fAspect, 4.0f, 100.0f, Pf);
        const bool bRange = LTFogRange() && Pf[0] != 0.0f && Pf[5] != 0.0f;
        LTMetal_SetFogRange(bRange ? 1.0f / fabsf(Pf[0]) : 0.0f,
                            bRange ? 1.0f / fabsf(Pf[5]) : 0.0f);
    }

    // ---- pass 1: the sky, from its own centre -------------------------- //
    if (m_pSky && LTSkyEnabled())
    {
        float c[3];
        for (int k = 0; k < 3; ++k)
            c[k] = (m_pSky->m_Min[k] + m_pSky->m_Max[k]) * 0.5f;

        float at[3] = { c[0] + (pAt3[0] - pEye3[0]),
                        c[1] + (pAt3[1] - pEye3[1]),
                        c[2] + (pAt3[2] - pEye3[2]) };

        float sr = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            float e = m_pSky->m_Max[k] - m_pSky->m_Min[k];
            if (e > sr) sr = e;
        }

        float SP[16], SV[16], SMVP[16];
        LT3D::Perspective(fFovY, fAspect, sr * 0.001f + 0.1f, sr * 4.0f, SP);

        // ⚠️ THE SCENE MIRROR, TAKEN FROM THE ONE PLACE THAT DECIDES IT. The
        // software path negates row 0 of the projection; this must do exactly
        // the same, and must not re-derive WHETHER to. When the mirror became
        // the default this file did not have it and the `metal` suite failed
        // within seconds -- agreement 99.7% -> 81.5%, mean difference 1.39 ->
        // 20.46 -- because the two backends were rendering opposite worlds.
        if (LTWorldMirror()) for (int k = 0; k < 4; ++k) SP[k] = -SP[k];
        // Reflected in a mirror's pass, as the software path does (5de).
        if (m_bReflecting) for (int k = 0; k < 4; ++k) SP[k] = -SP[k];

        LT3D::LookAt(c, at, up, SV);
        LT3D::Multiply(SP, SV, SMVP);
        { float t[16]; ToMetalClip(SMVP, t); memcpy(SMVP, t, sizeof(SMVP)); }

        std::vector<const LT3DTexture*> stex(m_pSky->m_Textures.size(), (const LT3DTexture*)0);
        for (size_t k = 0; k < m_pSky->m_Textures.size(); ++k)
            stex[k] = GetTexture(m_pSky->m_Textures[k]);

        batch.SetMVP(SMVP);
        // The sky fogs with its own distances -- the software path's rule
        // (ltworldrender.cpp). Batched draws take the fog at Flush.
        LTMetal_SetFog(m_FogSky.m_bOn, m_FogSky.m_fNear, m_FogSky.m_fFar, m_FogSky.m_RGB);
        DrawModel(dummy, SMVP, *m_pSky, stex, st, 0, &batch);
        batch.Flush(m_MetalTex);

        // ⚠️ Same rule as the software path: the sky is at infinity, so the
        // world must beat it everywhere. See LTMetal_ClearDepth.
        LTMetal_ClearDepth();
    }
    pt.Mark(CPassTimer::kSky);

    // Everything after the sky fogs with the world's distances.
    LTMetal_SetFog(m_FogWorld.m_bOn, m_FogWorld.m_fNear, m_FogWorld.m_fFar, m_FogWorld.m_RGB);

    float P[16], V[16], MVP[16];
    // ⚠️ FarZ is the projection's far value, and the GPU's own clip volume
    // (0 <= z <= w after ToMetalClip) drops what lies past it -- the same
    // place the software path's per-pixel m_fFarW test does (HANDOVER 5bp).
    LT3D::Perspective(fFovY, fAspect, 4.0f, WorldFar(), P);
    if (LTWorldMirror()) for (int k = 0; k < 4; ++k) P[k] = -P[k];
    LT3D::LookAt(pEye3, pAt3, up, V);
    ReflectProjection(P, V);        // a mirror's pass only (5de)
    LT3D::Multiply(P, V, MVP);

    // ⚠️ The SAME camera basis the software path builds, and built here rather
    // than shared through a helper only because the matrix below is already
    // duplicated for the Metal clip convention. If one changes, both must.
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
    { float t[16]; ToMetalClip(MVP, t); memcpy(MVP, t, sizeof(MVP)); }

    // ⚠️ The SAME nudge the software path applies to separate world models, and
    // a plain matrix precisely so that "the same" holds by construction rather
    // than by two implementations agreeing. See LTBuildModelDepthBias.
    //
    // ⚠️ BUILT AFTER ToMetalClip, AND THE FIRST VERSION WAS BUILT BEFORE IT.
    // The bias multiplies on the RIGHT of the FINISHED view-projection: it is a
    // world-space scale about the eye, so it goes inside every convention
    // correction. Folded into the OpenGL-convention matrix instead, every
    // separate world model -- every door, window and railing in the game --
    // rendered in the wrong place, which reads as a placement bug and is a
    // matrix-order one.
    float Bias[16], MVPBias[16];
    LTBuildModelDepthBias(pEye3, Bias);
    LT3D::Multiply(MVP, Bias, MVPBias);

    // ---- pass 2: the world --------------------------------------------- //
    if (s_bStatic && m_Static.m_bValid)
    {
        // ⚠️ THE BATCHER'S ORDER, REPLAYED, which is what keeps this pixel-
        // identical to the per-frame path: main-BSP models accumulate per
        // texture (pointer order, then model order) until a separate model
        // flushes them; a separate model draws its own runs with the nudged
        // matrix, a moving one with its own. Ties in the depth test go to the
        // first triangle drawn, so order is part of the picture.
        std::map<const LT3DTexture*, std::vector<std::pair<unsigned int, unsigned int> > > pending;
        auto drawRun = [&](const float* pM, const LT3DTexture* pTex, unsigned int nFirst, unsigned int nCount)
        {
            // ⚠️ A MIRROR'S RUN (5de): its reflection when the plane has one
            // this frame (and we are not inside a reflection), else the
            // surface's real texture, exactly as before.
            // ...and a reflection never draws its own mirror (5dg).
            if (pTex && pTex->m_pMirrorFallback && m_bReflecting
                && pTex->m_nMirrorPlane == m_nReflPlaneIdx && !NoReflSelfSkip())
                return;
            if (pTex && pTex->m_pMirrorFallback
                && (m_bReflecting || !MirrorActive(pTex->m_nMirrorPlane)))
                pTex = pTex->m_pMirrorFallback;
            if (pTex && pTex->m_pMirrorFallback)
            {
                const unsigned int hR = LTMetal_ReflectionTexture(pTex->m_nMirrorSlot);
                if (hR) LTMetal_DrawTrisFrom(pM, hR, m_Static.m_nBuffer, (int)nFirst, (int)nCount,
                                             false, 0.5f, false, 1.0f, true, 0);
                return;
            }
            std::map<const LT3DTexture*, unsigned int>::const_iterator h = m_MetalTex.find(pTex);
            if (!pTex || h == m_MetalTex.end() || h->second == 0) return;
            LTMetal_DrawTrisFrom(pM, h->second, m_Static.m_nBuffer, (int)nFirst, (int)nCount,
                                 pTex->m_bAlphaTest, pTex->m_fAlphaRef, pTex->m_bBlend,
                                 pTex->m_fConstAlpha, pTex->m_bTexAlpha, pTex->m_nBlendOp);
        };
        auto flushPending = [&]()
        {
            for (std::map<const LT3DTexture*, std::vector<std::pair<unsigned int, unsigned int> > >::iterator
                     it = pending.begin(); it != pending.end(); ++it)
                for (size_t k = 0; k < it->second.size(); ++k)
                    drawRun(MVP, it->first, it->second[k].first, it->second[k].second);
            pending.clear();
        };
        for (size_t i = 0; i < mdl.size() && i < m_Static.m_Models.size(); ++i)
        {
            if (&mdl[i] == m_pSky) continue;
            if (!Drawable(mdl[i], pXform)) continue;
            { float a; if (ModelObjectAlpha(mdl[i], pXform, &a)) continue; }    // pass 4 (5cf)
            const StaticModel& sm = m_Static.m_Models[i];
            st.m_nWorldTris += sm.m_nTris;
            if (sm.m_Runs.empty()) continue;

            const LTWorldModelHeader& M = mdl[i];
            const bool  bMain = LTIsMainWorldBSP(M);
            const float* pVP  = bMain ? MVP : MVPBias;
            float Local[16], ModelMVP[16];
            if (pXform && pXform->GetModelTransform(M.m_sName, Local))
            {
                flushPending();
                pt.Moved(1);
                LT3D::Multiply(pVP, Local, ModelMVP);
                for (size_t r = 0; r < sm.m_Runs.size(); ++r)
                    drawRun(ModelMVP, sm.m_Runs[r].m_pTex, sm.m_Runs[r].m_nFirst, sm.m_Runs[r].m_nCount);
            }
            else if (!bMain)
            {
                flushPending();
                for (size_t r = 0; r < sm.m_Runs.size(); ++r)
                    drawRun(MVPBias, sm.m_Runs[r].m_pTex, sm.m_Runs[r].m_nFirst, sm.m_Runs[r].m_nCount);
            }
            else
            {
                for (size_t r = 0; r < sm.m_Runs.size(); ++r)
                    pending[sm.m_Runs[r].m_pTex].push_back(
                        std::make_pair(sm.m_Runs[r].m_nFirst, sm.m_Runs[r].m_nCount));
            }
        }
        flushPending();
    }
    else
    {
    batch.SetMVP(MVP);
    for (size_t i = 0; i < mdl.size(); ++i)
    {
        if (&mdl[i] == m_pSky) continue;
        if (!Drawable(mdl[i], pXform)) continue;
        { float a; if (ModelObjectAlpha(mdl[i], pXform, &a)) continue; }    // pass 4 (5cf)

        const LTWorldModelHeader& M = mdl[i];
        std::vector<const LT3DTexture*> tex(M.m_Textures.size(), (const LT3DTexture*)0);
        for (size_t k = 0; k < M.m_Textures.size(); ++k)
            tex[k] = GetTexture(M.m_Textures[k]);

        // ⚠️ Everything except the main BSP takes the nudged matrix. The test
        // is the same flag the software pass reads, from the same header.
        const bool  bMain = LTIsMainWorldBSP(M);
        const float* pVP  = bMain ? MVP : MVPBias;

        float Local[16], ModelMVP[16];
        if (pXform && pXform->GetModelTransform(M.m_sName, Local))
        {
            // ⚠️ A moving brush needs its OWN matrix, so its run is flushed
            // before and after rather than merged with the static geometry.
            batch.Flush(m_MetalTex);
            LT3D::Multiply(pVP, Local, ModelMVP);
            batch.SetMVP(ModelMVP);
            DrawModel(dummy, ModelMVP, M, tex, st, 0, &batch);
            batch.Flush(m_MetalTex);
            batch.SetMVP(MVP);
        }
        else
        {
            // ⚠️ The batcher groups by texture, so a run must not mix matrices:
            // flush when this model's matrix differs from the one in flight.
            if (!bMain)
            {
                batch.Flush(m_MetalTex);
                batch.SetMVP(MVPBias);
                DrawModel(dummy, MVPBias, M, tex, st, 0, &batch);
                batch.Flush(m_MetalTex);
                batch.SetMVP(MVP);
            }
            else
            {
                // ⚠️ No front-to-back sort here: the GPU has early-Z, which is
                // what that sort was emulating.
                DrawModel(dummy, MVP, M, tex, st, 0, &batch);
            }
        }
    }
    batch.Flush(m_MetalTex);
    }   // LT_WORLDSTATIC=0

    // ...and the reflections of mirrors on a model pass 2 skipped (5de) --
    // the software pass's rule (ltworldrender.cpp).
    ForEachActiveMirror(false, MVP, MVPBias, pXform,
        [&](const LTWorldModelHeader& M, const float* pM, const std::vector<const LT3DTexture*>& otex,
            const std::vector<int>& polys)
        {
            batch.SetMVP(pM);
            DrawModel(dummy, pM, M, otex, st, &polys, &batch, kPassOpaque);
            batch.Flush(m_MetalTex);
        });
    batch.SetMVP(MVP);
    pt.Mark(CPassTimer::kWorld);
    pt.Tris(st.m_nWorldTris);

    // ---- pass 3: the objects ------------------------------------------- //
    //
    // ⚠️ Every placement rule -- Visible, Scale, MoveToFloor, being carried by
    // a door -- is read from the SAME object properties as the software path,
    // in the same order. Reproducing them here instead would be a second set of
    // rules to keep in step, which is what LTModelDraw::ISink exists to avoid.
    //
    // ⚠️ RUN TWICE, exactly as the software path runs its own object pass
    // twice: an object whose `Alpha` is below 1.0 is translucent and belongs
    // after the blended brushes. See ltworldrender.cpp pass 5.
    // Upload a model's skins on first sight -- used by the world props AND
    // the game-spawned models, which share the CachedModel cache.
    auto ensureSkins = [&](CachedModel* pM)
    {
        for (int k = 0; k < pM->m_nSkins; ++k)
        {
            if (!pM->m_Skins[k].m_pRGBA) continue;
            if (m_MetalTex.find(&pM->m_Skins[k]) != m_MetalTex.end()) continue;

            // ⚠️ THE WHOLE CHAIN, not level 0. Uploading one level while
            // the software samples several is a guaranteed disagreement on
            // anything minified -- and prop skins only grew a chain today
            // (CachedModel::m_SkinMipRGBA), so this loop grew with it.
            const unsigned char* pLv[LT3DTexture::kMaxMips];
            int w1[LT3DTexture::kMaxMips], h1[LT3DTexture::kMaxMips];
            int n1 = 0;
            for (int q = 0; q < pM->m_Skins[k].m_nMips; ++q)
            {
                if (!pM->m_Skins[k].m_pMipRGBA[q]) break;
                pLv[n1] = pM->m_Skins[k].m_pMipRGBA[q];
                w1[n1]  = pM->m_Skins[k].m_MipW[q];
                h1[n1]  = pM->m_Skins[k].m_MipH[q];
                ++n1;
            }
            if (n1 == 0)
            {
                pLv[0] = pM->m_Skins[k].m_pRGBA;
                w1[0]  = pM->m_Skins[k].m_Width;
                h1[0]  = pM->m_Skins[k].m_Height;
                n1 = 1;
            }
            m_MetalTex[&pM->m_Skins[k]] = LTMetal_CreateTexture(pLv, w1, h1, n1,
                                                                pM->m_Skins[k].m_bAlphaTest,
                                                                pM->m_Skins[k].m_fAlphaRef);
        }
    };

    // ⚠️ GPU SKINNING (HANDOVER 5cd). LTModelDraw::Draw's per-vertex work --
    // skinning, mirror, normal matrix, lighting -- runs in vs_skin instead
    // of on the CPU; the decisions around it (which model, which skins, the
    // pose with its layers and lip-sync node control, the light list) are the
    // same PrepareDynamic / PropRestPose answers both other backends use.
    // Returns false when the model must take the CPU path (switch off, an
    // instrument only the CPU implements, no pipeline, or a mesh that could
    // not be built). LT_GPUSKIN=0 = always the CPU path (says so).
    static const bool s_bGpuSkin = []() {
        const char* e = getenv("LT_GPUSKIN");
        const bool b = !(e && e[0] == '0');
        if (!b) { printf("[MODEL] LT_GPUSKIN=0 -- models skinned and lit on the CPU (old)\n"); fflush(stdout); }
        return b; }();
    SetGpuMeshFree(&LTMetal_FreeSkinMesh);
    auto gpuDraw = [&](CachedModel* pM, const float* ObjMVP, const float* pNormal16,
                       const LTModelPose* pPose,
                       const LTModelDraw::LTModelLighting* pLight, int nMirrorModel) -> bool
    {
        if (!s_bGpuSkin || !LTMetal_SkinningAvailable() || LTModelDraw::CpuOnlyInstrument())
            return false;
        if (pM->m_nGpuMesh == ~0u) return false;

        const std::vector<LTABCPiece>& pieces = pM->m_Model.GetPieces();
        if (pM->m_nGpuMesh == 0)
        {
            // Built once: LOD 0 of every piece, one corner per face corner.
            std::vector<LTMetalSkinVert>   verts;
            std::vector<LTMetalSkinWeight> weights;
            std::vector<LTMetalSkinCorner> corners;
            pM->m_GpuFirst.assign(pieces.size(), 0);
            pM->m_GpuCount.assign(pieces.size(), 0);
            for (size_t q = 0; q < pieces.size(); ++q)
            {
                pM->m_GpuFirst[q] = (int)corners.size();
                if (pieces[q].m_Lods.empty()) continue;
                const LTABCLod& lod = pieces[q].m_Lods[0];
                const unsigned int nBase = (unsigned int)verts.size();
                for (size_t v = 0; v < lod.m_Verts.size(); ++v)
                {
                    const LTABCVertex& vt = lod.m_Verts[v];
                    LTMetalSkinVert sv;
                    for (int k = 0; k < 3; ++k) { sv.m_Pos[k] = vt.m_Pos[k]; sv.m_Nrm[k] = vt.m_Normal[k]; }
                    sv.m_nWFirst = (unsigned int)weights.size();
                    sv.m_nWCount = (unsigned int)vt.m_Weights.size();
                    for (size_t w = 0; w < vt.m_Weights.size(); ++w)
                    {
                        LTMetalSkinWeight sw;
                        for (int k = 0; k < 3; ++k) sw.m_Offset[k] = vt.m_Weights[w].m_Offset[k];
                        sw.m_fBias = vt.m_Weights[w].m_fBias;
                        sw.m_nNode = vt.m_Weights[w].m_nNode;
                        sw.m_Pad[0] = sw.m_Pad[1] = sw.m_Pad[2] = 0;
                        weights.push_back(sw);
                    }
                    verts.push_back(sv);
                }
                for (size_t f = 0; f < lod.m_Faces.size(); ++f)
                {
                    const LTABCFace& face = lod.m_Faces[f];
                    bool bOK = true;
                    for (int i = 0; i < 3; ++i)
                        if (face.m_Verts[i].m_nIndex >= lod.m_Verts.size()) bOK = false;
                    if (!bOK) continue;     // Draw skips such a face too
                    for (int i = 0; i < 3; ++i)
                    {
                        LTMetalSkinCorner c;
                        c.m_nVert = nBase + face.m_Verts[i].m_nIndex;
                        c.m_U = face.m_Verts[i].m_fU;
                        c.m_V = face.m_Verts[i].m_fV;
                        c.m_Pad = 0;
                        corners.push_back(c);
                    }
                }
                pM->m_GpuCount[q] = (int)corners.size() - pM->m_GpuFirst[q];
            }
            pM->m_nGpuMesh = corners.empty() ? 0u
                           : LTMetal_CreateSkinMesh(&verts[0], (int)verts.size(),
                                                    weights.empty() ? 0 : &weights[0], (int)weights.size(),
                                                    &corners[0], (int)corners.size());
            if (pM->m_nGpuMesh == 0) { pM->m_nGpuMesh = ~0u; return false; }
        }

        bool bNoSkinNormal = false, bNoBindInv = false, bNoBiasNorm = false;
        LTModelDraw::SkinSwitches(&bNoSkinNormal, &bNoBindInv, &bNoBiasNorm);

        LTMetalSkinDraw sd;
        memset(&sd, 0, sizeof(sd));
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                sd.m_Normal[r*3 + c] = pNormal16 ? pNormal16[r*4 + c] : (r == c ? 1.0f : 0.0f);
        sd.m_nPaletteOfs = -1;
        sd.m_nNodes = 0;

        // The palette: SkinPosition's matrix rows, and SkinNormal's
        // R * R_bind^T folded into one 3x3 per node. Only for a pose that
        // matches the model -- Draw's own condition for skinning at all.
        const std::vector<LTABCNode>& nodes = pM->m_Model.GetNodes();
        if (pPose && pPose->Count() == nodes.size() && !nodes.empty())
        {
            thread_local std::vector<float> pal;
            pal.resize(nodes.size() * 24);
            for (size_t n = 0; n < nodes.size(); ++n)
            {
                const float* M = pPose->Get((unsigned int)n);
                const float* B = nodes[n].m_Matrix;
                float* o = &pal[n * 24];
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c) o[r*4 + c] = M[r*4 + c];
                for (int r = 0; r < 3; ++r)
                {
                    for (int j = 0; j < 3; ++j)
                        o[12 + r*4 + j] = bNoBindInv ? M[r*4 + j]
                                        : (M[r*4+0]*B[j*4+0] + M[r*4+1]*B[j*4+1] + M[r*4+2]*B[j*4+2]);
                    o[12 + r*4 + 3] = 0.0f;
                }
            }
            sd.m_nPaletteOfs = LTMetal_PushPalette(&pal[0], (int)nodes.size());
            if (sd.m_nPaletteOfs < 0) return false;
            sd.m_nNodes = (int)nodes.size();
        }

        sd.m_bMirror = (nMirrorModel < 0) ? LTModelDraw::MirrorX() : (nMirrorModel != 0);
        sd.m_bMirrorU = false;      // every Metal model call site passes false
        sd.m_bSkinNormal = !bNoSkinNormal;
        sd.m_bBiasNorm = !bNoBiasNorm;
        sd.m_bLit = (pLight != 0);
        if (pLight)
        {
            sd.m_nLights = pLight->m_nLights;
            for (int k = 0; k < 3; ++k) sd.m_Ambient[k] = pLight->m_Ambient[k];
            for (int l = 0; l < pLight->m_nLights && l < 8; ++l)
                for (int k = 0; k < 3; ++k) { sd.m_Dir[l][k] = pLight->m_Dir[l][k]; sd.m_Color[l][k] = pLight->m_Color[l][k]; }
        }

        // Each piece in order -- Draw's order -- with its skin's own state,
        // exactly as the batcher's Emit reads it. No skin, no draw (the
        // batcher drops those triangles too).
        for (size_t q = 0; q < pieces.size() && q < pM->m_GpuCount.size(); ++q)
        {
            if (pM->m_GpuCount[q] < 3) continue;
            if (pieces[q].m_nTexture >= (unsigned short)pM->m_nSkins) continue;
            const LT3DTexture* pTex = &pM->m_Skins[pieces[q].m_nTexture];
            if (!pTex->m_pRGBA || pTex->m_Width <= 0) continue;
            std::map<const LT3DTexture*, unsigned int>::const_iterator h = m_MetalTex.find(pTex);
            if (h == m_MetalTex.end() || h->second == 0) continue;
            LTMetal_DrawSkinned(ObjMVP, h->second, pM->m_nGpuMesh,
                                pM->m_GpuFirst[q], pM->m_GpuCount[q], sd,
                                pTex->m_bAlphaTest, pTex->m_fAlphaRef, pTex->m_bBlend,
                                pTex->m_fConstAlpha, pTex->m_bTexAlpha, pTex->m_nBlendOp);
        }
        return true;
    };

    auto drawObjectPass = [&](bool bTranslucentPass)
    {
        if (!LTObjectsEnabled()) return;
        const std::vector<LTWorldObject>& obj = m_World.GetObjects();

        std::vector<std::pair<float, int> > sorted;
        std::vector<int> order;
        order.reserve(obj.size());
        if (bTranslucentPass)
        {
            for (size_t q = 0; q < obj.size(); ++q)
            {
                float p3[3];
                if (!obj[q].GetPos(p3)) continue;
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

            // ⚠️ Sprites first, for the reason ltworldrender.cpp gives: they
            // are not models and never reach ResolveObjectModel.
            {
                LTSpriteDraw sd;
                if (ResolveObjectSprite(i, sd))
                {
                    if (!bTranslucentPass) continue;
                    float spos[3];
                    if (!obj[i].GetPos(spos)) continue;
                    batch.SetMVP(MVP);
                    DrawSpriteQuad(dummy, MVP, sd, spos, camR, camU, camF,
                                   pEye3, st, &batch);
                    batch.Flush(m_MetalTex);
                    ++st.m_nObjects;
                    continue;
                }
            }

            // ⚠️ ONE decision, shared with the software pass -- including the
            // PropType attribute-file override. See ResolveObjectModel.
            std::string sModelName, sSkinName;
            bool  bCK      = false;
            float fAlpha   = 1.0f;
            int   nBlendOp = 0;
            if (!ResolveObjectModel(i, sModelName, sSkinName, &bCK, &fAlpha, &nBlendOp))
                continue;
            if (LTIsTranslucentDraw(fAlpha, nBlendOp) != bTranslucentPass) continue;
            if (HiddenNow(i)) continue;     // same rule as the software pass

            const LTWorldProperty* pV = obj[i].Find("Visible");
            if (pV && !pV->m_bBool) continue;

            float pos[3], eul[3] = { 0, 0, 0 };
            if (!obj[i].GetPos(pos)) continue;
            obj[i].GetEuler(eul);
            eul[1] += LTPropYawOffset();      // same knob as the software pass
            // The software pass's live placement (HANDOVER 5bt), not re-derived.
            float livePos[3], liveQuat[4];
            const bool bLive = LTWorldScene_LivePose(i, livePos, liveQuat);
            if (bLive) { for (int k = 0; k < 3; ++k) pos[k] = livePos[k]; }
            else if (i < m_ObjectDrop.size()) pos[1] += m_ObjectDrop[i];

            // The software pass's rules, not re-derived (HANDOVER 5bt).
            float scale[3];
            ObjectScale(i, scale);
            const bool bScaled = (scale[0] != 1.0f || scale[1] != 1.0f || scale[2] != 1.0f);

            CachedModel* pM = GetModel(sModelName, sSkinName, bCK, fAlpha, nBlendOp);
            if (!pM) continue;

            ensureSkins(pM);

            // ⚠️ THE MODEL'S OWN ORIGIN OFFSET, and this call site did not have
            // it. An ABC declares the vector from its mesh origin to the centre
            // of the box it advertises, and `Pos` addresses that CENTRE -- so
            // the mesh has to be shifted by MINUS the offset. The software pass
            // has done this since the door-handle work (it moved two plates on
            // one door from 9.9 units apart to 0.15); Metal placed every prop
            // that declares an offset in the wrong place.
            //
            // ⚠️ Applied in MODEL space, through Place's 3x3, for the reason
            // ltworldrender.cpp gives at length: two knobs on opposite faces of
            // a door carry opposite yaws, so one model-space offset must land
            // in opposite world directions. And it is MIRRORED with the mesh
            // when LTModelDraw is mirroring X, or the lever points the right
            // way and the plates come apart.
            float ofs[3] = { 0.0f, 0.0f, 0.0f };
            pM->m_Model.GetOriginOffset(0, ofs);
            if (LTPropMirrorModel()) ofs[0] = -ofs[0];

            float World[16], Place[16], ObjMVP[16];
            if (bLive)
            {
                LT3D::FromQuatPos(liveQuat, pos, World);
                memcpy(Place, World, sizeof(Place));
                if (bScaled)
                    for (int c = 0; c < 3; ++c)
                        for (int r = 0; r < 3; ++r) Place[r*4 + c] *= scale[c];
            }
            else
            {
                WorldMatrixForMetal(pos, eul, World, 0);
                if (bScaled) WorldMatrixForMetal(pos, eul, Place, scale);
                else         memcpy(Place, World, sizeof(Place));
            }
            LTApplyPropTurn(World);         // the software pass's rule, not re-derived
            LTApplyPropTurn(Place);

            if (ofs[0] != 0.0f || ofs[1] != 0.0f || ofs[2] != 0.0f)
            {
                float d[3];
                for (int r = 0; r < 3; ++r)
                    d[r] = Place[r*4+0]*ofs[0] + Place[r*4+1]*ofs[1] + Place[r*4+2]*ofs[2];
                const float sg = LTOriginOffsetSign();  // the donor adds it (5bt)
                for (int r = 0; r < 3; ++r) { Place[r*4+3] += sg*d[r]; World[r*4+3] += sg*d[r]; }
            }

            if (pXform && !bLive)       // the server already carried a live one
            {
                const char* pName = obj[i].GetName();
                float Carry[16];
                if (pName && *pName && pXform->GetModelTransform(pName, Carry))
                {
                    float Tmp[16];
                    LT3D::Multiply(Carry, Place, Tmp); memcpy(Place, Tmp, sizeof(Place));
                    LT3D::Multiply(Carry, World, Tmp); memcpy(World, Tmp, sizeof(World));
                }
            }

            LT3D::Multiply(MVP, Place, ObjMVP);

            // ⚠️ Each object needs its OWN matrix, so its batch is flushed on
            // its own. Props are tens of triangles each; merging them would
            // mean transforming on the CPU, which is the thing being moved off.
            batch.SetMVP(ObjMVP);
            // ⚠️ bMirrorU IS FALSE, AND IT USED TO BE `LTWorldMirror()`.
            // The software object pass was corrected in §5c -- negating U
            // mirrors the whole texture SHEET, and a prop skin is an atlas, so
            // every polygon reads its neighbour's region -- and this call was
            // not corrected with it. The two backends therefore textured every
            // prop in the game differently, which only showed up where props
            // ARE the scenery: M12S01's circus tent is built from models, and
            // the two pictures disagreed by 46.6 mean absolute.
            //
            // ⚠️ SIXTH TIME a decision has been changed in one renderer and
            // not the other (HANDOVER §5e lists the first four, §5i the
            // fifth). The argument for the value lives at the software call
            // site in ltworldrender.cpp; this must not re-derive it.
            if (!gpuDraw(pM, ObjMVP, World, PropPose(i, pM), 0, LTPropMirrorModel()))
                LTModelDraw::Draw(dummy, pM->m_Model, ObjMVP, World,
                                  pM->m_nSkins ? pM->m_Skins : 0, pM->m_nSkins,
                                  0, false, PropPose(i, pM), &batch, false, 0, LTPropMirrorModel());
            batch.Flush(m_MetalTex);

            ++st.m_nObjects;
        }
    };

    drawObjectPass(false);
    pt.Mark(CPassTimer::kObjects);

    // ---- pass 3b: GAME-SPAWNED objects -- characters, their attachments --- //
    //
    // ⚠️ THE SOFTWARE PASS'S DECISIONS, NOT A COPY OF THEM: PrepareDynamic
    // (ltworldrender.cpp) picks the model, places it, poses it -- animation,
    // blink/twitch layers, the game's node control (lip sync) -- and lights
    // it. Only the destination of the triangles differs. Opaque, after the
    // world props, sharing their depth buffer, exactly as in software.
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
            pt.SubBegin();
            if (!PrepareDynamic(d, MVP, dd)) continue;
            CachedModel* pM = dd.m_pModel;
            ensureSkins(pM);
            pt.SubEnd(CPassTimer::kDynPrep);

            // LT_PASSTIME only: would its box (bind pose, grown 50% + 16)
            // have been outside the view? Counted, not acted on.
            bool bInView = true;
            const double fT0 = pt.On1() ? CPassTimer::NowMs() : 0.0;
            if (pt.On1())
            {
                static std::map<const CLTABCModel*, std::pair<bool, std::vector<float> > > s_B;
                std::pair<bool, std::vector<float> >& b = s_B[&pM->m_Model];
                if (b.second.empty())
                {
                    b.second.assign(6, 0.0f);
                    b.first = LTModelDraw::GetBounds(pM->m_Model, &b.second[0], &b.second[3]);
                }
                if (b.first)
                {
                    float c[3], e[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        c[k] = 0.5f * (b.second[k] + b.second[3+k]);
                        e[k] = 0.75f * (b.second[3+k] - b.second[k]) + 16.0f;
                    }
                    int nOut[6] = { 0, 0, 0, 0, 0, 0 };
                    for (int q = 0; q < 8; ++q)
                    {
                        const float pq[3] = { c[0] + ((q&1) ? e[0] : -e[0]),
                                              c[1] + ((q&2) ? e[1] : -e[1]),
                                              c[2] + ((q&4) ? e[2] : -e[2]) };
                        float cl[4];
                        LT3D::TransformPoint(dd.m_ObjMVP, pq, cl);
                        if (cl[0] < -cl[3]) ++nOut[0];
                        if (cl[0] >  cl[3]) ++nOut[1];
                        if (cl[1] < -cl[3]) ++nOut[2];
                        if (cl[1] >  cl[3]) ++nOut[3];
                        if (cl[2] <  0.0f)  ++nOut[4];
                        if (cl[2] >  cl[3]) ++nOut[5];
                    }
                    for (int k = 0; k < 6; ++k) if (nOut[k] == 8) bInView = false;
                }
            }

            batch.SetMVP(dd.m_ObjMVP);
            if (!gpuDraw(pM, dd.m_ObjMVP, dd.m_World, dd.m_bPose ? &dd.m_Pose : 0,
                         dd.m_bLit ? &dd.m_Light : 0, dd.m_bMirror ? 1 : 0))
                LTModelDraw::Draw(dummy, pM->m_Model, dd.m_ObjMVP, dd.m_World,
                              pM->m_nSkins ? pM->m_Skins : 0, pM->m_nSkins,
                              0, false, dd.m_bPose ? &dd.m_Pose : 0, &batch, false,
                              dd.m_bLit ? &dd.m_Light : 0, dd.m_bMirror ? 1 : 0);
            pt.SubEnd(CPassTimer::kDynDraw);
            batch.Flush(m_MetalTex);
            pt.SubEnd(CPassTimer::kDynFlush);
            pt.Dyn(1);
            if (pt.On1())
            {
                pt.DynVis(bInView, CPassTimer::NowMs() - fT0);
                double nc = 0, nv = 0;
                const std::vector<LTABCPiece>& pcs = pM->m_Model.GetPieces();
                for (size_t q = 0; q < pcs.size(); ++q)
                    if (!pcs[q].m_Lods.empty())
                    { nc += 3.0 * pcs[q].m_Lods[0].m_Faces.size(); nv += pcs[q].m_Lods[0].m_Verts.size(); }
                pt.Corners(nc, nv);
            }
            ++st.m_nObjects;
            ++st.m_nDynamic;
        }
    }
    m_nLastMetalDynamic = (unsigned)st.m_nDynamic;
    pt.Mark(CPassTimer::kDynamic);

    // ---- pass 4: BLENDED geometry --------------------------------------- //
    //
    // ⚠️ THE SAME PASS, IN THE SAME PLACE, AS THE SOFTWARE PATH'S pass 4 --
    // last, because a blended fragment writes no depth and so everything it
    // must appear in front of has to be on the screen already; and back to
    // front within each model, because compositing is order-dependent in a way
    // the depth test cannot rescue. Both facts are argued once, in
    // ltworldrender.cpp; this is the backend, and it re-derives neither.
    //
    // ⚠️ ORDERED BATCHING. See CMetalBatcher::SetOrdered -- grouping by texture
    // would undo the sort that makes this pass correct.
    batch.SetOrdered(true);

    // The mirrors' own textures over their reflections first -- the
    // software pass's order (ltworldrender.cpp pass 4, 5de).
    m_bMirrorOverlay = true;
    ForEachActiveMirror(true, MVP, MVPBias, pXform,
        [&](const LTWorldModelHeader& M, const float* pM, const std::vector<const LT3DTexture*>& btex,
            const std::vector<int>& polys)
        {
            batch.SetMVP(pM);
            DrawModel(dummy, pM, M, btex, st, &polys, &batch, kPassBlended);
            batch.Flush(m_MetalTex);
        });
    m_bMirrorOverlay = false;

    for (size_t i = 0; i < mdl.size(); ++i)
    {
        if (&mdl[i] == m_pSky) continue;
        if (!Drawable(mdl[i], pXform)) continue;

        const LTWorldModelHeader& M = mdl[i];

        // ⚠️ Translucent by its OBJECT's alpha (5cf): drawn whole, by the
        // object-alpha entries, and FLUSHED below before the next model --
        // those entries are rewritten per model. See ltworldrender.cpp pass 4.
        float fObjAlpha = 1.0f;
        const bool bObjAlpha = ModelObjectAlpha(M, pXform, &fObjAlpha);
        if (bObjAlpha && fObjAlpha <= 0.0f) continue;
        if (!bObjAlpha && !HasBlendedSurface(M)) continue;  // the overwhelming majority

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

        batch.SetMVP(pUse);
        DrawModel(dummy, pUse, M, btex, st, BuildBackToFrontOrder(M, pEye3),
                  &batch, bObjAlpha ? kPassAll : kPassBlended);
        batch.Flush(m_MetalTex);
    }

    // ---- pass 5: TRANSLUCENT MODELS ------------------------------------- //
    //
    // ⚠️ Still ORDERED: the batcher must not regroup a sorted submission.
    // See ltworldrender.cpp pass 5 for the ordering argument and its limits.
    pt.Mark(CPassTimer::kBlended);
    drawObjectPass(true);

    // ...and the translucent GAME-SPAWNED models, far to near -- the
    // software pass's list (LTTranslucentDynamics), so both draw one order.
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
            ensureSkins(pM);
            batch.SetMVP(dd.m_ObjMVP);
            if (!gpuDraw(pM, dd.m_ObjMVP, dd.m_World, dd.m_bPose ? &dd.m_Pose : 0,
                         dd.m_bLit ? &dd.m_Light : 0, dd.m_bMirror ? 1 : 0))
                LTModelDraw::Draw(dummy, pM->m_Model, dd.m_ObjMVP, dd.m_World,
                              pM->m_nSkins ? pM->m_Skins : 0, pM->m_nSkins,
                              0, false, dd.m_bPose ? &dd.m_Pose : 0, &batch, false,
                              dd.m_bLit ? &dd.m_Light : 0, dd.m_bMirror ? 1 : 0);
            batch.Flush(m_MetalTex);
            ++st.m_nObjects;
            ++st.m_nDynamic;
        }

        // ...and the PolyGrids, far to near -- the software pass's order.
        const std::vector<unsigned int> pgs = LTPolyGridsBackToFront(pDyn, pEye3);
        for (size_t q = 0; q < pgs.size(); ++q)
        {
            LTDynamicPolyGrid g;
            if (!pDyn->GetPolyGrid(pgs[q], g)) continue;
            batch.SetMVP(MVP);
            DrawPolyGrid(dummy, MVP, g, st, &batch);
            batch.Flush(m_MetalTex);
            ++st.m_nObjects;
        }
    }
    pt.Mark(CPassTimer::kTranslucent);

    // ---- pass 5b: the game's run-time sprites -- see the software pass ---- //
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
            batch.SetMVP(MVP);
            DrawSpriteQuad(dummy, MVP, v[i].second.m_Draw, v[i].second.m_Pos,
                           camR, camU, camF, pEye3, st, &batch);
            batch.Flush(m_MetalTex);
        }
    }
    batch.SetOrdered(false);
    pt.Mark(CPassTimer::kSprites);

    // ---- pass 6: FLAG_REALLYCLOSE -- the player-view weapon -------------- //
    // The software pass's rule (ltworldrender.cpp pass 6): last, depth
    // cleared, its own near plane, same x/y projection. Never in a mirror.
    if (pDyn && !m_bReflecting)
    {
        float PC[16], MVPC[16];
        LT3D::Perspective(fFovY, fAspect, LTReallyCloseNear(), 40000.0f, PC);
        if (LTWorldMirror()) for (int k = 0; k < 4; ++k) PC[k] = -PC[k];
        LT3D::Multiply(PC, V, MVPC);
        // ⚠️ AND INTO METAL'S CLIP CONVENTION, as the camera MVP is above
        // (Y flip, z remap). Missing at first: the view weapon hung from the
        // TOP of the screen in Metal only.
        { float tmp[16]; ToMetalClip(MVPC, tmp); memcpy(MVPC, tmp, sizeof(MVPC)); }
        bool bCleared = false;
        auto drawClose = [&](const LTDynamicModel& d)
        {
            DynamicDraw dd;
            if (!PrepareDynamic(d, MVPC, dd)) return;
            if (!bCleared) { batch.Flush(m_MetalTex); LTMetal_ClearDepth(); bCleared = true; }
            CachedModel* pM = dd.m_pModel;
            ensureSkins(pM);
            batch.SetMVP(dd.m_ObjMVP);
            if (!gpuDraw(pM, dd.m_ObjMVP, dd.m_World, dd.m_bPose ? &dd.m_Pose : 0,
                         dd.m_bLit ? &dd.m_Light : 0, dd.m_bMirror ? 1 : 0))
                LTModelDraw::Draw(dummy, pM->m_Model, dd.m_ObjMVP, dd.m_World,
                              pM->m_nSkins ? pM->m_Skins : 0, pM->m_nSkins,
                              0, false, dd.m_bPose ? &dd.m_Pose : 0, &batch, false,
                              dd.m_bLit ? &dd.m_Light : 0, dd.m_bMirror ? 1 : 0);
            batch.Flush(m_MetalTex);
            ++st.m_nDynamic;
        };
        // Opaque first, then translucent far to near -- see the software pass.
        const unsigned int nDyn = pDyn->Count();
        for (unsigned int i = 0; i < nDyn; ++i)
        {
            LTDynamicModel d;
            if (!pDyn->Get(i, d) || !d.m_bReallyClose) continue;
            if (LTIsTranslucentDraw(d.m_fAlpha, d.m_nBlendOp)) continue;
            drawClose(d);
        }
        const std::vector<unsigned int> order = LTTranslucentDynamics(pDyn, true, pEye3);
        batch.SetOrdered(true);
        for (size_t q = 0; q < order.size(); ++q)
        {
            LTDynamicModel d;
            if (pDyn->Get(order[q], d)) drawClose(d);
        }
        batch.SetOrdered(false);
        // ...and the REALLYCLOSE sprites (the muzzle flash).
        batch.SetOrdered(true);
        for (unsigned int i = 0; i < pDyn->SpriteCount(); ++i)
        {
            LTDynamicSprite sp;
            if (!pDyn->GetSprite(i, sp) || !sp.m_bReallyClose) continue;
            if (!bCleared) { batch.Flush(m_MetalTex); LTMetal_ClearDepth(); bCleared = true; }
            batch.SetMVP(MVPC);
            DrawSpriteQuad(dummy, MVPC, sp.m_Draw, sp.m_Pos, camR, camU, camF, pEye3, st, &batch);
            batch.Flush(m_MetalTex);
        }
        batch.SetOrdered(false);
        m_nLastMetalDynamic = (unsigned)st.m_nDynamic;
    }
    pt.Mark(CPassTimer::kClose);

    LTMetal_SetFog(false, 0.0f, 1.0f, 0);    // nobody else draws fogged
    LTMetal_SetFogRange(0.0f, 0.0f);
    // A mirror's pass ends here: its target is complete, the frame goes on.
    if (m_bReflecting)
    {
        LTMetal_EndReflection();
        return true;
    }
    if (pScene)
    {
        LTMetal_SceneLightScale(pScene->m_Scale, pScene->m_Add);
        LTMetal_EndScene();
        return true;
    }
    LTMetal_EndFrame();

    if (bOffscreen && pRGBA)
        return LTMetal_ReadPixels(pRGBA, nWidth, nHeight);

    return true;
}


bool CLTWorldScene::RenderMetalScene(const MetalSceneTarget& t,
                                     const float* pEye3, const float* pAt3,
                                     float fFovY,
                                     const ILTWorldModelXform* pXform,
                                     const ILTWorldDynamicObjects* pDyn)
{
    m_pMetalScene = &t;
    // The rectangle's own size, so the projection's aspect is the window's.
    const bool b = RenderMetalInto(false, 0, t.m_W, t.m_H, pEye3, pAt3, fFovY, pXform, pDyn);
    m_pMetalScene = 0;
    return b;
}


bool CLTWorldScene::RenderMetal(int nWidth, int nHeight,
                                const float* pEye3, const float* pAt3,
                                float fFovY, const ILTWorldModelXform* pXform)
{
    if (!LTMetal_Available()) return false;
    return RenderMetalInto(false, 0, nWidth, nHeight, pEye3, pAt3, fFovY, pXform, 0);
}


bool CLTWorldScene::RenderMetalOffscreen(unsigned char* pRGBA,
                                         int nWidth, int nHeight,
                                         const float* pEye3, const float* pAt3,
                                         float fFovY, const ILTWorldModelXform* pXform,
                                         const ILTWorldDynamicObjects* pDyn)
{
    return RenderMetalInto(true, pRGBA, nWidth, nHeight, pEye3, pAt3, fFovY, pXform, pDyn);
}


// ⚠️ The adapter walk mode is given. Defining it HERE, next to the backend, is
// what keeps the camera's own translation unit free of Metal: a build that does
// not link this file simply never installs a GPU renderer and runs in software.
static bool MetalWalkRender(CLTWorldScene& scene, int nWidth, int nHeight,
                            const float* pEye3, const float* pAt3,
                            float fFovY, const ILTWorldModelXform* pXform)
{
    if (!LTMetal_Available()) return false;
    return scene.RenderMetal(nWidth, nHeight, pEye3, pAt3, fFovY, pXform);
}


// ⚠️ THE GAME'S CAMERA ON THE GPU -- see LTScene_SetGpuCamera. Offscreen,
// then read back into the caller's surface, so the 2D interface the game
// draws afterwards lands on top of it unchanged.
bool LTWorldMetal_GameCamera(CLTWorldScene& scene, unsigned char* pRGBA,
                             int nWidth, int nHeight,
                             const float* pEye3, const float* pAt3, float fFovY,
                             const ILTWorldModelXform* pXform,
                             const ILTWorldDynamicObjects* pDyn,
                             unsigned int* pnDynamicDrawn)
{
    if (!scene.RenderMetalOffscreen(pRGBA, nWidth, nHeight, pEye3, pAt3, fFovY, pXform, pDyn))
        return false;
    if (pnDynamicDrawn) *pnDynamicDrawn = scene.LastMetalDynamic();
    return true;
}


// ⚠️ THE GAME'S CAMERA THROUGH THE COMPOSITOR -- see LTScene_SetGpuScene.
// Into the native 3D target; nothing comes back to the CPU.
bool LTWorldMetal_GameScene(CLTWorldScene& scene, int nOutW, int nOutH,
                            const int* pRect4, bool bClear,
                            const float* pEye3, const float* pAt3, float fFovY,
                            const ILTWorldModelXform* pXform,
                            const ILTWorldDynamicObjects* pDyn,
                            const float* pScale3, const float* pAdd3,
                            unsigned int* pnDynamicDrawn)
{
    CLTWorldScene::MetalSceneTarget t;
    t.m_OutW = nOutW; t.m_OutH = nOutH;
    t.m_X = pRect4[0]; t.m_Y = pRect4[1]; t.m_W = pRect4[2]; t.m_H = pRect4[3];
    t.m_bClear = bClear;
    for (int k = 0; k < 3; ++k) { t.m_Scale[k] = pScale3[k]; t.m_Add[k] = pAdd3[k]; }
    if (!scene.RenderMetalScene(t, pEye3, pAt3, fFovY, pXform, pDyn)) return false;
    if (pnDynamicDrawn) *pnDynamicDrawn = scene.LastMetalDynamic();
    return true;
}


bool LTWorldMetal_Install()
{
    if (!LTMetal_Available()) return false;
    LTWorldWalk_SetGpuRenderer(&MetalWalkRender);
    return true;
}


// ----------------------------------------------------------------------- //
// ⚠️ THE MENU'S 3D AT THE OUTPUT'S RESOLUTION (HANDOVER 5ce).
//
// CLTScene::RenderObjects decided every item (collect mode): the camera's
// matrices with the scene mirror, each sprite's quad and colour, each model's
// pose. Here they are drawn into the compositor's 3D target instead of the
// 640x480 menu surface -- models through LTModelDraw::Draw and the batcher,
// exactly as the world's CPU fallback does (the menu's key light, no U flip,
// the default model mirror -- the software call's own arguments), sprites as
// their two triangles. Textures are uploaded once per pixel buffer: the
// scene's skin, sprite and model caches live for the whole session.
// ----------------------------------------------------------------------- //

#include "ltscene.h"

bool LTWorldMetal_MenuObjects(int nOutW, int nOutH, const int* pRect4, bool bClear,
                              const LTSceneGpuItem* pItems, int nItems,
                              const LTModelPose* pPoses)
{
    if (!pRect4 || pRect4[2] <= 0 || pRect4[3] <= 0) return false;
    if (!LTMetal_BeginScene(nOutW, nOutH, pRect4[0], pRect4[1], pRect4[2], pRect4[3], bClear))
        return false;

    // Pixel buffer + cut-out flag -> Metal texture, for the session.
    static std::map<std::pair<const unsigned char*, bool>, unsigned int> s_Tex;
    std::map<const LT3DTexture*, unsigned int> handles;
    auto upload = [&](const LT3DTexture* t)
    {
        if (!t || !t->m_pRGBA || t->m_Width <= 0 || t->m_Height <= 0) return;
        if (handles.count(t)) return;
        const std::pair<const unsigned char*, bool> key(t->m_pRGBA, t->m_bAlphaTest);
        std::map<std::pair<const unsigned char*, bool>, unsigned int>::iterator it = s_Tex.find(key);
        unsigned int h = 0;
        if (it != s_Tex.end()) h = it->second;
        else
        {
            const unsigned char* pLv[LT3DTexture::kMaxMips];
            int w1[LT3DTexture::kMaxMips], h1[LT3DTexture::kMaxMips];
            int n1 = 0;
            for (int q = 0; q < t->m_nMips && q < LT3DTexture::kMaxMips; ++q)
            {
                if (!t->m_pMipRGBA[q]) break;
                pLv[n1] = t->m_pMipRGBA[q]; w1[n1] = t->m_MipW[q]; h1[n1] = t->m_MipH[q]; ++n1;
            }
            if (n1 == 0) { pLv[0] = t->m_pRGBA; w1[0] = t->m_Width; h1[0] = t->m_Height; n1 = 1; }
            h = LTMetal_CreateTexture(pLv, w1, h1, n1, t->m_bAlphaTest, t->m_fAlphaRef);
            s_Tex[key] = h;
        }
        handles[t] = h;
    };

    CMetalBatcher batch;
    LT3DTarget dummy;
    memset(&dummy, 0, sizeof(dummy));
    for (int i = 0; i < nItems; ++i)
    {
        const LTSceneGpuItem& it = pItems[i];
        float MVPm[16];
        ToMetalClip(it.m_MVP, MVPm);
        if (it.m_nKind == 1)
        {
            upload(it.m_pTex);
            if (!it.m_pTex) continue;
            static const float kU[4] = { 0.0f, 1.0f, 1.0f, 0.0f }, kV[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
            LT3DVert v[4];
            memset(v, 0, sizeof(v));
            for (int q = 0; q < 4; ++q)
            {
                v[q].m_U = kU[q]; v[q].m_V = kV[q];
                v[q].m_R = it.m_RGB[0]; v[q].m_G = it.m_RGB[1]; v[q].m_B = it.m_RGB[2];
            }
            const float t0[3][3] = { { it.m_Corner[0][0], it.m_Corner[0][1], it.m_Corner[0][2] },
                                     { it.m_Corner[1][0], it.m_Corner[1][1], it.m_Corner[1][2] },
                                     { it.m_Corner[2][0], it.m_Corner[2][1], it.m_Corner[2][2] } };
            const float t1[3][3] = { { it.m_Corner[0][0], it.m_Corner[0][1], it.m_Corner[0][2] },
                                     { it.m_Corner[2][0], it.m_Corner[2][1], it.m_Corner[2][2] },
                                     { it.m_Corner[3][0], it.m_Corner[3][1], it.m_Corner[3][2] } };
            const LT3DVert a[3] = { v[0], v[1], v[2] }, b[3] = { v[0], v[2], v[3] };
            batch.SetMVP(MVPm);
            batch.SetOrdered(true);         // the software pass draws them in list order
            batch.Tri(it.m_pTex, t0, a);
            batch.Tri(it.m_pTex, t1, b);
            batch.Flush(handles);
            batch.SetOrdered(false);
            continue;
        }
        if (!it.m_pModel) continue;
        for (int k = 0; k < it.m_nSkins; ++k) upload(&it.m_pSkins[k]);
        const LTModelPose* pPose = (it.m_nPose >= 0 && pPoses) ? &pPoses[it.m_nPose] : 0;
        batch.SetMVP(MVPm);
        LTModelDraw::Draw(dummy, *it.m_pModel, it.m_MVP, it.m_Normal,
                          it.m_nSkins ? it.m_pSkins : 0, it.m_nSkins,
                          0, false, pPose, &batch, false);
        batch.Flush(handles);
    }
    LTMetal_EndScene();
    return true;
}
