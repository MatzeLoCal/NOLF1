// ----------------------------------------------------------------------- //
//
// MODULE  : ltserverobjects.h
//
// PURPOSE : The server's CLASS SYSTEM and object registry -- the thing that
//           turns ObjectDLL's `BEGIN_CLASS(DoorKnob)` into an object that
//           exists.
//
// ⚠️ THIS IS THE PIECE EVERYTHING ELSE IS WAITING ON. `OnClientEnterWorld`
// returns NULL today for one reason: `CreatePlayer` asks
// `g_pLTServer->GetClass("CPlayerObj")`, gets a stub's null, and gives up
// before `CreateObject` is ever reached. No player, no triggers, no
// cinematics -- all of it starts here.
//
// HOW THE GAME PUBLISHES ITS CLASSES
//
// ObjectDLL exports `ObjectDLLSetup(int *nDefs, ILTServer*, int *version)`,
// which hands back an array of `ClassDef` (ltserverobj.h:218). Each carries the
// class name, its parent, the CF_ flags, a CONSTRUCT and DESTRUCT function, the
// property table, and `m_ClassObjectSize`. That is everything an engine needs
// to make one, and it is why this can be written at all: the game tells us how.
//
// ⚠️ `m_ClassObjectSize` is "set automatically" by the macros, so the engine
// allocates raw bytes of that size and lets `m_ConstructFn` placement-construct
// into them. Calling `new` on anything here would be wrong -- the engine does
// not know the C++ type.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_SERVER_OBJECTS_H__
#define __LT_SERVER_OBJECTS_H__

#include <string>
#include <vector>
#include <map>
#include <unordered_set>

class ILTServer;
struct ClassDef;
struct ObjectCreateStruct;
class BaseClass;
class CLTWorld;
struct LTWorldObject;
struct GenericProp;

class CLTServerObjects
{
public:

    CLTServerObjects();

    // Calls ObjectDLLSetup on an already-dlopen'd ObjectDLL and takes the
    // class table. Returns false when the module does not export it.
    bool                Init(void* pLib, ILTServer* pServer);

    unsigned int        ClassCount() const { return (unsigned int)m_Classes.size(); }

    // ⚠️ Case-INSENSITIVE, because the game asks for classes by literal name
    // from several places and the world file's `m_sType` is authored by a level
    // editor. A lookup that is right for `GetClass("CPlayerObj")` and wrong for
    // a world object's "GameStartPoint" would work for exactly as long as it
    // took to load a level.
    ClassDef*           FindClass(const char* pName) const;

    // Allocates `m_ClassObjectSize` bytes, placement-constructs through the
    // class's own construct function, registers the object and returns it.
    // Returns null on a bad class or a failed construct.
    BaseClass*          Create(ClassDef* pClass, ObjectCreateStruct* pStruct);

    unsigned int        ObjectCount() const { return (unsigned int)m_Objects.size(); }

    // The i'th live object, or null. Iteration order is creation order, which
    // is what the game's FindNamedObjects/GetNextObject walks expect.
    BaseClass*          ObjectAt(unsigned int i) const;

    // The class an object was created from, for GetObjectClass.
    ClassDef*           ClassOf(const BaseClass* p) const;

    // Every live object whose `Name` property matched pName at creation.
    void                FindByName(const char* pName, std::vector<BaseClass*>& out) const;

    // ⚠️ INSTANTIATE THE LEVEL. For every object the world file declares, look
    // its class up, construct it, and drive the engine's own creation
    // handshake: set the property context, MID_PRECREATE (where the game's
    // ReadProp calls back for its properties), then MID_INITIALUPDATE.
    //
    // ⚠️ Objects whose class the game does not define are SKIPPED and counted,
    // not treated as an error: a world names editor-only helpers that no
    // ObjectDLL class implements, and refusing the level over one of them would
    // trade a whole playable world for a tidy log.
    //
    // Returns how many it created; fills pnSkipped with the rest.
    unsigned int        CreateWorldObjects(const CLTWorld& world, int* pnSkipped);

