// ----------------------------------------------------------------------- //
//
// MODULE  : ltserverbrush.cpp
//
// See ltserverbrush.h.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltserverbrush.h"
#include "ltserverhost.h"
#include "ltserverobjects.h"
#include "ltworld.h"
#include "ltserverobj.h"
#include "ltworldrender.h"
#include "ltworldcollide.h"
#include "lt3d.h"
#include "ltquatbase.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <ctype.h>

typedef CLTServerObjects::Record SRec;

namespace
{
    struct MovableBrush
    {
        std::string         m_sName;
        SRec*               m_pRec;
        float               m_RestPos[3];
        float               m_RestQuat[4];
        std::vector<float>  m_Tris;         // rest triangles, 9 floats each
        std::vector<unsigned int> m_TriPoly;    // LTWorldPolyId per triangle (5dq)
    };

    std::vector<MovableBrush>  g_Brushes;

    // ⚠️ THE SOLID STATIC BRUSH OBJECTS (5ds): in the static grid, and gone
    // from it when their object is removed or stops being solid -- see
    // CLTWorldCollide::SetModelOff. World-model index, record, and whether its
    // triangles are on now.
    struct StaticBrush { unsigned int m_nModel; SRec* m_pRec; bool m_bOn; };
    std::vector<StaticBrush>   g_Static;

    // ⚠️ WHERE EACH BRUSH WAS LAST FRAME, so what stands on one can ride it.
    // See LTServerBrush_CarryPoint. Index-parallel to g_Brushes; a brush that
    // moved between two updates is listed in g_MovedNow with both placements.
    std::vector<std::vector<float> > g_PrevM;      // 16 floats, or empty = unknown
    struct MovedBrush { size_t m_nBrush; float m_Prev[16]; float m_Now[16]; };
    std::vector<MovedBrush>    g_MovedNow;

    // ⚠️ EVERY BRUSH OBJECT'S GEOMETRY, FOR RAYS -- see LTServerBrush_RayTest.
    // Rest-pose world-space triangles (9 floats each); a movable one is placed
    // through the same Matrix() the collision uses.
    struct HitBrush
    {
        const SRec*         m_pRec;
        int                 m_nMovable;     // index into g_Brushes, or -1
        std::vector<float>  m_Tris;
        std::vector<unsigned int> m_TriPoly;    // LTWorldPolyId per triangle
    };
    std::vector<HitBrush>              g_HitBrushes;
    std::map<const void*, size_t>      g_HitIndex;

    // ⚠️ BRUSH OBJECTS AND THEIR FLAGS -- see ILTWorldModelXform::IsModelHidden.
    // Every world model that belonged to a server object at load, by UPPER-
    // CASE name -> the world model's own name. Each update, the ones whose
    // object is gone or lacks FLAG_VISIBLE are hidden.
    std::map<std::string, std::string> g_BrushObjects;
    std::set<std::string>              g_Hidden;       // world-model names
    std::vector<void*>                 g_Live;         // scratch

    // ⚠️ AND THEIR ALPHA (HANDOVER 5cf) -- world-model name -> the object's
    // alpha, for the visible brush objects drawn translucent by it. See
    // ILTWorldModelXform::GetModelAlpha and BrushAlphaOf. Usually empty.
    std::map<std::string, float>       g_Alpha;

    // LT_NOBRUSHALPHA=1 -- brushes ignore their object's alpha, as before.
    bool NoBrushAlpha()
    {
        static const bool b = []() {
            const bool v = (getenv("LT_NOBRUSHALPHA") != 0);
            if (v) { printf("[BRUSH] LT_NOBRUSHALPHA=1 -- brushes drawn regardless of their object's alpha (old)\n"); fflush(stdout); }
            return v; }();
        return b;
    }

    // ⚠️ WHICH ALPHAS CHANGE HOW A BRUSH IS DRAWN. Below 0.95 only. The shipped
    // levels author 0.999 on 258 TranslucentWorldModels and 0.99 on 48 more --
    // a designer's way of asking for LT2's translucent pass, for glass whose
    // texture already carries the coverage. This port already blends every
    // SURF_TRANSPARENT surface by its texture, so those are drawn exactly as
    // before; moving them would only lose their depth writes. Below that the
    // census is 0.9 (12), 0.8, 0.75 (4), 0.7 (5), 0.5 (6), 0.25 (7), 0.05 and
    // 0 (21) -- 57 objects in 40 worlds whose alpha is the point.
    bool BrushAlphaOf(const SRec* p, float* pAlpha)
    {
        const float a = p->m_Color[3] * (1.0f / 255.0f);
        if (a >= 0.95f) return false;
        *pAlpha = a;
        return true;
    }

    // LT_NOBRUSHFLAGS=1 -- draw and collide every brush as before.
    bool NoBrushFlags()
    {
        static const bool b = (getenv("LT_NOBRUSHFLAGS") != 0);
        static const bool bSaid = (b ? (printf("[BRUSH] LT_NOBRUSHFLAGS=1 -- brush objects drawn "
                                               "and solid regardless of FLAG_VISIBLE/FLAG_SOLID\n"),
                                        fflush(stdout), true) : true);
        (void)bSaid;
        return b;
    }

    std::string Upper(std::string s)
    {
        for (size_t i = 0; i < s.size(); ++i) s[i] = (char)toupper((unsigned char)s[i]);
        return s;
    }
    std::vector<float>  g_Dyn;              // scratch, reused every update
    bool                g_bDynLive = false; // is anything currently handed over?
    unsigned int        g_nMoved   = 0;

