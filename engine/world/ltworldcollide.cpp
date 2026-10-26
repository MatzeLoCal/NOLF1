// ----------------------------------------------------------------------- //
//
// MODULE  : ltworldcollide.cpp
//
// See ltworldcollide.h.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltworldcollide.h"
#include "ltworld.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    const unsigned int kWIF_VISBSP  = (1u << 5);
    const unsigned int kSURF_SOLID  = (1u << 0);
    const unsigned int kSURF_SKY    = (1u << 4);

    inline float Dot(const float* a, const float* b)
    { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }

    inline void Sub(const float* a, const float* b, float* o)
    { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }

    // Closest point on triangle ABC to P -- the standard barycentric region
    // test. Handles the vertex and edge regions, which is what stops a sphere
    // slipping through the seam between two triangles.
    void ClosestOnTri(const float* p, const float* a, const float* b,
                      const float* c, float* out)
    {
        float ab[3], ac[3], ap[3];
        Sub(b, a, ab); Sub(c, a, ac); Sub(p, a, ap);

        float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f) { memcpy(out, a, sizeof(float)*3); return; }

        float bp[3]; Sub(p, b, bp);
        float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) { memcpy(out, b, sizeof(float)*3); return; }

        float vc = d1*d4 - d3*d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
        {
            float v = d1 / (d1 - d3);
            for (int k = 0; k < 3; ++k) out[k] = a[k] + ab[k] * v;
            return;
        }

        float cp[3]; Sub(p, c, cp);
        float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
        if (d6 >= 0.0f && d5 <= d6) { memcpy(out, c, sizeof(float)*3); return; }

        float vb = d5*d2 - d1*d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
        {
            float w = d2 / (d2 - d6);
            for (int k = 0; k < 3; ++k) out[k] = a[k] + ac[k] * w;
            return;
        }

        float va = d3*d6 - d5*d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
        {
            float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
            for (int k = 0; k < 3; ++k) out[k] = b[k] + (c[k] - b[k]) * w;
            return;
        }

        float denom = 1.0f / (va + vb + vc);
        float v = vb * denom, w = vc * denom;
        for (int k = 0; k < 3; ++k) out[k] = a[k] + ab[k]*v + ac[k]*w;
    }
}


CLTWorldCollide::CLTWorldCollide()
    : m_CellSize(256.0f), m_bBuilt(false)
{
    m_Min[0] = m_Min[1] = m_Min[2] = 0.0f;
    m_Dim[0] = m_Dim[1] = m_Dim[2] = 0;
}


void CLTWorldCollide::SetModelOff(unsigned int nModel, bool bOff)
{
    if (nModel >= m_ModelOff.size()) m_ModelOff.resize(nModel + 1, 0);
    if ((m_ModelOff[nModel] != 0) == bOff) return;
    m_ModelOff[nModel] = bOff ? 1 : 0;
    if (bOff) ++m_nModelsOff; else --m_nModelsOff;
}


