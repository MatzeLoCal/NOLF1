// ----------------------------------------------------------------------- //
//
// MODULE  : ltmessage.cpp
//
// See ltmessage.h for why the buffer is an ILTMessage, why the format is not a
// wire format, and why delivery is queued rather than immediate.
//
// ⚠️ THIS FILE ALSO DEFINES ILTCSBase's MESSAGE VIRTUALS. They are declared in
// iltcsbase.h WITHOUT `= 0`, so the interface's own vtable needs out-of-line
// bodies -- gen_skeleton.py used to emit them as stubs. It now leaves this
// family alone (see its HANDWRITTEN set) and they live here, once, for both
// halves of the game. That is the whole point: ILTCSBase is the base ILTClient
// and ILTServer share, so one definition here is one behaviour on both sides.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltreplicate.h"
#include "ltmessage.h"

#include "iltcsbase.h"
#include "ltstringmgr.h"
#include "ltrecord.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <map>
#include <vector>
#include <execinfo.h>

// ----------------------------------------------------------------------- //
// The buffer
// ----------------------------------------------------------------------- //

namespace { LTMsgStats g_Stats = {{0},{0},{0},0,0,0}; }


CLTMessage::CLTMessage(ELTMsgDest eDest, unsigned int nId)
    : m_hClient(0), m_hTarget(0), m_pSender(0), m_pSFXObject(0)
    , m_bQueued(false)
    , m_eDest(eDest), m_nId(nId), m_nRead(0), m_bOverran(false)
{
    m_SFXPos[0] = m_SFXPos[1] = m_SFXPos[2] = 0.0f;
    ++g_Stats.nLive;
}


void CLTMessage::PutRaw(const void* p, unsigned int n)
{
    if (!p || !n) return;
    const unsigned char* pb = (const unsigned char*)p;
    m_Data.insert(m_Data.end(), pb, pb + n);
}

void CLTMessage::PutByte(unsigned char v)      { m_Data.push_back(v); }
void CLTMessage::PutWord(unsigned short v)     { PutRaw(&v, sizeof(v)); }
void CLTMessage::PutFloat(float v)             { PutRaw(&v, sizeof(v)); }

// ⚠️ EIGHT BYTES, ALWAYS -- see the header. The game pushes char* through this
// field and casts back on the read side; on Win32 that round-tripped because a
// pointer was 32 bits, and under LP64 a 4-byte slot silently truncates it.
void CLTMessage::PutDWord(uintptr_t v)         { PutRaw(&v, sizeof(uintptr_t)); }

void CLTMessage::PutString(const char* p)
{
    // ⚠️ Length-prefixed, not NUL-terminated. A NUL scan cannot tell an empty
    // string from a missing one, and the read side has to know how far to
    // advance even when the caller's buffer is too small to take it all.
    const unsigned int n = p ? (unsigned int)strlen(p) : 0;
    PutRaw(&n, sizeof(n));
    if (n) PutRaw(p, n);
}

void CLTMessage::PutVector(const float* p3)    { PutRaw(p3, sizeof(float) * 3); }
void CLTMessage::PutRotation(const float* p4)  { PutRaw(p4, sizeof(float) * 4); }

void CLTMessage::PutMessage(const CLTMessage* p)
{
    const unsigned int n = p ? (unsigned int)p->m_Data.size() : 0;
    PutRaw(&n, sizeof(n));
    if (n) PutRaw(&p->m_Data[0], n);
}


bool CLTMessage::Take(void* pOut, unsigned int n)
{
    if (m_nRead + n > m_Data.size())
    {
        // ⚠️ ZERO THE OUTPUT AND SAY SO. The alternative -- leaving the
        // caller's variable alone -- is the uninitialised-out-parameter family
        // this port has now found six times, and here the engine is the one
        // that would be creating it.
        if (pOut) memset(pOut, 0, n);
        if (!m_bOverran) ++g_Stats.nOverruns;
        m_bOverran = true;
        return false;
    }
    if (pOut) memcpy(pOut, &m_Data[m_nRead], n);
    m_nRead += n;

    // ⚠️ AUTOMATIC RESET AT THE END, which iltcsbase.h promises in as many
    // words ("the message will AUTOMATICALLY reset when you hit the end"). The
    // game relies on it: a handler that reads a message to the end and is
    // called again on the same buffer expects to start over.
    if (m_nRead >= m_Data.size()) m_nRead = 0;
    return true;
}

unsigned char CLTMessage::TakeByte()   { unsigned char v = 0;  Take(&v, sizeof(v)); return v; }
unsigned short CLTMessage::TakeWord()  { unsigned short v = 0; Take(&v, sizeof(v)); return v; }
uintptr_t CLTMessage::TakeDWord()      { uintptr_t v = 0;      Take(&v, sizeof(v)); return v; }
float CLTMessage::TakeFloat()          { float v = 0.0f;       Take(&v, sizeof(v)); return v; }
void CLTMessage::TakeVector(float* p3) { if (p3) { p3[0]=p3[1]=p3[2]=0.0f; Take(p3, sizeof(float)*3); } }

void CLTMessage::TakeRotation(float* p4)
{
    if (!p4) return;
    // ⚠️ Identity, not zero, when there is nothing to read. A zero quaternion
    // is not a rotation -- it collapses whatever it is applied to -- so the
    // failure would show up as an object that vanished rather than one that
    // did not turn.
    p4[0] = p4[1] = p4[2] = 0.0f; p4[3] = 1.0f;
    Take(p4, sizeof(float) * 4);
}

void CLTMessage::TakeRaw(void* pOut, unsigned int n) { Take(pOut, n); }

void CLTMessage::TakeString(char* pOut, unsigned int nMax)
{
    if (pOut && nMax) pOut[0] = '\0';

    unsigned int n = 0;
    if (!Take(&n, sizeof(n))) return;

    // ⚠️ The read position advances by the FULL length even when the caller's
    // buffer is smaller. Advancing by what fitted would leave the rest of the
    // string in the stream and desync every field after it.
    if (m_nRead + n > m_Data.size()) { m_bOverran = true; ++g_Stats.nOverruns; return; }

    if (pOut && nMax)
    {
        const unsigned int nCopy = (n < nMax - 1) ? n : (nMax - 1);
        if (nCopy) memcpy(pOut, &m_Data[m_nRead], nCopy);
        pOut[nCopy] = '\0';
    }
    m_nRead += n;
    if (m_nRead >= m_Data.size()) m_nRead = 0;
}

