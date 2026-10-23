// ----------------------------------------------------------------------- //
//
// ltworldvis.cpp -- the level's visibility. See ltworldvis.h.
//
// ----------------------------------------------------------------------- //

#include "ltworldvis.h"
#include "ltworld.h"

#include <math.h>
#include <stdio.h>
#include <strings.h>

CLTWorldVis::CLTWorldVis()
    : m_nRoot(-1), m_nPortalGen(1), m_nViewLeaf(-2), m_nViewGen(0), m_bAll(true)
{
}

void CLTWorldVis::Clear()
{
    m_Nodes.clear();
    m_Leaves.clear();
    m_Portals.clear();
    m_State.clear();
    m_nRoot = -1;
    ++m_nPortalGen;
    m_nViewLeaf = -2;
    m_bAll = true;
}


bool CLTWorldVis::Build(const CLTWorld& world)
{
    Clear();

    const LTWorldModelHeader* v = 0;
    for (const LTWorldModelHeader& m : world.GetModels())
        if (m.m_nLeafs && m.m_bBodyParsed && m.m_nRootNode >= 0 && !m.m_Nodes.empty())
        { v = &m; break; }
    if (!v) return false;

    // ---- nodes, each with its polygon's plane ---------------------------- //
    //
    // ⚠️ THE PLANE IS COMPUTED, NOT LOOKED UP. Neither polygon field that
    // could be a plane index makes the polygon lie in that plane (25 of 2,034
    // on M01S01), so the plane comes from the polygon's own points (Newell).
    // ⚠️ AND ITS SIGN WAS MEASURED: with the points' winding as stored, 671
    // of M01S01's 731 objects end in a leaf (a front IN side); flipped, none.
    m_Nodes.resize(v->m_Nodes.size());
    for (size_t i = 0; i < v->m_Nodes.size(); ++i)
    {
        const LTWorldNode& src = v->m_Nodes[i];
        Node& n = m_Nodes[i];
        n.m_Sides[0] = src.m_Sides[0];
        n.m_Sides[1] = src.m_Sides[1];
        n.m_nLeaf = (src.m_nLeaf == 0xFFFF || src.m_nLeaf >= v->m_nLeafs) ? -1 : (int)src.m_nLeaf;
        n.m_Normal[0] = n.m_Normal[1] = n.m_Normal[2] = 0.0f;
        n.m_Dist = 0.0f;
        for (int s = 0; s < 2; ++s)
            if (n.m_Sides[s] >= (int)v->m_Nodes.size() || n.m_Sides[s] < -2) { Clear(); return false; }
        if (src.m_nPoly >= v->m_Polys.size()) { Clear(); return false; }

        const LTWorldPoly& po = v->m_Polys[src.m_nPoly];
        const size_t nv = po.m_Verts.size();
        double nx = 0, ny = 0, nz = 0, cx = 0, cy = 0, cz = 0;
        for (size_t k = 0; k < nv; ++k)
        {
            const unsigned a = po.m_Verts[k].m_nPoint, b = po.m_Verts[(k + 1) % nv].m_nPoint;
            if (a >= v->m_Points.size() || b >= v->m_Points.size()) { Clear(); return false; }
            const float* A = v->m_Points[a].m_Pos;
            const float* B = v->m_Points[b].m_Pos;
            nx += (double)(A[1] - B[1]) * (A[2] + B[2]);
            ny += (double)(A[2] - B[2]) * (A[0] + B[0]);
            nz += (double)(A[0] - B[0]) * (A[1] + B[1]);
            cx += A[0]; cy += A[1]; cz += A[2];
        }
        const double L = sqrt(nx*nx + ny*ny + nz*nz);
        if (L <= 0.0 || nv == 0) { Clear(); return false; }
        n.m_Normal[0] = (float)(nx / L);
        n.m_Normal[1] = (float)(ny / L);
        n.m_Normal[2] = (float)(nz / L);
        n.m_Dist = (float)((nx * cx + ny * cy + nz * cz) / (L * (double)nv));
    }
    m_nRoot = v->m_nRootNode;

    // ---- leaves: their lists, a shared leaf pointing at its owner's ------ //
    m_Leaves.resize(v->m_Leaves.size());
    for (size_t i = 0; i < v->m_Leaves.size(); ++i)
    {
        const LTWorldLeaf* pL = &v->m_Leaves[i];
        if (pL->m_nShare >= 0) pL = &v->m_Leaves[(size_t)pL->m_nShare];
        for (size_t k = 0; k < pL->m_Lists.size(); ++k)
        {
            List li;
            li.m_nPortal = (k == 0) ? -1 : (int)pL->m_Lists[k].m_nPortal;
            li.m_Bits = pL->m_Lists[k].m_Data;
            m_Leaves[i].push_back(li);
        }
    }

    // ---- named portals: closed unless the level says otherwise ----------- //
    //
    // The 30 bytes after a portal's name start with a uint32 that is 0 for
    // every portal in the shipped levels -- PORTAL_OPEN clear. Doors open
    // them (Door.cpp:1352).
    for (size_t i = 0; i < v->m_PortalNames.size(); ++i)
    {
        Portal p;
        p.m_sName = v->m_PortalNames[i];
        p.m_nFlags = 0;
        if (i < v->m_PortalData.size() && v->m_PortalData[i].size() >= 4)
        {
            const std::vector<unsigned char>& d = v->m_PortalData[i];
            p.m_nFlags = (unsigned)d[0] | ((unsigned)d[1] << 8) | ((unsigned)d[2] << 16) | ((unsigned)d[3] << 24);
        }
        m_Portals.push_back(p);
    }
    return true;
}