    // ⚠️ WHICH BRUSH OBJECTS CAN MOVE. Only these leave the static collision
    // grid: everything else is level geometry and must keep colliding exactly
    // as before. T01S02 has 41 Door and 7 RotatingWorldModel among its 105
    // brush objects; the other 57 (TranslucentWorldModel, AIVolume) do not
    // move and are left alone.
    bool Movable(const SRec* p)
    {
        for (const ClassDef* c = p->m_pClass; c; c = c->m_ParentClass)
        {
            if (!c->m_ClassName) continue;
            if (strcasecmp(c->m_ClassName, "Door") == 0)                return true;
            if (strcasecmp(c->m_ClassName, "RotatingWorldModel") == 0)  return true;
        }
        return false;
    }

    // ⚠️ OT_CONTAINER TOO -- every volume brush (Filter, Water, Ladder ...)
    // is a container, and its geometry is a world model like any door's.
    SRec* FindRecord(const std::string& sName)
    {
        const unsigned int n = LTServerHost_ObjectCount();
        for (unsigned int i = 0; i < n; ++i)
        {
            SRec* p = CLTServerObjects::FromHandle(LTServerHost_ObjectAt(i));
            if (!p || !p->m_pClass) continue;
            if (p->m_Type != OT_WORLDMODEL && p->m_Type != OT_CONTAINER) continue;
            if (strcasecmp(p->m_sName.c_str(), sName.c_str()) != 0) continue;
            return p;
        }
        return 0;
    }

    // The brush's current placement: rotate about where the level put it, then
    // translate by however far the game has moved it.
    //
    // ⚠️ ROTATION IS TAKEN RELATIVE TO THE REST ROTATION, not absolute. A door
    // that the level authored at 90 degrees is not a door that has swung 90
    // degrees, and treating it so would fling every door in the level on load.
    bool Matrix(const MovableBrush& b, float* pOut16)
    {
        const SRec* p = b.m_pRec;

        float dq[4];
        {
            const float* a = p->m_Rot;
            const float  c[4] = { -b.m_RestQuat[0], -b.m_RestQuat[1],
                                  -b.m_RestQuat[2],  b.m_RestQuat[3] };
            dq[3] = a[3]*c[3] - a[0]*c[0] - a[1]*c[1] - a[2]*c[2];
            dq[0] = a[3]*c[0] + a[0]*c[3] + a[1]*c[2] - a[2]*c[1];
            dq[1] = a[3]*c[1] - a[0]*c[2] + a[1]*c[3] + a[2]*c[0];
            dq[2] = a[3]*c[2] + a[0]*c[1] - a[1]*c[0] + a[2]*c[3];
        }

        const float fMove = fabsf(p->m_Pos[0]-b.m_RestPos[0])
                          + fabsf(p->m_Pos[1]-b.m_RestPos[1])
                          + fabsf(p->m_Pos[2]-b.m_RestPos[2]);
        const float fTurn = fabsf(fabsf(dq[3]) - 1.0f);

        // Unmoved: no transform, and the caller can skip the work entirely.
        if (fMove < 0.01f && fTurn < 1e-5f) return false;

        float R[16];
        const float z3[3] = { 0.0f, 0.0f, 0.0f };
        LT3D::FromQuatPos(dq, z3, R);

        // M = T(pos) * R * T(-restPos)
        LT3D::Identity(pOut16);
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c) pOut16[r*4+c] = R[r*4+c];
            pOut16[r*4+3] = p->m_Pos[r]
                          - (R[r*4+0]*b.m_RestPos[0] + R[r*4+1]*b.m_RestPos[1]
                             + R[r*4+2]*b.m_RestPos[2]);
        }
        return true;
    }

    class ServerXform : public ILTWorldModelXform
    {
    public:
        bool IsModelHidden(const std::string& sName) const override
        {
            return g_Hidden.count(sName) != 0;
        }

        bool GetModelAlpha(const std::string& sName, float* pAlpha) const override
        {
            if (g_Alpha.empty()) return false;
            std::map<std::string, float>::const_iterator it = g_Alpha.find(sName);
            if (it == g_Alpha.end()) return false;
            if (pAlpha) *pAlpha = it->second;
            return true;
        }

        bool GetModelTransform(const std::string& sName, float* pOut16) const override
        {
            for (size_t i = 0; i < g_Brushes.size(); ++i)
            {
                if (g_Brushes[i].m_sName != sName) continue;
                return Matrix(g_Brushes[i], pOut16);
            }
            return false;
        }
    };
    ServerXform g_Xform;
}


void LTServerBrush_Reset()
{
    g_Brushes.clear();
    g_Static.clear();
    g_HitBrushes.clear();
    g_HitIndex.clear();
    g_BrushObjects.clear();
    g_Hidden.clear();
    g_Alpha.clear();
    g_Dyn.clear();
    g_bDynLive = false;
    g_nMoved = 0;
    g_PrevM.clear();
    g_MovedNow.clear();
}


