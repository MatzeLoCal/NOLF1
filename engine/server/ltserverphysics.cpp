// ----------------------------------------------------------------------- //
//
// MODULE  : ltserverphysics.cpp
//
// See ltserverphysics.h -- in particular the note that this is NOT LithTech's
// physics, which the numbers below will otherwise invite you to forget.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltserverphysics.h"
#include "ltserverobjects.h"
#include "ltserverhost.h"
#include "ltworldcollide.h"
#include "ltworld.h"
#include "ltserverbrush.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef CLTServerObjects::Record SRec;

namespace
{

    // ⚠️ NOLF's own default, not a guess: `PhysicsLT::SetGlobalForce` is fed
    // from WorldProperties, and a level that does not set one gets this. Y is
    // up in this world, so gravity is negative Y.
    float g_Force[3] = { 0.0f, -1000.0f, 0.0f };

    // CCharacter sets 24; the engine default before anyone sets one.
    float g_fStairHeight = 24.0f;

    unsigned int g_nMoved   = 0;    // MoveTo calls that changed a position
    unsigned int g_nBlocked = 0;    // MoveTo calls that ended where they began
    unsigned int g_nStepped = 0;    // moves rescued by the stair lift
    unsigned int g_nIntegrated = 0; // objects the integrator touched
    unsigned int g_nGravity = 0;    // objects seen with FLAG_GRAVITY
    unsigned int g_nWithVel = 0;    // objects seen with a non-zero velocity
    unsigned int g_FlagsSeen = 0;   // OR of every object's flags
    unsigned int g_nWithFlags = 0;

    bool Tracing()
    {
        static const bool b = (getenv("LT_PHYSTRACE") != 0);
        return b;
    }

    // ⚠️ THE SPHERE THAT STANDS IN FOR A BOX. Objects are boxes in this game
    // and the collision takes a sphere, so the radius is the box's smaller
    // horizontal half-extent -- the LARGER one would wedge a character in a
    // doorway it is meant to fit through, which reads as "the AI are stuck"
    // and is the harder failure to diagnose.
    float RadiusOf(const SRec* p)
    {
        float rx = p->m_Dims[0], rz = p->m_Dims[2];
        float r = (rx < rz) ? rx : rz;
        if (r < 1.0f) r = 1.0f;         // a zero-dims object still needs a body
        return r;
    }
}


// ⚠️ These three now just forward to the SHARED grid (ltworldcollide.h). They
// are kept as server-named entry points because the server's world-load path
// calls them and should not have to know where the grid lives.
void LTServerPhys_BuildFor(const CLTWorld* pWorld) { LTWorldCollide_BuildFor(pWorld); }
void LTServerPhys_Reset()                          { LTWorldCollide_Reset(); }
bool LTServerPhys_Ready()                          { return LTWorldCollide_Ready(); }


