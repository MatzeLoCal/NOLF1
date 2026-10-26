// ----------------------------------------------------------------------- //
//
// MODULE  : ltserverhost.h
//
// PURPOSE : Loads NOLF's ObjectDLL and brings up its server shell.
//
// ⚠️ WHY A DYNAMIC LIBRARY AND NOT A LINK. NOLF ships TWO modules -- ObjectDLL
// and ClientShellDLL -- and each contains its own copy of `Shared/`, compiled
// with different `#if defined(_CLIENTBUILD)` bodies. Putting both in one Mach-O
// is a duplicate-symbol error, and HANDOVER.md §4.8 recorded that as the reason
// the server side "cannot be linked". It is not a limitation to work around:
// the two-module split IS the architecture, and reproducing it makes the
// problem disappear. `libnolfobject.dylib` is ObjectDLL, as a DLL.
//
// ⚠️ THIS IS THE GAME LOGIC. Everything the port has drawn so far is the
// CLIENT: geometry, models, the menu. Triggers, cinematics, AI, objectives,
// doors that open because something told them to -- all of that lives here, and
// none of it has ever run. Expect the first bring-up to be a census of stubs,
// exactly as the client side was.
//
// ⚠️ OPT-IN (`LT_SERVER=1`). The frame contract is built on a run that has no
// server in it; switching one on by default would change the recorded run
// before anyone had looked at whether it behaves.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_SERVER_HOST_H__
#define __LT_SERVER_HOST_H__

#include <vector>
#include <string>

class ILTServer;
class IServerShell;
class CLTFileMgr;

// Is the server side switched on for this run? (LT_SERVER=1)
bool            LTServerHost_Enabled();

// Loads the ObjectDLL dylib and calls its CreateServerShell(). Returns false
// and says why on stderr if the library is absent, will not load, or does not
// export the entry points. ⚠️ Never fatal: a run without the server is the
// state this port has been in all along and must keep working.
//
// pPath may be null, in which case NOLF_SERVER or a default beside the
// executable is used.
bool            LTServerHost_Start(CLTFileMgr* pFileMgr, const char* pPath);

// The shell, or null when it was never created.
IServerShell*   LTServerHost_GetShell();

// The ILTServer the shell was handed. Exposed so the host can report which of
// its methods the game actually reached -- the census is the point of the first
// bring-up, not the running.
ILTServer*      LTServerHost_GetServer();

// ⚠️ THE SEQUENCE THE REAL ENGINE DRIVES, in order. Each is a no-op when no
// shell was created, so a host can call them unconditionally.
//
//   StartWorld   PreStartWorld / PostStartWorld -- the world-load hooks. The
//                game uses these to find its level objects, so what they ask
//                for is the census of what world loading still owes it.
//   AddClient    OnAddClient then OnClientEnterWorld -- where CPlayerObj is
//                created. This is the step that turns "a shell exists" into
//                "there is a player in a world".
//   Update       the per-frame pump.
//
// ⚠️ They are SEPARATE calls rather than one Start() because the point of the
// first bring-up is to see how far each gets on its own. Collapsing them would
// make one stub abort the lot and hide everything behind it.
void            LTServerHost_StartWorld();
void            LTServerHost_AddClient();
void            LTServerHost_Update(float fElapsed);

// Puts the SERVER's player object at a position. A test hook -- see the .cpp
// for why moving only the client's copy is not enough.
bool            LTServerHost_PlacePlayer(const float* pPos3);

// Touch notification: how many pairs overlapped and how many messages that
// produced. Zero pairs means nothing in the level can be interacted with.
void            LTServerHost_DumpTouchCensus();
// The WORLD touch (5do): tells hObj it touched the world -- MID_TOUCHNOTIFY
// with the world as the other object, GetLastCollision answering the plane,
// polygon (0 = none) and stopping velocity. Called by the server mover.
// hDoor (5dp): the door it touched instead of the world, or 0; a door that
// asks for touches is told too, as the donor tells both sides.
void            LTServerHost_WorldTouch(void* hObj, void* hDoor, const float* pNormal3, float fPlaneDist,
                                        unsigned int nPoly, const float* pStopVel3, float fForce);
// True while hObj is a live object (not removed, not yet freed).
bool            LTServerHost_IsLive(void* hObj);

// How many stubs the last driven step reached. The census is the deliverable
// here, not the running.
int             LTServerHost_StubsLastStep();

// How many objects the last Update() actually ran, how many messages it
// delivered into the server, and the server clock.
unsigned int    LTServerHost_ObjectsUpdated();
unsigned int    LTServerHost_MessagesHandled();
float           LTServerHost_Time();

// ⚠️ DID THE GAME JUST START A WORLD? True exactly ONCE after the game's own
// RunWorld, then false again. The host uses it to do the CLIENT half of
// entering a world -- add the client, make its player object, call
// OnEnterWorld -- at the moment the game actually asked for a level, which is
// not something the host can predict at startup.
bool            LTServerHost_TakeWorldStarted();

