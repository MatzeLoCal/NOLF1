// ----------------------------------------------------------------------- //
//
// MODULE  : ltserverobjects.cpp
//
// See ltserverobjects.h. The class table comes from the game, so almost
// nothing here is reverse engineered -- it is reading what ObjectDLL publishes.
//
// ----------------------------------------------------------------------- //

#include "windows.h"
#include "lithtech.h"

#include "ltserverobjects.h"
#include "ltserverhost.h"

#include "iltserver.h"
#include "ltserverobj.h"
#include "ltengineobjects.h"
#include "ltworld.h"
#include "ltworldvis.h"
#include "ltmathimpl.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <strings.h>

namespace
{
    typedef ClassDef** (*ObjectDLLSetupFn)(int*, ILTServer*, int*);

    bool EqualNoCase(const char* a, const char* b)
    {
        if (!a || !b) return false;
        while (*a && *b)
        {
            if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
            ++a; ++b;
        }
        return *a == *b;
    }
}


CLTServerObjects::CLTServerObjects()
    : m_pCurrentProps(0)
{
}


bool CLTServerObjects::Init(void* pLib, ILTServer* pServer)
{
    m_Classes.clear();
    if (!pLib) return false;

    ObjectDLLSetupFn pSetup = (ObjectDLLSetupFn)dlsym(pLib, "ObjectDLLSetup");
    if (!pSetup)
    {
        fprintf(stderr, "[SERVER] ObjectDLL exports no ObjectDLLSetup\n");
        return false;
    }

    int nDefs = 0, nVersion = 0;
    ClassDef** ppDefs = pSetup(&nDefs, pServer, &nVersion);

    // ⚠️ The version is the module's statement about the interface it was built
    // against. Mismatched, every ClassDef below is a different shape and every
    // field read from it is garbage -- so say so loudly rather than walking it.
    if (nVersion != SERVEROBJ_VERSION)
        fprintf(stderr, "[SERVER] ⚠️  ObjectDLL class version %d, expected %d\n",
                nVersion, SERVEROBJ_VERSION);

    if (!ppDefs || nDefs <= 0)
    {
        fprintf(stderr, "[SERVER] ObjectDLLSetup returned %d class definitions\n", nDefs);
        return false;
    }

    m_Classes.reserve((size_t)nDefs);
    for (int i = 0; i < nDefs; ++i)
        if (ppDefs[i] && ppDefs[i]->m_ClassName) m_Classes.push_back(ppDefs[i]);

    printf("[SERVER] ObjectDLL publishes %zu classes (version %d)\n",
           m_Classes.size(), nVersion);
    return !m_Classes.empty();
}


ClassDef* CLTServerObjects::FindClass(const char* pName) const
{
    if (!pName || !*pName) return 0;
    for (size_t i = 0; i < m_Classes.size(); ++i)
        if (EqualNoCase(m_Classes[i]->m_ClassName, pName)) return m_Classes[i];
    return 0;
}


BaseClass* CLTServerObjects::Create(ClassDef* pClass, ObjectCreateStruct* pStruct)
{
    if (!pClass) return 0;

    // ⚠️ CF_NOTINEDITOR / abstract classes still have a construct function, but
    // a size of zero means the macros never saw a complete type and calling the
    // constructor would write into a zero-byte allocation.
    if (pClass->m_ClassObjectSize <= 0 || !pClass->m_ConstructFn)
    {
        fprintf(stderr, "[SERVER] class %s is not constructible (size %ld, fn %p)\n",
                pClass->m_ClassName, (long)pClass->m_ClassObjectSize,
                (void*)pClass->m_ConstructFn);
        return 0;
    }

    // ⚠️ RAW BYTES AND PLACEMENT CONSTRUCTION, not `new`. The engine does not
    // know the C++ type -- only its size and a function that constructs into a
    // buffer. `calloc` rather than `malloc` deliberately: the game's classes
    // assume the engine zeroed the object (the original engine did), and an
    // uninitialised member here is the same family of bug that has cost this
    // port six separate crashes.
    void* pMem = calloc(1, (size_t)pClass->m_ClassObjectSize);
    if (!pMem) return 0;

    pClass->m_ConstructFn(pMem);

    Record* pRec = new Record();
    pRec->m_bDead = false;
    pRec->m_pGame = (BaseClass*)pMem;
    pRec->m_pClass = pClass;
    pRec->m_Flags = pRec->m_UserFlags = 0;
    pRec->m_Type = 0;
    pRec->m_fNextUpdate = 0.0f;
    pRec->m_Color[0] = pRec->m_Color[1] = pRec->m_Color[2] = pRec->m_Color[3] = 255;
    for (int k = 0; k < 3; ++k) { pRec->m_Pos[k] = 0.0f; pRec->m_Dims[k] = 0.0f; }
    pRec->m_Rot[0] = pRec->m_Rot[1] = pRec->m_Rot[2] = 0.0f; pRec->m_Rot[3] = 1.0f;

    // ⚠️ ZEROED HERE AND NOWHERE ELSE. An object that never gets a velocity
    // must read back exactly zero, because the game BRANCHES on it -- and an
    // uninitialised float that happens to be tiny makes an object drift for
    // reasons nobody can find. Same rule as every out-parameter in this port.
    for (int k = 0; k < 3; ++k) { pRec->m_Vel[k] = 0.0f; pRec->m_Accel[k] = 0.0f; }
    pRec->m_fMass = 1.0f;
    pRec->m_fFriction = 0.0f;
    pRec->m_fForceIgnoreLimit = 0.0f;
    pRec->m_bOnGround = false;

    // ⚠️ -1, NOT 0. "No animation chosen" and "animation zero" are different
    // states and zero is a real animation -- defaulting to it would make every
    // object in the level silently play its first animation.
    pRec->m_nAnim = -1;
    // ⚠️ LOOPING UNTIL TOLD OTHERWISE (HANDOVER 5cg) -- the donor's trk_Init
    // sets AT_LOOPING on every model's tracker, and NOLF relies on it: a Prop
    // plays `World` and never calls SetModelLooping, so a fish, a fly or a
    // turning globe would play once and freeze. Everything that waits for an
    // animation to END (BodyState, AnimationMgr, Character, Body) sets
    // looping off first. LT_NOLOOPDEFAULT=1 = not looping (old, says so).
    static const bool s_bNoLoop = []() {
        const bool b = (getenv("LT_NOLOOPDEFAULT") != 0);
        if (b) { printf("[SERVER] LT_NOLOOPDEFAULT=1 -- model animations do not loop until told (old)\n"); fflush(stdout); }
        return b; }();
    pRec->m_bAnimLooping = !s_bNoLoop;
    pRec->m_fAnimStart = 0.0f;
    pRec->m_bFromWorld = false;     // CreateWorldObjects sets it
    pRec->m_nContainerCode = 0;
    pRec->m_nWorldIndex = -1;
    pRec->m_bHasBox = false;
    for (int k = 0; k < 3; ++k)
        pRec->m_BoxMin[k] = pRec->m_BoxMax[k] = pRec->m_BoxRestPos[k] = 0.0f;
    pRec->m_bRestFromFile = false;
    pRec->m_RestRot[0] = pRec->m_RestRot[1] = pRec->m_RestRot[2] = 0.0f;
    pRec->m_RestRot[3] = 1.0f;
    // Awake, and never falling asleep until the game says when (Apply reads
    // the create struct's m_fDeactivationTime -- Intelligence sets 0.5).
    pRec->m_nInactive = 0;
    pRec->m_fDeactTime = pRec->m_fDeactTimer = 0.0f;
    pRec->m_bDeactPending = false;

    if (pStruct)
    {
        if (pStruct->m_Name[0]) pRec->m_sName = pStruct->m_Name;
        pRec->m_Pos[0] = pStruct->m_Pos.x;
        pRec->m_Pos[1] = pStruct->m_Pos.y;
        pRec->m_Pos[2] = pStruct->m_Pos.z;
        pRec->m_Flags  = pStruct->m_Flags;
        pRec->m_Type   = (unsigned char)pStruct->m_ObjectType;
    }

    m_Objects.push_back(pRec);
    m_Members.insert(pRec);

    // ⚠️ THE HANDLE THE GAME WILL CARRY. Everything it later asks the engine
    // about this object goes through m_hObject, so an object created without
    // one is invisible to every ILTServer call that follows.
    pRec->m_pGame->m_hObject = (HOBJECT)pRec;

    return pRec->m_pGame;
}