    // ⚠️ THE PROPERTY CONTEXT. While an object is being created the game asks
    // the ENGINE for its properties by name, so the engine has to know which
    // object is currently being made. Null between creations, and a lookup
    // then correctly finds nothing rather than answering with the last
    // object's values -- which would be worse than failing.
    const LTWorldObject* CurrentProps() const { return m_pCurrentProps; }

    // ⚠️ SWAPS the property scope and hands back the old one, so a caller can
    // RESTORE it. Creation nests -- a character's attachments are created
    // inside its own initial update, while its world-file properties are
    // still the current scope -- so setting and clearing would cut the outer
    // object's reads short. See LTHost_CreateObjectProps.
    const LTWorldObject* SwapCurrentProps(const LTWorldObject* p)
    { const LTWorldObject* o = m_pCurrentProps; m_pCurrentProps = p; return o; }


    // Answers one property request from the current context. Returns false when
    // there is no context or no such property; the caller must then leave the
    // output ALONE, because the game reads it unconditionally.
    bool                ReadProp(const char* pName, GenericProp* pOut) const;

    // The raw Euler radians of a rotation property -- ReadProp returns the
    // quaternion, which is what GetPropGeneric's callers expect.
    bool                ReadPropEuler(const char* pName, float* pAngles3) const;

    // ⚠️ THE ENGINE'S OWN STATE FOR ONE GAME OBJECT, and the reason it exists.
    // `BaseClass` is the GAME's half; position, rotation, flags and dims belong
    // to the ENGINE and the game reaches them only through ILTServer. Without
    // somewhere to keep them every Get* is a stub that answers with the
    // caller's stack -- which is how the AI object crashed on its first
    // GetObjectUserFlags.
    //
    // ⚠️ Heap-allocated and never moved: HOBJECT is a POINTER to one of these,
    // so holding them by value in a vector would invalidate every handle the
    // game is carrying the moment the vector grew.
    struct Record
    {
        BaseClass*      m_pGame;        // the game's object
        ClassDef*       m_pClass;
        std::string     m_sName;
        float           m_Pos[3];
        float           m_Rot[4];       // quaternion
        float           m_Dims[3];
        unsigned int    m_Flags;
        unsigned int    m_UserFlags;
        unsigned char   m_Type;

        // ⚠️ PHYSICS STATE, and it lives here for the same reason m_Flags does:
        // it is the ENGINE's view of the object, the game reaches it only
        // through ILTPhysics, and exactly one place should know its layout.
        //
        // ⚠️ Zeroed on creation, so an object the game never gives a velocity
        // stays exactly where it was put. The integrator skips anything with
        // no velocity, no acceleration and no FLAG_GRAVITY, which is almost
        // every object in a level -- 731 of them on M01S01.
        float           m_Vel[3];
        float           m_Accel[3];
        float           m_fMass;
        float           m_fFriction;
        float           m_fForceIgnoreLimit;

        // ⚠️ Resting on something standable as of the last step. Read by
        // GetStandingOn, and the reason it is CACHED rather than recomputed on
        // demand: the game asks once per character per frame, and a fresh
        // sphere cast per ask would be the same work done twice.
        bool            m_bOnGround;

        // ⚠️ WHEN THIS OBJECT WANTS MID_UPDATE NEXT, in server seconds.
        // `SetNextUpdate(hObj, 0)` means "stop updating me" -- a trigger that
        // has fired says exactly that -- so 0 is NOT "update immediately".
        // Reading it the other way would run every finished object forever.
        float           m_fNextUpdate;

        // ⚠️ THE OBJECT'S SPECIAL-EFFECT MESSAGE -- LT2 keeps it on the object
        // (StartSpecialEffectMessage SETS it), a client gets it once, when the
        // object appears there, and it is saved with the object. Its payload
        // bytes (SFX id first); empty when the object never had one. Saved and
        // re-sent by Save/RestoreObjects (HANDOVER 5bz).
        std::vector<unsigned char> m_SFXMsg;

