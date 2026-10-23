// ----------------------------------------------------------------------- //
//
// MODULE  : ltmessage.h
//
// PURPOSE : The engine's message buffers and the pipe the client and the
//           server talk through.
//
// ⚠️ WHY THIS IS THE PIECE THAT MATTERS. The server now builds and runs every
// level and the client renders. They do not talk. Everything a player would
// notice crosses this boundary: the client asking to start a level, the server
// telling the client where the player is, a trigger firing at a door, a
// cinematic saying it began. Until messages exist the two halves are two
// programs sharing a process.
//
// ⚠️ ONE IMPLEMENTATION, BOTH SIDES. The Write*/Read* calls are virtuals on
// ILTCSBase -- the base BOTH ILTClient and ILTServer derive from -- so a single
// buffer type serves both directions. Writing it twice is how the two would
// drift, which this renderer has already had to fix three times in one day for
// mirrors, object resolution and the scene mirror.
//
// ⚠️ IT DERIVES FROM ILTMessage, AND THAT IS NOT DECORATION. ltbasedefs.h:138
// says `typedef ILTMessage* HMESSAGEWRITE`, so every handle the game holds IS
// an ILTMessage pointer. Casting some unrelated struct through it would work
// right up until the game called one of ILTMessage's own methods -- which the
// modern half of the API (SendToObject, SetObjectSFXMessage) does. Deriving
// makes the obsolete and modern halves the same buffer.
//
// ⚠️ NOT A WIRE FORMAT. Single player in the original is still a client and a
// server, but in one process; the buffer is handed over, never serialised. So
// the layout does not have to match LithTech's -- only the ORDER of reads must
// match the order of writes, and that is the game's own code on both ends.
// Two deliberate consequences:
//
//   - COMPRESSED FORMS ARE NOT COMPRESSED. WriteCompVector/CompPos/CompRotation
//     store full floats. Compression is a bandwidth trade for a network that is
//     not here, and paying its precision cost for nothing would be a silent
//     rounding bug in object positions.
//   - DWORDS ARE POINTER-WIDE. ILTCSBase::WriteToMessageDWord was widened to
//     uintptr_t for LP64 (see the note in iltcsbase.h -- the game passes char*
//     through it and 44 readers cast back). ILTMessage::WriteDWord takes a
//     uint32 but stores the same 8 bytes, so the two APIs stay interchangeable
//     instead of desyncing by four bytes whenever the game mixes them.
//
// ----------------------------------------------------------------------- //

#ifndef __LT_MESSAGE_H__
#define __LT_MESSAGE_H__

#include "iltmessage.h"

#include <vector>

class BaseClass;


// ⚠️ WHERE A FINISHED MESSAGE GOES. The game says this by WHICH Start* call it
// used, and nothing later in the message says it again -- so it is recorded
// here at Start time. A message whose destination had to be guessed at delivery
// time is a message that gets delivered to the wrong half.
enum ELTMsgDest
{
    kLTMsg_Data = 0,    // StartHMessageWrite: a buffer written into another
    kLTMsg_ToClient,    // ILTServer::StartMessage        -> IClientShell::OnMessage
    kLTMsg_ToServer,    // ILTClient::StartMessage        -> IServerShell::OnMessage
    kLTMsg_ToObject,    // StartMessageToObject           -> BaseClass::ObjectMessageFn
    kLTMsg_ToShell,     // StartMessageToServer           -> IServerShell::OnObjectMessage
    kLTMsg_SFX,         // StartSpecialEffectMessage      -> IClientShell::SpecialEffectNotify
    kLTMsg_SFXInstant,  // StartInstantSpecialEffectMessage        "
};


class CLTMessage : public ILTMessage
{
public:

    CLTMessage(ELTMsgDest eDest = kLTMsg_Data, unsigned int nId = 0);
    virtual ~CLTMessage() {}

    ELTMsgDest      Dest() const { return m_eDest; }
    unsigned int    GetId() const { return m_nId; }

    // ---- routing, filled in by whichever Start* call made this ---------- //
    //
    // Only one of these is meaningful per destination, and the rest stay null.
    void*           m_hClient;      // kLTMsg_ToClient / ToServer: which client
    void*           m_hTarget;      // kLTMsg_ToObject:  HOBJECT being sent to
    void*           m_pSender;      // kLTMsg_ToObject / ToShell: LPBASECLASS
    void*           m_pSFXObject;   // kLTMsg_SFX:       LPBASECLASS the fx is on
    float           m_SFXPos[3];    // kLTMsg_SFXInstant