// ----------------------------------------------------------------------- //
//
// Apply / Destroy
//
// ⚠️ APPLY IS THE SECOND HALF OF CREATION, and it exists because the game gets
// to edit the create struct in MID_PRECREATE. The engine hands the struct over
// seeded from the world file, the game rewrites whatever it likes -- its name,
// its position, its model -- and only THEN is the object's engine-side state
// real. Reading the struct once, before precreate, is reading the level's
// intent and not the game's.
//
// ----------------------------------------------------------------------- //

void CLTServerObjects::Apply(Record* pRec, const ObjectCreateStruct* pStruct)
{
    if (!pRec || !pStruct) return;

    // ⚠️ An empty name CLEARS the record's name rather than keeping the old
    // one. A game that blanks m_Name is saying "I have no name"; leaving the
    // world file's name behind would make it findable under a name it has
    // deliberately given up.
    pRec->m_sName = pStruct->m_Name;

    pRec->m_Pos[0] = pStruct->m_Pos.x;
    pRec->m_Pos[1] = pStruct->m_Pos.y;
    pRec->m_Pos[2] = pStruct->m_Pos.z;

    for (int k = 0; k < 4; ++k) pRec->m_Rot[k] = pStruct->m_Rotation.m_Quat[k];

    pRec->m_Flags = pStruct->m_Flags;
    pRec->m_Type  = (unsigned char)pStruct->m_ObjectType;
    pRec->m_nContainerCode = pStruct->m_ContainerCode;

    // "Amount of time before object deactivates self" (ltbasedefs.h).
    pRec->m_fDeactTime = pRec->m_fDeactTimer = pStruct->m_fDeactivationTime;

    // ⚠️ The model and its skins, for the client's mirror. See Record.
    pRec->m_sFilename = pStruct->m_Filename;
    pRec->m_Skins.clear();
    for (int k = 0; k < MAX_MODEL_TEXTURES; ++k)
    {
        if (!pStruct->m_SkinNames[k][0]) continue;
        pRec->m_Skins.push_back(pStruct->m_SkinNames[k]);
    }
}


void CLTServerObjects::DumpClassCensus() const
{
    std::map<std::string, int> live;
    for (size_t i = 0; i < m_Objects.size(); ++i)
    {
        if (!m_Objects[i] || !m_Objects[i]->m_pClass) continue;
        const char* pName = m_Objects[i]->m_pClass->m_ClassName;
        if (pName) ++live[pName];
    }

    printf("\n[SERVER] objects by class -- live / MID_UPDATEs delivered\n");

    // ⚠️ A class with updates but no live objects is NOT a contradiction: the
    // object ran and was removed. Printing the union rather than one side is
    // what makes that visible instead of confusing.
    std::map<std::string, int> all;
    for (std::map<std::string, int>::const_iterator i = live.begin();
         i != live.end(); ++i) all[i->first] = 0;
    for (std::map<std::string, unsigned int>::const_iterator i = m_Updates.begin();
         i != m_Updates.end(); ++i) all[i->first] = 0;

    for (std::map<std::string, int>::const_iterator i = all.begin();
         i != all.end(); ++i)
    {
        std::map<std::string, int>::const_iterator l = live.find(i->first);
        std::map<std::string, unsigned int>::const_iterator u =
            m_Updates.find(i->first);
        const int nLive = (l == live.end()) ? 0 : l->second;
        const unsigned int nUpd = (u == m_Updates.end()) ? 0u : u->second;
        if (!nLive && !nUpd) continue;
        printf("      %-28s %5d   %8u\n", i->first.c_str(), nLive, nUpd);
    }
    fflush(stdout);
}


bool CLTServerObjects::IsLive(const Record* p) const
{
    // Membership first, THEN the flag: a pointer to a freed record is never
    // dereferenced. Same answer as the scan this replaced (see m_Members).
    // LT_SCANLIVE=1 = the scan (says so).
    static const bool s_bScan = []() {
        const bool b = getenv("LT_SCANLIVE") != 0;
        if (b) { printf("[SERVER] LT_SCANLIVE=1 -- IsLive scans the object list\n"); fflush(stdout); }
        return b; }();
    if (!p) return false;
    if (s_bScan)
    {
        for (size_t i = 0; i < m_Objects.size(); ++i)
            if (m_Objects[i] == p) return !p->m_bDead;
        return false;
    }
    return m_Members.count(p) != 0 && !p->m_bDead;
}


namespace
{
    std::map<const void*, std::string> g_Freed;
    std::vector<const void*>           g_FreedOrder;
}

// The freed-name history (LT_SAVEREFTRACE=1 keeps every name an address has
// had, newest first, so a stale handle can be traced to what it meant; 5cu).
static void NoteFreed(const CLTServerObjects::Record* pRec, const char* pWhy)
{
    static const bool s_bHist = (getenv("LT_SAVEREFTRACE") != 0);
    std::string sName = pRec->m_sName + " (" + ((pRec->m_pClass && pRec->m_pClass->m_ClassName)
                                               ? pRec->m_pClass->m_ClassName : "?") + ")";
    if (s_bHist)
    {
        char szT[64]; snprintf(szT, sizeof(szT), " @t=%.3f%s", LTServerHost_Time(), pWhy);
        sName += szT;
        std::map<const void*, std::string>::iterator it = g_Freed.find(pRec);
        if (it != g_Freed.end() && it->second.size() < 400) sName += " <- " + it->second;
    }
    g_Freed[pRec] = sName;
    g_FreedOrder.push_back(pRec);
    if (g_FreedOrder.size() > 4096)
    {
        const void* old = g_FreedOrder.front();
        g_FreedOrder.erase(g_FreedOrder.begin());
        if (std::find(g_FreedOrder.begin(), g_FreedOrder.end(), old) == g_FreedOrder.end())
            g_Freed.erase(old);
    }
}

std::string CLTServerObjects::FreedName(const void* p) const
{
    std::map<const void*, std::string>::const_iterator it = g_Freed.find(p);
    return it == g_Freed.end() ? std::string() : it->second;
}

bool CLTServerObjects::Contains(const Record* p) const
{
    // The membership set, not a scan of m_Objects: they are kept in step
    // (Create, Destroy, both clears) and GetObjectClass now asks this on every
    // call (5cf). Never dereferences p.
    return p && m_Members.count(p) != 0;
}


bool CLTServerObjects::MarkDead(Record* p)
{
    if (!p) return false;

    if (!m_Members.count(p)) return false;

    if (p->m_bDead) return true;    // asking twice is not an error

    p->m_bDead = true;

    // ⚠️ Stop scheduling it NOW rather than at the sweep. Between the mark and
    // the sweep the update loop may still reach this index, and an object that
    // has been removed must not get another MID_UPDATE.
    p->m_fNextUpdate = 0.0f;
    return true;
}


unsigned int CLTServerObjects::SweepDead()
{
    unsigned int nSwept = 0;

    // ⚠️ ONE PASS PER CALL, deliberately, and not a loop-until-empty. A
    // destructor can remove other objects; sweeping those in the SAME call
    // would run destructors from inside a destructor's stack frame, which is
    // the shape of re-entrancy this port has been bitten by before. They are
    // marked, and the next frame's sweep takes them.
    std::vector<Record*> dead;
    for (size_t i = 0; i < m_Objects.size(); ++i)
        if (m_Objects[i] && m_Objects[i]->m_bDead) dead.push_back(m_Objects[i]);

    // ⚠️ LINKS FIRST, FOR EVERY DEAD RECORD, BEFORE ANY IS DESTROYED -- an
    // owner told "your speaker is gone" may itself be in this list, and must
    // still exist to hear it. A handler that removes something else only
    // marks it; the next sweep takes it, links and all.
    static const bool bTrace = (getenv("LT_LIFETRACE") != 0);
    for (size_t i = 0; i < dead.size(); ++i)
    {
        Record* d = dead[i];
        for (size_t k = 0; k < m_Links.size(); )
        {
            if (m_Links[k].first != d && m_Links[k].second != d) { ++k; continue; }
            Record* pOwner  = m_Links[k].first;
            Record* pLinked = m_Links[k].second;
            m_Links.erase(m_Links.begin() + k);
            if (bTrace)
            {
                printf("[LIFE] link broken: %s is told %s is gone\n",
                       pOwner->m_sName.c_str(), pLinked->m_sName.c_str());
                fflush(stdout);
            }
            if (pOwner->m_pGame)
                pOwner->m_pGame->EngineMessageFn(MID_LINKBROKEN, (void*)pLinked, 0.0f);
            k = 0;      // the handler may have created or broken links
        }
    }

    for (size_t i = 0; i < dead.size(); ++i)
    {
        Destroy(dead[i]);
        ++nSwept;
    }
    return nSwept;
}