        // ⚠️ THE OBJECT'S COLOUR, r g b a, 0..255 -- LT2's m_ColorR/G/B/A
        // (shared/objectmgr.cpp: 255 each at creation), written by
        // ILTServer::SetObjectColor and saved with the object (game_serialize
        // .cpp). A TranslucentWorldModel's `Alpha` arrives here, and the brush
        // renderer reads it (LTServerBrush, HANDOVER 5cf): T01S02's projector
        // beam is authored at 0 and drew as a solid wedge while this was a stub.
        unsigned char   m_Color[4];

        // ⚠️ WHAT THE OBJECT IS DRAWN AS, kept so the CLIENT can be told. The
        // record carried a position and a type but never the model, which was
        // enough while nothing outside the server looked at it. Object
        // replication looks at it: a mirror with no filename is a position the
        // client can query and nothing it can draw.
        //
        // ⚠️ Written in Apply, not in Create, for the reason Apply exists --
        // the game rewrites the create struct in MID_PRECREATE and its model
        // is one of the things it rewrites.
        std::string     m_sFilename;
        std::vector<std::string> m_Skins;

        // ⚠️ THE ANIMATION THE **GAME** SELECTED, on the server side.
        //
        // The server owns which animation an object is playing -- every
        // cinematic's `msg victim (animate anim=walkstop)` and every AI's
        // state change sets it there -- while only the CLIENT can draw. So the
        // index is stored here and replicated, exactly like position.
        //
        // -1 means "the game has not chosen one"; the client then draws the
        // bind pose rather than guessing animation 0.
        int             m_nAnim;
        bool            m_bAnimLooping;

        // When m_nAnim last CHANGED, in server seconds. The pose is chosen by
        // elapsed time, so without this every character would sit on frame 0.
        float           m_fAnimStart;

        // ⚠️ CREATED FROM THE WORLD FILE, as opposed to spawned by game code.
        // The world renderer already draws world-file objects that name their
        // own model; drawing them again from here would put every prop in the
        // level on screen twice. See LTServerHost_DynamicObjects.
        bool            m_bFromWorld;
        // The index of the world-file object this record was made from, -1
        // for anything spawned at run time. See CLTServerObjects::WorldState.
        int             m_nWorldIndex;
        // The world file's own `Filename` for that object ("" if none) -- what
        // the world renderer's prop pass would draw. See DrawnLive.
        std::string     m_sWorldModel;
        // ObjectCreateStruct::m_ContainerCode -- what a container IS (CC_LADDER,
        // CC_WATER ...). "Can only be set during creation" (iltcsbase.h).
        unsigned short  m_nContainerCode;

        // ⚠️ A BRUSH OBJECT'S OWN BOX -- doors, switches, anything built from
        // world geometry. World-space bounds of the world model with this
        // object's name, taken at load, plus the position the object had then
        // so the box can follow the object when it moves.
        //
        // ⚠️ DELIBERATELY NOT m_Dims. The server's mover uses m_Dims as the
        // object's own collision size, and a door sized to its own brush would
        // collide with itself -- its brush is also in the static world -- and
        // never open. This box is only for being HIT: the Activate ray.
        //
        // Every one of T01S02's 105 brush objects (41 doors) had zero dims and
        // no box, so the Activate ray passed through them into "the world" and
        // the game played a surface sound instead of opening anything.
        bool            m_bHasBox;
        float           m_BoxMin[3], m_BoxMax[3];
        float           m_BoxRestPos[3];

        // ⚠️ A RESTORED BRUSH'S REST POSE COMES FROM THE WORLD FILE (5bz).
        // LTServerBrush_Build normally takes the pose the object has at load
        // as the one its geometry was built in -- true for a level just
        // created, false for a loaded game, where the door is wherever it was
        // saved (open). Set only by RestoreObjects: rest = m_BoxRestPos and
        // m_RestRot, the world model's centre and the file's rotation.
        bool            m_bRestFromFile;
        float           m_RestRot[4];

        // ⚠️ REMOVED BUT NOT YET DESTROYED. RemoveObject cannot free the
        // record where it is called: the game calls it from INSIDE an object's
        // own MID_UPDATE, which the update loop is walking by index, and from
        // inside another object's destructor. Freeing there either shifts the
        // loop's index or deletes something the caller is still standing in.
        //
        // So removal is a FLAG and the record is swept at a point the server
        // chooses. Everything that answers questions about objects -- the name
        // search, the update loop, the replication validity check -- treats a
        // dead record as gone from the moment it is marked.
        bool            m_bDead;