void CLTWorldCollide::Build(const CLTWorld& world)
{
    m_Tris.clear();
    m_TriPoly.clear();
    m_ModelOff.clear();
    m_nModelsOff = 0;
    m_Cells.clear();
    m_bBuilt = false;

    const std::vector<LTWorldModelHeader>& mdl = world.GetModels();

    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };

    for (size_t i = 0; i < mdl.size(); ++i)
    {
        const LTWorldModelHeader& M = mdl[i];
        if (!M.m_bBodyParsed) continue;

        // ⚠️ Same exclusions as the renderer, for the same reasons. VisBSP
        // duplicates the main world -- colliding against both is harmless but
        // doubles the triangles. AIVolume brushes are the important one: they
        // ARE flagged solid, so including them would wall off every corridor
        // they cover with an invisible barrier.
        if (M.m_nInfoFlags & kWIF_VISBSP) continue;
        if (M.m_sName.compare(0, 8, "AIVolume") == 0) continue;

        // Doors are dynamic -- see SetDynamic.
        bool bSkip = false;
        for (size_t e = 0; e < m_Excluded.size() && !bSkip; ++e)
            if (m_Excluded[e] == M.m_sName) bSkip = true;
        if (bSkip) continue;

        for (size_t q = 0; q < M.m_Polys.size(); ++q)
        {
            const LTWorldPoly& po = M.m_Polys[q];
            if (po.m_Verts.size() < 3) continue;
            if (po.m_nSurface >= M.m_Surfaces.size()) continue;

            const LTWorldSurface& sf = M.m_Surfaces[po.m_nSurface];

            // ⚠️ SOLID, not visible. Invisible.dtx surfaces are clip brushes --
            // the level designer's invisible walls -- and they carry SURF_SOLID
            // precisely so the player is stopped by them. Colliding only with
            // what can be seen would let you walk out of the level.
            if (!(sf.m_nFlags & kSURF_SOLID)) continue;
            if (sf.m_nFlags & kSURF_SKY) continue;

            for (size_t k = 1; k + 1 < po.m_Verts.size(); ++k)
            {
                const unsigned short idx[3] = { po.m_Verts[0].m_nPoint,
                                                po.m_Verts[k].m_nPoint,
                                                po.m_Verts[k+1].m_nPoint };
                bool bOK = true;
                for (int v = 0; v < 3 && bOK; ++v)
                    if (idx[v] >= M.m_Points.size()) bOK = false;
                if (!bOK) continue;

                m_TriPoly.push_back(LTWorldPolyId((unsigned)i, (unsigned)q));
                for (int v = 0; v < 3; ++v)
                {
                    const float* p = M.m_Points[idx[v]].m_Pos;
                    for (int a = 0; a < 3; ++a)
                    {
                        m_Tris.push_back(p[a]);
                        if (p[a] < mn[a]) mn[a] = p[a];
                        if (p[a] > mx[a]) mx[a] = p[a];
                    }
                }
            }
        }
    }

    if (m_Tris.empty()) return;

    // ---- the grid -------------------------------------------------------- //
    //
    // ⚠️ Brute force is not an option at 60 Hz: a level has tens of thousands
    // of collision triangles and a move test would touch every one of them
    // several times per frame. The grid turns that into the handful in the
    // cells the sphere actually overlaps.
    for (int k = 0; k < 3; ++k)
    {
        m_Min[k] = mn[k] - 1.0f;
        float fSpan = (mx[k] + 1.0f) - m_Min[k];
        m_Dim[k] = (int)(fSpan / m_CellSize) + 1;
        if (m_Dim[k] < 1) m_Dim[k] = 1;
        if (m_Dim[k] > 512) m_Dim[k] = 512;     // keep the table bounded
    }

    m_Cells.resize((size_t)m_Dim[0] * m_Dim[1] * m_Dim[2]);

    const int nTris = (int)(m_Tris.size() / 9);
    for (int i = 0; i < nTris; ++i)
    {
        const float* t = &m_Tris[(size_t)i * 9];
        float tmn[3], tmx[3];
        for (int a = 0; a < 3; ++a)
        {
            tmn[a] = tmx[a] = t[a];
            for (int v = 1; v < 3; ++v)
            {
                float f = t[v*3 + a];
                if (f < tmn[a]) tmn[a] = f;
                if (f > tmx[a]) tmx[a] = f;
            }
        }

        int lo[3], hi[3];
        CellRange(tmn, tmx, lo, hi);
        for (int z = lo[2]; z <= hi[2]; ++z)
            for (int y = lo[1]; y <= hi[1]; ++y)
                for (int x = lo[0]; x <= hi[0]; ++x)
                    m_Cells[((size_t)z * m_Dim[1] + y) * m_Dim[0] + x].push_back(i);
    }

    m_bBuilt = true;
}