    // ⚠️ WHO FREES IT. A message handed to a queue belongs to the queue and is
    // deleted after delivery; one the game made with StartHMessageWrite belongs
    // to the game until it calls EndHMessageWrite. Release() therefore has to
    // know which it is, or it either double-frees a queued message or leaks
    // every data message ever built.
    bool            m_bQueued;

    // ---- the obsolete API's primitives, shared by both sides ------------ //
    void            PutByte(unsigned char v);
    void            PutWord(unsigned short v);
    void            PutDWord(uintptr_t v);          // ⚠️ 8 bytes -- see header note
    void            PutFloat(float v);
    void            PutString(const char* p);
    void            PutVector(const float* p3);
    void            PutRotation(const float* p4);
    void            PutRaw(const void* p, unsigned int n);
    void            PutMessage(const CLTMessage* p); // length-prefixed copy

    // ⚠️ EVERY READ PAST THE END RETURNS ZERO AND SETS THE OVERRUN FLAG rather
    // than reading adjacent memory. The game reads exactly what it wrote, so an
    // overrun means the two sides disagree -- and a zero that is REPORTED is a
    // bug you can find, while a byte of whatever follows in the heap is one you
    // cannot.
    unsigned char   TakeByte();
    unsigned short  TakeWord();
    uintptr_t       TakeDWord();
    float           TakeFloat();
    void            TakeString(char* pOut, unsigned int nMax);
    void            TakeVector(float* p3);
    void            TakeRotation(float* p4);
    void            TakeRaw(void* pOut, unsigned int n);
    CLTMessage*     TakeMessage();                  // caller frees

    bool            Overran() const  { return m_bOverran; }
    unsigned int    Size() const     { return (unsigned int)m_Data.size(); }

    // ⚠️ The BYTES, for the one caller that has to put them somewhere other
    // than a queue: SaveObjects writes them to a file. Null on an empty
    // message rather than &m_Data[0] on an empty vector, which is UB.
    const unsigned char* Bytes() const
                    { return m_Data.empty() ? 0 : &m_Data[0]; }

    // ⚠️ THE EFFECT TYPE OF A SPECIAL-EFFECT MESSAGE, which is NOT GetId().
    // StartSpecialEffectMessage makes the message with id 0 and the game then
    // writes the effect id as the first BYTE of the body (SFXMsgIds.h --
    // SFX_CAMERA_ID is 13). The census printed "id 0" for all 31 of M01S01's
    // attached effects, which is true and useless; this is the number that
    // says WHICH effect the client is not being told about.
    unsigned int    SFXId() const
                    { return m_Data.empty() ? 0u : (unsigned int)m_Data[0]; }
    unsigned int    ReadPos() const  { return m_nRead; }

    // ---- ILTMessage ----------------------------------------------------- //
    //
    // The modern half of the API, on the same bytes. Each is the matching
    // Put*/Take* above, so a message written through one API and read through
    // the other still lines up.
    LTRESULT    Release() override;

    LTRESULT    ReadByteFL(uint8 &val) override;
    LTRESULT    ReadWordFL(uint16 &val) override;
    LTRESULT    ReadDWordFL(uint32 &val) override;
    LTRESULT    ReadFloatFL(float &val) override;
    LTRESULT    ReadStringFL(char *pData, uint32 maxBytes) override;
    LTRESULT    ReadHStringFL(HSTRING &hString) override;
    LTRESULT    ReadHStringAsStringFL(char *pMsg, uint32 msgBufSize) override;
    LTRESULT    ReadRawFL(void *pData, uint32 len) override;
    LTRESULT    ReadVectorFL(LTVector &vec) override;
    LTRESULT    ReadCompVectorFL(LTVector &vec) override;
    LTRESULT    ReadCompPosFL(LTVector &vec) override;
    LTRESULT    ReadRotationFL(LTRotation &rot) override;
    LTRESULT    ReadCompRotationFL(LTRotation &rot) override;
    LTRESULT    ReadMessageFL(ILTMessage* &pMsg) override;
    LTRESULT    ReadObjectFL(HOBJECT &hObj) override;

    LTRESULT    WriteByte(uint8 val) override;
    LTRESULT    WriteWord(uint16 val) override;
    LTRESULT    WriteDWord(uint32 val) override;
    LTRESULT    WriteFloat(float val) override;
    LTRESULT    WriteString(char *pData) override;
    LTRESULT    WriteVector(LTVector &vec) override;
    LTRESULT    WriteCompVector(LTVector &vec) override;
    LTRESULT    WriteCompPos(LTVector &vec) override;
    LTRESULT    WriteRotation(LTRotation &rot) override;
    LTRESULT    WriteCompRotation(LTRotation &rot) override;
    LTRESULT    WriteRaw(void *pData, uint32 len) override;
    LTRESULT    WriteMessage(ILTMessage &msg) override;
    LTRESULT    WriteHString(HSTRING hString) override;
    LTRESULT    WriteHStringFormatted(int messageCode, ...) override;
    LTRESULT    WriteHStringArgList(int messageCode, va_list *pList) override;
    LTRESULT    WriteStringAsHString(char *pStr) override;
    LTRESULT    WriteObject(HOBJECT hObj) override;