        // ⚠️ ASLEEP OR AWAKE -- LT2 OBJECT ACTIVATION (HANDOVER 5cp). An
        // inactive object gets no MID_UPDATE, no physics and no model keys.
        // The bits are lithtech.exe's: kInactive (OBJSTATE_INACTIVE),
        // kInactiveTouch, kAutoDeactivated -- 0 is awake. See
        // CLTServerObjects::SetActivity and ActivationPass.
        //
        // m_fDeactTime is SetDeactivationTime's value (0 = never falls asleep
        // by itself); m_fDeactTimer counts it down while the object is awake
        // and unseen. m_bDeactPending: it ran out this frame -- asleep at the
        // end of it unless the player's view wakes it first.
        unsigned char   m_nInactive;
        float           m_fDeactTime;
        float           m_fDeactTimer;
        bool            m_bDeactPending;
    };

    enum { kInactive = 0x08, kInactiveTouch = 0x10, kAutoDeactivated = 0x20 };

    // ⚠️ THE ONE PLACE AN OBJECT CHANGES STATE, and the one place the game is
    // told: MID_DEACTIVATING when it falls asleep, MID_ACTIVATING when it
    // wakes. CCharacter counts those and leaves/rejoins the character manager
    // on them, so they must alternate -- a change between two kinds of asleep
    // sends nothing (lithtech.exe 0x46a550 would send DEACTIVATING twice).
    void                SetActivity(Record* p, unsigned char nInactive, const char* pWhy);

    // lithtech.exe 0x476660: seen (or pinged) -- restart the countdown and
    // wake it if it fell asleep BY ITSELF. An object the game made inactive
    // stays inactive.
    void                Activate(Record* p, float fFrameTime, const char* pWhy);

    // ⚠️ THE PLAYER'S VIEW, ONCE PER FRAME, after the objects have updated
    // (the shipped engine does it while building the client's update):
    // everything the view position can see is activated, then everything
    // whose countdown ran out and was not seen falls asleep. pView3 null = no
    // client in the world yet: nothing wakes and nothing falls asleep.
    void                ActivationPass(const float* pView3, bool bAutoActivate, float fFrameTime);

    // ILTServer::PingObjects -- "activates any objects seen by this object".
    void                PingFrom(const Record* p, float fFrameTime);

    // Whether objects fall asleep at all: the engine's `AutoDeactivate`
    // console variable (NOLF sets it at startup, GameServerShell.cpp:118),
    // and not LT_NOAUTODEACTIVATE=1 (the A/B: everything always awake, old).
    static bool         AutoDeactivateOn();

    // ⚠️ IS THIS RECORD STILL IN THE LIST? A handle the game kept after the
    // object died is indistinguishable from a live one by inspection, and the
    // replication map holds handles across frames by construction.
    bool                IsLive(const Record* p) const;

    // In the list at all, dead (marked for removal this frame) or not. Never
    // dereferences p unless it is found -- safe on a garbage handle.
    bool                Contains(const Record* p) const;

    // Destructs every game object, newest first, THEN frees every record.
    // The list itself is left for the caller to clear.
    void                DestroyAll();

    // What a FREED record was ("name (class)"), from a bounded history of the
    // last 4096 frees -- for naming a stale handle the game still holds.
    // "" when unknown. ⚠️ The allocator reuses addresses: the newest entry wins.
    std::string         FreedName(const void* p) const;

    // Marks a record for removal. Returns false for a handle this server does
    // not own, which is the game holding a stale handle and worth the answer.
    bool                MarkDead(Record* p);

    // ⚠️ Destroys everything marked dead. Call at a point where nothing is
    // iterating the object list and no game frame is on the stack -- see
    // LTServerHost_Update, which sweeps after the objects have updated AND
    // after the message pump, because a destructor can send messages.
    unsigned int        SweepDead();