void CLTWorldCollide::CellRange(const float* pMin3, const float* pMax3,
                                int* pLo3, int* pHi3) const
{
    for (int k = 0; k < 3; ++k)
    {
        int lo = (int)((pMin3[k] - m_Min[k]) / m_CellSize);
        int hi = (int)((pMax3[k] - m_Min[k]) / m_CellSize);
        if (lo < 0) lo = 0;
        if (hi < 0) hi = 0;
        if (lo > m_Dim[k] - 1) lo = m_Dim[k] - 1;
        if (hi > m_Dim[k] - 1) hi = m_Dim[k] - 1;
        pLo3[k] = lo; pHi3[k] = hi;
    }
}


void CLTWorldCollide::Gather(const float* pMin3, const float* pMax3,
                             std::vector<int>& out) const
{
    out.clear();
    if (!m_bBuilt) return;

    int lo[3], hi[3];
    CellRange(pMin3, pMax3, lo, hi);

    for (int z = lo[2]; z <= hi[2]; ++z)
        for (int y = lo[1]; y <= hi[1]; ++y)
            for (int x = lo[0]; x <= hi[0]; ++x)
            {
                const std::vector<int>& c =
                    m_Cells[((size_t)z * m_Dim[1] + y) * m_Dim[0] + x];
                for (size_t i = 0; i < c.size(); ++i) out.push_back(c[i]);
            }

    // ⚠️ A triangle spanning several cells appears several times. Left as is:
    // testing one twice costs a few instructions, while de-duplicating costs a
    // sort on every move.
}


bool CLTWorldCollide::Contact(const float* pC3, float fRadius, int nTri,
                              float* pNormal3, float* pDepth) const
{
    const float* t = &m_Tris[(size_t)nTri * 9];

    float closest[3];
    ClosestOnTri(pC3, t, t + 3, t + 6, closest);

    float d[3]; Sub(pC3, closest, d);
    float len2 = Dot(d, d);
    if (len2 >= fRadius * fRadius) return false;

    float len = sqrtf(len2);
    if (len > 1e-4f)
    {
        for (int k = 0; k < 3; ++k) pNormal3[k] = d[k] / len;
    }
    else
    {
        // ⚠️ Dead centre on the triangle's plane: the direction from the
        // contact point is undefined, so fall back to the face normal. Without
        // this the sphere gets a zero push and stays embedded.
        float ab[3], ac[3];
        Sub(t + 3, t, ab); Sub(t + 6, t, ac);
        pNormal3[0] = ab[1]*ac[2] - ab[2]*ac[1];
        pNormal3[1] = ab[2]*ac[0] - ab[0]*ac[2];
        pNormal3[2] = ab[0]*ac[1] - ab[1]*ac[0];
        float nl = sqrtf(Dot(pNormal3, pNormal3));
        if (nl < 1e-6f) return false;
        for (int k = 0; k < 3; ++k) pNormal3[k] /= nl;
    }

    *pDepth = fRadius - len;
    return true;
}


void CLTWorldCollide::SetDynamic(const std::vector<float>& tris)
{
    m_Dyn = tris;
    m_DynOwner.clear();         // owners belong to one list; set them after
    m_DynPoly.clear();
}


// Deepest contact against a flat triangle list, used for the dynamic set.
bool CLTWorldCollide::ContactAny(const float* pC3, float fRadius, const float* pTris,
                                 int nTris, float* pNormal3, float* pDepth, int* pTri) const
{
    bool  bAny = false;
    float fDeep = 0.0f;

    for (int i = 0; i < nTris; ++i)
    {
        const float* t = pTris + (size_t)i * 9;
        float closest[3];
        ClosestOnTri(pC3, t, t + 3, t + 6, closest);

        float d[3]; Sub(pC3, closest, d);
        float len2 = Dot(d, d);
        if (len2 >= fRadius * fRadius) continue;

        float len = sqrtf(len2);
        float n[3];
        if (len > 1e-4f) { for (int k = 0; k < 3; ++k) n[k] = d[k] / len; }
        else
        {
            float ab[3], ac[3];
            Sub(t + 3, t, ab); Sub(t + 6, t, ac);
            n[0] = ab[1]*ac[2] - ab[2]*ac[1];
            n[1] = ab[2]*ac[0] - ab[0]*ac[2];
            n[2] = ab[0]*ac[1] - ab[1]*ac[0];
            float nl = sqrtf(Dot(n, n));
            if (nl < 1e-6f) continue;
            for (int k = 0; k < 3; ++k) n[k] /= nl;
        }

        float depth = fRadius - len;
        if (depth > fDeep)
        { fDeep = depth; memcpy(pNormal3, n, sizeof(float)*3); bAny = true; if (pTri) *pTri = i; }
    }

    *pDepth = fDeep;
    return bAny;
}


