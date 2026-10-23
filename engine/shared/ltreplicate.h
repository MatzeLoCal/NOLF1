// ----------------------------------------------------------------------- //
//
// MODULE  : ltreplicate.h
//
// PURPOSE : SERVER -> CLIENT OBJECT REPLICATION -- the thing the attached
//           special-effect pipe has been waiting on.
//
// ⚠️ WHY THIS EXISTS, AND IT IS NOT AN ABSTRACTION FOR ITS OWN SAKE. The
// server and the client each keep their own object list, and an HOBJECT is not
// an HLOCALOBJ. When the game says "attach this effect to that object" it
// names a SERVER object, and `LTHost_DeliverToClient` had nothing honest to
// hand the client, so it dropped the message and counted it. HANDOVER §5n
// measured what that costs: on M01S01's first ten seconds, six CAMERA effects
// and twenty-four CHARACTER effects thrown away -- which is why there are no
// cinematics and no people.
//
// WHAT IT DOES
//
//   For a server object, get or make a CLIENT object that mirrors it, and keep
//   the two in step until the server's dies.
//
// ⚠️ ON DEMAND, NOT WHOLESALE. The real engine replicates everything visible;
// this mirrors an object the first time something actually needs a client-side
// handle for it. A level has thousands of server objects and tens that a
// special effect ever names, so the wholesale version would spend its time on
// objects nobody asks about -- and, worse, it would have to guess which ones
// matter, which is exactly the kind of rule this port keeps getting wrong.
//
// ⚠️ IT KNOWS NEITHER HALF OF THE ENGINE. The server's record layout lives in
// ltserverobjects.h and the client's scene in ltscene.h; a module that included
// both would drag Cocoa into the server and the class system into the renderer.
// So the two are injected as function pointers, the same shape
// LTMessage_SetClientDeliver already uses, and this file compiles against
// nothing.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_REPLICATE_H__
#define __LT_REPLICATE_H__

// How the replicator reaches the two halves it is not allowed to include.
struct LTReplicateHooks
{
    // ⚠️ Reads a server object's CURRENT state, and returns false when the
    // handle is dead. That second job is the important one: the map holds
    // handles across frames by construction, so "is this still an object?" is
    // asked every sync and cannot be inferred from the pointer.
    //
    // Every out-parameter is written on a true return, including the ones the
    // caller may not care about -- this port has lost six days to functions
    // that wrote some of their outputs (HANDOVER §2).
    bool (*GetServerState)(void* hServer,
                           unsigned int* pType,
                           float* pPos3,
                           float* pQuat4,
                           unsigned int* pFlags,
                           unsigned int* pUserFlags,
                           const char** ppFilename);

    // Makes the client-side object. Returns null when the client cannot
    // represent that type, which is not an error -- see LTScene::CreateObject.
    void* (*CreateClient)(unsigned int nType,
                          const float* pPos3,
                          const float* pQuat4,
                          unsigned int nFlags,
                          const char* pFilename);

    void  (*DestroyClient)(void* hClient);
    void  (*MoveClient)(void* hClient, const float* pPos3, const float* pQuat4);

    // ⚠️ SYNCED LIKE A TRANSFORM, not set once at creation. The game's own
    // flags CHANGE -- USRFLG_CAMERA_LIVE is exactly a bit the server turns on
    // when a cinematic starts and off when it ends, and a mirror that copied
    // them at birth would show a camera that is never on.
    void  (*SetClientUserFlags)(void* hClient, unsigned int nUserFlags);

    // ⚠️ SYNCED, NOT SET ONCE -- and this is where every NPC was lost.
    //
    // A server object's MODEL can arrive after the object does.
    // `CAI::InitialUpdate` runs AFTER MID_PRECREATE and only then asks the
    // bute manager which character this is, so at the moment the mirror is
    // created the filename is still empty. CreateClient took it once and Sync
    // never looked again, so the client held a positioned, moving,
    // fully-replicated object with no model for the life of the level.
    //
    // Null is allowed: a host that cannot change a model after creation simply
    // leaves this unset and gets the old behaviour.
    void  (*SetClientModel)(void* hClient, const char* pFilename);
};

// Installs the hooks. Passing a null clears them and the replicator becomes
// inert -- which is what a headless test or the walk viewer wants.
void LTReplicate_SetHooks(const LTReplicateHooks* pHooks);

// The client object mirroring hServer, creating it on first sight. Returns
// null when there are no hooks, the server object is gone, or the client
// cannot represent it.
void* LTReplicate_Ensure(void* hServer);

// The mirror for hServer, or null. Never creates.
void* LTReplicate_Find(void* hServer);

// The reverse: the SERVER object a client mirror stands for, or null.
void* LTReplicate_ServerOf(void* hClient);

// ⚠️ "Is this server object the local player? Then here is the client object
// to ADOPT instead of making a mirror." Installed by the host; see
// LTReplicate_Ensure. An adopted object is synced like a mirror and is never
// destroyed by the replicator.
typedef void* (*LTReplicatePlayerFn)(void* hServer);
void  LTReplicate_SetPlayerHook(LTReplicatePlayerFn fn);

// ⚠️ Is this CLIENT object a mirror of a server object? The game view draws
// server objects from the server's records, so a client pass must skip their
// mirrors or every character would be drawn twice.
bool  LTReplicate_IsMirror(void* hClient);

// ⚠️ Pushes every mirror's transform from the server and REAPS the ones whose
// server object has died. Call once per server tick, after the server has run
// and before the frame is drawn -- a mirror synced after the draw is a frame
// behind, which is invisible standing still and very visible on a camera move.
void LTReplicate_Sync();

// Forgets everything, destroying the mirrors. For a world change.
void LTReplicate_Reset();

// ---- census ------------------------------------------------------------ //
//
// ⚠️ Counted for the same reason the message census is: "replication works"
// is not a claim anyone can check, and "41 mirrors live, 3 reaped, 0 refused"
// is. LT_REPTRACE=1 prints each event as it happens.

unsigned int LTReplicate_Live();
unsigned int LTReplicate_Made();
unsigned int LTReplicate_Reaped();
unsigned int LTReplicate_Refused();     // the client could not represent it
void         LTReplicate_DumpCensus();

#endif // __LT_REPLICATE_H__
