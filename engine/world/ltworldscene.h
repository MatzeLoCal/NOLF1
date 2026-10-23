// ----------------------------------------------------------------------- //
//
// MODULE  : ltworldscene.h
//
// PURPOSE : THE world the engine currently has loaded -- one of them, owned
//           here, shared by everything that wants to look at it.
//
// ⚠️ WHY THIS EXISTS AT ALL. The world scene used to be a file-static inside
// ltworldwalk.cpp, which was right for as long as the walk viewer was the only
// thing that drew a level. It is not any more: the GAME's client draws the
// same level through ILTClient::RenderCamera. Two loaders would mean two copies
// of a 2.3-million-triangle world, two texture caches, and -- far worse -- two
// answers to questions like "which surface is under this point".
//
// ⚠️ THIS PORT HAS ALREADY PAID FOR THAT MISTAKE THREE TIMES IN ONE DAY. The
// scene mirror, the object-model resolution and the sky each had the software
// and GPU paths deciding the same thing separately, and each drifted. The rule
// that came out of it: when two things must agree, do not give them one copy
// each -- give them one thing.
//
// ⚠️ IT IS NOT A "CURRENT LEVEL" IN THE GAME'S SENSE. It holds geometry,
// props, textures and the static collision the world file describes. Who is
// standing in it, what is triggered and what is animating belong to the SERVER
// (engine/server/) and are deliberately not here.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_WORLD_SCENE_H__
#define __LT_WORLD_SCENE_H__

class CLTFileMgr;
class CLTWorldScene;

// The one scene. Never null -- an engine that has loaded no world still has a
// scene object, it is simply empty, and `IsLoaded()` says so. Returning null
// for "no world" would put a null check at every call site and one of them
// would be missing.
CLTWorldScene*  LTGetWorldScene();

// Load a world into it, replacing whatever was there, and decode everything
// up front (see CLTWorldScene::PreloadAll -- lazy decoding costs 86ms on
// M01S01's first frame and repeats on entering any new area).
//
// Reports what it loaded, or why it could not, on stdout. Returns false and
// leaves the scene EMPTY on failure rather than half-populated.
bool            LTWorldScene_Load(CLTFileMgr* pFileMgr, const char* pWorldName);

// Is a world loaded, and which one. The name is what was asked for, so it can
// be compared against a later request to avoid reloading the same level.
bool            LTWorldScene_IsLoaded();
const char*     LTWorldScene_Name();

#endif // __LT_WORLD_SCENE_H__