bool LTServerPhys_MoveTo(void* hObj, const float* pWantPos3, bool* pbGround)
{
    if (pbGround) *pbGround = false;

    SRec* p = CLTServerObjects::FromHandle(hObj);
    if (!p || !pWantPos3) return false;

    // ⚠️ WITH NO COLLISION BUILT THIS IS A TELEPORT, and saying so matters:
    // the walk viewer builds its own grid and the server builds this one, so
    // a path that forgot to build would move everything through walls and
    // look like a collision BUG rather than a missing call.
    if (!LTServerPhys_Ready())
    {
        for (int k = 0; k < 3; ++k) p->m_Pos[k] = pWantPos3[k];
        return true;
    }

    const float fR = RadiusOf(p);

    float start[3] = { p->m_Pos[0], p->m_Pos[1], p->m_Pos[2] };
    float delta[3] = { pWantPos3[0] - start[0],
                       pWantPos3[1] - start[1],
                       pWantPos3[2] - start[2] };

    if (delta[0] == 0.0f && delta[1] == 0.0f && delta[2] == 0.0f)
    {
        if (pbGround) *pbGround = p->m_bOnGround;
        return false;
    }

    float pos[3] = { start[0], start[1], start[2] };
    bool  bGround = false;
    CLTWorldCollide::Contacts contacts;
    LTGetWorldCollide()->MoveSphere(pos, delta, fR, &bGround, &contacts);

    // ⚠️ THE STAIR LIFT, and it is the difference between a level you can walk
    // through and one where every doorway sill stops you. If the flat part of
    // the move was blocked, try again from stair height up and drop back down.
    // Walk mode does exactly this (ltworldwalk.cpp) and it is why the viewer
    // can climb M01S01's steps; the server had no equivalent at all.
    const float fFlatWanted = fabsf(delta[0]) + fabsf(delta[2]);
    const float fFlatGot    = fabsf(pos[0] - start[0]) + fabsf(pos[2] - start[2]);

    if (g_fStairHeight > 0.0f && fFlatWanted > 0.01f && fFlatGot < fFlatWanted * 0.5f)
    {
        float lifted[3] = { start[0], start[1] + g_fStairHeight, start[2] };
        if (!LTGetWorldCollide()->Overlaps(lifted, fR))
        {
            float flat[3] = { delta[0], 0.0f, delta[2] };
            bool  bG2 = false;
            CLTWorldCollide::Contacts c2;
            LTGetWorldCollide()->MoveSphere(lifted, flat, fR, &bG2, &c2);

            const float fGot2 = fabsf(lifted[0] - start[0]) + fabsf(lifted[2] - start[2]);
            if (fGot2 > fFlatGot)
            {
                contacts = c2;          // the path taken is the one that touched
                float drop[3] = { 0.0f, -(g_fStairHeight + 1.0f), 0.0f };
                LTGetWorldCollide()->MoveSphere(lifted, drop, fR, &bG2);
                for (int k = 0; k < 3; ++k) pos[k] = lifted[k];
                bGround = bG2;
                ++g_nStepped;
            }
        }
    }

    for (int k = 0; k < 3; ++k) p->m_Pos[k] = pos[k];
    p->m_bOnGround = bGround;
    if (pbGround) *pbGround = bGround;

    // ⚠️ THE WORLD TOUCH (5do) -- the donor's CollideAgainstWorld
    // (moveobject.cpp) and collision response (collision.cpp). Without it a
    // projectile SLID along the wall it met and was never told: a crossbow
    // bolt fired at a wall lived on (no sticking), grenades could not bounce.
    // For each surface the sphere was pushed out of, in order: if the object
    // moves INTO it, the stopping velocity is -n(v.n) and the force that
    // times IMPULSE_TIME_CONSTANT (0.01) and the mass; the velocity offset
    // gets the extra -n x 0.05 (EXTRA_PENETRATION_ADD, "so friction will still
    // be applied"). A touch-notify object is told if the force reaches its
    // ignore limit (default 1000; projectiles set 0), with the LAST surface's
    // plane and polygon; then the offset is added to its velocity -- the
    // engine's half of every bounce the game computes. FLAG_GOTHRUWORLD
    // objects do not collide in the donor and are not touched here.
    // LT_NOWORLDTOUCH=1 = the old slide with nothing told (says so).
    static const bool s_bNoTouch = []() {
        const bool b = (getenv("LT_NOWORLDTOUCH") != 0);
        if (b) { printf("[PHYS] LT_NOWORLDTOUCH=1 -- moving objects slide along the world and are never told (old)\n"); fflush(stdout); }
        return b; }();
    if (contacts.m_nHits > 0 && !s_bNoTouch && !(p->m_Flags & FLAG_GOTHRUWORLD))
    {
        const int nN = contacts.m_nHits < CLTWorldCollide::Contacts::kMaxNormals
                     ? contacts.m_nHits : (int)CLTWorldCollide::Contacts::kMaxNormals;
        float off[3] = { 0, 0, 0 }, force[3] = { 0, 0, 0 };
        const float fMass = p->m_fMass > 0.0f ? p->m_fMass : 0.0f;
        for (int i = 0; i < nN; ++i)
        {
            const float* n = contacts.m_Normals[i];
            const float vd = (p->m_Vel[0] + off[0]) * n[0] + (p->m_Vel[1] + off[1]) * n[1]
                           + (p->m_Vel[2] + off[2]) * n[2];
            if (vd >= 0.0f) continue;
            for (int k = 0; k < 3; ++k)
            {
                const float add = -n[k] * vd;
                force[k] += add * 0.01f * fMass;
                off[k]   += add - n[k] * 0.05f;
            }
        }
        const float fForce2 = force[0]*force[0] + force[1]*force[1] + force[2]*force[2];
        const float fLimit = p->m_fForceIgnoreLimit;
        if ((p->m_Flags & FLAG_TOUCH_NOTIFY) && fForce2 >= fLimit * fLimit)
        {
            const float* nL = contacts.m_Normals[nN - 1];
            const float fDist = nL[0]*contacts.m_Point[0] + nL[1]*contacts.m_Point[1] + nL[2]*contacts.m_Point[2];
            LTServerHost_WorldTouch(hObj, contacts.m_pOwner, nL, fDist, contacts.m_nPoly, off, sqrtf(fForce2));
        }
        // The game may have removed it, moved it or set its velocity; the
        // offset still applies on top, as the donor applies it after.
        SRec* pAfter = CLTServerObjects::FromHandle(hObj);
        if (pAfter && LTServerHost_IsLive(hObj))
            for (int k = 0; k < 3; ++k) pAfter->m_Vel[k] += off[k];
    }

    const bool bMoved = (pos[0] != start[0] || pos[1] != start[1] || pos[2] != start[2]);
    if (bMoved) ++g_nMoved; else ++g_nBlocked;

    if (Tracing() && !bMoved)
        printf("[PHYS] blocked %-24s at (%.0f %.0f %.0f) r=%.0f\n",
               p->m_sName.empty() ? "(unnamed)" : p->m_sName.c_str(),
               start[0], start[1], start[2], fR);

    return bMoved;
}