bool CLTServerObjects::CreateLink(Record* pOwner, Record* pLinked)
{
    if (!pOwner || !pLinked || pOwner == pLinked) return false;

    // ⚠️ A RECORD MARKED DEAD THIS FRAME CAN STILL BE LINKED (HANDOVER 5cf),
    // as in the donor (interlink.cpp CreateInterLink checks nothing): it is
    // still allocated, and SweepDead breaks its links -- with MID_LINKBROKEN
    // -- before freeing it. Refusing left a handle with no link to clear it:
    // M01S01's assassin kills the ambassador and the same bullet's AddImpact
    // links his CharFireInfo to the man RemoveObject has just marked; once
    // swept, every AI's HearEnemyWeaponImpact sense read the freed record
    // (IsCharacter -> GetObjectClass) and the server crashed -- the moment
    // the AI senses came on. Only a FREED record (not a member) is refused.
    // LT_OLDLINKLIVE=1 = live objects only (old, says so).
    static const bool s_bOld = []() {
        const bool b = (getenv("LT_OLDLINKLIVE") != 0);
        if (b) { printf("[SERVER] LT_OLDLINKLIVE=1 -- links only between live objects (old)\n"); fflush(stdout); }
        return b; }();
    if (s_bOld) { if (!IsLive(pOwner) || !IsLive(pLinked)) return false; }
    else if (!m_Members.count(pOwner) || !m_Members.count(pLinked)) return false;
    for (size_t k = 0; k < m_Links.size(); ++k)
        if (m_Links[k].first == pOwner && m_Links[k].second == pLinked) return true;
    m_Links.push_back(std::make_pair(pOwner, pLinked));
    return true;
}


void CLTServerObjects::BreakLink(Record* pOwner, Record* pLinked)
{
    for (size_t k = 0; k < m_Links.size(); ++k)
        if (m_Links[k].first == pOwner && m_Links[k].second == pLinked)
        { m_Links.erase(m_Links.begin() + k); return; }
}


void CLTServerObjects::Destroy(Record* pRec)
{
    if (!pRec) return;

    // ⚠️ Silently drop any link still naming it -- the allocator may hand the
    // same address to the next object, and a stale entry would then notify an
    // owner about an object it never linked.
    for (size_t k = 0; k < m_Links.size(); )
        if (m_Links[k].first == pRec || m_Links[k].second == pRec) m_Links.erase(m_Links.begin() + k);
        else ++k;

    for (size_t i = 0; i < m_Objects.size(); ++i)
    {
        if (m_Objects[i] != pRec) continue;
        m_Objects.erase(m_Objects.begin() + i);
        m_Members.erase(pRec);
        break;
    }

    if (pRec->m_nWorldIndex >= 0 && (size_t)pRec->m_nWorldIndex < m_ByWorldIndex.size()
        && m_ByWorldIndex[pRec->m_nWorldIndex] == pRec)
        m_ByWorldIndex[pRec->m_nWorldIndex] = 0;

    NoteFreed(pRec, "");

    if (pRec->m_pGame)
    {
        // ⚠️ THE CLASS'S OWN DESTRUCT FUNCTION, then free -- the mirror of the
        // placement-construct in Create. Calling `delete` would run the wrong
        // deallocation for memory that came from calloc, and skipping the
        // destructor would leak every HSTRING the object had taken.
        pRec->m_pGame->m_hObject = 0;
        if (pRec->m_pClass && pRec->m_pClass->m_DestructFn)
            pRec->m_pClass->m_DestructFn(pRec->m_pGame);
        free(pRec->m_pGame);
    }
    delete pRec;
}


BaseClass* CLTServerObjects::ObjectAt(unsigned int i) const
{
    return (i < m_Objects.size()) ? m_Objects[i]->m_pGame : 0;
}

CLTServerObjects::Record* CLTServerObjects::RecordAt(unsigned int i) const
{
    return (i < m_Objects.size()) ? m_Objects[i] : 0;
}

CLTServerObjects::Record* CLTServerObjects::RecordOf(const BaseClass* p) const
{
    for (size_t i = 0; i < m_Objects.size(); ++i)
        if (m_Objects[i]->m_pGame == p) return m_Objects[i];
    return 0;
}


ClassDef* CLTServerObjects::ClassOf(const BaseClass* p) const
{
    for (size_t i = 0; i < m_Objects.size(); ++i)
        if (m_Objects[i]->m_pGame == p) return m_Objects[i]->m_pClass;
    return 0;
}


void CLTServerObjects::FindByName(const char* pName, std::vector<BaseClass*>& out) const
{
    out.clear();
    if (!pName) return;
    for (size_t i = 0; i < m_Objects.size(); ++i)
        if (m_Objects[i]->m_pGame && EqualNoCase(m_Objects[i]->m_sName.c_str(), pName))
            out.push_back(m_Objects[i]->m_pGame);
}


// ⚠️ TWO PASSES: every game object is destructed (newest first) before ANY
// record is freed. A destructor reaches other objects -- ~Body removes its
// attachments (Body.cpp:106) -- and one already torn down in a single pass was
// already deleted: MarkDead read the freed record (ASan, world change on
// M05S04 -> M05S05, HANDOVER 5bi). Here it finds a live record marked dead,
// which is "removed twice", not an error.
void CLTServerObjects::DestroyAll()
{
    for (size_t i = m_Objects.size(); i-- > 0; )
    {
        Record* r = m_Objects[i];
        r->m_bDead = true;
        BaseClass* pGame = r->m_pGame;
        if (r->m_pClass && r->m_pClass->m_DestructFn)
            r->m_pClass->m_DestructFn(pGame);
        free(pGame);
        r->m_pGame = 0;
    }
    static const bool s_bHist = (getenv("LT_SAVEREFTRACE") != 0);
    for (size_t i = 0; i < m_Objects.size(); ++i)
    {
        if (s_bHist) NoteFreed(m_Objects[i], " (world change)");
        delete m_Objects[i];
    }
}


void CLTServerObjects::Term()
{
    // ⚠️ NEWEST FIRST. Objects hold references to ones created before them --
    // a player to its start point, an attachment to its owner -- so tearing
    // down in creation order destroys the thing still being pointed at.
    DestroyAll();
    m_Objects.clear();
    m_Members.clear();
    m_Links.clear();
    m_ByWorldIndex.clear();
    m_WorldMade.clear();
    m_Classes.clear();
}


unsigned int CLTServerObjects::ClearObjects()
{
    const unsigned int n = (unsigned int)m_Objects.size();

    // ⚠️ NEWEST FIRST, for the reason Term gives: objects hold references to
    // ones created before them, so creation order destroys the thing still
    // being pointed at.
    DestroyAll();
    m_Objects.clear();
    m_Members.clear();
    m_Links.clear();
    m_ByWorldIndex.clear();
    m_WorldMade.clear();

    // ⚠️ THE PROPERTY CONTEXT POINTS INTO THE OLD WORLD'S object array, which
    // the caller is about to replace. Left set, the next object created would
    // read the previous LEVEL's properties -- and find plausible values rather
    // than failing, which is the worst way for this to go wrong.
    m_pCurrentProps = 0;

    // ⚠️ The class table is deliberately KEPT: it comes from ObjectDLL and
    // describes the game, not the level.
    return n;
}


// ----------------------------------------------------------------------- //
//
// ReadProp
//
// ⚠️ The world file's property types and GenericProp's fields do not line up
// one to one, so every branch here is a decision. LT_PT_ROTATION is EULER
// RADIANS plus a pad float in this format (FORMATS.md) even though LTRotation
// is a quaternion everywhere else, and LT_PT_FLAGS / LT_PT_LONGINT are stored
// as FLOATS and cast -- the editor has no integer property type.
//
// ⚠️ EVERY field of *pOut is filled on success, not just the one that matches
// the type. The game reads whichever field it feels like: `GetPropGeneric`
// hands back one struct and the caller picks. Filling only the "right" one
// leaves the others as whatever the caller's stack held -- which is the exact
// bug family that has cost this port six crashes.
//
// ----------------------------------------------------------------------- //