bool CLTWorldCollide::Overlaps(const float* pPos3, float fRadius) const
{
    if (!m_bBuilt) return false;

    float mn[3], mx[3];
    for (int k = 0; k < 3; ++k) { mn[k] = pPos3[k] - fRadius; mx[k] = pPos3[k] + fRadius; }

    std::vector<int> cand;
    Gather(mn, mx, cand);

    float n[3], depth;
    for (size_t i = 0; i < cand.size(); ++i)
        if (!TriOff(cand[i]) && Contact(pPos3, fRadius, cand[i], n, &depth)) return true;

    if (!m_Dyn.empty()
        && ContactAny(pPos3, fRadius, &m_Dyn[0], (int)(m_Dyn.size()/9), n, &depth))
        return true;
    return false;
}


void CLTWorldCollide::MoveSphere(float* pPos3, const float* pDelta3,
                                 float fRadius, bool* pbGround) const
{
    MoveSphere(pPos3, pDelta3, fRadius, pbGround, 0);
}

void CLTWorldCollide::MoveSphere(float* pPos3, const float* pDelta3,
                                 float fRadius, bool* pbGround, Contacts* pContacts) const
{
    if (pbGround) *pbGround = false;

    if (!m_bBuilt)
    {
        for (int k = 0; k < 3; ++k) pPos3[k] += pDelta3[k];
        return;
    }

    // ⚠️ SUBSTEP, or a fast move tunnels. This resolver only pushes out of what
    // it overlaps AT THE DESTINATION -- it is not a swept test -- so a step
    // longer than the sphere can land cleanly on the far side of a wall having
    // never touched it. Running (1,100 units/s) is 18 units per 60Hz frame,
    // under the radius; but the frame delta is clamped at 0.1s, and one long
    // frame is 110 units. Splitting the move into steps no longer than half a
    // radius makes the failure impossible rather than unlikely.
    float len = sqrtf(pDelta3[0]*pDelta3[0] + pDelta3[1]*pDelta3[1] + pDelta3[2]*pDelta3[2]);
    int nSteps = 1 + (int)(len / (fRadius * 0.5f));

    // ⚠️ THE CAP MUST NOT DEFEAT THE SUBSTEPPING, and at first it did. A cap of
    // 64 turned a 4,000-unit fall into 62-unit steps -- larger than the sphere,
    // so it tunnelled exactly as if there were no substepping at all, and the
    // test that compared one long fall against four hundred short ones caught
    // it. The cap exists only to stop a runaway delta hanging the frame, so it
    // has to sit far above any move a frame can legitimately ask for.
    if (nSteps > 512) nSteps = 512;

    float step[3];
    for (int k = 0; k < 3; ++k) step[k] = pDelta3[k] / (float)nSteps;

    float pos[3] = { pPos3[0], pPos3[1], pPos3[2] };
    std::vector<int> cand;

    for (int s = 0; s < nSteps; ++s)
    {
    for (int k = 0; k < 3; ++k) pos[k] += step[k];

    // ⚠️ ITERATE. One push-out per frame leaves the sphere embedded in the
    // second surface of any corner, and the next frame pushes it out of that
    // one back into the first -- the classic corner jitter. Resolving the
    // deepest contact repeatedly converges instead.
    const int kIterations = 6;

    for (int it = 0; it < kIterations; ++it)
    {
        float mn[3], mx[3];
        for (int k = 0; k < 3; ++k) { mn[k] = pos[k] - fRadius; mx[k] = pos[k] + fRadius; }
        Gather(mn, mx, cand);

        int   nDeepest = -1;
        bool  bDeepDoor = false;
        int   nDoorTri = -1;
        float fDeepest = 0.0f;
        float nBest[3] = { 0, 1, 0 };

        for (size_t i = 0; i < cand.size(); ++i)
        {
            float n[3], depth;
            if (TriOff(cand[i]) || !Contact(pos, fRadius, cand[i], n, &depth)) continue;
            if (depth > fDeepest)
            {
                fDeepest = depth; nDeepest = cand[i];
                memcpy(nBest, n, sizeof(nBest));
            }
        }

        // ⚠️ The doors are tested in the SAME loop, so a body wedged between a
        // door and a wall resolves against whichever is deeper -- separate
        // passes would fight each other.
        if (!m_Dyn.empty())
        {
            float n[3], depth;
            int nTri = -1;
            if (ContactAny(pos, fRadius, &m_Dyn[0], (int)(m_Dyn.size()/9), n, &depth, &nTri)
                && depth > fDeepest)
            { fDeepest = depth; nDeepest = 0; bDeepDoor = true; nDoorTri = nTri; memcpy(nBest, n, sizeof(nBest)); }
        }

        if (nDeepest < 0) break;

        if (pContacts)
        {
            if (pContacts->m_nHits < Contacts::kMaxNormals)
                memcpy(pContacts->m_Normals[pContacts->m_nHits], nBest, sizeof(nBest));
            ++pContacts->m_nHits;
            for (int k = 0; k < 3; ++k) pContacts->m_Point[k] = pos[k] - nBest[k] * (fRadius - fDeepest);
            pContacts->m_nPoly = (!bDeepDoor && nDeepest < (int)m_TriPoly.size()) ? m_TriPoly[nDeepest] : 0u;
            pContacts->m_pOwner = (bDeepDoor && nDoorTri >= 0 && nDoorTri < (int)m_DynOwner.size())
                                ? m_DynOwner[nDoorTri] : nullptr;
            if (bDeepDoor && nDoorTri >= 0 && nDoorTri < (int)m_DynPoly.size())
                pContacts->m_nPoly = m_DynPoly[nDoorTri];
        }

        // Push out just past touching. The epsilon keeps the next frame's test
        // from finding the same contact at depth ~0 forever.
        for (int k = 0; k < 3; ++k) pos[k] += nBest[k] * (fDeepest + 0.01f);

        // ⚠️ Flat enough to stand on. 0.7 is about 45 degrees -- steeper than
        // any stair and shallower than any wall, so a ramp holds you up and a
        // wall does not.
    }
    }   // substep

    memcpy(pPos3, pos, sizeof(float) * 3);

    // ---- standing is a PROBE, not a side effect of being pushed --------- //
    //
    // ⚠️ Deriving "grounded" from whatever the push-out happened to touch is
    // what made the body oscillate. The resolver deliberately pushes a hair
    // PAST touching, so a resting sphere ends each frame just out of contact,
    // reports itself airborne, gets gravity, sinks, is pushed out again --
    // a two-frame cycle that never settles. The walk test measures twenty
    // consecutive settled frames and showed exactly ten of them moving.
    //
    // So ground is asked as its own question afterwards, with a slightly
    // larger radius, and the skin is what makes resting stable.
    if (pbGround)
    {
        const float fSkin = 0.75f;
        float mn[3], mx[3];
        for (int k = 0; k < 3; ++k)
        { mn[k] = pos[k] - fRadius - fSkin; mx[k] = pos[k] + fRadius + fSkin; }

        Gather(mn, mx, cand);
        for (size_t i = 0; i < cand.size(); ++i)
        {
            float n[3], depth;
            if (TriOff(cand[i]) || !Contact(pos, fRadius + fSkin, cand[i], n, &depth)) continue;
            if (n[1] > 0.7f) { *pbGround = true; break; }
        }

        // ⚠️ AND THE MOVING WORLD MODELS -- doors, lifts, the M02S01 elevator
        // cab. The push-out above has always resolved against them, so a body
        // stood on a cab floor and rode it; but this probe asked only the
        // static grid, so it stood there "in the air" for the whole ride.
        // CMoveMgr keeps its last on-ground height while airborne, so the
        // first step back onto real floor counted the ride as ONE fall: 448
        // units down, 371 points of DT_CRUSH, dead on stepping out (user
        // report, HANDOVER 5bl). The donor's mover stands an object on
        // whatever it rests on, world model or not (moveobject.cpp,
        // m_pStandingOn). LT_STATICGROUND=1 = the old probe (says so).
        static const bool s_bStaticOnly = []() {
            const bool b = (getenv("LT_STATICGROUND") != 0);
            if (b) { printf("[PHYS] LT_STATICGROUND=1 -- moving world models are never ground\n"); fflush(stdout); }
            return b; }();
        if (!*pbGround && !s_bStaticOnly && !m_Dyn.empty())
        {
            const float fR = fRadius + fSkin;
            const int nTris = (int)(m_Dyn.size() / 9);
            for (int i = 0; i < nTris && !*pbGround; ++i)
            {
                const float* t = &m_Dyn[(size_t)i * 9];
                float closest[3];
                ClosestOnTri(pos, t, t + 3, t + 6, closest);
                float d[3]; Sub(pos, closest, d);
                const float len2 = Dot(d, d);
                if (len2 >= fR * fR || len2 < 1e-8f) continue;
                // The contact normal, as Contact() derives it: from the
                // nearest point on the triangle to the sphere's centre.
                if (d[1] / sqrtf(len2) > 0.7f) *pbGround = true;
            }
        }
    }
}