void LTServerPhys_TeleportTo(void* hObj, const float* pPos3)
{
    SRec* p = CLTServerObjects::FromHandle(hObj);
    if (!p || !pPos3) return;
    for (int k = 0; k < 3; ++k) p->m_Pos[k] = pPos3[k];
    p->m_bOnGround = false;
}


void LTServerPhys_Update(float fDelta)
{
    if (fDelta <= 0.0f) return;

    // ⚠️ CLAMPED. A frame that stalled -- a texture decode, a breakpoint, a
    // level load -- would otherwise integrate a single step across the level
    // and fling everything through the geometry. Walk mode clamps for the same
    // reason at the same value.
    if (fDelta > 0.10f) fDelta = 0.10f;

    const unsigned int n = LTServerHost_ObjectCount();
    for (unsigned int i = 0; i < n; ++i)
    {
        void* h = LTServerHost_ObjectAt(i);
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) continue;

        // Asleep (HANDOVER 5cp): lithtech.exe moves only awake objects.
        if (p->m_nInactive) continue;

        const bool bGravity = (p->m_Flags & FLAG_GRAVITY) != 0;
        const bool bVel = (p->m_Vel[0] != 0.0f || p->m_Vel[1] != 0.0f || p->m_Vel[2] != 0.0f);
        if (bGravity) ++g_nGravity;
        if (bVel)     ++g_nWithVel;
        g_FlagsSeen |= p->m_Flags;
        if (p->m_Flags) ++g_nWithFlags;

        // ⚠️ THE EARLY OUT THAT MAKES THIS AFFORDABLE. A level is 731 objects
        // and a handful of them ever move; without this every light, every
        // AINode and every trigger would be swept against the world each tick.
        if (!bGravity &&
            p->m_Vel[0] == 0.0f && p->m_Vel[1] == 0.0f && p->m_Vel[2] == 0.0f &&
            p->m_Accel[0] == 0.0f && p->m_Accel[1] == 0.0f && p->m_Accel[2] == 0.0f)
            continue;

        for (int k = 0; k < 3; ++k)
        {
            float a = p->m_Accel[k];
            if (bGravity) a += g_Force[k];
            p->m_Vel[k] += a * fDelta;
        }

        // ⚠️ RESTING KILLS DOWNWARD VELOCITY, or gravity accumulates forever
        // while the object sits on the floor and the first sideways nudge
        // launches it at whatever speed it has silently reached.
        if (p->m_bOnGround && p->m_Vel[1] < 0.0f) p->m_Vel[1] = 0.0f;

        float want[3] = { p->m_Pos[0] + p->m_Vel[0] * fDelta,
                          p->m_Pos[1] + p->m_Vel[1] * fDelta,
                          p->m_Pos[2] + p->m_Vel[2] * fDelta };

        bool bGround = false;
        LTServerPhys_MoveTo(h, want, &bGround);
        ++g_nIntegrated;
    }
}