// ⚠️ A DELIBERATE DEVIATION, ASKED FOR BY THE USER (2026-10-10): THE M01S02
// HOSTAGE GUARDS SHOOT LATER THAN THE LEVEL SAYS. DEVIATIONS.md E-007,
// HANDOVER 5bk.
//
// The level's fourteen `hostagetrigger`s (M01S02, Trigger, touched at the
// doorways) run e.g. "msg senemy1a aware;msg senemy1b aware;msg senemy1a
// (target hostage1a);msg senemy1a attack". Measured, that gives the guard's
// first shot 0.85 s after Cate crosses the doorway and the hostage ~5-6 s to
// live -- faithful, and too short for the user. The engine hands the game the
// same script with the guard's `target` and `attack` delayed together, so
// the guards still turn aware and the hostage still screams at once; only the
// execution starts later:
//   "...;delay 5 (msg senemy1a (target hostage1a));delay 5 (msg senemy1a attack)"
// ⚠️ TWO delays, not one holding both: ConParse splits at EVERY ';', inside
// parentheses too, so "delay 3 (a;b)" runs b at once (measured: `attack` at
// the touch, `target` 3 s later). Equal delays keep the order: CCommandMgr
// gives each the first free slot and its Update walks slots upward, so the
// target (added first, lower slot) lands in the same frame, before the
// attack. An existing `delay T (msg X attack)` becomes T+N for both. Nothing else
// is touched: a script without a target+attack pair for one guard (M04S02's
// own `hostagetrigger` attacks props) is returned unchanged.
//
// LT_HOSTAGEDELAY=<seconds> sets N (default 5, user 2026-10-10); 0 hands over the level's
// script as authored (says so). Each rewrite is printed once.
namespace
{
    std::string TrimCmd(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && isspace((unsigned char)s[a])) ++a;
        while (b > a && isspace((unsigned char)s[b - 1])) --b;
        return s.substr(a, b - a);
    }

    // Top-level ';' only -- parentheses nest, as ConParse treats them.
    void SplitCmds(const std::string& s, std::vector<std::string>& out)
    {
        out.clear();
        int nDepth = 0;
        std::string cur;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const char c = s[i];
            if (c == '(') ++nDepth;
            else if (c == ')' && nDepth > 0) --nDepth;
            if (c == ';' && nDepth == 0) { out.push_back(TrimCmd(cur)); cur.clear(); }
            else cur += c;
        }
        if (!TrimCmd(cur).empty()) out.push_back(TrimCmd(cur));
    }

    // "msg <obj> <arg>" -> obj, arg (one level of surrounding parentheses off).
    bool ParseMsgCmd(const std::string& c, std::string& sObj, std::string& sArg)
    {
        if (c.size() < 5 || strncasecmp(c.c_str(), "msg ", 4) != 0) return false;
        const std::string r = TrimCmd(c.substr(4));
        const size_t sp = r.find(' ');
        if (sp == std::string::npos) return false;
        sObj = r.substr(0, sp);
        sArg = TrimCmd(r.substr(sp + 1));
        if (sArg.size() >= 2 && sArg[0] == '(' && sArg[sArg.size() - 1] == ')')
            sArg = TrimCmd(sArg.substr(1, sArg.size() - 2));
        return true;
    }

    // "delay <t> (<inner>)" -> t, inner.
    bool ParseDelayCmd(const std::string& c, float& fT, std::string& sInner)
    {
        if (c.size() < 7 || strncasecmp(c.c_str(), "delay ", 6) != 0) return false;
        const std::string r = TrimCmd(c.substr(6));
        const size_t sp = r.find(' ');
        if (sp == std::string::npos) return false;
        fT = (float)atof(r.substr(0, sp).c_str());
        sInner = TrimCmd(r.substr(sp + 1));
        if (sInner.size() < 2 || sInner[0] != '(' || sInner[sInner.size() - 1] != ')') return false;
        sInner = TrimCmd(sInner.substr(1, sInner.size() - 2));
        return true;
    }

    // The rewrite itself. Returns false (and leaves sOut alone) when the
    // script holds no target+attack pair for any one guard.
    bool DelayHostageExecution(const std::string& sIn, float fDelay, std::string& sOut)
    {
        std::vector<std::string> cmds;
        SplitCmds(sIn, cmds);

        std::map<std::string, size_t> target;               // guard -> index
        std::map<std::string, std::pair<size_t, float> > attack;  // guard -> index, delay
        for (size_t i = 0; i < cmds.size(); ++i)
        {
            std::string sObj, sArg, sInner;
            float fT = 0.0f;
            if (ParseMsgCmd(cmds[i], sObj, sArg))
            {
                if (strncasecmp(sArg.c_str(), "target ", 7) == 0) target[sObj] = i;
                else if (strcasecmp(sArg.c_str(), "attack") == 0) attack[sObj] = std::make_pair(i, 0.0f);
            }
            else if (ParseDelayCmd(cmds[i], fT, sInner) && ParseMsgCmd(sInner, sObj, sArg)
                     && strcasecmp(sArg.c_str(), "attack") == 0)
                attack[sObj] = std::make_pair(i, fT);
        }

        std::vector<bool> bDrop(cmds.size(), false);
        bool bAny = false;
        for (std::map<std::string, std::pair<size_t, float> >::const_iterator it = attack.begin();
             it != attack.end(); ++it)
        {
            std::map<std::string, size_t>::const_iterator t = target.find(it->first);
            if (t == target.end()) continue;
            char szT[32];
            snprintf(szT, sizeof szT, "%g", it->second.second + fDelay);
            cmds[it->second.first] = std::string("delay ") + szT + " (" + cmds[t->second] + ");"
                                   + "delay " + szT + " (msg " + it->first + " attack)";
            bDrop[t->second] = true;
            bAny = true;
        }
        if (!bAny) return false;

        sOut.clear();
        for (size_t i = 0; i < cmds.size(); ++i)
        {
            if (bDrop[i]) continue;
            if (!sOut.empty()) sOut += ";";
            sOut += cmds[i];
        }
        return true;
    }
}