// ----------------------------------------------------------------------- //
// ⚠️ SEGMENT VS TRIANGLE SOUP. Möller-Trumbore per triangle, nearest hit wins.
//
// ⚠️ IT WALKS THE WHOLE GRID, not a traversal along the ray. A level is ~9,000
// triangles and the game casts a handful of rays per frame (one for activate,
// one per shot), so the honest simple version is affordable and cannot be
// subtly wrong about which cells a ray crosses. If a future weapon sprays rays
// this becomes the thing to optimise -- measure before assuming it is.
//
// ⚠️ Dynamic triangles (doors) are tested too: shooting through a closed door
// because the collision only knew about static geometry would be a bug the
// player can see.
// ----------------------------------------------------------------------- //

namespace
{
    // Returns true and fills t/normal when the segment crosses the triangle.
    // bFrontOnly: count the triangle only when the ray ENTERS through its
    // front (stored-winding) face -- the donor's rule; see RaySegment.
    bool SegTri(const float* o, const float* d, float fLen,
                const float* v0, const float* v1, const float* v2,
                float* pT, float* pN, bool bFrontOnly = false)
    {
        const float kEps = 1e-6f;

        float e1[3], e2[3];
        for (int k = 0; k < 3; ++k) { e1[k] = v1[k]-v0[k]; e2[k] = v2[k]-v0[k]; }

        float pv[3] = { d[1]*e2[2]-d[2]*e2[1],
                        d[2]*e2[0]-d[0]*e2[2],
                        d[0]*e2[1]-d[1]*e2[0] };
        const float det = e1[0]*pv[0] + e1[1]*pv[1] + e1[2]*pv[2];
        if (det > -kEps && det < kEps) return false;      // parallel
        if (bFrontOnly && det < 0.0f) return false;       // leaving through the back

        const float inv = 1.0f / det;
        float tv[3] = { o[0]-v0[0], o[1]-v0[1], o[2]-v0[2] };
        const float u = (tv[0]*pv[0] + tv[1]*pv[1] + tv[2]*pv[2]) * inv;
        if (u < 0.0f || u > 1.0f) return false;

        float qv[3] = { tv[1]*e1[2]-tv[2]*e1[1],
                        tv[2]*e1[0]-tv[0]*e1[2],
                        tv[0]*e1[1]-tv[1]*e1[0] };
        const float v = (d[0]*qv[0] + d[1]*qv[1] + d[2]*qv[2]) * inv;
        if (v < 0.0f || u + v > 1.0f) return false;

        const float t = (e2[0]*qv[0] + e2[1]*qv[1] + e2[2]*qv[2]) * inv;
        if (t < 0.0f || t > fLen) return false;

        *pT = t;

        // Geometric normal; the winding is the world's, so it may face either
        // way. The caller wants a surface normal, so flip it toward the ray.
        float n[3] = { e1[1]*e2[2]-e1[2]*e2[1],
                       e1[2]*e2[0]-e1[0]*e2[2],
                       e1[0]*e2[1]-e1[1]*e2[0] };
        const float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (len > 0.0f) { n[0]/=len; n[1]/=len; n[2]/=len; }
        if (n[0]*d[0] + n[1]*d[1] + n[2]*d[2] > 0.0f)
        { n[0]=-n[0]; n[1]=-n[1]; n[2]=-n[2]; }
        for (int k = 0; k < 3; ++k) pN[k] = n[k];
        return true;
    }
}