// ⚠️ DELIVER A COMMAND TRANSITION TO THE SERVER SHELL. The client shell gets
// its own OnCommandOn from the host's frame loop; this is the other half.
// Does nothing when no server is running, which is the normal state in menus.
void            LTServerHost_OnCommand(int nCommand, bool bOn);

// ⚠️ The game-spawned models the in-game view must draw -- characters above
// all. See ILTWorldDynamicObjects. Always valid; empty when no world is up.
class ILTWorldDynamicObjects;
const ILTWorldDynamicObjects* LTServerHost_DynamicObjects();

// ⚠️ WHERE THE SERVER'S PLAYER IS, for the client to mirror onto its own
// player object. Returns false when there is no player yet -- and the caller
// must then leave its outputs alone rather than treat the origin as a position.
//
// ⚠️ This is ONE object's worth of replication and nothing more. It exists
// because the camera follows the player and the game will not leave its
// loading screen without a client-side player object; every other object still
// waits for a real server->client protocol.
bool            LTServerHost_GetPlayerTransform(float* pPos3, float* pQuat4);

// The server's player object (HOBJECT), or null before one exists.
void*           LTServerHost_PlayerHandle();

// ⚠️ THE CLIENT'S OLD WORLD CLOSES WHEN THE SERVER STARTS THE NEW ONE. Called
// at the top of LoadWorld, before the old level's mirrors are reset and before
// any new object exists -- so the client's OnExitWorld runs while its effects
// still belong to the old level, and nothing of the new one is wiped by it.
typedef void (*LTServerWorldClosingFn)();
void            LTServerHost_SetWorldClosingHook(LTServerWorldClosingFn fn);

// The k-th secondary animation tracker of a server object (AddTracker order):
// its animation index. For the client's read-through copy of an object's
// trackers. false (and 0) when the object has no such tracker.
bool            LTServerHost_GetTrackerAnim(void* hServer, int k, int* pAnim);

// ⚠️ THE SERVER SIDE OF OBJECT REPLICATION -- one read, and the answer to "is
// this handle still an object?" in the same call. See ltreplicate.h for why
// those two questions belong together: the replication map holds handles
// across frames, so a dead one is indistinguishable from a live one until
// someone asks the list.
//
// Returns false for a handle the server no longer owns. Writes every output on
// a true return.
// ⚠️ WHERE THE SERVER THINKS THE CLIENT IS LOOKING FROM -- SetClientViewPos's
// stored value. False until the game has set one.
//
// ⚠️ This is NOT the client's camera and does not move it. See the
// implementation for why the name invites exactly that mistake.
// ⚠️ THE SERVER'S SOUNDS GO THROUGH THE CLIENT'S MIXER, because there is one
// machine and one pair of speakers. The server host cannot reach the client's
// sound manager directly -- that would put CoreAudio in the server's link --
// so the HOST installs the player, the same shape LTMessage_SetClientDeliver
// and LTWorldWalk_SetSoundPlayer already use.
//
// A door, a footstep and an ambient SoundFX object are all server sounds; 1,292
// SoundFX objects are placed across 91 of the 102 shipped worlds.
typedef LTRESULT (*LTServerSoundFn)(PlaySoundInfo* pInfo);
void            LTServerHost_SetSoundPlayer(LTServerSoundFn fn);

// ⚠️ AND SO DO THE QUESTIONS ABOUT THEM, which is not a symmetry for its own
// sake. `CCharacter::UpdateSounds` reads
//
//     if (g_pLTServer->IsSoundDone(m_hCurDlgSnd, &bIsDone) != LT_OK || bIsDone)
//         KillDlgSnd();
//
// (Character.cpp:1821) -- so a stub returning LT_ERROR does not mean "I don't
// know", it means "kill the line". Measured on M01S01: every voice line in the
// opening cinematic was cut one frame after it started, and the twenty-line
// conversation ran in twenty seconds instead of two minutes.
//
// ⚠️ THE MIXER IS THE ONLY PLACE THAT KNOWS. A voice ends when its samples are
// consumed, so these have to reach the same manager the sounds were started
// on -- answering from a server-side timer would drift away from what is
// actually playing.
typedef LTRESULT (*LTServerSoundDoneFn)(HLTSOUND h, LTBOOL* pbDone);
typedef LTRESULT (*LTServerSoundDurFn)(HLTSOUND h, LTFLOAT* pfDuration);
typedef void     (*LTServerSoundKillFn)(HLTSOUND h);
void            LTServerHost_SetSoundQueries(LTServerSoundDoneFn fnDone,
                                             LTServerSoundDurFn fnDur,
                                             LTServerSoundKillFn fnKill,
                                             LTServerSoundKillFn fnKillLoop);