bool CLTServerObjects::ReadProp(const char* pName, GenericProp* pOut) const
{
    if (!m_pCurrentProps || !pName || !pOut) return false;

    const LTWorldProperty* p = m_pCurrentProps->Find(pName);
    if (!p) return false;

    pOut->m_Vec.Init(p->m_Vec[0], p->m_Vec[1], p->m_Vec[2]);
    pOut->m_Color.Init(p->m_Vec[0], p->m_Vec[1], p->m_Vec[2]);
    // ⚠️ A QUATERNION, NOT THE ANGLES. The world file stores Euler radians,
    // but every game use of GenericProp::m_Rotation treats it as an LTRotation
    // -- Door.cpp:151 assigns it, KeyFramer keys store it, ClientLightFX feeds
    // it to GetRotationVectors. This used to pack the raw angles in as
    // (pitch, yaw, roll, 0), which is not a rotation at all: its length is the
    // yaw. Measured on T01S02's characters, |q| came out 0, pi and 3pi/2 --
    // exact yaws -- and a non-unit quaternion SHEARS a model, which is how the
    // receptionist became a flat disc.
    //
    // The angles themselves are still available, through ReadPropEuler, for
    // GetPropRotationEuler.
    {
        float q[4];
        LTMathImpl::SetupEuler(q, p->m_Euler[0], p->m_Euler[1], p->m_Euler[2]);
        pOut->m_Rotation.Init(q[0], q[1], q[2], q[3]);
    }
    pOut->m_Long  = p->m_nInt;
    pOut->m_Float = p->m_fReal;
    pOut->m_Bool  = p->m_bBool ? LTTRUE : LTFALSE;

    strncpy(pOut->m_String, p->m_sString.c_str(), MAX_GP_STRING_LEN);
    pOut->m_String[MAX_GP_STRING_LEN] = '\0';

    // ⚠️ EVERY FIELD FROM THE ONE VALUE, AS LT2 FILLED IT. The reference's
    // GenericProp constructors (ltproperty.h:76-130, "emulates the old
    // behavior of GetPropGeneric") fill each representation from the stored
    // one: a STRING gives atof/atoi/T-or-Y/"%f %f %f"; a number gives its
    // "%f"/"%d" text. NOLF's AI overrides are STRING properties read as
    // numbers -- `if (genProp.m_String[0]) m_fSoundRadius = genProp.m_Float`
    // (AI.cpp:692, likewise Awareness, HitPoints, Armor) -- so a level's
    // `SoundRadius "900"` reached the game as 0, the client refused the
    // character's lip-synced lines (NodeController.cpp:1047, fRadius <= 0) and
    // M01S01's locked-out man and courtyard pair never spoke (user report
    // 2026-10-17). CreateObjectProps already filled its string properties this
    // way. LT_OLDPROPGENERIC=1 = each field from its own type only (says so).
    static const bool s_bOld = []() {
        const bool b = (getenv("LT_OLDPROPGENERIC") != 0);
        if (b) { printf("[SRVOBJ] LT_OLDPROPGENERIC=1 -- GetPropGeneric fills only the stored type's field (old)\n"); fflush(stdout); }
        return b; }();
    if (!s_bOld)
    {
        switch (p->m_nType)
        {
            case kLTWP_String:
            {
                const char* v = p->m_sString.c_str();
                float f3[3] = { 0.0f, 0.0f, 0.0f };
                sscanf(v, "%f %f %f", &f3[0], &f3[1], &f3[2]);
                pOut->m_Vec.Init(f3[0], f3[1], f3[2]);
                pOut->m_Color = pOut->m_Vec;
                pOut->m_Long  = (toupper((unsigned char)*v) == 'T' || toupper((unsigned char)*v) == 'Y') ? 1 : atoi(v);
                pOut->m_Float = (float)atof(v);
                pOut->m_Bool  = (pOut->m_Long != 0) ? LTTRUE : LTFALSE;
                break;
            }
            case kLTWP_Vector:
            case kLTWP_Color:
                pOut->m_Long  = (int32)p->m_Vec[0];
                pOut->m_Float = p->m_Vec[0];
                pOut->m_Bool  = (p->m_Vec[0] != 0.0f) ? LTTRUE : LTFALSE;
                snprintf(pOut->m_String, MAX_GP_STRING_LEN + 1, "%f %f %f", p->m_Vec[0], p->m_Vec[1], p->m_Vec[2]);
                break;
            case kLTWP_Real:
                pOut->m_Long  = (int32)p->m_fReal;
                pOut->m_Float = p->m_fReal;
                pOut->m_Bool  = (p->m_fReal != 0.0f) ? LTTRUE : LTFALSE;
                snprintf(pOut->m_String, MAX_GP_STRING_LEN + 1, "%f", p->m_fReal);
                break;
            case kLTWP_Flags:
            case kLTWP_LongInt:
                pOut->m_Long  = p->m_nInt;
                pOut->m_Float = (float)p->m_nInt;
                pOut->m_Bool  = (p->m_nInt != 0) ? LTTRUE : LTFALSE;
                snprintf(pOut->m_String, MAX_GP_STRING_LEN + 1, "%d", p->m_nInt);
                break;
            case kLTWP_Bool:
                pOut->m_Long  = p->m_bBool ? 1 : 0;
                pOut->m_Float = p->m_bBool ? 1.0f : 0.0f;
                pOut->m_Bool  = p->m_bBool ? LTTRUE : LTFALSE;
                pOut->m_String[0] = p->m_bBool ? '1' : '0';
                pOut->m_String[1] = '\0';
                break;
            case kLTWP_Rotation:
                // The server-side reference also hands the EULER angles in
                // m_Vec (ltproperty.h's note on the rotation constructor).
                pOut->m_Vec.Init(p->m_Euler[0], p->m_Euler[1], p->m_Euler[2]);
                pOut->m_Long  = 0;
                pOut->m_Float = 0.0f;
                pOut->m_Bool  = LTFALSE;
                break;
            default:
                break;
        }
    }

    // The hostage guards' delay (see DelayHostageExecution above).
    if (strncasecmp(pName, "MessageName", 11) == 0
        && strcasecmp(m_pCurrentProps->m_sType.c_str(), "Trigger") == 0
        && strcasecmp(m_pCurrentProps->GetName(), "hostagetrigger") == 0)
    {
        static const float s_fDelay = []() {
            const char* e = getenv("LT_HOSTAGEDELAY");
            const float f = e ? (float)atof(e) : 5.0f;
            if (f <= 0.0f) { printf("[HOSTAGE] LT_HOSTAGEDELAY=0 -- hostage triggers as the level wrote them\n"); fflush(stdout); }
            return f; }();
        std::string sNew;
        if (s_fDelay > 0.0f && DelayHostageExecution(p->m_sString, s_fDelay, sNew)
            && sNew.size() <= MAX_GP_STRING_LEN)
        {
            strncpy(pOut->m_String, sNew.c_str(), MAX_GP_STRING_LEN);
            pOut->m_String[MAX_GP_STRING_LEN] = '\0';
            static std::map<const void*, int> s_Said;
            if (s_Said[(const void*)p]++ == 0)
            {
                printf("[HOSTAGE] %s %s: guard's shooting delayed %g s (LT_HOSTAGEDELAY=0 = as authored)\n"
                       "          \"%s\"\n", m_pCurrentProps->GetName(), pName, s_fDelay, sNew.c_str());
                fflush(stdout);
            }
        }
    }

    return true;
}


bool CLTServerObjects::ReadPropEuler(const char* pName, float* pAngles3) const
{
    if (!m_pCurrentProps || !pName || !pAngles3) return false;
    const LTWorldProperty* p = m_pCurrentProps->Find(pName);
    if (!p) return false;
    for (int k = 0; k < 3; ++k) pAngles3[k] = p->m_Euler[k];
    return true;
}


// ----------------------------------------------------------------------- //
//
// CreateWorldObjects
//
// ----------------------------------------------------------------------- //