CLTMessage* CLTMessage::TakeMessage()
{
    unsigned int n = 0;
    if (!Take(&n, sizeof(n))) return 0;
    if (m_nRead + n > m_Data.size()) { m_bOverran = true; ++g_Stats.nOverruns; return 0; }

    CLTMessage* p = new CLTMessage(kLTMsg_Data, 0);
    if (n) p->PutRaw(&m_Data[m_nRead], n);
    m_nRead += n;
    if (m_nRead >= m_Data.size()) m_nRead = 0;
    return p;
}


// ----------------------------------------------------------------------- //
// ILTMessage -- the modern half of the API, on the same bytes.
// ----------------------------------------------------------------------- //

LTRESULT CLTMessage::Release()
{
    // ⚠️ A QUEUED MESSAGE IS NOT THE GAME'S TO FREE. It belongs to the queue
    // until the pump delivers it; releasing it here would leave the pump
    // holding a dangling pointer, which is exactly the kind of fault that
    // shows up three frames later somewhere else.
    if (m_bQueued) return LT_OK;
    LTMessage_Free(this);
    return LT_OK;
}

LTRESULT CLTMessage::ReadByteFL(uint8 &val)   { val = TakeByte();  return m_bOverran ? LT_ERROR : LT_OK; }
LTRESULT CLTMessage::ReadWordFL(uint16 &val)  { val = TakeWord();  return m_bOverran ? LT_ERROR : LT_OK; }
LTRESULT CLTMessage::ReadDWordFL(uint32 &val) { val = (uint32)TakeDWord(); return m_bOverran ? LT_ERROR : LT_OK; }
LTRESULT CLTMessage::ReadFloatFL(float &val)  { val = TakeFloat(); return m_bOverran ? LT_ERROR : LT_OK; }

LTRESULT CLTMessage::ReadStringFL(char *pData, uint32 maxBytes)
{
    TakeString(pData, maxBytes);
    return m_bOverran ? LT_ERROR : LT_OK;
}

static bool TakeHStringText(CLTMessage* m, char* buf, unsigned int nMax);   // below, with WriteHString

LTRESULT CLTMessage::ReadHStringFL(HSTRING &hString)
{
    char buf[1024];
    hString = TakeHStringText(this, buf, sizeof(buf)) ? LTGetStringMgr()->Create(buf) : (HSTRING)0;
    return m_bOverran ? LT_ERROR : LT_OK;
}

LTRESULT CLTMessage::ReadHStringAsStringFL(char *pMsg, uint32 msgBufSize)
{
    TakeHStringText(this, pMsg, msgBufSize);
    return m_bOverran ? LT_ERROR : LT_OK;
}

LTRESULT CLTMessage::ReadRawFL(void *pData, uint32 len)
{
    return Take(pData, len) ? LT_OK : LT_ERROR;
}

LTRESULT CLTMessage::ReadVectorFL(LTVector &vec)
{
    float v[3]; TakeVector(v); vec.Init(v[0], v[1], v[2]);
    return m_bOverran ? LT_ERROR : LT_OK;
}

// ⚠️ The compressed forms read the SAME bytes as the uncompressed ones -- see
// the header. They are separate entry points only because the game calls them
// by different names.
LTRESULT CLTMessage::ReadCompVectorFL(LTVector &vec) { return ReadVectorFL(vec); }
LTRESULT CLTMessage::ReadCompPosFL(LTVector &vec)    { return ReadVectorFL(vec); }

LTRESULT CLTMessage::ReadRotationFL(LTRotation &rot)
{
    float q[4]; TakeRotation(q); rot.Init(q[0], q[1], q[2], q[3]);
    return m_bOverran ? LT_ERROR : LT_OK;
}

LTRESULT CLTMessage::ReadCompRotationFL(LTRotation &rot) { return ReadRotationFL(rot); }

LTRESULT CLTMessage::ReadMessageFL(ILTMessage* &pMsg)
{
    pMsg = TakeMessage();
    return pMsg ? LT_OK : LT_ERROR;
}

LTRESULT CLTMessage::ReadObjectFL(HOBJECT &hObj)
{
    hObj = (HOBJECT)TakeDWord();
    return m_bOverran ? LT_ERROR : LT_OK;
}

LTRESULT CLTMessage::WriteByte(uint8 val)    { PutByte(val);  return LT_OK; }
LTRESULT CLTMessage::WriteWord(uint16 val)   { PutWord(val);  return LT_OK; }
LTRESULT CLTMessage::WriteDWord(uint32 val)  { PutDWord((uintptr_t)val); return LT_OK; }
LTRESULT CLTMessage::WriteFloat(float val)   { PutFloat(val); return LT_OK; }
LTRESULT CLTMessage::WriteString(char *pData){ PutString(pData); return LT_OK; }

LTRESULT CLTMessage::WriteVector(LTVector &vec)
{
    const float v[3] = { vec.x, vec.y, vec.z };
    PutVector(v);
    return LT_OK;
}

LTRESULT CLTMessage::WriteCompVector(LTVector &vec) { return WriteVector(vec); }
LTRESULT CLTMessage::WriteCompPos(LTVector &vec)    { return WriteVector(vec); }

LTRESULT CLTMessage::WriteRotation(LTRotation &rot) { PutRotation(rot.m_Quat); return LT_OK; }
LTRESULT CLTMessage::WriteCompRotation(LTRotation &rot) { return WriteRotation(rot); }

LTRESULT CLTMessage::WriteRaw(void *pData, uint32 len) { PutRaw(pData, len); return LT_OK; }

LTRESULT CLTMessage::WriteMessage(ILTMessage &msg)
{
    PutMessage((const CLTMessage*)&msg);
    return LT_OK;
}