int CLTWorldVis::LeafAt(const float* p) const
{
    if (!IsValid() || !p) return -1;
    int node = m_nRoot;
    for (size_t guard = 0; guard <= m_Nodes.size(); ++guard)
    {
        const Node& n = m_Nodes[(size_t)node];
        const float d = n.m_Normal[0]*p[0] + n.m_Normal[1]*p[1] + n.m_Normal[2]*p[2] - n.m_Dist;
        const int side = (d > 0.0f) ? 1 : 0;
        const int c = n.m_Sides[side];
        if (c >= 0) { node = c; continue; }
        // A front IN is the inside of this node's leaf; anything else is out.
        return (c == -1 && side == 1) ? n.m_nLeaf : -1;
    }
    return -1;
}


// The walk itself -- lithtech.exe 0x48b8e0 and its recursive half 0x48be40.
// Iterative here: a node whose sides[0] subtree is being walked keeps its
// sides[1] bit pending on a stack, exactly the order the recursion reads in.
void CLTWorldVis::Walk(const std::vector<unsigned char>& bits, std::vector<unsigned char>& state,
                       bool* pRanPast, size_t* pBitsUsed) const
{
    size_t k = 0;
    bool bPast = false;
    const size_t nBits = bits.size() * 8;
    auto bit = [&]() -> int {
        if (k >= nBits) { bPast = true; ++k; return 0; }
        const int b = (bits[k >> 3] >> (k & 7)) & 1;
        ++k;
        return b;
    };

    std::vector<int> pendingB;
    int node = m_nRoot;
    bool bVisit = true;
    int B = -2;
    for (;;)
    {
        if (bVisit)
        {
            state[(size_t)node] |= kVisited;
            const Node& n = m_Nodes[(size_t)node];
            if (n.m_nLeaf >= 0 && bit()) state[(size_t)node] |= kLeafSeen;
            const int A = n.m_Sides[0];
            B = n.m_Sides[1];
            if (A >= 0 && bit())
            {
                if (B >= 0) pendingB.push_back(B);
                node = A;
                continue;
            }
        }
        if (B >= 0 && bit()) { node = B; bVisit = true; continue; }
        if (pendingB.empty()) break;
        B = pendingB.back();
        pendingB.pop_back();
        bVisit = false;
    }
    if (pRanPast) *pRanPast = bPast;
    if (pBitsUsed) *pBitsUsed = k;
}


void CLTWorldVis::SetViewLeaf(int nLeaf)
{
    if (nLeaf == m_nViewLeaf && m_nViewGen == m_nPortalGen) return;
    m_nViewLeaf = nLeaf;
    m_nViewGen = m_nPortalGen;

    m_bAll = !IsValid() || nLeaf < 0 || (size_t)nLeaf >= m_Leaves.size() || m_Leaves[(size_t)nLeaf].empty();
    if (m_bAll) return;

    m_State.assign(m_Nodes.size(), 0);
    const std::vector<List>& lists = m_Leaves[(size_t)nLeaf];
    for (size_t i = 0; i < lists.size(); ++i)
    {
        if (i > 0)
        {
            const int p = lists[i].m_nPortal;
            if (p < 0 || (size_t)p >= m_Portals.size() || !(m_Portals[(size_t)p].m_nFlags & 1)) continue;
        }
        Walk(lists[i].m_Bits, m_State, 0, 0);
    }
}