void LTServerBrush_Build(const CLTWorld& world)
{
    LTServerBrush_Reset();

    // LT_NOBRUSH=1 leaves every brush in the static grid, as before doors
    // moved. An A/B instrument: "did taking doors out of the world break
    // standing on the world?" is one run, not an argument.
    if (getenv("LT_NOBRUSH")) { printf("[BRUSH] disabled (LT_NOBRUSH)\n"); return; }

    const std::vector<LTWorldModelHeader>& mdl = world.GetModels();
    std::vector<std::string> excluded;
    unsigned int nNonSolid = 0;

    // ⚠️ WHAT A DOOR CARRIES MOVES TOO. A brush named in some object's
    // `Attachments` property (Door.cpp:1143, attached in the door's first
    // update -- after this list is made, hence read from the level data) is
    // carried by its parent every frame (LTHost_UpdateFreeAttachments), so it
    // has to leave the static grid like the door itself. T02S01's display
    // case: `slidingglass1` lifts `sg1partition`, a TranslucentWorldModel that
    // stayed behind as a solid pane in front of the glasses (HANDOVER 5bl).
    std::set<std::string> attached;
    {
        const std::vector<LTWorldObject>& objs = world.GetObjects();
        for (size_t o = 0; o < objs.size(); ++o)
        {
            const LTWorldProperty* pa = objs[o].Find("Attachments");
            if (!pa || pa->m_nType != 0 || pa->m_sString.empty()) continue;
            std::string cur;
            const std::string& v = pa->m_sString;
            for (size_t k = 0; k <= v.size(); ++k)
            {
                const char c = (k < v.size()) ? v[k] : ';';
                if (c == ';' || c == ',' || c == ' ' || c == '\t')
                { if (!cur.empty()) attached.insert(Upper(cur)); cur.clear(); }
                else cur += c;
            }
        }
    }

    for (size_t i = 0; i < mdl.size(); ++i)
    {
        const LTWorldModelHeader& W = mdl[i];
        if (!W.m_bBodyParsed || W.m_sName.empty()) continue;

        SRec* p = FindRecord(W.m_sName);
        if (!p) continue;
        g_BrushObjects[Upper(W.m_sName)] = W.m_sName;

        // Its geometry for rays: every polygon but sky portals.
        {
            HitBrush hb;
            hb.m_pRec = p;
            hb.m_nMovable = -1;
            for (size_t q = 0; q < W.m_Polys.size(); ++q)
            {
                const LTWorldPoly& po = W.m_Polys[q];
                if (po.m_Verts.size() < 3 || po.m_nSurface >= W.m_Surfaces.size()) continue;
                if (W.m_Surfaces[po.m_nSurface].m_nFlags & (1u << 4)) continue;     // SURF_SKY
                for (size_t k = 1; k + 1 < po.m_Verts.size(); ++k)
                {
                    const unsigned short idx[3] = { po.m_Verts[0].m_nPoint,
                                                    po.m_Verts[k].m_nPoint,
                                                    po.m_Verts[k+1].m_nPoint };
                    if (idx[0] >= W.m_Points.size() || idx[1] >= W.m_Points.size()
                        || idx[2] >= W.m_Points.size()) continue;
                    hb.m_TriPoly.push_back(LTWorldPolyId((unsigned)i, (unsigned)q));
                    for (int v = 0; v < 3; ++v)
                        for (int c = 0; c < 3; ++c)
                            hb.m_Tris.push_back(W.m_Points[idx[v]].m_Pos[c]);
                }
            }
            if (!hb.m_Tris.empty())
            {
                g_HitIndex[p] = g_HitBrushes.size();
                g_HitBrushes.push_back(hb);
            }
        }

        // ⚠️ A BRUSH OBJECT WITHOUT FLAG_SOLID DOES NOT BLOCK. Volume brushes
        // carry SURF_SOLID on their faces (it is how the editor made them),
        // but the engine collides with an object only when the OBJECT is
        // solid -- you swim through Water and climb into a Ladder. Decided at
        // load: these classes never change it.
        if (getenv("LT_BRUSHTRACE"))
        {
            // ...and where its geometry is (5dr): the object's Pos is only its
            // build origin. From the ray record, which every brush object has.
            float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
            std::map<const void*, size_t>::const_iterator hi = g_HitIndex.find(p);
            if (hi != g_HitIndex.end())
            {
                const std::vector<float>& T = g_HitBrushes[hi->second].m_Tris;
                for (size_t t = 0; t + 2 < T.size(); t += 3)
                    for (int k = 0; k < 3; ++k)
                    { mn[k] = std::min(mn[k], T[t+k]); mx[k] = std::max(mx[k], T[t+k]); }
            }
            printf("[BRUSH] object %-28s %-22s flags 0x%08x%s%s box (%.0f %.0f %.0f)..(%.0f %.0f %.0f)\n", W.m_sName.c_str(),
                   p->m_pClass->m_ClassName, (unsigned)p->m_Flags,
                   (p->m_Flags & FLAG_VISIBLE) ? " VISIBLE" : "",
                   (p->m_Flags & FLAG_SOLID) ? " SOLID" : "",
                   mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
        }
        if (!Movable(p) && !attached.count(Upper(W.m_sName)))
        {
            if (!(p->m_Flags & FLAG_SOLID) && !NoBrushFlags())
            {
                excluded.push_back(W.m_sName);
                ++nNonSolid;
            }
            else
            {
                StaticBrush sb = { (unsigned int)i, p, true };
                g_Static.push_back(sb);
            }
            continue;
        }

        MovableBrush b;
        b.m_sName = W.m_sName;
        b.m_pRec  = p;
        // ⚠️ A RESTORED BRUSH IS NOT AT ITS REST POSE -- see
        // Record::m_bRestFromFile. Its geometry is where the world file built
        // it; the object is wherever the save left it.
        for (int k = 0; k < 3; ++k) b.m_RestPos[k]  = p->m_bRestFromFile ? p->m_BoxRestPos[k] : p->m_Pos[k];
        for (int k = 0; k < 4; ++k) b.m_RestQuat[k] = p->m_bRestFromFile ? p->m_RestRot[k]    : p->m_Rot[k];

        // ⚠️ SOLID surfaces only, and triangulated the same way the static
        // grid and walk mode do it -- three copies of this loop would be three
        // chances to disagree about what the player can stand on.
        for (size_t q = 0; q < W.m_Polys.size(); ++q)
        {
            const LTWorldPoly& po = W.m_Polys[q];
            if (po.m_Verts.size() < 3) continue;
            if (po.m_nSurface >= W.m_Surfaces.size()) continue;
            if (!(W.m_Surfaces[po.m_nSurface].m_nFlags & 1u)) continue;

            for (size_t k = 1; k + 1 < po.m_Verts.size(); ++k)
            {
                const unsigned short idx[3] = { po.m_Verts[0].m_nPoint,
                                                po.m_Verts[k].m_nPoint,
                                                po.m_Verts[k+1].m_nPoint };
                bool bOK = true;
                for (int v = 0; v < 3 && bOK; ++v)
                    if (idx[v] >= W.m_Points.size()) bOK = false;
                if (!bOK) continue;

                b.m_TriPoly.push_back(LTWorldPolyId((unsigned)i, (unsigned)q));
                for (int v = 0; v < 3; ++v)
                    for (int c = 0; c < 3; ++c)
                        b.m_Tris.push_back(W.m_Points[idx[v]].m_Pos[c]);
            }
        }

        if (b.m_Tris.empty()) continue;
        // LT_BRUSHTRACE=1 -- where a movable brush's geometry really is: its
        // object Pos is only the origin it was built around (5dr).
        if (getenv("LT_BRUSHTRACE"))
        {
            float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
            for (size_t t = 0; t + 2 < b.m_Tris.size(); t += 3)
                for (int k = 0; k < 3; ++k)
                { mn[k] = std::min(mn[k], b.m_Tris[t+k]); mx[k] = std::max(mx[k], b.m_Tris[t+k]); }
            printf("[BRUSH] movable %-24s box (%.0f %.0f %.0f)..(%.0f %.0f %.0f), %zu triangle(s)\n",
                   b.m_sName.c_str(), mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], b.m_Tris.size() / 9);
        }
        excluded.push_back(b.m_sName);
        {
            std::map<const void*, size_t>::iterator it = g_HitIndex.find(p);
            if (it != g_HitIndex.end()) g_HitBrushes[it->second].m_nMovable = (int)g_Brushes.size();
        }
        g_Brushes.push_back(b);
    }

    // ⚠️ BEFORE the grid is built. These brushes are handed over as DYNAMIC
    // triangles instead, so an open door stops blocking its own doorway.
    if (LTGetWorldCollide()) LTGetWorldCollide()->SetExcluded(excluded);

    unsigned int nTris = 0;
    for (size_t i = 0; i < g_Brushes.size(); ++i)
        nTris += (unsigned int)(g_Brushes[i].m_Tris.size() / 9);

    printf("[BRUSH] %u movable brush object(s), %u triangle(s) -- excluded from "
           "the static grid, placed every frame\n",
           (unsigned int)g_Brushes.size(), nTris);
    printf("[BRUSH] %u brush object(s); %u not solid -- excluded from collision\n",
           (unsigned int)g_BrushObjects.size(), nNonSolid);
    fflush(stdout);
    LTServerBrush_UpdateVisibility();
}