// ⚠️ A NULL HSTRING TRAVELS AS NULL, not as "". The donor writes length
// 0xFFFF for it and reads it back as LTNULL (ltmessage.cpp ReadHString); the
// game tests its strings for NULL, not for emptiness (`if (m_hstrSound)`). Sent
// as "", every unset sound, command and name came back from a saved game as a
// non-null empty string: 11,202 "Couldn't play sound ''" after one quick load
// of M01S01 (HANDOVER 5bz). One marker byte precedes the text: 0 = NULL, 1 =
// text follows. Every HSTRING writer and reader here uses it; nothing else in
// the engine writes strings the game reads as an HSTRING.
static const bool g_bOldHStr = []() {
    const bool b = (getenv("LT_OLDHSTRING") != 0);
    if (b) { printf("[MSG] LT_OLDHSTRING=1 -- a NULL HSTRING is sent as \"\" (old)\n"); fflush(stdout); }
    return b; }();

static void PutHStringText(CLTMessage* m, const char* p)
{
    if (g_bOldHStr) { m->PutString(p ? p : ""); return; }
    m->PutByte(p ? 1 : 0);
    if (p) m->PutString(p);
}

// Reads one HSTRING's text into buf; false = it was NULL (buf = "").
static bool TakeHStringText(CLTMessage* m, char* buf, unsigned int nMax)
{
    if (!g_bOldHStr && m->TakeByte() == 0) { if (nMax) buf[0] = '\0'; return false; }
    m->TakeString(buf, nMax);
    return true;
}

LTRESULT CLTMessage::WriteHString(HSTRING hString)
{
    // ⚠️ THE TEXT, NOT THE HANDLE. An HSTRING is reference-counted and owned by
    // whichever side made it; putting the handle in the buffer would hand the
    // receiver a pointer into the sender's table that the sender is free to
    // release before the pump runs.
    PutHStringText(this, hString ? LTGetStringMgr()->GetData(hString) : 0);
    return LT_OK;
}

LTRESULT CLTMessage::WriteHStringFormatted(int messageCode, ...)
{
    va_list args;
    va_start(args, messageCode);
    HSTRING h = LTGetStringMgr()->Format(messageCode, args);
    va_end(args);

    WriteHString(h);
    if (h) LTGetStringMgr()->Free(h);
    return LT_OK;
}

LTRESULT CLTMessage::WriteHStringArgList(int messageCode, va_list *pList)
{
    HSTRING h = pList ? LTGetStringMgr()->Format(messageCode, *pList) : 0;
    WriteHString(h);
    if (h) LTGetStringMgr()->Free(h);
    return LT_OK;
}

LTRESULT CLTMessage::WriteStringAsHString(char *pStr) { PutHStringText(this, pStr); return LT_OK; }

LTRESULT CLTMessage::WriteObject(HOBJECT hObj) { PutDWord((uintptr_t)hObj); return LT_OK; }

LTRESULT CLTMessage::ResetPos()  { m_nRead = 0; m_bOverran = false; return LT_OK; }

LTRESULT CLTMessage::GetStatus(uint32 &flags)
{
    flags = m_bOverran ? LMSTAT_READOVERFLOW : 0;
    return LT_OK;
}


// ----------------------------------------------------------------------- //
// ILTCSBase -- the obsolete API, one definition for both halves.
//
// ⚠️ EVERY ONE OF THESE TAKES A HANDLE THE GAME COULD HAVE GOT WRONG, so each
// one checks. A null HMESSAGEWRITE is not rare in this game: several call sites
// do `hWrite = StartMessage(...)` and write unconditionally, which is fine when
// the engine always succeeds and a crash when it does not.
// ----------------------------------------------------------------------- //

namespace
{
    inline CLTMessage* Msg(HMESSAGEWRITE h) { return (CLTMessage*)h; }
}

LTRESULT ILTCSBase::WriteToMessageFloat(HMESSAGEWRITE h, float val)
{ if (!h) return LT_ERROR; Msg(h)->PutFloat(val); return LT_OK; }

LTRESULT ILTCSBase::WriteToMessageByte(HMESSAGEWRITE h, uint8 val)
{ if (!h) return LT_ERROR; Msg(h)->PutByte(val); return LT_OK; }

LTRESULT ILTCSBase::WriteToMessageWord(HMESSAGEWRITE h, uint16 val)
{ if (!h) return LT_ERROR; Msg(h)->PutWord(val); return LT_OK; }

LTRESULT ILTCSBase::WriteToMessageDWord(HMESSAGEWRITE h, uintptr_t val)
{ if (!h) return LT_ERROR; Msg(h)->PutDWord(val); return LT_OK; }

LTRESULT ILTCSBase::WriteToMessageString(HMESSAGEWRITE h, char *pStr)
{ if (!h) return LT_ERROR; Msg(h)->PutString(pStr); return LT_OK; }

LTRESULT ILTCSBase::WriteToMessageVector(HMESSAGEWRITE h, LTVector *pVal)
{
    if (!h || !pVal) return LT_ERROR;
    const float v[3] = { pVal->x, pVal->y, pVal->z };
    Msg(h)->PutVector(v);
    return LT_OK;
}

LTRESULT ILTCSBase::WriteToMessageCompVector(HMESSAGEWRITE h, LTVector *pVal)
{ return WriteToMessageVector(h, pVal); }

LTRESULT ILTCSBase::WriteToMessageCompPosition(HMESSAGEWRITE h, LTVector *pVal)
{ return WriteToMessageVector(h, pVal); }

LTRESULT ILTCSBase::WriteToMessageRotation(HMESSAGEWRITE h, LTRotation *pVal)
{
    if (!h || !pVal) return LT_ERROR;
    Msg(h)->PutRotation(pVal->m_Quat);
    return LT_OK;
}

LTRESULT ILTCSBase::WriteToMessageHString(HMESSAGEWRITE h, HSTRING hString)
{ if (!h) return LT_ERROR; return Msg(h)->WriteHString(hString); }

LTRESULT ILTCSBase::WriteToMessageHMessageWrite(HMESSAGEWRITE h, HMESSAGEWRITE hData)
{ if (!h) return LT_ERROR; Msg(h)->PutMessage(Msg(hData)); return LT_OK; }

LTRESULT ILTCSBase::WriteToMessageHMessageRead(HMESSAGEWRITE h, HMESSAGEREAD hData)
{ if (!h) return LT_ERROR; Msg(h)->PutMessage(Msg(hData)); return LT_OK; }

