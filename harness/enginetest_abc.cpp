// ----------------------------------------------------------------------- //
//
// enginetest_abc.cpp -- the ABC model reader, with negative controls.
//
// ⚠️ THIS TEST SWEEPS, IT DOES NOT SAMPLE
//
//   HANDOVER.md §5: "the ButeMgr grammar was derived from 3 files, and then
//   failed on 3 of 20 real ones." The ABC format has exactly that shape --
//   692 files agree on a layout and THREE do not -- so this enumerates every
//   `.abc` in the archives and loads all of them. A sampled version of this
//   test passes while the v9 and v11 files are broken.
//
// WHAT "PASSES" MEANS HERE, AND WHY IT IS A REAL STATEMENT
//
//   CLTABCModel::Load does not merely parse. The container declares where each
//   of the seven chunks ends, so the reader PREDICTS that offset and refuses
//   the file when it lands anywhere else. Every "loads" check below is
//   therefore an assertion that all seven boundaries were hit exactly -- the
//   per-chunk form of FORMATS.md §2's DTX size-prediction discipline.
//
//   Byte accounting still only proves LAYOUT, so the sweep also checks MEANING:
//   normals unit length, indices in range, header totals equal to what was
//   actually read.
//
// ----------------------------------------------------------------------- //

// ⚠️ Include order mirrors the game's Stdafx.h -- HANDOVER.md §4.3.
#include "windows.h"
#include "lithtech.h"
#include "mfcstub.h"

#include "ltabc.h"
#include "ltmodeldraw.h"
#include "ltrezfile.h"
#include "ltfilemgr.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <algorithm>
#include <string>
#include <vector>

static int g_nPass = 0;
static int g_nFail = 0;

static void ok(const char* w)             { printf("  ok    %s\n", w); ++g_nPass; }
static void bad(const char* w)            { printf("  FAIL  %s\n", w); ++g_nFail; }
static void check(bool b, const char* w)  { b ? ok(w) : bad(w); }


// Reads a whole file out of whichever tree wins, so a test can corrupt a copy
// of real data rather than a synthetic stand-in.
static bool SlurpThroughFileMgr(CLTFileMgr& fm, const char* pName,
                                std::vector<unsigned char>& out)
{
    out.clear();
    ILTStream* pStream = fm.OpenFile(pName);
    if (!pStream) return false;
    unsigned long nLen = 0;
    pStream->GetLen(&nLen);
    out.resize((size_t)nLen);
    bool bOK = (nLen > 0) && (pStream->Read(&out[0], nLen) == LT_OK);
    pStream->Release();
    if (!bOK) out.clear();
    return bOK;
}

static bool EndsWith(const std::string& s, const char* pSuffix)
{
    size_t n = strlen(pSuffix);
    if (s.size() < n) return false;
    return s.compare(s.size() - n, n, pSuffix) == 0;
}