unsigned int CLTServerObjects::CreateWorldObjects(const CLTWorld& world, int* pnSkipped)
{
    unsigned int nMade = 0;
    int nSkipped = 0;
    int nDeclined = 0;

    const std::vector<LTWorldObject>& obj = world.GetObjects();
    for (size_t i = 0; i < obj.size(); ++i)
    {
        ClassDef* pClass = FindClass(obj[i].m_sType.c_str());
        if (!pClass)
        {
            // ⚠️ Named, not just counted. "one object was skipped" is not
            // something anyone can act on; a class name is.
            fprintf(stderr, "[SRVOBJ] no class \"%s\" (object \"%s\") -- skipped\n",
                    obj[i].m_sType.c_str(), obj[i].GetName() ? obj[i].GetName() : "?");
            ++nSkipped;
            continue;
        }

        ObjectCreateStruct cs;
        cs.Clear();

        float pos[3] = { 0, 0, 0 };
        if (obj[i].GetPos(pos)) cs.m_Pos.Init(pos[0], pos[1], pos[2]);

        // ⚠️ THE ROTATION TOO, and it needs converting. The world file stores
        // EULER RADIANS (FORMATS.md) while ObjectCreateStruct wants the
        // quaternion everything else uses. Seeding only the position is why
        // every world object faced down +Z regardless of how the level was
        // authored.
        float eul[3] = { 0, 0, 0 };
        if (obj[i].GetEuler(eul))
        {
            float q[4];
            LTServerObjects_EulerToQuat(eul, q);
            cs.m_Rotation.Init(q[0], q[1], q[2], q[3]);
        }

        const char* pName = obj[i].GetName();
        if (pName) { strncpy(cs.m_Name, pName, MAX_CS_FILENAME_LEN); cs.m_Name[MAX_CS_FILENAME_LEN] = '\0'; }

        // ⚠️ THE CONTEXT IS SET BEFORE THE OBJECT EXISTS AND CLEARED AFTER,
        // because the game reads its properties from inside MID_PRECREATE --
        // i.e. during the call below, not before or after it.
        m_pCurrentProps = &obj[i];

        // ⚠️ stderr and UNBUFFERED, so the last line printed is the object
        // that crashed rather than whatever the buffer happened to hold. The
        // whole value of this trace is that it survives a segfault.
        static const bool bTrace = (getenv("LT_TRACE_SERVER") != 0);
        if (bTrace)
            fprintf(stderr, "[SRVOBJ] %4zu %-24s %s\n", i,
                    obj[i].m_sType.c_str(), pName ? pName : "");

        // ⚠️ EVERY PROPERTY THE WORLD ACTUALLY CARRIES, for one class.
        // LT_PROPDUMP=GameStartPoint. The game reads its level data by NAME
        // and treats a miss as "use my default", so a property that is absent
        // and one that is present-but-empty produce completely different
        // objects and look identical from the outside. This is how the player
        // was ending up with an INVALID model style: GameStartPoint leaves
        // m_ePlayerModelStyle at 0xFF when "PlayerModel" is not found, and
        // 0xFF then indexes past the end of the style array.
        {
            static const char* pDump = getenv("LT_PROPDUMP");
            if (pDump && *pDump && !strcasecmp(pDump, obj[i].m_sType.c_str()))
            {
                fprintf(stderr, "[PROPDUMP] %s \"%s\" -- %zu propert%s\n",
                        obj[i].m_sType.c_str(), pName ? pName : "",
                        obj[i].m_Props.size(),
                        obj[i].m_Props.size() == 1 ? "y" : "ies");
                for (size_t k = 0; k < obj[i].m_Props.size(); ++k)
                {
                    const LTWorldProperty& pr = obj[i].m_Props[k];
                    fprintf(stderr, "           %-28s type %-3u  \"%s\"  "
                                    "real %g  int %d  bool %d  vec (%g %g %g)\n",
                            pr.m_sName.c_str(), (unsigned)pr.m_nType,
                            pr.m_sString.c_str(), pr.m_fReal, pr.m_nInt,
                            pr.m_bBool ? 1 : 0,
                            pr.m_Vec[0], pr.m_Vec[1], pr.m_Vec[2]);
                }
            }
        }

        BaseClass* pObj = Create(pClass, &cs);
        if (pObj)
        {
            // ⚠️ THE RECORD, TAKEN NOW, AND NOT `m_Objects.back()` LATER.
            //
            // MID_PRECREATE below can CREATE MORE OBJECTS -- the game does it
            // routinely: `CinematicTrigger::CreateKeyFramer` and
            // `::CreateCamera` both build objects while the trigger is still
            // precreating. Each of those is pushed onto m_Objects, so by the
            // time precreate returns, `back()` is the object the GAME made,
            // not this one.
            //
            // ⚠️ WHAT THAT COST: cinema2's create struct -- its name, its
            // position, its flags -- was applied to cinema2's KEYFRAMER. The
            // keyframer was renamed from `cinema2_KF1` to `cinema2`, so
            // nothing could find it by name; and the trigger itself never got
            // its own struct applied at all. The visible symptom was an
            // opening cinematic that never ended and a player frozen for the
            // whole level (HANDOVER §5x).
            //
            // Records are heap-allocated and never moved (see Record), so this
            // pointer stays valid across the nested creations.
            Record* pMine = RecordOf(pObj);
            if (pMine) pMine->m_bFromWorld = true;   // see Record::m_bFromWorld
            if (pMine)
            {
                pMine->m_nWorldIndex = (int)i;
                const LTWorldProperty* pWF = obj[i].Find("Filename");
                pMine->m_sWorldModel = (pWF && pWF->m_nType == 0) ? pWF->m_sString : std::string();
                if (m_ByWorldIndex.size() < obj.size()) { m_ByWorldIndex.resize(obj.size(), 0); m_WorldMade.resize(obj.size(), 0); }
                m_ByWorldIndex[i] = pMine;
                m_WorldMade[i] = 1;
            }
            // ⚠️ PRECREATE_WORLDFILE, NOT 0.0f -- AND THIS WAS THE BUG THAT
            // EMPTIED EVERY LEVEL. `fData` is not a spare parameter: it says
            // WHY the object is being created, and 0.0f is PRECREATE_NORMAL,
            // "created at runtime" (ltserverobj.h:85). Objects gate their
            // property reading on it:
            //
            //     if (fData == PRECREATE_WORLDFILE) ReadProp(pStruct);
            //                                    -- GameStartPoint.cpp:110
            //
            // So with 0.0f the game politely declined to read the level data
            // the engine was holding out to it, and every such object was
            // constructed with its constructor's defaults. Nothing failed;
            // 731 objects were created and none of them knew where it was.
            const uint32 nPre = pObj->EngineMessageFn(MID_PRECREATE, &cs,
                                                      PRECREATE_WORLDFILE);

            // ⚠️ AN OBJECT MAY DECLINE TO EXIST, and the return value is how
            // it says so -- a difficulty-gated pickup or a multiplayer-only
            // spawner returns 0 and expects the engine to drop it. Ignoring
            // that leaves a level full of objects the game asked not to have.
            //
            // ⚠️ NO CF_CLASSONLY EXEMPTION, because LT2 HAS NO SUCH FLAG. The
            // later engine spares class-only definitions from this check; this
            // one's flags are CF_HIDDEN, CF_NORUNTIME, CF_STATIC and
            // CF_ALWAYSLOAD (ltserverobj.h:72-80) and none of them means that.
            // Borrowing the newer engine's condition would have added a branch
            // that is never taken and reads as though it were.
            if (!nPre)
            {
                Destroy(pMine);
                ++nDeclined;
                m_pCurrentProps = 0;
                continue;
            }

            // ⚠️ THE STRUCT IS RE-READ AFTER PRECREATE, because the game
            // REWRITES IT there. GameStartPoint.cpp:114 is the case that
            // matters most:
            //
            //     SAFE_STRCPY(pStruct->m_Name, "GameStartPoint");
            //
            // The world names that object "GameStartPoint0"; the game renames
            // it to "GameStartPoint" and then looks it up by that name with
            // FindNamedObjects. Taking the name before precreate -- which is
            // what this did -- means the lookup finds nothing, FindStartPoint
            // returns null, and the player is never given a spawn, a model
            // style or a position. That one ordering mistake is the whole
            // "the spawn point is not correct" report.
            Apply(pMine, &cs);

            // ⚠️ A WORLD MODEL IS MOVED TO THE CENTRE OF ITS GEOMETRY AT
            // CREATION, and takes its box -- the donor's se_InitWorldModel
            // (server_extradata.cpp: SetPos(m_WorldTranslation), dims = half
            // the BSP box). The authored `Pos` is an arbitrary editor point
            // (an AIVolume's is a CORNER), and the game is written against the
            // engine's answer: CAIVolume::Inside() tests position +- dims, so
            // with the authored point and zero dims no AI was ever inside any
            // volume and every GOTO in M01S01 went nowhere (HANDOVER 5bb).
            // Before MID_INITIALUPDATE, so a door caches the same origin the
            // engine gave it. LT_NOWMCENTER=1 = the old placement.
            static const bool s_bNoCenter = []() {
                const bool b = (getenv("LT_NOWMCENTER") != 0);
                if (b) { printf("[SRVOBJ] LT_NOWMCENTER=1 -- world models keep their authored Pos\n"); fflush(stdout); }
                return b; }();
            if (pMine && !s_bNoCenter
                && (pMine->m_Type == OT_WORLDMODEL || pMine->m_Type == OT_CONTAINER))
            {
                const std::vector<LTWorldModelHeader>& wm = world.GetModels();
                const char* pWM = cs.m_Filename[0] ? cs.m_Filename : pMine->m_sName.c_str();
                for (size_t k = 0; k < wm.size(); ++k)
                {
                    if (strcasecmp(wm[k].m_sName.c_str(), pWM) != 0
                        && strcasecmp(wm[k].m_sName.c_str(), pMine->m_sName.c_str()) != 0) continue;
                    for (int a = 0; a < 3; ++a)
                    {
                        pMine->m_Pos[a]        = wm[k].m_Translation[a];
                        pMine->m_BoxMin[a]     = wm[k].m_Min[a];
                        pMine->m_BoxMax[a]     = wm[k].m_Max[a];
                        pMine->m_BoxRestPos[a] = pMine->m_Pos[a];
                    }
                    pMine->m_bHasBox = true;
                    break;
                }
            }

            // ⚠️ INITIALUPDATE_WORLDFILE for the same reason as above.
            pObj->EngineMessageFn(MID_INITIALUPDATE, 0, INITIALUPDATE_WORLDFILE);
            LTServer_SendActivating(pObj);
            ++nMade;
        }
        else ++nSkipped;

        m_pCurrentProps = 0;
    }

    if (nDeclined)
        printf("[SRVOBJ] %d object(s) declined to be created (PRECREATE "
               "returned 0) -- that is the game's choice, not a failure\n",
               nDeclined);

    if (pnSkipped) *pnSkipped = nSkipped;
    return nMade;
}


// ----------------------------------------------------------------------- //
//
// UpdateObjects
//
// ⚠️ THE LIST IS INDEXED, NOT ITERATED, and the size is re-read every step.
// An object's MID_UPDATE can CREATE another object -- a trigger spawning
// something is the ordinary case -- which appends to m_Objects and would
// invalidate any iterator held across the call.
//
// ⚠️ Objects created DURING this frame are deliberately not updated in it.
// They get their MID_INITIALUPDATE at creation and their first MID_UPDATE on
// the next frame, which is what stops a spawn chain from running the whole
// level in one tick.
//
// ----------------------------------------------------------------------- //

// The object whose MID_UPDATE is running, for traces that want an owner the
// call itself does not name (LT_SOUNDTRACE). Null between updates.
static const void* g_pUpdatingRecord = 0;
const void* LTServerObjects_Updating() { return g_pUpdatingRecord; }