LTRESULT ILTCSBase::WriteToMessageFormattedHString(HMESSAGEWRITE h, int messageCode, ...)
{
    if (!h) return LT_ERROR;
    va_list args;
    va_start(args, messageCode);
    HSTRING hs = LTGetStringMgr()->Format(messageCode, args);
    va_end(args);

    Msg(h)->WriteHString(hs);
    if (hs) LTGetStringMgr()->Free(hs);
    return LT_OK;
}

// ⚠️ THE OBJECT HANDLE GOES ACROSS AS ITSELF, AND THAT IS ONLY HALF RIGHT.
// The server's HOBJECT is a CLTServerObjects::Record*; the CLIENT's HLOCALOBJ
// is a client-side object, and this port has no server->client object
// replication yet, so a handle sent to the client cannot be resolved into one.
// Within the server -- object to object, which is where all 16 of the game's
// StartMessageToObject calls live -- it is exactly right.
//
// The read side is where the distinction is enforced: see the note on
// ReadFromMessageObject below.
LTRESULT ILTCSBase::WriteToMessageObject(HMESSAGEWRITE h, HOBJECT hObj)
{ if (!h) return LT_ERROR; Msg(h)->PutDWord((uintptr_t)hObj); return LT_OK; }

// ----------------------------------------------------------------------- //
// The save-reference context. See ltmessage.h for why a pointer cannot go in
// a file and what it cost when one did.
// ----------------------------------------------------------------------- //
namespace
{
    std::map<void*, unsigned int> g_SaveIndexOf;   // handle -> 1-based index
    std::vector<void*>            g_LoadHandles;   // index-1 -> handle
    bool g_bInSave = false;
    bool g_bInLoad = false;

    // ⚠️ COUNTED, because "the reference came back null" has two causes that
    // look identical to the game: the object genuinely was not in the save
    // set, or the context was missing and every reference in the file is
    // garbage. RestoreObjects reports these.
    unsigned int g_nRefsWritten = 0, g_nRefsNull = 0;
    std::vector<void*> g_RefsOutside;
    unsigned int g_nRefsRead = 0, g_nRefsUnresolved = 0;
    bool g_bWarnedNoContext = false;

    void NoContextOnce(const char* pWhich)
    {
        if (g_bWarnedNoContext) return;
        g_bWarnedNoContext = true;
        fprintf(stderr, "[MSG] ⚠️  %s called outside a save/load context -- "
                        "object references in this file are null\n", pWhich);
    }
}

void LTSaveRefs_BeginSave(void* const* pHandles, unsigned int nCount)
{
    g_SaveIndexOf.clear();
    for (unsigned int i = 0; i < nCount; ++i)
        if (pHandles[i]) g_SaveIndexOf[pHandles[i]] = i + 1;
    g_bInSave = true;
    g_bInLoad = false;
    g_nRefsWritten = g_nRefsNull = 0;
    g_RefsOutside.clear();
}

void LTSaveRefs_BeginLoad(void* const* pHandles, unsigned int nCount)
{
    g_LoadHandles.assign(pHandles, pHandles + nCount);
    g_bInLoad = true;
    g_bInSave = false;
    g_nRefsRead = g_nRefsUnresolved = 0;
}

void LTSaveRefs_End()
{
    g_bInSave = g_bInLoad = false;
    g_SaveIndexOf.clear();
    g_LoadHandles.clear();
}

unsigned int LTSaveRefs_Written()    { return g_nRefsWritten; }
unsigned int LTSaveRefs_NullWrites() { return g_nRefsNull; }
void* LTSaveRefs_OutsideAt(unsigned int i) { return i < g_RefsOutside.size() ? g_RefsOutside[i] : 0; }
unsigned int LTSaveRefs_Read()       { return g_nRefsRead; }
unsigned int LTSaveRefs_Unresolved() { return g_nRefsUnresolved; }

LTRESULT ILTCSBase::WriteToLoadSaveMessageObject(HMESSAGEWRITE h, HOBJECT hObject)
{
    if (!h) return LT_ERROR;

    if (!g_bInSave)
    {
        NoContextOnce("WriteToLoadSaveMessageObject");
        Msg(h)->PutDWord(0);
        return LT_OK;
    }

    unsigned int nIndex = 0;
    if (hObject)
    {
        std::map<void*, unsigned int>::const_iterator it =
            g_SaveIndexOf.find((void*)hObject);
        if (it != g_SaveIndexOf.end()) nIndex = it->second;
    }

    // ⚠️ A reference to an object OUTSIDE the save set becomes null, and that
    // is the honest answer: the object it names will not exist after the
    // restore. The game checks its handles -- CWeapons::ChangeWeapon tests
    // m_hWeaponModel before using it.
    if (hObject && !nIndex)
    {
        ++g_nRefsNull; g_RefsOutside.push_back((void*)hObject);
        // LT_SAVEREFTRACE=1 -- who wrote it: the game's Save function (5cu).
        static const bool s_bWho = (getenv("LT_SAVEREFTRACE") != 0);
        if (s_bWho)
        {
            void* frames[8];
            const int n = backtrace(frames, 8);
            printf("[SAVEREF] handle %p written at blob offset %u by:\n", (void*)hObject, (unsigned)Msg(h)->Size());
            fflush(stdout);
            backtrace_symbols_fd(frames + 1, n - 1, 1);
        }
    }
    ++g_nRefsWritten;

    Msg(h)->PutDWord(nIndex);
    return LT_OK;
}


float ILTCSBase::ReadFromMessageFloat(HMESSAGEREAD h)
{ return h ? Msg(h)->TakeFloat() : 0.0f; }

uint8 ILTCSBase::ReadFromMessageByte(HMESSAGEREAD h)
{ return h ? Msg(h)->TakeByte() : (uint8)0; }

uint16 ILTCSBase::ReadFromMessageWord(HMESSAGEREAD h)
{ return h ? Msg(h)->TakeWord() : (uint16)0; }

uintptr_t ILTCSBase::ReadFromMessageDWord(HMESSAGEREAD h)
{ return h ? Msg(h)->TakeDWord() : (uintptr_t)0; }

void ILTCSBase::ReadFromMessageVector(HMESSAGEREAD h, LTVector *pVal)
{
    if (!pVal) return;
    float v[3] = { 0.0f, 0.0f, 0.0f };
    if (h) Msg(h)->TakeVector(v);
    pVal->Init(v[0], v[1], v[2]);
}

