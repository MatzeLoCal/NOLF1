// ----------------------------------------------------------------------- //
//
// modelview.cpp -- renders one ABC model with its DTX skin to a PPM.
//
// The first thing in this port that puts GEOMETRY on a surface. Everything it
// uses was verified separately before this existed: the ABC loader (695/695
// exact), the DTX decoder (4160/4160, S3TC checked against Pillow), and the
// rasteriser (28 checks on synthetic buffers). This is the join.
//
// ⚠️ It writes a PPM rather than opening a window, deliberately: a file can be
// diffed by framediff.py at 0.000 and a window cannot. Same reasoning as the
// rest of the frame contract.
//
// USAGE
//   modelview <game-dir>[:<dir>...] <model.abc> <out.ppm> [skin.dtx] [size] [yaw]
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltabc.h"
#include "ltdtx.h"
#include "lt3d.h"
#include "ltmodeldraw.h"
#include "ltfilemgr.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>
#include <vector>

static bool WritePPM(const char* pPath, const unsigned char* pRGBA, int w, int h)
{
    FILE* f = fopen(pPath, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) fwrite(pRGBA + (size_t)i * 4, 1, 3, f);
    fclose(f);
    return true;
}

// CHARS\MODELS\FOO.ABC -> CHARS\SKINS\FOO.DTX, and the same shape for the
// other model directories. A guess, and it SAYS it is guessing.
static std::string GuessSkin(const std::string& sModel)
{
    std::string s = sModel;
    size_t n = s.find("\\MODELS\\");
    if (n != std::string::npos) s.replace(n, 8, "\\SKINS\\");
    n = s.rfind(".ABC");
    if (n != std::string::npos) s.replace(n, 4, ".DTX");
    return s;
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: modelview <dirs> <model.abc> <out.ppm> "
                        "[skin.dtx] [size] [yaw-degrees]\n");
        return 2;
    }

    // Split the colon-separated directory list.
    std::vector<std::string> dirs;
    {
        std::string s(argv[1]);
        size_t a = 0;
        while (a <= s.size())
        {
            size_t b = s.find(':', a);
            if (b == std::string::npos) b = s.size();
            if (b > a) dirs.push_back(s.substr(a, b - a));
            a = b + 1;
        }
    }
    std::vector<const char*> pDirs;
    for (size_t i = 0; i < dirs.size(); ++i) pDirs.push_back(dirs[i].c_str());

    const char* pModel = argv[2];
    const char* pOut   = argv[3];
    const char* pSkin  = (argc > 4 && argv[4][0]) ? argv[4] : 0;
    int nSize = (argc > 5) ? atoi(argv[5]) : 512;
    float fYaw = (argc > 6) ? (float)atof(argv[6]) : 35.0f;
    // -1 means "bind pose"; otherwise a keyframe of animation <nAnim>.
    int nKeyframe = (argc > 7) ? atoi(argv[7]) : -1;

    // ⚠️ WHICH animation, because the interesting ones are never animation 0.
    // The game plays an animation chosen by name out of a merged list, and a
    // pose that comes out wrong does so for ONE animation -- see HANDOVER 5ai.5,
    // where anim 215 collapses two unrelated models to a third of their height
    // while 1, 11, 183, 214 and 343 are fine in the same models.
    int nAnim = (argc > 8) ? atoi(argv[8]) : 0;

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(&pDirs[0], (int)pDirs.size()) == 0)
    {
        fprintf(stderr, "no game data\n");
        return 2;
    }

    CLTABCModel model;
    if (!model.LoadThroughFileMgr(&fm, pModel))
    {
        fprintf(stderr, "model: %s\n", model.GetError());
        return 1;
    }
    printf("model  : %s\n", pModel);
    printf("         v%u, %u pieces, %u nodes, %u tris, %u verts, %u LODs\n",
           model.GetVersion(), (unsigned)model.GetPieces().size(),
           (unsigned)model.GetNodes().size(), model.GetNumTris(),
           model.GetNumVerts(), model.GetNumLODs());

    // ⚠️ THE ANIMATIONS THIS FILE HOLDS, which is NOT what the game plays.
    // The engine appends the animations of a model's CHILD models by name
    // (CLTABCModel::MergeChildAnimations), so an index that is valid in game
    // can be past the end here. A count that disagrees with the game's is the
    // first thing to check when a pose is wrong in game and right in this tool.
    printf("anims  : %u in this file\n", (unsigned)model.GetAnims().size());

    // ⚠️ WHAT EACH ANIMATION ACTUALLY CARRIES. An animation whose transform
    // table does not cover every node poses PART of the skeleton and leaves
    // the rest in whatever the builder left behind -- and a skeleton that is
    // half posed is what a smeared, duplicated model on screen IS.
    if (getenv("LT_ANIMDUMP"))
    {
        const std::vector<LTABCAnim>& aa = model.GetAnims();
        const unsigned nNodes = (unsigned)model.GetNodes().size();
        printf("anim table (%u node(s) in the skeleton)\n", nNodes);
        for (size_t i = 0; i < aa.size(); ++i)
        {
            unsigned nShort = 0;
            for (size_t n = 0; n < aa[i].m_Transforms.size(); ++n)
                if (aa[i].m_Transforms[n].size() != aa[i].m_Keyframes.size()) ++nShort;
            const bool bBad = (aa[i].m_Transforms.size() != nNodes) || nShort;
            if (bBad || getenv("LT_ANIMDUMP_ALL"))
                printf("  %3u %-24s %2u keyframe(s)  %2u node row(s)%s%s\n",
                       (unsigned)i, aa[i].m_sName.c_str(),
                       (unsigned)aa[i].m_Keyframes.size(),
                       (unsigned)aa[i].m_Transforms.size(),
                       (aa[i].m_Transforms.size() != nNodes) ? "  ⚠️  NOT ONE ROW PER NODE" : "",
                       nShort ? "  ⚠️  SHORT ROWS" : "");
        }
    }

    // ⚠️ MERGE THE CHILD MODELS' ANIMATIONS, exactly as the engine does when
    // it loads a model for the world (CLTWorldScene::GetModel). Without this
    // the tool cannot reach the animations the GAME plays: a character file
    // carries ~31 of its own and ends up with ~100 in game, and every index
    // above its own count belongs to a child. A pose bug that only appears in
    // game and never here is usually just this difference.
    {
        const unsigned nBefore = (unsigned)model.GetAnims().size();
        const unsigned nAdded  = model.MergeChildAnimations(&fm, pModel);
        printf("merged : %u child animation(s) appended -> %u total\n",
               nAdded, (unsigned)model.GetAnims().size());
        (void)nBefore;
    }

    // ---- skins ------------------------------------------------------ //
    //
    // ⚠️ A LIST, SPLIT ON ';' -- a character is NOT one skin. Every NOLF
    // character carries at least a body and a head (Hero_Casual.dtx +
    // Hero_Casual_head.dtx) and each PIECE names which one it wants by index
    // (LTABCPiece::m_nTexture). Handing this tool a single skin draws the body
    // with the head's texture and the head with nothing -- which looks exactly
    // like a model bug and is purely the tool's fault.
    enum { kMaxSkins = 8 };
    CLTDtx skin[kMaxSkins];
    std::vector<unsigned char> skinRGBA[kMaxSkins];
    LT3DTexture tex[kMaxSkins];
    int nSkins = 0;

    std::string sSkinList = pSkin ? std::string(pSkin) : GuessSkin(pModel);
    for (size_t a2 = 0; a2 <= sSkinList.size() && nSkins < kMaxSkins; )
    {
        size_t b2 = sSkinList.find(';', a2);
        if (b2 == std::string::npos) b2 = sSkinList.size();
        std::string one = sSkinList.substr(a2, b2 - a2);
        a2 = b2 + 1;

        size_t f2 = one.find_first_not_of(" \t");
        size_t l2 = one.find_last_not_of(" \t");
        if (f2 == std::string::npos) continue;
        one = one.substr(f2, l2 - f2 + 1);
        if (one.empty()) continue;

        const int k = nSkins;
        tex[k].m_pRGBA = 0; tex[k].m_Width = tex[k].m_Height = 0;
        tex[k].m_bAlphaTest = false;

        if (skin[k].LoadThroughFileMgr(&fm, one.c_str())
            && skin[k].DecodeMip(0, skinRGBA[k]) && !skinRGBA[k].empty())
        {
            tex[k].m_pRGBA = &skinRGBA[k][0];
            tex[k].m_Width  = (int)skin[k].GetMip(0)->m_nWidth;
            tex[k].m_Height = (int)skin[k].GetMip(0)->m_nHeight;
            tex[k].m_bAlphaTest = skin[k].AlphaIsMeaningful();
            // ⚠️ HOW MUCH OF THIS SKIN IS ACTUALLY TRANSPARENT. "alpha=tested"
            // only says the format CAN carry alpha; a cut-out piece (an
            // eyelash strip) needs texels that really are transparent, and if
            // there are none the piece draws as a solid quad -- which on a
            // face reads as a black wedge over the eye.
            {
                unsigned nClear = 0, nTot = 0;
                for (size_t q = 3; q < skinRGBA[k].size(); q += 4)
                { ++nTot; if (skinRGBA[k][q] < 128) ++nClear; }
                printf("         transparent texels: %u of %u (%.1f%%)\n",
                       nClear, nTot, nTot ? 100.0 * nClear / nTot : 0.0);
            }
            printf("skin %d : %s  %dx%d %s  alpha=%s%s\n", k, one.c_str(),
                   tex[k].m_Width, tex[k].m_Height, skin[k].GetFormatName(),
                   tex[k].m_bAlphaTest ? "tested" : "ignored",
                   pSkin ? "" : "   (GUESSED from the model path)");
        }
        else
        {
            printf("skin %d : none (%s) -- pieces using it draw untextured\n",
                   k, skin[k].GetError());
        }
        // ⚠️ Counted either way, so a failed load does not shift every later
        // skin down an index and repaint the whole model.
        ++nSkins;
    }

    // What the model actually asks for, against what it was given.
    {
        const std::vector<LTABCPiece>& pcs = model.GetPieces();
        int nWant = -1;
        for (size_t i = 0; i < pcs.size(); ++i)
            if ((int)pcs[i].m_nTexture > nWant) nWant = (int)pcs[i].m_nTexture;
        printf("pieces : %u, highest skin index used %d%s\n",
               (unsigned)pcs.size(), nWant,
               (nWant >= nSkins) ? "   ⚠️  MORE THAN WERE SUPPLIED" : "");
        for (size_t i = 0; i < pcs.size(); ++i)
            printf("         %-24s -> skin %u\n",
                   pcs[i].m_sName.c_str(), pcs[i].m_nTexture);
    }

    // ⚠️ LT_UVDUMP="<piece substring>" -- WHERE A PIECE SAMPLES ITS SKIN.
    // A character head skin is a HALF-FACE ATLAS: the face fills the left of
    // the image and the HAIR fills the right. A small piece whose UVs land on
    // the right-hand side therefore draws dark brown -- which over an eye
    // reads as a black wedge. This prints the UV box so "wrong place in the
    // atlas" can be told from "wrong geometry".
    if (const char* pUV = getenv("LT_UVDUMP"))
    {
        const std::vector<LTABCPiece>& pcs2 = model.GetPieces();
        for (size_t ip = 0; ip < pcs2.size(); ++ip)
        {
            if (pcs2[ip].m_sName.find(pUV) == std::string::npos) continue;
            if (pcs2[ip].m_Lods.empty()) continue;
            float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
            const LTABCLod& L2 = pcs2[ip].m_Lods[0];
            for (size_t fi = 0; fi < L2.m_Faces.size(); ++fi)
                for (int k2 = 0; k2 < 3; ++k2)
                {
                    const float u = L2.m_Faces[fi].m_Verts[k2].m_fU;
                    const float v = L2.m_Faces[fi].m_Verts[k2].m_fV;
                    if (u < u0) u0 = u; if (u > u1) u1 = u;
                    if (v < v0) v0 = v; if (v > v1) v1 = v;
                }
            printf("[UV] %-22s skin %u   u %.3f .. %.3f   v %.3f .. %.3f%s\n",
                   pcs2[ip].m_sName.c_str(), pcs2[ip].m_nTexture, u0, u1, v0, v1,
                   (u0 > 0.5f) ? "   <- entirely in the RIGHT half of the atlas" : "");
        }
    }

    // ⚠️ LT_SWEEP=1 -- SCORE EVERY ANIMATION FOR HEAD DEFORMATION, and rank
    // them. "Her face looks wrong" is one of 368 animations; rendering them by
    // hand is not a search, it is a hope.
    //
    // The score is the worst EDGE STRETCH inside the head pieces: posed edge
    // length over bind edge length. ⚠️ Deliberately a RATIO -- a head that
    // turns or moves scores zero, because every edge keeps its length. Only
    // deformation counts, which is exactly what "the face is wrong" means.
    //
    // ⚠️ RESTRICTED TO THE HEAD (LT_SWEEP_PIECE overrides). A whole-model
    // score is dominated by the shoulder, which legitimately stretches 7x when
    // an arm lifts -- that is how the first version of this measurement wasted
    // its answer on a correct part of the model.
    if (getenv("LT_SWEEP"))
    {
        const char* pWant = getenv("LT_SWEEP_PIECE");
        LTModelPose bindP;
        if (!LTModelDraw::BuildBindPose(model, bindP))
        { fprintf(stderr, "no bind pose\n"); return 1; }

        const std::vector<LTABCPiece>& pcs = model.GetPieces();
        const std::vector<LTABCAnim>&  aa  = model.GetAnims();

        std::vector<int> heads;
        for (size_t ip = 0; ip < pcs.size(); ++ip)
        {
            std::string nm = pcs[ip].m_sName;
            for (size_t q = 0; q < nm.size(); ++q) nm[q] = (char)tolower((unsigned char)nm[q]);
            const bool bWant = pWant ? (nm.find(pWant) != std::string::npos)
                                     : (nm.find("head") != std::string::npos ||
                                        nm.find("eyelash") != std::string::npos);
            if (bWant && !pcs[ip].m_Lods.empty()) heads.push_back((int)ip);
        }
        printf("sweep  : %u animation(s), %u head piece(s)\n",
               (unsigned)aa.size(), (unsigned)heads.size());

        struct Hit { double score; int anim; int kf; const char* piece; double lp, lb; };
        std::vector<Hit> hits;

        for (size_t ia = 0; ia < aa.size(); ++ia)
        {
            const size_t nKf = aa[ia].m_Keyframes.size();
            if (!nKf) continue;
            size_t samples[5] = { 0, nKf/4, nKf/2, (3*nKf)/4, nKf-1 };

            Hit best; best.score = 0.0; best.anim = (int)ia; best.kf = 0; best.piece = "";
            for (int si = 0; si < 5; ++si)
            {
                const size_t kf = samples[si];
                if (kf >= nKf) continue;
                LTModelPose pose;
                if (!LTModelDraw::BuildPose(model, (unsigned)ia, (unsigned)kf, pose)) continue;

                for (size_t hi = 0; hi < heads.size(); ++hi)
                {
                    const LTABCLod& L = pcs[heads[hi]].m_Lods[0];
                    for (size_t fi = 0; fi < L.m_Faces.size(); ++fi)
                    {
                        float P[3][3], B[3][3];
                        bool bOK = true;
                        for (int i2 = 0; i2 < 3; ++i2)
                        {
                            const unsigned short ix = L.m_Faces[fi].m_Verts[i2].m_nIndex;
                            if (ix >= L.m_Verts.size()) { bOK = false; break; }
                            const LTABCVertex& v = L.m_Verts[ix];
                            for (int k2 = 0; k2 < 3; ++k2) { P[i2][k2] = 0.0f; B[i2][k2] = 0.0f; }
                            if (v.m_Weights.empty())
                            { for (int k2 = 0; k2 < 3; ++k2) { P[i2][k2] = v.m_Pos[k2]; B[i2][k2] = v.m_Pos[k2]; } continue; }
                            for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                            {
                                const LTABCWeight& wt = v.m_Weights[iw];
                                const float* A2 = pose.Get(wt.m_nNode);
                                const float* B2 = bindP.Get(wt.m_nNode);
                                if (!A2 || !B2) continue;
                                for (int k2 = 0; k2 < 3; ++k2)
                                {
                                    P[i2][k2] += A2[k2*4+0]*wt.m_Offset[0] + A2[k2*4+1]*wt.m_Offset[1]
                                               + A2[k2*4+2]*wt.m_Offset[2] + A2[k2*4+3]*wt.m_fBias;
                                    B[i2][k2] += B2[k2*4+0]*wt.m_Offset[0] + B2[k2*4+1]*wt.m_Offset[1]
                                               + B2[k2*4+2]*wt.m_Offset[2] + B2[k2*4+3]*wt.m_fBias;
                                }
                            }
                        }
                        if (!bOK) continue;
                        for (int e = 0; e < 3; ++e)
                        {
                            const int j2 = (e + 1) % 3;
                            double lp = 0, lb = 0;
                            for (int k2 = 0; k2 < 3; ++k2)
                            {
                                const double d1 = P[e][k2]-P[j2][k2], d2 = B[e][k2]-B[j2][k2];
                                lp += d1*d1; lb += d2*d2;
                            }
                            lp = sqrt(lp); lb = sqrt(lb);
                            if (lb < 0.05) continue;
                            const double rr = (lp > lb) ? lp/lb : lb/lp;
                            if (rr > best.score)
                            { best.score = rr; best.kf = (int)kf; best.piece = pcs[heads[hi]].m_sName.c_str();
                              best.lp = lp; best.lb = lb; }
                        }
                    }
                }
            }
            if (best.score > 0.0) hits.push_back(best);
        }

        for (size_t i2 = 0; i2 + 1 < hits.size(); ++i2)
            for (size_t j2 = i2 + 1; j2 < hits.size(); ++j2)
                if (hits[j2].score > hits[i2].score) { Hit t2 = hits[i2]; hits[i2] = hits[j2]; hits[j2] = t2; }

        printf("\nworst head deformation, ten worst of %u animation(s):\n", (unsigned)hits.size());
        for (size_t i2 = 0; i2 < hits.size() && i2 < 10; ++i2)
            printf("  %2u.  anim %3d %-22s kf %4d   edge %.2f -> %.2f units  (x%.1f %s)  %s\n",
                   (unsigned)i2 + 1, hits[i2].anim,
                   aa[(size_t)hits[i2].anim].m_sName.c_str(),
                   hits[i2].kf, hits[i2].lb, hits[i2].lp, hits[i2].score,
                   (hits[i2].lp > hits[i2].lb) ? "STRETCHED" : "COLLAPSED",
                   hits[i2].piece);
        return 0;
    }

    // ---- frame the model -------------------------------------------- //
    float mn[3], mx[3];
    if (!LTModelDraw::GetBounds(model, mn, mx))
    {
        fprintf(stderr, "model has no geometry\n");
        return 1;
    }
    float c[3], fRadius = 0.0f;
    for (int i = 0; i < 3; ++i) c[i] = (mn[i] + mx[i]) * 0.5f;
    for (int i = 0; i < 3; ++i)
    {
        float d = (mx[i] - mn[i]) * 0.5f;
        fRadius += d * d;
    }
    fRadius = sqrtf(fRadius);
    printf("bounds : (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)  radius %.1f\n",
           mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], fRadius);

    const float kFov = 0.7853982f;                  // 45 degrees
    float fDist = fRadius / sinf(kFov * 0.5f) * 1.15f;
    float fRad = fYaw * 3.14159265f / 180.0f;

    float eye[3] = { c[0] + sinf(fRad) * fDist,
                     c[1] + fRadius * 0.35f,
                     c[2] + cosf(fRad) * fDist };
    float up[3] = { 0, 1, 0 };

    float P[16], V[16], MVP[16];
    LT3D::Perspective(kFov, 1.0f, fDist * 0.01f + 0.1f, fDist + fRadius * 4.0f, P);
    LT3D::LookAt(eye, c, up, V);
    LT3D::Multiply(P, V, MVP);

    // ---- render ------------------------------------------------------ //
    std::vector<unsigned char> rgba((size_t)nSize * nSize * 4, 0);
    std::vector<float> depth((size_t)nSize * nSize, 0.0f);
    LT3DTarget t;
    t.m_pRGBA = &rgba[0]; t.m_pDepth = &depth[0];
    t.m_Width = nSize; t.m_Height = nSize;

    // ⚠️ LT_UVPROBE=1 -- PER-PIXEL, WHICH TRIANGLE AND WHICH TEXEL.
    //
    // The finished pixel is texel x light, and no amount of looking at it can
    // say which factor made it pale. These two buffers record both halves at
    // the moment the fragment wins its pixel, so the question "is that slab a
    // bright light or a pale texel?" becomes arithmetic. See LT3DTarget.
    const bool bProbe = (getenv("LT_UVPROBE") != 0);
    std::vector<unsigned>      probeID;
    std::vector<unsigned char> probeTex;
    std::vector<float>         probeUV;
    if (bProbe)
    {
        probeID.assign((size_t)nSize * nSize, 0u);
        probeTex.assign((size_t)nSize * nSize * 4, 0);
        probeUV.assign((size_t)nSize * nSize * 2, 0.0f);
        t.m_pProbeID  = &probeID[0];
        t.m_pProbeTex = &probeTex[0];
        t.m_pProbeUV  = &probeUV[0];
    }

    LT3D::ClearColor(t, 0x1B2028);
    LT3D::ClearDepth(t);

    LTModelPose pose;
    const LTModelPose* pPose = 0;
    if (nKeyframe >= 0)
    {
        if (LTModelDraw::BuildPose(model, (unsigned)nAnim, (unsigned)nKeyframe, pose))
        {
            pPose = &pose;
            printf("pose   : animation %d (%s), keyframe %d of %u\n", nAnim,
                   model.GetAnims()[0].m_sName.c_str(), nKeyframe,
                   (unsigned)model.GetAnims()[0].m_Keyframes.size());
        }
        else printf("pose   : NO SUCH keyframe -- drawing the bind pose\n");
    }

    // ⚠️ LT_POSEDUMP=1 -- WHICH NODE MOVED, and how far from where the bind
    // pose puts it. A model that renders as a huge stretched wedge has ONE
    // node (or a few) in the wrong place, and every vertex weighted to it is
    // dragged there. The node's NAME is then the whole diagnosis -- guessing
    // from the shape on screen is how a texture bug, a lighting bug and a
    // skeleton bug all get confused for each other.
    if (getenv("LT_POSEDUMP") && pPose)
    {
        LTModelPose bind;
        if (LTModelDraw::BuildBindPose(model, bind))
        {
            const std::vector<LTABCNode>& nd = model.GetNodes();
            printf("pose vs bind, per node (translation distance):\n");
            for (size_t n = 0; n < nd.size(); ++n)
            {
                const float* A = pPose->Get((unsigned)n);
                const float* B = bind.Get((unsigned)n);
                if (!A || !B) continue;
                const float dx = A[3] - B[3], dy = A[7] - B[7], dz = A[11] - B[11];
                const float d  = sqrtf(dx*dx + dy*dy + dz*dz);
                // ⚠️ AND THE ROTATION, because a bone that stays put can still
                // ruin the geometry hanging off it. The translation test alone
                // called animation 215's head "fine" while the face was clearly
                // deformed -- the jaw and the eyelid had TURNED, not moved.
                // Reported as the angle between the two orientations, from the
                // trace of the relative rotation.
                double tr = 0.0;
                for (int r0 = 0; r0 < 3; ++r0)
                    for (int c0 = 0; c0 < 3; ++c0)
                        tr += (double)A[r0*4+c0] * (double)B[r0*4+c0];
                double cosang = (tr - 1.0) * 0.5;
                if (cosang > 1.0) cosang = 1.0;
                if (cosang < -1.0) cosang = -1.0;
                const double ang = acos(cosang) * 57.2957795;

                printf("  %2u %-22s  moved %7.1f  turned %6.1f deg   posed (%8.1f %8.1f %8.1f)%s\n",
                       (unsigned)n, nd[n].m_sName.c_str(), d, ang,
                       A[3], A[7], A[11],
                       (d > 200.0f) ? "   ⚠️  FLEW" : "");
            }
        }
    }

    // ⚠️ LT_VERTDUMP=1 -- WHICH VERTEX flew, when no BONE did. A skeleton can
    // be entirely sane and a handful of vertices still land far away, and the
    // result is one enormous stretched triangle -- the white wedge across half
    // the screen. The weights are printed with it, because the cause is always
    // in them: a node index out of range, or biases that do not sum to 1.
    if (getenv("LT_VERTDUMP") && pPose)
    {
        LTModelPose bind;
        if (LTModelDraw::BuildBindPose(model, bind))
        {
            const std::vector<LTABCPiece>& pcs = model.GetPieces();
            const unsigned nNodes = (unsigned)model.GetNodes().size();
            unsigned nBadNode = 0, nBadBias = 0, nFlew = 0, nTotal = 0;
            double fWorst = 0.0;

            for (size_t ip = 0; ip < pcs.size(); ++ip)
            {
                if (pcs[ip].m_Lods.empty()) continue;
                const std::vector<LTABCVertex>& vv = pcs[ip].m_Lods[0].m_Verts;
                for (size_t iv = 0; iv < vv.size(); ++iv)
                {
                    const LTABCVertex& v = vv[iv];
                    if (v.m_Weights.empty()) continue;
                    ++nTotal;

                    float fBias = 0.0f;
                    bool  bBadNode = false;
                    float a3[3] = { 0, 0, 0 }, b3[3] = { 0, 0, 0 };
                    for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                    {
                        const LTABCWeight& w = v.m_Weights[iw];
                        fBias += w.m_fBias;
                        if (w.m_nNode >= nNodes) { bBadNode = true; continue; }
                        const float* A = pPose->Get(w.m_nNode);
                        const float* B = bind.Get(w.m_nNode);
                        if (!A || !B) { bBadNode = true; continue; }
                        for (int k = 0; k < 3; ++k)
                        {
                            a3[k] += A[k*4+0]*w.m_Offset[0] + A[k*4+1]*w.m_Offset[1]
                                   + A[k*4+2]*w.m_Offset[2] + A[k*4+3]*w.m_fBias;
                            b3[k] += B[k*4+0]*w.m_Offset[0] + B[k*4+1]*w.m_Offset[1]
                                   + B[k*4+2]*w.m_Offset[2] + B[k*4+3]*w.m_fBias;
                        }
                    }
                    if (bBadNode) ++nBadNode;
                    if (fBias < 0.99f || fBias > 1.01f) ++nBadBias;

                    const float dx = a3[0]-b3[0], dy = a3[1]-b3[1], dz = a3[2]-b3[2];
                    const double d = sqrt((double)(dx*dx + dy*dy + dz*dz));
                    if (d > fWorst) fWorst = d;
                    if (d > 300.0)
                    {
                        if (nFlew < 8)
                        {
                            printf("  FLEW %7.0f units  piece %-20s vertex %5u  weights:",
                                   d, pcs[ip].m_sName.c_str(), (unsigned)iv);
                            for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                                printf("  node %u bias %.3f", v.m_Weights[iw].m_nNode,
                                       v.m_Weights[iw].m_fBias);
                            printf("\n");
                        }
                        ++nFlew;
                    }
                }
            }
            // ⚠️ AND HOW MANY HAVE NO WEIGHTS AT ALL. An unweighted vertex is
            // left at its BIND position while its neighbours follow the
            // skeleton, so any triangle that spans both is stretched between a
            // moving model and a stationary point -- which is what a long
            // spike or a screen-wide wedge IS.
            for (size_t ip = 0; ip < pcs.size(); ++ip)
            {
                if (pcs[ip].m_Lods.empty()) continue;
                const std::vector<LTABCVertex>& vv = pcs[ip].m_Lods[0].m_Verts;
                unsigned nNo = 0;
                for (size_t iv = 0; iv < vv.size(); ++iv)
                    if (vv[iv].m_Weights.empty()) ++nNo;
                printf("  piece %-22s %5u vertex/vertices, %u WITH NO WEIGHTS%s\n",
                       pcs[ip].m_sName.c_str(), (unsigned)vv.size(), nNo,
                       nNo ? "   ⚠️" : "");
            }

            // ⚠️ WHICH FACE VERTICES LEAVE THE HEAD. Measured against what the
            // HEAD NODE alone would do to them: a head that turns moves every
            // face vertex rigidly, so this is zero for correct posing and
            // large exactly where the face is being deformed. The weights then
            // name the bone that is dragging it.
            if (getenv("LT_FACEDEV"))
            {
                const unsigned nHead = 6;   // Head_node, per the pose dump
                const float* H  = pPose->Get(nHead);
                const float* HB = bind.Get(nHead);
                if (H && HB)
                {
                    struct Dev { double d; unsigned piece; unsigned vert; };
                    std::vector<Dev> devs;
                    for (size_t ip = 0; ip < pcs.size(); ++ip)
                    {
                        std::string nm = pcs[ip].m_sName;
                        for (size_t q = 0; q < nm.size(); ++q) nm[q] = (char)tolower((unsigned char)nm[q]);
                        if (nm.find("head") == std::string::npos) continue;
                        if (pcs[ip].m_Lods.empty()) continue;
                        const std::vector<LTABCVertex>& vv = pcs[ip].m_Lods[0].m_Verts;
                        for (size_t iv = 0; iv < vv.size(); ++iv)
                        {
                            const LTABCVertex& v = vv[iv];
                            if (v.m_Weights.empty()) continue;
                            float p3[3] = {0,0,0}, rig[3] = {0,0,0};
                            for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                            {
                                const LTABCWeight& wt = v.m_Weights[iw];
                                const float* A2 = pPose->Get(wt.m_nNode);
                                if (!A2) continue;
                                for (int k2 = 0; k2 < 3; ++k2)
                                    p3[k2] += A2[k2*4+0]*wt.m_Offset[0] + A2[k2*4+1]*wt.m_Offset[1]
                                            + A2[k2*4+2]*wt.m_Offset[2] + A2[k2*4+3]*wt.m_fBias;
                            }
                            // the same vertex carried rigidly by the head node
                            for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                            {
                                const LTABCWeight& wt = v.m_Weights[iw];
                                for (int k2 = 0; k2 < 3; ++k2)
                                    rig[k2] += H[k2*4+0]*wt.m_Offset[0] + H[k2*4+1]*wt.m_Offset[1]
                                             + H[k2*4+2]*wt.m_Offset[2] + H[k2*4+3]*wt.m_fBias;
                            }
                            const double dd = sqrt((double)((p3[0]-rig[0])*(p3[0]-rig[0])
                                                          + (p3[1]-rig[1])*(p3[1]-rig[1])
                                                          + (p3[2]-rig[2])*(p3[2]-rig[2])));
                            Dev dv; dv.d = dd; dv.piece = (unsigned)ip; dv.vert = (unsigned)iv;
                            devs.push_back(dv);
                        }
                    }
                    for (size_t i2 = 0; i2 + 1 < devs.size(); ++i2)
                        for (size_t j2 = i2 + 1; j2 < devs.size(); ++j2)
                            if (devs[j2].d > devs[i2].d) { Dev t2 = devs[i2]; devs[i2] = devs[j2]; devs[j2] = t2; }
                    printf("face vertices furthest from a RIGID head motion:\n");
                    for (size_t i2 = 0; i2 < devs.size() && i2 < 6; ++i2)
                    {
                        const LTABCVertex& v = pcs[devs[i2].piece].m_Lods[0].m_Verts[devs[i2].vert];
                        printf("  %7.2f units  %-18s vertex %4u  weights:", devs[i2].d,
                               pcs[devs[i2].piece].m_sName.c_str(), devs[i2].vert);
                        for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                            printf("  %s/%.3f",
                                   v.m_Weights[iw].m_nNode < model.GetNodes().size()
                                     ? model.GetNodes()[v.m_Weights[iw].m_nNode].m_sName.c_str() : "?",
                                   v.m_Weights[iw].m_fBias);
                        printf("\n");
                    }
                }
            }

            // ⚠️ THE ARTEFACT ITSELF: a TRIANGLE whose edges grow when posed.
            // A screen-wide wedge or a spike out of a head is one triangle
            // with one corner somewhere else, and neither a per-node nor a
            // per-vertex check finds it -- a bone can be sane and a vertex can
            // move a plausible distance while the TRIANGLE it belongs to is
            // torn apart. Measured as posed edge length against bind edge
            // length, so a model that simply moves scores zero.
            {
                double fWorstRatio = 0.0; unsigned nTorn = 0, nShown2 = 0;
                for (size_t ip = 0; ip < pcs.size(); ++ip)
                {
                    if (pcs[ip].m_Lods.empty()) continue;
                    const LTABCLod& L = pcs[ip].m_Lods[0];
                    for (size_t fi = 0; fi < L.m_Faces.size(); ++fi)
                    {
                        float P[3][3], B[3][3];
                        bool bOK = true;
                        for (int i = 0; i < 3; ++i)
                        {
                            const unsigned short ix = L.m_Faces[fi].m_Verts[i].m_nIndex;
                            if (ix >= L.m_Verts.size()) { bOK = false; break; }
                            const LTABCVertex& v = L.m_Verts[ix];
                            for (int k = 0; k < 3; ++k) { P[i][k] = 0.0f; B[i][k] = 0.0f; }
                            if (v.m_Weights.empty())
                            {
                                for (int k = 0; k < 3; ++k) { P[i][k] = v.m_Pos[k]; B[i][k] = v.m_Pos[k]; }
                                continue;
                            }
                            for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                            {
                                const LTABCWeight& w = v.m_Weights[iw];
                                const float* A2 = pPose->Get(w.m_nNode);
                                const float* B2 = bind.Get(w.m_nNode);
                                if (!A2 || !B2) continue;
                                for (int k = 0; k < 3; ++k)
                                {
                                    P[i][k] += A2[k*4+0]*w.m_Offset[0] + A2[k*4+1]*w.m_Offset[1]
                                             + A2[k*4+2]*w.m_Offset[2] + A2[k*4+3]*w.m_fBias;
                                    B[i][k] += B2[k*4+0]*w.m_Offset[0] + B2[k*4+1]*w.m_Offset[1]
                                             + B2[k*4+2]*w.m_Offset[2] + B2[k*4+3]*w.m_fBias;
                                }
                            }
                        }
                        if (!bOK) continue;

                        for (int e = 0; e < 3; ++e)
                        {
                            const int j = (e + 1) % 3;
                            double lp = 0, lb = 0;
                            for (int k = 0; k < 3; ++k)
                            {
                                const double a1 = P[e][k]-P[j][k], b1 = B[e][k]-B[j][k];
                                lp += a1*a1; lb += b1*b1;
                            }
                            lp = sqrt(lp); lb = sqrt(lb);
                            if (lb < 0.01) continue;
                            const double r = lp / lb;
                            if (r > fWorstRatio) fWorstRatio = r;
                            // ⚠️ LT_TEARMIN raises the bar. At 3x the list fills
                            // with the SHOULDER, which stretches legitimately
                            // when an arm lifts; the interesting tears are an
                            // order of magnitude worse.
                            static const double s_fMin = getenv("LT_TEARMIN")
                                ? atof(getenv("LT_TEARMIN")) : 3.0;
                            if (r > s_fMin)
                            {
                                ++nTorn;
                                if (nShown2 < 6)
                                {
                                    ++nShown2;
                                    printf("  TORN x%.1f  %-20s face %5u  edge %.1f -> %.1f units\n",
                                           r, pcs[ip].m_sName.c_str(), (unsigned)fi, lb, lp);

                                    // ⚠️ AND THE BONES BEHIND IT. A torn edge is
                                    // two vertices pulled apart, so the answer is
                                    // always in the pair's weights -- which nodes,
                                    // and whether those nodes were ANIMATED or left
                                    // at a fallback by the merge.
                                    for (int e2 = 0; e2 < 2; ++e2)
                                    {
                                        const unsigned short ix2 =
                                            L.m_Faces[fi].m_Verts[e2 ? j : e].m_nIndex;
                                        if (ix2 >= L.m_Verts.size()) continue;
                                        const LTABCVertex& vv2 = L.m_Verts[ix2];
                                        printf("            vertex %5u weights:", (unsigned)ix2);
                                        for (size_t iw2 = 0; iw2 < vv2.m_Weights.size(); ++iw2)
                                            printf("  node %u (%s) bias %.3f",
                                                   vv2.m_Weights[iw2].m_nNode,
                                                   vv2.m_Weights[iw2].m_nNode < model.GetNodes().size()
                                                     ? model.GetNodes()[vv2.m_Weights[iw2].m_nNode].m_sName.c_str()
                                                     : "?",
                                                   vv2.m_Weights[iw2].m_fBias);
                                        printf("\n");
                                    }
                                }
                            }
                        }
                    }
                }
                printf("tear test: %u edge(s) stretched more than 3x, worst x%.1f%s\n",
                       nTorn, fWorstRatio,
                       nTorn ? "   ⚠️  THIS IS THE ARTEFACT" : "   -- no triangle is torn in this pose");
            }

            // ⚠️ THE SAME ORACLE, FOR THE NORMAL -- and it is a different sum.
            //
            // A position is skinned from `m_Offset`, which the exporter stores
            // in the BONE's own frame, so `pos = SUM(R_i * offset_i) + ...`
            // reproduces the bind position exactly. `m_Normal` is NOT stored
            // that way: it is in MODEL space, like m_Pos. So skinning it as
            // `SUM(bias_i * R_i * n)` applies the bind rotation a SECOND time
            // and the bind pose does not reproduce the file's own normal.
            //
            // The correction is the inverse bind rotation:
            //
            //     n = normalize( SUM bias_i * R_i * R_bind_i^-1 * n )
            //
            // which is exactly the stored normal when R_i == R_bind_i, for any
            // weights, because the biases sum to 1.
            //
            // ⚠️ This needs no reference picture and no judgement, which is the
            // whole reason to state it as an oracle: the model file gives both
            // sides of the equation.
            {
                const std::vector<LTABCPiece>& pcs = model.GetPieces();
                double fSumOld = 0.0, fWorstOld = 0.0;
                double fSumNew = 0.0, fWorstNew = 0.0;
                double fWorstOrtho = 0.0;
                unsigned n = 0;

                // How far each bind matrix's 3x3 is from orthonormal. The
                // inverse of a rotation is its transpose and nothing else, so
                // if these carry scale the transpose is the wrong inverse and
                // this number says so before the fix is believed.
                for (size_t nd = 0; nd < model.GetNodes().size(); ++nd)
                {
                    const float* B = bind.Get((unsigned)nd);
                    if (!B) continue;
                    for (int r = 0; r < 3; ++r)
                        for (int c2 = 0; c2 < 3; ++c2)
                        {
                            double d2 = 0.0;
                            for (int k = 0; k < 3; ++k) d2 += (double)B[r*4+k] * B[c2*4+k];
                            const double want = (r == c2) ? 1.0 : 0.0;
                            if (fabs(d2 - want) > fWorstOrtho) fWorstOrtho = fabs(d2 - want);
                        }
                }

                for (size_t ip = 0; ip < pcs.size(); ++ip)
                {
                    if (pcs[ip].m_Lods.empty()) continue;
                    const std::vector<LTABCVertex>& vv = pcs[ip].m_Lods[0].m_Verts;
                    for (size_t iv = 0; iv < vv.size(); ++iv)
                    {
                        const LTABCVertex& v = vv[iv];
                        if (v.m_Weights.empty()) continue;
                        float o3[3] = {0,0,0}, n3[3] = {0,0,0};
                        for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                        {
                            const LTABCWeight& w = v.m_Weights[iw];
                            const float* B = bind.Get(w.m_nNode);
                            if (!B) continue;
                            // what the code does today
                            for (int k = 0; k < 3; ++k)
                                o3[k] += (B[k*4+0]*v.m_Normal[0] + B[k*4+1]*v.m_Normal[1]
                                        + B[k*4+2]*v.m_Normal[2]) * w.m_fBias;
                            // with the inverse bind rotation (transpose) first
                            float t3[3];
                            for (int k = 0; k < 3; ++k)
                                t3[k] = B[0*4+k]*v.m_Normal[0] + B[1*4+k]*v.m_Normal[1]
                                      + B[2*4+k]*v.m_Normal[2];
                            for (int k = 0; k < 3; ++k)
                                n3[k] += (B[k*4+0]*t3[0] + B[k*4+1]*t3[1] + B[k*4+2]*t3[2])
                                       * w.m_fBias;
                        }
                        double a[2] = { 0.0, 0.0 };
                        const float* srcs[2] = { o3, n3 };
                        for (int s = 0; s < 2; ++s)
                        {
                            const float* p = srcs[s];
                            const double len = sqrt((double)(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]));
                            if (len < 1e-9) { a[s] = 180.0; continue; }
                            double d2 = (p[0]*v.m_Normal[0] + p[1]*v.m_Normal[1]
                                       + p[2]*v.m_Normal[2]) / len;
                            if (d2 > 1.0) d2 = 1.0;
                            if (d2 < -1.0) d2 = -1.0;
                            a[s] = acos(d2) * 57.2957795;
                        }
                        ++n;
                        fSumOld += a[0]; if (a[0] > fWorstOld) fWorstOld = a[0];
                        fSumNew += a[1]; if (a[1] > fWorstNew) fWorstNew = a[1];
                    }
                }
                printf("normal oracle: %u weighted vertex/vertices; bind-skinned normal vs the "
                       "file's own\n"
                       "               R_i * n           mean %7.3f deg, worst %7.3f deg%s\n"
                       "               R_i * R_bind^-1 * n  mean %7.3f deg, worst %7.3f deg\n"
                       "               bind 3x3 furthest from orthonormal: %.6f\n",
                       n, n ? fSumOld/n : 0.0, fWorstOld,
                       fWorstOld > 1.0 ? "   ⚠️  THE BIND POSE DOES NOT REPRODUCE THE FILE" : "",
                       n ? fSumNew/n : 0.0, fWorstNew, fWorstOrtho);
            }

            // ⚠️ THE ORACLE: skinning a vertex with the BIND pose must reproduce
            // the vertex's own stored position. It is the one check that needs
            // no reference picture and no judgement -- the model states both
            // sides of it. Any vertex where the two disagree is mis-skinned in
            // EVERY pose, and the error is invisible in the bind pose only
            // because nothing else has moved yet.
            {
                unsigned nOff = 0, nShown = 0; double fMax = 0.0;
                for (size_t ip = 0; ip < pcs.size(); ++ip)
                {
                    if (pcs[ip].m_Lods.empty()) continue;
                    const std::vector<LTABCVertex>& vv = pcs[ip].m_Lods[0].m_Verts;
                    for (size_t iv = 0; iv < vv.size(); ++iv)
                    {
                        const LTABCVertex& v = vv[iv];
                        if (v.m_Weights.empty()) continue;
                        float b3[3] = { 0, 0, 0 };
                        for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                        {
                            const LTABCWeight& w = v.m_Weights[iw];
                            const float* B = bind.Get(w.m_nNode);
                            if (!B) continue;
                            for (int k = 0; k < 3; ++k)
                                b3[k] += B[k*4+0]*w.m_Offset[0] + B[k*4+1]*w.m_Offset[1]
                                       + B[k*4+2]*w.m_Offset[2] + B[k*4+3]*w.m_fBias;
                        }
                        const float dx = b3[0]-v.m_Pos[0], dy = b3[1]-v.m_Pos[1],
                                    dz = b3[2]-v.m_Pos[2];
                        const double d = sqrt((double)(dx*dx+dy*dy+dz*dz));
                        if (d > fMax) fMax = d;
                        if (d > 1.0)
                        {
                            ++nOff;
                            if (nShown < 6)
                            {
                                ++nShown;
                                printf("  MIS-SKINNED %6.1f units  %-20s vertex %5u"
                                       "  bind-skinned (%.1f %.1f %.1f) vs stored (%.1f %.1f %.1f)  weights:",
                                       d, pcs[ip].m_sName.c_str(), (unsigned)iv,
                                       b3[0], b3[1], b3[2],
                                       v.m_Pos[0], v.m_Pos[1], v.m_Pos[2]);
                                for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                                    printf("  n%u/%.3f", v.m_Weights[iw].m_nNode,
                                           v.m_Weights[iw].m_fBias);
                                printf("\n");
                            }
                        }
                    }
                }
                printf("bind oracle: %u vertex/vertices do not reproduce their own "
                       "position, worst %.1f units%s\n", nOff, fMax,
                       nOff ? "   ⚠️  THE SKINNING RULE IS WRONG FOR THESE" : "   -- skinning reproduces the model exactly");
            }

            printf("vertices: %u weighted, %u flew (>300u), worst %.0f units;"
                   " %u with a bad node index, %u whose biases do not sum to 1\n",
                   nTotal, nFlew, fWorst, nBadNode, nBadBias);
        }
    }

    // ⚠️ LT_NORMCHECK=1 -- DO THE SKINNED NORMALS STILL FIT THE POSED SURFACE?
    //
    // The bind-pose oracle (enginetest_abc) proves the skinning formula where
    // R_i == R_bind_i. It says NOTHING about any other pose, because there the
    // model states no answer. This is the check that does: a vertex normal on
    // a smooth mesh should sit near the average of the FACE normals of the
    // triangles around it, and that relationship is a property of the surface,
    // so it must survive posing.
    //
    // ⚠️ IT IS A COMPARISON, NOT A THRESHOLD. Smooth-shaded art disagrees with
    // its own facets by a real amount -- a hard edge is exactly a vertex whose
    // normal matches neither side. So the number that means something is the
    // BIND pose's disagreement, measured the same way, and the posed figure
    // next to it. Equal means the skinning carried the surface faithfully.
    if (getenv("LT_NORMCHECK") && pPose)
    {
        LTModelPose bind;
        if (LTModelDraw::BuildBindPose(model, bind))
        {
            const std::vector<LTABCPiece>& pcs = model.GetPieces();
            const char* pWant = getenv("LT_NORMCHECK_PIECE");

            for (int mode = 0; mode < 2; ++mode)       // 0 = bind, 1 = posed
            {
                const LTModelPose& P = mode ? *pPose : bind;

                double fSum = 0.0, fWorst = 0.0;
                unsigned nV = 0;

                for (size_t ip = 0; ip < pcs.size(); ++ip)
                {
                    if (pcs[ip].m_Lods.empty()) continue;
                    if (pWant && pcs[ip].m_sName.find(pWant) == std::string::npos) continue;
                    const LTABCLod& L = pcs[ip].m_Lods[0];

                    // Area-weighted face normal accumulated per vertex.
                    std::vector<float> acc(L.m_Verts.size() * 3, 0.0f);
                    std::vector<float> posed(L.m_Verts.size() * 3, 0.0f);
                    for (size_t iv = 0; iv < L.m_Verts.size(); ++iv)
                    {
                        const LTABCVertex& v = L.m_Verts[iv];
                        float p3[3] = {0,0,0};
                        if (v.m_Weights.empty())
                        { for (int k=0;k<3;++k) p3[k] = v.m_Pos[k]; }
                        else
                            for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                            {
                                const LTABCWeight& wt = v.m_Weights[iw];
                                const float* M = P.Get(wt.m_nNode);
                                if (!M) continue;
                                for (int k = 0; k < 3; ++k)
                                    p3[k] += M[k*4+0]*wt.m_Offset[0] + M[k*4+1]*wt.m_Offset[1]
                                           + M[k*4+2]*wt.m_Offset[2] + M[k*4+3]*wt.m_fBias;
                            }
                        for (int k = 0; k < 3; ++k) posed[iv*3+k] = p3[k];
                    }

                    for (size_t fi = 0; fi < L.m_Faces.size(); ++fi)
                    {
                        unsigned short ix[3];
                        bool bOK = true;
                        for (int k = 0; k < 3; ++k)
                        {
                            ix[k] = L.m_Faces[fi].m_Verts[k].m_nIndex;
                            if (ix[k] >= L.m_Verts.size()) bOK = false;
                        }
                        if (!bOK) continue;
                        const float* a = &posed[ix[0]*3];
                        const float* b = &posed[ix[1]*3];
                        const float* c = &posed[ix[2]*3];
                        const float e1[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
                        const float e2[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
                        const float fn[3] = { e1[1]*e2[2]-e1[2]*e2[1],
                                              e1[2]*e2[0]-e1[0]*e2[2],
                                              e1[0]*e2[1]-e1[1]*e2[0] };
                        // NOT normalised: the magnitude IS twice the area, which
                        // is the weight a vertex normal should use.
                        for (int k = 0; k < 3; ++k)
                            for (int j = 0; j < 3; ++j) acc[ix[k]*3+j] += fn[j];
                    }

                    for (size_t iv = 0; iv < L.m_Verts.size(); ++iv)
                    {
                        const LTABCVertex& v = L.m_Verts[iv];
                        if (v.m_Weights.empty()) continue;

                        float n3[3] = { v.m_Normal[0], v.m_Normal[1], v.m_Normal[2] };
                        if (mode && !LTModelDraw::SkinNormal(model, v, P, n3)) continue;

                        const float* g = &acc[iv*3];
                        const double gl = sqrt((double)(g[0]*g[0]+g[1]*g[1]+g[2]*g[2]));
                        if (gl < 1e-9) continue;

                        double d = (n3[0]*g[0] + n3[1]*g[1] + n3[2]*g[2]) / gl;
                        if (d < 0.0) d = -d;            // the light is two-sided
                        if (d > 1.0) d = 1.0;
                        const double deg = acos(d) * 57.2957795;
                        ++nV; fSum += deg;
                        if (deg > fWorst) fWorst = deg;
                    }
                }

                printf("normcheck %-5s : %5u vertex/vertices, vertex normal vs the "
                       "surface around it -- mean %6.2f deg, worst %6.2f deg\n",
                       mode ? "POSED" : "bind", nV,
                       nV ? fSum / nV : 0.0, fWorst);
            }
            printf("              ⚠️  the BIND row is the baseline; a POSED mean far "
                   "above it means the normals no longer fit the surface\n");
        }
    }

    // ⚠️ LT_CULL=1 -- draw with BACK-FACE CULLING. The model path passes
    // false everywhere (here and in the world pass), so the far side of a head
    // is rasterised as well as the near side and wins wherever the depth test
    // lets it. A flat pale band across a face is what the INSIDE of that face
    // looks like.
    const bool bCullArg = (getenv("LT_CULL") != 0);
    LTModelDrawStats st = LTModelDraw::Draw(t, model, MVP, V,
                                            nSkins ? tex : 0, nSkins,
                                            0, bCullArg, pPose);

    printf("drawn  : %d triangles, %d pixels, %d covered nothing\n",
           st.m_nTriangles, st.m_nPixels, st.m_nCulled);

    // ⚠️ THE PROBE REPORT. One row per TRIANGLE that owns pixels, carrying
    // the two things a PPM cannot show: the mean TEXEL it sampled and the
    // mean LIGHT it was multiplied by. A pale patch is one or the other and
    // the ratio says which, without a second render and without an opinion.
    //
    // ⚠️ Restricted to a piece by LT_UVPROBE_PIECE, because a whole character
    // is 1,638 triangles and the face is a few dozen of them.
    if (bProbe)
    {
        struct Acc
        {
            unsigned n;
            double fr, fg, fb;      // final pixel
            double tr, tg, tb;      // sampled texel
            int    x0, y0, x1, y1;  // screen box, so a row can be found by eye
        };
        std::map<unsigned, Acc> acc;

        for (int y = 0; y < nSize; ++y)
            for (int x = 0; x < nSize; ++x)
            {
                const size_t i = (size_t)y * nSize + x;
                const unsigned id = probeID[i];
                if (!id) continue;
                Acc& a = acc[id];
                if (!a.n) { a.fr=a.fg=a.fb=a.tr=a.tg=a.tb=0.0;
                            a.x0=a.x1=x; a.y0=a.y1=y; }
                ++a.n;
                a.fr += rgba[i*4+0];  a.fg += rgba[i*4+1];  a.fb += rgba[i*4+2];
                a.tr += probeTex[i*4+0]; a.tg += probeTex[i*4+1]; a.tb += probeTex[i*4+2];
                if (x < a.x0) a.x0 = x;  if (x > a.x1) a.x1 = x;
                if (y < a.y0) a.y0 = y;  if (y > a.y1) a.y1 = y;
            }

        // ⚠️ AND THE TWO BUFFERS AS FILES, beside the PPM. The table below is
        // per triangle; a picture of the TEXEL buffer alone -- and of the
        // final image divided by it, which is the LIGHT -- is what shows the
        // shape of a patch. Dividing afterwards is exact enough here because
        // both sides are the bytes the renderer actually produced.
        {
            std::string sT = std::string(pOut) + ".texel.ppm";
            WritePPM(sT.c_str(), &probeTex[0], nSize, nSize);
            std::string sI = std::string(pOut) + ".triid.bin";
            FILE* fI = fopen(sI.c_str(), "wb");
            if (fI)
            {
                fwrite(&probeID[0], sizeof(unsigned), (size_t)nSize * nSize, fI);
                fclose(fI);
            }
            std::string sU = std::string(pOut) + ".uv.bin";
            FILE* fU = fopen(sU.c_str(), "wb");
            if (fU)
            {
                fwrite(&probeUV[0], sizeof(float), (size_t)nSize * nSize * 2, fU);
                fclose(fU);
            }
            printf("probe  : wrote %s, %s and %s\n", sT.c_str(), sI.c_str(), sU.c_str());
        }

        const char* pWantPiece = getenv("LT_UVPROBE_PIECE");
        const unsigned nMinPx  = getenv("LT_UVPROBE_MIN")
            ? (unsigned)atoi(getenv("LT_UVPROBE_MIN")) : 40u;

        struct Row { unsigned id; Acc a; double lum; };
        std::vector<Row> rows;
        const std::vector<LTABCPiece>& pcs3 = model.GetPieces();
        for (std::map<unsigned, Acc>::const_iterator it = acc.begin(); it != acc.end(); ++it)
        {
            const unsigned ip = (it->first >> 20) & 0xFFFu;
            if (ip >= pcs3.size()) continue;
            if (pWantPiece && pcs3[ip].m_sName.find(pWantPiece) == std::string::npos) continue;
            if (it->second.n < nMinPx) continue;
            Row r; r.id = it->first; r.a = it->second;
            r.lum = (r.a.fr + r.a.fg + r.a.fb) / (3.0 * r.a.n);
            rows.push_back(r);
        }
        for (size_t i = 0; i + 1 < rows.size(); ++i)
            for (size_t j = i + 1; j < rows.size(); ++j)
                if (rows[j].lum > rows[i].lum) { Row tmp = rows[i]; rows[i] = rows[j]; rows[j] = tmp; }

        printf("\nprobe  : %u triangle(s) own %u+ pixels%s -- BRIGHTEST FIRST\n",
               (unsigned)rows.size(), nMinPx,
               pWantPiece ? " in the matching piece(s)" : "");
        printf("   piece                  face   px    final RGB        texel RGB      light   UVs (u,v) x3\n");
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const unsigned ip = (rows[i].id >> 20) & 0xFFFu;
            const unsigned fi = (rows[i].id & 0xFFFFFu) - 1u;
            const Acc& a = rows[i].a;
            const double tl = (a.tr + a.tg + a.tb) / (3.0 * a.n);
            printf("   %-20s %6u %5u  %5.0f %5.0f %5.0f   %5.0f %5.0f %5.0f   %5.3f ",
                   pcs3[ip].m_sName.c_str(), fi, a.n,
                   a.fr/a.n, a.fg/a.n, a.fb/a.n,
                   a.tr/a.n, a.tg/a.n, a.tb/a.n,
                   tl > 0.0 ? rows[i].lum / tl : 0.0);
            if (!pcs3[ip].m_Lods.empty() && fi < pcs3[ip].m_Lods[0].m_Faces.size())
            {
                const LTABCFace& fc = pcs3[ip].m_Lods[0].m_Faces[fi];
                for (int k3 = 0; k3 < 3; ++k3)
                    printf(" (%.3f,%.3f)", fc.m_Verts[k3].m_fU, fc.m_Verts[k3].m_fV);
            }
            printf("   box %d,%d..%d,%d\n", a.x0, a.y0, a.x1, a.y1);
        }
    }

    // ⚠️ "0 pixels" and "it worked" must not look the same. HANDOVER.md §5.
    if (st.m_nPixels == 0)
    {
        fprintf(stderr, "NOTHING WAS DRAWN -- geometry loaded but no pixel was written\n");
        return 1;
    }

    if (!WritePPM(pOut, &rgba[0], nSize, nSize))
    {
        fprintf(stderr, "could not write %s\n", pOut);
        return 1;
    }
    printf("wrote  : %s (%dx%d)\n", pOut, nSize, nSize);
    return 0;
}