    LTRESULT    ResetPos() override;
    LTRESULT    GetStatus(uint32 &flags) override;

private:

    bool        Take(void* pOut, unsigned int n);

    ELTMsgDest                  m_eDest;
    unsigned int                m_nId;
    std::vector<unsigned char>  m_Data;
    unsigned int                m_nRead;
    bool                        m_bOverran;
};


// ----------------------------------------------------------------------- //
// THE PIPE
//
// ⚠️ MESSAGES ARE QUEUED, NOT DELIVERED FROM INSIDE EndMessage. Three reasons,
// and the first one is not theoretical:
//
//   1. THE RECEIVER MAY NOT EXIST YET. The host brings the server up before
//      the client shell, so an object that sends to the client from
//      MID_INITIALUPDATE would be calling through a null shell.
//   2. RE-ENTRANCY. Delivering inside EndMessage runs the receiver's handler
//      in the middle of the sender's own update, so a handler that sends a
//      reply recurses into the sender. The original crosses a network here;
//      deferral is what that boundary actually is.
//   3. ORDER. A queue drained in order is reproducible, and this port is built
//      on runs being reproducible.
//
// Object-to-object messages are drained until the queue is EMPTY, so a trigger
// chain still completes within one frame; the cap only catches a loop.
// ----------------------------------------------------------------------- //

// Start one. Each corresponds to one of the game's Start* calls.
CLTMessage* LTMessage_StartData();
CLTMessage* LTMessage_StartToClient(void* hClient, unsigned char nId);
CLTMessage* LTMessage_StartToServer(void* hClient, unsigned char nId);
CLTMessage* LTMessage_StartToObject(void* pSender, void* hSendTo, unsigned int nId);
CLTMessage* LTMessage_StartToShell(void* pSender, unsigned int nId);
CLTMessage* LTMessage_StartSFX(void* pObject);

// ⚠️ AN OBJECT'S SPECIAL-EFFECT MESSAGE IS STATE, not only a delivery (LT2:
// StartSpecialEffectMessage sets the object's message). The server installs a
// hook that is told the payload every time one is ENDED, and keeps it on the
// object; LTMessage_QueueSFX sends a stored one again (a restored object).
typedef void (*LTMsgSFXCaptureFn)(void* pObject, const unsigned char* pBytes, unsigned int nBytes);
void              LTMessage_SetSFXCapture(LTMsgSFXCaptureFn fn);
void              LTMessage_QueueSFX(void* pObject, const unsigned char* pBytes, unsigned int nBytes);
CLTMessage* LTMessage_StartInstantSFX(const float* pPos);

// Finish one and hand it to its queue. Takes ownership.
LTRESULT    LTMessage_End(CLTMessage* pMsg, unsigned int nFlags);

// Free a message the game owns (StartHMessageWrite / ReadFromMessageHMessageRead).
void        LTMessage_Free(CLTMessage* pMsg);

// ----------------------------------------------------------------------- //
// ⚠️ OBJECT REFERENCES INSIDE A SAVE FILE -- AN INDEX, NEVER A POINTER.
//
// `WriteToLoadSaveMessageObject` / `ReadFromLoadSaveMessageObject` exist as a
// SEPARATE pair from the ordinary object-reference calls for one reason: a
// pointer is meaningless in a file. The game uses them 64 times -- a security
// camera's light, a volume brush's surface, a character's attachments, the
// weapon model in the player's hands.
//
// ⚠️ THEY WERE WRITING THE RAW POINTER AND READING IT BACK, and it survived
// only because nothing had ever restored a save. The first level change did,
// and every restored object came back holding a pointer to freed memory:
//
//     CHHWeaponModel::Setup      HHWeaponModel.cpp:179   <- SEGV on 0x3cb9
//     CWeapons::ChangeWeapon     Weapons.cpp:779
//     CAttachmentPosition::Load  Attachments.cpp:1602
//
// ⚠️ A DANGLING HANDLE IS NOT A CRASH YOU CAN COUNT ON. This one happened to
// land on an unmapped page; the same bug with a recycled allocation reads a
// live object of the wrong type and carries on.
//
// So: the save context maps HOBJECT -> 1-based index at write time, and index
// -> HOBJECT at read time. Index 0 is null and means "that object was not in
// the save set", which is a real and legitimate answer -- the game checks its
// handles.
//
// ⚠️ THE LOAD SIDE NEEDS EVERY OBJECT TO EXIST BEFORE ANY OF THEM LOADS,
// because A's blob may name B further down the file. RestoreObjects therefore
// creates in one pass and delivers MID_LOADOBJECT in a second.
//
// Outside a context both calls are no-ops that report once: the header says
// "use this only while saving/loading objects", so being outside one is a
// programming error and writing a pointer to disk anyway is how this started.
// ----------------------------------------------------------------------- //