void ILTCSBase::ReadFromMessageCompVector(HMESSAGEREAD h, LTVector *pVal)
{ ReadFromMessageVector(h, pVal); }

void ILTCSBase::ReadFromMessageCompPosition(HMESSAGEREAD h, LTVector *pVal)
{ ReadFromMessageVector(h, pVal); }

void ILTCSBase::ReadFromMessageRotation(HMESSAGEREAD h, LTRotation *pVal)
{
    if (!pVal) return;
    float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    if (h) Msg(h)->TakeRotation(q);
    pVal->Init(q[0], q[1], q[2], q[3]);
}

// ⚠️ ON THE CLIENT IT ANSWERS THE MIRROR -- AND FOR A LONG TIME IT ANSWERED
// NULL, after the reason for that had gone.
//
// Returning the server's pointer to the client would hand it something that
// looks like a valid HLOCALOBJ and is not. So this answered NULL "until object
// replication exists" -- and replication has existed since HANDOVER 5o. The
// comment went stale and the code with it: EVERY server->client message that
// names an object reached the client naming nothing. The one that showed it is
// MID_SFX_MESSAGE, which the client routes to the FX OF THAT OBJECT -- so a
// character's lip-sync and subtitle messages arrived and went to no one. In
// the Windows build Cate speaks with a subtitle; here she never did.
//
// ⚠️ FIND, NOT ENSURE. Only an object the client already mirrors is resolved;
// nothing new is created by reading a message, which keeps this a lookup and
// not a second, unaudited path into replication (the SFX gate in
// skeleton_host.cpp exists because opening that one crashed). An object with
// no mirror still answers NULL, exactly as before.
//
// LT_NOMSGOBJMAP=1 restores the old NULL, for an A/B. It announces itself.
namespace { LTMsgClientToServerFn g_pClientToServer = 0; }
void LTMessage_SetClientToServerObject(LTMsgClientToServerFn fn) { g_pClientToServer = fn; }

HOBJECT ILTCSBase::ReadFromMessageObject(HMESSAGEREAD h)
{
    if (!h) return (HOBJECT)0;

    const uintptr_t v = Msg(h)->TakeDWord();

    switch (Msg(h)->Dest())
    {
        // ⚠️ CLIENT -> SERVER: the value is a CLIENT handle. It used to be
        // returned as if it were a server one, so the fall damage the client
        // reports for its own object (CMoveMgr.cpp:916, CP_DAMAGE, DT_CRUSH)
        // went to whatever SERVER record lived at that address -- on M13S01 a
        // freed AIVolume, and the run died (HANDOVER 5bi). Cate never took
        // fall damage. The donor sends an object ID that both halves resolve.
        // LT_RAWCLIENTOBJ=1 = the old raw value (announces itself).
        case kLTMsg_ToServer:
        {
            static const bool bRaw = []() {
                const bool b = (getenv("LT_RAWCLIENTOBJ") != 0);
                if (b) { printf("[MSG] LT_RAWCLIENTOBJ=1 -- client-written object handles read raw on the server\n"); fflush(stdout); }
                return b; }();
            if (bRaw) return (HOBJECT)v;
            if (!v || !g_pClientToServer) return (HOBJECT)0;
            return (HOBJECT)g_pClientToServer((void*)v);
        }

        case kLTMsg_ToObject:
        case kLTMsg_ToShell:
        case kLTMsg_Data:
            return (HOBJECT)v;          // stays inside the server

        default:
        {
            // crossed to the client -- the client's MIRROR of that object
            static const bool bOff = (getenv("LT_NOMSGOBJMAP") != 0);
            static const bool bSaid = (bOff ? (printf("[MSG] LT_NOMSGOBJMAP=1 -- objects in "
                                                      "server->client messages read as NULL\n"),
                                               true) : true);
            (void)bSaid;
            if (bOff || !v) return (HOBJECT)0;
            return (HOBJECT)LTReplicate_Find((void*)v);
        }
    }
}

HSTRING ILTCSBase::ReadFromMessageHString(HMESSAGEREAD h)
{
    if (!h) return (HSTRING)0;
    char buf[1024];
    return TakeHStringText(Msg(h), buf, sizeof(buf)) ? LTGetStringMgr()->Create(buf) : (HSTRING)0;
}

LTRESULT ILTCSBase::ReadFromLoadSaveMessageObject(HMESSAGEREAD h, HOBJECT *hObject)
{
    // ⚠️ THE OUTPUT IS WRITTEN ON EVERY PATH, including the failures. The game
    // reads these into members it then uses -- an out-parameter left alone is
    // the single most expensive recurring bug class in this port, and this
    // function's own crash came from a handle nobody had set correctly.
    if (!hObject) return LT_ERROR;
    *hObject = (HOBJECT)0;
    if (!h) return LT_ERROR;

    const uintptr_t nIndex = Msg(h)->TakeDWord();

    if (!g_bInLoad)
    {
        NoContextOnce("ReadFromLoadSaveMessageObject");
        return LT_OK;
    }

    ++g_nRefsRead;

    if (nIndex == 0) return LT_OK;              // legitimately null

    if (nIndex > g_LoadHandles.size())
    {
        // ⚠️ Out of range means the file and the restore disagree about how
        // many objects there are. Null is the safe answer; silence is not.
        ++g_nRefsUnresolved;
        return LT_OK;
    }

    *hObject = (HOBJECT)g_LoadHandles[nIndex - 1];
    if (!*hObject) ++g_nRefsUnresolved;
    return LT_OK;
}

HMESSAGEREAD ILTCSBase::ReadFromMessageHMessageRead(HMESSAGEREAD h)
{ return h ? (HMESSAGEREAD)Msg(h)->TakeMessage() : (HMESSAGEREAD)0; }

void ILTCSBase::EndHMessageRead(HMESSAGEREAD h)  { LTMessage_Free(Msg(h)); }
void ILTCSBase::EndHMessageWrite(HMESSAGEWRITE h){ LTMessage_Free(Msg(h)); }
void ILTCSBase::ResetRead(HMESSAGEREAD h)        { if (h) Msg(h)->ResetPos(); }


// ----------------------------------------------------------------------- //
// The pipe
// ----------------------------------------------------------------------- //