    // ⚠️ INTER-OBJECT LINKS -- ILTServer::CreateInterObjectLink. The game
    // holds an HOBJECT across frames and relies on MID_LINKBROKEN (pData = the
    // linked handle) to null it when EITHER end is removed (iltserver.h:540).
    // Without it the handle outlives the record: CinematicTrigger kept its
    // speaker after a skipped/finished cinematic removed the AI, and its next
    // MID_UPDATE called IsPlayingDialogue on freed memory (M12S02, HANDOVER
    // 5av). Owners are notified in SweepDead, before ANY dead record is
    // destroyed; BreakLink is silent, as the contract says.
    bool                CreateLink(Record* pOwner, Record* pLinked);
    void                BreakLink(Record* pOwner, Record* pLinked);
    unsigned int        LinkCount() const { return (unsigned int)m_Links.size(); }
    // Every link, (owner, linked) -- saved with the game (save v8, 5cu).
    const std::vector<std::pair<Record*, Record*> >& Links() const { return m_Links; }

    // ⚠️ WHICH CLASSES EXIST AND WHICH ONES TICK, counted per class. "The
    // keyframer does not run" is three different claims -- it was never
    // created, it was created and never scheduled, or it ticks and does
    // nothing -- and telling them apart by reading code is how an afternoon
    // goes. Printed by DumpClassCensus.
    void                DumpClassCensus() const;

private:
    mutable std::map<std::string, unsigned int> m_Updates;
public:

    // The record behind an HOBJECT, or null. ⚠️ Does NOT validate that the
    // pointer is one of ours -- the game passes back handles the engine gave
    // it, and a scan of every object on every call would make this quadratic
    // in a level with 731 of them.
    static Record*      FromHandle(void* h) { return (Record*)h; }

    // ⚠️ THE SECOND HALF OF CREATION. The game rewrites the create struct in
    // MID_PRECREATE -- GameStartPoint renames itself from "GameStartPoint0" to
    // "GameStartPoint" there, and the game then looks it up under the new name
    // -- so the engine's copy of name, position, rotation, flags and type has
    // to be taken AFTER that call, never before.
    void                Apply(Record* pRec, const ObjectCreateStruct* pStruct);

    // Unregisters, destructs and frees one object. Used when MID_PRECREATE
    // returns 0, which is the game declining to exist.
    void                Destroy(Record* pRec);

    Record*             RecordOf(const BaseClass* p) const;
    Record*             RecordAt(unsigned int i) const;

    // ⚠️ RUN ONE SERVER FRAME. Calls MID_UPDATE on every object whose next
    // update time has arrived, then clears it -- an object that wants another
    // must ask again, which is what SetNextUpdate is for.
    //
    // Returns how many objects were updated.
    unsigned int        UpdateObjects(float fServerTime, float fFrameTime = 0.0f);

    // Destroys everything, newest first.
    void                Term();

    // ⚠️ TEAR DOWN A WORLD WITHOUT TEARING DOWN THE ENGINE -- Term without
    // forgetting the class table, which the next world needs immediately.
    //
    // ⚠️ THIS IS WHAT A LEVEL CHANGE ACTUALLY IS, and not doing it was a real
    // bug rather than an omission: LoadWorld built the new level's 644 objects
    // on top of the old level's 426 and left both alive. The game found TWO
    // objects named "player" -- it says so itself, now that CPrint reaches a
    // terminal -- two WorldProperties, and two of every named trigger the two
    // levels happen to share.
    //
    // ⚠️ THE GAME IS WRITTEN FOR THIS. `CGameServerShell::PreStartWorld` drops
    // exactly the references that would dangle -- the command manager, the
    // character manager, the dialogue window and the active camera list -- and
    // the engine calls it immediately before this. The objects that must
    // SURVIVE a level change do not survive by being left in the list; they
    // are written to the keep-alive save first and constructed into the new
    // world by RestoreObjects.
    //
    // Returns how many it destroyed.
    unsigned int        ClearObjects();