int main(int argc, char** argv)
{
    const char* pDirs[8];
    int nDirs = 0;
    for (int i = 1; i < argc && nDirs < 8; ++i) pDirs[nDirs++] = argv[i];
    if (nDirs == 0) { fprintf(stderr, "usage: enginetest_abc <game-dir> ...\n"); return 2; }

    CLTFileMgr fm;
    if (fm.InitFromGameDirs(pDirs, nDirs) == 0) { fprintf(stderr, "no game data\n"); return 2; }

    // ----------------------------------------------------------------- //
    // Enumerate every .abc the archives hold.
    //
    // The names come from the archives directly, but each model is LOADED
    // through the file manager, so the tree stack decides which copy of a
    // duplicated name wins -- the same override chain the game gets.
    // ----------------------------------------------------------------- //
    std::vector<std::string> names;
    for (int d = 0; d < nDirs; ++d)
    {
        DIR* pDir = opendir(pDirs[d]);
        if (!pDir) continue;
        struct dirent* pEnt;
        while ((pEnt = readdir(pDir)) != 0)
        {
            std::string sLower(pEnt->d_name);
            std::transform(sLower.begin(), sLower.end(), sLower.begin(), ::tolower);
            if (!EndsWith(sLower, ".rez")) continue;

            std::string sPath(pDirs[d]);
            sPath += "/";
            sPath += pEnt->d_name;

            CLTRezArchive rez;
            if (!rez.Open(sPath.c_str())) continue;
            for (unsigned int i = 0; i < rez.GetNumEntries(); ++i)
            {
                std::string sName(rez.GetEntryName(i));
                std::string sUp(sName);
                std::transform(sUp.begin(), sUp.end(), sUp.begin(), ::toupper);
                if (EndsWith(sUp, ".ABC")) names.push_back(sUp);
            }
        }
        closedir(pDir);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());

    printf("\n--- sweep: every .abc in the archives ---\n");
    printf("        %u distinct model names\n", (unsigned int)names.size());
    check(names.size() >= 690, "the archives yield the expected ~695 models");

    // ----------------------------------------------------------------- //
    // Load all of them.
    // ----------------------------------------------------------------- //
    unsigned int nLoaded = 0, nFailed = 0;
    unsigned int nV9 = 0, nV11 = 0, nV12 = 0, nVOther = 0;
    unsigned int nTotalCountMismatch = 0;
    unsigned int nBadNormal = 0, nBadNodeIndex = 0, nBadWeightNode = 0;
    unsigned int nMultiLOD = 0, nWithSockets = 0, nWithChildren = 0;
    unsigned long long nTrisTotal = 0;
    std::string sFirstFailure, sFirstFailureWhy;
    double fMinNormal = 2.0, fMaxNormal = -1.0;

    for (size_t i = 0; i < names.size(); ++i)
    {
        CLTABCModel m;
        if (!m.LoadThroughFileMgr(&fm, names[i].c_str()))
        {
            ++nFailed;
            if (sFirstFailure.empty())
            {
                sFirstFailure = names[i];
                sFirstFailureWhy = m.GetError();
            }
            continue;
        }
        ++nLoaded;

        switch (m.GetVersion())
        {
            case 9:  ++nV9;  break;
            case 11: ++nV11; break;
            case 12: ++nV12; break;
            default: ++nVOther; break;
        }

        // Header totals must equal what was actually read -- and they are sums
        // over ALL LODs, not LOD 0 (FORMATS.md §3).
        if (m.GetNumTris() != m.CountTris() || m.GetNumVerts() != m.CountVerts())
            ++nTotalCountMismatch;
        nTrisTotal += m.CountTris();

        if (m.GetNumLODs() > 1) ++nMultiLOD;
        if (!m.GetSockets().empty()) ++nWithSockets;
        if (m.GetChildModels().size() > 1) ++nWithChildren;

        const std::vector<LTABCNode>& nodes = m.GetNodes();
        for (unsigned int n = 0; n < nodes.size(); ++n)
            if (nodes[n].m_nIndex != n) ++nBadNodeIndex;

        const std::vector<LTABCPiece>& pieces = m.GetPieces();
        for (size_t p = 0; p < pieces.size(); ++p)
        {
            for (size_t l = 0; l < pieces[p].m_Lods.size(); ++l)
            {
                const std::vector<LTABCVertex>& vs = pieces[p].m_Lods[l].m_Verts;
                for (size_t v = 0; v < vs.size(); ++v)
                {
                    const float* nrm = vs[v].m_Normal;
                    double len = sqrt((double)nrm[0]*nrm[0] +
                                      (double)nrm[1]*nrm[1] +
                                      (double)nrm[2]*nrm[2]);
                    if (len > 0.0)
                    {
                        if (len < fMinNormal) fMinNormal = len;
                        if (len > fMaxNormal) fMaxNormal = len;
                        if (fabs(len - 1.0) > 1e-3) ++nBadNormal;
                    }
                    for (size_t w = 0; w < vs[v].m_Weights.size(); ++w)
                        if (vs[v].m_Weights[w].m_nNode >= nodes.size())
                            ++nBadWeightNode;
                }
            }
        }
    }

    printf("        loaded %u, refused %u\n", nLoaded, nFailed);
    if (nFailed)
        printf("        first refusal: %s -- %s\n",
               sFirstFailure.c_str(), sFirstFailureWhy.c_str());

    check(nFailed == 0,
          "every shipped model loads, hitting all 7 chunk boundaries exactly");

    printf("        versions: v12 x%u, v11 x%u, v9 x%u, other x%u\n",
           nV12, nV11, nV9, nVOther);

    // ⚠️ THE DRIFT TRAP, ASSERTED. FORMATS.md §3 measured v12 x692, v11 x1,
    // v9 x2. A loader that assumed v12 fails on exactly three files out of
    // 695 -- so this test states the census rather than merely counting
    // successes, and would fail if the off-version files stopped being read.
    check(nV9 == 2 && nV11 == 1, "the 3 off-version models are present and read");
    check(nVOther == 0, "no model carries a version this reader does not know");

    check(nTotalCountMismatch == 0,
          "header nTris/nVerts equal the geometry actually read (all LODs)");
    check(nBadNodeIndex == 0, "every node index is its depth-first ordinal");
    check(nBadWeightNode == 0, "every vertex weight names a node that exists");

    // ⚠️ This is the check that pins the VERTEX LAYOUT rather than its size.
    // A normal read from the wrong offset is not a unit vector.
    printf("        normal length range %.6f .. %.6f over %llu triangles\n",
           fMinNormal, fMaxNormal, nTrisTotal);
    check(nBadNormal == 0, "every vertex normal is unit length");

    printf("        %u multi-LOD, %u with sockets, %u with child models\n",
           nMultiLOD, nWithSockets, nWithChildren);
    check(nMultiLOD > 100 && nWithSockets > 0 && nWithChildren > 0,
          "the sweep really exercised LODs, sockets and child models");

    // ----------------------------------------------------------------- //
    // THE NORMAL ORACLE -- skinning at the BIND pose must give the file's
    // own normal back.
    //
    // ⚠️ WHY THIS TEST EXISTS. The frame contract asserts SAMENESS and the
    // ABC sweep above asserts that the file was READ correctly; neither can
    // say whether the renderer USES a normal correctly. It did not: for a
    // day every posed model in the game was lit by a normal a mean of 92.8
    // degrees away from its surface, because `m_Normal` is stored in MODEL
    // space while `m_Offset` is stored in the BONE's, and the sum that is
    // right for a position is wrong for a normal (HANDOVER.md 5aq).
    //
    // The oracle needs no reference picture: the model states both sides.
    // At the bind pose R_i == R_bind_i, so R_i * R_bind_i^-1 is the identity
    // and the weighted sum is the stored normal exactly, whatever the biases.
    // ----------------------------------------------------------------- //
    printf("\n--- the normal oracle: bind-pose skinning reproduces the file ---\n");
    {
        double fWorstDeg = 0.0, fSumDeg = 0.0;
        double fWorstNoInv = 0.0;
        unsigned long long nChecked = 0, nOver = 0, nModels = 0;
        std::string sWorst;

        for (size_t i = 0; i < names.size(); ++i)
        {
            CLTABCModel m;
            if (!m.LoadThroughFileMgr(&fm, names[i].c_str())) continue;
            LTModelPose bind;
            if (!LTModelDraw::BuildBindPose(m, bind)) continue;
            ++nModels;

            const std::vector<LTABCPiece>& pcs = m.GetPieces();
            for (size_t ip = 0; ip < pcs.size(); ++ip)
            {
                if (pcs[ip].m_Lods.empty()) continue;
                const std::vector<LTABCVertex>& vv = pcs[ip].m_Lods[0].m_Verts;
                for (size_t iv = 0; iv < vv.size(); ++iv)
                {
                    const LTABCVertex& v = vv[iv];
                    if (v.m_Weights.empty()) continue;

                    float n3[3];
                    if (!LTModelDraw::SkinNormal(m, v, bind, n3)) continue;

                    double d = n3[0]*v.m_Normal[0] + n3[1]*v.m_Normal[1]
                             + n3[2]*v.m_Normal[2];
                    if (d >  1.0) d =  1.0;
                    if (d < -1.0) d = -1.0;
                    const double deg = acos(d) * 57.2957795;

                    ++nChecked;
                    fSumDeg += deg;
                    if (deg > 1.0) ++nOver;
                    if (deg > fWorstDeg) { fWorstDeg = deg; sWorst = names[i]; }

                    // ⚠️ THE NEGATIVE CONTROL, COMPUTED IN PLACE. Without the
                    // inverse bind rotation the same sum must NOT reproduce
                    // the normal -- otherwise this whole check is a tautology
                    // that would pass against the bug it exists to catch.
                    float w3[3] = { 0.0f, 0.0f, 0.0f };
                    for (size_t iw = 0; iw < v.m_Weights.size(); ++iw)
                    {
                        const LTABCWeight& wt = v.m_Weights[iw];
                        const float* B = bind.Get(wt.m_nNode);
                        if (!B) continue;
                        for (int k = 0; k < 3; ++k)
                            w3[k] += (B[k*4+0]*v.m_Normal[0] + B[k*4+1]*v.m_Normal[1]
                                    + B[k*4+2]*v.m_Normal[2]) * wt.m_fBias;
                    }
                    const double wl = sqrt((double)(w3[0]*w3[0]+w3[1]*w3[1]+w3[2]*w3[2]));
                    if (wl > 1e-9)
                    {
                        double dw = (w3[0]*v.m_Normal[0] + w3[1]*v.m_Normal[1]
                                   + w3[2]*v.m_Normal[2]) / wl;
                        if (dw >  1.0) dw =  1.0;
                        if (dw < -1.0) dw = -1.0;
                        const double degw = acos(dw) * 57.2957795;
                        if (degw > fWorstNoInv) fWorstNoInv = degw;
                    }
                }
            }
        }

        printf("        %llu weighted vertex/vertices over %llu model(s)\n",
               nChecked, nModels);
        printf("        worst %.4f deg, mean %.4f deg   (%s)\n",
               fWorstDeg, nChecked ? fSumDeg / (double)nChecked : 0.0,
               sWorst.empty() ? "-" : sWorst.c_str());

        check(nChecked > 100000,
              "the oracle really swept the shipped models");
        check(nOver == 0 && fWorstDeg < 1.0,
              "bind-pose skinning reproduces every stored normal");
        check(fWorstNoInv > 30.0,
              "NEGATIVE CONTROL: without the inverse bind rotation it does NOT");
        printf("        without the inverse: worst %.1f deg\n", fWorstNoInv);
    }

    // ----------------------------------------------------------------- //
    // THE POSITION ORACLE -- bind-pose skinning reproduces every stored
    // position, and the BIAS SUM is why it did not.
    //
    // ⚠️ 4,417 LOD-0 vertices in 197 shipped models have biases that do not
    // add up to 1 -- Cate's face has single weights of Head_node/0.95. The
    // offset is pre-scaled by the bias, so an undivided sum is bias * p and
    // the vertex is pulled toward the model origin: ~2 units at head height,
    // which put pale wedges across her cheeks (HANDOVER.md 5ar).
    //
    // ⚠️ A RESIDUAL REMAINS AND IS NOT THIS RULE'S: 365 LOD-0 vertices, all
    // SINGLE weights of exactly 1.000 (MonkeyMask, Baron_Action_Multi, ...),
    // disagree with their stored positions by up to 5.2 units. No bias rule
    // can touch a weight of 1.0; it is recorded as an open data question, and
    // guarded here so it cannot grow unnoticed.
    // ----------------------------------------------------------------- //
    printf("\n--- the position oracle: bind-pose skinning reproduces the file ---\n");
    {
        unsigned long long nV = 0, nOff = 0, nOffUndiv = 0, nCate = 0, nCateOff = 0;
        double fWorst = 0.0;
        std::string sWorst;

        for (size_t i = 0; i < names.size(); ++i)
        {
            CLTABCModel m;
            if (!m.LoadThroughFileMgr(&fm, names[i].c_str())) continue;
            LTModelPose bind;
            if (!LTModelDraw::BuildBindPose(m, bind)) continue;
            const bool bCate = (names[i].find("HERO_CASUAL") != std::string::npos);

            const std::vector<LTABCPiece>& pcs = m.GetPieces();
            for (size_t ip = 0; ip < pcs.size(); ++ip)
            {
                if (pcs[ip].m_Lods.empty()) continue;
                const std::vector<LTABCVertex>& vv = pcs[ip].m_Lods[0].m_Verts;
                for (size_t iv = 0; iv < vv.size(); ++iv)
                {
                    const LTABCVertex& v = vv[iv];
                    float p3[3];
                    if (!LTModelDraw::SkinPosition(m, v, bind, p3)) continue;
                    ++nV;
                    double d = 0.0;
                    for (int k = 0; k < 3; ++k) d += (p3[k]-v.m_Pos[k]) * (p3[k]-v.m_Pos[k]);
                    d = sqrt(d);
                    if (d > 0.1) ++nOff;
                    if (d > fWorst) { fWorst = d; sWorst = names[i]; }
                    if (bCate) { ++nCate; if (d > 0.1) ++nCateOff; }

                    // The NEGATIVE CONTROL, in place: the same sum undivided.
                    float fSum = 0.0f;
                    for (size_t iw = 0; iw < v.m_Weights.size(); ++iw) fSum += v.m_Weights[iw].m_fBias;
                    double du = 0.0;
                    for (int k = 0; k < 3; ++k)
                    { const double q = p3[k] * fSum - v.m_Pos[k]; du += q * q; }
                    if (sqrt(du) > 0.1) ++nOffUndiv;
                }
            }
        }

        printf("        %llu LOD-0 weighted vertices; %llu off by >0.1 unit (worst %.3f, %s)\n",
               nV, nOff, fWorst, sWorst.c_str());
        printf("        Cate (HERO_CASUAL*): %llu vertices, %llu off\n", nCate, nCateOff);
        printf("        undivided: %llu off\n", nOffUndiv);

        check(nV > 100000, "the oracle really swept the shipped models");
        check(nCate > 1000 && nCateOff == 0,
              "every one of Cate's vertices reproduces its stored position");
        check(nOff <= 365,
              "no more than the 365 known single-weight residuals (see HANDOVER 5ar)");
        check(nOffUndiv > 5000,
              "NEGATIVE CONTROL: without dividing by the bias sum, thousands do NOT");
    }

    // ----------------------------------------------------------------- //
    // ROTATIONONLY -- a bone keeps its OWN length in every animation.
    //
    // ⚠️ Node flag bit 1 means "take only rotation from the animation"; the
    // translation is the bind offset from the parent. Every bone of NOLF's
    // characters carries it, because most of their animations are MERGED from
    // a shared skeleton with other proportions. Using that skeleton's position
    // keys put Cate's head 0.73 units lower on her neck while her eyelid node
    // stayed put, and the lash pieces hung over her eyes (HANDOVER.md 5at).
    //
    // The oracle: for every flagged node, the POSED distance to its parent
    // equals the BIND distance -- exactly, in every animation. The negative
    // control is the position keys themselves, which must NOT preserve it.
    // ----------------------------------------------------------------- //
    printf("\n--- ROTATIONONLY: bones keep their own length in merged animations ---\n");
    {
        unsigned long long nChecks = 0, nBad = 0, nKeyBad = 0;
        double fWorst = 0.0, fWorstKey = 0.0;
        unsigned nModels = 0;
        const char* kChars[] = { "CHARS\\MODELS\\HERO_WINTER.ABC", "CHARS\\MODELS\\HERO_CASUALM1.ABC",
                                 "CHARS\\MODELS\\MOROCCANAGENT_ACTION.ABC", "CHARS\\MODELS\\PLAYER_ACTION.ABC" };
        for (size_t im = 0; im < sizeof(kChars) / sizeof(kChars[0]); ++im)
        {
            CLTABCModel m;
            if (!m.LoadThroughFileMgr(&fm, kChars[im])) continue;
            m.MergeChildAnimations(&fm, kChars[im]);
            ++nModels;
            const std::vector<LTABCNode>& nd = m.GetNodes();

            std::vector<int> par(nd.size(), -1);
            {
                std::vector<unsigned int> st, rem;
                for (size_t i = 0; i < nd.size(); ++i)
                {
                    while (!rem.empty() && rem.back() == 0) { rem.pop_back(); st.pop_back(); }
                    if (!st.empty()) par[i] = (int)st.back();
                    if (!rem.empty()) --rem.back();
                    if (nd[i].m_nChildren) { st.push_back((unsigned int)i); rem.push_back(nd[i].m_nChildren); }
                }
            }

            const std::vector<LTABCAnim>& an = m.GetAnims();
            for (size_t ia = 0; ia < an.size(); ++ia)
            {
                const size_t nKf = an[ia].m_Keyframes.size();
                if (!nKf || an[ia].m_Transforms.size() != nd.size()) continue;
                const size_t kfs[2] = { 0, nKf / 2 };
                for (int s = 0; s < 2; ++s)
                {
                    LTModelPose pose;
                    if (!LTModelDraw::BuildPose(m, (unsigned)ia, (unsigned)kfs[s], pose)) continue;
                    for (size_t n = 0; n < nd.size(); ++n)
                    {
                        if (!(nd[n].m_nFlags & 2) || par[n] < 0) continue;
                        const float* A = pose.Get((unsigned)n);
                        const float* B = pose.Get((unsigned)par[n]);
                        const float* NB = nd[n].m_Matrix;
                        const float* PB = nd[(size_t)par[n]].m_Matrix;
                        const double lp = sqrt((double)((A[3]-B[3])*(A[3]-B[3]) + (A[7]-B[7])*(A[7]-B[7]) + (A[11]-B[11])*(A[11]-B[11])));
                        const double lb = sqrt((double)((NB[3]-PB[3])*(NB[3]-PB[3]) + (NB[7]-PB[7])*(NB[7]-PB[7]) + (NB[11]-PB[11])*(NB[11]-PB[11])));
                        const LTABCTransform& t = an[ia].m_Transforms[n][kfs[s]];
                        const double lk = sqrt((double)(t.m_Pos[0]*t.m_Pos[0] + t.m_Pos[1]*t.m_Pos[1] + t.m_Pos[2]*t.m_Pos[2]));
                        ++nChecks;
                        if (fabs(lp - lb) > 0.01) ++nBad;
                        if (fabs(lp - lb) > fWorst) fWorst = fabs(lp - lb);
                        if (fabs(lk - lb) > 0.1) ++nKeyBad;
                        if (fabs(lk - lb) > fWorstKey) fWorstKey = fabs(lk - lb);
                    }
                }
            }
        }
        printf("        %u character model(s), %llu bone/keyframe checks; posed vs bind worst %.5f\n",
               nModels, nChecks, fWorst);
        printf("        the animations' own position keys: %llu off by >0.1, worst %.2f\n", nKeyBad, fWorstKey);
        check(nModels == 4 && nChecks > 10000, "the sweep reached four characters' merged animations");
        check(nBad == 0, "every ROTATIONONLY bone keeps its bind length in every animation");
        check(nKeyBad > 100, "NEGATIVE CONTROL: the position keys alone do NOT keep it");
    }

    // ----------------------------------------------------------------- //
    // EXTRA TRACKERS -- the blink, blended through its weight set.
    //
    // ⚠️ CCharacter plays `blink` on a second tracker through the `Blink`
    // weight set (Eyelid = 2.00 -> ADDITIVE, relative to the layer's frame 0).
    // Frame 0 is the open lid; key 2 (t=2066) is the 56.4-degree close. So:
    // at an open key nothing may move; at the close ONLY the Eyelid subtree
    // may move, by 56.4 degrees; and a weight set that names other nodes must
    // move them (negative control -- the layer is really being read).
    // ----------------------------------------------------------------- //
    printf("\n--- extra trackers: the blink layer ---\n");
    {
        CLTABCModel m;
        const char* kHero = "CHARS\\MODELS\\HERO_WINTER.ABC";
        const bool bLoad = m.LoadThroughFileMgr(&fm, kHero);
        if (bLoad) m.MergeChildAnimations(&fm, kHero);
        check(bLoad, "HERO_WINTER.ABC loads for the blink test");

        int nBlink = -1, nMain = 351, wsBlink = -1, wsNull = -1, wsUpper = -1, nEyelid = -1, nHead = -1;
        const std::vector<LTABCAnim>& an = m.GetAnims();
        const std::vector<LTABCNode>& nd = m.GetNodes();
        const std::vector<LTABCWeightSet>& ws = m.GetWeightSets();
        for (size_t i = 0; i < an.size(); ++i) if (!strcasecmp(an[i].m_sName.c_str(), "blink")) nBlink = (int)i;
        for (size_t i = 0; i < ws.size(); ++i)
        {
            if (!strcasecmp(ws[i].m_sName.c_str(), "Blink")) wsBlink = (int)i;
            if (!strcasecmp(ws[i].m_sName.c_str(), "Null"))  wsNull  = (int)i;
            if (!strcasecmp(ws[i].m_sName.c_str(), "Upper")) wsUpper = (int)i;
        }
        for (size_t i = 0; i < nd.size(); ++i)
        {
            if (!strcasecmp(nd[i].m_sName.c_str(), "Eyelid"))    nEyelid = (int)i;
            if (!strcasecmp(nd[i].m_sName.c_str(), "Head_node")) nHead   = (int)i;
        }
        check(nBlink >= 0 && wsBlink >= 0 && wsNull >= 0 && wsUpper >= 0 && nEyelid >= 0 && nHead >= 0
              && (size_t)nMain < an.size() && an[nBlink].m_Keyframes.size() == 13,
              "found anim `blink` (13 keys), weight sets Blink/Null/Upper, nodes Eyelid/Head_node");

        if (nBlink >= 0 && wsBlink >= 0 && wsNull >= 0 && wsUpper >= 0 && nEyelid >= 0 && nHead >= 0)
        {
            // The Eyelid's subtree, from the depth-first child counts.
            std::vector<bool> inLid(nd.size(), false);
            {
                unsigned rem = 1;
                for (size_t i = (size_t)nEyelid; i < nd.size() && rem; ++i)
                { inLid[i] = true; --rem; rem += nd[i].m_nChildren; }
            }

            LTModelPose p0, pOpen, pShut, pNull, pUpper;
            LTModelDraw::BuildPose(m, (unsigned)nMain, 0, p0);
            LTModelDraw::Layer L = { (unsigned)nBlink, 1u, wsBlink };      // t=2000, open
            LTModelDraw::BuildPose(m, (unsigned)nMain, 0, pOpen, 0, 0, &L, 1);
            L.m_nKeyframe = 2;                                               // t=2066, closed
            LTModelDraw::BuildPose(m, (unsigned)nMain, 0, pShut, 0, 0, &L, 1);
            L.m_nWeightSet = wsNull;
            LTModelDraw::BuildPose(m, (unsigned)nMain, 0, pNull, 0, 0, &L, 1);
            L.m_nWeightSet = wsUpper;
            LTModelDraw::BuildPose(m, (unsigned)nMain, 0, pUpper, 0, 0, &L, 1);

            double dOpen = 0, dOutside = 0, dNull = 0;
            for (unsigned n = 0; n < nd.size(); ++n)
                for (int k = 0; k < 16; ++k)
                {
                    dOpen = std::max(dOpen, (double)fabsf(pOpen.Get(n)[k] - p0.Get(n)[k]));
                    dNull = std::max(dNull, (double)fabsf(pNull.Get(n)[k] - p0.Get(n)[k]));
                    if (!inLid[n]) dOutside = std::max(dOutside, (double)fabsf(pShut.Get(n)[k] - p0.Get(n)[k]));
                }

            // The angle between the closed and the plain Eyelid, globally.
            const float* A = p0.Get((unsigned)nEyelid);
            const float* B = pShut.Get((unsigned)nEyelid);
            double tr = 0;
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) tr += A[j*4+i] * B[j*4+i];
            double c = (tr - 1.0) / 2.0; if (c > 1) c = 1; if (c < -1) c = -1;
            const double fAngle = acos(c) * 57.29578;

            double dUpperHead = 0;
            for (int k = 0; k < 16; ++k)
                dUpperHead = std::max(dUpperHead, (double)fabsf(pUpper.Get((unsigned)nHead)[k] - p0.Get((unsigned)nHead)[k]));

            printf("        open key: max change %.6f; closed key: outside the lid %.6f, "
                   "Eyelid turned %.2f deg; Null %.6f; Upper moves Head_node by %.4f\n",
                   dOpen, dOutside, fAngle, dNull, dUpperHead);
            check(dOpen < 1e-4, "at an OPEN blink key nothing moves");
            check(dOutside == 0.0, "at the CLOSED key nothing outside the Eyelid subtree moves (exactly)");
            check(fabs(fAngle - 56.4) < 0.5, "at the CLOSED key the Eyelid turns the key's 56.4 degrees");
            check(dNull == 0.0, "NEGATIVE CONTROL: the same layer through `Null` changes nothing");
            check(dUpperHead > 0.01, "NEGATIVE CONTROL: through `Upper` (slerp 1.0) the head DOES move");
        }
    }

    // ----------------------------------------------------------------- //
    // A model small enough to state completely.
    // ----------------------------------------------------------------- //
    printf("\n--- a model with exactly known contents ---\n");
    {
        CLTABCModel m;
        check(m.LoadThroughFileMgr(&fm, "GUNS\\MODELS_PV\\LASERSPRITE.ABC"),
              "LASERSPRITE.ABC loads");
        check(m.GetVersion() == 12, "it is version 12");
        check(m.GetNumTris() == 2 && m.GetNumVerts() == 4,
              "2 triangles, 4 vertices");
        check(m.GetPieces().size() == 1 && m.GetNodes().size() == 1,
              "1 piece, 1 node");
        check(strcmp(m.GetCommandString(), "FovXOffset 20;FovYOffset 50") == 0,
              "its command string reads back exactly");
        check(m.GetPieces()[0].m_sName == "smallsprite", "the piece is named");
        check(m.GetAnims().size() == 1 && m.GetAnims()[0].m_sName == "Base",
              "one animation, called Base");

        // ⚠️ TWO CHUNKS AGREEING WITH EACH OTHER. The node's static matrix is
        // diag(-1,-1,1,1) and the keyframe quaternion is (0,0,1,0) -- the same
        // 180-degree turn about Z, stored two different ways in two different
        // chunks. Neither could be produced by a misaligned read of the other.
        const float* mat = m.GetNodes()[0].m_Matrix;
        check(mat[0] == -1.0f && mat[5] == -1.0f && mat[10] == 1.0f && mat[15] == 1.0f,
              "the node matrix is diag(-1,-1,1,1)");
        const LTABCTransform& t = m.GetAnims()[0].m_Transforms[0][0];
        check(t.m_Quat[0] == 0.0f && t.m_Quat[1] == 0.0f &&
              t.m_Quat[2] == 1.0f && t.m_Quat[3] == 0.0f,
              "and the keyframe quaternion is the same rotation, (0,0,1,0)");

        // The vertex `offset` is the node-space image of `pos`: this node
        // negates X and Y, so offset.xy == -pos.xy and offset.z == pos.z.
        const LTABCVertex& v0 = m.GetPieces()[0].m_Lods[0].m_Verts[0];
        check(v0.m_Weights.size() == 1, "its vertices carry one weight each");
        const float* off = v0.m_Weights[0].m_Offset;
        check(fabs(off[0] + v0.m_Pos[0]) < 1e-6f &&
              fabs(off[1] + v0.m_Pos[1]) < 1e-6f &&
              fabs(off[2] - v0.m_Pos[2]) < 1e-6f,
              "and weight offsets are pos through that same node transform");
    }

    // ----------------------------------------------------------------- //
    // Negative controls. Each of these must be REFUSED.
    // ----------------------------------------------------------------- //
    printf("\n--- negative controls ---\n");

    {
        CLTABCModel m;
        check(!m.Load(0, 0), "NEGATIVE CONTROL: a null buffer is refused");
        check(!m.IsLoaded(), "and the model stays empty");
    }
    {
        // A real PCX offered as a model.
        std::vector<unsigned char> buf;
        if (SlurpThroughFileMgr(fm, "interface\\pause.pcx", buf))
        {
            CLTABCModel m;
            check(!m.Load(&buf[0], (unsigned int)buf.size()),
                  "NEGATIVE CONTROL: a PCX offered as a model is refused");
            printf("        said: %s\n", m.GetError());
        }
        else bad("could not read a PCX to offer as a model");
    }

    std::vector<unsigned char> good;
    if (!SlurpThroughFileMgr(fm, "GUNS\\MODELS_PV\\LASERSPRITE.ABC", good))
    {
        bad("could not read a model to corrupt");
    }
    else
    {
        {
            CLTABCModel m;
            check(m.Load(&good[0], (unsigned int)good.size()),
                  "the uncorrupted copy still loads (the control's control)");
        }
        {
            // Truncation. ⚠️ A reader that returned what it managed to parse
            // would produce a model with missing geometry rather than an
            // error -- the failure this whole class of check exists to catch.
            CLTABCModel m;
            check(!m.Load(&good[0], (unsigned int)good.size() - 40),
                  "NEGATIVE CONTROL: a truncated model is refused");
            printf("        said: %s\n", m.GetError());
        }
        {
            // ⚠️ THE VERSION GATE, PROVEN LOAD-BEARING AT RUNTIME.
            //
            // Claim the v12 layout for data that is not v12 by rewriting only
            // the version word of a v12 file to 9. Everything else is real,
            // untouched, valid data -- so the only reason this can fail is
            // that v9 genuinely has a different layout (no piece lodWeight, no
            // anim interpTime, no anim length). If the version gates were
            // decoration, this would load happily.
            std::vector<unsigned char> v = good;
            v[12] = 9; v[13] = 0; v[14] = 0; v[15] = 0;
            CLTABCModel m;
            check(!m.Load(&v[0], (unsigned int)v.size()),
                  "NEGATIVE CONTROL: a v12 file relabelled v9 is refused");
            printf("        said: %s\n", m.GetError());
        }
        {
            // And an unknown version is refused by name rather than assumed
            // to resemble v12.
            std::vector<unsigned char> v = good;
            v[12] = 13; v[13] = 0; v[14] = 0; v[15] = 0;
            CLTABCModel m;
            check(!m.Load(&v[0], (unsigned int)v.size()),
                  "NEGATIVE CONTROL: an unknown version is refused, not guessed");
            printf("        said: %s\n", m.GetError());
        }
        {
            // ⚠️ THE BOUNDARY CHECK ITSELF, EXERCISED SURGICALLY.
            //
            // Moving a chunk's declared end is the obvious way to test this
            // and it is the WRONG way: the offsets are absolute, so shifting
            // one derails the directory walk and the file is refused a step
            // earlier, for a different reason. The check under test would
            // never run, and the control would still look green.
            //
            // So corrupt CONTENT instead, in the last chunk, in the one way
            // that changes how much is consumed without tripping any count
            // check: shorten the sole anim binding's name by one byte. The
            // directory stays perfectly valid; the parse simply stops one
            // byte short of where the container says it must.
            unsigned int o = 0, nAnimBindStart = 0;
            for (;;)
            {
                unsigned int nLen = (unsigned int)good[o] | ((unsigned int)good[o+1] << 8);
                std::string sName((const char*)&good[o+2], nLen);
                unsigned int nOff = o + 2 + nLen;
                unsigned int nNext = (unsigned int)good[nOff] | ((unsigned int)good[nOff+1] << 8) |
                                     ((unsigned int)good[nOff+2] << 16) | ((unsigned int)good[nOff+3] << 24);
                if (sName == "AnimBindings") { nAnimBindStart = nOff + 4; break; }
                if (nNext == 0xFFFFFFFFu) break;
                o = nNext;
            }
            check(nAnimBindStart != 0, "located the AnimBindings chunk to corrupt");

            std::vector<unsigned char> v = good;
            unsigned int nNameLenAt = nAnimBindStart + 4;   // after the block count
            check(v[nNameLenAt] == 4 && v[nNameLenAt + 1] == 0,
                  "its sole binding name is the expected 4 bytes");
            v[nNameLenAt] = 3;                              // "Base" -> "Bas"

            CLTABCModel m;
            check(!m.Load(&v[0], (unsigned int)v.size()),
                  "NEGATIVE CONTROL: a chunk that ends elsewhere than it says is refused");
            printf("        said: %s\n", m.GetError());
            check(strstr(m.GetError(), "AnimBindings") != 0 &&
                  strstr(m.GetError(), "off by") != 0,
                  "and it names the chunk and the shortfall, not just 'bad file'");
        }
        {
            // A model whose name no tree holds.
            CLTABCModel m;
            check(!m.LoadThroughFileMgr(&fm, "CHARS\\MODELS\\NOT_A_REAL_MODEL.ABC"),
                  "NEGATIVE CONTROL: a model that does not exist is refused");
            printf("        said: %s\n", m.GetError());
        }
    }

    printf("\n=== %d passed, %d failed ===\n", g_nPass, g_nFail);
    return g_nFail ? 1 : 0;
}