namespace
{
    // ⚠️ WHICH MESSAGES, NOT JUST HOW MANY. "69 messages reached the client" is
    // a number; "69 of them were MID_SFX_MESSAGE and none was
    // MID_PLAYER_SINGLEPLAYER_INIT" is the next thing to implement. The tally
    // is keyed by route AND id because the same number means different things
    // in each direction.
    std::map<unsigned int, unsigned int> g_ById;     // (dest<<16)|id -> count

    // LT_MSGTRACE=1 prints every message as it is delivered. Off by default:
    // a level start is a few hundred lines, which is exactly what you want
    // once and never want again.
    bool Tracing()
    {
        static const bool b = (getenv("LT_MSGTRACE") != 0);
        return b;
    }

    LTMsgDeliverFn      g_pClientDeliver = 0;
    LTMsgDeliverFn      g_pServerDeliver = 0;

    std::deque<CLTMessage*> g_ToClient;      // server -> client shell
    std::deque<CLTMessage*> g_ToServer;      // client -> server shell, and
                                             // server object -> object/shell

    // ⚠️ A CAP, not a frame budget. The object queue is drained until empty so
    // a trigger chain completes in the frame it started -- but a pair of
    // objects that answer each other would drain forever, and a hung frame is
    // much harder to diagnose than a counted refusal.
    const unsigned int  kMaxDrain = 20000;

    // Immediate object-message delivery recurses; see LTMessage_End.
    int                 g_nDeliverDepth = 0;
    const int           kMaxDepth = 32;

    void Track(std::deque<CLTMessage*>& q)
    {
        const unsigned int n = (unsigned int)(g_ToClient.size() + g_ToServer.size());
        if (n > g_Stats.nPeakQueued) g_Stats.nPeakQueued = n;
        (void)q;
    }

    CLTMessage* Make(ELTMsgDest e, unsigned int nId)
    {
        ++g_Stats.nSent[e];
        return new CLTMessage(e, nId);
    }
}


namespace { bool Deliver(CLTMessage* pMsg, LTMsgDeliverFn fn); }

CLTMessage* LTMessage_StartData() { return Make(kLTMsg_Data, 0); }

CLTMessage* LTMessage_StartToClient(void* hClient, unsigned char nId)
{
    CLTMessage* p = Make(kLTMsg_ToClient, nId);
    p->m_hClient = hClient;
    return p;
}

CLTMessage* LTMessage_StartToServer(void* hClient, unsigned char nId)
{
    CLTMessage* p = Make(kLTMsg_ToServer, nId);
    p->m_hClient = hClient;
    return p;
}

CLTMessage* LTMessage_StartToObject(void* pSender, void* hSendTo, unsigned int nId)
{
    CLTMessage* p = Make(kLTMsg_ToObject, nId);
    p->m_pSender = pSender;
    p->m_hTarget = hSendTo;
    return p;
}

CLTMessage* LTMessage_StartToShell(void* pSender, unsigned int nId)
{
    CLTMessage* p = Make(kLTMsg_ToShell, nId);
    p->m_pSender = pSender;
    return p;
}

CLTMessage* LTMessage_StartSFX(void* pObject)
{
    CLTMessage* p = Make(kLTMsg_SFX, 0);
    p->m_pSFXObject = pObject;
    return p;
}

namespace { LTMsgSFXCaptureFn g_pSFXCapture = 0; }
void LTMessage_SetSFXCapture(LTMsgSFXCaptureFn fn) { g_pSFXCapture = fn; }

void LTMessage_QueueSFX(void* pObject, const unsigned char* pBytes, unsigned int nBytes)
{
    if (!pObject || !pBytes || !nBytes) return;
    CLTMessage* p = LTMessage_StartSFX(pObject);
    if (!p) return;
    p->PutRaw(pBytes, nBytes);
    p->ResetPos();
    p->m_bQueued = true;
    g_ToClient.push_back(p);
    Track(g_ToClient);
}

CLTMessage* LTMessage_StartInstantSFX(const float* pPos)
{
    CLTMessage* p = Make(kLTMsg_SFXInstant, 0);
    if (pPos) { p->m_SFXPos[0] = pPos[0]; p->m_SFXPos[1] = pPos[1]; p->m_SFXPos[2] = pPos[2]; }
    return p;
}


namespace { int g_nPlayerState = -1; unsigned int g_nPlayerDeaths = 0; }

void LTMessage_PlayerState(int* pState, unsigned int* pDeaths)
{
    if (pState)  *pState  = g_nPlayerState;
    if (pDeaths) *pDeaths = g_nPlayerDeaths;
}

