// ----------------------------------------------------------------------- //
//
// MODULE  : ltserverphysics.h
//
// PURPOSE : SERVER-SIDE MOVEMENT -- the reason nothing in this game falls,
//           slides, or is stopped by a wall.
//
// ⚠️ WHAT WAS MISSING, AND WHAT IT COST. Eighteen `ILTPhysics::` entry points
// and six more on `ILTServer` were stubs. The game sets a velocity and expects
// the object to move; it got LT_ERROR and an untouched out-parameter, so every
// physics-driven thing in the level stood still -- and `GetStandingOn` had to
// answer "nothing" (DEVIATIONS E-001) because there was nothing to ask.
//
// ⚠️ THIS IS NOT LITHTECH'S PHYSICS AND MUST NOT BE READ AS IT. It is a swept
// sphere against the level's solid triangles, plus the per-object state the
// game reads back. No mass-based response, no object-vs-object collision, no
// containers, no moving platforms carrying riders. See ltworldcollide.h, which
// makes the same disclaimer for the same reason and is the layer underneath.
//
// ⚠️ THE COLLISION IS THE WALK VIEWER'S, and deliberately a SECOND instance
// rather than a shared one. Walk mode is switched off whenever the server runs
// (skeleton_host.cpp: `!LTServerHost_Enabled() && LTWorldWalk_Init(...)`), so
// the two are never live together and the duplicate costs nothing at runtime.
// Sharing one would mean editing the 102/102-worlds path to serve a brand new
// one, which is a trade this port does not need to make yet.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_SERVER_PHYSICS_H__
#define __LT_SERVER_PHYSICS_H__

class CLTWorld;

// Builds collision for the world now loaded. Safe to call twice; a no-op when
// the geometry has not changed. Called from the server's world-load path.
void  LTServerPhys_BuildFor(const CLTWorld* pWorld);

// Forgets the collision. For a world change.
void  LTServerPhys_Reset();

bool  LTServerPhys_Ready();

// ⚠️ MOVES A SPHERE, SLIDING, and writes back where it ENDED -- which is not
// necessarily where it was asked to go. The game reads the object's position
// afterwards, so a mover that reported success while leaving the object inside
// a wall would be worse than one that refuses.
//
// Returns true when the object ended anywhere other than where it started.
// pbGround, when given, says whether it came to rest on a standable surface.
bool  LTServerPhys_MoveTo(void* hObj, const float* pWantPos3, bool* pbGround);

// ⚠️ NO COLLISION, BY DEFINITION. TeleportObject is the game's way of saying
// "put it there, I know what I am doing" -- respawns, level starts, cinematic
// camera cuts. Running it through the mover would make a spawn point inside
// thin geometry silently relocate the player.
void  LTServerPhys_TeleportTo(void* hObj, const float* pPos3);

// ⚠️ ONE STEP OF INTEGRATION for every object the game has given a velocity,
// an acceleration, or FLAG_GRAVITY. Call once per server tick, after the
// objects have run their MID_UPDATE -- an object that sets its velocity this
// frame should move this frame, not next.
void  LTServerPhys_Update(float fDeltaSeconds);

// The world's gravity, shared by every object with FLAG_GRAVITY.
void  LTServerPhys_SetGlobalForce(const float* pF3);
void  LTServerPhys_GetGlobalForce(float* pF3);

// ⚠️ How far up a ledge a moving object is lifted rather than stopped. The
// game sets this per character (CCharacter uses 24 units); with no step-up a
// character is stopped dead by a kerb, which reads as "the AI are stuck".
void  LTServerPhys_SetStairHeight(float f);
float LTServerPhys_GetStairHeight();

// What this object is resting on, or false when it is not resting on anything.
// ⚠️ Answers about the WORLD only -- there is no object-vs-object collision, so
// a character standing on a crate still reports nothing (DEVIATIONS E-001).
bool  LTServerPhys_GetGround(void* hObj);

// ⚠️ THE RAY THE GAME ASKS "WHAT AM I LOOKING AT?" WITH.
//
// `CPlayerObj::DoActivate` casts one to find the door or switch in front of the
// player (PlayerObj.cpp:3172) and every weapon casts one to find what it hit.
// While it was a stub, E did nothing and nothing could be shot.
//
// ⚠️ IT MUST RETURN AN OBJECT, NOT JUST A SURFACE. The activate path branches
// on `IsMainWorld(IInfo.m_hObject)`: a world hit means "nothing to operate",
// and only an OBJECT handle makes a door open. So this tests the world's
// triangles AND every live object's box, and returns whichever is nearer.
//
// ⚠️ Boxes, not meshes, for objects -- the server knows each object's dims and
// nothing more. A shot clips the corner of a character's box rather than its
// silhouette; that is the same approximation the original engine's broad phase
// makes, but it is an approximation and belongs in DEVIATIONS when it matters.
//
// pFilter is the game's ObjectFilterFn, honoured so a player does not shoot
// himself. Returns false when the segment hits nothing.
bool  LTServerPhys_RayTest(const float* pFrom3, const float* pTo3,
                           bool (*pFilter)(void* hObj, void* pUser),
                           void* pUser,
                           float* pPoint3, float* pNormal3, void** ppHitObj,
                           bool bObjects = true, bool bIgnoreNonSolid = false,
                           unsigned int* pPolyId = 0);

// ⚠️ THE GAME'S POLYGON FILTER (IntersectQuery::m_PolyFilterFn), which the
// engine never called. The donor asks it for every BSP polygon the ray reaches
// -- the main world and world-model objects alike -- and a polygon it rejects
// is SKIPPED, the ray carrying on (world/src/intersect_line.cpp:122). NOLF's
// Activate passes DoVectorPolyFilterFn, which rejects ST_INVISIBLE: that is
// what lets E reach a window through the invisible blocker standing in front
// of it (M01S01 `blocker3`/`blocker4` before `vwindow5..8`, HANDOVER 5bf),
// while the blocker still stops Cate herself.
// Same contract as LTServerPhys_RayTest; a hit with no polygon (an object's
// box) cannot be filtered and is kept. LT_NOPOLYFILTER=1 ignores the filter.
bool  LTServerPhys_RayTestFiltered(const float* pFrom3, const float* pTo3,
                                   bool (*pFilter)(void* hObj, void* pUser),
                                   void* pUser,
                                   bool (*pPolyFilter)(unsigned int nPoly, void* pUser),
                                   float* pPoint3, float* pNormal3, void** ppHitObj,
                                   bool bObjects, bool bIgnoreNonSolid,
                                   unsigned int* pPolyId);

// ---- census ------------------------------------------------------------ //
void  LTServerPhys_DumpCensus();

#endif // __LT_SERVER_PHYSICS_H__