// ⚠️ ILTServer::IsCommandOn -- the server asking whether a CLIENT holds a
// command (fire, duck, run). PlayerObj.cpp:1213-1317 picks the player's
// server-side animation and movement status from it; the stub said "no" to
// everything. In LT2 the client sends its command bits with every update; in
// this one-process port the host installs a query on the same input state the
// client reads (one local client, so hClient is not consulted).
typedef LTBOOL (*LTServerCommandFn)(int nCommand);
void            LTServerHost_SetCommandQuery(LTServerCommandFn fn);

// The live state of world-file object i (-1 unknown, 0 hidden/removed, 1
// visible) -- installed into the world renderer by the host.
int             LTServerHost_WorldObjectLive(unsigned int nWorldIndex);
bool            LTServerHost_WorldObjectPose(unsigned int nWorldIndex, float* pPos3, float* pQuat4);
bool            LTServerHost_WorldObjectAnim(unsigned int nWorldIndex, int* pnAnim, float* pfSeconds, bool* pbLoop);

// Per-class object and MID_UPDATE counts. See CLTServerObjects::DumpClassCensus.
void            LTServerHost_DumpClassCensus();

bool            LTServerHost_GetClientViewPos(float* pPos3);

// The HOBJECT for one of the game's own object pointers, or null. ⚠️ The
// mapping is a member of the game's BaseClass, so it lives here rather than in
// whatever file happens to need it -- exactly one place knows that layout.
void*           LTServerHost_HandleOf(void* pGameObject);

bool            LTServerHost_GetObjectState(void* hObj,
                                            unsigned int* pType,
                                            float* pPos3, float* pQuat4,
                                            unsigned int* pFlags,
                                            unsigned int* pUserFlags,
                                            const char** ppFilename);

// ⚠️ THE OBJECT LIST, BY INDEX. Exposed for the physics integrator, which has
// to visit every object once a tick and lives in its own module so that neither
// the collision grid nor the class system ends up in the other's header.
//
// ⚠️ An index is only valid within one walk -- SweepDead compacts the list.
unsigned int    LTServerHost_ObjectCount();
void*           LTServerHost_ObjectAt(unsigned int i);   // HOBJECT, or null
// The class and object name of a server handle, for traces ("?" / "").
const char*     LTServerHost_ClassNameOf(void* hServer);
const char*     LTServerHost_ObjectNameOf(void* hServer);
bool            LTServerHost_RayBoxOf(void* hServer, float* pMin3, float* pMax3);   // what a ray tests
void            LTServerHost_CaptureSFX(void* pObject, const unsigned char* pBytes, unsigned int nBytes);
void            LTServerHost_LiveObjects(std::vector<void*>& out);  // every live HOBJECT
bool            LTServerHost_FindCharacter(const char* pSub, const float* pFrom3,
                                           float* pPos3, std::string* pName);

// LT_PLAYPROBE: the live AI characters (not the player), and how many of them
// have moved more than 8 units between two calls at least once since the last
// reset. bReset forgets everything (a new level).
void            LTServerHost_ProbeCharacters(bool bReset, int* pAlive, int* pMoved);

// ⚠️ WHERE THE LIVE CINEMATIC CAMERA IS, or false when none is live.
//
// A cinematic is a Camera object with USRFLG_CAMERA_LIVE (1<<5) set; the game
// turns the bit on when the cinematic starts and off when it ends
// (CinematicTrigger.cpp:471 reads exactly this). Finding it by that bit rather
// than by name is what makes this work on any level.
//
// ⚠️ THIS IS AN OBSERVER, NOT A RENDER PATH. `ILTClient::RenderCamera` is still
// a stub, so the game cannot draw its own camera; this lets the HOST point the
// world renderer at the same place, which is enough to SEE a cinematic and not
// enough to claim the game is rendering one. Do not mistake one for the other.
bool            LTServerHost_GetLiveCameraTransform(float* pPos3, float* pQuat4);

// ⚠️ THE ONE CLIENT'S HANDLE. The client half needs it to address the server,
// and it must be the SAME value the server handed the game in OnAddClient --
// a second invented handle would make every client->server message arrive from
// a client the game has never seen.
HCLIENT         LTServerHost_ClientHandle();

// Calls DeleteServerShell and unloads. Safe to call when nothing started.
void            LTServerHost_Stop();

// The containers a world point is inside (server handles), and a container's
// code -- for the CLIENT's GetPointContainers / GetContainerCode, which have
// no container objects of their own (HANDOVER 5bn).
unsigned int LTServerHost_PointContainers(const float* pPoint3, void** pList, unsigned int nMax);
bool         LTServerHost_ContainerCode(void* hServer, unsigned short* pCode);

// The server's GAME console variable (WorldProperties' fog, FarZ ...) -- what
// the client's GetServerConVarValueString/Float read. Null when unknown.
const char*  LTServerHost_GameConVarString(const char* pName);
bool         LTServerHost_GameConVarFloat(const char* pName, float* pValue);

#endif // __LT_SERVER_HOST_H__