LTRESULT LTMessage_End(CLTMessage* pMsg, unsigned int /*nFlags*/)
{
    // The player's HEALTH as the HUD is told it: MID_PLAYER_INFOCHANGE (103),
    // IC_HEALTH_ID (2), two pad bytes, the hit points (PlayerObj.cpp:2836).
    if (pMsg && pMsg->Dest() == kLTMsg_ToClient && pMsg->GetId() == 103 && pMsg->Size() >= 7
        && pMsg->Bytes()[0] == 2)
    {
        static const bool s_bSay = (getenv("LT_PLAYPROBE") != 0);
        if (s_bSay)
        {
            float f = 0.0f; memcpy(&f, pMsg->Bytes() + 3, sizeof(f));
            printf("[PROBE] player health %.1f\n", f);
            fflush(stdout);
        }
    }
    // The player-state watch (LT_PLAYPROBE). Observes only.
    if (pMsg && pMsg->Dest() == kLTMsg_ToClient && pMsg->GetId() == 105 && pMsg->Size() >= 1)
    {
        const int nNew = (int)pMsg->Bytes()[0];
        if (g_nPlayerState == 1 && (nNew == 2 || nNew == 3)) ++g_nPlayerDeaths;
        static const bool s_bSay = (getenv("LT_PLAYPROBE") != 0);
        if (s_bSay && nNew != g_nPlayerState)
        {
            static const char* kNames[] = { "UNKNOWN", "ALIVE", "DEAD", "DYING", "GHOST" };
            printf("[PROBE] player state %s -> %s\n",
                   (g_nPlayerState >= 0 && g_nPlayerState <= 4) ? kNames[g_nPlayerState] : "none",
                   (nNew >= 0 && nNew <= 4) ? kNames[nNew] : "?");
            fflush(stdout);
        }
        g_nPlayerState = nNew;
    }

    // ⚠️ THE FLAGS ARE DELIBERATELY IGNORED, and that is not the same as
    // unimplemented. MESSAGE_GUARANTEED, NAGGLE and NOTHROTTLE are all
    // statements about an unreliable link; in one process every message is
    // guaranteed, in order, with no bandwidth to throttle. Pretending to
    // honour them would be the more misleading choice.
    if (!pMsg) return LT_ERROR;

    // A data message was never going anywhere -- the game embeds it in another
    // and frees it itself. Ending one is a no-op, not a delivery.
    if (pMsg->Dest() == kLTMsg_Data) return LT_OK;

    pMsg->ResetPos();

    // The object keeps its effect message (see LTMessage_SetSFXCapture).
    if (pMsg->Dest() == kLTMsg_SFX && g_pSFXCapture && pMsg->m_pSFXObject)
        g_pSFXCapture(pMsg->m_pSFXObject, pMsg->Bytes(), pMsg->Size());

    switch (pMsg->Dest())
    {
        case kLTMsg_ToClient:
        case kLTMsg_SFX:
        case kLTMsg_SFXInstant:
            pMsg->m_bQueued = true;
            g_ToClient.push_back(pMsg);
            Track(g_ToClient);
            return LT_OK;

        case kLTMsg_ToServer:
            pMsg->m_bQueued = true;
            g_ToServer.push_back(pMsg);
            Track(g_ToServer);
            return LT_OK;

        // ⚠️ OBJECT MESSAGES ARE DELIVERED IMMEDIATELY, AND THIS IS NOT A
        // SHORTCUT -- QUEUEING THEM WAS A BUG, found by AddressSanitizer as a
        // stack-use-after-scope.
        //
        // The game passes RAW POINTERS through the message DWord field and
        // casts them back on the read side -- iltcsbase.h's own port note
        // counts 44 such readers, and KeyFramer is one of them:
        //
        //     const char* szMsg =
        //         (const char*)g_pLTServer->ReadFromMessageDWord(hRead);
        //     TriggerMsg(hSender, szMsg);      -- KeyFramer.cpp:1153
        //
        // The sender writes the address of a string it owns on its own stack.
        // That is only sound while the sender is still on the stack, so a
        // message delivered on the NEXT pump reads a dead frame. It crashed
        // intermittently -- sometimes the frame still held the old bytes --
        // which is exactly what makes this class of bug expensive.
        //
        // ⚠️ The original reasoning for queueing was about the CLIENT/SERVER
        // boundary, which really is a network hop and really is deferred.
        // Object to object is not a hop at all: it is one game object calling
        // another inside the same server frame. Applying the network rule to
        // it was reasoning by analogy where the analogy did not hold.
        default:
            break;
    }

    // ⚠️ A DEPTH CAP, because immediate delivery means a handler that sends a
    // message recurses into this function on the same stack. Two objects that
    // answer each other would otherwise run until the stack is gone -- and a
    // stack overflow is much harder to read than a counted refusal.
    if (g_nDeliverDepth >= kMaxDepth)
    {
        fprintf(stderr, "[MSG] ⚠️  object message nested %d deep (id %u) -- "
                        "refusing to recurse further\n",
                g_nDeliverDepth, pMsg->GetId());
        ++g_Stats.nDropped[pMsg->Dest()];
        LTMessage_Free(pMsg);
        return LT_ERROR;
    }

    ++g_nDeliverDepth;
    Deliver(pMsg, g_pServerDeliver);
    --g_nDeliverDepth;

    LTMessage_Free(pMsg);
    return LT_OK;
}


void LTMessage_Free(CLTMessage* pMsg)
{
    if (!pMsg) return;
    if (g_Stats.nLive) --g_Stats.nLive;
    delete pMsg;
}


void LTMessage_SetClientDeliver(LTMsgDeliverFn fn) { g_pClientDeliver = fn; }
void LTMessage_SetServerDeliver(LTMsgDeliverFn fn) { g_pServerDeliver = fn; }


namespace
{
    // Hand one message to its receiver and account for it. Returns whether it
    // was actually delivered.
    bool Deliver(CLTMessage* pMsg, LTMsgDeliverFn fn)
    {
        const ELTMsgDest e = pMsg->Dest();

        if (!fn) { ++g_Stats.nDropped[e]; return false; }

        // ⚠️ NOT QUEUED ANY MORE, and it has to be cleared BEFORE the handler
        // runs: the handler may call Release() on the message it was given, and
        // a message still marked queued would silently ignore that and leak.
        pMsg->m_bQueued = false;

        const bool bOk = fn(pMsg);
        if (bOk) ++g_Stats.nDelivered[e]; else ++g_Stats.nDropped[e];

        // ⚠️ A SPECIAL-EFFECT MESSAGE IS COUNTED BY ITS EFFECT TYPE, not by
        // its message id -- every one of them has id 0. See CLTMessage::SFXId.
        const unsigned int nKey =
            (e == kLTMsg_SFX || e == kLTMsg_SFXInstant) ? pMsg->SFXId()
                                                        : (pMsg->GetId() & 0xFFFF);
        ++g_ById[((unsigned int)e << 16) | nKey];

        // ⚠️ LT_TRIGTRACE=1 -- EVERY TRIGGER COMMAND, AS TEXT. MID_TRIGGER
        // (1000) carries a `const char*` in its DWord field -- the port made
        // that field uintptr_t and delivers object-to-object messages
        // immediately so the pointer stays valid -- so the engine can read the
        // command back and print it.
        //
        // ⚠️ AND A TRACE ON CreateString DOES NOT SEE THESE. Literal commands
        // ("SKIP", "OPEN") never become an HSTRING, so watching string
        // creation and concluding "the game never sends SKIP" is wrong -- an
        // hour was spent on exactly that mistake.
        // LT_TRIGTRACE=2: the server's delivery prints instead, with time,
        // sender and recipient (ltserverhost.cpp).
        if (nKey == 1000 && getenv("LT_TRIGTRACE") && strcmp(getenv("LT_TRIGTRACE"), "2") != 0
            && pMsg->Size() >= sizeof(uintptr_t))
        {
            uintptr_t v = 0;
            memcpy(&v, pMsg->Bytes(), sizeof(v));
            const char* pTxt = (const char*)v;
            if (pTxt)
            {
                printf("[TRIG] \"%s\"\n", pTxt);
                fflush(stdout);
            }
        }

        // ⚠️ LT_MSGTRACE="<id>[,<id>...]" -- the CONTENTS of a message, not
        // just that one went by. The census answers "was it sent"; a one-byte
        // payload like MID_PLAYER_STATE_CHANGE's player state is the whole
        // meaning of the message, and "sent once" is equally true of the state
        // that enables the player's weapon and the state that disables it.
        {
            static const char* pWant = getenv("LT_MSGTRACE");
            if (pWant)
            {
                bool bMatch = false;
                for (const char* p = pWant; *p; )
                {
                    while (*p == ' ' || *p == ',') ++p;
                    if (!*p) break;
                    if ((unsigned int)atoi(p) == nKey) { bMatch = true; break; }
                    while (*p && *p != ',') ++p;
                }
                if (bMatch)
                {
                    printf("[MSGT] %s id %u, %u byte(s):",
                           LTMessage_DestName(e), nKey, pMsg->Size());
                    const unsigned char* pB = pMsg->Bytes();
                    for (unsigned int i = 0; i < pMsg->Size() && i < 64; ++i)   // 64: a damage message is 37
                        printf(" %02x", pB[i]);
                    printf("\n");
                    fflush(stdout);
                }
            }
        }

        if (Tracing())
            printf("[MSG] %-16s id %-4u %4u bytes  %s\n",
                   LTMessage_DestName(e), nKey, pMsg->Size(),
                   bOk ? "delivered" : "DROPPED");
        return bOk;
    }
}