void LTServerBrush_UpdateVisibility()
{
    if (g_BrushObjects.empty()) { g_Hidden.clear(); g_Alpha.clear(); return; }

    std::set<std::string> visible;      // upper-case names of VISIBLE live objects
    std::map<std::string, float> alpha; // upper-case name -> alpha, translucent ones
    LTServerHost_LiveObjects(g_Live);
    for (size_t i = 0; i < g_Live.size(); ++i)
    {
        const SRec* p = CLTServerObjects::FromHandle(g_Live[i]);
        if (p->m_Type != OT_WORLDMODEL && p->m_Type != OT_CONTAINER) continue;
        if (!(p->m_Flags & FLAG_VISIBLE)) continue;
        visible.insert(Upper(p->m_sName));
        float a;
        if (!NoBrushAlpha() && BrushAlphaOf(p, &a)) alpha[Upper(p->m_sName)] = a;
    }

    // The alpha by WORLD-MODEL name, which is what the renderers ask with.
    // LT_BRUSHTRACE=1 prints each change.
    {
        static const bool bTrace = (getenv("LT_BRUSHTRACE") != 0);
        std::map<std::string, float> byModel;
        for (std::map<std::string, float>::const_iterator it = alpha.begin(); it != alpha.end(); ++it)
        {
            std::map<std::string, std::string>::const_iterator m = g_BrushObjects.find(it->first);
            if (m != g_BrushObjects.end()) byModel[m->second] = it->second;
        }
        if (bTrace && byModel != g_Alpha)
        {
            for (std::map<std::string, float>::const_iterator it = byModel.begin(); it != byModel.end(); ++it)
            {
                std::map<std::string, float>::const_iterator o = g_Alpha.find(it->first);
                if (o == g_Alpha.end() || o->second != it->second)
                    printf("[BRUSH] t=%.2f alpha %s %.3f\n", LTServerHost_Time(), it->first.c_str(), it->second);
            }
            for (std::map<std::string, float>::const_iterator o = g_Alpha.begin(); o != g_Alpha.end(); ++o)
                if (!byModel.count(o->first))
                    printf("[BRUSH] t=%.2f alpha %s opaque\n", LTServerHost_Time(), o->first.c_str());
            fflush(stdout);
        }
        g_Alpha.swap(byModel);
    }

    // ⚠️ LT_NOBRUSHFLAGS=1 still draws every brush (hidden set stays empty),
    // and it is tested HERE, after the alpha, so the two switches stay apart.
    if (NoBrushFlags()) { g_Hidden.clear(); return; }

    // Hidden = had an object at load, and no live visible object now (gone,
    // or invisible). LT_BRUSHTRACE=1 prints each change.
    static const bool bTrace = (getenv("LT_BRUSHTRACE") != 0);
    std::set<std::string> hidden;
    for (std::map<std::string, std::string>::const_iterator it = g_BrushObjects.begin();
         it != g_BrushObjects.end(); ++it)
        if (!visible.count(it->first)) hidden.insert(it->second);

    if (bTrace && hidden != g_Hidden)
    {
        unsigned nOn = 0, nOff = 0;
        for (std::set<std::string>::const_iterator h = hidden.begin(); h != hidden.end(); ++h)
            if (!g_Hidden.count(*h)) { if (nOff++ < 40) printf("[BRUSH] hide %s\n", h->c_str()); }
        for (std::set<std::string>::const_iterator h = g_Hidden.begin(); h != g_Hidden.end(); ++h)
            if (!hidden.count(*h)) { if (nOn++ < 40) printf("[BRUSH] show %s\n", h->c_str()); }
        printf("[BRUSH] %u hidden of %u brush objects (+%u hidden, +%u shown)\n",
               (unsigned)hidden.size(), (unsigned)g_BrushObjects.size(), nOff, nOn);
        fflush(stdout);
    }
    g_Hidden.swap(hidden);
}