// nCount handles, in the order they appear in the file. Index written is i+1.
void        LTSaveRefs_BeginSave(void* const* pHandles, unsigned int nCount);

// The handles the restore just created/found, same order as the file.
void        LTSaveRefs_BeginLoad(void* const* pHandles, unsigned int nCount);

void        LTSaveRefs_End();

// Counts for the census. ⚠️ "Unresolved" is the number that matters: a
// reference that came back null because its object was not in the save set is
// normal, but one that came back null because the index was out of range means
// the file and the restore disagree, and the game has been handed a wrong
// answer it cannot detect.
unsigned int LTSaveRefs_Written();
unsigned int LTSaveRefs_NullWrites();
// The i'th non-null handle written that was NOT in the set (0 past the end) --
// for LT_SAVEREFTRACE; cleared by BeginSave.
void*        LTSaveRefs_OutsideAt(unsigned int i);
unsigned int LTSaveRefs_Read();
unsigned int LTSaveRefs_Unresolved();

// ⚠️ THE RECEIVERS, registered by the two hosts rather than called directly.
// engine/shared cannot include engine/server or the skeleton host without
// making the shared layer depend on both halves it exists to keep apart.
// ⚠️ IT RETURNS WHETHER IT ACTUALLY DELIVERED. "The shell exists" and "the
// message reached something" are different facts -- a message to an object that
// has since been removed has a perfectly good receiver and still goes nowhere --
// and the census is worthless if the two are counted the same.
typedef bool (*LTMsgDeliverFn)(CLTMessage* pMsg);

void        LTMessage_SetClientDeliver(LTMsgDeliverFn fn);
void        LTMessage_SetServerDeliver(LTMsgDeliverFn fn);

// Drain. PumpServer runs the object and client->server queues (call it from the
// server's own update); PumpClient runs the server->client queue (call it from
// the host's frame loop, before the shell updates). Each returns how many it
// delivered.
unsigned int LTMessage_PumpServer();
unsigned int LTMessage_PumpClient();

// ⚠️ THE GAME'S MESSAGES ONLY -- special effects stay queued, in order. For a
// world start (5bz): LT2 sends a game message down the connection the moment
// it is written, so what the server said while it loaded the world (a loaded
// game's MID_PLAYER_LOADCLIENT) reaches the client BEFORE the world does,
// while object effects ride the first object update AFTER the client entered.
unsigned int LTMessage_PumpClientGameOnly();

// Throw away everything still queued -- at level change, and at shutdown.
void         LTMessage_Reset();

// ⚠️ THE CENSUS, which is the deliverable of a first bring-up. Every route
// counts what was sent, what was delivered, and what was dropped for want of a
// receiver -- so "the client got nothing" and "the client was never sent
// anything" stop looking the same from the outside.
struct LTMsgStats
{
    unsigned int nSent[7];
    unsigned int nDelivered[7];
    unsigned int nDropped[7];
    unsigned int nOverruns;
    unsigned int nLive;         // allocated and not yet freed
    unsigned int nPeakQueued;
};

const LTMsgStats& LTMessage_Stats();

// ⚠️ THE PLAYER'S STATE AS THE SERVER TOLD THE CLIENT, for LT_PLAYPROBE.
// CPlayerObj::ChangeState sends MID_PLAYER_STATE_CHANGE (105, MsgIDs.h) with
// the PlayerState as its first byte (ClientServerShared.h: ALIVE 1, DEAD 2,
// DYING 3). Deaths counts ALIVE -> DEAD/DYING transitions only: a player is
// created DEAD (PlayerObj.cpp:188) and that is not a death.
void              LTMessage_PlayerState(int* pState, unsigned int* pDeaths);

// ⚠️ A HANDLE THE CLIENT WROTE, READ ON THE SERVER. The client writes ITS OWN
// handles (its player object, its mirrors of server objects); the server needs
// the server object each stands for. The host installs the mapping, because
// this layer sees neither half. Unset = NULL for every client-written handle.
typedef void* (*LTMsgClientToServerFn)(void* hClient);
void              LTMessage_SetClientToServerObject(LTMsgClientToServerFn fn);
void              LTMessage_DumpStats();
const char*       LTMessage_DestName(ELTMsgDest e);

#endif // __LT_MESSAGE_H__