unsigned int LTMessage_PumpServer()
{
    unsigned int n = 0;

    // ⚠️ DRAINED UNTIL EMPTY, not "the messages that were there when we
    // started". A trigger that opens a door which fires another trigger has to
    // finish inside one frame; splitting a chain across frames turns a level's
    // scripted sequence into something that unfolds in slow motion.
    while (!g_ToServer.empty() && n < kMaxDrain)
    {
        CLTMessage* p = g_ToServer.front();
        g_ToServer.pop_front();
        Deliver(p, g_pServerDeliver);
        LTMessage_Free(p);
        ++n;
    }

    if (n >= kMaxDrain)
    {
        fprintf(stderr, "[MSG] ⚠️  server queue hit the %u drain cap -- a message "
                        "loop, %u still pending\n",
                kMaxDrain, (unsigned int)g_ToServer.size());
        LTRecord_Event("msg", "server drain cap hit");
    }
    return n;
}


unsigned int LTMessage_PumpClient()
{
    unsigned int n = 0;
    while (!g_ToClient.empty() && n < kMaxDrain)
    {
        CLTMessage* p = g_ToClient.front();
        g_ToClient.pop_front();
        Deliver(p, g_pClientDeliver);
        LTMessage_Free(p);
        ++n;
    }
    return n;
}


unsigned int LTMessage_PumpClientGameOnly()
{
    unsigned int n = 0;
    std::deque<CLTMessage*> kept;
    while (!g_ToClient.empty() && n < kMaxDrain)
    {
        CLTMessage* p = g_ToClient.front();
        g_ToClient.pop_front();
        if (p->Dest() == kLTMsg_SFX || p->Dest() == kLTMsg_SFXInstant) { kept.push_back(p); continue; }
        Deliver(p, g_pClientDeliver);
        LTMessage_Free(p);
        ++n;
    }
    // What a handler queued meanwhile goes AFTER the effects held back.
    while (!g_ToClient.empty()) { kept.push_back(g_ToClient.front()); g_ToClient.pop_front(); }
    g_ToClient.swap(kept);
    return n;
}


void LTMessage_Reset()
{
    while (!g_ToClient.empty()) { LTMessage_Free(g_ToClient.front()); g_ToClient.pop_front(); }
    while (!g_ToServer.empty()) { LTMessage_Free(g_ToServer.front()); g_ToServer.pop_front(); }
}


const LTMsgStats& LTMessage_Stats() { return g_Stats; }

const char* LTMessage_DestName(ELTMsgDest e)
{
    switch (e)
    {
        case kLTMsg_Data:       return "data";
        case kLTMsg_ToClient:   return "server->client";
        case kLTMsg_ToServer:   return "client->server";
        case kLTMsg_ToObject:   return "object->object";
        case kLTMsg_ToShell:    return "object->shell";
        case kLTMsg_SFX:        return "sfx (attached)";
        case kLTMsg_SFXInstant: return "sfx (instant)";
    }
    return "?";
}

void LTMessage_DumpStats()
{
    unsigned int nAny = 0;
    for (int i = 0; i < 7; ++i) nAny += g_Stats.nSent[i];
    if (!nAny) return;

    printf("\n[MSG] message census\n");
    printf("      %-16s %8s %10s %8s\n", "route", "sent", "delivered", "dropped");
    for (int i = 0; i < 7; ++i)
    {
        if (!g_Stats.nSent[i]) continue;
        printf("      %-16s %8u %10u %8u\n",
               LTMessage_DestName((ELTMsgDest)i),
               g_Stats.nSent[i], g_Stats.nDelivered[i], g_Stats.nDropped[i]);
    }
    printf("      peak queued %u, read overruns %u, still live %u\n",
           g_Stats.nPeakQueued, g_Stats.nOverruns, g_Stats.nLive);

    // ⚠️ EVERY ID, not a top-N. The interesting entry is nearly always a rare
    // one -- the single MID_PLAYER_SINGLEPLAYER_INIT that did or did not
    // arrive -- and a list truncated by frequency is guaranteed to hide it.
    if (!g_ById.empty())
    {
        printf("\n      by message id (see NOLF/Shared/MsgIDs.h; the sfx rows\n"
               "      are EFFECT ids from SFXMsgIds.h, not message ids)\n");
        for (std::map<unsigned int, unsigned int>::const_iterator i = g_ById.begin();
             i != g_ById.end(); ++i)
        {
            printf("      %-16s id %-4u  x%u\n",
                   LTMessage_DestName((ELTMsgDest)(i->first >> 16)),
                   i->first & 0xFFFF, i->second);
        }
    }
    fflush(stdout);
}