bool LTServerBrush_RaysPassThrough(const char* pName)
{
    static const bool s_bOriginal = []() {
        const bool b = getenv("LT_WIRESCATCHSHOTS") != 0;
        if (b) { printf("[PHYS] LT_WIRESCATCHSHOTS=1 -- the power lines catch and deflect shots, as on Windows\n"); fflush(stdout); }
        return b; }();
    if (s_bOriginal || !pName) return false;
    return strncasecmp(pName, "powerline", 9) == 0;
}


void LTServerBrush_Update()
{
    LTServerBrush_UpdateVisibility();

    // ⚠️ A STATIC BRUSH OBJECT THAT IS GONE STOPS BLOCKING (5ds). The static
    // grid is built once; a destructible world model removed at run time --
    // M08S04's vent louvres, TranslucentWorldModel28-33, "Machinery" debris,
    // 5 hit points -- left its triangles behind, an invisible wall exactly
    // where the player had just shot the way open. Its model is switched off
    // in the grid when the object is no longer live and solid (and back on if
    // it returns). LT_STATICBRUSHSTAY=1 = the old way (says so).
    static const bool s_bStay = []() {
        const bool b = (getenv("LT_STATICBRUSHSTAY") != 0);
        if (b) { printf("[BRUSH] LT_STATICBRUSHSTAY=1 -- a removed static brush keeps its collision (old)\n"); fflush(stdout); }
        return b; }();
    if (!s_bStay && LTGetWorldCollide())
        for (size_t k = 0; k < g_Static.size(); ++k)
        {
            StaticBrush& sb = g_Static[k];
            const bool bOn = LTServerHost_IsLive((void*)sb.m_pRec) && (sb.m_pRec->m_Flags & FLAG_SOLID);
            if (bOn == sb.m_bOn) continue;
            sb.m_bOn = bOn;
            LTGetWorldCollide()->SetModelOff(sb.m_nModel, !bOn);
            if (getenv("LT_BRUSHTRACE"))
            {
                printf("[BRUSH] t=%.2f static model %u %s collision\n", LTServerHost_Time(), sb.m_nModel,
                       bOn ? "back IN" : "taken OUT of");
                fflush(stdout);
            }
        }

    if (g_Brushes.empty()) return;

    g_Dyn.clear();
    std::vector<void*> dynOwner;    // one per g_Dyn triangle: whose door (5dp)
    std::vector<unsigned int> dynPoly;  // ...and which of its polygons (5dq)
    std::vector<float> dynRay;      // the same, less what rays pass through (E-008)
    g_nMoved = 0;
    g_MovedNow.clear();
    if (g_PrevM.size() != g_Brushes.size()) g_PrevM.assign(g_Brushes.size(), std::vector<float>());

    for (size_t i = 0; i < g_Brushes.size(); ++i)
    {
        const MovableBrush& b = g_Brushes[i];

        float M[16];
        const bool bMoved = Matrix(b, M);
        if (bMoved) ++g_nMoved;
        if (!bMoved) LT3D::Identity(M);

        // Moved since the last update? (Placement, not "away from rest".)
        if (g_PrevM[i].size() == 16)
        {
            float fDiff = 0.0f;
            for (int k = 0; k < 16; ++k) fDiff += fabsf(M[k] - g_PrevM[i][k]);
            if (fDiff > 1e-4f)
            {
                MovedBrush mb;
                mb.m_nBrush = i;
                memcpy(mb.m_Prev, &g_PrevM[i][0], sizeof(mb.m_Prev));
                memcpy(mb.m_Now, M, sizeof(mb.m_Now));
                g_MovedNow.push_back(mb);
            }
        }
        g_PrevM[i].assign(M, M + 16);

        // ⚠️ EVERY SOLID brush is handed over, moved or not. They were taken
        // OUT of the static grid, so a solid door left out of this list is a
        // doorway with no door in it.
        // ⚠️ AND ONLY A SOLID ONE BLOCKS (5dr). LithTech collides with an
        // object only when the OBJECT is solid -- the rule the load already
        // applies to static brushes -- and a door's solidity is decided at run
        // time. Every door's triangles were handed over regardless, so a
        // NON-SOLID one was a wall you could see through: M08S04's vent
        // louvres (Door6, Door28, hatch1: RAYHIT, not SOLID) trapped the
        // player in the vent the game means you to climb out of. Rays keep the
        // server's rule, solid OR rayhit (you can still shoot the louvres); a
        // removed object is neither. LT_NONSOLIDDOORSBLOCK=1 = the old way.
        static const bool s_bOldSolid = []() {
            const bool b = (getenv("LT_NONSOLIDDOORSBLOCK") != 0);
            if (b) { printf("[BRUSH] LT_NONSOLIDDOORSBLOCK=1 -- non-solid doors block movement (old)\n"); fflush(stdout); }
            return b; }();
        const bool bLive    = LTServerHost_IsLive((void*)b.m_pRec);
        const bool bCollide = s_bOldSolid || (bLive && (b.m_pRec->m_Flags & FLAG_SOLID));
        const bool bRays = !LTServerBrush_RaysPassThrough(b.m_sName.c_str())
                        && (s_bOldSolid || (bLive && (b.m_pRec->m_Flags & (FLAG_SOLID | FLAG_RAYHIT))));
        // LT_BRUSHTRACE: where a movable brush's geometry COMES TO REST, as a
        // box -- its Pos is only the build origin (5dr, 5du).
        static const bool s_bRestTrace = (getenv("LT_BRUSHTRACE") != 0);
        float bmn[3] = { 1e30f, 1e30f, 1e30f }, bmx[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t t = 0; t + 2 < b.m_Tris.size(); t += 3)
        {
            const float v[3] = { b.m_Tris[t], b.m_Tris[t+1], b.m_Tris[t+2] };
            float o[4] = { v[0], v[1], v[2], 1.0f };
            if (bMoved) LT3D::TransformPoint(M, v, o);
            if (s_bRestTrace)
                for (int k = 0; k < 3; ++k) { if (o[k] < bmn[k]) bmn[k] = o[k]; if (o[k] > bmx[k]) bmx[k] = o[k]; }
            if (bCollide)
            {
                if (((t / 3) % 3) == 0)                                     // a triangle starts
                {
                    dynOwner.push_back((void*)b.m_pRec);
                    const size_t nTri = t / 9;
                    dynPoly.push_back(nTri < b.m_TriPoly.size() ? b.m_TriPoly[nTri] : 0u);
                }
                g_Dyn.push_back(o[0]); g_Dyn.push_back(o[1]); g_Dyn.push_back(o[2]);
            }
            if (bRays) { dynRay.push_back(o[0]); dynRay.push_back(o[1]); dynRay.push_back(o[2]); }
        }
        if (s_bRestTrace)
        {
            static std::vector<char> s_Moving;
            if (s_Moving.size() != g_Brushes.size()) s_Moving.assign(g_Brushes.size(), 0);
            const bool bNow = !g_MovedNow.empty() && g_MovedNow.back().m_nBrush == i;
            if (s_Moving[i] && !bNow)
                printf("[BRUSH] t=%.2f %s came to rest: box (%.0f %.0f %.0f)..(%.0f %.0f %.0f)\n",
                       LTServerHost_Time(), b.m_sName.c_str(), bmn[0], bmn[1], bmn[2], bmx[0], bmx[1], bmx[2]);
            s_Moving[i] = bNow ? 1 : 0;
        }
    }

    if (LTGetWorldCollide())
    {
        LTGetWorldCollide()->SetDynamic(g_Dyn);
        LTGetWorldCollide()->SetDynamicOwners(dynOwner, dynPoly);
        LTGetWorldCollide()->SetDynamicRay(dynRay);
    }
    g_bDynLive = true;
}