// ⚠️ Slab test against an axis-aligned box. Returns the near hit distance.
static bool RayBox(const float* o, const float* d, float fLen,
                   const float* mn, const float* mx, float* pT, float* pN)
{
    float tmin = 0.0f, tmax = fLen;
    int nAxis = 0; float nSign = -1.0f;

    for (int k = 0; k < 3; ++k)
    {
        if (fabsf(d[k]) < 1e-8f)
        {
            // Parallel to this slab: miss unless the origin is already inside.
            if (o[k] < mn[k] || o[k] > mx[k]) return false;
            continue;
        }
        const float inv = 1.0f / d[k];
        float t1 = (mn[k] - o[k]) * inv;
        float t2 = (mx[k] - o[k]) * inv;
        float s = -1.0f;
        if (t1 > t2) { float t = t1; t1 = t2; t2 = t; s = 1.0f; }
        if (t1 > tmin) { tmin = t1; nAxis = k; nSign = s; }
        if (t2 < tmax) tmax = t2;
        if (tmin > tmax) return false;
    }

    *pT = tmin;
    pN[0] = pN[1] = pN[2] = 0.0f;
    pN[nAxis] = nSign;
    return true;
}


bool LTServerPhys_RayTest(const float* pFrom3, const float* pTo3,
                          bool (*pFilter)(void*, void*), void* pUser,
                          float* pPoint3, float* pNormal3, void** ppHitObj,
                          bool bObjects, bool bIgnoreNonSolid,
                          unsigned int* pPolyId)
{
    if (ppHitObj) *ppHitObj = 0;
    if (pPolyId) *pPolyId = 0;
    unsigned int nPoly = 0;
    if (!pFrom3 || !pTo3) return false;

    float d[3] = { pTo3[0]-pFrom3[0], pTo3[1]-pFrom3[1], pTo3[2]-pFrom3[2] };
    const float fLen = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (fLen <= 0.0f) return false;
    for (int k = 0; k < 3; ++k) d[k] /= fLen;

    float fBest = fLen;
    float nBest[3] = { 0.0f, 1.0f, 0.0f };
    void* hBest = 0;
    bool  bHit = false;

    // --- the world -------------------------------------------------------- //
    if (LTWorldCollide_Ready())
    {
        float pt[3], n[3], t;
        unsigned int nWP = 0;
        if (LTGetWorldCollide()->RaySegment(pFrom3, pTo3, pt, n, &t, &nWP))
        {
            const float fDist = t * fLen;
            if (fDist < fBest)
            { fBest = fDist; for (int k=0;k<3;++k) nBest[k]=n[k]; hBest = 0; bHit = true; nPoly = nWP; }
        }
    }

    // --- every live object's box ------------------------------------------ //
    const unsigned int n = LTServerHost_ObjectCount();
    for (unsigned int i = 0; i < n; ++i)
    {
        void* h = LTServerHost_ObjectAt(i);
        SRec* p = CLTServerObjects::FromHandle(h);
        if (!p) continue;

        // ⚠️ A ZERO-SIZED OBJECT IS NOT A TARGET -- unless it is a BRUSH
        // object with a box of its own (see Record::m_bHasBox). Triggers, AI
        // nodes and sounds have neither, and treating them as points would
        // make the player "activate" invisible bookkeeping.
        // ⚠️ THE GAME'S QUERY FLAGS, which IntersectSegment used to drop.
        // Activate asks for IGNORE_NONSOLID: a trigger volume is not something
        // a ray stops on. Measured on T01S03 before this: an Activate ray aimed
        // at TrainingDoor1 from behind stopped at 'CivTrigger' -- a 128x64x64
        // invisible volume -- and never reached the door.
        if (!bObjects) break;
        // ⚠️ SOLID **OR RAYHIT** -- the donor's rule (fullintersectline.cpp
        // i_ISCallback). A character is shot through its CCharacterHitBox,
        // which is RAYHIT and NOT solid; testing SOLID alone let every bullet
        // pass through every enemy (M01S01, HANDOVER 5bb). Triggers carry
        // neither flag, so the Activate fix above still holds.
        // LT_SOLIDONLYRAYS=1 = the old rule.
        static const bool s_bSolidOnly = []() {
            const bool b = (getenv("LT_SOLIDONLYRAYS") != 0);
            if (b) { printf("[PHYS] LT_SOLIDONLYRAYS=1 -- rays ignore RAYHIT-only objects\n"); fflush(stdout); }
            return b; }();
        const unsigned int nHitFlags = s_bSolidOnly ? FLAG_SOLID : (FLAG_SOLID | FLAG_RAYHIT);
        if (bIgnoreNonSolid && !(p->m_Flags & nHitFlags)) continue;
        // DEVIATIONS E-008: rays pass through the power lines (the user's call).
        if (LTServerBrush_RaysPassThrough(p->m_sName.c_str())) continue;

        const bool bBox = p->m_bHasBox;
        if (!bBox && p->m_Dims[0] <= 0.0f && p->m_Dims[1] <= 0.0f && p->m_Dims[2] <= 0.0f)
            continue;

        // ⚠️ A NON-SOLID brush (a trigger volume, an AI volume) is not
        // something a ray stops on. Same flag the game's IGNORE_NONSOLID means.
        if (bBox && !(p->m_Flags & nHitFlags)) continue;

        // The game's own filter -- this is how the player avoids shooting
        // himself and how activate skips things it does not care about.
        if (pFilter && !pFilter(h, pUser)) continue;

        // ⚠️ A BRUSH OBJECT IS HIT BY ITS GEOMETRY, WHERE IT IS NOW -- not by
        // its load-time box. An opened shutter (M01S01 `vwindow2`) kept
        // catching every shot through the window. LT_BOXRAYS=1 = the old rule.
        static const bool s_bBoxRays = []() {
            const bool b = (getenv("LT_BOXRAYS") != 0);
            if (b) { printf("[PHYS] LT_BOXRAYS=1 -- brush objects hit-tested by their box\n"); fflush(stdout); }
            return b; }();
        if (bBox && !s_bBoxRays)
        {
            float fFrac, nb[3];
            unsigned int nBP = 0;
            const int r = LTServerBrush_RayTest(h, pFrom3, pTo3, &fFrac, nb, &nBP);
            if (r >= 0)
            {
                // Same tie rule as the box below: a brush beats the world it
                // is also part of when they are hit at the same distance.
                if (r == 1 && fFrac * fLen < fBest + 1.0f)
                { fBest = fFrac * fLen; for (int k=0;k<3;++k) nBest[k]=nb[k]; hBest = h; bHit = true; nPoly = nBP; }
                continue;
            }
        }

        float mn[3], mx[3];
        if (bBox)
        {
            // ⚠️ The box FOLLOWS THE OBJECT: a door that has slid open is hit
            // where it is now, not where the level put it. Translation only --
            // a swinging door is approximated by its rest box moved with it.
            for (int k = 0; k < 3; ++k)
            {
                const float d0 = p->m_Pos[k] - p->m_BoxRestPos[k];
                mn[k] = p->m_BoxMin[k] + d0;
                mx[k] = p->m_BoxMax[k] + d0;
            }
        }
        else
        {
            for (int k = 0; k < 3; ++k)
            {
                mn[k] = p->m_Pos[k] - p->m_Dims[k];
                mx[k] = p->m_Pos[k] + p->m_Dims[k];
            }
        }

        // ⚠️ A BOX THE RAY STARTS INSIDE IS NOT A HIT AT ZERO DISTANCE.
        //
        // The game casts from an object's own position constantly --
        // MoveObjectToFloor (ServerUtilities.cpp:480) drops a 10,000-unit ray
        // straight down from the player -- and the player's own hit box sits
        // at that exact point. Reporting it gives a hit at distance 0, so
        // MoveObjectToFloor computes fDist = 0, decides the object is already
        // on the floor, and leaves it hanging in the air. Measured on T01S02:
        // the cast from the lobby TeleportPoint returned the player's own box
        // instead of the floor 64 units below.
        //
        // Real LithTech's filter callbacks are how the game normally avoids
        // this, but MoveObjectToFloor passes none -- so the engine has to be
        // the one that does not answer "you are standing on yourself".
        bool bInside = true;
        for (int k = 0; k < 3; ++k)
            if (pFrom3[k] < mn[k] || pFrom3[k] > mx[k]) { bInside = false; break; }
        if (bInside) continue;

        float t, nb[3];
        // ⚠️ A BRUSH OBJECT WINS A TIE WITH THE WORLD. Its triangles are in
        // the collision set too (as dynamic geometry, which carries no owner),
        // and its box encloses them exactly -- so the box face and the
        // triangle are hit at the SAME distance. With a strict "<" the world
        // won that tie and every door reported itself as "the world": the
        // Activate ray reached TrainingDoor1's face at z=-1788 and the game
        // was told it had touched a wall.
        const float fSlack = bBox ? 1.0f : 0.0f;     // world units, like fBest
        if (RayBox(pFrom3, d, fBest + fSlack, mn, mx, &t, nb) && t < fBest + fSlack)
        { fBest = t; for (int k=0;k<3;++k) nBest[k]=nb[k]; hBest = h; bHit = true; nPoly = 0; }
    }

    if (!bHit) return false;

    if (pPoint3)  for (int k = 0; k < 3; ++k) pPoint3[k] = pFrom3[k] + d[k]*fBest;
    if (pNormal3) for (int k = 0; k < 3; ++k) pNormal3[k] = nBest[k];
    if (ppHitObj) *ppHitObj = hBest;
    if (pPolyId) *pPolyId = nPoly;
    return true;
}