bool CLTWorldVis::BoxVisible(const float* mn, const float* mx) const
{
    if (m_bAll) return true;

    float c[3], e[3];
    for (int k = 0; k < 3; ++k) { c[k] = 0.5f * (mn[k] + mx[k]); e[k] = 0.5f * (mx[k] - mn[k]); if (e[k] < 0) e[k] = -e[k]; }

    // The object travels the walk's path, split by each plane, and is seen at
    // the first node it reaches whose leaf bit was set.
    int stack[512];
    int nTop = 0;
    stack[nTop++] = m_nRoot;
    while (nTop > 0)
    {
        const int node = stack[--nTop];
        const unsigned char s = m_State[(size_t)node];
        if (!(s & kVisited)) continue;
        if (s & kLeafSeen) return true;

        const Node& n = m_Nodes[(size_t)node];
        const float d = n.m_Normal[0]*c[0] + n.m_Normal[1]*c[1] + n.m_Normal[2]*c[2] - n.m_Dist;
        const float r = fabsf(n.m_Normal[0])*e[0] + fabsf(n.m_Normal[1])*e[1] + fabsf(n.m_Normal[2])*e[2];
        // A tree too deep for the stack counts as seen -- the safe answer.
        if (nTop > 510) return true;
        if (d + r > 0.0f && n.m_Sides[1] >= 0) stack[nTop++] = n.m_Sides[1];
        if (d - r <= 0.0f && n.m_Sides[0] >= 0) stack[nTop++] = n.m_Sides[0];
    }
    return false;
}


unsigned int CLTWorldVis::VisibleLeafCount() const
{
    if (m_bAll) return (unsigned int)m_Leaves.size();
    unsigned int n = 0;
    for (size_t i = 0; i < m_State.size(); ++i)
        if (m_State[i] & kLeafSeen) ++n;
    return n;
}


void CLTWorldVis::SelfCheck(unsigned int* pnLists, unsigned int* pnBad, unsigned int* pnSelf) const
{
    unsigned int nLists = 0, nBad = 0, nSelf = 0;
    std::vector<int> nodeOf(m_Leaves.size(), -1);
    for (size_t i = 0; i < m_Nodes.size(); ++i)
        if (m_Nodes[i].m_nLeaf >= 0) nodeOf[(size_t)m_Nodes[i].m_nLeaf] = (int)i;

    for (size_t i = 0; i < m_Leaves.size(); ++i)
    {
        for (size_t k = 0; k < m_Leaves[i].size(); ++k)
        {
            std::vector<unsigned char> st(m_Nodes.size(), 0);
            bool bPast = false;
            size_t nUsed = 0;
            Walk(m_Leaves[i][k].m_Bits, st, &bPast, &nUsed);
            ++nLists;
            // A list must end INSIDE its last byte: not past it, and not with
            // a whole byte to spare.
            if (bPast || nUsed + 8 <= m_Leaves[i][k].m_Bits.size() * 8) ++nBad;
            if (k == 0 && nodeOf[i] >= 0 && (st[(size_t)nodeOf[i]] & kLeafSeen)) ++nSelf;
        }
    }
    if (pnLists) *pnLists = nLists;
    if (pnBad) *pnBad = nBad;
    if (pnSelf) *pnSelf = nSelf;
}


bool CLTWorldVis::GetPortalFlags(const char* pName, unsigned int* pFlags) const
{
    if (!pName) return false;
    for (size_t i = 0; i < m_Portals.size(); ++i)
        if (!strcasecmp(m_Portals[i].m_sName.c_str(), pName))
        {
            if (pFlags) *pFlags = m_Portals[i].m_nFlags;
            return true;
        }
    return false;
}

bool CLTWorldVis::SetPortalFlags(const char* pName, unsigned int nFlags)
{
    if (!pName) return false;
    for (size_t i = 0; i < m_Portals.size(); ++i)
        if (!strcasecmp(m_Portals[i].m_sName.c_str(), pName))
        {
            if (m_Portals[i].m_nFlags != nFlags) { m_Portals[i].m_nFlags = nFlags; ++m_nPortalGen; }
            return true;
        }
    return false;
}


// ----------------------------------------------------------------------- //
// The shared instance.
// ----------------------------------------------------------------------- //

namespace
{
    CLTWorldVis g_Vis;
}

CLTWorldVis* LTGetWorldVis() { return &g_Vis; }

// ⚠️ NO "ALREADY BUILT" LATCH. The scene reuses one CLTWorld for every level,
// so a pointer latch would keep the first level's visibility forever; and a
// rebuild resets the portals, so the server calls this ONCE per level, before
// any object exists to open a door.
void LTWorldVis_BuildFor(const CLTWorld* pWorld)
{
    if (!pWorld) { g_Vis.Clear(); return; }

    if (!g_Vis.Build(*pWorld))
    {
        printf("[VIS] no visibility in this world -- everything counts as seen\n");
        fflush(stdout);
        return;
    }
    unsigned int nLists = 0, nBad = 0, nSelf = 0;
    g_Vis.SelfCheck(&nLists, &nBad, &nSelf);
    printf("[VIS] %u leaves, %u nodes, %u portal(s); %u list(s), %u misread, %u of %u leaves see themselves\n",
           g_Vis.NumLeaves(), g_Vis.NumNodes(), g_Vis.NumPortals(), nLists, nBad, nSelf, g_Vis.NumLeaves());
    fflush(stdout);
}