    // ⚠️ WHAT BECAME OF WORLD-FILE OBJECT i (2026-10-07, HANDOVER 5bg).
    // The renderer draws level props and sprites from the WORLD FILE; this
    // is how it learns that the game has since hidden, shown or removed one.
    // -1: no record was ever made for it (a class the game declined, or no
    // server at all -- walk mode); 0: removed, or its FLAG_VISIBLE is clear;
    // 1: alive and visible.
    int                 WorldState(size_t nWorldIndex) const;

    // Where the game has world-file object i NOW -- position and rotation of
    // its live record (HANDOVER 5bt). False when there is none.
    bool                WorldPose(size_t nWorldIndex, float* pPos3, float* pQuat4) const;
    // ...and its animation: index (-1 none chosen), start time, looping (5cg).
    bool                WorldAnim(size_t nWorldIndex, int* pnAnim, float* pfStart, bool* pbLoop) const;

    // A world-file object whose model the GAME chose (Intelligence from its
    // intel type, a WeaponItem from its weapon): the world file names none or
    // another, so the prop pass cannot draw it and the live pass must.
    bool                DrawnLive(const Record* p) const;

    // ⚠️ THE LINK TO THE WORLD FILE, CARRIED THROUGH A SAVED GAME (5bz). A
    // loaded game builds every object from the save, so none of them knew
    // which level object it was: the prop pass drew the FILE's props again
    // on top of the restored ones, and picked-up items came back. The save
    // writes WorldMade() and each object's index; RestoreObjects puts both
    // back, into a world loaded geometry-only.
    bool                HasWorldObjects() const { return !m_WorldMade.empty(); }
    const std::vector<unsigned char>& WorldMade() const { return m_WorldMade; }
    const Record*       WorldRecord(size_t i) const { return i < m_ByWorldIndex.size() ? m_ByWorldIndex[i] : 0; }
    void                SetWorldMade(const std::vector<unsigned char>& made);
    void                RelinkWorld(Record* p, int nWorldIndex, const CLTWorld& world);

private:

    std::vector<Record*>        m_ByWorldIndex;     // null once destroyed
    std::vector<unsigned char>  m_WorldMade;        // 1 once a record existed

    const LTWorldObject*    m_pCurrentProps;
    std::vector<ClassDef*>  m_Classes;
    std::vector<Record*>    m_Objects;
    // ⚠️ EXACTLY THE RECORDS IN m_Objects, kept in step at its four change
    // points, so IsLive is a lookup and not a scan. IsLive used to walk the
    // whole list, and most of its callers walk the list too: every
    // character's container check cost N^2 per frame, and M01S02's part-2
    // cutscene (~2,000 objects, dozens of AIs awake) ran at ~3 frames a
    // second (HANDOVER 5bw).
    std::unordered_set<const Record*> m_Members;
    std::vector<std::pair<Record*, Record*> > m_Links;     // (owner, linked)
};

// ⚠️ MID_ACTIVATING -- "this object is becoming active". Sent ONCE to every
// object as it starts life, right after its MID_INITIALUPDATE (and, on a
// restore, before its MID_LOADOBJECT -- see ltserverobjects.cpp).
//
// The game learns an object is active ONLY from this message. GameBase sets
// USRFLG_GAMEBASE_ACTIVE on characters in response, and CCharacterFX::Update
// returns at its first line without it -- so for the whole life of this port
// no character's client effects ran: no lip sync, no head following a
// speaker, no flashlight, no breath. CCharacter also joins the character
// manager here, which is how AI finds other characters.
//
// Since HANDOVER 5cp objects DO fall asleep (LT2 auto-deactivation): see
// CLTServerObjects::SetActivity, which sends MID_DEACTIVATING on the way down
// and this again on the way up. LT_NOACTIVATE=1 for an A/B (no character is
// ever active; nothing is sent either way).
void LTServer_SendActivating(BaseClass* pObj);

// The record whose MID_UPDATE is running (a CLTServerObjects::Record*), or null.
const void* LTServerObjects_Updating();

// The world file's euler angles (radians, FORMATS.md) as the quaternion an
// ObjectCreateStruct carries -- yaw, then pitch, then roll.
void LTServerObjects_EulerToQuat(const float* pEuler3, float* pQuat4);

#endif // __LT_SERVER_OBJECTS_H__