unsigned int CLTServerObjects::UpdateObjects(float fServerTime, float fFrameTime)
{
    unsigned int nUpdated = 0;
    const size_t nAtStart = m_Objects.size();
    const bool bAuto = AutoDeactivateOn();

    for (size_t i = 0; i < nAtStart && i < m_Objects.size(); ++i)
    {
        Record* p = m_Objects[i];
        if (!p || !p->m_pGame) continue;

        // Removed this frame; see Record::m_bDead.
        if (p->m_bDead) continue;

        // ⚠️ ASLEEP: NO UPDATE AT ALL (HANDOVER 5cp). lithtech.exe keeps its
        // sleeping objects at the back of the list and its update loop stops
        // at the first one. An AI that does not update does not sense.
        if (p->m_nInactive) continue;

        // ⚠️ THE COUNTDOWN RUNS FOR EVERY AWAKE OBJECT, due an update or not
        // (lithtech.exe 0x473c65). Running out only MARKS it: the player's
        // view gets a chance to keep it awake before ActivationPass decides.
        if (bAuto && p->m_fDeactTimer > 0.0f)
        {
            p->m_fDeactTimer -= fFrameTime;
            if (p->m_fDeactTimer <= 0.0f) p->m_bDeactPending = true;
        }

        // 0 means "do not update me". See the note on m_fNextUpdate.
        if (p->m_fNextUpdate <= 0.0f) continue;
        if (p->m_fNextUpdate > fServerTime) continue;

        // ⚠️ Cleared BEFORE the call, not after: the object almost always asks
        // for its next update from inside MID_UPDATE, and clearing afterwards
        // would throw that request away and stop it dead.
        p->m_fNextUpdate = 0.0f;

        // ⚠️ LT_UPDTRACE=<name-substring> -- which named objects are ticking.
        // "This object stopped updating" and "this object is updating and
        // doing nothing" are different bugs that look identical from the
        // outside, and the class census only counts per CLASS.
        {
            static const char* pWant = getenv("LT_UPDTRACE");
            const bool bMatch = pWant && *pWant &&
                ((!p->m_sName.empty() && p->m_sName.find(pWant) != std::string::npos) ||
                 (p->m_pClass && p->m_pClass->m_ClassName &&
                  strcmp(p->m_pClass->m_ClassName, pWant) == 0));
            if (bMatch)
            {
                static const char* pFrom = getenv("LT_UPDTRACE_FROM");
                static const float fFrom = pFrom ? (float)atof(pFrom) : 0.0f;
                static unsigned n = 0;
                if (fServerTime >= fFrom && n++ < 40)
                    printf("[UPD] %-16s %-22s t=%.3f\n",
                           (p->m_pClass && p->m_pClass->m_ClassName)
                               ? p->m_pClass->m_ClassName : "?",
                           p->m_sName.empty() ? "(unnamed)" : p->m_sName.c_str(),
                           fServerTime);
            }
        }

        g_pUpdatingRecord = p;
        p->m_pGame->EngineMessageFn(MID_UPDATE, 0, 0.0f);
        g_pUpdatingRecord = 0;
        ++nUpdated;
        if (p->m_pClass && p->m_pClass->m_ClassName)
            ++m_Updates[p->m_pClass->m_ClassName];
    }
    return nUpdated;
}


// ----------------------------------------------------------------------- //
//
// ACTIVATION -- LT2's auto-deactivation (HANDOVER 5cp)
//
// ⚠️ WHAT IT IS FOR. NOLF turns it on at startup ("autodeactivate 1.0",
// GameServerShell.cpp:118) and every AI then falls asleep the moment the
// level's auto-save is done (AI.cpp:456, OBJSTATE_AUTODEACTIVATE_NOW) or ten
// seconds after it was last seen. Asleep it does not update, so it does not
// hear: weapon fire is heard by distance alone (AISense.cpp), and with every
// AI awake a P38 shot at M01S01's gate reached the whole hotel.
//
// ⚠️ WHAT WAKES IT is the player's view: every frame, objects the view
// position can see in the level's PVS (ltworldvis) are activated, which
// restarts their countdown. A command sent to an AI wakes it as well
// (AI::ProcessCommand calls SetDeactivationTime).
//
// ----------------------------------------------------------------------- //

static bool ActTrace()
{
    static const bool b = (getenv("LT_ACTTRACE") != 0);
    return b;
}

void CLTServerObjects::SetActivity(Record* p, unsigned char nInactive, const char* pWhy)
{
    if (!p || !p->m_pGame || p->m_bDead) return;
    if (p->m_nInactive == nInactive) return;

    const bool bWasAwake = (p->m_nInactive == 0);
    p->m_nInactive = nInactive;
    if (nInactive == 0) p->m_bDeactPending = false;

    if (ActTrace())
        printf("[ACT] %-6s %-20s '%s' (0x%02x, %s)\n", nInactive ? "sleep" : "wake",
               (p->m_pClass && p->m_pClass->m_ClassName) ? p->m_pClass->m_ClassName : "?",
               p->m_sName.c_str(), nInactive, pWhy ? pWhy : "");

    if (bWasAwake && nInactive)
    {
        if (!getenv("LT_NOACTIVATE")) p->m_pGame->EngineMessageFn(MID_DEACTIVATING, 0, 0.0f);
    }
    else if (!bWasAwake && !nInactive)
        LTServer_SendActivating(p->m_pGame);
}

void CLTServerObjects::Activate(Record* p, float fFrameTime, const char* pWhy)
{
    if (!p) return;
    p->m_bDeactPending = false;
    p->m_fDeactTimer = (p->m_fDeactTime > 0.0f) ? fFrameTime + p->m_fDeactTime : p->m_fDeactTime;
    if (p->m_nInactive & kAutoDeactivated)
        SetActivity(p, (unsigned char)(p->m_nInactive & (kInactive | kInactiveTouch)), pWhy);
}

// What lithtech.exe 0x464ed0 lets the view wake: what it would SEND the
// client -- forced, visible, or solid/ray-hit, and a plain OT_NORMAL only if
// it carries a special effect.
static bool WakesBySight(const CLTServerObjects::Record* p)
{
    if (p->m_Flags & FLAG_FORCECLIENTUPDATE) return true;
    if (!(p->m_Flags & FLAG_VISIBLE) && !(p->m_Flags & (FLAG_RAYHIT | FLAG_SOLID))) return false;
    if (p->m_Type == OT_NORMAL && p->m_SFXMsg.empty()) return false;
    return true;
}

// ⚠️ A BRUSH IS TESTED BY ITS BOX, NOT ITS POSITION (HANDOVER 5cs). A door or
// window has no dims, and its position is its HINGE -- inside the wall or the
// frame, outside every leaf. Tested as a point, M04S01's club window
// (HingedDoor0) was unseen from right in front of it, fell asleep 1.1 s into
// opening and stayed half open. lithtech.exe tests an object's box; a brush
// gets its world-model box where it stands now, grown to a cube of the box's
// diagonal so a swing about any hinge inside it stays covered (too big only
// means "awake more often").
static void BoxOf(const CLTServerObjects::Record* p, float* mn, float* mx)
{
    if (p->m_bHasBox)
    {
        float c[3], h = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            const float lo = p->m_BoxMin[k] + (p->m_Pos[k] - p->m_BoxRestPos[k]);
            const float hi = p->m_BoxMax[k] + (p->m_Pos[k] - p->m_BoxRestPos[k]);
            c[k] = 0.5f * (lo + hi);
            h += (hi - lo) * (hi - lo);
        }
        h = sqrtf(h);
        for (int k = 0; k < 3; ++k) { mn[k] = c[k] - h; mx[k] = c[k] + h; }
        return;
    }
    for (int k = 0; k < 3; ++k)
    {
        const float d = (p->m_Dims[k] > 0.0f) ? p->m_Dims[k] : 0.0f;
        mn[k] = p->m_Pos[k] - d;
        mx[k] = p->m_Pos[k] + d;
    }
}