bool CLTWorldCollide::RaySegment(const float* pFrom3, const float* pTo3,
                                 float* pPoint3, float* pNormal3, float* pT) const
{
    return RaySegment(pFrom3, pTo3, pPoint3, pNormal3, pT, 0);
}

bool CLTWorldCollide::RaySegment(const float* pFrom3, const float* pTo3,
                                 float* pPoint3, float* pNormal3, float* pT,
                                 unsigned int* pPolyId) const
{
    unsigned int nBestPoly = 0;
    if (!pFrom3 || !pTo3) return false;

    float d[3] = { pTo3[0]-pFrom3[0], pTo3[1]-pFrom3[1], pTo3[2]-pFrom3[2] };
    const float fLen = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (fLen <= 0.0f) return false;
    for (int k = 0; k < 3; ++k) d[k] /= fLen;

    float fBest = fLen;
    float nBest[3] = { 0.0f, 1.0f, 0.0f };
    bool  bHit = false;

    const size_t nTris = m_Tris.size() / 9;
    for (size_t i = 0; i < nTris; ++i)
    {
        if (TriOff(i)) continue;
        const float* v = &m_Tris[i * 9];
        float t, n[3];
        if (SegTri(pFrom3, d, fBest, v, v+3, v+6, &t, n) && t < fBest)
        { fBest = t; for (int k=0;k<3;++k) nBest[k]=n[k]; bHit = true;
          nBestPoly = (i < m_TriPoly.size()) ? m_TriPoly[i] : 0; }
    }

    // ⚠️ DOOR TRIANGLES ARE ONE-SIDED, as the brush ray test is (server/
    // ltserverbrush.cpp LTServerBrush_RayTest). These are the same solid
    // brushes, so a ray leaving one must not stop on its far face here
    // either: M01S01 `blocker1` (an invisible-surface Door) -- the game
    // restarts the bullet 1 unit inside it and the far face, reported as
    // "the world", ended the shot (HANDOVER 5bh). Static world triangles keep
    // the two-sided rule. LT_TWOSIDEDBRUSH=1 = the old rule.
    static const bool s_bTwoSided = []() {
        const bool b = (getenv("LT_TWOSIDEDBRUSH") != 0);
        if (b) { printf("[COLLIDE] LT_TWOSIDEDBRUSH=1 -- door triangles hit back faces too\n"); fflush(stdout); }
        return b; }();
    const float fStatic = fBest;        // for LT_RAYDIFF below
    const bool  bStatic = bHit;
    const std::vector<float>& dyn = m_bDynRay ? m_DynRay : m_Dyn;     // DEVIATIONS E-008
    const size_t nDyn = dyn.size() / 9;
    for (size_t i = 0; i < nDyn; ++i)
    {
        const float* v = &dyn[i * 9];
        float t, n[3];
        if (SegTri(pFrom3, d, fBest, v, v+3, v+6, &t, n, !s_bTwoSided) && t < fBest)
        { fBest = t; for (int k=0;k<3;++k) nBest[k]=n[k]; bHit = true; nBestPoly = 0; }
    }

    // LT_RAYDIFF=1 -- the whole answer again with the door triangles under
    // the other rule; disagreements are listed (see ltserverbrush.cpp).
    static const bool s_bDiff = (getenv("LT_RAYDIFF") != 0);
    if (s_bDiff && nDyn)
    {
        static unsigned s_nAsked = 0, s_nDiff = 0;
        ++s_nAsked;
        float fOther = fStatic; bool bOther = bStatic;
        for (size_t i = 0; i < nDyn; ++i)
        {
            const float* v = &dyn[i * 9];
            float t, n[3];
            if (SegTri(pFrom3, d, fOther, v, v+3, v+6, &t, n, s_bTwoSided) && t < fOther)
            { fOther = t; bOther = true; }
        }
        if (bOther != bHit || (bHit && fabsf(fOther - fBest) > 0.01f))
        {
            ++s_nDiff;
            if (s_nDiff <= 40 || (s_nDiff % 500) == 0)
            {
                char a[32], b[32];
                if (bHit)   snprintf(a, sizeof(a), "HIT at %.1f", fBest);  else snprintf(a, sizeof(a), "miss");
                if (bOther) snprintf(b, sizeof(b), "HIT at %.1f", fOther); else snprintf(b, sizeof(b), "miss");
                printf("[RAYDIFF] world+doors #%u (of %u asked) %.0f-unit ray from (%.0f %.0f %.0f) dir (%.2f %.2f %.2f): %s %s, %s %s\n",
                       s_nDiff, s_nAsked, fLen, pFrom3[0], pFrom3[1], pFrom3[2], d[0], d[1], d[2],
                       s_bTwoSided ? "two-sided" : "one-sided", a, s_bTwoSided ? "one-sided" : "two-sided", b);
                fflush(stdout);
            }
        }
    }

    if (!bHit) return false;
    if (pPolyId) *pPolyId = nBestPoly;

    if (pPoint3) for (int k = 0; k < 3; ++k) pPoint3[k] = pFrom3[k] + d[k]*fBest;
    if (pNormal3) for (int k = 0; k < 3; ++k) pNormal3[k] = nBest[k];
    if (pT) *pT = fBest / fLen;
    return true;
}


// ----------------------------------------------------------------------- //
// The shared instance -- see the header for why there is exactly one.
// ----------------------------------------------------------------------- //

namespace
{
    CLTWorldCollide g_Shared;
    const CLTWorld* g_pBuiltFor = 0;
}

CLTWorldCollide* LTGetWorldCollide() { return &g_Shared; }

void LTWorldCollide_BuildFor(const CLTWorld* pWorld)
{
    if (!pWorld) return;
    if (g_pBuiltFor == pWorld && g_Shared.Triangles()) return;

    g_Shared.Build(*pWorld);
    g_pBuiltFor = pWorld;

    printf("[COLLIDE] %u triangle(s), %u cell(s)\n",
           g_Shared.Triangles(), g_Shared.Cells());
    fflush(stdout);
}

void LTWorldCollide_Reset()
{
    // ⚠️ Only the "which world" latch is cleared, not the triangles. The next
    // BuildFor replaces them wholesale; freeing here would leave a window in
    // which a query mid-level-change answers "nothing is solid".
    g_pBuiltFor = 0;
}

bool LTWorldCollide_Ready()
{
    return g_pBuiltFor != 0 && g_Shared.Triangles() > 0;
}
