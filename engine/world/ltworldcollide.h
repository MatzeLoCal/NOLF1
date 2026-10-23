// ----------------------------------------------------------------------- //
//
// MODULE  : ltworldcollide.h
//
// PURPOSE : Stops the camera walking through walls: a swept sphere against the
//           level's solid triangles, with gravity, sliding and step-up.
//
// ⚠️ WHY A TRIANGLE SOUP AND NOT THE BSP. The world ships a BSP -- the model
// literally named `PhysicsBSP` -- and traversing it would be the engine's own
// answer. But its node records are only partly understood: the 14-byte half is
// read, the trailing 6-byte half is not, and `FORMATS.md` records that the
// per-node count is `6*(nNodes - k)` for a `k` nobody has identified. Building
// collision on a structure that is not fully decoded would put a subtle,
// intermittent bug -- falling through one floor in twenty -- on top of an
// unfinished parse.
//
// The triangles, by contrast, are completely known: every one of them passed
// the boundary check AND the meaning checks before it was published. So the
// collision uses the geometry that is certain and pays for it with a uniform
// grid, which is a memory cost rather than a correctness risk.
//
// ⚠️ WHAT IT IS NOT: the game's physics. No stairs-as-ramps, no crouch, no
// water, no moving platforms, no push-out of overlapping objects. It keeps a
// sphere out of the walls and on the floor.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_WORLD_COLLIDE_H__
#define __LT_WORLD_COLLIDE_H__

#include <vector>
#include <string>

class CLTWorld;

class CLTWorldCollide
{
public:

    CLTWorldCollide();

    // Gathers every solid triangle in the world and indexes it. Safe to call
    // on an empty world; Triangles() is then 0 and every move succeeds.
    void            Build(const CLTWorld& world);

    // ⚠️ Doors move, so their triangles cannot live in the static grid: the
    // grid is built once from world-space positions. They are held separately
    // and tested brute force, which is affordable because a level has tens of
    // doors of a few polygons each -- M01S01's openable ones come to a few
    // hundred triangles against 9,189 static ones.
    //
    // Replaced wholesale rather than edited: the caller rebuilds this only on
    // frames where a door actually moved.
    void            SetDynamic(const std::vector<float>& tris);
    unsigned int    DynamicTriangles() const { return (unsigned int)(m_Dyn.size() / 9); }

    // ⚠️ THE DOOR TRIANGLES RAYS SEE, when that is not all of them (DEVIATIONS
    // E-008: rays pass through the power lines). Contact and ground tests keep
    // SetDynamic's list. Never set (walk mode) = the same list.
    void            SetDynamicRay(const std::vector<float>& tris) { m_DynRay = tris; m_bDynRay = true; }

    // Names of the models to leave OUT of the static grid.
    void            SetExcluded(const std::vector<std::string>& names) { m_Excluded = names; }

    unsigned int    Triangles() const { return (unsigned int)(m_Tris.size() / 9); }
    unsigned int    Cells() const     { return (unsigned int)m_Cells.size(); }

    // Moves a sphere from pos by delta, sliding along whatever it hits.
    // Writes the resolved centre back into pos.
    //
    // ⚠️ `bGround` reports whether the sphere ended the move resting on a
    // surface flat enough to stand on -- the caller needs it to decide whether
    // gravity keeps accelerating, and a mover that does not report it makes
    // the camera jitter permanently between falling and landing.
    void            MoveSphere(float* pPos3, const float* pDelta3,
                               float fRadius, bool* pbGround) const;

    // ⚠️ THE RAY, and it is what "use the door" and "shoot the guard" are made
    // of. `IntersectSegment` is how the game asks what is in front of the
    // player: DoActivate casts one to find what to operate (PlayerObj.cpp:3172)
    // and every weapon casts one to find what it hit. With it stubbed, E does
    // nothing and nothing can be shot -- the two are the same missing call.
    //
    // Returns the NEAREST hit between from and to. pNormal3 is the surface
    // normal, pT the fraction along the segment (0..1).
    bool            RaySegment(const float* pFrom3, const float* pTo3,
                               float* pPoint3, float* pNormal3, float* pT) const;

    // ...and WHICH POLYGON it hit: LTWorldPolyId() of it, or 0 for a door
    // triangle (doors answer through the brush module). The game asks for the
    // polygon's surface type -- shoot-through, impact effect, footstep sound.
    bool            RaySegment(const float* pFrom3, const float* pTo3,
                               float* pPoint3, float* pNormal3, float* pT,
                               unsigned int* pPolyId) const;

    // True when a sphere at this position overlaps anything solid. Used to
    // decide whether a spawn point is inside geometry.
    bool            Overlaps(const float* pPos3, float fRadius) const;

private:

    void            CellRange(const float* pMin3, const float* pMax3,
                              int* pLo3, int* pHi3) const;
    void            Gather(const float* pMin3, const float* pMax3,
                           std::vector<int>& out) const;

    // Deepest penetration of a sphere against one triangle. Returns false when
    // they do not touch.
    bool            Contact(const float* pC3, float fRadius, int nTri,
                            float* pNormal3, float* pDepth) const;

    bool            ContactAny(const float* pC3, float fRadius, const float* pTris,
                               int nTris, float* pNormal3, float* pDepth) const;

    std::vector<float>              m_Tris;     // 9 floats per triangle
    std::vector<unsigned int>       m_TriPoly;  // LTWorldPolyId, one per triangle
    std::vector<float>              m_Dyn;      // doors, in world space, now
    std::vector<float>              m_DynRay;   // the ones rays test, if m_bDynRay
    bool                            m_bDynRay = false;
    std::vector<std::string>        m_Excluded;
    std::vector<std::vector<int> >  m_Cells;    // triangle indices per cell
    float                           m_Min[3];
    float                           m_CellSize;
    int                             m_Dim[3];
    bool                            m_bBuilt;
};

// ----------------------------------------------------------------------- //
// THE SHARED INSTANCE
//
// ⚠️ ONE GRID PER LOADED WORLD, because the server and the client both need to
// ask the same question and must get the same answer. NOLF drives the PLAYER
// from the client (`CPlayerObj`: "m_dwFlags &= ~FLAG_GRAVITY; // This is
// controlled by the client") and its AI from the server, so a port that built
// collision on one side only moves half the game -- which is exactly what the
// first version of the server physics did.
//
// ⚠️ The walk viewer keeps its OWN instance and is deliberately left alone. It
// is the 102/102-worlds path, it is never live at the same time as the server,
// and rewiring it to serve a newer module is a risk with no payoff.
//
// Build is idempotent per world pointer; Reset forgets it for a level change.
// ----------------------------------------------------------------------- //

CLTWorldCollide* LTGetWorldCollide();
void             LTWorldCollide_BuildFor(const CLTWorld* pWorld);
void             LTWorldCollide_Reset();
bool             LTWorldCollide_Ready();

// ⚠️ AN HPOLY THIS PORT HANDS OUT: ((world model index + 1) << 20) | polygon.
// Never 0 (the game reads 0 as "no polygon") and never INVALID_HPOLY.
inline unsigned int LTWorldPolyId(unsigned int nModel, unsigned int nPoly)
{ return ((nModel + 1u) << 20) | (nPoly & 0xFFFFFu); }
inline bool LTWorldPolyDecode(unsigned int id, unsigned int* pModel, unsigned int* pPoly)
{
    if (id == 0 || id == 0xFFFFFFFFu || (id >> 20) == 0) return false;
    *pModel = (id >> 20) - 1u; *pPoly = id & 0xFFFFFu; return true;
}

#endif // __LT_WORLD_COLLIDE_H__