// ⚠️ A RAY AGAINST A BRUSH OBJECT'S ACTUAL GEOMETRY, where it is NOW.
//
// The ray test used the object's load-time box, moved by translation only --
// so a hinged shutter that had swung open still filled its whole window, and
// every shot Cate fired in M01S01 hit `vwindow2` 44 units in front of her.
// The donor intersects a world model against its BSP (fullintersectline.cpp
// i_HandlePossibleIntersection); this is the same answer from triangles.
//
// Returns -1 when hRec is not a brush object this module knows (the caller
// keeps its box rule), otherwise 1 for a hit (fraction and normal written),
// 0 for a miss.
namespace
{
    // One brush, one rule. Returns the nearest hit fraction (> 1 = none) and
    // writes its normal (facing the ray) and polygon.
    float BrushRay(const HitBrush& hb, const float* M, bool bMoved,
                   const float* pFrom3, const float* d, bool bTwoSided,
                   float* nBest, unsigned int* pBestPoly)
    {
        float fBest = 2.0f;
        for (size_t t = 0; t + 8 < hb.m_Tris.size(); t += 9)
        {
            float v[3][3];
            for (int k = 0; k < 3; ++k)
            {
                const float* src = &hb.m_Tris[t + (size_t)k*3];
                if (bMoved) { float o[4]; LT3D::TransformPoint(M, src, o); v[k][0]=o[0]; v[k][1]=o[1]; v[k][2]=o[2]; }
                else        { v[k][0]=src[0]; v[k][1]=src[1]; v[k][2]=src[2]; }
            }
            // Moller-Trumbore; front faces only unless bTwoSided.
            const float e1[3] = { v[1][0]-v[0][0], v[1][1]-v[0][1], v[1][2]-v[0][2] };
            const float e2[3] = { v[2][0]-v[0][0], v[2][1]-v[0][1], v[2][2]-v[0][2] };
            if (!bTwoSided)
            {
                const float fn[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
                if (fn[0]*d[0] + fn[1]*d[1] + fn[2]*d[2] >= 0.0f) continue;   // leaving, or edge-on
            }
            const float pv[3] = { d[1]*e2[2]-d[2]*e2[1], d[2]*e2[0]-d[0]*e2[2], d[0]*e2[1]-d[1]*e2[0] };
            const float det = e1[0]*pv[0] + e1[1]*pv[1] + e1[2]*pv[2];
            if (det > -1e-9f && det < 1e-9f) continue;
            const float inv = 1.0f / det;
            const float tv[3] = { pFrom3[0]-v[0][0], pFrom3[1]-v[0][1], pFrom3[2]-v[0][2] };
            const float u = (tv[0]*pv[0] + tv[1]*pv[1] + tv[2]*pv[2]) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const float qv[3] = { tv[1]*e1[2]-tv[2]*e1[1], tv[2]*e1[0]-tv[0]*e1[2], tv[0]*e1[1]-tv[1]*e1[0] };
            const float w = (d[0]*qv[0] + d[1]*qv[1] + d[2]*qv[2]) * inv;
            if (w < 0.0f || u + w > 1.0f) continue;
            const float f = (e2[0]*qv[0] + e2[1]*qv[1] + e2[2]*qv[2]) * inv;
            if (f < 0.0f || f > 1.0f || f >= fBest) continue;
            fBest = f;
            *pBestPoly = (t / 9 < hb.m_TriPoly.size()) ? hb.m_TriPoly[t / 9] : 0;
            float n[3] = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
            const float l = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            if (l > 0.0f) for (int k = 0; k < 3; ++k) n[k] /= l;
            // Facing the ray's origin.
            if (n[0]*d[0] + n[1]*d[1] + n[2]*d[2] > 0.0f) for (int k = 0; k < 3; ++k) n[k] = -n[k];
            for (int k = 0; k < 3; ++k) nBest[k] = n[k];
        }
        return fBest;
    }

    // LT_RAYDIFF=1 -- every brush ray is ALSO answered by the other rule, and
    // each disagreement is listed: which object, from where, and both answers.
    // A census, because two runs cannot be diffed -- the first different
    // answer changes the shared rand() sequence and the runs part ways.
    std::map<std::string, unsigned> g_RayDiff;
    unsigned g_nRayDiffAsked = 0;
    void RayDiffCensus()
    {
        unsigned n = 0;
        for (std::map<std::string, unsigned>::const_iterator i = g_RayDiff.begin(); i != g_RayDiff.end(); ++i) n += i->second;
        printf("\n[RAYDIFF] %u brush ray(s) asked, %u answered differently by the other rule\n", g_nRayDiffAsked, n);
        for (std::map<std::string, unsigned>::const_iterator i = g_RayDiff.begin(); i != g_RayDiff.end(); ++i)
            printf("          %6u  %s\n", i->second, i->first.c_str());
        fflush(stdout);
    }
}

int LTServerBrush_RayTest(const void* hRec, const float* pFrom3, const float* pTo3,
                          float* pFrac, float* pNormal3, unsigned int* pPolyId)
{
    unsigned int nBestPoly = 0;
    std::map<const void*, size_t>::const_iterator it = g_HitIndex.find(hRec);
    if (it == g_HitIndex.end()) return -1;
    const HitBrush& hb = g_HitBrushes[it->second];

    float M[16];
    const bool bMoved = (hb.m_nMovable >= 0) && Matrix(g_Brushes[(size_t)hb.m_nMovable], M);

    // ⚠️ ONE-SIDED, LIKE THE DONOR. intersect_line.cpp counts a polygon only
    // when the ray crosses it from its FRONT side to its back (a start ON the
    // plane counts as front), so a ray LEAVING a brush does not hit the face it
    // leaves through. The game relies on that for shoot-through surfaces:
    // CProjectile::HandleVectorImpact measures the thickness with a backward
    // ray and then continues the bullet from EXACTLY the far-side point. Two-
    // sided, that continuation re-hit the far face at distance 0 and the
    // bullet stopped -- M01S01 `powerline2` (Chain Link Fence, shoot-through)
    // swallowed every shot at the far-left balcony assassin (HANDOVER 5bh).
    // The stored winding is the front face: e1 x e2 points out of the brush
    // (measured, M01S01/M01S02/T01S03/M02S01). LT_TWOSIDEDBRUSH=1 = old rule.
    static const bool s_bTwoSided = []() {
        const bool b = (getenv("LT_TWOSIDEDBRUSH") != 0);
        if (b) { printf("[PHYS] LT_TWOSIDEDBRUSH=1 -- brush rays hit back faces too\n"); fflush(stdout); }
        return b; }();

    const float d[3] = { pTo3[0]-pFrom3[0], pTo3[1]-pFrom3[1], pTo3[2]-pFrom3[2] };
    float nBest[3] = { 0.0f, 1.0f, 0.0f };
    const float fBest = BrushRay(hb, M, bMoved, pFrom3, d, s_bTwoSided, nBest, &nBestPoly);

    static const bool s_bDiff = []() {
        const bool b = (getenv("LT_RAYDIFF") != 0);
        if (b) { printf("[PHYS] LT_RAYDIFF=1 -- brush rays also answered by the other rule\n"); fflush(stdout);
                 atexit(RayDiffCensus); }
        return b; }();
    if (s_bDiff)
    {
        ++g_nRayDiffAsked;
        float nOther[3]; unsigned int nOtherPoly = 0;
        const float fOther = BrushRay(hb, M, bMoved, pFrom3, d, !s_bTwoSided, nOther, &nOtherPoly);
        const bool bHit = fBest <= 1.0f, bOther = fOther <= 1.0f;
        const float fLen = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
        if (bHit != bOther || (bHit && fabsf(fBest - fOther) * fLen > 0.01f))
        {
            const std::string sName = (hb.m_pRec ? hb.m_pRec->m_sName : std::string("?"));
            const unsigned n = ++g_RayDiff[sName];
            if (n <= 6)
            {
                char a[32], b[32];
                if (bHit)   snprintf(a, sizeof(a), "HIT at %.1f", fBest * fLen);  else snprintf(a, sizeof(a), "miss");
                if (bOther) snprintf(b, sizeof(b), "HIT at %.1f", fOther * fLen); else snprintf(b, sizeof(b), "miss");
                printf("[RAYDIFF] t=%.3f %-20s %.0f-unit ray from (%.0f %.0f %.0f): %s %s, %s %s\n",
                       LTServerHost_Time(), sName.c_str(), fLen, pFrom3[0], pFrom3[1], pFrom3[2],
                       s_bTwoSided ? "two-sided" : "one-sided", a, s_bTwoSided ? "one-sided" : "two-sided", b);
                fflush(stdout);
            }
        }
    }

    if (fBest > 1.0f) return 0;
    if (pFrac) *pFrac = fBest;
    if (pPolyId) *pPolyId = nBestPoly;
    if (pNormal3) for (int k = 0; k < 3; ++k) pNormal3[k] = nBest[k];
    return 1;
}

const ILTWorldModelXform* LTServerBrush_Xform() { return &g_Xform; }
unsigned int LTServerBrush_Count()      { return (unsigned int)g_Brushes.size(); }
unsigned int LTServerBrush_MovedCount() { return g_nMoved; }

// Closest point on triangle ABC to P -- the same barycentric region test as
// ltworldcollide.cpp's ClosestOnTri (which is file-local there).
static void ClosestOnTriangle(const float* p, const float* a, const float* b,
                              const float* c, float* out)
{
    const float ab[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
    const float ac[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
    const float ap[3] = { p[0]-a[0], p[1]-a[1], p[2]-a[2] };
    #define LTB_DOT(x, y) ((x)[0]*(y)[0] + (x)[1]*(y)[1] + (x)[2]*(y)[2])
    const float d1 = LTB_DOT(ab, ap), d2 = LTB_DOT(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) { memcpy(out, a, sizeof(float)*3); return; }
    const float bp[3] = { p[0]-b[0], p[1]-b[1], p[2]-b[2] };
    const float d3 = LTB_DOT(ab, bp), d4 = LTB_DOT(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) { memcpy(out, b, sizeof(float)*3); return; }
    const float vc = d1*d4 - d3*d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    { const float v = d1 / (d1 - d3); for (int k = 0; k < 3; ++k) out[k] = a[k] + ab[k]*v; return; }
    const float cp[3] = { p[0]-c[0], p[1]-c[1], p[2]-c[2] };
    const float d5 = LTB_DOT(ab, cp), d6 = LTB_DOT(ac, cp);
    #undef LTB_DOT
    if (d6 >= 0.0f && d5 <= d6) { memcpy(out, c, sizeof(float)*3); return; }
    const float vb = d5*d2 - d1*d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    { const float w = d2 / (d2 - d6); for (int k = 0; k < 3; ++k) out[k] = a[k] + ac[k]*w; return; }
    const float va = d3*d6 - d5*d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    { const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); for (int k = 0; k < 3; ++k) out[k] = b[k] + (c[k]-b[k])*w; return; }
    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom, w = vc * denom;
    for (int k = 0; k < 3; ++k) out[k] = a[k] + ab[k]*v + ac[k]*w;
}

// ⚠️ RIDING A LIFT -- the donor's MO_MOVESTANDINGONS (client_iltphysics.cpp
// MovePushObjects, which CMoveMgr::OnObjectMove asks for whenever a solid
// world model moves). A sphere resting on a brush that moved this update is
// carried by the brush's own motion: the point is taken back to where it was
// relative to the brush (inverse of last frame's placement) and placed with
// this frame's. Resting = within the probe skin of one of the brush's
// last-frame triangles, contact normal up (y > 0.7) -- the same rule as the
// mover's ground probe (ltworldcollide.cpp).
bool LTServerBrush_CarryPoint(const float* pCentre3, float fRadius, float* pDelta3)
{
    pDelta3[0] = pDelta3[1] = pDelta3[2] = 0.0f;
    const float fR = fRadius + 0.75f;
    for (size_t m = 0; m < g_MovedNow.size(); ++m)
    {
        const MovedBrush& mb = g_MovedNow[m];
        const MovableBrush& b = g_Brushes[mb.m_nBrush];
        bool bOn = false;
        for (size_t t = 0; !bOn && t + 8 < b.m_Tris.size(); t += 9)
        {
            float w[9];
            for (int v = 0; v < 3; ++v)
            {
                float o[4];
                LT3D::TransformPoint(mb.m_Prev, &b.m_Tris[t + v*3], o);
                w[v*3] = o[0]; w[v*3+1] = o[1]; w[v*3+2] = o[2];
            }
            float c[3];
            ClosestOnTriangle(pCentre3, w, w + 3, w + 6, c);
            const float d[3] = { pCentre3[0]-c[0], pCentre3[1]-c[1], pCentre3[2]-c[2] };
            const float len2 = d[0]*d[0] + d[1]*d[1] + d[2]*d[2];
            if (len2 < fR*fR && len2 > 1e-8f && d[1] / sqrtf(len2) > 0.7f) bOn = true;
        }
        if (!bOn) continue;

        // Rigid inverse of last frame's placement: R^T, -R^T t.
        const float* P = mb.m_Prev;
        const float q[3] = { pCentre3[0]-P[3], pCentre3[1]-P[7], pCentre3[2]-P[11] };
        float rest[3];
        for (int r = 0; r < 3; ++r) rest[r] = P[0*4+r]*q[0] + P[1*4+r]*q[1] + P[2*4+r]*q[2];
        float o[4];
        LT3D::TransformPoint(mb.m_Now, rest, o);
        for (int k = 0; k < 3; ++k) pDelta3[k] = o[k] - pCentre3[k];
        return true;
    }
    return false;
}