void CLTServerObjects::ActivationPass(const float* pView3, bool bAutoActivate, float fFrameTime)
{
    // LT_ACTTRACE=1 also lists the AIs (AI_* classes) every 300 frames:
    // asleep or awake, and where -- "who came running" is a position change.
    if (ActTrace() && pView3)
    {
        static unsigned int nFrame = 0;
        if (++nFrame % 300 == 0)
        {
            unsigned int nAwake = 0, nAsleep = 0;
            for (size_t i = 0; i < m_Objects.size(); ++i)
            {
                const Record* p = m_Objects[i];
                if (!p || p->m_bDead || !p->m_pClass || !p->m_pClass->m_ClassName) continue;
                if (strncmp(p->m_pClass->m_ClassName, "AI_", 3) != 0) continue;
                (p->m_nInactive ? nAsleep : nAwake) += 1;
                printf("[ACTPOS] f%u %-14s %-6s (%.0f %.0f %.0f)\n", nFrame, p->m_sName.c_str(),
                       p->m_nInactive ? "asleep" : "awake", p->m_Pos[0], p->m_Pos[1], p->m_Pos[2]);
            }
            printf("[ACTSUM] f%u view (%.0f %.0f %.0f) leaf %d sees %u leaves; AIs awake %u asleep %u\n",
                   nFrame, pView3[0], pView3[1], pView3[2], LTGetWorldVis()->LeafAt(pView3),
                   LTGetWorldVis()->VisibleLeafCount(), nAwake, nAsleep);
            fflush(stdout);
        }
    }

    if (!pView3 || !AutoDeactivateOn()) return;

    CLTWorldVis* pVis = LTGetWorldVis();
    if (bAutoActivate)
    {
        pVis->SetViewLeaf(pVis->LeafAt(pView3));
        for (size_t i = 0; i < m_Objects.size(); ++i)
        {
            Record* p = m_Objects[i];
            if (!p || !p->m_pGame || p->m_bDead) continue;
            // Nothing to restart and nothing to wake: skip the walk.
            if (p->m_fDeactTime <= 0.0f && !(p->m_nInactive & kAutoDeactivated)) continue;
            if (!WakesBySight(p)) continue;
            float mn[3], mx[3];
            BoxOf(p, mn, mx);
            if (pVis->BoxVisible(mn, mx)) Activate(p, fFrameTime, "seen");
        }
    }

    // lithtech.exe 0x4655a0: what ran out and was not seen falls asleep.
    for (size_t i = 0; i < m_Objects.size(); ++i)
    {
        Record* p = m_Objects[i];
        if (!p || !p->m_bDeactPending) continue;
        p->m_bDeactPending = false;
        if (p->m_bDead || !p->m_pGame) continue;
        SetActivity(p, (unsigned char)((p->m_nInactive & (kInactive | kInactiveTouch)) | kAutoDeactivated),
                    "unseen");
    }
}

void CLTServerObjects::PingFrom(const Record* pFrom, float fFrameTime)
{
    if (!pFrom || !AutoDeactivateOn()) return;
    CLTWorldVis* pVis = LTGetWorldVis();
    pVis->SetViewLeaf(pVis->LeafAt(pFrom->m_Pos));
    for (size_t i = 0; i < m_Objects.size(); ++i)
    {
        Record* p = m_Objects[i];
        if (!p || !p->m_pGame || p->m_bDead) continue;
        if (p->m_fDeactTime <= 0.0f && !(p->m_nInactive & kAutoDeactivated)) continue;
        float mn[3], mx[3];
        BoxOf(p, mn, mx);
        if (pVis->BoxVisible(mn, mx)) Activate(p, fFrameTime, "pinged");
    }
}


void LTServer_SendActivating(BaseClass* pObj)
{
    if (!pObj) return;
    static const bool bOff = (getenv("LT_NOACTIVATE") != 0);
    static const bool bSaid = (bOff ? (printf("[ACTIVATE] LT_NOACTIVATE=1 -- MID_ACTIVATING is "
                                              "NOT sent; no character is ever active\n"), true)
                                    : true);
    (void)bSaid;
    if (bOff) return;
    pObj->EngineMessageFn(MID_ACTIVATING, 0, 0.0f);
}


// ⚠️ THE MODEL THE GAME CHOSE, NOT THE ONE IN THE FILE. Intelligence::ReadProp
// takes its model from the intel type ("Ledger 1"), and its world-file
// Filename is empty (PF_HIDDEN editor bookkeeping); a WeaponItem's file names
// the editor's AmmoBox.abc while the game shows the weapon's own pickup
// model. The prop pass drew the file's (nothing / an ammo box) and the live
// pass skipped every world-file object but characters -- so T02S01's ledger,
// the photograph the training asks for, was never drawn (user report,
// HANDOVER 5bm). PropType is left to the prop pass: the game and the world
// renderer resolve it from the same attribute file.
// LT_FILEMODELS=1 = the old split (says so).
bool CLTServerObjects::DrawnLive(const Record* p) const
{
    static const bool s_bOff = []() {
        const bool b = (getenv("LT_FILEMODELS") != 0);
        if (b) { printf("[LIVE] LT_FILEMODELS=1 -- world-file objects drawn from the file's model only\n"); fflush(stdout); }
        return b; }();
    if (s_bOff || !p || !p->m_bFromWorld || !p->m_pClass) return false;
    const std::string& f = p->m_sFilename;
    if (f.size() < 4 || strcasecmp(f.c_str() + f.size() - 4, ".abc") != 0) return false;
    for (const ClassDef* c = p->m_pClass; c; c = c->m_ParentClass)
        if (c->m_ClassName && (strcasecmp(c->m_ClassName, "PropType") == 0
                               || strcasecmp(c->m_ClassName, "CCharacter") == 0))
            return false;
    return strcasecmp(f.c_str(), p->m_sWorldModel.c_str()) != 0;
}

void LTServerObjects_EulerToQuat(const float* eul, float* q)
{
    // Yaw (Y) then pitch (X) then roll (Z) -- the order the editor applies
    // them, and the same one CLTWorldScene renders with.
    const float cx = cosf(eul[0]*0.5f), sx = sinf(eul[0]*0.5f);
    const float cy = cosf(eul[1]*0.5f), sy = sinf(eul[1]*0.5f);
    const float cz = cosf(eul[2]*0.5f), sz = sinf(eul[2]*0.5f);
    q[0] = sx*cy*cz + cx*sy*sz;
    q[1] = cx*sy*cz - sx*cy*sz;
    q[2] = cx*cy*sz - sx*sy*cz;
    q[3] = cx*cy*cz + sx*sy*sz;
}

void CLTServerObjects::SetWorldMade(const std::vector<unsigned char>& made)
{
    m_WorldMade = made;
    m_ByWorldIndex.assign(made.size(), (Record*)0);
}

// What CreateWorldObjects records about world-file object i, for an object
// that came from a save instead. Only into a slot SetWorldMade opened.
void CLTServerObjects::RelinkWorld(Record* p, int i, const CLTWorld& world)
{
    if (!p || i < 0 || (size_t)i >= m_ByWorldIndex.size()) return;
    const std::vector<LTWorldObject>& obj = world.GetObjects();
    if ((size_t)i >= obj.size()) return;
    p->m_bFromWorld = true;
    p->m_nWorldIndex = i;
    const LTWorldProperty* pWF = obj[i].Find("Filename");
    p->m_sWorldModel = (pWF && pWF->m_nType == 0) ? pWF->m_sString : std::string();
    m_ByWorldIndex[i] = p;
    m_WorldMade[i] = 1;
}

// -1 unknown, 0 hidden/removed, 1 visible, 2 visible but drawn by the live
// pass (DrawnLive) -- the prop pass must not draw the file's model there.
bool CLTServerObjects::WorldPose(size_t i, float* pPos3, float* pQuat4) const
{
    if (i >= m_WorldMade.size() || !m_WorldMade[i]) return false;
    const Record* p = m_ByWorldIndex[i];
    if (!p || p->m_bDead) return false;
    for (int k = 0; k < 3; ++k) pPos3[k] = p->m_Pos[k];
    for (int k = 0; k < 4; ++k) pQuat4[k] = p->m_Rot[k];
    return true;
}

bool CLTServerObjects::WorldAnim(size_t i, int* pnAnim, float* pfStart, bool* pbLoop) const
{
    if (i >= m_WorldMade.size() || !m_WorldMade[i]) return false;
    const Record* p = m_ByWorldIndex[i];
    if (!p || p->m_bDead) return false;
    *pnAnim = p->m_nAnim; *pfStart = p->m_fAnimStart; *pbLoop = p->m_bAnimLooping;
    return true;
}

int CLTServerObjects::WorldState(size_t i) const
{
    if (i >= m_WorldMade.size() || !m_WorldMade[i]) return -1;
    const Record* p = m_ByWorldIndex[i];
    if (!p || p->m_bDead) return 0;
    if (!(p->m_Flags & FLAG_VISIBLE)) return 0;
    return DrawnLive(p) ? 2 : 1;
}
