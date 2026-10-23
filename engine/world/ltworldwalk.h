// ----------------------------------------------------------------------- //
//
// MODULE  : ltworldwalk.h
//
// PURPOSE : Walk-around mode -- a camera you drive with the keyboard, drawing
//           a real level into the host's screen surface.
//
// ⚠️ OFF UNLESS ASKED FOR, and that is structural rather than a promise.
// LTWorldWalk_Init is only called when LT_WORLD names a world, and every hook
// in the host is guarded by LTWorldWalk_Active(). With it unset the frame loop
// runs the game's own Update exactly as before -- which is what the frame
// contract's 165 events and two reference frames were recorded against.
//
// ⚠️ WHAT THIS IS NOT: the game. It collides with the level and falls under
// gravity, but there is no server, nothing is simulated, and there are no
// characters -- they are spawned by game code rather than by the world file.
// It is a body in a level, and calling it anything more would be a lie.
//
// CONTROLS
//   W / A / S / D     walk
//   arrows            look
//   Space             jump
//   Shift             run
//   F                 toggle noclip (fly, Q/E for down/up)
//
// ----------------------------------------------------------------------- //

#ifndef __LT_WORLD_WALK_H__
#define __LT_WORLD_WALK_H__

struct LT2DSurf;
class CLTFileMgr;

// Loads the world named by LT_WORLD and stands the camera at its spawn.
// Returns false if there is nothing to do or the world will not load; the
// caller carries on with the ordinary menu path either way.
bool  LTWorldWalk_Init(CLTFileMgr* pFileMgr);

bool  LTWorldWalk_Active();

// Held-key state. ⚠️ Movement needs to know a key is DOWN, not that it was
// pressed: the host's input path delivers discrete events, so the state is
// accumulated here.
void  LTWorldWalk_OnKey(int nVK, bool bDown);

// ⚠️ Relative mouse motion, accumulated until the next frame consumes it.
// Accumulated rather than applied directly because several move events arrive
// per frame and each must turn the camera -- taking only the last would throw
// away most of a fast flick.
void  LTWorldWalk_OnMouse(int nDX, int nDY);

// Whether mouse motion turns the camera. Tab toggles it in-game; the HOST is
// what mirrors this onto the window's cursor capture, so that the camera itself
// has no dependency on the window layer and stays testable headlessly.
// ⚠️ THE GPU BACKEND, AS A HOOK, and for the same reason mouse look is a flag
// here rather than a call into the window: the `walk` test suite links this
// camera WITHOUT Metal, Cocoa or QuartzCore. A direct call would make a
// headless test drag in a GPU stack, which is how two earlier suites quietly
// stopped building. Null -- the default -- means the software renderer.
//
// Returns true if it drew and presented the frame; walk mode then does nothing
// further that frame.
class CLTWorldScene;
class ILTWorldModelXform;
typedef bool (*LTWalkGpuRenderFn)(CLTWorldScene& scene, int nWidth, int nHeight,
                                  const float* pEye3, const float* pAt3,
                                  float fFovY, const ILTWorldModelXform* pXform);
void  LTWorldWalk_SetGpuRenderer(LTWalkGpuRenderFn fn);

// Installs the Metal backend as that renderer. ⚠️ Defined in ltworldmetal.cpp,
// which ONLY the host links -- a build without it still compiles and runs, in
// software. Declared here rather than in ltworldrender.h so the host does not
// have to include the renderer to turn a GPU on.
bool  LTWorldMetal_Install();

// The GAME's camera through Metal (offscreen + readback) -- the function the
// host hands to LTScene_SetGpuCamera. Same signature as LTSceneGpuCameraFn.
class CLTWorldScene;
class ILTWorldModelXform;
class ILTWorldDynamicObjects;
bool  LTWorldMetal_GameCamera(CLTWorldScene& scene, unsigned char* pRGBA,
                              int nWidth, int nHeight,
                              const float* pEye3, const float* pAt3, float fFovY,
                              const ILTWorldModelXform* pXform,
                              const ILTWorldDynamicObjects* pDyn,
                              unsigned int* pnDynamicDrawn);

// The game's camera through the compositor (HANDOVER 5bq): into the native 3D
// target at pRect4 (x, y, w, h output pixels), not read back. Same signature
// as LTSceneGpuSceneFn.
bool  LTWorldMetal_GameScene(CLTWorldScene& scene, int nOutW, int nOutH,
                             const int* pRect4, bool bClear,
                             const float* pEye3, const float* pAt3, float fFovY,
                             const ILTWorldModelXform* pXform,
                             const ILTWorldDynamicObjects* pDyn,
                             const float* pScale3, const float* pAdd3,
                             unsigned int* pnDynamicDrawn);

// The MENU's 3D through the compositor (HANDOVER 5ce): RenderObjects' items
// into the native 3D target at pRect4. Same signature as LTSceneGpuObjectsFn.
struct LTSceneGpuItem;
struct LTModelPose;
bool  LTWorldMetal_MenuObjects(int nOutW, int nOutH, const int* pRect4, bool bClear,
                               const LTSceneGpuItem* pItems, int nItems,
                               const LTModelPose* pPoses);

// ⚠️ SOUND, AS A HOOK, for the same reason the GPU renderer is one: the `walk`
// suite links this camera with no AudioToolbox and no sound manager. A direct
// call would drag the audio stack into a headless test. Null -- the default --
// means silence, and silence is a correct way for walk mode to run.
//
// pPos3 may be null for a sound with no position (played in the head).
typedef void (*LTWalkSoundFn)(const char* pName, const float* pPos3,
                              float fInner, float fOuter);
void  LTWorldWalk_SetSoundPlayer(LTWalkSoundFn fn);

// Where the ears are, so the host can push it into the mixer each frame.
void  LTWorldWalk_GetListener(float* pPos3, float* pRight3);

void  LTWorldWalk_SetMouseLook(bool b);
bool  LTWorldWalk_GetMouseLook();

// Advances the camera and draws one frame into the surface.
void  LTWorldWalk_Frame(const LT2DSurf& surf, float fDeltaSeconds);

// Where the body is, and which way it faces. For diagnostics and tests --
// "it moved" is only checkable if someone can see the position.
void  LTWorldWalk_GetPos(float* pPos3, float* pYaw);

// One line for the console, or null.
const char* LTWorldWalk_Status();

#endif // __LT_WORLD_WALK_H__