bool LTServerPhys_RayTestFiltered(const float* pFrom3, const float* pTo3,
                                  bool (*pFilter)(void*, void*), void* pUser,
                                  bool (*pPolyFilter)(unsigned int, void*),
                                  float* pPoint3, float* pNormal3, void** ppHitObj,
                                  bool bObjects, bool bIgnoreNonSolid,
                                  unsigned int* pPolyId)
{
    static const bool s_bOff = []() {
        const bool b = (getenv("LT_NOPOLYFILTER") != 0);
        if (b) { printf("[PHYS] LT_NOPOLYFILTER=1 -- the game's polygon filter is ignored\n"); fflush(stdout); }
        return b; }();
    if (!pPolyFilter || s_bOff)
        return LTServerPhys_RayTest(pFrom3, pTo3, pFilter, pUser, pPoint3, pNormal3, ppHitObj,
                                    bObjects, bIgnoreNonSolid, pPolyId);

    if (ppHitObj) *ppHitObj = 0;
    if (pPolyId) *pPolyId = 0;
    if (!pFrom3 || !pTo3) return false;
    float d[3] = { pTo3[0]-pFrom3[0], pTo3[1]-pFrom3[1], pTo3[2]-pFrom3[2] };
    const float fLen = sqrtf(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (fLen <= 0.0f) return false;
    for (int k = 0; k < 3; ++k) d[k] /= fLen;

    // Re-cast from just past each rejected polygon. 0.25 units clears the
    // surface without stepping over anything a level could build.
    float from[3] = { pFrom3[0], pFrom3[1], pFrom3[2] };
    for (int nStep = 0; nStep < 16; ++nStep)
    {
        float pt[3], nrm[3];
        void* hHit = 0;
        unsigned int nPoly = 0;
        if (!LTServerPhys_RayTest(from, pTo3, pFilter, pUser, pt, nrm, &hHit,
                                  bObjects, bIgnoreNonSolid, &nPoly))
            return false;
        if (nPoly == 0 || pPolyFilter(nPoly, pUser))
        {
            if (pPoint3)  for (int k = 0; k < 3; ++k) pPoint3[k] = pt[k];
            if (pNormal3) for (int k = 0; k < 3; ++k) pNormal3[k] = nrm[k];
            if (ppHitObj) *ppHitObj = hHit;
            if (pPolyId) *pPolyId = nPoly;
            return true;
        }
        float fDone = 0.0f;
        for (int k = 0; k < 3; ++k) fDone += (pt[k] - pFrom3[k]) * d[k];
        if (fDone + 0.25f >= fLen) return false;
        for (int k = 0; k < 3; ++k) from[k] = pt[k] + d[k] * 0.25f;
    }
    return false;
}


void LTServerPhys_SetGlobalForce(const float* pF3)
{
    if (!pF3) return;
    for (int k = 0; k < 3; ++k) g_Force[k] = pF3[k];
}

void LTServerPhys_GetGlobalForce(float* pF3)
{
    if (!pF3) return;
    for (int k = 0; k < 3; ++k) pF3[k] = g_Force[k];
}

void  LTServerPhys_SetStairHeight(float f) { g_fStairHeight = (f >= 0.0f) ? f : 0.0f; }
float LTServerPhys_GetStairHeight()        { return g_fStairHeight; }

bool LTServerPhys_GetGround(void* hObj)
{
    SRec* p = CLTServerObjects::FromHandle(hObj);
    return p ? p->m_bOnGround : false;
}


void LTServerPhys_DumpCensus()
{
    // ⚠️ PRINTS EVEN WHEN EVERY COUNTER IS ZERO, once collision exists. "The
    // physics ran and moved nothing" is a RESULT -- it is how this module's
    // first run was found to be doing nothing at all -- and a census that goes
    // quiet in exactly that case is the one that hides it.
    if (!LTWorldCollide_Ready() && !g_nIntegrated) return;

    printf("\n[PHYS] server movement\n");
    printf("       collision %u triangle(s); %u integrated step(s)\n",
           LTGetWorldCollide()->Triangles(), g_nIntegrated);
    printf("       moves: %u went somewhere, %u blocked, %u rescued by the stair lift\n",
           g_nMoved, g_nBlocked, g_nStepped);
    printf("       %u object(s) carry FLAG_GRAVITY, %u have a velocity\n",
           g_nGravity, g_nWithVel);
    printf("       flags: %u object-tick(s) carried any flag; union 0x%08x\n",
           g_nWithFlags, g_FlagsSeen);
    if (!g_nIntegrated)
        printf("       ⚠️  NOTHING INTEGRATED. Either no object has velocity or\n"
               "           gravity, or the integrator is not being called.\n");
    if (!LTGetWorldCollide()->Triangles())
        printf("       ⚠️  NO COLLISION BUILT -- every move was a teleport.\n");
    fflush(stdout);
}
