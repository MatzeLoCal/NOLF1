// ----------------------------------------------------------------------- //
//
// MODULE  : ltserverbrush.h
//
// PURPOSE : MOVING BRUSH GEOMETRY IN GAME MODE -- doors, above all.
//
// ⚠️ A DOOR IS NOT A MODEL. It is world geometry owned by a game object, and
// three separate systems have to agree about where it is:
//
//     the RENDERER   draws its triangles           (ILTWorldModelXform)
//     COLLISION      stops the player on them      (SetDynamic)
//     the RAY TEST   lets Activate hit it          (Record::m_bHasBox)
//
// In game mode none of the first two existed. The static collision grid was
// built WITH the doors in it, so a door blocked its doorway forever, and
// RenderCamera passed no transform, so an opening door never moved on screen.
// Walk mode had all of this (CLTWorldDoors) because it simulates doors itself;
// the GAME owns them instead, and nothing carried the game's answer across.
//
// ⚠️ THE SERVER IS THE AUTHORITY. Each movable brush object's transform is
// derived from its record -- where the level put it versus where it is now --
// so the engine never second-guesses the game's own door logic.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_SERVER_BRUSH_H__
#define __LT_SERVER_BRUSH_H__

class CLTWorld;
class ILTWorldModelXform;

// Capture the movable brushes of a freshly loaded world: their names, their
// rest triangles, and the exclusion list the static collision grid needs.
// ⚠️ Call BEFORE building collision -- it decides what collision leaves out.
void LTServerBrush_Build(const CLTWorld& world);

void LTServerBrush_Reset();

// Re-place every moved brush: hands collision the transformed triangles.
// Cheap and idempotent; does nothing when nothing has moved.
void LTServerBrush_Update();

// Recomputes which brush objects are hidden (no FLAG_VISIBLE, or removed).
void LTServerBrush_UpdateVisibility();

// The renderer's view of the same answer. Never null; empty before a world.
const ILTWorldModelXform* LTServerBrush_Xform();

// A ray against a brush object's actual geometry in its current placement.
// -1: not a brush object; 0: miss; 1: hit (pFrac along the segment, normal).
// ⚠️ A DEVIATION FROM THE ORIGINAL (Docs/DEVIATIONS.md E-008, the user's
// call 2026-10-12): rays pass through the POWER LINES -- world models named
// `powerline...` (M01S01 powerline1-3, M01S03 powerline1/5). On Windows they
// catch every standing shot at M01S01's far-left balcony and deflect it by up
// to +-0.05 rad (HANDOVER 5bh, 5bv). Rays only: what they collide with is
// unchanged. LT_WIRESCATCHSHOTS=1 = the original (says so).
bool LTServerBrush_RaysPassThrough(const char* pObjectName);

int LTServerBrush_RayTest(const void* hRec, const float* pFrom3, const float* pTo3,
                          float* pFrac, float* pNormal3, unsigned int* pPolyId = 0);

unsigned int LTServerBrush_Count();
unsigned int LTServerBrush_MovedCount();

// A sphere resting on a brush that moved in the last update: true, and how
// far that brush carried the point (rotation included). False: not riding.
bool LTServerBrush_CarryPoint(const float* pCentre3, float fRadius, float* pDelta3);

#endif // __LT_SERVER_BRUSH_H__
